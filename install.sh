#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
./build.sh
mkdir -p "$HOME/.local/bin" "$HOME/.local/share/applications" "$HOME/.local/share/icons/hicolor/scalable/apps"
install -m 755 netfloat "$HOME/.local/bin/netfloat"
install -m 644 netfloat.svg "$HOME/.local/share/icons/hicolor/scalable/apps/netfloat.svg"
install -m 644 netfloat.svg "$HOME/.local/bin/netfloat.svg"
cat > "$HOME/.local/share/applications/netfloat.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=NetFloat 网络悬浮窗
Comment=实时显示网络、GPU 和系统状态
Exec=$HOME/.local/bin/netfloat
Icon=$HOME/.local/share/icons/hicolor/scalable/apps/netfloat.svg
Terminal=false
Categories=Utility;
DESKTOP
if command -v kbuildsycoca6 >/dev/null 2>&1; then
  kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi
echo "已安装。可以从应用菜单启动 NetFloat。"
