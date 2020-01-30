/*
 * Trace viewer utility for visualizing output on the TRACESWO pin via the
 * Black Magic Probe. This utility is built with Nuklear for a cross-platform
 * GUI.
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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined WIN32 || defined _WIN32
  #define STRICT
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #include <malloc.h>
  #if defined __MINGW32__ || defined __MINGW64__
    #include "strlcpy.h"
  #elif defined _MSC_VER
    #include "strlcpy.h"
    #define access(p,m)       _access((p),(m))
    #define mkdir(p)          _mkdir(p)
  #endif
#elif defined __linux__
  #include <alloca.h>
  #include <pthread.h>
  #include <unistd.h>
  #include <bsd/string.h>
  #include <sys/stat.h>
  #include <sys/time.h>
#endif

#include "guidriver.h"
#include "bmp-script.h"
#include "bmp-support.h"
#include "bmp-scan.h"
#include "gdb-rsp.h"
#include "minIni.h"
#include "noc_file_dialog.h"
#include "nuklear_tooltip.h"
#include "rs232.h"
#include "specialfolder.h"

#include "dwarf.h"
#include "elf.h"
#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"

#if defined __linux__ || defined __unix__
  #include "res/icon_download_64.h"
#endif

#if !defined _MAX_PATH
  #define _MAX_PATH 260
#endif

#ifndef NK_ASSERT
  #include <assert.h>
  #define NK_ASSERT(expr) assert(expr)
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  #define stricmp(s1,s2)    strcasecmp((s1),(s2))
#endif

#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if defined WIN32 || defined _WIN32
  #define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
  #define IS_OPTION(s)  ((s)[0] == '-')
#endif

static DWARF_LINELOOKUP dwarf_linetable = { NULL };
static DWARF_SYMBOLLIST dwarf_symboltable = { NULL};
static DWARF_PATHLIST dwarf_filetable = { NULL};

static int recent_statuscode = 0;

int ctf_error_notify(int code, int linenr, const char *message)
{
  static int ctf_statusset = 0;

  if (code == CTFERR_NONE) {
    ctf_statusset = 0;
  } else if (!ctf_statusset) {
    char msg[200];
    ctf_statusset = 1;
    if (linenr > 0)
      sprintf(msg, "TSDL file error, line %d: ", linenr);
    else
      strcpy(msg, "TSDL file error: ");
    strlcat(msg, message, sizearray(msg));
    tracelog_statusmsg(TRACESTATMSG_CTF, msg, 0);
  }
  return 0;
}

static int bmp_callback(int code, const char *message)
{
  recent_statuscode = code;
  tracelog_statusmsg(TRACESTATMSG_BMP, message, code);
  return code >= 0;
}


#define WINDOW_WIDTH    700     /* default window size (window is resizable) */
#define WINDOW_HEIGHT   400
#define FONT_HEIGHT     14
#define ROW_HEIGHT      (1.6 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.85 * opt_fontsize)


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

static void usage(void)
{
  printf("bmtrace - SWO Trace Viewer for the Black Magic Probe.\n\n"
         "Usage: bmtrace [options]\n\n"
         "Options:\n"
         "-f=value\t Font size to use (value must be 8 or larger).\n"
         "-h\t This help.\n"
         "-t=path\t path to the TSDL metadata file to use.\n");
}

int main(int argc, char *argv[])
{
  enum { TAB_CONFIGURATION, TAB_CHANNELS, /* --- */ TAB_COUNT };
  enum { SPLITTER_NONE, SPLITTER_VERTICAL, SPLITTER_HORIZONTAL };

  static const char *mode_strings[] = { "Manchester", "NRZ/async." };
  static const char *datasize_strings[] = { "auto", "8 bit", "16 bit", "32 bit" };
  struct nk_context *ctx;
  int canvas_width, canvas_height;
  int idx, insplitter;
  float splitter_hor = 0.70, splitter_ver = 0.70;
  enum nk_collapse_states tab_states[TAB_COUNT];
  char mcu_driver[32];
  char txtConfigFile[_MAX_PATH], findtext[128] = "", valstr[128] = "";
  char txtTSDLfile[_MAX_PATH] = "", txtELFfile[_MAX_PATH] = "";
  char cpuclock_str[15] = "", bitrate_str[15] = "";
  unsigned long cpuclock = 0, bitrate = 0;
  int chan, cur_chan_edit = -1;
  unsigned long channelmask = 0;
  enum { MODE_MANCHESTER = 1, MODE_ASYNC } opt_mode = MODE_MANCHESTER;
  unsigned char trace_endpoint = BMP_EP_TRACE;
  int opt_init_target = 1;
  int opt_init_bmp = 1;
  int opt_datasize = 0;
  int opt_fontsize = FONT_HEIGHT;
  int trace_status = TRACESTAT_NOT_INIT;
  int trace_running = 1;
  int reinitialize =  1;
  int reload_format = 1;
  int cur_match_line = -1;
  int find_popup = 0;

  /* locate the configuration file */
  if (folder_AppConfig(txtConfigFile, sizearray(txtConfigFile))) {
    strlcat(txtConfigFile, DIR_SEPARATOR "BlackMagic", sizearray(txtConfigFile));
    #if defined _WIN32
      mkdir(txtConfigFile);
    #else
      mkdir(txtConfigFile, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    #endif
    strlcat(txtConfigFile, DIR_SEPARATOR "bmtrace.ini", sizearray(txtConfigFile));
  }

  /* read channel configuration */
  for (chan = 0; chan < NUM_CHANNELS; chan++) {
    char key[40];
    unsigned clr;
    int enabled, result;
    channel_set(chan, (chan == 0), NULL, nk_rgb(190, 190, 190)); /* preset: port 0 is enabled by default, others disabled by default */
    sprintf(key, "chan%d", chan);
    ini_gets("Channels", key, "", valstr, sizearray(valstr), txtConfigFile);
    result = sscanf(valstr, "%d #%x %s", &enabled, &clr, key);
    if (result >= 2)
      channel_set(chan, enabled, (result >= 3) ? key : NULL, nk_rgb(clr >> 16,(clr >> 8) & 0xff, clr & 0xff));
  }
  /* other configuration */
  opt_mode = (int)ini_getl("Settings", "mode", MODE_MANCHESTER, txtConfigFile);
  opt_init_target = (int)ini_getl("Settings", "init-target", 1, txtConfigFile);
  opt_init_bmp = (int)ini_getl("Settings", "init-bmp", 1, txtConfigFile);
  if (opt_mode == 0) {  /* legacy: opt_mode == 0 was MODE_PASSIVE */
    opt_mode = MODE_MANCHESTER;
    opt_init_target = 0;
    opt_init_bmp = 0;
  }
  opt_datasize = (int)ini_getl("Settings", "datasize", 1, txtConfigFile);
  ini_gets("Settings", "tsdl", "", txtTSDLfile, sizearray(txtTSDLfile), txtConfigFile);
  ini_gets("Settings", "elf", "", txtELFfile, sizearray(txtELFfile), txtConfigFile);
  ini_gets("Settings", "mcu-freq", "48000000", cpuclock_str, sizearray(cpuclock_str), txtConfigFile);
  ini_gets("Settings", "bitrate", "100000", bitrate_str, sizearray(bitrate_str), txtConfigFile);
  ini_gets("Settings", "size", "", valstr, sizearray(valstr), txtConfigFile);
  opt_fontsize = (int)ini_getl("Settings", "fontsize", FONT_HEIGHT, txtConfigFile);
  if (sscanf(valstr, "%d %d", &canvas_width, &canvas_height) != 2 || canvas_width < 100 || canvas_height < 50) {
    canvas_width = WINDOW_WIDTH;
    canvas_height = WINDOW_HEIGHT;
  }
  ini_gets("Settings", "timeline", "", valstr, sizearray(valstr), txtConfigFile);
  if (strlen(valstr) > 0) {
    double spacing;
    unsigned long scale, delta;
    if (sscanf(valstr, "%lf %lu %lu", &spacing, &scale, &delta) == 3)
      timeline_setconfig(spacing, scale, delta);
  }

  ini_gets("Settings", "splitter", "", valstr, sizearray(valstr), txtConfigFile);
  if (sscanf(valstr, "%f %f", &splitter_hor, &splitter_ver) != 2 || splitter_hor < 0.1 || splitter_ver < 0.1) {
    splitter_hor = 0.70;
    splitter_ver = 0.70;
  }
  insplitter = SPLITTER_NONE;
  for (idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    int opened, result;
    tab_states[idx] = (idx == TAB_CONFIGURATION) ? NK_MAXIMIZED : NK_MINIMIZED;
    sprintf(key, "view%d", idx);
    ini_gets("Settings", key, "", valstr, sizearray(valstr), txtConfigFile);
    result = sscanf(valstr, "%d", &opened);
    if (result >= 1)
      tab_states[idx] = opened;
  }

  for (idx = 1; idx < argc; idx++) {
    const char *ptr;
    int value;
    if (IS_OPTION(argv[idx])) {
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage();
        return 0;
      case 'f':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        value = (int)strtol(ptr, NULL, 10);
        if (value >= 8)
          opt_fontsize = value;
        break;
      case 't':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        if (access(ptr, 0) == 0)
          strlcpy(txtTSDLfile, ptr, sizearray(txtTSDLfile));
        break;
      default:
        fprintf(stderr, "Unknown option %s; use option -h for help.\n", argv[idx]);
        return 1;
      }
    } else if (access(argv[idx], 0) == 0) {
      /* parameter is a filename, test whether that is an ELF file */
      FILE *fp = fopen(argv[idx], "rb");
      if (fp != NULL) {
        int err = elf_info(fp, NULL, NULL, NULL);
        if (err == ELFERR_NONE) {
          strlcpy(txtELFfile, argv[idx], sizearray(txtELFfile));
          if (access(txtTSDLfile, 0) != 0) {
            /* see whether there is a TSDL file with a matching name */
            char *ext;
            strlcpy(txtTSDLfile, txtELFfile, sizearray(txtTSDLfile));
            ext = strrchr(txtTSDLfile, '.');
            if (ext != NULL && strpbrk(ext, "\\/") == NULL)
              *ext = '\0';
            strlcat(txtTSDLfile, ".tsdl", sizearray(txtTSDLfile));
            if (access(txtTSDLfile, 0) != 0)
              txtTSDLfile[0] = '\0';  /* newly constructed file not found, clear name */
          }
        }
        fclose(fp);
      }
    }
  }

  trace_setdatasize((opt_datasize == 3) ? 4 : opt_datasize);
  bmp_setcallback(bmp_callback);
  reinitialize = 2; /* skip first iteration, so window is updated */
  recent_statuscode = BMPSTAT_SUCCESS;  /* must be a non-zero code to display anything */
  tracelog_statusmsg(TRACESTATMSG_BMP, "Initializing...", recent_statuscode);

  ctx = guidriver_init("BlackMagic Trace Viewer", canvas_width, canvas_height, GUIDRV_RESIZEABLE | GUIDRV_TIMER, FONT_HEIGHT);
  set_style(ctx);

  for ( ;; ) {
    if (reinitialize == 1) {
      int result;
      char msg[100];
      if ((cpuclock = strtol(cpuclock_str, NULL, 10)) == 0)
        cpuclock = 48000000;
      if ((bitrate = strtol(bitrate_str, NULL, 10)) == 0)
        bitrate = 100000;
      if (opt_init_target || opt_init_bmp) {
        /* open/reset the serial port/device if any initialization must be done */
        if (rs232_isopen())
          bmp_break();
        result = bmp_connect(); /* this function also opens the (virtual) serial port/device */
        if (result)
          result = bmp_attach(2, mcu_driver, sizearray(mcu_driver), NULL, 0);
        else
          trace_status = TRACESTAT_NO_CONNECT;
        if (result && opt_init_target) {
          /* initialize the target (target-specific configuration, generic
             configuration and channels */
          unsigned long params[2];
          bmp_runscript("swo_device", mcu_driver, NULL);
          assert(opt_mode == MODE_MANCHESTER || opt_mode == MODE_ASYNC);
          params[0] = opt_mode;
          params[1] = cpuclock / bitrate - 1;
          bmp_runscript("swo_generic", mcu_driver, params);
          /* enable active channels in the target (disable inactive channels) */
          channelmask = 0;
          for (chan = 0; chan < NUM_CHANNELS; chan++)
            if (channel_getenabled(chan))
              channelmask |= (1 << chan);
          params[0] = channelmask;
          bmp_runscript("swo_channels", mcu_driver, params);
        }
      } else if (rs232_isopen()) {
        /* no initialization is requested, if the serial port is open, close it
           (so that the gdbserver inside the BMP is available for debugging) */
        rs232_close();
        result = 1; /* flag status = ok, to drop into the next "if" */
      }
      if (result) {
        if (opt_init_bmp)
          bmp_enabletrace((opt_mode == MODE_ASYNC) ? bitrate : 0, &trace_endpoint);
        trace_status = trace_init(trace_endpoint);  /* this does nothing if initialization had already succeeded */
        bmp_restart();
      }
      tracestring_clear();
      trace_running = (trace_status != TRACESTAT_OK);
      switch (trace_status) {
      case TRACESTAT_OK:
        recent_statuscode = BMPSTAT_SUCCESS;
        if (opt_init_target || opt_init_bmp) {
          assert(strlen(mcu_driver) > 0);
          sprintf(msg, "Connected [%s]", mcu_driver);
          tracelog_statusmsg(TRACESTATMSG_BMP, msg, recent_statuscode);
        } else if (recent_statuscode >= 0) {
          tracelog_statusmsg(TRACESTATMSG_BMP, "Listening (passive mode)...", recent_statuscode);
        }
        break;
      case TRACESTAT_INIT_FAILED:
      case TRACESTAT_NO_INTERFACE:
      case TRACESTAT_NO_DEVPATH:
      case TRACESTAT_NO_PIPE:
        recent_statuscode = BMPERR_GENERAL;
        tracelog_statusmsg(TRACESTATMSG_BMP, "Trace interface not available", recent_statuscode);
        break;
      case TRACESTAT_NO_ACCESS:
        recent_statuscode = BMPERR_GENERAL;
        sprintf(msg, "Trace access denied (error %lu)", trace_errno());
        tracelog_statusmsg(TRACESTATMSG_BMP, msg, recent_statuscode);
        break;
      case TRACESTAT_NO_THREAD:
        recent_statuscode = BMPERR_GENERAL;
        sprintf(msg, "Multithreading failed (error %lu)", trace_errno());
        tracelog_statusmsg(TRACESTATMSG_BMP, msg, recent_statuscode);
        break;
      case TRACESTAT_NO_CONNECT:
        recent_statuscode = BMPERR_GENERAL;
        tracelog_statusmsg(TRACESTATMSG_BMP, "Failed to \"attach\" to Black Magic Probe", recent_statuscode);
        break;
      }
      reinitialize = 0;
    } else if (reinitialize > 0) {
      reinitialize -= 1;
    }

    if (reload_format) {
      ctf_parse_cleanup();
      ctf_decode_cleanup();
      tracestring_clear();
      dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
      cur_match_line = -1;
      tracelog_statusmsg(TRACESTATMSG_CTF, NULL, 0);
      ctf_error_notify(CTFERR_NONE, 0, NULL);
      if (strlen(txtTSDLfile)> 0 && access(txtTSDLfile, 0) == 0) {
        if (ctf_parse_init(txtTSDLfile) && ctf_parse_run()) {
          const CTF_STREAM *stream;
          int seqnr;
          /* stream names overrule configured channel names */
          for (seqnr = 0; (stream = stream_by_seqnr(seqnr)) != NULL; seqnr++)
            if (stream->name != NULL && strlen(stream->name) > 0)
              channel_setname(seqnr, stream->name);
        } else {
          ctf_parse_cleanup();
        }
      }
      if (strlen(txtELFfile) > 0 && access(txtELFfile, 0) == 0) {
        FILE *fp = fopen(txtELFfile, "rb");
        if (fp != NULL) {
          int address_size;
          dwarf_read(fp, &dwarf_linetable, &dwarf_symboltable, &dwarf_filetable, &address_size);
          fclose(fp);
        }
      }
      reload_format = 0;
    }

    /* Input */
    nk_input_begin(ctx);
    if (!guidriver_poll(1))
      break;
    nk_input_end(ctx);

    /* GUI */
    guidriver_appsize(&canvas_width, &canvas_height);
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, canvas_width, canvas_height), NK_WINDOW_NO_SCROLLBAR)) {
      #define SEPARATOR_HOR 4
      #define SEPARATOR_VER 4
      #define SPACING       4
      float splitter_columns[3];
      struct nk_rect rc_canvas = nk_rect(0, 0, canvas_width, canvas_height);
      struct nk_rect bounds;

      #define EXTRA_SPACE_HOR     (SEPARATOR_HOR + 3 * SPACING)
      splitter_columns[0] = (canvas_width - EXTRA_SPACE_HOR) * splitter_hor;
      splitter_columns[1] = SEPARATOR_HOR;
      splitter_columns[2] = (canvas_width - EXTRA_SPACE_HOR) - splitter_columns[0];
      nk_layout_row(ctx, NK_STATIC, canvas_height - 2 * SPACING, 3, splitter_columns);
      ctx->style.window.padding.x = 2;
      ctx->style.window.padding.y = 2;
      ctx->style.window.group_padding.x = 0;
      ctx->style.window.group_padding.y = 0;

      /* left column */
      if (nk_group_begin(ctx, "left", NK_WINDOW_NO_SCROLLBAR)) {
        const char *ptr;
        float splitter_rows[2];
        double click_time;

        #define EXTRA_SPACE_VER   (2 * SEPARATOR_VER + ROW_HEIGHT + 7 * SPACING)
        splitter_rows[0] = (canvas_height - EXTRA_SPACE_VER) * splitter_ver;
        splitter_rows[1] = (canvas_height - EXTRA_SPACE_VER) - splitter_rows[0];

        if (trace_status == TRACESTAT_OK && tracestring_isempty() && trace_getpacketerrors() > 0) {
          char msg[100];
          recent_statuscode = BMPERR_GENERAL;
          sprintf(msg, "SWO packet errors (%d), verify data size", trace_getpacketerrors());
          tracelog_statusmsg(TRACESTATMSG_BMP, msg, recent_statuscode);
        }
        tracestring_process(trace_running);
        nk_layout_row_dynamic(ctx, splitter_rows[0], 1);
        tracelog_widget(ctx, "tracelog", FONT_HEIGHT, cur_match_line, NK_WINDOW_BORDER);

        /* vertical splitter */
        nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
        bounds = nk_widget_bounds(ctx);
        nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_REPEAT(3));
        if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
          insplitter = SPLITTER_VERTICAL; /* in vertical splitter */
        else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
          insplitter = SPLITTER_NONE;
        if (insplitter == SPLITTER_VERTICAL)
          splitter_ver = (splitter_rows[0] + ctx->input.mouse.delta.y) / (canvas_height - EXTRA_SPACE_VER);

        nk_layout_row_dynamic(ctx, splitter_rows[1], 1);
        click_time = timeline_widget(ctx, "timeline", FONT_HEIGHT, NK_WINDOW_BORDER);
        cur_match_line = (click_time >= 0.0) ? tracestring_findtimestamp(click_time) : -1;

        nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
        nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 7, nk_ratio(7, 0.19, 0.08, 0.19, 0.08, 0.19, 0.08, 0.19));
        ptr = trace_running ? "Stop" : tracestring_isempty() ? "Start" : "Resume";
        if (nk_button_label(ctx, ptr) || nk_input_is_key_pressed(&ctx->input, NK_KEY_F5)) {
          trace_running = !trace_running;
          if (trace_running && trace_status != TRACESTAT_OK) {
            trace_status = trace_init(trace_endpoint);
            if (trace_status != TRACESTAT_OK)
              trace_running = 0;
          }
        }
        nk_spacing(ctx, 1);
        if (nk_button_label(ctx, "Clear")) {
          tracestring_clear();
          cur_match_line = -1;
        }
        nk_spacing(ctx, 1);
        if (nk_button_label(ctx, "Search") || nk_input_is_key_pressed(&ctx->input, NK_KEY_FIND))
          find_popup = 1;
        nk_spacing(ctx, 1);
        if (nk_button_label(ctx, "Save") || nk_input_is_key_pressed(&ctx->input, NK_KEY_SAVE)) {
          const char *s = noc_file_dialog_open(NOC_FILE_DIALOG_SAVE,
                                               "CSV files\0*.csv\0All files\0*.*\0",
                                               NULL, NULL, NULL, guidriver_apphandle());
          if (s != NULL) {
            trace_save(s);
            free((void*)s);
          }
        }
        nk_group_end(ctx);
      }

      /* column splitter */
      bounds = nk_widget_bounds(ctx);
      nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_VERTICAL | NK_SYMBOL_REPEAT(3));
      if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
        insplitter = SPLITTER_HORIZONTAL; /* in horizontal splitter */
      else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
        insplitter = SPLITTER_NONE;
      if (insplitter == SPLITTER_HORIZONTAL)
        splitter_hor = (splitter_columns[0] + ctx->input.mouse.delta.x) / (canvas_width - SEPARATOR_HOR - 2 * SPACING);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        #define LABEL_WIDTH (4.5 * FONT_HEIGHT)
        #define VALUE_WIDTH (splitter_columns[2] - LABEL_WIDTH - 26)
        int result;
        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", &tab_states[TAB_CONFIGURATION])) {
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "Mode", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_layout_row_push(ctx, VALUE_WIDTH);
          result = opt_mode - MODE_MANCHESTER;
          result = nk_combo(ctx, mode_strings, NK_LEN(mode_strings), result, FONT_HEIGHT, nk_vec2(VALUE_WIDTH,4.5*FONT_HEIGHT));
          if (opt_mode != result + MODE_MANCHESTER) {
            /* opt_mode is 1-based, the result of nk_combo() is 0-based, which is
               we MODE_MANCHESTER is added (MODE_MANCHESTER == 1) */
            opt_mode = result + MODE_MANCHESTER;
            reinitialize = 1;
          }
          nk_layout_row_end(ctx);
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          bounds = nk_widget_bounds(ctx);
          if (nk_checkbox_label(ctx, "Configure Target", &opt_init_target))
            reinitialize = 1;
          tooltip(ctx, bounds, "Configure the target microcontroller for SWO", &rc_canvas);
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          bounds = nk_widget_bounds(ctx);
          if (nk_checkbox_label(ctx, "Configure Debug Probe", &opt_init_bmp))
            reinitialize = 1;
          tooltip(ctx, bounds, "Activate SWO trace capture in the Black Magic Probe", &rc_canvas);
          if (opt_init_target) {
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
            nk_layout_row_push(ctx, LABEL_WIDTH);
            nk_label(ctx, "CPU clock", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
            nk_layout_row_push(ctx, VALUE_WIDTH);
            bounds = nk_widget_bounds(ctx);
            result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, cpuclock_str, sizearray(cpuclock_str), nk_filter_decimal);
            if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(cpuclock_str, NULL, 10) != cpuclock))
              reinitialize = 1;
            tooltip(ctx, bounds, "CPU clock of the target microcontroller", &rc_canvas);
            nk_layout_row_end(ctx);
          }
          if (opt_init_target || opt_init_bmp) {
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
            nk_layout_row_push(ctx, LABEL_WIDTH);
            nk_label(ctx, "Bit rate", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
            nk_layout_row_push(ctx, VALUE_WIDTH);
            bounds = nk_widget_bounds(ctx);
            result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, bitrate_str, sizearray(bitrate_str), nk_filter_decimal);
            if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(bitrate_str, NULL, 10) != bitrate))
              reinitialize = 1;
            tooltip(ctx, bounds, "SWO bit rate (data rate)", &rc_canvas);
            nk_layout_row_end(ctx);
          }
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "Data size", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_layout_row_push(ctx, VALUE_WIDTH);
          bounds = nk_widget_bounds(ctx);
          result = opt_datasize;
          opt_datasize = nk_combo(ctx, datasize_strings, NK_LEN(datasize_strings), opt_datasize, FONT_HEIGHT, nk_vec2(VALUE_WIDTH,5.5*FONT_HEIGHT));
          if (opt_datasize != result) {
            trace_setdatasize((opt_datasize == 3) ? 4 : opt_datasize);
            tracestring_clear();
            if (trace_status == TRACESTAT_OK) {
              recent_statuscode = BMPSTAT_SUCCESS;
              tracelog_statusmsg(TRACESTATMSG_BMP, "Listening ...", recent_statuscode);
            }
          }
          tooltip(ctx, bounds, "Payload size of an SWO packet (in bits); auto for autodetect", &rc_canvas);
          nk_layout_row_end(ctx);
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "TSDL file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
          bounds = nk_widget_bounds(ctx);
          result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, txtTSDLfile, sizearray(txtTSDLfile), nk_filter_ascii);
          if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
            reload_format = 1;
          tooltip(ctx, bounds, "Metadata file for Common Trace Format (CTF)", &rc_canvas);
          nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
          if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
            const char *s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                                                 "TSDL files\0*.tsdl;*.ctf\0All files\0*.*\0",
                                                 NULL, txtTSDLfile, "Select metadata file for CTF",
                                                 guidriver_apphandle());
            if (s != NULL && strlen(s) < sizearray(txtTSDLfile)) {
              strcpy(txtTSDLfile, s);
              reload_format = 1;
              free((void*)s);
            }
          }
          nk_layout_row_end(ctx);
          if (strlen(txtTSDLfile)> 0 && access(txtTSDLfile, 0) == 0) {
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
            nk_layout_row_push(ctx, LABEL_WIDTH);
            nk_label(ctx, "ELF file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
            nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
            bounds = nk_widget_bounds(ctx);
            result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, txtELFfile, sizearray(txtELFfile), nk_filter_ascii);
            if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
              reload_format = 1;
            tooltip(ctx, bounds, "ELF file for symbol lookup", &rc_canvas);
            nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
            if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
              const char *s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                                                   "ELF Executables\0*.elf;*.bin;*.\0All files\0*.*\0",
                                                   NULL, txtELFfile, "Select ELF Executable",
                                                   guidriver_apphandle());
              if (s != NULL && strlen(s) < sizearray(txtELFfile)) {
                strcpy(txtELFfile, s);
                reload_format = 1;
                free((void*)s);
              }
            }
            nk_layout_row_end(ctx);
          }
          nk_tree_state_pop(ctx);
        }

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Channels", &tab_states[TAB_CHANNELS])) {
          float labelwidth = tracelog_labelwidth(FONT_HEIGHT) + 10;
          struct nk_style_button stbtn = ctx->style.button;
          stbtn.border = 0;
          stbtn.rounding = 0;
          stbtn.padding.x = stbtn.padding.y = 0;
          for (chan = 0; chan < NUM_CHANNELS; chan++) {
            char label[32];
            int enabled;
            struct nk_color clrtxt, clrbk;
            nk_layout_row_begin(ctx, NK_STATIC, FONT_HEIGHT, 2);
            nk_layout_row_push(ctx, 3 * FONT_HEIGHT);
            sprintf(label, "%2d", chan);
            enabled = channel_getenabled(chan);
            if (nk_checkbox_label(ctx, label, &enabled)) {
              /* enable/disable channel in the target */
              channel_setenabled(chan, enabled);
              if (opt_init_target) {
                if (enabled)
                  channelmask |= (1 << chan);
                else
                  channelmask &= ~(1 << chan);
                bmp_runscript("swo_channels", mcu_driver, &channelmask);
              }
            }
            clrbk = channel_getcolor(chan);
            clrtxt = (clrbk.r + 2 * clrbk.g + clrbk.b < 700) ? nk_rgb(255,255,255) : nk_rgb(20,29,38);
            stbtn.normal.data.color = stbtn.hover.data.color
              = stbtn.active.data.color = stbtn.text_background = clrbk;
            stbtn.text_normal = stbtn.text_active = stbtn.text_hover = clrtxt;
            nk_layout_row_push(ctx, labelwidth);
            bounds = nk_widget_bounds(ctx);
            if (nk_button_label_styled(ctx, &stbtn, channel_getname(chan, NULL, 0))) {
              /* we want a contextual pop-up (that you can simply click away
                 without needing a close button), so we simulate a right-mouse
                 click */
              nk_input_motion(ctx, bounds.x, bounds.y + bounds.h - 1);
              nk_input_button(ctx, NK_BUTTON_RIGHT, bounds.x, bounds.y + bounds.h - 1, 1);
              nk_input_button(ctx, NK_BUTTON_RIGHT, bounds.x, bounds.y + bounds.h - 1, 0);
            }
            nk_layout_row_end(ctx);
            if (nk_contextual_begin_fitview(ctx, 0, nk_vec2(9*FONT_HEIGHT, 5*ROW_HEIGHT), bounds, &rc_canvas)) {
              nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
              clrbk.r = (nk_byte)nk_propertyi(ctx, "#R", 0, clrbk.r, 255, 1, 1);
              nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
              clrbk.g = (nk_byte)nk_propertyi(ctx, "#G", 0, clrbk.g, 255, 1, 1);
              nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
              clrbk.b = (nk_byte)nk_propertyi(ctx, "#B", 0, clrbk.b, 255, 1, 1);
              channel_setcolor(chan, clrbk);
              /* the name in the channels array must only be changed on closing
                 the popup, so it is copied to a local variable on first opening */
              if (cur_chan_edit == -1) {
                cur_chan_edit = chan;
                channel_getname(chan, valstr, sizearray(valstr));
              }
              nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.35, 0.65));
              nk_label(ctx, "name", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
              nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, valstr, sizearray(valstr), nk_filter_ascii);
              nk_contextual_end(ctx);
            } else if (cur_chan_edit == chan) {
              /* contextual popup is closed, copy the name back */
              if (strlen(valstr) == 0) {
                channel_setname(chan, NULL);
              } else {
                char *pspace;
                while ((pspace = strchr(valstr, ' ')) != NULL)
                  *pspace = '-'; /* can't handle spaces in the channel names */
                channel_setname(chan, valstr);
              }
              cur_chan_edit = -1;
            }
          }
          nk_tree_state_pop(ctx);
        }
        nk_group_end(ctx);
      }

      /* popup dialogs */
      if (find_popup > 0) {
        struct nk_rect rc;
        rc.x = canvas_width - 18 * FONT_HEIGHT;
        rc.y = canvas_height - 6.5 * ROW_HEIGHT;
        rc.w = 200;
        rc.h = 3.6 * ROW_HEIGHT;
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Search", NK_WINDOW_NO_SCROLLBAR, rc)) {
          nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.2, 0.8));
          nk_label(ctx, "Text", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_edit_focus(ctx, 0);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, findtext, sizearray(findtext), nk_filter_ascii);
          nk_layout_row(ctx, NK_DYNAMIC, FONT_HEIGHT, 2, nk_ratio(2, 0.2, 0.8));
          nk_spacing(ctx, 1);
          if (find_popup == 2)
            nk_label_colored(ctx, "Text not found", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, nk_rgb(255, 100, 128));
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 3);
          nk_spacing(ctx, 1);
          if (nk_button_label(ctx, "Find") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER)) {
            if (strlen(findtext) > 0) {
              int line = tracestring_find(findtext, cur_match_line);
              if (line != cur_match_line) {
                cur_match_line = line;
                find_popup = 0;
                trace_running = 0;
              } else {
                cur_match_line = -1;
                find_popup = 2; /* to mark "string not found" */
              }
              nk_popup_close(ctx);
            } /* if (len > 0) */
          }
          if (nk_button_label(ctx, "Cancel") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
            find_popup = 0;
            nk_popup_close(ctx);
          }
          nk_popup_end(ctx);
        } else {
          find_popup = 0;
        }
      }

    }
    nk_end(ctx);

    /* Draw */
    guidriver_render(nk_rgb(30,30,30));
  }

  for (chan = 0; chan < NUM_CHANNELS; chan++) {
    char key[40];
    struct nk_color color = channel_getcolor(chan);
    sprintf(key, "chan%d", chan);
    sprintf(valstr, "%d #%06x %s", channel_getenabled(chan),
            ((int)color.r << 16) | ((int)color.g << 8) | color.b,
            channel_getname(chan, NULL, 0));
    ini_puts("Channels", key, valstr, txtConfigFile);
  }
  sprintf(valstr, "%.2f %.2f", splitter_hor, splitter_ver);
  ini_puts("Settings", "splitter", valstr, txtConfigFile);
  for (idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    sprintf(key, "view%d", idx);
    sprintf(valstr, "%d", tab_states[idx]);
    ini_puts("Settings", key, valstr, txtConfigFile);
  }
  ini_putl("Settings", "fontsize", opt_fontsize, txtConfigFile);
  ini_putl("Settings", "mode", opt_mode, txtConfigFile);
  ini_putl("Settings", "init-target", opt_init_target, txtConfigFile);
  ini_putl("Settings", "init-bmp", opt_init_bmp, txtConfigFile);
  ini_putl("Settings", "datasize", opt_datasize, txtConfigFile);
  ini_puts("Settings", "tsdl", txtTSDLfile, txtConfigFile);
  ini_puts("Settings", "elf", txtELFfile, txtConfigFile);
  ini_putl("Settings", "mcu-freq", cpuclock, txtConfigFile);
  ini_putl("Settings", "bitrate", bitrate, txtConfigFile);
  sprintf(valstr, "%d %d", canvas_width, canvas_height);
  ini_puts("Settings", "size", valstr, txtConfigFile);
  {
    double spacing;
    unsigned long scale, delta;
    timeline_getconfig(&spacing, &scale, &delta);
    sprintf(valstr, "%.2f %lu %lu", spacing, scale, delta);
    ini_puts("Settings", "timeline", bitrate_str, txtConfigFile);
  }

  trace_close();
  guidriver_close();
  tracestring_clear();
  bmscript_clear();
  gdbrsp_packetsize(0);
  ctf_parse_cleanup();
  ctf_decode_cleanup();
  dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
  if (rs232_isopen()) {
    rs232_dtr(0);
    rs232_rts(0);
    rs232_close();
  }
  return 0;
}

