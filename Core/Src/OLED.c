#include "OLED_Font.h"
#include "OLED.h"
#include "cmsis_os.h"
#include "OLED_GB2312_Quwei.h"   /* GB2312 区位码->Unicode 映射表 */

/* GB2312 汉字字模库 (16x16, 每字32字节) + Unicode 索引表。
 * 该头文件由脚本 OLED_gen_font.py 生成(放入 Core/Inc)。
 * 若尚未生成, 使用内置精简占位, 编译可通过但中文不显示。
 */
#include "OLED_GB2312.h"

/* 字库来源开关:
 *   内 flash 的 GB2312 表(约 230KB) 现在只作"兜底"——W25Q64 字库里没有的字(或字库没烧)时用它。
 *   等你实测 W25Q64 字库一切正常后, 把下面改成 0 就能把这 230KB 从内 flash 里省掉。 */
#define OLED_USE_INTERNAL_GB2312   1

#include "font_store.h"     /* W25Q64 字库(西里尔/假名/谚文/汉字/全角) */

/* F407: I2C1 句柄定义在 main.c, 这里 extern 引用 (F103 是从 i2c.h 引入) */
extern I2C_HandleTypeDef hi2c1;

/* OLED I2C 访问互斥量 (在 freertos.c 中创建)。
 * 滚动定时器回调与音频任务(切歌/暂停等显示)都可能操作 OLED, 用递归互斥量
 * 保证整屏刷新原子、单字节传输互斥, 避免 I2C 传输冲突/全局发送缓冲被覆盖。
 * osKernelGetState() 判断调度器是否已启动: OLED_Init() 在 osKernelInitialize()
 * 之前调用, 那时不能获取互斥量。 */
extern osMutexId_t oled_mutexHandle;

static void OLED_Lock(void) {
    if (oled_mutexHandle != NULL && osKernelGetState() == osKernelRunning) {
        osMutexAcquire(oled_mutexHandle, osWaitForever);
    }
}

static void OLED_Unlock(void) {
    if (oled_mutexHandle != NULL && osKernelGetState() == osKernelRunning) {
        osMutexRelease(oled_mutexHandle);
    }
}

uint8_t witre_add[2];
uint8_t data_add[2];

uint8_t SramBuffer[2][BUFFER_WIDTH];
uint16_t scrollOffset = 0;
uint16_t scrollTextWidth = 0;   /* 当前缓冲中文本总宽度(像素), >128 时需要滚动 */
/*引脚配置*/
//#define OLED_W_SCL(x)		HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6,x)
//#define OLED_W_SDA(x)		HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7,x)
/*引脚初始化*/
/*void OLED_I2C_Init(void)
{
    MX_GPIO_Init();

	OLED_W_SCL(1);
	OLED_W_SDA(1);
}*/

/**
  * @brief  I2C开始
  * @param  无
  * @retval 无
  */
/*void OLED_I2C_Start(void)
{
	OLED_W_SDA(1);
	OLED_W_SCL(1);
	OLED_W_SDA(0);
	OLED_W_SCL(0);
}*/

/**
  * @brief  I2C停止
  * @param  无
  * @retval 无
  */
/*void OLED_I2C_Stop(void)
{
	OLED_W_SDA(0);
	OLED_W_SCL(1);
	OLED_W_SDA(1);
}*/

/**
  * @brief  I2C发送一个字节
  * @param  Byte 要发送的一个字节
  * @retval 无
  */
/*void OLED_I2C_SendByte(uint8_t Byte)
{
	uint8_t i;
	for (i = 0; i < 8; i++)
	{
		OLED_W_SDA(!!(Byte & (0x80 >> i)));
		OLED_W_SCL(1);
		OLED_W_SCL(0);
	}
	OLED_W_SCL(1);	//额外的一个时钟，不处理应答信号
	OLED_W_SCL(0);
}*/

/**
  * @brief  OLED写命令
  * @param  Command 要写入的命令
  * @retval 无
  */
void OLED_WriteCommand(uint8_t Command)
{
	/*OLED_I2C_Start();
	OLED_I2C_SendByte(0x78);		//从机地址
	OLED_I2C_SendByte(0x00);		//写命令
	OLED_I2C_SendByte(Command);
	OLED_I2C_Stop();*/
	//HAL_I2C_Mem_Write_IT(&hi2c2, 0x78, 0x00, I2C_MEMADD_SIZE_8BIT, &Command, 1);
	//HAL_I2C_Mem_Write(&hi2c2, 0x78, 0x00, I2C_MEMADD_SIZE_8BIT, &Command, 1, 100);
	//HAL_I2C_Master_Transmit(&hi2c1, 0x78, &witre_add, 2,100);
	OLED_Lock();
	witre_add[0] = 0x00;
	witre_add[1] = Command;
	HAL_I2C_Master_Transmit_DMA(&hi2c1, 0x78, &witre_add, 2);

	while(HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY);
	OLED_Unlock();
}

/**
  * @brief  OLED写数据
  * @param  Data 要写入的数据
  * @retval 无
  */
void OLED_WriteData(uint8_t Data)
{
	/*OLED_I2C_Start();
	OLED_I2C_SendByte(0x78);		//从机地址
	OLED_I2C_SendByte(0x40);		//写数据
	OLED_I2C_SendByte(Data);
	OLED_I2C_Stop();*/
	//HAL_I2C_Mem_Write_IT(&hi2c2, 0x78, 0x40, I2C_MEMADD_SIZE_8BIT, &Data, 1);
	//HAL_I2C_Mem_Write(&hi2c2, 0x78, 0x40, I2C_MEMADD_SIZE_8BIT, &Data, 1, 100);
	//HAL_I2C_Master_Transmit(&hi2c1, 0x78, &data_add, 2,100);
	OLED_Lock();
	data_add[0] = 0x40;
	data_add[1] = Data;
	HAL_I2C_Master_Transmit_DMA(&hi2c1, 0x78, &data_add, 2);

	while(HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY);
	OLED_Unlock();
}

/**
  * @brief  OLED设置光标位置
  * @param  Y 以左上角为原点，向下方向的坐标，范围：0~7
  * @param  X 以左上角为原点，向右方向的坐标，范围：0~127
  * @retval 无
  */
void OLED_SetCursor(uint8_t Y, uint8_t X)
{
	OLED_WriteCommand(0xB0 | Y);					//设置Y位置
	OLED_WriteCommand(0x10 | ((X & 0xF0) >> 4));	//设置X位置高4位
	OLED_WriteCommand(0x00 | (X & 0x0F));			//设置X位置低4位
}

/**
  * @brief  OLED清屏
  * @param  无
  * @retval 无
  */
void OLED_Clear(void)
{  
	uint8_t i, j;
	for (j = 0; j < 8; j++)
	{
		OLED_SetCursor(j, 0);
		for(i = 0; i < 128; i++)
		{
			OLED_WriteData(0x00);
		}
	}
}

/**
  * @brief  在GB2312字库中查找Unicode字符对应的字模索引
  * @param  unicode 汉字Unicode码点
  * @retval 字模数组索引; 若未找到返回0xFFFF
  * @note   字库按Unicode码点升序排列, 用二分查找加速
  */
uint16_t OLED_GB2312_GetIndex(uint16_t unicode)
{
	uint16_t lo = 0, hi = OLED_GB2312_COUNT - 1, mid;
	if (OLED_GB2312_COUNT == 0) return 0xFFFF;
	while (lo <= hi)
	{
		mid = (lo + hi) / 2;
		if (OLED_GB2312_Unicode[mid] == unicode)
			return mid;
		else if (OLED_GB2312_Unicode[mid] < unicode)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0xFFFF;
}

/**
  * @brief  OLED显示一个汉字
  * @param  Line 行位置，范围：1~4
  * @param  Column 列位置，范围：1~8（每个汉字占2个ASCII列宽）
  * @param  utf8 指向UTF-8编码的汉字字符串（自动取第1个字符）
  * @retval 无
  * @note   字模为16x16点阵, 32字节: 前16字节=上半区(页), 后16字节=下半区
  */
void OLED_ShowChinese(uint8_t Line, uint8_t Column, char *utf8)
{
	uint8_t i;
	uint16_t unicode, index;
	uint8_t *p;

	/* 解析UTF-8三字节汉字: 1110xxxx 10xxxxxx 10xxxxxx */
	p = (uint8_t *)utf8;
	if (p[0] < 0xE0) return;   /* 不是中文(3字节UTF-8) */
	if (p[1] == 0 || p[2] == 0) return;   /* 字符串提前结束, 避免越界 */
	unicode = ((uint16_t)(p[0] & 0x0F) << 12) |
	          ((uint16_t)(p[1] & 0x3F) << 6)  |
	          ((uint16_t)(p[2] & 0x3F));

	index = OLED_GB2312_GetIndex(unicode);
	if (index == 0xFFFF) {
		/* 字库无此字(繁体/日文/生僻字): 用 ?? 占位, 避免显示成空白 */
		OLED_ShowChar(Line, (Column - 1) * 2 + 1, '?');
		OLED_ShowChar(Line, (Column - 1) * 2 + 2, '?');
		return;
	}

	/* 16x16汉字 = 上下两页, 每页16字节(16列) */
	OLED_SetCursor((Line - 1) * 2, (Column - 1) * 8);       /* 上半页 */
	for (i = 0; i < 16; i++)
	{
		OLED_WriteData(OLED_GB2312_FontData[index][i]);
	}
	OLED_SetCursor((Line - 1) * 2 + 1, (Column - 1) * 8);   /* 下半页 */
	for (i = 16; i < 32; i++)
	{
		OLED_WriteData(OLED_GB2312_FontData[index][i]);
	}
}

/**
  * @brief  按 GB2312/GBK 区位码显示中文字符串
  * @param  Line 行位置，范围：1~4
  * @param  Column 列位置，范围：1~8（每个汉字占2个ASCII列宽）
  * @param  gbk 指向 GB2312/GBK 编码的字符串（FatFs _LFN_UNICODE=0 的文件名格式）
  * @retval 无
  * @note   自动识别: ASCII 单字节走 OLED_ShowChar, 汉字双字节(0xA1-0xFE开头)查区位码
  */
void OLED_ShowGBKString(uint8_t Line, uint8_t Column, char *gbk)
{
	uint8_t *p = (uint8_t *)gbk;
	uint16_t index;
	while (*p != 0)
	{
		if (*p < 0x80)
		{
			/* ASCII: 直接显示, 占1列 */
			OLED_ShowChar(Line, Column, (char)*p);
			p++;
			Column++;
		}
		else if (*p >= 0xA1 && *p <= 0xF7 && p[1] != 0)
		{
			/* GB2312 汉字双字节: 区号*p, 位号p[1] */
			uint8_t qu = *p - 0xA1;
			uint8_t wei = p[1] - 0xA1;
			uint16_t idx = (uint16_t)qu * 94 + wei;
			uint16_t unicode;

			if (idx < 8178)
			{
				unicode = OLED_GB2312_QuweiUnicode[idx];
				index = OLED_GB2312_GetIndex(unicode);
			}
			else
			{
				index = 0xFFFF;
			}

			if (index != 0xFFFF)
			{
				uint8_t i;
				OLED_SetCursor((Line - 1) * 2, (Column - 1) * 8);       /* 上半页 */
				for (i = 0; i < 16; i++)
					OLED_WriteData(OLED_GB2312_FontData[index][i]);
				OLED_SetCursor((Line - 1) * 2 + 1, (Column - 1) * 8);   /* 下半页 */
				for (i = 16; i < 32; i++)
					OLED_WriteData(OLED_GB2312_FontData[index][i]);
			}
			else
			{
				/* 字库没有这个字(繁体/日文/生僻字): 用 ?? 占位, 避免显示成空白 */
				OLED_ShowChar(Line, (Column - 1) * 2 + 1, '?');
				OLED_ShowChar(Line, (Column - 1) * 2 + 2, '?');
			}
			Column += 2;
			p += 2;
		}
		else
		{
			/* 无法识别: 跳过1字节 */
			p++;
			Column++;
		}
	}
}

/**
  * @brief  OLED显示一个字符
  * @param  Line 行位置，范围：1~4
  * @param  Column 列位置，范围：1~16
  * @param  Char 要显示的一个字符，范围：ASCII可见字符
  * @retval 无
  */
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char)
{
	uint8_t i;
	OLED_SetCursor((Line - 1) * 2, (Column - 1) * 8);		//设置光标位置在上半部分
	for (i = 0; i < 8; i++)
	{
		OLED_WriteData(OLED_F8x16[Char - ' '][i]);			//显示上半部分内容
	}
	OLED_SetCursor((Line - 1) * 2 + 1, (Column - 1) * 8);	//设置光标位置在下半部分
	for (i = 0; i < 8; i++)
	{
		OLED_WriteData(OLED_F8x16[Char - ' '][i + 8]);		//显示下半部分内容
	}
}

/* ================= UTF-8 取字引擎 =================
 * 三级取字顺序:
 *   1) ASCII(0x20~0x7E) → 内 flash 里专门的 8x16 点阵字体 OLED_F8x16
 *      (它按 8 像素宽设计, 显示英文比把 TTF 塞进 8px 更好看; 也永远可用, 是最终兜底)
 *   2) 其它字符 → 查 W25Q64 字库(西里尔/假名/谚文/汉字/全角, 见 tools/gen_font.py)
 *   3) 都没有 → 退回内 flash 的 GB2312 表(可被 OLED_USE_INTERNAL_GB2312 关掉), 再没有就画 '?'
 * 字形排布: 上页 cols*8 字节 + 下页 cols*8 字节, bit0 = 页顶(与 SSD1306 一致)
 */
static uint32_t OLED_Utf8Next(const char *s, uint8_t *adv)
{
	const uint8_t *p = (const uint8_t *)s;
	if (p[0] < 0x80)           { *adv = 1; return p[0]; }
	if ((p[0] & 0xE0) == 0xC0) {                       /* 2 字节: 西里尔等 */
		if (p[1] == 0) { *adv = 1; return 0xFFFD; }
		*adv = 2;
		return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
	}
	if ((p[0] & 0xF0) == 0xE0) {                       /* 3 字节: 汉字/假名/谚文 */
		if (p[1] == 0 || p[2] == 0) { *adv = 1; return 0xFFFD; }
		*adv = 3;
		return ((uint32_t)(p[0] & 0x0F) << 12) |
		       ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
	}
	if ((p[0] & 0xF8) == 0xF0) { *adv = 4; return 0x10000; }   /* 4 字节(emoji/扩展区): 跳过不画 */
	*adv = 1;
	return 0xFFFD;
}

/* 取字形数据。*cols 回传该字形占几个字符列(1=8 像素宽, 2=16 像素宽); 返回 NULL = 三级都没有 */
static const uint8_t *OLED_GetGlyphData(uint32_t cp, uint8_t *cols)
{
	uint8_t bpg = 0;

	if (cp >= 0x20 && cp <= 0x7E) {          /* 1) ASCII: 内 flash 8x16 */
		*cols = 1;
		return OLED_F8x16[cp - ' '];
	}
	if (font_store_ready()) {                 /* 2) W25Q64 字库 */
		const uint8_t *g = font_store_glyph(cp, &bpg);
		if (g != NULL) {
			*cols = (bpg == 16) ? 1 : 2;
			return g;
		}
	}
#if OLED_USE_INTERNAL_GB2312
	if (cp <= 0xFFFF) {                       /* 3) 内 flash GB2312 兜底 */
		uint16_t idx = OLED_GB2312_GetIndex((uint16_t)cp);
		if (idx != 0xFFFF) {
			*cols = 2;
			return OLED_GB2312_FontData[idx];
		}
	}
#endif
	return NULL;
}

/* 在硬件上画一个码点(Line 1~4, Column 1~16), 返回它占了几个字符列 */
static uint8_t OLED_DrawCodepoint(uint8_t Line, uint8_t Column, uint32_t cp)
{
	const uint8_t *g;
	uint8_t cols = 0, i, width;

	g = OLED_GetGlyphData(cp, &cols);
	if (g == NULL) {                          /* 缺字: 画两个 '?' 占位 */
		OLED_ShowChar(Line, (Column - 1) * 2 + 1, '?');
		OLED_ShowChar(Line, (Column - 1) * 2 + 2, '?');
		return 2;
	}
	width = cols * 8;
	OLED_SetCursor((Line - 1) * 2, (Column - 1) * 8);          /* 上半页 */
	for (i = 0; i < width; i++) {
		OLED_WriteData(g[i]);
	}
	OLED_SetCursor((Line - 1) * 2 + 1, (Column - 1) * 8);      /* 下半页 */
	for (i = width; i < width * 2; i++) {
		OLED_WriteData(g[i]);
	}
	return cols;
}

/**
  * @brief  OLED显示字符串（支持中英俄日韩混排）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  String UTF-8 字符串(ASCII 1 列; 西里尔/汉字/假名/谚文 2 列)
  * @retval 无
  * @note   取字来源见 OLED_GetGlyphData(): 内 flash 8x16 → W25Q64 字库 → 内 flash GB2312
  */
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String)
{
	const char *p = String;

	while (*p != 0 && Column <= 16)
	{
		uint8_t adv = 1;
		uint32_t cp = OLED_Utf8Next(p, &adv);
		p += adv;
		if (cp == 0x10000) continue;                 /* 4 字节字符跳过 */
		Column += OLED_DrawCodepoint(Line, Column, cp);
	}
}

/**
  * @brief  OLED次方函数
  * @retval 返回值等于X的Y次方
  */
uint32_t OLED_Pow(uint32_t X, uint32_t Y)
{
	uint32_t Result = 1;
	while (Y--)
	{
		Result *= X;
	}
	return Result;
}

/**
  * @brief  OLED显示数字（十进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~4294967295
  * @param  Length 要显示数字的长度，范围：1~10
  * @retval 无
  */
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i;
	for (i = 0; i < Length; i++)							
	{
		OLED_ShowChar(Line, Column + i, Number / OLED_Pow(10, Length - i - 1) % 10 + '0');
	}
}

/**
  * @brief  OLED显示数字（十进制，带符号数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：-2147483648~2147483647
  * @param  Length 要显示数字的长度，范围：1~10
  * @retval 无
  */
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length)
{
	uint8_t i;
	uint32_t Number1;
	if (Number >= 0)
	{
		OLED_ShowChar(Line, Column, '+');
		Number1 = Number;
	}
	else
	{
		OLED_ShowChar(Line, Column, '-');
		Number1 = -Number;
	}
	for (i = 0; i < Length; i++)							
	{
		OLED_ShowChar(Line, Column + i + 1, Number1 / OLED_Pow(10, Length - i - 1) % 10 + '0');
	}
}

/**
  * @brief  OLED显示数字（十六进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~0xFFFFFFFF
  * @param  Length 要显示数字的长度，范围：1~8
  * @retval 无
  */
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i, SingleNumber;
	for (i = 0; i < Length; i++)							
	{
		SingleNumber = Number / OLED_Pow(16, Length - i - 1) % 16;
		if (SingleNumber < 10)
		{
			OLED_ShowChar(Line, Column + i, SingleNumber + '0');
		}
		else
		{
			OLED_ShowChar(Line, Column + i, SingleNumber - 10 + 'A');
		}
	}
}

/**
  * @brief  OLED显示数字（二进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~1111 1111 1111 1111
  * @param  Length 要显示数字的长度，范围：1~16
  * @retval 无
  */
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
	uint8_t i;
	for (i = 0; i < Length; i++)							
	{
		OLED_ShowChar(Line, Column + i, Number / OLED_Pow(2, Length - i - 1) % 2 + '0');
	}
}

/**
  * @brief  OLED初始化
  * @param  无
  * @retval 无
  */

void OLED_Show_float_number(uint8_t Line, uint8_t Column, float number)
{
    char String[100], num;

    // 处理符号
    int is_negative = 0;
    if (number < 0) {
        is_negative = 1;
        number = -number;
    }

    int numberh = (int)number;  // 整数部分
    float numberm = number - (float)numberh;  // 小数部分

    // 计算需要显示的小数位数（最多4位）
    int decimal_digits = 0;
    float temp = numberm;
    while (temp > 0.00001 && decimal_digits < 6) {
        temp = temp * 10;
        temp = temp - (int)temp;
        decimal_digits++;
    }

    // 将小数部分转换为整数
    int multiplier = 1;
    for (int k = 0; k < decimal_digits; k++) {
        multiplier *= 10;
    }
    int numberl = (int)(numberm * multiplier + 0.5);  // 四舍五入

    // 特殊处理：如果小数部分为0，也要保留至少1位
    if (decimal_digits == 0 && numberm > 0.00001) {
        decimal_digits = 1;
        numberl = (int)(numberm * 10 + 0.5);
    }

    uint8_t flag = 1, n;
    uint8_t i = 0;

    // 处理小数部分
    if (numberl > 0) {
        flag = 1;
        while (flag != 0) {
            String[i] = (char)(numberl % 10) + '0';
            numberl = numberl / 10;
            i++;
            if (numberl == 0)
                flag = 0;
        }

        // 如果小数位数不足，补0
        while (i < decimal_digits) {
            String[i] = '0';
            i++;
        }

        String[i] = '.';
        i++;
    } else {
        // 没有小数部分，只显示整数
        String[i] = '.';
        i++;
    }

    // 处理整数部分
    flag = 1;
    // 处理0的情况
    if (numberh == 0) {
        String[i] = '0';
        i++;
        flag = 0;
    } else {
        while (flag != 0) {
            String[i] = (char)(numberh % 10) + '0';
            numberh = numberh / 10;
            i++;
            if (numberh == 0)
                flag = 0;
        }
    }

    // 添加负号
    if (is_negative) {
        String[i] = '-';
        i++;
    }

    // 反转字符串
    for (n = 0; n <= (i - 1) / 2; n++) {
        num = String[n];
        String[n] = String[i - 1 - n];
        String[i - 1 - n] = num;
    }

    String[i] = '\0';
    OLED_ShowString(Line, Column, String);
}

void OLED_Init(void)
{
	uint32_t i, j;
	
	for (i = 0; i < 1000; i++)			//上电延时
	{
		for (j = 0; j < 1000; j++);
	}
	
	/* F407: I2C1 已在 main.c 的 MX_I2C1_Init() 初始化, 这里不再重复 */
	
	OLED_WriteCommand(0xAE);	//关闭显示
	
	OLED_WriteCommand(0xD5);	//设置显示时钟分频比/振荡器频率
	OLED_WriteCommand(0x80);
	
	OLED_WriteCommand(0xA8);	//设置多路复用率
	OLED_WriteCommand(0x3F);
	
	OLED_WriteCommand(0xD3);	//设置显示偏移
	OLED_WriteCommand(0x00);
	
	OLED_WriteCommand(0x40);	//设置显示开始行
	
	OLED_WriteCommand(0xA1);	//设置左右方向，0xA1正常 0xA0左右反置
	
	OLED_WriteCommand(0xC8);	//设置上下方向，0xC8正常 0xC0上下反置

	OLED_WriteCommand(0xDA);	//设置COM引脚硬件配置
	OLED_WriteCommand(0x12);
	
	OLED_WriteCommand(0x81);	//设置对比度控制
	OLED_WriteCommand(0xCF);

	OLED_WriteCommand(0xD9);	//设置预充电周期
	OLED_WriteCommand(0xF1);

	OLED_WriteCommand(0xDB);	//设置VCOMH取消选择级别
	OLED_WriteCommand(0x30);

	OLED_WriteCommand(0xA4);	//设置整个显示打开/关闭

	OLED_WriteCommand(0xA6);	//设置正常/倒转显示

	OLED_WriteCommand(0x8D);	//设置充电泵
	OLED_WriteCommand(0x14);

	OLED_WriteCommand(0xAF);	//开启显示
		
	OLED_Clear();				//OLED清屏
}

void OLED_HorizontalScroll(uint8_t direction, uint8_t start_page, uint8_t end_page, uint8_t speed) {
    // 停止滚动
    OLED_WriteCommand(0x2E);
    // 设置方向：0x26 右，0x27 左
    if(direction == 0) // 右
        OLED_WriteCommand(0x26);
    else
        OLED_WriteCommand(0x27);
    // 虚拟字节
    OLED_WriteCommand(0x00);
    // 起始页
    OLED_WriteCommand(start_page & 0x07);
    // 速度
    OLED_WriteCommand(speed);
    // 结束页
    OLED_WriteCommand(end_page & 0x07);
    // 虚拟字节
    OLED_WriteCommand(0x00);
    // 虚拟字节
    OLED_WriteCommand(0xFF);
    // 启动滚动
    OLED_WriteCommand(0x2F);
}

void OLED_ClearBuffer(void) {
    memset(SramBuffer, 0, sizeof(SramBuffer));
}

void OLED_DrawStringToBuffer(char *String) {
	scrollOffset = 0;
	uint16_t col = 0;
	const char *p = String;

	/* 清空显示缓冲(页0/页1) */
	memset(SramBuffer[0], 0, BUFFER_WIDTH);
	memset(SramBuffer[1], 0, BUFFER_WIDTH);

	while (*p != 0 && col < (BUFFER_WIDTH - 16)) {
		uint8_t adv = 1, cols = 0;
		uint32_t cp = OLED_Utf8Next(p, &adv);
		p += adv;
		if (cp == 0x10000) continue;               /* 4 字节字符跳过 */

		{
			const uint8_t *g = OLED_GetGlyphData(cp, &cols);
			uint8_t width, j;
			if (g == NULL) {                       /* 缺字: 用两个 '?' 顶位 */
				g = (const uint8_t *)OLED_F8x16['?' - ' '];
				cols = 1;
			}
			width = cols * 8;
			for (j = 0; j < width; j++) {
				SramBuffer[0][col + j] = g[j];             /* 上半页 */
				SramBuffer[1][col + j] = g[width + j];     /* 下半页 */
			}
			col += width;
		}
	}
	scrollTextWidth = col;   /* 记录文本总宽度(像素) */
}

/**
 * @brief  渲染 UTF-8 字符串(含中文)到滚动缓冲并立即显示开头
 * @param  utf8  UTF-8 字符串
 * @retval 无
 * @note   长文本由调用方周期性调用 OLED_RefreshScreenWithScroll() + scrollOffset++
 *         实现横向滚动显示
 */
void OLED_ShowScrollingString(char *utf8) {
	OLED_DrawStringToBuffer(utf8);
	OLED_RefreshScreenWithScroll();
}

/**
 * @brief  把 GBK/GB2312 编码字符串转换为 UTF-8
 * @param  gbk      输入 GBK 字符串
 * @param  utf8_out 输出 UTF-8 字符串缓冲(需足够大, 中文最多3x输入长度)
 * @retval 无
 */
void OLED_GBKString_To_UTF8(const char *gbk, char *utf8_out) {
	const uint8_t *p = (const uint8_t *)gbk;
	uint8_t *o = (uint8_t *)utf8_out;

	while (*p != 0) {
		if (*p < 0x80) {
			/* ASCII 直接拷贝 */
			*o++ = *p++;
		}
		else if (*p >= 0xA1 && *p <= 0xF7 && p[1] != 0) {
			/* GB2312 双字节汉字 */
			uint8_t qu = *p++ - 0xA1;
			uint8_t wei = *p++ - 0xA1;
			uint16_t idx = (uint16_t)qu * 94 + wei;
			uint16_t unicode = (idx < 8178) ? OLED_GB2312_QuweiUnicode[idx] : 0;
			if (unicode != 0) {
				/* Unicode -> UTF-8 三字节 */
				*o++ = (uint8_t)(0xE0 | ((unicode >> 12) & 0x0F));
				*o++ = (uint8_t)(0x80 | ((unicode >> 6) & 0x3F));
				*o++ = (uint8_t)(0x80 | (unicode & 0x3F));
			} else {
				/* 超出 GB2312 的字符, 用 '?' 占位 */
				*o++ = '?';
			}
		}
		else {
			/* 无法识别, 跳过 */
			*o++ = *p++;
		}
	}
	*o = 0;
}

void OLED_RefreshScreenWithScroll(void) {
    uint8_t i, j;
    uint16_t col = scrollOffset;   /* 本次刷新使用一致的偏移, 避免中途被改写 */
    /* 递归互斥: 外层持锁使整个刷屏序列原子, 不会被切歌/暂停的显示操作插入导致花屏 */
    OLED_Lock();
    // 只刷新参与显示的页，例如页0和页1（一行文字）
    for (j = 0; j < 2; j++) { // 假设我们的文字只占两页
        OLED_SetCursor(j, 0); // 光标移动到页j的起始列
        for (i = 0; i < 128; i++) {
            // 关键步骤：从缓冲区中取出 (scrollOffset + i) 列的数据发送给屏幕
            // 如果超出缓冲区宽度，则取模，实现循环滚动
            uint16_t buffer_col = (col + i) % BUFFER_WIDTH;
            OLED_WriteData(SramBuffer[j][buffer_col]);
        }
    }
    OLED_Unlock();
}



