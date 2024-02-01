/*
 *  Support routines for QuickGuide.
 *
 *  Copyright (C) 2023-2024 CompuPhase
 *  All rights reserved.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if (defined WIN32 || defined _WIN32 || defined __WIN32__) && 0 //???
# include <assert/assert.h>
#else
# include <assert.h>
# define ASSERT(e)  assert(e)
#endif
#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#include "qglib.h"
#include "qoi.h"
#include "quickguide.h"

#if defined _MSC_VER && !defined _strdup
# define strdup(s)  _strdup(s)
#endif

#if !defined sizearray
# define sizearray(a)   (sizeof(a) / sizeof((a)[0]))
#endif


/* ***** file information ***** */

const QG_FILEHDR *qg_file_header(const void *guide)
{
  if (!guide)
    return NULL;
  const QG_FILEHDR *filehdr = (const QG_FILEHDR*)guide;
  if (memcmp(filehdr->signature, "QG\x1b", 3) != 0)
    return NULL;
  return filehdr;
}

unsigned qg_topic_count(const void *guide)
{
  const QG_FILEHDR *filehdr = qg_file_header(guide);
  if (!filehdr)
    return 0;
  return filehdr->topic_count;
}

const QG_TOPICHDR *qg_topic_by_index(const void *guide,unsigned index)
{
  const QG_FILEHDR *filehdr = qg_file_header(guide);
  if (!filehdr)
    return NULL;
  if (index>=filehdr->topic_count)
    return NULL;

  const QG_TOPICHDR *topichdr = (const QG_TOPICHDR*)((const uint8_t*)guide + filehdr->topic_offs);
  while (index>0) {
    topichdr = (const QG_TOPICHDR*)((const uint8_t*)topichdr + topichdr->size);
    index--;
  }
  return topichdr;
}

const QG_TOPICHDR *qg_topic_by_id(const void *guide,uint32_t topic)
{
  const QG_FILEHDR *filehdr = qg_file_header(guide);
  if (!filehdr)
    return NULL;

  const QG_TOPICHDR *topichdr = (const QG_TOPICHDR*)((const uint8_t*)guide + filehdr->topic_offs);
  for (int index = 0; index < filehdr->topic_count; index++) {
    if (topichdr->id==topic)
      return topichdr;
    topichdr=(const QG_TOPICHDR *)((const char *)topichdr+topichdr->size);
  }
  return NULL;
}

const char *qg_topic_caption(const void *guide,uint32_t topic)
{
  const QG_TOPICHDR *topichdr=qg_topic_by_id(guide,topic);
  if (!topichdr || topichdr->content_count==0)
    return NULL;
  const QG_LINE_RECORD *content=(const QG_LINE_RECORD*)((unsigned char*)guide+topichdr->content_offs);
  ASSERT(content->type==QPAR_HEADING);
  return (const char*)content+sizeof(QG_LINE_RECORD)+content->fmtcodes*sizeof(QG_FORMATCODE);
}

/* ***** links ***** */

void qg_link_clearall(QG_LINK *root)
{
  ASSERT(root);
  while (root->next) {
    QG_LINK *item=root->next;
    root->next=item->next;
    free(item);
  }
}

static bool qg_link_exists(const QG_LINK *root,int x1,int y1,int x2,int y2,uint32_t topic)
{
  ASSERT(root);
  for (QG_LINK *item=root->next; item; item=item->next)
    if (item->x1==x1 && item->x2==x2 && item->y1==y1 && item->y2==y2 && item->topic==topic)
      return true;
  return false;
}

bool qg_link_set(QG_LINK *root,int x1,int y1,int x2,int y2,uint32_t topic)
{
  ASSERT(topic!=QG_INVALID_LINK);
  if (qg_link_exists(root,x1,y1,x2,y2,topic))
    return false;

  QG_LINK *item=malloc(sizeof(QG_LINK));
  if (!item)
    return false;

  memset(item,0,sizeof(QG_LINK));
  item->x1=x1;
  item->y1=y1;
  item->x2=x2;
  item->y2=y2;
  item->topic=topic;

  ASSERT(root);
  QG_LINK *tail=root;
  while (tail->next)
    tail=tail->next;
  tail->next=item;
  return true;
}

uint32_t qg_link_get(const QG_LINK *root,int x,int y)
{
  ASSERT(root);
  for (QG_LINK *item=root->next; item; item=item->next)
    if (item->x1<=x && x<=item->x2 && item->y1<=y && y<=item->y2)
      return item->topic;
  return QG_INVALID_LINK;
}


/* ***** variables ***** */

bool qg_variables_collect(const void *guide,QG_VARIABLE *root)
{
  const QG_FILEHDR *filehdr=qg_file_header(guide);
  if (!filehdr)
    return false;
  if (filehdr->var_count==0)
    return true;

  const QG_VARIABLE_RECORD *varhdr=(const QG_VARIABLE_RECORD *)((const uint8_t *)guide+filehdr->var_offs);
  for (unsigned idx=0; idx<filehdr->var_count; idx++) {
    const char *name=(const char *)varhdr+sizeof(QG_VARIABLE_RECORD);
    const char *value=name+strlen(name)+1;
    qg_variable_set(root,name,value);
    varhdr=(const QG_VARIABLE_RECORD *)((const uint8_t *)varhdr+varhdr->size);
  }
  return true;
}

void qg_variable_clearall(QG_VARIABLE *root)
{
  ASSERT(root);
  while (root->next) {
    QG_VARIABLE *item=root->next;
    root->next=item->next;
    ASSERT(item->name && item->value);
    free(item->name);
    free(item->value);
    free(item);
  }
}

/** qg_variable_set() sets the value of an existing variable, or adds a new
 *  variable to the list.
 *
 *  \param root         The variable list.
 *  \param name         The name of the variable.
 *  \param value        The value of the variable. This may be an empty string,
 *                      but it should not be NULL.
 *
 *  \return `true` or `false` (memory allocation failure).
 *
 *  \note Although you can in principle add any variable to the list, it only
 *        makes sense to add variables that are referenced in the guide file.
 */
bool qg_variable_set(QG_VARIABLE *root,const char *name,const char *value)
{
  ASSERT(root);
  ASSERT(name);
  ASSERT(value);

  for (QG_VARIABLE *item=root->next; item; item=item->next) {
    ASSERT(item->name);
    if (strcmp(item->name,name)==0) {
      ASSERT(item->value);
      char *ptr=strdup(value);
      if (!ptr)
        return false; /* memory allocation failed, variable not adjusted */
      free(item->value);
      item->value=ptr;
      return true;
    }
  }

  /* variable not found, add it to the list */
  QG_VARIABLE *item=malloc(sizeof(QG_VARIABLE));
  if (!item)
    return false;

  memset(item,0,sizeof(QG_VARIABLE));
  item->name=strdup(name);
  item->value=strdup(value);
  if (!item->name || !item->value) {
    if (item->name)
      free(item->name);
    if (item->value)
      free(item->value);
    free(item);
    return false;
  }

  ASSERT(root);
  QG_VARIABLE *tail=root;
  while (tail->next)
    tail=tail->next;
  tail->next=item;
  return true;
}

/** qg_variable_find() returns the value of a variable that is referenced by
 *  name, or NULL if the variable is not in the list.
 */
const char *qg_variable_find(const QG_VARIABLE *root,const char *name)
{
  ASSERT(root);
  for (QG_VARIABLE *item=root->next; item; item=item->next) {
    ASSERT(item->name);
    if (strcmp(item->name,name)==0)
      return item->value;
  }
  return NULL;
}

/** qg_variable_get() returns the value of a variable that is referenced by
 *  index, or NULL if the index is invalid.
 */
const char *qg_variable_get(const QG_VARIABLE *root,int index)
{
  ASSERT(root);
  ASSERT(index>=0);
  QG_VARIABLE *item;
  for (item=root->next; item && index>0; item=item->next)
    index--;
  ASSERT(!item || item->value);
  return item ? item->value : NULL;
}


/* ***** pictures ***** */

#define QOI_OP_INDEX    0x00    /* 00xxxxxx */
#define QOI_OP_DIFF     0x40    /* 01xxxxxx */
#define QOI_OP_LUMA     0x80    /* 10xxxxxx */
#define QOI_OP_RUN      0xc0    /* 11xxxxxx */
#define QOI_OP_RGB      0xfe    /* 11111110 */
#define QOI_OP_RGBA     0xff    /* 11111111 */

#define QOI_MASK_2      0xc0    /* 11000000 */

#define QOI_COLOR_HASH(C) (C.rgba.r*3 + C.rgba.g*5 + C.rgba.b*7 + C.rgba.a*11)
#define QOI_MAGIC \
    (((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
     ((unsigned int)'i') <<  8 | ((unsigned int)'f'))
#define QOI_HEADER_SIZE 14

#define QOI_PIXELS_MAX  ((uint32_t)400000000)

typedef union {
  struct { unsigned char r, g, b, a; } rgba;
  unsigned int v;
} qoi_rgba_t;

static const unsigned char qoi_padding[8] = {0,0,0,0,0,0,0,1};

static uint32_t qoi_read_32(const unsigned char *bytes, int *p)
{
  unsigned int a = bytes[(*p)++];
  unsigned int b = bytes[(*p)++];
  unsigned int c = bytes[(*p)++];
  unsigned int d = bytes[(*p)++];
  return a << 24 | b << 16 | c << 8 | d;
}

/** qoi_decode() a QOI image to a plain bitmap.
 *
 *  \param data         The QOI image data.
 *  \param size         The size of the image data.
 *  \param desc         [out] The description of the image (dimensions and other
 *                      attributes).
 *  \param channels     If 0, use the channel count in the QOI image header, and
 *                      decode to either RGB or RGBA. If 3, always decode as an
 *                      RGB image (ignore alpha channel, if one is present). If
 *                      4, always decode as RGBA (even if alpha not present, in
 *                      which case alpha is set to 255, i.e. opaque).
 *
 *  \return A pointer to the decoded bitmap. This bitmap must be freed with
 *          qoi_free().
 */
void *qoi_decode(const void *data, size_t size, QOI_DESC *desc, int channels)
{
  const unsigned char *bytes;
  unsigned int header_magic;
  unsigned char *pixels;
  qoi_rgba_t index[64];
  qoi_rgba_t px;
  int px_len, chunks_len, px_pos;
  int p=0, run=0;

  if (data==NULL || desc==NULL
      || (channels!=0 && channels!=3 && channels!=4)
      || size<QOI_HEADER_SIZE+(int)sizeof(qoi_padding))
    return NULL;

  bytes=(const unsigned char *)data;

  header_magic=qoi_read_32(bytes,&p);
  desc->width=qoi_read_32(bytes,&p);
  desc->height=qoi_read_32(bytes,&p);
  desc->channels=bytes[p++];
  desc->colorspace=bytes[p++];

  if (desc->width==0 || desc->height==0
      || desc->channels<3 || desc->channels>4
      || desc->colorspace>1
      || header_magic!=QOI_MAGIC
      || desc->height>=QOI_PIXELS_MAX/desc->width)
    return NULL;

  if (channels==0)
    channels=desc->channels;

  px_len=desc->width*desc->height*channels;
  pixels=(unsigned char *)malloc(px_len);
  if (!pixels)
    return NULL;

  memset(index,0,sizeof(index));
  px.rgba.r=0;
  px.rgba.g=0;
  px.rgba.b=0;
  px.rgba.a=255;

  chunks_len=size-(int)sizeof(qoi_padding);
  for (px_pos=0; px_pos<px_len; px_pos+=channels) {
    if (run>0) {
      run--;
    } else if (p<chunks_len) {
      int b1=bytes[p++];
      if (b1==QOI_OP_RGB) {
        px.rgba.r=bytes[p++];
        px.rgba.g=bytes[p++];
        px.rgba.b=bytes[p++];
      } else if (b1==QOI_OP_RGBA) {
        px.rgba.r=bytes[p++];
        px.rgba.g=bytes[p++];
        px.rgba.b=bytes[p++];
        px.rgba.a=bytes[p++];
      } else if ((b1 & QOI_MASK_2)==QOI_OP_INDEX) {
        px=index[b1];
      } else if ((b1 & QOI_MASK_2)==QOI_OP_DIFF) {
        px.rgba.r+=((b1>>4) & 0x03)-2;
        px.rgba.g+=((b1>>2) & 0x03)-2;
        px.rgba.b+=(b1      & 0x03)-2;
      } else if ((b1 & QOI_MASK_2)==QOI_OP_LUMA) {
        int b2=bytes[p++];
        int vg=(b1 & 0x3f)-32;
        px.rgba.r+=vg-8+((b2>>4) & 0x0f);
        px.rgba.g+=vg;
        px.rgba.b+=vg-8+(b2      & 0x0f);
      } else if ((b1 & QOI_MASK_2)==QOI_OP_RUN) {
        run=(b1 & 0x3f);
      }
      index[QOI_COLOR_HASH(px) % 64]=px;
    }

    pixels[px_pos+0]=px.rgba.r;
    pixels[px_pos+1]=px.rgba.g;
    pixels[px_pos+2]=px.rgba.b;
    if (channels==4)
      pixels[px_pos+3]=px.rgba.a;
  }

  return pixels;
}

/** qg_picture_clearall() removes all pictures currently in the list, and
 *  frees memory allocated for the pictures.
 *
 *  \param root   The root of the picture list.
 */
void qg_picture_clearall(QG_PICTURE *root)
{
  ASSERT(root);
  while (root->next) {
    QG_PICTURE *item=root->next;
    root->next=item->next;
    ASSERT(item->pixels);
    free((void*)item->pixels);
    free(item);
  }
}

/** qg_picture_get() returns the pixel data for a given picture ID. If the
 *  picture is not yet in the list, it is read and decoded.
 *
 *  \param guide  The guide data.
 *  \param root   The root of the picture list.
 *  \param id     The unique ID of the picture (the index in the picture table
 *                in the guide data).
 *  \param width  [out] Will contain the picture width upon return. This
 *                paramater may be NULL.
 *  \param height [out] Will contain the picture height upon return. This
 *                paramater may be NULL.
 *  \param align  [out] Will contain the preferred alignment for the picture
 *                upon return. This paramater may be NULL.
 *  \param format [out] The format of the pixel data; either QG_PIXFMT_RGB or
 *                QG_PIXFMT_RGBA. This parameter may be NULL.
 *
 *  \return A pointer to the pixel data, or NULL on failure (invalid picture ID,
 *          or memory allocation failure).
 */
const void *qg_picture_get(const void *guide,QG_PICTURE *root,unsigned id,unsigned *width,unsigned *height,int *align,unsigned *format)
{
  /* see whether the picture is already in the list */
  for (QG_PICTURE *item=root->next; item; item=item->next) {
    if (item->id==id) {
      if (width)
        *width=item->width;
      if (height)
        *height=item->height;
      if (align)
        *align=item->align;
      if (format)
        *format=item->format;
      return item->pixels;
    }
  }

  /* find the picture in the guide data */
  const QG_FILEHDR *filehdr=qg_file_header(guide);
  if (!filehdr)
    return false;
  if (id>=filehdr->pict_count)
    return false;
  const QG_PICTURE_RECORD *picthdr=(const QG_PICTURE_RECORD*)((const char *)guide+filehdr->pict_offs);
  for (unsigned idx=0; idx<id; idx++)
    picthdr=(const QG_PICTURE_RECORD*)((const char*)picthdr+picthdr->size);
  /* decode the pixel data */
  const unsigned char *qoi=(const unsigned char*)picthdr+sizeof(QG_PICTURE_RECORD);
  size_t qoisize=picthdr->size-sizeof(QG_PICTURE_RECORD);
  QOI_DESC desc;
  void *pixels=qoi_decode(qoi,qoisize,&desc,0);
  if (!pixels)
    return NULL;

  /* add the loaded/decoded picture to the list */
  QG_PICTURE *item=malloc(sizeof(QG_PICTURE));
  if (!item) {
    free(pixels);
    return NULL;
  }
  memset(item,0,sizeof(QG_PICTURE));
  item->id=id;
  item->pixels=pixels;
  item->width=desc.width;
  item->height=desc.height;
  item->align=picthdr->align;
  item->format=(desc.channels==4) ? QG_PIXFMT_RGBA : QG_PIXFMT_RGB;

  ASSERT(root);
  QG_PICTURE *tail=root;
  while (tail->next)
    tail=tail->next;
  tail->next=item;

  if (width)
    *width=item->width;
  if (height)
    *height=item->height;
  if (align)
    *align=item->align;
  if (format)
    *format=item->format;
  return item->pixels;
}

/** qg_picture_next() returns the information on the next picture in the list.
 *
 *  \param current  A pointer to the current picture.
 *
 *  \return The next picture in the list, or NULL if there is no next picture.
 *          If NULL is passed in for `current`, the function also returns NULL.
 *
 *  \note To get the data for the first picture in the list, pass in the picture
 *        root.
 */
const QG_PICTURE *qg_picture_next(const QG_PICTURE *current)
{
  if (current)
    current=current->next;
  return current;
}


/* ***** search results ***** */

void qg_search_clearall(QG_SEARCHRESULT *root)
{
  ASSERT(root);
  while (root->next) {
    QG_SEARCHRESULT *item=root->next;
    root->next=item->next;
    free(item);
  }
  root->topic=-1;
}

bool qg_search_append(QG_SEARCHRESULT *root,uint32_t topic,int linenr,int position,int length)
{
  QG_SEARCHRESULT *item=malloc(sizeof(QG_SEARCHRESULT));
  if (!item)
    return false;

  memset(item,0,sizeof(QG_SEARCHRESULT));
  item->topic=topic;
  item->line=linenr;
  item->cpos=position;
  item->clength=length;
  item->ypos=0;

  ASSERT(root);
  QG_SEARCHRESULT *tail=root;
  while (tail->next)
    tail=tail->next;
  tail->next=item;
  return true;
}

QG_SEARCHRESULT *qg_search_next(const QG_SEARCHRESULT *current,uint32_t topic)
{
  ASSERT(current);
  if (topic==UINT32_MAX) {
    /* return the next item, regardless of which topic it is in */
    current=current->next;
  } else if (current->topic!=topic) {
    /* find first result further down the list matching the topic number */
    while (current && current->topic!=topic)
      current=current->next;
  } else {
    /* find next result, which still must match the topic number */
    current=current->next;
    if (current && current->topic!=topic)
      current=NULL; /* make sure to stay within the same topic */
  }
  return (QG_SEARCHRESULT*)current;
}


/* the file must be opened in UTF-8 mode to properly display the characters */
const char *chardef[][4] = {
  { "A",        "a",        "A", "a" },
  { "\xc3\x80", "\xc3\xa0", "A", "a" }, /* À, à */
  { "\xc3\x81", "\xc3\xa1", "A", "a" }, /* Á, á */
  { "\xc3\x82", "\xc3\xa2", "A", "a" }, /* Â, â */
  { "\xc3\x83", "\xc3\xa3", "A", "a" }, /* Ã, ã */
  { "\xc3\x84", "\xc3\xa4", "A", "a" }, /* Ä, ä */
  { "\xc3\x85", "\xc3\xa5", "A", "a" }, /* Å, å */
  { "B",        "b",        "B", "b" },
  { "C",        "c",        "C", "c" },
  { "\xc3\x87", "\xc3\xa7", "C", "c" }, /* Ç, ç */
  { "D",        "d",        "D", "d" },
  { "\xc3\x90", "\xc3\xb0", "D", "d" }, /* Ð, ð */
  { "\xc4\x90", "\xc4\x91", "D", "d" }, /* ?, ? (slashed D) */
  { "E",        "e",        "E", "e" },
  { "\xc3\x88", "\xc3\xa8", "E", "e" }, /* È, è */
  { "\xc3\x89", "\xc3\xa9", "E", "e" }, /* É, é */
  { "\xc3\x8a", "\xc3\xaa", "E", "e" }, /* Ê, ê */
  { "\xc3\x8b", "\xc3\xab", "E", "e" }, /* Ë, ë */
  { "F",        "f",        "F", "f" },
  { "G",        "g",        "G", "g" },
  { "H",        "h",        "H", "h" },
  { "I",        "i",        "I", "i" },
  { "\xc3\x8c", "\xc3\xac", "I", "i" }, /* Ì, ì */
  { "\xc3\x8d", "\xc3\xad", "I", "i" }, /* Í, í */
  { "\xc3\x8e", "\xc3\xae", "I", "i" }, /* Î, î */
  { "\xc3\x8f", "\xc3\xaf", "I", "i" }, /* Ï, ï */
  { "J",        "j",        "J", "j" },
  { "K",        "k",        "K", "k" },
  { "L",        "l",        "L", "l" },
  { "M",        "m",        "M", "m" },
  { "N",        "n",        "N", "n" },
  { "\xc3\x91", "\xc3\xb1", "N", "n" }, /* Ñ, ñ */
  { "O",        "o",        "O", "o" },
  { "\xc3\x92", "\xc3\xb2", "O", "o" }, /* Ò, ò */
  { "\xc3\x93", "\xc3\xb3", "O", "o" }, /* Ó, ó */
  { "\xc3\x94", "\xc3\xb4", "O", "o" }, /* Ô, ô */
  { "\xc3\x95", "\xc3\xb5", "O", "o" }, /* Õ, õ */
  { "\xc3\x96", "\xc3\xb6", "O", "o" }, /* Ö, ö */
  { "P",        "p",        "P", "p" },
  { "Q",        "q",        "Q", "q" },
  { "R",        "r",        "R", "r" },
  { "S",        "s",        "S", "s" },
  {  "",        "\xc3\x9f", "",  "sz"}, /* -, ß (upper case of sz ligature does not exist) */
  { "T",        "t",        "T", "t" },
  { "U",        "u",        "U", "u" },
  { "\xc3\x99", "\xc3\xb9", "U", "u" }, /* Ù, ù */
  { "\xc3\x9a", "\xc3\xba", "U", "u" }, /* Ú, ú */
  { "\xc3\x9b", "\xc3\xbb", "U", "u" }, /* Û, û */
  { "\xc3\x9c", "\xc3\xbc", "U", "u" }, /* Ü, ü */
  { "V",        "v",        "V", "v" },
  { "W",        "w",        "W", "w" },
  { "X",        "x",        "X", "x" },
  { "Y",        "y",        "Y", "y" },
  { "\xc3\x9d", "\xc3\xbd", "Y", "y" }, /* Ý, ý */
  { "Z",        "z",        "Z", "z" }
};

static const char **findchar(const char *utf,int column)
{
  for (unsigned idx=0; idx<sizearray(chardef); idx++) {
    const char *letter=chardef[idx][column];
    size_t len=strlen(letter);
    if (len>0 && memcmp(utf,letter,len)==0)
      return chardef[idx];
  }
  return NULL;
}

/** utf8_char() gets the next character from the string, which is assumed to
 *  be in UTF-8 encoding.
 *
 *  \param text   The zero-terminated string, of which the first character is
 *                analyzed.
 *  \param size   Will contain the length of the character (in UTF-8 encoding)
 *                on return. This parameter may be NULL.
 *  \param valid  Will be set to true if the character was decoded correctly,
 *                and to false when the character is not a valid UTF-8 code
 *                point. This parameter may be NULL.
 *
 *  \return The decoded character.
 *
 *  \note In case of an invalid UTF-8 character, this function returns the byte
 *        that the text parameter points to, and the size is set to 1 (and
 *        "valid" is set to false).
 *
 *  \note Adapted from: https://stackoverflow.com/a/1031773
 */
static uint32_t utf8_char(const char *text,int *size,bool *valid)
{
  #define optset(var,value) if (var) *(var)=(value)
  if (!text || !*text) {
    optset(size,0);
    optset(valid,false);
    return 0; /* zero terminator is flagged as invalid and zero-size */
  }

  const unsigned char *bytes=(const unsigned char*)text;
  optset(valid,true);   /* assumption, may be overruled */

  /* ASCII */
  if (bytes[0]<=0x7F) {
    optset(size,1);
    return bytes[0];
  }

  /* non-overlong 2-byte */
  if ((0xC2<=bytes[0] && bytes[0]<=0xDF) &&
      (0x80<=bytes[1] && bytes[1]<=0xBF)) {
    optset(size,2);
    return ((uint32_t)(bytes[0] & 0x1F) << 6) | (uint32_t)(bytes[1] & 0x3F);
  }

  if (( /* excluding overlongs */
       bytes[0]==0xE0 &&
       (0xA0<=bytes[1] && bytes[1]<=0xBF) &&
       (0x80<=bytes[2] && bytes[2]<=0xBF)
      ) ||
      ( /* straight 3-byte */
       ((0xE1<=bytes[0] && bytes[0]<=0xEC) ||
        bytes[0]==0xEE ||
        bytes[0]==0xEF) &&
          (0x80<=bytes[1] && bytes[1]<=0xBF) &&
          (0x80<=bytes[2] && bytes[2]<=0xBF)
      ) ||
      ( /* excluding surrogate pairs */
       bytes[0]==0xED &&
       (0x80<=bytes[1] && bytes[1]<=0x9F) &&
       (0x80<=bytes[2] && bytes[2]<=0xBF)
      )
     ) {
    optset(size,3);
    return ((uint32_t)(bytes[0] & 0x0F) << 12) | ((uint32_t)(bytes[1] & 0x3F) << 6) | (uint32_t)(bytes[2] & 0x3F);
  }

  if (( /* planes 1-3 */
       bytes[0]==0xF0 &&
       (0x90<=bytes[1] && bytes[1]<=0xBF) &&
       (0x80<=bytes[2] && bytes[2]<=0xBF) &&
       (0x80<=bytes[3] && bytes[3]<=0xBF)
      ) ||
      ( /* planes 4-15 */
       (0xF1<=bytes[0] && bytes[0]<=0xF3) &&
       (0x80<=bytes[1] && bytes[1]<=0xBF) &&
       (0x80<=bytes[2] && bytes[2]<=0xBF) &&
       (0x80<=bytes[3] && bytes[3]<=0xBF)
      ) ||
      ( /* plane 16 */
       bytes[0]==0xF4 &&
       (0x80<=bytes[1] && bytes[1]<=0x8F) &&
       (0x80<=bytes[2] && bytes[2]<=0xBF) &&
       (0x80<=bytes[3] && bytes[3]<=0xBF)
      )
     ) {
    optset(size,4);
    return ((uint32_t)(bytes[0] & 0x07) << 18) | ((uint32_t)(bytes[1] & 0x3F) << 12) | ((uint32_t)(bytes[2] & 0x3F) << 6) | (uint32_t)(bytes[3] & 0x3F);
  }

  /* not a valid UTF-8 sequence */
  optset(size,1);
  optset(valid,false);
  return bytes[0];
}


/** utf8_translate() makes a copy of the input string, but with characters with
 *  a match in one column replaced by the characters in a second column. The
 *  returned string must be freed with free().
 */
static char *utf8_translate(const char *source,int matchcolumn,int replacecolumn)
{
  /* dry run, check how long the target string will become */
  int length=0;
  const char *sptr=source;
  while (*sptr) {
    int clen;
    utf8_char(sptr,&clen,NULL);
    const char **cd=findchar(sptr,matchcolumn);
    if (cd)
      length+=strlen(cd[replacecolumn]);
    else
      length+=clen;
    sptr+=clen;
  }

  /* allocate memory */
  char *target=malloc(length+1);
  if (!target)
    return NULL;

  /* run through the string again, now converting it */
  char *tptr=target;
  sptr=source;
  while (*sptr) {
    int clen;
    utf8_char(sptr,&clen,NULL);
    const char **cd=findchar(sptr,matchcolumn);
    if (cd) {
      size_t tlen=strlen(cd[replacecolumn]);
      memcpy(tptr,cd[replacecolumn],tlen);
      tptr+=tlen;
    } else if (clen==1) {
      *tptr++=*sptr;
    } else {
      memcpy(tptr,sptr,clen);
      tptr+=clen;
    }
    sptr+=clen;
  }
  *tptr='\0';

  return target;
}

static char *utf8_lower(const char *source)
{
  /* first check whether the string is valid UTF-8 */
  bool valid=true;
  bool isascii=true;
  const char *sptr=source;
  while (*sptr && valid) {
    int clen;
    utf8_char(sptr,&clen,&valid);
    if (!valid || clen!=1)
      isascii=false;
    sptr+=clen;
  }
  if (!valid || isascii) {
    /* do only plain ASCII case conversion */
    char *target=malloc(strlen(source)+1);
    if (!target)
      return NULL;
    char *tptr=target;
    sptr=source;
    while (*sptr) {
      if ('A'<=*sptr && *sptr<='Z')
        *tptr=*sptr-'A'+'a';
      else
        *tptr=*sptr;
      sptr++;
      tptr++;
    }
    return target;
  }

  return utf8_translate(source,0,1);
}

static char *utf8_noaccents(const char *source)
{
  /* first check whether the string is valid UTF-8 */
  bool valid = true;
  const char *sptr=source;
  while (*sptr && valid) {
    int clen;
    utf8_char(sptr,&clen,&valid);
    sptr+=clen;
  }
  if (!valid)
    return NULL;

  char *target=utf8_translate(source,0,2);
  if (target) {
    char *tmp=utf8_translate(target,1,3);
    if (tmp) {
      free(target);
      target=tmp;
    }
  }
  return target;
}

/* adapted from https://stackoverflow.com/a/23457543 */
static int match(const char *pattern,const char *candidate,int p,int c)
{
  if (pattern[p]=='\0') {
    return c;
  } else if (pattern[p]=='*') {
    while (pattern[p+1]=='*')
      p++;      /* multiple "*" counts as single one */
    if (pattern[p+1]=='\0')
      return c; /* pattern ends with "*" */
    while (candidate[c]!='\0') {
      int r=match(pattern,candidate,p+1,c);
      if (r)
        return r;
      int clen;
      utf8_char(&candidate[c],&clen,NULL);
      c+=clen;
    }
    return 0;   /* candidate is at its end, but pattern is not */
  } else if (pattern[p]=='/' || pattern[p]==' ') {
    while (isspace(pattern[p+1]))
      p++;      /* multiple spaces counts as single one */
    if (candidate[c]!='\0' && !isspace(candidate[c]) && !ispunct(candidate[c]))
      return 0;
    while (candidate[c]!='\0' && (isspace(candidate[c]) || ispunct(candidate[c])))
      c++;
    return match(pattern, candidate, p+1, c);
  } else if (candidate[c]=='\0') {
    return 0;   /* candidate is at its end, but pattern is not */
  } else {
    int clen,plen;
    uint32_t pchar=utf8_char(&pattern[p],&plen,NULL);
    uint32_t cchar=utf8_char(&candidate[c],&clen,NULL);
    if (pattern[p]!='?' && pchar!=cchar)
      return 0;   /* not a wild-card and not the same */
    else
      return match(pattern, candidate, p+plen, c+clen);
  }
}

/** qg_strfind() finds the first occurrence of the "pattern" in the "text", and
 *  optionally allows you to use wild-cards in the pattern and/or do a
 *  case-insensitive search.
 *
 *  \param pattern      The text or pattern to search for in the text string.
 *                      This string must be in ASCII or UTF-8 encoding.
 *  \param text         The string to search in. This string must be in ASCII or
 *                      UTF-8 encoding.
 *  \param length       When the pattern is found, this parameter is set to the
 *                      length (in bytes) of the matched substring. When no
 *                      match is found, this parameter is not set. This
 *                      parameter may be NULL.
 *  \param ignorecase   If true, both parameters "pattern" and "text" are
 *                      converted to lower case, for a case-insensitive search.
 *  \param no_accents   If true, accented characters are replaced by their plain
 *                      equivalents in both parameters "pattern" and "text".
 *
 *  \return A pointer to the matched substring, or NULL if no match was found.
 *
 *  \note  The pattern mat contain the wild-card characters "?", "*" and "/".
 *         "?" matches a single character; "*" matches any number of characters
 *         (including zero); "/" matches any sequence of white-space and/or
 *         punctuation (and it also matches the end of the string). The "/"
 *         wild-card is therefore useful to match complete words.
 *
 *  \note  The "length" parameter is needed, because (due to the wild-cards) the
 *         length of a matched substring may not be the same as the length of
 *         the pattern. Concretely, "*" and "/" may match multiple characters;
 *         a "*" may also match zero characters.
 */
const char *qg_strfind(const char *pattern,const char *text,int *length,bool ignorecase,bool no_accents)
{
  if (!pattern || !*pattern || !text || !*text)
    return NULL;  /* empty pattern matches nothing, empty text string is never matched ... */

  /* ignore leading "*" and leading white-space on patterns */
  while (*pattern=='*' || isspace(*pattern))
    pattern++;
  if (!*pattern)
    return NULL;

  /* make copies, because we may need to change the strings */
  char *plocal=strdup(pattern);
  char *tlocal=strdup(text);
  if (!plocal || !tlocal) {
    if (plocal)
      free(plocal);
    if (tlocal)
      free(tlocal);
    return NULL;
  }

  /* convert to lower case */
  if (ignorecase) {
    char *tmp=utf8_lower(plocal);
    if (tmp) {
      free(plocal);
      plocal=tmp;
    }
    tmp=utf8_lower(tlocal);
    if (tmp) {
      free(tlocal);
      tlocal=tmp;
    }
  }

  /* for search without accents, translate characters in text and pattern */
  if (no_accents) {
    char *tmp=utf8_noaccents(plocal);
    if (tmp) {
      free(plocal);
      plocal=tmp;
    }
    tmp=utf8_noaccents(tlocal);
    if (tmp) {
      free(tlocal);
      tlocal=tmp;
    }
  }

  /* if there are no wild-cards in pattern, we can use strstr() */
  int offset=-1;
  int len=-1;
  if (!strpbrk(pattern,"?*/")) {
    const char *ptr=strstr(tlocal,plocal);
    if (ptr) {
      len=strlen(plocal);
      offset=ptr-tlocal;
    }
  } else {
    for (int i=0; tlocal[i]; i++) {
      int r=match(plocal,tlocal,0,i);
      if (r) {
        offset=i;
        len=r-i;
        break;
      }
    }
  }
  free(plocal);
  free(tlocal);
  if (offset<0)
    return NULL;  /* no match found */

  if (length)
    *length=len;
  return text+offset;
}

/* ***** context ***** */

/** qg_passcontext() returns whether the paragraph passes the context mask
 *  that is given as a parameter.
 *
 *  \param content      The record for a paragraph, including its text and
 *                      patterns.
 *  \param contextmask  The mask to match with.
 *
 *  \return `true` or `false`.
 */
bool qg_passcontext(const QG_LINE_RECORD *content, unsigned long contextmask)
{
  ASSERT(content);
  if (!(content->flags & QFLG_CONTEXT))
    return true;    /* no context: always pass */

  /* go to the start of the context patterns */
  const uint8_t *context=(const uint8_t *)content+sizeof(QG_LINE_RECORD)+content->fmtcodes*sizeof(QG_FORMATCODE);
  context+=strlen((const char *)context)+1;
  uint8_t numPass=*context++;
  uint8_t numBlock=*context++;
  uint32_t *pattern=(uint32_t *)context;

  /* current context must match any of the "pass" patterns (but also pass if
     the pass list is empty) */
  bool pass=(numPass==0);
  for (unsigned idx=0; idx<numPass; idx++) {
    if ((contextmask & *pattern)==*pattern)
      pass=true;
    pattern++;
  }
  if (!pass)
    return false;

  /* current context may not match any of the "block" patterns */
  for (unsigned idx=0; idx<numBlock; idx++) {
    if ((contextmask & *pattern)==*pattern)
      return false;
    pattern++;
  }

  return true;    /* passed all patterns */
}

/* ***** topic history ***** */

/** qg_history_init() intializes the history stack.
 *
 *  \params stack     The history stack structure (which is initialized).
 *  \params maxitems  The maximum number of pages that will be saved in the
 *                    stack. When more pages are pushed onto the stack, the
 *                    oldest items are dropped.
 *
 *  \return `true` or `false` (memory allocation error).
 */
bool qg_history_init(QG_HISTORY *stack,unsigned maxitems)
{
  ASSERT(stack);
  if (stack->pages!=NULL && stack->size==maxitems)
    return true;  /* re-init */
  if (maxitems==0)
    return false;
  stack->pages=malloc(maxitems*sizeof(QG_HISTORY_PAGE));
  if (!stack->pages)
    return false;
  memset(stack->pages,0,maxitems*sizeof(QG_HISTORY_PAGE));
  stack->size=maxitems;
  stack->count=0;
  stack->pos=0;
  return true;
}

/** qg_history_clear() releases memory allocated in initialization.
 *
 *  \param stack      The history stack.
 */
void qg_history_clear(QG_HISTORY *stack)
{
  ASSERT(stack);
  if (stack->pages)
    free(stack->pages);
  memset(stack,0,sizeof(QG_HISTORY));
}

/** qg_history_push() adds a page to the front of a history stack.
 *
 *  \param stack      The history stack.
 *  \param topic      The topic ID.
 *
 *  \return `true` or `false`; the table of contents (auto-generated) is never
 *          pushed to the hostory stack.
 *
 *  \note When the current position is non-zero, all items in front of it are
 *        dropped. The effect is that when you push a page, the "go forward"
 *        queue is cleared. The current position is reset to 0.
 *
 *  \note When the stack is full (and there was no "go forward" queue), the last
 *        item is dropped.
 *
 *  \note When the topic is already present in the stack, it is moved to the
 *        front (and the earlier occurence is removed). However, if the topic is
 *        present at the current position, nothing happens: the topic stays at
 *        its position and the current position is not reset to 0.
 */
bool qg_history_push(QG_HISTORY *stack,uint32_t topic)
{
  ASSERT(stack);
  ASSERT(stack->count<=stack->size);

  /* do not push the generated "content" page (and quit immediately when the
     structure is invalid) */
  if (topic==UINT32_MAX || stack->size==0)
    return false;

  /* if the topic is already at the current position, do nothing */
  ASSERT(stack->count==0 || stack->pos<stack->count);
  if (stack->pos<stack->count && stack->pages[stack->pos].topic==topic)
    return true;  /* return true, because the topic is in the hostory queue */

  /* remove any items at the head that are before the current position */
  if (stack->pos>0) {
    unsigned num=stack->count-stack->pos;
    for (unsigned idx=0; idx<num; idx++)
      stack->pages[idx]=stack->pages[idx+stack->pos];
    stack->count=num;
  }
  /* check whether the topic already exists in the stack */
  unsigned pos;
  for (pos = 0; pos<stack->count && stack->pages[pos].topic!=topic; pos++)
    {}
  if (pos==stack->count) {
    /* topic is not yet in the stack */
    if (stack->count<stack->size) {
        stack->count+=1;        /* there are slots available, grow the stack */
    } else {
        ASSERT(pos==stack->size);
        pos--;                  /* drop the item at the tail of the stack */
    }
  }
  /* move existing items to the back (free the item at the head) */
  while (pos>0) {
    stack->pages[pos]=stack->pages[pos-1];
    pos--;
  }
  stack->pages[0].topic=topic;
  stack->pages[0].scrollpos=0;
  stack->pos=0;
  return true;
}

/** qg_history_markpos() marks the vertical scroll position at the current page
 *  in the history stack. You would call this function before jumping to a new
 *  page, e.g. due to a click on a link. This way, when going back, you will be
 *  able to return to the scroll position at which you left.
 *
 *  \param stack      The history stack.
 *  \param topic      The current topic, used only to test whether the scroll
 *                    position is set for the correct page.
 *  \param scrollpos  The vertical scroll position of the viewer.
 *
 *  \return `true` or `false` (no current page in the stack).
 */
bool qg_history_markpos(QG_HISTORY *stack,uint32_t topic,int scrollpos)
{
  ASSERT(stack);
  if (stack->count==0)
    return 0;
  ASSERT(stack->pos<stack->count);
  if (stack->pages[stack->pos].topic!=topic)
    return false;
  stack->pages[stack->pos].scrollpos=scrollpos;
  return true;
}

/** qg_history_goback() moves back in the history (or tests whether it is
 *  possible to go back).
 *
 *  \param stack      The history stack.
 *  \param topic      The topic ID to jump to. This parameter may be NULL; see
 *                    the notes for the NULL case.
 *  \param scrollpos  The vertical scroll position of the viewer (related to the
 *                    page). This parameter may be NULL.
 *
 *  \return `true` or `false` (reached the tail of the stack).
 *
 *  \note If `topic` is NULL, the history stack is not changed. The return value
 *        indicates whether it would be possible to go back, but the current
 *        position in the stack is not updated.
 */
bool qg_history_goback(QG_HISTORY *stack,uint32_t *topic,int *scrollpos)
{
  ASSERT(stack);
  ASSERT(stack->count==0 || stack->pos<stack->count);
  if (stack->pos+1>=stack->count)
    return false;
  if (topic) {
    stack->pos+=1;
    *topic=stack->pages[stack->pos].topic;
    if (scrollpos)
      *scrollpos=stack->pages[stack->pos].scrollpos;
  }
  return true;
}

/** qg_history_goforward() moves forward in the history (or tests whether it is
 *  possible to go forward).
 *
 *  \param stack      The history stack.
 *  \param topic      The topic ID to jump to. This parameter may be NULL; see
 *                    the notes for the NULL case.
 *  \param scrollpos  The vertical scroll position of the viewer (related to the
 *                    page). This parameter may be NULL.
 *
 *  \return `true` or `false` (already at the front of the stack).
 *
 *  \note If `topic` is NULL, the history stack is not changed. The return value
 *        indicates whether it would be possible to go back, but the current
 *        position in the stack is not updated.
 */
bool qg_history_goforward(QG_HISTORY *stack,uint32_t *topic,int *scrollpos)
{
  ASSERT(stack);
  ASSERT(stack->count==0 || stack->pos<stack->count);
  if (stack->pos==0)
    return false;
  if (topic) {
    stack->pos-=1;
    *topic=stack->pages[stack->pos].topic;
    if (scrollpos)
      *scrollpos=stack->pages[stack->pos].scrollpos;
  }
  return true;
}

/** qg_history_pick() gets an item from the history stack and optionally removes
 *  it; the item is not necessarily the first one.
 *
 *  \param stack      The history stack.
 *  \param index      The position in the stack.
 *  \param remove     If `true`, the item is removed from the stack.
 *  \param topic      The topic ID to jump to. This parameter may be NULL.
 *  \param scrollpos  The vertical scroll position of the viewer (related to the
 *                    page). This parameter may be NULL.
 *
 *  \return `true` or `false` (invalid `index`).
 *
 *  \note To pop off the first page from the stack (as in a LIFO stack), set
 *        `index` to zero and `remove` to true.
 */
bool qg_history_pick(QG_HISTORY *stack,unsigned index,bool remove,uint32_t *topic,int *scrollpos)
{
  ASSERT(stack);
  if (index>=stack->count)
    return false;
  if (topic)
    *topic=stack->pages[index].topic;
  if (scrollpos)
    *scrollpos=stack->pages[index].scrollpos;
  if (remove) {
    stack->pos=0;
    stack->count-=1;
    while (index<stack->count) {
      stack->pages[index]=stack->pages[index+1];
      index++;
    }
  }
  return true;
}

/** qg_history_count() returns the number of items in the history stack.
 */
unsigned qg_history_count(QG_HISTORY *stack)
{
  ASSERT(stack);
  return stack->count;
}
