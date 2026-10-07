#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if ! pkg-config --exists Qt6Widgets Qt6Svg Qt6Network x11; then
  echo "需要 Qt 6 Widgets、Qt 6 Svg、Qt 6 Network、X11 的开发文件和 pkg-config。" >&2
  exit 1
fi
extra_flags=()
if pkg-config --exists KF6WindowSystem; then
  extra_flags=(-DNETFLOAT_KDE_BLUR)
  read -r -a kde_flags <<< "$(pkg-config --cflags --libs KF6WindowSystem)"
  extra_flags+=("${kde_flags[@]}")
fi
c++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -fPIC netfloat.cpp -o netfloat \
  $(pkg-config --cflags --libs Qt6Widgets Qt6Svg Qt6Network x11) "${extra_flags[@]}"
echo "构建完成：$(pwd)/netfloat"
