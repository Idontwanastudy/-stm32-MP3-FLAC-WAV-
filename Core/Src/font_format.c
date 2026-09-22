/*
 * font_format.c - 字库格式解析 / 区段查表 / 字形 LRU 缓存 (纯逻辑, 可在宿主机测试)
 * 格式说明见 font_format.h
 */
#include "font_format.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t font_crc32(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

int font_parse(font_t *f, font_read_fn read)
{
    uint8_t hdr[FONT_HDR_SIZE];

    if (f == NULL || read == NULL) return -100;
    memset(f, 0, sizeof(*f));
    f->read = read;

    read(0, hdr, FONT_HDR_SIZE);
    if (hdr[0] != 'M' || hdr[1] != 'F' || hdr[2] != 'N' || hdr[3] != 'T') return -1;  /* 没烧字库/魔数不对 */
    f->ver         = hdr[4];
    f->glyph_h     = hdr[5];
    f->glyph_w_max = hdr[6];
    f->bpg_max     = hdr[7];
    f->n_ranges    = (uint16_t)(hdr[8] | ((uint16_t)hdr[9] << 8));
    f->flags       = (uint16_t)(hdr[10] | ((uint16_t)hdr[11] << 8));
    f->total_size  = rd32(hdr + 12);
    f->crc32       = rd32(hdr + 16);

    if (f->ver != 1)                      return -2;   /* 版本不符 */
    if (f->glyph_h != FONT_GLYPH_H)       return -3;   /* 字高不是 16, 本工程不支持 */
    if (f->bpg_max == 0 || f->bpg_max > FONT_MAX_BPG) return -4;
    if (f->n_ranges == 0 || f->n_ranges > FONT_MAX_RANGES) return -5;
    if (f->total_size < FONT_HDR_SIZE + FONT_RANGE_SIZE * f->n_ranges) return -6;

    for (uint16_t i = 0; i < f->n_ranges; i++) {
        uint8_t e[FONT_RANGE_SIZE];
        font_range_t *r = &f->ranges[i];
        read(FONT_HDR_SIZE + (uint32_t)i * FONT_RANGE_SIZE, e, FONT_RANGE_SIZE);
        r->start_cp = rd32(e);
        r->end_cp   = rd32(e + 4);
        r->data_off = rd32(e + 8);
        r->bpg      = rd32(e + 12);

        if (r->end_cp < r->start_cp)                        return -7;   /* 区段反了 */
        if (r->bpg == 0 || r->bpg > FONT_MAX_BPG || (r->bpg & 1u)) return -8;  /* bpg 非法 */
        /* 越界保护: 该区段最后一个字形的末尾必须在字库总长度内 */
        {
            uint64_t last = (uint64_t)r->data_off +
                            (uint64_t)(r->end_cp - r->start_cp + 1) * r->bpg;
            if (last > (uint64_t)f->total_size)              return -9;
        }
    }

    f->valid = 1;
    return 0;
}

const uint8_t *font_glyph(font_t *f, uint32_t cp, uint8_t *bpg_out)
{
    const font_range_t *r = NULL;

    if (f == NULL || !f->valid) return NULL;

    for (uint16_t i = 0; i < f->n_ranges; i++) {
        if (cp >= f->ranges[i].start_cp && cp <= f->ranges[i].end_cp) {
            r = &f->ranges[i];
            break;
        }
    }
    if (r == NULL) return NULL;                 /* 字库里没有这个字 → 上层画 '?' */

    if (bpg_out) *bpg_out = (uint8_t)r->bpg;

#if FONT_CACHE_SLOTS > 0
    for (int i = 0; i < FONT_CACHE_SLOTS; i++) {
        if (f->cache_bpg[i] && f->cache_cp[i] == cp) {
            f->cache_tick[i] = ++f->tick;       /* 命中: 刷新 LRU */
            return f->cache_data[i];
        }
    }
    {
        int victim = 0;
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < FONT_CACHE_SLOTS; i++) {
            if (f->cache_bpg[i] == 0) { victim = i; break; }   /* 优先用空槽 */
            if (f->cache_tick[i] < oldest) { oldest = f->cache_tick[i]; victim = i; }
        }
        f->read(r->data_off + (cp - r->start_cp) * r->bpg,
                f->cache_data[victim], r->bpg);
        f->cache_cp[victim]   = cp;
        f->cache_bpg[victim]  = (uint8_t)r->bpg;
        f->cache_tick[victim] = ++f->tick;
        return f->cache_data[victim];
    }
#else
    /* 无缓存版: 调用方需自备缓冲, 这里不支持(保留接口以便裁剪) */
    static uint8_t raw[FONT_MAX_BPG];
    f->read(r->data_off + (cp - r->start_cp) * r->bpg, raw, r->bpg);
    return raw;
#endif
}
