// Local-only transport for Discord's client. Also searches the Discord Flatpak's
// runtime directory. Keep incomplete frames queued instead of consuming fragments.
#include "connection.h"  // the pinned MIT-licensed discord-rpc interface
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
struct Connection : BaseConnection { int socket = -1; } connection;
#ifdef FORZA_DISCORD_TEST_TRANSPORT
constexpr const char* kPrefix = "forza-discord-test-ipc-";
#else
constexpr const char* kPrefix = "discord-ipc-";
#endif
}
int GetProcessId() { return int(getpid()); }
BaseConnection* BaseConnection::Create() { return &connection; }
void BaseConnection::Destroy(BaseConnection*& value) { value->Close(); value = nullptr; }
bool BaseConnection::Open() {
  auto* self = static_cast<Connection*>(this);
  Close();
  std::array<const char*, 5> roots = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"),
                                    getenv("TMP"), getenv("TEMP"), "/tmp"};
  for (auto* root : roots) {
    if (!root || !*root) continue;
    for (auto suffix : {"", "/app/com.discordapp.Discord", "/app/com.discordapp.DiscordCanary"}) {
      for (int i = 0; i < 10; ++i) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto path = std::string(root) + suffix + "/" + kPrefix + std::to_string(i);
        if (path.size() >= sizeof(address.sun_path)) continue;
        struct stat st{};
        if (stat(path.c_str(), &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) continue;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
          self->socket = fd; isOpen = true; return true;
        }
        close(fd);
      }
    }
  }
  return false;
}
bool BaseConnection::Close() {
  auto* self = static_cast<Connection*>(this);
  if (self->socket >= 0) close(self->socket);
  self->socket = -1; isOpen = false;
  return true;
}
bool BaseConnection::Write(const void* bytes, size_t size) {
  auto* self = static_cast<Connection*>(this);
  if (self->socket < 0) return false;
  ssize_t sent;
  do { sent = send(self->socket, bytes, size, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
  if (sent == ssize_t(size)) return true;
  // A partial write cannot be retried as a new frame. Disconnect and let the RPC
  // client's backoff/reconnect logic resend the most recent complete activity.
  Close(); return false;
}
bool BaseConnection::Read(void* bytes, size_t size) {
  auto* self = static_cast<Connection*>(this);
  if (self->socket < 0) return false;
  int available = 0;
  if (ioctl(self->socket, FIONREAD, &available)) { Close(); return false; }
  if (available < int(size)) {
    char check;
    const auto peek = recv(self->socket, &check, 1, MSG_PEEK | MSG_NOSIGNAL);
    if (peek == 0 || (peek < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) Close();
    return false;
  }
  ssize_t received;
  do { received = recv(self->socket, bytes, size, MSG_NOSIGNAL); } while (received < 0 && errno == EINTR);
  if (received == ssize_t(size)) return true;
  Close(); return false;
}
