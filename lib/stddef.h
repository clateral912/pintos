#ifndef __LIB_STDDEF_H
#define __LIB_STDDEF_H

#define NULL ((void *) 0)
// 返回结构体中某个成员变量相对结构体开始处的偏移地址
// (size_t)表示将表达式的值强制转换位size_t类型
// (TYPE *) 0 表示将数字0强制转换为TYPE类型的结构体指针
// &(((TYPE *) 0) -> MEMBER) 表示取MEMBER的地址 
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *) 0)->MEMBER)

/* GCC predefines the types we need for ptrdiff_t and size_t,
   so that we don't have to guess. */
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __SIZE_TYPE__ size_t;

#endif /* lib/stddef.h */
