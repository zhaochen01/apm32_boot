
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


int32_t iap_init(void)
{

  Upgrade_t *upgrade_temp;
  
  Upgrade_cfg_t *upgrade_cfg_temp;
  
  
  upgrade_cfg_temp  = (Upgrade_cfg_t *)UPGRADE_CFG_ADDR;
  
  // 初始化
  if(upgrade_cfg_temp->data_crc != 0xFFFFFFFF)
  {
     uint32_t crc = CRC32((uint8_t *)UPGRADE_CFG_ADDR,\
                                    sizeof(Upgrade_cfg_t) - sizeof(uint32_t));
//    printf("upgrade_cfg crc:0x%X\tdata crc:0x%X\r\n", crc , upgrade_cfg_temp->data_crc);
    if(crc == upgrade_cfg_temp->data_crc)
    {
      memcpy(&upgrade_cfg, upgrade_cfg_temp, sizeof(Upgrade_cfg_t));
    }
    else
    {
      printf("upgrade_cfg crc error!\r\n");
      goto upgrade_cfg_init;
    }
  }
  else
//    if(upgrade_cfg_temp->config_addr == 0xFFFFFFFF  || upgrade_cfg_temp->app_addr == 0xFFFFFFFF \
                                                  || upgrade_cfg_temp->fw1_addr == 0xFFFFFFFF \
                                                  || upgrade_cfg_temp->fw2_addr == 0xFFFFFFFF \
                                                  || upgrade_cfg_temp->data_crc == 0xFFFFFFFF)
  {
upgrade_cfg_init:    
    
    memset(&upgrade_cfg, '\0', sizeof(Upgrade_cfg_t));
    
    upgrade_cfg.config_addr = UPGRADE_ADDR;
    upgrade_cfg.app_addr    = APP_ADDR;
    upgrade_cfg.fw1_addr    = UP_FILE_ADDR_1;
    upgrade_cfg.fw2_addr    = UP_FILE_ADDR_2;
    upgrade_cfg.data_crc    = CRC32((uint8_t *)&upgrade_cfg,\
                                    sizeof(Upgrade_cfg_t) - sizeof(uint32_t));
    /* 禁止中断，避免在擦写 flash 时被中断打断（可能导致写失败） */
    __disable_irq();
    
    hw_flash_erase(UPGRADE_CFG_ADDR, FLASH_PAGE_SIZE);
    hw_flash_write(UPGRADE_CFG_ADDR, &upgrade_cfg, sizeof(Upgrade_cfg_t));
    
    __enable_irq();
  }

  upgrade_temp      = (Upgrade_t *)upgrade_cfg.config_addr;
  memcpy(&upgrade, upgrade_temp, sizeof(Upgrade_t));
  
      // 调试信息：打印传入标志和 upgrade 结构的相关字段
  printf("upgrade.flag=0x%X, upgrade.fw_flag=%d\r\n", upgrade.flag, upgrade.fw_flag);
  printf("upgrade.fw[0].size=%lu fw[0].crc=0x%X\r\n", (unsigned long)upgrade.fw[0].size, (unsigned int)upgrade.fw[0].crc);
  printf("upgrade.fw[1].size=%lu fw[1].crc=0x%X\r\n", (unsigned long)upgrade.fw[1].size, (unsigned int)upgrade.fw[1].crc);
  printf("upgrade.fw[2].size=%lu fw[2].crc=0x%X\r\n", (unsigned long)upgrade.fw[2].size, (unsigned int)upgrade.fw[2].crc);
  
  if(upgrade_temp->flag == 0x5A)
  {
    Jump_to_App( upgrade_cfg.app_addr);
  }
  else if(upgrade_temp->flag == 0x3A)
  {
    return 0x3A;
  }
  else if(upgrade_temp->flag == 0xAA)
  {
    return 0xAA;
  }
  else if( upgrade_temp->flag == 0xFF)
  {
    if(*(uint32_t *)upgrade_cfg.app_addr != 0xffffffff) Jump_to_App( upgrade_cfg.app_addr);
  }
  return -1;
}


void iap_upgrade_state_save( uint8_t flag, uint8_t count, uint8_t exit)
{
  upgrade.flag = flag;
  upgrade.up_count = count;
  
  if( flag == 0x5A)
  {
    upgrade.fw[0].crc = upgrade.fw[upgrade.fw_flag].crc;
    upgrade.fw[0].size = upgrade.fw[upgrade.fw_flag].size; 
  }

  printf("selected fw_flag:%d\tcrc:0x%X\tsize:%d\r\n",\
            upgrade.fw_flag, upgrade.fw[upgrade.fw_flag].crc, upgrade.fw[upgrade.fw_flag].size);
  
  /* 禁止中断，避免在擦写 flash 时被中断打断（可能导致写失败） */
  __disable_irq();
  hw_flash_erase(upgrade_cfg.config_addr, FLASH_PAGE_SIZE);
  hw_flash_write(upgrade_cfg.config_addr, &upgrade, sizeof(upgrade));
  __enable_irq();

  /* 写后短延时 */
  Delay_ms(200);
  
 NVIC_SystemReset();
}



  
  
  
  