/* Implementation of functions to transmit data or strings to a debug probe
 * via a UART (or USART). The UART emulates the NRZ protocol of SWO. This allows
 * tracing of Cortex M0/M0+ microcontrollers, which do not have TRACESWO
 * support.
 *
 * These routines pack the bytes to transmit into 32-bit words, in order to
 * minimize overhead (each payload item is prefixed with a 1-byte header in the
 * SWO protocol and a payload item is 1 to 4 bytes).
 *
 * Routines for initializing the UART/USART of the micro-controller are not
 * included, as these are dependend on the particular micro-controller. The
 * functions rely on ARM_USART_Send() to do the actual transmission; this is
 * the CMSIS name (you may need to change it for other USART driver libraries).
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
#include "traceswo_uart.h"

uint32_t TRACESWO_TER = 0;
uint32_t TRACESWO_BPS = 0;

uint32_t traceswo_enable(uint32_t channelmask, int enable)
{
  if (enable)
    TRACESWO_TER |= channelmask;
  else
    TRACESWO_TER &= ~channelmask;
  return TRACESWO_TER;
}

void traceswo_sz(int channel, const char *msg)
{
  traceswo_bin(channel, (const unsigned char*)msg, strlen(msg));
}

void traceswo_bin(int channel, const unsigned char *data, unsigned size)
{
  if (TRACESWO_TER & (1 << channel)) {      /* if channel is enabled */
    uint8_t header;
    while (size >= 4) {
      header = (channel << 3) | 3;
      ARM_USART_Send(&header, 1);
      ARM_USART_Send(data, 4);
      data += 4;
      size -= 4;
    }
    if (size >= 2) {
      header = (channel << 3) | 2;
      ARM_USART_Send(&header, 1);
      ARM_USART_Send(data, 2);
      data += 2;
      size -= 2;
    }
    if (size >= 1) {
      header = (channel << 3) | 1;
      ARM_USART_Send(&header, 1);
      ARM_USART_Send(data, 1);
    }
  }
}

