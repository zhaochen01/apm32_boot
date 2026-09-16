
#include "main.h"
#include "iap.h"
#include "apm32e10x_iwdt.h"



/* copy size bytes from src flash to dst flash (no overlap allowed);
 * dst is erased page-aligned first, source is read via direct pointer
 * between FMC operations (same pattern as flash_write_with_wdt);
 * IWDT refreshed per page (page erase+write ~ 50-100ms, WDT ~15s) */
int32_t hw_flash_copy(uint32_t srcAddr, uint32_t dstAddr, uint32_t size)
{
  uint32_t copy_len = (size + FLASH_PAGE_SIZE - 1U) & (~(FLASH_PAGE_SIZE - 1U));
  uint32_t offset = 0U;

  while(offset < copy_len)
  {
    IWDT_Refresh();
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
  IWDT_Refresh();
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
  
  
  FMC_Unlock();
  for( uint16_t i = 0; i < page; i++)
  {
    FMC_ErasePage(addrStart + FLASH_PAGE_SIZE * i);
  }
  FMC_Lock();
  return page;
}


int32_t hw_flash_write(uint32_t addrStart, const void *pdata, uint32_t size)
{
  const uint8_t *p = (const uint8_t *)pdata;
  uint32_t offset = 0U;
  uint32_t word = 0xFFFFFFFFU; /* β�ֽڰ�����̬0xFF���룬����д�������� */

  FMC_Unlock();
  while(offset < size)
  {
    word = 0xFFFFFFFFU;
    if((size - offset) >= 4U)
    {
      memcpy(&word, p + offset, 4U);
      FMC_ProgramWord(addrStart + offset, word);
      offset += 4U;
    }
    else
    {
      uint8_t rem = (uint8_t)(size - offset);
      memcpy(&word, p + offset, rem);
      FMC_ProgramWord(addrStart + offset, word);
      break;
    }
  }
  FMC_Lock();
  return 0;
}


int32_t hw_flash_read(uint32_t addrStart, void *pdata, uint32_t size)
{
  uint8_t *p = (uint8_t *)pdata;
  while(size--)
  {
    *p++ = (*(__IO uint8_t*) addrStart++);  // ��ָ����ַ��һ������
//    addrStart += 1;
  }
  return 0;
}






