/*

    Implementation of POSIX directory browsing functions and types for Win32.

    Author:  Kevlin Henney (kevlin@acm.org, kevlin@curbralan.com)
    History: Created March 1997. Updated June 2003 and July 2012.
             Updated August 2021 to use Windows API (instead of DOS) and add
             file attribute & size, by Thiadmer Riemersma
    Rights:  See end of file.

*/

#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "dirent.h"

#ifdef __cplusplus
extern "C"
{
#endif


struct DIR
{
    HANDLE          handle; /* INVALID_HANDLE_VALUE for failed rewind */
    WIN32_FIND_DATA info;
    struct dirent   result; /* d_name NULL iff first time */
    char            *name;  /* null-terminated char string */
};

DIR *opendir(const char *name)
{
    DIR *dir = NULL;

    if(name && name[0])
    {
        size_t base_length = strlen(name);
        const char *all = /* search pattern must end with suitable wildcard */
            strchr("/\\", name[base_length - 1]) ? "*" : "/*";

        if((dir = (DIR *) malloc(sizeof *dir)) != 0 &&
           (dir->name = (char *) malloc(base_length + strlen(all) + 1)) != 0)
        {
            strcat(strcpy(dir->name, name), all);

            if((dir->handle =
                FindFirstFile(dir->name, &dir->info)) != INVALID_HANDLE_VALUE)
            {
                dir->result.d_name = NULL;
            }
            else /* rollback */
            {
                free(dir->name);
                free(dir);
                dir = NULL;
            }
        }
        else /* rollback */
        {
            free(dir);
            dir   = NULL;
            errno = ENOMEM;
        }
    }
    else
    {
        errno = EINVAL;
    }

    return dir;
}

int closedir(DIR *dir)
{
    int result = -1;

    if(dir)
    {
        if(dir->handle != INVALID_HANDLE_VALUE)
        {
            result = FindClose(dir->handle) ? 0 : -1;   /* closedir() returns 0 on success */
        }

        free(dir->name);
        free(dir);
    }

    if(result == -1) /* map all errors to EBADF */
    {
        errno = EBADF;
    }

    return result;
}

struct dirent *readdir(DIR *dir)
{
    struct dirent *result = NULL;

    if(dir && dir->handle != INVALID_HANDLE_VALUE)
    {
        if(dir->result.d_name == NULL || FindNextFile(dir->handle, &dir->info))
        {
            result         = &dir->result;
            result->d_name = dir->info.cFileName;
            result->d_attr = (short)dir->info.dwFileAttributes & ~0x80;
            result->d_size = dir->info.nFileSizeLow;
        }
    }
    else
    {
        errno = EBADF;
    }

    return result;
}

void rewinddir(DIR *dir)
{
    if(dir && dir->handle != INVALID_HANDLE_VALUE)
    {
        FindClose(dir->handle);
        dir->handle = FindFirstFile(dir->name, &dir->info);
        dir->result.d_name = NULL;
    }
    else
    {
        errno = EBADF;
    }
}

#ifdef __cplusplus
}
#endif

/*

    Copyright Kevlin Henney, 1997, 2003, 2012. All rights reserved.

    Permission to use, copy, modify, and distribute this software and its
    documentation for any purpose is hereby granted without fee, provided
    that this copyright and permissions notice appear in all copies and
    derivatives.

    This software is supplied "as is" without express or implied warranty.

    But that said, if there are any problems please get in touch.

*/

