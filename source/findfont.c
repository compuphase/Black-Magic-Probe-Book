// gcc -g -o findfont findfont.c -lfontconfig
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fontconfig/fontconfig.h>
#if defined __linux__
  #include <bsd/string.h>
#endif

/** font_locate() returns the path to a font file matching the family name
 *  and style.
 *
 *  \param path       The path of the font file is returned in this parameter.
 *  \param maxlength  The size (in characters) of parameter path (which must
 *                    include the zero-terminator.
 *  \param family     The font family name.
 *  \param style      A string with keywords for the style of the font, such as
 *                    "Regular", "Italic", "Bold" or "Bold Italic".
 *
 *  \return 1 on success, 0 on failure.
 */
int font_locate(char *path, size_t maxlength, const char *family, const char *style)
{
  #define MAX_STYLES 10
  FcPattern *pat;
  FcFontSet *fs;
  FcObjectSet *os;
  FcChar8 *s, *ptr;
  FcConfig *config;
  int i, match;
  char *style_copy;
  char *style_fields[MAX_STYLES];

  assert(path != NULL);
  assert(maxlength > 0);
  assert(family != NULL);
  assert(style != NULL);

  /* split style into keywords */
  style_copy = strdup(style);
  if (style_copy == NULL)
    return 0;
  memset(style_fields, 0, sizeof style_fields);
  style_fields[0] = strtok(style_copy, " ");
  for (i = 1; i < MAX_STYLES && style_fields[i - 1] != NULL; i++)
    style_fields[i] = strtok(NULL, " ");

  if (!FcInit())
    return 0;
  config = FcConfigGetCurrent();
  FcConfigSetRescanInterval(config, 0);

  pat = FcPatternCreate();
  os = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, (char *) 0);
  fs = FcFontList(config, pat, os);
  match = 0;
  for (i=0; fs && i < fs->nfont && !match; i++) {
    FcPattern *font = fs->fonts[i];
    s = FcNameUnparse(font);
    match = 1;
    if (FcPatternGetString(font, FC_FAMILY, 0, &ptr) == FcResultMatch) {
      if (strcasecmp((const char*)ptr, family) != 0)
        match = 0;
    } else {
      match = 0;
    }
    if (FcPatternGetString(font, FC_STYLE, 0, &ptr) == FcResultMatch) {
      int styles_to_match, idx;
      char *style_string;
      /* compare words in the font's style with words in the requested style */
      styles_to_match = 0;
      for (idx = 0; idx < MAX_STYLES && style_fields[idx] != NULL; idx++)
        if (strcasecmp(style_fields[idx], "Roman") != 0 && strcasecmp(style_fields[idx], "Regular") != 0 && strcasecmp(style_fields[idx], "Book") != 0)
          styles_to_match |= (1 << idx);
      style_string = strdup((const char*)ptr);
      if (style_string != NULL) {
        char *token;
        for (token = strtok(style_string, " "); token != NULL; token = strtok(NULL, " ")) {
          if (strcasecmp(token, "Roman") == 0 || strcasecmp(token, "Regular") == 0 || strcasecmp(token, "Book") == 0)
            continue; /* these are implied (unless Bold or Italic or Condensed are set) */
          if (strcasecmp(token, "Oblique") == 0)
            token = "Italic";
          for (idx = 0; idx < MAX_STYLES && style_fields[idx] != NULL; idx++)
            if (strcasecmp(token, style_fields[idx]) == 0)
              break;
          if (idx < MAX_STYLES && style_fields[idx] != NULL)
            styles_to_match &= ~(1 << match); /* font style found in styles to match */
          else
            match = 0;  /* font style not found in styles to match, this font has a different style */
        }
        free(style_string);
      }
      if (styles_to_match != 0)
        match = 0;      /* not all styles to match were present in the font style */
    } else {
      match = 0;
    }
    if (match && FcPatternGetString(font, FC_FILE, 0, &ptr) == FcResultMatch) {
      strlcpy(path, (const char*)ptr, maxlength);
      path[maxlength - 1] = '\0';
    } else {
      match = 0;
    }
    free(s);
  }
  free(style_copy);
  if (fs)
    FcFontSetDestroy(fs);
  FcObjectSetDestroy(os);
  FcPatternDestroy(pat);

  return match;
}

#if 0
int main(int argc,char *argv[])
{
  char path[256];

  if (font_locate(path, sizeof path, "DejaVu Sans Mono", ""))
    printf("Found: %s\n", path);
  else
    printf("Not found\n");
}
#endif
