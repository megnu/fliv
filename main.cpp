#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Box.H>
#include <FL/fl_draw.H>
#include <FL/Fl_RGB_Image.H>

#include <Imlib2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kPadding = 16;
constexpr int kMinWindowW = 240;
constexpr int kMinWindowH = 180;
constexpr int kStatusBarH = 24;
constexpr double kZoomStep = 1.10;
constexpr double kZoomMin = 0.05;
constexpr double kZoomMax = 40.0;
constexpr int kPanStep = 24;
constexpr double kPanTickSeconds = 1.0 / 30.0;
constexpr const char* kLoaderDirs[] = {
    "/usr/lib/imlib2/loaders",
    "/usr/lib64/imlib2/loaders",
    "/usr/local/lib/imlib2/loaders",
    "/usr/local/lib64/imlib2/loaders",
};

struct LoadedImage {
  int w = 0;
  int h = 0;
  std::vector<unsigned char> rgb;
};

struct FileMetadata {
  std::filesystem::path path;
  std::string mime;
  uintmax_t size_bytes = 0;
};

bool load_image_rgb(const char* path, LoadedImage& out, std::string& err_out) {
  int err = 0;
  Imlib_Image im = imlib_load_image_with_errno_return(path, &err);
  if (!im) {
    const char* err_str = imlib_strerror(err);
    err_out = err_str ? err_str : "unknown load error";
    return false;
  }

  imlib_context_set_image(im);
  out.w = imlib_image_get_width();
  out.h = imlib_image_get_height();

  if (out.w <= 0 || out.h <= 0) {
    imlib_free_image();
    return false;
  }

  DATA32* data = imlib_image_get_data_for_reading_only();
  if (!data) {
    err_out = "decoder returned no pixel data";
    imlib_free_image();
    return false;
  }

  out.rgb.resize(static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 3u);
  for (int y = 0; y < out.h; ++y) {
    for (int x = 0; x < out.w; ++x) {
      DATA32 px = data[y * out.w + x];
      size_t i = (static_cast<size_t>(y) * static_cast<size_t>(out.w) + static_cast<size_t>(x)) * 3u;
      const unsigned int a = (px >> 24) & 0xff;
      const unsigned int r = (px >> 16) & 0xff;
      const unsigned int g = (px >> 8) & 0xff;
      const unsigned int b = px & 0xff;

      // Checkerboard tile under transparency.
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

  imlib_free_image();
  return true;
}

class ImageView : public Fl_Widget {
 public:
  ImageView(int X, int Y, int W, int H, LoadedImage image)
      : Fl_Widget(X, Y, W, H), image_(std::move(image)) {
    sync_image_pointers();
  }

  void set_navigate_callback(std::function<bool(int)> cb) { navigate_cb_ = std::move(cb); }
  void set_copy_callback(std::function<void()> cb) { copy_cb_ = std::move(cb); }

  void set_image(LoadedImage image) {
    image_ = std::move(image);
    sync_image_pointers();
    reset_zoom();
  }

  int handle(int event) override {
    switch (event) {
      case FL_FOCUS:
        return 1;
      case FL_UNFOCUS:
        clear_pan_keys();
        return 1;
      case FL_PUSH:
        take_focus();
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
        const bool changed = set_pan_key_state(Fl::event_key(), false);
        if (changed) {
          if (!any_pan_key_pressed()) {
            stop_pan_timer();
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

    fl_color(fl_rgb_color(30, 30, 30));
    fl_rectf(x(), y(), w(), h());

    const int draw_w = scaled_w();
    const int draw_h = scaled_h();
    clamp_offsets();

    const int draw_x = viewport_x() + img_x_;
    const int draw_y = viewport_y() + img_y_;
    fl_color(fl_rgb_color(12, 12, 12));
    fl_rectf(draw_x - 1, draw_y - 1, draw_w + 2, draw_h + 2);

    draw_visible_region();

    fl_pop_clip();
  }

  ~ImageView() override { stop_pan_timer(); }

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

  void clear_pan_keys() {
    pan_w_ = pan_a_ = pan_s_ = pan_d_ = false;
    stop_pan_timer();
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

  void pan_tick() {
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

  int viewport_x() const { return x() + kPadding; }
  int viewport_y() const { return y() + kPadding; }
  int viewport_w() const { return std::max(1, w() - 2 * kPadding); }
  int viewport_h() const { return std::max(1, h() - 2 * kPadding); }

  double fit_scale() const {
    const double sx = static_cast<double>(viewport_w()) / static_cast<double>(src_w_);
    const double sy = static_cast<double>(viewport_h()) / static_cast<double>(src_h_);
    return std::min(1.0, std::min(sx, sy));
  }

  double current_scale() const { return std::clamp(fit_scale() * zoom_, kZoomMin, kZoomMax); }

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

    // Nearest-neighbor sampling for zoom/pan. Cost scales with viewport size, not zoomed image size.
    for (int y = 0; y < out_h; ++y) {
      const int vy = vis_y0 + y;
      int sy = static_cast<int>(std::floor((vy - img_y_) / scale));
      sy = std::clamp(sy, 0, src_h_ - 1);
      for (int x = 0; x < out_w; ++x) {
        const int vx = vis_x0 + x;
        int sx = static_cast<int>(std::floor((vx - img_x_) / scale));
        sx = std::clamp(sx, 0, src_w_ - 1);

        const size_t si = (static_cast<size_t>(sy) * static_cast<size_t>(src_w_) + static_cast<size_t>(sx)) * 3u;
        const size_t di = (static_cast<size_t>(y) * static_cast<size_t>(out_w) + static_cast<size_t>(x)) * 3u;
        scaled_view_[di + 0] = src_pixels_[si + 0];
        scaled_view_[di + 1] = src_pixels_[si + 1];
        scaled_view_[di + 2] = src_pixels_[si + 2];
      }
    }

    fl_draw_image(scaled_view_.data(), viewport_x() + vis_x0, viewport_y() + vis_y0, out_w, out_h, 3);
  }

  bool handle_zoom_shortcuts() {
    if (!(Fl::event_state() & FL_CTRL)) {
      return false;
    }

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
    return false;
  }

  bool handle_navigation_shortcuts() {
    const int key = Fl::event_key();
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

  void pan_view_by(int dx, int dy) {
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

  void sync_image_pointers() {
    src_pixels_ = image_.rgb.data();
    src_w_ = image_.w;
    src_h_ = image_.h;
    scaled_view_.clear();
  }

  LoadedImage image_;
  const unsigned char* src_pixels_ = nullptr;
  int src_w_ = 0;
  int src_h_ = 0;
  std::vector<unsigned char> scaled_view_;
  int img_x_ = 0;
  int img_y_ = 0;
  bool dragging_ = false;
  int drag_last_x_ = 0;
  int drag_last_y_ = 0;
  double zoom_ = 1.0;  // Relative to fit-to-window scale.
  bool pan_w_ = false;
  bool pan_a_ = false;
  bool pan_s_ = false;
  bool pan_d_ = false;
  bool pan_timer_active_ = false;
  std::function<bool(int)> navigate_cb_;
  std::function<void()> copy_cb_;
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
      if (ec || !entry.is_regular_file()) {
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
  std::fprintf(stderr, "Usage: %s <image-file>\n", argv0);
  std::fprintf(stderr, "       %s --list-formats\n", argv0);
}

std::string make_title(const std::filesystem::path& file) {
  return "fliv - " + file.filename().string();
}

std::vector<std::filesystem::path> list_directory_files(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec || !entry.is_regular_file()) {
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

std::string to_lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

std::string guess_mime_type(const std::filesystem::path& p) {
  const std::string ext = to_lower(p.extension().string());
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".png") return "image/png";
  if (ext == ".gif") return "image/gif";
  if (ext == ".bmp") return "image/bmp";
  if (ext == ".webp") return "image/webp";
  if (ext == ".tif" || ext == ".tiff") return "image/tiff";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".avif") return "image/avif";
  if (ext == ".heif") return "image/heif";
  if (ext == ".heic") return "image/heic";
  if (ext == ".jxl") return "image/jxl";
  if (ext == ".jp2" || ext == ".j2k") return "image/jp2";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".xbm") return "image/x-xbitmap";
  if (ext == ".xpm") return "image/x-xpixmap";
  if (ext == ".pnm") return "image/x-portable-anymap";
  if (ext == ".pbm") return "image/x-portable-bitmap";
  if (ext == ".pgm") return "image/x-portable-graymap";
  if (ext == ".ppm") return "image/x-portable-pixmap";
  if (ext == ".tga") return "image/x-tga";
  if (ext == ".qoi") return "image/qoi";
  return "application/octet-stream";
}

FileMetadata build_file_metadata(const std::filesystem::path& file) {
  FileMetadata meta;
  meta.path = file;
  meta.mime = guess_mime_type(file);
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
  return meta.path.filename().string() + " | " + meta.mime + " | " + human_size(meta.size_bytes);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--list-formats") {
    print_formats();
    return 0;
  }

  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  constexpr unsigned char kBgR = 30;
  constexpr unsigned char kBgG = 30;
  constexpr unsigned char kBgB = 30;

  namespace fs = std::filesystem;
  fs::path current_file = fs::absolute(fs::path(argv[1])).lexically_normal();
  fs::path current_dir = current_file.parent_path();
  FileMetadata current_meta = build_file_metadata(current_file);
  std::vector<fs::path> dir_files = list_directory_files(current_dir);

  size_t current_index = 0;
  for (size_t i = 0; i < dir_files.size(); ++i) {
    if (fs::absolute(dir_files[i]).lexically_normal() == current_file) {
      current_index = i;
      break;
    }
  }

  LoadedImage loaded;
  std::string load_err;
  if (!load_image_rgb(current_file.string().c_str(), loaded, load_err)) {
    std::fprintf(stderr, "Failed to load image: %s (%s)\n", current_file.string().c_str(), load_err.c_str());
    return 2;
  }
 
  int sx = 0;
  int sy = 0;
  int sw = 0;
  int sh = 0;
  Fl::screen_xywh(sx, sy, sw, sh, 0);

  const int wanted_w = loaded.w + 2 * kPadding;
  const int wanted_h = loaded.h + 2 * kPadding;

  const int win_w = std::clamp(wanted_w, kMinWindowW, static_cast<int>(sw * 0.9));
  const int win_h = std::clamp(wanted_h, kMinWindowH, static_cast<int>(sh * 0.9));

  const std::string title = make_title(current_file);
  AppWindow win(win_w, win_h, title.c_str());
  win.begin();
  win.color(fl_rgb_color(kBgR, kBgG, kBgB));
  ImageView view(0, 0, win_w, std::max(1, win_h - kStatusBarH), std::move(loaded));
  Fl_Box status(0, std::max(0, win_h - kStatusBarH), win_w, kStatusBarH);
  win.set_layout_widgets(&view, &status);
  status.box(FL_FLAT_BOX);
  status.color(fl_rgb_color(20, 20, 20));
  status.labelcolor(fl_rgb_color(230, 230, 230));
  status.labelfont(FL_HELVETICA);
  status.labelsize(13);
  status.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  status.copy_label(make_status_text(current_meta).c_str());
  win.resizable(&view);

  view.set_navigate_callback([&](int dir) -> bool {
    if (dir_files.empty()) {
      return false;
    }

    for (int i = static_cast<int>(current_index) + dir;
         i >= 0 && i < static_cast<int>(dir_files.size()); i += dir) {
      LoadedImage next;
      std::string err;
      const std::string candidate = dir_files[static_cast<size_t>(i)].string();
      if (load_image_rgb(candidate.c_str(), next, err)) {
        current_index = static_cast<size_t>(i);
        current_file = fs::absolute(dir_files[current_index]).lexically_normal();
        current_meta = build_file_metadata(current_file);
        const std::string new_title = make_title(current_file);
        win.copy_label(new_title.c_str());
        status.copy_label(make_status_text(current_meta).c_str());
        view.set_image(std::move(next));
        return true;
      }
    }
    return false;
  });

  view.set_copy_callback([&]() {
    if (!copy_file_to_clipboard(current_meta)) {
      std::fprintf(stderr,
                   "Copy failed for %s (need wl-copy on Wayland or xclip on X11)\n",
                   current_meta.path.string().c_str());
    }
  });

  win.end();
  win.show();
  Fl::focus(&view);
  const int rc = Fl::run();

  return rc;
}
