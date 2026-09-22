/*
 * Music_Driver.c
 *
 *  Created on: Mar 7, 2026
 *      Author: 15754
 */

#include "fatfs.h"
#include "bsp_driver_sd.h"
#include <stdio.h>
#include <string.h>
#include "Music_Driver.h"
#include "main.h"          /* F407: 引入 stm32f4xx_hal.h 及 hi2c1/hi2s2/hsd 句柄 */
#include "OLED.h"
#include "wm8960.h"        /* WM8960 codec: 切歌/停机时的上下电时序(停 MCLK 前先关 DAC, 见 wm8960.c) */
#if ENABLE_FLAC
#include "flac_decoder.h"
#endif
#if ENABLE_MP3
#include "mp3dec.h"        /* Helix 定点 MP3 解码器 (替代 minimp3: ARM 上无浮点问题) */
extern void helix_pool_init(void *pool, size_t size);   /* Helix 内存池初始化 */
#endif
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "queue.h"
#include "usbd_storage_if.h"

/* F407: hi2s2 句柄定义在 main.c (F103 是从 i2s.h 引入), 这里 extern */
extern I2S_HandleTypeDef hi2s2;

/* 磁盘层"读失败"标志(拔卡判定), 定义在 FATFS/Target/sd_diskio.c */
extern volatile uint8_t g_sd_io_err;

/* SD 卡扫描信号量 (定义在 freertos.c) */
extern osSemaphoreId_t LOAD_OR_NOTHandle;



/* 外部变量声明，FATFS相关对象通常在fatfs.c中定义 */
extern FATFS SDFatFS;    /* 文件系统对象 */
extern FIL SDFile;       /* 文件对象 */
DIR SDDir;        /* 目录对象 */
FILINFO SDFileInfo; /* 文件信息对象 */
FRESULT res;
UINT bytes_read;

/* FatFs R0.12c: _USE_LFN 时 fname 直接就是长文件名数组, 无需额外 lfn 缓冲 */

extern osMessageQueueId_t audioQueue;
extern osSemaphoreId_t sem_dma_done;
extern volatile uint8_t play_enabled;

/* 缓存区，用于存放从文件读出的数据 */
/* 两行: 行0(前半区) 和 行1(后半区)，配合 HAL 24bit/32bit 模式 DMA 长度翻倍语义 */
__attribute__((aligned(4))) uint16_t audio_buffer[NUM_BUFFERS][AUDIO_BUF_SIZE];
volatile uint8_t half_ready = 0;        // 标志：是否需要填充下半区
volatile uint8_t full_ready = 0;        // 标志：是否需要填充上半区
volatile PlayState play_state = STATE_STOPPED;
/* ★PF5 停止键防毛刺: 双沿触发 + 最小时长过滤(见 gpio.c 的 PF5 配置说明) */
static volatile uint8_t  key_stop_down = 0;
static volatile uint32_t key_stop_tick = 0;
#define STOP_KEY_MIN_MS 20              /* 按下时长 >= 20ms 才算真按键(毛刺一般 µs~ms) */
/* ★所有按键共用: 上电后这段时间内忽略一切按键边沿。
 * 原因(实测): 上电瞬间电源/地未稳, 按键线上易出毛刺; 而 PF0/PF1 的"切歌"标志(见 freertos.c
 * 滚动定时器里的短按判定)只能由 EXTI 边沿置位 → 一条上电毛刺会被当成"短按切下一首" →
 * 发出 CMD_NEXT → audio_file_read 里三个格式分支都不进、直接 current_index++ →
 * 表现就是"一上电跳过第一首歌"。 */
#define KEY_STARTUP_MS 1000
volatile Mode_Keyword play_cmd = CMD_NONE;
volatile int seek_speed = 0;      /* 0=正常, 2/3/4=快进倍速, -2/-3/-4=快退倍速 */
volatile uint8_t current_audio_type = 0;   /* 1=wav 2=mp3 3=flac (供按键长按逻辑判断快退支持) */

/* 切歌键(PE0/PE1)按下状态: EXTI 捕获按下并记录时刻, 滚动定时器轮询判定松开/长按 */
volatile uint8_t  key_prev_down = 0;
volatile uint8_t  key_next_down = 0;
volatile uint32_t key_prev_tick = 0;
volatile uint32_t key_next_tick = 0;
file_list audiofiles[max_size];
static uint8_t prod_idx = 0;
uint16_t file_count = 0;
uint16_t current_index = 0;
int i;
uint8_t file_ended = 0;

/* 工作缓冲 union: WAV 重采样 / FLAC 解码器 / Helix MP3 解码器 — 三者互斥, 复用同一块 CCM 内存 */
#define HELIX_POOL_SIZE 25600    /* Helix MP3 解码器状态实测 ~23.8KB, 留余量 */
static union {
    uint8_t wav_raw[AUDIO_BUF_SIZE * 4] __attribute__((aligned(4)));
#if ENABLE_FLAC
    flac_decoder flac;
#endif
#if ENABLE_MP3
    uint8_t helix_pool[HELIX_POOL_SIZE] __attribute__((aligned(8)));   /* Helix MP3 解码器状态(~23KB) */
#endif
} work_buf __attribute__((section(".ccmram")));
/* WAV 填充代码统一用 WAV_RAW 引用该缓冲 */
#define WAV_RAW work_buf.wav_raw

/* ============ MP3 播放状态 (Helix 定点解码器) ============ */
#if ENABLE_MP3
volatile uint8_t mp3_play_ok = 0;   /* 0=MP3打开/首帧失败, 1=正常播放(供 audio_file_read 决定是否跳下一首) */
static HMP3Decoder mp3_hdec = 0;             /* Helix 解码器句柄 */
static uint8_t  mp3_inbuf[4096];             /* 压缩数据缓冲 */
static uint32_t mp3_inbuf_len = 0;
static uint32_t mp3_inbuf_pos = 0;
static uint8_t  mp3_eof = 0;                 /* 文件读完标志 */
static int16_t  mp3_pcm[2304 + 256];         /* 一帧输出 (1152*2 样本 + 余量) */
static uint32_t mp3_channels = 2;            /* 声道数 */
static uint32_t mp3_hz = 0;                  /* MP3 采样率 */
static uint32_t mp3_total_frames = 0;        /* 已解码总样本数(全局样本计数) */
static uint32_t mp3_frame_start = 0;         /* 当前帧全局起始样本 */
static uint32_t mp3_frame_frames = 0;        /* 当前帧每声道样本数 */
#endif /* ENABLE_MP3 */

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
static uint32_t wav_data_start;       /* WAV data 区起始字节偏移 */
static uint32_t wav_total_frames;     /* 总帧数 = dataSize / bytes_per_frame */
static uint32_t wav_bytes_per_frame;  /* 每帧字节数 = numChannels * bitsPerSample/8 */
static uint8_t  wav_sample_bytes;     /* 单声道样本字节数 = bitsPerSample/8 (1/2/3/4) */
static int8_t   wav_norm_shift;       /* 样本归一化到 24bit 的有符号移位: 8bit->16, 16bit->8, 24bit->0, 32bit->-8 */
static uint8_t  wav_stereo;           /* 1=立体声, 0=单声道 */
static uint32_t out_frame_count;      /* 已输出的帧数 (全局计数, 用于淡出) */
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
    /* F407: 用 PLLI2S 精确产生采样率。
     * 本项目 HSE=25MHz, PLLM=25 -> PLLI2S VCO 输入 1MHz, 输出 = PLLI2SN/PLLI2SR MHz。
     * 目标: PLLI2S 输出 ≈ sample_rate * 1024 (24bit I2S标准, I2SPR div=2 时 fs = clk/1024)。
     * 在 N(192..432), R(2..7) 中搜索最接近的整数比。 */
    uint32_t target = (uint32_t)sample_rate * 1024u;
    uint32_t best_n = 271, best_r = 6;
    uint64_t best_err = UINT64_MAX;
    uint64_t out;
    uint32_t n, r;

    if (sample_rate == 0) sample_rate = 44100;

    for (r = 2; r <= 7; r++) {
        for (n = 192; n <= 432; n++) {
            out = (uint64_t)n * 1000000u / r;   /* PLLI2S 输出 Hz */
            uint64_t err = (out > target) ? (out - target) : (target - out);
            if (err < best_err) { best_err = err; best_n = n; best_r = r; }
        }
    }

    /* 配置 PLLI2S (N/R), 使 I2S 时钟 = PLLI2S 输出。
     * 注意: 必须检查返回值并重试, 否则切换采样率时 PLLI2S 可能没生效,
     * 硬件保持旧采样率, 而这里返回计算值, 导致重采样与硬件不匹配(音调变低/慢速)。 */
    RCC_PeriphCLKInitTypeDef PeriphClk = {0};
    PeriphClk.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    PeriphClk.PLLI2S.PLLI2SN = best_n;
    PeriphClk.PLLI2S.PLLI2SR = best_r;
    {
        int attempt;
        HAL_StatusTypeDef hst = HAL_ERROR;
        /* 关键修复: 先禁用 PLLI2S 再重新配置。
         * 否则 PLLI2S 在运行时改 N/R, 若没重新锁定到新频率, 残留旧锁
         * (寄存器显示新值但实际输出旧频率) -> 48k 文件按 44.1k 播放 = 慢+音调低。
         * 先禁用强制重新锁定到新 N/R。 */
        __HAL_RCC_PLLI2S_DISABLE();
        {
            uint32_t tmo = 100000;
            while (__HAL_RCC_GET_FLAG(RCC_FLAG_PLLI2SRDY) != RESET && tmo--) {}
        }
        for (attempt = 0; attempt < 5 && hst != HAL_OK; attempt++) {
            hst = HAL_RCCEx_PeriphCLKConfig(&PeriphClk);
            if (hst != HAL_OK) osDelay(10);
        }
    }

    /* I2SPR: div=2, odd=0, MCLK 使能 -> fs = PLLI2S输出 / 1024 */
    SPI2->I2SPR = 2u | SPI_I2SPR_MCKOE;

    /* 返回实际配置的采样率: 读 PLLI2SCFGR 的 N/R, 确保与硬件一致 */
    {
        uint32_t rn = (RCC->PLLI2SCFGR & RCC_PLLI2SCFGR_PLLI2SN) >> RCC_PLLI2SCFGR_PLLI2SN_Pos;
        uint32_t rr = (RCC->PLLI2SCFGR & RCC_PLLI2SCFGR_PLLI2SR) >> RCC_PLLI2SCFGR_PLLI2SR_Pos;
        if (rr < 2) rr = 2;
        return (uint32_t)(((uint64_t)rn * 1000000u / rr) / 1024u);
    }
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
    /* 1. 挂载文件系统。FAT 表不一致/瞬时错误时自动卸载重挂(重读 FAT 表) */
    {
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            res = f_mount(&SDFatFS, SDPath, 1);
            if (res == FR_OK) break;
            f_mount(NULL, SDPath, 0);   /* 卸载, 下次强制重挂 */
            osDelay(80);
        }
        if (res != FR_OK) {
        	OLED_ClearBuffer();
        	char strerror[44]={0};
        	if (res == FR_NO_FILESYSTEM) sprintf(strerror, "无文件系统 请格式化");
        	else if (res == FR_NOT_READY) sprintf(strerror, "SD卡未就绪 请重试");
        	else if (res == FR_INT_ERR)   sprintf(strerror, "文件系统异常 请检查");
        	else sprintf(strerror, "挂载失败 错误:%d", (int)res);
			OLED_DrawStringToBuffer(strerror);
        	for(i=0; i<40*8 ;i++)
        	{
        		OLED_RefreshScreenWithScroll();
        		scrollOffset++;
        	}
        	osSemaphoreRelease(LOAD_OR_NOTHandle);
        	return 1;
        }
    }

    /* 2. 打开根目录, 失败时重挂一次再试 */
    res = f_opendir(&SDDir, SDPath);
    if (res != FR_OK) {
        /* 可能是 FAT 表被修改导致目录扇区读错: 卸载重挂再试 */
        f_mount(NULL, SDPath, 0);
        osDelay(100);
        res = f_mount(&SDFatFS, SDPath, 1);
        if (res == FR_OK) res = f_opendir(&SDDir, SDPath);
    }
    if (res != FR_OK) {
    	OLED_ClearBuffer();
    	char strerror[44]={0};
    	if (res == FR_NO_FILESYSTEM) sprintf(strerror, "无文件系统 请格式化");
    	else if (res == FR_INT_ERR || res == FR_DISK_ERR) sprintf(strerror, "文件系统异常 请检查");
    	else sprintf(strerror, "打开目录失败 错误:%d", (int)res);
	    OLED_DrawStringToBuffer(strerror);
    	for(i=0; i<42*8 ;i++)
    	{
    	    OLED_RefreshScreenWithScroll();
    	    scrollOffset++;
    	}
    	osSemaphoreRelease(LOAD_OR_NOTHandle);
        return 2;
    }


    /* 3. 循环读取目录项 */
    while (1) {
        /* FatFs R0.12c: _USE_LFN 时 f_readdir 直接填充 fname 为长文件名(GBK编码) */
        res = f_readdir(&SDDir, &SDFileInfo);
        /* 读取失败或没有更多文件时退出 */
        if (SDFileInfo.fname[0] == 0) {
            break;
        }
        if (res != FR_OK) {
        	osSemaphoreRelease(LOAD_OR_NOTHandle);
            break;
        }

        /* 取文件名: fname 即长文件名(含中文) */
        {
        	const TCHAR *name = SDFileInfo.fname;

	        /* 忽略子目录，只处理文件 */
	        if (!(SDFileInfo.fattrib & AM_DIR)) {
	            /* 检查文件扩展名 */
	            if (check_extension(name, "wav") ||
	                check_extension(name, "mp3") ||
	                check_extension(name, "flac")) {

	            	if(file_count < max_size)
	            	{
	            		strcpy(audiofiles[file_count].audio_file_names, name);
	            		if(check_extension(name, "wav"))
	            			strcpy(audiofiles[file_count].audio_file_type,"wav");
	            		else if(check_extension(name, "mp3"))
	            			strcpy(audiofiles[file_count].audio_file_type,"mp3");
	            		else if(check_extension(name, "flac"))
	            		    strcpy(audiofiles[file_count].audio_file_type,"flac");
	            		file_count++;
	            	}
	            	else if (file_count >= max_size)
	            	{
	            		OLED_ShowString(1, 1, "文件已满");
	            		break;
	            	}
	            }
	        }
        }

    }
    if(audiofiles[0].audio_file_names[0] == '\0' && file_count == 0)
    {
    	return 3;
    }
    else
    {
    	char str[16] = {0};
    	sprintf(str, "文件数:%d", file_count);
    	OLED_Clear();
    	OLED_ShowString(1, 1, str);
    }


    /* 7. 关闭目录 */
    f_closedir(&SDDir);
    return 0;
}

/* ================= SD 卡热插拔处理 ================= */


/* 快速检查 SD 卡是否还在。注意: 仅在"没有打开任何文件"时调用(会重新挂载卷)! */
static uint8_t sd_card_present(void)
{
	return (f_mount(&SDFatFS, SDPath, 1) == FR_OK) ? 1 : 0;
}

/* 卡被拔出: 显示提示, 每 300ms 重试挂载, 直到插回(或 USB 占用 / 用户按停止)。
 * 插回后触发重新扫描 SD 并返回。返回 1=已恢复, 0=放弃。 */
uint8_t sd_wait_card(void)
{
	OLED_Clear();
	OLED_ShowString(1, 1, "SD卡已拔出");
	OLED_ShowString(2, 1, "请插入SD卡");
	while (1) {
		OLED_RefreshScreenWithScroll();          /* 静态消息需手动刷新 */
		osDelay(300);
		res = f_mount(&SDFatFS, SDPath, 1);
		if (res == FR_OK) break;                 /* 插回成功 */
		if (usb_sd_occupied) return 0;
		if (play_cmd == CMD_STOP) { play_cmd = CMD_NONE; return 0; }
	}
	OLED_Clear();
	OLED_ShowString(1, 1, "SD卡已插入");
	OLED_ShowString(2, 1, "重新扫描...");
	OLED_RefreshScreenWithScroll();
	osSemaphoreRelease(LOAD_OR_NOTHandle);       /* 触发文件列表重新扫描 */
	osDelay(1500);                               /* 停留显示, 让用户看到"已插入/重新扫描" */
	return 1;
}

uint8_t audio_file_read(file_list *ado_file)
{
	uint8_t audio_type;

	/* USB 占用 SD 卡（上位机连接）时，阻塞等待 USB 断开。
	 * 不用任务挂起，而是自旋等待标志清零，避免恢复时序问题。 */
	while (usb_sd_occupied)
	{
		osDelay(50);
	}

	if(strcmp(ado_file[current_index].audio_file_type,"wav")==0)
		audio_type=1;
	else if(strcmp(ado_file[current_index].audio_file_type,"mp3")==0)
		audio_type=2;
	else if(strcmp(ado_file[current_index].audio_file_type,"flac")==0)
		audio_type=3;
	current_audio_type = audio_type;

	res = f_mount(&SDFatFS, SDPath, 1);
	if (res != FR_OK)
	{
	    /* 卡被拔出: FR_NOT_READY(无卡) 或 FR_DISK_ERR(卡没了/读失败) 都按拔卡处理。
	     * 之前拔卡后要等 30 秒读超时才走到这里(已把 SD_TIMEOUT 改成 300ms)。 */
	    if (res == FR_NOT_READY || res == FR_DISK_ERR) {
	        sd_wait_card();
	        return 1;
	    }
	    /* 其它错误: 提示挂载失败 */
	    OLED_ClearBuffer();
	    char strerror[44]={0};
	    sprintf(strerror, "SD卡挂载失败, 请检查");
	    OLED_DrawStringToBuffer(strerror);
	    for(i=0; i<40*8 ;i++)
	    {
	        OLED_RefreshScreenWithScroll();
	        scrollOffset++;
	        osDelay(1);
	    }
	    osSemaphoreRelease(LOAD_OR_NOTHandle);
	    return 1;
	}

	if(audio_type == 1 && play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
	{
		OLED_ShowString(1, 1, "已停止");
		OLED_ShowString(2, 1, "按按键3");
		OLED_ShowString(3, 1, "开始播放");
		{
			uint16_t sd_chk = 0;
			while(play_state == STATE_STOPPED && play_cmd != CMD_RESUME) {
				osDelay(10);
				if (++sd_chk >= 50) {              /* 每 ~500ms 检查一次卡是否还在 */
					sd_chk = 0;
					if (!sd_card_present()) {
						sd_wait_card();            /* 拔卡: 提示并等插回 */
						OLED_Clear();
						OLED_ShowString(1, 1, "已停止");
						OLED_ShowString(2, 1, "按按键3");
						OLED_ShowString(3, 1, "开始播放");
					}
				}
			}
		}
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
		OLED_ShowString(1, 1, "已停止");
		OLED_ShowString(1, 1, "按按键3");
		OLED_ShowString(2, 1, "开始播放");
		{
			uint16_t sd_chk = 0;
			while(play_state == STATE_STOPPED && play_cmd != CMD_RESUME) {
				osDelay(10);
				if (++sd_chk >= 50) {              /* 每 ~500ms 检查一次卡是否还在 */
					sd_chk = 0;
					if (!sd_card_present()) {
						sd_wait_card();            /* 拔卡: 提示并等插回 */
						OLED_Clear();
						OLED_ShowString(1, 1, "已停止");
						OLED_ShowString(2, 1, "按按键3");
						OLED_ShowString(3, 1, "开始播放");
					}
				}
			}
		}
		play_cmd = CMD_NONE;
		play_state = STATE_PLAYING;
		file_ended = 0;
		mp3_file_process(ado_file);
		if(mp3_play_ok == 0) {
			/* MP3 失败(打开/首帧): 已停住, 这里按用户按键决定跳转 */
			if(play_cmd == CMD_PREV) current_index = (current_index > 0) ? current_index - 1 : file_count - 1;
			else if(play_cmd == CMD_STOP) { play_state = STATE_STOPPED; }   /* 停止返回列表 */
			else current_index = (current_index < (file_count-1)) ? current_index + 1 : 0;  /* 播放键/超时/下一首 -> 跳下一首 */
			play_cmd = CMD_NONE;
		} else {
			/* 正常播放结束 */
			if(play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
				current_index= (current_index < (file_count-1)) ? current_index + 1 : 0;
			else if (play_cmd == CMD_PREV || play_cmd == CMD_NEXT)
				play_cmd = CMD_NONE;
		}
	}
	else if(audio_type == 3 && play_cmd != CMD_PREV && play_cmd != CMD_NEXT)
	{
		OLED_ShowString(1, 1, "已停止");
		OLED_ShowString(1, 1, "按按键3");
		OLED_ShowString(2, 1, "开始播放");
		{
			uint16_t sd_chk = 0;
			while(play_state == STATE_STOPPED && play_cmd != CMD_RESUME) {
				osDelay(10);
				if (++sd_chk >= 50) {              /* 每 ~500ms 检查一次卡是否还在 */
					sd_chk = 0;
					if (!sd_card_present()) {
						sd_wait_card();            /* 拔卡: 提示并等插回 */
						OLED_Clear();
						OLED_ShowString(1, 1, "已停止");
						OLED_ShowString(2, 1, "按按键3");
						OLED_ShowString(3, 1, "开始播放");
					}
				}
			}
		}
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

/* 快进/快退状态显示: 仅在倍速变化时刷新一次; 恢复正常时恢复歌名显示 */
static void seek_update_display(file_list *ado_file)
{
	static int last_seek_disp = 0;
	if (seek_speed == last_seek_disp) return;
	last_seek_disp = seek_speed;

	if (seek_speed > 0) {
		OLED_Clear();
		OLED_ShowString(1, 1, "快进");
		OLED_ShowNum(2, 1, (uint32_t)seek_speed, 1);
		OLED_ShowString(2, 2, "x");
		OLED_ShowString(3, 1, "松开恢复");
	} else if (seek_speed < 0) {
		OLED_Clear();
		OLED_ShowString(1, 1, "快退");
		OLED_ShowNum(2, 1, (uint32_t)(-seek_speed), 1);
		OLED_ShowString(2, 2, "x");
		OLED_ShowString(3, 1, "松开恢复");
	} else {
		OLED_Clear();
		{
			static char song_utf8[256];
			OLED_GBKString_To_UTF8(ado_file[current_index].audio_file_names, song_utf8);
			OLED_ShowScrollingString(song_utf8);
		}
	}
}

void wav_file_process(file_list *ado_file)
{
	char full_path[128]={0};
	sprintf(full_path, "0:/%s", ado_file[current_index].audio_file_names);
	res = f_open(&SDFile, full_path, FA_READ);
	wav_file_parameters wave;
	int wav_flag = parse_wav_header(&SDFile, &wave);
	/* FAT 表瞬时错误: f_open 成功但读头失败, 重开一次再试 (自动恢复) */
	if (res == FR_OK && wav_flag != 0) {
		f_close(&SDFile);
		res = f_open(&SDFile, full_path, FA_READ);
		wav_flag = parse_wav_header(&SDFile, &wave);
	}
	if(wav_flag == 0 && res == FR_OK)
	{
		uint8_t sr_len, bits_len;

		/* data 区起点 = 解析完头后文件指针位置 (兼容非标准 44 字节头, 已按块扫描) */
		wav_data_start = f_tell(&SDFile);

		/* 每帧字节数 = 声道数 * 位深/8 (支持 8/16/24/32bit, 单/双声道) */
		if (wave.numChannels == 0) wave.numChannels = 1;
		if (wave.bitsPerSample == 0) wave.bitsPerSample = 16;
		wav_bytes_per_frame = wave.numChannels * (wave.bitsPerSample / 8);
		wav_sample_bytes = wave.bitsPerSample / 8;
		if (wav_sample_bytes < 1 || wav_sample_bytes > 4) wav_sample_bytes = 2;

		/* 样本归一化移位: 统一转成 24bit 有符号, 便于 I2S 32bit 左对齐输出 */
		switch (wave.bitsPerSample) {
			case 8:  wav_norm_shift = 16; break;   /* 8bit 无符号 -> 中心为0 */
			case 16: wav_norm_shift = 8;  break;
			case 24: wav_norm_shift = 0;  break;
			case 32: wav_norm_shift = -8; break;   /* 截掉低8位保留高24位 */
			default: wav_norm_shift = 8;  break;
		}
		wav_stereo = (wave.numChannels > 1) ? 1 : 0;
		wav_total_frames = wave.dataSize / wav_bytes_per_frame;

		/* 播放前显示文件采样频率和位数 */
		OLED_Clear();
		OLED_ShowString(2, 1, "采样率:");
		sr_len = (wave.sampleRate >= 10000) ? 5 : ((wave.sampleRate >= 1000) ? 4 : 3);
		OLED_ShowNum(2, 8, wave.sampleRate, sr_len);
		OLED_ShowString(2, 8 + sr_len, "Hz");
		OLED_ShowString(3, 1, "声道:");
		OLED_ShowNum(3, 5, wave.numChannels, 1);
		OLED_ShowString(4, 1, "位深:");
		bits_len = (wave.bitsPerSample >= 10) ? 2 : 1;
		OLED_ShowNum(4, 5, wave.bitsPerSample, bits_len);
		OLED_ShowString(4, 5 + bits_len, "位");
		/* 歌名: GBK->UTF-8 后滚动显示(第1行), 长名自动横向滚动 */
		{
			static char song_utf8[256];
			OLED_GBKString_To_UTF8(ado_file[current_index].audio_file_names, song_utf8);
			OLED_ShowScrollingString(song_utf8);
		}
		start_playing(wave.sampleRate);
	}
	else
	{
		OLED_ShowString(1, 1, "文件损坏/跳过");
		OLED_ShowString(3, 1, "按播放键下一首");
		OLED_ShowString(4, 1, "                ");
		osSemaphoreRelease(LOAD_OR_NOTHandle);
	}

	/* 滚动显示状态: 歌名是否超宽由滚动定时器回调检查 scrollTextWidth > 128 决定 */

	while (!file_ended && play_state != STATE_STOPPED)
	{
		if (play_cmd != CMD_NONE)
		{
		    switch (play_cmd)
		    {
		    	case CMD_PREV: case CMD_NEXT: file_ended = 1;break;
		        case CMD_PAUSE: {audio_pause(); play_cmd = CMD_NONE; OLED_Clear(); OLED_ShowString(1, 1, "暂停!"); break;}
		        case CMD_RESUME: {audio_resume(); play_cmd = CMD_NONE; OLED_Clear(); {static char song_utf8[256]; OLED_GBKString_To_UTF8(ado_file[current_index].audio_file_names, song_utf8); OLED_ShowScrollingString(song_utf8);} break;}
		        case CMD_STOP: {audio_stop(); play_cmd = CMD_NONE; OLED_Clear(); break;}
		        default: break;
		    }
		 }
		if (play_state == STATE_PLAYING)
			{
				/* USB 占用 SD 卡（上位机连接）时，停止播放让出 SD 卡。
				 * 正常情况下 USB_TASK 会挂起本任务，这里作为双保险。 */
				if (usb_sd_occupied)
				{
					audio_stop();
					file_ended = 1;
					break;
				}
				/* SD 读失败(很可能拔卡): 立即停止, 回到上层会检测无卡并提示 */
				if (g_sd_io_err) { g_sd_io_err = 0; audio_stop(); file_ended = 1; break; }
				/* 快进/快退状态显示 (仅变化时刷新); 快退到开头时由 fill 填静音, 松开后恢复 */
				seek_update_display(ado_file);
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
					/* 两个半区都已填满，本 tick 无需填充音频：阻塞让出 CPU 1 tick，
					 * 等待 DMA 半/全传输中断置位。
					 * 注意：不能用 osThreadYield()，它只切到同优先级任务，不会让
					 * 低优先级 OLED 滚动任务运行，会导致其被饿死。
					 *
					 * 滚动字幕刷新已移交软件定时器回调(freertos.c), 本任务不再碰 I2C。
					 * 原来的做法是在这里同步 OLED_RefreshScreenWithScroll()，一次整屏
					 * I2C 刷新约耗时 10ms+，接近甚至超过单个 DMA 缓冲区的播放时长
					 * (约 512 帧 / 46.875kHz ≈ 11ms)，音频任务被 I2C 忙等卡住而漏填
					 * 缓冲区，造成 DMA 下溢、播放变慢/卡顿。 */
					osDelay(1);
				}
			}
	}
	WM8960_BeforeClockStop();   /* ★手册 p69: 停 MCLK 前先软静音 + 关 DAC 并等 >=1ms(内部含延时) */
	HAL_I2S_DMAStop(&hi2s2);
	f_close(&SDFile);
	if(play_cmd == CMD_PREV) current_index= (current_index > 0) ? current_index - 1 : file_count-1;
	else if(play_cmd == CMD_NEXT) current_index= (current_index < (file_count-1)) ? current_index + 1 : 0;
}

void start_playing(uint32_t sample_rate)
{
	uint32_t actual_rate;

	/* 每次开始播放复位变速状态 */
	seek_speed = 0;

	/* 设置 I2S 分频到最接近目标采样率的档位，并拿到实际输出采样率 */
	actual_rate = i2s_set_sample_rate(sample_rate);

	/* 初始化软件重采样: 源率 -> 实际输出率
	 * step = src_rate / out_rate (定点 Q16, 用 64 位中间量防 96k 等溢出) */
	if (sample_rate == 0) sample_rate = 44100;
	resample_step = (uint32_t)(((uint64_t)sample_rate << RESAMPLE_FRAC_BITS) / actual_rate);
	resample_pos = 0;
	out_frame_count = 0;
	/* 总输出帧数估算: 源总帧数 * out_rate / src_rate */
	total_out_frames = (uint32_t)((uint64_t)wav_total_frames * actual_rate / sample_rate);

	force_i2s_config();
	fill_buffer_region(audio_buffer[0], AUDIO_BUF_SIZE);          /* 前半区 */
	fill_buffer_region(audio_buffer[1], AUDIO_BUF_SIZE);          /* 后半区 */
	half_ready = 0;
	full_ready = 0;
	play_state = STATE_PLAYING;

	/* 24bit/32bit 模式下 HAL 会把 Size<<1, 即实际 DMA 搬 2*AUDIO_BUF_SIZE 个半字，
	 * 正好覆盖 audio_buffer 两行(前后半区)。 */
	HAL_I2S_Transmit_DMA(&hi2s2, &audio_buffer[0][0], AUDIO_BUF_SIZE);
	WM8960_AfterClockStart();   /* ★时钟已稳(且在起 DMA 之后): 按手册反序恢复 codec(内部含延时) */
}

/* 读取一个源样本 (按当前位深), 返回有符号 int32, 值域与位深一致。
 * 8bit=无符号(0~255), 16/24/32bit=有符号小端。 */
static int32_t wav_read_sample(uint8_t *bp)
{
	switch (wav_sample_bytes) {
		case 1:  return ((int32_t)bp[0]) - 128;                                              /* 8bit 无符号 PCM, 居中为0 */
		case 2:  return (int16_t)(bp[0] | (bp[1] << 8));                                     /* 16bit 有符号 */
		case 3: {   /* 24bit 有符号, 显式符号扩展避免移位溢出未定义行为 */
			uint32_t u = (uint32_t)bp[0] | ((uint32_t)bp[1]<<8) | ((uint32_t)bp[2]<<16);
			if (u & 0x800000u) u |= 0xFF000000u;
			return (int32_t)u;
		}
		case 4:  return (int32_t)((uint32_t)bp[0] | ((uint32_t)bp[1]<<8) | ((uint32_t)bp[2]<<16) | ((uint32_t)bp[3]<<24)); /* 32bit 有符号 */
		default: return 0;
	}
}

/* 读取一帧的左/右样本, 并归一化到 24bit 有符号范围(便于统一 <<8 输出到 I2S 32bit 左对齐) */
static void wav_get_frame(uint8_t *buf, uint32_t idx, int32_t *pL, int32_t *pR)
{
	uint8_t *bp = buf + (uint32_t)idx * wav_bytes_per_frame;
	int32_t l = wav_read_sample(bp);
	int32_t r = wav_stereo ? wav_read_sample(bp + wav_sample_bytes) : l;
	if (wav_norm_shift >= 0) {
		*pL = l << wav_norm_shift;
		*pR = r << wav_norm_shift;
	} else {            /* 32bit -> 24bit: 算术右移截掉低8位 */
		*pL = l >> (-wav_norm_shift);
		*pR = r >> (-wav_norm_shift);
	}
}

/* 读取 WAV 数据，软件重采样 + 声道解帧 + 32bit 左对齐输出
 *
 * 源数据: 8/16/24/32bit PCM, 单/双声道, 帧 = 每声道 sample_bytes 字节 小端有符号
 *         (8bit 无符号, 居中为 0)
 * 输出:   I2S 32bit 左对齐, 每帧 = L(高半字|低半字) + R(高半字|低半字) = 4 半字
 *
 * 重采样: 硬件实际采样率 与 源采样率 不同,
 *         用定点相位 resample_pos 在源帧流中线性插值,
 *         step = (src_rate<<16)/out_rate, 每输出一帧前进 step。
 *         用 f_lseek 绝对定位 + 一次性读入, 相位跨缓冲区自然连续。
 *
 * 淡出:   输出帧接近末尾时线性衰减, 避免结尾爆音。
 */
void fill_buffer_region(uint16_t *start, uint16_t num_halfwords) {
	uint32_t num_out_frames = num_halfwords / 4;   /* 每帧 = L高|L低|R高|R低 = 4 半字 */
	uint32_t i;
	int reverse = (seek_speed < 0);
	uint64_t eff_step;

	/* 有效步进: 快进/快退时按倍速缩放 */
	if (seek_speed > 0)      eff_step = (uint64_t)resample_step * (uint32_t)seek_speed;
	else if (seek_speed < 0) eff_step = (uint64_t)resample_step * (uint32_t)(-seek_speed);
	else                     eff_step = resample_step;

	/* 淡出起点 (仅正向播放接近末尾时生效) */
	uint32_t fade_start = (total_out_frames > FADE_SAMPLES) ? (total_out_frames - FADE_SAMPLES) : 0;

	if (!reverse)
	{
		/* ================= 正向 (正常/快进) ================= */
		uint32_t first_src = (uint32_t)(resample_pos >> RESAMPLE_FRAC_BITS);
		uint32_t read_frames;

		if (first_src >= wav_total_frames) {
			/* 源数据已读完 */
			for (i = 0; i < num_out_frames; i++) {
				start[i*4] = 0; start[i*4+1] = 0; start[i*4+2] = 0; start[i*4+3] = 0;
			}
			/* 快进到末尾时先停在末尾(填静音)等松开, 松开后 seek_speed=0 再由下次填充正常收尾 */
			if (seek_speed > 0) return;
			file_ended = 1;
			return;
		}

		/* 本行覆盖的源帧跨度 */
		{
			uint32_t last_src = (uint32_t)((resample_pos + (uint64_t)num_out_frames * eff_step) >> RESAMPLE_FRAC_BITS);
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
			if (want > sizeof(WAV_RAW)) {
				want = sizeof(WAV_RAW);
				read_frames = want / wav_bytes_per_frame;
			}
			if (f_lseek(&SDFile, wav_data_start + (uint64_t)first_src * wav_bytes_per_frame) != FR_OK ||
			    f_read(&SDFile, WAV_RAW, want, &bytes_read) != FR_OK) {
				for (i = 0; i < num_out_frames; i++) {
					start[i*4] = 0; start[i*4+1] = 0; start[i*4+2] = 0; start[i*4+3] = 0;
				}
				file_ended = 1;
				return;
			}
		}
		uint32_t frames_avail = bytes_read / wav_bytes_per_frame;

		for (i = 0; i < num_out_frames; i++) {
			uint64_t pos = resample_pos + (uint64_t)i * eff_step;
			uint32_t idx = (uint32_t)(pos >> RESAMPLE_FRAC_BITS);
			uint32_t frac = (uint32_t)(pos & RESAMPLE_FRAC_MASK);
			int32_t l0, l1, r0, r1;
			int32_t out_l, out_r;

			if (idx >= wav_total_frames) {
				out_l = 0; out_r = 0;
			} else if (idx + 1 >= wav_total_frames) {
				uint32_t lidx = idx - first_src;
				if (lidx >= frames_avail) { out_l = 0; out_r = 0; }
				else wav_get_frame(WAV_RAW, lidx, &out_l, &out_r);
			} else {
				uint32_t i0 = idx - first_src;
				uint32_t i1 = i0 + 1;
				if (i0 >= frames_avail) { out_l = 0; out_r = 0; }
				else if (i1 >= frames_avail) { wav_get_frame(WAV_RAW, i0, &out_l, &out_r); }
				else {
					wav_get_frame(WAV_RAW, i0, &l0, &r0);
					wav_get_frame(WAV_RAW, i1, &l1, &r1);
					out_l = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) >> RESAMPLE_FRAC_BITS);
					out_r = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) >> RESAMPLE_FRAC_BITS);
				}
			}

			/* 末尾淡出 */
			if (out_frame_count + i >= fade_start) {
				uint32_t fade_idx = out_frame_count + i - fade_start;
				int32_t gain = (int32_t)(FADE_SAMPLES - fade_idx);
				if (gain < 0) gain = 0;
				out_l = (int32_t)(((int64_t)out_l * gain) >> 8);
				out_r = (int32_t)(((int64_t)out_r * gain) >> 8);
			}

			int32_t s32_l = out_l << 8;
			int32_t s32_r = out_r << 8;
			start[i*4]     = (uint16_t)((uint32_t)s32_l >> 16);
			start[i*4+1]   = (uint16_t)((uint32_t)s32_l & 0xFFFF);
			start[i*4+2]   = (uint16_t)((uint32_t)s32_r >> 16);
			start[i*4+3]   = (uint16_t)((uint32_t)s32_r & 0xFFFF);
		}

		/* 推进全局相位 */
		uint32_t fwd_speed = (seek_speed > 0) ? (uint32_t)seek_speed : 1;
		resample_pos += (uint64_t)num_out_frames * eff_step;
		out_frame_count += num_out_frames * fwd_speed;

		if (resample_pos >= ((uint64_t)wav_total_frames << RESAMPLE_FRAC_BITS)) {
			file_ended = 1;
		}
	}
	else
	{
		/* ================= 反向 (快退, 倒放加速) ================= */
		uint32_t first_src = (uint32_t)(resample_pos >> RESAMPLE_FRAC_BITS);
		uint32_t read_frames;
		uint32_t rev_speed = (uint32_t)(-seek_speed);

		if (first_src == 0) {
			/* 已退到开头: 停在开头(填静音), 等按键松开后恢复正常播放 */
			resample_pos = 0;
			for (i = 0; i < num_out_frames; i++) {
				start[i*4] = 0; start[i*4+1] = 0; start[i*4+2] = 0; start[i*4+3] = 0;
			}
			return;
		}

		uint64_t consumed = (uint64_t)num_out_frames * eff_step;
		uint64_t end_pos = (resample_pos > consumed) ? (resample_pos - consumed) : 0;
		uint32_t last_src = (uint32_t)(end_pos >> RESAMPLE_FRAC_BITS);
		uint32_t start_src = (last_src >= 1) ? (last_src - 1) : 0;

		read_frames = first_src - start_src + 1;
		{
			uint32_t want = read_frames * wav_bytes_per_frame;
			if (want > sizeof(WAV_RAW)) {
				want = sizeof(WAV_RAW);
				read_frames = want / wav_bytes_per_frame;
			}
			if (f_lseek(&SDFile, wav_data_start + (uint64_t)start_src * wav_bytes_per_frame) != FR_OK ||
			    f_read(&SDFile, WAV_RAW, want, &bytes_read) != FR_OK) {
				for (i = 0; i < num_out_frames; i++) {
					start[i*4] = 0; start[i*4+1] = 0; start[i*4+2] = 0; start[i*4+3] = 0;
				}
				file_ended = 1;
				return;
			}
		}
		uint32_t frames_avail = bytes_read / wav_bytes_per_frame;

		for (i = 0; i < num_out_frames; i++) {
			uint64_t pos = (resample_pos >= (uint64_t)i * eff_step) ? (resample_pos - (uint64_t)i * eff_step) : 0;
			uint32_t idx = (uint32_t)(pos >> RESAMPLE_FRAC_BITS);
			uint32_t frac = (uint32_t)(pos & RESAMPLE_FRAC_MASK);
			int32_t l0, l1, r0, r1;
			int32_t out_l, out_r;

			if (idx >= wav_total_frames) {
				out_l = 0; out_r = 0;
			} else if (idx + 1 >= wav_total_frames) {
				uint32_t lidx = idx - start_src;
				if (lidx >= frames_avail) { out_l = 0; out_r = 0; }
				else wav_get_frame(WAV_RAW, lidx, &out_l, &out_r);
			} else {
				uint32_t i0 = idx - start_src;
				uint32_t i1 = i0 + 1;
				if (i0 >= frames_avail) { out_l = 0; out_r = 0; }
				else if (i1 >= frames_avail) { wav_get_frame(WAV_RAW, i0, &out_l, &out_r); }
				else {
					wav_get_frame(WAV_RAW, i0, &l0, &r0);
					wav_get_frame(WAV_RAW, i1, &l1, &r1);
					out_l = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) >> RESAMPLE_FRAC_BITS);
					out_r = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) >> RESAMPLE_FRAC_BITS);
				}
			}

			/* 反向播放不做淡出 */
			int32_t s32_l = out_l << 8;
			int32_t s32_r = out_r << 8;
			start[i*4]     = (uint16_t)((uint32_t)s32_l >> 16);
			start[i*4+1]   = (uint16_t)((uint32_t)s32_l & 0xFFFF);
			start[i*4+2]   = (uint16_t)((uint32_t)s32_r >> 16);
			start[i*4+3]   = (uint16_t)((uint32_t)s32_r & 0xFFFF);
		}

		resample_pos = end_pos;
		if (out_frame_count >= num_out_frames * rev_speed) out_frame_count -= num_out_frames * rev_speed;
		else out_frame_count = 0;
	}
}

#if ENABLE_FLAC
/* FLAC 专用填充: 从 flac 解码器解码的 PCM 重采样, 32bit 左对齐写入 I2S 缓冲。
 * 源帧来自 work_buf.flac (解码一帧 block 个样本/声道), 跨帧自动解码下一帧。
 * 输出移位 = 32 - bitsPerSample, 使样本在 32bit 字内左对齐。 */
static void flac_fill_buffer_region(uint16_t *start, uint16_t num_halfwords)
{
	uint32_t num_out = num_halfwords / 4;
	uint32_t produced = 0;
	uint32_t shift = 32 - work_buf.flac.bitsPerSample;
	uint64_t eff_step;   /* 快进时按倍速缩放 (FLAC 仅支持快进, 不支持快退) */
	if (seek_speed > 0)      eff_step = (uint64_t)resample_step * (uint32_t)seek_speed;
	else                     eff_step = resample_step;

	while (produced < num_out) {
		/* SD 读失败(拔卡): 立即收尾, 不要继续解码0填充数据 */
		if (g_sd_io_err) {
			for (; produced < num_out; produced++) {
				start[produced*4] = 0; start[produced*4+1] = 0;
				start[produced*4+2] = 0; start[produced*4+3] = 0;
			}
			file_ended = 1;
			return;
		}
		/* 确保当前解码帧覆盖 resample_pos */
		if (resample_pos >= ((uint64_t)(work_buf.flac.frameStart + work_buf.flac.frameFrames) << RESAMPLE_FRAC_BITS)) {
			if (flac_decoder_decode_frame(&work_buf.flac) != 0) {
				/* 解码结束: 补 0 */
				for (; produced < num_out; produced++) {
					start[produced*4] = 0; start[produced*4+1] = 0;
					start[produced*4+2] = 0; start[produced*4+3] = 0;
				}
				file_ended = 1;
				return;
			}
		}

		while (produced < num_out &&
		       resample_pos < ((uint64_t)(work_buf.flac.frameStart + work_buf.flac.frameFrames) << RESAMPLE_FRAC_BITS)) {
			uint32_t idx = (uint32_t)(resample_pos >> RESAMPLE_FRAC_BITS);
			uint32_t frac = (uint32_t)(resample_pos & RESAMPLE_FRAC_MASK);
			uint32_t off = idx - work_buf.flac.frameStart;
			int32_t l0, l1, r0, r1, out_l, out_r;

			l0 = work_buf.flac.pcm[off];
			r0 = (work_buf.flac.channels == 2) ? work_buf.flac.pcm[FLAC_MAX_BLOCK + off] : l0;
			if (off + 1 < work_buf.flac.frameFrames) {
				l1 = work_buf.flac.pcm[off + 1];
				r1 = (work_buf.flac.channels == 2) ? work_buf.flac.pcm[FLAC_MAX_BLOCK + off + 1] : l1;
				out_l = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) >> RESAMPLE_FRAC_BITS);
				out_r = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) >> RESAMPLE_FRAC_BITS);
			} else {
				out_l = l0;
				out_r = r0;
			}

			/* 左对齐输出 (无符号移位避免符号位溢出) */
			uint32_t s32_l = (uint32_t)out_l << shift;
			uint32_t s32_r = (uint32_t)out_r << shift;
			start[produced*4]   = (uint16_t)(s32_l >> 16);
			start[produced*4+1] = (uint16_t)(s32_l & 0xFFFF);
			start[produced*4+2] = (uint16_t)(s32_r >> 16);
			start[produced*4+3] = (uint16_t)(s32_r & 0xFFFF);

			resample_pos += eff_step;
			produced++;
		}
	}
}

#endif /* ENABLE_FLAC */

/* 解析 WAV 头 (兼容非标准 44 字节头, 支持 WAVE_FORMAT_EXTENSIBLE)
 * 成功返回 0, 文件指针停留在 data 块数据起点; 失败返回负值错误码。 */
int parse_wav_header(FIL *file, wav_file_parameters *header) {
    unsigned int br;
    uint8_t  hdr[12];
    uint8_t  fmtFull[64];
    uint32_t fmtSize, csize, fmtUsed;
    uint8_t  cid[8];
    uint16_t audioFormat;

    /* 1. RIFF / WAVE 头 */
    if (f_read(file, hdr, 12, &br) != FR_OK || br != 12) return -1;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return -2;
    memcpy(header->riff, hdr, 4);
    memcpy(header->wave, hdr + 8, 4);
    header->fileSize = (uint32_t)hdr[4] | ((uint32_t)hdr[5]<<8) | ((uint32_t)hdr[6]<<16) | ((uint32_t)hdr[7]<<24);

    /* 2. fmt 块 */
    if (f_read(file, hdr, 8, &br) != FR_OK || br != 8) return -1;
    if (memcmp(hdr, "fmt ", 4) != 0) return -2;
    fmtSize = (uint32_t)hdr[4] | ((uint32_t)hdr[5]<<8) | ((uint32_t)hdr[6]<<16) | ((uint32_t)hdr[7]<<24);
    memcpy(header->fmt, hdr, 4);
    header->fmtLen = fmtSize;
    if (fmtSize < 16) return -3;

    fmtUsed = (fmtSize > sizeof(fmtFull)) ? sizeof(fmtFull) : fmtSize;
    if (f_read(file, fmtFull, fmtUsed, &br) != FR_OK || br != fmtUsed) return -1;

    audioFormat = (uint16_t)(fmtFull[0] | (fmtFull[1] << 8));
    if (audioFormat == 0xFFFE) {
        /* WAVE_FORMAT_EXTENSIBLE: 实际格式在 SubFormat GUID 前两字节, 1=PCM */
        if (fmtSize < 40 || fmtFull[24] != 0x01 || fmtFull[25] != 0x00) return -3;
        audioFormat = 1;   /* 视作 PCM */
    }
    if (audioFormat != 1) return -3;   /* 仅支持 PCM */

    header->audioFormat  = 1;
    header->numChannels  = (uint16_t)(fmtFull[2] | (fmtFull[3] << 8));
    header->sampleRate   = (uint32_t)fmtFull[4]  | ((uint32_t)fmtFull[5]<<8) | ((uint32_t)fmtFull[6]<<16) | ((uint32_t)fmtFull[7]<<24);
    header->byteRate     = (uint32_t)fmtFull[8]  | ((uint32_t)fmtFull[9]<<8) | ((uint32_t)fmtFull[10]<<16) | ((uint32_t)fmtFull[11]<<24);
    header->blockAlign   = (uint16_t)(fmtFull[12] | (fmtFull[13] << 8));
    header->bitsPerSample= (uint16_t)(fmtFull[14] | (fmtFull[15] << 8));

    /* 跳过 fmt 块剩余部分 */
    if (fmtSize > fmtUsed) {
        if (f_lseek(file, f_tell(file) + (fmtSize - fmtUsed)) != FR_OK) return -1;
    }

    /* 3. 依次扫描块, 定位 data 块 */
    for (;;) {
        if (f_read(file, cid, 8, &br) != FR_OK || br != 8) return -1;
        csize = (uint32_t)cid[4] | ((uint32_t)cid[5]<<8) | ((uint32_t)cid[6]<<16) | ((uint32_t)cid[7]<<24);
        if (memcmp(cid, "data", 4) == 0) {
            memcpy(header->data, cid, 4);
            header->dataSize = csize;
            return 0;    /* 文件指针停在 data 数据起点, wav_data_start 直接用 f_tell */
        }
        /* 跳过其它块 (块长按偶对齐) */
        if (f_lseek(file, f_tell(file) + csize + (csize & 1)) != FR_OK) return -1;
        if (f_tell(file) >= f_size(file)) return -2;
    }
}

#if ENABLE_MP3
/* 解码下一帧 MP3。成功返回每声道样本数(1152/576), 失败/结束返回 -1。 */
static int mp3_decode_next_frame(void)
{
	uint32_t remaining;
	UINT br;
	unsigned char *ptr;
	int left, off, err;
	MP3FrameInfo info;

	/* 防御: 缓冲状态异常时重置 */
	if (mp3_inbuf_len > sizeof(mp3_inbuf) || mp3_inbuf_pos > mp3_inbuf_len) {
		mp3_inbuf_len = 0; mp3_inbuf_pos = 0;
	}

	for (;;) {
		remaining = mp3_inbuf_len - mp3_inbuf_pos;

		/* 补数据: 不足"整帧+bit reservoir"(~1940 字节)时先补 */
		if (remaining < 1940 && !mp3_eof) {
			if (mp3_inbuf_pos > 0) {
				memmove(mp3_inbuf, mp3_inbuf + mp3_inbuf_pos, remaining);
				mp3_inbuf_len = remaining; mp3_inbuf_pos = 0;
			}
			if (mp3_inbuf_len < sizeof(mp3_inbuf)) {
				uint32_t want = sizeof(mp3_inbuf) - mp3_inbuf_len;
				if (f_read(&SDFile, mp3_inbuf + mp3_inbuf_len, want, &br) != FR_OK || br == 0) {
					mp3_eof = 1;
				} else {
					mp3_inbuf_len += br;
				}
			}
			remaining = mp3_inbuf_len - mp3_inbuf_pos;
		}

		if (remaining < 4) return -1;   /* 数据不足(文件尾) */

		/* 找同步字 */
		off = MP3FindSyncWord(mp3_inbuf + mp3_inbuf_pos, (int)remaining);
		if (off < 0) {
			if (mp3_eof) return -1;
			mp3_inbuf_pos = (mp3_inbuf_len > 2) ? (mp3_inbuf_len - 2) : mp3_inbuf_len;
			continue;
		}
		mp3_inbuf_pos += (uint32_t)off;

		/* 关键: 同步字后必须凑够整帧(含 reservoir), 否则整帧数据不全。
		 * 把从同步字开始的数据移到缓冲头再补满, 然后重找同步字。 */
		if ((mp3_inbuf_len - mp3_inbuf_pos) < 1940 && !mp3_eof) {
			uint32_t have = mp3_inbuf_len - mp3_inbuf_pos;
			memmove(mp3_inbuf, mp3_inbuf + mp3_inbuf_pos, have);
			mp3_inbuf_len = have; mp3_inbuf_pos = 0;
			if (have < sizeof(mp3_inbuf)) {
				uint32_t want = sizeof(mp3_inbuf) - have;
				if (f_read(&SDFile, mp3_inbuf + have, want, &br) != FR_OK || br == 0) mp3_eof = 1;
				else mp3_inbuf_len += br;
			}
			continue;
		}

		ptr = mp3_inbuf + mp3_inbuf_pos;
		left = (int)(mp3_inbuf_len - mp3_inbuf_pos);
		err = MP3Decode(mp3_hdec, &ptr, &left, mp3_pcm, 0);
		if (err == 0) {
			MP3GetLastFrameInfo(mp3_hdec, &info);
			mp3_inbuf_pos = (uint32_t)(ptr - mp3_inbuf);
			mp3_channels = (uint32_t)info.nChans;
			if (mp3_hz == 0) mp3_hz = (uint32_t)info.samprate;
			mp3_frame_start = mp3_total_frames;
			mp3_frame_frames = (uint32_t)(info.outputSamps / (info.nChans ? info.nChans : 1));
			mp3_total_frames += mp3_frame_frames;
			return (int)mp3_frame_frames;
		}
		/* 数据不足类错误: 回循环顶补数据再试(不跳字节, 否则流错位) */
		if ((err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) && !mp3_eof) {
			continue;
		}
		/* 其它错误: 跳 1 字节重同步 */
		mp3_inbuf_pos++;
		if (mp3_inbuf_pos >= mp3_inbuf_len) {
			if (mp3_eof) return -1;
		}
	}
}

/* MP3 专用填充: 解码 + 重采样, 16bit -> 32bit 左对齐写入 I2S 缓冲。 */
static void mp3_fill_buffer_region(uint16_t *start, uint16_t num_halfwords)
{
	uint32_t num_out = num_halfwords / 4;
	uint32_t produced = 0;
	uint64_t eff_step = (seek_speed > 0) ? (uint64_t)resample_step * (uint32_t)seek_speed : resample_step;

	while (produced < num_out) {
		/* SD 读失败(拔卡): 立即收尾 */
		if (g_sd_io_err) {
			for (; produced < num_out; produced++) {
				start[produced*4]=0; start[produced*4+1]=0;
				start[produced*4+2]=0; start[produced*4+3]=0;
			}
			file_ended = 1;
			return;
		}
		/* 确保当前帧覆盖 resample_pos */
		if (resample_pos >= ((uint64_t)(mp3_frame_start + mp3_frame_frames) << RESAMPLE_FRAC_BITS)) {
			/* 注意: mp3_decode_next_frame 成功返回样本数(>0), 失败/结束返回 -1。
			 * 不能写 != 0 (那样每帧成功都会被当成结束) —— 这里必须 < 0。 */
			if (mp3_decode_next_frame() < 0) {
				/* 解码结束: 补 0 */
				for (; produced < num_out; produced++) {
					start[produced*4]=0; start[produced*4+1]=0;
					start[produced*4+2]=0; start[produced*4+3]=0;
				}
				file_ended = 1;
				return;
			}
		}
		while (produced < num_out &&
		       resample_pos < ((uint64_t)(mp3_frame_start + mp3_frame_frames) << RESAMPLE_FRAC_BITS)) {
			uint32_t idx = (uint32_t)(resample_pos >> RESAMPLE_FRAC_BITS);
			uint32_t frac = (uint32_t)(resample_pos & RESAMPLE_FRAC_MASK);
			uint32_t off = idx - mp3_frame_start;
			int32_t l0, l1, r0, r1, out_l, out_r;

			l0 = mp3_pcm[off * mp3_channels];
			r0 = (mp3_channels == 2) ? mp3_pcm[off*2 + 1] : l0;
			if (off + 1 < mp3_frame_frames) {
				l1 = mp3_pcm[(off+1) * mp3_channels];
				r1 = (mp3_channels == 2) ? mp3_pcm[(off+1)*2 + 1] : l1;
				out_l = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) >> RESAMPLE_FRAC_BITS);
				out_r = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) >> RESAMPLE_FRAC_BITS);
			} else {
				out_l = l0; out_r = r0;
			}
			/* 16bit MP3 -> 32bit 左对齐 (<<16) */
			uint32_t s32_l = (uint32_t)(int32_t)(out_l << 16);
			uint32_t s32_r = (uint32_t)(int32_t)(out_r << 16);
			start[produced*4]   = (uint16_t)(s32_l >> 16);
			start[produced*4+1] = (uint16_t)(s32_l & 0xFFFF);
			start[produced*4+2] = (uint16_t)(s32_r >> 16);
			start[produced*4+3] = (uint16_t)(s32_r & 0xFFFF);

			resample_pos += eff_step;
			produced++;
		}
	}
}

void mp3_file_process(file_list *ado_file)
{
	char full_path[128] = {0};
	uint32_t actual_rate;

	sprintf(full_path, "0:/%s", ado_file[current_index].audio_file_names);
	res = f_open(&SDFile, full_path, FA_READ);
	if (res != FR_OK) {
		mp3_play_ok = 0;
		OLED_Clear();
		OLED_ShowString(1, 1, "MP3打开失败");
		OLED_ShowString(4, 1, "播放=跳过");
		osSemaphoreRelease(LOAD_OR_NOTHandle);
		play_cmd = CMD_NONE;
		{ uint32_t t0 = HAL_GetTick(); while (play_cmd == CMD_NONE) { osDelay(20); if ((HAL_GetTick()-t0) > 20000) break; } if (play_cmd == CMD_NONE) play_cmd = CMD_NEXT; }
		return;
	}

	/* 初始化 Helix 解码器 (内存池用 CCM union, 与 FLAC/WAV 互斥复用) */
	helix_pool_init(work_buf.helix_pool, sizeof(work_buf.helix_pool));
	mp3_hdec = MP3InitDecoder();
	mp3_inbuf_len = 0; mp3_inbuf_pos = 0; mp3_eof = 0;
	mp3_hz = 0; mp3_channels = 2;
	mp3_total_frames = 0; mp3_frame_start = 0; mp3_frame_frames = 0;
	if (!mp3_hdec) {
		mp3_play_ok = 0;
		OLED_Clear();
		OLED_ShowString(1, 1, "解码器内存不足");
		OLED_ShowString(4, 1, "播放=跳过");
		f_close(&SDFile);
		play_cmd = CMD_NONE;
		{ uint32_t t0 = HAL_GetTick(); while (play_cmd == CMD_NONE) { osDelay(20); if ((HAL_GetTick()-t0) > 20000) break; } if (play_cmd == CMD_NONE) play_cmd = CMD_NEXT; }
		return;
	}
	{
		UINT br;
		uint32_t tagsz = 0;
		if (f_read(&SDFile, mp3_inbuf, sizeof(mp3_inbuf), &br) == FR_OK && br > 0) {
			mp3_inbuf_len = br; mp3_inbuf_pos = 0;
		}
		/* 跳过 ID3v2 标签 (synchsafe 长度) */
		if (mp3_inbuf_len >= 10 && mp3_inbuf[0]=='I' && mp3_inbuf[1]=='D' && mp3_inbuf[2]=='3') {
			tagsz = ((uint32_t)(mp3_inbuf[6] & 0x7F) << 21) |
			        ((uint32_t)(mp3_inbuf[7] & 0x7F) << 14) |
			        ((uint32_t)(mp3_inbuf[8] & 0x7F) << 7)  |
			        ((uint32_t)(mp3_inbuf[9] & 0x7F));
			tagsz += 10;   /* 加上 10 字节标签头 */
			if (mp3_inbuf_pos + tagsz < mp3_inbuf_len) {
				mp3_inbuf_pos = tagsz;   /* 标签在缓冲内, 直接跳过 */
			} else {
				/* 标签超缓冲: 用 f_read 顺序读跳过。
				 * 已读 mp3_inbuf_len 字节(在标签内), 只需再跳 tagsz - mp3_inbuf_len 字节到标签末尾。 */
				uint32_t remain_skip = (tagsz > mp3_inbuf_len) ? (tagsz - mp3_inbuf_len) : 0;
				uint8_t tmp[256];
				while (remain_skip > 0) {
					uint32_t want = (remain_skip > sizeof(tmp)) ? sizeof(tmp) : remain_skip;
					UINT br2;
					if (f_read(&SDFile, tmp, want, &br2) != FR_OK || br2 == 0) break;
					remain_skip -= br2;
				}
				mp3_inbuf_len = 0; mp3_inbuf_pos = 0;
			}
		}
		{
			int d_rc = mp3_decode_next_frame();
			if (d_rc <= 0 || mp3_hz == 0) {
				mp3_play_ok = 0;
				OLED_Clear();
				OLED_ShowString(1, 1, "MP3失败");
				OLED_ShowString(4, 1, "播放=跳过");
				MP3FreeDecoder(mp3_hdec);
				f_close(&SDFile);
				play_cmd = CMD_NONE;
				{
					uint32_t t0 = HAL_GetTick();
					while (play_cmd == CMD_NONE) { osDelay(20); if ((HAL_GetTick()-t0) > 20000) break; }
					if (play_cmd == CMD_NONE) play_cmd = CMD_NEXT;   /* 超时自动跳下一首 */
				}
				return;
			}
		}
	}

	/* 显示采样率/声道 */
	{
		uint8_t sr_len;
		OLED_Clear();
		OLED_ShowString(2, 1, "采样率:");
		sr_len = (mp3_hz >= 10000) ? 5 : ((mp3_hz >= 1000) ? 4 : 3);
		OLED_ShowNum(2, 8, mp3_hz, sr_len);
		OLED_ShowString(2, 8+sr_len, "Hz");
		OLED_ShowString(3, 1, "声道:");
		OLED_ShowNum(3, 5, mp3_channels, 1);
		OLED_ShowString(4, 1, "位深:");
		OLED_ShowString(4, 5, "16位");
		{ static char song_utf8[256]; OLED_GBKString_To_UTF8(ado_file[current_index].audio_file_names, song_utf8); OLED_ShowScrollingString(song_utf8); }
	}

	/* 播放初始化 */
	seek_speed = 0;
	actual_rate = i2s_set_sample_rate(mp3_hz);
	resample_step = (uint32_t)(((uint64_t)mp3_hz << RESAMPLE_FRAC_BITS) / actual_rate);
	resample_pos = 0;
	force_i2s_config();
	mp3_fill_buffer_region(audio_buffer[0], AUDIO_BUF_SIZE);
	mp3_fill_buffer_region(audio_buffer[1], AUDIO_BUF_SIZE);
	if (file_ended) {
		/* 预填解码失败(后续帧解不出) */
		mp3_play_ok = 0;
		WM8960_BeforeClockStop();   /* ★手册 p69: 停 MCLK 前先软静音 + 关 DAC 并等 >=1ms(内部含延时) */
	HAL_I2S_DMAStop(&hi2s2);
		MP3FreeDecoder(mp3_hdec);
		f_close(&SDFile);
		OLED_Clear();
		OLED_ShowString(1, 1, "MP3预填失败");
		OLED_ShowString(4, 1, "播放=跳过");
		play_cmd = CMD_NONE;
		{ uint32_t t0 = HAL_GetTick(); while (play_cmd == CMD_NONE) { osDelay(20); if ((HAL_GetTick()-t0) > 20000) break; } if (play_cmd == CMD_NONE) play_cmd = CMD_NEXT; }
		return;
	}
	half_ready = 0; full_ready = 0;
	play_state = STATE_PLAYING;
	HAL_I2S_Transmit_DMA(&hi2s2, &audio_buffer[0][0], AUDIO_BUF_SIZE);
	WM8960_AfterClockStart();   /* ★时钟已稳(且在起 DMA 之后): 按手册反序恢复 codec(内部含延时) */

	/* 播放循环 */
	while (!file_ended && play_state != STATE_STOPPED)
	{
		if (play_cmd != CMD_NONE)
		{
			switch (play_cmd)
			{
				case CMD_PREV: case CMD_NEXT: file_ended = 1; break;
				case CMD_PAUSE: { audio_pause(); play_cmd = CMD_NONE; OLED_Clear(); OLED_ShowString(1, 1, "暂停!"); break; }
				case CMD_RESUME: { audio_resume(); play_cmd = CMD_NONE; OLED_Clear(); {static char song_utf8[256]; OLED_GBKString_To_UTF8(ado_file[current_index].audio_file_names, song_utf8); OLED_ShowScrollingString(song_utf8);} break; }
				case CMD_STOP: { audio_stop(); play_cmd = CMD_NONE; OLED_Clear(); break; }
				default: break;
			}
		}
		if (play_state == STATE_PLAYING)
		{
			if (usb_sd_occupied) { audio_stop(); file_ended = 1; break; }
			if (g_sd_io_err) { g_sd_io_err = 0; audio_stop(); file_ended = 1; break; }
			if (seek_speed < 0) seek_speed = 0;   /* MP3 不支持快退 */
			seek_update_display(ado_file);
			if (half_ready) { mp3_fill_buffer_region(audio_buffer[0], AUDIO_BUF_SIZE); half_ready = 0; }
			if (full_ready) { mp3_fill_buffer_region(audio_buffer[1], AUDIO_BUF_SIZE); full_ready = 0; }
			if (!half_ready && !full_ready) osDelay(1);
		}
	}

	mp3_play_ok = 1;   /* 正常播放结束 */
	WM8960_BeforeClockStop();   /* ★手册 p69: 停 MCLK 前先软静音 + 关 DAC 并等 >=1ms(内部含延时) */
	HAL_I2S_DMAStop(&hi2s2);
	MP3FreeDecoder(mp3_hdec);
	f_close(&SDFile);
	if (play_cmd == CMD_PREV) current_index = (current_index > 0) ? current_index - 1 : file_count - 1;
	else if (play_cmd == CMD_NEXT) current_index = (current_index < (file_count - 1)) ? current_index + 1 : 0;
}
#else /* ENABLE_MP3 == 0 */
/* MP3 未启用: 提示后跳过 */
void mp3_file_process(file_list *ado_file)
{
	OLED_ShowString(1, 1, "MP3未启用");
	osSemaphoreRelease(LOAD_OR_NOTHandle);
}
#endif /* ENABLE_MP3 */

#if ENABLE_FLAC
void flac_file_process(file_list *ado_file)
{
	char full_path[128] = {0};
	uint32_t actual_rate;
	int flac_rc;

	sprintf(full_path, "0:/%s", ado_file[current_index].audio_file_names);
	res = f_open(&SDFile, full_path, FA_READ);
	if (res != FR_OK) {
		OLED_ShowString(1, 1, "打开失败!");
		osSemaphoreRelease(LOAD_OR_NOTHandle);
		return;
	}

	/* 解析 STREAMINFO, 定位首帧 */
	flac_rc = flac_decoder_open(&work_buf.flac, &SDFile);
	if (flac_rc != 0) {
		/* 打开失败: 明确显示原因并停在提示, 等用户按播放键(跳过)或停止键(返回) */
		OLED_Clear();
		OLED_ShowString(1, 1, "FLAC不支持!");
		if (flac_rc == -2) {
			OLED_ShowString(2, 1, "压缩块超4608");
			OLED_ShowString(3, 1, "需转码block4608");
			OLED_ShowString(4, 1, "按播放键跳过");
		} else {
			OLED_ShowString(2, 1, "格式不支持");
			OLED_ShowString(3, 1, "按播放键跳过");
		}
		f_close(&SDFile);
		/* 等待用户操作: 播放/上一首/下一首=跳过, 停止=返回列表 */
		play_cmd = CMD_NONE;
		{
			uint32_t t0 = HAL_GetTick();
			while (play_cmd == CMD_NONE) {
				osDelay(20);
				if ((HAL_GetTick() - t0) > 20000) break;   /* 20s 超时 */
			}
			play_cmd = CMD_NONE;
		}
		return;
	}

	/* 播放初始化: 复位变速状态 + I2S 分频 + 重采样步进 */
	seek_speed = 0;
	actual_rate = i2s_set_sample_rate(work_buf.flac.sampleRate);
	resample_step = (uint32_t)(((uint64_t)work_buf.flac.sampleRate << RESAMPLE_FRAC_BITS) / actual_rate);
	resample_pos = 0;

	/* 播放前显示采样频率/声道/实际输出率 */
	{
		uint8_t sr_len, bits_len;
		OLED_Clear();
		OLED_ShowString(2, 1, "采样率:");
		sr_len = (work_buf.flac.sampleRate >= 10000) ? 5 : ((work_buf.flac.sampleRate >= 1000) ? 4 : 3);
		OLED_ShowNum(2, 8, work_buf.flac.sampleRate, sr_len);
		OLED_ShowString(2, 8 + sr_len, "Hz");
		OLED_ShowString(3, 1, "声道:");
		OLED_ShowNum(3, 5, work_buf.flac.channels, 1);
		/* 第4行: 位深 */
		{
			OLED_ShowString(4, 1, "位深:");
			OLED_ShowNum(4, 5, work_buf.flac.bitsPerSample, 2);
			OLED_ShowString(4, 7, "位");
		}
		/* 歌名滚动显示 */
		{
			static char song_utf8[256];
			OLED_GBKString_To_UTF8(ado_file[current_index].audio_file_names, song_utf8);
			OLED_ShowScrollingString(song_utf8);
		}
	}
	force_i2s_config();
	flac_fill_buffer_region(audio_buffer[0], AUDIO_BUF_SIZE);
	flac_fill_buffer_region(audio_buffer[1], AUDIO_BUF_SIZE);
	half_ready = 0;
	full_ready = 0;
	play_state = STATE_PLAYING;
	HAL_I2S_Transmit_DMA(&hi2s2, &audio_buffer[0][0], AUDIO_BUF_SIZE);
	WM8960_AfterClockStart();   /* ★时钟已稳(且在起 DMA 之后): 按手册反序恢复 codec(内部含延时) */

	/* 播放循环 */
	while (!file_ended && play_state != STATE_STOPPED)
	{
		if (play_cmd != CMD_NONE)
		{
			switch (play_cmd)
			{
				case CMD_PREV: case CMD_NEXT: file_ended = 1; break;
				case CMD_PAUSE: { audio_pause(); play_cmd = CMD_NONE; OLED_Clear(); OLED_ShowString(1, 1, "暂停!"); break; }
				case CMD_RESUME: { audio_resume(); play_cmd = CMD_NONE; OLED_Clear(); {static char song_utf8[256]; OLED_GBKString_To_UTF8(ado_file[current_index].audio_file_names, song_utf8); OLED_ShowScrollingString(song_utf8);} break; }
				case CMD_STOP: { audio_stop(); play_cmd = CMD_NONE; OLED_Clear(); break; }
				default: break;
			}
		}
		if (play_state == STATE_PLAYING)
		{
			if (usb_sd_occupied)
			{
				audio_stop();
				file_ended = 1;
				break;
			}
			/* SD 读失败(很可能拔卡): 立即停止, 回到上层会检测无卡并提示 */
			if (g_sd_io_err) { g_sd_io_err = 0; audio_stop(); file_ended = 1; break; }
			/* FLAC 不支持快退, 负倍速忽略 */
			if (seek_speed < 0) seek_speed = 0;
			seek_update_display(ado_file);
			if (half_ready) { flac_fill_buffer_region(audio_buffer[0], AUDIO_BUF_SIZE); half_ready = 0; }
			if (full_ready) { flac_fill_buffer_region(audio_buffer[1], AUDIO_BUF_SIZE); full_ready = 0; }
			if (!half_ready && !full_ready)
			{
				osDelay(1);
			}
		}
	}
	WM8960_BeforeClockStop();   /* ★手册 p69: 停 MCLK 前先软静音 + 关 DAC 并等 >=1ms(内部含延时) */
	HAL_I2S_DMAStop(&hi2s2);
	f_close(&SDFile);
	if (play_cmd == CMD_PREV) current_index = (current_index > 0) ? current_index - 1 : file_count - 1;
	else if (play_cmd == CMD_NEXT) current_index = (current_index < (file_count - 1)) ? current_index + 1 : 0;
}

#else /* ENABLE_FLAC == 0 */
/* FLAC 未启用: 显示提示后等待用户按播放键(跳过)或停止键(返回) */
void flac_file_process(file_list *ado_file)
{
	(void)ado_file;
	OLED_Clear();
	OLED_ShowString(1, 1, "FLAC未启用");
	OLED_ShowString(2, 1, "仅支持WAV");
	OLED_ShowString(3, 1, "按播放键跳过");
	play_cmd = CMD_NONE;
	{
		uint32_t t0 = HAL_GetTick();
		while (play_cmd == CMD_NONE) {
			osDelay(20);
			if ((HAL_GetTick() - t0) > 20000) break;
		}
		play_cmd = CMD_NONE;
	}
}
#endif /* ENABLE_FLAC */

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
	WM8960_BeforeClockStop();   /* ★手册 p69: 停 MCLK 前先软静音 + 关 DAC 并等 >=1ms(内部含延时) */
	HAL_I2S_DMAStop(&hi2s2);   // 停止 DMA，关闭外设
    play_state = STATE_STOPPED;
}

void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
	half_ready = 1;
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
	full_ready = 1;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{

	if(GPIO_Pin == GPIO_PIN_0) {
		/* 上一首: 捕获按下(去抖), 短按/长按由滚动定时器轮询判定 (长按=快退)。
		 * 去抖: 防机械抖动多个下降沿被当成多次按下(会连续跳好几首)。
		 * ★上电 1s 内忽略(防上电毛刺); ★电平确认: 真按下时 ISR 读回必为低,
		 *   毛刺在 ISR 执行时已回到高 → 直接丢掉(不更新 k0_last_tick, 以便抖动沿重试)。 */
		static uint32_t k0_last_tick = 0;
		uint32_t k0_now = HAL_GetTick();
		if (k0_now > KEY_STARTUP_MS
		    && HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_0) == GPIO_PIN_RESET
		    && (k0_now - k0_last_tick) >= 50) {
			k0_last_tick = k0_now;
			key_prev_down = 1;
			key_prev_tick = k0_now;
		}
	}
	else if(GPIO_Pin == GPIO_PIN_1) {
		/* 下一首: 捕获按下(去抖), 短按/长按由滚动定时器轮询判定 (长按=快进)
		 * ★同 PF0: 上电 1s 内忽略 + 电平确认 —— 这条就是"一上电跳过第一首歌"的源头 */
		static uint32_t k1_last_tick = 0;
		uint32_t k1_now = HAL_GetTick();
		if (k1_now > KEY_STARTUP_MS
		    && HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_1) == GPIO_PIN_RESET
		    && (k1_now - k1_last_tick) >= 50) {
			k1_last_tick = k1_now;
			key_next_down = 1;
			key_next_tick = k1_now;
		}
	}
	else if(GPIO_Pin == GPIO_PIN_2){
		/* 按键3 播放/暂停 (PF2)。必须去抖:
		 * 机械按键按下瞬间会产生多个下降沿(抖动), 若不去抖:
		 * 第1个沿让"开始播放", 后续抖动沿在状态已变成 PLAYING 后又发一次 → 被当成暂停,
		 * 表现为"按一下开始播放→立刻又暂停, 得再按一次才播放"。 */
		static uint32_t k2_last_tick = 0;
		uint32_t k2_now = HAL_GetTick();
		if (k2_now > KEY_STARTUP_MS && (k2_now - k2_last_tick) >= 200) {   /* ★上电 1s 内忽略; 200ms 去抖窗口 */
			k2_last_tick = k2_now;
			if (HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_2) == GPIO_PIN_RESET) {   /* F407: 暂停键 PF2 */
				if (play_state == STATE_PLAYING)      play_cmd = CMD_PAUSE;
				else if (play_state == STATE_PAUSED)  play_cmd = CMD_RESUME;
				else if (play_state == STATE_STOPPED) play_cmd = CMD_RESUME;
			}
		}
	}
	else if(GPIO_Pin == GPIO_PIN_5) {
		/* PF5: 菜单键(现暂作"停止"键); 原为 PF3, 现 PF3/PF4 预留给菜单上下选择。
		 * ★双沿 + 最小时长过滤(原来是"只认下降沿就立刻 CMD_STOP", 毫无防毛刺能力):
		 *   读回电平为低 = 刚才发生的是下降沿 → 记下按下起点(抖动期间的后续下降沿不重置起点);
		 *   读回电平为高 = 刚才发生的是上升沿  → 结算时长, >= STOP_KEY_MIN_MS 才算真按键。
		 *   音量键一按就是一串 I2C + CPU 突发, 本板电源/地耦合会在 PF5(仅内部上拉)上
		 *   产生 µs~ms 级毛刺 —— 这就是"按音量键有概率停歌"的来源(已实测确认)。 */
		if (HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_5) == GPIO_PIN_RESET) {
			if (!key_stop_down && HAL_GetTick() > KEY_STARTUP_MS) {   /* ★上电 1s 内忽略毛刺 */
				key_stop_down = 1;
				key_stop_tick = HAL_GetTick();
			}
		}
		else if (key_stop_down) {
			uint32_t dur = HAL_GetTick() - key_stop_tick;
			key_stop_down = 0;
			if (dur >= STOP_KEY_MIN_MS) {
				play_cmd = CMD_STOP;       /* 真按键(按住 >= STOP_KEY_MIN_MS): 执行停止 */
			}
		}
	}
	/* PF3/PF4 (菜单上下选择) / PF6 (返回键) 预留, 暂不处理 */
}


