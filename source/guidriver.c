/*
 * Helper functions for the back-end driver for the Nuklear GUI. Currently, GDI+
 * (for Windows) and GLFW with OpenGL (for Linux) are supported.
 *
 * Copyright 2019 CompuPhase
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
#if defined _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#elif defined __linux__
  #include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include "guidriver.h"

#if defined _WIN32
  #include "nuklear_gdip.h"
#elif defined __linux__ || defined __unix__
  #include "findfont.h"
  #include "lodepng.h"
  #include "nuklear_glfw_gl2.h"
#endif


#ifndef NK_ASSERT
  #include <assert.h>
  #define NK_ASSERT(expr) assert(expr)
#endif

#if defined _WIN32

static int fontType = 0;
static GdipFont *fontStd = NULL;
static GdipFont *fontMono = NULL;
static HWND hwndApp = NULL;

static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch (msg) {
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  if (nk_gdip_handle_event(wnd, msg, wparam, lparam))
    return 0;
  return DefWindowProcW(wnd, msg, wparam, lparam);
}

/** guidriver_init() creates the application window.
 *
 *  \param width      The width of the client area.
 *  \param height     The height of the client area.
 *  \param flags      A combination of options.
 *  \param fontsize   The size of the main font, in pixels.
 *
 *  \note In Microsoft Windows, the application icon (in the frame window) is
 *        set to the icon with the name "appicon" the application's resources.
 *        In Linux, the icon must be a PNG image that is converted to a C array
 *        with 'xd' or 'xxd'; the array must be called appicon_data, and the
 *        size variable must be appicon_datasize.
 */
struct nk_context* guidriver_init(const char *caption, int width, int height,
                                  int flags, int fontsize)
{
  struct nk_context *ctx;
  WNDCLASSW wc;
  DWORD style, exstyle;
  RECT rect;
  RECT rcDesktop;
  wchar_t wcapt[128];
  int i, j, len;

  SetRect(&rect, 0, 0, width, height);
  if (flags & GUIDRV_RESIZEABLE) {
    style = WS_OVERLAPPEDWINDOW;
    exstyle = 0;
  } else {
    style = WS_POPUPWINDOW | WS_CAPTION;
    exstyle = WS_EX_APPWINDOW;
  }
  memset(&wc, 0, sizeof(wc));
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(0);
  wc.hIcon = LoadIcon(wc.hInstance, "appicon");;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = GetStockObject(DKGRAY_BRUSH);
  wc.lpszClassName = L"NuklearWindowClass";
  RegisterClassW(&wc);

  /* convert string to Unicode */
  len = strlen(caption);
  for (i = j = 0; i < len; ) {
    char leader = caption[i];
    int tch;
    if ((leader & 0x80) == 0) {
      tch = caption[i];
      i += 1;
    } else if ((leader & 0xE0) == 0xC0) {
      tch = (caption[i] & 0x1F) << 6;
      tch |= (caption[i+1] & 0x3F);
      i += 2;
    } else if ((leader & 0xF0) == 0xE0) {
      tch = (caption[i] & 0xF) << 12;
      tch |= (caption[i+1] & 0x3F) << 6;
      tch |= (caption[i+2] & 0x3F);
      i += 3;
    } else if ((leader & 0xF8) == 0xF0) {
      tch = (caption[i] & 0x7) << 18;
      tch |= (caption[i+1] & 0x3F) << 12;
      tch |= (caption[i+2] & 0x3F) << 6;
      tch |= (caption[i+3] & 0x3F);
      i += 4;
    } else if ((leader & 0xFC) == 0xF8) {
      tch = (caption[i] & 0x3) << 24;
      tch |= (caption[i] & 0x3F) << 18;
      tch |= (caption[i] & 0x3F) << 12;
      tch |= (caption[i] & 0x3F) << 6;
      tch |= (caption[i] & 0x3F);
      i += 5;
    } else if ((leader & 0xFE) == 0xFC) {
      tch = (caption[i] & 0x1) << 30;
      tch |= (caption[i] & 0x3F) << 24;
      tch |= (caption[i] & 0x3F) << 18;
      tch |= (caption[i] & 0x3F) << 12;
      tch |= (caption[i] & 0x3F) << 6;
      tch |= (caption[i] & 0x3F);
      i += 6;
    } else {
      assert(0);
      tch = 0;  /* to avoid a compiler warning about a potentionally uninitialized variable */
    }
    wcapt[j++] = (wchar_t)tch;
  }
  wcapt[j] = 0;

  /* get size of primary monitor, to center the utility on in */
  GetWindowRect(GetDesktopWindow(), &rcDesktop);
  //??? handle GUIDRV_CENTER flag

  AdjustWindowRectEx(&rect, style, FALSE, exstyle);
  hwndApp = CreateWindowExW(exstyle, wc.lpszClassName, wcapt,
                            style | WS_MINIMIZEBOX | WS_VISIBLE,
                            (rcDesktop.right-rect.right)/ 2,(rcDesktop.bottom - rect.bottom)/ 2,
                            rect.right - rect.left, rect.bottom - rect.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (hwndApp == NULL)
    return NULL;

  if (flags & GUIDRV_TIMER)
    SetTimer(hwndApp, 1, 100, NULL);

  ctx = nk_gdip_init(hwndApp, width, height);

  fontStd = nk_gdipfont_create("Segoe UI", fontsize);
  if (fontStd == NULL)
    fontStd = nk_gdipfont_create("Tahoma", fontsize);
  if (fontStd == NULL)
    fontStd = nk_gdipfont_create("Arial", fontsize);

  fontMono = nk_gdipfont_create("DejaVu Sans Mono", fontsize);
  if (fontMono == NULL)
    fontMono = nk_gdipfont_create("Consolas", fontsize);
  if (fontMono == NULL)
    fontMono = nk_gdipfont_create("Courier New", fontsize);

  NK_ASSERT(fontStd != NULL);
  nk_gdipfont_set_voffset(fontStd, -3);
  nk_gdip_set_font(fontStd);

  return ctx;
}

void guidriver_close(void)
{
  nk_gdipfont_del(fontStd);
  nk_gdipfont_del(fontMono);
  nk_gdip_shutdown();
  // UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

/** guidriver_setfont() switches font between standard (proportional) and
 *  monospaced.
 *  \param ctx    The Nuklear context.
 *  \param type   Either FONT_STD ot FONT_MONO.
 *  \return The previous type.
 */
int guidriver_setfont(struct nk_context *ctx, int type)
{
  int prev = fontType;
  (void)ctx;
  switch (type) {
  case FONT_STD:
    if (fontStd != NULL) {
      nk_gdipfont_set_voffset(fontStd, -3);
      nk_gdip_set_font(fontStd);
      fontType = type;
    }
    break;
  case FONT_MONO:
    if (fontMono != NULL) {
      nk_gdipfont_set_voffset(fontMono, 0);
      nk_gdip_set_font(fontMono);
      fontType = type;
    }
    break;
  }
  return prev;
}

/** guidriver_appsize() returns the size of the client area of the
 *  application window. A program can use this to resize Nuklear windows to
 *  fit into the application window.
 */
int guidriver_appsize(int *width, int *height)
{
  if (IsWindow(hwndApp)) {
    RECT rc;
    GetClientRect(hwndApp, &rc);
    NK_ASSERT(width != NULL && height != NULL);
    *width = rc.right - rc.left;
    *height = rc.bottom - rc. top;
    return 1;
  }
  return 0;
}

void guidriver_render(struct nk_color clear)
{
  nk_gdip_render(NK_ANTI_ALIASING_ON, clear);
}

int guidriver_poll(int waitidle)
{
  MSG msg;

  if (waitidle) {
    /* wait for an event, to avoid taking CPU load without anything to do */
    if (GetMessageW(&msg, NULL, 0, 0) <= 0)
      return 0;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  /* so there was at least one event, handle all outstanding events */
  while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT)
      return 0;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 1;
}

void *guidriver_apphandle(void)
{
  return &hwndApp;
}

struct nk_image guidriver_image_from_memory(const unsigned char *data, unsigned size)
{
  return nk_gdip_load_image_from_memory(data, size);
}

#elif defined __linux__

static GLFWwindow *winApp;
static int fontType = 0;
static struct nk_font *fontStd = NULL;
static struct nk_font *fontMono = NULL;

static void error_callback(int e, const char *d)
{
  fprintf(stderr, "Error %d: %s\n", e, d);
}

struct nk_context* guidriver_init(const char *caption, int width, int height, int flags, int fontsize)
{
  extern const unsigned char appicon_data[];
  extern const unsigned int appicon_datasize;
  struct nk_context *ctx;
  char path[256];
  GLFWimage icons[1];
  unsigned error;

  /* GLFW */
  glfwSetErrorCallback(error_callback);
  if (!glfwInit()) {
    // fprintf(stderr, "[GFLW] failed to init!\n");
    return NULL;
  }
  glfwWindowHint(GLFW_RESIZABLE, (flags & GUIDRV_RESIZEABLE) != 0);
  winApp = glfwCreateWindow(width, height, caption, NULL, NULL);
  glfwMakeContextCurrent(winApp);

  /* add window icon */
  memset(icons, 0, sizeof icons);
  error = lodepng_decode32(&icons[0].pixels, (unsigned*)&icons[0].width, (unsigned*)&icons[0].height, appicon_data, appicon_datasize);
  if (!error)
    glfwSetWindowIcon(winApp, 1, icons);
  free(icons[0].pixels);

  ctx = nk_glfw3_init(winApp, NK_GLFW3_INSTALL_CALLBACKS);
  if (font_locate(path, sizeof path, "Ubuntu", "")
      || font_locate(path, sizeof path, "FreeSans", "")
      || font_locate(path, sizeof path, "Liberation Sans", ""))
  {
    struct nk_font_atlas *atlas;
    nk_glfw3_font_stash_begin(&atlas);
    fontStd = nk_font_atlas_add_from_file(atlas, path, fontsize, 0);
    nk_glfw3_font_stash_end();
    /* Load Cursor: if you uncomment cursor loading please hide the cursor */
    /*nk_style_load_all_cursors(ctx, atlas->cursors);*/
    if (fontStd != NULL)
      nk_style_set_font(ctx, &fontStd->handle);
  }
  if (font_locate(path, sizeof path, "Andale Mono", "")
      || font_locate(path, sizeof path, "Liberation Mono", ""))
  {
    struct nk_font_atlas *atlas;
    nk_glfw3_font_stash_begin(&atlas);
    fontMono = nk_font_atlas_add_from_file(atlas, path, fontsize, 0);
    nk_glfw3_font_stash_end();
  }

  return ctx;
}

void guidriver_close(void)
{
  nk_glfw3_shutdown();
  glfwTerminate();
}

/** guidriver_setfont() switches font between standard (proportional) and
 *  monospaced.
 *  \param type   Either FONT_STD ot FONT_MONO.
 *  \return The previous type.
 */
int guidriver_setfont(struct nk_context *ctx, int type)
{
  int prev = fontType;
  switch (type) {
  case FONT_STD:
    if (fontStd != NULL) {
      nk_style_set_font(ctx, &fontStd->handle);
      fontType = type;
    }
    break;
  case FONT_MONO:
    if (fontMono != NULL) {
      nk_style_set_font(ctx, &fontMono->handle);
      fontType = type;
    }
    break;
  }
  return prev;
}

int guidriver_appsize(int *width, int *height)
{
  glfwGetWindowSize(winApp, width, height);
  return 1;
}

void guidriver_render(struct nk_color clear)
{
  int width = 0, height = 0;

  glfwGetWindowSize(winApp, &width, &height);
  glViewport(0, 0, width, height);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(clear.r/255.0, clear.g/255.0, clear.b/255.0, clear.a/255.0);
  /* IMPORTANT: `nk_glfw_render` modifies some global OpenGL state
   * with blending, scissor, face culling and depth test and defaults everything
   * back into a default state. Make sure to either save and restore or
   * reset your own state after drawing rendering the UI. */
  nk_glfw3_render(NK_ANTI_ALIASING_ON);
  glfwSwapBuffers(winApp);
}

int guidriver_poll(int waitidle)
{
  (void)waitidle;
  if (glfwWindowShouldClose(winApp))
    return 0;
  glfwPollEvents();
  nk_glfw3_new_frame();
  return 1;
}

void *guidriver_apphandle(void)
{
  return winApp;
}

#if !defined(GL_GENERATE_MIPMAP)
  #define GL_GENERATE_MIPMAP 0x8191 /* from GLEW.h, OpenGL 1.4 only! */
#endif

struct nk_image guidriver_image_from_memory(const unsigned char *data, unsigned size)
{
  unsigned w, h;
  unsigned char *pixels;
  GLuint tex;

  if (lodepng_decode32(&pixels, &w, &h, data, size) != 0)
    return nk_image_id(0);

  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_NEAREST);
  #if defined(_USE_OPENGL) && (_USE_OPENGL > 2)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
  #else
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  #endif

  return nk_image_id((int)tex);
}

#endif

