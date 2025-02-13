#include "frame.h"
#include <list.h>
#include <stdint.h>
#include "../threads/malloc.h"
#include "../threads/thread.h"
#include "../userprog/pagedir.h"
#include "stdbool.h"

struct list frame_list;

void frame_init()
{
  list_init(&frame_list);
}

// 为进程分配一页新的内存页面, 先获取一页kapge,
// 再将kpage映射到upage上
bool frame_allocate_page(uint32_t *pd, const void *uaddr, bool writable, bool zeroed, bool evictable)
{
  ASSERT(is_user_vaddr(uaddr));

  struct frame_node *node;
  node = malloc(sizeof(struct frame_node));

  bool success;

  uint32_t flag = zeroed ? PAL_ZERO : 0;
  flag = flag | PAL_USER;

  void *kpage = palloc_get_page(flag);
  if (kpage == NULL)
    return false;

  void *upage = pagedir_get_page(pd, uaddr);

  success = pagedir_set_page(pd, upage, kpage, writable);
  if (!success)
    palloc_free_page(kpage);

  node->evictable = evictable;
  node->pte = *(lookup_page(pd, upage, false));
  node->paddr = (uint32_t)kpage;

  list_push_back(&frame_list, &node->elem);

  return success;
}

