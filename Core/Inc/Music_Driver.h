/*
 * Music_Driver.h
 *
 *  Created on: Mar 7, 2026
 *      Author: 15754
 */

#ifndef INC_MUSIC_DRIVER_H_
#define INC_MUSIC_DRIVER_H_

#include "fatfs.h"
#include "main.h"

/* FLAC 支持开关: 1=启用 (F407 192KB RAM 足够, 支持 block<=4608 的 FLAC)。
 * 0=禁用(F103 内存受限时的旧设置)。 */
#define ENABLE_FLAC 1

/* MP3 支持开关: 1=启用 (Helix 定点解码器) */
#define ENABLE_MP3 1

/* ★MP3 填充耗时诊断(临时脚手架): 1=开启。
 * 用 DWT CYCCNT 量"一行缓冲的填充耗时", 与"该行的播放时长"作比:
 *   比 < 1 → 填充跟得上;  比 > 1 → 填充跑不完播放窗口
 *   → 环形 I2S DMA 会绕回去重复旧数据 → 听感"变慢"而音高不变。
 * 开启后 MP3 播放界面显示: FILL(本次填充ms/该行播放ms)、R(比值)、PK(本秒最差比值)。
 * 定性完把这里改成 0 即可 —— 相关代码全部被这个宏包住, 不留残渣。 */
#define MP3_FILL_DIAG 1

#define max_size 100        /* 文件列表上限 (禁用FLAC后恢复为100) */
#define max_length 120
/* I2S DMA 缓冲：半字数组。
 * HAL 在 24bit/32bit 模式下会把 DMA 传输大小翻倍(Size<<1)，
 * 因此传 AUDIO_BUF_SIZE 实际会搬 2*AUDIO_BUF_SIZE 个半字。
 * 我们用 2 行、每行 AUDIO_BUF_SIZE 半字：行0=前半区,行1=后半区,正好被 2 个半传输回调覆盖。
 */
#define AUDIO_BUF_SIZE 8192           /* 半字长度（每行）= 2304 帧。对齐到 FLAC block 4608 的因数(2304=4608/2):
                                         每个 block 正好供给 2 行, 消除"连续跨块行"的 2×解码尖峰叠加。
                                         窗口 ~48ms@48k, 覆盖 21ms 解码+读卡突发 */
#define BUFFER_HALF_SIZE (AUDIO_BUF_SIZE/2)  /* 保留旧宏，不再使用 */
#define NUM_BUFFERS 2

typedef enum {
    STATE_PLAYING,   // 正常播放
    STATE_PAUSED,    // 暂停
    STATE_STOPPED    // 停止（文件结束或手动停止）
} PlayState;

typedef enum {
    CMD_NONE,
	CMD_PAUSE,
	CMD_RESUME,
	CMD_STOP,
	CMD_PREV,
	CMD_NEXT,
	CMD_ERROR
} Mode_Keyword;

typedef struct
{
	char audio_file_names[max_length];
	char audio_file_type[6];
}file_list;

typedef struct
{
	uint8_t  riff[4];         // "RIFF"
	uint32_t fileSize;        // 文件总长度-8
	uint8_t  wave[4];         // "WAVE"
	    // fmt子块
	uint8_t  fmt[4];          // "fmt "
	uint32_t fmtLen;          // fmt块长度（一般为16）
	uint16_t audioFormat;     // 音频格式（1表示PCM）
	uint16_t numChannels;     // 声道数
	uint32_t sampleRate;      // 采样率
	uint32_t byteRate;        // 字节率 = sampleRate * numChannels * bitsPerSample/8
	uint16_t blockAlign;      // 块对齐 = numChannels * bitsPerSample/8
	uint16_t bitsPerSample;   // 位深度
	    // data子块
	uint8_t  data[4];         // "data"
	uint32_t dataSize;		  // 音频数据长度（字节）
}wav_file_parameters;

extern file_list audiofiles[max_size];
extern uint16_t file_count;
extern uint16_t current_index;
extern volatile uint8_t half_ready;
extern volatile uint8_t full_ready;

/* MP3 填充耗时诊断(见上面 MP3_FILL_DIAG): 显示放在滚动定时器任务里做,
 * 音频任务只写这几个变量, 绝不在播放任务里做 I2C。 */
#if MP3_FILL_DIAG && ENABLE_MP3
extern volatile uint8_t mp3_diag_active;    /* 1=MP3 正在播放 */
void mp3_diag_show(void);                   /* 由定时器任务每秒调用一次 */
#endif

//extern FATFS SDFatFS;    /* 文件系统对象 */
//extern FIL SDFile;       /* 文件对象 */
//extern DIR SDDir;        /* 目录对象 */
//extern FILINFO SDFileInfo; /* 文件信息对象 */
//extern FRESULT res;
//extern UINT bytes_read;
//extern uint16_t audio_buffer[AUDIO_BUF_SIZE];
extern  __attribute__((aligned(4))) uint16_t audio_buffer[NUM_BUFFERS][AUDIO_BUF_SIZE];
volatile extern Mode_Keyword play_cmd;
volatile extern PlayState play_state;
extern volatile int seek_speed;   /* 0=正常, >0 快进倍速(2/3/4), <0 快退倍速(-2/-3/-4) */
extern volatile uint8_t current_audio_type;   /* 1=wav 2=mp3 3=flac */
extern volatile uint8_t  key_prev_down;   /* PE0 上一首 按下状态 */
extern volatile uint8_t  key_next_down;   /* PE1 下一首 按下状态 */
extern volatile uint32_t key_prev_tick;   /* PE0 按下时刻 */
extern volatile uint32_t key_next_tick;   /* PE1 按下时刻 */


uint8_t check_extension(const char *name, const char *ext);
uint8_t audio_file_load(void);
uint8_t audio_file_read(file_list *ado_file);
void wav_file_process(file_list *ado_file);
void start_playing(uint32_t sample_rate);
void fill_buffer_region(uint16_t *start, uint16_t num_halfwords);
int parse_wav_header(FIL *file, wav_file_parameters *header);
uint32_t i2s_set_sample_rate(uint32_t sample_rate);
void mp3_file_process(file_list *ado_file);
void flac_file_process(file_list *ado_file);
void audio_pause(void);
void audio_resume(void);
void audio_stop(void);

#endif /* INC_MUSIC_DRIVER_H_ */
