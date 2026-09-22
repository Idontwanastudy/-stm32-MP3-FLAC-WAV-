/*
 * wm8960.h - WM8960 立体声 CODEC 驱动 (仅实现: I2S 从模式 + DAC → 耳机输出)
 *
 * 说明: WM8960 与 PCM5102a 不同, 它是 **I2C 寄存器控制** 的 codec,
 *       上电后所有关键通路(VREF/VMID/DAC/输出驱动/混音器/音量)默认都是关闭的,
 *       必须由 MCU 通过 I2C 写一串初始化寄存器才会出声。
 *
 * 控制接口: SDIN/SCLK 接 I2C1 (与 OLED 共用总线, 地址不冲突: WM8960=0x34, OLED=0x78)
 */
#ifndef __WM8960_H
#define __WM8960_H

#include "main.h"

/* WM8960 I2C 从地址: 数据手册 7 位地址 0x1A, STM32 HAL 需左移 1 位 → 0x34 */
#define WM8960_I2C_ADDR   (0x1A << 1)

/* 写一个寄存器: reg=7位寄存器地址(0x00~0x37), val=9位数据(0x000~0x1FF) */
void WM8960_WriteReg(uint8_t reg, uint16_t val);

/* 初始化 WM8960 (耳机播放通路)。返回 1=成功, 0=I2C 无应答(查接线/供电/地址) */
uint8_t WM8960_Init(void);

/* ---- 音量控制 (耳机输出) ----
 * vol 取 0~100, 线性映射到 -60dB(0) ~ 0dB(100)。默认约 -12dB。
 * 音量键 PF7(加)/PF8(减) 调用 VolumeUp/Down 即可。 */
void    WM8960_SetVolume(uint8_t vol);   /* 0~100 */
uint8_t WM8960_GetVolume(void);          /* 返回当前 0~100 */
void    WM8960_VolumeUp(void);           /* 音量 +1 档(约 +3dB) */
void    WM8960_VolumeDown(void);         /* 音量 -1 档(约 -3dB) */

/* ---- DAC 数字软静音 ----
 * WM8960 的所有内部时钟都来自 SYSCLK, 而 SYSCLK 只能来自 MCLK 或 PLL(参考仍是 MCLK),
 * 所以 MCLK 一停, 它内部全部时钟都会停(用片内 PLL 也救不了)。
 * 而本工程切歌时 HAL_I2S_DMAStop() 会关掉 I2S 外设 → MCLK 消失, 这属于手册说的
 * "停止主时钟", 必须按手册的时序做(见下面两个函数), 光靠软静音不够。 */
void    WM8960_DacMute(void);            /* R5 DACMU=1 数字软静音 */
void    WM8960_DacUnmute(void);          /* R5 DACMU=0 取消静音 */

/* ---- 切歌/停机时的 codec 上下电时序 (严格按数据手册) ----
 * 依据 WM8960 Rev4.4 "STOPPING THE MASTER CLOCK" (p69):
 *   "Before DIGENB can be set, the control bits ADCL, ADCR, DACL and DACR must be set to
 *    zero and a waiting time of 1ms must be observed. Any failure to follow this procedure
 *    may prevent DACs and ADCs from re-starting correctly."
 * 用法(必须成对, 且不能打乱 HAL 的 I2S 启停顺序):
 *   WM8960_BeforeClockStop();     // 内部含延时
 *   HAL_I2S_DMAStop(&hi2s2);      // 此后 MCLK 才停
 *   ...
 *   HAL_I2S_Transmit_DMA(...);    // 时钟先跑起来
 *   WM8960_AfterClockStart();     // 再恢复 codec (内部含延时) */
void    WM8960_BeforeClockStop(void);
void    WM8960_AfterClockStart(void);

#endif /* __WM8960_H */
