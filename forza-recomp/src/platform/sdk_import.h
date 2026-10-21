// forza - the SDK's own implementation of a kernel/XAM import we override.
//
// Our `extern "C" REX_FUNC(__imp__Name)` definitions take precedence over the runtime's
// for the generated code. When an override only adjusts the SDK's behaviour it calls the
// original: found with dlsym(RTLD_NEXT) on Linux (ELF interposition) and with
// GetProcAddress in rexruntime.dll on Windows, which exports the same `__imp__` names.

#pragma once

#include <rex/ppc/context.h>

#include <cstdint>

namespace forza {

// Optional C entry point in the runtime, or null (also used by non-guest SDK extensions).
void* SdkSymbol(const char* name);

using GuestFunc = void (*)(PPCContext&, uint8_t*);

// The runtime's `__imp__Name` (pass the full symbol name), or null.
GuestFunc SdkImport(const char* name);

}  // namespace forza
