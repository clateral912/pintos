#include "page.h"
#include "frame.h"
#include "hash.h"
#include "../threads/thread.h"
#include "../threads/malloc.h"
#include "../threads/synch.h"
#include "../userprog/pagedir.h"
#include <stdint.h>
#include <stdio.h>

struct hash process_list;
struct lock process_list_lock;

unsigned page_process_hash_hash(const struct hash_elem *elem, void *aux UNUSED)
{
  struct process_node *node = hash_entry(elem, struct process_node, helem);
  return hash_bytes(&node->pid, sizeof(pid_t));
}

bool page_process_hash_less(const struct hash_elem *e1, const struct hash_elem *e2, void *aux UNUSED)
{
  struct process_node *node1 = hash_entry(e1, struct process_node, helem);
  struct process_node *node2 = hash_entry(e2, struct process_node, helem);

  return (node1->pid < node2->pid ? true : false);
}

unsigned page_hash_hash(const struct hash_elem *elem, void *aux UNUSED)
{
  struct page_node *node = hash_entry(elem, struct page_node, helem);
  return hash_bytes(&node->pte, sizeof(uint32_t));
}

bool page_hash_less(const struct hash_elem *e1, const struct hash_elem *e2, void *aux UNUSED)
{
  struct page_node *node1 = hash_entry(e1, struct page_node, helem);
  struct page_node *node2 = hash_entry(e2, struct page_node, helem);

  return (node1->pte < node2->pte ? true : false);
}

void page_init()
{
  hash_init(&process_list, page_process_hash_hash, page_process_hash_less, NULL); 
  lock_init(&process_list_lock);
}

void page_process_init(struct thread *t)
{
  struct process_node *process_node;
  process_node = malloc(sizeof(struct process_node));

  if (process_node == NULL)
    PANIC("Cannot allocate memory to store a hash map for process!\n");

  process_node->pid = t->tid;

  hash_init(&process_node->page_list, page_hash_hash, page_hash_less , NULL); 

  lock_acquire(&process_list_lock);
  hash_insert(&process_list, &process_node->helem);
  lock_release(&process_list_lock);
}

bool page_add_page(struct thread *t, uint32_t pte, bool sharing, enum location loc)
{
  //保证添加的pte一定对应一个已经存在的页
  void *upage = pte_get_page(pte);
  ASSERT(lookup_page(t->pagedir, upage, false) != NULL);

  bool success;
  struct process_node key_process_node;
  struct process_node *process_node;
  struct page_node *node;
  node = malloc(sizeof(struct page_node));
  if (node == NULL)
    PANIC("Cannot allocate memory to store a page in SPT!\n");

  node->owner = t->tid;
  node->pte = pte;
  node->sharing = sharing;
  node->loc = loc;

  key_process_node.pid = t->tid;

  struct hash_elem *helem = hash_find(&process_list, &key_process_node.helem);
  if (helem == NULL)
  {
    printf("Cannot find thread:%s in process list\n", t->name);
    return false;
  }

  process_node = hash_entry(helem, struct process_node, helem); 

  success = (hash_insert(&process_node->page_list, &node->helem) == NULL) ? true : false;

  return success;
}


