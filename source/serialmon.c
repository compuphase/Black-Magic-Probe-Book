/*
 * Simple serial monitor (receive data from a serial port).
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
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined WIN32 || defined _WIN32
  #define STRICT
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #if defined __MINGW32__ || defined __MINGW64__
    #include "strlcpy.h"
  #elif defined _MSC_VER
    #include "strlcpy.h"
    #define access(p,m)       _access((p),(m))
  #endif
#elif defined __linux__
  #include <errno.h>
  #include <pthread.h>
  #include <unistd.h>
  #include <bsd/string.h>
  #include <sys/stat.h>
#endif

#include "bmp-scan.h"
#include "guidriver.h"
#include "rs232.h"
#include "serialmon.h"
#include "parsetsdl.h"
#include "decodectf.h"


#if !defined _MAX_PATH
  #define _MAX_PATH 260
#endif

#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

typedef struct tagSERIALSTRING {
  struct tagSERIALSTRING *next;
  char *text;
  unsigned short length;
  unsigned short flags; /* used to keep state while decoding plain trace messages */
} SERIALSTRING;


#define SERIALSTRING_MAXLENGTH 256
static SERIALSTRING sermon_root = { NULL, NULL };
static SERIALSTRING *sermon_tail = NULL;
static SERIALSTRING *sermon_head = NULL;
static HCOM* hCom;
static char comport[64] = "";
static int baudrate = 0;
static int bmp_seqnr = -1;
static char tdsl_metadata[_MAX_PATH];

#if defined WIN32 || defined _WIN32
  static HANDLE hThread = NULL;
#else
  static pthread_t hThread;
#endif


static void sermon_addstring(const unsigned char *buffer, size_t length)
{
  assert(buffer != NULL);
  assert(length > 0);

  if (tdsl_metadata[0] != '\0') {
    /* CTF mode */
    int count = ctf_decode(buffer, length, 0);
    if (count > 0) {
      const char *message;
      while (msgstack_peek(NULL, NULL, &message)) {
        SERIALSTRING *item = malloc(sizeof(SERIALSTRING));
        if (item != NULL) {
          memset(item, 0, sizeof(SERIALSTRING));
          item->length = (unsigned short)strlen(message);
          item->text = malloc((item->length + 1) * sizeof(unsigned char));
          if (item->text != NULL) {
            strcpy(item->text, message);
            /* append to tail */
            if (sermon_tail != NULL)
              sermon_tail->next = item;
            else
              sermon_root.next = item;
            sermon_tail = item;
          } else {
            free((void*)item);
          }
        }
        msgstack_pop(NULL, NULL, NULL, 0);
      }
    }
  } else {
    /* plain text mode */
    unsigned idx;
    for (idx = 0; idx < length; idx++) {
      unsigned ch = buffer[idx];
      if (ch == '\0')
        ch = '\1';
      /* see whether to append to the recent string, or to add a new string */
      if (sermon_tail != NULL) {
        if (ch == '\r' || ch == '\n') {
          sermon_tail->flags |= 0x01;  /* on newline, create a new string */
          continue;
        } else if (sermon_tail->length >= (SERIALSTRING_MAXLENGTH-1)) {
          sermon_tail->flags |= 0x01;  /* line length limit */
        }
      }

      if (sermon_tail != NULL && (sermon_tail->flags & 0x01) == 0) {
        /* append text to the current string */
        assert(sermon_tail->length < SERIALSTRING_MAXLENGTH);
        sermon_tail->text[sermon_tail->length++] = ch;
      } else {
        /* truncate the buffer to the size needed, then create a new string item */
        SERIALSTRING *item;
        if (sermon_tail == NULL && (ch == '\r' || ch == '\n'))
          continue; /* don't create an empty first string */
        if (sermon_tail != NULL && sermon_tail->length < (SERIALSTRING_MAXLENGTH-1)) {
          sermon_tail->text = realloc(sermon_tail->text, (sermon_tail->length + 1) * sizeof(char));
          assert(sermon_tail->text != NULL);              /* shrinking memory should always succeed */
          sermon_tail->text[sermon_tail->length] = '\0';  /* zero-terminate */
        }
        item = malloc(sizeof(SERIALSTRING));
        if (item != NULL) {
          memset(item, 0, sizeof(SERIALSTRING));
          item->text = malloc(SERIALSTRING_MAXLENGTH * sizeof(unsigned char));
          if (item->text != NULL) {
            /* append to tail */
            if (sermon_tail != NULL)
              sermon_tail->next = item;
            else
              sermon_root.next = item;
            sermon_tail = item;
            sermon_tail->text[sermon_tail->length++] = ch;
          } else {
            free(item); /* adding a new string failed */
          }
        }
      }
    }
    if (sermon_tail != NULL) {
      assert(sermon_tail->length < SERIALSTRING_MAXLENGTH);
      sermon_tail->text[sermon_tail->length] = '\0';  /* also zero-terminate any intermediate result */
    }
  }
}

#if defined WIN32 || defined _WIN32

static DWORD __stdcall sermon_process(LPVOID arg)
{
  unsigned char buffer[256];

  (void)arg;
  while (rs232_isopen(hCom)) {
    size_t count = rs232_recv(hCom, buffer, sizearray(buffer));
    if (count > 0) {
      sermon_addstring(buffer, count);
      PostMessage((HWND)guidriver_apphandle(), WM_USER, 0, 0L); /* just a flag to wake up the GUI */
    } else {
      Sleep(10);
    }
  }
  hThread = NULL;

  return 0;
}

#else

static void *sermon_process(void *arg)
{
  unsigned char buffer[256];

  (void)arg;
  while (rs232_isopen(hCom)) {
    size_t count = rs232_recv(hCom, buffer, sizearray(buffer));
    if (count > 0)
      sermon_addstring(buffer, count);
    else
      usleep(10*1000);
  }
  hThread = 0;

  return 0;
}

#endif

int sermon_open(const char *port, int baud)
{
  char defaultport[64];

  if (hThread) {
    assert(rs232_isopen(hCom));
    return 1;   /* double initialization */
  }

  /* if a previous initialization did not succeed completely, clean up before
     retrying */
  sermon_close();

  if (port == NULL || *port == '\0') {
    /* find secondary port of the Black Magic Probe */
    bmp_seqnr = 0;
    if (!find_bmp(bmp_seqnr, BMP_IF_UART, defaultport, sizearray(defaultport)))
      return 0; /* no port explicitly passed and no secondary port found */
    port = defaultport;
  } else {
    bmp_seqnr = -1; /* port was passed explicitly */
  }
  if (baud <= 0)
    baud = 115200;

  hCom = rs232_open(port, baud, 8, 1, PAR_NONE, FLOWCTRL_NONE);
  if (hCom == NULL)
    return 0;

  #if defined WIN32 || defined _WIN32
    hThread = CreateThread(NULL, 0, sermon_process, NULL, 0, NULL);
    if (hThread == NULL) {
      rs232_close(hCom);
      return 0;
    }
    SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
  #else
    if (pthread_create(&hThread, NULL, sermon_process, NULL) != 0) {
      rs232_close(hCom);
      return 0;
    }
  #endif

  rs232_flush(hCom);
  #if defined WIN32 || defined _WIN32
    Sleep(50);
  #else
    usleep(50*1000);
  #endif
  sermon_clear();

  strcpy(comport, port);
  baudrate = baud;
  return 1;
}

void sermon_close(void)
{
  #if defined WIN32 || defined _WIN32
    if (hThread != NULL) {
      TerminateThread(hThread, 0);
      hThread = NULL;
    }
  #endif

  if (hCom != NULL) {
    rs232_close(hCom);
    hCom = NULL;
  }

  #if !(defined WIN32 || defined _WIN32)
    /* wait until the thread ends running and resets the handle */
    while (hThread != 0)
      usleep(10*1000);
  #endif

  sermon_clear();
}

int sermon_isopen(void)
{
  assert(!hThread || hCom);  /* if the thread is valid, the serial device handle should be too */
  return hThread && hCom;
}

void sermon_clear(void)
{
  SERIALSTRING *item;
  while (sermon_root.next != NULL) {
    item = sermon_root.next;
    sermon_root.next = item->next;
    assert(item->text!=NULL);
    free((void*)item->text);
    free((void*)item);
  }
  sermon_tail = NULL;
}

int sermon_countlines(void)
{
  int count = 0;
  SERIALSTRING *item = sermon_root.next;
  while (item != NULL) {
    count = 1;
    item = item->next;
  }
  return count;
}

void sermon_rewind(void)
{
  sermon_head = &sermon_root;
}

const char *sermon_next(void)
{
  if (sermon_head != NULL)
    sermon_head = sermon_head->next;
  return (sermon_head == NULL) ? NULL : sermon_head->text;
}

const char *sermon_getport(int translated)
{
  return (bmp_seqnr < 0 || translated) ? comport : "";
}

int sermon_getbaud(void)
{
  return baudrate;
}

void sermon_setmetadata(const char *tsdlfile)
{
  tdsl_metadata[0] = '\0';
  if (tsdlfile != NULL && access(tsdlfile, 0) == 0)
    strlcpy(tdsl_metadata, tsdlfile, sizearray(tdsl_metadata));
}

const char *sermon_getmetadata(void)
{
  return tdsl_metadata;
}

int sermon_save(const char *filename)
{
  FILE *fp = fopen(filename, "wt");
  if (fp != NULL) {
    int count = 0;
    SERIALSTRING *item = sermon_root.next;
    while (item != NULL) {
      assert(item->text != NULL);
      fprintf(fp, "%s\n", item->text);
      item = item->next;
      count++;
    }
    fclose(fp);
    return count;
  }

  return -1;
}

