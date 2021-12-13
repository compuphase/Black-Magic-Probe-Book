/*
 * Functions for reading and parsing CMSIS SVD files with MCU-specific register
 * definitions.
 *
 * Copyright 2020-2021 CompuPhase
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
#ifndef _SVD_SUPPORT_H
#define _SVD_SUPPORT_H

void svd_clear(void);
int  svd_load(const char *filename);
int  svd_xlate_name(const char *symbol, char *alias, size_t alias_size);
int  svd_xlate_all_names(char *text, size_t maxsize);

const char *svd_mcu_prefix(void);
const char *svd_peripheral(unsigned index, unsigned long *address, const char **description);
const char *svd_register(const char *peripheral, unsigned index, unsigned long *offset,
                         int *range, const char **description);
const char *svd_bitfield(const char *peripheral, const char *regname, unsigned index,
                         short *low_bit, short *high_bit, const char **description);

int svd_lookup(const char *symbol, int index, const char **periph_name, const char **reg_name,
               unsigned long *address, const char **description);

#endif /* _SVD_SUPPORT_H */
