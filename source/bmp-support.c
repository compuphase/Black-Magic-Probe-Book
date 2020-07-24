/*
 * General purpose Black Magic Probe support routines, based on the GDB-RSP
 * serial interface. The "script" support can also be used with GDB.
 *
 * Copyright 2019-2020 CompuPhase
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
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
    #include "strlcpy.h"
  #endif
#else
  #include <unistd.h>
  #include <bsd/string.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "bmp-scan.h"
#include "bmp-script.h"
#include "bmp-support.h"
#include "crc32.h"
#include "elf.h"
#include "gdb-rsp.h"
#include "rs232.h"
#include "tcpip.h"
#include "xmltractor.h"

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif
#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif


typedef struct tagFLASHRGN {
  unsigned long address;
  unsigned long size;
  unsigned int blocksize;
} FLASHRGN;
#define MAX_FLASHRGN  8

static int CurrentProbe = -1;
static int PacketSize = 0;
static FLASHRGN FlashRgn[MAX_FLASHRGN];
static int FlashRgnCount = 0;

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

/** bmp_setcallback() sets the callback function for detailed status
 *  messages. The callback receives status codes as well as a text message.
 *  All error codes are negative.
 */
void bmp_setcallback(BMP_STATCALLBACK func)
{
  stat_callback = func;
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
 *  \return 1 on success, 0 on failure. Status and error messages are passed via
 *          the callback.
 */
int bmp_connect(int probe, const char *ipaddress)
{
  char devname[128], probename[64];
  int initialize = 0;

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

  if (CurrentProbe >= 0 && !rs232_isopen()) {
    /* serial port is selected, and it is currently not open */
    FlashRgnCount = 0;
    if (find_bmp(probe, BMP_IF_GDB, devname, sizearray(devname))) {
      char buffer[256];
      size_t size;
      /* connect to the port */
      rs232_open(devname,115200,8,1,PAR_NONE);
      if (!rs232_isopen()) {
        notice(BMPERR_PORTACCESS, "Failure opening port %s", devname);
        return 0;
      }
      rs232_rts(1);
      rs232_dtr(1); /* required by GDB RSP */
      /* check for reception of the handshake */
      size = gdbrsp_recv(buffer, sizearray(buffer), 500);
      if (size == 0) {
        /* toggle DTR, to be sure */
        rs232_rts(0);
        rs232_dtr(0);
        #if defined _WIN32
          Sleep(200);
        #else
          usleep(200 * 1000);
        #endif
        rs232_rts(1);
        rs232_dtr(1);
        size = gdbrsp_recv(buffer, sizearray(buffer), 500);
      }
      if (size != 2 || memcmp(buffer, "OK", size)!= 0) {
        notice(BMPERR_NORESPONSE, "No response on %s", devname);
        rs232_close();
        return 0;
      }
      initialize = 1;
    }
  }

  if (CurrentProbe < 0 && ipaddress != NULL && !tcpip_isopen()) {
    /* network interface is selected, and it is currently not open */
    tcpip_open(ipaddress);
    if (!tcpip_isopen()) {
      notice(BMPERR_PORTACCESS, "Failure opening gdbserver at %s", devname);
      return 0;
    }
    initialize = 1;
  }

  /* check whether opening the communication interface succeeded */
  if ((CurrentProbe >= 0 && !rs232_isopen()) || (CurrentProbe < 0 && !tcpip_isopen())) {
    /* initialization failed */
    notice(BMPERR_NODETECT, "%s not detected", probename);
    return 0;
  }

  if (initialize) {
    char buffer[256], *ptr;
    size_t size;
    int retry;
    /* query parameters */
    gdbrsp_xmit("qSupported:multiprocess+", -1);
    size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    buffer[size] = '\0';
    if ((ptr = strstr(buffer, "PacketSize=")) != NULL)
      PacketSize = (int)strtol(ptr + 11, NULL, 16);
    gdbrsp_packetsize(PacketSize+16); /* allow for some margin */
    //??? check for "qXfer:memory-map:read+" as well
    /* connect to gdbserver */
    for (retry = 3; retry > 0; retry--) {
      gdbrsp_xmit("!",-1);
      size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
      if (size == 2 && memcmp(buffer, "OK", size) == 0)
        break;
      #if defined _WIN32
        Sleep(200);
      #else
        usleep(200 * 1000);
      #endif
    }
    if (retry == 0) {
      notice(BMPERR_NOCONNECT, "Connect failed on %s", devname);
      bmp_disconnect();
      return 0;
    }
    notice(BMPSTAT_SUCCESS, "Connected to %s (%s)", probename, devname);
  }

  return 1;
}

/** bmp_disconnect() closes the connection to the Black Magic Probe, it one was
 *  active.
 *
 *  \return 1 on success, 0 if no connection was open.
 */
int bmp_disconnect(void)
{
  int result = 0;

  if (rs232_isopen()) {
    rs232_dtr(0);
    rs232_rts(0);
    rs232_close();
    result = 1;
  }
  if (tcpip_isopen()) {
    tcpip_close();
    result = 1;
  }
  return result;
}

/** bmp_isopen() returns whether a connection to a Black Magic Probe or a
 *  ctxLink is open, cia USB or TCP/IP.
 */
int bmp_isopen(void)
{
  return rs232_isopen() || tcpip_isopen();
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
 *  \param tpwr         Set to 1 to power up the voltage-sense pin, 0 to
 *                      power-down, or 2 to optionally power this pin if the
 *                      initial scan returns a power of 0.0V.
 *  \param connect_srst Set to 1 to let the Black Magic Probe keep the target
 *                      MCU in reset while scanning and attaching.
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
 *  \return 1 on success, 0 on failure. Status and error messages are passed via
 *          the callback.
 */
int bmp_attach(int tpwr, int connect_srst, char *name, size_t namelength, char *arch, size_t archlength)
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
    return 0;
  }

restart:
  if (connect_srst != 0) {
    gdbrsp_xmit("qRcmd,connect_srst enable", -1);
    do {
      size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    } while (size > 0 && buffer[0] == 'o'); /* ignore console output */
    if (size != 2 || memcmp(buffer, "OK", size) != 0)
      notice(BMPERR_MONITORCMD, "Setting connect-with-reset option failed");
  }
  if (tpwr == 1) {
    gdbrsp_xmit("qRcmd,tpwr enable", -1);
    do {
      size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    } while (size > 0 && buffer[0] == 'o'); /* ignore console output */
    if (size != 2 || memcmp(buffer, "OK", size) != 0) {
      notice(BMPERR_MONITORCMD, "Power to target failed");
    } else {
      /* give the micro-controller a bit of time to start up, before issuing
         the swdp_scan command */
      #if defined _WIN32
        Sleep(100);
      #else
        usleep(100 * 1000);
      #endif
    }
  }
  gdbrsp_xmit("qRcmd,swdp_scan", -1);
  for ( ;; ) {
    size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    if (size > 2 && buffer[0] == 'o') {
      const char *ptr;
      buffer[size] = '\0';
      /* parse the string */
      if (tpwr == 2 && strchr(buffer, '\n') != NULL && (ptr = strstr(buffer + 1, "voltage:")) != NULL) {
        double voltage = strtod(ptr + 8, (char**)&ptr);
        if (*ptr == 'V' && voltage < 0.1) {
          notice(BMPSTAT_NOTICE, "Note: powering target");
          tpwr = 1;
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
    } else if (size != 2 || memcmp(buffer, "OK", size) != 0) {
      /* error message was already given by an "output"-response */
      return 0;
    } else {
      break;  /* OK was received */
    }
  }
  gdbrsp_xmit("vAttach;1", -1);
  size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
  /* accept OK, S##, T## (but in fact, Black Magic Probe always sends T05) */
  ok = (size == 2 && memcmp(buffer, "OK", size) == 0)
       || (size == 3 && buffer[0] == 'S' && isxdigit(buffer[1]) && isxdigit(buffer[2]))
       || (size >= 3 && buffer[0] == 'T' && isxdigit(buffer[1]) && isxdigit(buffer[2]));
  if (!ok) {
    notice(BMPERR_ATTACHFAIL, "Attach failed");
    return 0;
  }
  notice(BMPSTAT_NOTICE, "Attached to target 1");

  /* check memory map and features of the target */
  FlashRgnCount = 0;
  sprintf(buffer, "qXfer:memory-map:read::0,%x", PacketSize - 4);
  gdbrsp_xmit(buffer, -1);
  size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
  if (size > 10 && buffer[0] == 'm') {
    xt_Node* root = xt_parse(buffer + 1);
    if (root != NULL && FlashRgnCount < MAX_FLASHRGN) {
      xt_Node* node = xt_find_child(root, "memory");
      while (node != NULL) {
        xt_Attrib* attrib = xt_find_attrib(node, "type");
        if (attrib != NULL && attrib->szvalue == 5 && strncmp(attrib->value, "flash", attrib->szvalue) == 0) {
          xt_Node* prop;
          memset(&FlashRgn[FlashRgnCount], 0, sizeof(FLASHRGN));
          if ((attrib = xt_find_attrib(node, "start")) != NULL)
            FlashRgn[FlashRgnCount].address = strtoul(attrib->value, NULL, 0);
          if ((attrib = xt_find_attrib(node, "length")) != NULL)
            FlashRgn[FlashRgnCount].size = strtoul(attrib->value, NULL, 0);
          if ((prop = xt_find_child(node, "property")) != NULL
              && (attrib = xt_find_attrib(prop, "name")) != NULL
              && attrib->szvalue == 9 && strncmp(attrib->value, "blocksize", attrib->szvalue) == 0)
            FlashRgn[FlashRgnCount].blocksize = strtoul(prop->content, NULL, 0);
          FlashRgnCount += 1;
        }
        node = xt_find_sibling(node, "memory");
      }
      xt_destroy_node(root);
    }
  }
  if (FlashRgnCount == 0)
    notice(BMPERR_NOFLASH, "No Flash memory record");

  return 1;
}

int bmp_detach(int powerdown)
{
  int result = 1;

  if (bmp_isopen()) {
    char buffer[100];
    size_t size;
    /* optionally disable power */
    if (powerdown) {
      gdbrsp_xmit("qRcmd,tpwr disable", -1);
      do {
        size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
      } while (size > 0 && buffer[0] == 'o'); /* ignore console output */
      if (size != 2 || memcmp(buffer, "OK", size) != 0)
        result = 0;
    }
    /* detach */
    gdbrsp_xmit("D", -1);
    size = gdbrsp_recv(buffer, sizearray(buffer), 1000);
    if (size != 2 || memcmp(buffer, "OK", size) != 0)
      result = 0;
  }

  return result;
}

int bmp_fullerase(void)
{
  char *cmd;
  int rgn, rcvd, pktsize;

  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return 0;
  }
  if (FlashRgnCount == 0) {
    notice(BMPERR_NOFLASH, "No Flash memory record");
    return 0;
  }
  pktsize = (PacketSize > 0) ? PacketSize : 64;
  cmd = malloc((pktsize + 16) * sizeof(char));
  if (cmd == NULL) {
    notice(BMPERR_MEMALLOC, "Memory allocation error");
    return 0;
  }

  for (rgn = 0; rgn < FlashRgnCount; rgn++) {
    unsigned long size = FlashRgn[rgn].size;
    int failed;
    do {
      sprintf(cmd, "vFlashErase:%x,%x", (unsigned)FlashRgn[rgn].address, (unsigned)size);
      gdbrsp_xmit(cmd, -1);
      rcvd = gdbrsp_recv(cmd, pktsize, 500);
      failed = (rcvd != 2 || memcmp(cmd, "OK", rcvd) != 0);
      if (failed)
        size /= 2;
    } while (failed && size >= 1024);
    if (failed) {
      notice(BMPERR_FLASHERASE, "Flash erase failed");
      free(cmd);
      return 0;
    } else {
      sprintf(cmd, "Erased Flash at 0x%08x, size %u KiB",
              (unsigned)FlashRgn[rgn].address, (unsigned)size / 1024);
      notice(BMPSTAT_SUCCESS, cmd);
    }
  }

  gdbrsp_xmit("vFlashDone", -1);
  rcvd = gdbrsp_recv(cmd, pktsize, 500);
  if (rcvd != 2 || memcmp(cmd, "OK", rcvd)!= 0) {
    notice(BMPERR_FLASHDONE, "Flash completion failed");
    free(cmd);
    return 0;
  }

  free(cmd);
  return 1;
}

int bmp_download(FILE *fp)
{
  char *cmd;
  int rgn, rcvd, pktsize;

  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return 0;
  }
  if (FlashRgnCount == 0) {
    notice(BMPERR_NOFLASH, "No Flash memory record");
    return 0;
  }
  pktsize = (PacketSize > 0) ? PacketSize : 64;
  cmd = malloc((pktsize + 16) * sizeof(char));
  if (cmd == NULL) {
    notice(BMPERR_MEMALLOC, "Memory allocation error");
    return 0;
  }

  assert(fp != NULL);
  for (rgn = 0; rgn < FlashRgnCount; rgn++) {
    int segment, type;
    unsigned long topaddr, flashsectors, paddr, vaddr, fileoffs, filesize;
    /* walk through all segments in the ELF file that fall into this region */
    topaddr = 0;
    for (segment = 0; elf_segment_by_index(fp, segment, &type, &fileoffs, &filesize, NULL, &paddr, NULL) == ELFERR_NONE; segment++)
      if (type == 1 && paddr >= FlashRgn[rgn].address && paddr < FlashRgn[rgn].address + FlashRgn[rgn].size)
        topaddr = paddr + filesize;
    if (topaddr == 0)
      continue; /* no segment fitting in this Flash sector */
    /* erase the Flash memory */
    assert(topaddr <= FlashRgn[rgn].address + FlashRgn[rgn].size);
    flashsectors = ((topaddr - FlashRgn[rgn].address + (FlashRgn[rgn].blocksize - 1)) / FlashRgn[rgn].blocksize);
    assert(flashsectors * FlashRgn[rgn].blocksize <= FlashRgn[rgn].address + FlashRgn[rgn].size);
    sprintf(cmd, "vFlashErase:%x,%x", (unsigned)FlashRgn[rgn].address, (unsigned)(flashsectors * FlashRgn[rgn].blocksize));
    gdbrsp_xmit(cmd, -1);
    rcvd = gdbrsp_recv(cmd, pktsize, 500);
    if (rcvd != 2 || memcmp(cmd, "OK", rcvd)!= 0) {
      notice(BMPERR_FLASHERASE, "Flash erase failed");
      free(cmd);
      return 0;
    }
    /* walk through all segments again, to download the payload */
    for (segment = 0; elf_segment_by_index(fp, segment, &type, &fileoffs, &filesize, &vaddr, &paddr, NULL) == ELFERR_NONE; segment++) {
      unsigned char *data;
      unsigned pos, numbytes, esccount, idx;
      if (type != 1 || filesize == 0 || paddr < FlashRgn[rgn].address || paddr >= FlashRgn[rgn].address + FlashRgn[rgn].size)
        continue;
      notice(BMPSTAT_NOTICE, "%d: %s segment at 0x%x length 0x%x", segment, (vaddr == paddr) ? "Code" : "Data", (unsigned)paddr, (unsigned)filesize);
      data = malloc(filesize);
      if (data == NULL) {
        notice(BMPERR_MEMALLOC, "Memory allocation failure");
        free(cmd);
        return 0;
      }
      fseek(fp, fileoffs, SEEK_SET);
      fread(data, 1, filesize, fp);
      for (pos = 0; pos < filesize; pos += numbytes) {
        unsigned prefixlen;
        sprintf(cmd, "vFlashWrite:%x:", (unsigned)(paddr + pos));
        prefixlen = strlen(cmd) + 4;  /* +1 for '$', +3 for '#nn' checksum */
        /* make blocks that are a multiple of 16 bytes (for guaranteed alignment)
           that are less than (or equal to) PacketSize; start by subtracting the
           prefix length */
        numbytes = (pktsize - prefixlen) & ~0x0f;
        if (pos + numbytes > filesize)
          numbytes = filesize - pos;
        /* check how many bytes in the packet must be escaped, then check
           whether the packet would still fit (decrement the block length
           otherwise) */
        for ( ;; ) {
          esccount = 0;
          for (idx = 0; idx < numbytes; idx++)
            if (data[pos + idx] == '$' || data[pos + idx] == '#' || data[pos + idx] == '}')
              esccount += 1;
          if (numbytes + esccount + prefixlen <= (unsigned)pktsize)
            break;
          numbytes -= 16;
        }
        memmove(cmd + (prefixlen - 4), data + pos, numbytes);
        gdbrsp_xmit(cmd, (prefixlen - 4) + numbytes);
        rcvd = gdbrsp_recv(cmd, pktsize, 500);
        if (rcvd != 2 || memcmp(cmd, "OK", rcvd)!= 0) {
          notice(BMPERR_FLASHWRITE, "Flash write failed");
          free(data);
          free(cmd);
          return 0;
        }
      }
      free(data);
    }
    gdbrsp_xmit("vFlashDone", -1);
    rcvd = gdbrsp_recv(cmd, pktsize, 500);
    if (rcvd != 2 || memcmp(cmd, "OK", rcvd)!= 0) {
      notice(BMPERR_FLASHDONE, "Flash completion failed");
      free(cmd);
      return 0;
    }
  }

  free(cmd);
  return 1;
}

int bmp_verify(FILE *fp)
{
  char cmd[100];
  int segment, sector, type, allmatch;
  unsigned long offset, filesize, paddr;

  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return 0;
  }

  /* run over all segments in the ELF file */
  allmatch = 1;
  assert(fp != NULL);
  for (segment = 0;
       elf_segment_by_index(fp, segment, &type, &offset, &filesize, NULL, &paddr, NULL) == ELFERR_NONE;
       segment++)
  {
    unsigned char *data;
    unsigned crc_src, crc_tgt;
    size_t rcvd;

    if (type != 1 || filesize == 0)
      continue;   /* no loadable data */
    /* also check that paddr falls within a Flash memory sector */
    for (sector = 0; sector < FlashRgnCount; sector++)
      if (paddr >= FlashRgn[sector].address && paddr < FlashRgn[sector].address + FlashRgn[sector].size)
        break;
    if (sector >= FlashRgnCount)
      continue; /* segment is outside of any Flash sector */
    /* read entire segment, calc CRC */
    data = malloc((size_t)filesize * sizeof (unsigned char));
    if (data == NULL) {
      notice(BMPERR_MEMALLOC, "Memory allocation failure");
      return 0;
    }
    fseek(fp, offset, SEEK_SET);
    fread(data, 1, filesize, fp);
    crc_src = (unsigned)gdb_crc32((uint32_t)~0, data, filesize);
    free(data);
    /* request CRC from Black Magic Probe */
    sprintf(cmd, "qCRC:%lx,%lx",paddr,filesize);
    gdbrsp_xmit(cmd, -1);
    rcvd = gdbrsp_recv(cmd, sizearray(cmd), 3000);
    cmd[rcvd] = '\0';
    crc_tgt = (rcvd >= 2 && cmd[0] == 'C') ? strtoul(cmd + 1, NULL, 16) : 0;
    if (crc_tgt != crc_src) {
      notice(BMPERR_FLASHCRC, "Segment %d data mismatch", segment);
      allmatch = 0;
    }
  }
  if (allmatch)
    notice(BMPSTAT_SUCCESS, "Verification successful");

  return allmatch;
}

/** bmp_enabletrace() code enables trace in the Black Magic Probe.
 *  \param async_bitrate  [IN] The bitrate for ASYNC mode; set to 0 for
 *                        manchester mode.
 *  \param endpoint       [OUT] The endpoint for the SWO trace is copied into
 *                        this parameter. This parameter may be NULL.
 *
 *  \return 1 on success, 0 on failure.
 */
int bmp_enabletrace(int async_bitrate, unsigned char *endpoint)
{
  char buffer[100], *ptr;
  int rcvd, ok;

  if (!bmp_isopen()) {
    notice(BMPERR_NOCONNECT, "Not connected to Black Magic Probe");
    return 0;
  }

  if (async_bitrate > 0)  {
    sprintf(buffer, "qRcmd,traceswo %d", async_bitrate);
    gdbrsp_xmit(buffer, -1);
  } else {
    gdbrsp_xmit("qRcmd,traceswo", -1);
  }
  rcvd = gdbrsp_recv(buffer, sizearray(buffer), 1000);
  /* a correct answer starts with 'o' and contains a serial number, the
     interface for trace capture (0x05) and the endpoint (0x85, on the original
     Black Magic Probe) */
  assert(rcvd >= 0);
  buffer[rcvd] = '\0';
  ok = ((ptr = strchr(buffer, ':')) != NULL && strtol(ptr + 1, &ptr, 16) == BMP_IF_TRACE && *ptr == ':');
  if (ok) {
    long ep = strtol(ptr + 1, NULL, 16);
    ok = (ep > 0x80); /* this must be an IN enpoint, so high bit must be set */
    if (endpoint != NULL)
      *endpoint = (unsigned char)ep;
  }
  if (!ok) {
    notice(BMPERR_MONITORCMD, "Trace setup failed");
    return 0;
  }
  return 1;
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

static int hex2byte_array(const char *hex, unsigned char *byte)
{
  assert(hex != NULL && byte != NULL);
  while (hex[0] != '\0' && hex[1] != '\0') {
    unsigned char h, l;
    if (hex[0] >= '0' && hex[0] <= '9')
      h = hex[0] - '0';
    else if (hex[0] >= 'a' && hex[0] <= 'f')
      h = hex[0] - 'a' + 10;
    else if (hex[0] >= 'A' && hex[0] <= 'F')
      h = hex[0] - 'A' + 10;
    else
      return 0;
    if (hex[1] >= '0' && hex[1] <= '9')
      l = hex[1] - '0';
    else if (hex[1] >= 'a' && hex[1] <= 'f')
      l = hex[1] - 'a' + 10;
    else if (hex[1] >= 'A' && hex[1] <= 'F')
      l = hex[1] - 'A' + 10;
    else
      return 0;
    *byte++ = (h << 4) | l;
    hex += 2;
  }
  return *hex == '\0';
}

/** bmp_runscript() executes a script with memory/register assignments, e.g.
 *  for device-specific initialization.
 *
 *  \param name     The name of the script.
 *  \param driver   The name of the MCU driver (the MCU family name).
 *  \param params   An optional array with parameters to the script, this number
 *                  of required parameters depends on the stript.
 *
 *  \return 1 on success, 0 on failure.
 *
 *  \note When the line of a script has a magic value for the "value" field, it
 *        is replaced by a parameter.
 */
int bmp_runscript(const char *name, const char *driver, const unsigned long *params)
{
  uint32_t address, value;
  uint8_t size;
  char oper;
  int result;

  bmscript_clearcache();
  bmscript_load(driver);  /* very quick if the scripts are already in memory */
  result = 1;
  while (result && bmscript_line(name, &oper, &address, &value, &size)) {
    char cmd[100];
    size_t len = 0;
    if ((value & ~0xf) == SCRIPT_MAGIC) {
      assert(params != NULL);
      value = (uint32_t)params[value & 0xf];  /* replace parameters */
    }
    if (oper == '|' || oper == '&' || oper == '~') {
      uint32_t cur = 0;
      uint8_t bytes[4] = { 0, 0, 0, 0 };
      sprintf(cmd, "m%08X,%X:", address, size);
      gdbrsp_xmit(cmd, -1);
      len = gdbrsp_recv(cmd, sizearray(cmd), 1000);
      cmd[len] = '\0';
      hex2byte_array(cmd, bytes);
      memmove(&cur, bytes, size);
      if (oper == '|')
        value |= cur;
      else if (oper == '&')
        value &= cur;
      else
        value &= ~cur;
    }
    sprintf(cmd, "X%08X,%X:", address, size);
    len = strlen(cmd);
    memmove(cmd + len, &value, size);
    gdbrsp_xmit(cmd, len + size);
    len = gdbrsp_recv(cmd, sizearray(cmd), 1000);
    result = (len == 2 && memcmp(cmd, "OK", len) == 0);
  }

  return result;
}

