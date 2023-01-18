/* Implementation of a simple semihosting interface (output only) for ARM Cortex
 * micro-controllers. This implementation is based on CMSIS, and restricted to
 * the semihosting calls supported by the Black Magic Probe. It is easily
 * adapted to libopencm3 or other micro-controller support libraries. Likewise,
 * it is easily extended with the few semihosting calls that the Black Magic
 * Probe does not support.
 *
 * Copyright 2020-2023 CompuPhase
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
      "mov  r0, #0 \n"      /* set error code (r0 == -1) */
      "sub  r0, #1 \n"
      "bx   lr"
  );
}
#endif

__attribute__((naked))
int semihosting(uint32_t command, void *params)
{
#if defined __ARM_ARCH && __ARM_ARCH == 7
  /* for Cortex M3/M4, test whether we are running under a debugger (for
     Cortex M0(+), the HardFault exception handler intercepts the call) */
# define CoreDebug_DHCSR *((uint32_t*)0xE000EDF0UL)
  if (CoreDebug_DHCSR & 1) {
#endif

    __asm__ (
        "mov  r0, %0 \n"
        "mov  r1, %1 \n"
        "bkpt #0xAB \n"
        "bx   lr \n"
      :
      : "r" (command), "r" (params)
      : "r0", "r1", "memory"
    );

#if defined __ARM_ARCH && __ARM_ARCH == 7
  }
#endif
}

int sys_open(const char *path, const char *mode)
{
  uint32_t params[3] = { (uint32_t)path, 0, strlen(path) };
  if (*mode == 'r') {
    mode++;
  } else if (*mode == 'w') {
    params[1] |= 0x04;
    mode++;
  } else if (*mode == 'a') {
    params[1] |= 0x08;
    mode++;
  }
  if (*mode == '+') {
    params[1] |= 0x02;
    mode++;
  }
  if (*mode == 'b') {
    params[1] |= 0x01;
    mode++;
  }
  return semihosting(SYS_OPEN, params);
}

int sys_close(int fd)
{
  return semihosting(SYS_CLOSE, &fd);
}

void sys_writec(char c)
{
  semihosting(SYS_WRITEC, &c);
}

void sys_write0(const char *text)
{
  semihosting(SYS_WRITE0, (char*)text);
}

int sys_write(int fd, const unsigned char *buffer, size_t size)
{
  uint32_t params[3] = { fd, (uint32_t)buffer, size };
  return semihosting(SYS_WRITE, params);
}

int sys_read(int fd, char *buffer, size_t size)
{
  uint32_t params[3] = { fd, (uint32_t)buffer, size };
  return semihosting(SYS_READ, params);
}

int sys_readc(void)
{
  return semihosting(SYS_READC, NULL);
}

int sys_iserror(uint32_t code)
{
  return semihosting(SYS_ISERROR, &code);
}

int sys_istty(int fd)
{
  return semihosting(SYS_ISTTY, &fd);
}

int sys_seek(int fd, size_t offset)
{
  uint32_t params[2] = { fd, offset };
  return semihosting(SYS_SEEK, params);
}

int sys_flen(int fd)
{
  return semihosting(SYS_FLEN, &fd);
}

int sys_tmpnam(int id, char *buffer, size_t size)
{
  uint32_t params[3] = { (uint32_t)buffer, (id & 0xff), size };
  return semihosting(SYS_TMPNAM, params);
}

int sys_remove(const char *path)
{
  uint32_t params[2] = { (uint32_t)path, strlen(path) };
  return semihosting(SYS_REMOVE, params);
}

int sys_rename(const char *from, const char *to)
{
  uint32_t params[4] = { (uint32_t)from, strlen(from), (uint32_t)to, strlen(to) };
  return semihosting(SYS_RENAME, params);
}

int sys_clock(void)
{
  return semihosting(SYS_CLOCK, NULL);
}

int sys_time(void)
{
  return semihosting(SYS_TIME, NULL);
}

int sys_system(const char *command)
{
  uint32_t params[2] = { (uint32_t)command, strlen(command) };
  return semihosting(SYS_SYSTEM, params);
}

int sys_errno(void)
{
  return semihosting(SYS_ERRNO, NULL);
}

void sys_get_cmdline(char *buffer, size_t size)
{
  uint32_t params[2] = { (uint32_t)buffer, size };
  semihosting(SYS_GET_CMDLINE, params);
}

void sys_heapinfo(struct heapinfo *info)
{
  semihosting(SYS_HEAPINFO, &info);
}

void sys_exit(int trap)
{
  semihosting(SYS_EXIT, (void*)trap);
}

void sys_exit_extended(int trap, int subcode)
{
  uint32_t params[2] = { trap, subcode };
  semihosting(SYS_EXIT_EXTENDED, params);
}

