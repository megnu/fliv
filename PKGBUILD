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
source=("VERSION" "main.cpp" "Makefile" "fliv.desktop" "fliv-icon.png" "config.ini.example")
md5sums=('30a04cf33ee91a3ecf4b75c71268f316'
         'ec2d37c425da947c50b06edd4349ff89'
         '7a4fb32c6e28e87efc384a7a7451d8f2'
         '9ac1d7bd6901a9c6163c2b1b48866246'
         '68691c0ddcb0e12420741f0b1e622c52'
         '6ebd6ad29894bac36890ed89ae0910f7')

build() {
    make
}

package() {
    install -Dm755 fliv "$pkgdir/usr/bin/fliv"
    install -Dm644 fliv-icon.png "$pkgdir/usr/share/pixmaps/fliv.png"
    install -Dm644 fliv.desktop "$pkgdir/usr/share/applications/fliv.desktop"
    install -Dm644 config.ini.example "$pkgdir/usr/share/doc/fliv/config.ini.example"
}
