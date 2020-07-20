/*  tcpip - networking portability layer for Windows and Linux, limited to the
 *  functions that the GDB RSP needs
 *
 *  Copyright 2020, CompuPhase
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
#ifndef _TCPIP_H
#define _TCPIP_H

#if defined __cplusplus
  extern "C" {
#endif

#if defined WIN32 || defined _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
#else
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
#endif /* __linux__ */

int tcpip_init(void);
int tcpip_cleanup(void);

int tcpip_open(const char *ip_address);
int tcpip_close(void);
int tcpip_isopen(void);
size_t tcpip_xmit(const unsigned char *buffer, size_t size);
size_t tcpip_recv(unsigned char *buffer, size_t size);

/* general purpose functions */
unsigned long getlocalip(char *ip_address);
int connect_timeout(SOCKET sock, const char *host, short port, int timeout);

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  int closesocket(IN SOCKET s);
#endif /* __linux__ */

#if defined __cplusplus
  }
#endif

#endif /* _TCPIP_H */

