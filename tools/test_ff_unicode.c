/*
 * test_ff_unicode.c - cp936 转换表的宿主机单元测试
 *
 * 编译运行(在工程根目录):
 *   gcc -Wall -Wextra -O2 -I Middlewares/Third_Party/FatFs/src -I FATFS/Target \
 *       -I tools -o /tmp/test_ff Core/Src/ff_unicode.c tools/test_ff_unicode.c && /tmp/test_ff
 *
 * 原理: tools/gen_ff_unicode.py 生成表时**同时**用 Python 的 cp936 编码器导出期望值
 * (ff_vec.h), 这里逐一比对 C 实现的 ff_convert(), 保证两边一致(而不是"看起来差不多")。
 */
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "ff_vec.h"          /* 由 gen_ff_unicode.py --vectors 生成 */

static int fails;

static void chk(int cond, const char *what)
{
    if (cond) {
        printf("  [OK]   %s\n", what);
    } else {
        printf("  [FAIL] %s\n", what);
        fails++;
    }
}

/* GBK 双字节 → Unicode (dir=1); 转不出来返回 0 */
static WCHAR gbk2u(unsigned gbk)
{
    return ff_convert((WCHAR)gbk, 1);
}
/* Unicode → GBK (dir=0) */
static WCHAR u2gbk(unsigned cp)
{
    return ff_convert((WCHAR)cp, 0);
}

int main(void)
{
    printf("=== 1) 逐条比对 Python cp936 的期望值(%u 条) ===\n", (unsigned)FF_VECTOR_COUNT);
    int bad = 0;
    for (unsigned i = 0; i < FF_VECTOR_COUNT; i++) {
        unsigned cp = ff_vectors[i].cp, gbk = ff_vectors[i].gbk;
        if (gbk2u(gbk) != cp) {
            printf("  [FAIL] GBK 0x%04X → 期望 U+%04X, 得到 U+%04X\n", gbk, cp, gbk2u(gbk));
            bad++;
        }
        if (u2gbk(cp) != gbk) {
            printf("  [FAIL] U+%04X → 期望 0x%04X, 得到 0x%04X\n", cp, gbk, u2gbk(cp));
            bad++;
        }
    }
    chk(bad == 0, "双向转换与 Python cp936 完全一致");

    printf("\n=== 2) 关键字符抽查(这几类正是原来打不开文件的原因) ===\n");
    struct { unsigned cp; unsigned gbk; const char *name; } k[] = {
        { 0x0410, 0xA7A1, "俄文 А" }, { 0x044F, 0xA7F1, "俄文 я" },
        { 0x0451, 0xA7D7, "俄文 ё" },
        { 0x3042, 0xA4A2, "平假名 あ" }, { 0x30A2, 0xA5A2, "片假名 ア" },
        { 0x554A, 0xB0A1, "简体 啊" }, { 0x6F22, 0x9D68, "繁体/日文 漢" },
        { 0x5E2B, 0x8E9F, "繁体 師" }, { 0xFF21, 0xA3C1, "全角 Ａ" },
        { 0x03A9, 0xA6B8, "希腊 Ω" }, { 0x0401, 0xA7A7, "俄文 Ё" },
    };
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: U+%04X ↔ 0x%04X 双向一致",
                 k[i].name, k[i].cp, k[i].gbk);
        chk(gbk2u(k[i].gbk) == k[i].cp && u2gbk(k[i].cp) == k[i].gbk, buf);
    }

    printf("\n=== 3) 边界与容错 ===\n");
    chk(ff_convert('A', 1) == 'A' && ff_convert('A', 0) == 'A', "ASCII 原样返回");
    chk(gbk2u(0x0000) == 0 || gbk2u(0x0000) != 0xFFFF, "0x0000 不崩");
    chk(gbk2u(0x817F) == 0, "非法 GBK 码(低位 0x7F)返回 0 → FatFs 用 '?'");
    chk(u2gbk(0xD55C) == 0, "GBK 里没有的字符(韩文 한)返回 0(这是已知限制)");
    chk(ff_convert(0x4E00, 0) != 0, "最早汉字 U+4E00 可转换");
    chk(ff_convert(0x9FA5, 0) != 0, "最末汉字 U+9FA5 可转换");

    printf("\n=== 4) 模拟 FatFs 的用法: 把一个 UTF-16 文件名转成 GBK 再转回来 ===\n");
    {
        /* 文件名: ヨルシカ - ただ君に晴れ (含片假名/汉字/假名), 模拟磁盘上的 UTF-16 → GBK */
        const WCHAR u16[] = { 0x30E8,0x30EB,0x30B7,0x30AB,' ',0x002D,' ',
                              0x305F,0x3060,0x541B,0x306B,0x6674,0x308C };
        char gbk[64]; int n = 0, ok = 1;
        for (unsigned i = 0; i < sizeof(u16)/sizeof(u16[0]); i++) {
            WCHAR g = u2gbk(u16[i]);
            if (g == 0) { ok = 0; break; }
            if (g < 0x80) gbk[n++] = (char)g;
            else { gbk[n++] = (char)(g >> 8); gbk[n++] = (char)(g & 0xFF); }
        }
        gbk[n] = 0;
        printf("       盘上名字(UTF-16) → GBK: %d 字节%s\n", n, ok ? "" : " (有字符转不了!)");
        /* 再转回 UTF-16 逐字比对: 这就是 FatFs 打开文件时的比较过程 */
        int same = 1, m = 0;
        for (int i = 0; i < n; ) {
            WCHAR g = (unsigned char)gbk[i] < 0x80 ? (unsigned char)gbk[i]
                    : (WCHAR)(((unsigned char)gbk[i] << 8) | (unsigned char)gbk[i+1]);
            int step = ((unsigned char)gbk[i] < 0x80) ? 1 : 2;
            if (gbk2u(g) != u16[m]) same = 0;
            i += step; m++;
        }
        chk(ok && same && m == (int)(sizeof(u16)/sizeof(u16[0])),
            "日文文件名 GBK 往返后与磁盘上的 UTF-16 名字逐字相同 → f_open 能匹配上");
    }

    printf("\n===== 结果: %s (失败 %d 项) =====\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
