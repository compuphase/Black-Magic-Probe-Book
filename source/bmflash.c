/*
 * Utility to download executable programs to the target micro-controller via
 * the Black Magic Probe on a system. This utility is built with Nuklear for a
 * cross-platform GUI.
 *
 * Copyright 2019-2022 CompuPhase
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
  #define STRICT
  #define WIN32_LEAN_AND_MEAN
  #define _WIN32_WINNT   0x0500 /* for AttachConsole() */
  #include <windows.h>
  #include <shellapi.h>
  #include <direct.h>
  #include <io.h>
  #include <process.h>	/* for spawn() */
  #if defined __MINGW32__ || defined __MINGW64__
    #include <sys/stat.h>
    #include "strlcpy.h"
  #elif defined _MSC_VER
    #include <sys/stat.h>
    #include "strlcpy.h"
    #define stat _stat
    #define access(p,m)       _access((p),(m))
    #define mkdir(p)          _mkdir(p)
    #define stricmp(s1,s2)    _stricmp((s1),(s2))
  #endif
#elif defined __linux__
  #include <unistd.h>
  #include <bsd/string.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/wait.h>
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
#include "noc_file_dialog.h"
#include "nuklear_guide.h"
#include "nuklear_mousepointer.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"
#include "bmcommon.h"
#include "bmp-scan.h"
#include "bmp-script.h"
#include "bmp-support.h"
#include "cksum.h"
#include "elf.h"
#include "gdb-rsp.h"
#include "ident.h"
#include "minIni.h"
#include "picoro.h"
#include "rs232.h"
#include "tcpip.h"
#include "specialfolder.h"
#include "svnrev.h"

#if defined __linux__ || defined __unix__
  #include "res/icon_download_64.h"
#endif

#if !defined _MAX_PATH
  #define _MAX_PATH 260
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif
#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if defined WIN32 || defined _WIN32
  #define DIRSEP_CHAR '\\'
  #define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
  #define DIRSEP_CHAR '/'
  #define IS_OPTION(s)  ((s)[0] == '-')
#endif


#define FONT_HEIGHT     14              /* default font size */
#define WINDOW_WIDTH    (34 * opt_fontsize)
#define WINDOW_HEIGHT   (26 * opt_fontsize)
#define ROW_HEIGHT      (2 * opt_fontsize)
#define COMBOROW_CY     (0.9 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.5 * opt_fontsize)
#define LOGVIEW_ROWS    6

static float opt_fontsize = FONT_HEIGHT;


/* log_addstring() adds a string to the log data; the parameter "text" may be NULL
   to return the current log string without adding new data to it */
static char *logtext = NULL;
static unsigned loglines = 0;

static char *log_addstring(const char *text)
{
  int len = 0;
  char *buf;

  if (text == NULL || strlen(text) == 0)
    return logtext;

  if (logtext != NULL)
    len += strlen(logtext);
  len += strlen(text) + 1;  /* +1 for the \0 */
  buf = malloc(len * sizeof(char));
  if (buf == NULL)
    return logtext;

  *buf = '\0';
  if (logtext != NULL)
    strcat(buf, logtext);
  strcat(buf, text);

  if (logtext != NULL)
    free(logtext);
  logtext = buf;
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

static int copyfile(FILE *fdest, FILE *fsrc)
{
  unsigned char *buffer;
  size_t filesize, bytes, written;

  assert(fdest != NULL && fsrc != NULL);
  fseek(fsrc, 0, SEEK_END);
  filesize = ftell(fsrc);
  buffer = malloc(filesize);
  if (buffer == 0) {
    log_addstring("^1Memory allocation error\n");
    return 0;
  }

  fseek(fsrc, 0, SEEK_SET);
  bytes = fread(buffer, 1, filesize, fsrc);
  fseek(fdest, 0, SEEK_SET);
  written = fwrite(buffer, 1, bytes, fdest);
  fseek(fsrc, 0, SEEK_SET);
  fseek(fdest, 0, SEEK_SET);
  free(buffer);

  if (bytes != filesize || written != bytes) {
    log_addstring("^1Failed to create work copy of ELF file\n");
    return 0;
  }

  return 1;
}

static int patch_vecttable(FILE *fp, const char *mcutype)
{
  char msg[100];
  unsigned int chksum;
  int err = elf_patch_vecttable(fp, mcutype, &chksum);
  switch (err) {
  case ELFERR_NONE:
    sprintf(msg, "Checksum adjusted to %08x\n", chksum);
    log_addstring(msg);
    break;
  case ELFERR_CHKSUMSET:
    sprintf(msg, "Checksum already correct (%08x)\n", chksum);
    log_addstring(msg);
    break;
  case ELFERR_UNKNOWNDRIVER:
    log_addstring("^1Unsupported MCU type (internal error)\n");
    return 0;
  case ELFERR_FILEFORMAT:
    log_addstring("^1Not a 32-bit ELF file\n");
    return 0;
  }
  return 1;
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

static int serialize_address(FILE *fp, const char *section, unsigned long address,
                             unsigned char *data, int datasize)
{
  unsigned long offset;

  /* find section, if provided */
  assert(fp != NULL);
  if (strlen(section) > 0) {
    unsigned long length;
    int err = elf_section_by_name(fp, section, &offset, NULL, &length);
    if (err == ELFERR_NOMATCH) {
      log_addstring("^1Serialization section not found\n");
      return 0;
    } else if (address + datasize > length) {
      log_addstring("^1Serialization address exceeds section\n");
      return 0;
    }
  } else {
    offset = 0;
  }

  assert(data != NULL);
  assert(datasize != 0);
  fseek(fp, offset + address, SEEK_SET);
  fwrite(data, 1, datasize, fp);
  fseek(fp, 0, SEEK_SET);

  return 1;
}

static int serialize_match(FILE *fp, const char *match, const char *prefix,
                           unsigned char *data, int datasize)
{
  unsigned char matchbuf[100], prefixbuf[100];
  size_t matchbuf_len, prefixbuf_len;
  unsigned char *buffer;
  size_t bytes;
  size_t fileoffs, filesize;

  /* create buffer to match from the string */
  assert(match != NULL);
  matchbuf_len = serialize_parsepattern(matchbuf, sizearray(matchbuf), match, "match");
  assert(prefix != NULL);
  prefixbuf_len = serialize_parsepattern(prefixbuf, sizearray(prefixbuf), prefix, "prefix");
  if (matchbuf_len == (size_t)~0 || prefixbuf_len == (size_t)~0)
    return 0; /* error message already given */
  if (matchbuf_len == 0) {
    log_addstring("^1Serialization match text is empty\n");
    return 0;
  }

  /* find the buffer in the file */
  assert(fp != NULL);
  fseek(fp, 0, SEEK_END);
  filesize = ftell(fp);
  buffer = malloc(filesize);
  if (buffer == 0) {
    log_addstring("^1Memory allocation error\n");
    return 0;
  }
  fseek(fp, 0, SEEK_SET);
  bytes = fread(buffer, 1, filesize, fp);
  for (fileoffs = 0; fileoffs < bytes - matchbuf_len; fileoffs++) {
    if (buffer[fileoffs] == matchbuf[0] && memcmp(buffer + fileoffs, matchbuf, matchbuf_len) == 0)
      break;
  }
  free(buffer);
  if (fileoffs >= bytes - matchbuf_len) {
    log_addstring("^1Match string not found\n");
    return 0;
  }

  /* patch the prefix string and serial data at the position where the match was found */
  fseek(fp, fileoffs, SEEK_SET);
  fwrite(prefixbuf, 1, prefixbuf_len, fp);
  fwrite(data, 1, datasize, fp);
  fseek(fp, 0, SEEK_SET);

  return 1;
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
  time_t timestamp;
  struct stat fstat;
  int addheader;

  line[0] = '\0';

  /* current date/time */
  timestamp = time(NULL);
  strftime(substr, sizeof(substr), "%Y-%m-%d %H:%M:%S, ", localtime(&timestamp));
  strlcat(line, substr, sizearray(line));

  /* ELF file date/time */
  stat(filename, &fstat);
  strftime(substr, sizeof(substr), "%Y-%m-%d %H:%M:%S, ", localtime(&fstat.st_mtime));
  strlcat(line, substr, sizearray(line));

  /* ELF file size */
  sprintf(substr, "%ld, ", fstat.st_size);
  strlcat(line, substr, sizearray(line));

  /* ELF file CRC32 */
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

  /* RCS identification string in the ELF file */
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
  #if defined _WIN32  /* fix console output on Windows */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      freopen("CONOUT$", "wb", stdout);
      freopen("CONOUT$", "wb", stderr);
    }
    printf("\n");
  #endif

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
  #if defined _WIN32  /* fix console output on Windows */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      freopen("CONOUT$", "wb", stdout);
      freopen("CONOUT$", "wb", stderr);
    }
    printf("\n");
  #endif

  printf("BMFlash version %s.\n", SVNREV_STR);
  printf("Copyright 2019-2022 CompuPhase\nLicensed under the Apache License version 2.0\n");
}

static bool help_popup(struct nk_context *ctx)
{
  #include "bmflash_help.h"
  (void)bmflash_helpsize;

  struct nk_rect rc = nk_window_get_bounds(ctx);
  #define MARGIN  10
  rc.x += MARGIN;
  rc.y += MARGIN;
  rc.w -= 2*MARGIN;
  rc.h -= 2*MARGIN;
  #undef MARGIN
  return nk_guide(ctx, &rc, opt_fontsize, (const char*)bmflash_help, NULL);
}


enum {
  TOOL_OPEN = -1,
  TOOL_CLOSE,
  TOOL_RESCAN,
  TOOL_FULLERASE,
  TOOL_OPTIONERASE,
  TOOL_STM32PROTECT,
  TOOL_VERIFY,
};

static int tools_popup(struct nk_context *ctx, const struct nk_rect *anchor_button)
{
  #define MENUROWHEIGHT (1.5 * opt_fontsize)
  #define MARGIN        4
  static int prev_active = TOOL_CLOSE;
  int is_active = TOOL_OPEN;
  struct nk_rect rc;
  float height = 4 * MENUROWHEIGHT + 2 * MARGIN;
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
    if (nk_button_label_styled(ctx, &stbtn, "Full Flash Erase"))
      is_active = TOOL_FULLERASE;
    if (nk_button_label_styled(ctx, &stbtn, "Erase Option Bytes"))
      is_active = TOOL_OPTIONERASE;
    if (nk_button_label_styled(ctx, &stbtn, "Set CRP Option"))
      is_active = TOOL_STM32PROTECT;
    if (nk_button_label_styled(ctx, &stbtn, "Verify Download"))
      is_active = TOOL_VERIFY;
    //??? enhancement: add "Blank Check" (or make that a special result of "Verify Download")
    if (is_active != TOOL_OPEN)
      nk_popup_close(ctx);
    nk_popup_end(ctx);
  } else {
    is_active = TOOL_CLOSE;
  }
  #undef MENUROWHEIGHT
  #undef MARGIN
  stwin->spacing = item_spacing;
  prev_active = is_active;
  return is_active;
}

typedef struct tagAPPSTATE {
  int curstate;                 /**< current state */
  int is_attached;              /**< is debug probe attached? */
  int probe;                    /**< selected debug probe (index) */
  int netprobe;                 /**< index for the IP address (pseudo-probe) */
  const char **probelist;       /**< list of detected probes */
  int architecture;             /**< MCU architecture (index) */
  nk_bool tpwr;                 /**< option: tpwr (target power) */
  nk_bool fullerase;            /**< option: erase entire flash before download */
  nk_bool connect_srst;         /**< option: keep in reset during connect */
  nk_bool write_log;            /**< option: record downloads in log file */
  nk_bool print_time;           /**< option: print download time */
  int skip_download;            /**< do download+verify procedure without actually downloading */
  char IPaddr[64];              /**< IP address for network probe */
  char PostProcess[_MAX_PATH];  /**< path to post-process program */
  int serialize;                /**< serialization option */
  int SerialFmt;                /**< serialization: format */
  char Section[32];             /**< serialization: name of the ELF section */
  char Address[32];             /**< serialization: relative address in section */
  char Match[64];               /**< serialization: match string  */
  char Prefix[64];              /**< serialization: prefix string for "replace" */
  char Serial[32];              /**< serialization: serial number */
  char SerialSize[32];          /**< serialization: size (in bytes of characters) */
  char SerialIncr[32];          /**< serialization: increment */
  char ELFfile[_MAX_PATH];      /**< ELF path/filename (target) */
  char ParamFile[_MAX_PATH];    /**< configuration file for the target */
  char SerialFile[_MAX_PATH];   /**< optional file for serialization settings */
  FILE *fpTgt;                  /**< target file */
  FILE *fpWork;                 /**< intermediate work file */
  coro coro_download;           /**< co-routine handle */
  int coro_result;              /**< success/failure state of the download */
  clock_t tstamp_start;         /**< time-stamp of start of download procedure */
} APPSTATE;

enum {
  TAB_OPTIONS,
  TAB_SERIALIZATION,
  TAB_STATUS,
  /* --- */
  TAB_COUNT
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
  STATE_PATCH_ELF,
  STATE_CLEARFLASH,
  STATE_DOWNLOAD,
  STATE_VERIFY,
  STATE_FINISH,
  STATE_ERASE_OPTBYTES,
  STATE_SET_CRP,
  STATE_FULLERASE,
};

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

static const char *architectures[] = { "Standard", "LPC8xx", "LPC11xx", "LPC15xx",
                                       "LPC17xx", "LPC21xx", "LPC22xx", "LPC23xx",
                                       "LPC24xx", "LPC43xx" };

static bool load_targetparams(const char *filename, APPSTATE *state)
{
  assert(filename != NULL);
  assert(state != NULL);
  if (access(filename, 0) != 0)
    return false;

  state->connect_srst = (nk_bool)ini_getl("Settings", "connect-srst", 0, filename);
  state->write_log = (nk_bool)ini_getl("Settings", "write-log", 0, filename);
  state->print_time = (nk_bool)ini_getl("Settings", "print-time", 0, filename);

  char field[_MAX_PATH];
  ini_gets("Flash", "architecture", "", field, sizearray(field), filename);
  for (state->architecture = 0; state->architecture < sizearray(architectures); state->architecture++)
    if (architecture_match(architectures[state->architecture], field))
      break;
  if (state->architecture >= sizearray(architectures))
    state->architecture = 0;
  state->tpwr = (int)ini_getl("Flash", "tpwr", 0, filename);
  state->fullerase = (int)ini_getl("Flash", "full-erase", 0, filename);
  ini_gets("Flash", "postprocess", "", state->PostProcess, sizearray(state->PostProcess), filename);

  strlcpy(state->SerialFile, filename, sizearray(state->SerialFile));
  ini_gets("Serialize", "file", "", state->SerialFile, sizearray(state->SerialFile), filename);
  char serialfile[_MAX_PATH];
  strlcpy(serialfile, filename, sizearray(serialfile));
  if (strlen(state->SerialFile) > 0)
    getpath(serialfile, sizearray(serialfile), state->SerialFile, filename);

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

  ini_putl("Settings", "connect-srst", state->connect_srst, filename);
  ini_putl("Settings", "write-log", state->write_log, filename);
  ini_putl("Settings", "print-time", state->print_time, filename);

  char field[200] = "";
  if (state->architecture > 0 && state->architecture < sizearray(architectures))
    strcpy(field, architectures[state->architecture]);
  ini_puts("Flash", "architecture", field, filename);
  ini_putl("Flash", "tpwr", state->tpwr, filename);
  ini_putl("Flash", "full-erase", state->fullerase, filename);
  ini_puts("Flash", "postprocess", state->PostProcess, filename);

  ini_puts("Serialize", "file", state->SerialFile, filename);
  char serialfile[_MAX_PATH];
  strlcpy(serialfile, filename, sizearray(serialfile));
  if (strlen(state->SerialFile) > 0)
    getpath(serialfile, sizearray(serialfile), state->SerialFile, filename);
  ini_putl("Serialize", "option", state->serialize, serialfile);
  sprintf(field, "%s:%s", state->Section, state->Address);
  ini_puts("Serialize", "address", field, serialfile);
  sprintf(field, "%s:%s", state->Match, state->Prefix);
  ini_puts("Serialize", "match", field, serialfile);
  sprintf(field, "%s:%s:%d:%s", state->Serial, state->SerialSize, state->SerialFmt, state->SerialIncr);
  ini_puts("Serialize", "serial", field, serialfile);

  return true;
}

static void panel_options(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT])
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_states != NULL);
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Options", &tab_states[TAB_OPTIONS])) {
    struct nk_rect rcwidget;
    assert(state->probelist != NULL);
    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT * 0.8, 2, nk_ratio(2, 0.45, 0.55));
    nk_label(ctx, "Black Magic Probe", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    rcwidget = nk_widget_bounds(ctx);
    state->probe = nk_combo(ctx, state->probelist, state->netprobe+1, state->probe,
                            (int)COMBOROW_CY, nk_vec2(rcwidget.w, 4.5*ROW_HEIGHT));
    if (state->probe == state->netprobe) {
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(4, 0.05, 0.40, 0.49, 0.06));
      nk_spacing(ctx, 1);
      nk_label(ctx, "IP Address", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_flags result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                                       state->IPaddr, sizearray(state->IPaddr),
                                                       nk_filter_ascii);
      int reconnect = ((result & NK_EDIT_COMMITED) != 0 && bmp_is_ip_address(state->IPaddr));
      if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIPLE_DOT, NK_KEY_NONE, nk_true, "Scan network for ctxLink probes.")) {
        #if defined WIN32 || defined _WIN32
          HCURSOR hcur = SetCursor(LoadCursor(NULL, IDC_WAIT));
        #endif
        unsigned long addr;
        int count = scan_network(&addr, 1);
        #if defined WIN32 || defined _WIN32
          SetCursor(hcur);
        #endif
        if (count == 1) {
          sprintf(state->IPaddr, "%lu.%lu.%lu.%lu",
                 addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >> 24) & 0xff);
          reconnect = 1;
        } else {
          strlcpy(state->IPaddr, "no gdbserver found", sizearray(state->IPaddr));
        }
      }
      if (reconnect) {
        bmp_disconnect();
        bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
        state->curstate = STATE_IDLE;
      }
    }

    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT * 0.8, 2, nk_ratio(2, 0.45, 0.55));
    nk_label(ctx, "MCU Family", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    rcwidget = nk_widget_bounds(ctx);
    state->architecture = nk_combo(ctx, architectures, NK_LEN(architectures), state->architecture,
                                   (int)COMBOROW_CY, nk_vec2(rcwidget.w, 4.5*ROW_HEIGHT));

    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 3, nk_ratio(3, 0.45, 0.497, 0.053));
    nk_label(ctx, "Post-process", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    editctrl_tooltip(ctx, NK_EDIT_FIELD,
                     state->PostProcess, sizearray(state->PostProcess),
                     nk_filter_ascii, "Program/script to run after successful download");
    if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIPLE_DOT, NK_KEY_NONE, nk_true, "Browse...")) {
      #if defined _WIN32
        const char *filter = "Executables\0*.exe\0All files\0*.*\0";
      #else
        const char *filter = "Executables\0*\0All files\0*\0";
      #endif
      noc_file_dialog_open(state->PostProcess, sizearray(state->PostProcess),
                           NOC_FILE_DIALOG_OPEN, filter,
                           NULL, state->PostProcess,
                           "Select Executable", guidriver_apphandle());
    }

    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    checkbox_tooltip(ctx, "Power Target (3.3V)", &state->tpwr, NK_TEXT_LEFT,
                     "Let the debug probe provide power to the target");
    checkbox_tooltip(ctx, "Full Flash Erase before download", &state->fullerase, NK_TEXT_LEFT,
                     "Erase entire Flash memory, instead of only sectors that are overwritten");
    checkbox_tooltip(ctx, "Reset Target during connect", &state->connect_srst, NK_TEXT_LEFT,
                     "Keep target MCU reset while debug probe attaches");
    checkbox_tooltip(ctx, "Keep Log of downloads", &state->write_log, NK_TEXT_LEFT,
                     "Write successful downloads to a log file");
    checkbox_tooltip(ctx, "Print Download Time", &state->print_time, NK_TEXT_LEFT,
                     "Print how long the download took upon completion");

    nk_tree_state_pop(ctx);
  }
}

static void panel_serialize(struct nk_context *ctx, APPSTATE *state,
                            enum nk_collapse_states tab_states[TAB_COUNT])
{
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Serialization", &tab_states[TAB_SERIALIZATION])) {
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (nk_option_label(ctx, "No serialization", (state->serialize == SER_NONE), NK_TEXT_LEFT))
      state->serialize = SER_NONE;
    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(4, 0.25, 0.3, 0.15, 0.3));
    if (nk_option_label(ctx, "Address", (state->serialize == SER_ADDRESS), NK_TEXT_LEFT))
      state->serialize = SER_ADDRESS;
    editctrl_tooltip(ctx, NK_EDIT_FIELD, state->Section, sizearray(state->Section),
                     nk_filter_ascii, "The name of the section in the ELF file");
    nk_label(ctx, "offset", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
    editctrl_tooltip(ctx, NK_EDIT_FIELD, state->Address, sizearray(state->Address),
                     nk_filter_hex, "The offset in hexadecimal");
    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(4, 0.25, 0.3, 0.15, 0.3));
    if (nk_option_label(ctx, "Match", (state->serialize == SER_MATCH), NK_TEXT_LEFT))
      state->serialize = SER_MATCH;
    editctrl_tooltip(ctx, NK_EDIT_FIELD, state->Match, sizearray(state->Match),
                     nk_filter_ascii, "The text to match");
    nk_label(ctx, "prefix", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
    editctrl_tooltip(ctx, NK_EDIT_FIELD, state->Prefix, sizearray(state->Prefix),
                     nk_filter_ascii, "Text to write back at the matched position, prefixing the serial number");
    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.05, 0.193, 0.3, 0.155, 0.3));
    nk_spacing(ctx, 1);
    nk_label(ctx, "Serial", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    editctrl_tooltip(ctx, NK_EDIT_FIELD, state->Serial, sizearray(state->Serial),
                     nk_filter_decimal, "The serial number to write (decimal value)");
    nk_label(ctx, "size", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
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
    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.05, 0.193, 0.25, 0.5));
    nk_spacing(ctx, 1);
    nk_label(ctx, "Increment", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    editctrl_tooltip(ctx, NK_EDIT_FIELD, state->SerialIncr, sizearray(state->SerialIncr),
                     nk_filter_decimal, "The increment for the serial number");
    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(5, 0.05, 0.19, 0.75));
    nk_spacing(ctx, 1);
    nk_label(ctx, "File", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    editctrl_tooltip(ctx, NK_EDIT_FIELD, state->SerialFile, sizearray(state->SerialFile),
                     nk_filter_ascii, "The file to store the serialization settings in\nLeave empty to use the local configuration file");
    nk_tree_state_pop(ctx);
  }
}

static int handle_stateaction(APPSTATE *state, enum nk_collapse_states tab_states[TAB_COUNT])
{
  assert(state != NULL);

  int waitidle = 1;
  int result;

  switch (state->curstate) {
  case STATE_INIT:
    /* collect debug probes, connect to the selected one */
    state->probelist = get_probelist(&state->probe, &state->netprobe);
    tcpip_init();
    bmp_setcallback(bmp_callback);
    bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
    bmp_progress_reset(0);
    state->curstate = STATE_IDLE;
    waitidle = 0;
    break;

  case STATE_IDLE:
    if (state->fpTgt != NULL) {
      fclose(state->fpTgt);
      state->fpTgt = NULL;
    }
    if (state->fpWork != NULL) {
      fclose(state->fpWork);
      state->fpWork = NULL;
    }
    if (state->is_attached) {
      bmp_detach(1);  /* if currently attached, detach */
      state->is_attached = 0;
    }
    gdbrsp_clear();
    state->skip_download = 0;
    break;

  case STATE_SAVE:
    tab_states[TAB_OPTIONS] = NK_MINIMIZED;
    tab_states[TAB_SERIALIZATION] = NK_MINIMIZED;
    tab_states[TAB_STATUS] = NK_MAXIMIZED;
    if (access(state->ELFfile, 0) == 0) {
      /* save settings in cache file */
      strlcpy(state->ParamFile, state->ELFfile, sizearray(state->ParamFile));
      strlcat(state->ParamFile, ".bmcfg", sizearray(state->ParamFile));
      save_targetparams(state->ParamFile, state);
      state->curstate = STATE_ATTACH;
      state->tstamp_start = clock();
    } else {
      log_addstring("^1Failed to open the ELF file\n");
      state->curstate = STATE_IDLE;
    }
    waitidle = 0;
    break;

  case STATE_ATTACH:
    bmp_progress_reset(0);
    result = bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
    if (result) {
      char mcufamily[32];
      state->is_attached = bmp_attach(state->tpwr, state->connect_srst, mcufamily, sizearray(mcufamily), NULL, 0);
      if (state->is_attached) {
        /* check for particular architectures, try exact match first */
        int arch;
        for (arch = 0; arch < sizearray(architectures); arch++)
          if (architecture_match(architectures[arch], mcufamily))
            break;
        if (arch >= sizearray(architectures)) {
          /* try prefix match */
          for (arch = 0; arch < sizearray(architectures); arch++) {
            int len = strlen(architectures[arch]);
            char pattern[32];
            strcpy(pattern, mcufamily);
            pattern[len] = '\0';
            if (architecture_match(architectures[arch], pattern))
              break;
          }
        }
        if (arch >= sizearray(architectures))
          arch = 0;
        if (arch != state->architecture) {
          char msg[128];
          sprintf(msg, "^3Detected MCU family %s (check options)\n", architectures[arch]);
          log_addstring(msg);
        }
      }
      if (bmp_flashtotal() == 0)
        result = 0; /* no use downloading firmware to a chip that has no Flash */
    }
    state->curstate = (result && state->is_attached) ? STATE_PRE_DOWNLOAD : STATE_IDLE;
    waitidle = 0;
    break;

  case STATE_PRE_DOWNLOAD:
    /* open the working file */
    state->fpTgt = fopen(state->ELFfile, "rb");
    if (state->fpTgt == NULL) {
      log_addstring("^1Failed to load the target file\n");
      state->curstate = STATE_IDLE;
    } else {
      state->curstate = STATE_PATCH_ELF;
    }
    waitidle = 0;
    break;

  case STATE_PATCH_ELF:
    /* verify whether to patch the ELF file (create a temporary file) */
    if (state->architecture > 0 || state->serialize != SER_NONE) {
      assert(state->fpTgt != NULL);
      state->fpWork = tmpfile();
      if (state->fpWork == NULL) {
        log_addstring("^1Failed to process the target file\n");
        state->curstate = STATE_IDLE;
        waitidle = 0;
        break;
      }
      result = copyfile(state->fpWork, state->fpTgt);
      if (result && state->architecture > 0)
        result = patch_vecttable(state->fpWork, architectures[state->architecture]);
      if (result && state->serialize != SER_NONE) {
        /* create replacement buffer, depending on format */
        unsigned char data[50];
        int datasize = (int)strtol(state->SerialSize, NULL, 10);
        serialize_fmtoutput(data, datasize, serial_get(state->Serial), state->SerialFmt);
        //??? enhancement: run a script, because the serial number may include a check digit
        if (state->serialize == SER_ADDRESS)
          result = serialize_address(state->fpWork, state->Section, strtoul(state->Address,NULL,16), data, datasize);
        else if (state->serialize == SER_MATCH)
          result = serialize_match(state->fpWork, state->Match, state->Prefix, data, datasize);
        if (result) {
          char msg[100];
          sprintf(msg, "^4Serial adjusted to %d\n", serial_get(state->Serial));
          log_addstring(msg);
        }
      }
      state->curstate = result ? STATE_CLEARFLASH : STATE_IDLE;
    } else {
      state->curstate = STATE_CLEARFLASH;
    }
    waitidle = 0;
    break;

  case STATE_CLEARFLASH:
    if (!state->skip_download && state->fullerase) {
      if (state->architecture > 0)
        bmp_runscript("memremap", architectures[state->architecture], NULL, NULL, 0);
      result = bmp_fullerase();
      state->curstate = result ? STATE_DOWNLOAD : STATE_IDLE;
    } else {
      state->curstate = STATE_DOWNLOAD;
    }
    waitidle = 0;
    break;

  case STATE_DOWNLOAD:
    /* download to target */
    if (!state->skip_download) {
      if (state->architecture > 0 && state->coro_download == NULL)
        bmp_runscript("memremap", architectures[state->architecture], NULL, NULL, 0);
      /* create a coroutine for the function that does the download, so that
         this loop continues with updating the message log, while the download
         is in progress */
      if (state->coro_download == NULL) {
        state->coro_download = coroutine((coro_proc)bmp_download);
        state->coro_result = 0; /* preset for the case that the resumable() fails */
      }
      if (state->coro_download != NULL && resumable(state->coro_download)) {
        pointer_setstyle(CURSOR_WAIT);
        state->coro_result = (intptr_t)resume(state->coro_download,
                                              (state->fpWork != NULL) ? state->fpWork : state->fpTgt);
        if (state->coro_result == 0) {
          state->coro_download = NULL;
          state->curstate = STATE_IDLE;
        }
      } else {
        pointer_setstyle(CURSOR_NORMAL);
        state->coro_download = NULL;
        state->curstate = state->coro_result ? STATE_VERIFY : STATE_IDLE;
      }
    } else {
      state->curstate = STATE_VERIFY;
    }
    waitidle = 0;
    break;

  case STATE_VERIFY:
    if (state->architecture > 0) {
      /* check whether CRP was set; if so, verification will always fail */
      assert(state->fpWork != NULL);
      int crp;
      result = elf_check_crp(state->fpWork, &crp);
      if (result == ELFERR_NONE && crp > 0 && crp < 4) {
        /* CRP level set on the ELF file; it may still be that the code in
           the target does not have CRP set, but regardless, it won't match
           the code in the file */
        char msg[100];
        sprintf(msg, "^3Code Read Protection (CRP%d) is set\n", crp);
        log_addstring(msg);
      }
    }
    /* compare the checksum of Flash memory to the file */
    if (state->architecture > 0)
      bmp_runscript("memremap", architectures[state->architecture], NULL, NULL, 0);
    result = bmp_verify((state->fpWork != NULL)? state->fpWork : state->fpTgt);
    state->curstate = result ? STATE_FINISH : STATE_IDLE;
    if (result && state->print_time) {
      char msg[100];
      clock_t tstamp_stop = clock();
      sprintf(msg, "Completed in %.1f seconds\n", (double)(tstamp_stop - state->tstamp_start) / CLOCKS_PER_SEC);
      log_addstring(msg);
    }
    waitidle = 0;
    break;

  case STATE_FINISH:
    /* optionally log the download */
    if (state->write_log && !writelog(state->ELFfile, (state->serialize != SER_NONE) ? state->Serial : NULL))
      log_addstring("^3Failed to write to log file\n");
    /* optionally perform a post-processing step */
    if (strlen(state->PostProcess) > 0) {
      #if defined WIN32 || defined _WIN32
        if (state->serialize != SER_NONE)
          result = spawnlp(P_WAIT, state->PostProcess, state->PostProcess, state->ELFfile, state->Serial, NULL);
        else
          result = spawnlp(P_WAIT, state->PostProcess, state->PostProcess, state->ELFfile, NULL);
      #elif defined __linux__
        pid_t pid = fork();
        if (pid > 0) {
          int status; /* wait for prost-process to finish */
          waitpid(pid, &status, 0);
          if (WIFEXITED(status))
            result = WEXITSTATUS(status);
          else
            result = -1;
        } else {
          if (pid == 0) {
            if (state->serialize != SER_NONE)
              execlp(state->PostProcess, state->PostProcess, state->ELFfile, state->Serial, NULL);
            else
              execlp(state->PostProcess, state->PostProcess, state->ELFfile, NULL);
          }
          _exit(EXIT_FAILURE); /* this point is only reached on error, because execlp() does not return */
        }
      #endif
      if (result < 0) {
        log_addstring("^1Failed to run the post-processing program\n");
      } else if (result > 0) {
        char msg[100];
        sprintf(msg, "^3Post-processing program retuns %d\n", result);
        log_addstring(msg);
      } else {
        log_addstring("Post-processing finished\n");
      }
    }
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
    state->curstate = STATE_IDLE;
    waitidle = 0;
    break;

  case STATE_ERASE_OPTBYTES:
    bmp_progress_reset(0);
    if (bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL)
        && bmp_attach(state->tpwr, state->connect_srst, NULL, 0, NULL, 0))
    {
      state->is_attached = 1;
      result = bmp_monitor("option erase");
      if (!result)
        log_addstring("^1Failed to erase the option bytes\n");
    }
    state->curstate = STATE_IDLE;
    waitidle = 0;
    break;

  case STATE_SET_CRP:
    bmp_progress_reset(0);
    if (bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL)
        && bmp_attach(state->tpwr, state->connect_srst, NULL, 0, NULL, 0))
    {
      state->is_attached = 1;
      result = bmp_monitor("option option 0x1ffff800 0x00ff");
      if (!result)
        log_addstring("^1Failed to set the option byte for CRP\n");
    }
    state->curstate = STATE_IDLE;
    waitidle = 0;
    break;

  case STATE_FULLERASE:
    bmp_progress_reset(0);
    if (bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL)
        && bmp_attach(state->tpwr, state->connect_srst, NULL, 0, NULL, 0))
    {
      state->is_attached = 1;
      if (state->architecture > 0)
        bmp_runscript("memremap", architectures[state->architecture], NULL, NULL, 0);
      bmp_fullerase();
    }
    state->curstate = STATE_IDLE;
    waitidle = 0;
    break;
  }

  return waitidle;
}

int main(int argc, char *argv[])
{
  struct nk_context *ctx;
  enum nk_collapse_states tab_states[TAB_COUNT];
  APPSTATE appstate;
  int idx;
  char txtConfigFile[_MAX_PATH];
  bool help_active = false;
  int toolmenu_active = TOOL_CLOSE;
  int load_options = 0;
  char opt_fontstd[64] = "", opt_fontmono[64] = "";

  /* global defaults */
  memset(&appstate, 0, sizeof appstate);
  appstate.curstate = STATE_INIT;
  appstate.serialize = SER_NONE;
  appstate.SerialFmt = FMT_BIN;
  strcpy(appstate.Section, ".text");
  strcpy(appstate.Address, "0");
  strcpy(appstate.Serial, "1");
  strcpy(appstate.SerialSize, "4");
  strcpy(appstate.SerialIncr, "1");

  /* read defaults from the configuration file */
  get_configfile(txtConfigFile, sizearray(txtConfigFile), "bmflash.ini");
  appstate.probe = (int)ini_getl("Settings", "probe", 0, txtConfigFile);
  ini_gets("Settings", "ip-address", "127.0.0.1", appstate.IPaddr, sizearray(appstate.IPaddr), txtConfigFile);
  opt_fontsize = ini_getf("Settings", "fontsize", FONT_HEIGHT, txtConfigFile);
  ini_gets("Settings", "fontstd", "", opt_fontstd, sizearray(opt_fontstd), txtConfigFile);
  ini_gets("Settings", "fontmono", "", opt_fontmono, sizearray(opt_fontmono), txtConfigFile);

  for (idx = 1; idx < argc; idx++) {
    if (IS_OPTION(argv[idx])) {
      const char *ptr;
      float h;
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage(NULL);
        return EXIT_SUCCESS;
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
        strlcpy(appstate.ELFfile, argv[idx], sizearray(appstate.ELFfile));
        load_options = 1;
      }
    }
  }
  if (strlen(appstate.ELFfile) == 0) {
    ini_gets("Session", "recent", "", appstate.ELFfile, sizearray(appstate.ELFfile), txtConfigFile);
    if (access(appstate.ELFfile, 0) == 0)
      load_options = 1;
    else
      appstate.ELFfile[0] = '\0';
  }

  strlcpy(appstate.ParamFile, appstate.ELFfile, sizearray(appstate.ParamFile));
  strlcat(appstate.ParamFile, ".bmcfg", sizearray(appstate.ParamFile));

  ctx = guidriver_init("BlackMagic Flash Programmer", WINDOW_WIDTH, WINDOW_HEIGHT,
                       GUIDRV_CENTER | GUIDRV_TIMER, opt_fontstd, opt_fontmono, opt_fontsize);
  nuklear_style(ctx);

  tab_states[TAB_OPTIONS] = NK_MINIMIZED;
  tab_states[TAB_SERIALIZATION] = NK_MINIMIZED;
  tab_states[TAB_STATUS] = NK_MAXIMIZED;

  int running = 1;
  while (running) {
    /* handle state */
    int waitidle = handle_stateaction(&appstate, tab_states);

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
      appstate.curstate = STATE_INIT; /* BMP was inserted or removed */
    }

    /* GUI */
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT), 0)) {
      struct nk_rect rc_toolbutton;
      int result;
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, WINDOW_WIDTH - 4 * opt_fontsize);
      result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                              appstate.ELFfile, sizearray(appstate.ELFfile), nk_filter_ascii);
      if (result & NK_EDIT_COMMITED)
        load_options = 2;
      else if ((result & NK_EDIT_DEACTIVATED) != 0
               && strncmp(appstate.ELFfile, appstate.ParamFile, strlen(appstate.ELFfile)) != 0)
        load_options = 2;
      nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
      if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT) || nk_input_is_key_pressed(&ctx->input, NK_KEY_OPEN)) {
        #if defined _WIN32
          const char *filter = "ELF Executables\0*.elf;*.\0All files\0*.*\0";
        #else
          const char *filter = "ELF Executables\0*.elf\0All files\0*\0";
        #endif
        int res = noc_file_dialog_open(appstate.ELFfile, sizearray(appstate.ELFfile),
                                       NOC_FILE_DIALOG_OPEN, filter,
                                       NULL, NULL, "Select ELF Executable",
                                       guidriver_apphandle());
        if (res)
          load_options = 2;
      }
      nk_layout_row_end(ctx);

      nk_layout_row_dynamic(ctx, (LOGVIEW_ROWS+4)*ROW_HEIGHT, 1);
      if (nk_group_begin(ctx, "options", 0)) {
        panel_options(ctx, &appstate, tab_states);
        panel_serialize(ctx, &appstate, tab_states);

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Status", &tab_states[TAB_STATUS])) {
          nk_layout_row_dynamic(ctx, LOGVIEW_ROWS*ROW_HEIGHT, 1);
          log_widget(ctx, "status", logtext, opt_fontsize, &loglines);

          nk_layout_row_dynamic(ctx, ROW_HEIGHT*0.4, 1);
          nk_size progress, progress_range;
          bmp_progress_get(&progress, &progress_range);
          nk_progress(ctx, &progress, progress_range, NK_FIXED);

          nk_tree_state_pop(ctx);
        }

        nk_group_end(ctx);
      }

      /* the options are best reloaded after handling other settings, but before
         handling the download action */
      if (load_options != 0) {
        strlcpy(appstate.ParamFile, appstate.ELFfile, sizearray(appstate.ParamFile));
        strlcat(appstate.ParamFile, ".bmcfg", sizearray(appstate.ParamFile));
        if (load_targetparams(appstate.ParamFile, &appstate)) {
          if (load_options == 2)
            log_addstring("Changed target, settings loaded\n");
          else
            log_addstring("Settings for target loaded\n");
          /* for an LPC* target, check CRP */
          if (appstate.architecture > 0) {
            appstate.fpTgt = fopen(appstate.ELFfile, "rb");
            if (appstate.fpTgt != NULL) {
              result = elf_check_crp(appstate.fpTgt, &idx);
              fclose(appstate.fpTgt);
              appstate.fpTgt = NULL;
              if (result == ELFERR_NONE && idx > 0 && idx < 4) {
                char msg[100];
                sprintf(msg, "^3Code Read Protection (CRP%d) is set on the ELF file\n", idx);
                log_addstring(msg);
              }
            }
          }
        } else if (load_options == 2) {
          if (access(appstate.ELFfile, 0) != 0)
            log_addstring("^1Target not found\n");
          else
            log_addstring("New target, please check settings\n");
        }
        load_options = 0;
      }

      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.4, 0.025, 0.30, 0.025, 0.25));
      if (button_tooltip(ctx, "Download", NK_KEY_F5, appstate.curstate == STATE_IDLE, "Download ELF file into target (F5)")) {
        appstate.skip_download = 0;      /* should already be 0 */
        appstate.curstate = STATE_SAVE;  /* start the real download sequence */
      }
      nk_spacing(ctx, 1);
      rc_toolbutton = nk_widget_bounds(ctx);
      if (button_tooltip(ctx, "Tools", NK_KEY_NONE, appstate.curstate == STATE_IDLE, "Other commands"))
        toolmenu_active = TOOL_OPEN;
      nk_spacing(ctx, 1);
      if (nk_button_label(ctx, "Help") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F1))
        help_active = true;

      if (help_active)
        help_active = help_popup(ctx);

      if (toolmenu_active != TOOL_CLOSE) {
        toolmenu_active = tools_popup(ctx, &rc_toolbutton);
        switch (toolmenu_active) {
        case TOOL_RESCAN:
          appstate.curstate = STATE_INIT;
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_FULLERASE:
          appstate.curstate = STATE_FULLERASE;
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_OPTIONERASE:
          appstate.curstate = STATE_ERASE_OPTBYTES;
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_STM32PROTECT:
          appstate.curstate = STATE_SET_CRP;
          toolmenu_active = TOOL_CLOSE;
          break;
        case TOOL_VERIFY:
          appstate.skip_download = 1;
          appstate.curstate = STATE_SAVE;  /* start the pseudo-download sequence */
          toolmenu_active = TOOL_CLOSE;
          break;
        }
      }
    }
    nk_end(ctx);

    /* Draw */
    guidriver_render(COLOUR_BG0_S);
  }

  if (strlen(appstate.ParamFile) > 0 && access(appstate.ParamFile, 0) == 0)
    save_targetparams(appstate.ParamFile, &appstate);
  ini_putf("Settings", "fontsize", opt_fontsize, txtConfigFile);
  ini_puts("Settings", "fontstd", opt_fontstd, txtConfigFile);
  ini_puts("Settings", "fontmono", opt_fontmono, txtConfigFile);
  if (strlen(txtConfigFile) > 0)
    ini_puts("Session", "recent", appstate.ELFfile, txtConfigFile);
  if (bmp_is_ip_address(appstate.IPaddr))
    ini_puts("Settings", "ip-address", appstate.IPaddr, txtConfigFile);
  ini_putl("Settings", "appstate.probe", (appstate.probe == appstate.netprobe) ? 99 : appstate.probe, txtConfigFile);

  clear_probelist(appstate.probelist, appstate.netprobe);
  guidriver_close();
  bmscript_clear();
  gdbrsp_packetsize(0);
  bmp_disconnect();
  tcpip_cleanup();
  return EXIT_SUCCESS;
}

