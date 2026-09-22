/*
 * flac_decoder.c - 精简 FLAC 解码器实现
 *
 * 支持 FLAC subset: 16/24bit PCM, 单/双声道, constant/verbatim/fixed/LPC 子帧,
 * partitioned Rice residual (含 escape), 立体声去相关 (independent/left/right/mid-side)。
 * block size 受 FLAC_MAX_BLOCK(2048) 限制。
 *
 * 参考: FLAC 格式规范 + dr_flac (David Reid) 的解码逻辑。
 */
#include "flac_decoder.h"
#include <string.h>

#if ENABLE_FLAC

/* ---------- bit 输入流 ---------- */
static int flac_fill(flac_decoder *d)
{
    UINT br;
    if (d->inbuf_pos >= d->inbuf_len) {
        if (f_read(d->file, d->inbuf, FLAC_IN_BUF_SIZE, &br) != FR_OK) return -1;
        d->inbuf_len = br;
        d->inbuf_pos = 0;
        if (br == 0) return -1;   /* EOF */
    }
    return 0;
}

/* 位缓冲补位: bits_left<=8 时一次补 24 位(3字节), 否则补 8 位 (EOF 填 0)。
 * 注意 bitbuf 是 uint32, 补位不能超过 32, 故 bits_left<=8 才可补 24。 */
static inline void flac_refill(flac_decoder *d)
{
    if (d->inbuf_pos >= d->inbuf_len) {
        if (flac_fill(d)) {
            d->bitbuf <<= 8;   /* EOF: 填充 0 */
        } else {
            d->bitbuf = (d->bitbuf << 8) | d->inbuf[d->inbuf_pos++];
        }
    } else {
        d->bitbuf = (d->bitbuf << 8) | d->inbuf[d->inbuf_pos++];
    }
    d->bits_left += 8;
}

/* 读 n 位 (MSB first), n<=25; EOF 时填充 0。
 * 32 位位缓冲: 一次填满 4 字节, 显著减少 flac_fill 调用与逐字节移位开销。 */
static inline uint32_t flac_bits(flac_decoder *d, int n)
{
    while (d->bits_left < n) flac_refill(d);
    d->bits_left -= n;
    if (n >= 32) return d->bitbuf;   /* 保险, 实际 n<=25 */
    return (d->bitbuf >> d->bits_left) & ((1u << n) - 1u);
}

/* 读 n 位有符号补码 (符号扩展) */
static int32_t flac_s32(flac_decoder *d, int n)
{
    uint32_t v = flac_bits(d, n);
    if (n > 0 && n < 32 && (v & (1u << (n - 1)))) {
        v |= ~((1u << n) - 1);
    }
    return (int32_t)v;
}

/* ---------- 预测 ---------- */
/* 基于 buf[idx-1], buf[idx-2], ... 预测, 结果右移 shift。
 * 用 64 位累积: 24bit 源时 (bitsPerSample+precision+order)>32 会 int32 溢出。 */
static int32_t flac_prediction(uint32_t order, uint32_t shift, const int32_t *coeff, const int32_t *buf, uint32_t idx)
{
    int64_t pred = 0;
    uint32_t j;
    for (j = 0; j < order; j++) {
        pred += (int64_t)coeff[j] * buf[idx - 1 - j];
    }
    return (int32_t)(pred >> shift);
}

/* ---------- Rice 解码 ----------
 * unary 直接在位缓冲上数连续 0 (避免逐位 flac_bits 调用开销) */
static inline int32_t flac_rice(flac_decoder *d, uint32_t riceParam)
{
    uint32_t zero = 0;
    for (;;) {
        if (d->bits_left == 0) flac_refill(d);
        if ((d->bitbuf >> (d->bits_left - 1)) & 1u) break;
        d->bits_left--;
        zero++;
    }
    d->bits_left--;   /* 消费 unary 的停止位 1 */
    uint32_t val = (zero << riceParam) | (riceParam ? flac_bits(d, (int)riceParam) : 0);
    /* 折半有符号还原: 偶数 v -> v/2, 奇数 v -> -(v+1)/2 */
    return (int32_t)((val >> 1) ^ -(int32_t)(val & 1));
}

/* ---------- residual (partitioned Rice) ---------- */
/* 解码残差并立即做预测重建, 结果写到 out[order .. blockSize-1] (绝对索引) */
static int flac_residual(flac_decoder *d, int32_t *out, uint32_t blockSize, uint32_t order,
                         uint32_t lpcShift, const int32_t *coeff)
{
    uint32_t method = flac_bits(d, 2);
    uint32_t partitionOrder, partitions, samplesInPart, p, i;
    uint32_t base = order;   /* 绝对写入位置 */

    if (method > 1) return -1;
    partitionOrder = flac_bits(d, 4);
    if (partitionOrder > 8) return -1;
    if ((blockSize >> partitionOrder) < order) return -1;

    partitions = 1u << partitionOrder;
    samplesInPart = (blockSize >> partitionOrder) - order;

    for (p = 0; p < partitions; p++) {
        uint32_t riceParam;
        if (method == 0) {
            riceParam = flac_bits(d, 4);
            if (riceParam == 15) riceParam = 0xFF;
        } else {
            riceParam = flac_bits(d, 5);
            if (riceParam == 31) riceParam = 0xFF;
        }

        if (riceParam == 0xFF) {
            /* escape: 未编码原始样本 */
            uint32_t eb = flac_bits(d, 5);
            for (i = 0; i < samplesInPart; i++) {
                out[base + i] = flac_s32(d, (int)eb) + flac_prediction(order, lpcShift, coeff, out, base + i);
            }
        } else {
            for (i = 0; i < samplesInPart; i++) {
                out[base + i] = flac_rice(d, riceParam) + flac_prediction(order, lpcShift, coeff, out, base + i);
            }
        }

        base += samplesInPart;
        if (partitionOrder != 0) samplesInPart = blockSize >> partitionOrder;
    }
    return 0;
}

/* ---------- 子帧 ---------- */
static int flac_subframe(flac_decoder *d, int32_t *out, uint32_t blockSize, uint32_t bitsPerSample)
{
    static const int32_t fixedCoeff[5][4] = {
        {0, 0, 0, 0}, {1, 0, 0, 0}, {2, -1, 0, 0}, {3, -3, 1, 0}, {4, -6, 4, -1}
    };
    uint32_t type, wastedFlag, wastedBits = 0, sbps, i;

    flac_bits(d, 1);            /* zero padding */
    type = flac_bits(d, 6);
    wastedFlag = flac_bits(d, 1);
    if (wastedFlag) {
        uint32_t k = 0;
        while (flac_bits(d, 1) == 0) k++;
        wastedBits = k + 1;
    }
    if (wastedBits >= bitsPerSample) return -1;
    sbps = bitsPerSample - wastedBits;

    if (type == 0) {
        /* constant */
        int32_t v = flac_s32(d, (int)sbps);
        for (i = 0; i < blockSize; i++) out[i] = v;
    } else if (type == 1) {
        /* verbatim */
        for (i = 0; i < blockSize; i++) out[i] = flac_s32(d, (int)sbps);
    } else if (type >= 8 && type <= 12) {
        /* fixed predictor */
        uint32_t order = type - 8;
        for (i = 0; i < order && i < blockSize; i++) out[i] = flac_s32(d, (int)sbps);
        if (flac_residual(d, out, blockSize, order, 0, fixedCoeff[order])) return -1;
    } else if (type >= 32) {
        /* LPC: 字段顺序为 warmup -> precision(4bit,+1,15无效) -> shift(5bit) -> coefficients -> residual */
        uint32_t order = (type & 0x1F) + 1;
        uint32_t precision, lpcShift;
        int32_t coeff[32];
        if (order > 32 || order > blockSize) return -1;
        for (i = 0; i < order; i++) out[i] = flac_s32(d, (int)sbps);   /* warmup */
        precision = flac_bits(d, 4);
        if (precision == 15) return -1;
        precision += 1;
        lpcShift = flac_bits(d, 5);
        for (i = 0; i < order; i++) coeff[i] = flac_s32(d, (int)precision);
        if (flac_residual(d, out, blockSize, order, lpcShift, coeff)) return -1;
    } else {
        return -1;   /* 保留类型 */
    }

    /* 应用 wasted bits */
    if (wastedBits) {
        for (i = 0; i < blockSize; i++) out[i] <<= wastedBits;
    }
    return 0;
}

/* ---------- UTF-8 coded number (跳过) ---------- */
static void flac_skip_utf8(flac_decoder *d)
{
    uint32_t b0 = flac_bits(d, 8);
    uint32_t n = 0;
    if ((b0 & 0x80) == 0) return;
    if ((b0 & 0xE0) == 0xC0) n = 1;
    else if ((b0 & 0xF0) == 0xE0) n = 2;
    else if ((b0 & 0xF8) == 0xF0) n = 3;
    else if ((b0 & 0xFC) == 0xF8) n = 4;
    else if ((b0 & 0xFE) == 0xFC) n = 5;
    else if (b0 == 0xFE) n = 6;
    while (n--) flac_bits(d, 8);
}

/* ---------- 帧头 ---------- */
static int flac_frame_header(flac_decoder *d, uint32_t *blockSizeOut, uint32_t *bitsPerSampleOut,
                             uint32_t *channelsOut, uint32_t *chAssignOut)
{
    static const uint32_t sampleRateTable[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    static const uint8_t  bitsPerSampleTable[8] = {0, 8, 12, 0xFF, 16, 20, 24, 0xFF};
    uint32_t sync, reserved, bs, sr, ch, ss, blockSize, bitsPerSample;

    sync = flac_bits(d, 14);
    if (sync != 0x3FFE) return -1;
    reserved = flac_bits(d, 1);
    if (reserved) return -1;
    flac_bits(d, 1);            /* blocking strategy, 忽略 */
    bs = flac_bits(d, 4);
    if (bs == 0) return -1;
    sr = flac_bits(d, 4);
    ch = flac_bits(d, 4);
    if (ch > 10) return -1;
    ss = flac_bits(d, 3);
    if (ss == 3 || ss == 7) return -1;
    reserved = flac_bits(d, 1);
    if (reserved) return -1;

    flac_skip_utf8(d);

    if (bs == 1) blockSize = 192;
    else if (bs <= 5) blockSize = 576 * (1u << (bs - 2));
    else if (bs == 6) blockSize = flac_bits(d, 8) + 1;
    else if (bs == 7) {
        blockSize = flac_bits(d, 16) + 1;
        if (blockSize == 0x10000) return -1;
    } else {
        blockSize = 256 * (1u << (bs - 8));
    }

    if (sr <= 11) {
        /* 采样率在 STREAMINFO 中已确定, 这里不强制, 直接丢弃 */
    } else if (sr == 12) flac_bits(d, 8);
    else if (sr == 13 || sr == 14) flac_bits(d, 16);
    else return -1;

    bitsPerSample = bitsPerSampleTable[ss];
    if (bitsPerSample == 0xFF) return -1;
    if (bitsPerSample == 0) bitsPerSample = d->bitsPerSample;   /* 从 STREAMINFO */

    flac_bits(d, 8);            /* CRC-8, 跳过校验 */

    *blockSizeOut = blockSize;
    *bitsPerSampleOut = bitsPerSample;
    if (ch == 0) { *channelsOut = 1; *chAssignOut = 0; }
    else if (ch <= 7) { *channelsOut = ch + 1; *chAssignOut = 0; }   /* 多声道 independent */
    else { *channelsOut = 2; *chAssignOut = ch; }                    /* 8=left-side 9=right-side 10=mid-side */
    return 0;
}

/* ---------- 解码一帧 ---------- */
int flac_decoder_decode_frame(flac_decoder *d)
{
    uint32_t blockSize, bitsPerSample, channels, chAssign;
    uint32_t subframeBits[2];
    uint32_t i;
    int32_t *ch0 = d->pcm;
    int32_t *ch1 = d->pcm + FLAC_MAX_BLOCK;

    if (flac_frame_header(d, &blockSize, &bitsPerSample, &channels, &chAssign)) return -1;
    if (blockSize == 0 || blockSize > FLAC_MAX_BLOCK) return -1;
    if (channels > FLAC_MAX_CHANNELS) return -1;

    subframeBits[0] = bitsPerSample;
    subframeBits[1] = bitsPerSample;
    if (chAssign == 8 || chAssign == 10) subframeBits[1] += 1;   /* left-side/mid-side: 子帧1=side */
    if (chAssign == 9) subframeBits[0] += 1;                     /* right-side: 子帧0=side */

    if (flac_subframe(d, ch0, blockSize, subframeBits[0])) return -1;
    if (channels == 2) {
        if (flac_subframe(d, ch1, blockSize, subframeBits[1])) return -1;
    }

    /* 声道去相关 */
    if (channels == 2 && chAssign != 0) {
        for (i = 0; i < blockSize; i++) {
            int32_t a = ch0[i], b = ch1[i];
            if (chAssign == 8) {                 /* left-side: L, L-R */
                ch0[i] = a;
                ch1[i] = a - b;
            } else if (chAssign == 9) {          /* right-side: R-L, R */
                ch0[i] = a + b;
                ch1[i] = b;
            } else if (chAssign == 10) {         /* mid-side */
                uint32_t mid = (uint32_t)a, side = (uint32_t)b;
                mid = (mid << 1) | (side & 1);
                ch0[i] = (int32_t)(((int32_t)(mid + side)) >> 1);
                ch1[i] = (int32_t)(((int32_t)(mid - side)) >> 1);
            }
        }
    }

    d->curBlockSize = blockSize;
    d->frameFrames = blockSize;
    d->frameStart = d->decoded_total;   /* 当前帧起始 = 已解码总帧数 */
    d->decoded_total += blockSize;
    flac_bits(d, 16);           /* CRC-16, 跳过校验 */
    d->bits_left = 0;           /* FLAC 帧字节对齐, 丢弃当前字节剩余位 */
    return 0;
}

/* ---------- 打开: 解析 STREAMINFO ---------- */
int flac_decoder_open(flac_decoder *d, FIL *file)
{
    uint8_t magic[4], hdr[4], si[34];
    UINT br;
    int last = 0;

    memset(d, 0, sizeof(*d));
    d->file = file;

    if (f_read(file, magic, 4, &br) != FR_OK || br != 4) return -1;
    if (memcmp(magic, "fLaC", 4) != 0) return -1;

    while (!last) {
        if (f_read(file, hdr, 4, &br) != FR_OK || br != 4) return -1;
        last = hdr[0] & 0x80;
        if ((hdr[0] & 0x7F) == 0) {
            /* STREAMINFO */
            uint32_t len = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
            if (len < 34) return -1;
            if (f_read(file, si, 34, &br) != FR_OK || br != 34) return -1;
            d->maxBlockSize = ((uint16_t)si[2] << 8) | si[3];
            d->sampleRate = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | (si[12] >> 4);
            d->channels = (uint8_t)(((si[12] >> 1) & 0x07) + 1);
            d->bitsPerSample = (uint8_t)((((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1);
            d->totalPCMFrames = ((uint64_t)(si[13] & 0x0F) << 32) |
                                ((uint64_t)si[14] << 24) | ((uint64_t)si[15] << 16) |
                                ((uint64_t)si[16] << 8) | si[17];
            if (d->maxBlockSize > FLAC_MAX_BLOCK) return -2;   /* 超出支持 */
            if (d->channels > FLAC_MAX_CHANNELS) return -1;
            if (len > 34) {
                if (f_lseek(file, f_tell(file) + (len - 34)) != FR_OK) return -1;
            }
        } else {
            uint32_t len = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
            if (f_lseek(file, f_tell(file) + len) != FR_OK) return -1;
        }
    }
    return 0;
}

#endif /* ENABLE_FLAC */
