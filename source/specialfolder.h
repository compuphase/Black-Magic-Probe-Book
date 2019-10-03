/*
 * Return user folder locations (for local data or configuration). The Linux
 * code in this file is inspired by PlatformFolders.cpp by Poul Sander (see
 * https://github.com/sago007/PlatformFolders).
 *
 * Its is under the MIT license, to encourage reuse by cut-and-paste.
 *
 * Portions copyright (c) 2019 CompuPhase
 * Portions copyright (c) 2015-2016 Poul Sander
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef _SPECIALFOLDER_H
#define _SPECIALFOLDER_H

#if defined __cplusplus
  extern "C" {
#endif

#if defined _WIN32
  #define DIR_SEPARATOR "\\"
#else
  #define DIR_SEPARATOR "/"
#endif

int folder_AppData(char *path, size_t maxlength);
int folder_AppConfig(char *path, size_t maxlength);

#if defined __cplusplus
  }
#endif

#endif /* _SPECIALFOLDER_H */

