#include "free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "file.h"
#include "filesys.h"
#include "inode.h"

// 使用bitmap记录磁盘上空闲的扇区
// free_map_file内存储了bitmap
static struct file *free_map_file;   /* Free map file. */
static struct bitmap *free_map;      /* Free map, one bit per sector. */
// 每个扇区对应一个bit!

/* Initializes the free map. */
void
free_map_init (void) 
{
  // block_size返回device有多少个扇区
  free_map = bitmap_create (block_size (fs_device));
  if (free_map == NULL)
    PANIC ("bitmap creation failed--file system device is too large");
  // bitmap_mark()将bitmap中的某一位设置为true(1)
  bitmap_mark (free_map, FREE_MAP_SECTOR);
  bitmap_mark (free_map, ROOT_DIR_SECTOR);
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
bool
free_map_allocate (size_t cnt, block_sector_t *sectorp)
{
  block_sector_t sector = bitmap_scan_and_flip (free_map, 0, cnt, false);
  // 这三条条件中任意一条不满足都会触发allocate失败!
  // 利用了&&运算符"短路"的特性! 条件判断的顺序非常重要! 不能更改!
  if (sector != BITMAP_ERROR
      && free_map_file != NULL
      && !bitmap_write (free_map, free_map_file))
    {
      bitmap_set_multiple (free_map, sector, cnt, false); 
      sector = BITMAP_ERROR;
    }
  if (sector != BITMAP_ERROR)
    *sectorp = sector;
  return sector != BITMAP_ERROR;
}

/* Makes CNT sectors starting at SECTOR available for use. */
// 将已经使用的sector对应的bits设置为false
void
free_map_release (block_sector_t sector, size_t cnt)
{
  // bitmap_all()检测从sector开始cnt个sector是否都被设为true
  ASSERT (bitmap_all (free_map, sector, cnt));
  bitmap_set_multiple (free_map, sector, cnt, false);
  bitmap_write (free_map, free_map_file);
}

/* Opens the free map file and reads it from disk. */
// 从文件中读取free_map的bitmap
void
free_map_open (void) 
{
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_read (free_map, free_map_file))
    PANIC ("can't read free map");
}

/* Writes the free map to disk and closes the free map file. */
// TODO: 这哪来的写回数据???
void
free_map_close (void) 
{
  file_close (free_map_file);
}

/* Creates a new free map file on disk and writes the free map to
   it. */
void
free_map_create (void) 
{
  /* Create inode. */
  if (!inode_create (FREE_MAP_SECTOR, bitmap_file_size (free_map), false))
    PANIC ("free map creation failed");

  /* Write bitmap to file. */
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_write (free_map, free_map_file))
    PANIC ("can't write free map");
}
