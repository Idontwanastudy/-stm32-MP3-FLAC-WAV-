/*
 * test_font_format.c - 字库逻辑的宿主机单元测试 (gcc 编译, 不需要 STM32)
 *
 * 编译运行(在 tools/ 目录):
 *     gcc -Wall -Wextra -O2 -I../Core/Inc -o /tmp/test_font ../Core/Src/font_format.c test_font_format.c
 *     /tmp/test_font font16_basic.bin
 *
 * 它把字库 bin 读进一个"假 Flash"数组, 用 font_read_fn 回调喂给 font_format.c,
 * 然后验证: 头部解析 / 区段表 / CRC 自洽 / 查字字节正确 / 越界保护 / 缺字回退 / LRU 淘汰后仍正确。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font_format.h"

static uint8_t *g_flash;
static uint32_t g_flash_len;
static uint32_t g_read_count;      /* 统计 Flash 访问次数, 用来验证缓存真的生效 */

static void fake_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    g_read_count++;
    if (addr + len > g_flash_len) {          /* 越界: 用 0xFF 填充并报警 */
        fprintf(stderr, "!! 越界读 addr=0x%X len=%u (flash %u)\n", addr, len, g_flash_len);
        memset(buf, 0xFF, len);
        return;
    }
    memcpy(buf, g_flash + addr, len);
}

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  [OK]   %s\n", msg); \
                              else { printf("  [FAIL] %s\n", msg); fails++; } } while (0)

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "font16_basic.bin";
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("打开字库"); return 1; }
    fseek(fp, 0, SEEK_END);
    g_flash_len = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_flash = malloc(g_flash_len);
    if (fread(g_flash, 1, g_flash_len, fp) != g_flash_len) { fclose(fp); return 1; }
    fclose(fp);
    printf("=== 载入字库 %s (%u 字节) 作为假 Flash ===\n", path, g_flash_len);

    static font_t font;
    int rc = font_parse(&font, fake_read);
    printf("\n[1] 头部解析\n");
    CHECK(rc == 0, "font_parse 成功");
    printf("       ver=%u h=%u w_max=%u bpg_max=%u n_ranges=%u total=%u crc=0x%08X\n",
           font.ver, font.glyph_h, font.glyph_w_max, font.bpg_max,
           font.n_ranges, font.total_size, font.crc32);
    CHECK(font.total_size == g_flash_len, "total_size 与文件长度一致");
    CHECK(font.n_ranges > 0, "至少一个区段");

    printf("\n[2] 区段表\n");
    for (uint16_t i = 0; i < font.n_ranges; i++) {
        const font_range_t *r = &font.ranges[i];
        uint64_t last = (uint64_t)r->data_off + (uint64_t)(r->end_cp - r->start_cp + 1) * r->bpg;
        printf("       U+%04X-U+%04X off=0x%06X bpg=%u 宽=%u 末字节=%llu\n",
               r->start_cp, r->end_cp, r->data_off, r->bpg, r->bpg / 2,
               (unsigned long long)last);
        CHECK(last <= font.total_size, "区段数据不越界");
        CHECK(r->bpg == 16 || r->bpg == 32, "bpg 是 16 或 32");
    }

    printf("\n[3] CRC32 自洽(固件算出的 CRC 必须等于文件头里存的)\n");
    {
        uint8_t *body = malloc(font.total_size - FONT_HDR_SIZE);
        fake_read(FONT_HDR_SIZE, body, font.total_size - FONT_HDR_SIZE);
        uint32_t crc = font_crc32(body, font.total_size - FONT_HDR_SIZE);
        printf("       算出 0x%08X / 文件头 0x%08X\n", crc, font.crc32);
        CHECK(crc == font.crc32, "CRC 一致 (说明固件能识别字库是否烧坏)");
        free(body);
    }

    printf("\n[4] 查字字节正确性(与直接按偏移读出来的比)\n");
    const uint32_t test_cps[] = { 0x0416 /*Ж*/, 0x0451 /*ё*/, 0x00AB /*«*/ };
    for (unsigned k = 0; k < sizeof(test_cps) / sizeof(test_cps[0]); k++) {
        uint32_t cp = test_cps[k];
        uint8_t bpg = 0;
        const uint8_t *g = font_glyph(&font, cp, &bpg);
        if (!g) { printf("  [FAIL] U+%04X 没查到\n", cp); fails++; continue; }
        /* 自己按区段算一遍期望偏移 */
        const font_range_t *r = NULL;
        for (uint16_t i = 0; i < font.n_ranges; i++)
            if (cp >= font.ranges[i].start_cp && cp <= font.ranges[i].end_cp) r = &font.ranges[i];
        uint8_t expect[FONT_MAX_BPG];
        fake_read(r->data_off + (cp - r->start_cp) * r->bpg, expect, r->bpg);
        char msg[96];
        snprintf(msg, sizeof(msg), "U+%04X 字节一致 (%u 字节)", cp, bpg);
        CHECK(memcmp(g, expect, bpg) == 0, msg);
        int nonzero = 0;
        for (uint8_t i = 0; i < bpg; i++) if (g[i]) nonzero = 1;
        snprintf(msg, sizeof(msg), "U+%04X 不是空字形", cp);
        CHECK(nonzero, msg);
    }

    printf("\n[5] 缺字回退\n");
    {
        uint8_t bpg = 0;
        CHECK(font_glyph(&font, 0x9FFF, &bpg) == NULL, "字库里没有的码点返回 NULL(上层画 '?')");
        CHECK(font_glyph(&font, 0x20, &bpg) == NULL, "空格/ASCII 不在字库(走内 flash 的 F8x16)");
    }

    printf("\n[6] LRU 缓存: 连续查 %d 个不同字(远超 %d 个槽)后仍要正确\n",
           FONT_CACHE_SLOTS * 2 + 5, FONT_CACHE_SLOTS);
    {
        int bad = 0;
        for (uint32_t cp = font.ranges[0].start_cp;
             cp < font.ranges[0].start_cp + FONT_CACHE_SLOTS * 2 + 5 &&
             cp <= font.ranges[0].end_cp; cp++) {
            uint8_t bpg = 0;
            const uint8_t *g = font_glyph(&font, cp, &bpg);
            uint8_t expect[FONT_MAX_BPG];
            fake_read(font.ranges[0].data_off + (cp - font.ranges[0].start_cp) * font.ranges[0].bpg,
                      expect, font.ranges[0].bpg);
            if (!g || memcmp(g, expect, font.ranges[0].bpg) != 0) bad++;
        }
        CHECK(bad == 0, "淘汰后重新查到的字形仍然正确");
    }

    printf("\n[7] 重复查同一个字必须命中缓存(Flash 读取次数不增加)\n");
    {
        uint8_t bpg = 0;
        font_glyph(&font, 0x0416, &bpg);              /* 先查一次, 把字放进缓存 */
        uint32_t before = g_read_count;
        for (int i = 0; i < 100; i++) {
            const uint8_t *g = font_glyph(&font, 0x0416, &bpg);
            if (!g) { printf("  [FAIL] 缓存查询失败\n"); fails++; }
        }
        printf("       查 100 次 Flash 访问增量 = %u\n", g_read_count - before);
        CHECK(g_read_count == before, "100 次重复查询 0 次访问 Flash (滚动字幕就靠这个省开销)");
    }

    printf("\n[8] 头部损坏/野字库的容错\n");
    {
        static font_t f2;
        g_flash[0] = 'X';                          /* 破坏魔数 */
        CHECK(font_parse(&f2, fake_read) == -1, "魔数错 → 返回 -1(设备会提示没烧字库)");
        g_flash[0] = 'M';
        g_flash[4] = 9;                            /* 版本不符 */
        CHECK(font_parse(&f2, fake_read) == -2, "版本不符 → -2");
        g_flash[4] = 1;
        CHECK(font_parse(&f2, fake_read) == 0, "恢复后又能正常解析");
    }

    printf("\n===== 结果: %s (失败 %d 项) =====\n", fails ? "有失败" : "全部通过", fails);
    free(g_flash);
    return fails ? 1 : 0;
}
