#!/bin/bash
set -e
set -x

SERVICE_NAME="input_dispi"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
WORKDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_PATH="${WORKDIR}/bin/${SERVICE_NAME}"
CURRENT_USER="$(whoami)"
CURRENT_GROUP="$(id -gn)"
LOCK_FILE="/tmp/input_dispi.lock"

# 権限チェック
if [ "$CURRENT_USER" = "root" ]; then
    echo "OK: rootユーザーで実行中。"
elif sudo -l -U "$CURRENT_USER" >/dev/null 2>&1; then
    echo "OK: $CURRENT_USER はsudo権限を持っています。"
else
    echo "エラー: $CURRENT_USER はrootまたはsudo権限がありません。終了します。" >&2
    exit 1
fi

# sudoラッパー関数
sudo_if_needed() {
    if [ "$CURRENT_USER" != "root" ]; then
        sudo "$@"
    else
        "$@"
    fi
}

# バイナリ存在チェック
if [ ! -f "$BIN_PATH" ]; then
    echo "実行ファイルが存在しません: $BIN_PATH"
    exit 1
fi

# コメント付きユニットファイル作成
sudo_if_needed tee "$SERVICE_FILE" > /dev/null <<EOF
# ===========================================
# Input Display Service - systemd Unit File
# -------------------------------------------
# このサービスは、Raspberry PiのDRM/KMS環境上で、
# raylibベースの入力表示プログラム(input_dispi)を動作させるための設定です。
#
# 設計思想・運用方針
# - 本体が異常終了しても即時再起動し、無人運用を実現する
# - 短時間に異常再起動が多発した場合は無限ループを防ぐ
# - getty@tty1を停止して、直接TTY出力を行う
# - 最小限のリカバリタイム、最大の安定稼働を目指す
# ===========================================

[Unit]
Description=Input Display via raylib DRM/KMS
After=multi-user.target
Conflicts=getty@tty1.service

# -------------------------------------------
# [Service] セクション
# -------------------------------------------
[Service]
Type=simple
ExecStart=${BIN_PATH}
WorkingDirectory=${WORKDIR}

# 常時再起動設定
Restart=always
RestartSec=3
StartLimitIntervalSec=60
StartLimitBurst=10

# 実行ユーザー設定
User=${CURRENT_USER}
Group=${CURRENT_GROUP}

# TTYデバイス直接利用設定
TTYPath=/dev/tty1
StandardInput=tty
StandardOutput=tty

# サービス終了時の動作
RemainAfterExit=yes
TimeoutStopSec=5
KillMode=control-group

# -------------------------------------------
# [Install] セクション
# -------------------------------------------
[Install]
WantedBy=multi-user.target
EOF

# getty停止
sudo_if_needed systemctl disable getty@tty1
sudo_if_needed systemctl stop getty@tty1

SERVICE="${SERVICE_NAME}.service"

# サービス停止
echo "Stopping $SERVICE..."
sudo_if_needed systemctl stop "$SERVICE"

echo "Waiting for service to fully stop..."

timeout=5     # 最大待機秒数
interval=0.2  # チェック間隔秒

loops=$(awk "BEGIN { printf \"%d\", $timeout / $interval }")

for ((i=0; i<loops; i++)); do
    if ! systemctl is-active --quiet "$SERVICE"; then
        echo "Service has fully stopped."
        break
    fi
    sleep "$interval"
done

# ロックファイル削除
if [ -e "$LOCK_FILE" ]; then
    echo "Removing stale lock file: $LOCK_FILE"
    sudo_if_needed rm -f "$LOCK_FILE"
fi

# systemd再読込とサービス起動
echo "Reloading systemd daemon..."
sudo_if_needed systemctl daemon-reexec
sudo_if_needed systemctl daemon-reload

echo "Starting $SERVICE..."
sudo_if_needed systemctl enable "$SERVICE"
sudo_if_needed systemctl start "$SERVICE"

echo "Checking status of $SERVICE..."
sudo_if_needed systemctl status "$SERVICE" --no-pager

echo "${SERVICE_NAME}.service を有効化・起動しました。"
