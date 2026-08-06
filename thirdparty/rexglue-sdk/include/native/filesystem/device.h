/**
 * ReXGlue native filesystem layer
 * Part of the AC6 Recompilation project
 */

#pragma once

#include <memory>
#include <string>

#include <native/filesystem/entry.h>
#include <rex/string/buffer.h>
#include <rex/thread/mutex.h>

namespace rex::filesystem {

class Device {
 public:
  explicit Device(const std::string_view mount_path);
  virtual ~Device();

  virtual bool Initialize() = 0;

  const std::string& mount_path() const { return mount_path_; }
  virtual bool is_read_only() const { return true; }

  // Layered mounts: when true, a path that this device fails to resolve falls
  // through to the next registered device whose mount path also matches
  // (registration order = priority, so earlier devices win). Default false =
  // this device is terminal for its mount, exactly the historical behaviour.
  bool layered() const { return layered_; }
  void set_layered(bool layered) { layered_ = layered; }

  virtual void Dump(string::StringBuffer* string_buffer) = 0;
  virtual Entry* ResolvePath(const std::string_view path) = 0;

  virtual const std::string& name() const = 0;
  virtual uint32_t attributes() const = 0;
  virtual uint32_t component_name_max_length() const = 0;

  virtual uint32_t total_allocation_units() const = 0;
  virtual uint32_t available_allocation_units() const = 0;
  virtual uint32_t sectors_per_allocation_unit() const = 0;
  virtual uint32_t bytes_per_sector() const = 0;

 protected:
  rex::thread::global_critical_region global_critical_region_;
  std::string mount_path_;
  bool layered_ = false;
};

}  // namespace rex::filesystem
