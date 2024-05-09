/*
The MIT License (MIT)

Copyright (c) 2016 Serge Zaitsev
Portions copyright (c) 2023-2024 Thiadmer Riemersma, CompuPhase

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#ifndef _TCL_H
#define _TCL_H

#define TCL_DISABLE_PUTS
#define TCL_DISABLE_FILEIO


#include <stdbool.h>

typedef long long tcl_int;

struct tcl_value;
struct tcl;
struct tcl_value;
struct tcl {
  struct tcl_env *env;
  struct tcl_cmd *cmds;
  struct tcl_value *result;
  struct tcl_value *errinfo;
  int errcode;
  const void *user;
};


/* =========================================================================
    High level interface
   ========================================================================= */

/** tcl_init() initializes the interpreter context.
 *
 *  \param tcl      The interpreter context.
 */
void tcl_init(struct tcl *tcl, const void *user);

/** tcl_destroy() cleans up the interpreter context, frees all memory.
 *
 *  \param tcl      The interpreter context.
 */
void tcl_destroy(struct tcl *tcl);

/** tcl_eval() runs a script stored in a memory buffer.
 *
 *  \param tcl      The interpreter context.
 *  \param string   The buffer with the script (or part of a script).
 *  \param length   The length of the buffer.
 *
 *  \return 0 on success, 1 on error; other codes are used internally.
 *
 *  \note On completion (of a successful run), the output of the script is
 *        stored in the "result" field of the "tcl" context. You can read this
 *        value with tcl_return().
 */
int tcl_eval(struct tcl *tcl, const char *string, size_t length);

/** tcl_return() returns the result of the script execution (the "return" value
 *  of the script). This data is only valid if tcl_eval() returned success.
 *
 *  \param tcl      The interpreter context.
 *
 *  \note The return value is a pointer to the value in the context; it is not a
 *        copy (and should not be freed). To clean-up the entire context, use
 *        tcl_destroy().
 */
struct tcl_value *tcl_return(struct tcl *tcl);

/** tcl_errorinfo() returns the error code/message and the (approximate) line
 *  number of the error. The error information is cleared after this call.
 *
 *  \param tcl      The interpreter context.
 *  \param line     [out] The (approximate) line number (1-based). This
 *                  parameter may be set to NULL.
 *
 *  \return A pointer to a message describing the error code. When no error was
 *          registered, the function returns an empty string.
 */
const char *tcl_errorinfo(struct tcl *tcl, int *line);


/* =========================================================================
    Values & lists
   ========================================================================= */

/** tcl_isnumber() returns whether the value of the parameter is a valid integer
 *  number. Note that integers can also be accessed as strings in Tcl.
 *
 *  \param v        The value.
 *
 *  \return The detected value.
 */
bool tcl_isnumber(const struct tcl_value *value);

/** tcl_data() returns a pointer to the start of the contents of a value.
 *
 *  \param value    The value.
 *
 *  \return A pointer to the buffer.
 */
const char *tcl_data(const struct tcl_value *value);

/** tcl_length() returns the length of the contents of the value in characters.
 *
 *  \param value    The value.
 *
 *  \return The number of characters in the buffer of the value.
 *
 *  \note This function does _not_ check for escaped characters.
 */
size_t tcl_length(const struct tcl_value *value);

/** tcl_number() returns the value of a variable after parsing it as an integer
 *  value. The function supports decimal, octal and dexadecimal notation.
 *
 *  \param value    The value.
 *
 *  \return The numeric value of the parameter, or 0 on error.
 */
tcl_int tcl_number(const struct tcl_value *value);

/** tcl_value() creates a value from a C string or data block.
 *
 *  \param data     The contents to store in the value. A copy is made of this
 *                  buffer.
 *  \param len      The length of the data. If this parameter is set to -1, the
 *                  `data` buffer is assumed to be a zero-terminated string.
 *
 *  \return A pointer to the created value.
 *
 *  \note The value should be deleted with tcl_free().
 */
struct tcl_value *tcl_value(const char *data, long len);

/** tcl_free() deallocates a value or a list.
 *
 *  \param v          The value.
 *
 *  \return This function always returns NULL.
 *
 *  \note Lists are implemented as values (strings), so this function
 *        deallocates both.
 */
struct tcl_value *tcl_free(struct tcl_value *v);

/** tcl_list_new() creates an empty list. Use this function to start a new list
 *  (then append items to it). The list must be freed with tcl_free().
 */
struct tcl_value *tcl_list_new(void);

/** tcl_list_length() returns the number of elements in a list.
 *
 *  \param list       The list.
 *
 *  \return The number of elements in the list.
 */
int tcl_list_length(const struct tcl_value *list);

/** tcl_list_item() retrieves an element from the list.
 *
 *  \param list       The list.
 *  \param index      The zero-based index of the element to retrieve.
 *
 *  \return The selected element, or NULL if parameter "index" is out of range.
 *
 *  \note The returned element is a copy, which must be freed with tcl_free().
 */
struct tcl_value *tcl_list_item(const struct tcl_value *list, int index);

/** tcl_list_append() appends an item to the list, and frees the item.
 *
 *  \param list       The original list.
 *  \param tail       The item to append.
 *
 *  \return true on success, false on failure.
 *
 *  \note Both the original data in the `list` parameter and the `tail` item
 *        that was appended, are deallocated (freed).
 */
bool tcl_list_append(struct tcl_value *list, struct tcl_value *tail);

/** tcl_list_find() returns the index of the word in the list, or -1 if it is
 *  not found.
 *
 *  \note This is a helper function that is typically used to look for switches
 *        of a command.
 */
int tcl_list_find(const struct tcl_value *list, const char *word);


/* =========================================================================
    Variables
   ========================================================================= */

/** tcl_var() sets or reads a variable.
 *
 *  \param tcl      The interpreter context.
 *  \param name     The name of the variable.
 *  \param value    The value to set the variable to, or NULL to read the value
 *                  of the variable. See notes below.
 *
 *  \return A pointer to the value in the variable. It may return NULL on
 *          failure, see also the notes below.
 *
 *  \note When reading a variable that does not exist, the function sets an
 *        error and returns NULL.
 *
 *  \note The returned pointer points to the value in the tcl_var structure; it
 *        is not a copy (and must not be freed or changed).
 *
 *  \note The "value" parameter (if not NULL) is owned by the variable after
 *        this function completes. Thus, the parameter should not be freed.
 */
struct tcl_value *tcl_var(struct tcl *tcl, const char *name, struct tcl_value *value);


/* =========================================================================
    User commands
   ========================================================================= */

typedef int (*tcl_cmd_fn_t)(struct tcl *tcl, const struct tcl_value *args, const struct tcl_value *user);

/** tcl_register() registers a C function to the ParTcl command set.
 *
 *  \param tcl      The interpreter context.
 *  \param name     The name of the command.
 *  \param fn       The function pointer.
 *  \param subcmds  How many subcommands the command has; zero for none.
 *  \param minargs  The minimum number of parameters of the command. This value
 *                  excludes the number of subcommands.
 *  \param maxargs  The maximum number of parameters of the command, which
 *                  includes the command name itself. Set this to -1 for a
 *                  variable argument list. This value does typically *not*
 *                  include switches, see the notes below.
 *  \param user     A user value or list. It is typically used to pass a list of
 *                  switches that a command supports. It may also be used for
 *                  other purposes, or it be set to NULL. See the notes.
 *
 *  \return A pointer to the command structure that was just added.
 *
 *  \note If the `user` parameter in tcl_register() contains a list of switches,
 *        and if the call to the command has one or more switches, then these
 *        switches are removed from the argument list, and added to a new list,
 *        which is then passed as the third argument (`user`) in the C functon.
 *        That is, the C function has all standard arguments in the `args`
 *        parameter, and all switches in the `user` parameter.
 *
 *        All switches must precede normal arguments (but come behind any
 *        subcommand). When an argument is not recognized as a switch, it is
 *        assumed to be a normal argument. All arguments behind it, are then
 *        also assumed to be normal arguments.
 *
 *  \note If the `user` parameter does *not* contain a list of switches, it is
 *        passed to the C function unmodified (in the `user` parameter of the C
 *        function).
 */
struct tcl_cmd *tcl_register(struct tcl *tcl, const char *name, tcl_cmd_fn_t fn,
                             short subcmds, short minargs, short maxargs,
                             struct tcl_value *user);

/** tcl_result() sets the result of a C function into the ParTcl environment.
 *
 *  \param tcl      The interpreter context.
 *  \param flow     Should be set to 1 if an error occurred, or 0 on success
 *                  (other values for "flow" are used internally).
 *  \param result   The result (or "return value") of the C function. See notes
 *                  below.
 *
 *  \return This function returns the "flow" parameter. For the C interface, the
 *          return value can be ignored.
 *
 *  \note The "result" parameter is is owned by the interpreter context when
 *        this function completes. Thus, the parameter should not be freed.
 */
int tcl_result(struct tcl *tcl, int flow, struct tcl_value *result);


/* =========================================================================
    Internals
   ========================================================================= */

/** tcl_cur_scope() returns the current scope level. It is zero at the global
 *  level, and is incremented each time that a new local environment for a user
 *  procedure is allocated.
 *
 *  \param tcl      The interpreter context.
 *
 *  \return The active scope.
 */
int tcl_cur_scope(struct tcl *tcl);

/** tcl_append() creates a new value that is the concatenation of the two
 *  parameters, and deletes the input parameters.
 *
 *  \param value    The value to modify.
 *  \param tail     The data to append to parameter `value`.
 *
 *  \return true on success, false on failure (i.e. memory allocation failure).
 *
 *  \note The `value` parameter is modified, meaning that its `data` block is
 *        re-allocated. Any pointer held to the data, is therefore invalid after
 *        the call to tcl_append().
 *  \note The `tail` parameter is deleted by this function.
 */
bool tcl_append(struct tcl_value *value, struct tcl_value *tail);


/* =========================================================================
    Dynamic memory
   ========================================================================= */

#define _malloc(n)  malloc(n)
#define _free(p)    free(p)

#endif /* _TCL_H */
