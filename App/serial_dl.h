#ifndef _SERIAL_DL_H
#define _SERIAL_DL_H

/*
 * serial_dl —— 串口下载的【字节层】：USART1 收发 + 请求行解析 + BAKPR 请求标志。
 *
 * 职责边界（刻意划窄）：
 *   · 本文件只碰 USART1 和 BAKPR，**不碰 Flash、不链 stdio**（全程
 *     uart_send_data 发静态 ASCII，收页循环里一个 printf 都不许有）。
 *   · 状态机（preserve / 擦写 / CRC / 提交）在 upgrade_ctrl.c 的
 *     upgrade_serial_run()，那里才有 flash_*_with_wdt 这些 static 帮手。
 *
 * 触发链路（两个入口，都汇到 upgrade_ctrl.c 的 UPGRADE_ACTION_SERIAL）：
 *   A. 引导区上电侦听 0x55 0xAA 命中 -> 写 BAKPR 标志 -> 跳本 boot
 *      -> iap_init() 读标志 -> SERIAL
 *   B. 砖机（A/B 都不可用）-> main.c 的 UFS 探测之前开一个窗口等请求行
 */

#include <stdint.h>

/* ---- 请求行 ----
 *   "#DL <size> <crc32hex>\r\n"       例："#DL 121784 1a2b3c4d\r\n"
 *   size 十进制（≤8 位）、crc32 十六进制（≤8 位）。
 *
 * 上位机在上电【之前】就开始每 100ms 重发这一行（覆盖"操作员还没复位"
 * 的时序），所以设备侧必须容忍重复、串接和任意前置噪声 —— 用逐字节
 * 状态机而不是"收一整行再解析"，否则重发会把行缓冲灌满垃圾。 */
#define SERIAL_DL_REQ_PREFIX   "#DL "

/* ---- ACK/NAK ----
 * 刻意用控制字符而不是 'A'/'C'：设备日志本身是 ASCII，撞车即静默错位，
 * 而错位在按页 ACK 的协议里等于整包报废。0x06/0x15 本就是 ASCII 为此
 * 定义的（ACK / NAK），且不会出现在任何可见日志里。 */
#define SERIAL_DL_ACK   0x06u
#define SERIAL_DL_NAK   0x15u

/* ---- 超时 ----
 * 收一页（2048B @115200 约 180ms）给 3s：够慢速 PC 调度，又不至于让外部
 * WDT（约 1.8s）在等待中饿死 —— 接收循环里按 100ms 节流喂狗。 */
#define SERIAL_DL_RX_TIMEOUT_MS    3000u
/* 等请求行：操作员可能还没点"开始"，给足 30s。 */
#define SERIAL_DL_REQ_TIMEOUT_MS   30000u
/* 砖机兜底窗口（main.c 用）：没人来就继续走模组 UFS 探测。 */
#define SERIAL_DL_BOOT_WAIT_MS     8000u

/* 排空 RX FIFO（1 字节寄存器 + 移位寄存器）。带 guard —— PC 若在持续灌
 * 数据，"等到没有数据"这个条件永远不成立。 */
void    serial_dl_flush(void);

/* 非阻塞收一字节：有数据返回 0..255，无数据返回 -1。
 * 返回 int32_t 而不是 char/uint8_t：ARMCLANG 下 char 无符号，-1 会变 255，
 * 调用方的 `if (c < 0)` 静默失效（本项目踩过一次）。 */
int32_t serial_dl_getc(void);

/* 发静态 ASCII 串 / 单字节。返回 0 成功、-1 失败（TX 卡死，见 uart_send_data）。 */
int32_t serial_dl_send(const char *s);
int32_t serial_dl_send_byte(uint8_t b);

/* 阻塞收满 len 字节，超时返回 -1。等待期间按 100ms 节流喂外部看门狗。 */
int32_t serial_dl_recv(uint8_t *buf, uint32_t len, uint32_t timeout_ms);

/* 在 USART1 上等一条合法请求行。
 * 返回 1 = 收到（*size / *crc 有效）；0 = 超时，或 size 越界/为 0。
 * timeout_ms == 0 表示只看当前缓冲、不等待。 */
int32_t serial_dl_wait_request(uint32_t timeout_ms, uint32_t *size, uint32_t *crc);

/* 读并【清零】"串口下载请求"BAKPR 标志（引导区写、本函数清）。
 * 返回 1 = 有请求。
 *
 * 为什么必须读后即清：BAKPR 由 VBAT 维持、跨掉电存活。不清的话下次上电
 * 又会被当成"又要串口升级"，设备再也回不到正常启动路径。 */
int32_t serial_dl_take_request(void);

#endif /* _SERIAL_DL_H */
