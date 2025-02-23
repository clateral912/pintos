#ifndef FILESYS_INDEX_H
#define FILESYS_INDEX_H  

#include "inode.h"
#include "off_t.h"

void index_init();
void index_where_the_sector(off_t length, uint8_t *level, uint8_t *idx1, uint8_t *idx2);
bool index_extend(struct inode_disk *data, off_t new_length);
void index_relese_sectors(struct inode_disk *data);

#endif // !FILESYS_INDEX_H
