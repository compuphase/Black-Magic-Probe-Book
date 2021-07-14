/*
 * Functions to parse a TSDL file and store it in memory structures. This
 * parser is the base for the tracegen code generation utility and the CTF
 * binary stream decoder.
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined __linux__
  #include <bsd/string.h>
#endif
#if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
  #include "strlcpy.h"
  #if defined _MSC_VER
    #define strdup(s)       _strdup(s)
    #define stricmp(s1,s2)  _stricmp((s1),(s2))
  #endif
#endif

#include "parsetsdl.h"

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  #define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif
#if !defined sizearray
  #define sizearray(a)  (sizeof(a) / sizeof((a)[0]))
#endif

#define MAX_LINE_LENGTH   1024
#define MAX_TOKEN_LENGTH  512

enum {
  TOK_NONE = 0x100,  /* above all ASCII characters */
  /* keywords */
  TOK_ALIGN,
  TOK_CALLSITE,
  TOK_CHAR,
  TOK_CONST,
  TOK_CLOCK,
  TOK_DOUBLE,
  TOK_ENUM,
  TOK_ENV,
  TOK_EVENT,
  TOK_FIELDS,
  TOK_FLOAT,
  TOK_FLOATING_POINT,
  TOK_HEADER,
  TOK_INT,
  TOK_INTEGER,
  TOK_LONG,
  TOK_PACKET,
  TOK_SHORT,
  TOK_SIGNED,
  TOK_STREAM,
  TOK_STRING,
  TOK_STRUCT,
  TOK_TRACE,
  TOK_TYPEALIAS,
  TOK_TYPEDEF,
  TOK_UNSIGNED,
  TOK_VARIANT,
  TOK_VOID,
  /* multi-character operators */
  TOK_OP_TYPE_ASSIGN,
  TOK_OP_ARROW,
  TOK_OP_NAMESPACE,
  TOK_OP_ELLIPSIS,
  /* general tokens */
  TOK_IDENTIFIER,
  TOK_LCHAR,
  TOK_LSTRING,
  TOK_LINTEGER,
  TOK_LFLOAT,
  TOK_EOF,
};

typedef struct tagTOKEN {
  int id;
  char *text;
  long number;
  double real;
  int pushed;
} TOKEN;


static FILE *inputfile = NULL;
static char *linebuffer = NULL;
static int linebuffer_index = 0;
static int linenumber = 0;
static int comment_block_start = 0;
static int error_count = 0;
static CTF_TYPE type_root = { NULL };
static TOKEN recent_token = { TOK_NONE, NULL, 0, 0.0 };

static CTF_TRACE_GLOBAL ctf_trace;
static CTF_PACKET_HEADER ctf_packet;
static CTF_CLOCK ctf_clock_root = { NULL };
static CTF_STREAM ctf_stream_root = { NULL };
static CTF_EVENT ctf_event_root = { NULL };


static const char *token_description(int token);
static void parse_declaration(CTF_TYPE *type, char *identifier, int size);


int ctf_error(int code, ...)
{
static int recent_error = -1;
  char message[256];
  va_list args;

  if (recent_error == linenumber)
    return 0;
  recent_error = linenumber;
  error_count++;
  va_start(args, code);

  switch (code) {
  case CTFERR_FILEOPEN:
    vsprintf(message, "File open error (file not found?)", args);
    break;
  case CTFERR_MEMORY:
    vsprintf(message, "Memory allocation error", args);
    break;
  case CTFERR_LONGLINE:
    vsprintf(message, "Line in input file too long", args);
    break;
  case CTFERR_BLOCKCOMMENT:
    vsprintf(message, "Block comment starting at line %d is not closed", args);
    break;
  case CTFERR_STRING:
    vsprintf(message, "String literal is not terminated", args);
    break;
  case CTFERR_INVALIDTOKEN:
    vsprintf(message, "Unknown token on column %d", args);
    break;
  case CTFERR_SYNTAX_MAIN:
    vsprintf(message, "Syntax error", args);
    break;
  case CTFERR_NEEDTOKEN: {
    char p[2][CTF_NAME_LENGTH];
    strcpy(p[0], token_description(va_arg(args, int)));
    strcpy(p[1], token_description(va_arg(args, int)));
    sprintf(message, "Expected %s but found %s", p[0], p[1]);
    break;
  }
  case CTFERR_INVALIDFIELD:
    vsprintf(message, "Unknown field name '%s'", args);
    break;
  case CTFERR_UNKNOWNTYPE:
    vsprintf(message, "Unknown or invalid type '%s'", args);
    break;
  case CTFERR_WRONGTYPE:
    vsprintf(message, "Wrong type for the field or type", args);
    break;
  case CTFERR_TYPE_SIZE:
    vsprintf(message, "Type declaration for '%s' lacks a size", args);
    break;
  case CTFERR_DUPLICATE_ID:
    vsprintf(message, "This id already exists", args);  //??? show line number of previous definition
    break;
  case CTFERR_UNKNOWNSTREAM:
    vsprintf(message, "Stream with name '%s' is not defined", args);
    break;
  case CTFERR_UNKNOWNCLOCK:
    vsprintf(message, "Clock with name '%s' is not defined", args);
    break;
  case CTFERR_STREAM_NOTSET:
    vsprintf(message, "Event '%s' is not assigned to a stream", args);
    break;
  case CTFERR_STREAM_NO_DEF:
    vsprintf(message, "No definition for stream id %d (required for event header)", args);
    break;
  case CTFERR_TYPE_REDEFINE:
    vsprintf(message, "Type %s is already defined", args);
    break;
  case CTFERR_NAMEREQUIRED:
    vsprintf(message, "Name for %s is required", args);
    break;
  case CTFERR_DUPLICATE_NAME:
    vsprintf(message, "Duplicate name %s", args);
    break;
  case CTFERR_CLOCK_IS_INT:
    vsprintf(message, "Clock must be mapped to integer type", args);
    break;
  default:
    assert(code != CTFERR_NONE);
    sprintf(message, "Unknown error, code %d", code);
    break;
  }
  va_end(args);
  ctf_error_notify(code, linenumber, message);  /* external function */
  return 0;
}


static int readline_init(const char *filename)
{
  linenumber = 0;
  comment_block_start = 0;

  inputfile = fopen(filename, "rt");
  if (inputfile == NULL)
    return ctf_error(CTFERR_FILEOPEN);

  linebuffer = (char*)malloc(MAX_LINE_LENGTH * sizeof(char));
  if (linebuffer == NULL) {
    fclose(inputfile);
    return ctf_error(CTFERR_MEMORY);
  }
  linebuffer[0] = '\0';
  return 1;
}

static void readline_cleanup(void)
{
  if (inputfile != NULL)
    fclose(inputfile);
  if (linebuffer != NULL)
    free((void*)linebuffer);
}

static int readline_next(void)
{
  assert(inputfile != NULL);
  assert(linebuffer != NULL);
  for (;; ) {
    char *ptr;
    char in_quotes;
    if (fgets(linebuffer, MAX_LINE_LENGTH - 1, inputfile)== NULL) {
      if (comment_block_start > 0)
        ctf_error(CTFERR_BLOCKCOMMENT, comment_block_start);
      return 0; /* no more data in the file */
    }

    linenumber += 1;
    ptr = strchr(linebuffer, '\n');
    if (ptr == NULL && !feof(inputfile))
      ctf_error(CTFERR_LONGLINE);
    if (ptr != NULL)
      *ptr = '\0';
    /* preprocess the line (remove comments) */
    in_quotes = '\0';
    for (ptr = linebuffer; *ptr != '\0'; ptr++) {
      if (comment_block_start > 0) {
        if (*ptr == '*' && *(ptr + 1) == '/') {
          comment_block_start = 0;
          *ptr = ' '; /* replace the comment by white-space */
          ptr++;      /* skip '*', the '/' is skipped in the for loop (after "continue") */
        }
        *ptr = ' ';   /* replace the comment by white-space */
        continue;
      } else if (in_quotes != '\0') {
        if (*ptr == '\\')
          ptr++;      /* skip '\', the letter following it is skipped in the for loop (after "continue") */
        else if (*ptr == in_quotes)
          in_quotes = 0;
        continue;
      } else if (*ptr == '/' && *(ptr + 1) == '/') {
        *ptr = '\0';    /* terminate line at the start of a single-line comment */
        break;          /* exit the for loop */
      } else if (*ptr == '/' && *(ptr + 1) == '*') {
        comment_block_start = linenumber;
        *ptr = ' ';     /* replace the comment by white-space */
      } else if (*ptr < ' ') {
        *ptr = ' ';
      }
    }
    /* strip trailing white-space */
    ptr = strchr(linebuffer, '\0');
    while (ptr > linebuffer && *(ptr - 1) <= ' ')
      ptr--;
    *ptr = '\0';
    /* continue until there is something in the line */
    if (strlen(linebuffer) > 0)
      break;
  }
  return 1;
}

static CTF_TYPE *type_init(CTF_TYPE *root, const char *name, int typeclass, int size, int flags)
{
  CTF_TYPE *item = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
  if (item != NULL) {
    memset(item, 0, sizeof(CTF_TYPE));
    if (name != NULL)
      strlcpy(item->name, name, CTF_NAME_LENGTH);
    item->typeclass = (uint8_t)typeclass;
    item->size = size;
    item->flags = (uint8_t)flags;
    assert(root != NULL);
    item->next = root->next;
    root->next = item;
  }
  return item;
}

static void type_cleanup(CTF_TYPE *root)
{
  assert(root != NULL);
  while (root->next != NULL) {
    CTF_TYPE *item = root->next;
    root->next = item->next;
    if (item->identifier != NULL)
      free((void*)item->identifier);
    if (item->selector != NULL)
      free((void*)item->selector);
    if (item->fields != NULL) {
      type_cleanup(item->fields);
      free((void*)item->fields);
    }
    if (item->keys != NULL) {
      CTF_KEYVALUE *keyroot = item->keys;
      while (keyroot->next != NULL) {
        CTF_KEYVALUE *keyitem = keyroot->next;
        keyroot->next = keyitem->next;
        free((void*)keyitem);
      }
      free((void*)item->keys);
    }
    free(item);
  }
}

static void type_duplicate(CTF_TYPE *tgt, const CTF_TYPE *src)
{
  assert(tgt != NULL && src != NULL);
  memcpy(tgt, src, sizeof(CTF_TYPE)); /* copy simple fields */
  tgt->next = NULL;

  if (src->identifier != NULL)
    tgt->identifier = strdup(src->identifier);
  if (src->selector != NULL)
    tgt->selector = strdup(src->selector);

  if (src->fields != NULL) {
    CTF_TYPE *fsrc;
    tgt->fields = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
    if (tgt->fields == NULL) {
      ctf_error(CTFERR_MEMORY);
      return;
    }
    memset(tgt->fields, 0, sizeof(CTF_TYPE));
    for (fsrc = src->fields->next; fsrc != NULL; fsrc = fsrc->next) {
      CTF_TYPE *ftgt = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
      if (ftgt != NULL)
        type_duplicate(ftgt, fsrc);
    }
  }

  if (src->keys != NULL) {
    const CTF_KEYVALUE *kv_src;
    tgt->keys = (CTF_KEYVALUE*)malloc(sizeof(CTF_KEYVALUE));
    if (tgt->keys == NULL) {
      ctf_error(CTFERR_MEMORY);
      return;
    }
    memset(tgt->keys, 0, sizeof(CTF_KEYVALUE));
    for (kv_src = src->keys->next; kv_src != NULL; kv_src = kv_src->next) {
      CTF_KEYVALUE *kv_tgt = (CTF_KEYVALUE*)malloc(sizeof(CTF_KEYVALUE));
      if (kv_tgt != NULL) {
        memcpy(kv_tgt, kv_src, sizeof(CTF_KEYVALUE));
        kv_tgt->next = tgt->keys->next;
        tgt->keys->next = kv_tgt;
      }
    }
  }
}

static CTF_TYPE *type_lookup(CTF_TYPE *root, const char *name)
{
  CTF_TYPE *item;
  assert(root != NULL);
  assert(name != NULL);
  for (item = root->next; item != NULL; item = item->next)
    if (item->name != NULL && strcmp(item->name, name) == 0)
      return item;
  return NULL;
}

static void type_default_int(CTF_TYPE *type)
{
  CTF_TYPE *basetype = type_lookup(&type_root, "int");
  assert(type != NULL);
  if (basetype != NULL) {
    /* "int" type has been defined, so use it */
    memcpy(type, basetype, sizeof(CTF_TYPE));
  } else {
    /* int type has not been defined, assume 32-bit signed */
    type->typeclass = CLASS_INTEGER;
    type->size = 32;
    type->align = 0;
    type->scale = 0;
    type->length = 0;
    type->flags = TYPEFLAG_SIGNED;
    type->base = 10;
    type->identifier = NULL;
    type->selector = NULL;
    type->fields = NULL;
    type->keys = NULL;
  }
}

static int hexdigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  assert(0);  /* should no reach this point */
  return -1;
}

static int token_init(void)
{
  memset(&recent_token, 0, sizeof(TOKEN));
  recent_token.id = TOK_NONE;
  recent_token.pushed = 0;
  recent_token.text = (char*)malloc(MAX_TOKEN_LENGTH * sizeof(char));
  if (recent_token.text == NULL)
    return ctf_error(CTFERR_MEMORY);
  recent_token.text[0] = '\0';
  linebuffer_index = MAX_LINE_LENGTH;  /* force to read a line on the first call */
  return 1;
}

static void token_cleanup(void)
{
  if (recent_token.text != NULL)
    free((void*)recent_token.text);
  memset(&recent_token, 0, sizeof(TOKEN));
  recent_token.id = TOK_EOF;
}

/* name array must run parallel with TOK_xxx enum */
static const char *token_keywords[] = {
  "align", "callsite", "char", "const", "clock", "double", "enum", "env",
  "event", "fields", "float", "floating_point", "header", "int", "integer",
  "long", "packet", "short", "signed", "stream", "string", "struct", "trace",
  "typealias", "typedef", "unsigned", "variant", "void" };
static const char *token_operators[] = {
  ":=", "->", "::", "..." };
static const char *token_generic[] = {
 "identifier", "character literal", "string literal", "integer value",
 "floating-point value", "end of file" };

static const char *token_description(int token)
{
  if (token < 0x100) {
    static char name[10];
    sprintf(name, "'%c'", token);
    return name;
  }

  token -= TOK_NONE + 1;
  assert(token >= 0);
  if (token < sizearray(token_keywords))
    return token_keywords[token];

  token -= sizearray(token_keywords);
  assert(token >= 0);
  if (token < sizearray(token_operators))
    return token_operators[token];

  token -= sizearray(token_operators);
  assert(token >= 0);
  assert(token < sizearray(token_generic));
  return token_generic[token];
}

static int token_next(void)
{
  if (recent_token.pushed) {
    recent_token.pushed = 0;
    return recent_token.id;
  }

  assert(linebuffer != NULL);
  if ((unsigned)linebuffer_index >= strlen(linebuffer)) {
    if (!readline_next()) {
      recent_token.id = TOK_EOF;
      return recent_token.id;
    }
    linebuffer_index = 0;
  }

  while (linebuffer[linebuffer_index] == ' ')
    linebuffer_index++; /* skip white-space */
  if (isdigit(linebuffer[linebuffer_index])) {
    /* literal number (decimal, hexadecimal or floating point) */
    recent_token.id = TOK_LINTEGER; /* may be overruled later */
    recent_token.number = 0;
    recent_token.real = 0.0;
    if (linebuffer[linebuffer_index] == '0' && (linebuffer[linebuffer_index] == 'x' || linebuffer[linebuffer_index] == 'X')) {
      /* hexadecimal */
      linebuffer_index += 2;
      while (isxdigit(linebuffer[linebuffer_index])) {
        recent_token.number = (recent_token.number << 4) | hexdigit(linebuffer[linebuffer_index]);
        linebuffer_index++;
      }
    } else {
      /* decimal or floating point */
      while (isdigit(linebuffer[linebuffer_index])) {
        recent_token.number = (recent_token.number * 10) + (linebuffer[linebuffer_index] - '0');
        linebuffer_index++;
      }
      if (linebuffer[linebuffer_index] == '.') {
        double mult = 0.1;
        recent_token.id = TOK_LFLOAT;
        recent_token.real = recent_token.number;
        linebuffer_index++;
        while (isdigit(linebuffer[linebuffer_index])) {
          recent_token.real += (linebuffer[linebuffer_index] - '0') * mult;
          mult /= 10.0;
          linebuffer_index++;
        }
      }
    }
  } else if (linebuffer[linebuffer_index] == '\'') {
    /* literal character */
    int idx = 0;
    recent_token.id = TOK_LCHAR;
    linebuffer_index++;
    while (linebuffer[linebuffer_index] != '\'' && linebuffer[linebuffer_index] != '\0') {
      if (linebuffer[linebuffer_index] == '\\' && linebuffer[linebuffer_index + 1] != '\0')
        recent_token.text[idx++] = linebuffer[linebuffer_index++];
      recent_token.text[idx++] = linebuffer[linebuffer_index++];
      if (idx >= MAX_TOKEN_LENGTH)
        break;
    }
    recent_token.text[idx] = '\0';
    if (linebuffer[linebuffer_index] == '\'')
      linebuffer_index++;
    else
      ctf_error(CTFERR_STRING);
  } else if (linebuffer[linebuffer_index] == '"') {
    /* literal string */
    int idx = 0;
    recent_token.id = TOK_LSTRING;
    linebuffer_index++;
    while (linebuffer[linebuffer_index] != '"' && linebuffer[linebuffer_index] != '\0') {
      if (linebuffer[linebuffer_index] == '\\' && linebuffer[linebuffer_index + 1] != '\0')
        recent_token.text[idx++] = linebuffer[linebuffer_index++];
      recent_token.text[idx++] = linebuffer[linebuffer_index++];
      if (idx >= MAX_TOKEN_LENGTH)
        break;
    }
    recent_token.text[idx] = '\0';
    if (linebuffer[linebuffer_index] == '"')
      linebuffer_index++;
    else
      ctf_error(CTFERR_STRING);
  } else if (isalpha(linebuffer[linebuffer_index]) || linebuffer[linebuffer_index] == '_') {
    /* identifier or keyword */
    int idx = 0;
    recent_token.id = TOK_IDENTIFIER; /* may be reset later */
    while (isalnum(linebuffer[linebuffer_index]) || linebuffer[linebuffer_index] == '_') {
      recent_token.text[idx++] = linebuffer[linebuffer_index++];
      if (idx >= MAX_TOKEN_LENGTH)
        break;
    }
    recent_token.text[idx] = '\0';
    if (isalnum(linebuffer[linebuffer_index]))
      ctf_error(CTFERR_INVALIDTOKEN, linebuffer_index + 1);
    /* now check whether this is a keyword */
    for (idx = 0; idx < sizearray(token_keywords); idx++)
      if (strcmp(recent_token.text, token_keywords[idx]) == 0)
        break;
    if (idx < sizearray(token_keywords)) {
      recent_token.id = TOK_NONE + idx + 1;
    } else {
      /* also check for "boolean" values */
      if (strcmp(recent_token.text, "false") == 0 || strcmp(recent_token.text, "FALSE") == 0) {
        recent_token.id = TOK_LINTEGER;
        recent_token.number = 0;
      } else if (strcmp(recent_token.text, "true") == 0 || strcmp(recent_token.text, "TRUE") == 0) {
        recent_token.id = TOK_LINTEGER;
        recent_token.number = 1;
      }
    }
  } else {
    /* operator */
    if (linebuffer[linebuffer_index] == ':') {
      recent_token.id = linebuffer[linebuffer_index];
      linebuffer_index += 1;
      if (linebuffer[linebuffer_index] == '=') {
        recent_token.id = TOK_OP_TYPE_ASSIGN; /* := */
        linebuffer_index += 1;
      } else if (linebuffer[linebuffer_index] == ':') {
        recent_token.id = TOK_OP_NAMESPACE;   /* :: */
        linebuffer_index += 1;
      }
    } else if (linebuffer[linebuffer_index] == '-' && linebuffer[linebuffer_index + 1] == '>') {
      recent_token.id = TOK_OP_ARROW;
      linebuffer_index += 2;
    } else if (linebuffer[linebuffer_index] == '.' && linebuffer[linebuffer_index + 1] == '.' && linebuffer[linebuffer_index + 2] == '.') {
      recent_token.id = TOK_OP_ELLIPSIS;
      linebuffer_index += 3;
    } else if (strchr("[](){}.*+-<>;=,", linebuffer[linebuffer_index]) != NULL) {
      recent_token.id = linebuffer[linebuffer_index];
      linebuffer_index += 1;
    } else {
      recent_token.id = TOK_NONE;
      ctf_error(CTFERR_INVALIDTOKEN, linebuffer_index + 1);
    }
  }

  return recent_token.id;
}

static void token_pushback(void)
{
  assert(!recent_token.pushed);
  recent_token.pushed = 1;
}

static const char *token_gettext(void)
{
  return recent_token.text;
}

static long token_getlong(void)
{
  return recent_token.number;
}

static double token_getreal(void)
{
  return recent_token.real;
}

static int token_match(int token)
{
  int tok = token_next();
  if (token == TOK_IDENTIFIER && tok == TOK_LSTRING)
    tok = token;  /* identifiers may be quoted */
  if (tok != token)
    token_pushback();
  return tok == token;
}

static int token_need(int token)
{
  int tok = token_next();
  if (tok == token)
    return 1;
  if (token == TOK_IDENTIFIER && tok == TOK_LSTRING)
    return 1; /* identifiers may be quoted */
  ctf_error(CTFERR_NEEDTOKEN, token, tok);
  return (tok == TOK_EOF) ? -1 : 0;
}

const CTF_PACKET_HEADER *packet_header(void)
{
  return &ctf_packet;
}

static void clock_cleanup(void)
{
  while (ctf_clock_root.next != NULL) {
    CTF_CLOCK *iter = ctf_clock_root.next;
    ctf_clock_root.next = iter->next;
    free((void*)iter);
  }
}

const CTF_CLOCK *clock_by_name(const char *name)
{
  CTF_CLOCK *clock;
  for (clock = ctf_clock_root.next; clock != NULL; clock = clock->next)
    if (strcmp(clock->name, name) == 0)
      return clock;
  return NULL;
}

const CTF_CLOCK *clock_by_seqnr(int seqnr)
{
  CTF_CLOCK *clock;
  for (clock = ctf_clock_root.next; clock != NULL && seqnr > 0; clock = clock->next)
    seqnr -= 1;
  return clock;
}

static void stream_cleanup(void)
{
  while (ctf_stream_root.next != NULL) {
    CTF_STREAM *iter = ctf_stream_root.next;
    ctf_stream_root.next = iter->next;
    free((void*)iter);
  }
}

int stream_isactive(int stream_id)
{
  return (ctf_trace.stream_mask & (1 << stream_id)) != 0;
}

int stream_count(void)
{
  int count = 0;
  CTF_STREAM *stream;
  for (stream = ctf_stream_root.next; stream != NULL; stream = stream->next)
    count++;
  return count;
}

const CTF_STREAM *stream_by_name(const char *name)
{
  CTF_STREAM *stream;
  for (stream = ctf_stream_root.next; stream != NULL; stream = stream->next)
    if (strcmp(stream->name, name) == 0)
      return stream;
  return NULL;
}

const CTF_STREAM *stream_by_id(int stream_id)
{
  CTF_STREAM *stream;
  for (stream = ctf_stream_root.next; stream != NULL; stream = stream->next)
    if (stream->stream_id == stream_id)
      return stream;
  return NULL;
}

const CTF_STREAM *stream_by_seqnr(int seqnr)
{
  CTF_STREAM *stream;
  for (stream = ctf_stream_root.next; stream != NULL && seqnr > 0; stream = stream->next)
    seqnr -= 1;
  return stream;
}

static void event_cleanup(void)
{
  while (ctf_event_root.next != NULL) {
    CTF_EVENT *iter = ctf_event_root.next;
    ctf_event_root.next = iter->next;
    while (iter->field_root.next != NULL) {
      CTF_EVENT_FIELD *fld = iter->field_root.next;
      iter->field_root.next = fld->next;
      free((void*)fld);
    }
    free((void*)iter);
  }
}

/** event_count() returns the number of events in a stream; set parameter
 *  stream_id to -1 to return the total over all streams.
 */
int event_count(int stream_id)
{
  int count = 0;
  CTF_EVENT *event;
  for (event = ctf_event_root.next; event != NULL; event = event->next)
    if (stream_id == -1 || event->stream_id == stream_id)
      count++;
  return count;
}

/** event_next() returns a pointer to the event following the one passed in
 *  the parameter; it returns the first event if the "event" parameter is NULL.
 */
const CTF_EVENT *event_next(const CTF_EVENT *event)
{
  if (event == NULL)
    return ctf_event_root.next;
  return event->next;
}

const CTF_EVENT *event_by_id(int event_id)
{
  CTF_EVENT *event;
  for (event = ctf_event_root.next; event != NULL; event = event->next)
    if (event->id == event_id)
      return event;
  return NULL;
}

/** close_declaration() frees all memory for a single type declaration, but does
 *  not free the type structure itself. This function is used to clean-up a
 *  temporary declaration in an automatic variable (one obtained with
 *  parse_declaration()).
 */
static void close_declaration(CTF_TYPE *type)
{
  if (type->fields != NULL) {
    type_cleanup(type->fields);
    free((void*)type->fields);
    type->fields = NULL;
  }
  if (type->keys != NULL) {
    CTF_KEYVALUE *keyroot = type->keys;
    while (keyroot->next != NULL) {
      CTF_KEYVALUE *keyitem = keyroot->next;
      keyroot->next = keyitem->next;
      free((void*)keyitem);
    }
    free((void*)type->keys);
    type->keys = NULL;
  }
}

void parse_enum_fields(CTF_TYPE *type)
{
  long curval = 0;

  assert(type->keys == NULL); /* there should not already exist a key-value list */
  type->keys = (CTF_KEYVALUE*)malloc(sizeof(CTF_KEYVALUE));
  if (type->keys != NULL)
    memset(type->keys, 0, sizeof(CTF_KEYVALUE));
  else
    ctf_error(CTFERR_MEMORY);

  token_need('{');
  while (!token_match('}')) {
    int tok = token_next();
    if (tok == TOK_IDENTIFIER) {
      char identifier[CTF_NAME_LENGTH];
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      if (token_match('=')) {
        token_need(TOK_LINTEGER);
        curval = token_getlong();
      }
      if (type->keys != NULL) {
        CTF_KEYVALUE *kv;
        kv = (CTF_KEYVALUE*)malloc(sizeof(CTF_KEYVALUE));
        if (kv == NULL) {
          ctf_error(CTFERR_MEMORY);
        } else {
          memset(kv, 0, sizeof(CTF_KEYVALUE));
          strlcpy(kv->name, identifier, sizearray(kv->name));
          kv->value = curval++;
          kv->next = type->keys->next;
          type->keys->next = kv;
        }
      }
      /* comma between the enumeration items is required, but a comma behind the
         last item is optional */
      if (!token_match(',')) {
        token_need('}');
        break;
      }
    } else {
      ctf_error(CTFERR_NEEDTOKEN, '}', tok);
      if (tok == TOK_EOF)
        break;
    }
  }
}

void parse_struct_fields(CTF_TYPE *type)
{
  char identifier[CTF_NAME_LENGTH];
  CTF_TYPE subtype, *field, *tail;
  unsigned long structsize;
  int copytype;

  assert(type->fields == NULL); /* there should not already exist a list of fields */
  type->fields = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
  if (type->fields != NULL)
    memset(type->fields, 0, sizeof(CTF_TYPE));
  else
    ctf_error(CTFERR_MEMORY);

  copytype = 0;
  structsize = 0;
  token_need('{');
  while (!token_match('}')) {
    if (copytype) {
      type_duplicate(&subtype, &subtype);
      token_need(TOK_IDENTIFIER);
      strlcpy(identifier, token_gettext(), sizearray(identifier));
    } else {
      parse_declaration(&subtype, identifier, sizearray(identifier));
    }
    field = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
    if (field != NULL) {
      for (tail = type->fields; tail->next != NULL; tail = tail->next)
        /* nothing */;
      memcpy(field, &subtype, sizeof(CTF_TYPE));
      field->identifier = strdup(identifier);
      field->next = NULL;
      tail->next = field;
      /* accumulate the size of the fields */
      //??? check for typeclass == CLASS_STRING, because struct size is then variable
      if (field->length > 1)
        structsize += field->size * field->length;
      else
        structsize += field->size;
    } else {
      ctf_error(CTFERR_MEMORY);
      close_declaration(&subtype);
    }
    copytype = token_match(',');
    if (!copytype && token_need(';') < 0)
      break;
  }
  type->size = structsize;  /* set the total size of the struct */
}

static void parse_typealias_fields(CTF_TYPE *type)
{
  token_need('{');
  while (!token_match('}')) {
    int tok = token_next();
    if (tok == TOK_IDENTIFIER) {
      char identifier[CTF_NAME_LENGTH];
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      token_need('=');
      if (strcmp(identifier, "encoding") == 0) {
        token_need(TOK_LINTEGER);
        if (strcmp(token_gettext(), "utf8") == 0 || strcmp(token_gettext(), "UTF8") == 0)
          type->flags |= TYPEFLAG_UTF8;
      } else if (strcmp(identifier, "scale") == 0) {
        token_need(TOK_LINTEGER);
        type->scale = (int)token_getlong();
      } else if (strcmp(identifier, "size") == 0) {
        token_need(TOK_LINTEGER);
        type->size = (uint8_t)token_getlong();
      } else if (strcmp(identifier, "base") == 0) {
        if (token_match(TOK_LINTEGER)) {
          type->base = (uint8_t)token_getlong();
        } else {
          const char *p;
          token_need(TOK_IDENTIFIER);
          p = token_gettext();
          if (strcmp(p, "decimal") || strcmp(p, "dec") || strcmp(p, "d") || strcmp(p, "i")) {
            type->base = 10;
          } else if (strcmp(p, "hexadecimal") || strcmp(p, "hex") || stricmp(p, "x")) {
            type->base = 16;
          } else if (strcmp(p, "octal") || strcmp(p, "oct") || stricmp(p, "o")) {
            type->base = 8;
          } else if (strcmp(p, "binary") || stricmp(p, "b")) {
            type->base = 2;
          } else if (strcmp(p, "symaddress") || stricmp(p, "symaddr")) {
            type->base = CTF_BASE_ADDR;
            type->flags &= ~TYPEFLAG_SIGNED;
          }
        }
      } else if (strcmp(identifier, "byte_order") == 0 || strcmp(identifier, "exp_dig") == 0  || strcmp(identifier, "mant_dig") == 0) {
        token_need(TOK_IDENTIFIER);
        //??? error: feature not implemented
      } else if (strcmp(identifier, "map") == 0) {
        token_need(TOK_CLOCK);
        token_need('.');
        token_need(TOK_IDENTIFIER);
        type->selector = strdup(token_gettext());
        /* check that the clock exists */
        if (clock_by_name(token_gettext()) == NULL)
          ctf_error(CTFERR_UNKNOWNCLOCK, token_gettext());
        /* CTF specification says to map to clock.value */
        if (token_match('.')) {
          token_need(TOK_IDENTIFIER);
          if (strcmp(token_gettext(), "value") != 0)
            ctf_error(CTFERR_INVALIDFIELD, token_getlong());
        }
        /* the clock type must be an integer */
        if (type->typeclass != CLASS_INTEGER)
          ctf_error(CTFERR_CLOCK_IS_INT);
      }
      token_need(';');
    } else if (tok == TOK_ALIGN) {
      token_need('=');
      token_need(TOK_LINTEGER);
      type->align = (uint8_t)token_getlong();
      token_need(';');
    } else if (tok == TOK_SIGNED) {
      token_need('=');
      token_need(TOK_LINTEGER);
      if (token_getlong() != 0)
        type->flags |= TYPEFLAG_SIGNED;
      token_need(';');
    } else {
      ctf_error(CTFERR_NEEDTOKEN, '}', tok);
      if (tok == TOK_EOF)
        break;
    }
  }
}

/** parse_declaration() gets a type declaration. It optionally also parses
 *  the name of the field with the type, and the optional array specification
 *  following the name.
 *
 *  \param type         The type will be stored in this parameter. It needs to
 *                      be cleaned up with close_declaration().
 *  \param identifier   The name of the field (or new type) following the
 *                      declaration. This parameter may be NULL, in which case
 *                      it will not be parsed.
 *  \param size         The size (in characters) of the buffer for "identifier".
 *
 *  \note If the identifier is not parsed, any array specifications following
 *        the identifier will not be parsed either.
 */
static void parse_declaration(CTF_TYPE *type, char *identifier, int size)
{
  int token;

  /* get type */
  assert(type != NULL);
  memset(type, 0, sizeof(CTF_TYPE));
  token = token_next();
  if (token == TOK_IDENTIFIER) {
    /* look up user type */
    CTF_TYPE *usertype = type_lookup(&type_root, token_gettext());
    if (usertype != NULL)
      type_duplicate(type, usertype);
  } else if (token == TOK_INTEGER) {
    type->typeclass = CLASS_INTEGER;
    parse_typealias_fields(type);
  } else if (token == TOK_FLOATING_POINT) {
    type->typeclass = CLASS_FLOAT;
    parse_typealias_fields(type);
  } else if (token == TOK_STRING) {
    type->size = 8;
    type->typeclass = CLASS_STRING;
    if (token_match('{')) {
      /* parse options for this type (especially the encoding), but since the
         opening brace is matched first at the start of parse_typealias_fields()
         put it back first */
      token_pushback();
      parse_typealias_fields(type);
    }
  } else if (token == TOK_ENUM) {
    CTF_TYPE basetype;
    type_default_int(&basetype);
    type->typeclass = basetype.typeclass;
    type->size = basetype.size;
    type->align = basetype.align;
    type->flags = basetype.flags;
    parse_enum_fields(type);
  } else if (token == TOK_STRUCT) {
    CTF_TYPE *usertype = NULL;
    if (token_match(TOK_IDENTIFIER)) {
      strlcpy(type->name, token_gettext(), sizearray(type->name));  /* a name is redundant if fields follow */
      usertype = type_lookup(&type_root, token_gettext());
    }
    type->typeclass = CLASS_STRUCT;
    if (usertype != NULL && usertype->typeclass == CLASS_STRUCT) {
      if (token_match('{')) {
        /* struct definition follows, even though the type already exists */
        ctf_error(CTFERR_TYPE_REDEFINE, type->name);
        token_pushback(); /* push { back, parse the fields anyway */
        parse_struct_fields(type);
      } else {
        type_duplicate(type, usertype);
      }
    } else {
      parse_struct_fields(type);
    }
  } else if (token == TOK_VARIANT) {
    //??? error: feature not implemented
  } else {
    /* parse a C system type */
    int done = 0;
    type->flags = TYPEFLAG_SIGNED; /* C types are signed by default */
    if (token == TOK_CONST)
      token = token_next(); /* ignore "const" */
    if (token == TOK_SIGNED) {
      token = token_next(); /* ignore explicit "signed" */
    } else if (token == TOK_UNSIGNED) {
      type->flags &= ~TYPEFLAG_SIGNED;
      type->size = 32;  /* preset for "unsigned int" */
      type->typeclass = CLASS_INTEGER;
      token = token_next();
    } else if (token == TOK_FLOAT) {
      type->size = 32;
      type->typeclass = CLASS_FLOAT;
      done = 1;
    } else if (token == TOK_DOUBLE) {
      type->size = 64;
      type->typeclass = CLASS_FLOAT;
      done = 1;
    }
    if (!done) {
      if (token == TOK_CHAR) {
        type->size = 8;
        type->typeclass = CLASS_INTEGER;
        if (token_match('*'))
          type->typeclass = CLASS_STRING; /* "char *" -> string */
      } else if (token == TOK_SHORT) {
        type->size = 16;
        type->typeclass = CLASS_INTEGER;
        token_match(TOK_INT);   /* gobble up "int" after "short" */
      } else if (token == TOK_LONG) {
        type->size = 32;
        type->typeclass = CLASS_INTEGER;
        if (token_match(TOK_LONG))
          type->size = 32;      /* "long long" */
        token_match(TOK_INT);   /* gobble up "int" after "long" */
      } else if (token == TOK_INT) {
        type->size = 32;
        type->typeclass = CLASS_INTEGER;
      }
    }
  }
  if (type->size == 0)
    ctf_error(CTFERR_UNKNOWNTYPE, token_gettext());

  if (identifier != NULL && size > 0) {
    /* copy identifier name */
    identifier[0] = '\0';
    for ( ;; ) {
      token = token_next();
      /* a few keywords are also identifiers */
      if (token == TOK_EVENT)
        strlcat(identifier, "event", size);
      else if (token == TOK_STREAM)
        strlcat(identifier, "stream", size);
      else if (token == TOK_IDENTIFIER)
        strlcat(identifier, token_gettext(), size);
      else
        ctf_error(CTFERR_NEEDTOKEN, TOK_IDENTIFIER, token);
      if (!token_match('.'))
        break;  /* dotted names (structure.field) are considered individual identifiers */
      strlcat(identifier, ".", size);
    }

    /* match [##] for array */
    if (token_match('[')) {
      token_need(TOK_LINTEGER);
      type->length = token_getlong();
      token_need(']');
    }
  }
}

static void parse_packet_header(void)
{
  CTF_TYPE *knowntype = NULL;
  char identifier[CTF_NAME_LENGTH] = "";

  if (token_match(TOK_IDENTIFIER)) {
    /* typedef'ed type */
    knowntype = type_lookup(&type_root, token_gettext());
    if (knowntype == NULL)
      ctf_error(CTFERR_UNKNOWNTYPE, token_gettext());
  } else {
    token_need(TOK_STRUCT);
    if (token_match(TOK_IDENTIFIER)) {
      /* defined struct */
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      knowntype = type_lookup(&type_root, identifier);
    }
    if (token_match('{')) {
      knowntype = NULL; /* ignore the struct name if a definition follows */
    } else if (knowntype == NULL) {
      if (strlen(identifier) == 0)
        ctf_error(CTFERR_NEEDTOKEN, '{', token_next());
      else
        ctf_error(CTFERR_UNKNOWNTYPE, identifier);
    }
  }
  if (knowntype == NULL) {
    CTF_TYPE type;
    while (!token_match('}')) {
      parse_declaration(&type, identifier, sizearray(identifier));
      if (strcmp(identifier, "magic") == 0) {
        if (type.typeclass != CLASS_INTEGER || type.length != 0)
          ctf_error(CTFERR_WRONGTYPE);
        ctf_packet.header.magic_size = (uint8_t)type.size;
      } else if (strcmp(identifier, "stream.id") == 0 || strcmp(identifier, "stream_id") == 0) {
        if (type.typeclass != CLASS_INTEGER || type.length != 0)
          ctf_error(CTFERR_WRONGTYPE);
        ctf_packet.header.streamid_size = (uint8_t)type.size;
      } else if (strcmp(identifier, "uuid") == 0) {
        if (type.typeclass != CLASS_INTEGER || type.size != 8 || type.length == 0)
        ctf_error(CTFERR_WRONGTYPE);
        ctf_packet.header.uuid_size = (uint8_t)(type.length * type.size);
      } else {
        ctf_error(CTFERR_INVALIDFIELD, identifier);
      }
      if (token_need(';') < 0)
        break;
    }
    token_match(';'); /* ';' after closing brace is optional */
    close_declaration(&type);
  } else {
    if (knowntype->typeclass != CLASS_STRUCT) {
      ctf_error(CTFERR_WRONGTYPE);
    } else {
      CTF_TYPE *field;
      assert(knowntype->fields != NULL);
      for (field = knowntype->fields->next; field != NULL; field = field->next) {
        if (strcmp(field->identifier, "magic")== 0) {
          if (field->typeclass != CLASS_INTEGER || field->length != 0)
            ctf_error(CTFERR_WRONGTYPE);
          ctf_packet.header.magic_size = (uint8_t)field->size;
        } else if (strcmp(field->identifier, "stream.id") == 0 || strcmp(field->identifier, "stream_id") == 0) {
          if (field->typeclass != CLASS_INTEGER || field->length != 0)
            ctf_error(CTFERR_WRONGTYPE);
          ctf_packet.header.streamid_size = (uint8_t)field->size;
        } else if (strcmp(field->identifier, "uuid") == 0) {
          if (field->typeclass != CLASS_INTEGER || field->size != 8 || field->length == 0)
          ctf_error(CTFERR_WRONGTYPE);
          ctf_packet.header.uuid_size = (uint8_t)(field->length * field->size);
        } else {
          ctf_error(CTFERR_INVALIDFIELD, field->identifier);
        }
      }
    }
    token_need(';');
  }
}

static void parse_event_header(CTF_EVENT_HEADER *evthdr, CTF_TYPE **clock)
{
  CTF_TYPE *knowntype = NULL;
  char identifier[CTF_NAME_LENGTH] = "";

  assert(evthdr != NULL);
  if (token_match(TOK_IDENTIFIER)) {
    /* typedef'ed type */
    knowntype = type_lookup(&type_root, token_gettext());
    if (knowntype == NULL)
      ctf_error(CTFERR_UNKNOWNTYPE, token_gettext());
  } else {
    token_need(TOK_STRUCT);
    if (token_match(TOK_IDENTIFIER)) {
      /* defined struct */
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      knowntype = type_lookup(&type_root, identifier);
    }
    if (token_match('{')) {
      knowntype = NULL; /* ignore the struct name if a definition follows */
    } else if (knowntype == NULL) {
      if (strlen(identifier) == 0)
        ctf_error(CTFERR_NEEDTOKEN, '{', token_next());
      else
        ctf_error(CTFERR_UNKNOWNTYPE, identifier);
    }
  }
  if (knowntype == NULL) {
    CTF_TYPE type;
    while (!token_match('}')) {
      parse_declaration(&type, identifier, sizearray(identifier));
      if (strcmp(identifier, "event.id") == 0 || strcmp(identifier, "id") == 0) {
        if (type.typeclass != CLASS_INTEGER || type.length != 0)
          ctf_error(CTFERR_WRONGTYPE);
        evthdr->header.id_size = (uint8_t)type.size;
      } else if (strcmp(identifier, "timestamp") == 0) {
        if (type.typeclass != CLASS_INTEGER || type.length != 0)
          ctf_error(CTFERR_WRONGTYPE);
        evthdr->header.timestamp_size = (uint8_t)type.size;
        /* store a reference to the clock (a clock type must always be created
           with typealias, because of the "map" attribute, so the type always
           has a name) */
        if (clock != NULL && strlen(type.name) > 0)
          *clock = type_lookup(&type_root, type.name);
      } else {
        ctf_error(CTFERR_INVALIDFIELD, identifier);
      }
      if (token_need(';') < 0)
        break;
    }
    token_match(';'); /* ';' after closing brace is optional */
    close_declaration(&type);
  } else {
    if (knowntype->typeclass != CLASS_STRUCT) {
      ctf_error(CTFERR_WRONGTYPE);
    } else {
      CTF_TYPE *field;
      assert(knowntype->fields != NULL);
      for (field = knowntype->fields->next; field != NULL; field = field->next) {
        if (strcmp(field->identifier, "event.id")== 0 || strcmp(field->identifier, "id")== 0) {
          if (field->typeclass != CLASS_INTEGER || field->length != 0)
            ctf_error(CTFERR_WRONGTYPE);
          evthdr->header.id_size = (uint8_t)field->size;
        } else if (strcmp(field->identifier, "timestamp") == 0) {
          if (field->typeclass != CLASS_INTEGER || field->length != 0)
            ctf_error(CTFERR_WRONGTYPE);
          evthdr->header.timestamp_size = (uint8_t)field->size;
          /* store a reference to the clock (a clock type must always be created
             with typealias, because of the "map" attribute, so the type always
             has a name) */
          if (clock != NULL && strlen(field->name) > 0)
            *clock = type_lookup(&type_root, field->name);
        } else {
          ctf_error(CTFERR_INVALIDFIELD, field->identifier);
        }
      }
    }
    token_need(';');
  }
}

static void parse_event_fields(CTF_EVENT_FIELD *fieldroot)
{
  CTF_TYPE *knowntype = NULL;

  assert(fieldroot != NULL);
  if (token_match(TOK_IDENTIFIER)) {
    /* typedef'ed type */
    knowntype = type_lookup(&type_root, token_gettext());
    if (knowntype == NULL)
      ctf_error(CTFERR_UNKNOWNTYPE, token_gettext());
  } else {
    char identifier[CTF_NAME_LENGTH] = "";
    token_need(TOK_STRUCT);
    if (token_match(TOK_IDENTIFIER)) {
      /* defined struct */
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      knowntype = type_lookup(&type_root, identifier);
    }
    if (token_match('{')) {
      knowntype = NULL; /* ignore the struct name if a definition follows */
    } else if (knowntype == NULL) {
      if (strlen(identifier) == 0)
        ctf_error(CTFERR_NEEDTOKEN, '{', token_next());
      else
        ctf_error(CTFERR_UNKNOWNTYPE, identifier);
    }
  }
  if (knowntype == NULL) {
    CTF_TYPE type;
    while (!token_match('}')) {
      char identifier[CTF_NAME_LENGTH];
      parse_declaration(&type, identifier, sizearray(identifier));
      if (type.size > 0) {
        /* add field */
        CTF_EVENT_FIELD *field = (CTF_EVENT_FIELD*)malloc(sizeof(CTF_EVENT_FIELD));
        if (field != NULL) {
          CTF_EVENT_FIELD *tail;  /* must keep fields in order of declaration */
          strlcpy(field->name, identifier, sizearray(field->name));
          type_duplicate(&field->type, &type);
          field->next = NULL;
          for (tail = fieldroot; tail->next != NULL; tail = tail->next)
            /* nothing */;
          assert(tail != NULL && tail->next == NULL);
          tail->next = field;
        }
      }
      close_declaration(&type);
      if (token_need(';') < 0)
        break;
    }
    token_match(';'); /* ';' after closing brace is optional */
  } else {
    if (knowntype->typeclass != CLASS_STRUCT) {
      ctf_error(CTFERR_WRONGTYPE);
    } else {
      CTF_TYPE *field;
      assert(knowntype->fields != NULL);
      /* copy the fields (keep the order of declaration) */
      for (field = knowntype->fields->next; field != NULL; field = field->next) {
        CTF_EVENT_FIELD *newfield = (CTF_EVENT_FIELD*)malloc(sizeof(CTF_EVENT_FIELD));
        if (newfield != NULL) {
          CTF_EVENT_FIELD *tail;
          memcpy(newfield, field, sizeof(CTF_EVENT_FIELD));
          assert(field->identifier != NULL);
          strlcpy(newfield->name, field->identifier, sizearray(newfield->name));
          newfield->type.identifier = NULL; /* clear these in the copy, to avoid a double free() on clean-up */
          newfield->type.selector = NULL;
          newfield->type.keys = NULL;
          newfield->next = NULL;
          for (tail = fieldroot; tail->next != NULL; tail = tail->next)
            /* nothing */;
          assert(tail != NULL && tail->next == NULL);
          tail->next = newfield;
        }
      }
    }
    token_need(';');
  }
}

/** parse_enum() parses an enumeration from the root. For Enumerations with
 *  this syntax, a name is required (a type is optional).
 */
static void parse_enum(void)
{
  CTF_TYPE basetype;
  CTF_TYPE *type;

  type = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
  if (type == NULL) {
    ctf_error(CTFERR_MEMORY);
    return;
  }
  memset(type, 0, sizeof(CTF_TYPE));
  type->next = type_root.next;
  type_root.next = type;

  token_need(TOK_IDENTIFIER);
  strlcpy(type->name, token_gettext(), sizearray(type->name));

  if (token_match(':')) {
    parse_declaration(&basetype, NULL, 0);
    type->typeclass = basetype.typeclass;
    type->size = basetype.size;
    type->align = basetype.align;
    type->flags = basetype.flags; /* for signed/unsigned */
    close_declaration(&basetype);
  } else {
    type_default_int(&basetype);
    type->typeclass = basetype.typeclass;
    type->size = basetype.size;
    type->align = basetype.align;
    type->flags = basetype.flags;
  }
  if (type->typeclass != CLASS_INTEGER || type->size == 0 || type->length != 0)
    ctf_error(CTFERR_WRONGTYPE);  /* enumerations must be integer */
  type->typeclass = CLASS_ENUM;

  parse_enum_fields(type);  /* complete the declaration */
  token_match(';'); /* ';' after closing brace is optional */
}

/** parse_struct() parses a struct from the root. For struct with this syntax, a
 *  name is required.
 */
static void parse_struct(void)
{
  char identifier[CTF_NAME_LENGTH];
  CTF_TYPE *type;

  token_need(TOK_IDENTIFIER);
  strlcpy(identifier, token_gettext(), sizearray(identifier));
  if ((type = type_lookup(&type_root, identifier)) != NULL && (type->flags & TYPEFLAG_WEAK) == 0)
    ctf_error(CTFERR_TYPE_REDEFINE, identifier);

  type = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
  if (type == NULL) {
    ctf_error(CTFERR_MEMORY);
    return;
  }
  memset(type, 0, sizeof(CTF_TYPE));
  type->next = type_root.next;
  type_root.next = type;
  strlcpy(type->name, identifier, sizearray(type->name));
  type->typeclass = CLASS_STRUCT;

  parse_struct_fields(type);  /* complete the declaration */
  token_match(';'); /* ';' after closing brace is optional */
}

static void parse_typedef(void)
{
  CTF_TYPE type;
  char identifier[CTF_NAME_LENGTH];

  parse_declaration(&type, identifier, sizearray(identifier));
  token_need(';');

  if (type.size > 0 && strlen(identifier) > 0) {
    CTF_TYPE *newtype = type_lookup(&type_root, identifier);
    if (newtype != NULL && (newtype->flags & TYPEFLAG_WEAK) == 0)
      ctf_error(CTFERR_TYPE_REDEFINE, identifier);
    else if (newtype == NULL)
      newtype = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
    if (newtype != NULL) {
      memcpy(newtype, &type, sizeof(CTF_TYPE));
      newtype->flags |= TYPEFLAG_STRONG;
      strlcpy(newtype->name, identifier, sizearray(newtype->name));
      newtype->next = type_root.next;
      type_root.next = newtype;
    }
  }
  /* do not call close_declaration(&type) because the parsed type was copied */
}

static void parse_typealias(void)
{
  CTF_TYPE *type;
  int token;

  type = (CTF_TYPE*)malloc(sizeof(CTF_TYPE));
  if (type == NULL) {
    ctf_error(CTFERR_MEMORY);
    return;
  }
  memset(type, 0, sizeof(CTF_TYPE));
  type->next = type_root.next;
  type_root.next = type;

  token = token_next();
  switch (token) {
  case TOK_INTEGER:
    type->typeclass = CLASS_INTEGER;
    break;
  case TOK_FLOATING_POINT:
    type->typeclass = CLASS_FLOAT;
    break;
  case TOK_STRING:
    type->typeclass = CLASS_STRING;
    type->size = 8;
    break;
  case TOK_STRUCT:
    type->typeclass = CLASS_STRUCT;
    break;
  }

  parse_typealias_fields(type);
  type->flags |= TYPEFLAG_STRONG;

  if (!token_match(TOK_OP_TYPE_ASSIGN))
    token_need('=');
  token_need(TOK_IDENTIFIER);
  strlcpy(type->name, token_gettext(), sizearray(type->name));
  if (type->size == 0)
    ctf_error(CTFERR_TYPE_SIZE, type->name);

  token_need(';');
}

static void parse_trace(void)
{
  token_need('{');
  while (!token_match('}')) {
    int tok = token_next();
    if (tok == TOK_IDENTIFIER) {
      char identifier[CTF_NAME_LENGTH];
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      token_need('=');
      if (strcmp(identifier, "major") == 0) {
        token_need(TOK_LINTEGER);
        ctf_trace.major = (uint8_t)token_getlong();
      } else if (strcmp(identifier, "minor") == 0) {
        token_need(TOK_LINTEGER);
        ctf_trace.minor = (uint8_t)token_getlong();
      } else if (strcmp(identifier, "version") == 0) {
        token_need(TOK_LFLOAT);
        ctf_trace.major = (uint8_t)token_getreal();
        ctf_trace.minor = (uint8_t)(token_getreal() - ctf_trace.major) * 10;
      } else if (strcmp(identifier, "byte_order") == 0) {
        token_need(TOK_IDENTIFIER);
        ctf_trace.byte_order = (strcmp(token_gettext(), "be") == 0) ? BYTEORDER_BE : BYTEORDER_LE;
      } else if (strcmp(identifier, "uuid") == 0) {
        int idx;
        const char *ptr;
        token_need(TOK_LSTRING);
        /* convert string to byte array */
        memset(ctf_trace.uuid, 0, sizearray(ctf_trace.uuid));
        ptr = token_gettext();
        for (idx = 0; idx < sizearray(ctf_trace.uuid); idx++) {
          if (*ptr == '-')
            ptr++;
          if (!isxdigit(ptr[0]) || !isxdigit(ptr[1]))
            break;
          ctf_trace.uuid[idx] = (uint8_t)((hexdigit(ptr[0]) << 4) | hexdigit(ptr[1]));
        }
      }
      token_need(';');
    } else if (tok == TOK_PACKET) {
      token_need('.');
      if (token_match(TOK_HEADER)) {
        if (!token_match(TOK_OP_TYPE_ASSIGN))
          token_need('=');
        parse_packet_header();
      } else {
        ctf_error(CTFERR_INVALIDFIELD, token_gettext());
      }
    } else {
      ctf_error(CTFERR_NEEDTOKEN, '}', tok);
      if (tok == TOK_EOF)
        break;
    }
  }
  token_match(';'); /* ';' after closing brace is optional */
}

static void parse_clock(void)
{
  CTF_CLOCK *clock;

  /* add a clock */
  clock = (CTF_CLOCK*)malloc(sizeof(CTF_CLOCK));
  if (clock == NULL) {
    ctf_error(CTFERR_MEMORY);
    return;
  }
  memset(clock, 0, sizeof(CTF_CLOCK));
  clock->next = ctf_clock_root.next;
  ctf_clock_root.next = clock;

  if (token_match(TOK_IDENTIFIER))
    strlcpy(clock->name, token_gettext(), sizearray(clock->name));
  token_need('{');
  while (!token_match('}')) {
    int tok = token_next();
    if (tok == TOK_IDENTIFIER) {
      char identifier[CTF_NAME_LENGTH];
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      token_need('=');
      if (strcmp(identifier, "name") == 0) {
        token_need(TOK_IDENTIFIER);
        strlcpy(clock->name, token_gettext(), sizearray(clock->name));
      } else if (strcmp(identifier, "description") == 0) {
        token_need(TOK_LSTRING);
        strlcpy(clock->description, token_gettext(), sizearray(clock->description));
      } else if (strcmp(identifier, "uuid") == 0) {
        int idx;
        const char *ptr;
        token_need(TOK_LSTRING);
        /* convert string to byte array */
        memset(clock->uuid, 0, sizearray(clock->uuid));
        ptr = token_gettext();
        for (idx = 0; idx < sizearray(clock->uuid); idx++) {
          if (*ptr == '-')
            ptr++;
          if (!isxdigit(ptr[0]) || !isxdigit(ptr[1]))
            break;
          clock->uuid[idx] = (uint8_t)((hexdigit(ptr[0]) << 4) | hexdigit(ptr[1]));
        }
      } else if (strcmp(identifier, "freq") == 0) {
        token_need(TOK_LINTEGER);
        clock->frequeny = token_getlong();
      } else if (strcmp(identifier, "precision") == 0) {
        token_need(TOK_LINTEGER);
        clock->frequeny = token_getlong();
      } else if (strcmp(identifier, "offset") == 0) {
        token_need(TOK_LINTEGER);
        clock->offset = token_getlong();
      } else if (strcmp(identifier, "offset_s") == 0) {
        token_need(TOK_LINTEGER);
        clock->offset_s = token_getlong();
      } else if (strcmp(identifier, "absolute") == 0) {
        token_need(TOK_LINTEGER);
        clock->absolute = (int)token_getlong();
      }
      token_need(';');
    } else {
      ctf_error(CTFERR_NEEDTOKEN, '}', tok);
      if (tok == TOK_EOF)
        break;
    }
  }
  token_match(';'); /* ';' after closing brace is optional */

  /* check that the name is set and that it is unique */
  if (strlen(clock->name) == 0) {
    ctf_error(CTFERR_NAMEREQUIRED, "clock");
  } else {
    CTF_CLOCK *iter;
    for (iter = ctf_clock_root.next; iter != NULL; iter = iter->next)
      if (iter != clock && strcmp(iter->name, clock->name) == 0)
        ctf_error(CTFERR_DUPLICATE_NAME, clock->name);
  }
}

static void parse_stream(void)
{
  CTF_STREAM *stream, *iter;
  int streamid_set = 0;

  /* add a stream */
  stream = (CTF_STREAM*)malloc(sizeof(CTF_STREAM));
  if (stream == NULL) {
    ctf_error(CTFERR_MEMORY);
    return;
  }
  memset(stream, 0, sizeof(CTF_STREAM));
  stream->next = ctf_stream_root.next;
  ctf_stream_root.next = stream;

  if (token_match(TOK_IDENTIFIER))
    strlcpy(stream->name, token_gettext(), sizearray(stream->name));
  token_need('{');
  while (!token_match('}')) {
    int tok = token_next();
    if (tok == TOK_IDENTIFIER) {
      char identifier[CTF_NAME_LENGTH];
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      token_need('=');
      if (strcmp(identifier, "id") == 0) {
        token_need(TOK_LINTEGER);
        stream->stream_id = (int)token_getlong();
        streamid_set = 1;
      } else if (strcmp(identifier, "name") == 0) {
        token_need(TOK_IDENTIFIER);
        strlcpy(stream->name, token_gettext(), sizearray(stream->name));
      }
      token_need(';');
    } else if (tok == TOK_EVENT) {
      token_need('.');
      if (token_match(TOK_HEADER)) {
        if (!token_match(TOK_OP_TYPE_ASSIGN))
          token_need('=');
        parse_event_header(&stream->event, &stream->clock);
      } else {
        ctf_error(CTFERR_INVALIDFIELD, token_gettext());
      }
    } else {
      ctf_error(CTFERR_NEEDTOKEN, '}', tok);
      if (tok == TOK_EOF)
        break;
    }
  }
  token_match(';'); /* ';' after closing brace is optional */

  if (streamid_set) {
    /* check whether the id is unique */
    for (iter = ctf_stream_root.next; iter != NULL; iter = iter->next)
      if (iter != stream && iter->stream_id == stream->stream_id)
        ctf_error(CTFERR_DUPLICATE_ID);
  } else {
    /* assign stream_id to be 1 higher than the current highest */
    for (iter = ctf_stream_root.next; iter != NULL; iter = iter->next)
      if (iter != stream && stream->stream_id >= iter->stream_id)
        stream->stream_id = iter->stream_id + 1;
  }
}

static void parse_event(void)
{
  CTF_EVENT *event, *iter;
  const CTF_STREAM *stream;
  int id_set = 0;
  int streamid_set = 0;

  /* add an event */
  event = (CTF_EVENT*)malloc(sizeof(CTF_EVENT));
  if (event == NULL) {
    ctf_error(CTFERR_MEMORY);
    return;
  }
  memset(event, 0, sizeof(CTF_EVENT));
  /* append to the tail, so the order in the generated header file is the same
     as in the trace specification */
  for (iter = &ctf_event_root; iter->next != NULL; iter = iter->next)
    /* nothing */;
  iter->next = event;
  event->next = NULL;

  if (token_match(TOK_IDENTIFIER)) {
    char identifier[CTF_NAME_LENGTH];
    strlcpy(identifier, token_gettext(), sizearray(identifier));
    if (token_match(TOK_OP_NAMESPACE)) {
      /* name before the :: is the stream, what is after it is the event name */
      token_need(TOK_IDENTIFIER);
      strlcpy(event->name, token_gettext(), sizearray(event->name));
      stream = stream_by_name(identifier);
      if (stream != NULL)
        event->stream_id = stream->stream_id;
      else
        ctf_error(CTFERR_UNKNOWNSTREAM, identifier);
      streamid_set = 1;
    } else {
      /* stream not given, set only the event name */
      strlcpy(event->name, identifier, sizearray(event->name));
    }
  }
  token_need('{');
  while (!token_match('}')) {
    int tok = token_next();
    if (tok == TOK_IDENTIFIER) {
      if (strcmp(token_gettext(), "id") == 0) {
        token_need('=');
        token_need(TOK_LINTEGER);
        event->id = (int)token_getlong();
        id_set = 1;
      } else if (strcmp(token_gettext(), "stream_id") == 0) {
        token_need('=');
        if (token_match(TOK_LSTRING)) {
          stream = stream_by_name(token_gettext());
          if (stream != NULL)
            event->stream_id = stream->stream_id;
          else
            ctf_error(CTFERR_UNKNOWNSTREAM, token_gettext());
        } else {
          token_need(TOK_LINTEGER);
          event->stream_id = (int)token_getlong();
        }
        streamid_set = 1;
      } else if (strcmp(token_gettext(), "name") == 0) {
        token_need('=');
        token_need(TOK_IDENTIFIER);
        strlcpy(event->name, token_gettext(), sizearray(event->name));
      }
      token_need(';');
    } else if (tok == TOK_STREAM) {
      char identifier[CTF_NAME_LENGTH];
      token_need('.');
      token_need(TOK_IDENTIFIER);
      strlcpy(identifier, token_gettext(), sizearray(identifier));
      token_need('=');
      if (strcmp(token_gettext(), "id") == 0) {
        if (token_match(TOK_LSTRING)) {
          stream = stream_by_name(token_gettext());
          if (stream != NULL)
            event->stream_id = stream->stream_id;
          else
            ctf_error(CTFERR_UNKNOWNSTREAM, token_gettext());
        } else {
          token_need(TOK_LINTEGER);
          event->stream_id = (int)token_getlong();
        }
        streamid_set = 1;
      }
      token_need(';');
    } else if (tok == TOK_FIELDS) {
      if (!token_match(TOK_OP_TYPE_ASSIGN))
        token_need('=');
      parse_event_fields(&event->field_root);
    } else {
      ctf_error(CTFERR_NEEDTOKEN, '}', tok);
      if (tok == TOK_EOF)
        break;
    }
  }
  token_match(';'); /* ';' after closing brace is optional */

  if (strlen(event->name) == 0) {
    ctf_error(CTFERR_NAMEREQUIRED, "event");
  } else {
    for (iter = ctf_event_root.next; iter != NULL; iter = iter->next)
      if (iter != event && strcmp(iter->name, event->name) == 0)
        ctf_error(CTFERR_DUPLICATE_NAME, event->name);
  }

  if (id_set) {
    /* check whether the id is unique */
    for (iter = ctf_event_root.next; iter != NULL; iter = iter->next)
      if (iter != event && event->id == iter->id)
        ctf_error(CTFERR_DUPLICATE_ID);
  } else {
    /* assign the id to be 1 higher than the current highest */
    for (iter = ctf_event_root.next; iter != NULL; iter = iter->next)
      if (iter != event && event->id >= iter->id)
        event->id = iter->id + 1;
  }

  if (!streamid_set) {
    /* if there are multiple streams, each event should have a stream_id;
       if there is only one stream, the stream.id may only be omitted if the
       stream is defined with id 0 */
    int count = stream_count();
    if (count == 1) {
      stream = ctf_stream_root.next;
      if (stream->stream_id != 0)
        ctf_error(CTFERR_STREAM_NOTSET, event->name);
    } else if (count > 0) {
      ctf_error(CTFERR_STREAM_NOTSET, event->name);
    }
  }

  /* mark stream that this event is part of as active */
  if (stream_by_id(event->stream_id) == NULL
      && event_count(event->stream_id) == 2) /* warn for the 2nd event in this stream, but not for the 3rd, 4th, etc, */
    ctf_error(CTFERR_STREAM_NO_DEF, event->stream_id);
  ctf_trace.stream_mask |= (1 << event->stream_id);
}

/** ctf_parse_init() initializes the TSDL parser and sets up default types.
 *  It retuns 1 on success and 0 on error; the error message has then already
 *  been issued via ctf_error_notify().
 */
int ctf_parse_init(const char *filename)
{
  if (!readline_init(filename))
    return 0; /* error message already set via ctf_error() */
  if (!token_init())
    return 0; /* error message already set via ctf_error() */
  memset(&ctf_trace, 0, sizeof ctf_trace);
  memset(&ctf_packet, 0, sizeof ctf_packet);

  /* add default types */
  type_init(&type_root, "int8_t", CLASS_INTEGER, 8, TYPEFLAG_WEAK | TYPEFLAG_SIGNED);
  type_init(&type_root, "uint8_t", CLASS_INTEGER, 8, TYPEFLAG_WEAK);
  type_init(&type_root, "int16_t", CLASS_INTEGER, 16, TYPEFLAG_WEAK | TYPEFLAG_SIGNED);
  type_init(&type_root, "uint16_t", CLASS_INTEGER, 16, TYPEFLAG_WEAK);
  type_init(&type_root, "int32_t", CLASS_INTEGER, 32, TYPEFLAG_WEAK | TYPEFLAG_SIGNED);
  type_init(&type_root, "uint32_t", CLASS_INTEGER, 32, TYPEFLAG_WEAK);
  type_init(&type_root, "int64_t", CLASS_INTEGER, 64, TYPEFLAG_WEAK | TYPEFLAG_SIGNED);
  type_init(&type_root, "uint64_t", CLASS_INTEGER, 64, TYPEFLAG_WEAK);

  error_count = 0;

  return 1;
}

void ctf_parse_cleanup(void)
{
  readline_cleanup();
  token_cleanup();
  clock_cleanup();
  stream_cleanup();
  event_cleanup();
  type_cleanup(&type_root);
  memset(&ctf_trace, 0, sizeof ctf_trace); /* to reset the active streams mask */
}

/** ctf_parse_run() runs the TSDL parser. It returns 1 on success and 0 if one
 *  or more errors were found. The error messages have then already been issued
 *  via ctf_parse_run().
 */
int ctf_parse_run(void)
{
  int tok;

  while ((tok = token_next()) != TOK_EOF) {
    switch (tok) {
    case TOK_ENV:
      //??? error: feature not implemented
      break;
    case TOK_ENUM:
      parse_enum();
      break;
    case TOK_STRUCT:
      parse_struct();
      break;
    case TOK_TYPEDEF:
      parse_typedef();
      break;
    case TOK_TYPEALIAS:
      parse_typealias();
      break;
    case TOK_TRACE:
      parse_trace();
      break;
    case TOK_CLOCK:
      parse_clock();
      break;
    case TOK_STREAM:
      parse_stream();
      break;
    case TOK_EVENT:
      parse_event();
      break;
    case TOK_CALLSITE:
      //??? error: feature not implemented
      break;
    default:
      ctf_error(CTFERR_SYNTAX_MAIN);
    }
  }
  return error_count == 0;
}

