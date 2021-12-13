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

#ifndef _CKSUM_H
#define _CKSUM_H

#include <stdint.h>
#include <stdio.h>

#if defined __cplusplus
  extern "C" {
#endif

uint32_t cksum(FILE *fp);

#if defined __cplusplus
  }
#endif

#endif /* _CKSUM_H */

