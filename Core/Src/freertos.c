/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "Music_Driver.h"
#include "OLED.h"
#include "wm8960.h"    /* 音量键 PF7/PF8 → I2C 调 WM8960 音量 */
#include "timers.h"
#include "usbd_storage_if.h"
#include "usbd_core.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
extern USBD_HandleTypeDef hUsbDeviceFS;
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal2,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* 音频播放任务 / 文件扫描任务句柄 (创建在 MX_FREERTOS_Init 的 RTOS_THREADS USER CODE 区) */
osThreadId_t audio_file_readHandle;
const osThreadAttr_t audio_file_read_attributes = {
  .name = "audio_file_read",
  .stack_size = 2048 * 4,   /* 8KB: Helix 定点解码器栈需求极小(MP3Decode 仅 112 字节, 全库最大 448),
                               原来 32KB 是为 minimp3(需 17.5KB 栈)留的, 换 Helix 后可大幅缩小 */
  .priority = (osPriority_t) osPriorityNormal,
};
osThreadId_t audio_file_loadHandle;
const osThreadAttr_t audio_file_load_attributes = {
  .name = "audio_file_load",
  .stack_size = 768 * 4,
  .priority = (osPriority_t) osPriorityNormal1,
};

osSemaphoreId_t LOAD_OR_NOTHandle;
const osSemaphoreAttr_t LOAD_OR_NOT_attributes = {
  .name = "LOAD_OR_NOT"
};
osSemaphoreId_t LOAD_DONE_OR_NOTHandle;
const osSemaphoreAttr_t LOAD_DONE_OR_NOT_attributes = {
  .name = "LOAD_DONE_OR_NOT"
};

/* 歌名滚动字幕定时器 */
osTimerId_t scroll_timerHandle;
const osTimerAttr_t scroll_timer_attributes = {
  .name = "scroll_timer"
};

/* OLED I2C 访问互斥量 (递归: 整屏刷新外层持锁 + 单字节传输内层递归获取) */
osMutexId_t oled_mutexHandle;
const osMutexAttr_t oled_mutex_attributes = {
  .name = "oled_mutex",
  .attr_bits = osMutexRecursive,
};

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void AUDIO_READ(void *argument);
void FILE_LOAD(void *argument);
void OLED_SCROLL_Callback(void *argument);

extern void MX_USB_DEVICE_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* OLED I2C 访问互斥量: 防止滚动定时器回调与音频任务(切歌显示)并发操作 I2C 导致花屏/丢数据 */
  oled_mutexHandle = osMutexNew(&oled_mutex_attributes);
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* creation of LOAD_OR_NOT */
  LOAD_OR_NOTHandle = osSemaphoreNew(1, 1, &LOAD_OR_NOT_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  LOAD_DONE_OR_NOTHandle = osSemaphoreNew(1, 0, &LOAD_DONE_OR_NOT_attributes);
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* 歌名滚动字幕定时器 (50ms 周期, 在 StartDefaultTask 启动后 osTimerStart)。
   * 同时承载切歌键长按检测。 */
  scroll_timerHandle = osTimerNew(OLED_SCROLL_Callback, osTimerPeriodic, NULL, &scroll_timer_attributes);
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* 音频播放任务 + SD 扫描任务 */
  audio_file_readHandle = osThreadNew(AUDIO_READ, NULL, &audio_file_read_attributes);
  audio_file_loadHandle = osThreadNew(FILE_LOAD, NULL, &audio_file_load_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread (USB 任务).
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for USB_DEVICE */
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN StartDefaultTask */
  /* 启动滚动+按键检测定时器 (50ms 周期) */
  if (scroll_timerHandle != NULL) {
    osTimerStart(scroll_timerHandle, 50);
  }
  uint8_t usb_was_connected = 0;
  /* Infinite loop */
  for(;;)
  {
    /* 读取当前 USB 枚举状态 */
    uint8_t now_connected = (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED);
    if (now_connected != usb_was_connected)
    {
      usb_was_connected = now_connected;
      usb_sd_occupied = now_connected;   /* 同步占用标志 */
      if (now_connected)
      {
        OLED_Clear();
        OLED_ShowString(1, 1, "USB已连接");
        OLED_ShowString(2, 1, "暂停播放");
        audio_stop();
      }
      else
      {
        osSemaphoreRelease(LOAD_OR_NOTHandle);   /* 触发重新扫描 SD */
        OLED_Clear();
        OLED_ShowString(1, 1, "USB已断开");
        OLED_ShowString(2, 1, "重新扫描...");
      }
    }
    osDelay(200);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_AUDIO_READ */
/**
* @brief 音频播放任务
*/
/* USER CODE END Header_AUDIO_READ */
void AUDIO_READ(void *argument)
{
  /* USER CODE BEGIN AUDIO_READ */
  for(;;)
  {
    OLED_Clear();
    osSemaphoreAcquire(LOAD_DONE_OR_NOTHandle, osWaitForever);
    while(1)
    {
      audio_file_read(&audiofiles[0]);
    }
  }
  /* USER CODE END AUDIO_READ */
}

/* USER CODE BEGIN Header_FILE_LOAD */
/**
* @brief SD 卡音频文件扫描任务
*/
/* USER CODE END Header_FILE_LOAD */
void FILE_LOAD(void *argument)
{
  /* USER CODE BEGIN FILE_LOAD */
  uint8_t sem_num=0, load_flag;
  for(;;)
  {
    osSemaphoreAcquire(LOAD_OR_NOTHandle, osWaitForever);
    sem_num++;
    load_flag = audio_file_load();
    if (load_flag == 0)
    {
      OLED_Clear();
      OLED_ShowString(1, 1, "就绪");
      osSemaphoreRelease(LOAD_DONE_OR_NOTHandle);
      OLED_ShowString(2, 1, "完成");
      OLED_ShowNum(3, 1, sem_num, 3);
    }
    else if (load_flag == 3)
    {
      OLED_Clear();
      OLED_ShowString(1, 1, "没有文件");
    }
  }
  /* USER CODE END FILE_LOAD */
}

/* 把当前音量显示到 OLED 第 4 行 (音量键按下时调用; 本函数只在定时器任务里调用, 不涉及并发) */
static void oled_show_volume(void)
{
  char str[16];
  sprintf(str, "音量:%d", (int)WM8960_GetVolume());
  OLED_ShowString(4, 1, str);
  OLED_RefreshScreenWithScroll();
}

/* USER CODE BEGIN Header_OLED_SCROLL */
/**
* @brief 软件定时器回调: 长歌名滚动字幕 + 切歌键长按检测 + 音量键 (每 50ms)。
*/
/* USER CODE END Header_OLED_SCROLL */
void OLED_SCROLL_Callback(void *argument)
{
  /* USER CODE BEGIN OLED_SCROLL */
  uint16_t offset;
  uint32_t now;
  uint8_t prev, next;
  static uint8_t scroll_div = 0;   /* 50ms 回调, 滚动每 2 次推进一列(~100ms) */

  /* ===== 切歌键长按检测 (按下由 EXTI 捕获, 这里轮询松开/长按) ===== */
  now = HAL_GetTick();
  prev = (HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_0) == GPIO_PIN_RESET);
  next = (HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_1) == GPIO_PIN_RESET);

  /* PF0: 上一首 (短按=上一首, 长按=快退) */
  if (!prev && key_prev_down) {
    if ((now - key_prev_tick) < 30) {
      key_prev_down = 0;
    } else if ((now - key_prev_tick) < 300) {
      key_prev_down = 0;
      play_cmd = CMD_PREV;
    } else {
      key_prev_down = 0;
      seek_speed = 0;
    }
  } else if (prev && key_prev_down && (now - key_prev_tick >= 300)) {
    if (current_audio_type != 3) {
      int s = 2 + (int)((now - key_prev_tick - 300) / 5000);
      if (s > 4) s = 4;
      seek_speed = -s;
    }
  }

  /* PF1: 下一首 (短按=下一首, 长按=快进) */
  if (!next && key_next_down) {
    if ((now - key_next_tick) < 30) {
      key_next_down = 0;
    } else if ((now - key_next_tick) < 300) {
      key_next_down = 0;
      play_cmd = CMD_NEXT;
    } else {
      key_next_down = 0;
      seek_speed = 0;
    }
  } else if (next && key_next_down && (now - key_next_tick >= 300)) {
    int s = 2 + (int)((now - key_next_tick - 300) / 5000);
    if (s > 4) s = 4;
    seek_speed = s;
  }

  /* ===== 音量键 PF7(加) / PF8(减): 轮询读取 =====
   * 不用 EXTI 的原因: 便于实现"按住连调", 且避免在中断里做 I2C 写
   * (I2C 写会阻塞中断, 还会与 OLED 抢总线)。
   * 行为: 按下立即调一档; 按住超过 400ms 后每 200ms 再连调一档。 */
  {
    static uint8_t  vol_key = 0;      /* 0=无 / 1=PF7(音量+) / 2=PF8(音量-) */
    static uint32_t vol_tick = 0;
    uint8_t vkey = 0;

    if (HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_7) == GPIO_PIN_RESET)      vkey = 1;
    else if (HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_8) == GPIO_PIN_RESET) vkey = 2;

    if (vkey != 0) {
      if (vkey != vol_key) {                    /* 刚按下: 立刻调一档 */
        vol_key = vkey;
        vol_tick = now;
        if (vkey == 1) WM8960_VolumeUp(); else WM8960_VolumeDown();
        oled_show_volume();
      } else if ((now - vol_tick) >= 400) {      /* 按住 400ms 后连调(每 200ms 一档) */
        vol_tick = now;
        if (vkey == 1) WM8960_VolumeUp(); else WM8960_VolumeDown();
        oled_show_volume();
      }
    } else {
      vol_key = 0;                               /* 松开 */
    }
  }

  /* ===== 歌名滚动 (每 2 次推进一列 ~100ms) ===== */
  if (scrollTextWidth > 128)
  {
    if (++scroll_div >= 2) {
      scroll_div = 0;
      offset = scrollOffset + 1;
      if (offset >= scrollTextWidth) offset = 0;
      scrollOffset = offset;
      OLED_RefreshScreenWithScroll();
    }
  }
  /* USER CODE END OLED_SCROLL */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */
