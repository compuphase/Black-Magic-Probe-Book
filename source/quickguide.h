/*
 *  QuickGuide file format structure and constants.
 *
 *  Copyright (C) 2023-2024 CompuPhase
 *  All rights reserved.
 */
#ifndef _QUICKGUIDE_H
#define _QUICKGUIDE_H

#include <stdint.h>

/* ************************************ */
/* File format structures and constants */
/* ************************************ */

enum {
  QPAR_STANDARD,
  QPAR_HEADING,
  QPAR_ULIST,
  QPAR_OLIST,
  QPAR_PREFMT,
  QPAR_TABLE,
  QPAR_HLINE,
};

enum {
  QFMT_SENTINEL,
  QFMT_STYLE,           /* 0=roman, 1=italic, 2=bold, 3=code/monospaced */
  QFMT_NOBREAK,         /* character at pos is non-breaking (may be " " or "-") */
  QFMT_SOFTBREAK,       /* soft-hyphen */
  QFMT_LINEBREAK,       /* forced line break */
  QFMT_COLBREAK,        /* column break (in table) */
  QFMT_LINK,
  QFMT_PICT,
  QFMT_VARIABLE,
};

enum {
  QALIGN_LEFT,
  QALIGN_RIGHT,
  QALIGN_CENTRE,
};
#define QALIGN_CENTER QALIGN_CENTRE

#define QFLG_VSPACE       (1u << 0)
#define QFLG_CONTEXT      (1u << 1)

#if defined __GNUC__
# define PACKED         __attribute__((packed))
#else
# define PACKED
#endif

#if defined __LINUX__ || defined __FreeBSD__ || defined __APPLE__
# pragma pack(1)        /* structures must be packed (byte-aligned) */
#elif defined MACOS && defined __MWERKS__
# pragma options align=mac68k
#else
# pragma pack(push)
# pragma pack(1)        /* structures must be packed (byte-aligned) */
# if defined __TURBOC__
#   pragma option -a-   /* "pack" pragma for older Borland compilers */
# endif
#endif


typedef struct tagQG_FILEHDR {
  char signature[3];
  uint16_t version;
  uint32_t topic_offs;    /* offset to the topic table */
  uint16_t topic_count;
  uint32_t var_offs;
  uint16_t var_count;
  uint32_t pict_offs;
  uint16_t pict_count;
} PACKED QG_FILEHDR;

typedef struct tagQG_TOPICHDR {
  uint16_t size;
  uint32_t id;
  uint32_t content_offs;  /* offset to the first string */
  uint16_t content_count;
  /* zero-terminated string for topic caption follows */
} PACKED QG_TOPICHDR;

typedef struct tagQG_VARIABLE_RECORD {
  uint16_t size;
  /* zero-terminated strings for the variable name and the default value follow */
} PACKED QG_VARIABLE_RECORD;

typedef struct tagQG_PICTURE_RECORD {
  uint32_t size;
  uint8_t align;
  /* picture data follows */
} PACKED QG_PICTURE_RECORD;

typedef struct tagQG_FORMATCODE {
  uint16_t type;
  uint16_t param;
  uint32_t pos;
} PACKED QG_FORMATCODE;

typedef struct tagQG_LINE_RECORD {
  uint16_t size;
  uint8_t type;
  uint8_t indent;
  uint8_t param;
  uint8_t flags;
  uint16_t fmtcodes;
  /* a list with format codes follows (fmtcodes is always > 0)
     the plain text of the paragraph follows
     a list with context patterns may optionally follow (if QFLG_CONTEXT is set) */
} PACKED QG_LINE_RECORD;

#if defined __LINUX__ || defined __FreeBSD__ || defined __APPLE__
# pragma pack()         /* reset default packing */
#elif defined MACOS && defined __MWERKS__
# pragma options align=reset
#else
# pragma pack(pop)      /* reset previous packing */
#endif

/* ********************************************** */
/* Viewer API structures, constants and functions */
/* ********************************************** */

enum {
  QBKG_STANDARD,
  QBKG_NOTE,
  QBKG_TIP,
  QBKG_IMPORTANT,
  QBKG_CAUTION,
  QBKG_WARNING,
  QBKG_TABLEHEADER,
  QBKG_TABLEROW1,
  QBKG_TABLEROW2,
};

#endif /* _QUICKGUIDE_H */

