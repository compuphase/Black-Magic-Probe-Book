/*
 *  rs232 - RS232 support, limited to the functions that the GDB RSP needs.
 *
 *  Copyright 2012-2023, CompuPhase
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may not
 *  use this file except in compliance with the License. You may obtain a copy
 *  of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *  License for the specific language governing permissions and limitations
 *  under the License.
 */
#ifndef _RS232_H
#define _RS232_H

#include <stdbool.h>

#if defined __cplusplus
  extern "C" {
#endif

#if defined _WIN32
  typedef HANDLE HCOM;
#else /* _WIN32 */
  typedef int HCOM;
#endif /* _WIN32 */

enum {
  PAR_NONE = 0,
  PAR_ODD,
  PAR_EVEN,
  PAR_MARK,
  PAR_SPACE,
};

enum {
  FLOWCTRL_NONE,
  FLOWCTRL_RTSCTS,  /* RTS / CTS */
  FLOWCTRL_XONXOFF, /* XON / XOFF */
};

#define LINESTAT_RTS    0x0001
#define LINESTAT_DTR    0x0002
#define LINESTAT_CTS    0x0004
#define LINESTAT_DSR    0x0008
#define LINESTAT_RI     0x0010
#define LINESTAT_CD     0x0020
#define LINESTAT_ERR    0x0040
#define LINESTAT_BREAK  0x0080  /* remote host sent break */
#define LINESTAT_LBREAK 0x0100  /* local host sent break */

HCOM*    rs232_open(const char *port, unsigned baud, int databits, int stopbits, int parity, int flowctrl);
HCOM*    rs232_close(HCOM *hCom);
bool     rs232_isopen(const HCOM *hCom);
size_t   rs232_xmit(HCOM *hCom, const unsigned char *buffer, size_t size);
size_t   rs232_recv(HCOM *hCom, unsigned char *buffer, size_t size);
void     rs232_flush(HCOM *hCom);
size_t   rs232_peek(HCOM *hCom);
void     rs232_setstatus(HCOM *hCom, int code, int status);
unsigned rs232_getstatus(HCOM *hCom);
void     rs232_framecheck(HCOM *hCom, int enable);
int      rs232_collect(char **portlist, int listsize);

#if defined __cplusplus
  }
#endif

#endif /* _RS232_H */
