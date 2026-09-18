/*
 * serial_dl.c —— 串口下载的字节层（详见 serial_dl.h）
 *
 * 【铁律】本文件全程只用 uart_send_data() 发静态 ASCII，不链 stdio：
 * 收页循环里混进一个 printf 字节，整包就废。工具侧"ACK 不是 0x06 就
 * 报错退出"，一旦污染立刻可见，不会静默装进一段坏固件。
 *
 * 【铁律】收页循环里禁止 Delay_ms() —— 它是 SysTick 计数忙等
 * （bsp_delay.c:73），关中断时不递减。超时一律用 Delay_GetTick() 的
 * 32 位差（自带回绕安全）。
 */

#include "main.h"
#include "hw_uart.h"
#include "bsp_delay.h"
#include "serial_dl.h"
#include "iap.h"          /* APP_MAX_SIZE */

#define DL_USART   USART1

/* 接收等待期间的喂狗节流间隔。不能每轮都喂：轮询循环是 MHz 级的，
 * PB0 按 MHz 翻转对外部看门狗芯片不是有效喂狗（正常节流是 2Hz）。
 * 100ms 相对约 1.8s 的超时余量充足。 */
#define DL_FEED_MS   100u

/* 排空时最多丢多少字节。PC 若在持续灌数据，"等到没有数据"永远不成立，
 * 没有这个上界就是死循环。 */
#define DL_FLUSH_MAX   4096u

void serial_dl_flush(void)
{
  uint32_t guard = 0u;

  while((USART_ReadStatusFlag(DL_USART, USART_FLAG_RXBNE) != RESET) &&
        (guard < DL_FLUSH_MAX))
  {
    (void)USART_RxData(DL_USART);
    guard++;
  }
}

int32_t serial_dl_getc(void)
{
  if(USART_ReadStatusFlag(DL_USART, USART_FLAG_RXBNE) == RESET) return -1;
  return (int32_t)(uint8_t)USART_RxData(DL_USART);
}

int32_t serial_dl_send(const char *s)
{
  uint32_t n = 0u;

  if(s == NULL) return -1;
  while(s[n] != '\0') n++;
  if(n == 0u) return 0;

  return uart_send_data(DL_USART, (void *)s, n);
}

int32_t serial_dl_send_byte(uint8_t b)
{
  return uart_send_data(DL_USART, &b, 1u);
}

/* 按 DL_FEED_MS 节流喂狗。last 由调用方持有（每个等待循环一个）。 */
static void serial_dl_feed_soft(uint32_t *last)
{
  uint32_t now = Delay_GetTick();

  if((uint32_t)(now - *last) >= DL_FEED_MS)
  {
    *last = now;
    WDT_FEED();
  }
}

int32_t serial_dl_recv(uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
  uint32_t got  = 0u;
  uint32_t start;
  uint32_t last_feed;
  int32_t  c;

  if((buf == NULL) || (len == 0u)) return -1;

  start     = Delay_GetTick();
  last_feed = start;

  while(got < len)
  {
    c = serial_dl_getc();

    if(c >= 0)
    {
      buf[got] = (uint8_t)c;
      got++;
      continue;
    }

    if((uint32_t)(Delay_GetTick() - start) >= timeout_ms) return -1;
    serial_dl_feed_soft(&last_feed);
  }

  return (int32_t)got;
}

static int32_t serial_dl_hexval(uint8_t ch)
{
  if((ch >= '0') && (ch <= '9')) return (int32_t)(ch - '0');
  if((ch >= 'a') && (ch <= 'f')) return (int32_t)(ch - 'a') + 10;
  if((ch >= 'A') && (ch <= 'F')) return (int32_t)(ch - 'A') + 10;
  return -1;
}

/*
 * 请求行状态机。
 *
 *   0: 找 '#'      1: 'D'      2: 'L'      3: ' '
 *   4: size（十进制，≤8 位）   5: crc32（十六进制，≤8 位）
 *
 * 任何不匹配都退回 0 重新找 '#' —— 于是"0x55 0xAA 魔数残留 / 上次的
 * 半行 / 操作员乱敲的字符"都只是被跳过，不需要任何转义或握手。
 * 唯一能返回 1 的路径是收到 '\r' 或 '\n' 且两个字段都非空。
 */
int32_t serial_dl_wait_request(uint32_t timeout_ms, uint32_t *size, uint32_t *crc)
{
  uint32_t start;
  uint32_t last_feed;
  uint32_t sz = 0u, ck = 0u;
  uint32_t sz_n = 0u, ck_n = 0u;
  int32_t  c, hv;
  uint8_t  ch;
  uint8_t  st = 0u;

  if((size == NULL) || (crc == NULL)) return 0;

  start     = Delay_GetTick();
  last_feed = start;

  for(;;)
  {
    c = serial_dl_getc();

    if(c < 0)
    {
      if((timeout_ms == 0u) ||
         ((uint32_t)(Delay_GetTick() - start) >= timeout_ms))
      {
        return 0;
      }
      serial_dl_feed_soft(&last_feed);
      continue;
    }

    ch = (uint8_t)c;

    switch(st)
    {
    case 0u:
      if(ch == '#') st = 1u;
      break;

    case 1u:
      if(ch == 'D')           st = 2u;
      else if(ch == '#')      st = 1u;
      else                    st = 0u;
      break;

    case 2u:
      if(ch == 'L')           st = 3u;
      else if(ch == '#')      st = 1u;
      else                    st = 0u;
      break;

    case 3u:
      if(ch == ' ')
      {
        st = 4u; sz = 0u; sz_n = 0u;
      }
      else if(ch == '#')      st = 1u;
      else                    st = 0u;
      break;

    case 4u:
      if((ch >= '0') && (ch <= '9'))
      {
        sz = (sz * 10u) + (uint32_t)(ch - '0');
        sz_n++;
        /* 越界即弃，而不是留给上层去拒绝：让 PC 收到 RDY 之后再报
         * "装不下"，它已经把整份固件读进内存、也停了重发定时器。 */
        if((sz_n > 8u) || (sz > (uint32_t)APP_MAX_SIZE)) st = 0u;
      }
      else if((ch == ' ') && (sz_n > 0u) && (sz > 0u))
      {
        st = 5u; ck = 0u; ck_n = 0u;
      }
      else if(ch == '#')      st = 1u;
      else                    st = 0u;
      break;

    case 5u:
      hv = serial_dl_hexval(ch);
      if(hv >= 0)
      {
        ck = (ck << 4) | (uint32_t)hv;
        ck_n++;
        if(ck_n > 8u) st = 0u;
      }
      else if(((ch == '\r') || (ch == '\n')) && (ck_n > 0u))
      {
        *size = sz;
        *crc  = ck;
        return 1;
      }
      else if(ch == '#')      st = 1u;
      else                    st = 0u;
      break;

    default:
      st = 0u;
      break;
    }
  }
}

/*
 * 读并清零串口下载请求标志。
 *   1 = 有请求（已清零）  0 = 无请求  -1 = 有请求但清不掉
 *
 * 清不掉【必须】让调用方停车，不能"当作没有"继续往下走：
 * iap_init 在标志存在时会走正常启动分支 -> RESUME_APP -> 复位 -> 又读到
 * 标志（还在）-> 又是 RESUME_APP -> 无限复位环。
 */
int32_t serial_dl_take_request(void)
{
  uint16_t v;

  /* 读之前就要开时钟：没有 BKP 外设的 APB1 时钟，数据寄存器根本访问不到。
   * 现实里引导区跳过来之前已经开过，但那是"跳转不复位外设"这个隐含前提，
   * 依赖它等于把正确性押在别人的实现细节上 —— 无条件开一次，零成本。
   * 清理写还要额外使能后备域写访问：BAKPR 数据寄存器在备份域复位后是
   * 写保护的，BAKPR_ConfigBackupRegister() 自己不开。
   * 两个时钟都要开 —— 只开 PMU 没有 BKP 时钟，写同样不落地
   * （2026-09-18 引导区实测踩过）。 */
  RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_PMU | RCM_APB1_PERIPH_BAKR);
  PMU_EnableBackupAccess();

  v = BAKPR_ReadBackupRegister(BOOT_SERIAL_REQ_REG);
  if(v != (uint16_t)BOOT_SERIAL_REQ_MAGIC) return 0;

  BAKPR_ConfigBackupRegister(BOOT_SERIAL_REQ_REG, 0u);

  return (BAKPR_ReadBackupRegister(BOOT_SERIAL_REQ_REG) == 0u) ? 1 : -1;
}
