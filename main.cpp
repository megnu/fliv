#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>
#include <FL/Fl_RGB_Image.H>

#include <Imlib2.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kPadding = 16;
constexpr int kMinWindowW = 240;
constexpr int kMinWindowH = 180;
constexpr double kZoomStep = 1.10;
constexpr double kZoomMin = 0.05;
constexpr double kZoomMax = 40.0;
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
  ImageView(int X, int Y, int W, int H, Fl_RGB_Image* src)
      : Fl_Widget(X, Y, W, H), src_(src) {}

  int handle(int event) override {
    switch (event) {
      case FL_FOCUS:
      case FL_UNFOCUS:
        return 1;
      case FL_PUSH:
        take_focus();
        return 1;
      case FL_MOUSEWHEEL:
        if (Fl::event_dy() < 0) {
          zoom_by(kZoomStep, Fl::event_x() - viewport_x(), Fl::event_y() - viewport_y());
        } else if (Fl::event_dy() > 0) {
          zoom_by(1.0 / kZoomStep, Fl::event_x() - viewport_x(), Fl::event_y() - viewport_y());
        }
        return 1;
      case FL_KEYDOWN:
      case FL_SHORTCUT:
        if (handle_zoom_shortcuts()) {
          return 1;
        }
        break;
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
    ensure_scaled_image(draw_w, draw_h);

    const int draw_x = viewport_x() + img_x_;
    const int draw_y = viewport_y() + img_y_;
    fl_color(fl_rgb_color(12, 12, 12));
    fl_rectf(draw_x - 1, draw_y - 1, draw_w + 2, draw_h + 2);

    if (scaled_) {
      scaled_->draw(draw_x, draw_y);
    }

    fl_pop_clip();
  }

  ~ImageView() override { delete scaled_; }

 private:
  int viewport_x() const { return x() + kPadding; }
  int viewport_y() const { return y() + kPadding; }
  int viewport_w() const { return std::max(1, w() - 2 * kPadding); }
  int viewport_h() const { return std::max(1, h() - 2 * kPadding); }

  double fit_scale() const {
    const double sx = static_cast<double>(viewport_w()) / static_cast<double>(src_->w());
    const double sy = static_cast<double>(viewport_h()) / static_cast<double>(src_->h());
    return std::min(1.0, std::min(sx, sy));
  }

  double current_scale() const { return std::clamp(fit_scale() * zoom_, kZoomMin, kZoomMax); }

  int scaled_w() const { return std::max(1, static_cast<int>(src_->w() * current_scale())); }
  int scaled_h() const { return std::max(1, static_cast<int>(src_->h() * current_scale())); }

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

  void ensure_scaled_image(int w, int h) {
    if (scaled_ && scaled_w_ == w && scaled_h_ == h) {
      return;
    }
    delete scaled_;
    scaled_ = static_cast<Fl_RGB_Image*>(src_->copy(w, h));
    scaled_w_ = w;
    scaled_h_ = h;
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

  Fl_RGB_Image* src_ = nullptr;
  Fl_RGB_Image* scaled_ = nullptr;
  int scaled_w_ = 0;
  int scaled_h_ = 0;
  int img_x_ = 0;
  int img_y_ = 0;
  double zoom_ = 1.0;  // Relative to fit-to-window scale.
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

  LoadedImage loaded;
  std::string load_err;
  if (!load_image_rgb(argv[1], loaded, load_err)) {
    std::fprintf(stderr, "Failed to load image: %s (%s)\n", argv[1], load_err.c_str());
    return 2;
  }

  auto* src = new Fl_RGB_Image(loaded.rgb.data(), loaded.w, loaded.h, 3);
  if (!src || !src->data()[0]) {
    std::fprintf(stderr, "Failed to build FLTK image\n");
    delete src;
    return 3;
  }

  // Data is owned by `loaded.rgb`; do not let FLTK free it.
  src->alloc_array = 0;

  int sx = 0;
  int sy = 0;
  int sw = 0;
  int sh = 0;
  Fl::screen_xywh(sx, sy, sw, sh, 0);

  const int wanted_w = loaded.w + 2 * kPadding;
  const int wanted_h = loaded.h + 2 * kPadding;

  const int win_w = std::clamp(wanted_w, kMinWindowW, static_cast<int>(sw * 0.9));
  const int win_h = std::clamp(wanted_h, kMinWindowH, static_cast<int>(sh * 0.9));

  Fl_Double_Window win(win_w, win_h, "fliv - stage 1");
  win.color(fl_rgb_color(kBgR, kBgG, kBgB));
  ImageView view(0, 0, win_w, win_h, src);
  win.resizable(&view);

  win.end();
  win.show();
  Fl::focus(&view);
  const int rc = Fl::run();

  delete src;
  return rc;
}
