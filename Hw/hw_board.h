#ifndef __HW_BOARD_H__
#define __HW_BOARD_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apm32e10x_gpio.h"
#include "apm32e10x_usart.h"
#include "apm32e10x_rcm.h"
//#include "apm32e10x_dma.h"
#include "apm32e10x_misc.h"
#include "apm32e10x_rtc.h"
#include "apm32e10x_pmu.h"
//#include "apm32e10x_adc.h"
#include "apm32e10x_fmc.h"
//#include "apm32e10x_bakpr.h"


// GPIO
typedef struct
{
	RCM_APB2_PERIPH_T 		clk;
	GPIO_T* 							port;
	uint16_t 							pin;
	GPIO_SPEED_T    			speed;
	GPIO_MODE_T     			mode;
}HW_GPIO_INIT_T;



extern void hw_gpio_init( HW_GPIO_INIT_T gpio_cfg);
  
// adc
uint16_t Get_Temperature(void);


// led

#define LED_ENABLE_LEVEL   					0
#define HW_LED_BLINK_ENABLE					1

#define LED_RUN  		0X01
#define LED_DATA		0X02
#define LED_SIM  		0X04
#define LED_GNSS		0X08

typedef enum {
	Hw_Led_On,
	Hw_Led_Off,
	Hw_Led_Reverse,
}Hw_Led_Cntl_T;

void hw_led_control(uint8_t leds, Hw_Led_Cntl_T cntl);
void hw_led_blink(uint8_t leds, uint32_t on, uint32_t off);




// UPGRADE
#pragma pack(push, 1) // push保存当前对齐设置，1表示按1字节对齐 __packed  
typedef struct{
  uint8_t     flag;
  uint8_t     up_flag;
  uint16_t    test;
  uint32_t    size;
  uint32_t    crc;
  char        url[64];
}upgrade;
#pragma pack(pop)     // 恢复之前的对齐设置



/* 自定义不定长消息结构体 */
typedef struct{
    uint16_t len;       /* 消息长度 */
    uint8_t data[0];     /* 柔性数组，存储不定长数据 */
}msg_packet;




#endif
