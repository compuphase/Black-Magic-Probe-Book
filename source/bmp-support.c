/*
 * General purpose Black Magic Probe support routines, based on the GDB-RSP
 * serial interface. The "script" support can also be used with GDB.
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
#if defined _WIN32
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
# if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
#   include "strlcpy.h"
# endif
#else
# include <unistd.h>
# include <bsd/string.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "bmp-scan.h"
#include "bmp-script.h"
#include "bmp-support.h"
#include "crc32.h"
#include "elf.h"
#include "fileloader.h"
#include "gdb-rsp.h"
#include "tcpip.h"
#include "xmltractor.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif


#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
# define stricmp(s1,s2)  strcasecmp((s1),(s2))
#elif defined _MSC_VER
# define strdup(s)       _strdup(s)
#endif

#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif


typedef struct tagMEMBLOCK {
  struct tagMEMBLOCK *next;
  unsigned long address;
  unsigned long size;     /* total size of the region */
  unsigned int blocksize; /* Flash sector size */
} MEMBLOCK;

static HCOM *hCom = NULL;
static int CurrentProbe = -1;
static int PacketSize = 0;
static MEMBLOCK FlashRegions = { NULL };

static BMP_STATCALLBACK stat_callback = NULL;


static int notice(int code, const char *fmt, ...)
{
  if (stat_callback != NULL) {
    char message[200];
    va_list args;
    va_start(args, fmt);
    vsprintf(message, fmt, args);
    va_end(args);
    return stat_callback(code, message);
  }
  return 0;
}

static void memblock_cleanup(MEMBLOCK *root)
{
  assert(root != NULL);
  while (root->next != NULL) {
    MEMBLOCK *cur = root->next;
    root->next = cur->next;
    free((void*)cur);
  }
}

void bmp_flash_cleanup(void)
{
  memblock_cleanup(&FlashRegions);
}

/** bmp_flashtotal() returns the Flash memory total range and the number of
 *  regions.
 *
 *  \param low_addr   [out] Set to the lowest Flash address. This parameter may
 *                    be NULL.
 *  \param high_addr  [out] Set to the address just beyond the highest Flash
 *                    address. This parameter may be NULL.
 *
 *  \return The number of flash regions, as reported by the BMP (a region may be
 *          composed of several Flash sectors).
 */
int bmp_flashtotal(unsigned long *low_addr, unsigned long *high_addr)
{
  unsigned long low = ULONG_MAX;
  unsigned long high = 0;
  int count = 0;
  for (const MEMBLOCK *rgn = FlashRegions.next; rgn != NULL; rgn = rgn->next) {
    if (rgn->address < low)
      low = rgn->address;
    if (rgn->address + rgn->size > high)
      high = rgn->address + rgn->size;
    count += 1;
  }
  if (count == 0)
    low = high = 0;
  if (low_addr != NULL)
    *low_addr = low;
  if (high_addr != NULL)
    *high_addr = high;
  return count;
}

/** bmp_setcallback() sets the callback function for detailed status
 *  messages. The callback receives status codes as well as a text message.
 *  All error codes are negative.
 */
void bmp_setcallback(BMP_STATCALLBACK func)
{
  stat_callback = func;
}

static bool testreply(const char *reply, size_t reply_length, const char *match)
{
  assert(reply != NULL);
  assert(match != NULL);

  if (reply_length == 0)
    return false;
  size_t match_length = strlen(match);
  /* Black Magic Probe can append a '\0' behind the reply (this is a bug in
     the firmware, but one that we have to deal with) */
  if (reply_length == match_length + 1 && reply[reply_length - 1] == '\0')
    reply_length -= 1;

  return (reply_length == match_length && memcmp(reply, match, reply_length) == 0);
}

/** bmp_connect() scans for the USB port of the Black Magic Probe and connects
 *  to it. It can also connect to a gdbserver via TCP/IP; in this case, the IP
 *  address must be passed, and the scanning phase is skipped.
 *
 *  This function retrieves the essential "packet size" parameter, but does not
 *  issue any other command.
 *
 *  \param probe      The probe sequence number, 0 if only a single probe is
 *                    connected. This parameter is ignored if ipaddress is not
 *                    NULL.
 *  \param ipaddress  NULL to connect to an USB probe, or a valid IP address to
 *                    connect to a gdbserver over TCP/IP.
 *
 *  \return true on success, false on failure. Status and error messages are
 *          passed via the callback.
 */
bool bmp_connect(int probe, const char *ipaddress)
{
  char devname[128], probename[64];
  bool initialize = false;

  /* if switching between probes, reconnect (so close the current connection) */
  if ((probe != CurrentProbe && ipaddress == NULL) || (CurrentProbe >= 0 && ipaddress != NULL)) {
    bmp_disconnect();
    CurrentProbe = (ipaddress == NULL) ? probe : -1;
  }

  if (CurrentProbe >= 0) {
    strlcpy(probename, "Black Magic Probe", sizearray(probename));
  } else {
    strlcpy(probename, "ctxLink", sizearray(probename));
    strlcpy(devname, ipaddress, sizearray(devname));
  }

  if (CurrentProbe >= 0 && !rs232_isopen(hCom)) {
    /* serial port is selected, and it is currently not open */
    bmp_flash_cleanup();
    if (find_bmp(probe, BMP_IF_GDB, devname, sizearray(devname))) {
      char buffer[512];
      size_t size;
      /* connect to the port */
      hCom = rs232_open(devname, 115200, 8, 1, PAR_NONE, FLOWCTRL_NONE);
      if (!rs232_isopen(hCom)) {
        notice(BMPERR_PORTACCESS, "Failure opening port %s", devname);
        return false;
      }
      rs232_setstatus(hCom, LINESTAT_RTS, 1);
      rs232_setstatus(hCom, LINESTAT_DTR, 1); /* required by GDB RSP */
      /* check for reception of the handshake */
      size = gdbrsp_recv(buffer, sizearray(buffer), 250);
      if (size == 0) {
        /* toggle DTR, to be sure */
        rs232_setstatus(hCom, LINESTAT_RTS, 0);
        rs232_setstatus(hCom, LINESTAT_DTR, 0);
#       if defined _WIN32
          Sleep(200);
#       else
          usleep(200 * 1000);
#       endif
        rs232_setstatus(hCom, LINESTAT_RTS, 0);
        rs232_setstatus(hCom, LINESTAT_DTR, 1);
        size = gdbrsp_recv(buffer, sizearray(buffer), 250);
      }
      if (!testreply(buffer, size, "OK")) {
        /* send "monitor version" command to check for a response (ignore the
           text of the response, only check for the "OK" end code) */
        rs232_flush(hCom);
        gdbrsp_xmit("qRcmd,version", -1);
        do {
          size = gdbrsp_recv(buffer, sizearray(buffer)-1, 250);
        } while (size > 0 && size < 2);
        if (!testreply(buffer, size, "OK")) {
          notice(BMPERR_NORESPONSE, "No response on %s", devname);
          hCom = rs232_close(hCom);
          return false;
        }
      }
      initialize = true;
    }
  }

  if (CurrentProbe < 0 && ipaddress != NULL && !tcpip_isopen()) {
    /* network interface is selected, and it is currently not open */
    tcpip_open(ipaddress);
    if (!tcpip_isopen()) {
      notice(BMPERR_PORTACCESS, "Failure opening gdbserver at %s", devname);
      return false;
    }
    initialize = true;
  }

  /* check whether opening the communication interface succeeded */
  if ((CurrentProbe >= 0 && !rs232_isopen(hCom)) || (CurrentProbe < 0 && !tcpip_isopen())) {
    /* initialization failed */
    notice(BMPERR_NODETECT, "%s not detected", probename);
    return false;
  }

  if (initialize) {
    char buffer[256];
    /* clear stray data that is still in the queue */
    while (gdbrsp_recv(buffer, sizearray(buffer), 10) > 0)
      {}
    /* query parameters */
    gdbrsp_xmit("qSupported:multiprocess+", -1);
    size_t size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    buffer[size] = '\0';
    char *ptr;
    if ((ptr = strstr(buffer, "PacketSize=")) != NULL)
      PacketSize = (int)strtol(ptr + 11, NULL, 16);
    gdbrsp_packetsize(PacketSize+16); /* allow for some margin */
    //??? check for "qXfer:memory-map:read+" as well
    /* connect to gdbserver */
    int retry;
    for (retry = 3; retry > 0; retry--) {
      gdbrsp_xmit("!",-1);
      size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
      if (testreply(buffer, size, "OK"))
        break;
#     if defined _WIN32
        Sleep(200);
#     else
        usleep(200 * 1000);
#     endif
    }
    if (retry == 0) {
      notice(BMPERR_NOCONNECT, "Connect failed on %s", devname);
      bmp_disconnect();
      return false;
    }
    notice(BMPSTAT_NOTICE, "Connected to %s (%s)", probename, devname);
  }

  return true;
}

/** bmp_disconnect() closes the connection to the Black Magic Probe, it one was
 *  active.
 *
 *  \return true on success, false if no connection was open.
 */
bool bmp_disconnect(void)
{
  bool result = false;

  if (rs232_isopen(hCom)) {
    rs232_setstatus(hCom, LINESTAT_RTS, 0);
    rs232_setstatus(hCom, LINESTAT_DTR, 0);
    hCom = rs232_close(hCom);
    result = true;
  }
  if (tcpip_isopen()) {
    tcpip_close();
    result = true;
  }
  return result;
}

/** bmp_sethandle() sets a COM handle to use for the communication with the
 *  Black Magic Probe (for those applications that open the RS232 port by other
 *  means than bmp_connect()).
 */
void bmp_sethandle(HCOM *hcom)
{
  hCom = hcom;
}

/** bmp_comport() returns the COM port handle for gdbserver. It returns NULL if
 *  the connection is over TCP/IP, or if no connection is open.
 */
HCOM *bmp_comport(void)
{
  return rs232_isopen(hCom) ? hCom : NULL;
}

/** bmp_isopen() returns whether a connection to a Black Magic Probe or a
 *  ctxLink is open, via USB (virtual COM port) or TCP/IP.
 */
bool bmp_isopen(void)
{
  return rs232_isopen(hCom) || tcpip_isopen();
}

/** bmp_is_ip_address() returns 1 if the input string appears to contain a
 *  valid IP address, or 0 if the format is incorrect.
 */
int bmp_is_ip_address(const char *address)
{
  int a, b, c, d;
  return sscanf(address, "%d.%d.%d.%d", &a, &b, &c, &d) == 4
         && a > 0 && a < 255 && b >= 0 && b < 255 && c >= 0 && c < 255 && d >= 0 && d < 255;
}

/** bmp_break() interrupts a running target by sending a Ctrl-C byte. */
int bmp_break(void)
{
  gdbrsp_xmit("\3", 1);
  return 1; /* there is no reply on Ctrl-C */
}

/** bmp_attach() attaches to the target that is connected to the Black Magic
 *  Probe (the Black Magic Probe must have been connected first). It
 *  optionally switches power on the voltage-sense pin (to power the target).
 *  The name of the driver for the MCU (that the Black Magic Probe uses) is
 *  returned.
 *
 *  \param autopower    If set, and if the swdp_scan command returns 0V power,
 *                      the "tpwr" command is given, before the swdp_scan
 *                      command is retried.
 *  \param name         Will be set to the name of the driver for the MCU (the
 *                      MCU series name) on output. This parameter may be NULL.
 *  \param namelength   The maximum length of the name, including the \0 byte.
 *  \param arch         Will be set to the architecture of the MCU on output.
 *                      This is typically M0, M3, M3/M4, or similar. This
 *                      parameter may be NULL. Note that Black Magic Probe
 *                      firmware 1.6 does not return an architecture name.
 *  \param archlength   The maximum length of the architecture name, including
 *                      the \0 byte.
 *
 *  \return true on success, false on failure. Status and error messages are
 *          passed via the callback.
 */
bool bmp_attach(bool autopower, char *name, size_t namelength, char *arch, size_t archlength)
{
  char buffer[512];
  size_t size;
  int ok;

  if (name != NULL && namelength > 0)
    *name = '\0';
  if (arch != NULL && archlength > 0)
    *arch = '\0';

  if (!bmp_isopen()) {
    notice(BMPERR_ATTACHFAIL, "No connection to debug probe");
    return false;
  }

restart:
  gdbrsp_xmit("qRcmd,swdp_scan", -1); /* causes a detach, if MCU was attached */
  for ( ;; ) {
    size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    if (size > 2 && buffer[0] == 'o') {
      const char *ptr;
      buffer[size] = '\0';
      /* parse the string */
      if (autopower && strchr(buffer, '\n') != NULL && (ptr = strstr(buffer + 1, "voltage:")) != NULL) {
        double voltage = strtod(ptr + 8, (char**)&ptr);
        if (*ptr == 'V' && voltage < 0.1) {
          notice(BMPSTAT_NOTICE, "Note: powering target");
          if (bmp_monitor("tpwr enable")) {
            /* give the micro-controller a bit of time to start up, before issuing
               the swdp_scan command */
#           if defined _WIN32
              Sleep(100);
#           else
              usleep(100 * 1000);
#           endif
          } else {
            notice(BMPERR_MONITORCMD, "Power to target failed");
          }
          autopower = false;  /* do not drop in this case again */
          goto restart;
        }
      }
      if (name != NULL && strchr(buffer, '\n') != NULL && strtol(buffer + 1, (char**)&ptr, 10) == 1) {
        char namebuffer[100];
        while (*ptr <= ' ' && *ptr != '\0')
          ptr++;
        strlcpy(namebuffer, ptr, sizearray(namebuffer));
        if ((ptr = strchr(namebuffer, '\n')) != NULL)
          *(char*)ptr = '\0';
        /* possibly split the name into a family and an architecture */
        if ((ptr = strrchr(namebuffer, ' ')) != NULL && ptr[1] == 'M' && isdigit(ptr[2])) {
          *(char*)ptr = '\0';
          if (arch != NULL && archlength > 0)
            strlcpy(arch, ptr + 1, archlength);
          while (ptr > namebuffer && *(ptr - 1) == ' ')
            *(char*)--ptr = '\0'; /* strip trailing whitespace */
        }
        strlcpy(name, namebuffer, namelength);
      }
      notice(BMPSTAT_NOTICE, buffer + 1);  /* skip the 'o' at the start */
    } else if (!testreply(buffer, size, "OK")) {
      /* error message was already given by an "output"-response */
      return false;
    } else {
      break;  /* OK was received */
    }
  }
  gdbrsp_xmit("vAttach;1", -1);
  size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
  /* accept OK, S##, T## (but in fact, Black Magic Probe always sends T05) */
  ok = (testreply(buffer, size, "OK"))
       || (size == 3 && buffer[0] == 'S' && isxdigit(buffer[1]) && isxdigit(buffer[2]))
       || (size >= 3 && buffer[0] == 'T' && isxdigit(buffer[1]) && isxdigit(buffer[2]));
  if (!ok) {
    notice(BMPERR_ATTACHFAIL, "Attach failed");
    return false;
  }
  notice(BMPSTAT_NOTICE, "Attached to target 1");

  /* check memory map and features of the target */
  bmp_flash_cleanup();
  sprintf(buffer, "qXfer:memory-map:read::0,%x", PacketSize - 4);
  gdbrsp_xmit(buffer, -1);
  size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
  if (size > 10 && buffer[0] == 'm') {
    xt_Node* root = xt_parse(buffer + 1);
    if (root != NULL) {
      xt_Node* node = xt_find_child(root, "memory");
      while (node != NULL) {
        xt_Attrib* attrib = xt_find_attrib(node, "type");
        if (attrib != NULL && attrib->szvalue == 5 && strncmp(attrib->value, "flash", attrib->szvalue) == 0) {
          MEMBLOCK *rgn = malloc(sizeof(MEMBLOCK));
          if (rgn != NULL) {
            memset(rgn, 0, sizeof(MEMBLOCK));
            if ((attrib = xt_find_attrib(node, "start")) != NULL)
              rgn->address = strtoul(attrib->value, NULL, 0);
            if ((attrib = xt_find_attrib(node, "length")) != NULL)
              rgn->size = strtoul(attrib->value, NULL, 0);
            xt_Node* prop;
            if ((prop = xt_find_child(node, "property")) != NULL
                && (attrib = xt_find_attrib(prop, "name")) != NULL
                && attrib->szvalue == 9 && strncmp(attrib->value, "blocksize", attrib->szvalue) == 0)
              rgn->blocksize = strtoul(prop->content, NULL, 0);
            /* append to list, sorted on address */
            MEMBLOCK *pos = &FlashRegions;
            while (pos->next != NULL && pos->next->address < rgn->address)
              pos = pos->next;
            rgn->next = pos->next;
            pos->next = rgn;
          } else {
            notice(BMPERR_MEMALLOC, "Memory allocation error while parsing MCU memory map");
          }
        }
        node = xt_find_sibling(node, "memory");
      }
      xt_destroy_node(root);
    }
  }
  if (bmp_flashtotal(NULL, NULL) == 0)
    notice(BMPERR_NOFLASH, "No Flash memory record");

  return true;
}

bool bmp_detach(bool powerdown)
{
  bool result = false;

  if (bmp_isopen()) {
    char buffer[100];
    size_t size;
    result = true;
    /* detach */
    gdbrsp_xmit("D", -1);
    size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    if (!testreply(buffer, size, "OK"))
      result = false;
    /* optionally disable power */
    if (powerdown) {
      if (!bmp_monitor("tpwr disable"))
        result = false;
    }
    /* clean up flash information data */
    memblock_cleanup(&FlashRegions);
  }

  return result;
}

/** bmp_checkversionstring() issues the "monitor version" command to the
 *  debug probe and scans the result for known values for the native BMP and
 *  for ctxLink.
 */
int bmp_checkversionstring(void)
{
  if (!bmp_isopen())
    return PROBE_UNKNOWN;

  char line[512];
  memset(line, 0, sizeof line);

  int probe = PROBE_UNKNOWN;
  gdbrsp_xmit("qRcmd,version", -1);
  while (probe == PROBE_UNKNOWN) {
    char buffer[512];
    size_t size = gdbrsp_recv(buffer, sizearray(buffer) - 1, 1000);
    if (size > 0) {
      assert(size < sizearray(buffer));
      buffer[size] = '\0';
      char *ptr;
      if (buffer[0] == 'o') {
        if (line[0] == 'o')
          strlcat(line, buffer + 1, sizearray(line));
        else
          strlcpy(line, buffer, sizearray(line));
        if (strchr(line, '\n') != NULL) {
          int p = check_versionstring(line + 1);
          if (p != PROBE_UNKNOWN)
            probe = p;
          memset(line, 0, sizeof line);
        }
      } else if ((ptr = strchr(buffer, 'o')) != NULL) {
        strlcpy(line, ptr, sizearray(line));
      } else if (testreply(buffer, size, "OK")) {
        /* end response found (when arriving here, the version string has
           probably not been recognized) */
        break;
      }
    } else {
      /* no new data arrived within the time-out, assume failure */
      return PROBE_UNKNOWN;
    }
  }
  return probe;
}

/** bmp_get_partid() issues the "monitor partid" command to the debug probe,
 *  for the LPC family and other microcontrollers that may provide the command.
 *  \return The part-ID, or 0 on failure.
 */
uint32_t bmp_get_partid(void)
{
  if (!bmp_isopen())
    return PROBE_UNKNOWN;

  char line[512];
  memset(line, 0, sizeof line);

  uint32_t partid = 0;
  gdbrsp_xmit("qRcmd,partid", -1);
  while (partid == 0) {
    char buffer[512];
    size_t size = gdbrsp_recv(buffer, sizearray(buffer) - 1, 1000);
    if (size > 0) {
      assert(size < sizearray(buffer));
      buffer[size] = '\0';
      char *ptr;
      if (buffer[0] == 'o') {
        if (line[0] == 'o')
          strlcat(line, buffer + 1, sizearray(line));
        else
          strlcpy(line, buffer, sizearray(line));
        if (strchr(line, '\n') != NULL) {
          if (strncmp(line + 1, "Part ID", 7) == 0) {
            const char *ptr = line + 1 + 7;
            if (*ptr == ':')
              ptr++;
            partid = strtoul(ptr, NULL, 0);
          }
          memset(line, 0, sizeof line);
        }
      } else if ((ptr = strchr(buffer, 'o')) != NULL) {
        strlcpy(line, ptr, sizearray(line));
      } else if (testreply(buffer, size, "OK")) {
        /* end response found (when arriving here, the version string has
           probably not been recognized) */
        break;
      }
    } else {
      /* no new data arrived within the time-out, assume failure */
      return 0;
    }
  }
  return partid;
}

/** bmp_get_monitor_cmds() collects the list of "monitor" commands. These are
 *  probe-dependent and target-dependent (plus probe firmware version
 *  dependent).
 *
 *  When this function is called after connecting to the probe (but before
 *  attaching to the target), it returns only the probe-dependent commands.
 *
 *  \return A pointer to a dynamically allocated string, which contains the
 *          commands separated by a space. The returned list must be freed with
 *          free().
 */
const char *bmp_get_monitor_cmds(void)
{
  if (!bmp_isopen())
    return NULL;

  int count = 0;
  int listsize = 4;
  char **list = malloc(listsize * sizeof(char*));
  if (list == NULL)
    return NULL;
  memset(list, 0, listsize * sizeof(char*));

  char line[512];
  memset(line, 0, sizeof line);

  gdbrsp_xmit("qRcmd,help",-1);
  for (;;) {
    char buffer[512];
    size_t size = gdbrsp_recv(buffer, sizearray(buffer) - 1, 1000);
    if (size > 0) {
      assert(size < sizearray(buffer));
      buffer[size] = '\0';
      char *ptr;
      if (buffer[0] == 'o') {
        if (line[0] == 'o')
          strlcat(line, buffer + 1, sizearray(line));
        else
          strlcpy(line, buffer, sizearray(line));
        if (strchr(line, '\n') != NULL) {
          /* get only the command (strip summary) */
          char *ptr = strstr(line, "--");
          if (ptr != NULL) {
            while (ptr > line && *(ptr - 1) <= ' ')
              ptr -= 1;
            *ptr = '\0';
            /* check whether to grow the list */
            if (count + 1 >= listsize) {
              int newsize = 2 * listsize;
              char **newlist = malloc(newsize * sizeof(char*));
              if (newlist != NULL) {
                memset(newlist, 0, newsize * sizeof(char*));
                memcpy(newlist, list, count * sizeof(char*));
                free((void*)list);
                list = newlist;
                listsize = newsize;
              }
            }
            if (count + 1 < listsize) {
              ptr = line + 1; /* skip 'o' that starts the line of the reply */
              while (*ptr != '\0' && *ptr <= ' ')
                ptr++;        /* skip whitespace too */
              list[count]=strdup(ptr);
              count++;
            }
          }
          memset(line, 0, sizeof line);
        }
      } else if ((ptr = strchr(buffer, 'o')) != NULL) {
        strlcpy(line, ptr, sizearray(line));
      } else if (testreply(buffer, size, "OK")) {
        /* end response found -> done */
        break;
      }
    } else {
      /* no new data arrived within the time-out, assume failure */
      break;
    }
  }

  /* sort the retrieved list (insertion sort) */
  for (int i = 1; i < count; i++) {
    char *key = list[i];
    int j;
    for (j = i; j > 0 && strcmp(list[j - 1], key) > 0; j--)
      list[j] = list[j - 1];
    list[j] = key;
  }

  /* build a string from the list */
  size_t total_length = 0;
  for (int idx = 0; idx < count; idx++) {
    assert(list[idx] != NULL);
    total_length += strlen(list[idx]) + 1;  /* +1 for space between words */
  }
  char *buffer = malloc((total_length + 1) * sizeof(char));
  if (buffer != NULL) {
    *buffer = '\0';
    for (int idx = 0; idx < count; idx++) {
      assert(list[idx] != NULL);
      strcat(buffer, list[idx]);
      if (idx + 1 < count)
        strcat(buffer, " ");
    }
  }

  /* clean up */
  for (int idx = 0; idx < count; idx++) {
    assert(list[idx] != NULL);
    free((void*)list[idx]);
  }
  free(list);

  return (const char*)buffer;
}

/** bmp_has_command() checks whether the given command appears in the list.
 *
 *  \param name   [in] The command name to match. This parameter must be valid.
 *  \param list   [in] The list of commands; this may be NULL if no commands
 *                have been queried.
 *  \return true on success, false on failure.
 */
bool bmp_has_command(const char *name, const char *list)
{
  if (list == NULL)
    return false;

  assert(name != NULL);
  size_t name_len = strlen(name);
  const char *head = list;
  while (*head != '\0') {
    /* the assumption is that the list of commands is "well-formed": no leading
       or trailing spaces and the tokens separated by a single space */
    assert(*head > ' ');
    const char *tail = strchr(head, ' ');
    if (tail == NULL)
      tail = head + strlen(head);
    size_t token_len = tail - head;
    if (token_len == name_len && strncmp(name, head, name_len) == 0)
      return true;
    head = tail;
    while (*head != '\0' && *head <= ' ')
      head++;
  }
  return false;
}

/** bmp_expand_monitor_cmd() finds the complete command from a prefix.
 *
 *  \param buffer   [out] Will contain the complete command. This parameter may
 *                  be set to NULL (in which case the complete command is not
 *                  stored).
 *  \param bufsize  The size of the output buffer.
 *  \param name     [in] The prefix to complete.
 *  \param list     [in] A string with all commands (separated by spaces).
 *
 *  \return true on success, false if the prefix does not match any command.
 *
 *  \note This function can also be used to check whether a command is supported
 *        on the target (parameters "buffer" and "bufsize" can be set to NULL
 *        and zero respectively).
 */
bool bmp_expand_monitor_cmd(char *buffer, size_t bufsize, const char *name, const char *list)
{
  assert(name != NULL);
  assert(list != NULL);
  size_t name_len = strlen(name);

  buffer[0] = '\0';
  const char *head = list;
  while (*head != '\0') {
    /* the assumption is that the list of commands is "well-formed": no leading
       or trailing spaces and the tokens separated by a single space */
    assert(*head > ' ');
    const char *tail = strchr(head, ' ');
    if (tail == NULL)
      tail = head + strlen(head);
    size_t token_len = tail - head;
    if (token_len >= name_len && strncmp(name, head, name_len) == 0) {
      /* match, copy the token */
      if (buffer != NULL && bufsize > 0) {
        if (bufsize <= token_len)
          token_len = bufsize - 1;
        strncpy(buffer, head, token_len);
        buffer[token_len] = '\0';
      }
      return true;
    }
    head = tail;
    while (*head != '\0' && *head <= ' ')
      head++;
  }

  return false;
}

/** bmp_monitor() executes a "monitor" command and returns whether the reply
 *  indicates success. This is suitable for simple monitor commands, that do
 *  not require analysis of the reply strings sent by the device (other than
 *  OK or error)
 */
bool bmp_monitor(const char *command)
{
  char buffer[512];
  size_t size;

  assert(command != NULL && strlen(command) > 0);

  if (!bmp_isopen()) {
    notice(BMPERR_ATTACHFAIL, "No connection to debug probe");
    return false;
  }

  strlcpy(buffer, "qRcmd,", sizearray(buffer));
  strlcat(buffer, command, sizearray(buffer));
  gdbrsp_xmit(buffer, -1);
  do {
    size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
  } while (size > 0 && buffer[0] == 'o'); /* ignore console output */
  return testreply(buffer, size, "OK");
}

static unsigned long download_numsteps = 0;
static unsigned long download_step = 0;
void bmp_progress_reset(unsigned long numsteps)
{
  download_step = 0;
  download_numsteps = numsteps;
}
void bmp_progress_step(unsigned long step)
{
  download_step += step;
  if (download_step > download_numsteps)
    download_step = download_numsteps;
}
void bmp_progress_get(unsigned long *step, unsigned long *range)
{
  if (step != NULL)
    *step = download_step;
  if (range != NULL)
    *range = download_numsteps;
}

bool bmp_download(void)
{
  bmp_progress_reset(0);
  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return false;
  }
  if (bmp_flashtotal(NULL, NULL) == 0) {
    notice(BMPERR_NOFLASH, "No Flash memory record");
    return false;
  }
  if (filesection_filetype() == FILETYPE_NONE) {
    notice(BMPERR_NOFILEDATA, "No target file loaded");
    return false;
  }
  int pktsize = (PacketSize > 0) ? PacketSize : 64;
  char *cmd = malloc((pktsize + 16) * sizeof(char));
  if (cmd == NULL) {
    notice(BMPERR_MEMALLOC, "Memory allocation error");
    return false;
  }

  unsigned long progress_range = 0;
  for (const MEMBLOCK *rgn = FlashRegions.next; rgn != NULL; rgn = rgn->next) {
    unsigned long saddr, ssize;
    /* walk through all sections in the target file that fall into this Flash region */
    unsigned long topaddr = 0;
    for (int segment = 0; filesection_getdata(segment, &saddr, NULL, &ssize, NULL); segment++) {
      if (saddr >= rgn->address && saddr < rgn->address + rgn->size) {
        topaddr = saddr + ssize;
        progress_range += ssize;
      }
    }
    if (topaddr == 0)
      continue; /* no segment fitting in this Flash sector -> continue with next region */
    bmp_progress_reset(progress_range+1);
    /* erase the Flash memory */
    assert(topaddr <= rgn->address + rgn->size);
    assert(rgn->blocksize > 0);
    unsigned long flashsectors = ((topaddr - rgn->address + (rgn->blocksize - 1)) / rgn->blocksize);
    assert(flashsectors * rgn->blocksize <= rgn->address + rgn->size);
    notice(BMPSTAT_NOTICE, "Erase Flash at 0x%x length 0x%x",
           (unsigned)rgn->address, (unsigned)(flashsectors * rgn->blocksize));
    sprintf(cmd, "vFlashErase:%x,%x", (unsigned)rgn->address, (unsigned)(flashsectors * rgn->blocksize));
    gdbrsp_xmit(cmd, -1);
    size_t rcvd = gdbrsp_recv(cmd, pktsize, 500);
    if (!testreply(cmd, rcvd, "OK")) {
      notice(BMPERR_FLASHERASE, "Flash erase failed");
      free(cmd);
      return false;
    }
    bmp_progress_step(1);
    /* walk through all segments again, to download the payload */
    int stype;
    unsigned char *sdata;
    for (int segment = 0; filesection_getdata(segment, &saddr, &sdata, &ssize, &stype); segment++) {
      if (ssize == 0 || saddr < rgn->address || saddr >= rgn->address + rgn->size)
        continue;
      const char *desc = "Download to";
      if (stype == SECTIONTYPE_CODE)
        desc = "Code section at";
      else if (stype == SECTIONTYPE_DATA)
        desc = "Data section at";
      notice(BMPSTAT_NOTICE, "%d: %s 0x%lx length 0x%lx", segment, desc, saddr, ssize);
      unsigned pos, numbytes;
      for (pos = numbytes = 0; pos < ssize; pos += numbytes) {
        unsigned prefixlen;
        sprintf(cmd, "vFlashWrite:%x:", (unsigned)(saddr + pos));
        prefixlen = strlen(cmd) + 4;  /* +1 for '$', +3 for '#nn' checksum */
        /* make blocks that are a multiple of 16 bytes (for guaranteed alignment)
           that are less than (or equal to) PacketSize; start by subtracting the
           prefix length */
        numbytes = (pktsize - prefixlen) & ~0x0f;
        if (pos + numbytes > ssize)
          numbytes = ssize - pos;
        /* check how many bytes in the packet must be escaped, then check
           whether the packet would still fit (decrement the block length
           otherwise) */
        for ( ;; ) {
          unsigned esccount = 0;
          for (unsigned idx = 0; idx < numbytes; idx++)
            if (sdata[pos + idx] == '$' || sdata[pos + idx] == '#' || sdata[pos + idx] == '}')
              esccount += 1;
          if (numbytes + esccount + prefixlen <= (unsigned)pktsize)
            break;
          numbytes -= 16;
        }
        memmove(cmd + (prefixlen - 4), sdata + pos, numbytes);
        gdbrsp_xmit(cmd, (prefixlen - 4) + numbytes);
        rcvd = gdbrsp_recv(cmd, pktsize, 500);
        if (!testreply(cmd, rcvd, "OK")) {
          notice(BMPERR_FLASHWRITE, "Flash write failed");
          free(cmd);
          return false;
        }
        bmp_progress_step(numbytes);
      }
    }
    gdbrsp_xmit("vFlashDone", -1);
    rcvd = gdbrsp_recv(cmd, pktsize, 500);
    if (!testreply(cmd, rcvd, "OK")) {
      notice(BMPERR_FLASHDONE, "Flash completion failed");
      free(cmd);
      return false;
    }
  }

  free(cmd);
  return true;
}

bool bmp_verify(void)
{
  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return false;
  }
  if (bmp_flashtotal(NULL, NULL) == 0) {
    notice(BMPERR_NOFLASH, "No Flash memory record");
    return false;
  }

  /* run over all segments in the ELF file */
  bool allmatch = true;
  unsigned long saddr, ssize;
  unsigned char *sdata;
  for (int segment = 0; filesection_getdata(segment, &saddr, &sdata, &ssize, NULL); segment++) {
    if (ssize == 0)
      continue;   /* no loadable data */
    /* also check that saddr falls within a Flash memory sector */
    const MEMBLOCK *rgn;
    for (rgn = FlashRegions.next; rgn != NULL; rgn = rgn->next)
      if (saddr >= rgn->address && saddr < rgn->address + rgn->size)
        break;
    if (rgn == NULL)
      continue; /* segment is outside of any Flash sector */
    /* calculate CRC of the section in memory */
    unsigned crc_src = (unsigned)gdb_crc32((uint32_t)~0, sdata, ssize);
    /* request CRC from Black Magic Probe */
    char cmd[100];
    sprintf(cmd, "qCRC:%lx,%lx", saddr, ssize);
    gdbrsp_xmit(cmd, -1);
    size_t rcvd = gdbrsp_recv(cmd, sizearray(cmd), 3000);
    cmd[rcvd] = '\0';
    unsigned crc_tgt = (rcvd >= 2 && cmd[0] == 'C') ? strtoul(cmd + 1, NULL, 16) : 0;
    if (crc_tgt != crc_src) {
      notice(BMPERR_FLASHCRC, "Segment %d data mismatch", segment);
      allmatch = false;
    }
  }
  if (allmatch)
    notice(BMPSTAT_SUCCESS, "Verification successful");

  return allmatch;
}

bool bmp_fullerase(unsigned flashsize)
{
  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return false;
  }
  if (bmp_flashtotal(NULL, NULL) == 0) {
    notice(BMPERR_NOFLASH, "No Flash memory record");
    return false;
  }
  int pktsize = (PacketSize > 0) ? PacketSize : 64;
  char *cmd = malloc((pktsize + 16) * sizeof(char));
  if (cmd == NULL) {
    notice(BMPERR_MEMALLOC, "Memory allocation error");
    return false;
  }

  for (const MEMBLOCK *rgn = FlashRegions.next; rgn != NULL; rgn = rgn->next) {
    unsigned size = (unsigned)rgn->size;
    if (size > flashsize)
      size = flashsize;
    int failed;
    do {
      sprintf(cmd, "vFlashErase:%x,%x", (unsigned)rgn->address, size);
      gdbrsp_xmit(cmd, -1);
      int rcvd = gdbrsp_recv(cmd, pktsize, 5000); /* erase may take some time */
      failed = !testreply(cmd, rcvd, "OK");
      if (failed)
        size /= 2;
    } while (failed && size >= 1024);
    if (failed) {
      notice(BMPERR_FLASHERASE, "Flash erase failed");
      free(cmd);
      return false;
    } else {
      sprintf(cmd, "Erased Flash at 0x%08x, size %u KiB",
              (unsigned)rgn->address, (unsigned)size / 1024);
      notice(BMPSTAT_SUCCESS, cmd);
    }
  }

  gdbrsp_xmit("vFlashDone", -1);
  int rcvd = gdbrsp_recv(cmd, pktsize, 500);
  if (!testreply(cmd, rcvd, "OK")) {
    notice(BMPERR_FLASHDONE, "Flash completion failed");
    free(cmd);
    return false;
  }

  free(cmd);
  return true;
}

bool bmp_blankcheck(unsigned flashsize)
{
# define BLOCKSIZE 512

  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return false;
  }
  if (bmp_flashtotal(NULL, NULL) == 0) {
    notice(BMPERR_NOFLASH, "No Flash memory record");
    return false;
  }
  char *cmd = malloc((2*BLOCKSIZE + 16) * sizeof(char));
  if (cmd == NULL) {
    notice(BMPERR_MEMALLOC, "Memory allocation error");
    return false;
  }

  bool is_success = true;
  for (const MEMBLOCK *rgn = FlashRegions.next; rgn != NULL && is_success; rgn = rgn->next) {
    bool is_blank = true;
    unsigned size = (unsigned)rgn->size;
    if (size > flashsize)
      size = flashsize;
    unsigned addr = (unsigned)rgn->address;
    unsigned remaining = size;
    while (remaining > 0 && is_blank && is_success) {
      unsigned blksize = (remaining > BLOCKSIZE) ? BLOCKSIZE : remaining;
      sprintf(cmd, "m%08X,%X:", addr, blksize);
      gdbrsp_xmit(cmd, -1);
      size_t len = gdbrsp_recv(cmd, 2*BLOCKSIZE, 1000);
      if (len == 0) {
        sprintf(cmd, "Error reading from address %08x", addr);
        notice(BMPERR_FLASHREAD, cmd);
        is_success = false;
      } else {
        for (int idx = 0; idx < len && is_blank; idx++)
          is_blank = (cmd[idx] == 'F' || cmd[idx] == 'f');
        remaining -= len;
        addr += len;
      }
    }
    if (is_success) {
      if (is_blank) {
        sprintf(cmd, "Flash region at 0x%08x, size %u KiB is blank",
                (unsigned)rgn->address, (unsigned)size / 1024);
        notice(BMPSTAT_SUCCESS, cmd);
      } else {
        sprintf(cmd, "Flash region at 0x%08x, size %u KiB contains data",
                (unsigned)rgn->address, (unsigned)size / 1024);
        notice(BMPERR_GENERAL, cmd);
      }
    }
  }

  free(cmd);
  return is_success;
# undef BLOCKSIZE
}

bool bmp_dumpflash(const char *path, unsigned flashsize)
{
# define BLOCKSIZE  512
# define FLASHLIMIT (1024*1024) /* limit size of dumped BIN file to 1 MiB */

  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return false;
  }
  if (bmp_flashtotal(NULL, NULL) == 0) {
    notice(BMPERR_NOFLASH, "No Flash memory record");
    return false;
  }

  /* get memory range */
  unsigned long base = ULONG_MAX;
  unsigned long top = 0;
  for (const MEMBLOCK *rgn = FlashRegions.next; rgn != NULL; rgn = rgn->next) {
    if (rgn->address < base)
      base = rgn->address;
    if (rgn->address + rgn->size > top)
      top = rgn->address + rgn->size;
  }
  if (top - base < flashsize)
    flashsize = top - base;
  if (flashsize > FLASHLIMIT) {
    char msg[100];
    sprintf(msg, "Flash memory size reported to be %u KiB, exceeding limit of 1024 KiB", flashsize);
    notice(BMPERR_MEMALLOC, msg);
    return false;
  }

  unsigned char *pgm = malloc(flashsize * sizeof(unsigned char));
  char *cmd = malloc((2 * BLOCKSIZE + 16) * sizeof(char));
  if (pgm == NULL || cmd == NULL) {
    notice(BMPERR_MEMALLOC, "Memory allocation error");
    if (pgm != NULL)
      free(pgm);
    if (cmd != NULL)
      free(cmd);
    return false;
  }

  /* read the data in memory first */
  memset(pgm, 0xff, flashsize * sizeof(unsigned char));
  bool is_success = true;
  for (const MEMBLOCK *rgn = FlashRegions.next; rgn != NULL && is_success; rgn = rgn->next) {
    assert(rgn->address < base + flashsize);
    unsigned size = (unsigned)rgn->size;
    if (rgn->address + size > base + flashsize)
      size = base + flashsize - rgn->address;
    unsigned addr = (unsigned)rgn->address;
    unsigned remaining = size;
    while (remaining > 0 && is_success) {
      unsigned blksize = (remaining > BLOCKSIZE) ? BLOCKSIZE : remaining;
      sprintf(cmd, "m%08X,%X:", addr, blksize);
      gdbrsp_xmit(cmd, -1);
      size_t len = gdbrsp_recv(cmd, 2*BLOCKSIZE, 1000);
      if (len == 0) {
        sprintf(cmd, "Error reading from address %08x", addr);
        notice(BMPERR_FLASHREAD, cmd);
        is_success = false;
      } else {
        cmd[2*BLOCKSIZE] = '\0';
        assert(addr >= base);
        gdbrsp_hex2array(cmd, pgm + (addr - base), flashsize - addr);
        remaining -= len/2;
        addr += len/2;
      }
    }
  }
  free(cmd);

  /* trim the data from the top */
  if (is_success) {
    top = flashsize;
    while (top > 0 && pgm[top - 1] == 0xff)
      top--;
    if (top == 0) {
      notice(BMPERR_FLASHREAD, "Flash memory is blank");
      is_success = false;
    }
    while ((top & 0x03) != 0)
      top++;  /* round up to 4 bytes */
  }

  /* now store the file */
  if (is_success) {
    FILE *fp = fopen(path, "wb");
    if (fp != NULL) {
      fwrite(pgm, 1, top, fp);
      fclose(fp);
      char msg[100];
      if (top >= 10*1024)
        sprintf(msg, "Successfully written %lu KiB", (top + 1023) / 1024);
      else
        sprintf(msg, "Successfully written %lu B", top);
      notice(BMPSTAT_SUCCESS, msg);
    } else {
      notice(BMPERR_GENERAL, "File cannot be written");
      is_success = false;
    }
  }

  free(pgm);
  return is_success;
# undef BLOCKSIZE
# undef FLASHLIMIT
}

/** bmp_parsetracereply() checks the reply for a "monitor traceswo" command.
 *  \param reply          [IN] The string with the response.
 *  \param endpoint       [OUT] The endpoint for the SWO trace is copied into
 *                        this parameter. This parameter may be NULL.
 *
 *  \return true on success, false on failure.
 */
static bool bmp_parsetracereply(const char *reply, unsigned char *endpoint)
{
  bool ok = false;

  /* first try the old reply format (1.6 up to 1.8.2): <serial>:<interface>:<endpoint> */
  const char *ptr = strchr(reply, ':');
  if (ptr != NULL && strtol(ptr + 1, (char**)&ptr, 16) == BMP_IF_TRACE && *ptr == ':') {
    long ep = strtol(ptr + 1, NULL, 16);
    ok = (ep > 0x80); /* this must be an IN enpoint, so high bit must be set */
    if (endpoint != NULL)
      *endpoint = (unsigned char)ep;
  }

  /* reply changed in release 1.9: "Trace enabled for BMP serial <serial>, USB EP <endpoint>>" */
  if (!ok && strncmp(reply, "Trace enabled", 13) == 0) {
    ptr = strstr(reply, "USB EP");
    if (ptr != NULL) {
      long ep = strtol(ptr + 6, NULL, 16);
      if (endpoint != NULL)
        *endpoint = (unsigned char)(ep | 0x80); /* direction flag is not set in the reply */
      ok = true;
    }
  }

  return ok;
}

/** bmp_enabletrace() enables trace in the Black Magic Probe.
 *  \param async_bitrate  [IN] The bitrate for ASYNC mode; set to 0 for
 *                        manchester mode.
 *  \param endpoint       [OUT] The endpoint for the SWO trace is copied into
 *                        this parameter. This parameter may be NULL.
 *
 *  \return true on success, false on failure.
 */
bool bmp_enabletrace(int async_bitrate, unsigned char *endpoint)
{
  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return false;
  }

  char buffer[100];
  int rcvd;
  for (int retry = 3; retry > 0; retry--) {
    if (async_bitrate > 0)  {
      sprintf(buffer, "qRcmd,traceswo %d", async_bitrate);
      gdbrsp_xmit(buffer, -1);
    } else {
      gdbrsp_xmit("qRcmd,traceswo", -1);
    }
    rcvd = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    if (rcvd > 0)
      break;
  }
  /* a correct answer starts with 'o' and contains a serial number, the
     interface for trace capture (0x05) and the endpoint (0x85, on the original
     Black Magic Probe) */
  buffer[rcvd] = '\0';
  bool ok = (buffer[0] == 'o') && bmp_parsetracereply(buffer + 1, endpoint);
  if (!ok)
    notice(BMPERR_MONITORCMD, "Trace setup failed");
  return ok;
}

int bmp_restart(void)
{
  char buffer[100];
  int rcvd;

  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return 0;
  }

  gdbrsp_xmit("vRun;", -1);
  rcvd = gdbrsp_recv(buffer, sizearray(buffer), 3000);
  buffer[rcvd] = '\0';
  if (buffer[0] == 'E')
    return 0;
  gdbrsp_xmit("c", -1);
  return 1;
}

/*
 to interrupt a running program, send character \x03 (without header and checksum),
 it will return with the "stop code" T02 (including header and checksum).
*/

/** bmp_runscript() executes a script with memory/register assignments, e.g.
 *  for device-specific initialization.
 *
 *  \param name     The name of the script.
 *  \param mcu      The name of the MCU driver (the MCU family name). This
 *                  parameter must be valid.
 *  \param arch     The name of the ARM Cortex architecture (M0, M3, etc.). This
 *                  parameter may be NULL.
 *  \param params   An optional array with parameters to the script, this number
 *                  of required parameters depends on the stript. This parameter
 *                  may be NULL if the script needs no parameters at all.
 *
 *  \return true on success, false on failure.
 *
 *  \note If the script returns a value, this is stored in params[0] on return.
 *        Thus, a script that has a result, should have a "params" parameter for
 *        at least one element.
 */
bool bmp_runscript(const char *name, const char *mcu, const char *arch, unsigned long *params, size_t paramcount)
{
  bmscript_clearcache();
  bmscript_load(mcu, arch);  /* very quick if the scripts for the MCU are already in memory */
  bool result = true;
  OPERAND lvalue, rvalue;
  uint16_t oper;
  while (result && bmscript_line(name, &oper, &lvalue, &rvalue)) {
    bool copyresult = false;
    if (lvalue.type == OT_PARAM) {
      if (params == NULL)
        continue;
      if (lvalue.data < paramcount)
        lvalue.data = (uint32_t)params[lvalue.data];  /* replace address parameter */
      else if (lvalue.data == ~0)
        copyresult = true;  /* special "$" parameter */
      else
        continue;           /* ignore row on invalid parameter */
    }
    char cmd[100];
    size_t len = 0;
    if (oper == OP_ORR || oper == OP_AND || oper == OP_AND_INV) {
      uint32_t cur = 0;
      uint8_t bytes[4] = { 0, 0, 0, 0 };
      sprintf(cmd, "m%08X,%X:", lvalue.data, lvalue.size);
      gdbrsp_xmit(cmd, -1);
      len = gdbrsp_recv(cmd, sizearray(cmd), 1000);
      cmd[len] = '\0';
      gdbrsp_hex2array(cmd, bytes, sizearray(bytes));
      memmove(&cur, bytes, lvalue.size);
      if (oper == OP_ORR)
        rvalue.data |= cur;
      else if (oper == OP_AND)
        rvalue.data &= cur;
      else if (oper == OP_AND_INV)
        rvalue.data &= ~cur;
    }
    if (rvalue.type == OT_PARAM) {
      if (params != NULL && rvalue.data < paramcount)
        rvalue.data = (uint32_t)params[rvalue.data];  /* replace address parameter */
      else
        continue; /* ignore row on invalid parameter */
      if (rvalue.pshift > 0)
        rvalue.data <<= rvalue.pshift;
      rvalue.data |= rvalue.plit;
    } else if (rvalue.type == OT_ADDRESS) {
      uint8_t bytes[4] = { 0, 0, 0, 0 };
      sprintf(cmd, "m%08X,%X:", rvalue.data, rvalue.size);
      gdbrsp_xmit(cmd, -1);
      len = gdbrsp_recv(cmd, sizearray(cmd), 1000);
      cmd[len] = '\0';
      gdbrsp_hex2array(cmd, bytes, sizearray(bytes));
      memmove(&rvalue.data, bytes, rvalue.size);
    }
    if (copyresult) {
      assert(params != NULL);
      params[0] = rvalue.data;
      result = true;
    } else {
      sprintf(cmd, "X%08X,%X:", lvalue.data, lvalue.size);
      len = strlen(cmd);
      memmove(cmd + len, &rvalue.data, rvalue.size);
      gdbrsp_xmit(cmd, len + rvalue.size);
      len = gdbrsp_recv(cmd, sizearray(cmd), 1000);
      result = testreply(cmd, len, "OK");
    }
  }

  return result;
}

