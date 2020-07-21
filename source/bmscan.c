/*
 * Utility functions to scan for the Black Magic Probe on a system, and return
 * the (virtual) serial ports that it is assigned to. Under Microsoft Windows,
 * it scans the registry for the Black Magic Probe device, under Linux, it
 * browses through sysfs.
 *
 * Build this file with the macro STANDALONE defined on the command line to
 * create a self-contained executable.
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
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bmp-scan.h"
#include "tcpip.h"


#if defined _MSC_VER
  #define stricmp(s1,s2)  _stricmp((s1),(s2))
#elif defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  #define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif

#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif


static void print_port(const char *portname)
{
  #if defined WIN32 || defined _WIN32
    if (strncmp(portname, "COM", 3) == 0 && strlen(portname) >= 5)
      printf("\\\\.\\");
  #endif
  printf("%s", portname);
}

int main(int argc, char *argv[])
{
#if !(defined WIN32 || defined _WIN32)
  typedef char TCHAR;
#endif
  TCHAR port_gdb[64], port_term[64], port_swo[128], serial[64];
  const TCHAR *iface = NULL;
  TCHAR *ptr;
  int argbase = 1;
  long seqnr = 0;
  int print_all = 1;

  if (argc >= 2 && (argv[1][0] == '-' || argv[1][0] == '/' || argv[1][0] == '?')) {
    printf("bmscan detects the ports used by Black Magic Probe devices that are connected.\n\n"
           "There are two options:\n"
           "* The sequence number of the Black Magic Probe (if multiple are connected).\n"
           "  Alternatively, you may specify the serial number of the Black Magic Probe, in\n"
           "  hexadecimal.\n"
           "* The port name or device name to return, one of \"gdbserver\", \"uart\" or \"swo\".\n"
           "  for the ctxLink probe, this may also be \"ip\" to detect debug probes on the\n"
           "  Wi-Fi network.\n\n"
           "Examples: bmscan             - list all ports of all connected devices\n"
           "          bmscan 2           - list all ports of the second Black Magic Probe.\n"
           "          bmscan 7bb180b4    - list all ports of the Black Magic Probe with the\n"
           "                               serial number in the parameter.\n"
           "          bmscan gdbserver   - list the COM-port / tty device for GDB-server of\n"
           "                               the first device.\n"
           "          bmscan 2 swo       - list the GUID / device for the SWO trace output\n"
           "                               for the second device\n");
    return 0;
  }

  /* check command line arguments */
  serial[0] = '\0';
  if (argc >= 2 && (seqnr = strtol(argv[1], &ptr, 16)) != 0 && *ptr == '\0') {
    /* if sequence number > 9 assume a serial number */
    if (seqnr > 9)
      strcpy(serial, argv[1]);
    else
      seqnr -= 1;
    print_all = 0;
    argbase = 2;
  } else {
    seqnr = 0;
  }
  if (argbase < argc)
    iface = argv[argbase];

  /* if a serial number was passed, look it up */
  if (strlen(serial) > 0) {
    TCHAR match[64];
    int idx;
    seqnr = -1;
    for (idx = 0; seqnr == -1 && find_bmp(idx, BMP_IF_SERIAL, match, sizearray(match)); idx++)
      if (stricmp(match, serial) == 0)
        seqnr = idx;
    if (seqnr == -1) {
      printf("\nBlack Magic Probe with serial number %s is not found.\n", serial);
      return 1;
    }
  }

  if (seqnr < 0) {
    printf("\nInvalid sequence number %ld, sequence numbers start at 1.\n", seqnr + 1);
    return 1;
  }

  if (iface != NULL) {
    if (strcmp(argv[1], "gdbserver")== 0) {
      /* print out only the gdbserver port */
      if (!find_bmp(seqnr, BMP_IF_GDB, port_gdb, sizearray(port_gdb)))
        printf("unavailable");
      else
        print_port(port_gdb);
    } else if (strcmp(argv[1], "uart") == 0) {
      /* print out only the TTL UART port */
      if (!find_bmp(seqnr, BMP_IF_UART, port_term, sizearray(port_term)))
        printf("unavailable");
      else
        print_port(port_term);
    } else if (strcmp(argv[1], "swo") == 0) {
      /* print out only the SWO trace interface */
      if (!find_bmp(seqnr, BMP_IF_TRACE, port_swo, sizearray(port_swo)))
        printf("unavailable");
      else
        print_port(port_swo);
    } else if (strcmp(argv[1], "ip") == 0) {
      unsigned long addresses[10];
      int count;
      int result = tcpip_init();
      if (result != 0) {
          printf("network initialization failure (error code %d)\n", result);
          return 1;
      }
      count = scan_network(addresses, sizearray(addresses));
      if (seqnr == 0) {
        if (count == 0)
          printf("\nNo ctxLink could be found on this network.\n");
        while (seqnr < count) {
          unsigned long addr = addresses[seqnr];
          printf("\nctxLink found:\n  IP address %lu.%lu.%lu.%lu\n",
                 addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >> 24) & 0xff);
          seqnr++;
        }
      } else if (seqnr < count) {
        unsigned long addr = addresses[seqnr - 1];
        printf("%lu.%lu.%lu.%lu", addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >> 24) & 0xff);
      } else {
        printf("unavailable");
      }
      tcpip_cleanup();
    } else {
      printf("Unknown interface \"%s\"\n", iface);
    }
  } else {
    assert(!print_all || seqnr == 0); /* if seqnr were set, print_all is false */
    do {
      /* print both ports of each Black Magic Probe */
      if (!find_bmp(seqnr, BMP_IF_GDB, port_gdb, sizearray(port_gdb))) {
        if (print_all && seqnr > 0)
          break;  /* simply exit the do..while loop without giving a further message */
        switch (seqnr) {
        case 0:
          printf("\nNo Black Magic Probe could be found on this system.\n");
          break;
        case 1:
          printf("\nNo %ldnd Black Magic Probe could be found on this system.\n", seqnr + 1);
          break;
        case 2:
          printf("\nNo %ldrd Black Magic Probe could be found on this system.\n", seqnr + 1);
          break;
        default:
          printf("\nNo %ldth Black Magic Probe could be found on this system.\n", seqnr + 1);
        }
        return 1;
      }

      if (!find_bmp(seqnr, BMP_IF_UART, port_term, sizearray(port_term)))
        strcpy(port_term, "not detected");
      if (!find_bmp(seqnr, BMP_IF_TRACE, port_swo, sizearray(port_swo)))
        strcpy(port_swo, "not detected");
      if (!find_bmp(seqnr, BMP_IF_SERIAL, serial, sizearray(serial)))
        strcpy(serial, "(unknown)");

      printf("\nBlack Magic Probe found, serial %s:\n", serial);
      printf("  gdbserver port: %s\n", port_gdb);
      printf("  TTL UART port:  %s\n", port_term);
      printf("  SWO interface:  %s\n", port_swo);
      seqnr += 1;
    } while (print_all);
  }

  return 0;
}

