//#define NK_BUTTON_TRIGGER_ON_RELEASE  // gives problems with dragging scroll bars
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_STRING

#if defined _WIN32
  #define NK_SIN
  #define NK_COS
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
  #define NK_INCLUDE_STANDARD_IO
  #define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
  #define NK_INCLUDE_FONT_BAKING
  #define NK_KEYSTATE_BASED_INPUT
#endif

