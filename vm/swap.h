#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>

void swap_init();
size_t swap_in(const void *upage);
void swap_out(size_t page_idx, void *kpage);

#endif // !VM_SWAP_H
