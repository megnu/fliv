#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_draw.H>
#include <FL/Fl_RGB_Image.H>

#include <Imlib2.h>
#include <magic.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kPadding = 0;
constexpr int kMinWindowW = 240;
constexpr int kMinWindowH = 180;
constexpr int kStatusBarH = 24;
constexpr double kZoomStep = 1.10;
constexpr double kZoomHoldStep = 1.03;
constexpr double kZoomMin = 0.05;
constexpr double kZoomMax = 40.0;
constexpr int kPanStep = 20;
constexpr double kPanTickSeconds = 1.0 / 30.0;
constexpr double kZoomTickSeconds = 1.0 / 30.0;
constexpr const char* kLoaderDirs[] = {
    "/usr/lib/imlib2/loaders",
    "/usr/lib64/imlib2/loaders",
    "/usr/local/lib/imlib2/loaders",
    "/usr/local/lib64/imlib2/loaders",
};
std::mutex g_imlib_decode_mutex;

constexpr unsigned char kDefaultFrameBgR = 30;
constexpr unsigned char kDefaultFrameBgG = 30;
constexpr unsigned char kDefaultFrameBgB = 30;
constexpr unsigned char kDefaultStatusBgR = 20;
constexpr unsigned char kDefaultStatusBgG = 20;
constexpr unsigned char kDefaultStatusBgB = 20;
constexpr unsigned char kDefaultStatusFgR = 230;
constexpr unsigned char kDefaultStatusFgG = 230;
constexpr unsigned char kDefaultStatusFgB = 230;

struct LoadedImage {
  int w = 0;
  int h = 0;
  std::vector<unsigned char> rgb;
};

struct RawImage {
  int w = 0;
  int h = 0;
  std::vector<DATA32> argb;
};

struct AnimatedState {
  std::filesystem::path path;
  int frame_count = 0;
  int loop_count = 0;  // 0 means infinite.
  int canvas_w = 0;
  int canvas_h = 0;
  std::vector<DATA32> canvas;
  std::vector<DATA32> saved_canvas;
  bool has_saved_canvas = false;

  int current_frame_num = 0;  // 1..frame_count
  int current_flags = 0;
  int current_delay_ms = 100;
  int current_fx = 0;
  int current_fy = 0;
  int current_fw = 0;
  int current_fh = 0;
};

struct DecodedImage {
  LoadedImage frame;
  bool animated = false;
  AnimatedState anim;

  int width() const { return frame.w; }
  int height() const { return frame.h; }
};

struct FileMetadata {
  std::filesystem::path path;
  std::string mime;
  uintmax_t size_bytes = 0;
  int width = 0;
  int height = 0;
};

struct UiConfig {
  unsigned char frame_bg_r = kDefaultFrameBgR;
  unsigned char frame_bg_g = kDefaultFrameBgG;
  unsigned char frame_bg_b = kDefaultFrameBgB;
  unsigned char status_bg_r = kDefaultStatusBgR;
  unsigned char status_bg_g = kDefaultStatusBgG;
  unsigned char status_bg_b = kDefaultStatusBgB;
  unsigned char status_fg_r = kDefaultStatusFgR;
  unsigned char status_fg_g = kDefaultStatusFgG;
  unsigned char status_fg_b = kDefaultStatusFgB;
  std::string font_name;
  int font_size = 13;
};

struct CliOptions {
  bool help = false;
  bool list_formats = false;
  std::optional<std::filesystem::path> image_file;
  std::optional<std::filesystem::path> config_file;
};

std::string trim(std::string s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool parse_hex_color(std::string s, unsigned char& r, unsigned char& g, unsigned char& b) {
  s = trim(std::move(s));
  if (s.size() != 7 || s[0] != '#') return false;
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  int v[6];
  for (int i = 0; i < 6; ++i) {
    v[i] = hex(s[1 + i]);
    if (v[i] < 0) return false;
  }
  r = static_cast<unsigned char>(v[0] * 16 + v[1]);
  g = static_cast<unsigned char>(v[2] * 16 + v[3]);
  b = static_cast<unsigned char>(v[4] * 16 + v[5]);
  return true;
}

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool parse_int_in_range(const std::string& s, int min_v, int max_v, int& out_v) {
  const std::string t = trim(s);
  if (t.empty()) return false;
  char* end = nullptr;
  long v = std::strtol(t.c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  if (v < min_v || v > max_v) return false;
  out_v = static_cast<int>(v);
  return true;
}

Fl_Font resolve_font_name(const std::string& name) {
  const std::string want = to_lower_ascii(trim(name));
  if (want.empty()) return FL_HELVETICA;
  const Fl_Font n = Fl::set_fonts();
  for (int f = 0; f < n; ++f) {
    int attrs = 0;
    const char* got = Fl::get_font_name(static_cast<Fl_Font>(f), &attrs);
    (void)attrs;
    if (!got) continue;
    if (to_lower_ascii(got) == want) {
      return static_cast<Fl_Font>(f);
    }
  }
  Fl::set_font(FL_FREE_FONT, name.c_str());
  return FL_FREE_FONT;
}

int normalize_frame_delay_ms(int delay_ms) {
  if (delay_ms <= 0) {
    return 100;
  }
  return std::clamp(delay_ms, 20, 10000);
}

inline DATA32 alpha_blend_argb(DATA32 dst, DATA32 src) {
  const unsigned int sa = (src >> 24) & 0xff;
  const unsigned int da = (dst >> 24) & 0xff;
  if (sa == 0) return dst;
  if (sa == 255 || da == 0) return src;

  const unsigned int sr = (src >> 16) & 0xff;
  const unsigned int sg = (src >> 8) & 0xff;
  const unsigned int sb = src & 0xff;
  const unsigned int dr = (dst >> 16) & 0xff;
  const unsigned int dg = (dst >> 8) & 0xff;
  const unsigned int db = dst & 0xff;

  const unsigned int inv_sa = 255 - sa;
  const unsigned int oa = sa + (da * inv_sa + 127) / 255;
  if (oa == 0) return 0;

  // Straight-alpha source-over compositing.
  const unsigned int premul_r = sr * sa + (dr * da * inv_sa + 127) / 255;
  const unsigned int premul_g = sg * sa + (dg * da * inv_sa + 127) / 255;
  const unsigned int premul_b = sb * sa + (db * da * inv_sa + 127) / 255;
  const unsigned int or_ = (premul_r + oa / 2) / oa;
  const unsigned int og = (premul_g + oa / 2) / oa;
  const unsigned int ob = (premul_b + oa / 2) / oa;

  return static_cast<DATA32>(((oa & 0xffu) << 24) | ((or_ & 0xffu) << 16) |
                             ((og & 0xffu) << 8) | (ob & 0xffu));
}

void argb_to_checkerboard_rgb(const std::vector<DATA32>& argb, int w, int h, LoadedImage& out) {
  out.w = w;
  out.h = h;
  out.rgb.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const DATA32 px = argb[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3u;
      const unsigned int a = (px >> 24) & 0xff;
      const unsigned int r = (px >> 16) & 0xff;
      const unsigned int g = (px >> 8) & 0xff;
      const unsigned int b = px & 0xff;

      constexpr int kTile = 12;
      constexpr unsigned int kLight = 210;
      constexpr unsigned int kDark = 165;
      const bool light_tile = ((x / kTile) + (y / kTile)) % 2 == 0;
      const unsigned int cb = light_tile ? kLight : kDark;

      out.rgb[i + 0] = static_cast<unsigned char>((r * a + cb * (255 - a)) / 255);
      out.rgb[i + 1] = static_cast<unsigned char>((g * a + cb * (255 - a)) / 255);
      out.rgb[i + 2] = static_cast<unsigned char>((b * a + cb * (255 - a)) / 255);
    }
  }
}

bool read_current_imlib_image_raw(RawImage& out, std::string& err_out) {
  out.w = imlib_image_get_width();
  out.h = imlib_image_get_height();

  if (out.w <= 0 || out.h <= 0) {
    err_out = "invalid decoded image dimensions";
    return false;
  }

  const DATA32* data = imlib_image_get_data_for_reading_only();
  if (!data) {
    err_out = "decoder returned no pixel data";
    return false;
  }

  const size_t n = static_cast<size_t>(out.w) * static_cast<size_t>(out.h);
  out.argb.assign(data, data + n);
  return true;
}

bool load_image_frame_raw(const std::filesystem::path& path, int frame_num, RawImage& raw,
                          Imlib_Frame_Info& finfo, std::string& err_out) {
  std::lock_guard<std::mutex> lk(g_imlib_decode_mutex);
  Imlib_Image im = imlib_load_image_frame(path.string().c_str(), frame_num);
  if (!im) {
    err_out = "failed to decode animation frame";
    return false;
  }
  imlib_context_set_image(im);
  const bool ok = read_current_imlib_image_raw(raw, err_out);
  if (ok) {
    finfo = {};
    imlib_image_get_frame_info(&finfo);
  }
  imlib_free_image();
  return ok;
}

void resolve_frame_rect(const RawImage& raw, const Imlib_Frame_Info& info, int canvas_w, int canvas_h,
                        int& fx, int& fy, int& fw, int& fh) {
  fx = std::clamp(info.frame_x, 0, canvas_w);
  fy = std::clamp(info.frame_y, 0, canvas_h);
  const int default_fw = (raw.w == canvas_w && raw.h == canvas_h) ? (canvas_w - fx) : raw.w;
  const int default_fh = (raw.w == canvas_w && raw.h == canvas_h) ? (canvas_h - fy) : raw.h;
  fw = std::clamp((info.frame_w > 0) ? info.frame_w : default_fw, 0, canvas_w - fx);
  fh = std::clamp((info.frame_h > 0) ? info.frame_h : default_fh, 0, canvas_h - fy);
}

void compose_frame_onto_canvas(AnimatedState& anim, const RawImage& raw, const Imlib_Frame_Info& info) {
  int fx = 0, fy = 0, fw = 0, fh = 0;
  resolve_frame_rect(raw, info, anim.canvas_w, anim.canvas_h, fx, fy, fw, fh);

  if (info.frame_flags & IMLIB_FRAME_DISPOSE_PREV) {
    anim.saved_canvas = anim.canvas;
    anim.has_saved_canvas = true;
  } else {
    anim.has_saved_canvas = false;
  }

  for (int y = 0; y < fh; ++y) {
    for (int x = 0; x < fw; ++x) {
      int sx = x;
      int sy = y;
      if (raw.w == anim.canvas_w && raw.h == anim.canvas_h) {
        sx = fx + x;
        sy = fy + y;
      }
      if (sx < 0 || sy < 0 || sx >= raw.w || sy >= raw.h) {
        continue;
      }
      const size_t src_i = static_cast<size_t>(sy) * static_cast<size_t>(raw.w) + static_cast<size_t>(sx);
      const size_t dst_i =
          static_cast<size_t>(fy + y) * static_cast<size_t>(anim.canvas_w) + static_cast<size_t>(fx + x);
      const DATA32 src_px = raw.argb[src_i];
      if (info.frame_flags & IMLIB_FRAME_BLEND) {
        anim.canvas[dst_i] = alpha_blend_argb(anim.canvas[dst_i], src_px);
      } else {
        anim.canvas[dst_i] = src_px;
      }
    }
  }

  anim.current_flags = info.frame_flags;
  anim.current_delay_ms = normalize_frame_delay_ms(info.frame_delay);
  anim.current_fx = fx;
  anim.current_fy = fy;
  anim.current_fw = fw;
  anim.current_fh = fh;
}

void apply_previous_frame_disposal(AnimatedState& anim) {
  if (anim.current_flags & IMLIB_FRAME_DISPOSE_CLEAR) {
    for (int y = 0; y < anim.current_fh; ++y) {
      const size_t row = static_cast<size_t>(anim.current_fy + y) * static_cast<size_t>(anim.canvas_w);
      for (int x = 0; x < anim.current_fw; ++x) {
        anim.canvas[row + static_cast<size_t>(anim.current_fx + x)] = 0;
      }
    }
  } else if ((anim.current_flags & IMLIB_FRAME_DISPOSE_PREV) && anim.has_saved_canvas) {
    anim.canvas = anim.saved_canvas;
  }
}

struct DecodedAnimFrame {
  RawImage raw;
  Imlib_Frame_Info info = {};
};

class AnimationDecoder {
 public:
  explicit AnimationDecoder(std::filesystem::path path) : path_(std::move(path)) {}

  bool decode_frame(int frame_num, DecodedAnimFrame& out, std::string& err_out) const {
    return load_image_frame_raw(path_, frame_num, out.raw, out.info, err_out);
  }

 private:
  std::filesystem::path path_;
};

class FrameCacheLRU {
 public:
  explicit FrameCacheLRU(size_t capacity) : capacity_(std::max<size_t>(1, capacity)) {}

  std::shared_ptr<DecodedAnimFrame> get(int frame_num) {
    auto it = map_.find(frame_num);
    if (it == map_.end()) return nullptr;
    order_.erase(it->second.order_it);
    order_.push_front(frame_num);
    it->second.order_it = order_.begin();
    return it->second.frame;
  }

  void put(int frame_num, std::shared_ptr<DecodedAnimFrame> frame) {
    auto it = map_.find(frame_num);
    if (it != map_.end()) {
      order_.erase(it->second.order_it);
      map_.erase(it);
    }
    order_.push_front(frame_num);
    map_[frame_num] = {std::move(frame), order_.begin()};
    while (map_.size() > capacity_) {
      const int victim = order_.back();
      order_.pop_back();
      map_.erase(victim);
    }
  }

 private:
  struct Entry {
    std::shared_ptr<DecodedAnimFrame> frame;
    std::list<int>::iterator order_it;
  };
  size_t capacity_;
  std::list<int> order_;
  std::unordered_map<int, Entry> map_;
};

class AnimationEngine {
 public:
  explicit AnimationEngine(AnimatedState state)
      : state_(std::move(state)), decoder_(state_.path), cache_(6) {
    worker_ = std::thread([this]() { worker_loop(); });
  }

  ~AnimationEngine() { stop_worker(); }

  int current_delay_ms() const { return state_.current_delay_ms; }

  void prime_prefetch_next() {
    const int next = next_frame_num();
    if (next <= 0) return;
    request_prefetch(next);
  }

  bool advance(LoadedImage& out_rgb) {
    const int next = next_frame_num();
    if (next <= 0) return false;

    apply_previous_frame_disposal(state_);

    auto frame = pop_cached_or_decode(next);
    if (!frame) {
      return false;
    }

    compose_frame_onto_canvas(state_, frame->raw, frame->info);
    state_.current_frame_num = next;
    argb_to_checkerboard_rgb(state_.canvas, state_.canvas_w, state_.canvas_h, out_rgb);
    request_prefetch(next_frame_num());
    return true;
  }

 private:
  int next_frame_num() {
    if (state_.frame_count <= 1) return -1;
    int next = state_.current_frame_num + 1;
    if (next > state_.frame_count) {
      if (state_.loop_count > 0) {
        ++loops_done_;
        if (loops_done_ >= state_.loop_count) {
          return -1;
        }
      }
      next = 1;
    }
    return next;
  }

  std::shared_ptr<DecodedAnimFrame> pop_cached_or_decode(int frame_num) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (auto cached = cache_.get(frame_num)) {
        return cached;
      }
    }

    auto decoded = std::make_shared<DecodedAnimFrame>();
    std::string err;
    if (!decoder_.decode_frame(frame_num, *decoded, err)) {
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      cache_.put(frame_num, decoded);
    }
    return decoded;
  }

  void request_prefetch(int frame_num) {
    if (frame_num <= 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (cache_.get(frame_num)) {
      return;
    }
    pending_frame_ = frame_num;
    has_pending_ = true;
    cv_.notify_one();
  }

  void worker_loop() {
    for (;;) {
      int target = 0;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&]() { return stop_ || has_pending_; });
        if (stop_) return;
        target = pending_frame_;
        has_pending_ = false;
        if (cache_.get(target)) {
          continue;
        }
      }

      auto decoded = std::make_shared<DecodedAnimFrame>();
      std::string err;
      if (!decoder_.decode_frame(target, *decoded, err)) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!stop_) {
          cache_.put(target, decoded);
        }
      }
    }
  }

  void stop_worker() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
      has_pending_ = false;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  AnimatedState state_;
  int loops_done_ = 0;
  AnimationDecoder decoder_;
  FrameCacheLRU cache_;
  std::thread worker_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool has_pending_ = false;
  int pending_frame_ = 0;
};

bool load_image_decoded(const char* path, DecodedImage& out, std::string& err_out) {
  out = {};
  const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(path)).lexically_normal();

  RawImage raw0;
  Imlib_Frame_Info info0 = {};
  if (load_image_frame_raw(abs, 1, raw0, info0, err_out)) {
    const int frame_count = std::max(1, info0.frame_count);
    const int canvas_w = (info0.canvas_w > 0) ? info0.canvas_w : raw0.w;
    const int canvas_h = (info0.canvas_h > 0) ? info0.canvas_h : raw0.h;

    AnimatedState anim;
    anim.path = abs;
    anim.frame_count = frame_count;
    anim.loop_count = std::max(0, info0.loop_count);
    anim.canvas_w = canvas_w;
    anim.canvas_h = canvas_h;
    anim.canvas.assign(static_cast<size_t>(canvas_w) * static_cast<size_t>(canvas_h), 0);
    anim.current_frame_num = 1;

    compose_frame_onto_canvas(anim, raw0, info0);

    LoadedImage composed;
    argb_to_checkerboard_rgb(anim.canvas, anim.canvas_w, anim.canvas_h, composed);
    out.frame = std::move(composed);
    if (frame_count > 1) {
      out.animated = true;
      out.anim = std::move(anim);
    }
    return true;
  }

  RawImage raw;
  {
    std::lock_guard<std::mutex> lk(g_imlib_decode_mutex);
    int err = 0;
    Imlib_Image im = imlib_load_image_with_errno_return(abs.string().c_str(), &err);
    if (!im) {
      const char* err_str = imlib_strerror(err);
      err_out = err_str ? err_str : "unknown load error";
      return false;
    }

    imlib_context_set_image(im);
    if (!read_current_imlib_image_raw(raw, err_out)) {
      imlib_free_image();
      return false;
    }
    imlib_free_image();
  }

  argb_to_checkerboard_rgb(raw.argb, raw.w, raw.h, out.frame);
  return true;
}

class ImageView : public Fl_Widget {
 public:
  ImageView(int X, int Y, int W, int H, LoadedImage image = {})
      : Fl_Widget(X, Y, W, H) {
    set_image(std::move(image));
  }

  void set_navigate_callback(std::function<bool(int)> cb) { navigate_cb_ = std::move(cb); }
  void set_copy_callback(std::function<void()> cb) { copy_cb_ = std::move(cb); }
  void set_reload_callback(std::function<void()> cb) { reload_cb_ = std::move(cb); }
  void set_open_gimp_callback(std::function<void()> cb) { open_gimp_cb_ = std::move(cb); }
  void set_open_inkscape_callback(std::function<void()> cb) { open_inkscape_cb_ = std::move(cb); }
  void set_open_file_callback(std::function<void()> cb) { open_file_cb_ = std::move(cb); }
  void set_toggle_fullscreen_callback(std::function<void()> cb) { toggle_fullscreen_cb_ = std::move(cb); }
  void set_exit_fullscreen_callback(std::function<bool()> cb) { exit_fullscreen_cb_ = std::move(cb); }
  void set_external_app_availability(bool gimp_available, bool inkscape_available) {
    gimp_available_ = gimp_available;
    inkscape_available_ = inkscape_available;
  }
  void set_frame_background_color(Fl_Color c) { frame_bg_color_ = c; }
  void set_menu_font(Fl_Font font, Fl_Fontsize size) {
    menu_font_ = font;
    menu_font_size_ = size;
  }

  void set_image(LoadedImage image) {
    stop_animation();
    anim_engine_.reset();
    animation_ = {};
    has_animation_ = false;

    image_ = std::move(image);
    has_image_ = (image_.w > 0 && image_.h > 0 && !image_.rgb.empty());
    sync_image_pointers();
    fit_to_window_no_upscale();
    redraw();
  }

  void set_animation(AnimatedState animation, LoadedImage first_frame) {
    stop_animation();
    anim_engine_.reset();
    animation_ = std::move(animation);
    has_animation_ = animation_.frame_count > 1;
    image_ = std::move(first_frame);
    has_image_ = (image_.w > 0 && image_.h > 0 && !image_.rgb.empty());
    sync_image_pointers();
    fit_to_window_no_upscale();
    if (has_animation_) {
      anim_engine_ = std::make_unique<AnimationEngine>(animation_);
      anim_engine_->prime_prefetch_next();
      schedule_animation_for_current_frame();
    }
    redraw();
  }

  bool has_image() const { return has_image_; }

  void clear_image() {
    stop_animation();
    anim_engine_.reset();
    image_ = {};
    animation_ = {};
    has_animation_ = false;
    has_image_ = false;
    sync_image_pointers();
    clear_pan_keys();
    redraw();
  }

  int handle(int event) override {
    switch (event) {
      case FL_FOCUS:
        return 1;
      case FL_UNFOCUS:
        clear_pan_keys();
        clear_zoom_keys();
        return 1;
      case FL_PUSH:
        take_focus();
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          show_context_menu();
          return 1;
        }
        if (Fl::event_button() == FL_LEFT_MOUSE) {
          dragging_ = true;
          drag_last_x_ = Fl::event_x();
          drag_last_y_ = Fl::event_y();
        }
        return 1;
      case FL_DRAG:
        if (dragging_) {
          const int nx = Fl::event_x();
          const int ny = Fl::event_y();
          pan_image_by(nx - drag_last_x_, ny - drag_last_y_);
          drag_last_x_ = nx;
          drag_last_y_ = ny;
          return 1;
        }
        break;
      case FL_RELEASE:
        if (dragging_ && Fl::event_button() == FL_LEFT_MOUSE) {
          dragging_ = false;
          return 1;
        }
        break;
      case FL_MOUSEWHEEL:
        if (Fl::event_dy() < 0) {
          zoom_by(kZoomStep, Fl::event_x() - viewport_x(), Fl::event_y() - viewport_y());
        } else if (Fl::event_dy() > 0) {
          zoom_by(1.0 / kZoomStep, Fl::event_x() - viewport_x(), Fl::event_y() - viewport_y());
        }
        return 1;
      case FL_KEYDOWN:
      case FL_SHORTCUT:
        if (handle_context_menu_shortcut()) {
          return 1;
        }
        if (handle_escape_shortcut()) {
          return 1;
        }
        if (handle_fullscreen_shortcut()) {
          return 1;
        }
        if (handle_open_shortcut()) {
          return 1;
        }
        if (handle_reload_shortcut()) {
          return 1;
        }
        if (handle_external_open_shortcuts()) {
          return 1;
        }
        if (!(Fl::event_state() & FL_CTRL)) {
          const int key = Fl::event_key();
          const bool is_pan = is_pan_key(key);
          const bool changed = set_pan_key_state(key, true);
          if (is_pan) {
            if (changed) {
              pan_tick();
              ensure_pan_timer();
            }
            return 1;  // swallow repeats; timer is the single pan driver
          }
          const bool is_zoom = is_zoom_hold_key(key);
          const bool zoom_changed = set_zoom_key_state(key, true);
          if (is_zoom) {
            if (zoom_changed) {
              zoom_tick();
              ensure_zoom_timer();
            }
            return 1;  // swallow repeats; timer is the single zoom driver
          }
        }
        if (handle_copy_shortcut()) {
          return 1;
        }
        if (handle_navigation_shortcuts()) {
          return 1;
        }
        if (handle_zoom_shortcuts()) {
          return 1;
        }
        break;
      case FL_KEYUP: {
        const bool pan_changed = set_pan_key_state(Fl::event_key(), false);
        if (pan_changed) {
          if (!any_pan_key_pressed()) {
            stop_pan_timer();
          }
          return 1;
        }
        const bool zoom_changed = set_zoom_key_state(Fl::event_key(), false);
        if (zoom_changed) {
          if (!any_zoom_key_pressed()) {
            stop_zoom_timer();
          }
          return 1;
        }
        break;
      }
      default:
        break;
    }
    return Fl_Widget::handle(event);
  }

  void draw() override {
    fl_push_clip(x(), y(), w(), h());

    fl_color(frame_bg_color_);
    fl_rectf(x(), y(), w(), h());

    if (!has_image_) {
      const char* hint = "Press O / Ctrl+O or right-click -> Open Image...";
      fl_color(fl_rgb_color(220, 220, 220));
      fl_font(FL_HELVETICA, 16);
      int tw = 0, th = 0;
      fl_measure(hint, tw, th, 0);
      fl_draw(hint, x() + (w() - tw) / 2, y() + (h() + th) / 2);
      fl_pop_clip();
      return;
    }

    const int draw_w = scaled_w();
    const int draw_h = scaled_h();
    clamp_offsets();
    (void)draw_w;
    (void)draw_h;

    draw_visible_region();

    fl_pop_clip();
  }

  ~ImageView() override {
    stop_animation();
    stop_pan_timer();
    stop_zoom_timer();
  }

 private:
  static void pan_timer_cb(void* userdata) {
    auto* self = static_cast<ImageView*>(userdata);
    self->pan_tick();
    if (self->any_pan_key_pressed()) {
      Fl::repeat_timeout(kPanTickSeconds, pan_timer_cb, userdata);
    } else {
      self->pan_timer_active_ = false;
    }
  }

  static void animation_timer_cb(void* userdata) {
    auto* self = static_cast<ImageView*>(userdata);
    self->animation_timer_active_ = false;
    self->advance_animation_frame();
  }

  static void zoom_timer_cb(void* userdata) {
    auto* self = static_cast<ImageView*>(userdata);
    self->zoom_tick();
    if (self->any_zoom_key_pressed()) {
      Fl::repeat_timeout(kZoomTickSeconds, zoom_timer_cb, userdata);
    } else {
      self->zoom_timer_active_ = false;
    }
  }

  void schedule_animation_for_current_frame() {
    if (!has_image_ || !has_animation_ || !anim_engine_) {
      return;
    }
    const int delay_ms = normalize_frame_delay_ms(anim_engine_->current_delay_ms());
    animation_timer_active_ = true;
    Fl::add_timeout(static_cast<double>(delay_ms) / 1000.0, animation_timer_cb, this);
  }

  void stop_animation() {
    if (animation_timer_active_) {
      Fl::remove_timeout(animation_timer_cb, this);
      animation_timer_active_ = false;
    }
  }

  void advance_animation_frame() {
    if (!has_image_ || !has_animation_ || !anim_engine_) {
      return;
    }
    if (!anim_engine_->advance(image_)) {
      return;
    }
    sync_image_pointers();
    clamp_offsets();
    redraw();
    schedule_animation_for_current_frame();
  }

  bool set_pan_key_state(int key, bool down) {
    bool* slot = nullptr;
    if (key == 'w' || key == 'W') slot = &pan_w_;
    else if (key == 'a' || key == 'A') slot = &pan_a_;
    else if (key == 's' || key == 'S') slot = &pan_s_;
    else if (key == 'd' || key == 'D') slot = &pan_d_;
    else return false;

    if (*slot == down) {
      return false;
    }
    *slot = down;
    return true;
  }

  static bool is_pan_key(int key) {
    return key == 'w' || key == 'W' || key == 'a' || key == 'A' ||
           key == 's' || key == 'S' || key == 'd' || key == 'D';
  }

  bool any_pan_key_pressed() const { return pan_w_ || pan_a_ || pan_s_ || pan_d_; }
  bool any_zoom_key_pressed() const { return zoom_in_ || zoom_out_; }

  void clear_pan_keys() {
    pan_w_ = pan_a_ = pan_s_ = pan_d_ = false;
    stop_pan_timer();
  }

  bool set_zoom_key_state(int key, bool down) {
    bool* slot = nullptr;
    if (key == 'e' || key == 'E' || key == '+' || key == '=' || key == (FL_KP + '+')) {
      slot = &zoom_in_;
    } else if (key == 'q' || key == 'Q' || key == '-' || key == '_' || key == (FL_KP + '-')) {
      slot = &zoom_out_;
    } else {
      return false;
    }

    if (*slot == down) {
      return false;
    }
    *slot = down;
    return true;
  }

  static bool is_zoom_hold_key(int key) {
    return key == 'e' || key == 'E' || key == 'q' || key == 'Q' ||
           key == '+' || key == '=' || key == '-' || key == '_' ||
           key == (FL_KP + '+') || key == (FL_KP + '-');
  }

  void clear_zoom_keys() {
    zoom_in_ = false;
    zoom_out_ = false;
    stop_zoom_timer();
  }

  void ensure_pan_timer() {
    if (!pan_timer_active_) {
      pan_timer_active_ = true;
      Fl::add_timeout(kPanTickSeconds, pan_timer_cb, this);
    }
  }

  void stop_pan_timer() {
    if (pan_timer_active_) {
      Fl::remove_timeout(pan_timer_cb, this);
      pan_timer_active_ = false;
    }
  }

  void ensure_zoom_timer() {
    if (!zoom_timer_active_) {
      zoom_timer_active_ = true;
      Fl::add_timeout(kZoomTickSeconds, zoom_timer_cb, this);
    }
  }

  void stop_zoom_timer() {
    if (zoom_timer_active_) {
      Fl::remove_timeout(zoom_timer_cb, this);
      zoom_timer_active_ = false;
    }
  }

  void pan_tick() {
    if (!has_image_) return;
    const int vx = (pan_d_ ? 1 : 0) - (pan_a_ ? 1 : 0);
    const int vy = (pan_s_ ? 1 : 0) - (pan_w_ ? 1 : 0);
    if (vx == 0 && vy == 0) {
      return;
    }

    const double len = std::sqrt(static_cast<double>(vx * vx + vy * vy));
    const int dx = static_cast<int>(std::lround(static_cast<double>(kPanStep) * vx / len));
    const int dy = static_cast<int>(std::lround(static_cast<double>(kPanStep) * vy / len));
    pan_view_by(dx, dy);
  }

  void zoom_tick() {
    if (!has_image_) return;
    const int vz = (zoom_in_ ? 1 : 0) - (zoom_out_ ? 1 : 0);
    if (vz == 0) {
      return;
    }
    const double factor = (vz > 0) ? kZoomHoldStep : (1.0 / kZoomHoldStep);
    zoom_by(factor, viewport_w() / 2, viewport_h() / 2);
  }

  int viewport_x() const { return x() + kPadding; }
  int viewport_y() const { return y() + kPadding; }
  int viewport_w() const { return std::max(1, w() - 2 * kPadding); }
  int viewport_h() const { return std::max(1, h() - 2 * kPadding); }

  double fit_scale_no_upscale() const {
    if (!has_image_) return 1.0;
    const double sx = static_cast<double>(viewport_w()) / static_cast<double>(src_w_);
    const double sy = static_cast<double>(viewport_h()) / static_cast<double>(src_h_);
    return std::min(1.0, std::min(sx, sy));
  }

  double fit_scale_full() const {
    if (!has_image_) return 1.0;
    const double sx = static_cast<double>(viewport_w()) / static_cast<double>(src_w_);
    const double sy = static_cast<double>(viewport_h()) / static_cast<double>(src_h_);
    return std::min(sx, sy);
  }

  double current_scale() const { return std::clamp(zoom_, kZoomMin, kZoomMax); }

  int scaled_w() const { return std::max(1, static_cast<int>(src_w_ * current_scale())); }
  int scaled_h() const { return std::max(1, static_cast<int>(src_h_ * current_scale())); }

  void clamp_offsets() {
    const int sw = scaled_w();
    const int sh = scaled_h();
    const int vw = viewport_w();
    const int vh = viewport_h();

    if (sw <= vw) {
      img_x_ = (vw - sw) / 2;
    } else {
      const int min_x = vw - sw;
      img_x_ = std::clamp(img_x_, min_x, 0);
    }

    if (sh <= vh) {
      img_y_ = (vh - sh) / 2;
    } else {
      const int min_y = vh - sh;
      img_y_ = std::clamp(img_y_, min_y, 0);
    }
  }

  void draw_visible_region() {
    if (!has_image_) return;
    const int vw = viewport_w();
    const int vh = viewport_h();
    const int sw = scaled_w();
    const int sh = scaled_h();
    const double scale = current_scale();

    const int vis_x0 = std::max(0, img_x_);
    const int vis_y0 = std::max(0, img_y_);
    const int vis_x1 = std::min(vw, img_x_ + sw);
    const int vis_y1 = std::min(vh, img_y_ + sh);
    if (vis_x1 <= vis_x0 || vis_y1 <= vis_y0) {
      return;
    }

    const int out_w = vis_x1 - vis_x0;
    const int out_h = vis_y1 - vis_y0;
    const size_t need = static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 3u;
    if (scaled_view_.size() != need) {
      scaled_view_.resize(need);
    }

    if (scale >= 1.0) {
      // Nearest-neighbor for native/zoom-in inspection clarity.
      for (int y = 0; y < out_h; ++y) {
        const int vy = vis_y0 + y;
        int sy = static_cast<int>(std::floor((vy - img_y_) / scale));
        sy = std::clamp(sy, 0, src_h_ - 1);
        for (int x = 0; x < out_w; ++x) {
          const int vx = vis_x0 + x;
          int sx = static_cast<int>(std::floor((vx - img_x_) / scale));
          sx = std::clamp(sx, 0, src_w_ - 1);

          const size_t si =
              (static_cast<size_t>(sy) * static_cast<size_t>(src_w_) + static_cast<size_t>(sx)) * 3u;
          const size_t di =
              (static_cast<size_t>(y) * static_cast<size_t>(out_w) + static_cast<size_t>(x)) * 3u;
          scaled_view_[di + 0] = src_pixels_[si + 0];
          scaled_view_[di + 1] = src_pixels_[si + 1];
          scaled_view_[di + 2] = src_pixels_[si + 2];
        }
      }
    } else {
      // Bilinear for zoom-out quality.
      for (int y = 0; y < out_h; ++y) {
        const int vy = vis_y0 + y;
        const double src_y = (static_cast<double>(vy - img_y_) + 0.5) / scale - 0.5;
        int y0 = static_cast<int>(std::floor(src_y));
        int y1 = y0 + 1;
        double wy = src_y - static_cast<double>(y0);
        if (y0 < 0) {
          y0 = y1 = 0;
          wy = 0.0;
        } else if (y1 >= src_h_) {
          y1 = y0 = src_h_ - 1;
          wy = 0.0;
        }

        for (int x = 0; x < out_w; ++x) {
          const int vx = vis_x0 + x;
          const double src_x = (static_cast<double>(vx - img_x_) + 0.5) / scale - 0.5;
          int x0 = static_cast<int>(std::floor(src_x));
          int x1 = x0 + 1;
          double wx = src_x - static_cast<double>(x0);
          if (x0 < 0) {
            x0 = x1 = 0;
            wx = 0.0;
          } else if (x1 >= src_w_) {
            x1 = x0 = src_w_ - 1;
            wx = 0.0;
          }

          const size_t p00 = (static_cast<size_t>(y0) * static_cast<size_t>(src_w_) + static_cast<size_t>(x0)) * 3u;
          const size_t p10 = (static_cast<size_t>(y0) * static_cast<size_t>(src_w_) + static_cast<size_t>(x1)) * 3u;
          const size_t p01 = (static_cast<size_t>(y1) * static_cast<size_t>(src_w_) + static_cast<size_t>(x0)) * 3u;
          const size_t p11 = (static_cast<size_t>(y1) * static_cast<size_t>(src_w_) + static_cast<size_t>(x1)) * 3u;
          const size_t di =
              (static_cast<size_t>(y) * static_cast<size_t>(out_w) + static_cast<size_t>(x)) * 3u;

          const double w00 = (1.0 - wx) * (1.0 - wy);
          const double w10 = wx * (1.0 - wy);
          const double w01 = (1.0 - wx) * wy;
          const double w11 = wx * wy;

          for (int c = 0; c < 3; ++c) {
            const double v =
                src_pixels_[p00 + c] * w00 + src_pixels_[p10 + c] * w10 +
                src_pixels_[p01 + c] * w01 + src_pixels_[p11 + c] * w11;
            scaled_view_[di + static_cast<size_t>(c)] =
                static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
          }
        }
      }
    }

    fl_draw_image(scaled_view_.data(), viewport_x() + vis_x0, viewport_y() + vis_y0, out_w, out_h, 3);
  }

  void show_context_menu() {
    const int image_flags = has_image_ ? 0 : FL_MENU_INACTIVE;
    const int gimp_flags = gimp_available_ ? 0 : FL_MENU_INACTIVE;
    const int inkscape_flags = inkscape_available_ ? 0 : FL_MENU_INACTIVE;
    Fl_Menu_Item items[13] = {};
    items[0] = {"Copy", 'c', nullptr, nullptr, image_flags, 0, 0, 0, 0};
    items[1] = {"Reload", 'r', nullptr, nullptr, image_flags | FL_MENU_DIVIDER, 0, 0, 0, 0};
    items[2] = {"Previous File", 'p', nullptr, nullptr, image_flags, 0, 0, 0, 0};
    items[3] = {"Next File", 'n', nullptr, nullptr, image_flags, 0, 0, 0, 0};
    items[4] = {"Open File...", 'o', nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0};
    items[5] = {"Zoom In", '+', nullptr, nullptr, image_flags, 0, 0, 0, 0};
    items[6] = {"Zoom Out", '-', nullptr, nullptr, image_flags, 0, 0, 0, 0};
    items[7] = {"Zoom Reset", '0', nullptr, nullptr, image_flags, 0, 0, 0, 0};
    items[8] = {"Fit to Window", 'f', nullptr, nullptr, image_flags, 0, 0, 0, 0};
    items[9] = {"Toggle Fullscreen", FL_F + 11, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0};
    items[10] = {"Open with GIMP", 'g', nullptr, nullptr, image_flags | gimp_flags, 0, 0, 0, 0};
    items[11] = {"Open with Inkscape", 'i', nullptr, nullptr, image_flags | inkscape_flags, 0, 0, 0, 0};
    for (int i = 0; i < 12; ++i) {
      items[i].labelfont(menu_font_);
      items[i].labelsize(menu_font_size_);
    }

    Fl_Menu_Button popup_btn(Fl::event_x(), Fl::event_y(), 0, 0);
    popup_btn.type(Fl_Menu_Button::POPUP3);
    popup_btn.menu(items);
    const Fl_Menu_Item* chosen = popup_btn.popup();
    if (!chosen) return;

    if (chosen == &items[0]) {
      if (copy_cb_) copy_cb_();
    } else if (chosen == &items[1]) {
      if (reload_cb_) reload_cb_();
    } else if (chosen == &items[2]) {
      if (navigate_cb_) (void)navigate_cb_(-1);
    } else if (chosen == &items[3]) {
      if (navigate_cb_) (void)navigate_cb_(1);
    } else if (chosen == &items[4]) {
      if (open_file_cb_) open_file_cb_();
    } else if (chosen == &items[5]) {
      zoom_by(kZoomStep, viewport_w() / 2, viewport_h() / 2);
    } else if (chosen == &items[6]) {
      zoom_by(1.0 / kZoomStep, viewport_w() / 2, viewport_h() / 2);
    } else if (chosen == &items[7]) {
      reset_zoom();
    } else if (chosen == &items[8]) {
      fit_to_window();
    } else if (chosen == &items[9]) {
      if (toggle_fullscreen_cb_) toggle_fullscreen_cb_();
    } else if (chosen == &items[10]) {
      if (open_gimp_cb_) open_gimp_cb_();
    } else if (chosen == &items[11]) {
      if (open_inkscape_cb_) open_inkscape_cb_();
    }
  }

  bool handle_context_menu_shortcut() {
    const int key = Fl::event_key();
    const bool shift = (Fl::event_state() & FL_SHIFT) != 0;
    if (key == FL_Menu || (shift && key == (FL_F + 10))) {
      show_context_menu();
      return true;
    }
    return false;
  }

  bool handle_zoom_shortcuts() {
    const int key = Fl::event_key();
    if (key == '+' || key == '=' || key == (FL_KP + '+')) {
      zoom_by(kZoomStep, viewport_w() / 2, viewport_h() / 2);
      return true;
    }
    if (key == '-' || key == '_' || key == (FL_KP + '-')) {
      zoom_by(1.0 / kZoomStep, viewport_w() / 2, viewport_h() / 2);
      return true;
    }
    if (key == '0' || key == (FL_KP + '0')) {
      reset_zoom();
      return true;
    }
    if (key == 'f' || key == 'F') {
      fit_to_window();
      return true;
    }
    return false;
  }

  bool handle_navigation_shortcuts() {
    const int key = Fl::event_key();
    const bool prev_key = (key == 'p' || key == 'P');
    const bool next_key = (key == 'n' || key == 'N');
    if (prev_key) {
      return navigate_cb_ ? navigate_cb_(-1) : false;
    }
    if (next_key) {
      return navigate_cb_ ? navigate_cb_(1) : false;
    }
    if (key == FL_Left) {
      return navigate_cb_ ? navigate_cb_(-1) : false;
    }
    if (key == FL_Right) {
      return navigate_cb_ ? navigate_cb_(1) : false;
    }
    return false;
  }

  bool handle_copy_shortcut() {
    const int key = Fl::event_key();
    if (key == 'c' || key == 'C') {
      if (copy_cb_) {
        copy_cb_();
      }
      return true;
    }
    if ((Fl::event_state() & FL_CTRL) && key == 3) {  // Ctrl+C (ETX)
      if (copy_cb_) {
        copy_cb_();
      }
      return true;
    }
    return false;
  }

  bool handle_reload_shortcut() {
    const int key = Fl::event_key();
    if (key == 'r' || key == 'R') {
      if (reload_cb_) {
        reload_cb_();
      }
      return true;
    }
    return false;
  }

  bool handle_open_shortcut() {
    const int key = Fl::event_key();
    if (key == 'o' || key == 'O' || ((Fl::event_state() & FL_CTRL) && key == ('o' & 0x1f))) {
      if (open_file_cb_) {
        open_file_cb_();
      }
      return true;
    }
    return false;
  }

  bool handle_fullscreen_shortcut() {
    const int key = Fl::event_key();
    if (key == (FL_F + 11)) {
      if (toggle_fullscreen_cb_) toggle_fullscreen_cb_();
      return true;
    }
    return false;
  }

  bool handle_escape_shortcut() {
    const int key = Fl::event_key();
    if (key == FL_Escape) {
      if (exit_fullscreen_cb_ && exit_fullscreen_cb_()) {
        return true;
      }
    }
    return false;
  }

  bool handle_external_open_shortcuts() {
    const int key = Fl::event_key();
    const bool gimp_key = (key == 'g' || key == 'G');
    if (gimp_key) {
      if (open_gimp_cb_) {
        open_gimp_cb_();
      }
      return true;
    }

    const bool inkscape_key = (key == 'i' || key == 'I');
    if (inkscape_key) {
      if (open_inkscape_cb_) {
        open_inkscape_cb_();
      }
      return true;
    }

    return false;
  }

  void pan_view_by(int dx, int dy) {
    if (!has_image_) return;
    const int old_x = img_x_;
    const int old_y = img_y_;
    img_x_ -= dx;
    img_y_ -= dy;
    clamp_offsets();
    if (img_x_ != old_x || img_y_ != old_y) {
      redraw();
    }
  }

  void pan_image_by(int dx, int dy) {
    if (!has_image_) return;
    const int old_x = img_x_;
    const int old_y = img_y_;
    img_x_ += dx;
    img_y_ += dy;
    clamp_offsets();
    if (img_x_ != old_x || img_y_ != old_y) {
      redraw();
    }
  }

  void zoom_by(double factor, int anchor_x, int anchor_y) {
    if (!has_image_) return;
    const double old_scale = current_scale();
    const double next_zoom = std::clamp(zoom_ * factor, kZoomMin, kZoomMax);
    if (next_zoom == zoom_) {
      return;
    }

    anchor_x = std::clamp(anchor_x, 0, viewport_w());
    anchor_y = std::clamp(anchor_y, 0, viewport_h());

    const double src_x = (static_cast<double>(anchor_x) - static_cast<double>(img_x_)) / old_scale;
    const double src_y = (static_cast<double>(anchor_y) - static_cast<double>(img_y_)) / old_scale;

    zoom_ = next_zoom;
    const double new_scale = current_scale();

    img_x_ = static_cast<int>(anchor_x - src_x * new_scale);
    img_y_ = static_cast<int>(anchor_y - src_y * new_scale);
    clamp_offsets();
    redraw();
  }

  void reset_zoom() {
    zoom_ = 1.0;
    img_x_ = 0;
    img_y_ = 0;
    clamp_offsets();
    redraw();
  }

  void fit_to_window() {
    zoom_ = fit_scale_full();
    img_x_ = 0;
    img_y_ = 0;
    clamp_offsets();
    redraw();
  }

  void fit_to_window_no_upscale() {
    zoom_ = fit_scale_no_upscale();
    img_x_ = 0;
    img_y_ = 0;
    clamp_offsets();
    redraw();
  }

  void sync_image_pointers() {
    src_pixels_ = image_.rgb.data();
    src_w_ = image_.w;
    src_h_ = image_.h;
    scaled_view_.clear();
  }

  LoadedImage image_;
  AnimatedState animation_;
  std::unique_ptr<AnimationEngine> anim_engine_;
  bool has_animation_ = false;
  bool animation_timer_active_ = false;
  bool has_image_ = false;
  const unsigned char* src_pixels_ = nullptr;
  int src_w_ = 0;
  int src_h_ = 0;
  std::vector<unsigned char> scaled_view_;
  int img_x_ = 0;
  int img_y_ = 0;
  bool dragging_ = false;
  int drag_last_x_ = 0;
  int drag_last_y_ = 0;
  double zoom_ = 1.0;  // Absolute image scale; 1.0 = 100% native size.
  bool pan_w_ = false;
  bool pan_a_ = false;
  bool pan_s_ = false;
  bool pan_d_ = false;
  bool pan_timer_active_ = false;
  bool zoom_in_ = false;
  bool zoom_out_ = false;
  bool zoom_timer_active_ = false;
  std::function<bool(int)> navigate_cb_;
  std::function<void()> copy_cb_;
  std::function<void()> reload_cb_;
  std::function<void()> open_file_cb_;
  std::function<void()> open_gimp_cb_;
  std::function<void()> open_inkscape_cb_;
  std::function<void()> toggle_fullscreen_cb_;
  std::function<bool()> exit_fullscreen_cb_;
  bool gimp_available_ = false;
  bool inkscape_available_ = false;
  Fl_Color frame_bg_color_ = fl_rgb_color(kDefaultFrameBgR, kDefaultFrameBgG, kDefaultFrameBgB);
  Fl_Font menu_font_ = FL_HELVETICA;
  Fl_Fontsize menu_font_size_ = FL_NORMAL_SIZE;
};

class AppWindow : public Fl_Double_Window {
 public:
  AppWindow(int W, int H, const char* L) : Fl_Double_Window(W, H, L) {}

  void set_layout_widgets(ImageView* view, Fl_Box* status) {
    view_ = view;
    status_ = status;
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Double_Window::resize(X, Y, W, H);
    if (view_) {
      view_->resize(0, 0, W, std::max(1, H - kStatusBarH));
    }
    if (status_) {
      status_->resize(0, std::max(0, H - kStatusBarH), W, kStatusBarH);
    }
  }

 private:
  ImageView* view_ = nullptr;
  Fl_Box* status_ = nullptr;
};

std::vector<std::string> discover_loaders(std::string& loader_dir) {
  namespace fs = std::filesystem;
  for (const char* dir : kLoaderDirs) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      continue;
    }
    loader_dir = dir;
    std::vector<std::string> out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      std::error_code entry_ec;
      if (ec || !entry.is_regular_file(entry_ec) || entry_ec) {
        continue;
      }
      const fs::path p = entry.path();
      if (p.extension() != ".so") {
        continue;
      }
      out.push_back(p.stem().string());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }
  return {};
}

bool is_aux_loader(const std::string& name) {
  static const std::set<std::string> kAux = {"argb", "bz2", "ff", "id3", "lzma", "zlib"};
  return kAux.find(name) != kAux.end();
}

void print_formats() {
  std::string loader_dir;
  const std::vector<std::string> loaders = discover_loaders(loader_dir);
  if (loaders.empty()) {
    std::printf("No imlib2 loader directory found.\n");
    return;
  }

  std::vector<std::string> image_loaders;
  std::vector<std::string> aux_loaders;
  for (const auto& loader : loaders) {
    if (is_aux_loader(loader)) {
      aux_loaders.push_back(loader);
    } else {
      image_loaders.push_back(loader);
    }
  }

  std::printf("imlib2 loaders: %s\n", loader_dir.c_str());
  std::printf("image formats:\n");
  for (const auto& name : image_loaders) {
    std::printf("  %s\n", name.c_str());
  }
  if (!aux_loaders.empty()) {
    std::printf("auxiliary loaders:\n");
    for (const auto& name : aux_loaders) {
      std::printf("  %s\n", name.c_str());
    }
  }
}

void print_usage(const char* argv0) {
  std::fprintf(stdout, "Usage:\n");
  std::fprintf(stdout, "  %s [--config <path>] [image-file]\n", argv0);
  std::fprintf(stdout, "  %s --list-formats\n", argv0);
  std::fprintf(stdout, "  %s --help\n", argv0);
  std::fprintf(stdout, "\nOptions:\n");
  std::fprintf(stdout, "  --config <path>   Load UI config file from path\n");
  std::fprintf(stdout, "  --list-formats    List imlib2 loaders and exit\n");
  std::fprintf(stdout, "  -h, --help        Show this help and exit\n");
}

std::filesystem::path default_config_path() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0]) {
    return std::filesystem::path(xdg) / "fliv" / "config.ini";
  }
  if (const char* home = std::getenv("HOME"); home && home[0]) {
    return std::filesystem::path(home) / ".config" / "fliv" / "config.ini";
  }
  return std::filesystem::path("config.ini");
}

bool parse_cli(int argc, char** argv, CliOptions& out, std::string& err_out) {
  out = {};
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      out.help = true;
      continue;
    }
    if (arg == "--list-formats") {
      out.list_formats = true;
      continue;
    }
    if (arg == "--config") {
      if (i + 1 >= argc) {
        err_out = "--config requires a path";
        return false;
      }
      out.config_file = std::filesystem::path(argv[++i]);
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      err_out = "unknown option: " + arg;
      return false;
    }
    if (out.image_file.has_value()) {
      err_out = "only one image file argument is supported";
      return false;
    }
    out.image_file = std::filesystem::path(arg);
  }
  if (out.list_formats && out.image_file.has_value()) {
    err_out = "--list-formats cannot be combined with image file argument";
    return false;
  }
  return true;
}

bool load_ui_config_file(const std::filesystem::path& path, UiConfig& cfg, std::string& err_out) {
  std::ifstream in(path);
  if (!in) {
    err_out = "cannot open config file";
    return false;
  }

  bool in_ui = false;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    std::string s = trim(line);
    if (s.empty() || s[0] == '#' || s[0] == ';') continue;
    if (s.front() == '[' && s.back() == ']') {
      std::string section = trim(s.substr(1, s.size() - 2));
      in_ui = (section == "ui");
      continue;
    }
    if (!in_ui) continue;

    const size_t eq = s.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(s.substr(0, eq));
    std::string val = trim(s.substr(eq + 1));
    if (!val.empty() && ((val.front() == '"' && val.back() == '"') ||
                         (val.front() == '\'' && val.back() == '\''))) {
      val = val.substr(1, val.size() - 2);
    }

    unsigned char r = 0, g = 0, b = 0;
    if (key == "frame_bg") {
      if (parse_hex_color(val, r, g, b)) {
        cfg.frame_bg_r = r; cfg.frame_bg_g = g; cfg.frame_bg_b = b;
      }
    } else if (key == "status_bg") {
      if (parse_hex_color(val, r, g, b)) {
        cfg.status_bg_r = r; cfg.status_bg_g = g; cfg.status_bg_b = b;
      }
    } else if (key == "status_fg") {
      if (parse_hex_color(val, r, g, b)) {
        cfg.status_fg_r = r; cfg.status_fg_g = g; cfg.status_fg_b = b;
      }
    } else if (key == "font") {
      cfg.font_name = val;
    } else if (key == "font_size") {
      int sz = 0;
      if (parse_int_in_range(val, 6, 96, sz)) {
        cfg.font_size = sz;
      }
    }
  }
  return true;
}

std::string make_title(const std::filesystem::path& file) {
  return "fliv - " + file.filename().string();
}

std::vector<std::filesystem::path> list_directory_files(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    std::error_code entry_ec;
    if (ec || !entry.is_regular_file(entry_ec) || entry_ec) {
      continue;
    }
    out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
    return a.filename().string() < b.filename().string();
  });
  return out;
}

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

bool command_exists(const char* cmd) {
  const std::string probe = std::string("command -v ") + cmd + " >/dev/null 2>&1";
  return std::system(probe.c_str()) == 0;
}

std::string detect_mime_type(const std::filesystem::path& p) {
  static std::mutex mu;
  static bool initialized = false;
  static bool available = false;
  static magic_t cookie = nullptr;

  std::lock_guard<std::mutex> lk(mu);
  if (!initialized) {
    cookie = magic_open(MAGIC_MIME_TYPE);
    if (cookie && magic_load(cookie, nullptr) == 0) {
      available = true;
    } else if (cookie) {
      magic_close(cookie);
      cookie = nullptr;
    }
    initialized = true;
  }

  if (available && cookie) {
    const std::string ps = p.string();
    const char* mime = magic_file(cookie, ps.c_str());
    if (mime && mime[0]) {
      return std::string(mime);
    }
  }
  return "application/octet-stream";
}

FileMetadata build_file_metadata(const std::filesystem::path& file) {
  FileMetadata meta;
  meta.path = file;
  meta.mime = detect_mime_type(file);
  std::error_code ec;
  meta.size_bytes = std::filesystem::file_size(file, ec);
  if (ec) {
    meta.size_bytes = 0;
  }
  return meta;
}

bool copy_file_to_clipboard(const FileMetadata& meta) {
  const std::string fq = shell_quote(meta.path.string());
  const std::string mq = shell_quote(meta.mime);

  if (std::getenv("WAYLAND_DISPLAY") && command_exists("wl-copy")) {
    const std::string cmd = "wl-copy --type " + mq + " < " + fq;
    return std::system(cmd.c_str()) == 0;
  }
  if (command_exists("xclip")) {
    const std::string cmd = "xclip -selection clipboard -t " + mq + " -i " + fq;
    return std::system(cmd.c_str()) == 0;
  }
  return false;
}

bool launch_app_if_available(const char* app, const std::filesystem::path& file) {
  if (!command_exists(app)) {
    return false;
  }
  const std::string cmd =
      std::string(app) + " " + shell_quote(file.string()) + " >/dev/null 2>&1 &";
  const int rc = std::system(cmd.c_str());
  return rc == 0;
}

std::string human_size(uintmax_t bytes) {
  static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  char buf[64];
  if (unit == 0) {
    std::snprintf(buf, sizeof(buf), "%.0f %s", value, kUnits[unit]);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
  }
  return std::string(buf);
}

std::string make_status_text(const FileMetadata& meta) {
  if (meta.path.empty()) {
    return "No image loaded | Press O / Ctrl+O or right-click -> Open Image...";
  }
  return meta.path.filename().string() + " | " + meta.mime + " | " +
         human_size(meta.size_bytes) + " | " +
         std::to_string(meta.width) + "x" + std::to_string(meta.height);
}

bool pick_image_file(std::filesystem::path& out_file, const std::filesystem::path& start_dir) {
  Fl_Native_File_Chooser chooser;
  chooser.title("Open Image");
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.filter(
      "Images\t*.{avif,bmp,gif,heic,heif,ico,j2k,jp2,jpeg,jpg,jxl,pbm,pgm,png,pnm,ppm,qoi,svg,tga,tif,tiff,webp,xbm,xpm}\n"
      "All Files\t*");
  if (!start_dir.empty()) {
    chooser.directory(start_dir.string().c_str());
  }

  const int rc = chooser.show();
  if (rc != 0) {
    return false;
  }

  const char* selected = chooser.filename();
  if (!selected || !selected[0]) {
    return false;
  }
  out_file = std::filesystem::absolute(std::filesystem::path(selected)).lexically_normal();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions cli;
  std::string cli_err;
  if (!parse_cli(argc, argv, cli, cli_err)) {
    std::fprintf(stderr, "%s\n", cli_err.c_str());
    print_usage(argv[0]);
    return 1;
  }

  if (cli.help) {
    print_usage(argv[0]);
    return 0;
  }

  if (cli.list_formats) {
    print_formats();
    return 0;
  }

  namespace fs = std::filesystem;
  UiConfig ui_cfg;
  std::string cfg_err;
  const fs::path cfg_path = cli.config_file.has_value()
      ? fs::absolute(*cli.config_file).lexically_normal()
      : default_config_path();
  if (fs::exists(cfg_path)) {
    if (!load_ui_config_file(cfg_path, ui_cfg, cfg_err)) {
      std::fprintf(stderr, "Config load failed: %s (%s)\n", cfg_path.string().c_str(), cfg_err.c_str());
      if (cli.config_file.has_value()) {
        return 1;
      }
    }
  } else if (cli.config_file.has_value()) {
    std::fprintf(stderr, "Config file not found: %s\n", cfg_path.string().c_str());
    return 1;
  }

  fs::path current_file;
  fs::path current_dir = fs::current_path();
  FileMetadata current_meta;
  std::vector<fs::path> dir_files;
  size_t current_index = 0;

  DecodedImage decoded;
  bool have_initial_image = false;
  if (cli.image_file.has_value()) {
    current_file = fs::absolute(*cli.image_file).lexically_normal();
    current_dir = current_file.parent_path();
    std::string load_err;
    if (!load_image_decoded(current_file.string().c_str(), decoded, load_err)) {
      std::fprintf(stderr, "Failed to load image: %s (%s)\n", current_file.string().c_str(), load_err.c_str());
      return 2;
    }
    current_meta = build_file_metadata(current_file);
    current_meta.width = decoded.width();
    current_meta.height = decoded.height();
    dir_files = list_directory_files(current_dir);
    for (size_t i = 0; i < dir_files.size(); ++i) {
      if (fs::absolute(dir_files[i]).lexically_normal() == current_file) {
        current_index = i;
        break;
      }
    }
    have_initial_image = true;
  }
 
  int sx = 0;
  int sy = 0;
  int sw = 0;
  int sh = 0;
  Fl::screen_xywh(sx, sy, sw, sh, 0);

  const int wanted_w = have_initial_image ? (decoded.width() + 2 * kPadding) : 960;
  const int wanted_h = have_initial_image ? (decoded.height() + 2 * kPadding + kStatusBarH) : 720;

  const int win_w = std::clamp(wanted_w, kMinWindowW, static_cast<int>(sw * 0.9));
  const int win_h = std::clamp(wanted_h, kMinWindowH, static_cast<int>(sh * 0.9));

  const std::string title = have_initial_image ? make_title(current_file) : "fliv";
  AppWindow win(win_w, win_h, title.c_str());
  win.begin();
  const Fl_Color frame_bg = fl_rgb_color(ui_cfg.frame_bg_r, ui_cfg.frame_bg_g, ui_cfg.frame_bg_b);
  const Fl_Color status_bg = fl_rgb_color(ui_cfg.status_bg_r, ui_cfg.status_bg_g, ui_cfg.status_bg_b);
  const Fl_Color status_fg = fl_rgb_color(ui_cfg.status_fg_r, ui_cfg.status_fg_g, ui_cfg.status_fg_b);
  win.color(frame_bg);
  ImageView view(0, 0, win_w, std::max(1, win_h - kStatusBarH), LoadedImage{});
  view.set_frame_background_color(frame_bg);
  Fl_Box status(0, std::max(0, win_h - kStatusBarH), win_w, kStatusBarH);
  win.set_layout_widgets(&view, &status);
  status.box(FL_FLAT_BOX);
  status.color(status_bg);
  status.labelcolor(status_fg);
  Fl_Font status_font = FL_HELVETICA;
  if (!ui_cfg.font_name.empty()) {
    status_font = resolve_font_name(ui_cfg.font_name);
  }
  status.labelfont(status_font);
  status.labelsize(ui_cfg.font_size);
  status.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  status.copy_label(make_status_text(current_meta).c_str());
  view.set_menu_font(status_font, static_cast<Fl_Fontsize>(ui_cfg.font_size));
  win.resizable(&view);
  view.set_external_app_availability(command_exists("gimp"), command_exists("inkscape"));

  auto apply_decoded_to_view = [&](DecodedImage&& media) {
    if (media.animated) {
      view.set_animation(std::move(media.anim), std::move(media.frame));
    } else if (media.frame.w > 0 && media.frame.h > 0 && !media.frame.rgb.empty()) {
      view.set_image(std::move(media.frame));
    } else {
      view.clear_image();
    }
  };

  if (have_initial_image) {
    apply_decoded_to_view(std::move(decoded));
  }

  auto load_and_apply = [&](const fs::path& file_to_open, bool refresh_dir) -> bool {
    DecodedImage next;
    std::string err;
    const fs::path abs = fs::absolute(file_to_open).lexically_normal();
    if (!load_image_decoded(abs.string().c_str(), next, err)) {
      std::fprintf(stderr, "Failed to load image: %s (%s)\n", abs.string().c_str(), err.c_str());
      return false;
    }

    current_file = abs;
    current_meta = build_file_metadata(current_file);
    current_meta.width = next.width();
    current_meta.height = next.height();
    if (refresh_dir) {
      current_dir = current_file.parent_path();
      dir_files = list_directory_files(current_dir);
      current_index = 0;
      for (size_t i = 0; i < dir_files.size(); ++i) {
        if (fs::absolute(dir_files[i]).lexically_normal() == current_file) {
          current_index = i;
          break;
        }
      }
    }
    win.copy_label(make_title(current_file).c_str());
    status.copy_label(make_status_text(current_meta).c_str());
    apply_decoded_to_view(std::move(next));
    return true;
  };

  view.set_navigate_callback([&](int dir) -> bool {
    if (current_file.empty() || dir_files.empty()) {
      return false;
    }
    const int n = static_cast<int>(dir_files.size());
    if (n <= 1) {
      return false;
    }
    const int step = (dir >= 0) ? 1 : -1;
    int i = static_cast<int>(current_index);
    for (int tried = 0; tried < n - 1; ++tried) {
      i = (i + step + n) % n;
      const std::string candidate = dir_files[static_cast<size_t>(i)].string();
      if (load_and_apply(candidate, true)) {
        return true;
      }
    }
    return false;
  });

  view.set_copy_callback([&]() {
    if (current_file.empty()) return;
    if (!copy_file_to_clipboard(current_meta)) {
      std::fprintf(stderr,
                   "Copy failed for %s (need wl-copy on Wayland or xclip on X11)\n",
                   current_meta.path.string().c_str());
    }
  });
  view.set_reload_callback([&]() {
    if (current_file.empty()) return;
    (void)load_and_apply(current_file, true);
  });
  view.set_open_file_callback([&]() {
    fs::path selected;
    if (pick_image_file(selected, current_dir)) {
      (void)load_and_apply(selected, true);
    }
  });
  view.set_open_gimp_callback([&]() {
    if (!current_file.empty()) (void)launch_app_if_available("gimp", current_meta.path);
  });
  view.set_open_inkscape_callback(
      [&]() {
        if (!current_file.empty()) (void)launch_app_if_available("inkscape", current_meta.path);
      });
  view.set_toggle_fullscreen_callback([&]() {
    if (win.fullscreen_active()) {
      win.fullscreen_off();
    } else {
      win.fullscreen();
    }
  });
  view.set_exit_fullscreen_callback([&]() -> bool {
    if (win.fullscreen_active()) {
      win.fullscreen_off();
      return true;
    }
    return false;
  });

  win.end();
  win.show();
  Fl::focus(&view);
  const int rc = Fl::run();

  return rc;
}
