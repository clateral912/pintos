#include "cache.h"
#include <hash.h>
#include <list.h>
#include <stdint.h>
#include <string.h>
#include "../threads/malloc.h"
#include "../threads/palloc.h"
#include "../threads/vaddr.h"
#include "../devices/block.h"
#include "filesys.h"
#include "inode.h"
#include "off_t.h"
#include "stdbool.h"

#define CACHE_SIZE 64

struct inode_cache_entry
{
  block_sector_t start;
  off_t length;
};

struct cache_sector_node
{
  uint8_t cache_idx;
  struct cache_entry *centry;
  void *addr;
  bool accessed;
  bool dirty;

  bool is_inode_sector;
  uint8_t inode_entry_cnt;
  struct hash_elem *inode_helem[64];
  struct list_elem elem;
};

struct cache_entry
{
  bool is_inode;
  block_sector_t sector;
  void *cache_addr;
  struct cache_sector_node *cnode;    //所属的Cache Node
  struct hash_elem helem;
};

void *cache;
uint8_t cache_sectors_cnt;
struct cache_sector_node *free_inode_sector;
struct hash cache_hashmap;
struct list cache_list;
struct list_elem *clist_ptr;

struct cache_sector_node *cache_evict();

bool cache_hash_less(const struct hash_elem *h1, const struct hash_elem *h2, void *aux UNUSED)
{
  struct cache_entry *entry1 = hash_entry(h1, struct cache_entry, helem);
  struct cache_entry *entry2 = hash_entry(h2, struct cache_entry, helem);
  return (entry1->sector < entry2->sector ? true : false);
}

unsigned cache_hash_hash(const struct hash_elem *helem, void *aux UNUSED)
{
  struct cache_entry *entry = hash_entry(helem, struct cache_entry, helem);
  return hash_bytes(&entry->sector, sizeof(block_sector_t));
}

void
cache_init()
{
  list_init(&cache_list);
  hash_init(&cache_hashmap, cache_hash_hash, cache_hash_less, NULL);
  free_inode_sector = calloc(1, BLOCK_SECTOR_SIZE);
  clist_ptr = list_end(&cache_list);
  cache = palloc_get_multiple(PAL_ZERO, ((CACHE_SIZE * BLOCK_SECTOR_SIZE) / PGSIZE));
  if (cache == NULL)
    PANIC("cache_init(): Cannot allocate memory for cache");
}

inline bool 
cache_full()
{
  return cache_sectors_cnt >= 64;
}

struct cache_entry *
cache_seek(block_sector_t sector)
{
  struct cache_entry key_entry;
  key_entry.sector = sector;

  struct hash_elem *helem = hash_find(&cache_hashmap, &key_entry.helem);
  if (helem == NULL)
    return NULL;

  return hash_entry(helem, struct cache_entry, helem);
}


//向某个存储inode元数据的cache_sector中添加inode元数据
void *
cache_add_inode(struct inode_cache_entry *inode_entry, struct cache_entry *centry)
{
  if (free_inode_sector->inode_entry_cnt == 64)
    free_inode_sector = cache_evict();

  free_inode_sector->inode_entry_cnt++;
  free_inode_sector->centry = NULL;  //用SIZE_MAX表示其没有对应的物理磁盘sector
  void *addr = free_inode_sector->addr + free_inode_sector->inode_entry_cnt * sizeof(struct inode_cache_entry);
  // 向addr中写入inode_entry的内容
  free_inode_sector->inode_helem[free_inode_sector->inode_entry_cnt - 1] = &centry->helem;
  *(struct inode_cache_entry *)addr = *inode_entry;
  return addr;
}


// 从磁盘中读入数据到cache中, 返回已经写入内存的文件内容的内存地址
// 若if_read为true, 则从磁盘中读取数据
void *
cache_fill(block_sector_t disk_sector, bool is_inode, bool if_read)
{
  void *buffer = calloc(1, BLOCK_SECTOR_SIZE);
  if (if_read)
    block_read(fs_device, disk_sector, buffer);

  struct cache_entry *centry = calloc(1, sizeof(struct cache_entry));

  if (is_inode)
  {
    struct inode_cache_entry *inode_entry = buffer;
    centry->is_inode    = true;
    centry->sector      = disk_sector;
    centry->cache_addr  = cache_add_inode(inode_entry, centry);
  }
  else 
  {
    uint8_t sector_idx;
    struct cache_sector_node *cnode; 
    
    if (!cache_full())
    {
      cnode = calloc(1, sizeof(struct cache_sector_node));
      sector_idx = ++cache_sectors_cnt;
    }
    else
      cnode = cache_evict();

    cnode->is_inode_sector  = false;
    cnode->addr             = cnode->addr == NULL ? cache + sector_idx * BLOCK_SECTOR_SIZE : cnode->addr;
    cnode->centry           = centry;
    cnode->cache_idx        = sector_idx;
    centry->is_inode        = false;
    centry->sector          = disk_sector;
    centry->cache_addr      = cnode->addr;
    //向cache中写入数据
    memcpy(cnode->addr, buffer, BLOCK_SECTOR_SIZE);
    list_push_back(&cache_list, &cnode->elem);
  }
  hash_insert(&cache_hashmap, &centry->helem);
  free(buffer);
  // TODO: 添加返回false的情况!
  return centry->cache_addr;
}

//将cache中某个sector的内容写回
void
cache_writeback(struct cache_sector_node *cnode)
{
  // 保证cnode指向的一定是一个存储正常文件数据的sector, 而不是存储inode的
  ASSERT(!cnode->is_inode_sector)
  block_write(fs_device, cnode->centry->sector, cnode->addr);
}

void
cache_writeback_all()
{
  struct list_elem *e;
  struct cache_sector_node *cnode;

  for (e = list_begin(&cache_list); e != list_end(&cache_list);)
  {
    cnode = list_entry(e, struct cache_sector_node, elem);
    e = list_next(e);
    cache_writeback(cnode);
  }
}

// 从cache中读取某块数据
// 在cache中寻找某个disk_sector对应的内容, 会留下访问痕迹(accessed位)
// 如果找不到要读取的内容, 就从磁盘中读取内容, 再返回
void 
cache_read(block_sector_t disk_sector, void *buffer, bool is_inode)
{
  void *cache_addr;
  struct cache_entry *centry = cache_seek(disk_sector);
  if (centry == NULL)
    cache_addr = cache_fill(disk_sector, is_inode, true);
  else
    cache_addr = centry->cache_addr;
  
  if (is_inode)
    memcpy(buffer, cache_addr, sizeof(struct inode_cache_entry));
  else
  {
    memcpy(buffer, cache_addr, BLOCK_SECTOR_SIZE);
    // TODO: 异步地进行read_ahead
    // cache_fill(disk_sector + 1, is_inode, true);
  }
  centry->cnode->accessed = true;
}

// 向cache中写入某块数据
// 如果找不到要写入的内容, 就先写入到cache, 随后在某个时机写回到磁盘
void 
cache_write(block_sector_t disk_sector, const void *buffer, bool is_inode)
{
  void *cache_addr;
  struct cache_entry *centry = cache_seek(disk_sector);
  if (centry == NULL)
    cache_addr = cache_fill(disk_sector, is_inode, false);
  else
    cache_addr = centry->cache_addr;

  if (is_inode)
    memcpy(cache_addr, buffer, sizeof(struct inode_cache_entry));
  else
    memcpy(cache_addr, buffer, BLOCK_SECTOR_SIZE);

  centry->cnode->accessed = true;
  centry->cnode->dirty    = true; 
}

struct cache_sector_node *
cache_which_to_evict()
{
  ASSERT(cache_full());
  ASSERT(clist_ptr != NULL);

  bool second_turn = false;
  struct list_elem *old_ptr = clist_ptr;
  struct cache_sector_node *cnode;

  for(;;)
  {
    if(clist_ptr == list_end(&cache_list))
      clist_ptr = list_begin(&cache_list);
    
    cnode = list_entry(clist_ptr, struct cache_sector_node, elem);

    bool accessed = cnode->accessed;
    bool dirty    = cnode->dirty;
    
    if (clist_ptr == old_ptr)
      second_turn = true;
    
    if (!second_turn)
    {
      if (!accessed && !dirty)
      {
        clist_ptr = list_next(clist_ptr);
        return cnode;
      }
    }
    else 
    {
      if (!accessed)
      {
        clist_ptr = list_next(clist_ptr);
        return cnode;
      }
      cnode->accessed = false;
    }
  }
}

struct cache_sector_node *
cache_evict()
{
  struct cache_sector_node *cnode = cache_which_to_evict();
  
  if(cnode->dirty && !cnode->is_inode_sector)
    cache_writeback(cnode);

  cnode->accessed = false;
  cnode->dirty    = false;

  if (!cnode->is_inode_sector)
  {
    struct hash_elem *helem = hash_delete(&cache_hashmap, &cnode->centry->helem);
    ASSERT(helem != NULL);
    free(cnode->centry);
    cnode->centry = NULL;
  }
  else 
  {
    for (int i = 0; i < cnode->inode_entry_cnt; i++)
    {
      struct cache_entry *inode_centry = hash_entry(cnode->inode_helem[i], struct cache_entry, helem);
      struct hash_elem *helem = hash_delete(&cache_hashmap, cnode->inode_helem[i]);
      ASSERT(helem != NULL);
      free(inode_centry);
    }

    cnode->is_inode_sector = false;
    cnode->inode_entry_cnt = 0;
    memset(cnode->inode_helem, 0, sizeof(cnode->inode_helem));
  }
  
  return cnode;
}
