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


/* 返回 int32_t 而不是 uint32_t：失败码是 -1，无符号类型下会变成 0xFFFFFFFF，
 * 调用方写 `if (ret < 0)` 就静默失效（与 ARMCLANG 无符号 char 哨兵同类坑）。 */
extern int32_t uart_send_data(USART_T *usart, void *pdata, uint32_t len);
extern uint32_t dma_send_data(USART_T *usart, void *pdata, uint32_t len);

/* 运行时改波特率（串口下载提速用；只在 USART1 的下载会话内调用）。
 * 返回 0 成功、-1 失败（TX 未归位，见实现里的 TC 等待）。
 * 帧格式固定 8N1 —— 与 uart_cfg_list 里两个口的配置一致（不读回原值，
 * 免得为省几十字节代码引入"读到的格式和实际不一致"的隐患）。 */
extern int32_t user_uart_set_baud(USART_T *usart, uint32_t baud);

#endif // _NETWORK_EG800K_H
