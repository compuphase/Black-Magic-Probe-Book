/*
 * GDB front-end with specific support for the Black Magic Probe.
 * This utility is built with Nuklear for a cross-platform GUI.
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
# include <shellapi.h>
# include <direct.h>
# include <io.h>
# include <malloc.h>
# if defined __MINGW32__ || defined __MINGW64__
#   include <dirent.h>
#   include <sys/stat.h>
#   include "strlcpy.h"
# elif defined _MSC_VER
#   include "strlcpy.h"
#   include <sys/stat.h>
#   include "dirent.h"
#   if _MSC_VER < 1900
#     include "c99_snprintf.h"
#   endif
#   define stat _stat
#   define access(p,m)       _access((p),(m))
#   define memicmp(p1,p2,c)  _memicmp((p1),(p2),(c))
#   define mkdir(p)          _mkdir(p)
#   define strdup(s)         _strdup(s)
#   define stricmp(s1,s2)    _stricmp((s1),(s2))
#   define strnicmp(s1,s2,c) _strnicmp((s1),(s2),(c))
#   define strupr(s)         _strupr(s)
# endif
#elif defined __linux__
# include <alloca.h>
# include <dirent.h>
# include <poll.h>
# include <signal.h>
# include <unistd.h>
# include <bsd/string.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/types.h>
# include <sys/wait.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "armdisasm.h"
#include "bmcommon.h"
#include "bmp-scan.h"
#include "bmp-script.h"
#include "demangle.h"
#include "dwarf.h"
#include "elf.h"
#include "guidriver.h"
#include "mcu-info.h"
#include "memdump.h"
#include "minIni.h"
#include "nuklear_mousepointer.h"
#include "nuklear_style.h"
#include "nuklear_splitter.h"
#include "nuklear_tooltip.h"
#include "osdialog.h"
#include "pathsearch.h"
#include "serialmon.h"
#include "specialfolder.h"
#include "svd-support.h"
#include "tcpip.h"
#include "svnrev.h"

#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __linux__ || defined __unix__
# include "res/icon_debug_64.h"
#endif

#if !defined _MAX_PATH
# define _MAX_PATH 260
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
# define stricmp(s1,s2)    strcasecmp((s1),(s2))
# define strnicmp(s1,s2,n) strncasecmp((s1),(s2),(n))
# define min(a,b)          ( ((a) < (b)) ? (a) : (b) )
#endif
#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if defined WIN32 || defined _WIN32
# define DIRSEP_CHAR '\\'
# define DIRSEP_STR  "\\"
# define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
# define DIRSEP_CHAR '/'
# define DIRSEP_STR  "/"
# define IS_OPTION(s)  ((s)[0] == '-')
#endif


static const char *lastdirsep(const char *path);
static char *translate_path(char *path, bool native);
static const char *source_getname(unsigned idx);
static time_t file_timestamp(const char *path);

static DWARF_LINETABLE dwarf_linetable = { NULL };
static DWARF_SYMBOLLIST dwarf_symboltable = { NULL};
static DWARF_PATHLIST dwarf_filetable = { NULL};


#define STRFLG_INPUT    0x0001  /* stdin echo */
#define STRFLG_ERROR    0x0002  /* stderr */
#define STRFLG_RESULT   0x0004  /* '^' */
#define STRFLG_EXEC     0x0008  /* '*' */
#define STRFLG_STATUS   0x0010  /* '+' */
#define STRFLG_NOTICE   0x0020  /* '=' */
#define STRFLG_LOG      0x0040  /* '&' */
#define STRFLG_TARGET   0x0080  /* '@' */
#define STRFLG_MI_INPUT 0x0100  /* '-' */
#define STRFLG_SCRIPT   0x0200  /* log output from script */
#define STRFLG_MON_OUT  0x0400  /* monitor echo */
#define STRFLG_NO_EOL   0x2000  /* output string not terminated with \n */
#define STRFLG_STARTUP  0x4000
#define STRFLG_HANDLED  0x8000

typedef struct tagSTRINGLIST {
  struct tagSTRINGLIST *next;
  char *text;
  unsigned short flags;
} STRINGLIST;

typedef struct tagSWOSETTINGS {
  unsigned mode;
  unsigned bitrate;
  unsigned clock;
  unsigned datasize;
  char metadata[_MAX_PATH];
  int force_plain;
  int enabled;
  int init_status;
} SWOSETTINGS;

enum { SWOMODE_NONE, SWOMODE_MANCHESTER, SWOMODE_ASYNC };


static const char *gdbmi_leader(char *buffer, int *flags, char **next_segment);
static void trace_info_mode(const SWOSETTINGS *swo, int showchannels, STRINGLIST *textroot);
static void serial_info_mode(STRINGLIST *textroot);
static void source_getcursorpos(int *fileindex, int *linenumber);


/** stringlist_append() adds a string to the tail of the list. */
static STRINGLIST *stringlist_append(STRINGLIST *root, const char *text, int flags)
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
    {}
  tail->next = item;
  return item;
}

/** stringlist_insert() inserts a string after the passed-in pointer. Pass
 *  in "root" to insert a string at ad the head of the list.
 */
static STRINGLIST *stringlist_insert(STRINGLIST *listpos, const char *text, int flags)
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

  assert(listpos != NULL);
  item->next = listpos->next;
  listpos->next = item;
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

static unsigned stringlist_count(STRINGLIST *root)
{
  STRINGLIST *item;
  unsigned count = 0;
  for (item = root->next; item != NULL; item = item->next)
    count++;
  return count;
}

/** stringlist_getlast() returns the last string that has the "include" flags
 *  all set and that has none of the "exclude" flags set. So setting include to
 *  STRFLG_RESULT and exclude to STRFLG_HANDLED returns the last result message
 *  that is not handled, and include and exclude to 0 simply returns the very
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

static const char *skipwhite(const char *text)
{
  assert(text != NULL);
  while (*text != '\0' && *text <= ' ')
    text++;
  return text;
}

static char *striptrailing(char *text)
{
  assert(text != NULL);
  char *ptr = text + strlen(text);
  while (ptr > text && *(ptr - 1) <= ' ')
    *--ptr = '\0'; /* strip trailing whitespace */
  return text;
}

static const char *strchr_nest(const char *text, char match)
{
  char ch_nest = '\0';
  switch (match) {
  case ')': ch_nest = '('; break;
  case ']': ch_nest = '['; break;
  case '}': ch_nest = '{'; break;
  case '>': ch_nest = '<'; break;
  }
  int level = 0;
  while (*text != '\0') {
    if (*text == match) {
      if (--level < 0)
        return text;
    } else if (*text == ch_nest) {
      level++;
    }
    text++;
  }
  return NULL;
}

static int strtokenize(const char *token, int *length, char delimiter)
{
  assert(token != NULL);
  assert(length != NULL);
  const char *p = strchr(token, delimiter);
  *length = (p != NULL) ? (int)(p - token) : strlen(token);
  return p != NULL;
}

static int helptext_add(STRINGLIST *root, char *text, int reformat)
{
  char *start, *linebuffer;
  unsigned linebuf_size = 1024;
  int xtraflags = 0;

  linebuffer = malloc(linebuf_size * sizeof(char));
  if (linebuffer == NULL)
    return 0;
  *linebuffer = '\0';

  assert(text != NULL);
  start = (char*)skipwhite(text);
  while (start != NULL && *start != '\0') {
    if (*start == '^') {
      /* end of the standard console output, remove any text already handled and quit */
      if (start != text)
        memmove(text, start, strlen(start)+ 1);
      free((void*)linebuffer);
      return 0;
    }

    /* handle only standard console output, ignore any "log" strings */
    if (*start == '~' || *start == '@') {
      const char *ptr = gdbmi_leader(start, &xtraflags, &start);
      /* after gdbmi_leader(), there may again be '\n' characters in the resulting string */
      const char *tok = ptr;
      int toklen, tokresult;
      do {
        tokresult = strtokenize(tok, &toklen, '\n');
        if (toklen > 0) {
          size_t len = strlen(linebuffer);
          if (len + toklen + 2 >= linebuf_size) {
            /* grow buffer size */
            linebuf_size *= 2;
            char *newbuffer = malloc(linebuf_size * sizeof(char));
            if (newbuffer != NULL) {
              strcpy(newbuffer, linebuffer);
              free((void*)linebuffer);
              linebuffer = newbuffer;
            }
          }
          if (len + toklen + 2 < linebuf_size) {
            memcpy(linebuffer + len, tok, toklen);
            linebuffer[len + toklen] = '\0';
          }
        }
        if (tokresult) {
          /* reformat using some heuristics */
          nk_bool linebreak = nk_false;
          int len = strlen(linebuffer);
          if (!reformat
              || len == 0                                                 /* line break for fully empty strings */
              || linebuffer[len - 1] == '.' || linebuffer[len - 1] == ':' /* line break if phrase ends with a period or colon */
              || strpbrk(linebuffer, "[|]") != NULL                       /* line break if special characters appear in the string */
              || stringlist_count(root) < 2)                              /* never concatenate the first two lines */
            linebreak = nk_true;
          if (linebreak) {
            stringlist_append(root, linebuffer, xtraflags);
            *linebuffer = '\0';
          } else {
            /* more text likely follows, add a space character */
            strlcat(linebuffer, " ", linebuf_size);
          }
          tok += toklen + 1; /* skip delimiter */
        }
      } while (tokresult);
    } else {
      gdbmi_leader(start, &xtraflags, &start);
    }
    if (start != NULL)
      start = (char*)skipwhite(start);
  }

  if (strlen(linebuffer) > 0)
    stringlist_append(root, linebuffer, xtraflags);

  free((void*)linebuffer);
  return 1;
}

static void semihosting_add(STRINGLIST *root, const char *text, unsigned short flags)
{
  STRINGLIST *item;

  assert(root != NULL);
  /* get the last string ends, for checking whether it ended with a newline */
  for (item = root; item->next != NULL; item = item->next)
    {}

  assert(text != NULL);
  while (*text != '\0') {
    char *buffer;
    size_t buflen;
    const char *tail = strchr(text, '\n');
    if (tail == NULL)
      tail = strchr(text, '\0');
    buflen = (tail - text);
    if (buflen > 0) {
      assert(item != NULL && item->next == NULL);
      if (item != root && (item->flags & STRFLG_HANDLED) == 0) {
        /* concatenate the string to the tail */
        size_t curlen;
        assert(item->text != NULL);
        curlen = strlen(item->text);
        buffer = (char*)malloc((curlen + buflen + 1) * sizeof(char));
        if (buffer != NULL) {
          strcpy(buffer, item->text);
          strncpy(buffer + curlen, text, buflen);
          buffer[curlen + buflen] = '\0';
          free((void*)item->text);
          item->text = buffer;
        }
      } else {
        buffer = (char*)malloc((buflen + 1) * sizeof(char));
        if (buffer != NULL) {
          strncpy(buffer, text, buflen);
          buffer[buflen] = '\0';
          item = stringlist_append(root, text, flags);
          free(buffer);
          if (item == NULL)
            return; /* stringlist_add() failed! -> quit */
        }
      }
    }
    /* prepare for the next part of the string */
    text = tail;
    if (*text == '\n') {
      char *start;
      text++;             /* skip '\n' (to set start of next string) */
      /* text ended with a newline -> set "handled" flag */
      assert(item != NULL);
      item->flags |= STRFLG_HANDLED;
      /* look up file:line information from addresses */
      if (dwarf_linetable.entries > 0 && dwarf_filetable.next != NULL
          && (start = strstr(item->text, "*0x")) != NULL)
      {
        unsigned long addr = strtoul(start + 3, (char**)&tail, 16);
        const DWARF_LINEENTRY *lineinfo = dwarf_line_from_address(&dwarf_linetable, addr);
        if (lineinfo != NULL) {
          const char *path = dwarf_path_from_fileindex(&dwarf_filetable, lineinfo->fileindex);
          if (path != NULL) {
            size_t sz = (start - item->text) + strlen(tail);
            const char *basename = lastdirsep(path);
            if (basename == NULL)
              basename = path;
            sz += strlen(basename) + 12;  /* +12 for the line number plus colon */
            buffer = (char*)malloc((sz + 1) * sizeof(char));
            if (buffer != NULL) {
              size_t pos = start - item->text;
              size_t len = strlen(basename);
              strncpy(buffer, item->text, pos);
              strncpy(buffer + pos, basename, len);
              buffer[pos + len] = ':';
              sprintf(buffer + pos + len + 1, "%d", lineinfo->line);
              strcat(buffer, tail);
              assert(strlen(buffer) <= sz);
              free((void*)item->text);
              item->text = buffer;
            }
          }
        }
      }
    }
  }
}

/** skip_string() skips a string between quotes, or a word.
 *
 *  \param string     Points to the start of the text to skip.
 *  \param stopchars  A string with optional "stop characters" at which the
 *                    parsing stops. This parameter may be NULL, in which case
 *                    the single stop character is a space.
 *
 *  \return A pointer to the first character behind the string or word.
 *
 *  \note Parameter "stopchars" is a don't-care if parameter "string" points to
 *        a string between double quotes.
 *  \note In case "string" points to non-quoted text, all control characters
 *        (below ASCII 32) are also stop characters.
 */
static const char *skip_string(const char *string, const char *stopchars)
{
  assert(string != NULL);
  if (stopchars == NULL)
    stopchars = " ";
  if (*string == '"') {
    string++;
    while (*string != '"' && *string != '\0') {
      if (*string == '\\' && *(string + 1) != '\0')
        string++;
      string++;
    }
    if (*string == '"')
      string++;
  } else {
    while (*string >= ' ' && strchr(stopchars, *string) == NULL)
      string++;
  }
  return string;
}

static const char *str_matchchar(const char *string, char match)
{
  assert(string != NULL);
  while (*string != '\0' && *string != match) {
    if (*string == '"')
      string = skip_string(string, NULL);
    else
      string += 1;
  }
  return (*string == match) ? string : NULL;
}

static char *format_string(char *buffer)
{
  assert(buffer != NULL);
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
          if (isdigit(*src)) {
            int v = *src - '0';
            int count = 0;
            while (isdigit(*(src + 1)) && count++ < 3) {
              src += 1;
              v = (v << 3) + *src - '0';
            }
            *tgt = (char)v;
          } else {
            assert(0);
            *tgt = '?';
          }
        }
      } else {
        *tgt = *src;
      }
      tgt++;
      src++;
    }
    *tgt = '\0';
    return (*src == '"') ? src + 1 : src;
  }
  return buffer + strlen(buffer);
}

/* format_value() formats an integer in a text string into both decimal and
   hexadecimal */
static char *format_value(char *buffer, size_t size)
{
  assert(buffer != NULL);
  buffer = (char*)skipwhite(buffer);
  if (*buffer != '\0') {
    char *ptr;
    long v = strtol(buffer, &ptr, 0);
    ptr = (char*)skipwhite(ptr);
    if (*ptr == '\0') {
      /* so the buffer contains a single integer value -> reformat it */
      snprintf(buffer, size, "%ld [0x%lx]", v, (unsigned long)v);
    }
  }
  return buffer;
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
static STRINGLIST helptext_root = { NULL, NULL, 0 };
static unsigned console_hiddenflags = 0; /* when a message contains a flag in this set, it is hidden in the console */
static unsigned console_replaceflags = 0;/* when a message contains a flag in this set, it is "translated" to console_xlateflags */
static unsigned console_xlateflags = 0;

static const char *gdbmi_leader(char *buffer, int *flags, char **next_segment)
{
  char *tail = NULL;

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
    tail = format_string(buffer);
    break;
  case '-': /* MI command input */
    *flags |= STRFLG_MI_INPUT;
    tail = format_string(buffer);
    break;
  case '&': /* logged commands & replies (plain commands in MI mode) */
    *flags |= STRFLG_LOG;
    buffer += 1;
    tail = format_string(buffer);
    break;
  case '@': /* target output in MI mode */
    *flags |= STRFLG_TARGET;
    buffer += 1;
    tail = format_string(buffer);
    break;
  }

  if (*flags & console_replaceflags)
    *flags = (*flags & ~console_replaceflags) | console_xlateflags;
  if (next_segment != NULL)
    *next_segment = tail;

  return buffer;
}

static const char *gdbmi_isresult(void)
{
  STRINGLIST *item = stringlist_getlast(&consolestring_root, STRFLG_RESULT, STRFLG_HANDLED);
  assert(item == NULL || item->text != NULL);
  return (item != NULL) ? item->text : NULL;
}

static void gdbmi_sethandled(bool all)
{
  STRINGLIST *item;
  do {
    item = stringlist_getlast(&consolestring_root, STRFLG_RESULT, STRFLG_HANDLED);
    if (item != NULL)
      item->flags |= STRFLG_HANDLED;
  } while (item != NULL && all);
  assert(stringlist_getlast(&consolestring_root, STRFLG_RESULT, STRFLG_HANDLED) == NULL);
}

static int is_gdb_prompt(const char *text)
{
  text = skipwhite(text);
  return strncmp(text, "(gdb)", 5) == 0 && strlen(text) <= 6;
}


static int is_keyword(const char *word)
{
  static const char *keywords[] = { "auto", "break", "case", "char", "const",
                                    "continue", "default", "do", "double",
                                    "else", "enum", "extern", "float", "for",
                                    "goto", "if", "int", "long", "register",
                                    "return", "short", "signed", "sizeof",
                                    "static", "struct", "switch", "typedef",
                                    "union", "unsigned", "void", "volatile",
                                    "while" };
  int idx;

  assert(word != NULL);
  if (!isalpha(*word))
    return 0; /* quick test: all keywords begin with a letter */
  for (idx = 0; idx < sizearray(keywords); idx++)
    if (strcmp(word, keywords[idx]) == 0)
      return 1;
  return 0;
}

static char *console_buffer = NULL;
static size_t console_bufsize = 0;

static void console_growbuffer(size_t extra)
{
  if (console_buffer == NULL) {
    console_bufsize = 256;
    while (extra >= console_bufsize)
      console_bufsize *= 2;
    console_buffer = (char*)malloc(console_bufsize * sizeof(char));
    if (console_buffer != NULL)
      console_buffer[0]= '\0';
  } else if (strlen(console_buffer) + extra >= console_bufsize) {
    console_bufsize *= 2;
    while (strlen(console_buffer) + extra >= console_bufsize)
      console_bufsize *= 2;
    char *newbuffer = (char*)malloc(console_bufsize * sizeof(char));
    if (newbuffer != NULL)
      strcpy(newbuffer, console_buffer);
    free((void*)console_buffer);
    console_buffer = newbuffer;
  }
  if (console_buffer == NULL) {
    fprintf(stderr, "Memory allocation error.\n");
    exit(EXIT_FAILURE);
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

static bool console_add(const char *text, int flags)
{
  static int curflags = -1;
  const char *head, *ptr;
  bool addstring = false;
  bool foundprompt = false;

  console_growbuffer(strlen(text));

  if (curflags != flags && console_buffer[0] != '\0') {
    int xtraflags;
    assert(curflags >= 0);
    ptr = gdbmi_leader(console_buffer, &xtraflags, NULL);
    if ((curflags & STRFLG_MON_OUT) != 0 && (xtraflags & STRFLG_TARGET) != 0)
      xtraflags = (xtraflags & ~STRFLG_TARGET) | STRFLG_STATUS;
    if ((xtraflags & STRFLG_TARGET) != 0 && (curflags & STRFLG_STARTUP) == 0)
      semihosting_add(&semihosting_root, ptr, curflags | xtraflags);
    /* after gdbmi_leader(), there may again be '\n' characters in the resulting string */
    for (char *tok = strtok((char*)ptr, "\n"); tok != NULL; tok = strtok(NULL, "\n")) {
      striptrailing(tok);
      stringlist_append(&consolestring_root, tok, curflags | xtraflags);
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
      addstring = false;  /* string is not terminated, wait for more characters to come in */
    } else {
      addstring = true;
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
      int xtraflags;
      ptr = gdbmi_leader(console_buffer, &xtraflags, NULL);
      bool targetmsg = ((xtraflags & STRFLG_TARGET) != 0);
      if ((curflags & STRFLG_MON_OUT) && (xtraflags & STRFLG_TARGET)) {
        /* distinguish replies to monitor commands from semihosting */
        xtraflags = (xtraflags & ~STRFLG_TARGET) | STRFLG_STATUS;
      }
      bool prompt = is_gdb_prompt(ptr) && (xtraflags & STRFLG_TARGET) == 0;
      if (prompt) {
        foundprompt = true; /* don't add prompt to the output console, but mark that we've seen it */
      } else {
        /* after gdbmi_leader(), there may again be '\n' characters in the resulting string */
        if ((xtraflags & STRFLG_TARGET) != 0 && (curflags & STRFLG_STARTUP) == 0)
          semihosting_add(&semihosting_root, ptr, curflags | xtraflags);
        char *str = (char*)ptr;
        while (str != NULL && *str != '\0') {
          char *eol = strchr(str, '\n');
          if (eol != NULL)
            *eol = '\0';  /* terminate the string here (simulate strtok()) */
          else if (targetmsg)
            xtraflags |= STRFLG_NO_EOL;
          /* avoid adding a "log" string when the same string is already at the
             tail of the list */
          STRINGLIST *last = stringlist_getlast(&consolestring_root, 0, 0);
          if ((xtraflags & STRFLG_LOG) == 0 || last == NULL || strcmp(last->text, str) != 0) {
            int fullflags = flags | xtraflags;
            /* check whether the concatenate this line to the last line */
            if (last != NULL && (last->flags & STRFLG_NO_EOL) && ((last->flags ^ fullflags) & ~STRFLG_NO_EOL) == 0) {
              size_t sz = strlen(last->text) + strlen(str) + 1;
              char *newtext = malloc(sz * sizeof(char));
              if (newtext != NULL) {
                strcpy(newtext, last->text);
                strcat(newtext, str);
                free((void*)last->text);
                last->text = newtext;
                last->flags = fullflags;
              }
            } else {
              striptrailing(str);
              stringlist_append(&consolestring_root, str, fullflags);
            }
          }
          str = (eol != NULL) ? eol + 1 : NULL;
        }
      }
      console_buffer[0]= '\0';
    }
  }
  return foundprompt;
}

static void console_input(const char *text)
{
  gdbmi_sethandled(false); /* clear result on new input */
  assert(text != NULL);
  console_add(text, STRFLG_INPUT);
}

static int console_autocomplete(char *text, size_t textsize, const DWARF_SYMBOLLIST *symboltable)
{
  typedef struct tagGDBCOMMAND {
    const char *command;
    const char *shorthand;
    const char *parameters;
  } GDBCOMMAND;
  static const GDBCOMMAND commands[] = {
    { "assembly", NULL, "off on" },
    { "attach", NULL, NULL },
    { "backtrace", "bt", NULL },
    { "break", "b", "%func %file" },
    { "clear", NULL, "%func %file" },
    { "command", NULL, NULL },
    { "compare-sections", NULL, NULL },
    { "cond", NULL, NULL },
    { "continue", "c", NULL },
    { "delete", NULL, NULL },
    { "directory", "dir", "%dir" },
    { "disable", NULL, NULL },
    { "disassemble", "disas", "off on" },
    { "display", NULL, "%var %reg" },
    { "dprintf", NULL, "%func %var" },
    { "down", NULL, NULL },
    { "enable", NULL, NULL },
    { "file", NULL, "%path" },
    { "find", NULL, "%func %var" },
    { "finish", "fin", NULL },
    { "frame", "f", NULL },
    { "help", NULL, "assembly break breakpoints data files find keyboard monitor running serial stack status support svd trace user-defined" },
    { "info", NULL, "args breakpoints frame functions locals scope set sources stack svd variables vtbl %var" },
    { "list", NULL, "%func %var %file" },
    { "load", NULL, NULL },
    { "monitor", "mon", "auto_scan connect_srst frequency halt_timeout hard_srst heapinfo jtag_scan morse option swdp_scan reset rtt targets tpwr traceswo vector_catch" },
    { "next", "n", NULL },
    { "print", "p", "%var %reg" },
    { "ptype", NULL, "%var" },
    { "quit", NULL, NULL },
    { "reset", NULL, "hard load" },
    { "run", NULL, NULL },
    { "semihosting", NULL, "clear" },
    { "serial", NULL, "clear disable enable info plain save %path" },
    { "set", NULL, "%var" },
    { "start", NULL, NULL },
    { "step", "s", NULL },
    { "target", NULL, "extended-remote remote" },
    { "tbreak", NULL, "%func %file" },
    { "trace", NULL, "async auto bitrate channel clear disable enable info passive plain save %path" },
    { "undisplay", NULL, NULL },
    { "until", "u", NULL },
    { "up", NULL, NULL },
    { "watch", NULL, "%var" }
  };
  static char *cache_text = NULL;
  static int cache_cutoff = 0;
  static int cache_skip = 0;
  char *word;
  size_t len, idx, skip;
  int result = 0;

  assert(text != NULL);

  /* first check whether the text is unmodified from the last cached string */
  if (cache_text != NULL && strcmp(text, cache_text) == 0) {
    assert(cache_cutoff >= 0 && (size_t)cache_cutoff < textsize);
    text[cache_cutoff] = '\0';
    cache_skip += 1;
  } else {
    cache_skip = 0;
    cache_cutoff = strlen(text);
  }

  /* delete any leading spaces (they cause trouble in parsing) */
  while (text[0] == ' ')
    memmove(text, text + 1, strlen(text));

  /* get the start of the last word */
  if ((word = strrchr(text, ' ')) == NULL)
    word = text;
  else
    word = (char*)skipwhite(word);
  if (strlen(word) == 0 && word == text)
    return 0; /* no text -> nothing to autocomplete */

  skip = cache_skip;
  if (word == text) {
    /* first word: auto-complete command */
    const GDBCOMMAND *first = NULL;
    /* step 1: find shorthand (must be full match) */
    for (idx = 0; !result && idx < sizearray(commands); idx++) {
      if (commands[idx].shorthand != NULL && strcmp(word, commands[idx].shorthand) == 0) {
        strlcpy(text, commands[idx].command, textsize);
        result = 1;
      }
    }
    /* step 2: find a partial match on the full name, but skip the first matches */
    len = strlen(word);
    for (idx = 0; !result && idx < sizearray(commands); idx++) {
      assert(commands[idx].command != NULL);
      if (strncmp(word, commands[idx].command, len) == 0) {
        if (first == NULL)
          first = &commands[idx];
        if (skip == 0) {
          strlcpy(text, commands[idx].command, textsize);
          result = 1;
        }
        skip--;
      }
    }
    /* step 3: if there were matches, but all were skipped, wrap around to find
       the first match */
    if (!result && first != NULL) {
      cache_skip = 0;
      strlcpy(text, first->command, textsize);
      result = 1;
    }
    if (result)
      strlcat(text, " ", textsize);
  } else {
    /* any next word: auto-completion depends on the command */
    const GDBCOMMAND *cmd = NULL;
    char str[50], *ptr;
    int count, fullmatch;
    /* get the command */
    ptr = strchr(text, ' ');
    assert(ptr != NULL);  /* last space was found, so first space must be too */
    len = ptr - text;
    if (len >= sizearray(str))
      len = sizearray(str) - 1;
    strncpy(str, text, len);
    str[len] = '\0';
    /* accept either a full match on the alias or a partial match on the command
       name, but only if there is a single partial match */
    count = fullmatch = 0;
    for (idx = 0; !fullmatch && idx < sizearray(commands); idx++) {
      if (commands[idx].shorthand != NULL && strcmp(str, commands[idx].shorthand) == 0) {
        cmd = &commands[idx];
        fullmatch = 1;
      } else if (strncmp(str, commands[idx].command, len) == 0) {
        cmd = &commands[idx];
        count++;
      }
    }
    if (fullmatch || count == 1) {
      assert(cmd != NULL);
      len = strlen(word);
      if (cmd->parameters != NULL) {
        char *params = strdup(cmd->parameters);
        if (params != NULL) {
          const char *first = NULL, *first_prefix = NULL, *first_suffix = NULL;
          for (ptr = strtok(params, " "); !result && ptr != NULL; ptr = strtok(NULL, " ")) {
            if (strcmp(ptr, "%file") == 0) {
              const char *fname;
              for (idx = 0; !result && (fname = source_getname(idx)) != NULL; idx++) {
                if (strncmp(word, fname, len)== 0) {
                  if (first == NULL)
                    first = fname;
                  if (skip == 0) {
                    strlcpy(word, fname, textsize - (word - text));
                    result = 1;
                  }
                  skip--;
                }
              }
            } else if (strcmp(ptr, "%var") == 0 || strcmp(ptr, "%func") == 0) {
              const DWARF_SYMBOLLIST *sym;
              int match_var = (strcmp(ptr, "%var") == 0);
              int curfile, curline;
              source_getcursorpos(&curfile, &curline);
              /* translate the file index from the list returned by GDB to the
                 list collected from DWARF */
              const char *path = source_getname(curfile);
              assert(path != NULL);
              curfile = dwarf_fileindex_from_path(&dwarf_filetable, path);
              for (idx = 0; !result && (sym = dwarf_sym_from_index(symboltable, idx)) != 0; idx++) {
                assert(sym->name != NULL);
                if (strncmp(word, sym->name, len)== 0) {
                  int match = 0;
                  if (match_var && DWARF_IS_VARIABLE(sym)) {
                    if (sym->scope == SCOPE_EXTERNAL
                        || (sym->scope == SCOPE_UNIT && sym->fileindex == curfile)
                        || (sym->scope == SCOPE_FUNCTION && sym->fileindex == curfile && sym->line <= curline && curline < sym->line_limit))
                      match = 1;
                  } else if (!match_var && DWARF_IS_FUNCTION(sym)) {
                    if (sym->scope == SCOPE_EXTERNAL
                        || (sym->scope == SCOPE_UNIT && sym->fileindex == curfile))
                      match = 1;
                  }
                  if (match) {
                    if (first == NULL)
                      first = sym->name;
                    if (skip == 0) {
                      strlcpy(word, sym->name, textsize - (word - text));
                      result = 1;
                    }
                    skip--;
                  }
                }
              }
            } else if (strcmp(ptr, "%reg") == 0) {
              int prefix_len = strlen(svd_mcu_prefix());
              const char *sep;
              /* auto-complete prefix */
              if (prefix_len > 0  && len < (size_t)prefix_len && strncmp(word, svd_mcu_prefix(), len) == 0) {
                strlcpy(word, svd_mcu_prefix(), textsize - (word - text));
                result = 1;
              }
              /* auto-complete peripheral */
              if ((sep = strstr(word, "->")) != NULL)
                sep += 2;
              else if ((sep = strchr(word, '.')) != NULL)
                sep += 1;
              if (!result && len >= (size_t)prefix_len && sep == NULL) {
                int iter;
                const char *name;
                word += prefix_len;
                len -= prefix_len;
                for (iter = 0; !result && (name = svd_peripheral(iter, NULL, NULL)) != NULL; iter++) {
                  if (strncmp(word, name, len) == 0) {
                    first_suffix = "->";
                    if (first == NULL)
                      first = name;
                    if (skip == 0) {
                      strlcpy(word, name, textsize - (word - text));
                      strlcat(word, first_suffix, textsize - (word - text));
                      result = 1;
                    }
                    skip--;
                  }
                }
              }
              /* autocomplete register name */
              if (!result && sep != NULL) {
                /* find peripheral first */
                int iter, ln;
                const char *name;
                char periph_name[50];
                ln = (sep - word);
                while (ln > prefix_len && (word[ln - 1] == '-' || word[ln - 1] == '>' || word[ln - 1] == '.' || word[ln - 1] == ' '))
                  ln -= 1;
                if (ln >= sizearray(periph_name))
                  ln = sizearray(periph_name) - 1;
                strncpy(periph_name, word + prefix_len, ln - prefix_len);
                periph_name[ln - prefix_len] = '\0';
                word += ln;
                ln = strlen(sep);
                for (iter = 0; !result && (name = svd_register(periph_name, iter, NULL, NULL, NULL)) != NULL; iter++) {
                  if (strncmp(sep, name, ln) == 0) {
                    first_prefix = "->";
                    if (first == NULL)
                      first = name;
                    if (skip == 0) {
                      strlcpy(word, first_prefix, textsize - (word - text));
                      strlcat(word, name, textsize - (word - text));
                      if (strlen(name) > 2) {
                        ln = strlen(word);
                        assert(ln > 2);
                        if (word[ln - 2] == '%' && word[ln - 1] == 's') {
                          word[ln - 2] = '\0';
                          strlcat(word, "[0]", textsize - (word - text));
                        }
                      }
                      result = 1;
                    }
                    skip--;
                  }
                }
              }
            } else if (strcmp(ptr, "%path") == 0 || strcmp(ptr, "%dir") == 0) {
              bool dir_only = (strcmp(ptr, "%dir") == 0);
              char *dirname = strdup(word);
              if (dirname != NULL) {
                const char dirseparator[] = { DIRSEP_CHAR, '\0' };
                char *base = strrchr(dirname, DIRSEP_CHAR);
#               if defined _WIN32
                  if (base == NULL)
                    base = strrchr(dirname, '/');
                  else if (strchr(base, '/') != NULL)
                    base = strrchr(base, '/');
#               endif
                DIR *dir;
                if (base != NULL) {
                  *base = '\0'; /* cut off directory name at the slash */
                  dir = opendir((strlen(dirname) > 0) ? dirname : dirseparator);
                  word += (base - dirname) + 1; /* only complete part behind the slash */
                } else {
                  dir = opendir("."); /* start from current directory */
                }
                len = strlen(word);
                if (dir != NULL) {
                  struct dirent *entry;
                  while (!result && (entry = readdir(dir)) != NULL) {
                    if (len == 0 || strnicmp(word, entry->d_name, len) == 0) {
#                     if defined __MINGW32__ || defined __MINGW64__
                        /* neither file attributes nor file type are returned by
                           MingW implementation of readdir() */
                        #define IS_DIR(e) 0
#                     elif defined _WIN32
                        #define IS_DIR(e) (((e)->d_attr & _A_SUBDIR) != 0)
#                     else
                        #define IS_DIR(e) ((e)->d_type == DT_DIR)
#                     endif
                      static char firstfile[_MAX_PATH];
                      char filename[_MAX_PATH];
                      assert(strlen(entry->d_name) < sizearray(filename));
                      strlcpy(filename, entry->d_name, sizearray(filename));
                      if (IS_DIR(entry))  /* for directory, append slash */
                        strlcat(filename, dirseparator, textsize - (word - text));
                      else if (dir_only)
                        continue;         /* directory requested, skip names of normal files */
                      if (first == NULL) {
                        strcpy(firstfile, filename);
                        first = firstfile;
                      }
                      if (skip == 0) {
                        strlcpy(word, filename, textsize - (word - text));
                        result = 1;
                      }
                      skip--;
                    }
                  }
                  closedir(dir);
                }
                free(dirname);
              }
            } else if (strncmp(word, ptr, len)== 0) {
              if (first == NULL)
                first = ptr;
              if (skip == 0) {
                strlcpy(word, ptr, textsize - (word - text));
                result = 1;
              }
              skip--;
            }
          }
          if (!result && first != NULL) {
            cache_skip = 0;
            if (first_prefix != NULL) {
              strlcpy(word, first_prefix, textsize -(word - text));
              word += strlen(first_prefix);
            }
            strlcpy(word, first, textsize - (word - text));
            if (first_suffix != NULL)
              strlcat(word, first_suffix, textsize - (word - text));
            result = 1;
          }
          free((void*)params);
        }
      }
    }
  }

  /* update cache */
  if (cache_text != NULL) {
    free((void*)cache_text);
    cache_text = NULL;
  }
  if (result)
    cache_text = strdup(text);

  return result;
}

static void console_history_add(STRINGLIST *root, char *text, bool tail)
{
  assert(root != NULL);
  assert(text != NULL);
  text[strcspn(text, "\n")] = '\0'; // remove EOL from command

  // If the command history is empty (ex new install of bmdebug) then store the first command
  if (root->next == NULL) {
    stringlist_append(root, text, 0);
    return;
  }

  // If the current command = most recent command then do not store it again
  if (strcmp(root->next->text, text) == 0)
    return;

  // Store the command
  if (tail)
    stringlist_append(root, text, 0);
  else
    stringlist_insert(root, text, 0);
}

static const STRINGLIST *console_history_step(const STRINGLIST *root, const STRINGLIST *mark, int forward)
{
  assert(root != NULL);
  if (root->next == NULL)
    return NULL;

  if (forward) {
    const STRINGLIST *sentinel = (mark == NULL || mark == root->next) ? root->next : mark;
    for (mark = root->next; mark->next != NULL && mark->next != sentinel; mark = mark->next)
      {}
  } else {
    mark = (mark == NULL || mark->next == NULL) ? root->next : mark->next;
  }
  return mark;
}

static const STRINGLIST *console_history_match(const STRINGLIST *root, const STRINGLIST *mark,
                                               char *text, size_t textsize)
{
  static char *cache_text = NULL;
  static int cache_cutoff = 0;
  /* special case, for clean-up */
  if (root == NULL && mark == NULL && text == NULL) {
    if (cache_text != NULL) {
      free((void*)cache_text);
      cache_text = NULL;
    }
    return NULL;
  }

  assert(text != NULL);

  /* first check whether the text is unmodified from the last cached string */
  if (cache_text != NULL && strcmp(text, cache_text) == 0) {
    assert(cache_cutoff >= 0 && (size_t)cache_cutoff < textsize);
    text[cache_cutoff] = '\0';
  } else {
    cache_cutoff = strlen(text);
  }

  /* find a string in the history that matches the given text */
  const STRINGLIST *item = mark;
  while ((item = console_history_step(root, item, 0)) != NULL && item != mark) {
    assert(item->text != NULL);
    if ((int)strlen(item->text) > cache_cutoff && strncmp(item->text, text, cache_cutoff) == 0)
      break;  /* match found */
  }

  /* update cache */
  if (cache_text != NULL) {
    free((void*)cache_text);
    cache_text = NULL;
  }
  if (item != NULL && item != mark)
    cache_text = strdup(item->text);

  return item;
}


typedef struct tagSOURCELINE {
  struct tagSOURCELINE *next;
  char *text;
  unsigned long address;  /* memory address (if known) */
  int linenumber;         /* line number in the source file (0 for assembly) */
  bool hidden;            /* assembly lines are shown or hidden, depending on mode */
} SOURCELINE;

typedef struct tagSOURCEFILE {
  struct tagSOURCEFILE *next;
  int *srcindex;          /* list of GDB file indices that map to this source file */
  unsigned srccount, srcsize;
  char *basename;         /* filename excluding path */
  char *path;             /* full path to the source file */
  SOURCELINE root;        /* root of text lines */
  time_t timestamp;
} SOURCEFILE;

static ELF_SYMBOL *elf_symbols = NULL;
static int elf_symbol_count = 0;
static SOURCEFILE sources_root = { NULL };

/** sourceline_append() adds a string to the tail of the list. */
static SOURCELINE *sourceline_append(SOURCELINE *root, const char *text,
                                     unsigned long address, int linenumber)
{
  SOURCELINE *item = (SOURCELINE*)malloc(sizeof(SOURCELINE));
  if (item == NULL)
    return NULL;
  memset(item, 0, sizeof(SOURCELINE));
  item->text = strdup(text);
  if (item->text == NULL) {
    free((void*)item);
    return NULL;
  }
  item->address = address;
  item->linenumber = linenumber;

  assert(root != NULL);
  SOURCELINE *tail;
  for (tail = root; tail->next != NULL; tail = tail->next)
    {}
  tail->next = item;
  return item;
}

/** sourceline_insert() inserts a string *after* the passed-in pointer. Pass
 *  in "root" to insert a string at ad the head of the list.
 */
static SOURCELINE *sourceline_insert(SOURCELINE *listpos, const char *text,
                                     unsigned long address, int linenumber)
{
  SOURCELINE *item = (SOURCELINE*)malloc(sizeof(SOURCELINE));
  if (item == NULL)
    return NULL;
  memset(item, 0, sizeof(SOURCELINE));
  item->text = strdup(text);
  if (item->text == NULL) {
    free((void*)item);
    return NULL;
  }
  item->address = address;
  item->linenumber = linenumber;

  assert(listpos != NULL);
  item->next = listpos->next;
  listpos->next = item;
  return item;
}

static void sourceline_clear(SOURCELINE *root)
{
  assert(root != NULL);
  while (root->next != NULL) {
    SOURCELINE *item = root->next;
    root->next = item->next;
    assert(item->text != NULL);
    free((void*)item->text);
    free((void*)item);
  }
}

static void sourcefile_load(FILE *fp, SOURCELINE *root)
{
  assert(fp != NULL);
  assert(root != NULL);
  assert(root->next == NULL); /* line list should be empty */

  int linenumber = 1;
  char input[512]; /* source code really should not have lines as long as this */
  while (fgets(input, sizearray(input), fp) != NULL) {
    /* reformat input text (at the moment, only TAB expansion) */
    #define TABSIZE 4 /* should be parametrizable */
    char line[512];
    int idx = 0;
    for (const char *ptr = input; *ptr != '\0' && *ptr != '\n' && idx < sizearray(line) - 1; ptr++) {
      if (*ptr == '\t') {
        line[idx++] = ' ';
        while (idx < sizearray(line) - 1 && idx % TABSIZE != 0)
          line[idx++] = ' ';
      } else {
        line[idx++] = *ptr;
      }
    }
    line[idx] = '\0';
    sourceline_append(root, line, 0, linenumber++);
  }
}

static SOURCEFILE *source_fromindex(int srcindex)
{
  for (SOURCEFILE *src = sources_root.next; src != NULL; src = src->next)
    for (unsigned idx = 0; idx < src->srccount; idx++)
      if (src->srcindex[idx] == srcindex)
        return src;
  return NULL;
}

static bool source_isvalid(int srcindex)
{
  return (source_fromindex(srcindex) != NULL);
}

static int source_linecount(int srcindex)
{
  int count = 0;
  SOURCEFILE *src = source_fromindex(srcindex);
  if (src != NULL)
    for (SOURCELINE *item = src->root.next; item != NULL; item = item->next)
      count++;
  return count;
}

/** sourceline_get() returns the physical line of the file (which is the same
 *  as the source line if on source mode, but not in disassembly mode). The
 *  linenr parameter is 1-based.
 */
static SOURCELINE *sourceline_get(int srcindex, int linenr)
{
  SOURCEFILE *src = source_fromindex(srcindex);
  if (src == NULL)
    return NULL;
  SOURCELINE *line = src->root.next;
  while (line != NULL && linenr > 1) {
    line = line->next;
    linenr -= 1;
  }
  return line;
}

static bool disasm_callback(uint32_t address, const char *text, void *user)
{
  const DWARF_LINEENTRY *entry = dwarf_line_from_address(&dwarf_linetable, address);
  if (entry != NULL) {
    SOURCELINE *root = (SOURCELINE*)user;
    assert(root != NULL);
    SOURCELINE *item = root->next;
    while (item != NULL && item->linenumber != entry->line)
      item = item->next;
    /* dropped on the source line with the given line number, now skip any
       assembly lines that follow */
    if (item != NULL) {
      while (item->next != NULL && item->next->linenumber == 0 && item->next->address < address)
        item = item->next;
    }
    if (item == NULL)
      sourceline_append(root, text, address, 0);
    else
      sourceline_insert(item, text, address, 0);
  }
  return true;
}

static void disasm_show_hide(int fileindex, bool visible)
{
  SOURCELINE *item = sourceline_get(fileindex, 1);
  while (item != NULL) {
    if (item->linenumber == 0)
      item->hidden = !visible;
    item = item->next;
  }
}

static bool sourcefile_disassemble(const char *path, const SOURCEFILE *source, ARMSTATE *armstate)
{
  assert(path != NULL);
  assert(source != NULL);
  assert(armstate != NULL);

  /* if not yet done, get the list of symbols from the ELF file; this is just
     used to get the start addresses of the functions and to determine whether
     to disassemble in Thumb mode or ARM mode; the function names are not
     important (and therefore not demangled) */
  if (elf_symbols == NULL) {
    FILE *fp = fopen(path, "rb");
    if (fp != NULL) {
      unsigned count = 0;
      if (elf_load_symbols(fp, NULL, &count) == ELFERR_NONE && count > 0) {
        elf_symbols = malloc(count * sizeof(ELF_SYMBOL));
        if (elf_symbols != NULL) {
          elf_load_symbols(fp, elf_symbols, &count);
          elf_symbol_count = count;
        }
      }
      fclose(fp);
    }
    /* add all code symbols to the ARM debugger state */
    disasm_init(armstate, DISASM_ADDRESS | DISASM_INSTR | DISASM_COMMENT);
    for (int i = 0; i < elf_symbol_count; i++) {
      if (elf_symbols[i].is_func) {
        uint32_t address = elf_symbols[i].address & ~1;
        int mode = (elf_symbols[i].address & 1) ? ARMMODE_THUMB : ARMMODE_ARM;
        char demangled[256];
        const char *name = elf_symbols[i].name;
        if (demangle(demangled, sizeof(demangled), name))
          name = demangled;
        disasm_symbol(armstate, name, address, mode);
      }
    }
    /* add SVD peripherals to the disassembler as well */
    const char *name;
    unsigned long address;
    for (int idx = 0; (name = svd_peripheral(idx, &address, NULL)) != NULL; idx++)
      disasm_symbol(armstate, name, address, ARMMODE_DATA);
  }

  int fileidx = dwarf_fileindex_from_path(&dwarf_filetable, source->basename);
  if (fileidx == -1)
    return false;   /* source file not found in the DWARF file table */

  /* associate addresses to source lines */
  assert(source != NULL);
  for (SOURCELINE *item = source->root.next; item != NULL; item = item->next)
    item->address = 0;  /* clear any old address mappings */
  SOURCELINE *item = source->root.next;
  int curline = 1;
  for (unsigned idx = 0; idx < dwarf_linetable.entries; idx++) {
    if (dwarf_linetable.table[idx].fileindex != fileidx)
      continue;         /* address belongs to a different file, ignore */
    if (curline > dwarf_linetable.table[idx].line) {
      item = source->root.next; /* restart line count from the root */
      curline = 1;
    }
    while (curline < dwarf_linetable.table[idx].line && item != NULL) {
      item = item->next;
      curline += 1;
    }
    if (curline == dwarf_linetable.table[idx].line && item != NULL) {
      /* there may be multiple addresses associated to a single source line;
         keep the lowest non-zero address */
      if (item->address == 0 || item->address < dwarf_linetable.table[idx].address)
        item->address = dwarf_linetable.table[idx].address;
    }
  }

  /* get the address range for the current file */
  unsigned addr_low = UINT_MAX, addr_high = 0;
  const DWARF_SYMBOLLIST *sym;
  for (unsigned i = 0; (sym = dwarf_sym_from_index(&dwarf_symboltable, i)) != NULL; i++) {
    /* check for a function in the actve file */
    if (sym->code_range > 0 && sym->fileindex == fileidx) {
      if (sym->code_addr < addr_low)
        addr_low = sym->code_addr;
      if (sym->code_addr + sym->code_range > addr_high)
        addr_high = sym->code_addr + sym->code_range;
    }
  }
  if (addr_low >= addr_high)
    return false;   /* no functions in this file (like in a header file) */
  unsigned addr_range = addr_high - addr_low;

  unsigned char *bincode = NULL;
  int mode = ARMMODE_UNKNOWN;
  FILE *fp = fopen(path, "rb");
  if (fp != NULL) {
    /* get initial mode from address of ELF entry point */
    unsigned long entry;
    if (elf_info(fp, NULL, NULL, NULL, &entry) == ELFERR_NONE)
      mode = (entry & 1) ? ARMMODE_THUMB : ARMMODE_ARM;
    /* find the section to read and read the portion relevant for this source file */
    unsigned long offset, address, length;
    if (elf_section_by_name(fp, ".text", &offset, &address, &length) == ELFERR_NONE) {
      if (address <= addr_low && addr_high <= address + length) {
        /* address <= addr_low           => .text section address must be <= object code address
           addr_high <= address + length => .text section must exceed object code range
           if one of the above criterions is not true, we are trying to
           disassemble data in a different section than .text
         */
        bincode = malloc(addr_range * sizeof(unsigned char));
        if (bincode != NULL) {
          fseek(fp, offset + (addr_low - address), SEEK_SET);
          fread(bincode, 1, addr_range, fp);
        }
      }
    }
    fclose(fp);
  }
  if (bincode == NULL)
    return false;   /* unable to read the ELF file, or insufficient memory */

  /* finally, start the disassembly */
  disasm_address(armstate, addr_low);
  disasm_buffer(armstate, bincode, addr_range, mode, disasm_callback, (void*)&source->root);
  disasm_compact_codepool(armstate, addr_low, addr_range);
  free((void*)bincode);
  return true;
}

/** sources_add() adds a source file unless it already exists in the list.
 *  \param srcindex   GDB file index;
 *  \param filename   The base name of the source file (including extension but
 *                    excluding the path).
 *  \param filepath   The (full or partial) path to the file (already in
 *                    OS-specific format). This parameter may be NULL (in which
 *                    case the path is assumed the same as the base name).
 */
static bool sources_add(int srcindex, const char *filename, const char *filepath, bool debugmode)
{
  assert(filename != NULL);

  /* check whether the source already exists */
  for (SOURCEFILE *src = sources_root.next; src != NULL; src = src->next) {
    if (strcmp(src->basename, filename) == 0) {
      const char *p1 = (src->path != NULL) ? src->path : "";
      const char *p2 = (filepath != NULL) ? filepath : "";
      if (strcmp(p1, p2) == 0) {
        /* both base name and full path are the same -> do not add duplicate,
           but add the index */
        assert(src->srcindex != NULL);
        if (src->srccount >= src->srcsize) {
          /* grow list */
          unsigned newsize = 2 * src->srcsize;
          int *newindex = malloc(newsize * sizeof(int));
          if (newindex != NULL) {
            memcpy(newindex, src->srcindex, src->srccount * sizeof(int));
            free((void*)src->srcindex);
            src->srcindex = newindex;
            src->srcsize = newsize;
          }
        }
        if (src->srccount < src->srcsize) {
          src->srcindex[src->srccount] = srcindex;
          src->srccount += 1;
        }
        if (debugmode)
          printf("exists, mapped to index %d\n", src->srcindex[0]);
        return true;
      }
    }
  }

  /* create new entry */
  SOURCEFILE *newsrc = malloc(sizeof(SOURCEFILE));
  if (newsrc == NULL) {
    fprintf(stderr, "Memory allocation error.\n");
    exit(EXIT_FAILURE);
  }
  memset(newsrc, 0, sizeof(SOURCEFILE));
  assert(filename != NULL && strlen(filename) > 0);
  newsrc->basename = strdup(filename);
  newsrc->path = (filepath != NULL && strlen(filepath) > 0) ? strdup(filepath) : NULL;
  newsrc->srcsize = 1;
  newsrc->srccount = 1;
  newsrc->srcindex = malloc(newsrc->srcsize * sizeof(int));
  if (newsrc->basename == NULL || newsrc->srcindex == NULL) {
    fprintf(stderr, "Memory allocation error.\n");
    exit(EXIT_FAILURE);
  }
  newsrc->srcindex[0] = srcindex;

  /* add to list*/
  SOURCEFILE *tail = &sources_root;
  while (tail->next != NULL)
    tail = tail->next;
  tail->next = newsrc;

  /* load the source file */
  const char *path = (newsrc->path != NULL) ? newsrc->path : newsrc->basename;
  FILE *fp = fopen(path, "rt");
  if (fp == NULL) {
    if (debugmode)
      printf("file open failed, error %d\n", errno);
    return false;
  }
  sourcefile_load(fp, &newsrc->root);
  fclose(fp);
  if (debugmode)
    printf("loaded\n");
  /* get it's timestamp too */
  newsrc->timestamp = file_timestamp(path);
  return true;
}

/** sources_clear() removes all source files from the sources lists, and
 *  optionally removes these lists too.
 *  \param free_sym   Whether to also clear the symbols.
 *  \param namelist   An array of base filenames, which is also freed (if not
 *                    NULL).
 */
static void sources_clear(bool free_sym, const char *namelist[])
{
  while (sources_root.next != NULL) {
    SOURCEFILE *src = sources_root.next;
    sources_root.next = src->next;
    assert(src->basename != NULL);
    free((void*)src->basename);
    if (src->path != NULL)
      free((void*)src->path);
    assert(src->srcindex != NULL);
    free((void*)src->srcindex);
    sourceline_clear(&src->root);
    free((void*)src);
  }

  if (free_sym && elf_symbols != NULL) {
    assert(elf_symbol_count > 0);
    elf_clear_symbols(elf_symbols, elf_symbol_count);
    free((void*)elf_symbols);
    elf_symbols = NULL;
    elf_symbol_count = 0;
  }

  if (namelist != NULL)
    free((void*)namelist);
}

static int sources_reload(const char *sourcepath, bool debugmode)
{
  int count = 0;

  char path[_MAX_PATH] = "";
  size_t pathlen = 0;
  if (sourcepath != NULL && strlen(sourcepath) > 0) {
    /* copy the source path, then check:
       - whether it has an ".elf" file extension, in which case the filename is
         stripped off;
       - whether it ends with a directory separator (which is added)
     */
    strlcpy(path, sourcepath, sizearray(path));
    translate_path(path, true);
    pathlen = strlen(path);
    if (pathlen > 4 && strcmp(path + pathlen - 4, ".elf") == 0) {
      char *ptr = strrchr(path, DIRSEP_CHAR);
      if (ptr != NULL)
        *(ptr + 1) = '\0';  /* strip off name behind the '\' or '/' */
      else
        path[0] = '\0';
    }
    if (pathlen > 0 && sourcepath[pathlen - 1] != DIRSEP_CHAR) {
      strlcat(path, DIRSEP_STR, sizearray(path));
      pathlen = strlen(path);
    }
  }

  for (SOURCEFILE *src = sources_root.next; src != NULL; src = src->next) {
    if (src->root.next != NULL)
      continue; /* source for this file was already loaded */
    assert(src->basename != NULL && strlen(src->basename) > 0);
    /* check whether a partial (relative) path exists in the path returned
       from GDB */
    bool relative_path = false;
    if (src->path != NULL) {
      char fname[_MAX_PATH];
      strlcpy(fname, src->path, sizearray(fname));
      translate_path(fname, false);
      char *ptr = strstr(fname, "/./"); /* signals the presence of a relative path */
      if (ptr != NULL) {
        path[pathlen] = '\0'; /* truncate "source path" to only the directory */
        strlcat(path, ptr + 3, sizearray(path));
        translate_path(path, true);
        if (access(path, 0) == 0)
          relative_path = true;
      }
    }
    if (!relative_path) {
      path[pathlen] = '\0';   /* truncate "source path" to only the directory */
      strlcat(path, src->basename, sizearray(path));
      translate_path(path, true);
    }
    /* attempt to load from this path */
    FILE *fp = fopen(path, "rt");
    if (fp != NULL) {
      sourcefile_load(fp, &src->root);
      fclose(fp);
      /* set (or replace) path for this source */
      translate_path(path, false);
      if (src->path != NULL)
        free((void*)src->path);
      src->path = strdup(path);
      if (debugmode)
        printf("SRC: %d: %s [%s] re-loaded\n", src->srcindex[0], src->basename, src->path);
      count++;
    }
  }

  return count;
}

static bool sources_parse(const char *gdbresult, bool debugmode)
{
  const char *head = gdbresult;
  if (*head == '^')
    head++;
  if (strncmp(head, "done", 4) == 0)
    head += 4;
  if (*head == ',')
    head++;
  if (strncmp(head, "files=", 6) != 0)
    return false;

  int fileidx = 0;
  assert(head[6] == '[');
  head += 7;  /* skip [ too */
  while (*head != ']') {
    char name[_MAX_PATH] = "", path[_MAX_PATH] = "";
    const char *sep = head;
    int len;
    assert(*head == '{');
    head++;
    if (strncmp(head, "file=", 5) == 0) {
      head += 5;
      sep = skip_string(head, ",}");
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
      sep = skip_string(head, ",}");
      len = (int)(sep - head);
      if (len >= sizearray(path))
        len = sizearray(path) - 1;
      memcpy(path, head, len);
      path[len] = '\0';
      if (path[0] == '"' && path[len - 1] == '"')
        format_string(path);
      while (*sep != '}' && *sep != '\0')
        sep++;
    }
    if (strlen(path) == 0)
      strcpy(path, name);
    const char *basename;
    for (basename = name + strlen(name) - 1; basename > name && *(basename -1 ) != '/' && *(basename -1 ) != '\\'; basename--)
      {}
    if (debugmode)
      printf("SRC: %d: %s [%s] ", fileidx, basename, path);
    sources_add(fileidx, basename, path, debugmode);
    fileidx++;
    assert(*sep != '\0');
    head = sep + 1;
    assert(*head == ',' || *head == ']');
    if (*head == ',')
      head++;
  }
  return true;
}

/** file_timestamp() returns the latest modification time of a file, which may
 *  be either the "modification" time or the "status change" time.
 */
static time_t file_timestamp(const char *path)
{
  assert(path != NULL);
  struct stat buf;
  if (stat(path, &buf) != 0)
    return 0;
  time_t tstamp = buf.st_ctime;
  if (tstamp < buf.st_mtime)
    tstamp = buf.st_mtime;
  return tstamp;
}

static unsigned sources_count(void)
{
  unsigned count = 0;
  for (SOURCEFILE *src = sources_root.next; src != NULL; src = src->next)
    count += 1;
  return count;
}

/** sources_ischanged() checks the timestamps of the entries in the sources list
 *  and returns the count of files that were changed.
 */
static unsigned sources_ischanged(void)
{
  unsigned count = 0;
  for (SOURCEFILE *src = sources_root.next; src != NULL; src = src->next) {
    const char *fname = (src->path != NULL) ? src->path : src->basename;
    assert(fname != NULL);
    if (file_timestamp(fname) != src->timestamp)
      count += 1;
  }
  return count;
}

/** elf_up_to_date() checks whether the ELF file is more recent than any of the
 *  source files. It returns nk_true if so, or nk_false if there is a source
 *  file that is more recent that the ELF file.
 */
static bool elf_up_to_date(const char *elffile)
{
  assert(elffile != NULL);
  time_t tstamp_elf = file_timestamp(elffile);
  for (SOURCEFILE *src = sources_root.next; src != NULL; src = src->next)
    if (src->timestamp > tstamp_elf)
      return false;
  return true;
}

static int source_getindex(const char *filename)
{
  const char *base;
  if ((base = strrchr(filename, '/')) != NULL)
    filename = base + 1;
# if defined _WIN32
    if ((base = strrchr(filename, '\\')) != NULL)
      filename = base + 1;
# endif

  for (SOURCEFILE *src = sources_root.next; src != NULL; src = src->next) {
    if (strcmp(filename, src->basename)== 0) {
      assert(src->srcindex != NULL && src->srccount > 0);
      return src->srcindex[0];  /* in case multiple indices map to this source, return the first */
    }
  }

  return -1;
}

static const char *source_getname(unsigned srcindex)
{
  SOURCEFILE *src = source_fromindex(srcindex);
  if (src != NULL)
    return src->basename;
  return NULL;
}

static const char **sources_getnames(unsigned *count)
{
  assert(count != NULL);
  *count = sources_count();
  if (*count == 0)
    return NULL;

  const char **namelist = (const char**)malloc(*count * sizeof(char*));
  if (namelist != NULL) {
    for (unsigned idx = 0; idx < *count; idx++)
      namelist[idx] = source_getname(idx);

    /* sort the source file list (insertion sort) */
    for (unsigned i = 1; i < *count; i++) {
      const char *key=namelist[i];
      assert(key != NULL);
      unsigned j;
      for (j = i; j > 0 && stricmp(namelist[j-1], key) > 0; j--)
        namelist[j] = namelist[j-1];
      namelist[j] = key;
    }
  }

  return namelist;
}

typedef struct tagBREAKPOINT {
  struct tagBREAKPOINT *next;
  short number; /* sequential number, as assigned by GDB */
  short type;   /* 0 = breakpoint, 1 = watchpoint, 2 = dprintf */
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

#define BKPTTYPE_BREAK    0
#define BKPTTYPE_WATCH    1
#define BKPTTYPE_DPRINTF  2

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
      ptr = skip_string(ptr, NULL);
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
    const char *tail = skip_string(ptr, NULL);
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
    assert(strncmp(start, "bkpt", 4) == 0);
    start = skipwhite(start + 4);
    assert(*start == '=');
    start = skipwhite(start + 1);
    assert(*start == '{');
    start = skipwhite(start + 1);
    const char *tail = strchr_nest(start, '}');
    assert(tail != NULL);
    size_t len = tail - start;
    char *line;
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
          if (strncmp(start, "breakpoint", 10) == 0)
            bp->type = BKPTTYPE_BREAK;
          else if (strncmp(start, "dprintf", 7) == 0)
            bp->type = BKPTTYPE_DPRINTF;
          else
            bp->type = BKPTTYPE_WATCH;
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
          char filename[_MAX_PATH];
          start = fieldvalue(start, &len);
          assert(start != NULL);
          if (len >= sizearray(filename))
            len = sizearray(filename) - 1;
          strncpy(filename, start, len);
          filename[len] = '\0';
          bp->filenr = (short)source_getindex(filename);
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
          {}
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


enum {
  FORMAT_NATURAL,
  FORMAT_DECIMAL,
  FORMAT_HEX,
  FORMAT_OCTAL,
  FORMAT_BINARY,
  FORMAT_STRING,
};

static int change_integer_format(char *value, size_t size, int format)
{
  if (format == FORMAT_NATURAL)
    return 0;

  assert(value != NULL);
  assert(strlen(value) < size);

  const char *head = skipwhite(value);
  if (format == FORMAT_STRING) {
    if (*head != '{')
      return 0; /* value not an array (cannot be converted to string) */
    char *buffer = malloc((size + 1) * sizeof(char));
    if (buffer == NULL)
      return 0;
    strlcpy(buffer, head + 1, size);
    value[0] = '"';
    size_t idx = 1;
    char *tail;
    for (head = buffer; idx < (size - 1) && *head != '\0'; head = tail, idx++) {
      long c = strtol(head, &tail, 0);
      value[idx] = (char)c;
      if (*tail == ',')
        tail += 1;
      tail = (char*)skipwhite(tail);
    }
    value[idx] = '"';
    value[idx + 1] = '\0';
    free((void*)buffer);
  } else {
    char *buffer = NULL;
    size_t buf_count = 0;
    union {
      long s;
      unsigned long u;
    } v;
    int is_signed = 0;
    if (*head == '"') {
      /* if the format is currently a string, collect the bytes */
      buffer = malloc(size * sizeof(unsigned char));
      if (buffer == NULL)
        return 0;
      head = skipwhite(head + 1);
      while (*head != '\0' && *head != '"') {
        assert(buf_count < size);
        if (*head == '\\') {
          head++;
          switch (*head) {
          case '"':
          case '\'':
          case '\\':
            buffer[buf_count++] = *head++;
            break;
          case 'b':
            buffer[buf_count++] = '\b';
            break;
          case 'n':
            buffer[buf_count++] = '\n';
            break;
          case 'r':
            buffer[buf_count++] = '\r';
            break;
          case 't':
            buffer[buf_count++] = '\t';
            break;
          default:
            if (isdigit(*head)) {
              int val = 0;
              if (*head == '0' && *(head + 1) == 'x') {
                head += 2;
                for (int i = 0; i < 2 && isxdigit(*head); i++, head++) {
                  if ('a' <= *head && *head <= 'f')
                    val = (val << 4) + (*head - 'a') + 10;
                  else if ('A' <= *head && *head <= 'F')
                    val = (val << 4) + (*head - 'A') + 10;
                  else
                    val = (val << 4) + (*head - '0');
                }
              } else {
                for (int i = 0; i < 3 && isdigit(*head); i++, head++)
                  val = (val << 3) + (*head - '0');
              }
              buffer[buf_count++] = (unsigned char)val;
            }
          }
        } else {
          buffer[buf_count++] = *head++;
        }
      }
    } else {
      char *tail;
      if (head[0] == '-') {
        v.s = strtol(head, &tail, 0);
        is_signed = 1;
      } else {
        v.u = strtoul(head, &tail, 0);
      }
      if (*skipwhite(tail) != '\0')
        return 0; /* the contents of "value" do not form a valid integer */
    }

    /* so this is a valid integer (signed or unsigned), or a valid buffer */
    unsigned buf_idx = 0;
    do {
      if (buf_count > 0) {
        v.u = buffer[buf_idx];
        if (buf_idx == 0)
          strlcpy(value, "{ ", size);
        else
          strlcat(value, ", ", size);
      }
      char valstr[65];
      unsigned long bitmask;
      int idx;
      switch (format) {
      case FORMAT_DECIMAL:
        if (is_signed)
          snprintf(valstr, sizearray(valstr), "%ld", v.s);
        else
          snprintf(valstr, sizearray(valstr), "%lu", v.u);
        strlcat(value, valstr, size);
        break;
      case FORMAT_HEX:
        snprintf(valstr, sizearray(valstr), "0x%lx", v.u);
        strlcat(value, valstr, size);
        break;
      case FORMAT_OCTAL:
        snprintf(valstr, sizearray(valstr), "0%lo", v.u);
        strlcat(value, valstr, size);
        break;
      case FORMAT_BINARY:
        idx = 0;
        bitmask = ~0;
        while ((v.u & LONG_MIN) == 0) {
          v.u <<= 1;  /* ignore leading zeros */
          bitmask <<= 1;
        }
        while (bitmask != 0) {
          if (idx < sizearray(valstr) - 1)
            valstr[idx++] = ((v.u & LONG_MIN) == 0) ? '0' : '1';
          v.u <<= 1;
          bitmask <<= 1;
        }
        valstr[idx] = '\0';
        strlcat(value, valstr, size);
        break;
      }
    } while (++buf_idx < buf_count);
    if (buf_count > 0) {
      strlcat(value, " }", size);
      /* if the contents do not fit, end with a ... */
      if (strchr(value, '}') == NULL) {
        int len = strlen(value);
        assert(len > 3);
        value[len - 1] = '.';
        value[len - 2] = '.';
        value[len - 3] = '.';
      }
    }
    if (buffer != NULL)
      free((void*)buffer);
  }

  return 1;
}

typedef struct tagLOCALVAR {
  struct tagLOCALVAR *next;
  char *name;
  char *value;
  char *value_fmt;  /* reformatted value */
  unsigned short flags;
  unsigned short format;
} LOCALVAR;
#define LOCALFLG_INSCOPE  0x0001
#define LOCALFLG_CHANGED  0x0002

static LOCALVAR localvar_root = { NULL };

static void locals_clear(void)
{
  while (localvar_root.next != NULL) {
    LOCALVAR *var = localvar_root.next;
    localvar_root.next = var->next;
    assert(var->name != NULL);
    free((void*)var->name);
    assert(var->value != NULL);
    free((void*)var->value);
    if (var->value_fmt != NULL)
      free((void*)var->value_fmt);
    free((void*)var);
  }
}

static int locals_update(const char *gdbresult)
{
  /* clear all changed & in-scope flags */
  LOCALVAR *var;
  for (var = localvar_root.next; var != NULL; var = var->next)
    var->flags = 0;

  if (strncmp(gdbresult, "done", 4) != 0)
    return 0;
  const char *head;
  if ((head = strchr(gdbresult, ',')) == NULL)
    return 0;
  head = skipwhite(head + 1);
  if (strncmp(head, "variables", 9) != 0)
    return 0;
  head = skipwhite(head + 9);
  assert(*head == '=');
  head = skipwhite(head + 1);
  assert(*head == '[');
  head = skipwhite(head + 1);
  int count = 0;
  while (*head != ']') {
    const char *tail;
    char *line;
    size_t len;
    assert(*head == '{');
    head = skipwhite(head + 1);
    tail = str_matchchar(head, '}');
    assert(tail != NULL);
    len = tail - head;
    if ((line = malloc((len + 1) * sizeof(char))) != NULL) {
      strncpy(line, head, len);
      line[len] = '\0';
      if ((head=fieldfind(line, "name")) != NULL) {
        size_t namelen;
        const char *name = fieldvalue(head, &namelen);
        assert(name != NULL);
        if ((head = fieldfind(line, "value")) != NULL) {
          size_t valuelen;
          const char *value = fieldvalue(head, &valuelen);
          assert(value != NULL);
          /* copy the value in a temporary string */
#         define LOCALVAR_MAX 32
          char valstr[LOCALVAR_MAX + 4]; /* +3 for "...", +1 for '\0' */
          size_t copylen = (valuelen <= LOCALVAR_MAX) ? valuelen : LOCALVAR_MAX;
          if (*value == '\\' && *(value + 1) == '"') {
            /* parse strings in a special way, to handle escaped codes */
            unsigned value_idx = 2;
            unsigned idx = 0;
            valstr[idx++] = '"';
            while (idx < copylen && value_idx < valuelen) {
              if (value[value_idx] == '\\' && value[value_idx + 1] == '\\') {
                valstr[idx] = '\\';
                value_idx += 1;
              } else {
                valstr[idx] = value[value_idx];
              }
              value_idx += 1;
              idx += 1;
            }
            valstr[idx] = '\0';
            assert(idx >= 2);
            if (valstr[idx - 1] == '"' && valstr[idx - 2] == '\\') {
              /* transform trailing \" to " */
              valstr[idx - 2] = '"';
              valstr[idx - 1] = '\0';
            } else if (idx >= LOCALVAR_MAX - 1) {
              valstr[LOCALVAR_MAX - 1] = '\0';
              strlcat(valstr, "...\"", sizearray(valstr));
            }
          } else {
            strncpy(valstr, value, copylen);
            valstr[copylen] = '\0';
            if (valuelen > LOCALVAR_MAX)
              strlcat(valstr, "...", sizearray(valstr));
          }
          /* see if the local variable already exists (to detect changes in its value) */
          for (var = localvar_root.next; var != NULL && (strlen(var->name) != namelen || strncmp(name, var->name, namelen) != 0); var = var->next)
            {}
          if (var != NULL) {
            assert(var->value != NULL);
            if (strcmp(var->value, valstr) != 0) {
              free((void*)var->value);
              var->value = strdup(valstr);
              var->flags |= LOCALFLG_CHANGED;
              /* change format (if an explicit format was selected) */
              if (var->value_fmt != NULL) {
                free((void*)var->value_fmt);
                var->value_fmt = NULL;
              }
              if (change_integer_format(valstr, sizearray(valstr), var->format) && strcmp(valstr, var->value) != 0)
                var->value_fmt = strdup(valstr);
            }
            var->flags |= LOCALFLG_INSCOPE;
          } else {
            /* local variable isn't found -> create it */
            var = malloc(sizeof(LOCALVAR));
            if (var != NULL) {
              var->name = strdup_len(name, namelen);
              var->value = strdup(valstr);
              var->value_fmt = NULL;
              if (var->name != NULL && var->value != NULL) {
                var->flags |= LOCALFLG_INSCOPE | LOCALFLG_CHANGED;
                var->format = FORMAT_NATURAL;
                var->next = localvar_root.next;
                localvar_root.next = var;
              } else {
                free((void*)var->name);
                free((void*)var->value);
              }
            }
          }
        }
      }
      free((void*)line);
      count++;
    }
    head = skipwhite(tail + 1);
    if (*head == ',')
      head = skipwhite(head + 1);
  }

  /* now that we've parsed all locals that GDB returned (i.e. all locals that are
     in scope), delete the locals in the list that were not touched (i.e. that
     no longer exist or are unreachable in this scope) */
  LOCALVAR *prev =&localvar_root;
  var = localvar_root.next;
  while (var != NULL) {
    if (var->flags & LOCALFLG_INSCOPE) {
      prev = var;
      var = var->next;
      continue;
    }
    assert(prev != NULL);
    prev->next = var->next;
    assert(var->name != NULL);
    free((void*)var->name);
    assert(var->value != NULL);
    free((void*)var->value);
    if (var->value_fmt != NULL)
      free((void*)var->value_fmt);
    free((void*)var);
    var = prev->next;
  }

  return count;
}


typedef struct tagWATCH {
  struct tagWATCH *next;
  char *expr;
  char *value;
  char *type;
  unsigned int seqnr;
  unsigned short flags;
  unsigned short format;
} WATCH;
#define WATCHFLG_INSCOPE  0x0001
#define WATCHFLG_CHANGED  0x0002

static WATCH watch_root = { NULL };

/* watch_add() parses the reply for adding a variable watch (appends a new
   "watch" entry to the list). It returns the watch sequence number on success,
   or 0 on failure */
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
    {}
  tail->next = watch;
  return seqnr;
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
    tail = str_matchchar(start, '}');
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
          {}
        assert(watch != NULL);
        if (watch != NULL) {
          if (watch->value != NULL) {
            free((void*)watch->value);
            watch->value = NULL;
          }
          if ((start = fieldfind(line, "value")) != NULL) {
#           define WATCH_MAX 32
            start = fieldvalue(start, &len);
            assert(start != NULL);
            if (len <= WATCH_MAX) {
              watch->value = strdup_len(start, len);
            } else {
              char tmpstr[WATCH_MAX + 4]; /* +3 for "...", +1 for '\0' */
              strncpy(tmpstr, start, WATCH_MAX);
              tmpstr[WATCH_MAX] = '\0';
              strcat(tmpstr, "...");
              watch->value = strdup(tmpstr);
            }
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

static bool watch_update_format(unsigned seqnr, const char *gdbresult)
{
  WATCH *watch;

  /* find the watch before anything else */
  for (watch = watch_root.next; watch != NULL && watch->seqnr != seqnr; watch = watch->next)
    {}
  assert(watch != NULL);
  if (watch == NULL)
    return false;

  if (strncmp(gdbresult, "done", 4) != 0)
    return false;

  const char *start;
  if ((start = strchr(gdbresult, ',')) == NULL)
    return false;
  start = skipwhite(start + 1);
  if (strncmp(start, "format", 6) != 0)
    return false;
  start = skipwhite(start + 6);
  assert(*start == '=');
  start = skipwhite(start + 1);
  assert(*start == '"');
  if (strncmp(start + 1, "natural", 7) == 0)
    watch->format = FORMAT_NATURAL;
  else if (strncmp(start + 1, "decimal", 7) == 0)
    watch->format = FORMAT_DECIMAL;
  else if (strncmp(start + 1, "hexadecimal", 11) == 0)
    watch->format = FORMAT_HEX;
  else if (strncmp(start + 1, "octal", 5) == 0)
    watch->format = FORMAT_OCTAL;
  else if (strncmp(start + 1, "binary", 6) == 0)
    watch->format = FORMAT_BINARY;
  start = skip_string(start, NULL);
  if (*start == ',')
    start += 1;
  start = skipwhite(start);
  if (strncmp(start, "value", 5) != 0)
    return 0;
  if (watch->value != NULL) {
    free((void*)watch->value);
    watch->value = NULL;
  }
  size_t len;
  start = fieldvalue(start, &len);
  assert(start != NULL);
  watch->value = strdup_len(start, len);
  return true;
}


typedef struct tagREGISTER_DEF {
  const char *name;
  unsigned long value;
  unsigned short flags;
} REGISTER_DEF;
#define REGFLG_CHANGED    0x0002
static REGISTER_DEF register_def[] = {
  { "r0", 0, 0 },
  { "r1", 0, 0 },
  { "r2", 0, 0 },
  { "r3", 0, 0 },
  { "r4", 0, 0 },
  { "r5", 0, 0 },
  { "r6", 0, 0 },
  { "r7", 0, 0 },
  { "r8", 0, 0 },
  { "r9", 0, 0 },
  { "r10", 0, 0 },
  { "r11", 0, 0 },
  { "r12", 0, 0 },
  { "sp", 0, 0 },
  { "lr", 0, 0 },
  { "pc", 0, 0 },
};

static bool registers_update(const char *gdbresult)
{
  /* clear all changed flags */
  for (int idx = 0; idx < sizearray(register_def); idx++)
    register_def[idx].flags = 0;

  if (strncmp(gdbresult, "done", 4) != 0)
    return false;
  const char *head;
  if ((head = strchr(gdbresult, ',')) == NULL)
    return false;
  head = skipwhite(head + 1);
  if (strncmp(head, "register-values", 15) != 0)
    return false;
  head = skipwhite(head + 15);
  assert(*head == '=');
  head = skipwhite(head + 1);
  assert(*head == '[');
  head = skipwhite(head + 1);
  while (*head != ']') {
    const char *tail;
    assert(*head == '{');
    head = skipwhite(head + 1);
    tail = str_matchchar(head, '}');
    assert(tail != NULL);
    if (strncmp(head, "number", 6) == 0) {
      const char *ptr = skipwhite(head + 6);
      assert(*ptr == '=');
      ptr = skipwhite(ptr + 1);
      assert(*ptr == '"');
      int reg = (int)strtol(ptr + 1, (char**)&ptr, 10);
      ptr = skipwhite(ptr);
      assert(*ptr == '"');
      ptr = skipwhite(ptr + 1);
      assert(*ptr == ',');
      ptr = skipwhite(ptr + 1);
      if (strncmp(ptr, "value", 5) == 0) {
        ptr = skipwhite(ptr + 5);
        assert(*ptr == '=');
        ptr = skipwhite(ptr + 1);
        assert(*ptr == '"');
        unsigned long val = strtoul(ptr + 1, NULL, 0);
        if (reg >= 0 && reg < sizearray(register_def) && register_def[reg].value != val) {
          register_def[reg].value = val;
          register_def[reg].flags = REGFLG_CHANGED;
        }
      }
    }
    head = skipwhite(tail + 1);
    if (*head == ',')
      head = skipwhite(head + 1);
  }

  return true;
}

static const char *lastdirsep(const char *path)
{
  const char *ptr;

  if ((ptr = strrchr(path, DIRSEP_CHAR)) == NULL)
    ptr = path;
# if defined _WIN32
    if (strrchr(ptr, '/') != NULL)
      ptr = strrchr(ptr, '/');
# endif

  return (ptr == path) ? NULL : ptr;
}

static bool ctf_findmetadata(const char *target, char *metadata, size_t metadata_len)
{
  assert(target != NULL);
  assert(metadata != NULL);

  if (strcmp(metadata, "-") == 0)
    return false; /* metadata explicitly disabled */

  /* if no metadata filename was set, create the base name (without path) from
     the target name; add a .tsdl extension */
  char basename[_MAX_PATH];
  if (strlen(metadata) == 0) {
    char *ptr = (char*)lastdirsep(target);
    strlcpy(basename, (ptr == NULL) ? target : ptr + 1, sizearray(basename));
    if ((ptr = strrchr(basename, '.')) != NULL)
      *ptr = '\0';
    strlcat(basename, ".tsdl", sizearray(basename));
  } else {
    char *ptr = (char*)lastdirsep(metadata);
    if (ptr == NULL)
      ptr = metadata;
    if (strchr(ptr, '.') != NULL) {
      /* there is a filename with extension in the metadata parameter already */
      strlcpy(basename, metadata, sizearray(basename));
    } else {
      /* there appears to be only a path in the metadata parameter */
      strlcpy(basename, metadata, sizearray(basename));
      assert(ptr != NULL);
      if (*(ptr + 1) != '\0') {
#       if defined _WIN32
          strlcat(basename, "\\", sizearray(basename));
#       else
          strlcat(basename, "/", sizearray(basename));
#       endif
      }
      ptr = (char*)lastdirsep(target);
      strlcat(basename, (ptr == NULL) ? target : ptr + 1, sizearray(basename));
      if ((ptr = strrchr(basename, '.')) != NULL)
        *ptr = '\0';
      strlcat(basename, ".tsdl", sizearray(basename));
    }
  }

  /* try current directory */
  if (access(basename, 0) == 0) {
    strlcpy(metadata, basename, metadata_len);
    return true;
  }

  /* try target directory (if there is one) */
  const char *ptr = lastdirsep(target);
  if (ptr != NULL) {
    char path[_MAX_PATH];
    unsigned len = min(ptr - target, sizearray(path) - 2);
    strncpy(path, target, len);
    path[len] = DIRSEP_CHAR;
    path[len + 1] = '\0';
    strlcat(path, basename, sizearray(path));
    translate_path(path, true);
    if (access(path, 0) == 0) {
      strlcpy(metadata, path, metadata_len);
      return true;
    }
  }

  /* try directories in the sources array */
  for (SOURCEFILE *src = sources_root.next; src != NULL; src = src->next) {
    ptr = lastdirsep(src->path);
    if (ptr != NULL) {
      char path[_MAX_PATH];
      unsigned len = min(ptr - src->path, sizearray(path) - 2);
      strncpy(path, src->path, len);
      path[len] = DIRSEP_CHAR;
      path[len + 1] = '\0';
      strlcat(path, basename, sizearray(path));
      translate_path(path, true);
      if (access(path, 0) == 0) {
        strlcpy(metadata, path, metadata_len);
        return true;
      }
    }
  }

  return false;
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
      snprintf(msg, sizearray(msg), "TSDL file error, line %d: ", linenr);
    else
      strlcpy(msg, "TSDL file error: ", sizearray(msg));
    strlcat(msg, message, sizearray(msg));
    tracelog_statusmsg(TRACESTATMSG_CTF, msg, 0);
  }
  return 0;
}

static char *enquote(char *dest, const char *source, size_t dest_size)
{
  if (strchr(source, ' ') != NULL) {
    strlcpy(dest, "\"", dest_size);
    strlcat(dest, source, dest_size);
    strlcat(dest, "\"", dest_size);
  } else {
    strlcpy(dest, source, dest_size);
  }
  return dest;
}


static int check_stopped(int *filenr, int *linenr, uint32_t *address)
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
        if ((head = strstr(item->text, "file=")) != NULL) {
          char filename[_MAX_PATH];
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
          /* look up the file (but there is the possibility that no source files
             were yet loaded) */
          if (sources_root.next != NULL) {
            assert(filenr != NULL);
            *filenr = source_getindex(filename);
          }
        }
        if ((head = strstr(item->text, "line=")) != NULL) {
          assert(head[5] == '"');
          head += 6;
          assert(linenr != NULL);
          *linenr = (int)strtol(head, NULL, 10);
        }
        if ((head = strstr(item->text, "addr=")) != NULL) {
          assert(head[5] == '"');
          head += 6;
          assert(address != NULL);
          *address = (uint32_t)strtoul(head, NULL, 0);
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

void task_init(TASK *task)
{
  assert(task != NULL);
  memset(task, -1, sizeof(TASK));
  task->hProcess = task->hThread = INVALID_HANDLE_VALUE;
}

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

bool task_isrunning(TASK *task)
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

  task_init(task);
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

/** translate_path()
 *  \param path     The path name
 *  \param native   Set to 1 to convert slashes to backslashes (required by C
 *                  library); set to 0 to convert backslashes to slashes
 *                  (required by GDB)
 *  \return A pointer to the translated name (parameter "path").
 */
static char *translate_path(char *path, bool native)
{
  char *p;
  assert(path != NULL);
  if (native) {
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

bool task_isrunning(TASK *task);

void task_init(TASK *task)
{
  assert(task != NULL);
  memset(task, 0, sizeof(TASK));
}

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
    exit(EXIT_FAILURE);  /* if execv() returns, there was an error */
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

bool task_isrunning(TASK *task)
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

  task_init(task);
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

static char *translate_path(char *path, bool native)
{
  (void)native;
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

#define WINDOW_WIDTH    750     /* default window size (window is resizable) */
#define WINDOW_HEIGHT   500
#define FONT_HEIGHT     14      /* default font size */
#define ROW_HEIGHT      (1.6f * opt_fontsize)  /* extra spaced rows (for controls) */
#define COMBOROW_CY     (0.9f * opt_fontsize)
#define BUTTON_WIDTH    (3.0f * opt_fontsize)
#define BROWSEBTN_WIDTH (1.5f * opt_fontsize)

static float opt_fontsize = FONT_HEIGHT;


static int textview_widget(struct nk_context *ctx, const char *id,
                           const STRINGLIST *content, float rowheight)
{
  int linecount = 0;

  /* dark background on group */
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
  nk_style_push_vec2(ctx, &ctx->style.window.group_padding, nk_vec2(6, 4));
  if (nk_group_begin(ctx, id, NK_WINDOW_BORDER)) {
    struct nk_user_font const *font = ctx->style.font;
    float linewidth = 0;
    const STRINGLIST *item;
    for (item = content; item != NULL; item = item->next) {
      /* do a word-wrap routine */
      int indent = 0;
      assert(item->text != NULL);
      const char *head = item->text;
      do {
        nk_layout_row_begin(ctx, NK_STATIC, rowheight, 1 + (indent > 0));
        if (linewidth < 0.1) {
          struct nk_rect rcline = nk_layout_widget_bounds(ctx);
          linewidth = rcline.w;
        }
        const char *tail = head + strlen(head);
        float textwidth;
        for ( ;; ) {
          assert(font != NULL && font->width != NULL);
          textwidth = font->width(font->userdata, font->height, head, (int)(tail - head));
          if (textwidth <= linewidth - indent)
            break;
          while (tail != head && *(tail - 1) > ' ')
            tail -= 1;
          if (tail == head) {
            tail = head + strlen(head); /* a non-breakable very long word -> panic */
            break;
          }
          tail -= 1;  /* also skip the ' ' we stopped on */
        }
        if (indent > 0) {
          nk_layout_row_push(ctx, (float)indent);
          nk_spacing(ctx, 1);
        }
        nk_layout_row_push(ctx, textwidth + 4);
        nk_text(ctx, head, (int)(tail - head), NK_TEXT_LEFT);
        nk_layout_row_end(ctx);
        /* prepare for next part to the string */
        linecount += 1;
        if (strstr(head, " -- ") != NULL)
          indent = 40;
        head = tail;
        while (*head == ' ')
          head += 1;
      } while (*head != '\0');
    }
    /* if there are no lines at all, show that there is no information */
    if (linecount == 0) {
      nk_layout_row_dynamic(ctx, rowheight, 1);
      nk_label(ctx, "No information on this topic.", NK_TEXT_LEFT);
      linecount += 1;
    }
    /* add an empty line to fill up any remaining space below */
    nk_layout_row_dynamic(ctx, rowheight, 1);
    nk_spacing(ctx, 1);
    nk_group_end(ctx);
  }
  nk_style_pop_vec2(ctx);
  nk_style_pop_color(ctx);
  return linecount;
}

/* console_widget() draws the text in the console window and scrolls to the last
   line if new text was added */
static void console_widget(struct nk_context *ctx, const char *id, float rowheight)
{
  STRINGLIST *item;
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window const *stwin = &ctx->style.window;
  struct nk_user_font const *font = ctx->style.font;

  /* black background on group */
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, NK_WINDOW_BORDER)) {
    int lines = 0;
    float lineheight = 0;
    for (item = consolestring_root.next; item != NULL; item = item->next) {
      float textwidth;
      if (item->flags & console_hiddenflags)
        continue;
      assert(item->text != NULL);
      nk_layout_row_begin(ctx, NK_STATIC, rowheight, 1);
      if (lineheight <= 0.1) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        lineheight = rcline.h;
      }
      /* calculate size of the text */
      assert(font != NULL && font->width != NULL);
      textwidth = font->width(font->userdata, font->height, item->text, strlen(item->text)) + 10;
      nk_layout_row_push(ctx, textwidth);
      if (item->flags & (STRFLG_INPUT | STRFLG_MI_INPUT))
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_YELLOW);
      else if (item->flags & STRFLG_ERROR)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_RED);
      else if (item->flags & STRFLG_RESULT)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_CYAN);
      else if (item->flags & STRFLG_NOTICE)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_PURPLE);
      else if (item->flags & STRFLG_STATUS)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_YELLOW);
      else if (item->flags & STRFLG_EXEC)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_GREEN);
      else if (item->flags & STRFLG_LOG)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_AQUA);
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
      static int scrollpos = 0;
      static int linecount = 0;
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
static uint32_t exec_address = 0;
static float source_lineheight = 0;
static float source_charwidth = 0;
static int source_vp_rows = 0;
static bool source_force_refresh = false;
static bool source_autoscroll = true;    /* scrolling due to stepping through code (as opposed to explicit scrolling) */

/* returns the line number as appears in the source code widget, that maps to
   the line in the source code; these two are the same in source view, but
   different in disassembly view */
static int line_source2phys(int fileindex, int source_line)
{
  assert(fileindex >= 0);
  int line = 1;
  SOURCELINE *item;
  for (item = sourceline_get(fileindex, 1); item != NULL; item = item->next) {
    if (item->hidden)
      continue;
    if (item->linenumber == source_line)
      break;
    line += 1;
  }
  return (item != NULL) ? line : source_line;
}

/* returns the line number in the source file that is *on* or *before* the
   physical line that is passed in */
static int line_phys2source(int fileindex, int phys_line)
{
  assert(fileindex >= 0);
  int line = 1;
  for (SOURCELINE *item = sourceline_get(fileindex, 1); item != NULL && phys_line > 0; item = item->next) {
    if (item->hidden)
      continue;
    if (item->linenumber > 0)
      line = item->linenumber;
    phys_line -= 1;
  }
  return line;
}

/* returns the line number as appears in the source code widget, that maps to
   the address (or the line just before the address) */
static int line_addr2phys(int fileindex, uint32_t address)
{
  assert(fileindex >= 0);
  int best_line = 1;
  uint32_t low_addr = 0;
  int line = 1;
  for (SOURCELINE *item = sourceline_get(fileindex, 1); item != NULL; item = item->next) {
    if (item->hidden)
      continue;
    if (item->address >= low_addr && item->address <= address) {
      low_addr = item->address;
      best_line = line;
    }
    line += 1;
  }
  return best_line;
}

/* returns the address for the given line number in the source file, or 0 on error */
static uint32_t line_phys2addr(int fileindex, int phys_line)
{
  assert(fileindex >= 0);
  SOURCELINE *item;
  for (item = sourceline_get(fileindex, 1); item != NULL; item = item->next) {
    if (item->hidden)
      continue;
    if (--phys_line == 0)
      break;
  }
  return (item == NULL) ? 0 : item->address;
}

/* source_widget() draws the text of a source file */
static void source_widget(struct nk_context *ctx, const char *id, float rowheight,
                          bool grayed, bool disassembly)
{
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window const *stwin = &ctx->style.window;

  /* preset common parts of the new button style */
  struct nk_style_button stbtn = ctx->style.button;
  stbtn.border = 0;
  stbtn.rounding = 0;
  stbtn.padding.x = stbtn.padding.y = 0;

  /* monospaced font */
  int fonttype = guidriver_setfont(ctx, FONT_MONO);
  struct nk_user_font const *font = ctx->style.font;

  /* black background on group */
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, NK_WINDOW_BORDER)) {
    int lines = 0, maxlen = 0;
    float maxwidth = 0;
    for (SOURCELINE *item = sourceline_get(source_cursorfile, 1); item != NULL; item = item->next) {
      if (item->hidden)
        continue;
      lines++;
      assert(item->text != NULL);
      nk_layout_row_begin(ctx, NK_STATIC, rowheight, 4);
      if (source_lineheight <= 0.1) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        source_lineheight = rcline.h;
      }
      /* line number or active/breakpoint markers */
      BREAKPOINT *bkpt;
      if ((bkpt = breakpoint_lookup(source_cursorfile, item->linenumber)) != NULL) {
        nk_layout_row_push(ctx, rowheight - ctx->style.window.spacing.x);
        nk_spacing(ctx, 1);
        /* breakpoint marker */
        nk_layout_row_push(ctx, rowheight);
        assert(bkpt != NULL);
        stbtn.normal.data.color = stbtn.hover.data.color
          = stbtn.active.data.color = stbtn.text_background
          = COLOUR_BG0;
        stbtn.text_normal = stbtn.text_active = stbtn.text_hover = COLOUR_BG_RED;
        nk_button_symbol_styled(ctx, &stbtn, bkpt->enabled ? NK_SYMBOL_CIRCLE_SOLID : NK_SYMBOL_CIRCLE_OUTLINE);
      } else if (item->linenumber != 0) {
        nk_layout_row_push(ctx, 2 * rowheight);
        char str[20];
        sprintf(str, "%4d", item->linenumber);
        if (grayed)
          nk_label_colored(ctx, str, NK_TEXT_LEFT, COLOUR_FG_GRAY);
        else if (lines == source_cursorline)
          nk_label_colored(ctx, str, NK_TEXT_LEFT, COLOUR_FG_YELLOW);
        else
          nk_label(ctx, str, NK_TEXT_LEFT);
      } else {
        nk_layout_row_push(ctx, 2 * rowheight);
        nk_spacing(ctx, 1);
      }
      /* active line marker */
      nk_layout_row_push(ctx, rowheight / 2);
      bool is_exec_point = false;
      if (source_cursorfile == source_execfile) {
        if (disassembly)
          is_exec_point = (item->address == exec_address);
        else
          is_exec_point = (item->linenumber == source_execline);
      }
      if (is_exec_point) {
        stbtn.normal.data.color = stbtn.hover.data.color
          = stbtn.active.data.color = stbtn.text_background
          = COLOUR_BG0;
        stbtn.text_normal = stbtn.text_active = stbtn.text_hover = COLOUR_FG_YELLOW;
        nk_button_symbol_styled(ctx, &stbtn, NK_SYMBOL_TRIANGLE_RIGHT);
      } else {
        nk_spacing(ctx, 1);
      }
      /* calculate size of the text */
      assert(font != NULL && font->width != NULL);
      float textwidth = font->width(font->userdata, font->height, item->text, strlen(item->text));
      if (textwidth > maxwidth) {
        maxwidth = textwidth;
        maxlen = strlen(item->text);
      }
      nk_layout_row_push(ctx, textwidth + 10);
      if (grayed)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_GRAY);
      else if (lines == source_cursorline)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_YELLOW);
      else if (item->linenumber == 0)
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, COLOUR_FG_AQUA);
      else
        nk_label(ctx, item->text, NK_TEXT_LEFT);
      nk_layout_row_end(ctx);
    }
    nk_layout_row_dynamic(ctx, rowheight, 1);
    nk_spacing(ctx, 1);
    if (lines == 0) {
      nk_layout_row_dynamic(ctx, rowheight, 1);
      nk_label(ctx, "NO SOURCE", NK_TEXT_CENTERED);
    }
    nk_group_end(ctx);
    if (maxlen > 0)
      source_charwidth = maxwidth / maxlen;
    source_vp_rows = (int)((rcwidget.h - 2 * stwin->padding.y) / source_lineheight);
    if (lines > 0) {
      static int saved_execfile = 0, saved_execline = 0;
      static int saved_cursorline = 0;
      if (saved_execline != source_execline || saved_execfile != source_execfile || source_force_refresh) {
        saved_execfile = source_execfile;
        saved_execline = source_execline;
        source_cursorline = line_source2phys(source_cursorfile, source_execline);
        source_force_refresh = false;
      }
      if (saved_cursorline != source_cursorline) {
        /* calculate scrolling: make cursor line fit in the window */
        int extra_lines = source_autoscroll ? min(source_vp_rows / 2, 8) : 0; /* show extra lines above/below scroll position */
        unsigned xscroll, yscroll;
        nk_group_get_scroll(ctx, id, &xscroll, &yscroll);
        int topline = (int)(yscroll / source_lineheight);
        int newtop = topline;
        if (source_cursorline <= topline + 1) {
          newtop = source_cursorline - 1 - extra_lines;
          if (newtop < 0)
            newtop = 0;
        } else if (source_cursorline >= topline + source_vp_rows - 1 && lines > 3) {
          newtop = source_cursorline - source_vp_rows + 1 + extra_lines;
          if (newtop + source_vp_rows >= lines)
            newtop = source_cursorline - source_vp_rows + 1;
        }
        if (newtop != topline) {
          topline = newtop;
          nk_group_set_scroll(ctx, id, 0, (nk_uint)(topline * source_lineheight));
        }
        saved_cursorline = source_cursorline;
      }
    }
  }
  nk_style_pop_color(ctx);
  guidriver_setfont(ctx, fonttype);
  source_autoscroll = true; /* auto-scrolling is set to false before explicit scrolling, but automatically switched back on */
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
  assert(ctx != NULL);
  struct nk_mouse *mouse = &ctx->input.mouse;
  assert(mouse != NULL);
  if (!NK_INBOX(mouse->pos.x, mouse->pos.y, widget_bounds.x, widget_bounds.y, widget_bounds.w, widget_bounds.h))
    return 0;
  assert(id != NULL);
  unsigned xscroll, yscroll;
  nk_group_get_scroll(ctx, id, &xscroll, &yscroll);
  if (source_lineheight < 0.1)
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
static bool source_getsymbol(char *symname, size_t symlen, int row, int col)
{
  assert(symname != NULL && symlen > 0);
  *symname = '\0';
  if (row < 1 || col < 1)
    return false;
  SOURCELINE *item = sourceline_get(source_cursorfile, row);
  if (item == NULL)
    return false;
  assert(item->text != NULL);
  if ((unsigned)col > strlen(item->text))
    return false;
  /* when moving to the left, skip '.' and '->' to complete the structure field
     with the structure variable; also skip '*' so that pointing at '*ptr' shows
     the dereferenced value */
  const char *head = item->text + (col - 1);
  if (!isalpha(*head) && !isdigit(*head) && *head != '_')
    return false;
  while (head > item->text
         && (isalpha(*(head - 1)) || isdigit(*(head - 1)) || *(head - 1) == '_'
             || *(head - 1) == '.' || (*(head - 1) == '>' && *(head - 2) == '-') || (*(head - 1) == '-' && *head == '>')
             || *(head - 1) == '*'))
    head--;
  if (!isalpha(*head) && *head != '_' && *head != '*')
    return false;       /* symbol must start with a letter or '_' (but make an exception for the '*' prefix) */
  /* run from the start of the line to the head, check for preprocessor directives,
     comments and literal strings (it does not work well for multi-line comments
     and it also does not take continued lines into account) */
  const char *ptr = skipwhite(item->text);
  if (*ptr == '#')
    return false;       /* symbol is on a preprocessor directive */
  while (ptr < head) {
    assert(*ptr != '\0');
    if (*ptr == '/' && *(ptr + 1) == '/')
      return false;     /* symbol is in a single-line comment */
    if (*ptr == '/' && *(ptr + 1) == '*') {
      ptr += 2;
      while (*ptr != '\0' && (*ptr != '*' || *(ptr + 1) != '/')) {
        if (ptr >= head)
          return false; /* symbol is in a block comment */
        ptr += 1;
      }
      ptr += 1; /* we stopped on the '*', move to the '/' which is the end of the comment */
    } else if (*ptr == '\'' || *ptr == '"') {
      char quote = *ptr++;
      while (*ptr != '\0' && *ptr != quote) {
        if (ptr >= head)
          return false; /* symbol is in a literal character or string */
        if (*ptr == '\\')
          ptr += 1; /* escape character, skip 2 characters */
        ptr += 1;
      }
    }
    ptr++;
  }
  /* when moving to the right, skip '[' and ']' so that pointing at 'vector[i]'
     shows the element 'i' of array 'vector' (but skip ']' only if '[' was seen
     too) */
  int nest = 0;
  const char *tail = item->text + (col - 1);
  while (isalpha(*tail) || isdigit(*tail) || *tail == '_' || *tail == '[' || (*tail == ']' && nest > 0)) {
    if (*tail == '[')
      nest++;
    else if (*tail == ']')
      nest--;
    tail++;
  }
  if (nest != 0)
    return false;
  unsigned len = tail - head;
  if (len >= symlen)
    return false;       /* full symbol name does not fit, no need to try to look it up */
  strncpy(symname, head, len);
  symname[len] = '\0';
  if (is_keyword(symname))
    return false;       /* reserved words are not symbols */
  return true;
}

/* returns the current file index and line that the cursor is on */
static void source_getcursorpos(int *fileindex, int *linenumber)
{
  assert(fileindex != NULL);
  *fileindex = source_cursorfile;
  assert(linenumber != NULL);
  *linenumber = source_cursorline;
}

static int is_ip_address(const char *address)
{
  int a, b, c, d;
  return sscanf(address, "%d.%d.%d.%d", &a, &b, &c, &d) == 4
         && a > 0 && a < 255 && b >= 0 && b < 255 && c >= 0 && c < 255 && d >= 0 && d < 255;
}


enum {
  STATE_INIT,
  STATE_GDB_TASK,
  STATE_SCAN_BMP,
  STATE_GDBVERSION,
  STATE_FILE,
  STATE_TARGET_EXT,
  STATE_PROBE_TYPE,
  STATE_PROBE_CMDS_1,   /* get probe commands before target scan */
  STATE_CONNECT_SRST,
  STATE_MON_TPWR,
  STATE_MON_SCAN,
  STATE_ASYNC_MODE,
  STATE_ATTACH,
  STATE_PROBE_CMDS_2,   /* get probe + target commands after attach */
  STATE_GET_SOURCES,
  STATE_MEMACCESS,      /* allow GDB to access memory beyond ELF file boundaries */
  STATE_MEMMAP,         /* LPC: make sure low memory is mapped to Flash */
  STATE_PARTID_1,       /* part id via command */
  STATE_PARTID_2,       /* part id via script */
  STATE_VERIFY,
  STATE_DOWNLOAD,
  STATE_CHECK_MAIN,
  STATE_START,
  STATE_EXEC_CMD,
  STATE_HARDRESET,
  /* ----- */
  STATE_STOPPED,
  STATE_RUNNING,
  STATE_LIST_BREAKPOINTS,
  STATE_LIST_LOCALS,
  STATE_LIST_WATCHES,
  STATE_LIST_REGISTERS,
  STATE_VIEWMEMORY,
  STATE_BREAK_TOGGLE,
  STATE_WATCH_TOGGLE,
  STATE_WATCH_FORMAT,
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
#define REFRESH_REGISTERS   0x0008
#define REFRESH_MEMORY      0x0010
#define IGNORE_DOUBLE_DONE  0x8000  /* input comes from a console, check for extra "done" result */

#define MSG_BMP_NOT_FOUND   0x0001

#define TERM_END(s, i)      ((s)[i] == ' ' || (s)[i] == '\0')
#define TERM_EQU(s, key, i) (strncmp((s), (key), (i)) == 0 && TERM_END((s), (i)))

enum {
  POPUP_NONE,
  POPUP_HELP,
  POPUP_INFO,
  /* --- */
  POPUP_COUNT
};


static bool svd_info(const char *params, STRINGLIST *textroot, bool bitfields, unsigned long value)
{
  if (!bitfields) {
    stringlist_append(textroot, "System View Description", 0);
    stringlist_append(textroot, "", 0);
  }

  bool retvalue = true;

  assert(params != NULL);
  if (*params == '\0') {
    unsigned long address;
    const char *name, *description;
    int idx = 0;
    nk_bool has_info = nk_false;
    while ((name = svd_peripheral(idx, &address, &description)) != NULL) {
      has_info = nk_true;
      if (idx == 0)
        stringlist_append(textroot, "Peripherals:", 0);
      char line[200];
      sprintf(line, "    [%lx] %s", address, name);
      if (description != NULL) {
        /* add first sentence of description */
        strcat(line, " -- ");
        const char *ptr;
        for (ptr = description; *ptr != '\0' && *ptr != '.'; ptr++)
          {}
        int count = (ptr - description);
        int len = strlen(line);
        if (len + count < sizearray(line)) {
          memcpy(line + len, description, count);
          line[len + count] = '\0';
        }
      }
      stringlist_append(textroot, line, 0);
      idx += 1;
    }
    if (!has_info) {
      stringlist_append(textroot, "No SVD file loaded, no information available", 0);
      retvalue = false;
    }
  } else {
    char *params_upcase = NULL;
    unsigned long address;
    const char *periph_name, *reg_name, *description;
    int result = svd_lookup(params, 0, &periph_name, &reg_name, &address, &description);
    if (result == 0) {
      /* on failure, also try with params converted to upper case */
#     if !defined __linux__
        params_upcase = strdup(params);
        if (params_upcase != NULL) {
          strupr(params_upcase);
          result = svd_lookup(params_upcase, 0, &periph_name, &reg_name, &address, &description);
        }
#     endif
    }
    if (result > 0) {
      char line[256];
      if (reg_name != NULL && result == 1) {
        /* register details */
        sprintf(line, "[%lx] %s%s->%s", address, svd_mcu_prefix(), periph_name, reg_name);
        if (bitfields)
          sprintf(line + strlen(line), " = %ld [0x%08lx]", (long)value, value);
        stringlist_append(textroot, line, 0);
        /* full description, reformatted in strings of 80 characters */
        if (description != NULL && !bitfields) {
          const char *head = description;
          while (*head != '\0') {
            int cutoff = strlen(head);
            if (cutoff > 80)
              cutoff = 80;
            int len = cutoff;
            while (len > 0 && head[len] > ' ')
              len -= 1;
            if (len == 0) {
              /* single "word" of 80 characters or longer */
              len = cutoff;
              while (head[len] > ' ')
                len += 1;
            }
            if (len < sizearray(line)) {
              strncpy(line, head, len);
              line[len] = '\0';
              stringlist_append(textroot, line, 0);
            } else {
              /* remainder text does not fit in line variable -> just dump the
                 remainder (this should not occur) */
              stringlist_append(textroot, head, 0);
              len = strlen(head);
            }
            head += len;
            while (*head != '\0' && *head <= ' ')
              head++;
          }
        }
        stringlist_append(textroot, "", 0);
        /* register fields */
        int idx = 0;
        const char *name;
        short low_bit, high_bit;
        while ((name = svd_bitfield(periph_name, reg_name, idx, &low_bit, &high_bit, &description)) != NULL) {
          strcpy(line, "    ");
          if (low_bit >= 0 && high_bit > low_bit)
            sprintf(line + strlen(line), "[%d:%d] ", high_bit, low_bit);
          else if (low_bit >= 0)
            sprintf(line + strlen(line), "[%d] ", low_bit);
          strlcat(line, name, sizearray(line));
          if (bitfields) {
            int numbits = high_bit - low_bit + 1;
            unsigned long mask = ~(~0 << numbits);
            unsigned long field = (value >> low_bit) & mask;
            sprintf(line + strlen(line), " = %lu [0x%lx] ", field, field);
          }
          if (description != NULL) {
            strlcat(line, " -- ", sizearray(line));
            strlcat(line, description, sizearray(line));
          }
          stringlist_append(textroot, line, 0);
          idx += 1;
        }
      } else if (reg_name != NULL && result > 1) {
        /* register name with implied peripheral, and there are multiple
           peripherals with this register name */
        stringlist_append(textroot, "Multiple matches:", 0);
        for (int idx = 0; idx < result; idx++) {
          if (idx > 0) {
            /* name for index 0 was already looked up, so only look up registers
               above 0 */
            svd_lookup((params_upcase != NULL) ? params_upcase : params, idx,
                       &periph_name, &reg_name, &address, &description);
          }
          sprintf(line, "    [%lx] %s%s->%s", address, svd_mcu_prefix(),
                  periph_name, reg_name);
          if (description != NULL) {
            /* add first sentence of description */
            strcat(line, " -- ");
            const char *ptr;
            for (ptr = description; *ptr != '\0' && *ptr != '.'; ptr++)
              {}
            int count = (ptr - description);
            int len = strlen(line);
            if (len + count < sizearray(line)) {
              memcpy(line + len, description, count);
              line[len + count] = '\0';
            }
          }
          stringlist_append(textroot, line, 0);
        }
      } else {
        /* peripheral details, list all registers */
        assert(result == 1 && reg_name == NULL);
        sprintf(line, "%s%s:", svd_mcu_prefix(), periph_name);
        stringlist_append(textroot, line, 0);
        if (description != NULL)
          stringlist_append(textroot, description, 0);
        stringlist_append(textroot, "", 0);
        /* list all registers in the peripheral */
        stringlist_append(textroot, "Registers:", 0);
        unsigned long offset;
        int range;
        const char *name;
        int idx = 0;
        while ((name = svd_register(periph_name, idx, &offset, &range, &description)) != NULL) {
          char formatted_name[50];
          if (strstr(name, "%s") != NULL && strlen(name) < sizearray(formatted_name) - 8) {
            /* replace index */
            strcpy(formatted_name, name);
            char *ptr = strchr(formatted_name, '%');
            assert(ptr != NULL);
            sprintf(ptr, "[%d]", range);
            name = formatted_name;
          }
          sprintf(line, "    [%lx] %s", address + offset, name);
          if (description != NULL) {
            /* add first sentence of description (so, the text up to the first period) */
            strcat(line, " -- ");
            const char *ptr;
            for (ptr = description; *ptr != '\0' && *ptr != '.'; ptr++)
              {}
            int count = (ptr - description);
            int len = strlen(line);
            if (len + count < sizearray(line)) {
              memcpy(line + len, description, count);
              line[len + count] = '\0';
            }
          }
          stringlist_append(textroot, line, 0);
          idx += 1;
        }
      }
    } else {
      /* check whether SVD is available at all */
      const char *name = svd_peripheral(0, NULL, NULL);
      if (name == NULL)
        stringlist_append(textroot, "No SVD file loaded, no information available", 0);
      else
        stringlist_append(textroot, "Specified register of peripheral is not found", 0);
      retvalue = false;
    }
    if (params_upcase != NULL)
      free((void*)params_upcase);
  }
  return retvalue;
}

static bool handle_help_cmd(char *command, STRINGLIST *textroot, int *active,
                            nk_bool *reformat)
{
  assert(command != NULL);
  assert(textroot != NULL);
  assert(active != NULL);
  assert(reformat != NULL);

  /* delete leading white-space */
  char *ptr = (char*)skipwhite(command);
  if (ptr != command)
    memmove(command, ptr, strlen(ptr) + 1);
  /* move a trailing "help" to the beginning (so "serial help" becomes "help serial"
     but make an exception for "monitor" */
  ptr = strrchr(command, ' ');
  if (ptr != NULL && strcmp(skipwhite(ptr), "help") == 0) {
    if (TERM_EQU(command, "mon", 3) || TERM_EQU(command, "monitor", 7)) {
      *active = POPUP_HELP;
      *reformat = nk_false;
    } else {
      *ptr = '\0';
      memmove(command + 5, command, strlen(command) + 1);
      memcpy(command, "help ", 5);
    }
  }

  if (TERM_EQU(command, "help", 4)) {
    const char *cmdptr = skipwhite(command + 4);
    *active = POPUP_HELP;
    *reformat = nk_true;    /* by default, reformat (overruled for "help mon") */
    if (*cmdptr == '\0') {
      stringlist_append(textroot, "BMDebug is a GDB front-end, specifcally for embedded debugging with the Black Magic Probe.", 0);
      stringlist_append(textroot, "Copyright 2019-2023 CompuPhase", 0);
      stringlist_append(textroot, "Licensed under the Apache License version 2.0", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "Front-end topics.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "assembly -- show assembly mixed with source code.", 0);
      stringlist_append(textroot, "find -- search text in the source view.", 0);
      stringlist_append(textroot, "keyboard -- list special keys.", 0);
      stringlist_append(textroot, "mouse -- mouse actions.", 0);
      stringlist_append(textroot, "semihosting -- options for the semihosting view.", 0);
      stringlist_append(textroot, "serial -- configure the serial monitor.", 0);
      stringlist_append(textroot, "svd -- show or list peripherals and registers.", 0);
      stringlist_append(textroot, "trace -- configure SWO tracing.", 0);
      stringlist_append(textroot, "", 0);
      /* drop to the default "return false", so that GDB's help follows */
    } else if (TERM_EQU(cmdptr, "assembly", 8) || TERM_EQU(command, "disassemble", 11) || TERM_EQU(command, "disas", 5)) {
      stringlist_append(textroot, "Show disassembled code interleaved with source code.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "assembly [on | off] -- set the assembly mode on or off; when no parameter is given, the command toggles the current status.", 0);
      stringlist_append(textroot, "disassemble [on | off] -- a synonym for the \"assembly\" command.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "Note that in assembly mode, the function keys F10 and F11 step by"
                                  " instruction, rather than by source line. You can still use the \"next\""
                                  " and \"step\" commands to step by source line.", 0);
    } else if (TERM_EQU(cmdptr, "find", 4)) {
      stringlist_append(textroot, "Find text in source code, or bytes in target memory.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "find [text] -- Find text in the current source file (case-insensitive). Without parameter, the find command repeats the previous search.", 0);
      stringlist_append(textroot, "find [/sn] start, end, values -- Search the address range from \"start\" to \"end\" for a sequence of values. The \"values\" parameter is a list of numbers separated by commas.", 0);
      return true;
    } else if (TERM_EQU(cmdptr, "kbd", 3) || TERM_EQU(cmdptr, "keys", 4) || TERM_EQU(cmdptr, "keyboard", 8)) {
      stringlist_append(textroot, "Keyboard navigation and commands.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "Up/Down arrow -- scroll the source view up and down (one line).", 0);
      stringlist_append(textroot, "PageUp/PageDown -- scroll the source view up and down (one screen).", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "F3 -- find next (see \"find\" command).", 0);
      stringlist_append(textroot, "F5 -- continue running, same as \"continue\".", 0);
      stringlist_append(textroot, "F7 -- run to cursor line (in the source view), same as \"until\".", 0);
      stringlist_append(textroot, "F9 -- set/reset breakpoint on the current line.", 0);
      stringlist_append(textroot, "F10 -- step to next line (step over functions), same as \"next\".", 0);
      stringlist_append(textroot, "F11 -- Step single line (step into functions), same as \"step\".", 0);
      stringlist_append(textroot, "TAB -- auto-complete command or parameter.", 0);
      stringlist_append(textroot, "Shift-F11 -- Step out of functions, same as \"finish\".", 0);
      stringlist_append(textroot, "Ctrl+F -- find text (\"find\" command).", 0);
      stringlist_append(textroot, "Ctrl+G -- go to file or line in source view, (\"list\" command).", 0);
      stringlist_append(textroot, "Ctrl+R -- scroll backward through the command history. You can also use"
                                  " Ctrl+ArrowUp and Ctrl+ArrowDown to scroll through the command history.", 0);
      stringlist_append(textroot, "Ctrl+F2 -- reset program, same as \"start\".", 0);
      stringlist_append(textroot, "Ctrl+F5 -- interrupt program (stop).", 0);
      return true;
    } else if (TERM_EQU(cmdptr, "mouse", 5)) {
      stringlist_append(textroot, "Mouse actions.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "A left-click in the left margin of the source view, toggles a breakpoint"
                               " on that line (or the nearest applicable line, if the line that was"
                               " clicked on has no code).", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "A right-click on a word or symbol in the source view, copies that word or"
                               " symbol name onto the command line. To add a watch on a variable, for"
                               " example, you can type \"disp\" and right click on the variable (and press Enter).", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "A right-click on a value in the Locals or Watches views enables you to"
                               " select the format in which the value is displayed: decimal, hexadecimal,"
                               " octal or binary. This only works for integer values.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "Hovering over a variable or symbol, shows information on the symbol (such"
                               " as the current value, in case of a variable).", 0);
    } else if (TERM_EQU(cmdptr, "reset", 5)) {
      stringlist_append(textroot, "Restart debugging.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "reset -- restart the target program; keep breakpoints and variable watches.", 0);
      stringlist_append(textroot, "reset hard -- restart both the debugger and the target program.", 0);
      stringlist_append(textroot, "reset load -- restart the debugger and reload the target program.", 0);
      return true;
    } else if (TERM_EQU(cmdptr, "semihosting", 11)) {
      stringlist_append(textroot, "Semihosting options.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "semimosting clear -- clear the semihosting monitor view (delete contents).", 0);
    } else if (TERM_EQU(cmdptr, "serial", 6)) {
      stringlist_append(textroot, "Configure the serial monitor.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "serial [port] [bitrate] -- open the port at the bitrate. If no port is specified,"
                               " the secondary UART of the Black Magic Probe is used.", 0);
      stringlist_append(textroot, "serial enable -- open the serial monitor with the previously confugured settings.", 0);
      stringlist_append(textroot, "serial disable -- close the virtual monitor.", 0);
      stringlist_append(textroot, "serial info -- show current status and configuration.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "serial [filename] -- configure CTF decoding using the given TSDL file.", 0);
      stringlist_append(textroot, "serial plain -- disable CTF decoding, display received data as text.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "serial clear -- clear the serial monitor view (delete contents).", 0);
      stringlist_append(textroot, "serial save [filename] -- save the contents in the serial monitor to a file.", 0);
    } else if (TERM_EQU(cmdptr, "svd", 3)) {
      stringlist_append(textroot, "Show information from the System View Description file.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "info svd -- list all peripherals.", 0);
      stringlist_append(textroot, "info svd [peripheral] -- list registers in the peripheral.", 0);
      stringlist_append(textroot, "info svd [register] -- look up register, display matching registers.", 0);
      stringlist_append(textroot, "info svd [peripheral->register] -- show register details.", 0);
    } else if (TERM_EQU(cmdptr, "trace", 5) || TERM_EQU(cmdptr, "tracepoint", 10) || TERM_EQU(cmdptr, "tracepoints", 11)) {
      stringlist_append(textroot, "Configure SWO tracing.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "trace [target-clock] [bitrate] -- configure Manchester tracing.", 0);
      stringlist_append(textroot, "trace passive -- activate Manchester tracing, without configuration.", 0);
      stringlist_append(textroot, "trace async [target-clock] [bitrate] -- configure asynchronous tracing.", 0);
      stringlist_append(textroot, "trace async passive [bitrate] -- activate asynchronous tracing, without configuration.", 0);
      stringlist_append(textroot, "trace bitrate [value] -- set only the bitrate, without changing other parameters.", 0);
      stringlist_append(textroot, "      The target-clock may be given as 12000000 or as 12MHZ.", 0);
      stringlist_append(textroot, "      The bitrate may be given as 115200 or as 115.2kbps.", 0);
      stringlist_append(textroot, "      The option \"passive\" can be abbreviated to \"pasv\".", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "trace enable -- enable SWO tracing with previously configured settings.", 0);
      stringlist_append(textroot, "trace disable -- disable SWO tracing.", 0);
      stringlist_append(textroot, "trace info -- show current status and configuration.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "trace [filename] -- configure CTF decoding using the given TSDL file.", 0);
      stringlist_append(textroot, "trace plain -- disable CTF decoding, trace plain input data.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "trace channel [index] enable -- enable a channel (0..31).", 0);
      stringlist_append(textroot, "trace channel [index] disable -- disable a channel (0..31).", 0);
      stringlist_append(textroot, "trace channel [index] [name] -- set the name of a channel.", 0);
      stringlist_append(textroot, "trace channel [index] #[colour] -- set the colour of a channel.", 0);
      stringlist_append(textroot, "      The option \"channel\" can be abbreviated to \"chan\" or \"ch\".", 0);
      stringlist_append(textroot, "      The parameter [index] may be a range, like 0-7 for the first eight channels.", 0);
      stringlist_append(textroot, "", 0);
      stringlist_append(textroot, "trace clear -- clear the trace view (delete contents).", 0);
      stringlist_append(textroot, "trace save [filename] -- save the contents in the trace view to a file.", 0);
      return true;
    } else if (TERM_EQU(cmdptr, "mon", 3)) {
      memcpy(command, "mon help", 8);       /* translate "help mon" -> "mon help" */
      *reformat = nk_false;
    } else if (TERM_EQU(cmdptr, "monitor", 7)) {
      memcpy(command, "monitor help", 12);  /* translate "help monitor" -> "monitor help" */
      *reformat = nk_false;
    }
  } else if (*active == POPUP_HELP && !(TERM_EQU(command, "mon", 3) || TERM_EQU(command, "monitor", 7))) {
    *active = POPUP_NONE;
  }
  return false;
}

static bool handle_info_cmd(char *command, STRINGLIST *textroot, int *active,
                            nk_bool *reformat, const SWOSETTINGS *swo, TASK *task)
{
  assert(command != NULL);
  assert(textroot != NULL);
  assert(active != NULL);
  assert(reformat != NULL);
  assert(swo != NULL);

  command = (char*)skipwhite(command);
  bool is_info = TERM_EQU(command, "info", 4);
  bool is_print = TERM_EQU(command, "print", 5) || TERM_EQU(command, "p", 1);
  if (!is_info && !is_print) {
    if (*active == POPUP_INFO)
      *active = POPUP_NONE;
    return false;
  }

  const char *cmdptr = command;
  if (is_info) {
    cmdptr = skipwhite(cmdptr + 4);
  } else {
    while (isalpha(*cmdptr))
      cmdptr++;
    cmdptr = skipwhite(cmdptr);
  }

  if (is_info) {
    /* always pop up info view when the "info" command is given (for the "print"
       command, this is handled later) */
    *active = POPUP_INFO;
    *reformat = nk_false; /* never reformat "info" */
  }

  if (is_info && *cmdptr == '\0') {
    stringlist_append(textroot, "Front-end topics.", 0);
    stringlist_append(textroot, "", 0);
    stringlist_append(textroot, "serial -- status of the serial monitor.", 0);
    stringlist_append(textroot, "svd -- list peripherals and registers.", 0);
    stringlist_append(textroot, "trace -- status SWO tracing.", 0);
    stringlist_append(textroot, "", 0);
    /* drop to the default "return false", so that GDB's info follows */
  } else if (is_info && TERM_EQU(cmdptr, "trace", 5)) {
    trace_info_mode(swo, 1, textroot);
    return true;
  } else if (is_info && TERM_EQU(cmdptr, "serial", 6)) {
    serial_info_mode(textroot);
    return true;
  } else if (is_info && TERM_EQU(cmdptr, "svd", 3)) {
    svd_info(skipwhite(cmdptr + 3), textroot, false, 0);
    return true;
  } else {
    /* check whether the parameter is a register name (defined in SVD) */
    char symbol[128];
    strlcpy(symbol, cmdptr, sizearray(symbol));
    const char *periph_name, *reg_name;
    int matches = svd_lookup(symbol, 0, &periph_name, &reg_name, NULL, NULL);
    /* only decode the register when it is fully specified (so not for multiple
       matches); also only pop up the "info" view for the info command, or when
       the register has bitfields */
    if (matches == 1 && (is_info || svd_bitfield(periph_name, reg_name, 0, NULL, NULL, NULL) != NULL)) {
      /* make cleaned-up name */
      strlcpy(symbol, periph_name, sizearray(symbol));
      strlcat(symbol, "->", sizearray(symbol));
      strlcat(symbol, reg_name, sizearray(symbol));
      /* look up its value */
      char alias[40] = "p ";
      svd_xlate_name(symbol, alias + 2, sizearray(alias) - 2);
      strlcat(alias, "\n", sizearray(alias));
      if (task_stdin(task, alias))
        gdbmi_sethandled(false); /* clear result on new input */
      unsigned long regvalue = 0;
      bool valid_result = false;
      bool done = false;
      while (!done) {
        char buffer[256];
        while (task_stdout(task, buffer, sizearray(buffer)) > 0) {
          char *start = buffer;
          while (start != NULL && *start != '\0') {
            int flags;
            start = (char*)skipwhite(start);
            const char *ptr = gdbmi_leader(start, &flags, &start);
            if (*ptr == '$' && isdigit(*(ptr + 1))) {
              /* get the value behind the '=' */
              ptr++;  /* skip '$' */
              while (isdigit(*ptr))
                ptr++;
              ptr = (char*)skipwhite(ptr);
              if (*ptr == '=') {
                regvalue = strtoul(ptr + 1, NULL, 0);
                valid_result = true;
              }
            } else if (strncmp(ptr, "done", 4) == 0 && (*(ptr + 4) == '\n' || *(ptr + 4) == '\r')) {
              done = true;
            }
          }
        }
      }
      /* now decode the register */
      if (valid_result) {
        svd_info(symbol, textroot, true, regvalue);
        if (is_print) {
          /* only go to popup mode for a print command, if the print command
             has the details */
          *active = POPUP_INFO;
          *reformat = nk_false;
        }
      }
      return valid_result;
    }
  }

  return false;
}

static bool handle_disasm_cmd(const char *command, bool *curstate)
{
  command = skipwhite(command);
  if (TERM_EQU(command, "disassemble", 11) || TERM_EQU(command, "disas", 5)
      || TERM_EQU(command, "assembly", 8))
  {
    const char *ptr = command;
    while (isalpha(*ptr))
      ptr++;
    ptr = skipwhite(ptr);
    if (*ptr == '\0')
      *curstate =!*curstate;
    else if (TERM_EQU(ptr, "on", 2))
      *curstate = true;
    else if (TERM_EQU(ptr, "off", 3))
      *curstate = false;
    else
      console_add("Invalid argument\n", STRFLG_ERROR);
    disasm_show_hide(source_cursorfile, *curstate);
    source_force_refresh = true;
    return true;
  }
  return false;
}

static bool handle_list_cmd(const char *command, const DWARF_SYMBOLLIST *symboltable,
                            const DWARF_PATHLIST *filetable)
{
  command = skipwhite(command);
  if (TERM_EQU(command, "list", 4)) {
    const char *p1 = skipwhite(command + 4);
    if (*p1 == '+' || *p1 == '\0') {
      int linecount = source_linecount(source_cursorfile);
      source_cursorline += source_vp_rows;      /* "list" & "list +" */
      if (source_cursorline > linecount)
        source_cursorline = linecount;
      return true;
    } else if (*p1 == '-') {
      source_cursorline -= source_vp_rows;      /* "list -" */
      if (source_cursorline < 1)
        source_cursorline = 1;
      return true;
    } else if (isdigit(*p1)) {
      int line = (int)strtol(p1, NULL, 10);     /* "list #" (where # is a line number) */
      if (line >= 1 && line <= source_linecount(source_cursorfile)) {
        source_cursorline = line;
        return true;
      }
    } else {
      const DWARF_SYMBOLLIST *sym;              /* "list filename", "list filename:#" or "list function" */
      unsigned line = 0, idx = UINT_MAX;
      sym = dwarf_sym_from_name(symboltable, p1, source_cursorfile, source_cursorline);
      if (sym != NULL) {
        const char *path = dwarf_path_from_fileindex(filetable, sym->fileindex);
        if (path != NULL)
          idx = source_getindex(path);
        line = sym->line;
      } else {
        char *p2 = strchr(p1, ':');
        if (p2 != NULL) {
          *p2++ = '\0';
          line = (int)strtol(p2, NULL, 10);
        } else {
          line = 1;
        }
        if (strchr(p1, '.') != NULL) {
          /* extension is given, try an exact match */
          idx = source_getindex(p1);
        } else {
          /* no extension, ignore extension on match */
          unsigned len = strlen(p1);
          for (idx = 0; ; idx++) {
            const char *basename = source_getname(idx);
            if (basename == NULL)
              break;
            if (strncmp(basename, p1, len) == 0 && basename[len] == '.')
              break;
          }
        }
      }
      if (source_isvalid(idx) && line >= 1) {
        source_cursorfile = idx;
        source_cursorline = line;
        return true;
      }
    }
  }
  return false;
}

static bool handle_display_cmd(const char *command, int *param, char *symbol, size_t symlength)
{
  const char *ptr;

  assert(command != NULL);
  assert(param != NULL);
  assert(symbol != NULL && symlength > 0);
  command = skipwhite(command);
  if (TERM_EQU(command, "disp", 4) || TERM_EQU(command, "display", 7)) {
    param[0] = STATEPARAM_WATCH_SET;
    param[1] = FORMAT_NATURAL;  /* preset, may be overruled, see below */
    ptr = strchr(command, ' ');
    assert(ptr != NULL);
    ptr = skipwhite(ptr);
    if (*ptr == '/') {
      /* get format option */
      ptr += 1; /* skip '/' */
      switch (*ptr) {
      case 'd':
        param[1] = FORMAT_DECIMAL;
        break;
      case 'x':
        param[1] = FORMAT_HEX;
        break;
      case 'o':
        param[1] = FORMAT_OCTAL;
        break;
      case 't':
        param[1] = FORMAT_BINARY;
        break;
      }
      while (*ptr > ' ')
        ptr += 1; /* skip any characters following the format */
    }
    strlcpy(symbol, skipwhite(ptr), symlength);
    return true;
  } else if (TERM_EQU(command, "undisp", 6) || TERM_EQU(command, "undisplay", 9)) {
    param[0] = STATEPARAM_WATCH_DEL;
    ptr = strchr(command, ' ');
    assert(ptr != NULL);
    if (isdigit(*ptr)) {
      param[1] = (int)strtol(ptr, NULL, 10);
      return true;
    } else {
      /* find a watch with the name */
      WATCH *watch;
      for (watch = watch_root.next; watch != NULL; watch = watch->next) {
        if (strcmp(watch->expr, ptr) == 0) {
          param[1] = watch->seqnr;
          return true;
        }
      }
    }
  }
  return false;
}

#define RESET_FILE    1 /* equivalent to the GDB "file" command */
#define HARD_RESET    2 /* stop & restart GDB */
#define LOAD_CUR_ELF  3 /* equivalent to the GDB "load" command, plus running to the entry point */
#define LOAD_FILE_ELF 4 /* equivalent to the GDB "load" command with a specific ELF file, plus running to the entry point */
#define RESET_LOAD    5 /* equivalent to the GDB "file" command, but forcing ELF file download */

static int handle_file_load_reset(const char *command, char *filename, size_t namelength)
{
  assert(command != NULL);
  assert(filename != NULL && namelength > 0);
  command = skipwhite(command);
  if (TERM_EQU(command, "file", 4)) {
    const char *ptr = strchr(command, ' ');
    if (ptr != NULL) {
      strlcpy(filename, skipwhite(ptr), namelength);
      translate_path(filename, true);
    }
    return RESET_FILE;
  } else if (TERM_EQU(command, "reset", 5)) {
    /* interpret "reset" as "file ..." for the current ELF file, but "reset hard"
       does a full reset (GDB is stopped and restarted) */
    const char *ptr = strchr(command, ' ');
    if (ptr != NULL) {
      ptr = skipwhite(ptr);
      if (TERM_EQU(ptr, "hard", 4))
        return HARD_RESET;
      if (TERM_EQU(ptr, "load", 4))
        return RESET_LOAD;
    }
    return RESET_FILE;
  } else if (TERM_EQU(command, "load", 4)) {
    /* load has an optional filename */
    const char *ptr = strchr(command, ' ');
    if (ptr != NULL) {
      strlcpy(filename, skipwhite(ptr), namelength);
      translate_path(filename, true);
      return LOAD_FILE_ELF;
    }
    /* check if source files have changed, if so -> reload sources when
       re-flashing the target */
    if (sources_ischanged())
      return RESET_LOAD;
    return LOAD_CUR_ELF;
  }
  return 0;
}

static int find_substring(const char *text, const char *pattern)
{
  int idx, txtlen, patlen;

  assert(text != NULL && pattern != NULL);
  txtlen = strlen(text);
  patlen = strlen(pattern);
  idx = 0;
  while (idx < txtlen) {
    while (idx < txtlen && toupper(text[idx]) != toupper(pattern[0]))
      idx++;
    if (idx + patlen > txtlen)
      break;      /* not found on this line */
    if (memicmp((const unsigned char*)text + idx, (const unsigned char*)pattern, patlen) == 0)
      break;      /* found on this line */
    idx++;
  }
  return (idx + patlen <= txtlen);
}

static bool handle_find_cmd(const char *command)
{
  assert(command != NULL);
  command = skipwhite(command);
  if (TERM_EQU(command, "find", 4)) {
    /* check whether this is a memory search command */
    const char *ptr = skipwhite(command + 4);
    const char *p2 = ptr;
    if (*p2 == '/') {
      while (*p2 > ' ')
        p2++;
      p2 = skipwhite(p2);
      long s, e, v;
      if (sscanf(p2, "%li, %li, %li", &s, &e, &v) == 3)
        return false; /* this is the syntax for GDB find command */
    }
    /* copy pattern the search for */
    static char pattern[100] = "";
    if (*ptr != '\0') {
      size_t len = strlen(ptr);
      if (*ptr == '"' && *(ptr + len - 1) == '"') {
        strlcpy(pattern, ptr + 1, sizearray(pattern));
        len -= 2;
        if (len < sizearray(pattern))
          pattern[len] = '\0';
      } else {
        strlcpy(pattern, ptr, sizearray(pattern));
      }
    }
    if (strlen(pattern) == 0)
      return true; /* invalid pattern, but command syntax is ok */
    /* find pattern, starting from source_cursorline */
    int linenr = source_cursorline;
    SOURCELINE *item = sourceline_get(source_cursorfile, linenr);
    int found_on_curline;
    if (item == NULL || source_cursorline <= 0) {
      found_on_curline = 0;
      linenr = 1;
      item = sourceline_get(source_cursorfile, linenr);
    } else {
      found_on_curline = find_substring(item->text, pattern);
      item = item->next;
      linenr++;
    }
    while (linenr != source_cursorline) {
      assert(item != NULL && item->text != NULL);
      if (find_substring(item->text, pattern)) {
        source_cursorline = linenr;
        return true;     /* found, stop search */
      }
      item = item->next;
      linenr++;
      if (item == NULL) {
        linenr = 1;
        item = sourceline_get(source_cursorfile, linenr);
        if (source_cursorline == 0)
          source_cursorline = 1;
      }
    } /* while (linenr != source_cursorline) */
    /* pattern not found */
    if (found_on_curline)
      console_add("No further matches found\n", STRFLG_ERROR);
    else
      console_add("Text not found\n", STRFLG_ERROR);
    return true;
  }
  return false;
}

/** handle_x_command()
 *  \param command    [in] command string.
 *  \param memdump    [out] the various settings, see below.
 *
 *  \note syntax expr /[count][fmt][size]
 *        * expr      numeric address, or expression that resolves to an address
 *        * count     count of elements
 *        * fmt       'x', 'd', 'u', 'o', 't', 'f', 'c', 'a', 'i', 's'
 *        * size      1, 2, 4, 8
 */
static bool handle_x_cmd(const char *command, MEMDUMP *memdump)
{
  char *ptr;

  assert(command != NULL);
  command = skipwhite(command);
  if (!TERM_EQU(command, "x", 1))
    return false;

  assert(memdump != NULL);
  ptr = (char*)skipwhite(command + 1);
  if (*ptr == '/') {
    ptr += 1;
    while (*ptr > ' ') {
      if (isdigit(*ptr)) {
        memdump->count = (unsigned short)strtol(ptr, &ptr, 10);
      } else if (*ptr == 'x' || *ptr == 'd' || *ptr == 'u' || *ptr == 'o'
                 || *ptr == 't' || *ptr == 'f' || *ptr == 'c' || *ptr == 'a'
                 || *ptr == 'i' || *ptr == 's') {
        memdump->fmt = *ptr;
        ptr += 1;
      } else if (*ptr == 'b') {
        memdump->size = 1;
        ptr += 1;
      } else if (*ptr == 'h') {
        memdump->size = 2;
        ptr += 1;
      } else if (*ptr == 'w') {
        memdump->size = 4;
        ptr += 1;
      } else if (*ptr == 'g') {
        memdump->size = 8;
        ptr += 1;
      }
    }
  }

  /* get address expression */
  ptr = (char*)skipwhite(ptr);
  if (*ptr != '\0') {
    if (memdump->expr != NULL)
      free(memdump->expr);
    memdump->expr = strdup(ptr);
  }

  if (!memdump_validate(memdump)) {
    console_add("Missing address\n", STRFLG_ERROR);
    return true; /* still say "return true", because the command was recognized */
  }

  /* free memory of the previous dump */
  memdump_cleanup(memdump);

  return true;
}

static bool is_monitor_cmd(const char *command)
{
  assert(command != NULL);
  return TERM_EQU(command, "mon", 3) || TERM_EQU(command, "monitor", 7);
}

static void trace_info_channel(int ch_start, int ch_end, STRINGLIST *textroot)
{
  char msg[100];
  int chan;

  for (chan = ch_start; chan <= ch_end; chan++) {
    snprintf(msg, sizearray(msg), "Channel %d: ", chan);
    if (chan < 0 || chan >= NUM_CHANNELS) {
      strlcat(msg, "invalid", sizearray(msg));
    } else {
      if (channel_getenabled(chan))
        strlcat(msg, "enabled ", sizearray(msg));
      else
        strlcat(msg, "disabled", sizearray(msg));
      const char *ptr = channel_getname(chan, NULL, 0);
      if (ptr != NULL && strlen(ptr) > 0) {
        strlcat(msg, " \"", sizearray(msg));
        strlcat(msg, ptr, sizearray(msg));
        strlcat(msg, "\"", sizearray(msg));
      }
      struct nk_color clr = channel_getcolor(chan);
      struct nk_color defclr = SWO_TRACE_DEFAULT_COLOR;
      if (clr.r != defclr.r || clr.g != defclr.g || clr.b != defclr.b) {
        char str[30];
        sprintf(str, " #%02x%02x%02x", clr.r, clr.g, clr.b);
        strlcat(msg, str, sizearray(msg));
      }
    }
    if (textroot != NULL) {
      stringlist_append(textroot, msg, 0);
    } else {
      strlcat(msg, "\n", sizearray(msg));
      console_add(msg, STRFLG_STATUS);
    }
  }
}

static void trace_info_mode(const SWOSETTINGS *swo, int showchannels, STRINGLIST *textroot)
{
  char msg[200];

  strlcpy(msg, "SWO Trace configuration", sizearray(msg));
  if (textroot != NULL) {
    stringlist_append(textroot, msg, 0);
    stringlist_append(textroot, "", 0);
  } else {
    strlcat(msg, ": ", sizearray(msg));
  }

  if (textroot != NULL)
    strlcpy(msg, "Mode: ", sizearray(msg));
  switch (swo->mode) {
  case SWOMODE_NONE:
    strlcat(msg, "disabled", sizearray(msg));
    break;
  case SWOMODE_MANCHESTER:
    if (swo->clock == 0l)
      strlcat(msg, "Manchester encoding, passive", sizearray(msg));
    else
      sprintf(msg + strlen(msg), "Manchester encoding, clock = %u, bitrate = %u", swo->clock, swo->bitrate);
    break;
  case SWOMODE_ASYNC:
    if (swo->clock == 0l)
      sprintf(msg + strlen(msg), "Asynchronous encoding, passive, bitrate = %u", swo->bitrate);
    else
      sprintf(msg + strlen(msg), "Asynchronous encoding, clock = %u, bitrate = %u", swo->clock, swo->bitrate);
    break;
  }
  if (textroot != NULL)
    stringlist_append(textroot, msg, 0);

  if (textroot == NULL)
    strlcat(msg, ", data width = ", sizearray(msg));
  else
    strlcpy(msg, "Data width: ", sizearray(msg));
  if (swo->datasize == 0)
    strlcat(msg, "auto", sizearray(msg));
  else
    sprintf(msg + strlen(msg), "%u-bit", swo->datasize * 8);
  if (textroot != NULL) {
    stringlist_append(textroot, msg, 0);
  } else {
    strlcat(msg, "\n", sizearray(msg));
    console_add(msg, STRFLG_STATUS);
  }

  assert(swo->metadata != NULL);
  if (textroot != NULL) {
    strlcpy(msg, "CTF / TSDL: ", sizearray(msg));
    if (strlen(swo->metadata) > 0 && strcmp(swo->metadata, "-") != 0) {
      const char *basename = lastdirsep(swo->metadata);
      strlcat(msg, (basename != NULL) ? basename + 1 : swo->metadata, sizearray(msg));
    } else {
      strlcat(msg, "-", sizearray(msg));
    }
    stringlist_append(textroot, msg, 0);
  } else {
    if (strlen(swo->metadata) > 0 && strcmp(swo->metadata, "-") != 0) {
      const char *basename = lastdirsep(swo->metadata);
      snprintf(msg, sizearray(msg), "CTF / TSDL = %s\n", (basename != NULL) ? basename + 1 : swo->metadata);
      console_add(msg, STRFLG_STATUS);
    }
  }

  if (textroot != NULL) {
    stringlist_append(textroot, "", 0);
    stringlist_append(textroot, "Enabled channels", 0);
  }
  if (showchannels && swo->mode != SWOMODE_NONE && swo->enabled) {
    int count, chan;
    for (count = chan = 0; chan < NUM_CHANNELS; chan++)
      if (channel_getenabled(chan))
        count++;
    if (textroot != NULL) {
      if (count == 0) {
        stringlist_append(textroot, "(all channels disabled)", 0);
      } else {
        for (chan = 0; chan < NUM_CHANNELS; chan++)
          if (channel_getenabled(chan))
            trace_info_channel(chan, chan, textroot);
      }
    } else {
      if (count == NUM_CHANNELS) {
        console_add("All channels enabled\n", STRFLG_STATUS);
      } else if (count == 0) {
        console_add("All channels disabled\n", STRFLG_STATUS);
      } else {
        int comma = 0;
        strlcpy(msg, "Enabled channels:", sizearray(msg));
        for (chan = 0; chan < NUM_CHANNELS; chan++) {
          if (channel_getenabled(chan)) {
            if (comma)
              strlcat(msg, ",", sizearray(msg));
            sprintf(msg + strlen(msg), " %d", chan);
            comma = 1;
          }
        }
        strlcat(msg, "\n", sizearray(msg));
        console_add(msg, STRFLG_STATUS);
      }
    }
  }
}

/** handle_trace_cmd()
 *  \param command    [in] command string.
 *  \param swo        [out] the various settings, see below.
 *
 *  \note * mode       SWOMODE_NONE, SWOMODE_ASYNC or SWOMODE_MANCHESTER.
 *        * clock      target clock rate; this is set to zero for passive mode.
 *        * bitrate    transmission speed.
 *        * datasize   payload data size in bytes (not bits).
 *        * metadata   definition file for CTF.
 *  \return 0=unchanged, 1=protocol settings changed, 2=channels changed,
 *          3="info"
 */
static int handle_trace_cmd(const char *command, SWOSETTINGS *swo)
{
  char *ptr, *cmdcopy;
  unsigned newmode = SWOMODE_NONE;
  int tsdl_set = 0;

  assert(command != NULL);
  command = skipwhite(command);
  if (!TERM_EQU(command, "trace", 5))
    return 0;

  /* make a copy of the command string, to be able to clear parts of it */
  cmdcopy = alloca(strlen(command) + 1);
  strcpy(cmdcopy, command);
  ptr = (char*)skipwhite(cmdcopy + 5);

  if (*ptr == '\0' || TERM_EQU(ptr, "info", 4))
    return 3; /* if only "trace" is typed, interpret it as "trace info" */

  char *opt_clear;
  if ((opt_clear = strstr(cmdcopy, "clear")) != NULL && TERM_END(opt_clear, 5)) {
    tracestring_clear();
    memset(opt_clear, ' ', 5);  /* erase the "clear" parameter from the string */
  }

  assert(swo != NULL);

  if (TERM_EQU(ptr, "channel", 7) || TERM_EQU(ptr, "chan", 4) || TERM_EQU(ptr, "ch", 2)) {
    int ch_start, ch_end;
    char *opts, *p2;
    ptr = ((p2 = strchr(ptr, ' ')) != NULL) ? (char*)skipwhite(p2) : strchr(ptr, '\0');
    if (*ptr == '\0') {
      ch_start = 0;
      ch_end = NUM_CHANNELS - 1;
    } else {
      ch_start =(int)strtol(ptr,(char**)&ptr, 10);
      ptr = (char*)skipwhite(ptr);
      if (*ptr == '-') {
        ch_end = (int)strtol(ptr + 1, (char**)&ptr, 10);
        ptr = (char*)skipwhite(ptr);
      } else {
        ch_end = ch_start;
      }
    }
    opts = alloca(strlen(ptr)+ 1);
    strcpy(opts, ptr);
    for (ptr = strtok(opts, " "); ptr != NULL; ptr = strtok(NULL, " ")) {
      int chan;
      if (stricmp(ptr, "enable") == 0) {
        for (chan = ch_start; chan <= ch_end; chan++)
          channel_setenabled(chan, 1);
      } else if (stricmp(ptr, "disable") == 0) {
        for (chan = ch_start; chan <= ch_end; chan++)
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
        for (chan = ch_start; chan <= ch_end; chan++)
          channel_setcolor(chan, nk_rgb(r, g, b));
      } else {
        for (chan = ch_start; chan <= ch_end; chan++)
          channel_setname(chan, ptr);
      }
    }
    trace_info_channel(ch_start, ch_end, NULL);
    return 2; /* only channel set changed */
  }

  /* key terms like "8-bit", "16-bit", "32-bit" or "auto" may be combined on
     a mode set command */
  if ((ptr = strstr(cmdcopy, "bit")) != 0 && TERM_END(ptr, 3) && (*(ptr - 1) == ' ' || *(ptr - 1) == '-' || isdigit(*(ptr - 1)))) {
    char *endptr = ptr + 3;
    ptr--;
    if (!isdigit(*ptr))
      ptr--;
    while (isdigit(*ptr))
      ptr--;
    assert(ptr > cmdcopy);  /* the command string started with "trace", so ptr cannnot overrun the start */
    ptr++;                  /* reset overrun, point to first digit */
    if (isdigit(*ptr)) {
      unsigned v = (unsigned)strtol(ptr, NULL, 10);
      if (v == 8 || v == 16 || v == 32)
        swo->datasize = v / 8;
    }
    memset(ptr, ' ', (unsigned)(endptr - ptr)); /* erase the datasize setting from the string */
  } else if ((ptr = strstr(cmdcopy, "auto")) != 0 && TERM_END(ptr, 4) && *(ptr - 1) == ' ') {
    swo->datasize = 0;
    memset(ptr, ' ', 4);    /* erase the datasize setting from the string */
  }

  /* when "passive" is set, the clock is set to 0 (so that the target is not
     initialized) */
  if ((ptr = strstr(cmdcopy, "passive")) != 0 && TERM_END(ptr, 7)) {
    swo->clock = 0;
    memset(ptr, ' ', 7);    /* erase the setting from the string */
  } else if ((ptr = strstr(cmdcopy, "pasv")) != 0 && TERM_END(ptr, 4)) {
    swo->clock = 0;
    memset(ptr, ' ', 4);    /* erase the setting from the string */
  }

  /* explicit bitrate */
  if ((ptr = strstr(cmdcopy, "bitrate")) != 0 && TERM_END(ptr, 7)) {
    char *start = ptr;
    if ((ptr = strchr(ptr, ' ')) != NULL) {
      double v = strtod(ptr, (char**)&ptr);
      ptr = (char*)skipwhite(ptr);
      if ((strnicmp(ptr, "mhz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "m", 1) == 0 && TERM_END(ptr, 1))) {
        v *= 1000000;
        ptr = strchr(ptr, ' ');
        ptr = (ptr != NULL) ? (char*)skipwhite(ptr) : strchr(cmdcopy, '\0');
      } else if ((strnicmp(ptr, "kbps", 4) == 0 && TERM_END(ptr, 4)) || (strnicmp(ptr, "khz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "k", 1) == 0 && TERM_END(ptr, 1))) {
        v *= 1000;
        ptr = strchr(ptr, ' ');
        ptr = (ptr != NULL) ? (char*)skipwhite(ptr) : strchr(cmdcopy, '\0');
      }
      swo->bitrate = (unsigned)(v + 0.5);
      if (swo->mode != SWOMODE_ASYNC)
        newmode = SWOMODE_MANCHESTER; /* if bitrate is set, mode must be async or manchester */
    }
    memset(start, ' ', (ptr - start));/* erase the setting from the string */
  }

  /* optionally clear a TSDL file */
  ptr = (char*)skipwhite(cmdcopy + 6);  /* reset to start */
  if (TERM_EQU(ptr, "plain", 5)) {
    strcpy(swo->metadata, "-"); /* set it explicitly to "none" */
    memset(ptr, ' ', 5);    /* erase the setting from the string */
    swo->force_plain = 1;
  }
  /* check whether any of the parameters is a TSDL file */
  ptr = (char*)skipwhite(cmdcopy + 6);  /* reset to start */
  while (*ptr != '\0' && !tsdl_set) {
    char *endptr = ptr;
    /* check the word for '.', '/' or '\' */
    while (*endptr > ' ') {
      if (*endptr == '.' || *endptr == '/' || *endptr == '\\')
        tsdl_set = 1;
      endptr++;
    }
    if (tsdl_set) {
      unsigned len = (unsigned)(endptr - ptr);
      strncpy(swo->metadata, ptr, len);
      swo->metadata[len] = '\0';
      memset(ptr, ' ', len);    /* erase the filename from the string */
      swo->force_plain = 0;
    }
    ptr =(char*)skipwhite(endptr);
  }

  /* mode (explicit or implied) */
  ptr = (char*)skipwhite(cmdcopy + 6);  /* reset to start */
  if (TERM_EQU(ptr, "disable", 7)) {
    swo->enabled = 0;
    swo->mode = SWOMODE_NONE;
    return 1; /* special mode, disable all channels when turning tracing off */
  }
  if (TERM_EQU(ptr, "enable", 6)) {
    swo->enabled = 1;
    newmode = (swo->mode == SWOMODE_NONE) ? SWOMODE_MANCHESTER : swo->mode;
    ptr = (char*)skipwhite(ptr + 6);
  }
  if (TERM_EQU(ptr, "async", 5)) {
    newmode = SWOMODE_ASYNC;
    ptr = (char*)skipwhite(ptr + 5);
  }
  /* clock */
  if (isdigit(*ptr)) {
    double v = strtod(ptr, (char**)&ptr);
    ptr = (char*)skipwhite(ptr);
    if ((strnicmp(ptr, "mhz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "m", 1) == 0 && TERM_END(ptr, 1))) {
      v *= 1000000;
      ptr = strchr(ptr, ' ');
      ptr = (ptr != NULL) ? (char*)skipwhite(ptr) : strchr(cmdcopy, '\0');
    }
    swo->clock = (unsigned)(v + 0.5);
    if (swo->mode != SWOMODE_ASYNC)
      newmode = SWOMODE_MANCHESTER; /* if clock is set, mode must be async or manchester */
    swo->enabled = 1;
  }
  /* (implied) bitrate */
  if (isdigit(*ptr)) {
    double v = strtod(ptr, (char**)&ptr);
    ptr = (char*)skipwhite(ptr);
    if ((strnicmp(ptr, "mhz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "m", 1) == 0 && TERM_END(ptr, 1))) {
      v *= 1000000;
      ptr = strchr(ptr, ' ');
      ptr = (ptr != NULL) ? (char*)skipwhite(ptr) : strchr(cmdcopy, '\0');
    } else if ((strnicmp(ptr, "kbps", 4) == 0 && TERM_END(ptr, 4)) || (strnicmp(ptr, "khz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "k", 1) == 0 && TERM_END(ptr, 1))) {
      v *= 1000;
      ptr = strchr(ptr, ' ');
      ptr = (ptr != NULL) ? (char*)skipwhite(ptr) : strchr(cmdcopy, '\0');
    }
    swo->bitrate = (unsigned)(v + 0.5);
    if (swo->mode != SWOMODE_ASYNC)
      newmode = SWOMODE_MANCHESTER; /* if bitrate is set, mode must be async or manchester */
    swo->enabled = 1;
  }
  if (newmode != SWOMODE_NONE && swo->clock > 0 && swo->bitrate > swo->clock) {
    /* if bitrate > clock swap the two (this can never happen, so it is likely
       an error) */
    unsigned t = swo->bitrate;
    swo->bitrate = swo->clock;
    swo->clock = t;
  }
  if (newmode != SWOMODE_NONE)
    swo->mode = newmode;

  trace_info_mode(swo, 0, NULL);
  return 1; /* assume entire protocol changed */
}

/** handle_semihosting_cmd()
 *  \param command    [in] command string.
 *
 *  \return false=not a "semihosting" command, true=ok (command handled).
 */
static bool handle_semihosting_cmd(const char *command)
{
  assert(command != NULL);
  command = skipwhite(command);
  if (!TERM_EQU(command, "semihosting", 11))
    return false;
  const char *ptr = (char*)skipwhite(command + 11);
  if (TERM_EQU(ptr, "clear", 5)) {
    stringlist_clear(&semihosting_root);
  }

  return true;
}

/** handle_directory_cmd()
 *  \param command      [in] command string.
 *  \param sourcepath   [in/out] search path for source files.
 *  \param maxlen       maximum size of the "sourcepath" buffer.
 *
 *  \return false=not a "directory" command, true=ok (command handled).
 */
static bool handle_directory_cmd(const char *command, char *sourcepath, size_t maxlen)
{
  const char *ptr = NULL;
  assert(command != NULL);
  command = skipwhite(command);
  if (TERM_EQU(command, "directory", 9))
    ptr = (char*)skipwhite(command + 9);
  else if (TERM_EQU(command, "dir", 3))
    ptr = (char*)skipwhite(command + 3);
  else
    return false;

  assert(sourcepath != NULL);
  if (*ptr == '\0') {
    char msg[_MAX_PATH + 5];
    if (*sourcepath == '\0') {
      strlcpy(msg, "(none)", sizearray(msg));
    } else {
      strlcpy(msg, sourcepath, sizearray(msg));
      strlcat(msg, "\n", sizearray(msg));
    }
    console_add(msg, STRFLG_STATUS);
  } else {
    strlcpy(sourcepath, ptr, maxlen);
    sources_reload(sourcepath, false);
  }
  return true;
}

static void serial_info_mode(STRINGLIST *textroot)
{
  char msg[_MAX_PATH + 20];

  strlcpy(msg, "Serial monitor configuration", sizearray(msg));
  if (textroot != NULL) {
    stringlist_append(textroot, msg, 0);
    stringlist_append(textroot, "", 0);
    msg[0] = '\0';
  } else {
    strlcat(msg, ": ", sizearray(msg));
  }

  if (sermon_isopen()) {
    strlcat(msg, sermon_getport(1), sizearray(msg));
    sprintf(msg + strlen(msg), " at %d bps", sermon_getbaud());
  } else {
    strlcat(msg, "disabled", sizearray(msg));
  }
  if (textroot != NULL) {
    stringlist_append(textroot, msg, 0);
  } else {
    strlcat(msg, "\n", sizearray(msg));
    console_add(msg, STRFLG_STATUS);
  }

  if (sermon_isopen()) {
    const char *tdsl = sermon_getmetadata();
    if (strlen(tdsl) > 0 && strcmp(tdsl, "-") != 0) {
      sprintf(msg, "CTF mode: %s", tdsl);
      if (textroot != NULL) {
        stringlist_append(textroot, msg, 0);
      } else {
        strlcat(msg, "\n", sizearray(msg));
        console_add(msg, STRFLG_STATUS);
      }
    }
  }
}

/** handle_serial_cmd()
 *  \param command    [in] command string.
 *  \param port       [out] the name of the serial port to open/monitor.
 *  \param baud       [out] the baud rate to use.
 *  \param tsdlfile   [out] the metadata file for CTF mode.
 *  \param tsdlmaxlen [in] the maximum length of the TSDL metadata filename.
 *
 *  \return 0=not "serial" command, 1=open or re-open, 2=close, 3="info",
 *          4=do-nothing (already handled)
 */
static int handle_serial_cmd(const char *command, char *port, int *baud,
                             char *tsdlfile, size_t tsdlmaxlen)
{
  const char *ptr;

  assert(command != NULL);
  command = skipwhite(command);
  if (!TERM_EQU(command, "serial", 6))
    return 0;
  ptr = (char*)skipwhite(command + 6);
  if (*ptr == '\0' || TERM_EQU(ptr, "info", 4))
    return 3; /* if only "serial" is typed in, handle it as "serial info" */

  if (TERM_EQU(ptr, "disable", 7))
    return 2;
  if (TERM_EQU(ptr, "enable", 6))
    return 1;
  if (TERM_EQU(ptr, "clear", 5)) {
    sermon_clear();
    return 4;
  }
  if (TERM_EQU(ptr, "save", 4)) {
    ptr = skipwhite(ptr + 4);
    if (*ptr != '\0') {
      int count = sermon_save(ptr);
      if (count >= 0) {
        char message[100];
        sprintf(message,"%d lines saved\n", count);
        console_add(message, STRFLG_STATUS);
      } else {
        console_add("Failed to save to file\n", STRFLG_ERROR);
      }
    } else {
      console_add("Missing filename\n", STRFLG_ERROR);
    }
    return 4;
  }
  if (TERM_EQU(ptr, "plain", 5) && tsdlfile != NULL && tsdlmaxlen > 0)
    tsdlfile[0] = '\0'; /* reset to plain mode (disable TSDL) */

  assert(port != NULL);
  if (isalpha(*ptr) || *ptr == DIRSEP_CHAR) {
    int len, isport;
    for (len = 0; ptr[len] > ' '; len++)
      {}
    /* check that this is a port name, not a filename (TSDL metadata) */
#   if defined _WIN32
      isport = (len > 4 && strncmp(ptr, "\\\\.\\", 4) == 0)
               || (len > 3 && strnicmp(ptr, "com", 3) == 0 && isdigit(*ptr + 3));
#   else
      isport = (len > 5 && strncmp(ptr, "/dev/", 5) == 0);
#   endif
    if (isport) {
      int i;
      for (i = 0; ptr[i] > ' '; i++)
        port[i] = ptr[i];
      port[i] = '\0';
      ptr = skipwhite(ptr + i);
    }
  }

  assert(baud != NULL);
  if (isdigit(*ptr)) {
    double v = strtod(ptr, (char**)&ptr);
    if (toupper(*ptr) == 'K') {
      v *= 1000.0;
      while (*ptr > ' ')
        ptr++;
    }
    *baud = (int)(v + 0.5);
    ptr = skipwhite(ptr);
  }

  if (tsdlfile != NULL && tsdlmaxlen > 0) {
    char stop = ' ';
    if (*ptr == '"') {
      ptr++;
      stop = '"';
    }
    unsigned count = 0;
    while (*ptr != stop && *ptr != '\0') {
      if (count < tsdlmaxlen - 1) /* -1 to make sure space is left for the terminating zero */
        tsdlfile[count++] = *ptr;
      ptr++;
    }
    if (stop == '"' && *ptr == '"')
      ptr++;
    tsdlfile[count] = '\0';
    /* also test whether the file exists */
    if (strcmp(tsdlfile, "plain") == 0 || access(tsdlfile, 0) != 0)
      tsdlfile[0] = '\0';
  }

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
    printf("BMDebug - GDB front-end for the Black Magic Probe.\n\n");
  printf("Usage: bmdebug [options] elf-file\n\n"
         "Options:\n"
         "-f=value  Font size to use (value must be 8 or larger).\n"
         "-g=path   Path to the GDB executable to use.\n"
         "-t=value  Target to attach to, for systems with multiple targets\n"
         "-h        This help.\n"
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

  printf("BMDebug version %s.\n", SVNREV_STR);
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
#   endif
    printf("Fortify: [%d] %s\n", type, str);
  }
#endif

static void config_read_tabstate(const char *key, enum nk_collapse_states *state, SIZERBAR *sizer,
                                 enum nk_collapse_states default_state, float default_height,
                                 const char *configfile)
{
  assert(state != NULL);
  *state = default_state;
  if (sizer != NULL)
    sizer->size = default_height;

  assert(key != NULL);
  char valstr[64];
  int opened;
  float size;
  ini_gets("Views", key, "", valstr, sizearray(valstr), configfile);
  int result = sscanf(valstr, "%d %f", &opened, &size);
  if (result >= 1)
    *state = opened;
  if (sizer != NULL && result >= 2 && size > ROW_HEIGHT)
    sizer->size = size;
}

static void config_write_tabstate(const char *key, enum nk_collapse_states state, const SIZERBAR *sizer,
                                  const char *configfile)
{
  assert(key != NULL);
  char valstr[64];
  if (sizer != NULL)
    sprintf(valstr, "%d %f", state, sizer->size);
  else
    sprintf(valstr, "%d", state);
  ini_puts("Views", key, valstr, configfile);
}

static bool exist_monitor_cmd(const char *name, const char *list)
{
  if (list == NULL)
    return false;

  assert(name != NULL);
  size_t name_len = strlen(name);
  const char *head = list;
  while (*head != '\0') {
    /* the assumption is that the list of commands is "well-formed": no leading
       or trailing spaces and the tokens separated by a single space */
    assert(*head > ' ');
    const char *tail = strchr(head, ' ');
    if (tail == NULL)
      tail = head + strlen(head);
    size_t token_len = tail - head;
    if (token_len == name_len && strncmp(name, head, name_len) == 0)
      return true;
    head = tail;
    while (*head != '\0' && *head <= ' ')
      head++;
  }
  return false;
}

typedef struct tagAPPSTATE {
  int curstate;                 /**< current (or new) state */
  int prevstate;                /**< previous state (to detect state changes) */
  int nextstate;                /**< on occasion, follow-up state depends on logic */
  bool debugmode;               /**< for extra debug logging */
  unsigned long gdbversion;     /**< version of GDB, xx.yy.zzzz */
  int stateparam[3];            /**< parameters for state change */
  int refreshflags;             /**< flags for refresh of various views */
  int probe;                    /**< selected debug probe (index) */
  int netprobe;                 /**< index for the IP address (pseudo-probe) */
  const char **probelist;       /**< list of detected probes */
  int probe_type;               /**< BMP or ctxLink (needed to select manchester/async mode) */
  char port_gdb[64];            /**< COM port for GDB */
  char IPaddr[64];              /**< IP address for network probe */
  char mcu_family[64];          /**< detected MCU family (on attach), also the "driver" name of BMP */
  char mcu_architecture[32];    /**< detected ARM architecture (on attach) */
  unsigned long mcu_partid;     /**< specific Part ID or Chip ID code (0 if unknown) */
  const char *monitor_cmds;     /**< list of "monitor" commands (target & probe dependent) */
  char GDBpath[_MAX_PATH];      /**< path to GDB executable */
  TASK gdb_task;                /**< GDB process */
  char *cmdline;                /**< command & response buffer (for GDB task) */
  char port_sermon[64];         /**< COM port name for serial monitor */
  int sermon_baud;              /**< serial monitor baud rate */
  int trace_status;             /**< status of TRACESWO */
  unsigned char trace_endpoint; /**< USB endpoint for TRACESWO */
  nk_bool tpwr;                 /**< option: tpwr (target power) */
  nk_bool connect_srst;         /**< option: keep in reset during connect */
  nk_bool autodownload;         /**< option: download ELF file to target on changes */
  nk_bool force_download;       /**< temporary option: download ELF file */
  nk_bool allmsg;               /**< option: show all GDB output (instead of filtered) */
  int target_count;             /**< number of targets found by swdp_scan */
  int target_select;            /**< target number to attach to (in case there are multiple targets) */
  bool atprompt;                /**< whether GDB has displayed the prompt (and waits for input) */
  bool monitor_cmd_active;      /**< to silence output of scripts */
  bool monitor_cmd_finish;      /**< automatically set the command as handled when the monitor command completes */
  bool waitidle;                /**< whether to yield while waiting for GUI input */
  bool target_errmsg_set;       /**< whether error message for failure to connect was already given */
  bool is_attached;             /**< BMP is attached */
  bool cont_is_run;             /**< "cont" command must be interpreted as "run" command */
  bool warn_source_tstamps;     /**< whether warning must be given when source files are more recent than target */
  unsigned watchseq;            /**< sequence number for watches */
  int console_activate;         /**< whether the edit line should get the focus (and whether to move the cursor to the end) */
  bool console_isactive;        /**< whether the console is currently active */
  char console_edit[256];       /**< edit line for console input */
  const STRINGLIST *console_mark;/**< marker to help parsing GDB output */
  STRINGLIST consoleedit_root;  /**< edit history */
  const STRINGLIST *consoleedit_next; /**< start point in history to search backward from */
  unsigned long ctrl_c_tstamp;  /**< timestamp of Ctrl+C being pressed, to force a break */
  char ELFfile[_MAX_PATH];      /**< ELF file being debugged */
  char ParamFile[_MAX_PATH];    /**< debug parameters for the ELF file */
  char sourcepath[_MAX_PATH];   /**< additional path where the sources may be found */
  char SVDfile[_MAX_PATH];      /**< target MCU definitions */
  char EntryPoint[64];          /**< name of the entry-point function (e.g. "main") */
  SWOSETTINGS swo;              /**< TRACESWO configuration */
  ARMSTATE armstate;            /**< state of the disassembler */
  unsigned long scriptparams[4];/**< parameters for running configuration scripts (for TRACESWO) */
  const char **sourcefiles;     /**< array of all source file names (base names) */
  unsigned sourcefiles_count;   /**< size of the array with all source file names */
  int sourcefiles_index;        /**< index into the sourcefiles array, or -1 if undetermined */
  bool disassemble_mode;        /**< whether source code is mixed with disassembly */
  bool dwarf_loaded;            /**< whether DWARF info is loaded */
  int prev_clicked_line;        /**< line in source view that was previously clicked on (to detect multiple clicks on the same line) */
  char statesymbol[128];        /**< name of the symbol hovered over (in source view) */
  char ttipvalue[256];          /**< text for variable value in tooltip (when hovering above symbol) */
  unsigned long tooltip_tstamp; /**< time-stamp of when the tooltip popped up */
  char watch_edit[128];         /**< edit string for watches panel */
  MEMDUMP memdump;              /**< information for memory watch/view */
  unsigned semihosting_lines;   /**< number of lines in the semihosting view (to detect new incoming lines) */
  unsigned sermon_lines;        /**< number of lines in the serial monitor (to detect new incoming lines) */
  unsigned swo_lines;           /**< number of lines in the traceswo monitor (to detect new incoming lines) */
  int popup_active;             /**< whether a popup is active (and which one) */
  nk_bool reformat_help;        /**< whether help text (of GDB) needs to be cleaned-up */
  char help_edit[128];          /**< edit field for help and info popup windows */
  SIZERBAR sizerbar_breakpoints;/**< info for resizable breakpoints view */
  SIZERBAR sizerbar_locals;     /**< info for resizable local variables view */
  SIZERBAR sizerbar_watches;    /**< info for resizable "variable watches" view */
  SIZERBAR sizerbar_registers;  /**< info for resizable local variables view */
  SIZERBAR sizerbar_memory;     /**< info for resizable memory dump view */
  SIZERBAR sizerbar_semihosting;/**< info for resizable semihosting output view */
  SIZERBAR sizerbar_serialmon;  /**< info for resizable serial monitor */
  SIZERBAR sizerbar_swo;        /**< info for resizable TRACESWO monitor */
} APPSTATE;

enum {
  TAB_CONFIGURATION,
  TAB_BREAKPOINTS,
  TAB_LOCALS,
  TAB_WATCHES,
  TAB_REGISTERS,
  TAB_MEMORY,
  TAB_SEMIHOSTING,
  TAB_SERMON,
  TAB_SWO,
  /* --- */
  TAB_COUNT
};

#define SPACING       8 /* general spacing between widgets */
#define SEPARATOR_HOR 4
#define SEPARATOR_VER 4

#define CMD_BUFSIZE   2048

#define RESETSTATE(app, state)  ((app)->prevstate = (app)->nextstate = -1, (app)->waitidle = false, (app)->curstate = (state), log_state(app))
#define MOVESTATE(app, state)   ((app)->waitidle = false, (app)->curstate = (state), log_state(app))
#define STATESWITCH(app)        ((app)->curstate != (app)->prevstate)
#define MARKSTATE(app)          ((app)->prevstate = (app)->curstate)
#define ISSTATE(app, state)     ((app)->curstate == (state))


static int log_state(const APPSTATE *state)
{
  if (state->debugmode) {
    printf("State: %d (moved from %d)\n", state->curstate, state->prevstate);
    fflush(stdout);
  }
  return state->curstate;
}

static void log_console_strings(const APPSTATE *state)
{
  if (state->debugmode) {
    static int skip = 0;
    STRINGLIST *item = consolestring_root.next;
    int count = 0;
    while (item != NULL && count < skip) {
      item = item->next;
      count++;
    }
    if (item != NULL) {
      printf("List:");
      while (item != NULL) {
        printf("\t[%d] %04x %s\n", skip, item->flags, item->text);
        item = item->next;
        skip++;
      }
    }
    fflush(stdout);
  }
}

static void follow_address(const APPSTATE *state, int direction)
{
  assert(state != NULL);
  assert(direction == -1 || direction == 1);
  if (state->disassemble_mode && source_isvalid(source_cursorfile)) {
    /* get address of current line (the line that the cursor is on) */
    uint32_t addr = line_phys2addr(source_cursorfile, source_cursorline);
    for (int retries = 0; retries < 2; retries++) {
      if (direction < 0)
        addr -= 2;  /* size of a thumb instruction is 2 bytes, so decrementing by 2 is sufficient */
      else
        addr += 2;
      /* look up the new address in the line table, basically because we need
         to check for file switches */
      const DWARF_LINEENTRY *entry = dwarf_line_from_address(&dwarf_linetable, addr);
      if (entry == NULL)
        break;  /* previous/next address is invalid, do nothing */
      /* check whether source file must change (previous or next address may be
         in a different file, as as what happens with inline functions declared
         in a header file) */
      int fileidx = source_cursorfile;  /* preset */
      const char *path = dwarf_path_from_fileindex(&dwarf_filetable, entry->fileindex);
      if (path != NULL)
        fileidx = source_getindex(path);
      int linenr = line_addr2phys(fileidx, addr);
      if (linenr != source_cursorline || source_cursorfile != fileidx) {
        source_cursorfile = fileidx;
        source_cursorline = linenr;
        break;  /* position updated */
      }
      /* the next address drops onto the same instruction, advance the address
         some more and retry (Thumb2 instructions may be 16-bit or 32-bit) */
    }
  }
}

static bool save_targetoptions(const char *filename, const APPSTATE *state)
{
  if (filename == NULL || strlen(filename) == 0)
    return false;

  assert(state != NULL);
  ini_puts("Target", "entrypoint", state->EntryPoint, filename);
  ini_puts("Target", "cmsis-svd", state->SVDfile, filename);
  ini_puts("Target", "source-path", state->sourcepath, filename);

  ini_putl("Settings", "tpwr", state->tpwr, filename);
  ini_putl("Settings", "connect_srst", state->connect_srst, filename);

  ini_putl("Flash", "auto-download", state->autodownload, filename);

  ini_putl("SWO trace", "mode", state->swo.mode, filename);
  ini_putl("SWO trace", "bitrate", state->swo.bitrate, filename);
  ini_putl("SWO trace", "clock", state->swo.clock, filename);
  ini_putl("SWO trace", "datasize", state->swo.datasize * 8, filename);
  ini_putl("SWO trace", "enabled", state->swo.enabled, filename);
  ini_puts("SWO trace", "ctf", state->swo.metadata, filename);
  for (unsigned idx = 0; idx < NUM_CHANNELS; idx++) {
    char key[32], value[128];
    struct nk_color color = channel_getcolor(idx);
    sprintf(key, "chan%d", idx);
    sprintf(value, "%d #%06x %s", channel_getenabled(idx),
            ((int)color.r << 16) | ((int)color.g << 8) | color.b,
            channel_getname(idx, NULL, 0));
    ini_puts("SWO trace", key, value, filename);
  }

  ini_putl("Serial monitor", "mode", sermon_isopen(), filename);
  ini_puts("Serial monitor", "port", sermon_getport(0), filename);
  ini_putl("Serial monitor", "baud", sermon_getbaud(), filename);

  return access(filename, 0) == 0;
}

static bool load_targetoptions(const char *filename, APPSTATE *state)
{
  if (filename == NULL || strlen(filename) == 0 || access(filename, 0) != 0)
    return false;

  assert(state != NULL);
  ini_gets("Target", "entrypoint", "main", state->EntryPoint, sizearray(state->EntryPoint), filename);
  ini_gets("Target", "cmsis-svd", "", state->SVDfile, sizearray(state->SVDfile), filename);
  ini_gets("Target", "source-path", "", state->sourcepath, sizearray(state->sourcepath), filename);

  state->tpwr =(int)ini_getl("Settings", "tpwr", 0, filename);
  state->connect_srst = (int)ini_getl("Settings", "connect_srst", 0, filename);

  state->autodownload = (int)ini_getl("Flash", "auto-download", 1, filename);

  state->swo.mode = (unsigned)ini_getl("SWO trace", "mode", SWOMODE_NONE, filename);
  state->swo.bitrate = (unsigned)ini_getl("SWO trace", "bitrate", 100000, filename);
  state->swo.clock = (unsigned)ini_getl("SWO trace", "clock", 48000000, filename);
  state->swo.datasize = (unsigned)ini_getl("SWO trace", "datasize", 8, filename) / 8;
  state->swo.enabled = (unsigned)ini_getl("SWO trace", "enabled", 0, filename);
  state->swo.force_plain = 0;
  state->swo.init_status = 0;
  ini_gets("SWO trace", "ctf", "", state->swo.metadata, sizearray(state->swo.metadata), filename);
  for (unsigned idx = 0; idx < NUM_CHANNELS; idx++) {
    char key[41], value[128];
    /* preset: port 0 is enabled by default, others disabled by default */
    channel_set(idx, (idx == 0), NULL, SWO_TRACE_DEFAULT_COLOR);
    sprintf(key, "chan%d", idx);
    unsigned clr;
    int enabled;
    ini_gets("SWO trace", key, "", value, sizearray(value), filename);
    int result = sscanf(value, "%d #%x %40s", &enabled, &clr, key);
    if (result >= 2)
      channel_set(idx, enabled, (result >= 3) ? key : NULL, nk_rgb(clr >> 16,(clr >> 8) & 0xff, clr & 0xff));
  }

  int mode = ini_getl("Serial monitor", "mode", 0, filename);
  if (mode) {
    char portname[64];
    int baud;
    ini_gets("Serial monitor", "port", "", portname, sizearray(portname), filename);
    baud = ini_getl("Serial monitor", "baud", 0, filename);
    sermon_open(portname, baud);
    sermon_setmetadata(state->swo.metadata);
  }

  return true;
}

static void help_popup(struct nk_context *ctx, APPSTATE *state, float canvas_width, float canvas_height)
{
  assert(ctx != NULL);
  assert(state != NULL);

  float w = opt_fontsize * 40;
  if (w > canvas_width - 20)  /* clip "ideal" help window size of canvas size */
    w = canvas_width - 20;
  float h = canvas_height * 0.75f;
  struct nk_rect rc = nk_rect((canvas_width - w) / 2, (canvas_height - h) / 2, w, h);
  nk_style_push_color(ctx, &ctx->style.window.popup_border_color, COLOUR_FG_YELLOW);
  nk_style_push_float(ctx, &ctx->style.window.popup_border, 2);
  if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Help", NK_WINDOW_NO_SCROLLBAR, rc)) {
    static const float bottomrow_ratio[] = {0.15f, 0.68f, 0.17f};
    nk_layout_row_dynamic(ctx, h - 1.75f*ROW_HEIGHT, 1);
    int rows = textview_widget(ctx, "help", helptext_root.next, opt_fontsize);
    nk_layout_row(ctx, NK_DYNAMIC, ROW_HEIGHT, 3, bottomrow_ratio);
    nk_label(ctx, (state->popup_active == POPUP_INFO) ? "More info" : "More help", NK_TEXT_LEFT);
    nk_edit_focus(ctx, 0);
    int result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                                state->help_edit, sizearray(state->help_edit),
                                                nk_filter_ascii);
    if ((result & NK_EDIT_COMMITED) != 0 && strlen(state->help_edit) > 0) {
      stringlist_clear(&helptext_root);
      if (strncmp(state->help_edit, "help", 4) != 0 && strncmp(state->help_edit, "info", 4) != 0) {
        /* insert "help" or "info" before the command */
        memmove(state->help_edit + 5, state->help_edit, strlen(state->help_edit) + 1);
        memmove(state->help_edit, (state->popup_active == POPUP_INFO) ? "info " : "help ", 5);
      }
      if (!handle_help_cmd(state->help_edit, &helptext_root, &state->popup_active, &state->reformat_help)
          && !handle_info_cmd(state->help_edit, &helptext_root, &state->popup_active, &state->reformat_help, &state->swo, &state->gdb_task))
      {
        strlcat(state->help_edit, "\n", sizearray(state->help_edit));
        if (task_stdin(&state->gdb_task, state->help_edit))
          gdbmi_sethandled(false); /* clear result on new input */
      }
      state->help_edit[0] = '\0';
    }
    if (nk_button_label(ctx, "Close") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
      state->popup_active = POPUP_NONE;
      state->atprompt = true;
      state->help_edit[0] = '\0';
      stringlist_clear(&helptext_root);
      nk_popup_close(ctx);
    }

    /* handle cursor/scrolling keys */
    float delta = 0.0;
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_UP))
      delta = -opt_fontsize;
    else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN))
      delta = opt_fontsize;
    else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_UP))
      delta = - (h - 2.5f*ROW_HEIGHT);
    else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_DOWN))
      delta = (h - 2.5f*ROW_HEIGHT);
    else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_TOP))
      delta = (float)INT_MIN;
    else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_BOTTOM))
      delta = (float)INT_MAX;
    if (delta < -0.1 || delta > 0.1) {
      nk_uint xoffs, yoffs;
      nk_group_get_scroll(ctx, "help", &xoffs, &yoffs);
      if (delta < 0 && yoffs < -delta) {
        yoffs = 0;
      } else {
        yoffs += (int)delta;
        int maxscroll = (int)((rows + 1) * (opt_fontsize + 4) - (h - 2*ROW_HEIGHT - 2*SPACING));
        if (maxscroll < 0)
          yoffs = 0;
        else if (yoffs > maxscroll)
          yoffs = maxscroll;
      }
      nk_group_set_scroll(ctx, "help", xoffs, yoffs);
    }

    /* other keyboard functions */
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_TAB))
      console_autocomplete(state->help_edit, sizearray(state->help_edit), &dwarf_symboltable);

    nk_popup_end(ctx);
  } else {
    state->popup_active = POPUP_NONE;
    state->atprompt = true;
  }
  nk_style_pop_float(ctx);
  nk_style_pop_color(ctx);
}

static void button_bar(struct nk_context *ctx, APPSTATE *state, float panel_width)
{
  assert(ctx != NULL);
  assert(state != NULL);

  nk_layout_row_push(ctx, BUTTON_WIDTH);
  if (button_tooltip(ctx, "reset", NK_KEY_CTRL_F2, (state->curstate != STATE_RUNNING),
                     "Reload and restart the program (Ctrl+F2)")) {
    if (strlen(state->ELFfile) > 0 && access(state->ELFfile, 0) == 0)
      save_targetoptions(state->ParamFile, state);
    RESETSTATE(state, STATE_FILE);
  }
  nk_layout_row_push(ctx, BUTTON_WIDTH);
  if (state->curstate == STATE_RUNNING) {
    if (button_tooltip(ctx, "stop", NK_KEY_CTRL_F5, nk_true, "Interrupt the program (Ctrl+F5)")) {
      RESETSTATE(state, STATE_EXEC_CMD);
      state->stateparam[0] = STATEPARAM_EXEC_STOP;
    } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_COPY)) { /* Ctrl+C = break (plus "copy to clipboard") */
      unsigned long tstamp = timestamp();
      if (tstamp - state->ctrl_c_tstamp < 3000) {
        /* when Ctrl+C is pressed twice within a 3-second period (whilst the firmware
           did not stop on the first Ctrl+C within these 3 seconds), go for a hard-reset */
        RESETSTATE(state, STATE_HARDRESET);
        state->ctrl_c_tstamp = 0;
      } else {
        RESETSTATE(state, STATE_EXEC_CMD);
        state->stateparam[0] = STATEPARAM_EXEC_STOP;
        state->ctrl_c_tstamp = tstamp;
      }
    }
  } else {
    if (button_tooltip(ctx, "cont", NK_KEY_F5, (state->curstate != STATE_RUNNING), "Continue running (F5)"))
    {
      RESETSTATE(state, STATE_EXEC_CMD);
      state->stateparam[0] = STATEPARAM_EXEC_CONTINUE;
    }
  }
  nk_layout_row_push(ctx, BUTTON_WIDTH);
  if (button_tooltip(ctx, "next", NK_KEY_F10, (state->curstate != STATE_RUNNING), "Step over (F10)"))
  {
    RESETSTATE(state, STATE_EXEC_CMD);
    state->stateparam[0] = STATEPARAM_EXEC_NEXT;
  }
  nk_layout_row_push(ctx, BUTTON_WIDTH);
  if (button_tooltip(ctx, "step", NK_KEY_F11, (state->curstate != STATE_RUNNING), "Step into (F11)"))
  {
    RESETSTATE(state, STATE_EXEC_CMD);
    state->stateparam[0] = STATEPARAM_EXEC_STEP;
  }
  nk_layout_row_push(ctx, BUTTON_WIDTH);
  if (button_tooltip(ctx, "finish", NK_KEY_SHIFT_F11, (state->curstate != STATE_RUNNING), "Step out of function (Shift+F11)"))
  {
    RESETSTATE(state, STATE_EXEC_CMD);
    state->stateparam[0] = STATEPARAM_EXEC_FINISH;
  }
  nk_layout_row_push(ctx, BUTTON_WIDTH);
  if (button_tooltip(ctx, "until", NK_KEY_F7, (state->curstate != STATE_RUNNING), "Run until cursor (F7)"))
  {
    RESETSTATE(state, STATE_EXEC_CMD);
    state->stateparam[0] = STATEPARAM_EXEC_UNTIL;
    state->stateparam[1] = line_phys2source(source_cursorfile, source_cursorline);
  }
  float combo_width = panel_width - 6 * (BUTTON_WIDTH + 5);
  nk_layout_row_push(ctx, combo_width);
  if (state->sourcefiles_count > 0) {
    if ((state->sourcefiles_index < 0 || (unsigned)state->sourcefiles_index > state->sourcefiles_count)) {
      assert(state->sourcefiles != NULL);
      state->sourcefiles_index = 0;
      SOURCEFILE *src = source_fromindex(source_cursorfile);
      if (src != NULL) {
        for (unsigned idx = 0; idx < state->sourcefiles_count; idx++) {
          assert(state->sourcefiles[idx] != NULL);
          if (strcmp(src->basename, state->sourcefiles[idx]) == 0) {
            state->sourcefiles_index = idx;
            break;
          }
        }
      }
    }
    assert(state->sourcefiles_index >= 0 && (unsigned)state->sourcefiles_index < state->sourcefiles_count);
    int curfile = nk_combo(ctx, state->sourcefiles, state->sourcefiles_count,
                           state->sourcefiles_index,
                           (int)COMBOROW_CY, nk_vec2(combo_width, 10*ROW_HEIGHT));
    if (curfile != state->sourcefiles_index) {
      state->sourcefiles_index = curfile;
      curfile = source_getindex(state->sourcefiles[curfile]);
      if (source_cursorfile != curfile) {
        source_cursorfile = curfile;
        source_cursorline = 1;  /* reset scroll */
      }
    }
  }
}

static void toggle_breakpoint(APPSTATE *state, int source_idx, int linenr)
{
  assert(state != NULL);

  /* click in the margin (or F9): set/clear/enable/disable breakpoint
     - if there is no breakpoint on this line -> add a breakpoint
     - if there is an enabled breakpoint on this line -> disable it
     - if there is a disabled breakpoint on this line -> check
       whether the current line is the same as the one previously
       clicked on; if yes -> delete; if no: -> enable */

  BREAKPOINT *bp = breakpoint_lookup(source_idx, linenr);
  if (bp == NULL) {
    /* no breakpoint yet -> add (but check first whether that can
       be done) */
    if (source_isvalid(source_idx)) {
      RESETSTATE(state, STATE_BREAK_TOGGLE);
      state->stateparam[0] = STATEPARAM_BP_ADD;
      state->stateparam[1] = source_idx;
      state->stateparam[2] = linenr;
    }
  } else if (bp->enabled) {
    /* enabled breakpoint -> disable */
    RESETSTATE(state, STATE_BREAK_TOGGLE);
    state->stateparam[0] = STATEPARAM_BP_DISABLE;
    state->stateparam[1] = bp->number;
  } else if (state->prev_clicked_line != linenr) {
    /* disabled breakpoint & not a double click -> enable */
    RESETSTATE(state, STATE_BREAK_TOGGLE);
    state->stateparam[0] = STATEPARAM_BP_ENABLE;
    state->stateparam[1] = bp->number;
  } else {
    /* disabled breakpoint & double click -> delete */
    RESETSTATE(state, STATE_BREAK_TOGGLE);
    state->stateparam[0] = STATEPARAM_BP_DELETE;
    state->stateparam[1] = bp->number;
  }

}

static void sourcecode_view(struct nk_context *ctx, APPSTATE *state)
{
  assert(ctx != NULL);
  assert(state != NULL);

  if (source_isvalid(source_cursorfile)) {
    int linecount = source_linecount(source_cursorfile);
    if (source_cursorline > linecount)
      source_cursorline = linecount;
  }

  /* if disassembly is requested, check whether to do this first */
  if (state->disassemble_mode && source_isvalid(source_cursorfile)) {
    SOURCELINE *item;
    for (item = sourceline_get(source_cursorfile, 1); item != NULL && item->linenumber != 0; item = item->next)
      {}  /* lines in assembly have address set, but no linenumber */
    if (item == NULL) {
      /* no disassembly present yet (disassemble each source file only once,
         until they get reloaded) */
      bool ok = sourcefile_disassemble(state->ELFfile, source_fromindex(source_cursorfile), &state->armstate);
      if (!ok) {
        /* on failure, switch disassembly off (otherwise, this routine will be
           re-entered each update) */
        state->disassemble_mode = false;
      }
    }
  }

  struct nk_rect bounds = nk_widget_bounds(ctx);
  source_widget(ctx, "source", opt_fontsize, state->curstate == STATE_RUNNING, state->disassemble_mode);
  if (state->curstate == STATE_STOPPED && nk_input_is_mouse_hovering_rect(&ctx->input, bounds)) {
    /* handle input for the source view only when not running */
    int row, col;
    source_mouse2char(ctx, "source", opt_fontsize, bounds, &row, &col);
    if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, bounds)) {
      if (col == 0) {
        toggle_breakpoint(state, source_cursorfile, line_phys2source(source_cursorfile, row));
      } else {
        /* set the cursor line */
        if (row > 0 && row <= source_linecount(source_cursorfile)) {
          source_cursorline = row;
          source_autoscroll = false;
        }
      }
      state->prev_clicked_line = row;
    } else {
      if (row != state->prev_clicked_line)
        state->prev_clicked_line = -1; /* mouse leaves the line clicked on, erase "repeat click" */
      char sym[64];
      if (source_getsymbol(sym, sizearray(sym), row, col)) {
        /* we are hovering over a symbol; if the symbol hovered over is
           different from the previous one -> clear the tooltip value */
        if (strcmp(sym, state->statesymbol) != 0) {
          state->ttipvalue[0] = '\0';
          state->tooltip_tstamp = 0;
          strlcpy(state->statesymbol, sym, sizearray(state->statesymbol));
          /* if the new symbol is valid -> start timeout for hovering tooltip */
          if (state->statesymbol[0] != '\0')
            state->tooltip_tstamp = timestamp();
        } else if (state->tooltip_tstamp != 0) {
          unsigned long tstamp = timestamp();
          if (tstamp - state->tooltip_tstamp >= TOOLTIP_DELAY) {
            RESETSTATE(state, STATE_HOVER_SYMBOL);
            state->tooltip_tstamp = 0; /* reset, to this case is only dropped into once */
          }
        } else if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_RIGHT, bounds)) {
          /* right click, if it is on a symbol -> append that symbol onto the
             text on the command line */
          state->tooltip_tstamp = 0;
          if (sym[0] != '\0') {
            int len = strlen(state->console_edit);
            if (len > 0 && state->console_edit[len - 1] != ' ')
              strlcat(state->console_edit, " ", sizearray(state->console_edit));
            strlcat(state->console_edit, sym, sizearray(state->console_edit));
            state->console_activate = 2;
          }
        }
        /* if the tooltip value is valid, show it */
        if (state->ttipvalue[0] != '\0')
          nk_tooltip(ctx, state->ttipvalue);
      } else {
        state->ttipvalue[0] = '\0';
      }
    }
  }
}

static void console_view(struct nk_context *ctx, APPSTATE *state,
                         enum nk_collapse_states tab_states[TAB_COUNT],
                         float panel_height)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_states != NULL);
  if (nk_group_begin(ctx, "console", NK_WINDOW_NO_SCROLLBAR|NK_WINDOW_BORDER)) {
    nk_layout_row_dynamic(ctx, panel_height - ROW_HEIGHT - SPACING, 1);
    console_widget(ctx, "console-out", opt_fontsize);
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);

    if ((state->curstate < STATE_START && !state->atprompt) || state->curstate == STATE_RUNNING) {
      /* while initializing (or running), say "please wait" and disable entering a command */
      if (state->curstate < STATE_START)
        strlcpy(state->console_edit, "Initializing. Please wait...", sizearray(state->console_edit));
      else if (state->curstate == STATE_RUNNING)
        strlcpy(state->console_edit, "Running... (Press Ctrl+C to interrupt)", sizearray(state->console_edit));
      nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_READ_ONLY|NK_EDIT_NO_CURSOR,
                                     state->console_edit, sizearray(state->console_edit), nk_filter_ascii);
      state->console_edit[0] = '\0';
    } else {
      /* true edit line */
      if (state->console_activate) {
        nk_edit_focus(ctx, (state->console_activate == 2) ? NK_EDIT_GOTO_END_ON_ACTIVATE : 0);
        state->console_activate = 1;
      }
      int result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                                  state->console_edit, sizearray(state->console_edit),
                                                  nk_filter_ascii);
      state->console_isactive = ((result & NK_EDIT_ACTIVE) != 0);
      if (result & NK_EDIT_COMMITED) {
        striptrailing(state->console_edit);
        /* some commands are handled internally (fully or partially) */
        if (handle_help_cmd(state->console_edit, &helptext_root, &state->popup_active, &state->reformat_help)) {
          /* nothing else to do */
        } else if (handle_display_cmd(state->console_edit, state->stateparam, state->statesymbol, sizearray(state->statesymbol))) {
          RESETSTATE(state, STATE_WATCH_TOGGLE);
          tab_states[TAB_WATCHES] = NK_MAXIMIZED; /* make sure the watch view to open */
        } else if ((result = handle_file_load_reset(state->console_edit, state->ELFfile, sizearray(state->ELFfile))) != 0) {
          state->force_download = nk_false;
          if (result == HARD_RESET) {
            RESETSTATE(state, STATE_HARDRESET);
          } else if (result == LOAD_CUR_ELF) {
            state->force_download = nk_true;
            RESETSTATE(state, STATE_MEMACCESS);
          } else {
            if (result == LOAD_FILE_ELF || result == RESET_LOAD)
              state->force_download = nk_true;
            RESETSTATE(state, STATE_FILE);
          }
          /* save target-specific settings in the parameter file before
             switching to the new file (new options are loaded in the
             STATE_FILE case) */
          if (strlen(state->ELFfile) > 0 && access(state->ELFfile, 0) == 0)
            save_targetoptions(state->ParamFile, state);
        } else if ((result = handle_serial_cmd(state->console_edit,
                                               state->port_sermon, &state->sermon_baud,
                                               state->swo.metadata, sizearray(state->swo.metadata))) != 0) {
          if (result == 1) {
            if (sermon_isopen())
              sermon_close();
            sermon_open(state->port_sermon, state->sermon_baud);
            if (sermon_isopen()) {
              sermon_setmetadata(state->swo.metadata);
              if (strlen(state->swo.metadata) > 0 && strcmp(state->swo.metadata, "-") != 0) {
                ctf_parse_cleanup();
                ctf_decode_cleanup();
                ctf_error_notify(CTFERR_NONE, 0, NULL);
                if (ctf_parse_init(state->swo.metadata) && ctf_parse_run()) {
                  if (state->dwarf_loaded)
                    ctf_set_symtable(&dwarf_symboltable);
                } else {
                  ctf_parse_cleanup();
                }
              }
              serial_info_mode(NULL);
              tab_states[TAB_SERMON] = NK_MAXIMIZED;  /* make sure the serial monitor view is open */
            } else {
              console_add("Failed to configure the port.\n", STRFLG_STATUS);
            }
          } else if (result == 2 && sermon_isopen()) {
            sermon_close();
          } else if (result == 3) {
            serial_info_mode(NULL);
          }
        } else if ((result = handle_trace_cmd(state->console_edit, &state->swo)) != 0) {
          if (result == 1) {
            state->monitor_cmd_active = true;   /* to silence output of scripts */
            if (state->swo.enabled)
              RESETSTATE(state, STATE_SWOTRACE);
            else
              RESETSTATE(state, STATE_SWOCHANNELS);  /* special case: disabling SWO tracing disables all channels */
          } else if (result == 2) {
            state->monitor_cmd_active = true;   /* to silence output of scripts */
            RESETSTATE(state, STATE_SWOCHANNELS);
          } else if (result == 3) {
            trace_info_mode(&state->swo, 1, NULL);
          }
          tab_states[TAB_SWO] = NK_MAXIMIZED;  /* make sure the SWO tracing view is open */
        } else if (handle_x_cmd(state->console_edit, &state->memdump)) {
          if (state->memdump.count > 0 && state->memdump.size > 0) {
            /* if in stopped state, perform the memory view command right
               away, otherwise make sure it is scheduled */
            if (state->curstate == STATE_STOPPED)
              RESETSTATE(state, STATE_VIEWMEMORY);
            else
              state->refreshflags |= REFRESH_MEMORY;
            tab_states[TAB_MEMORY] = NK_MAXIMIZED;  /* make sure the memory dump view is open */
          }
        } else if (!handle_list_cmd(state->console_edit, &dwarf_symboltable, &dwarf_filetable)
                   && !handle_find_cmd(state->console_edit)
                   && !handle_info_cmd(state->console_edit, &helptext_root, &state->popup_active, &state->reformat_help, &state->swo, &state->gdb_task)
                   && !handle_disasm_cmd(state->console_edit, &state->disassemble_mode)
                   && !handle_semihosting_cmd(state->console_edit)
                   && !handle_directory_cmd(state->console_edit, state->sourcepath, sizearray(state->sourcepath)))
        {
          char translated[sizearray(state->console_edit)];
          /* check monitor command, to avoid that the output should goes
             to the semihosting view */
          state->monitor_cmd_active = is_monitor_cmd(state->console_edit);
          strlcat(state->console_edit, "\n", sizearray(state->console_edit));
          /* translate any register names in the command */
          strcpy(translated, state->console_edit);
          svd_xlate_all_names(translated, sizearray(translated));
          if (task_stdin(&state->gdb_task, translated))
            console_input(state->console_edit);
        }
        /* check for a list of breakpoint commands, so that we can refresh
           the breakpoint list after the command executed */
        if (TERM_EQU(state->console_edit, "b", 1) ||
            TERM_EQU(state->console_edit, "break", 5) ||
            TERM_EQU(state->console_edit, "watch", 5) ||
            TERM_EQU(state->console_edit, "del", 3) ||
            TERM_EQU(state->console_edit, "delete", 6) ||
            TERM_EQU(state->console_edit, "clear", 5) ||
            TERM_EQU(state->console_edit, "disable", 7) ||
            TERM_EQU(state->console_edit, "enable", 6) ||
            TERM_EQU(state->console_edit, "dprintf", 7))
          state->refreshflags |= REFRESH_BREAKPOINTS | IGNORE_DOUBLE_DONE;

        // save most recent keyboard command in the command history list list
        console_history_add(&state->consoleedit_root, state->console_edit, false);
        state->consoleedit_next = NULL;
        state->console_edit[0] = '\0';
      }
    } /* if (curstate < STATE_START) */

    nk_group_end(ctx);
  }
}

static void widget_stringlist(struct nk_context *ctx, const char *id,
                              const STRINGLIST *root, unsigned *count)
{
  assert(ctx != NULL);
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window const *stwin = &ctx->style.window;
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, 0)) {
    float lineheight = 0;
    assert(count != NULL);
    unsigned count_prev = *count;
    *count = 0;
    for (STRINGLIST *item = root->next; item != NULL; item = item->next) {
      nk_layout_row_dynamic(ctx, opt_fontsize, 1);
      if (lineheight <= 0.1) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        lineheight = rcline.h;
      }
      nk_label(ctx, item->text, NK_TEXT_LEFT);
      *count += 1;
    }
    nk_group_end(ctx);
    if (*count != count_prev) {
      /* number of lines changed -> scroll to the last line */
      int ypos = 0;
      if (*count > 0) {
        assert(lineheight>0.1);
        int widgetlines = (int)((rcwidget.h - 2 * stwin->padding.y) / lineheight);
        ypos = (int)((*count - widgetlines + 1) * lineheight);
        if (ypos < 0)
          ypos = 0;
      }
      nk_group_set_scroll(ctx, id, 0, ypos);
    }
  }
  nk_style_pop_color(ctx);
}

static void refresh_panel_contents(APPSTATE *state, int new_state, int refreshflag)
{
  assert(state != NULL);

  /* if in stopped state, perform the memory view command right away,
     otherwise make sure it is scheduled */
  if (state->curstate == STATE_STOPPED)
    RESETSTATE(state, new_state);
  else
    state->refreshflags |= refreshflag;
}

static void panel_configuration(struct nk_context *ctx, APPSTATE *state,
                                enum nk_collapse_states *tab_state)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", tab_state, NULL)) {
#   define LABEL_WIDTH (5.0f * opt_fontsize)
    struct nk_rect bounds = nk_widget_bounds(ctx);
    float edtwidth = bounds.w - LABEL_WIDTH - BROWSEBTN_WIDTH - (2 * 5);

    /* debug probe (and IP address) */
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Probe", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
    nk_layout_row_push(ctx, edtwidth);
    bounds = nk_widget_bounds(ctx);
    int newprobe = nk_combo(ctx, state->probelist, state->netprobe+1, state->probe, (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5f*ROW_HEIGHT));
    if (newprobe == state->netprobe) {
      bool reconnect = false;
      nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
      nk_layout_row_push(ctx, LABEL_WIDTH);
      nk_label(ctx, "IP", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
      nk_layout_row_push(ctx, edtwidth);
      int result = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                    state->IPaddr, sizearray(state->IPaddr), nk_filter_ascii,
                                    "IP address of the ctxLink");
      if ((result & (NK_EDIT_COMMITED|NK_EDIT_DEACTIVATED)) != 0 && is_ip_address(state->IPaddr))
        reconnect = true;
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
          reconnect = true;
        } else {
          strlcpy(state->IPaddr, "none found", sizearray(state->IPaddr));
        }
      }
      if (reconnect)
        RESETSTATE(state, STATE_SCAN_BMP);
    }
    if (newprobe != state->probe) {
      state->probe = newprobe;
      RESETSTATE(state, STATE_SCAN_BMP);
    }

    /* GDB */
    char tiptext[_MAX_PATH];
    char basename[_MAX_PATH], *p;
    p = strrchr(state->GDBpath, DIRSEP_CHAR);
    strlcpy(basename, (p == NULL) ? state->GDBpath : p + 1, sizearray(basename));
    strlcpy(tiptext, (strlen(basename) > 0) ? state->GDBpath : "Path to the GDB executable", sizearray(tiptext));
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "GDB", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, edtwidth);
    bool error = editctrl_cond_color(ctx, !task_isrunning(&state->gdb_task), COLOUR_BG_DARKRED);
    int res = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_READ_ONLY,
                               basename, sizearray(basename), nk_filter_ascii, tiptext);
    editctrl_reset_color(ctx, error);
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT) || (res & NK_EDIT_BLOCKED) != 0) {
      nk_input_clear_mousebuttons(ctx);
#     if defined _WIN32
        osdialog_filters *filters = osdialog_filters_parse("Executables:exe;All files:*");
#     else
        osdialog_filters *filters = osdialog_filters_parse("Executables:*;All files:*");
#     endif
      char *fname = osdialog_file(OSDIALOG_OPEN, "Select GDB program", NULL, state->GDBpath, filters);
      osdialog_filters_free(filters);
      if (fname != NULL) {
        strlcpy(state->GDBpath, fname, sizearray(state->GDBpath));
        free(fname);
        /* terminate running instance of GDB (if any), and restart */
        task_close(&state->gdb_task);
        RESETSTATE(state, STATE_INIT);
      }
    }
    nk_layout_row_end(ctx);

    /* target executable */
    p = strrchr(state->ELFfile, '/');
    strlcpy(basename, (p == NULL) ? state->ELFfile : p + 1, sizearray(basename));
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "ELF file", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, edtwidth);
    strlcpy(tiptext, (strlen(state->ELFfile) > 0) ? state->ELFfile : "Path to the target ELF file", sizearray(tiptext));
    error = editctrl_cond_color(ctx, strlen(state->ELFfile) == 0 || access(state->ELFfile, 0) != 0, COLOUR_BG_DARKRED);
    res = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_READ_ONLY,
                           basename, sizearray(basename), nk_filter_ascii, tiptext);
    editctrl_reset_color(ctx, error);
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT) || (res & NK_EDIT_BLOCKED)) {
      nk_input_clear_mousebuttons(ctx);
      translate_path(state->ELFfile, true);
      osdialog_filters *filters = osdialog_filters_parse("ELF Executables:elf;All files:*");
      char *fname = osdialog_file(OSDIALOG_OPEN, "Select ELF executable", NULL, state->ELFfile, filters);
      osdialog_filters_free(filters);
      if (fname != NULL) {
        strlcpy(state->ELFfile, fname, sizearray(state->ELFfile));
        free(fname);
        if (state->curstate > STATE_FILE)
          RESETSTATE(state, STATE_FILE);
      }
      translate_path(state->ELFfile, false);
    }
    nk_layout_row_end(ctx);

    /* target entry point */
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Entry point", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, edtwidth + BROWSEBTN_WIDTH);
    res = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                           state->EntryPoint, sizearray(state->EntryPoint), nk_filter_ascii,
                           "The name of the entry point function (if not \"main\")");
    nk_layout_row_end(ctx);
    if (res & NK_EDIT_ACTIVATED)
      state->console_activate = 0;

    /* CMSIS SVD file */
    bool reload_svd = false;
    p = strrchr(state->SVDfile, '/');
    strlcpy(basename, (p == NULL) ? state->SVDfile : p + 1, sizearray(basename));
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "SVD", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, edtwidth);
    strlcpy(tiptext, (strlen(state->SVDfile) > 0) ? state->SVDfile : "Path to an SVD file with the MCU description & registers", sizearray(tiptext));
    error = editctrl_cond_color(ctx, strlen(state->SVDfile) > 0 && access(state->SVDfile, 0) != 0, COLOUR_BG_DARKRED);
    res = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER, basename, sizearray(basename),
                           nk_filter_ascii, tiptext);
    editctrl_reset_color(ctx, error);
    if (res & NK_EDIT_ACTIVATED)
      state->console_activate = 0;
    if ((res & (NK_EDIT_COMMITED|NK_EDIT_DEACTIVATED)) != 0)
      reload_svd = true;
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
      nk_input_clear_mousebuttons(ctx);
      translate_path(state->SVDfile, true);
      osdialog_filters *filters = osdialog_filters_parse("CMSIS SVD files:svd;All files:*");
      char *fname = osdialog_file(OSDIALOG_OPEN, "Select CMSIS SVD file", NULL, state->SVDfile, filters);
      osdialog_filters_free(filters);
      if (fname != NULL) {
        strlcpy(state->SVDfile, fname, sizearray(state->SVDfile));
        free(fname);
        if (state->curstate > STATE_FILE)
          reload_svd = true;
      }
      translate_path(state->SVDfile, false);
    }
    nk_layout_row_end(ctx);
    if (reload_svd) {
      svd_clear();
      if (strlen(state->SVDfile) > 0) {
        translate_path(state->SVDfile, true);
        svd_load(state->SVDfile);
        translate_path(state->SVDfile, false);
      }
    }

    /* source directory */
    bool reload_sources = false;
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    nk_label(ctx, "Sources", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, edtwidth);
    res = editctrl_tooltip(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                           state->sourcepath, sizearray(state->sourcepath), nk_filter_ascii,
                           "Path to the source files (in case these were moved after the build)");
    if (res & NK_EDIT_ACTIVATED)
      state->console_activate = 0;
    if ((res & (NK_EDIT_COMMITED|NK_EDIT_DEACTIVATED)) != 0)
      reload_sources = true;
    nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
    if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
      nk_input_clear_mousebuttons(ctx);
      char *fname = osdialog_file(OSDIALOG_OPEN_DIR, "Path to Source files", NULL, state->sourcepath, NULL);
      if (fname != NULL) {
        strlcpy(state->sourcepath, fname, sizearray(state->sourcepath));
        free(fname);
        //??? also pass on the GDB: -environment-directory -r path "path" ...
        if (state->curstate > STATE_GET_SOURCES)
          reload_sources = true;
      }
    }
    nk_layout_row_end(ctx);
    if (reload_sources)
      sources_reload(state->sourcepath, state->debugmode);

    /* TPWR option */
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Power Target (3.3V)", &state->tpwr, NK_TEXT_LEFT, "Let the debug probe provide power to the target")) {
      state->monitor_cmd_active = true;
      if (!state->tpwr)
        task_stdin(&state->gdb_task, "monitor tpwr disable\n");
      if (state->tpwr && state->curstate != STATE_MON_SCAN)
        task_stdin(&state->gdb_task, "monitor tpwr enable\n");
      if (state->curstate == STATE_MON_SCAN)
        RESETSTATE(state, STATE_MON_TPWR);
      else
        state->monitor_cmd_finish = true;
    }

    /* reset during connect */
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Reset target during connect", &state->connect_srst, NK_TEXT_LEFT,
                         "Keep target MCU reset while debug probe attaches"))
    {
      state->monitor_cmd_active = true;
      char cmd[50];
      if (exist_monitor_cmd("connect_srst", state->monitor_cmds))
        strcpy(cmd, "monitor connect_srst");
      else
        strcpy(cmd, "monitor connect_rst");
      if (state->connect_srst)
        strcat(cmd, " enable\n");
      else
        strcat(cmd, " disable\n");
      task_stdin(&state->gdb_task, cmd);
      RESETSTATE(state, STATE_MON_SCAN);
    }

    /* auto-download */
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    checkbox_tooltip(ctx, "Download to target on mismatch", &state->autodownload, NK_TEXT_LEFT,
                     "Download firmware to the target MCU if it is different from the code currently in it");

    /* show all GDB output */
    nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
    if (checkbox_tooltip(ctx, "Show all GDB messages", &state->allmsg, NK_TEXT_LEFT, "Do not filter GDB output in the console to only relevant messages"))
      console_hiddenflags = state->allmsg ? 0 : STRFLG_NOTICE | STRFLG_RESULT | STRFLG_EXEC | STRFLG_MI_INPUT | STRFLG_TARGET | STRFLG_SCRIPT;

    nk_tree_state_pop(ctx);
  }
}

static void panel_breakpoints(struct nk_context *ctx, APPSTATE *state,
                              enum nk_collapse_states *tab_state)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  nk_sizer_refresh(&state->sizerbar_breakpoints);
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Breakpoints", tab_state, NULL)) {
    char label[300];
    /* find longest breakpoint description */
    struct nk_user_font const *font = ctx->style.font;
    float width = 0;
    for (BREAKPOINT *bp = breakpoint_root.next; bp != NULL; bp = bp->next) {
      float w;
      if (bp->flags & BKPTFLG_FUNCTION) {
        assert(bp->name != NULL);
        strlcpy(label, bp->name, sizearray(label));
      } else if (source_getname(bp->filenr) != NULL) {
        snprintf(label, sizearray(label), "%s : %d", source_getname(bp->filenr), bp->linenr);
      }
      w = font->width(font->userdata, font->height, label, strlen(label)) + 10;
      if (w > width)
        width = w;
    }

    nk_layout_row_dynamic(ctx, state->sizerbar_breakpoints.size, 1);
    nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
    if (nk_group_begin(ctx, "breakpoints", 0)) {
      for (BREAKPOINT *bp = breakpoint_root.next; bp != NULL; bp = bp->next) {
        int en;
        nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
        nk_layout_row_push(ctx, LABEL_WIDTH);
        snprintf(label, sizearray(label), "%d", bp->number);
        en = bp->enabled;
        if (nk_checkbox_label(ctx, label, &en, NK_TEXT_LEFT)) {
          RESETSTATE(state, STATE_BREAK_TOGGLE);
          state->stateparam[0] = (en == 0) ? STATEPARAM_BP_DISABLE : STATEPARAM_BP_ENABLE;
          state->stateparam[1] = bp->number;
        }
        nk_layout_row_push(ctx, width);
        if (bp->flags & BKPTFLG_FUNCTION) {
          assert(bp->name != NULL);
          strlcpy(label, bp->name, sizearray(label));
        } else if (source_getname(bp->filenr) != NULL) {
          snprintf(label, sizearray(label), "%s : %d", source_getname(bp->filenr), bp->linenr);
        } else {
          strlcpy(label, "??", sizearray(label));
        }
        nk_label(ctx, label, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, ROW_HEIGHT);
        if (nk_button_symbol(ctx, NK_SYMBOL_X)) {
          RESETSTATE(state, STATE_BREAK_TOGGLE);
          state->stateparam[0] = STATEPARAM_BP_DELETE;
          state->stateparam[1] = bp->number;
        }
      }
      if (breakpoint_root.next == NULL) {
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        nk_label(ctx, "No breakpoints", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE);
      }
      nk_group_end(ctx);
    }
    nk_style_pop_color(ctx);

    nk_sizer(ctx, &state->sizerbar_breakpoints);
    nk_tree_state_pop(ctx);
  }
}

static int label_formatmenu(struct nk_context *ctx, const char *text, int changeflag,
                            int format, float rowheight)
{
  struct nk_rect bounds = nk_layout_widget_bounds(ctx);
  if (changeflag)
    nk_label_colored(ctx, text, NK_TEXT_LEFT, COLOUR_FG_RED);
  else
    nk_label(ctx, text, NK_TEXT_LEFT);

  /* check if the text is a number (for which we can choose the format) */
  int rows = 4; /* 4 integer display formats */
  int ok = 0;
  if (isdigit(*text)) {
    if (text[0] == '0' && tolower(text[1]) == 'x') {
      char c;
      int count = 0;
      const char *ptr;
      for (ptr = text + 2; (c = tolower(*ptr)) != '\0' && (isdigit(c) || (c >= 'a' && c <= 'f')); ptr++)
        count += 1;
      if (*ptr == '\0' && count > 0)
        ok = 1; /* entire string forms a valid integer (note: 0x is invalid, 0x1 is ok) */
    } else {
      const char *ptr;
      for (ptr = text; isdigit(*ptr); ptr++)
        {}
      if (*ptr == '\0')
        ok = 1; /* entire string forms a valid integer (decimal, octal or binary) */
    }
  } else if (*text == '"') {
    ok = 1;     /* string may be converted to array of bytes */
    rows += 1;  /* extra format to show as a string */
  } else if (*text == '{') {
    /* check whether all entries in the array are between 0..255 (when unsigned) */
    ok = 1;
    while (ok && *text != '\0' && *text != '}') {
      char *tail;
      long v = strtol(text + 1, &tail, 0);
      if (v < -127 || v > 255 || (tail == text + 1))
        ok = 0;
      text = skipwhite(tail);
    }
  }
  if (!ok)
    return FORMAT_NATURAL;

  /* add a context menu to select the number format */
  float spacing = ctx->style.window.spacing.y;
  float padding = ctx->style.button.padding.y;
  if (nk_contextual_begin(ctx, NK_PANEL_CONTEXTUAL, nk_vec2(120, rows*(rowheight + spacing + padding)), bounds)) {
    nk_layout_row_dynamic(ctx, rowheight, 1);
    if (nk_contextual_item_symbol_label(ctx, (format == FORMAT_DECIMAL) ? NK_SYMBOL_CIRCLE_SOLID : NK_SYMBOL_NONE,
                                        "Decimal", NK_TEXT_LEFT))
      format = FORMAT_DECIMAL;
    if (nk_contextual_item_symbol_label(ctx, (format == FORMAT_HEX) ? NK_SYMBOL_CIRCLE_SOLID : NK_SYMBOL_NONE,
                                        "Hexadecimal", NK_TEXT_LEFT))
      format = FORMAT_HEX;
    if (nk_contextual_item_symbol_label(ctx, (format == FORMAT_OCTAL) ? NK_SYMBOL_CIRCLE_SOLID : NK_SYMBOL_NONE,
                                        "Octal", NK_TEXT_LEFT))
      format = FORMAT_OCTAL;
    if (nk_contextual_item_symbol_label(ctx, (format == FORMAT_BINARY) ? NK_SYMBOL_CIRCLE_SOLID : NK_SYMBOL_NONE,
                                        "Binary", NK_TEXT_LEFT))
      format = FORMAT_BINARY;
    if (rows >= FORMAT_STRING
        && nk_contextual_item_symbol_label(ctx, (format == FORMAT_STRING) ? NK_SYMBOL_CIRCLE_SOLID : NK_SYMBOL_NONE,
                                           "String", NK_TEXT_LEFT))
      format = FORMAT_STRING;
    nk_contextual_end(ctx);
  }
  return format;
}

static void panel_locals(struct nk_context *ctx, APPSTATE *state,
                         enum nk_collapse_states *tab_state, float rowheight)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  nk_sizer_refresh(&state->sizerbar_locals);
  int result = *tab_state;  /* save old state */
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Locals", tab_state, NULL)) {
    if (result == NK_MINIMIZED)
      refresh_panel_contents(state, STATE_LIST_LOCALS, REFRESH_LOCALS);

    /* find longest variable name and value */
    float namewidth = 0;
    float valwidth = 2 * ROW_HEIGHT;
    struct nk_user_font const *font = ctx->style.font;
    for (LOCALVAR *var = localvar_root.next; var != NULL; var = var->next) {
      assert(var->name != NULL);
      float w = font->width(font->userdata, font->height, var->name, strlen(var->name)) + 10;
      if (w > namewidth)
        namewidth = w;
      assert(var->value != NULL);
      const char *ptr = (var->value_fmt != NULL) ? var->value_fmt : var->value;
      w = font->width(font->userdata, font->height, ptr, strlen(ptr)) + 10;
      if (w > valwidth)
        valwidth = w;
    }

    nk_layout_row_dynamic(ctx, state->sizerbar_locals.size, 1);
    nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
    if (nk_group_begin(ctx, "locals", 0)) {
      for (LOCALVAR *var = localvar_root.next; var != NULL; var = var->next) {
        nk_layout_row_begin(ctx, NK_STATIC, rowheight, 2);
        nk_layout_row_push(ctx, namewidth);
        nk_label(ctx, var->name, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, valwidth);
        const char *ptr = (var->value_fmt != NULL) ? var->value_fmt : var->value;
        int format = label_formatmenu(ctx, ptr, (var->flags & LOCALFLG_CHANGED), var->format, rowheight);
        if (format != var->format) {
          var->format = format;
          char valstr[40];
          strlcpy(valstr, var->value, sizearray(valstr));
          change_integer_format(valstr, sizearray(valstr), var->format);
          if (var->value_fmt != NULL)
            free((void*)var->value_fmt);
          var->value_fmt = strdup(valstr);
        }
      }
      if (localvar_root.next == NULL) {
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        nk_label(ctx, "No locals", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE);
      }
      nk_group_end(ctx);
    }
    nk_style_pop_color(ctx);

    nk_sizer(ctx, &state->sizerbar_locals);
    nk_tree_state_pop(ctx);
  }
}

static void panel_watches(struct nk_context *ctx, APPSTATE *state,
                          enum nk_collapse_states *tab_state, float rowheight)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  nk_sizer_refresh(&state->sizerbar_watches);
  int result = *tab_state;  /* save old state */
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Watches", tab_state, NULL)) {
    /* if previous state for the memory was closed, force an update of the memory view */
    if (result == NK_MINIMIZED)
      refresh_panel_contents(state, STATE_LIST_WATCHES, REFRESH_WATCHES);

    /* find longest watch expression and value */
    struct nk_user_font const *font = ctx->style.font;
    float namewidth = 0;
    float valwidth = 2 * ROW_HEIGHT;
    for (WATCH *watch = watch_root.next; watch != NULL; watch = watch->next) {
      assert(watch->expr != NULL);
      float w = font->width(font->userdata, font->height, watch->expr, strlen(watch->expr)) + 10;
      if (w > namewidth)
        namewidth = w;
      if (watch->value != NULL) {
        w = font->width(font->userdata, font->height, watch->value, strlen(watch->value)) + 10;
        if (w > valwidth)
          valwidth = w;
      }
    }

    nk_layout_row_dynamic(ctx, state->sizerbar_watches.size, 1);
    nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
    if (nk_group_begin(ctx, "watches", 0)) {
      for (WATCH *watch = watch_root.next; watch != NULL; watch = watch->next) {
        nk_layout_row_begin(ctx, NK_STATIC, rowheight, 4);
        nk_layout_row_push(ctx, LABEL_WIDTH);
        char label[20];
        snprintf(label, sizearray(label), "%u", watch->seqnr); /* print sequence number for undisplay command */
        nk_label(ctx, label, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, namewidth);
        nk_label(ctx, watch->expr, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, valwidth);
        if (watch->value != NULL) {
          int format = label_formatmenu(ctx, watch->value, (watch->flags & WATCHFLG_CHANGED), watch->format, rowheight);
          if (format > 0 && format != watch->format) {
            RESETSTATE(state, STATE_WATCH_FORMAT);
            state->stateparam[0] = watch->seqnr;
            state->stateparam[1] = format;
          }
        } else {
          nk_label(ctx, "?", NK_TEXT_LEFT);
        }
        nk_layout_row_push(ctx, ROW_HEIGHT);
        if (nk_button_symbol(ctx, NK_SYMBOL_X)) {
          RESETSTATE(state, STATE_WATCH_TOGGLE);
          state->stateparam[0] = STATEPARAM_WATCH_DEL;
          state->stateparam[1] = watch->seqnr;
        }
      }
      if (watch_root.next == NULL) {
        nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
        nk_label(ctx, "No watches", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE);
      }
      nk_group_end(ctx);
    }
    nk_style_pop_color(ctx);

    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
    nk_layout_row_push(ctx, LABEL_WIDTH);
    float w = namewidth + valwidth + ctx->style.window.spacing.x;
    if (w < 150)
      w = 150;
    nk_layout_row_push(ctx, w);
    result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER,
                                            state->watch_edit, sizearray(state->watch_edit),
                                            nk_filter_ascii);
    nk_layout_row_push(ctx, ROW_HEIGHT);
    if ((nk_button_symbol(ctx, NK_SYMBOL_PLUS) || (result & NK_EDIT_COMMITED))
        && state->curstate == STATE_STOPPED && strlen(state->watch_edit) > 0) {
      RESETSTATE(state, STATE_WATCH_TOGGLE);
      state->stateparam[0] = STATEPARAM_WATCH_SET;
      state->stateparam[1] = FORMAT_NATURAL;
      strlcpy(state->statesymbol, state->watch_edit, sizearray(state->statesymbol));
      state->watch_edit[0] = '\0';
    } else if (result & NK_EDIT_ACTIVATED) {
      state->console_activate = 0;
    }

    nk_sizer(ctx, &state->sizerbar_watches);
    nk_tree_state_pop(ctx);
  }
}

static void panel_registers(struct nk_context *ctx, APPSTATE *state,
                            enum nk_collapse_states *tab_state, float rowheight)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  nk_sizer_refresh(&state->sizerbar_registers);
  int result = *tab_state;  /* save old state */
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Registers", tab_state, NULL)) {
    if (result == NK_MINIMIZED)
      refresh_panel_contents(state, STATE_LIST_REGISTERS, REFRESH_REGISTERS);

    float namewidth = 2 * ROW_HEIGHT;
    float valwidth = 4 * ROW_HEIGHT;
    nk_layout_row_dynamic(ctx, state->sizerbar_registers.size, 1);
    nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
    if (nk_group_begin(ctx, "registers", 0)) {
      for (int idx = 0; idx < sizearray(register_def); idx++) {
        nk_layout_row_begin(ctx, NK_STATIC, rowheight, 2);
        nk_layout_row_push(ctx, namewidth);
        nk_label(ctx, register_def[idx].name, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, valwidth);
        int fonttype = guidriver_setfont(ctx, FONT_MONO);
        char field[20];
        sprintf(field, "0x%08lx", register_def[idx].value);
        if (register_def[idx].flags & REGFLG_CHANGED)
          nk_label_colored(ctx, field, NK_TEXT_LEFT, COLOUR_FG_RED);
        else
          nk_label(ctx, field, NK_TEXT_LEFT);
        guidriver_setfont(ctx, fonttype);
      }
      nk_group_end(ctx);
    }
    nk_style_pop_color(ctx);

    nk_sizer(ctx, &state->sizerbar_registers);
    nk_tree_state_pop(ctx);
  }
}

static void panel_memory(struct nk_context *ctx, APPSTATE *state,
                             enum nk_collapse_states *tab_state, float rowheight)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  nk_sizer_refresh(&state->sizerbar_memory);
  int result = *tab_state;  /* save old state */
  if (nk_tree_state_push(ctx, NK_TREE_TAB, "Memory", tab_state, NULL)) {
    /* if previous state for the memory was closed, force an update of the memory view */
    if (result == NK_MINIMIZED)
      refresh_panel_contents(state, STATE_VIEWMEMORY, REFRESH_MEMORY);
    if (state->memdump.data != NULL) {
      memdump_widget(ctx, &state->memdump, state->sizerbar_memory.size, rowheight);
      nk_sizer(ctx, &state->sizerbar_memory); /* make view height resizeable */
    } else {
      nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
      if (state->memdump.message != NULL)
        nk_label_colored(ctx, state->memdump.message, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE, COLOUR_FG_RED);
      else
        nk_label(ctx, "Use \"x\" command to view memory", NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE);
    }
    nk_tree_state_pop(ctx);
  }
}

static void panel_semihosting(struct nk_context *ctx, APPSTATE *state,
                              enum nk_collapse_states *tab_state)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  /* highlight tab text if new content arrives and the tab is closed */
  bool highlight = !(*tab_state) && stringlist_count(&semihosting_root) != state->semihosting_lines;
  if (highlight)
    nk_style_push_color(ctx, &ctx->style.tab.text, COLOUR_FG_YELLOW);
  int result = nk_tree_state_push(ctx, NK_TREE_TAB, "Semihosting output", tab_state, NULL);
  if (highlight)
    nk_style_pop_color(ctx);

  nk_sizer_refresh(&state->sizerbar_semihosting);
  if (result) {
    nk_layout_row_dynamic(ctx, state->sizerbar_semihosting.size, 1);
    widget_stringlist(ctx, "semihosting", &semihosting_root, &state->semihosting_lines);
    nk_sizer(ctx, &state->sizerbar_semihosting);
    nk_tree_state_pop(ctx);
  }
}

static void panel_serialmonitor(struct nk_context *ctx, APPSTATE *state,
                                enum nk_collapse_states *tab_state)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  /* highlight tab text if new content arrives and the tab is closed */
  bool highlight = !(*tab_state) && sermon_countlines() != state->sermon_lines;
  if (highlight)
    nk_style_push_color(ctx, &ctx->style.tab.text, COLOUR_FG_YELLOW);
  int result = nk_tree_state_push(ctx, NK_TREE_TAB, "Serial console", tab_state, NULL);
  if (highlight)
    nk_style_pop_color(ctx);

  nk_sizer_refresh(&state->sizerbar_serialmon);
  if (result) {
    nk_layout_row_dynamic(ctx, state->sizerbar_serialmon.size, 1);
    struct nk_rect bounds = nk_layout_widget_bounds(ctx);
    nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
    if (nk_group_begin(ctx, "serial", 0)) {
      struct nk_user_font const *font = ctx->style.font;
      int linecount = 0;
      float lineheight = 0;
      const char *text;
      sermon_rewind();
      while ((text = sermon_next()) != NULL) {
        nk_layout_row_begin(ctx, NK_STATIC, opt_fontsize, 1);
        if (lineheight < 0.01) {
          struct nk_rect rcline = nk_layout_widget_bounds(ctx);
          lineheight = rcline.h;
        }
        size_t textlength = strlen(text);
        assert(font != NULL && font->width != NULL);
        float textwidth = font->width(font->userdata, font->height, text, textlength) + 10;
        nk_layout_row_push(ctx, textwidth);
        nk_text(ctx, text, textlength, NK_TEXT_LEFT);
        nk_layout_row_end(ctx);
        linecount += 1;
      }
      if (!sermon_isopen()) {
        struct nk_color clr = COLOUR_FG_RED;
        nk_layout_row_dynamic(ctx, opt_fontsize, 1);
        nk_label_colored(ctx, "No port opened", NK_TEXT_LEFT, clr);
        linecount += 1;
      }
      nk_group_end(ctx);
      if (linecount != state->sermon_lines && lineheight > 0.01) {
        /* scroll to end */
        int widgetlines = (int)((bounds.h - 2 * ctx->style.window.padding.y) / lineheight);
        int sermon_scroll = (int)((linecount - widgetlines) * lineheight);
        if (sermon_scroll < 0)
          sermon_scroll = 0;
        state->sermon_lines = linecount;
        nk_group_set_scroll(ctx, "serial", 0, sermon_scroll);
      }
    }
    nk_style_pop_color(ctx);
    nk_sizer(ctx, &state->sizerbar_serialmon);
    nk_tree_state_pop(ctx);
  }
}

static void panel_traceswo(struct nk_context *ctx, APPSTATE *state,
                           enum nk_collapse_states *tab_state)
{
  assert(ctx != NULL);
  assert(state != NULL);
  assert(tab_state != NULL);

  /* highlight tab text if new content arrives and the tab is closed */
  bool highlight = !(*tab_state) && tracestring_count() != state->swo_lines;
  if (highlight)
    nk_style_push_color(ctx, &ctx->style.tab.text, COLOUR_FG_YELLOW);
  int result = nk_tree_state_push(ctx, NK_TREE_TAB, "SWO tracing", tab_state, NULL);
  if (highlight)
    nk_style_pop_color(ctx);

  nk_sizer_refresh(&state->sizerbar_swo);
  if (result) {
    tracestring_process(state->trace_status == TRACESTAT_OK);
    nk_layout_row_dynamic(ctx, state->sizerbar_swo.size, 1);
    tracelog_widget(ctx, "tracelog", opt_fontsize, -1, -1, NULL, 0);
    state->swo_lines = tracestring_count();
    nk_sizer(ctx, &state->sizerbar_swo);
    nk_tree_state_pop(ctx);
  }
}

static bool bmp_parsetracereply(const char *reply, unsigned char *endpoint)
{
  bool ok = false;

  /* first try the old reply format (1.6 up to 1.8.2): <serial>:<interface>:<endpoint> */
  const char *ptr = strchr(reply, ':');
  if (ptr != NULL && strtol(ptr + 1, (char**)&ptr, 16) == BMP_IF_TRACE && *ptr == ':') {
    long ep = strtol(ptr + 1, NULL, 16);
    ok = (ep > 0x80); /* this must be an IN enpoint, so high bit must be set */
    if (endpoint != NULL)
      *endpoint = (unsigned char)ep;
  }

  /* reply changed in release 1.9: "Trace enabled for BMP serial <serial>, USB EP <endpoint>>" */
  if (!ok && strncmp(reply, "Trace enabled", 13) == 0) {
    ptr = strstr(reply, "USB EP");
    if (ptr != NULL) {
      long ep = strtol(ptr + 6, NULL, 16);
      if (endpoint != NULL)
        *endpoint = (unsigned char)(ep | 0x80); /* direction flag is not set in the reply */
      ok = true;
    }
  }

  return ok;
}

static void handle_kbdinput_main(struct nk_context *ctx, APPSTATE *state)
{
  if (nk_input_is_key_pressed(&ctx->input, NK_KEY_UP) && source_cursorline > 1) {
    source_cursorline--;
    source_autoscroll = false;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN) && source_cursorline < source_linecount(source_cursorfile)) {
    source_cursorline++;
    source_autoscroll = false;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_UP)) {
    source_cursorline -= source_vp_rows;
    if (source_cursorline < 1)
      source_cursorline = 1;
    source_autoscroll = false;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_DOWN)) {
    int linecount = source_linecount(source_cursorfile);
    source_cursorline += source_vp_rows;
    if (source_cursorline > linecount)
      source_cursorline = linecount;
    source_autoscroll = false;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_TOP)) {
    source_cursorline = 1;
    source_autoscroll = false;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_BOTTOM)) {
    source_cursorline = source_linecount(source_cursorfile);
    source_autoscroll = false;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_ALT_UP) || nk_input_is_key_pressed(&ctx->input, NK_KEY_ALT_LEFT)) {
    follow_address(state, -1);
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_ALT_DOWN) || nk_input_is_key_pressed(&ctx->input, NK_KEY_ALT_RIGHT)) {
    follow_address(state, 1);
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_FIND)) {
    strlcpy(state->console_edit, "find ", sizearray(state->console_edit));
    state->console_activate = 2;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F3)) {
    handle_find_cmd("find");  /* find next */
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_GOTO)) {
    strlcpy(state->console_edit, "list ", sizearray(state->console_edit));
    state->console_activate = 2;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F9)) {
    toggle_breakpoint(state, source_cursorfile, source_cursorline);
  } else if (state->console_isactive & nk_input_is_key_pressed(&ctx->input, NK_KEY_PAR_UP)) {
    state->consoleedit_next = console_history_step(&state->consoleedit_root, state->consoleedit_next, 0);
    if (state->consoleedit_next != NULL)
      strlcpy(state->console_edit, state->consoleedit_next->text, sizearray(state->console_edit));
  } else if (state->console_isactive & nk_input_is_key_pressed(&ctx->input, NK_KEY_PAR_DOWN)) {
    state->consoleedit_next = console_history_step(&state->consoleedit_root, state->consoleedit_next, 1);
    if (state->consoleedit_next != NULL)
      strlcpy(state->console_edit, state->consoleedit_next->text, sizearray(state->console_edit));
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_REFRESH)) {
    state->consoleedit_next = console_history_match(&state->consoleedit_root, state->consoleedit_next, state->console_edit, sizearray(state->console_edit));
    if (state->consoleedit_next != NULL) {
      strlcpy(state->console_edit, state->consoleedit_next->text, sizearray(state->console_edit));
      state->console_activate = 2;
    }
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_TAB)) {
    if (console_autocomplete(state->console_edit, sizearray(state->console_edit), &dwarf_symboltable))
      state->console_activate = 2;
  } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
    state->console_edit[0] = '\0';
    state->console_activate = 2;
  }
}

static void handle_stateaction(APPSTATE *state, const enum nk_collapse_states tab_states[TAB_COUNT])
{
  assert(state != NULL);
  assert(state->cmdline != NULL);

  if (!is_idle()) {
    switch (state->curstate) {
    case STATE_INIT:
      /* kill GDB if it is running */
      if (task_isrunning(&state->gdb_task))
        task_close(&state->gdb_task);
      RESETSTATE(state, STATE_GDB_TASK);
      state->refreshflags = 0;
      state->is_attached = false;
      state->atprompt = false;
      state->cont_is_run = false;
      state->target_errmsg_set = false;
      break;
    case STATE_GDB_TASK:
      assert(!task_isrunning(&state->gdb_task));
      if (strlen(state->GDBpath) == 0 || access(state->GDBpath, 0) != 0) {
#       if defined _WIN32
          pathsearch(state->GDBpath, sizearray(state->GDBpath), "arm-none-eabi-gdb.exe");
#       else
          if (!pathsearch(state->GDBpath, sizearray(state->GDBpath), "arm-none-eabi-gdb"))
            pathsearch(state->GDBpath, sizearray(state->GDBpath), "gdb-multiarch");
#       endif
      }
      if (strlen(state->GDBpath) > 0 && task_launch(state->GDBpath, "--interpreter=mi2", &state->gdb_task)) {
        RESETSTATE(state, STATE_SCAN_BMP); /* GDB started, now find Black Magic Probe */
      } else {
        if (STATESWITCH(state)) {
          if (strlen(state->GDBpath) == 0)
            console_add("Path to GDB is not set, check the configuration\n", STRFLG_ERROR);
          else
            console_add("GDB failed to launch, check the configuration\n", STRFLG_ERROR);
        }
        MARKSTATE(state);
        set_idle_time(1000); /* repeat scan on timeout (but don't sleep the GUI thread) */
      }
      break;
    case STATE_SCAN_BMP:
      state->port_gdb[0] = '\0';
      if (state->probe == state->netprobe) {
        if (is_ip_address(state->IPaddr)) {
          char portnr[20];
          sprintf(portnr, ":%d", BMP_PORT_GDB);
          strlcpy(state->port_gdb, state->IPaddr, sizearray(state->port_gdb));
          strlcat(state->port_gdb, portnr, sizearray(state->port_gdb));
        }
      } else {
        const char *ptr;
        ptr = state->probelist[state->probe];
        if (*ptr != '\0' && *ptr != '-') {
          if (strncmp(ptr, "COM", 3)== 0 && strlen(ptr)>= 5) {
            /* special case for Microsoft Windows, for COM ports >= 10 */
            strlcpy(state->port_gdb, "\\\\.\\", sizearray(state->port_gdb));
          }
          strlcat(state->port_gdb, ptr, sizearray(state->port_gdb));
        }
      }
      if (state->port_gdb[0] != '\0') {
        RESETSTATE(state, STATE_GDBVERSION);
      } else if (state->atprompt) {
        if (STATESWITCH(state)) {
          if (state->probe == state->netprobe)
            console_add("ctxLink Probe not found, invalid IP address\n", STRFLG_ERROR);
          else
            console_add("Black Magic Probe not found\n", STRFLG_ERROR);
          MARKSTATE(state);
        }
        set_idle_time(1000); /* repeat scan on timeout (but don't sleep the GUI thread) */
      }
      log_console_strings(state);
      gdbmi_sethandled(false);
      break;
    case STATE_GDBVERSION:
      if (state->atprompt) {
        /* walk over the introductory text */
        STRINGLIST *item;
        for (item = consolestring_root.next; item != NULL; item = item->next) {
          assert(item->text != NULL);
          if (strncmp(item->text, "GNU gdb", 7) == 0) {
            /* first try to find a number behind a closing paranthesis */
            int major = 0, minor = 0, build = 0, parts = 0;
            const char *ptr = strchr(item->text, ')');
            if (ptr != NULL) {
              while (*ptr != '\0' && !isdigit(*ptr))
                ptr++;
              if (*ptr != '\0')
                parts = sscanf(ptr, "%d.%d.%d", &major, &minor, &build);
            }
            /* then try to find a version anywhere */
            if (parts < 3 || major < 4) {
              for (ptr = item->text; *ptr != '\0' && !isdigit(*ptr); ptr++)
                {}
              if (*ptr != '\0')
                parts = sscanf(ptr, "%d.%d.%d", &major, &minor, &build);
            }
            /* store in appstate */
            if (parts >= 2 || major >= 4)
              state->gdbversion = (major << 24) | (minor << 16) | (build & 0xffff);
            break;
          }
        }
        MOVESTATE(state, STATE_FILE);
        log_console_strings(state);
        gdbmi_sethandled(true);
      }
      break;
    case STATE_FILE:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        char temp[_MAX_PATH];
        dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
        svd_clear();
        /* create parameter filename from target filename, then read target-specific settings */
        strlcpy(state->ParamFile, state->ELFfile, sizearray(state->ParamFile));
        strlcat(state->ParamFile, ".bmcfg", sizearray(state->ParamFile));
        load_targetoptions(state->ParamFile, state);
        /* load target filename in GDB */
        snprintf(state->cmdline, CMD_BUFSIZE, "-file-exec-and-symbols %s\n",
                 enquote(temp, state->ELFfile, sizeof(temp)));
        if (task_stdin(&state->gdb_task, state->cmdline))
          console_input(state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
          /* read the DWARF information (for function and variable lookup) */
          FILE *fp = fopen(state->ELFfile, "rb");
          if (fp != NULL) {
            int address_size;
            state->dwarf_loaded = dwarf_read(fp, &dwarf_linetable, &dwarf_symboltable,
                                             &dwarf_filetable, &address_size);
            if (!state->dwarf_loaded)
              console_add("No DWARF debug information\n", STRFLG_ERROR);
            fclose(fp);
          }
          /* read a CMSIS "SVD" file if any was provided */
          if (strlen(state->SVDfile) > 0)
            svd_load(state->SVDfile);
          /* (re-)load a TSDL metadata file, if any was provided */
          if (strlen(state->swo.metadata) > 0 && strcmp(state->swo.metadata, "-") != 0) {
            ctf_parse_cleanup();
            ctf_decode_cleanup();
            ctf_error_notify(CTFERR_NONE, 0, NULL);
            if (ctf_parse_init(state->swo.metadata) && ctf_parse_run()) {
              if (state->dwarf_loaded)
                ctf_set_symtable(&dwarf_symboltable);
            } else {
              ctf_parse_cleanup();
            }
          }
          source_cursorfile = source_cursorline = 0;
          source_execfile = source_execline = 0;
          /* if already attached, skip a to re-loading the sources */
          MOVESTATE(state, state->is_attached ? STATE_GET_SOURCES : STATE_TARGET_EXT);
        } else {
          if (strncmp(gdbmi_isresult(), "error", 5) == 0) {
            strlcpy(state->cmdline, gdbmi_isresult(), CMD_BUFSIZE);
            strlcat(state->cmdline, "\n", CMD_BUFSIZE);
            console_add(state->cmdline, STRFLG_ERROR);
          }
          set_idle_time(1000); /* stay in file state */
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_TARGET_EXT:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        snprintf(state->cmdline, CMD_BUFSIZE, "-target-select extended-remote %s\n", state->port_gdb);
        if (task_stdin(&state->gdb_task, state->cmdline))
          console_input(state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        if (strncmp(gdbmi_isresult(), "connected", 9) == 0) {
          state->target_errmsg_set = false;
          MOVESTATE(state, STATE_PROBE_TYPE);
        } else {
          if (!state->target_errmsg_set) {
            if (strstr(state->cmdline, "Permission denied") != NULL)
              snprintf(state->cmdline, CMD_BUFSIZE, "Port %s permission denied (check user/group permissions)\n", state->port_gdb);
            else
              snprintf(state->cmdline, CMD_BUFSIZE, "Port %s busy or unavailable\n", state->port_gdb);
            console_add(state->cmdline, STRFLG_ERROR);
            state->target_errmsg_set = true;
          }
          if (get_bmp_count() > 0) {
            MOVESTATE(state, STATE_SCAN_BMP);
            set_idle_time(1000); /* drop back to scan (after on timeout) */
          }
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_PROBE_TYPE:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        state->console_mark = stringlist_getlast(&consolestring_root, STRFLG_RESULT, 0);
        assert(state->console_mark != NULL);
        task_stdin(&state->gdb_task, "monitor version\n");
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
          assert(state->console_mark != NULL && state->console_mark->text != NULL);
          assert(state->console_mark->flags & STRFLG_RESULT);
          state->console_mark = state->console_mark->next;  /* skip the mark */
          int type;
          while (state->console_mark != NULL && (state->console_mark->flags & STRFLG_RESULT) == 0
                 && (type = check_versionstring(state->console_mark->text)) == PROBE_UNKNOWN)
            state->console_mark = state->console_mark->next;
          if (type != PROBE_UNKNOWN) {
            state->probe_type = type;
            /* overrule defaults, if we know the probe */
            if (state->probe_type == PROBE_BMPv21 || state->probe_type == PROBE_BMPv23)
              state->swo.mode = SWOMODE_MANCHESTER;
            else if (state->probe_type == PROBE_CTXLINK)
              state->swo.mode = SWOMODE_ASYNC;
          }
          MOVESTATE(state, STATE_PROBE_CMDS_1);
          state->console_mark = NULL;
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_PROBE_CMDS_1:
    case STATE_PROBE_CMDS_2:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        state->console_mark = stringlist_getlast(&consolestring_root, STRFLG_RESULT, 0);
        assert(state->console_mark != NULL);
        task_stdin(&state->gdb_task, "monitor help\n");
        state->atprompt = false;
        if (state->monitor_cmds != NULL) {
          free((void*)state->monitor_cmds);
          state->monitor_cmds = NULL;
        }
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
#         define MONITOR_CMD_SIZE 512
          char *cmdlist = malloc(MONITOR_CMD_SIZE*sizeof(char));
          if (cmdlist != NULL) {
            cmdlist[0] = '\0';
            assert(state->console_mark != NULL && state->console_mark->text != NULL);
            assert(state->console_mark->flags & STRFLG_RESULT);
            state->console_mark = state->console_mark->next;  /* skip the mark */
            while (state->console_mark != NULL && (state->console_mark->flags & STRFLG_RESULT) == 0) {
              const char *tail = strstr(state->console_mark->text, "--");
              if (tail != NULL) {
                const char *head = skipwhite(state->console_mark->text);
                while (tail > head && *(tail - 1) <= ' ')
                  tail--;
                size_t len = tail - head;
                assert(len > 0);
                char cmdname[40];
                if (len > 0 && len < sizearray(cmdname)) {
                  strncpy(cmdname, head, len);
                  cmdname[len] = '\0';
                  if (cmdlist[0] != '\0')
                    strlcat(cmdlist, " ", MONITOR_CMD_SIZE);
                  strlcat(cmdlist, cmdname, MONITOR_CMD_SIZE);
                }
              }
              state->console_mark = state->console_mark->next;
            }
            state->monitor_cmds = cmdlist;
          }
          if (ISSTATE(state, STATE_PROBE_CMDS_1))
            MOVESTATE(state, STATE_CONNECT_SRST);
          else
            MOVESTATE(state, STATE_GET_SOURCES);
          state->console_mark = NULL;
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_CONNECT_SRST:
      if (!state->connect_srst) {
        MOVESTATE(state, STATE_MON_TPWR);
      } else {
        if (!state->atprompt)
          break;
        if (STATESWITCH(state)) {
          char cmd[50];
          if (exist_monitor_cmd("connect_srst", state->monitor_cmds))
            strcpy(cmd, "monitor connect_srst enable");
          else
            strcpy(cmd, "monitor connect_rst enable");
          task_stdin(&state->gdb_task, cmd);
          state->atprompt = false;
          MARKSTATE(state);
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0)
            MOVESTATE(state, STATE_MON_TPWR);
          log_console_strings(state);
          gdbmi_sethandled(false);
        }
      }
      break;
    case STATE_MON_TPWR:
      if (!state->tpwr) {
        MOVESTATE(state, STATE_MON_SCAN);
      } else {
        if (!state->atprompt)
          break;
        if (STATESWITCH(state)) {
          task_stdin(&state->gdb_task, "monitor tpwr enable\n");
          state->atprompt = false;
          MARKSTATE(state);
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0)
            MOVESTATE(state, STATE_MON_SCAN);
          else
            set_idle_time(500); /* stay in current state, TPWR may take a little time */
          log_console_strings(state);
          gdbmi_sethandled(false);
        }
      }
      break;
    case STATE_MON_SCAN:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        gdbmi_sethandled(false);
        state->console_mark = stringlist_getlast(&consolestring_root, STRFLG_RESULT, 0);
        assert(state->console_mark != NULL);
        task_stdin(&state->gdb_task, "monitor swdp_scan\n");
        state->atprompt = false;
        MARKSTATE(state);
        state->mcu_family[0] = '\0';
        state->mcu_architecture[0] = '\0';
        state->mcu_partid = 0;
        state->target_count = 0;
      } else if (gdbmi_isresult() != NULL) {
        if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
          /* save architecture */
          assert(state->console_mark != NULL && state->console_mark->text != NULL);
          assert(state->console_mark->flags & STRFLG_RESULT);
          STRINGLIST *item = state->console_mark->next;  /* skip the mark */
          while (item != NULL && (item->flags & STRFLG_RESULT) == 0) {
            const char *ptr = skipwhite(item->text);
            if (isdigit(*ptr))
              break;
            item = item->next;
          }
          /* expect: 1 STM32F10x medium density [M3/M4] */
          if (item != NULL && (item->flags & STRFLG_TARGET) != 0) {
            assert(item->text != NULL);
            const char *ptr = skipwhite(item->text);
            if (isdigit(*ptr)) {
              int val = strtol(ptr, (char**)&ptr, 10);
              if (val > state->target_count)
                state->target_count = val;
              ptr = skipwhite(ptr);
            }
            if (*ptr == '*')
              ptr = skipwhite(ptr + 1);
            assert(*ptr != '\0');
            strlcpy(state->mcu_family, ptr, sizearray(state->mcu_family));
            /* split off architecture (if present) */
            state->mcu_architecture[0] = '\0';
            if ((ptr = strrchr(state->mcu_family, ' ')) != NULL && ptr[1] == 'M' && isdigit(ptr[2])) {
              *(char*)ptr++ = '\0';
              strlcpy(state->mcu_architecture, ptr, sizearray(state->mcu_architecture));
            }
          } else {
            /* no targets found */
            item = stringlist_getlast(&consolestring_root, 0, STRFLG_RESULT);
            assert(item != NULL && item->text != NULL);
            strlcpy(state->cmdline, skipwhite(item->text), CMD_BUFSIZE);
            strlcat(state->cmdline, "\n", CMD_BUFSIZE);
            console_add(state->cmdline, STRFLG_ERROR);
          }
        }
        if (strlen(state->mcu_family) > 0) {
          bmscript_load(state->mcu_family, state->mcu_architecture);
          MOVESTATE(state, STATE_ASYNC_MODE);
        } else {
          set_idle_time(500);   /* stay in scan state, wait for more data */
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_ASYNC_MODE:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        strcpy(state->cmdline, "-gdb-set mi-async 1\n");
        if (task_stdin(&state->gdb_task, state->cmdline))
          console_input(state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        MOVESTATE(state, STATE_ATTACH);
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_ATTACH:
      if (check_stopped(&source_execfile, &source_execline, &exec_address)) {
        source_cursorfile = source_execfile;
        if (state->disassemble_mode)
          source_cursorline = line_addr2phys(source_execfile, exec_address);
        else
          source_cursorline = line_source2phys(source_execfile, source_execline);
        state->atprompt = true; /* in GDB 10, no prompt follows on error, so force it */
      }
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        assert(state->target_select > 0);
        int tgt = (state->target_select <= state->target_count) ? state->target_select : 1;
        sprintf(state->cmdline, "-target-attach %d\n", tgt);
        if (task_stdin(&state->gdb_task, state->cmdline))
          console_input(state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
          state->is_attached = true;
          MOVESTATE(state, STATE_PROBE_CMDS_2);
        } else {
          MOVESTATE(state, STATE_STOPPED);
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_GET_SOURCES:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        /* clear source files (if any were loaded); these will be reloaded
           after the list of source files is refreshed */
        sources_clear(false, state->sourcefiles);
        state->sourcefiles = NULL;
        state->sourcefiles_count = 0;
        /* also get the list of source files from GDB (it might use a different
           order of the file index numbers) */
        strcpy(state->cmdline, "-file-list-exec-source-files\n");
        if (task_stdin(&state->gdb_task, state->cmdline))
          console_input(state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        if (strncmp(gdbmi_isresult(), "done,", 5) == 0) {
          sources_parse(gdbmi_isresult() + 5, state->debugmode);      /* + 5 to skip "done" and comma */
          sources_reload(state->sourcepath, state->debugmode);        /* load missing sources from alternate path (if set) */
          sources_reload(state->ELFfile, state->debugmode);
          state->sourcefiles = sources_getnames(&state->sourcefiles_count); /* create array with names for the dropdown */
          state->warn_source_tstamps = !elf_up_to_date(state->ELFfile);     /* check timestamps of sources against elf file */
          MOVESTATE(state, STATE_MEMACCESS);
        } else {
          if (strncmp(gdbmi_isresult(), "error", 5) == 0) {
            strlcpy(state->cmdline, gdbmi_isresult(), CMD_BUFSIZE);
            strlcat(state->cmdline, "\n", CMD_BUFSIZE);
            console_add(state->cmdline, STRFLG_ERROR);
          }
          set_idle_time(1000); /* stay in attach state */
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_MEMACCESS:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        task_stdin(&state->gdb_task, "set mem inaccessible-by-default off\n");
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        /* GDB tends to "forget" this setting, so before running a script we
           drop back to this state, but then proceed with the appropriate
           script for the state */
        MOVESTATE(state, (state->nextstate > 0) ? state->nextstate : STATE_MEMMAP);
        state->nextstate = -1;
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_MEMMAP:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        if (bmscript_line_fmt("memremap", state->cmdline, NULL, 0)) {
          /* run first line from the script */
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
          MARKSTATE(state);
          console_replaceflags = STRFLG_LOG;  /* move LOG to SCRIPT, to hide script output by default */
          console_xlateflags = STRFLG_SCRIPT;
        } else {
          MOVESTATE(state, STATE_PARTID_1);
        }
      } else if (gdbmi_isresult() != NULL) {
        /* run next line from the script (on the end of the script, move to
           the next state) */
        if (bmscript_line_fmt(NULL, state->cmdline, NULL, 0)) {
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
        } else {
          console_replaceflags = console_xlateflags = 0;
          MOVESTATE(state, STATE_PARTID_1);
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;

    case STATE_PARTID_1:
      if (!exist_monitor_cmd("partid", state->monitor_cmds)) {
        MOVESTATE(state, STATE_PARTID_2);  /* no command for part id, use a script */
      } else {
        if (!state->atprompt)
          break;
        if (STATESWITCH(state)) {
          state->console_mark = stringlist_getlast(&consolestring_root, STRFLG_RESULT, 0);
          assert(state->console_mark != NULL);
          task_stdin(&state->gdb_task, "monitor partid\n");
          state->atprompt = false;
          MARKSTATE(state);
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
            assert(state->console_mark != NULL && state->console_mark->text != NULL);
            assert(state->console_mark->flags & STRFLG_RESULT);
            state->console_mark = state->console_mark->next;  /* skip the mark */
            while (state->console_mark != NULL && (state->console_mark->flags & STRFLG_RESULT) == 0) {
              if (strncmp(state->console_mark->text, "Part ID", 7) == 0) {
                const char *ptr = state->console_mark->text + 7;
                if (*ptr == ':')
                  ptr++;
                state->mcu_partid = strtoul(ptr, NULL, 0);
              }
              state->console_mark = state->console_mark->next;
            }
            MOVESTATE(state, state->force_download ? STATE_DOWNLOAD : STATE_VERIFY);
            state->console_mark = NULL;
          }
          log_console_strings(state);
          gdbmi_sethandled(false);
        }
      }
      break;

    case STATE_PARTID_2:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        if (bmscript_line_fmt("partid", state->cmdline, NULL, 0)) {
          /* run first line from the script */
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
          MARKSTATE(state);
          console_replaceflags = STRFLG_LOG;  /* move LOG to SCRIPT, to hide script output by default */
          console_xlateflags = STRFLG_SCRIPT;
        } else {
          MOVESTATE(state, state->force_download ? STATE_DOWNLOAD : STATE_VERIFY);
        }
      } else if (gdbmi_isresult() != NULL) {
        /* run next line from the script (on the end of the script, move to
           the next state) */
        if (bmscript_line_fmt(NULL, state->cmdline, NULL, 0)) {
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
        } else {
          /* read the last response of the script, because it contains the ID */
          STRINGLIST *item = stringlist_getlast(&consolestring_root, 0, STRFLG_RESULT|STRFLG_HANDLED);
          unsigned long id;
          if (sscanf(item->text, "$%*d = 0x%lx", &id) == 1) {
            state->mcu_partid = id;
            /* see whether to translate the mcu_family name, and to reload the scripts */
            const char *mcuname = mcuinfo_lookup(state->mcu_family, state->mcu_partid);
            if (mcuname != NULL) {
              strlcpy(state->mcu_family, mcuname, sizearray(state->mcu_family));
              bmscript_clear();
              bmscript_load(state->mcu_family, state->mcu_architecture);
            }
          }
          console_replaceflags = console_xlateflags = 0;
          MOVESTATE(state, state->force_download ? STATE_DOWNLOAD : STATE_VERIFY);
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_VERIFY:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        /* check whether the firmware CRC matches the file in GDB */
        task_stdin(&state->gdb_task, "compare-sections\n");
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        gdbmi_sethandled(false); /* first flag the result message as handled, to find the line preceding it */
        STRINGLIST *item;
        for (;;) {
          item = stringlist_getlast(&consolestring_root,0,STRFLG_HANDLED);
          if (item == NULL || strncmp(item->text, "warning:", 8) == 0)
            break;
          item->flags |= STRFLG_HANDLED;
        }
        if (item != NULL && strncmp(item->text, "warning:", 8) == 0) {
          MOVESTATE(state, STATE_DOWNLOAD);
          if (!state->autodownload) {
            /* make the mismatch stand out */
            while (item != NULL && (item->flags & STRFLG_RESULT) == 0) {
              item->flags = (item->flags & ~STRFLG_LOG) | STRFLG_ERROR;
              item = item->next;
            }
          }
        } else {
          MOVESTATE(state, STATE_CHECK_MAIN);
        }
        log_console_strings(state);
      }
      break;
    case STATE_DOWNLOAD:
      if (!state->autodownload && !state->force_download) {
        MOVESTATE(state, STATE_CHECK_MAIN);  /* skip this step if download is disabled */
        break;
      }
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        task_stdin(&state->gdb_task, "-target-download\n");
        state->atprompt = false;
        MARKSTATE(state);
        state->force_download = nk_false;
      } else if (gdbmi_isresult() != NULL) {
        STRINGLIST *item = stringlist_getlast(&consolestring_root, STRFLG_RESULT, STRFLG_HANDLED);
        assert(item != NULL);
        log_console_strings(state);
        gdbmi_sethandled(false);
        if (strncmp(item->text, "error", 5) == 0)
          item->flags = (item->flags & ~STRFLG_RESULT) | STRFLG_ERROR;
        MOVESTATE(state, STATE_CHECK_MAIN);
      }
      break;
    case STATE_CHECK_MAIN:
      /* first try to find "main" in the DWARF information (if this fails,
         we have failed to load/parse the DWARF information) */
      if (strlen(state->EntryPoint) == 0)
        strlcpy(state->EntryPoint, "main", sizearray(state->EntryPoint));
      if (STATESWITCH(state)) {
        const DWARF_SYMBOLLIST *entry = dwarf_sym_from_name(&dwarf_symboltable, state->EntryPoint, -1, -1);
        if (entry != NULL && entry->code_range != 0) {
          MOVESTATE(state, STATE_START);  /* main() found, restart program at main */
          break;
        }
      }
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        /* check whether the there is a function "main" */
        assert(strlen(state->EntryPoint) != 0);
        snprintf(state->cmdline, CMD_BUFSIZE, "info functions ^%s$\n", state->EntryPoint);
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        STRINGLIST *item;
        const char *ptr;
        gdbmi_sethandled(false); /* first flag the result message as handled, to find the line preceding it */
        item = stringlist_getlast(&consolestring_root, 0, STRFLG_HANDLED);
        assert(item != NULL && item->text != NULL);
        ptr = strstr(item->text, state->EntryPoint);
        if (ptr != NULL && (ptr == item->text || *(ptr - 1) == ' ')) {
          MOVESTATE(state, STATE_START);    /* main() found, restart program at main */
        } else {
          check_stopped(&source_execfile, &source_execline, &exec_address);
          source_cursorfile = source_execfile;
          if (state->disassemble_mode)
            source_cursorline = line_addr2phys(source_execfile, exec_address);
          else
            source_cursorline = line_source2phys(source_execfile, source_execline);
          MOVESTATE(state, STATE_STOPPED);  /* main() not found, stay stopped */
          state->cont_is_run = true;        /* but when "Cont" is pressed, "run" is performed */
        }
        log_console_strings(state);
      }
      break;
    case STATE_START:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        assert(strlen(state->EntryPoint) != 0);
        snprintf(state->cmdline, CMD_BUFSIZE, "-break-insert -t %s\n", state->EntryPoint);
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        MOVESTATE(state, STATE_EXEC_CMD);
        state->stateparam[0] = STATEPARAM_EXEC_RESTART;
        log_console_strings(state);
        gdbmi_sethandled(false);
        if (sermon_isopen())
          sermon_clear(); /* erase any serial input that was received while starting up */
      }
      break;
    case STATE_EXEC_CMD:
      if (STATESWITCH(state)) {
        switch (state->stateparam[0]) {
        case STATEPARAM_EXEC_RESTART:
        case STATEPARAM_EXEC_CONTINUE:
          if (state->cont_is_run || state->stateparam[0] == STATEPARAM_EXEC_RESTART) {
            strcpy(state->cmdline, "-exec-run --start\n");
            state->cont_is_run = false;   /* do this only once */
          } else {
            strcpy(state->cmdline, "-exec-continue\n");
          }
          break;
        case STATEPARAM_EXEC_STOP:
          strcpy(state->cmdline, "-exec-interrupt\n");
          break;
        case STATEPARAM_EXEC_NEXT:
          if (state->disassemble_mode)
            strcpy(state->cmdline, "-exec-next-instruction\n");
          else
            strcpy(state->cmdline, "-exec-next\n");
          break;
        case STATEPARAM_EXEC_STEP:
          if (state->disassemble_mode)
            strcpy(state->cmdline, "-exec-step-instruction\n");
          else
            strcpy(state->cmdline, "-exec-step\n");
          break;
        case STATEPARAM_EXEC_UNTIL:
          sprintf(state->cmdline, "-exec-until %d\n", state->stateparam[1]);
          break;
        case STATEPARAM_EXEC_FINISH:
          strcpy(state->cmdline, "-exec-finish\n");
          break;
        }
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        /* after "interrupt" command, still switch to state "running" so that
           "check_stopped()" is called to update the source file and line */
        if ((state->stateparam[0] == STATEPARAM_EXEC_STOP && strncmp(gdbmi_isresult(), "done", 4) == 0)
            || strncmp(gdbmi_isresult(), "running", 7) == 0)
          MOVESTATE(state, STATE_RUNNING);
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_HARDRESET:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        state->monitor_cmd_active = true;
        if (state->tpwr) {
          /* when probe powers the target, do reset by power cycle */
          task_stdin(&state->gdb_task, "monitor tpwr disable\n");  /* will be enabled before re-attach */
        } else {
          char cmd[50];
          if (exist_monitor_cmd("hard_srst", state->monitor_cmds))
            strcpy(cmd, "monitor hard_srst\n");
          else
            strcpy(cmd, "monitor reset\n");  /* "monitor hard_srst" is renamed to "monitor reset" in version 1.9 */
          task_stdin(&state->gdb_task, cmd);
        }
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        MOVESTATE(state, STATE_INIT);  /* regardless of the result, we restart */
        if (state->tpwr)
          set_idle_time(200);   /* make sure power is off for a minimum duration */
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_RUNNING:
      MARKSTATE(state);
      if (check_stopped(&source_execfile, &source_execline, &exec_address)) {
        source_cursorfile = source_execfile;
        if (state->disassemble_mode)
          source_cursorline = line_addr2phys(source_execfile, exec_address);
        else
          source_cursorline = line_source2phys(source_execfile, source_execline);
        MOVESTATE(state, STATE_STOPPED);
        state->refreshflags = 0;
        /* refresh locals & watches only when the respective view is open */
        if (tab_states[TAB_LOCALS] == NK_MAXIMIZED)
          state->refreshflags |= REFRESH_LOCALS;
        if (tab_states[TAB_WATCHES] == NK_MAXIMIZED)
          state->refreshflags |= REFRESH_WATCHES;
        if (tab_states[TAB_REGISTERS] == NK_MAXIMIZED)
          state->refreshflags |= REFRESH_REGISTERS;
        /* only refresh memory if format & count is set, and if memory view is open */
        if (state->memdump.count > 0 && state->memdump.size > 0 && tab_states[TAB_MEMORY] == NK_MAXIMIZED)
          state->refreshflags |= REFRESH_MEMORY;
      }
      break;
    case STATE_STOPPED:
      if (STATESWITCH(state)) {
        state->sourcefiles_index = -1;  /* look up "current" source file again */
        log_console_strings(state);
        gdbmi_sethandled(true);
        MARKSTATE(state);
      }
      if (state->swo.enabled && state->swo.mode != SWOMODE_NONE && !state->swo.init_status) {
        state->monitor_cmd_active = true;   /* to silence output of scripts */
        RESETSTATE(state, STATE_SWOTRACE);
      } else if (state->refreshflags & REFRESH_BREAKPOINTS) {
        RESETSTATE(state, STATE_LIST_BREAKPOINTS);
      } else if (state->refreshflags & REFRESH_LOCALS) {
        RESETSTATE(state, STATE_LIST_LOCALS);
      } else if (state->refreshflags & REFRESH_WATCHES) {
        RESETSTATE(state, STATE_LIST_WATCHES);
      } else if (state->refreshflags & REFRESH_REGISTERS) {
        RESETSTATE(state, STATE_LIST_REGISTERS);
      } else if (state->refreshflags & REFRESH_MEMORY) {
        RESETSTATE(state, STATE_VIEWMEMORY);
      } else if (check_running()) {
        RESETSTATE(state, STATE_RUNNING);
      }
      if (state->warn_source_tstamps) {
        console_add("Sources have more recent date/time stamps than the target\n", STRFLG_ERROR);
        state->warn_source_tstamps = false;   /* do it only once */
      }
      state->ctrl_c_tstamp = 0; /* when execution stops, forget any Ctrl+C press */
      break;
    case STATE_LIST_BREAKPOINTS:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        task_stdin(&state->gdb_task, "-break-list\n");
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        log_console_strings(state);
        const char *ptr = gdbmi_isresult();
        if (state->refreshflags & IGNORE_DOUBLE_DONE && strncmp(ptr, "done", 4) == 0 && ptr[4] != ',') {
          state->refreshflags &= ~IGNORE_DOUBLE_DONE;
          gdbmi_sethandled(false);
        } else if (breakpoint_parse(ptr)) {
          state->refreshflags &= ~(REFRESH_BREAKPOINTS | IGNORE_DOUBLE_DONE);
          MOVESTATE(state, STATE_STOPPED);
          gdbmi_sethandled(true);
        }
      }
      break;
    case STATE_LIST_LOCALS:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        task_stdin(&state->gdb_task, "-stack-list-variables --skip-unavailable --all-values\n");
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        state->refreshflags &= ~REFRESH_LOCALS;
        MOVESTATE(state, STATE_STOPPED);
        locals_update(gdbmi_isresult());
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_LIST_WATCHES:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        task_stdin(&state->gdb_task, "-var-update --all-values *\n");
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        state->refreshflags &= ~REFRESH_WATCHES;
        MOVESTATE(state, STATE_STOPPED);
        watch_update(gdbmi_isresult());
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_LIST_REGISTERS:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        task_stdin(&state->gdb_task, "-data-list-register-values --skip-unavailable x\n");
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        state->refreshflags &= ~REFRESH_REGISTERS;
        MOVESTATE(state, STATE_STOPPED);
        registers_update(gdbmi_isresult());
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_VIEWMEMORY:
      if (!state->atprompt)
        break;
      if (state->memdump.count == 0 && state->memdump.size == 0) {
        MOVESTATE(state, STATE_STOPPED);
        break;
      }
      if (STATESWITCH(state)) {
        assert(state->memdump.expr != NULL && strlen(state->memdump.expr) > 0);
        sprintf(state->cmdline, "-data-read-memory \"%s\" %c %d 1 %d\n",
                state->memdump.expr, state->memdump.fmt, state->memdump.size, state->memdump.count);
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        log_console_strings(state);
        if (memdump_parse(gdbmi_isresult(), &state->memdump)) {
          state->refreshflags &= ~REFRESH_MEMORY;
          MOVESTATE(state, STATE_STOPPED);
          gdbmi_sethandled(true);
        } else {
          gdbmi_sethandled(false);
        }
      }
      break;
    case STATE_BREAK_TOGGLE:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        switch (state->stateparam[0]) {
        case STATEPARAM_BP_ENABLE:
          sprintf(state->cmdline, "-break-enable %d\n", state->stateparam[1]);
          break;
        case STATEPARAM_BP_DISABLE:
          sprintf(state->cmdline, "-break-disable %d\n", state->stateparam[1]);
          break;
        case STATEPARAM_BP_ADD:
          if (source_isvalid(state->stateparam[1]))
            sprintf(state->cmdline, "-break-insert %s:%d\n", source_getname(state->stateparam[1]), state->stateparam[2]);
          else
            sprintf(state->cmdline, "-break-insert %d\n", state->stateparam[2]);
          break;
        case STATEPARAM_BP_DELETE:
          sprintf(state->cmdline, "-break-delete %d\n", state->stateparam[1]);
          break;
        default:
          assert(0);
        }
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        state->refreshflags |= REFRESH_BREAKPOINTS;
        MOVESTATE(state, STATE_STOPPED);
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_WATCH_TOGGLE:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        char regalias[128];
        switch (state->stateparam[0]) {
        case STATEPARAM_WATCH_SET:
          state->watchseq += 1;
          if (svd_xlate_name(state->statesymbol, regalias, sizearray(regalias)) != 0)
            snprintf(state->cmdline, CMD_BUFSIZE, "-var-create watch%u * \"%s\"\n", state->watchseq, regalias);
          else
            snprintf(state->cmdline, CMD_BUFSIZE, "-var-create watch%u * \"%s\"\n", state->watchseq, state->statesymbol);
          break;
        case STATEPARAM_WATCH_DEL:
          snprintf(state->cmdline, CMD_BUFSIZE, "-var-delete watch%d\n", state->stateparam[1]);
          break;
        default:
          assert(0);
        }
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        const char *ptr = gdbmi_isresult();
        int next_state = STATE_STOPPED;
        if (strncmp(ptr, "done", 4) == 0) {
          switch (state->stateparam[0]) {
          case STATEPARAM_WATCH_SET:
            ptr = skipwhite(ptr + 5);
            state->stateparam[0] = watch_add(ptr, state->statesymbol);
            if (state->stateparam[0] != 0 && state->stateparam[1] != FORMAT_NATURAL)
              next_state = STATE_WATCH_FORMAT;
            break;
          case STATEPARAM_WATCH_DEL:
            watch_del(state->stateparam[1]);
            break;
          default:
            assert(0);
          }
          state->refreshflags |= REFRESH_WATCHES;
        }
        MOVESTATE(state, next_state);
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_WATCH_FORMAT:
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        const char *fmt = "natural";
        switch ((char)state->stateparam[1]) {
        case FORMAT_DECIMAL:
          fmt = "decimal";
          break;
        case FORMAT_HEX:
          fmt = "hexadecimal";
          break;
        case FORMAT_OCTAL:
          fmt = "octal";
          break;
        case FORMAT_BINARY:
          fmt = "binary";
          break;
        }
        snprintf(state->cmdline, CMD_BUFSIZE, "-var-set-format watch%u %s\n",
                 (unsigned)state->stateparam[0], fmt);
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        /* GDB reply holds the new format for the watch -> update */
        watch_update_format((unsigned)state->stateparam[0], gdbmi_isresult());
        MOVESTATE(state, STATE_STOPPED);
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_SWOTRACE:
      state->swo.init_status = 1;  /* avoid dual automatic init */
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        ctf_parse_cleanup();
        ctf_decode_cleanup();
        tracestring_clear();
        tracelog_statusclear();
        if (state->swo.mode == SWOMODE_NONE || !state->swo.enabled)
          tracelog_statusmsg(TRACESTATMSG_BMP, "Disabled", -1);
        ctf_error_notify(CTFERR_NONE, 0, NULL);
        if (!state->swo.force_plain
            && ctf_findmetadata(state->ELFfile, state->swo.metadata, sizearray(state->swo.metadata))
            && ctf_parse_init(state->swo.metadata) && ctf_parse_run())
        {
          if (state->dwarf_loaded)
            ctf_set_symtable(&dwarf_symboltable);
          /* stream names overrule configured channel names */
          const CTF_STREAM *stream;
          for (int idx = 0; (stream = stream_by_seqnr(idx)) != NULL; idx++)
            if (stream->name != NULL && strlen(stream->name) > 0)
              channel_setname(idx, stream->name);
        } else {
          ctf_parse_cleanup();
        }
        if (state->swo.mode == SWOMODE_ASYNC)
          sprintf(state->cmdline, "monitor traceswo %u\n", state->swo.bitrate); /* automatically select async mode in the BMP */
        else
          strlcpy(state->cmdline, "monitor traceswo\n", CMD_BUFSIZE);
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
      } else if (gdbmi_isresult() != NULL) {
        /* check the reply of "monitor traceswo" to get the endpoint */
        STRINGLIST *item = stringlist_getlast(&consolestring_root, STRFLG_STATUS, 0);
        assert(item != NULL);
        bmp_parsetracereply(item->text, &state->trace_endpoint);
        /* initial setup (only needs to be done once) */
        if (state->trace_status != TRACESTAT_OK) {
          /* trace_init() does nothing if initialization had already succeeded */
          if (state->probe == state->netprobe)
            state->trace_status = trace_init(BMP_PORT_TRACE, state->IPaddr);
          else
            state->trace_status = trace_init(state->trace_endpoint, NULL);
          if (state->trace_status != TRACESTAT_OK) {
            console_add("Failed to initialize SWO tracing\n", STRFLG_ERROR);
            if (state->probe_type == PROBE_UNKNOWN
                && ((state->probe == state->netprobe && state->swo.mode != SWOMODE_ASYNC) || (state->probe != state->netprobe && state->swo.mode != SWOMODE_MANCHESTER)))
              console_add("Check trace mode (manchester versus async)\n", STRFLG_ERROR);
          } else {
            trace_setdatasize(state->swo.datasize);
          }
        }
        /* GDB may have reset the "mem inaccessible-by-default off" setting,
           so we jump back to the state, after making sure that the state
           that follows this is the one to run the SWO script */
        state->nextstate = STATE_SWODEVICE;
        MOVESTATE(state, STATE_MEMACCESS);
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_SWODEVICE:
      if ((state->swo.mode != SWOMODE_MANCHESTER && state->swo.mode != SWOMODE_ASYNC) || state->swo.clock == 0) {
        MOVESTATE(state, STATE_SWOCHANNELS);
        break;
      }
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        if (bmscript_line_fmt("swo_device", state->cmdline, NULL, 0)) {
          /* run first line from the script */
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
          MARKSTATE(state);
          console_replaceflags = STRFLG_LOG;  /* move LOG to SCRRIPT, to hide script output by default */
          console_xlateflags = STRFLG_SCRIPT;
        } else {
          MOVESTATE(state, STATE_SWOGENERIC);
        }
      } else if (gdbmi_isresult() != NULL) {
        /* run next line from the script (on the end of the script, move to
           the next state) */
        if (bmscript_line_fmt(NULL, state->cmdline, NULL, 0)) {
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
        } else {
          console_replaceflags = console_xlateflags = 0;
          MOVESTATE(state, STATE_SWOGENERIC);
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_SWOGENERIC:
      if ((state->swo.mode != SWOMODE_MANCHESTER && state->swo.mode != SWOMODE_ASYNC) || state->swo.clock == 0) {
        MOVESTATE(state, STATE_SWOCHANNELS);
        break;
      }
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        const DWARF_SYMBOLLIST *symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_BPS", -1, -1);
        assert(state->swo.bitrate > 0 && state->swo.clock > 0);
        unsigned swvclock = (state->swo.mode == SWOMODE_MANCHESTER) ? 2 * state->swo.bitrate : state->swo.bitrate;
        state->scriptparams[0] = (state->swo.mode == SWOMODE_MANCHESTER) ? 1 : 2;
        state->scriptparams[1] = state->swo.clock / swvclock - 1;
        state->scriptparams[2] = state->swo.bitrate;
        state->scriptparams[3] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
        if (bmscript_line_fmt("swo_trace", state->cmdline, state->scriptparams, 4)) {
          /* run first line from the script */
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
          MARKSTATE(state);
          console_replaceflags = STRFLG_LOG;  /* move LOG to SCRIPT, to hide script output by default */
          console_xlateflags = STRFLG_SCRIPT;
        } else {
          MOVESTATE(state, STATE_SWOCHANNELS);
        }
      } else if (gdbmi_isresult() != NULL) {
        /* run next line from the script (on the end of the script, move to
           the next state) */
        if (bmscript_line_fmt(NULL, state->cmdline, state->scriptparams, 4)) {
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
        } else {
          console_replaceflags = console_xlateflags = 0;
          MOVESTATE(state, STATE_SWOCHANNELS);
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_SWOCHANNELS:
      state->swo.init_status = 1;
      if (!state->atprompt)
        break;
      if (STATESWITCH(state)) {
        const DWARF_SYMBOLLIST *symbol = dwarf_sym_from_name(&dwarf_symboltable, "TRACESWO_TER", -1, -1);
        state->scriptparams[0] = 0;
        state->scriptparams[1] = (symbol != NULL) ? (unsigned long)symbol->data_addr : ~0;
        if (state->swo.mode != SWOMODE_NONE && state->swo.enabled) {
          /* if SWO mode is disabled, simply turn all channels off (so skip
             testing whether they should be on) */
          for (int idx = 0; idx < NUM_CHANNELS; idx++)
            if (channel_getenabled(idx))
              state->scriptparams[0] |= (1 << idx);
        }
        if (bmscript_line_fmt("swo_channels", state->cmdline, state->scriptparams, 2)) {
          /* run first line from the script */
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
          MARKSTATE(state);
          console_replaceflags = STRFLG_LOG;  /* move LOG to SCRRIPT, to hide script output by default */
          console_xlateflags = STRFLG_SCRIPT;
        } else {
          MOVESTATE(state, STATE_STOPPED);
        }
      } else if (gdbmi_isresult() != NULL) {
        /* run next line from the script (on the end of the script, move to
           the next state) */
        if (bmscript_line_fmt(NULL, state->cmdline, state->scriptparams, 2)) {
          task_stdin(&state->gdb_task, state->cmdline);
          state->atprompt = false;
        } else {
          console_replaceflags = console_xlateflags = 0;
          bmscript_clearcache();
          MOVESTATE(state, STATE_STOPPED);
        }
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    case STATE_HOVER_SYMBOL:
      if (!state->atprompt)
        break;
      if (strlen(state->statesymbol)== 0 || state->popup_active != POPUP_NONE) {
        state->ttipvalue[0] = '\0';
        MOVESTATE(state, STATE_STOPPED);
        break;
      }
      if (STATESWITCH(state)) {
        char regalias[128];
        gdbmi_sethandled(true);
        if (svd_xlate_name(state->statesymbol, regalias, sizearray(regalias)) != 0)
          snprintf(state->cmdline, CMD_BUFSIZE, "-data-evaluate-expression %s\n", regalias);
        else
          snprintf(state->cmdline, CMD_BUFSIZE, "-data-evaluate-expression %s\n", state->statesymbol);
        task_stdin(&state->gdb_task, state->cmdline);
        state->atprompt = false;
        MARKSTATE(state);
        state->ttipvalue[0] = '\0';
      } else if (gdbmi_isresult() != NULL) {
        const char *head = gdbmi_isresult();
        if (strncmp(head, "done,", 5) == 0) {
          const char *tail;
          size_t len;
          assert(*(head + 4) == ',');
          head = skipwhite(head + 5);
          assert(strncmp(head, "value=", 6) == 0);
          head = skipwhite(head + 6);
          assert(*head == '"');
          tail = skip_string(head, NULL);
          len = tail - head;
          if (len >= sizearray(state->ttipvalue))
            len = sizearray(state->ttipvalue) - 1;
          strncpy(state->ttipvalue, head, len);
          state->ttipvalue[len] = '\0';
          format_string(state->ttipvalue);
          format_value(state->ttipvalue, sizearray(state->ttipvalue));
        }
        MOVESTATE(state, STATE_STOPPED);
        log_console_strings(state);
        gdbmi_sethandled(false);
      }
      break;
    } /* switch (curstate) */
  } /* if (!is_idle()) */

  if (state->curstate > STATE_GDB_TASK && !task_isrunning(&state->gdb_task))
    RESETSTATE(state, STATE_QUIT);   /* GDB ended -> quit the front-end too */
}

int main(int argc, char *argv[])
{
  struct nk_context *ctx;
  APPSTATE appstate;
  SPLITTERBAR splitter_hor, splitter_ver;
  char valstr[128];
  int canvas_width, canvas_height;
  enum nk_collapse_states tab_states[TAB_COUNT];
  char opt_fontstd[64] = "", opt_fontmono[64] = "";
  int exitcode;
  int idx;

# if defined FORTIFY
    Fortify_SetOutputFunc(Fortify_OutputFunc);
# endif

  /* global defaults */
  memset(&appstate, 0, sizeof appstate);
  appstate.target_select = 1;
  appstate.prev_clicked_line = -1;
  appstate.console_activate = 1;
  appstate.trace_status = TRACESTAT_INIT_FAILED;
  appstate.trace_endpoint = BMP_EP_TRACE;
  appstate.popup_active = POPUP_NONE;
  appstate.sourcefiles_index = -1;
  appstate.cmdline = malloc(CMD_BUFSIZE * sizeof(char));
  if (appstate.cmdline == NULL)
    return EXIT_FAILURE;
  /* locate the configuration file for settings */
  char txtConfigFile[_MAX_PATH];
  get_configfile(txtConfigFile, sizearray(txtConfigFile), "bmdebug.ini");

  ini_gets("Settings", "gdb", "", appstate.GDBpath, sizearray(appstate.GDBpath), txtConfigFile);
  ini_gets("Settings", "size", "", valstr, sizearray(valstr), txtConfigFile);
  if (sscanf(valstr, "%d %d", &canvas_width, &canvas_height) != 2 || canvas_width < 100 || canvas_height < 50) {
    canvas_width = WINDOW_WIDTH;
    canvas_height = WINDOW_HEIGHT;
  }
  ini_gets("Settings", "splitter", "", valstr, sizearray(valstr), txtConfigFile);
  splitter_hor.ratio = splitter_ver.ratio = 0.0;
  sscanf(valstr, "%f %f", &splitter_hor.ratio, &splitter_ver.ratio);
  if (splitter_hor.ratio < 0.05f || splitter_hor.ratio > 0.95f)
    splitter_hor.ratio = 0.70f;
  if (splitter_ver.ratio < 0.05f || splitter_ver.ratio > 0.95f)
    splitter_ver.ratio = 0.70f;
  nk_splitter_init(&splitter_hor, (float)canvas_width - 2 * SPACING, SEPARATOR_HOR, splitter_hor.ratio);
  nk_splitter_init(&splitter_ver, (float)canvas_height - 4 * SPACING, SEPARATOR_VER, splitter_ver.ratio);
  config_read_tabstate("configuration", &tab_states[TAB_CONFIGURATION], NULL, NK_MAXIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  config_read_tabstate("breakpoints", &tab_states[TAB_BREAKPOINTS], &appstate.sizerbar_breakpoints, NK_MAXIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  config_read_tabstate("locals", &tab_states[TAB_LOCALS], &appstate.sizerbar_locals, NK_MAXIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  config_read_tabstate("watches", &tab_states[TAB_WATCHES], &appstate.sizerbar_watches, NK_MINIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  config_read_tabstate("registers", &tab_states[TAB_REGISTERS], &appstate.sizerbar_registers, NK_MINIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  config_read_tabstate("memory", &tab_states[TAB_MEMORY], &appstate.sizerbar_memory, NK_MINIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  config_read_tabstate("semihosting", &tab_states[TAB_SEMIHOSTING], &appstate.sizerbar_semihosting, NK_MINIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  config_read_tabstate("serialmon", &tab_states[TAB_SERMON], &appstate.sizerbar_serialmon, NK_MINIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  config_read_tabstate("traceswo", &tab_states[TAB_SWO], &appstate.sizerbar_swo, NK_MINIMIZED, 5 * ROW_HEIGHT, txtConfigFile);
  nk_sizer_init(&appstate.sizerbar_breakpoints, appstate.sizerbar_breakpoints.size, ROW_HEIGHT, SEPARATOR_VER);
  nk_sizer_init(&appstate.sizerbar_locals, appstate.sizerbar_locals.size, ROW_HEIGHT, SEPARATOR_VER);
  nk_sizer_init(&appstate.sizerbar_watches, appstate.sizerbar_watches.size, ROW_HEIGHT, SEPARATOR_VER);
  nk_sizer_init(&appstate.sizerbar_registers, appstate.sizerbar_registers.size, ROW_HEIGHT, SEPARATOR_VER);
  nk_sizer_init(&appstate.sizerbar_memory, appstate.sizerbar_memory.size, ROW_HEIGHT, SEPARATOR_VER);
  nk_sizer_init(&appstate.sizerbar_semihosting, appstate.sizerbar_semihosting.size, ROW_HEIGHT, SEPARATOR_VER);
  nk_sizer_init(&appstate.sizerbar_serialmon, appstate.sizerbar_serialmon.size, ROW_HEIGHT, SEPARATOR_VER);
  nk_sizer_init(&appstate.sizerbar_swo, appstate.sizerbar_swo.size, ROW_HEIGHT, SEPARATOR_VER);
  appstate.allmsg = (int)ini_getl("Settings", "allmessages", 0, txtConfigFile);
  opt_fontsize = ini_getf("Settings", "fontsize", FONT_HEIGHT, txtConfigFile);
  ini_gets("Settings", "fontstd", "", opt_fontstd, sizearray(opt_fontstd), txtConfigFile);
  ini_gets("Settings", "fontmono", "", opt_fontmono, sizearray(opt_fontmono), txtConfigFile);
  /* selected interface */
  appstate.probe = (int)ini_getl("Settings", "probe", 0, txtConfigFile);
  ini_gets("Settings", "ip-address", "127.0.0.1", appstate.IPaddr, sizearray(appstate.IPaddr), txtConfigFile);
  /* read saved recent commands */
  for (idx = 1; ; idx++) {
    char key[32];
    sprintf(key, "cmd%d", idx);
    ini_gets("Commands", key, "", appstate.console_edit, sizearray(appstate.console_edit), txtConfigFile);
    if (strlen(appstate.console_edit) == 0)
      break;
    console_history_add(&appstate.consoleedit_root, appstate.console_edit, true);
  }

  strcpy(appstate.EntryPoint, "main");
  for (idx = 1; idx < argc; idx++) {
    const char *ptr;
    float h;
    if (IS_OPTION(argv[idx])) {
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
          printf("\n");
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
      case 'g':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        strlcpy(appstate.GDBpath, ptr, sizearray(appstate.GDBpath));
        break;
      case 't':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        appstate.target_select = strtol(ptr, NULL, 10);
        if (appstate.target_select < 1)
          appstate.target_select = 1;
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
         on Windows), otherwise access() fails; the path is translated to Unix
         format (as used internally by GDB) */
      if (access(argv[idx], 0) == 0) {
        strlcpy(appstate.ELFfile, argv[idx], sizearray(appstate.ELFfile));
        translate_path(appstate.ELFfile, false);
      }
    }
  }
  if (strlen(appstate.ELFfile) == 0) {
    ini_gets("Session", "recent", "", appstate.ELFfile, sizearray(appstate.ELFfile), txtConfigFile);
    /* filename from the configuration file is stored in the GDB format (convert
       it to native format to test whether it exists) */
    translate_path(appstate.ELFfile, true);
    if (access(appstate.ELFfile, 0) != 0)
      appstate.ELFfile[0] = '\0';
    else
      translate_path(appstate.ELFfile, false); /* convert back to GDB format */
  }
  assert(strchr(appstate.ELFfile, '\\') == 0); /* backslashes should already have been replaced */
  /* if a target filename is known, create the parameter filename from target
     filename and read target-specific options */
  memset(&appstate.swo, 0, sizeof(appstate.swo));
  appstate.swo.mode = SWOMODE_NONE;
  appstate.swo.clock = 48000000;
  appstate.swo.bitrate = 100000;
  appstate.swo.datasize = 1;
  if (strlen(appstate.ELFfile) > 0) {
    strlcpy(appstate.ParamFile, appstate.ELFfile, sizearray(appstate.ParamFile));
    strlcat(appstate.ParamFile, ".bmcfg", sizearray(appstate.ParamFile));
    translate_path(appstate.ParamFile, true);
    load_targetoptions(appstate.ParamFile, &appstate);
  }
  if (appstate.swo.mode == SWOMODE_NONE || !appstate.swo.enabled)
    tracelog_statusmsg(TRACESTATMSG_BMP, "Disabled", -1);

  /* collect debug probes, connect to the selected one */
  appstate.probelist = get_probelist(&appstate.probe, &appstate.netprobe);
  tcpip_init();

  task_init(&appstate.gdb_task);
  memdump_init(&appstate.memdump);
  RESETSTATE(&appstate, STATE_INIT);
  console_hiddenflags = appstate.allmsg ? 0 : STRFLG_NOTICE | STRFLG_RESULT | STRFLG_EXEC | STRFLG_MI_INPUT | STRFLG_TARGET | STRFLG_SCRIPT;
  source_cursorline = 0;
  source_execfile = -1;
  source_execline = 0;
  disasm_init(&appstate.armstate, DISASM_ADDRESS | DISASM_INSTR | DISASM_COMMENT);

  ctx = guidriver_init("BlackMagic Debugger", canvas_width, canvas_height,
                       GUIDRV_RESIZEABLE | GUIDRV_TIMER, opt_fontstd, opt_fontmono, opt_fontsize);
  nuklear_style(ctx);

  while (appstate.curstate != STATE_QUIT) {
    appstate.waitidle = true;
    /* handle state */
    handle_stateaction(&appstate, tab_states);

    /* parse GDB output (stderr first, because the prompt is given in stdout) */
    while (task_stderr(&appstate.gdb_task, appstate.cmdline, CMD_BUFSIZE) > 0) {
      int flag = STRFLG_ERROR;
      /* silence a meaningless error (downgrade it to "notice") */
      if (strstr(appstate.cmdline,"path for the index cache") != NULL)
        flag = STRFLG_NOTICE;
      console_add(appstate.cmdline, flag);
      appstate.waitidle = false;  /* output was added, so not idle */
    }
    while (task_stdout(&appstate.gdb_task, appstate.cmdline, CMD_BUFSIZE) > 0) {
      if (appstate.debugmode)
        printf("IN: %s\n", appstate.cmdline);
      int flags = 0;
      if (appstate.curstate < STATE_START)
        flags |= STRFLG_STARTUP;
      if (appstate.monitor_cmd_active)
        flags |= STRFLG_MON_OUT;  /* so output goes to the main console instead of semihosting view */
      if (appstate.popup_active != POPUP_NONE)
        helptext_add(&helptext_root, appstate.cmdline, appstate.reformat_help);
      if (appstate.popup_active == POPUP_NONE && console_add(appstate.cmdline, flags)) {
        appstate.atprompt = true;
        appstate.console_activate = 1;
        if (appstate.monitor_cmd_active) {
          appstate.monitor_cmd_active = false;
          if (appstate.monitor_cmd_finish) {
            appstate.monitor_cmd_finish = false;
            gdbmi_sethandled(false);
          }
        }
      }
      appstate.waitidle = false;
    }

    /* handle user input */
    nk_input_begin(ctx);
    if (!guidriver_poll(appstate.waitidle)) /* if text was added to the console, don't wait in guidriver_poll(); system is NOT idle */
      break;
    nk_input_end(ctx);

    /* other events */
    int dev_event = guidriver_monitor_usb(0x1d50, 0x6018);
    if (dev_event != 0) {
      clear_probelist(appstate.probelist, appstate.netprobe);
      appstate.probelist = get_probelist(&appstate.probe, &appstate.netprobe);
      appstate.curstate = STATE_INIT; /* BMP was inserted or removed */
    }

    /* GUI */
    guidriver_appsize(&canvas_width, &canvas_height);
    if (nk_begin(ctx, "MainPanel", nk_rect(0, 0, (float)canvas_width, (float)canvas_height), NK_WINDOW_NO_SCROLLBAR)
        && canvas_width > 0 && canvas_height > 0) {
      nk_splitter_resize(&splitter_hor, (float)canvas_width - 2 * SPACING, RESIZE_TOPLEFT);
      nk_splitter_resize(&splitter_ver, (float)canvas_height - 4 * SPACING, RESIZE_TOPLEFT);
      nk_hsplitter_layout(ctx, &splitter_hor, (float)canvas_height - 2 * SPACING);

      ctx->style.window.padding.x = 2;
      ctx->style.window.padding.y = 2;
      ctx->style.window.group_padding.x = 0;
      ctx->style.window.group_padding.y = 0;

      /* left column */
      if (nk_group_begin(ctx, "left", NK_WINDOW_NO_SCROLLBAR)) {

        /* source view + control buttons */
        nk_layout_row_dynamic(ctx, nk_vsplitter_rowheight(&splitter_ver, 0), 1);
        if (nk_group_begin(ctx, "filebrowser", NK_WINDOW_NO_SCROLLBAR|NK_WINDOW_BORDER)) {
          /* button bar */
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 7);
          button_bar(ctx, &appstate, nk_hsplitter_colwidth(&splitter_hor, 0));
          nk_layout_row_end(ctx);

          /* source code view (including handling hovering over symbols and
             breakpoint toggling */
          nk_layout_row_dynamic(ctx, nk_vsplitter_rowheight(&splitter_ver, 0) - 4, 1);
          sourcecode_view(ctx, &appstate);
          nk_group_end(ctx);
        }

        /* vertical splitter */
        nk_vsplitter(ctx, &splitter_ver);

        /* console space (plus handling of internal/enhanced commands) */
        nk_layout_row_dynamic(ctx, nk_vsplitter_rowheight(&splitter_ver, 1), 1);
        console_view(ctx, &appstate, tab_states, nk_vsplitter_rowheight(&splitter_ver, 1));
        nk_group_end(ctx);
      }

      /* column splitter */
      nk_hsplitter(ctx, &splitter_hor);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        panel_configuration(ctx, &appstate, &tab_states[TAB_CONFIGURATION]);
        panel_breakpoints(ctx, &appstate, &tab_states[TAB_BREAKPOINTS]);
        panel_locals(ctx, &appstate, &tab_states[TAB_LOCALS], opt_fontsize);
        panel_watches(ctx, &appstate, &tab_states[TAB_WATCHES], opt_fontsize);
        panel_registers(ctx, &appstate, &tab_states[TAB_REGISTERS], opt_fontsize);
        panel_memory(ctx, &appstate, &tab_states[TAB_MEMORY], opt_fontsize);
        panel_semihosting(ctx, &appstate, &tab_states[TAB_SEMIHOSTING]);
        panel_serialmonitor(ctx, &appstate, &tab_states[TAB_SERMON]);
        panel_traceswo(ctx, &appstate, &tab_states[TAB_SWO]);

        nk_group_end(ctx);
      } /* right column */

      /* keyboard input (for main view) */
      if (appstate.popup_active == POPUP_NONE)
        handle_kbdinput_main(ctx, &appstate);
      else
        help_popup(ctx, &appstate, (float)canvas_width, (float)canvas_height);

      /* mouse cursor shape */
      if (nk_is_popup_open(ctx))
        pointer_setstyle(CURSOR_NORMAL);
      else if (appstate.sizerbar_breakpoints.hover || appstate.sizerbar_locals.hover
               || appstate.sizerbar_watches.hover || appstate.sizerbar_registers.hover
               || appstate.sizerbar_memory.hover || appstate.sizerbar_semihosting.hover
               || appstate.sizerbar_serialmon.hover || appstate.sizerbar_swo.hover)
        pointer_setstyle(CURSOR_UPDOWN);
      else if (splitter_ver.hover)
        pointer_setstyle(CURSOR_UPDOWN);
      else if (splitter_hor.hover)
        pointer_setstyle(CURSOR_LEFTRIGHT);
#if defined __linux__
      else
        pointer_setstyle(CURSOR_NORMAL);
#endif
    } /* window */

    nk_end(ctx);

    /* Draw */
    guidriver_render(COLOUR_BG0_S);
  }
  exitcode = task_close(&appstate.gdb_task);

  /* save parameter file */
  if (strlen(appstate.ELFfile) > 0 && access(appstate.ELFfile, 0) == 0)
    save_targetoptions(appstate.ParamFile, &appstate);

  /* save settings */
  ini_puts("Settings", "gdb", appstate.GDBpath, txtConfigFile);
  sprintf(valstr, "%d %d", canvas_width, canvas_height);
  ini_puts("Settings", "size", valstr, txtConfigFile);
  sprintf(valstr, "%.2f %.2f", splitter_hor.ratio, splitter_ver.ratio);
  ini_puts("Settings", "splitter", valstr, txtConfigFile);
  config_write_tabstate("configuration", tab_states[TAB_CONFIGURATION], NULL, txtConfigFile);
  config_write_tabstate("breakpoints", tab_states[TAB_BREAKPOINTS], &appstate.sizerbar_breakpoints, txtConfigFile);
  config_write_tabstate("locals", tab_states[TAB_LOCALS], &appstate.sizerbar_locals, txtConfigFile);
  config_write_tabstate("watches", tab_states[TAB_WATCHES], &appstate.sizerbar_watches, txtConfigFile);
  config_write_tabstate("registers", tab_states[TAB_REGISTERS], &appstate.sizerbar_registers, txtConfigFile);
  config_write_tabstate("memory", tab_states[TAB_MEMORY], &appstate.sizerbar_memory, txtConfigFile);
  config_write_tabstate("semihosting", tab_states[TAB_SEMIHOSTING], &appstate.sizerbar_semihosting, txtConfigFile);
  config_write_tabstate("serialmon", tab_states[TAB_SERMON], &appstate.sizerbar_serialmon, txtConfigFile);
  config_write_tabstate("traceswo", tab_states[TAB_SWO], &appstate.sizerbar_swo, txtConfigFile);
  ini_putl("Settings", "allmessages", appstate.allmsg, txtConfigFile);
  ini_putf("Settings", "fontsize", opt_fontsize, txtConfigFile);
  ini_puts("Settings", "fontstd", opt_fontstd, txtConfigFile);
  ini_puts("Settings", "fontmono", opt_fontmono, txtConfigFile);
  ini_puts("Session", "recent", appstate.ELFfile, txtConfigFile);
  /* save history of commands */
  ini_puts("Commands", NULL, NULL, txtConfigFile);  /* erase section first */
  idx = 1;
  for (appstate.consoleedit_next = appstate.consoleedit_root.next; appstate.consoleedit_next != NULL; appstate.consoleedit_next = appstate.consoleedit_next->next) {
    char key[32];
    sprintf(key, "cmd%d", idx);
    ini_puts("Commands", key, appstate.consoleedit_next->text, txtConfigFile);
    if (idx++ > 50)
      break;  /* limit the number of memorized commands */
  }
  console_history_match(NULL, NULL, NULL, 0); /* clear history cache */
  /* save selected debug probe */
  if (is_ip_address(appstate.IPaddr))
    ini_puts("Settings", "ip-address", appstate.IPaddr, txtConfigFile);
  ini_putl("Settings", "probe", (appstate.probe == appstate.netprobe) ? 99 : appstate.probe, txtConfigFile);

  free(appstate.cmdline);
  if (appstate.monitor_cmds != NULL)
    free((void*)appstate.monitor_cmds);
  clear_probelist(appstate.probelist, appstate.netprobe);
  guidriver_close();
  stringlist_clear(&consolestring_root);
  stringlist_clear(&appstate.consoleedit_root);
  stringlist_clear(&semihosting_root);
  tracelog_statusclear();
  tracestring_clear();
  breakpoint_clear();
  svd_clear();
  locals_clear();
  memdump_cleanup(&appstate.memdump);
  if (appstate.memdump.expr != NULL)
    free(appstate.memdump.expr);
  console_clear();
  sources_clear(true, appstate.sourcefiles);
  bmscript_clear();
  ctf_parse_cleanup();
  ctf_decode_cleanup();
  dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
  disasm_cleanup(&appstate.armstate);
  tcpip_cleanup();
  sermon_close();
# if defined FORTIFY
    Fortify_CheckAllMemory();
    Fortify_ListAllMemory();
# endif
  return exitcode;
}

