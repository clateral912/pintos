#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "stdbool.h"
#include "virtual-memory.h"
#include "../threads/thread.h"
#include <stdint.h>

extern uint32_t frame_cnt;
extern struct lock flist_lock;
// flags = 0, 说明生成的页面RW, 不zero, 可驱逐, 不sharing
#define FRM_RW 0
#define FRM_RO 1
#define FRM_ZERO 2
#define FRM_NO_EVICT 4
#define PG_SHARING 8

extern struct list frame_list;

void frame_init(void);
struct frame_node *frame_allocate_page(uint32_t *pd, uint32_t flags);
void frame_destroy_frame(struct frame_node *fnode);
struct frame_node *frame_evict(uint32_t flags);
bool frame_full(void);

#endif
