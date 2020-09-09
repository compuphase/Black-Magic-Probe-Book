/* Implementation of functions to transmit data or strings to a debug probe
 * via a SPI. It emulates the Manchester protocol of SWO. This allows tracing
 * of Cortex M0/M0+ microcontrollers, which do not have TRACESWO support.
 *
 * These routines pack the bytes to transmit into 32-bit words, in order to
 * minimize overhead (each payload item is prefixed with a 1-byte header in the
 * SWO protocol and a payload item is 1 to 4 bytes).
 *
 * Routines for initializing the SPI peripheral of the micro-controller are not
 * included, as these are dependend on the particular micro-controller.
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

#ifndef __TRACESWO_H
#define __TRACESWO_H

/** traceswo_enable() allows you to enable or disable any of the 32 channels.
 *
 *  \param channelmask  A bit mask. Set the bits for the channels that you wish
 *                      to enabe or disable. Zero bits in the mask have no
 *                      effect.
 *  \param enable       If non-zero, the channels in the mask are enabled; if
 *                      zero, the channels in the mask are disabled.
 *
 *  \return The updated channel mask.
 *
 *  \note   To read the current channel mask without changing it, call this
 *          function with "channelmask" set to zero (the value of the "enable"
 *          parameter does not matter if the mask is zero).
 */
uint32_t traceswo_enable(uint32_t channelmask, int enable);

/** traceswo_sz() transmits a zero-terminated string. This function is built
 *  upon traceswo_bin().
 *
 *  \param channel  The channel number (0..31).
 *  \param msg      A zero-terminated string.
 */
void traceswo_sz(int channel, const char *msg);

/** traceswo_bin() transmits a buffer of data (which may contain embedded
 *  zeros). The function trasmits four bytes at a time. If the size of the data
 *  buffer is not a multiple of 4, the data is padded with zeros.
 *
 *  \param channel  The channel number (0..31).
 *  \param data     The buffer to transmit. The function transmits four bytes at
 *                  a time.
 *  \param size     The size of the data buffer.
 */
void traceswo_bin(int channel, const unsigned char *data, unsigned size);

#endif /* __TRACESWO_H */
