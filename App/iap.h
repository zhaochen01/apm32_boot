#ifndef __IAP_H__
#define __IAP_H__

#include "stdint.h"

#define UPGRADE_ADDR       0x807E000
#define UPGRADE_CFG_ADDR   0x807E800
#define APP_ADDR           0x8006000   /* 运行区: 0x6000 + 224KB (0x38000) */
#define UP_BACKUP_ADDR     0x803E000   /* 保底区: 0x3E000 + 224KB (0x38000) */

/* 单区容量上限 (224KB): 固件大小必须 <= 该值 */
#define APP_MAX_SIZE       (0x38000U)

/* 升级状态 (Upgrade_t.status)
 * TRIAL: APP 请求升级时写入=请求启用 trial 回退; Boot 升级成功后保持,
 *        每次启动递减 up_count, 减到 0 未获 APP 确认 → 自动回退保底区
 * OK:    稳定运行; APP 自检通过后写入, Boot 回退成功后写入 */
#define UPGRADE_STATUS_TRIAL  0x01
#define UPGRADE_STATUS_OK     0x02
#define UPGRADE_TRIAL_LIMIT   3U     /* trial 启动尝试次数 */

/* 元数据中的固件信息有效判断 (Flash 空白为 0xFFFFFFFF) */
#define FW_VALID(fw) (((fw)->size > 0U) && ((fw)->size != 0xFFFFFFFFU) && \
                      ((fw)->crc != 0xFFFFFFFFU) && ((fw)->size <= APP_MAX_SIZE))

/* iap_init 返回值: 启动校验失败或 trial 耗尽, main 需从保底区恢复 */
#define UPGRADE_ACTION_AUTO_ROLLBACK 0x5B



typedef struct
  {
  uint32_t    size;           // �ļ���С
  uint32_t    crc;            // �ļ�crcУ��ֵ
} Firmware;


// UPGRADE
#pragma pack(push, 1) // push���浱ǰ�������ã�1��ʾ��1�ֽڶ��� __packed  
typedef struct
  {
  uint8_t     flag;           // 5A:有效/跳转  3A:网络升级   AA:本地回退   FF:首次启动
  uint8_t     up_count;       // trial 启动尝试计数 (Boot 维护)
  uint8_t     fw_flag;        // 保留字段, 固定为 1
  uint8_t     status;         // 升级状态: 0x01=TRIAL  0x02=OK
  uint32_t    size;           // 文件大小
  uint32_t    crc;            // 文件crc校验值
  char        url[128];       // 下载地址
  char        name[64];       // 文件名
  Firmware    fw[3];          // 固件校验信息 0=运行区  1=保底区  2=保留
//  uint32_t data_crc;            // 该结构体数据的crc校验值
}Upgrade_t;
#pragma pack(pop)     // �ָ�֮ǰ�Ķ�������


typedef struct
{
  uint32_t config_addr;
  uint32_t app_addr;
  uint32_t fw1_addr;
  uint32_t fw2_addr;
  uint32_t data_crc;            // ���ṹ�����ݵ�crcУ��ֵ
}Upgrade_cfg_t;


extern void Jump_to_App(uint32_t address);
extern Upgrade_t upgrade;
extern Upgrade_cfg_t upgrade_cfg;
extern int32_t iap_init(void);
extern void iap_upgrade_state_save( uint8_t flag, uint8_t count, uint8_t exit);

extern int32_t hw_flash_erase(uint32_t addrStart, uint32_t size);
extern int32_t hw_flash_write(uint32_t addrStart, const void *pdata, uint32_t size);
extern int32_t hw_flash_read(uint32_t addrStart, void *pdata, uint32_t size);
extern int32_t hw_flash_copy(uint32_t srcAddr, uint32_t dstAddr, uint32_t size);
  
#endif