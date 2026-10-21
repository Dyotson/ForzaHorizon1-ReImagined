// forza - the SDK's own implementation of an import we override (see sdk_import.h).

#include "sdk_import.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace forza {

void* SdkSymbol(const char* name) {
#ifdef _WIN32
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return runtime ? reinterpret_cast<void*>(GetProcAddress(runtime, name)) : nullptr;
#else
  return dlsym(RTLD_NEXT, name);
#endif
}

GuestFunc SdkImport(const char* name) {
  return reinterpret_cast<GuestFunc>(SdkSymbol(name));
}

}  // namespace forza
