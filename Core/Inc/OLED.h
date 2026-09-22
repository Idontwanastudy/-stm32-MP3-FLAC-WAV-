#ifndef __OLED_H
#define __OLED_H

#define MAX_CHAR_WIDTH 100
#define BUFFER_WIDTH (MAX_CHAR_WIDTH * 8)

#define SCROLL_SPEED_2FRAMES   0x07
#define SCROLL_SPEED_3FRAMES   0x04
#define SCROLL_SPEED_4FRAMES   0x05
#define SCROLL_SPEED_5FRAMES   0x00
#define SCROLL_SPEED_25FRAMES  0x06
#define SCROLL_SPEED_64FRAMES  0x01
#define SCROLL_SPEED_128FRAMES 0x02
#define SCROLL_SPEED_256FRAMES 0x03

extern uint8_t srocll_buffer[2][BUFFER_WIDTH];
extern uint8_t SramBuffer[2][BUFFER_WIDTH];   /* 只保留 2 页(歌名滚动行), 为 FLAC 解码器腾 RAM */
extern uint16_t scrollOffset;
extern uint16_t scrollTextWidth;   /* 缓冲中文本总宽度(像素), >128 需滚动 */

void OLED_Init(void);
void OLED_Clear(void);
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char);
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String);
/* 中文显示：Line行(1~4), Column列(1~8, 每汉字占2个ASCII列), utf8为UTF-8编码的中文字符串 */
void OLED_ShowChinese(uint8_t Line, uint8_t Column, char *utf8);
/* 按 GB2312/GBK 区位码显示中文字符串（FatFs _LFN_UNICODE=0 返回的文件名格式） */
void OLED_ShowGBKString(uint8_t Line, uint8_t Column, char *gbk);
/* 字库查找：返回GB2312区位码索引，未找到返回0xFFFF */
uint16_t OLED_GB2312_GetIndex(uint16_t unicode);
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length);
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_HorizontalScroll(uint8_t direction, uint8_t start_page, uint8_t end_page, uint8_t speed);
void OLED_Show_float_number(uint8_t Line, uint8_t Column, float number);
void OLED_ClearBuffer(void);
void OLED_DrawStringToBuffer(char *String);
void OLED_RefreshScreenWithScroll(void);
/* 将 UTF-8 字符串(含中文)渲染到滚动缓冲(页0/页1)并立即显示开头。
 * 长文本由调用方周期性调 OLED_RefreshScreenWithScroll()+scrollOffset++ 实现滚动。 */
void OLED_ShowScrollingString(char *utf8);
/* 把 GBK/GB2312 编码的字符串转换为 UTF-8 (供 OLED_ShowString 等显示) */
void OLED_GBKString_To_UTF8(const char *gbk, char *utf8_out);


#endif
