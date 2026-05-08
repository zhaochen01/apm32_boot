
#include "hw_board.h"


typedef struct
{
  HW_GPIO_INIT_T        gpio;
	
	char 									name[16];
	uint16_t 							time[3];
	void 									(*timeout_handler)(void *parameter);
	rt_timer_t 						rt_timer;
}Hw_Led_Config_T;




static void led_timeout_handler(void *parameter);


Hw_Led_Config_T led_cfg_list[] = 
{
	{
		RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_6, GPIO_SPEED_50MHz, GPIO_MODE_OUT_PP,\
		"led_run", 100, 100, 0, led_timeout_handler, NULL
	},
	
	{
		RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_7, GPIO_SPEED_50MHz, GPIO_MODE_OUT_PP,\
		"led_data", 100, 100, 0, led_timeout_handler, NULL
	},
		
	{
		RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_8, GPIO_SPEED_50MHz, GPIO_MODE_OUT_PP,\
		"led_sim", 100, 100, 0, led_timeout_handler, NULL
	},
		
	{
		RCM_APB2_PERIPH_GPIOC, GPIOC, GPIO_PIN_9, GPIO_SPEED_50MHz, GPIO_MODE_OUT_PP,\
		"led_gnss", 100, 100, 0, led_timeout_handler, NULL
	},
};

#define HW_LED_NUM          (sizeof(led_cfg_list) / sizeof( Hw_Led_Config_T ))
	
static inline void _hw_led_on(Hw_Led_Config_T *led);
static inline void _hw_led_off(Hw_Led_Config_T *led);
static inline void _hw_led_reverse(Hw_Led_Config_T *led);

static int hw_led_init(void)
{
	/* Enable GPIO clock */
	GPIO_Config_T  configStruct;
	Hw_Led_Config_T *led;
	for(uint8_t i = 0; i < HW_LED_NUM; i++)
	{
		led = &led_cfg_list[i];
    /* Enable the GPIO_LED Clock */
    RCM_EnableAPB2PeriphClock(led->gpio.clk);

    /* Configure the GPIO_LED pin */
    configStruct.pin 		= led->gpio.pin;
    configStruct.mode 	= led->gpio.mode;
    configStruct.speed 	= led->gpio.speed;

    GPIO_Config(led->gpio.port, &configStruct);
	
		// 关闭led
		_hw_led_off(led);
	
	 led->rt_timer = rt_timer_create(led->name, led->timeout_handler,\
																				led, led->time[0],\
																				RT_TIMER_FLAG_ONE_SHOT);
		// 启动定时器
//		if (led->rt_timer != RT_NULL)
//				rt_timer_start(rt_timer);		
	}
	
	rt_kprintf("led init ok!\r\n");
	
	return RT_EOK;
}
INIT_DEVICE_EXPORT(hw_led_init);


void hw_led_control(uint8_t leds, Hw_Led_Cntl_T cntl)
{
  int8_t     i;  
  uint8_t   bit;
  Hw_Led_Config_T *led;
  
  bit = 0x01;
  
  for (i = 0; i < HW_LED_NUM; i++)
  {
    if (leds & bit)
    {      
      led = &led_cfg_list[i];
      
      switch (cntl)
      {     
				case Hw_Led_On:
				default:
					_hw_led_on(led);
					break;
					
				case Hw_Led_Off:
					_hw_led_off(led);
					break;
					
				case Hw_Led_Reverse:
					_hw_led_reverse(led);
					break;
      }
		}
    bit <<= 1;
  }
}


void hw_led_blink(uint8_t leds, uint32_t on, uint32_t off)
{
#if (HW_LED_BLINK_ENABLE)  
  uint8_t     i;  
  uint8_t   bit;
  Hw_Led_Config_T *led;
  
  bit = 0x01;
  
  for (i = 0; i < HW_LED_NUM; i++)
  {
    if (leds & bit)
    {      
      led = &led_cfg_list[i];
      
      led->gpio.port->BC = led->gpio.pin;  
      
      led->time[2] = on;
      led->time[0] = on;
      led->time[1] = off;
      
      rt_timer_control(led->rt_timer, led->time[0], RT_TIMER_CTRL_SET_TIME);
      rt_timer_start(led->rt_timer);
    }
    
    bit <<= 1;
  }
#endif
}

static inline void _hw_led_on(Hw_Led_Config_T *led)
{
#if (LED_ENABLE_LEVEL == 0)
  led->gpio.port->BC = led->gpio.pin;
#else
	led->port->BSC = led->pin;
#endif
}

static inline void _hw_led_off(Hw_Led_Config_T *led)
{
#if (LED_ENABLE_LEVEL == 1)
  led->port->BC = led->pin;
#else
	led->gpio.port->BSC = led->gpio.pin;
#endif
}





static inline void _hw_led_reverse(Hw_Led_Config_T *led)
{
	led->gpio.port->ODATA ^= led->gpio.pin;
}


#if (HW_LED_BLINK_ENABLE)
static void led_timeout_handler(void *parameter)
{
  Hw_Led_Config_T *led;
  uint16_t index = 0; 
  
  RT_ASSERT(parameter);
  
  led = (Hw_Led_Config_T *)parameter;  
  
	_hw_led_reverse(led);

	if(led->time[2] == led->time[0]) 	led->time[2] = led->time[1];
	else 							 								led->time[2] = led->time[0];
	
	rt_timer_control(led->rt_timer, RT_TIMER_CTRL_SET_TIME, &led->time[2]);
	rt_timer_start(led->rt_timer);

}
#endif





