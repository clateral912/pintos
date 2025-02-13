#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <stdint.h>
#include "frame.h"


struct process_node
{
  pid_t pid;
  struct hash page_list;
  struct hash_elem helem;
};


#endif // !VM_PAGE_H
