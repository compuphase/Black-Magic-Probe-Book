/* source https://gist.github.com/Fonger/98cc95ac39fbe1a7e4d9
   modified for more robustness */

#include <stdlib.h>
#include <string.h>
#include "strlcpy.h"

/*
 * '_cups_strlcat()' - Safely concatenate two strings.
 */

size_t                      /* O - Length of string (excluding the terminating zero) */
strlcat(char       *dst,    /* O - Destination string */
        const char *src,    /* I - Source string */
        size_t     size)    /* I - Size of destination string buffer */
{
  size_t    srclen;         /* Length of source string */
  size_t    dstlen;         /* Length of destination string */

  if (size == 0)
    return 0;

  /* Figure out how much room is left... */
  dstlen = strlen(dst);
  if (size <= dstlen + 1)
    return (dstlen);        /* No room, return immediately... */
  size -= dstlen + 1;

  /* Figure out how much room is needed... */
  srclen = strlen(src);

  /* Copy the appropriate amount... */
  if (srclen > size)
    srclen = size;

  memcpy(dst + dstlen, src, srclen);
  dst[dstlen + srclen] = '\0';

  return (dstlen + srclen);
}


/*
 * '_cups_strlcpy()' - Safely copy two strings.
 */

size_t                      /* O - Length of string */
strlcpy(char       *dst,    /* O - Destination string */
        const char *src,    /* I - Source string */
        size_t      size)   /* I - Size of destination string buffer */
{
  size_t    srclen;         /* Length of source string */


  /* Figure out how much room is needed... */
  if (size == 0)
    return 0;
  size --;

  srclen = strlen(src);

  /* Copy the appropriate amount... */
  if (srclen > size)
    srclen = size;

  memcpy(dst, src, srclen);
  dst[srclen] = '\0';

  return (srclen);
}

