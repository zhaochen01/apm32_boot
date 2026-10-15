/*!
 * @file        main.c
 *
 * @brief       Main program body
 *
 * @version     V1.0.3
 *
 * @date        2025-06-17
 *
 * @attention
 *
 *  Copyright (C) 2022-2025 Geehy Semiconductor
 *
 *  You may not use this file except in compliance with the
 *  GEEHY COPYRIGHT NOTICE (GEEHY SOFTWARE PACKAGE LICENSE).
 *
 *  The program is only for reference, which is distributed in the hope
 *  that it will be useful and instructional for customers to develop
 *  their software. Unless required by applicable law or agreed to in
 *  writing, the program is distributed on an "AS IS" BASIS, WITHOUT
 *  ANY WARRANTY OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the GEEHY SOFTWARE PACKAGE LICENSE for the governing permissions
 *  and limitations under the License.
 */

/* Includes */
#include "main.h"

#include "bsp_delay.h"
#include "hw_uart.h"
#include "upgrade_ctrl.h"

#include "at_file_ops.h"
#include "lte_modem.h"

#include "iap.h"
#include "serial_dl.h"




#define DEBUG_USART USART1

uint8_t count = 0;




// WDT ���Ź� 
HW_GPIO_INIT_T Hw_WDT_Cfg = { RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_0, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };

HW_GPIO_INIT_T Hw_RUN_Cfg = { RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_6, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };
HW_GPIO_INIT_T Hw_DATA_Cfg = { RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_7, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };
HW_GPIO_INIT_T Hw_SIM_Cfg = { RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_9, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };


/* JumpAddress / Jump_To_Application 已于 2026-09-17 随 Jump_to_App() 一并删除：
 * 新架构下跳 APP 由引导区负责，本工程不再有第二套跳转实现。 */




void hw_gpio_init( HW_GPIO_INIT_T gpio_cfg)
{
  	/* Enable GPIO clock */
	GPIO_Config_T  configStruct;
  
  /* Enable the GPIO_LED Clock */
  RCM_EnableAPB2PeriphClock(gpio_cfg.clk);

  /* Configure the GPIO_LED pin */
  configStruct.pin 		= gpio_cfg.pin;
  configStruct.mode 	= gpio_cfg.mode;
  configStruct.speed 	= gpio_cfg.speed;

  GPIO_Config(gpio_cfg.port, &configStruct);
}




/*!
 * @brief   This function handles TMR1 Update Handler
 *
 * @param   None
 *
 * @retval  None
 *
 */
void TMR5_IRQHandler(void)
{
  static uint8_t i = 0;
    
  if (TMR_ReadIntFlag(TMR5, TMR_INT_UPDATE) == SET)
  {
//    printf("tim5!\r\n");
      if( i == 0)
      {
        Hw_RUN_Cfg.port->BC  = Hw_RUN_Cfg.pin;
        Hw_DATA_Cfg.port->BC = Hw_DATA_Cfg.pin;
        Hw_SIM_Cfg.port->BC  = Hw_SIM_Cfg.pin;
        i = 1;
      }
      else
      {
        Hw_RUN_Cfg.port->BSC  = Hw_RUN_Cfg.pin;
        Hw_DATA_Cfg.port->BSC = Hw_DATA_Cfg.pin;
        Hw_SIM_Cfg.port->BSC  = Hw_SIM_Cfg.pin;
        i = 0;
      }
    
    TMR_ClearIntFlag(TMR5, TMR_INT_UPDATE);
  }
}


/**
 * @brief  TIM5��ʼ����500ms��ʱ�жϣ�����36MHz��Ƶ��
 * @retval ��
 */
void TIM5_Init_500ms_Interrupt(void)
{
    TMR_BaseConfig_T TMR_BaseConfigStruct;

    RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_TMR5);

    TMR_Reset(TMR5);
  
    TMR_BaseConfigStruct.clockDivision = TMR_CLOCK_DIV_1;        // ������Ƶ1��ͨ�ö�ʱ��Ĭ��ֵ���Ƴ������DIV4��
    TMR_BaseConfigStruct.countMode = TMR_COUNTER_MODE_UP;        // ���ϼ���ģʽ
    // Ԥ��Ƶ��36MHz / (35999 + 1) = 1000Hz��1ms/������
    TMR_BaseConfigStruct.division = 35999;                      
    // �Զ���װ��1000Hz �� 500ms = 500�� �� 499��500-1��
    TMR_BaseConfigStruct.period = 499; 
    TMR_BaseConfigStruct.repetitionCounter = 0;
    TMR_ConfigTimeBase(TMR5, &TMR_BaseConfigStruct);

    TMR_ClearIntFlag(TMR5, TMR_INT_UPDATE);
    TMR_EnableInterrupt(TMR5, TMR_INT_UPDATE);
    NVIC_EnableIRQRequest(TMR5_IRQn, 2, 0);

    TMR_Enable(TMR5);
}


/**
 * @brief  ��λ����CRC32����׼IEEE 802.3����ʽ��0xEDB88320��
 * @param  data: �������ݻ�����
 * @param  length: ���ݳ��ȣ��ֽ�����
 * @retval ���յ�CRC32ֵ��ȡ����
 */
uint32_t CRC32(uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFF; // ��ʼֵ
    uint32_t poly = 0xEDB88320; // ��׼CRC32����ʽ������

    for (uint32_t i = 0; i < length; i++)
    {
        // ��ǰ�ֽ���CRC��8λ���
        crc ^= data[i];
        // ��λ������8λ��
        for (uint8_t j = 0; j < 8; j++)
        {
            // ���ƣ����λΪ1��������ʽ
            crc = (crc >> 1) ^ ((crc & 0x01) ? poly : 0);
        }
    }
    // ���ս��ȡ��
    return ~crc;
}


/** @defgroup USART_Interrupt_Functions Functions
  @{
*/

/*!
 * @brief       Main program
 *
 * @param       None
 *
 * @retval      None
 *
 */
int main(void)
{
	SystemInit();
	SystemCoreClockUpdate();
  
  Delay_Init();

  // ʹ��SWJ�����أ�����jtag���ء�
  RCM_EnableAPB2PeriphClock(RCM_APB2_PERIPH_AFIO);
  GPIO_ConfigPinRemap(GPIO_REMAP_SWJ_JTAGDISABLE);
 
  hw_gpio_init(Hw_WDT_Cfg);
  hw_gpio_init(Hw_RUN_Cfg);
  hw_gpio_init(Hw_DATA_Cfg);
  hw_gpio_init(Hw_SIM_Cfg);

  TIM5_Init_500ms_Interrupt();

  user_uart_init();
  
  printf("\r\nBoot Loader... %s\r\n", VERSION);
  
  /* 2026-09-16：片内 IWDT 已移除——它一旦使能无法软件关闭，跳转 APP 后
   * 会因无人喂而 15s 复位。看门狗统一由外部 WDT（PB0）承担，
   * 且改为长操作主动喂（见 WDT_FEED），保证主流程卡死时能复位。 */
  {
    UPGRADE_CTX_T ctx;
    int32_t act = iap_init(&ctx);   /* ctx 带回安装参数；返回码见 iap.h IAP_ACT_* */

    if(act == IAP_ACT_INSTALL)
    {
      upgrade_run(&ctx);            /* 内部完成安装并复位，永不返回 */
      /* 真返回了（不该发生）—— 不猜，直接 park */
      printf("[upgrade-boot] unexpected: upgrade_run returned\r\n");
      act = IAP_ACT_PARK;
    }

    if(act == IAP_ACT_RESUME_APP)
    {
      /* APP 有效、flag 已被 iap_init 改成稳态 -> 复位交回引导区。
       * 复位是终止性的：flag 变了，引导区的下一次决策必然不同。 */
      printf("[upgrade-boot] APP ready, hand back to bootstrap\r\n");
      NVIC_SystemReset();
    }

    if(act != IAP_ACT_PARK)
    {
      /* ---- 串口下载兜底窗口（2026-09-18）----
       * 砖机（A/B 都不可用）时引导区【无论有没有收到魔数】都会把我们跳进来
       * —— 那是它的修复路径。操作员可能忘了发魔数，或者工具接晚了、复位
       * 按早了。给 8 秒等一条请求行，没人来就继续走原来的模组 UFS 探测。
       *
       * 不会和"魔数入口"抢：那条路在上面的 iap_init() 里就返回 SERIAL 了，
       * 根本到不了这里 —— 这里的窗口纯粹靠请求行本身触发。 */
      {
        uint32_t dl_sz = 0U;
        uint32_t dl_crc = 0U;
        uint32_t dl_baud = 0U;

        printf("[upgrade-boot] serial download window (%lu ms)...\r\n",
               (unsigned long)SERIAL_DL_BOOT_WAIT_MS);

        if(serial_dl_wait_request(SERIAL_DL_BOOT_WAIT_MS, &dl_sz, &dl_crc, &dl_baud))
        {
          UPGRADE_CTX_T sctx;

          memset(&sctx, 0, sizeof(sctx));
          sctx.action      = UPGRADE_ACTION_SERIAL;
          sctx.run_addr    = upgrade_cfg.app_addr;
          sctx.backup_addr = upgrade_cfg.fw1_addr;
          sctx.expect_size = dl_sz;
          sctx.expect_crc  = dl_crc;
          sctx.serial_baud = dl_baud;   /* 请求行里协商的波特率（缺省 115200） */

          printf("[upgrade-boot] serial request caught\r\n");
          upgrade_run(&sctx);       /* 内部完成安装并复位，永不返回 */
        }
      }

      /* ---- 无可用固件：去模组 UFS 找升级程序（生产阶段的判断依据） ---- */
      uint32_t ufs_sz = 0U;

      printf("[upgrade-boot] checking module UFS...\r\n");

      if(lte_modem_init(&LTE_MODEM_CFG_EG800K) == 0)
      {
        AT_FILE_CTX_T f;
        int32_t r = at_file_open(&f, UPGRADE_FILE_NAME);

        if(r == AT_FILE_ERR_OK)
        {
          r = at_file_get_size(&f);
          if(r == AT_FILE_ERR_OK)
          {
            ufs_sz = f.file_size;
            printf("[upgrade-boot] module file %s size=%lu\r\n", UPGRADE_FILE_NAME,
                   (unsigned long)ufs_sz);
          }
          else
          {
            printf("[upgrade-boot] QFLST failed, ret=%d\r\n", (int)r);
          }
          (void)at_file_close(&f);      /* 用完即关，upgrade_run 会重新打开 */
        }
        else
        {
          /* 打不开 = 模组 UFS 里没有这个文件。这是"模块里没固件"的唯一线索，
           * 之前静默跳过，现场只能看到 UFS file size=0，无法区分
           * "文件不存在" 与 "AT 通道失败"。 */
          printf("[upgrade-boot] no firmware on module (open ret=%d)\r\n", (int)r);
        }
      }
      else
      {
        printf("[upgrade-boot] modem init FAILED - cannot check UFS\r\n");
      }

      if((ufs_sz > 0U) && (ufs_sz <= APP_MAX_SIZE))
      {
        UPGRADE_CTX_T uctx;
        memset(&uctx, 0, sizeof(uctx));
        uctx.action      = UPGRADE_ACTION_NETWORK;
        uctx.run_addr    = upgrade_cfg.app_addr;
        uctx.backup_addr = upgrade_cfg.fw1_addr;
        uctx.expect_size = ufs_sz;

        /* 有元数据可比对就全量校验；否则退化为"只看大小"并告警。
         * 模组 UFS 里的文件若损坏（写入中断 / 位翻转），零校验会把它原样
         * 装进 APP —— 而 fw[0] 记录的就是这段内容的 CRC，引导区之后自然
         * 校验通过，于是一个被截断的镜像会被当成好固件跳进去。 */
        if((ufs_sz == upgrade.size) &&
           (upgrade.crc != 0U) && (upgrade.crc != 0xFFFFFFFFU))
        {
          uctx.expect_crc = upgrade.crc;
        }
        else
        {
          uctx.expect_crc = 0U;
          printf("[upgrade-boot] WARN: no metadata, size-only check\r\n");
        }
        upgrade_run(&uctx);           /* 内部完成安装并复位，不返回 */
      }
    }
  }

  /* 生产阶段（空板）或需人工介入 —— 提示并停留等待烧录。
   * 停留期间持续喂狗：此循环的存续本身即"未卡死"。
   * 绝不复位 —— 复位会与引导区形成环。 */
  printf("[upgrade-boot] production mode - please flash firmware\r\n");

  while(1)
  {
    WDT_FEED();
    Delay_ms(500);
  }
}




#if defined (__CC_ARM) || defined (__ICCARM__) || (defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050))

/*!
* @brief       Redirect C Library function printf to serial port.
*              After Redirection, you can use printf function.
*
* @param       ch:  The characters that need to be send.
*
* @param       *f:  pointer to a FILE that can recording all information
*              needed to control a stream
*
* @retval      The characters that need to be send.
*
* @note
*/
int fputc(int ch, FILE* f)
{
 // __disable_irq();
  uart_send_data(DEBUG_USART, &ch, 1);
//  __enable_irq();
  return (ch);
}

#else
#warning Not supported compiler type
#endif

/**@} end of group USART_Interrupt_Functions */
/**@} end of group USART_Interrupt */
/**@} end of group Examples */
