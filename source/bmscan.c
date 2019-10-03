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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined WIN32 || defined _WIN32
#  define STRICT
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <tchar.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

#include "bmscan.h"


#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif
#if !defined MAX_PATH
#  define MAX_PATH    300
#endif


#if defined WIN32 || defined _WIN32

/** find_bmp() scans the system for the Black Magic Probe and a specific
 *  interface. For a serial interface, it returns the COM port and for the
 *  trace or DFU interfaces, it returns the GUID (needed to open a WinUSB
 *  handle on it).
 */
int find_bmp(int interface, TCHAR *name, size_t namelen)
{
  HKEY hkeySection;
  TCHAR regpath[128], subkey[128], portname[128], basename[128], *ptr;
  DWORD maxlen;
  int idx;

  assert(name != NULL);
  assert(namelen > 0);
  *name = '\0';

  /* find the device path */
  _stprintf(regpath, _T("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_%04X&PID_%04X&MI_%02X"),
            BMP_VID, BMP_PID, interface);
  if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath, 0, KEY_READ, &hkeySection) != ERROR_SUCCESS)
    return 0;

  /* find the first sub-key */
  maxlen = sizearray(subkey);
  if (RegEnumKeyEx(hkeySection, 0, subkey, &maxlen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
    RegCloseKey(hkeySection);
    return 0;
  }
  /* add the fixed portion */
  _tcscat(subkey, "\\Device Parameters");

  if (interface == BMP_IF_GDB || interface == BMP_IF_UART) {
    /* read the port name setting */
    maxlen = sizearray(portname);
    if (RegGetValue(hkeySection, subkey, _T("PortName"), RRF_RT_REG_EXPAND_SZ | RRF_RT_REG_SZ,
                    NULL, portname, &maxlen) != ERROR_SUCCESS)
    {
      RegCloseKey(hkeySection);
      return 0;
    }
  } else {
    /* read GUID */
    LSTATUS stat;
    maxlen = sizearray(portname);
    stat = RegGetValue(hkeySection, subkey, _T("DeviceInterfaceGUIDs"),
                       RRF_RT_REG_MULTI_SZ | RRF_RT_REG_SZ, NULL, portname, &maxlen);
    /* ERROR_MORE_DATA is returned because there may technically be more GUIDs
       assigned to the device; we only care about the first one */
    if (stat != ERROR_SUCCESS && stat != ERROR_MORE_DATA)
    {
      RegCloseKey(hkeySection);
      return 0;
    }
  }

  RegCloseKey(hkeySection);

  if (interface == BMP_IF_GDB || interface == BMP_IF_UART) {
    /* skip all characters until we find a digit */
    if ((ptr = _tcsrchr(portname, _T('\\'))) != NULL)
      _tcscpy(basename, ptr + 1); /* skip '\\.\', if present */
    else
      _tcscpy(basename, portname);
    for (idx = 0; basename[idx] != '\0' && (basename[idx] < '0' || basename[idx] > '9'); idx++)
      /* empty */;
    if (basename[idx] == '\0')
      return 0;

    /* check that the COM port exists (if it doesn't, we just read the "preferred"
       COM port for the Black Magic Probe) */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("HARDWARE\\DEVICEMAP\\SERIALCOMM"), 0,
                     KEY_READ, &hkeySection) != ERROR_SUCCESS)
      return 0;

    idx = 0;
    for ( ;; ) {
      TCHAR value[128], *ptr;
      DWORD valsize;
      maxlen = sizearray(portname);
      valsize = sizearray(value);
      if (RegEnumValue(hkeySection, idx, portname, &maxlen, NULL, NULL, value, &valsize) != ERROR_SUCCESS)
        break;
      if ((ptr = _tcsrchr(value, _T('\\'))) != NULL)
        ptr += 1;   /* skip '\\.\', if present */
      else
        ptr = value;
      if (_tcsicmp(ptr, basename) == 0) {
        _tcsncpy(name, basename, namelen);
        name[namelen - 1] = '\0';
        break;
      }
      idx++;
    }

    RegCloseKey(hkeySection);
  } else {
    /* just return the GUID, whether or not the device is connected, must be
       handled by the caller */
    _tcsncpy(name, portname, namelen);
    name[namelen - 1] = '\0';
  }

  return _tcslen(name) > 0;
}

#else

static int gethex(const char *ptr, int length)
{
  char hexstr[20];

  assert(ptr != NULL);
  assert(length > 0 && length < sizeof hexstr);
  memcpy(hexstr, ptr, length);
  hexstr[length] = '\0';
  return (int)strtol(hexstr, NULL, 16);
}

int find_bmp(int interface, char *name, size_t namelen)
{
  DIR *dsys;
  struct dirent *dir;
  int found;

  /* run through directories in the sysfs branch */
  #define SYSFS_ROOT  "/sys/bus/usb/devices"
  dsys = opendir(SYSFS_ROOT);
  if (dsys == NULL)
    return 0;

  found = 0;
  while (!found && (dir = readdir(dsys)) != NULL) {
    if (dir->d_type == DT_LNK || (dir->d_type == DT_DIR && dir->d_name[0] != '.')) {
      /* check the modalias file */
      char path[MAX_PATH];
      FILE *fp;
      sprintf(path, SYSFS_ROOT "/%s/modalias", dir->d_name);
      fp = fopen(path, "r");
      if (fp) {
        char str[256];
        memset(str, 0, sizeof str);
        fread(str, 1, sizeof str, fp);
        fclose(fp);
        if (memcmp(str, "usb:", 4) == 0) {
          const char *vid = strchr(str, 'v');
          const char *pid = strchr(str, 'p');
          const char *inf = strstr(str, "in");
          if (vid != NULL && gethex(vid + 1, 4) == BMP_VID
              && pid != NULL && gethex(pid + 1, 4) == BMP_PID
              && inf != NULL && gethex(inf + 2, 2) == BMP_IF_GDB)
          {
            DIR *ddev;
            /* tty directory this should be present for CDC ACM class devices */
            sprintf(path, SYSFS_ROOT "/%s/tty", dir->d_name);
            /* check the name of the subdirectory inside */
            ddev = opendir(path);
            if (ddev != NULL) {
              while (!found && (dir = readdir(ddev)) != NULL) {
                if (dir->d_type == DT_LNK || (dir->d_type == DT_DIR && dir->d_name[0] != '.')) {
                  printf("Found /dev/%s\n", dir->d_name);
                  found = 1;
                }
              }
              closedir(ddev);
            }
          }
        }
      }
    }
  }

  closedir(dsys);
  return found;
}

#endif

#if defined STANDALONE

static void print_port(const char *portname)
{
  #if defined WIN32 || defined _WIN32
    if (strncmp(portname, "COM", 3) == 0 && strlen(portname) >= 5)
      printf("\\\\.\\");
  #endif
  printf("%s", portname);
}

int main(int argc,char *argv[])
{
#if !(defined WIN32 || defined _WIN32)
  typedef char TCHAR;
#endif
  TCHAR port_gdb[64], port_term[64];

  if (argc == 2) {
    if (strcmp(argv[1], "gdbserver") == 0) {
      /* print out only the gdbserver port */
      if (!find_bmp(BMP_IF_GDB, port_gdb, sizearray(port_gdb)))
        printf("unavailable");
      else
        print_port(port_gdb);
    } else if (strcmp(argv[1], "uart") == 0) {
      /* print out only the TTL UART port */
      if (!find_bmp(BMP_IF_UART, port_term, sizearray(port_term)))
        printf("unavailable");
      else
        print_port(port_term);
    }
  } else {
    /* print both ports */
    if (!find_bmp(BMP_IF_GDB, port_gdb, sizearray(port_gdb))) {
      printf("\nNo Black Magic Probe could be found on this system.\n");
      return 1;
    }

    printf("\nBlack Magic Probe found:\n");
    printf("  gdbserver port: %s\n", port_gdb);
    printf("  TTL UART port:  %s\n",
           find_bmp(BMP_IF_UART, port_term, sizearray(port_term)) ? port_term : "not detected");
  }

  return 0;
}

#endif /* STANDALONE */
