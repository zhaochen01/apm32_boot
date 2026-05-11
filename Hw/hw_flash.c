
#include "main.h"




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
  uint32_t word = 0xFFFFFFFFU; /* 尾字节按擦除态0xFF补齐，避免写入脏数据 */

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
    *p++ = (*(__IO uint8_t*) addrStart++);  // 读指定地址的一个数据
//    addrStart += 1;
  }
  return 0;
}






