/*
 * Utility to download executable programs to the target micro-controller via
 * the Black Magic Probe on a system. This utility is built with Nuklear for a
 * cross-platform GUI.
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

#if defined _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellapi.h>
  #include <direct.h>
  #include <io.h>
#elif defined __linux__
  #include <unistd.h>
  #include <bsd/string.h>
  #include <sys/stat.h>
#endif
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guidriver.h"
#include "noc_file_dialog.h"
#include "bmp-script.h"
#include "bmp-support.h"
#include "elf-postlink.h"
#include "gdb-rsp.h"
#include "minIni.h"
#include "rs232.h"
#include "specialfolder.h"

#include "res/btn_folder.h"
#if defined __linux__ || defined __unix__
  #include "res/icon_download_64.h"
#endif

#ifndef NK_ASSERT
  #include <assert.h>
  #define NK_ASSERT(expr) assert(expr)
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif
#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif


#define WINDOW_WIDTH  400
#define WINDOW_HEIGHT 300
#define FONT_HEIGHT   14
#define ROW_HEIGHT    (2 * FONT_HEIGHT)
#define COMBOROW_CY   (0.65 * ROW_HEIGHT)


static float *nk_ratio(int count, ...)
{
  #define MAX_ROW_FIELDS 10
  static float r_array[MAX_ROW_FIELDS];
  va_list ap;
  int i;

  NK_ASSERT(count < MAX_ROW_FIELDS);
  va_start(ap, count);
  for (i = 0; i < count; i++)
    r_array[i] = (float) va_arg(ap, double);
  va_end(ap);
  return r_array;
}

static void set_style(struct nk_context *ctx)
{
  struct nk_color table[NK_COLOR_COUNT];

  table[NK_COLOR_TEXT]= nk_rgba(201, 243, 255, 255);
  table[NK_COLOR_WINDOW]= nk_rgba(35, 52, 71, 255);
  table[NK_COLOR_HEADER]= nk_rgba(122, 20, 50, 255);
  table[NK_COLOR_BORDER]= nk_rgba(128, 128, 128, 255);
  table[NK_COLOR_BUTTON]= nk_rgba(122, 20, 50, 255);
  table[NK_COLOR_BUTTON_HOVER]= nk_rgba(140, 25, 50, 255);
  table[NK_COLOR_BUTTON_ACTIVE]= nk_rgba(140, 25, 50, 255);
  table[NK_COLOR_TOGGLE]= nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_TOGGLE_HOVER]= nk_rgba(45, 60, 60, 255);
  table[NK_COLOR_TOGGLE_CURSOR]= nk_rgba(122, 20, 50, 255);
  table[NK_COLOR_SELECT]= nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_SELECT_ACTIVE]= nk_rgba(122, 20, 50, 255);
  table[NK_COLOR_SLIDER]= nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_SLIDER_CURSOR]= nk_rgba(122, 20, 50, 255);
  table[NK_COLOR_SLIDER_CURSOR_HOVER]= nk_rgba(140, 25, 50, 255);
  table[NK_COLOR_SLIDER_CURSOR_ACTIVE]= nk_rgba(140, 25, 50, 255);
  table[NK_COLOR_PROPERTY]= nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_EDIT]= nk_rgba(20, 29, 38, 225);
  table[NK_COLOR_EDIT_CURSOR]= nk_rgba(201, 243, 255, 255);
  table[NK_COLOR_COMBO]= nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_CHART]= nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_CHART_COLOR]= nk_rgba(170, 40, 60, 255);
  table[NK_COLOR_CHART_COLOR_HIGHLIGHT]= nk_rgba(255, 0, 0, 255);
  table[NK_COLOR_SCROLLBAR]= nk_rgba(30, 40, 60, 255);
  table[NK_COLOR_SCROLLBAR_CURSOR]= nk_rgba(179, 175, 132, 255);
  table[NK_COLOR_SCROLLBAR_CURSOR_HOVER]= nk_rgba(204, 199, 141, 255);
  table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE]= nk_rgba(204, 199, 141, 255);
  table[NK_COLOR_TAB_HEADER]= nk_rgba(122, 20, 50, 255);
  nk_style_from_table(ctx, table);
}

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

/* log_widget() draws the text in the log window and scrolls to the last line
   if new text was added */
static int log_widget(struct nk_context *ctx, const char *id, const char *content, float rowheight, unsigned *scrollpos)
{
  int lines = 0;
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window *stwin = &ctx->style.window;
  struct nk_style_item bkgnd;

  /* black background on group */
  bkgnd = stwin->fixed_background;
  stwin->fixed_background = nk_style_item_color(nk_rgba(20, 29, 38, 225));
  if (nk_group_begin_titled(ctx, id, "", NK_WINDOW_BORDER)) {
    int lineheight = 0;
    const char *head = content;
    const char *tail;
    while (head != NULL && *head != '\0' && !(*head == '\n' && *(head + 1) == '\0')) {
      if ((tail = strchr(head, '\n')) == NULL)
        tail = strchr(head, '\0');
      NK_ASSERT(tail != NULL);
      nk_layout_row_dynamic(ctx, rowheight, 1);
      if (lineheight == 0) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        lineheight = rcline.h;
      }
      if (*head == '^' && *(head + 1) == '1')
        nk_text_colored(ctx, head + 2, (int)(tail - head - 2), NK_TEXT_LEFT, nk_rgb(255, 100, 128));
      else if (*head == '^' && *(head + 1) == '2')
        nk_text_colored(ctx, head + 2, (int)(tail - head - 2), NK_TEXT_LEFT, nk_rgb(100, 255, 100));
      else if (*head == '^' && *(head + 1) == '3')
        nk_text_colored(ctx, head + 2, (int)(tail - head - 2), NK_TEXT_LEFT, nk_rgb(255, 255, 100));
      else
        nk_text(ctx, head, (int)(tail - head), NK_TEXT_LEFT);
      lines++;
      head = (*tail != '\0') ? tail + 1 : tail;
    }
    /* add an empty line to fill up any remaining space below */
    nk_layout_row_dynamic(ctx, rowheight, 1);
    nk_spacing(ctx, 1);
    nk_group_end(ctx);
    if (scrollpos != NULL) {
      /* calculate scrolling */
      int widgetlines = (rcwidget.h - 2 * stwin->padding.y) / lineheight;
      int ypos = (lines - widgetlines + 1) * lineheight;
      if (ypos < 0)
        ypos = 0;
      if (ypos != *scrollpos) {
        nk_group_set_scroll(ctx, id, 0, ypos);
        *scrollpos = ypos;
      }
    }
  }
  stwin->fixed_background = bkgnd;
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

static int serialize_databuffer(unsigned char *buffer, int size, int serialnum, int format)
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

static int serialize_address(FILE *fp, const char *section, unsigned long address,
                             unsigned char *data, int datasize)
{
  unsigned long offset;
  int err;

  /* find section, if provided */
  assert(fp != NULL);
  if (strlen(section) > 0) {
    unsigned long length;
    err = elf_section_by_name(fp, section, &offset, NULL, &length);
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

static int serialize_match(FILE *fp, const char *match, unsigned long offset,
                           unsigned char *data, int datasize)
{
  unsigned char matchbuf[100];
  int buflength;
  unsigned char *buffer;
  size_t bytes;
  size_t fileoffs, filesize;
  int widechars = 0;

  /* create buffer to match from the string */
  assert(match != NULL);
  if (strlen(match) == 0) {
    log_addstring("^1Serialization match text is empty\n");
    return 0;
  }
  buflength = 0;
  while (*match != '\0' && buflength < sizearray(matchbuf) - 2) {
    if (*match == '\\') {
      if (*(match + 1) == '\\') {
        match++;
        matchbuf[buflength] = *match; /* '\\' is replaced by a single '\'*/
      } else if (*(match + 1) == 'x' && isxdigit(*(match + 2))) {
        int val = 0;
        int len = 0;
        match += 2;     /* skip '\x' */
        while (len < 2 && isdigit(*match)) {
          int ch;
          if (*match >= '0' && *match <= '9')
            ch = *match - '0';
          else if (*match >= 'A' && *match <= 'F')
            ch = *match - 'A' + 10;
          else if (*match >= 'a' && *match <= 'f')
            ch = *match - 'a' + 10;
          val = (val << 4) | ch;
          match++;
          len++;
        }
      } else if (isdigit(*(match + 1))) {
        int val = 0;
        int len = 0;
        match += 1;     /* skip '\' */
        while (len < 3 && isdigit(*match)) {
          val = 10 * val + (*match - '0');
          match++;
          len++;
        }
        matchbuf[buflength] = (unsigned char)val;
      } else if (*(match + 1) == 'A' && *(match + 2) == '*') {
        match += 2;
        widechars = 0;
      } else if (*(match + 1) == 'U' && *(match + 2) == '*') {
        match += 2;
        widechars = 1;
      } else {
        /* nothing recognizable follows the '\', take it literally */
        log_addstring("^1Invalid syntax for match string\n");
        matchbuf[buflength] = *match;
      }
    } else {
      matchbuf[buflength] = *match;
    }
    if (widechars)
      matchbuf[++buflength] = '\0';
    buflength++;
    match++;
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
  for (fileoffs = 0; fileoffs < bytes - buflength; fileoffs++) {
    if (buffer[fileoffs] == matchbuf[0] && memcmp(buffer + fileoffs, matchbuf, buflength) == 0)
      break;
  }
  free(buffer);
  if (fileoffs >= bytes - buflength) {
    log_addstring("^1Match string not found\n");
    return 0;
  }

  /* path the replacement data at the found file position */
  fseek(fp, fileoffs + offset, SEEK_SET);
  fwrite(data, 1, datasize, fp);
  fseek(fp, 0, SEEK_SET);

  return 1;
}

int main(int argc, char *argv[])
{
  static const char *architectures[] = { "generic", "lpc8xx", "lpc11xx", "lpc15xx",
                                         "lpc17xx", "lpc21xx", "lpc22xx", "lpc23xx",
                                         "lpc24xx", "lpc43xx"};
  static const char helptext[] =
    "This utility downloads firmware into a micro-controller\n"
    "using the Black Magic Probe. It automatically handles\n"
    "idiosyncrasies of MCU families, and supports setting a\n"
    "serial number during the download (serialization).\n"
    "It does not require GDB.\n\n"
    "^3Options\n"
    "The MCU family must be set for post-processing the\n"
    "ELF file or performing additional configurations before\n"
    "the download. It is currently needed for the LPC family\n"
    "by NXP. For other micro-controllers, this field should\n"
    "be set to \"generic\"\n\n"
    "The \"Power Target\" option can be set to drive the\n"
    "power-sense pin with 3.3V (to power the target).\n\n"
    "^3Serialization\n"
    "The serialization method is either \"No serialization\",\n"
    "or \"Address\" to store the serial number at a specific\n"
    "address, or \"Match\" to search for a text or byte pattern\n"
    "and replace it with the serial number.\n\n"
    "For \"Address\" mode, you may optionally give the name\n"
    "of a section in the ELF file. The address is relative to the\n"
    "section, or relative to the beginning of the ELF file if no\n"
    "section name is given. The address is interpreted as a\n"
    "hexadecimal value.\n\n"
    "For \"Match\" mode, you give a pattern to match and an\n"
    "offset from the start of the pattern where to store the\n"
    "serial number at. The offset is interpreted as a hexa-\n"
    "decimal value. The match string may contain \\### and\n"
    "\\x## specifications (where \"#\" represents a decimal or\n"
    "hexadecimal digit) for non-ASCII byte values. It may\n"
    "furthermore contain the sequence \\U* to interpret the\n"
    "text that follows as Unicode, or \\A* to switch back to\n"
    "ASCII. When a literal \\ is part of the pattern, it must\n"
    "be doubled, as in \\\\.\n\n"
    "The serial number is a decimal value. It is incremented\n"
    "after each successful download. The size of the serial\n"
    "number is in bytes. The format can be chosen as binary,\n"
    "ASCII or Unicode. In the latter two cases, the serial\n"
    "number is stored as readable text.\n\n";

  enum { SER_NONE, SER_ADDRESS, SER_MATCH };
  enum { FMT_BIN, FMT_ASCII, FMT_UNICODE };
  enum { TAB_OPTIONS, TAB_SERIALIZATION, TAB_STATUS, /* --- */ TAB_COUNT };

  struct nk_context *ctx;
  struct nk_image btn_folder;
  struct nk_rect rcwidget;
  enum nk_collapse_states tab_states[TAB_COUNT];
  int running = 1;
  char txtFilename[256] = "", txtCfgFile[256];
  char txtSection[32] = "", txtAddress[32] = "", txtMatch[64] = "", txtOffset[32] = "";
  char txtSerial[32] = "", txtSerialSize[32] = "";
  char txtConfigFile[256];
  int opt_tpwr = nk_false;
  int opt_architecture = 0;
  int opt_serialize = SER_NONE;
  int opt_format = FMT_BIN;
  int help_active = 0;
  int load_options = 0;

  /* locate the configuration file */
  if (folder_AppConfig(txtConfigFile, sizearray(txtConfigFile))) {
    strlcat(txtConfigFile, DIR_SEPARATOR "BlackMagic", sizearray(txtConfigFile));
    #if defined _WIN32
      mkdir(txtConfigFile);
    #else
      mkdir(txtConfigFile, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    #endif
    strlcat(txtConfigFile, DIR_SEPARATOR "bmflash.ini", sizearray(txtConfigFile));
  }

  txtFilename[0] = '\0';
  if (argc >= 2 && access(argv[1], 0) == 0) {
    strlcpy(txtFilename, argv[1], sizearray(txtFilename));
    load_options = 1;
  } else {
    ini_gets("Session", "recent", "", txtFilename, sizearray(txtFilename), txtConfigFile);
    if (access(txtFilename, 0) == 0)
      load_options = 1;
    else
      txtFilename[0] = '\0';
  }
  strcpy(txtCfgFile, txtFilename);
  strcpy(txtSection, ".text");
  strcpy(txtAddress, "0");
  strcpy(txtMatch, "");
  strcpy(txtOffset, "0");
  strcpy(txtSerial, "1");
  strcpy(txtSerialSize, "4");

  /* check presence of the debug probe */
  bmp_setcallback(bmp_callback);
  bmp_connect();

  ctx = guidriver_init("BlackMagic Flash Programmer", WINDOW_WIDTH, WINDOW_HEIGHT, 0, FONT_HEIGHT);
  set_style(ctx);
  btn_folder = guidriver_image_from_memory(btn_folder_data, btn_folder_datasize);

  tab_states[TAB_OPTIONS] = NK_MINIMIZED;
  tab_states[TAB_SERIALIZATION] = NK_MINIMIZED;
  tab_states[TAB_STATUS]= NK_MAXIMIZED;

  while (running) {
    /* Input */
    nk_input_begin(ctx);
    if (!guidriver_poll(1))
      running = 0;
    nk_input_end(ctx);

    /* GUI */
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT), 0)) {
      int result;
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, WINDOW_WIDTH - 57);
      result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, txtFilename, sizearray(txtFilename), nk_filter_ascii);
      if (result & NK_EDIT_COMMITED)
        load_options = 2;
      else if ((result & NK_EDIT_DEACTIVATED) != 0 && strncmp(txtFilename, txtCfgFile, strlen(txtFilename)) != 0)
        load_options = 2;
      nk_layout_row_push(ctx, 26);
      if (nk_button_image(ctx, btn_folder) || nk_input_is_key_pressed(&ctx->input, NK_KEY_OPEN)) {
        const char *s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                                             "ELF Executables\0*.elf;*.bin;*.\0All files\0*.*\0",
                                             NULL, NULL, "Select ELF Executable",
                                             guidriver_apphandle());
        if (s != NULL && strlen(s) < sizearray(txtFilename)) {
          strcpy(txtFilename, s);
          load_options = 2;
          free((void*)s);
        }
      }
      nk_layout_row_end(ctx);

      nk_layout_row_dynamic(ctx, 7.5*ROW_HEIGHT, 1);
      if (nk_group_begin_titled(ctx, "options", "", 0)) {
        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Options", &tab_states[TAB_OPTIONS])) {
          nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT * 0.8, 2 , nk_ratio(2, 0.45, 0.55));
          nk_label(ctx, "MCU Family", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          rcwidget = nk_widget_bounds(ctx);
          opt_architecture = nk_combo(ctx, architectures, NK_LEN(architectures), opt_architecture, COMBOROW_CY, nk_vec2(rcwidget.w, 4.5*ROW_HEIGHT));

          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          nk_checkbox_label(ctx, "Power Target (3.3V)", &opt_tpwr);

          nk_tree_state_pop(ctx);
        }

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Serialization", &tab_states[TAB_SERIALIZATION])) {
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          if (nk_option_label(ctx, "No serialization", opt_serialize == SER_NONE))
            opt_serialize = SER_NONE;
          nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(4, 0.3, 0.3, 0.1, 0.3));
          if (nk_option_label(ctx, "Address", opt_serialize == SER_ADDRESS))
            opt_serialize = SER_ADDRESS;
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, txtSection, sizearray(txtSection), nk_filter_ascii);
          nk_label(ctx, "+ 0x", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, txtAddress, sizearray(txtAddress), nk_filter_hex);
          nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(4, 0.3, 0.3, 0.1, 0.3));
          if (nk_option_label(ctx, "Match", opt_serialize == SER_MATCH))
            opt_serialize = SER_MATCH;
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, txtMatch, sizearray(txtMatch), nk_filter_ascii);
          nk_label(ctx, "+ 0x", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, txtOffset, sizearray(txtOffset), nk_filter_hex);
          nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.05, 0.24, 0.31, 0.1, 0.3));//??? needed to tweak 0.25:0.30 to 0.24:0.31
          nk_spacing(ctx, 1);
          nk_label(ctx, "Serial", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, txtSerial, sizearray(txtSerial), nk_filter_decimal);
          nk_label(ctx, "size", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, txtSerialSize, sizearray(txtSerialSize), nk_filter_decimal);
          nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.05, 0.25, 0.23, 0.23, 0.23));
          nk_spacing(ctx, 1);
          nk_label(ctx, "Format", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          if (nk_option_label(ctx, "Binary", opt_format == FMT_BIN))
            opt_format = FMT_BIN;
          if (nk_option_label(ctx, "ASCII", opt_format == FMT_ASCII))
            opt_format = FMT_ASCII;
          if (nk_option_label(ctx, "Unicode", opt_format == FMT_UNICODE))
            opt_format = FMT_UNICODE;
          nk_tree_state_pop(ctx);
        }

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Status", &tab_states[TAB_STATUS])) {
          nk_layout_row_dynamic(ctx, 4*ROW_HEIGHT, 1);
          log_widget(ctx, "status", logtext, FONT_HEIGHT, &loglines);
          nk_tree_state_pop(ctx);
        }

        nk_group_end(ctx);
      }

      /* the options are best reloaded after handling other settings, but before
         handling the download action */
      if (load_options != 0) {
        strcpy(txtCfgFile, txtFilename);
        strcat(txtCfgFile, ".prog");
        if (access(txtCfgFile, 0) == 0) {
          char field[80], *ptr;
          ini_gets("Options", "architecture", "", field, sizearray(field), txtCfgFile);
          for (opt_architecture = 0; opt_architecture < sizearray(architectures); opt_architecture++)
            if (stricmp(architectures[opt_architecture], field) == 0)
              break;
          if (opt_architecture >= sizearray(architectures))
            opt_architecture = 0;
          opt_tpwr = (int)ini_getl("Options", "tpwr", 0, txtCfgFile);
          opt_serialize = (int)ini_getl("Serialize", "option", 0, txtCfgFile);
          ini_gets("Serialize", "address", ".text:0", field, sizearray(field), txtCfgFile);
          if ((ptr = strchr(field, ':')) != NULL) {
            *ptr++ = '\0';
            strcpy(txtSection, field);
            strcpy(txtAddress, ptr);
          }
          ini_gets("Serialize", "match", ":0", field, sizearray(field), txtCfgFile);
          if ((ptr = strchr(field, ':')) != NULL) {
            *ptr++ = '\0';
            strcpy(txtMatch, field);
            strcpy(txtOffset, ptr);
          }
          ini_gets("Serialize", "serial", "1:4:0", field, sizearray(field), txtCfgFile);
          if ((ptr = strchr(field, ':')) != NULL) {
            char *p2;
            *ptr++ = '\0';
            strcpy(txtSerial, strlen(field) > 0 ? field : "0");
            if ((p2 = strchr(ptr, ':')) != NULL) {
              *p2++ = '\0';
              strcpy(txtSerialSize, strlen(ptr) > 0 ? ptr : "1");
              opt_format = (int)strtol(p2, NULL, 10);
            }
          }
          if (load_options == 2)
            log_addstring("Changed target, settings loaded\n");
          else
            log_addstring("Settings for target loaded\n");
        } else if (load_options == 2) {
          if (access(txtFilename, 0) != 0)
            log_addstring("^1Target not found\n");
          else
            log_addstring("New target, please check settings\n");
        }
        load_options = 0;
      }

      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 3, nk_ratio(3, 0.2, 0.4, 0.4));
      if (nk_button_label(ctx, "Help") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F1))
        help_active = 1;
      nk_spacing(ctx, 1);
      if (nk_button_label(ctx, "Download") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F5)) {
        /* close Options and Serialization, open Status */
        tab_states[TAB_OPTIONS] = NK_MINIMIZED;
        tab_states[TAB_SERIALIZATION] = NK_MINIMIZED;
        tab_states[TAB_STATUS]= NK_MAXIMIZED;
        if (access(txtFilename, 0) == 0) {
          char field[100];
          int result;
          FILE *fpTgt, *fpWork;
          /* save settings in cache file */
          strcpy(txtCfgFile, txtFilename);
          strcat(txtCfgFile, ".prog");
          if (opt_architecture > 0 && opt_architecture < sizearray(architectures))
            strcpy(field, architectures[opt_architecture]);
          else
            field[0] = '\0';
          ini_puts("Options", "architecture", field, txtCfgFile);
          ini_putl("Options", "tpwr", opt_tpwr, txtCfgFile);
          ini_putl("Serialize", "option", opt_serialize, txtCfgFile);
          sprintf(field, "%s:%s", txtSection, txtAddress);
          ini_puts("Serialize", "address", field, txtCfgFile);
          sprintf(field, "%s:%s", txtMatch, txtOffset);
          ini_puts("Serialize", "match", field, txtCfgFile);
          sprintf(field, "%s:%s:%d", txtSerial, txtSerialSize, opt_format);
          ini_puts("Serialize", "serial", field, txtCfgFile);
          /* attach the Black Magic Probe to the target */
          fpTgt = fpWork = NULL;
          result = bmp_connect();
          if (result) {
            char mcufamily[32];
            int arch;
            result = bmp_attach(opt_tpwr, mcufamily, sizearray(mcufamily), NULL, 0);
            for (arch = 0; arch < sizearray(architectures); arch++)
              if (stricmp(architectures[arch], mcufamily) == 0)
                break;
            if (arch >= sizearray(architectures))
              arch = 0;
            if (arch != opt_architecture) {
              char msg[128];
              sprintf(msg, "^3Detected MCU family %s (check options)\n", architectures[arch]);
              log_addstring(msg);
            }
          }
          if (result) {
            fpTgt = fopen(txtFilename, "rb");
            if (fpTgt == NULL) {
              log_addstring("^1Failed to load the target file\n");
              result = 0;
            }
            /* verify whether to patch the ELF file (create a temporary file) */
            if (result && (opt_architecture > 0 || opt_serialize != SER_NONE)) {
              fpWork = tmpfile();
              if (fpTgt == NULL || fpWork == NULL) {
                log_addstring("^1Failed to process the target file\n");
                result = 0;
              }
              if (result)
                result = copyfile(fpWork, fpTgt);
              if (result && opt_architecture > 0)
                result = patch_vecttable(fpWork, architectures[opt_architecture]);
              if (result && opt_serialize != SER_NONE) {
                /* create replacement buffer, depending on format */
                unsigned char data[50];
                int datasize = (int)strtol(txtSerialSize, NULL, 10);
                serialize_databuffer(data, datasize, (int)strtol(txtSerial,NULL,10), opt_format);
                if (opt_serialize == SER_ADDRESS)
                  result = serialize_address(fpWork, txtSection, strtoul(txtAddress,NULL,16), data, datasize);
                else if (opt_serialize == SER_MATCH)
                  result = serialize_match(fpWork, txtMatch, strtoul(txtOffset,NULL,16), data, datasize);
                if (result) {
                  char msg[100];
                  sprintf(msg, "Serial adjusted to %d\n", (int)strtol(txtSerial, NULL, 10));
                  log_addstring(msg);
                }
              }
            }
          }

          /* download to target */
          if (result) {
            if (opt_architecture > 0)
              bmp_runscript("memremap", architectures[opt_architecture], NULL);
            result = bmp_download((fpWork != NULL)? fpWork : fpTgt);
          }
          if (result) {
            if (opt_architecture > 0)
              bmp_runscript("memremap", architectures[opt_architecture], NULL);
            result = bmp_verify((fpWork != NULL)? fpWork : fpTgt);
          }

          /* optionally increment the serial number */
          if (result && opt_serialize != SER_NONE) {
            int num = (int)strtol(txtSerial, NULL, 10);
            sprintf(txtSerial, "%d", num + 1);
          }

          if (fpTgt != NULL)
            fclose(fpTgt);
          if (fpWork != NULL) {
            fclose(fpWork);
            fpWork = NULL;
          }
          bmp_detach(0);
        } else {
          log_addstring("^1Failed to open the ELF file\n");
        }
      }

      if (help_active) {
        static struct nk_rect rc = {10, 10, WINDOW_WIDTH - 20, WINDOW_HEIGHT - 20};
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Help", NK_WINDOW_NO_SCROLLBAR, rc)) {
          nk_layout_row_dynamic(ctx, 8*ROW_HEIGHT, 1);
          log_widget(ctx, "help", helptext, FONT_HEIGHT, NULL);
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 4);
          nk_spacing(ctx, 3);
          if (nk_button_label(ctx, "Close") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
            help_active = 0;
            nk_popup_close(ctx);
          }
          nk_popup_end(ctx);
        } else {
          help_active = 0;
        }
      }

    }
    nk_end(ctx);

    /* Draw */
    guidriver_render(nk_rgb(30,30,30));
  }

  if (strlen(txtConfigFile) > 0)
    ini_puts("Session", "recent", txtFilename, txtConfigFile);

  guidriver_close();
  gdbrsp_packetsize(0);
  if (rs232_isopen()) {
    rs232_dtr(0);
    rs232_rts(0);
    rs232_close();
  }
  return 0;
}
