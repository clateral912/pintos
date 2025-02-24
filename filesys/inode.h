#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "off_t.h"
#include "../devices/block.h"

#define DIRECT_BLOCKS 5
#define INDIRECT_BLOCKS 128
struct bitmap;


/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    block_sector_t direct[DIRECT_BLOCKS];               /* First data sector. */
    block_sector_t indirect;
    block_sector_t double_indirect;
    unsigned magic;                     /* Magic number. */
    uint32_t unused[119];               /* Not used. */
  };

void inode_init (void);
bool inode_create (block_sector_t, off_t);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
block_sector_t byte_to_sector (const struct inode *inode, off_t pos);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);

#endif /* filesys/inode.h */
