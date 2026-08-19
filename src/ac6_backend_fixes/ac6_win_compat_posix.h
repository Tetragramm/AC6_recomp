// Minimal Win32 compatibility for the AC6 backend-fix files on POSIX.
//
// The backend fixes probe and poke guest memory whose pages can be
// uncommitted or GPU-write-watched. On Windows they lean on __try/__except
// with the SDK's vectored handler running first. On Linux the SDK's
// ExceptionHandlerCallback (installed by the MMIO handler) is the active
// SIGSEGV handler, and a fault it does not claim is retried forever - there
// is no fallthrough into the SEH emulation. Probing reads therefore must not
// fault at all: Ac6SafeMemRead uses process_vm_readv on the current process,
// which returns EFAULT for a bad range instead of raising a signal. Writes
// stay direct (after a probing read validates the mapping) so a write to a
// GPU-write-watched page still faults into the SDK's recovery handler and is
// tracked like any guest write.

#pragma once

#if !defined(_WIN32)

#include <cstdint>
#include <ctime>
#include <sys/uio.h>
#include <unistd.h>

using DWORD = uint32_t;
using LPVOID = void*;
#define WINAPI

inline uint32_t _byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }

inline void Sleep(DWORD milliseconds) {
  timespec ts{time_t(milliseconds / 1000), long(milliseconds % 1000) * 1000000L};
  nanosleep(&ts, nullptr);
}

// Copy from possibly-unmapped memory without ever taking a fault. Returns
// false if any part of the range is unreadable (unmapped, PROT_NONE, or gone
// mid-copy).
inline bool Ac6SafeMemRead(void* dst, const void* src, size_t bytes) {
  iovec local{dst, bytes};
  iovec remote{const_cast<void*>(src), bytes};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(bytes);
}

#endif  // !defined(_WIN32)
