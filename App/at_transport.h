#ifndef _AT_TRANSPORT_H
#define _AT_TRANSPORT_H

#include "stdint.h"

/* AT transport error codes */
#define AT_ERR_OK         0
#define AT_ERR_TIMEOUT   (-1)
#define AT_ERR_NOMATCH   (-2)
#define AT_ERR_PARAM     (-3)
#define AT_ERR_FAULT     (-4)

/* max length of a single AT response line */
#define AT_LINE_MAX_LEN  256U

/* default command timeout when 0 is passed */
#define AT_CMD_DEFAULT_TIMEOUT  5000U

/* response matching mode */
typedef enum
{
  AT_MATCH_EXACT    = 0,  /* line must start with target (after trimming spaces) */
  AT_MATCH_CONTAINS = 1   /* line must contain target as substring */
} AT_MATCH_MODE_T;

/* UART rx diagnostics (filled by at_take_fault_snapshot) */
typedef struct
{
  uint32_t recv_len;
  uint32_t expect_len;
  uint32_t ovre_cnt;
  uint32_t fe_cnt;
  uint32_t ne_cnt;
} AT_RX_DIAG_T;

/* register callbacks with hw_uart3 and reset transport state */
void    at_transport_init(void);

/* send an AT command string (must include \r\n terminator) */
int32_t at_send(const char *cmd);

/* wait for a response line matching target within timeout_ms.
 * line_out / out_size（可传 NULL / 0）收下命中那一行 —— 它是从 ISR 队列里
 * 原子拷出来的快照，调用方直接解析它，不要自己去读队列。 */
int32_t at_wait(const char *target, AT_MATCH_MODE_T mode, uint32_t timeout_ms,
                char *line_out, uint16_t out_size);

/* flush pending rx, send command, then wait for response */
int32_t at_send_wait(const char *cmd, const char *target, AT_MATCH_MODE_T mode,
                     uint32_t timeout_ms, char *line_out, uint16_t out_size);

/* discard all buffered rx data and reset transient fault state */
void    at_flush_rx(void);

/* arm raw reception: if the next complete line is "CONNECT <len>",
 * the following <len> bytes stream into buf automatically (ISR context).
 * Must be called before sending AT+QFREAD. */
void    at_arm_raw_rx(uint8_t *buf, uint32_t buf_size);

/* cancel a pending raw reception */
void    at_disarm_raw_rx(void);

/* wait for the raw stream started by the armed CONNECT header */
int32_t at_wait_raw_done(uint32_t timeout_ms, uint32_t *actual_len);

/* take and clear the fault snapshot; returns 1 if a fault occurred */
uint8_t at_take_fault_snapshot(AT_RX_DIAG_T *diag);

#endif /* _AT_TRANSPORT_H */
