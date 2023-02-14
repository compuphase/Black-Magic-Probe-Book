/*
 * General purpose Black Magic Probe support routines, based on the GDB-RSP
 * serial interface.
 *
 * Copyright 2019-2023 CompuPhase
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

#include <stdbool.h>
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

unsigned long bmp_flashtotal(void);

typedef int (*BMP_STATCALLBACK)(int code, const char *message);
void bmp_setcallback(BMP_STATCALLBACK func);

bool bmp_connect(int probe, const char *ipaddress);
bool bmp_disconnect(void);
bool bmp_isopen(void);
void bmp_sethandle(HCOM *hcom);
HCOM *bmp_comport(void);

int bmp_is_ip_address(const char *address);
int bmp_checkversionstring(void);
const char *bmp_get_monitor_cmds(void);
bool bmp_expand_monitor_cmd(char *buffer, size_t bufsize, const char *name, const char *list);

bool bmp_attach(bool autopower, char *name, size_t namelength, char *arch, size_t archlength);
bool bmp_detach(bool powerdown);

int bmp_monitor(const char *command);
int bmp_fullerase(void);
bool bmp_download(FILE *fp);
bool bmp_verify(FILE *fp);

void bmp_progress_reset(unsigned long numsteps);
void bmp_progress_step(unsigned long step);
void bmp_progress_get(unsigned long *step, unsigned long *range);

int bmp_enabletrace(int async_bitrate, unsigned char *endpoint);

int bmp_restart(void);
int bmp_break(void);

bool bmp_runscript(const char *name, const char *driver, const char *arch, unsigned long *params, size_t paramcount);

#if defined __cplusplus
  }
#endif

#endif /* _BMP_SUPPORT_H */

