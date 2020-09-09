/* Implementation of functions to transmit data or strings to a debug probe
 * via a SPI. It emulates the Manchester protocol of SWO. This allows tracing
 * of Cortex M0/M0+ microcontrollers, which do not have TRACESWO support.
 *
 * These routines pack the bytes to transmit into 32-bit words, in order to
 * minimize overhead (each payload item is prefixed with a 1-byte header in the
 * SWO protocol and a payload item is 1 to 4 bytes).
 *
 * Routines for initializing the SPI peripheral of the micro-controller are not
 * included, as these are dependend on the particular micro-controller. In fact,
 * only the MOSI line is used (and connected to the debug probe); the SPI clock
 * line is not connected and neither are "slave select" and MISO lines.
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
#include "traceswo_spi.h"

#define START   0x02  /* 0000 0010 (space for 3 periods, followed by a '1') */
#define SPACE   0x00  /* space code for at the end of a transfer */

static const uint8_t manchester_lookup[16] = {
  0x55, /* 0000 -> 0101 0101 */
  0x95, /* 0001 -> 1001 0101 */
  0x65, /* 0010 -> 0110 0101 */
  0xa5, /* 0011 -> 1010 0101 */
  0x59, /* 0100 -> 0101 1001 */
  0x99, /* 0101 -> 1001 1001 */
  0x69, /* 0110 -> 0110 1001 */
  0xa9, /* 0111 -> 1010 1001 */
  0x56, /* 1000 -> 0101 0110 */
  0x96, /* 1001 -> 1001 0110 */
  0x66, /* 1010 -> 0110 0110 */
  0xa6, /* 1011 -> 1010 0110 */
  0x5a, /* 1100 -> 0101 1010 */
  0x9a, /* 1101 -> 1001 1010 */
  0x6a, /* 1110 -> 0110 1010 */
  0xaa, /* 1111 -> 1010 1010 */
};

#define M_EXPAND(buffer, byte) \
  ( (buffer)[0] = manchester_lookup[(byte) & 0x0f],         /* low bits */ \
    (buffer)[1] = manchester_lookup[(uint8_t)(byte) >> 4] ) /* high bits */

uint32_t TRACESWO_TER = 0;

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
    uint8_t buffer[12], hdr;
    while (size >= 4) {
      hdr = (channel << 3) | 3;
      buffer[0] = START;
      M_EXPAND(buffer + 1, hdr);
      M_EXPAND(buffer + 3, data[0]);
      M_EXPAND(buffer + 5, data[1]);
      M_EXPAND(buffer + 7, data[2]);
      M_EXPAND(buffer + 9, data[3]);
      buffer[11] = SPACE;
      __disable_irq();
      ARM_SPI_Send(buffer, 11 + (size == 4));
      __enable_irq();
      data += 4;
      size -= 4;
    }
    if (size >= 2) {
      hdr = (channel << 3) | 2;
      buffer[0] = START;
      M_EXPAND(buffer + 1, hdr);
      M_EXPAND(buffer + 3, data[0]);
      M_EXPAND(buffer + 5, data[1]);
      buffer[7] = SPACE;
      __disable_irq();
      ARM_SPI_Send(buffer, 7 + (size == 2));
      __enable_irq();
      data += 2;
      size -= 2;
    }
    if (size >= 1) {
      hdr = (channel << 3) | 1;
      buffer[0] = START;
      M_EXPAND(buffer + 1, hdr);
      M_EXPAND(buffer + 3, data[0]);
      buffer[5] = SPACE;
      __disable_irq();
      ARM_SPI_Send(buffer, 6);
      __enable_irq();
    }
  }
}

