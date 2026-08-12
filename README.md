# 🎵 STM32 Music Player

基于 **STM32F103ZET6** 的实时嵌入式音乐播放器 —— 从 MicroSD 卡读取音频文件，经 **I2S** 接口输出解码后的数字音频至外置 DAC（CS4344）。

<p align="center">
  <img src="https://img.shields.io/badge/MCU-STM32F103ZET6-blue" alt="MCU"/>
  <img src="https://img.shields.io/badge/RTOS-FreeRTOS%2010.0.1-brightgreen" alt="RTOS"/>
  <img src="https://img.shields.io/badge/FATFS-R0.14-orange" alt="FATFS"/>
  <img src="https://img.shields.io/badge/IDE-STM32CubeIDE-0091BD" alt="IDE"/>
  <img src="https://img.shields.io/badge/%E9%9F%B3%E9%A2%91-WAV%20%E2%9C%85%20%7C%20FLAC%20%E2%9C%8D%EF%B8%8F%20%7C%20MP3%20%E2%9C%8D%EF%B8%8F-yellow" alt="Codec"/>
</p>

---

## ✨ 项目简介

本项目是一个运行在 **STM32F103ZET6**（72MHz / 64KB SRAM / 512KB Flash）上的 FreeRTOS 多任务音乐播放器，实现完整的 **"SD 卡 → FATFS → 双缓冲 → DMA → I2S → DAC → 音频输出"** 数据通路。

当前已完成 **WAV（PCM）** 格式的实时播放，通过硬件 DMA 双缓冲 + 中断回调实现无缝音频流。FLAC / MP3 解码为下一阶段目标，架构上已为其预留接口。

---

## 🚀 功能特性

| 功能 | 状态 | 说明 |
|:---|:---:|:---|
| WAV 播放 | ✅ 已完成 | 24bit/32bit 左对齐，44.1kHz 最接近采样率播放 |
| FLAC 解码 | 🔜 规划中 | `flac_file_process()` 已预留 |
| MP3 解码 | 🔜 规划中 | `mp3_file_process()` 已预留 |
| 上一首 / 下一首 | ✅ 已实现 | PE0 / PE1 按键 |
| 播放 / 暂停 | ✅ 已实现 | PE2 按键 |
| 停止 | ✅ 已实现 | PE3 按键 |
| OLED 显示 | ✅ 已实现 | 歌曲名 / 文件数 / WAV 信息 / 错误信息 |
| USB Mass Storage | ✅ 已实现 | 可通过 USB 访问 SD 卡 |
| SD 卡热插拔自动重载 | 🔜 规划中 | — |

---

## 🛠 硬件连接

### 引脚分配

| 外设 | 引脚 | 功能 |
|:---|:---|:---|
| **I2S2** | PB12 | WS（声道选择） |
| | PB13 | CK（位时钟） |
| | PB15 | SD（数据输出） |
| | PC6 | MCK（主时钟输出） |
| **SDIO** | PC8 | SDIO_D0 |
| | PC12 | SDIO_CK |
| | PD2 | SDIO_CMD |
| **I2C1** | PB6 | SCL |
| | PB7 | SDA |
| **按键** | PE0 | 上一首（EXTI0，下降沿，上拉） |
| | PE1 | 下一首（EXTI1，下降沿，上拉） |
| | PE2 | 播放 / 暂停（EXTI2，下降沿，上拉） |
| | PE3 | 停止（EXTI3，下降沿，上拉） |

> **DAC**: CS4344（I2S 接口，MSB-first，需要 64×fs 的 MCLK），支持 16/24/32bit 数据。

### 时钟树

```
HSE 8MHz ──┬── PLL ×9 ──> SYSCLK 72MHz
            │               ├── AHB ──> SDIO, DMA, GPIO
            │               └── I2S2 时钟源 = SYSCLK
            └── I2S2        ──> 分频 ──> BCLK / MCLK
```

---

## 🧠 软件架构

### RTOS 任务（FreeRTOS 10.0.1 / CMSIS-RTOS V2）

```
┌─────────────────────────────────────────────────────────────────┐
│                        FreeRTOS 调度器                           │
│                                                                  │
│  ┌──────────────┐   ┌──────────────────┐   ┌────────────────┐   │
│  │  USB_TASK    │   │   AUDIO_READ     │   │   FILE_LOAD    │   │
│  │  prio Normal2│   │  prio Normal     │   │  prio Normal1  │   │
│  │  栈 512B     │   │  栈 8KB          │   │  栈 4KB        │   │
│  └──────┬───────┘   └────────┬─────────┘   └────────┬───────┘   │
│         │ 初始化 USB          │ 播放主控              │ 扫描文件    │
│         │                    │                       │           │
│         │                    │   LOAD_DONE_OR_NOT    │           │
│         │                    │◄────── 信号量 ────────│           │
│         │                    │                       │           │
│         │                    │  LOAD_OR_NOT 信号量    │           │
│         │                    │─────── 循环 ─────────►│           │
└─────────┴────────────────────┴───────────────────────┴───────────┘
```

### 音频数据通路

```mermaid
graph LR
    A[MicroSD 卡] -->|FATFS f_read| B[raw_buf<br/>24bit 原始字节]
    B -->|字节序转换<br/>24bit → 32bit 左对齐| C[audio_buffer[2][4096]<br/>DMA 双缓冲]
    C -->|DMA Circular 模式| D[I2S2]
    D -->|I2S Philips 标准| E[CS4344 DAC]
    E --> F[音频输出]

    G[HAL_I2S_TxHalfCpltCallback] -->|half_ready| C
    H[HAL_I2S_TxCpltCallback] -->|full_ready| C
```

### 播放核心机制

- **DMA 双缓冲**：`audio_buffer` 为 2 行 × 4096 半字，`HAL_I2S_Transmit_DMA` 在 24bit/32bit 模式下自动将传输长度翻倍（`Size << 1`），实际搬运 8192 半字，恰好完整覆盖两行，配合 DMA 循环模式实现无缝播放。
- **中断驱动填充**：DMA 半传输完成 → 重填后半区；全传输完成 → 重填前半区，`AUDIO_READ` 任务阻塞轮询标志。
- **动态采样率配置**：`i2s_set_sample_rate()` 按 WAV 头声明的采样率暴力搜索最优 `I2SDIV / ODD` 分频组合。

---

## 📁 目录结构

```
Music_Player_FreeRTOS/
├── Core/
│   ├── Inc/                  # 头文件
│   │   ├── Music_Driver.h    # 音频驱动接口 / WAV 头结构体
│   │   ├── FreeRTOSConfig.h  # FreeRTOS 配置
│   │   └── OLED_Font.h       # OLED 字库
│   ├── Src/                  # 源文件
│   │   ├── main.c            # 主程序 / 时钟配置
│   │   ├── freertos.c        # RTOS 任务创建
│   │   ├── Music_Driver.c    # ★ 音频核心逻辑
│   │   ├── i2s.c / dma.c     # I2S2 / DMA 外设配置
│   │   ├── sdio.c            # SDIO 外设配置
│   │   ├── OLED.c            # OLED 显示驱动
│   │   └── stm32f1xx_it.c    # 中断服务程序
│   └── Startup/
├── Drivers/                  # STM32 HAL 库 / CMSIS
├── FATFS/                    # FatFs 文件系统
├── Middlewares/              # FreeRTOS 内核 / FatFs
├── USB_DEVICE/               # USB Mass Storage 设备
└── Music_Player_FreeRTOS.ioc # STM32CubeMX 工程文件
```

---

## 🔧 编译与烧录

1. 安装 [STM32CubeIDE](https://www.st.com/en/development-tools/stm32cubeide.html)
2. 打开工程：`File → Open Projects from File System…`，选择 `Music_Player_FreeRTOS` 目录
3. 连接 ST-Link，选择 Debug 配置，编译并烧录
4. 将 **WAV 文件**（推荐 44.1kHz / 24bit / 立体声）放入 SD 卡根目录

---

## 🎮 使用方法

| 按键 | 功能 |
|:---:|:---|
| **PE0** | 上一首 |
| **PE1** | 下一首 |
| **PE2** | 播放 / 暂停切换 |
| **PE3** | 停止 |

> 上电后自动扫描 SD 卡根目录，OLED 显示文件数量；选中 WAV 文件后按 **PE2** 开始播放。

---

## ⚠️ 已知限制

- **无法精确输出 44.1kHz**：STM32F103 的 I2S2 时钟源仅有 SYSCLK（72MHz）与 PLL3（整数倍频），无小数 PLL。24bit/32bit Philips 标准下帧长为 64bit，实际采样率只能取 `72MHz / (2 × (DIV + ODD)) / 64` 的离散值。44.1kHz 最接近的分频为 **43.269kHz（偏差 -1.9%）**，播放时音高约低 32 音分（人耳基本无感）。
- 当前仅支持 **WAV（PCM）** 格式；FLAC / MP3 需移植解码器。
- 采样率支持已按 WAV 头动态配置，但受上述分频限制为近似值。

---

## 📅 开发日志

- **2026-03-07** — 项目创建，WAV 头解析与基础播放框架。
- **2026-06-30** — 完成 I2S2 + DMA 双缓冲播放通路；加入按键控制、OLED 显示。
- **2026-08-11** — 修复 WAV 播放问题：缓冲寻址写穿、字节序错误、I2S 采样率硬编码；新增动态采样率分频。

---

## 📄 许可证

本项目使用 **ST 官方 HAL 库**（由 STM32CubeMX 生成），遵循 ST 软件许可条款。应用层代码仅供学习交流使用。

---

<p align="center">
  <sub>Made with ❤️ and FreeRTOS · STM32F103ZET6 · CS4344</sub>
</p>
