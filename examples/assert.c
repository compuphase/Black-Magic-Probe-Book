/* Implementation of assertions for ARM Cortex micro-controllers, based
 * on semihosting.
 *
 * Copyright 2020 CompuPhase
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <stdint.h>
#include "assert.h"
#include "semihosting.h"

#ifndef NDEBUG

#define __BKPT(value)	__asm volatile ("bkpt "#value)

__attribute__ ((always_inline)) static inline uint32_t __get_LR(void)
{
  register uint32_t result;
  __asm volatile("mov %0, lr\n" : "=r" (result));
  return result;
}

static void addr_to_string(uint32_t addr, char *str)
{
  int i = sizeof(addr) * 2;   /* always do 8 digits for a 32-bit value */
  str[i]= '\0';
  while (i > 0) {
    int digit = addr & 0x0f;
    str[--i] = (digit > 9) ? digit +('a' - 10) : digit + '0';
    addr >>= 4;
  }
}

__attribute__ ((weak)) void assert_abort(void)
{
  __BKPT(0);
}

void assert_fail(void)
{
  register uint32_t addr = (__get_LR() & ~1) - 4;
  char buffer[] = "Assertion failed at *0x00000000\n";
  addr_to_string(addr, buffer + 23);
  host_puts(STDERR, buffer);

  assert_abort();
}

#endif /* NDEBUG */
