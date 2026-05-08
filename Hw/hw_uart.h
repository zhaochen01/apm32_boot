#ifndef _HW_UART_H
#define _HW_UART_H


#include "apm32e10x_gpio.h"
#include "apm32e10x_usart.h"
#include "apm32e10x_rcm.h"
#include "apm32e10x_dma.h"
#include "apm32e10x_misc.h"


//usart
/* Set Buff Size */
#define BufSize                  512


extern void user_uart_init(void);
extern void USART3_RX_DMA_Init(void);


extern uint32_t uart_send_data(USART_T *usart, void *pdata, uint32_t len);
extern uint32_t dma_send_data(USART_T *usart, void *pdata, uint32_t len);

#endif // _NETWORK_EG800K_H
