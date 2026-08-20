#ifndef _LTE_MODEM_H
#define _LTE_MODEM_H

#include "stdint.h"

/* modem driver error codes */
#define LTE_MODEM_ERR_OK      0
#define LTE_MODEM_ERR_NO_RDY (-1)
#define LTE_MODEM_ERR_TIMEOUT (-2)
#define LTE_MODEM_ERR_PARAM   (-3)

/* AT channel timeout (ms) */
#define LTE_MODEM_AT_TIMEOUT_MS  1000U

/* per-model configuration; add a new constant to support a new module */
typedef struct
{
  uint32_t      power_cycle_hold_ms;  /* how long the power pin is held high */
  uint32_t      rdy_timeout_ms;       /* max wait for the RDY line per attempt */
  uint8_t       rdy_retry_count;      /* full power-cycle retries if no RDY */
  const char   *rdy_pattern;          /* boot indication string, e.g. "RDY" */
} LTE_MODEM_CFG_T;

extern const LTE_MODEM_CFG_T LTE_MODEM_CFG_EG800K;
extern const LTE_MODEM_CFG_T LTE_MODEM_CFG_EC800;

/* power-cycle the module and bring up the AT channel (RDY + echo off) */
int32_t lte_modem_init(const LTE_MODEM_CFG_T *cfg);

/* full power-cycle + re-init using the last configuration */
int32_t lte_modem_reset(void);

#endif /* _LTE_MODEM_H */
