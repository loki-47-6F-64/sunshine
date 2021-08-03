#ifndef SUNSHINE_WAYLAND_PORTAL_H
#define SUNSHINE_WAYLAND_PORTAL_H

#include <cstdint>
#include <future>
#include <memory>
#include <string>

extern "C" {
struct _GDBusConnection;
struct _GDBusProxy;
struct _GCancellable;
struct _GMainLoop;
}

namespace portal {
constexpr std::uint32_t NONE = 0;

enum capture_type_e : std::uint32_t {
  DESKTOP = 0x01,
  WINDOW  = 0x02,
};

enum mouse_type_e : std::uint32_t {
  METADATA       = 0x01,
  ALWAYS_VISIBLE = 0x02,
  HIDDEN         = 0x04,
};

class session_t {
public:
  // Start a session through dbus
  static std::unique_ptr<session_t> make(
    _GDBusConnection *connection, _GDBusProxy *proxy,
    capture_type_e capture_type, mouse_type_e mouse_type);

  ~session_t();

private:
  // Set source type
  void set_source(capture_type_e capture_type, mouse_type_e mouse_type);
  void start();

public:
  std::uint32_t pipewire_node {};

  _GDBusConnection *connection {};
  _GDBusProxy *proxy {};
  _GCancellable *cancellable {};

  char *handle {};
  std::string name;
};

/**
 * The Main loop
 * This is required for The callbacks in GLib's D-Bus API.
 */
class loop_t {
public:
  loop_t(loop_t &&)      = delete;
  loop_t(const loop_t &) = delete;

  void start(std::launch policy);
  void stop();

  loop_t();
  ~loop_t();

  std::thread worker;
  _GMainLoop *ctx;
};

// Singletons, not thread safe
// If you want thread safety, create your own connection
_GDBusConnection *dbus();
_GDBusProxy *proxy();

std::string unique_name(_GDBusConnection *dbus_p);

mouse_type_e mouse_types(_GDBusProxy *proxy_p);
capture_type_e capture_types(_GDBusProxy *proxy_p);
} // namespace portal
#endif