#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "list.h"
#include <stdint.h>
#include "../threads/palloc.h"
#include "../threads/pte.h"
#include <hash.h>

typedef int pid_t;

enum location{
  LOC_MEMORY,
  LOC_FILE,
  LOC_SWAP,
  LOC_NOT_PRESENT
};

struct frame_node 
{
  bool evictable;
  uint32_t pte;
  uint32_t paddr;
  struct page_node *page_node;
  struct list_elem elem;
};

struct page_node 
{
  pid_t owner;
  bool sharing;
  enum location loc;
  uint32_t pte;
  struct frame_node* frame_node;
  struct hash_elem helem;
};

extern struct list frame_list;

void frame_init();
bool frame_allocate_page(uint32_t *pd, const void *uaddr, bool writable, bool zeroed, bool evictable);

#endif
