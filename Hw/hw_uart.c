
#include "main.h"
#include "hw_uart.h"

/* 本工程只有两个口：USART1（调试 + 串口刷机）、USART3（4G 模块），
 * 配置固定 8N1 / 无流控 / 115200。原来那张“通用多口表 + 循环”是为 N 个口
 * 准备的，实际只有 2 个，而且最后还得靠 if 区分 APB1/APB2 时钟 —— 直写更短。
 * 时钟：APB2 = 72MHz（USART1），APB1 = 36MHz（USART3）。
 *
 * 寄存器直写替掉 SDK 的 USART_Config（152B）+ RCM_ReadPCLKFreq（156B）：
 * 后者只是为了在运行时问出 PCLK，而这里的 PCLK 是编译期常量。 */
#define UART_PCLK2_HZ   72000000u
#define UART_PCLK1_HZ   36000000u

/* 8N1 + TX + RX + UEN。CTRL1 位：RXEN=bit2、TXEN=bit3、UEN=bit13；
 * STS 位：TC=bit6、TXBE=bit7。 */
#define UART_CTRL1_8N1_TX_RX    ((1u << 13) | (1u << 3) | (1u << 2))
#define UART_STS_TC             (1u << 6)
#define UART_STS_TXBE           (1u << 7)

/* 与 SDK USART_Config 逐字相同的算式（含截断方式），保证换实现后波特率
 * 一个 LSB 都不差。div = 100 × USARTDIV，高 12 位是整数、低 4 位是 1/16。 */
static uint32_t uart_br(uint32_t pclk, uint32_t baud)
{
  uint32_t div = (25u * pclk) / (4u * baud);
  uint32_t br  = (div / 100u) << 4;
  uint32_t frac = div - (100u * (br >> 4));

  return br | (((frac * 16u) + 50u) / 100u & 0x0Fu);
}

static void uart_hw_init(USART_T *u, uint32_t pclk, uint32_t baud)
{
  u->CTRL1 = UART_CTRL1_8N1_TX_RX;
  u->CTRL2 = 0u;                     /* 1 个停止位 */
  u->CTRL3 = 0u;                     /* 无硬件流控、无 DMA */
  u->BR    = uart_br(pclk, baud);
}

void user_uart_init(void)
{
  static const HW_GPIO_INIT_T u1_tx = { RCM_APB2_PERIPH_GPIOA, GPIOA, GPIO_PIN_9,
                                        GPIO_SPEED_50MHz, GPIO_MODE_AF_PP };
  static const HW_GPIO_INIT_T u1_rx = { RCM_APB2_PERIPH_GPIOA, GPIOA, GPIO_PIN_10,
                                        GPIO_SPEED_50MHz, GPIO_MODE_IN_FLOATING };
  static const HW_GPIO_INIT_T u3_tx = { RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_10,
                                        GPIO_SPEED_50MHz, GPIO_MODE_AF_PP };
  static const HW_GPIO_INIT_T u3_rx = { RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_11,
                                        GPIO_SPEED_50MHz, GPIO_MODE_IN_PU };

  /* 引脚（同时开 GPIO 时钟） */
  hw_gpio_init(u1_tx);
  hw_gpio_init(u1_rx);
  hw_gpio_init(u3_tx);
  hw_gpio_init(u3_rx);

  RCM_EnableAPB2PeriphClock(RCM_APB2_PERIPH_USART1);
  RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_USART3);

  uart_hw_init(USART1, UART_PCLK2_HZ, 115200u);
  uart_hw_init(USART3, UART_PCLK1_HZ, 115200u);
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
		usart->DATA = *(uint8_t *)pdata++;

		/* wait for the data to be send  */
		guard = UART_TXBE_WAIT_MAX;
		while(((usart->STS & UART_STS_TXBE) == 0u) && (guard != 0u))
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
 * 【只写 BR】帧格式（CTRL1/2/3）没变，重写它们只会带来风险 —— 尤其 CTRL3
 * 里可能已经开着 DMA 接收（serial_dl 的收页路径）。旧实现走 USART_Config，
 * 对三个控制寄存器做掩码改写，虽然也安全，但没必要。
 */
int32_t user_uart_set_baud(USART_T *usart, uint32_t baud)
{
	uint32_t guard = UART_TXBE_WAIT_MAX;
	uint32_t pclk;

	if((usart == NULL) || (baud == 0u))
	{
		return -1;
	}

	while(((usart->STS & UART_STS_TC) == 0u) && (guard != 0u))
	{
		guard--;
	}
	if(guard == 0u)
	{
		return -1;
	}

	pclk = (usart == USART1) ? UART_PCLK2_HZ : UART_PCLK1_HZ;
	usart->BR = uart_br(pclk, baud);

	return 0;
}
