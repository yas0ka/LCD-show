#!/bin/bash
# Orange Pi 4 Pro の公式 Ubuntu に、MPI3501 用の液晶転送を入れる。
# sudo bash install.sh
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
	echo "sudo bash install.sh で実行してください。"
	exit 1
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="${SUDO_USER:-orangepi}"
if ! id "${USER_NAME}" >/dev/null 2>&1; then
	echo "ユーザー ${USER_NAME} が見つかりません。"
	exit 1
fi
HOME_DIR="$(getent passwd "${USER_NAME}" | cut -d: -f6)"
ENV_FILE=/boot/orangepiEnv.txt
OVERLAY=spi3-cs0-cs1-spidev

if [[ ! -f "${ENV_FILE}" ]]; then
	echo "${ENV_FILE} がありません。Orange Pi の公式イメージで実行してください。"
	exit 1
fi
if [[ ! -x /usr/bin/gpio ]]; then
	echo "/usr/bin/gpio がありません。wiringOP 入りの公式 Ubuntu で実行してください。"
	exit 1
fi

if ! command -v gcc >/dev/null 2>&1 || ! pkg-config --exists x11 xtst; then
	apt-get update
	apt-get install -y gcc pkg-config libx11-dev libxtst-dev
fi

install -d /usr/local/bin
gcc -O2 -Wall -Wextra -o /usr/local/bin/spi-lcd-desktop "${ROOT}/desktop_35.c" -lX11 -lXtst -lpthread
chmod 755 /usr/local/bin/spi-lcd-desktop

install -m 644 "${ROOT}/99-spidev-dialout.rules" /etc/udev/rules.d/99-spidev-dialout.rules
udevadm control --reload-rules || true
usermod -aG dialout "${USER_NAME}"

if [[ ! -f "${ENV_FILE}.bak-spi-lcd" ]]; then
	cp -a "${ENV_FILE}" "${ENV_FILE}.bak-spi-lcd"
fi
python3 - "${ENV_FILE}" "${OVERLAY}" <<'PY'
import sys
from pathlib import Path
path, name = sys.argv[1], sys.argv[2]
lines = Path(path).read_text(encoding="utf-8").splitlines()
found = False
out = []
for line in lines:
    if line.startswith("overlays="):
        found = True
        parts = line.split("=", 1)[1].split()
        if name not in parts:
            parts.append(name)
        out.append("overlays=" + " ".join(parts))
    else:
        out.append(line)
if not found:
    out.append("overlays=" + name)
Path(path).write_text("\n".join(out) + "\n", encoding="utf-8")
PY

sed -e "s/__USER__/${USER_NAME}/g" -e "s|__HOME__|${HOME_DIR}|g" \
	"${ROOT}/spi-lcd-desktop.service" > /etc/systemd/system/spi-lcd-desktop.service
systemctl daemon-reload
systemctl enable spi-lcd-desktop.service

echo "インストールしました。"
if [[ -e /dev/spidev3.0 ]]; then
	systemctl restart spi-lcd-desktop.service
	echo "SPI は既に有効です。液晶を 40 ピンの 1 番に合わせて挿すと、ローカルのデスクトップが出ます。"
else
	echo "再起動してください。そのあと液晶を挿すと、ログイン画面のデスクトップが液晶に出ます。"
	echo "リモートデスクトップは別画面のままです。"
fi
