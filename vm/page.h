#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "virtual-memory.h"
#include "../threads/thread.h"
#include <stdint.h>

void page_init(void);
void page_process_init(struct thread *);
struct page_node *page_add_page(struct thread *t, const void *uaddr, uint32_t flags, enum location loc);
struct page_node *page_seek(struct thread *t, const void *uaddr);
void page_destroy_pagelist(struct thread *);
void page_assign_frame(struct page_node *, struct frame_node *);
bool page_get_page(struct thread *t, const void *uaddr, uint32_t flags);
void page_free_page(struct thread *t, const void *uaddr);

#endif // !VM_PAGE_H
