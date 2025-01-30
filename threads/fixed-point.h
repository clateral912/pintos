#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include <stdint.h>

#define FRACTION_BITS 14
#define FRACTION_SCALE (1 << FRACTION_BITS)
#define LOADAVG_COEFF_59_60 16111
#define LOADAVG_COEFF_01_60 273

int32_t fp_convert_to_fp(int32_t n);
int32_t fp_convert_to_int_rdn(int32_t x);
int32_t fp_convert_to_int_rd0(int32_t x);
int32_t fp_add(int32_t x, int32_t y);
int32_t fp_add_int(int32_t x, int32_t n);
int32_t fp_sub(int32_t x, int32_t y);
int32_t fp_sub_int(int32_t x, int32_t n);
int32_t fp_multiply(int32_t x, int32_t y);
int32_t fp_multiply_by_int(int32_t x, int32_t n);
int32_t fp_divide(int32_t x, int32_t y);
int32_t fp_divide_by_int(int32_t x, int32_t n);

// 将整数转换为定点数
inline int32_t 
fp_convert_to_fp(int32_t n)
{
  return n << FRACTION_BITS;
}

// 将定点数转换为整数（向零舍入）
inline int32_t 
fp_convert_to_int_rd0(int32_t x)
{
  return x >> FRACTION_BITS;
}

// 将定点数转换为整数（四舍五入）
inline int32_t 
fp_convert_to_int_rdn(int32_t x)
{
  return x >= 0 ? ((x + (1 << (FRACTION_BITS - 1))) >> FRACTION_BITS) 
                : ((x - (1 << (FRACTION_BITS - 1))) >> FRACTION_BITS);
}

// 定点数加法
inline int32_t 
fp_add(int32_t x, int32_t y)
{
  return x + y;
}

// 定点数与整数加法
inline int32_t 
fp_add_int(int32_t x, int32_t n)
{
  return x + (n << FRACTION_BITS);
}

// 定点数减法
inline int32_t 
fp_sub(int32_t x, int32_t y)
{
  return x - y;
}

// 定点数与整数减法
inline int32_t 
fp_sub_int(int32_t x, int32_t n)
{
  return x - (n << FRACTION_BITS);
}

// 定点数乘法
inline int32_t 
fp_multiply(int32_t x, int32_t y)
{
  return (int32_t)(((int64_t) x * y) >> FRACTION_BITS);
}

// 定点数与整数乘法
inline int32_t 
fp_multiply_by_int(int32_t x, int32_t n)
{
  return x * n;
}

// 定点数除法
inline int32_t 
fp_divide(int32_t x, int32_t y)
{
  return (int32_t)(((int64_t) x << FRACTION_BITS) / y);
}

// 定点数与整数除法
inline int32_t 
fp_divide_by_int(int32_t x, int32_t n)
{
  return x / n;
}

#endif //FIXED_POINT_H
