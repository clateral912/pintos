#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "virtual-memory.h"
#include "../threads/thread.h"
#include <stdint.h>

void page_init(void);
void page_process_init(struct thread *);
struct page_node *page_add_page(struct thread *t, const void *uaddr, uint32_t flags, enum location loc, enum role role);
struct page_node *page_seek(struct thread *t, const void *uaddr);
void page_destroy_pagelist(struct thread *);
void page_assign_frame(struct page_node *, struct frame_node *);
bool page_get_page(struct thread *t, const void *uaddr, uint32_t flags, enum role role);
void page_free_page(struct thread *t, const void *uaddr);
enum role page_check_role(struct thread *t, const void *uaddr);
mapid_t page_mmap_map(struct thread *t, uint32_t fd, struct file *file, void *addr);
void page_mmap_unmap(struct thread *t, mapid_t mapid);
struct mmap_vma_node *page_mmap_seek(struct thread *t, mapid_t mapid, const void *addr);
void page_mmap_unmap_all(struct thread *t);

#endif // !VM_PAGE_H
