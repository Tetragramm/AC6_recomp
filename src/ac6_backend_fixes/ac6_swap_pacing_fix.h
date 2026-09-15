#pragma once

#include <cstdint>

namespace rex::graphics {
class GraphicsSystem;
}

namespace ac6::backend {

// Registers the flip-on-fence hook on the graphics system. Inert unless
// ac6_swap_flip_on_fence is on.
void InstallSwapPacingFix(rex::graphics::GraphicsSystem* graphics_system);

uint64_t SwapPacingFlipsForced();

}  // namespace ac6::backend
