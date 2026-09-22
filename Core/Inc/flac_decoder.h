/*
 * flac_decoder.h - 精简 FLAC 解码器 (针对 STM32F103 内存受限)
 *
 * 仅支持 FLAC subset (16/24bit, 单/双声道, block size <= FLAC_MAX_BLOCK)。
 * 不支持: 变长 block 超过 FLAC_MAX_BLOCK 的帧、8 声道、非 PCM。
 * 解码输出: 有符号 32bit PCM, 每帧 block 个样本/声道, 顺序解码不可回退。
 */
#ifndef __FLAC_DECODER_H
#define __FLAC_DECODER_H

#include <stdint.h>
#include "ff.h"
#include "Music_Driver.h"   /* 获取 ENABLE_FLAC 开关 (0=禁用, FLAC 编译为空) */

#if ENABLE_FLAC

/* 受 RAM 约束的限制 */
#ifndef FLAC_MAX_BLOCK
#define FLAC_MAX_BLOCK       4608    /* 每帧每声道最大样本数 (F407 192KB RAM; 覆盖 ffmpeg/libFLAC 默认 4608) */
#endif
#ifndef FLAC_MAX_CHANNELS
#define FLAC_MAX_CHANNELS    2
#endif
#ifndef FLAC_IN_BUF_SIZE
#define FLAC_IN_BUF_SIZE     6144    /* 压缩数据输入缓冲 (4096→6144: 每 block 读卡次数 5→3, 减少同步读耗时, 降低 DMA 欠载风险) */
#endif

typedef struct {
    /* 文件流 */
    FIL *file;

    /* STREAMINFO */
    uint32_t sampleRate;
    uint8_t  channels;
    uint8_t  bitsPerSample;
    uint64_t totalPCMFrames;    /* 每声道总帧数 */
    uint16_t maxBlockSize;

    /* bit 输入流 */
    uint8_t  inbuf[FLAC_IN_BUF_SIZE];
    uint32_t inbuf_len;
    uint32_t inbuf_pos;
    uint32_t bitbuf;     /* 32位位缓冲 (优化: 一次填4字节) */
    int      bits_left;  /* bitbuf 有效位数 0-32 */

    /* 当前帧解码输出。
     * 声道0 -> pcm[0 .. frameFrames-1]
     * 声道1 -> pcm[FLAC_MAX_BLOCK .. FLAC_MAX_BLOCK+frameFrames-1] (仅双声道)
     */
    int32_t  pcm[FLAC_MAX_BLOCK * FLAC_MAX_CHANNELS];
    uint32_t frameFrames;       /* 当前帧帧数 */
    uint64_t frameStart;        /* 当前帧的全局起始样本索引 */
    uint64_t decoded_total;     /* 已解码总帧数 */
    uint32_t curBlockSize;
} flac_decoder;

/* 解析 STREAMINFO 并定位到第一个音频帧。
 * 返回 0 成功; -1 解析失败/不支持格式; -2 文件 block size 超过 FLAC_MAX_BLOCK。 */
int flac_decoder_open(flac_decoder *d, FIL *file);

/* 解码下一帧到 d->pcm。
 * 返回 0 成功(帧有效), -1 到达文件尾/解码失败。 */
int flac_decoder_decode_frame(flac_decoder *d);

#endif /* ENABLE_FLAC */
#endif /* __FLAC_DECODER_H */
