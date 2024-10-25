/*
The MIT License (MIT)

Copyright (c) 2016 Serge Zaitsev
Portions copyright (c) 2023-2024 Thiadmer Riemersma, CompuPhase

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#define __USE_POSIX /* for GNU GCC / Linux */
#define __USE_XOPEN
#define _XOPEN_SOURCE  600
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svnrev.h"
#include "tcl.h"

#if defined WIN32 || defined _WIN32
# if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
#   include "strlcpy.h"
# endif
#else
# if defined __linux__
#   include <bsd/string.h>
#   include <strings.h>
# endif
# define stricmp(s1, s2)  strcasecmp((s1), (s2))
#endif

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __GNUC__
# pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
# pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#endif

#define MAX_VAR_LENGTH  256

struct tcl_value {
  char *data;
  size_t length;  /**< size of the valid data */
  size_t size;    /**< size of the memory buffer that was allocated */
};

/* Token type and control flow constants */
enum { TERROR, TEXECPOINT, TFIELD, TPART, TDONE };
enum { FNORMAL, FERROR, FRETURN, FBREAK, FCONT, FEXIT };

/* Lexer flags & options */
#define LEX_QUOTE   0x01  /* inside a double-quote section */
#define LEX_VAR     0x02  /* special mode for parsing variable names */
#define LEX_NO_CMT  0x04  /* don't allow comment here (so comment is allowed when this is not set) */
#define LEX_SKIPERR 0x08  /* continue parsing despit errors */
#define LEX_SUBST   0x10  /* "subst" command mode -> quotes are not special */

/* special character classification */
#define CTYPE_OPERATOR  0x01
#define CTYPE_SPACE     0x02
#define CTYPE_TERM      0x04
#define CTYPE_SPECIAL   0x08
#define CTYPE_Q_SPECIAL 0x10
#define CTYPE_ALPHA     0x20
#define CTYPE_DIGIT     0x40
#define CTYPE_HEXDIGIT  0x80

static unsigned char ctype_table[256] = { 0 };
static void init_ctype(void) {
  if (!ctype_table[0]) {
    memset(ctype_table, 0, sizeof(ctype_table));
    for (unsigned c = 0; c < 256; c++) {
      /* operator */
      if (c == '|' || c == '&' || c == '~' || c == '<' || c == '>' ||
          c == '=' || c == '!' || c == '-' || c == '+' || c == '*' ||
          c == '/' || c == '%' || c == '?' || c == ':') {
        ctype_table[c] |= CTYPE_OPERATOR;
      }
      /* space */
      if (c == ' ' || c == '\t') {
        ctype_table[c] |= CTYPE_SPACE;
      }
      /* terminator (execution point) */
      if (c == '\n' || c == '\r' || c == ';' || c == '\0') {
        ctype_table[c] |= CTYPE_TERM;
      }
      /* special: always */
      if (c == '[' || c == ']' || c == '"' || c == '\\' || c == '\0' || c == '$') {
        ctype_table[c] |= CTYPE_SPECIAL;
      }
      /* special: if outside double quotes */
      if (c == '{' || c == '}' || c == ';' || c == '\r' || c == '\n') {
        ctype_table[c] |= CTYPE_Q_SPECIAL;
      }
      /* digits (decimal digits are also hexadecimal) */
      if (c >= '0' && c <= '9') {
        ctype_table[c] |= CTYPE_DIGIT | CTYPE_HEXDIGIT;
      }
      /* other hexadecimal digits */
      if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
        ctype_table[c] |= CTYPE_HEXDIGIT;
      }
      /* alphabetic characters (ASCII only) */
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        ctype_table[c] |= CTYPE_ALPHA;
      }
    }
  }
}
#define tcl_is_operator(c)  ((ctype_table[(unsigned char)(c)] & CTYPE_OPERATOR) != 0)
#define tcl_is_space(c)     ((ctype_table[(unsigned char)(c)] & CTYPE_SPACE) != 0)
#define tcl_is_end(c)       ((ctype_table[(unsigned char)(c)] & CTYPE_TERM) != 0)
#define tcl_is_special(c, quote) \
        ((ctype_table[(unsigned char)(c)] & CTYPE_SPECIAL) != 0 ||  \
         (!quote && (ctype_table[(unsigned char)(c)] & CTYPE_Q_SPECIAL) != 0))
#define tcl_isalpha(c)      ((ctype_table[(unsigned char)(c)] & CTYPE_ALPHA) != 0)
#define tcl_isdigit(c)      ((ctype_table[(unsigned char)(c)] & CTYPE_DIGIT) != 0)
#define tcl_isxdigit(c)     ((ctype_table[(unsigned char)(c)] & CTYPE_HEXDIGIT) != 0)

static int tcl_next(const char *string, size_t length, const char **from, const char **to,
                    unsigned *flags) {
  unsigned int i = 0;
  int depth = 0;
  bool quote = ((*flags & LEX_QUOTE) != 0);

  /* Skip leading spaces if not quoted */
  for (; !quote && length > 0 && tcl_is_space(*string); string++, length--)
    {}
  if (length == 0) {
    *from = *to = string;
    return TDONE;
  }
  /* Skip comment (then skip leading spaces again */
  if (*string == '#' && !(*flags & LEX_NO_CMT)) {
    assert(!quote); /* this flag cannot be set, if the "no-comment" flag is set */
    for (; length > 0 && *string != '\n' && *string != '\r'; string++, length--)
      {}
    for (; length > 0 && tcl_is_space(*string); string++, length--)
      {}
  }
  *flags |= LEX_NO_CMT;

  *from = string;
  /* Terminate command if not quoted */
  if (!quote && length > 0 && tcl_is_end(*string)) {
    *to = string + 1;
    *flags &= ~LEX_NO_CMT;  /* allow comment at this point */
    return TEXECPOINT;
  }

  if (*string == '$') { /* Variable token, must not start with a space or quote */
    int deref = 1;
    while (string[deref] == '$' && !(*flags & LEX_VAR)) {  /* double deref */
      deref++;
    }
    if (tcl_is_space(string[deref]) || string[deref] == '"' || (*flags & LEX_VAR)) {
      return TERROR;
    }
    unsigned saved_flags = *flags;
    *flags = (*flags & ~LEX_QUOTE) | LEX_VAR; /* quoting is off, variable name parsing is on */
    int r = tcl_next(string + deref, length - deref, to, to, flags);
    *flags = saved_flags;
    return (r == TFIELD && quote) ? TPART : r;
  }

  if ((*flags & (LEX_SUBST | LEX_VAR)) == LEX_SUBST && (*string == '{' || *string == '}' || *string == '"')) {
    i = 1;
  } else if (*string == '[' || (*string == '{' && !quote)) {
    /* Interleaving pairs are not welcome, but it simplifies the code */
    char open = *string;
    char close = (open == '[' ? ']' : '}');
    for (i = 1, depth = 1; i < length && depth != 0; i++) {
      if (string[i] == '\\' && i+1 < length && (string[i+1] == open || string[i+1] == close || string[i+1] == '\\')) {
        i++;  /* escaped brace/bracket, skip both '\' and the character that follows it */
      } else if (string[i] == open) {
        depth++;
      } else if (string[i] == close) {
        depth--;
      }
    }
  } else if (*string == '"') {
    *flags ^= LEX_QUOTE;                  /* toggle flag */
    quote = ((*flags & LEX_QUOTE) != 0);  /* update local variable to match */
    *from = *to = string + 1;
    if (quote) {
      return TPART;
    }
    if (length < 1 || (length > 1 && (!tcl_is_space(string[1]) && !tcl_is_end(string[1])))) {
      return TERROR;
    }
    *from = *to = string + 1;
    return TFIELD;
  } else if (*string == ']' || *string == '}') {
    return TERROR;    /* Unbalanced bracket or brace */
  } else if (*string == '\\') {
    i = (length >= 4 && *(string + 1) == 'x') ? 4 : 2;
  } else {
    bool isvar = ((*flags & LEX_VAR) != 0);
    bool array_close = false;
    while (i < length && !array_close &&              /* run until string completed... */
           (quote || !tcl_is_space(string[i])) &&     /* ... and no whitespace (unless quoted) ... */
           !(isvar && tcl_is_operator(string[i])) &&  /* ... and no operator in variable mode ... */
           !tcl_is_special(string[i], quote)) {       /* ... and no special characters (where "special" depends on quote status) */
      if (string[i] == '(' && !quote && isvar) {
        for (i = 1, depth = 0; i < length; i++) {
          if (string[i] == '\\' && i+1 < length && (string[i+1] == '(' || string[i+1] == ')')) {
            i++;  /* escaped brace/bracket, skip both '\' and the character that follows it */
          } else if (string[i] == '(') {
            depth++;
          } else if (string[i] == ')') {
            if (--depth == 0)
              break;
          }
        }
        if (string[i] == ')') {
          array_close = true;
        } else {
          i--;
        }
      } else if (string[i] == ')' && !quote && isvar) {
        break;
      }
      i++;
    }
  }
  *to = string + i;
  if (i > length || (i == length && depth)) {
    return TERROR;
  }
  if (quote) {
    return TPART;
  }
  return (tcl_is_space(string[i]) || tcl_is_end(string[i])) ? TFIELD : TPART;
}

/* A helper parser struct and macro (requires C99) */
struct tcl_parser {
  const char *from;
  const char *to;
  const char *start;
  const char *end;
  unsigned flags;
  int token;
};
static struct tcl_parser init_tcl_parser(const char *start, const char *end, int token, unsigned flags) {
  struct tcl_parser p;
  memset(&p, 0, sizeof(p));
  p.start = start;
  p.end = end;
  p.token = token;
  p.flags = flags;
  return p;
}
#define tcl_each(s, len, flag)                                                  \
  for (struct tcl_parser p = init_tcl_parser((s), (s) + (len), TERROR, (flag)); \
       p.start < p.end &&                                                       \
       (((p.token = tcl_next(p.start, p.end - p.start, &p.from, &p.to,          \
                             &p.flags)) != TERROR) ||                           \
        (p.flags & LEX_SKIPERR) != 0);                                          \
       p.start = p.to)

/* -------------------------------------------------------------------------- */

bool tcl_isnumber(const struct tcl_value *value) {
  if (!value) {
    return false;
  }
  const char *p = value->data;
  while (tcl_is_space(*p)) { p++; }     /* allow leading whitespace */
  if (*p == '-') { p++; }               /* allow minus sign before the number */
  if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X') ) {
    p += 2;
    while (tcl_isxdigit(*p)) { p++; }   /* hexadecimal number */
  } else if (*p == '0') {
    p++;                                /* octal number */
    while ('0' <= *p && *p <= '7') { p++; }
  } else {
    while (tcl_isdigit(*p)) { p++; }    /* decimal number */
  }
  while (tcl_is_space(*p)) { p++; }     /* allow trailing whitespace */
  return (*p == '\0');                  /* if dropped on EOS -> successfully
                                           parsed an integer format */
}

const char *tcl_data(const struct tcl_value *value) {
  return value ? value->data : NULL;
}

size_t tcl_length(const struct tcl_value *value) {
  return value ? value->length : 0;
}

tcl_int tcl_number(const struct tcl_value *value) {
  if (tcl_isnumber(value)) {
    return strtoll(value->data, NULL, 0);
  }
  return 0;
}

struct tcl_value *tcl_free(struct tcl_value *value) {
  assert(value && value->data);
  _free(value->data);
  _free(value);
  return NULL;
}

bool tcl_append(struct tcl_value *value, struct tcl_value *tail) {
  assert(value);
  assert(tail);
  if (tcl_length(tail) == 0) {
    tcl_free(tail);     /* no new data to append, done quickly */
    return true;
  }
  size_t needed = tcl_length(value) + tcl_length(tail) + 1;  /* allocate 1 byte extra for EOS */
  if (value->size < needed) {
    size_t newsize = value->size * 2;
    while (newsize < needed) {
      newsize *= 2;
    }
    char *b = _malloc(newsize);
    if (b) {
      if (tcl_length(value) > 0) {
        memcpy(b, tcl_data(value), tcl_length(value));
      }
      _free(value->data);
      value->data = b;
      value->size = newsize;
    }
  }
  if (value->size >= needed) {
    memcpy(value->data + tcl_length(value), tcl_data(tail), tcl_length(tail));
    value->length = tcl_length(value) + tcl_length(tail);
    value->data[value->length] = '\0';
  }
  tcl_free(tail);
  return (value->size >= needed);
}

struct tcl_value *tcl_value(const char *data, long len) {
  assert(data);
  if (len == -1) {
    len = strlen(data);
  }
  assert(len >= 0);
  struct tcl_value *value = _malloc(sizeof(struct tcl_value));
  if (value) {
    size_t size = 8;
    while (size < len + 1) {
      size *= 2;
    }
    value->data = _malloc(size);
    if (value->data) {
      memcpy(value->data, data, len);
      value->data[len] = '\0';  /* set EOS */
      value->length = len;
      value->size = size;
    } else {
      _free(value);
      value = NULL;
    }
  }
  return value;
}

static struct tcl_value *tcl_dup(const struct tcl_value *value) {
  assert(value);
  return tcl_value(tcl_data(value), tcl_length(value));
}

struct tcl_value *tcl_list_new(void) {
  return tcl_value("", 0);
}

int tcl_list_length(const struct tcl_value *list) {  /* returns the number of items in the list */
  int count = 0;
  tcl_each(tcl_data(list), tcl_length(list) + 1, 0) {
    if (p.token == TFIELD) {
      count++;
    }
  }
  return count;
}

static bool tcl_list_item_ptr(const struct tcl_value *list, int index, const char **data, size_t *size) {
  int i = 0;
  tcl_each(tcl_data(list), tcl_length(list) + 1, 0) {
    if (p.token == TFIELD) {
      if (i == index) {
        *data = p.from;
        *size = p.to - p.from;
        if (**data == '{') {
          *data += 1;
          *size -= 2;
        }
        return true;
      }
      i++;
    }
  }
  return false;
}

struct tcl_value *tcl_list_item(const struct tcl_value *list, int index) {
  const char *data;
  size_t sz;
  if (tcl_list_item_ptr(list, index, &data, &sz))
    return tcl_value(data, sz);
  return NULL;
}

bool tcl_list_append(struct tcl_value *list, struct tcl_value *tail) {
  /* calculate required memory */
  assert(list);
  assert(tail);
  bool separator = (tcl_length(list) > 0);
  size_t extrasz = separator ? 1 : 0;
  /* when "tail" contains white space or special characters, it must be quoted
     (so that it is appended to the list as a single item); when "tail" contains
     unbalanced braces, these must be escaped (and as it is convoluted to detect
     exactly which of a sequence of braces is unbalanced, we escape all braces
     in case "unbalance" is detected). */
  bool quote = false;     /* to check whether brace-quoting is needed */
  bool escape = false;    /* to check whether escaping (of braces) is needed */
  if (tcl_length(tail) > 0) {
    int brace_count = 0;  /* to keep to count of braces that may need escaping */
    int nesting = 0;      /* to detect unbalance */
    const char *p = tcl_data(tail);
    size_t len = tcl_length(tail);
    while (len > 0) {
      if (tcl_is_space(*p) || tcl_is_special(*p, false)) {
        quote = true;
      }
      if (*p == '{') {
        nesting++;
        brace_count += 1;
      } else if (*p == '}') {
        if (nesting == 0) {
          escape = true;  /* closing brace without opening brace */
        } else {
          nesting--;
        }
        brace_count += 1;
      }
      len--;
      p++;
    }
    if (nesting > 0) {
      escape = true;    /* opening brace without closing brace */
      extrasz += brace_count;
    }
  } else {
    quote = true;       /* quote, to create an empty element */
  }
  if (quote) {
    extrasz += 2;
  }
  /* allocate & copy */
  size_t needed = tcl_length(list) + tcl_length(tail) + extrasz + 1;
  if (list->size < needed) {
    size_t newsize = list->size * 2;
    while (newsize < needed) {
      newsize *= 2;
    }
    char *newbuf = _malloc(newsize);
    if (!newbuf) {
      tcl_free(tail);
      return false;
    }
    memcpy(newbuf, tcl_data(list), tcl_length(list));
    _free(list->data);
    list->data = newbuf;
    list->size = newsize;
  }
  char *tgt = (char*)tcl_data(list) + tcl_length(list);
  if (separator) {
    *tgt++ = ' ';
  }
  if (quote) {
    *tgt++ = '{';
  }
  if (tcl_length(tail) > 0) {
    if (escape) {
      const char *p = tcl_data(tail);
      size_t len = tcl_length(tail);
      while (len > 0) {
        if (*p == '{' || *p == '}') {
          *tgt++ = '\\';
        }
        *tgt++ = *p++;
        len--;
      }
    } else {
      memcpy(tgt, tcl_data(tail), tcl_length(tail));
      tgt += tcl_length(tail);
    }
  }
  if (quote) {
    *tgt++ = '}';
  }
  *tgt = '\0';
  list->length = tcl_length(list) + tcl_length(tail) + extrasz;
  tcl_free(tail);
  return true;
}

/* -------------------------------------------------------------------------- */

static char *tcl_int2string(char *buffer, size_t bufsz, int radix, tcl_int value);
static int tcl_var_index(const char *name, size_t *baselength);

struct tcl_cmd {
  struct tcl_value *name; /**< function name */
  tcl_cmd_fn_t fn;        /**< function pointer */
  unsigned short subcmds; /**< how many subcommand levels there are, 0 for none */
  unsigned short minargs; /**< minimum number of words for the command (including command & subcommand, excluding options) */
  unsigned short maxargs; /**< maximum number of words for the command, USHRT_MAX = no maximum */
  struct tcl_value *user; /**< user value, used for the code block for Tcl procs & option list for built-ins */
  const char *declpos;    /**< position of declaration (Tcl procs) */
  struct tcl_cmd *next;
};

struct tcl_errinfo {
  const char *codebase;   /**< the string with the main block of a proc or of the script */
  size_t codesize;        /**< the size of the main plock */
  const char *currentpos; /**< points to the start of the current command */
};

struct tcl_env {
  struct tcl_var *vars;
  struct tcl_env *parent;
  struct tcl_errinfo errinfo;
};

struct tcl_var {
  struct tcl_value *name;
  struct tcl_value **value; /**< array of values */
  int elements;           /**< for an array, the number of "values" allocated */
  struct tcl_env *env;    /**< if set, this variable is an alias for a variable at a different scope */
  struct tcl_value *alias;/**< if set, the variable name this variable is aliased to */
  struct tcl_var *next;
};

static struct tcl_env *tcl_env_alloc(struct tcl_env *parent) {
  struct tcl_env *env = _malloc(sizeof(struct tcl_env));
  memset(env, 0, sizeof(struct tcl_env));
  env->parent = parent;
  return env;
}

static struct tcl_var *tcl_env_var(struct tcl_env *env, const char *name) {
  struct tcl_var *var = _malloc(sizeof(struct tcl_var));
  if (var) {
    memset(var, 0, sizeof(struct tcl_var));
    assert(name);
    size_t namesz;
    tcl_var_index(name, &namesz);
    var->name = tcl_value(name, namesz);
    var->elements = 1;
    var->value = _malloc(var->elements * sizeof(struct tcl_value*));
    if (var->value) {
      var->value[0] = tcl_value("", 0);
      var->next = env->vars;
      env->vars = var;
    } else {
      _free(var);
      var = NULL;
    }
  }
  return var;
}

static void tcl_var_free_values(struct tcl_var *var) {
  assert(var && var->value);
  for (int idx = 0; idx < var->elements; idx++) {
    if (var->value[idx]) {
      tcl_free(var->value[idx]);
    }
  }
  _free(var->value);
}

static struct tcl_env *tcl_env_free(struct tcl_env *env) {
  struct tcl_env *parent = env->parent;
  while (env->vars) {
    struct tcl_var *var = env->vars;
    env->vars = env->vars->next;
    tcl_free(var->name);
    tcl_var_free_values(var);
    if (var->alias) {
      tcl_free(var->alias);
    }
    _free(var);
  }
  _free(env);
  return parent;
}

int tcl_cur_scope(struct tcl *tcl) {
  struct tcl_env *global_env = tcl->env;
  int n = 0;
  while (global_env->parent) {
    global_env = global_env->parent;
    n++;
  }
  return n;
}

static struct tcl_env *tcl_env_scope(struct tcl *tcl, int level) {
  assert(level >= 0 && level <= tcl_cur_scope(tcl));
  level = tcl_cur_scope(tcl) - level; /* from absolute to relative level */
  struct tcl_env *env = tcl->env;
  while (level-- > 0) {
    env = env->parent;
  }
  assert(env);
  return env;
}

static const char *tcl_alias_name(const struct tcl_var *var) {
  assert(var);
  assert(var->env);     /* should only call this function on aliased variables */
  return tcl_data(var->alias ? var->alias : var->name);
}

static int tcl_var_index(const char *name, size_t *baselength) {
  size_t len = strlen(name);
  const char *ptr = name + len;
  int idx = 0;
  if (ptr > name && *(ptr - 1) == ')') {
    ptr--;
    while (ptr > name && tcl_isdigit(*(ptr - 1))) {
      ptr--;
    }
    if (ptr > name + 1 && *(ptr - 1) == '(') {
      idx = (int)strtol(ptr, NULL, 10);
      if (idx >= 0) {
        len = ptr - name - 1;
      }
    }
  }
  if (baselength) {
    *baselength = len;
  }
  return idx;
}

static struct tcl_var *tcl_findvar(struct tcl_env *env, const char *name) {
  struct tcl_var *var;
  size_t namesz;
  tcl_var_index(name, &namesz);
  for (var = env->vars; var; var = var->next) {
    const char *varptr = tcl_data(var->name);
    size_t varsz;
    tcl_var_index(tcl_data(var->name), &varsz);
    if (varsz == namesz && strncmp(varptr, name, varsz) == 0) {
      return var;
    }
  }
  return NULL;
}

struct tcl_value *tcl_var(struct tcl *tcl, const char *name, struct tcl_value *value) {
  struct tcl_var *var = tcl_findvar(tcl->env, name);
  if (var && var->env) { /* found local alias of a global/upvar variable; find the global/upvar */
    var = tcl_findvar(var->env, tcl_alias_name(var));
  }
  if (!var) {
    if (!value) {
      /* value being read before being set */
      tcl_error_result(tcl, TCL_ERROR_VARUNKNOWN, name);
      return NULL;
    }
    /* create new variable */
    var = tcl_env_var(tcl->env, name);
  }
  int idx = tcl_var_index(name, NULL);
  if (var->elements <= idx) {
    int newsize = var->elements;
    while (newsize <= idx) {
      newsize *= 2;
    }
    struct tcl_value **newlist = _malloc(newsize * sizeof(struct tcl_value*));
    if (newlist) {
      memset(newlist, 0, newsize * sizeof(struct tcl_value*));
      memcpy(newlist, var->value, var->elements * sizeof(struct tcl_value*));
      _free(var->value);
      var->value = newlist;
      var->elements = newsize;
    } else {
      tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
      idx = 0;  /* sets/returns wrong index, but avoid accessing out of range index */
    }
  }
  if (value) {
    if (var->value[idx]) {
      tcl_free(var->value[idx]);
    }
    var->value[idx] = value;
  } else if (var->value[idx] == NULL) {
    var->value[idx] = tcl_value("", 0);
  }
  return var->value[idx];
}

static void tcl_var_free(struct tcl_env *env, struct tcl_var *var) {
  /* unlink from list */
  if (env->vars == var) {
    env->vars = var->next;
  } else {
    struct tcl_var *pred = env->vars;
    while (pred->next && pred->next != var) {
      pred = pred->next;
    }
    if (pred->next == var) {
      pred->next = var->next;
    }
  }
  /* then delete */
  tcl_free(var->name);
  tcl_var_free_values(var);
  if (var->alias) {
    tcl_free(var->alias);
  }
  _free(var);
}

void tcl_raise_error(struct tcl *tcl, int code, const char *info) {
  static const char _errorInfo[] = "errorInfo";
  static const char *errmsg[] = {
    /* TCL_ERROR_NONE */       "(none)",
    /* TCL_ERROR_GENERAL */    "unspecified error",
    /* TCL_ERROR_MEMORY */     "memory allocation failure",
    /* TCL_ERROR_SYNTAX */     "general syntax error",
    /* TCL_ERROR_BRACES */     "unbalanced curly braces",
    /* TCL_ERROR_EXPR */       "error in expression",
    /* TCL_ERROR_CMDUNKNOWN */ "unknown command",
    /* TCL_ERROR_CMDARGCOUNT */"wrong argument count on command",
    /* TCL_ERROR_SUBCMD */     "unknown command option",
    /* TCL_ERROR_VARUNKNOWN */ "unknown variable name",
    /* TCL_ERROR_NAMEINVALID */"invalid symbol name (variable or command)",
    /* TCL_ERROR_NAMEEXISTS */ "duplicate symbol name (symbol already defined)",
    /* TCL_ERROR_ARGUMENT */   "incorrect (or missing) argument to a command",
    /* TCL_ERROR_DEFAULTARG */ "incorrect default value on parameter",
    /* TCL_ERROR_SCOPE */      "invalid scope (or command not valid at current scope)",
    /* TCL_ERROR_FILEIO */     "operation on file failed",
    /* TCL_ERROR_USER */       "user error",
  };
  assert(tcl);
  assert(code > 0 && code < (sizeof errmsg)/(sizeof errmsg[0]));
  struct tcl_env *global_env = tcl_env_scope(tcl, 0);
  /* check errcode not already set, register only the first error */
  if (tcl->errcode == 0) {
    tcl->errcode = code;
    size_t len = strlen(errmsg[code]) + 1;  /* +1 for '\0' */
    if (info)
      len += strlen(info) + 2;  /* +2 for ": " */
    char *msg = _malloc(len);
    if (msg) {
      strcpy(msg, errmsg[code]);
      if (info) {
        strcat(msg, ": ");
        strcat(msg, info);
      }
      if (tcl->errinfo) {
        tcl_free(tcl->errinfo);
      }
      tcl->errinfo = tcl_value(msg, len);
      _free(msg);
    }
    /* create a global variable that holds a copy of the error info */
    struct tcl_env *env_save = tcl->env;
    tcl->env = global_env;
    tcl_var(tcl, _errorInfo, tcl_dup(tcl->errinfo));
    tcl->env = env_save;
  }
  /* make sure the errorInfo variable can be accessed locally */
  if (tcl->env != global_env) {
    tcl_var(tcl, _errorInfo, tcl_value("", 0));  /* make empty local... */
    struct tcl_var *lclvar = tcl_findvar(tcl->env, _errorInfo);
    assert(lclvar);
    lclvar->env = global_env;                     /* ... link to global */
  }
}

int tcl_result(struct tcl *tcl, int flow, struct tcl_value *result) {
  assert(tcl && tcl->result);
  tcl_free(tcl->result);
  tcl->result = result;
  return flow;
}

int tcl_numeric_result(struct tcl *tcl, tcl_int result) {
  char buf[64] = "";
  const char *ptr = tcl_int2string(buf, sizeof(buf), 10, result);
  return tcl_result(tcl, FNORMAL, tcl_value(ptr, -1));
}

int tcl_error_result(struct tcl *tcl, int code, const char *info) {
  assert(tcl && tcl->result);
  tcl_raise_error(tcl, code, info);
  return tcl_result(tcl, FERROR, tcl_value("", 0));
}

int tcl_empty_result(struct tcl *tcl) {
  assert(tcl && tcl->result);
  if (tcl_length(tcl->result) > 0) {
    tcl_result(tcl, FNORMAL, tcl_value("", 0)); /* skip setting empty result if
                                                   the result is already empty */
  }
  return FNORMAL;
}

static int tcl_hexdigit(char c) {
  if ('0' <= c && c <= '9') return c - '0';
  if ('A' <= c && c <= 'F') return c - 'A' + 10;
  if ('a' <= c && c <= 'f') return c - 'a' + 10;
  return 0;
}

static int tcl_subst(struct tcl *tcl, const char *string, size_t len) {
  if (len == 0) {
    return tcl_empty_result(tcl);
  }
  switch (string[0]) {
  case '{':
    if (len < 2 || *(string + len - 1) != '}') {
      return tcl_error_result(tcl, TCL_ERROR_BRACES, NULL);
    }
    return tcl_result(tcl, FNORMAL, tcl_value(string + 1, len - 2));
  case '$': {
    if (len >= MAX_VAR_LENGTH) {
      return tcl_error_result(tcl, TCL_ERROR_NAMEINVALID, string + 1);
    }
    string += 1; len -= 1;      /* skip '$' */
    if (*string == '$') {
      int r = tcl_subst(tcl, string, len);
      if (r != FNORMAL) {
        return r;
      }
      string = tcl_data(tcl->result);
      len = tcl_length(tcl->result);
    }
    if (*string == '{' && len > 1 && string[len - 1] == '}') {
      string += 1; len -= 2;    /* remove one layer of braces */
    } else if (*string == '[' && len > 1 && string[len - 1] == ']') {
      struct tcl_value *expr = tcl_value(string + 1, len - 2);
      int r = tcl_eval(tcl, tcl_data(expr), tcl_length(expr) + 1);
      tcl_free(expr);
      if (r != FNORMAL) {
        return r;
      }
      string = tcl_data(tcl->result);
      len = tcl_length(tcl->result);
    }
    struct tcl_value *name = tcl_value(string, len);
    /* check for a variable index (arrays) */
    const char *start;
    for (start = tcl_data(name); *start != '\0' && *start != '('; start++)
      {}
    if (*start == '(') {
      start++;
      bool is_var = false;
      int depth = 1;
      const char *end = start;
      while (*end != '\0') {
        if (*end == ')') {
          if (--depth == 0) {
            break;
          }
        } else if (*end == '(') {
          depth++;
        } else if (*end == '$' && depth == 1) {
          is_var = true;
        }
        end++;
      }
      if (*end == ')' && is_var) {  /* evaluate index, build new name */
        int baselen = start - tcl_data(name);
        tcl_subst(tcl, start, end - start);
        tcl_free(name);
        name = tcl_value(string, baselen);  /* includes opening '(' */
        tcl_append(name, tcl_dup(tcl->result));
        tcl_append(name, tcl_value(")", 1));
      }
    }
    const struct tcl_value *v = tcl_var(tcl, tcl_data(name), NULL);
    tcl_free(name);
    return v ? tcl_result(tcl, FNORMAL, tcl_dup(v)) : tcl_error_result(tcl, TCL_ERROR_VARUNKNOWN, NULL);
  }
  case '[': {
    struct tcl_value *expr = tcl_value(string + 1, len - 2);
    int r = tcl_eval(tcl, tcl_data(expr), tcl_length(expr) + 1);
    tcl_free(expr);
    return r;
  }
  case '\\': {
    if (len <= 1) {
      return tcl_error_result(tcl, TCL_ERROR_SYNTAX, NULL);
    }
    char buf[] = "*";
    switch (*(string + 1)) {
    case 'n':
      buf[0] = '\n';
      break;
    case 'r':
      buf[0] = '\r';
      break;
    case 't':
      buf[0] = '\t';
      break;
    case '\n':
      buf[0] = ' ';
      break;
    case 'x':
      if (len >= 4) {
        buf[0] = (char)(tcl_hexdigit(string[2]) << 4 | tcl_hexdigit(string[3]));
      }
      break;
    default:
      buf[0] = *(string + 1);
    }
    return (tcl_result(tcl, FNORMAL, tcl_value(buf, 1)));
  }
  default:
    return tcl_result(tcl, FNORMAL, tcl_value(string, len));
  }
}

static struct tcl_cmd *tcl_lookup_cmd(struct tcl *tcl, struct tcl_value *name) {
  assert(name);
  for (struct tcl_cmd *cmd = tcl->cmds; cmd; cmd = cmd->next) {
    if (strcmp(tcl_data(name), tcl_data(cmd->name)) == 0) {
      return cmd;
    }
  }
  return NULL;
}

static int tcl_exec_cmd(struct tcl *tcl, const struct tcl_value *list) {
  struct tcl_value *cmdname = tcl_list_item(list, 0);
  struct tcl_cmd *cmd = tcl_lookup_cmd(tcl, cmdname);
  int r;
  if (cmd) {
    /* see whether to split off the switches */
    bool has_optlist = (cmd->user && *tcl_data(cmd->user) == '-');
    struct tcl_value *optlist = NULL;
    struct tcl_value *arglist = NULL;
    unsigned numargs = tcl_list_length(list);
    if (has_optlist && numargs > cmd->minargs) {
      unsigned maxopts = numargs - cmd->minargs;
      unsigned numopts = tcl_list_length(cmd->user);
      unsigned count = 0;
      unsigned skipopt = 0;
      for (unsigned i = 0; i < maxopts; i++) {
        struct tcl_value *arg = tcl_list_item(list, i + cmd->subcmds);
        bool is_option = (*tcl_data(arg) == '-');
        if (is_option) {
          if (strcmp(tcl_data(arg), "--") == 0) {
            skipopt = 1;  /* special case, "--" marks the end of the switches in an argument list */
          }
          for (unsigned n = 0; n < numopts; n++) {
            struct tcl_value *opt = tcl_list_item(cmd->user, n);
            if (strcmp(tcl_data(arg), tcl_data(opt)) == 0)
              count++;
            tcl_free(opt);
          }
        }
        tcl_free(arg);
        if (!is_option || skipopt) {
          break;
        }
      }
      if (count > 0) {
        /* move first count elements from the argument list to the option list,
           but keep the command (and subcommand) */
        optlist = tcl_list_new();
        arglist = tcl_list_new();
        for (unsigned i = 0; i < cmd->subcmds; i++) {
          tcl_list_append(arglist, tcl_list_item(list, i));
        }
        for (unsigned i = 0; i < count; i++) {
          tcl_list_append(optlist, tcl_list_item(list, i + cmd->subcmds));
        }
        for (unsigned i = count + cmd->subcmds; i < numargs; i++) {
          tcl_list_append(arglist, tcl_list_item(list, i));
        }
        numargs -= count;
      }
    }
    if (cmd->minargs <= numargs && numargs <= cmd->maxargs)
      r = cmd->fn(tcl, arglist ? arglist : list, has_optlist ? optlist : cmd->user);
    else
      r = tcl_error_result(tcl, TCL_ERROR_CMDARGCOUNT, tcl_data(cmdname));
    if (optlist) {
      tcl_free(optlist);
    }
    if (arglist) {
      tcl_free(arglist);
    }
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_CMDUNKNOWN, tcl_data(cmdname));
  }
  tcl_free(cmdname);
  return r;
}

int tcl_list_find(const struct tcl_value *list, const char *word) {
  int found = -1;
  if (list) {
    int n = tcl_list_length(list);
    assert(word);
    for (int i = 0; i < n && found < 0; i++) {
      struct tcl_value *v = tcl_list_item(list, i);
      if (strcmp(tcl_data(v), word) == 0) {
        found = i;
      }
      tcl_free(v);
    }
  }
  return found;
}

int tcl_eval(struct tcl *tcl, const char *string, size_t length) {
  if (!tcl->env->errinfo.codebase) {
    tcl->env->errinfo.codebase = string;
    tcl->env->errinfo.codesize = length;
  }
  struct tcl_value *list = tcl_list_new();
  struct tcl_value *cur = NULL;
  int result = tcl_empty_result(tcl);  /* preset to empty result */
  bool markposition = true;
  tcl_each(string, length, LEX_SKIPERR) {
    if (markposition) {
      struct tcl_errinfo *info = &tcl->env->errinfo;
      if (p.from >= info->codebase && p.from < info->codebase + info->codesize) {
        info->currentpos = p.from;
      }
      markposition = false;
    }
    switch (p.token) {
    case TERROR:
      result = tcl_error_result(tcl, TCL_ERROR_SYNTAX, NULL);
      break;
    case TFIELD:
      result = tcl_subst(tcl, p.from, p.to - p.from);
      if (cur) {
        tcl_append(cur, tcl_dup(tcl->result));
      } else {
        cur = tcl_dup(tcl->result);
      }
      tcl_list_append(list, cur);
      cur = NULL;
      break;
    case TPART:
      result = tcl_subst(tcl, p.from, p.to - p.from);
      struct tcl_value *part = tcl_dup(tcl->result);
      if (cur) {
        tcl_append(cur, part);
      } else {
        cur = part;
      }
      break;
    case TEXECPOINT:
    case TDONE:
      if (tcl_list_length(list) > 0) {
        result = tcl_exec_cmd(tcl, list);
        tcl_free(list);
        list = tcl_list_new();
      } else {
        result = FNORMAL;
      }
      markposition = true;
      break;
    }
    if (result != FNORMAL) {
      break;  /* error information already set */
    }
  }
  /* when arrived at the end of the buffer, if the list is non-empty, run that
     last command */
  if (result == FNORMAL && tcl_list_length(list) > 0) {
    if (cur) {
      tcl_list_append(list, cur);
      cur = NULL;
    }
    result = tcl_exec_cmd(tcl, list);
  }
  tcl_free(list);
  if (cur) {
    tcl_free(cur);
  }
  return result;
}

/* -------------------------------------------------------------------------- */

struct exprval; /* forward declaration */
static int tcl_expression(struct tcl *tcl, const char *expression, struct exprval *result); /* forward declaration */

struct tcl_cmd *tcl_register(struct tcl *tcl, const char *name, tcl_cmd_fn_t fn,
                             short subcmds, short minargs, short maxargs,
                             struct tcl_value *user) {
  assert(maxargs == -1 || maxargs >= minargs);
  struct tcl_cmd *cmd = _malloc(sizeof(struct tcl_cmd));
  if (cmd) {
    memset(cmd, 0, sizeof(struct tcl_cmd));
    cmd->name = tcl_value(name, -1);
    cmd->fn = fn;
    cmd->user = user;
    cmd->subcmds = subcmds;
    cmd->minargs = minargs + subcmds + 1; /* + 1 for command itself */
    cmd->maxargs = (maxargs < 0) ? USHRT_MAX : maxargs + subcmds + 1;
    cmd->next = tcl->cmds;
    tcl->cmds = cmd;
  }
  return cmd;
}

static int tcl_cmd_set(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *name = tcl_list_item(args, 1);
  assert(name);
  struct tcl_value *val = tcl_list_item(args, 2);
  val = tcl_var(tcl, tcl_data(name), val);
  tcl_free(name);
  return val ? tcl_result(tcl, FNORMAL, tcl_dup(val)) : tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
}

static int tcl_cmd_unset(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int n = tcl_list_length(args);
  for (int i = 1; i < n; i++) {
    struct tcl_value *name = tcl_list_item(args, 1);
    assert(name);
    struct tcl_var *var = tcl_findvar(tcl->env, tcl_data(name));
    if (var) {
      if (var->env) {
        struct tcl_var *ref_var = tcl_findvar(var->env, tcl_alias_name(var));
        tcl_var_free(var->env, ref_var);
      }
      tcl_var_free(tcl->env, var);
    }
    tcl_free(name);
  }
  return tcl_empty_result(tcl);
}

static int tcl_cmd_global(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  if (!tcl->env->parent) {
    return tcl_error_result(tcl, TCL_ERROR_SCOPE, NULL);
  }
  int r = FNORMAL;
  int n = tcl_list_length(args);
  for (int i = 1; i < n && r == FNORMAL; i++) {
    struct tcl_value *name = tcl_list_item(args, i);
    assert(name);
    if (tcl_findvar(tcl->env, tcl_data(name))) {
      /* name exists locally, cannot create an alias with the same name */
      r = tcl_error_result(tcl, TCL_ERROR_NAMEEXISTS, tcl_data(name));
    } else {
      struct tcl_env *global_env = tcl_env_scope(tcl, 0);
      if (!tcl_findvar(global_env, tcl_data(name))) {
        /* name not known as a global, create it first */
        struct tcl_env *save_env = tcl->env;
        tcl->env = global_env;
        tcl_var(tcl, tcl_data(name), tcl_value("", 0));
        tcl->env = save_env;
      }
      /* make local, find it back, mark it as an alias for a global */
      tcl_var(tcl, tcl_data(name), tcl_value("", 0));  /* make local */
      struct tcl_var *var = tcl_findvar(tcl->env, tcl_data(name));
      if (var) {
        var->env = global_env; /* mark as an alias to the global */
      }
    }
    tcl_free(name);
  }
  return r;
}

static int tcl_cmd_upvar(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int r = FNORMAL;
  int numargs = tcl_list_length(args);
  int i = 1;
  /* get desired level (which is optional) */
  int level;
  struct tcl_value *v = tcl_list_item(args, i);
  assert(v);
  const char *ptr = tcl_data(v);
  if (isdigit(*ptr) || (*ptr == '-' && isdigit(*(ptr + 1))) || (*ptr == '#' && isdigit(*(ptr + 1)))) {
    if (*ptr == '#') {
      level = (int)strtol(ptr + 1, NULL, 10);
    } else {
      level = (int)strtol(ptr, NULL, 10);
      if (level < 0) {
        level = 0;
      }
      level = tcl_cur_scope(tcl) - level;   /* make absolute level (from relative) */
    }
    i++;
  } else {
    level = tcl_cur_scope(tcl) - 1;         /* move up one level */
  }
  tcl_free(v);
  if (level < 0 || level > tcl_cur_scope(tcl)) {
    return tcl_error_result(tcl, TCL_ERROR_SCOPE, NULL);
  }
  struct tcl_env *ref_env = tcl_env_scope(tcl, level);
  assert(ref_env);
  /* go through all variable pairs */
  while (i + 1 < numargs && r == FNORMAL) {
    struct tcl_value *name = tcl_list_item(args, i);
    assert(name);
    struct tcl_value *alias = tcl_list_item(args, i + 1);
    assert(alias);
    if (tcl_findvar(tcl->env, tcl_data(alias))) {
      /* name for the alias already exists (as a local variable) */
      r = tcl_error_result(tcl, TCL_ERROR_NAMEEXISTS, tcl_data(alias));
    } else if (!tcl_findvar(ref_env, tcl_data(name))) {
      /* reference variable does not exist (at the indicated scope) */
      r = tcl_error_result(tcl, TCL_ERROR_VARUNKNOWN, tcl_data(name));
    } else {
      /* make local, find it back, mark it as an alias for a global */
      tcl_var(tcl, tcl_data(alias), tcl_value("", 0));  /* make local */
      struct tcl_var *var = tcl_findvar(tcl->env, tcl_data(alias));
      if (var) {
        var->env = ref_env; /* mark as an alias to the upvar */
        var->alias = tcl_dup(name);
      }
    }
    tcl_free(name);
    tcl_free(alias);
    i += 2;
  }
  if (i < numargs) {
    r = tcl_error_result(tcl, TCL_ERROR_CMDARGCOUNT, "upvar"); /* there must be at least 2 arguments */
  }
  return r;
}

static int tcl_cmd_subst(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *arg = tcl_list_item(args, 1);
  const char *string = tcl_data(arg);
  size_t length = tcl_length(arg);
  struct tcl_value *list = tcl_list_new();
  struct tcl_value *cur = NULL;
  int r = tcl_empty_result(tcl);  /* preset to empty result */
  tcl_each(string, length, LEX_SKIPERR | LEX_SUBST) {
    switch (p.token) {
    case TERROR:
      r = tcl_error_result(tcl, TCL_ERROR_SYNTAX, NULL);
      break;
    case TFIELD:
      r = tcl_subst(tcl, p.from, p.to - p.from);
      if (cur) {
        tcl_append(cur, tcl_dup(tcl->result));
      } else {
        cur = tcl_dup(tcl->result);
      }
      if (tcl_length(list) > 0) {
        tcl_append(list, tcl_value(" ", 1));
      }
      tcl_append(list, cur);
      cur = NULL;
      break;
    case TPART: {
      struct tcl_value *part;
      if (p.to - p.from == 1 && (*p.from == '{' || *p.from == '}' || *p.from == '"')) {
        part = tcl_value(p.from, 1);
      } else {
        r = tcl_subst(tcl, p.from, p.to - p.from);
        part = tcl_dup(tcl->result);
      }
      if (cur) {
        tcl_append(cur, part);
      } else {
        cur = part;
      }
      break;
    }
    case TEXECPOINT:
      tcl_append(list, tcl_value("\n", 1)); /* mark execution point */
      break;
    case TDONE:
      break;
    }
    if (r != FNORMAL) {
      break;  /* error information already set */
    }
  }
  tcl_free(arg);
  if (cur) {
    tcl_free(cur);
  }
  if (r == FNORMAL) {
    tcl_result(tcl, r, list);
  }
  return r;
}

static int tcl_cmd_scan(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *string = tcl_list_item(args, 1);
  struct tcl_value *format = tcl_list_item(args, 2);
  assert(string && format);
  int match = 0;
  const char *sptr = tcl_data(string);
  const char *fptr = tcl_data(format);
  while (*fptr) {
    if (*fptr == '%') {
      fptr++; /* skip '%' */
      char buf[64] = "";
      if (tcl_isdigit(*fptr)) {
        int w = (int)strtol(fptr, (char**)&fptr, 10);
        if (w > 0 && (unsigned)w < sizeof(buf) - 1) {
          strncpy(buf, sptr, w);
          buf[w] = '\0';
          sptr += w;
        }
      }
      int radix = -1;
      int v = 0;
      switch (*fptr++) {
      case 'c':
        if (buf[0]) {
          v = (int)buf[0];
        } else {
          v = (int)*sptr++;
        }
        break;
      case 'd':
        radix = 10;
        break;
      case 'i':
        radix = 0;
        break;
      case 'x':
        radix = 16;
        break;
      }
      if (radix >= 0) {
        if (buf[0]) {
          v = (int)strtol(buf, NULL, radix);
        } else {
          v = (int)strtol(sptr, (char**)&sptr, radix);
        }
      }
      match++;
      struct tcl_value *var = tcl_list_item(args, match + 2);
      if (var) {
        const char *p = tcl_int2string(buf, sizeof buf, 10, v);
        tcl_var(tcl, tcl_data(var), tcl_value(p, -1));
      }
      tcl_free(var);
    } else if (*fptr == *sptr) {
      fptr++;
      sptr++;
    } else {
      break;
    }
  }
  tcl_free(string);
  tcl_free(format);
  return tcl_numeric_result(tcl, match);
}

static int tcl_cmd_format(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  size_t bufsize = 256;
  char *buffer = _malloc(bufsize);
  if (!buffer) {
    return tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
  }
  struct tcl_value *format = tcl_list_item(args, 1);
  assert(format);
  const char *fptr = tcl_data(format);
  size_t buflen = 0;
  int index = 2;
  struct tcl_value *argcopy = NULL;
  while (*fptr) {
    char field[64] = "";
    size_t fieldlen = 0;
    int pad = 0;
    bool left_justify = false;
    if (*fptr == '%' && *(fptr + 1) != '%') {
      fptr++; /* skip '%' */
      if (*fptr == '-') {
        left_justify = true;
        fptr++;
      }
      if (tcl_isdigit(*fptr)) {
        bool zeropad = (*fptr == '0');
        pad = (int)strtol(fptr, (char**)&fptr, 10);
        if (pad <= 0 || (unsigned)pad > sizeof(field) - 1) {
          pad = 0;
        } else {
          memset(field, zeropad ? '0' : ' ', sizeof(field));
        }
      }
      char ival[32];
      const char *pval = NULL;
      int skip = 0;
      argcopy = tcl_list_item(args, index++);
      switch (*fptr) {
      case 'c':
        fieldlen = (pad > 1) ? pad : 1;
        skip = left_justify ? 0 : fieldlen - 1;
        field[skip] = (char)tcl_number(argcopy);
        break;
      case 'd':
      case 'i':
      case 'x':
        pval = tcl_int2string(ival, sizeof(ival), (*fptr == 'x') ? 16 : 10, tcl_number(argcopy));
        fieldlen = strlen(pval);
        if ((unsigned)pad > fieldlen) {
          skip = left_justify ? 0 : pad - fieldlen;
          fieldlen = pad;
        }
        memcpy(field + skip, pval, strlen(pval));
        break;
      case 's':
        fieldlen = tcl_length(argcopy);
        if ((unsigned)pad > fieldlen) {
          fieldlen = pad;
        }
        break;
      }
      if (*fptr != 's' && argcopy) {
        argcopy = tcl_free(argcopy);
      }
    } else {
      fieldlen = 1;
      field[0] = *fptr;
      if (*fptr == '%') {
        assert(*(fptr + 1) == '%'); /* otherwise, should not have dropped in the else clause */
        fptr++;
      }
    }
    if (buflen + fieldlen + 1 >= bufsize) {
      size_t newsize = 2 * bufsize;
      while (buflen + fieldlen + 1 >= newsize) {
        newsize *= 2;
      }
      char *newbuf = _malloc(newsize);
      if (newbuf) {
        memcpy(newbuf, buffer, buflen);
        _free(buffer);
        buffer = newbuf;
        bufsize = newsize;
      }
    }
    if (buflen + fieldlen + 1 < bufsize) {
      if (argcopy) {
        int slen = tcl_length(argcopy);
        int skip = (pad > slen && !left_justify) ? pad - slen : 0;
        if (pad > slen) {
          memset(buffer + buflen, ' ', pad);
        }
        memcpy(buffer + buflen + skip, tcl_data(argcopy), slen);
      } else {
        memcpy(buffer + buflen, field, fieldlen);
      }
      buflen += fieldlen;
    }
    if (argcopy) {
      argcopy = tcl_free(argcopy);
    }
    fptr++;
  }
  if (buflen + 1 < bufsize) {
    buffer[buflen] = '\0';
  }
  tcl_free(format);
  int r = tcl_result(tcl, FNORMAL, tcl_value(buffer, buflen));
  _free(buffer);
  return r;
}

static int tcl_cmd_incr(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int val = 1;
  if (tcl_list_length(args) == 3) {
    struct tcl_value *incr = tcl_list_item(args, 2);
    val = tcl_number(incr);
    tcl_free(incr);
  }
  struct tcl_value *name = tcl_list_item(args, 1);
  assert(name);
  const struct tcl_value *v = tcl_var(tcl, tcl_data(name), NULL);
  const char *p = "";
  if (v) {
    char buf[64];
    p = tcl_int2string(buf, sizeof buf, 10, tcl_number(v) + val);
    tcl_var(tcl, tcl_data(name), tcl_value(p, -1));
  }
  tcl_free(name);
  return tcl_result(tcl, FNORMAL, tcl_value(p, -1));
}

static int tcl_cmd_append(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int nargs = tcl_list_length(args);
  struct tcl_value *name = tcl_list_item(args, 1);
  assert(name);
  struct tcl_value *val = tcl_var(tcl, tcl_data(name), NULL);
  if (val) {
    val = tcl_dup(val); /* make a copy */
    for (int i = 2; i < nargs; i++) {
      struct tcl_value *tail = tcl_list_item(args, i);
      tcl_append(val, tail);
    }
    tcl_var(tcl, tcl_data(name), tcl_dup(val));
  }
  tcl_free(name);
  return val ? tcl_result(tcl, FNORMAL, val) : tcl_error_result(tcl, TCL_ERROR_VARUNKNOWN, NULL);
}

#define SUBCMD(v, s)  (strcmp(tcl_data(v), (s)) == 0)

/* source: https://github.com/cacharle/globule */
static bool tcl_fnmatch(const char *pattern, const char *string) {
  if (*pattern == '\0')
    return (*string == '\0');
  if (*string == '\0')
    return (strcmp(pattern, "*") == 0);
  switch (*pattern) {
  case '*':
    if (tcl_fnmatch(pattern + 1, string))
      return true;
    if (tcl_fnmatch(pattern, string + 1))
      return true;
    return (tcl_fnmatch(pattern + 1, string + 1));
  case '?':
    return (tcl_fnmatch(pattern + 1, string + 1));
  case '[':
    pattern++;
    bool complement = *pattern == '!';
    if (complement)
      pattern++;
    const char *closing = strchr(pattern + 1, ']') + 1;
    if (*pattern == *string)  // has to contain at least one character
      return (!complement ? tcl_fnmatch(closing, string + 1) : false);
    pattern++;
    for (; *pattern != ']'; pattern++) {
      if (pattern[0] == '-' && pattern + 2 != closing) {
        char range_start = pattern[-1];
        char range_end = pattern[1];
        if (*string >= range_start && *string <= range_end)
          return (!complement ? tcl_fnmatch(closing, string + 1) : false);
        pattern++;
      } else if (*pattern == *string)
        return (!complement ? tcl_fnmatch(closing, string + 1) : false);
    }
    return (!complement ? false : tcl_fnmatch(closing, string + 1));
  case '\\':
    if (*(pattern + 1) == '*' || *(pattern + 1) == '?' || *(pattern + 1) == '[' || *(pattern + 1) == ']') {
      pattern++;  /* ignore escape character in pattern */
    }
    break;
  }
  if (*pattern == *string)
    return (tcl_fnmatch(pattern + 1, string + 1));
  return false;
}

static bool tcl_match(bool exact, const char *pattern, const char *string) {
  if (exact) {
    return (strcmp(pattern, string) == 0);
  }
  return tcl_fnmatch(pattern, string);
}

static int tcl_cmd_string(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  int nargs = tcl_list_length(args);
  int r = FERROR;
  struct tcl_value *subcmd = tcl_list_item(args, 1);
  struct tcl_value *arg1 = tcl_list_item(args, 2);
  if (SUBCMD(subcmd, "length")) {
    r = tcl_numeric_result(tcl, tcl_length(arg1));
  } else if (SUBCMD(subcmd, "tolower") || SUBCMD(subcmd, "toupper")) {
    bool lcase = SUBCMD(subcmd, "tolower");
    const struct tcl_value *tgt = tcl_dup(arg1);
    size_t sz = tcl_length(tgt);
    char *base = (char*)tcl_data(tgt);
    for (size_t i = 0; i < sz; i++) {
      base[i] = lcase ? tolower(base[i]) : toupper(base[i]);
    }
  } else if (SUBCMD(subcmd, "trim") || SUBCMD(subcmd, "trimleft") || SUBCMD(subcmd, "trimright")) {
    const char *chars = " \t\r\n";
    struct tcl_value *arg2 = NULL;
    if (nargs >= 4) {
      arg2 = tcl_list_item(args, 3);
      chars = tcl_data(arg2);
    }
    const char *first = tcl_data(arg1);
    const char *last = first + tcl_length(arg1);
    if (SUBCMD(subcmd, "trim") || SUBCMD(subcmd, "trimleft")) {
      while (*first && strchr(chars, *first)) {
        first++;
      }
    }
    if (SUBCMD(subcmd, "trim") || SUBCMD(subcmd, "trimright")) {
      while (last>first && strchr(chars, *(last - 1))) {
        last--;
      }
    }
    r = tcl_result(tcl, FNORMAL, tcl_value(first, last - first));
    if (arg2) {
      tcl_free(arg2);
    }
  } else {
    if (nargs < 4) {  /* need at least "string subcommand arg arg" */
      tcl_free(subcmd);
      tcl_free(arg1);
      return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
    }
    struct tcl_value *arg2 = tcl_list_item(args, 3);
    if (SUBCMD(subcmd, "compare")) {
      int match = (tcl_list_find(user, "-nocase") >= 0)
                    ? stricmp(tcl_data(arg1), tcl_data(arg2))
                    : strcmp(tcl_data(arg1), tcl_data(arg2));
      r = tcl_numeric_result(tcl, match);
    } else if (SUBCMD(subcmd, "equal")) {
      int match = (tcl_list_find(user, "-nocase") >= 0)
                    ? stricmp(tcl_data(arg1), tcl_data(arg2))
                    : strcmp(tcl_data(arg1), tcl_data(arg2));
      r = tcl_numeric_result(tcl, match == 0);
    } else if (SUBCMD(subcmd, "first") || SUBCMD(subcmd, "last")) {
      int pos = 0;
      if (nargs >= 5) {
        struct tcl_value *arg3 = tcl_list_item(args, 4);
        pos = tcl_number(arg3);
        tcl_free(arg3);
      }
      const char *haystack = tcl_data(arg2);
      const char *needle = tcl_data(arg1);
      const char *p = NULL;
      if (SUBCMD(subcmd, "first")) {
        if (pos < (int)tcl_length(arg2)) {
          p = strstr(haystack + pos, needle);
        }
      } else {
        if (nargs < 5) {
          pos = tcl_length(arg2);
        }
        int len = strlen(needle);
        p = haystack + pos - len;
        while (p >= haystack) {
          if (strncmp(p, needle, len) == 0) {
            break;
          }
          p--;
        }
      }
      r = tcl_numeric_result(tcl, (p && p >= haystack) ? (p - haystack) : -1);
    } else if (SUBCMD(subcmd, "index")) {
      int pos = tcl_number(arg2);
      if (pos >= (int)tcl_length(arg1)) {
        r = tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
      } else {
        r = tcl_result(tcl, FNORMAL, tcl_value(tcl_data(arg1) + pos, 1));
      }
    } else if (SUBCMD(subcmd, "match")) {
      r = tcl_numeric_result(tcl, tcl_fnmatch(tcl_data(arg1), tcl_data(arg2)) == true);
    } else if (SUBCMD(subcmd, "range")) {
      int first = tcl_number(arg2);
      if (first < 0) {
        first = 0;
      }
      int last = INT_MAX;
      if (nargs >= 5) {
        struct tcl_value *arg3 = tcl_list_item(args, 4);
        if (strcmp(tcl_data(arg3), "end") != 0) {
          last = tcl_number(arg3);
        }
        tcl_free(arg3);
      }
      if (last >= (int)tcl_length(arg1)) {
        last = tcl_length(arg1) - 1;
      }
      if (last < first) {
        last = first;
      }
      r = tcl_result(tcl, FNORMAL, tcl_value(tcl_data(arg1) + first, last - first + 1));
    } else if (SUBCMD(subcmd, "replace")) {
      if (nargs < 6) {  /* need at least "string replace text idx1 idx2 word" */
        tcl_free(subcmd);
        tcl_free(arg1);
        tcl_free(arg2);
        return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
      }
      size_t len = tcl_length(arg1);
      int idx1 = tcl_number(arg2);
      if (idx1 < 0 || (unsigned)idx1 >= len) {
        idx1 = 0;
      }
      struct tcl_value *arg3 = tcl_list_item(args, 4);
      int idx2 = tcl_number(arg3);
      if (idx2 < 0 || (unsigned)idx2 >= len) {
        idx2 = len - 1;
      }
      struct tcl_value *modified = tcl_value(tcl_data(arg1), idx1);
      tcl_append(modified, tcl_dup(tcl_list_item(args, 5)));
      tcl_append(modified, tcl_value(tcl_data(arg1) + idx2 + 1, tcl_length(arg1) - (idx2 + 1)));
      tcl_free(arg3);
      r = tcl_result(tcl, FNORMAL, modified);
    } else {
      r = tcl_error_result(tcl, TCL_ERROR_SUBCMD, tcl_data(subcmd));
    }
    if (arg2) {
      tcl_free(arg2);
    }
  }
  tcl_free(subcmd);
  tcl_free(arg1);
  if (r == FERROR) {
    r = tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL); /* generic argument error */
  }
  return r;
}

static int tcl_cmd_info(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int nargs = tcl_list_length(args);
  struct tcl_value *subcmd = tcl_list_item(args, 1);
  int r = FERROR;
  if (SUBCMD(subcmd, "exists")) {
    if (nargs >= 3) {
      struct tcl_value *name = tcl_list_item(args, 2);
      r = tcl_numeric_result(tcl, (tcl_findvar(tcl->env, tcl_data(name)) != NULL));
      tcl_free(name);
    }
  } else if (SUBCMD(subcmd, "tclversion")) {
    r = tcl_result(tcl, FNORMAL, tcl_value(SVNREV_STR, -1));
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_SUBCMD, tcl_data(subcmd));
  }
  tcl_free(subcmd);
  if (r == FERROR) {
    r = tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL); /* generic argument error */
  }
  return r;
}

static int tcl_cmd_array(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int nargs = tcl_list_length(args);
  struct tcl_value *subcmd = tcl_list_item(args, 1);
  struct tcl_value *name = tcl_list_item(args, 2);
  int r = FERROR;
  if (SUBCMD(subcmd, "length") || SUBCMD(subcmd, "size")) {
    struct tcl_var *var = tcl_findvar(tcl->env, tcl_data(name));
    if (var && var->env) { /* found local alias of a global/upvar; find the global/upvar */
      var = tcl_findvar(var->env, tcl_alias_name(var));
    }
    int count = 0;
    if (var) {
      for (int i = 0; i < var->elements; i++) {
        if (var->value[i]) {
          count++;
        }
      }
    }
    r = tcl_numeric_result(tcl, count);
  } else if (SUBCMD(subcmd, "slice")) {
    if (nargs < 4) {  /* need at least "array slice var blob" */
      tcl_free(subcmd);
      tcl_free(name);
      return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
    }
    struct tcl_value *blob = tcl_list_item(args, 3);
    const unsigned char *bptr = (const unsigned char*)tcl_data(blob);
    size_t blen = tcl_length(blob);
    struct tcl_value *wsize = (nargs > 4) ? tcl_list_item(args, 4) : NULL;
    int step = wsize ? (int)tcl_number(wsize) : 1;
    if (step < 1) {
      step = 1;
    }
    if (wsize) {
      tcl_free(wsize);
    }
    struct tcl_value *wendian = (nargs > 5) ? tcl_list_item(args, 5) : NULL;
    bool bigendian = wendian ? strcmp(tcl_data(wendian), "be") == 0 : false;
    if (wendian) {
      tcl_free(wendian);
    }
    int count = 0;
    struct tcl_var *var = NULL;
    while (blen > 0) {
      /* make value (depending on step size) and convert to string */
      tcl_int value = 0;
      for (unsigned i = 0; i < (unsigned)step && i < blen; i++) {
        if (bigendian) {
          value |= (tcl_int)*(bptr + i) << 8 * (step - 1 - i);
        } else {
          value |= (tcl_int)*(bptr + i) << 8 * i;
        }
      }
      char buf[64];
      const char *p = tcl_int2string(buf, sizeof buf, 10, value);
      if (!var) {
        /* make the variable (implicitly sets index 0), find it back */
        tcl_var(tcl, tcl_data(name), tcl_value(p, -1));
        var = tcl_findvar(tcl->env, tcl_data(name));
        assert(var);            /* if it didn't yet exist, it was just created */
        if (var->env) {         /* found local alias of a global/upvar; find the global/upvar */
          var = tcl_findvar(var->env, tcl_alias_name(var));
        }
        /* can immediately allocate the properly sized value list */
        int numelements = blen / step + 1;
        struct tcl_value **newlist = _malloc(numelements * sizeof(struct tcl_value*));
        if (newlist) {
          memset(newlist, 0, numelements * sizeof(struct tcl_value*));
          assert(var->elements == 1);
          newlist[0] = var->value[0];
          _free(var->value);
          var->value = newlist;
          var->elements = numelements;
        } else {
          tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
          count = 0;
          break;
        }
      } else {
        assert(count < var->elements);
        var->value[count] = tcl_value(p, -1);
      }
      count++;
      bptr += step;
      blen = (blen > (size_t)step) ? blen - step : 0;
    }
    tcl_free(blob);
    tcl_numeric_result(tcl, count);
    r = (count > 0) ? FNORMAL : FERROR;
  } else if (SUBCMD(subcmd, "split")) {
    if (nargs < 4) {  /* need at least "array split var string" */
      tcl_free(subcmd);
      tcl_free(name);
      return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
    }
    char varname[MAX_VAR_LENGTH];
    struct tcl_value *v_string = tcl_list_item(args, 3);
    const char *string = tcl_data(v_string);
    size_t string_len = tcl_length(v_string);
    struct tcl_value *v_sep = (tcl_list_length(args) > 4) ? tcl_list_item(args, 4) : NULL;
    const char *chars = v_sep ? tcl_data(v_sep) : " \t\r\n";
    const char *start = string;
    const char *end = start;
    int index = 0;
    while (end - string < (int)string_len) {
      assert(*end);
      if (strchr(chars, *end)) {
        sprintf(varname, "%s(%d)", tcl_data(name), index);
        tcl_var(tcl, varname, tcl_value(start, end - start));
        start = end + 1;
        index += 1;
      }
      end++;
    }
    /* append last item */
    sprintf(varname, "%s(%d)", tcl_data(name), index++);
    tcl_var(tcl, varname, tcl_value(start, end - start));
    tcl_free(v_string);
    if (v_sep) {
      tcl_free(v_sep);
    }
    tcl_numeric_result(tcl, index);
    r = (index > 0) ? FNORMAL : FERROR;
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_SUBCMD, tcl_data(subcmd));
  }
  tcl_free(subcmd);
  tcl_free(name);
  if (r == FERROR) {
    r = tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL); /* generic argument error */
  }
  return r;
}

static int tcl_cmd_list(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *list = tcl_list_new();
  int n = tcl_list_length(args);
  for (int i = 1; i < n; i++) {
    tcl_list_append(list, tcl_list_item(args, i));
  }
  return tcl_result(tcl, FNORMAL, list);
}

static int tcl_cmd_concat(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *list = tcl_list_new();
  int n = tcl_list_length(args);
  for (int i = 1; i < n; i++) {
    struct tcl_value *sublst = tcl_list_item(args, i);
    int sublst_len = tcl_list_length(sublst);
    for (int j = 0; j < sublst_len; j++) {
      tcl_list_append(list, tcl_list_item(sublst, j));
    }
    tcl_free(sublst);
  }
  return tcl_result(tcl, FNORMAL, list);
}

static int tcl_cmd_lappend(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int n = tcl_list_length(args);
  /* check whether the value exists */
  struct tcl_value *name = tcl_list_item(args, 1);
  struct tcl_var *var = tcl_findvar(tcl->env, tcl_data(name));
  if (var && var->env) {    /* found local alias of a global/upvar; find the global/upvar */
    var = tcl_findvar(var->env, tcl_alias_name(var));
  }
  struct tcl_value *list;
  if (var) {
    list = tcl_var(tcl, tcl_data(name), NULL);
    assert(list); /* it was already checked that the variable exists */
  } else {
    /* variable does not exist, create empty list and create variable */
    list = tcl_list_new();
    tcl_var(tcl, tcl_data(name), list);
  }
  /* append other arguments, update the variable */
  for (int i = 2; i < n; i++) {
    tcl_list_append(list, tcl_list_item(args, i));
  }
  tcl_free(name);
  return tcl_result(tcl, FNORMAL, tcl_dup(list));
}

static int tcl_cmd_linsert(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int nargs = tcl_list_length(args);
  struct tcl_value *list = tcl_list_item(args, 1);
  int list_len = tcl_list_length(list);
  struct tcl_value *index = tcl_list_item(args, 2);
  int skip = tcl_number(index);
  tcl_free(index);
  if (skip > list_len) {
    skip = list_len;  /* "insertion" behind the last element (i.e. append) */
  }
  struct tcl_value *newlist = tcl_list_new();
  /* copy elements before the insertion point from the original list */
  for (int i = 0; i < skip; i++) {
    tcl_list_append(newlist, tcl_list_item(list, i));
  }
  /* append arguments on the tail of the linsert command */
  for (int i = 3; i < nargs; i++) {
    tcl_list_append(newlist, tcl_list_item(args, i));
  }
  /* copy the items after the insertion point from the original list */
  for (int i = skip; i < list_len; i++) {
    tcl_list_append(newlist, tcl_list_item(list, i));
  }
  tcl_free(list);
  return tcl_result(tcl, FNORMAL, newlist);
}

static int tcl_cmd_lreplace(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int nargs = tcl_list_length(args);
  struct tcl_value *list = tcl_list_item(args, 1);
  int list_len = tcl_list_length(list);
  struct tcl_value *v_first = tcl_list_item(args, 2);
  int first = tcl_number(v_first);
  tcl_free(v_first);
  struct tcl_value *v_last = tcl_list_item(args, 3);
  int last = (strcmp(tcl_data(v_last), "end") == 0) ? list_len - 1 : tcl_number(v_last);
  tcl_free(v_last);
  struct tcl_value *newlist = tcl_list_new();
  /* copy up to "first" elements from the original list */
  for (int i = 0; i < first; i++) {
    tcl_list_append(newlist, tcl_list_item(list, i));
  }
  /* append arguments after the lreplace command */
  for (int i = 4; i < nargs; i++) {
    tcl_list_append(newlist, tcl_list_item(args, i));
  }
  /* copy the items behind "last" from the original list */
  for (int i = last + 1; i < list_len; i++) {
    tcl_list_append(newlist, tcl_list_item(list, i));
  }
  tcl_free(list);
  return tcl_result(tcl, FNORMAL, newlist);
}

static int tcl_cmd_llength(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *list = tcl_list_item(args, 1);
  int n = tcl_list_length(list);
  tcl_free(list);
  return tcl_numeric_result(tcl, n);
}

static int tcl_cmd_lindex(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *list = tcl_list_item(args, 1);
  struct tcl_value *v_index = tcl_list_item(args, 2);
  int index = tcl_number(v_index);
  tcl_free(v_index);
  int n = tcl_list_length(list);
  int r = FNORMAL;
  if (index < n) {
    r = tcl_result(tcl, FNORMAL, tcl_list_item(list, index));
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
  }
  tcl_free(list);
  return r;
}

static int tcl_cmd_lsearch(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  bool exact_match = (tcl_list_find(user, "-exact") >= 0);
  struct tcl_value *list = tcl_list_item(args, 1);
  struct tcl_value *pattern = tcl_list_item(args, 2);
  int list_len = tcl_list_length(list);
  int result = -1;
  for (int i = 0; i < list_len && result < 0; i++) {
    struct tcl_value *item = tcl_list_item(list, i);
    if (tcl_match(exact_match, tcl_data(pattern), tcl_data(item)))
      result = i;
    tcl_free(item);
  }
  tcl_free(list);
  tcl_free(pattern);
  return tcl_numeric_result(tcl, result);
}

static int tcl_cmd_lsort(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  /* split the list into separate items */
  struct tcl_value *list = tcl_list_item(args, 1);
  int list_len = tcl_list_length(list);
  struct tcl_value **items = malloc(list_len * sizeof(struct tcl_value*));
  bool ok = true;
  if (items) {
    for (int i = 0; i < list_len; i++) {
      items[i] = tcl_list_item(list, i);
      if (!items[i]) {
        ok = false;
      }
    }
  } else {
    ok = false;
  }
  tcl_free(list); /* the original list is no longer needed */
  if (!ok) {
    /* on failure, clean up and return an error code */
    if (items) {
      for (int i = 0; i < list_len; i++) {
        if (items[i]) {
          tcl_free(items[i]);
        }
      }
    }
    return tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
  }
  /* sort the array
     insertion sort, source: Programming Pearls, Jon Bentley, chapter 12 */
  if (tcl_list_find(user, "-integer") >= 0) {
    for (int i = 1; i < list_len; i++) {
      struct tcl_value *key = items[i];
      tcl_int key_int = tcl_number(key);
      int j;
      for (j = i; j > 0 && tcl_number(items[j-1]) > key_int; j--) {
        items[j] = items[j - 1];
      }
      items[j] = key;
    }
  } else {
    for (int i = 1; i < list_len; i++) {
      struct tcl_value *key = items[i];
      int j;
      for (j = i; j > 0 && strcmp(tcl_data(items[j-1]), tcl_data(key)) > 0; j--) {
        items[j] = items[j - 1];
      }
      items[j] = key;
    }
  }
  /* make a new list and copy the sorted array */
  list = tcl_list_new();
  if (tcl_list_find(user, "-decreasing") >= 0) {
    for (int i = list_len - 1; i >= 0; i--) {
      tcl_list_append(list, items[i]);
    }
  } else {
    for (int i = 0; i < list_len; i++) {
      tcl_list_append(list, items[i]);
    }
  }
  return tcl_result(tcl, FNORMAL, list);
}

static int tcl_cmd_lrange(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *list = tcl_list_item(args, 1);
  int list_len = tcl_list_length(list);
  struct tcl_value *v_first = tcl_list_item(args, 2);
  int first = tcl_number(v_first);
  tcl_free(v_first);
  struct tcl_value *v_last = tcl_list_item(args, 3);
  int last = (strcmp(tcl_data(v_last), "end") == 0) ? list_len - 1 : tcl_number(v_last);
  tcl_free(v_last);
  struct tcl_value *rangelist = tcl_list_new();
  for (int i = first; i <= last; i++) {
    tcl_list_append(rangelist, tcl_list_item(list, i));
  }
  tcl_free(list);
  return tcl_result(tcl, FNORMAL, rangelist);
}

static int tcl_cmd_split(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *v_string = tcl_list_item(args, 1);
  const char *string = tcl_data(v_string);
  size_t string_len = tcl_length(v_string);
  struct tcl_value *v_sep = (tcl_list_length(args) > 2) ? tcl_list_item(args, 2) : NULL;
  const char *chars = v_sep ? tcl_data(v_sep) : " \t\r\n";
  struct tcl_value *list = tcl_list_new();
  const char *start = string;
  const char *end = start;
  while (end - string < (long)string_len) {
    assert(*end);
    if (strchr(chars, *end)) {
      tcl_list_append(list, tcl_value(start, end - start));
      start = end + 1;
    }
    end++;
  }
  /* append last item */
  tcl_list_append(list, tcl_value(start, end - start));
  tcl_free(v_string);
  if (v_sep) {
    tcl_free(v_sep);
  }
  return tcl_result(tcl, FNORMAL, list);
}

static int tcl_cmd_join(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *list = tcl_list_item(args, 1);
  int list_len = tcl_list_length(list);
  struct tcl_value *sep = (tcl_list_length(args) >= 3) ? tcl_list_item(args, 2) : tcl_value(" ", 1);
  struct tcl_value *string = tcl_value("", 0);
  for (int i = 0; i < list_len; i++) {
    tcl_append(string, tcl_list_item(list, i));
    if (i + 1 < list_len && tcl_length(sep) > 0) {
      tcl_append(string, tcl_dup(sep));
    }
  }
  tcl_free(list);
  tcl_free(sep);
  return tcl_result(tcl, FNORMAL, string);
}

#if !defined TCL_DISABLE_BINARY
static bool binary_fmt(const char **source, unsigned *width, unsigned *count, bool *is_signed, bool *is_bigendian) {
  assert(source != NULL);
  const char *src = *source;
  assert(src != NULL);
  assert(width != NULL);
  switch (*src) {
  case 'c':   /* 8-bit integer */
    *width = 1;
    break;
  case 's':   /* 16-bit integer */
  case 'S':
    *width = 2;
    break;
  case 'i':   /* 32-bit integer */
  case 'I':
    *width = 4;
    break;
  case 'w':   /* 64-bit integer */
  case 'W':
    *width = 8;
    break;
  default:
    return false;
  }
  assert(is_bigendian != NULL);
  *is_bigendian = isupper(*src);
  src++; /* skip type letter */
  assert(is_signed != NULL);
  *is_signed = true;
  if (*src == 'u') {
    *is_signed = false;
    src++;
  }
  assert(count != NULL);
  *count = 1;
  if (isdigit(*src)) {
    *count = (unsigned)strtol(src, (char**)&src, 10);
  } else if (*src == '*') {
    *count = UINT_MAX;
    src++;
  }
  if (count == 0) {
    return false;
  }
  *source = src;
  return true;
}

static void binary_swapbytes(unsigned char *data, size_t width) {
  unsigned low = 0;
  unsigned high = width - 1;
  while (low < high) {
    unsigned char t = data[low];
    data[low] = data[high];
    data[high] = t;
    low++;
    high--;
  }
}

static bool binary_checkformat(struct tcl_value *fmt, int numvars, size_t *datasize) {
  size_t dsize = 0;
  const char *ftmp = tcl_data(fmt);
  for (int i = 0; i < numvars; i++) {
    unsigned width, count;
    bool sign_extend, is_bigendian;
    if (!binary_fmt(&ftmp, &width, &count, &sign_extend, &is_bigendian)) {
      return false;
    }
    if (count > (unsigned)numvars) {
      count = numvars;
    }
    dsize += width * count;
  }
  if (datasize) {
    *datasize = dsize;
  }
  return true;
}

static int tcl_cmd_binary(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int r = FERROR;
  int nargs = tcl_list_length(args);
  int reqargs = 4, fmtidx = 2;
  struct tcl_value *subcmd = tcl_list_item(args, 1);
  if (SUBCMD(subcmd, "scan")) {
    reqargs = 5;
    fmtidx = 3;
  }
  if (nargs < reqargs) {
    tcl_free(subcmd);
    return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
  }

  struct tcl_value *fmt = tcl_list_item(args, fmtidx);
  int numvars = nargs - (fmtidx + 1); /* first argument is behind the format string */
  if (SUBCMD(subcmd, "format")) {
    /* syntax "binary format fmt arg ..." */
    /* get data size */
    size_t datalen = 0;
    if (!binary_checkformat(fmt, numvars, &datalen)) {
      tcl_free(subcmd);
      tcl_free(fmt);
      return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
    }
    unsigned char *rawdata = _malloc(datalen);
    if (rawdata) {
      int dataidx = 0;
      unsigned width, count = 0;
      bool sign_extend, is_bigendian;
      const char *fmtraw = tcl_data(fmt);
      for (int varidx = 0; varidx < numvars; varidx++) {
        if (count == 0) {
          binary_fmt(&fmtraw, &width, &count, &sign_extend, &is_bigendian);
        }
        struct tcl_value *argvalue = tcl_list_item(args, varidx + fmtidx + 1);
        tcl_int val = tcl_number(argvalue);
        tcl_free(argvalue);
        memcpy(rawdata + dataidx, &val, width);
        if (is_bigendian) {
          binary_swapbytes(rawdata + dataidx, width);
        }
        dataidx += width;
        count--;
      }
      r = tcl_result(tcl, FNORMAL, tcl_value((char*)rawdata, datalen));
      _free(rawdata);
    } else {
      r = tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
    }
  } else if (SUBCMD(subcmd, "scan")) {
    /* syntax "binary scan data fmt var ..." */
    if (!binary_checkformat(fmt, numvars, NULL)) {
      tcl_free(subcmd);
      tcl_free(fmt);
      return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
    }
    struct tcl_value *data = tcl_list_item(args, 2);
    size_t datalen = tcl_length(data);
    unsigned char *rawdata = _malloc(datalen);  /* make a copy of the data */
    if (!rawdata) {
      tcl_free(subcmd);
      tcl_free(fmt);
      tcl_free(data);
      return tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
    }
    memcpy(rawdata, tcl_data(data), datalen);
    tcl_free(data);
    int dataidx = 0;
    int cvtcount = 0;
    const char *fmtraw = tcl_data(fmt);
    for (int varidx = 0; varidx < numvars; varidx++) {
      /* get variable name */
      char varname[128];
      struct tcl_value *var = tcl_list_item(args, varidx + fmtidx + 1);
      strlcpy(varname, tcl_data(var), sizeof varname);
      tcl_free(var);
      /* get field format (validity was already checked) */
      unsigned width, count;
      bool sign_extend, is_bigendian;
      binary_fmt(&fmtraw, &width, &count, &sign_extend, &is_bigendian);
      /* get & format the data */
      struct tcl_value *value = NULL;
      assert(count > 0);
      assert(width > 0);
      for (unsigned c = 0; c < count && dataidx + width <= datalen; c++) {
        if (is_bigendian) {
          /* convert Big Endian to Little Endian before moving to numeric value */
          binary_swapbytes(rawdata + dataidx, width);
        }
        tcl_int val = 0;
        /* for sign extension, look at the sign bit of the last byte (now that the
           bytes are stored in Little Endian) */
        if (sign_extend) {
          unsigned high = width - 1;
          if (rawdata[dataidx + high] & 0x80) {
            val = -1;
          }
        }
        memcpy(&val, rawdata + dataidx, width);
        dataidx += width;
        /* store data */
        char field[30];
        const char *fptr = tcl_int2string(field, sizeof field, 10, val);
        if (count == 1) {
          value = tcl_value(fptr, -1);
        } else {
          if (c == 0) {
            value = tcl_list_new();
          }
          tcl_list_append(value, tcl_value(fptr, -1));
        }
      }
      if (value) {
        tcl_var(tcl, varname, value);
        cvtcount++;
      }
    }
    _free(rawdata);
    r = tcl_numeric_result(tcl, cvtcount);
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_SUBCMD, tcl_data(subcmd));
  }
  tcl_free(subcmd);
  tcl_free(fmt);
  return r;
}
#endif

#if !defined TCL_DISABLE_CLOCK
#include <time.h>

static int tcl_cmd_clock(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int r = FERROR;
  struct tcl_value *subcmd = tcl_list_item(args, 1);
  if (SUBCMD(subcmd, "seconds")) {
    char buffer[20];
    sprintf(buffer, "%ld", (long)time(NULL));
    r = tcl_result(tcl, FNORMAL, tcl_value(buffer, -1));
  } else if (SUBCMD(subcmd, "format")) {
    int argcount = tcl_list_length(args);
    if (argcount < 3) {
      r = tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
    } else {
      struct tcl_value *v_tstamp = tcl_list_item(args, 2);
      time_t tstamp = (time_t)tcl_number(v_tstamp);
      tcl_free(v_tstamp);
      const struct tm *timeinfo = localtime(&tstamp);
      struct tcl_value *v_format = (tcl_list_length(args) > 3) ? tcl_list_item(args, 3) : NULL;
      const char *format = v_format ? tcl_data(v_format) : "%Y-%m-%d %H:%M:%S";
      char buffer[50];
      strftime(buffer, sizeof buffer, format, timeinfo);
      r = tcl_result(tcl, FNORMAL, tcl_value(buffer, -1));
      if (v_format) {
        tcl_free(v_format);
      }
    }
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_SUBCMD, tcl_data(subcmd));
  }
  tcl_free(subcmd);
  return r;
}
#endif

#if !defined TCL_DISABLE_SOURCE
static int tcl_cmd_source(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *path = tcl_list_item(args, 1);
# if defined _WIN32 || defined _WIN64
    char buffer[_MAX_PATH];
    strlcpy(buffer, tcl_data(path), sizeof buffer);
    char *p;
    while ((p = strchr(buffer, '/')) != NULL) {
      *p = '\\';
    }
    p = buffer;
# else
    const char *p = tcl_data(path);
# endif
  FILE *fp = fopen(p, "rt");
  if (!fp) {
    tcl_free(path);
    return tcl_error_result(tcl, TCL_ERROR_FILEIO, tcl_data(path));
  }
  tcl_free(path);
  /* get length of the file */
  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  /* allocate memory and read the file */
  char *script = _malloc(fsize + 1);
  if (!script) {
    return tcl_error_result(tcl, TCL_ERROR_MEMORY, "source");
  }
  memset(script, 0, fsize + 1);
  fread(script, 1, fsize, fp);
  fclose(fp);
  /* evaluate the script */
  int r = tcl_eval(tcl, script, strlen(script));
  _free(script);
  if (r == FRETURN) {
    r = FNORMAL;
  }
  return r;
}
#endif

#if !defined TCL_DISABLE_FILEIO
# if defined _WIN32 || defined _WIN64
#   include <io.h>
# else
#   define _S_IFDIR       S_IFDIR
#   define _S_IFREG       S_IFREG
#   include <unistd.h>
#   include <fcntl.h>
# endif
# include <sys/stat.h>

static FILE *tcl_stdhandle(const struct tcl_value *arg) {
  assert(arg);
  FILE *result = NULL;
  if (strcmp(tcl_data(arg), "stdin") == 0) {
    result = stdin;
  } else if (strcmp(tcl_data(arg), "stdout") == 0) {
    result = stdout;
  } else if (strcmp(tcl_data(arg), "stderr") == 0) {
    result = stderr;
  }
  return result;
}

static int tcl_cmd_open(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *path = tcl_list_item(args, 1);
  if (tcl_stdhandle(path)) {
    /* make it an error to open a standard handle */
    int r = tcl_error_result(tcl, TCL_ERROR_FILEIO, tcl_data(path));
    tcl_free(path);
    return r;
  }
# if defined _WIN32 || defined _WIN64
    char buffer[_MAX_PATH];
    strlcpy(buffer, tcl_data(path), sizeof buffer);
    char *p;
    while ((p = strchr(buffer, '/')) != NULL) {
      *p = '\\';
    }
    p = buffer;
# else
    const char *p = tcl_data(path);
# endif
  struct tcl_value *mode = (tcl_list_length(args) == 3) ? tcl_list_item(args, 2) : NULL;
  FILE *fp = fopen(p, mode ? tcl_data(mode) : "rt");
  int r = FNORMAL;
  if (fp) {
    r = tcl_numeric_result(tcl, (tcl_int)fp);
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_FILEIO, tcl_data(path));
  }
  tcl_free(path);
  if (mode) {
    tcl_free(mode);
  }
  return r;
}

static int tcl_cmd_close(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *fd = tcl_list_item(args, 1);
  int r = tcl_stdhandle(fd) ? EOF : fclose((FILE*)tcl_number(fd)); /* make it an error to close a standard handle */
  tcl_free(fd);
  return (r == 0) ? tcl_empty_result(tcl) : tcl_error_result(tcl, TCL_ERROR_FILEIO, "close");
}

static int tcl_cmd_flush(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *fd = tcl_list_item(args, 1);
  FILE *fp = tcl_stdhandle(fd) ? tcl_stdhandle(fd) : (FILE*)tcl_number(fd);
  tcl_free(fd);
  int r = fflush(fp);
  return (r == 0) ? tcl_empty_result(tcl) : tcl_error_result(tcl, TCL_ERROR_FILEIO, "flush");
}

static int tcl_cmd_seek(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *fd = tcl_list_item(args, 1);
  struct tcl_value *offset = tcl_list_item(args, 2);
  int org = SEEK_SET;
  if (tcl_list_length(args) == 4) {
    struct tcl_value *origin = tcl_list_item(args, 3);
    if (strcmp(tcl_data(origin), "current")) {
      org = SEEK_CUR;
    } else if (strcmp(tcl_data(origin), "end")) {
      org = SEEK_END;
    }
    tcl_free(origin);
  }
  FILE *fp = tcl_stdhandle(fd) ? tcl_stdhandle(fd) : (FILE*)tcl_number(fd);
  fseek(fp, (long)tcl_number(offset), org);
  tcl_free(fd);
  tcl_free(offset);
  return tcl_empty_result(tcl);
}

static int tcl_cmd_tell(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *fd = tcl_list_item(args, 1);
  FILE *fp = tcl_stdhandle(fd) ? tcl_stdhandle(fd) : (FILE*)tcl_number(fd);
  tcl_free(fd);
  long r = ftell(fp);
  return tcl_numeric_result(tcl, r);
}

static int tcl_cmd_eof(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *fd = tcl_list_item(args, 1);
  FILE *fp = tcl_stdhandle(fd) ? tcl_stdhandle(fd) : (FILE*)tcl_number(fd);
  tcl_free(fd);
  int r = feof(fp);
  return tcl_numeric_result(tcl, r);
}

static int tcl_cmd_read(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  struct tcl_value *fd = tcl_list_item(args, 1);
  long bytecount = LONG_MAX;
  if (tcl_list_length(args) == 3) {
    struct tcl_value *bytes = tcl_list_item(args, 2);
    bytecount = tcl_number(bytes);
    tcl_free(bytes);
  }
  FILE *fp = tcl_stdhandle(fd) ? tcl_stdhandle(fd) : (FILE*)tcl_number(fd);
  tcl_free(fd);
  if (fileno(fp) < 0) {
    return tcl_error_result(tcl, TCL_ERROR_FILEIO, "read");
  }
  /* check how many bytes to read (especially if no "bytecount" argument was given) */
  long pos = ftell(fp);
  fseek(fp, 0, SEEK_END);
  long remaining = ftell(fp) - pos;
  fseek(fp, pos, SEEK_SET);
  if (bytecount > remaining) {
    bytecount = remaining;
  }
  if (bytecount <= 0) {
    return tcl_error_result(tcl, TCL_ERROR_FILEIO, "negative bytecount argument");
  }
  char *buffer = _malloc(bytecount);
  if (!buffer) {
    return tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
  }
  size_t count = fread(buffer, 1, bytecount, fp);
  assert(count <= (unsigned)bytecount); /* can be smaller when \r\n is translated to \n */
  if (tcl_list_find(user, "-nonewline") >= 0 && count > 0 && buffer[count - 1] == '\n') {
    count--;
  }
  struct tcl_value *result = tcl_value(buffer, count);
  _free(buffer);
  return tcl_result(tcl, FNORMAL, result);
}

static int tcl_cmd_gets(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *fd = tcl_list_item(args, 1);
  FILE *fp = tcl_stdhandle(fd) ? tcl_stdhandle(fd) : (FILE*)tcl_number(fd);
  tcl_free(fd);
  if (fileno(fp) < 0) {
    return tcl_error_result(tcl, TCL_ERROR_FILEIO, "gets");
  }
  char buffer[1024];
  const char *p = fgets(buffer, sizeof(buffer), fp);
  if (!p) {
    return tcl_empty_result(tcl);
  }
  char *newline = strchr(buffer, '\n');
  if (newline) {
    *newline = '\0';  /* remove trailing newline */
  }
  struct tcl_value *result = tcl_value(buffer, -1);
  if (tcl_list_length(args) == 3) {
    struct tcl_value *varname = tcl_list_item(args, 2);
    tcl_var(tcl, tcl_data(varname), tcl_dup(result));
    tcl_free(varname);
  }
  return tcl_result(tcl, FNORMAL, result);
}

static int tcl_cmd_file(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int r = FERROR;
  struct tcl_value *subcmd = tcl_list_item(args, 1);
  struct tcl_value *path = tcl_list_item(args, 2);
# if defined _WIN32 || defined _WIN64
#   define DIRSEP '\\'
    char buffer[_MAX_PATH];
    strlcpy(buffer, tcl_data(path), sizeof buffer);
    char *pname;
    while ((pname = strchr(buffer, '/')) != NULL) {
      *pname = DIRSEP;
    }
    pname = buffer;
# else
#   define DIRSEP '/'
    const char *pname = tcl_data(path);
# endif
  if (SUBCMD(subcmd, "dirname")) {
    const char *p = strrchr(pname, DIRSEP);
    size_t len = 1;
    if (p) {
      if (p != pname) {
        len = p - pname;  /* there is a directory in the path, strip off the last '/' */
      }
    } else {
      p = ".";            /* there is no directory in the path, set a '.' */
    }
    r = tcl_result(tcl, FNORMAL, tcl_value(p, len));
  } else if (SUBCMD(subcmd, "exists")) {
    r = tcl_numeric_result(tcl, (access(pname, 0) == 0));
  } else if (SUBCMD(subcmd, "extension")) {
    const char *p = strrchr(pname, '.');
    if (p && !strchr(p, DIRSEP)) {
      r = tcl_result(tcl, FNORMAL, tcl_value(p, -1));
    } else {
      r = tcl_empty_result(tcl);
    }
  } else if (SUBCMD(subcmd, "isdirectory")) {
    struct stat st;
    int i = stat(pname, &st);
    r = tcl_numeric_result(tcl, (i == 0) ? (st.st_mode & _S_IFDIR) != 0 : 0);
  } else if (SUBCMD(subcmd, "isfile")) {
    struct stat st;
    int i = stat(pname, &st);
    r = tcl_numeric_result(tcl, (i == 0) ? (st.st_mode & _S_IFREG) != 0 : 0);
  } else if (SUBCMD(subcmd, "rootname")) {
    const char *p = strrchr(pname, '.');
    if (p && !strchr(p, DIRSEP)) {
      r = tcl_result(tcl, FNORMAL, tcl_value(pname, p - pname));
    } else {
      r = tcl_result(tcl, FNORMAL, tcl_value(pname, -1));
    }
  } else if (SUBCMD(subcmd, "size")) {
    struct stat st;
    int i = stat(pname, &st);
    r = tcl_numeric_result(tcl, (i == 0) ? st.st_size : 0);
  } else if (SUBCMD(subcmd, "tail")) {
    const char *p = strrchr(pname, DIRSEP);
    p = p ? p + 1 : pname;
    r = tcl_result(tcl, FNORMAL, tcl_value(p, -1));
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_SUBCMD, tcl_data(subcmd));
  }
  tcl_free(subcmd);
  tcl_free(path);
  return r;
# undef DIRSEP
}
#endif /* TCL_DISABLE_FILEIO */

#if !defined TCL_DISABLE_PUTS
static int tcl_cmd_puts(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  bool no_eol = (tcl_list_find(user, "-nonewline") >= 0);
  int argcount = tcl_list_length(args);
  struct tcl_value *text = tcl_list_item(args, argcount - 1);
  int r = FNORMAL;
  if (argcount == 3) {
#   if !defined TCL_DISABLE_FILEIO
      struct tcl_value *fd = tcl_list_item(args, 1);
      FILE *fp = tcl_stdhandle(fd) ? tcl_stdhandle(fd) : (FILE*)tcl_number(fd);
      if (fileno(fp) < 0) {
        r = tcl_error_result(tcl, TCL_ERROR_FILEIO, NULL);
      } else {
        fprintf(fp, "%s", tcl_data(text));
        if (!no_eol) {
          fprintf(fp, "\n");
        }
      }
      tcl_free(fd);
#   endif
  } else {
    printf("%s", tcl_data(text));
    if (!no_eol) {
      printf("\n");
    }
  }
  tcl_free(text);
  return (r == FNORMAL) ? tcl_empty_result(tcl) : r;
}
#endif

#if !defined TCL_DISABLE_EXEC
static int tcl_cmd_exec(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  /* calculate size for complete command & argument list */
  size_t needed = 0;
  int nargs = tcl_list_length(args);
  for (int i = 1; i < nargs; i++) {
    struct tcl_value *v = tcl_list_item(args, i);
    needed += tcl_length(v) + 1;
    tcl_free(v);
  }
  /* build command line */
  char *cmd = _malloc(needed);
  if (!cmd) {
    return tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
  }
  size_t pos = 0;
  for (int i = 1; i < nargs; i++) {
    struct tcl_value *v = tcl_list_item(args, i);
    size_t len = tcl_length(v);
    memcpy(cmd + pos, tcl_data(v), len);
    cmd[pos + len] = (i + 1 == nargs) ? '\0' : ' ';
    tcl_free(v);
    pos += len + 1;
  }
  /* prepare the buffer for captured output */
  size_t bufsize = 256;
  size_t buflen = 0;
  char *buf = _malloc(bufsize);
  if (!buf) {
    return tcl_error_result(tcl, TCL_ERROR_MEMORY, NULL);
  }
  /* execute command */
  int r = FNORMAL;
  FILE* fp = _popen(cmd, "r" );
  if (fp) {
    /* collect the output */
    while (!feof(fp)) {
      size_t n = fread(buf + buflen, 1, bufsize - buflen, fp);
      assert(n <= bufsize - buflen);
      buflen += n;
      if (buflen >= bufsize) {
        size_t newsize = 2 * bufsize;
        char *newbuf = _malloc(newsize);
        if (!newbuf) {
          break;
        }
        memcpy(newbuf, buf, buflen);
        _free(buf);
        buf = newbuf;
        bufsize = newsize;
      }
    }
    _pclose(fp);
    r = tcl_result(tcl, FNORMAL, tcl_value(buf, buflen));
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_FILEIO, cmd);
  }
  _free(buf);
  _free(cmd);
  return r;
}
#endif

static int tcl_user_proc(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  /* "user" argument is the entire "proc" definition, including the "proc"
     command, the procedure name, the argument list and the body */
  struct tcl_value *paramlist = tcl_list_item(user, 2);
  struct tcl_value *body = tcl_list_item(user, 3);
  tcl->env = tcl_env_alloc(tcl->env);
  /* copy arguments to parameters (local variables) */
  unsigned paramlist_len = tcl_list_length(paramlist);
  unsigned arglist_len = tcl_list_length(args) - 1; /* first "argument" is the command name, ignore it */
  if (arglist_len > paramlist_len) {
    /* this happens for a user procedure with variable arguments, ignore the
       last argument for now, but handle it separately after the fixed arguments */
    paramlist_len--;  /* this is the number of fixed arguments (which may be zero) */
  }
  for (unsigned i = 0; i < paramlist_len; i++) {
    struct tcl_value *param = tcl_list_item(paramlist, i);
    struct tcl_value *v;
    if (i < arglist_len) {
      /* argument is passed */
      v = tcl_list_item(args, i + 1);
    } else {
      /* argument is *not* passed, use default value */
      assert(tcl_list_length(param) == 2);  /* this should have been checked when registering the user procedure */
      v = tcl_list_item(param, 1);
    }
    if (tcl_list_length(param) > 1) {
      struct tcl_value *p = tcl_list_item(param, 0);
      tcl_free(param);
      param = p;
    }
    tcl_var(tcl, tcl_data(param), v);
    tcl_free(param);
  }
  if (arglist_len > paramlist_len + 1) {
#   if !defined NDEBUG
      struct tcl_value *param = tcl_list_item(paramlist, paramlist_len);
      assert(strcmp(tcl_data(param), "args") == 0);
      tcl_free(param);
#   endif
    struct tcl_value *lst = tcl_list_new();
    for (unsigned i = paramlist_len; i < arglist_len; i++) {
      struct tcl_value *v = tcl_list_item(args, i + 1);
      tcl_list_append(lst, v);
    }
    tcl_var(tcl, "args", lst);
  }
  int r = tcl_eval(tcl, tcl_data(body), tcl_length(body) + 1);
  if (r == FERROR) {
    size_t error_offs = tcl->env->errinfo.currentpos - tcl->env->errinfo.codebase;
    /* need to calculate the offset of the body relative to the start of the
       proc declaration */
    const char *body_ptr;
    size_t body_sz;
    tcl_list_item_ptr(user, 3, &body_ptr, &body_sz);
    error_offs += body_ptr - tcl_data(user);
    /* need to find the proc again */
    struct tcl_value *cmdname = tcl_list_item(user, 1);
    struct tcl_cmd *cmd = tcl_lookup_cmd(tcl, cmdname);
    tcl_free(cmdname);
    assert(cmd);  /* it just ran, so it must be found */
    struct tcl_env *global_env = tcl_env_scope(tcl, 0);
    global_env->errinfo.currentpos = cmd->declpos + error_offs;
  }
  tcl->env = tcl_env_free(tcl->env);
  tcl_free(paramlist);
  tcl_free(body);
  assert(r != FBREAK && r != FCONT);
  return (r == FRETURN) ? FNORMAL : r;
}

static int tcl_cmd_proc(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int r = FNORMAL;
  struct tcl_value *name = tcl_list_item(args, 1);
  struct tcl_value *paramlist = tcl_list_item(args, 2);
  short paramcount = 0;
  short defaultcount = 0;     /* check whether parameters have default values */
  bool is_varargs = false;    /* check for variable number of arguments */
  unsigned paramlist_len = tcl_list_length(paramlist);
  for (unsigned i = 0; i < paramlist_len; i++) {
    struct tcl_value *param = tcl_list_item(paramlist, i);
    int len = tcl_list_length(param);
    assert(len >= 0);
    if (len == 0 || is_varargs) {
      r = tcl_error_result(tcl, TCL_ERROR_NAMEINVALID, tcl_data(param)); /* invalid name */
    } else if ((len == 1 && defaultcount > 0) || len > 2) {
      r = tcl_error_result(tcl, TCL_ERROR_DEFAULTVAL, tcl_data(param));  /* parameters with default values must come behind parameters without defaults */
    } else if (len == 2) {
      defaultcount++;
    } else {
      paramcount++;
    }
    if (strcmp(tcl_data(param), "args") == 0) {
      is_varargs = true;
      if (len != 1) {
        r = tcl_error_result(tcl, TCL_ERROR_DEFAULTVAL, tcl_data(param));/* special parameter "args" may not have a default value */
      }
    }
    tcl_free(param);
  }
  short max_params = is_varargs ? -1 : paramcount + defaultcount;
  struct tcl_cmd *cmd = tcl_register(tcl, tcl_data(name), tcl_user_proc, 0, paramcount, max_params, tcl_dup(args));
  tcl_free(name);
  tcl_free(paramlist);
  struct tcl_env *global_env = tcl_env_scope(tcl, 0);
  cmd->declpos = global_env->errinfo.currentpos;
  return r == FERROR ? r : tcl_empty_result(tcl);
}

static struct tcl_value *tcl_make_condition_list(struct tcl_value *cond) {
  assert(cond);
  /* add the implied "expr" in front of the condition, except if either:
     - the condition is enveloped between [...] (so it is already evaluated)
     - the condition is a number (so there is nothing to evaluate) */
  if (tcl_isnumber(cond)) {
    return cond;
  } else {
    const char *data = tcl_data(cond);
    size_t len = tcl_length(cond);
    if (len >= 2 && *data == '[' && *(data + len - 1) == ']') {
      size_t i;
      for (i = 1; i < len - 1 && data[i] != '[' && data[i] != ']' && data[i] != '\\'; i++)
        {}
      if (i == len - 1) {
        return cond;  /* expression between [...] and no nested [...] or escapes */
      }
    }
  }
  struct tcl_value *list = tcl_list_new();
  tcl_list_append(list, tcl_value("expr", 4));
  tcl_list_append(list, cond);
  return list;
}

int tcl_eval_condition(struct tcl *tcl, struct tcl_value *cond) {
  assert(cond);
  const char *data = tcl_data(cond);
  size_t len = tcl_length(cond);
  int r = FNORMAL;
  if ((len >= 2 && *data == '[' && *(data + len - 1) == ']')) {
    r = tcl_subst(tcl, data, len);
  } else if (tcl_isnumber(cond)) {
    r = tcl_numeric_result(tcl, tcl_number(cond));
  } else {
    r = tcl_eval(tcl, tcl_data(cond), tcl_length(cond) + 1);
  }
  return r;
}

static int tcl_cmd_if(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int i = 1;
  int n = tcl_list_length(args);
  int r = tcl_empty_result(tcl);
  while (i < n) {
    struct tcl_value *cond = tcl_make_condition_list(tcl_list_item(args, i++));
    struct tcl_value *branch = (i < n) ? tcl_list_item(args, i++) : NULL;
    if (strcmp(tcl_data(branch), "then") == 0) {
      tcl_free(branch); /* ignore optional keyword "then", get next branch */
      branch = (i < n) ? tcl_list_item(args, i++) : NULL;
    }
    r = tcl_eval_condition(tcl, cond);
    tcl_free(cond);
    if (r != FNORMAL) {
      tcl_free(branch);   /* error in condition expression, abort */
      break;
    } else if (!branch) {
      return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
    }
    if (tcl_number(tcl->result)) {
      r = tcl_eval(tcl, tcl_data(branch), tcl_length(branch) + 1);
      tcl_free(branch);
      break;              /* branch taken, do not take any other branch */
    }
    tcl_free(branch);
    /* "then" branch not taken, check how to continue, first check for keyword */
    if (i < n) {
      branch = tcl_list_item(args, i);
      if (strcmp(tcl_data(branch), "elseif") == 0) {
        tcl_free(branch);
        i++;              /* skip the keyword (then drop back into the loop,
                             parsing the next condition) */
      } else if (strcmp(tcl_data(branch), "else") == 0) {
        tcl_free(branch);
        i++;              /* skip the keyword */
        if (i < n) {
          branch = tcl_list_item(args, i++);
          r = tcl_eval(tcl, tcl_data(branch), tcl_length(branch) + 1);
          tcl_free(branch);
          break;          /* "else" branch taken, do not take any other branch */
        } else {
          return tcl_error_result(tcl, TCL_ERROR_ARGUMENT, NULL);
        }
      } else if (i + 1 < n) {
        /* no explicit keyword, but at least two blocks in the list:
           assume it is {cond} {branch} (implied elseif) */
        tcl_free(branch);
      } else {
        /* last block: must be (implied) else */
        i++;
        r = tcl_eval(tcl, tcl_data(branch), tcl_length(branch) + 1);
        tcl_free(branch);
      }
    }
  }
  return r;
}

static int tcl_cmd_switch(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  bool exact_match = (tcl_list_find(user, "-exact") >= 0);
  int nargs = tcl_list_length(args);
  int r = tcl_empty_result(tcl);
  struct tcl_value *crit = tcl_list_item(args, 1);
  /* there are two forms of switch: all pairs in a list, or all pairs simply
     appended onto the tail of the command */
  const struct tcl_value *list;
  int list_idx, list_len;
  if (nargs == 3) {
    list = tcl_list_item(args, 2);
    list_idx = 0;
    list_len = tcl_list_length(list);
  } else {
    list = args;
    list_idx = 2;
    list_len = nargs;
  }
  /* find a match */
  while (list_idx < list_len) {
    struct tcl_value *pattern = tcl_list_item(list, list_idx);
    bool match = (strcmp(tcl_data(pattern), "default") == 0 ||
                  tcl_match(exact_match, tcl_data(pattern), tcl_data(crit)));
    tcl_free(pattern);
    if (match) {
      break;
    }
    list_idx += 2;  /* skip pattern & body pair */
  }
  /* find body */
  struct tcl_value *body = NULL;
  list_idx += 1;
  while (list_idx < list_len) {
    body = tcl_list_item(list, list_idx);
    if (strcmp(tcl_data(body), "-") != 0)
      break;
    body = tcl_free(body);
    list_idx += 2;  /* body, plus pattern of the next case */
  }
  /* clean up values that are no longer needed */
  tcl_free(crit);
  if (nargs == 3) {
    tcl_free((struct tcl_value*)list);
  }
  /* execute the body */
  if (body) {
    r = tcl_eval(tcl, tcl_data(body), tcl_length(body) + 1);
    tcl_free(body);
  }
  return r;
}

static int tcl_cmd_while(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  assert(tcl_list_length(args) == 3);
  struct tcl_value *cond = tcl_make_condition_list(tcl_list_item(args, 1));
  struct tcl_value *body = tcl_list_item(args, 2);
  int r = FNORMAL;
  for (;;) {
    r = tcl_eval_condition(tcl, cond);
    if (r != FNORMAL) {
      break;
    }
    if (!tcl_number(tcl->result)) {
      r = FNORMAL;
      break;
    }
    r = tcl_eval(tcl, tcl_data(body), tcl_length(body) + 1);
    if (r != FCONT && r != FNORMAL) {
      assert(r == FBREAK || r == FRETURN || r == FEXIT || r == FERROR);
      if (r == FBREAK) {
        r = FNORMAL;
      }
      break;
    }
  }
  tcl_free(cond);
  tcl_free(body);
  return r;
}

static int tcl_cmd_for(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  assert(tcl_list_length(args) == 5);
  struct tcl_value *setup = tcl_list_item(args, 1);
  int r = tcl_eval(tcl, tcl_data(setup), tcl_length(setup) + 1);
  tcl_free(setup);
  if (r != FNORMAL) {
    return r;
  }
  struct tcl_value *cond = tcl_make_condition_list(tcl_list_item(args, 2));
  struct tcl_value *post = tcl_list_item(args, 3);
  struct tcl_value *body = tcl_list_item(args, 4);
  for (;;) {
    r = tcl_eval_condition(tcl, cond);
    if (r != FNORMAL) {
      break;
    }
    if (!tcl_number(tcl->result)) {
      r = FNORMAL;  /* condition failed, drop out of loop */
      break;
    }
    r = tcl_eval(tcl, tcl_data(body), tcl_length(body) + 1);
    if (r != FCONT && r != FNORMAL) {
      assert(r == FBREAK || r == FRETURN || r == FEXIT || r == FERROR);
      if (r == FBREAK) {
        r = FNORMAL;
      }
      break;
    }
    r = tcl_eval(tcl, tcl_data(post), tcl_length(post) + 1);
    if (r != FNORMAL) {
      break;
    }
  }
  tcl_free(cond);
  tcl_free(post);
  tcl_free(body);
  return r;
}

static int tcl_cmd_foreach(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  assert(tcl_list_length(args) == 4);
  struct tcl_value *name = tcl_list_item(args, 1);
  struct tcl_value *list = tcl_list_item(args, 2);
  struct tcl_value *body = tcl_list_item(args, 3);
  int r = FNORMAL;
  int n = tcl_list_length(list);
  for (int i = 0; i < n; i++) {
    tcl_var(tcl, tcl_data(name), tcl_list_item(list, i));
    r = tcl_eval(tcl, tcl_data(body), tcl_length(body) + 1);
    if (r != FCONT && r != FNORMAL) {
      assert(r == FBREAK || r == FRETURN || r == FEXIT || r == FERROR);
      if (r == FBREAK) {
        r = FNORMAL;
      }
      break;
    }
  }
  tcl_free(name);
  tcl_free(list);
  tcl_free(body);
  return r;
}

static int tcl_cmd_flow(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  int r = FERROR;
  struct tcl_value *flowval = tcl_list_item(args, 0);
  const char *flow = tcl_data(flowval);
  if (strcmp(flow, "break") == 0) {
    r = FBREAK;
  } else if (strcmp(flow, "continue") == 0) {
    r = FCONT;
  } else if (strcmp(flow, "return") == 0) {
    r = tcl_result(tcl, FRETURN,
                   (tcl_list_length(args) == 2) ? tcl_list_item(args, 1) : tcl_value("", 0));
  } else if (strcmp(flow, "exit") == 0) {
    r = tcl_result(tcl, FEXIT,
                   (tcl_list_length(args) == 2) ? tcl_list_item(args, 1) : tcl_value("", 0));
  }
  tcl_free(flowval);
  return r;
}

static int tcl_cmd_catch(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *body = tcl_list_item(args, 1);
  int r = tcl_eval(tcl, tcl_data(body), tcl_length(body) + 1);
  if (r == FEXIT) {
    r = FRETURN;
  }
  if (tcl_list_length(args) == 3) {
    struct tcl_value *val = tcl->errinfo ? tcl_dup(tcl->errinfo) : tcl_value("", 0);
    struct tcl_value *name = tcl_list_item(args, 2);
    tcl_var(tcl, tcl_data(name), val);
    tcl_free(name);
  }
  tcl_free(body);
  return tcl_numeric_result(tcl, r);
}

static int tcl_cmd_error(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  struct tcl_value *msg = tcl_list_item(args, 1);
  tcl_error_result(tcl, TCL_ERROR_USER, tcl_data(msg));
  tcl_free(msg);
  return FERROR;
}

/* -------------------------------------------------------------------------- */

enum {
  TOK_END_EXPR = 0,
  TOK_NUMBER = 256, /* literal number */
  TOK_STRING,       /* literal string (may contain escapes that need to be resolved) */
  TOK_BSTRING,      /* brace-quoted string */
  TOK_VARIABLE,     /* variable name plus optional index (for arrays) */
  TOK_OR,           /* || */
  TOK_AND,          /* && */
  TOK_EQ,           /* == */
  TOK_NE,           /* != */
  TOK_GE,           /* >= */
  TOK_LE,           /* <= */
  TOK_SHL,          /* << */
  TOK_SHR,          /* >> */
  TOK_EXP,          /* ** */
};

enum {
  eNONE = 0,        /* no error */
  eNUM_EXPECT,      /* number expected */
  eINVALID_NUM,     /* invalid number syntax */
  ePARENTHESES,     /* unbalanced parentheses */
  eEXTRA_CHARS,     /* extra characters after expression (missing operator?) */
  eINVALID_CHAR,
  eDIV0,            /* divide by zero */
  eMEMORY,          /* memory allocation failure */
};

struct exprval {
  tcl_int i;        /* literal value (integer) */
  char *pc;         /* literal string */
};

struct expr {
  const char *pos;  /* current position in expression */
  struct tcl *tcl;  /* for variable lookup */
  int error;
  int token;        /* current token */
  struct exprval val;/* copy of the most recent literal value */
  bool pushed;      /* push-back of most recently read token */
};

static void expr_error(struct expr *expr, int number);
static struct exprval expr_conditional(struct expr *expr);
#define lex(e)          ((e)->pushed ? ((e)->pushed = false, (e)->token) : expr_lex(e) )
#define unlex(e)        ((e)->pushed = true)

static void exprval_clear(struct exprval *value) {
  assert(value);
  if (value->pc) {
    _free(value->pc);
    value->pc = NULL;
  }
}

static struct exprval exprval_set(tcl_int i, const char *pc, int len)
{
  struct exprval value;
  value.i = i;
  if (pc) {
    if (len < 0) {
      len = strlen(pc);
    }
    value.pc = _malloc(len + 1);
    if (value.pc) {
      memcpy(value.pc, pc, len);
      value.pc[len] = '\0';
    }
  } else {
    value.pc = NULL;
  }
  return value;
}

static bool exprval_check_num(struct expr *expr, struct exprval *value) {
  /* checks that value contains a number, and raises an error if not;
     if there is an error (i.e. value is a string), the string is cleared (and
     if there is no error, there is nothing to clear) */
  assert(expr);
  assert(value);
  if (value->pc) {
    exprval_clear(value);
    expr_error(expr, eNUM_EXPECT);
    return false;
  }
  return true;
}

static struct exprval exprval_set_num(struct expr *expr, struct exprval *value, tcl_int i) {
  exprval_check_num(expr, value);
  return exprval_set(i, NULL, 0);
}

static void expr_error(struct expr *expr, int number) {
  if (expr->error == eNONE)
    expr->error = number;
  assert(expr->pos != NULL);
  while (*expr->pos != '\0')
    expr->pos += 1; /* skip rest of string, to forcibly end parsing */
}

static void expr_skip(struct expr *expr, int number) {
  while (*expr->pos != '\0' && number-- > 0)
    expr->pos++;
  while (*expr->pos != '\0' && *expr->pos <= ' ')
    expr->pos++;
}

static int expr_lex(struct expr *expr) {
  static const char special[] = "?:|&^~<>=!-+*/%(){}";

  assert(expr);
  exprval_clear(&expr->val);
  assert(expr->pos);
  if (*expr->pos == '\0') {
    expr->token = TOK_END_EXPR;
    return expr->token;
  }

  if (strchr(special, *expr->pos)) {
    expr->token = (int)*expr->pos;  /* preset */
    expr->pos += 1; /* don't skip whitespace yet, first check for multi-character operators */
    switch (expr->token) {
    case '|':
      if (*expr->pos == '|') {
        expr->token = TOK_OR;
        expr->pos += 1;
      }
      break;
    case '&':
      if (*expr->pos == '&') {
        expr->token = TOK_AND;
        expr->pos += 1;
      }
      break;
    case '=':
      if (*expr->pos == '=') {
        expr->token = TOK_EQ;
        expr->pos += 1;
      }
      break;
    case '!':
      if (*expr->pos == '=') {
        expr->token = TOK_NE;
        expr->pos += 1;
      }
      break;
    case '<':
      if (*expr->pos == '=') {
        expr->token = TOK_LE;
        expr->pos += 1;
      } else if (*expr->pos == '<') {
        expr->token = TOK_SHL;
        expr->pos += 1;
      }
      break;
    case '>':
      if (*expr->pos == '=') {
        expr->token = TOK_GE;
        expr->pos += 1;
      } else if (*expr->pos == '>') {
        expr->token = TOK_SHR;
        expr->pos += 1;
      }
      break;
    case '*':
      if (*expr->pos == '*') {
        expr->token = TOK_EXP;
        expr->pos += 1;
      }
      break;
    case '{': {
      /* either a brace-quoted string, or the opening brace of a subexpression */
      const char *from = expr->pos;
      const char *to = from;
      while (!tcl_is_special(*to, false) && !tcl_is_operator(*to)) {
        to += 1;
      }
      if (*to == '}') {
        expr->val = exprval_set(0, from, to - from);
        expr->token = TOK_BSTRING;
        expr->pos = to + 1; /* skip '}' */
      }
      break;
    } /* case '{' */
    }
    expr_skip(expr, 0);          /* erase white space */
  } else if (tcl_isdigit(*expr->pos)) {
    char *ptr;
    expr->val = exprval_set(strtoll(expr->pos, &ptr, 0), NULL, 0);
    expr->pos = ptr;
    expr->token = TOK_NUMBER;
    if (tcl_isalpha(*expr->pos) || *expr->pos == '.' || *expr->pos == ',')
      expr_error(expr, eINVALID_NUM);
    expr_skip(expr, 0);          /* erase white space */
  } else if (*expr->pos == '$') {
    const char *from = expr->pos;
    expr->pos += 1;   /* skip '$' */
    char quote = (*expr->pos == '{') ? '}' : '\0';
    if (quote) {
      expr->pos += 1; /* skip '{' */
    }
    while (*expr->pos != quote && *expr->pos != '(' && *expr->pos != ')'
           && (quote || !(tcl_is_space(*expr->pos) || tcl_is_operator(*expr->pos)))
           && !tcl_is_special(*expr->pos, false)) {
      expr->pos++;
    }
    if (quote && *expr->pos == quote) {
      expr->pos += 1; /* skip '}' */
    }
    if (*expr->pos == '(') {
      while (*expr->pos != ')' && !tcl_is_special(*expr->pos, false)) {
        expr->pos += 1;
      }
      if (*expr->pos == ')') {
        expr->pos += 1;
      }
    }
    expr->val = exprval_set(0, from, expr->pos - from);
    expr->token = TOK_VARIABLE;
    expr_skip(expr, 0);          /* skip white space */
  } else {
    char quote = (*expr->pos == '"') ? '"' : '\0';
    if (quote) {
      expr->pos += 1; /* skip '"' */
    }
    const char *from = expr->pos;
    while (*expr->pos != quote && *expr->pos != '\0'
           && (quote || !tcl_is_space(*expr->pos))) {
      if (*expr->pos == '\\' && (*(expr->pos + 1) == '"' || *(expr->pos + 1) == '\\')) {
        expr->pos += 1; /* skip both \ & " */
      }
      expr->pos += 1;
    }
    expr->val = exprval_set(0, from, expr->pos - from);
    expr->token = TOK_STRING;
    if (quote && *expr->pos == quote) {
      expr->pos += 1; /* skip closing '"' */
    }
    expr_skip(expr, 0);          /* skip white space */
  }
  return expr->token;
}

static struct exprval expr_primary(struct expr *expr) {
  struct exprval v;
  int tok_close = 0;
  switch (lex(expr)) {
  case '-':
    v = expr_primary(expr);
    v = exprval_set_num(expr, &v, -v.i);
    break;
  case '+':
    v = expr_primary(expr);
    break;
  case '!':
    v = expr_primary(expr);
    v = exprval_set_num(expr, &v, !v.i);
    break;
  case '~':
    v = expr_primary(expr);
    v = exprval_set_num(expr, &v, ~v.i);
    break;
  case '(':
  case '{':
    tok_close = (expr->token == '(') ? ')' : '}';
    v = expr_conditional(expr);
    if (lex(expr) != tok_close)
      expr_error(expr, ePARENTHESES);
    break;
  case TOK_NUMBER:
    v = expr->val;
    break;
  case TOK_VARIABLE:
  case TOK_STRING:
    tcl_subst(expr->tcl, expr->val.pc, strlen(expr->val.pc));
    if (tcl_isnumber(expr->tcl->result)) {
      v = exprval_set(tcl_number(expr->tcl->result), NULL, 0);
    } else {
      v = exprval_set(0, tcl_data(expr->tcl->result), tcl_length(expr->tcl->result));
    }
    break;
  case TOK_BSTRING:
    v = exprval_set(0, expr->val.pc, -1); /* make a copy, because expr->val is reset */
    break;
  default:
    expr_error(expr, eNUM_EXPECT);
    v = exprval_set(0, NULL, 0);
  }
  return v;
}

static struct exprval expr_power(struct expr *expr) {
  struct exprval v1 = expr_primary(expr);
  while (lex(expr) == TOK_EXP) {
    struct exprval v2 = expr_power(expr); /* right-to-left associativity */
    exprval_check_num(expr, &v2);
    if (v2.i < 0) {
      v1 = exprval_set_num(expr, &v1, 0);
    } else {
      tcl_int n = v1.i;
      tcl_int r = 1;
      while (v2.i--)
        r *= n;
      v1 = exprval_set_num(expr, &v1, r);
    }
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_product(struct expr *expr) {
  struct exprval v1 = expr_power(expr);
  int op;
  while ((op = lex(expr)) == '*' || op == '/' || op == '%') {
    struct exprval v2 = expr_power(expr);
    exprval_check_num(expr, &v2);
    if (op == '*') {
      v1 = exprval_set_num(expr, &v1, v1.i * v2.i);
    } else {
      if (v2.i != 0L) {
        tcl_int div = v1.i / v2.i;
        tcl_int rem = v1.i % v2.i;
        /* ensure floored division, with positive remainder */
        if (rem < 0) {
          div -= 1;
          rem += v1.i;
        }
        v1 = exprval_set_num(expr, &v1, (op == '/') ? div : rem);
      } else {
        expr_error(expr, eDIV0);
        exprval_clear(&v1);
      }
    }
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_sum(struct expr *expr) {
  struct exprval v1 = expr_product(expr);
  int op;
  while ((op = lex(expr)) == '+' || op == '-') {
    struct exprval v2 = expr_product(expr);
    exprval_check_num(expr, &v2);
    v1 = exprval_set_num(expr, &v1, (op == '+') ? v1.i + v2.i : v1.i - v2.i);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_shift(struct expr *expr) {
  struct exprval v1 = expr_sum(expr);
  int op;
  while ((op = lex(expr)) == TOK_SHL || op == TOK_SHR) {
    struct exprval v2 = expr_sum(expr);
    exprval_check_num(expr, &v2);
    v1 = exprval_set_num(expr, &v1, (op == TOK_SHL) ? v1.i << v2.i : v1.i >> v2.i);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_relational(struct expr *expr) {
  struct exprval v1 = expr_shift(expr);
  int op;
  while ((op = lex(expr)) == '<' || op == '>' || op == TOK_LE || op == TOK_GE) {
    struct exprval v2 = expr_shift(expr);
    tcl_int r;
    if (!v1.pc && !v2.pc) {
      /* numeric compare */
      r = v1.i - v2.i;
    } else {
      /* string compare */
      char buf1[64], buf2[64];
      char *p1 = (v1.pc) ? v1.pc : tcl_int2string(buf1, sizeof(buf1), 10, v1.i);
      char *p2 = (v2.pc) ? v2.pc : tcl_int2string(buf2, sizeof(buf2), 10, v2.i);
      r = strcmp(p1, p2);
    }
    switch (op) {
    case '<':
      v1 = exprval_set_num(expr, &v1, (r < 0));
      break;
    case '>':
      v1 = exprval_set_num(expr, &v1, (r > 0));
      break;
    case TOK_LE:
      v1 = exprval_set_num(expr, &v1, (r <= 0));
      break;
    case TOK_GE:
      v1 = exprval_set_num(expr, &v1, (r >= 0));
      break;
    }
    exprval_clear(&v2);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_equality(struct expr *expr) {
  struct exprval v1 = expr_relational(expr);
  int op;
  while ((op = lex(expr)) == TOK_EQ || op == TOK_NE) {
    struct exprval v2 = expr_relational(expr);
    tcl_int r;
    if (!v1.pc && !v2.pc) {
      /* numeric compare */
      r = v1.i - v2.i;
    } else {
      /* string compare */
      char buf1[64], buf2[64];
      char *p1 = (v1.pc) ? v1.pc : tcl_int2string(buf1, sizeof(buf1), 10, v1.i);
      char *p2 = (v2.pc) ? v2.pc : tcl_int2string(buf2, sizeof(buf2), 10, v2.i);
      r = strcmp(p1, p2);
      exprval_clear(&v1); /* clear value, because we switch from string to numeric */
    }
    v1 = exprval_set_num(expr, &v1, (op == TOK_EQ) ? (r == 0) : (r != 0));
    exprval_clear(&v2);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_binary_and(struct expr *expr) {
  struct exprval v1 = expr_equality(expr);
  while (lex(expr) == '&') {
    struct exprval v2 = expr_equality(expr);
    exprval_check_num(expr, &v2);
    v1 = exprval_set_num(expr, &v1, v1.i & v2.i);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_binary_xor(struct expr *expr) {
  struct exprval v1 = expr_binary_and(expr);
  while (lex(expr) == '^') {
    struct exprval v2 = expr_binary_and(expr);
    exprval_check_num(expr, &v2);
    v1 = exprval_set_num(expr, &v1, v1.i ^ v2.i);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_binary_or(struct expr *expr) {
  struct exprval v1 = expr_binary_xor(expr);
  while (lex(expr) == '|') {
    struct exprval v2 = expr_binary_xor(expr);
    exprval_check_num(expr, &v2);
    v1 = exprval_set_num(expr, &v1, v1.i | v2.i);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_logic_and(struct expr *expr) {
  struct exprval v1 = expr_binary_or(expr);
  while (lex(expr) == TOK_AND) {
    struct exprval v2 = expr_binary_or(expr);
    exprval_check_num(expr, &v2);
    v1 = exprval_set_num(expr, &v1, v1.i && v2.i);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_logic_or(struct expr *expr) {
  struct exprval v1 = expr_logic_and(expr);
  while (lex(expr) == TOK_OR) {
    struct exprval v2 = expr_logic_and(expr);
    exprval_check_num(expr, &v2);
    v1 = exprval_set_num(expr, &v1, v1.i || v2.i);
  }
  unlex(expr);
  return v1;
}

static struct exprval expr_conditional(struct expr *expr) {
  struct exprval v1 = expr_logic_or(expr);
  if (lex(expr) == '?') {
    exprval_check_num(expr, &v1);
    struct exprval v2 = expr_conditional(expr);
    if (lex(expr) != ':')
      expr_error(expr, eINVALID_CHAR);
    struct exprval v3 = expr_logic_or(expr);
    if (v1.i) {
      v1 = v2;
      exprval_clear(&v3);
    } else {
      v1 = v3;
      exprval_clear(&v2);
    }
  }
  unlex(expr);
  return v1;
}

static int tcl_expression(struct tcl *tcl, const char *expression, struct exprval *result) {
  struct expr expr;
  memset(&expr, 0, sizeof(expr));
  expr.pos = expression;
  expr.tcl = tcl;
  expr_skip(&expr, 0);            /* erase leading white space */
  *result = expr_conditional(&expr);
  expr_skip(&expr, 0);            /* erase trailing white space */
  exprval_clear(&expr.val);
  if (expr.error == eNONE) {
    int op = lex(&expr);
    if (op == ')')
      expr_error(&expr, ePARENTHESES);
    else if (op != TOK_END_EXPR)
      expr_error(&expr, eEXTRA_CHARS);
  }
  return expr.error;
}

static char *tcl_int2string(char *buffer, size_t bufsz, int radix, tcl_int value) {
  assert(buffer);
  assert(radix == 10 || radix == 16);
  char *p = buffer + bufsz - 1;
  *p-- = '\0';
  if (radix == 16) {
    do {
      unsigned char c = (value & 0xf);
      *p-- = (c < 10) ? '0' + c : 'A' + (c - 10);
      value = value >> 4;
    } while (value > 0);
  } else {
    bool neg = (value < 0);
    if (neg) {
      value = -value;
    }
    do {
      *p-- = '0' + (value % 10);
      value = value / 10;
    } while (value > 0);
    if (neg) {
      *p-- = '-';
    }
  }
  return p + 1;
}

static int tcl_cmd_expr(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user) {
  (void)user;
  /* re-construct the expression (it may have been tokenized by the Tcl Lexer) */
  struct tcl_value *expression;
  int count = tcl_list_length(args);
  if (count == 2) {
    expression = tcl_list_item(args, 1);
  } else {
    size_t size = tcl_length(args);
    assert(strncmp(tcl_data(args), "expr", 4) == 0);
    expression = tcl_value(tcl_data(args) + 4, size - 4);  /* "expr" is 4 characters */
  }
  int r = FNORMAL;
  /* nested square brackets must be evaluated before proceeding with the expression */
  for (;;) {
    const char *open = tcl_data(expression);
    while (*open != '\0' && *open != '[') {
      open++;
    }
    if (*open != '[') {
      break;
    }
    const char *close = open + 1;
    int depth = 1;
    while (*close != '\0') {
      if (*close == ']') {
        if (--depth == 0) {
          break;
        }
      } else if (*close == '[') {
        depth++;
      }
      close++;
    }
    if (depth == 0) {
      assert(*close == ']');
      /* split the expression string up in 3 blocks, evaluate the middle part */
      struct tcl_value *prefix = tcl_value(tcl_data(expression), open - tcl_data(expression));
      struct tcl_value *suffix = tcl_value(close + 1, tcl_length(expression) - ((close + 1) - tcl_data(expression)));
      r = tcl_eval(tcl, open + 1, close - open - 1);
      /* build the new expression */
      tcl_free(expression);
      expression = prefix;
      tcl_append(expression, tcl_dup(tcl->result));
      tcl_append(expression, suffix);
    }
  }
  /* parse expression */
  struct exprval result;
  int err = tcl_expression(tcl, tcl_data(expression), &result);
  if (err == eNONE) {
    if (result.pc) {
      r = tcl_result(tcl, FNORMAL, tcl_value(result.pc, -1));
    } else {
      tcl_numeric_result(tcl, result.i);
    }
  } else {
    r = tcl_error_result(tcl, TCL_ERROR_EXPR, tcl_data(expression));
  }
  tcl_free(expression);
  exprval_clear(&result);

  /* convert result to string & store */
  return r;
}

/* -------------------------------------------------------------------------- */

void tcl_init(struct tcl *tcl, const void *user) {
  init_ctype();
  assert(tcl);
  memset(tcl, 0, sizeof(struct tcl));
  tcl->env = tcl_env_alloc(NULL);
  tcl->result = tcl_value("", 0);
  tcl->user = user;

  /* built-in commands */
  tcl_register(tcl, "append", tcl_cmd_append, 0, 2, -1, NULL);
  tcl_register(tcl, "array", tcl_cmd_array, 1, 1, 3, NULL);
  tcl_register(tcl, "break", tcl_cmd_flow, 0, 0, 0, NULL);
  tcl_register(tcl, "catch", tcl_cmd_catch, 0, 1, 2, NULL);
  tcl_register(tcl, "concat", tcl_cmd_concat, 0, 1, -1, NULL);
  tcl_register(tcl, "continue", tcl_cmd_flow, 0, 0, 0, NULL);
  tcl_register(tcl, "error", tcl_cmd_error, 0, 1, 1, NULL);
  tcl_register(tcl, "exit", tcl_cmd_flow, 0, 0, 1, NULL);
  tcl_register(tcl, "expr", tcl_cmd_expr, 0, 1, -1, NULL);
  tcl_register(tcl, "for", tcl_cmd_for, 0, 4, 4, NULL);
  tcl_register(tcl, "foreach", tcl_cmd_foreach, 0, 3, 3, NULL);
  tcl_register(tcl, "format", tcl_cmd_format, 0, 1, -1, NULL);
  tcl_register(tcl, "global", tcl_cmd_global, 0, 1, -1, NULL);
  tcl_register(tcl, "if", tcl_cmd_if, 0, 2, -1, NULL);
  tcl_register(tcl, "incr", tcl_cmd_incr, 0, 1, 2, NULL);
  tcl_register(tcl, "info", tcl_cmd_info, 1, 0, 1, NULL);
  tcl_register(tcl, "join", tcl_cmd_join, 0, 1, 2, NULL);
  tcl_register(tcl, "lappend", tcl_cmd_lappend, 0, 2, -1, NULL);
  tcl_register(tcl, "list", tcl_cmd_list, 0, 0, -1, NULL);
  tcl_register(tcl, "lindex", tcl_cmd_lindex, 0, 2, 2, NULL);
  tcl_register(tcl, "linsert", tcl_cmd_linsert, 0, 3, -1, NULL);
  tcl_register(tcl, "llength", tcl_cmd_llength, 0, 1, 1, NULL);
  tcl_register(tcl, "lrange", tcl_cmd_lrange, 0, 3, 3, NULL);
  tcl_register(tcl, "lreplace", tcl_cmd_lreplace, 0, 3, -1, NULL);
  tcl_register(tcl, "lsearch", tcl_cmd_lsearch, 0, 2, 2, tcl_value("-exact -glob", -1));
  tcl_register(tcl, "lsort", tcl_cmd_lsort, 0, 1, 1, tcl_value("-ascii -integer -increasing -decreasing", -1));
  tcl_register(tcl, "proc", tcl_cmd_proc, 0, 3, 3, NULL);
  tcl_register(tcl, "return", tcl_cmd_flow, 0, 0, 1, NULL);
  tcl_register(tcl, "scan", tcl_cmd_scan, 0, 2, -1, NULL);
  tcl_register(tcl, "set", tcl_cmd_set, 0, 1, 2, NULL);
  tcl_register(tcl, "split", tcl_cmd_split, 0, 1, 2, NULL);
  tcl_register(tcl, "string", tcl_cmd_string, 1, 1, 4, tcl_value("-nocase", -1));
  tcl_register(tcl, "subst", tcl_cmd_subst, 0, 1, 1, NULL);
  tcl_register(tcl, "switch", tcl_cmd_switch, 0, 2, -1, tcl_value("-exact -glob", -1));
  tcl_register(tcl, "unset", tcl_cmd_unset, 0, 1, -1, NULL);
  tcl_register(tcl, "upvar", tcl_cmd_upvar, 0, 1, -1, NULL);
  tcl_register(tcl, "while", tcl_cmd_while, 0, 2, 2, NULL);
# if !defined TCL_DISABLE_CLOCK
    tcl_register(tcl, "clock", tcl_cmd_clock, 1, 0, 2, NULL);
# endif
# if !defined TCL_DISABLE_FILEIO
    tcl_register(tcl, "close", tcl_cmd_close, 0, 1, 1, NULL);
    tcl_register(tcl, "eof", tcl_cmd_eof, 0, 1, 1, NULL);
    tcl_register(tcl, "file", tcl_cmd_file, 1, 1, 1, NULL);
    tcl_register(tcl, "flush", tcl_cmd_flush, 0, 1, 1, NULL);
    tcl_register(tcl, "gets", tcl_cmd_gets, 0, 1, 2, NULL);
    tcl_register(tcl, "open", tcl_cmd_open, 0, 1, 2, NULL);
    tcl_register(tcl, "read", tcl_cmd_read, 0, 1, 2, tcl_value("-nonewline", -1));
    tcl_register(tcl, "seek", tcl_cmd_seek, 0, 2, 3, NULL);
    tcl_register(tcl, "tell", tcl_cmd_tell, 0, 1, 1, NULL);
# endif
# if !defined TCL_DISABLE_PUTS
    tcl_register(tcl, "puts", tcl_cmd_puts, 0, 1, 2, tcl_value("-nonewline", -1));
# endif
# if !defined TCL_DISABLE_EXEC
    tcl_register(tcl, "exec", tcl_cmd_exec, 0, 1, -1, NULL);
# endif
# if !defined TCL_DISABLE_SOURCE
    tcl_register(tcl, "source", tcl_cmd_source, 0, 1, 1, NULL);
# endif
# if !defined TCL_DISABLE_BINARY
    tcl_register(tcl, "binary", tcl_cmd_binary, 1, 2, -1, NULL);
# endif
}

void tcl_destroy(struct tcl *tcl) {
  assert(tcl);
  while (tcl->env) {
    tcl->env = tcl_env_free(tcl->env);
  }
  while (tcl->cmds) {
    struct tcl_cmd *cmd = tcl->cmds;
    tcl->cmds = tcl->cmds->next;
    tcl_free(cmd->name);
    if (cmd->user) {
      tcl_free(cmd->user);
    }
    _free(cmd);
  }
  tcl_free(tcl->result);
  if (tcl->errinfo) {
    tcl_free(tcl->errinfo);
  }
  memset(tcl, 0, sizeof(struct tcl));
}

struct tcl_value *tcl_return(struct tcl *tcl) {
  assert(tcl);
  return (tcl->result);
}

const char *tcl_errorinfo(struct tcl *tcl, int *line) {
  assert(tcl);
  int errcode = tcl->errcode; /* save, before it is cleared */
  struct tcl_env *global_env = tcl_env_scope(tcl, 0);
  /* reset errorcode and the errorInfo variable */
  tcl->errcode = 0;
  struct tcl_env *env_save = global_env;
  tcl->env = global_env;
  tcl_var(tcl, "errorInfo", tcl_value("", 0));
  tcl->env = env_save;
  /* find nearest line of the error */
  if (line) {
    *line = 0;
    const char *script = global_env->errinfo.codebase;
    assert(script);
    if (script) {
      *line = 1;
      while (script < global_env->errinfo.currentpos) {
        if (*script == '\r' || *script == '\n') {
          *line += 1;
          if (*script == '\r' && *(script + 1) == '\n') {
            script++; /* handle \r\n as a single line feed */
          }
        }
        script++;
      }
    }
  }
  /* return the message */
  return (errcode != 0 && tcl->errinfo) ? tcl_data(tcl->errinfo) : "";
}

