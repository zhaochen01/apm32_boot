

#include "main.h"
#include "hw_uart.h"


/* USART1 TX Buf */
/* USART2 RX Buf */





				

typedef struct
{
	USART_T*  						uart;
	uint32_t							baudRate;
	uint32_t							mode;
	uint32_t 							uart_clk;
	
  HW_GPIO_INIT_T        tx_gpio;
	
  HW_GPIO_INIT_T        rx_gpio;
}User_Uart_Config_T;



User_Uart_Config_T uart_cfg_list[] = 
{
	{ USART1, 115200, USART_MODE_TX_RX, RCM_APB2_PERIPH_USART1,\
		RCM_APB2_PERIPH_GPIOA, GPIOA, GPIO_PIN_9,  GPIO_SPEED_50MHz, GPIO_MODE_AF_PP,\
		RCM_APB2_PERIPH_GPIOA, GPIOA, GPIO_PIN_10, GPIO_SPEED_50MHz, GPIO_MODE_IN_FLOATING
	},
	
	{ USART3, 115200, USART_MODE_TX_RX, RCM_APB1_PERIPH_USART3,\
		RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_10, GPIO_SPEED_50MHz, GPIO_MODE_AF_PP,\
		RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_11, GPIO_SPEED_50MHz, GPIO_MODE_IN_PU
	},
	
};

#define HW_USART_NUM          (sizeof(uart_cfg_list) / sizeof( User_Uart_Config_T ))


void user_uart_init(void)
{
	GPIO_Config_T g_configStruct;
	USART_Config_T u_configStruct;

  User_Uart_Config_T *uart_cfg;
	for(uint8_t i = 0; i < HW_USART_NUM; i++)
	{
    uart_cfg = &uart_cfg_list[i];
		if (uart_cfg->uart == USART1)	RCM_EnableAPB2PeriphClock(uart_cfg->uart_clk);
    else 													RCM_EnableAPB1PeriphClock(uart_cfg->uart_clk);
		
		/* Enable GPIO clock */
	  RCM_EnableAPB2PeriphClock(uart_cfg->tx_gpio.clk);
		RCM_EnableAPB2PeriphClock(uart_cfg->rx_gpio.clk);
		
		/* Configure USART Tx as alternate function push-pull */
    g_configStruct.mode 	= uart_cfg->tx_gpio.mode;
    g_configStruct.pin 	  = uart_cfg->tx_gpio.pin;
    g_configStruct.speed  = uart_cfg->tx_gpio.speed;
    GPIO_Config(uart_cfg->tx_gpio.port, &g_configStruct);

    /* Configure USART Rx as input floating */
    g_configStruct.mode 	= uart_cfg->rx_gpio.mode;
    g_configStruct.pin 	  = uart_cfg->rx_gpio.pin;
//    g_configStruct.speed  = uart_cfg->rx_gpio.speed;
    GPIO_Config(uart_cfg->rx_gpio.port, &g_configStruct);
		
		/* USART configuration */
		u_configStruct.baudRate 		= uart_cfg->baudRate;
    u_configStruct.hardwareFlow = USART_HARDWARE_FLOW_NONE;
    u_configStruct.mode 				= uart_cfg->mode;
		u_configStruct.wordLength 	= USART_WORD_LEN_8B;
    u_configStruct.parity 			= USART_PARITY_NONE;
    u_configStruct.stopBits 		= USART_STOP_BIT_1;
    USART_Config(uart_cfg->uart, &u_configStruct);

    /* Enable USART */
    USART_Enable(uart_cfg->uart);
	}
}




/* TXBE 等待上限。正常情形下写 DR 后一个字节时间（115200 约 87us）就置位，
 * 这个上界给足余量；真卡住时报错返回而不是无限自旋 —— 自旋不喂狗 -> 外部
 * WDT 拉 NRST -> 重启后重走同一条路 -> 复位环。
 * 本函数是所有 printf 与所有 AT 发送的唯一通道。 */
#define UART_TXBE_WAIT_MAX   200000u

int32_t uart_send_data(USART_T *usart, void *pdata, uint32_t len)
{
	uint32_t i;
	uint32_t guard;
  if (len < 1) return -1;
  if (pdata == NULL) return -1;


	for( i= 0; i < len; i++)
	{
		/* send a byte of data to the serial port */
		USART_TxData(usart, *(uint8_t *)pdata++ );

		/* wait for the data to be send  */
		guard = UART_TXBE_WAIT_MAX;
		while((USART_ReadStatusFlag(usart, USART_FLAG_TXBE) == RESET) && (guard != 0u))
		{
			guard--;
		}
		if(guard == 0u)
		{
			return -1;              /* TX 卡死：报错，别无限自旋 */
		}
	}

  return 0;
}


/*!
 * @brief       运行时改波特率（串口下载提速；只在 USART1 下载会话内调用）
 *
 * 【必须先等 TC，不能只等 TXBE】TXBE=1 只表示发送数据寄存器空，最后几个
 * 字节还在移位寄存器里 —— 这时改 BR 会把在途字节的位宽切坏。设备切波特率
 * 前刚发完的就是控制字 "#RDY\r\n"，它一乱整个按页 ACK 协议立刻错位。
 * TC=1 才代表最后一个 bit 已经出线。
 *
 * 复用 SDK 的 USART_Config：它对 CTRL1/2/3 都是掩码改写（0xE9F3/0xCFFF/
 * 0xFCFF），UE/TE/RE 与中断配置都不受影响，运行中重配是安全的。
 * 帧格式固定 8N1（与 uart_cfg_list 一致）。 */
int32_t user_uart_set_baud(USART_T *usart, uint32_t baud)
{
	USART_Config_T cfg;
	uint32_t guard = UART_TXBE_WAIT_MAX;

	if((usart == NULL) || (baud == 0u))
	{
		return -1;
	}

	while((USART_ReadStatusFlag(usart, USART_FLAG_TXC) == RESET) && (guard != 0u))
	{
		guard--;
	}
	if(guard == 0u)
	{
		return -1;
	}

	cfg.baudRate	    = baud;
	cfg.hardwareFlow  = USART_HARDWARE_FLOW_NONE;
	cfg.mode		      = USART_MODE_TX_RX;
	cfg.wordLength	  = USART_WORD_LEN_8B;
	cfg.parity		    = USART_PARITY_NONE;
	cfg.stopBits	    = USART_STOP_BIT_1;
	USART_Config(usart, &cfg);

	return 0;
}



/*!
 * @brief       USART1_Interrupt
 *
 * @param       None
 *
 * @retval      None
 *
 */
void USART1_Isr(void)
{
  /* �쳣����ʱ���־��������ѭ�� */
  if(USART_ReadIntFlag(USART1, USART_INT_RXBNE))
  {
    (void)USART_RxData(USART1);
  }
  USART_ClearIntFlag(USART1, USART_INT_RXBNE);
}



