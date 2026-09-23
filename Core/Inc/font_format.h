/*
 * font_format.h - 点阵字库"格式与查表"逻辑 (纯逻辑, 不依赖 HAL/STM32)
 *
 * 字库文件(烧在 W25Q64 里)的格式由 tools/gen_font.py 生成, 小端:
 *   0x00 char[4] magic "MFNT"
 *   0x04 u8 ver | u8 h | u8 w_max | u8 bpg_max
 *   0x08 u16 n_ranges | u16 flags(bit0: 上页+下页, bit0=页顶)
 *   0x0C u32 total_size | u32 crc32(body) | u32 rsv | u32 rsv | u32 rsv
 *   0x20 区段表: 每项 {u32 start_cp; u32 end_cp; u32 data_off; u32 bpg}
 *   之后  字形数据: 上页 bpg/2 字节 + 下页 bpg/2 字节, byte[page*cols+col] bit0=页顶
 *
 * 查字 = 纯算术: 偏移 = data_off + (码点 - start_cp) * bpg   → 无索引表, 不占额外 RAM
 *
 * 本文件刻意不包含任何 STM32 头文件: 读写 Flash 通过 font_read_fn 回调注入,
 * 这样同一份代码可以在宿主机上用假 Flash 数组做单元测试(见 tools/test_font_format.c)。
 */
#ifndef __FONT_FORMAT_H
#define __FONT_FORMAT_H

#include <stdint.h>

#define FONT_HDR_SIZE      32u
#define FONT_RANGE_SIZE    16u
#define FONT_MAX_RANGES    16u
#define FONT_MAX_BPG       32u      /* 单字形最大字节数(16x16 = 32) */
#define FONT_GLYPH_H       16u      /* 本工程字高固定 16 */

#ifndef FONT_CACHE_SLOTS
#define FONT_CACHE_SLOTS   96       /* 字形 LRU 缓存槽数(96*32B ≈ 3KB), 0=关缓存 */
#endif

/* 从字库所在介质读 len 字节到 buf(地址相对字库起始; 由 font_store.c 用 W25Q64 实现) */
typedef void (*font_read_fn)(uint32_t addr, uint8_t *buf, uint32_t len);

typedef struct {
    uint32_t start_cp;
    uint32_t end_cp;
    uint32_t data_off;
    uint32_t bpg;        /* 本区段每字形字节数 (16 或 32) */
} font_range_t;

typedef struct {
    uint8_t  valid;      /* 1 = 字库可用 */
    uint8_t  ver;
    uint8_t  glyph_h;    /* 字高(16) */
    uint8_t  glyph_w_max;/* 最大字宽(16) */
    uint8_t  bpg_max;
    uint16_t n_ranges;
    uint16_t flags;
    uint32_t total_size;
    uint32_t crc32;
    font_range_t ranges[FONT_MAX_RANGES];
    font_read_fn read;
#if FONT_CACHE_SLOTS > 0
    uint32_t cache_cp[FONT_CACHE_SLOTS];
    uint32_t cache_tick[FONT_CACHE_SLOTS];
    uint8_t  cache_bpg[FONT_CACHE_SLOTS];       /* 0 = 空槽 */
    uint8_t  cache_data[FONT_CACHE_SLOTS][FONT_MAX_BPG];
    uint32_t tick;
#endif
} font_t;

/* 与 zlib.crc32 一致的 CRC32 (多项式 0xEDB88320, 用于校验字库没烧坏) */
uint32_t font_crc32(const uint8_t *p, uint32_t len);

/* 读头部+区段表并做完整性校验。返回 0=成功; <0 = 具体错误码(见 .c 内注释) */
int font_parse(font_t *f, font_read_fn read);

/* 查字形: 命中返回指向 bpg 字节的指针(仅在下一次 font_glyph 调用前有效!), 缺字返回 NULL。
 * bpg_out 回传本字形的字节数(16=8x16 单列 / 32=16x16 双列) */
const uint8_t *font_glyph(font_t *f, uint32_t cp, uint8_t *bpg_out);

#endif /* __FONT_FORMAT_H */
