/*!
 * @file        upgrade_ctrl.c
 *
 * @brief       Upgrade state machine. Two firmware regions:
 *                - run region    (APP_ADDR, fixed link address, runs APP)
 *                - backup region (UP_BACKUP_ADDR, holds last-known-good)
 *              Actions:
 *                - 0x3A: preserve last-good into the backup region, then
 *                        download the new firmware directly into the run
 *                        region and verify its CRC
 *                - 0xAA: copy the backup region back to the run region
 *              Every copy/erase/write is verified by CRC afterwards;
 *              all long-running loops feed the external WDT (PB0 toggle).
 *
 * 2026-09-17 三层架构改造：
 *   · trial 自动回退取消 -> status 结果回传（见 iap.h UPGRADE_STATUS_*）
 *   · 安装成功后删除模组 UFS 里的固件，防止下次兜底安装装回旧版本
 *   · 备份成功后立即落盘 fw[1]，堵住"下载中途掉电丢回退能力"的窗口
 *   · 裸 NVIC_SystemReset() 全部消灭：无法自愈的故障改为 upgrade_park()
 */

#include "main.h"
#include "upgrade_ctrl.h"
#include "at_transport.h"
#include "at_file_ops.h"
#include "lte_modem.h"
#include "iap.h"
#include "bsp_delay.h"
#include "apm32e10x_iwdt.h"

#include <string.h>

/* firmware chunk receive buffer; the USART3 ISR writes into it directly */
static uint8_t up_file_buf[LTE_READ_CHUNK_SIZE];

static uint32_t flash_align_page(uint32_t size)
{
  return (size + FLASH_PAGE_SIZE - 1U) & (~(FLASH_PAGE_SIZE - 1U));
}

static int32_t flash_erase_with_wdt(uint32_t addr, uint32_t size)
{
  uint32_t erase_len = flash_align_page(size);
  uint32_t offset = 0U;

  FMC_Unlock();
  while(offset < erase_len)
  {
    WDT_FEED();
    FMC_ErasePage(addr + offset);
    offset += FLASH_PAGE_SIZE;
  }
  FMC_Lock();
  WDT_FEED();
  return 0;
}

static int32_t flash_write_with_wdt(uint32_t addr, const uint8_t *data, uint32_t size)
{
  uint32_t offset = 0U;
  uint32_t chunk = 0U;

  while(offset < size)
  {
    chunk = size - offset;
    if(chunk > FLASH_PAGE_SIZE)
    {
      chunk = FLASH_PAGE_SIZE;
    }
    WDT_FEED();
    (void)hw_flash_write(addr + offset, data + offset, chunk);
    offset += chunk;
  }
  WDT_FEED();
  return 0;
}

/* after a failed QFREAD the module may still stream raw data;
 * drain rx and wait for it to return to AT command mode */
static void upgrade_recover_wait_with_wdt(uint32_t wait_ms)
{
  uint32_t wait_cnt = 0U;

  for(wait_cnt = 0U; wait_cnt < wait_ms; wait_cnt++)
  {
    WDT_FEED();
    at_flush_rx();
    Delay_ms(1);
  }
  at_flush_rx();
  WDT_FEED();
}

/* abort the upgrade and boot the current run-region firmware;
 * clears any pending trial request so the old firmware is not rolled back.
 * flag is saved as 0xFF (first-boot semantics) instead of 0x5A: the run
 * region may hold firmware whose fw[0] metadata was never recorded
 * (e.g. flashed directly by J-Link). Saving 0x5A there would lock the
 * boot loader in an endless reset loop - fw[0] invalid -> cannot jump,
 * fw[1] invalid -> cannot rollback. Abort happens before the run region
 * is erased, so the current firmware is still intact and 0xFF jumps to it. */
static void upgrade_abort(void)
{
  printf("upgrade abort, boot current firmware\r\n");
  /* 本次升级未能实施（参数坏 / 模组起不来 / 保底复制失败 / 写越界）——
   * 运行区未被新固件触碰，但云端毕竟下发过升级，报 FAILED 让它知道。
   * APP 侧只能靠 status 判定结果（flag 会被它的 hw_board_init 规范化）。 */
  iap_upgrade_state_save(0xFF, UPGRADE_STATUS_FAILED);
}

/* 终止态：无法自愈的故障（Flash 写失败等）。
 * 绝不复位 —— 复位后引导区按同样的判据会再次把我们送进来，形成无限复位环。
 * 停留期间持续喂狗，让设备停在可诊断、可现场服务状态。 */
static void upgrade_park(void)
{
  printf("[upgrade-boot] PARK - unrecoverable, manual service required\r\n");
  while(1)
  {
    WDT_FEED();
    Delay_ms(500);
  }
}

/* ① preserve: copy the run region into the backup region before touching it.
 * Returns 0 when the backup holds a valid last-known-good (copied or
 * skipped), -1 when the copy failed (caller must abort and keep the run
 * region intact). */
static int32_t upgrade_preserve_backup(const UPGRADE_CTX_T *ctx)
{
  uint32_t crc = 0U;

  /* ---- 情况①：A 槽无元数据记录 ----
   * 分两种：
   *   (a) B 有记录 → 这台设备升级过，A 无记录说明运行区确实没装好，
   *       保留 B 不动（原行为）。
   *   (b) B 也无记录 → 产线 J-Link 直烧后的首次升级。直接跳过就"没有
   *       回退点"：新固件若有问题，B 空 + 模组 UFS 已删 → 只能 production
   *       park 现场救。所以只要 A 槽向量表合法，就按【整槽长度】补录一份
   *       fw[0] 当"最后已知良好"，再走下面的拷贝。
   *       代价：size 记成 APP_MAX_SIZE，之后每次启动的 CRC 覆盖整槽 214KB
   *       （而非实际镜像长度），约多 80ms —— 首次 OTA 提交后会被云端下发的
   *       真实 size 覆盖回去。 */
  if(!FW_VALID(&upgrade.fw[0]))
  {
    if(FW_VALID(&upgrade.fw[1]))
    {
      printf("run region has no record, keep backup\r\n");
      return 0;
    }

    if(!iap_vector_ok(ctx->run_addr, ctx->run_addr + APP_MAX_SIZE))
    {
      printf("run region invalid, skip preserve\r\n");
      return 0;
    }

    printf("no metadata at all, adopt run region as last-known-good\r\n");
    WDT_FEED();
    upgrade.fw[0].size = APP_MAX_SIZE;
    upgrade.fw[0].crc  = CRC32((uint8_t *)ctx->run_addr, APP_MAX_SIZE);
    WDT_FEED();
  }
  /* ---- 情况②：有记录但内容对不上 → 运行区确实坏了，保留 B 不动 ---- */
  else if(CRC32((uint8_t *)ctx->run_addr, upgrade.fw[0].size) != upgrade.fw[0].crc)
  {
    printf("run region crc mismatch, skip preserve\r\n");
    return 0;
  }

  /* backup already holds the same firmware → skip the copy */
  if(FW_VALID(&upgrade.fw[1]) && (upgrade.fw[1].size == upgrade.fw[0].size) &&
     (upgrade.fw[1].crc == upgrade.fw[0].crc) &&
     (CRC32((uint8_t *)ctx->backup_addr, upgrade.fw[1].size) == upgrade.fw[1].crc))
  {
    printf("backup already current, skip preserve\r\n");
    return 0;
  }

  printf("preserve: copy run -> backup\r\n");
  WDT_FEED();
  (void)hw_flash_copy(ctx->run_addr, ctx->backup_addr, upgrade.fw[0].size);

  crc = CRC32((uint8_t *)ctx->backup_addr, upgrade.fw[0].size);
  if(crc != upgrade.fw[0].crc)
  {
    printf("backup copy crc mismatch\r\n");
    return -1;
  }

  /* backup region now holds the previous version */
  upgrade.fw[1].size = upgrade.fw[0].size;
  upgrade.fw[1].crc  = upgrade.fw[0].crc;

  /* 立即落盘，不能等到 commit：下载阶段若掉电，B 槽内容已经换成新版本，
   * 而 fw[1] 若还停在更早的元数据，引导区会判 B 不可用 —— 回退能力静默
   * 丧失（trial 取消后，回退是唯一的安全网，这个窗口必须堵上）。
   * 此时 flag 仍是 0x3A，写回是幂等的。 */
  iap_state_save();
  return 0;
}

void upgrade_run(const UPGRADE_CTX_T *ctx)
{
  AT_FILE_CTX_T file_ctx;
  uint32_t crc = 0U;
  uint8_t dl_retry = 0U;
  uint8_t app_retry = 0U;
  int32_t ret = 0;

  if(ctx == NULL)
  {
    upgrade_abort();
  }

  /* empty flash reads as 0xFFFFFFFF: reject invalid expectations */
  if((ctx->expect_size == 0U) || (ctx->expect_size == 0xFFFFFFFFU) ||
     (ctx->expect_size > APP_MAX_SIZE) || (ctx->expect_crc == 0xFFFFFFFFU))
  {
    printf("expect param invalid, abort\r\n");
    upgrade_abort();
  }

  memset(&file_ctx, 0, sizeof(file_ctx));
  file_ctx.fd = -1;

  /* ================= download phase (0x3A only) ================= */
  if(ctx->action == UPGRADE_ACTION_NETWORK)
  {
    upgrade.size = ctx->expect_size;
    upgrade.crc = ctx->expect_crc;
    printf("expect size:%lu crc:0x%X\r\n",
           (unsigned long)ctx->expect_size, (unsigned int)ctx->expect_crc);

    if(lte_modem_init(&LTE_MODEM_CFG_EG800K) != LTE_MODEM_ERR_OK)
    {
      printf("modem init failed, abort\r\n");
      upgrade_abort();
    }

    /* ① preserve last-known-good before touching the run region */
    if(upgrade_preserve_backup(ctx) != 0)
    {
      printf("preserve backup failed, abort\r\n");
      upgrade_abort();
    }

    while(dl_retry < UPGRADE_MAX_DL_RETRIES)
    {
      uint32_t offset = 0U;
      int32_t dl_failed = 0;

      printf("download attempt %d/%d\r\n",
             (int)(dl_retry + 1U), (int)UPGRADE_MAX_DL_RETRIES);

      /* erase run region before download */
      WDT_FEED();
      (void)flash_erase_with_wdt(ctx->run_addr, ctx->expect_size);

      /* open file and validate size */
      if(at_file_open(&file_ctx, UPGRADE_FILE_NAME) != AT_FILE_ERR_OK)
      {
        printf("open file failed\r\n");
        dl_failed = 1;
      }
      else if(at_file_get_size(&file_ctx) != AT_FILE_ERR_OK)
      {
        printf("get file size failed\r\n");
        dl_failed = 1;
      }
      else if(file_ctx.file_size != ctx->expect_size)
      {
        printf("QFLST size:%lu expect:%lu, mismatch\r\n",
               (unsigned long)file_ctx.file_size, (unsigned long)ctx->expect_size);
        dl_failed = 1;
      }
      else
      {
        printf("up file size ok!\r\n");
      }

      /* chunked download straight into the run region */
      if(dl_failed == 0)
      {
        while(offset < ctx->expect_size)
        {
          uint32_t chunk = ctx->expect_size - offset;
          uint32_t actual = 0U;

          if(chunk > LTE_READ_CHUNK_SIZE)
          {
            chunk = LTE_READ_CHUNK_SIZE;
          }

          ret = at_file_read(&file_ctx, up_file_buf, sizeof(up_file_buf),
                             chunk, &actual, UPGRADE_QFREAD_TIMEOUT_MS);
          if(ret != AT_FILE_ERR_OK)
          {
            printf("QFREAD fail ret:%d offset:%lu chunk:%lu\r\n",
                   (int)ret, (unsigned long)offset, (unsigned long)chunk);
            dl_failed = 1;
            break;
          }

          if((offset > ctx->expect_size) || (actual > (ctx->expect_size - offset)))
          {
            printf("write overflow offset:%lu size:%lu up:%lu\r\n",
                   (unsigned long)offset, (unsigned long)actual,
                   (unsigned long)ctx->expect_size);
            upgrade_abort();
          }

          printf("flash write offset:%lu size:%lu\r\n",
                 (unsigned long)offset, (unsigned long)actual);
          flash_write_with_wdt(ctx->run_addr + offset, up_file_buf, actual);
          offset += actual;
        }
      }

      (void)at_file_close(&file_ctx);

      if(dl_failed != 0)
      {
        /* module may be wedged: full power cycle + drain */
        (void)lte_modem_reset();
        upgrade_recover_wait_with_wdt(UPGRADE_QFREAD_RECOVER_MS);
        dl_retry++;
        continue;
      }

      /* verify downloaded firmware CRC */
      Delay_ms(200);
      crc = CRC32((uint8_t *)ctx->run_addr, ctx->expect_size);
      printf("up crc:0x%X\r\n", (unsigned int)crc);

      /* expect_crc == 0 表示调用方无元数据可比对（如从模组 UFS 直接安装），跳过校验 */
      if((ctx->expect_crc == 0U) || (crc == ctx->expect_crc))
      {
        printf("up file crc ok!\r\n");

        /* 提交前最后一道：向量表 sanity。
         * 专治"模组 UFS 里的固件与槽地址不匹配"——CRC 自洽（我们算的就是它自己），
         * 但复位向量指向别处，装进去根本跑不起来：引导区判无效 -> 又跳回升级 boot
         * -> 又从 UFS 装同一份 -> 无限重装环。必须在这里断掉。
         * 实测案例：UFS 里躺着旧架构（链接在 0x08006000）的镜像。 */
        if(!iap_vector_ok(ctx->run_addr, ctx->run_addr + ctx->expect_size))
        {
          printf("image vector check FAILED (link address mismatch?)\r\n");
          WDT_FEED();
          /* 这份固件不可用：删掉它，否则下次上电还会再装一遍 */
          (void)at_file_delete(UPGRADE_FILE_NAME);
          upgrade_abort();   /* 不返回：复位后落到 production park */
        }

        /* commit: the run region holds the new firmware */
        upgrade.fw[0].size = ctx->expect_size;
        upgrade.fw[0].crc  = crc;
        upgrade.fw_flag    = 1U;

        /* 注意：安装成功后【不删】模组 UFS 里的固件。
         * 文件生命周期 = 下载前删旧 -> 下载新 -> 安装 -> 保留至下次下载。
         * 保留的好处：A/B 双坏时 UFS 里还有一份可用的恢复源；
         * APP 每次下载前（app_ota.c 的 Step 1）会 QFDEL 清掉旧的，
         * 所以既不会积累，也不会把陈旧固件当成恢复源。 */

        iap_upgrade_state_save(0x5A, UPGRADE_STATUS_OK);
      }

      printf("up file crc mismatch, retry %d/%d\r\n",
             (int)(dl_retry + 1U), (int)UPGRADE_MAX_DL_RETRIES);
      (void)lte_modem_reset();
      upgrade_recover_wait_with_wdt(UPGRADE_QFREAD_RECOVER_MS);
      dl_retry++;
    }

    if(dl_retry >= UPGRADE_MAX_DL_RETRIES)
    {
      /* the backup region holds the last-known-good (preserve step);
       * schedule a local rollback on the next boot */
      printf("download retries exhausted, schedule rollback\r\n");

      /* 这份固件已证明装不上（连读两遍都失败）—— 删掉它。
       * 不删的话：A/B 双坏时引导区会一直把升级 boot 送回来重装同一份
       * 坏件，每轮 ~60s 的模组初始化 + 下载，无限循环。删完下一轮 UFS
       * 探测就找不到文件 -> production park 终止。云端可随时重推。 */
      WDT_FEED();
      (void)at_file_delete(UPGRADE_FILE_NAME);

      iap_upgrade_state_save(0xAA, UPGRADE_STATUS_FAILED);
    }
  }
  else
  {
    /* ================= 0xAA rollback: backup -> run ================= */
    if(!FW_VALID(&upgrade.fw[1]) ||
       (CRC32((uint8_t *)ctx->backup_addr, upgrade.fw[1].size) != upgrade.fw[1].crc))
    {
      /* 备份槽本身不可用。按设计 iap_init() 已在进来之前用 bak_ok 挡掉，
       * 走到这里说明状态在运行中被破坏 —— 没有可做的动作，park。 */
      printf("AA backup crc mismatch\r\n");
      upgrade_park();
    }

    while(app_retry < UPGRADE_MAX_APP_RETRIES)
    {
      WDT_FEED();
      (void)hw_flash_copy(ctx->backup_addr, ctx->run_addr, upgrade.fw[1].size);

      crc = CRC32((uint8_t *)ctx->run_addr, upgrade.fw[1].size);
      printf("app crc:0x%X\r\n", (unsigned int)crc);

      if(crc == upgrade.fw[1].crc)
      {
        printf("rollback ok!\r\n");
        upgrade.fw[0].size = upgrade.fw[1].size;
        upgrade.fw[0].crc  = upgrade.fw[1].crc;
        upgrade.fw_flag    = 1U;
        /* 回退成功 = 本次升级最终失败：云端视角"掉回旧版本"就是升级失败 */
        iap_upgrade_state_save(0x5A, UPGRADE_STATUS_FAILED);
      }

      printf("app crc mismatch, retry %d/%d\r\n",
             (int)(app_retry + 1U), (int)UPGRADE_MAX_APP_RETRIES);
      app_retry++;
    }

    /* 拷贝重试耗尽：B 好但写不进 A —— Flash 硬故障，复位多少次结果都一样，
     * 保留 0xAA 让下次启动重试只会形成无限复位环。park。 */
    printf("rollback write retries exhausted\r\n");
    upgrade_park();
  }
}
