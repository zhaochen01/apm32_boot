
#include "main.h"
#include "iap.h"



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
    /* hw_flash_write() relocks the FMC when it returns, so unlock again
     * before each page erase. Erasing while locked fails silently and the
     * subsequent write ANDs the new data into the stale page - this
     * corrupted the 0xAA rollback target (backup->run), while preserve
     * (run->backup) worked only because the backup area was blank 0xFF. */
    FMC_Unlock();
    if(FMC_ErasePage(dstAddr + offset) != FMC_STATUS_COMPLETE)
    {
      FMC_Lock();
      return -1;
    }
    (void)hw_flash_write(dstAddr + offset, (const void *)(srcAddr + offset),
                         FLASH_PAGE_SIZE);
    offset += FLASH_PAGE_SIZE;
  }
  FMC_Lock();
  WDT_FEED();
  return 0;
}


int32_t hw_flash_erase(uint32_t addrStart, uint32_t size)
{
  uint16_t page;

//  if(size < FLASH_PAGE_SIZE )page = 1;
//  else{
//    page = size / FLASH_PAGE_SIZE;
//    if( (size % FLASH_PAGE_SIZE) != 0) page = size / FLASH_PAGE_SIZE + 1;
//  }
  
  page = (size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
  
  
  /* 如实报错：原实现把 FMC_ErasePage 的返回值丢掉、返回页数（不是状态），
   * 调用方拿不到任何失败信号。擦除静默失败后写入会 AND 进旧数据 ——
   * 这正是 hw_flash_copy() 注释里记的那次事故。 */
  FMC_Unlock();
  for( uint16_t i = 0; i < page; i++)
  {
    if(FMC_ErasePage(addrStart + FLASH_PAGE_SIZE * i) != FMC_STATUS_COMPLETE)
    {
      FMC_Lock();
      return -1;
    }
  }
  FMC_Lock();
  return 0;
}


int32_t hw_flash_write(uint32_t addrStart, const void *pdata, uint32_t size)
{
  const uint8_t *p = (const uint8_t *)pdata;
  uint32_t offset = 0U;
  uint32_t word = 0xFFFFFFFFU; /* β�ֽڰ�����̬0xFF���룬����д�������� */

  /* 如实报错：原实现把 FMC_ProgramWord 的结果丢掉、恒返回 0，
   * 编程失败对调用方完全不可见。 */
  FMC_Unlock();
  while(offset < size)
  {
    word = 0xFFFFFFFFU;
    if((size - offset) >= 4U)
    {
      memcpy(&word, p + offset, 4U);
      if(FMC_ProgramWord(addrStart + offset, word) != FMC_STATUS_COMPLETE)
      {
        FMC_Lock();
        return -1;
      }
      offset += 4U;
    }
    else
    {
      uint8_t rem = (uint8_t)(size - offset);
      memcpy(&word, p + offset, rem);
      if(FMC_ProgramWord(addrStart + offset, word) != FMC_STATUS_COMPLETE)
      {
        FMC_Lock();
        return -1;
      }
      break;
    }
  }
  FMC_Lock();
  return 0;
}







