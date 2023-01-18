/* Implementation of a simple semihosting interface (output only) for ARM Cortex
 * micro-controllers. This implementation is based on CMSIS, and restricted to
 * the semihosting calls supported by the Black Magic Probe. It is easily
 * adapted to libopencm3 or other micro-controller support libraries. Likewise,
 * it is easily extended with the few semihosting calls that the Black Magic
 * Probe does not support.
 *
 * Copyright 2020-2023 CompuPhase
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#ifndef __SEMIHOSTING_H
#define __SEMIHOSTING_H

#define SYS_OPEN            0x01
#define SYS_CLOSE           0x02
#define SYS_WRITEC          0x03
#define SYS_WRITE0          0x04
#define SYS_WRITE           0x05
#define SYS_READ            0x06
#define SYS_READC           0x07
#define SYS_ISERROR         0x08
#define SYS_ISTTY           0x09
#define SYS_SEEK            0x0A
#define SYS_FLEN            0x0C
#define SYS_TMPNAM          0x0D
#define SYS_REMOVE          0x0E
#define SYS_RENAME          0x0F
#define SYS_CLOCK           0x10
#define SYS_TIME            0x11
#define SYS_SYSTEM          0x12
#define SYS_ERRNO           0x13
#define SYS_GET_CMDLINE     0x15
#define SYS_HEAPINFO        0x16
#define SYS_EXIT            0x18
#define SYS_EXIT_EXTENDED   0x20

#define STDOUT  1
#define STDERR  2

struct heapinfo {
  void *heap_base;
  void *heap_limit;
  void *stack_base;
  void *stack_limit;
};

#include <stddef.h>

/** sys_open() opens a file on the host. The mode is one of:
 *    - "r", "rb", "r+", "r+b"
 *    - "w", "wb", "w+", "w+b"
 *    - "a", "ab", "a+", "a+b"
 *
 *  \return A file handle.
 */
int sys_open(const char *path, const char *mode);

/** sys_close() closes a file.
 *
 *  \return 0 on success, -1 on failure.
 */
int sys_close(int fd);

/** sys_writec() writes a single character to the debugger console.
 */
void sys_writec(char c);

/** sys_write0() sends a zero-terminated string to the debugger console. The
 *  string is transmitted as is, no newline is appended.
 */
void sys_write0(const char *text);

/** sys_write() writes a data buffer to a file. The file handle can be the
 *  pre-defined handles 1 or 2 (for stdio and stderr respectively), or a
 *  handle previously opened with sys_open().
 *
 *  \return The number of bytes *not* written. Hence, on success, the function
 *          returns 0.
 */
int sys_write(int fd, const unsigned char *buffer, size_t size);

/** sys_read() reads data from a file opened on the host.
 *
 *  \return The number of bytes *not* read. More specifically, the function
 *          requests to read "size" bytes. When it successfully does so, the
 *          function returns 0. When it reads less than "size", it returns
 *          the number of bytes that it comes short of "size"
 */
int sys_read(int fd, char *buffer, size_t size);

/** sys_readc() reads a character from the console (stdin).
 *
 *  \return The character read.
 */
int sys_readc(void);

/** sys_iserror() returns whether the code in the parameter is an error code
 *  (or whether it is a normal return code).
 */
int sys_iserror(uint32_t code);

/** sys_istty() returns whether the file handle is a TTY device. GDB defines
 *  only stdin, stdout & stderr as TTY devices.
 */
int sys_istty(int fd);

/** sys_seek() sets the read or write position in a file. The position is
 *  relative to the beginning of the file.
 *
 *  \return 0 on success, -1 or error.
 */
int sys_seek(int fd, size_t offset);

/** sys_flen() returns the length of a file in bytes (returns -1 on error).
 */
int sys_flen(int fd);

/** sys_tmpnam() returns a name for a temporary file. The "id" parameter must
 *  be between 0 and 255. It is up to the implementation whether the "id"
 *  becomes part of the filename (or whether it is completely ignored).
 *
 *  \return 0 on success, -1 on failure.
 */
int sys_tmpnam(int id, char *buffer, size_t size);

/** sys_remove() deletes a file on the host.
 *
 *  \return 0 on success, an host-specific error code (errno) on failure.
 */
int sys_remove(const char *path);

/** sys_rename() renames a file on the host.
 *
 *  \return 0 on success, an host-specific error code (errno) on failure.
 */
int sys_rename(const char *from, const char *to);

/** sys_clock() returns execution time in hundredths of a second (centisecond).
 *  Note that with this call, the target asks the debugger how long it has been
 *  running.
 *
 *  \return The number of centiseconds passed, or -1 in case of an error.
 */
int sys_clock(void);

/** sys_time() returns the current time ("real world time") in the form of the
 *  number of seconds since the Unix Epoch.
 *
 *  \return The number of seconds since 00:00:00 January 1, 1970.
 */
int sys_time(void);

/** sys_system() executes the command in a command shell on the host.
 *
 *  \return The exit code of the command.
 */
int sys_system(const char *command);

/** sys_errno() returns the error code of the previous command that sets it:
 *  sys_open(), sys_close(), sys_read(), sys_write(), sys_seek()m sys_remove(),
 *  sys_rename().
 *
 *  \return The "errno" value.
 */
int sys_errno(void);

/** sys_get_cmdline() returns the parameters passed to the target on a "start"
 *  or "run" command.
 */
void sys_get_cmdline(char *buffer, size_t size);

/** sys_heapinfo() retrieves the top & bottom addresses of the stack and the
 *  heap.
 */
void sys_heapinfo(struct heapinfo *info);

/** sys_exit() signals the host that the target has dropping into an exception
 *  trap (where areaching the end of the application, is also considered an
 *  exception).
 */
void sys_exit(int trap);

/** sys_exit_extended() signals the host that the target has dropping into an
 *  exception trap (where areaching the end of the application, is also
 *  considered an exception).
 */
void sys_exit_extended(int trap, int subcode);

#endif /* __SEMIHOSTING_H */

