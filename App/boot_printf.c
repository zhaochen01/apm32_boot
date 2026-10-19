/*
 * boot_printf.c —— 极简格式化输出（替代 C 库 stdio，省约 1.1KB Flash）
 *
 * 全工程实际只用到这 6 种格式符（2026-10-19 用 grep 统计）：
 *     %s (7)   %d (20)   %X (14)   %02X (1)   %lu (38)   %08lX (7)
 * 只实现这些（外加 %c 与 %%，零成本），不支持精度/其他长度修饰符 ——
 * 遇到不认识的格式符会【原样输出】，便于一眼看出写错了，不会静默吞参数。
 *
 * 为什么自己写：库的 printf 引擎（printf8.o）编进镜像 1228B，加上它拖进来的
 * puts/stdout/init/handlers 共约 1.37KB，而本工程只用到上面 6 种最简单的
 * 格式。换成这个 ~250B 的实现后，全部日志一行不少、输出逐字节不变。
 *
 * 输出通道：printf 直接走 uart_send_data(USART1)（与旧 fputc 重定向同一条路），
 * 不再经过 FILE 指针；snprintf 写调用方缓冲并保证 '\0' 结尾。
 * 只允许在主流程调用（本工程 ISR 里没有日志）；缓冲区在栈上，不占静态 RAM。
 */

#include "main.h"
#include "hw_uart.h"

#include <stdarg.h>
#include <stdio.h>   /* 原型必须与库声明一致；不会因此链接库实现 */

#define PF_BUF_SIZE   160u   /* 最长一条日志实测 103 字节，留足余量 */

typedef struct
{
  char    *buf;
  uint32_t cap;   /* 含结尾 '\0' 的容量 */
  uint32_t len;   /* 已产生的字符数（可大于 cap-1，表示被截断） */
} PF_T;

/* noinline 不是性能提示，是体积要求：这四个助手若被内联进 pf_fmt 的各个
 * switch 分支，会被复制成好几份 —— 实测让 pf_fmt 从 ~250B 涨到 1856B
 * （与 Hw/hw_flash.c 里 fmc_wait 的经历完全相同）。 */
#define PF_NOINLINE   __attribute__((noinline))

PF_NOINLINE static void pf_ch(PF_T *o, char c)
{
  if((o->len + 1u) < o->cap)
  {
    o->buf[o->len] = c;
  }
  o->len++;
}

PF_NOINLINE static void pf_str(PF_T *o, const char *s)
{
  if(s == NULL) s = "(null)";
  while(*s != '\0')
  {
    pf_ch(o, *s++);
  }
}

PF_NOINLINE static void pf_u32(PF_T *o, uint32_t v)
{
  char     b[10];
  uint8_t  n = 0u;

#pragma clang loop unroll(disable)
  do
  {
    b[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  } while(v != 0u);

#pragma clang loop unroll(disable)
  while(n != 0u)
  {
    pf_ch(o, b[--n]);
  }
}

/* width > 0 时左侧补 '0'；hex 字母大写（全工程只此一种） */
PF_NOINLINE static void pf_hex(PF_T *o, uint32_t v, uint8_t width)
{
  char     b[8];
  uint8_t  n = 0u;

#pragma clang loop unroll(disable)
  do
  {
    b[n++] = "0123456789ABCDEF"[v & 0x0Fu];
    v >>= 4;
  } while(v != 0u);

#pragma clang loop unroll(disable)
  while(width > n)
  {
    pf_ch(o, '0');
    width--;
  }
#pragma clang loop unroll(disable)
  while(n != 0u)
  {
    pf_ch(o, b[--n]);
  }
}

static void pf_fmt(PF_T *o, const char *f, va_list ap)
{
  while(*f != '\0')
  {
    char     c = *f++;
    uint8_t  pad0 = 0u;
    uint8_t  width = 0u;

    if(c != '%')
    {
      pf_ch(o, c);
      continue;
    }

    c = *f++;
    if(c == '%')
    {
      pf_ch(o, '%');
      continue;
    }
    if(c == '0')
    {
      pad0 = 1u;
      c = *f++;
    }
    while((c >= '0') && (c <= '9'))
    {
      width = (uint8_t)(width * 10u + (uint8_t)(c - '0'));
      c = *f++;
    }
    if(c == 'l')            /* %lu / %08lX：32 位平台忽略 l */
    {
      c = *f++;
    }

    switch(c)
    {
    case 's':
      pf_str(o, va_arg(ap, const char *));
      break;

    case 'd':
      {
        int32_t  sv = (int32_t)va_arg(ap, int);
        uint32_t uv;

        if(sv < 0)
        {
          pf_ch(o, '-');
          uv = (uint32_t)(-(sv + 1)) + 1u;   /* 避免 INT32_MIN 取反溢出 */
        }
        else
        {
          uv = (uint32_t)sv;
        }
        pf_u32(o, uv);
      }
      break;

    case 'u':
      pf_u32(o, (uint32_t)va_arg(ap, unsigned int));
      break;

    case 'X':
      pf_hex(o, (uint32_t)va_arg(ap, unsigned int), pad0 ? width : 0u);
      break;

    case 'c':
      pf_ch(o, (char)va_arg(ap, int));
      break;

    default:               /* 不认识的格式符原样输出 */
      pf_ch(o, '%');
      if(c != '\0')
      {
        pf_ch(o, c);
      }
      else
      {
        return;            /* 格式串以 % 结尾 */
      }
      break;
    }
  }
}

/* 名字不叫 printf/snprintf：main.h 里用宏把标准名重定向到这两个。
 * 若直接定义成 printf，编译器会把 printf("纯字面量") 优化成库的 puts，
 * 从而把整条 stdio 链（printfa/puts/stdout/init）又拉回镜像 —— 实测撞过。 */
int boot_printf(const char *format, ...)
{
  char     buf[PF_BUF_SIZE];
  PF_T     o;
  va_list  ap;
  uint32_t n;

  o.buf = buf;
  o.cap = PF_BUF_SIZE;
  o.len = 0u;

  va_start(ap, format);
  pf_fmt(&o, format, ap);
  va_end(ap);

  n = (o.len < (PF_BUF_SIZE - 1u)) ? o.len : (PF_BUF_SIZE - 1u);
  buf[n] = '\0';
  (void)uart_send_data(USART1, buf, n);

  return (int)o.len;
}

int boot_snprintf(char *str, uint32_t size, const char *format, ...)
{
  PF_T     o;
  va_list  ap;
  uint32_t n;

  o.buf = str;
  o.cap = (uint32_t)size;
  o.len = 0u;

  va_start(ap, format);
  pf_fmt(&o, format, ap);
  va_end(ap);

  if(size != 0u)
  {
    n = (o.len < (uint32_t)size) ? o.len : ((uint32_t)size - 1u);
    str[n] = '\0';
  }
  return (int)o.len;
}
