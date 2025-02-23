#include "syscall.h"
#include <stddef.h>
#include <string.h>
#include "../devices/shutdown.h"
#include "../devices/input.h"
#include <round.h>
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "../threads/thread.h"
#include "../threads/vaddr.h"
#include "../filesys/filesys.h"
#include "../filesys/cache.h"
#include "../vm/frame.h"
#include "../vm/page.h"
#include "../vm/virtual-memory.h"
#include "process.h"
#include "stdbool.h"
#include "stdio.h"

#define NO_LIMIT 32768    //32768是随便想出来的一个maigic number
#define ERROR -1

static void retval(struct intr_frame *, int32_t);
static void syscall_handler (struct intr_frame *);
static void syscall_tell(struct intr_frame *);
static void syscall_seek(struct intr_frame *);
static void syscall_write(struct intr_frame *);
static void syscall_read(struct intr_frame *);
static void syscall_filesize(struct intr_frame *);
static void syscall_create(struct intr_frame *);
static void syscall_remove(struct intr_frame *);
static void syscall_open(struct intr_frame *);
static void syscall_close(struct intr_frame *);
static void syscall_filesize(struct intr_frame *);
static void syscall_exec(struct intr_frame *);
static void syscall_wait(struct intr_frame *);

// arg0 位于栈中的低地址
struct syscall_frame_3args{
  uint32_t arg0;
  uint32_t arg1;
  uint32_t arg2;
};

struct syscall_frame_2args{
  uint32_t arg0;
  uint32_t arg1;
};

//从中断帧指向的栈中提取出指向参数的指针
static uint32_t *
get_args(struct intr_frame *f)
{
  uint32_t *args_ptr = ((uint32_t *)(f->esp) + 1);
  if (!is_user_vaddr(args_ptr))
    syscall_exit(f, FORCE_EXIT);

  return args_ptr;
}

static inline
void
retval(struct intr_frame *f, int32_t num)
{
  f->eax = num;  
}


static void
syscall_create(struct intr_frame *f)
{
  uint32_t *args_ptr = get_args(f); 

  struct syscall_frame_2args* args = (struct syscall_frame_2args *)args_ptr;
  
  const char *name = (char *)args->arg0;
  uint32_t initial_size = args->arg1;

  if (!is_user_vaddr(name) || name == NULL)
    syscall_exit(f, FORCE_EXIT);

  bool success = filesys_create(name, initial_size);
  
  retval(f, success);
}

static void
syscall_remove(struct intr_frame *f)
{
  const char *file = (const char *)(*get_args(f));

  if (!is_user_vaddr(file) || file == NULL)
    syscall_exit(f, -1);

  bool success = filesys_remove(file);
  retval(f, success);
}

static void
syscall_seek(struct intr_frame *f)
{
  uint32_t *args_ptr = get_args(f); 

  struct syscall_frame_2args* args = (struct syscall_frame_2args *)args_ptr;

  uint32_t fd = args->arg0;
  uint32_t pos = args->arg1;

  struct file *file = process_from_fd_get_file(thread_current(), fd);
  if (file == NULL)
    return ;

  file_seek(file, pos);
}

static void
syscall_tell(struct intr_frame *f)
{
  uint32_t fd = *(get_args(f));

  struct file* file = process_from_fd_get_file(thread_current(), fd);
  if (file == NULL)
    return ;

  uint32_t pos = file_tell(file);
  retval(f, pos);
}

static void
syscall_filesize(struct intr_frame *f)
{
  uint32_t fd = *(get_args(f));

  struct file *file = process_from_fd_get_file(thread_current(), fd);
  if (file == NULL)
  {
    retval(f, ERROR);
    return ;
  }
  size_t size = file_length(file);
  retval(f, size);
}

static void
syscall_open(struct intr_frame *f)
{
  uint32_t fd;
  const char *file_name = (const char *)(*get_args(f));
  if (!is_user_vaddr(file_name) || file_name == NULL)
  {
    retval(f, ERROR);
    return ;
  }

  struct file *file = filesys_open(file_name);
  // file==NULL的情况有内部内存分配错误, 以及未找到文件
  // 未找到文件的情况在此处处理
  // TODO: 逻辑漏洞! 如果是内部内存错误怎么办?
  // 从测试点的逻辑倒推, 打开文件失败应该正常退出才对
  // 而不是把进程杀死
  if (file == NULL)
  {
    retval(f, ERROR);
    return ;
  }
  fd = process_create_fd_node(thread_current(), file);
  retval(f, fd);
}

static void
syscall_close(struct intr_frame *f)
{
  struct thread *cur = thread_current();
  int fd = *(get_args(f));
  if (fd == 1 || fd == 0)
    return ;

  struct file *file = process_from_fd_get_file(cur, fd);
  
  if (file == NULL) 
    return ;

  struct fd_node *fnode = process_get_fd_node(cur, fd);
  ASSERT(fnode != NULL);

  if(fnode->mapid != UNMAPPED)
  {
    struct mmap_vma_node *mnode = page_mmap_seek(cur, fnode->mapid, USE_MAPID);
    ASSERT(mnode != NULL);
    mnode->file = file_reopen(file);
  }

  file_close(file); 
  process_remove_fd_node(cur, fd);
  cache_writeback_all();
}

static void
syscall_read(struct intr_frame *f)
{
  struct thread *cur = thread_current();
  uint32_t *args_ptr = get_args(f);  
  struct syscall_frame_3args *args = (struct syscall_frame_3args *)(args_ptr);

  uint32_t fd = args->arg0;
  void *buffer = (void *)args->arg1;
  size_t size = args->arg2;
  
  if (!is_user_vaddr(buffer) || buffer == NULL)
    syscall_exit(f, FORCE_EXIT);

  //读取stdout是不可行的!
  if (fd == 1)
    return ;

  if (fd == 0)
  {
    // 手册说的不清楚! 当fd为0时返回什么?
    uint8_t key = input_getc();
    retval(f, key);
    return ;
  }

  //TODO: 未在frame_full时做内存安全性检查! 可能fail测试点!
  //如果在读入时内存已满, 我们需要手动分配内存
  //而不是等到ide_read()触发Page Fault后再处理
  if (frame_full())
  {
    size_t pages = DIV_ROUND_UP(size, PGSIZE);
    void *addr = buffer;
    while (pages > 0)
    {
      struct page_node *pnode = page_seek(cur, addr);
      if (pnode == NULL)
        page_get_new_page(cur, addr, FRM_NO_EVICT, SEG_STACK);
      else if (pnode->loc != LOC_MEMORY)
        page_pull_page(cur, pnode);

      addr += PGSIZE;
      pages--;
    }
  }

  struct file *file = process_from_fd_get_file(thread_current(), fd);
  if (file == NULL)
  {
    retval(f, ERROR);
    return ;
  }
  size_t bytes = file_read(file, buffer, size);
  retval(f, bytes);
}

static void
syscall_exec(struct intr_frame *f)
{
  const char *file = (const char *)(*get_args(f));

  int pid;
  
  pid = process_execute(file);
  
  sema_down(&thread_current()->exec_sema);

  if (pid == TID_ERROR || load_failed)
  {
    lock_acquire(&load_failure_lock);
    load_failed = false;
    lock_release(&load_failure_lock);

    retval(f, ERROR);
  }
  else
    retval(f, pid);
}

static void
syscall_wait(struct intr_frame *f)
{
  int pid = *(get_args(f));
  int child_status;

  child_status = process_wait(pid);

  retval(f, child_status);
}

//第二个参数用于手动控制退出的参数
void
syscall_exit(struct intr_frame *f, int status_)
{
  int status;

  if (status_ == NOT_SPECIFIED)
    status = *((int32_t *)get_args(f));
  else if (status_ == FORCE_EXIT) {
    status = -1;
  }
  else
    status = status_;

  struct semaphore *sema = NULL;
  struct thread *cur = thread_current();

  if (cur->pwait_node != NULL)
  {
    cur->pwait_node->status = status;
    //将sema指针暂时存起来, 当thread_exit后, cur指针将不再可用
    sema = &cur->pwait_node->sema;
  }

  // TODO: 释放进程持有的所有锁！
  //释放进程持有的资源, 包括pagelist和mmap以及fd_list
  cache_writeback_all();
  page_mmap_unmap_all(cur);
  page_destroy_pagelist(cur);
  process_destroy_fd_list(cur);
  
  // 恢复当前进程可执行文件的可修改性
  if (cur->exec_file != NULL)
    file_close(cur->exec_file);

  //刷新filesys的缓存
  // 打印Process Termination Messages
  printf("%s: exit(%d)\n", cur->name, status);

  if (sema != NULL)
    sema_up(sema);
  retval(f, status);

  //IMPORTANT: 线程所有要做的事情都要在thread_exit()前做完!
  thread_exit();
}

static void
syscall_write(struct intr_frame *f)
{
  // 检验内存合法性
  uint32_t *args_ptr = get_args(f);  
  struct syscall_frame_3args *args = (struct syscall_frame_3args *)(args_ptr);

  uint32_t fd = args->arg0;
  void *buffer = (void *)args->arg1;
  size_t size = args->arg2;

  // 向stdin中写入是不可行的!
  if (fd == 0)
    return ;

  // 仅对printf做初步的支持
  if (fd == 1)
  {
    putbuf((const char *)buffer, size);
    return ;
  }

  struct file *file = process_from_fd_get_file(thread_current(), fd);
  if (file == NULL)
    return ;

  size_t bytes = file_write(file, buffer, size);

  retval(f, bytes);
}

void
syscall_mmap(struct intr_frame *f)
{
  uint32_t *args_ptr = get_args(f);
  struct syscall_frame_2args *args = (struct syscall_frame_2args *)(args_ptr);

  int fd = args->arg0;
  
  if (fd == 0 || fd == 1)
  {
    retval(f, ERROR);
    return ;
  }
  struct thread *cur = thread_current();

  struct file *file = process_from_fd_get_file(cur, fd);
  void *uaddr = (void *)args->arg1;
  if (file == NULL)
  {
    retval(f, ERROR);
    return ;
  }
  
  mapid_t mapid = page_mmap_map(thread_current(), fd, file, uaddr);
  if (mapid != -1)
   process_fd_set_mapped(cur, fd, mapid);

  retval(f, mapid); 
}

void
syscall_munmap(struct intr_frame *f)
{
  uint32_t mapid = *(get_args(f));
  struct thread *cur = thread_current();
  // 以下几行在清除fd_node中的mapid 
  // 这样在用户close文件的时候就不会reopen file了
  struct mmap_vma_node *mnode = page_mmap_seek(cur, mapid, USE_MAPID);  
  struct fd_node *fnode = process_get_fd_node(cur,mnode->fd);
  // 若fnode == NULL 说明用户在unmap前就手动close了文件 无需操作
  if (fnode != NULL)
    fnode->mapid = UNMAPPED;

  page_mmap_unmap(thread_current(), mapid);
}

void
syscall_init (void) 
{
  lock_init(&load_failure_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  unsigned int syscall_no = *(uint32_t *)(f->esp);

  switch(syscall_no)
  {
    case SYS_FILESIZE:
      syscall_filesize(f);
      break;
    case SYS_READ:
      syscall_read(f);
      break;
    case SYS_WRITE:
      syscall_write(f);
      break;
    case SYS_SEEK:
      syscall_seek(f);
      break;
    case SYS_TELL:
      syscall_tell(f);
      break;
    case SYS_HALT:
      shutdown_power_off();
      break;
    case SYS_EXIT:
      syscall_exit(f, NOT_SPECIFIED); 
      break;
    case SYS_EXEC:
      syscall_exec(f);
      break;
    case SYS_WAIT:
      syscall_wait(f);
      break;
    case SYS_CREATE:
      syscall_create(f);
      break;
    case SYS_REMOVE:
      syscall_remove(f);
      break;
    case SYS_OPEN:
      syscall_open(f);
      break;
    case SYS_CLOSE:
      syscall_close(f);
      break;
    case SYS_MMAP:
      syscall_mmap(f);
      break;
    case SYS_MUNMAP:
      syscall_munmap(f);
      break;
    default:
      printf("Unknown syscall number! Killing process...\n");
      syscall_exit(f, FORCE_EXIT);
  }
}
