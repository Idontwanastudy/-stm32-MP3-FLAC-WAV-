/*
 * font_store.h - 字库存储层: 把 W25Q64 当字库盘 + 从 SD 卡烧写
 *
 * 分工:
 *   font_format.c  纯逻辑(解析/查表/缓存, 不依赖 HAL, 可宿主机测试)
 *   font_store.c   胶水: 用 W25Q64 读字库, 从 SD 卡上的 font16.bin 烧写并校验
 *
 * W25Q64 分区(8MB, 本工程此前未使用这块 flash, 8MB 全空):
 *   0x000000  字库(头部+区段表+字形数据, 六种文字全含约 1MB)
 *   以后如需多字号/图标, 从 0x200000 往后排。
 */
#ifndef __FONT_STORE_H
#define __FONT_STORE_H

#include <stdint.h>

#define FONT_FLASH_ADDR      0x000000u                     /* 字库在 W25Q64 里的起始地址 */
#define FONT_FLASH_MAX_SIZE  (2u * 1024u * 1024u)          /* 允许的字库最大长度(2MB) */

/* 从 W25Q64 读字库头部并校验(默认还会整段校验 CRC)。返回 0=可用, <0=失败 */
int  font_store_init(void);

/* 字库是否可用(1=可用) */
uint8_t font_store_ready(void);

/* 字库总字节数(0=不可用) 与 区段个数, 供状态显示用 */
uint32_t font_store_size(void);
uint16_t font_store_ranges(void);

/* 查字形: 命中返回指向该字形的指针(下次调用前有效), 缺字返回 NULL。
 * bpg_out 回传字节数: 16=8x16(占 1 列) / 32=16x16(占 2 列) */
const uint8_t *font_store_glyph(uint32_t cp, uint8_t *bpg_out);

/* 从 SD 卡文件烧写字库到 W25Q64(擦除→写入→回读校验)。
 * progress 可为 NULL; 非空时每烧完一段调用一次(用于 OLED 显示进度)。
 * 返回 0=成功 1=打不开文件 2=文件太短/太大 3=源文件魔数或CRC不对 4=烧完回读CRC不符 */
uint8_t font_store_burn_file(const char *path,
                             void (*progress)(uint32_t done, uint32_t total));

/* 上电自动检查: SD 上有 font16.bin 且与 Flash 里的字库不是同一份(或 Flash 里没有/校验不过)时就烧它。
 * 返回 0=无需烧写 1=已烧写成功 <0=出错(见 font_store_burn_file 的返回码) */
int font_store_boot_check(const char *sd_path,
                          void (*progress)(uint32_t done, uint32_t total));

#endif /* __FONT_STORE_H */
