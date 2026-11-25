#!/bin/sh
# Builds build/LetsGoRecomp-<version>-x86_64.AppImage: the runtime, the game code library and the libraries it links
# (SDL3, FFmpeg, shaderc), so the game runs without the source tree and without installing those.
#
#   tools/make_appimage.sh [--keep]   --keep keeps build/appimage/AppDir for a look
#
# The Vulkan loader and the driver stay the system's (as with the Windows build's vulkan-1.dll), and so does the
# shared font (the game's font engine uses one of the system's TrueType faces). Saves, settings, mods and the game files
# go to $XDG_DATA_HOME/lasrecomp: the AppImage's own folder is read-only (runtime/paths.c).
#
# Portability is the libc of the machine that builds it: an AppImage made on Fedora 45 needs Fedora 45's glibc or newer.
set -eu
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define RT_APP_VERSION "\(.*\)".*/\1/p' runtime/paths.h)
OUT=build/LetsGoRecomp-$VERSION-x86_64.AppImage
APPDIR=build/appimage/AppDir
TOOL=build/appimage/appimagetool

[ -x build/lasrecomp ] && [ -f build/lasgame.so ] || { echo "make first" >&2; exit 1; }

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/icons/hicolor/512x512/apps"
cp build/lasrecomp build/lasgame.so build/lasgame.so.id "$APPDIR/usr/bin/"
cp docs/icon/icon_1024.png "$APPDIR/letsgo-recomp.png"
cp docs/icon/icon_1024.png "$APPDIR/usr/share/icons/hicolor/512x512/apps/letsgo-recomp.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$APPDIR/usr/bin/"  # (the start screen's font)
cp third_party/fonts/Baloo2/OFL.txt "$APPDIR/usr/bin/Baloo2-OFL.txt"

# The libraries behind the binary, each under the soname the loader asks for (found through LD_LIBRARY_PATH in
# AppRun). `ldd` gives the whole closure, which is what a self-contained AppImage needs; the blacklist is what a Linux
# system provides itself -- glibc and its loader above all, the Vulkan loader and the driver, the graphical stack the
# desktop owns, and FFmpeg's smb:// client together with samba (the game reads local files only).
ldd "$APPDIR/usr/bin/lasrecomp" | awk '/=> \// {print $1, $3}' > build/appimage/libs.txt
while read -r soname path; do
    case "$soname" in
    libc.so.6 | libm.so.6 | libmvec.so.1 | libdl.so.2 | libpthread.so.0 | librt.so.1 | libresolv.so.2 | ld-linux*) ;;
    libvulkan* | libdrm* | libGL* | libEGL* | libGLX* | libgbm* | libvdpau* | libva* ) ;;
    libX11* | libxcb* | libXau* | libXdmcp* | libXext* | libXfixes* | libXrender* | libwayland*) ;;
    libsystemd* | libudev* | libdbus* | libselinux* | libsepol* | libcap* | libmount* | libblkid*) ;;
    libglib* | libgio* | libgobject* | libgmodule* | libgthread* | libgdk* | libgtk* | libpango* ) ;;
    libcairo* | libpixman* | libharfbuzz* | libfontconfig* | libfreetype* | libpng* | libjpeg* | libtiff*) ;;
    libldap* | liblber* | libsasl2* | libkrb5* | libgssapi* | libsamba* | libsmbclient* | libndr* ) ;;
    libtalloc* | libtdb* | libtevent* | libwbclient* | libldb* | *-private-samba* | libgensec* ) ;;
    libk5crypto* | libkrb5support* | libasn1util* | libhcrypto* | libhx509* | libroken* | libwind* ) ;;
    libcom_err* | libkeyutils* | libgpg-error* | libgcrypt* ) ;;
    *) cp -L "$path" "$APPDIR/usr/lib/$soname" ;;
    esac
done < build/appimage/libs.txt
ls "$APPDIR/usr/lib"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/lasrecomp" "$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/letsgo-recomp.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Let's Go Recomp
Comment=Pokemon: Let's Go (static recompilation)
Exec=lasrecomp --play
Icon=letsgo-recomp
Terminal=false
Categories=Game;
EOF

if [ ! -x "$TOOL" ]; then
    echo "downloading appimagetool"
    curl -sSfL -o "$TOOL" \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOL"
fi

ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" --no-appstream "$APPDIR" "$OUT"
[ "${1:-}" = "--keep" ] || rm -rf build/appimage/AppDir
echo "$OUT"
