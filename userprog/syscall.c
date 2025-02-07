#include "syscall.h"
#include <string.h>
#include "../devices/shutdown.h"
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "../threads/interrupt.h"
#include "../threads/thread.h"
#include "process.h"
#include "stdio.h"

static void syscall_handler (struct intr_frame *);
static void retval(struct intr_frame *f, int32_t);

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



static inline
void
retval(struct intr_frame *f, int32_t num)
{
  f->eax = num;  
}

static inline 
struct intr_frame *
syscall_get_intr_frame(void *esp)
{
  struct intr_frame *f = (struct intr_frame *)((char *)&(esp) - offsetof(struct intr_frame, esp));
  return f;
}

static void
syscall_exec(struct intr_frame *f)
{
  const char *file = (char *)(*((uint32_t *)(f->esp) + 1));
  int pid;
  
  pid = process_execute(file);
  
  sema_down(&thread_current()->exec_sema);

  // load fail也应该返回-1 此处未处理这种情况!
  if (pid == TID_ERROR)
    retval(f, -1);
  else
    retval(f, pid);

}

static void
syscall_wait(struct intr_frame *f)
{
  int pid = *((uint32_t *)(f->esp) + 1);
  int child_status;

  child_status = process_wait(pid);

  retval(f, child_status);
}

static void
syscall_exit(struct intr_frame *f)
{
  int status = *((int32_t *)(f->esp) + 1); 
  struct semaphore *sema = NULL;

  struct thread *cur = thread_current();
  if (cur->pwait_node != NULL)
  {
    cur->pwait_node->status = status;
    cur->pwait_node->exited = true;
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
  struct syscall_frame_3args *args = (struct syscall_frame_3args *)((uint32_t *)(f->esp) + 1);

  uint32_t fd = args->arg0;
  void *buffer = (void *)args->arg1;
  size_t size = args->arg2;

  // 仅对printf做初步的支持
  if (fd == 1)
    putbuf((const char *)buffer, size);

  // !!!并没有实现返回写入多少字符的功能!!!
  retval(f, size);
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  // TODO: 验证进程提供的指针的合法性, 比如进程提供的指针可能指向内核地址空间
  // 造成安全问题
  unsigned int syscall_no = *(uint32_t*)(f->esp);
  switch(syscall_no)
  {
    case SYS_WRITE:
      syscall_write(f);
      break;
    case SYS_HALT:
      shutdown_power_off();
      break;
    case SYS_EXIT:
      syscall_exit(f); 
      break;
    case SYS_EXEC:
      syscall_exec(f);
      break;
  }
}
