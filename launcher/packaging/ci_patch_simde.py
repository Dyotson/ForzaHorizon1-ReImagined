#!/usr/bin/env python3
"""simde (an SDK submodule) stores an MMX value with a builtin that clang >= 23 rejects on MSVC's
union __m64; take its plain store there instead (the same edit tools/build_windows.sh makes)."""
import sys

path = sys.argv[1]
text = open(path).read()
old = "#elif HEDLEY_HAS_BUILTIN(__builtin_nontemporal_store) && ( \\\n      defined(SIMDE_ARM_NEON_A32V7_NATIVE) || defined(SIMDE_MIPS_LOONGSON_MMI_NATIVE)"
new = "#elif HEDLEY_HAS_BUILTIN(__builtin_nontemporal_store) && !defined(SIMDE_X86_MMX_USE_NATIVE_TYPE) && ( \\\n      defined(SIMDE_ARM_NEON_A32V7_NATIVE) || defined(SIMDE_MIPS_LOONGSON_MMI_NATIVE)"
if old in text:
    open(path, "w").write(text.replace(old, new, 1))
    print("patched", path)
