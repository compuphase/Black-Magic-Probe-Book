/*
 * Statistical Profiler for the Black Magic Probe, using the PC sampler of the
 * DWT/ITM modules of the Cortex debug architecture. This utility is built with
 * Nuklear for a cross-platform GUI.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmcommon.h"
#include "bmp-script.h"
#include "bmp-scan.h"
#include "bmp-support.h"
#include "demangle.h"
#include "dwarf.h"
#include "elf.h"
#include "gdb-rsp.h"
#include "guidriver.h"
#include "mcu-info.h"
#include "minIni.h"
#include "nuklear_guide.h"
#include "nuklear_mousepointer.h"
#include "nuklear_splitter.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"
#include "osdialog.h"
#include "swotrace.h"
#include "tcpip.h"
#include "svnrev.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __linux__ || defined __unix__
# include "res/icon_profile_64.h"
#endif

#if !defined _MAX_PATH
# define _MAX_PATH 260
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


static DWARF_LINETABLE dwarf_linetable = { NULL };
static DWARF_SYMBOLLIST dwarf_symboltable = { NULL};
static DWARF_PATHLIST dwarf_filetable = { NULL};

#define WINDOW_WIDTH    700     /* default window size (window is resizable) */
#define WINDOW_HEIGHT   400
#define FONT_HEIGHT     14      /* default font size */
#define ROW_HEIGHT      (1.6 * opt_fontsize)
#define COMBOROW_CY     (0.9 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.5 * opt_fontsize)
static float opt_fontsize = FONT_HEIGHT;


int ctf_error_notify(int code, int linenr, const char *message)
{
  /* This function is not used in the profiler, but required as a dependency
     due to the "swotrace" module */
  (void)code;
  (void)linenr;
  (void)message;
  return 0;
}

static int bmp_callback(int code, const char *message)
{
  tracelog_statusmsg(TRACESTATMSG_BMP, message, code);
  return code >= 0;
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
    printf("BMProfile - Statistical Profiler for the Black Magic Probe.\n\n");
  printf("Usage: bmprofile [options] [filename]\n\n"
         "Options:\n"
         "-f=value  Font size to use (value must be 8 or larger).\n"
         "-h        This help.\n\n"
         "filename  Path to the ELF file to profile (must contain debug info).\n"
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

  printf("BMProfile version %s.\n", SVNREV_STR);
  printf("Copyright 2022-2023 CompuPhase\nLicensed under the Apache License version 2.0\n");
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

enum {
  TAB_CONFIGURATION,
  TAB_PROFILE,
  TAB_STATUS,
  /* --- */
  TAB_COUNT
};

enum {
  MODE_MANCHESTER = 1,
  MODE_ASYNC
};

enum {
  VIEW_TOP,
  VIEW_FUNCTION,
};

typedef struct tagFUNCTIONINFO {
  const char *name;
  uint32_t addr_low, addr_high;
  int line_low, line_high;      /**< line number range in the source file */
  short fileindex;              /**< file index in DWARF table */
  unsigned count;               /**< sample count (for the function) */
  double ratio;                 /**< scaling ratio (bar graph) */
  char percentage[16];          /**< pre-formatted string */
} FUNCTIONINFO;

typedef struct tagLINEINFO {
  const char *text;
  unsigned linenr;
  unsigned count;               /**< sample count (for the source line) */
  double ratio;                 /**< scaling ratio (bar graph) */
  char percentage[16];          /**< pre-formatted string */
} LINEINFO;

typedef struct tagAPPSTATE {
  int curstate;                 /**< current state */
  int probe;                    /**< selected debug probe (index) */
  int netprobe;                 /**< index for the IP address (pseudo-probe) */
  const char **probelist;       /**< list of detected probes */
  char mcu_family[64];          /**< detected MCU family (on attach), also the "driver" name of BMP */
  char mcu_architecture[32];    /**< detected ARM architecture (on attach) */
  unsigned long mcu_partid;     /**< specific ID code (0 if unknown) */
  const char *monitor_cmds;     /**< list of "monitor" commands (target & probe dependent) */
  char IPaddr[64];              /**< IP address for network probe */
  unsigned char trace_endpoint; /**< standard USB endpoint for tracing */
  int probe_type;               /**< BMP or ctxLink (needed to select manchester/async mode) */
  int swomode;                  /**< manchester or async */
  int init_target;              /**< whether to configure the target MCU for tracing */
  nk_bool init_bmp;             /**< whether to configure the debug probe for tracing */
  int connect_srst;             /**< whether to force reset while attaching */
  char mcuclock_str[16];        /**< edit buffer for CPU clock frequency */
  unsigned long mcuclock;       /**< active CPU clock frequency */
  char bitrate_str[16];         /**< edit buffer for bitrate */
  unsigned long bitrate;        /**< active bitrate */
  int trace_status;             /**< status of the USB or TCP/IP configuration for SWO tracing */
  bool connected;               /**< connected to BMP? */
  bool attached;                /**< attached to target? */
  bool dwarf_loaded;            /**< whether DWARF information is valid */
  bool init_done;               /**< whether target & BMP have been initialized (assuming init_target is set) */
  bool firstrun;                /**< is this the first run after a connect? */
  int view;                     /**< histogram or function list */
  char refreshrate_str[16];     /**< edit buffer for refresh intreval */
  double refreshrate;           /**< refresh interval */
  double refresh_tstamp;        /**< timestamp of most recent refresh */
  double capture_tstamp;        /**< time stamp of the start of collecting the samples */
  char samplingfreq_str[16];    /**< edit buffer for sampling frequency */
  unsigned long samplingfreq;   /**< set sampling frequency */
  unsigned long actual_freq;    /**< calculated sampling frequency */
  int accumulate;               /**< accumulate all samples since start of a run */
  char ELFfile[_MAX_PATH];      /**< ELF file for symbol/address look-up */
  char ParamFile[_MAX_PATH];    /**< debug parameters for the ELF file */
  unsigned long code_base;      /**< low address of code range of the ELF file */
  unsigned long code_top;       /**< top address */
  unsigned *sample_map;         /**< array containing sample counts */
  unsigned sample_unknown;      /**< samples that fall outside the ELF file address range */
  unsigned total_samples;       /**< total number of samples collected */
  unsigned overflow;            /**< number of overflow packets reported */
  unsigned numfunctions;        /**< top view: number of functions in the function list */
  FUNCTIONINFO *functionlist;   /**< top view: name + address range of al functions (sorted on address) */
  unsigned *functionorder;      /**< top view: indices in the function list for ordering by hit count */
  unsigned numlines;            /**< source view: number of source lines */
  LINEINFO *sourcelines;        /**< source view: source text */
  uint32_t source_addr_low;     /**< source view: lowest code address of interest */
  uint32_t source_addr_high;    /**< source view: highest code address of interest */
  unsigned *addr2line;          /**< source view: address-to-linenumber map */
  bool help_popup;              /**< whether "help" popup is active */
} APPSTATE;

enum {
  STATE_IDLE,
  STATE_CONNECT,        /**< connect to BMP */
  STATE_ATTACH,         /**< attach to target MCU */
  STATE_INIT_USB,
  STATE_LOAD_DWARF,
  STATE_INIT_TARGET,
  STATE_CONFIG_PROFILE,
  STATE_RUN,            /**< start to run */
  STATE_RUNNING,        /**< currently sampling */
  STATE_STOP,           /**< stop running */
  STATE_STOPPED,
};

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

  ini_putl("Settings", "init-target", state->init_target, filename);
  ini_putl("Settings", "init-bmp", state->init_bmp, filename);
  ini_putf("Settings", "splitter", splitter_hor->ratio, filename);
  for (int idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    sprintf(key, "view%d", idx);
    ini_putl("Settings", key, tab_states[idx], filename);
  }

  if (bmp_is_ip_address(state->IPaddr))
    ini_puts("Settings", "ip-address", state->IPaddr, filename);
  ini_putl("Settings", "probe", (state->probe == state->netprobe) ? 99 : state->probe, filename);

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

  state->init_target = (int)ini_getl("Settings", "init-target", 1, filename);
  state->init_bmp = (nk_bool)ini_getl("Settings", "init-bmp", 1, filename);
  state->probe = (int)ini_getl("Settings", "probe", 0, filename);
  ini_gets("Settings", "ip-address", "127.0.0.1", state->IPaddr, sizearray(state->IPaddr), filename);

  splitter_hor->ratio = ini_getf("Settings", "splitter", 0.0, filename);
  if (splitter_hor->ratio < 0.05f || splitter_hor->ratio > 0.95f)
    splitter_hor->ratio = 0.70f;

  for (int idx = 0; idx < TAB_COUNT; idx++) {
    char key[40], valstr[100];
    int opened, result;
    tab_states[idx] = NK_MAXIMIZED;
    sprintf(key, "view%d", idx);
    ini_gets("Settings", key, "", valstr, sizearray(valstr), filename);
    result = sscanf(valstr, "%d", &opened);
    if (result >= 1)
      tab_states[idx] = opened;
  }

  return true;
}

static bool save_targetoptions(const char *filename, APPSTATE *state)
{
  assert(state != NULL);

  if (filename == NULL || strlen(filename) == 0)
    return false;

  ini_putl("Settings", "connect-srst", state->connect_srst, filename);
  ini_putl("SWO trace", "mode", state->swomode, filename);
  ini_putl("SWO trace", "clock", state->mcuclock, filename);
  ini_putl("SWO trace", "bitrate", state->bitrate, filename);

  ini_putl("Profile", "sample-rate", state->samplingfreq, filename);
  ini_putf("Profile", "refresh-rate", state->refreshrate, filename);
  ini_putl("Profile", "accumulate", state->accumulate, filename);

  return access(filename, 0) == 0;
}

static bool load_targetoptions(const char *filename, APPSTATE *state)
{
  assert(state != NULL);

  if (filename == NULL || strlen(filename) == 0 || access(filename, 0) != 0)
    return false;

  state->connect_srst = (int)ini_getl("Settings", "connect-srst", 0, filename);
  state->swomode = (int)ini_getl("SWO trace", "mode", MODE_MANCHESTER, filename);
  state->mcuclock = ini_getl("SWO trace", "clock", 48000000, filename);
  state->bitrate = ini_getl("SWO trace", "bitrate", 100000, filename);

  state->samplingfreq = ini_getl("Profile", "sample-rate", 1000, filename);
  state->refreshrate = ini_getf("Profile", "refresh-rate", 1.0, filename);
  state->accumulate = (int)ini_getl("Profile", "accumulate", 0, filename);

  if (state->samplingfreq == 0)
    state->samplingfreq = 1000;
  if (state->refreshrate < 0.1)
    state->refreshrate = 1.0;

  sprintf(state->mcuclock_str, "%lu", state->mcuclock);
  sprintf(state->bitrate_str, "%lu", state->bitrate);
  sprintf(state->samplingfreq_str, "%lu", state->samplingfreq);
  sprintf(state->refreshrate_str, "%.1f", state->refreshrate);
  return true;
}

static void probe_set_options(APPSTATE *state)
{
  if (bmp_isopen() && state->monitor_cmds != NULL) {
    char cmd[100];
    if (bmp_expand_monitor_cmd(cmd, sizearray(cmd), "connect", state->monitor_cmds)) {
      strlcat(cmd, " ", sizearray(cmd));
      strlcat(cmd, state->connect_srst ? "enable" : "disable", sizearray(cmd));
      if (!bmp_monitor(cmd))
        bmp_callback(BMPERR_MONITORCMD, "Setting connect-with-reset option failed");
    }
  }
}

static void profile_graph(struct nk_context *ctx, const char *id, APPSTATE *state, float rowheight, nk_flags widget_flags)
{
  assert(ctx != NULL);
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  float graphwidth = rcwidget.w * 0.25;
  float lineheight = 0.0;
  int linecount = 0;

  struct nk_window *win = ctx->current;
  struct nk_user_font const *font = ctx->style.font;

  struct nk_style_window *stwin = &ctx->style.window;
  nk_style_push_color(ctx, &stwin->fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, widget_flags)) {
    if (tracelog_getstatusmsg(0) != NULL) {
      const char *text;
      for (int idx = 0; (text = tracelog_getstatusmsg(idx)) != NULL; idx++) {
        struct nk_color clr = COLOUR_FG_YELLOW;
        nk_layout_row_dynamic(ctx, rowheight, 1);
        nk_label_colored(ctx, text, NK_TEXT_LEFT, clr);
      }
    } else if (state->view == VIEW_TOP) {
      for (unsigned idx = 0; idx < state->numfunctions; idx++) {
        unsigned fidx = state->functionorder[idx];
        assert(fidx < state->numfunctions);
        nk_layout_row_begin(ctx, NK_STATIC, rowheight, 2);
        if (lineheight < 0.1) {
          struct nk_rect rcline = nk_layout_widget_bounds(ctx);
          lineheight = rcline.h;
        }
        /* draw bar */
        nk_layout_row_push(ctx, graphwidth);
        struct nk_rect rc = nk_widget_bounds(ctx);
        assert(state->functionlist[fidx].ratio >= 0.0 && state->functionlist[fidx].ratio <= 1.0);
        rc.w *= state->functionlist[fidx].ratio;
        nk_fill_rect(&win->buffer, rc, 0.0f, COLOUR_BG_YELLOW);
        nk_label(ctx, state->functionlist[fidx].percentage, NK_TEXT_RIGHT);
        /* print function name (get the width for the text first) */
        const char *name = state->functionlist[fidx].name;
        int len = strlen(name);
        assert(font != NULL && font->width != NULL);
        int textwidth = (int)font->width(font->userdata, font->height, name, len) + 10;
        nk_layout_row_push(ctx, (float)textwidth);
        nk_text(ctx, name, len, NK_TEXT_LEFT);
        nk_layout_row_end(ctx);
        linecount += 1;
      }
    } else {
      assert(state->view == VIEW_FUNCTION);
      for (unsigned idx = 0; idx < state->numlines; idx++) {
        nk_layout_row_begin(ctx, NK_STATIC, rowheight, 2);
        if (lineheight < 0.1) {
          struct nk_rect rcline = nk_layout_widget_bounds(ctx);
          lineheight = rcline.h;
        }
        /* draw bar */
        nk_layout_row_push(ctx, graphwidth);
        struct nk_rect rc = nk_widget_bounds(ctx);
        rc.w *= state->sourcelines[idx].ratio;
        nk_fill_rect(&win->buffer, rc, 0.0f, COLOUR_BG_YELLOW);
        nk_label(ctx, state->sourcelines[idx].percentage, NK_TEXT_RIGHT);
        /* print source line (get the width for the text first) */
        const char *text = state->sourcelines[idx].text;
        int len = strlen(text);
        assert(font != NULL && font->width != NULL);
        int textwidth = (int)font->width(font->userdata, font->height, text, len) + 10;
        nk_layout_row_push(ctx, (float)textwidth);
        nk_text(ctx, text, len, NK_TEXT_LEFT);
        nk_layout_row_end(ctx);
        linecount += 1;
      }
    }
    nk_group_end(ctx);
  }
  nk_style_pop_color(ctx);

  /* handle mouse input (reduce width & height of rcwidget, so that a click on
     a scrollbar is not taken into account */
  rcwidget.w -= 16;
  rcwidget.h -= 16;
  if (nk_input_is_mouse_hovering_rect(&ctx->input, rcwidget) && lineheight >= 0.1 && tracelog_getstatusmsg(0) == NULL) {
    struct nk_mouse *mouse = &ctx->input.mouse;
    assert(mouse != NULL);
    assert(NK_INBOX(mouse->pos.x, mouse->pos.y, rcwidget.x, rcwidget.y, rcwidget.w, rcwidget.h));
    unsigned xscroll, yscroll;
    nk_group_get_scroll(ctx, id, &xscroll, &yscroll);
    int row = (int)(((mouse->pos.y - rcwidget.y) + yscroll) / lineheight);
    if (row < linecount) {
      if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, rcwidget)) {
        /* clear source-code data (regardless of whether moving into the source
           code view or returning to the function list */
        if (state->sourcelines != NULL) {
          for (unsigned idx = 0; idx < state->numlines; idx++) {
            assert(state->sourcelines[idx].text != NULL);
            if (state->sourcelines[idx].text != NULL)
              free((void*)state->sourcelines[idx].text);
          }
          free((void*)state->sourcelines);
          state->sourcelines = NULL;
        }
        if (state->addr2line != NULL) {
          free((void*)state->addr2line);
          state->addr2line = NULL;
        }
        state->numlines = 0;
        state->source_addr_low = 0;
        state->source_addr_high = 0;
        /* toggle view */
        state->view = (state->view == VIEW_TOP) ? VIEW_FUNCTION : VIEW_TOP;
        if (state->view == VIEW_FUNCTION) {
          assert(row < state->numfunctions);
          unsigned fidx = state->functionorder[row];
          assert(fidx < state->numfunctions);
          state->source_addr_low = state->functionlist[fidx].addr_low;
          state->source_addr_high = state->functionlist[fidx].addr_high;
          /* create map to look up line number from address */
          uint32_t addr_range = (state->source_addr_high - state->source_addr_low) / ADDRESS_ALIGN;
          assert(addr_range > 0);
          if (addr_range > 0)
            state->addr2line = (unsigned*)malloc(addr_range * sizeof(unsigned));
          if (state->addr2line != NULL) {
            for (uint32_t addr = state->source_addr_low; addr < state->source_addr_high; addr += ADDRESS_ALIGN) {
              const DWARF_LINEENTRY *lineinfo = dwarf_line_from_address(&dwarf_linetable, addr);
              if (lineinfo != NULL) {
                unsigned idx = Address2Index(addr, state->source_addr_low);
                assert(idx < addr_range);
                state->addr2line[idx] = lineinfo->line;
              }
            }
          }
          /* load source code for the function */
          FILE *fp = NULL;
          const char *path = (addr_range > 0) ? dwarf_path_from_fileindex(&dwarf_filetable, state->functionlist[fidx].fileindex) : NULL;
          if (path != NULL) {
            fp = fopen(path, "rt");
            if (fp == NULL) {
              /* get directory of ELF file, append "path" and retry */
              char fullpath[_MAX_PATH];
              strlcpy(fullpath, state->ELFfile, sizearray(fullpath));
              char *ptr = strrchr(fullpath, DIRSEP_CHAR);
              if (ptr != NULL)
                *(ptr + 1) = '\0';
              strlcat(fullpath, path, sizearray(fullpath));
              fp = fopen(fullpath, "rt");
            }
          }
          if (fp != NULL) {
            unsigned numlines = state->functionlist[fidx].line_high - state->functionlist[fidx].line_low;
            state->sourcelines = (LINEINFO*)malloc(numlines * sizeof(LINEINFO));
            if (state->sourcelines != NULL) {
              memset(state->sourcelines, 0, numlines * sizeof(LINEINFO));
              char text[200];
              for (unsigned idx = 1; idx < state->functionlist[fidx].line_low; idx++)
                fgets(text, sizeof text, fp); /* skip first lines */
              numlines = 0;
              for (unsigned idx = state->functionlist[fidx].line_low; idx < state->functionlist[fidx].line_high; idx++) {
                if (fgets(text, sizeof text, fp) == NULL)
                  break;  /* should not happen: source file is shorter than DWARF info indicates */
                char *ptr = strchr(text, '\n');
                if (ptr != NULL)
                  *ptr = '\0';
                state->sourcelines[numlines].text = strdup(text);
                state->sourcelines[numlines].linenr = idx;
                numlines += 1;
              }
              state->numlines = numlines;
            }
            fclose(fp);
          } else {
            /* add string "No source code for ***" */
            unsigned numlines = 2;
            state->sourcelines = (LINEINFO*)malloc(numlines * sizeof(LINEINFO));
            if (state->sourcelines != NULL) {
              memset(state->sourcelines, 0, numlines * sizeof(LINEINFO));
              char text[200];
              sprintf(text, "No source code for \"%s\"", state->functionlist[fidx].name);
              state->sourcelines[0].text = strdup(text);
              state->sourcelines[1].text = strdup("Click here to return to the function list");
              state->numlines = numlines;
            }
          }
        }
      } else {
        if (state->view == VIEW_FUNCTION)
          nk_tooltip(ctx, "Click to return to the function list");
        else
          nk_tooltip(ctx, "Click for a detailed view");
      }
    }
  }
}

static void profile_reset(APPSTATE *state, bool samples)
{
  if (samples && state->sample_map != NULL) {
    unsigned count = (state->code_top - state->code_base) / ADDRESS_ALIGN + 1;
    memset(state->sample_map, 0, count * sizeof(unsigned));
  }

  if (state->view == VIEW_TOP && state->functionlist != NULL) {
    FUNCTIONINFO *functionlist = state->functionlist;
    unsigned numfunctions = state->numfunctions;
    for (unsigned idx = 0; idx < numfunctions; idx++) {
      functionlist[idx].count = 0;
      functionlist[idx].ratio = 0.0;
    }
  }

  if (state->view == VIEW_FUNCTION && state->sourcelines != NULL) {
    LINEINFO *sourcelines = state->sourcelines;
    unsigned numlines = state->numlines;
    for (unsigned idx = 0; idx < numlines; idx++) {
      sourcelines[idx].count = 0;
      sourcelines[idx].ratio = 0.0;
    }
  }
}

static bool profile_save(const char *filename, APPSTATE *state)
{
  FILE *fp = fopen(filename, "wt");
  if (fp == NULL)
    return false;
  fprintf(fp, "Address,Samples,Function,Source,Line\n");
  if (state->sample_map != NULL) {
    unsigned *sample_map = state->sample_map;
    unsigned count = (state->code_top - state->code_base) / ADDRESS_ALIGN;
    uint32_t code_base = state->code_base;
    for (unsigned idx = 0; idx < count; idx++) {
      if (sample_map[idx] == 0)
        continue;
      uint32_t addr = Index2Address(idx, code_base);

      /* get function (binary search) */
      FUNCTIONINFO *functionlist = state->functionlist;
      unsigned numfunctions = state->numfunctions;
      unsigned func_idx;
      unsigned low = 0;
      unsigned high = numfunctions - 1;
      while (low <= high) {
        func_idx = low + (high - low) / 2;
        if (functionlist[func_idx].addr_low <= addr && addr < functionlist[func_idx].addr_high)
          break;
        assert(functionlist[func_idx].addr_low < addr || functionlist[func_idx].addr_high >= addr);
        if (functionlist[func_idx].addr_low < addr)
          low = func_idx + 1;
        else
          high = func_idx - 1;
      }

      const char *name = "";
      int linenr = 0;
      const char *path = "";
      if (addr >= functionlist[func_idx].addr_low && addr < functionlist[func_idx].addr_high) {
        name = functionlist[func_idx].name;
        /* get line number & file path */
        const DWARF_LINEENTRY *lineinfo = dwarf_line_from_address(&dwarf_linetable, addr);
        if (lineinfo != NULL) {
          linenr = lineinfo->line;
          const char *p = dwarf_path_from_fileindex(&dwarf_filetable, lineinfo->fileindex);
          if (p != NULL)
            path = p;
        }
      }

      fprintf(fp, "%lx,%u,\"%s\",\"%s\",%d\n", (unsigned long)addr, sample_map[idx], name, path, linenr);
    }
  }
  fclose(fp);
  return true;
}

static void profile_graph_top(APPSTATE *state)
{
  if (state->sample_map != NULL && state->functionlist != NULL && state->numfunctions > 0) {
    /* clear functions counts */
    FUNCTIONINFO *functionlist = state->functionlist;
    unsigned numfunctions = state->numfunctions;
    for (unsigned idx = 0; idx < numfunctions; idx++)
      functionlist[idx].count = 0;
    state->sample_unknown = 0;

    /* accumulate function counts from sample map */
    unsigned total_samples = 0;
    unsigned *sample_map = state->sample_map;
    unsigned count = (state->code_top - state->code_base) / ADDRESS_ALIGN;
    uint32_t code_base = state->code_base;
    unsigned func_idx = 0;
    for (unsigned idx = 0; idx < count; idx++) {
      if (sample_map[idx] == 0)
        continue;
      uint32_t addr = Index2Address(idx, code_base);
      if (addr < functionlist[func_idx].addr_low || addr >= functionlist[func_idx].addr_high) {
        /* use binary search to find the function */
        unsigned low = 0;
        unsigned high = numfunctions - 1;
        while (low <= high) {
          func_idx = low + (high - low) / 2;
          if (functionlist[func_idx].addr_low <= addr && addr < functionlist[func_idx].addr_high)
            break;
          assert(functionlist[func_idx].addr_low < addr || functionlist[func_idx].addr_high >= addr);
          if (functionlist[func_idx].addr_low < addr)
            low = func_idx + 1;
          else
            high = func_idx - 1;
        }
      }
      if (functionlist[func_idx].addr_low <= addr && addr < functionlist[func_idx].addr_high)
        functionlist[func_idx].count += sample_map[idx];
      else
        state->sample_unknown += sample_map[idx];
      total_samples += sample_map[idx];
    }

    /* all samples beyond the ELF file address range are collected in this slot */
    state->sample_unknown += sample_map[count];
    total_samples += sample_map[count];
    state->total_samples = total_samples;

    /* calculate scaling factors */
    if (total_samples > 0) {
      double peak = 0.0;
      for (unsigned idx = 0; idx < numfunctions; idx++) {
        functionlist[idx].ratio = (double)functionlist[idx].count / total_samples;
        sprintf(functionlist[idx].percentage, "%5.1f%%  ", 100.0 * functionlist[idx].ratio);
        if (functionlist[idx].ratio > peak)
          peak = functionlist[idx].ratio;
      }
      /* equalize the scaling somewhat, so that the bar graph emphasizes the
         differences */
      if (peak < 0.1)
        peak = 0.1;
      double scale = 0.5 / peak + 0.5;
      for (unsigned idx = 0; idx < numfunctions; idx++)
        functionlist[idx].ratio *= scale;
    } else {
      for (unsigned idx = 0; idx < numfunctions; idx++) {
        functionlist[idx].ratio = 0.0;
        functionlist[idx].percentage[0] = '\0';
      }
    }

    /* sort the functions using "insertion sort"; this is classified as an
       inefficient sort, but it is quite efficient if the list is already
       (nearly) sorted */
    unsigned *functionorder = state->functionorder;
    for (unsigned i = 1; i < numfunctions; i++) {
      unsigned key = functionorder[i];
      assert(key < numfunctions);
      unsigned long key_samples = functionlist[key].count;
      unsigned j;
      for (j = i; j > 0 && functionlist[functionorder[j - 1]].count < key_samples; j--)
        functionorder[j] = functionorder[j - 1];
      functionorder[j] = key;
    }
  }
}

static void profile_graph_source(APPSTATE *state)
{
  if (state->sample_map != NULL && state->sourcelines != NULL && state->numlines > 0 && state->addr2line != NULL) {
    /* clear line counts */
    LINEINFO *sourcelines = state->sourcelines;
    unsigned numlines = state->numlines;
    for (unsigned idx = 0; idx < numlines; idx++)
      sourcelines[idx].count = 0;

    /* accumulate function counts from sample map */
    unsigned total_samples = 0;
    unsigned *sample_map = state->sample_map;
    unsigned count = (state->code_top - state->code_base) / ADDRESS_ALIGN;
    uint32_t code_base = state->code_base;
    uint32_t addr_low = state->source_addr_low;
    uint32_t addr_high = state->source_addr_high;
    unsigned *addr2line = state->addr2line;
    unsigned line_count = state->numlines;
    unsigned first_line = sourcelines[0].linenr;
    for (unsigned idx = 0; idx < count; idx++) {
      if (sample_map[idx] == 0)
        continue;
      total_samples += sample_map[idx];
      uint32_t addr = Index2Address(idx, code_base);
      if (addr < addr_low || addr >= addr_high)
        continue;
      unsigned addr_idx = Address2Index(addr, addr_low);
      unsigned line_idx = addr2line[addr_idx] - first_line;
      if (line_idx < line_count)
        sourcelines[line_idx].count += sample_map[idx];
    }
    state->total_samples = total_samples;
    state->sample_unknown = sample_map[count];

    /* calculate scaling factors */
    if (total_samples > 0) {
      double peak = 0.0;
      for (unsigned idx = 0; idx < line_count; idx++) {
        sourcelines[idx].ratio = (double)sourcelines[idx].count / total_samples;
        sprintf(sourcelines[idx].percentage, "%5.1f%%  ", 100.0 * sourcelines[idx].ratio);
        if (sourcelines[idx].ratio > peak)
          peak = sourcelines[idx].ratio;
      }
      /* equalize the scaling somewhat, so that the bar graph emphasizes the
         differences */
      if (peak < 0.1)
        peak = 0.1;
      double scale = 0.5 / peak + 0.5;
      for (unsigned idx = 0; idx < line_count; idx++)
        sourcelines[idx].ratio *= scale;
    } else {
      for (unsigned idx = 0; idx < line_count; idx++) {
        sourcelines[idx].ratio = 0.0;
        sourcelines[idx].percentage[0] = '\0';
      }
    }
  }
}

static void clear_functions(APPSTATE *state)
{
  assert(state != NULL);
  if (state->functionlist != NULL) {
    for (unsigned idx = 0; idx < state->numfunctions; idx++)
      free((void*)state->functionlist[idx].name);
    free((void*)state->functionlist);
    state->functionlist = NULL;
  }
  if (state->functionorder != NULL) {
    free((void*)state->functionorder);
    state->functionorder = NULL;
  }
  state->numfunctions = 0;
}

static bool collect_functions(APPSTATE *state)
{
  assert(state != NULL);
  clear_functions(state);

  /* count & collect the function symbols from the DWARF info */
  unsigned dwarf_count = dwarf_collect_functions_in_file(&dwarf_symboltable, -1, DWARF_SORT_ADDRESS, NULL, 0);
  if (dwarf_count == 0)
    return false;
  const DWARF_SYMBOLLIST **dwarf_list = (const DWARF_SYMBOLLIST**)malloc(dwarf_count * sizeof(DWARF_SYMBOLLIST*));
  if (dwarf_list != NULL)
    dwarf_collect_functions_in_file(&dwarf_symboltable, -1, DWARF_SORT_ADDRESS, dwarf_list, dwarf_count);

  /* count & collect the function symbols in the ELF symbol table */
  unsigned elf_count = 0;
  ELF_SYMBOL *elf_list = NULL;
  FILE *fp_elf = fopen(state->ELFfile, "rb");
  assert(fp_elf != NULL);
  if (fp_elf != NULL) {
    if (elf_load_symbols(fp_elf, NULL, &elf_count) == ELFERR_NONE && elf_count > 0) {
      elf_list = (ELF_SYMBOL*)malloc(elf_count * sizeof(ELF_SYMBOL));
      if (elf_list != NULL)
        elf_load_symbols(fp_elf, elf_list, &elf_count);
    }
    fclose(fp_elf);
  }

  /* use the DWARF info as the primary table, but walk through the ELF symbols
     to find any functions that are not present in the DWARF table */
  state->numfunctions = dwarf_count;
  if (elf_list != NULL) {
    assert(elf_count > 0);
    for (unsigned elf_idx = 0; elf_idx < elf_count; elf_idx++) {
      if (!elf_list[elf_idx].is_func)
        continue;
      /* functions are always on even addresses, but the ELF symbol table uses
         the low bit to indicate a Thumb function; this is irrelevant here, so
         the low bit is cleared */
      unsigned long addr = elf_list[elf_idx].address & ~1;
      unsigned dwarf_idx;
      for (dwarf_idx = 0; dwarf_idx < dwarf_count && dwarf_list[dwarf_idx]->code_addr != addr; dwarf_idx++)
        {}
      if (dwarf_idx >= dwarf_count)
        state->numfunctions += 1; /* found a function in the ELF table that is not in the DWARF table */
      else
        elf_list[elf_idx].is_func = 0;  /* clear flag, to make the loop for inserting the symbols easier */
    }
  }

  /* allocate memory for the merged tables from DWARF and ELF */
  state->functionlist = (FUNCTIONINFO*)malloc(state->numfunctions * sizeof(FUNCTIONINFO));
  state->functionorder = (unsigned*)malloc(state->numfunctions * sizeof(unsigned));

  if (state->functionlist != NULL && state->functionorder != NULL) {
    memset(state->functionlist, 0, state->numfunctions * sizeof(FUNCTIONINFO));
    memset(state->functionorder, 0, state->numfunctions * sizeof(unsigned));
    /* the DWARF list is already sorted on address, copy relevant fields */
    for (unsigned idx = 0; idx < dwarf_count; idx++) {
      state->functionlist[idx].name = strdup(dwarf_list[idx]->name);
      state->functionlist[idx].addr_low = dwarf_list[idx]->code_addr;
      state->functionlist[idx].addr_high = dwarf_list[idx]->code_addr + dwarf_list[idx]->code_range;
      state->functionlist[idx].line_low = dwarf_list[idx]->line;
      state->functionlist[idx].line_high = dwarf_list[idx]->line_limit;
      state->functionlist[idx].fileindex = dwarf_list[idx]->fileindex;
    }
    /* add functions from the ELF symbol table */
    if (elf_list != NULL) {
      assert(elf_count > 0);
      for (unsigned elf_idx = 0; elf_idx < elf_count; elf_idx++) {
        if (!elf_list[elf_idx].is_func)
          continue; /* all functions that are also in the DWARF table had this flag cleared, see above */
        unsigned long addr = elf_list[elf_idx].address & ~1;
        /* find insertion point */
        unsigned pos;
        for (pos = 0; state->functionlist[pos].name != NULL && state->functionlist[pos].addr_low < addr; pos++)
          {}
        assert(pos < state->numfunctions);
        if (state->functionlist[pos].name != NULL) {
          unsigned count = state->numfunctions - (pos + 1);
          memmove(&state->functionlist[pos + 1], &state->functionlist[pos], count * sizeof(FUNCTIONINFO));
        }
        char plain[256];
        if (!demangle(plain, sizearray(plain), elf_list[elf_idx].name))
          strlcpy(plain, elf_list[elf_idx].name, sizearray(plain));
        state->functionlist[pos].name = strdup(plain);
        state->functionlist[pos].addr_low = elf_list[elf_idx].address;
        state->functionlist[pos].addr_high = elf_list[elf_idx].address + elf_list[elf_idx].size;
        state->functionlist[pos].line_low = 0;
        state->functionlist[pos].line_high = 0;
        state->functionlist[pos].fileindex = 0;
      }
    }
    /* create an initial sort order */
    for (unsigned idx = 0; idx < state->numfunctions; idx++)
      state->functionorder[idx] = idx;
  } else {
    if (state->functionlist != NULL) {
      free((void*)state->functionlist);
      state->functionlist = NULL;
    }
    if (state->functionorder != NULL) {
      free((void*)state->functionorder);
      state->functionorder = NULL;
    }
  }
  if (dwarf_list != NULL)
    free((void*)dwarf_list);
  if (elf_list != NULL) {
    elf_clear_symbols(elf_list, elf_count);
    free((void*)elf_list);
  }

  return state->functionlist != NULL && state->functionorder != NULL;
}

static void help_popup(struct nk_context *ctx, APPSTATE *state, float canvas_width, float canvas_height)
{
# include "bmprofile_help.h"

  if (state->help_popup) {
#   define MARGIN  10
    float w = opt_fontsize * 40;
    if (w > canvas_width - 2*MARGIN)  /* clip "ideal" help window size of canvas size */
      w = canvas_width - 2*MARGIN;
    float h = canvas_height * 0.75;
    struct nk_rect rc = nk_rect((canvas_width - w) / 2, (canvas_height - h) / 2, w, h);
#   undef MARGIN

    state->help_popup = nk_guide(ctx, &rc, opt_fontsize, (const char*)bmprofile_help, NULL);
  }
}

static void panel_options(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT],
                          float panel_width)
{
  static const char *mode_strings[] = { "Manchester", "NRZ/async." };

# define LABEL_WIDTH (4.5 * opt_fontsize)
# define VALUE_WIDTH (panel_width - LABEL_WIDTH - 26)

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", &tab_states[TAB_CONFIGURATION], NULL)) {
    int result;
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
      result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
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
      nk_layout_row_end(ctx);
      if (reconnect) {
        bmp_disconnect();
        state->curstate = STATE_CONNECT;
      }
    }
    if (state->probe_type == PROBE_UNKNOWN) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "Mode", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      result = state->swomode - MODE_MANCHESTER;
      result = nk_combo(ctx, mode_strings, NK_LEN(mode_strings), result, (int)opt_fontsize, nk_vec2(VALUE_WIDTH,4.5*opt_fontsize));
      if (state->swomode != result + MODE_MANCHESTER) {
        /* mode is 1-based, the result of nk_combo() is 0-based, which is
           why MODE_MANCHESTER is added (MODE_MANCHESTER == 1) */
        state->swomode = result + MODE_MANCHESTER;
        state->curstate = STATE_CONNECT;
      }
      nk_layout_row_end(ctx);
    }

    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Configure Target", &state->init_target, NK_TEXT_LEFT, "Configure the target microcontroller for SWO"))
      state->curstate = STATE_INIT_TARGET;
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Configure Debug Probe", &state->init_bmp, NK_TEXT_LEFT, "Activate SWO capture in the Black Magic Probe"))
      state->curstate = STATE_ATTACH;
    if (state->init_target || state->init_bmp) {
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
      if (checkbox_tooltip(ctx, "Reset target during connect", &state->connect_srst, NK_TEXT_LEFT, "Keep the target in reset state while scanning and attaching"))
        state->curstate = STATE_INIT_TARGET;
    }

    if (state->init_target) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "CPU clock", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                state->mcuclock_str, sizearray(state->mcuclock_str), nk_filter_decimal,
                                "CPU clock of the target microcontroller");
      if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(state->mcuclock_str, NULL, 10) != state->mcuclock))
        state->curstate = STATE_INIT_TARGET;
      nk_layout_row_end(ctx);
    }

    if (state->init_target || (state->init_bmp && state->swomode == MODE_ASYNC)) {
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "Bit rate", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, VALUE_WIDTH);
      result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                state->bitrate_str, sizearray(state->bitrate_str), nk_filter_decimal,
                                "SWO bit rate (data rate)");
      if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(state->bitrate_str, NULL, 10) != state->bitrate))
        state->curstate = STATE_INIT_TARGET;
      nk_layout_row_end(ctx);
    }

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "ELF file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
    bool error = editctrl_cond_color(ctx, !state->dwarf_loaded, COLOUR_BG_DARKRED);
    result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                              state->ELFfile, sizearray(state->ELFfile), nk_filter_ascii,
                              "ELF file for symbol lookup");
    editctrl_reset_color(ctx, error);
    if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED)) {
      state->dwarf_loaded = false;
      state->curstate = STATE_LOAD_DWARF;
    }
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
      nk_input_clear_mousebuttons(ctx);
      osdialog_filters *filters = osdialog_filters_parse("ELF Executables:elf;All files:*");
      char *fname = osdialog_file(OSDIALOG_OPEN, "Select ELF executable", NULL, state->ELFfile, filters);
      osdialog_filters_free(filters);
      if (fname != NULL) {
        strlcpy(state->ELFfile, fname, sizearray(state->ELFfile));
        free(fname);
        state->dwarf_loaded = false;
        state->curstate = STATE_LOAD_DWARF;
      }
    }
    nk_layout_row_end(ctx);
    nk_tree_state_pop(ctx);
  }
# undef LABEL_WIDTH
# undef VALUE_WIDTH
}

static void panel_profile(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states tab_states[TAB_COUNT],
                          float panel_width)
{
# define LABEL_WIDTH(n) ((n) * opt_fontsize)
# define VALUE_WIDTH(n) (panel_width - LABEL_WIDTH(n) - 26)

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Profile options", &tab_states[TAB_PROFILE], NULL)) {
    int result;
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH(7));
    nk_label(ctx, "Sample rate", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH(7));
    result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                              state->samplingfreq_str, sizearray(state->samplingfreq_str),
                              nk_filter_decimal, "Frequency in Hz at which the PC is sampled\n(Approximate: real sampling rate may deviate)");
    if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(state->samplingfreq_str, NULL, 10) != state->samplingfreq))
      state->curstate = STATE_CONFIG_PROFILE;
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH(7));
    nk_label(ctx, "Refresh interval", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH(7));
    result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                              state->refreshrate_str, sizearray(state->refreshrate_str),
                              nk_filter_float, "Interval in seconds between refreshes of the graph\nA fractional value can be set");
    if ((result & NK_EDIT_COMMITED) || (result & NK_EDIT_DEACTIVATED)) {
      state->refreshrate = strtod(state->refreshrate_str, NULL);
      if (state->refreshrate < 0.1)
        state->refreshrate = 1.0;
      else if (state->refreshrate > 600.0)
        state->refreshrate = 600.0;
    }
    nk_layout_row_end(ctx);

    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    checkbox_tooltip(ctx, "Accumulate samples", &state->accumulate, NK_TEXT_LEFT,
                     "Accumulate all samples since starting a profiling run");

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

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Status", &tab_states[TAB_STATUS], NULL)) {
    char valuestr[20];
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH(8));
    nk_label(ctx, "Real sample rate", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH(8));
    if (state->curstate == STATE_RUNNING)
      sprintf(valuestr, "%lu Hz", state->actual_freq);
    else
      sprintf(valuestr, "-");
    label_tooltip(ctx, valuestr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, "Measured sample rate");
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH(8));
    nk_label(ctx, "Overflow events", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH(8));
    sprintf(valuestr, "%u", state->overflow);
    label_tooltip(ctx, valuestr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, "Overflow event count (sample rate too high)");
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH(8));
    nk_label(ctx, "Overhead", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, VALUE_WIDTH(8));
    if (state->total_samples > 0)
      sprintf(valuestr, "%.1f%%",(100.0 * state->sample_unknown)/(double)state->total_samples);
    else
      strcpy(valuestr, "-");
    label_tooltip(ctx, valuestr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, "Percentage of samples in unidentified code");
    nk_layout_row_end(ctx);

    nk_tree_state_pop(ctx);
  }
# undef LABEL_WIDTH
# undef VALUE_WIDTH
}

static void button_bar(struct nk_context *ctx, APPSTATE *state)
{
  nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 4, nk_ratio(4, 0.25, 0.25, 0.25, 0.25));

  const char *ptr = (state->curstate == STATE_RUNNING) ? "Stop" : "Start";
  if (nk_button_label(ctx, ptr) || nk_input_is_key_pressed(&ctx->input, NK_KEY_F5)) {
    if (state->curstate == STATE_RUNNING)
      state->curstate = STATE_STOP;
    else if (!state->connected)
      state->curstate = STATE_CONNECT;
    else if (!state->attached)
      state->curstate = STATE_ATTACH;
    else if (state->trace_status != TRACESTAT_OK)
      state->curstate = STATE_INIT_USB;
    else
      state->curstate = STATE_RUN;
  }

  if (nk_button_label(ctx, "Clear")) {
    profile_reset(state, true);
    state->capture_tstamp = get_timestamp();
  }

  if (nk_button_label(ctx, "Save") || nk_input_is_key_pressed(&ctx->input, NK_KEY_SAVE)) {
    osdialog_filters *filters = osdialog_filters_parse("CSV files:csv;All files:*");
    char *fname = osdialog_file(OSDIALOG_SAVE, "Save to CSV file", NULL, NULL, filters);
    osdialog_filters_free(filters);
    if (fname != NULL) {
      /* copy to local path, so that default extension can be appended */
      char path[_MAX_PATH];
      strlcpy(path, fname, sizearray(path));
      free(fname);
      const char *ext;
      if ((ext = strrchr(path, '.')) == NULL || strchr(ext, DIRSEP_CHAR) != NULL)
        strlcat(path, ".csv", sizearray(path)); /* default extension .csv */
      profile_save(path, state);
    }
  }

  if (nk_button_label(ctx, "Help") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F1))
    state->help_popup = true;
}

static void handle_stateaction(APPSTATE *state)
{
  switch (state->curstate) {
  case STATE_IDLE:
    break;
  case STATE_CONNECT:
    trace_close();
    bmp_disconnect();
    dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
    tracelog_statusclear();
    tracelog_statusmsg(TRACESTATMSG_BMP, "Initializing...", BMPSTAT_SUCCESS);
    state->connected = bmp_connect(state->probe, (state->probe == state->netprobe) ? state->IPaddr : NULL);
    state->firstrun = true;
    state->dwarf_loaded = false;
    state->attached = false;
    state->curstate = (state->connected) ? STATE_ATTACH : STATE_IDLE;
    if (state->connected && state->monitor_cmds == NULL)
      state->monitor_cmds = bmp_get_monitor_cmds();
    break;
  case STATE_ATTACH:
    if (state->init_bmp) {
      probe_set_options(state);
      state->attached = bmp_attach(true, state->mcu_family, sizearray(state->mcu_family),
                                   state->mcu_architecture, sizearray(state->mcu_architecture));
      if (state->attached) {
        /* overrule any default protocol setting, if the debug probe can be
           verified */
        state->probe_type = bmp_checkversionstring();
        if (state->probe_type == PROBE_BMPv21 || state->probe_type == PROBE_BMPv23)
          state->swomode = MODE_MANCHESTER;
        else if (state->probe_type == PROBE_CTXLINK)
         state->swomode = MODE_ASYNC;
        if (strncmp(state->mcu_architecture, "M0", 2) == 0)
          tracelog_statusmsg(TRACESTATMSG_BMP, "Cortex M0/M0+ architecture does not support profiling.", BMPSTAT_NOTICE);
        /* get probe commands again, to also get the target-specific commands */
        if (state->monitor_cmds != NULL)
          free((void*)state->monitor_cmds);
        state->monitor_cmds = bmp_get_monitor_cmds();
      }
      state->curstate = (state->attached) ? STATE_LOAD_DWARF : STATE_IDLE;
    } else {
      state->curstate = STATE_LOAD_DWARF;
    }
    break;
  case STATE_LOAD_DWARF:
    if (!state->attached) {
      state->curstate = STATE_IDLE;
      break;
    }
    if (strlen(state->ELFfile) == 0) {
      tracelog_statusmsg(TRACESTATMSG_BMP, "No ELF file given.", BMPSTAT_NOTICE);
    } else if (access(state->ELFfile, 0) != 0) {
      tracelog_statusmsg(TRACESTATMSG_BMP, "Specified ELF cannot be opened.", BMPSTAT_NOTICE);
    } else {
      FILE *fp = fopen(state->ELFfile, "rb");
      if (fp != NULL) {
        /* get range of all code sections */
        state->code_base = state->code_top = 0;
        for (int segm = 0; ; segm++) {
          unsigned long vaddr, memsize;
          int type, flags;
          int err = elf_segment_by_index(fp, segm, &type, &flags, NULL, NULL, &vaddr, NULL, &memsize);
          if (err != ELFERR_NONE)
            break;
          if (type == ELF_PT_LOAD && (flags & ELF_PF_X) != 0) {
            /* only handle loadable segments that are executable */
            if (state->code_base == 0 && state->code_top == 0) {
              state->code_base = vaddr;
              state->code_top = vaddr + memsize;
            } else {
              unsigned long top = vaddr + memsize;
              if (vaddr < state->code_base)
                state->code_base = vaddr;
              if (top > state->code_top)
                state->code_top = vaddr;
            }
          }
        }
        /* allocate memory for sample map */
        unsigned count = (state->code_top - state->code_base) / ADDRESS_ALIGN + 1;  /* +1 for out-of-range samples */
        state->sample_map = (unsigned*)malloc(count * sizeof(unsigned));
        if (state->sample_map != NULL)
          memset(state->sample_map, 0, count * sizeof(unsigned));
        else
          tracelog_statusmsg(TRACESTATMSG_BMP, "Memory allocation error.", BMPSTAT_NOTICE);
        /* load dwarf */
        int address_size;
        if (dwarf_read(fp, &dwarf_linetable, &dwarf_symboltable, &dwarf_filetable, &address_size))
          state->dwarf_loaded = true;
        else
          tracelog_statusmsg(TRACESTATMSG_BMP, "No debug information in ELF file (DWARF format).", BMPSTAT_NOTICE);
        fclose(fp);
        if (state->dwarf_loaded)
          collect_functions(state);
      }
    }
    profile_reset(state, true);
    state->curstate = state->dwarf_loaded ? STATE_INIT_TARGET : STATE_IDLE;
    break;
  case STATE_INIT_TARGET:
    if (state->init_target) {
      state->mcuclock = strtoul(state->mcuclock_str, NULL, 10);
      state->bitrate = strtoul(state->bitrate_str, NULL, 10);
      state->samplingfreq = strtoul(state->samplingfreq_str, NULL, 10);
      state->refreshrate = strtod(state->refreshrate_str, NULL);
      int errcount = 0;
      if (state->mcuclock < 1000) {
        tracelog_statusmsg(TRACESTATMSG_BMP, "CPU clock frequency not set (or invalid).", BMPSTAT_NOTICE);
        errcount += 1;
      }
      if (state->bitrate < 100) {
        tracelog_statusmsg(TRACESTATMSG_BMP, "Bit rate (SWO) not set (or invalid).", BMPSTAT_NOTICE);
        errcount += 1;
      }
      if (state->samplingfreq < 10) {
        tracelog_statusmsg(TRACESTATMSG_BMP, "Sampling rate not set (or invalid).", BMPSTAT_NOTICE);
        errcount += 1;
      }
      if (state->refreshrate < 0.001) {
        tracelog_statusmsg(TRACESTATMSG_BMP, "Refresh interval not set (or invalid).", BMPSTAT_NOTICE);
        errcount += 1;
      }
      if (errcount > 0) {
        state->curstate = STATE_IDLE;
        break;
      }
      unsigned long params[4];
      /* check to get more specific information on the MCU (specifically to
         update the mcu_family name, so that the appropriate scripts can be
         loaded) */
      if (bmp_has_command("partid", state->monitor_cmds)) {
        state->mcu_partid = bmp_get_partid();
      } else if (bmp_runscript("partid", state->mcu_family, state->mcu_architecture, params, 1)) {
        state->mcu_partid = params[0];
        const char *mcuname = mcuinfo_lookup(state->mcu_family, state->mcu_partid);
        if (mcuname != NULL) {
          strlcpy(state->mcu_family, mcuname, sizearray(state->mcu_family));
          bmscript_clear();
        }
      }
      /* initialize the target (target-specific configuration, generic
         configuration and channels */
      bmp_runscript("swo_device", state->mcu_family, state->mcu_architecture, NULL, 0);
      assert(state->swomode == MODE_MANCHESTER || state->swomode == MODE_ASYNC);
      unsigned long swvclock = (state->swomode == MODE_MANCHESTER) ? 2 * state->bitrate : state->bitrate;
      assert(state->mcuclock > 0 && swvclock > 0);
      double div_value = (double)state->mcuclock / (1024 * (double)state->samplingfreq);
      unsigned long divider = (unsigned long)(div_value + 0.5);
      if (divider < 1)
        divider = 1;
      else if (divider > 16)
        divider = 16;
      params[0]= state->swomode;
      params[1] = state->mcuclock / swvclock - 1;
      params[2] = divider - 1;
      bmp_runscript("swo_profile", state->mcu_family, state->mcu_architecture, params, 3);
      state->init_done = true;
    }
    tracelog_statusmsg(TRACESTATMSG_BMP, "Starting profiling run...", BMPSTAT_SUCCESS);
    state->curstate = STATE_RUN;
    break;
  case STATE_RUN:
    tracelog_statusclear();
    if (state->init_target && !state->init_done) {
      state->curstate = STATE_INIT_TARGET;
      break;
    }
    profile_reset(state, true);
    state->trace_status = trace_init(state->trace_endpoint, (state->probe == state->netprobe) ? state->IPaddr : NULL);
    state->curstate = (state->trace_status == TRACESTAT_OK) ? STATE_RUNNING : STATE_IDLE;
    if (state->firstrun) {
      state->capture_tstamp = get_timestamp();
      state->actual_freq = state->samplingfreq;
      bmp_enabletrace((state->swomode == MODE_ASYNC) ? state->bitrate : 0, &state->trace_endpoint);
      bmp_restart();
      state->firstrun = false;
    }
    state->curstate = STATE_RUNNING;
    break;
  case STATE_RUNNING:
    break;
  case STATE_STOP:
    trace_close();
    state->curstate = STATE_STOPPED;
    break;
  case STATE_STOPPED:
    break;
  }
}

int main(int argc, char *argv[])
{
  /* global defaults */
  APPSTATE appstate;
  memset(&appstate, 0, sizeof appstate);
  appstate.curstate = STATE_CONNECT;
  appstate.swomode = MODE_MANCHESTER;
  appstate.mcuclock = 48000000;
  appstate.bitrate = 100000;
  appstate.probe_type = PROBE_UNKNOWN;
  appstate.trace_endpoint = BMP_EP_TRACE;
  appstate.init_target = nk_true;
  appstate.init_bmp = nk_true;
  appstate.connect_srst = nk_false;
  appstate.view = VIEW_TOP;

# if defined FORTIFY
    Fortify_SetOutputFunc(Fortify_OutputFunc);
# endif

  char txtConfigFile[_MAX_PATH];
  get_configfile(txtConfigFile, sizearray(txtConfigFile), "bmprofile.ini");
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

# define SEPARATOR_HOR 4.0f
# define SPACING       4.0f
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
    } else {
      /* filename on the command line must be in native format (using backslashes
         on Windows) */
      if (access(argv[idx], 0) == 0)
        strlcpy(appstate.ELFfile, argv[idx], sizearray(appstate.ELFfile));
    }
  }
  if (strlen(appstate.ELFfile) == 0) {
    ini_gets("Session", "recent", "", appstate.ELFfile, sizearray(appstate.ELFfile), txtConfigFile);
    if (access(appstate.ELFfile, 0) != 0)
      appstate.ELFfile[0] = '\0';
  }

  /* if a target filename is known, create the parameter filename from target
     filename and read target-specific options */
  if (strlen(appstate.ELFfile) > 0) {
    strlcpy(appstate.ParamFile, appstate.ELFfile, sizearray(appstate.ParamFile));
    strlcat(appstate.ParamFile, ".bmcfg", sizearray(appstate.ParamFile));
    load_targetoptions(appstate.ParamFile, &appstate);
  }

  /* collect debug probes, initialize interface */
  appstate.probelist = get_probelist(&appstate.probe, &appstate.netprobe);
  tcpip_init();
  bmp_setcallback(bmp_callback);

  struct nk_context *ctx = guidriver_init("BlackMagic Profiler", canvas_width, canvas_height,
                                          GUIDRV_RESIZEABLE | GUIDRV_TIMER,
                                          opt_fontstd, opt_fontmono, opt_fontsize);
  nuklear_style(ctx);

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
    if (dev_event != 0) {
      if (dev_event == DEVICE_REMOVE)
        bmp_disconnect();
      appstate.curstate = STATE_CONNECT; /* BMP was inserted or removed */
    }

    /* GUI */
    guidriver_appsize(&canvas_width, &canvas_height);
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, (float)canvas_width, (float)canvas_height), NK_WINDOW_NO_SCROLLBAR)
        && canvas_width > 0 && canvas_height > 0) {
      nk_splitter_resize(&splitter_hor, (float)canvas_width - 3 * SPACING, RESIZE_TOPLEFT);
      nk_hsplitter_layout(ctx, &splitter_hor, (float)canvas_height - 2 * SPACING);
      ctx->style.window.padding.x = 2;
      ctx->style.window.padding.y = 2;
      ctx->style.window.group_padding.x = 0;
      ctx->style.window.group_padding.y = 0;

      /* left column */
      if (nk_group_begin(ctx, "left", NK_WINDOW_NO_SCROLLBAR)) {
        /* buttons */
        button_bar(ctx, &appstate);

        /* profile graph */
        int events = traceprofile_process(appstate.curstate == STATE_RUNNING, appstate.sample_map,
                                          appstate.code_base, appstate.code_top,
                                          &appstate.overflow);
        waitidle = (events == 0);
        /* if interval has passed, make copy of data for the graph */
        double tstamp = get_timestamp();
        if (tstamp - appstate.refresh_tstamp >= appstate.refreshrate && appstate.sample_map != NULL) {
          appstate.refresh_tstamp = tstamp;
          if (appstate.view == VIEW_TOP)
            profile_graph_top(&appstate);
          else
            profile_graph_source(&appstate);
          double freq = appstate.total_samples / (tstamp - appstate.capture_tstamp);
          appstate.actual_freq = (appstate.actual_freq + (unsigned long)(freq + 0.5)) / 2;
          if (appstate.curstate == STATE_RUNNING && !appstate.accumulate) {
            unsigned count = (appstate.code_top - appstate.code_base) / ADDRESS_ALIGN + 1;
            memset(appstate.sample_map, 0, count * sizeof(unsigned));
            appstate.capture_tstamp = tstamp;
          }
        }
        nk_layout_row_dynamic(ctx, canvas_height - ROW_HEIGHT - 3 * SPACING, 1);
        profile_graph(ctx, "graph", &appstate, opt_fontsize, NK_WINDOW_BORDER);

        nk_group_end(ctx);
      }

      /* column splitter */
      nk_hsplitter(ctx, &splitter_hor);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        panel_options(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        panel_profile(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        panel_status(ctx, &appstate, tab_states, nk_hsplitter_colwidth(&splitter_hor, 1));
        nk_group_end(ctx);
      }

      /* popup dialogs */
      help_popup(ctx, &appstate, (float)canvas_width, (float)canvas_height);

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
  save_targetoptions(appstate.ParamFile, &appstate);
  sprintf(valstr, "%d %d", canvas_width, canvas_height);
  ini_puts("Settings", "size", valstr, txtConfigFile);
  ini_puts("Session", "recent", appstate.ELFfile, txtConfigFile);

  clear_functions(&appstate);
  clear_probelist(appstate.probelist, appstate.netprobe);
  if (appstate.monitor_cmds != NULL)
    free((void*)appstate.monitor_cmds);
  trace_close();
  guidriver_close();
  tracestring_clear();
  bmscript_clear();
  gdbrsp_packetsize(0);
  dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
  bmp_disconnect();
  tcpip_cleanup();
  nk_guide_cleanup();
  return EXIT_SUCCESS;
}

