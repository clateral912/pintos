#ifndef VM_VIRTUAL_MEMORY_H
#define VM_VIRTUAL_MEMORY_H

#include "list.h"
#include <stdint.h>
#include "../threads/palloc.h"
#include "../threads/pte.h"
#include <hash.h>

#define UNMAPPED -1
#define USE_ADDR -1
#define USE_MAPID 0x0

typedef int pid_t;
typedef int mapid_t;

enum location{
  LOC_MEMORY,
  LOC_FILE,
  LOC_SWAP,
  LOC_NOT_PRESENT
};

enum role{
  SEG_STACK,
  SEG_CODE,
  SEG_DATA,
  SEG_MMAP,
  SEG_UNUSED
};

//  以下所有数据结构都定义在内核虚拟内存中, 保证全局可见性


// Frame Table节点
struct frame_node 
{
  bool evictable;                 //是否可驱逐
  void *kaddr;                    //用户页面映射的内核页面的内核虚拟地址
  struct page_node *page_node;    //被某个进程持有的, 辅助页表的页面对象
  struct list_elem elem;          
};

//Supplemental Page Table第一级节点
//以线程为索引, 维护每个线程持有的页面列表
struct process_node
{
  pid_t pid;                //用户进程的pid
  struct hash page_list;    //该进场持有的页面列表
  struct hash_elem helem;
};

//Supplemental Page Table第二级节点
//标记每个页面的具体信息
struct page_node 
{
  pid_t owner;                      //页面持有者: 某个用户进程
  size_t swap_pg_idx;               //页面在swap磁盘上的序列号
  bool sharing;                     //是否支持shairng
  enum location loc;                //页面当前位置: 内存中/swap中/文件中/尚未存在
  enum role role;                   //页面的角色(存储的是哪种类型的数据)
  void *upage;                      //页面的用户虚拟地址, 低12位为0
                                    //用户虚拟地址(uaddr)的高20位(Page Directory Index + Page Table Index), 
  struct frame_node* frame_node;    //如果页面在内存中, 指向一个物理frame对象, 不在内存中则为NULL
  struct hash_elem helem;         
};

#endif // !VM_VIRTUAL_MEMORY_H
