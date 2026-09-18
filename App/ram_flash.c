/*
 * ram_flash.c —— RAM 常驻的 Flash 擦写例程（boot 自升级专用）
 *
 * 为什么必须放 RAM：
 *   升级 boot 跑在 A 槽（0x08003000）里，而 boot 自升级要擦写的正是 A 槽。
 *   擦除 A 的那一刻，Flash 里的本函数就没了 —— 只有 RAM 里的副本还能把
 *   剩下的擦除 + 写入跑完。这就是 IAP 的经典做法。
 *
 * ============================ 实现约束 ============================
 * 破坏了任意一条，都会在擦除后跳回 Flash 取指 -> HardFault：
 *   1. 完全自包含：不调用任何其他函数
 *   2. 不访问任何全局/静态变量（全部走参数）
 *   3. 除传入的 src 指针外，不读任何 Flash 地址
 *   4. 字面量池必须随代码一起进 .ramfunc（由 scatter 保证）
 *   5. 完成后直接复位 —— 调用者在 A 槽里，已经被擦掉了，不能返回
 * ==================================================================
 *
 * 由 scatter 的 ER_RAMFUNC 区承载：__main 启动时把 Flash 里的加载副本
 * 搬到 RAM 执行区（见 MDK/apm32e103_boot.sct）。
 */

#include "apm32e10x.h"
#include <stdint.h>

#define RAMFN   __attribute__((section(".ramfunc"))) __attribute__((noinline))

#define FMC_PAGE_SIZE   0x800u          /* 2KB */
#define FMC_TIMEOUT     0x000B0000u     /* 与 SDK FMC_WaitForLastOperation 同值 */
#define FMC_BUSY_BIT    0x00000001u     /* FMC->STS bit0 = BUSY */

/*
 * 等 FMC 空闲：1=就绪 0=超时。
 * 不调 SDK 的 FMC_WaitForLastOperation —— 那个函数在 Flash 里。
 */
RAMFN static int32_t ram_fmc_wait(void)
{
    uint32_t t = FMC_TIMEOUT;

    while (t != 0u) {
        if ((FMC->STS & FMC_BUSY_BIT) == 0u) return 1;
        t--;
    }
    return 0;
}

/* 写一个 16 位半字（与 SDK FMC_ProgramWord 的单次半字编程语义一致） */
RAMFN static int32_t ram_fmc_prog_half(uint32_t addr, uint16_t half)
{
    if (!ram_fmc_wait()) return 0;
    FMC->CTRL2_B.PG = 1u;
    *(__IOM uint16_t *)addr = half;
    if (!ram_fmc_wait()) { FMC->CTRL2_B.PG = 0u; return 0; }
    FMC->CTRL2_B.PG = 0u;
    return 1;
}

/*
 * 把 src 里的 img_size 字节写进 slot_addr 开的槽：整槽擦除 -> 逐半字写镜像
 * -> 槽尾写 {crc, size}（crc@slot_size-12 / size@slot_size-8）-> 复位。
 *
 * wdt_odata : 外部看门狗那个 GPIO 的 ODATA 寄存器地址（调用方算好传进来，
 *             避免本函数去取全局变量的地址 -> 字面量池留在 Flash 的风险）
 * wdt_pin   : 对应引脚掩码
 *
 * 返回：永不返回（末尾复位）。
 */
RAMFN void ram_flash_program(uint32_t slot_addr, uint32_t slot_size,
                             const uint8_t *src, uint32_t img_size, uint32_t crc,
                             volatile uint32_t *wdt_odata, uint32_t wdt_pin)
{
    uint32_t off;
    uint32_t i;
    uint16_t half;

    /* 全程关中断：ISR 在 Flash 里，擦除后取指必崩 */
    __disable_irq();

    /* 解锁 */
    FMC->KEY = 0x45670123u;
    FMC->KEY = 0xCDEF89ABu;

    /* ---- 整槽擦除 ---- */
    for (off = 0u; off < slot_size; off += FMC_PAGE_SIZE) {
        if (!ram_fmc_wait()) goto out;
        FMC->CTRL2_B.PAGEERA = 1u;
        FMC->ADDR = slot_addr + off;
        FMC->CTRL2_B.STA = 1u;
        if (!ram_fmc_wait()) { FMC->CTRL2_B.PAGEERA = 0u; goto out; }
        FMC->CTRL2_B.PAGEERA = 0u;
        if (wdt_odata) *wdt_odata ^= wdt_pin;      /* 每页喂狗 */
    }

    /* ---- 写镜像（逐半字）---- */
    for (i = 0u; i < img_size; i += 2u) {
        half = (uint16_t)src[i];
        half |= (uint16_t)((i + 1u < img_size) ? ((uint16_t)src[i + 1u] << 8)
                                                : 0xFF00u);
        if (!ram_fmc_prog_half(slot_addr + i, half)) goto out;

        /* 512 字节喂一次狗：24KB 写入约 0.6s，够密 */
        if ((i & 0x1FFu) == 0u && wdt_odata) *wdt_odata ^= wdt_pin;
    }

    /* ---- 槽尾 {crc, size} ---- */
    if (!ram_fmc_prog_half(slot_addr + slot_size - 12u, (uint16_t)crc)) goto out;
    if (!ram_fmc_prog_half(slot_addr + slot_size - 10u, (uint16_t)(crc >> 16))) goto out;
    if (!ram_fmc_prog_half(slot_addr + slot_size -  8u, (uint16_t)img_size)) goto out;
    if (!ram_fmc_prog_half(slot_addr + slot_size -  6u, (uint16_t)(img_size >> 16))) goto out;

out:
    FMC->CTRL2_B.LOCK = 1u;
    if (wdt_odata) *wdt_odata ^= wdt_pin;

    /* 不返回 —— 调用者在 A 槽里，已经被擦掉了。直接复位交回引导区。
     * 手写 AIRCR 而不调 NVIC_SystemReset()，避免任何跳回 Flash 的可能。 */
    SCB->AIRCR = (0x5FAu << 16) | (1u << 2);
    for (;;) { }
}