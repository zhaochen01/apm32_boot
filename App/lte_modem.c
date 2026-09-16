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

const LTE_MODEM_CFG_T LTE_MODEM_CFG_EC800 =
{
  .power_cycle_hold_ms = 500U,
  .rdy_timeout_ms      = 5000U,
  .rdy_retry_count     = 2U,
  .rdy_pattern         = "RDY"
};

static void modem_power_cycle(uint32_t hold_ms)
{
  hw_gpio_init(s_modem_power_cfg);

  /* low (power off) -> high (power on, stay high) */
  s_modem_power_cfg.port->BC = s_modem_power_cfg.pin;
  Delay_ms(hold_ms);
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
    Delay_ms(1500);

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
