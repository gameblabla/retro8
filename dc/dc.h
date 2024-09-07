#ifndef DC_H
#define DC_H

#include "mem_dc.h"

#if !defined(DREAMCAST) && !defined(NOOPT)

#ifdef __cplusplus
#define FLOOR_REAL std::floor
#define CEIL_REAL std::ceil
#define MIN_REAL std::min
#define MAX_REAL std::max
#define ABS_REAL std::abs
#define SIN_REAL std::sin
#define COS_REAL std::cos
#else
#define FLOOR_REAL floor
#define CEIL_REAL ceil
#define MIN_REAL min
#define MAX_REAL max
#define ABS_REAL abs
#define SIN_REAL sin
#define COS_REAL cos
#endif

#define SQRTF_REAL sqrtf
#define DIVIDE_REAL(a,b) (a / b)
#define MEMSET_REAL memset
#define MEMCPY_REAL memcpy

#define SUPER_MEMCPY_REAL memcpy

#define FMAC(a, b, c) ((a) * (b) + (c))
#define FMAC_DEC(a, b, c) ((a) * (b) - (c))

#else
#include "sh4_math.h"
#include <dc/fmath.h>
#define FLOOR_REAL MATH_Fast_Floorf
// MATH_Very_Fast_Floorf doesn't work properly with Dusk Child

#define CEIL_REAL MATH_Very_Fast_Ceilf
#define MIN_REAL MATH_Fast_Fminf
#define MAX_REAL MATH_Fast_Fmaxf

#define ABS_REAL MATH_fabs
#define SQRTF_REAL MATH_Fast_Sqrt

#define SIN_REAL fsin
#define COS_REAL fcos
	
#define DIVIDE_REAL(a,b) MATH_Fast_Divide(a, b)
#define MEMSET_REAL memsetasm
#define MEMCPY_REAL bit64_sq_cpy

#define SUPER_MEMCPY_REAL bit64_sq_cpy

#define FMAC(a, b, c) MATH_fmac(a,b,c)
#define FMAC_DEC(a, b, c) MATH_fmac_Dec(a,b,c)


#endif


#endif
