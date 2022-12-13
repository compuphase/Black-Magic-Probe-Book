/*
 * A utility to post-process ELF files for requirements of specific
 * micro-controllers. At this moment, the utility supports various ranges
 * of the LPC family by NXP.
 *
 * Copyright 2019-2022 CompuPhase
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
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"
#include "svnrev.h"


#define FLAG_HEADER   0x01
#define FLAG_MCU_LIST 0x02
#define FLAG_ALLINFO  0xff

static void usage(int flags)
{
  if (flags & FLAG_HEADER)
    printf("\nPostprocess an ELF file for requirements of specific micro-controllers.\n\n"
           "Usage: elf-postlink [mcu] [elf-file]\n\n");
  if (flags & FLAG_MCU_LIST)
    printf("MCU types:\n"
           "\tlpc8xx  - NXP LPC800, LPC810, LPC820, LPC830 and LPC840 Cortex-M0/M0+\n"
           "\t          series\n"
           "\tlpc11xx - NXP LPC1100, LPC11C00 and LPC11U00 Cortex-M0+ series\n"
           "\tlpc15xx - NXP LPC1500 Cortex-M3 series\n"
           "\tlpc17xx - NXP LPC1700 Cortex-M3 series\n"
           "\tlpc21xx - NXP LPC2100 ARM7TDMI series\n"
           "\tlpc22xx - NXP LPC2200 ARM7TDMI series\n"
           "\tlpc23xx - NXP LPC2300 ARM7TDMI series\n"
           "\tlpc24xx - NXP LPC2400 ARM7TDMI series\n"
           "\tlpc43xx - NXP LPC4300 Cortex-M4/M0 series\n");
}

static void version(void)
{
  printf("elf-postlink version %s.\n", SVNREV_STR);
  printf("Copyright 2019-2022 CompuPhase\nLicensed under the Apache License version 2.0\n");
}

int main(int argc, char *argv[])
{
  uint32_t chksum;
  int result, idx_file, idx_type;
  FILE *fp;

  if (argc == 2 && strcmp(argv[1], "-v") == 0) {
    version();
    return EXIT_SUCCESS;
  }
  if (argc != 3) {
    usage(FLAG_ALLINFO);
    return EXIT_SUCCESS;
  }

  idx_type = 1; /* assume correct order of command-line options */
  idx_file = 2;
  fp = fopen(argv[idx_file], "rb+");
  if (fp == NULL) {
    /* test for inverted order of options */
    fp = fopen(argv[idx_type], "rb+");
    if (fp == NULL) {
      /* failed too, this must be some error */
      printf("File \"%s\" could not be opened.\n\n", argv[idx_file]);
      usage(FLAG_ALLINFO);
      return 0;
    }
    /* file open worked at inverted order, assume inverted order then */
    idx_type = 2;
    idx_file = 1;
  }

  result=elf_patch_vecttable(fp, argv[idx_type], &chksum);
  fclose(fp);
  switch (result) {
  case ELFERR_NONE:
    printf("Checksum set to 0x%08x\n", chksum);
    break;
  case ELFERR_CHKSUMSET:
    printf("Checksum already correct (0x%08x)\n", chksum);
    break;
  case ELFERR_UNKNOWNDRIVER:
    printf("Unsupported MCU type \"%s\".\n", argv[idx_type]);
    usage(FLAG_MCU_LIST);
    break;
  case ELFERR_FILEFORMAT:
    printf("File \"%s\" has an unsupported format. A 32-bit ELF file is required\n", argv[idx_file]);
    usage(FLAG_HEADER);
    break;
  }

  return EXIT_SUCCESS;
}

