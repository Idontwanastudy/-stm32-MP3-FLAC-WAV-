/*
 * font_store.c - 字库存储层实现 (W25Q64 + SD 卡烧写)
 * 格式与查表逻辑在 font_format.c; 这里只做"介质相关"的事。
 */
#include "font_store.h"
#include "font_format.h"
#include "w25q64.h"
#include "fatfs.h"
#include <string.h>

/* 上电时是否整段校验字库 CRC(读 1MB 约 0.2s; 能自动发现字库烧坏并触发重烧) */
#ifndef FONT_VERIFY_AT_INIT
#define FONT_VERIFY_AT_INIT  1
#endif

static font_t s_font;

/* ---- 介质读取钩子: 地址是"相对字库起始"的偏移 ---- */
static void flash_read_hook(uint32_t addr, uint8_t *buf, uint32_t len)
{
    W25Q64_ReadData(FONT_FLASH_ADDR + addr, buf, len);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 把字库正文(头部之后)整段读一遍算 CRC32 —— 与 gen_font.py / font_crc32 用同一个算法。
 * 分块读以省 RAM: CRC32 是流式的, 可以逐块累积。 */
static uint32_t flash_body_crc(uint32_t total_size)
{
    static uint8_t chunk[512];
    uint32_t crc = 0xFFFFFFFFu, off, len;

    for (off = FONT_HDR_SIZE; off < total_size; off += len) {
        len = total_size - off;
        if (len > sizeof(chunk)) len = sizeof(chunk);
        flash_read_hook(off, chunk, len);
        for (uint32_t i = 0; i < len; i++) {
            crc ^= chunk[i];
            for (int k = 0; k < 8; k++) {
                crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(0u - (crc & 1u)));
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

int font_store_init(void)
{
    int rc = font_parse(&s_font, flash_read_hook);
    if (rc != 0) return rc;
#if FONT_VERIFY_AT_INIT
    {
        uint32_t crc = flash_body_crc(s_font.total_size);
        if (crc != s_font.crc32) {
            s_font.valid = 0;
            return -20;                 /* 字库存在但内容坏了(CRC 不符) */
        }
    }
#endif
    return 0;
}

uint8_t font_store_ready(void)
{
    return s_font.valid;
}

uint32_t font_store_size(void)
{
    return s_font.valid ? s_font.total_size : 0u;
}

uint16_t font_store_ranges(void)
{
    return s_font.valid ? s_font.n_ranges : 0u;
}

const uint8_t *font_store_glyph(uint32_t cp, uint8_t *bpg_out)
{
    return font_glyph(&s_font, cp, bpg_out);
}

uint8_t font_store_burn_file(const char *path,
                             void (*progress)(uint32_t done, uint32_t total))
{
    static uint8_t page[W25Q64_PAGE_SIZE];      /* 256 字节页缓冲, 省 RAM */
    FIL f;
    UINT br;
    uint32_t total, file_crc, off;
    uint8_t  hdr[FONT_HDR_SIZE];

    /* ---- 第 1 遍: 打开 → 查头 → 顺序读完算源文件 CRC(不用 f_lseek, 避免大偏移定位问题) ---- */
    if (f_open(&f, path, FA_READ) != FR_OK) return 1;
    total = (uint32_t)f_size(&f);
    if (total < FONT_HDR_SIZE || total > FONT_FLASH_MAX_SIZE) { f_close(&f); return 2; }
    if (f_read(&f, hdr, FONT_HDR_SIZE, &br) != FR_OK || br != FONT_HDR_SIZE) { f_close(&f); return 2; }
    if (memcmp(hdr, "MFNT", 4) != 0) { f_close(&f); return 3; }
    {
        uint32_t want = rd32(hdr + 12);
        if (want != total) { f_close(&f); return 2; }     /* 头部里写的长度必须等于文件长度 */
    }
    file_crc = 0xFFFFFFFFu;
    for (off = 0; off < total - FONT_HDR_SIZE; ) {
        UINT want = (total - FONT_HDR_SIZE - off > sizeof(page)) ? sizeof(page)
                                                                 : (UINT)(total - FONT_HDR_SIZE - off);
        if (f_read(&f, page, want, &br) != FR_OK || br != want) { f_close(&f); return 2; }
        for (UINT i = 0; i < br; i++) {
            file_crc ^= page[i];
            for (int k = 0; k < 8; k++) {
                file_crc = (file_crc >> 1) ^ (0xEDB88320u & (uint32_t)(0u - (file_crc & 1u)));
            }
        }
        off += br;
    }
    file_crc ^= 0xFFFFFFFFu;
    f_close(&f);
    if (file_crc != rd32(hdr + 16)) return 3;             /* 源文件自己就坏了, 别烧 */

    /* ---- 第 2 遍: 重新打开, 逐 4KB 扇区 擦除+写入 ---- */
    if (f_open(&f, path, FA_READ) != FR_OK) return 1;
    for (off = 0; off < total; off += W25Q64_SECTOR_SIZE) {
        uint32_t sec_end = off + W25Q64_SECTOR_SIZE;
        if (sec_end > total) sec_end = total;

        W25Q64_EraseSector(FONT_FLASH_ADDR + off);
        for (uint32_t p = off; p < sec_end; p += W25Q64_PAGE_SIZE) {
            uint32_t n = sec_end - p;
            if (n > W25Q64_PAGE_SIZE) n = W25Q64_PAGE_SIZE;
            if (f_read(&f, page, n, &br) != FR_OK || br != n) { f_close(&f); return 2; }
            if (n < W25Q64_PAGE_SIZE) memset(page + n, 0xFF, W25Q64_PAGE_SIZE - n);
            W25Q64_WritePage(FONT_FLASH_ADDR + p, page, W25Q64_PAGE_SIZE);
        }
        if (progress && ((off / W25Q64_SECTOR_SIZE) % 16) == 0) progress(sec_end, total);
    }
    f_close(&f);

    /* ---- 第 3 遍: 回读校验 ---- */
    if (flash_body_crc(total) != rd32(hdr + 16)) return 4;

    if (progress) progress(total, total);
    font_store_init();                                    /* 重新加载区段表 */
    return 0;
}

int font_store_boot_check(const char *sd_path,
                          void (*progress)(uint32_t done, uint32_t total))
{
    uint8_t hdr[FONT_HDR_SIZE], fh[FONT_HDR_SIZE];
    FIL f; UINT br;
    uint32_t file_crc;
    uint8_t rc;

    /* SD 上没有字库文件 → 什么都不做(保留 Flash 里原有字库) */
    if (f_open(&f, sd_path, FA_READ) != FR_OK) return 0;
    if (f_read(&f, hdr, FONT_HDR_SIZE, &br) != FR_OK || br != FONT_HDR_SIZE) { f_close(&f); return 0; }
    f_close(&f);
    if (memcmp(hdr, "MFNT", 4) != 0) return 0;             /* 不是字库文件, 忽略 */
    file_crc = rd32(hdr + 16);

    /* Flash 里已经有一份可用且 CRC 相同的字库 → 不用烧 */
    if (s_font.valid && s_font.crc32 == file_crc) return 0;

    /* 看看 Flash 头部是否同 CRC(即使本次没做整段校验也能快速判断) */
    W25Q64_ReadData(FONT_FLASH_ADDR, fh, FONT_HDR_SIZE);
    if (memcmp(fh, "MFNT", 4) == 0 && rd32(fh + 16) == file_crc) return 0;

    rc = font_store_burn_file(sd_path, progress);
    if (rc != 0) return -(int)rc;
    return 1;
}
