

#include "hw_board.h"


Board board_info;
const Board *board_temp;

// WDT 看门狗 
HW_GPIO_INIT_T Hw_WDT_Cfg = { RCM_APB2_PERIPH_GPIOB, GPIOB, GPIO_PIN_0, GPIO_SPEED_10MHz, GPIO_MODE_OUT_PP };


  
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



static void thread_entry(void* parameter)
{

  while(1)
  {
//    Hw_WDT_Cfg.port->ODATA ^= Hw_WDT_Cfg.pin;
    rt_thread_delay(500);
  }
}

static void _timeout_handler(void *parameter)
{
  Hw_WDT_Cfg.port->ODATA ^= Hw_WDT_Cfg.pin;
}


void board_info_save(void)
{
    board_info.Flag = 0x5A;
    hw_flash_erase(BOARD_INFO_ADDR, FLASH_PAGE_SIZE);
    hw_flash_write(BOARD_INFO_ADDR, &board_info, sizeof(Board));
}



/**
 * @brief 任意长度字节数组转无分隔符十六进制字符串（大写）
 * @param src       输入：待转换的字节数组（unsigned char/uint8_t类型）
 * @param src_len   输入：字节数组长度（≥1）
 * @param dest      输出：转换后的字符串缓冲区（需提前分配足够空间）
 * @param uppercase 输入：是否大写（1=大写，0=小写）
 * @return 成功返回转换后字符串的有效长度（不含结束符），失败返回-1
 * @note 缓冲区大小计算公式：src_len * 2 + 1（例如：10字节需21字节缓冲区）
 */
int hex_array_to_str(const uint8_t *src, size_t src_len, char *dest, int uppercase)
{
    // 入参合法性检查：空指针/长度为0直接返回失败
    if (src == NULL || dest == NULL || src_len == 0) {
        return -1;
    }
 
    // 十六进制字符表（大写/小写二选一）
    const char hex_upper[] = "0123456789ABCDEF";
    const char hex_lower[] = "0123456789abcdef";
    const char *hex_chars = uppercase ? hex_upper : hex_lower;

    // 遍历每个字节，转换为2位十六进制字符
    for (size_t i = 0; i < src_len; i++) {
        // 高4位转换（右移4位后取低4位）
        dest[2 * i]     = hex_chars[(src[i] >> 4) & 0x0F];
        // 低4位转换（直接取低4位）
        dest[2 * i + 1] = hex_chars[src[i] & 0x0F];
    }

    // 字符串末尾添加结束符（必须）
    dest[2 * src_len] = '\0';

    // 返回有效字符长度（不含结束符）
    return (int)(2 * src_len);
}



static int hw_board_init(void)
{

  extern int net_info_init(uint8_t flag);
  
  board_temp = ( Board *)BOARD_INFO_ADDR;
  
  
//  hw_flash_read(BOARD_INFO_ADDR, board_temp, sizeof(Board));
  
  if(board_temp->Flag == 0x5A)
  {
    if(board_temp->code_id != SYSTEM_CODE_ID)  goto Update_System_Info;
    
    if(board_temp->SW_ver[0] != 0xff)
      rt_memcpy(&board_info, board_temp, sizeof(Board));
    
    net_info_init(0);
  }
  else
  {

Update_System_Info:
    
    rt_memset(&board_info, '\0', sizeof(Board));
    
    
    net_info_init(1);
    
    if((board_temp->Flag == 0x5A || board_temp->Flag == 0x3A) && *((uint8_t *)(BOARD_INFO_ADDR + 12)) != 0xFF)
   {
//     hex_array_to_str((uint8_t *)board_temp->SN, 6, board_info.SN, 1);
     rt_strncpy(board_info.SN, board_temp->SN, 12);
   }
   else
   {
      uint32_t cpu_id[3];
      cpu_id[0] = *((uint32_t *)0x1FFFF7E8);
      cpu_id[1] = *((uint32_t *)0x1FFFF7EC);
      cpu_id[2] = *((uint32_t *)0x1FFFF7EF);
      uint32_t cpu_id_crc = crc32((uint8_t *)cpu_id, 12);   
      rt_sprintf(board_info.SN, "E400%04X\0", cpu_id_crc );
   }     
   
    rt_strcpy(board_info.Name,  DEVICE_NAME);
    rt_strcpy(board_info.Model, DEVICE_MODEL);
//    rt_strncpy(board_info.SN, board_temp->SN, 15); 
    

    
 //   rt_snprintf(board_info.SN, 16, "123456789012\0");
    

    board_info.code_id = SYSTEM_CODE_ID;
    
   if(board_temp->Flag == 0x5A ) board_info.lift = board_temp->lift;
   else board_info.lift = 24;
    
#if( SW_VERSION_O == 0)
  rt_sprintf(board_info.SW_ver,"%d.%d.%d.%d\0", SW_VERSION_X, SW_VERSION_Y, SW_VERSION_Z, SW_VERSION_P);
#else
  rt_sprintf(board_info.SW_ver,"%d.%d.%d.%d-%d\0", SW_VERSION_X, SW_VERSION_Y, SW_VERSION_Z, SW_VERSION_P, SW_VERSION_O);
#endif

#if( HW_VERSION_O == 0)
  rt_sprintf(board_info.HW_ver,"%d.%d.%d.%d\0", HW_VERSION_X, HW_VERSION_Y, HW_VERSION_Z, HW_VERSION_P);
#else 
  rt_sprintf(board_info.HW_ver,"%d.%d.%d.%d-%d\0", HW_VERSION_X, HW_VERSION_Y, HW_VERSION_Z, HW_VERSION_P, HW_VERSION_O);
#endif
  }
  
  rt_kprintf("\r\nSystem Time Zone:%d\r\n", timezone = board_info.Utc);
  
  rt_kprintf("\r\nSoftware Version: %s\r\n", board_info.SW_ver);
  
  rt_kprintf("\r\nHardware Version: %s\r\n", board_info.HW_ver);
  
  rt_kprintf("\r\nDevice Name: %s\r\n", board_info.Name);
    
  rt_kprintf("\r\nDevice Model: %s\r\n", board_info.Model);
  
  rt_kprintf("\r\nDevice SN: %s\r\n", board_info.SN);
  
  board_info_save();
  
  hw_gpio_init(Hw_WDT_Cfg);
  Hw_WDT_Cfg.port->BC = Hw_WDT_Cfg.pin;
  
  rt_timer_t rt_timer  = rt_timer_create("WDT", _timeout_handler,\
																				RT_NULL, 500,\
																				RT_TIMER_FLAG_PERIODIC);
  // 启动定时器
  if (rt_timer != RT_NULL)
      rt_timer_start(rt_timer);		
                                    
  
//  rt_thread_t tid = rt_thread_create(  "SW-WDT",
//                            thread_entry, RT_NULL,
//                            256,
//                            2,
//                            10);
//  if (tid != RT_NULL)
//  {
//    rt_thread_startup(tid);
//    rt_kprintf("task -> SW-WDT ok!\r\n");
//  }
  
  
	return RT_EOK;
}
INIT_DEVICE_EXPORT(hw_board_init);
  
  

void reset_system(void)
{
    NVIC_SystemReset();

}

MSH_CMD_EXPORT(reset_system, rest system);



/**
 * @brief  设置MAC地址的带参数命令
 * @param  argc: 参数个数（包含命令名）
 * @param  argv: 参数数组，argv[1]为12位MAC字符串
 * @return RT_EOK成功，-RT_ERROR失败
 */
static int set_mac(int argc, char** argv)
{
    // 1. 检查参数个数
    if (argc != 2)
    {
        rt_kprintf("用法错误！正确用法：set_mac <12位MAC字符串>\n");
        rt_kprintf("示例：set_mac 4EXC28JRHQ57\n");
        return -RT_ERROR;
    }

    // 2. 检查MAC字符串长度是否为12位
    char* mac_str = argv[1];
    if (strlen(mac_str) != 12)
    {
        rt_kprintf("错误：MAC地址必须是12位字符串！当前长度：%d\n", strlen(mac_str));
        return -RT_ERROR;
    }

    // 3. （可选）校验MAC字符串的字符合法性（按需启用，例如允许字母数字）
//    for (int i = 0; i < 12; i++)
//    {
//        if (!isalnum(mac_str[i])) // 检查是否为字母/数字（需包含ctype.h）
//        {
//            rt_kprintf("错误：MAC地址包含非法字符！位置%d：%c\n", i+1, mac_str[i]);
//            return -RT_ERROR;
//        }
//    }

    // 4. 保存MAC地址（实际项目中可添加写入硬件/配置文件的逻辑）
    rt_memcpy(board_info.SN, mac_str, 12);
    board_info_save();
    
    // 5. 格式化输出（添加分隔符，符合MAC地址显示习惯）
//    char formatted_mac[18] = {0}; // 存储带分隔符的MAC（如XX:XX:XX:XX:XX:XX）
//    for (int i = 0; i < 12; i += 2)
//    {
//        rt_snprintf(&formatted_mac[i/2*3], 4, "%c%c:", mac_str[i], mac_str[i+1]);
//    }
//    formatted_mac[17] = '\0'; // 移除最后一个冒号

    // 6. 反馈结果
    rt_kprintf("MAC set ok!\n");
    rt_kprintf("mac：%s\n", board_info.SN);
//    rt_kprintf("格式化MAC：%s\n", formatted_mac);

    return RT_EOK;
}

// 导出命令到FinSH
MSH_CMD_EXPORT(set_mac, set mac:set_mac <12B char>);










void hex_array_to_string(const uint8_t *data, size_t len, char *output)
{
    const char hex_chars[] = "0123456789ABCDEF"; // 十六进制字符映射表
    
    for (size_t i = 0; i < len; ++i) {
        output[i*3]     = hex_chars[(data[i] >> 4) & 0x0F];
        output[i*3 + 1] = hex_chars[data[i] & 0x0F];
        
        // 最后一个字节后不添加空格
        if (i < len - 1) {
            output[i*3 + 2] = ' ';
        }
    }
    output[len*3 - 1] = '\0'; // 正确设置结束符位置
}


void printfHEX( void  *data, uint16_t size)
{
  
  uint8_t *pbuff = rt_malloc(1024);
  if(pbuff == RT_NULL)
  {
    rt_kprintf("rt_malloc init error!\r\n");
    return;
  }
  
//  if(size > 256) size = 256;
  
  hex_array_to_string((uint8_t *)data, size, (char*)pbuff);
  
  rt_kprintf("hex:%s\r\n", pbuff);
  rt_free(pbuff);
}

