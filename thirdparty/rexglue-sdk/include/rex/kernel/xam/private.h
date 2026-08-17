#pragma once
/**
 * ReXGlue runtime - AC6 Recompilation project
 * Copyright (c) 2026 Tom Clay. All rights reserved.
 */

#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

bool xeXamIsUIActive();

// Profile-alias compat shim (ac6_profile_always_signed_in): while the cvar is on,
// user indices 1-3 alias to 0 - the one synthetic profile answers for any
// slot the player's pad landed on. Index 0 and invalid indices (>= 4) always
// pass through. Defined in xam_user.cpp; declared here for the unit test.
uint32_t XamEffectiveUserIndex(uint32_t user_index);

rex::runtime::Export* RegisterExport_xam(rex::runtime::Export* export_entry);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                       \
  void Register##n##Exports(rex::runtime::ExportResolver* export_resolver, \
                            system::KernelState* kernel_state);
#include "module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace rex
