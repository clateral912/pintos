#include "inode.h"
#include "cache.h"
#include "round.h"
#include "free-map.h"
#include "../devices/block.h"
#include "../threads/malloc.h"
#include <stdint.h>
#include <debug.h>
#include <stdlib.h>
#include <string.h>

// 用于填充0的辅助内存空间
char zeros[BLOCK_SECTOR_SIZE];

void
index_init()
{
  memset(zeros, 0, BLOCK_SECTOR_SIZE);
}

// 给定一个
block_sector_t
index_which_sector()
{
  return 1;
}

// 在磁盘中分配一块空闲的sector, 并把扇区号写入sector指针指向的地址
void
index_allocate_sector(block_sector_t *sector)
{
  bool success = free_map_allocate(1, sector) ;
  if (!success)
    PANIC("index_extend(): Cannot allocate direct blocks!\n");
  // 将分配好的扇区清空
  cache_write(*sector, zeros, false);
}

// 使用free_map获取下一个空闲的扇区编号, 随后清空目标扇区
// 再将扇区编号写入inode索引
// 当文件长度增长到需要间接块时, 由该函数负责为其分配新的间接块
void
index_extend(struct inode_disk *data, off_t new_length)
{
  off_t old_length = data->length;
  // 文件当前占有的扇区总量
  uint32_t now_sector = old_length / BLOCK_SECTOR_SIZE;
  // 需要分配的新扇区数量
  uint32_t new_sectors = DIV_ROUND_UP(new_length - old_length, BLOCK_SECTOR_SIZE);
  // 分配直接块
  for (int i = 0; i < DIRECT_BLOCKS && new_sectors > 0; i++)
  {
    // 如果第i个直接块尚未分配
    if (data->direct[i] == 0)
    {
      index_allocate_sector(&data->direct[i]);
      now_sector++;
      new_sectors--;
    }
  }
  // 分配间接块
  if (new_sectors > 0)
  {
    // 分配间接块的索引, 如果间接块尚未分配
    if (data->indirect == 0)
      index_allocate_sector(&data->indirect);

    // 间接块内部的编号, 为当前占有的扇区号 - 5, 因为我们有5个直接块
    uint32_t indirect_idx = now_sector - DIRECT_BLOCKS;
    //读取间接块
    block_sector_t *indirect_sector = calloc(1, BLOCK_SECTOR_SIZE);
    cache_read(data->indirect, indirect_sector, false);

    // 为间接块分配新条目
    for (int i = 0; i < INDIRECT_BLOCKS && new_sectors > 0; i++)
    {
      index_allocate_sector(indirect_sector + indirect_idx + i);
      now_sector++;
      new_sectors--;
    }
    // 将一级间接块写入磁盘
    cache_write(data->indirect, indirect_sector, false);
    free(indirect_sector);
  }

  // 分配二级间接块
  // 能进入以下分支说明文件的长度已经大到需要用二级间接块来索引扇区
  if (new_sectors > 0)
  {
    // 分配间接块的索引, 如果尚未启用二级间接块
    if (data->double_indirect == 0)
      index_allocate_sector(&data->double_indirect);

    // 读取二级间接块的内容
    block_sector_t *double_indirect_sector = calloc(1, BLOCK_SECTOR_SIZE);
    cache_read(data->double_indirect, double_indirect_sector, false);

    // 在二级间接块内条目的编号, 每个编号对应一个一级间接块
    uint32_t double_indirect_idx = (now_sector - DIRECT_BLOCKS - INDIRECT_BLOCKS) / INDIRECT_BLOCKS;
    // 读取一级间接块
    for (int i = 0; i < INDIRECT_BLOCKS && new_sectors > 0; i++)
    {
      // 获取一级间接块存储的扇区编号
      block_sector_t indirect_sector_idx = double_indirect_sector[double_indirect_idx + i];
      // 如果一级间接块尚未分配, 则分配一个一级间接块
      if (indirect_sector_idx == 0)
        index_allocate_sector(double_indirect_sector + i);

      // 获取二级间接块当前条目指向的一级间接块, 将其读入内存
      block_sector_t *indirect_sector = calloc(1, BLOCK_SECTOR_SIZE);
      cache_read(indirect_sector_idx, indirect_sector, false);
      // 在第一级间接块中的编号
      block_sector_t indirect_idx = now_sector - DIRECT_BLOCKS - INDIRECT_BLOCKS - INDIRECT_BLOCKS * i;
      // 读取第一级间接块, 访问第一级间接块中的各个条目
      for (int j = 0; j < INDIRECT_BLOCKS && now_sector > 0; j++)
      {
        index_allocate_sector(indirect_sector + indirect_idx + j);
        now_sector++;
        new_sectors--;
      }
      // 将添加了扇区索引信息的第一级间接块写入磁盘
      cache_write(indirect_sector_idx, indirect_sector, false);
      free(indirect_sector);
    }
    // 将二级间接块写入磁盘
    cache_write(data->double_indirect, double_indirect_sector, false);
    free(double_indirect_sector);
  } 
  return ;
}


