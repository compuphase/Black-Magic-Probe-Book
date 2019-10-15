/*
 * Trace function generation utility: it creates a .c and a .h file with
 * functions that create packets conforming to the Common Trace Format (CTF).
 * The * input is a file in the Trace Stream Description Language (TDSL), the
 * primary specification language for CTF.
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

#include "parsetsdl.h"

#if !defined sizearray
  #define sizearray(a)  (sizeof(a) / sizeof((a)[0]))
#endif

#if defined _WIN32
  #define DIR_SEPARATOR "\\"
#else
  #define DIR_SEPARATOR "/"
#endif

#define FLAG_MACRO      0x0001
#define FLAG_INDENT     0x0002
#define FLAG_BASICTYPES 0x0004
#define FLAG_STREAMID   0x0008


int ctf_error_notify(int code, int linenr, const char *message)
{
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
  }

  fprintf(fp, ")");
  return 1;
}

void generate_prototypes(FILE *fp, unsigned flags)
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

  if (flags & FLAG_STREAMID)
    fprintf(fp, "void trace_xmit(int stream_id, const unsigned char *data, unsigned size);\n");
  else
    fprintf(fp, "void trace_xmit(const unsigned char *data, unsigned size);\n");
  /* assume all all streams to have compatible clocks, so get only the first clock */
  for (seqnr = 0; (stream = stream_by_seqnr(seqnr)) != NULL && stream->clock == NULL; seqnr++)
    /* nothing */;
  if (stream != NULL) {
    char typedesc[64];
    /* the clock type must be converted to a standard C type, because the
       TSDL type is not compatible with C */
    assert(stream->clock != NULL);
    fprintf(fp, "%s trace_timestamp(void);\n", type_to_string(stream->clock, typedesc, sizearray(typedesc)));
  }
  fprintf(fp, "\n");

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

void generate_funcstubs(FILE *fp, unsigned flags, const char *headerfile)
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
              "#include \"%s\"\n\n", headerfile);

  for (evt = event_next(NULL); evt != NULL; evt = event_next(evt)) {
    const CTF_PACKET_HEADER *pkthdr = packet_header();
    const CTF_STREAM *stream = stream_by_id(evt->stream_id);
    const CTF_EVENT_HEADER *evthdr = (stream != NULL) ? &stream->event : NULL;
    const CTF_EVENT_FIELD *field;
    int hdrsize;

    generate_functionheader(fp, evt, flags);
    fprintf(fp, "\n{\n");

    if (flags & FLAG_STREAMID)
      sprintf(xmit_call, "trace_xmit(%d, ", (stream != NULL) ? stream->stream_id : 0);
    else
      strcpy(xmit_call, "trace_xmit(");

    /* handle the constant part of the headers */
    fprintf(fp, "  static const unsigned char header[] = {");
    /* check for a packet header (for stream-based protocols, there should be one) */
    assert(pkthdr != NULL);
    hdrsize = pkthdr->header.magic_size / 8;
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
    if (pkthdr->header.streamid_size > 0) {
      unsigned long val;
      if (hdrsize > 0)
        fprintf(fp, ", ");
      val = (stream != NULL) ? stream->stream_id : 0;
      dumphex(fp, (unsigned char*)&val, pkthdr->header.streamid_size / 8);
      hdrsize += pkthdr->header.streamid_size / 8;
    }
    /* check for an event header (there really should be one)
       note that only the id is handled here (because it is constant for the
       function); the timestamp is dynamic and cannot be stored in the array */
    if (evthdr != NULL && evthdr->header.id_size > 0) {
      if (hdrsize > 0)
        fprintf(fp, ", ");
      dumphex(fp, (unsigned char*)&evt->id, evthdr->header.id_size / 8);
      hdrsize += evthdr->header.id_size / 8;
    }
    fprintf(fp, " };\n");
    /* check whether the timestamp must be stored (and its type) */
    if (evthdr != NULL && evthdr->header.timestamp_size > 0) {
      char typedesc[64];
      const CTF_TYPE *clock = stream->clock;
      assert(clock != NULL);
      /* the clock type must be converted to a standard C type, because the
         TSDL type is not compatible with C */
      fprintf(fp, "  %s tstamp = trace_timestamp();\n", type_to_string(clock, typedesc, sizearray(typedesc)));
    }
    fprintf(fp, "  %sheader, %d);\n", xmit_call, hdrsize);
    if (evthdr != NULL && evthdr->header.timestamp_size > 0)
      fprintf(fp, "  %s&tstamp, %d);\n", xmit_call, evthdr->header.timestamp_size / 8);

    /* the parameters */
    for (field = evt->field_root.next; field != NULL; field = field->next) {
      fprintf(fp, "  %s", xmit_call);
      fprintf(fp, "(const unsigned char*)");
      if (field->type.typeclass == CLASS_INTEGER || field->type.typeclass == CLASS_FLOAT || field->type.typeclass == CLASS_ENUM)
        fprintf(fp, "&");
      fprintf(fp, "%s, ", field->name);
      if (field->type.typeclass == CLASS_STRING)
        fprintf(fp, "strlen(%s) + 1);\n", field->name);
      else
        fprintf(fp, "%u);\n", field->type.size / 8);
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
         "-o=name\t Base output filename; a .c and .h suffix is added to this name.\n"
         "-s\t Pass stream ID as separate parameter (SWO tracing).\n"
         "-t\t Force basic C types on arguments, if availalble.\n");
}

int main(int argc, char *argv[])
{
  char infile[256], outfile[256], *ptr;
  unsigned opt_flags;
  int idx;

  if (argc <= 1) {
    usage();
    return 1;
  }

  /* command line options */
  infile[0] = '\0';
  outfile[0] = '\0';
  opt_flags = 0;
  for (idx = 0; idx < argc; idx++) {
    if (argv[idx][0] == '-' || argv[idx][0] == '/') {
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage();
        return 0;
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
        fprintf(stderr, "Unknown option %s; use option -h for help.\n", argv[idx]);
        return 1;
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
      generate_prototypes(fp, opt_flags);
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
      generate_funcstubs(fp, opt_flags, outfile);
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
  return 0;
}

