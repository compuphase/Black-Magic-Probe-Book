/*
 * The GDB "Remote Serial Protocol" support.
 *
 * Copyright 2019-2023 CompuPhase
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
#if defined _WIN32
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#endif
#if defined __linux__
# include <unistd.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bmp-support.h"
#include "gdb-rsp.h"
#include "rs232.h"
#include "tcpip.h"

#if defined _MSC_VER
# if _MSC_VER < 1900
#   define inline __inline
# endif
#endif

#if defined FORTIFY
# include <alloc/fortify.h>
#endif


#define TIMEOUT       500
#define POLL_INTERVAL 50
#define RETRIES       3


static unsigned char *cache = NULL; /* cache for received data */
static size_t cache_size = 0;       /* maximum size of the cache */
static size_t cache_idx = 0;        /* index to the free area of the cache */


static const char hexdigits[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8',
                               '9', 'a', 'b', 'c', 'd', 'e', 'f' };
static inline char int2hex(int v)
{
  assert(v >= 0 && v < 16);
  return hexdigits[v];
}

static inline int hex2int(char ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  return -1;
}

/** gdbrsp_hex2array() converts an ASCIIZ string encoded with hexadecimal values
 *  to an array with byte values.
 *
 *  \param hex    [in] An ASCIIZ string with the hex digits.
 *  \param byte   [out] The array filled with the decoded values. The required
 *                size is strlen(hex) / 2.
 *  \param size   The size of the "byte" parameter.
 *
 *  \return true on success, false on failure (reasons for failure: 1) the input
 *          string has non-hex charactes, 2) the "size" parameter is too small).
 *
 *  \note In-place conversion is allowed (parameters "hex" and "byte" may point
 *        to the same buffer).
 */
bool gdbrsp_hex2array(const char *hex, unsigned char *byte, size_t size)
{
  assert(hex != NULL && byte != NULL);
  while (hex[0] != '\0' && hex[1] != '\0' && size > 0) {
    int h = hex2int(hex[0]);
    int l = hex2int(hex[1]);
    if (h == -1 || l == -1)
      return false;
    *byte++ = (unsigned char)((h << 4) | l);
    hex += 2;
    size -= 1;
  }
  return *hex == '\0';
}

#if !defined NDEBUG
  static FILE *log_file = NULL;
  static char *log_buf = NULL;
  static size_t log_bufsz = 0;
  static void gdbrsp_log(const char *text, size_t size, bool transmit)
  {
    assert(text != NULL);
    assert(size > 0);
    if (log_file == NULL)
      log_file = fopen("gdbrsp.log", "wt");
    if (log_file != NULL) {
      /* get size after escaping */
      size_t sz = 0;
      for (size_t i = 0; i < size; i++)
        sz += (text[i] >= ' ' && text[i] < 127) ? 1 : 4;
      sz += 5;  /* for leading '>> ' and terminating '\n\0' */
      /* grow the buffer, if necessary */
      if (sz > log_bufsz) {
        char *buf = malloc(sz);
        if (buf != NULL) {
          if (log_buf != NULL)
            free(log_buf);
          log_buf = buf;
          log_bufsz = sz;
        }
      }
      /* fill the buffer */
      if (sz <= log_bufsz) {
        memcpy(log_buf, transmit ? ">> " : "<< ", 3);
        size_t j = 3;
        for (size_t i = 0; i < size; i++) {
          if (text[i] >= ' ' && text[i] < 127) {
            log_buf[j] = text[i];
            j += 1;
          } else {
            log_buf[j] = '\\';
            log_buf[j+1] = 'x';
            log_buf[j+2] = int2hex((text[i] >> 4) & 0x0f);
            log_buf[j+3] = int2hex(text[i] & 0x0f);
            j += 4;
          }
        }
        log_buf[j] = '\n';
        log_buf[j+1] = '\0';
        /* dump it to file */
        fputs(log_buf, log_file);
      }
    }
  }
  static void gdbrsp_closelog(void)
  {
    if (log_file != NULL) {
      fclose(log_file);
      log_file = NULL;
    }
    if (log_buf != NULL) {
      free(log_buf);
      log_buf = NULL;
    }
    log_bufsz = 0;
  }
#else
# define gdbrsp_log(text, size, transmit)
# define gdbrsp_closelog()
#endif

/** gdbrsp_packetsize() sets the maximum size of incoming packets. It uses
 *  this to allocate a buffer for incoming data. If the size is set to 0, the
 *  current buffer is freed. Otherwise, the cache for incoming packets is only
 *  adjusted to receive bigger packets (it does not shrink).
 */
void gdbrsp_packetsize(size_t size)
{
  if (size == 0) {
    if (cache != NULL) {
      free(cache);
      cache = NULL;
    }
    cache_size = size;
    cache_idx = 0;
    gdbrsp_closelog();
  } else if (size > cache_size) {
    unsigned char *buf = malloc(size * sizeof(char));
    if (buf != NULL) {
      if (cache != NULL) {
        memcpy(buf, cache, cache_idx * sizeof(char));
        free(cache);
      }
      cache = buf;
      cache_size = size;
    }
  }
}

/** gdbrsp_recv() returns a received packet (from the gdbserver).
 *
 *  \param buffer   [OUT] Will hold the received data, but the payload only (so
 *                  the '$' at the start and the checksum at the end are
 *                  stripped off).
 *  \param size     The maximum number of bytes that the buffer can hold. Note
 *                  that the checksum bytes are stored in this buffer before
 *                  analysis, so the buffer must be 3 bytes larger than the
 *                  largest expected response).
 *  \param timeout  Time to wait for a response, in ms.
 *
 *  \return The number of bytes received, or zero on time-out (or error). The
 *          return value can be bigger than parameter size, which indicates that
 *          the received data was bigger than the buffer size (so the buffer
 *          contains truncated data.
 *
 *  \note Console output messages by the target will have a lower case 'o' at
 *        the start of the output buffer (not an upper case letter). The message
 *        has already been translated from hex encoding to ASCII.
 */
size_t gdbrsp_recv(char *buffer, size_t size, int timeout)
{
  if (!bmp_isopen())
    return 0;
  if (cache == NULL) {
    gdbrsp_packetsize(256);
    if (cache == NULL)
      return 0;
  }

  int cycles = (timeout < 0) ? -1 : (timeout + POLL_INTERVAL - 1) / POLL_INTERVAL;
  int chk_cache = (cache_idx > 0);  /* analyse data in the cache even if no new data is received */
  size_t head = 0;
  while (cache_idx < cache_size) {
    size_t count;
    if (bmp_comport() != NULL)
      count = rs232_recv(bmp_comport(), cache + cache_idx, cache_size - cache_idx);
    else
      count = tcpip_recv(cache + cache_idx, cache_size - cache_idx);
    cache_idx += count;
    if (count > 0 || chk_cache) {
      chk_cache = 0;
      /* check start character (throw away everything before this) */
      if (head == 0) {
        size_t idx;
        for (idx = 0; idx < cache_idx && cache[idx] != '$'; idx++)
          /* nothing */;
        if (idx == cache_idx) {
          cache_idx = 0;        /* throw away all received data */
        } else {
          assert(idx < cache_idx && cache[idx] == '$');
          head = idx + 1;       /* also skip '$' */
        }
      }
      /* check whether we have an end mark and a checksum */
      size_t tail;
      for (tail = head; tail < cache_idx && cache[tail] != '#'; tail++)
        /* nothing */;
      if (tail + 2 < cache_idx) {
        /* '#' found and 2 characters follow, verify the checksum */
        int chksum = (hex2int(cache[tail + 1]) << 4) | hex2int(cache[tail + 2]);
        int sum = 0;
        for (size_t idx = head; idx < tail; idx++)
          sum += cache[idx];
        sum &= 0xff;
        if (sum == chksum) {
          /* confirm reception and copy to the buffer */
          if (bmp_comport() != NULL)
            rs232_xmit(bmp_comport(), (const unsigned char*)"+", 1);
          else
            tcpip_xmit((const unsigned char*)"+", 1);
          count = tail - head;  /* number of payload bytes */
          if (count >= 3 && cache[head] == 'O' && isxdigit(cache[head + 1]) && isxdigit(cache[head + 2])) {
            /* convert the first letter to a lower-case 'o', so that an output
               message of the single letter 'K' won't be mis-interpreted as 'OK' */
            buffer[0] = 'o';
            count = (count + 1) / 2;
            unsigned c;
            size_t idx;
            for (c = 1, idx = head + 1; c < count && c < size; c += 1, idx += 2)
              buffer[c] = (char)((hex2int(cache[idx]) << 4) | hex2int(cache[idx + 1]));
          } else {
            unsigned c;
            size_t idx;
            for (c = 0, idx = head; c < count && c < size; c += 1, idx += 1) {
              if (cache[idx] == '}') {
                /* escaped binary encoding */
                idx += 1;
                buffer[c] = cache[idx] ^ 0x20;
              } else {
                buffer[c] = cache[idx];
                /* the Black Magic Probe does currently not support run-length
                   encoding, so we currently do not check for it */
              }
            }
          }
          /* remove the packet from the cache */
          tail += 3;
          assert(tail <= cache_idx);
          if (tail < cache_idx)
            memmove(cache, cache + tail, cache_idx - tail);
          cache_idx -= tail;
          gdbrsp_log(buffer, count, false);
          return count; /* return payload size (excluding checksum) */
        } else {
          /* send NAK */
          if (bmp_comport() != NULL)
            rs232_xmit(bmp_comport(), (const unsigned char*)"-", 1);
          else
            tcpip_xmit((const unsigned char*)"-", 1);
        }
        /* remove the packet from the cache */
        tail += 3;
        assert(tail <= cache_idx);
        if (tail < cache_idx)
          memmove(cache, cache + tail, cache_idx - tail);
        cache_idx -= tail;
        head = 0;
      }
    }
    if (cycles > 0 && --cycles == 0) {
      gdbrsp_log("-", 1, false);
      return 0;       /* nothing received within timeout period */
    }
#   if defined _WIN32
      Sleep(POLL_INTERVAL);
#   else
      usleep(POLL_INTERVAL * 1000);
#   endif
  }

  /* when arrived here, tail == size (so the buffer is filled to its maximum),
     but no end mark with checksum was yet received, meaning that the buffer was
     too small; this should never happen */
  assert(0);
  return 0;
}

/** gdbrsp_xmit() transmits a packet to the gdbserver.
 *
 *  \param buffer   [IN] The buffer. It must contain a complete command, but
 *                  without the '$' prefix and the '#nn' suffix (where 'nn' is
 *                  the checksum).
 *  \param size     The number of characters/bytes in the buffer. If set to -1,
 *                  the buffer is assumed to contain a zero-terminated string.
 *
 *  \return true on success, false on timeout or error.
 */
bool gdbrsp_xmit(const char *buffer, int size)
{
  assert(buffer != NULL);
  if (!bmp_isopen())
    return false;

  gdbrsp_log(buffer, (size == -1) ? strlen(buffer) : size, true);

  size_t buflen = (size == -1) ? strlen(buffer) : size;
  size_t payload_offs = 0;
  if (buflen > 6 && memcmp(buffer, "qRcmd,", 6) == 0) {
    payload_offs = 6;
  } else if (buflen > 5 && memcmp(buffer, "vRun;", 5) == 0) {
    payload_offs = 5;
  } else {
    size = 0;
    for (size_t idx = 0; idx < buflen; idx++) {
      size += 1;
      if (buffer[idx] == '$' || buffer[idx] == '#' || buffer[idx] == '}')
        size += 1;      /* these characters must be escaped */
    }
  }
  if (payload_offs > 0)
    size = ((buflen - payload_offs) * 2) + payload_offs;  /* payload is hex-encoded */
  size += 4;            /* add '$' prefix and '#nn' suffix */

  unsigned char *fullbuffer = malloc(size);
  if (fullbuffer == NULL)
    return false;

  /* add prefix, handle payload */
  *fullbuffer = '$';
  if (payload_offs > 0) {
    const char *src = buffer + payload_offs;
    unsigned char *dest = fullbuffer + payload_offs + 1;
    size_t count = buflen - payload_offs;
    memcpy(fullbuffer + 1, buffer, payload_offs); /* copy qRcmd or vRun */
    while (count > 0) {
      *dest++ = int2hex((*src >> 4) & 0x0f);
      *dest++ = int2hex(*src & 0x0f);
      src++;
      count--;
    }
  } else {
    const char *src = buffer;
    unsigned char *dest = fullbuffer + 1;
    for (size_t idx = 0; idx < buflen; idx++) {
      for (idx = 0; idx < buflen; idx++) {
        if (*src == '$' || *src == '#' || *src == '}') {
          *dest++ = '}';        /* these characters must be escaped */
          *dest++ = *src++ ^ 0x20;
        } else {
          *dest++ = *src++;
        }
      }
    }
  }
  /* add checksum */
  int sum = 0;
  for (int idx = 1; idx < size - 3; idx++)
    sum += fullbuffer[idx];     /* run over fullbuffer, so that the checksum is over the translated buffer */
  *(fullbuffer + size - 3) = '#';
  *(fullbuffer + size - 2) = int2hex((sum >> 4) & 0x0f);
  *(fullbuffer + size - 1) = int2hex(sum & 0x0f);

  for (int retry = 0; retry < RETRIES; retry++) {
    if (bmp_comport() != NULL)
      rs232_xmit(bmp_comport(), fullbuffer, size);
    else
      tcpip_xmit(fullbuffer, size);
    for (int cycle = 0; cycle < TIMEOUT / POLL_INTERVAL; cycle++) {
      size_t count;
      do {
        unsigned char buf[10];
        if (bmp_comport() != NULL)
          count = rs232_recv(bmp_comport(), buf, 1);
        else
          count = tcpip_recv(buf, 1);
        if (count == 1) {
          if (buf[0] == '+') {
            free(fullbuffer);
            return true;
          }
          if (buf[0] == '-') {
            cycle = TIMEOUT / POLL_INTERVAL;  /* retransmit without timeout */
            break;
          }
        }
      } while (count == 1);
      #if defined _WIN32
        Sleep(POLL_INTERVAL);
      #else
        usleep(POLL_INTERVAL * 1000);
      #endif
    }
  }

  free(fullbuffer);
  return false;
}

/** gdbrsp_clear() clears the cache, to remove any superfluous OK or error
 *  codes that GDB sent.
 */
void gdbrsp_clear(void)
{
  cache_idx = 0;
}

