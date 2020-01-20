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

#ifndef __SEMIHOSTING_H
#define __SEMIHOSTING_H

#define STDOUT  1
#define STDERR  2

/** host_puts() sends a string to the debugger console via semihosting. The
 *  string is transmitted as is, no newline is appended.
 */
void host_puts(int file, const char *text);

#endif /* __SEMIHOSTING_H */

