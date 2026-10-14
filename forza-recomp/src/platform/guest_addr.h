// forza - guest addresses for the executable this build recompiles.
//
// Hooks name guest functions (sub_XXXXXXXX) and addresses as they are in the disc's
// default.xex. A Title Update 4 build (FORZA_TITLE_UPDATE=4, docs/dlc.md) includes the
// table that tools/tu_address_map.py writes (tu4_addresses.h): it renames those functions
// to their TU4 names and maps FORZA_ADDR(base address) to the TU4 address. Include this
// before any use of either.
#pragma once

#include <cstdint>

#if defined(FORZA_TITLE_UPDATE) && FORZA_TITLE_UPDATE == 4
#include "tu4_addresses.h"
#define FORZA_ADDR(a) (::forza::Tu4Address(a))
#else
#define FORZA_ADDR(a) (a)
#endif
