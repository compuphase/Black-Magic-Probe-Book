/*
 * Trace function generation utility: it creates a .c and a .h file with
 * functions that create packets conforming to the Common Trace Format (CTF).
 * The * input is a file in the Trace Stream Description Language (TDSL), the
 * primary specification language for CTF.
 *
 * Copyright 2019-2024 CompuPhase
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
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svnrev.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __linux__
# include <bsd/string.h>
#elif defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
# include "strlcpy.h"
#endif

#include "parsetsdl.h"

typedef struct tagPATHLIST {
  struct tagPATHLIST *next;
  char *path;
  int system; /* 1 for system header, 0 for project-local header */
} PATHLIST;

#if !defined _MAX_PATH
# define _MAX_PATH 260
#endif

#if !defined sizearray
# define sizearray(a)  (sizeof(a) / sizeof((a)[0]))
#endif

#if defined WIN32 || defined _WIN32
# define DIR_SEPARATOR "\\"
# define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
# define DIR_SEPARATOR "/"
# define IS_OPTION(s)  ((s)[0] == '-')
#endif

#define FLAG_MACRO        0x0001
#define FLAG_INDENT       0x0002
#define FLAG_BASICTYPES   0x0004
#define FLAG_STREAMID     0x0008
#define FLAG_C99          0x0010
#define FLAG_NO_INSTR     0x0020
#define FLAG_STREAM_MASK  0x0040
#define FLAG_SEVERITY_LVL 0x0080


int ctf_error_notify(int code, const char *filename, int linenr, const char *message)
{
  (void)code; /* unused */
  if (linenr > 0)
    fprintf(stderr, "ERROR %s line %d: ", filename, linenr);
  else
    fprintf(stderr, "ERROR: ");
  assert(message != NULL);
  fprintf(stderr, "%s\n", message);
  return 0;
}

static const char *type_to_string(const CTF_TYPE *type, char *typedesc, int size, unsigned flags)
{
  assert(typedesc != NULL);
  *typedesc = '\0';
  switch (type->typeclass) {
  case CLASS_INTEGER:
  case CLASS_ENUM:
    if (type->flags & TYPEFLAG_SIGNED) {
      if (type->size == 8)
        strlcat(typedesc, "signed ", size); /* only on char signed is explicitly set */
    } else {
      strlcat(typedesc, "unsigned ", size);
    }
    switch (type->size) {
    case 8:
      strlcat(typedesc, "char", size);
      break;
    case 16:
      strlcat(typedesc, "short", size);
      break;
    case 32:
      strlcat(typedesc, "long", size);
      break;
    case 64:
      strlcat(typedesc, "long long", size);
      break;
    }
    break;
  case CLASS_FLOAT:
    if (type->size == 32) {
      strlcat(typedesc, "float", size);
    } else {
      assert(type->size == 64);
      strlcat(typedesc, "double", size);
    }
    break;
  case CLASS_BOOL:
    if (flags & FLAG_C99)
      strlcat(typedesc, "_Bool", size);
    else
      strlcat(typedesc, "int", size);
    break;
  case CLASS_STRING:
    strlcat(typedesc, "const char*", size);
    break;
  case CLASS_STRUCT:
    //??? anonymous struct, not supported
    break;
  case CLASS_VARIANT:
    //??? anonymous variant, not supported
    break;
  default:
    assert(0);
  }
  return typedesc;
}

static void dumphex(FILE *fp, const unsigned char *value, int size)
{
  while (size > 0) {
    fprintf(fp, "0x%02x", *value);
    if (size > 1)
      fprintf(fp, ", ");
    value++;
    size--;
  }
}

static const char *generate_symbolname(char *symbol, size_t size, const char *name)
{
  char *ptr;

  assert(symbol != NULL);
  assert(name != NULL);
  assert(size > 0);
  strlcpy(symbol, name, size);
  while ((ptr = strpbrk(symbol, " ~@#$%^*-+=<>()[]{};.,?!/\\")) != NULL)
    *ptr = '_';
  return symbol;
}

static int generate_functionheader(FILE *fp, const CTF_EVENT *evt, unsigned flags)
{
  if ((flags & FLAG_NO_INSTR)) {
    if (flags & FLAG_INDENT)
      fprintf(fp, "  ");
    fprintf(fp, "__attribute__((no_instrument_function))\n");
  }

  if (evt->attribute != NULL) {
    if (flags & FLAG_INDENT)
      fprintf(fp, "  ");
    fprintf(fp, "__attribute__((%s))\n", evt->attribute);
  }

  if (flags & FLAG_INDENT)
    fprintf(fp, "  ");

  /* the function name */
  if (flags & FLAG_MACRO)
    fprintf(fp, "#define trace_");
  else
    fprintf(fp, "void trace_");

  char streamname[128];
  char funcname[128];
  const CTF_STREAM *stream = stream_by_id(evt->stream_id);
  if (stream == NULL || (strlen(stream->name) == 0 && stream_count() == 1 && stream->stream_id == 0))
    fprintf(fp, "%s", generate_symbolname(funcname, sizearray(funcname), evt->name));
  else if (strlen(stream->name) == 0)
    fprintf(fp, "%d_%s", stream->stream_id, generate_symbolname(funcname, sizearray(funcname), evt->name));
  else
    fprintf(fp, "%s_%s",
            generate_symbolname(streamname, sizearray(streamname), stream->name),
            generate_symbolname(funcname, sizearray(funcname), evt->name));

  fprintf(fp, "(");

  /* the parameters */
  int paramcount = 0;
  for (const CTF_EVENT_FIELD *field = evt->field_root.next; field != NULL; field = field->next) {
    if (field != evt->field_root.next)
      fprintf(fp, ", ");
    if (!(flags & FLAG_MACRO)) {
      char typedesc[64] = "";
      if ((flags & FLAG_BASICTYPES)                                         /* translation to basic types is requested */
          || field->type.typeclass == CLASS_ENUM                            /* always translate TSDL enums to C type */
          || (field->type.typeclass == CLASS_BOOL && !(flags & FLAG_C99))   /* translate TSDL "bool" type to "int" for C90 */
          || strlen(field->type.name) == 0)                                 /* make type if TSDL typename is anonymous */
        type_to_string(&field->type, typedesc, sizearray(typedesc), flags);
      if (strlen(typedesc)>0) {
        fprintf(fp, "%s ", typedesc);
      } else if (field->type.typeclass == CLASS_STRUCT) {
        /* check whether this is a typedef declaration, if not -> add "struct"
           in front of the name */
        if (field->type.flags & TYPEFLAG_STRONG)
          fprintf(fp, "const %s* ", field->type.name);
        else
          fprintf(fp, "const struct %s* ", field->type.name);
      } else {
        fprintf(fp, "%s ", field->type.name);
      }
    }
    fprintf(fp, "%s", field->name);
    paramcount++;
  }
  if (paramcount == 0)
    fprintf(fp, "void");

  fprintf(fp, ")");
  return 1;
}

void generate_prototypes(FILE *fp, unsigned flags, const char *trace_func,
                         const char *timestamp_func, const PATHLIST *includepaths)
{
  const CTF_EVENT *evt;
  const CTF_STREAM *stream;
  int seqnr;

  /* file header */
  fprintf(fp, "/*\n"
              " * Trace functions header file, generated by tracegen\n"
              " */\n"
              "#ifndef TRACEGEN_PROTOTYPE_FUNCTIONS\n"
              "#define TRACEGEN_PROTOTYPE_FUNCTIONS\n\n");

  if (flags & FLAG_C99) {
    fprintf(fp, "#include <stdbool.h>\n");
    fprintf(fp, "#include <stdint.h>\n");
  }
  if (includepaths != NULL) {
    const PATHLIST *path;
    for (path = includepaths->next; path != NULL; path = path->next)
      if (path->system)
        fprintf(fp, "#include <%s>\n", path->path);
    for (path = includepaths->next; path != NULL; path = path->next)
      if (!path->system)
        fprintf(fp, "#include \"%s\"\n", path->path);
    fprintf(fp, "\n");
  }

  assert(trace_func != NULL && strlen(trace_func) > 0);
  if (flags & FLAG_STREAMID)
    fprintf(fp, "void %s(unsigned stream_id, const unsigned char *data, unsigned size);\n", trace_func);
  else
    fprintf(fp, "void %s(const unsigned char *data, unsigned size);\n", trace_func);
  /* assume all all streams to have compatible clocks, so get only the first clock */
  for (seqnr = 0; (stream = stream_by_seqnr(seqnr)) != NULL && stream->clock == NULL; seqnr++)
    {}
  if (stream != NULL) {
    char typedesc[64];
    /* the clock type must be converted to a standard C type, because the
       TSDL type is not compatible with C */
    assert(stream->clock != NULL);
    assert(timestamp_func != NULL && strlen(timestamp_func) > 0);
    fprintf(fp, "%s %s(void);\n", type_to_string(stream->clock, typedesc, sizearray(typedesc), flags), timestamp_func);
  }
  fprintf(fp, "\n");

  flags &= ~FLAG_NO_INSTR;  /* set the attribute only on the implementation */
  for (evt = event_next(NULL); evt != NULL; evt = event_next(evt)) {
    /* #ifdef NTRACE wrapper */
    fprintf(fp, "#ifdef NTRACE\n");
    generate_functionheader(fp, evt, flags | FLAG_INDENT | FLAG_MACRO);
    fprintf(fp, "\n#else\n");
    generate_functionheader(fp, evt, flags | FLAG_INDENT);
    fprintf(fp, ";\n#endif\n\n");
  }

  /* file trailer */
  fprintf(fp, "#endif /* TRACEGEN_PROTOTYPE_FUNCTIONS */\n");
}

bool generate_funcstubs(FILE *fp, unsigned flags, const char *trace_func,
                        const char *timestamp_func, const char *headerfile,
                        int severitylevel, unsigned long streammask)
{
  bool ok = true;

  /* file header */
  assert(fp != NULL);
  assert(headerfile != NULL);
  fprintf(fp, "/*\n"
              " * Trace functions implementation file, generated by tracegen\n"
              " */\n"
              "#ifndef NTRACE\n"
              "#include <string.h>\n");

  if (!(flags & FLAG_C99))
    fprintf(fp, "#include <alloca.h>\n");
  assert(headerfile != NULL && strlen(headerfile) > 0);
  fprintf(fp, "#include \"%s\"\n\n", headerfile);

  if (flags & FLAG_STREAM_MASK)
    fprintf(fp, "unsigned long trace_stream_mask = %08lxLu;\n", streammask);
  if (flags & FLAG_SEVERITY_LVL)
    fprintf(fp, "unsigned char trace_severity_level = %d;\n", severitylevel);
  if (flags & (FLAG_STREAM_MASK | FLAG_SEVERITY_LVL))
    fprintf(fp, "\n");

  const CTF_EVENT *evt;
  for (evt = event_next(NULL); evt != NULL; evt = event_next(evt)) {
    const CTF_PACKET_HEADER *pkthdr = packet_header();
    const CTF_STREAM *stream = stream_by_id(evt->stream_id);
    const CTF_EVENT_HEADER *evthdr = (stream != NULL) ? &stream->event : NULL;

    generate_functionheader(fp, evt, flags);
    fprintf(fp, "\n{\n");

    char xmit_call[40];
    if (flags & FLAG_STREAMID)
      sprintf(xmit_call, "%s(%d, ", trace_func, (stream != NULL) ? stream->stream_id : 0);
    else
      sprintf(xmit_call, "%s(", trace_func);

    /* go through the options and arguments to determine the fixed size of the
       trace message (the size excluding arguments that are variable length
       strings) */
    int stringcount = 0;
    int fixedsz = 0;
    assert(pkthdr != NULL);
    int headersz = pkthdr->header.magic_size / 8;
    if (pkthdr->header.streamid_size > 0)
      headersz += pkthdr->header.streamid_size / 8;
    if (evthdr != NULL && evthdr->header.id_size > 0)
      headersz += evthdr->header.id_size / 8;
    if (evthdr != NULL && evthdr->header.timestamp_size > 0)
      fixedsz += evthdr->header.timestamp_size / 8;
    /* for strings, only add the zero terminator byte, the parameter lengths
       are added later (by generating a call to strlen) */
    const CTF_EVENT_FIELD *field;
    for (field = evt->field_root.next; field != NULL; field = field->next) {
      if (field->type.typeclass == CLASS_STRING)
        stringcount += 1;  /* count the number of string parameters */
      else
        fixedsz += field->type.size / 8;
    }

    /* check options for filtering */
    const char *indent = "  ";
    if (flags & (FLAG_STREAM_MASK | FLAG_SEVERITY_LVL)) {
      if ((flags & FLAG_STREAM_MASK) && stream->stream_id >= (8*sizeof(unsigned long))) {
        if (strlen(stream->name) > 0)
          fprintf(stderr, "ERROR: stream '%s' has id %d, which is larger than the stream mask\n", stream->name, stream->stream_id);
        else
          fprintf(stderr, "ERROR: anonymous stream has id %d, which is larger than the stream mask\n", stream->stream_id);
        ok = false;
      }
      if ((flags & (FLAG_STREAM_MASK | FLAG_SEVERITY_LVL)) == (FLAG_STREAM_MASK | FLAG_SEVERITY_LVL))
        fprintf(fp, "%sif ((trace_stream_mask & 0x%08lxLu) && trace_severity_level <= %d) {\n",
                indent, 1Lu << stream->stream_id, evt->severity);
      else if (flags & FLAG_STREAM_MASK)
        fprintf(fp, "%sif (trace_stream_mask & 0x%08lxLu) {\n", indent, 1Lu<<stream->stream_id);
      else
        fprintf(fp, "%sif (trace_severity_level <= %d) {\n", indent, evt->severity);
      indent = "    ";
    }

    /* handle the constant part of the headers */
    int pos = 0;
    if (headersz > 0) {
      fprintf(fp, "%sstatic const unsigned char header[%d] = {", indent, headersz);
      /* check for a packet header (for stream-based protocols, there should be one) */
      assert(pkthdr != NULL);
      switch (pkthdr->header.magic_size) {
      case 8:
        fprintf(fp, "0xc1");
        break;
      case 16:
        fprintf(fp, "0xc1, 0x1f");
        break;
      case 32:
        fprintf(fp, "0xc1, 0x1f, 0xfc, 0xc1");
        break;
      }
      pos = pkthdr->header.magic_size / 8;
      if (pkthdr->header.streamid_size > 0) {
        unsigned long val;
        if (pos > 0)
          fprintf(fp, ", ");
        val = (stream != NULL) ? stream->stream_id : 0;
        dumphex(fp, (unsigned char*)&val, pkthdr->header.streamid_size / 8);
        pos += pkthdr->header.streamid_size / 8;
      }
      /* check for an event header (there really should be one)
         note that only the id is handled here (because it is constant for the
         function); the timestamp is dynamic and copied into the array later */
      if (evthdr != NULL && evthdr->header.id_size > 0) {
        if (pos > 0)
          fprintf(fp, ", ");
        dumphex(fp, (unsigned char*)&evt->id, evthdr->header.id_size / 8);
        pos += evthdr->header.id_size / 8;
      }
      fprintf(fp, " };\n");
    }
    assert(pos == headersz);
    /* check whether the timestamp must be stored (and its type) and create
       a variable for it */
    if (evthdr != NULL && evthdr->header.timestamp_size > 0) {
      char typedesc[64];
      const CTF_TYPE *clock = stream->clock;
      assert(clock != NULL);
      assert(timestamp_func != NULL && strlen(timestamp_func) > 0);
      /* the clock type must be converted to a standard C type, because the
         TSDL type is not compatible with C */
      fprintf(fp, "%s%s tstamp = %s();\n", indent, type_to_string(clock, typedesc, sizearray(typedesc), flags), timestamp_func);
    }
    /* if there are string parameters, create variables for their lengths and
       their positions in the buffer */
    const char *var_totallength = NULL;
    const char *var_index = NULL;
    if (stringcount > 0) {
      int count = 0;
      if (stringcount > 1)
        fprintf(fp, "%sunsigned index = 0;\n", indent);
      for (field = evt->field_root.next; field != NULL; field = field->next)
        if (field->type.typeclass == CLASS_STRING)
          fprintf(fp, "%sunsigned length%d = strlen(%s);\n", indent, count++, field->name);
      if (stringcount == 1) {
        var_totallength = "length0";
        var_index = "length0";
      } else {
        int idx;
        var_totallength = "totallength";
        var_index = "index";
        fprintf(fp, "%sunsigned %s = ", indent, var_totallength);
        for (idx = 0; idx < count; idx++) {
          if (idx > 0)
            fprintf(fp, " + ");
          fprintf(fp, "length%d", idx);
        }
        fprintf(fp, ";\n");
      }
    }

    if (stringcount == 0 && fixedsz == 0) {
      /* if there are no parameters and no timestamp, there is no variable part
         in the message, so the generated code can be very simple */
      fprintf(fp, "%s%sheader, %d);\n", indent, xmit_call, headersz);
    } else {
      int seq;
      /* create a variable for the buffer (the stringcount count is added to the
         fixed size because the zero byte of each string must be allocated too */
      if ((flags & FLAG_C99) == 0 || stringcount == 0)
        fprintf(fp, "%sunsigned char buffer[%d", indent, headersz + fixedsz + stringcount);
      else
        fprintf(fp, "%sunsigned char *buffer = alloca(%d", indent, headersz + fixedsz + stringcount);
      if (stringcount > 0)
        fprintf(fp, " + %s", var_totallength);
      if ((flags & FLAG_C99) == 0 || stringcount == 0)
        fprintf(fp, "];\n");
      else
        fprintf(fp, ");\n");
      /* copy the fixed header to the buffer */
      if (headersz > 0)
        fprintf(fp, "%smemcpy(buffer, header, %d);\n", indent, headersz);
      /* copy the timestamp */
      if (evthdr != NULL && evthdr->header.timestamp_size > 0) {
        fprintf(fp, "%smemcpy(buffer + %d, &tstamp, %d);\n", indent, headersz, evthdr->header.timestamp_size / 8);
        pos += evthdr->header.timestamp_size / 8;
      }
      /* the parameters */
      seq = 0;
      for (field = evt->field_root.next; field != NULL; field = field->next) {
        if (seq == 0) {
          fprintf(fp, "%smemcpy(buffer + %d, ", indent, pos);
        } else {
          assert(var_index != NULL);
          fprintf(fp, "%smemcpy(buffer + %d + %s, ", indent, pos, var_index);
        }
        if (field->type.typeclass == CLASS_INTEGER || field->type.typeclass == CLASS_BOOL
            || field->type.typeclass == CLASS_FLOAT || field->type.typeclass == CLASS_ENUM
            || field->type.typeclass == CLASS_STRUCT)
          fprintf(fp, "&");
        fprintf(fp, "%s, ", field->name);
        if (field->type.typeclass == CLASS_STRING) {
          fprintf(fp, "length%d + 1);\n", seq);
          if (stringcount == 1)
            pos += 1; /* for the zero byte */
          else
            fprintf(fp, "%sindex += length%d + 1;\n", indent, seq);
          seq++;
        } else {
          fprintf(fp, "%u);\n", field->type.size / 8);
          pos += field->type.size / 8;
        }
      }

      fprintf(fp, "%s%sbuffer, %d", indent, xmit_call, headersz + fixedsz + stringcount);
      if (stringcount > 0) {
        assert(var_totallength != NULL);
        fprintf(fp, " + %s", var_totallength);
      }
      fprintf(fp, ");\n");
    }

    if (flags & (FLAG_STREAM_MASK | FLAG_SEVERITY_LVL))
      fprintf(fp, "  }\n"); /* end "if" block for filtering on stream id or severity level */
    fprintf(fp, "}\n\n");   /* end function */
  }

  /* file trailer */
  fprintf(fp, "#endif /* NTRACE */\n");
  return ok;
}

static const char *skip_opt(const char *opt, int count)
{
  opt += count;
  if (*opt == '=' || *opt == ':')
    opt += 1;
  return opt;
}

#define MAX_STREAMS 32
static char *enabled_streams[MAX_STREAMS] = { NULL };

static int collect_streams(const char *list)
{
  memset(enabled_streams, 0, sizeof(enabled_streams));
  assert(list != NULL);
  int count = 0;
  while (*list != '\0') {
    const char *tail = strchr(list, ',');
    if (tail == NULL)
      tail = list + strlen(list);
    size_t len = tail - list;
    if (len > 0) {
      if (count >= MAX_STREAMS) {
        fprintf(stderr, "Too many stream names listed on '-f=streams' option\n");
        return count;
      }
      enabled_streams[count] = malloc((len + 1) * sizeof(char));
      if (enabled_streams[count] == NULL) {
        fprintf(stderr, "Memory allocation failure\n");
        return count;
      }
      strncpy(enabled_streams[count], list, len);
      enabled_streams[count][len] = '\0';
      count++;
    }
    list = tail;
    if (*list == ',')
      list++;
  }
  return count;
}

static void free_streams(void)
{
  for (int idx = 0; idx < sizearray(enabled_streams); idx++) {
    if (enabled_streams[idx] != NULL) {
      free(enabled_streams[idx]);
      enabled_streams[idx] = NULL;
    }
  }
}

static void usage(int status)
{
  printf("\ntragegen - generate C source & header files from TSDL specifications, for\n"
         "           tracing in the Common Trace Format.\n\n"
         "Usage: tracegen [options] inputfile\n\n"
         "Options:\n"
         "-c=99      Generate C99-compatible code (default is C90).\n"
         "-c=basic   Force basic C types on arguments, if available.\n"
         "-f=level   Generate code to enable/disable message severity levels.\n"
         "           The initial level may be set in the option, e.g. '-f=level:3' to set\n"
         "           'warning' level. If not specified, the initial level is 1 ('info').\n"
         "           Note that this is only the initial level; a debugger or trace viewer\n"
         "           may overrule this setting at run-time.\n"
         "-f=stream  Generate code to enable/disable streams.\n"
         "           The names of the initially active streams may be appended to the\n"
         "           option, e.g. '-f=stream:main,graphics' (enables streams 'main' and\n"
         "           'graphics' by default, all others disabled). If not specified, all\n"
         "           streams are initially enabled. Note that the enabled streams are\n"
         "           only the initial status; a debugger or trace viewer may overrule\n"
         "           this setting at run-time.\n"
         "-fs=name   Set the name for the time stamp function, default: 'trace_timestamp'\n"
         "-fx=name   Set the name for the trace transmit function, default: 'trace_xmit'\n"
         "-i=path    Add an '#include <...>' directive with this path.\n"
         "-I=path    Add an '#include \"...\"' directive with this path.\n"
         "           The '-i' and '-I' options may appear multiple times.\n"
         "-no-instr  Add a 'no_instrument_function' attribute to all generated functions.\n"
         "-o=name    Base output filename; a .c and .h suffix are added to this name.\n"
         "-s=swo     SWO tracing: use SWO channels for stream ids.\n"
         "-v         Show version information.\n");
  exit(status);
}

static void unknown_option(const char *option)
{
  fprintf(stderr, "Unknown option \"%s\"; use option -h for help.\n", option);
  exit(EXIT_FAILURE);
}

static void incomplete_option(const char *option)
{
  fprintf(stderr, "Missing parameter or value in option \"%s\"; use option -h for help.\n", option);
  exit(EXIT_FAILURE);
}

static void version(int status)
{
  printf("tracegen version %s.\n", SVNREV_STR);
  printf("Copyright 2019-2024 CompuPhase\nLicensed under the Apache License version 2.0\n");
  exit(status);
}

int main(int argc, char *argv[])
{
  if (argc <= 1)
    usage(EXIT_FAILURE);

  /* command line options */
  char infile[_MAX_PATH] = "";
  char outfile[_MAX_PATH] = "";
  char trace_func[64] = "trace_xmit";
  char timestamp_func[64] = "trace_timestamp";
  PATHLIST includepaths = { NULL, NULL };
  PATHLIST *path;
  unsigned opt_flags = 0;
  int opt_severity = 1;
  const char *opt;
  for (int idx = 1; idx < argc; idx++) {
    if (IS_OPTION(argv[idx])) {
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage(EXIT_SUCCESS);
        break;
      case 'c':
        opt = skip_opt(argv[idx], 2);
        if (strcmp(argv[idx]+1, "99") == 0)
          opt_flags |= FLAG_C99;
        else if (strcmp(argv[idx]+1, "basic") == 0)
          opt_flags |= FLAG_BASICTYPES;
        else
          unknown_option(argv[idx]);
        break;
      case 'f':
        opt = &argv[idx][2];
        if (strcmp(opt, "stream") == 0) {
          opt_flags |= FLAG_STREAM_MASK;
          opt = skip_opt(opt, 6);
          collect_streams(opt);
        } else if (*opt == 's' || *opt == 'x') {
          const char *name = skip_opt(opt, 1);
          if (*name == '\0')
            incomplete_option(argv[idx]);
          switch (*opt) {
          case 's':
            strlcpy(timestamp_func, name, sizearray(timestamp_func));
            break;
          case 'x':
            strlcpy(trace_func, name, sizearray(trace_func));
            break;
          }
        } else {
          opt = skip_opt(opt, 0);
          if (strcmp(opt, "stream") == 0) {
            opt_flags |= FLAG_STREAM_MASK;
            opt = skip_opt(opt, 6);
            collect_streams(opt);
          } else if (strncmp(opt, "level", 5) == 0) {
            opt_flags |= FLAG_SEVERITY_LVL;
            opt = skip_opt(opt, 5);
            if (*opt != '\0') {
              opt_severity = strtol(opt, NULL, 10);
              if (opt_severity < 0 || opt_severity > 6) {
                fprintf(stderr, "Invalid level %d for -f=level option.\n", opt_severity);
                opt_severity = 1;
              }
            }
          } else {
            unknown_option(argv[idx]);
          }
        }
        break;
      case 'I':
      case 'i':
        opt = skip_opt(argv[idx], 2);
        path = malloc(sizeof(PATHLIST));
        if (path != NULL) {
          path->path = strdup(opt);
          if (path->path != NULL) {
            PATHLIST *last;
            path->system =(argv[idx][1]== 'i');
            path->next = NULL;
            for (last = &includepaths; last->next != NULL; last = last->next)
              {}
            last->next = path;
          } else {
            free(path);
          }
        }
        break;
      case 'n':
        if (strcmp(argv[idx]+1, "no-instr") == 0)
          opt_flags |= FLAG_NO_INSTR;
        else
          unknown_option(argv[idx]);
        break;
      case 'o':
        opt = skip_opt(argv[idx], 2);
        strlcpy(outfile, opt, sizearray(outfile));
        break;
      case 's':
        opt = &argv[idx][2];
        if (*opt == '\0') {
          opt_flags |= FLAG_STREAMID; /* old option, now -s=swo */
        } else {
          opt = skip_opt(opt, 0);
          if (strcmp(opt, "swo") == 0)
            opt_flags |= FLAG_STREAMID;
          else
            unknown_option(argv[idx]);
        }
        break;
      case 't':
        opt_flags |= FLAG_BASICTYPES; /* old option, now -c=basic */
        break;
      case 'v':
        version(EXIT_SUCCESS);
        break;
      default:
        unknown_option(argv[idx]);
      }
    } else {
      strlcpy(infile, argv[idx], sizearray(infile));
    }
  }
  if (strlen(infile) == 0) {
    fprintf(stderr, "No input file specified.\n");
    return EXIT_FAILURE;
  }
  if (strlen(outfile) == 0) {
#   if defined _WIN32
      char *ptr = infile;
      if (strrchr(ptr, '\\') != NULL)
        ptr = strrchr(ptr, '\\') + 1;
      if (strrchr(ptr, '/') != NULL)
        ptr = strrchr(ptr, '/') + 1;
#   else
      char *ptr = strrchr(infile, '/');
      ptr = (ptr != NULL) ? ptr + 1 : infile;
#   endif
    strlcat(outfile, "trace_", sizearray(outfile));
    strlcat(outfile, ptr, sizearray(outfile));
    if ((ptr = strrchr(outfile, '.')) != NULL)
      *ptr = '\0';
  }

  if (!ctf_parse_init(infile))
    return EXIT_FAILURE;  /* error message already issued via ctf_error_notify() */
  if (ctf_parse_run()) {
    bool ok = true;       /* toggled to false when an error message is printed */

    /* create mask from streams */
    unsigned long stream_mask = ~0;
    if (opt_flags & FLAG_STREAM_MASK) {
      stream_mask = 0;
      for (int idx = 0; idx < sizearray(enabled_streams); idx++) {
        const CTF_STREAM *stream = stream_by_name(enabled_streams[idx]);
        if (stream != NULL)
          stream_mask |= stream->stream_id;
      }
    }

    strlcat(outfile, ".h", sizearray(outfile));
    FILE *fp = fopen(outfile, "wt");
    if (fp != NULL) {
      generate_prototypes(fp, opt_flags, trace_func, timestamp_func, &includepaths);
      fclose(fp);
    } else {
      fprintf(stderr, "Error writing file \"%s\", error %d.\n", outfile, errno);
      ok = false;
    }

    char *ptr = strrchr(outfile, '.');
    assert(ptr != NULL);
    *(ptr + 1) = 'c';
    fp = fopen(outfile, "wt");
    if (fp != NULL) {
      /* temporarily rename the extension back to .h */
      assert(ptr != NULL && *(ptr + 1) == 'c');
      *(ptr + 1) = 'h';
      ok = generate_funcstubs(fp, opt_flags, trace_func, timestamp_func, outfile,
                              opt_severity, stream_mask);
      assert(ptr != NULL && *(ptr + 1) == 'h');
      *(ptr + 1) = 'c';
      fclose(fp);
    } else {
      fprintf(stderr, "Error writing file \"%s\", error %d.\n", outfile, errno);
      ok = false;
    }

    if (ok)
      printf("Generated \"%s\".\n", outfile);
  }

  free_streams();
  ctf_parse_cleanup();
  while (includepaths.next != NULL) {
    path = includepaths.next;
    includepaths.next = path->next;
    free(path->path);
    free(path);
  }

# if defined FORTIFY
    Fortify_CheckAllMemory();
    Fortify_ListAllMemory();
# endif
  return EXIT_SUCCESS;
}

