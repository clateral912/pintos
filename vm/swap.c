#include "swap.h"
#include <bitmap.h>
#include "../threads/vaddr.h"
#include "../devices/block.h"

//单位为Byte, 共4MiB
#define SWAP_SIZE 4194304 
#define BITMAP_START 0
#define SINGLE_PAGE 1 
#define SECTOR_PER_PAGE 4
#define FREE 0
#define USED 1

struct block *swap_disk;
struct bitmap *swap_bitmap;

void 
swap_init()
{
  // 获取swap分区对应的磁盘
  swap_disk = block_get_role(BLOCK_SWAP);
  // 使用bitmap管理扇区
  // 默认分配4MiB / 4KiB = 1024页
  swap_bitmap = bitmap_create(SWAP_SIZE / PGSIZE);
}

// 以页为单位获取一个空闲的起始扇区号,
// 该扇区后3个扇区应该都是空闲的
// 每个扇区512Byte, 每页4096个Byte
// 每页需要4个扇区存储
static block_sector_t
swap_get_free_sector()
{
  size_t page_idx = bitmap_scan_and_flip(swap_bitmap, BITMAP_START, SINGLE_PAGE, FREE);
  return page_idx * 4;
}

// 释放第page_idx位
// 将扇区标记为可用
static void
swap_free_used_sector(size_t page_idx)
{
  // 确保释放的page_idx已经被占用
  ASSERT(bitmap_test(swap_bitmap, page_idx) == USED);
  bitmap_reset(swap_bitmap, page_idx);
}

// 将起始位置位于uaddr的页面换入swap磁盘中
// 并返回page_idx
size_t 
swap_in(const void *upage)
{
  ASSERT(pg_ofs(upage) == 0);
  block_sector_t free_sector_begin = swap_get_free_sector();
  size_t page_idx = free_sector_begin / SECTOR_PER_PAGE;

  for (int i = SECTOR_PER_PAGE; i > 0; i--)
  {
    block_write(swap_disk, free_sector_begin, upage);
    free_sector_begin += 1;
    upage += BLOCK_SECTOR_SIZE;
  }

  return page_idx;
}

// 将swap磁盘中对应页面序号为page_idx的页面读取到upage中
void
swap_out(size_t page_idx, void *upage)
{
  ASSERT(pg_ofs(upage) == 0);
  block_sector_t sector = page_idx * SECTOR_PER_PAGE;

  for (int i = SECTOR_PER_PAGE; i > 0; i--)
  {
    block_read(swap_disk, sector, upage);
    sector += 1;
    upage += BLOCK_SECTOR_SIZE;
  }

  swap_free_used_sector(page_idx);
}

