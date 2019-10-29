/*
 * A utility to post-process ELF files for requirements of specific
 * micro-controllers. At this moment, the utility supports various ranges
 * of the LPC family by NXP.
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
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"


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
           "\tlpc8xx  - NXP LPC810 and LPC820 Cortex-M0 series\n"
           "\tlpc11xx - NXP LPC1100, LPC11C00 and LPC11U00 Cortex-M0+ series\n"
           "\tlpc15xx - NXP LPC1500 Cortex-M3 series\n"
           "\tlpc17xx - NXP LPC1700 Cortex-M3 series\n"
           "\tlpc21xx - NXP LPC2100 ARM7TDMI series\n"
           "\tlpc22xx - NXP LPC2200 ARM7TDMI series\n"
           "\tlpc23xx - NXP LPC2300 ARM7TDMI series\n"
           "\tlpc24xx - NXP LPC2400 ARM7TDMI series\n"
           "\tlpc43xx - NXP LPC4300 Cortex-M4/M0 series\n");
}

int main(int argc,char *argv[])
{
  uint32_t chksum;
  int result;
  FILE *fp;

  if (argc!=3) {
    usage(FLAG_ALLINFO);
    return 1;
  }

  fp=fopen(argv[2],"rb+");
  if (fp==NULL) {
    printf("File \"%s\" could not be opened.\n",argv[2]);
    return 0;
  }

  result=elf_patch_vecttable(fp,argv[1],&chksum);
  fclose(fp);
  switch (result) {
  case ELFERR_NONE:
    printf("Checksum set to 0x%08x\n",chksum);
    break;
  case ELFERR_CHKSUMSET:
    printf("Checksum already correct (0x%08x)\n",chksum);
    break;
  case ELFERR_UNKNOWNDRIVER:
    printf("Unsupported MCU type \"%s\".\n",argv[1]);
    usage(FLAG_MCU_LIST);
    break;
  case ELFERR_FILEFORMAT:
    printf("File \"%s\" has an unsupported format. A 32-bit ELF file is required\n",argv[2]);
    usage(FLAG_HEADER);
    break;
  }

  return 0;
}

