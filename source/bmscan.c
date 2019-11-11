/*
 * Utility functions to scan for the Black Magic Probe on a system, and return
 * the (virtual) serial ports that it is assigned to. Under Microsoft Windows,
 * it scans the registry for the Black Magic Probe device, under Linux, it
 * browses through sysfs.
 *
 * Build this file with the macro STANDALONE defined on the command line to
 * create a self-contained executable.
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
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bmp-scan.h"


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
  TCHAR port_gdb[64], port_term[64], port_swo[128];
  int seqnr = 0;
  int print_all = 1;

  if (argc >= 2 && (argv[1][0] == '-' || argv[1][0] == '/' || argv[1][0] == '?')) {
    printf("bmscan detects the ports used by Black Magic Probe devices that are connected.\n\n"
           "There are two options:\n"
           "* The sequence number of the Black Magic Probe (if multiple are connected).\n"
           "* The port name or device name to return, one of \"gdbserver\", \"uart\" or \"swo\".\n\n"
           "Examples: bmscan             - list all ports of all connected devices\n"
           "          bmscan 2           - list all ports of the second Black Magic Probe.\n"
           "          bmscan gdbserver   - list the COM-port / tty device for GDB-server of\n"
           "                               the first device.\n"
           "          bmscan 2 swo       - list the GUID / device for the SWO trace output\n"
           "                               for the second device\n");
    return 0;
  }

  if (argc >= 3) {
    seqnr = atoi(argv[2]) - 1;
  } else if (argc >= 2 && isdigit(argv[1][0])) {
    seqnr = atoi(argv[1]) - 1;
    print_all = 0;
    argc -= 1;
  }
  if (seqnr < 0) {
    printf("\nInvalid sequence number %d, sequence numbers start at 1.\n", seqnr + 1);
    return 1;
  }

  if (argc >= 2) {
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
          printf("\nNo %dnd Black Magic Probe could be found on this system.\n", seqnr + 1);
          break;
        case 2:
          printf("\nNo %drd Black Magic Probe could be found on this system.\n", seqnr + 1);
          break;
        default:
          printf("\nNo %dth Black Magic Probe could be found on this system.\n", seqnr + 1);
        }
        return 1;
      }

      if (!find_bmp(seqnr, BMP_IF_UART, port_term, sizearray(port_term)))
        strcpy(port_term, "not detected");
      if (!find_bmp(seqnr, BMP_IF_TRACE, port_swo, sizearray(port_swo)))
        strcpy(port_swo, "not detected");

      printf("\nBlack Magic Probe found:\n");
      printf("  gdbserver port: %s\n", port_gdb);
      printf("  TTL UART port:  %s\n", port_term);
      printf("  SWO interface:  %s\n", port_swo);
      seqnr += 1;
    } while (print_all);
  }

  return 0;
}

