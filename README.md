# fliv

A simple FLTK 1.4 + imlib2 image viewer.

## Build

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

## Current behavior

- Loads images via imlib2 (including multiframe/animated files when supported by loader).
- Opens a resizable FLTK window.
- Draws the image centered with padding and a simple frame.
- Scales down to fit the window if needed.
- Transparency is composited onto a checkerboard background.
- Animated/multiframe images auto-play using per-frame delays from imlib2 frame info.
- Zoom controls:
- `+` or `=` or `Ctrl` + `+` or `Ctrl` + `=`: zoom in
- `-`: zoom out
- `0`: reset zoom
- `f`: fit image to window
- `F11`: toggle fullscreen
- `Menu` key or `Shift+F10`: open context menu
- Mouse wheel: zoom in/out
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
- Program can start with no input image; empty state shows an open hint and accepts open shortcut/menu.
- Right-click menu: Copy, Reload, Previous/Next File, Open File, Zoom In/Out/Reset, Fit to Window, Toggle Fullscreen, Open with GIMP, Open with Inkscape.
- GIMP/Inkscape menu entries are disabled if the app is unavailable at startup.
- Bottom status bar shows: filename, mime type, human-readable file size, and dimensions.
- MIME type is detected from file content using `libmagic` (package: `file`).

## Config file

- Optional default path: `${XDG_CONFIG_HOME:-$HOME/.config}/fliv/config.ini`
- Optional override path: `--config /path/to/config.ini`
- Config file is not auto-created; create it manually if you want overrides.
- Example config is shipped at `/usr/share/doc/fliv/config.ini.example`.

Supported keys (`[ui]` section):

- `frame_bg = #RRGGBB`
- `status_bg = #RRGGBB`
- `status_fg = #RRGGBB`
- `font = <font name>` (applies to status bar and right-click menu)
- `font_size = <6..96>` (applies to status bar and right-click menu)

## Format support

The runtime loader directory is auto-detected from standard imlib2 loader paths.

`./fliv --list-formats` reports these image formats:

- `ani`, `bmp`, `gif`, `heif`, `ico`, `j2k`, `jpeg`, `jxl`, `lbm`, `png`, `pnm`, `ps`, `qoi`, `svg`, `tga`, `tiff`, `webp`, `xbm`, `xpm`

`./fliv --list-formats` reports these auxiliary loaders:

- `argb`, `bz2`, `ff`, `id3`, `lzma`, `zlib`

Format-to-dependency mapping on Arch:

- `gif` -> `giflib`
- `jpeg` -> `libjpeg-turbo`
- `png` -> `libpng`
- `tiff` -> `libtiff`
- `webp` -> `libwebp`
- `heif` -> `libheif`
- `jxl` -> `libjxl` (with `highway` as build dependency)
- `j2k` -> `openjpeg2`
- `svg` -> `librsvg`
- `ps` -> `libspectre`
- `id3` loader -> `libid3tag`
- `bz2` loader -> `bzip2`
- `lzma` loader -> `xz`

Core imlib2 runtime deps used by these loaders include:

- `freetype2`, `libxext`

Rendering pipeline facts for this app:

- Decoding is done by imlib2.
- Pixel input from imlib2 is `DATA32` ARGB.
- `fliv` converts to RGB and composites alpha onto a checkerboard.
- Multiframe formats are played with imlib2 frame timing and disposal/blend compositing.

## Reference docs used

- `reference/imlib2-docs.html`
- `/usr/include/Imlib2.h`
