#!/bin/sh
# Installs the Edax icons and file types for the current user.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
mkdir -p "$DATA/icons" "$DATA/mime/packages"
cp -r "$DIR/icons/hicolor" "$DATA/icons/"
cp "$DIR/mime/packages/edax.xml" "$DATA/mime/packages/"
update-mime-database "$DATA/mime"
gtk-update-icon-cache -f -t "$DATA/icons/hicolor" 2>/dev/null || true
echo "Edax file types installed."
