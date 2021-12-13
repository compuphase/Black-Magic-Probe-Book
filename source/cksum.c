/*
 * Calculates and prints the POSIX checksums and sizes of files.
 * This is a re-implementation of the cksum program provided with Unix
 * and Linux distributions.
 *
 * For the original, see:
 * - http://www.gnu.org/software/coreutils/
 * - http://gnuwin32.sourceforge.net/packages/coreutils.htm
 *
 * In contrast to the original, this implementation:
 * - does not support any options, except --help
 * - has no option to read from stdin (so does not support calculating
 *   a checksum from data "piped" in through redirection)
 * - does not support wild-cards (file globbing) on Windows
 *
 * Note that the FreeBSD implementation of cksum is different from the
 * version of Unix and Linux, in at least two ways:
 * - the FreeBSD implementation offers the choice of three checksum
 *   algorithms (one of which is CRC32), whereas the Linux version only
 *   offers CRC32
 * - the Linux version includes the value of the file length (in bytes)
 *   in the CRC (exactly like Wikipedia describes the algorithm), but
 *   the FreeBSD version only uses the file contents for the CRC32.
 * The net result is that the cksum program in FreeBSD gives a different
 * result than Linux, when run on the same file.
 *
 * Copyright 2021 CompuPhase
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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "cksum.h"

static uint32_t const crc_table[256] = {
  0x00000000Lu, 0x04c11db7Lu, 0x09823b6eLu, 0x0d4326d9Lu,
  0x130476dcLu, 0x17c56b6bLu, 0x1a864db2Lu, 0x1e475005Lu,
  0x2608edb8Lu, 0x22c9f00fLu, 0x2f8ad6d6Lu, 0x2b4bcb61Lu,
  0x350c9b64Lu, 0x31cd86d3Lu, 0x3c8ea00aLu, 0x384fbdbdLu,
  0x4c11db70Lu, 0x48d0c6c7Lu, 0x4593e01eLu, 0x4152fda9Lu,
  0x5f15adacLu, 0x5bd4b01bLu, 0x569796c2Lu, 0x52568b75Lu,
  0x6a1936c8Lu, 0x6ed82b7fLu, 0x639b0da6Lu, 0x675a1011Lu,
  0x791d4014Lu, 0x7ddc5da3Lu, 0x709f7b7aLu, 0x745e66cdLu,
  0x9823b6e0Lu, 0x9ce2ab57Lu, 0x91a18d8eLu, 0x95609039Lu,
  0x8b27c03cLu, 0x8fe6dd8bLu, 0x82a5fb52Lu, 0x8664e6e5Lu,
  0xbe2b5b58Lu, 0xbaea46efLu, 0xb7a96036Lu, 0xb3687d81Lu,
  0xad2f2d84Lu, 0xa9ee3033Lu, 0xa4ad16eaLu, 0xa06c0b5dLu,
  0xd4326d90Lu, 0xd0f37027Lu, 0xddb056feLu, 0xd9714b49Lu,
  0xc7361b4cLu, 0xc3f706fbLu, 0xceb42022Lu, 0xca753d95Lu,
  0xf23a8028Lu, 0xf6fb9d9fLu, 0xfbb8bb46Lu, 0xff79a6f1Lu,
  0xe13ef6f4Lu, 0xe5ffeb43Lu, 0xe8bccd9aLu, 0xec7dd02dLu,
  0x34867077Lu, 0x30476dc0Lu, 0x3d044b19Lu, 0x39c556aeLu,
  0x278206abLu, 0x23431b1cLu, 0x2e003dc5Lu, 0x2ac12072Lu,
  0x128e9dcfLu, 0x164f8078Lu, 0x1b0ca6a1Lu, 0x1fcdbb16Lu,
  0x018aeb13Lu, 0x054bf6a4Lu, 0x0808d07dLu, 0x0cc9cdcaLu,
  0x7897ab07Lu, 0x7c56b6b0Lu, 0x71159069Lu, 0x75d48ddeLu,
  0x6b93dddbLu, 0x6f52c06cLu, 0x6211e6b5Lu, 0x66d0fb02Lu,
  0x5e9f46bfLu, 0x5a5e5b08Lu, 0x571d7dd1Lu, 0x53dc6066Lu,
  0x4d9b3063Lu, 0x495a2dd4Lu, 0x44190b0dLu, 0x40d816baLu,
  0xaca5c697Lu, 0xa864db20Lu, 0xa527fdf9Lu, 0xa1e6e04eLu,
  0xbfa1b04bLu, 0xbb60adfcLu, 0xb6238b25Lu, 0xb2e29692Lu,
  0x8aad2b2fLu, 0x8e6c3698Lu, 0x832f1041Lu, 0x87ee0df6Lu,
  0x99a95df3Lu, 0x9d684044Lu, 0x902b669dLu, 0x94ea7b2aLu,
  0xe0b41de7Lu, 0xe4750050Lu, 0xe9362689Lu, 0xedf73b3eLu,
  0xf3b06b3bLu, 0xf771768cLu, 0xfa325055Lu, 0xfef34de2Lu,
  0xc6bcf05fLu, 0xc27dede8Lu, 0xcf3ecb31Lu, 0xcbffd686Lu,
  0xd5b88683Lu, 0xd1799b34Lu, 0xdc3abdedLu, 0xd8fba05aLu,
  0x690ce0eeLu, 0x6dcdfd59Lu, 0x608edb80Lu, 0x644fc637Lu,
  0x7a089632Lu, 0x7ec98b85Lu, 0x738aad5cLu, 0x774bb0ebLu,
  0x4f040d56Lu, 0x4bc510e1Lu, 0x46863638Lu, 0x42472b8fLu,
  0x5c007b8aLu, 0x58c1663dLu, 0x558240e4Lu, 0x51435d53Lu,
  0x251d3b9eLu, 0x21dc2629Lu, 0x2c9f00f0Lu, 0x285e1d47Lu,
  0x36194d42Lu, 0x32d850f5Lu, 0x3f9b762cLu, 0x3b5a6b9bLu,
  0x0315d626Lu, 0x07d4cb91Lu, 0x0a97ed48Lu, 0x0e56f0ffLu,
  0x1011a0faLu, 0x14d0bd4dLu, 0x19939b94Lu, 0x1d528623Lu,
  0xf12f560eLu, 0xf5ee4bb9Lu, 0xf8ad6d60Lu, 0xfc6c70d7Lu,
  0xe22b20d2Lu, 0xe6ea3d65Lu, 0xeba91bbcLu, 0xef68060bLu,
  0xd727bbb6Lu, 0xd3e6a601Lu, 0xdea580d8Lu, 0xda649d6fLu,
  0xc423cd6aLu, 0xc0e2d0ddLu, 0xcda1f604Lu, 0xc960ebb3Lu,
  0xbd3e8d7eLu, 0xb9ff90c9Lu, 0xb4bcb610Lu, 0xb07daba7Lu,
  0xae3afba2Lu, 0xaafbe615Lu, 0xa7b8c0ccLu, 0xa379dd7bLu,
  0x9b3660c6Lu, 0x9ff77d71Lu, 0x92b45ba8Lu, 0x9675461fLu,
  0x8832161aLu, 0x8cf30badLu, 0x81b02d74Lu, 0x857130c3Lu,
  0x5d8a9099Lu, 0x594b8d2eLu, 0x5408abf7Lu, 0x50c9b640Lu,
  0x4e8ee645Lu, 0x4a4ffbf2Lu, 0x470cdd2bLu, 0x43cdc09cLu,
  0x7b827d21Lu, 0x7f436096Lu, 0x7200464fLu, 0x76c15bf8Lu,
  0x68860bfdLu, 0x6c47164aLu, 0x61043093Lu, 0x65c52d24Lu,
  0x119b4be9Lu, 0x155a565eLu, 0x18197087Lu, 0x1cd86d30Lu,
  0x029f3d35Lu, 0x065e2082Lu, 0x0b1d065bLu, 0x0fdc1becLu,
  0x3793a651Lu, 0x3352bbe6Lu, 0x3e119d3fLu, 0x3ad08088Lu,
  0x2497d08dLu, 0x2056cd3aLu, 0x2d15ebe3Lu, 0x29d4f654Lu,
  0xc5a92679Lu, 0xc1683bceLu, 0xcc2b1d17Lu, 0xc8ea00a0Lu,
  0xd6ad50a5Lu, 0xd26c4d12Lu, 0xdf2f6bcbLu, 0xdbee767cLu,
  0xe3a1cbc1Lu, 0xe760d676Lu, 0xea23f0afLu, 0xeee2ed18Lu,
  0xf0a5bd1dLu, 0xf464a0aaLu, 0xf9278673Lu, 0xfde69bc4Lu,
  0x89b8fd09Lu, 0x8d79e0beLu, 0x803ac667Lu, 0x84fbdbd0Lu,
  0x9abc8bd5Lu, 0x9e7d9662Lu, 0x933eb0bbLu, 0x97ffad0cLu,
  0xafb010b1Lu, 0xab710d06Lu, 0xa6322bdfLu, 0xa2f33668Lu,
  0xbcb4666dLu, 0xb8757bdaLu, 0xb5365d03Lu, 0xb1f740b4Lu
};

/* update the CRC with the data in the buffer;
   the "crc" parameter is the initial value (the value returned from
   the previous call, or 0 on the very first call) */
static uint32_t update_crc(const unsigned char *buffer, size_t size, uint32_t crc)
{
  while (size--)
    crc = (crc << 8) ^ crc_table[((crc >> 24) ^ *buffer++) & 0xff];
  return crc;
}

uint32_t cksum(FILE *fp)
{
  unsigned char buffer[256];
  uint32_t crc = 0;
  unsigned long total_length = 0;
  int count;

  assert(fp != NULL);
  rewind(fp);
  do {
    count = fread(buffer, sizeof(unsigned char), sizeof buffer, fp);
    total_length += count;
    crc = update_crc(buffer, count, crc);
  } while (count == sizeof buffer);

  /* append file length (binary) to CRC */
  for (count = 0; total_length != 0; total_length >>= 8, count++)
    buffer[count]=(unsigned char)(total_length & 0xff);
  crc = update_crc(buffer, count, crc);

  return ~crc & 0xffffffff;
}


#if defined STANDALONE

#if !defined STREQ
  #define STREQ(s1, s2)  (stricmp((s1), (s2)) == 0)
#endif

static void usage(int status)
{
  printf("cksum - show CRC checksum and byte count of each file.\n\n"
         "Usage: cksum [filename] [...]\n\n");
  exit(status);
}

int main(int argc, char **argv)
{
  int i;
  int errors = 0;

  if (argc <= 1)
    usage(EXIT_FAILURE);

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      if (STREQ(argv[i], "-?") || STREQ(argv[i], "-h") || STREQ(argv[i], "--help")) {
        usage(EXIT_SUCCESS);
      } else {
        fprintf(stderr, "Invalid option \"%s\", use --help to see the syntax\n\n", argv[i]);
        usage(EXIT_FAILURE);
      }
    } else {
      FILE *fp = fopen(argv[i], "rb");
      if (fp != NULL) {
        uint32_t crc = cksum(fp);
        unsigned long filelength = ftell(fp);
        fclose(fp);
        /* print the information in a way compatible with the POSIX cksum program */
        printf("%10lu %10lu   %s\n", (unsigned long)crc, filelength, argv[i]);
      } else {
        fprintf(stderr, "Failed to open \"%s\", error %d\n", argv[i], errno);
        errors += 1;
      }
    }
  }

  return (errors == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* STANDALONE */

