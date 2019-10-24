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

#if defined WIN32 || defined _WIN32
  #define STRICT
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <tchar.h>
  #if defined __MINGW32__ || defined __MINGW64__
    #include <winreg.h>
    #define LSTATUS LONG
  #endif
#else
  #include <dirent.h>
  #include <unistd.h>
  #include <bsd/string.h>
#endif

#include "bmscan.h"


#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif
#if !defined MAX_PATH
  #define MAX_PATH    300
#endif


#if defined WIN32 || defined _WIN32

/** find_bmp() scans the system for the Black Magic Probe and a specific
 *  interface. For a serial interface, it returns the COM port and for the
 *  trace or DFU interfaces, it returns the GUID (needed to open a WinUSB
 *  handle on it).
 *  \param seqnr    The sequence number, must be 0 to find the first connected
 *                  device, 1 to find the second connected device, and so forth.
 *  \param iface    The interface number, e,g, BMP_IF_GDB for the GDB server.
 *  \param name     The COM-port name (or interface GUID) will be copied in
 *                  this parameter.
 *  \param namelen  The size of the "name" parameter (in characters).
 */
int find_bmp(int seqnr, int iface, TCHAR *name, size_t namelen)
{
  HKEY hkeySection;
  TCHAR regpath[128];
  DWORD maxlen;
  int idx_device;

  assert(name != NULL);
  assert(namelen > 0);
  *name = '\0';

  /* find the device path */
  _stprintf(regpath, _T("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_%04X&PID_%04X&MI_%02X"),
            BMP_VID, BMP_PID, iface);
  if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath, 0, KEY_READ, &hkeySection) != ERROR_SUCCESS)
    return 0;

  /*
    Now we need to enumerate all the keys below the device path because more than
    a single BMP may have been connected to this computer.

    As we enumerate each sub-key we also check if it is the one currently connected
  */
  idx_device = 0 ;
  while (_tcslen(name) == 0 && seqnr >= 0) {
    TCHAR subkey[128], portname[128];
    HKEY hkeyItem;
    /* find the sub-key */
    maxlen = sizearray(subkey);
    if (RegEnumKeyEx(hkeySection, idx_device, subkey, &maxlen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
      RegCloseKey(hkeySection);
      return 0;
    }
    /* add the fixed portion & open the key to the item */
    _tcscat(subkey, "\\Device Parameters");
    if (RegOpenKeyEx(hkeySection, subkey, 0, KEY_READ, &hkeyItem) != ERROR_SUCCESS) {
      RegCloseKey(hkeySection);
      return 0;
    }

    maxlen = sizearray(portname);
    memset(portname, 0, maxlen);
    if (iface == BMP_IF_GDB || iface == BMP_IF_UART) {
      /* read the port name setting */
      if (RegQueryValueEx(hkeyItem, _T("PortName"), NULL, NULL, (LPBYTE)portname, &maxlen) != ERROR_SUCCESS) {
        RegCloseKey(hkeyItem);
        RegCloseKey(hkeySection);
        return 0;
      }
    } else {
      /* read GUID */
      LSTATUS stat = RegQueryValueEx(hkeyItem, _T("DeviceInterfaceGUIDs"), NULL, NULL, (LPBYTE)portname, &maxlen);
      /* continue scanning through the subkeys if we didn't find 'DeviceInterfaceGUIDs' in this one */
      if(stat == ERROR_FILE_NOT_FOUND) {
        idx_device++;
        continue;
      }
      /* ERROR_MORE_DATA is returned because there may technically be more GUIDs
         assigned to the device; we only care about the first one */
      if (stat != ERROR_SUCCESS && stat != ERROR_MORE_DATA) {
        RegCloseKey(hkeyItem);
        RegCloseKey(hkeySection);
        return 0;
      }
    }

    RegCloseKey(hkeyItem);

    if (iface == BMP_IF_GDB || iface == BMP_IF_UART) {
      TCHAR basename[128], *ptr;
      HKEY hkeySerialComm;
      int idx;
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
                      KEY_READ, &hkeySerialComm) != ERROR_SUCCESS)
        return 0; /* no COM ports at all! */
      for (idx = 0; ; idx++) {
        TCHAR value[128];
        DWORD valsize;
        maxlen = sizearray(portname);
        valsize = sizearray(value);
        if (RegEnumValue(hkeySerialComm, idx, portname, &maxlen, NULL, NULL, (LPBYTE)value, &valsize) != ERROR_SUCCESS)
          break;
        if ((ptr = _tcsrchr(value, _T('\\'))) != NULL)
          ptr += 1;   /* skip '\\.\', if present */
        else
          ptr = value;
        if (_tcsicmp(ptr, basename) == 0) {
          if (seqnr-- == 0) {
            _tcsncpy(name, basename, namelen);
            name[namelen - 1] = '\0';
          }
          break;
        }
      }
      RegCloseKey(hkeySerialComm);
    } else {
      /* just return the GUID, whether or not the device is connected, must be
         handled by the caller */
      //??? should verify presence of the device, for the case that multiple BMPs
      //    have been connected to the workstation
      if (seqnr-- == 0) {
        _tcsncpy(name, portname, namelen);
        name[namelen - 1] = '\0';
      }
    }
    idx_device++;
  }
  RegCloseKey(hkeySection);

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

/** find_bmp() scans the system for the Black Magic Probe and a specific
 *  interface. For a serial interface, it returns the COM port and for the
 *  trace or DFU interfaces, it returns the GUID (needed to open a WinUSB
 *  handle on it).
 *  \param seqnr    The sequence number, must be 0 to find the first connected
 *                  device, 1 to find the second connected device, and so forth.
 *  \param iface    The interface number, e,g, BMP_IF_GDB for the GDB server.
 *  \param name     The COM-port name (or interface GUID) will be copied in
 *                  this parameter.
 *  \param namelen  The size of the "name" parameter (in characters).
 */
int find_bmp(int seqnr, int iface, char *name, size_t namelen)
{
  DIR *dsys;
  struct dirent *dir;

  assert(name != NULL);
  assert(namelen > 0);
  *name = '\0';

  /* run through directories in the sysfs branch */
  #define SYSFS_ROOT  "/sys/bus/usb/devices"
  dsys = opendir(SYSFS_ROOT);
  if (dsys == NULL)
    return 0;

  while (strlen(name) == 0 && seqnr >= 0 && (dir = readdir(dsys)) != NULL) {
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
              while (strlen(name) == 0 && (dir = readdir(ddev)) != NULL) {
                if (dir->d_type == DT_LNK || (dir->d_type == DT_DIR && dir->d_name[0] != '.')) {
                  if (seqnr-- == 0) {
                    strlcpy(name, "/dev/", namelen);
                    strlcat(name, dir->d_name, namelen);
                  }
                }
              }
              closedir(ddev);
            }
            if (strlen(name) > 0 && iface != BMP_IF_GDB) {
              /* GDB server was found for the requested sequence number,
                 but the requested interface is the UART -> patch the directory
                 name and search again */
              char *ptr = path + strlen(path) - 5;  /* -4 for "/tty", -1 to get to the last character before "/tty" */
              assert(strlen(path) > 5);
              assert(*ptr == '0' && *(ptr-1) == '.' && *(ptr + 1) == '/');
              *ptr = iface + '0';
              *name = '\0'; /* clear device name for GDB-server (we want the name for the UART) */
              ddev = opendir(path);
              if (ddev != NULL) {
                while (strlen(name) == 0 && (dir = readdir(ddev)) != NULL) {
                  if (dir->d_type == DT_LNK || (dir->d_type == DT_DIR && dir->d_name[0] != '.')) {
                    strlcpy(name, "/dev/", namelen);
                    strlcat(name, dir->d_name, namelen);
                  }
                }
                closedir(ddev);
              }
            }
          }
        }
      }
    }
  }

  closedir(dsys);
  return strlen(name) > 0;
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

int main(int argc, char *argv[])
{
#if !(defined WIN32 || defined _WIN32)
  typedef char TCHAR;
#endif
  TCHAR port_gdb[64], port_term[64];
  int seqnr = 0;
  int print_all = 1;

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

      printf("\nBlack Magic Probe found:\n");
      printf("  gdbserver port: %s\n", port_gdb);
      printf("  TTL UART port:  %s\n",
             find_bmp(seqnr, BMP_IF_UART, port_term, sizearray(port_term)) ? port_term : "not detected");
      seqnr += 1;
    } while (print_all);
  }

  return 0;
}

#endif /* STANDALONE */
