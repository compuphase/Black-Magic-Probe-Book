/*
 * Functions to parse a TSDL file and store it in memory structures. This
 * parser is the base for the tracegen code generation utility and the CTF
 * binary stream parser.
 *
 * Copyright 2019-2022 CompuPhase
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
#ifndef _PARSETSDL_H
#define _PARSETSDL_H

enum {
  CLASS_UNKNOWN,
  CLASS_INTEGER,
  CLASS_FLOAT,
  CLASS_STRING,
  CLASS_STRUCT,
  CLASS_VARIANT,
  CLASS_ENUM,
};
#define TYPEFLAG_SIGNED 0x01
#define TYPEFLAG_UTF8   0x02
#define TYPEFLAG_STRONG 0x04    /* strong type, from typedef or typealias */
#define TYPEFLAG_WEAK   0x08    /* weak type, predefined, but may be overruled */

enum {
  CTFERR_NONE,
  CTFERR_FILEOPEN,      /* file open error (file ... not found?) */
  CTFERR_MEMORY,        /* memory allocation error */
  CTFERR_LONGLINE,      /* line too long */
  CTFERR_BLOCKCOMMENT,  /* comment not closed */
  CTFERR_STRING,        /* literal string is not terminated */
  CTFERR_INVALIDTOKEN,  /* invalid token */
  CTFERR_NUMBER,        /* invalid number */
  CTFERR_SYNTAX_MAIN,   /* syntax error at main level */
  CTFERR_NEEDTOKEN,     /* expected token ... */
  CTFERR_INVALIDFIELD,  /* unknown field name ... */
  CTFERR_UNKNOWNTYPE,   /* type ... not found */
  CTFERR_WRONGTYPE,     /* incorrect type for the field or type */
  CTFERR_TYPE_SIZE,     /* type declaration lacks a size */
  CTFERR_DUPLICATE_ID,
  CTFERR_UNKNOWNSTREAM, /* stream with name ... is not defined */
  CTFERR_UNKNOWNCLOCK,  /* clock with name ... is not defined */
  CTFERR_STREAM_NO_DEF, /* no definition for stream id ... (required for event header) */
  CTFERR_STREAM_NOTSET, /* event ... is not assigned to a stream */
  CTFERR_TYPE_REDEFINE, /* type ... was already defined */
  CTFERR_NAMEREQUIRED,  /* a name for ... is required */
  CTFERR_DUPLICATE_NAME,/* duplicate name ... */
  CTFERR_CLOCK_IS_INT,  /* clock must be mapped to integer type */
  CTFERR_DUPLICATE_SETTING,
  CTFERR_EXCEED_INCLUDES,/* #include nesting too deep */
};

enum {
  BYTEORDER_LE = 0,
  BYTEORDER_BE,
};

#define CTF_NAME_LENGTH   64
#define CTF_UUID_LENGTH   16
#define CTF_BASE_ADDR     255

typedef struct tagCTF_KEYVALUE {
  struct tagCTF_KEYVALUE *next;
  char name[CTF_NAME_LENGTH];
  long value;
} CTF_KEYVALUE;

typedef struct tagCTF_TYPE {
  struct tagCTF_TYPE *next;
  char name[CTF_NAME_LENGTH]; /* name of the type */
  uint32_t size;        /* in bits (for integer, fixed-point, floating-point & struct) */
  uint8_t typeclass;    /* integer, floating-point, string, struct, variant, enum */
  uint8_t align;        /* (in bits) */
  uint8_t flags;        /* signed y/n (for integer types); encoding (ascii/utf8, for strings) */
  uint8_t base;         /* preferred base, for pretty printing */
  int scale;            /* scale factor, for fixed-point (scaled integer) */
  int length;           /* for arrays */
  char *identifier;     /* name, for field of structure */
  char *selector;       /* identifier name (selector for variant, map for clock) */
  struct tagCTF_TYPE *fields;       /* (for struct & variant) */
  struct tagCTF_KEYVALUE *keys;     /* namevalue_root (for enum) */
} CTF_TYPE;

typedef struct tagCTF_TRACE_GLOBAL {
  uint8_t major;
  uint8_t minor;
  uint8_t byte_order;
  uint8_t uuid[CTF_UUID_LENGTH];
  uint32_t stream_mask; /* bit mask of which streams are active */
} CTF_TRACE_GLOBAL;

typedef struct tagCTF_PACKET_HEADER {
  struct {
    uint8_t magic_size; /* 32-bit: 0xC1FC1FC1, 16-bit: 0x1FC1, 8-bit: 0xC1 */
    uint8_t uuid_size;
    uint8_t streamid_size;
  } header;
} CTF_PACKET_HEADER;

typedef struct tagCTF_EVENT_HEADER {
  struct {
    uint8_t id_size;
    uint8_t timestamp_size;
  } header;
} CTF_EVENT_HEADER;

typedef struct tagCTF_CLOCK {
  struct tagCTF_CLOCK *next;
  char name[CTF_NAME_LENGTH];
  char description[CTF_NAME_LENGTH];
  uint8_t uuid[CTF_UUID_LENGTH];
  uint32_t frequeny;
  uint32_t precision;
  uint32_t offset_s, offset;
  int absolute;
} CTF_CLOCK;

typedef struct tagCTF_STREAM {
  struct tagCTF_STREAM *next;
  int stream_id;
  char name[CTF_NAME_LENGTH];
  CTF_EVENT_HEADER event;
  CTF_TYPE *clock;
} CTF_STREAM;

typedef struct tagCTF_EVENT_FIELD {
  struct tagCTF_EVENT_FIELD *next;
  char name[CTF_NAME_LENGTH];
  CTF_TYPE type;
} CTF_EVENT_FIELD;

typedef struct tagCTF_EVENT {
  struct tagCTF_EVENT *next;
  int id;
  int stream_id;
  char name[CTF_NAME_LENGTH];
  char *attribute;
  CTF_EVENT_FIELD field_root;
} CTF_EVENT;


int ctf_error_notify(int code, int linenr, const char *message); /* must be implemented in the calling application */

const CTF_PACKET_HEADER *packet_header(void);

const CTF_CLOCK *clock_by_name(const char *name);
const CTF_CLOCK *clock_by_seqnr(int seqnr);

int stream_isactive(int stream_id);
int stream_count(void);
const CTF_STREAM *stream_by_name(const char *name);
const CTF_STREAM *stream_by_id(int stream_id);
const CTF_STREAM *stream_by_seqnr(int seqnr);

int event_count(int stream_id);
const CTF_EVENT *event_next(const CTF_EVENT *event);
const CTF_EVENT *event_by_id(int event_id);

int ctf_parse_init(const char *filename);
void ctf_parse_cleanup(void);
int ctf_parse_run(void);

#endif /* _PARSETSDL_H */

