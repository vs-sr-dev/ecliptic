// PACE-lite: a minimal 68k Palm OS environment (Unicorn m68k + HLE system traps).
#pragma once
#include "palmos.h"

namespace m68k {
void init();
// Runs the application's 68k code (#1) as a normal launch. Returns false if it has none.
bool run_app(Db* app);
bool faulted();   // the 68k code crashed
// Call68KFunc: trapOrFunction <= 0xFFF is a system trap number, otherwise a 68k function.
// argsP holds argsSize bytes of 68k stack arguments (big endian).
uint32_t call68k(uint32_t trapOrFunction, uint32_t argsP, uint32_t argsSizeAndWantA0);
}
