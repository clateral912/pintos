#include "frame.h"
#include <list.h>
#include <stdint.h>
#include <stdio.h>
#include "../threads/malloc.h"
#include "../threads/thread.h"
#include "../userprog/pagedir.h"
#include "stdbool.h"
#include "virtual-memory.h"

struct list frame_list;

void frame_init()
{
  list_init(&frame_list);
}

// TODO: 尚未实现evict功能!
// 根据uaddr, 分配uaddr所在的页面
// 为进程分配一页新的内存页面, 先获取一页kapge,
// 再将kpage映射到upage上
// upage: 是一个用户内存空间内的地址, 且低12位为0
struct frame_node *
frame_allocate_page(uint32_t *pd, const void *uaddr, uint32_t flags)
{
  ASSERT(is_user_vaddr(uaddr));

  //在内核虚拟内存上为frame_node分配内存
  struct frame_node *node;
  node = malloc(sizeof(struct frame_node));

  bool success;
  //解码各个flag
  bool zeroed     = flags & FRM_ZERO;
  bool writable   = flags & FRM_RW;
  bool evictable  = !(flags & FRM_NO_EVICT);

  // 在用户内存池中获取一个用户页面
  // 为什么用户页面要叫kpage? 因为其位于内核虚拟内存中
  uint32_t palloc_flag = zeroed ? PAL_ZERO : 0;
  palloc_flag = palloc_flag | PAL_USER;

  void *kpage = palloc_get_page(palloc_flag);
  if (kpage == NULL)
  {
    printf("frame_allocate_page(): Cannot allocate memory for kpage!\n");
    return NULL;
  }

  // 获取upage, 即把uaddr右移12位
  void *upage = pg_round_down(uaddr);

  // pagedir_set_page中分配了upage的PTE并创建其所在的页表
  success = pagedir_set_page(pd, upage, kpage, writable);
  if (!success)
  {
    printf("frame_allocate_page(): Cannot set kpage to upage!\n");
    return NULL;
  }

  node->evictable = evictable;
  node->kaddr = kpage;
  node->page_node = NULL;

  list_push_back(&frame_list, &node->elem);

  return node;
}

//完全销毁一个frame对象, 释放其对应的upage与kpage的内存空间
//删除所有引用关系
void 
frame_destroy_frame(struct frame_node *fnode)
{
  ASSERT(fnode != NULL);

  fnode->page_node->frame_node = NULL;
  palloc_free_page(fnode->kaddr);
  list_remove(&fnode->elem);
  free(fnode);
}
