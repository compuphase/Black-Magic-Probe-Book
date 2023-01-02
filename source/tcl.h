#ifndef _TCL_H
#define _TCL_H

#define TCL_DISABLE_PUTS


#include <stdbool.h>

typedef long long tcl_int;

struct tcl_value;
struct tcl;
struct tcl_value;
struct tcl {
  struct tcl_env *env;
  struct tcl_cmd *cmds;
  struct tcl_value *result;
};



/* =========================================================================
    High level interface
   ========================================================================= */

/** tcl_init() initializes the interpreter context.
 *
 *  \param tcl      The interpreter context.
 */
void tcl_init(struct tcl *tcl);

/** tcl_destroy() cleans up the interpreter context, frees all memory.
 *  \param tcl      The interpreter context.
 */
void tcl_destroy(struct tcl *tcl);

/** tcl_eval() runs a script stored in a memory buffer.
 *
 *  \param tcl      The interpreter context.
 *  \param string   The buffer with the script (or part of a script).
 *  \param length   The length of the buffer.
 *
 *  \return 0 on error, 1 on success; other non-zero codes are used internally
 *          (and may be assumed "success").
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
 */
struct tcl_value *tcl_return(struct tcl *tcl);

/** tcl_errorpos() returns the error code/message and the (approximate) line
 *  number of the error. The error information is cleared after this call.
 *
 *  \param tcl      The interpreter context.
 *  \param code     [out] The error code. This parameter may be set to NULL.
 *  \param line     [out] The line number (1-based). This parameter may be set
 *                  to NULL.
 *  \param symbol   [out] May contain extra information on the error (such as
 *                  the name of a proc or variable). This parameter may be set
 *                  to NULL.
 *  \param symsize  The size of the buffer for "symbol". Should be set to 0 if
 *                  "symbol" is NULL.
 *
 *  \return A pointer to a message describing the error code. It returns NULL if
 *          no error information is available.
 */
const char *tcl_errorinfo(struct tcl *tcl, int *code, int *line, char *symbol, size_t symsize);
enum {
  TCLERR_GENERAL,     /**< unspecified error */
  TCLERR_MEMORY,      /**< memory allocation error */
  TCLERR_SYNTAX,      /**< general syntax error */
  TCLERR_BRACES,      /**< unbalanced curly braces */
  TCLERR_EXPR,        /**< error in expression */
  TCLERR_CMDUNKNOWN,  /**< unknown command (mismatch in name or arity) */
  TCLERR_VARUNKNOWN,  /**< unknown variable name */
  TCLERR_VARNAME,     /**< invalid variable name (e.g. too long) */
  TCLERR_PARAM,       /**< incorrect (or missing) parameter to a command */
  TCLERR_SCOPE,       /**< scope error (e.g. command is allowed in local scope only) */
};


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
 *  \param data     The contents to store in the value.
 *  \param len      The length of the data.
 *
 *  \return A pointer to the created value.
 *
 *  \note The value should be deleted with tcl_free().
 */
struct tcl_value *tcl_value(const char *data, size_t len);

/** tcl_free() deallocates a value or a list.
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
 *  \param list       The list.
 *
 *  \return The number of elements in the list.
 */
int tcl_list_length(struct tcl_value *list);

/** tcl_list_item() retrieves an element from the list.
 *  \param list       The list.
 *  \param index      The zero-based index of the element to retrieve.
 *
 *  \return The selected element, or NULL if parameter "index" is out of range.
 *
 *  \note The returned element is a copy, which must be freed with tcl_free().
 */
struct tcl_value *tcl_list_item(struct tcl_value *list, int index);

/** tcl_list_append() appends an item to the list, and frees the item.
 *  \param list       The original list.
 *  \param tail       The item to append.
 *
 *  \return true on success, false on failure.
 *
 *  \note Both the original list and the item that was appended, are
 *        deallocated (freed).
 */
bool tcl_list_append(struct tcl_value *list, struct tcl_value *tail);


/* =========================================================================
    Variables
   ========================================================================= */

/** tcl_var() sets or reads a variable
 *  \param tcl      The interpreter context.
 *  \param name     The name of the variable.
 *  \param value    The value to set the variable to, or NULL to read the value
 *                  of the variable. See notes below.
 *
 *  \return A pointer to the value in the variable. See notes below.
 *
 *  \note When reading a variable that does not exist, an new variable is
 *        created, with empty contents.
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

typedef int (*tcl_cmd_fn_t)(struct tcl *tcl, struct tcl_value *args, void *user);

/** tcl_register() registers a C function to the ParTcl command set.
 *  \param tcl      The interpreter context.
 *  \param name     The name of the command.
 *  \param fn       The function pointer.
 *  \param minargs  The number of parameters of the command, which includes
 *                  the command name itself. Set this to zero for a variable
 *                  argument list.
 *  \param maxargs  The number of parameters of the command, which includes
 *                  the command name itself. Set this to zero for a variable
 *                  argument list.
 *  \param user     A user value (which is passed to the C function).
 *
 *  \return A pointer to the command structure that was just added.
 */
struct tcl_cmd *tcl_register(struct tcl *tcl, const char *name, tcl_cmd_fn_t fn, unsigned short minargs, unsigned short maxargs, void *user);

/** tcl_result() sets the result of a C function into the ParTcl environment.
 *  \param tcl      The interpreter context.
 *  \param flow     Should be set to 0 if an error occurred, or 1 on success
 *                  (other values for "flow" are used internally).
 *  \param result   The result (or "return value") of the C function. See notes
 *                  below.
 *
 *  \return This function returs the "flow" parameter. For the C interface, the
 *          return value can be ignored.
 *
 *  \note The "result" parameter is is owned by the interpreter context when
 *        this function completes. Thus, the parameter should not be freed.
 */
int tcl_result(struct tcl *tcl, int flow, struct tcl_value *result);


/* =========================================================================
    Internals
   ========================================================================= */

/** tcl_append() creates a new value that is the concatenation of the two
 *  parameters, and deletes the input parameters.
 */
bool tcl_append(struct tcl_value *value, struct tcl_value *tail);

#endif /* _TCL_H */
