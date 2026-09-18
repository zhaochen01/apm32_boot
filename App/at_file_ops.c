/*!
 * @file        at_file_ops.c
 *
 * @brief       File operations on the 4G module UFS via AT commands:
 *              AT+QFOPEN / AT+QFLST / AT+QFREAD / AT+QFCLOSE.
 *
 *              Response parsing uses at_get_last_line() (an atomic
 *              snapshot of the matched line) instead of reading the
 *              ISR buffer directly, removing the race of the legacy
 *              implementation.
 */

#include "main.h"
#include "at_file_ops.h"
#include "at_transport.h"

#include <string.h>

/* parse "..+QFOPEN: <fd>.." -> fd, or -1 */
static int32_t parse_qfopen_fd(const char *line)
{
  const char *p;

  if(line == NULL)
  {
    return -1;
  }

  p = strstr(line, "+QFOPEN:");
  if(p == NULL)
  {
    return -1;
  }
  p = strchr(p, ':');
  if(p == NULL)
  {
    return -1;
  }
  p++;
  while((*p == ' ') || (*p == '\t'))
  {
    p++;
  }
  if((*p < '0') || (*p > '9'))
  {
    printf("get qfopen fd error!\r\n");
    return -1;
  }

  int32_t fd = 0;
  while((*p >= '0') && (*p <= '9'))
  {
    fd = (fd * 10) + (*p - '0');
    p++;
  }
  printf("QFOPEN fd:%d\r\n", (int)fd);
  return fd;
}

/* parse "+QFLST: "<name>",<size>" -> size, or -1 */
static int32_t parse_file_len(const char *line)
{
  const char *p;
  uint32_t val = 0U;

  if(line == NULL)
  {
    return -1;
  }

  p = strrchr(line, ',');
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
    if(val < prev)
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

int32_t at_file_open(AT_FILE_CTX_T *ctx, const char *filename)
{
  char cmd[64];
  char resp[AT_LINE_MAX_LEN];
  int32_t fd;

  if((ctx == NULL) || (filename == NULL))
  {
    return AT_FILE_ERR_PARAM;
  }

  ctx->fd = -1;
  ctx->file_size = 0U;
  ctx->filename = filename;

  (void)snprintf(cmd, sizeof(cmd), "AT+QFOPEN=\"%s\",2\r\n", filename);
  if(at_send_wait(cmd, "+QFOPEN:", AT_MATCH_CONTAINS, AT_FILE_OPEN_TIMEOUT_MS) != AT_ERR_OK)
  {
    return AT_FILE_ERR_OPEN;
  }
  if(at_get_last_line(resp, sizeof(resp)) != AT_ERR_OK)
  {
    return AT_FILE_ERR_OPEN;
  }

  fd = parse_qfopen_fd(resp);
  if(fd < 0)
  {
    return AT_FILE_ERR_OPEN;
  }

  ctx->fd = fd;
  return AT_FILE_ERR_OK;
}

int32_t at_file_get_size(AT_FILE_CTX_T *ctx)
{
  char cmd[64];
  char resp[AT_LINE_MAX_LEN];
  int32_t size;

  if((ctx == NULL) || (ctx->filename == NULL))
  {
    return AT_FILE_ERR_PARAM;
  }

  (void)snprintf(cmd, sizeof(cmd), "AT+QFLST=\"%s\"\r\n", ctx->filename);
  if(at_send_wait(cmd, "+QFLST:", AT_MATCH_CONTAINS, AT_FILE_FLST_TIMEOUT_MS) != AT_ERR_OK)
  {
    return AT_FILE_ERR_SIZE;
  }
  if(at_get_last_line(resp, sizeof(resp)) != AT_ERR_OK)
  {
    return AT_FILE_ERR_SIZE;
  }

  size = parse_file_len(resp);
  if(size <= 0)
  {
    return AT_FILE_ERR_SIZE;
  }

  ctx->file_size = (uint32_t)size;
  return AT_FILE_ERR_OK;
}

int32_t at_file_read(AT_FILE_CTX_T *ctx, uint8_t *buf, uint32_t buf_size,
                     uint32_t req_len, uint32_t *actual_len, uint32_t timeout_ms)
{
  char cmd[40];
  int32_t ret;
  uint32_t raw_len = 0U;

  if((ctx == NULL) || (buf == NULL) || (actual_len == NULL))
  {
    return AT_FILE_ERR_PARAM;
  }
  if((ctx->fd < 0) || (req_len == 0U) || (req_len > buf_size))
  {
    return AT_FILE_ERR_PARAM;
  }

  at_flush_rx();
  at_arm_raw_rx(buf, buf_size);

  (void)snprintf(cmd, sizeof(cmd), "AT+QFREAD=%d,%lu\r\n", (int)ctx->fd, (unsigned long)req_len);
  if(at_send(cmd) != AT_ERR_OK)
  {
    at_disarm_raw_rx();
    return AT_FILE_ERR_READ_FAULT;
  }

  /* wait for the CONNECT header; the raw stream starts inside the
   * ISR callback, so no data is lost between header and stream */
  ret = at_wait("CONNECT ", AT_MATCH_CONTAINS, AT_FILE_READ_CMD_TIMEOUT_MS);
  if(ret != AT_ERR_OK)
  {
    at_disarm_raw_rx();
    return (ret == AT_ERR_TIMEOUT) ? AT_FILE_ERR_READ_TIMEOUT : AT_FILE_ERR_READ_FAULT;
  }

  /* wait for the raw stream completion */
  ret = at_wait_raw_done(timeout_ms, &raw_len);
  if(ret != AT_ERR_OK)
  {
    AT_RX_DIAG_T diag;

    memset(&diag, 0, sizeof(diag));
    (void)at_take_fault_snapshot(&diag);
    if(diag.recv_len + diag.expect_len + diag.ovre_cnt + diag.fe_cnt + diag.ne_cnt > 0U)
    {
      printf("QFREAD fault recv:%lu expect:%lu ovre:%lu fe:%lu ne:%lu\r\n",
             (unsigned long)diag.recv_len,
             (unsigned long)diag.expect_len,
             (unsigned long)diag.ovre_cnt,
             (unsigned long)diag.fe_cnt,
             (unsigned long)diag.ne_cnt);
    }
    at_disarm_raw_rx();
    return (ret == AT_ERR_TIMEOUT) ? AT_FILE_ERR_READ_TIMEOUT : AT_FILE_ERR_READ_FAULT;
  }

  if((raw_len == 0U) || (raw_len > req_len))
  {
    at_disarm_raw_rx();
    return AT_FILE_ERR_READ_SIZE;
  }

  *actual_len = raw_len;
  return AT_FILE_ERR_OK;
}

int32_t at_file_close(AT_FILE_CTX_T *ctx)
{
  char cmd[24];
  int32_t ret;

  if(ctx == NULL)
  {
    return AT_FILE_ERR_PARAM;
  }
  if(ctx->fd < 0)
  {
    return AT_FILE_ERR_OK;
  }

  (void)snprintf(cmd, sizeof(cmd), "AT+QFCLOSE=%d\r\n", (int)ctx->fd);
  ret = at_send_wait(cmd, "OK", AT_MATCH_EXACT, AT_FILE_CLOSE_TIMEOUT_MS);

  ctx->fd = -1;
  return (ret == AT_ERR_OK) ? AT_FILE_ERR_OK : AT_FILE_ERR_CLOSE;
}

int32_t at_file_delete(const char *filename)
{
  char cmd[64];
  int32_t ret;

  if(filename == NULL)
  {
    return AT_FILE_ERR_PARAM;
  }

  (void)snprintf(cmd, sizeof(cmd), "AT+QFDEL=\"%s\"\r\n", filename);
  ret = at_send_wait(cmd, "OK", AT_MATCH_EXACT, AT_FILE_DELETE_TIMEOUT_MS);

  return (ret == AT_ERR_OK) ? AT_FILE_ERR_OK : AT_FILE_ERR_DELETE;
}
