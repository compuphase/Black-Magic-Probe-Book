/*
 * Serial monitor/terminal. This is a general purpose utility, but developed as
 * part of the "Black Magic" utilities (because an adequate serial terminal
 * was lacking).
 * This utility is built with Nuklear for a cross-platform GUI.
 *
 * Copyright 2022-2023 CompuPhase
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
# include <direct.h>
# include <io.h>
# include <malloc.h>
# if defined __MINGW32__ || defined __MINGW64__
#   include "strlcpy.h"
# elif defined _MSC_VER
#   include "strlcpy.h"
#   define access(p,m)       _access((p),(m))
# endif
#elif defined __linux__
# include <alloca.h>
# include <pthread.h>
# include <unistd.h>
# include <bsd/string.h>
# include <sys/stat.h>
# include <sys/time.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guidriver.h"
#include "minIni.h"
#include "noc_file_dialog.h"
#include "nuklear_guide.h"
#include "nuklear_mousepointer.h"
#include "nuklear_splitter.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"
#include "rs232.h"
#include "specialfolder.h"
#include "svnrev.h"
#include "tcl.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __linux__ || defined __unix__
# include "res/icon_serial_64.h"
#endif

#if !defined _MAX_PATH
# define _MAX_PATH 260
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif
#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if defined WIN32 || defined _WIN32
# define DIRSEP_CHAR '\\'
# define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
# define DIRSEP_CHAR '/'
# define IS_OPTION(s)  ((s)[0] == '-')
#endif


#define WINDOW_WIDTH    700     /* default window size (window is resizable) */
#define WINDOW_HEIGHT   420
#define FONT_HEIGHT     14      /* default font size */
#define ROW_HEIGHT      (1.6 * opt_fontsize)
#define COMBOROW_CY     (0.9 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.5 * opt_fontsize)
static float opt_fontsize = FONT_HEIGHT;


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
    printf("BMSerial - Serial Monitor/Terminal.\n\n");
  printf("Usage: bmserial [options]\n\n"
         "Options:\n"
         "-f=value  Font size to use (value must be 8 or larger).\n"
         "-h        This help.\n\n"
         "-v        Show version information.\n");
  //??? command line options to select the port & frame format
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

  printf("BMSerial version %s.\n", SVNREV_STR);
  printf("Copyright 2022 CompuPhase\nLicensed under the Apache License version 2.0\n");
}

#if defined FORTIFY
  void Fortify_OutputFunc(const char *str, int type)
  {
#   if defined _WIN32  /* fix console output on Windows */
      if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "wb", stdout);
        freopen("CONOUT$", "wb", stderr);
      }
      printf("Fortify: [%d] %s\n", type, str);
#   endif
  }
#endif


#define DFLAG_LOCAL   0x01    /* this data block is local input (transmitted text) */
#define DFLAG_SCRIPT  0x02    /* this data block is script output */

typedef struct tagDATALIST {
  struct tagDATALIST *next;
  unsigned char *data;        /* raw data, as received */
  size_t datasize;
  char **text;                /* reformatted data */
  int textlines;              /* the number of (valid) lines in the text array */
  int maxlines;               /* the maximum size of the text array */
  unsigned long timestamp;    /* timestamp of reception (milliseconds) */
  int flags;                  /* data flags */
} DATALIST;

static DATALIST datalist_root = { NULL };
static time_t reception_timestamp;

static void datalist_freeitem(DATALIST *item)
{
  assert(item != NULL);
  assert(item->data != NULL);
  free((void*)item->data);
  if (item->text != NULL) {
    for (int idx = 0; idx < item->textlines; idx++)
      free((void*)item->text[idx]);
    free((void*)item->text);
  }
  free((void*)item);
}

static void datalist_clear(void)
{
  while (datalist_root.next != NULL) {
    DATALIST *item = datalist_root.next;
    datalist_root.next = item->next;
    datalist_freeitem(item);
  }
}

static DATALIST *datalist_append(const unsigned char *buffer, size_t size, bool isascii, int flags)
{
  assert(buffer != NULL && size > 0);

  /* get the time (in milliseconds) of this reception */
  unsigned long tstamp = timestamp();

  /* for the very first data block, also get the local time, and store it in
     the root block */
  if (datalist_root.next == NULL) {
    reception_timestamp = time(NULL);
    datalist_root.timestamp = tstamp;
  }

  /* check whether the buffer should be appended to the previous one */
  DATALIST *last = &datalist_root;
  while (last->next != NULL)
    last = last->next;

  /* never append a local echo, and obviously don't append if the list is empty,
     but also don't append *to* a local echo */
  bool append = flags == 0 && datalist_root.next != NULL && !datalist_root.next->flags == 0;
  if (append) {
    /* if there is significant delay between the reception of the two blocks,
       assume separate receptions; but "significant" is different for blocks
       with ASCII text that end on an EOL character, than for blocks with non-ASCII
       text or that end at some other character */
    assert(last->datasize > 0);
    unsigned char final = last->data[last->datasize - 1];
    unsigned long max_gap = (isascii && (final == '\r' || final == '\n')) ? 5 : 50;
    if (tstamp - last->timestamp > max_gap)
      append = false;
  }
  if (append && last->datasize + size > 512) {
    /* when blocks grow too large, assume a separate reception */
    append = false;
  }

  DATALIST *item;
  if (append) {
    unsigned char *newbuf = malloc((last->datasize + size) * sizeof(unsigned char));
    if (newbuf != NULL) {
      memcpy(newbuf, last->data, last->datasize);
      memcpy(newbuf + last->datasize, buffer, size);
      free((void*)last->data);
      last->data = newbuf;
      last->datasize += size;
    }
    item = last;
  } else {
    /* add a new data block, and append to the list */
    item = malloc(sizeof(DATALIST));
    if (item == NULL)
      return NULL;
    memset(item, 0, sizeof(DATALIST));
    item->data = malloc(size * sizeof(unsigned char));
    if (item->data == NULL) {
      free((void*)item);
      return NULL;
    }
    memcpy(item->data, buffer, size);
    item->datasize = size;
    item->flags = flags;
    item->timestamp = tstamp - datalist_root.timestamp;
    assert(last != NULL && last->next == NULL);
    last->next = item;
  }

  return item;
}


typedef struct tagFILTER {
  struct tagFILTER *next;
  char text[64];
  struct nk_color colour;
  nk_bool enabled;
} FILTER;

static FILTER *filter_add(FILTER *root, char *text, struct nk_color colour, nk_bool enabled)
{
  assert(root != NULL);
  FILTER *flt = malloc(sizeof(FILTER));
  if (flt != NULL) {
    memset(flt, 0, sizeof(FILTER));
    strlcpy(flt->text, text, sizearray(flt->text));
    flt->colour = colour;
    flt->enabled = enabled;
    FILTER *last = root;
    while (last->next != NULL)
      last = last->next;
    last->next = flt;
  }
  return flt;
}

static bool filter_delete(FILTER *root, FILTER *flt)
{
  assert(root != NULL);
  assert(flt != NULL);
  /* find filter item in the list that comes before this one */
  FILTER *pred = root;
  while (pred->next != NULL && pred->next != flt)
    pred = pred->next;
  if (pred->next == NULL)
    return false;
  pred->next = flt->next;
  free((void*)flt);
  return true;
}

static void filter_clear(FILTER *root)
{
  assert(root != NULL);
  while (root->next != NULL) {
    FILTER *flt = root->next;
    root->next = flt->next;
    free((void*)flt);
  }
}

static FILTER *filter_match(FILTER *root, const char *string)
{
  assert(root != NULL);
  assert(string != NULL);
  if (strlen(string) == 0)
    return NULL;
  for (FILTER *flt = root->next; flt != NULL; flt = flt->next)
    if (flt->enabled && flt->text[0] != '\0' && strstr(string, flt->text) != NULL)
      return flt;
  return NULL;
}

static struct nk_color filter_defcolour(FILTER *root)
{
  static const struct nk_color defcolours[] = {
    { 0xcc, 0x24, 0x1d, 0xff }, /* COLOUR_BG_RED    */
    { 0x78, 0xa7, 0x1a, 0xff }, /* COLOUR_BG_GREEN  */
    { 0xd7, 0x99, 0x21, 0xff }, /* COLOUR_BG_YELLOW */
    { 0x45, 0x85, 0x88, 0xff }, /* COLOUR_BG_BLUE   */
    { 0xb1, 0x62, 0x86, 0xff }, /* COLOUR_BG_PURPLE */
    { 0x68, 0x9d, 0x6a, 0xff }, /* COLOUR_BG_AQUA   */
    { 0xa8, 0x99, 0x84, 0xff }  /* COLOUR_BG_GRAY   */
  };

  assert(root != NULL);
  unsigned flt_count = 0;
  unsigned mask = ~(~0 << sizearray(defcolours)); /* 1-bit for every entry in defcolours */
  for (FILTER *flt = root->next; flt != NULL; flt = flt->next) {
    for (unsigned idx = 0; idx < sizearray(defcolours); idx++) {
      if (memcmp(&flt->colour, &defcolours[idx], sizeof(struct nk_color)) == 0) {
        mask &= ~(1 << idx);  /* this colour is used, by preference, do not re-use it */
        break;
      }
    }
    flt_count += 1;
  }

  unsigned idx;
  if (mask != 0) {
    /* there are default colours that have not already been used, select the
       first one */
    for (idx = 0; (mask & (1 << idx)) == 0; idx++)
      {}
    assert(idx < sizearray(defcolours));
  } else {
    /* all default colours have been used, just pick one */
    idx = flt_count % sizearray(defcolours);
  }
  return defcolours[idx];
}


enum {
  TAB_PORTCONFIG,
  TAB_LINESTATUS,
  TAB_DISPLAYOPTIONS,
  TAB_TRANSMITOPTIONS,
  TAB_FILTERS,
  TAB_SCRIPT,
  /* --- */
  TAB_COUNT
};

enum {
  VIEW_TEXT,
  VIEW_HEX,
};

enum {
  TIMESTAMP_NONE,
  TIMESTAMP_RELATIVE,
  TIMESTAMP_ABSOLUTE,
};

enum {
  EOL_NONE,
  EOL_CR,
  EOL_LF,
  EOL_CRLF,
};

typedef struct tagAPPSTATE {
  char **portlist;              /**< list of detected serial ports/devices */
  int numports;                 /**< number of ports in the list */
  int curport;                  /**< currently selected port, index in portlist (-1 if none) */
  bool reconnect;               /**< try to (re-)connect to the port? */
  char console_edit[256];       /**< text to transmit */
  int console_activate;         /**< whether the edit line should get the focus (and whether to move the cursor to the end) */
  bool console_isactive;        /**< whether the console is currently active (for history) */
  HCOM *hCom;                   /**< handle of the open port */
  unsigned baudrate;            /**< active bitrate */
  int databits;                 /**< 5 to 8 */
  int stopbits;                 /**< 1 or 2 */
  int parity;                   /**< none, odd, even, mark, space */
  int flowctrl;                 /**< none, hard, soft (XON/XOFF) */
  unsigned linestatus;          /**< line/modem status (CTS/DSR/DI/CD) */
  unsigned long linestat_tstamp;/**< timestamp of last update of the line/modem status */
  int breakdelay;               /**< delay for making sure BREAK condition is visible */
  int view;                     /**< ASCII or HEX */
  bool reformat_view;           /**< if true, all contents in the viewport are reformatted */
  nk_bool wordwrap;             /**< word-wrap option (ASCII mode) */
  nk_bool scrolltolast;         /**< scroll to bottom on reception of new data */
  char bytesperline[16];        /**< bytes per line (Hex mode), edit field */
  int bytesperline_val;         /**< copied from the edit buffer when the edit field looses focus */
  int recv_timestamp;           /**< whether to prefix timestamp to received text */
  char linelimit[16];           /**< max. number of lines retained in the viewport */
  int linelimit_val;            /**< copied from the edit buffer when the edit field looses focus */
  nk_bool localecho;            /**< echo transmitted text */
  int append_eol;               /**< append CR, LF or CR+LF */
  FILTER filter_root;           /**< highlight filters */
  FILTER filter_edit;           /**< the filter being edited */
  char scriptfile[_MAX_PATH];   /**< path of the script file */
  time_t scriptfiletime;        /**< "modified" timestamp of the loaded file */
  char *script;                 /**< script loaded in memory */
  bool script_reload;           /**< whether the script must be reloaded */
  bool script_block_run;        /**< block running the script (because errors were found) */
  bool script_cache;            /**< whether data from a previous reception must be kept */
  unsigned char *script_recv;   /**< data buffer for the script */
  size_t script_recv_size;      /**< size of the memory buffer */
  struct tcl tcl;               /**< Tcl context (for running the script) */
  bool help_popup;              /**< whether "help" popup is active */
  int viewport_width;           /**< width of the viewport in characters (excluding the width of a timestamp field) */
} APPSTATE;

static bool get_configfile(char *filename, size_t maxsize, const char *basename)
{
  assert(filename != NULL);
  assert(maxsize > 0);
  *filename = '\0';
  if (!folder_AppConfig(filename, maxsize))
    return false;

  strlcat(filename, DIR_SEPARATOR "BlackMagic", maxsize);
# if defined _WIN32
    mkdir(filename);
# else
    mkdir(filename, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
# endif
  strlcat(filename, DIR_SEPARATOR, maxsize);
  strlcat(filename, basename, maxsize);
  return true;
}

static bool save_settings(const char *filename, const APPSTATE *state,
                          const enum nk_collapse_states tab_states[],
                          const SPLITTERBAR *splitter_hor)
{
  assert(filename != NULL);
  assert(state != NULL);
  assert(tab_states != NULL);
  assert(splitter_hor != NULL);

  if (strlen(filename) == 0)
    return false;

  if (state->portlist != NULL && state->curport < state->numports)
    ini_puts("Port", "port", state->portlist[state->curport], filename);
  ini_putl("Port", "baudrate", state->baudrate, filename);
  ini_putl("Port", "databits", state->databits, filename);
  ini_putl("Port", "stopbits", state->stopbits, filename);
  ini_putl("Port", "parity", state->parity, filename);
  ini_putl("Port", "flowcontrol", state->flowctrl, filename);

  ini_putl("Port", "localecho", state->localecho, filename);
  ini_putl("Port", "eol", state->append_eol, filename);

  ini_putl("View", "mode", state->view, filename);
  ini_putl("View", "wordwrap", state->wordwrap, filename);
  ini_putl("View", "scrolltolast", state->scrolltolast, filename);
  ini_putl("View", "bytesperline", state->bytesperline_val, filename);
  ini_putl("View", "timestamp", state->recv_timestamp, filename);
  ini_putl("View", "linemimit", state->linelimit_val, filename);

  ini_putf("Settings", "splitter", splitter_hor->ratio, filename);
  for (int idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    sprintf(key, "view%d", idx);
    ini_putl("Settings", key, tab_states[idx], filename);
  }

  ini_puts("Filters", NULL, NULL, filename);
  int idx = 0;
  for (FILTER *flt = state->filter_root.next; flt != NULL; flt = flt->next) {
    char key[40], data[sizeof(flt->text)+20];
    sprintf(key, "flt%d", idx++);
    sprintf(data, "%d,#%02x%02x%02x,%s", flt->enabled,
            flt->colour.r, flt->colour.g, flt->colour.b, flt->text);
    ini_puts("Filters", key, data, filename);
  }

  ini_puts("Script", "file", state->scriptfile, filename);

  return access(filename, 0) == 0;
}

static bool load_settings(const char *filename, APPSTATE *state,
                          enum nk_collapse_states tab_states[],
                          SPLITTERBAR *splitter_hor)
{
  assert(filename != NULL);
  assert(state != NULL);
  assert(tab_states != NULL);
  assert(splitter_hor != NULL);

  if (state->portlist != NULL && state->numports > 0) {
    char portname[128];
    ini_gets("Port", "port", "", portname, sizearray(portname), filename);
    int idx;
    for (idx = 0; idx < state->numports && stricmp(state->portlist[idx], portname) != 0; idx++)
      {}
    if (idx < state->numports)
      state->curport = idx;
  }
  state->baudrate = ini_getl("Port", "baudrate", 9600, filename);
  state->databits = (int)ini_getl("Port", "databits", 8, filename);
  state->stopbits = (int)ini_getl("Port", "stopbits", 1, filename);
  state->parity = (int)ini_getl("Port", "parity", 0, filename);
  state->flowctrl = (int)ini_getl("Port", "flowcontrol", FLOWCTRL_NONE, filename);

  state->localecho = (nk_bool)ini_getl("Port", "localecho", nk_true, filename);
  state->append_eol = (int)ini_putl("Port", "eol", EOL_CRLF, filename);

  state->view = (int)ini_getl("View", "mode", VIEW_TEXT, filename);
  state->wordwrap = (nk_bool)ini_getl("View", "wordwrap", nk_false, filename);
  state->scrolltolast = (nk_bool)ini_getl("View", "scrolltolast", nk_true, filename);
  state->bytesperline_val = ini_getl("View", "bytesperline", 8, filename);
  if (state->bytesperline_val <= 0)
    state->bytesperline_val = 8;
  sprintf(state->bytesperline, "%d", state->bytesperline_val);
  state->recv_timestamp = (int)ini_getl("View", "timestamp", TIMESTAMP_NONE, filename);
  state->linelimit_val = ini_getl("View", "linelimit", 0, filename);
  if (state->linelimit_val <= 0)
    state->linelimit[0] = '\0';
  else
    sprintf(state->linelimit, "%d", state->linelimit_val);

  splitter_hor->ratio = ini_getf("Settings", "splitter", 0.0, filename);
  if (splitter_hor->ratio < 0.05 || splitter_hor->ratio > 0.95)
    splitter_hor->ratio = 0.70;

  for (int idx = 0; idx < TAB_COUNT; idx++) {
    char key[40], valstr[100];
    int opened, result;
    tab_states[idx] = (idx == TAB_PORTCONFIG || idx == TAB_DISPLAYOPTIONS) ? NK_MAXIMIZED : NK_MINIMIZED;
    sprintf(key, "view%d", idx);
    ini_gets("Settings", key, "", valstr, sizearray(valstr), filename);
    result = sscanf(valstr, "%d", &opened);
    if (result >= 1)
      tab_states[idx] = opened;
  }

  for (int idx = 0;; idx++) {
    char key[40], data[sizeof(state->filter_root.text)+20];
    sprintf(key, "flt%d", idx);
    ini_gets("Filters", key, "", data, sizearray(data), filename);
    if (strlen(data) == 0)
      break;
    int enabled = nk_true;
    int r = 255, g = 255, b = 255;
    char flttext[sizeof(state->filter_root.text)] = "";
    if (sscanf(data, "%d,#%2x%2x%2x,%s", &enabled, &r, &g, &b, flttext) == 5 && strlen(flttext) > 0)
      filter_add(&state->filter_root, flttext, nk_rgb(r, g, b), enabled);
  }

  ini_gets("Script", "file", "", state->scriptfile, sizearray(state->scriptfile), filename);

  return true;
}

static void reformat_data(APPSTATE *state, DATALIST *item);
static void tcl_add_message(APPSTATE *state, const char *text, size_t length, bool isascii)
{
  DATALIST *item = datalist_append((const unsigned char*)text, length, isascii, DFLAG_SCRIPT);
  if (item != NULL)
    reformat_data(state, item); /* format the block of data */
}

static int tcl_cmd_exec(struct tcl *tcl, struct tcl_value *args, void *arg)
{
  (void)arg;
  struct tcl_value *cmd = tcl_list_item(args, 1);
  int retcode = system(tcl_data(cmd));
  tcl_free(cmd);
  return tcl_result(tcl, (retcode >= 0), tcl_value("", 0));
}

static int tcl_cmd_puts(struct tcl *tcl, struct tcl_value *args, void *arg)
{
  (void)arg;
  APPSTATE *state = (APPSTATE*)arg;
  assert(state);
  struct tcl_value *text = tcl_list_item(args, 1);
  tcl_add_message(state, tcl_data(text), tcl_length(text), false);
  return tcl_result(tcl, true, text);
}

static int tcl_cmd_wait(struct tcl *tcl, struct tcl_value *args, void *arg)
{
  (void)arg;
  struct tcl_value *text = tcl_list_item(args, 1);
# if defined _WIN32
    Sleep((int)tcl_number(text));
# else
    usleep((int)tcl_number(text) * 1000);
# endif
  return tcl_result(tcl, true, text);
}

static int tcl_cmd_serial(struct tcl *tcl, struct tcl_value *args, void *arg)
{
  (void)arg;
  APPSTATE *state = (APPSTATE*)arg;
  assert(state);
  int nargs = tcl_list_length(args);
  struct tcl_value *subcmd = tcl_list_item(args, 1);
  if (strcmp(tcl_data(subcmd), "cache") == 0 || strcmp(tcl_data(subcmd), "gobble") == 0) {
    int gobble = INT_MAX; /* default for "serial gobble" is: gobble everything */
    if (strcmp(tcl_data(subcmd), "cache") == 0) {
      state->script_cache = true;
      gobble = 0;         /* default for "serial cache" is: keep everything, gobble nothing */
    }
    if (nargs >= 3) {
      struct tcl_value *v_gobble = tcl_list_item(args, 2);
      gobble = tcl_number(v_gobble);
      tcl_free(v_gobble);
    }
    if (gobble > 0 && gobble < state->script_recv_size) {
      state->script_recv_size -= gobble;
      memmove(state->script_recv, state->script_recv + gobble, state->script_recv_size);
    } else if (gobble != 0) {
      state->script_recv_size = 0;  /* gobble more than there is data -> nothing left */
    }
    if (gobble != 0)
      tcl_var(&state->tcl, "recv", tcl_value((const char*)state->script_recv, state->script_recv_size));
  } else if (strcmp(tcl_data(subcmd), "send") == 0) {
    if (nargs >= 3 && rs232_isopen(state->hCom)) {
      struct tcl_value *data = tcl_list_item(args, 1);
      rs232_xmit(state->hCom, (const unsigned char*)tcl_data(data), tcl_length(data));
      tcl_free(data);
    }
  }
  tcl_free(subcmd);
  return tcl_result(tcl, 1, tcl_value("", 0));
}

static bool tcl_runscript(APPSTATE *state, const unsigned char *data, size_t size)
{
  /* (re-)load the script file */
  assert(state != NULL);
  /* check whether the file date/time changes; if so, also reload the script */
  if (state->script != NULL && strlen(state->scriptfile) > 0) {
    struct stat fstat;
    if (stat(state->scriptfile, &fstat) == 0 && state->scriptfiletime != fstat.st_mtime) {
      state->scriptfiletime =fstat.st_mtime;
      state->script_reload = true;
    }
  }
  if (state->script_reload) {
    state->script_reload = false;
    state->script_block_run = false;
    if (state->script != NULL) {
      free((void*)state->script);
      state->script = NULL;
    }
    if (strlen(state->scriptfile) == 0)
      return false;
    FILE *fp = fopen(state->scriptfile, "rt");
    if (fp == NULL) {
      static const char *msg = "Tcl script file not found.";
      tcl_add_message(state, msg, strlen(msg), false);
      return false;
    }
    fseek(fp, 0, SEEK_END);
    size_t sz = ftell(fp) + 2;
    state->script = malloc((sz)*sizeof(char));
    if (state->script == NULL) {
      static const char *msg = "Memory allocation failure (when loading Tcl script).";
      tcl_add_message(state, msg, strlen(msg), false);
      fclose(fp);
      return false;
    }
    fseek(fp, 0, SEEK_SET);
    memset(state->script, 0, sz);
    char *line = state->script;
    while (fgets(line, sz, fp) != NULL)
      line += strlen(line);
    assert(line - state->script < sz);
    fclose(fp);
  }
  if (state->script == NULL || state->script_block_run)
    return false;
  /* set variables, but first build the memory buffer (together with cached data) */
  size_t total = state->script_recv_size + size;
  unsigned char *buffer = malloc(total * sizeof(unsigned char));
  if (buffer != NULL) {
    if (state->script_recv_size > 0) {
      assert(state->script_recv != NULL);
      memcpy(buffer, state->script_recv, state->script_recv_size);
    }
    memcpy(buffer + state->script_recv_size, data, size);
    if (state->script_recv != NULL)
      free((void*)state->script_recv);
    state->script_recv = buffer;
    state->script_recv_size = total;
  }
  if (state->script_recv == NULL)
    return false; /* these are small allocations, if these go wrong, there is more trouble */
  tcl_var(&state->tcl, "recv", tcl_value((const char*)state->script_recv, state->script_recv_size));
  /* now run it */
  state->script_cache = false;
  bool ok = tcl_eval(&state->tcl, state->script, strlen(state->script) + 1);
  if (!ok) {
    int line;
    char symbol[64];
    const char *err = tcl_errorinfo(&state->tcl, NULL, &line, symbol, sizearray(symbol));
    char msg[256];
    sprintf(msg, "Tcl script error: %s, on or after line %d", err, line);
    if (strlen(symbol) > 0)
      sprintf(msg + strlen(msg), ": %s", symbol);
    tcl_add_message(state, msg, strlen(msg), false);
    state->script_block_run = true; /* block the script from running, until it is reloaded */
  }
  /* if data was marked to be cached, leave it in the buffer; otherwise, free it
     (and always delete a buffer if every data has been gobbled) */
  if ((!state->script_cache || state->script_recv_size == 0) && state->script_recv != NULL) {
    free((void*)state->script_recv);
    state->script_recv = NULL;
    state->script_recv_size = 0;
    state->script_cache = false;
  }
  return ok;
}

static char *format_time(char *buffer, size_t size, unsigned long timestamp, time_t basetime, int format)
{
  assert(format == TIMESTAMP_RELATIVE || TIMESTAMP_ABSOLUTE);
  assert(buffer != NULL);
  assert(size >= 20);
  if (format == TIMESTAMP_RELATIVE) {
    sprintf(buffer, "%9.3f", timestamp/1000.0);
  } else {
    time_t tstamp = basetime + (timestamp + 500) / 1000;
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", localtime(&tstamp));
  }
  return buffer;
}

static bool save_data(const char *filename, const APPSTATE *state)
{
  FILE *fp = fopen(filename, "wt");
  if (fp == NULL)
    return false;

  for (DATALIST *item = datalist_root.next; item != NULL; item = item->next) {
    assert(item->text != NULL);
    for (int lineidx = 0; lineidx < item->textlines; lineidx++) {
      if (state->recv_timestamp != TIMESTAMP_NONE) {
        char buffer[40];
        format_time(buffer, sizearray(buffer), item->timestamp, reception_timestamp, state->recv_timestamp);
        fprintf(fp, "[%s]", buffer);
        fputc((state->view == VIEW_TEXT) ? ' ' : '\n', fp);
      }
      assert(item->text[lineidx] != NULL);
      fputs(item->text[lineidx], fp);
      fputc('\n', fp);
    }
  }

  fclose(fp);
  return true;
}

static void reformat_data(APPSTATE *state, DATALIST *item)
{
  assert(state != NULL);
  assert(item != NULL);

  /* remove old formatting (if any) */
  if (item->text != NULL) {
    for (int idx = 0; idx < item->textlines; idx++)
      free((void*)item->text[idx]);
    free((void*)item->text);
    item->text = NULL;
    item->textlines = 0;
    item->maxlines = 0;
  }

  if (state->view == VIEW_TEXT || (item->flags & DFLAG_SCRIPT)) {
    /* split the data buffer into lines */
    if (state->wordwrap && state->viewport_width == 0)
      state->reformat_view = true;  /* special case, viewport width is not yet calculated */
    int maxchars = (state->wordwrap && state->viewport_width > 0) ? state->viewport_width : INT_MAX;
    int start = 0;
    while (start < item->datasize) {
      int stop = start;
      while (stop < item->datasize && (stop - start) < maxchars
             && item->data[stop] != '\r' && item->data[stop] != '\n')
        stop++;
      if (stop + 1 < item->datasize && item->data[stop] == '\r' && item->data[stop + 1] == '\n') {
        stop += 2;  /* skip \r\n */
      } else if (stop < item->datasize && (item->data[stop] == '\r' || item->data[stop] == '\n')) {
        stop += 1;  /* skip either \r or \n */
      } else if (stop - start >= maxchars) {
        if (item->data[stop] == ' ') {
          stop += 1;  /* gobble up one space at the end of a line */
        } else {
          int pos = stop;
          while (pos > start && item->data[pos - 1] > ' ')
            pos -= 1;
          if (pos > start)
            stop = pos;
        }
      }
      /* convert each line to UTF-8 */
      int len = 0;
      for (int idx = start; idx < stop; idx++) {
        if ((item->data[idx] < ' ' && item->data[idx] != '\r' && item->data[idx] != '\n')
            || (item->data[idx] >= 0x80 && item->data[idx] < 0xa0))
          len += 3; // 0xE2 0x96 0xAB for unknown character
        else if (item->data[idx] >= 0xa0)
          len += 2;
        else
          len += 1;
      }
      char *line = malloc((len + 1) * sizeof(char));
      if (line != NULL) {
        char *pos = line;
        for (int idx = start; idx < stop; idx++) {
          if (item->data[idx] == '\r' || item->data[idx] == '\n'
              || (idx == stop - 1 && item->data[idx] == ' '))
          {
            /* ignore \r and \n in the formatted output, and also ignore a space
               at the end of a string (or segment), because a space is a break
               position in word-wrap */
          } else if ((item->data[idx] < ' ')
              || (item->data[idx] >= 0x80 && item->data[idx] < 0xa0)) {
            memcpy(pos, "\xe2\x96\xab", 3); /* glyph for "unknown character" */
            pos += 3;
          } else if (item->data[idx] >= 0xa0) {
            *pos++ = (char)(0xc0 | ((item->data[idx] >> 6) & 0x3));
            *pos++ = (char)(0x80 | (item->data[idx] & 0x3f));
          } else {
            *pos++ = item->data[idx];
          }
        }
        *pos = '\0';
        /* append to the list */
        if (item->textlines >= item->maxlines) {
          int nummax = (item->maxlines > 0) ? 2 * item->maxlines : 1 + (stop < item->datasize);
          char **text = malloc(nummax * sizeof(char*));
          if (text != NULL) {
            if (item->text != NULL)
              memcpy(text, item->text, item->maxlines * sizeof(char*));
            free((void*)item->text);
            item->text = text;
            item->maxlines = nummax;
          }
        }
        if (item->textlines < item->maxlines) {
          item->text[item->textlines] = strdup(line);
          if (item->text[item->textlines] != NULL)
            item->textlines += 1;
        }
        free(line);
      }
      start = stop;
    }
  } else {
    static const char hexdigit[] = "0123456789ABCDEF";
    /* we know exactly how many rows are needed */
    assert(state->bytesperline_val > 0);
    int numlines = (item->datasize + state->bytesperline_val - 1) / state->bytesperline_val;
    item->text = malloc(numlines * sizeof(char*));
    if (item->text != NULL) {
      item->maxlines = numlines;
      item->textlines = 0;
      int len = 4 * state->bytesperline_val + 4;
      char *line = malloc((len + 1) * sizeof(char));
      if (line != NULL) {
        int start = 0;
        while (start < item->datasize) {
          memset(line, ' ', len);
          line[len - 1] = '\0';
          int stop = start + state->bytesperline_val;
          if (stop > item->datasize)
            stop = item->datasize;
          int pos = 0;
          for (int idx = start; idx < stop; idx++) {
            line[3*pos] = hexdigit[(item->data[idx] >> 4) & 0x0f];
            line[3*pos + 1] = hexdigit[item->data[idx] & 0x0f];
            line[3 * state->bytesperline_val + 2 + pos] = (item->data[idx] >= ' ' && item->data[idx] < 128) ? item->data[idx] : '.';
            pos++;
          }
          item->text[item->textlines] = strdup(line);
          if (item->text[item->textlines] != NULL)
            item->textlines += 1;
          start = stop;
        }
        free((void*)line);
      }
    }
  }
}

static int process_data(APPSTATE *state)
{
  unsigned char buffer[256];
  size_t count = rs232_recv(state->hCom, buffer, sizearray(buffer));
  if (count == 0)
    return 0;

# if !defined _WIN32
    /* In Linux (and similar), frame errors show up in the data stream as a
       byte sequence FF 00, and breaks as the sequence FF 00 00.
       However, such sequences might also occur in normal (binary) transfer,
       and thus, any "normal" FF bytes are doubled. */
    for (int idx = 0; idx < count - 1; idx++) {
      if (buffer[idx] == 0xff) {
        int remove = 2;
        if (buffer[idx + 1] == 0) {
          if (idx < count - 2 && buffer[idx + 2] == 0) {
            state->linestatus |= LINESTAT_BREAK;
            remove = 3;
          } else {
            state->linestatus |= LINESTAT_ERR;
          }
          state->breakdelay = 2;
          memmove(buffer + idx, buffer + idx + remove, count - idx - remove);
          count -= remove;
        } else if (buffer[idx + 1] == 0xff) {
          /* ff ff -> ff (just remove the first) */
          memmove(buffer + idx, buffer + idx + 1, count - idx - 1);
          count -= 1;
        }
      }
    }
# endif

  /* check whether the entirity of the block is ASCII */
  bool isascii = true;
  for (size_t idx = 0; isascii && idx < count; idx++)
    if (buffer[idx] >= '\x80')
      isascii = false;

  /* for ASCII text, add lines (ending with \r and or \n) if possible */
  size_t start = 0;
  while (start < count) {
    size_t stop;
    if (isascii) {
      stop = start;
      while (stop < count && buffer[stop] != '\r' && buffer[stop] != '\n')
        stop++;
      if (stop + 1 < count && buffer[stop] == '\r' && buffer[stop + 1] == '\n')
        stop += 2;  /* skip \r\n */
      else if (stop < count)
        stop += 1;  /* skip either \r or \n */
    } else {
      stop = count;
    }

    DATALIST *item = datalist_append(buffer + start, stop - start, isascii, 0);
    if (item != NULL)
      reformat_data(state, item); /* format (or re-format) the block of data */

    start = stop;
  }

  /* run script after handling raw data */
  tcl_runscript(state, buffer, count);

  if (state->linelimit_val > 0) {
    /* count number of blocks in the datalist, then decide home many lines to
       drop */
    int numlines = 0;
    for (DATALIST *item = datalist_root.next; item != NULL; item = item->next)
      numlines++;
    int drop = numlines - state->linelimit_val;
    while (drop-- > 0) {
      DATALIST *item = datalist_root.next;
      datalist_root.next = item->next;
      datalist_freeitem(item);
    }
  }

  return count;
}

static void free_portlist(APPSTATE *state)
{
  assert(state != NULL);
  if (state->portlist != NULL) {
    for (int idx = 0; idx < state->numports; idx++)
      if (state->portlist[idx] != NULL)
        free((void*)state->portlist[idx]);
    free((void*)state->portlist);
    state->portlist = NULL;
    state->numports = 0;
  }
}

static void collect_portlist(APPSTATE *state)
{
  assert(state != NULL);

  /* free the current portlist (if any) before collecting the up-to-date list,
     but before doing so, save the name of the open port (if any) */
  char portname[128] = "";
  if (state->portlist != NULL && state->curport < state->numports)
    strlcpy(portname, state->portlist[state->curport], sizearray(portname));
  free_portlist(state);
  state->curport = 0;

  int count = rs232_collect(NULL, 0);
  if (count > 0) {
    state->portlist = malloc(count * sizeof(char*));
    if (state->portlist != NULL) {
      state->numports = rs232_collect(state->portlist, count);
      /* restore the port index to the saved port name */
      int idx;
      for (idx = 0; idx < state->numports && stricmp(state->portlist[idx], portname) != 0; idx++)
        {}
      if (idx != state->curport || state->numports == 0)
        state->reconnect = true;
      if (idx < state->numports)
        state->curport = idx;
    }
  }
}

static int value_listindex(long value, const char **list, int entries)
{
  assert(list != NULL);
  int idx;
  for (idx = 0; idx < entries && strtol(list[idx], NULL, 10) != value; idx++)
    {}
  return (idx < entries) ? idx : -1;
}

static void widget_monitor(struct nk_context *ctx, const char *id, APPSTATE *state, float rowheight, nk_flags widget_flags)
{
  assert(ctx != NULL);
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window *stwin = &ctx->style.window;

  /* monospaced font */
  int fonttype = guidriver_setfont(ctx, FONT_MONO);
  struct nk_user_font const *font = ctx->style.font;

  /* calculate viewport & timestamp field width */
  assert(font != NULL && font->width != NULL);
  float charwidth = font->width(font->userdata, font->height, "1234567890", 10) / 10.0;
  float timefield_width = 0;
  if (state->recv_timestamp != TIMESTAMP_NONE) {
    char buffer[40];
    format_time(buffer, sizearray(buffer), 0, 0, state->recv_timestamp);
    timefield_width = strlen(buffer) * charwidth + 2 * stwin->padding.x;
  }
  state->viewport_width = (int)((rcwidget.w - timefield_width - 2 * stwin->padding.x - 4) / charwidth);

  nk_style_push_color(ctx, &stwin->fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, widget_flags)) {
    static int scrollpos = 0;
    static int prev_linecount = 0;
    float lineheight = 0.0;
    float vpwidth = 0.0;
    int cur_linecount = 0;
    for (DATALIST *item = datalist_root.next; item != NULL; item = item->next) {
      assert(item->text != NULL);
      for (int lineidx = 0; lineidx < item->textlines; lineidx++) {
        cur_linecount++;
        assert(item->text[lineidx] != NULL);
        nk_layout_row_begin(ctx, NK_STATIC, rowheight, 1 + (timefield_width > 1));
        if (lineheight <= 0.1) {
          struct nk_rect rcline = nk_layout_widget_bounds(ctx);
          lineheight = rcline.h;
          vpwidth = rcline.w;
        }
        if (timefield_width > 1) {
          nk_layout_row_push(ctx, timefield_width);
          if (lineidx == 0) {
            unsigned long tstamp = item->timestamp;
            if (state->recv_timestamp == TIMESTAMP_ABSOLUTE)
              tstamp += datalist_root.timestamp;
            char buffer[40];
            format_time(buffer, sizearray(buffer), tstamp, reception_timestamp, state->recv_timestamp);
            nk_text_colored(ctx, buffer, strlen(buffer), NK_TEXT_LEFT, COLOUR_FG_CYAN);
          } else {
            nk_spacing(ctx, 1);
          }
        }
        struct nk_color fgcolour = COLOUR_TEXT;
        if (item->flags & DFLAG_SCRIPT)
          fgcolour = COLOUR_FG_GREEN;
        else if (item->flags & DFLAG_LOCAL)
          fgcolour = COLOUR_FG_AQUA;
        int len = strlen(item->text[lineidx]);
        float textwidth = len * charwidth + 8;
        if (textwidth < vpwidth - timefield_width)
          textwidth = vpwidth - timefield_width;
        nk_layout_row_push(ctx, textwidth);
        FILTER* flt = filter_match(&state->filter_root, item->text[lineidx]);
        if (flt != NULL) {
          struct nk_rect rcline = nk_widget_bounds(ctx);
          nk_fill_rect(&ctx->current->buffer, rcline, 0, flt->colour);
          fgcolour = CONTRAST_COLOUR(flt->colour);
        }
        nk_text_colored(ctx, item->text[lineidx], len, NK_TEXT_LEFT, fgcolour);
        nk_layout_row_end(ctx);
      }
    }
    nk_layout_row_dynamic(ctx, rowheight, 1);
    if (cur_linecount == 0 && !rs232_isopen(state->hCom)) {
      nk_label_colored(ctx, "No Connection", NK_TEXT_CENTERED, COLOUR_FG_RED);
    } else {
      /* append an empty line to the viewport, to be sure that the "scrolling
         to the last line" won't show a truncated last line */
      nk_spacing(ctx, 1);
    }
    nk_group_end(ctx);
    /* calculate scrolling: if number of lines change, scroll to the last line */
    if (state->scrolltolast) {
      int ypos = scrollpos;
      if (cur_linecount != prev_linecount) {
        prev_linecount = cur_linecount;
        float widgetlines = rcwidget.h - 2 * stwin->padding.y;
        ypos = (int)((cur_linecount + 1) * lineheight - widgetlines);
      }
      if (ypos < 0)
        ypos = 0;
      if (ypos != scrollpos) {
        nk_group_set_scroll(ctx, id, 0, ypos);
        scrollpos = ypos;
      }
    }
  }
  nk_style_pop_color(ctx);
  guidriver_setfont(ctx, fonttype);
}

static void widget_lineinput(struct nk_context *ctx, APPSTATE *state)
{
# define SPACING 4

  assert(ctx != NULL);
  assert(state != NULL);
  int edtflags = 0;
  nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
  struct nk_rect rcline = nk_layout_widget_bounds(ctx);
  nk_layout_row_push(ctx, rcline.w - 2 * ROW_HEIGHT - SPACING);
  if (state->hCom != NULL) {
    if (state->console_activate) {
      nk_edit_focus(ctx, (state->console_activate == 2) ? NK_EDIT_GOTO_END_ON_ACTIVATE : 0);
      state->console_activate = 1;
    }
    edtflags = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                              state->console_edit, sizearray(state->console_edit),
                                              nk_filter_ascii);
    state->console_isactive = ((edtflags & NK_EDIT_ACTIVE) != 0);
  } else {
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_READ_ONLY|NK_EDIT_NO_CURSOR,
                                   state->console_edit, sizearray(state->console_edit), nk_filter_ascii);
    state->console_edit[0] = '\0';
  }
  nk_layout_row_push(ctx, 2*ROW_HEIGHT);
  int btnclicked = button_tooltip(ctx, "Send", NK_KEY_NONE, (state->hCom != NULL), "Transmit text or data");
  nk_layout_row_end(ctx);

  if ((edtflags & NK_EDIT_COMMITED) != 0 || btnclicked) {
    size_t len = strlen(state->console_edit);
    if (len > 0) {
      /* calculate size (translate \xx codes  */
      size_t size = 0;
      for (int idx = 0; idx < len; idx++) {
        if (state->console_edit[idx] == '`' && isxdigit(state->console_edit[idx + 1]) && isxdigit(state->console_edit[idx + 2])) {
          while (isxdigit(state->console_edit[idx + 1]) && isxdigit(state->console_edit[idx + 2])) {
            idx += 2;
            size += 1;
          }
        } else {
          size += 1;
        }
      }
      if (state->append_eol == EOL_CR || state->append_eol == EOL_LF)
        size += 1;
      else if (state->append_eol == EOL_CRLF)
        size += 2;
      assert(size > 0);
      unsigned char *buffer = malloc(size * sizeof(unsigned char));
      if (buffer != NULL) {
        int pos = 0;
        for (int idx = 0; idx < len; idx++) {
          if (state->console_edit[idx] == '`' && isxdigit(state->console_edit[idx + 1]) && isxdigit(state->console_edit[idx + 2])) {
            while (isxdigit(state->console_edit[idx + 1]) && isxdigit(state->console_edit[idx + 2])) {
              char c1 = toupper(state->console_edit[idx + 1]);
              char c2 = toupper(state->console_edit[idx + 2]);
              unsigned char v1 = (c1 <= '9') ? c1 - '0' : c1 - 'A' + 10;
              unsigned char v2 = (c2 <= '9') ? c2 - '0' : c2 - 'A' + 10;
              buffer[pos++] = (v1 << 4) | v2;
              idx += 2;
            }
          } else {
            buffer[pos++] = state->console_edit[idx];
          }
        }
        if (state->append_eol == EOL_CR)
          memcpy(buffer + pos, "\r", 1);
        else if (state->append_eol == EOL_LF)
          memcpy(buffer + pos, "\n", 1);
        else if (state->append_eol == EOL_CRLF)
          memcpy(buffer + pos, "\r\n", 2);
        rs232_xmit(state->hCom, buffer, size);
        if (state->localecho) {
          DATALIST *item = datalist_append(buffer, size, false, DFLAG_LOCAL);
          if (item != NULL)
            reformat_data(state, item);
        }
        free((void*)buffer);
      }
      state->console_edit[0] = '\0';
    }
  }
# undef SPACING
}

static void help_popup(struct nk_context *ctx, APPSTATE *state, float canvas_width, float canvas_height)
{
# include "bmserial_help.h"

  if (state->help_popup) {
#   define MARGIN  10
    float w = opt_fontsize * 40;
    if (w > canvas_width - 2*MARGIN)  /* clip "ideal" help window size of canvas size */
      w = canvas_width - 2*MARGIN;
    float h = canvas_height * 0.75;
    struct nk_rect rc = nk_rect((canvas_width - w) / 2, (canvas_height - h) / 2, w, h);
#   undef MARGIN

    state->help_popup = nk_guide(ctx, &rc, opt_fontsize, (const char*)bmserial_help, NULL);
  }
}

static void panel_portconfig(struct nk_context *ctx, APPSTATE *state,
                             enum nk_collapse_states tab_states[TAB_COUNT],
                             float panel_width)
{
  static const char *empty_portlist[] = { "(no port)" };
  static const char *baud_strings[] = { "1200", "2400", "4800", "9600", "14400",
                                        "19200", "28800", "38400", "57600",
                                        "115200", "230400" };
  static const char *datab_strings[] = { "5", "6", "7", "8" };
  static const char *stopb_strings[] = { "1", "2" };
  static const char *parity_strings[] = { "None", "Odd", "Even", "Mark", "Space" };
  static const char *flowctrl_strings[] = { "None", "RTS / CTS", "XON / XOFF" };

# define SPACING     4
# define LABEL_WIDTH (5.5 * opt_fontsize)
# define VALUE_WIDTH (panel_width - LABEL_WIDTH - (2 * SPACING + 18))

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", &tab_states[TAB_PORTCONFIG])) {
    int result, curidx;
    struct nk_rect bounds;

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Port", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bounds = nk_widget_bounds(ctx);
    const char **portlist = (state->portlist != NULL) ? (const char**)state->portlist : empty_portlist;
    int numports = (state->portlist != NULL) ? state->numports : 1;
    result = nk_combo(ctx, portlist, numports, state->curport,
                      (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
    nk_layout_row_end(ctx);
    if (result != state->curport) {
      state->curport = result;
      state->reconnect = true;
    }

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Baudrate", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bounds = nk_widget_bounds(ctx);
    curidx = value_listindex(state->baudrate, baud_strings, sizearray(baud_strings));
    result = nk_combo(ctx, baud_strings, sizearray(baud_strings), curidx,
                      (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
    nk_layout_row_end(ctx);
    if (result != curidx) {
      state->baudrate = strtol(baud_strings[result], NULL, 10);
      state->reconnect = true;
    }

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Data bits", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bounds = nk_widget_bounds(ctx);
    curidx = value_listindex(state->databits, datab_strings, sizearray(datab_strings));
    result = nk_combo(ctx, datab_strings, sizearray(datab_strings), curidx,
                      (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
    nk_layout_row_end(ctx);
    if (result != curidx) {
      state->databits = strtol(datab_strings[result], NULL, 10);
      state->reconnect = true;
    }

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Stop bits", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bounds = nk_widget_bounds(ctx);
    curidx = value_listindex(state->stopbits, stopb_strings, sizearray(stopb_strings));
    result = nk_combo(ctx, stopb_strings, sizearray(stopb_strings), curidx,
                      (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
    nk_layout_row_end(ctx);
    if (result != curidx) {
      state->stopbits = strtol(stopb_strings[result], NULL, 10);
      state->reconnect = true;
    }

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Parity", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bounds = nk_widget_bounds(ctx);
    result = nk_combo(ctx, parity_strings, sizearray(parity_strings), state->parity,
                      (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
    nk_layout_row_end(ctx);
    if (result != state->parity) {
      state->parity = result;
      state->reconnect = true;
    }

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Flow control", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bounds = nk_widget_bounds(ctx);
    result = nk_combo(ctx, flowctrl_strings, sizearray(flowctrl_strings), state->flowctrl,
                      (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
    nk_layout_row_end(ctx);
    if (result != state->flowctrl) {
      state->flowctrl = result;
      state->reconnect = true;
    }

    nk_tree_state_pop(ctx);
  }
# undef LABEL_WIDTH
# undef VALUE_WIDTH
# undef SPACING
}

static int nk_ledbutton(struct nk_context *ctx, const char *label, const char *tiptext,
                        struct nk_color color, nk_bool enabled)
{
  struct nk_style_button *button = &ctx->style.button;
  struct nk_style_button save_style = *button;
  struct nk_color textcolor = ((color.r + 2*color.g + color.b) > 400) ? COLOUR_BG0_S : COLOUR_TEXT;
  button->normal = button->hover = button->active = nk_style_item_color(color);
  button->text_background = color;
  button->text_normal = button->text_hover = button->text_active = textcolor;
  button->border = 1;
  int font = guidriver_setfont(ctx, FONT_SMALL);
  struct nk_rect bounds = nk_widget_bounds(ctx);
  int result = nk_button_label(ctx, label);
  if (tiptext != NULL)
    tooltip(ctx, bounds, tiptext);
  ctx->style.button = save_style;
  guidriver_setfont(ctx, font);

  if (!enabled)
    result = 0;
  return result;
}

static void panel_linestatus(struct nk_context *ctx, APPSTATE *state,
                             enum nk_collapse_states tab_states[TAB_COUNT],
                             float panel_width)
{
# define LABEL_WIDTH   (4 * opt_fontsize)
# define BUTTON_WIDTH  ((panel_width - LABEL_WIDTH) / 3 - 12)
# define BUTTON_HEIGHT (ROW_HEIGHT * 0.6)

  const char *caption = "Line status";
  if (!rs232_isopen(state->hCom)) {
    nk_style_push_color(ctx, &ctx->style.tab.text, COLOUR_FG_RED);
    nk_style_push_color(ctx, &ctx->style.tab.tab_maximize_button.text_normal, COLOUR_FG_RED);
    nk_style_push_color(ctx, &ctx->style.tab.tab_minimize_button.text_normal, COLOUR_FG_RED);
    caption = "No connection";
  }
  if (nk_tree_state_push(ctx, NK_TREE_TAB, caption, &tab_states[TAB_LINESTATUS])) {
    /* update the status roughly every 0.1 second */
    unsigned long tstamp = timestamp();
    if (tstamp - state->linestat_tstamp >= 100) {
      if (rs232_isopen(state->hCom)) {
        unsigned delayedstat = 0;
        if (state->breakdelay > 0) {
          delayedstat = state->linestatus & (LINESTAT_LBREAK | LINESTAT_BREAK | LINESTAT_ERR);
          state->breakdelay -= 1;
          /* local break must be toggled of manually in Windows (it does not
             need so in Linx, but it does no harm either) */
          if (state->breakdelay == 0 && (delayedstat & LINESTAT_LBREAK) != 0)
            rs232_setstatus(state->hCom, LINESTAT_LBREAK, 0);
        }
#       if defined _WIN32
          /* Windows does not return the status that the host sets itself, so
             these must be saved, and merged back in */
          unsigned localstat = state->linestatus & (LINESTAT_RTS | LINESTAT_DTR);
          delayedstat |= localstat;
#       endif
        state->linestatus = rs232_getstatus(state->hCom);
        /* if a break or frame error are detected in the active states (as opposed
           to the delayed status), make sure it stays "on" for long enough to
           be visible */
        if (state->linestatus & (LINESTAT_BREAK | LINESTAT_ERR))
          state->breakdelay = 2;
        state->linestatus |= delayedstat;
      } else {
        state->linestatus = 0;
        state->breakdelay = 0;
        state->hCom = NULL;
      }
      state->linestat_tstamp = tstamp;
    }

    struct nk_color clr_on = COLOUR_BG_RED;
    struct nk_color clr_off = COLOUR_BG0;

    nk_layout_row_begin(ctx, NK_STATIC, BUTTON_HEIGHT, 4);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Local", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);

    nk_layout_row_push(ctx, BUTTON_WIDTH);
    const char *ttip = (state->flowctrl != FLOWCTRL_RTSCTS) ? "Request To Send\nClick to toggle" : "Request To Send\nHandled by hardware flow control";
    if (nk_ledbutton(ctx, "RTS", ttip,
                     (state->linestatus & LINESTAT_RTS) ? clr_on : clr_off,
                     state->hCom != NULL && state->flowctrl != FLOWCTRL_RTSCTS))
    {
      if (state->linestatus & LINESTAT_RTS)
        state->linestatus &= ~LINESTAT_RTS;
      else
        state->linestatus |= LINESTAT_RTS;
      rs232_setstatus(state->hCom, LINESTAT_RTS, (state->linestatus & LINESTAT_RTS) != 0);
    }
    nk_layout_row_push(ctx, BUTTON_WIDTH);
    if (nk_ledbutton(ctx, "DTR", "Data Terminal Ready\nClick to toggle",
                     (state->linestatus & LINESTAT_DTR) ? clr_on : clr_off,
                     state->hCom != NULL))
    {
      if (state->linestatus & LINESTAT_DTR)
        state->linestatus &= ~LINESTAT_DTR;
      else
        state->linestatus |= LINESTAT_DTR;
      rs232_setstatus(state->hCom, LINESTAT_DTR, (state->linestatus & LINESTAT_DTR) != 0);
    }
    nk_layout_row_push(ctx, BUTTON_WIDTH);
    if (nk_ledbutton(ctx, "BREAK", "Click to send \"break\" signal",
                     (state->linestatus & LINESTAT_LBREAK) ? clr_on : clr_off,
                     state->hCom != NULL))
    {
      state->linestatus |= LINESTAT_LBREAK;
      state->breakdelay = 2;
      rs232_setstatus(state->hCom, LINESTAT_LBREAK, 1);
    }
    nk_layout_row_end(ctx);

    nk_layout_row(ctx, NK_DYNAMIC, 2, 3, nk_ratio(3, 0.025, 0.95, 0.025));
    nk_spacing(ctx, 1);
    nk_rule_horizontal(ctx, COLOUR_FG_GRAY, nk_false);
    nk_spacing(ctx, 1);

    nk_layout_row_begin(ctx, NK_STATIC, BUTTON_HEIGHT, 4);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Remote", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);

    nk_layout_row_push(ctx, BUTTON_WIDTH);
    nk_ledbutton(ctx, "CTS", "Clear To Send\nStatus set by remote host",
                 (state->linestatus & LINESTAT_CTS) ? clr_on : clr_off, nk_false);
    nk_layout_row_push(ctx, BUTTON_WIDTH);
    nk_ledbutton(ctx, "DSR", "Data Set Ready\nStatus set by remote host",
                 (state->linestatus & LINESTAT_DSR) ? clr_on : clr_off, nk_false);
    nk_layout_row_push(ctx, BUTTON_WIDTH);
    nk_ledbutton(ctx, "BREAK", "Remote host sent \"break\" signal",
                 (state->linestatus & LINESTAT_BREAK) ? clr_on : clr_off, nk_false);
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_STATIC, BUTTON_HEIGHT, 4);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, BUTTON_WIDTH);
    nk_ledbutton(ctx, "RI", "Ring Indicator\nModem status",
                 (state->linestatus & LINESTAT_RI) ? clr_on : clr_off, nk_false);
    nk_layout_row_push(ctx, BUTTON_WIDTH);
    nk_ledbutton(ctx, "CD", "Carrier Detect\nModem status",
                 (state->linestatus & LINESTAT_CD) ? clr_on : clr_off, nk_false);
    nk_layout_row_push(ctx, BUTTON_WIDTH);
    nk_ledbutton(ctx, "ERR", "Framing or parity error detected",
                 (state->linestatus & LINESTAT_ERR) ? clr_on : clr_off, nk_false);
    nk_layout_row_end(ctx);

    nk_tree_state_pop(ctx);
  }
  if (!rs232_isopen(state->hCom)) {
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
  }
# undef LABEL_WIDTH
# undef BUTTON_WIDTH
# undef BUTTON_HEIGHT
}

static void panel_displayoptions(struct nk_context *ctx, APPSTATE *state,
                                 enum nk_collapse_states tab_states[TAB_COUNT],
                                 float panel_width)
{
# define SPACING     4
# define LABEL_WIDTH (5.5 * opt_fontsize)
# define VALUE_WIDTH (panel_width - LABEL_WIDTH - (2 * SPACING + 18))

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Display options", &tab_states[TAB_DISPLAYOPTIONS])) {

    int result;

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "View mode", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH / 2);
    int curview = state->view;
    if (option_tooltip(ctx, "Text", (state->view == VIEW_TEXT), NK_TEXT_LEFT, "Display received data as text"))
      state->view = VIEW_TEXT;
    nk_layout_row_push(ctx, VALUE_WIDTH / 2);
    if (option_tooltip(ctx, "Hex", (state->view == VIEW_HEX), NK_TEXT_LEFT, "Display received data as hex dump"))
      state->view = VIEW_HEX;
    nk_layout_row_end(ctx);
    if (state->view != curview)
      state->reformat_view = true;

    if (state->view == VIEW_TEXT) {
      nk_bool cur_wrap = state->wordwrap;
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
      checkbox_tooltip(ctx, "Word-wrap", &state->wordwrap, NK_TEXT_LEFT,
                       "Wrap lines at the edge of the viewport");
      if (state->wordwrap != cur_wrap)
        state->reformat_view = true;
    } else {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "Bytes / line", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                state->bytesperline, sizearray(state->bytesperline),
                                nk_filter_decimal, "The number of bytes on a line");
      nk_layout_row_end(ctx);
      if ((result & (NK_EDIT_DEACTIVATED | NK_EDIT_COMMITED)) != 0) {
        int cur_bpl = state->bytesperline_val;
        state->bytesperline_val = strtol(state->bytesperline, NULL, 10);
        if (state->bytesperline_val <= 0) {
          state->bytesperline_val = 8;
          sprintf(state->bytesperline, "%d", state->bytesperline_val);
        }
        if (state->bytesperline_val != cur_bpl)
          state->reformat_view = true;
      } else if (result & NK_EDIT_ACTIVATED) {
        state->console_activate = 0;
      }
    }

    int cur_timestamp = state->recv_timestamp;
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    nk_bool add_tstamp = (state->recv_timestamp != TIMESTAMP_NONE);
    checkbox_tooltip(ctx, "Timestamp", &add_tstamp, NK_TEXT_LEFT,
                     "Add timestamp to the received data.");
    if (add_tstamp) {
      if (state->recv_timestamp == TIMESTAMP_NONE)
        state->recv_timestamp = TIMESTAMP_RELATIVE;
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
      nk_layout_row_push(ctx, 1.0 * opt_fontsize);
      nk_spacing(ctx, 1);
      nk_layout_row_push(ctx, 5 * opt_fontsize);
      if (option_tooltip(ctx, "Relative", (state->recv_timestamp == TIMESTAMP_RELATIVE),
                         NK_TEXT_LEFT, "Milliseconds since the first reception"))
        state->recv_timestamp = TIMESTAMP_RELATIVE;
      nk_layout_row_push(ctx, 5 * opt_fontsize);
      if (option_tooltip(ctx, "Absolute", (state->recv_timestamp == TIMESTAMP_ABSOLUTE),
                         NK_TEXT_LEFT, "Wall-clock time"))
        state->recv_timestamp = TIMESTAMP_ABSOLUTE;
      nk_layout_row_end(ctx);
    } else {
      state->recv_timestamp = TIMESTAMP_NONE;
    }
    if (state->recv_timestamp != cur_timestamp)
      state->reformat_view = true;

    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    checkbox_tooltip(ctx, "Scroll to last", &state->scrolltolast, NK_TEXT_LEFT,
                     "Scroll to bottom of the viewport on reception of new data");

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Line limit", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                              state->linelimit, sizearray(state->linelimit), nk_filter_decimal,
                              "The maximum number of lines kept in the viewport (zero = unlimited)");
    nk_layout_row_end(ctx);
    if ((result & (NK_EDIT_DEACTIVATED | NK_EDIT_COMMITED)) != 0) {
      state->linelimit_val = strtol(state->linelimit, NULL, 10);
      if (state->linelimit_val <= 0)
        state->linelimit[0] = '\0';
    } else if (result & NK_EDIT_ACTIVATED) {
      state->console_activate = 0;
    }

    nk_tree_state_pop(ctx);
  }
# undef LABEL_WIDTH
# undef VALUE_WIDTH
# undef SPACING

  if (state->reformat_view) {
    for (DATALIST *item = datalist_root.next; item != NULL; item = item->next)
      reformat_data(state, item); /* re-format the block of data */
  }
}

static void panel_transmitoptions(struct nk_context *ctx, APPSTATE *state,
                                  enum nk_collapse_states tab_states[TAB_COUNT],
                                  float panel_width)
{
# define SPACING     4
# define LABEL_WIDTH (1.0 * opt_fontsize)
# define VALUE_WIDTH (panel_width - LABEL_WIDTH - (2 * SPACING + 18))

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Local options", &tab_states[TAB_TRANSMITOPTIONS])) {
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    checkbox_tooltip(ctx, "Local echo", &state->localecho, NK_TEXT_LEFT,
                     "Copy transmitted text to the viewport");

    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    nk_bool append = (state->append_eol != EOL_NONE);
    checkbox_tooltip(ctx, "Append EOL", &append, NK_TEXT_LEFT,
                     "Append CR, LF or CR+LF to transmitted text");
    if (append) {
      if (state->append_eol == EOL_NONE)
        state->append_eol = EOL_CR;
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 4);
      nk_layout_row_push(ctx, 1.0 * opt_fontsize);
      nk_spacing(ctx, 1);
      nk_layout_row_push(ctx, 3.0 * opt_fontsize);
      if (nk_option_label(ctx, "CR", (state->append_eol == EOL_CR), NK_TEXT_LEFT))
        state->append_eol = EOL_CR;
      nk_layout_row_push(ctx, 3.0 * opt_fontsize);
      if (nk_option_label(ctx, "LF", (state->append_eol == EOL_LF), NK_TEXT_LEFT))
        state->append_eol = EOL_LF;
      nk_layout_row_push(ctx, 4.0 * opt_fontsize);
      if (nk_option_label(ctx, "CR+LF", (state->append_eol == EOL_CRLF), NK_TEXT_LEFT))
        state->append_eol = EOL_CRLF;
      nk_layout_row_end(ctx);
    } else {
      state->append_eol = EOL_NONE;
    }

    nk_tree_state_pop(ctx);
  }
# undef LABEL_WIDTH
# undef VALUE_WIDTH
# undef SPACING
}

static void panel_filters(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT],
                          float panel_width)
{
# define SPACING       4
# define ENABLED_WIDTH (2.0 * opt_fontsize)
# define BUTTON_WIDTH  (1.6 * opt_fontsize)
# define LABEL_WIDTH   (panel_width - ENABLED_WIDTH - BUTTON_WIDTH - (3 * SPACING + 18))

  struct nk_style_button stbtn = ctx->style.button;
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Highlight filters", &tab_states[TAB_FILTERS])) {

    for (FILTER *flt = state->filter_root.next, *next = NULL; flt != NULL; flt = next) {
      next = flt->next; /* save pointer to next filter, for the case the current filter is delected */
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 4);
      nk_layout_row_push(ctx, ENABLED_WIDTH);
      checkbox_tooltip(ctx, "", &flt->enabled, NK_TEXT_LEFT, "Enable / disable filter");
      nk_layout_row_push(ctx, LABEL_WIDTH);
      struct nk_rect bounds = nk_widget_bounds(ctx);
      bounds.x -= SPACING;
      bounds.w += 2*SPACING;
      nk_fill_rect(&ctx->current->buffer, bounds, 0, flt->colour);
      struct nk_color c = CONTRAST_COLOUR(flt->colour);
      nk_label_colored(ctx, flt->text, NK_TEXT_LEFT, c);
      nk_layout_row_push(ctx, BUTTON_WIDTH);
      if (button_symbol_tooltip(ctx, NK_SYMBOL_X, NK_KEY_NONE, nk_true, "Remove this filter"))
        filter_delete(&state->filter_root, flt);
      nk_layout_row_end(ctx);
    }

    /* row with edit fields at the bottom (for new filter) */
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 4);

    nk_layout_row_push(ctx, ENABLED_WIDTH);
    struct nk_rect bounds = nk_widget_bounds(ctx);
    stbtn.normal.data.color = stbtn.hover.data.color
      = stbtn.active.data.color = stbtn.text_background = state->filter_edit.colour;
    if (nk_button_label_styled(ctx, &stbtn, "")) {
      /* we want a contextual pop-up (that you can simply click away
         without needing a close button), so we simulate a right-mouse
         click */
      nk_input_motion(ctx, bounds.x, bounds.y + bounds.h - 1);
      nk_input_button(ctx, NK_BUTTON_RIGHT, bounds.x, bounds.y + bounds.h - 1, 1);
      nk_input_button(ctx, NK_BUTTON_RIGHT, bounds.x, bounds.y + bounds.h - 1, 0);
    }
    tooltip(ctx, bounds, "Highlight colour; click to change");

    nk_layout_row_push(ctx, LABEL_WIDTH);
    int edtflags = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                    state->filter_edit.text, sizearray(state->filter_edit.text),
                                    nk_filter_ascii, "Keyword for a new highlight filter");
    if (edtflags & NK_EDIT_ACTIVATED)
      state->console_activate = 0;

    nk_layout_row_push(ctx, BUTTON_WIDTH);
    if (button_symbol_tooltip(ctx, NK_SYMBOL_PLUS, NK_KEY_NONE, nk_true, "Add this filter")
        || (edtflags & NK_EDIT_COMMITED) != 0)
    {
      char *ptr = state->filter_edit.text;
      while (*ptr != '\0' && *ptr <= ' ')
        ptr++;
      if (*ptr !='\0') {
        filter_add(&state->filter_root, ptr, state->filter_edit.colour, nk_true);
        memset(&state->filter_edit, 0, sizeof state->filter_edit);
        state->filter_edit.colour = filter_defcolour(&state->filter_root);
      }
    }
    nk_layout_row_end(ctx);

    /* pop up for the colour selection */
    if (nk_contextual_begin(ctx, 0, nk_vec2(9*opt_fontsize, 4*ROW_HEIGHT), bounds)) {
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
      state->filter_edit.colour.r = (nk_byte)nk_propertyi(ctx, "#R", 0, state->filter_edit.colour.r, 255, 1, 1);
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
      state->filter_edit.colour.g = (nk_byte)nk_propertyi(ctx, "#G", 0, state->filter_edit.colour.g, 255, 1, 1);
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
      state->filter_edit.colour.b = (nk_byte)nk_propertyi(ctx, "#B", 0, state->filter_edit.colour.b, 255, 1, 1);
      nk_contextual_end(ctx);
    }

    nk_tree_state_pop(ctx);
  }
# undef ENABLED_WIDTH
# undef LABEL_WIDTH
# undef COLOUR_WIDTH
# undef BUTTON_WIDTH
# undef SPACING
}

static void panel_script(struct nk_context *ctx, APPSTATE *state,
                         enum nk_collapse_states tab_states[TAB_COUNT],
                         float panel_width)
{
# define SPACING     4
# define LABEL_WIDTH (2 * opt_fontsize)
# define BROWSEBTN_WIDTH (1.5 * opt_fontsize)
# define VALUE_WIDTH (panel_width - LABEL_WIDTH - BROWSEBTN_WIDTH - (3 * SPACING + 18))

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Script", &tab_states[TAB_SCRIPT])) {
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "File", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bool patherror = (strlen(state->scriptfile) > 0 && access(state->scriptfile, 0) != 0);
    if (patherror)
      nk_style_push_color(ctx, &ctx->style.edit.text_normal, COLOUR_FG_RED);
    int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                  state->scriptfile, sizearray(state->scriptfile),
                                  nk_filter_ascii, "TCL script");
    if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
      state->script_reload = true;
    if (patherror)
      nk_style_pop_color(ctx);
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
      const char *filter = "TCL files\0*.tcl\0All files\0*\0";
      int res = noc_file_dialog_open(state->scriptfile, sizearray(state->scriptfile),
                                     NOC_FILE_DIALOG_OPEN, filter, NULL,
                                     state->scriptfile, "Select TCL script file",
                                     guidriver_apphandle());
      if (res)
        state->script_reload = true;
    }
    nk_layout_row_end(ctx);

    nk_tree_state_pop(ctx);
  }
# undef LABEL_WIDTH
# undef BROWSEBTN_WIDTH
# undef VALUE_WIDTH
# undef SPACING
}

static void button_bar(struct nk_context *ctx, APPSTATE *state)
{
  nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(4, 0.25, 0.25, 0.25, 0.25));

  const char* ptr = rs232_isopen(state->hCom) ? "Disconnect" : "Connect";
  if (nk_button_label(ctx, ptr)) {
    if (rs232_isopen(state->hCom)) {
      rs232_close(state->hCom);
      state->hCom = NULL;
    } else {
      state->reconnect = true;
    }
  }

  if (nk_button_label(ctx, "Clear")) {
    datalist_clear();
  }

  if (nk_button_label(ctx, "Save") || nk_input_is_key_pressed(&ctx->input, NK_KEY_SAVE)) {
    char path[_MAX_PATH];
    int res = noc_file_dialog_open(path, sizearray(path), NOC_FILE_DIALOG_SAVE,
                                   "Text files\0*.txt\0All files\0*.*\0",
                                   NULL, NULL, NULL, guidriver_apphandle());
    if (res) {
      const char *ext;
      if ((ext = strrchr(path, '.')) == NULL || strchr(ext, DIRSEP_CHAR) != NULL)
        strlcat(path, ".txt", sizearray(path)); /* default extension .txt */
      save_data(path, state);
    }
  }

  if (nk_button_label(ctx, "Help") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F1))
    state->help_popup = true;
}

static void handle_stateaction(APPSTATE *state)
{
  if (state->reconnect) {
    if (rs232_isopen(state->hCom)) {
      rs232_close(state->hCom);
      state->hCom = NULL;
    }
    if (state->curport >= 0 && state->curport < state->numports) {
      const char* port = state->portlist[state->curport];
      state->hCom = rs232_open(port, state->baudrate, state->databits,
                               state->stopbits, state->parity, state->flowctrl);
      if (state->hCom != NULL)
        rs232_framecheck(state->hCom, 1);
    }
    state->reconnect = false;
  }
}

int main(int argc, char *argv[])
{
  /* global defaults */
  APPSTATE appstate;
  memset(&appstate, 0, sizeof appstate);
  appstate.reconnect = true;
  appstate.baudrate = 9600;
  appstate.view = VIEW_TEXT;
  appstate.scrolltolast = nk_true;
  appstate.console_activate = 1;
  appstate.script_reload = true;

# if defined FORTIFY
    Fortify_SetOutputFunc(Fortify_OutputFunc);
# endif

  collect_portlist(&appstate);
  char txtConfigFile[_MAX_PATH];
  get_configfile(txtConfigFile, sizearray(txtConfigFile), "bmserial.ini");
  enum nk_collapse_states tab_states[TAB_COUNT];
  SPLITTERBAR splitter_hor;
  load_settings(txtConfigFile, &appstate, tab_states, &splitter_hor);
  /* other configuration */
  opt_fontsize = ini_getf("Settings", "fontsize", FONT_HEIGHT, txtConfigFile);
  char opt_fontstd[64] = "", opt_fontmono[64] = "";
  ini_gets("Settings", "fontstd", "", opt_fontstd, sizearray(opt_fontstd), txtConfigFile);
  ini_gets("Settings", "fontmono", "", opt_fontmono, sizearray(opt_fontmono), txtConfigFile);
  char valstr[128];
  int canvas_width, canvas_height;
  ini_gets("Settings", "size", "", valstr, sizearray(valstr), txtConfigFile);
  if (sscanf(valstr, "%d %d", &canvas_width, &canvas_height) != 2 || canvas_width < 100 || canvas_height < 50) {
    canvas_width = WINDOW_WIDTH;
    canvas_height = WINDOW_HEIGHT;
  }
  appstate.filter_edit.colour = filter_defcolour(&appstate.filter_root);

# define SEPARATOR_HOR 4
# define SPACING       4
  nk_splitter_init(&splitter_hor, canvas_width - 3 * SPACING, SEPARATOR_HOR, splitter_hor.ratio);

  for (int idx = 1; idx < argc; idx++) {
    const char *ptr;
    float h;
    if (IS_OPTION(argv[idx])) {
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
    }
  }

  tcl_init(&appstate.tcl);
  tcl_register(&appstate.tcl, "exec", tcl_cmd_exec, 2, 2, &appstate);
  tcl_register(&appstate.tcl, "puts", tcl_cmd_puts, 2, 2, &appstate);
  tcl_register(&appstate.tcl, "serial", tcl_cmd_serial, 2, 3, &appstate);
  tcl_register(&appstate.tcl, "wait", tcl_cmd_wait, 2, 2, &appstate);

  struct nk_context *ctx = guidriver_init("BlackMagic Serial Monitor", canvas_width, canvas_height,
                                          GUIDRV_RESIZEABLE | GUIDRV_TIMER,
                                          opt_fontstd, opt_fontmono, opt_fontsize);
  nuklear_style(ctx);

  int mainview_width = 0; /* updated in the loop */
  int waitidle = 1;
  for ( ;; ) {
    /* handle state */
    handle_stateaction(&appstate);

    /* Input */
    nk_input_begin(ctx);
    if (!guidriver_poll(waitidle))
      break;
    nk_input_end(ctx);

    /* other events */
    int dev_event = guidriver_monitor_usb(0x1d50, 0x6018);
    if (dev_event != 0)
      collect_portlist(&appstate);
    if (nk_hsplitter_colwidth(&splitter_hor, 0) != mainview_width && !splitter_hor.hover) {
      /* reformat the main view when its width changes and word-wrap is in effect */
      mainview_width = nk_hsplitter_colwidth(&splitter_hor, 0);
      if (appstate.view == VIEW_TEXT && appstate.wordwrap)
        appstate.reformat_view = true;
    }

    /* GUI */
    guidriver_appsize(&canvas_width, &canvas_height);
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, canvas_width, canvas_height), NK_WINDOW_NO_SCROLLBAR)) {
      nk_splitter_resize(&splitter_hor, canvas_width - 3 * SPACING, RESIZE_TOPLEFT);
      nk_hsplitter_layout(ctx, &splitter_hor, canvas_height - 2 * SPACING);
      ctx->style.window.padding.x = 2;
      ctx->style.window.padding.y = 2;
      ctx->style.window.group_padding.x = 0;
      ctx->style.window.group_padding.y = 0;

      /* left column */
      if (nk_group_begin(ctx, "left", NK_WINDOW_NO_SCROLLBAR)) {
        /* buttons */
        button_bar(ctx, &appstate);

        /* monitor contents + input field */
        size_t received = process_data(&appstate);
        waitidle = (received == 0);
        nk_layout_row_dynamic(ctx, canvas_height - 2 * ROW_HEIGHT - 4 * SPACING, 1);
        widget_monitor(ctx, "monitor", &appstate, opt_fontsize, NK_WINDOW_BORDER);
        widget_lineinput(ctx, &appstate);

        nk_group_end(ctx);
      }

      /* column splitter */
      nk_hsplitter(ctx, &splitter_hor);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        panel_portconfig(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        panel_linestatus(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        panel_displayoptions(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        panel_transmitoptions(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        panel_filters(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        panel_script(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        nk_group_end(ctx);
      }

      /* popup dialogs */
      help_popup(ctx, &appstate, canvas_width, canvas_height);

      /* keyboard input (hotkeys) */
      if (nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
        appstate.console_edit[0] = '\0';
        appstate.console_activate = 2;
      }

      /* mouse cursor shape */
      if (nk_is_popup_open(ctx))
        pointer_setstyle(CURSOR_NORMAL);
      else if (splitter_hor.hover)
        pointer_setstyle(CURSOR_LEFTRIGHT);
#if defined __linux__
      else
        pointer_setstyle(CURSOR_NORMAL);
#endif
    }

    nk_end(ctx);

    /* Draw */
    guidriver_render(COLOUR_BG0_S);
  }

  save_settings(txtConfigFile, &appstate, tab_states, &splitter_hor);
  sprintf(valstr, "%d %d", canvas_width, canvas_height);
  ini_puts("Settings", "size", valstr, txtConfigFile);

  tcl_destroy(&appstate.tcl);
  if (appstate.script != NULL)
    free((void*)appstate.script);
  if (appstate.script_recv != NULL)
    free((void*)appstate.script_recv);
  filter_clear(&appstate.filter_root);
  datalist_clear();
  free_portlist(&appstate);
  guidriver_close();
  return EXIT_SUCCESS;
}

