/*
 * Music_Driver.c
 *
 *  Created on: Mar 7, 2026
 *      Author: 15754
 */

#include "fatfs.h"
#include <stdio.h>
#include <string.h>
#include "Music_Driver.h"
#include "stm32f1xx_it.h"
#include "stm32f1xx_hal.h"
#include "stm32f1xx_hal_i2s.h"
#include "i2s.h"
#include "OLED.h"
#include "stm32f103xe.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "queue.h"
//#include "stm32f1xx_hal.h"



/* 外部变量声明，FATFS相关对象通常在fatfs.c中定义 */
extern FATFS SDFatFS;    /* 文件系统对象 */
extern FIL SDFile;       /* 文件对象 */
DIR SDDir;        /* 目录对象 */
FILINFO SDFileInfo; /* 文件信息对象 */
FRESULT res;
UINT bytes_read;

extern osMessageQueueId_t audioQueue;
extern osSemaphoreId_t sem_dma_done;
extern volatile uint8_t play_enabled;

/* 缓存区，用于存放从文件读出的数据 */
/* 两行: 行0(前半区) 和 行1(后半区)，配合 HAL 24bit/32bit 模式 DMA 长度翻倍语义 */
__attribute__((aligned(4))) uint16_t audio_buffer[NUM_BUFFERS][AUDIO_BUF_SIZE];
//static uint8_t raw_buf[8192];
volatile uint8_t half_ready = 0;        // 标志：是否需要填充下半区
volatile uint8_t full_ready = 0;        // 标志：是否需要填充上半区
volatile PlayState play_state = STATE_STOPPED;
volatile Mode_Keyword play_cmd = CMD_NONE;
file_list audiofiles[max_size];
static uint8_t prod_idx = 0;
uint16_t file_count = 0;
uint16_t current_index = 0;
int i;
uint8_t file_ended = 0;

/* raw_buf: 源数据读取中转缓冲区。
 * 重采样单行最多读约 (AUDIO_BUF_SIZE/2) 个输出帧对应的源帧。
 * 最坏情况 48k->46.875k 时 step≈1.024，每行 1024 输出帧 ≈ 1049 源帧 × 6B ≈ 6.3KB。
 * 取 AUDIO_BUF_SIZE*4 = 8192B，足够覆盖最坏情况。
 * 用 __attribute__((aligned(4))) 保证 4 字节对齐，便于 DMA/FATFS 访问。
 */
static uint8_t raw_buf[AUDIO_BUF_SIZE * 4] __attribute__((aligned(4)));

/* ============ 软件重采样状态 ============
 * 硬件 I2S 时钟是离散档位(如 46.875kHz)，而源文件是 44.1k/48k，
 * 通过线性插值把源样本流映射到输出时钟，恢复正确音高和时长。
 * 源 24bit 有符号 -> 内部用 int32 保存；输出 I2S 32bit 左对齐。
 * 用 f_lseek 绝对定位读取源帧，相位 resample_pos 跨缓冲区自然连续。
 */
#define RESAMPLE_FRAC_BITS 16
#define RESAMPLE_FRAC_MASK 0xFFFFu
static uint32_t resample_step;      /* 定点步进: (src_rate << 16) / out_rate */
static uint64_t resample_pos;       /* 定点位置: 高16位帧索引, 低16位分数 (64位防长歌溢出) */

/* WAV 源数据区信息，供重采样按绝对偏移定位读取 */
static uint32_t wav_data_start;       /* WAV data 区起始字节偏移 (通常 44) */
static uint32_t wav_total_frames;     /* 总立体声帧数 = dataSize / bytes_per_frame */
static uint32_t wav_bytes_per_frame;  /* 每帧字节数 = numChannels * bitsPerSample/8 */
static uint8_t  wav_stereo;           /* 1=立体声, 0=单声道 */
static uint32_t out_frame_count;      /* 已输出的立体声帧数 (全局计数, 用于淡出) */
static uint32_t total_out_frames;     /* 总输出帧数估算 = wav_total_frames / step */

/* 淡出参数: 最后 FADE_SAMPLES 个输出样本线性衰减到 0，消除结尾爆音 */
#define FADE_SAMPLES 256


void force_i2s_config(void)
{
    // 可以只保留使能 I2S 的步骤，因为 CubeMX 已经完成了基本初始化
    __HAL_I2S_ENABLE(&hi2s2);

    // 验证 I2S 是否使能成功 (可选)
    if (!(SPI2->I2SCFGR & SPI_I2SCFGR_I2SE)) {
        while(1); // 死循环，表示使能失败
    }
}

/**
  * @brief 动态设置 I2S2 采样率分频（I2SPR）并返回实际输出采样率
  *        时钟源 SYSCLK=72MHz, 24bit/32bit 帧长 64bit(I2S标准, CHLEN=1)
  *        STM32F1 I2S 正确分频公式 (RM0008):
  *            fs = I2S_CK / (4 * (2*I2SDIV + ODD) * frame_bits)
  *        在给定目标采样率下，遍历 I2SDIV=2..255、ODD=0/1，
  *        选使实际采样率最接近目标值的组合。
  *        注：F103 无小数 PLL，44.1kHz 最接近为 DIV3/ODD0 -> 46.875kHz (+6.3%)，
  *            这是 CubeMX/HAL 的标准选择。
  * @param  sample_rate 目标采样率(Hz)，来自 WAV 头
  * @retval 实际输出采样率(Hz)，供重采样器计算插值步进
  */
uint32_t i2s_set_sample_rate(uint32_t sample_rate)
{
    const uint32_t i2sclk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_I2S2); /* 72MHz */
    const uint32_t frame_bits = 64;   /* 32bit * 2 (Philips) */
    uint32_t best_div = 3;
    uint32_t best_odd = 0;
    uint32_t best_actual = 46875;
    int32_t  best_err = INT32_MAX;
    uint32_t div, odd;

    if (sample_rate == 0) sample_rate = 44100;

    for (div = 2; div <= 255; div++) {
        for (odd = 0; odd <= 1; odd++) {
            /* 实际采样率 (正确 F1 公式) */
            uint32_t actual = i2sclk / (4 * (2 * div + odd)) / frame_bits;
            if (actual == 0) continue;
            int32_t err = (int32_t)actual - (int32_t)sample_rate;
            if (err < 0) err = -err;
            if (err < best_err) {
                best_err = err;
                best_div = div;
                best_odd = odd;
                best_actual = actual;
            }
        }
    }

    SPI2->I2SPR = (best_div << 0) | (best_odd << 8) | SPI_I2SPR_MCKOE;
    return best_actual;
}

/**
  * @brief 检查文件名是否以指定的后缀结尾
  * @param name: 文件名
  * @param ext: 后缀 (如 "WAV")
  * @retval 1: 是, 0: 否
  */
uint8_t check_extension(const char *name, const char *ext) {
    char *p = strrchr((char*)name, '.');
    if (p == NULL) return 0;
    return (strcasecmp(p + 1, ext) == 0) ? 1 : 0;
}
/**
  * @brief 扫描并读取SD卡中的音频文件示例
  * @param 无
  * @retval 0: 成功, 其他: 错误码
  */
uint8_t audio_file_load(void) {
    /* 1. 挂载文件系统 (如果尚未挂载) */
    res = f_mount(&SDFatFS, SDPath, 1);
    if (res != FR_OK) {
    	OLED_ClearBuffer();
    	char strerror[33]={0};
    	sprintf(strerror, "Mount SD Card Failed! Error:%d", (int)res);
		OLED_DrawStringToBuffer(strerror);
    	for(i=0; i<40*8 ;i++)
    	{
    		OLED_RefreshScreenWithScroll();
    		scrollOffset++;
    		//printf("Mount SD Card Failed! Error: %d\r\n", res);
    	}
    	osSemaphoreRelease(LOAD_OR_NOTHandle);
    	return 1;
    }

    /* 2. 打开根目录 */
    res = f_opendir(&SDDir, SDPath);
    if (res != FR_OK) {
    	OLED_ClearBuffer();
    	char strerror[38]={0};
	    sprintf(strerror, "Open root directory failed! Error:%d", (int)res);
	    OLED_DrawStringToBuffer(strerror);
    	for(i=0; i<42*8 ;i++)
    	{
    	    OLED_RefreshScreenWithScroll();
    	    scrollOffset++;
    	    //printf("Open root directory failed! Error: %d\r\n", res);
    	}
    	osSemaphoreRelease(LOAD_OR_NOTHandle);
        return 2;
    }

    //printf("Scanning audio files...\r\n");

    /* 3. 循环读取目录项 */
    while (1) {
        res = f_readdir(&SDDir, &SDFileInfo);
        /* 读取失败或没有更多文件时退出 */
        if (SDFileInfo.fname[0] == 0) {
            break;
        }
        if (res != FR_OK) {
        	osSemaphoreRelease(LOAD_OR_NOTHandle);
            break;
        }

        /* 忽略子目录，只处理文件 */
        if (!(SDFileInfo.fattrib & AM_DIR)) {
            /* 检查文件扩展名 */
            if (check_extension(SDFileInfo.fname, "wav") ||
                check_extension(SDFileInfo.fname, "mp3") ||
                check_extension(SDFileInfo.fname, "flac")) {

            	if(file_count < max_size)
            	{
            		strcpy(audiofiles[file_count].audio_file_names,SDFileInfo.fname);
            		if(check_extension(SDFileInfo.fname, "wav"))
            			strcpy(audiofiles[file_count].audio_file_type,"wav");
            		else if(check_extension(SDFileInfo.fname, "mp3"))
            			strcpy(audiofiles[file_count].audio_file_type,"mp3");
            		else if(check_extension(SDFileInfo.fname, "flac"))
            		    strcpy(audiofiles[file_count].audio_file_type,"flac");
            		file_count++;
            	}
            	else if (file_count >= max_size)
            	{
            		OLED_ShowString(1, 1, "files full");
            		break;
            	}
            }
        }

    }
    if(audiofiles[0].audio_file_names[0] == '\0' && file_count == 0)
    {
    	/*OLED_Clear();
    	OLED_ShowString(1, 1, "Not audio files");*/
    	return 3;
    }
    else
    {
    	char str[16] = {0};
    	sprintf(str, "files:%d", file_count);
    	OLED_Clear();
    	OLED_ShowString(1, 1, str);
    }


    /* 7. 关闭目录 */
    f_closedir(&SDDir);
    return 0;
}

uint8_t audio_file_read(file_list *ado_file)
{
	uint8_t audio_type;
	if(strcmp(ado_file[current_index].audio_file_type,"wav")==0)
		audio_type=1;
	else if(strcmp(ado_file[current_index].audio_file_type,"mp3")==0)
		audio_type=2;
	else if(strcmp(ado_file[current_index].audio_file_type,"flac")==0)
		audio_type=3;
	OLED_ShowNum(2, 1, audio_type, 1);

	res = f_mount(&SDFatFS, SDPath, 1);
	if (res != FR_OK)
	{
	    //printf("Mount SD Card Failed! Error: %d\r\n", res);
	    OLED_ClearBuffer();
	    char strerror[33]={0};
	    sprintf(strerror, "Mount SD Card Failed! Error:%d", (int)res);
	    OLED_DrawStringToBuffer(strerror);
	    for(i=0; i<40*8 ;i++)
	    {
	        OLED_RefreshScreenWithScroll();
	        scrollOffset++;
	        osDelay(1);
	        //printf("Mount SD Card Failed! Error: %d\r\n", res);
	    }
	    osSemaphoreRelease(LOAD_OR_NOTHandle);
	    return 1;
	}

	if(audio_type == 1 && play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
	{
		OLED_ShowString(1, 1, "Stopped.");
		OLED_ShowString(2, 1, "Press key3");
		OLED_ShowString(3, 1, "to play");
		while(play_state == STATE_STOPPED && play_cmd != CMD_RESUME) {osDelay(10);}
		play_cmd = CMD_NONE;
		play_state = STATE_PLAYING;
		file_ended = 0;
		wav_file_process(ado_file);
		if(play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
			current_index= (current_index < (file_count-1)) ? current_index + 1 : 0;
		else if(play_cmd == CMD_PREV || play_cmd == CMD_NEXT)
			play_cmd = CMD_NONE;
	}
	else if(audio_type == 2 && play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
	{
		OLED_ShowString(1, 1, "Stopped.");
		OLED_ShowString(1, 1, "Press key3");
		OLED_ShowString(2, 1, "to play");
		while(play_state == STATE_STOPPED && play_cmd != CMD_RESUME) {osDelay(10);}
		play_cmd = CMD_NONE;
		play_state = STATE_PLAYING;
		file_ended = 0;
		mp3_file_process(ado_file);
		if(play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
			current_index= (current_index < (file_count-1)) ? current_index + 1 : 0;
		else if (play_cmd == CMD_PREV || play_cmd == CMD_NEXT)
			play_cmd = CMD_NONE;
	}
	else if(audio_type == 3 && play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
	{
		OLED_ShowString(1, 1, "Stopped.");
		OLED_ShowString(1, 1, "Press key3");
		OLED_ShowString(2, 1, "to play");
		while(play_state == STATE_STOPPED && play_cmd != CMD_RESUME) {osDelay(10);}
		play_cmd = CMD_NONE;
		play_state = STATE_PLAYING;
		file_ended = 0;
		flac_file_process(ado_file);
		if(play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
			current_index= (current_index < (file_count-1)) ? current_index + 1 : 0;
		else if (play_cmd == CMD_PREV || play_cmd == CMD_NEXT)
			play_cmd = CMD_NONE;
	}
	return 0;
}

void wav_file_process(file_list *ado_file)
{
	char full_path[128]={0};
	sprintf(full_path, "0:/%s", ado_file[current_index].audio_file_names);
	res = f_open(&SDFile, full_path, FA_READ);
	wav_file_parameters wave;
	int wav_flag = parse_wav_header(&SDFile, &wave);
	if(wav_flag == 0 && res == FR_OK)
	{
		/* WAV 头为 44 字节 (标准 PCM)，数据区紧随其后。
		 * 每帧字节数 = 声道数 * 位深/8 (立体声24bit = 6字节) */
		wav_data_start = sizeof(wav_file_parameters);   /* = 44 */
		wav_bytes_per_frame = wave.numChannels * (wave.bitsPerSample / 8);
		if (wav_bytes_per_frame == 0) wav_bytes_per_frame = 6;
		wav_stereo = (wave.numChannels > 1) ? 1 : 0;
		wav_total_frames = wave.dataSize / wav_bytes_per_frame;

		OLED_Clear();
		OLED_ShowString(2, 1, "WAV Info:");
		OLED_ShowNum(2, 10, wave.sampleRate, 3);
		OLED_ShowString(3, 1, "Channels:");
		OLED_ShowNum(3, 10, wave.numChannels, 3);
		OLED_ShowNum(4, 1, wave.bitsPerSample, 3);
		OLED_ShowString(4, 4, "bits");
		OLED_ShowString(1, 1, ado_file[current_index].audio_file_names);
		start_playing(wave.sampleRate);
	}
	else
	{
		char erroritem[22]={0};
		OLED_ShowString(1, 1, "Error!");
		sprintf(erroritem, "res:%d,flag:%d",(int)res,wav_flag);
		OLED_ShowString(2, 1, erroritem);
		OLED_ShowString(3, 1, "                ");
		OLED_ShowString(4, 1, "                ");
		osSemaphoreRelease(LOAD_OR_NOTHandle);
	}

	while (!file_ended && play_state != STATE_STOPPED)
	{
		if (play_cmd != CMD_NONE)
		{
		    switch (play_cmd)
		    {
		    	case CMD_PREV: case CMD_NEXT: file_ended = 1;break;
		        case CMD_PAUSE: {audio_pause(); play_cmd = CMD_NONE; OLED_Clear(); OLED_ShowString(1, 1, "Pause!"); break;}
		        case CMD_RESUME: {audio_resume(); play_cmd = CMD_NONE; OLED_Clear(); OLED_ShowString(1, 1, ado_file[current_index].audio_file_names); break;}
		        case CMD_STOP: {audio_stop(); play_cmd = CMD_NONE; OLED_Clear(); break;}
		        default: break;
		    }
		 }
		if (play_state == STATE_PLAYING)
			{
				/* DMA 循环模式时序:
				 * 半传输回调 -> DMA 刚播完 row0, 正读 row1 -> row0 空闲, 重填 row0
				 * 全传输回调 -> DMA 刚播完 row1, 回绕读 row0 -> row1 空闲, 重填 row1
				 */
				if (half_ready)
				{
					fill_buffer_region(audio_buffer[0], AUDIO_BUF_SIZE);  /* row0 已播完, 重填 row0 */
					half_ready = 0;
				}
				if (full_ready)
				{
					fill_buffer_region(audio_buffer[1], AUDIO_BUF_SIZE);  /* row1 已播完, 重填 row1 */
					full_ready = 0;
				}
				if (!half_ready && !full_ready)
				{
					/* 两个半区都没就绪：让出 CPU，等待 DMA 中断置位 */
					osThreadYield();
				}
						//OLED_ShowString(1 , 1,ado_file[current_index].audio_file_names);
			}
	}
	//audio_stop();
	HAL_I2S_DMAStop(&hi2s2);
	f_close(&SDFile);
	if(play_cmd == CMD_PREV) current_index= (current_index > 0) ? current_index - 1 : file_count-1;
	else if(play_cmd == CMD_NEXT) current_index= (current_index < (file_count-1)) ? current_index + 1 : 0;
}

void start_playing(uint32_t sample_rate)
{
	uint32_t actual_rate;

	/* 设置 I2S 分频到最接近目标采样率的档位，并拿到实际输出采样率 */
	actual_rate = i2s_set_sample_rate(sample_rate);

	/* 初始化软件重采样: 源率 -> 实际输出率
	 * step = src_rate / out_rate (定点 Q16)
	 */
	if (sample_rate == 0) sample_rate = 44100;
	resample_step = ((uint32_t)sample_rate << RESAMPLE_FRAC_BITS) / actual_rate;
	resample_pos = 0;
	out_frame_count = 0;
	/* 总输出帧数估算: 源总帧数 * out_rate / src_rate */
	total_out_frames = (uint32_t)((uint64_t)wav_total_frames * actual_rate / sample_rate);

	force_i2s_config();
	//OLED_Clear();
	//OLED_ShowNum(1, 1, SPI2->I2SPR, 4);
	fill_buffer_region(audio_buffer[0], AUDIO_BUF_SIZE);          /* 前半区 */
	fill_buffer_region(audio_buffer[1], AUDIO_BUF_SIZE);          /* 后半区 */
	half_ready = 0;
	full_ready = 0;
	play_state = STATE_PLAYING;

	/* 24bit/32bit 模式下 HAL 会把 Size<<1, 即实际 DMA 搬 2*AUDIO_BUF_SIZE 个半字，
	 * 正好覆盖 audio_buffer 两行(前后半区)。 */
	HAL_I2S_Transmit_DMA(&hi2s2, &audio_buffer[0][0], AUDIO_BUF_SIZE);
}

/* 读取 WAV 数据，软件重采样 + 立体声解帧 + 32bit 左对齐输出
 *
 * 源数据: 立体声 24bit PCM，帧 = L(3B) + R(3B) 小端有符号
 * 输出:   I2S 32bit 左对齐, 每帧 = L(高半字|低半字) + R(高半字|低半字) = 4 半字
 *
 * 重采样: 硬件实际采样率(如46.875kHz) 与源采样率(如44.1k) 不同，
 *         用定点相位 resample_pos 在源帧流中线性插值，
 *         step = (src_rate<<16)/out_rate，每输出一帧前进 step。
 *         用 f_lseek 绝对定位 + 一次性读入，相位跨缓冲区自然连续。
 *
 * 淡出:   输出帧接近末尾时线性衰减，避免结尾爆音。
 */
void fill_buffer_region(uint16_t *start, uint16_t num_halfwords) {
	uint32_t num_out_frames = num_halfwords / 4;   /* 每帧 = L高|L低|R高|R低 = 4 半字 */
	uint32_t i;
	uint32_t first_src = (uint32_t)(resample_pos >> RESAMPLE_FRAC_BITS);
	uint32_t read_frames;

	if (first_src >= wav_total_frames) {
		/* 源数据已读完 */
		for (i = 0; i < num_out_frames; i++) {
			start[i*4] = 0; start[i*4+1] = 0; start[i*4+2] = 0; start[i*4+3] = 0;
		}
		file_ended = 1;
		return;
	}

	/* 本行覆盖的源帧跨度: 从 first_src 到最后一帧的整数索引 + 2 帧余量(右端点+保险) */
	{
		uint32_t last_src = (uint32_t)((resample_pos + (uint64_t)num_out_frames * resample_step) >> RESAMPLE_FRAC_BITS);
		read_frames = last_src - first_src + 2;
	}
	if (read_frames > wav_total_frames - first_src) {
		read_frames = wav_total_frames - first_src;
	}
	if (read_frames == 0) {
		for (i = 0; i < num_out_frames; i++) {
			start[i*4] = 0; start[i*4+1] = 0; start[i*4+2] = 0; start[i*4+3] = 0;
		}
		file_ended = 1;
		return;
	}

	/* 定位到 first_src 帧并读取 */
	{
		uint32_t want = read_frames * wav_bytes_per_frame;
		if (want > sizeof(raw_buf)) want = sizeof(raw_buf);
		if (f_lseek(&SDFile, wav_data_start + (uint64_t)first_src * wav_bytes_per_frame) != FR_OK ||
		    f_read(&SDFile, raw_buf, want, &bytes_read) != FR_OK) {
			for (i = 0; i < num_out_frames; i++) {
				start[i*4] = 0; start[i*4+1] = 0; start[i*4+2] = 0; start[i*4+3] = 0;
			}
			file_ended = 1;
			return;
		}
	}
	uint32_t frames_avail = bytes_read / wav_bytes_per_frame;

	/* 淡出起点: 距末尾 FADE_SAMPLES 帧开始 */
	uint32_t fade_start = (total_out_frames > FADE_SAMPLES) ? (total_out_frames - FADE_SAMPLES) : 0;

	/* 读取单帧样本(L,R)的辅助宏 */
	#define GET_FRAME(buf, idx, pL, pR) do { \
		uint8_t *bp = (buf) + (idx) * (wav_bytes_per_frame); \
		if (wav_stereo) { \
			*(pL) = (int32_t)(((int32_t)(bp[0] | (bp[1]<<8) | (bp[2]<<16)) << 8) >> 8); \
			*(pR) = (int32_t)(((int32_t)(bp[3] | (bp[4]<<8) | (bp[5]<<16)) << 8) >> 8); \
		} else { \
			*(pL) = (int32_t)(((int32_t)(bp[0] | (bp[1]<<8) | (bp[2]<<16)) << 8) >> 8); \
			*(pR) = *(pL); \
		} \
	} while(0)

	for (i = 0; i < num_out_frames; i++) {
		uint64_t pos = resample_pos + (uint64_t)i * resample_step;
		uint32_t idx = (uint32_t)(pos >> RESAMPLE_FRAC_BITS);
		uint32_t frac = (uint32_t)(pos & RESAMPLE_FRAC_MASK);
		int32_t l0, l1, r0, r1;
		int32_t out_l, out_r;

		if (idx >= wav_total_frames) {
			out_l = 0; out_r = 0;
		} else if (idx + 1 >= wav_total_frames) {
			/* 最后一个源帧，无右端点 -> 直接用 */
			uint32_t lidx = idx - first_src;
			if (lidx >= frames_avail) { out_l = 0; out_r = 0; }
			else {
				GET_FRAME(raw_buf, lidx, &out_l, &out_r);
			}
		} else {
			uint32_t i0 = idx - first_src;
			uint32_t i1 = i0 + 1;
			if (i0 >= frames_avail) {
				out_l = 0; out_r = 0;
			} else if (i1 >= frames_avail) {
				GET_FRAME(raw_buf, i0, &l0, &r0);
				out_l = l0; out_r = r0;
			} else {
				GET_FRAME(raw_buf, i0, &l0, &r0);
				GET_FRAME(raw_buf, i1, &l1, &r1);
				out_l = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) >> RESAMPLE_FRAC_BITS);
				out_r = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) >> RESAMPLE_FRAC_BITS);
			}
		}

		/* 末尾淡出: 输出帧序号 >= fade_start 时线性衰减 */
		if (out_frame_count + i >= fade_start) {
			uint32_t fade_idx = out_frame_count + i - fade_start;
			int32_t gain = (int32_t)(FADE_SAMPLES - fade_idx);  /* 0..FADE_SAMPLES */
			if (gain < 0) gain = 0;
			out_l = (int32_t)(((int64_t)out_l * gain) >> 8);
			out_r = (int32_t)(((int64_t)out_r * gain) >> 8);
		}

		/* 左对齐 24bit -> 32bit: << 8, 拆成高/低半字 */
		int32_t s32_l = out_l << 8;
		int32_t s32_r = out_r << 8;
		start[i*4]     = (uint16_t)((uint32_t)s32_l >> 16);
		start[i*4+1]   = (uint16_t)((uint32_t)s32_l & 0xFFFF);
		start[i*4+2]   = (uint16_t)((uint32_t)s32_r >> 16);
		start[i*4+3]   = (uint16_t)((uint32_t)s32_r & 0xFFFF);
	}

	/* 推进全局相位 (本行共输出 num_out_frames 帧) */
	resample_pos += (uint64_t)num_out_frames * resample_step;
	out_frame_count += num_out_frames;

	/* 判断文件是否已播完 */
	if (resample_pos >= ((uint64_t)wav_total_frames << RESAMPLE_FRAC_BITS)) {
		file_ended = 1;
	}

	#undef GET_FRAME
}

int parse_wav_header(FIL *file, wav_file_parameters *header) {
    unsigned int bytesRead;
    // 读取头部44字节
    if (f_read(file, header, sizeof(wav_file_parameters), &bytesRead) != FR_OK || bytesRead != sizeof(wav_file_parameters))
        return -1;
    // 简单校验
    if (memcmp(header->riff, "RIFF", 4) != 0 || memcmp(header->wave, "WAVE", 4) != 0)
        return -2;
    if (memcmp(header->fmt, "fmt ", 4) != 0 || header->audioFormat != 1) // 仅支持PCM
        return -3;
    return 0;
}

void mp3_file_process(file_list *ado_file)
{

}

void flac_file_process(file_list *ado_file)
{

}

void audio_pause(void) {
    if (play_state == STATE_PLAYING) {
        HAL_I2S_DMAPause(&hi2s2);
        play_state = STATE_PAUSED;
    }
}

void audio_resume(void) {
    if (play_state == STATE_PAUSED) {
        HAL_I2S_DMAResume(&hi2s2);
        play_state = STATE_PLAYING;
    }
}

void audio_stop(void) {
    HAL_I2S_DMAStop(&hi2s2);   // 停止 DMA，关闭外设
    //f_close(&SDFile);
    play_state = STATE_STOPPED;
}

void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
	//HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_5);
	half_ready = 1;
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
	full_ready = 1;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{

	if(GPIO_Pin == GPIO_PIN_0) play_cmd = CMD_PREV;
	else if(GPIO_Pin == GPIO_PIN_1) play_cmd = CMD_NEXT;
	else if(GPIO_Pin == GPIO_PIN_2){
		//osDelay(20);
		if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_2) == GPIO_PIN_RESET) {
			    if (play_state == STATE_PLAYING) {
			        play_cmd = CMD_PAUSE;
			    }
			    else if (play_state == STATE_PAUSED) {
			        play_cmd = CMD_RESUME;
			    }
			    else if (play_state == STATE_STOPPED) {
			        play_cmd = CMD_RESUME;
			    }
		    }
	}
	else if(GPIO_Pin == GPIO_PIN_3) play_cmd = CMD_STOP;
}


