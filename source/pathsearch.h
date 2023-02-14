/*
 * Searching the path for a filename.
 *
 * Copyright 2023 CompuPhase
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
#ifndef _PATHSEARCH_H
#define _PATHSEARCH_H

#include <stdbool.h>

bool pathsearch(char *buffer, size_t bufsize, const char *filename);

#endif /* _PATHSEARCH_H */
