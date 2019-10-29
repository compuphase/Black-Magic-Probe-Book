/*
 * GDB front-end with specific support for the Black Magic Probe.
 * This utility is built with Nuklear for a cross-platform GUI.
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
  #include <malloc.h>
  #if defined __MINGW32__ || defined __MINGW64__
    #include <sys/stat.h>
    #include "strlcpy.h"
  #elif defined _MSC_VER
    #include "strlcpy.h"
    #include <sys/stat.h>
    #define stat _stat
    #define access(p,m)       _access((p),(m))
    #define memicmp(p1,p2,c)  _memicmp((p1),(p2),(c))
    #define mkdir(p)          _mkdir(p)
    #define strdup(s)         _strdup(s)
    #define stricmp(s1,s2)    _stricmp((s1),(s2))
    #define strnicmp(s1,s2,c) _strnicmp((s1),(s2),(c))
  #endif
#elif defined __linux__
  #include <alloca.h>
  #include <poll.h>
  #include <signal.h>
  #include <unistd.h>
  #include <bsd/string.h>
  #include <sys/stat.h>
  #include <sys/time.h>
  #include <sys/types.h>
  #include <sys/wait.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bmscan.h"
#include "bmp-script.h"
#include "guidriver.h"
#include "noc_file_dialog.h"
#include "minIni.h"
#include "specialfolder.h"

#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"

#include "res/btn_folder.h"
#if defined __linux__ || defined __unix__
  #include "res/icon_debug_64.h"
#endif

#ifndef NK_ASSERT
  #define NK_ASSERT(expr) assert(expr)
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  #define stricmp(s1,s2)    strcasecmp((s1),(s2))
  #define strnicmp(s1,s2,n) strncasecmp((s1),(s2),(n))
  #define min(a,b)          ( ((a) < (b)) ? (a) : (b) )
#endif
#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if defined _WIN32
  #define DIRSEP_CHAR '\\'
#else
  #define DIRSEP_CHAR '/'
#endif

static char *translate_path(char *path, int todos);


#define STRFLG_INPUT    0x0001  /* stdin echo */
#define STRFLG_ERROR    0x0002  /* stderr */
#define STRFLG_RESULT   0x0004  /* '^' */
#define STRFLG_EXEC     0x0008  /* '*' */
#define STRFLG_STATUS   0x0010  /* '+' */
#define STRFLG_NOTICE   0x0020  /* '=' */
#define STRFLG_LOG      0x0040  /* '&' */
#define STRFLG_TARGET   0x0080  /* '@' */
#define STRFLG_MI_INPUT 0x0100  /* '-' */
#define STRFLG_STARTUP  0x4000
#define STRFLG_HANDLED  0x8000

typedef struct tagSTRINGLIST {
  struct tagSTRINGLIST *next;
  char *text;
  unsigned short flags;
} STRINGLIST;


/** stringlist_add() adds a string to the tail of the list. */
static STRINGLIST *stringlist_add(STRINGLIST *root, const char *text, int flags)
{
  STRINGLIST *tail, *item;

  item = (STRINGLIST*)malloc(sizeof(STRINGLIST));
  if (item == NULL)
    return NULL;
  memset(item, 0, sizeof(STRINGLIST));
  item->text = strdup(text);
  if (item->text == NULL) {
    free((void*)item);
    return NULL;
  }
  item->flags = (unsigned short)flags;

  assert(root != NULL);
  for (tail = root; tail->next != NULL; tail = tail->next)
    /* nothing */;
  tail->next = item;
  return item;
}

/** stringlist_add_head() adds a string to the head of the list. */
static STRINGLIST *stringlist_add_head(STRINGLIST *root, const char *text, int flags)
{
  STRINGLIST *item = (STRINGLIST*)malloc(sizeof(STRINGLIST));
  if (item == NULL)
    return NULL;
  memset(item, 0, sizeof(STRINGLIST));
  item->text = strdup(text);
  if (item->text == NULL) {
    free((void*)item);
    return NULL;
  }
  item->flags = (unsigned short)flags;

  assert(root != NULL);
  item->next = root->next;
  root->next = item;
  return item;
}

static void stringlist_clear(STRINGLIST *root)
{
  assert(root != NULL);
  while (root->next != NULL) {
    STRINGLIST *item = root->next;
    root->next = item->next;
    assert(item->text != NULL);
    free((void*)item->text);
    free((void*)item);
  }
}

/** stringlist_getlast() returns the last string that has the "include" flags
 *  all set and that has none of the "exclude" flags set. So setting include to
 *  STRFLG_RESULT and exclude to STRFLG_HANDLED returns the last result message
 *  that is not hidden, and include and exclude to 0 simply returns the very
 *  last message.
 */
static STRINGLIST *stringlist_getlast(STRINGLIST *root, int include, int exclude)
{
  STRINGLIST *item, *last;
  last = NULL;
  for (item = root->next; item != NULL; item = item->next)
    if ((item->flags & include) == include && (item->flags & exclude) == 0)
      last = item;
  assert(last == NULL || last->text != NULL);
  return last;
}


static const char *skip_string(const char *buffer)
{
  if (*buffer == '"') {
    buffer++;
    while (*buffer != '"' && *buffer != '\0') {
      if (*buffer == '\\' && *(buffer + 1) != '\0')
        buffer++;
      buffer++;
    }
    if (*buffer == '"')
      buffer++;
  } else {
    while (*buffer > ' ')
      buffer++;
  }
  return buffer;
}

static void format_string(char *buffer)
{
  if (*buffer == '"') {
    char *tgt = buffer;
    char *src = buffer + 1;
    while (*src != '"' && *src != '\0') {
      if (*src == '\\') {
        src++;
        switch (*src) {
        case 'n':
          *tgt = '\n';
          break;
        case 'r':
          *tgt = '\r';
          break;
        case 't':
          *tgt = '\t';
          break;
        case '\'':
          *tgt = '\'';
          break;
        case '"':
          *tgt = '"';
          break;
        case '\\':
          *tgt = '\\';
          break;
        default:
          assert(0);
          *tgt = '?';
        }
      } else {
        *tgt = *src;
      }
      tgt++;
      src++;
    }
    *tgt = '\0';
  }
}

char *strdup_len(const char *source, size_t length)
{
  char *tgt = (char*)malloc((length + 1) * sizeof(char));
  if (tgt != NULL) {
    strncpy(tgt, source, length);
    tgt[length] = '\0';
  }
  return tgt;
}

static STRINGLIST consolestring_root = { NULL, NULL, 0 };
static STRINGLIST semihosting_root = { NULL, NULL, 0 };
static int console_hiddenflags = 0; /* when a message contains a flag in this set, it is hidden in the console */

static const char *gdbmi_leader(char *buffer, int *flags)
{
  assert(buffer != NULL && flags != NULL);
  *flags = 0;
  switch (*buffer) {
  case '^':
    *flags |= STRFLG_RESULT;
    buffer += 1;
    break;
  case '*':
    *flags |= STRFLG_EXEC;
    buffer += 1;
    break;
  case '+':
    *flags |= STRFLG_STATUS;
    buffer += 1;
    break;
  case '=':
    *flags |= STRFLG_NOTICE;
    buffer += 1;
    break;
  case '~': /* normal console output */
    buffer += 1;
    format_string(buffer);
    break;
  case '-': /* MI command input */
    *flags |= STRFLG_MI_INPUT;
    format_string(buffer);
    break;
  case '&': /* logged commands & replies (plain commands in MI mode) */
    *flags |= STRFLG_LOG;
    buffer += 1;
    format_string(buffer);
    break;
  case '@': /* target output in MI mode */
    *flags |= STRFLG_TARGET;
    buffer += 1;
    format_string(buffer);
    break;
  }
  return buffer;
}

static const char *gdbmi_isresult(void)
{
  STRINGLIST *item = stringlist_getlast(&consolestring_root, STRFLG_RESULT, STRFLG_HANDLED);
  assert(item == NULL || item->text != NULL);
  return (item != NULL) ? item->text : NULL;
}

static void gdbmi_sethandled(int all)
{
  STRINGLIST *item;
  do {
    item = stringlist_getlast(&consolestring_root, STRFLG_RESULT, STRFLG_HANDLED);
    if (item != NULL)
      item->flags |= STRFLG_HANDLED;
  } while (item != NULL && all);
  assert(stringlist_getlast(&consolestring_root, STRFLG_RESULT, STRFLG_HANDLED) == NULL);
}

static const char *skipwhite(const char *text)
{
  assert(text != NULL);
  while (*text != '\0' && *text <= ' ')
    text++;
  return text;
}

static int is_gdb_prompt(const char *text)
{
  text = skipwhite(text);
  return strncmp(text, "(gdb)", 5) == 0 && strlen(text) <= 6;
}


static char *console_buffer = NULL;
static size_t console_bufsize = 0;

static void console_growbuffer(size_t extra)
{
  if (console_buffer == NULL) {
    console_bufsize = 256;
    console_buffer = (char*)malloc(console_bufsize * sizeof(char));
    console_buffer[0] = '\0';
  } else if (strlen(console_buffer) + extra >= console_bufsize) {
    console_bufsize *= 2;
    while (strlen(console_buffer) + extra >= console_bufsize)
      console_bufsize *= 2;
    console_buffer = (char*)realloc(console_buffer, console_bufsize * sizeof(char));
  }
  if (console_buffer == NULL) {
    fprintf(stderr, "Memory allocation error.\n");
    exit(1);
  }
}

static void console_clear(void)
{
  if (console_buffer != NULL) {
    free((void*)console_buffer);
    console_buffer = NULL;
  }
  console_bufsize = 0;
}

static int console_add(const char *text, int flags)
{
  static int curflags = -1;
  const char *head, *ptr;
  int addstring = 0;
  int foundprompt = 0;

  console_growbuffer(strlen(text));

  if (curflags != flags && console_buffer[0] != '\0') {
    int xtraflags;
    char *tok;
    assert(curflags >= 0);
    ptr = gdbmi_leader(console_buffer, &xtraflags);
    /* after gdbmi_leader(), there may again be \n in the resulting string */
    for (tok = strtok((char*)ptr, "\n"); tok != NULL; tok = strtok(NULL, "\n")) {
      stringlist_add(&consolestring_root, tok, curflags | xtraflags);
      if ((xtraflags & STRFLG_TARGET) != 0 && (curflags & STRFLG_STARTUP) == 0)
        stringlist_add(&semihosting_root, tok, curflags | xtraflags);
    }
    console_buffer[0] = '\0';
  }
  curflags = flags;

  head = text;
  while (*head != '\0') {
    size_t len, pos;
    const char *tail = strpbrk(head, "\r\n");
    if (tail == NULL) {
      tail = head + strlen(head);
      addstring = 0;
    } else {
      addstring = 1;
    }
    pos = strlen(console_buffer);
    len = tail - head;
    assert(pos + len < console_bufsize);
    memcpy(console_buffer + pos, head, len);
    pos += len;
    console_buffer[pos] = '\0';
    head += len;
    if (*head == '\r')
      head++;
    if (*head == '\n')
      head++;
    if (addstring) {
      int xtraflags, prompt;
      ptr = gdbmi_leader(console_buffer, &xtraflags);
      prompt = is_gdb_prompt(ptr) && (xtraflags & STRFLG_TARGET) == 0;
      if (prompt) {
        foundprompt = 1;  /* don't add prompt to the output console, but mark that we've seen it */
      } else {
        /* after gdbmi_leader(), there may again be \n in the resulting string */
        char *tok;
        for (tok = strtok((char*)ptr, "\n"); tok != NULL; tok = strtok(NULL, "\n")) {
          stringlist_add(&consolestring_root, tok, flags | xtraflags);
          if ((xtraflags & STRFLG_TARGET) != 0 && (curflags & STRFLG_STARTUP) == 0)
            stringlist_add(&semihosting_root, tok, curflags | xtraflags);
        }
      }
      console_buffer[0]= '\0';
    }
  }
  return foundprompt;
}

static void console_input(const char *text)
{
  gdbmi_sethandled(0); /* clear result on new input */
  assert(text != NULL);
  console_add(text, STRFLG_INPUT);
}


static char **sources_namelist = NULL;
static char **sources_pathlist = NULL;
static unsigned sources_size = 0;
static unsigned sources_count = 0;

static void sources_add(const char *filename, const char *filepath)
{
  assert((sources_namelist == NULL && sources_pathlist == NULL && sources_size == 0)
         || (sources_namelist != NULL && sources_pathlist != NULL));
  assert(filename != NULL);

  /* check whether the source already exists */
  if (sources_namelist != NULL) {
    unsigned idx;
    for (idx = 0; idx < sources_count; idx++) {
      if (strcmp(sources_namelist[idx], filename) == 0) {
        const char *p1 = (sources_pathlist[idx] != NULL) ? sources_pathlist[idx] : "";
        const char *p2 = (filepath != NULL) ? filepath : "";
        if (strcmp(p1, p2) == 0)
          return; /* both base name and full path are the same -> do not add duplicate */
      }
    }
  }

  /* add the source, first grow the lists if needed */
  if (sources_size == 0) {
    assert(sources_count == 0);
    sources_size = 16;
    sources_namelist = (char**)malloc(sources_size * sizeof(char*));
    sources_pathlist = (char**)malloc(sources_size * sizeof(char*));
  } else if (sources_count >= sources_size) {
    sources_size *= 2;
    sources_namelist = (char**)realloc(sources_namelist, sources_size * sizeof(char*));
    sources_pathlist = (char**)realloc(sources_pathlist, sources_size * sizeof(char*));
  }
  if (sources_namelist == NULL || sources_pathlist == NULL) {
    fprintf(stderr, "Memory allocation error.\n");
    exit(1);
  }
  assert(filename != NULL && strlen(filename) > 0);
  sources_namelist[sources_count] = strdup(filename);
  if (filepath != NULL && strlen(filepath) > 0)
    sources_pathlist[sources_count] = strdup(filepath);
  else
    sources_pathlist[sources_count] = NULL;
  sources_count += 1;
}

/** sources_clear() removes all files from the sources lists, and optionally
 *  removes these lists too.
 *  \param freelist   0 -> clear only files; 1 -> free lists fully.
 */
static void sources_clear(int freelists)
{
  unsigned idx;

  if (sources_size == 0) {
    assert(sources_count == 0);
    assert(sources_namelist == NULL && sources_pathlist == NULL);
    return;
  }

  for (idx = 0; idx < sources_count; idx++) {
    assert(sources_namelist[idx] != NULL);
    free((void*)sources_namelist[idx]);
    sources_namelist[idx] = NULL;
    if (sources_pathlist[idx] != NULL) {
      free((void*)sources_pathlist[idx]);
      sources_pathlist[idx] = NULL;
    }
  }
  sources_count = 0;

  if (freelists) {
    assert(sources_namelist != NULL && sources_pathlist != NULL);
    free((void*)sources_namelist);
    free((void*)sources_pathlist);
    sources_namelist = NULL;
    sources_pathlist = NULL;
    sources_size = 0;
  }
}

static void sources_parse(const char *gdbresult)
{
  const char *head, *basename;

  head = gdbresult;
  if (*head == '^')
    head++;
  if (strncmp(head, "done", 4) == 0)
    head += 4;
  if (*head == ',')
    head++;
  if (strncmp(head, "files=", 6) != 0)
    return;

  assert(head[6] == '[');
  head += 7;  /* skip [ too */
  for ( ;; ) {
    char name[256] = "", path[256] = "";
    const char *sep = head;
    int len;
    assert(*head == '{');
    head++;
    if (strncmp(head, "file=", 5) == 0) {
      head += 5;
      sep = skip_string(head);
      while (*sep != ',' && *sep != '}' && *sep != '\0')
        sep++;
      assert(*sep != '\0');
      len = (int)(sep - head);
      if (len >= sizearray(name))
        len = sizearray(name) - 1;
      memcpy(name, head, len);
      name[len] = '\0';
      if (name[0] == '"' && name[len - 1] == '"')
        format_string(name);
    }
    if (*sep == ',' && strncmp(sep + 1, "fullname=", 9) == 0) {
      head = sep + 9 + 1;
      sep = skip_string(head);
      while (*sep != '}' && *sep != '\0')
        sep++;
      assert(*sep != '\0');
      len = (int)(sep - head);
      if (len >= sizearray(path))
        len = sizearray(path) - 1;
      if (*head == '"' && head[len - 1] == '"') {
        head++;
        len -= 2;
      }
      memcpy(path, head, len);
      path[len] = '\0';
      if (path[0] == '"' && path[len - 1] == '"')
        format_string(path);
    }
    if (strlen(path) == 0)
      strcpy(path, name);
    for (basename = name + strlen(name) - 1; basename > name && *(basename -1 ) != '/' && *(basename -1 ) != '\\'; basename--)
      /* nothing */;
    sources_add(basename, path);
    head = sep + 1;
    if (*head == ']')
      break;
    assert(*head == ',');
    head++;
  }
}

static int check_sources_tstamps(const char *elffile)
{
  struct stat fstat;
  int result = 1; /* assume all is ok */

  if (stat(elffile, &fstat) >= 0) {
    time_t tstamp_elf = fstat.st_mtime;
    unsigned idx;
    for (idx = 0; idx < sources_count; idx++) {
      const char *fname = (sources_pathlist[idx] != NULL) ? sources_pathlist[idx] : sources_namelist[idx];
      if (stat(fname, &fstat) >= 0 && fstat.st_mtime > tstamp_elf)
        result = 0;
    }
  }
  return result;
}

static int source_lookup(const char *filename)
{
  unsigned idx;
  const char *base;

  if (sources_namelist == NULL)
    return -1;

  if ((base = strrchr(filename, '/')) != NULL)
    filename = base + 1;
  #if defined _WIN32
    if ((base = strrchr(filename, '\\')) != NULL)
      filename = base + 1;
  #endif

  for (idx = 0; idx < sources_count; idx++)
    if (strcmp(filename, sources_namelist[idx]) == 0)
      return idx;

  return -1;
}


static STRINGLIST sourcefile_root = { NULL, NULL, 0 };
static int sourcefile_index = -1;

static void source_clear(void)
{
  stringlist_clear(&sourcefile_root);
  sourcefile_index = -1;
}

static int source_load(int srcindex)
{
  FILE *fp;
  char line[256];

  if (srcindex == sourcefile_index)
    return 0;           /* file does not change */

  source_clear();
  assert(srcindex >= 0);
  if (srcindex >= (int)sources_count)
    return 0;           /* new file index is invalid */

  fp = fopen(sources_pathlist[srcindex], "rt");
  if (fp == NULL)
    return 0;           /* source file could not be opened */
  while (fgets(line, sizearray(line), fp) != NULL) {
    char *ptr = strchr(line, '\n');
    if (ptr != NULL)
      *ptr = '\0';
    stringlist_add(&sourcefile_root, line, 0);
  }
  fclose(fp);
  return 1;
}

static int source_linecount(void)
{
  int count = 0;
  STRINGLIST *item;
  for (item = sourcefile_root.next; item != NULL; item = item->next)
    count++;
  return count;
}

typedef struct tagBREAKPOINT {
  struct tagBREAKPOINT *next;
  short number; /* sequential number, as assigned by GDB */
  short type;   /* 0 = breakpoint, 1 = watchpoint */
  short keep;
  short enabled;
  unsigned long address;
  int linenr;
  short filenr;
  unsigned short flags;
  char *name;   /* function or variable name */
  int hitcount;
} BREAKPOINT;
#define BKPTFLG_FUNCTION  0x0001

static BREAKPOINT breakpoint_root = { NULL };

static const char *fieldfind(const char *line, const char *field)
{
  const char *ptr;
  int len;

  assert(line != NULL);
  assert(field != NULL);
  len = strlen(field);
  ptr = line;
  while (*ptr != '\0') {
    if (*ptr == '"')
      ptr = skip_string(ptr);
    else if (strncmp(ptr, field, len) == 0)
      return ptr;
    else
      ptr++;
  }
  return NULL;
}

static const char *fieldvalue(const char *field, size_t *len)
{
  const char *ptr;

  ptr = strchr(field, '=');
  if (ptr == NULL)
    return NULL;
  ptr = skipwhite(ptr + 1);
  if (*ptr != '"')
    return NULL;
  if (len != NULL) {
    const char *tail = skip_string(ptr);
    *len = tail - (ptr + 1) - 1;
  }
  return ptr + 1;
}

static void breakpoint_clear(void)
{
  while (breakpoint_root.next != NULL) {
    BREAKPOINT *bp = breakpoint_root.next;
    breakpoint_root.next = bp->next;
    if (bp->name != NULL)
      free((void*)bp->name);
    free((void*)bp);
  }
}

static int breakpoint_parse(const char *gdbresult)
{
  const char *start;
  int count;

  /* first check whether a breakpoint table follows */
  if ((start = strchr(gdbresult, '{')) == NULL)
    return 0;
  start = skipwhite(start + 1);
  if (strncmp(start, "nr_rows", 7) != 0)
    return 0;
  if ((start = fieldvalue(start, NULL)) == NULL)
    return 0;

  /* at this point we may assume this is a valid breakpoint table */
  breakpoint_clear();
  count = strtol(start, NULL, 10);
  if (count == 0)
    return 1; /* no breakpoints, done */
  start = strstr(start, "body");
  start = skipwhite(start + 4);
  assert(*start == '=');
  start = skipwhite(start + 1);
  assert(*start == '[');
  start = skipwhite(start + 1);
  while (*start != ']') {
    const char *tail;
    char *line;
    size_t len;
    assert(strncmp(start, "bkpt", 4) == 0);
    start = skipwhite(start + 4);
    assert(*start == '=');
    start = skipwhite(start + 1);
    assert(*start == '{');
    start = skipwhite(start + 1);
    tail = strchr(start, '}');
    assert(tail != NULL);
    len = tail - start;
    if ((line = malloc((len + 1) * sizeof(char))) != NULL) {
      BREAKPOINT *bp;
      strncpy(line, start, len);
      line[len] = '\0';
      if ((bp = (BREAKPOINT*)malloc(sizeof(BREAKPOINT))) != NULL) {
        BREAKPOINT *bptail;
        memset(bp, 0, sizeof(BREAKPOINT));
        if ((start=fieldfind(line, "number")) != NULL) {
          start = fieldvalue(start, NULL);
          assert(start != NULL);
          bp->number = (short)strtol(start, NULL, 10);
        }
        if ((start=fieldfind(line, "type")) != NULL) {
          start = fieldvalue(start, NULL);
          assert(start != NULL);
          bp->type = (strncmp(start, "breakpoint", 10) == 0) ? 0 : 1;
        }
        if ((start=fieldfind(line, "disp")) != NULL) {
          start = fieldvalue(start, NULL);
          assert(start != NULL);
          bp->keep = (strncmp(start, "keep", 4) == 0);
        }
        if ((start=fieldfind(line, "enabled")) != NULL) {
          start = fieldvalue(start, NULL);
          assert(start != NULL);
          bp->enabled = (*start == 'y');
        }
        if ((start=fieldfind(line, "addr")) != NULL) {
          start = fieldvalue(start, NULL);
          assert(start != NULL);
          bp->address = strtoul(start, NULL, 0);
        }
        if ((start=fieldfind(line, "file")) != NULL) {
          char filename[256];
          start = fieldvalue(start, &len);
          assert(start != NULL);
          if (len >= sizearray(filename))
            len = sizearray(filename) - 1;
          strncpy(filename, start, len);
          filename[len] = '\0';
          bp->filenr = (short)source_lookup(filename);
        }
        if ((start=fieldfind(line, "line")) != NULL) {
          start = fieldvalue(start, NULL);
          assert(start != NULL);
          bp->linenr = strtol(start, NULL, 10);
        }
        if ((start=fieldfind(line, "func")) != NULL) {
          char funcname[256];
          start = fieldvalue(start, &len);
          assert(start != NULL);
          if (len >= sizearray(funcname))
            len = sizearray(funcname) - 1;
          strncpy(funcname, start, len);
          funcname[len] = '\0';
          bp->name = strdup(funcname);
          if ((start=fieldfind(line, "original-location")) != NULL) {
            start = fieldvalue(start, &len);
            assert(start != NULL);
            if (len >= sizearray(funcname))
              len = sizearray(funcname) - 1;
            strncpy(funcname, start, len);
            funcname[len] = '\0';
            if (strcmp(bp->name, funcname) == 0)
              bp->flags |= BKPTFLG_FUNCTION;
          }
        }
        if ((start=fieldfind(line, "times")) != NULL) {
          start = fieldvalue(start, NULL);
          assert(start != NULL);
          bp->hitcount = strtol(start, NULL, 10);
        }
        /* add to tail of the breakpoint list */
        for (bptail = &breakpoint_root; bptail->next != NULL; bptail = bptail->next)
          /* nothing */;
        bptail->next = bp;
      }
      free((void*)line);
    }
    start = skipwhite(tail + 1);
    if (*start == ',')
      start = skipwhite(start + 1);
  }
  return 1;
}

static BREAKPOINT *breakpoint_lookup(int filenr, int linenr)
{
  BREAKPOINT *bp;
  for (bp = breakpoint_root.next; bp != NULL; bp = bp->next)
    if (bp->filenr == filenr && bp->linenr == linenr)
      return bp;
  return NULL;
}

typedef struct tagWATCH {
  struct tagWATCH *next;
  char *expr;
  char *value;
  char *type;
  unsigned seqnr;
  unsigned short flags;
} WATCH;
#define WATCHFLG_INSCOPE  0x0001
#define WATCHFLG_CHANGED  0x0002

static WATCH watch_root = { NULL };

static int watch_add(const char *gdbresult, const char *expr)
{
  WATCH *watch, *tail;
  const char *ptr;
  unsigned seqnr;
  size_t len;

  /* "done" and comma have already been skipped */
  if ((ptr = fieldfind(gdbresult, "name")) == NULL)
    return 0;
  ptr = fieldvalue(ptr, NULL);
  assert(ptr != NULL);
  if (strncmp(ptr, "watch", 5) != 0)
    return 0;
  seqnr = (unsigned)strtoul(ptr + 5, NULL, 10);
  assert(seqnr > 0);

  watch = malloc(sizeof(WATCH));
  if (watch == NULL)
    return 0;
  memset(watch, 0, sizeof(WATCH));
  watch->expr = strdup(expr);
  if (watch->expr == NULL) {
    free(watch);
    return 0;
  }
  if ((ptr = fieldfind(gdbresult, "value")) != NULL) {
    ptr = fieldvalue(ptr, &len);
    assert(ptr != NULL);
    watch->value = strdup_len(ptr, len);
  }
  if ((ptr = fieldfind(gdbresult, "type")) != NULL) {
    ptr = fieldvalue(ptr, &len);
    assert(ptr != NULL);
    watch->type = strdup_len(ptr, len);
  }
  watch->seqnr = seqnr;

  for (tail = &watch_root; tail->next != NULL; tail = tail->next)
    /* nothing */;
  tail->next = watch;
  return 1;
}

static int watch_del(unsigned seqnr)
{
  WATCH *watch, *prev;

  prev = &watch_root;
  watch = prev->next;
  while (watch != NULL && watch->seqnr != seqnr) {
    prev = watch;
    watch = prev->next;
  }
  if (watch == NULL)
    return 0; /* watch not found */
  assert(prev != NULL);
  prev->next = watch->next;
  if (watch->expr)
    free((void*)watch->expr);
  if (watch->value)
    free((void*)watch->value);
  if (watch->type)
    free((void*)watch->type);
  free((void*)watch);
  return 1;
}

static int watch_update(const char *gdbresult)
{
  WATCH *watch;
  const char *start;
  int count;

  /* clear all changed flags */
  for (watch = watch_root.next; watch != NULL; watch = watch->next)
    watch->flags &= ~WATCHFLG_CHANGED;

  if (strncmp(gdbresult, "done", 4) != 0)
    return 0;
  if ((start = strchr(gdbresult, ',')) == NULL)
    return 0;
  start = skipwhite(start + 1);
  if (strncmp(start, "changelist", 10) != 0)
    return 0;
  start = skipwhite(start + 10);
  assert(*start == '=');
  start = skipwhite(start + 1);
  assert(*start == '[');
  start = skipwhite(start + 1);
  count = 0;
  while (*start != ']') {
    const char *tail;
    char *line;
    size_t len;
    assert(*start == '{');
    start = skipwhite(start + 1);
    tail = strchr(start, '}');
    assert(tail != NULL);
    len = tail - start;
    if ((line = malloc((len + 1) * sizeof(char))) != NULL) {
      strncpy(line, start, len);
      line[len] = '\0';
      if ((start=fieldfind(line, "name")) != NULL) {
        unsigned seqnr;
        start = fieldvalue(start, NULL);
        assert(start != NULL);
        assert(strncmp(start, "watch", 5) == 0);
        seqnr = (unsigned)strtoul(start + 5, NULL, 0);
        assert(seqnr > 0);
        for (watch = watch_root.next; watch != NULL && watch->seqnr != seqnr; watch = watch->next)
          /* nothing */;
        assert(watch != NULL);
        if (watch != NULL) {
          if (watch->value != NULL) {
            free((void*)watch->value);
            watch->value = NULL;
          }
          if ((start = fieldfind(line, "value")) != NULL) {
            start = fieldvalue(start, &len);
            assert(start != NULL);
            watch->value = strdup_len(start, len);
          }
          if ((start = fieldfind(line, "in_scope")) != NULL) {
            start = fieldvalue(start, NULL);
            assert(start != NULL);
            if (*start == 't' || *start == '1')
              watch->flags |= WATCHFLG_INSCOPE;
            else
              watch->flags &= ~WATCHFLG_INSCOPE;
          }
          watch->flags |= WATCHFLG_CHANGED;
        }
      }
      free((void*)line);
      count++;
    }
    start = skipwhite(tail + 1);
    if (*start == ',')
      start = skipwhite(start + 1);
  }
  return count;
}

static const char *lastdirsep(const char *path)
{
  const char *ptr;

  if ((ptr = strrchr(path, DIRSEP_CHAR)) == NULL)
    ptr = path;
  #if defined _WIN32
    if (strrchr(ptr, '/') != NULL)
      ptr = strrchr(ptr, '/');
  #endif

  return (ptr == path) ? NULL : ptr;
}

static int ctf_findmetadata(const char *target, char *metadata, size_t metadata_len)
{
  char basename[256], path[256], *ptr;
  unsigned len, idx;

  /* create the base name (without path) from the target name; add a .tsdl extension */
  ptr = (char*)lastdirsep(target);
  strlcpy(basename, (ptr == NULL) ? target : ptr + 1, sizearray(basename));
  if ((ptr = strrchr(basename, '.')) != NULL)
    *ptr = '\0';
  strlcat(basename, ".tsdl", sizearray(basename));

  /* try current directory */
  if (access(basename, 0) == 0) {
    strlcpy(metadata, basename, metadata_len);
    return 1;
  }

  /* try target directory (if there is one) */
  ptr = (char*)lastdirsep(target);
  if (ptr != NULL) {
    len = min(ptr - target, sizearray(path) - 2);
    strncpy(path, target, len);
    path[len] = DIRSEP_CHAR;
    path[len + 1] = '\0';
    strlcat(path, basename, sizearray(path));
    translate_path(path, 1);
    if (access(path, 0) == 0) {
      strlcpy(metadata, path, metadata_len);
      return 1;
    }
  }

  /* try directories in the sources array */
  for (idx = 0; idx < sources_count; idx++) {
    ptr = (char*)lastdirsep(sources_pathlist[idx]);
    if (ptr != NULL) {
      len = min(ptr - sources_pathlist[idx], sizearray(path) - 2);
      strncpy(path, sources_pathlist[idx], len);
      path[len] = DIRSEP_CHAR;
      path[len + 1] = '\0';
      strlcat(path, basename, sizearray(path));
      translate_path(path, 1);
      if (access(path, 0) == 0) {
        strlcpy(metadata, path, metadata_len);
        return 1;
      }
    }
  }

  return 0;
}

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


static int check_stopped(int *filenr, int *linenr)
{
  STRINGLIST *item;
  int lastfound = 0;
  int last_is_stopped = 0;

  while ((item = stringlist_getlast(&consolestring_root, STRFLG_EXEC, STRFLG_HANDLED)) != NULL) {
    assert(item->text != NULL);
    item->flags |= STRFLG_HANDLED;
    if (!lastfound) {
      lastfound = 1;
      if (strncmp(item->text, "stopped", 7) == 0) {
        const char *head, *tail;
        last_is_stopped = 1;
        if ((head = strstr(item->text, "file=")) != 0) {
          char filename[256];
          unsigned len;
          assert(head[5] == '"');
          head += 6;
          tail = strchr(head, '"');
          assert(tail != NULL);
          len = tail - head;
          if (len >= sizearray(filename))
            len = sizearray(filename) - 1;
          strncpy(filename, head, len);
          filename[len] = '\0';
          /* look up the file */
          assert(sources_namelist != NULL);
          assert(filenr != NULL);
          *filenr = source_lookup(filename);
        }
        if ((head = strstr(item->text, "line=")) != 0) {
          assert(head[5] == '"');
          head += 6;
          assert(linenr != NULL);
          *linenr = (int)strtol(head, NULL, 10) ;
        }
      }
    }
  }

  return lastfound && last_is_stopped;
}

static int check_running(void)
{
  STRINGLIST *item;
  int lastfound = 0;
  int last_is_running = 0;

  while ((item = stringlist_getlast(&consolestring_root, STRFLG_EXEC, STRFLG_HANDLED)) != NULL) {
    assert(item->text != NULL);
    item->flags |= STRFLG_HANDLED;
    if (!lastfound) {
      lastfound = 1;
      if (strncmp(item->text, "running", 7) == 0)
        last_is_running = 1;
    }
  }

  return lastfound && last_is_running;
}

#if defined _WIN32

typedef struct tagTASK {
  HANDLE hProcess;
  HANDLE hThread;
  HANDLE prStdIn, pwStdIn;
  HANDLE prStdOut, pwStdOut;
  HANDLE prStdErr, pwStdErr;
} TASK;

int task_launch(const char *program, const char *options, TASK *task)
{
  STARTUPINFO startupInfo;
  PROCESS_INFORMATION processInformation;
  SECURITY_ATTRIBUTES secattr;
  char *cmdline;
  int result = 0;

  memset(&secattr, 0, sizeof secattr);
  secattr.nLength = sizeof secattr;
  secattr.bInheritHandle = TRUE;

  assert(task != NULL);
  memset(task, -1, sizeof(TASK));

  CreatePipe(&task->prStdIn, &task->pwStdIn, &secattr, 0);
  CreatePipe(&task->prStdOut, &task->pwStdOut, &secattr, 0);
  CreatePipe(&task->prStdErr, &task->pwStdErr, &secattr, 0);
  SetHandleInformation(task->pwStdIn, HANDLE_FLAG_INHERIT, 0);  /* make sure the write handle to stdin is not inherited */
  SetHandleInformation(task->prStdOut, HANDLE_FLAG_INHERIT, 0); /* make sure the read handle to stdout is not inherited */
  SetHandleInformation(task->prStdErr, HANDLE_FLAG_INHERIT, 0); /* make sure the read handle to stderr is not inherited */

  memset(&startupInfo, 0, sizeof startupInfo);
  startupInfo.cb = sizeof startupInfo;
  startupInfo.dwFlags = STARTF_USESTDHANDLES;
  startupInfo.hStdInput = task->prStdIn;
  startupInfo.hStdOutput = task->pwStdOut;
  startupInfo.hStdError = task->pwStdErr;

  memset(&processInformation, 0, sizeof processInformation);

  if (options != NULL) {
    assert(program != NULL);
    cmdline = (char*)malloc((strlen(program) + strlen(options) + 2) * sizeof(char));
    if (cmdline != NULL) {
      strcpy(cmdline, program);
      strcat(cmdline, " ");
      strcat(cmdline, options);
    }
  } else {
    cmdline = NULL;
  }

  assert(program != NULL);
  if (CreateProcess(program, cmdline, 0, 0, TRUE,
                    NORMAL_PRIORITY_CLASS | DETACHED_PROCESS,
                    0, 0, &startupInfo, &processInformation))
  {
    task->hProcess = processInformation.hProcess;
    task->hThread = processInformation.hThread;
    result = 1;
  }

  if (cmdline != NULL)
    free((void*)cmdline);

  return result;
}

int task_isrunning(TASK *task)
{
  DWORD dwExitCode;

  assert(task != NULL);
  if (task->hProcess == INVALID_HANDLE_VALUE)
    return 0;
  return GetExitCodeProcess(task->hProcess, &dwExitCode) && (dwExitCode == STILL_ACTIVE);
}

int task_close(TASK *task)
{
  DWORD dwExitCode = 0;

  assert(task != NULL);
  if (task->hProcess != INVALID_HANDLE_VALUE) {
    if (task_isrunning(task))
      TerminateProcess(task->hProcess, dwExitCode);
    else
      GetExitCodeProcess(task->hProcess, &dwExitCode);
  }
  if (task->hThread != INVALID_HANDLE_VALUE)
    CloseHandle(task->hThread);
  if (task->hProcess != INVALID_HANDLE_VALUE)
    CloseHandle(task->hProcess);

  memset(task, -1, sizeof(TASK));
  return (int)dwExitCode;
}

int task_stdin(TASK *task, const char *text)
{
  DWORD dwWritten;

  assert(task != NULL);
  assert(text != NULL);
  if (task->hProcess == INVALID_HANDLE_VALUE || task->pwStdIn == INVALID_HANDLE_VALUE)
    return 0;
  return WriteFile(task->pwStdIn, text, strlen(text), &dwWritten, NULL) ? (int)dwWritten : -1;
}

//void task_break(TASK *task)
//{
//  assert(task != NULL);
//  if (task->hProcess != INVALID_HANDLE_VALUE) {
//    HINSTANCE hinstKrnl = LoadLibrary("kernel32.dll");
//    BOOL WINAPI (*AttachConsole)(DWORD dwProcessId) = GetProcAddress(hinstKrnl, "AttachConsole");
//    DWORD id = GetProcessId(task->hProcess);
//    AttachConsole(id);
//    SetConsoleCtrlHandler(NULL, FALSE);
//    GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
//    FreeLibrary(hinstKrnl);
//  }
//}

int task_stdout(TASK *task, char *text, size_t maxlength)
{
  DWORD dwRead, dwAvail;
  BOOL result;

  assert(task != NULL);
  if (task->hProcess == INVALID_HANDLE_VALUE || task->prStdOut == INVALID_HANDLE_VALUE)
    return 0;

  if (PeekNamedPipe(task->prStdOut, NULL, 0, NULL, &dwAvail, NULL) == 0 || dwAvail == 0)
    return 0;
  if (dwAvail > maxlength)
    dwAvail = maxlength;

  assert(text != NULL);
  assert(dwAvail > 0);
  result = ReadFile(task->prStdOut, text, dwAvail - 1, &dwRead, NULL);
  if (!result)
    dwRead = 0;
  text[dwRead] = '\0';  /* zero-terminate the output */
  return dwRead;
}

int task_stderr(TASK *task, char *text, size_t maxlength)
{
  DWORD dwRead, dwAvail;
  BOOL result;

  assert(task != NULL);
  if (task->hProcess == INVALID_HANDLE_VALUE || task->prStdErr == INVALID_HANDLE_VALUE)
    return 0;

  if (PeekNamedPipe(task->prStdErr, NULL, 0, NULL, &dwAvail, NULL) == 0 || dwAvail == 0)
    return 0;
  if (dwAvail > maxlength)
    dwAvail = maxlength;

  assert(text != NULL);
  assert(dwAvail > 0);
  result = ReadFile(task->prStdErr, text, dwAvail - 1, &dwRead, NULL);
  if (!result)
    dwRead = 0;
  text[dwRead] = '\0';  /* zero-terminate the output */
  return dwRead;
}

static char *translate_path(char *path, int todos)
{
  char *p;
  if (todos) {
    while ((p = strchr(path, '/')) != NULL)
      *p = '\\';
  } else {
    while ((p = strchr(path, '\\')) != NULL)
      *p = '/';
  }
  return path;
}

#else

typedef struct tagTASK {
  pid_t pid;
  int pStdIn[2];  /* in a pipe, pipe[0] is for read, pipe[1] is for write */
  int pStdOut[2];
  int pStdErr[2];
} TASK;

int task_isrunning(TASK *task);

int task_launch(const char *program, const char *options, TASK *task)
{
  assert(task != NULL);
  memset(task, 0, sizeof(TASK));

  pipe(task->pStdIn);
  pipe(task->pStdOut);
  pipe(task->pStdErr);

  task->pid = fork();
  if (task->pid == 0) {
    /* child: map pipe endpoints to console I/O, then close the (inherited) pipe handles */
    const char *argv[3] = { program, options, NULL };

    dup2(task->pStdIn[0], STDIN_FILENO);
    dup2(task->pStdOut[1], STDOUT_FILENO);
    dup2(task->pStdErr[1], STDERR_FILENO);
    close(task->pStdIn[0]);
    close(task->pStdIn[1]);
    close(task->pStdOut[0]);
    close(task->pStdOut[1]);
    close(task->pStdErr[0]);
    close(task->pStdErr[1]);

    execvp(program, (char**)argv);
    exit(0);  /* if execv() returns, there was an error */
  } else {
    /* parent: close the pipe handles not required (by the parent) */
    assert(task->pid != 0);
    close(task->pStdIn[0]);
    close(task->pStdOut[1]);
    close(task->pStdErr[1]);
  }

  usleep(200*1000); /* give GDB a moment to start */
  return task_isrunning(task);
}

int task_isrunning(TASK *task)
{
  int status;
  pid_t result;

  assert(task != NULL);
  if (task->pid == 0)
    return 0;
  result = waitpid(task->pid, &status, WNOHANG);
  return (result == 0);
}

int task_close(TASK *task)
{
  int exitcode = 0;

  assert(task != NULL);
  if (task->pid != 0) {
    int status;
    if (task_isrunning(task))
      kill(task->pid, SIGTERM);
    if (waitpid(task->pid, &status, 0) >= 0 && WIFEXITED(status))
      exitcode = WEXITSTATUS(status);
  }
  close(task->pStdIn[1]);
  close(task->pStdOut[0]);
  close(task->pStdErr[0]);

  memset(task, 0, sizeof(TASK));
  return exitcode;
}

int task_stdin(TASK *task, const char *text)
{
  assert(task != NULL);
  assert(text != NULL);
  if (task->pid == 0 || task->pStdIn[1] == 0)
    return 0;
  return write(task->pStdIn[1], text, strlen(text));
}

//void task_break(TASK *task)
//{
//}

static int peekpipe(int fd)
{
  int result;
  struct pollfd fds;
  fds.fd = fd;
  fds.events = POLLIN;
  result = poll(&fds, 1, 0);
  return (result >= 0 && (fds.revents & (POLLERR | POLLNVAL | POLLIN)) == POLLIN);
}

int task_stdout(TASK *task, char *text, size_t maxlength)
{
  int count;

  assert(task != NULL);
  if (task->pid == 0 || task->pStdOut[0] == 0)
    return 0;

  if (peekpipe(task->pStdOut[0]) == 0)
    return 0;

  assert(text != NULL);
  assert(maxlength > 0);
  count = read(task->pStdOut[0], text, maxlength - 1);
  if (count < 0)
    count = 0;
  text[count] = '\0';   /* zero-terminate the output */
  return count;
}

int task_stderr(TASK *task, char *text, size_t maxlength)
{
  int count;

  assert(task != NULL);
  if (task->pid == 0 || task->pStdErr[0] == 0)
    return 0;

  if (peekpipe(task->pStdErr[0]) == 0)
    return 0;

  assert(text != NULL);
  assert(maxlength > 0);
  count = read(task->pStdErr[0], text, maxlength - 1);
  if (count < 0)
    count = 0;
  text[count] = '\0';   /* zero-terminate the output */
  return count;
}

unsigned long GetTickCount(void)
{
  struct timeval  tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static char *translate_path(char *path, int todos)
{
  (void)todos;
  return path;
}

static int memicmp(const unsigned char *p1, const unsigned char *p2, size_t count)
{
  int diff = 0;
  while (count-- > 0 && diff == 0)
    diff = toupper(*p1++) - toupper(*p2++);
  return diff;
}

#endif

static unsigned long idle_wait = 0;
static unsigned long idle_mark = 0;
static void set_idle_time(unsigned long timeout)
{
  idle_wait = timeout;
  idle_mark = GetTickCount();
}
static int is_idle(void)
{
  unsigned long stamp;
  if (idle_wait == 0 || idle_mark == 0)
    return 0;
  stamp = GetTickCount();
  if (stamp - idle_mark > idle_wait) {
    idle_wait = idle_mark = 0;
    return 0;
  }
  return 1;
}

#define WINDOW_WIDTH  750   /* default window size (window is resizable) */
#define WINDOW_HEIGHT 500
#define FONT_HEIGHT   14
#define ROW_HEIGHT    (1.6 * FONT_HEIGHT)
#define COMBOROW_CY   (0.8 * ROW_HEIGHT)

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

/* console_widget() draws the text in the console window and scrolls to the last
   line if new text was added */
static void console_widget(struct nk_context *ctx, const char *id, float rowheight)
{
  static int scrollpos = 0;
  static int linecount = 0;
  STRINGLIST *item;
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window const *stwin = &ctx->style.window;
  struct nk_user_font const *font = ctx->style.font;

  /* black background on group */
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, nk_rgba(20, 29, 38, 225));
  if (nk_group_begin_titled(ctx, id, "", NK_WINDOW_BORDER)) {
    int lines = 0;
    float lineheight = 0;
    for (item = consolestring_root.next; item != NULL; item = item->next) {
      float textwidth;
      if (item->flags & console_hiddenflags)
        continue;
      NK_ASSERT(item->text != NULL);
      nk_layout_row_begin(ctx, NK_STATIC, rowheight, 1);
      if (lineheight <= 0.1) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        lineheight = rcline.h;
      }
      /* calculate size of the text */
      NK_ASSERT(font != NULL && font->width != NULL);
      textwidth = font->width(font->userdata, font->height, item->text, strlen(item->text)) + 10;
      nk_layout_row_push(ctx, textwidth);
      if (item->flags & (STRFLG_INPUT | STRFLG_MI_INPUT))
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, nk_rgb(204, 199, 141));
      else if (item->flags & STRFLG_ERROR)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, nk_rgb(255, 100, 128));
      else if (item->flags & STRFLG_RESULT)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, nk_rgb(64, 220, 255));
      else if (item->flags & STRFLG_NOTICE)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, nk_rgb(220, 220, 128));
      else if (item->flags & STRFLG_STATUS)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, nk_rgb(255, 255, 128));
      else if (item->flags & STRFLG_EXEC)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, nk_rgb(128, 222, 128));
      else if (item->flags & STRFLG_LOG)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, nk_rgb(128, 222, 222));
      else
        nk_label(ctx, item->text, NK_TEXT_LEFT);
      nk_layout_row_end(ctx);
      lines++;
    }
    if (lines > 0) {
      nk_layout_row_dynamic(ctx, rowheight, 1);
      nk_spacing(ctx, 1);
    }
    nk_group_end(ctx);
    if (lines > 0) {
      /* calculate scrolling: if number of lines change, scroll to the last line */
      int ypos = scrollpos;
      int widgetlines = (int)((rcwidget.h - 2 * stwin->padding.y) / lineheight);
      if (lines != linecount) {
        linecount = lines;
        ypos = (int)((lines - widgetlines + 1) * lineheight);
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
}

static int source_cursorfile = 0; /* file and line in the view and that the cursor is at */
static int source_cursorline = 0;
static int source_execfile = 0;   /* file and line that the execution point is at */
static int source_execline = 0;
static float source_lineheight = 0;
static float source_charwidth = 0;
static int source_vp_rows = 0;

/* source_widget() draws the text of a source file */
static void source_widget(struct nk_context *ctx, const char *id, float rowheight)
{
  static int saved_execfile = 0, saved_execline = 0;
  static int saved_cursorline = 0;
  int fonttype;
  STRINGLIST *item;
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window const *stwin = &ctx->style.window;
  struct nk_style_button stbtn = ctx->style.button;
  struct nk_user_font const *font;

  /* preset common parts of the new button style */
  stbtn.border = 0;
  stbtn.rounding = 0;
  stbtn.padding.x = stbtn.padding.y = 0;

  /* monospaced font */
  fonttype = guidriver_setfont(ctx, FONT_MONO);
  font = ctx->style.font;

  /* black background on group */
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, nk_rgba(20, 29, 38, 225));
  if (nk_group_begin_titled(ctx, id, "", NK_WINDOW_BORDER)) {
    int lines = 0, maxlen = 0;
    float maxwidth = 0;
    for (item = sourcefile_root.next; item != NULL; item = item->next) {
      float textwidth;
      BREAKPOINT *bkpt;
      lines++;
      NK_ASSERT(item->text != NULL);
      nk_layout_row_begin(ctx, NK_STATIC, rowheight, 4);
      if (source_lineheight <= 0.1) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        source_lineheight = rcline.h;
      }
      /* line number or active/breakpoint markers */
      if ((bkpt = breakpoint_lookup(source_cursorfile, lines)) == NULL) {
        char str[20];
        nk_layout_row_push(ctx, 2 * rowheight);
        sprintf(str, "%4d", lines);
        if (lines == source_cursorline)
          nk_label_colored(ctx, str, NK_TEXT_LEFT, nk_rgb(255, 250, 150));
        else
          nk_label(ctx, str, NK_TEXT_LEFT);
      } else {
        nk_layout_row_push(ctx, rowheight - ctx->style.window.spacing.x);
        nk_spacing(ctx, 1);
        /* breakpoint marker */
        nk_layout_row_push(ctx, rowheight);
        assert(bkpt != NULL);
        stbtn.normal.data.color = stbtn.hover.data.color
          = stbtn.active.data.color = stbtn.text_background
          = nk_rgba(20, 29, 38, 225);
        if (bkpt->enabled)
          stbtn.text_normal = stbtn.text_active = stbtn.text_hover = nk_rgb(140, 25, 50);
        else
          stbtn.text_normal = stbtn.text_active = stbtn.text_hover = nk_rgb(255, 50, 120);
        nk_button_symbol_styled(ctx, &stbtn, bkpt->enabled ? NK_SYMBOL_CIRCLE_SOLID : NK_SYMBOL_CIRCLE_OUTLINE);
      }
      /* active line marker */
      nk_layout_row_push(ctx, rowheight / 2);
      if (lines == source_execline && source_cursorfile == source_execfile) {
        stbtn.normal.data.color = stbtn.hover.data.color
          = stbtn.active.data.color = stbtn.text_background
          = nk_rgba(20, 29, 38, 225);
        stbtn.text_normal = stbtn.text_active = stbtn.text_hover = nk_rgb(255, 250, 150);
        nk_button_symbol_styled(ctx, &stbtn, NK_SYMBOL_TRIANGLE_RIGHT);
      } else {
        nk_spacing(ctx, 1);
      }
      /* calculate size of the text */
      NK_ASSERT(font != NULL && font->width != NULL);
      textwidth = font->width(font->userdata, font->height, item->text, strlen(item->text));
      if (textwidth > maxwidth) {
        maxwidth = textwidth;
        maxlen = strlen(item->text);
      }
      nk_layout_row_push(ctx, textwidth + 10);
      if (lines == source_cursorline)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, nk_rgb(255, 250, 150));
      else
        nk_label(ctx, item->text, NK_TEXT_LEFT);
      nk_layout_row_end(ctx);
    }
    if (lines == 0) {
      nk_layout_row_dynamic(ctx, rowheight, 1);
      nk_spacing(ctx, 1);
      nk_label(ctx, "NO SOURCE", NK_TEXT_CENTERED);
    }
    nk_group_end(ctx);
    if (maxlen > 0)
      source_charwidth = maxwidth / maxlen;
    source_vp_rows = (int)((rcwidget.h - 2 * stwin->padding.y) / source_lineheight);
    if (lines > 0) {
      if (saved_execline != source_execline || saved_execfile != source_execfile) {
        saved_execfile = source_execfile;
        source_cursorline = saved_execline = source_execline;
      }
      if (saved_cursorline != source_cursorline) {
        /* calculate scrolling: make cursor line fit in the window */
        int topline;
        unsigned xscroll, yscroll;
        nk_group_get_scroll(ctx, id, &xscroll, &yscroll);
        topline = (int)(yscroll / source_lineheight);
        if (source_cursorline < topline + 1) {
          topline = source_cursorline - 1;
          if (topline < 0)
            topline = 0;
          nk_group_set_scroll(ctx, id, 0, (nk_uint)(topline * source_lineheight));
        } else if (source_cursorline >= topline + source_vp_rows && lines > 3) {
          topline = source_cursorline - source_vp_rows;
          nk_group_set_scroll(ctx, id, 0, (nk_uint)(topline * source_lineheight));
        }
        saved_cursorline = source_cursorline;
      }
    }
  }
  nk_style_pop_color(ctx);
  guidriver_setfont(ctx, fonttype);
}

/** source_mouse2char()
 *  \return 0 if the mouse pointer is not in the source view (or the source
 *          view is not valid), 1 if valid row/column is returned.
 *  \note "col" is set to 0 if clicked in the margin.
 */
static int source_mouse2char(struct nk_context *ctx, const char *id,
                             float rowheight, struct nk_rect widget_bounds,
                             int *row, int *col)
{
  struct nk_mouse *mouse;
  unsigned xscroll, yscroll;

  assert(ctx != NULL);
  mouse = &ctx->input.mouse;
  assert(mouse != NULL);
  if (!NK_INBOX(mouse->pos.x, mouse->pos.y, widget_bounds.x, widget_bounds.y, widget_bounds.w, widget_bounds.h))
    return 0;
  assert(id != NULL);
  nk_group_get_scroll(ctx, id, &xscroll, &yscroll);
  if (source_lineheight <= 0)
    return 0;
  if (row != NULL)
    *row = (int)(((mouse->pos.y - widget_bounds.y) + yscroll) / source_lineheight) + 1;
  if (col != NULL) {
    float offs = 2 * rowheight + rowheight / 2 + 2 * ctx->style.window.spacing.x;
    float c = mouse->pos.x - widget_bounds.x - offs + xscroll;
    if (c < 0.0)
      *col = 0;
    else
      *col = (int)(c / source_charwidth) + 1;
  }
  return 1;
}

/** source_getsymbol() returns the symbol at the row and column in the
 *  current source. Parameters row and column are 1-based.
 */
static int source_getsymbol(char *symname, size_t symlen, int row, int col)
{
  STRINGLIST *item;
  const char *head, *tail;
  int len, nest;

  assert(symname != NULL && symlen > 0);
  *symname = '\0';
  if (row < 1 || col < 1)
    return 0;
  for (item = sourcefile_root.next; item != NULL && row > 1; item = item->next)
    row--;
  if (item == NULL)
    return 0;
  assert(item->text != NULL);
  if ((unsigned)col > strlen(item->text))
    return 0;
  /* when moving to the left, skip '.' and '->' to complete the structure field
     with the structure variable; also skip '*' so that pointing at '*ptr' shows
     the dereferenced value */
  head = item->text + (col - 1);
  if (!isalpha(*head) && !isdigit(*head) && *head != '_')
    return 0;
  while (head > item->text
         && (isalpha(*(head - 1)) || isdigit(*(head - 1)) || *(head - 1) == '_'
             || *(head - 1) == '.' || (*(head - 1) == '>' && *(head - 2) == '-') || (*(head - 1) == '-' && *head == '>')
             || *(head - 1) == '*'))
    head--;
  if (!isalpha(*head) && *head != '_' && *head != '*')
    return 0; /* symbol must start with a letter or '_' (but make an exception for the '*' prefix) */
  /* when moving to the right, skip '[' and ']' so that pointing at 'vector[i]'
     shows the element 'i' of array 'vector' (but skip ']' only if '[' was seen
     too) */
  nest = 0;
  tail = item->text + (col - 1);
  while (isalpha(*tail) || isdigit(*tail) || *tail == '_' || *tail == '[' || (*tail == ']' && nest > 0)) {
    if (*tail == '[')
      nest++;
    else if (*tail == ']')
      nest--;
    tail++;
  }
  if (nest != 0)
    return 0;
  len = tail - head;
  if ((unsigned)len >= symlen)
    return 0; /* full symbol name does not fit, no need to try to look it up */
  strncpy(symname, head, len);
  symname[len] = '\0';
  return 1;
}

#define TOOLTIP_DELAY 1000
static int tooltip(struct nk_context *ctx, struct nk_rect bounds, const char *text, struct nk_rect *viewport)
{
  static struct nk_rect recent_bounds;
  static unsigned long start_tstamp;
  unsigned long tstamp;

  #if defined WIN32 || defined _WIN32
    tstamp = GetTickCount();  /* 55ms granularity, but good enough */
  #else
    struct timeval  tv;
    gettimeofday(&tv, NULL);
    tstamp = tv.tv_sec * 1000 + tv.tv_usec / 1000;
  #endif

  if (!nk_input_is_mouse_hovering_rect(&ctx->input, bounds))
    return 0;           /* not hovering this control/area */
  if (memcmp(&bounds, &recent_bounds, sizeof(struct nk_rect)) != 0) {
    /* hovering this control/area, but it's a different one from the previous;
       restart timer */
    recent_bounds = bounds;
    start_tstamp = tstamp;
    return 0;
  }
  if (tstamp - start_tstamp < TOOLTIP_DELAY)
    return 0;           /* delay time has not reached its value yet */
  if (text != NULL)
    nk_tooltip(ctx, text, viewport);
  return 1;
}

enum {
  STATE_INIT,
  STATE_GDB_TASK,
  STATE_SCAN_BMP,
  STATE_TARGET_EXT,
  STATE_MON_TPWR,
  STATE_MON_SCAN,
  STATE_ASYNC_MODE,
  STATE_ATTACH,
  STATE_FILE,
  STATE_FILE_TEST,
  STATE_MEMACCESS_1,
  STATE_MEMACCESS_2,
  STATE_DOWNLOAD,
  STATE_VERIFY,
  STATE_CHECK_MAIN,
  STATE_START,
  STATE_EXEC_CMD,
  /* ----- */
  STATE_STOPPED,
  STATE_RUNNING,
  STATE_LIST_BREAKPOINTS,
  STATE_LIST_LOCALS,
  STATE_LIST_WATCHES,
  STATE_BREAK_TOGGLE,
  STATE_WATCH_TOGGLE,
  STATE_SWOTRACE,
  STATE_SWODEVICE,
  STATE_SWOGENERIC,
  STATE_SWOCHANNELS,
  STATE_HOVER_SYMBOL,
  STATE_QUIT,
};

enum {
  STATEPARAM_EXEC_RESTART,
  STATEPARAM_EXEC_CONTINUE,
  STATEPARAM_EXEC_STOP,
  STATEPARAM_EXEC_NEXT,
  STATEPARAM_EXEC_STEP,
  STATEPARAM_EXEC_FINISH,
  STATEPARAM_EXEC_UNTIL,
  /* ----- */
  STATEPARAM_BP_ENABLE,
  STATEPARAM_BP_DISABLE,
  STATEPARAM_BP_ADD,
  STATEPARAM_BP_DELETE,
  /* ----- */
  STATEPARAM_WATCH_SET,
  STATEPARAM_WATCH_DEL,
};

#define REFRESH_BREAKPOINTS 0x0001
#define REFRESH_LOCALS      0x0002
#define REFRESH_WATCHES     0x0004
#define REFRESH_CONSOLE     0x8000  /* input comes from a console, check for extra "done" result */

#define MSG_BMP_NOT_FOUND   0x0001

#define TERM_END(s, i)  ((s)[i] == ' ' || (s)[i] == '\0')

static int handle_list_cmd(const char *command)
{
  command = skipwhite(command);
  if (strncmp(command, "list", 4) == 0 && TERM_END(command, 4)) {
    const char *p1 = skipwhite(command + 4);
    if (*p1 == '+' || *p1 == '\0') {
      source_cursorline += source_vp_rows;      /* "list" & "list +" */
      if (source_cursorline > source_linecount())
        source_cursorline = source_linecount();
      return 1;
    } else if (*p1 == '-') {
      source_cursorline -= source_vp_rows;      /* "list -" */
      if (source_cursorline < 1)
        source_cursorline = 1;
      return 1;
    } else if (isdigit(*p1)) {
      int line = (int)strtol(p1, NULL, 10);     /* "list #" (where # is a line number) */
      if (line >= 1 && line <= source_linecount()) {
        source_cursorline = line;
        return 1;
      }
    } else {
      unsigned line, idx;                       /* "list filename" & "list filename:#" */
      char *p2 = strchr(p1, ':');
      if (p2 != NULL) {
        *p2++ = '\0';
        line = (int)strtol(p2, NULL, 10);
      } else {
        line = 1;
      }
      if (strchr(p1, '.') != NULL) {
        /* extension is given, try an exact match */
        for (idx = 0; idx < sources_count; idx++)
          if (strcmp(sources_namelist[idx], p1) == 0)
            break;
      } else {
        /* no extension, ignore extension on match */
        unsigned len = strlen(p1);
        for (idx = 0; idx < sources_count; idx++)
          if (strncmp(sources_namelist[idx], p1, len) == 0 && sources_namelist[idx][len] == '.')
            break;
      }
      if (idx < sources_count && line >= 1) {
        source_cursorfile = idx;
        source_cursorline = line;
        return 1;
      }
    }
  }
  return 0;
}

static int handle_display_cmd(const char *command, int *param, char *symbol, size_t symlength)
{
  const char *ptr;

  assert(command != NULL);
  assert(param != NULL);
  assert(symbol != NULL && symlength > 0);
  command = skipwhite(command);
  if (strncmp(command, "disp ", 5) == 0 || strncmp(command, "display ", 8) == 0) {
    param[0] = STATEPARAM_WATCH_SET;
    ptr = strchr(command, ' ');
    assert(ptr != NULL);
    strlcpy(symbol, skipwhite(ptr), symlength);
    return 1;
  } else if (strncmp(command, "undisp ", 7) == 0 || strncmp(command, "undisplay ", 10) == 0) {
    param[0] = STATEPARAM_WATCH_DEL;
    ptr = strchr(command, ' ');
    assert(ptr != NULL);
    if (isdigit(*ptr)) {
      param[1] = (int)strtol(ptr, NULL, 10);
      return 1;
    } else {
      /* find a watch with the name */
      WATCH *watch;
      for (watch = watch_root.next; watch != NULL; watch = watch->next) {
        if (strcmp(watch->expr, ptr) == 0) {
          param[1] = watch->seqnr;
          return 1;
        }
      }
    }
  }
  return 0;
}

static int handle_find_cmd(const char *command)
{
  static char pattern[50] = "";

  assert(command != NULL);
  command = skipwhite(command);
  if (strncmp(command, "find", 4) == 0 && TERM_END(command, 4)) {
    STRINGLIST *item = sourcefile_root.next;
    int linenr, patlen;
    const char *ptr;
    if ((ptr = strchr(command, ' ')) != NULL) {
      ptr = skipwhite(ptr);
      if (*ptr != '\0')
        strlcpy(pattern, ptr, sizearray(pattern));
    }
    patlen = strlen(pattern);
    if (patlen == 0)
      return 1; /* invalid pattern, but command syntax is ok */
    /* find pattern, starting from source_cursorline */
    linenr = 1;
    while (item != NULL && linenr < source_cursorline) {
      linenr++;
      item = item->next;
    }
    if (item == NULL || source_cursorline <= 0) {
      item = sourcefile_root.next;
      linenr = 1;
    } else {
      item = item->next;
      linenr++;
    }
    while (linenr != source_cursorline) {
      int idx, txtlen;
      assert(item != NULL && item->text != NULL);
      txtlen = strlen(item->text);
      idx = 0;
      while (idx < txtlen) {
        while (idx < txtlen && toupper(item->text[idx]) != toupper(pattern[0]))
          idx++;
        if (idx + patlen > txtlen)
          break;      /* not found on this line */
        if (memicmp((const unsigned char*)item->text + idx, (const unsigned char*)pattern, patlen) == 0)
          break;      /* found on this line */
        idx++;
      }
      if (idx + patlen <= txtlen) {
        source_cursorline = linenr;
        return 1;     /* found, stop search */
      }
      item = item->next;
      linenr++;
      if (item == NULL) {
        item = sourcefile_root.next;
        linenr = 1;
        if (source_cursorline == 0)
          source_cursorline = 1;
      }
    } /* while (linenr != source_cursorline) */
    /* pattern not found */
    console_add("Text not found\n", STRFLG_ERROR);
    return 1;
  }
  return 0;
}

enum { SWOMODE_NONE, SWOMODE_MANCHESTER, SWOMODE_ASYNC, SWOMODE_PASSIVE };

static void trace_info_channel(int chan, int enabled_only)
{
  char msg[200];

  if (enabled_only && !channel_getenabled(chan))
    return;

  sprintf(msg, "Channel %d: ", chan);
  if (chan < 0 || chan >= NUM_CHANNELS) {
    strlcat(msg, "invalid", sizearray(msg));
  } else {
    const char *ptr;
    if (channel_getenabled(chan))
      strlcat(msg, "enabled", sizearray(msg));
    else
      strlcat(msg, "disabled", sizearray(msg));
    ptr = channel_getname(chan, NULL, 0);
    if (ptr != NULL && strlen(ptr) > 0) {
      strlcat(msg, " \"", sizearray(msg));
      strlcat(msg, ptr, sizearray(msg));
      strlcat(msg, "\"", sizearray(msg));
    }
  }
  strlcat(msg, "\n", sizearray(msg));
  console_add(msg, STRFLG_STATUS);
}

static void trace_info_mode(unsigned mode, unsigned clock, unsigned bitrate)
{
  char msg[200];

  strcpy(msg, "Active configuration: ");
  switch (mode) {
  case SWOMODE_NONE:
    strcat(msg, "disabled");
    break;
  case SWOMODE_MANCHESTER:
    sprintf(msg + strlen(msg), "Manchester encoding, clock = %u, bitrate = %u", clock, bitrate);
    break;
  case SWOMODE_ASYNC:
    sprintf(msg + strlen(msg), "Asynchronous encoding, clock = %u, bitrate = %u", clock, bitrate);
    break;
  case SWOMODE_PASSIVE:
    strcat(msg, "Manchester encoding, passive");
    break;
  }
  strlcat(msg, "\n", sizearray(msg));
  console_add(msg, STRFLG_STATUS);
}

static int handle_trace_cmd(const char *command, unsigned *mode, unsigned *clock, unsigned *bitrate)
{
  const char *ptr;

  assert(command != NULL);
  if (strncmp(command, "trace ", 6) != 0)
    return 0;
  ptr = skipwhite(command + 6);

  if (*ptr == '\0' || (strncmp(ptr, "info", 4) == 0 && TERM_END(ptr, 4)))
    return 3; /* if only "trace" is typed, interpret it as "trace info" */

  if (strncmp(ptr, "channel ", 7) == 0 || strncmp(ptr, "chan ", 5) == 0 || strncmp(ptr, "ch ", 3) == 0) {
    int chan;
    char *opts;
    ptr = strchr(ptr, ' ');
    assert(ptr != NULL);
    chan = (int)strtol(skipwhite(ptr), (char**)ptr, 10);
    ptr = skipwhite(ptr);
    opts = alloca(strlen(ptr) + 1);
    strcpy(opts, ptr);
    for (ptr = strtok(opts, " "); ptr != NULL; ptr = strtok(NULL, " ")) {
      if (stricmp(ptr, "enable")) {
        channel_setenabled(chan, 1);
      } else if (stricmp(ptr, "disable")) {
        channel_setenabled(chan, 0);
      } else if (*ptr == '#') {
        unsigned long v = strtoul(ptr + 1, NULL, 16);
        int r, g, b;
        if (strlen(ptr) == 3 + 1) {
          r = ((v & 0xf00) >> 4) | ((v & 0xf00) >> 8);
          g = (v & 0x0f0) | ((v & 0x0f0) >> 4);
          b = (v & 0x00f) << 4 | (v & 0x00f);
        } else {
          r = (v & 0xff0000) >> 16;
          g = (v & 0x00ff00) >> 8;
          b = (v & 0x0000ff);
        }
        channel_setcolor(chan, nk_rgb(r, g, b));
      } else {
        channel_setname(chan, ptr);
      }
    }
    trace_info_channel(chan, 0);
    return 2; /* only channel set changed */
  }

  /* mode */
  assert(mode != NULL);
  if (strncmp(ptr, "disable", 7) == 0 && TERM_END(ptr, 7)) {
    *mode = SWOMODE_NONE;
    return 2; /* special mode, disable all channels when turning tracing off */
  }
  if (strncmp(ptr, "async", 5) == 0 && TERM_END(ptr, 5)) {
    *mode = SWOMODE_ASYNC;
    ptr = skipwhite(ptr + 5);
  } else if ((strncmp(ptr, "passive", 7) == 0 && TERM_END(ptr, 7)) || (strncmp(ptr, "pasv", 4) == 0 && TERM_END(ptr, 4))) {
    *mode = SWOMODE_PASSIVE;
    ptr = strchr(ptr, ' ');
    ptr = (ptr != NULL) ? skipwhite(ptr) : strchr(command, '\0');
  } else {
    *mode = SWOMODE_MANCHESTER;
  }
  /* clock */
  if (isdigit(*ptr)) {
    double v = strtod(ptr, (char**)&ptr);
    if ((strnicmp(ptr, "mhz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "m", 1) == 0 && TERM_END(ptr, 1))) {
      v *= 1000000;
      ptr = strchr(ptr, ' ');
      ptr = (ptr != NULL) ? skipwhite(ptr) : strchr(command, '\0');
    }
    assert(clock != NULL);
    *clock = (unsigned)(v + 0.5);
  }
  /* bitrate */
  if (isdigit(*ptr)) {
    double v = strtod(ptr, (char**)&ptr);
    if ((strnicmp(ptr, "mhz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "m", 1) == 0 && TERM_END(ptr, 1))) {
      v *= 1000000;
      ptr = strchr(ptr, ' ');
      ptr = (ptr != NULL) ? skipwhite(ptr) : strchr(command, '\0');
    } else if ((strnicmp(ptr, "khz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "k", 1) == 0 && TERM_END(ptr, 1))) {
      v *= 1000;
      ptr = strchr(ptr, ' ');
      ptr = (ptr != NULL) ? skipwhite(ptr) : strchr(command, '\0');
    }
    assert(bitrate != NULL);
    *bitrate = (unsigned)(v + 0.5);
  }
  trace_info_mode(*mode, *clock, *bitrate);
  return 1; /* assume entire protocol changed */
}

int main(int argc, char *argv[])
{
  enum { TAB_CONFIGURATION, TAB_BREAKPOINTS, TAB_WATCHES, TAB_SEMIHOSTING, TAB_SWO, /* --- */ TAB_COUNT };
  enum { SPLITTER_NONE, SPLITTER_VERTICAL, SPLITTER_HORIZONTAL, SIZER_SEMIHOSTING, SIZER_SWO };

  struct nk_context *ctx;
  struct nk_image btn_folder;
  char txtFilename[256], txtConfigFile[256], txtGDBpath[256], txtTSDLfile[256];
  char port_gdb[64], mcu_family[64], mcu_architecture[64];
  char valstr[128];
  int canvas_width, canvas_height;
  enum nk_collapse_states tab_states[TAB_COUNT];
  float tab_heights[TAB_COUNT];
  int opt_tpwr = nk_false;
  int opt_allmsg = nk_false;
  int opt_autodownload = nk_true;
  unsigned opt_swomode = SWOMODE_NONE, opt_swobaud = 100000, opt_swoclock = 48000000;
  float splitter_hor = 0.75, splitter_ver = 0.75;
  char console_edit[128] = "", watch_edit[128] = "";
  STRINGLIST consoleedit_root = { NULL, NULL, 0 }, *consoleedit_next;
  TASK task;
  char cmd[300], statesymbol[64], ttipvalue[256];
  int curstate, prevstate, stateparam[3];
  int refreshflags, trace_status, warn_source_tstamps;
  int atprompt, insplitter, console_activate, cont_is_run, exitcode;
  int idx, result;
  int prev_clicked_line;
  unsigned watchseq;
  unsigned long scriptparams[3];

  /* locate the configuration file */
  if (folder_AppConfig(txtConfigFile, sizearray(txtConfigFile))) {
    strlcat(txtConfigFile, DIR_SEPARATOR "BlackMagic", sizearray(txtConfigFile));
    #if defined _WIN32
      mkdir(txtConfigFile);
    #else
      mkdir(txtConfigFile, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    #endif
    strlcat(txtConfigFile, DIR_SEPARATOR "bmdebug.ini", sizearray(txtConfigFile));
  }
  #if defined _WIN32
    ini_gets("Settings", "gdb", "arm-none-eabi-gdb.exe", txtGDBpath, sizearray(txtGDBpath), txtConfigFile);
  #else
    ini_gets("Settings", "gdb", "arm-none-eabi-gdb", txtGDBpath, sizearray(txtGDBpath), txtConfigFile);
  #endif
  ini_gets("Settings", "size", "", valstr, sizearray(valstr), txtConfigFile);
  if (sscanf(valstr, "%d %d", &canvas_width, &canvas_height) != 2 || canvas_width < 100 || canvas_height < 50) {
    canvas_width = WINDOW_WIDTH;
    canvas_height = WINDOW_HEIGHT;
  }
  ini_gets("Settings", "splitter", "", valstr, sizearray(valstr), txtConfigFile);
  if (sscanf(valstr, "%f %f", &splitter_hor, &splitter_ver) != 2 || splitter_hor < 0.1 || splitter_ver < 0.1) {
    splitter_hor = 0.75;
    splitter_ver = 0.75;
  }
  for (idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    int opened, size;
    tab_states[idx] = (idx == TAB_SEMIHOSTING || idx == TAB_SWO) ? NK_MINIMIZED : NK_MAXIMIZED;
    tab_heights[idx] = 5 * ROW_HEIGHT;
    sprintf(key, "view%d", idx);
    ini_gets("Settings", key, "", cmd, sizearray(cmd), txtConfigFile);
    result = sscanf(cmd, "%d %d", &opened, &size);
    if (result >= 1)
      tab_states[idx] = opened;
    if (result >= 2 && size > ROW_HEIGHT)
      tab_heights[idx] = size;
  }
  opt_tpwr =(int)ini_getl("Settings", "tpwr", 0, txtConfigFile);
  opt_allmsg = (int)ini_getl("Settings", "allmessages", 0, txtConfigFile);
  opt_autodownload = (int)ini_getl("Settings", "auto-download", 1, txtConfigFile);
  /* read SWO tracing & channel configuration */
  opt_swomode = (unsigned)ini_getl("SWO trace", "mode", SWOMODE_NONE, txtConfigFile);
  opt_swobaud = (unsigned)ini_getl("SWO trace", "bitrate", 100000, txtConfigFile);
  opt_swoclock = (unsigned)ini_getl("SWO trace", "clock", 48000000, txtConfigFile);
  for (idx = 0; idx < NUM_CHANNELS; idx++) {
    char key[40];
    unsigned clr;
    int enabled;
    channel_set(idx, (idx == 0), NULL, nk_rgb(190, 190, 190)); /* preset: port 0 is enabled by default, others disabled by default */
    sprintf(key, "chan%d", idx);
    ini_gets("SWO trace", key, "", cmd, sizearray(cmd), txtConfigFile);
    result = sscanf(cmd, "%d #%x %s", &enabled, &clr, key);
    if (result >= 2)
      channel_set(idx, enabled, (result >= 3) ? key : NULL, nk_rgb(clr >> 16,(clr >> 8) & 0xff, clr & 0xff));
  }

  txtFilename[0] = '\0';
  if (argc >= 2 && access(argv[1], 0) == 0) {
    strlcpy(txtFilename, argv[1], sizearray(txtFilename));
    translate_path(txtFilename, 0);
  } else {
    ini_gets("Session", "recent", "", txtFilename, sizearray(txtFilename), txtConfigFile);
    if (access(txtFilename, 0) != 0)
      txtFilename[0] = '\0';
  }
  assert(strchr(txtFilename, '\\') == 0); /* backslashes should already have been replaced */

  insplitter = SPLITTER_NONE;
  curstate = STATE_INIT;
  prevstate = -1;
  refreshflags = 0;
  console_hiddenflags = opt_allmsg ? 0 : STRFLG_NOTICE | STRFLG_RESULT | STRFLG_EXEC | STRFLG_MI_INPUT | STRFLG_TARGET;
  atprompt = 0;
  console_activate = 1;
  consoleedit_next = NULL;
  cont_is_run = 0;
  warn_source_tstamps = 0;
  source_cursorline = 0;
  source_execfile = -1;
  source_execline = 0;
  prev_clicked_line = -1;
  watchseq = 0;
  trace_status = TRACESTAT_INIT_FAILED;

  ctx = guidriver_init("BlackMagic Debugger", canvas_width, canvas_height, GUIDRV_RESIZEABLE | GUIDRV_TIMER, FONT_HEIGHT);
  set_style(ctx);
  btn_folder = guidriver_image_from_memory(btn_folder_data, btn_folder_datasize);

  while (curstate != STATE_QUIT) {
    /* handle state */
    int waitidle;
    if (!is_idle()) {
      switch (curstate) {
      case STATE_INIT:
        curstate = STATE_GDB_TASK;
        break;
      case STATE_GDB_TASK:
        if (task_launch(txtGDBpath, "--interpreter=mi2", &task)) {
          curstate = STATE_SCAN_BMP; /* GDB started, now find Black Magic Probe */
        } else {
          /* dialog to select GDB */
          #if defined _WIN32
            const char *filter = "Executables\0*.exe\0All files\0*.*\0";
          #else
            const char *filter = "Executables\0*\0All files\0*\0";
          #endif
          const char *s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN, filter,
                                               NULL, txtGDBpath, "Select GDB Executable",
                                               guidriver_apphandle());
          if (s != NULL && strlen(s) < sizearray(txtGDBpath)) {
            strcpy(txtGDBpath, s);
            free((void*)s);
            assert(curstate == STATE_GDB_TASK); /* drop back into this case after */
          } else {
            curstate = STATE_QUIT;  /* selection dialog was canceled, quit the front-end */
          }
        }
        break;
      case STATE_SCAN_BMP:
        if (find_bmp(0, BMP_IF_GDB, port_gdb, sizearray(port_gdb))) {
          if (strncmp(port_gdb, "COM", 3) == 0 && strlen(port_gdb) >= 5) {
            memmove(port_gdb + 4, port_gdb, strlen(port_gdb) + 1);
            memmove(port_gdb, "\\\\.\\", 4);
          }
          curstate = STATE_TARGET_EXT;
        } else if (atprompt) {
          if (prevstate != curstate) {
            console_add("Black Magic Probe not found\n", STRFLG_ERROR);
            prevstate = curstate;
          }
          set_idle_time(1000); /* repeat scan on timeout (but don't sleep the GUI thread) */
        }
        gdbmi_sethandled(0);
        break;
      case STATE_TARGET_EXT:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          sprintf(cmd, "-target-select extended-remote %s\n", port_gdb);
          if (task_stdin(&task, cmd))
            console_input(cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "connected", 9) == 0) {
            curstate = STATE_MON_TPWR;
          } else {
            curstate = STATE_SCAN_BMP;
            set_idle_time(1000); /* drop back to scan (after on timeout) */
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_MON_TPWR:
        if (!opt_tpwr) {
          curstate = STATE_MON_SCAN;
        } else {
          if (!atprompt)
            break;
          if (prevstate != curstate) {
            task_stdin(&task, "monitor tpwr enable\n");
            atprompt = 0;
            prevstate = curstate;
          } else if (gdbmi_isresult() != NULL) {
            if (strncmp(gdbmi_isresult(), "done", 4) == 0)
              curstate = STATE_MON_SCAN;
            else
              set_idle_time(1000); /* stay in scan state, so toggling TPWR works */
            gdbmi_sethandled(0);
          }
        }
        break;
      case STATE_MON_SCAN:
        if (!atprompt)
          break;
        if (prevstate != STATE_MON_SCAN) {
          task_stdin(&task, "monitor swdp_scan\n");
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
            /* save architecture */
            const char *ptr;
            STRINGLIST *item = stringlist_getlast(&consolestring_root, 0, STRFLG_RESULT);
            assert(item != NULL && item->text != NULL);
            ptr = item->text;
            while (*ptr <= ' ' && *ptr != '\0')
              ptr++;
            assert(isdigit(*ptr));
            while (isdigit(*ptr))
              ptr++;
            while (*ptr <= ' ' && *ptr != '\0')
              ptr++;
            assert(*ptr != '\0');
            strlcpy(mcu_family, ptr, sizearray(mcu_family));
            /* split off architecture (if present) */
            mcu_architecture[0] = '\0';
            if ((ptr = strrchr(mcu_family, ' ')) != NULL && ptr[1] == 'M' && isdigit(ptr[2])) {
              *(char*)ptr++ = '\0';
              strlcpy(mcu_architecture, ptr, sizearray(mcu_architecture));
            }
            curstate = STATE_ASYNC_MODE;
          } else {
            set_idle_time(1000); /* stay in scan state, so toggling TPWR works */
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_ASYNC_MODE:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          strcpy(cmd, "-gdb-set target-async 1\n");
          if (task_stdin(&task, cmd))
            console_input(cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          curstate = STATE_ATTACH;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_ATTACH:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          strcpy(cmd, "-target-attach 1\n");
          if (task_stdin(&task, cmd))
            console_input(cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0)
            curstate = STATE_FILE;
          else
            set_idle_time(1000); /* stay in attach state */
          gdbmi_sethandled(0);
        }
        break;
      case STATE_FILE:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          sprintf(cmd, "-file-exec-and-symbols %s\n", txtFilename);
          if (task_stdin(&task, cmd))
            console_input(cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
            curstate = STATE_FILE_TEST;
            source_cursorfile = source_cursorline = 0;
            source_execfile = source_execline = 0;
          } else {
            if (strncmp(gdbmi_isresult(), "error", 5) == 0)
              console_add(gdbmi_isresult(), STRFLG_ERROR);
            set_idle_time(1000); /* stay in attach state */
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_FILE_TEST:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          sources_clear(0);
          source_clear();
          strcpy(cmd, "-file-list-exec-source-files\n");
          if (task_stdin(&task, cmd))
            console_input(cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
            sources_parse(gdbmi_isresult() + 5);  /* skip "done" and comma */
            warn_source_tstamps = !check_sources_tstamps(txtFilename); /* check timestamps of sources against elf file */
            curstate = STATE_MEMACCESS_1;
          } else {
            if (strncmp(gdbmi_isresult(), "error", 5) == 0)
              console_add(gdbmi_isresult(), STRFLG_ERROR);
            set_idle_time(1000); /* stay in attach state */
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_MEMACCESS_1:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          task_stdin(&task, "set mem inaccessible-by-default off\n");
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          curstate = STATE_MEMACCESS_2;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_MEMACCESS_2:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          if (bmscript_line_fmt("memremap", mcu_family, cmd, NULL)) {
            /* run first line from the script */
            task_stdin(&task, cmd);
            atprompt = 0;
            prevstate = curstate;
            if (!opt_allmsg)
              console_hiddenflags |= STRFLG_LOG;
          } else {
            curstate = STATE_VERIFY;
          }
        } else if (gdbmi_isresult() != NULL) {
          /* run next line from the script (on the end of the script, move to
             the next state) */
          if (bmscript_line_fmt(NULL, mcu_family, cmd, NULL)) {
            task_stdin(&task, cmd);
            atprompt = 0;
          } else {
            console_hiddenflags &= ~STRFLG_LOG;
            curstate = STATE_VERIFY;
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_VERIFY:
        if (!opt_autodownload) {
          curstate = STATE_CHECK_MAIN;  /* skip the verification step if download is disabled */
          break;
        }
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          /* check whether the firmware CRC matches the file in GDB */
          //??? for LPC targets, checksum must be corrected in the ELF file first
          task_stdin(&task, "compare-sections\n");
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          STRINGLIST *item;
          gdbmi_sethandled(0); /* first flag the result message as handled, to find the line preceding it */
          item = stringlist_getlast(&consolestring_root, 0, STRFLG_HANDLED);
          assert(item != NULL && item->text != NULL);
          if (strncmp(item->text, "the loaded file", 15) == 0) {
            item->flags |= STRFLG_HANDLED;
            item = stringlist_getlast(&consolestring_root, 0, STRFLG_HANDLED);
          }
          if (strncmp(item->text, "warning:", 8) == 0)
            curstate = STATE_DOWNLOAD;
          else
            curstate = STATE_CHECK_MAIN;
        }
        break;
      case STATE_DOWNLOAD:
        if (!opt_autodownload) {
          curstate = STATE_CHECK_MAIN;  /* skip this step if download is disabled */
          break;
        }
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          task_stdin(&task, "-target-download\n");
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          STRINGLIST *item = stringlist_getlast(&consolestring_root, STRFLG_RESULT, STRFLG_HANDLED);
          assert(item != NULL);
          gdbmi_sethandled(0);
          if (strncmp(item->text, "error", 5) == 0)
            item->flags = (item->flags & ~STRFLG_RESULT) | STRFLG_ERROR;
          curstate = STATE_CHECK_MAIN;
        }
        break;
      case STATE_CHECK_MAIN:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          /* check whether the there is a function "main" */
          //??? use the DWARF information
          task_stdin(&task, "info functions ^main$\n");
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          STRINGLIST *item;
          const char *ptr;
          gdbmi_sethandled(0); /* first flag the result message as handled, to find the line preceding it */
          item = stringlist_getlast(&consolestring_root, 0, STRFLG_HANDLED);
          assert(item != NULL && item->text != NULL);
          ptr = strstr(item->text, "main");
          if (ptr != NULL && (ptr == item->text || *(ptr - 1) == ' ')) {
            curstate = STATE_START;   /* main() found, restart program at main */
          } else {
            check_stopped(&source_execfile, &source_execline);
            source_cursorfile = source_execfile;
            source_cursorline = source_execline;
            curstate = STATE_STOPPED; /* main() not found, stay stopped */
            cont_is_run = 1;            /* but when "Cont" is pressed, "run" is performed */
          }
        }
        break;
      case STATE_START:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          task_stdin(&task, "-break-insert -t main\n");
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          curstate = STATE_EXEC_CMD;
          stateparam[0] = STATEPARAM_EXEC_RESTART;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_EXEC_CMD:
        if (prevstate != curstate) {
          switch (stateparam[0]) {
          case STATEPARAM_EXEC_RESTART:
          case STATEPARAM_EXEC_CONTINUE:
            if (cont_is_run || stateparam[0] == STATEPARAM_EXEC_RESTART) {
              strcpy(cmd, "-exec-run --start\n");
              cont_is_run = 0;  /* do this only once */
            } else {
              strcpy(cmd, "-exec-continue\n");
            }
            break;
          case STATEPARAM_EXEC_STOP:
            strcpy(cmd, "-exec-interrupt\n");
            break;
          case STATEPARAM_EXEC_NEXT:
            strcpy(cmd, "-exec-next\n");
            break;
          case STATEPARAM_EXEC_STEP:
            strcpy(cmd, "-exec-step\n");
            break;
          case STATEPARAM_EXEC_UNTIL:
            sprintf(cmd, "-exec-until %d\n", stateparam[1]);
            break;
          case STATEPARAM_EXEC_FINISH:
            strcpy(cmd, "-exec-finish\n");
            break;
          }
          task_stdin(&task, cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "running", 7) == 0)
            curstate = STATE_RUNNING;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_RUNNING:
        prevstate = curstate;
        if (check_stopped(&source_execfile, &source_execline)) {
          source_cursorfile = source_execfile;
          source_cursorline = source_execline;
          curstate = STATE_STOPPED;
          refreshflags = REFRESH_LOCALS | REFRESH_WATCHES;
        }
        break;
      case STATE_STOPPED:
        if (prevstate != curstate) {
          gdbmi_sethandled(1);
          prevstate = curstate;
        }
        if (refreshflags & REFRESH_BREAKPOINTS)
          curstate = STATE_LIST_BREAKPOINTS;
        else if (refreshflags & REFRESH_WATCHES)
          curstate = STATE_LIST_WATCHES;
        else if (check_running())
          curstate = STATE_RUNNING;
        if (warn_source_tstamps) {
          console_add("Sources have more recent date/time stamps than the target\n", STRFLG_ERROR);
          warn_source_tstamps = 0;
        }
        break;
      case STATE_LIST_BREAKPOINTS:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          task_stdin(&task, "-break-list\n");
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          if (!breakpoint_parse(gdbmi_isresult()) && (refreshflags & REFRESH_CONSOLE)) {
            refreshflags &= ~REFRESH_CONSOLE;
            gdbmi_sethandled(0);
          } else {
            refreshflags &= ~(REFRESH_BREAKPOINTS | REFRESH_CONSOLE);
            curstate = STATE_STOPPED;
            gdbmi_sethandled(1);
          }
        }
        break;
      case STATE_LIST_LOCALS:
        //??? -stack-list-variables (show local variables & function arguments)
        break;
      case STATE_LIST_WATCHES:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          task_stdin(&task, "-var-update --all-values *\n");
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          refreshflags &= ~REFRESH_WATCHES;
          curstate = STATE_STOPPED;
          watch_update(gdbmi_isresult());
          gdbmi_sethandled(0);
        }
        break;
      case STATE_BREAK_TOGGLE:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          switch (stateparam[0]) {
          case STATEPARAM_BP_ENABLE:
            sprintf(cmd, "-break-enable %d\n", stateparam[1]);
            break;
          case STATEPARAM_BP_DISABLE:
            sprintf(cmd, "-break-disable %d\n", stateparam[1]);
            break;
          case STATEPARAM_BP_ADD:
            sprintf(cmd, "-break-insert %s:%d\n", sources_pathlist[stateparam[1]], stateparam[2]);
            break;
          case STATEPARAM_BP_DELETE:
            sprintf(cmd, "-break-delete %d\n", stateparam[1]);
            break;
          default:
            assert(0);
          }
          task_stdin(&task, cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          refreshflags |= REFRESH_BREAKPOINTS;
          curstate = STATE_STOPPED;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_WATCH_TOGGLE:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          switch (stateparam[0]) {
          case STATEPARAM_WATCH_SET:
            sprintf(cmd, "-var-create watch%u * \"%s\"\n", ++watchseq, statesymbol);
            break;
          case STATEPARAM_WATCH_DEL:
            sprintf(cmd, "-var-delete watch%d\n", stateparam[1]);
            break;
          default:
            assert(0);
          }
          task_stdin(&task, cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          const char *ptr = gdbmi_isresult();
          if (strncmp(ptr, "done", 4) == 0) {
            switch (stateparam[0]) {
            case STATEPARAM_WATCH_SET:
              watch_add(skipwhite(ptr + 5), statesymbol);
              break;
            case STATEPARAM_WATCH_DEL:
              watch_del(stateparam[1]);
              break;
            default:
              assert(0);
            }
            refreshflags |= REFRESH_WATCHES;
          }
          curstate = STATE_STOPPED;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_SWOTRACE:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          /* initial setup */
          if (trace_status != TRACESTAT_OK) {
            trace_status = trace_init();
            if (trace_status != TRACESTAT_OK)
              console_add("Failed to initialize SWO tracing\n", STRFLG_ERROR);
          }
          ctf_parse_cleanup();
          ctf_decode_cleanup();
          tracestring_clear();
          tracelog_statusmsg(TRACESTATMSG_CTF, NULL, 0);
          ctf_error_notify(CTFERR_NONE, 0, NULL);
          if (ctf_findmetadata(txtFilename, txtTSDLfile, sizearray(txtTSDLfile))
              && ctf_parse_init(txtTSDLfile) && ctf_parse_run())
          {
            const CTF_STREAM *stream;
            trace_enablectf(1);
            /* stream names overrule configured channel names */
            for (idx = 0; (stream = stream_by_seqnr(idx)) != NULL; idx++)
              if (stream->name != NULL && strlen(stream->name) > 0)
                channel_setname(idx, stream->name);
          } else {
            ctf_parse_cleanup();
          }
          if (opt_swomode == SWOMODE_ASYNC)
            sprintf(cmd, "monitor traceswo %u\n", opt_swobaud); /* automatically select async mode in the BMP */
          else
            strlcpy(cmd, "monitor traceswo\n", sizearray(cmd));
          task_stdin(&task, cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          curstate = STATE_SWODEVICE;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_SWODEVICE:
        if (opt_swomode != SWOMODE_MANCHESTER && opt_swomode != SWOMODE_ASYNC) {
          curstate = STATE_SWOCHANNELS;
          break;
        }
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          if (bmscript_line_fmt("swo-device", mcu_family, cmd, NULL)) {
            /* run first line from the script */
            task_stdin(&task, cmd);
            atprompt = 0;
            prevstate = curstate;
            if (!opt_allmsg)
              console_hiddenflags |= STRFLG_LOG;
          } else {
            curstate = STATE_SWOGENERIC;
          }
        } else if (gdbmi_isresult() != NULL) {
          /* run next line from the script (on the end of the script, move to
             the next state) */
          if (bmscript_line_fmt(NULL, mcu_family, cmd, NULL)) {
            task_stdin(&task, cmd);
            atprompt = 0;
          } else {
            console_hiddenflags &= ~STRFLG_LOG;
            curstate = STATE_SWOGENERIC;
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_SWOGENERIC:
        if (opt_swomode != SWOMODE_MANCHESTER && opt_swomode != SWOMODE_ASYNC) {
          curstate = STATE_SWOCHANNELS;
          break;
        }
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          assert(opt_swobaud > 0);
          scriptparams[0] = (opt_swomode == SWOMODE_MANCHESTER) ? 1 : 2;
          scriptparams[1] = opt_swoclock / opt_swobaud - 1;
          if (bmscript_line_fmt("swo-generic", mcu_family, cmd, scriptparams)) {
            /* run first line from the script */
            task_stdin(&task, cmd);
            atprompt = 0;
            prevstate = curstate;
            if (!opt_allmsg)
              console_hiddenflags |= STRFLG_LOG;
          } else {
            curstate = STATE_SWOCHANNELS;
          }
        } else if (gdbmi_isresult() != NULL) {
          /* run next line from the script (on the end of the script, move to
             the next state) */
          if (bmscript_line_fmt(NULL, mcu_family, cmd, scriptparams)) {
            task_stdin(&task, cmd);
            atprompt = 0;
          } else {
            console_hiddenflags &= ~STRFLG_LOG;
            curstate = STATE_SWOCHANNELS;
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_SWOCHANNELS:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          assert(opt_swobaud > 0);
          scriptparams[0] = 0;
          if (opt_swomode != SWOMODE_NONE) {
            /* if SWO mode is disabled, simply turn all channels off (so skip testing whether they should be on) */
            for (idx = 0; idx < NUM_CHANNELS; idx++)
              if (channel_getenabled(idx))
                scriptparams[0] |= (1 << idx);
          }
          if (bmscript_line_fmt("swo-channels", mcu_family, cmd, scriptparams)) {
            /* run first line from the script */
            task_stdin(&task, cmd);
            atprompt = 0;
            prevstate = curstate;
            if (!opt_allmsg)
              console_hiddenflags |= STRFLG_LOG;
          } else {
            curstate = STATE_STOPPED;
          }
        } else if (gdbmi_isresult() != NULL) {
          /* run next line from the script (on the end of the script, move to
             the next state) */
          if (bmscript_line_fmt(NULL, mcu_family, cmd, scriptparams)) {
            task_stdin(&task, cmd);
            atprompt = 0;
          } else {
            console_hiddenflags &= ~STRFLG_LOG;
            curstate = STATE_STOPPED;
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_HOVER_SYMBOL:
        if (!atprompt)
          break;
        if (strlen(statesymbol) == 0) {
          ttipvalue[0] = '\0';
          curstate = STATE_STOPPED;
          break;
        }
        if (prevstate != curstate) {
          gdbmi_sethandled(1);
          sprintf(cmd, "-data-evaluate-expression %s\n", statesymbol);
          task_stdin(&task, cmd);
          atprompt = 0;
          prevstate = curstate;
          ttipvalue[0] = '\0';
        } else if (gdbmi_isresult() != NULL) {
          const char *head = gdbmi_isresult();
          if (strncmp(head, "done", 4) == 0) {
            const char *tail;
            size_t len;
            assert(*(head + 4) == ',');
            head = skipwhite(head + 5);
            assert(strncmp(head, "value=", 6) == 0);
            head = skipwhite(head + 6);
            assert(*head == '"');
            tail = skip_string(head);
            len = tail - head;
            if (len >= sizearray(ttipvalue))
              len = sizearray(ttipvalue) - 1;
            strncpy(ttipvalue, head, len);
            ttipvalue[len] = '\0';
            format_string(ttipvalue);
          }
          curstate = STATE_STOPPED;
          gdbmi_sethandled(0);
        }
        break;
      } /* switch (curstate) */
    } /* if (!is_idle()) */
    if (curstate > STATE_GDB_TASK && !task_isrunning(&task))
      curstate = STATE_QUIT;  /* GDB ended -> quit the front-end too */

    /* parse GDB output (stderr first, because the prompt is given in stdout) */
    waitidle = 1;
    while (task_stderr(&task, cmd, sizeof cmd) > 0) {
      console_add(cmd, STRFLG_ERROR);
      waitidle = 0;
    }
    while (task_stdout(&task, cmd, sizeof cmd) > 0) {
      int flags = 0;
      if (curstate < STATE_START)
        flags |= STRFLG_STARTUP;
      if (console_add(cmd, flags)) {
        atprompt = 1;
        console_activate = 1;
      }
      waitidle = 0;
    }

    /* handle user input */
    nk_input_begin(ctx);
    if (!guidriver_poll(waitidle)) /* if text was added to the console, don't wait in guidriver_poll(); system is NOT idle */
      break;
    nk_input_end(ctx);

    /* GUI */
    guidriver_appsize(&canvas_width, &canvas_height);
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, canvas_width, canvas_height), NK_WINDOW_NO_SCROLLBAR)) {
      #define SEPARATOR_HOR 4
      #define SEPARATOR_VER 4
      #define SPACING       8
      struct nk_rect rc_canvas = nk_rect(0, 0, canvas_width, canvas_height);
      float splitter_columns[3];
      struct nk_rect bounds;

      splitter_columns[0] = (canvas_width - SEPARATOR_HOR - 2 * SPACING) * splitter_hor;
      splitter_columns[1] = SEPARATOR_HOR;
      splitter_columns[2] = (canvas_width - SEPARATOR_HOR - 2 * SPACING) - splitter_columns[0];
      nk_layout_row(ctx, NK_STATIC, canvas_height - 2 * SPACING, 3, splitter_columns);
      ctx->style.window.padding.x = 2;
      ctx->style.window.padding.y = 2;
      ctx->style.window.group_padding.x = 0;
      ctx->style.window.group_padding.y = 0;

      /* left column */
      if (nk_group_begin(ctx, "left", NK_WINDOW_NO_SCROLLBAR)) {
        float splitter_rows[2];

        splitter_rows[0] = (canvas_height - SEPARATOR_VER - 4 * SPACING) * splitter_ver;
        splitter_rows[1] = (canvas_height - SEPARATOR_VER - 4 * SPACING) - splitter_rows[0];

        /* file browser section */
        nk_layout_row_dynamic(ctx, splitter_rows[0], 1);
        if (nk_group_begin(ctx, "filebrowser", NK_WINDOW_NO_SCROLLBAR|NK_WINDOW_BORDER)) {
          float combo_width;
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 7);
          nk_layout_row_push(ctx, 45);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "reset"))
            curstate = STATE_FILE;
          tooltip(ctx, bounds, " Reload and restart the program", &rc_canvas);
          nk_layout_row_push(ctx, 45);
          bounds = nk_widget_bounds(ctx);
          if (curstate == STATE_RUNNING) {
            if (nk_button_label(ctx, "stop") || nk_input_is_key_pressed(&ctx->input, NK_KEY_CTRL_F5)) {
              prevstate = -1;
              curstate = STATE_EXEC_CMD;
              stateparam[0] = STATEPARAM_EXEC_STOP;
            }
            tooltip(ctx, bounds, " Interrupt the program (Ctrl+F5)", &rc_canvas);
          } else {
            if (nk_button_label(ctx, "cont") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F5)) {
              curstate = STATE_EXEC_CMD;
              stateparam[0] = STATEPARAM_EXEC_CONTINUE;
            }
            tooltip(ctx, bounds, " Continue running (F5)", &rc_canvas);
          }
          nk_layout_row_push(ctx, 45);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "next") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F10)) {
            curstate = STATE_EXEC_CMD;
            stateparam[0] = STATEPARAM_EXEC_NEXT;
          }
          tooltip(ctx, bounds, " Step over (F10)", &rc_canvas);
          nk_layout_row_push(ctx, 45);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "step") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F11)) {
            curstate = STATE_EXEC_CMD;
            stateparam[0] = STATEPARAM_EXEC_STEP;
          }
          tooltip(ctx, bounds, " Step into (F11)", &rc_canvas);
          nk_layout_row_push(ctx, 45);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "finish") || nk_input_is_key_pressed(&ctx->input, NK_KEY_SHIFT_F11)) {
            curstate = STATE_EXEC_CMD;
            stateparam[0] = STATEPARAM_EXEC_FINISH;
          }
          tooltip(ctx, bounds, " Step out of function (Shift+F11)", &rc_canvas);
          nk_layout_row_push(ctx, 45);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "until") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F7)) {
            curstate = STATE_EXEC_CMD;
            stateparam[0] = STATEPARAM_EXEC_UNTIL;
            stateparam[1] = source_cursorline;
          }
          tooltip(ctx, bounds, " Run until cursor (F7)", &rc_canvas);
          combo_width = splitter_columns[0] - 6 * (45 + 5);
          nk_layout_row_push(ctx, combo_width);
          if (sources_count > 0) {
            int curfile = source_cursorfile;
            if (curfile < 0 || (unsigned)curfile >= sources_count)
              curfile = 0;
            source_cursorfile = nk_combo(ctx, (const char**)sources_namelist, sources_count, curfile, (int)COMBOROW_CY, nk_vec2(combo_width, 10*ROW_HEIGHT));
            if (source_cursorfile != curfile)
              source_cursorline = 1;  /* reset scroll */
          }
          nk_layout_row_end(ctx);
          if (source_load(source_cursorfile)) { /* does nothing if the current source file is already loaded */
            int count = source_linecount();
            if (source_cursorline > count)
              source_cursorline = count;
          }
          nk_layout_row_dynamic(ctx, splitter_rows[0] - ROW_HEIGHT - 4, 1);
          bounds = nk_widget_bounds(ctx);
          source_widget(ctx, "source", FONT_HEIGHT);
          if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, bounds)) {
            int row, col;
            source_mouse2char(ctx, "source", FONT_HEIGHT, bounds, &row, &col);
            if (col == 0) {
              /* click in the margin: set/clear/enable/disable breakpoint
                 - if there is no breakpoint on this line -> add a breakpoint
                 - if there is an enabled breakpoint on this line -> disable it
                 - if there is a disabled breakpoint on this line -> check
                   whether the current line is the same as the one previously
                   clicked on; if yes -> delete; if no: -> enable */
              BREAKPOINT *bp = breakpoint_lookup(source_cursorfile, row);
              if (bp == NULL) {
                /* no breakpoint yet -> add */
                curstate = STATE_BREAK_TOGGLE;
                stateparam[0] = STATEPARAM_BP_ADD;
                stateparam[1] = source_cursorfile;
                stateparam[2] = row;
              } else if (bp->enabled) {
                /* enabled breakpoint -> disable */
                curstate = STATE_BREAK_TOGGLE;
                stateparam[0] = STATEPARAM_BP_DISABLE;
                stateparam[1] = bp->number;
              } else if (prev_clicked_line != row) {
                /* disabled breakpoint & not a double click -> enable */
                curstate = STATE_BREAK_TOGGLE;
                stateparam[0] = STATEPARAM_BP_ENABLE;
                stateparam[1] = bp->number;
              } else {
                /* disabled breakpoint & double click -> enable */
                curstate = STATE_BREAK_TOGGLE;
                stateparam[0] = STATEPARAM_BP_DELETE;
                stateparam[1] = bp->number;
              }
            } else {
              /* set the cursor line */
              if (row > 0 && row <= source_linecount())
                source_cursorline = row;
            }
            prev_clicked_line = row;
          } else if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds)) {
            int row, col;
            char sym[64];
            source_mouse2char(ctx, "source", FONT_HEIGHT, bounds, &row, &col);
            if (row != prev_clicked_line)
              prev_clicked_line = -1; /* mouse leaves the line clicked on, erase "repeat click" */
            if (source_getsymbol(sym, sizearray(sym), row, col)) {
              /* we are hovering over a symbol; if the symbol hovered over is
                 different from the previous one -> clear the tooltip value */
              if (strcmp(sym, statesymbol) != 0) {
                ttipvalue[0] = '\0';
                strlcpy(statesymbol, sym, sizearray(statesymbol));
                /* if the new symbol is valid -> curstate = STATE_HOVER_SYMBOL */
                if (statesymbol[0] != '\0' && curstate == STATE_STOPPED)
                  curstate = STATE_HOVER_SYMBOL;
              }
              /* if the tooltip value is valid, show it */
              if (ttipvalue[0] != '\0')
                nk_tooltip(ctx, ttipvalue, NULL);
            } else {
              ttipvalue[0] = '\0';
            }
          }
          nk_group_end(ctx);
        }

        /* vertical splitter */
        nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
        bounds = nk_widget_bounds(ctx);
        nk_label(ctx, "\xe2\x80\xa2 \xe2\x80\xa2 \xe2\x80\xa2", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE); /* \xe2\x88\x99\xe2\x88\x99\xe2\x88\x99 */
        if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
          insplitter = SPLITTER_VERTICAL; /* in vertical splitter */
        else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
          insplitter = SPLITTER_NONE;
        if (insplitter == SPLITTER_VERTICAL)
          splitter_ver = (splitter_rows[0] + ctx->input.mouse.delta.y) / (canvas_height - SEPARATOR_HOR - 4 * SPACING);

        /* console space */
        nk_layout_row_dynamic(ctx, splitter_rows[1], 1);
        if (nk_group_begin(ctx, "console", NK_WINDOW_NO_SCROLLBAR|NK_WINDOW_BORDER)) {
          nk_layout_row_dynamic(ctx, splitter_rows[1] - ROW_HEIGHT - SPACING, 1);
          console_widget(ctx, "console-out", FONT_HEIGHT);
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          if (curstate < STATE_START && curstate != STATE_SCAN_BMP) {
            /* while initializing, say "please wait" */
            strlcpy(console_edit, "Please wait...", sizearray(console_edit));
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_READ_ONLY, console_edit, sizearray(console_edit), nk_filter_ascii);
            console_edit[0] = '\0';
          } else {
            /* true edit line */
            if (console_activate) {
              nk_edit_focus(ctx, (console_activate == 2) ? NK_EDIT_GOTO_END_ON_ACTIVATE : 0);
              console_activate = 1;
            }
            result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, console_edit, sizearray(console_edit), nk_filter_ascii);
            if (result & NK_EDIT_COMMITED) {
              /* some commands are handled internally */
              if (handle_display_cmd(console_edit, stateparam, statesymbol, sizearray(statesymbol))) {
                curstate = STATE_WATCH_TOGGLE;
                tab_states[TAB_WATCHES] = nk_true; /* make sure the watch view to open */
              } else if ((result = handle_trace_cmd(console_edit, &opt_swomode, &opt_swoclock, &opt_swobaud)) != 0) {
                if (result == 1) {
                  curstate = STATE_SWOTRACE;
                } else if (result == 2) {
                  curstate = STATE_SWOCHANNELS;
                } else if (result == 3) {
                  trace_info_mode(opt_swomode, opt_swoclock, opt_swobaud);
                  if (opt_swomode != SWOMODE_NONE) {
                    int chan;
                    for (chan = 0; chan < NUM_CHANNELS; chan++)
                      trace_info_channel(chan, 1);
                  }
                }
                tab_states[TAB_SWO] = nk_true;  /* make sure the SWO tracing view is open */
              } else if (!handle_list_cmd(console_edit) && !handle_find_cmd(console_edit)) {
                strlcat(console_edit, "\n", sizearray(console_edit));
                if (task_stdin(&task, console_edit))
                  console_input(console_edit);
              }
              /* check for a list of breakpoint commands, so that we can refresh
                 the breakpoint list after the command executed */
              if (strncmp(console_edit, "b ", 2) == 0 ||
                  strncmp(console_edit, "break ", 6) == 0 ||
                  strncmp(console_edit, "watch ", 6) == 0 ||
                  strncmp(console_edit, "del ", 4) == 0 ||
                  strncmp(console_edit, "delete ", 7) == 0 ||
                  strncmp(console_edit, "clear ", 6) == 0 ||
                  strncmp(console_edit, "disable ", 8) == 0 ||
                  strncmp(console_edit, "enable ", 7) == 0)
                refreshflags |= REFRESH_BREAKPOINTS | REFRESH_CONSOLE;
              /* save console_edit in a recent command list */
              stringlist_add_head(&consoleedit_root, console_edit, 0);
              consoleedit_next = NULL;
              console_edit[0] = '\0';
            }
          } /* if (curstate < STATE_START) */
          nk_group_end(ctx);
        }
        nk_group_end(ctx);
      }

      /* column splitter */
      bounds = nk_widget_bounds(ctx);
      nk_label(ctx, "\xe2\x8b\xae", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE);
      if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
        insplitter = SPLITTER_HORIZONTAL; /* in horizontal splitter */
      else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
        insplitter = SPLITTER_NONE;
      if (insplitter == SPLITTER_HORIZONTAL)
        splitter_hor = (splitter_columns[0] + ctx->input.mouse.delta.x) / (canvas_width - SEPARATOR_HOR - 2 * SPACING);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", &tab_states[TAB_CONFIGURATION])) {
          float edtwidth;
          char basename[256], *p;
          bounds = nk_widget_bounds(ctx);
          edtwidth = bounds.w - 65;
          /* GDB */
          p = strrchr(txtGDBpath, DIRSEP_CHAR);
          strlcpy(basename,(p == NULL)? txtGDBpath : p + 1, sizearray(basename));
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
          nk_layout_row_push(ctx, 30);
          nk_label(ctx, "GDB", NK_TEXT_LEFT);
          nk_layout_row_push(ctx, edtwidth);
          bounds = nk_widget_bounds(ctx);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_READ_ONLY, basename, sizearray(basename), nk_filter_ascii);
          tooltip(ctx, bounds, txtGDBpath, &rc_canvas);
          nk_layout_row_push(ctx, 25);
          if (nk_button_image(ctx, btn_folder)) {
            const char *s;
            s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                                     "ELF Executables\0*.elf;*.bin;*.\0All files\0*.*\0",
                                     NULL, txtGDBpath, "Select ELF Executable",
                                     guidriver_apphandle());
            if (s != NULL && strlen(s) < sizearray(txtGDBpath)) {
              strcpy(txtGDBpath, s);
              free((void*)s);
              task_close(&task);  /* terminate running instance of GDB */
              curstate = STATE_INIT;
            }
          }
          nk_layout_row_end(ctx);
          /* target */
          p = strrchr(txtFilename, '/');
          strlcpy(basename, (p == NULL) ? txtFilename : p + 1, sizearray(basename));
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
          nk_layout_row_push(ctx, 30);
          nk_label(ctx, "File", NK_TEXT_LEFT);
          nk_layout_row_push(ctx, edtwidth);
          bounds = nk_widget_bounds(ctx);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_READ_ONLY, basename, sizearray(basename), nk_filter_ascii);
          tooltip(ctx, bounds, txtFilename, &rc_canvas);
          nk_layout_row_push(ctx, 25);
          if (nk_button_image(ctx, btn_folder)) {
            const char *s;
            translate_path(txtFilename, 1);
            s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                                     "ELF Executables\0*.elf;*.bin;*.\0All files\0*.*\0",
                                     NULL, txtFilename, "Select ELF Executable",
                                     guidriver_apphandle());
            if (s != NULL && strlen(s) < sizearray(txtFilename)) {
              strcpy(txtFilename, s);
              translate_path(txtFilename, 0);
              free((void*)s);
              if (curstate > STATE_FILE)
                curstate = STATE_FILE;
            }
          }
          nk_layout_row_end(ctx);
          /* source directory */
          //??? -environment-directory -r path "path" ...
          /* TPWR option */
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          if (nk_checkbox_label(ctx, "Power Target (3.3V)", &opt_tpwr)) {
            if (!opt_tpwr)
              task_stdin(&task, "monitor tpwr disable\n");
            if (opt_tpwr && curstate != STATE_MON_SCAN)
              task_stdin(&task, "monitor tpwr enable\n");
            if (curstate == STATE_MON_SCAN)
               curstate = STATE_MON_TPWR;
          }
          /* auto-download */
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          nk_checkbox_label(ctx, "Download to target on mismatch", &opt_autodownload);
          /* show all GDB output */
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          if (nk_checkbox_label(ctx, "Show all GDB messages", &opt_allmsg))
            console_hiddenflags = opt_allmsg ? 0 : STRFLG_NOTICE | STRFLG_RESULT | STRFLG_EXEC | STRFLG_MI_INPUT | STRFLG_TARGET;
          nk_tree_state_pop(ctx);
        } /* config */

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Breakpoints", &tab_states[TAB_BREAKPOINTS])) {
          BREAKPOINT *bp;
          char label[300];
          float width = 0;
          struct nk_user_font const *font = ctx->style.font;
          /* find longest breakpoint description */
          for (bp = breakpoint_root.next; bp != NULL; bp = bp->next) {
            float w;
            if (bp->flags & BKPTFLG_FUNCTION) {
              assert(bp->name != NULL);
              strlcpy(label, bp->name, sizearray(label));
            } else {
              assert((unsigned)bp->filenr < sources_count);
              sprintf(label, "%s : %d", sources_namelist[bp->filenr], bp->linenr);
            }
            w = font->width(font->userdata, font->height, label, strlen(label)) + 10;
            if (w > width)
              width = w;
          }
          for (bp = breakpoint_root.next; bp != NULL; bp = bp->next) {
            int en;
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
            nk_layout_row_push(ctx, 30);
            sprintf(label, "%d", bp->number);
            en = bp->enabled;
            if (nk_checkbox_label(ctx, label, &en)) {
              curstate = STATE_BREAK_TOGGLE;
              stateparam[0] = (en == 0) ? STATEPARAM_BP_DISABLE : STATEPARAM_BP_ENABLE;
              stateparam[1] = bp->number;
            }
            nk_layout_row_push(ctx, width);
            if (bp->flags & BKPTFLG_FUNCTION) {
              assert(bp->name != NULL);
              strlcpy(label, bp->name, sizearray(label));
            } else {
              assert((unsigned)bp->filenr < sources_count);
              sprintf(label, "%s : %d", sources_namelist[bp->filenr], bp->linenr);
            }
            nk_label(ctx, label, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, ROW_HEIGHT);
            if (nk_button_symbol(ctx, NK_SYMBOL_X)) {
              curstate = STATE_BREAK_TOGGLE;
              stateparam[0] = STATEPARAM_BP_DELETE;
              stateparam[1] = bp->number;
            }
          }
          if (width == 0) {
            /* this means that there are no breakpoints */
            nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
            nk_label(ctx, "No breakpoints", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE);
          }
          nk_tree_state_pop(ctx);
        } /* breakpoints */

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Watches", &tab_states[TAB_WATCHES])) {
          WATCH *watch;
          float namewidth, valwidth, w;
          char label[20];
          struct nk_user_font const *font = ctx->style.font;
          /* find longest watch expression and value */
          namewidth = 0;
          valwidth = 2 * ROW_HEIGHT;
          for (watch = watch_root.next; watch != NULL; watch = watch->next) {
            assert(watch->expr != NULL);
            w = font->width(font->userdata, font->height, watch->expr, strlen(watch->expr)) + 10;
            if (w > namewidth)
              namewidth = w;
            if (watch->value != NULL) {
              w = font->width(font->userdata, font->height, watch->value, strlen(watch->value)) + 10;
              if (w > valwidth)
                valwidth = w;
            }
          }
          for (watch = watch_root.next; watch != NULL; watch = watch->next) {
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 4);
            nk_layout_row_push(ctx, 30);
            sprintf(label, "%u", watch->seqnr); /* print sequence number for undisplay command */
            nk_label(ctx, label, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, namewidth);
            nk_label(ctx, watch->expr, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, valwidth);
            if (watch->value != NULL) {
              if (watch->flags & WATCHFLG_CHANGED)
                nk_label_colored(ctx, watch->value, NK_TEXT_LEFT, nk_rgb(255, 100, 128));
              else
                nk_label(ctx, watch->value, NK_TEXT_LEFT);
            } else {
              nk_label(ctx, "?", NK_TEXT_LEFT);
            }
            nk_layout_row_push(ctx, ROW_HEIGHT);
            if (nk_button_symbol(ctx, NK_SYMBOL_X)) {
              curstate = STATE_WATCH_TOGGLE;
              stateparam[0] = STATEPARAM_WATCH_DEL;
              stateparam[1] = watch->seqnr;
            }
          }
          if (namewidth <= 0.1) {
            /* this means there are no watches */
            nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
            nk_label(ctx, "No watches", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE);
          }
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
          nk_layout_row_push(ctx, 30);
          w = namewidth + valwidth + ctx->style.window.spacing.x;
          if (w < 150)
            w = 150;
          nk_layout_row_push(ctx, w);
          result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, watch_edit, sizearray(watch_edit), nk_filter_ascii);
          nk_layout_row_push(ctx, ROW_HEIGHT);
          if ((nk_button_symbol(ctx, NK_SYMBOL_PLUS) || (result & NK_EDIT_COMMITED)) && curstate == STATE_STOPPED && strlen(watch_edit) > 0) {
            curstate = STATE_WATCH_TOGGLE;
            stateparam[0] = STATEPARAM_WATCH_SET;
            strlcpy(statesymbol, watch_edit, sizearray(statesymbol));
            watch_edit[0] = '\0';
          } else if (result & NK_EDIT_ACTIVATED) {
            console_activate = 0;
          }
          nk_tree_state_pop(ctx);
        } /* watches */

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Semihosting output", &tab_states[TAB_SEMIHOSTING])) {
          nk_layout_row_dynamic(ctx, tab_heights[TAB_SEMIHOSTING], 1);
          nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, nk_rgba(20, 29, 38, 225));
          if (nk_group_begin_titled(ctx, "semihosting", "", 0)) {
            STRINGLIST *item;
            for (item = semihosting_root.next; item != NULL; item = item->next) {
              nk_layout_row_dynamic(ctx, FONT_HEIGHT, 1);
              nk_label(ctx, item->text, NK_TEXT_LEFT);
            }
            nk_group_end(ctx);
          }
          nk_style_pop_color(ctx);
          /* make view height resizeable */
          nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
          bounds = nk_widget_bounds(ctx);
          nk_label(ctx, "\xe2\x80\xa2 \xe2\x80\xa2 \xe2\x80\xa2", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE); /* \xe2\x88\x99\xe2\x88\x99\xe2\x88\x99 */
          if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
            insplitter = SIZER_SEMIHOSTING; /* in semihosting sizer */
          else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
            insplitter = SPLITTER_NONE;
          if (insplitter == SIZER_SEMIHOSTING) {
            tab_heights[TAB_SEMIHOSTING] += ctx->input.mouse.delta.y;
            if (tab_heights[TAB_SEMIHOSTING] < ROW_HEIGHT)
              tab_heights[TAB_SEMIHOSTING] = ROW_HEIGHT;
          }
          nk_tree_state_pop(ctx);
        } /* semihosting (Target output) */

        if (nk_tree_state_push(ctx, NK_TREE_TAB, "SWO tracing", &tab_states[TAB_SWO])) {
          tracestring_process(trace_status == TRACESTAT_OK);
          nk_layout_row_dynamic(ctx, tab_heights[TAB_SWO], 1);
          tracelog_widget(ctx, "tracelog", FONT_HEIGHT, -1, 0);
          /* make view height resizeable */
          nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
          bounds = nk_widget_bounds(ctx);
          nk_label(ctx, "\xe2\x80\xa2 \xe2\x80\xa2 \xe2\x80\xa2", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE); /* \xe2\x88\x99\xe2\x88\x99\xe2\x88\x99 */
          if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
            insplitter = SIZER_SWO; /* in swo sizer */
          else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
            insplitter = SPLITTER_NONE;
          if (insplitter == SIZER_SWO) {
            tab_heights[TAB_SWO] += ctx->input.mouse.delta.y;
            if (tab_heights[TAB_SWO] < ROW_HEIGHT)
              tab_heights[TAB_SWO] = ROW_HEIGHT;
          }
          nk_tree_state_pop(ctx);
        } /* SWO tracing */

        nk_group_end(ctx);
      } /* right column */

      /* keyboard input */
      if (nk_input_is_key_pressed(&ctx->input, NK_KEY_UP) && source_cursorline > 1) {
        source_cursorline--;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN) && source_cursorline < source_linecount()) {
        source_cursorline++;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_UP)) {
        source_cursorline -= source_vp_rows;
        if (source_cursorline < 1)
          source_cursorline = 1;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_DOWN)) {
        source_cursorline += source_vp_rows;
        if (source_cursorline > source_linecount())
          source_cursorline = source_linecount();
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_TOP)) {
        source_cursorline = 1;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_BOTTOM)) {
        source_cursorline = source_linecount();
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_FIND)) {
        strlcpy(console_edit, "find ", sizearray(console_edit));
        console_activate = 2;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F3)) {
        handle_find_cmd("find");  /* find next */
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_REFRESH)) {
        consoleedit_next = (consoleedit_next == NULL || consoleedit_next->next == NULL) ? consoleedit_root.next : consoleedit_next->next;
        if (consoleedit_next != NULL) {
          strlcpy(console_edit, consoleedit_next->text, sizearray(console_edit));
          console_activate = 2;
        }
      }

    } /* window */

    nk_end(ctx);

    /* Draw */
    guidriver_render(nk_rgb(30,30,30));
  }
  exitcode = task_close(&task);

  /* save settings */
  ini_puts("Settings", "gdb", txtGDBpath, txtConfigFile);
  sprintf(valstr, "%d %d", canvas_width, canvas_height);
  ini_puts("Settings", "size", valstr, txtConfigFile);
  sprintf(valstr, "%.2f %.2f", splitter_hor, splitter_ver);
  ini_puts("Settings", "splitter", valstr, txtConfigFile);
  for (idx = 0; idx < TAB_COUNT; idx++) {
    char key[40];
    sprintf(key, "view%d", idx);
    sprintf(valstr, "%d %d", tab_states[idx], (int)tab_heights[idx]);
    ini_puts("Settings", key, valstr, txtConfigFile);
  }
  ini_putl("Settings", "tpwr", opt_tpwr, txtConfigFile);
  ini_putl("Settings", "allmessages", opt_allmsg, txtConfigFile);
  ini_putl("Settings", "auto-download", opt_autodownload, txtConfigFile);
  ini_puts("Session", "recent", txtFilename, txtConfigFile);
  ini_putl("SWO trace", "mode", opt_swomode, txtConfigFile);
  ini_putl("SWO trace", "bitrate", opt_swobaud, txtConfigFile);
  ini_putl("SWO trace", "clock", opt_swoclock, txtConfigFile);
  for (idx = 0; idx < NUM_CHANNELS; idx++) {
    char key[32];
    struct nk_color color = channel_getcolor(idx);
    sprintf(key, "chan%d", idx);
    sprintf(cmd, "%d #%06x %s", channel_getenabled(idx),
            ((int)color.r << 16) | ((int)color.g << 8) | color.b,
            channel_getname(idx, NULL, 0));
    ini_puts("SWO trace", key, cmd, txtConfigFile);
  }

  guidriver_close();
  stringlist_clear(&consolestring_root);
  stringlist_clear(&consoleedit_root);
  console_clear();
  sources_clear(1);
  source_clear();
  return exitcode;
}
