/*
 * wm8960.c - WM8960 CODEC 驱动 (I2S 从模式 + DAC → 耳机输出)
 *
 * 参考: WM8960 Rev 4.4 数据手册 寄存器表(REGISTER MAP) 与 2-Wire 控制接口一节。
 *
 * 本项目用法: STM32 作 I2S 主机(Philips 标准/24bit), WM8960 作从机,
 *             MCLK = 256 x fs (由 I2S2_MCK / PC6 提供), 音频从耳机口输出。
 */
#include "wm8960.h"
#include "i2c.h"
#include "cmsis_os.h"

extern I2C_HandleTypeDef hi2c1;
/* WM8960 与 OLED 共用 I2C1, 必须用同一个互斥量保护总线(否则并发访问会把 I2C 时序打乱) */
extern osMutexId_t oled_mutexHandle;

/* ---------- 用到的寄存器地址 (7 位) ---------- */
#define WM8960_REG_LOUT1_VOL    0x02   /* 左耳机音量 (OUT1VU[8] LO1ZC[7] LOUT1VOL[6:0]) */
#define WM8960_REG_ROUT1_VOL    0x03   /* 右耳机音量 */
#define WM8960_REG_CLOCKING1    0x04   /* ADCDIV[8:6] DACDIV[5:3] SYSCLKDIV[2:1] CLKSEL[0] */
#define WM8960_REG_DAC_CTRL     0x05   /* ... DACMU[3](0=取消静音) DEEMPH[2:1] ADCHPD[0] */
#define WM8960_REG_AUDIO_IFACE  0x07   /* ALRSWAP[8] BCLKINV[7] MS[6] DLRSWAP[5] LRP[4] WL[3:2] FORMAT[1:0] */
#define WM8960_REG_LDAC_VOL     0x0A   /* DACVU[8] LDACVOL[7:0] */
#define WM8960_REG_RDAC_VOL     0x0B   /* DACVU[8] RDACVOL[7:0] */
#define WM8960_REG_RESET        0x0F   /* 写任意值 = 软件复位 */
#define WM8960_REG_PWR1         0x19   /* VMIDSEL[8:7] VREF[6] AINL[5] AINR[4] ADCL[3] ADCR[2] MICB[1] DIGENB[0] */
#define WM8960_REG_PWR2         0x1A   /* DACL[8] DACR[7] LOUT1[6] ROUT1[5] SPKL[4] SPKR[3] -[2] OUT3[1] PLL_EN[0] */
#define WM8960_REG_LOUT_MIX     0x22   /* LD2LO[8] LI2LO[7] LI2LOVOL[6:4] */
#define WM8960_REG_ROUT_MIX     0x25   /* RD2RO[8] RI2RO[7] RI2ROVOL[6:4] */
#define WM8960_REG_PWR3         0x2F   /* -[8:6] LMIC[5] RMIC[4] LOMIX[3] ROMIX[2] */
#define WM8960_REG_SPK_VOL_L    0x28   /* SPKVU[8] SPKLZC[7] SPKLVOL[6:0]  (类D 左, 备用) */
#define WM8960_REG_SPK_VOL_R    0x29   /* 类D 右 (备用) */
#define WM8960_REG_CLASS_D1     0x31   /* SPK_OP_EN[7:6] ... (类D 使能, 备用) */
#define WM8960_REG_ANTIPOP1     0x1C   /* Anti-pop 1: POBCTRL[7] BUFDCOPEN[4] BUFIOEN[3] SOFT_ST[2] HPSTBY[0] */

/* 若耳机口实际接在类D 功放输出(SPK_LP/LN, SPK_RP/RN)上, 把下面改成 1 */
#define WM8960_USE_CLASSD_OUT   0

/* 当前音量档位: 0~100 (线性映射 -60dB~0dB)。默认 80 ≈ -12dB, 避免一开机太响。 */
static uint8_t wm8960_vol = 80;

/* 写一个寄存器: 16 位控制字 = [7位寄存器地址 | 数据bit8] + [数据低8位]。
 * 手册: CONTROL BYTE 1 = B15..B8 = 寄存器地址(7bit) + 第一个数据位(B8) */
void WM8960_WriteReg(uint8_t reg, uint16_t val)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)((reg << 1) | ((val >> 8) & 0x01));
    buf[1] = (uint8_t)(val & 0xFF);

    /* 与 OLED 共用 I2C1: 调度器已运行时先拿总线锁; 启动阶段(main 里)调度器还没跑就不用锁 */
    if (oled_mutexHandle != NULL && osKernelGetState() == osKernelRunning)
    {
        osMutexAcquire(oled_mutexHandle, osWaitForever);
        (void)HAL_I2C_Master_Transmit(&hi2c1, WM8960_I2C_ADDR, buf, 2, 100);
        osMutexRelease(oled_mutexHandle);
    }
    else
    {
        (void)HAL_I2C_Master_Transmit(&hi2c1, WM8960_I2C_ADDR, buf, 2, 100);
    }
}

/* 初始化 WM8960: 按数据手册寄存器表配置出 "I2S 从模式 → DAC → 耳机" 通路。
 * 注意默认值: R25/R26/R47/R34/R37 默认全是 0(通路关), R5 默认 DAC 静音, R2/R3 默认耳机模拟静音,
 *             所以这一串写寄存器是"必须的", 不写就没声音。 */
uint8_t WM8960_Init(void)
{
    /* 0) 先探测器件是否应答 (查 I2C 接线/供电/地址) */
    if (HAL_I2C_IsDeviceReady(&hi2c1, WM8960_I2C_ADDR, 3, 100) != HAL_OK)
    {
        return 0;   /* 无应答: WM8960 没响应 I2C */
    }

    /* 1) 软件复位(所有寄存器回默认值) */
    WM8960_WriteReg(WM8960_REG_RESET, 0x000);
    HAL_Delay(10);

    /* 2) 时钟: SYSCLK 直接取 MCLK, 不分频 (MCLK = 256 x fs)
     *    CLKSEL[0]=0(MCLK), SYSCLKDIV[2:1]=00(/1), DACDIV[5:3]=000(/1) */
    WM8960_WriteReg(WM8960_REG_CLOCKING1, 0x000);

    /* 3) 音频接口: 从模式(MS[6]=0) + I2S(FORMAT[1:0]=10) + 24bit(WL[3:2]=10) → 0x00A
     *    与 STM32 I2S2 的 "Philips 标准 + 24bit + 主机" 匹配 */
    WM8960_WriteReg(WM8960_REG_AUDIO_IFACE, 0x00A);

    /* 4) 电源管理1: 使能 VMID 分压(2x50k, 供模拟参考) + VREF
     *    VMIDSEL[8:7]=01 → 0x080 ; VREF[6]=1 → 0x040  ⇒ 0x0C0 */
    WM8960_WriteReg(WM8960_REG_PWR1, 0x0C0);

    /* 5) 电源管理2: 使能左右 DAC + 耳机输出驱动 LOUT1/ROUT1
     *    DACL[8]=1(0x100) DACR[7]=1(0x080) LOUT1[6]=1(0x040) ROUT1[5]=1(0x020) ⇒ 0x1E0 */
    WM8960_WriteReg(WM8960_REG_PWR2, 0x1E0);

    /* 6) 电源管理3: 使能左右输出混音器
     *    LOMIX[3]=1(0x008) ROMIX[2]=1(0x004) ⇒ 0x00C */
    WM8960_WriteReg(WM8960_REG_PWR3, 0x00C);

    /* 7) 把 DAC 接到输出混音器 (默认是断开的!) */
    WM8960_WriteReg(WM8960_REG_LOUT_MIX, 0x100);   /* LD2LO[8]=1: 左DAC → 左输出混音器 */
    WM8960_WriteReg(WM8960_REG_ROUT_MIX, 0x100);   /* RD2RO[8]=1: 右DAC → 右输出混音器 */

    /* 8) 取消 DAC 数字静音 (R5 默认 DACMU[3]=1 是静音!) */
    WM8960_WriteReg(WM8960_REG_DAC_CTRL, 0x000);

    /* 9) DAC 数字音量 = 0dB (LDACVOL/RDACVOL=0xFF), 并置更新位 DACVU[8]=1.
     *    默认值本来就是 0xFF, 但要先写更新位才会真正加载。 */
    WM8960_WriteReg(WM8960_REG_LDAC_VOL, 0x1FF);
    WM8960_WriteReg(WM8960_REG_LDAC_VOL, 0x0FF);
    WM8960_WriteReg(WM8960_REG_RDAC_VOL, 0x1FF);
    WM8960_WriteReg(WM8960_REG_RDAC_VOL, 0x0FF);

    /* 10) 耳机输出音量 (R2/R3 默认 0x000 = 模拟静音! 必须设置) */
    WM8960_SetVolume(wm8960_vol);

#if WM8960_USE_CLASSD_OUT
    /* 备用: 若耳机口接在类D 功放输出上, 使能类D 输出并设音量 */
    WM8960_WriteReg(WM8960_REG_PWR2, 0x1F8);       /* DACL/DACR + SPKL[4]/SPKR[3] */
    WM8960_WriteReg(WM8960_REG_SPK_VOL_L, 0x17F);  /* 0dB + 更新 */
    WM8960_WriteReg(WM8960_REG_SPK_VOL_L, 0x07F);
    WM8960_WriteReg(WM8960_REG_SPK_VOL_R, 0x17F);
    WM8960_WriteReg(WM8960_REG_SPK_VOL_R, 0x07F);
    WM8960_WriteReg(WM8960_REG_CLASS_D1, 0x0C7);   /* SPK_OP_EN[7:6]=11 使能类D */
#endif

    return 1;
}

/* ================= 音量控制 =================
 * 耳机音量寄存器 LOUT1VOL/ROUT1VOL (R2/R3 低 7 位):
 *   0x79 = 0dB, 每 -1 为 -1dB, 0x30 = -73dB, 0x00~0x2F = 模拟静音。
 * 把 0~100 档线性映射到 -60dB(0) ~ 0dB(100): code = 0x3D + vol*60/100。
 * 写寄存器时必须置更新位 OUT1VU[8]=1 才会真正生效。 */
void WM8960_SetVolume(uint8_t vol)
{
    uint16_t code;
    if (vol > 100) vol = 100;
    wm8960_vol = vol;
    code = (uint16_t)(0x3D + (uint16_t)((uint16_t)vol * 60U / 100U));
    WM8960_WriteReg(WM8960_REG_LOUT1_VOL, (uint16_t)(0x100 | code));   /* 左 + 更新位 */
    WM8960_WriteReg(WM8960_REG_ROUT1_VOL, (uint16_t)(0x100 | code));   /* 右 + 更新位 */
}

uint8_t WM8960_GetVolume(void)
{
    return wm8960_vol;
}

void WM8960_VolumeUp(void)
{
    if (wm8960_vol >= 95) WM8960_SetVolume(100);
    else                  WM8960_SetVolume((uint8_t)(wm8960_vol + 5));
}

void WM8960_VolumeDown(void)
{
    if (wm8960_vol <= 5) WM8960_SetVolume(0);
    else                 WM8960_SetVolume((uint8_t)(wm8960_vol - 5));
}

/* ================= DAC 数字软静音 =================
 * R5 (0x05) DACMU[3]: 1 = 数字软静音(渐进斜坡, 不会有"啪"声), 0 = 取消静音。
 * 用途: 切歌/换采样率时 I2S 时钟会变化, WM8960 内部滤波/电荷泵需重新锁定,
 *       若此时输出还开着就会听到"呲呲"瞬态噪声。
 *       正确做法: 停机前 DacMute(), 新歌时钟稳定后再 DacUnmute()。 */
void WM8960_DacMute(void)
{
    WM8960_WriteReg(WM8960_REG_DAC_CTRL, 0x008);   /* DACMU[3]=1: 软静音 */
}

void WM8960_DacUnmute(void)
{
    WM8960_WriteReg(WM8960_REG_DAC_CTRL, 0x000);   /* DACMU[3]=0: 取消静音 */
}

/* ================= 切歌/停机时的 codec 上下电时序 =================
 * 依据手册 "STOPPING THE MASTER CLOCK" (WM8960 Rev4.4, p69):
 *   "MCLK should not be stopped while the class D outputs are enabled"
 *   "Before DIGENB can be set, the control bits ADCL, ADCR, DACL and DACR must be set to
 *    zero and a waiting time of 1ms must be observed. Any failure to follow this procedure
 *    may prevent DACs and ADCs from re-starting correctly."
 *
 * 本工程切歌时 HAL_I2S_DMAStop() 会关掉 I2S 外设 → MCLK 直接消失, 就是"停止主时钟",
 * 因此必须按上面这条时序做: 先软静音 → 关 DAC → 等 >=1ms → 才让 MCLK 停。
 * 另外用 R28(Anti-Pop 1) 的 HPSTBY 把耳机放大器送 standby, 让时钟中断期间输出彻底安静。
 * (注: 本工程未使用 ADC, R25 里 ADCL/ADCR 一直是 0, 符合手册要求) */

#define WM8960_PWR2_DAC_ON        0x1E0u   /* DACL|DACR|LOUT1|ROUT1  (正常播放态) */
#define WM8960_PWR2_DAC_OFF       0x060u   /* LOUT1|ROUT1            (关 DAC, 保留输出级) */
#define WM8960_ANTIPOP1_HP_NORM   0x000u   /* HPSTBY=0: 耳机放大器正常工作 */
#define WM8960_ANTIPOP1_HP_STBY   0x001u   /* HPSTBY=1: 耳机放大器 standby */

/* 停 MCLK 之前调用(调用方随后执行 HAL_I2S_DMAStop)。
 * 内部自带全部延时, 直接调用即可, 不要再自己加延时。 */
void WM8960_BeforeClockStop(void)
{
    /* 1) DAC 数字软静音。斜坡速率由 R6 DACMR 决定: 默认 0=快(fs/2, @48k 最大 10.7ms) */
    WM8960_DacMute();
    osDelay(15);                                        /* 等软静音斜坡走完(>10.7ms) */

    /* 2) 耳机放大器进 standby: 时钟中断期间输出最安静(反相过程见 AfterClockStart) */
    WM8960_WriteReg(WM8960_REG_ANTIPOP1, WM8960_ANTIPOP1_HP_STBY);

    /* 3) ★手册要求: 停 MCLK 前把 DACL/DACR 清零 */
    WM8960_WriteReg(WM8960_REG_PWR2, WM8960_PWR2_DAC_OFF);

    /* 4) ★手册要求: 观察时间 >= 1ms */
    osDelay(3);
}

/* 新时钟已稳定、数据流已起来之后调用(与 BeforeClockStop 反序)。
 * ★必须在 HAL_I2S_Transmit_DMA() 之后调用 —— 千万不要插在
 * 「force_i2s_config() 使能外设」和「起 DMA」之间, 那会打乱 I2S 数据流的相位对齐。 */
void WM8960_AfterClockStart(void)
{
    /* 1) 时钟已经跑起来了(DAC 上电要求 SYSCLK 有效), 重新打开 DAC */
    WM8960_WriteReg(WM8960_REG_PWR2, WM8960_PWR2_DAC_ON);
    osDelay(5);                                         /* 等 DAC/数字滤波器稳定 */

    /* 2) 耳机放大器退出 standby */
    WM8960_WriteReg(WM8960_REG_ANTIPOP1, WM8960_ANTIPOP1_HP_NORM);
    osDelay(1);

    /* 3) 解除软静音。首帧就开始播放, 所以用默认 DACSMM=0(立即回到音量设定),
     *    这样新曲开头不会被渐升压低(手册 p35 的建议) */
    WM8960_DacUnmute();
}
