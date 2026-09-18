#ifndef __RAM_FLASH_H
#define __RAM_FLASH_H

#include <stdint.h>

/*
 * RAM 常驻的 Flash 擦写例程 —— boot 自升级专用（实现见 App/ram_flash.c）。
 *
 * 为什么需要它：升级 boot 跑在 A 槽（0x08003000）里，而自升级要擦写的正是
 * A 槽。擦除 A 的那一刻，Flash 里的代码就没了 —— 只有这段跑在 RAM 里的
 * 副本能把剩下的擦除 + 写入做完。这是 IAP 的标准做法。
 *
 * !!! 调用约定（破坏任意一条都会变砖）：
 *   1. src 指向的镜像必须【已经在 RAM 里】—— 函数一执行就会擦掉 Flash，
 *      之后再从 Flash 读源码必崩
 *   2. crc 必须由调用方先算好（函数内不能再调 CRC32()，那在 Flash 里）
 *   3. 【永不返回】—— 末尾直接复位交回引导区
 *   4. 只能写【不是自己正在跑】的那个槽
 *
 * slot_addr : 目标槽基址
 * slot_size : 槽容量（BOOT_SLOT_SIZE = 24576）
 * src       : RAM 里的镜像缓冲
 * img_size  : 镜像长度（<= slot_size - 12）
 * crc       : 镜像的 CRC32（IEEE 802.3）
 * wdt_odata : 外部看门狗 GPIO 的 ODATA 寄存器地址（由调用方算好传进来，
 *             避免本函数去取全局变量地址 -> 字面量池留在 Flash 的风险）
 * wdt_pin   : 看门狗引脚掩码
 */
void ram_flash_program(uint32_t slot_addr, uint32_t slot_size,
                       const uint8_t *src, uint32_t img_size, uint32_t crc,
                       volatile uint32_t *wdt_odata, uint32_t wdt_pin);

#endif /* __RAM_FLASH_H */
