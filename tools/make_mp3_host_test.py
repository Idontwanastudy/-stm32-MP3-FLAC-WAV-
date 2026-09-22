#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_mp3_host_test.py —— 把工程里的 MP3 解码逻辑「原样」搬到 PC 上做测试

为什么这样做: 手抄代码容易和固件不一致(抄错一个符号就白测了)。
这里直接从 Core/Src/Music_Driver.c **按行切出**两块:
    · MP3 状态变量声明 (static HMP3Decoder mp3_hdec ... mp3_frame_frames)
    · "MP3 帧头校验"那一段 + mp3_decode_next_frame() (源码里连续)
再拼进 tools/mp3_host_test.c 的骨架里 → 编译运行, 跑的就是"固件里那段代码"。

用法(在工程根目录):
    python3 tools/make_mp3_host_test.py > /tmp/mp3_host_test.c
    gcc -O2 -I Core/Inc -I Middlewares/libhelix-mp3 -I Middlewares/libhelix-mp3/utils \\
        -o /tmp/mp3_test /tmp/mp3_host_test.c \\
        Middlewares/libhelix-mp3/*.c Middlewares/libhelix-mp3/utils/*.c
    /tmp/mp3_test 你的.mp3 输出.pcm
"""

import re
import sys

SRC = "Core/Src/Music_Driver.c"
SKEL = "tools/mp3_host_test.c"


def line_index(lines, prefix, what):
    for i, l in enumerate(lines):
        if l.startswith(prefix):
            return i
    sys.exit("在 %s 里找不到 %s (以 %r 开头的那行)" % (SRC, what, prefix))


def main():
    src = open(SRC, encoding="utf-8").read().split("\n")
    skel = open(SKEL, encoding="utf-8").read()

    # 1) MP3 状态变量
    i0 = line_index(src, "static HMP3Decoder mp3_hdec", "MP3 状态变量")
    i1 = line_index(src, "static uint32_t mp3_frame_frames", "mp3_frame_frames 声明")
    statics = "\n".join(src[i0:i1 + 1]) + "\n"

    # 2) 帧头校验 + 解码函数(源码里连续, 一起切)
    j0 = line_index(src, "/* ================= MP3 帧头校验", "帧头校验段")
    j2 = line_index(src, "static int mp3_decode_next_frame(void)", "mp3_decode_next_frame")
    j3 = None
    for k in range(j2 + 1, len(src)):
        if src[k].rstrip() == "}":
            j3 = k
            break
    if j3 is None:
        sys.exit("找不到 mp3_decode_next_frame 的结束大括号")
    func = "\n".join(src[j0:j3 + 1]) + "\n"

    # 3) 贴进骨架(标记用前缀匹配, 免得改个名字就找不到 —— 踩过一次)
    m1 = re.search(r'/\* ==== BEGIN[^\n]*\*/', skel)
    m2 = re.search(r'/\* ==== END header[^\n]*\*/', skel)
    if not m1 or not m2:
        sys.exit("骨架 %s 里找不到 BEGIN/END 标记" % SKEL)
    out = skel[:m1.start()] + m1.group(0) + "\n" + statics + "\n" + func + "\n" + skel[m2.start():]

    sys.stderr.write("已抽取: 状态变量 %d 行, 校验+解码函数 %d 行\n"
                     % (statics.count("\n"), func.count("\n")))
    sys.stdout.write(out)


if __name__ == "__main__":
    main()
