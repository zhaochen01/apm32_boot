/*!
 * @file        main.c
 *
 * @brief       Main program body
 *
 * @version     V1.0.3
 *
 * @date        2025-06-17
 *
 * @attention
 *
 *  Copyright (C) 2022-2025 Geehy Semiconductor
 *
 *  You may not use this file except in compliance with the
 *  GEEHY COPYRIGHT NOTICE (GEEHY SOFTWARE PACKAGE LICENSE).
 *
 *  The program is only for reference, which is distributed in the hope
 *  that it will be useful and instructional for customers to develop
 *  their software. Unless required by applicable law or agreed to in
 *  writing, the program is distributed on an "AS IS" BASIS, WITHOUT
 *  ANY WARRANTY OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the GEEHY SOFTWARE PACKAGE LICENSE for the governing permissions
 *  and limitations under the License.
 */

/* Includes */
#include "main.h"

#include "bsp_delay.h"
#include "hw_uart.h"
#include "lte_eg800k.h"

#include "apm32e10x_iwdt.h"

#include "iap.h"




#define DEBUG_USART USART1

uint8_t count = 0;




// WDT 看门狗 
HW_GPIO_INIT_T Hw_WDT_Cfg = { RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_0, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };

HW_GPIO_INIT_T Hw_RUN_Cfg = { RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_6, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };
HW_GPIO_INIT_T Hw_DATA_Cfg = { RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_7, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };
HW_GPIO_INIT_T Hw_SIM_Cfg = { RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_9, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };


uint32_t JumpAddress;
pFunction Jump_To_Application;




void hw_gpio_init( HW_GPIO_INIT_T gpio_cfg)
{
  	/* Enable GPIO clock */
	GPIO_Config_T  configStruct;
  
  /* Enable the GPIO_LED Clock */
  RCM_EnableAPB2PeriphClock(gpio_cfg.clk);

  /* Configure the GPIO_LED pin */
  configStruct.pin 		= gpio_cfg.pin;
  configStruct.mode 	= gpio_cfg.mode;
  configStruct.speed 	= gpio_cfg.speed;

  GPIO_Config(gpio_cfg.port, &configStruct);
}




int32_t get_data_len( char *pdat)
{
  uint32_t val = 0U;
  const char *prefix = "CONNECT ";
  char *p = NULL;

  if(pdat == NULL)
  {
    return -1;
  }

  /* 行内搜索 CONNECT，容忍前导噪声/残留CRLF */
  p = strstr(pdat, prefix);
  if(p == NULL) return -1;
  p += strlen(prefix);

  if((*p < '0') || (*p > '9'))
  {
    return -1;
  }

  while((*p >= '0') && (*p <= '9'))
  {
    val = (val * 10U) + (uint32_t)(*p - '0');
    p++;
  }

  if((val == 0U) || (val > LTE_READ_CHUNK_SIZE))
  {
    return -1;
  }

  printf("get data len -> %lu\r\n", (unsigned long)val);
  return (int32_t)val;
}


int32_t get_file_len(char *pdat)
{
  uint32_t val = 0U;
  char *p = NULL;

  if(pdat == NULL)
  {
    return -1;
  }

  /* 典型格式：+QFLST: "UFS:iot_update.bin",89584 */
  p = strrchr(pdat, ',');
  if((p == NULL) || (*(p + 1) < '0') || (*(p + 1) > '9'))
  {
    printf("get file len error!\r\n");
    return -1;
  }

  p++;
  while((*p >= '0') && (*p <= '9'))
  {
    uint32_t prev = val;
    val = (val * 10U) + (uint32_t)(*p - '0');
    if(val < prev) /* overflow guard */
    {
      printf("get file len overflow!\r\n");
      return -1;
    }
    p++;
  }

  if(val > 0U)
  {
    printf("up file size %lu\r\n", (unsigned long)val);
    return (int32_t)val;
  }

  printf("get file len error!\r\n");
  return -1;
}

int32_t get_qfopen_fd(char *pdat)
{
  int32_t fd = 0;
  char *p = NULL;

  if(pdat == NULL)
  {
    return -1;
  }

  p = strstr(pdat, "+QFOPEN:");
  if(p != NULL)
  {
    p = strchr(p, ':');
    if(p != NULL)
    {
      p++;
      while((*p == ' ') || (*p == '\t')) p++;
      if((*p >= '0') && (*p <= '9'))
      {
        fd = 0;
        while((*p >= '0') && (*p <= '9'))
        {
          fd = (fd * 10) + (*p - '0');
          p++;
        }
        printf("QFOPEN fd:%d\r\n", fd);
        return fd;
      }
    }
  }

  printf("get qfopen fd error!\r\n");
  return -1;
}

/*!
 * @brief   This function handles TMR1 Update Handler
 *
 * @param   None
 *
 * @retval  None
 *
 */
void TMR5_IRQHandler(void)
{
  static uint8_t i = 0;
    
  if (TMR_ReadIntFlag(TMR5, TMR_INT_UPDATE) == SET)
  {
//    printf("tim5!\r\n");
      if( i == 0)
      {
        Hw_WDT_Cfg.port->BC  = Hw_WDT_Cfg.pin;
        Hw_RUN_Cfg.port->BC  = Hw_RUN_Cfg.pin;
        Hw_DATA_Cfg.port->BC = Hw_DATA_Cfg.pin;
        Hw_SIM_Cfg.port->BC  = Hw_SIM_Cfg.pin;
        i = 1;
      }
      else
      {
        Hw_WDT_Cfg.port->BSC  = Hw_WDT_Cfg.pin;
        Hw_RUN_Cfg.port->BSC  = Hw_RUN_Cfg.pin;
        Hw_DATA_Cfg.port->BSC = Hw_DATA_Cfg.pin;
        Hw_SIM_Cfg.port->BSC  = Hw_SIM_Cfg.pin;
        i = 0;
      }
    
    TMR_ClearIntFlag(TMR5, TMR_INT_UPDATE);
  }
}


/**
 * @brief  TIM5初始化（500ms定时中断，适配36MHz主频）
 * @retval 无
 */
void TIM5_Init_500ms_Interrupt(void)
{
    TMR_BaseConfig_T TMR_BaseConfigStruct;

    RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_TMR5);

    TMR_Reset(TMR5);
  
    TMR_BaseConfigStruct.clockDivision = TMR_CLOCK_DIV_1;        // 采样分频1（通用定时器默认值，移除冗余的DIV4）
    TMR_BaseConfigStruct.countMode = TMR_COUNTER_MODE_UP;        // 向上计数模式
    // 预分频：36MHz / (35999 + 1) = 1000Hz（1ms/计数）
    TMR_BaseConfigStruct.division = 35999;                      
    // 自动重装：1000Hz × 500ms = 500次 → 499（500-1）
    TMR_BaseConfigStruct.period = 499; 
    TMR_BaseConfigStruct.repetitionCounter = 0;
    TMR_ConfigTimeBase(TMR5, &TMR_BaseConfigStruct);

    TMR_ClearIntFlag(TMR5, TMR_INT_UPDATE);
    TMR_EnableInterrupt(TMR5, TMR_INT_UPDATE);
    NVIC_EnableIRQRequest(TMR5_IRQn, 2, 0);

    TMR_Enable(TMR5);
}


/**
 * @brief  逐位计算CRC32（标准IEEE 802.3多项式：0xEDB88320）
 * @param  data: 输入数据缓冲区
 * @param  length: 数据长度（字节数）
 * @retval 最终的CRC32值（取反后）
 */
uint32_t CRC32(uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFF; // 初始值
    uint32_t poly = 0xEDB88320; // 标准CRC32多项式（反向）

    for (uint32_t i = 0; i < length; i++)
    {
        // 当前字节与CRC低8位异或
        crc ^= data[i];
        // 逐位处理（8位）
        for (uint8_t j = 0; j < 8; j++)
        {
            // 右移，最低位为1则异或多项式
            crc = (crc >> 1) ^ ((crc & 0x01) ? poly : 0);
        }
    }
    // 最终结果取反
    return ~crc;
}

static uint32_t flash_align_page(uint32_t size)
{
  return (size + FLASH_PAGE_SIZE - 1U) & (~(FLASH_PAGE_SIZE - 1U));
}

static int32_t flash_erase_with_wdt(uint32_t addr, uint32_t size)
{
  uint32_t erase_len = flash_align_page(size);
  uint32_t offset = 0U;

  while(offset < erase_len)
  {
    IWDT_Refresh();
    (void)hw_flash_erase(addr + offset, FLASH_PAGE_SIZE);
    offset += FLASH_PAGE_SIZE;
  }
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


int32_t iap_upgrade( uint8_t up_flag)
{
  uint32_t expect_size = 0U, expect_crc = 0U, file_len = 0U, file_count = LTE_READ_CHUNK_SIZE;
  uint32_t crc, erase_count, up_file_addr = upgrade_cfg.fw1_addr; /* 默认地址 */
  uint32_t write_offset = 0;
  uint32_t read_wait_cnt = 0;
  uint8_t read_retry_cnt = 0;
  int32_t qf_fd = -1;
  char qfread_cmd[40];
  char qfclose_cmd[24];
  
  static uint8_t fw_flag = 0, up_count = 0;
  


  if (up_flag == 0x3A)
  {
    /* 主动升级：切换到另一个缓存区（1 <-> 2） */
    if (upgrade.fw_flag == 1)
    {
      fw_flag = 2;
    }
    else
    {
      fw_flag = 1;
    }
    up_file_addr = (fw_flag == 2) ? upgrade_cfg.fw2_addr : upgrade_cfg.fw1_addr;
  }
  else if (up_flag == 0xAA)
  {
    /* 回退或按标志执行：优先使用结构中记录的 fw_flag（有效性检查） */
    if (upgrade.fw_flag == 2) fw_flag = 2;
    else fw_flag = 1;
    up_file_addr = (fw_flag == 2) ? upgrade_cfg.fw2_addr : upgrade_cfg.fw1_addr;
  }

  printf("selected fw_flag:%d\tup_file_addr:0x%X \r\n", fw_flag, up_file_addr);

  if(up_flag == 0x3A)
  {
    expect_size = upgrade.size;
    expect_crc = upgrade.crc;
  }
  else if((fw_flag >= 1U) && (fw_flag <= 2U) &&
          (upgrade.fw[fw_flag].size > 0U) &&
          (upgrade.fw[fw_flag].size != 0xFFFFFFFFU) &&
          (upgrade.fw[fw_flag].crc != 0xFFFFFFFFU))
  {
    expect_size = upgrade.fw[fw_flag].size;
    expect_crc = upgrade.fw[fw_flag].crc;
  }
  if((expect_size == 0U) || (expect_size == 0xFFFFFFFFU) ||
     (expect_crc == 0xFFFFFFFFU))
  {
    printf("expect param invalid, abort\r\n");
    iap_upgrade_state_save(0x5A, 0, 1);
  }
  upgrade.size = expect_size;
  
  // 擦除固件缓存区
//  printf("erase_count:%d\tupgrade.size:%d\r\n", erase_count, upgrade.size);
  
  // 回退功能
  if(up_flag == 0xAA)
  {
      crc = CRC32((uint8_t *)up_file_addr, expect_size);
      printf("AA up crc:0x%X\r\n", crc);
      
      if(crc == upgrade.fw[fw_flag].crc)
      {
flag_0xAA:
        IWDT_Refresh();
        erase_count = flash_erase_with_wdt(upgrade_cfg.app_addr, expect_size);
        (void)erase_count;
        flash_write_with_wdt(upgrade_cfg.app_addr, (uint8_t *)up_file_addr, expect_size);

        crc = CRC32((uint8_t *)upgrade_cfg.app_addr, expect_size);
        printf("AA app crc:0x%X\r\n", crc);
        
        if(crc == upgrade.fw[fw_flag].crc)
        {
          // 升级成功
          upgrade.fw_flag = fw_flag;
          iap_upgrade_state_save(0x5A, 0, 1);
        }
        else
        {
           if(up_count < 5 )
          {
            up_count++;
            goto flag_0xAA;
            //iap_upgrade_state_save(0xAA, upgrade.up_count += 1, 1);
          }
          else NVIC_SystemReset();
        }
      }
      else NVIC_SystemReset();
  }
  
  up_count = 0;
  eg800k_init();
  
flag_0x3A:
  IWDT_Refresh();
  RX_FLAG = 0;
  FILE_SIZE = 0;
  file_len = expect_size;
  write_offset = 0;
  read_wait_cnt = 0;
  read_retry_cnt = 0;
  qf_fd = -1;
  erase_count = flash_erase_with_wdt(up_file_addr, expect_size);
  (void)erase_count;

  LTE_SendCmdWaitResp("AT\r\n\0", "OK", 500);
  LTE_SendCmdWaitResp("ATE0\r\n\0", "OK", 500);
  
  if(LTE_SendCmdWaitResp("AT+QFOPEN=\"UFS:iot_update.bin\",2\r\n\0", "+QFOPEN:", 1000) != 0)
  {
    iap_upgrade_state_save(0x5A, 0, 1);
  }
  qf_fd = get_qfopen_fd((char *)uart3_rx_buf);
  if(qf_fd < 0)
  {
    iap_upgrade_state_save(0x5A, 0, 1);
  }

//  LTE_SendCmdWaitResp("AT+QFLST=\"*\"\r\n\0", "OK", 10000);
//  Delay_ms(500);

  if(LTE_SendCmdWaitResp("AT+QFLST=\"UFS:iot_update.bin\"\r\n\0", "+QFLST:", 1000) ==0)
  {
    int32_t file_size = get_file_len((char *)uart3_rx_buf);
    printf("QFLST file size:%ld\texpect size:%lu\r\n", (long)file_size, (unsigned long)expect_size);
    if((file_size <= 0) || ((uint32_t)file_size != expect_size))
    {
      // 文件大小不一致，退出本次升级。
//      LTE_SendCmdWaitResp("AT+QFCLOSE=1\r\n\0", "OK", 1000);
//      Delay_ms(200);
//      goto flag_0x3A;
      iap_upgrade_state_save(0x5A, 0, 1);
//    NVIC_SystemReset();
    }
    else
    {
      printf("up file size ok!\r\n");
    }
  }
  else
  {
    iap_upgrade_state_save(0x5A, 0, 1);
  }
  
  while(1)
  {      
    
    if(file_len < LTE_READ_CHUNK_SIZE)  file_count = file_len;
    else file_count = LTE_READ_CHUNK_SIZE;

    if(file_len > 0 && RX_FLAG == 0)
    {
      IWDT_Refresh();
      (void)snprintf(qfread_cmd, sizeof(qfread_cmd), "AT+QFREAD=%d,%lu\r\n", (int)qf_fd, (unsigned long)file_count);
      LTE_SendCmdWaitResp(qfread_cmd, NULL, 100);
      RX_FLAG = 1;
      read_wait_cnt = 0;
    }
    else if((RX_FLAG == 1) || (RX_FLAG == 2))
    {
      IWDT_Refresh();
      read_wait_cnt++;
      if(read_wait_cnt >= 10000U) /* 10s timeout with 1ms loop delay */
      {
        __disable_irq();
        uart3_rx_len = 0;
        memset(uart3_rx_buf, 0, UART3_RX_BUF_SIZE);
        __enable_irq();
        RX_FLAG = 0;
        FILE_SIZE = 0;
        read_wait_cnt = 0;
        if(read_retry_cnt < 3)
        {
          read_retry_cnt++;
          printf("QFREAD timeout, retry chunk:%d\r\n", read_retry_cnt);
        }
        else
        {
          if(up_count < 5)
          {
            up_count++;
            printf("QFREAD timeout, restart file:%d\r\n", up_count);
            goto flag_0x3A;
          }
          else
          {
            iap_upgrade_state_save(0x5A, 0, 1);
          }
        }
      }
      Delay_ms(1);
    }
    else if(RX_FLAG == 3)
    {
      uint32_t chunk_size = FILE_SIZE;
      if((chunk_size == 0U) || (chunk_size > file_count))
      {
        RX_FLAG = 0;
        FILE_SIZE = 0;
        read_wait_cnt = 0;
        if(read_retry_cnt < 3)
        {
          read_retry_cnt++;
          printf("QFREAD size error, retry chunk:%d\r\n", read_retry_cnt);
        }
        else
        {
          if(up_count < 5)
          {
            up_count++;
            printf("QFREAD size error, restart file:%d\r\n", up_count);
            goto flag_0x3A;
          }
          else
          {
            iap_upgrade_state_save(0x5A, 0, 1);
          }
        }
        continue;
      }
      if((write_offset > expect_size) || (chunk_size > (expect_size - write_offset)))
      {
        printf("write overflow offset:%lu size:%lu up:%lu\r\n",
               (unsigned long)write_offset,
               (unsigned long)chunk_size,
               (unsigned long)expect_size);
        iap_upgrade_state_save(0x5A, 0, 1);
      }
      printf("flash write offset:%lu size:%lu\r\n", (unsigned long)write_offset, (unsigned long)chunk_size);
      flash_write_with_wdt(up_file_addr + write_offset, up_file_buf, chunk_size);
      write_offset += chunk_size;
      /* 在成功写入后再减少剩余长度，避免在发送命令后立即把 file_len 置为0 导致提前关闭 */
      if (file_len > 0)
      {
        if (file_len > chunk_size) file_len -= chunk_size;
        else file_len = 0;
      }
      printf("file_len %lu\r\n\r\n", (unsigned long)(expect_size - file_len));
      read_retry_cnt = 0;
      read_wait_cnt = 0;
      RX_FLAG = 0;
      FILE_SIZE = 0;
    }
    else if( file_len == 0)
    {
      IWDT_Refresh();
      (void)snprintf(qfclose_cmd, sizeof(qfclose_cmd), "AT+QFCLOSE=%d\r\n", (int)qf_fd);
      LTE_SendCmdWaitResp(qfclose_cmd, "OK", 300);
      Delay_ms(200);
      crc = CRC32((uint8_t *)up_file_addr, expect_size);
      printf("up crc:0x%X\r\n", crc);

      if(crc == expect_crc)
      {
        printf("up file crc ojbk!\r\n");

        // 文件成功更新到缓存固件区域
        upgrade.fw[fw_flag].crc = crc;
        upgrade.fw[fw_flag].size = expect_size;

        uint8_t app_count = 0;

flag_app:
        IWDT_Refresh();        
        erase_count = flash_erase_with_wdt(upgrade_cfg.app_addr, expect_size);
        (void)erase_count;
        flash_write_with_wdt(upgrade_cfg.app_addr, (uint8_t *)up_file_addr, expect_size);

        crc = CRC32((uint8_t *)upgrade_cfg.app_addr, expect_size);
        printf("app crc:0x%X\r\n", crc);

        if(crc == expect_crc)
        {
          // 文件成功更新到app区域
          printf("app file crc ojbk!\r\n");
          upgrade.fw_flag = fw_flag;
          upgrade.fw[fw_flag].size = expect_size;
          upgrade.fw[fw_flag].crc = crc;
          iap_upgrade_state_save(0x5A, 0, 1);
        }
        else
        {
          if(app_count < 5 )
          {
            app_count++;
            goto flag_app;
          }
          else iap_upgrade_state_save(0xAA, 0, 0); // 超过5次，执行回退版本操作。
        }
      }
      else
      {
        if(up_count < 5 )
        {
          up_count++;
          file_len = expect_size;
          goto flag_0x3A;
        }
        else iap_upgrade_state_save(0x5A, 0, 1);
      }

    }
    else
    {
      Delay_ms(1);
    }
  }

  return -1;
}





/** @defgroup USART_Interrupt_Functions Functions
  @{
*/

/*!
 * @brief       Main program
 *
 * @param       None
 *
 * @retval      None
 *
 */
int main(void)
{
  static uint16_t i = 0, flag = 0, erase_count = 0;
  
  int32_t req = -1;
  
  uint32_t file_len = 0, file_count = 2048;
  uint32_t crc;
  
  
	SystemInit();
	SystemCoreClockUpdate();
  
  Delay_Init();

  // 使能SWJ口下载，禁用jtag下载。
  RCM_EnableAPB2PeriphClock(RCM_APB2_PERIPH_AFIO);
  GPIO_ConfigPinRemap(GPIO_REMAP_SWJ_JTAGDISABLE);
 
  hw_gpio_init(Hw_WDT_Cfg);
  hw_gpio_init(Hw_RUN_Cfg);
  hw_gpio_init(Hw_DATA_Cfg);
  hw_gpio_init(Hw_SIM_Cfg);

  TIM5_Init_500ms_Interrupt();

  user_uart_init();
  
  printf("\r\nBoot Loader... %s\r\n", VERSION);
  
  req = iap_init();

  IWDT_EnableWriteAccess();
  IWDT_ConfigDivider(IWDT_DIVIDER_256);
  IWDT_ConfigReload(2343);
  IWDT_Refresh();
  IWDT_Enable();
    
  if( req >= 0 )      iap_upgrade(req);
  else  NVIC_SystemReset();
  
  while(1)
  {      
    
    Delay_ms(100);
  }
}




#if defined (__CC_ARM) || defined (__ICCARM__) || (defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050))

/*!
* @brief       Redirect C Library function printf to serial port.
*              After Redirection, you can use printf function.
*
* @param       ch:  The characters that need to be send.
*
* @param       *f:  pointer to a FILE that can recording all information
*              needed to control a stream
*
* @retval      The characters that need to be send.
*
* @note
*/
int fputc(int ch, FILE* f)
{
 // __disable_irq();
  uart_send_data(DEBUG_USART, &ch, 1);
//  __enable_irq();
  return (ch);
}

#elif defined (__GNUC__)

/*!
* @brief       Redirect C Library function printf to serial port.
*              After Redirection, you can use printf function.
*
* @param       ch:  The characters that need to be send.
*
* @retval      The characters that need to be send.
*
* @note
*/
int __io_putchar(int ch)
{
    /* send a byte of data to the serial port */
    USART_TxData(DEBUG_USART, ch);

    /* wait for the data to be send  */
    while (USART_ReadStatusFlag(DEBUG_USART, USART_FLAG_TXBE) == RESET);

    return ch;
}

/*!
* @brief       Redirect C Library function printf to serial port.
*              After Redirection, you can use printf function.
*
* @param       file:  Meaningless in this function.
*
* @param       *ptr:  Buffer pointer for data to be sent.
*
* @param       len:  Length of data to be sent.
*
* @retval      The characters that need to be send.
*
* @note
*/
int _write(int file, char* ptr, int len)
{
	UNUSED(file);
    int i;
    for (i = 0; i < len; i++)
    {
        __io_putchar(*ptr++);
    }

    return len;
}

#else
#warning Not supported compiler type
#endif

/**@} end of group USART_Interrupt_Functions */
/**@} end of group USART_Interrupt */
/**@} end of group Examples */
