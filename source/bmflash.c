/*
 * Utility to download executable programs to the target micro-controller via
 * the Black Magic Probe on a system. This utility is built with Nuklear for a
 * cross-platform GUI.
 *
 * Copyright 2019-2024 CompuPhase
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

#if defined WIN32 || defined _WIN32
# define STRICT
# define WIN32_LEAN_AND_MEAN
# define _WIN32_WINNT   0x0500 /* for AttachConsole() */
# include <windows.h>
# include <shellapi.h>
# include <direct.h>
# include <io.h>
# include <process.h>	/* for spawn() */
# if defined __MINGW32__ || defined __MINGW64__
#   include <sys/stat.h>
#   include "strlcpy.h"
# elif defined _MSC_VER
#   include <sys/stat.h>
#   include "strlcpy.h"
#   define stat _stat
#   define access(p,m)      _access((p),(m))
#   define mkdir(p)         _mkdir(p)
#   define stricmp(s1,s2)   _stricmp((s1),(s2))
# endif
#elif defined __linux__
# include <unistd.h>
# include <bsd/string.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <sys/wait.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guidriver.h"
#include "nuklear_guide.h"
#include "nuklear_mousepointer.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"
#include "bmcommon.h"
#include "bmp-scan.h"
#include "bmp-script.h"
#include "bmp-support.h"
#include "c11threads.h"
#include "cksum.h"
#include "elf.h"
#include "fileloader.h"
#include "gdb-rsp.h"
#include "ident.h"
#include "mcu-info.h"
#include "minIni.h"
#include "osdialog.h"
#include "rs232.h"
#include "specialfolder.h"
#include "svnrev.h"
#include "tcl.h"
#include "tcpip.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __linux__ || defined __unix__
# include "res/icon_download_64.h"
#endif

#if !defined _MAX_PATH
# define _MAX_PATH 260
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif
#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if defined WIN32 || defined _WIN32
# define DIRSEP_CHAR '\\'
# define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
# define DIRSEP_CHAR '/'
# define IS_OPTION(s)  ((s)[0] == '-')
#endif


#define FONT_HEIGHT     14              /* default font size */
#define WINDOW_WIDTH    (35 * opt_fontsize)
#define WINDOW_HEIGHT   (30 * opt_fontsize)
#define ROW_HEIGHT      (2 * opt_fontsize)
#define COMBOROW_CY     (0.9 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.5 * opt_fontsize)
#define SPACING         2.0f
#define LOGVIEW_HEIGHT  (24 * opt_fontsize + SPACING)

static float opt_fontsize = FONT_HEIGHT;


/* log_addstring() adds a string to the log data; the parameter "text" may be NULL
   to return the current log string without adding new data to it */
static char *logtext = NULL;
static unsigned loglines = 0;
static mtx_t logmutex;
static bool logmtx_init = false;


static char *log_addstring(const char *text)
{
  /* initialize mutex on first call */
  if (!logmtx_init) {
    mtx_init(&logmutex, mtx_plain);
    logmtx_init = true;
  }

  if (text == NULL || strlen(text) == 0)
    return logtext;

  /* in addition to the main thread, the bmp_download and Tcl threads also call
     this function, which is why mutex protection is needed */
  mtx_lock(&logmutex);
  int len = 0;
  char *base = logtext;
  if (base != NULL) {     /* drop lines from the top if the log buffer becomes long */
    len = strlen(base);
    while (len > 2048) {
      while (*base != '\0' && *base != '\n')
        base++;
      if (*base == '\n')
        base++;
      len = strlen(base);
    }
  }
  len += strlen(text) + 1;  /* +1 for the \0 */
  char *buf = malloc(len * sizeof(char));
  if (buf != NULL) {
    *buf = '\0';
    if (base != NULL)
      strcpy(buf, base);
    strcat(buf, text);

    if (logtext != NULL)
      free(logtext);
    logtext = buf;
  }
  mtx_unlock(&logmutex);
  return logtext;
}

/* log_widget() draws the text in the log window, with support for colour codes
   (color codes apply to a full line); if the "scrollpos" parameter is not NULL,
   the window scrolls to the most recent text */
static int log_widget(struct nk_context *ctx, const char *id, const char *content, float rowheight, unsigned *scrollpos)
{
  int lines = 0;
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window *stwin = &ctx->style.window;

  /* black background on group */
  nk_style_push_color(ctx, &stwin->fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, NK_WINDOW_BORDER)) {
    float lineheight = 0;
    const char *head = content;
    while (head != NULL && *head != '\0' && !(*head == '\n' && *(head + 1) == '\0')) {
      const char *tail;
      if ((tail = strchr(head, '\n')) == NULL)
        tail = strchr(head, '\0');
      assert(tail != NULL);
      nk_layout_row_dynamic(ctx, rowheight, 1);
      if (lineheight <= 0.1) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        lineheight = rcline.h;
      }
      if (*head == '^' && isdigit(*(head + 1))) {
        struct nk_color clr = COLOUR_TEXT;
        switch (*(head + 1)) {
        case '1': /* error (red) */
          clr = COLOUR_FG_RED;
          break;
        case '2': /* ok (green) */
          clr = COLOUR_FG_GREEN;
          break;
        case '3': /* warning (yellow) */
          clr = COLOUR_FG_YELLOW;
          break;
        case '4': /* notice (highlighted) */
          clr = COLOUR_HIGHLIGHT;
          break;
        }
        nk_text_colored(ctx, head + 2, (int)(tail - head - 2), NK_TEXT_LEFT, clr);
      } else {
        nk_text(ctx, head, (int)(tail - head), NK_TEXT_LEFT);
      }
      lines++;
      head = (*tail != '\0') ? tail + 1 : tail;
    }
    /* add an empty line to fill up any remaining space below */
    nk_layout_row_dynamic(ctx, rowheight, 1);
    nk_spacing(ctx, 1);
    nk_group_end(ctx);
    if (scrollpos != NULL) {
      /* calculate scrolling */
      int widgetlines = (int)((rcwidget.h - 2 * stwin->padding.y) / lineheight);
      int ypos = (int)((lines - widgetlines + 1) * lineheight);
      if (ypos < 0)
        ypos = 0;
      if ((unsigned)ypos != *scrollpos) {
        nk_group_set_scroll(ctx, id, 0, ypos);
        *scrollpos = ypos;
      }
    }
  }
  nk_style_pop_color(ctx);
  return lines;
}

static int bmp_callback(int code, const char *message)
{
  char fullmsg[200] = "";
  assert(strlen(message) < sizearray(fullmsg) - 4);  /* colour code and \n may be added */
  if (code < 0)
    strcpy(fullmsg, "^1");  /* errors in red */
  else if (code > 0)
    strcpy(fullmsg, "^2");  /* success code in green */
  strcat(fullmsg, message);
  if (strchr(message, '\n') == NULL)
    strcat(fullmsg, "\n");
  log_addstring(fullmsg);

  return code >= 0;
}

static bool patch_vecttable(const char *mcutype)
{
  char msg[100];
  unsigned int chksum;
  int err = filesection_patch_vecttable(mcutype, &chksum);
  switch (err) {
  case FSERR_NONE:
    sprintf(msg, "Checksum adjusted to %08x\n", chksum);
    log_addstring(msg);
    break;
  case FSERR_CHKSUMSET:
    sprintf(msg, "Checksum already correct (%08x)\n", chksum);
    log_addstring(msg);
    break;
  case FSERR_NO_DRIVER:
    log_addstring("^1Unsupported MCU type\n");
    return false;
  case FSERR_NO_VECTTABLE:
    log_addstring("^1Vector table not present\n");
    return false;
  }
  return true;
}

static int serialize_fmtoutput(unsigned char *buffer, int size, int serialnum, int format)
{
  int idx, len;
  char localbuf[50];

  if (size < 1) {
    log_addstring("^1Invalid size for serial number\n");
    return 0;
  }
  if (format == 2 && (size & 1) == 1) {
    log_addstring("^1Unicode string size must be an even number\n");
    return 0;
  }

  assert(buffer != NULL);
  switch (format) {
  case 0: /* binary */
    /* assume Little Endian */
    for (idx = 0; idx < size; idx++) {
      buffer[idx] = serialnum & 0xff;
      serialnum >>= 8;
    }
    break;
  case 1: /* ascii */
    sprintf(localbuf, "%d", serialnum);
    len = strlen(localbuf);
    idx = size - len;
    while (idx > 0) {
      *buffer++ = '0';
      idx--;
    }
    memmove(buffer, localbuf - idx, len + idx); /* if idx < 0, strip off leading digits */
    break;
  case 2: /* unicode */
    sprintf(localbuf, "%d", serialnum);
    len = strlen(localbuf);
    idx = size - 2 * len;
    assert((idx & 1) == 0); /* must be even */
    while (idx > 0) {
      *buffer++ = '0';
      *buffer++ = 0;
      idx -= 2;
    }
    if (idx < 0) {
      idx = -idx / 2;
      len -= idx;
    } else {
      assert(idx == 0);
    }
    while (len > 0) {
      *buffer++ = localbuf[idx];
      *buffer++ = 0;
      idx++;
      len--;
    }
    break;
  }

  return 1;
}

static size_t serialize_parsepattern(unsigned char *output, size_t output_size,
                                     const char *input, const char *description)
{
  size_t buflength;
  int widechars = 0;

  assert(output != NULL);
  assert(output_size >= 2);
  assert(input != NULL);

  buflength = 0;
  while (*input != '\0' && buflength < output_size - 2) {
    if (*input == '\\') {
      if (*(input + 1) == '\\') {
        input++;
        output[buflength] = *input; /* '\\' is replaced by a single '\'*/
      } else if (*(input + 1) == 'x' && isxdigit(*(input + 2))) {
        int val = 0;
        int len = 0;
        input += 2;     /* skip '\x' */
        while (len < 2 && isdigit(*input)) {
          int ch = -1;
          if (*input >= '0' && *input <= '9')
            ch = *input - '0';
          else if (*input >= 'A' && *input <= 'F')
            ch = *input - 'A' + 10;
          else if (*input >= 'a' && *input <= 'f')
            ch = *input - 'a' + 10;
          assert(ch >= 0 && ch <= 15);
          val = (val << 4) | ch;
          input++;
          len++;
        }
      } else if (isdigit(*(input + 1))) {
        int val = 0;
        int len = 0;
        input += 1;     /* skip '\' */
        while (len < 3 && isdigit(*input)) {
          val = 10 * val + (*input - '0');
          input++;
          len++;
        }
        output[buflength] = (unsigned char)val;
      } else if ((*(input + 1) == 'A' || *(input + 1) == 'U') && *(input + 2) == '*') {
        widechars = (*(input + 1) == 'U');
        input += 3;
        continue; /* skip storing some character in output */
      } else {
        /* nothing recognizable follows the '\', take it literally */
        char msg[100];
        sprintf(msg, "^1Invalid syntax for \"%s\" string\n", description);
        log_addstring(msg);
        output[buflength] = *input;
        return ~0;  /* return failure, so do not proceed with match & replace */
      }
    } else {
      output[buflength] = *input;
    }
    if (widechars)
      output[++buflength] = '\0';
    buflength++;
    input++;
  }

  return buflength;
}

static bool serialize_address(const char *filename, const char *section,
                              unsigned long address, unsigned char *data, int datasize)
{
  /* find section, if provided */
  assert(filename != NULL);
  assert(section != NULL);
  if (strlen(section) > 0) {
    int err = ELFERR_NOMATCH;
    unsigned long elf_address = 0;
    FILE *fp = fopen(filename, "rb");
    if (fp != NULL)
      err = elf_section_by_name(fp, section, NULL, &elf_address, NULL);
    if (err == ELFERR_NOMATCH) {
      log_addstring("^1Serialization section not found\n");
      return false;
    }
    address += elf_address;
  }

  /* find file section that the address points in */
  unsigned long base_address, length;
  unsigned char *memblock;
  for (int idx = 0; filesection_getdata(idx, &base_address, &memblock, &length, NULL); idx++) {
    if (base_address <= address && address < base_address + length) {
      unsigned long offset = address - base_address;
      if (offset + datasize > length) {
        log_addstring("^1Serialization address exceeds section\n");
        return false;
      }
      memcpy(memblock + offset, data, datasize);
      return true;
    }
  }

  log_addstring("^1Address out of range\n");
  return false;
}

static bool serialize_match(const char *match, const char *prefix,
                            unsigned char *data, int datasize)
{
  /* create match & prefix buffers from the strings */
  assert(match != NULL);
  unsigned char matchbuf[100];
  size_t matchbuf_len = serialize_parsepattern(matchbuf, sizearray(matchbuf), match, "match");
  assert(prefix != NULL);
  unsigned char prefixbuf[100];
  size_t prefixbuf_len = serialize_parsepattern(prefixbuf, sizearray(prefixbuf), prefix, "prefix");
  if (matchbuf_len == (size_t)~0 || prefixbuf_len == (size_t)~0)
    return false;   /* error message already given */
  if (matchbuf_len == 0) {
    log_addstring("^1Serialization match text is empty\n");
    return false;
  }

  /* find the match buffer in the file (in memory) */
  int matchcount = 0;
  unsigned long length;
  unsigned char *memblock;
  for (int idx = 0; filesection_getdata(idx, NULL, &memblock, &length, NULL); idx++) {
    if (length < matchbuf_len)
      continue;
    for (unsigned offset = 0; offset <= length - matchbuf_len; offset++) {
      if (memblock[offset] == matchbuf[0] && memcmp(memblock + offset, matchbuf, matchbuf_len) == 0) {
        memcpy(memblock + offset, prefixbuf, prefixbuf_len);
        memcpy(memblock + offset + prefixbuf_len, data, datasize);
        matchcount++;
      }
    }
  }

  if (matchcount == 0) {
    log_addstring("^1Match string not found\n");
    return false;
  }
  if (matchcount > 1)
    log_addstring("^3Match string found multiple times\n");

  return true;
}

static const char *skipwhite(const char *str)
{
  assert(str != NULL);
  while (*str <= ' ' && *str != '\0')
    str++;
  return str;
}

static int serial_get(const char *field)
{
  FILE *fp;
  int serial;
  const char *ptr = skipwhite(field);
  if (*ptr == '\0')
    return 1;                           /* no serial number filled in, start at 1 */
  if (isdigit(*ptr))
    return (int)strtol(ptr, NULL, 10);  /* direct value filled in */
  /* separate serial number file */
  fp = fopen(ptr, "rt");
  if (fp == NULL)
    return 1;                           /* no file (yet), start at 1 */
  if (fscanf(fp, "%d", &serial) == 0)
    serial = 1;                         /* file exists, but has no valid value in it */
  fclose(fp);
  return serial;
}

static void serial_increment(char *field, int increment)
{
  int serial = serial_get(field) + increment;
  const char *ptr = skipwhite(field);
  if (*ptr == '\0' || isdigit(*ptr)) {
    sprintf(field, "%d", serial);   /* store updated number in the field */
  } else {
    /* store updated number in the file */
    FILE *fp = fopen(ptr, "r+t");
    if (fp == NULL)
      fp = fopen(ptr, "wt");
    if (fp != NULL) {
      fprintf(fp, "%d", serial);
      fclose(fp);
    }
  }
}

static int writelog(const char *filename, const char *serial)
{
  char txtLogFile[_MAX_PATH];
  FILE *fpLog, *fpElf;
  char substr[128], line[256];
  time_t tstamp;
  struct stat fstat;
  int addheader;

  line[0] = '\0';

  /* current date/time */
  tstamp = time(NULL);
  strftime(substr, sizeof(substr), "%Y-%m-%d %H:%M:%S, ", localtime(&tstamp));
  strlcat(line, substr, sizearray(line));

  /* target file date/time */
  stat(filename, &fstat);
  strftime(substr, sizeof(substr), "%Y-%m-%d %H:%M:%S, ", localtime(&fstat.st_mtime));
  strlcat(line, substr, sizearray(line));

  /* target file size */
  sprintf(substr, "%ld, ", fstat.st_size);
  strlcat(line, substr, sizearray(line));

  /* target file CRC32 */
  fpElf = fopen(filename, "rb");
  if (fpElf != NULL) {
    uint32_t crc = cksum(fpElf);
    sprintf(substr, "%u, ", crc);
  } else {
    /* if the file cannot be opened, neither can its checksum be calculated, nor
       any RCS keywords be read */
    strcpy(substr,"-, ");
  }
  strlcat(line, substr, sizearray(line));

  /* RCS identification string in the target file (only works on ELF files) */
  strcpy(substr,"-, "); /* preset, for the case the "ident" string cannot be read */
  if (fpElf != NULL) {
    char key[32], value[128];
    ident(fpElf, 0, key, sizearray(key), value, sizearray(value));
    if (strlen(key) > 0 && strlen(value) > 0) {
      strlcpy(substr, key, sizearray(substr));
      strlcat(substr, ": ", sizearray(substr));
      strlcat(substr, value, sizearray(substr));
      strlcat(substr, ", ", sizearray(substr));
    }
  }
  strlcat(line, substr, sizearray(line));

  if (fpElf != NULL)
    fclose(fpElf);

  /* serial number (if any) */
  if (serial != NULL && strlen(serial) > 0)
    strlcat(line, serial, sizearray(line));
  else
    strlcat(line, "-", sizearray(line));

  /* write to log (first check whether the file exists, in order to write a
     header if it does not yet exist) */
  assert(filename != NULL);
  strlcpy(txtLogFile, filename, sizearray(txtLogFile));
  strlcat(txtLogFile, ".log", sizearray(txtLogFile));

  addheader = (access(txtLogFile, 0) != 0);

  fpLog = fopen(txtLogFile, "at");
  if (fpLog == NULL)
    return 0;
  if (addheader)
    fprintf(fpLog, "download-time, file-time, file-size, cksum, ident, serial\n");
  fprintf(fpLog, "%s\n", line);
  fclose(fpLog);

  return 1;
}

static void usage(const char *invalid_option)
{
# if defined _WIN32  /* fix console output on Windows */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      freopen("CONOUT$", "wb", stdout);
      freopen("CONOUT$", "wb", stderr);
    }
    printf("\n");
# endif

  if (invalid_option != NULL)
    fprintf(stderr, "Unknown option %s; use -h for help.\n\n", invalid_option);
  else
    printf("BMFlash - Firmware Programming utility for the Black Magic Probe.\n\n");
  printf("Usage: bmflash [options] elf-file\n\n"
         "Options:\n"
         "-f=value  Font size to use (value must be 8 or larger).\n"
         "-h        This help."
         "-v        Show version information.\n");
}

static void version(void)
{
# if defined _WIN32  /* fix console output on Windows */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      freopen("CONOUT$", "wb", stdout);
      freopen("CONOUT$", "wb", stderr);
    }
    printf("\n");
# endif

  printf("BMFlash version %s.\n", SVNREV_STR);
  printf("Copyright 2019-2024 CompuPhase\nLicensed under the Apache License version 2.0\n");
}

#if defined FORTIFY
  void Fortify_OutputFunc(const char *str, int type)
  {
#   if defined _WIN32  /* fix console output on Windows */
      if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "wb", stdout);
        freopen("CONOUT$", "wb", stderr);
      }
#   endif
    printf("Fortify: [%d] %s\n", type, str);
  }
#endif

static bool help_popup(struct nk_context *ctx)
{
# include "bmflash_help.h"

  struct nk_rect rc = nk_window_get_bounds(ctx);
# define MARGIN  10
  rc.x += MARGIN;
  rc.y += MARGIN;
  rc.w -= 2*MARGIN;
  rc.h -= 2*MARGIN;
# undef MARGIN
  return nk_guide(ctx, &rc, opt_fontsize, (const char*)bmflash_help, 1);
}


enum {
  TOOL_OPEN = -1,
  TOOL_CLOSE,
  TOOL_RESCAN,
  TOOL_VERIFY,
  TOOL_OPTIONERASE,
  TOOL_FULLERASE,
  TOOL_BLANKCHECK,
  TOOL_DUMPFLASH,
};

static int tools_popup(struct nk_context *ctx, const struct nk_rect *anchor_button)
{
# define MENUROWHEIGHT (1.5 * opt_fontsize)
# define MARGIN        4
  static int prev_active = TOOL_CLOSE;
  int is_active = TOOL_OPEN;
  struct nk_rect rc;
  float height = 6 * MENUROWHEIGHT + 2 * MARGIN;
  struct nk_style_button stbtn = ctx->style.button;
  struct nk_style_window *stwin = &ctx->style.window;
  struct nk_vec2 item_spacing;

  rc.x = anchor_button->x - MARGIN;
  rc.y = anchor_button->y - height;
  rc.w = anchor_button->w;
  rc.h = height;

  /* change button style, to make it more like a menu item */
  item_spacing = stwin->spacing;
  stwin->spacing.y = 0;
  stbtn.border = 0;
  stbtn.rounding = 0;
  stbtn.padding.y = 0;
  stbtn.text_alignment = NK_TEXT_LEFT;

  /* check whether the mouse was clicked outside this popup (this closes the
     popup), but skip this check at the initial "open" */
  if (prev_active == TOOL_OPEN) {
    for (int i = 0; i < NK_BUTTON_MAX; i++)
      if (nk_input_is_mouse_pressed(&ctx->input, (enum nk_buttons)i)
          && !nk_input_is_mouse_click_in_rect(&ctx->input, (enum nk_buttons)i, rc))
        is_active = TOOL_CLOSE;
  }

  if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Tools", NK_WINDOW_NO_SCROLLBAR, rc)) {
    nk_layout_row_dynamic(ctx, MENUROWHEIGHT, 1);
    if (nk_button_label_styled(ctx, &stbtn, "Re-scan Probe List"))
      is_active = TOOL_RESCAN;
    if (nk_button_label_styled(ctx, &stbtn, "Verify Download"))
      is_active = TOOL_VERIFY;
    if (nk_button_label_styled(ctx, &stbtn, "Erase Option Bytes"))
      is_active = TOOL_OPTIONERASE;
    if (nk_button_label_styled(ctx, &stbtn, "Full Flash Erase"))
      is_active = TOOL_FULLERASE;
    if (nk_button_label_styled(ctx, &stbtn, "Blank Check"))
      is_active = TOOL_BLANKCHECK;
    if (nk_button_label_styled(ctx, &stbtn, "Dump Flash to File"))
      is_active = TOOL_DUMPFLASH;
    if (is_active != TOOL_OPEN)
      nk_popup_close(ctx);
    nk_popup_end(ctx);
  } else {
    is_active = TOOL_CLOSE;
  }
# undef MENUROWHEIGHT
# undef MARGIN
  stwin->spacing = item_spacing;
  prev_active = is_active;
  return is_active;
}


typedef struct tagRSPREPLY {
  struct tagRSPREPLY *next;
  char *text;
} RSPREPLY;

static RSPREPLY rspreply_root = { NULL };
static mtx_t semi_mutex;

static void rspreply_init(void)
{
  mtx_init(&semi_mutex, mtx_plain);
}

static void rspreply_clear(void)
{
  while (rspreply_root.next != NULL) {
    RSPREPLY *head = rspreply_root.next;
    rspreply_root.next = head->next;
    free((void*)head->text);
    free((void*)head);
  }
}

static bool rspreply_push(const char *text)
{
  RSPREPLY *item = malloc(sizeof(RSPREPLY));
  if (item == NULL)
    return false;
  item->next = NULL;
  item->text = strdup(text);
  if (item->text == NULL) {
    free((void*)item);
    return false;
  }

  mtx_lock(&semi_mutex);
  RSPREPLY *tail;
  for (tail = &rspreply_root; tail->next != NULL; tail = tail->next)
    {}
  tail->next = item;
  mtx_unlock(&semi_mutex);
  return true;
}

/* the value returned by rspreply_pop() must be freed (if not NULL) */
static const char *rspreply_pop(void)
{
  char *ptr = NULL;
  mtx_lock(&semi_mutex);
  RSPREPLY *head = rspreply_root.next;
  if (head != NULL) {
    rspreply_root.next = head->next;
    ptr = head->text;
    free((void*)head);
    assert(ptr != NULL);
  }
  mtx_unlock(&semi_mutex);
  return ptr;
}

static bool rspreply_semihosting(char *packet)
{
  assert(packet != NULL);
  if (*packet != 'F')
    return false;
  packet++; /* skip 'F' */
  if (strncmp(packet, "gettimeofday,", 13) == 0) {
    struct {              /* structure copied from Black Magic Probe firmware */
      uint32_t ftv_sec;
      uint64_t ftv_usec;
    } fio_timeval;
    fio_timeval.ftv_sec = time(NULL);
    fio_timeval.ftv_usec = 0;
    unsigned addr;
    sscanf(packet + 13, "%x", &addr);
    char buffer[100];
    sprintf(buffer, "X%08X,%lX:", addr, (unsigned long)sizeof(fio_timeval));
    size_t offs = strlen(buffer);
    memcpy(buffer + offs, &fio_timeval, sizeof(fio_timeval));
    gdbrsp_xmit(buffer, offs + sizeof(fio_timeval));
    gdbrsp_xmit("F0", -1);
  } else if (strncmp(packet, "system,", 7) == 0) {
    unsigned addr, size;
    sscanf(packet + 7, "%x/%x", &addr, &size);
    char *buffer = malloc((2 * size + 1) * sizeof(char));
    if (buffer == NULL)
      return false;
    char cmd[30];
    sprintf(cmd, "m%08X,%X:", addr, size);
    gdbrsp_xmit(cmd, -1);
    size_t len = gdbrsp_recv(buffer, 2 * size, 1000);
    buffer[len] = '\0';
    gdbrsp_hex2array(buffer, (unsigned char*)buffer, 2 * size);
    buffer[size] = '\0';
    sprintf(packet, "%s", buffer);
    free((void*)buffer);
    gdbrsp_xmit("F0", -1);
  } else if (strncmp(packet, "write,", 6) == 0) {
    packet += 6;
    unsigned handle, addr, size;
    sscanf(packet, "%x,%x,%x", &handle, &addr, &size);
    char *buffer = malloc((2 * size + 1) * sizeof(char));
    if (buffer == NULL)
      return false;
    char cmd[30];
    sprintf(cmd, "m%08X,%X:", addr, size);
    gdbrsp_xmit(cmd, -1);
    size_t len = gdbrsp_recv(buffer, 2 * size, 1000);
    buffer[len] = '\0';
    gdbrsp_hex2array(buffer, (unsigned char*)buffer, 2 * size);
    buffer[size] = '\0';
    sprintf(packet, "%u,%s", handle, buffer);
    free((void*)buffer);
    sprintf(cmd, "F%X:", size);
    gdbrsp_xmit(cmd, -1);
  }
  return true;
}

static void rspreply_poll(void)
{
  char buffer[1024];
  size_t size = gdbrsp_recv(buffer, sizearray(buffer) - 1, 50);
  if (size > 0) {
    buffer[size] = '\0';
    rspreply_semihosting(buffer); /* translate semihosting packets */
    rspreply_push(buffer);
  }
}


typedef struct tagAPPSTATE {
  int curstate;                 /**< current state */
  bool debugmode;               /**< for extra debug logging */
  bool is_attached;             /**< is debug probe attached? */
  int probe;                    /**< selected debug probe (index) */
  int netprobe;                 /**< index for the IP address (pseudo-probe) */
  const char **probelist;       /**< list of detected probes */
  char mcufamily[32];           /**< name of the target driver */
  const char *monitor_cmds;     /**< list of "monitor" commands (target & probe dependent) */
  unsigned partid;              /**< target MCU part id (or chip id); 0 if not supported */
  bool set_probe_options;       /**< whether option in the debug probe must be set/updated */
  nk_bool tpwr;                 /**< option: tpwr (target power) */
  nk_bool connect_srst;         /**< option: keep in reset during connect */
  nk_bool fullerase;            /**< option: erase entire flash before download */
  nk_bool write_log;            /**< option: record downloads in log file */
  nk_bool print_time;           /**< option: print download time */
  bool skip_download;           /**< do download+verify procedure without actually downloading */
  int load_status;              /**< whether to load/reload the target file & options */
  char IPaddr[64];              /**< IP address for network probe */
  int crp_level;                /**< code read protection level */
  int serialize;                /**< serialization option */
  int SerialFmt;                /**< serialization: format */
  char Section[32];             /**< serialization: name of the ELF section */
  char Address[32];             /**< serialization: relative address in section */
  char Match[64];               /**< serialization: match string  */
  char Prefix[64];              /**< serialization: prefix string for "replace" */
  char Serial[32];              /**< serialization: serial number */
  char SerialSize[32];          /**< serialization: size (in bytes of characters) */
  char SerialIncr[32];          /**< serialization: increment */
  char TargetFile[_MAX_PATH];   /**< ELF/HEX/BIN path/filename to download into the target */
  char ParamFile[_MAX_PATH];    /**< configuration file for the target */
  int TargetFileType;           /**< ELF, HEX, BIN */
  char DownloadAddress[32];     /**< address in Flash memory where the target file is downloaded to */
  char SerialFile[_MAX_PATH];   /**< optional file for serialization settings */
  char ScriptFile[_MAX_PATH];   /**< path to post-process script */
  nk_bool ScriptOnFailures;     /**< whether to execute the post-process script on failed uploads too */
  struct tcl tcl;               /**< Tcl context */
  char *tcl_script;             /**< Tcl script (loaded from file) */
  thrd_t thrd_download;         /**< thread id for downloading firmware */
  thrd_t thrd_tcl;              /**< thread id for Tcl script */
  int isrunning_tcl;            /**< running state of the Tcl script */
  int isrunning_download;       /**< running state of the download thread */
  bool download_success;        /**< success/failure state of most recent download */
  unsigned long tstamp_start;   /**< timestamp of start of download */
} APPSTATE;

enum {
  TAB_OPTIONS,
  TAB_SERIALIZATION,
  TAB_STATUS,
  /* --- */
  TAB_COUNT
};

enum {
  THRD_IDLE,      /**< not started (or finished and cleaned-up) */
  THRD_RUNNING,
  THRD_COMPLETED, /**< completed running, but not yet cleaned up */
  THRD_ABORT,
};
enum {
  SER_NONE,
  SER_ADDRESS,
  SER_MATCH
};

enum {
  FMT_BIN,
  FMT_ASCII,
  FMT_UNICODE
};

enum {
  STATE_INIT,
  STATE_IDLE,
  STATE_SAVE,
  STATE_ATTACH,
  STATE_PRE_DOWNLOAD,
  STATE_PATCH_FILE,
  STATE_CLEARFLASH,
  STATE_DOWNLOAD,
  STATE_OPTIONBYTES,
  STATE_VERIFY,
  STATE_FINISH,
  STATE_POSTPROCESS,
  STATE_ERASE_OPTBYTES,
  STATE_FULLERASE,
  STATE_BLANKCHECK,
  STATE_DUMPFLASH,
};

#define SETSTATE(app, state) \
        do { if ((app).debugmode) printf("State %d -> %d\n", (app).curstate, (state)); \
             (app).curstate = (state); \
        } while (0)

static int tcl_cmd_exec(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user)
{
  (void)user;
  struct tcl_value *cmd = tcl_list_item(args, 1);
  int retcode = system(tcl_data(cmd));
  tcl_free(cmd);
  return tcl_result(tcl, (retcode < 0), tcl_value("", 0));
}

static int tcl_cmd_syscmd(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user)
{
  (void)user;
  struct tcl_value *cmd = tcl_list_item(args, 1);
  int result = gdbrsp_xmit(tcl_data(cmd), tcl_length(cmd));
  tcl_free(cmd);
  return tcl_result(tcl, (result < 0), tcl_value("", 0));
}

static int tcl_cmd_puts(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user)
{
  char msg[512] = "";
  struct tcl_value *text = tcl_list_item(args, tcl_list_length(args) - 1);
  strlcpy(msg, tcl_data(text), sizearray(msg));
  if (tcl_list_find(user, "-nonewline") < 0)
    strlcat(msg, "\n", sizearray(msg));
  tcl_free(text);
  log_addstring(msg);
  return tcl_result(tcl, 0, tcl_value("", 0));
}

static int tcl_cmd_wait(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user)
{
  volatile APPSTATE *state = (APPSTATE*)tcl->user;
  assert(state != NULL);
  int nargs = tcl_list_length(args);
  struct tcl_value *arg1 = tcl_list_item(args, 1);
  struct tcl_value *arg2 = (nargs >= 3) ? tcl_list_item(args, 2) : NULL;
  unsigned long timeout_ms = ULONG_MAX;
  const char *varname = NULL;
  int body_arg = 0;
  if (tcl_isnumber(arg1)) {
    /* scenario 1: wait <timeout> [body] */
    timeout_ms = (unsigned long)tcl_number(arg1);
    body_arg = (nargs == 3) ? 2 : 0;
  } else {
    /* scenario 2: wait <var> [timeout] [body] */
    varname = strdup(tcl_data(arg1));
    if (arg2 != NULL && tcl_isnumber(arg2))
      timeout_ms = (unsigned long)tcl_number(arg2);
    body_arg = (nargs == 4 && tcl_isnumber(arg2)) ? 3 : 0;
  }
  unsigned long tstamp_start = timestamp();
  tcl_free(arg1);
  if (arg2 != NULL)
    tcl_free(arg2);
  /* check for data and timeout */
  unsigned long tstamp = timestamp();
  while (state->isrunning_tcl == THRD_RUNNING) {
    tstamp = timestamp();
    if (tstamp - tstamp_start >= timeout_ms)
      break;    /* wait timed out */
    const char *ptr = rspreply_pop();
    if (ptr != NULL) {
      if (varname != NULL && strcmp(varname, "sysreply") == 0) {
        tcl_var(tcl, varname, tcl_value(ptr, strlen(ptr)));
        free((void*)ptr);
        break;  /* variable changed, exit loop */
      }
      free((void*)ptr);
    }
    thrd_yield();
  }
  /* done waiting */
  if (varname != NULL)
    free((void*)varname);
  /* check whether to run the block on timeout */
  bool is_timeout = (tstamp - tstamp_start >= timeout_ms);
  int result;
  if (is_timeout && body_arg > 0 && state->isrunning_tcl == THRD_RUNNING) {
    struct tcl_value *body = tcl_list_item(args, body_arg);
    result = tcl_eval(tcl, tcl_data(body), tcl_length(body) + 1);
    tcl_free(body);
  } else {
    result = tcl_result(tcl, state->isrunning_tcl != THRD_RUNNING, tcl_value(is_timeout ? "0" : "1", 1));
  }
  return result;
}

static int tcl_thread(void *arg)
{
  pointer_setstyle(CURSOR_WAIT);
  APPSTATE *state = (APPSTATE*)arg;
  assert(state != NULL);
  assert(state->tcl_script != NULL);
  int result = tcl_eval(&state->tcl, state->tcl_script, strlen(state->tcl_script) + 1);
  if (result == 1) {
    int line;
    const char *err = tcl_errorinfo(&state->tcl, &line);
    char msg[256];
    snprintf(msg, sizearray(msg), "^1Tcl script error on or after line %d: %s\n", line, err);
    log_addstring(msg);
  }
  free(state->tcl_script);
  state->tcl_script = NULL;
  pointer_setstyle(CURSOR_NORMAL);
  if (state->isrunning_tcl != THRD_ABORT)
    state->isrunning_tcl = THRD_COMPLETED;
  return result == 0;
}

static bool tcl_preparescript(APPSTATE *state)
{
  /* load the script file */
  FILE *fp = fopen(state->ScriptFile,"rt");
  if (fp == NULL) {
    log_addstring("^1Tcl script file not found.\n");
    return false;
  }
  fseek(fp, 0, SEEK_END);
  size_t sz = ftell(fp) + 2;
  state->tcl_script = malloc((sz) * sizeof(char));
  if (state->tcl_script == NULL) {
    fclose(fp);
    log_addstring("^1Memory allocation failure (when loading Tcl script).\n");
    return false;
  }
  fseek(fp, 0, SEEK_SET);
  memset(state->tcl_script, 0, sz);
  char *line = state->tcl_script;
  while (fgets(line, sz, fp) != NULL)
    line += strlen(line);
  assert(line - state->tcl_script < sz);
  fclose(fp);
  /* set variables */
  tcl_var(&state->tcl, "filename", tcl_value(state->TargetFile, strlen(state->TargetFile)));
  const char *serial = (state->serialize != SER_NONE) ? state->Serial : "";
  tcl_var(&state->tcl, "serial", tcl_value(serial, strlen(serial)));
  fp = fopen(state->TargetFile, "rb");
  if (fp != NULL) {
    uint32_t crc = cksum(fp);
    char key[32], value[128];
    sprintf(value, "%u, ", crc);
    tcl_var(&state->tcl, "cksum", tcl_value(value, strlen(value)));
    ident(fp, 0, key, sizearray(key), value, sizearray(value));
    tcl_var(&state->tcl, "ident", tcl_value(value, strlen(value)));
    fclose(fp);
  } else {
    tcl_var(&state->tcl, "cksum", tcl_value("", 0));
    tcl_var(&state->tcl, "ident", tcl_value("", 0));
  }
  tcl_var(&state->tcl, "sysreply", tcl_value("", 0));
  tcl_var(&state->tcl, "status", state->download_success ? tcl_value("1", 1) : tcl_value("0", 1));
  return true;
}

static int download_thread(void *arg)
{
  APPSTATE *state = (APPSTATE*)arg;
  assert(state != NULL);
  pointer_setstyle(CURSOR_WAIT);
  bool result = bmp_download();
  state->isrunning_download = THRD_COMPLETED;
  return result;
}

static char *getpath(char *path, size_t pathlength, const char *basename, const char *basepath)
{
  assert(path != NULL && pathlength > 0);
  assert(basename != NULL);
  assert(basepath != NULL);

  if (basename[0] == DIRSEP_CHAR || (strlen(basename) >= 3 && basename[1] == ':' && basename[2] == '\\')) {
    /* absolute path (ignore basepath) */
    strlcpy(path, basename, pathlength);
  } else {
    /* relative path, use directory part of the basepath parameter */
    strlcpy(path, basepath, pathlength);
    char *ptr = strrchr(path, DIRSEP_CHAR);
    size_t len = (ptr != NULL) ? (ptr - path) + 1 : 0;
    path[len] = '\0';
    strlcat(path, basename, pathlength);
  }
  return path;
}

static bool load_targetparams(const char *filename, APPSTATE *state)
{
  assert(filename != NULL);
  assert(state != NULL);
  if (access(filename, 0) != 0) {
    /* use defaults for all settings */
    state->connect_srst = nk_false;
    state->write_log = nk_false;
    state->print_time = nk_false;
    state->tpwr = nk_false;
    state->fullerase = nk_false;
    state->crp_level = 0;
    state->DownloadAddress[0] = '\0';
    state->ScriptFile[0] = '\0';
    state->ScriptOnFailures = nk_false;
    state->serialize = 0;
    state->Section[0] = '\0';
    state->Address[0] = '\0';
    state->Match[0] = '\0';
    state->Prefix[0] = '\0';
    return false;
  }

  state->connect_srst = (nk_bool)ini_getl("Settings", "connect-srst", 0, filename);
  state->write_log = (nk_bool)ini_getl("Settings", "write-log", 0, filename);
  state->print_time = (nk_bool)ini_getl("Settings", "print-time", 0, filename);

  state->tpwr = (nk_bool)ini_getl("Flash", "tpwr", 0, filename);
  state->fullerase = (nk_bool)ini_getl("Flash", "full-erase", 0, filename);
  state->crp_level = (int)ini_getl("Flash", "crp-level", 0, filename);
  ini_gets("Flash", "download-address", "", state->DownloadAddress, sizearray(state->DownloadAddress), filename);
  ini_gets("Flash", "postprocess", "", state->ScriptFile, sizearray(state->ScriptFile), filename);
  state->ScriptOnFailures = (nk_bool)ini_getl("Flash", "postprocess-failures", 0, filename);

  ini_gets("Serialize", "file", "", state->SerialFile, sizearray(state->SerialFile), filename);
  char serialfile[_MAX_PATH];
  strlcpy(serialfile, filename, sizearray(serialfile));
  if (strlen(state->SerialFile) > 0)
    getpath(serialfile, sizearray(serialfile), state->SerialFile, filename);

  char field[_MAX_PATH];
  state->serialize = (int)ini_getl("Serialize", "option", 0, serialfile);
  ini_gets("Serialize", "address", ".text:0", field, sizearray(field), serialfile);
  char *ptr;
  if ((ptr = strchr(field, ':')) != NULL) {
    *ptr++ = '\0';
    strlcpy(state->Section, field, sizearray(state->Section));
    strlcpy(state->Address, ptr, sizearray(state->Address));
  }
  ini_gets("Serialize", "match", ":0", field, sizearray(field), serialfile);
  if ((ptr = strchr(field, ':')) != NULL) {
    *ptr++ = '\0';
    strlcpy(state->Match, field, sizearray(state->Match));
    strlcpy(state->Prefix, ptr, sizearray(state->Prefix));
  }
  ini_gets("Serialize", "serial", "1:4:0:1", field, sizearray(field), serialfile);
  ptr = (char*)skipwhite(field);
  if (isalpha(*ptr) && *(ptr + 1) == ':')
    ptr += 2; /* looks like the start of a Windows path */
  if ((ptr = strchr(ptr, ':'))!= NULL) {
    char *p2;
    *ptr++ = '\0';
    strlcpy(state->Serial, (strlen(field) > 0) ? field : "1", sizearray(state->Serial));
    if ((p2 = strchr(ptr, ':')) != NULL) {
      *p2++ = '\0';
      strlcpy(state->SerialSize, (strlen(ptr) > 0) ? ptr : "4", sizearray(state->SerialSize));
      state->SerialFmt = (int)strtol(p2, &p2, 10);
      if ((p2 = strchr(p2, ':')) != NULL)
        strlcpy(state->SerialIncr, p2 + 1, sizearray(state->SerialIncr));
    }
  }

  return true;
}

static bool save_targetparams(const char *filename, const APPSTATE *state)
{
  assert(filename != NULL);
  assert(state != NULL);
  if (strlen(filename) == 0)
    return false;

  ini_putl("Settings", "connect-srst", state->connect_srst, filename);
  ini_putl("Settings", "write-log", state->write_log, filename);
  ini_putl("Settings", "print-time", state->print_time, filename);

  ini_putl("Flash", "tpwr", state->tpwr, filename);
  ini_putl("Flash", "full-erase", state->fullerase, filename);
  ini_putl("Flash", "crp-level", state->crp_level, filename);
  ini_puts("Flash", "download-address", state->DownloadAddress, filename);
  ini_puts("Flash", "postprocess", state->ScriptFile, filename);
  ini_putl("Flash", "postprocess-failures", state->ScriptOnFailures, filename);

  ini_puts("Serialize", "file", state->SerialFile, filename);
  char serialfile[_MAX_PATH];
  strlcpy(serialfile, filename, sizearray(serialfile));
  if (strlen(state->SerialFile) > 0)
    getpath(serialfile, sizearray(serialfile), state->SerialFile, filename);
  ini_putl("Serialize", "option", state->serialize, serialfile);
  char field[150];
  sprintf(field, "%s:%s", state->Section, state->Address);
  ini_puts("Serialize", "address", field, serialfile);
  sprintf(field, "%s:%s", state->Match, state->Prefix);
  ini_puts("Serialize", "match", field, serialfile);
  sprintf(field, "%s:%s:%d:%s", state->Serial, state->SerialSize, state->SerialFmt, state->SerialIncr);
  ini_puts("Serialize", "serial", field, serialfile);

  return true;
}

static bool probe_set_options(APPSTATE *state)
{
  assert(state != NULL);
  bool ok = bmp_isopen();
  if (ok && state->set_probe_options && state->monitor_cmds != NULL) {
    char cmd[100];
    if (bmp_expand_monitor_cmd(cmd, sizearray(cmd), "connect", state->monitor_cmds)) {
      strlcat(cmd, " ", sizearray(cmd));
      strlcat(cmd, state->connect_srst ? "enable" : "disable", sizearray(cmd));
      if (!bmp_monitor(cmd)) {
        bmp_callback(BMPERR_MONITORCMD, "Setting connect-with-reset option failed");
        ok = false;
      }
    }
    strcpy(cmd, "tpwr ");
    strlcat(cmd, state->tpwr ? "enable" : "disable", sizearray(cmd));
    if (bmp_monitor(cmd)) {
      /* give the micro-controller a bit of time to start up, after power-up */
#     if defined _WIN32
        Sleep(100);
#     else
        usleep(100 * 1000);
#     endif
    } else {
      bmp_callback(BMPERR_MONITORCMD, "Power to target failed");
      ok = false;
    }
    state->set_probe_options = false;
  }
  return ok;
}

static const char *match_target(const char *mcufamily, const char *drivers[], unsigned numdrivers)
{
  const char *match = NULL;
  int matchlevel = INT_MAX;

  /* try exact match first */
  for (int arch = 0; arch < numdrivers; arch++) {
    int level = architecture_match(drivers[arch], mcufamily);
    if (level > 0 && level < matchlevel) {
      match = drivers[arch];
      matchlevel = level;
    }
  }
  if (match != NULL)
    return match;       /* match was found -> done (skip prefix match) */

  /* then try prefix match */
  assert(matchlevel == INT_MAX);  /* should still be unchanged, otherwise a match had been found */
  for (int arch = 0; arch < numdrivers; arch++) {
    int len = strlen(drivers[arch]);
    char pattern[32];
    strcpy(pattern, mcufamily);
    pattern[len] = '\0';
    int level = architecture_match(drivers[arch], pattern);
    if (level > 0 && level < matchlevel) {
      match = drivers[arch];
      matchlevel = level;
    }
  }

  return match;
}

static const char *target_is_lpc(const char *mcufamily)
{
  static const char *drivers[] = { "LPC8xx", "LPC110x", "LPC11xx", "LPC11Axx",
                                   "LPC11Cxx", "LPC11Exx", "LPC11Uxx", "LPC122x",
                                   "LPC13xx", "LPC15xx", "LPC17xx", "LPC18xx",
                                   "LPC21xx", "LPC22xx", "LPC23xx", "LPC24xx",
                                   "LPC40xx", "LPC43xx", "LPC546xx" };
  return match_target(mcufamily, drivers, sizearray(drivers));
}

static const char *target_is_stm32(const char *mcufamily)
{
  static const char *drivers[] = { "STM32F0xx", "STM32F1xx", "STM32F3xx",
                                   "STM32L0x", "STM32L1x", "STM32L4xx",
                                   "STM32L5xx", "STM32WBxx", "STM32WLxx",
                                   "STM32G4xx", "STM32G0x/x" };
  return match_target(mcufamily, drivers, sizearray(drivers));
}

static const char *target_is_gd32(const char *mcufamily)
{
  static const char *drivers[] = { "GD32F0xx", "GD32F1xx", "GD32F3xx", "GD32E230" };
  return match_target(mcufamily, drivers, sizearray(drivers));
}

static bool target_partid(APPSTATE *state)
{
  assert(state != NULL);
  state->partid = 0;
  if (state->is_attached) {
    if (bmp_has_command("partid", state->monitor_cmds)) {
      state->partid = bmp_get_partid();
    } else {
      unsigned long params[4];
      if (bmp_runscript("partid", state->mcufamily, NULL, params, 1)) {
        state->partid = params[0];
        return true;
      }
    }
  }
  return false;
}

static bool target_flashsize(APPSTATE *state, unsigned *size)
{
  assert(state != NULL);
  assert(size != NULL);
  if (state->partid != 0) {
    const MCUINFO *info = mcuinfo_data(state->mcufamily, state->partid);
    if (info != NULL && info->flash != ~0) {
     *size = info->flash;
     return true;
    }
    if (state->is_attached && (target_is_stm32(state->mcufamily) || target_is_gd32(state->mcufamily))) {
      unsigned long params[4];
      if (bmp_runscript("flashsize", state->mcufamily, NULL, params, 1)) {
        *size = (params[0] & 0xffff) * 1024;
        return true;
      }
    }
  }
  return false;
}

static void check_crp_level(APPSTATE *state)
{
  if (state->TargetFileType != FILETYPE_ELF)
    return;

  FILE *fp = fopen(state->TargetFile, "rb");
  if (fp == NULL)
    return; /* should not happen, because filetype was already established */
  int lpc_crp_level;
  int result = elf_check_crp(fp, &lpc_crp_level);
  bool lpc_cksum = (result == ELFERR_NONE && elf_check_vecttable(fp) == ELFERR_NONE);
  fclose(fp);

  if (0 < lpc_crp_level && lpc_crp_level < 4 && state->crp_level != lpc_crp_level) {
    char msg[100];
    sprintf(msg, "^3Code Read Protection (CRP%d) is hardcoded in the ELF file\n", lpc_crp_level);
    log_addstring(msg);
    if (state->crp_level == 0) {
      log_addstring("^1Option mismatch: CRP option is not set (CRP will be removed).\n");
    } else {
      sprintf(msg, "^1Option mismatch: CRP option is set to level %d.\n", state->crp_level);
      log_addstring(msg);
    }
  } else if (state->crp_level != 0) {
    char msg[100];
    sprintf(msg, "^3Code Read Protection level %d is set\n", state->crp_level);
    log_addstring(msg);
    if (lpc_crp_level == 4)
      log_addstring("^1Cannot set CRP because NO-ISP signature is set\n");
    else if (lpc_cksum && lpc_crp_level == 0 )
      log_addstring("^1Cannot set CRP because the signature is missing\n");
  }
}

static bool panel_target(struct nk_context *ctx, APPSTATE *state)
{
  /* edit/selection field for the target (ELF) file (plus browse button) */
  const char *p = strrchr(state->TargetFile, DIRSEP_CHAR);
  char basename[_MAX_PATH];
  strlcpy(basename, (p == NULL) ? state->TargetFile : p + 1, sizearray(basename));
  nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
  nk_layout_row_push(ctx, 8 * opt_fontsize);
  guidriver_setfont(ctx, FONT_HEADING2);
  nk_style_push_vec2(ctx, &ctx->style.button.padding, nk_vec2(0, 0));
  nk_label(ctx, " Target file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
  guidriver_setfont(ctx, FONT_STD);
  nk_style_pop_vec2(ctx);
  nk_layout_row_push(ctx, WINDOW_WIDTH - 12 * opt_fontsize);
  bool error = (state->load_status == 0 && state->TargetFileType == FILETYPE_NONE);
  char tiptext[_MAX_PATH] = "";
  if (strlen(basename) == 0) {
    strcpy(tiptext, "Please select a target file");
  } else {
    if (error)
      strcpy(tiptext, "NOT FOUND: ");
    strlcat(tiptext, state->TargetFile, sizearray(tiptext));
  }
  editctrl_cond_color(ctx, error, COLOUR_BG_DARKRED);
  int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_READ_ONLY,
                                basename, sizearray(basename),
                                nk_filter_ascii, tiptext);
  editctrl_reset_color(ctx, error);
  nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
  if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)
      || nk_input_is_key_pressed(&ctx->input, NK_KEY_OPEN)
      || (result & NK_EDIT_BLOCKED) != 0)
  {
    nk_input_clear_mousebuttons(ctx);
    osdialog_filters *filters = osdialog_filters_parse("ELF Executables:elf;HEX file:hex;All files:*");
    char *fname = osdialog_file(OSDIALOG_OPEN, "Select Target file", NULL, state->TargetFile, filters);
    osdialog_filters_free(filters);
    if (fname != NULL) {
      strlcpy(state->TargetFile, fname, sizearray(state->TargetFile));
      free(fname);
      state->load_status = 2;
    }
  }
  nk_layout_row_end(ctx);

  if (!error && state->TargetFileType == FILETYPE_UNKNOWN) {
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, 8 * opt_fontsize);
    nk_label(ctx, " Address", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, 8 * opt_fontsize);
    editctrl_tooltip(ctx, NK_EDIT_FIELD, state->DownloadAddress, sizearray(state->DownloadAddress),
                     nk_filter_dec_hex,
                     "Base address where the file will be downloaded to.\n"
                     "When left empty, the file is downloaded to the start of\n"
                     "the Flash memory of the microcontroller.");
    nk_layout_row_end(ctx);
  }

  return error;
}

static void panel_options(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT])
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_states != NULL);

  #define COMPACT_ROW   (ROW_HEIGHT * 0.8)
  nk_bool toggled;
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Options", &tab_states[TAB_OPTIONS], &toggled)) {
    assert(state->probelist != NULL);
    bool reconnect = false;
    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.45, 0.55));
    nk_label(ctx, "Black Magic Probe", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    struct nk_rect rcwidget = nk_widget_bounds(ctx);
    int select = nk_combo(ctx, state->probelist, state->netprobe+1, state->probe,
                          (int)COMBOROW_CY, nk_vec2(rcwidget.w, 4.5*ROW_HEIGHT));
    if (select != state->probe) {
      state->probe = select;
      reconnect = true;
    }
    if (state->probe == state->netprobe) {
      nk_layout_row(ctx, NK_DYNAMIC, COMPACT_ROW, 4, nk_ratio(4, 0.05, 0.40, 0.50, 0.05));
      nk_spacing(ctx, 1);
      nk_label(ctx, "IP Address", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_flags result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                                       state->IPaddr, sizearray(state->IPaddr),
                                                       nk_filter_ascii);
      if ((result & NK_EDIT_COMMITED) != 0 && bmp_is_ip_address(state->IPaddr))
        reconnect = true;
      if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIPLE_DOT, NK_KEY_NONE, nk_true, "Scan network for ctxLink probes.")) {
#       if defined WIN32 || defined _WIN32
          HCURSOR hcur = SetCursor(LoadCursor(NULL, IDC_WAIT));
#       endif
        unsigned long addr;
        int count = scan_network(&addr, 1);
#       if defined WIN32 || defined _WIN32
          SetCursor(hcur);
#       endif
        if (count == 1) {
          sprintf(state->IPaddr, "%lu.%lu.%lu.%lu",
                 addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >> 24) & 0xff);
          reconnect = true;
        } else {
          strlcpy(state->IPaddr, "no gdbserver found", sizearray(state->IPaddr));
        }
      }
    }
    if (reconnect) {
      bmp_disconnect();
      bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
      SETSTATE(*state, STATE_IDLE);
    }

    nk_layout_row(ctx, NK_DYNAMIC, COMPACT_ROW, 4, nk_ratio(4, 0.45, 0.15, 0.10, 0.30));
    int crp_level = state->crp_level;
    nk_bool crp_enabled = (state->crp_level > 0);
    checkbox_tooltip(ctx, "Code Read Protection", &crp_enabled, NK_TEXT_LEFT,
                     "Protect code from being extracted or modified");
    if (crp_enabled) {
      nk_label(ctx, "Level", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      if (state->crp_level == 0)
        state->crp_level = 1;
      static const char *levels[3] = { "1", "2", "3" };
      rcwidget = nk_widget_bounds(ctx);
      int select = nk_combo(ctx, levels, 3, state->crp_level - 1,
                            (int)COMBOROW_CY, nk_vec2(rcwidget.w, 3*ROW_HEIGHT));
      state->crp_level = select + 1;
    } else {
      state->crp_level = 0;
      nk_spacing(ctx, 1);
    }
    if (state->crp_level != crp_level)
      check_crp_level(state); /* test for conflicts with loaded ELF file (NXP LPC series) */

    nk_layout_row_dynamic(ctx, COMPACT_ROW, 1);
    if (checkbox_tooltip(ctx, "Power Target (3.3V)", &state->tpwr, NK_TEXT_LEFT,
                         "Let the debug probe provide power to the target"))
      state->set_probe_options = true;
    checkbox_tooltip(ctx, "Full Flash Erase before download", &state->fullerase, NK_TEXT_LEFT,
                     "Erase entire Flash memory, instead of only sectors that are overwritten");
    if (checkbox_tooltip(ctx, "Reset Target during connect", &state->connect_srst, NK_TEXT_LEFT,
                         "Keep target MCU reset while debug probe attaches"))
      state->set_probe_options = true;
    checkbox_tooltip(ctx, "Keep Log of downloads", &state->write_log, NK_TEXT_LEFT,
                     "Write successful downloads to a log file");
    checkbox_tooltip(ctx, "Show Download Time", &state->print_time, NK_TEXT_LEFT,
                     "Print after each download, how many seconds it took");

    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 3, nk_ratio(3, 0.45, 0.497, 0.053));
    nk_label(ctx, "Post-processing script (Tcl)", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    bool error = editctrl_cond_color(ctx, strlen(state->ScriptFile) > 0 && access(state->ScriptFile, 0) != 0, COLOUR_BG_DARKRED);
    editctrl_tooltip(ctx, NK_EDIT_FIELD,
                     state->ScriptFile, sizearray(state->ScriptFile),
                     nk_filter_ascii, "Tcl script to run after a successful download");
    editctrl_reset_color(ctx, error);
    if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIPLE_DOT, NK_KEY_NONE, nk_true, "Browse...")) {
      nk_input_clear_mousebuttons(ctx);
      osdialog_filters *filters = osdialog_filters_parse("Tcl scripts:tcl;All files:*");
      char *fname = osdialog_file(OSDIALOG_OPEN, "Select Tcl script", NULL, state->ScriptFile, filters);
      osdialog_filters_free(filters);
      if (fname != NULL) {
        strlcpy(state->ScriptFile, fname, sizearray(state->ScriptFile));
        free(fname);
      }
    }
    nk_layout_row(ctx, NK_DYNAMIC, COMPACT_ROW, 2, nk_ratio(2, 0.45, 0.55));
    nk_spacing(ctx, 1);
    checkbox_tooltip(ctx, "Post-process on failed downloads", &state->ScriptOnFailures, NK_TEXT_LEFT,
                     "Also run the post-process script after a failed download");

    nk_tree_state_pop(ctx);

    tab_states[TAB_SERIALIZATION] = NK_MINIMIZED;
    tab_states[TAB_STATUS] = NK_MINIMIZED;
  } else if (toggled) {
    tab_states[TAB_STATUS] = NK_MAXIMIZED;
  }
}

static void panel_serialize(struct nk_context *ctx, APPSTATE *state,
                            enum nk_collapse_states tab_states[TAB_COUNT])
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_states != NULL);

  nk_bool toggled;
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Serialization", &tab_states[TAB_SERIALIZATION], &toggled)) {
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 3);
    if (nk_option_label(ctx, "No serialization", (state->serialize == SER_NONE), NK_TEXT_LEFT))
      state->serialize = SER_NONE;
    if (nk_option_label(ctx, "Address", (state->serialize == SER_ADDRESS), NK_TEXT_LEFT))
      state->serialize = SER_ADDRESS;
    if (nk_option_label(ctx, "Match", (state->serialize == SER_MATCH), NK_TEXT_LEFT))
      state->serialize = SER_MATCH;
    if (state->serialize == SER_ADDRESS) {
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.05, 0.20, 0.3, 0.15, 0.3));
      nk_spacing(ctx, 1);
      nk_label(ctx, "Section", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      editctrl_tooltip(ctx,NK_EDIT_FIELD,state->Section,sizearray(state->Section),
                       nk_filter_ascii, "The name of the section in the ELF file\n(ignored for HEX & BIN files)");
      nk_label(ctx, "Offset", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
      editctrl_tooltip(ctx, NK_EDIT_FIELD, state->Address, sizearray(state->Address),
                       nk_filter_hex, "The offset in hexadecimal");
    }
    if (state->serialize == SER_MATCH) {
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.05, 0.20, 0.3, 0.15, 0.3));
      nk_spacing(ctx, 1);
      nk_label(ctx, "Pattern", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      editctrl_tooltip(ctx,NK_EDIT_FIELD,state->Match,sizearray(state->Match),
                       nk_filter_ascii, "The text to match");
      nk_label(ctx, "Prefix", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
      editctrl_tooltip(ctx, NK_EDIT_FIELD, state->Prefix, sizearray(state->Prefix),
                       nk_filter_ascii, "Text to write back at the matched position, prefixing the serial number");
    }
    if (state->serialize != SER_NONE) {
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.05, 0.20, 0.3, 0.15, 0.3));
      nk_spacing(ctx, 1);
      nk_label(ctx, "Serial", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      editctrl_tooltip(ctx, NK_EDIT_FIELD, state->Serial, sizearray(state->Serial),
                       nk_filter_decimal, "The serial number to write (decimal value)");
      nk_label(ctx, "Size", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
      editctrl_tooltip(ctx, NK_EDIT_FIELD, state->SerialSize, sizearray(state->SerialSize),
                       nk_filter_decimal, "The size (in bytes) that the serial number is padded to");
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.05, 0.20, 0.25, 0.25, 0.25));
      nk_spacing(ctx, 1);
      nk_label(ctx, "Format", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      if (nk_option_label(ctx, "Binary", (state->SerialFmt == FMT_BIN), NK_TEXT_LEFT))
        state->SerialFmt = FMT_BIN;
      if (nk_option_label(ctx, "ASCII", (state->SerialFmt == FMT_ASCII), NK_TEXT_LEFT))
        state->SerialFmt = FMT_ASCII;
      if (nk_option_label(ctx, "Unicode", (state->SerialFmt == FMT_UNICODE), NK_TEXT_LEFT))
        state->SerialFmt = FMT_UNICODE;
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(4, 0.05, 0.20, 0.25, 0.5));
      nk_spacing(ctx, 1);
      nk_label(ctx, "Increment", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      editctrl_tooltip(ctx, NK_EDIT_FIELD, state->SerialIncr, sizearray(state->SerialIncr),
                       nk_filter_decimal, "The increment for the serial number");
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(5, 0.05, 0.20, 0.75));
      nk_spacing(ctx, 1);
      nk_label(ctx, "File", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      editctrl_tooltip(ctx, NK_EDIT_FIELD, state->SerialFile, sizearray(state->SerialFile),
                       nk_filter_ascii, "The file to store the serialization settings in\nLeave empty to use the local configuration file");
    }
    nk_tree_state_pop(ctx);

    tab_states[TAB_OPTIONS] = NK_MINIMIZED;
    tab_states[TAB_STATUS] = NK_MINIMIZED;
  } else if (toggled) {
    tab_states[TAB_STATUS] = NK_MAXIMIZED;
  }
}

static bool handle_stateaction(APPSTATE *state, enum nk_collapse_states tab_states[TAB_COUNT])
{
  assert(state != NULL);

  bool waitidle = true;
  int result;

  switch (state->curstate) {
  case STATE_INIT:
    /* collect debug probes, connect to the selected one */
    clear_probelist(state->probelist, state->netprobe);
    state->probelist = get_probelist(&state->probe, &state->netprobe);
    tcpip_init();
    bmp_setcallback(bmp_callback);
    result = bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
    if (result && state->monitor_cmds == NULL)
      state->monitor_cmds = bmp_get_monitor_cmds();
    state->set_probe_options = true;  /* probe changed, make sure options are set */
    bmp_progress_reset(0);
    SETSTATE(*state, STATE_IDLE);
    waitidle = false;
    break;

  case STATE_IDLE:
    filesection_clearall();
    if (state->is_attached) {
      bmp_detach(false);    /* if currently attached, detach */
      state->is_attached = false;
    }
    if (state->set_probe_options && bmp_isopen())
      probe_set_options(state);
    gdbrsp_clear();
    state->skip_download = false;
    state->partid = 0;
    break;

  case STATE_SAVE:
    tab_states[TAB_OPTIONS] = NK_MINIMIZED;
    tab_states[TAB_SERIALIZATION] = NK_MINIMIZED;
    tab_states[TAB_STATUS] = NK_MAXIMIZED;
    if (access(state->TargetFile, 0) == 0) {
      /* save settings in config file */
      strlcpy(state->ParamFile, state->TargetFile, sizearray(state->ParamFile));
      strlcat(state->ParamFile, ".bmcfg", sizearray(state->ParamFile));
      save_targetparams(state->ParamFile, state);
      SETSTATE(*state, STATE_ATTACH);
      state->tstamp_start = timestamp();
    } else {
      log_addstring("^1Failed to open the target file (ELF/HEX/BIN)\n");
      SETSTATE(*state, STATE_IDLE);
    }
    waitidle = false;
    break;

  case STATE_ATTACH:
    bmp_progress_reset(0);
    result = bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
    if (result) {
      if (state->monitor_cmds == NULL)
        state->monitor_cmds = bmp_get_monitor_cmds(); /* get probe commands before attaching, to set the options for "attach" */
      probe_set_options(state);
      state->is_attached = bmp_attach(false, state->mcufamily, sizearray(state->mcufamily), NULL, 0);
      if (state->is_attached) {
        char msg[128];
        sprintf(msg, "Target found: %s\n", state->mcufamily);
        log_addstring(msg);
        if (state->monitor_cmds != NULL)  /* get probe commands again, to also get the target-specific commands */
          free((void*)state->monitor_cmds);
        state->monitor_cmds = bmp_get_monitor_cmds();
      }
      if (bmp_flashtotal(NULL, NULL) == 0)
        result = 0; /* no use downloading firmware to a chip that has no Flash */
    }
    SETSTATE(*state, (result && state->is_attached) ? STATE_PRE_DOWNLOAD : STATE_IDLE);
    waitidle = false;
    break;

  case STATE_PRE_DOWNLOAD:
    if (!filesection_loadall(state->TargetFile)) {
      log_addstring("^1Failed to read the target file into memory\n");
      if (state->TargetFileType == FILETYPE_UNKNOWN) {
        unsigned long addr;
        if (strlen(state->DownloadAddress) == 0)
          bmp_flashtotal(&addr, NULL);
        else
          addr = strtoul(state->DownloadAddress, NULL, 0);
        if (addr != 0) {
          filesection_relocate(addr);
          char msg[128];
          sprintf(msg, "BIN file relocated to 0x%08lx\n", addr);
          log_addstring(msg);
        }
      }
      SETSTATE(*state, STATE_IDLE);
    } else {
      SETSTATE(*state, STATE_PATCH_FILE);
    }
    waitidle = false;
    break;

  case STATE_PATCH_FILE:
    /* verify whether to patch the ELF file (already in memory) */
    if (target_is_lpc(state->mcufamily) != NULL || state->serialize != SER_NONE) {
      assert(filesection_filetype() != FILETYPE_NONE);  /* target file must already be in memory */
      const char *tgtdriver = target_is_lpc(state->mcufamily);
      bool result = true;
      if (tgtdriver != NULL) {
        result = patch_vecttable(tgtdriver);
        if (result) {
          int old_level = filesection_get_crp();
          int set_level = (state->crp_level > 0) ? state->crp_level : 9;
          bool crp_result = filesection_set_crp(set_level);  /* fails if the ELF file was not prepared for CRP */
          char msg[100] = "";
          if (crp_result) {
            assert(old_level > 0);
            if ((old_level == 9 || old_level == set_level) && set_level < 9)
              sprintf(msg, "^4Code Read Protection is set, level %d\n", set_level);
            else if (old_level < 9 && set_level == 9)
              sprintf(msg, "^4Code Read Protection is reset to level 0\n");
            else if (old_level != set_level && set_level != 9)
              sprintf(msg, "^4Code Read Protection is overruled to level %d\n", set_level);
          } else if (state->crp_level > 0) {
            sprintf(msg, "^1Failure setting CRP level %d\n", state->crp_level);
            result = false;
          }
          if (strlen(msg) > 0 && !state->skip_download)
            log_addstring(msg);
        }
      }
      if (result && state->serialize != SER_NONE) {
        /* create replacement buffer, depending on format */
        unsigned char data[50];
        int datasize = (int)strtol(state->SerialSize, NULL, 10);
        serialize_fmtoutput(data, datasize, serial_get(state->Serial), state->SerialFmt);
        //??? enhancement: run a script, because the serial number may include a check digit
        if (state->serialize == SER_ADDRESS)
          result = serialize_address(state->TargetFile, state->Section, strtoul(state->Address,NULL,16), data, datasize);
        else if (state->serialize == SER_MATCH)
          result = serialize_match(state->Match, state->Prefix, data, datasize);
        if (result && !state->skip_download) {
          char msg[100];
          sprintf(msg, "^4Serial adjusted to %d\n", serial_get(state->Serial));
          log_addstring(msg);
        }
      }
      SETSTATE(*state, result ? STATE_CLEARFLASH : STATE_IDLE);
    } else {
      SETSTATE(*state, STATE_CLEARFLASH);
    }
    waitidle = false;
    break;

  case STATE_CLEARFLASH:
    if (!state->skip_download && state->fullerase) {
      unsigned flashsize = UINT_MAX;
      target_partid(state);
      target_flashsize(state, &flashsize);
      const char *tgtdriver = target_is_lpc(state->mcufamily);
      if (tgtdriver != NULL)
        bmp_runscript("memremap", tgtdriver, NULL, NULL, 0);
      result = bmp_fullerase(flashsize);
      SETSTATE(*state, result ? STATE_DOWNLOAD : STATE_IDLE);
    } else {
      SETSTATE(*state, STATE_DOWNLOAD);
    }
    waitidle = false;
    break;

  case STATE_DOWNLOAD:
    /* download to target */
    if (!state->skip_download) {
      bool ok = true;
      if (state->isrunning_download == THRD_IDLE) {
        if (state->partid == 0)
          target_partid(state);
        const char *tgtdriver = target_is_lpc(state->mcufamily);
        if (tgtdriver != NULL)
          bmp_runscript("memremap", tgtdriver, NULL, NULL, 0);
        /* create a thread to do the download, so that this loop continues with
           updating the message log, while the download is in progress */
        state->isrunning_download = THRD_RUNNING;
        if (thrd_create(&state->thrd_download, download_thread, state) != thrd_success) {
          ok = false;
          state->isrunning_download = THRD_IDLE;
        }
      } else if (state->isrunning_download == THRD_COMPLETED || state->isrunning_download == THRD_ABORT) {
        if (state->isrunning_download == THRD_ABORT)
          log_addstring("^1Aborted\n");
        int retcode;
        thrd_join(state->thrd_download, &retcode);
        ok = (retcode > 0 && state->isrunning_download == THRD_COMPLETED);
        state->isrunning_download = THRD_IDLE;
      }
      if (state->isrunning_download == THRD_IDLE)
        SETSTATE(*state, ok ? STATE_OPTIONBYTES : STATE_IDLE);
    } else {
      SETSTATE(*state, STATE_OPTIONBYTES);
    }
    waitidle = false;
    break;

  case STATE_OPTIONBYTES:
    if (state->crp_level > 0 && (target_is_stm32(state->mcufamily) != NULL || target_is_gd32(state->mcufamily) != NULL)) {
      /* set option bytes for CRP */
      if (state->crp_level == 1 || state->crp_level == 2) {
        char cmd[100];
        sprintf(cmd, "option 0x1ffff800 0x%04x", (state->crp_level == 1) ? 0x00ff : 0x33cc);
        if (bmp_monitor(cmd)) {
          sprintf(cmd, "^4CRP level %d set; power cycle is needed\n", state->crp_level);
          log_addstring(cmd);
        } else {
          log_addstring("^1Failed to set the option byte for CRP\n");
        }
      } else {
        char msg[100];
        sprintf(msg, "^1CRP level %d is invalid for the target MCU\n", state->crp_level);
        log_addstring(msg);
      }
    }
    SETSTATE(*state, STATE_VERIFY);
    waitidle = false;
    break;

  case STATE_VERIFY:
    /* compare the checksum of Flash memory to the file */
    if (target_is_lpc(state->mcufamily) != NULL)
      bmp_runscript("memremap", target_is_lpc(state->mcufamily), NULL, NULL, 0);
    state->download_success = bmp_verify();
    if (state->download_success)
      SETSTATE(*state, STATE_FINISH);
    else if (strlen(state->ScriptFile) > 0 && state->ScriptOnFailures)
      SETSTATE(*state, STATE_POSTPROCESS);
    else
      SETSTATE(*state, STATE_IDLE);
    if (state->download_success && state->print_time) {
      char msg[100];
      clock_t tstamp_stop = clock();
      sprintf(msg, "Completed in %.1f seconds\n", (tstamp_stop - state->tstamp_start) / 1000.0);
      log_addstring(msg);
    }
    waitidle = false;
    break;

  case STATE_FINISH:
    /* optionally log the download */
    if (state->write_log && !writelog(state->TargetFile, (state->serialize != SER_NONE) ? state->Serial : NULL))
      log_addstring("^3Failed to write to log file\n");
    /* optionally increment the serial number */
    if (state->serialize != SER_NONE && !state->skip_download) {
      int incr = (int)strtol(state->SerialIncr, NULL, 10);
      if (incr < 1)
        incr = 1;
      serial_increment(state->Serial, incr);
      /* must update this in the cache file immediately (so that the cache is
         up-to-date when the user aborts/quits the utility) */
      char field[200];
      sprintf(field, "%s:%s:%d:%s", state->Serial, state->SerialSize, state->SerialFmt, state->SerialIncr);
      char serialfile[_MAX_PATH];
      strlcpy(serialfile, state->ParamFile, sizearray(serialfile));
      if (strlen(state->SerialFile) > 0)
        getpath(serialfile, sizearray(serialfile), state->SerialFile, state->ParamFile);
      ini_puts("Serialize", "serial", field, serialfile);
    }
    SETSTATE(*state, STATE_POSTPROCESS);
    waitidle = false;
    break;

  case STATE_POSTPROCESS:
    /* optionally perform a post-processing step */
    if (strlen(state->ScriptFile) > 0) {
      if (state->isrunning_tcl == THRD_IDLE) {
        char *basename = strrchr(state->ScriptFile, DIRSEP_CHAR);
        if (basename != NULL)
          basename += 1;
        else
          basename = state->ScriptFile;
        if (tcl_preparescript(state)) {
          char msg[280];
          sprintf(msg, "Running: %s\n", basename);
          log_addstring(msg);
          gdbrsp_clear();
          /* start a thread to run the script (it is resumed in the main loop) */
          state->isrunning_tcl = THRD_RUNNING;
          if (thrd_create(&state->thrd_tcl, tcl_thread, state) != thrd_success)
            state->isrunning_tcl = THRD_IDLE;
        } else {
          char msg[280];
          sprintf(msg, "^1Failed running: %s\n", basename);
          log_addstring(msg);
          gdbrsp_clear();
        }
      } else if (state->isrunning_tcl == THRD_COMPLETED || state->isrunning_tcl == THRD_ABORT) {
        log_addstring(state->isrunning_tcl == THRD_COMPLETED ? "^2Done\n" : "^1Aborted\n");
        thrd_join(state->thrd_tcl, &state->isrunning_tcl);
        state->isrunning_tcl = THRD_IDLE;
      } else if (state->isrunning_tcl == THRD_RUNNING) {
        rspreply_poll();
      }
      if (state->isrunning_tcl == THRD_IDLE) {
        SETSTATE(*state, STATE_IDLE);
        waitidle = false;
      }
    } else {
      SETSTATE(*state, STATE_IDLE);
      waitidle = false;
    }
    break;

  case STATE_ERASE_OPTBYTES:
    bmp_progress_reset(0);
    if (bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL)
        && probe_set_options(state)
        && bmp_attach(false, state->mcufamily, sizearray(state->mcufamily), NULL, 0))
    {
      state->is_attached = true;
      /* get the monitor commands (again, now that the target is attached),
         check that "option" is available */
      if (state->monitor_cmds != NULL)
        free((void*)state->monitor_cmds);
      state->monitor_cmds = bmp_get_monitor_cmds();
      assert(state->monitor_cmds != NULL);
      if (bmp_has_command("option", state->monitor_cmds)) {
        result = bmp_monitor("option erase");
        if (result) {
          bmp_detach(state->tpwr);
          state->is_attached = false;
          if (state->tpwr) {
            log_addstring("^2Option bytes erased\n");
            /* make sure power stays off for a short while */
#           if defined _WIN32
              Sleep(100);
#           else
              usleep(100 * 1000);
#           endif
            state->set_probe_options = true;
          } else {
            log_addstring("^2Option bytes erased; power cycle is needed\n");
          }
        } else {
          log_addstring("^1Failed to erase the option bytes\n");
        }
      } else {
        char msg[100];
        sprintf(msg, "^1Command not supported for target driver %s\n", state->mcufamily);
      }
    }
    SETSTATE(*state, STATE_IDLE);
    waitidle = false;
    break;

  case STATE_FULLERASE:
    bmp_progress_reset(0);
    pointer_setstyle(CURSOR_WAIT);
    if (bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL)
        && probe_set_options(state)
        && bmp_attach(false, state->mcufamily, sizearray(state->mcufamily), NULL, 0))
    {
      if (state->partid == 0 || !state->is_attached) {
        state->is_attached = true;
        target_partid(state);
      }
      unsigned flashsize = UINT_MAX;
      target_flashsize(state, &flashsize);
      const char *tgtdriver = target_is_lpc(state->mcufamily);
      if (tgtdriver != NULL)
        bmp_runscript("memremap", tgtdriver, NULL, NULL, 0);
      bmp_fullerase(flashsize); //??? do this in a thread, for better user interface response
    }
    pointer_setstyle(CURSOR_NORMAL);
    SETSTATE(*state, STATE_IDLE);
    waitidle = false;
    break;

  case STATE_BLANKCHECK:
    bmp_progress_reset(0);
    pointer_setstyle(CURSOR_WAIT);
    if (bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL)
        && probe_set_options(state)
        && bmp_attach(false, state->mcufamily, sizearray(state->mcufamily), NULL, 0))
    {
      if (state->partid == 0 || !state->is_attached) {
        state->is_attached = true;
        target_partid(state);
      }
      unsigned flashsize = UINT_MAX;
      target_flashsize(state, &flashsize);
      const char *tgtdriver = target_is_lpc(state->mcufamily);
      if (tgtdriver != NULL)
        bmp_runscript("memremap", tgtdriver, NULL, NULL, 0);
      bmp_blankcheck(flashsize); //??? do this in a thread, for better user interface response
    }
    pointer_setstyle(CURSOR_NORMAL);
    SETSTATE(*state, STATE_IDLE);
    waitidle = false;
    break;

  case STATE_DUMPFLASH:
    bmp_progress_reset(0);
    pointer_setstyle(CURSOR_WAIT);
    if (bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL)
        && probe_set_options(state)
        && bmp_attach(false, state->mcufamily, sizearray(state->mcufamily), NULL, 0))
    {
      osdialog_filters *filters = osdialog_filters_parse("BIN files:bin;All files:*");
      char *fname = osdialog_file(OSDIALOG_SAVE, "Save as BIN file", NULL, NULL, filters);
      osdialog_filters_free(filters);
      if (fname != NULL && strlen(fname) > 0) {
        /* copy to local path, so that default extension can be appended */
        char path[_MAX_PATH];
        strlcpy(path, fname, sizearray(path));
        free(fname);
        const char *ext;
        if ((ext = strrchr(path, '.')) == NULL || strchr(ext, DIRSEP_CHAR) != NULL)
          strlcat(path, ".bin", sizearray(path)); /* default extension .csv */
        if (state->partid == 0 || !state->is_attached) {
          state->is_attached = true;
          target_partid(state);
        }
        pointer_setstyle(CURSOR_WAIT);
        unsigned flashsize = UINT_MAX;
        target_flashsize(state, &flashsize);
        const char *tgtdriver = target_is_lpc(state->mcufamily);
        if (tgtdriver != NULL)
          bmp_runscript("memremap", tgtdriver, NULL, NULL, 0);
        bmp_dumpflash(path, flashsize); //??? do this in a thread, for better user interface response
      }
    }
    pointer_setstyle(CURSOR_NORMAL);
    SETSTATE(*state, STATE_IDLE);
    waitidle = false;
    break;
  }

  return waitidle;
}

int main(int argc, char *argv[])
{
# if defined FORTIFY
    Fortify_SetOutputFunc(Fortify_OutputFunc);
# endif

  /* global defaults */
  APPSTATE appstate;
  memset(&appstate, 0, sizeof appstate);
  appstate.curstate = STATE_INIT;
  appstate.serialize = SER_NONE;
  appstate.SerialFmt = FMT_BIN;
  appstate.isrunning_tcl = THRD_IDLE;
  appstate.isrunning_download = THRD_IDLE;
  appstate.set_probe_options = true;
  strcpy(appstate.Section, ".text");
  strcpy(appstate.Address, "0");
  strcpy(appstate.Serial, "1");
  strcpy(appstate.SerialSize, "4");
  strcpy(appstate.SerialIncr, "1");

  /* read defaults from the configuration file */
  char txtConfigFile[_MAX_PATH];
  get_configfile(txtConfigFile, sizearray(txtConfigFile), "bmflash.ini");
  appstate.probe = (int)ini_getl("Settings", "probe", 0, txtConfigFile);
  ini_gets("Settings", "ip-address", "127.0.0.1", appstate.IPaddr, sizearray(appstate.IPaddr), txtConfigFile);
  opt_fontsize = ini_getf("Settings", "fontsize", FONT_HEIGHT, txtConfigFile);
  char opt_fontstd[64] = "", opt_fontmono[64] = "";
  ini_gets("Settings", "fontstd", "", opt_fontstd, sizearray(opt_fontstd), txtConfigFile);
  ini_gets("Settings", "fontmono", "", opt_fontmono, sizearray(opt_fontmono), txtConfigFile);

  for (int idx = 1; idx < argc; idx++) {
    if (IS_OPTION(argv[idx])) {
      const char *ptr;
      float h;
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage(NULL);
        return EXIT_SUCCESS;
      case 'd':
        appstate.debugmode = true;
#       if defined _WIN32  /* fix console output on Windows */
          if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "wb", stdout);
            freopen("CONOUT$", "wb", stderr);
          }
          printf("\nBMFlash started\n");
#       endif
        break;
      case 'f':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        h = (float)strtod(ptr, (char**)&ptr);
        if (h >= 8.0)
          opt_fontsize = h;
        if (*ptr == ',') {
          char *mono;
          ptr++;
          if ((mono = strchr(ptr, ',')) != NULL)
            *mono++ = '\0';
          if (*ptr != '\0')
            strlcpy(opt_fontstd, ptr, sizearray(opt_fontstd));
          if (mono != NULL && *mono == '\0')
            strlcpy(opt_fontmono, mono, sizearray(opt_fontmono));
        }
        break;
      case 'v':
        version();
        return EXIT_SUCCESS;
      default:
        usage(argv[idx]);
        return EXIT_FAILURE;
      }
    } else {
      if (access(argv[idx], 0) == 0) {
        strlcpy(appstate.TargetFile, argv[idx], sizearray(appstate.TargetFile));
        appstate.load_status = 1;
      }
    }
  }
  if (strlen(appstate.TargetFile) == 0) {
    ini_gets("Session", "recent", "", appstate.TargetFile, sizearray(appstate.TargetFile), txtConfigFile);
    if (access(appstate.TargetFile, 0) == 0)
      appstate.load_status = 1;
    else
      appstate.TargetFile[0] = '\0';
  }

  strlcpy(appstate.ParamFile, appstate.TargetFile, sizearray(appstate.ParamFile));
  if (strlen(appstate.ParamFile) > 0)
    strlcat(appstate.ParamFile, ".bmcfg", sizearray(appstate.ParamFile));

  tcl_init(&appstate.tcl, &appstate);
  tcl_register(&appstate.tcl, "exec", tcl_cmd_exec, 0, 1, 1, NULL);
  tcl_register(&appstate.tcl, "puts", tcl_cmd_puts, 0, 1, 2, tcl_value("-nonewline", -1));
  tcl_register(&appstate.tcl, "syscmd", tcl_cmd_syscmd, 0, 1, 1, NULL);
  tcl_register(&appstate.tcl, "wait", tcl_cmd_wait, 0, 1, 3, NULL);
  rspreply_init();

  struct nk_context *ctx = guidriver_init("BlackMagic Flash Programmer",
                                          WINDOW_WIDTH, WINDOW_HEIGHT,
                                          GUIDRV_CENTER | GUIDRV_TIMER,
                                          opt_fontstd, opt_fontmono, opt_fontsize);
  nuklear_style(ctx);

  enum nk_collapse_states tab_states[TAB_COUNT];
  tab_states[TAB_OPTIONS] = NK_MINIMIZED;
  tab_states[TAB_SERIALIZATION] = NK_MINIMIZED;
  tab_states[TAB_STATUS] = NK_MAXIMIZED;

  bool help_active = false;
  int toolmenu_active = TOOL_CLOSE;
  int running = 1;
  while (running) {
    /* handle state */
    bool waitidle = handle_stateaction(&appstate, tab_states);

    /* handle user input */
    nk_input_begin(ctx);
    if (!guidriver_poll(waitidle)) /* if text was added to the log, don't wait in guidriver_poll(); system is NOT idle */
      running = 0;
    nk_input_end(ctx);

    /* other events */
    int dev_event = guidriver_monitor_usb(0x1d50, 0x6018);
    if (dev_event != 0) {
      if (dev_event == DEVICE_REMOVE)
        bmp_disconnect();
      SETSTATE(appstate, STATE_INIT); /* BMP was inserted or removed */
    }

    /* GUI */
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT), 0)) {
      bool error = panel_target(ctx, &appstate);

      float logview_height = LOGVIEW_HEIGHT;
      if (!error && appstate.TargetFileType == FILETYPE_UNKNOWN)
        logview_height -= ROW_HEIGHT + 2*SPACING;
      nk_layout_row_dynamic(ctx, logview_height, 1);
      if (nk_group_begin(ctx, "SubPanel", 0)) {
        panel_options(ctx, &appstate, tab_states);
        panel_serialize(ctx, &appstate, tab_states);

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Status", &tab_states[TAB_STATUS], NULL)) {
          nk_layout_row_dynamic(ctx, logview_height - 3.4 * ROW_HEIGHT - 8 *SPACING, 1);
          log_widget(ctx, "status", logtext, opt_fontsize, &loglines);

          nk_layout_row_dynamic(ctx, ROW_HEIGHT*0.4, 1);
          unsigned long progress_pos, progress_range;
          bmp_progress_get(&progress_pos, &progress_range);
          nk_size progress = progress_pos;
          nk_progress(ctx, &progress, progress_range, NK_FIXED);

          nk_tree_state_pop(ctx);

          tab_states[TAB_OPTIONS] = NK_MINIMIZED;
          tab_states[TAB_SERIALIZATION] = NK_MINIMIZED;
        }

        nk_group_end(ctx);
      }

      /* the options are best reloaded after handling other settings, but before
         handling the download action */
      if (appstate.load_status != 0) {
        /* check the type of the file (ELF/HEX/BIN) and whether NXP CRP
           signature is found */
        FILE *fp = fopen(appstate.TargetFile, "rb");
        if (fp != NULL) {
          int wordsize;
          int result = elf_info(fp, &wordsize, NULL, NULL, NULL);
          if (result == ELFERR_NONE && wordsize == 32)
            appstate.TargetFileType = FILETYPE_ELF;
          else if (hex_isvalid(fp))
            appstate.TargetFileType = FILETYPE_HEX;
          else
            appstate.TargetFileType = FILETYPE_UNKNOWN; /* BIN file */
          fclose(fp);
        } else {
          appstate.TargetFileType = FILETYPE_NONE;
        }
        /* reset options (load new options from file) */
        strlcpy(appstate.ParamFile, appstate.TargetFile, sizearray(appstate.ParamFile));
        if (strlen(appstate.ParamFile) > 0)
          strlcat(appstate.ParamFile, ".bmcfg", sizearray(appstate.ParamFile));
        if (load_targetparams(appstate.ParamFile, &appstate)) {
          if (appstate.load_status == 2)
            log_addstring("Changed target, settings loaded\n");
          else
            log_addstring("Settings for target loaded\n");
          appstate.set_probe_options = true;
          check_crp_level(&appstate);
        } else if (appstate.load_status == 2) {
          if (access(appstate.TargetFile, 0) != 0)
            log_addstring("^1Target file not found\n");
          else
            log_addstring("New target, please check settings\n");
        }
        appstate.load_status = 0;
      }

      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.4, 0.025, 0.30, 0.025, 0.25));
      if (appstate.isrunning_download != THRD_RUNNING && appstate.isrunning_tcl != THRD_RUNNING) {
        if (button_tooltip(ctx, "Download", NK_KEY_F5, appstate.curstate == STATE_IDLE, "Download ELF/HEX/BIN file into target (F5)")) {
          appstate.skip_download = false;  /* should already be false */
          SETSTATE(appstate, STATE_SAVE);  /* start the download sequence */
          if (appstate.debugmode) {
            printf("State: bmp_isopen()=%d, is_attached=%d, tpwr=%d\n", bmp_isopen(), appstate.is_attached, appstate.tpwr);
          }
        }
      } else {
        if (button_tooltip(ctx, "Abort", NK_KEY_COPY, nk_true, "Abort download / post-processing (Ctrl+C)")) {
          if (appstate.isrunning_download == THRD_RUNNING)
            appstate.isrunning_download = THRD_ABORT;
          if (appstate.isrunning_tcl == THRD_RUNNING)
            appstate.isrunning_tcl = THRD_ABORT;
        }
      }
      nk_spacing(ctx, 1);
      struct nk_rect rc_toolbutton = nk_widget_bounds(ctx);
      if (button_tooltip(ctx, "Tools", NK_KEY_NONE, appstate.curstate == STATE_IDLE, "Other commands"))
        toolmenu_active = TOOL_OPEN;
      nk_spacing(ctx, 1);
      if (nk_button_label(ctx, "Help") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F1)) {
        nk_input_clear_mousebuttons(ctx);
        help_active = true;
      }

      if (help_active)
        help_active = help_popup(ctx);

      if (toolmenu_active != TOOL_CLOSE) {
        toolmenu_active = tools_popup(ctx, &rc_toolbutton);
        switch (toolmenu_active) {
        case TOOL_RESCAN:
          SETSTATE(appstate, STATE_INIT);
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_VERIFY:
          appstate.skip_download = true;
          SETSTATE(appstate, STATE_SAVE);  /* start the pseudo-download sequence */
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_OPTIONERASE:
          SETSTATE(appstate, STATE_ERASE_OPTBYTES);
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_FULLERASE:
          SETSTATE(appstate, STATE_FULLERASE);
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_BLANKCHECK:
          SETSTATE(appstate, STATE_BLANKCHECK);
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_DUMPFLASH:
          SETSTATE(appstate, STATE_DUMPFLASH);
          toolmenu_active = TOOL_CLOSE;
          break;
        }
      }
    }
    nk_end(ctx);

    /* Draw */
    guidriver_render(COLOUR_BG0_S);
  }

  if (appstate.tpwr && bmp_isopen())
    bmp_monitor("tpwr disable");
  if (appstate.is_attached)
    bmp_detach(false);    /* if currently attached, detach */
  bmp_disconnect();

  if (strlen(appstate.ParamFile) > 0)
    save_targetparams(appstate.ParamFile, &appstate);
  ini_putf("Settings", "fontsize", opt_fontsize, txtConfigFile);
  ini_puts("Settings", "fontstd", opt_fontstd, txtConfigFile);
  ini_puts("Settings", "fontmono", opt_fontmono, txtConfigFile);
  if (strlen(txtConfigFile) > 0)
    ini_puts("Session", "recent", appstate.TargetFile, txtConfigFile);
  if (bmp_is_ip_address(appstate.IPaddr))
    ini_puts("Settings", "ip-address", appstate.IPaddr, txtConfigFile);
  ini_putl("Settings", "appstate.probe", (appstate.probe == appstate.netprobe) ? 99 : appstate.probe, txtConfigFile);

  clear_probelist(appstate.probelist, appstate.netprobe);
  tcl_destroy(&appstate.tcl);
  rspreply_clear();
  guidriver_close();
  bmscript_clear();
  gdbrsp_packetsize(0);
  tcpip_cleanup();
  nk_guide_cleanup();
  if (appstate.monitor_cmds != NULL)
    free((void*)appstate.monitor_cmds);
  if (logtext != NULL)
    free(logtext);
# if defined FORTIFY
    Fortify_CheckAllMemory();
    Fortify_ListAllMemory();
# endif
  return EXIT_SUCCESS;
}

