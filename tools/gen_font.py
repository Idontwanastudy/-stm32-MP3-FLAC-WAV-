#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py —— 把 TTF/OTF 字体转成 STM32 播放器用的点阵字库 bin
                  (SSD1306 页式, 多语言共存于一个文件, 每区段可用不同宽度)

为什么是"一个文件 + Unicode 区段"而不是"每个语言一个文件":
    简体/繁体/日文汉字在 Unicode 里都在 U+4E00~U+9FFF 这同一个区段,
    拆成多份会大量重复。所以输出一个文件, 内部按"连续区段"组织:
        查字偏移 = 区段.data_off + (码点 - 区段.start_cp) * 区段.bpg
    纯算术, 不需要索引表, 也不占 MCU 的 RAM。

宽度:  拉丁补充用 8x16(1 列); 西里尔/假名/谚文/汉字/全角用 16x16(2 列)。
       ★ ASCII(英文) 不在字库里: 内 flash 已有专门的 8x16 点阵字 OLED_F8x16(1.5KB),
         按 8 像素宽设计, 显示效果比把 TTF 挤进 8px 更好, 同时兼作字库缺失时的兜底。

文件格式 (小端):
    0x00 char[4] magic "MFNT"
    0x04 u8 ver | u8 h | u8 w_max | u8 bpg_max
    0x08 u16 n_ranges | u16 flags(bit0: 上页+下页排布, bit0=页顶)
    0x0C u32 total_size | u32 crc32(区段表+字形数据) | u32 rsv | u32 rsv | u32 rsv
    0x20 区段表: 每项 16 字节 {u32 start_cp; u32 end_cp; u32 data_off; u32 bpg}
    之后  字形数据(按区段紧凑排列)

字形字节布局 (与 SSD1306 页式、与工程里 OLED_F8x16 一致):
    上页 bytes 在前、下页在后; 每字节 = 一列 8 个竖像素, bit0 = 该页最上面一行
    byte[page*cols + col] = Σ (像素(col, page*8+bit) << bit),  共 cols*2 字节

用法:
    python3 gen_font.py --font NotoSansCJKsc-Regular.otf --out font16.bin     # 六种文字全含
    python3 gen_font.py --font Carlito-Regular.ttf --preset basic --out f.bin # 只英/西文/俄(小)
    python3 gen_font.py --selftest --out font16_selftest.bin                  # 不需要字体, 几何图案
    python3 gen_font.py --font X.ttf --preview "音AЖ漢한"                      # 终端预览点阵

依赖: pillow
"""

import argparse
import struct
import sys
import zlib

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow: pip install pillow")

GLYPH_H = 16
MAGIC = b"MFNT"
VERSION = 1
FLAGS = 0x01          # 上页+下页排布, bit0 为页顶

# 区段: (起始码点, 结束码点, 列数, 渲染字号, x偏移, 基线行, 说明)
#   cols=8 → 8x16(占 1 字符列);  cols=16 → 16x16(占 2 字符列)
#   ★基线行 = 把文字基线放在第几行(0~15)。渲染用 anchor="ls"(左-基线)对齐,
#     这样所有字共享同一条基线; 若改用"墨迹顶部"对齐, 像 "一" 这种矮字会被顶到最上面。
#   ★字号/基线不是拍脑袋定的, 是用 tools 里的测量脚本按"整段墨迹必须落在 0~15 行内"实测出来的:
#     汉字/假名 15px@基线14, 谚文 15px@基线14, 俄文 13px@基线13, 全角 12px@基线13, 西文补充 12px@基线12。
RANGES_FULL = [
    (0x00A0, 0x00FF,  8, 12,  0, 12, "Latin-1 补充/西文标点"),
    (0x0400, 0x04FF, 16, 13,  2, 13, "西里尔(俄文)"),
    # CJK 标点拆三段: 把 U+302A~302D(声调符号) 与 U+3031~3035(竖排假名重复记号) 排除,
    # 它们比 em 还高近两倍(实测 27 行), 混在同一段里会把整段可用字号被迫压到 10px。
    (0x3000, 0x3029, 16, 14,  0, 13, "CJK 标点(含《》「」【】)"),
    (0x302E, 0x3030, 16, 14,  0, 13, "CJK 标点(续)"),
    (0x3036, 0x303F, 16, 14,  0, 13, "CJK 标点(续)"),
    (0x3040, 0x30FF, 16, 15,  0, 14, "平假名/片假名(日文)"),
    (0xAC00, 0xD7A3, 16, 15,  0, 14, "谚文音节(韩文)"),
    (0x4E00, 0x9FFF, 16, 15,  0, 14, "CJK 汉字(简/繁/日共用)"),
    (0xFF00, 0xFFEF, 16, 12,  0, 13, "全角字符"),
]
RANGE_ASCII = (0x0020, 0x007E, 16, 15, 0, 14, "ASCII(双宽 16x16)")

PRESETS = {
    "full": RANGES_FULL,
    "basic": [r for r in RANGES_FULL if r[0] in (0x00A0, 0x0400)],   # 西文补充 + 俄文(最小)
    "cjk":   [r for r in RANGES_FULL if "CJK" in r[6]],              # CJK 标点 + 汉字
}


def render_glyph(font, cp, cols, x_off=0, baseline=16):
    """渲染一个码点, 返回 cols*2 字节(上页 cols 字节 + 下页 cols 字节)
       x_off/baseline: 定位用"左-基线"锚点, baseline 是把基线放在第几行"""
    bpg = cols * 2
    img = Image.new("1", (cols, GLYPH_H), 0)
    if font is not None:
        d = ImageDraw.Draw(img)
        try:
            d.text((x_off, baseline), chr(cp), font=font, fill=1, anchor="ls")
        except Exception:
            return None
    buf = bytearray(bpg)
    p = img.load()
    for y in range(GLYPH_H):
        page, bit = divmod(y, 8)
        for x in range(cols):
            if p[x, y]:
                buf[page * cols + x] |= (1 << bit)
    return bytes(buf)


def selftest_glyph(cp, cols):
    """几何图案代替真字形: 外框 + 码点低 8 位画成方块, 用来验证整条链路"""
    bpg = cols * 2
    buf = bytearray(bpg)

    def px(x, y):
        page, bit = divmod(y, 8)
        buf[page * cols + x] |= (1 << bit)

    for x in range(cols):
        px(x, 0)
        px(x, GLYPH_H - 1)
    for y in range(GLYPH_H):
        px(0, y)
        px(cols - 1, y)
    span = cols - 4
    for i in range(min(8, span)):
        if (cp >> i) & 1:
            for x in range(2 + i, 4 + i):
                for y in range(6, 10):
                    px(x, y)
    return bytes(buf)


def ascii_art(data, cols):
    lines = []
    for y in range(GLYPH_H):
        page, bit = divmod(y, 8)
        lines.append("".join("#" if (data[page * cols + x] >> bit) & 1 else "."
                             for x in range(cols)))
    return lines


def pack(entries, blob):
    """entries: (start, end, data_off, bpg); 返回 (bin, 绝对偏移表, crc)"""
    hdr_len = 32 + 16 * len(entries)
    body = bytearray()
    abs_entries = []
    for s, e, off, bpg in entries:
        body += struct.pack("<IIII", s, e, off + hdr_len, bpg)
        abs_entries.append((s, e, off + hdr_len, bpg))
    body += blob
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    header = bytearray()
    header += MAGIC
    header += struct.pack("<BBBB", VERSION, GLYPH_H, 16, 32)   # ver, h, w_max, bpg_max
    header += struct.pack("<HH", len(entries), FLAGS)
    header += struct.pack("<IIIII", hdr_len + len(blob), crc, 0, 0, 0)
    assert len(header) == 32, len(header)
    return bytes(header) + bytes(body), abs_entries, crc


def build_all(font_file, ranges, selftest):
    cache = {}
    entries = []
    blob = bytearray()
    total = sum(e - s + 1 for s, e, _, _, _, _, _ in ranges)
    done = 0
    print("  %-26s %-18s %6s %9s" % ("区段", "码点范围", "宽度", "大小"))
    for start, end, cols, px, xo, base, name in ranges:
        font = None
        if not selftest:
            if px not in cache:
                cache[px] = ImageFont.truetype(font_file, px)
            font = cache[px]
        data_off = len(blob)
        bpg = cols * 2
        for cp in range(start, end + 1):
            g = selftest_glyph(cp, cols) if selftest else render_glyph(font, cp, cols, xo, base)
            blob += g if g else bytes(bpg)
            done += 1
            if done % 4000 == 0:
                print("  渲染 %d/%d ..." % (done, total), end="\r", flush=True)
        entries.append((start, end, data_off, bpg))
        print("  %-26s U+%04X-U+%04X %4dx16 %7.1f KB" %
              (name, start, end, cols, (end - start + 1) * bpg / 1024))
    print("  %-26s %-18s %6s %7.2f MB" % ("合计", "", "", len(blob) / 1048576))
    return pack(entries, blob)


def main():
    ap = argparse.ArgumentParser(description="TTF → STM32 点阵字库 bin")
    ap.add_argument("--font", help="TTF/OTF 字体文件路径")
    ap.add_argument("--out", default="font16.bin", help="输出文件名")
    ap.add_argument("--preset", default="full", choices=list(PRESETS.keys()),
                    help="full=六种文字全含(~1MB) basic=西文补充+俄文(小,便于验证) cjk=标点+汉字")
    ap.add_argument("--selftest", action="store_true", help="不用字体文件, 生成几何图案自检字库")
    ap.add_argument("--preview", help="渲染这些字符并打印点阵(不烧板子先看效果)")
    ap.add_argument("--px", type=int, help="覆盖所有区段的渲染字号(调这个字会变大变小)")
    ap.add_argument("--baseline", type=int, help="覆盖所有区段的基线行(整体上下微调)")
    ap.add_argument("--x-off", type=int, help="覆盖所有区段的 x 偏移(整体左右微调)")
    ap.add_argument("--with-ascii", action="store_true",
                    help="把 ASCII 也放进字库(16x16 双宽); 默认不放, 用内 flash 的 OLED_F8x16")
    args = ap.parse_args()

    if not args.selftest and not args.font:
        ap.error("要么给 --font, 要么用 --selftest")

    ranges = [list(r) for r in PRESETS[args.preset]]
    if args.with_ascii:
        ranges = [list(RANGE_ASCII)] + ranges
    if args.px is not None:
        for r in ranges:
            r[3] = args.px
    if args.baseline is not None:
        for r in ranges:
            r[5] = args.baseline
    if args.x_off is not None:
        for r in ranges:
            r[4] = args.x_off

    if args.preview:
        if not args.font:
            ap.error("--preview 需要 --font")
        print("=== 预览 ===")
        pcache = {}
        for ch in args.preview:
            cp = ord(ch)
            cols, px, xo, yo = 16, 16, 0, 0
            for s, e, c, p, x, y, _ in ranges:
                if s <= cp <= e:
                    cols, px, xo, yo = c, p, x, y
                    break
            key = (cols, px)
            if key not in pcache:
                pcache[key] = ImageFont.truetype(args.font, px)
            g = render_glyph(pcache[key], cp, cols, xo, yo)
            if g is None:
                print("--- U+%04X %s : 字体里没这个字形 ---" % (cp, ch))
                continue
            print("--- U+%04X  %s  (%dx16, 字号 %d) ---" % (cp, ch, cols, px))
            for ln in ascii_art(g, cols):
                print("  " + ln)
        return

    print("=== 生成字库 (%s) ===" % ("自检几何图案" if args.selftest else args.font))
    data, entries, crc = build_all(args.font, ranges, args.selftest)
    with open(args.out, "wb") as f:
        f.write(data)
    print("\n输出: %s  (%.2f MB)" % (args.out, len(data) / 1048576))
    print("区段数: %d, 字形数: %d, CRC32: 0x%08X" %
          (len(entries), sum(e - s + 1 for s, e, _, _ in entries), crc))
    print("把这个 bin 改名成 font16.bin 放 SD 卡根目录, 设备开机会自动烧进 W25Q64 并校验")


if __name__ == "__main__":
    main()
