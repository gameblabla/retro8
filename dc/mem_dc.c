#include "mem_dc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "sh4_math.h"

void * memcpy6 (void *dest, const void *src, size_t len)
{
  /*if(!len)
  {
    return dest;
  }*/

  const uint8_t *s = (uint8_t *)src;
  uint8_t *d = (uint8_t *)dest;

  uint32_t diff = (uint32_t)d - (uint32_t)(s + 1); // extra offset because input gets incremented before output is calculated
  // Underflow would be like adding a negative offset

  // Can use 'd' as a scratch reg now
  asm volatile (
    "clrs\n" // Align for parallelism (CO) - SH4a use "stc SR, Rn" instead with a dummy Rn
  ".align 2\n"
  "0:\n\t"
    "dt %[size]\n\t" // (--len) ? 0 -> T : 1 -> T (EX 1)
    "mov.b @%[in]+, %[scratch]\n\t" // scratch = *(s++) (LS 1/2)
    "bf.s 0b\n\t" // while(s != nexts) aka while(!T) (BR 1/2)
    " mov.b %[scratch], @(%[offset], %[in])\n" // *(datatype_of_s*) ((char*)s + diff) = scratch, where src + diff = dest (LS 1)
    : [in] "+&r" ((uint32_t)s), [scratch] "=&r" ((uint32_t)d), [size] "+&r" (len) // outputs
    : [offset] "z" (diff) // inputs
    : "t", "memory" // clobbers
  );

  return dest;
}


void * memsetasm (void *dest, const uint8_t val, size_t len)
{
  /*if(!len)
  {
    return dest;
  }*/

  uint8_t * d = (uint8_t*)dest;
  uint8_t * nextd = d + len;

  asm volatile (
    "clrs\n\t" // Align for parallelism (CO) - SH4a use "stc SR, Rn" instead with a dummy Rn
    "dt %[size]\n" // Decrement and test size here once to prevent extra jump (EX 1)
  ".align 2\n"
  "1:\n\t"
    // *--nextd = val
    "mov.b %[in], @-%[out]\n\t" // (LS 1/1)
    "bf.s 1b\n\t" // (BR 1/2)
    " dt %[size]\n" // (--len) ? 0 -> T : 1 -> T (EX 1)
    : [out] "+r" ((uint32_t)nextd), [size] "+&r" (len) // outputs
    : [in] "r" (val) // inputs
    : "t", "memory" // clobbers
  );

  return dest;
}


/* Only if not linked against math lib */
float floorf(float x)
{
	return MATH_Fast_Floorf(x);
}
