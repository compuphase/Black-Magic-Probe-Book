/*
 * Common functions for bmdebug, bmflash, bmprofile and bmtrace.
 *
 * Copyright 2021-2023 CompuPhase
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
#ifndef _BMCOMMON_H
#define _BMCOMMON_H

const char **get_probelist(int *probe, int *netprobe);
void clear_probelist(const char **probelist, int netprobe);

bool get_configfile(char *filename, size_t maxsize, const char *basename);

#endif /* _BMCOMMON_H */
