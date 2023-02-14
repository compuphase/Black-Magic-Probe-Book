/*
 * Trace viewer utility for visualizing output on the TRACESWO pin via the
 * Black Magic Probe. This utility is built with Nuklear for a cross-platform
 * GUI.
 *
 * Copyright 2019-2023 CompuPhase
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guidriver.h"
#include "bmcommon.h"
#include "bmp-script.h"
#include "bmp-scan.h"
#include "bmp-support.h"
#include "demangle.h"
#include "dwarf.h"
#include "elf.h"
#include "gdb-rsp.h"
#include "mcu-info.h"
#include "minIni.h"
#include "noc_file_dialog.h"
#include "nuklear_guide.h"
#include "nuklear_mousepointer.h"
#include "nuklear_splitter.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"
#include "rs232.h"
#include "specialfolder.h"
#include "tcpip.h"
#include "svnrev.h"

#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __linux__ || defined __unix__
# include "res/icon_trace_64.h"
#endif

#if defined _MSC_VER
# define stricmp(a,b)    _stricmp((a),(b))
# define strdup(s)       _strdup(s)
#endif

#if !defined _MAX_PATH
# define _MAX_PATH 260
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
# define stricmp(s1,s2)    strcasecmp((s1),(s2))
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


static DWARF_LINELOOKUP dwarf_linetable = { NULL };
static DWARF_SYMBOLLIST dwarf_symboltable = { NULL };
static DWARF_PATHLIST dwarf_filetable = { NULL };

#define WINDOW_WIDTH    700     /* default window size (window is resizable) */
#define WINDOW_HEIGHT   400
#define FONT_HEIGHT     14      /* default font size */
#define ROW_HEIGHT      (1.6 * opt_fontsize)
#define COMBOROW_CY     (0.9 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.5 * opt_fontsize)
static float opt_fontsize = FONT_HEIGHT;


int ctf_error_notify(int code, int linenr, const char *message)
{
  char msg[200];

  (void)code;
  assert(message != NULL);
  if (linenr > 0)
    sprintf(msg, "TSDL file error, line %d: ", linenr);
  else
    strcpy(msg, "TSDL file error: ");
  strlcat(msg, message, sizearray(msg));
  tracelog_statusmsg(TRACESTATMSG_CTF, msg, 0);
  return 0;
}

static int bmp_callback(int code, const char *message)
{
  tracelog_statusmsg(TRACESTATMSG_BMP, message, code);
  return code >= 0;
}


#define FILTER_MAXSTRING  128

#define ERROR_NO_TSDL 0x0001
#define ERROR_NO_ELF  0x0002

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
    printf("BMTrace - SWO Trace Viewer for the Black Magic Probe.\n\n");
  printf("Usage: bmtrace [options]\n\n"
         "Options:\n"
         "-f=value  Font size to use (value must be 8 or larger).\n"
         "-h        This help.\n"
         "-t=path   Path to the TSDL metadata file to use.\n"
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

  printf("BMTrace version %s.\n", SVNREV_STR);
  printf("Copyright 2019-2023 CompuPhase\nLicensed under the Apache License version 2.0\n");
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

typedef struct tagAPPSTATE {
  int probe;                    /**< selected debug probe (index) */
  int netprobe;                 /**< index for the IP address (pseudo-probe) */
  const char **probelist;       /**< list of detected probes */
  char mcu_family[64];          /**< detected MCU family (on attach), also the "driver" name of BMP */
  char mcu_architecture[32];    /**< detected ARM architecture (on attach) */
  unsigned long mcu_partid;     /**< specific ID code (0 if unknown) */
  const char *monitor_cmds;     /**< list of "monitor" commands (target & probe dependent) */
  int reinitialize;             /**< whether to re-initialize the traceswo interface */
  int trace_status;             /**< status of traceswo */
  bool trace_running;           /**< whether tracing is running or paused */
  int error_flags;              /**< errors in initialization or decoding */
  char IPaddr[64];              /**< IP address for network probe */
  unsigned char trace_endpoint; /**< standard USB endpoint for tracing */
  int probe_type;               /**< BMP or ctxLink (needed to select manchester/async mode) */
  int swomode;                  /**< manchester or async */
  int init_target;              /**< whether to configure the target MCU for tracing */
  int init_bmp;                 /**< whether to configure the debug probe for tracing */
  int connect_srst;             /**< whether to force reset while attaching */
  unsigned long trace_count;    /**< accumulated number of captured trace messages */
  char cpuclock_str[16];        /**< edit buffer for CPU clock frequency */
  unsigned long mcuclock;       /**< active CPU clock frequency */
  char bitrate_str[16];         /**< edit buffer for bitrate */
  unsigned long bitrate;        /**< active bitrate */
  int datasize;                 /**< packet size */
  int overflow;                 /**< number of overflow events detected */
  int line_limit;               /**< max. number of lines in the viewport while running */
  bool reload_format;           /**< whether to reload the TSDL file */
  bool clear_channels;          /**< whether to reset all channels to default */
  char TSDLfile[_MAX_PATH];     /**< CTF decoding, message file */
  char ELFfile[_MAX_PATH];      /**< ELF file for symbol/address look-up */
  TRACEFILTER *filterlist;      /**< filter expressions */
  int filtercount;              /**< count of valid entries in filterlist */
  int filterlistsize;           /**< count of allocated entries in filterlist */
  char newfiltertext[FILTER_MAXSTRING]; /**< text field for filters */
  unsigned long channelmask;    /**< bit mask of enabled channels */
  int cur_chan_edit;            /**< channel info currently being edited (-1 if none) */
  char chan_str[64];            /**< edit string for channel currently being edited */
  int cur_match_line;           /**< current line matched in "find" function */
  int find_popup;               /**< whether "find" popup is active (plus match state) */
  char findtext[128];           /**< search text (keywords) */
  bool help_popup;              /**< whether "help" popup is active */
} APPSTATE;

enum {
  TAB_CONFIGURATION,
  TAB_STATUS,
  TAB_FILTERS,
  TAB_CHANNELS,
  /* --- */
  TAB_COUNT
};

enum {
  MODE_MANCHESTER = 1,
  MODE_ASYNC
};

static bool save_settings(const char *filename, const APPSTATE *state,
                          const enum nk_collapse_states tab_states[],
                          const SPLITTERBAR *splitter_hor, const SPLITTERBAR *splitter_ver)
{
  assert(filename != NULL);
  assert(state != NULL);
  assert(tab_states != NULL);
  assert(splitter_hor != NULL && splitter_ver != NULL);

  if (strlen(filename) == 0)
    return false;

  char valstr[128];
  for (int chan = 0; chan < NUM_CHANNELS; chan++) {
    char key[40];
    struct nk_color color = channel_getcolor(chan);
    sprintf(key, "chan%d", chan);
    sprintf(valstr, "%d #%06x %s", channel_getenabled(chan),
            ((int)color.r << 16) | ((int)color.g << 8) | color.b,
            channel_getname(chan, NULL, 0));
    ini_puts("Channels", key, valstr, filename);
  }
  ini_putl("Filters", "count", state->filtercount, filename);
  for (int idx = 0; idx < state->filtercount; idx++) {
    char key[40], expr[FILTER_MAXSTRING+10];
    assert(state->filterlist != NULL && state->filterlist[idx].expr != NULL);
    sprintf(key, "filter%d", idx + 1);
    sprintf(expr, "%d,%s", state->filterlist[idx].enabled, state->filterlist[idx].expr);
    ini_puts("Filters", key, expr, filename);
    free(state->filterlist[idx].expr);
  }
  if (state->filterlist != NULL)
    free(state->filterlist);
  sprintf(valstr, "%.2f %.2f", splitter_hor->ratio, splitter_ver->ratio);
  ini_puts("Settings", "splitter", valstr, filename);
  for (int idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    sprintf(key, "view%d", idx);
    sprintf(valstr, "%d", tab_states[idx]);
    ini_puts("Settings", key, valstr, filename);
  }
  ini_putl("Settings", "mode", state->swomode, filename);
  ini_putl("Settings", "init-target", state->init_target, filename);
  ini_putl("Settings", "init-bmp", state->init_bmp, filename);
  ini_putl("Settings", "connect-srst", state->connect_srst, filename);
  ini_putl("Settings", "datasize", state->datasize, filename);
  ini_puts("Settings", "tsdl", state->TSDLfile, filename);
  ini_puts("Settings", "elf", state->ELFfile, filename);
  ini_putl("Settings", "mcu-freq", state->mcuclock, filename);
  ini_putl("Settings", "bitrate", state->bitrate, filename);

  double spacing;
  unsigned long scale, delta;
  timeline_getconfig(&spacing, &scale, &delta);
  sprintf(valstr, "%.2f %lu %lu", spacing, scale, delta);
  ini_puts("Settings", "timeline", valstr, filename);

  if (bmp_is_ip_address(state->IPaddr))
    ini_puts("Settings", "ip-address", state->IPaddr, filename);
  ini_putl("Settings", "probe", (state->probe == state->netprobe) ? 99 : state->probe, filename);

  return access(filename, 0) == 0;
}

static bool load_settings(const char *filename, APPSTATE *state,
                          enum nk_collapse_states tab_states[],
                          SPLITTERBAR *splitter_hor, SPLITTERBAR *splitter_ver)
{
  assert(filename != NULL);
  assert(state != NULL);
  assert(tab_states != NULL);
  assert(splitter_hor != NULL && splitter_ver != NULL);

  /* read channel configuration */
  char valstr[128];
  for (int chan = 0; chan < NUM_CHANNELS; chan++) {
    char key[41];
    unsigned clr;
    int enabled, result;
    channel_set(chan, (chan == 0), NULL, SWO_TRACE_DEFAULT_COLOR); /* preset: port 0 is enabled by default, others disabled by default */
    sprintf(key, "chan%d", chan);
    ini_gets("Channels", key, "", valstr, sizearray(valstr), filename);
    result = sscanf(valstr, "%d #%x %40s", &enabled, &clr, key);
    if (result >= 2)
      channel_set(chan, enabled, (result >= 3) ? key : NULL, nk_rgb(clr >> 16,(clr >> 8) & 0xff, clr & 0xff));
  }
  /* read filters (initialize the filter list) */
  state->filtercount = ini_getl("Filters", "count", 0, filename);;
  state->filterlistsize = state->filtercount + 1; /* at least 1 extra, for a NULL sentinel */
  state->filterlist = malloc(state->filterlistsize * sizeof(TRACEFILTER));  /* make sure unused entries are NULL */
  if (state->filterlist != NULL) {
    memset(state->filterlist, 0, state->filterlistsize * sizeof(TRACEFILTER));
    int idx;
    for (idx = 0; idx < state->filtercount; idx++) {
      char key[40], *ptr;
      state->filterlist[idx].expr = malloc(sizearray(state->newfiltertext) * sizeof(char));
      if (state->filterlist[idx].expr == NULL)
        break;
      sprintf(key, "filter%d", idx + 1);
      ini_gets("Filters", key, "", state->newfiltertext, sizearray(state->newfiltertext), filename);
      state->filterlist[idx].enabled = (int)strtol(state->newfiltertext, &ptr, 10);
      assert(ptr != NULL && *ptr != '\0');  /* a comma should be found */
      if (*ptr == ',')
        ptr += 1;
      strcpy(state->filterlist[idx].expr, ptr);
    }
    state->filtercount = idx;
  } else {
    state->filtercount = state->filterlistsize = 0;
  }
  state->newfiltertext[0] = '\0';

  /* other configuration */
  state->probe = (int)ini_getl("Settings", "probe", 0, filename);
  ini_gets("Settings", "ip-address", "127.0.0.1", state->IPaddr, sizearray(state->IPaddr), filename);
  state->swomode = (int)ini_getl("Settings", "mode", MODE_MANCHESTER, filename);
  state->init_target = (int)ini_getl("Settings", "init-target", 1, filename);
  state->init_bmp = (int)ini_getl("Settings", "init-bmp", 1, filename);
  if (state->swomode == 0) {  /* legacy: state->mode == 0 was MODE_PASSIVE */
    state->swomode = MODE_MANCHESTER;
    state->init_target = 0;
    state->init_bmp = 0;
  }
  state->connect_srst = (int)ini_getl("Settings", "connect-srst", 0, filename);
  state->datasize = (int)ini_getl("Settings", "datasize", 1, filename);
  ini_gets("Settings", "tsdl", "", state->TSDLfile, sizearray(state->TSDLfile), filename);
  ini_gets("Settings", "elf", "", state->ELFfile, sizearray(state->ELFfile), filename);
  ini_gets("Settings", "mcu-freq", "48000000", state->cpuclock_str, sizearray(state->cpuclock_str), filename);
  ini_gets("Settings", "bitrate", "100000", state->bitrate_str, sizearray(state->bitrate_str), filename);
  ini_gets("Settings", "timeline", "", valstr, sizearray(valstr), filename);
  if (strlen(valstr) > 0) {
    double spacing;
    unsigned long scale, delta;
    if (sscanf(valstr, "%lf %lu %lu", &spacing, &scale, &delta) == 3)
      timeline_setconfig(spacing, scale, delta);
  }

  ini_gets("Settings", "splitter", "", valstr, sizearray(valstr), filename);
  splitter_hor->ratio = splitter_ver->ratio = 0.0;
  sscanf(valstr, "%f %f", &splitter_hor->ratio, &splitter_ver->ratio);
  if (splitter_hor->ratio < 0.05 || splitter_hor->ratio > 0.95)
    splitter_hor->ratio = 0.70;
  if (splitter_ver->ratio < 0.05 || splitter_ver->ratio > 0.95)
    splitter_ver->ratio = 0.70;

  for (int idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    int opened, result;
    tab_states[idx] = (idx == TAB_CONFIGURATION || idx == TAB_STATUS) ? NK_MAXIMIZED : NK_MINIMIZED;
    sprintf(key, "view%d", idx);
    ini_gets("Settings", key, "", valstr, sizearray(valstr), filename);
    result = sscanf(valstr, "%d", &opened);
    if (result >= 1)
      tab_states[idx] = opened;
  }

  return true;
}

static void probe_set_options(APPSTATE *state)
{
  if (bmp_isopen()) {
    char cmd[100];
    if (bmp_expand_monitor_cmd(cmd, sizearray(cmd), "connect", state->monitor_cmds)) {
      strlcat(cmd, " ", sizearray(cmd));
      strlcat(cmd, state->connect_srst ? "enable" : "disable", sizearray(cmd));
      if (!bmp_monitor(cmd))
        bmp_callback(BMPERR_MONITORCMD, "Setting connect-with-reset option failed");
    }
  }
}

static void find_popup(struct nk_context *ctx, APPSTATE *state, float canvas_width, float canvas_height)
{
  if (state->find_popup > 0) {
    struct nk_rect rc;
    rc.x = canvas_width * 0.425;
    rc.y = 1.4 * ROW_HEIGHT;
    rc.w = 200;
    rc.h = 3.6 * ROW_HEIGHT;
    if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Search", NK_WINDOW_NO_SCROLLBAR, rc)) {
      nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.2, 0.8));
      nk_label(ctx, "Text", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_edit_focus(ctx, 0);
      nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
                                     state->findtext, sizearray(state->findtext),
                                     nk_filter_ascii);
      nk_layout_row(ctx, NK_DYNAMIC, opt_fontsize, 2, nk_ratio(2, 0.2, 0.8));
      nk_spacing(ctx, 1);
      if (state->find_popup == 2)
        nk_label_colored(ctx, "Text not found", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, COLOUR_FG_RED);
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 3);
      nk_spacing(ctx, 1);
      if (nk_button_label(ctx, "Find") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER)) {
        if (strlen(state->findtext) > 0) {
          int line = tracestring_find(state->findtext, state->cur_match_line);
          if (line != state->cur_match_line) {
            state->cur_match_line = line;
            state->find_popup = 0;
            state->trace_running = false;
          } else {
            state->cur_match_line = -1;
            state->find_popup = 2; /* to mark "string not found" */
          }
          nk_popup_close(ctx);
        } /* if (len > 0) */
      }
      if (nk_button_label(ctx, "Cancel") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
        state->find_popup = 0;
        nk_popup_close(ctx);
      }
      nk_popup_end(ctx);
    } else {
      state->find_popup = 0;
    }
  }
}

static void help_popup(struct nk_context *ctx, APPSTATE *state, float canvas_width, float canvas_height)
{
# include "bmtrace_help.h"

  if (state->help_popup) {
#   define MARGIN  10
    float w = opt_fontsize * 40;
    if (w > canvas_width - 2*MARGIN)  /* clip "ideal" help window size of canvas size */
      w = canvas_width - 2*MARGIN;
    float h = canvas_height * 0.75;
    struct nk_rect rc = nk_rect((canvas_width - w) / 2, (canvas_height - h) / 2, w, h);
#   undef MARGIN

    state->help_popup = nk_guide(ctx, &rc, opt_fontsize, (const char*)bmtrace_help, NULL);
  }
}

static void panel_options(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT],
                          float panel_width)
{
  static const char *datasize_strings[] = { "auto", "8 bit", "16 bit", "32 bit" };
  static const char *mode_strings[] = { "Manchester", "NRZ/async." };

# define LABEL_WIDTH (4.5 * opt_fontsize)
# define VALUE_WIDTH (panel_width - LABEL_WIDTH - 26)

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", &tab_states[TAB_CONFIGURATION])) {
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Probe", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    struct nk_rect bounds = nk_widget_bounds(ctx);
    state->probe = nk_combo(ctx, state->probelist, state->netprobe+1, state->probe,
                            (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
    if (state->probe == state->netprobe) {
      int reconnect = 0;
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "IP Addr", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                    state->IPaddr, sizearray(state->IPaddr), nk_filter_ascii,
                                    "IP address of the ctxLink");
      if ((result & NK_EDIT_COMMITED) != 0 && bmp_is_ip_address(state->IPaddr))
        reconnect = 1;
      nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
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
          reconnect = 1;
        } else {
          strlcpy(state->IPaddr, "none found", sizearray(state->IPaddr));
        }
      }
      if (reconnect) {
        bmp_disconnect();
        state->reinitialize = nk_true;
      }
    }
    if (state->probe_type == PROBE_UNKNOWN) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "Mode", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      int result = state->swomode - MODE_MANCHESTER;
      result = nk_combo(ctx, mode_strings, NK_LEN(mode_strings), result, opt_fontsize, nk_vec2(VALUE_WIDTH,4.5*opt_fontsize));
      if (state->swomode != result + MODE_MANCHESTER) {
        /* mode is 1-based, the result of nk_combo() is 0-based, which is
           why MODE_MANCHESTER is added (MODE_MANCHESTER == 1) */
        state->swomode = result + MODE_MANCHESTER;
        state->reinitialize = nk_true;
      }
      nk_layout_row_end(ctx);
    }
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Configure Target", &state->init_target, NK_TEXT_LEFT, "Configure the target microcontroller for SWO"))
      state->reinitialize = nk_true;
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Configure Debug Probe", &state->init_bmp, NK_TEXT_LEFT, "Activate SWO trace capture in the Black Magic Probe"))
      state->reinitialize = nk_true;
    if (state->init_target || state->init_bmp) {
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
      if (checkbox_tooltip(ctx, "Reset target during connect", &state->connect_srst, NK_TEXT_LEFT, "Keep the target in reset state while scanning and attaching"))
        state->reinitialize = nk_true;
    }
    if (state->init_target) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "CPU clock", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                    state->cpuclock_str, sizearray(state->cpuclock_str), nk_filter_decimal,
                                    "CPU clock of the target microcontroller");
      if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(state->cpuclock_str, NULL, 10) != state->mcuclock))
        state->reinitialize = nk_true;
      nk_layout_row_end(ctx);
    }
    if (state->init_target || (state->init_bmp && state->swomode == MODE_ASYNC)) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "Bit rate", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                    state->bitrate_str, sizearray(state->bitrate_str), nk_filter_decimal,
                                    "SWO bit rate (data rate)");
      if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(state->bitrate_str, NULL, 10) != state->bitrate))
        state->reinitialize = nk_true;
      nk_layout_row_end(ctx);
    }
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Data size", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH);
    bounds = nk_widget_bounds(ctx);
    int result = state->datasize;
    state->datasize = nk_combo(ctx, datasize_strings, NK_LEN(datasize_strings), state->datasize, opt_fontsize, nk_vec2(VALUE_WIDTH,5.5*opt_fontsize));
    if (state->datasize != result) {
      trace_setdatasize((state->datasize == 3) ? 4 : (short)state->datasize);
      tracestring_clear();
      trace_overflowerrors(true);
      ctf_decode_reset();
      state->trace_count = 0;
      state->overflow = 0;
      if (state->trace_status == TRACESTAT_OK)
        tracelog_statusmsg(TRACESTATMSG_BMP, "Listening ...", BMPSTAT_SUCCESS);
    }
    tooltip(ctx, bounds, "Payload size of an SWO packet (in bits); auto for autodetect");
    nk_layout_row_end(ctx);
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "TSDL file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
    bool error = editctrl_cond_color(ctx, (state->error_flags & ERROR_NO_TSDL), COLOUR_BG_DARKRED);
    result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                              state->TSDLfile, sizearray(state->TSDLfile), nk_filter_ascii,
                              "Metadata file for Common Trace Format (CTF)");
    if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED)) {
      state->clear_channels = true;
      state->reload_format = true;
    }
    editctrl_reset_color(ctx, error);
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
      nk_input_clear_mousebuttons(ctx);
#     if defined _WIN32
        const char *filter = "TSDL files\0*.tsdl;*.ctf\0All files\0*.*\0";
#     else
        const char *filter = "TSDL files\0*.tsdl\0All files\0*\0";
#     endif
      int res = noc_file_dialog_open(state->TSDLfile, sizearray(state->TSDLfile),
                                     NOC_FILE_DIALOG_OPEN, filter,
                                     NULL, state->TSDLfile, "Select metadata file for CTF",
                                     guidriver_apphandle());
      if (res) {
        state->clear_channels = true;
        state->reload_format = true;
      }
    }
    nk_layout_row_end(ctx);
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "ELF file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
    error = editctrl_cond_color(ctx, (state->error_flags & ERROR_NO_ELF), COLOUR_BG_DARKRED);
    result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                              state->ELFfile, sizearray(state->ELFfile), nk_filter_ascii,
                              "ELF file for symbol lookup");
    if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
      state->reload_format = true;
    editctrl_reset_color(ctx, error);
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
      nk_input_clear_mousebuttons(ctx);
#     if defined _WIN32
        const char *filter = "ELF Executables\0*.elf;*.\0All files\0*.*\0";
#     else
        const char *filter = "ELF Executables\0*.elf\0All files\0*\0";
#     endif
      int res = noc_file_dialog_open(state->ELFfile, sizearray(state->ELFfile),
                                     NOC_FILE_DIALOG_OPEN, filter,
                                     NULL, state->ELFfile, "Select ELF Executable",
                                     guidriver_apphandle());
      if (res)
        state->reload_format = true;
    }
    nk_layout_row_end(ctx);

    nk_tree_state_pop(ctx);
  }
# undef LABEL_WIDTH
# undef VALUE_WIDTH
}

static void panel_status(struct nk_context *ctx, APPSTATE *state,
                         enum nk_collapse_states tab_states[TAB_COUNT],
                         float panel_width)
{
# define LABEL_WIDTH(n) ((n) * opt_fontsize)
# define VALUE_WIDTH(n) (panel_width - LABEL_WIDTH(n) - 26)
# define LINE_HEIGHT (1.2 * opt_fontsize)

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Status", &tab_states[TAB_STATUS])) {
    char valuestr[20];
    nk_layout_row_begin(ctx, NK_STATIC, LINE_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH(8));
    nk_label(ctx, "Total received", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH(8));
    sprintf(valuestr, "%lu", state->trace_count);
    label_tooltip(ctx, valuestr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, "Total number of messages received.");
    nk_layout_row_end(ctx);

    int overflow = trace_overflowerrors(false);
    if (overflow > state->overflow && state->line_limit > 50) {
      state->overflow = overflow;
      state->line_limit /= 2;
    }
    nk_layout_row_begin(ctx, NK_STATIC, LINE_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH(8));
    nk_label(ctx, "Overflow events", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH(8));
    sprintf(valuestr, "%d", overflow);
    label_tooltip(ctx, valuestr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, "Overflow event count.\nLimit the number of displayed traces to avoid overflows.");
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_STATIC, LINE_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH(8));
    nk_label(ctx, "Packet errors", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH(8));
    sprintf(valuestr, "%d", trace_getpacketerrors(false));
    label_tooltip(ctx, valuestr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, "SWO packet errors.\nVerify 'Data size' setting.");
    nk_layout_row_end(ctx);

    nk_tree_state_pop(ctx);
  }
# undef LABEL_WIDTH
# undef VALUE_WIDTH
}

static void filter_options(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT])
{
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Filters", &tab_states[TAB_FILTERS])) {
    char filter[FILTER_MAXSTRING];
    assert(state->filterlistsize == 0 || state->filterlist != NULL);
    assert(state->filterlistsize == 0 || state->filtercount < state->filterlistsize);
    assert(state->filterlistsize == 0 || (state->filterlist[state->filtercount].expr == NULL && !state->filterlist[state->filtercount].enabled));
    struct nk_rect bounds = nk_widget_bounds(ctx);
    int txtwidth = bounds.w - 2 * BROWSEBTN_WIDTH - (2 * 5);
    for (int idx = 0; idx < state->filtercount; idx++) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
      nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
      checkbox_tooltip(ctx, "", &state->filterlist[idx].enabled, NK_TEXT_LEFT, "Enable/disable this filter");
      nk_layout_row_push(ctx, txtwidth);
      assert(state->filterlist[idx].expr != NULL);
      strcpy(filter, state->filterlist[idx].expr);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                    filter, sizearray(filter), nk_filter_ascii,
                                    "Text to filter on (case-sensitive)");
      if (strcmp(filter, state->filterlist[idx].expr) != 0) {
        strcpy(state->filterlist[idx].expr, filter);
        state->filterlist[idx].enabled = (strlen(state->filterlist[idx].expr) > 0);
      }
      nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
      if (button_symbol_tooltip(ctx, NK_SYMBOL_X, NK_KEY_NONE, nk_true, "Remove this filter")
          || ((result & NK_EDIT_COMMITED) && strlen(filter) == 0))
      {
        /* remove row */
        assert(state->filterlist[idx].expr != NULL);
        free(state->filterlist[idx].expr);
        state->filtercount -= 1;
        if (idx < state->filtercount)
          memmove(&state->filterlist[idx], &state->filterlist[idx+1], (state->filtercount - idx) * sizeof(TRACEFILTER));
        state->filterlist[state->filtercount].expr = NULL;
        state->filterlist[state->filtercount].enabled = 0;
      }
    }
    txtwidth = bounds.w - 1 * BROWSEBTN_WIDTH - (1 * 5);
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, txtwidth);
    int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                  state->newfiltertext, sizearray(state->newfiltertext),
                                  nk_filter_ascii, "New filter (case-sensitive)");
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if ((button_symbol_tooltip(ctx, NK_SYMBOL_PLUS, NK_KEY_NONE, nk_true, "Add filter")
         || (result & NK_EDIT_COMMITED))
        && strlen(state->newfiltertext) > 0)
    {
      /* add row */
      if (state->filterlistsize > 0) {
        /* make sure there is an extra entry at the top of the array, for
           a NULL terminator */
        assert(state->filtercount < state->filterlistsize);
        if (state->filtercount + 1 == state->filterlistsize) {
          int newsize = 2 * state->filterlistsize;
          TRACEFILTER *newlist = malloc(newsize * sizeof(TRACEFILTER));
          if (newlist != NULL) {
            assert(state->filterlist != NULL);
            memset(newlist, 0, newsize * sizeof(TRACEFILTER));  /* set all new entries to NULL */
            memcpy(newlist, state->filterlist, state->filterlistsize * sizeof(TRACEFILTER));
            free(state->filterlist);
            state->filterlist = newlist;
            state->filterlistsize = newsize;
          }
        }
      }
      if (state->filtercount + 1 < state->filterlistsize) {
        state->filterlist[state->filtercount].expr = malloc(sizearray(state->newfiltertext) * sizeof(char));
        if (state->filterlist[state->filtercount].expr != NULL) {
          strcpy(state->filterlist[state->filtercount].expr, state->newfiltertext);
          state->filterlist[state->filtercount].enabled = 1;
          state->filtercount += 1;
          state->newfiltertext[0] = '\0';
        }
      }
    }
    nk_tree_state_pop(ctx);
  }
}

static void channel_options(struct nk_context *ctx, APPSTATE *state,
                            enum nk_collapse_states tab_states[TAB_COUNT])
{
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Channels", &tab_states[TAB_CHANNELS])) {
    float labelwidth = tracelog_labelwidth(opt_fontsize) + 10;
    struct nk_style_button stbtn = ctx->style.button;
    stbtn.border = 0;
    stbtn.rounding = 0;
    stbtn.padding.x = stbtn.padding.y = 0;
    for (int chan = 0; chan < NUM_CHANNELS; chan++) {
      char label[32];
      int enabled;
      struct nk_color clrtxt, clrbk;
      nk_layout_row_begin(ctx, NK_STATIC, opt_fontsize, 2);
      nk_layout_row_push(ctx, 3 * opt_fontsize);
      sprintf(label, "%2d", chan);
      enabled = channel_getenabled(chan);
      if (checkbox_tooltip(ctx, label, &enabled, NK_TEXT_LEFT, "Enable/disable this channel")) {
        /* enable/disable channel in the target */
        pointer_setstyle(CURSOR_WAIT);
        channel_setenabled(chan, enabled);
        if (state->init_target) {
          if (enabled)
            state->channelmask |= (1 << chan);
          else
            state->channelmask &= ~(1 << chan);
          if (state->trace_status != TRACESTAT_NO_CONNECT) {
            const DWARF_SYMBOLLIST *symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_TER", -1, -1);
            unsigned long params[2];
            params[0] = state->channelmask;
            params[1] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
            bmp_runscript("swo_channels", state->mcu_family, state->mcu_architecture, params, 2);
          }
        }
        pointer_setstyle(CURSOR_NORMAL);
      }
      clrbk = channel_getcolor(chan);
      clrtxt = CONTRAST_COLOUR(clrbk);
      stbtn.normal.data.color = stbtn.hover.data.color
        = stbtn.active.data.color = stbtn.text_background = clrbk;
      stbtn.text_normal = stbtn.text_active = stbtn.text_hover = clrtxt;
      nk_layout_row_push(ctx, labelwidth);
      struct nk_rect bounds = nk_widget_bounds(ctx);
      if (nk_button_label_styled(ctx, &stbtn, channel_getname(chan, NULL, 0))) {
        /* we want a contextual pop-up (that you can simply click away
           without needing a close button), so we simulate a right-mouse
           click */
        nk_input_motion(ctx, bounds.x, bounds.y + bounds.h - 1);
        nk_input_button(ctx, NK_BUTTON_RIGHT, bounds.x, bounds.y + bounds.h - 1, 1);
        nk_input_button(ctx, NK_BUTTON_RIGHT, bounds.x, bounds.y + bounds.h - 1, 0);
      }
      tooltip(ctx, bounds, "Channel name & colour; click to change");
      nk_layout_row_end(ctx);
      if (nk_contextual_begin(ctx, 0, nk_vec2(9*opt_fontsize, 5*ROW_HEIGHT), bounds)) {
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        clrbk.r = (nk_byte)nk_propertyi(ctx, "#R", 0, clrbk.r, 255, 1, 1);
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        clrbk.g = (nk_byte)nk_propertyi(ctx, "#G", 0, clrbk.g, 255, 1, 1);
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        clrbk.b = (nk_byte)nk_propertyi(ctx, "#B", 0, clrbk.b, 255, 1, 1);
        channel_setcolor(chan, clrbk);
        /* the name in the channels array must only be changed on closing
           the popup, so it is copied to a local variable on first opening */
        if (state->cur_chan_edit == -1) {
          state->cur_chan_edit = chan;
          channel_getname(chan, state->chan_str, sizearray(state->chan_str));
        }
        nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.35, 0.65));
        nk_label(ctx, "name", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
                                       state->chan_str, sizearray(state->chan_str),
                                       nk_filter_ascii);
        nk_contextual_end(ctx);
      } else if (state->cur_chan_edit == chan) {
        /* contextual popup is closed, copy the name back */
        if (strlen(state->chan_str) == 0) {
          channel_setname(chan, NULL);
        } else {
          char *pspace;
          while ((pspace = strchr(state->chan_str, ' ')) != NULL)
            *pspace = '-'; /* can't handle spaces in the channel names */
          channel_setname(chan, state->chan_str);
        }
        state->cur_chan_edit = -1;
      }
    }
    nk_tree_state_pop(ctx);
  }
}

static void button_bar(struct nk_context *ctx, APPSTATE *state)
{
  nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 5, nk_ratio(5, 0.2, 0.2, 0.2, 0.2, 0.2));
  const char *caption;
  if (state->trace_running) {
    caption = "Stop";
  } else if (tracestring_isempty()) {
    caption = "Start";
    state->trace_count = 0;
  } else {
     caption = "Resume";
  }
  if (nk_button_label(ctx, caption) || nk_input_is_key_pressed(&ctx->input, NK_KEY_F5)) {
    state->trace_running = !state->trace_running;
    trace_overflowerrors(true);
    state->overflow = 0;
    if (state->trace_running && state->trace_status != TRACESTAT_OK) {
      state->trace_status = trace_init(state->trace_endpoint, (state->probe == state->netprobe) ? state->IPaddr : NULL);
      if (state->trace_status != TRACESTAT_OK)
        state->trace_running = false;
    }
  }
  if (nk_button_label(ctx, "Clear")) {
    tracestring_clear();
    trace_overflowerrors(true);
    ctf_decode_reset();
    state->trace_count = 0;
    state->overflow = 0;
    state->cur_match_line = -1;
  }
  if (nk_button_label(ctx, "Search") || nk_input_is_key_pressed(&ctx->input, NK_KEY_FIND))
    state->find_popup = 1;
  if (nk_button_label(ctx, "Save") || nk_input_is_key_pressed(&ctx->input, NK_KEY_SAVE)) {
    char path[_MAX_PATH];
    int res = noc_file_dialog_open(path, sizearray(path), NOC_FILE_DIALOG_SAVE,
                                   "CSV files\0*.csv\0All files\0*.*\0",
                                   NULL, NULL, NULL, guidriver_apphandle());
    if (res) {
      const char *ext;
      if ((ext = strrchr(path, '.')) == NULL || strchr(ext, DIRSEP_CHAR) != NULL)
        strlcat(path, ".csv", sizearray(path)); /* default extension .csv */
      tracestring_save(path);
    }
  }
  if (nk_button_label(ctx, "Help") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F1))
    state->help_popup = true;
}

static void handle_stateaction(APPSTATE *state)
{
  if (state->reinitialize == 1) {
    int result;
    char msg[100];
    tracelog_statusclear();
    tracestring_clear();
    trace_overflowerrors(true);
    ctf_decode_reset();
    state->trace_count = 0;
    state->overflow = 0;
    state->line_limit = 400;
    if ((state->mcuclock = strtol(state->cpuclock_str, NULL, 10)) == 0)
      state->mcuclock = 48000000;
    if (state->swomode == MODE_MANCHESTER || (state->bitrate = strtol(state->bitrate_str, NULL, 10)) == 0)
      state->bitrate = 100000;
    if (state->init_target || state->init_bmp) {
      /* open/reset the serial port/device if any initialization must be done */
      if (bmp_comport() != NULL)
        bmp_break();
      result = bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
      if (result) { /* bmp_connect() also opens the (virtual) serial port/device */
        if (state->monitor_cmds == NULL)
          state->monitor_cmds = bmp_get_monitor_cmds();
        probe_set_options(state);
        result = bmp_attach(true, state->mcu_family, sizearray(state->mcu_family),
                            state->mcu_architecture, sizearray(state->mcu_architecture));
      } else {
        state->trace_status = TRACESTAT_NO_CONNECT;
      }
      if (result) {
        /* overrule any default protocol setting, if the debug probe can be
           verified */
        state->probe_type = bmp_checkversionstring();
        if (state->probe_type == PROBE_BMPv21 || state->probe_type == PROBE_BMPv23)
          state->swomode = MODE_MANCHESTER;
        else if (state->probe_type == PROBE_CTXLINK)
         state->swomode = MODE_ASYNC;
      }
      if (result && state->init_target) {
        unsigned long params[4];
        /* check to get more specific information on the MCU (specifically to
           update the mcu_family name, so that the appropriate scripts can be
           loaded) */
        if (bmp_runscript("partid", state->mcu_family, state->mcu_architecture, params, 1)) {
          state->mcu_partid = params[0];
          const MCUINFO *info = mcuinfo_lookup(state->mcu_family, state->mcu_partid);
          if (info != NULL && info->mcuname != NULL) {
            strlcpy(state->mcu_family, info->mcuname, sizearray(state->mcu_family));
            bmscript_clear();
          }
        }
        /* initialize the target (target-specific configuration, generic
           configuration and channels */
        bmp_runscript("swo_device", state->mcu_family, state->mcu_architecture, NULL, 0);
        const DWARF_SYMBOLLIST *symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_BPS", -1, -1);
        assert(state->swomode == MODE_MANCHESTER || state->swomode == MODE_ASYNC);
        unsigned swvclock = (state->swomode == MODE_MANCHESTER) ? 2 * state->bitrate : state->bitrate;
        assert(state->mcuclock > 0 && swvclock > 0);
        params[0] = state->swomode;
        params[1] = state->mcuclock / swvclock - 1;
        params[2] = state->bitrate;
        params[3] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
        bmp_runscript("swo_trace", state->mcu_family, state->mcu_architecture, params, 4);
        /* enable active channels in the target (disable inactive channels) */
        state->channelmask = 0;
        for (int chan = 0; chan < NUM_CHANNELS; chan++)
          if (channel_getenabled(chan))
            state->channelmask |= (1 << chan);
        symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_TER", -1, -1);
        params[0] = state->channelmask;
        params[1] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
        bmp_runscript("swo_channels", state->mcu_family, state->mcu_architecture, params, 2);
      }
    } else if (bmp_isopen()) {
      /* no initialization is requested, if the serial port is open, close it
         (so that the gdbserver inside the BMP is available for debugging) */
      bmp_disconnect();
      result = 1; /* flag status = ok, to drop into the next "if" */
    }
    if (result) {
      if (state->init_bmp)
        bmp_enabletrace((state->swomode == MODE_ASYNC) ? state->bitrate : 0, &state->trace_endpoint);
      /* trace_init() does nothing if initialization had already succeeded */
      if (state->probe == state->netprobe)
        state->trace_status = trace_init(BMP_PORT_TRACE, state->IPaddr);
      else
        state->trace_status = trace_init(state->trace_endpoint, NULL);
      bmp_restart();
    }
    state->trace_running = (state->trace_status == TRACESTAT_OK);
    switch (state->trace_status) {
    case TRACESTAT_OK:
      if (state->init_target || state->init_bmp) {
        assert(strlen(state->mcu_family) > 0);
        sprintf(msg, "Connected [%s]", state->mcu_family);
        tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPSTAT_SUCCESS);
      } else {
        tracelog_statusmsg(TRACESTATMSG_BMP, "Listening (passive mode)...", BMPSTAT_SUCCESS);
      }
      break;
    case TRACESTAT_INIT_FAILED:
    case TRACESTAT_NO_INTERFACE:
    case TRACESTAT_NO_DEVPATH:
    case TRACESTAT_NO_PIPE:
      strlcpy(msg, "Trace interface not available", sizearray(msg));
      if (state->probe == state->netprobe && state->swomode != MODE_ASYNC)
        strlcat(msg, "; try NRZ/Async mode", sizearray(msg));
      tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPERR_GENERAL);
      break;
    case TRACESTAT_NO_ACCESS:
      { int loc;
        unsigned long error = trace_errno(&loc);
        sprintf(msg, "Trace access denied (error %d:%lu)", loc, error);
        tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPERR_GENERAL);
      }
      break;
    case TRACESTAT_NO_THREAD:
      { int loc;
        unsigned long error = trace_errno(&loc);
        sprintf(msg, "Multi-threading set-up failure (error %d:%lu)", loc, error);
        tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPERR_GENERAL);
      }
      break;
    case TRACESTAT_NO_CONNECT:
      tracelog_statusmsg(TRACESTATMSG_BMP, "Failed to \"attach\" to Black Magic Probe", BMPERR_GENERAL);
      break;
    }
    state->reinitialize = nk_false;
  } else if (state->reinitialize > 0) {
    state->reinitialize -= 1;
  }

  if (state->reload_format) {
    ctf_parse_cleanup();
    ctf_decode_cleanup();
    tracestring_clear();
    trace_overflowerrors(true);
    ctf_decode_reset();
    dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
    state->cur_match_line = -1;
    state->error_flags = 0;
    state->trace_count = 0;
    state->overflow = 0;
    if (strlen(state->TSDLfile) > 0)
      state->error_flags |= ERROR_NO_TSDL;
    if (strlen(state->TSDLfile)> 0 && access(state->TSDLfile, 0) == 0) {
      if (ctf_parse_init(state->TSDLfile) && ctf_parse_run()) {
        /* optionally clear all channel names & colours */
        if (state->clear_channels)
          for (int chan = 0; chan < NUM_CHANNELS; chan++)
            channel_set(chan, false, NULL, SWO_TRACE_DEFAULT_COLOR);
        /* stream names overrule configured channel names */
        const CTF_STREAM *stream;
        for (int seqnr = 0; (stream = stream_by_seqnr(seqnr)) != NULL; seqnr++) {
          if (stream->name != NULL && strlen(stream->name) > 0) {
            int chan = stream->stream_id;
            assert(chan >= 0 && chan < NUM_CHANNELS);
            channel_set(chan, true, stream->name, SWO_TRACE_DEFAULT_COLOR);
          }
        }
        state->error_flags &= ~ERROR_NO_TSDL;
        tracelog_statusmsg(TRACESTATMSG_CTF, "CTF mode active", BMPSTAT_SUCCESS);
      } else {
        ctf_parse_cleanup();
      }
    }
    if (strlen(state->ELFfile) > 0)
      state->error_flags |= ERROR_NO_ELF;
    if (strlen(state->ELFfile) > 0 && access(state->ELFfile, 0) == 0) {
      FILE *fp = fopen(state->ELFfile, "rb");
      if (fp != NULL) {
        int address_size;
        dwarf_read(fp, &dwarf_linetable, &dwarf_symboltable, &dwarf_filetable, &address_size);
        fclose(fp);
        ctf_set_symtable(&dwarf_symboltable);
        state->error_flags &= ~ERROR_NO_ELF;
      }
    }
    state->reload_format = false;
  }
}

int main(int argc, char *argv[])
{
  /* global defaults */
  APPSTATE appstate;
  memset(&appstate, 0, sizeof appstate);
  appstate.reinitialize = nk_true;
  appstate.reload_format = true;
  appstate.trace_status = TRACESTAT_NOT_INIT;
  appstate.trace_running = true;
  appstate.swomode = MODE_MANCHESTER;
  appstate.probe_type = PROBE_UNKNOWN;
  appstate.trace_endpoint = BMP_EP_TRACE;
  appstate.init_target = nk_true;
  appstate.init_bmp = nk_true;
  appstate.connect_srst = nk_false;
  appstate.cur_chan_edit = -1;
  appstate.cur_match_line = -1;
  appstate.line_limit = 400;

# if defined FORTIFY
    Fortify_SetOutputFunc(Fortify_OutputFunc);
# endif

  /* locate the configuration file for settings */
  char txtConfigFile[_MAX_PATH];
  get_configfile(txtConfigFile, sizearray(txtConfigFile), "bmtrace.ini");
  SPLITTERBAR splitter_hor, splitter_ver;
  enum nk_collapse_states tab_states[TAB_COUNT];
  load_settings(txtConfigFile, &appstate, tab_states, &splitter_hor, &splitter_ver);
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

# define SEPARATOR_HOR 4
# define SEPARATOR_VER 4
# define SPACING       4
  nk_splitter_init(&splitter_hor, canvas_width - 3 * SPACING, SEPARATOR_HOR, splitter_hor.ratio);
  nk_splitter_init(&splitter_ver, canvas_height - (ROW_HEIGHT + 8 * SPACING), SEPARATOR_VER, splitter_ver.ratio);

  for (int idx = 1; idx < argc; idx++) {
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
      case 't':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        if (access(ptr, 0) == 0)
          strlcpy(appstate.TSDLfile, ptr, sizearray(appstate.TSDLfile));
        break;
      case 'v':
        version();
        return EXIT_SUCCESS;
      default:
        usage(argv[idx]);
        return EXIT_FAILURE;
      }
    } else if (access(argv[idx], 0) == 0) {
      /* parameter is a filename, test whether that is an ELF file */
      FILE *fp = fopen(argv[idx], "rb");
      if (fp != NULL) {
        int err = elf_info(fp, NULL, NULL, NULL, NULL);
        if (err == ELFERR_NONE) {
          strlcpy(appstate.ELFfile, argv[idx], sizearray(appstate.ELFfile));
          if (access(appstate.TSDLfile, 0) != 0) {
            /* see whether there is a TSDL file with a matching name */
            char *ext;
            strlcpy(appstate.TSDLfile, appstate.ELFfile, sizearray(appstate.TSDLfile));
            ext = strrchr(appstate.TSDLfile, '.');
            if (ext != NULL && strpbrk(ext, "\\/") == NULL)
              *ext = '\0';
            strlcat(appstate.TSDLfile, ".tsdl", sizearray(appstate.TSDLfile));
            if (access(appstate.TSDLfile, 0) != 0)
              appstate.TSDLfile[0] = '\0';  /* presumed TSDL file not found, clear name */
          }
        }
        fclose(fp);
      }
    }
  }

  /* collect debug probes, initialize interface */
  appstate.probelist = get_probelist(&appstate.probe, &appstate.netprobe);
  trace_setdatasize((appstate.datasize == 3) ? 4 : (short)appstate.datasize);
  tcpip_init();
  bmp_setcallback(bmp_callback);
  appstate.reinitialize = 2; /* skip first iteration, so window is updated */
  tracelog_statusmsg(TRACESTATMSG_BMP, "Initializing...", BMPSTAT_SUCCESS);

  struct nk_context *ctx = guidriver_init("BlackMagic Trace Viewer", canvas_width, canvas_height,
                                          GUIDRV_RESIZEABLE | GUIDRV_TIMER,
                                          opt_fontstd, opt_fontmono, opt_fontsize);
  nuklear_style(ctx);

  int waitidle = 1;
  for ( ;; ) {
    /* handle state, (re-)connect and/or (re-)load of CTF definitions */
    handle_stateaction(&appstate);

    /* Input */
    nk_input_begin(ctx);
    if (!guidriver_poll(waitidle))
      break;
    nk_input_end(ctx);

    /* other events */
    int dev_event = guidriver_monitor_usb(0x1d50, 0x6018);
    if (dev_event != 0) {
      if (dev_event == DEVICE_REMOVE)
        bmp_disconnect();
      appstate.reinitialize = nk_true;
    }

    /* GUI */
    guidriver_appsize(&canvas_width, &canvas_height);
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, canvas_width, canvas_height), NK_WINDOW_NO_SCROLLBAR)) {
      nk_splitter_resize(&splitter_hor, canvas_width - 3 * SPACING, RESIZE_TOPLEFT);
      nk_splitter_resize(&splitter_ver, canvas_height - (ROW_HEIGHT + 6 * SPACING), RESIZE_TOPLEFT);
      nk_hsplitter_layout(ctx, &splitter_hor, canvas_height - 2 * SPACING);
      ctx->style.window.padding.x = 2;
      ctx->style.window.padding.y = 2;
      ctx->style.window.group_padding.x = 0;
      ctx->style.window.group_padding.y = 0;

      /* left column */
      if (nk_group_begin(ctx, "left", NK_WINDOW_NO_SCROLLBAR)) {
        button_bar(ctx, &appstate);

        /* trace log */
        int count = tracestring_process(appstate.trace_running);
        appstate.trace_count += count;
        waitidle = (count == 0);
        nk_layout_row_dynamic(ctx, nk_vsplitter_rowheight(&splitter_ver, 0), 1);
        int limitlines = appstate.trace_running ? appstate.line_limit : -1;
        tracelog_widget(ctx, "tracelog", opt_fontsize, limitlines, appstate.cur_match_line, appstate.filterlist, NK_WINDOW_BORDER);

        /* vertical splitter */
        nk_vsplitter(ctx, &splitter_ver);

        /* timeline & button bar */
        nk_layout_row_dynamic(ctx, nk_vsplitter_rowheight(&splitter_ver, 1), 1);
        double click_time = timeline_widget(ctx, "timeline", opt_fontsize, limitlines, NK_WINDOW_BORDER);
        appstate.cur_match_line = (click_time >= 0.0) ? tracestring_findtimestamp(click_time) : -1;

        nk_group_end(ctx);
      }

      /* column splitter */
      nk_hsplitter(ctx, &splitter_hor);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        panel_options(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        panel_status(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        filter_options(ctx, &appstate, tab_states);
        channel_options(ctx, &appstate, tab_states);
        nk_group_end(ctx);
      }

      /* popup dialogs */
      find_popup(ctx, &appstate, nk_hsplitter_colwidth(&splitter_hor, 0), canvas_height);
      help_popup(ctx, &appstate, canvas_width, canvas_height);

      /* mouse cursor shape */
      if (nk_is_popup_open(ctx))
        pointer_setstyle(CURSOR_NORMAL);
      else if (splitter_ver.hover)
        pointer_setstyle(CURSOR_UPDOWN);
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

  /* save configuration */
  save_settings(txtConfigFile, &appstate, tab_states, &splitter_hor, &splitter_ver);
  ini_putf("Settings", "fontsize", opt_fontsize, txtConfigFile);
  ini_puts("Settings", "fontstd", opt_fontstd, txtConfigFile);
  ini_puts("Settings", "fontmono", opt_fontmono, txtConfigFile);
  sprintf(valstr, "%d %d", canvas_width, canvas_height);
  ini_puts("Settings", "size", valstr, txtConfigFile);

  clear_probelist(appstate.probelist, appstate.netprobe);
  if (appstate.monitor_cmds != NULL)
    free((void*)appstate.monitor_cmds);
  trace_close();
  guidriver_close();
  tracestring_clear();
  bmscript_clear();
  gdbrsp_packetsize(0);
  ctf_parse_cleanup();
  ctf_decode_cleanup();
  dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
  bmp_disconnect();
  tcpip_cleanup();
  return EXIT_SUCCESS;
}

