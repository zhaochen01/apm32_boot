
#include "hw_board.h"

// 温度传感器相关参数（参考数据手册校准值）
#define V25        1.46  // 25℃时电压(V)
#define AVG_SLOPE  3.7   // 温度系数(mV/℃)
#define VREF       3.25   // 参考电压(V)


static int ADC1_Init(void)
{
  ADC_Config_T            adcConfig;

  // 启用 ADC 时钟
  RCM_EnableAPB2PeriphClock(RCM_APB2_PERIPH_ADC1);
   /* ADCCLK = PCLK2/4 */
  RCM_ConfigADCCLK(RCM_PCLK2_DIV_6);
  
  /* ADC configuration */
  ADC_Reset(ADC1);
//  ADC_ConfigStructInit(&adcConfig);
  adcConfig.mode                  = ADC_MODE_INDEPENDENT;
  adcConfig.scanConvMode          = DISABLE;
  adcConfig.continuosConvMode     = DISABLE;
  adcConfig.externalTrigConv      = ADC_EXT_TRIG_CONV_None;
  adcConfig.dataAlign             = ADC_DATA_ALIGN_RIGHT;  // 数据右对齐
  
  /* channel number */
  adcConfig.nbrOfChannel          = 1;
  
  ADC_Config(ADC1, &adcConfig);

  // 配置温度传感器通道(不同型号可能有差异，请确认手册)
  ADC_EnableTempSensorVrefint(ADC1); 
  
  /* Enable ADC */
  ADC_Enable(ADC1);
  
    // ADC校准
  /* Enable ADC1 reset calibration register */
  ADC_ResetCalibration(ADC1);
  
  /* Check the end of ADC1 reset calibration register */
  while (ADC_ReadResetCalibrationStatus(ADC1));

  /* Start ADC1 calibration */
  ADC_StartCalibration(ADC1);
  
  /* Check the end of ADC1 calibration */
  while (ADC_ReadCalibrationStartFlag(ADC1));
  
  return 0;
}
INIT_DEVICE_EXPORT(ADC1_Init);


uint16_t get_adc_value(uint8_t ch)
{
  ADC_ClearIntFlag(ADC1, ADC_FLAG_EOC);
  
  ADC_ConfigRegularChannel(ADC1, ch, 1, ADC_SAMPLETIME_239CYCLES5 ); //ADC_SAMPLETIME_13CYCLES5
  
  // 启动 ADC 转换
  /* Start ADC1 Software Conversion */
  ADC_EnableSoftwareStartConv(ADC1);

  // 等待转换完成
  while (!ADC_ReadStatusFlag(ADC1, ADC_FLAG_EOC))
  {
    rt_thread_mdelay(1);
  }
  
  return ADC_ReadConversionValue(ADC1); // 读取转换结果
}


 //获取通道ch的转换值
//取times次,然后平均
uint16_t T_Get_Adc_Average(uint8_t ch,uint8_t times)
{
	uint32_t temp_val=0;
	uint8_t t;
	for(t=0; t < times; t++)
	{
		temp_val += get_adc_value(ch);
		rt_thread_delay(5);
	}
	return temp_val / times;
} 


uint16_t Get_Temperature(void)
{
  uint32_t adc_value = T_Get_Adc_Average(ADC_CHANNEL_TEMP_SENSOR, 20);	//读取通道16,20次取平均
  
  // 转换为电压值（假设VREF=3.3V）
  double voltage = (float)adc_value * VREF / 4096.0f;

  // 计算温度（公式来自数据手册）
  double temperature = ((((V25 - voltage) * 1000) / AVG_SLOPE) + 25.0f);

  return (uint16_t)(temperature *= 100);
}
