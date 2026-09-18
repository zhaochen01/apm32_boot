#ifndef __IAP_H__
#define __IAP_H__

#include "stdint.h"
#include "upgrade_ctrl.h"   /* UPGRADE_CTX_T / UPGRADE_ACTION_T（仅依赖 stdint.h，无环） */

#define UPGRADE_ADDR       0x807E000
#define UPGRADE_CFG_ADDR   0x807E800
#define APP_ADDR           0x800F000   /* APP A 运行槽: 0xF000 + 214KB */
#define UP_BACKUP_ADDR     0x8044800   /* APP B 备份槽: 0x44800 + 214KB */

/* 单区容量上限 (224KB): 固件大小必须 <= 该值 */
#define APP_MAX_SIZE       (0x35800U)  /* 每个 APP 槽 214KB */

/* ---- 槽容量与 RAM 边界：必须与引导区 apm32_e103_bootstrap/App/boot.h 一致 ----
 * 对应关系：BOOT_RAM_BASE/BOOT_RAM_TOP <-> APP_RAM_BASE/APP_RAM_TOP
 *           BOOT_APP_SIZE <-> APP_MAX_SIZE (两者均为 0x35800)
 * 升级 boot 的槽判据 slot_usable() 与引导区 boot_app_valid() 必须逐条同构；
 * 两边一旦分歧，IAP_ACT_RESUME_APP 的复位就会变成无限复位环。 */
#define APP_RAM_BASE       0x20000000U
#define APP_RAM_TOP        0x2001F800U

/* ---- Upgrade_t.status：升级结果回传（2026-09-17 取代 trial 自动回退） ----
 * 写者：升级 boot。读者：APP（下次启动读走 -> 上报云端 -> 清 0）。
 *
 * 注意：APP 判定升级结果只能看 status，不能看 flag ——
 *   IoT-4G-v2/Hw/hw_board.c 的 hw_board_init() 一旦发现 flag==0xFF 就整页
 *   擦写改成 0x5A，于是 abort 与安装成功在 flag 上完全不可区分。 */
#define UPGRADE_STATUS_NONE   0x00   /* 无结果：初始态 / 已被 APP 消费 */
#define UPGRADE_STATUS_OK     0x02   /* 安装成功  -> 上行 state=3 */
#define UPGRADE_STATUS_FAILED 0x03   /* 安装失败  -> 上行 state=4 */
                                     /*  （含 abort 未实施、以及已回退两种） */

/* 元数据中的固件信息有效判断 (Flash 空白为 0xFFFFFFFF) */
#define FW_VALID(fw) (((fw)->size > 0U) && ((fw)->size != 0xFFFFFFFFU) && \
                      ((fw)->crc != 0xFFFFFFFFU) && ((fw)->size <= APP_MAX_SIZE))

/* ---- iap_init 返回值：只表示"要做什么"，参数由 ctx 带回 ----
 *
 * 铁律（写进 iap.c 顶部注释）：
 *   复位只允许发生在"持久状态已改变"之后——不存在裸复位。
 *   旧码 IAP_RSLT_APP_OK(0x1000) / UPGRADE_ACTION_AUTO_ROLLBACK(0x5B) 已删除，
 *   不允许复用：它们在旧架构下意味着"自己跳 APP"，在新架构下就是复位环的环源。 */
#define IAP_ACT_NONE        0   /* 无事可做：main 走 UFS 兜底探测 -> 失败则 park */
#define IAP_ACT_INSTALL     1   /* 执行安装：ctx 已填好 -> upgrade_run() */
#define IAP_ACT_RESUME_APP  2   /* APP 有效、flag 陈旧：flag 已改写，main 复位交回引导区 */
#define IAP_ACT_PARK        3   /* 判据漂移等异常：main 直接 park，跳过 UFS 探测、绝不复位 */



typedef struct
  {
  uint32_t    size;           // �ļ���С
  uint32_t    crc;            // �ļ�crcУ��ֵ
} Firmware;


// UPGRADE
#pragma pack(push, 1) // push���浱ǰ�������ã�1��ʾ��1�ֽڶ��� __packed  
typedef struct
  {
  uint8_t     flag;           // 3A=升级请求 5A=无请求  AA=回退请求 FF=空白
  uint8_t     up_count;       // 废弃(trial 已取消), 恒 0
  uint8_t     fw_flag;        // 保留字段, 固定为 1
  uint8_t     status;         // 升级结果: 0x00=无 0x02=成功 0x03=失败 (UPGRADE_STATUS_*)
  uint32_t    size;           // 文件大小
  uint32_t    crc;            // 文件crc校验值
  char        url[128];       // 下载地址
  char        name[64];       // 文件名
  Firmware    fw[2];          // 固件校验信息 0=APP A 槽  1=APP B 槽
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


/* Jump_to_App() 已于 2026-09-17 删除：新架构下跳 APP 由引导区负责，
 * 升级 boot 只做"装 APP / 修 APP"，干完 NVIC_SystemReset() 交回。
 * 留着它就等于在同一个文件里放着第二套跳转实现，日后接回去就绕过了引导区的决策。 */
extern Upgrade_t upgrade;
extern Upgrade_cfg_t upgrade_cfg;
extern int32_t iap_init(UPGRADE_CTX_T *ctx);
extern void iap_upgrade_state_save(uint8_t flag, uint8_t status);
extern void iap_state_save(void);   /* 只落盘、不复位（备份成功后立即固化 fw[1]） */
/* 向量表 sanity（MSP 落 RAM / 复位向量落 [addr,end] / Thumb 位）。
 * 安装提交前也要用它 —— 专治固件与槽地址不匹配（CRC 自洽但跑不起来）。
 * 三个调用方必须口径一致：本处、iap.c 的槽判据、引导区 boot_app_valid()。 */
extern int32_t iap_vector_ok(uint32_t addr, uint32_t end);

extern int32_t hw_flash_erase(uint32_t addrStart, uint32_t size);
extern int32_t hw_flash_write(uint32_t addrStart, const void *pdata, uint32_t size);
extern int32_t hw_flash_copy(uint32_t srcAddr, uint32_t dstAddr, uint32_t size);
  
#endif