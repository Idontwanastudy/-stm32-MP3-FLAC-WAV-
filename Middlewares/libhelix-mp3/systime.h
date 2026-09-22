/* systime.h - Helix PROFILE 用计时接口 (未定义 PROFILE 时不使用, 此处为空实现) */
#ifndef _SYSTIME_H
#define _SYSTIME_H
static inline long systime_get(void) { return 0; }
#endif
