/*
 * GDB front-end with specific support for the Black Magic Probe.
 * This utility is built with Nuklear for a cross-platform GUI.
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

#include "bmp-scan.h"
#include "bmp-script.h"
#include "dwarf.h"
#include "guidriver.h"
#include "noc_file_dialog.h"
#include "nuklear_tooltip.h"
#include "minIni.h"
#include "specialfolder.h"
#include "tcpip.h"

#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"

#if defined __linux__ || defined __unix__
  #include "res/icon_debug_64.h"
#endif

#if !defined _MAX_PATH
  #define _MAX_PATH 260
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  #define stricmp(s1,s2)    strcasecmp((s1),(s2))
  #define strnicmp(s1,s2,n) strncasecmp((s1),(s2),(n))
  #define min(a,b)          ( ((a) < (b)) ? (a) : (b) )
#endif
#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if defined WIN32 || defined _WIN32
  #define DIRSEP_CHAR '\\'
  #define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
  #define DIRSEP_CHAR '/'
  #define IS_OPTION(s)  ((s)[0] == '-')
#endif


static const char *lastdirsep(const char *path);
static char *translate_path(char *path, int native);
static const char *source_getname(unsigned idx);

static DWARF_LINELOOKUP dwarf_linetable = { NULL };
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
#define STRFLG_MON_OUT  0x0200  /* monitor echo */
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


static void semihosting_add(STRINGLIST *root, const char *text, unsigned short flags)
{
  STRINGLIST *item;

  assert(root != NULL);
  /* get the last string ends, for checking whether it ended with a newline */
  for (item = root; item->next != NULL; item = item->next)
    /* nothing */;

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
          item = stringlist_add(root, text, flags);
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
      if (dwarf_linetable.next != NULL && dwarf_filetable.next != NULL
          && (start = strstr(item->text, "*0x")) != NULL)
      {
        unsigned long addr = strtoul(start + 3, (char**)&tail, 16);
        const DWARF_LINELOOKUP *lineinfo = dwarf_line_from_address(&dwarf_linetable, addr);
        if (lineinfo != NULL) {
          const char *path = dwarf_path_from_index(&dwarf_filetable, lineinfo->fileindex);
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
static unsigned console_hiddenflags = 0; /* when a message contains a flag in this set, it is hidden in the console */
static unsigned console_replaceflags = 0;/* when a message contains a flag in this set, it is "translated" to console_xlateflags */
static unsigned console_xlateflags = 0;

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
  if (*flags & console_replaceflags)
    *flags = (*flags & ~console_replaceflags) | console_xlateflags;
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
    if ((curflags & STRFLG_MON_OUT) != 0 && (xtraflags & STRFLG_TARGET) != 0)
      xtraflags = (xtraflags & ~STRFLG_TARGET) | STRFLG_STATUS;
    if ((xtraflags & STRFLG_TARGET) != 0 && (curflags & STRFLG_STARTUP) == 0)
      semihosting_add(&semihosting_root, ptr, curflags | xtraflags);
    /* after gdbmi_leader(), there may again be '\n' characters in the resulting string */
    for (tok = strtok((char*)ptr, "\n"); tok != NULL; tok = strtok(NULL, "\n"))
      stringlist_add(&consolestring_root, tok, curflags | xtraflags);
    console_buffer[0] = '\0';
  }
  curflags = flags;

  head = text;
  while (*head != '\0') {
    size_t len, pos;
    const char *tail = strpbrk(head, "\r\n");
    if (tail == NULL) {
      tail = head + strlen(head);
      addstring = 0;  /* string is not terminated, wait for more characters to come in */
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
      if ((curflags & STRFLG_MON_OUT) != 0 && (xtraflags & STRFLG_TARGET) != 0)
        xtraflags = (xtraflags & ~STRFLG_TARGET) | STRFLG_STATUS;
      prompt = is_gdb_prompt(ptr) && (xtraflags & STRFLG_TARGET) == 0;
      if (prompt) {
        foundprompt = 1;  /* don't add prompt to the output console, but mark that we've seen it */
      } else {
        /* after gdbmi_leader(), there may again be '\n' characters in the resulting string */
        char *tok;
        if ((xtraflags & STRFLG_TARGET) != 0 && (curflags & STRFLG_STARTUP) == 0)
          semihosting_add(&semihosting_root, ptr, curflags | xtraflags);
        for (tok = strtok((char*)ptr, "\n"); tok != NULL; tok = strtok(NULL, "\n")) {
          /* avoid adding a "log" string when the same string is at the tail of
             the list */
          if (xtraflags & STRFLG_LOG) {
            STRINGLIST *last = stringlist_getlast(&consolestring_root, 0, 0);
            if (strcmp(last->text, tok) == 0)
              continue;
          }
          stringlist_add(&consolestring_root, tok, flags | xtraflags);
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

static int console_autocomplete(char *text, size_t textsize, const DWARF_SYMBOLLIST *symboltable)
{
  typedef struct tagGDBCOMMAND {
    const char *command;
    const char *shorthand;
    const char *parameters;
  } GDBCOMMAND;
  static const GDBCOMMAND commands[] = {
    { "attach", NULL, NULL },
    { "backtrace", "bt", NULL },
    { "break", "b", "%func %file" },
    { "clear", NULL, "%func %file" },
    { "command", NULL, NULL },
    { "cond", NULL, NULL },
    { "continue", "c", NULL },
    { "delete", NULL, NULL },
    { "disable", NULL, NULL },
    { "display", NULL, "%func" },
    { "down", NULL, NULL },
    { "enable", NULL, NULL },
    { "file", NULL, NULL },
    { "find", NULL, NULL },
    { "finish", "fin", NULL },
    { "frame", NULL, NULL },
    { "info", NULL, "args locals sources" },
    { "list", NULL, "%func %var %file" },
    { "load", NULL, NULL },
    { "monitor", NULL, "connect_srst hard_srst jtag_scan morse option swdp_scan targets tpwr traceswo vector_catch" },
    { "next", "n", NULL },
    { "print", "p", "%var" },
    { "ptype", NULL, "%var" },
    { "quit", NULL, NULL },
    { "run", NULL, NULL },
    { "set", NULL, "%var" },
    { "start", NULL, NULL },
    { "step", "s", NULL },
    { "target", NULL, "extended-remote remote" },
    { "tbreak", NULL, "%func %file" },
    { "trace", NULL, "async auto channel disable enable info passive" },
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
    assert(cache_cutoff >= 0 && cache_cutoff < textsize);
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
  if (strlen(word) == 0)
    return 0; /* no start of a word; nothing to autocomplete */

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
          const char *first = NULL;
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
              for (idx = 0; !result && (sym = dwarf_sym_from_index(symboltable, idx)) != 0; idx++) {
                assert(sym->name != NULL);
                if (strncmp(word, sym->name, len)== 0) {
                  if ((strcmp(ptr, "%var") == 0 && sym->address == 0) || (strcmp(ptr, "%func") == 0 && sym->address != 0)) {
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
            strlcpy(word, first, textsize - (word - text));
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


typedef struct tagSOURCEFILE {
  char *basename;
  char *path;
  STRINGLIST root; /* base of text lines */
} SOURCEFILE;

static SOURCEFILE *sources = NULL;
static unsigned sources_size = 0;   /* size of the sources array (max. files that the array can contain) */
static unsigned sources_count = 0;  /* number of entries in the sources array */

/** sources_add() adds a source file unless it already exists in the list.
 *  \param filename   The base name of the source file (including extension but
 *                    excluding the path).
 *  \param filepath   The (full or partial) path to the file. This parameter
 *                    may be NULL (in which case the path is assumed the same
 *                    as the base name).
 */
static void sources_add(const char *filename, const char *filepath)
{
  FILE *fp;

  assert((sources == NULL && sources_size == 0) || (sources != NULL && sources_size > 0));
  assert(filename != NULL);

  /* check whether the source already exists */
  if (sources != NULL) {
    unsigned idx;
    for (idx = 0; idx < sources_count; idx++) {
      if (strcmp(sources[idx].basename, filename)== 0) {
        const char *p1 = (sources[idx].path != NULL) ? sources[idx].path : "";
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
    sources = (SOURCEFILE*)malloc(sources_size * sizeof(SOURCEFILE));
  } else if (sources_count >= sources_size) {
    sources_size *= 2;
    sources = (SOURCEFILE*)realloc(sources, sources_size * sizeof(SOURCEFILE));
  }
  if (sources == NULL) {
    fprintf(stderr, "Memory allocation error.\n");
    exit(1);
  }
  assert(filename != NULL && strlen(filename) > 0);
  sources[sources_count].basename = strdup(filename);
  sources[sources_count].path = (filepath != NULL && strlen(filepath) > 0) ? strdup(filepath) : NULL;
  memset(&sources[sources_count].root, 0, sizeof(STRINGLIST));

  /* load the source file */
  fp = fopen(sources[sources_count].path, "rt");
  if (fp != NULL) {
    char line[256]; /* source code really should not have lines as long as this */
    while (fgets(line, sizearray(line), fp) != NULL) {
      char *ptr = strchr(line, '\n');
      if (ptr != NULL)
        *ptr = '\0';
      stringlist_add(&sources[sources_count].root, line, 0);
    }
    fclose(fp);
  }

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
    assert(sources == NULL);
    return;
  }

  for (idx = 0; idx < sources_count; idx++) {
    assert(sources[idx].basename != NULL);
    free((void*)sources[idx].basename);
    sources[idx].basename = NULL;
    if (sources[idx].path != NULL) {
      free((void*)sources[idx].path);
      sources[idx].path = NULL;
    }
    stringlist_clear(&sources[idx].root);
    memset(&sources[idx].root, 0, sizeof(STRINGLIST));
  }
  sources_count = 0;

  if (freelists) {
    assert(sources != NULL);
    free((void*)sources);
    sources = NULL;
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
    char name[_MAX_PATH] = "", path[_MAX_PATH] = "";
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
      const char *fname = (sources[idx].path != NULL) ? sources[idx].path : sources[idx].basename;
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

  if (sources == NULL)
    return -1;

  if ((base = strrchr(filename, '/')) != NULL)
    filename = base + 1;
  #if defined _WIN32
    if ((base = strrchr(filename, '\\')) != NULL)
      filename = base + 1;
  #endif

  for (idx = 0; idx < sources_count; idx++)
    if (strcmp(filename, sources[idx].basename)== 0)
      return idx;

  return -1;
}

static const char *source_getname(unsigned idx)
{
  if (idx < sources_count)
    return sources[idx].basename;
  return NULL;
}

static const char **sources_getnames(void)
{
  const char **namelist;

  if (sources_count == 0)
    return NULL;
  assert(sources != NULL);
  namelist = (const char**)malloc(sources_count * sizeof(char*));
  if (namelist != NULL) {
    int idx;
    for (idx = 0; idx < sources_count; idx++)
      namelist[idx] = source_getname(idx);
  }

  return namelist;
}

static int source_linecount(int srcindex)
{
  int count = 0;
  STRINGLIST *item;
  if (srcindex >= 0 && srcindex < sources_count)
    for (item = sources[srcindex].root.next; item != NULL; item = item->next)
      count++;
  return count;
}

static STRINGLIST *source_firstline(int srcindex)
{
  return (srcindex >= 0 && srcindex < sources_count) ? sources[srcindex].root.next : NULL;
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
          char filename[_MAX_PATH];
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
  char basename[_MAX_PATH], path[_MAX_PATH], *ptr;
  unsigned len, idx;

  assert(target != NULL);
  assert(metadata != NULL);

  /* if no metadata filename was set, create the base name (without path) from
     the target name; add a .tsdl extension */
  if (strlen(metadata) == 0) {
    ptr = (char*)lastdirsep(target);
    strlcpy(basename, (ptr == NULL) ? target : ptr + 1, sizearray(basename));
    if ((ptr = strrchr(basename, '.')) != NULL)
      *ptr = '\0';
    strlcat(basename, ".tsdl", sizearray(basename));
  } else {
    if ((ptr = (char*)lastdirsep(metadata)) == NULL)
      ptr = metadata;
    if (strchr(ptr, '.') != NULL) {
      /* there is a filename in the metadata parameter already */
      strlcpy(basename, metadata, sizearray(basename));
    } else {
      /* there is only a path in the metadata parameter */
      strlcpy(basename, metadata, sizearray(basename));
      if (ptr == NULL || *(ptr + 1) != '\0') {
        #if defined _WIN32
          strlcat(basename, "\\", sizearray(basename));
        #else
          strlcat(basename, "/", sizearray(basename));
        #endif
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
    ptr =(char*)lastdirsep(sources[idx].path);
    if (ptr != NULL) {
      len = min(ptr - sources[idx].path, sizearray(path) - 2);
      strncpy(path, sources[idx].path, len);
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
          /* look up the file */
          assert(sources != NULL);
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

/** translate_path()
 *  \param path     The path name
 *  \param native   Set to 1 to convert slashes to backslashes (required by C
 *                  library); set to 0 to convert backslashes to slashes
 *                  (required by GDB)
 *  \return A pointer to the translated name (parameter "path").
 */
static char *translate_path(char *path, int native)
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

static char *translate_path(char *path, int native)
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
#define ROW_HEIGHT      (1.6 * opt_fontsize)
#define COMBOROW_CY     (0.9 * opt_fontsize)
#define BUTTON_WIDTH    (3 * opt_fontsize)
#define BROWSEBTN_WIDTH (1.85 * opt_fontsize)


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
  if (nk_group_begin(ctx, id, NK_WINDOW_BORDER)) {
    int lines = 0, maxlen = 0;
    float maxwidth = 0;
    for (item = source_firstline(source_cursorfile); item != NULL; item = item->next) {
      float textwidth;
      BREAKPOINT *bkpt;
      lines++;
      assert(item->text != NULL);
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
      assert(font != NULL && font->width != NULL);
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
        } else if (source_cursorline >= topline + source_vp_rows - 1 && lines > 3) {
          topline = source_cursorline - source_vp_rows + 1;
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
  const char *head, *tail, *ptr;
  int len, nest;

  assert(symname != NULL && symlen > 0);
  *symname = '\0';
  if (row < 1 || col < 1)
    return 0;
  for (item = source_firstline(source_cursorfile); item != NULL && row > 1; item = item->next)
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
  /* run from the start of the line to the head, check for preprocessor directives,
     comments and literal strings (it does not work well for multi-line comments
     and it also does not take continued lines into account) */
  ptr = skipwhite(item->text);
  if (*ptr == '#')
    return 0;     /* symbol is on a preprocessor directive */
  while (ptr < head) {
    assert(*ptr != '\0');
    if (*ptr == '/' && *(ptr + 1) == '/')
      return 0;   /* symbol is in a single-line comment */
    if (*ptr == '/' && *(ptr + 1) == '*') {
      ptr += 2;
      while (*ptr != '\0' && (*ptr != '*' || *(ptr + 1) != '/')) {
        if (ptr >= head)
          return 0; /* symbol is in ablock comment */
        ptr += 1;
      }
      ptr += 1; /* we stopped on the '*', move to the '/' which is the end of the comment */
    } else if (*ptr == '\'' || *ptr == '"') {
      char quote = *ptr++;
      while (*ptr != '\0' && *ptr != quote) {
        if (ptr >= head)
          return 0; /* symbol is in a literal character or string */
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
  if (is_keyword(symname))
    return 0; /* reserved words are not symbols */
  return 1;
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
  STATE_TARGET_EXT,
  STATE_MON_TPWR,
  STATE_MON_SCAN,
  STATE_ASYNC_MODE,
  STATE_ATTACH,
  STATE_FILE,
  STATE_FILE_TEST,
  STATE_MEMACCESS_1,
  STATE_MEMACCESS_2,
  STATE_VERIFY,
  STATE_DOWNLOAD,
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

typedef struct tagSWOSETTINGS {
  unsigned mode;
  unsigned bitrate;
  unsigned clock;
  unsigned datasize;
  char metadata[_MAX_PATH];
  int force_plain;
} SWOSETTINGS;

enum { SWOMODE_NONE, SWOMODE_MANCHESTER, SWOMODE_ASYNC };

static int save_settings(const char *filename, const char *entrypoint,
                         int tpwr, int autodownload, const SWOSETTINGS *swo)
{
  int idx;

  if (filename == NULL || strlen(filename) == 0)
    return 0;

  assert(entrypoint != NULL);
  ini_puts("Target", "entrypoint", entrypoint, filename);

  ini_putl("Flash", "tpwr", tpwr, filename);
  ini_putl("Flash", "auto-download", autodownload, filename);

  ini_putl("SWO trace", "mode", swo->mode, filename);
  ini_putl("SWO trace", "bitrate", swo->bitrate, filename);
  ini_putl("SWO trace", "clock", swo->clock, filename);
  ini_putl("SWO trace", "datasize", swo->datasize * 8, filename);
  ini_puts("SWO trace", "ctf", swo->metadata, filename);
  for (idx = 0; idx < NUM_CHANNELS; idx++) {
    char key[32], value[128];
    struct nk_color color = channel_getcolor(idx);
    sprintf(key, "chan%d", idx);
    sprintf(value, "%d #%06x %s", channel_getenabled(idx),
            ((int)color.r << 16) | ((int)color.g << 8) | color.b,
            channel_getname(idx, NULL, 0));
    ini_puts("SWO trace", key, value, filename);
  }

  return access(filename, 0) == 0;
}

static int load_settings(const char *filename, char *entrypoint, size_t entrypoint_sz,
                         int *tpwr, int *autodownload, SWOSETTINGS *swo)
{
  int idx;

  if (filename == NULL || strlen(filename) == 0 || access(filename, 0) != 0)
    return 0;

  assert(entrypoint != NULL);
  ini_gets("Target", "entrypoint", "main", entrypoint, entrypoint_sz, filename);

  assert(tpwr != NULL);
  *tpwr =(int)ini_getl("Flash", "tpwr", 0, filename);
  assert(autodownload != NULL);
  *autodownload = (int)ini_getl("Flash", "auto-download", 1, filename);

  swo->mode = (unsigned)ini_getl("SWO trace", "mode", SWOMODE_NONE, filename);
  swo->bitrate = (unsigned)ini_getl("SWO trace", "bitrate", 100000, filename);
  swo->clock = (unsigned)ini_getl("SWO trace", "clock", 48000000, filename);
  swo->datasize = (unsigned)ini_getl("SWO trace", "datasize", 8, filename) / 8;
  swo->force_plain = 0;
  ini_gets("SWO trace", "ctf", "", swo->metadata, sizearray(swo->metadata), filename);
  for (idx = 0; idx < NUM_CHANNELS; idx++) {
    char key[32], value[128];
    unsigned clr;
    int enabled, result;
    channel_set(idx, (idx == 0), NULL, nk_rgb(190, 190, 190)); /* preset: port 0 is enabled by default, others disabled by default */
    sprintf(key, "chan%d", idx);
    ini_gets("SWO trace", key, "", value, sizearray(value), filename);
    result = sscanf(value, "%d #%x %s", &enabled, &clr, key);
    if (result >= 2)
      channel_set(idx, enabled, (result >= 3) ? key : NULL, nk_rgb(clr >> 16,(clr >> 8) & 0xff, clr & 0xff));
  }

  return 1;
}

static int handle_help_cmd(const char *command)
{
  command = skipwhite(command);
  if (strncmp(command, "help", 4) == 0 && TERM_END(command, 4)) {
    /* always add a extra line as a separator, to make the help stand out more */
    console_add(command, STRFLG_INPUT);
    command = skipwhite(command + 4);
    if (strncmp(command, "find", 4) == 0 && TERM_END(command, 4)) {
      console_add("Find text in the current source file (case-insensitive).\n", 0);
      console_add("find [text]\n", 0);
      console_add("Without parameter, the find command repeats the previous search.\n", 0);
      return 1;
    } else if (strncmp(command, "trace", 5) == 0 && TERM_END(command, 5)) {
      console_add("Configure SWO tracing.\n", 0);
      console_add("trace [target-clock] [bitrate]   - configure Manchester tracing.\n", 0);
      console_add("trace passive   - activate Manchester tracing, without configuration.\n", 0);
      console_add("trace async [target-clock] [bitrate]   - configure asynchronous tracing.\n", 0);
      console_add("trace async passive [bitrate]   - activate asynchronous tracing, without configuration.\n", 0);
      console_add("trace enable   - enable SWO tracing with previously configured settings.\n", 0);
      console_add("trace disable   - disable SWO tracing.\n", 0);
      console_add("trace [filename]   - configure CTF decoding using the given TSDL file.\n", 0);
      console_add("trace plain   - disable CTF decoding, trace plain input data.\n", 0);
      console_add("trace channel [index] enable   - enable a channel (0..31).\n", 0);
      console_add("trace channel [index] disable   - disable a channel (0..31).\n", 0);
      console_add("trace channel [index] [name]   - set the name of a channel.\n", 0);
      console_add("trace channel [index] #[colour]   - set the colour of a channel.\n", 0);
      console_add("trace info   - show current status and configuration.\n", 0);
      return 1;
    }
  }
  return 0;
}

static int handle_list_cmd(const char *command, const DWARF_SYMBOLLIST *symboltable,
                           const DWARF_PATHLIST *filetable)
{
  command = skipwhite(command);
  if (strncmp(command, "list", 4) == 0 && TERM_END(command, 4)) {
    const char *p1 = skipwhite(command + 4);
    if (*p1 == '+' || *p1 == '\0') {
      int linecount = source_linecount(source_cursorfile);
      source_cursorline += source_vp_rows;      /* "list" & "list +" */
      if (source_cursorline > linecount)
        source_cursorline = linecount;
      return 1;
    } else if (*p1 == '-') {
      source_cursorline -= source_vp_rows;      /* "list -" */
      if (source_cursorline < 1)
        source_cursorline = 1;
      return 1;
    } else if (isdigit(*p1)) {
      int line = (int)strtol(p1, NULL, 10);     /* "list #" (where # is a line number) */
      if (line >= 1 && line <= source_linecount(source_cursorfile)) {
        source_cursorline = line;
        return 1;
      }
    } else {
      const DWARF_SYMBOLLIST *sym;              /* "list filename", "list filename:#" or "list function" */
      unsigned line = 0, idx = UINT_MAX;
      sym = dwarf_sym_from_name(symboltable, p1);
      if (sym != NULL) {
        const char *path = dwarf_path_from_index(filetable, sym->fileindex);
        if (path != NULL)
          idx = source_lookup(path);
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
          idx = source_lookup(p1);
        } else {
          /* no extension, ignore extension on match */
          unsigned len = strlen(p1);
          for (idx = 0; idx < sources_count; idx++) {
            const char *basename = source_getname(idx);
            assert(basename != NULL);
            if (strncmp(basename, p1, len) == 0 && basename[len] == '.')
              break;
          }
        }
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

static int handle_file_cmd(const char *command, char *filename, size_t namelength)
{
  assert(command != NULL);
  assert(filename != NULL && namelength > 0);
  command = skipwhite(command);
  if (strncmp(command, "file ", 5) == 0) {
    const char *ptr = strchr(command, ' ');
    assert(ptr != NULL);
    strlcpy(filename, skipwhite(ptr), namelength);
    translate_path(filename, 1);
    return 1;
  }
  return 0;
}

static int handle_find_cmd(const char *command)
{
  static char pattern[50] = "";

  assert(command != NULL);
  command = skipwhite(command);
  if (strncmp(command, "find", 4) == 0 && TERM_END(command, 4)) {
    STRINGLIST *item = source_firstline(source_cursorfile);
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
      item = source_firstline(source_cursorfile);
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
        item = source_firstline(source_cursorfile);
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

static int is_monitor_cmd(const char *command)
{
  assert(command != NULL);
  if (strncmp(command, "mon", 3) == 0 && TERM_END(command, 3))
    return 1;
  if (strncmp(command, "monitor", 7) == 0 && TERM_END(command, 7))
    return 1;
  return 0;
}

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

static void trace_info_mode(const SWOSETTINGS *swo)
{
  char msg[200];

  strlcpy(msg, "Active configuration: ", sizearray(msg));
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

  strlcat(msg, ", data width = ", sizearray(msg));
  if (swo->datasize == 0)
    strlcat(msg, "auto", sizearray(msg));
  else
    sprintf(msg + strlen(msg), "%u-bit", swo->datasize * 8);

  strlcat(msg, "\n", sizearray(msg));
  console_add(msg, STRFLG_STATUS);

  assert(swo->metadata != NULL);
  if (strlen(swo->metadata) > 0) {
    const char *basename = lastdirsep(swo->metadata);
    sprintf(msg, "CTF / TSDL = %s\n", (basename != NULL) ? basename + 1 : swo->metadata);
    console_add(msg, STRFLG_STATUS);
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
  if (strncmp(command, "trace", 5) != 0 || !TERM_END(command, 5))
    return 0;

  /* make a copy of the command string, to be able to clear parts of it */
  cmdcopy = alloca(strlen(command) + 1);
  strcpy(cmdcopy, command);
  ptr = (char*)skipwhite(cmdcopy + 5);

  if (*ptr == '\0' || (strncmp(ptr, "info", 4) == 0 && TERM_END(ptr, 4)))
    return 3; /* if only "trace" is typed, interpret it as "trace info" */

  assert(swo != NULL);

  if (strncmp(ptr, "channel ", 7) == 0 || strncmp(ptr, "chan ", 5) == 0 || strncmp(ptr, "ch ", 3) == 0) {
    int chan;
    char *opts;
    ptr = strchr(ptr, ' ');
    assert(ptr != NULL);
    chan = (int)strtol(skipwhite(ptr), (char**)&ptr, 10);
    ptr = (char*)skipwhite(ptr);
    opts = alloca(strlen(ptr) + 1);
    strcpy(opts, ptr);
    for (ptr = strtok(opts, " "); ptr != NULL; ptr = strtok(NULL, " ")) {
      if (stricmp(ptr, "enable") == 0) {
        channel_setenabled(chan, 1);
      } else if (stricmp(ptr, "disable") == 0) {
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

  /* optionally clear a TSDL file */
  ptr = (char*)skipwhite(cmdcopy + 6);  /* reset to start */
  if (strncmp(ptr, "plain", 5) == 0 && TERM_END(ptr, 5)) {
    swo->metadata[0] = '\0';
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
  if (strncmp(ptr, "disable", 7) == 0 && TERM_END(ptr, 7)) {
    swo->mode = SWOMODE_NONE;
    return 2; /* special mode, disable all channels when turning tracing off */
  }
  if ((strncmp(ptr, "enable", 6) == 0 && TERM_END(ptr, 6))) {
    ptr = (char*)skipwhite(ptr + 6);
    newmode = (swo->mode == SWOMODE_NONE) ? SWOMODE_MANCHESTER : swo->mode;
  }
  if (strncmp(ptr, "async", 5) == 0 && TERM_END(ptr, 5)) {
    newmode = SWOMODE_ASYNC;
    ptr = (char*)skipwhite(ptr + 5);
  }
  /* clock */
  if (isdigit(*ptr)) {
    double v = strtod(ptr, (char**)&ptr);
    if ((strnicmp(ptr, "mhz", 3) == 0 && TERM_END(ptr, 3)) || (strnicmp(ptr, "m", 1) == 0 && TERM_END(ptr, 1))) {
      v *= 1000000;
      ptr = strchr(ptr, ' ');
      ptr = (ptr != NULL) ? (char*)skipwhite(ptr) : strchr(cmdcopy, '\0');
    }
    swo->clock = (unsigned)(v + 0.5);
    if (swo->mode != SWOMODE_ASYNC)
      newmode = SWOMODE_MANCHESTER; /* if clock is set, mode must be async or manchester */
  }
  /* bitrate */
  if (isdigit(*ptr)) {
    double v = strtod(ptr, (char**)&ptr);
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
  if (newmode != SWOMODE_NONE && swo->clock > 0 && swo->bitrate > swo->clock) {
    /* if bitrate > clock swap the two (this can never happen, so it is likely
       an error) */
    unsigned t = swo->bitrate;
    swo->bitrate = swo->clock;
    swo->clock = t;
  }
  if (newmode != SWOMODE_NONE)
    swo->mode = newmode;

  trace_info_mode(swo);
  return 1; /* assume entire protocol changed */
}

static void usage(void)
{
  printf("bmdebug - GDB front-end for the Black Magic Probe.\n\n"
         "Usage: bmdebug [options] elf-file\n\n"
         "Options:\n"
         "-f=value\t Font size to use (value must be 8 or larger).\n"
         "-g=path\t path to the GDB executable to use.\n"
         "-h\t This help.\n");
}

int main(int argc, char *argv[])
{
  enum { TAB_CONFIGURATION, TAB_BREAKPOINTS, TAB_WATCHES, TAB_SEMIHOSTING, TAB_SWO, /* --- */ TAB_COUNT };
  enum { SPLITTER_NONE, SPLITTER_VERTICAL, SPLITTER_HORIZONTAL, SIZER_SEMIHOSTING, SIZER_SWO };

  struct nk_context *ctx;
  char txtFilename[_MAX_PATH], txtConfigFile[_MAX_PATH], txtGDBpath[_MAX_PATH],
       txtParamFile[_MAX_PATH];
  char txtEntryPoint[64];
  char port_gdb[64], mcu_family[64], mcu_architecture[64];
  char txtIPaddr[64] = "";
  int probe, usbprobes, netprobe;
  const char **probelist;
  char valstr[128];
  int canvas_width, canvas_height;
  enum nk_collapse_states tab_states[TAB_COUNT];
  float tab_heights[TAB_COUNT];
  int opt_tpwr = nk_false;
  int opt_allmsg = nk_false;
  int opt_autodownload = nk_true;
  int opt_fontsize = FONT_HEIGHT;
  char opt_fontstd[64] = "", opt_fontmono[64] = "";
  SWOSETTINGS opt_swo;
  float splitter_hor = 0.75, splitter_ver = 0.75;
  char console_edit[128] = "", watch_edit[128] = "";
  STRINGLIST consoleedit_root = { NULL, NULL, 0 }, *consoleedit_next;
  TASK task;
  char cmd[300], statesymbol[64], ttipvalue[256];
  int curstate, prevstate, nextstate, stateparam[3];
  int refreshflags, trace_status, warn_source_tstamps;
  int atprompt, insplitter, console_activate, cont_is_run, exitcode, monitor_cmd_active;
  int idx, result, highlight;
  unsigned char trace_endpoint = BMP_EP_TRACE;
  unsigned semihosting_lines, swo_lines;
  int prev_clicked_line;
  unsigned watchseq;
  unsigned long scriptparams[3];
  const char **sourcefiles = NULL;

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
    char key[32];
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
  opt_allmsg = (int)ini_getl("Settings", "allmessages", 0, txtConfigFile);
  opt_fontsize = (int)ini_getl("Settings", "fontsize", FONT_HEIGHT, txtConfigFile);
  ini_gets("Settings", "fontstd", "", opt_fontstd, sizearray(opt_fontstd), txtConfigFile);
  ini_gets("Settings", "fontmono", "", opt_fontmono, sizearray(opt_fontmono), txtConfigFile);
  /* selected interface */
  probe = (int)ini_getl("Settings", "probe", 0, txtConfigFile);
  ini_gets("Settings", "ip-address", "127.0.0.1", txtIPaddr, sizearray(txtIPaddr), txtConfigFile);
  /* read saved recent commands */
  for (idx = 1; ; idx++) {
    char key[32];
    sprintf(key, "cmd%d", idx);
    ini_gets("Commands", key, "", console_edit, sizearray(console_edit), txtConfigFile);
    if (strlen(console_edit) == 0)
      break;
    stringlist_add(&consoleedit_root, console_edit, 0);
  }

  txtFilename[0] = '\0';
  txtParamFile[0] = '\0';
  strcpy(txtEntryPoint, "main");
  for (idx = 1; idx < argc; idx++) {
    const char *ptr;
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
      case 'g':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        strlcpy(txtGDBpath, ptr, sizearray(txtGDBpath));
        break;
      default:
        fprintf(stderr, "Unknown option %s; use option -h for help.\n", argv[idx]);
        return 1;
      }
    } else {
      /* filename on the command line must be in native format (using backslashes
         on Windows) */
      if (access(argv[idx], 0) == 0) {
        strlcpy(txtFilename, argv[idx], sizearray(txtFilename));
        translate_path(txtFilename, 0);
      }
    }
  }
  if (strlen(txtFilename) == 0) {
    ini_gets("Session", "recent", "", txtFilename, sizearray(txtFilename), txtConfigFile);
    /* filename from the configuration file is stored in the GDB format (convert
       it to native format to test whether it exists) */
    translate_path(txtFilename, 1);
    if (access(txtFilename, 0) != 0)
      txtFilename[0] = '\0';
    else
      translate_path(txtFilename, 0); /* convert back to GDB format */
  }
  assert(strchr(txtFilename, '\\') == 0); /* backslashes should already have been replaced */
  /* if a target filename is known, create the parameter filename from target
     filename and read target-specific options */
  memset(&opt_swo, 0, sizeof(opt_swo));
  opt_swo.mode = SWOMODE_NONE;
  opt_swo.clock = 48000000;
  opt_swo.bitrate = 100000;
  opt_swo.datasize = 1;
  if (strlen(txtFilename) > 0) {
    strlcpy(txtParamFile, txtFilename, sizearray(txtParamFile));
    strlcat(txtParamFile, ".bmcfg", sizearray(txtParamFile));
    translate_path(txtParamFile, 1);
    load_settings(txtParamFile, txtEntryPoint, sizearray(txtEntryPoint), &opt_tpwr, &opt_autodownload, &opt_swo);
  }

  /* collect debug probes, connect to the selected one */
  usbprobes = get_bmp_count();
  netprobe = (usbprobes > 0) ? usbprobes : 1;
  probelist = malloc((netprobe+1)*sizeof(char*));
  if (probelist != NULL) {
    char portname[64];
    if (usbprobes == 0) {
      probelist[0] = strdup("-");
    } else {
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
  tcpip_init();

  memset(&task, 0, sizeof task);
  insplitter = SPLITTER_NONE;
  curstate = STATE_INIT;
  prevstate = nextstate = -1;
  refreshflags = 0;
  console_hiddenflags = opt_allmsg ? 0 : STRFLG_NOTICE | STRFLG_RESULT | STRFLG_EXEC | STRFLG_MI_INPUT | STRFLG_TARGET | STRFLG_SCRIPT;
  atprompt = 0;
  console_activate = 1;
  consoleedit_next = NULL;
  cont_is_run = 0;
  monitor_cmd_active = 0;
  warn_source_tstamps = 0;
  source_cursorline = 0;
  source_execfile = -1;
  source_execline = 0;
  prev_clicked_line = -1;
  semihosting_lines = 0;
  swo_lines = 0;
  watchseq = 0;
  trace_status = TRACESTAT_INIT_FAILED;

  ctx = guidriver_init("BlackMagic Debugger", canvas_width, canvas_height,
                       GUIDRV_RESIZEABLE | GUIDRV_TIMER, opt_fontstd, opt_fontmono, opt_fontsize);
  set_style(ctx);

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
        port_gdb[0] = '\0';
        if (probe == netprobe) {
          if (is_ip_address(txtIPaddr)) {
            char portnr[20];
            sprintf(portnr, ":%d", BMP_PORT_GDB);
            strlcpy(port_gdb, txtIPaddr, sizearray(port_gdb));
            strlcat(port_gdb, portnr, sizearray(port_gdb));
          }
        } else {
          const char *ptr;
          assert(probe < usbprobes || (probe == 0 && usbprobes == 0));
          ptr = probelist[probe];
          if (*ptr != '\0' && *ptr != '-') {
            if (strncmp(ptr, "COM", 3)== 0 && strlen(ptr)>= 5) {
              /* special case for Microsoft Windows, for COM ports >= 10 */
              strlcpy(port_gdb, "\\\\.\\", sizearray(port_gdb));
            }
            strlcat(port_gdb, ptr, sizearray(port_gdb));
          }
        }
        if (port_gdb[0] != '\0') {
          curstate = STATE_TARGET_EXT;
        } else if (atprompt) {
          if (prevstate != curstate) {
            if (probe == netprobe)
              console_add("ctxLink Probe not found, invalid IP address\n", STRFLG_ERROR);
            else
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
            sprintf(cmd, "Port %s busy or unavailable\n", port_gdb);
            console_add(cmd, STRFLG_ERROR);
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
          mcu_family[0] = '\0';
          mcu_architecture[0] = '\0';
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
            /* save architecture */
            const char *ptr;
            STRINGLIST *item = stringlist_getlast(&consolestring_root, 0, STRFLG_RESULT);
            assert(item != NULL && item->text != NULL);
            ptr = item->text;
            while (*ptr <= ' ' && *ptr != '\0')
              ptr++;
            /* expect: 1 STM32F10x medium density [M3/M4] */
            if (isdigit(*ptr)) {
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
            } else {
              strlcpy(cmd, ptr, sizearray(cmd));
              strlcat(cmd, "\n", sizearray(cmd));
              console_add(cmd, STRFLG_ERROR);
            }
          }
          if (strlen(mcu_family) > 0) {
            bmscript_load(mcu_family);
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
        check_stopped(&source_execfile, &source_execline);
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          strcpy(cmd, "-target-attach 1\n");
          if (task_stdin(&task, cmd))
            console_input(cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          curstate = (strncmp(gdbmi_isresult(), "done", 4) == 0) ? STATE_FILE : STATE_STOPPED;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_FILE:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          dwarf_cleanup(&dwarf_linetable, &dwarf_symboltable, &dwarf_filetable);
          /* create parameter filename from target filename, then read target-specific settings */
          strlcpy(txtParamFile, txtFilename, sizearray(txtParamFile));
          strlcat(txtParamFile, ".bmcfg", sizearray(txtParamFile));
          load_settings(txtParamFile, txtEntryPoint, sizearray(txtEntryPoint), &opt_tpwr, &opt_autodownload, &opt_swo);
          if (opt_tpwr && curstate == STATE_MON_SCAN) /* set power and (re-)attach */
             curstate = STATE_MON_TPWR;
          /* load target filename in GDB */
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
            if (strncmp(gdbmi_isresult(), "error", 5) == 0) {
              strlcpy(cmd, gdbmi_isresult(), sizearray(cmd));
              strlcat(cmd, "\n", sizearray(cmd));
              console_add(cmd, STRFLG_ERROR);
            }
            set_idle_time(1000); /* stay in file state */
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_FILE_TEST:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          /* read the DWARF information (for function and variable lookup) */
          FILE *fp = fopen(txtFilename, "rb");
          if (fp != NULL) {
            int address_size;
            dwarf_read(fp, &dwarf_linetable, &dwarf_symboltable, &dwarf_filetable, &address_size);
            fclose(fp);
          }
          /* clear source files (if any were loaded); these will be reloaded
             after the list of source files is refreshed */
          sources_clear(0);
          if (sourcefiles != NULL) {
            free((void*)sourcefiles);
            sourcefiles = NULL;
          }
          /* also get the list of source files from GDB (it might use a different
             order of the file index numbers) */
          strcpy(cmd, "-file-list-exec-source-files\n");
          if (task_stdin(&task, cmd))
            console_input(cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          if (strncmp(gdbmi_isresult(), "done", 4) == 0) {
            sources_parse(gdbmi_isresult() + 5);  /* + 5 to skip "done" and comma */
            sourcefiles = sources_getnames();     /* create array with names for the dropdown */
            warn_source_tstamps = !check_sources_tstamps(txtFilename); /* check timestamps of sources against elf file */
            curstate = STATE_MEMACCESS_1;
          } else {
            if (strncmp(gdbmi_isresult(), "error", 5) == 0) {
              strlcpy(cmd, gdbmi_isresult(), sizearray(cmd));
              strlcat(cmd, "\n", sizearray(cmd));
              console_add(cmd, STRFLG_ERROR);
            }
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
          /* GDB tends to "forget" this setting, so before running a script we
             drop back to this state, but then proceed with the appropriate
             script for the state */
          curstate = (nextstate > 0) ? nextstate : STATE_MEMACCESS_2;
          nextstate = -1;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_MEMACCESS_2:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          if (bmscript_line_fmt("memremap", cmd, NULL)) {
            /* run first line from the script */
            task_stdin(&task, cmd);
            atprompt = 0;
            prevstate = curstate;
            console_replaceflags = STRFLG_LOG;  /* move LOG to SCRIPT, to hide script output by default */
            console_xlateflags = STRFLG_SCRIPT;
          } else {
            curstate = STATE_VERIFY;
          }
        } else if (gdbmi_isresult() != NULL) {
          /* run next line from the script (on the end of the script, move to
             the next state) */
          if (bmscript_line_fmt(NULL, cmd, NULL)) {
            task_stdin(&task, cmd);
            atprompt = 0;
          } else {
            console_replaceflags = console_xlateflags = 0;
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
        /* first try to find "main" in the DWARF information (if this fails,
           we have for some reason failed to load/parse the DWARF information) */
        if (strlen(txtEntryPoint) == 0)
          strcpy(txtEntryPoint, "main");
        if (prevstate != curstate && dwarf_sym_from_name(&dwarf_symboltable, txtEntryPoint)!= NULL) {
          //??? also check whether "main" (or the chosen entry point) is a function
          curstate = STATE_START;     /* main() found, restart program at main */
          break;
        }
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          /* check whether the there is a function "main" */
          assert(strlen(txtEntryPoint) != 0);
          sprintf(cmd, "info functions ^%s$\n", txtEntryPoint);
          task_stdin(&task, cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          STRINGLIST *item;
          const char *ptr;
          gdbmi_sethandled(0); /* first flag the result message as handled, to find the line preceding it */
          item = stringlist_getlast(&consolestring_root, 0, STRFLG_HANDLED);
          assert(item != NULL && item->text != NULL);
          ptr = strstr(item->text, txtEntryPoint);
          if (ptr != NULL && (ptr == item->text || *(ptr - 1) == ' ')) {
            curstate = STATE_START;     /* main() found, restart program at main */
          } else {
            check_stopped(&source_execfile, &source_execline);
            source_cursorfile = source_execfile;
            source_cursorline = source_execline;
            curstate = STATE_STOPPED;   /* main() not found, stay stopped */
            cont_is_run = 1;            /* but when "Cont" is pressed, "run" is performed */
          }
        }
        break;
      case STATE_START:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          assert(strlen(txtEntryPoint) != 0);
          sprintf(cmd, "-break-insert -t %s\n", txtEntryPoint);
          task_stdin(&task, cmd);
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
            assert(sources != NULL);
            sprintf(cmd, "-break-insert %s:%d\n", sources[stateparam[1]].path, stateparam[2]);
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
          ctf_parse_cleanup();
          ctf_decode_cleanup();
          tracestring_clear();
          tracelog_statusmsg(TRACESTATMSG_CTF, NULL, 0);
          ctf_error_notify(CTFERR_NONE, 0, NULL);
          if (!opt_swo.force_plain
              && ctf_findmetadata(txtFilename, opt_swo.metadata, sizearray(opt_swo.metadata))
              && ctf_parse_init(opt_swo.metadata) && ctf_parse_run())
          {
            const CTF_STREAM *stream;
            /* stream names overrule configured channel names */
            for (idx = 0; (stream = stream_by_seqnr(idx)) != NULL; idx++)
              if (stream->name != NULL && strlen(stream->name) > 0)
                channel_setname(idx, stream->name);
          } else {
            ctf_parse_cleanup();
          }
          if (opt_swo.mode == SWOMODE_ASYNC)
            sprintf(cmd, "monitor traceswo %u\n", opt_swo.bitrate); /* automatically select async mode in the BMP */
          else
            strlcpy(cmd, "monitor traceswo\n", sizearray(cmd));
          task_stdin(&task, cmd);
          atprompt = 0;
          prevstate = curstate;
        } else if (gdbmi_isresult() != NULL) {
          /* check the reply of "monitor traceswo" to get the endpoint */
          STRINGLIST *item = stringlist_getlast(&consolestring_root, STRFLG_STATUS, 0);
          char *ptr;
          if ((ptr = strchr(item->text, ':'))!= NULL && strtol(ptr + 1,&ptr, 16)!= 5 && *ptr == ':') {
            long ep = strtol(ptr + 1, NULL, 16);
            if (ep > 0x80)
              trace_endpoint = (unsigned char)ep;
          }
          /* initial setup (only needs to be done once) */
          if (trace_status != TRACESTAT_OK) {
            /* trace_status() does nothing if initialization had already succeeded */
            if (probe == netprobe)
              trace_status = trace_init(BMP_PORT_TRACE, txtIPaddr);
            else
              trace_status = trace_init(trace_endpoint, NULL);
            if (trace_status != TRACESTAT_OK) {
              console_add("Failed to initialize SWO tracing\n", STRFLG_ERROR);
              if ((probe == netprobe && opt_swo.mode != SWOMODE_ASYNC) || (probe != netprobe && opt_swo.mode != SWOMODE_MANCHESTER))
                console_add("Check trace mode (manchester versus async)\n", STRFLG_ERROR);
            } else {
              trace_setdatasize(opt_swo.datasize);
            }
          }
          /* GDB may have reset the "mem inaccessible-by-default off" setting,
             so we jump back to the state, after making sure that the state
             that follows this is the one to run the SWO script */
          nextstate = STATE_SWODEVICE;
          curstate = STATE_MEMACCESS_1;
          gdbmi_sethandled(0);
        }
        break;
      case STATE_SWODEVICE:
        if ((opt_swo.mode != SWOMODE_MANCHESTER && opt_swo.mode != SWOMODE_ASYNC) || opt_swo.clock == 0) {
          curstate = STATE_SWOCHANNELS;
          break;
        }
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          if (bmscript_line_fmt("swo_device", cmd, NULL)) {
            /* run first line from the script */
            task_stdin(&task, cmd);
            atprompt = 0;
            prevstate = curstate;
            console_replaceflags = STRFLG_LOG;  /* move LOG to SCRRIPT, to hide script output by default */
            console_xlateflags = STRFLG_SCRIPT;
          } else {
            curstate = STATE_SWOGENERIC;
          }
        } else if (gdbmi_isresult() != NULL) {
          /* run next line from the script (on the end of the script, move to
             the next state) */
          if (bmscript_line_fmt(NULL, cmd, NULL)) {
            task_stdin(&task, cmd);
            atprompt = 0;
          } else {
            console_replaceflags = console_xlateflags = 0;
            curstate = STATE_SWOGENERIC;
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_SWOGENERIC:
        if ((opt_swo.mode != SWOMODE_MANCHESTER && opt_swo.mode != SWOMODE_ASYNC) || opt_swo.clock == 0) {
          curstate = STATE_SWOCHANNELS;
          break;
        }
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          assert(opt_swo.bitrate > 0 && opt_swo.clock > 0);
          scriptparams[0] = (opt_swo.mode == SWOMODE_MANCHESTER) ? 1 : 2;
          scriptparams[1] = opt_swo.clock / opt_swo.bitrate - 1;
          if (bmscript_line_fmt("swo_generic", cmd, scriptparams)) {
            /* run first line from the script */
            task_stdin(&task, cmd);
            atprompt = 0;
            prevstate = curstate;
            console_replaceflags = STRFLG_LOG;  /* move LOG to SCRRIPT, to hide script output by default */
            console_xlateflags = STRFLG_SCRIPT;
          } else {
            curstate = STATE_SWOCHANNELS;
          }
        } else if (gdbmi_isresult() != NULL) {
          /* run next line from the script (on the end of the script, move to
             the next state) */
          if (bmscript_line_fmt(NULL, cmd, scriptparams)) {
            task_stdin(&task, cmd);
            atprompt = 0;
          } else {
            console_replaceflags = console_xlateflags = 0;
            curstate = STATE_SWOCHANNELS;
          }
          gdbmi_sethandled(0);
        }
        break;
      case STATE_SWOCHANNELS:
        if (!atprompt)
          break;
        if (prevstate != curstate) {
          assert(opt_swo.bitrate > 0);
          scriptparams[0] = 0;
          if (opt_swo.mode != SWOMODE_NONE) {
            /* if SWO mode is disabled, simply turn all channels off (so skip testing whether they should be on) */
            for (idx = 0; idx < NUM_CHANNELS; idx++)
              if (channel_getenabled(idx))
                scriptparams[0] |= (1 << idx);
          }
          if (bmscript_line_fmt("swo_channels", cmd, scriptparams)) {
            /* run first line from the script */
            task_stdin(&task, cmd);
            atprompt = 0;
            prevstate = curstate;
            console_replaceflags = STRFLG_LOG;  /* move LOG to SCRRIPT, to hide script output by default */
            console_xlateflags = STRFLG_SCRIPT;
          } else {
            curstate = STATE_STOPPED;
          }
        } else if (gdbmi_isresult() != NULL) {
          /* run next line from the script (on the end of the script, move to
             the next state) */
          if (bmscript_line_fmt(NULL, cmd, scriptparams)) {
            task_stdin(&task, cmd);
            atprompt = 0;
          } else {
            console_replaceflags = console_xlateflags = 0;
            bmscript_clearcache();
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
      if (monitor_cmd_active)
        flags |= STRFLG_MON_OUT;  /* so output goes to the main console instead of semihosting view */
      if (console_add(cmd, flags)) {
        atprompt = 1;
        console_activate = 1;
        monitor_cmd_active = 0;
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
          nk_layout_row_push(ctx, BUTTON_WIDTH);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "reset") || nk_input_is_key_pressed(&ctx->input, NK_KEY_CTRL_F2))
            curstate = STATE_FILE;
          tooltip(ctx, bounds, " Reload and restart the program (F2)", &rc_canvas);
          nk_layout_row_push(ctx, BUTTON_WIDTH);
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
          nk_layout_row_push(ctx, BUTTON_WIDTH);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "next") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F10)) {
            curstate = STATE_EXEC_CMD;
            stateparam[0] = STATEPARAM_EXEC_NEXT;
          }
          tooltip(ctx, bounds, " Step over (F10)", &rc_canvas);
          nk_layout_row_push(ctx, BUTTON_WIDTH);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "step") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F11)) {
            curstate = STATE_EXEC_CMD;
            stateparam[0] = STATEPARAM_EXEC_STEP;
          }
          tooltip(ctx, bounds, " Step into (F11)", &rc_canvas);
          nk_layout_row_push(ctx, BUTTON_WIDTH);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "finish") || nk_input_is_key_pressed(&ctx->input, NK_KEY_SHIFT_F11)) {
            curstate = STATE_EXEC_CMD;
            stateparam[0] = STATEPARAM_EXEC_FINISH;
          }
          tooltip(ctx, bounds, " Step out of function (Shift+F11)", &rc_canvas);
          nk_layout_row_push(ctx, BUTTON_WIDTH);
          bounds = nk_widget_bounds(ctx);
          if (nk_button_label(ctx, "until") || nk_input_is_key_pressed(&ctx->input, NK_KEY_F7)) {
            curstate = STATE_EXEC_CMD;
            stateparam[0] = STATEPARAM_EXEC_UNTIL;
            stateparam[1] = source_cursorline;
          }
          tooltip(ctx, bounds, " Run until cursor (F7)", &rc_canvas);
          combo_width = splitter_columns[0] - 6 * (BUTTON_WIDTH + 5);
          nk_layout_row_push(ctx, combo_width);
          if (sources_count > 0) {
            int curfile = source_cursorfile;
            if (curfile < 0 || (unsigned)curfile >= sources_count)
              curfile = 0;
            assert(sourcefiles != NULL);
            source_cursorfile = nk_combo(ctx, sourcefiles, sources_count, curfile, (int)COMBOROW_CY, nk_vec2(combo_width, 10*ROW_HEIGHT));
            if (source_cursorfile != curfile)
              source_cursorline = 1;  /* reset scroll */
          }
          nk_layout_row_end(ctx);
          if (source_cursorfile >= 0 && source_cursorfile < sources_count) {
            int linecount = source_linecount(source_cursorfile);
            if (source_cursorline > linecount)
              source_cursorline = linecount;
          }
          nk_layout_row_dynamic(ctx, splitter_rows[0] - ROW_HEIGHT - 4, 1);
          bounds = nk_widget_bounds(ctx);
          source_widget(ctx, "source", opt_fontsize);
          if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, bounds)) {
            int row, col;
            source_mouse2char(ctx, "source", opt_fontsize, bounds, &row, &col);
            if (col == 0) {
              /* click in the margin: set/clear/enable/disable breakpoint
                 - if there is no breakpoint on this line -> add a breakpoint
                 - if there is an enabled breakpoint on this line -> disable it
                 - if there is a disabled breakpoint on this line -> check
                   whether the current line is the same as the one previously
                   clicked on; if yes -> delete; if no: -> enable */
              BREAKPOINT *bp = breakpoint_lookup(source_cursorfile, row);
              if (bp == NULL) {
                /* no breakpoint yet -> add (bu check first whether that can
                   be done) */
                if (source_cursorfile < sources_count) {
                  curstate = STATE_BREAK_TOGGLE;
                  stateparam[0] = STATEPARAM_BP_ADD;
                  stateparam[1] = source_cursorfile;
                  stateparam[2] = row;
                }
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
              if (row > 0 && row <= source_linecount(source_cursorfile))
                source_cursorline = row;
            }
            prev_clicked_line = row;
          } else if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds)) {
            int row, col;
            char sym[64];
            source_mouse2char(ctx, "source", opt_fontsize, bounds, &row, &col);
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
        nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_REPEAT(3));
        if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
          insplitter = SPLITTER_VERTICAL; /* in vertical splitter */
        else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
          insplitter = SPLITTER_NONE;
        if (insplitter == SPLITTER_VERTICAL)
          splitter_ver = (splitter_rows[0] + ctx->input.mouse.delta.y) / (canvas_height - SEPARATOR_VER - 4 * SPACING);

        /* console space */
        nk_layout_row_dynamic(ctx, splitter_rows[1], 1);
        if (nk_group_begin(ctx, "console", NK_WINDOW_NO_SCROLLBAR|NK_WINDOW_BORDER)) {
          nk_layout_row_dynamic(ctx, splitter_rows[1] - ROW_HEIGHT - SPACING, 1);
          console_widget(ctx, "console-out", opt_fontsize);
          nk_layout_row_dynamic(ctx, ROW_HEIGHT, 1);
          if (curstate < STATE_START && !atprompt) {
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
              char *ptr;
              for (ptr = strchr(console_edit, '\0'); ptr > console_edit && *(ptr - 1) <= ' '; )
                *--ptr = '\0'; /* strip trailing whitespace */
              /* some commands are handled internally */
              if (handle_display_cmd(console_edit, stateparam, statesymbol, sizearray(statesymbol))) {
                curstate = STATE_WATCH_TOGGLE;
                tab_states[TAB_WATCHES] = nk_true; /* make sure the watch view to open */
              } else if (handle_file_cmd(console_edit, txtFilename, sizearray(txtFilename))) {
                curstate = STATE_FILE;
                /* save target-specific settings in the parameter file before
                   switching to the new file (new options are loaded in the
                   STATE_FILE case) */
                if (strlen(txtFilename) > 0 && access(txtFilename, 0) == 0)
                  save_settings(txtParamFile, txtEntryPoint, opt_tpwr, opt_autodownload, &opt_swo);
              } else if ((result = handle_trace_cmd(console_edit, &opt_swo)) != 0) {
                if (result == 1) {
                  monitor_cmd_active = 1; /* to silence output of scripts */
                  curstate = STATE_SWOTRACE;
                } else if (result == 2) {
                  curstate = STATE_SWOCHANNELS;
                } else if (result == 3) {
                  trace_info_mode(&opt_swo);
                  if (opt_swo.mode != SWOMODE_NONE) {
                    int chan;
                    for (chan = 0; chan < NUM_CHANNELS; chan++)
                      trace_info_channel(chan, 1);
                  }
                }
                tab_states[TAB_SWO] = nk_true;  /* make sure the SWO tracing view is open */
              } else if (!handle_list_cmd(console_edit, &dwarf_symboltable, &dwarf_filetable)
                         && !handle_find_cmd(console_edit)
                         && !handle_help_cmd(console_edit))
              {
                /* check monitor command, to avoid that the output should goes
                   to the semihosting view */
                monitor_cmd_active = is_monitor_cmd(console_edit);
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
      nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_VERTICAL | NK_SYMBOL_REPEAT(3));
      if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
        insplitter = SPLITTER_HORIZONTAL; /* in horizontal splitter */
      else if (insplitter != SPLITTER_NONE && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
        insplitter = SPLITTER_NONE;
      if (insplitter == SPLITTER_HORIZONTAL)
        splitter_hor = (splitter_columns[0] + ctx->input.mouse.delta.x) / (canvas_width - SEPARATOR_HOR - 2 * SPACING);

      /* right column */
      if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
        if (nk_tree_state_push(ctx, NK_TREE_TAB, "Configuration", &tab_states[TAB_CONFIGURATION])) {
          #define LABEL_WIDTH (2.5 * opt_fontsize)
          float edtwidth, newprobe;
          char basename[_MAX_PATH], *p;
          bounds = nk_widget_bounds(ctx);
          edtwidth = bounds.w - LABEL_WIDTH - BROWSEBTN_WIDTH - (2 * 5);
          /* debug probe (and IP address) */
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "Probe", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
          nk_layout_row_push(ctx, edtwidth);
          bounds = nk_widget_bounds(ctx);
          newprobe = nk_combo(ctx, probelist, netprobe+1, probe, (int)COMBOROW_CY, nk_vec2(bounds.w, 4.5*ROW_HEIGHT));
          if (newprobe == netprobe) {
            int reconnect = 0;
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
            nk_layout_row_push(ctx, LABEL_WIDTH);
            nk_label(ctx, "IP", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
            nk_layout_row_push(ctx, edtwidth);
            bounds = nk_widget_bounds(ctx);
            result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, txtIPaddr, sizearray(txtIPaddr), nk_filter_ascii);
            if ((result & NK_EDIT_COMMITED) != 0 && is_ip_address(txtIPaddr))
              reconnect = 1;
            tooltip(ctx, bounds, "IP address of the ctxLink", &rc_canvas);
            nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
            bounds = nk_widget_bounds(ctx);
            if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
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
            tooltip(ctx, bounds, "Scan network for ctxLink probes.", &rc_canvas);
            if (reconnect)
              curstate = STATE_SCAN_BMP;
          }
          if (newprobe != probe) {
            probe = newprobe;
            curstate = STATE_SCAN_BMP;
          }
          /* GDB */
          p = strrchr(txtGDBpath, DIRSEP_CHAR);
          strlcpy(basename,(p == NULL)? txtGDBpath : p + 1, sizearray(basename));
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "GDB", NK_TEXT_LEFT);
          nk_layout_row_push(ctx, edtwidth);
          bounds = nk_widget_bounds(ctx);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_READ_ONLY, basename, sizearray(basename), nk_filter_ascii);
          tooltip(ctx, bounds, txtGDBpath, &rc_canvas);
          nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
          if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
            const char *s;
            s = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                                     "Executables\0*.elf;*.\0All files\0*.*\0",
                                     NULL, txtGDBpath, "Select GDB program",
                                     guidriver_apphandle());
            if (s != NULL && strlen(s) < sizearray(txtGDBpath)) {
              strcpy(txtGDBpath, s);
              free((void*)s);
              task_close(&task);  /* terminate running instance of GDB */
              curstate = STATE_INIT;
            }
          }
          nk_layout_row_end(ctx);
          /* target executable */
          p = strrchr(txtFilename, '/');
          strlcpy(basename, (p == NULL) ? txtFilename : p + 1, sizearray(basename));
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "File", NK_TEXT_LEFT);
          nk_layout_row_push(ctx, edtwidth);
          bounds = nk_widget_bounds(ctx);
          nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_READ_ONLY, basename, sizearray(basename), nk_filter_ascii);
          tooltip(ctx, bounds, txtFilename, &rc_canvas);
          nk_layout_row_push(ctx, BROWSEBTN_WIDTH);
          if (nk_button_symbol(ctx, NK_SYMBOL_TRIPLE_DOT)) {
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
          /* target entry point */
          nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 2);
          nk_layout_row_push(ctx, LABEL_WIDTH);
          nk_label(ctx, "Entry point", NK_TEXT_LEFT);
          nk_layout_row_push(ctx, edtwidth + BROWSEBTN_WIDTH);
          result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, txtEntryPoint, sizearray(txtEntryPoint), nk_filter_ascii);
          nk_layout_row_end(ctx);
          if (result & NK_EDIT_ACTIVATED)
            console_activate = 0;
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
              assert(source_getname(bp->filenr) != NULL);
              sprintf(label, "%s : %d", source_getname(bp->filenr), bp->linenr);
            }
            w = font->width(font->userdata, font->height, label, strlen(label)) + 10;
            if (w > width)
              width = w;
          }
          for (bp = breakpoint_root.next; bp != NULL; bp = bp->next) {
            int en;
            nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 3);
            nk_layout_row_push(ctx, LABEL_WIDTH);
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
              assert(source_getname(bp->filenr) != NULL);
              sprintf(label, "%s : %d", source_getname(bp->filenr), bp->linenr);
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
            nk_layout_row_push(ctx, LABEL_WIDTH);
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
          nk_layout_row_push(ctx, LABEL_WIDTH);
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

        /* highlight tab text if new content arrives and the tab is closed */
        highlight = !tab_states[TAB_SEMIHOSTING] && stringlist_count(&semihosting_root) != semihosting_lines;
        if (highlight)
          nk_style_push_color(ctx,&ctx->style.tab.text, nk_rgba(255, 255, 160, 255));
        result = nk_tree_state_push(ctx, NK_TREE_TAB, "Semihosting output", &tab_states[TAB_SEMIHOSTING]);
        if (highlight)
          nk_style_pop_color(ctx);
        if (result) {
          nk_layout_row_dynamic(ctx, tab_heights[TAB_SEMIHOSTING], 1);
          nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, nk_rgba(20, 29, 38, 225));
          if (nk_group_begin(ctx, "semihosting", 0)) {
            STRINGLIST *item;
            semihosting_lines = 0;
            for (item = semihosting_root.next; item != NULL; item = item->next) {
              nk_layout_row_dynamic(ctx, opt_fontsize, 1);
              nk_label(ctx, item->text, NK_TEXT_LEFT);
              semihosting_lines += 1;
            }
            nk_group_end(ctx);
          }
          nk_style_pop_color(ctx);
          /* make view height resizeable */
          nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
          bounds = nk_widget_bounds(ctx);
          nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_REPEAT(3));
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

        /* highlight tab text if new content arrives and the tab is closed */
        highlight = !tab_states[TAB_SWO] && tracestring_count() != swo_lines;
        if (highlight)
          nk_style_push_color(ctx,&ctx->style.tab.text, nk_rgba(255, 255, 160, 255));
        result = nk_tree_state_push(ctx, NK_TREE_TAB, "SWO tracing", &tab_states[TAB_SWO]);
        if (highlight)
          nk_style_pop_color(ctx);
        if (result) {
          tracestring_process(trace_status == TRACESTAT_OK);
          nk_layout_row_dynamic(ctx, tab_heights[TAB_SWO], 1);
          tracelog_widget(ctx, "tracelog", opt_fontsize, -1, 0);
          swo_lines = tracestring_count();
          /* make view height resizeable */
          nk_layout_row_dynamic(ctx, SEPARATOR_VER, 1);
          bounds = nk_widget_bounds(ctx);
          nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_REPEAT(3));
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
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN) && source_cursorline < source_linecount(source_cursorfile)) {
        source_cursorline++;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_UP)) {
        source_cursorline -= source_vp_rows;
        if (source_cursorline < 1)
          source_cursorline = 1;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_DOWN)) {
        int linecount = source_linecount(source_cursorfile);
        source_cursorline += source_vp_rows;
        if (source_cursorline > linecount)
          source_cursorline = linecount;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_TOP)) {
        source_cursorline = 1;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_BOTTOM)) {
        source_cursorline = source_linecount(source_cursorfile);
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_FIND)) {
        strlcpy(console_edit, "find ", sizearray(console_edit));
        console_activate = 2;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F3)) {
        handle_find_cmd("find");  /* find next */
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_GOTO)) {
        strlcpy(console_edit, "list ", sizearray(console_edit));
        console_activate = 2;
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_REFRESH)) {
        consoleedit_next = (consoleedit_next == NULL || consoleedit_next->next == NULL) ? consoleedit_root.next : consoleedit_next->next;
        if (consoleedit_next != NULL) {
          strlcpy(console_edit, consoleedit_next->text, sizearray(console_edit));
          console_activate = 2;
        }
      } else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_TAB)) {
        if (console_autocomplete(console_edit, sizearray(console_edit), &dwarf_symboltable))
          console_activate = 2;
      }

    } /* window */

    nk_end(ctx);

    /* Draw */
    guidriver_render(nk_rgb(30,30,30));
  }
  exitcode = task_close(&task);

  /* save parameter file */
  if (strlen(txtFilename) > 0 && access(txtFilename, 0) == 0)
    save_settings(txtParamFile, txtEntryPoint, opt_tpwr, opt_autodownload, &opt_swo);

  /* save settings */
  ini_puts("Settings", "gdb", txtGDBpath, txtConfigFile);
  sprintf(valstr, "%d %d", canvas_width, canvas_height);
  ini_puts("Settings", "size", valstr, txtConfigFile);
  sprintf(valstr, "%.2f %.2f", splitter_hor, splitter_ver);
  ini_puts("Settings", "splitter", valstr, txtConfigFile);
  for (idx = 0; idx < TAB_COUNT; idx++) {
    char key[32];
    sprintf(key, "view%d", idx);
    sprintf(valstr, "%d %d", tab_states[idx], (int)tab_heights[idx]);
    ini_puts("Settings", key, valstr, txtConfigFile);
  }
  ini_putl("Settings", "allmessages", opt_allmsg, txtConfigFile);
  ini_putl("Settings", "fontsize", opt_fontsize, txtConfigFile);
  ini_puts("Settings", "fontstd", opt_fontstd, txtConfigFile);
  ini_puts("Settings", "fontmono", opt_fontmono, txtConfigFile);
  ini_puts("Session", "recent", txtFilename, txtConfigFile);
  /* save history of commands */
  idx = 1;
  for (consoleedit_next = consoleedit_root.next; consoleedit_next != NULL; consoleedit_next = consoleedit_next->next) {
    char key[32];
    sprintf(key, "cmd%d", idx);
    ini_puts("Commands", key, consoleedit_next->text, txtConfigFile);
    if (idx++ > 50)
      break;  /* limit the number of memorized commands */
  }
  /* save selected debug probe */
  if (is_ip_address(txtIPaddr))
    ini_puts("Settings", "ip-address", txtIPaddr, txtConfigFile);
  ini_putl("Settings", "probe", (probe == netprobe) ? 99 : probe, txtConfigFile);
  if (probelist != NULL) {
    for (idx = 0; idx < netprobe + 1; idx++)
      free((void*)probelist[idx]);
    free(probelist);
  }

  guidriver_close();
  stringlist_clear(&consolestring_root);
  stringlist_clear(&consoleedit_root);
  stringlist_clear(&semihosting_root);
  console_clear();
  sources_clear(nk_true);
  bmscript_clear();
  dwarf_cleanup(&dwarf_linetable,&dwarf_symboltable,&dwarf_filetable);
  tcpip_cleanup();
  return exitcode;
}

