/* helix_memory.h - Helix 解码器内存分配接口 (原版在 utils/, 用户未拷贝, 此处补齐) */
#ifndef _HELIX_MEMORY_H
#define _HELIX_MEMORY_H
#include <stddef.h>
/* 使用前必须先调用 helix_pool_init(pool, size) 指定内存池 */
void helix_pool_init(void *pool, size_t size);
void *helix_malloc(size_t size);
void helix_free(void *ptr);
#endif
