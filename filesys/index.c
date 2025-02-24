#include "index.h"
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
    cache_write(*sector, zeros, false);
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
  uint32_t sectors = length / BLOCK_SECTOR_SIZE; 

  if (sectors >= MAX_FILE_SECTORS)
    PANIC("Too long File!\n");

  if (sectors < DIRECT_BLOCKS)
  {
    *level = 0;
    *idx1 = sectors;
    *idx2 = 255;
    return ;
  }
  sectors -= DIRECT_BLOCKS;
  if (sectors > INDIRECT_PER_BLOCK)
  {
    ASSERT(sectors > 0);
    // 在执行下一步运算后, sector代表的含义就是要储存在第二级间接块中的扇区总数
    sectors -= DIRECT_BLOCKS + INDIRECT_PER_BLOCK;
    *level = 2;
    *idx1 = sectors / INDIRECT_PER_BLOCK;
    *idx2 = sectors % INDIRECT_PER_BLOCK;
    return ;
  }
  else
  {
    *level = 1;
    *idx1 = sectors;
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
      ASSERT(table != NULL);

      cache_read(data->indirect, table, false);

      ASSERT(table[idx1] == 0);
      index_allocate_single_sector(&(table[idx1]));
      // 将修改后的table写入磁盘
      cache_write(data->indirect, table, false);
      free(table);
    }
    else if (level == 2)
    {
      // 如果尚未分配第二级间接块目录
      if (data->double_indirect == 0)
        index_allocate_single_sector(&data->double_indirect);
      // 读取第二级间接块目录
      block_sector_t *table2 = calloc(1, BLOCK_SECTOR_SIZE);
      ASSERT(table2 != NULL);

      cache_read(data->double_indirect, table2, false);
      // 获取一级间接块的扇区编号
      block_sector_t table1_sector = table2[idx1];
      // 如果尚未分配第一级间接块目录
      if (table1_sector == 0)
        index_allocate_single_sector(&(table2[idx1]));
      // 读取第一级间接块目录
      block_sector_t *table1 = calloc(1, BLOCK_SECTOR_SIZE);
      ASSERT(table1 != NULL);
      
      ASSERT(table1[idx2] == 0);
      index_allocate_single_sector(&(table1[idx2]));
      // 将修改后的table写入磁盘
      cache_write(data->double_indirect, table2, false);
      cache_write(table2[idx1], table1, false);
      free(table2);
      free(table1);
    }
    else {
      PANIC("Unknown level!");
    }
    now_length += BLOCK_SECTOR_SIZE;
  }    
  //更新文件的长度
  data->length = new_length;
  return true;
}

// 释放文件持有的所有sector
void
index_relese_sectors(struct inode_disk *data)
{
  // 释放直接块
  for (int i = 0; i < DIRECT_BLOCKS && data->direct[i] != 0; i++)
    free_map_release(data->direct[i], 1);

  // 下面释放间接块
  // 如果文件压根没用到第一级间接块
  if (data->indirect == 0)
    return ;
  // 读取第一级间接块
  block_sector_t *indirect_table = calloc(1, BLOCK_SECTOR_SIZE);
  ASSERT(indirect_table != NULL);
  cache_read(data->indirect, indirect_table, true);

  // 释放第一级间接块指向的所有扇区
  for (int i = 0; i < INDIRECT_PER_BLOCK && indirect_table[i] != 0; i++)
    free_map_release(indirect_table[i], 1);

  free(indirect_table);
  //释放第一级间接块本身
  free_map_release(data->indirect, 1);

  // 释放第二级间接块
  // 如果文件压根没用到第二级间接块
  if (data->double_indirect == 0)
    return ;

  block_sector_t *double_indirect_table = calloc(1, BLOCK_SECTOR_SIZE);
  ASSERT(double_indirect_table != NULL);
  cache_read(data->double_indirect, double_indirect_table, false);

  // 遍历第二级间接块内指向的所有第一级间接块
  for (int i = 0; i < INDIRECT_PER_BLOCK && double_indirect_table[i] != 0; i++)
  {
    // 读取指向的第一级间接块
    block_sector_t *table1 = calloc(1, BLOCK_SECTOR_SIZE);
    ASSERT(table1 != NULL);
    cache_read(double_indirect_table[i], table1, false);

    // 释放第一级间接块指向的所有扇区
    for (int j = 0; j < INDIRECT_PER_BLOCK && table1[j] != 0; j++)
      free_map_release(table1[j], 1);

    free(table1);
    // 释放第一级间接块本身
    free_map_release(double_indirect_table[i], 1);
  }

  free(double_indirect_table);
  // 释放第二级间接块本身
  free_map_release(data->double_indirect, 1);
}
