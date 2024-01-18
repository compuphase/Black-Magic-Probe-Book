/*
 * String matching with wildcard support (globbing).
 *
 * Copyright 2024 CompuPhase
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

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "strmatch.h"


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
uint32_t utf8_char(const char *text,int *size,bool *valid)
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

/** match() matches a string against a pattern.
 *
 *  \param pattern    The pattern with wildcards.
 *  \param candidate  The string to test.
 *  \param p          The current index in the pattern.
 *  \param c          The current index in the candidate.
 *
 *  \return The position in the candidate string where the match finished, or 0
 *          on no match.
 *
 *  \note Only the start of the candidate string is matched (as if there is an
 *        implicit `*` wildcard at the end of the pattern).
 *
 *  \note The pattern may contain the wildcards:
 *        - `*`   Matches zero or more arbitrary characters
 *        - `?` Matches a single character
 *        - `/` Matches punctuation or white-space, as well as the terminating
 *          zero byte.
 *
 *  \note Adapted from https://stackoverflow.com/a/23457543
 */
static int match(const char *pattern, const char *candidate, int p, int c)
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
    return match(pattern, candidate, p+plen, c+clen);
  }
}

/** strmatch() finds the first occurrence of the "pattern" in the "text", and
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
 *
 *  \return A pointer to the matched substring, or NULL if no match was found.
 *
 *  \note  The pattern may contain the following wild-card characters:
 *         - `?` matches a single character
 *         - `*` matches any number of characters (including zero)
 *         - `/` matches any sequence of white-space and/or punctuation (and it
 *         also matches the end of the string). The `/` wild-card is therefore
 *         useful to match complete words.
 *
 *  \note  The `length` parameter is useful, because (due to the wild-cards) the
 *         length of a matched substring may not be the same as the length of
 *         the pattern. Concretely, `*` and `/` may match multiple characters; a
 *         `*` may also match zero characters.
 */
const char *strmatch(const char *pattern,const char *text,int *length)
{
  if (!pattern || !*pattern || !text || !*text)
    return NULL;  /* empty pattern matches nothing, empty text string is never matched ... */

  /* ignore leading "*" and leading white-space on patterns */
  while (*pattern=='*' || isspace(*pattern))
    pattern++;
  if (!*pattern)
    return NULL;

  /* if there are no wild-cards in pattern, we can use strstr() */
  int offset=-1;
  int len=-1;
  if (!strpbrk(pattern,"?*/")) {
    const char *ptr=strstr(text,pattern);
    if (ptr)
      len=strlen(pattern);
    offset=ptr-text;
  } else {
    for (int i=0; text[i]; i++) {
      int r=match(pattern,text,0,i);
      if (r) {
        offset=i;
        len=r-i;
        break;
      }
    }
  }
  if (offset<0)
    return NULL;  /* no match found */
  if (length)
    *length=len;
  return text+offset;
}

