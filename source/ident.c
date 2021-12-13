/*
 * Basic re-implementation of the "ident" utility of the RCS suite, to extract
 * RCS identification strings from source and binary files. This implementation
 * only supports the keywords "Author", "Date", "Id", and "Revision" (which may
 * be abbreviated to "Rev").
 *
 * Copyright 2021 CompuPhase
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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "ident.h"

#if defined _MSC_VER
  #define stricmp(s1,s2)    _stricmp((s1),(s2))
#elif defined __linux__
  #define stricmp(s1,s2)    strcasecmp((s1),(s2))
#endif

#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif
#if !defined STREQ
#  define STREQ(s1, s2)   (stricmp((s1), (s2)) == 0)
#endif

enum {
  STATE_SCAN,
  STATE_KEY,
  STATE_VALUE,
};

int ident(FILE *fp, int skip, char *key, size_t key_size, char *value, size_t value_size)
{
  int ch, state;
  int key_idx, val_idx;

  assert(fp != NULL);
  assert(skip >= 0);
  assert(key != NULL && key_size > 0);
  assert(value != NULL && value_size > 0);

  rewind(fp);
  state = STATE_SCAN;
  while ((ch = fgetc(fp)) >= 0) {
    switch (state) {
    case STATE_SCAN:
      /* state 0: search for '$' as a start point */
      if (ch == '$') {
        key_idx = 0;
        state = STATE_KEY;
      }
      break;

    case STATE_KEY:
      if (ch == ':') {
        while (key_idx > 0 && key[key_idx - 1] <= ' ')
          key_idx--;          /* strip trailing spaces */
        key[key_idx]= '\0';   /* terminate key */
        val_idx = 0;
        state = (key_idx > 0) ? STATE_VALUE : STATE_SCAN;
      } else if (!isalpha(ch) && ch != ' ') {
        /* accept only alphabetic characters in the key; on invalid character,
           drop back to start */
        state = STATE_SCAN;
      } else if (key_idx >= key_size) {
        /* if length of key exceeded, drop back to start */
        state = STATE_SCAN;
      } else {
        key[key_idx++] = (char)ch;
      }
      break;

    case STATE_VALUE:
      if (ch == '$') {
        /* found a key/value pair */
        while (val_idx > 0 && value[val_idx - 1] <= ' ')
          val_idx--;          /* strip trailing spaces */
        value[val_idx] = '\0';
        if (val_idx > 0) {
          assert(strlen(key) > 0);
          /* both key and value have non-zero length, so appear to be valid,
             filter for standard keywords */
          if (STREQ(key, "Author") || STREQ(key, "Date") || STREQ(key, "Id")
              || STREQ(key, "Rev") || STREQ(key, "Revision") || STREQ(key, "Header")
              || STREQ(key, "URL"))
            if (skip-- == 0)
              return 1;
        }
        state = STATE_SCAN;
      } else if (ch < ' ' || ch > 127) {
        state = STATE_SCAN; /* on invalid character, drop back to start */
      } else if (val_idx >= value_size) {
        /* if length of value exceeded, drop back to start */
        state = STATE_SCAN;
      } else if (val_idx > 0 || ch != ' ') {
        value[val_idx++] = (char)ch;
      }
      break;
    }
  }

  /* on failure to find (another) RCS identification, clear the output parameters */
  assert(key != NULL && value != NULL);
  *key = '\0';
  *value = '\0';
  return 0; /* no (more) RCS identification strings found */
}

#if defined STANDALONE

static void usage(int status)
{
  printf("ident - show RCS identification strings in the file.\n\n"
         "Usage: ident [filename] [...]\n\n");
  exit(status);
}

int main(int argc, char *argv[])
{
  int i;

  if (argc <= 1)
    usage(EXIT_FAILURE);

  for (i = 1; i < argc; i++) {
    if (STREQ(argv[i], "-?") || STREQ(argv[i], "-h") || STREQ(argv[i], "--help")) {
      usage(EXIT_SUCCESS);
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "Invalid option \"%s\", use --help to see the syntax\n\n", argv[i]);
      usage(EXIT_FAILURE);
    } else {
      FILE *fpElf = fopen(argv[i], "rb");
      if (fpElf != NULL) {
        char key[32], value[128];
        int count;
        printf("%s\n", argv[i]);
        for (count = 0; ident(fpElf, count, key, sizearray(key), value, sizearray(value)); count++)
          printf("\t%s: %s\n", key, value);
        fclose(fpElf);
      } else {
        fprintf(stderr, "Failed to open \"%s\", error %d\n", argv[i], errno);
        return EXIT_FAILURE;
      }
    }
  }

  return EXIT_SUCCESS;
}

#endif /* STANDALONE */

