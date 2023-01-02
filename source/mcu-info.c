/*
 * Microcontroller description lookup, based on brand and part id.
 *
 * Copyright 2022 CompuPhase
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
#endif
#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

static MCUINFO mcutable[] = {
  {      0x410, 0x0fff, "STM32F",   NULL, "STM32F1xx medium-density" },
  {      0x411, 0x0fff, "STM32F",   NULL, "STM32F2xx" },
  {      0x412, 0x0fff, "STM32F",   NULL, "STM32F1xx low-density" },
  {      0x413, 0x0fff, "STM32F",   NULL, "STM32F405/407xx or STM32F415/417xx" },
  {      0x414, 0x0fff, "STM32F",   NULL, "STM32F1xx high-density" },
  {      0x415, 0x0fff, "STM32L",   NULL, "STM32L4xx" },
  {      0x416, 0x0fff, "STM32L",   NULL, "STM32L1xx medium-density" },
  {      0x417, 0x0fff, "STM32L",   NULL, "STM32L0xx" },
  {      0x418, 0x0fff, "STM32F",   NULL, "STM32F1xx connectivity line" },
  {      0x419, 0x0fff, "STM32F",   NULL, "STM32F4xx high-density" },
  {      0x420, 0x0fff, "STM32F",   NULL, "STM32F1xx value line" },
  {      0x421, 0x0fff, "STM32F",   NULL, "STM32F446xx" },
  {      0x422, 0x0fff, "STM32F",   NULL, "STM32F3xx" },
  {      0x423, 0x0fff, "STM32F",   NULL, "STM32F4xx low power" },
  {      0x425, 0x0fff, "STM32L",   NULL, "STM32L0xx cat. 2" },
  {      0x427, 0x0fff, "STM32L",   NULL, "STM32L1xx medium-density/plus" },
  {      0x428, 0x0fff, "STM32F",   NULL, "STM32F1xx value line/high-density" },
  {      0x429, 0x0fff, "STM32L",   NULL, "STM32L1xx cat. 2" },
  {      0x430, 0x0fff, "STM32F",   NULL, "STM32F1xx xl-density" },
  {      0x431, 0x0fff, "STM32F",   NULL, "STM32F411re" },
  {      0x432, 0x0fff, "STM32F",   NULL, "STM32F37x" },
  {      0x433, 0x0fff, "STM32F",   NULL, "STM32F4xx de" },
  {      0x434, 0x0fff, "STM32F",   NULL, "STM32F4xx dsi" },
  {      0x435, 0x0fff, "STM32L",   NULL, "STM32L43x" },
  {      0x436, 0x0fff, "STM32L",   NULL, "STM32L1xx high-density" },
  {      0x437, 0x0fff, "STM32L",   NULL, "STM32L152RE" },
  {      0x438, 0x0fff, "STM32F",   NULL, "STM32F334" },
  {      0x439, 0x0fff, "STM32F",   NULL, "STM32F3xx small" },
  {      0x440, 0x0fff, "STM32F",   NULL, "STM32F0xx" },
  {      0x441, 0x0fff, "STM32F",   NULL, "STM32F412" },
  {      0x442, 0x0fff, "STM32F",   NULL, "STM32F09x" },
  {      0x444, 0x0fff, "STM32F",   NULL, "STM32F0xx small" },
  {      0x445, 0x0fff, "STM32F",   NULL, "STM32F04x" },
  {      0x446, 0x0fff, "STM32F",   NULL, "STM32F303 high-density" },
  {      0x447, 0x0fff, "STM32L",   NULL, "STM32L0xx cat. 5" },
  {      0x448, 0x0fff, "STM32F",   NULL, "STM32F0xx can" },
  {      0x449, 0x0fff, "STM32F",   NULL, "STM32F74xxx/F75xxx" },
  {      0x450, 0x0fff, "STM32H",   NULL, "STM32H7xxx" },
  {      0x451, 0x0fff, "STM32F",   NULL, "STM32F76xxx/77xxx" },
  {      0x452, 0x0fff, "STM32F",   NULL, "STM32F72xxx/73xxx" },
  {      0x457, 0x0fff, "STM32L",   NULL, "STM32L011" },
  {      0x458, 0x0fff, "STM32F",   NULL, "STM32F410" },
  {      0x463, 0x0fff, "STM32F",   NULL, "STM32F413" },

  {      0x410, 0x0fff, "GD32E",    NULL, "GD32E230" },
  {      0x410, 0x0fff, "GD32F",    NULL, "GD32F103" },
  {      0x414, 0x0fff, "GD32F",    NULL, "GD32F303" },

  { 0x00008A04,     ~0, "LPC",      NULL, "LPC8N04" },
  { 0x00008021,     ~0, "LPC",      NULL, "LPC802M001JDH20 - 16K Flash 2K SRAM" },
  { 0x00008022,     ~0, "LPC",      NULL, "LPC802M011JDH20 - 16K Flash 2K SRAM" },
  { 0x00008023,     ~0, "LPC",      NULL, "LPC802M001JDH16 - 16K Flash 2K SRAM" },
  { 0x00008024,     ~0, "LPC",      NULL, "LPC802M001JHI33 - 16K Flash 2K SRAM" },
  { 0x00008040,     ~0, "LPC",      NULL, "LPC804M101JBD64 - 32K Flash 4K SRAM" },
  { 0x00008041,     ~0, "LPC",      NULL, "LPC804M101JDH20" },
  { 0x00008042,     ~0, "LPC",      NULL, "LPC804M101JDH24" },
  { 0x00008043,     ~0, "LPC",      NULL, "LPC804M111JDH24" },
  { 0x00008044,     ~0, "LPC",      NULL, "LPC804M101JHI33" },
  { 0x00008100,     ~0, "LPC",      NULL, "LPC810M021FN8 - 4K Flash 1K SRAM" },
  { 0x00008110,     ~0, "LPC",      NULL, "LPC811M001JDH16 - 8K Flash 2K SRAM" },
  { 0x00008120,     ~0, "LPC",      NULL, "LPC812M101JDH16 - 16K Flash 4K SRAM" },
  { 0x00008121,     ~0, "LPC",      NULL, "LPC812M101JD20 - 16K Flash 4K SRAM" },
  { 0x00008122,     ~0, "LPC",      NULL, "LPC812M101JDH20 / LPC812M101JTB16 - 16K Flash 4K SRAM" },
  { 0x00008221,     ~0, "LPC",      NULL, "LPC822M101JHI33 - 16K Flash 4K SRAM" },
  { 0x00008222,     ~0, "LPC",      NULL, "LPC822M101JDH20" },
  { 0x00008241,     ~0, "LPC",      NULL, "LPC824M201JHI33 - 32K Flash 8K SRAM" },
  { 0x00008242,     ~0, "LPC",      NULL, "LPC824M201JDH20" },
  { 0x00008322,     ~0, "LPC",      NULL, "LPC832M101FDH20 - 16K Flash 4K SRAM" },
  { 0x00008341,     ~0, "LPC",      NULL, "LPC834M101FHI33 - 32K Flash 4K SRAM" },
  { 0x00008441,     ~0, "LPC",      NULL, "LPC844M201JBD64 - 64K Flash 8K SRAM" },
  { 0x00008442,     ~0, "LPC",      NULL, "LPC844M201JBD48" },
  { 0x00008443,     ~0, "LPC",      NULL, "LPC844M201JHI48, note UM11029 Rev.1.4 table 29 is wrong, see table 174 (in same manual)" },
  { 0x00008444,     ~0, "LPC",      NULL, "LPC844M201JHI33" },
  { 0x00008451,     ~0, "LPC",      NULL, "LPC845M301JBD64 - 64K Flash 16K SRAM" },
  { 0x00008452,     ~0, "LPC",      NULL, "LPC845M301JBD48" },
  { 0x00008453,     ~0, "LPC",      NULL, "LPC845M301JHI48" },
  { 0x00008454,     ~0, "LPC",      NULL, "LPC845M301JHI33" },
  { 0x0A07102B,     ~0, "LPC",      NULL, "LPC1110 - 4K Flash 1K SRAM" },
  { 0x1A07102B,     ~0, "LPC",      NULL, "LPC1110 - 4K Flash 1K SRAM" },
  { 0x0A16D02B,     ~0, "LPC",      NULL, "LPC1111/002 - 8K Flash 2K SRAM" },
  { 0x1A16D02B,     ~0, "LPC",      NULL, "LPC1111/002 - 8K Flash 2K SRAM" },
  { 0x041E502B,     ~0, "LPC",      NULL, "LPC1111/101 - 8K Flash 2K SRAM" },
  { 0x2516D02B,     ~0, "LPC",      NULL, "LPC1111/101/102 - 8K Flash 2K SRAM" },
  { 0x00010013,     ~0, "LPC",      NULL, "LPC1111/103 - 8K Flash 2K SRAM" },
  { 0x0416502B,     ~0, "LPC",      NULL, "LPC1111/201 - 8K Flash 4K SRAM" },
  { 0x2516902B,     ~0, "LPC",      NULL, "LPC1111/201/202 - 8K Flash 4K SRAM" },
  { 0x00010012,     ~0, "LPC",      NULL, "LPC1111/203 - 8K Flash 4K SRAM" },
  { 0x042D502B,     ~0, "LPC",      NULL, "LPC1112/101 - 16K Flash 2K SRAM" },
  { 0x0A24902B,     ~0, "LPC",      NULL, "LPC1112/102 - 16K Flash 4K SRAM" },
  { 0x1A24902B,     ~0, "LPC",      NULL, "LPC1112/102 - 16K Flash 4K SRAM" },
  { 0x0A23902B,     ~0, "LPC",      NULL, "LPC1112/102 - 16K Flash 4K SRAM" },
  { 0x1A23902B,     ~0, "LPC",      NULL, "LPC1112/102 - 16K Flash 4K SRAM" },
  { 0x2524D02B,     ~0, "LPC",      NULL, "LPC1112/101/102 - 16K Flash 2K SRAM" },
  { 0x00020023,     ~0, "LPC",      NULL, "LPC1112/103 - 16K Flash 2K SRAM" },
  { 0x0425502B,     ~0, "LPC",      NULL, "LPC1112/201 - 16K Flash 4K SRAM" },
  { 0x2524902B,     ~0, "LPC",      NULL, "LPC1112/201/202 - 16K Flash 4K SRAM" },
  { 0x00020022,     ~0, "LPC",      NULL, "LPC1112/203 - 16K Flash 4K SRAM" },
  { 0x0434502B,     ~0, "LPC",      NULL, "LPC1113/201 - 24K Flash 4K SRAM" },
  { 0x2532902B,     ~0, "LPC",      NULL, "LPC1113/201/202 - 24K Flash 4K SRAM" },
  { 0x00030032,     ~0, "LPC",      NULL, "LPC1113/203 - 24K Flash 4K SRAM" },
  { 0x0434102B,     ~0, "LPC",      NULL, "LPC1113/301 - 24K Flash 8K SRAM" },
  { 0x2532102B,     ~0, "LPC",      NULL, "LPC1113/301/302 - 24K Flash 8K SRAM" },
  { 0x00030030,     ~0, "LPC",      NULL, "LPC1113/303 - 24K Flash 8K SRAM" },
  { 0x0A40902B,     ~0, "LPC",      NULL, "LPC1114/102 - 32K Flash 4K SRAM" },
  { 0x1A40902B,     ~0, "LPC",      NULL, "LPC1114/102 - 32K Flash 4K SRAM" },
  { 0x0444502B,     ~0, "LPC",      NULL, "LPC1114/201 - 32K Flash 4K SRAM" },
  { 0x2540902B,     ~0, "LPC",      NULL, "LPC1114/201/202 - 32K Flash 4K SRAM" },
  { 0x00040042,     ~0, "LPC",      NULL, "LPC1114/203 - 32K Flash 4K SRAM" },
  { 0x0444102B,     ~0, "LPC",      NULL, "LPC1114/301 - 32K Flash 8K SRAM" },
  { 0x2540102B,     ~0, "LPC",      NULL, "LPC1114/301/302 & LPC11D14/302 - 32K Flash 8K SRAM" },
  { 0x00040040,     ~0, "LPC",      NULL, "LPC1114/303 - 32K Flash 8K SRAM" },
  { 0x00040060,     ~0, "LPC",      NULL, "LPC1114/323 - 48K Flash 8K SRAM" },
  { 0x00040070,     ~0, "LPC",      NULL, "LPC1114/333 - 56K Flash 8K SRAM" },
  { 0x00050080,     ~0, "LPC",      NULL, "LPC1115/303 - 64K Flash 8K SRAM" },
  { 0x1421102B,     ~0, "LPC",      NULL, "LPC11c12/301 - 16K Flash 8K SRAM" },
  { 0x1440102B,     ~0, "LPC",      NULL, "LPC11c14/301 - 32K Flash 8K SRAM" },
  { 0x1431102B,     ~0, "LPC",      NULL, "LPC11c22/301 - 16K Flash 8K SRAM" },
  { 0x1430102B,     ~0, "LPC",      NULL, "LPC11c24/301 - 32K Flash 8K SRAM" },
  { 0x095C802B,     ~0, "LPC",      NULL, "LPC11u12x/201 - 16K Flash 4K SRAM" },
  { 0x295C802B,     ~0, "LPC",      NULL, "LPC11u12x/201 - 16K Flash 4K SRAM" },
  { 0x097A802B,     ~0, "LPC",      NULL, "LPC11u13/201 - 24K Flash 4K SRAM" },
  { 0x297A802B,     ~0, "LPC",      NULL, "LPC11u13/201 - 24K Flash 4K SRAM" },
  { 0x0998802B,     ~0, "LPC",      NULL, "LPC11u14/201 - 32K Flash 4K SRAM" },
  { 0x2998802B,     ~0, "LPC",      NULL, "LPC11u14/201 - 32K Flash 4K SRAM" },
  { 0x2954402B,     ~0, "LPC",      NULL, "LPC11u22/301 - 16K Flash 6K SRAM" },
  { 0x2972402B,     ~0, "LPC",      NULL, "LPC11u23/301 - 24K Flash 6K SRAM" },
  { 0x2988402B,     ~0, "LPC",      NULL, "LPC11u24x/301 - 32K Flash 6K SRAM" },
  { 0x2980002B,     ~0, "LPC",      NULL, "LPC11u24x/401 - 32K Flash 8K SRAM" },
  { 0x0003D440,     ~0, "LPC",      NULL, "LPC11U34/311 - 40K Flash 8K SRAM" },
  { 0x0001cc40,     ~0, "LPC",      NULL, "LPC11U34/421 - 48K Flash 8K SRAM" },
  { 0x0001BC40,     ~0, "LPC",      NULL, "LPC11U35/401 - 64K Flash 8K SRAM" },
  { 0x0000BC40,     ~0, "LPC",      NULL, "LPC11U35/501 - 64K Flash 8K SRAM" },
  { 0x00019C40,     ~0, "LPC",      NULL, "LPC11U36/401 - 96K Flash 8K SRAM" },
  { 0x00017C40,     ~0, "LPC",      NULL, "LPC11U37FBD48/401 - 128K Flash 8K SRAM" },
  { 0x00007C44,     ~0, "LPC",      NULL, "LPC11U37HFBD64/401" },
  { 0x00007C40,     ~0, "LPC",      NULL, "LPC11U37FBD64/501" },
  { 0x1000002b,     ~0, "LPC",      NULL, "FX LPC11U6 - 256K Flash 32K SRAM" },
  { 0x2C42502B,     ~0, "LPC", "LPC1311", "LPC1311 - 8K Flash 4K SRAM" },
  { 0x1816902B,     ~0, "LPC", "LPC1311", "LPC1311/01 - 8K Flash 4K SRAM" },
  { 0x2C40102B,     ~0, "LPC", "LPC1313", "LPC1313 - 32K Flash 8K SRAM" },
  { 0x1830102B,     ~0, "LPC", "LPC1313", "LPC1313/01 - 32K Flash 8K SRAM" },
  { 0x3A010523,     ~0, "LPC", "LPC1315", "LPC1315 - 32K Flash 8K SRAM" },
  { 0x1A018524,     ~0, "LPC", "LPC1316", "LPC1316 - 48K Flash 8K SRAM" },
  { 0x1A020525,     ~0, "LPC", "LPC1317", "LPC1317 - 64K Flash 8K SRAM" },
  { 0x3D01402B,     ~0, "LPC", "LPC1342", "LPC1342 - 16K Flash 4K SRAM" },
  { 0x3D00002B,     ~0, "LPC", "LPC1343", "LPC1343 - 32K Flash 8K SRAM" },
  { 0x3000002B,     ~0, "LPC", "LPC1343", "LPC1343 - 32K Flash 8K SRAM" },
  { 0x28010541,     ~0, "LPC", "LPC1345", "LPC1345 - 32K Flash 8K SRAM" },
  { 0x08018542,     ~0, "LPC", "LPC1346", "LPC1346 - 48K Flash 8K SRAM" },
  { 0x08020543,     ~0, "LPC", "LPC1347", "LPC1347 - 64K Flash 8K SRAM" },
  { 0x00001517,     ~0, "LPC",      NULL, "LPC1517 - 64K Flash 12K SRAM" },
  { 0x00001518,     ~0, "LPC",      NULL, "LPC1518 - 128K Flash 20K SRAM" },
  { 0x00001519,     ~0, "LPC",      NULL, "LPC1519 - 256K Flash 36K SRAM" },
  { 0x00001547,     ~0, "LPC",      NULL, "LPC1547 - 64K Flash 12K SRAM" },
  { 0x00001548,     ~0, "LPC",      NULL, "LPC1548 - 128K Flash 20K SRAM" },
  { 0x00001549,     ~0, "LPC",      NULL, "LPC1549 - 256K Flash 36K SRAM" },
  { 0x26113F37,     ~0, "LPC",      NULL, "LPC1769" },
  { 0x26013F37,     ~0, "LPC",      NULL, "LPC1768" },
  { 0x26012837,     ~0, "LPC",      NULL, "LPC1767" },
  { 0x26013F33,     ~0, "LPC",      NULL, "LPC1766" },
  { 0x26013733,     ~0, "LPC",      NULL, "LPC1765" },
  { 0x26011922,     ~0, "LPC",      NULL, "LPC1764" },
  { 0x25113737,     ~0, "LPC",      NULL, "LPC1759" },
  { 0x25013F37,     ~0, "LPC",      NULL, "LPC1758" },
  { 0x25011723,     ~0, "LPC",      NULL, "LPC1756" },
  { 0x25011722,     ~0, "LPC",      NULL, "LPC1754" },
  { 0x25001121,     ~0, "LPC",      NULL, "LPC1752" },
  { 0x25001118,     ~0, "LPC",      NULL, "LPC1751" },
  { 0x25001110,     ~0, "LPC",      NULL, "LPC1751 (No CRP)" },
};

const MCUINFO *mcuinfo_lookup(const char *family, uint32_t id)
{
  assert(family != NULL);
  if (strlen(family) == 0 || id == 0)
    return NULL;

# if !defined NDEBUG
    for (int i = 0; i < sizearray(mcutable); i++) {
      uint32_t id1 = mcutable[i].partid & mcutable[i].maskid;
      const char *name1 = mcutable[i].prefix;
      for (int j = i + 1; j < sizearray(mcutable); j++) {
        uint32_t id2 = mcutable[j].partid & mcutable[j].maskid;
        const char *name2 = mcutable[j].prefix;
        assert(id1 != id2 || stricmp(name1, name2) != 0);
      }
    }
# endif

  for (int i = 0; i < sizearray(mcutable); i++) {
    if (mcutable[i].partid == (id & mcutable[i].maskid)
        && strnicmp(family, mcutable[i].prefix, strlen(mcutable[i].prefix)) == 0)
      return &mcutable[i];
  }

  return NULL;
}

