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
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "specialfolder.h"

#if defined _WIN32

#define WIN32_LEAN_AND_MEAN
#include <tchar.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#if defined __MINGW32__ || defined __MINGW64__
  #include "strlcpy.h"
#endif

#if defined UNICODE || defined _UNICODE
  #define SHGETFOLDERPATH "SHGetFolderPathW"
#else
  #define SHGETFOLDERPATH "SHGetFolderPathA"
#endif

static void _SHFree(void *p)       // free shell item identifier memory
{
  IMalloc *pMalloc;

  SHGetMalloc(&pMalloc);
  if (pMalloc != NULL) {
    pMalloc->lpVtbl->Free(pMalloc, p);
    pMalloc->lpVtbl->Release(pMalloc);
  }
}

typedef HRESULT (WINAPI *fpSHGetFolderPath)(HWND hwndOwner, int nFolder, HANDLE hToken, DWORD dwFlags, LPTSTR pszPath);
typedef HRESULT (WINAPI *fpSHGetSpecialFolderLocation)(HWND hwndOwner, int nFolder, LPITEMIDLIST *ppidl);
#if !defined CSIDL_FLAG_DONT_VERIFY
  #define CSIDL_FLAG_DONT_VERIFY  0x4000
#endif
#if !defined CSIDL_FLAG_CREATE
  #define CSIDL_FLAG_CREATE       0x8000
#endif
#if !defined SHGFP_TYPE_CURRENT
  #define SHGFP_TYPE_CURRENT      0
  #define SHGFP_TYPE_DEFAULT      1
#endif

static BOOL GetShellFolder(int FolderID, LPTSTR pszSelectedDir, size_t MaxPathLength)
{
  /* check platform support */
  BOOL bValid = FALSE;
  fpSHGetFolderPath xSHGetFolderPath = NULL;
  fpSHGetSpecialFolderLocation xSHGetSpecialFolderLocation = NULL;
  HINSTANCE hinstShell;
  TCHAR tmpPath[MAX_PATH];

  assert(pszSelectedDir != NULL);
  assert(MaxPathLength > 0);
  *pszSelectedDir = '\0';

  /* SHELL32.DLL should already have been implicitly loaded, because of the
   * calls to SHGetMalloc() and SHGetPathFromIDList(). The alternative DLL,
   * SHFOLDER.DLL is usually not loaded, though.
   */
  hinstShell = LoadLibrary(_T("shell32.dll"));
  if (hinstShell == NULL)
    hinstShell = LoadLibrary(_T("shfolder.dll"));
  if (hinstShell == NULL)
    return FALSE;

  xSHGetFolderPath = (void*)GetProcAddress(hinstShell, SHGETFOLDERPATH);
  if (xSHGetFolderPath == NULL) {
    /* new function is not present in this DLL (probably SHELL32.DLL), try again
       with SHFOLDER.DLL */
    FreeLibrary(hinstShell);
    hinstShell = LoadLibrary(_T("shfolder.dll"));
    if (hinstShell != NULL)
      xSHGetFolderPath = (void*)GetProcAddress(hinstShell, SHGETFOLDERPATH);
  } /* if */

  if (xSHGetFolderPath == NULL) {
    /* new function is not present in either DLL, try the older function */
    if (hinstShell != NULL)
      xSHGetSpecialFolderLocation = (void*)GetProcAddress(hinstShell, _T("SHGetSpecialFolderLocation"));
    if (xSHGetSpecialFolderLocation == NULL) {
      /* older function is not present in this DLL (possibly SHFOLDER.DLL),
       * try the SHELL32.DLL
       */
      if (hinstShell != NULL)
        FreeLibrary(hinstShell);
      hinstShell = LoadLibrary(_T("shell32.dll"));
      if (hinstShell == NULL)
        return FALSE;
      xSHGetSpecialFolderLocation = (void*)GetProcAddress(hinstShell, _T("SHGetSpecialFolderLocation"));
      if (xSHGetSpecialFolderLocation == NULL) {
        FreeLibrary(hinstShell);
        return FALSE;
      } /* if */
    } /* if */
  } /* if */

  /* now we have obtained either of two functions in either of two DLLs */
  if (xSHGetFolderPath != NULL) {
    HRESULT hr;
    hr = xSHGetFolderPath(NULL, FolderID | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, tmpPath);
    if (!SUCCEEDED(hr) || tmpPath[0] == '\0')
      xSHGetFolderPath(NULL, FolderID | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, tmpPath);
    bValid = (tmpPath[0] != '\0');
  } else {
    LPITEMIDLIST pidl;
    if (xSHGetSpecialFolderLocation(NULL, FolderID, &pidl) == NOERROR) {
      if (SHGetPathFromIDList(pidl, tmpPath))
        bValid = TRUE;
      _SHFree(pidl);
    } /* if */
  } /* if */

  if (hinstShell!=NULL)
    FreeLibrary(hinstShell);
  if (bValid)
    strlcpy(pszSelectedDir, tmpPath, MaxPathLength);
  return bValid;
}

#else /* !_WIN32 */

#include <unistd.h>
#include <bsd/string.h>

static int GetHomeFolder(char *path, size_t maxlength)
{
  int uid = getuid();
  const char *envHome = getenv("HOME");
  assert(path != NULL);
  if (uid != 0 && envHome) {
    /* we only acknowlegde HOME if not root */
    strlcpy(path, envHome, maxlength);
    return 1;
  } else if (uid == 0) {
    strlcpy(path, "/root", maxlength);
    return 1;
  }
  *path = '\0';
  return 0;
}

#if !defined __APPLE__
static int GetDefaultFolder(char *path, size_t maxlength,
                            const char *envName, const char *relativePath)
{
  const char *envPath;

  assert(path != NULL);
  assert(envName != NULL);
  assert(relativePath != NULL);
  envPath = getenv(envName);
  if (envPath && *envPath == '/') {
    strlcpy(path, envPath, maxlength);
    return 1;
  }
  if (GetHomeFolder(path, maxlength)) {
    strlcat(path, "/", maxlength);
    strlcat(path, relativePath, maxlength);
    return 1;
  }
  *path = '\0';
  return 0;
}
#endif /* __APPLE__ */

#endif  /* _WIN32 */


/** folder_AppData() the base name for storing application data.
 */
int folder_AppData(char *path, size_t maxlength)
{
  assert(path != NULL);
  #if defined _WIN32
    return GetShellFolder(CSIDL_APPDATA, path, maxlength);
  #elif defined __APPLE__
    if (GetHomeFolder(path, maxlength))
      strlcat(path, "/Library/Application Support", maxlength);
  #else
    return GetDefaultFolder(path, maxlength, "XDG_DATA_HOME", ".local/share");
  #endif
}

/** folder_AppConfig() the base name for storing configuration files for the
 *  application.
 */
int folder_AppConfig(char *path, size_t maxlength)
{
  assert(path != NULL);
  #if defined _WIN32
    return GetShellFolder(CSIDL_APPDATA, path, maxlength);
  #elif defined __APPLE__
    if (GetHomeFolder(path, maxlength))
      strlcat(path, "/Library/Application Support", maxlength);
  #else
    return GetDefaultFolder(path, maxlength, "XDG_CONFIG_HOME", ".config");
  #endif
}

