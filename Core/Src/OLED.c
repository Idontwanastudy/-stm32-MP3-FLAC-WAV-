#include "OLED_Font.h"
#include "OLED.h"
#include "gpio.h"
#include "i2c.h"

uint8_t witre_add[2];
uint8_t data_add[2];

uint8_t SramBuffer[8][BUFFER_WIDTH];
uint16_t scrollOffset = 0;
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
	witre_add[0] = 0x00;
	witre_add[1] = Command;
	/*OLED_I2C_Start();
	OLED_I2C_SendByte(0x78);		//从机地址
	OLED_I2C_SendByte(0x00);		//写命令
	OLED_I2C_SendByte(Command); 
	OLED_I2C_Stop();*/
	//HAL_I2C_Mem_Write_IT(&hi2c2, 0x78, 0x00, I2C_MEMADD_SIZE_8BIT, &Command, 1);
	//HAL_I2C_Mem_Write(&hi2c2, 0x78, 0x00, I2C_MEMADD_SIZE_8BIT, &Command, 1, 100);
	//HAL_I2C_Master_Transmit(&hi2c1, 0x78, &witre_add, 2,100);
	HAL_I2C_Master_Transmit_DMA(&hi2c1, 0x78, &witre_add, 2);

	while(HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY);
}

/**
  * @brief  OLED写数据
  * @param  Data 要写入的数据
  * @retval 无
  */
void OLED_WriteData(uint8_t Data)
{
	data_add[0] = 0x40;
	data_add[1] = Data;
	/*OLED_I2C_Start();
	OLED_I2C_SendByte(0x78);		//从机地址
	OLED_I2C_SendByte(0x40);		//写数据
	OLED_I2C_SendByte(Data);
	OLED_I2C_Stop();*/
	//HAL_I2C_Mem_Write_IT(&hi2c2, 0x78, 0x40, I2C_MEMADD_SIZE_8BIT, &Data, 1);
	//HAL_I2C_Mem_Write(&hi2c2, 0x78, 0x40, I2C_MEMADD_SIZE_8BIT, &Data, 1, 100);
	//HAL_I2C_Master_Transmit(&hi2c1, 0x78, &data_add, 2,100);
	HAL_I2C_Master_Transmit_DMA(&hi2c1, 0x78, &data_add, 2);

	while(HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY);

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

/**
  * @brief  OLED显示字符串
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  String 要显示的字符串，范围：ASCII可见字符
  * @retval 无
  */
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String)
{
	uint8_t i;
	for (i = 0; String[i] != '\0'; i++)
	{
		OLED_ShowChar(Line, Column + i, String[i]);
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
	
	MX_I2C1_Init();			//端口初始化
	
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
    uint16_t i, j, k;
    uint16_t len = strlen(String);
    if (len > MAX_CHAR_WIDTH) len = MAX_CHAR_WIDTH; // 限制长度

    for (i = 0; i < len; i++) {
        char ch = String[i];
        if (ch >= ' ' && ch <= '~') {
            uint8_t index = ch - ' ';
            // 将字符的字模数据写入缓冲区的对应位置
            // 每个字符占8列，起始列为 i*8
            for (j = 0; j < 16; j++) { // 一个字符有16字节数据
                // 上半部分 (页0-3) 和 下半部分 (页4-7) 的处理
                // 这里简化处理：假设我们将所有字符放在屏幕的上半部分，即页0-3
                // 实际使用时，你需要根据你的OLED_ShowChar的逻辑，将数据分配到正确的页
                // 一个简单的处理是使用一个更大的8行缓冲区，但为了简化示例，
                // 我们假设字符显示在屏幕顶部两行（页0和页1），因此只填充页0和页1
                if (j < 8) {
                    // 上半部分数据写入页0
                    SramBuffer[0][i*8 + j] = OLED_F8x16[index][j];
                } else {
                    // 下半部分数据写入页1
                    SramBuffer[1][i*8 + (j-8)] = OLED_F8x16[index][j];
                }
            }
        }
    }
}

void OLED_RefreshScreenWithScroll(void) {
    uint8_t i, j;
    // 只刷新参与显示的页，例如页0和页1（一行文字）
    for (j = 0; j < 2; j++) { // 假设我们的文字只占两页
        OLED_SetCursor(j, 0); // 光标移动到页j的起始列
        for (i = 0; i < 128; i++) {
            // 关键步骤：从缓冲区中取出 (scrollOffset + i) 列的数据发送给屏幕
            // 如果超出缓冲区宽度，则取模，实现循环滚动
            uint16_t buffer_col = (scrollOffset + i) % BUFFER_WIDTH;
            OLED_WriteData(SramBuffer[j][buffer_col]);
        }
    }
}



