
#ifndef _FINDFONT_H
#define _FINDFONT_H

#include <stdint.h>

#if defined __cplusplus
  extern "C"
#endif
int font_locate(char *path, size_t maxlength, const char *family, const char *style);

#endif /* _FINDFONT_H */
