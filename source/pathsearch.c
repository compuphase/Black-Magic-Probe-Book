/*
 * Searching the path for a filename.
 *
 * Copyright 2023 CompuPhase
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
#if defined WIN32 || defined _WIN32
# define STRICT
# define WIN32_LEAN_AND_MEAN
# include <io.h>
# include <malloc.h>
# if defined __MINGW32__ || defined __MINGW64__
#   include "strlcpy.h"
# elif defined _MSC_VER
#   include "strlcpy.h"
#   define access(p,m)       _access((p),(m))
#   define strdup(s)         _strdup(s)
# endif
#elif defined __linux__
# include <unistd.h>
# include <bsd/string.h>
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pathsearch.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#if !defined _MAX_PATH
# define _MAX_PATH 260
#endif

#if defined WIN32 || defined _WIN32
# define SEPARATOR  ";"
# define DIRSEP_CHR '\\'
# define DIRSEP_STR "\\"
#else
# define SEPARATOR  ":"
# define DIRSEP_CHR '/'
# define DIRSEP_STR "/"
#endif

/** pathsearch() locates a file in the path.
 *  \param buffer   [out] Contains the full path of the file on return, if the
 *                  file is found.
 *  \param bufsize  The size in characters of the buffer.
 *  \param filename [in] The base name of the file.
 *  \return true on success, false on failure.
 *  \note If the function fails, parameter "buffer" is not changed.
 *  \note If the file is found, but the "buffer" parameter is too small to
 *        contain the full path, the function returns false (and "buffer" is not
 *        changed).
 */
bool pathsearch(char *buffer, size_t bufsize, const char *filename)
{
  assert(buffer != NULL && bufsize > 0);
  assert(filename != NULL);
  if (strlen(filename) == 0)
    return false;

  char *temp_env = getenv("PATH");
  if (temp_env == NULL)
    return false;
  char *env = strdup(temp_env);

  bool result = false;
  for (char *tok = strtok(env, SEPARATOR); tok != NULL; tok = strtok(NULL, SEPARATOR)) {
    char path[_MAX_PATH];
    strlcpy(path, tok, sizearray(path));
    size_t len = strlen(path);
    assert(len < sizearray(path));
    if (len > 0 && path[len - 1] != DIRSEP_CHR)
      strlcat(path, DIRSEP_STR, sizearray(path));
    strlcat(path, filename, sizearray(path));
    if (access(path, 0) == 0 && strlen(path) < bufsize) {
      strlcpy(buffer, path, bufsize);
      result = true;
      break;
    }
  }

  free(env);
  return result;
}

