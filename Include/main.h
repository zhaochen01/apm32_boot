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
#include "apm32e10x_adc.h"
#include "apm32e10x_fmc.h"
#include "apm32e10x_bakpr.h"
#include "apm32e10x_tmr.h"




#define VERSION "1.3.2.100\0"


#define FLASH_PAGE_SIZE 2048


void USART1_Isr(void);
void USART3_Isr(void);




// GPIO
typedef struct
{
	RCM_APB2_PERIPH_T 		clk;
	GPIO_T* 							port;
	uint16_t 							pin;
	GPIO_SPEED_T    			speed;
	GPIO_MODE_T     			mode;
}HW_GPIO_INIT_T;





typedef  void (*pFunction)(void);

/* extern variables */
extern volatile uint32_t tick;
extern pFunction Jump_To_Application;
extern uint32_t JumpAddress;

void hw_gpio_init( HW_GPIO_INIT_T gpio_cfg);
void Jump_to_App(uint32_t address);



#endif /* __MAIN_H */
