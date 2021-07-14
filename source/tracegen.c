/*
 * Trace function generation utility: it creates a .c and a .h file with
 * functions that create packets conforming to the Common Trace Format (CTF).
 * The * input is a file in the Trace Stream Description Language (TDSL), the
 * primary specification language for CTF.
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

#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined __linux__
  #include <bsd/string.h>
#elif defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
  #include "strlcpy.h"
#endif

#include "parsetsdl.h"

typedef struct tagPATHLIST {
  struct tagPATHLIST *next;
  char *path;
  int system; /* 1 for system header, 0 for project-local header */
} PATHLIST;

#if !defined _MAX_PATH
  #define _MAX_PATH 260
#endif

#if !defined sizearray
  #define sizearray(a)  (sizeof(a) / sizeof((a)[0]))
#endif

#if defined WIN32 || defined _WIN32
  #define DIR_SEPARATOR "\\"
  #define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
  #define DIR_SEPARATOR "/"
  #define IS_OPTION(s)  ((s)[0] == '-')
#endif

#define FLAG_MACRO      0x0001
#define FLAG_INDENT     0x0002
#define FLAG_BASICTYPES 0x0004
#define FLAG_STREAMID   0x0008
#define FLAG_C99        0x0010
#define FLAG_NO_INSTR   0x0020


int ctf_error_notify(int code, int linenr, const char *message)
{
  (void)code; /* unused */
  if (linenr > 0)
    fprintf(stderr, "ERROR on line %d: ", linenr);
  else
    fprintf(stderr, "ERROR: ");
  fprintf(stderr, "%s\n", message);
  return 0;
}

static const char *type_to_string(const CTF_TYPE *type, char *typedesc, int size)
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
  const CTF_STREAM *stream;
  const CTF_EVENT_FIELD *field;
  char streamname[128], funcname[128];
  int paramcount;

  if (flags & FLAG_NO_INSTR) {
    if (flags & FLAG_INDENT)
      fprintf(fp, "  ");
    fprintf(fp, "__attribute__((no_instrument_function))\n");
  }

  if (flags & FLAG_INDENT)
    fprintf(fp, "  ");

  /* the function name */
  if (flags & FLAG_MACRO)
    fprintf(fp, "#define trace_");
  else
    fprintf(fp, "void trace_");

  stream = stream_by_id(evt->stream_id);
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
  paramcount = 0;
  for (field = evt->field_root.next; field != NULL; field = field->next) {
    if (field != evt->field_root.next)
      fprintf(fp, ", ");
    if (!(flags & FLAG_MACRO)) {
      char typedesc[64] = "";
      if ((flags & FLAG_BASICTYPES) || strlen(field->type.name) == 0)
        type_to_string(&field->type, typedesc, sizearray(typedesc));
      if (strlen(typedesc) > 0) {
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

void generate_prototypes(FILE *fp, unsigned flags, const char *trace_func, const PATHLIST *includepaths)
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

  if (flags & FLAG_C99)
    fprintf(fp, "#include <stdint.h>\n");
  if (includepaths != NULL) {
    const PATHLIST *path;
    for (path = includepaths->next; path != NULL; path = path->next) {
      if (path->system)
        fprintf(fp, "#include <%s>\n\n", path->path);
      else
        fprintf(fp, "#include \"%s\"\n\n", path->path);
    }
  }

  assert(trace_func != NULL && strlen(trace_func) > 0);
  if (flags & FLAG_STREAMID)
    fprintf(fp, "void %s(int stream_id, const unsigned char *data, unsigned size);\n", trace_func);
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
    fprintf(fp, "%s trace_timestamp(void);\n", type_to_string(stream->clock, typedesc, sizearray(typedesc)));
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

void generate_funcstubs(FILE *fp, unsigned flags, const char *trace_func, const char *headerfile)
{
  char xmit_call[40];
  const CTF_EVENT *evt;

  /* file header */
  assert(fp != NULL);
  assert(headerfile != NULL);
  fprintf(fp, "/*\n"
              " * Trace functions implementation file, generated by tracegen\n"
              " */\n"
              "#ifndef NTRACE\n"
              "#include <string.h>\n");

  if ((flags & FLAG_C99) == 0)
    fprintf(fp, "#include <alloca.h>\n");
  assert(headerfile != NULL && strlen(headerfile) > 0);
  fprintf(fp, "#include \"%s\"\n\n", headerfile);

  for (evt = event_next(NULL); evt != NULL; evt = event_next(evt)) {
    const CTF_PACKET_HEADER *pkthdr = packet_header();
    const CTF_STREAM *stream = stream_by_id(evt->stream_id);
    const CTF_EVENT_HEADER *evthdr = (stream != NULL) ? &stream->event : NULL;
    const CTF_EVENT_FIELD *field;
    int pos, seq, stringcount, headersz, fixedsz;
    const char *var_totallength, *var_index;

    generate_functionheader(fp, evt, flags);
    fprintf(fp, "\n{\n");

    if (flags & FLAG_STREAMID)
      sprintf(xmit_call, "%s(%d, ", trace_func, (stream != NULL) ? stream->stream_id : 0);
    else
      sprintf(xmit_call, "%s(", trace_func);

    /* go through the options and arguments to determine the fixed size of the
       trace message (the size excluding arguments that are variable length
       strings) */
    stringcount = 0;
    fixedsz = 0;
    assert(pkthdr != NULL);
    headersz = pkthdr->header.magic_size / 8;
    if (pkthdr->header.streamid_size > 0)
      headersz += pkthdr->header.streamid_size / 8;
    if (evthdr != NULL && evthdr->header.id_size > 0)
      headersz += evthdr->header.id_size / 8;
    if (evthdr != NULL && evthdr->header.timestamp_size > 0)
      fixedsz += evthdr->header.timestamp_size / 8;
    /* for strings, only add the zero terminator byte, the parameter lengths
       are added later (by generating a call to strlen) */
    for (field = evt->field_root.next; field != NULL; field = field->next) {
      if (field->type.typeclass == CLASS_STRING)
        stringcount += 1;  /* count the number of string parameters */
      else
        fixedsz += field->type.size / 8;
    }

    /* handle the constant part of the headers */
    pos = 0;
    if (headersz > 0) {
      fprintf(fp, "  static const unsigned char header[%d] = {", headersz);
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
      /* the clock type must be converted to a standard C type, because the
         TSDL type is not compatible with C */
      fprintf(fp, "  %s tstamp = trace_timestamp();\n", type_to_string(clock, typedesc, sizearray(typedesc)));
    }
    /* if there are string parameters, create variables for their lengths and
       their positions in the buffer */
    if (stringcount > 0) {
      int count = 0, idx;
      if (stringcount > 1)
        fprintf(fp, "  unsigned index = 0;\n");
      for (field = evt->field_root.next; field != NULL; field = field->next)
        if (field->type.typeclass == CLASS_STRING)
          fprintf(fp, "  unsigned length%d = strlen(%s);\n", count++, field->name);
      if (stringcount == 1) {
        var_totallength = "length0";
        var_index = "length0";
      } else {
        var_totallength = "totallength";
        var_index = "index";
        fprintf(fp, "  unsigned %s = ", var_totallength);
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
      fprintf(fp, "  %sheader, %d);\n", xmit_call, headersz);
    } else {
      /* create a variable for the buffer (the stringcount count is added to the
         fixed size because the zero byte of each string must be allocated too */
      if ((flags & FLAG_C99) == 0 || stringcount == 0)
        fprintf(fp, "  unsigned char buffer[%d", headersz + fixedsz + stringcount);
      else
        fprintf(fp, "  unsigned char *buffer = alloca(%d", headersz + fixedsz + stringcount);
      if (stringcount > 0)
        fprintf(fp, " + %s", var_totallength);
      if ((flags & FLAG_C99) == 0 || stringcount == 0)
        fprintf(fp, "];\n");
      else
        fprintf(fp, ");\n");
      /* copy the fixed header to the buffer */
      if (headersz > 0)
        fprintf(fp, "  memcpy(buffer, header, %d);\n", headersz);
      /* copy the timestamp */
      if (evthdr != NULL && evthdr->header.timestamp_size > 0) {
        fprintf(fp, "  memcpy(buffer + %d, &tstamp, %d);\n", headersz, evthdr->header.timestamp_size / 8);
        pos += evthdr->header.timestamp_size / 8;
      }
      /* the parameters */
      seq = 0;
      for (field = evt->field_root.next; field != NULL; field = field->next) {
        if (seq == 0)
          fprintf(fp, "  memcpy(buffer + %d, ", pos);
        else
          fprintf(fp, "  memcpy(buffer + %d + %s, ", pos, var_index);
        if (field->type.typeclass == CLASS_INTEGER || field->type.typeclass == CLASS_FLOAT
            || field->type.typeclass == CLASS_ENUM || field->type.typeclass == CLASS_STRUCT)
          fprintf(fp, "&");
        fprintf(fp, "%s, ", field->name);
        if (field->type.typeclass == CLASS_STRING) {
          fprintf(fp, "length%d + 1);\n", seq);
          if (stringcount == 1)
            pos += 1; /* for the zero byte */
          else
            fprintf(fp, "  index += length%d + 1;\n", seq);
          seq++;
        } else {
          fprintf(fp, "%u);\n", field->type.size / 8);
          pos += field->type.size / 8;
        }
      }

      fprintf(fp, "  %sbuffer, %d", xmit_call, headersz + fixedsz + stringcount);
      if (stringcount > 0)
        fprintf(fp, " + %s", var_totallength);
      fprintf(fp, ");\n");
    }
    fprintf(fp, "}\n\n");
  }

  /* file trailer */
  fprintf(fp, "#endif /* NTRACE */\n");
}

static void usage(void)
{
  printf("tragegen - generate C source & header files from TSDL specifications,"
         "           for tracing in the Common Trace Format.\n\n"
         "Usage: tracegen [options] inputfile\n\n"
         "Options:\n"
         "-c99      Generate C99-compatible code (default is C90).\n"
         "-fs=name  Set the name for the time stamp function, default = trace_timestamp\n"
         "-fx=name  Set the name for the trace transmit function, default = trace_xmit\n"
         "-i=path   Generate an #include <...> directive with this path.\n"
         "-I=path   Generate an #include \"...\" directive with this path.\n"
         "          The -i and -I options may appear multiple times.\n"
         "-no-instr Add a \"no_instrument_function\" attribute to the generated functions.\n"
         "-o=name   Base output filename; a .c and .h suffix is added to this name.\n"
         "-s        SWO tracing: use channels for stream ids.\n"
         "-t        Force basic C types on arguments, if available.\n");
}

static void unknown_option(const char *option)
{
  fprintf(stderr, "Unknown option \"%s\"; use option -h for help.\n", option);
  exit(1);
}

int main(int argc, char *argv[])
{
  PATHLIST includepaths = { NULL, NULL }, *path;
  char infile[_MAX_PATH], outfile[_MAX_PATH];
  char trace_func[64], timestamp_func[64];
  char *ptr;
  unsigned opt_flags;
  int idx;

  if (argc <= 1) {
    usage();
    return 1;
  }

  /* command line options */
  infile[0] = '\0';
  outfile[0] = '\0';
  strcpy(trace_func, "trace_xmit");
  opt_flags = 0;
  for (idx = 1; idx < argc; idx++) {
    if (IS_OPTION(argv[idx])) {
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage();
        return 0;
      case 'c':
        if (strcmp(argv[idx]+1, "c99") == 0)
          opt_flags |= FLAG_C99;
        else
          unknown_option(argv[idx]);
        break;
      case 'f':
        switch (argv[idx][2]) {
        case 's':
          ptr = &argv[idx][3];
          if (*ptr == '=' || *ptr == ':')
            ptr++;
          strlcpy(timestamp_func, ptr, sizearray(timestamp_func));
          break;
        case 'x':
          ptr = &argv[idx][3];
          if (*ptr == '=' || *ptr == ':')
            ptr++;
          strlcpy(trace_func, ptr, sizearray(trace_func));
          break;
        default:
          unknown_option(argv[idx]);
        }
        break;
      case 'I':
      case 'i':
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        path = malloc(sizeof(PATHLIST));
        if (path != NULL) {
          path->path = strdup(ptr);
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
        ptr = &argv[idx][2];
        if (*ptr == '=' || *ptr == ':')
          ptr++;
        strlcpy(outfile, ptr, sizearray(outfile));
        break;
      case 's':
        opt_flags |= FLAG_STREAMID;
        break;
      case 't':
        opt_flags |= FLAG_BASICTYPES;
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
    return 1;
  }
  if (strlen(outfile) == 0) {
    #if defined _WIN32
      ptr = infile;
      if (strrchr(ptr, '\\') != NULL)
        ptr = strrchr(ptr, '\\') + 1;
      if (strrchr(ptr, '/') != NULL)
        ptr = strrchr(ptr, '/') + 1;
    #else
      ptr = strrchr(infile, '/');
      ptr = (ptr != NULL) ? ptr + 1 : infile;
    #endif
    strlcat(outfile, "trace_", sizearray(outfile));
    strlcat(outfile, ptr, sizearray(outfile));
    if ((ptr = strrchr(outfile, '.')) != NULL)
      *ptr = '\0';
  }

  if (!ctf_parse_init(infile))
    return 1; /* error message already issued via ctf_error_notify() */
  if (ctf_parse_run()) {
    FILE *fp;
    int done_msg = 1;

    strlcat(outfile, ".h", sizearray(outfile));
    fp = fopen(outfile, "wt");
    if (fp != NULL) {
      generate_prototypes(fp, opt_flags, trace_func, &includepaths);  //??? pass in timestamp_func
      fclose(fp);
    } else {
      fprintf(stderr, "Error writing file %s.\n", outfile);
      done_msg = 0;
    }

    ptr = strrchr(outfile, '.');
    assert(ptr != NULL);
    *(ptr + 1) = 'c';
    fp = fopen(outfile, "wt");
    if (fp != NULL) {
      /* temporarily rename the extension back to .h */
      assert(ptr != NULL && *(ptr + 1) == 'c');
      *(ptr + 1) = 'h';
      generate_funcstubs(fp, opt_flags, trace_func, outfile); //??? pass in timestamp_func
      assert(ptr != NULL && *(ptr + 1) == 'h');
      *(ptr + 1) = 'c';
      fclose(fp);
    } else {
      fprintf(stderr, "Error writing file %s.\n", outfile);
      done_msg = 0;
    }

    if (done_msg)
      printf("Generated %s.\n", outfile);
  }

  ctf_parse_cleanup();
  while (includepaths.next != NULL) {
    path = includepaths.next;
    includepaths.next = path->next;
    free(path->path);
    free(path);
  }

  return 0;
}

