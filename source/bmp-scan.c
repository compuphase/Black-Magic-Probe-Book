/*
 * Utility functions to scan for the Black Magic Probe on a system, and return
 * the (virtual) serial ports that it is assigned to. Under Microsoft Windows,
 * it scans the registry for the Black Magic Probe device, under Linux, it
 * browses through sysfs.
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
  #include <pthread.h>
  #include <unistd.h>
  #include <bsd/string.h>
#endif

#include "bmp-scan.h"
#include "tcpip.h"


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
 *  \param seqnr    [in] The sequence number, must be 0 to find the first
 *                  connected device, 1 to find the second connected device, and
 *                  so forth.
 *  \param iface    [in] The interface number, e,g, BMP_IF_GDB for the GDB
 *                  server.
 *  \param name     [out] The COM-port name (or interface GUID) will be copied
 *                  in this parameter.
 *  \param namelen  [in] The size of the "name" parameter (in characters).
 *
 *  \return 1 on success, 0 on failure.
 */
int find_bmp(int seqnr, int iface, TCHAR *name, size_t namelen)
{
  HKEY hkeySection, hkeyItem;
  TCHAR regpath[128];
  TCHAR subkey[128], portname[128], basename[128], *ptr;
  DWORD maxlen;
  int idx_device;
  BOOL found;

  assert(name != NULL);
  assert(namelen > 0);
  *name = '\0';

  /* find the device path for the GDB server */
  _stprintf(regpath, _T("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_%04X&PID_%04X&MI_%02X"),
            BMP_VID, BMP_PID, BMP_IF_GDB);
  if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath, 0, KEY_READ, &hkeySection) != ERROR_SUCCESS)
    return 0;

  /* Now we need to enumerate all the keys below the device path because more
     than a single BMP may have been connected to this computer.
     As we enumerate each sub-key we also check if it is the one currently
     connected */
  found = FALSE;
  idx_device = 0;
  while (!found && seqnr >= 0) {
    HKEY hkeySerialComm;
    int idx;
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
    /* read the port name setting */
    maxlen = sizearray(portname);
    memset(portname, 0, maxlen);
    if (RegQueryValueEx(hkeyItem, _T("PortName"), NULL, NULL, (LPBYTE)portname, &maxlen) != ERROR_SUCCESS) {
      RegCloseKey(hkeyItem);
      RegCloseKey(hkeySection);
      return 0;
    }
    RegCloseKey(hkeyItem);
    /* clean up the port name and check that it looks correct (for a COM port) */
    if ((ptr = _tcsrchr(portname, _T('\\'))) != NULL)
      _tcscpy(basename, ptr + 1);     /* skip '\\.\', if present */
    else
      _tcscpy(basename, portname);
    for (idx = 0; basename[idx] != '\0' && (basename[idx] < '0' || basename[idx] > '9'); idx++)
      /* nothing */;
    if (basename[idx] == '\0') {  /* there is no digit in the port name, this can't be right */
      RegCloseKey(hkeySection);
      return 0;
    }

    /* check that the COM port exists (if it doesn't, portname is the "preferred"
       COM port for the Black Magic Probe, which is disconnected at the moment) */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("HARDWARE\\DEVICEMAP\\SERIALCOMM"), 0,
                    KEY_READ, &hkeySerialComm) != ERROR_SUCCESS) {
      RegCloseKey(hkeySection);
      return 0; /* no COM ports at all! */
    }
    for (idx = 0; !found; idx++) {
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
      if (_tcsicmp(ptr, basename) == 0 && seqnr-- == 0)
        found = TRUE;
    }
    RegCloseKey(hkeySerialComm);
    idx_device += 1;
  }
  RegCloseKey(hkeySection);

  if (!found)
    return 0;

  /* if we were querying for the port for GDB-server, the port name just found
     is also the one we need */
  if (iface == BMP_IF_GDB) {
    _tcsncpy(name, basename, namelen);
    name[namelen - 1] = '\0';
    return _tcslen(name) > 0;
  }

  /* for the serial number, get the Container ID one level up, then look up
     the composite device with the same Container ID */
  if (iface == BMP_IF_SERIAL) {
    TCHAR cid_iface[128], cid_device[128];
    LSTATUS stat;
    _stprintf(regpath, _T("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_%04X&PID_%04X&MI_%02X"),
              BMP_VID, BMP_PID, BMP_IF_GDB);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath, 0, KEY_READ, &hkeySection) != ERROR_SUCCESS)
      return 0; /* this should not happen, because we opened it just a while ago */
    ptr = _tcschr(subkey, '\\');
    assert(ptr != NULL);
    *ptr = '\0';
    if (RegOpenKeyEx(hkeySection, subkey, 0, KEY_READ, &hkeyItem) != ERROR_SUCCESS) {
      RegCloseKey(hkeySection);
      return 0;
    }
    maxlen = sizearray(cid_iface);
    memset(cid_iface, 0, maxlen);
    stat = RegQueryValueEx(hkeyItem, _T("ContainerID"), NULL, NULL, (LPBYTE)cid_iface, &maxlen);
    RegCloseKey(hkeyItem);
    RegCloseKey(hkeySection);
    if (stat != ERROR_SUCCESS)
      return 0;
    /* go to the entry for the composite device */
    _stprintf(regpath, _T("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_%04X&PID_%04X"),
              BMP_VID, BMP_PID);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath, 0, KEY_READ, &hkeySection) != ERROR_SUCCESS)
      return 0;
    idx_device = 0;
    for ( ;; ) {
      maxlen = sizearray(subkey);
      if (RegEnumKeyEx(hkeySection, idx_device, subkey, &maxlen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
        RegCloseKey(hkeySection);
        return 0;
      }
      if (RegOpenKeyEx(hkeySection, subkey, 0, KEY_READ, &hkeyItem) != ERROR_SUCCESS) {
        RegCloseKey(hkeySection);
        return 0;
      }
      /* read the Container ID of this device */
      maxlen = sizearray(cid_device);
      memset(cid_device, 0, maxlen);
      stat = RegQueryValueEx(hkeyItem, _T("ContainerID"), NULL, NULL, (LPBYTE)cid_device, &maxlen);
      RegCloseKey(hkeyItem);
      if (stat != ERROR_SUCCESS) {
        RegCloseKey(hkeySection);
        return 0;
      }
      if (_tcsicmp(cid_iface, cid_device) == 0) {
        /* if we found the Container IDs match, the subkey is the serial number */
        RegCloseKey(hkeySection);
        _tcsncpy(name, subkey, namelen);
        name[namelen - 1] = '\0';
        return _tcslen(name) > 0;
      }
      idx_device++;
    }
  }

  /* at this point, neither the GDB-server, nor the serial number were requested,
     now open the key to the correct interface, and get a handle to the same
     subkey as the one for GDB-server */
  _stprintf(regpath, _T("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_%04X&PID_%04X&MI_%02X"),
            BMP_VID, BMP_PID, iface);
  if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath, 0, KEY_READ, &hkeySection) != ERROR_SUCCESS)
    return 0;
  ptr = _tcschr(subkey, '\\');
  assert(ptr != NULL);
  *(ptr - 1) = (TCHAR)(iface + '0'); /* interface is encoded in the subkey too */
  if (RegOpenKeyEx(hkeySection, subkey, 0, KEY_READ, &hkeyItem) != ERROR_SUCCESS) {
    RegCloseKey(hkeySection);
    return 0;
  }
  maxlen = sizearray(portname);
  memset(portname, 0, maxlen);
  if (iface == BMP_IF_UART) {
    /* read the port name setting */
    if (RegQueryValueEx(hkeyItem, _T("PortName"), NULL, NULL, (LPBYTE)portname, &maxlen) != ERROR_SUCCESS) {
      RegCloseKey(hkeyItem);
      RegCloseKey(hkeySection);
      return 0;
    }
    if ((ptr = _tcsrchr(portname, _T('\\'))) != NULL)
      ptr += 1;     /* skip '\\.\', if present */
    else
      ptr = portname;
  } else {
    /* read GUID */
    LSTATUS stat = RegQueryValueEx(hkeyItem, _T("DeviceInterfaceGUIDs"), NULL, NULL, (LPBYTE)portname, &maxlen);
    /* ERROR_MORE_DATA is returned because there may technically be more GUIDs
       assigned to the device; we only care about the first one
       ERROR_FILE_NOT_FOUND is returned when the key is not found, which may
       happen on a clone BMP (without SWO trace support) */
    if (stat != ERROR_SUCCESS && stat != ERROR_MORE_DATA && stat != ERROR_FILE_NOT_FOUND) {
      RegCloseKey(hkeyItem);
      RegCloseKey(hkeySection);
      return 0;
    }
    ptr = portname;
  }
  RegCloseKey(hkeyItem);
  RegCloseKey(hkeySection);

  _tcsncpy(name, ptr, namelen);
  name[namelen - 1] = '\0';
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
                 but the requested interface is the UART or the SWO trace
                 interface -> patch the directory name and search again */
              char *ptr = path + strlen(path) - 5;  /* -4 for "/tty", -1 to get to the last character before "/tty" */
              assert(strlen(path) > 5);
              assert(*ptr == '0' && *(ptr-1) == '.' && *(ptr + 1) == '/');
              *ptr = iface + '0';
              *name = '\0'; /* clear device name for GDB-server (we want the name for the UART) */
              if (iface == BMP_IF_UART) {
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
              } else if (iface == BMP_IF_TRACE) {
                ptr = path + strlen(path) - 4;  /* -4 for "/tty" */
                assert(strlen(path) > 4);
                assert(*ptr == '/' && *(ptr - 1) == (iface + '0'));
                *ptr = '\0';  /* remove "/tty" */
                strlcat(path, "/modalias", sizearray(path));
                if (access(path, 0) == 0) {
                  /* file exists, so interface exists */
                  *ptr = '\0';  /* erase "/modalias" again */
                  ptr = path + strlen(SYSFS_ROOT) + 1;  /* skip root */
                  strlcpy(name, ptr, namelen);  /* return <bus> '-' <port> ':' <???> '.' <iface> */
                }
              } else {
                FILE *fpser;
                assert(iface == BMP_IF_SERIAL);
                assert(strlen(path) > 4);
                ptr = path + strlen(path) - 4;  /* -4 for "/tty" */
                assert(*ptr == '/');
                while (ptr>path && *ptr != ':' && *(ptr - 1) != '/')
                  ptr--;
                assert(ptr > path && *ptr == ':');
                *ptr = '\0';  /* remove sub-path */
                strlcat(path, "/serial", sizearray(path));
                fpser = fopen(path, "r");
                if (fpser != NULL) {
                  fgets(name, namelen, fpser);
                  if ((ptr = strchr(name, '\n')) != NULL)
                    *ptr = '\0';  /* drop trailing newline */
                  fclose(fpser);
                }
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

int get_bmp_count(void)
{
  int idx;
  char portname[64];

  for (idx = 0; find_bmp(idx, BMP_IF_GDB, portname, sizearray(portname)); idx++)
    {}
  return idx;
}

int check_versionstring(const char *string)
{
  if (strncmp(string, "Black Magic Probe", 17) == 0) {
    if (strstr(string, "Hardware Version 3") != NULL)
      return PROBE_BMPv21;
    if (strstr(string, "Hardware Version 6") != NULL)
      return PROBE_BMPv23;
  }
  if (strncmp(string, "Wireless Debug Probe", 20) == 0)
    return PROBE_CTXLINK;
  return PROBE_UNKNOWN;
}


/* --------------------------------------------------------------------------
   ctxLink networking code
   -------------------------------------------------------------------------- */


typedef struct tagPORTRANGE {
  const char *base;
  short start, end;
  unsigned long mask;
} PORTRANGE;

#if !(defined WIN32 || defined _WIN32)
  static volatile int running_threads = 0;
  static pthread_mutex_t running_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#if defined WIN32 || defined _WIN32
static DWORD __stdcall scan_range(LPVOID arg)
#else
static void *scan_range(void *arg)
#endif
{
  short idx, start, end;
  const char *base;
  unsigned long mask = 0, bit = 1;

  assert(arg != NULL);
  base = ((PORTRANGE*)arg)->base;
  start = ((PORTRANGE*)arg)->start;
  end = ((PORTRANGE*)arg)->end;
  assert(end-start < 8*sizeof(unsigned long));  /* bit mask with matches should be sufficiently big */

  for (idx = start; idx <= end; idx++) {
    SOCKET sock;
    char addr[20];
    sprintf(addr, "%s%d", base, idx);
    if ((sock = socket(AF_INET , SOCK_STREAM , 0)) == INVALID_SOCKET)
      break;  // could not create socket, check WSAGetLastError()
    if (connect_timeout(sock, addr, BMP_PORT_GDB, 250) >= 0)
      mask |= bit;
    bit <<= 1;
    closesocket(sock);
  }

  ((PORTRANGE*)arg)->mask = mask;

  #if !(defined WIN32 || defined _WIN32)
    pthread_mutex_lock(&running_mutex);
    running_threads--;
    pthread_mutex_unlock(&running_mutex);
  #endif
  return 0;
}

int scan_network(unsigned long *addresses, int address_count)
{
  #define NUM_THREADS 32
  PORTRANGE pr[NUM_THREADS];
  #if defined WIN32 || defined _WIN32
    HANDLE hThread[NUM_THREADS];
  #else
    pthread_t hThread[NUM_THREADS];
  #endif
  char local_ip[30], *ptr;
  unsigned long local_ip_addr;
  int idx, range, count;

  /* check local IP address, replace the last number by a wildcard (i.e., assume
     that the netmask is 255.255.255.0) */
  local_ip_addr = getlocalip(local_ip);
  if ((ptr = strrchr(local_ip, '.')) != NULL)
    *(ptr + 1) = '\0';

  range = (254-1+NUM_THREADS/2) / NUM_THREADS;
  for (idx=0; idx<NUM_THREADS; idx++) {
    pr[idx].base = local_ip;
    pr[idx].start = (short)(1 + (idx*range));
    pr[idx].end = (short)(1 + (idx*range) + (range-1));
  }
  pr[NUM_THREADS-1].end = 254;

  /* create threads to scan the network and wait for all these threads to
     finish */
  #if defined WIN32 || defined _WIN32
    for (idx=0; idx<NUM_THREADS; idx++)
      hThread[idx] = CreateThread(NULL, 0, scan_range, &pr[idx], 0, NULL);
    WaitForMultipleObjects(NUM_THREADS, hThread, TRUE, INFINITE);
  #else
    for (idx=0; idx<NUM_THREADS; idx++) {
      if (pthread_create(&hThread[idx], NULL, scan_range, NULL) == 0) {
        pthread_mutex_lock(&running_mutex);
        running_threads++;
        pthread_mutex_unlock(&running_mutex);
      }
    }
    while (running_threads > 0)
      usleep(50000);
  #endif

  /* construct the list of matching addresses */
  assert(addresses != NULL && address_count > 0);
  count = 0;
  for (idx=0; idx<NUM_THREADS; idx++) {
    unsigned long bit = 1;
    short j;
    for (j = pr[idx].start; j <= pr[idx].end; j++) {
      if (count < address_count && (pr[idx].mask & bit) != 0)
        addresses[count++] = (local_ip_addr & 0xffffff) | (j << 24);
      bit <<= 1;
    }
  }

  return count;
}

