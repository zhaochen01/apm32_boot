#include "bsp_delay.h"


volatile uint32_t g_sys_tick = 0; // 系统滴答数（ms级）

/* The count of milliseconds */
__IO uint32_t cntMs = 0;


/**
 * @brief  SysTick中断服务函数（必须添加到中断向量表）
 * @retval 无
 */
void SysTick_Handler(void)
{
    g_sys_tick++; // 每1ms加1
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
    cntMs = nms;
    while (cntMs != 0);
}
