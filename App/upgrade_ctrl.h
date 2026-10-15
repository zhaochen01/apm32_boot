#ifndef _UPGRADE_CTRL_H
#define _UPGRADE_CTRL_H

#include "stdint.h"

/* retry and timing configuration */
#define UPGRADE_MAX_DL_RETRIES     2U    /* whole-file download: 1st attempt + 1 retry */
#define UPGRADE_MAX_APP_RETRIES    5U    /* backup-to-run copy retries */
#define UPGRADE_QFREAD_TIMEOUT_MS  10000U
#define UPGRADE_QFREAD_RECOVER_MS  5000U /* drain wait after QFREAD failure */
#define UPGRADE_FILE_NAME          "UFS:iot_update.bin"

/* one firmware chunk (raw receive buffer size) */
#define LTE_READ_CHUNK_SIZE        40960U

/* ---- 升级 boot 槽（与引导区 App/boot.h、tools/postbuild_boot_slot.py 三处一致）----
 * 尾部布局：crc@slot_size-12 / size@slot_size-8
 * ⚠ 引导区、构建脚本、本文件任何一处改动都必须同时改另外两处 */
#define BOOT_SLOT_SIZE        24576u                  /* 24KB */
#define BOOT_SLOT_MAX_IMAGE   (BOOT_SLOT_SIZE - 12u)  /* 24564：尾部 12B 留给 crc/size */
#define BOOT_A_ADDR           0x08003000u
#define BOOT_B_ADDR           0x08009000u

/* boot 自升级用 UFS 文件名 —— 与 APP 的 iot_update.bin 区分，两者可共存 */
#define BOOT_UPDATE_FILE_NAME "UFS:boot_update.bin"

/* boot 试用计数所在的 BAKPR（引导区读写，APP 清零；升级 boot 写入置 1） */
#define BOOT_TRIAL_REG        BAKPR_DATA17

/* ---- 串口下载请求标志：引导区写、升级 boot 读后清零（2026-09-18）----
 * 上位机在上电时发 0x55 0xAA（见 apm32_e103_bootstrap/App/boot_serial.h），
 * 引导区命中后置本标志再跳升级 boot，boot 据此直接进串口下载模式，
 * 而不是走 4G 模组那条路。三个工程用的是同一个寄存器与同一个魔数。
 *
 * 为什么不写 Upgrade_t.flag：引导区是"Flash 只读"的信任根，写 Flash 会破坏
 * 这个边界。BAKPR 它本来就在写（试用计数）。
 *
 * 【为什么是 DATA18】DATA1~DATA17 已被占满（DATA2~DATA16 = APP 崩溃转储、
 * DATA17 = boot 试用计数）。曾经选 DATA16，撞上了 APP 的 CRASH_REG_R1_H：
 * 每次 HardFault 写 r1>>16，约 1/65536 概率等于 0x5A5A，凭空造出一个串口
 * 请求并且没人清。加寄存器前先全工程 grep 一遍 BAKPR_DATA。
 * 三处必须一致：本处 / apm32_e103_bootstrap/App/boot.h / 各 APP 侧的 app_ota.c
 * （IoT-4G-v2、Collector-4G 两个产品共用这一套 boot，宏值必须同值） */
#define BOOT_SERIAL_REQ_REG    BAKPR_DATA18
#define BOOT_SERIAL_REQ_MAGIC  0x5A5Au

typedef enum
{
  UPGRADE_ACTION_NETWORK  = 0x3A,   /* download from 4G module */
  UPGRADE_ACTION_SELF     = 0x3B,   /* boot 自升级：UFS 的新升级 boot 镜像 -> A 槽 */
  UPGRADE_ACTION_SERIAL   = 0x3C,   /* 串口下载：USART1 收镜像 -> APP A 槽 */
  UPGRADE_ACTION_ROLLBACK = 0xAA    /* copy backup region to run region */
} UPGRADE_ACTION_T;

/* upgrade parameters; caller fills before calling upgrade_run() */
typedef struct
{
  UPGRADE_ACTION_T action;
  uint32_t         run_addr;       /* run region base (APP_ADDR) */
  uint32_t         backup_addr;    /* backup region base (UP_BACKUP_ADDR) */
  uint32_t         expect_size;    /* expected firmware size */
  uint32_t         expect_crc;     /* expected CRC32 */
  uint32_t         serial_baud;    /* 串口下载：请求行里协商的波特率；
                                    * 0 或 115200 = 不切（缺省行为） */
} UPGRADE_CTX_T;

extern uint32_t CRC32(uint8_t *data, uint32_t length);

/* run the upgrade; never returns (saves state and resets the MCU) */
void upgrade_run(const UPGRADE_CTX_T *ctx);

#endif /* _UPGRADE_CTRL_H */
