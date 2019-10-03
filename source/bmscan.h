/*
 * Utility functions to scan for the Black Magic Probe on a system, and return
 * the (virtual) serial ports that it is assigned to. Under Microsoft Windows,
 * it scans the registry for the Black Magic Probe device, under Linux, it
 * browses through sysfs.
 *
 * Copyright 2019 CompuPhase
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _BMP_SCAN_H
#define _BMP_SCAN_H

#if defined __cplusplus
  extern "C" {
#endif

#define BMP_VID           0x1d50
#define BMP_PID_DFU       0x6017/* legacy versions, current version has DFU as an interface */
#define BMP_PID           0x6018
#define BMP_IF_GDB        0     /* interface 0 -> GDB server */
#define BMP_IF_UART       2     /* interface 2 -> 3.3V TTL UART */
#define BMP_IF_DFU        4
#define BMP_IF_TRACE      5
#define BMP_EP_TRACE      0x85  /* endpoint 5 is bulk data endpoint for trace interface */

/* find_bmp() returns 1 on success and 0 on failure; the interface must be either
   BMP_IF_GDB or BMP_IF_UART. */
#if defined WIN32 || defined _WIN32
  #include <tchar.h>
  int find_bmp(int iface, TCHAR *name, size_t namelen);
#else
  int find_bmp(int interface, char *name, size_t namelen);
#endif

#if defined __cplusplus
  }
#endif

#endif /* _BMP_SCAN_H */
