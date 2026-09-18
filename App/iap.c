/*
 * iap.c —— 升级元数据与启动决策（升级 boot / Stage-2）
 *
 * ============================ 三条不变量 ============================
 * 1. 复位只允许发生在「持久状态已改变」之后 —— 不存在裸复位。
 *    每次 NVIC_SystemReset() 之前必须先写升级元数据；否则引导区按同样的
 *    判据仍会把我们跳回来，形成无限复位环。
 *
 * 2. 本文件的槽判据 slot_usable() 必须与引导区
 *    apm32_e103_bootstrap/App/boot.c 的 boot_app_valid() 逐条同构。
 *    两边一旦分歧，IAP_ACT_RESUME_APP 的复位就会变成无限复位环。
 *
 * 3. 只要没有可执行的安装动作且 APP 不可用，就 park（喂狗死循环），
 *    由引导区提示 / 产线介入 —— 绝不靠复位"再试一次"。
 * ===================================================================
 *
 * 2026-09-17 三层架构改造：
 *   · 跳 APP 的职责归引导区，删除 Jump_to_App()
 *   · iap_init() 改为"只返回要做什么"，安装参数由 ctx 带回
 *   · trial 自动回退取消，改为 status 结果回传（见 iap.h）
 */

#include "iap.h"
#include "main.h"
#include "bsp_delay.h"
#include "serial_dl.h"   /* serial_dl_take_request()：串口下载请求标志 */

Upgrade_t upgrade;
Upgrade_cfg_t upgrade_cfg;

extern uint32_t CRC32(uint8_t *data, uint32_t length);

/*
 * 落盘升级元数据（不复位、不跳转）。
 *
 * 擦写范围：0x0807E000 ~ 0x0807E7FF，恰好 1 页（2KB），只属于 Upgrade_t。
 * 日后若有人往这一页塞别的小数据，每次状态保存都会把它抹掉。
 */
static void iap_state_flush(void)
{
  __disable_irq();
  hw_flash_erase(upgrade_cfg.config_addr, FLASH_PAGE_SIZE);
  hw_flash_write(upgrade_cfg.config_addr, &upgrade, sizeof(upgrade));
  __enable_irq();

  /* 回读校验 —— 这里失败【绝不能再复位】。
   *
   * 元数据没真正写进去，持久状态就没改变，而调用方紧接着 NVIC_SystemReset()：
   * 引导区会读到旧的 flag（比如还是 0x3A）再把我们跳回来，每轮重来一次完整
   * 下载 -> 无限重装环。违反 iap.c 顶部不变量 1（"复位只允许发生在持久状态
   * 已改变之后"）。
   *
   * 用回读而不是查 FMC_ErasePage/FMC_ProgramWord 的返回码：回读是唯一能同时
   * 覆盖"擦除失败"和"写入失败"的判据，也不依赖 SDK 状态位是否正确。 */
  if(memcmp((const void *)upgrade_cfg.config_addr, &upgrade, sizeof(upgrade)) != 0)
  {
    printf("[iap] metadata verify FAILED -> park (no reset)\r\n");
    for(;;)
    {
      WDT_FEED();
      Delay_ms(500);
    }
  }
}

/*
 * 槽可用性判定 —— 与引导区 boot_app_valid() 逐条同构，改一处必须同时改两处。
 * 两边一旦分歧，IAP_ACT_RESUME_APP 的复位就会变成无限复位环。
 *
 *   ① 向量表合理性：MSP 落 RAM、复位向量落本槽内、Thumb 位为 1   [iap_vector_ok]
 *   ② 无记录（crc == 0xFFFFFFFF）-> 产线 J-Link 裸烧，放行      [slot_usable]
 *   ③ 记录损坏（size 为 0 / 0xFFFFFFFF / 越界）-> 判无效        [slot_usable]
 *      不属于"无记录"，放行等于跳过校验
 *   ④ 有记录 -> 按记录长度做全量 CRC32                          [slot_usable]
 */
int32_t iap_vector_ok(uint32_t addr, uint32_t end)
{
  uint32_t sp = *(volatile uint32_t *)addr;
  uint32_t pc = *(volatile uint32_t *)(addr + 4U);

  if((sp < APP_RAM_BASE) || (sp > APP_RAM_TOP)) return 0;
  if((pc < addr) || (pc > end)) return 0;
  if((pc & 1U) == 0U) return 0;
  return 1;
}

static int32_t slot_usable(uint32_t addr, const Firmware *fw)
{
  if(!iap_vector_ok(addr, addr + APP_MAX_SIZE)) return 0;

  /* 无记录 -> 产线 J-Link 直烧后的首次启动：放行，不校验。
   * 判据只能是 crc == 0xFFFFFFFF（Upgrade_t 被擦除后唯一的形态）。
   *
   * 【不能用 !FW_VALID(fw) 代替】：那个宏把 size 异常（0 / 0xFFFFFFFF /
   * 越界）也算作"无记录"，可 size 异常配着有效 crc 是【记录损坏】——
   * 放行等于跳过校验跳进一段没验过的代码，坏了就是 HardFault -> NRST ->
   * 同样放行 -> 无限复位环。这类记录必须判无效，交给回退 / UFS 重装。
   * 与引导区 boot_app_valid() 逐条同构，改这里必须同时改那边。 */
  if(fw->crc == 0xFFFFFFFFu) return 1;
  if(fw->size == 0u || fw->size > APP_MAX_SIZE) return 0;

  /* 214KB 逐位 CRC 约 100~150ms，期间必须喂外部看门狗 */
  WDT_FEED();
  return (CRC32((uint8_t *)addr, fw->size) == fw->crc) ? 1 : 0;
}

/*
 * 备份槽（B）完好性判定 —— 只验记录 + 全量 CRC，不做"能不能跳"的向量表检查。
 *
 * 为什么不能复用 slot_usable()：B 槽存的是 A 槽镜像的拷贝，
 * 其复位向量指向 A 的链接地址（0x0800F000），必然落不进 B 槽范围
 * [0x08044800, 0x0807A000) —— 套用可运行槽判据必然返回 0，
 * 回退路径会静默失效（实测日志：slot check: app=0 bak=0）。
 *
 * 与 A 槽的语义差异：A 槽"无记录 = 放行"（首次 J-Link 裸烧），
 * B 槽"无记录 = 不可用"（没有备份就是没有，不能拿空白去"恢复"）。
 */
static int32_t slot_intact(uint32_t addr, const Firmware *fw)
{
  if(!FW_VALID(fw)) return 0;

  WDT_FEED();
  return (CRC32((uint8_t *)addr, fw->size) == fw->crc) ? 1 : 0;
}

/*
 * 只落盘、不复位（iap_state_flush 的对外包装）。
 * 供 upgrade_preserve_backup() 在备份成功后立即固化 fw[1] ——
 * 下载阶段掉电时，B 槽内容已经换成新版，若 fw[1] 还停在更早的元数据，
 * 引导区会判 B 不可用，回退能力静默丧失。
 */
void iap_state_save(void)
{
  iap_state_flush();
}

/*
 * 丢弃挂起的升级请求：只落盘、不复位。
 * flag 写 0x5A（= 引导区眼中的"无请求"稳态），status 清 0（不产生上报）。
 * 与 iap_upgrade_state_save() 的唯一区别就是不复位。
 */
static void drop_request(void)
{
  upgrade.flag     = 0x5A;
  upgrade.status   = UPGRADE_STATUS_NONE;
  upgrade.up_count = 0U;
  iap_state_flush();
}

/* boot 自升级的参数可信性：上限是 boot 槽容量，不是 APP 的 214KB */
static int32_t expect_params_ok_boot(uint32_t size, uint32_t crc)
{
  if(size == 0u || size == 0xFFFFFFFFu) return 0;
  if(size > BOOT_SLOT_MAX_IMAGE)        return 0;
  if(crc == 0u || crc == 0xFFFFFFFFu)   return 0;
  return 1;
}

/* 安装参数可信性：空片 / 越界一律拒绝，绝不拿坏参数去擦运行区 */
static int32_t expect_params_ok(uint32_t size, uint32_t crc)
{
  if(size == 0U || size == 0xFFFFFFFFU) return 0;
  if(size > APP_MAX_SIZE)               return 0;
  if(crc == 0U || crc == 0xFFFFFFFFU)   return 0;
  return 1;
}

/*
 * 启动决策。返回值见 iap.h 的 IAP_ACT_*；安装参数通过 ctx 带回。
 *
 * 三层架构下升级 boot 被跳进来只有两种可能：APP 无效，或 flag==0x3A。
 * 因此本函数绝不返回"跳 APP"类动作 —— 那种码在旧架构下正确，
 * 在新架构下就是复位环的环源。
 */
int32_t iap_init(UPGRADE_CTX_T *ctx)
{
  Upgrade_cfg_t *upgrade_cfg_temp;
  uint8_t cfg_need_init = 0;
  int32_t app_ok, bak_ok;

  if(ctx == NULL) return IAP_ACT_PARK;
  memset(ctx, 0, sizeof(*ctx));

  upgrade_cfg_temp = (Upgrade_cfg_t *)UPGRADE_CFG_ADDR;

  /* cfg: use when crc matches AND addresses fit the current layout;
   * re-init on first boot / crc error / old-layout migration */
  if(upgrade_cfg_temp->data_crc != 0xFFFFFFFF)
  {
    uint32_t crc = CRC32((uint8_t *)UPGRADE_CFG_ADDR, sizeof(Upgrade_cfg_t) - sizeof(uint32_t));
    if(crc == upgrade_cfg_temp->data_crc)
    {
      memcpy(&upgrade_cfg, upgrade_cfg_temp, sizeof(Upgrade_cfg_t));
      if((upgrade_cfg.app_addr != APP_ADDR) || (upgrade_cfg.fw1_addr != UP_BACKUP_ADDR))
      {
        printf("upgrade_cfg layout mismatch, re-init\r\n");
        cfg_need_init = 1;
      }
    }
    else
    {
      printf("upgrade_cfg crc error!\r\n");
      cfg_need_init = 1;
    }
  }
  else
  {
    cfg_need_init = 1;
  }

  if(cfg_need_init)
  {
    memset(&upgrade_cfg, '\0', sizeof(Upgrade_cfg_t));

    upgrade_cfg.config_addr = UPGRADE_ADDR;
    upgrade_cfg.app_addr    = APP_ADDR;
    upgrade_cfg.fw1_addr    = UP_BACKUP_ADDR;
    upgrade_cfg.fw2_addr    = UP_BACKUP_ADDR;   /* unused, keep valid */
    upgrade_cfg.data_crc    = CRC32((uint8_t *)&upgrade_cfg, sizeof(Upgrade_cfg_t) - sizeof(uint32_t));

    /* disable interrupts while writing flash inside */
    __disable_irq();
    hw_flash_erase(UPGRADE_CFG_ADDR, FLASH_PAGE_SIZE);
    hw_flash_write(UPGRADE_CFG_ADDR, &upgrade_cfg, sizeof(Upgrade_cfg_t));
    __enable_irq();
  }

  /* 所有分支共用：运行槽 / 备份槽地址 */
  ctx->run_addr    = upgrade_cfg.app_addr;
  ctx->backup_addr = upgrade_cfg.fw1_addr;

  memcpy(&upgrade, (void *)upgrade_cfg.config_addr, sizeof(Upgrade_t));

  /* print metadata summary for debug log */
  printf("upgrade.flag=0x%X, fw_flag=%d, status=0x%X, up_count=%d\r\n",
         upgrade.flag, upgrade.fw_flag, upgrade.status, upgrade.up_count);
  printf("upgrade.fw[0].size=%lu fw[0].crc=0x%X\r\n",
         (unsigned long)upgrade.fw[0].size, (unsigned int)upgrade.fw[0].crc);
  printf("upgrade.fw[1].size=%lu fw[1].crc=0x%X\r\n",
         (unsigned long)upgrade.fw[1].size, (unsigned int)upgrade.fw[1].crc);

  /* 两槽判据 —— 与引导区同构，结果打进日志便于现场比对 */
  app_ok = slot_usable(APP_ADDR,       &upgrade.fw[0]);
  bak_ok = slot_intact(UP_BACKUP_ADDR, &upgrade.fw[1]);   /* 备份槽只验完整性 */
  printf("slot check: app=%d bak=%d\r\n", (int)app_ok, (int)bak_ok);

  /* ---- 串口下载请求（2026-09-18）—— 优先级【高于】下面的 flag 分派 ----
   * 操作员正拿着线、手上电时就发了 0x55 0xAA，引导区据此把我们跳进来。
   * 此时任何"自动回退 / 自动重装"都是在抢方向盘：砖机且 B 槽可用时，
   * 下面的 default 分支会判 bak_ok 直接回退，操作员就再也进不了串口模式。 */
  switch(serial_dl_take_request())
  {
  case 1:
    ctx->action = UPGRADE_ACTION_SERIAL;
    return IAP_ACT_INSTALL;   /* 尺寸由 PC 的请求行给，此处不填 expect_* */

  case -1:
    /* 标志清不掉 -> 刷完复位又会被送回来 -> 无限重刷。不赌，park 等现场。
     * （BAKPR 写不进去通常意味着后备域已失效，不是能靠复位自愈的故障。） */
    printf("!! serial request flag stuck -> PARK\r\n");
    return IAP_ACT_PARK;

  default:
    break;
  }

  switch(upgrade.flag)
  {
  /* ============ 安装请求（APP 主动发起） ============ */
  case 0x3A:
    if(expect_params_ok(upgrade.size, upgrade.crc))
    {
      ctx->action      = UPGRADE_ACTION_NETWORK;
      ctx->expect_size = upgrade.size;
      ctx->expect_crc  = upgrade.crc;
      return IAP_ACT_INSTALL;
    }
    /* 0x3A 但参数不可信（空片 0xFFFFFFFF / 越界）—— 绝不能复位：
     * 复位后引导区仍读到 0x3A，又跳回本函数，成环。 */
    printf("0x3A with bad params -> drop request\r\n");
    drop_request();
    return app_ok ? IAP_ACT_RESUME_APP : IAP_ACT_NONE;

  /* ============ boot 自升级请求（0x3B）============
   * 参数校验走同一套：APP 已把云端下发的 size/crc 写进顶层字段，
   * 这里只需确认可信（镜像实际长度上限由 upgrade_self_run 再检一次）。 */
  case 0x3B:
    if(expect_params_ok_boot(upgrade.size, upgrade.crc))
    {
      ctx->action      = UPGRADE_ACTION_SELF;
      /* expect_* 必须一起填：upgrade_run() 开头有个【共用】的参数检查
       * （expect_size==0 即 abort），它排在本 case 的 SELF 分支之前。
       * 只设 action 不设 expect_* -> 恒被判 invalid -> boot 自升级永远 abort。 */
      ctx->expect_size = upgrade.size;
      ctx->expect_crc  = upgrade.crc;
      return IAP_ACT_INSTALL;
    }
    printf("0x3B with bad params -> drop request\r\n");
    drop_request();
    return app_ok ? IAP_ACT_RESUME_APP : IAP_ACT_NONE;

  /* ============ 回退请求 ============ */
  case 0xAA:
    if(bak_ok)
    {
      ctx->action      = UPGRADE_ACTION_ROLLBACK;
      ctx->expect_size = upgrade.fw[1].size;
      ctx->expect_crc  = upgrade.fw[1].crc;
      return IAP_ACT_INSTALL;
    }
    printf("0xAA but backup unusable -> drop request\r\n");
    drop_request();
    return app_ok ? IAP_ACT_RESUME_APP : IAP_ACT_NONE;

  /* ============ 0x5A / 0xFF / 未知值 ============ */
  default:
    if(app_ok)
    {
      /* 本函数判 APP 有效、引导区却把我们送了进来 —— 只可能是判据漂移。
       * 复位必然成环（引导区仍会判无效），唯一安全动作是 park。
       * 这条分支是判据漂移的熔断器，勿删。 */
      printf("!! verdict mismatch (stage2 app_ok, flag=0x%X) -> PARK\r\n",
             upgrade.flag);
      return IAP_ACT_PARK;
    }

    if(bak_ok)
    {
      /* APP 坏、备份好，但 flag 不是 0xAA（引导区落盘失败 / 来自 0xFF）。
       * 不依赖引导区再决策，就地接管回退 —— 少一次复位、少一条环。 */
      printf("app bad, backup ok -> rollback now\r\n");
      ctx->action      = UPGRADE_ACTION_ROLLBACK;
      ctx->expect_size = upgrade.fw[1].size;
      ctx->expect_crc  = upgrade.fw[1].crc;
      return IAP_ACT_INSTALL;
    }

    /* A/B 都不可用 -> 交给 main 探测模组 UFS；没有文件就 park。
     * 绝不能返回"复位"类动作。 */
    printf("no usable firmware, fall through to UFS probe\r\n");
    return IAP_ACT_NONE;
  }
}

/*
 * 保存状态后复位。会写升级元数据再 NVIC_SystemReset —— 满足不变量 1。
 * status 见 iap.h 的 UPGRADE_STATUS_*（0x02=安装成功 / 0x03=安装失败）。
 */
void iap_upgrade_state_save(uint8_t flag, uint8_t status)
{
  upgrade.flag     = flag;
  upgrade.status   = status;
  upgrade.up_count = 0U;      /* trial 已废弃，恒 0 */

  printf("state save flag:0x%X status:0x%X fw0:0x%X/%lu fw1:0x%X/%lu\r\n",
         upgrade.flag, upgrade.status,
         (unsigned int)upgrade.fw[0].crc, (unsigned long)upgrade.fw[0].size,
         (unsigned int)upgrade.fw[1].crc, (unsigned long)upgrade.fw[1].size);

  iap_state_flush();

  /* settle time after write */
  Delay_ms(200);

  NVIC_SystemReset();
}
