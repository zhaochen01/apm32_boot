

#include "main.h"
#include "hw_uart.h"


/* USART1 TX Buf */
//uint8_t DMA_USART1_TxBuf[BufSize];

/* USART2 RX Buf */
uint8_t DMA_USART3_RxBuf[BufSize];



DMA_Config_T dmaConfig_tx;
DMA_Config_T dmaConfig_rx;



				

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
	
//	{ USART2, 9600, USART_MODE_TX_RX, RCM_APB1_PERIPH_USART2,\
//		RCM_APB2_PERIPH_GPIOA, GPIOA, GPIO_PIN_2, GPIO_SPEED_50MHz, GPIO_MODE_AF_PP,\
//		RCM_APB2_PERIPH_GPIOA, GPIOA, GPIO_PIN_3, GPIO_SPEED_50MHz, GPIO_MODE_IN_FLOATING
//	},
	
	{ USART3, 115200, USART_MODE_TX_RX, RCM_APB1_PERIPH_USART3,\
		RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_10, GPIO_SPEED_50MHz, GPIO_MODE_AF_PP,\
		RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_11, GPIO_SPEED_50MHz, GPIO_MODE_IN_FLOATING
	},
		
//	{ UART4, 115200, USART_MODE_TX_RX, RCM_APB1_PERIPH_UART4,\
//		RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_10, GPIO_SPEED_50MHz, GPIO_MODE_AF_PP,\
//		RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_11, GPIO_SPEED_50MHz, GPIO_MODE_IN_FLOATING
//	},
//		
//	{ UART5, 115200, USART_MODE_TX_RX, RCM_APB1_PERIPH_UART5,\
//    RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_12, GPIO_SPEED_50MHz, GPIO_MODE_AF_PP,\
//		RCM_APB2_PERIPH_GPIOD, GPIOD, GPIO_PIN_2,  GPIO_SPEED_50MHz, GPIO_MODE_IN_FLOATING
//	},
	
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


void USART3_RX_DMA_Init(void)
{
    /* DMA config */
    dmaConfig_rx.peripheralBaseAddr = (USART3_BASE+0x04);
    dmaConfig_rx.memoryBaseAddr = (uint32_t)DMA_USART3_RxBuf;
    dmaConfig_rx.dir = DMA_DIR_PERIPHERAL_SRC;
    dmaConfig_rx.bufferSize = BufSize;
    dmaConfig_rx.peripheralInc = DMA_PERIPHERAL_INC_DISABLE;
    dmaConfig_rx.memoryInc = DMA_MEMORY_INC_ENABLE;
    dmaConfig_rx.peripheralDataSize = DMA_PERIPHERAL_DATA_SIZE_BYTE;
    dmaConfig_rx.memoryDataSize = DMA_MEMORY_DATA_SIZE_BYTE;
    dmaConfig_rx.loopMode = DMA_MODE_NORMAL;
    dmaConfig_rx.priority = DMA_PRIORITY_MEDIUM;
    dmaConfig_rx.M2M = DMA_M2MEN_DISABLE;

    /* Enable DMA channel */
    DMA_Config(DMA1_Channel3, &dmaConfig_rx);

    /* Enable DMA */
    DMA_Enable(DMA1_Channel3);
}


uint32_t uart_send_data(USART_T *usart, void *pdata, uint32_t len)
{
	uint32_t i;
  if (len < 1) return -1;
  if (pdata == NULL) return -1;


	for( i= 0; i < len; i++)
	{
		/* send a byte of data to the serial port */
		USART_TxData(usart, *(uint8_t *)pdata++ );
	
		/* wait for the data to be send  */
		while (USART_ReadStatusFlag(usart, USART_FLAG_TXBE) == RESET);
	}

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
  
  
}



