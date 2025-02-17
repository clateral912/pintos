#include "page.h"
#include "bitmap.h"
#include "frame.h"
#include <hash.h>
#include "../threads/malloc.h"
#include "../filesys/file.h"
#include "../filesys/filesys.h"
#include "../threads/palloc.h"
#include "../threads/synch.h"
#include "../userprog/pagedir.h"
#include <list.h>
#include "stdbool.h"
#include "swap.h"
#include "virtual-memory.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct hash process_list;
struct lock process_list_lock;
uint32_t page_cnt;

void page_free_multiple(struct thread *t, const void *begin, const void *end);
static void page_mmap_readin(struct thread *t, void *uaddr);

static unsigned 
page_process_hash_hash(const struct hash_elem *elem, void *aux UNUSED)
{
  struct process_node *node = hash_entry(elem, struct process_node, helem);
  return hash_bytes(&node->pid, sizeof(pid_t));
}

static bool 
page_process_hash_less(const struct hash_elem *e1, const struct hash_elem *e2, void *aux UNUSED)
{
  struct process_node *node1 = hash_entry(e1, struct process_node, helem);
  struct process_node *node2 = hash_entry(e2, struct process_node, helem);

  return (node1->pid < node2->pid ? true : false);
}

static unsigned 
page_hash_hash(const struct hash_elem *elem, void *aux UNUSED)
{
  struct page_node *node = hash_entry(elem, struct page_node, helem);
  return hash_bytes(&node->upage, sizeof(uintptr_t));
}

static bool 
page_hash_less(const struct hash_elem *e1, const struct hash_elem *e2, void *aux UNUSED)
{
  struct page_node *node1 = hash_entry(e1, struct page_node, helem);
  struct page_node *node2 = hash_entry(e2, struct page_node, helem);

  return (node1->upage < node2->upage ? true : false);
}

//在SPT的Process List中寻找Process node 
//返回指向process node的指针
static struct process_node *
find_process_node(struct thread *t)
{
  struct process_node *process; 
  struct process_node key_process;
  key_process.pid = t->tid;

  //寻找对应的process
  lock_acquire(&process_list_lock);
  struct hash_elem *process_helem = hash_find(&process_list, &key_process.helem);
  lock_release(&process_list_lock);

  if (process_helem == NULL)
  {
    // printf("find_process(): Cannot find process whose name is %s, pid is %d\n", t->name, t->tid);
    return NULL;
  }

  process = hash_entry(process_helem, struct process_node, helem); 
  return process;
}

// 在SPT的各个线程的pagelist中查找uaddr对应的page node 
// 返回指向page node的指针
static struct page_node *
find_page_node(struct process_node *pnode, const void *uaddr)
{
  struct page_node key_page;
  struct page_node *page;

  key_page.upage = pg_round_down(uaddr);

  struct hash_elem *page_helem = hash_find(&pnode->page_list, &key_page.helem);
  if (page_helem == NULL)
  { 
    //printf("find_page(): Cannot find page in process %d whose uaddr is %d\n", pnode->pid, (uint32_t)uaddr);
    return NULL;
  }
  page = hash_entry(page_helem, struct page_node, helem);

  return page;
}

void 
page_init()
{
  hash_init(&process_list, page_process_hash_hash, page_process_hash_less, NULL); 
  lock_init(&process_list_lock);
  page_cnt = 0;
}

void 
page_process_init(struct thread *t)
{
  struct process_node *process_node;
  process_node = malloc(sizeof(struct process_node));

  if (process_node == NULL)
    PANIC("Cannot allocate memory to store a hash map for process!\n");

  process_node->pid = t->tid;

  hash_init(&process_node->page_list, page_hash_hash, page_hash_less , NULL); 

  lock_acquire(&process_list_lock);
  hash_insert(&process_list, &process_node->helem);
  lock_release(&process_list_lock);
}

// 根据uaddr创建一个Supplemental Page Table对象(Page Node)
// 完成初始化并将其加入对应进程持有的页面目录中
// 返回一个Page Node对象
struct page_node *
page_add_page(struct thread *t, const void *uaddr, uint32_t flags, enum location loc, enum role role)
{
  ASSERT(role != SEG_UNUSED);
  //保证添加的pte一定对应一个已经存在的页
  if (loc == LOC_MEMORY)
  {
    ASSERT(lookup_page(t->pagedir, uaddr, false) != NULL);
  }
  //准备用来查找的key结构体
  struct process_node *process_node;
  struct page_node *node;
  //为Page Node分配内存
  node = malloc(sizeof(struct page_node));
  if (node == NULL)
    PANIC("Cannot allocate memory to store a page in SPT!\n");

  //初始化Page Node
  node->owner       = t;
  node->upage       = pg_round_down(uaddr);;
  node->sharing     = flags & PG_SHARING;
  node->loc         = loc;
  node->frame_node  = NULL;
  node->role        = role;
  node->swap_pg_idx = SIZE_MAX;

  process_node = find_process_node(t); 
  ASSERT(process_node != NULL);
  //保证每个进程获取的内存页面不超过1024页(4GiB)
  ASSERT(process_node->page_list.elem_cnt <= 1024);
  //在Process Node的hashmap中插入Page Node
  bool success = (hash_insert(&process_node->page_list, &node->helem) == NULL) ? true : false;
  
  if (!success)
  {
    free(node);
    return NULL;
  }
  
  page_cnt++;

  return node;
}

void
page_print_vm_stat()
{
  printf("frame: %d, page: %d\n", frame_cnt, page_cnt);
  printf("kernel pool remaining pages: %zu\n", bitmap_count(kernel_pool.used_map, 0, kernel_pool.used_map->bit_cnt, false));
}
// 在SPT中寻找uaddr对应的页面对象(Page Node)
struct page_node *
page_seek(struct thread *t, const void *uaddr)
{
  struct process_node *process_node = find_process_node(t);
  if (process_node == NULL)
    return NULL;

  struct page_node *page_node = find_page_node(process_node, uaddr);
  return page_node;
}

//用于销毁进程持有的Pagelist的辅助函数
//用于释放物理页帧frame, 并释放Page Node的硬件资源(free(node))
static void
page_page_destructor(struct hash_elem *helem, void *aux)
{
  struct thread *t = aux;
  struct page_node *node = hash_entry(helem, struct page_node, helem);
  if(node->loc == LOC_MEMORY)
    frame_destroy_frame(node->frame_node);
  pagedir_clear_page(t->pagedir, node->upage);
  free(node);
}

//完全销毁一个进程持有的Page List, 释放其所有持有的页面
//释放所有页面对应的内存
//TODO: 当前未实现清除swap中的内容!!!!!!!
void 
page_destroy_pagelist(struct thread *t)
{
  struct process_node *process = find_process_node(t);
  ASSERT(process != NULL);
  //摧毁前传入辅助参数: 线程句柄
  process->page_list.aux = t;

  //摧毁该进程持有的pagelist
  hash_destroy(&process->page_list, page_page_destructor);

  // 在process_list中删除该process
  lock_acquire(&process_list_lock);
  hash_delete(&process_list, &process->helem);
  lock_release(&process_list_lock);
}

// 在page和frame之间建立链接, 把空闲的frame分配给一个Page node
void
page_assign_frame(struct thread *t, struct page_node *pnode, struct frame_node *fnode, bool writable)
{
  ASSERT(pnode->loc != LOC_MEMORY);
  ASSERT(pnode->frame_node == NULL)
  ASSERT(fnode->page_node == NULL);
  ASSERT(fnode->kaddr != NULL);

  // 获取upage, 即uaddr的高20位, 低12位为0 
  void *upage = pnode->upage;

  // pagedir_set_page中分配了upage的PTE并创建其所在的页表
  bool success = pagedir_set_page(t->pagedir, upage, fnode->kaddr, writable);
  if (!success)
  {
    free(fnode);
    printf("page_assign_frame(): Cannot set kpage to upage!\n");
    return ;
  }


  pnode->frame_node = fnode;
  fnode->page_node  = pnode;

  pnode->loc = LOC_MEMORY;
}

// 直接为uaddr快速分配内存页面
// 从物理内存中获取一帧frame
// 并在当前进程的process page list中添加一个node 
// 再把它们链接起来
bool
page_get_new_page(struct thread *t, const void *uaddr, uint32_t flags, enum role role)
{
  // 若文件长度不是PGSIZE的整数倍, 你需要在最末尾的页面, 且不是文件的部分上填充0
  if (role == SEG_MMAP)
    flags |= FRM_ZERO;

  struct frame_node *fnode = frame_allocate_page(t->pagedir, flags);
  struct page_node  *pnode = page_add_page(t, uaddr, flags, LOC_NOT_PRESENT, role);
  ASSERT(pnode != NULL);
  //如果fnode为NULL, 说明我们需要进行页面驱逐!
  if (fnode == NULL)
    fnode = frame_evict();

  page_assign_frame(t, pnode, fnode, !(flags & FRM_RO));
  if (role == SEG_MMAP)
    page_mmap_readin(t, (void *)uaddr);

  return true;
}

// 释放页面
// 如果在所有页面列表中找不到page, 那么直接忽略, 不会有后续清除的行为
void
page_free_page(struct thread *t, const void *uaddr)
{
  struct page_node *page_node = page_seek(t, uaddr);
  if (page_node == NULL)
    return ;

  struct process_node *process_node = find_process_node(t);
  ASSERT(process_node != NULL);

  struct hash_elem *helem = hash_delete(&process_node->page_list, &page_node->helem);
  ASSERT(helem != NULL);

  page_page_destructor(&page_node->helem, (void *)t);
}

//释放一整段内存上的page
void
page_free_multiple(struct thread *t, const void *begin, const void *end)
{
  ASSERT(begin < end);
  begin = pg_round_down(begin);

  while(begin < end)
  {
    page_free_page(t, begin);
    begin = ((uint8_t *)begin) + PGSIZE;
  }
}

// 双模式查找, 可提供mapid或addr.
struct mmap_vma_node *
page_mmap_seek(struct thread *t, mapid_t mapid, const void *addr)
{
  struct list *mmap_list = &t->vma.mmap_vma_list;
  struct list_elem *e;
  struct mmap_vma_node *mnode;

  for (e = list_begin(mmap_list); e != list_end(mmap_list); e = list_next(e))
  {
    mnode = list_entry(e, struct mmap_vma_node, elem);
    if (mnode->mapid == mapid)
      return mnode;
    if (addr >= mnode->mmap_seg_begin && addr < mnode->mmap_seg_end)
      return mnode;
  }
  
  return NULL;
}

//检查传入的uaddr在数据段中的合法性, 返回其位于哪个数据段(role)
//若uaddr不合法, 返回SEG_UNUSED
enum role
page_check_role(struct thread *t, const void *uaddr, bool writable)
{
  void *esp = t->vma.stack_seg_begin;
  // 注意! 地址不包含end!
  if (uaddr >= t->vma.code_seg_begin && uaddr < t->vma.code_seg_end)
    return SEG_CODE;

  if (uaddr >= t->vma.data_seg_begin && uaddr < t->vma.data_seg_end)
    return SEG_DATA;

  if (uaddr >= t->vma.stack_seg_begin && uaddr < t->vma.stack_seg_end)
    return SEG_STACK;

  // uaddr位于Code或Data段的边缘, 且正在加载exe, 且可读写:
  // 该段为Data段!
  if ((uaddr == t->vma.code_seg_end || uaddr == t->vma.data_seg_end)
      && 
      t->vma.loading_exe 
      && 
      writable)
    return SEG_DATA;
  //如果uaddr恰好位于code段的最高位并且当前正在读取exe文件, 且只读
  //说明访问是合法的, 且uaddr应该是code段的地址
  if (uaddr == t->vma.code_seg_end && t->vma.loading_exe && !writable)
    return SEG_CODE;

  if (uaddr == esp - 4 || uaddr == esp - 32)
    return SEG_STACK;
  
  if (page_mmap_seek(t, USE_ADDR, uaddr) != NULL)
    return SEG_MMAP;

  return SEG_UNUSED;
}

// 监测需要mmap的内存区域与其他内存区域是否有重叠
static bool
page_mmap_overlap(struct thread *t, void *addr, size_t filesize)
{
  void *begin = addr;
  void *end   = (uint8_t *)(addr) + filesize;

  if (!(begin >= t->vma.code_seg_end || end <= t->vma.code_seg_begin))
    return false;

  if (!(begin >= t->vma.data_seg_end || end <= t->vma.data_seg_begin))
    return false;

  if (end >= t->vma.stack_seg_begin)
    return false;

  struct list *mmap_list = &t->vma.mmap_vma_list;
  struct list_elem *e;
  struct mmap_vma_node *mnode;

  for (e = list_begin(mmap_list); e != list_end(mmap_list); e = list_next(e))
  {
    mnode = list_entry(e, struct mmap_vma_node, elem);
    if (!(begin >= mnode->mmap_seg_end || end <= mnode->mmap_seg_begin))
      return false;
  }

  return true;
}

inline static mapid_t
page_allocate_mapid(struct thread *t)
{
  return (++t->vma.mapid);
}

static void
page_mmap_readin(struct thread *t, void *uaddr)
{
    // 必须整页读取文件
    uaddr = pg_round_down(uaddr);
    //  找到对应的文件node
    struct mmap_vma_node *mnode = page_mmap_seek(t, USE_ADDR, uaddr);     
    if(mnode == NULL)
      PANIC("Cannot find mmap mapping accroding to uaddr!\n");

    // IMPORTANT: 必须记录并恢复old_pos!
    size_t old_pos = file_tell(mnode->file);
    size_t filesize = mnode->mmap_seg_end - mnode->mmap_seg_begin;
    // 只读取一页的内容
    // 准备读取文件, 移动文件指针到pos位置
    size_t pos = uaddr - mnode->mmap_seg_begin;
    uint32_t read_bytes = filesize - pos >= PGSIZE ? PGSIZE : filesize - pos;
    file_seek(mnode->file, pos);
    if (file_read(mnode->file, (void *)uaddr, read_bytes) != read_bytes)
    {
      page_free_page(t, uaddr);
      PANIC("page_get_new_page(): read bytes for mmap file failed!\n");
    }
    // 清除dirty与accessed位, 避免后续误判
    pagedir_set_accessed(t->pagedir, uaddr, false);
    pagedir_set_dirty(t->pagedir, uaddr, false); 
    file_seek(mnode->file, old_pos);
}

void 
page_mmap_writeback(struct thread *t, mapid_t mapid)
{
  struct mmap_vma_node *mnode = page_mmap_seek(t, mapid, USE_MAPID);
  ASSERT(mnode != NULL);

  struct file *file = mnode->file;
  ASSERT(file != NULL);

  void *addr      = mnode->mmap_seg_begin;
  void *end       = mnode->mmap_seg_end; 
  size_t filesize = end - addr;
  size_t old_pos  = mnode->file->pos;
  // IMPORTANT: 这里必须记录并恢复文件原来的pos!!!
  // 否则在写回后, 很可能会读取到错误的数据!!

  while(addr < end)
  {
    // 只对写回有修改的页面
    if (pagedir_is_accessed(t->pagedir, addr)
        &&
        pagedir_is_dirty(t->pagedir, addr))
    {
      size_t pos = addr - mnode->mmap_seg_begin;
      uint32_t write_bytes = filesize - pos >= PGSIZE ? PGSIZE : filesize - pos;
      file_seek(file, pos);
      if (file_write(file, addr, write_bytes) != write_bytes)
        PANIC("page_mmap_writeback(): Cannot write back to file!\n");
    }

    addr = (uint8_t *)(addr) + PGSIZE;
  }
  file_seek(mnode->file, old_pos);

}

mapid_t
page_mmap_map(struct thread *t, uint32_t fd, struct file *file, void *addr)
{
  // fd是否为0和1的检查需要在syscall中做！
  if (pg_ofs(addr) != 0 || addr == NULL)
    return -1;

  struct mmap_vma_node *node;
  node = malloc(sizeof(struct mmap_vma_node));
  if (node == NULL)
  {
    printf("page_mmap_create(): Cannot allocate memory for mmap_vma_node!\n");
    return -1;
  }

  size_t filesize = file_length(file);
  if (filesize == 0)
  {
    free(node);
    return -1;
  }
  if (page_mmap_overlap(t, addr, filesize))
  {
    node->mapid           = page_allocate_mapid(t);
    node->mmap_seg_begin  = addr;
    node->mmap_seg_end    = (uint8_t *)(addr) + filesize;
    node->file            = file;
    node->fd              = fd;
  }
  else
  {
    free(node);
    return -1;
  }

  list_push_back(&t->vma.mmap_vma_list, &node->elem);

  return node->mapid;
}

void
page_mmap_unmap(struct thread *t, mapid_t mapid)
{
  struct mmap_vma_node *mnode = page_mmap_seek(t, mapid, USE_MAPID);
  if (mnode == NULL)
    return ;

  page_mmap_writeback(t, mapid);
  page_free_multiple(t, mnode->mmap_seg_begin, mnode->mmap_seg_end); 
  list_remove(&mnode->elem);
  free(mnode);
}

// unmap一个进程持有的所有mmap对象
// 一般在进程结束时调用
void
page_mmap_unmap_all(struct thread *t)
{
  struct list *mmap_list = &t->vma.mmap_vma_list;
  struct list_elem *e;
  struct mmap_vma_node *mnode;

  for (e = list_begin(mmap_list); e != list_end(mmap_list);)
  {
    mnode = list_entry(e, struct mmap_vma_node, elem);
    // IMPORTANT: 必须在unmap前获取下一个节点!!
    // unmap后节点被销毁, list_next()得到的是无效节点!!
    e = list_next(e);
    page_mmap_unmap(t, mnode->mapid);
  }
}

void
page_set_rw(struct thread *t, const void *uaddr, bool writable)
{
  uint32_t *pte = lookup_page(t->pagedir, uaddr, false);
  ASSERT(pte != NULL);
  if (!writable)
    *pte &= ~(uint32_t)PTE_W;
  else 
    *pte |=  PTE_W;
}

// 当发生Page Fault且进程发现自己持有某个页面
// 但这个页面不在内存中, 我们需要把页面从文件或swap中拉取过来
void
page_pull_page(struct thread *t, struct page_node *pnode)
{
  ASSERT(pnode != NULL);
  ASSERT(pnode->loc != LOC_MEMORY && pnode->loc != LOC_NOT_PRESENT);

  struct frame_node *fnode = frame_evict();
  // 默认可读写, 能被换出的页面一定是可读写的!
  // 不可能有只读页面被换出!
  page_assign_frame(t, pnode, fnode, true);

  //接下来把文件内容复制到内存中
  if (pnode->role == SEG_MMAP)
    page_mmap_readin(t, pnode->upage);
  else if(pnode->role == SEG_STACK || pnode->role == SEG_DATA)
  {
    ASSERT(pnode->swap_pg_idx != SIZE_MAX);
    swap_out(pnode->swap_pg_idx, pnode->upage);
  }

  pnode->loc = LOC_MEMORY;
}

