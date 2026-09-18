#ifndef _HW_UART3_H
#define _HW_UART3_H

#include "stdint.h"

/* line-mode buffer size for AT response reception */
#define HW_UART3_LINE_BUF_SIZE  512U

/* USART3 receive mode */
typedef enum
{
  UART3_MODE_LINE = 0,   /* accumulate bytes, deliver complete \r\n lines */
  UART3_MODE_RAW  = 1    /* stream bytes into caller-provided buffer */
} UART3_RX_MODE_T;

/* callbacks invoked from ISR context - keep them short and non-blocking */
typedef void (*hw_uart3_line_cb_t)(const uint8_t *line, uint16_t len);
typedef void (*hw_uart3_raw_done_cb_t)(uint32_t actual_len);
typedef void (*hw_uart3_fault_cb_t)(uint8_t ovre, uint8_t fe, uint8_t ne);

typedef struct
{
  hw_uart3_line_cb_t     on_line;      /* complete line received (len excludes \r\n) */
  hw_uart3_raw_done_cb_t on_raw_done;  /* raw stream received expect_len bytes */
  hw_uart3_fault_cb_t    on_fault;     /* UART hardware error detected */
} HW_UART3_CALLBACKS_T;

void     hw_uart3_init(const HW_UART3_CALLBACKS_T *callbacks);
void     hw_uart3_send_bytes(const uint8_t *data, uint32_t len);
void     hw_uart3_flush_rx(void);                      /* reset all rx state */
void     hw_uart3_start_raw_rx(uint8_t *buf, uint32_t buf_size, uint32_t expect_len);
void     hw_uart3_abort_raw_rx(void);                  /* force exit raw mode */
uint8_t  hw_uart3_take_fault_snapshot(uint32_t *recv_len, uint32_t *expect_len,
                                      uint32_t *ovre_cnt, uint32_t *fe_cnt,
                                      uint32_t *ne_cnt);

#endif /* _HW_UART3_H */
