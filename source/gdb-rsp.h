/*
 * The GDB "Remote Serial Protocol" support.
 *
 * Copyright 2019-2023 CompuPhase
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
#ifndef _GDB_RSP_H
#define _GDB_RSP_H

#if defined __cplusplus
  extern "C" {
#endif

bool   gdbrsp_hex2array(const char *hex, unsigned char *byte, size_t size);
void   gdbrsp_packetsize(size_t size);
size_t gdbrsp_recv(char *buffer, size_t size, int timeout);
bool   gdbrsp_xmit(const char *buffer, int size);
void   gdbrsp_clear(void);

#if defined __cplusplus
  }
#endif

#endif /* _GDB_RSP_H */

