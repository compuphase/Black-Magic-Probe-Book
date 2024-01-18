/*
 * Simple serial monitor (receive data from a serial port).
 *
 * Copyright 2021-2024 CompuPhase
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
#ifndef _SERIALMON_H
#define _SERIALMON_H

bool   sermon_open(const char *port, int baud);
void   sermon_close(void);
bool   sermon_isopen(void);

void   sermon_clear(void);
int    sermon_countlines(void);
void   sermon_rewind(void);
const char *sermon_next(bool *is_error, int *severity, int *streamid);

const char *sermon_getport(int translated);
int    sermon_getbaud(void);

void sermon_setmetadata(const char *tsdlfile);
const char *sermon_getmetadata(void);

void sermon_statusmsg(const char *message, bool is_error);
int sermon_save(const char *filename, bool csvformat);

#endif /* _SERIALMON_H */
