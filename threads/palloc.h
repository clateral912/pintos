#ifndef THREADS_PALLOC_H
#define THREADS_PALLOC_H

#include <stddef.h>

// 枚举类型的常量都是2的幂, 方便进行位运算来组合多个标志位
// enum palloc_flags flag = PAL_ASSERT | PAL_ZERO 这种赋值是合法的!
/* How to allocate pages. */
enum palloc_flags
  {
    PAL_ASSERT = 001,           /* Panic on failure. */
    PAL_ZERO = 002,             /* Zero page contents. */
    PAL_USER = 004              /* User page. */
  };

void palloc_init (size_t user_page_limit);
void *palloc_get_page (enum palloc_flags);
void *palloc_get_multiple (enum palloc_flags, size_t page_cnt);
void palloc_free_page (void *);
void palloc_free_multiple (void *, size_t page_cnt);

#endif /* threads/palloc.h */
