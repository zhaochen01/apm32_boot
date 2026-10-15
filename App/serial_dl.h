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
 *   "#DL <size> <crc32hex> [baud]\r\n"   例："#DL 121784 1a2b3c4d 921600\r\n"
 *   size 十进制（≤8 位）、crc32 十六进制（≤8 位）、baud 十进制（≤7 位，可省）。
 *
 * 上位机在上电【之前】就开始每 100ms 重发这一行（覆盖"操作员还没复位"
 * 的时序），所以设备侧必须容忍重复、串接和任意前置噪声 —— 用逐字节
 * 状态机而不是"收一整行再解析"，否则重发会把行缓冲灌满垃圾。
 *
 * baud 省略 / 越界 / 不在白名单 -> 一律按 115200（老工具、老脚本零影响）。 */
#define SERIAL_DL_REQ_PREFIX   "#DL "

/* ---- 下载阶段的波特率协商（2026-10-14）----
 * 目的：串口刷机的瓶颈是传输本身 —— 实测恒定 8.6KB/s（每页 2048B 用掉
 * 178ms 传输 + 52ms 擦写），提到 921600 后传输降到 22ms/页，整轮快 3 倍。
 *
 * 时序（PC 侧 = tools/serial_dl.py）：
 *
 *   PC  @115200  "#DL <size> <crc> <baud>"   （重发，不变）
 *   DEV @115200  "#RDY"                       （不变）
 *   DEV         切到 <baud>，排空 RX，等确认行（SERIAL_DL_BAUD_ACK_MS）
 *   PC  @<baud>  "#DL <size> <crc> <baud>"   （确认链路，size/crc 必须一致，**只发一条**）
 *   DEV @<baud>  "#SYNC"                      （双向链路已确认）
 *   DEV @<baud>  【等 350ms】让确认行的任何冗余先到齐
 *   DEV @<baud>  preserve -> "#GO"            （以下全在新波特率）
 *
 * 【PC 必须在读到 #RDY 时就切】不能等 #GO —— 设备在 #RDY 之后的所有输出
 * （preserve 日志、#GO）都是新波特率，PC 无法用旧波特率判断切换时刻。
 *
 * 【确认行只发一条，且设备要等 350ms】2026-10-15 实测挖出的坑：上位机原先把
 * 确认行每 100ms 重发 3 次（想压过设备"排空 + 切档"的窗口），但设备只收下
 * 第一条就往下走 —— 后两条晚到 0~200ms，正好落进紧接着的收页窗口，被 DMA
 * 当成【页首数据】写进 flash，整条字节流从此错位。症状是"每页都收满、零超时、
 * 整包 CRC 全错"，且**只有切档时（≥230400）才会出现** —— 115200 不协商、
 * 没有确认行，所以一直好的。
 * 现在两边都堵：上位机只发一条；设备收下后等 350ms（> 旧版上位机 3 条 × 100ms
 * 的跨度）再让第 ④ 步的 flush 把冗余排掉。**这两条是协议级约束，改一边要同时
 * 改另一边**（tools/serial_dl.py、SerialDlGui/SerialDlClient.cs）。
 *
 * 【确认行必须排在 preserve 之前】切换失败时 A/B 一个字节都还没动，
 * 设备回 "#ERR baud" 后干净复位（只写了元数据页），什么事都没发生。
 *
 * 【切换不落盘】不写 Flash / 不写 BAKPR：任何复位都会回到引导区重新以
 * 115200 初始化，旧工具、三个 APP 的 serial_dl 命令、引导区 150ms 魔数
 * 侦听全都不受影响。 */
#define SERIAL_DL_BAUD_DEFAULT   115200u

/* 白名单：只有这四个值会被接受，其余一律回落 115200。
 * 与 tools/serial_dl.py 的 BAUD_WHITELIST 必须一致。 */
#define SERIAL_DL_BAUD_ACK_MS    2000u   /* 等 PC 在新波特率的确认行 */

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

/* 接收诊断：got = 最近一次 recv 超时时【已收到】的字节数
 * （区分"对端没发"与"中途断流"）。入参可传 NULL。 */
void serial_dl_get_rx_diag(uint32_t *got);

/* 发静态 ASCII 串 / 单字节。返回 0 成功、-1 失败（TX 卡死，见 uart_send_data）。 */
int32_t serial_dl_send(const char *s);
int32_t serial_dl_send_byte(uint8_t b);

/* 阻塞收满 len 字节，超时返回 -1。等待期间按 100ms 节流喂外部看门狗。 */
int32_t serial_dl_recv(uint8_t *buf, uint32_t len, uint32_t timeout_ms);

/* 取整（不等待）并把 USART1 切到 baud。返回 0 成功、-1 失败。
 * 只改这一个口，不落盘；调用点必须保证此刻 TX 已空（内部的 TC 等待负责）。 */
int32_t serial_dl_set_baud(uint32_t baud);

/* 在 USART1 上等一条合法请求行。
 * 返回 1 = 收到（*size / *crc / *baud 有效）；0 = 超时，或 size 越界/为 0。
 * *baud 缺省为 SERIAL_DL_BAUD_DEFAULT；不是白名单值时也回落成它。
 * baud 可传 NULL（调用方不关心波特率的场合）。
 * timeout_ms == 0 表示只看当前缓冲、不等待。 */
int32_t serial_dl_wait_request(uint32_t timeout_ms, uint32_t *size,
                               uint32_t *crc, uint32_t *baud);

/* 读并【清零】"串口下载请求"BAKPR 标志（引导区写、本函数清）。
 * 返回 1 = 有请求。
 *
 * 为什么必须读后即清：BAKPR 由 VBAT 维持、跨掉电存活。不清的话下次上电
 * 又会被当成"又要串口升级"，设备再也回不到正常启动路径。 */
int32_t serial_dl_take_request(void);

#endif /* _SERIAL_DL_H */
