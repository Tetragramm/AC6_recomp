/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 *
 * Shared parsing for the AC6 per-shader allowlist cvars (ac6_*_hashes).
 * Both shader translators gate their AC6 payloads on these lists, so the
 * syntax has to be parsed identically on the DXBC and SPIR-V paths.
 */

#ifndef REX_GRAPHICS_PIPELINE_SHADER_AC6_SHADER_HASH_LIST_H_
#define REX_GRAPHICS_PIPELINE_SHADER_AC6_SHADER_HASH_LIST_H_

#include <cstdint>
#include <cstdlib>
#include <string>

namespace rex::graphics {

// Tests whether `hash` (a guest ucode hash) and `tfetch_index` are selected by
// a whitespace/comma separated allowlist of "<hex hash>[:<slot>[+<slot>...]]"
// tokens. A token with no ":<slot>" list matches every slot of that shader.
inline bool UcodeHashSlotInList(uint64_t hash, uint32_t tfetch_index, const std::string& list) {
  auto is_sep = [](char c) {
    return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
  };
  size_t i = 0, n = list.size();
  while (i < n) {
    while (i < n && is_sep(list[i])) {
      ++i;
    }
    size_t start = i;
    while (i < n && !is_sep(list[i])) {
      ++i;
    }
    if (i > start) {
      std::string token = list.substr(start, i - start);
      size_t colon = token.find(':');
      std::string hash_part = colon == std::string::npos ? token : token.substr(0, colon);
      if (std::strtoull(hash_part.c_str(), nullptr, 16) == hash) {
        if (colon == std::string::npos) {
          return true;  // No slot list - all slots.
        }
        size_t p = colon + 1;
        while (p < token.size()) {
          size_t q = token.find('+', p);
          if (q == std::string::npos) {
            q = token.size();
          }
          if (q > p &&
              std::strtoul(token.substr(p, q - p).c_str(), nullptr, 10) == tfetch_index) {
            return true;
          }
          p = q + 1;
        }
        // Hash matched but this slot isn't listed - keep scanning (the same
        // hash may appear again with other slots).
      }
    }
  }
  return false;
}

}  // namespace rex::graphics

#endif  // REX_GRAPHICS_PIPELINE_SHADER_AC6_SHADER_HASH_LIST_H_
