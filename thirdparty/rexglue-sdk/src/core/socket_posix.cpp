#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  int fd = static_cast<int>(handle);
  // The guest passes WINSOCK ioctl command codes (ioctlsocket constants).
  // They do not match the Linux ioctl numbers, so passing them through makes
  // the call a silent no-op - most fatally for FIONBIO, which leaves guest
  // sockets in blocking mode and hangs game threads inside recvfrom.
  switch (cmd) {
    case 0x8004667E: {  // WinSock FIONBIO
      // Any nonzero u32 argument enables non-blocking mode (the value is
      // byte-order-agnostic for the zero/nonzero test).
      uint32_t raw = 0;
      if (arg) {
        std::memcpy(&raw, arg, sizeof(raw));
      }
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0) {
        return -1;
      }
      flags = raw ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
      return fcntl(fd, F_SETFL, flags);
    }
    case 0x4004667F: {  // WinSock FIONREAD
      int avail = 0;
      int ret = ioctl(fd, FIONREAD, &avail);
      if (ret == 0 && arg) {
        // Written in host byte order, matching what ioctlsocket does with
        // the guest-provided pointer on the Windows build.
        uint32_t value = static_cast<uint32_t>(avail);
        std::memcpy(arg, &value, sizeof(value));
      }
      return ret;
    }
    default:
      return ioctl(fd, cmd, arg);
  }
}

}  // namespace rex::net
