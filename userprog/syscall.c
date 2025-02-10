#include "syscall.h"
#include <stddef.h>
#include <string.h>
#include "../devices/shutdown.h"
#include "../devices/input.h"
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "../threads/interrupt.h"
#include "../threads/thread.h"
#include "../threads/vaddr.h"
#include "../filesys/filesys.h"
#include "process.h"
#include "stdbool.h"
#include "stdio.h"

#define NO_LIMIT 32768    //32768是随便想出来的一个maigic number

static int get_user(const uint8_t *);
static bool put_user(uint8_t *, uint8_t);
static bool byte_valid(void *);
static bool mem_valid(void *, size_t);
static bool str_valid(const char *, uint32_t);
static const char * check_charptr_validity(struct intr_frame *);
static void retval(struct intr_frame *, int32_t);
static void syscall_handler (struct intr_frame *);
static void syscall_exit(struct intr_frame *, int32_t);
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

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  __asm__ ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  __asm__ ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

// 检查一个字符串是否有效(全部都在可访问的内存空间中)
static bool
str_valid(const char *string, uint32_t len_limit)
{
  const char *p;
  char c;
  uint32_t length = 0;
  
  if (string == NULL)
    return false;

  if (!is_user_vaddr(string))
    return false;
  
  for (p = string; ; p++) 
  {
    c = get_user((const uint8_t *)p);
    if (c == -1)
        return false;
    if (c == '\0')
        break;

    if(++length > len_limit)
      return false;
  }
  return true;
}

// 检查一个指针指向的一字节长度是否可访问
static bool
byte_valid(void *ptr)
{
  if (ptr == NULL)
    return false;
  
  if (!is_user_vaddr(ptr))
    return false;

  return !(get_user((const uint8_t *) ptr) == -1);
}

//检查一块内存区域是否可访问
static bool
mem_valid(void *ptr_, size_t size)
{
  if (ptr_ == NULL)
    return false;

  if (!is_user_vaddr(ptr_))
    return false;

  char *ptr = (char *)ptr_;
  for (; size > 0; size--)
  {
    if(!byte_valid(ptr))
      return false;

    ptr++;
  }
  return true;
}

// 仅对第一个参数为const char *的系统调用有效
// 对其他类型的系统调用进行检查是未定义行为!
static const char *
check_charptr_validity(struct intr_frame *f)
{
  if (!mem_valid(((uint32_t *)(f->esp) + 1), sizeof(uint32_t)))
  {
    syscall_exit(f, FORCE_EXIT);
    return NULL;
  }
  
  const char *file = (char *)(*((uint32_t *)(f->esp) + 1));
  
  // 检查file指向的字符串的合法性
  if (!str_valid(file, 255))
  {
    syscall_exit(f, FORCE_EXIT);
    return NULL;
  }
  return file;
}

// 检查esp指针后size个byte的有效性
// 就是检查中断帧指向的栈中的参数的可访问性
static uint32_t *
check_args_validity(struct intr_frame *f, size_t size)
{
 if (!mem_valid(((uint32_t *)(f->esp) + 1), size))
    syscall_exit(f, FORCE_EXIT);

  return ((uint32_t *)(f->esp) + 1);

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
  uint32_t *args_ptr = check_args_validity(f, sizeof(struct syscall_frame_2args)); 

  struct syscall_frame_2args* args = (struct syscall_frame_2args *)args_ptr;
  
  const char *file = (char *)args->arg0;
  uint32_t initial_size = args->arg1;

  if(!str_valid(file, NO_LIMIT))
    syscall_exit(f, FORCE_EXIT);

  bool success = filesys_create(file, initial_size);
  
  retval(f, success);
}

static void
syscall_remove(struct intr_frame *f)
{
  const char *file = check_charptr_validity(f);
  bool success = filesys_remove(file);
  retval(f, success);
}

static void
syscall_seek(struct intr_frame *f)
{
  uint32_t *args_ptr = check_args_validity(f, sizeof(struct syscall_frame_2args)); 

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
  uint32_t fd = *(check_args_validity(f, sizeof(uint32_t)));

  struct file* file = process_from_fd_get_file(thread_current(), fd);
  if (file == NULL)
    return ;

  uint32_t pos = file_tell(file);
  retval(f, pos);
}

static void
syscall_filesize(struct intr_frame *f)
{
  uint32_t fd = *(check_args_validity(f, sizeof(uint32_t)));

  struct file *file = process_from_fd_get_file(thread_current(), fd);
  if (file == NULL)
  {
    retval(f, -1);
    return ;
  }
  size_t size = file_length(file);
  retval(f, size);
}

static void
syscall_open(struct intr_frame *f)
{
  uint32_t fd;
  const char *file_name = check_charptr_validity(f);
  struct file *file = filesys_open(file_name);
  // file==NULL的情况有内部内存分配错误, 以及未找到文件
  // 未找到文件的情况在此处处理
  // TODO: 逻辑漏洞! 如果是内部内存错误怎么办?
  // 从测试点的逻辑倒推, 打开文件失败应该正常退出才对
  // 而不是把进程杀死
  if (file == NULL)
  {
    retval(f, -1);
    return ;
  }
  fd = process_create_fd_node(thread_current(), file);
  retval(f, fd);
}

static void
syscall_close(struct intr_frame *f)
{
  int fd = *(check_args_validity(f, sizeof(int32_t)));
  struct file *file = process_from_fd_get_file(thread_current(), fd);
  
  if (file == NULL)
    return ;

  file_close(file); 
  process_remove_fd_node(thread_current(), fd);
}

static void
syscall_read(struct intr_frame *f)
{
  uint32_t *args_ptr = check_args_validity(f, sizeof(struct syscall_frame_3args));  
  struct syscall_frame_3args *args = (struct syscall_frame_3args *)(args_ptr);

  uint32_t fd = args->arg0;
  void *buffer = (void *)args->arg1;
  size_t size = args->arg2;

  if(!mem_valid(buffer, size))
    syscall_exit(f, FORCE_EXIT);
  
  if (fd == 0)
  {
    // 手册说的不清楚! 当fd为0时返回什么?
    uint8_t key = input_getc();
    retval(f, key);
    return ;
  }
  struct file *file = process_from_fd_get_file(thread_current(), fd);
  if (file == NULL)
  {
    retval(f, -1);
    return ;
  }
  size_t bytes = file_read(file, buffer, size);
  retval(f, bytes);
}

static void
syscall_exec(struct intr_frame *f)
{
  const char *file = check_charptr_validity(f);

  int pid;
  
  pid = process_execute(file);
  
  sema_down(&thread_current()->exec_sema);

  if (pid == TID_ERROR || load_failed)
  {
    load_failed = false;
    retval(f, -1);
  }
  else
    retval(f, pid);
}

static void
syscall_wait(struct intr_frame *f)
{
  int pid = *(check_args_validity(f, sizeof(int)));
  int child_status;

  child_status = process_wait(pid);

  retval(f, child_status);
}

//第二个参数用于手动控制退出的参数
static void
syscall_exit(struct intr_frame *f, int status_)
{
  int status;

  if (status_ == NOT_SPECIFIED)
    status = *((int32_t *)check_args_validity(f, sizeof(int32_t)));
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
  uint32_t *args_ptr = check_args_validity(f, sizeof(struct syscall_frame_3args));  
  struct syscall_frame_3args *args = (struct syscall_frame_3args *)(args_ptr);

  uint32_t fd = args->arg0;
  void *buffer = (void *)args->arg1;
  size_t size = args->arg2;

  if(!mem_valid(buffer, size))
    syscall_exit(f, FORCE_EXIT);

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
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  // 校验esp指针的合法性, 我们在下面将esp作为uint32_t类型来解引用
  // 故在此处要检验四个字节的合法性
  if (!mem_valid(f->esp, sizeof(uint32_t)))
    syscall_exit(f, FORCE_EXIT);

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
    default:
      printf("Unknown syscall number! Killing process...\n");
      syscall_exit(f, FORCE_EXIT);
  }
}
