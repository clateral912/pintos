#include "process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gdt.h"
#include "list.h"
#include "pagedir.h"
#include "tss.h"
#include "../filesys/file.h"
#include "../filesys/filesys.h"
#include "../threads/flags.h"
#include "../threads/interrupt.h"
#include "../threads/palloc.h"
#include "../threads/malloc.h"
#include "../threads/thread.h"
#include "../threads/vaddr.h"
#include "../threads/synch.h"
#include "../vm/virtual-memory.h"
#include "../vm/frame.h"
#include "../vm/page.h"

static void* process_push_arguments(uint8_t *esp, char *args);
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp, char *args);

bool load_failed;
struct lock load_failure_lock;
// 为当前线程初始化堆栈
// 根据args, 在栈上压入argc与agrv
// 专门为main函数压栈
// main(int argc, char *argv[])
void*
process_push_arguments(uint8_t *esp, char* args)
{
  char *token, *save_ptr;
  char *argv[MAX_CMDLINE_TOKENS];         // 存储指向arg中各个token的指针 
  int argc = 0;
  
  // 初始化argv
  for (int i = 0; i < MAX_CMDLINE_TOKENS; i++)
  {
    argv[i] = NULL;
  }

  // 获取args中的各个token, 并将其直接按从左到右的顺序压入栈中
  // 此处的顺序并不重要, 我们只是将参数搬运到用户内存中
  for (token = strtok_r(args, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr))
  {
    esp -= strlen(token) + 1; //别忘记为"\0"分配空间!
    strlcpy((char *)esp, (const char *)token, strlen(token) + 1);
    // *esp = *token;
    argv[argc] = (char *)esp;
    argc++;
  }

  // 将栈指针与4对其
  while((uintptr_t)esp % 4 != 0)
    esp = (void *)esp - 1;
  
  //压入NULL, 标识argv[argc] = NULL
  esp -= sizeof(uintptr_t);
  memset((void *)esp, 0, sizeof(char *));

  // 压入argv的各个元素, 它们都是指向token字符串的指针
  // 按照从右到左的顺序压栈
  for (int j = MAX_CMDLINE_TOKENS - 1; j >= 0; j--)
  {
    if (argv[j] == NULL)
      continue;

    esp -= sizeof(char *);
    *(uintptr_t *)esp = (uintptr_t)argv[j];
  }

  // 按照从右到左的顺序压入argv与argc
  // argv[0]就在argv上面4个byte的位置
  esp -= sizeof(char *);
  *(uintptr_t *)esp = (uintptr_t)(esp + 4);

  esp -= sizeof(argc);
  *(uintptr_t *)esp = argc;

  // 压入形式意义上的返回地址: NULL 
  // 符合调用约定
  esp -= sizeof(uintptr_t);
  memset((void *)esp, 0, sizeof(char *));

  // 打印esp指向的栈空间, 范围从esp 到 PHYS_BASE
  // for debug only 
  //hex_dump((uintptr_t) esp, (const void *)esp, (uintptr_t)(PHYS_BASE - (void *)esp), true);

  return (void *)esp;
}

inline static uint32_t
process_allocate_fd(struct thread *t)
{
  return (++t->current_fd);
}

void
process_destroy_fd_list(struct thread *t)
{
  struct list_elem *e;
  struct fd_node *node;

  for (e = list_begin(&t->fd_list); e != list_end(&t->fd_list); )
  {
    node = list_entry(e, struct fd_node, elem);
    file_close(node->file);
    e = list_next(e);
    free(node);
  }
}

uint32_t
process_create_fd_node(struct thread *t, struct file *file)
{
  struct fd_node *node;
  node = malloc(sizeof(struct fd_node));
  if (node == NULL)
    PANIC("fd node memory allocation failed!\n");

  node->fd = process_allocate_fd(t);
  node->file = file;
  node->mapid = UNMAPPED;

  list_push_back(&t->fd_list, &node->elem);
  return node->fd;
}

bool
process_remove_fd_node(struct thread *t, uint32_t fd)
{
  struct list_elem *e;
  struct fd_node *node;
  for (e = list_begin(&t->fd_list); e != list_end(&t->fd_list); )
  {
    node = list_entry(e, struct fd_node, elem);
    if (node->fd == fd)
    {
      node->file = NULL;
      list_remove(e);
      free(node);
      return true;
    }
  }
  return false;
}

struct fd_node *
process_get_fd_node(struct thread *t, uint32_t fd)
{
  ASSERT(fd != 0 && fd != 1);
  
  struct list_elem *e;
  struct fd_node *node;
  for (e = list_begin(&t->fd_list); e != list_end(&t->fd_list); e = list_next(e))
  {
    node = list_entry(e, struct fd_node, elem);
    if (node->fd == fd)
      return node;
  }
  return NULL;
}

struct file *
process_from_fd_get_file(struct thread *t, uint32_t fd)
{
  struct fd_node *node = process_get_fd_node(t, fd);
  if (node == NULL)
    return NULL;

  return node->file;
}

void
process_fd_set_mapped(struct thread *t, uint32_t fd, mapid_t mapid)
{
  ASSERT(mapid != -1);

  struct fd_node *node = process_get_fd_node(t, fd);
  if (node != NULL)
  {
    node->mapid = mapid;
    return ;
  }

  PANIC("fd_set_mapped(): Cannot find fd: %d\n", fd);
}


/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  // 为进程名称分配一页内存, 避免两个线程的竞争(?)
  // 分配内存后, 我们可以安全地修改字符串了, 不能直接对参数中的file_name做修改!
  // 因为file_name是const修饰的
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);
  // 指定缓冲区的大小为PGSIZE(一页内存的大小), 确保名字不会超过这个大小

  /* Create a new thread to execute FILE_NAME. */
  // PintOS 只能支持单线程的进程
  // fn_copy将会是start_process()函数的参数
  tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy); 
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  char args[MAX_CMDLINE_LENGTH];    // 绝对不可以写成char *args = file_name; !!!
                                    // 128是经过尝试的magic number
  char *save_ptr;
  struct intr_frame if_;
  bool success;


  lock_acquire(&load_failure_lock);
  load_failed = false;
  lock_release(&load_failure_lock);

  page_process_init(thread_current());

  strlcpy(args, file_name, MAX_CMDLINE_LENGTH);
  file_name = strtok_r(file_name, " ", &save_ptr);
  // 将当前线程的名字设定为文件名本身
  strlcpy(thread_current()->name, (const char *)file_name, 16);
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp, args);

  /* If load failed, quit. */
  // load()执行完毕, 则把之前分配的内存释放
  palloc_free_page (file_name);

  lock_acquire(&load_failure_lock);
  load_failed = !success;
  lock_release(&load_failure_lock);

  sema_up(&thread_current()->pwait_node->parent->exec_sema);
  if (load_failed)
    thread_exit();
  
  // 保证当前正在执行的文件不会被其他进程修改 
  struct file* file = filesys_open(thread_current()->name);
  file_deny_write(file);
  thread_current()->exec_file = file;

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  __asm__ volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

    This function will be implemented in problem 2-2.  For now, it
   does nothing. */
// 注意! 每个进程的TID都是不同的! 不会用TID复用的情况!
//
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  struct pwait_node_ *node;
  struct semaphore *pwait_sema = NULL;

  for (e = list_begin(&cur->pwait_list); e != list_end(&cur->pwait_list); e = list_next(e))
  {
    node = list_entry(e, struct pwait_node_, elem);
    
    //注意! 这里不能写node->child->tid, 因为此时我们在用户进程内存空间中
    //虽然node->child的地址没变, 但这个虚拟地址在不同页表和页目录下指向了完全不同的地方!
    //因此我们只能额外用一个变量child_pid来存储pid 
    //但是在内核上下文中, 我们可用使用node->child来访问线程句柄
    //比如thread_destroy_pwait_list();
    if (node->child_pid == child_tid)
    {
      pwait_sema = &node->sema;
      break;
    }
  }
  
  // 若未找到pid子进程或进程已经退出, 那么返回-1
  if (pwait_sema == NULL || node->waited)
    return -1;

  //执行等待
  sema_down(pwait_sema);
  node->waited = true;

  return node->status;
}

/* Free the current process's resources. */
// IMPORTANT: 此处未实现线程的销毁! 需要额外调用一次thread_exit()!!!
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  page_mmap_unmap_all(cur);
  page_destroy_pagelist(cur);
  process_destroy_fd_list(cur);
  // 恢复当前进程可执行文件的可修改性
  if (cur->exec_file != NULL)
  {
    file_close(cur->exec_file);
  }
    /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      // 这几行代码的执行顺序很重要! 
      cur->pagedir = NULL;
      // 给pagedir_activate()传入NULL会直接激活init_page_dir, 切换为内核虚拟内存页目录
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
// 每次操作系统执行上下文切换都要执行这个代码!
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp, char *args) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  //保证加载成功才将当前进程的可执行文件设置成该文件
  //t->exec_file = file;
  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  *esp = (void *)process_push_arguments(*esp, args);
  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  struct thread *cur = thread_current();
  file_seek (file, ofs);

  if ((void *)upage < cur->vma.code_seg_begin || cur->vma.code_seg_begin == NULL)
    cur->vma.code_seg_begin   = upage;

  cur->vma.code_seg_end = upage;
  cur->vma.loading_exe      = true;
  cur->page_default_flags   = FRM_ZERO | FRM_NO_EVICT | FRM_RW;
  //cur->page_default_flags  |= writable ? FRM_RW : FRM_RO;
  //如果此处设置了read_only , 会造成后续无法写入文件!
  //TODO: 实现read_only的功能

  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      /* Load this page. */
      if (file_read (file, upage, page_read_bytes) != (int) page_read_bytes)
        {
          page_free_page(cur, upage);
          return false; 
        }
      memset (upage + page_read_bytes, 0, page_zero_bytes);

      // 读取后, 将需要只读的页面设置为只读
      if (!writable)
      {
        uint32_t *pte = lookup_page(cur->pagedir, upage, false);
        ASSERT(pte != NULL);
        *pte &= ~(uint32_t)PTE_W;
      }
           /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }

  cur->vma.loading_exe    = false;
  cur->page_default_flags = 0;
  //由于我们更改了某些页面的读写权限, 需要刷新TLB
  invalidate_pagedir(cur->pagedir);

  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  struct thread *cur = thread_current();
  bool success = false;
  success = page_get_new_page(cur, ((uint8_t *) PHYS_BASE) - PGSIZE, FRM_ZERO | FRM_RW, SEG_STACK);
  if (success)
  {
    cur->vma.stack_seg_end = PHYS_BASE;
    cur->vma.stack_seg_begin = ((uint8_t *)PHYS_BASE) - PGSIZE;
    *esp = PHYS_BASE;
  }
  else
    page_free_page(thread_current(), ((uint8_t *) PHYS_BASE) - PGSIZE);

  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
