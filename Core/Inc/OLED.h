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
extern uint8_t SramBuffer[8][BUFFER_WIDTH];
extern uint16_t scrollOffset;

void OLED_Init(void);
void OLED_Clear(void);
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char);
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String);
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length);
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_HorizontalScroll(uint8_t direction, uint8_t start_page, uint8_t end_page, uint8_t speed);
void OLED_Show_float_number(uint8_t Line, uint8_t Column, float number);
void OLED_ClearBuffer(void);
void OLED_DrawStringToBuffer(char *String);
void OLED_RefreshScreenWithScroll(void);


#endif
