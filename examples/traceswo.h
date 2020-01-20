/* Implementation of functions to transmit data or strings over the TRACESWO
 * wire of the ARM Cortex micro-controllers.
 *
 * These routines pack the bytes to * transmit into 32-bit words, in order to
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

#ifndef __TRACESWO_H
#define __TRACESWO_H

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
