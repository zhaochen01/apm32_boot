/*!
 * @file        upgrade_ctrl.c
 *
 * @brief       Upgrade state machine replacing the legacy goto-based
 *              iap_upgrade() in main.c. Two actions:
 *                - 0x3A: download firmware from the 4G module UFS into
 *                        a firmware slot, verify CRC, copy to app area
 *                - 0xAA: copy the current pre-verified slot to app area
 *              Retries are explicit counters in while loops, no gotos.
 *              All long-running loops refresh the IWDT.
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
    IWDT_Refresh();
    FMC_ErasePage(addr + offset);
    offset += FLASH_PAGE_SIZE;
  }
  FMC_Lock();
  IWDT_Refresh();
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
    IWDT_Refresh();
    (void)hw_flash_write(addr + offset, data + offset, chunk);
    offset += chunk;
  }
  IWDT_Refresh();
  return 0;
}

/* after a failed QFREAD the module may still stream raw data;
 * drain rx and wait for it to return to AT command mode */
static void upgrade_recover_wait_with_wdt(uint32_t wait_ms)
{
  uint32_t wait_cnt = 0U;

  for(wait_cnt = 0U; wait_cnt < wait_ms; wait_cnt++)
  {
    IWDT_Refresh();
    at_flush_rx();
    Delay_ms(1);
  }
  at_flush_rx();
  IWDT_Refresh();
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
    iap_upgrade_state_save(0x5A, 0, 1);
  }

  /* empty flash reads as 0xFFFFFFFF: reject invalid expectations */
  if((ctx->expect_size == 0U) || (ctx->expect_size == 0xFFFFFFFFU) ||
     (ctx->expect_crc == 0xFFFFFFFFU))
  {
    printf("expect param invalid, abort\r\n");
    iap_upgrade_state_save(0x5A, 0, 1);
  }

  memset(&file_ctx, 0, sizeof(file_ctx));
  file_ctx.fd = -1;

  /* ================= download phase (0x3A only) ================= */
  if(ctx->action == UPGRADE_ACTION_NETWORK)
  {
    upgrade.size = ctx->expect_size;

    if(lte_modem_init(&LTE_MODEM_CFG_EG800K) != LTE_MODEM_ERR_OK)
    {
      printf("modem init failed, abort\r\n");
      iap_upgrade_state_save(0x5A, 0, 1);
    }

    while(dl_retry < UPGRADE_MAX_DL_RETRIES)
    {
      uint32_t offset = 0U;
      int32_t dl_failed = 0;

      printf("download attempt %d/%d\r\n",
             (int)(dl_retry + 1U), (int)UPGRADE_MAX_DL_RETRIES);

      /* erase target slot before download */
      IWDT_Refresh();
      (void)flash_erase_with_wdt(ctx->up_file_addr, ctx->expect_size);

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

      /* chunked download loop */
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
            iap_upgrade_state_save(0x5A, 0, 1);
          }

          printf("flash write offset:%lu size:%lu\r\n",
                 (unsigned long)offset, (unsigned long)actual);
          flash_write_with_wdt(ctx->up_file_addr + offset, up_file_buf, actual);
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
      crc = CRC32((uint8_t *)ctx->up_file_addr, ctx->expect_size);
      printf("up crc:0x%X\r\n", (unsigned int)crc);

      if(crc == ctx->expect_crc)
      {
        printf("up file crc ok!\r\n");
        upgrade.fw[ctx->fw_flag].crc = crc;
        upgrade.fw[ctx->fw_flag].size = ctx->expect_size;
        break;   /* download success */
      }

      printf("up file crc mismatch, retry %d/%d\r\n",
             (int)(dl_retry + 1U), (int)UPGRADE_MAX_DL_RETRIES);
      (void)lte_modem_reset();
      upgrade_recover_wait_with_wdt(UPGRADE_QFREAD_RECOVER_MS);
      dl_retry++;
    }

    if(dl_retry >= UPGRADE_MAX_DL_RETRIES)
    {
      printf("download retries exhausted, abort\r\n");
      iap_upgrade_state_save(0x5A, 0, 1);
    }
  }
  else
  {
    /* 0xAA rollback: slot must match the recorded CRC before copying */
    crc = CRC32((uint8_t *)ctx->up_file_addr, ctx->expect_size);
    printf("AA slot crc:0x%X\r\n", (unsigned int)crc);
    if(crc != ctx->expect_crc)
    {
      printf("AA slot crc mismatch, reset\r\n");
      NVIC_SystemReset();
    }
  }

  /* ================= app write phase (0x3A and 0xAA) ================= */
  while(app_retry < UPGRADE_MAX_APP_RETRIES)
  {
    IWDT_Refresh();
    (void)flash_erase_with_wdt(upgrade_cfg.app_addr, ctx->expect_size);
    flash_write_with_wdt(upgrade_cfg.app_addr, (uint8_t *)ctx->up_file_addr,
                         ctx->expect_size);

    crc = CRC32((uint8_t *)upgrade_cfg.app_addr, ctx->expect_size);
    printf("app crc:0x%X\r\n", (unsigned int)crc);

    if(crc == ctx->expect_crc)
    {
      printf("app crc ok!\r\n");
      upgrade.fw_flag = ctx->fw_flag;
      upgrade.fw[ctx->fw_flag].size = ctx->expect_size;
      upgrade.fw[ctx->fw_flag].crc = crc;
      iap_upgrade_state_save(0x5A, 0, 1);
    }

    printf("app crc mismatch, retry %d/%d\r\n",
           (int)(app_retry + 1U), (int)UPGRADE_MAX_APP_RETRIES);
    app_retry++;
  }

  /* app write retries exhausted */
  if(ctx->action == UPGRADE_ACTION_NETWORK)
  {
    /* schedule local rollback on next boot */
    printf("app write retries exhausted, schedule rollback\r\n");
    iap_upgrade_state_save(0xAA, 0, 0);
  }
  else
  {
    /* keep the 0xAA flag and retry the rollback */
    printf("app write retries exhausted, reset\r\n");
    NVIC_SystemReset();
  }
}
