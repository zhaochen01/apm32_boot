#ifndef _UPGRADE_CTRL_H
#define _UPGRADE_CTRL_H

#include "stdint.h"

/* retry and timing configuration */
#define UPGRADE_MAX_DL_RETRIES     5U    /* whole-file download retries */
#define UPGRADE_MAX_APP_RETRIES    5U    /* app area write retries */
#define UPGRADE_QFREAD_TIMEOUT_MS  10000U
#define UPGRADE_QFREAD_RECOVER_MS  5000U /* drain wait after QFREAD failure */
#define UPGRADE_FILE_NAME          "UFS:iot_update.bin"

/* one firmware chunk (raw receive buffer size) */
#define LTE_READ_CHUNK_SIZE        40960U

typedef enum
{
  UPGRADE_ACTION_NETWORK  = 0x3A,   /* download from 4G module */
  UPGRADE_ACTION_ROLLBACK = 0xAA    /* copy current slot to app area */
} UPGRADE_ACTION_T;

/* upgrade parameters; caller fills before calling upgrade_run() */
typedef struct
{
  UPGRADE_ACTION_T action;
  uint8_t          fw_flag;        /* target slot: 1 or 2 */
  uint32_t         up_file_addr;   /* slot base address */
  uint32_t         expect_size;    /* expected firmware size */
  uint32_t         expect_crc;     /* expected CRC32 */
} UPGRADE_CTX_T;

extern uint32_t CRC32(uint8_t *data, uint32_t length);

/* run the upgrade; never returns (saves state and resets the MCU) */
void upgrade_run(const UPGRADE_CTX_T *ctx);

#endif /* _UPGRADE_CTRL_H */
