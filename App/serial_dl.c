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

  /* 走 serial_dl_getc() 而不是"自己判 RXBNE + 读 DATA"：getc 会在 RXBNE 没置位
   * 但 OVRE/FE/NE 置位时补一次 DATA 读，把标志清掉（见那里的长注释）。原实现
   * 直接读 DATA，遇到"OVRE 已锁存、RXBNE 不再置位"时一次都清不掉 —— flush 变成
   * 空操作，紧接着的收页从第一字节起就是聋的。 */
  while((guard < DL_FLUSH_MAX) && (serial_dl_getc() >= 0))
  {
    guard++;
  }
}

/* USART1 接收错误计数（诊断用）。为什么要它：切到 460800/921600 后若刷机失败，
 * "设备丢了几个字节"与"PC 根本没发"在日志上长得一模一样（都表现为 rx timeout），
 * 而这两者的修法完全不同（前者要改接收路径，后者要查上位机/线材）。
 * ORE=硬件接收寄存器被覆盖（真丢字节）、FE=帧错、NE=噪声。
 *
 * 只在 getc 里【按位累加】，不改变状态：先读一次 STS（ORE/FE/NE/RXBNE 都在里面），
 * 再按需读 DATA —— 读 STS+DR 正是清 ORE 的规定序列。 */
int32_t serial_dl_getc(void)
{
  uint32_t sts = DL_USART->STS;

  if((sts & (uint32_t)USART_FLAG_RXBNE) != 0u)
  {
    return (int32_t)(uint8_t)USART_RxData(DL_USART);
  }

  /* ============ 本工程最贵的一课（2026-10-14 实测）============
   * RXBNE 没置位 ≠ "什么都没发生"：OVRE/FE/NE 只要置位，就必须【无条件再读
   * 一次 DATA】才算走完"读 STS + 读 DATA"这个规定清除序列。
   *
   * 原实现只在 RXBNE 置位时才读 DATA —— 于是只要出现一次"OVRE 置位且 RXBNE
   * 不置位"的时刻，OVRE 就永远没人清，接收端【永久失聪】，而轮询循环还在
   * 一圈圈地把同一个标志记成新的溢出。
   *
   * 实测症状（460800 串口刷机，第 2 页）：
   *   [serial] rx timeout at off=2048 got=0/2048 ore=2569752 fe=0 ne=0
   * 注意 fe=0 ne=0 —— 波特率是对的、线路是干净的；字节确实在进（否则 OVRE
   * 不可能置位），但一个也读不出来。上位机那边把那 2048 字节完整发了出去。
   * 修掉它之后，一次偶发溢出最多只丢 1 个字节（由整包 CRC 兜住），而不是
   * 整轮下载报废。
   *
   * 读的是 32 位 DATA 而不是 USART_RxData()：后者经 DATA_B 位域（9 位），
   * 编译器可能发窄读；清除序列要求的是"读 DR 这个寄存器地址"。返回值是垃圾
   * （可能是残留字节），必须丢掉 —— 要的只是清标志这个副作用。 */
  if((sts & (uint32_t)(USART_FLAG_OVRE | USART_FLAG_FE | USART_FLAG_NE)) != 0u)
  {
    (void)DL_USART->DATA;
  }
  return -1;
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

/* ==================== 收页改走 DMA（2026-10-14）====================
 * 为什么必须上 DMA：460800 下用轮询收字节实测会【偶发丢字节】—— 一次 166KB
 * 传输丢 1~2 个，整包 CRC 因此不过、整轮下载在最后一页报废（实测记录：
 * 82 页全收完，最后校验失败）。丢的机制是"轮询循环被饿到超过 1 字节时间
 * （460800 下 21.7µs）就触发一次 OVRE"（见 getc 的长注释）。
 * DMA 让字节由硬件从 DATA 搬进 RAM，CPU 完全不参与搬运，从根上消掉这一类。
 *
 * 【协议不变】：收的还是同样多字节，只是搬运工换了人 —— 上位机与旧工具
 * 都不受影响。握手阶段（等请求行）仍走轮询 getc：那一次只有几十字节，
 * 而且状态机本来就要求逐字节看内容。 */
#define DL_DMA_CH       DMA1_Channel5   /* USART1_RX 固定映射 DMA1 通道 5（F1 系映射） */
#define DL_DMA_FLAGS    ((uint32_t)(DMA1_FLAG_GINT5 | DMA1_FLAG_TC5 | \
                                    DMA1_FLAG_HT5 | DMA1_FLAG_TERR5))

static uint8_t      s_dma_ready = 0u;
static DMA_Config_T s_dma_cfg;

static void serial_dl_dma_setup(void)
{
  RCM_EnableAHBPeriphClock(RCM_AHB_PERIPH_DMA1);
  DMA_Reset(DL_DMA_CH);

  DMA_ConfigStructInit(&s_dma_cfg);
  s_dma_cfg.peripheralBaseAddr = (uint32_t)(&DL_USART->DATA);
  s_dma_cfg.dir                = DMA_DIR_PERIPHERAL_SRC;
  s_dma_cfg.peripheralInc      = DMA_PERIPHERAL_INC_DISABLE;
  s_dma_cfg.memoryInc          = DMA_MEMORY_INC_ENABLE;
  s_dma_cfg.peripheralDataSize = DMA_PERIPHERAL_DATA_SIZE_BYTE;
  s_dma_cfg.memoryDataSize     = DMA_MEMORY_DATA_SIZE_BYTE;
  s_dma_cfg.loopMode           = DMA_MODE_NORMAL;     /* 一页一次，不用环形 */
  s_dma_cfg.priority           = DMA_PRIORITY_VERYHIGH;   /* 收页期间没有别的 DMA 用户 */
  s_dma_cfg.M2M                = DMA_M2MEN_DISABLE;
  /* memoryBaseAddr / bufferSize 每页重设 */

  DMA_ClearStatusFlag(DL_DMA_FLAGS);
  USART_EnableDMA(DL_USART, USART_DMA_RX);
  s_dma_ready = 1u;
}

/* 收满 len 字节返回 len；超时返回 -1（已收字节数进诊断）。语义与旧的轮询版
 * 完全一致，只是搬运由 DMA 做 —— 上层（收页循环）一行都不用改。 */
int32_t serial_dl_recv(uint8_t *buf, uint32_t len, uint32_t timeout_ms,
                       uint32_t *got)
{
  uint32_t start;
  uint32_t last_feed;

  if(got != NULL) *got = 0u;
  if((buf == NULL) || (len == 0u)) return -1;
  if(s_dma_ready == 0u)
  {
    serial_dl_dma_setup();
  }

  /* 先排空：滞留在 DATA 里的残字节会被 DMA 当成【页首】搬进来 → 整页错位
   * （那正是原来"收满 2048 但内容不对"的形态）。顺带把 OVRE/FE/NE 清掉
   * —— 见 getc 里为什么必须无条件清。 */
  serial_dl_flush();

  s_dma_cfg.memoryBaseAddr = (uint32_t)buf;
  s_dma_cfg.bufferSize     = len;
  DMA_Config(DL_DMA_CH, &s_dma_cfg);      /* DMA_Config 只写配置寄存器，不使能通道 */
  DMA_ClearStatusFlag(DL_DMA_FLAGS);
  DMA_Enable(DL_DMA_CH);

  start     = Delay_GetTick();
  last_feed = start;

  while(DMA_ReadStatusFlag(DMA1_FLAG_TC5) == RESET)
  {
    if((DMA_ReadStatusFlag(DMA1_FLAG_TERR5) != RESET) ||
       ((uint32_t)(Delay_GetTick() - start) >= timeout_ms))
    {
      /* 超时/传输出错：把【已搬进来的字节数】交出去 —— 它区分
       * "对端没发"（接近 0）与"中途断流"（接近满）。 */
      if(got != NULL)
      {
        *got = len - (uint32_t)DMA_ReadDataNumber(DL_DMA_CH);
      }
      DMA_Disable(DL_DMA_CH);
      DMA_ClearStatusFlag(DL_DMA_FLAGS);
      return -1;
    }
    serial_dl_feed_soft(&last_feed);
  }

  DMA_Disable(DL_DMA_CH);
  DMA_ClearStatusFlag(DL_DMA_FLAGS);
  if(got != NULL) *got = len;
  return (int32_t)len;
}

static int32_t serial_dl_hexval(uint8_t ch)
{
  if((ch >= '0') && (ch <= '9')) return (int32_t)(ch - '0');
  if((ch >= 'a') && (ch <= 'f')) return (int32_t)(ch - 'a') + 10;
  if((ch >= 'A') && (ch <= 'F')) return (int32_t)(ch - 'A') + 10;
  return -1;
}

/* 波特率白名单：只有这四个值会被接受，其余一律回落 115200。
 * 宁可慢，不可把链路切到双方对不上的速率上 —— 那时"请求行已确认"
 * 就成了一句空话，而现场只会看到刷机失败。
 * 与 tools/serial_dl.py 的 BAUD_WHITELIST 必须一致。 */
static uint32_t serial_dl_check_baud(uint32_t b)
{
  if((b == 115200u) || (b == 230400u) || (b == 460800u) || (b == 921600u))
  {
    return b;
  }
  return SERIAL_DL_BAUD_DEFAULT;
}

int32_t serial_dl_set_baud(uint32_t baud)
{
  return user_uart_set_baud(DL_USART, baud);
}

/*
 * 请求行状态机。
 *
 *   0: 找 '#'      1: 'D'      2: 'L'      3: ' '
 *   4: size（十进制，≤8 位）   5: crc32（十六进制，≤8 位）
 *   6: baud（十进制，≤7 位，可选字段）
 *
 * 任何不匹配都退回 0 重新找 '#' —— 于是"0x55 0xAA 魔数残留 / 上次的
 * 半行 / 操作员乱敲的字符 / 波特率切换窗口里落进来的错帧字节"都只是
 * 被跳过，不需要任何转义或握手。唯一能返回 1 的路径是收到 '\r' 或
 * '\n' 且必填字段都非空。
 *
 * 不带 baud 字段（老格式）时按 115200 收尾 —— 老工具零影响。
 */
int32_t serial_dl_wait_request(uint32_t timeout_ms, uint32_t *size,
                               uint32_t *crc, uint32_t *baud)
{
  uint32_t start;
  uint32_t last_feed;
  uint32_t sz = 0u, ck = 0u, bd = 0u;
  uint32_t sz_n = 0u, ck_n = 0u, bd_n = 0u;
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
        /* 老格式（无 baud 字段）：按缺省波特率收尾 */
        *size = sz;
        *crc  = ck;
        if(baud != NULL) *baud = SERIAL_DL_BAUD_DEFAULT;
        return 1;
      }
      else if((ch == ' ') && (ck_n > 0u))
      {
        st = 6u; bd = 0u; bd_n = 0u;     /* 可选波特率字段 */
      }
      else if(ch == '#')      st = 1u;
      else                    st = 0u;
      break;

    case 6u:
      if((ch >= '0') && (ch <= '9'))
      {
        bd = (bd * 10u) + (uint32_t)(ch - '0');
        bd_n++;
        /* 越界即弃：最长的 921600 也只有 6 位 */
        if(bd_n > 7u) st = 0u;
      }
      else if(((ch == '\r') || (ch == '\n')) && (bd_n > 0u))
      {
        *size = sz;
        *crc  = ck;
        if(baud != NULL) *baud = serial_dl_check_baud(bd);
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
