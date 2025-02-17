#include "frame.h"
#include "page.h"
#include <list.h>
#include <stdint.h>
#include <stdio.h>
#include "../threads/malloc.h"
#include "../threads/thread.h"
#include "../userprog/pagedir.h"
#include "stdbool.h"
#include "swap.h"
#include "virtual-memory.h"

#define NO_ACCESSED 0;

struct list frame_list;
struct list_elem *flist_ptr;
uint32_t frame_cnt;

void frame_init()
{
  list_init(&frame_list);
  flist_ptr = list_begin(&frame_list);
  frame_cnt = 0;
}

// TODO: 尚未实现evict功能!
// 根据uaddr, 分配uaddr所在的页面
// 为进程分配一页新的内存页面, 先获取一页kapge
// 将upage与kpage建立联系需要在page_assign_frame()中实现!
// 该函数只实现在物理上分配一页可用的内存
// 并不关心这一页内存被哪个upage所映射
struct frame_node *
frame_allocate_page(uint32_t *pd, uint32_t flags)
{
  //在内核虚拟内存上为frame_node分配内存
  struct frame_node *node;
  node = malloc(sizeof(struct frame_node));

  bool success;
  //解码各个flag
  bool zeroed     = flags & FRM_ZERO;
  bool writable   = !(flags & FRM_RO);
  bool evictable  = !(flags & FRM_NO_EVICT);

  // 在用户内存池中获取一个用户页面
  // 为什么用户页面要叫kpage? 因为其位于内核虚拟内存中
  uint32_t palloc_flag = zeroed ? PAL_ZERO : 0;
  palloc_flag = palloc_flag | PAL_USER;

  void *kpage = palloc_get_page(palloc_flag);
  if (kpage == NULL)
  {
    free(node);
    return NULL;
  }

  node->evictable = evictable;
  node->kaddr = kpage;
  node->page_node = NULL;

  list_push_back(&frame_list, &node->elem);

  flist_ptr = &node->elem;
  frame_cnt++;

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

//将内存中的页面换入文件中或swap磁盘中
static void
frame_swap(struct thread *t, struct frame_node *fnode, bool dirty)
{
  struct page_node *pnode = fnode->page_node;
  void *upage = pnode->upage;
  size_t page_idx = SIZE_MAX;

  // 如果页面是脏页, 那么需要写回到文件或磁盘中
  if (dirty)
  {
    if (pnode->role == SEG_MMAP)
    {
      struct mmap_vma_node *mnode = page_mmap_seek(t, USE_ADDR, upage);
      page_mmap_writeback(t, mnode->mapid);
    } 
    else
      page_idx = swap_in(upage);
  }
  
  // 将此页的"Present"标志位清零, 确保下一次进程访问该页面时会发生Page Fault
  pagedir_clear_page(t->pagedir, upage);
  pnode->swap_pg_idx  = page_idx;
  pnode->loc          = pnode->role == SEG_MMAP ? LOC_FILE : LOC_SWAP;
  pnode->frame_node   = NULL;
  fnode->page_node    = NULL;
}

// 在当前的frame table中按照[改进版]Clock算法驱逐出一页(放入swap磁盘)
// 随后返回被驱逐后已经可用的frame_node
// 注意! 没有清空frame的内容!
// IMPORTANT: 在多进程的情况下, frame_list中的frame有各自的主人!
// 必须对owner做判断! 
struct frame_node *
frame_evict()
{
  struct frame_node *fnode;
  struct list_elem *e;
  
  bool second_turn = false;

  struct list_elem *old_ptr = flist_ptr;
  
  for (;;)
  {
    // 我们的链表是有头尾节点的, 头尾节点是不在任何node中的


    if (flist_ptr == list_end(&frame_list))
      flist_ptr = list_begin(&frame_list);

    fnode = list_entry(flist_ptr, struct frame_node, elem);
    void *upage = fnode->page_node->upage;
    // 必须按照进程来访问pagedir!
    struct thread *t = fnode->page_node->owner;
    // 如果找不到pte则创建一个
    // 为什么找不到? 因为进程切换, 页目录(Page Directory)也切换了!
    // 若前一个进程
    uint32_t *pte = lookup_page(t->pagedir, upage, false);
    ASSERT(pte != NULL)
    
    bool accessed  = pagedir_is_accessed(t->pagedir, upage); 
    bool dirty     = pagedir_is_dirty(t->pagedir, upage);
    bool writable  = *pte & PTE_W;

    if (flist_ptr == old_ptr)
      second_turn = true;

    if (fnode->evictable && writable)
    {
      if (!second_turn)
      {
        // 第一次轮询, 我们不修改access位
        if (!accessed && !dirty)
        {
          frame_swap(t, fnode, dirty);
          return fnode;
        }
      }
      else 
      {
        //已经进入第二轮, 说明我们在遍历整个frame_list后都没有找到
        //既未被访问也没有修改的页面, 接下来我们找一找未访问过且为脏的页面
        if (!accessed)
        {
          frame_swap(t, fnode, dirty);
          return fnode;
        }
        // 将access位设置为false
        pagedir_set_accessed(t->pagedir, upage, false);
      }
    }

    flist_ptr = list_next(flist_ptr);
  }    
}

