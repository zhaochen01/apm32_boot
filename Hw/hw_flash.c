
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
  uint32_t  *p = (uint32_t *)pdata;
  uint32_t  count;
  uint8_t   psize = sizeof(uint32_t);
//  if( (size / psize) == 0 ) count = size / psize;
//  else  count = size / psize + 1;
  
  // 计算需要编程的字数量（向上取整）
  count = (size + 4 - 1) / 4;
  
  
  FMC_Unlock();
  for(uint32_t i = 0; i < count; i++)
  {
    FMC_ProgramWord(addrStart + i * psize, *p++);
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






