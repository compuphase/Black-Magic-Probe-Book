/*
 * Shared code for SWO Trace for the bmtrace and bmdebug utilities.
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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined WIN32 || defined _WIN32
  #define STRICT
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <tchar.h>
  #include <initguid.h>
  #include <setupapi.h>
  #include <winusb.h>
  #include <malloc.h>
  #if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
    #include "strlcpy.h"
  #endif
  #if defined _MSC_VER
    #define memicmp(p1,p2,c)  _memicmp((p1),(p2),(c))
  #endif
#elif defined __linux__
  #include <alloca.h>
  #include <pthread.h>
  #include <unistd.h>
  #include <bsd/string.h>
  #include <sys/stat.h>
  #include <libusb-1.0/libusb.h>
#endif

#include "bmscan.h"
#include "guidriver.h"
#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"


#ifndef NK_ASSERT
  #include <assert.h>
  #define NK_ASSERT(expr) assert(expr)
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  #define stricmp(s1,s2)    strcasecmp((s1),(s2))
  static int memicmp(const unsigned char *p1, const unsigned char *p2, size_t count);
#endif

#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif


#define CHANNEL_NAMELENGTH  30
typedef struct tagCHANNELINFO {
  int enabled;
  struct nk_color color;
  char name[CHANNEL_NAMELENGTH];
} CHANNELINFO;
static CHANNELINFO channels[NUM_CHANNELS];

void channel_set(int index, int enabled, const char *name, struct nk_color color)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  channels[index].enabled = enabled;
  channels[index].color = color;
  if (name == NULL)
    sprintf(channels[index].name, "%d", index);
  else
    strlcpy(channels[index].name, name, sizearray(channels[index].name));
}

int channel_getenabled(int index)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  return channels[index].enabled;
}

void channel_setenabled(int index, int enabled)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  channels[index].enabled = enabled;
}

/** channel_getname() returns the name of the channel and optionally copies
 *  it into the parameter.
 *  \param index    The channel index, 0 to 32
 *  \param name     The channel name is copied into this parameter, unless
 *                  this parameter is NULL.
 *  \param size     The buffer size reserved for parameter name.
 *  \return A pointer to the name in the channel structure. If parameter "name"
 *          is NULL, one can use this pointer. If parameter "name" is not NULL,
 *          note that the returned pointer to the name in the channel structure
 *          (so, not to the "name" parameter).
 */
const char *channel_getname(int index, char *name, size_t size)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  if (name != NULL && size > 0)
    strlcpy(name, channels[index].name, size);
  return channels[index].name;
}

void channel_setname(int index, const char *name)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  if (name == NULL)
    sprintf(channels[index].name, "%d", index);
  else
    strlcpy(channels[index].name, name, sizearray(channels[index].name));
}

struct nk_color channel_getcolor(int index)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  return channels[index].color;
}

void channel_setcolor(int index, struct nk_color color)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  channels[index].color = color;
}


#define PACKET_SIZE 64
#define PACKET_NUM  32
typedef struct tagPACKET {
  unsigned char data[PACKET_SIZE];
  size_t length;
  double timestamp;
} PACKET;
static PACKET trace_queue[PACKET_NUM];
static int tracequeue_head = 0, tracequeue_tail = 0;


typedef struct tagTRACESTRING {
  struct tagTRACESTRING *next;
  char *text;
  double timestamp;       /* in seconds */
  char timefmt[15];       /* formatted string with time stamp */
  unsigned short timefmt_len;
  unsigned short length, size;  /* text length & text buffer size (length <= size) */
  unsigned char channel;
  unsigned char flags;    /* used to keep state while decoding plain trace messages */
} TRACESTRING;

#define TRACESTRING_MAXLENGTH 256
#define TRACESTRING_INITSIZE  32
static TRACESTRING tracestring_root = { NULL, NULL };
static TRACESTRING *tracestring_tail = NULL;
static int trace_decodectf = 0;

void tracestring_add(const unsigned char *buffer, size_t length, double timestamp)
{
  unsigned idx, chan;

  NK_ASSERT(buffer != NULL);
  NK_ASSERT(length > 0);
  NK_ASSERT((length & 1) == 0);

  if (trace_decodectf) {
    /* CTF mode */
    unsigned char *bytestream = alloca(length);
    int pos;
    idx = 0;
    while (idx < length) {
      if ((buffer[idx] & 0x07) != 0x01) {
        ctf_decode_reset();
        idx += 2;
        continue; /* this is not an ITM packet */
      }
      chan = (buffer[idx] >> 3) & 0x1f;
      /* collect packets from the same channel in a separate buffer */
      pos = 0;
      bytestream[pos++] = buffer[idx + 1];
      idx += 2;
      while (idx < length && ((buffer[idx] >> 3) & 0x1f) == chan) {
        bytestream[pos++] = buffer[idx + 1];
        idx += 2;
      }
      /* check whether the channel is enabled (in passive mode, the target that
         sends the trace messages is oblivious of the settings in this viewer,
         so it may send trace messages for disabled channels) */
      if (channels[chan].enabled) {
        int count = ctf_decode(bytestream, pos, chan);
        if (count > 0) {
          uint16_t streamid;
          double tstamp;
          const char *message;
          while (msgstack_peek(&streamid, &tstamp, &message)) {
            TRACESTRING *item = malloc(sizeof(TRACESTRING));
            if (item != NULL) {
              memset(item, 0, sizeof(TRACESTRING));
              item->length = (unsigned short)strlen(message);
              item->size = item->length + 1;
              item->text = malloc(item->size * sizeof(unsigned char));
              if (item->text != NULL) {
                strcpy(item->text, message);
                item->length = item->size - 1;
                item->channel = (unsigned char)streamid;
                if (tstamp > 0.001)
                  timestamp = tstamp; /* use precision timestamp from remote host */
                item->timestamp = timestamp;
                if (tracestring_root.next != NULL)
                  timestamp -= tracestring_root.next->timestamp;
                else
                  timestamp = 0.0;
                /* create formatted timestamp */
                if (tstamp > 0.001)
                  sprintf(item->timefmt, "%.6f", timestamp);
                else
                  sprintf(item->timefmt, "%.3f", timestamp);
                item->timefmt_len = (unsigned short)strlen(item->timefmt);
                assert(item->timefmt_len < sizearray(item->timefmt));
                /* append to tail */
                if (tracestring_tail != NULL)
                  tracestring_tail->next = item;
                else
                  tracestring_root.next = item;
                tracestring_tail = item;
              }
            }
            msgstack_pop(NULL, NULL, NULL, 0);
          }
        }
      }
    }
  } else {
    /* plain text mode */
    for (idx = 0; idx < length; idx += 2) {
      if ((buffer[idx] & 0x07) != 0x01)
        continue; /* this is not an ITM packet */
      chan = (buffer[idx] >> 3) & 0x1f;
      /* check whether the channel is enabled (in passive mode, the target that
         sends the trace messages is oblivious of the settings in this viewer,
         so it may send trace messages for disabled channels) */
      if (!channels[chan].enabled)
        continue;

      /* see whether to append to the recent string, or to add a new string */
      if (tracestring_tail != NULL) {
        if (buffer[idx + 1] == '\r' || buffer[idx + 1] == '\n') {
          tracestring_tail->flags |= 0x01;  /* on newline, create a new string */
          continue;
        } else if (tracestring_tail->channel != chan) {
          tracestring_tail->flags |= 0x01;  /* different channel, terminate previous string */
        } else if (tracestring_tail->length >= TRACESTRING_MAXLENGTH) {
          tracestring_tail->flags |= 0x01;  /* line length limit */
        }
        /* time criterion: there should not be more that 0.1 seconds between
           parts of a continued string */
        if (tracestring_tail != NULL && timestamp - tracestring_tail->timestamp > 0.1)
          tracestring_tail->flags |= 0x01;  /* interval limit */
      }

      if (tracestring_tail != NULL && (tracestring_tail->flags & 0x01) == 0) {
        /* append text to the current string */
        if (tracestring_tail->length >= tracestring_tail->size) {
          int newsize = tracestring_tail->size * 2;
          char *ptr = malloc(newsize * sizeof(unsigned char));
          if (ptr != NULL) {
            memcpy(ptr, tracestring_tail->text, tracestring_tail->length);
            free((void*)tracestring_tail->text);
            tracestring_tail->text = ptr;
            tracestring_tail->size = (unsigned short)newsize;
          }
        }
        if (tracestring_tail->length < tracestring_tail->size)
          tracestring_tail->text[tracestring_tail->length++] = buffer[idx + 1];
      } else {
        /* create a new string */
        TRACESTRING *item;
        if (tracestring_tail == NULL && (buffer[idx + 1] == '\r' || buffer[idx + 1] == '\n'))
          continue; /* don't create an empty first string */
        item = malloc(sizeof(TRACESTRING));
        if (item != NULL) {
          memset(item, 0, sizeof(TRACESTRING));
          item->size = TRACESTRING_INITSIZE;
          item->text = malloc(item->size * sizeof(unsigned char));
          if (item->text != NULL) {
            item->channel = (unsigned char)chan;
            item->timestamp = timestamp;
            if (tracestring_root.next != NULL)
              timestamp -= tracestring_root.next->timestamp;
            else
              timestamp = 0.0;
            /* create formatted timestamp */
            sprintf(item->timefmt, "%.3f", timestamp);
            item->timefmt_len = (unsigned short)strlen(item->timefmt);
            assert(item->timefmt_len < sizearray(item->timefmt));
            /* append to tail */
            if (tracestring_tail != NULL)
              tracestring_tail->next = item;
            else
              tracestring_root.next = item;
            tracestring_tail = item;
            tracestring_tail->text[tracestring_tail->length++] = buffer[idx + 1];
          } else {
            free(item); /* adding a new string failed */
          }
        }
      }
    }
  }
}

void tracestring_clear(void)
{
  TRACESTRING *item;
  while (tracestring_root.next != NULL) {
    item = tracestring_root.next;
    tracestring_root.next = item->next;
    assert(item->text!=NULL);
    free((void*)item->text);
    free((void*)item);
  }
  tracestring_tail = NULL;
}

int tracestring_isempty(void)
{
  return (tracestring_root.next == NULL);
}

void tracestring_process(int enabled)
{
  while (tracequeue_head != tracequeue_tail) {
    if (enabled)
      tracestring_add(trace_queue[tracequeue_head].data, trace_queue[tracequeue_head].length,
                      trace_queue[tracequeue_head].timestamp);
    tracequeue_head = (tracequeue_head + 1) % PACKET_NUM;
  }
}

int tracestring_find(const char *text, int curline)
{
  TRACESTRING *item;
  int line, cur_mark, len;

  assert(curline >= 0 || curline == -1);
  assert(text != NULL);
  len = strlen(text);

  cur_mark = curline + 1;
  item = tracestring_root.next;
  line = 0;
  while (item != NULL && line < cur_mark) {
    line++;
    item = item->next;
  }
  if (item == NULL || curline < 0) {
    item = tracestring_root.next;
    line = 0;
  } else {
    item = item->next;
    line++;
  }
  while ((line != cur_mark || curline < 0) && item != NULL) {
    int idx;
    curline = cur_mark;
    idx = 0;
    while (idx < item->length) {
      while (idx < item->length && toupper(item->text[idx]) != toupper(text[0]))
        idx++;
      if (idx + len > item->length)
        break;      /* not found on this line */
      if (memicmp((const unsigned char*)item->text + idx, (const unsigned char*)text, len) == 0)
        break;      /* found on this line */
      idx++;
    }
    if (idx + len <= item->length)
      return line;  /* found, stop search */
    item = item->next;
    line++;
    if (item == NULL) {
      item = tracestring_root.next;
      line = 0;
    }
  } /* while (line != cur_mark) */

  return -1;  /* not found */
}

/** tracestring_findtimestamp() finds the line closest to the given
 *  timestamp. Note that it can return -1 when there are not strings in the
 *  list.
 */
int tracestring_findtimestamp(double timestamp)
{
  TRACESTRING *item;
  int line = 0;
  for (item = tracestring_root.next; item != NULL && item->timestamp < timestamp; item = item->next)
    line += 1;
  return line - 1;
}

int trace_save(const char *filename)
{
  FILE *fp;
  TRACESTRING *item;
  char *buffer;
  size_t bufsize;

  fp = fopen(filename, "wt");
  if (fp == NULL)
    return 0;

  bufsize = 0;
  for (item = tracestring_root.next; item != NULL; item = item->next)
    if (item->length > bufsize)
      bufsize = item->length;

  buffer = malloc((bufsize + 1) * sizeof(char));
  if (buffer == NULL) {
    fclose(fp);
    return 0;
  }

  fprintf(fp, "Number,Name,Timestamp,Text\n");
  for (item = tracestring_root.next; item != NULL; item = item->next) {
    memcpy(buffer, item->text, item->length);
    buffer[item->length] = '\0';
    fprintf(fp, "%d,\"%s\",%.6f,\"%s\"\n", item->channel, channels[item->channel].name,
            item->timestamp, buffer);
  }

  free((void*)buffer);
  fclose(fp);
  return 1;
}

/** trace_enablectf() sets or queries the CTF decoding mode. A TSDL file must
 *  have been parsed for the mode to become active. Set parameter "enable" to
 *  -1 to query the current mode (without changing it).
 */
int trace_enablectf(int enable)
{
  int curval = trace_decodectf;
  if (enable == 0 || enable == 1) {
    if (enable && event_count() == 0)
      enable = 0;
    trace_decodectf = enable;
  }
  return curval;
}


#if defined WIN32 || defined _WIN32

static BOOL MakeGUID(const char *label, GUID *guid)
{
  unsigned i;
  char b[5];

  /* check whether the string has a valid format &*/
  if (strlen(label) != 38)
    return FALSE;
  for (i=0; i<strlen(label); i++) {
    char c = label[i];
    if (i == 0) {
      if (c != '{')
        return FALSE;
    } else if (i == 37) {
      if (c != '}')
        return FALSE;
    } else if (i == 9 || i == 14 || i == 19 || i == 24) {
      if (c != '-')
        return FALSE;
    } else {
      if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F') && !(c >= 'a' && c <= 'f'))
        return FALSE;
    }
  }

  guid->Data1 = strtoul(label+1, NULL, 16);
  guid->Data2 = (unsigned short)strtoul(label+10, NULL, 16);
  guid->Data3 = (unsigned short)strtoul(label+15, NULL, 16);
  memset(b, 0, sizeof b);
  for (i = 0; i < 2; i++) {
    memcpy(&b[0], label+20+i*2, 2*sizeof(b[0]));
    guid->Data4[i] = (unsigned char)strtoul(&b[0], NULL, 16);
  }
  for (i = 0; i < 6; i++) {
    memcpy(&b[0], label+25+i*2, 2*sizeof(b[0]));
    guid->Data4[2+i] = (unsigned char)strtoul(&b[0], NULL, 16);
  }

  return TRUE;
}

static BOOL usb_GetDevicePath(const TCHAR *guid, TCHAR *path, size_t pathsize)
{
  GUID ClsId;
  HDEVINFO hDevInfo;
  DWORD dwSize;
  BOOL result = FALSE;

  /* convert the string to a GUID */
  MakeGUID(guid, &ClsId);

  /* get the device information set for all USB devices that have a device
     interface and are currently present on the system (plugged in). */
  hDevInfo = SetupDiGetClassDevs(&ClsId, NULL, 0, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
  if (hDevInfo != INVALID_HANDLE_VALUE) {
    SP_DEVICE_INTERFACE_DATA DevIntfData;
    /* keep calling SetupDiEnumDeviceInterfaces(..) until it fails with code
       ERROR_NO_MORE_ITEMS (with call the dwMemberIdx value needs to be
       incremented to retrieve the next device interface information */
    DevIntfData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    result = SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &ClsId, 0, &DevIntfData);

    if (result) {
      SP_DEVINFO_DATA DevData;
      PSP_DEVICE_INTERFACE_DETAIL_DATA DevIntfDetailData;
      /* get more details for each of the interfaces, including the device path
         (which contains the device's VID/PID */
      DevData.cbSize = sizeof(DevData);

      /* get the required buffer size, then allocate the memory for it and
         initialize the cbSize field */
      SetupDiGetDeviceInterfaceDetail(hDevInfo, &DevIntfData, NULL, 0,&dwSize, NULL);
      DevIntfDetailData = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize);
      #if defined _UNICODE
        DevIntfDetailData->cbSize = sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);
      #elif defined _WIN64
        DevIntfDetailData->cbSize = 8; // sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);
      #else
        DevIntfDetailData->cbSize = 5; // sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);
      #endif

      if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &DevIntfData, DevIntfDetailData, dwSize,&dwSize,&DevData)) {
        NK_ASSERT(path!=NULL);
        NK_ASSERT(pathsize>0);
        #if defined _UNICODE
          memset(path, 0, pathsize*sizeof(TCHAR));
          _tcsncpy(path, (TCHAR*)DevIntfDetailData->DevicePath, pathsize-1);
        #else
          strlcpy(path, DevIntfDetailData->DevicePath, pathsize);
        #endif
      } else {
        result = FALSE;
      }

      HeapFree(GetProcessHeap(), 0, DevIntfDetailData);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
  }

  return result;
}

static HANDLE usb_OpenDevice(const TCHAR *path)
{
  BOOL result;
  HANDLE hDev;
  WINUSB_INTERFACE_HANDLE hUSB;

  hDev = CreateFile(path, GENERIC_WRITE | GENERIC_READ,
                    FILE_SHARE_WRITE | FILE_SHARE_READ,
                    NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
  if (hDev == INVALID_HANDLE_VALUE)
    return INVALID_HANDLE_VALUE;

  result = WinUsb_Initialize(hDev, &hUSB);
  if (!result) {
    CloseHandle(hDev);
    return INVALID_HANDLE_VALUE;
  }

  return hUSB;
}

/* endpoint: use 0x85 for input endpoint #5
 */
static BOOL usb_ConfigEndpoint(WINUSB_INTERFACE_HANDLE hUSB, unsigned char endpoint)
{
  BOOL result;
  USB_INTERFACE_DESCRIPTOR ifaceDescriptor;

  NK_ASSERT(hUSB != INVALID_HANDLE_VALUE);

  if (WinUsb_QueryInterfaceSettings(hUSB, 0, &ifaceDescriptor)) {
    WINUSB_PIPE_INFORMATION pipeInfo;
    int idx;
    for (idx=0; idx<ifaceDescriptor.bNumEndpoints; idx++) {
      memset(&pipeInfo, 0, sizeof(pipeInfo));
      result = WinUsb_QueryPipe(hUSB, 0, (unsigned char)idx, &pipeInfo);
      if (result && pipeInfo.PipeId == endpoint)
        return TRUE;
    }
  }
  return FALSE;
}

static HANDLE hThread = NULL;
static WINUSB_INTERFACE_HANDLE hUSB = INVALID_HANDLE_VALUE;
static LARGE_INTEGER pcfreq;

/** get_timestamp() return precision timestamp in seconds.
 */
static double get_timestamp(void)
{
  LARGE_INTEGER t;

  if (pcfreq.LowPart == 0 && pcfreq.HighPart == 0)
    QueryPerformanceFrequency(&pcfreq);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / (double)pcfreq.QuadPart;
}

static DWORD __stdcall trace_read(LPVOID arg)
{
  unsigned char buffer[PACKET_SIZE];
  unsigned long numread = 0;

  (void)arg;
  for ( ;; ) {
    if (WinUsb_ReadPipe(hUSB, BMP_EP_TRACE, buffer, sizearray(buffer), &numread, NULL)) {
      /* add the packet to the queue */
      int next = (tracequeue_tail + 1) % PACKET_NUM;
      if (numread > 0 && next != tracequeue_head) {
        memcpy(trace_queue[tracequeue_tail].data, buffer, numread);
        trace_queue[tracequeue_tail].length = numread;
        trace_queue[tracequeue_tail].timestamp = get_timestamp();
        tracequeue_tail = next;
        PostMessage((HWND)guidriver_apphandle(), WM_USER, 0, 0L); /* just a flag to wake up the GUI */
      }
    } else {
      Sleep(100);
    }
  }
  return 0;
}

int trace_init(void)
{
  TCHAR guid[100], path[_MAX_PATH];

  if (hThread != NULL && hUSB != INVALID_HANDLE_VALUE)
    return TRACESTAT_OK;            /* double initialization */

  if (!find_bmp(0, BMP_IF_TRACE, guid, sizearray(guid)))
    return TRACESTAT_NO_INTERFACE;  /* Black Magic Probe not found (trace interface not found) */
  if (!usb_GetDevicePath(guid, path, sizearray(path)))
    return TRACESTAT_NO_DEVPATH;    /* device path to trace interface not found (should not occur) */

  hUSB = usb_OpenDevice(path);
  if (hUSB == INVALID_HANDLE_VALUE)
    return TRACESTAT_NO_ACCESS;     /* failure opening the device interface */
  if (!usb_ConfigEndpoint(hUSB, BMP_EP_TRACE))
    return TRACESTAT_NO_PIPE;       /* endpoint pipe could not be found -> not a Black Magic Probe? */

  hThread = CreateThread(NULL, 0, trace_read, NULL, 0, NULL);
  if (hThread == NULL)
    return TRACESTAT_NO_THREAD;
  SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);

  return TRACESTAT_OK;
}

void trace_close(void)
{
  if (hThread != NULL) {
    TerminateThread(hThread, 0);
    hThread = NULL;
  }
  if (hUSB != INVALID_HANDLE_VALUE) {
    WinUsb_Free(hUSB);
    hUSB = INVALID_HANDLE_VALUE;
  }
}

#else

static pthread_t hThread;
static libusb_device_handle *hUSB;

static int memicmp(const unsigned char *p1, const unsigned char *p2, size_t count)
{
  int diff = 0;
  while (count-- > 0 && diff == 0)
    diff = toupper(*p1++) - toupper(*p2++);
  return diff;
}

static double timestamp(void)
{
  struct timeval tv;
  gettimeofday(&tv,NULL);
  return 1000000.0 * tv.tv_sec + tv.tv_usec;
}

static void *trace_read(void *arg)
{
  unsigned char buffer[PACKET_SIZE];
  int numread = 0;

  (void)arg;
  for ( ;; ) {
    if (libusb_bulk_transfer(hUSB, BMP_EP_TRACE, buffer, sizeof(buffer), &numread, 0) == 0) {
      /* add the packet to the queue */
      int next = (tracequeue_tail + 1) % PACKET_NUM;
      if (numread > 0 && next != tracequeue_head) {
        memcpy(trace_queue[tracequeue_tail].data, buffer, numread);
        trace_queue[tracequeue_tail].length = numread;
        trace_queue[tracequeue_tail].timestamp = timestamp();
        tracequeue_tail = next;
      }
    }
  }
  return 0;
}

static int usb_OpenDevice(libusb_device_handle **hUSB)
{
  libusb_device **devs;
  libusb_device_handle *handle;
  ssize_t cnt;
  int devidx, i, res;

  /* get list of all devices */
  cnt = libusb_get_device_list(0, &devs);
  if (cnt < 0)
    return TRACESTAT_INIT_FAILED;

  /* find the BMP */
  devidx = -1;
  for (i = 0; devs[i] != NULL; i++) {
    struct libusb_device_descriptor desc;
    res = libusb_get_device_descriptor(devs[i], &desc);
    if (res >= 0 && desc.idVendor == BMP_VID && desc.idProduct == BMP_PID) {
      devidx = i;
      break;
    }
  }
  if (devidx < 0) {
    libusb_free_device_list(devs, 1);
    return TRACESTAT_NO_DEVPATH;
  }

  res = libusb_open(devs[devidx], &handle);
  libusb_free_device_list(devs, 1);
  if (res < 0)
    return TRACESTAT_NO_ACCESS;

  /* connect to the BMP capture interface */
  res = libusb_claim_interface(handle, BMP_IF_TRACE);
  if (res < 0) {
    libusb_close(handle);
    return TRACESTAT_NO_INTERFACE;
  }

  assert(hUSB != NULL);
  *hUSB = handle;
  return TRACESTAT_OK;
}

int trace_init(void)
{
  int result;

  hUSB = NULL;
  hThread = 0;

  result = libusb_init(0);
  if (result < 0)
    return TRACESTAT_INIT_FAILED;

  result = usb_OpenDevice(&hUSB);
  if (result != TRACESTAT_OK)
    return result;

  result = pthread_create(&hThread, NULL, trace_read, NULL);
  if (result != 0)
    return TRACESTAT_NO_THREAD;

  return TRACESTAT_OK;
}

void trace_close(void)
{
  if (hThread != 0) {
    pthread_cancel(hThread);
    hThread = 0;
  }
  if (hUSB != NULL) {
    libusb_close(hUSB);
    hUSB = NULL;
  }
}

#endif

static int recent_statuscode = 0;
static char recent_statusmsg[200] = "";
static char recent_ctf_msg[200] = "";

void tracelog_statusmsg(int type, const char *msg, int code)
{
  assert(type == TRACESTATMSG_BMP || type == TRACESTATMSG_CTF);
  if (msg == NULL)
    msg = "";
  if (type == TRACESTATMSG_BMP) {
    strlcpy(recent_statusmsg, msg, sizearray(recent_statusmsg));
    recent_statuscode = code;
  } else {
    strlcpy(recent_ctf_msg, msg, sizearray(recent_ctf_msg));
  }
}

float tracelog_labelwidth(float rowheight)
{
  int idx;
  float labelwidth = 0;
  for (idx = 0; idx < NUM_CHANNELS; idx++) {
    int len = strlen(channels[idx].name);
    if (channels[idx].enabled && labelwidth < len)
      labelwidth = len;
  }
  return labelwidth * (rowheight / 2);
}

/* tracelog_widget() draws the text in the log window and scrolls to the last line
   if new text was added */
void tracelog_widget(struct nk_context *ctx, const char *id, float rowheight, int markline, nk_flags widget_flags)
{
  static int scrollpos = 0;
  static int linecount = 0;
  static int recent_markline = -1;
  TRACESTRING *item;
  int labelwidth, tstampwidth;
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window *stwin = &ctx->style.window;
  struct nk_style_button stbtn = ctx->style.button;
  struct nk_user_font const *font = ctx->style.font;

  /* preset common parts of the new button style */
  stbtn.border = 0;
  stbtn.rounding = 0;
  stbtn.padding.x = stbtn.padding.y = 0;

  /* check the length of the longest channel name, and the longest timestamp */
  labelwidth = (int)tracelog_labelwidth(rowheight) + 10;
  tstampwidth = 0;
  for (item = tracestring_root.next; item != NULL; item = item->next)
    if (tstampwidth < item->timefmt_len)
      tstampwidth = item->timefmt_len;
  tstampwidth = (int)((tstampwidth * rowheight) / 2) + 10;

  /* black background on group */
  nk_style_push_color(ctx, &stwin->fixed_background.data.color, nk_rgba(20, 29, 38, 225));
  if (nk_group_begin(ctx, id, widget_flags)) {
    int lines = 0, widgetlines = 0, ypos;
    float lineheight = 0;
    for (item = tracestring_root.next; item != NULL; item = item->next) {
      int textwidth;
      struct nk_color clrtxt;
      NK_ASSERT(item->text != NULL);
      nk_layout_row_begin(ctx, NK_STATIC, rowheight, 4);
      if (lineheight <= 0.1) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        lineheight = rcline.h;
      }
      /* marker symbol */
      nk_layout_row_push(ctx, rowheight); /* width is same as height*/
      if (lines == markline) {
        stbtn.normal.data.color = stbtn.hover.data.color
          = stbtn.active.data.color = stbtn.text_background
          = nk_rgb(0, 0, 0);
        stbtn.text_normal = stbtn.text_active = stbtn.text_hover = nk_rgb(255, 255, 128);
        nk_button_symbol_styled(ctx, &stbtn, NK_SYMBOL_TRIANGLE_RIGHT);
      } else {
        nk_spacing(ctx, 1);
      }
      /* channel label */
      NK_ASSERT(item->channel < NUM_CHANNELS);
      stbtn.normal.data.color = stbtn.hover.data.color
        = stbtn.active.data.color = stbtn.text_background
        = channels[item->channel].color;
      if (channels[item->channel].color.r + 2 * channels[item->channel].color.g + channels[item->channel].color.b < 700)
        clrtxt = nk_rgb(255,255,255);
      else
        clrtxt = nk_rgb(20,29,38);
      stbtn.text_normal = stbtn.text_active = stbtn.text_hover = clrtxt;
      nk_layout_row_push(ctx, labelwidth);
      nk_button_label_styled(ctx, &stbtn, channels[item->channel].name);
      /* timestamp (relative time since previous trace) */
      nk_layout_row_push(ctx, tstampwidth);
      nk_label_colored(ctx, item->timefmt, NK_TEXT_RIGHT, nk_rgb(255, 255, 128));
      /* calculate size of the text */
      NK_ASSERT(font != NULL && font->width != NULL);
      textwidth = font->width(font->userdata, font->height, item->text, item->length) + 10;
      nk_layout_row_push(ctx, textwidth);
      if (lines == markline)
        nk_text_colored(ctx, item->text, item->length, NK_TEXT_LEFT, nk_rgb(255, 255, 128));
      else
        nk_text(ctx, item->text, item->length, NK_TEXT_LEFT);
      nk_layout_row_end(ctx);
      lines++;
    }
    nk_layout_row_dynamic(ctx, rowheight, 1);
    if (lines == 0) {
      if (strlen(recent_statusmsg) > 0 && recent_statuscode != 0) {
        struct nk_color clr = (recent_statuscode >= 0) ? nk_rgb(100, 255, 100) : nk_rgb(255, 100, 128);
        nk_label_colored(ctx, recent_statusmsg, NK_TEXT_LEFT, clr);
        lines++;
      }
      if (recent_ctf_msg[0] != '\0') {
        if (lines == 1)
          nk_layout_row_dynamic(ctx, rowheight, 1);
        nk_label_colored(ctx, recent_ctf_msg, NK_TEXT_LEFT, nk_rgb(255, 100, 128));
        lines++;
      }
    } else {
      nk_spacing(ctx, 1);
    }
    nk_group_end(ctx);
    /* calculate scrolling
       1) if number of lines change, scroll to the last line
       2) if line to mark is different than last time (and valid) make that
          line visible */
    ypos = scrollpos;
    widgetlines = (int)((rcwidget.h - 2 * stwin->padding.y) / lineheight);
    if (lines != linecount) {
      linecount = lines;
      ypos = (int)((lines - widgetlines + 1) * lineheight);
    } else if (markline != recent_markline) {
      recent_markline = markline;
      if (markline >= 0) {
        ypos = markline - widgetlines / 2;
        if (ypos > lines - widgetlines + 1)
          ypos = lines - widgetlines + 1;
        ypos = (int)(ypos * lineheight);
      }
    }
    if (ypos < 0)
      ypos = 0;
    if (ypos != scrollpos) {
      nk_group_set_scroll(ctx, id, 0, ypos);
      scrollpos = ypos;
    }
  }
  nk_style_pop_color(ctx);
}


typedef struct tagTLMARK {
  float pos;
  int count;
} TLMARK;
typedef struct tagTIMELINE {
  TLMARK *marks;
  size_t length, size;  /* number of entries & max. number of entries */
} TIMELINE;

#define EPSILON     0.001
#define FLTEQ(a,b)  ((a)-EPSILON<(b) && (a)+EPSILON>(b))  /* test whether floating-point values are equal within a small margin */
#define MARK_SECOND   1000000
static double mark_spacing = 100.0;             /* spacing between two mark_deltatime positions */
static unsigned long mark_scale = MARK_SECOND;  /* 1 -> us, 1000 -> ms, 1000000 -> s, 60000000 -> min, etc. */
static unsigned long mark_deltatime = 1;        /* in seconds / mark_scale */
static TRACESTRING *tracestring_tail_prev = NULL;
static TIMELINE timeline[NUM_CHANNELS];
static float timeline_maxpos = 0.0;
static double timeoffset = 0.0;
static int timeline_maxcount = 1;

void timeline_getconfig(double *spacing, unsigned long *scale, unsigned long *delta)
{
  assert(spacing != NULL);
  *spacing = mark_spacing;
  assert(scale != NULL);
  *scale = mark_scale;
  assert(delta != NULL);
  *delta = mark_deltatime;
}

void timeline_setconfig(double spacing, unsigned long scale, unsigned long delta)
{
  if (spacing > 10.0 && scale > 0 && delta > 0 && delta <= 100) {
    mark_spacing = spacing;
    mark_scale = scale;
    mark_deltatime = delta;
  }
}

void timeline_rebuild(void)
{
  timeline_maxpos = 0.0;  /* this variable is recalculated */
  timeoffset = 0.0;
  timeline_maxcount = 1;

  /* marks only get added, until the list is cleared completely */
  if (tracestring_root.next == NULL) {
    int chan;
    for (chan = 0; chan < NUM_CHANNELS; chan++) {
      if (timeline[chan].marks != NULL) {
        free((void*)timeline[chan].marks);
        timeline[chan].marks = NULL;
        timeline[chan].length = timeline[chan].size = 0;
      }
    }
  } else {
    TRACESTRING *item;
    int chan;
    assert(tracestring_root.next != NULL);
    timeoffset = tracestring_root.next->timestamp;
    for (chan = 0; chan < NUM_CHANNELS; chan++)
      timeline[chan].length = 0;
    for (item = tracestring_root.next; item != NULL; item = item->next) {
      int idx;
      float pos;
      chan = item->channel;
      if (!channels[chan].enabled)
        continue;
      /* make sure array is big enough for another mark */
      assert(timeline[chan].length <= timeline[chan].size);
      if (timeline[chan].length == timeline[chan].size) {
        size_t newsize;
        if (timeline[chan].marks == NULL) {
          assert(timeline[chan].size == 0);
          newsize = 32;
          timeline[chan].marks = malloc(newsize * sizeof(TLMARK));
          if (timeline[chan].marks != NULL)
            timeline[chan].size = newsize;
        } else {
          TLMARK *curptr = timeline[chan].marks; /* save, for special case of realloc fail */
          newsize = timeline[chan].size * 2;
          timeline[chan].marks = realloc(timeline[chan].marks, newsize * sizeof(TLMARK));
          if (timeline[chan].marks != NULL)
            timeline[chan].size = newsize;
          else
            timeline[chan].marks = curptr;  /* restore old pointer on realloc fail */
        }
      }
      if (timeline[chan].length == timeline[chan].size)
        continue; /* no space for another mark (growing the array failed) */
      /* convert timestamp to position */
      pos = (item->timestamp - timeoffset) * mark_spacing * MARK_SECOND / (mark_scale * mark_deltatime);
      idx = timeline[chan].length;
      /* check collapsing marks */
      assert(idx == 0 || pos >= timeline[chan].marks[idx - 1].pos);
      if (idx > 0 && (pos - timeline[chan].marks[idx - 1].pos) < 0.5) {
        idx -= 1;
        timeline[chan].marks[idx].count += 1;
        if (timeline[chan].marks[idx].count > timeline_maxcount)
          timeline_maxcount = timeline[chan].marks[idx].count;
      } else {
        timeline[chan].marks[idx].pos = pos;
        timeline[chan].marks[idx].count = 1;
        timeline[chan].length = idx + 1;
      }
      if (pos > timeline_maxpos)
        timeline_maxpos = pos;
    }
  }
}

double timeline_widget(struct nk_context *ctx, const char *id, float rowheight, nk_flags widget_flags)
{
  int labelwidth;
  double click_time = -1.0;
  struct nk_rect rcwidget;
  struct nk_style_button stbtn;

  NK_ASSERT(ctx != NULL);
  NK_ASSERT(ctx->current != NULL);
  NK_ASSERT(ctx->current->layout != NULL);
  if (ctx == NULL || ctx->current == NULL || ctx->current->layout == NULL)
    return click_time;

  if (tracestring_tail != tracestring_tail_prev) {
    timeline_rebuild();         /* rebuild the "trace marks" data */
    tracestring_tail_prev = tracestring_tail;
  }

  /* preset common parts of the new button style */
  stbtn = ctx->style.button;
  // stbtn.border = 0;
  // stbtn.rounding = 0;
  stbtn.padding.x = stbtn.padding.y = 0;

  /* check the length of the longest channel name */
  labelwidth = (int)tracelog_labelwidth(rowheight) + 10;
  rcwidget = nk_layout_widget_bounds(ctx);

  /* no spacing & black background on group */
  nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(0, 0));
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, nk_rgba(20, 29, 38, 225));
  if (nk_group_begin(ctx, id, widget_flags | NK_WINDOW_NO_SCROLLBAR)) {
    struct nk_window *win = ctx->current;
    struct nk_user_font const *font = ctx->style.font;
    struct nk_rect rc;
    long mark_stamp, mark_inv_scale;
    int submark_count, submark_iter;
    int len, chan;
    char valstr[60];
    float x1, x2;
    const char *unit;
    nk_uint xscroll, yscroll;

    submark_count = 10;
    if (mark_spacing / submark_count < 20)
      submark_count = 5;
    if (mark_spacing / submark_count < 20)
      submark_count = 2;

    #define HORPADDING  4
    #define VERPADDING  1

    /* get the scroll position of the graph, because scrolling is manual */
    sprintf(valstr, "%s_graph", id);
    nk_group_get_scroll(ctx, valstr, &xscroll, &yscroll);

    /* first row: timer ticks, may not scroll vertically */
    switch (mark_scale) {
      case 1: unit = "\xC2\xB5s"; break; /* us */
      case 1000: unit = "ms";     break;
      case 1000000: unit = "s";  break;
      case 60000000: unit = "min"; break;
      default: assert(0);
    }
    nk_layout_row_begin(ctx, NK_STATIC, rowheight + VERPADDING, 3);
    nk_layout_row_push(ctx, rcwidget.w - 2 * (1.5 * rowheight));
    rc = nk_layout_widget_bounds(ctx);
    nk_fill_rect(&win->buffer, rc, 0.0f, nk_rgb(35, 52, 71));
    x2 = rc.x + rc.w;
    mark_stamp = submark_iter = 0;
    mark_inv_scale = MARK_SECOND / mark_scale;
    for (x1 = rc.x + labelwidth + HORPADDING - xscroll; x1 < x2; x1 += mark_spacing / submark_count) {
      if (submark_iter == 0) {
        struct nk_color clr;
        if (mark_stamp % mark_inv_scale == 0) {
          sprintf(valstr, "%ld s", mark_stamp / mark_inv_scale);
          clr = nk_rgb(255, 255, 220);
        } else {
          sprintf(valstr, "+%ld %s", mark_stamp, unit);
          clr = nk_rgb(144, 144, 128);
        }
        nk_stroke_line(&win->buffer, x1, rc.y, x1, rc.y + rowheight - 2, 1, clr);
        rc.x = x1 + 2;
        rc.w = x2 - rc.x;
        nk_draw_text(&win->buffer, rc, valstr, strlen(valstr), font, nk_rgb(35, 52, 71), clr);
        mark_stamp += mark_deltatime;
      } else {
        nk_stroke_line(&win->buffer, x1, rc.y, x1, rc.y + rowheight / 2 - 2, 1, nk_rgb(144, 144, 128));
      }
      if (++submark_iter == submark_count)
        submark_iter = 0;
    }
    rc = nk_layout_widget_bounds(ctx);
    nk_stroke_line(&win->buffer, rc.x, rc.y + rc.h, rc.x + rc.w - labelwidth - HORPADDING, rc.y + rc.h, 1, nk_rgb(80, 80, 80));
    rc.w = labelwidth;
    rc.h -= 1;
    nk_fill_rect(&win->buffer, rc, 0.0f, nk_rgb(20, 29, 38));
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, 1.5 * rowheight);
    if (nk_button_symbol_styled(ctx, &stbtn, NK_SYMBOL_PLUS)) {
      mark_spacing *= 1.5;
      if (mark_spacing > 700.0 && (mark_deltatime > 1 || mark_scale > 1)) {
        mark_deltatime /= 10;
        mark_spacing /= 10.0;
        if (mark_deltatime == 0 && mark_scale >= 1000) {
          mark_scale /= 1000;
          mark_deltatime = 100;
        }
      }
      timeline_rebuild();
    }
    nk_layout_row_push(ctx, 1.5 * rowheight);
    if (nk_button_symbol_styled(ctx, &stbtn, NK_SYMBOL_MINUS)) {
      if (mark_spacing > 45.0 || mark_scale < 60000000 || mark_deltatime == 1)
        mark_spacing /= 1.5;
      if (mark_spacing < 70.0) {
        mark_deltatime *= 10;
        mark_spacing *= 10.0;
        if (mark_scale < MARK_SECOND && mark_deltatime >= 1000) {
          mark_scale *= 1000;
          mark_deltatime /= 1000;
        }
      }
      timeline_rebuild();
    }
    nk_layout_row_end(ctx);

    /* extra (small) spacing between timeline and top of the graphs */
    nk_layout_row_dynamic(ctx, VERPADDING, 1);
    nk_spacing(ctx, 1);

    /* remaining rows collected in two groups: labels and graph */
    nk_layout_row_begin(ctx, NK_STATIC, rcwidget.h - rowheight - 2 * VERPADDING, 2);
    nk_layout_row_push(ctx, labelwidth + HORPADDING);
    sprintf(valstr, "%s_label", id);
    if (nk_group_begin(ctx, valstr, NK_WINDOW_NO_SCROLLBAR)) {
      /* labels */
      for (chan = 0; chan < NUM_CHANNELS; chan++) {
        struct nk_color clrtxt;
        float textwidth;
        if (!channels[chan].enabled)
          continue; /* only draw enable channels */
        nk_layout_row_dynamic(ctx, rowheight + VERPADDING, 1);
        rc = nk_layout_widget_bounds(ctx);
        rc.x += HORPADDING;
        rc.y -= yscroll;
        rc.w -= HORPADDING;
        rc.h -= 1;
        nk_fill_rect(&win->buffer, rc, 0.0f, channels[chan].color);
        if (channels[chan].color.r + 2 * channels[chan].color.g + channels[chan].color.b < 700)
          clrtxt = nk_rgb(255,255,255);
        else
          clrtxt = nk_rgb(20,29,38);
        /* center the text in the rect */
        len = strlen(channels[chan].name);
        textwidth = font->width(font->userdata, font->height, channels[chan].name, len);
        rc.x += (rc.w - textwidth) / 2;
        nk_draw_text(&win->buffer, rc, channels[chan].name, len, font, channels[chan].color, clrtxt);
      }
      nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, rcwidget.w - labelwidth - HORPADDING);
    sprintf(valstr, "%s_graph", id);
    if (nk_group_begin(ctx, valstr, 0)) {
      /* graphs */
      int row = 0;
      for (chan = 0; chan < NUM_CHANNELS; chan++) {
        int idx;
        if (!channels[chan].enabled)
          continue; /* only draw enabled channels */
        nk_layout_row_begin(ctx, NK_STATIC, rowheight + VERPADDING, 2);
        nk_layout_row_push(ctx, timeline_maxpos);
        rc = nk_layout_widget_bounds(ctx);
        rc.y -= yscroll;
        if (row & 1)
          nk_fill_rect(&win->buffer, rc, 0.0f, nk_rgb(30, 40, 50));
        row++;
        /* draw marks for each active channel */
        for (idx = 0; idx < timeline[chan].length; idx++) {
          float x = timeline[chan].marks[idx].pos + labelwidth + 2 * HORPADDING - xscroll;
          float y = 0.75 * rowheight * (1 - (float)timeline[chan].marks[idx].count / (float)timeline_maxcount);
          nk_stroke_line(&win->buffer, x, rc.y + y, x, rc.y + rowheight, 1, nk_rgb(144, 144, 128));
        }
        nk_spacing(ctx, 1);
        nk_layout_row_end(ctx);
        /* handle mouse click in timeline, to scroll the trace view */
        if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, rc)) {
          float pos;
          struct nk_mouse *mouse = &ctx->input.mouse;
          NK_ASSERT(mouse != NULL);
          NK_ASSERT(NK_INBOX(mouse->pos.x, mouse->pos.y, rc.x, rc.y, rc.w, rc.h));
          pos = mouse->pos.x - labelwidth - 2 * HORPADDING + xscroll;
          if (pos >= 0.0)
            click_time = pos * (mark_scale * mark_deltatime) / (mark_spacing * MARK_SECOND) + timeoffset;
        }
      }
      nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_group_end(ctx);
  }

  /* restore locally modified styles */
  nk_style_pop_color(ctx);
  nk_style_pop_vec2(ctx);

  return click_time;
}

