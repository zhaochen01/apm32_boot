/*!
 * @file        bsp_delay.h
 *
 * @brief       Header for bsp_delay.c module
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

/* Define to prevent recursive inclusion */
#ifndef _BSP_DELAY_H
#define _BSP_DELAY_H

#ifdef __cplusplus
  extern "C" {
#endif

/* Includes ***************************************************************/
#include "main.h"

/* Exported typedef *******************************************************/

// 初始化延时函数
void Delay_Init(void);

// 阻塞式毫秒延时
void Delay_ms(uint32_t ms);

// 非阻塞式延时：检查是否超时（返回1表示超时，0表示未超时）
uint8_t Delay_CheckTimeout(uint32_t *start_tick, uint32_t ms);

// 获取当前系统滴答数（ms级）
uint32_t Delay_GetTick(void);



extern volatile uint32_t g_sys_tick;

#ifdef __cplusplus
}
#endif

#endif
