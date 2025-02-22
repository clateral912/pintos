#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "stdbool.h"
#include "../devices/block.h"

void cache_init(void);
void cache_writeback_all(void);
void cache_read(block_sector_t disk_sector, void *buffer, bool is_inode);
void cache_write(block_sector_t disk_sector, const void *buffer, bool is_inode);
void *cache_find_inode(block_sector_t sector);

#endif // !FILESYS_CACHE_H
