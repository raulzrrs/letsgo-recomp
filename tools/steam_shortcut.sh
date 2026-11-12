#!/bin/sh
# A Steam entry that starts the game, not the start screen (docs/COMPILAR-LINUX.md, "Steam").
#
#   tools/steam_shortcut.sh            writes the .desktop file and says the two clicks left in Steam
#   tools/steam_shortcut.sh --remove   removes it
#
# The game folder is the one the start screen set up in the data root (extracted/), so `--play` alone is enough: the
# entry does not need a path that goes stale. LARECOMP_NAME and LARECOMP_BIN override the name and the program.
set -eu
cd "$(dirname "$0")/.."

name=${LARECOMP_NAME:-"Let's Go Recomp"}
bin=${LARECOMP_BIN:-$(pwd)/build/lasrecomp}
desktop=${XDG_DATA_HOME:-$HOME/.local/share}/applications/letsgo-recomp.desktop
icon=$(pwd)/docs/icon/icon_1024.png

if [ "${1:-}" = "--remove" ]; then
    rm -f "$desktop"
    echo "removed $desktop (the Steam shortcut itself is removed in Steam)"
    exit 0
fi

[ -x "$bin" ] || { echo "no $bin: make first" >&2; exit 1; }
[ -f build/lasgame.so ] || { echo "no build/lasgame.so: make first" >&2; exit 1; }

mkdir -p "$(dirname "$desktop")"
cat > "$desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$name
Comment=Pokemon: Let's Go (static recompilation)
Exec="$bin" --play
Icon=$icon
Terminal=false
Categories=Game;
EOF
echo "wrote $desktop"

case "${XDG_CURRENT_DESKTOP:-}" in
*KDE*) how="right-click the entry and 'Add to Steam', or";;
*) how="";;
esac

cat <<EOF

In Steam: ${how:+$how
}Games > "Add a Non-Steam Game..." > "Browse..." > $bin
(the name and icon can be changed with right-click > Properties)

Launch options are not needed: --play is in the entry. To reach the start screen from Steam, remove it there.
EOF
