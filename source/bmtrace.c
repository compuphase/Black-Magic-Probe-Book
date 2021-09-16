/*
 * Trace viewer utility for visualizing output on the TRACESWO pin via the
 * Black Magic Probe. This utility is built with Nuklear for a cross-platform
 * GUI.
 *
 * Copyright 2019-2021 CompuPhase
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
  #define _WIN32_WINNT   0x0500 /* for AttachConsole() */
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
#include "nuklear_mousepointer.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"
#include "rs232.h"
#include "specialfolder.h"
#include "tcpip.h"

#include "dwarf.h"
#include "elf.h"
#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"

#if defined __linux__ || defined __unix__
  #include "res/icon_download_64.h"
#endif

#if defined _MSC_VER
  #define stricmp(a,b)    _stricmp((a),(b))
  #define strdup(s)       _strdup(s)
#endif

#if !defined _MAX_PATH
  #define _MAX_PATH 260
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


#define WINDOW_WIDTH    700     /* default window size (window is resizable) */
#define WINDOW_HEIGHT   400
#define FONT_HEIGHT     14      /* default font size */
#define ROW_HEIGHT      (1.6 * opt_fontsize)
#define COMBOROW_CY     (0.9 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.5 * opt_fontsize)

#define FILTER_MAXSTRING  128


#define ERROR_NO_TSDL 0x0001
#define ERROR_NO_ELF  0x0002

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
    printf("BMTrace - SWO Trace Viewer for the Black Magic Probe.\n\n");
  printf("Usage: bmtrace [options]\n\n"
         "Options:\n"
         "-f=value  Font size to use (value must be 8 or larger).\n"
         "-h        This help.\n"
         "-t=path   Path to the TSDL metadata file to use.\n");
}

int main(int argc, char *argv[])
{
  enum { TAB_CONFIGURATION, TAB_CHANNELS, TAB_FILTERS, /* --- */ TAB_COUNT };
  enum { SPLITTER_NONE, SPLITTER_VERTICAL, SPLITTER_HORIZONTAL };

  static const char *mode_strings[] = { "Manchester", "NRZ/async." };
  static const char *datasize_strings[] = { "auto", "8 bit", "16 bit", "32 bit" };
  struct nk_context *ctx;
  int canvas_width, canvas_height;
  int idx, insplitter;
  float splitter_hor = 0.70, splitter_ver = 0.70;
  enum nk_collapse_states tab_states[TAB_COUNT];
  char mcu_driver[32], mcu_architecture[32];
  char txtConfigFile[_MAX_PATH], findtext[128] = "", valstr[128] = "";
  char txtTSDLfile[_MAX_PATH] = "", txtELFfile[_MAX_PATH] = "";
  char txtIPaddr[64] = "";
  char cpuclock_str[15] = "", bitrate_str[15] = "";
  unsigned long cpuclock = 0, bitrate = 0;
  int chan, cur_chan_edit = -1;
  unsigned long channelmask = 0;
  int probe, usbprobes, netprobe;
  const char **probelist;
  int probe_type = PROBE_UNKNOWN;
  enum { MODE_MANCHESTER = 1, MODE_ASYNC } opt_mode = MODE_MANCHESTER;
  unsigned char trace_endpoint = BMP_EP_TRACE;
  char newfiltertext[FILTER_MAXSTRING] = "";
  TRACEFILTER *filterlist = NULL;
  int filtercount = 0, filterlistsize = 0;
  int opt_init_target = nk_true;
  int opt_init_bmp = nk_true;
  int opt_connect_srst = nk_false;
  int opt_datasize = 0;
  int opt_fontsize = FONT_HEIGHT;
  char opt_fontstd[64] = "", opt_fontmono[64] = "";
  int trace_status = TRACESTAT_NOT_INIT;
  int trace_running = 1;
  int reinitialize =  1;
  int reload_format = 1;
  int cur_match_line = -1;
  int find_popup = 0;
  int error_flags = 0;

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
    char key[41];
    unsigned clr;
    int enabled, result;
    channel_set(chan, (chan == 0), NULL, nk_rgb(190, 190, 190)); /* preset: port 0 is enabled by default, others disabled by default */
    sprintf(key, "chan%d", chan);
    ini_gets("Channels", key, "", valstr, sizearray(valstr), txtConfigFile);
    result = sscanf(valstr, "%d #%x %40s", &enabled, &clr, key);
    if (result >= 2)
      channel_set(chan, enabled, (result >= 3) ? key : NULL, nk_rgb(clr >> 16,(clr >> 8) & 0xff, clr & 0xff));
  }
  /* read filters (initialize the filter list) */
  filtercount = ini_getl("Filters", "count", 0, txtConfigFile);;
  filterlistsize = filtercount + 1; /* at least 1 extra, for a NULL sentinel */
  filterlist = malloc(filterlistsize * sizeof(TRACEFILTER));  /* make sure unused entries are NULL */
  if (filterlist != NULL) {
    memset(filterlist, 0, filterlistsize * sizeof(TRACEFILTER));
    for (idx = 0; idx < filtercount; idx++) {
      char key[40], *ptr;
      filterlist[idx].expr = malloc(sizearray(newfiltertext)* sizeof(char));
      if (filterlist[idx].expr == NULL)
        break;
      sprintf(key, "filter%d", idx + 1);
      ini_gets("Filters", key, "", newfiltertext, sizearray(newfiltertext), txtConfigFile);
      filterlist[idx].enabled = (int)strtol(newfiltertext, &ptr, 10);
      assert(ptr != NULL && *ptr != '\0');  /* a comma should be found */
      if (*ptr == ',')
        ptr += 1;
      strcpy(filterlist[idx].expr, ptr);
    }
    filtercount = idx;
  } else {
    filtercount = filterlistsize = 0;
  }
  newfiltertext[0] = '\0';

  /* other configuration */
  probe = (int)ini_getl("Settings", "probe", 0, txtConfigFile);
  ini_gets("Settings", "ip-address", "127.0.0.1", txtIPaddr, sizearray(txtIPaddr), txtConfigFile);
  opt_mode = (int)ini_getl("Settings", "mode", MODE_MANCHESTER, txtConfigFile);
  opt_init_target = (int)ini_getl("Settings", "init-target", 1, txtConfigFile);
  opt_init_bmp = (int)ini_getl("Settings", "init-bmp", 1, txtConfigFile);
  if (opt_mode == 0) {  /* legacy: opt_mode == 0 was MODE_PASSIVE */
    opt_mode = MODE_MANCHESTER;
    opt_init_target = 0;
    opt_init_bmp = 0;
  }
  opt_connect_srst = (int)ini_getl("Settings", "connect-srst", 0, txtConfigFile);
  opt_datasize = (int)ini_getl("Settings", "datasize", 1, txtConfigFile);
  ini_gets("Settings", "tsdl", "", txtTSDLfile, sizearray(txtTSDLfile), txtConfigFile);
  ini_gets("Settings", "elf", "", txtELFfile, sizearray(txtELFfile), txtConfigFile);
  ini_gets("Settings", "mcu-freq", "48000000", cpuclock_str, sizearray(cpuclock_str), txtConfigFile);
  ini_gets("Settings", "bitrate", "100000", bitrate_str, sizearray(bitrate_str), txtConfigFile);
  ini_gets("Settings", "size", "", valstr, sizearray(valstr), txtConfigFile);
  opt_fontsize = (int)ini_getl("Settings", "fontsize", FONT_HEIGHT, txtConfigFile);
  ini_gets("Settings", "fontstd", "", opt_fontstd, sizearray(opt_fontstd), txtConfigFile);
  ini_gets("Settings", "fontmono", "", opt_fontmono, sizearray(opt_fontmono), txtConfigFile);
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
    if (IS_OPTION(argv[idx])) {
      const char *ptr;
      int result;
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage(NULL);
        return 0;
      case 'f':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        result = (int)strtol(ptr, (char**)&ptr, 10);
        if (result >= 8)
          opt_fontsize = result;
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
          strlcpy(txtTSDLfile, ptr, sizearray(txtTSDLfile));
        break;
      default:
        usage(argv[idx]);
        return EXIT_FAILURE;
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

  /* collect debug probes, connect to the selected one */
  usbprobes = get_bmp_count();
  netprobe = (usbprobes > 0) ? usbprobes : 1;
  probelist = malloc((netprobe+1)*sizeof(char*));
  if (probelist != NULL) {
    if (usbprobes == 0) {
      probelist[0] = strdup("-");
    } else {
      char portname[64];
      for (idx = 0; idx < usbprobes; idx++) {
        find_bmp(idx, BMP_IF_GDB, portname, sizearray(portname));
        probelist[idx] = strdup(portname);
      }
    }
    probelist[netprobe] = strdup("TCP/IP");
  }
  if (probe == 99)
    probe = netprobe;
  else if (probe > usbprobes)
    probe = 0;

  trace_setdatasize((opt_datasize == 3) ? 4 : (short)opt_datasize);
  tcpip_init();
  bmp_setcallback(bmp_callback);
  reinitialize = 2; /* skip first iteration, so window is updated */
  tracelog_statusmsg(TRACESTATMSG_BMP, "Initializing...", BMPSTAT_SUCCESS);

  ctx = guidriver_init("BlackMagic Trace Viewer", canvas_width, canvas_height,
                       GUIDRV_RESIZEABLE | GUIDRV_TIMER, opt_fontstd, opt_fontmono, opt_fontsize);
  nuklear_style(ctx);

  for ( ;; ) {
    if (reinitialize == 1) {
      int result;
      char msg[100];
      tracelog_statusclear();
      tracestring_clear();
      if ((cpuclock = strtol(cpuclock_str, NULL, 10)) == 0)
        cpuclock = 48000000;
      if (opt_mode == MODE_MANCHESTER || (bitrate = strtol(bitrate_str, NULL, 10)) == 0)
        bitrate = 100000;
      if (opt_init_target || opt_init_bmp) {
        /* open/reset the serial port/device if any initialization must be done */
        if (bmp_comport() != NULL)
          bmp_break();
        result = bmp_connect(probe, (probe == netprobe) ? txtIPaddr : NULL);
        if (result) /* bmp_connect() also opens the (virtual) serial port/device */
          result = bmp_attach(2, opt_connect_srst, mcu_driver, sizearray(mcu_driver),
                              mcu_architecture, sizearray(mcu_architecture));
        else
          trace_status = TRACESTAT_NO_CONNECT;
        if (result) {
          /* overrule any default protocol setting, if the debug probe can be
             verified */
          probe_type = bmp_checkversionstring();
          if (probe_type == PROBE_ORG_BMP)
            opt_mode = MODE_MANCHESTER;
          else if (probe_type == PROBE_CTXLINK)
           opt_mode = MODE_ASYNC;
        }
        if (result && opt_init_target) {
          /* initialize the target (target-specific configuration, generic
             configuration and channels */
          unsigned long params[4];
          const DWARF_SYMBOLLIST *symbol;
          bmp_runscript("swo_device", mcu_driver, mcu_architecture, NULL);
          assert(opt_mode == MODE_MANCHESTER || opt_mode == MODE_ASYNC);
          symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_BPS", -1, -1);
          params[0] = opt_mode;
          params[1] = cpuclock / bitrate - 1;
          params[2] = bitrate;
          params[3] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
          bmp_runscript("swo_generic", mcu_driver, mcu_architecture, params);
          /* enable active channels in the target (disable inactive channels) */
          channelmask = 0;
          for (chan = 0; chan < NUM_CHANNELS; chan++)
            if (channel_getenabled(chan))
              channelmask |= (1 << chan);
          symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_TER", -1, -1);
          params[0] = channelmask;
          params[1] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
          bmp_runscript("swo_channels", mcu_driver, mcu_architecture, params);
        }
      } else if (bmp_isopen()) {
        /* no initialization is requested, if the serial port is open, close it
           (so that the gdbserver inside the BMP is available for debugging) */
        bmp_disconnect();
        result = 1; /* flag status = ok, to drop into the next "if" */
      }
      if (result) {
        if (opt_init_bmp)
          bmp_enabletrace((opt_mode == MODE_ASYNC) ? bitrate : 0, &trace_endpoint);
        /* trace_status() does nothing if initialization had already succeeded */
        if (probe == netprobe)
          trace_status = trace_init(BMP_PORT_TRACE, txtIPaddr);
        else
          trace_status = trace_init(trace_endpoint, NULL);
        bmp_restart();
      }
      trace_running = (trace_status == TRACESTAT_OK);
      switch (trace_status) {
      case TRACESTAT_OK:
        if (opt_init_target || opt_init_bmp) {
          assert(strlen(mcu_driver) > 0);
          sprintf(msg, "Connected [%s]", mcu_driver);
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
        if (probe == netprobe && opt_mode != MODE_ASYNC)
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
      error_flags = 0;
      if (strlen(txtTSDLfile) > 0)
        error_flags |= ERROR_NO_TSDL;
      if (strlen(txtTSDLfile)> 0 && access(txtTSDLfile, 0)== 0) {
        if (ctf_parse_init(txtTSDLfile) && ctf_parse_run()) {
          const CTF_STREAM *stream;
          int seqnr;
          /* stream names overrule configured channel names */
          for (seqnr = 0; (stream = stream_by_seqnr(seqnr)) != NULL; seqnr++)
            if (stream->name != NULL && strlen(stream->name) > 0)
              channel_setname(seqnr, stream->name);
          error_flags &= ~ERROR_NO_TSDL;
          tracelog_statusmsg(TRACESTATMSG_CTF, "CTF mode active", BMPSTAT_SUCCESS);
        } else {
          ctf_parse_cleanup();
        }
      }
      if (strlen(txtELFfile) > 0)
        error_flags |= ERROR_NO_ELF;
      if (strlen(txtELFfile) > 0 && access(txtELFfile, 0) == 0) {
        FILE *fp = fopen(txtELFfile, "rb");
        if (fp != NULL) {
          int address_size;
          dwarf_read(fp, &dwarf_linetable, &dwarf_symboltable, &dwarf_filetable, &address_size);
          fclose(fp);
          error_flags &= ~ERROR_NO_ELF;
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
      struct nk_rect bounds;
      int mouse_hover = 0;

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
          sprintf(msg, "SWO packet errors (%d), verify data size", trace_getpacketerrors());
          tracelog_statusmsg(TRACESTATMSG_BMP, msg, BMPERR_GENERAL);
        }
        tracestring_process(trace_running);
        nk_layout_row_dynamic(ctx, splitter_rows[0], 1);
        tracelog_widget(ctx, "tracelog", opt_fontsize, cur_match_line, filterlist, NK_WINDOW_BORDER);

        /* vertical splitter */
        nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
        bounds = nk_widget_bounds(ctx);
        if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds))
          mouse_hover |= CURSOR_UPDOWN;
        nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_REPEAT(3));
        if ((mouse_hover & CURSOR_UPDOWN) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
          insplitter = SPLITTER_VERTICAL; /* in vertical splitter */
        else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
          insplitter = SPLITTER_NONE;
        if (insplitter == SPLITTER_VERTICAL)
          splitter_ver = (splitter_rows[0] + ctx->input.mouse.delta.y) / (canvas_height - EXTRA_SPACE_VER);

        nk_layout_row_dynamic(ctx, splitter_rows[1], 1);
        click_time = timeline_widget(ctx, "timeline", opt_fontsize, NK_WINDOW_BORDER);
        cur_match_line = (click_time >= 0.0) ? tracestring_findtimestamp(click_time) : -1;

        nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
        nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 7, nk_ratio(7, 0.19, 0.08, 0.19, 0.08, 0.19, 0.08, 0.19));
        ptr = trace_running ? "Stop" : tracestring_isempty() ? "Start" : "Resume";
        if (nk_button_label(ctx, ptr) || nk_input_is_key_pressed(&ctx->input, NK_KEY_F5)) {
          trace_running = !trace_running;
          if (trace_running && trace_status != TRACESTAT_OK) {
            trace_status = trace_init(trace_endpoint, (probe == netprobe) ? txtIPaddr : NULL);
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
      if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds))
        mouse_hover |= CURSOR_LEFTRIGHT;
      nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_VERTICAL | NK_SYMBOL_REPEAT(3));
      if ((mouse_hover & CURSOR_LEFTRIGHT) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
        insplitter = SPLITTER_HORIZONTAL; /* in horizontal splitter */
      else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
        insplitter = SPLITTER_NONE;
      if (insplitter == SPLITTER_HORIZONTAL)
        splitter_hor = (splitter_columns[0] + ctx->input.mouse.delta.x) / (canvas_width - EXTRA_SPACE_HOR);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        #define LABEL_WIDTH (4.5 * opt_fontsize)
        #define VALUE_WIDTH (splitter_columns[2] - LABEL_WIDTH - 26)
        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", &tab_states[TAB_CONFIGURATION])) {
          int result;
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "Probe", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_layout_row_push(ctx, VALUE_WIDTH);
          bounds = nk_widget_bounds(ctx);
          probe = nk_combo(ctx, probelist, netprobe+1, probe, (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
          if (probe == netprobe) {
            int reconnect = 0;
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
            nk_layout_row_push(ctx, LABEL_WIDTH);
            nk_label(ctx, "IP Addr", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
            nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
            result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                           txtIPaddr, sizearray(txtIPaddr), nk_filter_ascii,
                                           "IP address of the ctxLink");
            if ((result & NK_EDIT_COMMITED) != 0 && bmp_is_ip_address(txtIPaddr))
              reconnect = 1;
            nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
            if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIPLE_DOT, NK_KEY_NONE, "Scan network for ctxLink probes.")) {
              #if defined WIN32 || defined _WIN32
                HCURSOR hcur = SetCursor(LoadCursor(NULL, IDC_WAIT));
              #endif
              unsigned long addr;
              int count = scan_network(&addr, 1);
              #if defined WIN32 || defined _WIN32
                SetCursor(hcur);
              #endif
              if (count == 1) {
                sprintf(txtIPaddr, "%lu.%lu.%lu.%lu",
                       addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >> 24) & 0xff);
                reconnect = 1;
              } else {
                strlcpy(txtIPaddr, "none found", sizearray(txtIPaddr));
              }
            }
            if (reconnect) {
              bmp_disconnect();
              reinitialize = 1;
            }
          }
          if (probe_type == PROBE_UNKNOWN) {
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
            nk_layout_row_push(ctx, LABEL_WIDTH);
            nk_label(ctx, "Mode", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
            nk_layout_row_push(ctx, VALUE_WIDTH);
            result = opt_mode - MODE_MANCHESTER;
            result = nk_combo(ctx, mode_strings, NK_LEN(mode_strings), result, opt_fontsize, nk_vec2(VALUE_WIDTH,4.5*opt_fontsize));
            if (opt_mode != result + MODE_MANCHESTER) {
              /* opt_mode is 1-based, the result of nk_combo() is 0-based, which is
                 we MODE_MANCHESTER is added (MODE_MANCHESTER == 1) */
              opt_mode = result + MODE_MANCHESTER;
              reinitialize = 1;
            }
            nk_layout_row_end(ctx);
          }
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          if (checkbox_tooltip(ctx, "Configure Target", &opt_init_target, "Configure the target microcontroller for SWO"))
            reinitialize = 1;
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          if (checkbox_tooltip(ctx, "Configure Debug Probe", &opt_init_bmp, "Activate SWO trace capture in the Black Magic Probe"))
            reinitialize = 1;
          if (opt_init_target || opt_init_bmp) {
            nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
            if (checkbox_tooltip(ctx, "Reset target during connect", &opt_connect_srst, "Keep the target in reset state while scanning and attaching"))
              reinitialize = 1;
          }
          if (opt_init_target) {
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
            nk_layout_row_push(ctx, LABEL_WIDTH);
            nk_label(ctx, "CPU clock", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
            nk_layout_row_push(ctx, VALUE_WIDTH);
            result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                      cpuclock_str, sizearray(cpuclock_str), nk_filter_decimal,
                                      "CPU clock of the target microcontroller");
            if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(cpuclock_str, NULL, 10) != cpuclock))
              reinitialize = 1;
            nk_layout_row_end(ctx);
          }
          if (opt_init_target || (opt_init_bmp && opt_mode == MODE_ASYNC)) {
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
            nk_layout_row_push(ctx, LABEL_WIDTH);
            nk_label(ctx, "Bit rate", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
            nk_layout_row_push(ctx, VALUE_WIDTH);
            result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                           bitrate_str, sizearray(bitrate_str), nk_filter_decimal,
                                           "SWO bit rate (data rate)");
            if ((result & NK_EDIT_COMMITED) != 0 || ((result & NK_EDIT_DEACTIVATED) && strtoul(bitrate_str, NULL, 10) != bitrate))
              reinitialize = 1;
            nk_layout_row_end(ctx);
          }
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "Data size", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_layout_row_push(ctx, VALUE_WIDTH);
          bounds = nk_widget_bounds(ctx);
          result = opt_datasize;
          opt_datasize = nk_combo(ctx, datasize_strings, NK_LEN(datasize_strings), opt_datasize, opt_fontsize, nk_vec2(VALUE_WIDTH,5.5*opt_fontsize));
          if (opt_datasize != result) {
            trace_setdatasize((opt_datasize == 3) ? 4 : (short)opt_datasize);
            tracestring_clear();
            if (trace_status == TRACESTAT_OK)
              tracelog_statusmsg(TRACESTATMSG_BMP, "Listening ...", BMPSTAT_SUCCESS);
          }
          tooltip(ctx, bounds, "Payload size of an SWO packet (in bits); auto for autodetect");
          nk_layout_row_end(ctx);
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "TSDL file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
          if (error_flags & ERROR_NO_TSDL)
            nk_style_push_color(ctx,&ctx->style.edit.text_normal, nk_rgb(255, 80, 100));
          result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                    txtTSDLfile, sizearray(txtTSDLfile), nk_filter_ascii,
                                    "Metadata file for Common Trace Format (CTF)");
          if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
            reload_format = 1;
          if (error_flags & ERROR_NO_TSDL)
            nk_style_pop_color(ctx);
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
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "ELF file", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_layout_row_push(ctx, VALUE_WIDTH - BROWSEBTN_WIDTH - 5);
          if (error_flags & ERROR_NO_ELF)
            nk_style_push_color(ctx,&ctx->style.edit.text_normal, nk_rgb(255, 80, 100));
          result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                         txtELFfile, sizearray(txtELFfile), nk_filter_ascii,
                                         "ELF file for symbol lookup");
          if (result & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
            reload_format = 1;
          if (error_flags & ERROR_NO_ELF)
            nk_style_pop_color(ctx);
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
          nk_tree_state_pop(ctx);
        }

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Filters", &tab_states[TAB_FILTERS])) {
          int txtwidth, result;
          char filter[FILTER_MAXSTRING];
          assert(filterlistsize == 0 || filterlist != NULL);
          assert(filterlistsize == 0 || filtercount < filterlistsize);
          assert(filterlistsize == 0 || (filterlist[filtercount].expr == NULL && !filterlist[filtercount].enabled));
          bounds = nk_widget_bounds(ctx);
          txtwidth = bounds.w - 2 * BROWSEBTN_WIDTH - (2 * 5);
          for (idx = 0; idx < filtercount; idx++) {
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
            nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
            checkbox_tooltip(ctx, "", &filterlist[idx].enabled, "Enable/disable this filter");
            nk_layout_row_push(ctx, txtwidth);
            assert(filterlist[idx].expr != NULL);
            strcpy(filter, filterlist[idx].expr);
            result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                      filter, sizearray(filter), nk_filter_ascii,
                                      "Text to filter on (case-sensitive)");
            if (strcmp(filter, filterlist[idx].expr) != 0) {
              strcpy(filterlist[idx].expr, filter);
              filterlist[idx].enabled = (strlen(filterlist[idx].expr) > 0);
            }
            nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
            if (button_symbol_tooltip(ctx, NK_SYMBOL_X, NK_KEY_NONE, "Remove this filter")
                || ((result & NK_EDIT_COMMITED) && strlen(filter) == 0))
            {
              /* remove row */
              assert(filterlist[idx].expr != NULL);
              free(filterlist[idx].expr);
              filtercount -= 1;
              if (idx < filtercount)
                memmove(&filterlist[idx], &filterlist[idx+1], (filtercount - idx) * sizeof(TRACEFILTER));
              filterlist[filtercount].expr = NULL;
              filterlist[filtercount].enabled = 0;
            }
          }
          txtwidth = bounds.w - 1 * BROWSEBTN_WIDTH - (1 * 5);
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
          nk_layout_row_push(ctx, txtwidth);
          result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER|NK_EDIT_CLIPBOARD,
                                    newfiltertext, sizearray(newfiltertext), nk_filter_ascii,
                                    "New filter (case-sensitive)");
          nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
          if ((button_symbol_tooltip(ctx, NK_SYMBOL_PLUS, NK_KEY_NONE, "Add filter")
               || (result & NK_EDIT_COMMITED))
              && strlen(newfiltertext) > 0)
          {
            /* add row */
            if (filterlistsize > 0) {
              /* make sure there is an extra entry at the top of the array, for
                 a NULL terminator */
              assert(filtercount < filterlistsize);
              if (filtercount + 1 == filterlistsize) {
                int newsize = 2 * filterlistsize;
                TRACEFILTER *newlist = malloc(newsize * sizeof(TRACEFILTER));
                if (newlist != NULL) {
                  assert(filterlist != NULL);
                  memset(newlist, 0, newsize * sizeof(TRACEFILTER));  /* set all new entries to NULL */
                  memcpy(newlist, filterlist, filterlistsize * sizeof(TRACEFILTER));
                  free(filterlist);
                  filterlist = newlist;
                  filterlistsize = newsize;
                }
              }
            }
            if (filtercount + 1 < filterlistsize) {
              filterlist[filtercount].expr = malloc(sizearray(newfiltertext) * sizeof(char));
              if (filterlist[filtercount].expr != NULL) {
                strcpy(filterlist[filtercount].expr, newfiltertext);
                filterlist[filtercount].enabled = 1;
                filtercount += 1;
                newfiltertext[0] = '\0';
              }
            }
          }
          nk_tree_state_pop(ctx);
        }

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Channels", &tab_states[TAB_CHANNELS])) {
          float labelwidth = tracelog_labelwidth(opt_fontsize) + 10;
          struct nk_style_button stbtn = ctx->style.button;
          stbtn.border = 0;
          stbtn.rounding = 0;
          stbtn.padding.x = stbtn.padding.y = 0;
          for (chan = 0; chan < NUM_CHANNELS; chan++) {
            char label[32];
            int enabled;
            struct nk_color clrtxt, clrbk;
            nk_layout_row_begin(ctx, NK_STATIC, opt_fontsize, 2);
            nk_layout_row_push(ctx, 3 * opt_fontsize);
            sprintf(label, "%2d", chan);
            enabled = channel_getenabled(chan);
            if (checkbox_tooltip(ctx, label, &enabled, "Enable/disable this channel")) {
              /* enable/disable channel in the target */
              channel_setenabled(chan, enabled);
              if (opt_init_target) {
                if (enabled)
                  channelmask |= (1 << chan);
                else
                  channelmask &= ~(1 << chan);
                if (trace_status != TRACESTAT_NO_CONNECT) {
                  const DWARF_SYMBOLLIST *symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_TER", -1, -1);
                  unsigned long params[2];
                  params[0] = channelmask;
                  params[1] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
                  bmp_runscript("swo_channels", mcu_driver, mcu_architecture, params);
                }
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
              if (cur_chan_edit == -1) {
                cur_chan_edit = chan;
                channel_getname(chan, valstr, sizearray(valstr));
              }
              nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.35, 0.65));
              nk_label(ctx, "name", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
              nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_CLIPBOARD,
                                             valstr, sizearray(valstr), nk_filter_ascii);
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
        rc.x = canvas_width - 18 * opt_fontsize;
        rc.y = canvas_height - 6.5 * ROW_HEIGHT;
        rc.w = 200;
        rc.h = 3.6 * ROW_HEIGHT;
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Search", NK_WINDOW_NO_SCROLLBAR, rc)) {
          nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 2, nk_ratio(2, 0.2, 0.8));
          nk_label(ctx, "Text", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_edit_focus(ctx, 0);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_CLIPBOARD,
                                         findtext, sizearray(findtext), nk_filter_ascii);
          nk_layout_row(ctx, NK_DYNAMIC, opt_fontsize, 2, nk_ratio(2, 0.2, 0.8));
          nk_spacing(ctx, 1);
          if (find_popup == 2)
            nk_label_colored(ctx, "Text not found", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, nk_rgb(255, 80, 100));
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

      pointer_setstyle(mouse_hover);
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
  ini_putl("Filters", "count", filtercount, txtConfigFile);
  for (idx = 0; idx < filtercount; idx++) {
    char key[40], expr[FILTER_MAXSTRING+10];
    assert(filterlist != NULL && filterlist[idx].expr != NULL);
    sprintf(key, "filter%d", idx + 1);
    sprintf(expr, "%d,%s", filterlist[idx].enabled, filterlist[idx].expr);
    ini_puts("Filters", key, expr, txtConfigFile);
    free(filterlist[idx].expr);
  }
  if (filterlist != NULL)
    free(filterlist);
  sprintf(valstr, "%.2f %.2f", splitter_hor, splitter_ver);
  ini_puts("Settings", "splitter", valstr, txtConfigFile);
  for (idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    sprintf(key, "view%d", idx);
    sprintf(valstr, "%d", tab_states[idx]);
    ini_puts("Settings", key, valstr, txtConfigFile);
  }
  ini_putl("Settings", "fontsize", opt_fontsize, txtConfigFile);
  ini_puts("Settings", "fontstd", opt_fontstd, txtConfigFile);
  ini_puts("Settings", "fontmono", opt_fontmono, txtConfigFile);
  ini_putl("Settings", "mode", opt_mode, txtConfigFile);
  ini_putl("Settings", "init-target", opt_init_target, txtConfigFile);
  ini_putl("Settings", "init-bmp", opt_init_bmp, txtConfigFile);
  ini_putl("Settings", "connect-srst", opt_connect_srst, txtConfigFile);
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
  if (bmp_is_ip_address(txtIPaddr))
    ini_puts("Settings", "ip-address", txtIPaddr, txtConfigFile);
  ini_putl("Settings", "probe", (probe == netprobe) ? 99 : probe, txtConfigFile);
  if (probelist != NULL) {
    for (idx = 0; idx < netprobe + 1; idx++)
      free((void*)probelist[idx]);
    free((void*)probelist);
  }

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

