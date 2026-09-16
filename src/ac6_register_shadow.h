#pragma once

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, ac6_register_shadow_verify);

namespace ac6::shadow {

// Expands the guest D3D9 device struct into a Xenos register-file image.
//
// On the Xbox 360, D3D9 keeps a shadow of the Xenos register file inside the
// device object and flushes dirty ranges into PM4 TYPE0 packets at draw time.
// That means every register the GPU will see for a draw is already sitting in
// guest memory at a known offset BEFORE any packet is written - so the whole
// PM4 round trip (emit, ring, parse, apply) is redundant work we can skip.
//
// This class is the proof of that claim. Expand() reconstructs the register
// image from the device struct; CompareAgainst() diffs it against the register
// file Xenia actually built from the PM4 stream. Zero mismatches across a
// mission means the map is right and the draw path can be driven from the
// struct directly.
class GuestRegisterShadow {
 public:
  // Rebuilds the register image from the guest device object. `base` is the
  // guest memory base, `device` the guest pointer to the D3D device.
  void Expand(const uint8_t* base, uint32_t device);

  // Verifies the expansion against the PM4 the guest just emitted. After a
  // draw/clear/resolve returns, the packets it wrote sit in the command buffer
  // between the write pointer as it was before the call and as it is after;
  // every TYPE0 register write in there must equal what Expand() produced for
  // the same register. Synchronous, on the guest thread, no CP involvement -
  // and a stricter test of the map than diffing whole register files, because
  // it checks exactly the registers the guest considered dirty.
  // Returns the mismatch count for this call.
  uint32_t VerifyEmitted(const uint8_t* base, uint32_t wp_before, uint32_t wp_after,
                         const char* what);

  // Overrides one register with a value that comes from the draw call's
  // arguments rather than from the struct.
  void SetDrawArgument(uint32_t reg, uint32_t value) {
    if (reg < kRegisterCount) {
      values_[reg] = value;
      mapped_[reg >> 6] |= uint64_t(1) << (reg & 63);
    }
  }

  const uint32_t* values() const { return values_; }

  // Aggregate counters for the once-a-second summary.
  static void LogSummary();

 private:
  // 0x5003 registers, matching rex::graphics::RegisterFile::kRegisterCount.
  static constexpr uint32_t kRegisterCount = 0x5003;

  uint32_t values_[kRegisterCount]{};
  // Which registers Expand() actually wrote; a register outside the mapped
  // blocks that the guest flushes is a block we have not mapped yet.
  uint64_t mapped_[(kRegisterCount + 63) / 64]{};
};

// Entry points for the hooks. Cheap no-ops unless ac6_register_shadow_verify
// is on. Call BeginVerify before the original function and EndVerify after it.
// BeginVerify returns the write pointer to hand back to EndVerify.
uint32_t BeginVerify(const uint8_t* base, uint32_t device);
// `vgt_indx_offset` is the one register that is a draw ARGUMENT rather than
// device state: DrawVertices emits it from StartVertex (r5), DrawIndexedVertices
// from BaseVertexIndex (also r5). The native draw path will set it the same way.
void EndVerify(const uint8_t* base, uint32_t device, uint32_t wp_before, uint32_t vgt_indx_offset,
               const char* what);

}  // namespace ac6::shadow
