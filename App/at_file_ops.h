#ifndef _AT_FILE_OPS_H
#define _AT_FILE_OPS_H

#include "stdint.h"

/* file operation error codes */
#define AT_FILE_ERR_OK            0
#define AT_FILE_ERR_OPEN         (-1)
#define AT_FILE_ERR_NOTFOUND     (-2)
#define AT_FILE_ERR_SIZE         (-3)
#define AT_FILE_ERR_READ_FAULT   (-4)
#define AT_FILE_ERR_READ_TIMEOUT (-5)
#define AT_FILE_ERR_READ_SIZE    (-6)
#define AT_FILE_ERR_PARAM        (-7)
#define AT_FILE_ERR_CLOSE        (-8)
#define AT_FILE_ERR_DELETE       (-9)

/* timeouts (ms) */
#define AT_FILE_OPEN_TIMEOUT_MS    1000U
#define AT_FILE_FLST_TIMEOUT_MS    1000U
#define AT_FILE_READ_CMD_TIMEOUT_MS 1000U
#define AT_FILE_CLOSE_TIMEOUT_MS   300U
#define AT_FILE_DELETE_TIMEOUT_MS  3000U

/* context for one open file on the module UFS */
typedef struct
{
  const char *filename;   /* e.g. "UFS:iot_update.bin" */
  int32_t     fd;         /* file descriptor from +QFOPEN, -1 if closed */
  uint32_t    file_size;  /* cached from +QFLST */
  uint8_t     is_open;
} AT_FILE_CTX_T;

/* open file read-only (AT+QFOPEN), store fd into ctx */
int32_t at_file_open(AT_FILE_CTX_T *ctx, const char *filename);

/* query file size (AT+QFLST), store into ctx->file_size */
int32_t at_file_get_size(AT_FILE_CTX_T *ctx);

/* read one chunk (AT+QFREAD): raw data lands in buf, actual length in
 * *actual_len. timeout_ms bounds the whole read. */
int32_t at_file_read(AT_FILE_CTX_T *ctx, uint8_t *buf, uint32_t buf_size,
                     uint32_t req_len, uint32_t *actual_len, uint32_t timeout_ms);

/* close file (AT+QFCLOSE), invalidate fd */
int32_t at_file_close(AT_FILE_CTX_T *ctx);

/* delete a file on the module UFS (AT+QFDEL).
 * 安装成功后清理固件用；对仍处于打开状态的文件会失败，调用方自行忽略即可。 */
int32_t at_file_delete(const char *filename);

#endif /* _AT_FILE_OPS_H */
