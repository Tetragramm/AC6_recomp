/**
 * @file        cvar.cpp
 * @brief       Configuration variable system implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include <CLI/CLI.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <toml++/toml.hpp>

namespace rex::cvar {

namespace {

bool g_finalized = false;
bool g_lifecycle_override = false;
std::mutex g_mutex;

// Recursive: FlagRegistrar chain methods re-enter; change callbacks invoked
// from SetFlagByName must not mutate the registry.
std::recursive_mutex& GetRegistryMutex() {
  static std::recursive_mutex m;
  return m;
}

// Flag registry - use functions to avoid static init order issues
std::vector<FlagEntry>& GetRegistryStorage() {
  static std::vector<FlagEntry> registry;
  return registry;
}

std::unordered_map<std::string, size_t>& GetRegistryIndex() {
  static std::unordered_map<std::string, size_t> index;
  return index;
}

// Convert flag name to environment variable: gpu_vsync -> REX_GPU_VSYNC
std::string FlagNameToEnvVar(std::string_view name) {
  std::string result = "REX_";
  for (char c : name) {
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

// Recursively apply TOML values
void ApplyTomlTable(const toml::table& table, const std::string& prefix) {
  for (const auto& [key, value] : table) {
    std::string full_key = prefix.empty() ? std::string(key) : prefix + "_" + std::string(key);

    if (value.is_table()) {
      ApplyTomlTable(*value.as_table(), full_key);
    } else {
      std::string value_str;
      if (value.is_boolean()) {
        value_str = value.as_boolean()->get() ? "true" : "false";
      } else if (value.is_integer()) {
        value_str = std::to_string(value.as_integer()->get());
      } else if (value.is_floating_point()) {
        value_str = std::to_string(value.as_floating_point()->get());
      } else if (value.is_string()) {
        value_str = value.as_string()->get();
      } else {
        REXLOG_WARN("Config: unsupported type for key '{}'", full_key);
        continue;
      }

      if (SetFlagByName(full_key, value_str)) {
        REXLOG_DEBUG("Config: {} = {}", full_key, value_str);
      } else {
        REXLOG_WARN("Config: unknown cvar '{}'", full_key);
      }
    }
  }
}

// todo(tomc): move restart manager to Runtime
std::vector<std::string>& GetPendingRestartStorage() {
  static std::vector<std::string> pending;
  return pending;
}

// Callback storage for change notifications
std::unordered_map<std::string, std::vector<ChangeCallback>>& GetCallbackStorage() {
  static std::unordered_map<std::string, std::vector<ChangeCallback>> callbacks;
  return callbacks;
}

void MarkPendingRestart(std::string_view name) {
  auto& pending = GetPendingRestartStorage();
  std::string name_str(name);
  if (std::find(pending.begin(), pending.end(), name_str) == pending.end()) {
    pending.push_back(name_str);
  }
}

bool ValidateConstraints(const FlagEntry& entry, std::string_view value) {
  const auto& c = entry.constraints;

  // Range validation for numeric types
  if (c.HasRangeConstraint()) {
    double numeric_val = 0;
    if (entry.type == FlagType::String || entry.type == FlagType::Boolean) {
      // These types don't have numeric range constraints
    } else if (entry.type == FlagType::Double) {
      if (!ParseDouble(value, numeric_val))
        return false;
    } else {
      // Integer types
      int64_t int_val = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), int_val);
      if (ec != std::errc())
        return false;
      numeric_val = static_cast<double>(int_val);
    }

    if (c.min.has_value() && numeric_val < *c.min) {
      REXLOG_WARN("Flag '{}': value {} below min ({})", entry.name, value, *c.min);
      return false;
    }
    if (c.max.has_value() && numeric_val > *c.max) {
      REXLOG_WARN("Flag '{}': value {} exceeds max ({})", entry.name, value, *c.max);
      return false;
    }
  }

  // Allowed values validation
  if (c.HasAllowedValues()) {
    bool found = false;
    for (const auto& allowed : c.allowed_values) {
      if (allowed == value) {
        found = true;
        break;
      }
    }
    if (!found) {
      REXLOG_WARN("Flag '{}': '{}' not in allowed values", entry.name, value);
      return false;
    }
  }

  // Custom validator
  if (c.custom_validator && !c.custom_validator(value)) {
    REXLOG_WARN("Flag '{}': custom validation failed for '{}'", entry.name, value);
    return false;
  }

  return true;
}

// Caller holds the registry mutex (recursive, so callbacks may re-enter).
void InvokeChangeCallbacksLocked(std::string_view name, std::string_view value) {
  auto& callbacks = GetCallbackStorage();
  auto it = callbacks.find(std::string(name));
  if (it != callbacks.end()) {
    for (const auto& callback : it->second) {
      callback(name, value);
    }
  }
}

// TOML basic-string escaping for serialized values. Without this a saved
// Windows path ("C:\Games\ac6.iso") produces an invalid escape sequence,
// the next LoadConfig hits a parse error, and the WHOLE config silently
// reverts to defaults.
std::string EscapeTomlBasicString(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

//=============================================================================
// Registry
//=============================================================================

std::vector<FlagEntry>& GetRegistry() {
  return GetRegistryStorage();
}

std::optional<size_t> RegisterFlag(FlagEntry entry) {
  std::lock_guard lock(GetRegistryMutex());
  (void)GetCallbackStorage();
  (void)GetPendingRestartStorage();
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  auto it = index.find(entry.name);
  if (it != index.end()) {
    REXLOG_ERROR("cvar: duplicate registration of '{}'; second registration ignored", entry.name);
    return std::nullopt;
  }
  size_t pos = storage.size();
  index[entry.name] = pos;
  storage.push_back(std::move(entry));
  return pos;
}

void UnregisterFlag(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  std::string key(name);
  auto idx_it = index.find(key);
  if (idx_it == index.end()) {
    return;
  }
  size_t pos = idx_it->second;
  index.erase(idx_it);
  storage.erase(storage.begin() + pos);
  for (auto& [n, i] : index) {
    if (i > pos) {
      --i;
    }
  }
  GetCallbackStorage().erase(key);
  auto& pending = GetPendingRestartStorage();
  pending.erase(std::remove(pending.begin(), pending.end(), key), pending.end());
}

void FlagRegistrar::apply_(std::function<void(FlagEntry&)> fn) {
  if (owned_name_.empty()) {
    return;
  }
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto it = index.find(owned_name_);
  if (it == index.end()) {
    return;
  }
  fn(GetRegistryStorage()[it->second]);
}

bool SetFlagByName(std::string_view name, std::string_view value) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }

  auto& entry = GetRegistryStorage()[it->second];

  // Check lifecycle
  if (!g_lifecycle_override && entry.lifecycle == Lifecycle::kInitOnly && IsFinalized()) {
    REXLOG_WARN("Cannot modify init-only flag '{}' after initialization", name);
    return false;
  }

  // Validate constraints
  if (!ValidateConstraints(entry, value)) {
    return false;
  }

  bool success = entry.setter(value);

  if (success) {
    // Every SetFlagByName caller is a user-intent path (config file load,
    // console, settings UI), so the flag is now user-owned: SaveConfig
    // persists it, session defaults no longer apply to it. The canonical
    // value is captured so later code writes to the live value (REXCVAR_SET
    // enforcement) cannot corrupt what gets saved.
    entry.user_set = true;
    entry.user_value = entry.getter();

    // Track pending restart flags
    if (entry.lifecycle == Lifecycle::kRequiresRestart) {
      MarkPendingRestart(name);
    }

    InvokeChangeCallbacksLocked(name, value);
  }

  return success;
}

bool SetSessionDefault(std::string_view name, std::string_view value, std::string_view driver) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    REXLOG_WARN("SetSessionDefault: unknown flag '{}'", name);
    return false;
  }

  auto& entry = GetRegistryStorage()[it->second];
  if (entry.type == FlagType::Command) {
    return false;
  }
  if (!ValidateConstraints(entry, value)) {
    return false;
  }

  entry.has_session_default = true;
  entry.session_default = std::string(value);
  entry.session_driver = std::string(driver);

  // A user-set value always wins: record the session default (for UI display)
  // but leave the user's value in place.
  if (entry.user_set) {
    return false;
  }

  bool success = entry.setter(value);
  if (success) {
    // Deliberately no MarkPendingRestart: this is the value the app starts
    // from, not a change the user needs to restart for.
    InvokeChangeCallbacksLocked(name, value);
  }
  return success;
}

bool IsUserSet(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  return GetRegistryStorage()[it->second].user_set;
}

std::string GetFlagByName(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return "";
  }

  return GetRegistryStorage()[it->second].getter();
}

std::vector<std::string> ListFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  result.reserve(GetRegistryStorage().size());
  for (const auto& entry : GetRegistryStorage()) {
    result.push_back(entry.name);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByCategory(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByLifecycle(Lifecycle lc) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.lifecycle == lc) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

const FlagEntry* GetFlagInfo(std::string_view name) {
  // Pointer is invalidated by any subsequent registry call.
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return nullptr;
  }
  return &GetRegistryStorage()[it->second];
}

template <>
bool Query<bool>(std::string_view name) {
  std::string v = GetFlagByName(name);
  return v == "true" || v == "1" || v == "yes";
}

template <>
int32_t Query<int32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
int64_t Query<int64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint32_t Query<uint32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint64_t Query<uint64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
double Query<double>(std::string_view name) {
  std::string v = GetFlagByName(name);
  double out = 0.0;
  ParseDouble(v, out);
  return out;
}

template <>
std::string Query<std::string>(std::string_view name) {
  return GetFlagByName(name);
}

std::vector<std::string> GetPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  return GetPendingRestartStorage();
}

void ClearPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  GetPendingRestartStorage().clear();
}

void ResetToDefault(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return;
  }
  auto& entry = GetRegistryStorage()[it->second];
  // Reset means "back to stock, forget my choice": the effective default is
  // the session default when one is in force, and the flag stops being
  // user-owned so the next SaveConfig drops it.
  const std::string& target =
      entry.has_session_default ? entry.session_default : entry.default_value;
  if (entry.setter(target)) {
    entry.user_set = false;
    entry.user_value.clear();
    InvokeChangeCallbacksLocked(name, target);
  }
}

void ResetAllToDefaults() {
  std::lock_guard lock(GetRegistryMutex());
  for (const auto& entry : GetRegistryStorage()) {
    entry.setter(entry.default_value);
  }
}

bool HasNonDefaultValue(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  const auto& entry = GetRegistryStorage()[it->second];
  return entry.getter() != entry.default_value;
}

std::vector<std::string> ListModifiedFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.getter() != entry.default_value) {
      result.push_back(entry.name);
    }
  }
  return result;
}

namespace {

void AppendTomlLine(std::string& result, const FlagEntry& entry) {
  // user_value, not getter(): the live value may be feature-forced (e.g.
  // performance mode holding a diagnostic off); the save keeps the user's
  // own choice.
  if (entry.type == FlagType::String) {
    result += entry.name + " = \"" + EscapeTomlBasicString(entry.user_value) + "\"\n";
  } else {
    result += entry.name + " = " + entry.user_value + "\n";
  }
}

}  // namespace

std::string SerializeToTOML() {
  std::lock_guard lock(GetRegistryMutex());
  std::string result;
  for (const auto& entry : GetRegistryStorage()) {
    // Persist exactly what the user chose. Values written by presets or
    // feature code (REXCVAR_SET / SetSessionDefault) are session state and
    // must not outlive the code that applied them by leaking into the
    // user's config.
    if (entry.user_set) {
      AppendTomlLine(result, entry);
    }
  }
  return result;
}

std::string SerializeToTOML(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::string result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category && entry.user_set) {
      AppendTomlLine(result, entry);
    }
  }
  return result;
}

void RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage()[std::string(name)].push_back(std::move(callback));
}

void UnregisterChangeCallbacks(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage().erase(std::string(name));
}

//=============================================================================
// Initialization
//=============================================================================

std::vector<std::string> Init(int argc, char** argv) {
  CLI::App app{"", ""};
  app.allow_extras();

  for (auto& entry : GetRegistryStorage()) {
    if (entry.type == FlagType::Boolean) {
      app.add_flag_function(
          "--" + entry.name + ",!--no-" + entry.name,
          [&entry](int64_t count) { entry.setter(count > 0 ? "true" : "false"); },
          entry.description);
    } else {
      app.add_option_function<std::string>(
          "--" + entry.name, [&entry](const std::string& val) { entry.setter(val); },
          entry.description);
    }
  }

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // TODO(tomc): dumb workaround for the stupid chicken and its egg.
    //             dont call rex logging funcs here for now.
    fprintf(stderr, "cvar: CLI11  parse error: %s\n", e.what());
  }

  return app.remaining();
}

void LoadConfig(const std::filesystem::path& config_path) {
  if (!std::filesystem::exists(config_path)) {
    REXLOG_DEBUG("Config file not found: {}", rex::path_to_utf8(config_path));
    return;
  }

  try {
    // Open through a path-native stream, exactly as SaveConfig below writes.
    // parse_file(path.string()) narrowed the path through the ANSI code page
    // on Windows, so a config that SAVED fine from a non-ASCII install path
    // (Cyrillic/CJK folder) could never be read back - every setting
    // silently reverted to defaults. Upstream Xenia's config.cc
    // does the equivalent via xe::path_to_utf8; the fork's rename dropped
    // the helper from this call site while the save side stayed path-native.
    std::ifstream file(config_path, std::ios::binary);
    if (!file) {
      REXLOG_ERROR("Failed to open config {}", rex::path_to_utf8(config_path));
      return;
    }
    auto config = toml::parse(file, rex::path_to_utf8(config_path));
    ApplyTomlTable(config, "");
    REXLOG_INFO("Loaded config from {}", rex::path_to_utf8(config_path));
  } catch (const toml::parse_error& err) {
    // path_to_utf8, not .string(): MSVC's path::string() can itself throw
    // for characters outside the ACP - a diagnostic must never throw while
    // reporting the failure.
    REXLOG_ERROR("Failed to parse config {}: {}", rex::path_to_utf8(config_path), err.what());
  }
}

void ApplyEnvironment() {
  int count = 0;
  for (const auto& entry : GetRegistryStorage()) {
    std::string env_name = FlagNameToEnvVar(entry.name);
    const char* env_value = std::getenv(env_name.c_str());
    if (env_value != nullptr) {
      if (entry.setter(env_value)) {
        REXLOG_DEBUG("Env: {} = {} (from {})", entry.name, env_value, env_name);
        ++count;
      } else {
        REXLOG_WARN("Env: failed to parse {} = {}", env_name, env_value);
      }
    }
  }

  if (count > 0) {
    REXLOG_INFO("Applied {} environment variable override(s)", count);
  }
}

void FinalizeInit() {
  std::lock_guard lock(g_mutex);
  g_finalized = true;
  REXLOG_DEBUG("cvar: initialization finalized");
}

bool IsFinalized() {
  return g_finalized;
}

void SaveConfig(const std::filesystem::path& config_path) {
  std::string content = SerializeToTOML();
  if (content.empty()) {
    REXLOG_DEBUG("SaveConfig: no modified flags to save");
    return;
  }

  try {
    std::ofstream file(config_path);
    if (!file) {
      REXLOG_ERROR("SaveConfig: failed to open {}", rex::path_to_utf8(config_path));
      return;
    }
    file << "# Auto-generated cvar configuration\n";
    file << content;
    REXLOG_INFO("Saved config to {}", rex::path_to_utf8(config_path));
  } catch (const std::exception& e) {
    REXLOG_ERROR("SaveConfig: {}", e.what());
  }
}

namespace testing {

ScopedLifecycleOverride::ScopedLifecycleOverride() {
  g_lifecycle_override = true;
}

ScopedLifecycleOverride::~ScopedLifecycleOverride() {
  g_lifecycle_override = false;
}

void ResetAllForTesting() {
  {
    std::lock_guard lock(GetRegistryMutex());
    for (auto& entry : GetRegistryStorage()) {
      entry.setter(entry.default_value);
      entry.user_set = false;
      entry.user_value.clear();
      entry.has_session_default = false;
      entry.session_default.clear();
      entry.session_driver.clear();
    }
  }
  ClearPendingRestartFlags();
  g_finalized = false;
}

}  // namespace testing

}  // namespace rex::cvar
