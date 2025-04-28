#!/bin/bash
set -e
set -x

echo "=== raylib のビルドとインストールを開始します ==="

# 依存パッケージのインストール（X11とWayland両対応を想定）
sudo apt update
sudo apt install -y \
    git cmake build-essential \
    libgl1-mesa-dev libx11-dev libxcursor-dev libxrandr-dev libxinerama-dev libxi-dev \
    libwayland-dev libdrm-dev libgbm-dev libudev-dev \
    procps

# raylib をクローン
if [ ! -d "raylib" ]; then
    git clone https://github.com/raysan5/raylib.git
else
    echo "raylib ディレクトリが既に存在します。スキップします。"
fi

# ビルド用ディレクトリの作成
cd raylib
pwd
mkdir -p build && cd build
pwd
cmake ..
sudo make uninstall || true  # 一度アンインストール（失敗してもOK）

make clean

# CMake設定とビルド
cmake -DCMAKE_INSTALL_PREFIX=/usr/local \
     -DBUILD_SHARED_LIBS=ON \
     -DPLATFORM=Desktop \
     -DUSE_AUDIO=OFF \
     -DCMAKE_SHARED_LINKER_FLAGS="-latomic" \
     -DCMAKE_EXE_LINKER_FLAGS="-latomic" \
     -DBUILD_EXAMPLES=OFF \
     -DPLATFORM=DRM ..
make -j$(nproc)

# インストール
sudo make install

sudo ldconfig

echo "=== raylib のインストールが完了しました ==="
