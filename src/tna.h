// Tapwave Native Application module loader (TwLoadModule / PceNativeCall).
#pragma once
#include "palmos.h"

namespace tna {
void init(uint32_t call68k);                    // Call68KFunc trap address passed to modules
bool has_native_code(Db* db, uint16_t id);
// Copies/gunzips the code resource, then runs its initialisation call. Returns the entry point.
uint32_t load(Db* db, uint32_t rsrcType, uint16_t id);
uint32_t run(uint32_t entry, uint32_t userData); // PceNativeCall(entry, userData)
bool launched();                                // a module has been run
}
