/* helix_memory.c - Helix 内存分配实现。
 * 用调用者提供的外部内存池(不放 malloc, 避免堆耗尽/碎片)。
 * Helix 在 MP3InitDecoder 时一次性分配解码器状态(~23KB), 之后不释放。 */
#include "helix_memory.h"

static unsigned char *g_pool = 0;
static size_t g_pool_size = 0;
static size_t g_pool_used = 0;

void helix_pool_init(void *pool, size_t size)
{
    g_pool = (unsigned char *)pool;
    g_pool_size = size;
    g_pool_used = 0;   /* 重置: 每次播放 MP3 前重新分配 */
}

void *helix_malloc(size_t size)
{
    void *p;
    size = (size + 7) & ~((size_t)7);   /* 8 字节对齐 */
    if (!g_pool || g_pool_used + size > g_pool_size) return 0;
    p = &g_pool[g_pool_used];
    g_pool_used += size;
    return p;
}

void helix_free(void *ptr)
{
    (void)ptr;   /* 池不逐个释放; 下次 helix_pool_init 时整体重置 */
}
