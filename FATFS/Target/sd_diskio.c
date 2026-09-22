/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sd_diskio.c
  * @brief   SD Disk I/O driver
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

/* Note: code generation based on sd_diskio_dma_rtos_template_bspv1.c v2.1.4
   as FreeRTOS is enabled. */

/* USER CODE BEGIN firstSection */
/* can be used to modify / undefine following code or add new definitions */
/* USER CODE END firstSection*/

/* Includes ------------------------------------------------------------------*/
#include "ff_gen_drv.h"
#include "sd_diskio.h"

#include <string.h>
#include <stdio.h>

/* SDIO 句柄 (定义在 sdio.c) */
extern SD_HandleTypeDef hsd;

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

#define QUEUE_SIZE         (uint32_t) 10
#define READ_CPLT_MSG      (uint32_t) 1
#define WRITE_CPLT_MSG     (uint32_t) 2
/*
==================================================================
enable the defines below to send custom rtos messages
when an error or an abort occurs.
Notice: depending on the HAL/SD driver the HAL_SD_ErrorCallback()
may not be available.
See BSP_SD_ErrorCallback() and BSP_SD_AbortCallback() below
==================================================================

#define RW_ERROR_MSG       (uint32_t) 3
#define RW_ABORT_MSG       (uint32_t) 4
*/
/*
 * the following Timeout is useful to give the control back to the applications
 * in case of errors in either BSP_SD_ReadCpltCallback() or BSP_SD_WriteCpltCallback()
 * the value by default is as defined in the BSP platform driver otherwise 30 secs
 */
/* SD 读写超时(ms)。原值 30*1000=30 秒: HAL 用 HAL_GetTick 计时, 拔卡后要空转 30 秒才返回错误,
 * 导致"拔卡提示不立即"。正常读一次 <1ms, 设 300ms 既安全又能快速检测到拔卡。 */
#define SD_TIMEOUT 300

#define SD_DEFAULT_BLOCK_SIZE 512

/*
 * Depending on the use case, the SD card initialization could be done at the
 * application level: if it is the case define the flag below to disable
 * the BSP_SD_Init() call in the SD_Initialize() and add a call to
 * BSP_SD_Init() elsewhere in the application.
 */
/* USER CODE BEGIN disableSDInit */
/* #define DISABLE_SD_INIT */
/* USER CODE END disableSDInit */

/*
 * when using cacheable memory region, it may be needed to maintain the cache
 * validity. Enable the define below to activate a cache maintenance at each
 * read and write operation.
 * Notice: This is applicable only for cortex M7 based platform.
 */
/* USER CODE BEGIN enableSDDmaCacheMaintenance */
/* #define ENABLE_SD_DMA_CACHE_MAINTENANCE  1 */
/* USER CODE END enableSDDmaCacheMaintenance */

/*
* Some DMA requires 4-Byte aligned address buffer to correctly read/write data,
* in FatFs some accesses aren't thus we need a 4-byte aligned scratch buffer to correctly
* transfer data
*/
/* USER CODE BEGIN enableScratchBuffer */
/* #define ENABLE_SCRATCH_BUFFER */
/* USER CODE END enableScratchBuffer */

/* Private variables ---------------------------------------------------------*/
#if defined(ENABLE_SCRATCH_BUFFER)
#if defined (ENABLE_SD_DMA_CACHE_MAINTENANCE)
ALIGN_32BYTES(static uint8_t scratch[BLOCKSIZE]); // 32-Byte aligned for cache maintenance
#else
__ALIGN_BEGIN static uint8_t scratch[BLOCKSIZE] __ALIGN_END;
#endif
#endif
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;

/* DMA 读对齐缓冲: SDIO DMA 要求 4 字节对齐, FATFS 给的 buff 可能不对齐, 用它中转 */
__ALIGN_BEGIN static uint8_t scratch[SD_DEFAULT_BLOCK_SIZE] __ALIGN_END;

#if (osCMSIS <= 0x20000U)
static osMessageQId SDQueueID = NULL;
#else
static osMessageQueueId_t SDQueueID = NULL;
#endif
/* Private function prototypes -----------------------------------------------*/
static DSTATUS SD_CheckStatus(BYTE lun);
DSTATUS SD_initialize (BYTE);
DSTATUS SD_status (BYTE);
DRESULT SD_read (BYTE, BYTE*, DWORD, UINT);
#if _USE_WRITE == 1
DRESULT SD_write (BYTE, const BYTE*, DWORD, UINT);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
DRESULT SD_ioctl (BYTE, BYTE, void*);
#endif  /* _USE_IOCTL == 1 */

const Diskio_drvTypeDef  SD_Driver =
{
  SD_initialize,
  SD_status,
  SD_read,
#if  _USE_WRITE == 1
  SD_write,
#endif /* _USE_WRITE == 1 */

#if  _USE_IOCTL == 1
  SD_ioctl,
#endif /* _USE_IOCTL == 1 */
};

/* USER CODE BEGIN beforeFunctionSection */
/* can be used to modify / undefine following code or add new code */
/* USER CODE END beforeFunctionSection */

/* Private functions ---------------------------------------------------------*/

static int SD_CheckStatusWithTimeout(uint32_t timeout)
{
  uint32_t timer;
  /* block until SDIO peripheral is ready again or a timeout occur */
#if (osCMSIS <= 0x20000U)
  timer = osKernelSysTick();
  while( osKernelSysTick() - timer < timeout)
#else
  timer = osKernelGetTickCount();
  while( osKernelGetTickCount() - timer < timeout)
#endif
  {
    if (BSP_SD_GetCardState() == SD_TRANSFER_OK)
    {
      return 0;
    }
  }

  return -1;
}

static DSTATUS SD_CheckStatus(BYTE lun)
{
  Stat = STA_NOINIT;

  if(BSP_SD_GetCardState() == SD_TRANSFER_OK)
  {
    Stat &= ~STA_NOINIT;
  }

  return Stat;
}

/**
  * @brief  Initializes a Drive
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_initialize(BYTE lun)
{
Stat = STA_NOINIT;

  /*
   * check that the kernel has been started before continuing
   * as the osMessage API will fail otherwise
   */
#if (osCMSIS <= 0x20000U)
  if(osKernelRunning())
#else
  if(osKernelGetState() == osKernelRunning)
#endif
  {
#if !defined(DISABLE_SD_INIT)

    if(BSP_SD_Init() == MSD_OK)
    {
      Stat = SD_CheckStatus(lun);
    }

#else
    Stat = SD_CheckStatus(lun);
#endif

    /*
    * if the SD is correctly initialized, create the operation queue
    * if not already created
    */

    if (Stat != STA_NOINIT)
    {
      if (SDQueueID == NULL)
      {
 #if (osCMSIS <= 0x20000U)
      osMessageQDef(SD_Queue, QUEUE_SIZE, uint16_t);
      SDQueueID = osMessageCreate (osMessageQ(SD_Queue), NULL);
#else
      SDQueueID = osMessageQueueNew(QUEUE_SIZE, 2, NULL);
#endif
      }

      if (SDQueueID == NULL)
      {
        Stat |= STA_NOINIT;
      }
    }
  }

  return Stat;
}

/**
  * @brief  Gets Disk Status
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_status(BYTE lun)
{
  return SD_CheckStatus(lun);
}

/* USER CODE BEGIN beforeReadSection */
/* 全局: 最近一次 SD 读/写是否失败。=1 时上层(播放循环)判定卡可能被拔出。
 * 在磁盘 I/O 层统一设置, WAV/FLAC/MP3 三种格式都能可靠检测到。 */
volatile uint8_t g_sd_io_err = 0;
/* USER CODE END beforeReadSection */
/**
  * @brief  Reads Sector(s)
  * @param  lun : not used
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */

DRESULT SD_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
  /* 轮询读取 (4-bit 8MHz 下带宽充足, 对音频播放完全够用, 稳定可靠)。
   * 之前尝试改 DMA(DMA2_Stream6/Ch4)会卡在等 DMA 完成回调(中断链有问题),
   * 导致挂载时 FILE_LOAD 阻塞, 已回退轮询。 */
  UNUSED(lun);
  if (SD_CheckStatusWithTimeout(SD_TIMEOUT) < 0)
  {
    g_sd_io_err = 1;                 /* 读失败: 很可能是拔卡 */
    Stat |= STA_NOINIT;              /* 关键: 标记"未初始化", 下次挂载会重新识别卡(否则插回也不重挂!) */
    return RES_ERROR;
  }
  if (BSP_SD_ReadBlocks((uint32_t*)buff, (uint32_t)sector, count, SD_TIMEOUT) == MSD_OK)
  {
    g_sd_io_err = 0;
    return RES_OK;
  }
  g_sd_io_err = 1;
  Stat |= STA_NOINIT;              /* 关键: 同上, 强制下次重新初始化 */
  return RES_ERROR;
}

/* USER CODE BEGIN beforeWriteSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END beforeWriteSection */
/**
  * @brief  Writes Sector(s)
  * @param  lun : not used
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1

DRESULT SD_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
  /* 轮询写入 (不依赖 DMA) */
  if (SD_CheckStatusWithTimeout(SD_TIMEOUT) < 0)
  {
    g_sd_io_err = 1;
    Stat |= STA_NOINIT;
    return RES_ERROR;
  }
  if (BSP_SD_WriteBlocks((uint32_t*)buff, (uint32_t)sector, count, SD_TIMEOUT) == MSD_OK)
  {
    g_sd_io_err = 0;
    return RES_OK;
  }
  g_sd_io_err = 1;
  Stat |= STA_NOINIT;
  return RES_ERROR;
}
 #endif /* _USE_WRITE == 1 */

/* USER CODE BEGIN beforeIoctlSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END beforeIoctlSection */
/**
  * @brief  I/O control operation
  * @param  lun : not used
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE lun, BYTE cmd, void *buff)
{
  DRESULT res = RES_ERROR;
  BSP_SD_CardInfo CardInfo;

  if (Stat & STA_NOINIT) return RES_NOTRDY;

  switch (cmd)
  {
  /* Make sure that no pending write process */
  case CTRL_SYNC :
    res = RES_OK;
    break;

  /* Get number of sectors on the disk (DWORD) */
  case GET_SECTOR_COUNT :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockNbr;
    res = RES_OK;
    break;

  /* Get R/W sector size (WORD) */
  case GET_SECTOR_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(WORD*)buff = CardInfo.LogBlockSize;
    res = RES_OK;
    break;

  /* Get erase block size in unit of sector (DWORD) */
  case GET_BLOCK_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockSize / SD_DEFAULT_BLOCK_SIZE;
    res = RES_OK;
    break;

  default:
    res = RES_PARERR;
  }

  return res;
}
#endif /* _USE_IOCTL == 1 */

/* USER CODE BEGIN afterIoctlSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END afterIoctlSection */

/* USER CODE BEGIN callbackSection */
/* can be used to modify / following code or add new code */
/* USER CODE END callbackSection */
/**
  * @brief Tx Transfer completed callbacks
  * @param hsd: SD handle
  * @retval None
  */
void BSP_SD_WriteCpltCallback(void)
{

  /*
   * No need to add an "osKernelRunning()" check here, as the SD_initialize()
   * is always called before any SD_Read()/SD_Write() call
   */
#if (osCMSIS < 0x20000U)
   osMessagePut(SDQueueID, WRITE_CPLT_MSG, 0);
#else
   const uint16_t msg = WRITE_CPLT_MSG;
   osMessageQueuePut(SDQueueID, (const void *)&msg, 0, 0);
#endif
}

/**
  * @brief Rx Transfer completed callbacks
  * @param hsd: SD handle
  * @retval None
  */
void BSP_SD_ReadCpltCallback(void)
{
  /*
   * No need to add an "osKernelRunning()" check here, as the SD_initialize()
   * is always called before any SD_Read()/SD_Write() call
   */
#if (osCMSIS < 0x20000U)
   osMessagePut(SDQueueID, READ_CPLT_MSG, 0);
#else
   const uint16_t msg = READ_CPLT_MSG;
   osMessageQueuePut(SDQueueID, (const void *)&msg, 0, 0);
#endif
}

/* USER CODE BEGIN ErrorAbortCallbacks */
/*
void BSP_SD_AbortCallback(void)
{
#if (osCMSIS < 0x20000U)
   osMessagePut(SDQueueID, RW_ABORT_MSG, 0);
#else
   const uint16_t msg = RW_ABORT_MSG;
   osMessageQueuePut(SDQueueID, (const void *)&msg, 0, 0);
#endif
}
*/
/* USER CODE END ErrorAbortCallbacks */

/* USER CODE BEGIN lastSection */
/* can be used to modify / undefine previous code or add new code */
/* USER CODE END lastSection */
