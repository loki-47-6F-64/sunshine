#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>

#include <pipewire/pipewire.h>

#include "sunshine/main.h"
#include "sunshine/platform/common.h"

using namespace std::literals;

namespace platf {
namespace pw {
void reg_free(pw_registry *reg) {
  pw_proxy_destroy((pw_proxy *)reg);
}

// using loop_t        = util::safe_ptr<pw_main_loop, pw_main_loop_destroy>;
using loop_t   = util::safe_ptr<pw_thread_loop, pw_thread_loop_destroy>;
using ctx_t    = util::safe_ptr<pw_context, pw_context_destroy>;
using core_t   = util::safe_ptr_v2<pw_core, int, pw_core_disconnect>;
using reg_t    = util::safe_ptr<pw_registry, reg_free>;
using stream_t = util::safe_ptr<pw_stream, pw_stream_destroy>;

class mutex {
public:
  KITTY_DEFAULT_CONSTR(mutex)
  mutex(loop_t::pointer loop) : loop { loop } {}

  void lock() {
    pw_thread_loop_lock(loop);
  }

  void unlock() {
    pw_thread_loop_unlock(loop);
  }

  loop_t::pointer loop;
};

namespace video {
inline spa_pod *make(spa_pod_builder *builder, int fps) {
  spa_rectangle rec[] {
    SPA_RECTANGLE(320, 240), // Arbitrary
    SPA_RECTANGLE(1, 1),
    SPA_RECTANGLE(8192, 4320),
  };

  spa_fraction frac[] {
    SPA_FRACTION((std::uint32_t)fps, 1),
    SPA_FRACTION(0, 1),
    SPA_FRACTION(360, 1),
  };

  return (spa_pod *)spa_pod_builder_add_object(builder,
    SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),

    SPA_FORMAT_VIDEO_format,
    SPA_POD_CHOICE_ENUM_Id(4,
      SPA_VIDEO_FORMAT_BGRA,
      SPA_VIDEO_FORMAT_RGBA,
      SPA_VIDEO_FORMAT_BGRx,
      SPA_VIDEO_FORMAT_RGBx),

    SPA_FORMAT_VIDEO_size,
    SPA_POD_CHOICE_RANGE_Rectangle(
      &rec[0], &rec[1], &rec[2]),
    SPA_FORMAT_VIDEO_framerate,

    SPA_POD_CHOICE_RANGE_Fraction(
      &frac[0], &frac[1], &frac[2]));
}

inline spa_pod *buffer_opt(spa_pod_builder *builder) {
  return (spa_pod *)spa_pod_builder_add_object(builder,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,

    SPA_PARAM_BUFFERS_dataType,
    SPA_POD_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_DmaBuf)));
}
/**
 * Needs to be used directly with pw_stream_new()!!
 */
inline pw_properties *props_screen() {
  return pw_properties_new(
    PW_KEY_MEDIA_TYPE, "Video",
    PW_KEY_MEDIA_CATEGORY, "Capture",
    PW_KEY_MEDIA_ROLE, "Screen",
    nullptr);
}
} // namespace video

class control_t {
public:
  control_t(const control_t &) = delete;
  control_t()                  = default;

  ~control_t() {
    if(loop.get()) {
      BOOST_LOG(debug) << "Waiting for PipeWire main thread..."sv;

      pw_thread_loop_wait(loop.get());
      pw_thread_loop_stop(loop.get());

      BOOST_LOG(debug) << "PipeWire main thread ended"sv;
    }

    if(stream.get()) {
      pw_stream_disconnect(stream.get());
    }
  }

  int init(int framerate) {
    pw_init(nullptr, nullptr);

    BOOST_LOG(debug) << "Compiled with libpipewire: "sv << pw_get_headers_version();
    BOOST_LOG(debug) << "Linked with libpipewire: "sv << pw_get_library_version();

    loop.reset(pw_thread_loop_new("Sunshine thread loop", nullptr));
    ctx.reset(pw_context_new(pw_thread_loop_get_loop(loop.get()), nullptr, 0));

    if(pw_thread_loop_start(loop.get()) < 0) {
      BOOST_LOG(error) << "Couldn't start threaded main PipeWire loop"sv;
      return -1;
    }

    pw::mutex mutex { loop.get() };
    std::unique_lock ul { mutex };

    core.reset(pw_context_connect(ctx.get(), nullptr, 0));
    if(!core) {
      BOOST_LOG(error) << "Couldn't connect to PipeWire instance: "sv << strerror(errno);
    }

    pw_core_add_listener(core.get(), &core_hook, &core_events, this);

    stream.reset(pw_stream_new(core.get(), "Sunshine", video::props_screen()));
    BOOST_LOG(info) << "Created stream [0x"sv << util::hex(this).to_string_view() << ']';

    std::uint8_t params_buffer[1024];
    auto pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

    const spa_pod *param = video::make(&pod_builder, framerate);

    auto status = pw_stream_connect(
      stream.get(),
      PW_DIRECTION_INPUT,
      PW_ID_ANY,
      pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
      &param, 1);

    if(status < 0) {
      BOOST_LOG(error) << "Couldn't connect to a PipeWire stream"sv;
      return -1;
    }

    BOOST_LOG(info) << "Playing PipeWire stream with "sv << framerate << " fps"sv;

    return 0;
  }

  void param_changed_cb(std::uint32_t id, const spa_pod *param) {
    if(!param || id != SPA_PARAM_Format) {
      return;
    }

    auto status = spa_format_parse(param, &format.media_type, &format.media_subtype);
    if(status < 0) {
      BOOST_LOG(error) << "Couldn't parse SPA format"sv;

      return;
    }

    if(format.media_type != SPA_MEDIA_TYPE_video) {
      BOOST_LOG(debug) << "SPA media type != SPA_MEDIA_TYPE_video"sv;
      return;
    }

    if(format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
      BOOST_LOG(debug) << "SPA media subtype != SPA_MEDIA_SUBTYPE_raw"sv;
      return;
    }

    spa_format_video_raw_parse(param, &format.info.raw);

    std::uint8_t params_buffer[1024];
    auto pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const spa_pod *params[] {
      video::buffer_opt(&pod_builder)
    };

    status = pw_stream_update_params(stream.get(), params, sizeof(params) / sizeof(spa_pod *));
    if(status < 0) {
      BOOST_LOG(error) << "Couldn't update stream params"sv;
    }
  }

  void core_error_cb(std::uint32_t id, int seq, int status, const char *message) {
    char string[1024];
    BOOST_LOG(error)
      << "Core error ["sv << id
      << "] seq ["sv << seq
      << "] status ["sv << status
      << "]: ("sv << strerror_r(status, string, sizeof(string))
      << "): "sv << message;

    pw_thread_loop_signal(loop.get(), false);
  }

  void core_done_cb(std::uint32_t id, int seq) {
    if(id == PW_ID_CORE) {
      BOOST_LOG(debug) << "Core done: signalling"sv;
      pw_thread_loop_signal(loop.get(), false);
    }
  }

  static void core_error_cb(void *userdata, std::uint32_t id, int seq, int status, const char *message) {
    ((control_t *)userdata)->core_error_cb(id, seq, status, message);
  }

  static void core_done_cb(void *userdata, std::uint32_t id, int seq) {
    ((control_t *)userdata)->core_done_cb(id, seq);
  }

  static void state_changed_cb(void *userdata, enum pw_stream_state /* old */, pw_stream_state state, const char *error_msg) {
    if(!error_msg) {
      BOOST_LOG(debug) << "PiperWire Stream [0x"sv << util::hex(userdata).to_string_view() << "] state: "sv << pw_stream_state_as_string(state);
    }
    else {
      BOOST_LOG(error) << "PiperWire Stream [0x"sv << util::hex(userdata).to_string_view() << "] state: ["sv << pw_stream_state_as_string(state) << "]: "sv << error_msg;
    }
  }

  static void param_changed_cb(void *userdata, std::uint32_t id, const spa_pod *param) {
    ((control_t *)userdata)->param_changed_cb(id, param);
  }

  static constexpr pw_stream_events stream_events {
    PW_VERSION_STREAM_EVENTS,
    nullptr, // destroy
    state_changed_cb,
    nullptr, // control_info
    nullptr, // io changed
    param_changed_cb
  };

  static constexpr pw_core_events core_events {
    PW_VERSION_CORE_EVENTS,
    nullptr, // info
    core_done_cb,
    nullptr, // ping
    core_error_cb
  };

  spa_hook video_hook {};
  spa_hook core_hook {};
  spa_video_info format;

  pw::loop_t loop;
  pw::ctx_t ctx;
  pw::core_t core;
  pw::stream_t stream;
};
} // namespace pw

struct wayland_t : public display_t {
  std::chrono::nanoseconds delay;
};

std::shared_ptr<display_t> display(mem_type_e mem_type, int framerate) {
  auto shutdown = mail::man->event<bool>(mail::shutdown);

  pw::control_t control;
  if(control.init(60)) {
    // return nullptr;
  }

  shutdown->view();
  return nullptr;
}
} // namespace platf