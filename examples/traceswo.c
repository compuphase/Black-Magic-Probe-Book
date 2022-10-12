/* Implementation of functions to transmit data or strings over the TRACESWO
 * wire of the ARM Cortex micro-controllers.
 *
 * These routines pack the bytes to transmit into 32-bit words, in order to
 * minimize overhead (each item that is transmitted over the TRACESWO pin is
 * prefixed with a 1-byte header, so that when tranmitting single bytes, each
 * byte has that 1-byte header overhead).
 *
 * Routines for initializing the micro-controller for TRACESWO are not included,
 * as these are (in part) dependend on the particular micro-controller.
 *
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
#include "traceswo.h"

void traceswo_sz(int channel, const char *msg)
{
  traceswo_bin(channel, msg, strlen(msg));
}

void traceswo_bin(int channel, const unsigned char *data, unsigned size)
{
  if ((ITM->TCR & ITM_TCR_ITMENA) != 0UL &&   /* ITM tracing enabled */
      (ITM->TER & (1 << channel)) != 0UL)     /* ITM channel enabled */
  {
    /* collect and transmit bytes in packets of 4 bytes */
    uint32_t value = 0, shift = 0;
    while (size-- > 0) {
      value |= (uint32_t)*data++ << shift;
      shift += 8;
      if (shift >= 32) {
        while (ITM->PORT[channel].u32 == 0UL)
          __NOP();
        ITM->PORT[channel].u32 = value;
        value = shift = 0;
      }
    }
    /* transmit last collected bytes */
    if (shift > 0) {
      while (ITM->PORT[channel].u32 == 0UL)
        __NOP();
      ITM->PORT[channel].u32 = value;
    }
  }
}

