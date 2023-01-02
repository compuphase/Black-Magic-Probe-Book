/*
 * Setting the mouse pointer shape in Windows and in GLFW.
 *
 * Copyright 2020 CompuPhase
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
# include <windows.h>
#elif defined __linux__
# include <GLFW/glfw3.h>
#endif

#include <assert.h>
#include "nuklear_mousepointer.h"

#if defined __linux__
  static GLFWcursor *cursor_hresize = NULL, *cursor_vresize = NULL, *cursor_wait = NULL;
  static GLFWwindow *cursor_window = NULL;
#endif

void pointer_init(void *window)
{
# if defined __linux__
    cursor_window = (GLFWwindow*)window;
    cursor_hresize = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
    cursor_vresize = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
#   if defined GLFW_NOT_ALLOWED_CURSOR
      cursor_wait = glfwCreateStandardCursor(GLFW_NOT_ALLOWED_CURSOR);
#   endif
# else
    (void)window;
# endif
}

void pointer_cleanup(void)
{
# if defined __linux__
    glfwDestroyCursor(cursor_hresize);
    glfwDestroyCursor(cursor_vresize);
    if (cursor_wait != NULL)
      glfwDestroyCursor(cursor_wait);
# endif
}

void pointer_setstyle(int style)
{
# if defined WIN32 || defined _WIN32
    switch (style) {
    case CURSOR_UPDOWN:
      SetCursor(LoadCursor(NULL, IDC_SIZENS));
      break;
    case CURSOR_LEFTRIGHT:
      SetCursor(LoadCursor(NULL, IDC_SIZEWE));
      break;
    case CURSOR_WAIT:
      SetCursor(LoadCursor(NULL, IDC_WAIT));
      break;
    default:
      SetCursor(LoadCursor(NULL, IDC_ARROW));
    }
# elif defined __linux__
    switch (style) {
    case CURSOR_UPDOWN:
      glfwSetCursor(cursor_window, cursor_vresize);
      break;
    case CURSOR_LEFTRIGHT:
      glfwSetCursor(cursor_window, cursor_hresize);
      break;
    case CURSOR_WAIT:
      glfwSetCursor(cursor_window, cursor_wait);  /* resets to default it undefined */
      break;
    default:
      glfwSetCursor(cursor_window, NULL);  /* reset to default */
    }
# endif
}

