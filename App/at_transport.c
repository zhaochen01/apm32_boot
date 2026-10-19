/*!
 * @file        at_transport.c
 *
 * @brief       AT command/response transport engine for the 4G module.
 *              Producer-consumer line queue: the USART3 ISR callback
 *              (producer) pushes complete lines, at_wait() (consumer)
 *              drains and matches them. This removes the race of the
 *              legacy code which read the ISR buffer directly.
 *
 *              Raw mode: at_arm_raw_rx() registers a sink buffer before
 *              sending AT+QFREAD. When the ISR callback sees a complete
 *              "CONNECT <len>" line, it switches hw_uart3 to raw mode
 *              immediately (still in ISR context) so no raw byte can be
 *              lost between the header and the stream.
 */

#include "main.h"
#include "at_transport.h"
#include "hw_uart3.h"
#include "bsp_delay.h"

#include <string.h>

#define AT_LINE_QUEUE_SIZE   8U
#define AT_CONNECT_PREFIX    "CONNECT "

/* one queued AT response line */
typedef struct
{
  char     data[AT_LINE_MAX_LEN];
  uint16_t len;
} AT_LINE_ENTRY_T;

static AT_LINE_ENTRY_T s_line_queue[AT_LINE_QUEUE_SIZE];
static volatile uint8_t s_queue_head = 0U;   /* written by ISR callback */
static uint8_t          s_queue_tail = 0U;   /* written by at_wait */
static volatile uint8_t s_queue_count = 0U;

/* raw reception state */
static volatile uint8_t  s_raw_armed = 0U;
static volatile uint8_t  s_raw_done = 0U;
static volatile uint32_t s_raw_actual = 0U;
static volatile uint8_t  s_raw_fault = 0U;   /* CONNECT parse/length error */
static uint8_t          *s_raw_buf = NULL;   /* written under irq disable */
static uint32_t          s_raw_buf_size = 0U;

/* fault state accumulated from the on_fault callback */
static volatile uint8_t  s_fault_flag = 0U;
static volatile uint32_t s_fault_ovre_cnt = 0U;
static volatile uint32_t s_fault_fe_cnt = 0U;
static volatile uint32_t s_fault_ne_cnt = 0U;

static int32_t parse_connect_len(const uint8_t *line, uint16_t len);

/* ---------------- ISR callbacks (producer side) ---------------- */

static void at_on_line(const uint8_t *line, uint16_t len)
{
  if((len < AT_LINE_MAX_LEN) && (s_queue_count < AT_LINE_QUEUE_SIZE))
  {
    AT_LINE_ENTRY_T *entry = &s_line_queue[s_queue_head];
    memcpy(entry->data, line, len);
    entry->data[len] = '\0';
    entry->len = len;
    s_queue_head = (s_queue_head + 1U) % AT_LINE_QUEUE_SIZE;
    s_queue_count++;
  }

  /* armed: a "CONNECT <len>" header starts the raw stream right away */
  if((s_raw_armed != 0U) && (len >= strlen(AT_CONNECT_PREFIX)) &&
     (memcmp(line, AT_CONNECT_PREFIX, strlen(AT_CONNECT_PREFIX)) == 0))
  {
    int32_t dlen = parse_connect_len(line, len);
    if((dlen > 0) && ((uint32_t)dlen <= s_raw_buf_size))
    {
      s_raw_armed = 0U;
      hw_uart3_start_raw_rx(s_raw_buf, s_raw_buf_size, (uint32_t)dlen);
    }
    else
    {
      /* invalid CONNECT length: report fault */
      s_raw_armed = 0U;
      s_raw_fault = 1U;
    }
  }
}

static void at_on_raw_done(uint32_t actual_len)
{
  s_raw_actual = actual_len;
  s_raw_done = 1U;
}

static void at_on_fault(uint8_t ovre, uint8_t fe, uint8_t ne)
{
  s_fault_flag = 1U;
  if(ovre != 0U)
  {
    s_fault_ovre_cnt++;
  }
  if(fe != 0U)
  {
    s_fault_fe_cnt++;
  }
  if(ne != 0U)
  {
    s_fault_ne_cnt++;
  }
}

/* ---------------- helpers ---------------- */

static int32_t parse_connect_len(const uint8_t *line, uint16_t len)
{
  const char *p;
  uint32_t val = 0U;
  size_t prefix_len = strlen(AT_CONNECT_PREFIX);

  if((line == NULL) || (len < (uint16_t)prefix_len) ||
     (memcmp(line, AT_CONNECT_PREFIX, prefix_len) != 0))
  {
    return -1;
  }

  p = (const char *)line + prefix_len;
  if((*p < '0') || (*p > '9'))
  {
    return -1;
  }

  while((*p >= '0') && (*p <= '9'))
  {
    uint32_t prev = val;

    val = (val * 10U) + (uint32_t)(*p - '0');
    /* 溢出保护：模组返回的行不可信。超长数字串会让 val 回绕成一个
     * <= s_raw_buf_size 的小值，绕过调用点的上界检查，于是 raw 模式按
     * 错误的 expect_len 启动 -> 提前报 raw_done -> 截断的数据被当成
     * 一次成功读取交付。写法与 at_file_ops.c 的 parse_file_len 一致。 */
    if(val < prev)
    {
      return -1;
    }
    p++;
  }

  if(val == 0U)
  {
    return -1;
  }
  return (int32_t)val;
}

static int32_t match_line(const char *line, const char *target, AT_MATCH_MODE_T mode)
{
  const char *p = line;

  if((line == NULL) || (target == NULL))
  {
    return -1;
  }

  /* trim leading whitespace before comparing */
  while((*p == ' ') || (*p == '\t'))
  {
    p++;
  }

  if(mode == AT_MATCH_EXACT)
  {
    return (strncmp(p, target, strlen(target)) == 0) ? 0 : -1;
  }
  if(mode == AT_MATCH_CONTAINS)
  {
    return (strstr(p, target) != NULL) ? 0 : -1;
  }
  return -1;
}

/* ---------------- public API ---------------- */

void at_transport_init(void)
{
  HW_UART3_CALLBACKS_T cb;

  memset(&cb, 0, sizeof(cb));
  cb.on_line = at_on_line;
  cb.on_raw_done = at_on_raw_done;
  cb.on_fault = at_on_fault;

  /* 关中断做整段重置：本函数会在升级失败后被 lte_modem_reset() 再次调用，
   * 那时模组仍上电、USART3 中断还开着、可能仍在吐数据。开着中断重置
   * head/tail/count，ISR 会在中途入队 -> 索引与计数不一致（孤儿槽 / 丢行）。
   * at_flush_rx() 一直是关中断写的，照抄。
   * hw_uart3_init() 内部同样要 memset(&s_uart3) 并开 NVIC，一并罩住；
   * CPU 级中断在末尾 __enable_irq() 才放行。 */
  __disable_irq();

  s_queue_head = 0U;
  s_queue_tail = 0U;
  s_queue_count = 0U;
  s_raw_armed = 0U;
  s_raw_done = 0U;
  s_raw_fault = 0U;
  s_raw_buf = NULL;
  s_raw_buf_size = 0U;
  s_fault_flag = 0U;
  s_fault_ovre_cnt = 0U;
  s_fault_fe_cnt = 0U;
  s_fault_ne_cnt = 0U;

  hw_uart3_init(&cb);

  __enable_irq();
}

int32_t at_send(const char *cmd)
{
  if(cmd == NULL)
  {
    return AT_ERR_PARAM;
  }
  hw_uart3_send_bytes((const uint8_t *)cmd, (uint32_t)strlen(cmd));
  return AT_ERR_OK;
}

int32_t at_wait(const char *target, AT_MATCH_MODE_T mode, uint32_t timeout_ms,
                char *line_out, uint16_t out_size)
{
  uint32_t start_tick;
  uint32_t timeout;

  if((mode != AT_MATCH_EXACT) && (mode != AT_MATCH_CONTAINS))
  {
    return AT_ERR_PARAM;
  }
  if(target == NULL)
  {
    return AT_ERR_PARAM;
  }

  timeout = (timeout_ms == 0U) ? AT_CMD_DEFAULT_TIMEOUT : timeout_ms;
  start_tick = Delay_GetTick();

  while(1)
  {
    /* drain the line queue (producer is the ISR callback) */
    __disable_irq();
    while(s_queue_count > 0U)
    {
      AT_LINE_ENTRY_T *entry = &s_line_queue[s_queue_tail];
      char line[AT_LINE_MAX_LEN];

      s_queue_tail = (s_queue_tail + 1U) % AT_LINE_QUEUE_SIZE;
      s_queue_count--;

      /* 必须在临界区内把行拷出来再开中断。
       * 队列满时 head == tail：本槽取走后 count 变 7，ISR 立刻就会往
       * queue[head] 写 —— 而那正是刚被取走的这一个。留在中断打开的窗口里
       * 读 entry->data，读到的会是被 memcpy 撕开的行。 */
      memcpy(line, entry->data, sizeof(line));
      __enable_irq();

      if(match_line(line, target, mode) == 0)
      {
        /* 交出去的必须是【本轮行】的副本 —— line 是临界区里拷出来的局部量，
         * 调用方拿到后可以随便解析，不会和 ISR 抢队列里的那个槽。 */
        if((line_out != NULL) && (out_size > 0U))
        {
          (void)strncpy(line_out, line, out_size - 1U);
          line_out[out_size - 1U] = '\0';
        }
        return AT_ERR_OK;
      }
      __disable_irq();
    }
    __enable_irq();

    if((Delay_GetTick() - start_tick) >= timeout)
    {
      return AT_ERR_TIMEOUT;
    }

    WDT_FEED();
    Delay_ms(1);
  }
}

int32_t at_send_wait(const char *cmd, const char *target,
                     AT_MATCH_MODE_T mode, uint32_t timeout_ms,
                     char *line_out, uint16_t out_size)
{
  int32_t ret;

  at_flush_rx();
  ret = at_send(cmd);
  if(ret != AT_ERR_OK)
  {
    return ret;
  }
  return at_wait(target, mode, timeout_ms, line_out, out_size);
}

void at_flush_rx(void)
{
  /* 顺序要紧：先清 UART 侧的行装配状态，再清队列。
   *
   * 原顺序是反的（清队列 -> 开中断 -> hw_uart3_flush_rx）。中间那段窗口里
   * ISR 可以把一行装配完并入队，而末尾那句只清 UART 侧、不清队列 —— 那一行
   * 就活过了这次 flush。后果：上一条命令迟到的 "OK" 被当成下一条命令的响应，
   * QFCLOSE / QFDEL 失败被当成功（fd 泄漏、坏固件文件残留未删）。
   *
   * 反过来写：hw_uart3_flush_rx() 自带临界区；它跑完后 ISR 即便入队，
   * 也会被紧随其后的队列清零抹掉。 */
  hw_uart3_flush_rx();

  __disable_irq();
  s_queue_head = 0U;
  s_queue_tail = 0U;
  s_queue_count = 0U;
  s_fault_flag = 0U;
  s_raw_armed = 0U;
  s_raw_done = 0U;
  s_raw_fault = 0U;
  s_raw_buf = NULL;
  s_raw_buf_size = 0U;
  __enable_irq();
}

void at_arm_raw_rx(uint8_t *buf, uint32_t buf_size)
{
  __disable_irq();
  s_raw_buf = buf;
  s_raw_buf_size = buf_size;
  s_raw_armed = 1U;
  s_raw_done = 0U;
  s_raw_fault = 0U;
  __enable_irq();
}

void at_disarm_raw_rx(void)
{
  __disable_irq();
  s_raw_armed = 0U;
  s_raw_done = 0U;
  s_raw_fault = 0U;
  s_raw_buf = NULL;
  s_raw_buf_size = 0U;
  __enable_irq();
  hw_uart3_abort_raw_rx();
}

int32_t at_wait_raw_done(uint32_t timeout_ms, uint32_t *actual_len)
{
  uint32_t start_tick = Delay_GetTick();

  while(1)
  {
    if((s_fault_flag != 0U) || (s_raw_fault != 0U))
    {
      return AT_ERR_FAULT;
    }
    if(s_raw_done != 0U)
    {
      if(actual_len != NULL)
      {
        *actual_len = s_raw_actual;
      }
      s_raw_done = 0U;
      return AT_ERR_OK;
    }
    if((Delay_GetTick() - start_tick) >= timeout_ms)
    {
      return AT_ERR_TIMEOUT;
    }

    WDT_FEED();
    Delay_ms(1);
  }
}

uint8_t at_take_fault_snapshot(AT_RX_DIAG_T *diag)
{
  uint8_t fault = 0U;
  uint32_t recv_len = 0U;
  uint32_t expect_len = 0U;
  uint32_t ovre_cnt = 0U;
  uint32_t fe_cnt = 0U;
  uint32_t ne_cnt = 0U;

  fault = hw_uart3_take_fault_snapshot(&recv_len, &expect_len,
                                       &ovre_cnt, &fe_cnt, &ne_cnt);

  __disable_irq();
  if(s_fault_flag != 0U)
  {
    fault = 1U;
  }
  s_fault_flag = 0U;
  if(s_raw_fault != 0U)
  {
    fault = 1U;
  }
  s_raw_fault = 0U;
  ovre_cnt += s_fault_ovre_cnt;
  fe_cnt += s_fault_fe_cnt;
  ne_cnt += s_fault_ne_cnt;
  s_fault_ovre_cnt = 0U;
  s_fault_fe_cnt = 0U;
  s_fault_ne_cnt = 0U;
  __enable_irq();

  if(diag != NULL)
  {
    diag->recv_len = recv_len;
    diag->expect_len = expect_len;
    diag->ovre_cnt = ovre_cnt;
    diag->fe_cnt = fe_cnt;
    diag->ne_cnt = ne_cnt;
  }
  return fault;
}
