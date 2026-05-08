#ifndef __IAP_H__
#define __IAP_H__

#include "stdint.h"

#define UPGRADE_ADDR       0x807E000
#define UPGRADE_CFG_ADDR   0x807E800
#define APP_ADDR           0x8006000
#define UP_FILE_ADDR_1     0x8020000
#define UP_FILE_ADDR_2     0x8040000



typedef struct
  {
  uint32_t    size;           // 文件大小
  uint32_t    crc;            // 文件crc校验值
} Firmware;


// UPGRADE
#pragma pack(push, 1) // push保存当前对齐设置，1表示按1字节对齐 __packed  
typedef struct
  {
  uint8_t     flag;           // 5A:可用标志
  uint8_t     up_count;       // 升级次数计数
  uint8_t     fw_flag;        // 固件标志，指示固件的标识
  uint8_t     status;         // 固件状态
  uint32_t    size;           // 文件大小
  uint32_t    crc;            // 文件crc校验值
  char        url[128];       // 下载地址
  char        name[64];       // 文件名称
  Firmware    fw[3];          // 文件校验信息 0：app 1：固件1  2：固件2
//  uint32_t data_crc;            // 本结构体数据的crc校验值
}Upgrade_t;
#pragma pack(pop)     // 恢复之前的对齐设置


typedef struct
{
  uint32_t config_addr;
  uint32_t app_addr;
  uint32_t fw1_addr;
  uint32_t fw2_addr;
  uint32_t data_crc;            // 本结构体数据的crc校验值
}Upgrade_cfg_t;


extern void Jump_to_App(uint32_t address);
extern Upgrade_t upgrade;
extern Upgrade_cfg_t upgrade_cfg;
extern int32_t iap_init(void);
extern void iap_upgrade_state_save( uint8_t flag, uint8_t count, uint8_t exit);

extern int32_t hw_flash_erase(uint32_t addrStart, uint32_t size);
extern int32_t hw_flash_write(uint32_t addrStart, const void *pdata, uint32_t size);
extern int32_t hw_flash_read(uint32_t addrStart, void *pdata, uint32_t size);
  
#endif