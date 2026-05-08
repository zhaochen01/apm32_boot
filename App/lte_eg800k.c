


#include "bsp_delay.h"
#include "hw_uart.h"

#include "lte_eg800k.h"
#include "iap.h"

#include <string.h>


HW_GPIO_INIT_T Hw_IoT_Cfg = { RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_1, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };


// UART3接收缓冲区与状态变量
uint8_t uart3_rx_buf[UART3_RX_BUF_SIZE] = {0};
uint16_t uart3_rx_len = 0;
uint8_t uart3_rx_flag = 0, uart3_idle_flag;
volatile uint8_t RX_FLAG = 0;
volatile uint32_t FILE_SIZE = 0;

uint8_t up_file_buf[2048];




extern int32_t get_data_len( char *pdat);
extern int32_t get_file_len(char *pdat);

void eg800k_init(void)
{
      // 初始化4G模块控制GPIO
  hw_gpio_init(Hw_IoT_Cfg);                 // 配置GPIO引脚
  
  USART_ClearIntFlag(USART3, USART_INT_RXBNE); 
  USART_EnableInterrupt(USART3, USART_INT_RXBNE);  // 使能接收缓冲区非空中断
  
//  USART_EnableInterrupt(USART3, USART_INT_IDLE);
  
  NVIC_EnableIRQRequest(USART3_IRQn, 1, 0);  
  
//  USART3_RX_DMA_Init();
  Hw_IoT_Cfg.port->BSC = Hw_IoT_Cfg.pin; 
  Delay_ms(500);
  Hw_IoT_Cfg.port->BC = Hw_IoT_Cfg.pin;    // 拉高4G模块控制引脚，使能模块
  
  CheckResp_loop("RDY",3000); 
  
  printf("eg800k init successful!\r\n");
}


uint8_t get_uart3_flag(void)
{
  return uart3_idle_flag;
}

/**
 * @brief  检查缓冲区中是否包含目标响应
 * @param  target_resp：目标响应字符串
 * @retval -1：未找到，0：找到
 */
int32_t CheckResp(char *target_resp)
{
    if (target_resp == NULL || uart3_rx_len == 0)
    {
        return -1;
    }
    // strstr要求字符串以\0结尾，中断中已保证缓冲区末尾有\0
    return (strstr((char *)uart3_rx_buf, target_resp) != NULL) ? 0 : -1;
}

/**
 * @brief  检测指定响应字符串是否存在，固定5次循环+可设置单次延时
 * @param  target_resp: 目标响应字符串（如"+QFLST:"）
 * @param  delay_ms: 每次循环的延时时间（单位：ms，如100ms、200ms）
 * @retval 0: 找到目标字符串; -1: 未找到或参数错误/超时
 * @note   固定循环5次后仍未找到则退出，单次延时时间可自定义
 */
int32_t CheckResp_loop(char *target_resp, uint32_t delay_ms)
{
    // 固定循环次数为5次
    const uint32_t MAX_LOOP_COUNT = 5;
    uint32_t current_loop = 0;

    // 参数合法性检查
    if (target_resp == NULL)
    {
        return -1;
    }

    // 循环检测：固定5次，每次延时指定时间
    while (current_loop < MAX_LOOP_COUNT)
    {

      // strstr要求字符串以\0结尾，中断中已保证缓冲区末尾有\0
      if (strstr((char *)uart3_rx_buf, target_resp) != NULL)
      {
          return 0; // 找到目标，返回成功
      }

      current_loop++; // 循环计数+1
      Delay_ms(delay_ms); // 每次循环后延时指定时间（核心：延时时间可配置）
    }

    // 固定5次循环后仍未找到，返回超时失败
    return -1;
}



/**
 * @brief  单函数实现：发送AT指令到EC800，等待指定响应（支持超时配置，无外部函数调用）
 * @param  cmd：要发送的AT指令（如"AT+CGATT=1"，无需加\r\n）
 * @param  target_resp：要等待的目标响应（如"OK"、"+CNACT:"、"ERROR"）
 * @param  timeout_ms：超时时间（单位：ms，传入0则使用默认值5000ms）
 * @retval 0：超时/参数错误/未找到响应，1：成功匹配目标响应
 * @note   内部直接内联所有逻辑，不调用任何外部辅助函数
 */
int32_t LTE_SendCmdWaitResp(char *cmd, char *target_resp, uint32_t timeout_ms)
{
    /************************* 步骤1：参数合法性检查 *************************/
    if (cmd == NULL)
    {
        return -1; // 指令或响应为空，直接返回失败
    }
    
    // 处理超时时间：传入0则使用默认值5000ms
    uint32_t timeout = (timeout_ms == 0) ? AT_CMD_DEFAULT_TIMEOUT : timeout_ms;
    uint32_t start_tick = g_sys_tick; // 记录起始时间戳（1ms精度）

    /************************* 步骤2：清空接收缓冲区（原子操作） *************************/
//    __disable_irq(); // 关闭全局中断，防止中断中修改缓冲区导致数据错乱
//    uart3_rx_len = 0;
//    memset(uart3_rx_buf, 0, UART3_RX_BUF_SIZE); // 清空缓冲区
//    __enable_irq(); // 开启全局中断

    /************************* 步骤3：发送AT指令 *************************/
    // 3.1 发送指令主体（调用你已实现的串口发送函数）
    uart_send_data(USART3, cmd, strlen(cmd));


    /************************* 步骤4：循环检测响应 + 超时判断（改进版） *************************/
    if (target_resp == NULL)
    {
      /* 不等待任何响应，立即返回成功 */
      return 0;
    }

    while (1)
    {
      /* 原子性拷贝接收缓冲到本地，避免中断竞争 */
      char local_buf[UART3_RX_BUF_SIZE + 1];
      uint32_t copy_len = 0;
      __disable_irq();
      copy_len = uart3_rx_len;
      if (copy_len > UART3_RX_BUF_SIZE) copy_len = UART3_RX_BUF_SIZE;
      if (copy_len > 0)
      {
        memcpy(local_buf, uart3_rx_buf, copy_len);
      }
      __enable_irq();
      local_buf[copy_len] = '\0';

      /* 按行解析 local_buf（以 CRLF 为行分隔），更智能地匹配 target_resp */
      if (copy_len > 0)
      {
        char *line = local_buf;
        while (*line)
        {
          /* 找到行结尾 */
          char *eol = strstr(line, "\r\n");
          size_t linelen = eol ? (size_t)(eol - line) : strlen(line);

          /* 跳过行首空白 */
          char *p = line;
          while (linelen > 0 && (*p == ' ' || *p == '\t')) { p++; linelen--; }

          /* 简单匹配策略：前缀匹配或包含匹配都视为命中 */
          size_t tr_len = strlen(target_resp);
          if (linelen >= tr_len && strncmp(p, target_resp, tr_len) == 0)
          {
            return 0; /* 前缀匹配 */
          }
          else if (strstr(p, target_resp) != NULL)
          {
            return 0; /* 包含匹配 */
          }

          if (!eol) break;
          line = eol + 2; /* 跳过 CRLF */
        }
      }

      /* 超时判断（处理 g_sys_tick 溢出） */
      uint32_t tick_diff = (g_sys_tick >= start_tick) ? (g_sys_tick - start_tick) : (0xFFFFFFFF - start_tick + g_sys_tick + 1);
      if (tick_diff >= timeout)
      {
        return -2; /* 超时，返回失败 */
      }
    }
}



/*!
 * @brief       USART3_Interrupt
 *
 * @param       None
 *
 * @retval      None
 *
 */
void USART3_Isr(void)
{
  uint8_t dat = 0;
  static uint16_t i = 0;
  static uint32_t len = 0;
  static uint32_t rx2_count = 0; /* 专用于 RX_FLAG==2 的计数器 */

    // 检查接收缓冲区非空中断标志
  if(USART_ReadIntFlag(USART3, USART_INT_RXBNE))
  {
    dat = (uint8_t)USART_RxData(USART3);  // 读取接收到的字节

    if (RX_FLAG != 2)
    {
      /* 避免把LTE原始数据透传到调试串口，防止与printf日志串扰 */
      // uart_send_data(USART1, &dat, 1);
      i %= UART3_RX_BUF_SIZE;
      uart3_rx_buf[i++] = dat;                     // 存储到缓冲区并递增索引

      /* 检查数据包结束条件：换行符（且缓冲区有数据）—— 仅在普通模式下 */
      if (i >= 2 && uart3_rx_buf[i-2] == '\r' && uart3_rx_buf[i-1] == '\n')
      {
        uart3_rx_buf[i] = '\0';
        uart3_rx_len = i + 1;

        if (RX_FLAG == 1)
        {
          len = get_data_len((char *)uart3_rx_buf);
          if(len != -1)
          {
            FILE_SIZE = len;
            if ((len > 0U) && (len <= sizeof(up_file_buf)))
            {
              /* 进入长度接收模式：使用独立计数器，不再使用 i 作为计数 */
              RX_FLAG = 2;
              rx2_count = 0;
              /* 如果 len 为0或过大，可选择默认2048保护 (保持原行为) */
         //     if (len == 0) len = 2048;
            }
            else
            {
              RX_FLAG = 0;
              FILE_SIZE = 0;
              len = 0;
              rx2_count = 0;
            }
          }
          else
          {
            RX_FLAG = 0;
            FILE_SIZE = 0;
            len = 0;
            rx2_count = 0;
          }
        }
        i = 0;                              // 重置普通缓冲区索引
      }
      else if ((dat == '\r' || dat == '\n') && i == 1) // 处理单独的\r或\n（空行，直接清空）
      {
        i = 0;
      }
      else if (dat == '>')
      {
        i = 0;
      }
    }
    else /* RX_FLAG == 2：长度接收模式，使用 rx2_count 统计接收字节数 */
    {
      if((len == 0U) || (len > sizeof(up_file_buf)))
      {
        RX_FLAG = 0;
        FILE_SIZE = 0;
        len = 0;
        rx2_count = 0;
        return;
      }

      /* 增加接收计数（每接收一个字节） */
      if(rx2_count < len)
      {
        up_file_buf[rx2_count++] = dat;
      }
      else
      {
        RX_FLAG = 0;
        FILE_SIZE = 0;
        len = 0;
        rx2_count = 0;
        return;
      }
//      i %= 2048;
      /* 可选：把数据存入专用缓冲，或由上层直接读取串口 DMA 缓冲；这里仅计数 */
      if (rx2_count >= len)
      {
        /* 完成 */
        RX_FLAG = 3;
        rx2_count = 0;
        len = 0;
      }
      /* 调试信息，避免频繁打印 */
      // if (rx2_count == 2048) printf("RX_FLAG:%d\trx2_count:%lu\r\n", RX_FLAG, (unsigned long)rx2_count);
    }
  }

}

