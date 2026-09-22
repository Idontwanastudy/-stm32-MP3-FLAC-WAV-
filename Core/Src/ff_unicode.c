/* ff_unicode.c - FatFs R0.11 LFN 必需的 OEM<->Unicode 转换函数
 *
 * 说明:
 *   FatFs 在 _USE_LFN 开启后, 需要 ff_convert() 和 ff_wtoupper()
 *   两个外部函数(通常位于 option/unicode.c)。本工程没有该文件,
 *   这里基于项目已有的 GB2312 区位码->Unicode 表自行实现,
 *   以支持简体中文(GBK/GB2312)长文件名。
 *
 *   注意: _LFN_UNICODE 保持为 0(ANSI/OEM 字符串),
 *   因此文件名以 GBK 编码的 char 数组返回。
 */

#include "ff.h"            /* FatFs 类型与声明 (WCHAR, UINT, ff_convert...) */
#include "integer.h"       /* 基础类型 */
#include "OLED_GB2312_Quwei.h"   /* GB2312 区位码 -> Unicode 映射表 */

/**
 * @brief  OEM(GBK) 与 Unicode 双向转换
 * @param  chr: 待转换字符
 *         dir: 1=OEM->Unicode, 0=Unicode->OEM
 * @retval 转换后的字符; 失败返回 0 (FatFs 会以 '?' 处理)
 * @note   仅覆盖 GB2312 一级/二级汉字 + ASCII。
 *         超出 GB2312 范围的 GBK 扩展字符(生僻字)无法转换, 返回 0。
 */
WCHAR ff_convert (WCHAR chr, UINT dir)
{
    /* ---------- Unicode -> OEM (dir=0): 路径字符串转 GBK ---------- */
    if (dir == 0)
    {
        if (chr < 0x80) return chr;              /* ASCII 单字节 */
        /* 在 GB2312 表中查找该 Unicode 对应的区位码 */
        {
            UINT i;
            for (i = 0; i < 8178; i++)
            {
                if (OLED_GB2312_QuweiUnicode[i] == chr)
                {
                    BYTE b1 = (BYTE)(0xA1 + i / 94);   /* 区号 */
                    BYTE b2 = (BYTE)(0xA1 + i % 94);   /* 位号 */
                    return (WCHAR)(((WCHAR)b1 << 8) | b2);  /* GBK 双字节 */
                }
            }
        }
        return 0;   /* 未找到 */
    }
    /* ---------- OEM -> Unicode (dir=1): GBK 文件名字符转 Unicode ---------- */
    else
    {
        if (chr < 0x80) return chr;              /* ASCII 单字节 */
        {
            BYTE b1 = (BYTE)(chr >> 8);          /* 区号 */
            BYTE b2 = (BYTE)(chr & 0xFF);        /* 位号 */
            /* 仅处理 GB2312 汉字区 0xA1A1-0xF7FE */
            if (b1 >= 0xA1 && b1 <= 0xF7 && b2 >= 0xA1 && b2 <= 0xFE)
            {
                UINT idx = ((UINT)(b1 - 0xA1) * 94) + (UINT)(b2 - 0xA1);
                if (idx < 8178)
                {
                    return OLED_GB2312_QuweiUnicode[idx];
                }
            }
        }
        return 0;   /* 非 GB2312 范围 */
    }
}

/**
 * @brief  Unicode 字符大写转换 (用于大小写不敏感的文件名比较)
 * @param  chr: Unicode 字符
 * @retval 大写形式; 汉字无大小写, 原样返回
 */
WCHAR ff_wtoupper (WCHAR chr)
{
    /* ASCII 小写转大写 */
    if (chr >= 'a' && chr <= 'z')
    {
        return (WCHAR)(chr - ('a' - 'A'));
    }
    /* 汉字等其它字符无大小写概念, 原样返回 */
    return chr;
}
