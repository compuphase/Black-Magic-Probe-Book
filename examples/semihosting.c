/* Implementation of a simple semihosting interface (output only) for ARM Cortex
 * micro-controllers. This implementation is based on CMSIS, but it is easily
 * adapted to libopencm3 or other micro-controller support libraries.
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
#include <string.h>
#include "semihosting.h"


#if defined __ARM_ARCH && __ARM_ARCH == 6
/* overrule HardFault handler for Cortex M0/M0+
   for libopencm3: rename this function to hard_fault_handler */
__attribute__((naked))
void HardFault_Handler(void)
{
  __asm__ (
      "mov  r0, #4\n"   	/* check bit 2 in LR */
      "mov  r1, lr\n"
      "tst  r0, r1\n"
      "beq  msp_stack\n"    /* load either MSP or PSP in r0 */
      "mrs  r0, PSP\n"
      "b    get_fault\n"
  "msp_stack:\n"
      "mrs  r0, MSP\n"
  "get_fault:\n"
      "ldr  r1, [r0,#24]\n" /* read program counter from the stack */
      "ldrh r2, [r1]\n" 	/* read the instruction that caused the fault */
      "ldr  r3, =0xbeab\n"  /* test for BKPT 0xAB (or 0xBEAB) */
      "cmp  r2, r3\n"
      "beq  ignore\n"       /* BKPT 0xAB found, ignore */
      "b    .\n"            /* other reason for HardFault, infinite loop */
  "ignore:\n"
      "add  r1, #2\n"       /* skip behind BKPT 0xAB */
      "str  r1, [r0,#24]\n" /* store this value on the stack */
      "bx   lr"
  );
}
#endif

void host_puts(int file, const char *text)
{
#if defined __ARM_ARCH && __ARM_ARCH == 7
  /* for Cortex M3/M4, test whether we are running under a debugger */
  #define CoreDebug_DHCSR *((uint32_t*)0xE000EDF0UL)
  if (CoreDebug_DHCSR & 1) {
#endif

    uint32_t command = 5;    /*SYS_WRITE*/
    uint32_t packet[3] = { file, (uint32_t)text, strlen(text) };
    __asm__ (
        "mov r0, %0\n"
        "mov r1, %1\n"
        "bkpt #0xAB\n"
      :
      : "r" (command), "r" (packet)
      : "r0", "r1", "memory"
    );

#if defined __ARM_ARCH && __ARM_ARCH == 7
  }
#endif
}

