#ifndef __IAP_H__
#define __IAP_H__

#include "stdint.h"

#define UPGRADE_ADDR       0x807E000
#define UPGRADE_CFG_ADDR   0x807E800
#define APP_ADDR           0x8006000
#define UP_FILE_ADDR_1     0x802A000   /* 0x6000 + 144KB (0x24000) */
#define UP_FILE_ADDR_2     0x804E000   /* 0x2A000 + 144KB (0x24000) */



typedef struct
  {
  uint32_t    size;           // �ļ���С
  uint32_t    crc;            // �ļ�crcУ��ֵ
} Firmware;


// UPGRADE
#pragma pack(push, 1) // push���浱ǰ�������ã�1��ʾ��1�ֽڶ��� __packed  
typedef struct
  {
  uint8_t     flag;           // 5A:���ñ�־
  uint8_t     up_count;       // ������������
  uint8_t     fw_flag;        // �̼���־��ָʾ�̼��ı�ʶ
  uint8_t     status;         // �̼�״̬
  uint32_t    size;           // �ļ���С
  uint32_t    crc;            // �ļ�crcУ��ֵ
  char        url[128];       // ���ص�ַ
  char        name[64];       // �ļ�����
  Firmware    fw[3];          // �ļ�У����Ϣ 0��app 1���̼�1  2���̼�2
//  uint32_t data_crc;            // ���ṹ�����ݵ�crcУ��ֵ
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
  
#endif