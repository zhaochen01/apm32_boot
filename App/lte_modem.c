/*!
 * @file        lte_modem.c
 *
 * @brief       4G module lifecycle driver: power-cycle control, RDY
 *              detection with retry, AT channel bring-up (echo off).
 *              Model-independent - behaviour is driven by a config
 *              constant (see LTE_MODEM_CFG_EG800K / _EC800).
 */

#include "main.h"
#include "lte_modem.h"
#include "at_transport.h"
#include "bsp_delay.h"

#include <string.h>

/* module power-on control pin: PB1 -> 4G_PWR-EN (LDO enable),
 * high level = power on, low level = power off (measured polarity) */
static HW_GPIO_INIT_T s_modem_power_cfg = { RCM_APB2_PERIPH_GPIOB, GPIOB,
                                            GPIO_PIN_1, GPIO_SPEED_10MHz,
                                            GPIO_MODE_OUT_PP };

/* 4G module PCIE_RESET control pin: PC0, keep high = module running
 * (measured polarity, must stay high while powered; same as Collector-4G) */
static HW_GPIO_INIT_T s_modem_rst_cfg = { RCM_APB2_PERIPH_GPIOC, GPIOC,
                                          GPIO_PIN_0, GPIO_SPEED_10MHz,
                                          GPIO_MODE_OUT_PP };

static const LTE_MODEM_CFG_T *s_cfg = NULL;

const LTE_MODEM_CFG_T LTE_MODEM_CFG_EG800K =
{
  .power_cycle_hold_ms = 500U,
  .rdy_timeout_ms      = 5000U,
  .rdy_retry_count     = 2U,
  .rdy_pattern         = "RDY"
};

/* 分段延时并喂狗：长延时期间若不给外部 WDT 喂食会被 NRST 复位。
 * 2026-09-16 新增——原 Delay_ms(hold_ms)/Delay_ms(1500) 期间完全不喂，
 * 1500ms 已逼近典型 WDT 超时窗口。 */
/* AT 通道探测前的稳定等待：boot 刚接手时模组还在跑，AT 通道基本是活的，
 * 留一点余量避免误判成"模组没起"而多走一次 power cycle。 */
#define LTE_MODEM_AT_PROBE_DELAY_MS   500U

static void delay_feed(uint32_t ms)
{
  while(ms > 0U)
  {
    uint32_t step = (ms > 100U) ? 100U : ms;
    WDT_FEED();
    Delay_ms(step);
    ms -= step;
  }
}

static void modem_power_cycle(uint32_t hold_ms)
{
  hw_gpio_init(s_modem_power_cfg);

  /* low (power off) -> high (power on, stay high) */
  s_modem_power_cfg.port->BC = s_modem_power_cfg.pin;
  delay_feed(hold_ms);
  s_modem_power_cfg.port->BSC = s_modem_power_cfg.pin;
}

static int32_t modem_wait_rdy(const LTE_MODEM_CFG_T *cfg)
{
  at_flush_rx();
  if(at_wait(cfg->rdy_pattern, AT_MATCH_CONTAINS, cfg->rdy_timeout_ms) == AT_ERR_OK)
  {
    return LTE_MODEM_ERR_OK;
  }
  return LTE_MODEM_ERR_NO_RDY;
}

static int32_t modem_at_channel_check(void)
{
  /* basic AT sanity check + disable echo */
  if(at_send_wait("AT\r\n", "OK", AT_MATCH_EXACT, LTE_MODEM_AT_TIMEOUT_MS) != AT_ERR_OK)
  {
    return LTE_MODEM_ERR_TIMEOUT;
  }
  if(at_send_wait("ATE0\r\n", "OK", AT_MATCH_EXACT, LTE_MODEM_AT_TIMEOUT_MS) != AT_ERR_OK)
  {
    return LTE_MODEM_ERR_TIMEOUT;
  }
  return LTE_MODEM_ERR_OK;
}

int32_t lte_modem_init(const LTE_MODEM_CFG_T *cfg)
{
  uint8_t attempt;

  if(cfg == NULL)
  {
    return LTE_MODEM_ERR_PARAM;
  }
  s_cfg = cfg;

  /* register UART3 callbacks and reset rx state */
  at_transport_init();

  /* release the module reset before powering up: PC0 high = module
   * running (must be kept high while powered, same as Collector-4G) */
  hw_gpio_init(s_modem_rst_cfg);
  s_modem_rst_cfg.port->BSC = s_modem_rst_cfg.pin;

  /* ---- 快速路径：模组可能已经在上电运行，先探 AT 通道，【不动电源】----
   * 升级场景下模组必然是【热的】—— APP 刚用它把固件下载进 UFS 才复位的，
   * boot 接手时它还在跑。而 RDY 只在模组【启动时】发一次，热模组不会再发，
   * 所以原来"先 power cycle 再等 RDY"在这里既多此一举又危险：
   * 本工程三个产品共用同一份 boot，但 PB1 的有效极性并不相同
   * （NEMA 低=上电，IoT-4G-v2 / Collector-4G 高=上电），那个 power cycle
   * 对其中一个必然是【反的】—— 会把好端端的模组断电，然后重试全部等不到 RDY，
   * 表现为升级整个失败（2026-09-21 NEMA 云端 OTA 实测就是这个症状）。
   * 所以先直接探 AT：通了就说明模组活着，什么都别动。 */
  delay_feed(LTE_MODEM_AT_PROBE_DELAY_MS);
  if(modem_at_channel_check() == LTE_MODEM_ERR_OK)
  {
    printf("modem already up (AT ok), skip power cycle\r\n");
    return LTE_MODEM_ERR_OK;
  }

  /* ---- 冷启动兜底：模组真的没起来，才走 power cycle + 等 RDY ---- */
  for(attempt = 0U; attempt <= cfg->rdy_retry_count; attempt++)
  {
    modem_power_cycle(cfg->power_cycle_hold_ms);

    if(modem_wait_rdy(cfg) != LTE_MODEM_ERR_OK)
    {
      /* loop runs rdy_retry_count + 1 times (first try + retries) */
      printf("modem no RDY, retry %d/%d\r\n",
             (int)(attempt + 1U), (int)(cfg->rdy_retry_count + 1U));
      continue;
    }

    /* RDY is just the UART-ready signal; the AT channel needs a moment
     * to settle. Sending the first AT too early gets swallowed (learned
     * in the Collector-4G project: RDY + 1500ms, 500ms was too short). */
    delay_feed(1500);

    if(modem_at_channel_check() == LTE_MODEM_ERR_OK)
    {
      printf("modem init successful!\r\n");
      return LTE_MODEM_ERR_OK;
    }
    printf("modem AT check failed, retry %d/%d\r\n",
           (int)(attempt + 1U), (int)(cfg->rdy_retry_count + 1U));
  }

  printf("modem init failed: no RDY!\r\n");
  return LTE_MODEM_ERR_NO_RDY;
}

int32_t lte_modem_reset(void)
{
  if(s_cfg == NULL)
  {
    return LTE_MODEM_ERR_PARAM;
  }
  return lte_modem_init(s_cfg);
}
