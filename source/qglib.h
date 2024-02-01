/*
 *  Support routines for QuickGuide.
 *
 *  Copyright (C) 2023-2024 CompuPhase
 *  All rights reserved.
 */
#ifndef __QGLIB_H
#define __QGLIB_H

#include <stdbool.h>
#include "quickguide.h"

#if defined __cplusplus
  extern "C" {
#endif

/* ***** file information ***** */

const QG_FILEHDR *qg_file_header(const void *guide);
unsigned qg_topic_count(const void *guide);
const QG_TOPICHDR* qg_topic_by_index(const void *guide,unsigned index);
const QG_TOPICHDR* qg_topic_by_id(const void *guide,uint32_t topic);
const char *qg_topic_caption(const void *guide,uint32_t topic);

/* ***** links ***** */

typedef struct QG_LINK {
  struct QG_LINK *next;
  int x1,y1,x2,y2;
  uint32_t topic;       /* topic ID */
} QG_LINK;

#define QG_INVALID_LINK    0xffff

void qg_link_clearall(QG_LINK *root);
bool qg_link_set(QG_LINK *root,int x1,int y1,int x2,int y2,uint32_t topic);
uint32_t qg_link_get(const QG_LINK *root,int x,int y);

/* ***** variables ***** */

typedef struct QG_VARIABLE {
  struct QG_VARIABLE *next;
  char *name;
  char *value;
} QG_VARIABLE;

bool qg_variables_collect(const void *guide,QG_VARIABLE *root);
void qg_variable_clearall(QG_VARIABLE *root);
bool qg_variable_set(QG_VARIABLE *root,const char *name,const char *value);
const char *qg_variable_find(const QG_VARIABLE *root,const char *name);
const char *qg_variable_get(const QG_VARIABLE *root,int index);

/* ***** pictures ***** */

#define QG_PIXFMT_RGB   3
#define QG_PIXFMT_RGBA  4

typedef struct QG_PICTURE {
  struct QG_PICTURE *next;
  unsigned id;
  const char *pixels;
  unsigned width,height;
  int align;
  unsigned format;
} QG_PICTURE;

void qg_picture_clearall(QG_PICTURE *root);
const void *qg_picture_get(const void *guide,QG_PICTURE *root,unsigned id,unsigned *width,unsigned *height,int *align,unsigned *format);

/* ***** search results ***** */

typedef struct QG_SEARCHRESULT {
  struct QG_SEARCHRESULT *next;
  uint32_t topic;     /* topic ID */
  unsigned line;      /* line number (related to source strings, before reformatting) */
  unsigned cpos;      /* character position in the string */
  unsigned clength;   /* length of the search result */
  int ypos;           /* y position after most recent reformatting */
} QG_SEARCHRESULT;

void qg_search_clearall(QG_SEARCHRESULT *root);
bool qg_search_append(QG_SEARCHRESULT *root,uint32_t topic,int linenr,int position,int length);
QG_SEARCHRESULT *qg_search_next(const QG_SEARCHRESULT *current,uint32_t topic);

const char *qg_strfind(const char *pattern,const char *text,int *length,bool ignorecase,bool no_accents);

/* ***** context ***** */

bool qg_passcontext(const QG_LINE_RECORD *content, unsigned long contextmask);

/* ***** topic history ***** */

typedef struct QG_HISTORY_PAGE {
  uint32_t topic;
  int scrollpos;  /* scroll position at the moment the page was pushed on the stack */
} QG_HISTORY_PAGE;

typedef struct QG_HISTORY {
  QG_HISTORY_PAGE *pages;
  unsigned size;        /* max. number of items the stack can hold */
  unsigned count;       /* current number of items in the stack */
  unsigned pos;         /* current position in the history */
} QG_HISTORY;

bool qg_history_init(QG_HISTORY *stack,unsigned maxitems);
void qg_history_clear(QG_HISTORY *stack);
bool qg_history_push(QG_HISTORY *stack,uint32_t topic);
bool qg_history_markpos(QG_HISTORY *stack,uint32_t topic,int scrollpos);
bool qg_history_goback(QG_HISTORY *stack,uint32_t *topic,int *scrollpos);
bool qg_history_goforward(QG_HISTORY *stack,uint32_t *topic,int *scrollpos);
bool qg_history_pick(QG_HISTORY *stack,unsigned index,bool remove,uint32_t *topic,int *scrollpos);
unsigned qg_history_count(QG_HISTORY *stack);

#if defined __cplusplus
  }
#endif

#endif /* __QGLIB_H */
