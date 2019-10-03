/*
 * Nuklear - v1.32.0 - public domain
 * no warrenty implied; use at your own risk.
 * authored from 2015-2017 by Micha Mettke
 */
/*
 * ==============================================================
 *
 *                              API
 *
 * ===============================================================
 */
#ifndef NK_GLFW_GL2_H_
#define NK_GLFW_GL2_H_

#include <GLFW/glfw3.h>

enum nk_glfw_init_state{
    NK_GLFW3_DEFAULT = 0,
    NK_GLFW3_INSTALL_CALLBACKS
};
NK_API struct nk_context*   nk_glfw3_init(GLFWwindow *win, enum nk_glfw_init_state);
NK_API void                 nk_glfw3_shutdown(void);
NK_API void                 nk_glfw3_font_stash_begin(struct nk_font_atlas **atlas);
NK_API void                 nk_glfw3_font_stash_end(void);

NK_API void                 nk_glfw3_new_frame(void);
NK_API void                 nk_glfw3_render(enum nk_anti_aliasing);

NK_API void                 nk_glfw3_char_callback(GLFWwindow *win, unsigned int codepoint);
NK_API void                 nk_gflw3_scroll_callback(GLFWwindow *win, double xoff, double yoff);

#endif

