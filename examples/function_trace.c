/* Example functions for function enter/exit tracing via GCC instrumentation.
 * It is based on SWO tracing, and should be used with the function_trace.tsdl
 * file.
 *
 * Routines for initializing the micro-controller for TRACESWO are not included,
 * as these are (in part) dependend on the particular micro-controller.
 *
 *
 * Copyright 2022 CompuPhase
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <stdint.h>

#if !defined ITM_TCR_ITMENA
  #define ITM_TCR_ITMENA ITM_TCR_ITMENA_Msk
#endif

__attribute__((no_instrument_function))
void trace_xmit(int stream_id, const unsigned char *data, unsigned size)
{
  if ((ITM->TCR & ITM_TCR_ITMENA) != 0UL &&   /* ITM tracing enabled */
      (ITM->TER & (1 << channel)) != 0UL)     /* ITM channel enabled */
  {
    /* collect and transmit bytes in packets of 4 bytes */
    uint32_t value = 0, shift = 0;
    while (size-- > 0) {
      value |= (uint32_t)*data++ << shift;
      shift += 8;
      if (shift >= 32) {
        /* in the waiting loop, use an empty statement and not __NOP();
           although __NOP() is an inline function, it would still be
           instrumented */
        while (ITM->PORT[channel].u32 == 0UL)
          {}
        ITM->PORT[channel].u32 = value;
        value = shift = 0;
      }
    }
    /* transmit last collected bytes */
    if (shift > 0) {
      while (ITM->PORT[channel].u32 == 0UL)
        {}
      ITM->PORT[channel].u32 = value;
    }
  }
}

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *this_fn, void *call_site)
{
  (void)call_site;
  trace_function_profile_enter((unsigned long)this_fn);
}
__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *this_fn, void *call_site)
{
  (void)call_site;
  trace_function_profile_exit((unsigned long)this_fn);
}

