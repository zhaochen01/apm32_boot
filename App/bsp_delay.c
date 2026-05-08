#include "bsp_delay.h"


volatile uint32_t g_sys_tick = 0; // 系统滴答数（ms级）

/* The count of microseconds */
__IO uint32_t cntUs = 0;
/* The count of milliseconds */
__IO uint32_t cntMs = 0;


/**
 * @brief  SysTick中断服务函数（必须添加到中断向量表）
 * @retval 无
 */
void SysTick_Handler(void)
{
    g_sys_tick++; // 每1ms加1
    cntUs--;
    cntMs--;
}

/**
 * @brief  初始化延时函数（SysTick 1ms中断一次）
 * @retval 无
 * @note   系统时钟为36MHz
 */
void Delay_Init(void)
{
  SysTick_Config(SystemCoreClock / 1000);
}


/**
 * @brief  获取当前系统滴答数（ms级）
 * @retval 当前滴答数
 */
uint32_t Delay_GetTick(void)
{
    return g_sys_tick;
}

/**
 * @brief  非阻塞式延时：检查是否超时
 * @param  start_tick: 起始滴答数（传入变量地址，首次调用前需赋值为Delay_GetTick()）
 * @param  ms: 超时时间（ms）
 * @retval 1: 超时，0: 未超时
 * @note   示例：
 *         uint32_t tick = Delay_GetTick();
 *         if (Delay_CheckTimeout(&tick, 1000)) { ... } // 1秒后执行
 */
uint8_t Delay_CheckTimeout(uint32_t *start_tick, uint32_t ms)
{
    if (Delay_GetTick() - *start_tick >= ms)
    {
        *start_tick = Delay_GetTick(); // 重置起始滴答数（可选）
        return 1;
    }
    return 0;
}


/*!
 * @brief       Configures Delay ms.
 *
 * @param       nms: Specifies the delay to be configured.
 *              This parameter can be one of following parameters:
 *              @arg nms
 *
 *
 * @retval      None
 */
void Delay_ms(__IO u32 nms)
{
    SysTick_Config(SystemCoreClock / 1000);

    cntMs = nms;
    while (cntMs != 0);
}

/*!
 * @brief       Configures Delay us.
 *
 * @param       nus: Specifies the delay to be configured.
 *              This parameter can be one of following parameters:
 *              @arg nus
 *
 *
 * @retval      None
 */
void Delay_us(__IO u32 nus)
{
    SysTick_Config(SystemCoreClock / 1000000);

    cntUs = nus;
    while (cntUs != 0);
}


