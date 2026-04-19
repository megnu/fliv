# fliv
![fliv icon](fliv.png)

A simple FLTK 1.4 + imlib2 image viewer with an OpenGL image canvas.

SPDX-License-Identifier: GPL-2.0-or-later

https://github.com/user-attachments/assets/548e3667-b4ea-41b6-8d0a-dd7326619969

## Requirements

Build requirements:

- C++17 compiler
- `make`
- `pkg-config`
- `fltk-config`

Runtime/library requirements:

- FLTK 1.4
- imlib2
- libmagic
- OpenGL 1.1+ compatible graphics stack/driver

Optional runtime tools:

- `wl-copy` for copy on Wayland
- `xclip` for copy on X11
- `gimp` for Open with GIMP action
- `inkscape` for Open with Inkscape action

Optional imlib2 format loader libraries:

- libheif for HEIF/AVIF
- libid3tag for ID3
- libjxl for JPEG XL (JXL)
- librsvg for SVG
- libspectre for PostScript (PS)
- libwebp for WEBP
- openjpeg2 for JPEG 2000 (J2K)

## Arch Packages

Required Arch packages:

- `fltk`
- `imlib2`
- `file`
- `make`
- `pkgconf`
- C++ compiler toolchain (`gcc` or `clang`)

Optional Arch packages:

- `wl-clipboard` (provides `wl-copy`)
- `xclip`
- `gimp`
- `inkscape`
- `libheif`
- `libid3tag`
- `libjxl`
- `librsvg`
- `libspectre`
- `libwebp`
- `openjpeg2`

## Build From Source

```bash
make
```

## Run

```bash
./fliv /path/to/image.png
```

```bash
./fliv
```

```bash
./fliv --config /path/to/config.ini /path/to/image.png
```

```bash
./fliv --list-formats
```

```bash
./fliv --help
```

```bash
./fliv --version
```

## Behavior

- Loads images via imlib2 (including multiframe/animated files when supported by loader).
- Opens a resizable FLTK window.
- Draws the image centered in a frame.
- Uses a texture-backed OpenGL canvas for image drawing.
- Scales down to fit the window if needed.
- Transparency is composited onto a checkerboard background.
- Animated/multiframe images auto-play using per-frame delays from imlib2 frame info.
- Zoom controls:
- `+` or `=` or `E`: zoom in
- `-` or `Q`: zoom out
- `0`: reset zoom
- `f`: fit image to window
- `F11`: toggle fullscreen
- `F4`: quit
- `Menu` key or `Shift+F10`: open context menu
- Mouse wheel: zoom in/out
- Hold `+` / `-` / `Q` / `E` for smooth continuous zoom
- `W` / `A` / `S` / `D`: pan up / left / down / right
- WASD combinations pan diagonally.
- Left-click drag: pan image by dragging
- Pan is edge-restricted (no movement past image bounds).
- Zoom rendering is viewport-based (only visible pixels are sampled/rendered).
- `p` / `n` or `Left` / `Right`: previous/next decodable file in the current directory.
- Unsupported/unreadable files are skipped automatically.
- `r`: reload current image from disk.
- `o` or `Ctrl+O`: open file picker.
- `c` or `Ctrl+C`: copy current image file to clipboard (`wl-copy` on Wayland, `xclip` on X11).
- `g`: open current image in GIMP (if installed).
- `i`: open current image in Inkscape (if installed).
- `Esc`: exit fullscreen first (if active), otherwise quit.
- Program can start with no input image: empty state shows an open hint and accepts open shortcut/menu.
- Right-click menu: Copy, Reload, Previous/Next File, Open File, Zoom In/Out/Reset, Fit to Window, Toggle Fullscreen, Open with GIMP, Open with Inkscape, Quit.
- GIMP/Inkscape menu entries are disabled if the app is unavailable at startup.
- Bottom status bar shows: filename, mime type, human-readable file size, and dimensions.
- MIME type is detected from file content using `libmagic`.

## Versioning

- `fliv` uses Semantic Versioning (`MAJOR.MINOR.PATCH`).
- The canonical version is stored in `VERSION`.
- Git release tags use `vX.Y.Z`.

## Config file

- Optional default path: `${XDG_CONFIG_HOME:-$HOME/.config}/fliv/config.ini`
- Optional override path: `--config /path/to/config.ini`
- Config file is not auto-created. Create it manually if you want overrides.
- Example config is shipped at `/usr/share/doc/fliv/config.ini.example`.

Supported keys (`[ui]` section):

- `frame_bg = #RRGGBB`
- `status_bg = #RRGGBB`
- `status_fg = #RRGGBB`
- `font = <font name>` (applies to status bar and right-click menu)
- `font_size = <6..96>` (applies to status bar and right-click menu)

## Format support

The runtime loader directory is auto-detected from standard imlib2 loader paths.

`./fliv --list-formats` can report these image formats:

- `ani`, `bmp`, `gif`, `heif`, `ico`, `j2k`, `jpeg`, `jxl`, `lbm`, `png`, `pnm`, `ps`, `qoi`, `svg`, `tga`, `tiff`, `webp`, `xbm`, `xpm`

And these auxiliary loaders:

- `argb`, `bz2`, `ff`, `id3`, `lzma`, `zlib`

Rendering pipeline:

- Decoding is done by imlib2.
- Pixel input from imlib2 is `DATA32` ARGB.
- `fliv` converts to RGB and composites alpha onto a checkerboard.
- Image drawing uses an OpenGL texture (`Fl_Gl_Window`).
- Zoom filter mode is scale-dependent:
- `>= 100%`: nearest-neighbor
- `< 100%`: linear filtering
- Multiframe formats are played with imlib2 frame timing and disposal/blend compositing.
