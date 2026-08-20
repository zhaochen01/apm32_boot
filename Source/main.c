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
#include "upgrade_ctrl.h"

#include "apm32e10x_iwdt.h"

#include "iap.h"




#define DEBUG_USART USART1

uint8_t count = 0;




// WDT ���Ź� 
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
 * @brief  TIM5��ʼ����500ms��ʱ�жϣ�����36MHz��Ƶ��
 * @retval ��
 */
void TIM5_Init_500ms_Interrupt(void)
{
    TMR_BaseConfig_T TMR_BaseConfigStruct;

    RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_TMR5);

    TMR_Reset(TMR5);
  
    TMR_BaseConfigStruct.clockDivision = TMR_CLOCK_DIV_1;        // ������Ƶ1��ͨ�ö�ʱ��Ĭ��ֵ���Ƴ������DIV4��
    TMR_BaseConfigStruct.countMode = TMR_COUNTER_MODE_UP;        // ���ϼ���ģʽ
    // Ԥ��Ƶ��36MHz / (35999 + 1) = 1000Hz��1ms/������
    TMR_BaseConfigStruct.division = 35999;                      
    // �Զ���װ��1000Hz �� 500ms = 500�� �� 499��500-1��
    TMR_BaseConfigStruct.period = 499; 
    TMR_BaseConfigStruct.repetitionCounter = 0;
    TMR_ConfigTimeBase(TMR5, &TMR_BaseConfigStruct);

    TMR_ClearIntFlag(TMR5, TMR_INT_UPDATE);
    TMR_EnableInterrupt(TMR5, TMR_INT_UPDATE);
    NVIC_EnableIRQRequest(TMR5_IRQn, 2, 0);

    TMR_Enable(TMR5);
}


/**
 * @brief  ��λ����CRC32����׼IEEE 802.3����ʽ��0xEDB88320��
 * @param  data: �������ݻ�����
 * @param  length: ���ݳ��ȣ��ֽ�����
 * @retval ���յ�CRC32ֵ��ȡ����
 */
uint32_t CRC32(uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFF; // ��ʼֵ
    uint32_t poly = 0xEDB88320; // ��׼CRC32����ʽ������

    for (uint32_t i = 0; i < length; i++)
    {
        // ��ǰ�ֽ���CRC��8λ���
        crc ^= data[i];
        // ��λ������8λ��
        for (uint8_t j = 0; j < 8; j++)
        {
            // ���ƣ����λΪ1��������ʽ
            crc = (crc >> 1) ^ ((crc & 0x01) ? poly : 0);
        }
    }
    // ���ս��ȡ��
    return ~crc;
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
  int32_t req = -1;


	SystemInit();
	SystemCoreClockUpdate();
  
  Delay_Init();

  // ʹ��SWJ�����أ�����jtag���ء�
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
    
  if(req == 0x3A)
  {
    UPGRADE_CTX_T ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.action = UPGRADE_ACTION_NETWORK;
    ctx.fw_flag = (upgrade.fw_flag == 1) ? 2 : 1;   /* toggle slot */
    ctx.up_file_addr = (ctx.fw_flag == 2) ? upgrade_cfg.fw2_addr : upgrade_cfg.fw1_addr;
    ctx.expect_size = upgrade.size;
    ctx.expect_crc = upgrade.crc;
    upgrade_run(&ctx);
  }
  else if(req == 0xAA)
  {
    UPGRADE_CTX_T ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.action = UPGRADE_ACTION_ROLLBACK;
    ctx.fw_flag = (upgrade.fw_flag == 2) ? 2 : 1;   /* current slot */
    ctx.up_file_addr = (ctx.fw_flag == 2) ? upgrade_cfg.fw2_addr : upgrade_cfg.fw1_addr;
    ctx.expect_size = upgrade.fw[ctx.fw_flag].size;
    ctx.expect_crc = upgrade.fw[ctx.fw_flag].crc;
    upgrade_run(&ctx);
  }

  NVIC_SystemReset();

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
