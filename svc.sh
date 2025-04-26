#!/bin/bash
set -e
set -x

SERVICE_NAME="input_dispi"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
BIN_PATH="/usr/local/project/bin/input_dispi"
WORKDIR="/usr/local/project"

# 確認
if [ ! -f "$BIN_PATH" ]; then
    echo "実行ファイルが存在しません: $BIN_PATH"
    exit 1
fi

# ユニットファイルを作成
sudo tee "$SERVICE_FILE" > /dev/null <<EOF
[Unit]
Description=Input Display via raylib DRM/KMS
After=multi-user.target
Conflicts=getty@tty1.service

[Service]
Type=simple
ExecStart=${BIN_PATH}
WorkingDirectory=${WORKDIR}
Restart=always
RestartSec=10
User=pi
Group=pi
TTYPath=/dev/tty1
StandardInput=tty
StandardOutput=tty
RemainAfterExit=yes
TimeoutStopSec=5
KillMode=mixed

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl disable getty@tty1
sudo systemctl stop getty@tty1

# systemdに反映して起動＆自動起動有効化
SERVICE="${SERVICE_NAME}.service"

echo "Stopping $SERVICE..."
sudo systemctl stop "$SERVICE"

echo "Waiting for service to fully stop..."
sleep 5  # 必要に応じて短く

echo "Reloading systemd daemon..."
sudo systemctl daemon-reexec
sudo systemctl daemon-reload

echo "Starting $SERVICE..."
sudo systemctl enable "$SERVICE"
sudo systemctl start "$SERVICE"

echo "Checking status of $SERVICE..."
sudo systemctl status "$SERVICE" --no-pager

echo " ${SERVICE_NAME}.service を有効化・起動しました。"
