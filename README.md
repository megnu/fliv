# fliv (stage 1)

A minimal FLTK 1.4 + imlib2 image viewer baseline.

## Build

```bash
make
```

## Run

```bash
./fliv /path/to/image.png
```

```bash
./fliv --list-formats
```

## Current behavior

- Loads one image using imlib2.
- Opens a resizable FLTK window.
- Draws the image centered with padding and a simple frame.
- Scales down to fit the window if needed.
- Transparency is composited onto a checkerboard background.
- Zoom controls:
- `Ctrl` + `+` or `Ctrl` + `=`: zoom in
- `Ctrl` + `-`: zoom out
- `Ctrl` + `0`: reset zoom
- Mouse wheel: zoom in/out
- `W` / `A` / `S` / `D`: pan up / left / down / right
- Left-click drag: pan image by dragging
- Pan is edge-restricted (no movement past image bounds).
- Zoom rendering is viewport-based (only visible pixels are sampled/rendered).
- `Left` / `Right`: previous/next decodable file in the current directory.
- Unsupported/unreadable files are skipped automatically.
- `c` or `Ctrl+C`: copy current image file to clipboard (`wl-copy` on Wayland, `xclip` on X11).

## Format support

The current runtime loader directory is `/usr/lib/imlib2/loaders`.

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
- Animated/container formats are currently displayed as a single still frame in stage 1.

## Reference docs used

- `reference/imlib2-docs.html`
- `/usr/include/Imlib2.h`
