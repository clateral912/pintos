#include "filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "cache.h"
#include "file.h"
#include "free-map.h"
#include "inode.h"
#include "directory.h"
#include "../threads/synch.h"

/* Partition that contains the file system. */
// 指向存有filesystem的块设备
struct block *fs_device;
struct lock filesys_lock;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  lock_init(&filesys_lock);
  cache_init();
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
// TODO: 在此处加上把Cache中的数据写回磁盘的操作
void
filesys_done (void) 
{
  cache_writeback_all();
  free_map_close ();
}

// 注意! 现在的open close remove操作都只在根目录下进行!
/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (block_sector_t dir_sector, const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_open(inode_open(dir_sector));
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, false)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (block_sector_t dir_sector, const char *name)
{
  struct dir *dir = dir_open(inode_open(dir_sector));
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (block_sector_t dir_sector, const char *name) 
{
  struct dir *dir = dir_open(inode_open(dir_sector));
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
// 格式化
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, "root",16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
