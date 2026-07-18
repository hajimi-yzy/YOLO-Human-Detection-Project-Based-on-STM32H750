#include "eeprom.h"
#include "main.h"
#include "string.h"

//extern void  FLASH_PageErase(uint32_t PageAddress);

uint16_t STMFLASH_BUF[STM_SECTOR_SIZE/2];//最多是2K字节


//功能：读取指定地址的char
//参数：addr表示要读的地址
//返回值：data为指定地址内的数据
uint8_t Read_Char(uint32_t addr)
{
	uint8_t data;
	data=*(uint8_t*)addr;//先把地址强制装换为指针，然后再用*取出该指针所指向的数据
	return data;
}

//功能：读取指定地址的半个字
//参数：addr表示要读的地址
//返回值：data为指定地址内的数据
uint16_t Read_HalfWord(uint32_t addr)
{
	uint16_t data;
	data=*(uint16_t*)addr;//先把地址强制装换为指针，然后再用*取出该指针所指向的数据
	return data;
}

//功能：读取指定地址的一个字
//参数：addr表示要读的地址
//返回值：data为指定地址内的数据
uint32_t Read_Word(uint32_t addr)
{
	uint32_t data;
	data=*(uint32_t*)addr;//先把地址强制装换为指针，然后再用*取出该指针所指向的数据
	return data;
}

//从指定地址开始读出指定长度的数据
//ReadAddr:起始地址
//pBuffer:数据指针
//NumToWrite:半字(16位)数
void STMFLASH_ReadData(uint32_t Readaddr,uint16_t *pBuffer,uint16_t NumToRead)
{
	uint16_t i;
	for(i=0;i<NumToRead;i++)
	{
		 pBuffer[i]=Read_HalfWord(Readaddr);
		 Readaddr+=2;
	}
}

//不检查的写入
//WriteAddr:起始地址
//pBuffer:数据指针
//NumToWrite:半字(16位)数   
uint16_t write_num;
void STMFLASH_Write_NoCheck(uint32_t WriteAddr,uint16_t *pBuffer,uint16_t NumToWrite)   
{
	for(write_num=0;write_num<NumToWrite;write_num++)
	{
		while(__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY))
		;
		
		//HAL_FLASH_Program(FLASH_TYPEE,WriteAddr,pBuffer[write_num]);
		WriteAddr+=2;//地址增加2.
	}
}

//从指定地址开始写入指定长度的数据
//WriteAddr:起始地址(此地址必须为2的倍数!!)
//pBuffer:数据指针
//NumToWrite:半字(16位)数(就是要写入的16位数据的个数.)
void STMFLASH_Write(uint32_t WriteAddr,uint16_t *pBuffer,uint16_t NumToWrite)	
{
	uint32_t sec_addr;//扇区地址
	uint16_t sec_off;//扇区内偏移地址(16位字计算)
	uint16_t sec_remain;//扇区内剩余地址(16位字计算)
 	uint16_t i;    
	uint32_t offaddr;   //去掉0X08000000后的地址
	if(WriteAddr<STM32_FLASH_BASE||(WriteAddr>=(STM32_FLASH_BASE+1024*STM32_FLASH_SIZE)))return;//非法地址
	HAL_FLASH_Unlock();						//解锁
	offaddr=WriteAddr-STM32_FLASH_BASE;		//实际偏移地址.
	
	sec_addr=offaddr/STM_SECTOR_SIZE;			//扇区地址  0~256 for STM32F103ZET6
	sec_off=(offaddr%STM_SECTOR_SIZE)/2;	//扇区内的偏移地址（2个字节为基本单位）
	sec_remain=((STM_SECTOR_SIZE/2)-sec_off); //以两个字节为基本单位的话，所以一页内有（STM_SECTOR_SIZE/2）个基本单位。
																				//一页内剩余多少个基本单位呢？直接减去一页内的偏移地址就可以了
	if(NumToWrite<=sec_remain)sec_remain=NumToWrite;//不大于该扇区范围
	while(1)
	{
		STMFLASH_ReadData(sec_addr*STM_SECTOR_SIZE+STM32_FLASH_BASE,STMFLASH_BUF,STM_SECTOR_SIZE/2);
		for(i=0;i<sec_remain;i++)
		{
			if(STMFLASH_BUF[sec_off+i]!=0xffff)break;//当检查到有地址内地数据不为0xffff,那么就跳出for循环;
		}
		if(i<sec_remain)//判断i的值是否小于该页的剩余量，如果小于，则说明要擦除整页。
		{
			//FLASH_PageErase(sec_addr*STM_SECTOR_SIZE+STM32_FLASH_BASE);//擦除这个扇区
			for(i=0;i<sec_remain;i++)
			{
				STMFLASH_BUF[i+sec_off]=pBuffer[i];
			}
			CLEAR_BIT(FLASH->CR1,1<<1);
			STMFLASH_Write_NoCheck(sec_addr*STM_SECTOR_SIZE+STM32_FLASH_BASE,STMFLASH_BUF,STM_SECTOR_SIZE/2);//写入整个扇区（页）
		}
		else STMFLASH_Write_NoCheck(WriteAddr,pBuffer,sec_remain);//写已经擦除了的,直接写入扇区剩余区间. 
		if(NumToWrite==sec_remain)break;//写入结束了，跳出while循环
		else//写入未结束
		{
			sec_addr++;				//扇区地址增1
			sec_off=0;				//偏移位置为0 	 
			pBuffer+=sec_remain;  	//指针偏移
			WriteAddr+=sec_remain;	//写地址偏移	   
			NumToWrite-=sec_remain;	//字节(16位)数递减
			if(NumToWrite>(STM_SECTOR_SIZE/2))sec_remain=STM_SECTOR_SIZE/2;//下一个扇区还是写不完
			else sec_remain=NumToWrite;//下一个扇区可以写完了
		}
	}
	HAL_FLASH_Lock();//上锁
}




