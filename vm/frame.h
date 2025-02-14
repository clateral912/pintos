#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "stdbool.h"
#include "virtual-memory.h"
#include <stdint.h>

#define FRM_RO 0
#define FRM_RW 1
#define FRM_ZERO 2
#define FRM_NO_EVICT 4

extern struct list frame_list;

void frame_init(void);
struct frame_node *frame_allocate_page(uint32_t *pd, const void *uaddr, uint32_t flags);
void frame_destroy_frame(struct frame_node *fnode);

#endif
