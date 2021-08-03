#include <gio/gio.h>

#include "portal.h"
#include "sunshine/main.h"

using namespace std::literals;

namespace portal {
constexpr char template_request[] = "/org/freedesktop/portal/desktop/request/%.*s/sunshine%u";
constexpr char template_session[] = "/org/freedesktop/portal/desktop/session/%.*s/sunshine%u";

constexpr auto dbus_name_desktop         = "org.freedesktop.portal.Desktop";
constexpr auto dbus_interface_session    = "org.freedesktop.portal.Session";
constexpr auto dbus_interface_request    = "org.freedesktop.portal.Request";
constexpr auto dbus_interface_screencast = "org.freedesktop.portal.ScreenCast";

constexpr auto dbus_object_desktop = "/org/freedesktop/portal/desktop";

static GDBusConnection *dbus_p {};
static GDBusProxy *proxy_p {};

template<std::size_t N>
static inline void make_path(const char (&TEMPLATE)[N], int token, std::array<char, 562> &buf, const std::string_view &session_name, char **out_path, char **out_token) {
  if(out_path) {
    auto bytes = std::snprintf(buf.data(), buf.size(), TEMPLATE, (int)session_name.size(), session_name.data(), token);

    if(out_token) {
      auto pos = std::find(std::reverse_iterator(std::begin(buf) + bytes), std::reverse_iterator(std::begin(buf)), '/');

      *out_token = (&*pos) + 1;
    }

    *out_path = buf.data();

    return;
  }

  std::snprintf(buf.data(), buf.size(), "sunshine%u", token);
  *out_token = buf.data();
}

static inline void make_session_path(std::array<char, 562> &buf, const std::string_view &session_name, char **out_path, char **out_token) {
  static int token_count {};

  make_path(template_session, ++token_count, buf, session_name, out_path, out_token);
}

static inline void make_request_path(std::array<char, 562> &buf, const std::string_view &session_name, char **out_path, char **out_token) {
  static int token_count {};

  make_path(template_request, ++token_count, buf, session_name, out_path, out_token);
}

std::string unique_name(GDBusConnection *dbus_p) {
  std::string name = g_dbus_connection_get_unique_name(dbus_p) + 1;

  std::replace(std::begin(name), std::end(name), '.', '_');

  return name;
}

static inline void init_proxy() {
  g_autoptr(GError) gerror {};

  if(!dbus_p) {
    dbus_p = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &gerror);

    if(gerror) {
      BOOST_LOG(error) << "Couldn't retrieve D-Bus connection: "sv << gerror->message;

      return;
    }
  }

  if(!proxy_p) {
    proxy_p = g_dbus_proxy_new_sync(
      dbus_p, G_DBUS_PROXY_FLAGS_NONE, nullptr,
      dbus_name_desktop, dbus_object_desktop, dbus_interface_screencast,
      nullptr, &gerror);

    if(gerror) {
      BOOST_LOG(error) << "Couldn't create D-Bus proxy object: "sv << gerror->message;

      return;
    }
  }
}

class dbus_call_t {
public:
  using cb_t = std::function<void(session_t *session_p, GVariant *params)>;

  ~dbus_call_t() {
    if(signal_id) {
      g_dbus_connection_signal_unsubscribe(session_p->connection, signal_id);
    }

    if(cancelled_id) {
      g_signal_handler_disconnect(session_p->cancellable, cancelled_id);
    }
  }

  cb_t cb;

  std::string path;

  guint signal_id {};
  gulong cancelled_id {};

  session_t *session_p {};
};

static void session_created_cb(GObject *source, GAsyncResult *res, void *) {
  g_autoptr(GError) gerror {};

  g_autoptr(GVariant) result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &gerror);
  if(gerror) {
    if(!g_error_matches(gerror, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      BOOST_LOG(error) << "Failed to create PipeWire Screencast session: "sv << gerror->message;
    }
  }
}

static void source_set_cb(GObject *source, GAsyncResult *res, void *) {
  g_autoptr(GError) gerror {};

  g_autoptr(GVariant) result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &gerror);
  if(gerror) {
    if(!g_error_matches(gerror, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      BOOST_LOG(error) << "Failed to set PipeWire source: "sv << gerror->message;
    }
  }
}

static void started_cb(GObject *source, GAsyncResult *res, void *) {
  g_autoptr(GError) gerror {};

  g_autoptr(GVariant) result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &gerror);
  if(gerror) {
    if(!g_error_matches(gerror, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      BOOST_LOG(error) << "Failed to start PipeWire ScreenCast: "sv << gerror->message;
    }
  }
}

static void cancelled_cb(GCancellable *, void *userdata) {
  auto call = (dbus_call_t *)userdata;

  BOOST_LOG(info) << "PipeWire Screencast session cancelled"sv;

  auto session_p = call->session_p;
  g_dbus_connection_call(
    session_p->connection,
    dbus_name_desktop,
    call->path.c_str(),
    dbus_interface_request, "Close",
    nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

static void signal_response_received_cb(
  GDBusConnection *,
  const char *, const char *, const char *, const char *,
  GVariant *params, void *userdata) {

  auto call = (dbus_call_t *)userdata;
  call->cb(call->session_p, params);

  delete call;
}

std::unique_ptr<dbus_call_t> subscribe_signal(session_t *session, std::string &&path, dbus_call_t::cb_t &&cb) {
  BOOST_LOG(debug) << "Subscribing to path: "sv << path;

  auto call = std::make_unique<dbus_call_t>();

  call->session_p    = session;
  call->path         = std::move(path);
  call->cancelled_id = g_signal_connect(session->cancellable, "cancelled", G_CALLBACK(cancelled_cb), call.get());
  call->signal_id    = g_dbus_connection_signal_subscribe(
    session->connection, dbus_name_desktop, dbus_interface_request, "Response",
    call->path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, signal_response_received_cb, call.get(), nullptr);

  call->cb = std::move(cb);

  return call;
}

session_t::~session_t() {
  if(handle) {
    g_dbus_connection_call(
      connection, dbus_name_desktop, handle, dbus_interface_session, "Close",
      nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);

    g_clear_pointer(&handle, g_free);
  }

  g_cancellable_cancel(cancellable);
  g_clear_object(&cancellable);
}

std::unique_ptr<session_t> session_t::make(
  GDBusConnection *connection, GDBusProxy *proxy,
  capture_type_e capture_type, mouse_type_e mouse_type) {

  auto session = std::make_unique<session_t>();

  session->connection = connection;
  session->proxy      = proxy;

  session->cancellable = g_cancellable_new();

  session->name = unique_name(connection);

  std::array<char, 562> dbus_req_buf;
  std::array<char, 562> dbus_sess_buf;

  char
    *request_token {},
    *request_path {},
    *session_token {};

  make_request_path(dbus_req_buf, session->name, &request_path, &request_token);
  make_session_path(dbus_sess_buf, session->name, nullptr, &session_token);

  auto call = subscribe_signal(session.get(), request_path, [capture_type, mouse_type](session_t *session_p, GVariant *params) {
    g_autoptr(GVariant) result {};
    std::uint32_t status;

    g_variant_get(params, "(u@a{sv})", &status, &result);

    if(status) {
      BOOST_LOG(error) << "Couldn't create PipeWire session, denied or cancelled by user"sv;

      return;
    }

    g_variant_lookup(result, "session_handle", "s", &session_p->handle);
    BOOST_LOG(info) << "PipeWire Screencast session created: "sv << session_p->handle;

    session_p->set_source(capture_type, mouse_type);
  });

  GVariantBuilder vbuilder;
  g_variant_builder_init(&vbuilder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&vbuilder, "{sv}", "handle_token", g_variant_new_string(request_token));
  g_variant_builder_add(&vbuilder, "{sv}", "session_handle_token", g_variant_new_string(session_token));
  g_dbus_proxy_call(
    session->proxy, "CreateSession",
    g_variant_new("(a{sv})", &vbuilder),
    G_DBUS_CALL_FLAGS_NONE, -1, session->cancellable,
    session_created_cb, call.release());


  return session;
}

void session_t::set_source(capture_type_e capture_type, mouse_type_e mouse_type) {
  std::array<char, 562> request_buf;

  char
    *request_token {},
    *request_path {};

  make_request_path(request_buf, name, &request_path, &request_token);
  auto call = subscribe_signal(this, request_path, [](session_t *session_p, GVariant *params) {
    g_autoptr(GVariant) result {};
    std::uint32_t status;

    g_variant_get(params, "(u@a{sv})", &status, &result);
    if(status) {
      BOOST_LOG(error) << "Couldn't set PipeWire source, denied or cancelled by user"sv;

      return;
    }

    BOOST_LOG(info) << "PipeWire source selected"sv;

    session_p->start();
  });

  GVariantBuilder vbuilder;
  g_variant_builder_init(&vbuilder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&vbuilder, "{sv}", "types", g_variant_new_uint32(capture_type));
  g_variant_builder_add(&vbuilder, "{sv}", "multiple", g_variant_new_boolean(false));
  g_variant_builder_add(&vbuilder, "{sv}", "handle_token", g_variant_new_string(request_token));
  g_variant_builder_add(&vbuilder, "{sv}", "cursor_mode", g_variant_new_uint32(mouse_type));

  // Wait for handle to be pointed to the session handle
  while(!handle)
    ;

  g_dbus_proxy_call(
    proxy, "SelectSources",
    g_variant_new("(oa{sv})", handle, &vbuilder),
    G_DBUS_CALL_FLAGS_NONE, -1, cancellable,
    source_set_cb, call.release());
}

void session_t::start() {
  std::array<char, 562> request_buf;

  char
    *request_token {},
    *request_path {};

  make_request_path(request_buf, name, &request_path, &request_token);

  auto call = subscribe_signal(this, request_path, [](session_t *session_p, GVariant *params) {
    g_autoptr(GVariant) result {};
    std::uint32_t status;

    g_variant_get(params, "(u@a{sv})", &status, &result);
    if(status) {
      BOOST_LOG(error) << "Couldn't start PipeWire ScreenCast, denied or cancelled by user"sv;

      return;
    }

    g_autoptr(GVariant) streams = g_variant_lookup_value(result, "streams", G_VARIANT_TYPE_ARRAY);

    GVariantIter iter;
    g_variant_iter_init(&iter, streams);

    auto n_streams = g_variant_iter_n_children(&iter);
    if(n_streams > 1) {
      BOOST_LOG(warning)
        << "Received more than one PipeWire ScreenCast stream when only one was expected. "
           "This is probably a bug in the desktop portal implementation you are "
           "using.";

      // According to OBS Studio -->
      //
      // The KDE Desktop portal implementation sometimes sends an invalid
      // response where more than one stream is attached, and only the
      // last one is the one we're looking for. This is the only known
      // buggy implementation, so let's at least try to make it work here.
      for(std::size_t x = 0; x < n_streams; ++x) {
        g_autoptr(GVariant) throwaway_properties {};
        uint32_t throwaway_pipewire_node;

        g_variant_iter_loop(&iter, "(u@a{sv})", &throwaway_pipewire_node, &throwaway_properties);
      }
    }

    g_autoptr(GVariant) stream_properties {};
    g_variant_iter_loop(&iter, "(u@a{sv})", &session_p->pipewire_node, &stream_properties);

    BOOST_LOG(info) << "Setting up Desktop ScreenCast"sv;

    // TODO: open pipewire remote
  });

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));

  g_dbus_proxy_call(
    proxy, "Start",
    g_variant_new("(osa{sv})", handle, "", &builder),
    G_DBUS_CALL_FLAGS_NONE, -1, cancellable,
    started_cb, call.release());
}

loop_t::loop_t() : ctx { nullptr } {}

void loop_t::start(std::launch policy) {
  ctx = g_main_loop_new(nullptr, false);

  if((policy & std::launch::async) == std::launch::async) {
    worker = std::thread(g_main_loop_run, ctx);
  }
  else {
    g_main_loop_run(ctx);
  }
}

void loop_t::stop() {
  if(ctx) {
    g_main_loop_quit(ctx);

    if(worker.joinable()) {
      worker.join();
    }

    g_main_loop_unref(ctx);

    ctx = nullptr;
  }
}

loop_t::~loop_t() {
  stop();
}

GDBusConnection *dbus() {
  init_proxy();
  return dbus_p;
}

GDBusProxy *proxy() {
  init_proxy();
  return proxy_p;
}

capture_type_e capture_types(GDBusProxy *proxy_p) {
  if(!proxy_p) {
    return (capture_type_e)NONE;
  }

  g_autoptr(GVariant) cached_source_types =
    g_dbus_proxy_get_cached_property(proxy_p, "AvailableSourceTypes");

  return cached_source_types ? (capture_type_e)g_variant_get_uint32(cached_source_types) : (capture_type_e)NONE;
}

mouse_type_e mouse_types(GDBusProxy *proxy_p) {
  if(!proxy_p) {
    return (mouse_type_e)NONE;
  }

  g_autoptr(GVariant) cached_source_types =
    g_dbus_proxy_get_cached_property(proxy_p, "AvailableCursorModes");

  return cached_source_types ? (mouse_type_e)g_variant_get_uint32(cached_source_types) : (mouse_type_e)NONE;
}
} // namespace portal