/**
 * @file        tests/unit/kernel/xam_user_alias_test.cpp
 * @brief       Unit tests for the profile alias
 *              (ac6_profile_always_signed_in / XamEffectiveUserIndex)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/kernel/xam/private.h>

using rex::kernel::xam::XamEffectiveUserIndex;

namespace {

// RAII: force the cvar to a value for one section, restore the default after.
struct ScopedCvar {
  ScopedCvar(const char* name, const char* value) : name_(name) {
    REQUIRE(rex::cvar::SetFlagByName(name_, value));
  }
  ~ScopedCvar() { rex::cvar::ResetToDefault(name_); }
  const char* name_;
};

}  // namespace

TEST_CASE("profile alias: cvar ON maps every valid nonzero index to the one profile",
          "[xam][profile_alias]") {
  ScopedCvar on("ac6_profile_always_signed_in", "true");

  // The player's pad landed at slot 1/2/3: the profile follows the player.
  CHECK(XamEffectiveUserIndex(0) == 0u);
  CHECK(XamEffectiveUserIndex(1) == 0u);
  CHECK(XamEffectiveUserIndex(2) == 0u);
  CHECK(XamEffectiveUserIndex(3) == 0u);

  // Invalid indices stay invalid: error paths are unchanged.
  CHECK(XamEffectiveUserIndex(4) == 4u);
  CHECK(XamEffectiveUserIndex(0xFF) == 0xFFu);
}

TEST_CASE("profile alias: cvar OFF restores index-0-only behaviour unchanged",
          "[xam][profile_alias]") {
  ScopedCvar off("ac6_profile_always_signed_in", "false");

  CHECK(XamEffectiveUserIndex(0) == 0u);
  CHECK(XamEffectiveUserIndex(1) == 1u);
  CHECK(XamEffectiveUserIndex(2) == 2u);
  CHECK(XamEffectiveUserIndex(3) == 3u);
  CHECK(XamEffectiveUserIndex(4) == 4u);
  CHECK(XamEffectiveUserIndex(0xFF) == 0xFFu);
}

TEST_CASE("profile alias: cvar ships default ON", "[xam][profile_alias]") {
  const auto* info = rex::cvar::GetFlagInfo("ac6_profile_always_signed_in");
  REQUIRE(info != nullptr);
  CHECK(info->default_value == "true");
  CHECK(info->category == "AC6/Fixes");
}
