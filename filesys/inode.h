#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include <list.h>
#include "off_t.h"
#include "../devices/block.h"

#define DIRECT_BLOCKS 5
#define INDIRECT_BLOCKS 128
struct bitmap;


/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    bool is_dir;
    off_t length;                       /* File size in bytes. */
    block_sector_t direct[DIRECT_BLOCKS];               /* First data sector. */
    block_sector_t indirect;
    block_sector_t double_indirect;
    unsigned magic;                     /* Magic number. */
    char unused[471];               /* Not used. */
  };

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
  };

void inode_init (void);
bool inode_create (block_sector_t, off_t, bool);
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
bool inode_is_dir(const struct inode *);

#endif /* filesys/inode.h */
