/*!
 * @file        hw_uart3.c
 *
 * @brief       USART3 hardware abstraction for the 4G module link.
 *              Owns the RX ISR state machine: line mode (AT responses)
 *              and raw mode (QFREAD binary stream). All ISR state is
 *              file-static; upper layers interact through callbacks.
 *
 *              Faults (OVRE/FE/NE) record diagnostics, reset rx state
 *              back to line mode and notify the registered callback.
 */

#include "main.h"
#include "hw_uart3.h"
#include "hw_uart.h"

#include <string.h>

/* ISR-internal state - not visible outside this file */
typedef struct
{
  uint8_t   mode;              /* UART3_RX_MODE_T */
  uint16_t  line_idx;          /* write cursor into line_buf */
  uint8_t   line_buf[HW_UART3_LINE_BUF_SIZE];

  uint8_t  *raw_buf;           /* caller-provided raw stream buffer */
  uint32_t  raw_buf_size;
  uint32_t  expect_len;        /* raw stream target length */
  uint32_t  raw_rx_count;      /* raw bytes received so far */

  uint32_t  ovre_cnt;          /* cumulative error counters */
  uint32_t  fe_cnt;
  uint32_t  ne_cnt;
  uint8_t   fault_flag;        /* set on fault, cleared by take_fault_snapshot */
  uint32_t  fault_recv_len;
  uint32_t  fault_expect_len;
} UART3_ISR_STATE_T;

static UART3_ISR_STATE_T s_uart3 = {0};
static HW_UART3_CALLBACKS_T s_cb = {0};

static void uart3_mark_fault(void)
{
  s_uart3.fault_flag = 1U;
  s_uart3.fault_recv_len = s_uart3.raw_rx_count;
  s_uart3.fault_expect_len = s_uart3.expect_len;

  /* reset back to idle line mode */
  s_uart3.mode = UART3_MODE_LINE;
  s_uart3.line_idx = 0U;
  s_uart3.raw_buf = NULL;
  s_uart3.raw_buf_size = 0U;
  s_uart3.expect_len = 0U;
  s_uart3.raw_rx_count = 0U;
}

void hw_uart3_init(const HW_UART3_CALLBACKS_T *callbacks)
{
  if(callbacks != NULL)
  {
    s_cb = *callbacks;
  }
  else
  {
    memset(&s_cb, 0, sizeof(s_cb));
  }

  memset(&s_uart3, 0, sizeof(s_uart3));
  s_uart3.mode = UART3_MODE_LINE;

  /* note: USART3 peripheral itself is configured by user_uart_init() */
  USART_ClearIntFlag(USART3, USART_INT_RXBNE);
  USART_EnableInterrupt(USART3, USART_INT_RXBNE);
  USART_EnableInterrupt(USART3, USART_INT_ERR);

  NVIC_EnableIRQRequest(USART3_IRQn, 1, 0);
}

void hw_uart3_send_bytes(const uint8_t *data, uint32_t len)
{
  (void)uart_send_data(USART3, (void *)data, len);
}

void hw_uart3_flush_rx(void)
{
  __disable_irq();
  s_uart3.mode = UART3_MODE_LINE;
  s_uart3.line_idx = 0U;
  s_uart3.raw_buf = NULL;
  s_uart3.raw_buf_size = 0U;
  s_uart3.expect_len = 0U;
  s_uart3.raw_rx_count = 0U;
  s_uart3.fault_flag = 0U;
  s_uart3.fault_recv_len = 0U;
  s_uart3.fault_expect_len = 0U;
  __enable_irq();
}

void hw_uart3_start_raw_rx(uint8_t *buf, uint32_t buf_size, uint32_t expect_len)
{
  __disable_irq();
  if((buf != NULL) && (buf_size > 0U) && (expect_len > 0U) && (expect_len <= buf_size))
  {
    s_uart3.raw_buf = buf;
    s_uart3.raw_buf_size = buf_size;
    s_uart3.expect_len = expect_len;
    s_uart3.raw_rx_count = 0U;
    s_uart3.mode = UART3_MODE_RAW;
  }
  else
  {
    /* invalid request: record fault, stay in line mode */
    uart3_mark_fault();
  }
  __enable_irq();
}

void hw_uart3_abort_raw_rx(void)
{
  __disable_irq();
  s_uart3.mode = UART3_MODE_LINE;
  s_uart3.raw_buf = NULL;
  s_uart3.raw_buf_size = 0U;
  s_uart3.expect_len = 0U;
  s_uart3.raw_rx_count = 0U;
  __enable_irq();
}

uint8_t hw_uart3_take_fault_snapshot(uint32_t *recv_len, uint32_t *expect_len,
                                     uint32_t *ovre_cnt, uint32_t *fe_cnt,
                                     uint32_t *ne_cnt)
{
  uint8_t fault_flag = 0U;

  __disable_irq();
  fault_flag = s_uart3.fault_flag;
  if(recv_len != NULL)
  {
    *recv_len = s_uart3.fault_recv_len;
  }
  if(expect_len != NULL)
  {
    *expect_len = s_uart3.fault_expect_len;
  }
  if(ovre_cnt != NULL)
  {
    *ovre_cnt = s_uart3.ovre_cnt;
  }
  if(fe_cnt != NULL)
  {
    *fe_cnt = s_uart3.fe_cnt;
  }
  if(ne_cnt != NULL)
  {
    *ne_cnt = s_uart3.ne_cnt;
  }
  s_uart3.fault_flag = 0U;
  __enable_irq();

  return fault_flag;
}

/*!
 * @brief       USART3 interrupt handler - 4G module UART.
 *              Line mode: accumulates bytes until \r\n, delivers the
 *              complete line (without terminator) via on_line callback.
 *              Raw mode: streams bytes into the caller-provided buffer,
 *              delivers actual length via on_raw_done callback.
 */
void USART3_Isr(void)
{
  uint8_t dat = 0U;
  uint8_t ovre_flag = USART_ReadStatusFlag(USART3, USART_FLAG_OVRE);
  uint8_t fe_flag = USART_ReadStatusFlag(USART3, USART_FLAG_FE);
  uint8_t ne_flag = USART_ReadStatusFlag(USART3, USART_FLAG_NE);

  if((ovre_flag != RESET) || (fe_flag != RESET) || (ne_flag != RESET))
  {
    /* read status and data registers to clear the hardware flags */
    volatile uint32_t usart_sts = USART3->STS;
    volatile uint32_t usart_dat = USART3->DATA;
    (void)usart_sts;
    (void)usart_dat;

    if(ovre_flag != RESET)
    {
      s_uart3.ovre_cnt++;
    }
    if(fe_flag != RESET)
    {
      s_uart3.fe_cnt++;
    }
    if(ne_flag != RESET)
    {
      s_uart3.ne_cnt++;
    }

    uart3_mark_fault();
    if(s_cb.on_fault != NULL)
    {
      s_cb.on_fault(ovre_flag, fe_flag, ne_flag);
    }
    return;
  }

  if(USART_ReadIntFlag(USART3, USART_INT_RXBNE))
  {
    dat = (uint8_t)USART_RxData(USART3);

    if(s_uart3.mode == UART3_MODE_RAW)
    {
      /* raw binary stream: write directly into caller buffer */
      if((s_uart3.expect_len == 0U) || (s_uart3.expect_len > s_uart3.raw_buf_size))
      {
        uart3_mark_fault();
        return;
      }

      if(s_uart3.raw_rx_count < s_uart3.expect_len)
      {
        s_uart3.raw_buf[s_uart3.raw_rx_count++] = dat;
      }
      else
      {
        /* byte beyond expect_len: desync, record fault */
        uart3_mark_fault();
        return;
      }

      if(s_uart3.raw_rx_count >= s_uart3.expect_len)
      {
        uint32_t actual = s_uart3.expect_len;
        s_uart3.mode = UART3_MODE_LINE;
        s_uart3.raw_rx_count = 0U;
        s_uart3.expect_len = 0U;
        if(s_cb.on_raw_done != NULL)
        {
          s_cb.on_raw_done(actual);
        }
      }
    }
    else
    {
      /* line mode: accumulate AT response bytes */
      s_uart3.line_idx %= HW_UART3_LINE_BUF_SIZE;
      s_uart3.line_buf[s_uart3.line_idx++] = dat;

      if((s_uart3.line_idx >= 2U) &&
         (s_uart3.line_buf[s_uart3.line_idx - 2U] == '\r') &&
         (s_uart3.line_buf[s_uart3.line_idx - 1U] == '\n'))
      {
        uint16_t line_len = (uint16_t)(s_uart3.line_idx - 2U);

        /* null-terminate within bounds (line_idx max 512 -> index 510) */
        s_uart3.line_buf[line_len] = '\0';

        if(s_cb.on_line != NULL)
        {
          s_cb.on_line(s_uart3.line_buf, line_len);
        }
        s_uart3.line_idx = 0U;
      }
      else if(((dat == '\r') || (dat == '\n')) && (s_uart3.line_idx == 1U))
      {
        /* lone \r or \n at start of line: discard */
        s_uart3.line_idx = 0U;
      }
      else if(dat == '>')
      {
        /* Quectel prompt character: reset line */
        s_uart3.line_idx = 0U;
      }
    }
  }
}
