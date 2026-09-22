/*
 * w25q64.c - W25Q64 SPI Flash 驱动实现
 *
 * 基于 HAL_SPI 阻塞传输 (SPI1), CS 为 PA4 软件控制。
 * 参考 W25Q64JV 数据手册命令集。
 */
#include "w25q64.h"

/* 发送一条无数据命令 (只写命令字节) */
static void W25Q64_SendCmd(uint8_t cmd)
{
    W25Q64_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, 1000);
    W25Q64_CS_HIGH();
}

/* 写使能 (任何写/擦除前必须调用) */
static void W25Q64_WriteEnable(void)
{
    W25Q64_SendCmd(W25Q_CMD_WRITE_ENABLE);
}

/* 等待 BUSY 位清零 (写/擦除进行中) */
static void W25Q64_WaitBusy(void)
{
    uint8_t cmd = W25Q_CMD_READ_STATUS;
    uint8_t status = 0;
    do {
        W25Q64_CS_LOW();
        HAL_SPI_Transmit(&hspi1, &cmd, 1, 1000);
        HAL_SPI_Receive(&hspi1, &status, 1, 1000);
        W25Q64_CS_HIGH();
    } while (status & 0x01);   /* bit0 = BUSY */
}

uint32_t W25Q64_ReadID(void)
{
    uint8_t cmd[4] = {W25Q_CMD_READ_ID, 0x00, 0x00, 0x00};   /* 0x90 + 3 字节地址 */
    uint8_t id[2] = {0, 0};

    W25Q64_CS_LOW();
    HAL_SPI_Transmit(&hspi1, cmd, 4, 1000);
    HAL_SPI_Receive(&hspi1, id, 2, 1000);
    W25Q64_CS_HIGH();

    return ((uint32_t)id[0] << 8) | id[1];
}

void W25Q64_ReadData(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t cmd[4] = {
        W25Q_CMD_READ_DATA,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)(addr)
    };

    W25Q64_CS_LOW();
    HAL_SPI_Transmit(&hspi1, cmd, 4, 1000);
    while (len > 0) {
        /* 分块传输, 每块 <= 4096 字节 */
        uint32_t chunk = (len > 4096) ? 4096 : len;
        HAL_SPI_Receive(&hspi1, buf, chunk, 1000);
        buf += chunk;
        len -= chunk;
    }
    W25Q64_CS_HIGH();
}

void W25Q64_EraseSector(uint32_t addr)
{
    uint8_t cmd[4] = {
        W25Q_CMD_ERASE_SECTOR,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)(addr)
    };

    W25Q64_WriteEnable();
    W25Q64_CS_LOW();
    HAL_SPI_Transmit(&hspi1, cmd, 4, 1000);
    W25Q64_CS_HIGH();
    W25Q64_WaitBusy();
}

void W25Q64_WritePage(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t cmd[4] = {
        W25Q_CMD_PAGE_PROGRAM,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)(addr)
    };

    if (len == 0 || len > W25Q64_PAGE_SIZE) return;

    W25Q64_WriteEnable();
    W25Q64_CS_LOW();
    HAL_SPI_Transmit(&hspi1, cmd, 4, 1000);
    HAL_SPI_Transmit(&hspi1, buf, len, 1000);
    W25Q64_CS_HIGH();
    W25Q64_WaitBusy();
}
