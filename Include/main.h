/*!
 * @file        main.h
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

/* Define to prevent recursive inclusion */
#ifndef __MAIN_H
#define __MAIN_H

/* Includes */


#include "stdio.h"
#include "string.h"

#include "apm32e10x_rcm.h"
#include "apm32e10x_gpio.h"
#include "apm32e10x_usart.h"
#include "apm32e10x_dma.h"
#include "apm32e10x_misc.h"
#include "apm32e10x_pmu.h"
#include "apm32e10x_fmc.h"
#include "apm32e10x_bakpr.h"
#include "apm32e10x_tmr.h"




#define VERSION "2.5.0.100\0"


#define FLASH_PAGE_SIZE 2048


void USART3_Isr(void);

/* ---- 极简格式化输出（App/boot_printf.c）----
 * 只支持 %s / %d / %u / %X / %02X / %lu / %08lX（+ %c / %%），
 * 替掉了 C 库的 printf 引擎（省约 1.1KB）。
 * 用宏重定向而不是直接定义 printf：直接定义时编译器会把 printf("字面量")
 * 优化成库的 puts，把整条 stdio 链又拉回来（实测链接期 printfa.o 重复定义）。 */
int  boot_printf(const char *format, ...);
int  boot_snprintf(char *str, uint32_t size, const char *format, ...);
#define printf    boot_printf
#define snprintf  boot_snprintf




// GPIO
typedef struct
{
	RCM_APB2_PERIPH_T 		clk;
	GPIO_T* 							port;
	uint16_t 							pin;
	GPIO_SPEED_T    			speed;
	GPIO_MODE_T     			mode;
}HW_GPIO_INIT_T;





void hw_gpio_init( HW_GPIO_INIT_T gpio_cfg);

/* 外部看门狗喂狗：PB0 电平翻转（与 APP 侧 hard_wdt_set 同语义）
 * 2026-09-16 改造：原由 TMR5 中断喂狗，改为长操作中主动喂——
 * 中断喂会导致主流程卡死时狗照样被喂，失去保护意义。 */
extern HW_GPIO_INIT_T Hw_WDT_Cfg;
#define WDT_FEED()   (Hw_WDT_Cfg.port->ODATA ^= Hw_WDT_Cfg.pin)

/* 2026-09-17：Jump_to_App() / JumpAddress / Jump_To_Application / pFunction
 * 全部删除 —— 新架构下跳 APP 由引导区负责，升级 boot 不再持有跳转实现。 */



#endif /* __MAIN_H */
