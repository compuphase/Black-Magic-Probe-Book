/*
 * Utility functions to create a calltree from a CSV file that BMTrace has
 * generated from function function entry and function exit traces.
 *
 * Build this file with the macro STANDALONE defined on the command line to
 * create a self-contained executable.
 *
 * Copyright 2022-2023 CompuPhase
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svnrev.h"

#if defined __linux__
# include <bsd/string.h>
#elif defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
# include "strlcpy.h"
#endif

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if !defined _MAX_PATH
# define _MAX_PATH 260
#endif

#if !defined sizearray
# define sizearray(a)  (sizeof(a) / sizeof((a)[0]))
#endif

#if defined WIN32 || defined _WIN32
# define IS_OPTION(s)  ((s)[0] == '-' || (s)[0] == '/')
#else
# define IS_OPTION(s)  ((s)[0] == '-')
#endif

typedef struct tagFUNCDEF {
  struct tagFUNCDEF *next;
  char *name;
  int count;
  bool skip;
  struct tagFUNCDEF *caller;
  struct tagFUNCDEF *callees;
} FUNCDEF;

enum {
  TYPE_INVALID,
  TYPE_ENTER,
  TYPE_EXIT,
};

static FUNCDEF calltree = { NULL };
static FUNCDEF *current = NULL;


static const char *skipwhite(const char *text)
{
  assert(text != NULL);
  while (*text != '\0' && *text <= ' ')
    text++;
  return text;
}

static const char *skiptodelim(const char *text, char delimiter)
{
  assert(text != NULL);
  while (*text != '\0' && *text != delimiter)
    text++;
  return text;
}

static int match_function(const char *line, int channel, char *name, size_t namesize,
                          const char *func_enter, const char *func_exit)
{
  assert(func_enter != NULL && strlen(func_enter) > 0);
  assert(func_exit != NULL && strlen(func_exit) > 0);
  size_t func_enter_len = strlen(func_enter);
  size_t func_exit_len = strlen(func_exit);

  assert(line != NULL);
  long seqnr = strtol(line, NULL, 10);
  if (seqnr != channel)
    return TYPE_INVALID;  /* not the channel for channel trace */

  const char *ptr = line;
  /* skip channel number */
  ptr = skiptodelim(ptr, ',');
  if (*ptr == ',')
    ptr++;
  /* skip channel name */
  ptr = skipwhite(ptr);
  if (*ptr == '"')
    ptr = skiptodelim(ptr + 1, '"');
  ptr = skiptodelim(ptr, ',');
  if (*ptr == ',')
    ptr++;
  /* skip timestamp */
  ptr = skiptodelim(ptr, ',');
  if (*ptr == ',')
    ptr++;
  /* check enter/exit code */
  if (*ptr == '"')
    ptr = skipwhite(ptr + 1);
  if (strncmp(ptr, func_enter, func_enter_len) != 0 && strncmp(ptr, func_exit, func_exit_len) != 0)
    return TYPE_INVALID;  /* required keyword not found */
  if (strchr(ptr, ':') == NULL || strchr(ptr, '=') == NULL)
    return TYPE_INVALID;  /* syntax not correct for function trace */

  int result = (strncmp(ptr, func_enter, func_enter_len)== 0) ? TYPE_ENTER : TYPE_EXIT;
  ptr = skiptodelim(ptr, '=');
  assert(ptr != NULL);  /* presence of '=' was already checked above */
  const char *fname = skipwhite(ptr + 1);
  size_t len = ((ptr = strchr(fname, '"')) != NULL) ? ptr - fname : strlen(fname);
  len += 1; /* add size of '\0' terminator */
  if (len > namesize)
    len = namesize;
  assert(name != NULL && namesize > 0);
  strlcpy(name, fname, len);

  return result;
}

static void enter_function(const char *name)
{
  assert(name != NULL);

  /* check whether this function is already among the callees of the current
     function */
  FUNCDEF *localroot = (current != NULL) ? current->callees : &calltree;
  assert(localroot != NULL);
  FUNCDEF *entry;
  for (entry = localroot->next; entry != NULL && strcmp(entry->name, name) != 0; entry = entry->next)
    {}
  if (entry != NULL) {
    entry->count += 1;  /* this function was already called at this level -> just increment count */
    current = entry;
    return;
  }

  /* add entry */
  entry = malloc(sizeof(FUNCDEF));
  if (entry != NULL) {
    memset(entry, 0, sizeof(FUNCDEF));
    entry->callees = malloc(sizeof(FUNCDEF));
    entry->name = strdup(name);
    if (entry->callees != NULL && entry->name != NULL) {
      entry->count = 1;
      entry->caller = current;
      memset(entry->callees, 0, sizeof(FUNCDEF));
      FUNCDEF *pos = (current != NULL) ? current->callees : &calltree;
      while (pos->next != NULL)
        pos = pos->next;
      pos->next = entry;
      current = entry;
    } else {
      if (entry->callees != NULL)
        free((void*)entry->callees);
      if (entry->name != NULL)
        free((void*)entry->name);
      free((void*)entry);
    }
  }
}

static void exit_function(const char *name)
{
  if (current != NULL) {
    assert(current->name != NULL);
    if (strcmp(current->name, name) != 0)
      fprintf(stderr, "Warning: exit function '%s' does not match entry for '%s'.\n", name, current->name);
    current = current->caller;
  } else {
    fprintf(stderr, "Warning: exit function '%s' at call stack level 0.\n", name);
  }
}

static void print_graph(FUNCDEF *root, int level)
{
  for (FUNCDEF *entry = root->next; entry != NULL; entry = entry->next) {
    for (int indent = 0; indent < level; indent++)
      printf("    ");
    assert(entry->name != NULL);
    printf("%s", entry->name);
    if (entry->count > 1)
      printf(" [%dx]", entry->count);
    printf("\n");
    print_graph(entry->callees, level + 1);
  }
}

static FUNCDEF *findnext(FUNCDEF *root, const char *name)
{
  if (root == NULL)
    return NULL;
  for (FUNCDEF *entry = root->next; entry != NULL; entry = entry->next) {
    FUNCDEF *sub = findnext(entry->callees, name);
    assert(sub == NULL || sub->skip);
    if (sub != NULL)
      return sub;
    if (!entry->skip && (name == NULL || strcmp(entry->name, name) == 0)) {
      entry->skip = true;
      return entry;
    }
  }
  return NULL;
}

static void print_graph_reverse(FUNCDEF *root)
{
  FUNCDEF *entry;
  while ((entry = findnext(root, NULL)) != NULL) {
    entry->skip = true; /* don't find it again */
    assert(entry->name != NULL);
    printf("%s:\n", entry->name);
    while (entry != NULL) {
      FUNCDEF *parent = entry->caller;
      int level = 1;
      while (parent != NULL) {
        for (int indent = 0; indent < level; indent++)
          printf("    ");
        printf("%s", parent->name);
        if (parent->count > 1)
          printf(" [%dx]", parent->count);
        printf("\n");
        level += 1;
        parent = parent->caller;
      }
      entry = findnext(root, entry->name);
    }
  }
}

static void delete_graph(FUNCDEF *root)
{
  while (root->next != NULL) {
    FUNCDEF *entry = root->next;
    root->next = entry->next; /* unlink first */
    free((void*)entry->name);
    delete_graph(entry->callees);
    free((void*)entry);
  }
}

static void usage(int status)
{
  printf("\ncalltree - generate a calltree from the output of the function profiling trace\n"
         "           data (in the Common Trace Format).\n\n"
         "Usage: calltree [options] inputfile\n\n"
         "       The input file must be in CSV format, as saved by the bmtrace utility.\n\n"
         "Options:\n"
         "-c value        The channel number that contains the function entry/exit\n"
         "                traces. The default channel is 31.\n"
         "-r, --reverse   Create a reverse tree.\n"
         "--enter=name    The name for the \"__cyg_profile_func_enter\" function in the\n"
         "                TSDL file. The default name is \"enter\".\n"
         "--exit=name     The name for the \"__cyg_profile_func_exit\" function in the TSDL\n"
         "                file. The default name is \"exit\".\n"
         "-v              Show version information.\n");
  exit(status);
}

static void version(int status)
{
  printf("calltree version %s.\n", SVNREV_STR);
  printf("Copyright 2022-2023 CompuPhase\nLicensed under the Apache License version 2.0\n");
  exit(status);
}

static void unknown_option(const char *option)
{
  fprintf(stderr, "Unknown option \"%s\"; use option -h for help.\n", option);
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
  if (argc <= 1)
    usage(EXIT_SUCCESS);

  int channel = 31;
  bool reverse = false;
  char func_enter[64] = "enter";
  char func_exit[64] = "exit";
  char infile[_MAX_PATH] = "";
  /* command line options */
  for (int idx = 1; idx < argc; idx++) {
    if (IS_OPTION(argv[idx])) {
      switch (argv[idx][1]) {
      case '?':
      case 'h':
        usage(EXIT_SUCCESS);
        break;
      case 'c':
        if (isdigit(argv[idx][2])) {
          channel = (int)strtol(argv[idx] + 2, NULL, 10);
        } else if (argv[idx][2] == '=' && isdigit(argv[idx][3])) {
          channel = (int)strtol(argv[idx] + 3, NULL, 10);
        } else if (isdigit(argv[idx + 1][0])) {
          idx += 1;
          channel = (int)strtol(argv[idx], NULL, 10);
        } else {
          unknown_option(argv[idx]);
        }
        break;
      case 'r':
        reverse = true;
        break;
      case 'v':
        version(EXIT_SUCCESS);
        break;
      case '-': /* long options, starting with double hyphen */
        if (strcmp(argv[idx] + 2, "reverse") == 0)
          reverse = true;
        else if (strncmp(argv[idx] + 2, "enter=", 6) == 0 && strlen(argv[idx] + 8) > 0)
          strlcpy(func_enter, argv[idx] + 8, sizearray(func_enter));
        else if (strncmp(argv[idx] + 2, "exit=", 5) == 0 && strlen(argv[idx] + 7) > 0)
          strlcpy(func_exit, argv[idx] + 7, sizearray(func_exit));
        else
          unknown_option(argv[idx]);
        break;
      default:
        unknown_option(argv[idx]);
      }
    } else {
      if (strlen(infile) == 0)
        strlcpy(infile, argv[idx], sizearray(infile));
      else
        unknown_option(argv[idx]);
    }
  }
  if (strlen(infile) == 0) {
    fprintf(stderr, "No input file specified.\n");
    return EXIT_FAILURE;
  }

  FILE *fp = fopen(infile, "rt");
  if (fp == NULL) {
    fprintf(stderr, "File not found: %s\n", infile);
    return EXIT_FAILURE;
  }
  char line[512];
  while (fgets(line, sizearray(line), fp) != NULL) {
    char name[256];
    int type = match_function(line, channel, name, sizearray(name), func_enter, func_exit);
    switch (type) {
    case TYPE_ENTER:
      enter_function(name);
      break;
    case TYPE_EXIT:
      exit_function(name);
      break;
    }
  }
  fclose(fp);

  if (reverse)
    print_graph_reverse(&calltree);
  else
    print_graph(&calltree, 0);
  delete_graph(&calltree);
  return EXIT_SUCCESS;
}

