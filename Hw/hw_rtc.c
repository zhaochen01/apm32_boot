#include "hw_board.h"


// RTC初始化标志，存储在备份寄存器中
#define RTC_INIT_FLAG_VALUE     0x5A5A    // RTC已初始化标志值
#define RTC_INIT_FLAG_REG       BAKPR_DATA1  // 使用备份寄存器1存储初始化标志



DateTime utc_System;

// RTC状态标志
static uint8_t rtc_initialized = 0;
static uint8_t rtc_lse_ready = 0;

/*!
 * @brief     检查RTC是否已经初始化过（通过备份寄存器判断）
 *
 * @param     None
 *
 * @retval    1: 已初始化, 0: 未初始化
 */
uint8_t rtc_check_initialized_flag(void)
{
    uint16_t flag_value;
    
    // 确保PMU和BAKR时钟已使能
    RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_PMU);
    RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_BAKR);
    
    // 确保备份域访问权限已开启
    PMU_EnableBackupAccess();
    
    // 读取备份寄存器中的初始化标志
    flag_value = BAKPR_ReadBackupRegister(RTC_INIT_FLAG_REG);
    
    return (flag_value == RTC_INIT_FLAG_VALUE) ? 1 : 0;
}

/*!
 * @brief     设置RTC初始化标志到备份寄存器
 *
 * @param     None
 *
 * @retval    0: 成功, -1: 失败
 */
static int rtc_set_initialized_flag(void)
{
    uint16_t read_value;
    int retry_count = 0;
    
    // 确保PMU和BAKR时钟已使能
    RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_PMU);
    RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_BAKR);
    
    // 确保备份域访问权限已开启
    PMU_EnableBackupAccess();
    
    // 尝试写入备份寄存器
    BAKPR_ConfigBackupRegister(RTC_INIT_FLAG_REG, RTC_INIT_FLAG_VALUE);
    
    // 验证写入是否成功，最多重试3次
    do {
        rt_thread_mdelay(1); // 短暂延时确保写入完成
        read_value = BAKPR_ReadBackupRegister(RTC_INIT_FLAG_REG);
        
        if(read_value == RTC_INIT_FLAG_VALUE) {
            rt_kprintf("RTC init flag set successfully: 0x%04X\r\n", read_value);
            return 0;
        }
        
        retry_count++;
        if(retry_count < 3) {
            rt_kprintf("RTC init flag write failed, retrying... (attempt %d)\r\n", retry_count + 1);
            // 重新尝试写入
            BAKPR_ConfigBackupRegister(RTC_INIT_FLAG_REG, RTC_INIT_FLAG_VALUE);
        }
        
    } while(retry_count < 3);
    
    rt_kprintf("ERROR: Failed to set RTC init flag after 3 attempts!\r\n");
    rt_kprintf("Expected: 0x%04X, Got: 0x%04X\r\n", RTC_INIT_FLAG_VALUE, read_value);
    return -1;
}

/*!
 * @brief     RTC初始化函数
 *
 * @param     None
 *
 * @retval    0: 成功, -1: 失败
 */
static int hw_rtc_init(void)
{
    // 使能PMU和BAKR时钟
    RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_PMU | RCM_APB1_PERIPH_BAKR);
    PMU_EnableBackupAccess();

    // 检查RTC是否已经初始化过
    if(BAKPR_ReadBackupRegister(RTC_INIT_FLAG_REG) == RTC_INIT_FLAG_VALUE) 
    {
        rt_kprintf("RTC already initialized, skipping...\r\n");
        RTC_EnableInterrupt(RTC_INT_SEC);
        RTC_WaitForLastTask();
        // 确保中断仍然启用
        NVIC_EnableIRQRequest(RTC_IRQn, 0, 0);
        return 0;
    }
    
    rt_kprintf("Initializing RTC...\r\n");
    
    // 配置外部低速振荡器(LSE)
    RCM_ConfigLSE(RCM_LSE_OPEN);
    rt_kprintf("Waiting for LSE ready...\r\n");
    while (RCM_ReadStatusFlag(RCM_FLAG_LSERDY) == RESET);
    rt_kprintf("LSE ready!\r\n");
    
    // 选择LSE作为RTC时钟源并使能RTC时钟
    RCM_ConfigRTCCLK(RCM_RTCCLK_LSE);
    RCM_EnableRTCCLK();

    // 进入临界区进行RTC配置
    rt_enter_critical();
    
    // 等待RTC同步
    RTC_WaitForSynchro();
    RTC_WaitForLastTask();

    // 进入配置模式
    RTC_EnableConfigMode();
    RTC_WaitForLastTask();

    // 使能秒中断
    RTC_EnableInterrupt(RTC_INT_SEC);
    RTC_WaitForLastTask();

    // 设置预分频器 (32768-1 = 32767，产生1Hz的秒脉冲)
    RTC_ConfigPrescaler(32767);
    RTC_WaitForLastTask(); 
    
    // 退出配置模式
    RTC_DisableConfigMode();
    RTC_WaitForLastTask();
    
    // 设置初始化标志
    BAKPR_ConfigBackupRegister(RTC_INIT_FLAG_REG, RTC_INIT_FLAG_VALUE);
    
    rt_exit_critical();
    
    // 配置NVIC中断
    RTC_ClearIntFlag(RTC_INT_SEC);
    NVIC_EnableIRQRequest(RTC_IRQn, 0, 0);
    
    timeUpdata_GPS = 1;
    rt_kprintf("RTC initialization completed!\r\n");
    return 0;
}


/*!
 * @brief     设置RTC日期时间
 *
 * @param     dt: 时间戳
 *
 * @retval    0: 成功, -1: 失败
 */
__INLINE int RTC_SetTimestamp(uint32_t timestamp)
{
 
    // 只包裹硬件相关操作
    rt_enter_critical();
    
//    RTC_EnableConfigMode();
//    RTC_WaitForLastTask();
    
    RTC_ConfigCounter(timestamp);
    RTC_WaitForLastTask();
    
//    RTC_DisableConfigMode();
//    RTC_WaitForLastTask();
    
    rt_exit_critical();
    
    return 0;
}

/*!
 * @brief     获取RTC日期时间
 *
 * @param     Note
 *
 * @retval    时间戳
 */
__INLINE uint32_t RTC_GetTimestamp(void)
{

  // 读取RTC计数器值，这个操作需要保护以避免读取过程中被中断
//  rt_enter_critical();
  uint32_t timestamp = RTC_ReadCounter();
//  rt_exit_critical();
  
  return timestamp;
}







INIT_DEVICE_EXPORT(hw_rtc_init);
