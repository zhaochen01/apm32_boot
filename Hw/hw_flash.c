
#include "main.h"
#include "iap.h"

/* ---- 极简 FMC 原语（2026-10-15）--------------------------------------
 * 本工程只用得到"解锁 / 擦一页 / 写一个字 / 上锁"四件事，SDK 那份
 * apm32e10x_fmc.c 编进来要 1878B（FMC_ProgramWord 1218 + FMC_ErasePage
 * 612 + Unlock/Lock），而寄存器时序就这么几行 —— 于是自带一份。
 *
 * 序列与 App/ram_flash.c 的 RAM 例程逐条一致：那份已在真机上跑通 boot
 * 自升级（整槽擦除 + 逐半字写入 24KB），是同一颗硅片上验证过的写法。
 *
 * 返回值与 SDK 严格对齐（调用方按 "!= COMPLETE" 判失败）：
 *    0 = COMPLETE，-1 = 超时 / 编程错 PEF / 写保护错 WPEF。
 * SDK 的 FMC_ReadStatus() 查的就是 BUSYF/PEF/WPEF 这三位，顺序也一样 ——
 * 别改成"只看 BUSY"，那会把 PEF/WPEF 的真实故障吞成成功。
 * --------------------------------------------------------------------- */
#define FMC_TIMEOUT     0x000B0000u   /* 与 SDK FMC_WaitForLastOperation 同值 */
#define FMC_STS_BUSYF   0x00000001u
#define FMC_STS_PEF     0x00000004u
#define FMC_STS_WPEF    0x00000010u

/* noinline 不是优化提示，是体积要求：fmc_wait 的超时循环会在
 * fmc_erase_page(2 处) + fmc_prog_word(3 处) 里各展开一份，而这两个
 * 又各有两个调用点 —— -O2 默认内联会把它复制成十来份，实测把这个文件
 * 从 344B 顶到 2270B。加 noinline 后各留一份，省下约 1.5KB。 */
#define FMC_NOINLINE   __attribute__((noinline))

FMC_NOINLINE static int32_t fmc_wait(void)
{
  uint32_t t = FMC_TIMEOUT;
  uint32_t sts;

  /* 体太小，-O2 会把它展开 8 份（12B×8 的检查块），一个"等忙位"的循环
   * 就膨胀到 194B —— SDK 那份 1218B 的 FMC_ProgramWord 大半是这么来的。 */
#pragma clang loop unroll(disable)
  for(;;)
  {
    sts = FMC->STS;
    if((sts & FMC_STS_BUSYF) == 0u)
    {
      return ((sts & (FMC_STS_PEF | FMC_STS_WPEF)) != 0u) ? -1 : 0;
    }
    if(--t == 0u)
    {
      return -1;
    }
  }
}

static void fmc_unlock(void)
{
  FMC->KEY = 0x45670123u;
  FMC->KEY = 0xCDEF89ABu;
}

static void fmc_lock(void)
{
  FMC->CTRL2_B.LOCK = 1u;
}

/* 擦一页。SDK FMC_ErasePage 同样是两次等待：操作前等空闲、操作后等完成。 */
FMC_NOINLINE static int32_t fmc_erase_page(uint32_t page_addr)
{
  if(fmc_wait() != 0) return -1;

  FMC->CTRL2_B.PAGEERA = 1u;
  FMC->ADDR = page_addr;
  FMC->CTRL2_B.STA = 1u;

  if(fmc_wait() != 0)
  {
    FMC->CTRL2_B.PAGEERA = 0u;
    return -1;
  }
  FMC->CTRL2_B.PAGEERA = 0u;
  return 0;
}

/* 写 4 字节 = 两个半字（F1 系 FMC 的编程粒度是半字），与 SDK
 * FMC_ProgramWord 同构，包括那两句关中断 —— CLAUDE.md 的"临界区模式"一节
 * 考证过：真正撑住页原子性的就是这层内部关中断，外层的包裹是假象。
 * 半字的分界落在 4 字节边界上，所以这里不会写出半个 2 字节对齐。 */
FMC_NOINLINE static int32_t fmc_prog_word(uint32_t addr, uint32_t word)
{
  int32_t ok;

  __disable_irq();
  ok = fmc_wait();
  if(ok == 0)
  {
    FMC->CTRL2_B.PG = 1u;
    *(__IOM uint16_t *)addr = (uint16_t)word;
    ok = fmc_wait();
    if(ok == 0)
    {
      *(__IOM uint16_t *)(addr + 2u) = (uint16_t)(word >> 16);
      ok = fmc_wait();
    }
    FMC->CTRL2_B.PG = 0u;
  }
  __enable_irq();
  return ok;
}


/* copy size bytes from src flash to dst flash (no overlap allowed);
 * dst is erased page-aligned first, source is read via direct pointer
 * between FMC operations (same pattern as flash_write_with_wdt);
 * External WDT fed per page (page erase+write ~ 50-100ms; WDT ~15s).
 * NOTE: the on-chip IWDT is NOT used in this project (main.c says so
 * explicitly) — WDT_FEED() toggles the external chip via PB0. */
int32_t hw_flash_copy(uint32_t srcAddr, uint32_t dstAddr, uint32_t size)
{
  uint32_t copy_len = (size + FLASH_PAGE_SIZE - 1U) & (~(FLASH_PAGE_SIZE - 1U));
  uint32_t offset = 0U;

  while(offset < copy_len)
  {
    WDT_FEED();
    /* 每次擦除前重新解锁：hw_flash_write() 返回时会重新上锁。带锁擦除
     * 会静默失败，随后的写入只会把新数据 AND 进旧页 —— 这正是当年
     * 0xAA 回退目标(backup->run)被写坏的事故；preserve(run->backup)
     * 当时"看起来正常"只是因为备份区本来是空的 0xFF。 */
    fmc_unlock();
    if(fmc_erase_page(dstAddr + offset) != 0)
    {
      fmc_lock();
      return -1;
    }
    /* 写入也查返回码（2026-10-15）：上面擦除一路在查、这里却 (void) 吞掉，
     * 是同一次事故的另外半截。调用方虽有回读校验兜底，本模块自己如实
     * 报错才是对的。 */
    if(hw_flash_write(dstAddr + offset, (const void *)(srcAddr + offset),
                      FLASH_PAGE_SIZE) != 0)
    {
      fmc_lock();
      return -1;
    }
    offset += FLASH_PAGE_SIZE;
  }
  fmc_lock();
  WDT_FEED();
  return 0;
}


int32_t hw_flash_erase(uint32_t addrStart, uint32_t size)
{
  uint32_t page;
  uint32_t i;

  page = (size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

  /* 如实报错：原实现把 FMC_ErasePage 的返回值丢掉、返回页数（不是状态），
   * 调用方拿不到任何失败信号。擦除静默失败后写入会 AND 进旧数据 ——
   * 这正是 hw_flash_copy() 注释里记的那次事故。
   *
   * 每页喂狗：整槽 214KB = 107 页 × ~25ms ≈ 2.7s，超过外部 WDT 的窗口，
   * 所以原来 upgrade_ctrl.c 里那个 flash_erase_with_wdt() 的喂狗挪到了这里
   * （小擦除多翻一次 PB0 无副作用）。 */
  fmc_unlock();
  for(i = 0U; i < page; i++)
  {
    WDT_FEED();
    if(fmc_erase_page(addrStart + FLASH_PAGE_SIZE * i) != 0)
    {
      fmc_lock();
      return -1;
    }
  }
  fmc_lock();
  WDT_FEED();
  return 0;
}


int32_t hw_flash_write(uint32_t addrStart, const void *pdata, uint32_t size)
{
  const uint8_t *p = (const uint8_t *)pdata;
  uint32_t offset = 0U;
  uint32_t word;

  /* 如实报错：原实现把 FMC_ProgramWord 的结果丢掉、恒返回 0，
   * 编程失败对调用方完全不可见。 */
  fmc_unlock();
  while(offset < size)
  {
    word = 0xFFFFFFFFU;          /* 末字不足 4 字节时用 0xFF 补：Flash 只能把 1 清成 0 */
    if((size - offset) >= 4U)
    {
      memcpy(&word, p + offset, 4U);
      if(fmc_prog_word(addrStart + offset, word) != 0)
      {
        fmc_lock();
        return -1;
      }
      offset += 4U;
    }
    else
    {
      uint8_t rem = (uint8_t)(size - offset);
      memcpy(&word, p + offset, rem);
      if(fmc_prog_word(addrStart + offset, word) != 0)
      {
        fmc_lock();
        return -1;
      }
      break;
    }
  }
  fmc_lock();
  return 0;
}
