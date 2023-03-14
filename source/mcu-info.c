/*
 * Microcontroller description lookup, based on brand and part id.
 *
 * Copyright 2022-2023 CompuPhase
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
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "mcu-info.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
# define stricmp(s1,s2)    strcasecmp((s1),(s2))
# define strnicmp(s1,s2,n) strncasecmp((s1),(s2),(n))
#elif defined _MSC_VER
# define stricmp(s1,s2)    _stricmp((s1),(s2))
# define strnicmp(s1,s2,c) _strnicmp((s1),(s2),(c))
#endif
#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

typedef struct tagMCUFAMILY {
  uint32_t partid;
  const char *name;
} MCULOOKUP;


static const MCUINFO mcutable_stm32[] = {
  {      0x410, "STM32F", ~0,      ~0, "STM32F1xx medium-density" },
  {      0x411, "STM32F", ~0,      ~0, "STM32F2xx" },
  {      0x412, "STM32F", ~0,      ~0, "STM32F1xx low-density" },
  {      0x413, "STM32F", ~0,      ~0, "STM32F405xx/F407xx/F415xx/F417xx" },
  {      0x414, "STM32F", ~0,      ~0, "STM32F1xx high-density" },
  {      0x415, "STM32L", ~0,      ~0, "STM32L4xx" },
  {      0x416, "STM32L", ~0,      ~0, "STM32L1xx medium-density" },
  {      0x417, "STM32L", ~0,      ~0, "STM32L0xx" },
  {      0x418, "STM32F", ~0,      ~0, "STM32F1xx connectivity line" },
  {      0x419, "STM32F", ~0,      ~0, "STM32F4xx high-density" },
  {      0x420, "STM32F", ~0,      ~0, "STM32F1xx value line" },
  {      0x421, "STM32F", ~0,      ~0, "STM32F446xx" },
  {      0x422, "STM32F", ~0,      ~0, "STM32F3xx" },
  {      0x423, "STM32F", ~0,      ~0, "STM32F4xx low power" },
  {      0x425, "STM32L", ~0,      ~0, "STM32L0xx cat. 2" },
  {      0x427, "STM32L", ~0,      ~0, "STM32L1xx medium-density/plus" },
  {      0x428, "STM32F", ~0,      ~0, "STM32F1xx value line/high-density" },
  {      0x429, "STM32L", ~0,      ~0, "STM32L1xx cat. 2" },
  {      0x430, "STM32F", ~0,      ~0, "STM32F1xx xl-density" },
  {      0x431, "STM32F", ~0,      ~0, "STM32F411re" },
  {      0x432, "STM32F", ~0,      ~0, "STM32F37x" },
  {      0x433, "STM32F", ~0,      ~0, "STM32F4xx de" },
  {      0x434, "STM32F", ~0,      ~0, "STM32F4xx dsi" },
  {      0x435, "STM32L", ~0,      ~0, "STM32L43x" },
  {      0x436, "STM32L", ~0,      ~0, "STM32L1xx high-density" },
  {      0x437, "STM32L", ~0,      ~0, "STM32L152RE" },
  {      0x438, "STM32F", ~0,      ~0, "STM32F334" },
  {      0x439, "STM32F", ~0,      ~0, "STM32F3xx small" },
  {      0x440, "STM32F", 0x10000, ~0, "STM32F05xx/F03xx" },
  {      0x441, "STM32F", ~0,      ~0, "STM32F412" },
  {      0x442, "STM32F", 0x40000, ~0, "STM32F030xC/F09xx" },
  {      0x444, "STM32F", 0x8000,  ~0, "STM32F03xx" },
  {      0x445, "STM32F", 0x8000,  ~0, "STM32F04xx/F07x6" },
  {      0x446, "STM32F", ~0, ~0,      "STM32F303 high-density" },
  {      0x447, "STM32L", ~0, ~0,      "STM32L0xx cat. 5" },
  {      0x448, "STM32F", 0x20000, ~0, "STM32F07xx" },
  {      0x449, "STM32F", ~0, ~0,      "STM32F74xxx/F75xxx" },
  {      0x450, "STM32H", ~0, ~0,      "STM32H7xxx" },
  {      0x451, "STM32F", ~0, ~0,      "STM32F76xxx/77xxx" },
  {      0x452, "STM32F", ~0, ~0,      "STM32F72xxx/73xxx" },
  {      0x457, "STM32L", ~0, ~0,      "STM32L011" },
  {      0x458, "STM32F", ~0, ~0,      "STM32F410" },
  {      0x463, "STM32F", ~0, ~0,      "STM32F413" },

  {      0x410, "GD32E",  ~0, ~0,      "GD32E230" },
  {      0x410, "GD32F",  ~0, ~0,      "GD32F103" },
  {      0x414, "GD32F",  ~0, ~0,      "GD32F303" },
};

static const MCUINFO mcutable_lpc[] = {
  { 0x00008A04, NULL,  0x8000, 0x2000, "LPC8N04 - M0+ 32K Flash 8K SRAM" },         /* UM11074 Rev 1.3 2018 Ch 4.5.19 Table 25 */
  { 0x00008021, NULL,  0x4000,  0x800, "LPC802M001 - M0+ 16K Flash 2K SRAM" },      /* UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
  { 0x00008023, NULL,  0x4000,  0x800, "LPC802M001 - M0+ 16K Flash 2K SRAM" },      /* UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
  { 0x00008024, NULL,  0x4000,  0x800, "LPC802M001 - M0+ 16K Flash 2K SRAM" },      /* UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
  { 0x00008022, NULL,  0x4000,  0x800, "LPC802M011 - M0+ 16K Flash 2K SRAM" },      /* UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
  { 0x00008040, NULL,  0x8000, 0x1000, "LPC804M101 - M0+ 32K Flash 4K SRAM" },      /* UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
  { 0x00008041, NULL,  0x8000, 0x1000, "LPC804M101 - M0+ 32K Flash 4K SRAM" },      /* UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
  { 0x00008042, NULL,  0x8000, 0x1000, "LPC804M101 - M0+ 32K Flash 4K SRAM" },      /* UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
  { 0x00008043, NULL,  0x8000, 0x1000, "LPC804M111 - M0+ 32K Flash 4K SRAM" },      /* UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
  { 0x00008044, NULL,  0x8000, 0x1000, "LPC804M101 - M0+ 32K Flash 4K SRAM" },      /* UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
  { 0x00008100, NULL,  0x1000,  0x400, "LPC810M021 - M0+ 4K Flash 1K SRAM" },       /* UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
  { 0x00008110, NULL,  0x2000,  0x800, "LPC811M001 - M0+ 8K Flash 2K SRAM" },       /* UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
  { 0x00008120, NULL,  0x4000, 0x1000, "LPC812M101 - M0+ 16K Flash 4K SRAM" },      /* UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
  { 0x00008121, NULL,  0x4000, 0x1000, "LPC812M101 - M0+ 16K Flash 4K SRAM" },      /* UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
  { 0x00008122, NULL,  0x4000, 0x1000, "LPC812M101 - M0+ 16K Flash 4K SRAM" },      /* UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
  { 0x00008221, NULL,  0x4000, 0x1000, "LPC822M101 - M0+ 16K Flash 4K SRAM" },      /* UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
  { 0x00008222, NULL,  0x4000, 0x1000, "LPC822M101 - M0+ 16K Flash 4K SRAM" },      /* UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
  { 0x00008241, NULL,  0x8000, 0x2000, "LPC824M201 - M0+ 32K Flash 8K SRAM" },      /* UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
  { 0x00008242, NULL,  0x8000, 0x2000, "LPC824M201 - M0+ 32K Flash 8K SRAM" },      /* UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
  { 0x00008322, NULL,  0x4000, 0x1000, "LPC832M101 - M0+ 16K Flash 4K SRAM" },      /* UM11021 Rev 1.1 2016 Ch 24.6.1.11 Table 317 */
  { 0x00008341, NULL,  0x8000, 0x1000, "LPC834M101 - M0+ 32K Flash 4K SRAM" },      /* UM11021 Rev 1.1 2016 Ch 24.6.1.11 Table 317 */
  { 0x00008441, NULL, 0x10000, 0x2000, "LPC844M201 - M0+ 64K Flash 8K SRAM" },      /* UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
  { 0x00008442, NULL, 0x10000, 0x2000, "LPC844M201 - M0+ 64K Flash 8K SRAM" },      /* UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
  { 0x00008443, NULL, 0x10000, 0x2000, "LPC844M201 - M0+ 64K Flash 8K SRAM" },      /* UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173; note: table 29 is wrong) */
  { 0x00008444, NULL, 0x10000, 0x2000, "LPC844M201 - M0+ 64K Flash 8K SRAM" },      /* UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
  { 0x00008451, NULL, 0x10000, 0x4000, "LPC845M301 - M0+ 64K Flash 16K SRAM" },     /* UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
  { 0x00008452, NULL, 0x10000, 0x4000, "LPC845M301 - M0+ 64K Flash 16K SRAM" },     /* UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
  { 0x00008453, NULL, 0x10000, 0x4000, "LPC845M301 - M0+ 64K Flash 16K SRAM" },     /* UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
  { 0x00008454, NULL, 0x10000, 0x4000, "LPC845M301 - M0+ 64K Flash 16K SRAM" },     /* UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
  { 0x2500102B, NULL,  0x8000, 0x2000, "LPC1102 - M0 32K Flash 8K SRAM" },          /* UM10429 Rev 6 2013 Ch 17.5.11 Table 173 */
  { 0x2548102B, NULL,  0x8000, 0x2000, "LPC1104 - M0 32K Flash 8K SRAM" },          /* UM10429 Rev 6 2013 Ch 17.5.11 Table 173 */
  { 0x0A07102B, NULL,  0x1000,  0x400, "LPC1110 - M0 4K Flash 1K SRAM" },           /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1A07102B, NULL,  0x1000,  0x400, "LPC1110 - M0 4K Flash 1K SRAM" },           /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0A16D02B, NULL,  0x2000,  0x800, "LPC1111/002 - M0 8K Flash 2K SRAM" },       /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1A16D02B, NULL,  0x2000,  0x800, "LPC1111/002 - M0 8K Flash 2K SRAM" },       /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x041E502B, NULL,  0x2000,  0x800, "LPC1111/101 - M0 8K Flash 2K SRAM" },       /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x2516D02B, NULL,  0x2000,  0x800, "LPC1111/102 - M0 8K Flash 2K SRAM" },       /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00010013, NULL,  0x2000,  0x800, "LPC1111/103 - M0 8K Flash 2K SRAM" },       /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0416502B, NULL,  0x4000, 0x1000, "LPC1111/201 - M0 8K Flash 4K SRAM" },       /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x2516902B, NULL,  0x4000, 0x1000, "LPC1111/202 - M0 8K Flash 4K SRAM" },       /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00010012, NULL,  0x4000, 0x1000, "LPC1111/203 - M0 8K Flash 4K SRAM" },       /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x042D502B, NULL,  0x4000,  0x800, "LPC1112/101 - M0 16K Flash 2K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x2524D02B, NULL,  0x4000,  0x800, "LPC1112/102 - M0 16K Flash 2K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0A24902B, NULL,  0x4000, 0x1000, "LPC1112/102 - M0 16K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1A24902B, NULL,  0x4000, 0x1000, "LPC1112/102 - M0 16K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0A23902B, NULL,  0x4000, 0x1000, "LPC1112/102 - M0 16K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1A23902B, NULL,  0x4000, 0x1000, "LPC1112/102 - M0 16K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00020023, NULL,  0x4000,  0x800, "LPC1112/103 - M0 16K Flash 2K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0425502B, NULL,  0x4000, 0x1000, "LPC1112/201 - M0 16K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x2524902B, NULL,  0x4000, 0x1000, "LPC1112/202 - M0 16K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00020022, NULL,  0x4000, 0x1000, "LPC1112/203 - M0 16K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0434502B, NULL,  0x6000, 0x1000, "LPC1113/201 - M0 24K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x2532902B, NULL,  0x6000, 0x1000, "LPC1113/202 - M0 24K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00030032, NULL,  0x6000, 0x1000, "LPC1113/203 - M0 24K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0434102B, NULL,  0x6000, 0x2000, "LPC1113/301 - M0 24K Flash 8K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x2532102B, NULL,  0x6000, 0x2000, "LPC1113/302 - M0 24K Flash 8K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00030030, NULL,  0x6000, 0x2000, "LPC1113/303 - M0 24K Flash 8K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0A40902B, NULL,  0x8000, 0x1000, "LPC1114/102 - M0 32K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1A40902B, NULL,  0x8000, 0x1000, "LPC1114/102 - M0 32K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0444502B, NULL,  0x8000, 0x1000, "LPC1114/201 - M0 32K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x2540902B, NULL,  0x8000, 0x1000, "LPC1114/202 - M0 32K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00040042, NULL,  0x8000, 0x1000, "LPC1114/203 - M0 32K Flash 4K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x0444102B, NULL,  0x8000, 0x2000, "LPC1114/301 - M0 32K Flash 8K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x2540102B, NULL,  0x8000, 0x2000, "LPC1114/302 & LPC11D14/302 - M0 32K Flash 8K SRAM" },/* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00040040, NULL,  0x8000, 0x2000, "LPC1114/303 - M0 32K Flash 8K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00040060, NULL,  0xc000, 0x2000, "LPC1114/323 - M0 48K Flash 8K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00040070, NULL,  0xe000, 0x2000, "LPC1114/333 - M0 56K Flash 8K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x00050080, NULL, 0x10000, 0x2000, "LPC1115/303 - M0 64K Flash 8K SRAM" },      /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1421102B, NULL,  0x4000, 0x2000, "LPC11C12/301 - M0 16K Flash 8K SRAM" },     /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1440102B, NULL,  0x8000, 0x2000, "LPC11C14/301 - M0 32K Flash 8K SRAM" },     /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1431102B, NULL,  0x4000, 0x2000, "LPC11C22/301 - M0 16K Flash 8K SRAM" },     /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x1430102B, NULL,  0x8000, 0x2000, "LPC11C24/301 - M0 32K Flash 8K SRAM" },     /* UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
  { 0x293E902B, NULL,  0x2000, 0x1000, "LPC11E11/101 - M0 8K Flash 4K SRAM" },      /* UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
  { 0x2954502B, NULL,  0x4000, 0x1800, "LPC11E12/201 - M0 16K Flash 6K SRAM" },     /* UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
  { 0x296A102B, NULL,  0x6000, 0x2000, "LPC11E13/301 - M0 24K Flash 8K SRAM" },     /* UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
  { 0x2980102B, NULL,  0x8000, 0x2800, "LPC11E14/401 - M0 32K Flash 10K SRAM" },    /* UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
  { 0x0000BC41, NULL, 0x10000, 0x3000, "LPC11E35/501 - M0 64K Flash 12K SRAM" },    /* UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
  { 0x00009C41, NULL, 0x18000, 0x3000, "LPC11E36/501 - M0 96K Flash 12K SRAM" },    /* UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
  { 0x00007C45, NULL, 0x20000, 0x2800, "LPC11E37/401 - M0 128K Flash 10K SRAM" },   /* UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
  { 0x00007C41, NULL, 0x20000, 0x3000, "LPC11E37/501 - M0 128K Flash 12K SRAM" },   /* UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
  { 0x0000DCC1, NULL, 0x10000, 0x2000, "LPC11E66 - M0+ 64K Flash 8K SRAM" },        /* UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
  { 0x0000BC81, NULL, 0x20000, 0x4000, "LPC11E67 - M0+ 128K Flash 16K SRAM" },      /* UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
  { 0x00007C01, NULL, 0x40000, 0x8000, "LPC11E68 - M0+ 256K Flash 32K SRAM" },      /* UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
  { 0x095C802B, NULL,  0x4000, 0x1000, "LPC11U12/201 - M0 16K Flash 4K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x295C802B, NULL,  0x4000, 0x1000, "LPC11U12/201 - M0 16K Flash 4K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x097A802B, NULL,  0x6000, 0x1000, "LPC11U13/201 - M0 24K Flash 4K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x297A802B, NULL,  0x6000, 0x1000, "LPC11U13/201 - M0 24K Flash 4K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x0998802B, NULL,  0x8000, 0x1000, "LPC11U14/201 - M0 32K Flash 4K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x2998802B, NULL,  0x8000, 0x1000, "LPC11U14/201 - M0 32K Flash 4K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x2954402B, NULL,  0x4000, 0x1800, "LPC11U22/301 - M0 16K Flash 6K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x2972402B, NULL,  0x6000, 0x1800, "LPC11U23/301 - M0 24K Flash 6K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x2988402B, NULL,  0x8000, 0x1800, "LPC11U24/301 - M0 32K Flash 6K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x2980002B, NULL,  0x8000, 0x2000, "LPC11U24/401 - M0 32K Flash 8K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x0003D440, NULL,  0xa000, 0x2000, "LPC11U34/311 - M0 40K Flash 8K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x0001cc40, NULL,  0xc000, 0x2000, "LPC11U34/421 - M0 48K Flash 8K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x0001BC40, NULL, 0x10000, 0x2000, "LPC11U35/401 - M0 64K Flash 8K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x0000BC40, NULL, 0x10000, 0x2000, "LPC11U35/501 - M0 64K Flash 8K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x00019C40, NULL, 0x18000, 0x2000, "LPC11U36/401 - M0 96K Flash 8K SRAM" },     /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x00017C40, NULL, 0x20000, 0x2000, "LPC11U37/401 - M0 128K Flash 8K SRAM" },    /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x00007C44, NULL, 0x20000, 0x2000, "LPC11U37/401 - M0 128K Flash 8K SRAM" },    /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x00007C40, NULL, 0x20000, 0x2000, "LPC11U37/501 - M0 128K Flash 8K SRAM" },    /* UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
  { 0x0000DCC8, NULL, 0x10000, 0x2000, "LPC11U66 - M0+ 64K Flash 8K SRAM" },        /* UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
  { 0x0000BC88, NULL, 0x20000, 0x4000, "LPC11U67 - M0+ 128K Flash 16K SRAM" },      /* UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
  { 0x0000BC80, NULL, 0x20000, 0x4000, "LPC11U67 - M0+ 128K Flash 16K SRAM" },      /* UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
  { 0x00007C08, NULL, 0x40000, 0x8000, "LPC11U68 - M0+ 256K Flash 32K SRAM" },      /* UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
  { 0x00007C00, NULL, 0x40000, 0x8000, "LPC11U68 - M0+ 256K Flash 32K SRAM" },      /* UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
  { 0x3640C02B, NULL,  0x8000, 0x1000, "LPC1224/101 - M0 32K Flash 4K SRAM" },     /* UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
  { 0x3642C02B, NULL,  0xc000, 0x1000, "LPC1224/121 - M0 48K Flash 4K SRAM" },     /* UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
  { 0x3650002B, NULL, 0x10000, 0x2000, "LPC1225/301 - M0 64K Flash 8K SRAM" },     /* UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
  { 0x3652002B, NULL, 0x14000, 0x2000, "LPC1225/321 - M0 80K Flash 8K SRAM" },     /* UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
  { 0x3660002B, NULL, 0x18000, 0x2000, "LPC1226/301 - M0 96K Flash 8K SRAM" },     /* UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
  { 0x3670002B, NULL, 0x20000, 0x2000, "LPC1227/301 & LPC12D27/301 - M0 128K Flash 8K SRAM" },/* UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
  { 0x2C42502B, NULL,  0x2000, 0x1000, "LPC1311 - M3 8K Flash 4K SRAM" },           /* UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
  { 0x1816902B, NULL,  0x2000, 0x1000, "LPC1311/01 - M3 8K Flash 4K SRAM" },        /* UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
  { 0x2C40102B, NULL,  0x8000, 0x2000, "LPC1313 - M3 32K Flash 8K SRAM" },          /* UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
  { 0x1830102B, NULL,  0x8000, 0x2000, "LPC1313/01 - M3 32K Flash 8K SRAM" },       /* UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
  { 0x3A010523, NULL,  0x8000, 0x2000, "LPC1315 - M3 32K Flash 8K SRAM" },          /* UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
  { 0x1A018524, NULL,  0xc000, 0x2000, "LPC1316 - M3 48K Flash 8K SRAM" },          /* UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
  { 0x1A020525, NULL, 0x10000, 0x2000, "LPC1317 - M3 64K Flash 8K SRAM" },          /* UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
  { 0x3D01402B, NULL,  0x4000, 0x1000, "LPC1342 - M3 16K Flash 4K SRAM" },          /* UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
  { 0x3D00002B, NULL,  0x8000, 0x2000, "LPC1343 - M3 32K Flash 8K SRAM" },          /* UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
  { 0x3000002B, NULL,  0x8000, 0x2000, "LPC1343 - M3 32K Flash 8K SRAM" },          /* UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
  { 0x28010541, NULL,  0x8000, 0x2000, "LPC1345 - M3 32K Flash 8K SRAM" },          /* UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
  { 0x08018542, NULL,  0xc000, 0x2000, "LPC1346 - M3 48K Flash 8K SRAM" },          /* UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
  { 0x08020543, NULL, 0x10000, 0x2000, "LPC1347 - M3 64K Flash 8K SRAM" },          /* UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
  { 0x00001517, NULL, 0x10000, 0x3000, "LPC1517 - M3 64K Flash 12K SRAM" },         /* UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
  { 0x00001518, NULL, 0x20000, 0x5000, "LPC1518 - M3 128K Flash 20K SRAM" },        /* UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
  { 0x00001519, NULL, 0x40000, 0x9000, "LPC1519 - M3 256K Flash 36K SRAM" },        /* UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
  { 0x00001547, NULL, 0x10000, 0x3000, "LPC1547 - M3 64K Flash 12K SRAM" },         /* UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
  { 0x00001548, NULL, 0x20000, 0x5000, "LPC1548 - M3 128K Flash 20K SRAM" },        /* UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
  { 0x00001549, NULL, 0x40000, 0x9000, "LPC1549 - M3 256K Flash 36K SRAM" },        /* UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
  { 0x25001118, NULL,  0x8000, 0x2000, "LPC1751 - M3 32K Flash 8K SRAM" },          /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x25001110, NULL,  0x8000, 0x2000, "LPC1751 (No CRP) - M3 32K Flash 8K SRAM" }, /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x25001121, NULL, 0x10000, 0x4000, "LPC1752 - M3 64K Flash 16K SRAM" },         /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x25011722, NULL, 0x20000, 0x8000, "LPC1754 - M3 128K Flash 32K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x25011723, NULL, 0x40000, 0x8000, "LPC1756 - M3 256K Flash 32K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x25013F37, NULL, 0x80000,0x10000, "LPC1758 - M3 512K Flash 64K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x25113737, NULL, 0x80000,0x10000, "LPC1759 - M3 512K Flash 64K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x26012033, NULL, 0x40000,0x10000, "LPC1763 - M3 256K Flash 64K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x26011922, NULL, 0x20000, 0x8000, "LPC1764 - M3 128K Flash 32K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x26013733, NULL, 0x40000,0x10000, "LPC1765 - M3 256K Flash 64K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x26013F33, NULL, 0x40000,0x10000, "LPC1766 - M3 256K Flash 64K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x26012837, NULL, 0x80000,0x10000, "LPC1767 - M3 512K Flash 64K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x26013F37, NULL, 0x80000,0x10000, "LPC1768 - M3 512K Flash 64K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
  { 0x26113F37, NULL, 0x80000,0x10000, "LPC1769 - M3 512K Flash 64K SRAM" },        /* UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
//{          0, NULL, 0x20000, 0x8000, "LPC1773 - M3 128K Flash 32K SRAM" },        /* UM10470 Rev 4.0 2016, no part id is given */
  { 0x27011132, NULL, 0x20000, 0x8000, "LPC1774 - M3 128K Flash 32K SRAM" },        /* UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
  { 0x27191F43, NULL, 0x40000,0x10000, "LPC1776 - M3 256K Flash 64K SRAM" },        /* UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
  { 0x27193747, NULL, 0x80000,0x10000, "LPC1777 - M3 512K Flash 64K SRAM" },        /* UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
  { 0x27193F47, NULL, 0x80000,0x10000, "LPC1778 - M3 512K Flash 64K SRAM" },        /* UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
  { 0x281D1743, NULL, 0x40000,0x10000, "LPC1785 - M3 256K Flash 64K SRAM" },        /* UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
  { 0x281D1F43, NULL, 0x40000,0x10000, "LPC1786 - M3 256K Flash 64K SRAM" },        /* UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
  { 0x281D3747, NULL, 0x80000,0x10000, "LPC1787 - M3 512K Flash 64K SRAM" },        /* UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
  { 0x281D3F47, NULL, 0x80000,0x10000, "LPC1788 - M3 512K Flash 64K SRAM" },        /* UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
  { 0x5284E02B, NULL,       0,0x22000, "LPC18[S]x0 - M3 no Flash 104K~136K SRAM" }, /* UM10430 Rev 3.1 2019 Ch 10.4.10 Yable 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
  { 0x6284E02B, NULL,       0,0x22000, "LPC18[S]x0 - M3 no Flash 104K~136K SRAM" }, /* UM10430 Rev 3.1 2019 Ch 10.4.10 Yable 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
  { 0x4284E02B, NULL,0x100000,0x12000, "LPC18[S]xx - M3 512K~1M Flash 72K SRAM" },  /* UM10430 Rev 3.1 2019 Ch 10.4.10 Yable 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
  { 0x7284E02B, NULL,0x100000,0x12000, "LPC18[S]xx - M3 512K~1M Flash 72K SRAM" },  /* UM10430 Rev 3.1 2019 Ch 10.4.10 Yable 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
//{          0, NULL, 0x10000, 0x6000, "LPC4072 - M4 64K Flash 24K SRAM" },         /* UM10562 Rev 3 2014, no part id is given */
  { 0x47011132, NULL, 0x20000, 0xa000, "LPC4074 - M4 128K Flash 40K SRAM" },        /* UM10562 Rev 3 2014 Ch 38.7.11 Table 753 */
  { 0x47191F43, NULL, 0x40000,0x14000, "LPC4076 - M4 256K Flash 80K SRAM" },        /* UM10562 Rev 3 2014 Ch 38.7.11 Table 753 */
  { 0x47193F47, NULL, 0x80000,0x18000, "LPC4078 - M4 512K Flash 96K SRAM" },        /* UM10562 Rev 3 2014 Ch 38.7.11 Table 753 */
  { 0x481D3F47, NULL, 0x80000,0x18000, "LPC4088 - M4 512K Flash 96K SRAM" },        /* UM10562 Rev 3 2014 Ch 38.7.11 Table 753 */
  { 0x5906002B, NULL,       0,0x32000, "LPC43[S]x0 - M4/M0 no Flash 136K~200K SRAM" },/* UM10503 Rev 2.3 2017, note: single "CHIP ID" for a group of MCUs with different memory sizes */
  { 0x6906002B, NULL,       0,0x32000, "LPC43[S]x0 - M4/M0 no Flash 136K~200K SRAM" },/* UM10503 Rev 2.3 2017, note: single "CHIP ID" for a group of MCUs with different memory sizes */
  { 0x4906002B, NULL,0x100000,0x12000, "LPC43[S]xx - M4/M0 512K~1M Flash 72K SRAM" }, /* UM10503 Rev 2.3 2017, note: single "CHIP ID" for a group of MCUs with different memory sizes */
  { 0x7906002B, NULL,0x100000,0x12000, "LPC43[S]xx - M4/M0 512K~1M Flash 72K SRAM" }, /* UM10503 Rev 2.3 2017, note: single "CHIP ID" for a group of MCUs with different memory sizes */
  { 0x7F954605, NULL, 0x40000,0x20000, "LPC54605J256 - M4 256K Flash 128K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0xFFF54605, NULL, 0x80000,0x30000, "LPC54605J512 - M4 512K Flash 192K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0x7F954606, NULL, 0x40000,0x20000, "LPC54606J256 - M4 256K Flash 128K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0xFFF54606, NULL, 0x80000,0x30000, "LPC54606J512 - M4 512K Flash 192K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0x7F954607, NULL, 0x40000,0x20000, "LPC54607J256 - M4 256K Flash 128K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0xFFF54607, NULL, 0x80000,0x30000, "LPC54607J512 - M4 512K Flash 192K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0xFFF54608, NULL, 0x80000,0x30000, "LPC54608J512 - M4 512K Flash 192K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0x7F954616, NULL, 0x40000,0x20000, "LPC54616J256 - M4 256K Flash 128K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0xFFF54616, NULL, 0x80000,0x30000, "LPC54616J512 - M4 512K Flash 192K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0xFFF54618, NULL, 0x80000,0x30000, "LPC54618J512 - M4 512K Flash 192K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
  { 0xFFF54628, NULL, 0x80000,0x30000, "LPC54628J512 - M4 512K Flash 192K SRAM" },  /* UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
};

static const MCULOOKUP mculookup_lpc[] = {
  { 0x2C42502B, "LPC1311" },  /* UM10375 Rev 5 2012 */
  { 0x1816902B, "LPC1311" },  /* UM10375 Rev 5 2012 */
  { 0x2C40102B, "LPC1313" },  /* UM10375 Rev 5 2012 */
  { 0x1830102B, "LPC1313" },  /* UM10375 Rev 5 2012 */
  { 0x3A010523, "LPC1315" },  /* UM10524 Rev 4 2013 */
  { 0x1A018524, "LPC1316" },  /* UM10524 Rev 4 2013 */
  { 0x1A020525, "LPC1317" },  /* UM10524 Rev 4 2013 */
  { 0x3D01402B, "LPC1342" },  /* UM10375 Rev 5 2012 */
  { 0x3D00002B, "LPC1343" },  /* UM10375 Rev 5 2012 */
  { 0x3000002B, "LPC1343" },  /* UM10375 Rev 5 2012 */
  { 0x28010541, "LPC1345" },  /* UM10524 Rev 4 2013 */
  { 0x08018542, "LPC1346" },  /* UM10524 Rev 4 2013 */
  { 0x08020543, "LPC1347" },  /* UM10524 Rev 4 2013 */
};

const MCUINFO *mcuinfo_data(const char *family, uint32_t id)
{
  assert(family != NULL);
  if (strlen(family) == 0 || id == 0)
    return NULL;

# if !defined NDEBUG
    for (int i = 0; i < sizearray(mcutable_stm32); i++) {
      uint32_t id1 = mcutable_stm32[i].partid & 0x0fff;
      const char *name1 = mcutable_stm32[i].prefix;
      for (int j = i + 1; j < sizearray(mcutable_stm32); j++) {
        uint32_t id2 = mcutable_stm32[j].partid & 0x0fff;
        const char *name2 = mcutable_stm32[j].prefix;
        assert(id1 != id2 || stricmp(name1, name2) != 0);
      }
    }

    for (int i = 0; i < sizearray(mcutable_lpc); i++) {
      for (int j = i + 1; j < sizearray(mcutable_lpc); j++) {
        assert(mcutable_lpc[i].partid != mcutable_lpc[j].partid);
      }
    }
# endif

  if (strnicmp(family, "STM32", 5) == 0 || strnicmp(family, "GD32", 4) == 0) {
    id &= 0x0fff;
    for (int i = 0; i < sizearray(mcutable_stm32); i++) {
      if (mcutable_stm32[i].partid == id
          && strnicmp(family, mcutable_stm32[i].prefix, strlen(mcutable_stm32[i].prefix)) == 0)
        return &mcutable_stm32[i];
    }
  } else if (strnicmp(family, "LPC", 3) == 0 && isdigit(family[3])) {
    for (int i = 0; i < sizearray(mcutable_lpc); i++)
      if (mcutable_lpc[i].partid == id)
        return &mcutable_lpc[i];
  }

  return NULL;
}

/** mcuinfo_rename() assigns a distinguishing name to parts where the "family
 *  name" that the Black Magic Probe assigns to it, is ambiguous.
 */
const char *mcuinfo_lookup(const char *family, uint32_t id)
{
  assert(family != NULL);
  if (strnicmp(family, "LPC", 3) == 0 && isdigit(family[3])) {
    for (int i = 0; i < sizearray(mculookup_lpc); i++)
      if (mculookup_lpc[i].partid == id)
        return mculookup_lpc[i].name;
  }

  return NULL;
}
