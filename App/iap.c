#include "iap.h"
#include "main.h"
#include "bsp_delay.h"
#include "apm32e10x_iwdt.h"


Upgrade_t upgrade;
Upgrade_cfg_t upgrade_cfg;


extern uint32_t CRC32(uint8_t *data, uint32_t length);


void Jump_to_App(uint32_t address)
{

  printf("\r\nJump_to_App...\r\n");
    /* Lock the Program memory */
    FMC_Lock();

    /* Stop SysTick before handing over. SysTick is a system exception
     * (SysTick_IRQn == -1), so the NVIC loop below does not cover it. It is
     * enabled by Delay_Init() and ticks every 1 ms; the APP writes SCB->VTOR
     * in its SystemInit(), long before its C runtime / RTOS is up, so the
     * next tick vectors into the APP's table and runs its SysTick_Handler on
     * an uninitialised runtime (RT-Thread: rt_tick_increase() ->
     * rt_thread_self() on a garbage rt_current_thread -> hard fault ->
     * lockup). Writing CTRL does not clear a pending tick, clear it too. */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    __DSB();

    for(int i=0; i<128; i++)
    {
      NVIC_DisableIRQ(i);
      NVIC_ClearPendingIRQ(i);
    }

    /* Jump to user application */
    JumpAddress = *(__IO uint32_t*)(address + 4);
    Jump_To_Application = (pFunction) JumpAddress;

    /* Initialize user application's Stack Pointer */
    __set_MSP(*(__IO uint32_t*) address);

    /* Jump to application */
    Jump_To_Application();
}


/* persist the RAM copy of upgrade metadata to flash (no reset);
 * interrupts off while programming to avoid FMC conflicts */
static void iap_state_flush(void)
{
  __disable_irq();
  hw_flash_erase(upgrade_cfg.config_addr, FLASH_PAGE_SIZE);
  hw_flash_write(upgrade_cfg.config_addr, &upgrade, sizeof(upgrade));
  __enable_irq();
}


int32_t iap_init(void)
{
  Upgrade_t *upgrade_temp;

  Upgrade_cfg_t *upgrade_cfg_temp;
  uint8_t cfg_need_init = 0;

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

  upgrade_temp = (Upgrade_t *)upgrade_cfg.config_addr;
  memcpy(&upgrade, upgrade_temp, sizeof(Upgrade_t));

  /* print metadata summary for debug log */
  printf("upgrade.flag=0x%X, fw_flag=%d, status=0x%X, up_count=%d\r\n",
         upgrade.flag, upgrade.fw_flag, upgrade.status, upgrade.up_count);
  printf("upgrade.fw[0].size=%lu fw[0].crc=0x%X\r\n",
         (unsigned long)upgrade.fw[0].size, (unsigned int)upgrade.fw[0].crc);
  printf("upgrade.fw[1].size=%lu fw[1].crc=0x%X\r\n",
         (unsigned long)upgrade.fw[1].size, (unsigned int)upgrade.fw[1].crc);

  if(upgrade_temp->flag == 0x5A)
  {
    /* run region verified against the recorded metadata */
    if(FW_VALID(&upgrade.fw[0]) &&
       (CRC32((uint8_t *)upgrade_cfg.app_addr, upgrade.fw[0].size) == upgrade.fw[0].crc))
    {
      /* trial countdown: boot decrements up_count each boot; if the new
       * firmware never confirmed healthy (APP writes status=OK), the
       * count reaches 0 and the boot loader rolls back to the backup */
      if(upgrade.status == UPGRADE_STATUS_TRIAL)
      {
        if(upgrade.up_count == 0U)
        {
          printf("trial exhausted, auto rollback\r\n");
          return UPGRADE_ACTION_AUTO_ROLLBACK;
        }
        upgrade.up_count--;
        iap_state_flush();
      }
      Jump_to_App(upgrade_cfg.app_addr);
    }

    /* run region corrupt or metadata invalid: restore from backup */
    if(FW_VALID(&upgrade.fw[1]) &&
       (CRC32((uint8_t *)upgrade_cfg.fw1_addr, upgrade.fw[1].size) == upgrade.fw[1].crc))
    {
      printf("app invalid, auto rollback\r\n");
      return UPGRADE_ACTION_AUTO_ROLLBACK;
    }

    /* no usable metadata at all, but the run region still holds firmware
     * (e.g. flashed directly by J-Link, fw[0] never recorded): jump anyway.
     * Without this a failed upgrade that saved flag=0x5A would leave the
     * boot loader stuck in an endless reset loop - fw[0] invalid -> cannot
     * jump, fw[1] invalid -> cannot rollback. */
    if(*(uint32_t *)upgrade_cfg.app_addr != 0xFFFFFFFFU)
    {
      printf("run region unverified, jump directly\r\n");
      Jump_to_App(upgrade_cfg.app_addr);
    }
    printf("no valid firmware, stay in boot\r\n");
    return -1;
  }
  else if(upgrade_temp->flag == 0x3A)
  {
    return 0x3A;
  }
  else if(upgrade_temp->flag == 0xAA)
  {
    return 0xAA;
  }
  else if(upgrade_temp->flag == 0xFF)
  {
    if(*(uint32_t *)upgrade_cfg.app_addr != 0xffffffff) Jump_to_App(upgrade_cfg.app_addr);
  }
  return -1;
}


void iap_upgrade_state_save(uint8_t flag, uint8_t count, uint8_t exit)
{
  upgrade.flag = flag;
  upgrade.up_count = count;

  printf("state save flag:0x%X fw0:0x%X/%lu fw1:0x%X/%lu\r\n", upgrade.flag,
         (unsigned int)upgrade.fw[0].crc, (unsigned long)upgrade.fw[0].size,
         (unsigned int)upgrade.fw[1].crc, (unsigned long)upgrade.fw[1].size);

  iap_state_flush();

  /* settle time after write */
  Delay_ms(200);

  NVIC_SystemReset();
}
