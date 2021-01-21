/*  rs232 - RS232 support, limited to the functions that the GDB RSP needs
 *
 *  Copyright 2012-2021, CompuPhase
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

#if defined __cplusplus
  extern "C" {
#endif

#if defined _WIN32
  typedef HANDLE HCOM;
#else /* _WIN32 */
  typedef int HCOM;
#endif /* _WIN32 */

enum {
  PAR_NONE = 1,
  PAR_ODD,
  PAR_EVEN,
};

HCOM*  rs232_open(const char *port, unsigned baud, int databits, int stopbits, int parity);
void   rs232_close(HCOM *hCom);
int    rs232_isopen(HCOM *hCom);
size_t rs232_xmit(HCOM *hCom, const unsigned char *buffer, size_t size);
size_t rs232_recv(HCOM *hCom, unsigned char *buffer, size_t size);
void   rs232_flush(HCOM *hCom);
void   rs232_break(HCOM *hCom);
void   rs232_dtr(HCOM *hCom, int set);
void   rs232_rts(HCOM *hCom, int set);

#if defined __cplusplus
  }
#endif

#endif /* _RS232_H */
