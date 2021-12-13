/*
 * Common functions for bmdebug, bmflash and bmtrace.
 *
 * Copyright 2021 CompuPhase
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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#if defined WIN32 || defined _WIN32
  #include <direct.h>
  #if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
    #include "strlcpy.h"
  #endif
#elif defined __linux__
  #include <bsd/string.h>
  #include <sys/stat.h>
  #include <sys/types.h>
#endif
#include "bmcommon.h"
#include "bmp-scan.h"
#include "specialfolder.h"

#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

const char **get_probelist(int *probe, int *netprobe)
{
  int usbprobes = get_bmp_count();
  assert(netprobe != NULL);
  *netprobe = (usbprobes > 0) ? usbprobes : 1;

  const char **probelist = malloc((*netprobe+1)*sizeof(char*));
  if (probelist != NULL) {
    if (usbprobes == 0) {
      probelist[0] = strdup("-");
    } else {
      char portname[64];
      int idx;
      for (idx = 0; idx < usbprobes; idx++) {
        find_bmp(idx, BMP_IF_GDB, portname, sizearray(portname));
        probelist[idx] = strdup(portname);
      }
    }
    probelist[*netprobe] = strdup("TCP/IP");
  }

  assert(probe != NULL);
  if (*probe == 99)
    *probe = *netprobe;
  else if (*probe > usbprobes)
    *probe = 0;

  return probelist;
}

void clear_probelist(const char **probelist, int netprobe)
{
  if (probelist != NULL) {
    int idx;
    for (idx = 0; idx < netprobe + 1; idx++)
      free((void*)probelist[idx]);
    free((void*)probelist);
  }
}

int get_configfile(char *filename, size_t maxsize, const char *basename)
{
  assert(filename != NULL);
  assert(maxsize > 0);
  *filename = '\0';
  if (!folder_AppConfig(filename, maxsize))
    return 0;

  strlcat(filename, DIR_SEPARATOR "BlackMagic", maxsize);
  #if defined _WIN32
    mkdir(filename);
  #else
    mkdir(filename, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  #endif
  strlcat(filename, DIR_SEPARATOR, maxsize);
  strlcat(filename, basename, maxsize);
  return 1;
}

