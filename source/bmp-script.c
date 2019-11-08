/*
 * General purpose "script" support for the Black Magic Probe, so that it can
 * automatically handle device-specific settings. It can use the GDB-RSP serial
 * interface, or the GDB-MI console interface.
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
#if defined _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #if defined _MSC_VER
    #define strdup(s)         _strdup(s)
    #define stricmp(s1,s2)    _stricmp((s1),(s2))
  #endif
#else
  #include <unistd.h>
  #include <bsd/string.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "bmp-script.h"

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif
#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

typedef struct tagREG_DEF {
  const char *name;
  uint32_t address;
  uint8_t size;
} REG_DEF;

typedef struct tagREG_SCRIPT {
  const char *name;
  const char *mcu_list;
  const char *script;
} REG_SCRIPT;

typedef struct tagREG_CACHE {
  const char *name;
  const char *mcu;
  const char *script_ptr;
} REG_CACHE;


static const REG_DEF registers[] = {
  { "SYSCON_SYSMEMREMAP",   0x40048000, 4 },  /**< LPC Cortex M0 series */
  { "SYSCON_SYSMEMREMAP_A", 0x40074000, 4 },  /**< LPC15xx series */
  { "SCB_MEMMAP_V6",        0x400FC040, 4 },  /**< LPC175x/176x series */
  { "SCB_MEMMAP_V4",        0xE01FC040, 4 },  /**< LPC ARM7TDMI series */
  { "M4MEMMAP",             0x40043100, 4 },  /**< LPC43xx series */

  { "RCC_APB2ENR",          0x40021018, 4 },  /**< STM32F1 APB2 Peripheral Clock Enable Register */
  { "AFIO_MAPR",            0x40010004, 4 },  /**< STM32F1 AF remap and debug I/O configuration */
  { "RCC_AHB1ENR",          0x40023830, 4 },  /**< STM32F4 AHB1 Peripheral Clock Enable Register */
  { "GPIOB_MODER",          0x40020400, 4 },  /**< STM32F4 GPIO Port B Mode Register */
  { "GPIOB_AFRL",           0x40020420, 4 },  /**< STM32F4 GPIO Port B Alternate Function Low Register */
  { "GPIOB_OSPEEDR",        0x40020408, 4 },  /**< STM32F4 GPIO Port B Output Speed Register */
  { "GPIOB_PUPDR",          0x4002040C, 4 },  /**< STM32F4 GPIO Port B Pull-Up/Pull-Down Register */
  { "DBGMCU_CR",            0xE0042004, 4 },  /**< STM32 Debug MCU Configuration Register */

  { "TRACECLKDIV_LPC13xx",  0x400480AC, 4 },
  { "TRACECLKDIV_LPC15xx",  0x400740D8, 4 },
  { "IOCON_PIO0_9_LPC13xx", 0x40044024, 4 },

  { "SCB_DHCSR",            0xE000EDF0, 4 },  /**< Debug Halting Control and Status Register */
  { "SCB_DCRSR",            0xE000EDF4, 4 },  /**< Debug Core Register Selector Register */
  { "SCB_DCRDR",            0xE000EDF8, 4 },  /**< Debug Core Register Data Register */
  { "SCB_DEMCR",            0xE000EDFC, 4 },  /**< Debug Exception and Monitor Control Register */

  { "TPIU_SSPSR",           0xE0040000, 4 },  /**< Supported Parallel Port Sizes Register */
  { "TPIU_CSPSR",           0xE0040004, 4 },  /**< Current Parallel Port Size Register */
  { "TPIU_ACPR",            0xE0040010, 4 },  /**< Asynchronous Clock Prescaler Register */
  { "TPIU_SPPR",            0xE00400F0, 4 },  /**< Selected Pin Protocol Register */
  { "TPIU_FFCR",            0xE0040304, 4 },  /**< Formatter and Flush Control Register */
  { "TPIU_DEVID",           0xE0040FC8, 4 },  /**< TPIU Type Register */

  { "DWT_CTRL",             0xE0001000, 4 },  /**< Control Register */
  { "DWT_CYCCNT",           0xE0001004, 4 },  /**< Cycle Count Register */

  { "ITM_TER",              0xE0000E00, 4 },  /**< Trace Enable Register */
  { "ITM_TPR",              0xE0000E40, 4 },  /**< Trace Privilege Register */
  { "ITM_TCR",              0xE0000E80, 4 },  /**< Trace Control Register */
  { "ITM_LAR",              0xE0000FB0, 4 },  /**< Lock Access Register */
  { "ITM_IWR",              0xE0000EF8, 4 },  /**< Integration Write Register */
  { "ITM_IRR",              0xE0000EFC, 4 },  /**< Integration Read Register */
  { "ITM_IMCR",             0xE0000F00, 4 },  /**< Integration Mode Control Register */
  { "ITM_LSR",              0xE0000FB4, 4 },  /**< Lock Status Register */
};

static const REG_SCRIPT scripts[] = {
  /* memory mapping (for Flash programming) */
  { "memremap", "lpc8xx,lpc11xx,lpc12xx,lpc13xx",
    "SYSCON_SYSMEMREMAP = 2"
  },
  { "memremap", "lpc15xx",
    "SYSCON_SYSMEMREMAP_A = 2"
  },
  { "memremap", "lpc17xx",
    "SCB_MEMMAP_V6 = 1"
  },
  { "memremap", "lpc21xx,lpc22xx,lpc23xx,lpc24xx",
    "SCB_MEMMAP_V4 = 1"
  },
  { "memremap", "LPC43xx Cortex-M4",
    "M4MEMMAP = 0"
  },

  /* MCU-specific & generic configuration for SWO tracing */
  { "swo-device", "STM32F1 medium density,STM32F1 high density",
    "RCC_APB2ENR |= 1 \n"
    "AFIO_MAPR |= 0x2000000 \n" /* 2 << 24 */
    "DBGMCU_CR |= 0x20 \n"      /* 1 << 5 */
  },
  { "swo-device", "STM32F3,STM32F03,STM32F05,STM32F07,STM32F09,STM32F2",
    "DBGMCU_CR |= 0x20 \n"      /* 1 << 5 */
  },
  { "swo-device", "STM32F4,STM32F7",
    "RCC_AHB1ENR |= 0x02 \n"    /* enable GPIOB clock */
    "GPIOB_MODER ~= 0x00c0 \n"  /* PB3: use alternate function */
    "GPIOB_MODER |= 0x0080 \n"
    "GPIOB_AFRL ~= 0xf000 \n"   /* set AF0 (==TRACESWO) on PB3 */
    "GPIOB_OSPEEDR |= 0x00c0 \n"/* set max speed on PB3 */
    "GPIOB_PUPDR ~= 0x00c0 \n"  /* no pull-up or pull-down on PB3 */
    "DBGMCU_CR |= 0x20 \n"      /* 1 << 5 */
  },
  { "swo-device", "LPC13xx",
     "TRACECLKDIV_LPC13xx = 1 \n"
     "IOCON_PIO0_9_LPC13xx = 0x93 \n"
  },
  { "swo-device", "LPC15xx",
    "TRACECLKDIV_LPC13xx = 1\n"
    /* LPC_SWM->PINASSIGN15 = (LPC_SWM->PINASSIGN15 & ~(0xff << 8)) | (pin << 8); */
  },

  { "swo-generic", "*",
    "SCB_DEMCR = 0x1000000 \n"   /* 1 << 24 */
    "TPIU_CSPSR = 1 \n"          /* protocol width = 1 bit */
    "TPIU_SSPSR = $0 \n"         /* 1 = Manchester, 2 = Asynchronous */
    "TPIU_ACPR = $1 \n"          /* CPU clock divider */
    "TPIU_FFCR = 0 \n"           /* turn off formatter, discard ETM output */
    "ITM_LAR = 0xC5ACCE55 \n"    /* unlock access to ITM registers */
    "ITM_TCR = 0x11 \n"          /* (1 << 4) | 1 */
    "ITM_TPR = 0 \n"             /* privileged access is off */
  },

  { "swo-channels", "*",
    "ITM_TER = $0"              /* enable stimulus channel(s) */
  },

  /* ----- */
  { NULL, NULL, NULL }
};


static REG_CACHE cache = { NULL, NULL, NULL };


/** clearcache() clears the cache for the script mist recently found. It
 *  is needed if you want to run the same script on the same MCU a second
 *  time. If the cache is not cleared in between, scriptline() would return
 *  false (for end of script reached) immediately.
 */
void bmscript_clearcache(void)
{
  if (cache.name != NULL)
    free((void*)cache.name);
  if (cache.mcu != NULL)
    free((void*)cache.mcu);
  cache.name = NULL;
  cache.mcu = NULL;
  cache.script_ptr = NULL;
}

/** bmp_scriptline() returns the next instruction from a script for a specific
 *  micro-controller. When this function is called with a new script name or a
 *  new mcu name, the first instruction for the requested script that matches
 *  the given mcu is returned. For every next call with the same parameters, the
 *  next instruction is returned, until the script completes.
 *
 *  \param name     The name of te script; may be set to NULL to continue on the
 *                  last active script.
 *  \param mcu      The name of the MCU; may be set to NULL to use the MCU that
 *                  was previously set.
 *  \param oper     The operation code, should be '=', '|' or '~'.
 *  \param address  The address of the register or memory location to set.
 *  \param value    The value to set the register or memory location to.
 *  \param size     The size of the register in bytes.
 *
 *  \return 1 of success, 0 on failure. Failure can mean that no script matches,
 *          or that the script contains no more instructions.
 *
 *  \note The script can be for a specific device or it can be a generic script.
 *        In this last case, the script has a "*" in its device list.
 *
 *        Each line in the script has a register/memory setting (it is assumed
 *        that registers are memory-mapped). The setting consists of an address,
 *        a value, a size, and an operator. The size is typically 4 (32-bit
 *        reisters), but may be 1 or 2 as well. The operator is '=' for a simple
 *        assignment ("value" is stored as "address"), '|' to set bits in the
 *        current register value, and '~' to clear bits. For the last option: a
 *        1 bit in value, clears that bit im the register (so it is an AND with
 *        the inverse of "value").
 */
int bmscript_line(const char *name, const char *mcu, char *oper,
                  uint32_t *address, uint32_t *value, uint8_t *size)
{
  const char *head;
  int idx;

  if (name == NULL)
    name = cache.name;
  if (mcu == NULL)
    mcu = cache.mcu;
  assert(name != NULL && mcu != NULL);
  assert(oper != NULL && address != NULL && value != NULL && size != NULL);

  if (cache.name == NULL || strcmp(name, cache.name) != 0 || cache.mcu == NULL || strcmp(mcu, cache.mcu) != 0) {
    /* find a script with the given name, where the MCU is in the list */
    for (idx = 0; scripts[idx].name != NULL; idx++) {
      if (stricmp(name, scripts[idx].name) == 0) {
        char *list;
        /* check whether this is a generic script (valid for all) */
        if (scripts[idx].mcu_list[0] == '*')
          break;  /* no need to search for a matching MCU */
        /* check whether the MCU is in the list of MCUs */
        list = strdup(scripts[idx].mcu_list);
        if (list != NULL) {
          char *tok, *space;
          for (tok = strtok(list, ","); tok != NULL; tok = strtok(NULL, ",")) {
            if (stricmp(mcu, tok) == 0)
              break;
            /* also check whether the CPU architecture is added to the name */
            if ((space = strrchr(tok, ' ')) != NULL && space[1] == 'M' && isdigit(space[2])) {
              *space = '\0';
              if (stricmp(mcu, tok) == 0)
                break;
              *space = ' '; /* restore string (although probably redundant) */
            }
          }
          free((void*)list);
          if (tok != NULL)
            break;  /* script with matching name and mcu is found -> don't look further */
        }
      }
    }
    if (scripts[idx].name == NULL)
      return 0;     /* no script with matching name and mcu is found */

    if (cache.name != NULL)
      free((void*)cache.name);
    if (cache.mcu != NULL)
      free((void*)cache.mcu);
    cache.name = strdup(name);
    cache.mcu = strdup(mcu);
    cache.script_ptr = scripts[idx].script;
  }

  assert(cache.script_ptr != NULL);
  while (*cache.script_ptr != '\0' && *cache.script_ptr <= ' ')
    cache.script_ptr += 1;
  if (*cache.script_ptr == '\0')
    return 0; /* end of script reached */

  /* parse the line */
  head = cache.script_ptr;
  assert(*head > ' ');
  if (isdigit(*head)) {
    *address = strtoul(head, (char**)&head, 0);
    *size = 4;
  } else {
    const char *tail;
    for (tail = head; isalnum(*tail) || *tail == '_'; tail++)
      /* nothing */;
    for (idx = 0; idx < sizearray(registers) && strncmp(head, registers[idx].name, (tail - head)) != 0; idx++)
      /* nothing */;
    assert(idx < sizearray(registers));
    *address = registers[idx].address;
    *size = registers[idx].size;
    head = tail;
  }
  while (*head != '\0' && *head <= ' ')
    head += 1;
  assert(*head == '=' || *head == '|' || *head == '~');
  *oper = *head;
  head += 1;
  if (*head == '=')
    head += 1;    /* allow |= to mean | and ~= to mean ~ */
  while (*head != '\0' && *head <= ' ')
    head += 1;

  if (*head == '$') {
    *value = SCRIPT_MAGIC + (head[1] - '0');
    head += 2;
  } else {
    *value = strtoul(head, (char**)&head, 0);
  }
  while (*head != '\0' && *head != '\n' && *head <= ' ')
    head += 1;
  assert(*head == '\n' || *head == '\0');
  if (*head == '\n')
    head += 1;
  cache.script_ptr = head;

  return 1;
}

int bmscript_line_fmt(const char *name, const char *mcu, char *line, const unsigned long *params)
{
  char oper;
  uint32_t address, value;
  uint8_t size;
  if (bmscript_line(name, mcu, &oper, &address, &value, &size)) {
    char operstr[10];
    switch (oper) {
    case '=':
      strcpy(operstr, "=");
      break;
    case '|':
      strcpy(operstr, "|=");
      break;
    case '~':
      strcpy(operstr, "&= ~");
      break;
    default:
      assert(0);
    }
    if ((value & ~0xf) == SCRIPT_MAGIC) {
      assert(params != NULL);
      value = (uint32_t)params[value & 0xf];  /* replace parameters */
    }
    switch (size) {
    case 1:
      sprintf(line, "set {char}0x%x %s 0x%x\n", address, operstr, value & 0xff);
      break;
    case 2:
      sprintf(line, "set {short}0x%x %s 0x%x\n", address, operstr, value & 0xffff);
      break;
    case 4:
      sprintf(line, "set {int}0x%x %s 0x%x\n", address, operstr, value);
      break;
    default:
      assert(0);
    }
    return 1;
  }
  return 0;
}

