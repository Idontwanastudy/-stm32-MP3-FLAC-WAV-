/*
 * mp3_host_test.c - 在 PC 上跑「工程里那段 MP3 解码逻辑」，与 ffmpeg 参考解码逐样本对比
 *
 * 做法(关键: 不手抄代码, 保证与固件一致):
 *   tools/make_mp3_host_test.py 会把 Core/Src/Music_Driver.c 里的
 *   `static int mp3_decode_next_frame(void)` **原样按行切出来**, 连同它依赖的静态变量一起
 *   拼进本文件; 这里再补一个"读真实文件"的假 FatFs 和 main。
 *
 * 编译运行(在工程根目录):
 *   python3 tools/make_mp3_host_test.py > /tmp/mp3_host_test.c
 *   gcc -O2 -I Core/Inc -I Middlewares/libhelix-mp3 -I Middlewares/libhelix-mp3/utils \
 *       -o /tmp/mp3_test /tmp/mp3_host_test.c Middlewares/libhelix-mp3/*.c \
 *       Middlewares/libhelix-mp3/utils/*.c
 *   /tmp/mp3_test /tmp/test.mp3 /tmp/mirror.pcm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mp3dec.h"
#include "helix_memory.h"   /* 固件里 Helix 的状态是从外部内存池分配的 */

/* ---------- 假 FatFs: 把 /tmp/xxx.mp3 当"SD 卡上的文件" ---------- */
typedef unsigned int UINT;     /* FatFs 的 UINT */
#define FR_OK 0                /* FatFs 的 OK */
#define FRESULT int
typedef struct { FILE *fp; long size; } FIL_stub;
static FIL_stub SDFile;

static int f_read(void *f, void *buf, unsigned len, unsigned *br)
{
    FIL_stub *s = (FIL_stub *)f;
    size_t n = fread(buf, 1, len, s->fp);
    *br = (unsigned)n;
    return (n || len == 0) ? 0 : 1;     /* FR_OK=0 */
}
static long f_size(void *f) { return ((FIL_stub *)f)->size; }

/* 下面这段是从 Music_Driver.c 原样切出来的 MP3 解码逻辑 */
/* ==== BEGIN mp3_decode_next_frame (verbatim) ==== */
#define MP3_INBUF_SIZE 4096
static HMP3Decoder mp3_hdec = 0;
static uint8_t  mp3_inbuf[MP3_INBUF_SIZE];
static uint32_t mp3_inbuf_len = 0;
static uint32_t mp3_inbuf_pos = 0;
static uint8_t  mp3_eof = 0;
static int16_t  mp3_pcm[2304 + 256];
static uint32_t mp3_channels = 2;
static uint32_t mp3_hz = 0;
static uint32_t mp3_total_frames = 0;
static uint32_t mp3_frame_start = 0;
static uint32_t mp3_frame_frames = 0;
/* ==== END header ==== */

int main(int argc, char **argv)
{
    const char *in = (argc > 1) ? argv[1] : "/tmp/test.mp3";
    const char *out = (argc > 2) ? argv[2] : "/tmp/mirror.pcm";
    FILE *fo;

    SDFile.fp = fopen(in, "rb");
    if (!SDFile.fp) { perror("打开 mp3"); return 1; }
    fseek(SDFile.fp, 0, SEEK_END); SDFile.size = ftell(SDFile.fp); fseek(SDFile.fp, 0, SEEK_SET);

    /* 与固件一样: 跳过 ID3v2(这里交给 mp3_decode_next_frame 的同步字搜索兜底) */
    /* 与固件一致: 先给 Helix 一块内存池(固件里放在 work_buf 的 CCM union 里) */
    static unsigned char pool[32 * 1024];
    helix_pool_init(pool, sizeof(pool));
    mp3_hdec = MP3InitDecoder();
    if (!mp3_hdec) { printf("MP3InitDecoder 失败\n"); return 1; }

    fo = fopen(out, "wb");
    int frames = 0; long total = 0;
    for (;;) {
        int n = mp3_decode_next_frame();          /* ← 固件里的那个函数 */
        if (n < 0) break;
        frames++;
        fwrite(mp3_pcm, sizeof(int16_t), (size_t)(n * mp3_channels), fo);
        total += n;
        if (frames == 1) printf("首帧: %d 样本/声道, %u 声道, %u Hz\n", n, mp3_channels, mp3_hz);
    }
    fclose(fo);
    printf("共解码 %d 帧, %ld 样本/声道, 结束方式=%s, 采样率=%u\n",
           frames, total, mp3_eof ? "文件尾" : "解码错误", mp3_hz);
    if (frames == 0) { printf("!! 一帧都没解出来\n"); return 2; }
    return 0;
}
