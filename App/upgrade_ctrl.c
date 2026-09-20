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
#include "ram_flash.h"   /* ram_flash_program()：RAM 常驻的 Flash 擦写例程 */
#include "serial_dl.h"   /* 串口下载的字节层（USART1 + 请求行 + BAKPR 标志） */

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
    if(FMC_ErasePage(addr + offset) != FMC_STATUS_COMPLETE)
    {
      FMC_Lock();
      return -1;                 /* 如实报错，不再吞掉 */
    }
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
    if(hw_flash_write(addr + offset, data + offset, chunk) != 0)
    {
      return -1;                 /* 如实报错，不再吞掉 */
    }
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

/* 放弃本次升级。运行区未被触碰，写标志后复位交回引导区。
 *
 * flag 写 0xFF 而不是 0x5A：
 *   · 三层架构下引导区只认 0x3A(APP 升级) / 0x3B(boot 自升级) 两个请求码，
 *     abort 走正常启动分支跳 APP —— 0xFF 与 0x5A 对引导区等价。
 *   · 差别在 APP 侧：hw_board_init() 会把 flag==0xFF 规范化为 0x5A。
 *     abort 时运行区可能是一份从未记录过 fw[0] 的固件（产线 J-Link 直烧），
 *     让 APP 走一遍规范化更稳。
 *
 * 【旧注释已废弃，勿再按它推断】原文写的是"flag=0xFF jumps to it"、
 * "fw[1] invalid -> cannot rollback" —— 那是**旧架构**（升级 boot 自己
 * 跳 APP）的说法。新架构下 iap.c 里已经没有跳 APP 的实现，本函数只写标志
 * 不复位、也不跳转，决策全在引导区。 */

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
    /* ---- 半写保护（2026-09-18）----
     * 擦运行区【之前】必须让引导区有能力判它无效。
     *
     * fw[0].crc == 0xFFFFFFFF 在引导区 boot_app_valid() 眼里是
     * "无记录 = 放行"（产线 J-Link 裸烧语义）。走到本块说明 fw[0] 没有
     * 记录，而下面无论如何都会擦 A 槽 —— 一旦中途掉电，引导区会放行
     * 半写的 A（第 0 页就是新镜像的向量表，必然通过它的向量表检查）->
     * 执行到 0xFF -> HardFault -> NRST -> 复位环；且此时 app != 0，
     * 引导区永远跳 APP，升级 boot 再也进不去 —— 串口救援都够不着。
     *
     * 写一条不可能匹配的占位记录：crc=0 与真实 CRC32 撞上的概率 2^-32。
     * 装成功时会被真实值覆盖；装失败时正好让 A 被判无效、走回退路径。
     * 两侧判据同构（iap.c 的 slot_usable 与引导区 boot_app_valid 都是
     * "crc != 0xFFFFFFFF 就做全量 CRC 比对"），不会产生判据漂移。
     *
     * 【必须放在本块最前面，不能只放在下面"向量表无效"那条分支里】：
     * (a) 分支（B 有记录 -> 保留 B 不动）同样是"fw[0] 无记录 + 接下来要擦
     * A"，漏了它就等于保护网破了一个口。两条子分支现在同构。
     *
     * 用 iap_state_save()（只落盘不复位）—— 复位了就轮不到下面擦写了。
     * adopt 分支会立刻用真实 CRC 覆盖它，多几十毫秒的代价换覆盖完整。 */
    upgrade.fw[0].size = ctx->expect_size;
    upgrade.fw[0].crc  = 0u;
    iap_state_save();

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

/*
 * boot 自升级：读 UFS 的 BOOT_UPDATE_FILE_NAME -> 写 A 槽。
 *
 * 为什么在升级 boot 里做（而不是 APP）：这里已经有整套 QFREAD 机制；
 * 而 APP 侧没有 raw 接收能力。
 *
 * 关键顺序（每一步都不能颠倒）：
 *   ① 先把整个镜像读进 RAM 并自检 —— 之后 A 槽会被擦掉，不能再从 Flash 取东西
 *   ② 校验镜像的向量表确实指向 0x08003000（挡住链接地址不对的固件）
 *   ③ 把当前 A 存档到 B（纯 Flash 拷贝，写 B 不重叠）
 *   ④ 交给 RAM 例程擦写 A —— 它永不返回
 *
 * 掉电安全：
 *   ②之前掉电 -> A 完好，下次重来
 *   ③之后掉电 -> A 可能半写 -> 引导区判 A 无效 -> 若 APP 有效则跳 APP，
 *                由 APP 从 B 把旧 boot 拷回（见各 APP 侧的 boot 修复逻辑）
 */
static int32_t boot_image_ok(const uint8_t *img, uint32_t size)
{
  uint32_t sp = *(const uint32_t *)img;
  uint32_t pc = *(const uint32_t *)(img + 4u);

  if(size < 8u || size > BOOT_SLOT_MAX_IMAGE) return 0;
  if(sp < 0x20000000u || sp > 0x2001F800u) return 0;
  if(pc < BOOT_A_ADDR || pc > (BOOT_A_ADDR + BOOT_SLOT_MAX_IMAGE)) return 0;
  if((pc & 1u) == 0u) return 0;
  return 1;
}

static void upgrade_self_run(void)
{
  AT_FILE_CTX_T fc;
  uint32_t off = 0u;
  uint32_t img_size = 0u;
  uint32_t crc = 0u;

  printf("[boot-self] reading %s from module\r\n", BOOT_UPDATE_FILE_NAME);

  memset(&fc, 0, sizeof(fc));
  fc.fd = -1;

  /* ---- ① 从 UFS 读整个镜像进 RAM（复用 40KB 的 up_file_buf）---- */
  if(at_file_open(&fc, BOOT_UPDATE_FILE_NAME) != AT_FILE_ERR_OK) {
    printf("[boot-self] open %s failed\r\n", BOOT_UPDATE_FILE_NAME);
    upgrade_abort();
  }
  if(at_file_get_size(&fc) != AT_FILE_ERR_OK) {
    printf("[boot-self] QFLST failed\r\n");
    (void)at_file_close(&fc);
    upgrade_abort();
  }
  img_size = fc.file_size;

  if(img_size == 0u || img_size > BOOT_SLOT_MAX_IMAGE) {
    printf("[boot-self] bad image size %lu (max %lu)\r\n",
           (unsigned long)img_size, (unsigned long)BOOT_SLOT_MAX_IMAGE);
    (void)at_file_close(&fc);
    upgrade_abort();
  }

  while(off < img_size) {
    uint32_t chunk = img_size - off;
    uint32_t actual = 0u;

    if(chunk > LTE_READ_CHUNK_SIZE) chunk = LTE_READ_CHUNK_SIZE;

    /* 上界必须是【剩余空间】而不是整块缓冲的容量：at_file_read() 内部按
     * 这个 buf_size 限 ISR 的写入范围（at_arm_raw_rx），而数据是写在
     * up_file_buf + off 处的。传全容量等于告诉接收方有 40960 字节可用，
     * 实际只有 40960 - off。模组若超发就越界 —— QFREAD 失败后模组仍会
     * 继续吐数据（upgrade_recover_wait_with_wdt 就是为这个而设）。 */
    if(at_file_read(&fc, up_file_buf + off, LTE_READ_CHUNK_SIZE - off,
                    chunk, &actual, UPGRADE_QFREAD_TIMEOUT_MS) != AT_FILE_ERR_OK) {
      printf("[boot-self] QFREAD fail at offset %lu\r\n", (unsigned long)off);
      (void)at_file_close(&fc);
      upgrade_abort();
    }
    off += actual;
  }
  (void)at_file_close(&fc);
  printf("[boot-self] image read ok, %lu bytes\r\n", (unsigned long)img_size);

  /* ---- ② 镜像自检：向量表必须指向 A 槽（挡住链接地址不对的固件）---- */
  if(!boot_image_ok(up_file_buf, img_size)) {
    printf("[boot-self] image vector check FAILED (wrong link address?)\r\n");
    WDT_FEED();
    (void)at_file_delete(BOOT_UPDATE_FILE_NAME);   /* 坏件删掉，断掉重装环 */
    upgrade_abort();
  }

  crc = CRC32(up_file_buf, img_size);
  printf("[boot-self] image crc=0x%08lX\r\n", (unsigned long)crc);

  /* 与云端下发时写进 Upgrade_t 的参数比对 —— 装 APP 那条路径同款判据
   * （upgrade_ctrl.c 的 expect_crc）。这一步必须在动 A 槽之前：不比对的话，
   * UFS 里的坏文件会先把 A 擦掉再写进去，虽然能靠"引导区判槽无效 ->
   * APP 从 B 拷回来"自愈，但白折腾一轮，且这期间设备没有可用的 boot。 */
  if(img_size != upgrade.size || crc != upgrade.crc) {
    printf("[boot-self] param mismatch: got %lu/0x%08lX, expect %lu/0x%08lX\r\n",
           (unsigned long)img_size, (unsigned long)crc,
           (unsigned long)upgrade.size, (unsigned long)upgrade.crc);
    WDT_FEED();
    (void)at_file_delete(BOOT_UPDATE_FILE_NAME);   /* 坏件删掉，断掉重装环 */
    upgrade_abort();                               /* 写 FAILED 后复位 */
  }

  /* ---- ③ 当前 A 存档到 B（回退源）---- */
  printf("[boot-self] archiving A -> B\r\n");
  WDT_FEED();
  if(hw_flash_copy(BOOT_A_ADDR, BOOT_B_ADDR, BOOT_SLOT_SIZE) != 0) {
    printf("[boot-self] archive A->B failed\r\n");
    upgrade_abort();
  }

  /* ---- ④ 清升级请求 + 记结果：必须在 RAM 例程复位之前写 ----
   * 不清 0x3B 的话，新 boot 下次启动又读到它 -> 再读 UFS 再装一遍 ->
   * 无限重装。status=OK 顺带给 APP 依据，上报 upgrade_result state=3。
   * 这里只能走 iap_state_save()（只落盘不复位）——
   * iap_upgrade_state_save() 内部会 NVIC_SystemReset()，
   * 用它就走不到下面的 RAM 例程了。 */
  upgrade.flag     = 0x5A;
  upgrade.status   = UPGRADE_STATUS_OK;
  upgrade.up_count = 0U;
  iap_state_save();

  /* ---- ⑤ 置试用计数：新 boot 起不来时引导区能退到"跳 APP 去修" ----
   * 必须写在这里 —— RAM 例程只碰 Flash/GPIO，不碰 BAKPR。
   * 引导区每次启动 +1，超过 BOOT_TRIAL_LIMIT 就放弃 A 槽、返回 0，
   * 上层据此跳 APP，由 APP 把 B 槽的旧 boot 拷回 A 完成回退。
   * 新 boot 正常跑起来后，由 APP 首次进初始化链时清 0。 */
  /* 必须先使能后备域写访问：ST 系的 BKP 数据寄存器在备份域复位后是写保护的，
   * BAKPR_ConfigBackupRegister() 自己不开。不开这句，下面那次写就是空操作，
   * 引导区的试用计数永远是 0 —— "新 boot 起不来就回退"的安全网整个失效
   * （2026-09-18 硬件实测：崩溃 boot 能无限跳、无限复位）。
   * APP 侧之所以没事，是因为 hw_rtc_init / hw_board_init 已经开过了。 */
  /* PMU 与 BAKR 两个时钟都要开：只开 PMU 不够，没有 BKP 外设时钟，
   * 数据寄存器的写同样不落地。与 APP 的 hw_rtc_init 保持一致。 */
  RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_PMU | RCM_APB1_PERIPH_BAKR);
  PMU_EnableBackupAccess();

  BAKPR_ConfigBackupRegister(BOOT_TRIAL_REG, 1u);

  /* ---- ⑥ 交给 RAM 例程擦写 A；永不返回 ---- */
  printf("[boot-self] programming slot A from RAM\r\n");
  ram_flash_program(BOOT_A_ADDR, BOOT_SLOT_SIZE, up_file_buf, img_size, crc,
                    &Hw_WDT_Cfg.port->ODATA, Hw_WDT_Cfg.pin);

  /* 不会到这里 */
  for(;;) { }
}

/* ==================================================================
 * 串口下载（0x3C）—— 从 USART1（调试口）收镜像写进 APP A 槽
 *
 * 协议（PC 侧 = tools/serial_dl.py）：
 *
 *   PC   "#DL <size> <crc32>\r\n"（每 100ms 重发，上电前就开）
 *   DEV  校验 -> 排空 RX -> "#RDY\r\n"
 *   DEV  preserve（A 备份到 B，214KB 满槽约 7s）
 *   DEV  排空 RX -> "#GO\r\n"
 *   PC   第 0 页（≤2048B）
 *   DEV  擦 1 页 + 写 -> ACK(0x06)
 *   ...  每页等一个 ACK ...
 *   PC   末页（可不足 2048B）
 *   DEV  擦 1 页 + 写 -> 回读全量 CRC32
 *   DEV  ACK(0x06) = 提交 + 复位 / NAK(0x15) = 失败
 *
 * 【控制字词表】PC 侧接收窗口里混着设备日志（工具先开、板子后上电），
 * 所以设备的控制信号必须和日志不可能撞：
 *   · 请求行 / #RDY / #GO 一律带 "#" 前缀
 *   · ACK / NAK 用 0x06 / 0x15 控制字节，不用 'A'/'C'
 *   · 任何终止路径上【裁决字节先于日志】（2026-09-18 两次实测各自踩过一条）
 *
 * 【为什么按页 ACK 是整个协议的灵魂】
 * ACK 天然制造"PC 一定不在发数据"的窗口，擦除/写入全塞进这个窗口 ->
 * 1 字节接收寄存器 + 无硬件流控的问题自动消失。反过来：擦一页要
 * 20~40ms，没有 ACK 的话这段时间正好够 PC 把接收寄存器冲爆。
 *
 * 【为什么可以阻塞轮询、不开中断】
 * 节奏由 ACK 控制，本来就没有并发。轮询循环约 10 cycle/字节，而字节
 * 间隔 6250 cycle（87us @72MHz）-> 占空比 0.16%。
 * 不引入 ISR 还避开了一个必须靠手册背书的论证：本工程 USART1 的 RXNE
 * 中断从不使能（hw_uart.c 的 USART1_Isr 是死代码，只有 USART3 开了中断），
 * 所以收页路径上没有任何东西会和轮询抢字节。开中断去收反而会引入这个风险。
 *
 * 【复用】页缓冲直接用 up_file_buf 的前 2KB（QFDEL/QFREAD 那条路与
 * 串口互斥，不会同时在跑）-> 新增 RAM = 0。改小 LTE_READ_CHUNK_SIZE
 * 到 2048 以下会静默踩到这里。
 *
 * 【铁律】收页循环内禁止 printf —— 混进一个字节整包就废。
 * 本函数的 printf 全部在循环之外，或循环内的【终止分支】上。
 * ================================================================== */
static void upgrade_serial_run(const UPGRADE_CTX_T *ctx)
{
  UPGRADE_CTX_T c = *ctx;
  uint32_t size = ctx->expect_size;
  uint32_t crc  = ctx->expect_crc;
  uint32_t off  = 0u;
  uint32_t calc = 0u;
  uint8_t *page = up_file_buf;
  int32_t  rc;

  /* ---- ① 拿到参数 ----
   * 砖机入口（main.c 的 UFS 兜底窗口）已经在 ctx 里填好了；引导区
   * 魔数入口（iap_init 只认 BAKPR 标志，不知道尺寸）要在这里等。 */
  if(size == 0u)
  {
    printf("[serial] waiting for download request...\r\n");
    if(!serial_dl_wait_request(SERIAL_DL_REQ_TIMEOUT_MS, &size, &crc))
    {
      /* 没等到 = 什么都没发生：运行区没被碰过。用 0x5A/NONE 交回引导区
       * 正常启动，而不是 upgrade_abort() 的 FAILED —— 云端根本没下发过，
       * 报一次"升级失败"是假消息。
       *
       * 【例外】flag 已经是 0x3A / 0x3B 时原样留着并复位：那是一次真实的
       * 挂起请求（串口标志陈旧、或操作员改了主意），把它改成 0x5A 等于
       * 直接把云端的 OTA 吞掉 —— 云端收不到任何 upgrade_result，现场只
       * 看到设备重启一次、版本没变。
       *
       * 这里用裸 NVIC_SystemReset() 不违反 iap.c 的不变量 1：导致我们走到
       * 这里的那个持久状态（BAKPR 串口标志）已经由 iap_init 读后清零，
       * 下一次启动的决策必然不同，不会成环。若 flag 是 0x3A 但参数不可信
       * （入口 B 命令填的就是 size=0），iap_init 会走 drop_request 把它
       * 规范化掉，再复位一次就落到 APP —— 有界。 */
      if((upgrade.flag == 0x3AU) || (upgrade.flag == 0x3BU))
      {
        printf("[serial] no request in time, keep pending upgrade 0x%X\r\n",
               upgrade.flag);
        NVIC_SystemReset();
      }

      printf("[serial] no request in time, hand back\r\n");
      iap_upgrade_state_save(0x5A, UPGRADE_STATUS_NONE);
    }
  }
  c.expect_size = size;
  c.expect_crc  = crc;

  printf("[serial] request ok: size=%lu crc=0x%08lX\r\n",
         (unsigned long)size, (unsigned long)crc);

  /* 标志已由 iap_init 清零；走 main.c 窗口那条路进来时还没清，这里补
   * 一次（幂等）。不清的话刷完复位又会被送回串口模式。 */
  rc = serial_dl_take_request();
  if(rc < 0)
  {
    /* 清不掉 -> 刷完也会被再送回来 -> 无限重刷。宁可这次不刷。 */
    printf("[serial] request flag stuck -> park\r\n");
    upgrade_park();
  }

  /* ---- ② 参数落盘（必须在动运行区之前）----
   * 下载中途掉电时，引导区要靠 flag 判断该走哪条路：0x5A = 无请求 ->
   * 它判 A 无效后由 B 槽回退接管。写 0x3C 则是给引导区留一个它不认的
   * 未知值（0x3C 只是本工程的内部动作码，不是升级请求码）。 */
  upgrade.flag     = 0x5A;
  upgrade.up_count = 0U;
  upgrade.size     = size;
  upgrade.crc      = crc;
  iap_state_save();   /* 只落盘不复位；写不进去内部 park，不会走到 RDY */

  /* "#" 前缀与请求行同族：这两行会被上位机当控制信号解析，而它的接收
   * 窗口里混着设备日志（工具先开、板子后上电，复位前 APP 还在打日志）。
   * 裸 "RDY" 会被 "[.. 4G] RDY confirmed" 这类日志行误命中 —— 2026-09-18
   * 实测就是这么失败的：工具提前收工、停发魔数，复位时引导区啥也没收到。 */
  serial_dl_send("#RDY\r\n");

  /* ---- ③ preserve：当前 A -> B（回退源）----
   * 整条路径最慢的一步（214KB 满槽擦+拷约 7s）。PC 收到 RDY 后已停发，
   * 所以这里不需要边收边等；但串口上可能还有在途字节，第 ④ 步要排空。 */
  printf("[serial] preserve run -> backup\r\n");
  if(upgrade_preserve_backup(&c) != 0)
  {
    printf("[serial] preserve failed, abort\r\n");
    serial_dl_send("#ERR preserve\r\n");
    upgrade_abort();
  }

  /* ---- ④ 排空在途字节再放行 ----
   * PC 在收到 RDY 之前的重发残字节若不清掉，会被当成第 0 页数据写进
   * Flash 开头 —— 而且 CRC 会"自洽地"错（我们校验的是写进去的东西）。 */
  serial_dl_flush();

  /* 【顺序不可调】日志必须在 GO 之前 —— GO 是给 PC 的放行信号，它一收到
   * 就立刻开始灌数据。收页循环开始前任何耗时输出（这句 printf 约 2.6ms）
   * 都会让 USART 那 1 字节接收寄存器溢出，丢掉头几十字节。 */
  printf("[serial] receiving %lu bytes\r\n", (unsigned long)size);
  serial_dl_send("#GO\r\n");

  /* ---- ⑤ 收页循环 ---- */
  while(off < size)
  {
    uint32_t chunk = size - off;
    int32_t  fres;

    if(chunk > FLASH_PAGE_SIZE) chunk = FLASH_PAGE_SIZE;

    if(serial_dl_recv(page, chunk, SERIAL_DL_RX_TIMEOUT_MS) < 0)
    {
      serial_dl_send_byte(SERIAL_DL_NAK);   /* 先回 NAK 再打印，别抢线 */
      printf("[serial] rx timeout at off=%lu\r\n", (unsigned long)off);
      upgrade_abort();   /* 运行区半写：引导区判 A 无效 -> B 槽回退接管 */
    }

    /* 【这里刻意不包 __disable_irq()】
     * 原计划是"擦+写包在临界区里"，但**那个前提是错的**：SDK 的
     * FMC_ErasePage / FMC_ProgramWord（apm32e10x_fmc.c:254/283/307/321）
     * 自己无条件 __set_PRIMASK(1) ... __set_PRIMASK(0)，外层 disable 在
     * 第一次 FMC 调用返回时就被撤销了 —— 包了也只是个"看起来关了中断"
     * 的假象，比不包更危险。
     * Flash 的原子性由 SDK 那两句自己保证（每次擦/写内部真关中断）；
     * 此刻使能的中断只有 SysTick（加计数）和 TMR5（翻 GPIO），都不碰 FMC。
     * 与 UFS 那条路（flash_write_with_wdt 直接调用）保持一致。
     *
     * 仍然成立的两条铁律：不许 Delay_ms()（它是 SysTick 忙等，真关中断时
     * 会死等）、不许调用自带 __enable_irq() 的函数改变中断状态。 */
    fres = flash_erase_with_wdt(c.run_addr + off, FLASH_PAGE_SIZE);
    if(fres == 0)
    {
      fres = flash_write_with_wdt(c.run_addr + off, page, chunk);
    }

    if(fres != 0)
    {
      /* Flash 硬故障：复位多少次结果都一样，park 等现场。 */
      serial_dl_send_byte(SERIAL_DL_NAK);
      printf("[serial] flash fail at off=%lu -> park\r\n", (unsigned long)off);
      upgrade_park();
    }

    off += chunk;

    /* 末页不回"页 ACK" —— 它的应答就是最终裁决（ACK=已提交 / NAK=失败），
     * 由下面的 ⑥ 负责。页 ACK 的唯一作用是制造"PC 不在发数据"的静默窗口
     * 好让擦写安全进行，末页之后没有下一页要收，不需要这个窗口。
     * 多发一个会让上位机把"页 ACK"当成裁决（CRC 失败也报成功）。 */
    if(off < size)
    {
      serial_dl_send_byte(SERIAL_DL_ACK);
    }
  }

  /* ================== 裁决字节先于日志 ==================
   * 末页之后上位机等的【下一个字节】就是最终裁决（见文件头协议图）。
   * 任何插在它前面的日志都会被当成裁决 —— 2026-09-18 实测：工具读到了
   * "[serial] verifying" 的 '[' (0x5B) 当 ACK，61 页全传完却报"协议错位"。
   *
   * 所以本函数所有终止路径一律【先发 NAK/ACK，再打印】。下面这两段
   * 诊断日志刻意排在裁决之后（或干脆并在裁决行里一次出）。 */
  WDT_FEED();
  calc = CRC32((uint8_t *)c.run_addr, size);

  /* PC 报的 crc 只是交叉校验；真正说了算的是从刚写进去的 Flash 读回来
   * 现算 —— 它同时覆盖"PC 发错"和"Flash 写坏"两种故障。 */
  if(calc != crc)
  {
    serial_dl_send_byte(SERIAL_DL_NAK);
    printf("[serial] crc mismatch: calc=0x%08lX expect=0x%08lX\r\n",
           (unsigned long)calc, (unsigned long)crc);
    upgrade_abort();
  }

  /* 向量表 sanity，与 UFS 路径同款判据：专治"PC 送了一份链接地址不对的
   * 镜像"。CRC 自洽（我们算的就是它自己），但复位向量指向别处，装进去
   * 根本跑不起来 -> 引导区判无效 -> 又跳回升级 boot -> 又进串口模式。 */
  if(!iap_vector_ok(c.run_addr, c.run_addr + size))
  {
    serial_dl_send_byte(SERIAL_DL_NAK);
    printf("[serial] vector check FAILED (wrong link address?)\r\n");
    upgrade_abort();
  }

  /* ---- ⑦ 提交 ---- */
  upgrade.fw[0].size = size;
  upgrade.fw[0].crc  = calc;
  upgrade.fw_flag    = 1U;
  upgrade.flag       = 0x5A;
  upgrade.status     = UPGRADE_STATUS_OK;
  upgrade.up_count   = 0U;

  /* url/name 也改掉：它们描述的是"待安装的那份固件"。若不改，串口刷完后
   * APP 会向云端报 upgrade_result state=3，而元数据里还留着云端那次
   * set_upgrade 的 url —— 云端会以为自己下发的那份装上了。名称是给人看的，
   * 顺手留下"这份是串口刷的"这个唯一线索。 */
  upgrade.url[0] = '\0';
  (void)strncpy(upgrade.name, "serial", sizeof(upgrade.name) - 1u);
  upgrade.name[sizeof(upgrade.name) - 1u] = '\0';

  /* 这里刻意【不用】iap_upgrade_state_save()：它落盘后立刻复位，没机会
   * 回 ACK —— 而 ACK 必须代表"已提交"而不是"正在提交"。拆开两步：
   * iap_state_save() 自带回读校验，写不进去就 park，绝不会对 PC 撒谎。 */
  iap_state_save();

  serial_dl_send_byte(SERIAL_DL_ACK);   /* 裁决先出，日志后出（见 ⑥ 的说明） */
  printf("[serial] committed: size=%lu crc=0x%08lX\r\n",
         (unsigned long)size, (unsigned long)calc);

  Delay_ms(200);        /* 让 ACK 出完再复位（复位会把 TX 脚打回输入态） */
  NVIC_SystemReset();
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

  /* ================= 串口下载（0x3C）=================
   * 参数来自 PC 的请求行、不是云端元数据，所以必须排在下面那个
   * expect_* 校验【之前】—— 那里 expect_size==0 一律判 invalid，
   * 而引导区魔数入口进来时确实还没有尺寸（要等 PC 报）。 */
  if(ctx->action == UPGRADE_ACTION_SERIAL)
  {
    upgrade_serial_run(ctx);   /* 永不返回 */
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

  /* ================= boot 自升级（0x3B）================= */
  if(ctx->action == UPGRADE_ACTION_SELF)
  {
    /* 模组必须先起来 —— 下面是唯一一次 lte_modem_init 调用，而且在 0x3A
     * 分支里，SELF 分支在它之前。漏了这一步 AT 通道根本不通，症状是
     * [boot-self] open UFS:boot_update.bin failed（文件其实存在）。 */
    if(lte_modem_init(&LTE_MODEM_CFG_EG800K) != LTE_MODEM_ERR_OK)
    {
      printf("[boot-self] modem init failed, abort\r\n");
      upgrade_abort();
    }
    upgrade_self_run();   /* 永不返回 */
  }

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
  else if(ctx->action == UPGRADE_ACTION_ROLLBACK)
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
  else
  {
    /* 未知 action。iap_init() 只产生 NETWORK / SELF / SERIAL / ROLLBACK 四种，
     * 而 SELF 与 SERIAL 在各自的分支里永不返回 —— 走到这里说明枚举被扩充过
     * 而这里没跟上。不猜该做什么，park 等现场。 */
    printf("unknown action 0x%X -> park\r\n", (int)ctx->action);
    upgrade_park();
  }
}
