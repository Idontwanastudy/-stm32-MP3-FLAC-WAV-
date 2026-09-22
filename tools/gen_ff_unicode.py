#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_ff_unicode.py —— 生成 FatFs 需要的 cp936(GBK) <-> Unicode 双向转换表

为什么需要它:
    FatFs 在 _USE_LFN 且 _LFN_UNICODE=0 时, 文件名以"OEM 编码(本工程=cp936/GBK)"的
    char 数组返回。磁盘上的长文件名实际是 UTF-16, 所以 FatFs 每次列目录/开文件都要
    用 ff_convert() 在两者之间转换。

    本工程原来的 ff_unicode.c 是拿 GB2312 区位码表实现的, 而那张表**只填了汉字**,
    1~9 区(全角字符、平假名、片假名、希腊字母、西里尔字母)全是 0 —— 于是:
        · 俄语文件名(西里尔在 GB2312 第 7 区) → 转换失败 → 打开失败 → "文件损坏/跳过"
        · 日语文件名里的假名(第 4/5 区)        → 同样失败
        · 中文文件名正常(汉字在 16~87 区)      → 所以以前一直没暴露
    而且显示歌名用的 OLED_GBKString_To_UTF8 也用同一张表, 所以即使能打开, 名字也是乱的。

    换成完整的 cp936 表后, 简/繁/日/韩汉字(整个 CJK 基本区)、假名、西里尔、希腊、
    全角字符都能正确转换 → 这类文件名既能打开也能正确显示。

表结构(两向都做成 O(1) 查表, 不用每次二分):
    g2u_tbl[]      : GBK 码按"紧凑索引"排布的 Unicode 值           (126*190 项, 约 48KB)
    u2g_hanzi[]    : U+4E00~U+9FA5 每个码点对应的 GBK 码           (20902 项, 约 42KB)
    u2g_sym[]      : 其余映射(假名/西里尔/希腊/全角/符号), 按 Unicode 排序, 二分查找
    合计约 95KB 内 flash —— 相比它解决的问题(多语言文件名)非常值, 而且以后把内 flash 里
    那 230KB 的 GB2312 点阵字库关掉(字库已搬 W25Q64)还能净省 130KB。

用法:
    python3 gen_ff_unicode.py --out ../Core/Src/ff_unicode.c --vectors /tmp/ff_vec.h
"""

import argparse
import sys

HANZI_LO, HANZI_HI = 0x4E00, 0x9FA5        # CJK 基本区(GBK 全部覆盖)


def gbk_index(hi, lo):
    """GBK 双字节码 → 紧凑索引(0..23939)。与 FatFs 官方 cc936 的索引方式一致"""
    if lo > 0x7E:
        lo -= 1
    return (hi - 0x81) * 190 + (lo - 0x40)


GBK_SLOTS = 126 * 190                       # 0x81..0xFE 每字节 190 个位置


def build_tables():
    g2u = [0] * GBK_SLOTS
    for hi in range(0x81, 0xFF):
        for lo in range(0x40, 0xFF):
            if lo == 0x7F:
                continue
            try:
                s = bytes([hi, lo]).decode('cp936')
            except Exception:
                continue
            if len(s) != 1:                 # 只收"一个双字节码对应一个字符"
                continue
            cp = ord(s)
            if cp > 0xFFFF:
                continue
            g2u[gbk_index(hi, lo)] = cp

    u2g_hanzi = [0] * (HANZI_HI - HANZI_LO + 1)
    u2g_sym = []
    for cp in range(0x20, 0x10000):
        if cp < 0x80:
            continue
        if HANZI_LO <= cp <= HANZI_HI:
            pass
        try:
            b = chr(cp).encode('cp936')
        except Exception:
            continue
        if len(b) != 2:
            continue
        code = (b[0] << 8) | b[1]
        if HANZI_LO <= cp <= HANZI_HI:
            u2g_hanzi[cp - HANZI_LO] = code
        else:
            u2g_sym.append((cp, code))      # 已按 cp 升序(循环顺序) ✓
    return g2u, u2g_hanzi, u2g_sym


def fmt_array(words, per_line=8, width=6):
    out = []
    for i in range(0, len(words), per_line):
        out.append("  " + " ".join(("0x%04X," % w).ljust(width) for w in words[i:i + per_line]))
    return "\n".join(out)


C_TEMPLATE = r'''/*
 * ff_unicode.c - FatFs 的 cp936(GBK) <-> Unicode 转换表  ★本文件由 tools/gen_ff_unicode.py 生成, 勿手改★
 *
 * 用途: _LFN_UNICODE=0 时, FatFs 用 ff_convert() 在 OEM(本工程 = cp936/GBK)与 Unicode 之间转换,
 *       列目录和打开文件都要用它。原来那份只覆盖 GB2312 汉字, 导致**俄语/日语/繁体文件名
 *       转换失败 → 打不开 → 显示"文件损坏/跳过"**(假名在 GB2312 第4/5区、西里尔在第7区, 原表那几区是空的)。
 *
 * 本表覆盖完整 cp936: 整个 CJK 基本区(U+4E00~U+9FA5, 简繁日韩汉字都在内) + 假名 + 西里尔 +
 * 希腊 + 全角字符 + 符号。两向都是 O(1) 查表(汉字区用索引数组, 其余二分)。
 *
 * 覆盖统计: GBK→Unicode %d 项, Unicode→GBK 汉字 %d 项 + 其它 %d 项
 */

#include "ff.h"

#define GBK_SLOT_COUNT   %du   /* 0x81..0xFE 每字节 190 个位置 */
#define HANZI_LO         0x4E00u
#define HANZI_COUNT      %du
#define SYM_COUNT        %du

/* GBK 双字节码 → 紧凑索引(与 FatFs 官方 cc936 一致) */
static unsigned gbk_index(unsigned gbk)
{
    unsigned hi = (gbk >> 8) & 0xFF, lo = gbk & 0xFF;
    if (hi < 0x81 || hi > 0xFE)  return 0xFFFFFFFFu;
    if (lo < 0x40 || lo > 0xFE || lo == 0x7F) return 0xFFFFFFFFu;
    if (lo > 0x7E) lo--;
    return (hi - 0x81) * 190u + (lo - 0x40);
}

/* GBK(紧凑索引) → Unicode; 0 = 该位置没有字符 */
static const WCHAR g2u_tbl[GBK_SLOT_COUNT] = {
%s
};

/* Unicode(U+4E00 起) → GBK 码; 0 = 没有 */
static const WCHAR u2g_hanzi[HANZI_COUNT] = {
%s
};

/* 其余映射(unicode 升序, 便于二分) */
static const WCHAR u2g_sym[SYM_COUNT][2] = {
%s
};

WCHAR ff_convert(WCHAR chr, UINT dir)
{
    if (chr < 0x80) return chr;                  /* ASCII 原样 */

    if (dir) {                                   /* 1: OEM(GBK) → Unicode */
        unsigned idx = gbk_index(chr);
        if (idx >= GBK_SLOT_COUNT) return 0;
        return g2u_tbl[idx];
    } else {                                     /* 0: Unicode → OEM(GBK) */
        if (chr >= HANZI_LO && chr < HANZI_LO + HANZI_COUNT)
            return u2g_hanzi[chr - HANZI_LO];
        {                                        /* 其余: 二分查找 */
            unsigned lo = 0, hi = SYM_COUNT;
            while (lo < hi) {
                unsigned mid = (lo + hi) / 2;
                if (u2g_sym[mid][0] == chr) return u2g_sym[mid][1];
                if (u2g_sym[mid][0] < chr)  lo = mid + 1;
                else                        hi = mid;
            }
        }
        return 0;                                /* 表里没有(如 emoji/扩展区) → FatFs 会用 '?' */
    }
}

/* FatFs 大小写不敏感比较用: 只需处理 ASCII/Latin-1 */
WCHAR ff_wtoupper(WCHAR chr)
{
    if (chr >= 'a' && chr <= 'z') return (WCHAR)(chr - 0x20);
    if (chr >= 0xE0 && chr <= 0xFE && chr != 0xF7) return (WCHAR)(chr - 0x20);
    return chr;
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="../Core/Src/ff_unicode.c")
    ap.add_argument("--vectors", default=None, help="顺带输出测试向量头文件(宿主机测试用)")
    args = ap.parse_args()

    g2u, u2g_hanzi, u2g_sym = build_tables()
    n_g2u = sum(1 for w in g2u if w)
    n_hanzi = sum(1 for w in u2g_hanzi if w)
    print("GBK→Unicode: %d 项 (表长 %d)" % (n_g2u, len(g2u)))
    print("Unicode→GBK: 汉字 %d 项, 其它 %d 项" % (n_hanzi, len(u2g_sym)))

    # 顺便检查几个关键字符, 免得生成了一张没用的表
    key = {'А': 0x0410, 'я': 0x044F, 'あ': 0x3042, 'ア': 0x30A2, '漢': 0x6F22,
           '師': 0x5E2B, '測': 0x6E2C, '한': 0xD55C, 'Ａ': 0xFF21}
    for ch, cp in key.items():
        idx = [i for i, w in enumerate(g2u) if w == cp]
        gbk = None
        if cp >= HANZI_LO and cp < HANZI_LO + len(u2g_hanzi):
            gbk = u2g_hanzi[cp - HANZI_LO]
        else:
            for u, g in u2g_sym:
                if u == cp:
                    gbk = g
        print("  %s (U+%04X): 可转换=%s  GBK码=%s" %
              (ch, cp, "是" if idx or (cp >= HANZI_LO and gbk) else "否",
               ("0x%04X" % gbk) if gbk else "无"))

    body = C_TEMPLATE % (
        n_g2u, n_hanzi, len(u2g_sym),
        GBK_SLOTS, len(u2g_hanzi) + 1, len(u2g_sym),
        fmt_array(g2u),
        fmt_array(u2g_hanzi),
        "\n".join("  {0x%04X,0x%04X}," % (u, g) for u, g in u2g_sym),
    )
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(body)
    print("\n已生成 %s  (%.1f KB 源码)" % (args.out, len(body) / 1024))
    print("内 flash 数据量约 %.1f KB (两个索引表 + 符号表)" % ((len(g2u) + len(u2g_hanzi) + 2 * len(u2g_sym)) * 2 / 1024))

    if args.vectors:
        # 生成测试向量: 双向都要能往返
        vec = ['/* 由 tools/gen_ff_unicode.py 生成: 期望值来自 Python 的 cp936 编码器 */',
               '#ifndef FF_TEST_VECTORS_H', '#define FF_TEST_VECTORS_H',
               'typedef struct { unsigned short cp; unsigned short gbk; } ff_vec_t;',
               'static const ff_vec_t ff_vectors[] = {']
        for ch in "АяЁё" "あいうアイウ" "漢師測한" "Ａ＄" "μΩ" "々":
            try:
                b = ch.encode('cp936')
                if len(b) == 2:
                    vec.append("  {0x%04X, 0x%04X},   /* %s */" % (ord(ch), (b[0] << 8) | b[1], ch))
            except Exception:
                pass
        for cp in (0x4E00, 0x9FA5, 0x554A, 0x6F22):
            b = chr(cp).encode('cp936')
            vec.append("  {0x%04X, 0x%04X}," % (cp, (b[0] << 8) | b[1]))
        vec.append("};")
        vec.append("#define FF_VECTOR_COUNT (sizeof(ff_vectors)/sizeof(ff_vectors[0]))")
        vec.append("#endif")
        with open(args.vectors, "w", encoding="utf-8") as f:
            f.write("\n".join(vec) + "\n")
        print("已生成测试向量 %s" % args.vectors)


if __name__ == "__main__":
    main()
