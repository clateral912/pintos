#include "pagedir.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "../threads/init.h"
#include "../threads/pte.h"
#include "../threads/palloc.h"

static uint32_t *active_pd (void);
static void invalidate_pagedir (uint32_t *);

// IMPORTANT: 每个进程的页目录和页表都存储在内核虚拟内存中!
// 因此, 分配PTE, PDE, 创建或销毁Page Directory, 都是在内核虚拟地址空间

// IMPORTANT: 每个PDE, PTE中的地址都是实际物理内存的地址! 绝对不是虚拟地址!

/* Creates a new page directory that has mappings for kernel
   virtual addresses, but none for user virtual addresses.
   Returns the new page directory, or a null pointer if memory
   allocation fails. */
// 创建一个页目录
// 先给新的页目录分配一页内存, 再将kernel虚拟内存的页目录复制到这个新的页目录中
// 此时, 该页目录不包含用户虚拟内存的映射关系
uint32_t *
pagedir_create (void) 
{
  uint32_t *pd = palloc_get_page (0);
  if (pd != NULL)
    memcpy (pd, init_page_dir, PGSIZE);
  return pd;
}

/* Destroys page directory PD, freeing all the pages it
   references. */
void
pagedir_destroy (uint32_t *pd) 
{
  uint32_t *pde;

  if (pd == NULL)
    return;

  // init_page_dir是内核的页表, 绝对不可能删除
  ASSERT (pd != init_page_dir);
  // 遍历每一条PDE, 若其present则继续删除
  for (pde = pd; pde < pd + pd_no (PHYS_BASE); pde++)
    if (*pde & PTE_P) 
      {
        uint32_t *pt = pde_get_pt (*pde);
        uint32_t *pte;
        
        // 遍历每一条pte, 若存在则释放对应的内存页面
        for (pte = pt; pte < pt + PGSIZE / sizeof *pte; pte++)
          if (*pte & PTE_P) 
            palloc_free_page (pte_get_page (*pte));
        palloc_free_page (pt);
      }
  // 最后释放掉存储页目录和页表的内存页面, 彻底将一个页目录从内存中抹去
  palloc_free_page (pd);
}

/* Returns the address of the page table entry for virtual
   address VADDR in page directory PD.
   If PD does not have a page table for VADDR, behavior depends
   on CREATE.  If CREATE is true, then a new page table is
   created and a pointer into it is returned.  Otherwise, a null
   pointer is returned. */
// 返回vaddr所在page在Page Table中的PTE的地址, 
// 或者说, 返回指向一个PTE的地址, 这个PTE指向的page中包含vaddr 
// 如果包含vaddr的Page Table不存在(尚未分配), 那么看create的值决定是否创建新的Page Table
// (是否创建新的Page Table)
uint32_t *
lookup_page (uint32_t *pd, const void *vaddr, bool create)
{
  // 函数的整体思路:
  // 先通过传入的参数pd找到进程的页目录, 随后利用虚拟地址的高10位定位页表的位置(即找到PDE)
  // 通过提取PDE的高20位得到页表的地址, 随后访问页表, 以vaddr的中间10位作为索引, 找到对应的pte
  // 通过&pt[pt_no(vaddr)]返回一个指向PTE的指针
  uint32_t *pt, *pde;

  ASSERT (pd != NULL);

  /* Shouldn't create new kernel virtual mappings. */
  // 不应该在内核虚拟内存中创建页面!
  // create = false: 无论如何assert都通过
  // create = true : 当vaddr指向kernel空间时, 不通过assert
  ASSERT (!create || is_user_vaddr (vaddr));

  /* Check for a page table for VADDR.
     If one is missing, create one if requested. */
  // 获取指向含有vaddr的Page Table的地址(这个地址就是pde的一部分)
  pde = pd + pd_no (vaddr);
  if (*pde == 0) 
    {
      if (create)
        {
          // 创建新的页表, 返回指向空白内存页面的指针, 若创建失败则返回NULL
          // flags中不含PAL_USER, 将从kernel内存池中分配内存
          // IMPORTANT: 此时pt尚未被初始化! pt对应的page里面全是0! 并没有指向任何内存地址!
          pt = palloc_get_page (PAL_ZERO);
          if (pt == NULL) 
            return NULL; 
      
          // 根据刚刚创建的页表, 创建新的PDE
          *pde = pde_create (pt);
        }
      else
        return NULL;
    }

  /* Return the page table entry. */
  pt = pde_get_pt (*pde);
  // 返回指向某个PTE的内存地址
  return &pt[pt_no (vaddr)];
}

/* Adds a mapping in page directory PD from user virtual page
   UPAGE to the physical frame identified by kernel virtual
   address KPAGE.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   If WRITABLE is true, the new page is read/write;
   otherwise it is read-only.
   Returns true if successful, false if memory allocation
   failed. */
// 在进程虚拟内存与内核虚拟内存之间建立映射关系
// 函数的目的: 为进程分配物理内存帧
// 它的核心功能是将用户虚拟地址空间中的页映射到物理内存中的帧，
// 从而支持进程的隔离、内存保护和动态内存分配
// 将某个内核虚拟内存页面kpage, 映射到进程持有的某一页内存upage
// 这样进程就可以通过访问upage的方式来访问kpage
// 进程不可以自己给自己分配或释放内存 
bool
pagedir_set_page (uint32_t *pd, void *upage, void *kpage, bool writable)
{
  uint32_t *pte;

  // 保证kpage, upage是页对其的(低12位(offset)为0)
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (pg_ofs (kpage) == 0);
  ASSERT (is_user_vaddr (upage));
  // 保证内核虚拟内存页面kpage的编号不超过系统最大内存
  ASSERT (vtop (kpage) >> PTSHIFT < init_ram_pages);
  ASSERT (pd != init_page_dir);

  // 先在获取一个pte, 指向upage
  // 注意! 此处pte是一个指针/地址! 指向upage所属的Page Table中的某一条pte
  pte = lookup_page (pd, upage, true);


  if (pte != NULL) 
    {
      // 保证upage尚未present (PTE_P位为0, 尚未出现在内存中), 
      // 因为若PTE_P == 1说明upage很有可能已经被使用了, 
      // 我们需要的是空白的内存页面来进行映射
      ASSERT ((*pte & PTE_P) == 0);
      // 创建一个指向kpage的pte(是货真价实的pte而不是指向pte的指针), 
      // 将其赋值给进程pd中的对应pte
      *pte = pte_create_user (kpage, writable);
      return true;
    }
  else
    return false;
}

/* Looks up the physical address that corresponds to user virtual
   address UADDR in PD.  Returns the kernel virtual address
   corresponding to that physical address, or a null pointer if
   UADDR is unmapped. */
// 返回uaddr所在页的内核虚拟地址
void *
pagedir_get_page (uint32_t *pd, const void *uaddr) 
{
  uint32_t *pte;

  ASSERT (is_user_vaddr (uaddr));
  
  // 在进程页目录中找到指向包含uaddr的PTE的指针
  pte = lookup_page (pd, uaddr, false);
  if (pte != NULL && (*pte & PTE_P) != 0)
    // 返回一个内核虚拟地址, *pte的高20位是实际物理地址!
    // 这意味着内核利用这个函数可用访问用户页面uaddr指向的内容了!
    return pte_get_page (*pte) + pg_ofs (uaddr);
    // 必须注意一点: 无论虚拟内存地址如何, 
    // 虚拟地址的低12位offset一定是与实际物理地址的低12位完全一样!
    // 因此pg_ofs(uaddr)返回的就是实际物理地址的低12位
    // 你也可以理解为是内核虚拟地址的低12位(反正其低12位一定和物理地址的低12位相同)
  else
    return NULL;
}

/* Marks user virtual page UPAGE "not present" in page
   directory PD.  Later accesses to the page will fault.  Other
   bits in the page table entry are preserved.
   UPAGE need not be mapped. */
// 在某个Page Table中找到指向upage 的PTE, 并将PTE的PTE_P位清零
// 此时upage将不可访问, upage可用于后续的内存分配
void
pagedir_clear_page (uint32_t *pd, void *upage) 
{
  uint32_t *pte;

  ASSERT (pg_ofs (upage) == 0);
  ASSERT (is_user_vaddr (upage));

  pte = lookup_page (pd, upage, false);
  // 保证PTE是一个有效的PTE
  if (pte != NULL && (*pte & PTE_P) != 0)
    {
      // 清空PTE的PTE_P位(not present)
      *pte &= ~PTE_P;
      // 刷新CPU的TLB, TLB中存储的是旧的PTE,
      // 若不清除旧的PTE, 则下次CPU通过MMU访问同一个page时
      // TLB会直接命中, 给CPU提供错误的物理地址, 
      // 而不会查询我们修改过的PTE
      invalidate_pagedir (pd);
    }
}

/* Returns true if the PTE for virtual page VPAGE in PD is dirty,
   that is, if the page has been modified since the PTE was
   installed.
   Returns false if PD contains no PTE for VPAGE. */
bool
pagedir_is_dirty (uint32_t *pd, const void *vpage) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  return pte != NULL && (*pte & PTE_D) != 0;
}

/* Set the dirty bit to DIRTY in the PTE for virtual page VPAGE
   in PD. */
// 如果vpage被修改过, 则其PTE_D位会被置为1
void
pagedir_set_dirty (uint32_t *pd, const void *vpage, bool dirty) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  if (pte != NULL) 
    {
      if (dirty)
        // 将pte的PTE_D位设为1, 其余位保持不变
        *pte |= PTE_D;
      else 
        {
          // 清空pte的PTE_D位
          *pte &= ~(uint32_t) PTE_D;
          // 刷新TLB
          invalidate_pagedir (pd);
        }
    }
}

/* Returns true if the PTE for virtual page VPAGE in PD has been
   accessed recently, that is, between the time the PTE was
   installed and the last time it was cleared.  Returns false if
   PD contains no PTE for VPAGE. */
bool
pagedir_is_accessed (uint32_t *pd, const void *vpage) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  return pte != NULL && (*pte & PTE_A) != 0;
}

/* Sets the accessed bit to ACCESSED in the PTE for virtual page
   VPAGE in PD. */
void
pagedir_set_accessed (uint32_t *pd, const void *vpage, bool accessed) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  if (pte != NULL) 
    {
      if (accessed)
        *pte |= PTE_A;
      else 
        {
          *pte &= ~(uint32_t) PTE_A; 
          invalidate_pagedir (pd);
        }
    }
}

/* Loads page directory PD into the CPU's page directory base
   register. */
void
pagedir_activate (uint32_t *pd) 
{
  if (pd == NULL)
    pd = init_page_dir;

  /* Store the physical address of the page directory into CR3
     aka PDBR (page directory base register).  This activates our
     new page tables immediately.  See [IA32-v2a] "MOV--Move
     to/from Control Registers" and [IA32-v3a] 3.7.5 "Base
     Address of the Page Directory". */
  __asm__ volatile ("movl %0, %%cr3" : : "r" (vtop (pd)) : "memory");
}

/* Returns the currently active page directory. */
static uint32_t *
active_pd (void) 
{
  /* Copy CR3, the page directory base register (PDBR), into
     `pd'.
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 3.7.5 "Base Address of the Page Directory". */
  uintptr_t pd;
  __asm__ volatile ("movl %%cr3, %0" : "=r" (pd));
  return ptov (pd);
}

/* Seom page table changes can cause the CPU's translation
   lookaside buffer (TLB) to become out-of-sync with the page
   table.  When this happens, we have to "invalidate" the TLB by
   re-activating it.

   This function invalidates the TLB if PD is the active page
   directory.  (If PD is not active then its entries are not in
   the TLB, so there is no need to invalidate anything.) */
static void
invalidate_pagedir (uint32_t *pd) 
{
  if (active_pd () == pd) 
    {
      /* Re-activating PD clears the TLB.  See [IA32-v3a] 3.12
         "Translation Lookaside Buffers (TLBs)". */
      pagedir_activate (pd);
    } 
}
