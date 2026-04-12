pkgname=fliv
pkgver=$(<VERSION)
pkgrel=1
pkgdesc="Simple FLTK image viewer powered by imlib2"
arch=('x86_64')
url="https://github.com/megnu/fliv"
license=('GPL-2.0-or-later')
depends=('fltk' 'imlib2' 'file')
optdepends=(
    'libheif: HEIF/AVIF loader support'
    'libid3tag: ID3 loader support'
    'libjxl: JPEG XL (JXL) loader support'
    'librsvg: SVG loader support'
    'libspectre: PostScript (PS) loader support'
    'libwebp: WEBP loader support'
    'openjpeg2: JPEG 2000 (J2K) loader support'
    'wl-clipboard: copy image to clipboard on Wayland (wl-copy)'
    'xclip: copy image to clipboard on X11'
    'gimp: open current image with g/Ctrl+G'
    'inkscape: open current image with i/Ctrl+I'
)
makedepends=('gcc')
source=("VERSION" "main.cpp" "Makefile" "fliv.desktop" "icon-fliv.png" "config.ini.example")
sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')

build() {
    make
}

package() {
    install -Dm755 fliv "$pkgdir/usr/bin/fliv"
    install -Dm644 icon-fliv.png "$pkgdir/usr/share/pixmaps/fliv.png"
    install -Dm644 fliv.desktop "$pkgdir/usr/share/applications/fliv.desktop"
    install -Dm644 config.ini.example "$pkgdir/usr/share/doc/fliv/config.ini.example"
}
