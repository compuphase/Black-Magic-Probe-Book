/* noc_file_dialog library
 *
 * Copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "noc_file_dialog.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#ifdef NOC_FILE_DIALOG_GTK

#include <gtk/gtk.h>

int noc_file_dialog_open(char *path, size_t pathsize,   /* output */
                         int flags,
                         const char *filters,
                         const char *default_path,
                         const char *default_name,
                         const char *caption,
                         const void *parent)
{
    GtkWidget *dialog;
    GtkFileFilter *filter;
    GtkFileChooser *chooser;
    GtkFileChooserAction action;
    gint res;

    action = flags & NOC_FILE_DIALOG_SAVE ? GTK_FILE_CHOOSER_ACTION_SAVE :
                                            GTK_FILE_CHOOSER_ACTION_OPEN;
    if (flags & NOC_FILE_DIALOG_DIR)
        action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;

    if (caption == NULL)
        caption = (flags & NOC_FILE_DIALOG_SAVE) ? "Save File" : "Open File";

    gtk_init_check(NULL, NULL);
    dialog = gtk_file_chooser_dialog_new(
            caption,
            NULL,
            action,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Open", GTK_RESPONSE_ACCEPT,
            NULL );
    chooser = GTK_FILE_CHOOSER(dialog);
    if (flags & NOC_FILE_DIALOG_OVERWRITE_CONFIRMATION)
        gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);

    if (default_path)
        gtk_file_chooser_set_filename(chooser, default_path);
    if (default_name && (flags & NOC_FILE_DIALOG_SAVE))
        gtk_file_chooser_set_current_name(chooser, default_name);

    while (filters && *filters) {
        filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, filters);
        filters += strlen(filters) + 1;
        gtk_file_filter_add_pattern(filter, filters);
        gtk_file_chooser_add_filter(chooser, filter);
        filters += strlen(filters) + 1;
    }

    res = gtk_dialog_run(GTK_DIALOG(dialog));

    assert(path != NULL && pathsize > 0);
    if (res == GTK_RESPONSE_ACCEPT) {
        gchar *ptr = gtk_file_chooser_get_filename(chooser);
        if (ptr != NULL) {
            strncpy(path, ptr, pathsize);
            path[pathsize - 1] = '\0';
            g_free(ptr);
        } else {
            res = GTK_RESPONSE_CANCEL;
        }
    }
    gtk_widget_destroy(dialog);
    while (gtk_events_pending())
        gtk_main_iteration();
    return (res == GTK_RESPONSE_ACCEPT);
}

#endif

#ifdef NOC_FILE_DIALOG_WIN32

#include <windows.h>
#include <commdlg.h>

#if defined _MSC_VER
# define strdup(s)       _strdup(s)
#endif

int noc_file_dialog_open(char *path, size_t pathsize,   /* output */
                         int flags,
                         const char *filters,
                         const char *default_path,
                         const char *default_name,
                         const char *caption,
                         const void *parent)
{
    OPENFILENAME ofn;       // common dialog box structure
    char szFile[260] = "";  // buffer for file name
    int ret;

    if (default_name != NULL && strlen(default_name) < sizeof szFile)
        strcpy(szFile, default_name);

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filters;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = default_path;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (flags & NOC_FILE_DIALOG_OVERWRITE_CONFIRMATION)
        ofn.Flags |= OFN_OVERWRITEPROMPT;
    ofn.lpstrTitle = caption;
    if (parent != NULL)
        ofn.hwndOwner = *(HWND*)parent;

    if (flags & NOC_FILE_DIALOG_SAVE)
        ret = GetSaveFileName(&ofn);
    else
        ret = GetOpenFileName(&ofn);

    assert(path != NULL && pathsize > 0);
    if (ret) {
        strncpy(path, szFile, pathsize);
        path[pathsize - 1] = '\0';
    }

    return ret;
}

#endif

#ifdef NOC_FILE_DIALOG_OSX

#include <AppKit/AppKit.h>

int noc_file_dialog_open(char *path, size_t pathsize,   /* output */
                         int flags,
                         const char *filters,
                         const char *default_path,
                         const char *default_name,
                         const char *caption,
                         const void *parent)
{
    NSURL *url;
    const char *utf8_path;
    NSSavePanel *panel;
    NSOpenPanel *open_panel;
    NSMutableArray *types_array;
    NSURL *default_url;
    int result = 0;
    // XXX: I don't know about memory management with cococa, need to check
    // if I leak memory here.
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    if (flags & NOC_FILE_DIALOG_SAVE) {
        panel = [NSSavePanel savePanel];
    } else {
        panel = open_panel = [NSOpenPanel openPanel];
    }

    if (flags & NOC_FILE_DIALOG_DIR) {
        [open_panel setCanChooseDirectories:YES];
        [open_panel setCanChooseFiles:NO];
    }

    if (default_path) {
        default_url = [NSURL fileURLWithPath:
            [NSString stringWithUTF8String:default_path]];
        [panel setDirectoryURL:default_url];
        [panel setNameFieldStringValue:default_url.lastPathComponent];
    }

    if (filters) {
        types_array = [NSMutableArray array];
        while (*filters) {
            filters += strlen(filters) + 1; // skip the name
            assert(strncmp(filters, "*.", 2) == 0);
            filters += 2; // Skip the "*."
            [types_array addObject:[NSString stringWithUTF8String: filters]];
            filters += strlen(filters) + 1;
        }
        [panel setAllowedFileTypes:types_array];
    }

    assert(path != NULL && pathsize > 0);
    path[0] = '\0';
    if ( [panel runModal] == NSModalResponseOK ) {
        url = [panel URL];
        utf8_path = [[url path] UTF8String];
        strncpy(path, utf8_path, pathsize);
        path[pathsize - 1] = '\0';
        result = 1;
    }

    [pool release];
    return result;
}
#endif

