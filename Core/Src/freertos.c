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
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
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
#include "Music_Driver.h"
#include "OLED.h"
#include "timers.h"
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

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal2,
};
/* Definitions for audio_file_read */
osThreadId_t audio_file_readHandle;
const osThreadAttr_t audio_file_read_attributes = {
  .name = "audio_file_read",
  .stack_size = 2048 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for audio_file_load */
osThreadId_t audio_file_loadHandle;
const osThreadAttr_t audio_file_load_attributes = {
  .name = "audio_file_load",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityNormal1,
};
/* Definitions for LOAD_OR_NOT */
osSemaphoreId_t LOAD_OR_NOTHandle;
const osSemaphoreAttr_t LOAD_OR_NOT_attributes = {
  .name = "LOAD_OR_NOT"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

osSemaphoreId_t LOAD_DONE_OR_NOTHandle;
const osSemaphoreAttr_t LOAD_DONE_OR_NOT_attributes = {
  .name = "LOAD_DONE_OR_NOT"
};

/* USER CODE END FunctionPrototypes */

void USB_TASK(void *argument);
void AUDIO_READ(void *argument);
void FILE_LOAD(void *argument);

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
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* creation of LOAD_OR_NOT */
  LOAD_OR_NOTHandle = osSemaphoreNew(1, 1, &LOAD_OR_NOT_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  LOAD_DONE_OR_NOTHandle = osSemaphoreNew(1, 0, &LOAD_DONE_OR_NOT_attributes);
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */

  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(USB_TASK, NULL, &defaultTask_attributes);

  /* creation of audio_file_read */
  audio_file_readHandle = osThreadNew(AUDIO_READ, NULL, &audio_file_read_attributes);

  /* creation of audio_file_load */
  audio_file_loadHandle = osThreadNew(FILE_LOAD, NULL, &audio_file_load_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_USB_TASK */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_USB_TASK */
void USB_TASK(void *argument)
{
  /* init code for USB_DEVICE */
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN USB_TASK */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1000);
  }
  /* USER CODE END USB_TASK */
}

/* USER CODE BEGIN Header_AUDIO_READ */
/**
* @brief Function implementing the audio_file_read thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_AUDIO_READ */
void AUDIO_READ(void *argument)
{
  /* USER CODE BEGIN AUDIO_READ */
  /* Infinite loop */
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
* @brief Function implementing the audio_file_load thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_FILE_LOAD */
void FILE_LOAD(void *argument)
{
  /* USER CODE BEGIN FILE_LOAD */
	uint8_t sem_num=0,load_flag;
  /* Infinite loop */
    for(;;)
    {
	    osSemaphoreAcquire(LOAD_OR_NOTHandle, osWaitForever);
	    sem_num++;
	    load_flag = audio_file_load();
	    if (load_flag == 0)
	    {
	    	OLED_Clear();
	    	OLED_ShowString(1, 1, "RELEASE");
	        osSemaphoreRelease(LOAD_DONE_OR_NOTHandle);
	        OLED_ShowString(2, 1, "DONE");
	        OLED_ShowNum(3, 1, sem_num, 3);
	    }
	    else if(load_flag == 3)
	    {
	        OLED_Clear();
	        OLED_ShowString(1, 1, "Not files.");
	    }
    }
  /* USER CODE END FILE_LOAD */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

