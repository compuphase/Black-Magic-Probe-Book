/*
 * General purpose Black Magic Probe support routines, based on the GDB-RSP
 * serial interface.
 *
 * Copyright 2019-2021 CompuPhase
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
#ifndef _BMP_SUPPORT_H
#define _BMP_SUPPORT_H

#include <stdio.h>
#include "rs232.h"

#if defined __cplusplus
  extern "C" {
#endif

enum {
  BMPSTAT_NOTICE    = 0,
  BMPSTAT_SUCCESS   = 1,

  BMPERR_PORTACCESS = -1, /* cannot access/open serial port */
  BMPERR_NODETECT   = -2, /* no BMP detected */
  BMPERR_NORESPONSE = -3, /* no response on serial port */
  BMPERR_NOCONNECT  = -4, /* connection to BMP failed */
  BMPERR_MONITORCMD = -5, /* "monitor" command failed */
  BMPERR_ATTACHFAIL = -6, /* "attach" failed */
  BMPERR_MEMALLOC   = -7, /* memory allocation error */
  BMPERR_NOFLASH    = -8, /* no records of Flash memory */
  BMPERR_FLASHERASE = -9, /* Flash erase failed */
  BMPERR_FLASHWRITE = -10,/* Flash write failed */
  BMPERR_FLASHDONE  = -11,/* Flash programming completion failed */
  BMPERR_FLASHCRC   = -12,/* Flash CRC verification failed */
  BMPERR_GENERAL    = -14,
};

typedef int (*BMP_STATCALLBACK)(int code, const char *message);

void bmp_setcallback(BMP_STATCALLBACK func);
int bmp_connect(int probe, const char *ipaddress);
int bmp_disconnect(void);
int bmp_isopen(void);
HCOM *bmp_comport(void);

int bmp_checkversionstring(void);
int bmp_is_ip_address(const char *address);

int bmp_attach(int tpwr, int connect_srst, char *name, size_t namelength, char *arch, size_t archlength);
int bmp_detach(int powerdown);

int bmp_fullerase(void);
int bmp_download(FILE *fp);
int bmp_verify(FILE *fp);

int bmp_enabletrace(int async_bitrate, unsigned char *endpoint);

int bmp_restart(void);
int bmp_break(void);

int bmp_runscript(const char *name, const char *driver, const char *arch, const unsigned long *params);

#if defined __cplusplus
  }
#endif

#endif /* _BMP_SUPPORT_H */

