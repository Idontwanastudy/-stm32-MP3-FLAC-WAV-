/*
 * w25q64.h - W25Q64 SPI Flash 驱动 (F407, SPI1, CS=PA4)
 *
 * 用法: 移植后先调 W25Q64_ReadID() 验证接线, 正常应返回 0xEF14 (W25Q64 厂家/型号)。
 * 注意: hspi1 句柄定义在 main.c, CS 引脚 PA4 在 MX_GPIO_Init 里配置(USER CODE 区)。
 */
#ifndef __W25Q64_H
#define __W25Q64_H

#include "main.h"

/* SPI1 句柄 (定义在 main.c) */
extern SPI_HandleTypeDef hspi1;

/* W25Q64 片选: PA4 (低有效) */
#define W25Q64_CS_LOW()   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET)
#define W25Q64_CS_HIGH()  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET)

/* 命令 */
#define W25Q_CMD_READ_ID       0x90   /* JEDEC ID */
#define W25Q_CMD_READ_DATA     0x03   /* 读数据 */
#define W25Q_CMD_WRITE_ENABLE  0x06   /* 写使能 */
#define W25Q_CMD_READ_STATUS   0x05   /* 读状态寄存器 */
#define W25Q_CMD_PAGE_PROGRAM  0x02   /* 页编程(<=256B) */
#define W25Q_CMD_ERASE_SECTOR  0x20   /* 扇区擦除 4KB */

/* 容量: W25Q64 = 8MB = 8,388,608 字节 */
#define W25Q64_TOTAL_SIZE   (8u * 1024u * 1024u)
#define W25Q64_PAGE_SIZE    256u
#define W25Q64_SECTOR_SIZE  4096u

/* 读 JEDEC ID, 正常返回 0xEF14 (高字节=厂家0xEF, 低字节=型号0x14) */
uint32_t W25Q64_ReadID(void);

/* 从 addr 读 len 字节到 buf */
void W25Q64_ReadData(uint32_t addr, uint8_t *buf, uint32_t len);

/* 擦除 addr 所在的 4KB 扇区 */
void W25Q64_EraseSector(uint32_t addr);

/* 写 len(<=256) 字节到 addr 起始的页 (addr 须按 256 对齐, 跨页需多次调用) */
void W25Q64_WritePage(uint32_t addr, uint8_t *buf, uint16_t len);

#endif /* __W25Q64_H */
