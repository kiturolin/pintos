#include "inode.h"
#include "cache.h"
#include "off_t.h"
#include "round.h"
#include "free-map.h"
#include "../devices/block.h"
#include "../threads/malloc.h"
#include "stdbool.h"
#include <stdint.h>
#include <debug.h>
#include <stdlib.h>
#include <string.h>

// 每个间接块可容纳的指针数
#define INDIRECT_PER_BLOCK   (BLOCK_SECTOR_SIZE / sizeof(block_sector_t)) 
#define MAX_FILE_SECTORS     (DIRECT_BLOCKS + INDIRECT_PER_BLOCK + (INDIRECT_PER_BLOCK * INDIRECT_PER_BLOCK))
// 用于填充0的辅助内存空间
char zeros[BLOCK_SECTOR_SIZE];

void
index_init()
{
  memset(zeros, 0, BLOCK_SECTOR_SIZE);
}


// 在freemap中分配一个空白的sector, 并且向里面填充0, 同时修改sector指向的内存
bool
index_allocate_single_sector(block_sector_t *sector)
{
  if (free_map_allocate(1, sector))
  {
    cache_write(*sector, zeros, true);
    return true;
  }
  else 
  {
    return false; 
  }
}

// 返回length位置在当前文件的间接块树中的索引
// 比如, 假设length = 18499238, 返回level = 2, idx1 = 103, idx2 = 48(这是随口说的, 不准确)
// 说明文件的第18499238个字节所在的扇区地址位于
// 第二级间接块第103条目对应的第一级间接块中的第48个条目锁存储的扇区号
void
index_where_the_sector(off_t length, uint8_t *level, uint8_t *idx1, uint8_t *idx2)
{
  uint32_t sectors = DIV_ROUND_UP(length, BLOCK_SECTOR_SIZE);
  if (sectors >= MAX_FILE_SECTORS)
    PANIC("Too long File!\n");

  if (sectors <= DIRECT_BLOCKS)
  {
    *level = 0;
    *idx1 = sectors - 1;
    *idx2 = 255;
    return ;
  }
  sectors -= DIRECT_BLOCKS;
  if (sectors > INDIRECT_PER_BLOCK)
  {
    ASSERT(sectors > 0);
    // 在执行下一步运算后, sector代表的含义就是要储存在第二级间接块中的扇区总数
    sectors -= DIRECT_BLOCKS + INDIRECT_PER_BLOCK;
    // 这里的运算有点复杂, 但我会给你举个例子
    // 比如sector = 129, 那它位于第二级间接块的第0个entry, 第一级间接块的第1个entry
    *level = 2;
    *idx1 = (sectors - 1) / INDIRECT_PER_BLOCK;
    *idx2 = (sectors - INDIRECT_PER_BLOCK * (*idx1) - 1);
    return ;
  }
  else
  {
    *level = 1;
    *idx1 = sectors - 1;
    *idx2 = 255;
    return ;
  }
}

bool
index_extend(struct inode_disk *data, off_t new_length) 
{
  off_t now_length = data->length;
  uint8_t level, idx1, idx2;
  
  // 如果当前sector内还有空余空间, 那就不需要分配新的sector
  if (ROUND_UP(now_length, BLOCK_SECTOR_SIZE) >= new_length)
    return true;

  while(now_length < new_length)
  {
    now_length += BLOCK_SECTOR_SIZE;
    // 获取下一个sector位于文件索引树的位置信息
    index_where_the_sector(now_length, &level, &idx1, &idx2);
    // 处理新的sector
    if (level == 0)
      index_allocate_single_sector(&data->direct[idx1]);
    else if (level == 1)
    {
      // 如果尚未分配第一级间接块目录
      if (data->indirect == 0)
        index_allocate_single_sector(&data->indirect);
      // 读取第一级间接块目录
      block_sector_t *table = calloc(1, BLOCK_SECTOR_SIZE);
      cache_read(data->indirect, table, true);

      ASSERT(table[idx1] == 0);
      index_allocate_single_sector(&(table[idx1]));
      // 将修改后的table写入磁盘
      cache_write(data->indirect, table, true);
      free(table);
    }
    else if (level == 2)
    {
      // 如果尚未分配第二级间接块目录
      if (data->double_indirect == 0)
        index_allocate_single_sector(&data->double_indirect);
      // 读取第二级间接块目录
      block_sector_t *table2 = calloc(1, BLOCK_SECTOR_SIZE);
      cache_read(data->double_indirect, table2, true);
      block_sector_t table1_sector = table2[idx1];
      // 如果尚未分配第一级间接块目录
      if (table1_sector == 0)
        index_allocate_single_sector(&(table2[idx1]));
      // 读取第一级间接块目录
      block_sector_t *table1 = calloc(1, BLOCK_SECTOR_SIZE);
      
      ASSERT(table1[idx2] == 0);
      index_allocate_single_sector(&(table1[idx2]));
      // 将修改后的table写入磁盘
      cache_write(data->double_indirect, table2, true);
      cache_write(table2[idx1], table1, true);
      free(table2);
      free(table1);
    }
    else {
      PANIC("Unknown level!");
    }
  }    
  return true;
}
