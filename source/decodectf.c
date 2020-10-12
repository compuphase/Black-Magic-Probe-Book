/*
 * Functions to decode a byte stream matching a trace stream description (TSDL
 * file). It uses data structures created by parsectf.
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
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined __linux__
  #include <bsd/string.h>
#elif defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
  #include "strlcpy.h"
#endif
#if defined _MSC_VER
    #define strdup(s)         _strdup(s)
#endif

#include "parsetsdl.h"
#include "decodectf.h"


#if !defined sizearray
  #define sizearray(a)  (sizeof(a) / sizeof((a)[0]))
#endif

typedef struct tagTRACEMSG {
  uint16_t streamid;
  double timestamp;
  const char *message;
} TRACEMSG;

static const unsigned char magic[] = { 0xc1, 0x1f, 0xfc, 0xc1 };

enum {
  STATE_SCAN_MAGIC,
  STATE_SKIP_UID,
  STATE_GET_STREAMID,
  STATE_GET_EVENTID,
  STATE_GET_TIMESTAMP,
  STATE_GET_FIELDS,
};
static int state = STATE_SCAN_MAGIC;                /* current state */
static const CTF_PACKET_HEADER *pkt_header = NULL;  /* general packet header definition */
static const CTF_EVENT_HEADER *evt_header = NULL;   /* event header definition for "current" stream */
static const CTF_EVENT *event = NULL;               /* event currently being parsed */
static const CTF_EVENT_FIELD *field = NULL;         /* field currently being parsed */
static const CTF_CLOCK *clock;                      /* clock set for the stream */
static double timestamp = 0.0;                      /* timestamp in the event header */

static unsigned char *cache = NULL;
static size_t cache_size = 0;
static size_t cache_filled = 0;

static char *msgbuffer = NULL;
static size_t msgbuffer_size = 0;
static size_t msgbuffer_filled = 0;

static TRACEMSG *msgstack = NULL;
static size_t msgstack_size = 0;
static size_t msgstack_head = 0;
static size_t msgstack_tail = 0;

static const DWARF_SYMBOLLIST *symboltable = NULL;


static void cache_grow(size_t extra)
{
  if (cache_filled + extra > cache_size) {
    if (cache_size == 0)
      cache_size = 32;
    while (cache_size < cache_filled + extra)
      cache_size *= 2;
    if (cache == NULL)
      cache = (unsigned char*)malloc(cache_size);
    else
      cache = (unsigned char*)realloc(cache, cache_size);
    assert(cache != NULL);
  }
}

static void cache_clear(void)
{
  if (cache != NULL) {
    free((void*)cache);
    cache = NULL;
  }
  cache_size = 0;
  cache_filled = 0;
}

static void cache_reset(void)
{
  cache_filled = 0;
}

static void msgbuffer_grow(size_t extra)
{
  if (msgbuffer_filled + extra > msgbuffer_size) {
    if (msgbuffer_size == 0)
      msgbuffer_size = 32;
    while (msgbuffer_size < msgbuffer_filled + extra)
      msgbuffer_size *= 2;
    if (msgbuffer == NULL)
      msgbuffer = (char*)malloc(msgbuffer_size);
    else
      msgbuffer = (char*)realloc(msgbuffer, msgbuffer_size);
    assert(msgbuffer != NULL);
  }
}

static void msgbuffer_clear(void)
{
  if (msgbuffer != NULL) {
    free((void*)msgbuffer);
    msgbuffer = NULL;
  }
  msgbuffer_size = 0;
  msgbuffer_filled = 0;
}

static void msgbuffer_reset(void)
{
  msgbuffer_filled = 0;
}

static void msgbuffer_append(const char *data, int length)
{
  assert(data != NULL);
  if (length < 0)
    length = strlen(data);
  msgbuffer_grow(length);
  memcpy(msgbuffer + msgbuffer_filled, data, length);
  msgbuffer_filled += length;
}

static void msgstack_grow(void)
{
  /* always grows by one entry, so no parameter required
     it is a circular buffer, so one extra (unused) entry is needed */
  size_t filled;

  if (msgstack_tail >= msgstack_head)
    filled = msgstack_tail - msgstack_head;
  else
    filled = msgstack_size - (msgstack_head - msgstack_tail);
  assert(filled <= msgstack_size);

  if (filled + 2 > msgstack_size) {
    TRACEMSG *curstack = msgstack;
    size_t cursize = msgstack_size;
    size_t idx;
    if (msgstack_size == 0)
      msgstack_size = 16;
    msgstack_size *= 2;
    assert(msgstack_size > filled + 2);
    msgstack = (TRACEMSG*)malloc(msgstack_size * sizeof(TRACEMSG));
    assert(msgstack != NULL);
    /* copy existing entries from the old stack to the new one */
    assert(msgstack_head == msgstack_tail || curstack != NULL);
    idx = 0;
    while (msgstack_head != msgstack_tail) {
      msgstack[idx++] = curstack[msgstack_head];
      if (++msgstack_head > cursize)
        msgstack_head = 0;
    }
    msgstack_head = 0;
    msgstack_tail = idx;
    if (curstack != NULL)
      free((void*)curstack);
    /* clear the new entries */
    while (idx < msgstack_size) {
      memset(msgstack + idx, 0, sizeof(TRACEMSG));
      idx++;
    }
  }
}

static void msgstack_clear(void)
{
  if (msgstack != NULL) {
    while (msgstack_head != msgstack_tail) {
      free((void*)(msgstack[msgstack_head].message));
      if (++msgstack_head >= msgstack_size)
        msgstack_head = 0;
    }
    free((void*)msgstack);
    msgstack = NULL;
  }
  msgstack_size = 0;
  msgstack_head = 0;
  msgstack_tail = 0;
}

static void msgstack_push(uint16_t streamid, double timestamp, const char *message)
{
  msgstack_grow();
  assert(msgstack_tail < msgstack_size);
  msgstack[msgstack_tail].streamid = streamid;
  msgstack[msgstack_tail].timestamp = timestamp;
  msgstack[msgstack_tail].message = strdup(message);
  if (++msgstack_tail >= msgstack_size)
    msgstack_tail = 0;
}

/** msgstack_pop() gets a message from a FIFO stack/queue. It returns 0 if the
 *  stack is empty, or 1 if a message was copied. The data is not copied if the
 *  parameters are NULL (so by setting all parameters to NULL, the message at
 *  the head is dropped).
 */
int msgstack_pop(uint16_t *streamid, double *timestamp, char *message, size_t size)
{
  if (msgstack_head == msgstack_tail)
    return 0;
  if (streamid != NULL)
    *streamid = msgstack[msgstack_head].streamid;
  if (timestamp != NULL)
    *timestamp = msgstack[msgstack_head].timestamp;
  assert(msgstack[msgstack_head].message != NULL);
  if (message != NULL && size > 0)
    strlcpy(message, msgstack[msgstack_head].message, size);
  free((void*)(msgstack[msgstack_head].message));
  if (++msgstack_head >= msgstack_size)
    msgstack_head = 0;
  return 1;
}

/** msgstack_peek() returns information on the message at the head, but
 *  without popping if from the list. The message pointer is returned as a
 *  pointer into the list.
 *  \return 1 on success, 0 on failure.
 */
int msgstack_peek(uint16_t *streamid, double *timestamp, const char **message)
{
  if (msgstack_head == msgstack_tail)
    return 0;
  if (streamid != NULL)
    *streamid = msgstack[msgstack_head].streamid;
  if (timestamp != NULL)
    *timestamp = msgstack[msgstack_head].timestamp;
  if (message != NULL)
    *message = msgstack[msgstack_head].message;
  return 1;
}

void ctf_set_symtable(const DWARF_SYMBOLLIST *symtable)
{
  symboltable = symtable;
}

static int lookup_symbol(uint32_t address, char *symname, size_t maxlength)
{
  const DWARF_SYMBOLLIST *sym;

  if (symboltable == NULL)
    return 0;
  sym = dwarf_sym_from_address(symboltable, address, 1);
  if (sym == NULL)
    return 0;
  assert(sym->name != NULL);
  strlcpy(symname, sym->name, maxlength);
  return 1;
}

static void str_reverse(char *str, int length)
{
  char *tail;

  assert(str != NULL);
  assert(length >= 0);
  tail = str + length - 1;
  while (tail > str) {
    char temp = *str;
    *str++ = *tail;
    *tail-- = temp;
  }
}

static char *fmt_uint32(uint32_t num, char *str, int base)
{
  int i = 0;

  /* handle 0 explicitely, otherwise empty string is printed for 0 */
  if (num == 0) {
    str[i++]= '0';
    str[i]= '\0';
    return str;
  }

  while (num != 0) {
    int rem = num % base;
    str[i++] = (char)((rem > 9) ? (rem - 10) + 'a' : rem + '0');
    num = num / base;
  }
  str[i]= '\0';         /* append string terminator */

  str_reverse(str, i);  /* reverse the string */
  return str;
}

static char *fmt_int32(int32_t num, char *str, int base)
{
  /* negative numbers are handled only with base 10 */
  if (num < 0 && base == 10) {
    str[0] = '-';
    fmt_uint32((uint32_t)-num, str + 1, base);
  } else {
    fmt_uint32((uint32_t)num, str, base);
  }
  return str;
}

static char *fmt_uint64(uint64_t num, char *str, int base)
{
  int i = 0;

  /* handle 0 explicitely, otherwise empty string is printed for 0 */
  if (num == 0) {
    str[i++]= '0';
    str[i]= '\0';
    return str;
  }

  while (num != 0) {
    int rem = num % base;
    str[i++] = (char)((rem > 9) ? (rem - 10) + 'a' : rem + '0');
    num = num / base;
  }
  str[i]= '\0';         /* append string terminator */

  str_reverse(str, i);  /* reverse the string */
  return str;
}

static char *fmt_int64(int64_t num, char *str, int base)
{
  /* negative numbers are handled only with base 10 */
  if (num < 0 && base == 10) {
    str[0] = '-';
    fmt_uint64((uint64_t)-num, str + 1, base);
  } else {
    fmt_uint64((uint64_t)num, str, base);
  }
  return str;
}

static void format_field(const char *fieldname, const CTF_TYPE *type, const unsigned char *data)
{
  msgbuffer_append(fieldname, -1);
  msgbuffer_append(" = ", 3);

  switch (type->typeclass) {
  case CLASS_INTEGER: {
    char txt[128];
    uint8_t base = type->base;
    if (base < 2 || base > 16)
      base = 10;
    if (type->size > 32) {
      uint64_t v = 0;
      memcpy(&v, data, type->size / 8);
      if (type->flags & TYPEFLAG_SIGNED)
        fmt_int64((int64_t)v, txt, base);
      else
        fmt_uint64(v, txt, base);
    } else {
      uint32_t v = 0;
      memcpy(&v, data, type->size / 8);
      if (base == CTF_BASE_ADDR) {
        if (!lookup_symbol(v, txt, sizearray(txt)))
          fmt_uint32(v, txt, 16);
      } else if (type->flags & TYPEFLAG_SIGNED) {
        fmt_int32((int32_t)v, txt, base);
      } else {
        fmt_uint32(v, txt, base);
      }
    }
    msgbuffer_append(txt, -1);
    break;
  } /* case */

  case CLASS_FLOAT: {
    char txt[32];
    if (type->size > 32) {
      double v = 0;
      memcpy(&v, data, type->size / 8);
      sprintf(txt, "%f", v);
    } else {
      float v = 0;
      memcpy(&v, data, type->size / 8);
      sprintf(txt, "%f", v);
    }
    msgbuffer_append(txt, -1);
    break;
  } /* case */

  case CLASS_ENUM: {
    const CTF_KEYVALUE *kv;
    int32_t v = 0;
    memcpy(&v, data, type->size / 8);
    for (kv = type->keys->next; kv != NULL && kv->value != v; kv = kv->next)
      /* nothing */;
    if (kv != NULL) {
      msgbuffer_append(kv->name, -1);
    } else {
      char txt[32];
      sprintf(txt, "(%d)", (int)v);
      msgbuffer_append(txt, -1);
    }
    break;
  } /* case */

  case CLASS_STRING:
    msgbuffer_append("\"", 1);
    msgbuffer_append((const char*)data, -1);
    msgbuffer_append("\"", 1);
    break;

  case CLASS_STRUCT:
    msgbuffer_append("{ ", 2);
    if (type->fields != NULL) {
      const CTF_TYPE *subtype;
      for (subtype = type->fields->next; subtype != NULL; subtype = subtype->next) {
        if (subtype->size / 8 == 0)
          break;
        if (subtype != type->fields->next)
          msgbuffer_append(", ", 2);
        format_field(subtype->identifier, subtype, data);
        data += (subtype->size / 8);
      }
    }
    msgbuffer_append(" }", 2);
    break;

  default:
    assert(0);
  } /* switch (typeclass) */
}

int ctf_decode(const unsigned char *stream, size_t size, long channel)
{
  size_t idx, len, result;

  cache_reset();
  result = 0;
  idx = 0;

restart:
  if (idx >= size)
    return result;

  switch (state) {
  case STATE_SCAN_MAGIC:
    if (pkt_header == NULL)
      pkt_header = packet_header();
    assert(pkt_header != NULL);
    if (pkt_header->header.magic_size == 0) {
      /* advance state and restart */
      state++;
      goto restart;
    }
    if (cache_filled > 0) {
      /* the first bytes in cache already matched, check the remaining bytes */
      len = (pkt_header->header.magic_size / 8) - cache_filled;
      assert(len > 0);
      assert(idx == 0);
      if (len > size)
        len = size;
      if (memcmp(stream, magic + cache_filled, len) == 0) {
        /* match, check whether this is still a patial match */
        if (cache_filled + len == (pkt_header->header.magic_size / 8u)) {
          state++;
          idx += len;
          cache_reset();
          goto restart;
        } else {
          cache_filled += len;
          return result; /* nothing to do further, wait for more bytes */
        }
      } else {
        /* mismatch, re-scan */
        cache_reset();
      }
    }
    if (state == STATE_SCAN_MAGIC) {
      while (idx < size) {
        while (idx < size && stream[idx] != magic[0])
          idx++;  /* find first byte of the magic */
        if (idx < size) {
          /* potential start of magic found */
          len = pkt_header->header.magic_size / 8;
          if (idx + len > size)
            len = size - idx;
          if (memcmp(stream, magic + cache_filled, len) == 0) {
            /* match, check whether this is still a patial match */
            if (len == pkt_header->header.magic_size / 8u) {
              state++;  /* full match -> advance state & restart */
              idx += len;
              cache_reset();
              goto restart;
            } else {
              assert(cache_filled == 0);
              cache_filled = len;
              return result; /* nothing to do further, wait for more bytes */
            }
          } else {
            idx += 1;
          }
        }
      }
    }
    break;

  case STATE_SKIP_UID:
    len = (pkt_header->header.uuid_size / 8) - cache_filled;
    if (idx + len <= size) {
      /* UUID fully skipped (or uuid_size == 0) */
      state++;
      idx += len;
      cache_reset();
      goto restart;
    } else {
      cache_filled += size - idx;
      /* no data is truly stored in the cache, because we are skipping
         this field */
    }
    break;

  case STATE_GET_STREAMID:
    if (pkt_header->header.streamid_size == 0) {
      state++;
      assert(cache_filled == 0);
      goto restart;
    }
    len = (pkt_header->header.streamid_size / 8) - cache_filled;
    if (idx + len <= size) {
      /* get the stream.id; this code assumes Little Endian */
      unsigned long streamid = 0;
      if (cache_filled > 0) {
        assert(cache_filled < pkt_header->header.streamid_size / 8u);
        memcpy((unsigned char*)&streamid, cache, cache_filled);
      }
      assert(len > 0 && len <= pkt_header->header.streamid_size / 8u);
      memcpy((unsigned char*)&streamid + cache_filled, stream + idx, len);
      channel = (long)streamid; /* stream id in the header overrules the parameter */
      state++;
      idx += len;
      cache_reset();
      goto restart;
    } else {
      len = size - idx;
      cache_grow(len);
      memcpy(cache + cache_filled, stream + idx, len);
      cache_filled += len;
    }
    break;

  case STATE_GET_EVENTID:
    /* get the event header from the stream.id or the passed-in channel */
    { /* local block */
      const CTF_STREAM *s = stream_by_id(channel);
      if (s != NULL) {
        evt_header = &s->event;
        clock = (s->clock != NULL) ? clock_by_name(s->clock->selector) : NULL;
      } else {
        /* stream not found, drop the decoding */
        state = STATE_SCAN_MAGIC;
        assert(cache_filled == 0);
        goto restart;
      }
    }
    assert(evt_header != NULL);
    if (evt_header->header.id_size == 0) {
      state++;
      assert(cache_filled == 0);
      goto restart;
    }
    len = (evt_header->header.id_size / 8) - cache_filled;
    if (idx + len <= size) {
      /* get the event.id; this code assumes Little Endian */
      unsigned long id = 0;
      assert(cache_filled + len < sizeof id);
      if (cache_filled > 0) {
        assert(cache_filled < evt_header->header.id_size / 8u);
        memcpy((unsigned char*)&id, cache, cache_filled);
      }
      assert(len > 0 && len <= evt_header->header.id_size / 8u);
      memcpy((unsigned char*)&id + cache_filled, stream + idx, len);
      /* get the event from the id */
      event = event_by_id(id);
      if (event != NULL) {
        assert(msgbuffer_filled == 0);
        msgbuffer_append(event->name, -1);
        state++;
        idx += len;
        field = event->field_root.next;
        if (field == NULL) {
          /* this event has no fields */
          msgbuffer_append("", 1);  /* force zero-terminate msgbuffer */
          msgstack_push((uint16_t)event->stream_id, timestamp, msgbuffer);
          msgbuffer_reset();
          result += 1;  /* flag: one more trace message completed */
          state = STATE_SCAN_MAGIC;
        }
      } else {
        /* event not found, drop the decoding */
        state = STATE_SCAN_MAGIC;
      }
      cache_reset();
      goto restart;
    } else {
      len = size - idx;
      cache_grow(len);
      memcpy(cache + cache_filled, stream + idx, len);
      cache_filled += len;
    }
    break;

  case STATE_GET_TIMESTAMP:
    assert(evt_header != NULL);
    if (evt_header->header.timestamp_size == 0) {
      state++;
      assert(cache_filled == 0);
      goto restart;
    }
    len = (evt_header->header.timestamp_size / 8) - cache_filled;
    if (idx + len <= size) {
      /* get the timestamp; this code assumes Little Endian */
      uint64_t tstamp = 0;
      assert(cache_filled + len < sizeof tstamp);
      if (cache_filled > 0)
        memcpy((unsigned char*)&tstamp, cache, cache_filled);
      memcpy((unsigned char*)&tstamp + cache_filled, stream + idx, len);
      /* convert timestamp to seconds */
      if (clock != NULL)
        timestamp = (double)(tstamp + clock->offset) / (double)clock->frequeny + clock->offset_s;
      state++;
      idx += len;
      cache_reset();
      goto restart;
    } else {
      len = size - idx;
      cache_grow(len);
      memcpy(cache + cache_filled, stream + idx, len);
      cache_filled += len;
    }
    break;

  case STATE_GET_FIELDS:
    assert(field != NULL);
    switch (field->type.typeclass) {
    case CLASS_INTEGER:
    case CLASS_FLOAT:
    case CLASS_ENUM:
    case CLASS_STRUCT:
      assert(field->type.size / 8 > 0);
      len = (field->type.size / 8) - cache_filled;
      if (idx + len > size)
        len = size - idx;
      cache_grow(len);
      memcpy(cache + cache_filled, stream + idx, len);
      idx += len;
      cache_filled += len;
      if (cache_filled < (field->type.size / 8))
        return result;  /* full field not yet in the buffer, wait for more incoming bytes */
      break;
    case CLASS_STRING:
      /* store the string (temporarily) in the cache */
      while (idx < size && stream[idx] != 0) {
        cache_grow(1);
        cache[cache_filled++] = stream[idx++];
      }
      if (idx < size) {
        assert(stream[idx] == 0);
        cache_grow(1);
        cache[cache_filled++] = 0;
        idx++;
      } else {
        /* zero terminating byte not found, wait for more incoming bytes */
        return result;
      }
      break;
    default:
      assert(0);
    }
    /* format the field */
    if (field == event->field_root.next)
      msgbuffer_append(": ", 2);  /* first field */
    else
      msgbuffer_append(", ", 2);  /* next field */
    format_field(field->name, &field->type, cache);
    cache_reset();
    /* move to the next field */
    field = field->next;
    if (field == NULL) {
      msgbuffer_append("", 1);  /* force zero-terminate msgbuffer */
      msgstack_push((uint16_t)event->stream_id, timestamp, msgbuffer);
      msgbuffer_reset();
      result += 1;  /* flag: one more trace message completed */
      state = STATE_SCAN_MAGIC;
    }
    goto restart;
  }

  return result;
}

void ctf_decode_cleanup(void)
{
  cache_clear();
  msgbuffer_clear();
  msgstack_clear();
}

void ctf_decode_reset(void)
{
  cache_reset();
  msgbuffer_reset();
  state = STATE_SCAN_MAGIC;
}
