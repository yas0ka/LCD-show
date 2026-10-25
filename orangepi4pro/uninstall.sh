#!/bin/bash
# sudo bash uninstall.sh
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
	echo "sudo bash uninstall.sh で実行してください。"
	exit 1
fi

ENV_FILE=/boot/orangepiEnv.txt
OVERLAY=spi3-cs0-cs1-spidev

systemctl disable --now spi-lcd-desktop.service 2>/dev/null || true
rm -f /etc/systemd/system/spi-lcd-desktop.service
systemctl daemon-reload
rm -f /usr/local/bin/spi-lcd-desktop
rm -f /etc/udev/rules.d/99-spidev-dialout.rules
udevadm control --reload-rules || true

if [[ -f "${ENV_FILE}" ]]; then
	python3 - "${ENV_FILE}" "${OVERLAY}" <<'PY'
import sys
from pathlib import Path
path, name = sys.argv[1], sys.argv[2]
file = Path(path)
lines = file.read_text(encoding="utf-8").splitlines()
out = []
for line in lines:
    if line.startswith("overlays="):
        parts = [part for part in line.split("=", 1)[1].split() if part != name]
        if parts:
            out.append("overlays=" + " ".join(parts))
    else:
        out.append(line)
file.write_text("\n".join(out) + "\n", encoding="utf-8")
PY
fi

echo "外しました。SPI を止めるには再起動してください。"
echo "ローカル画面のサイズを戻すときは、ログイン中に次を実行します。"
echo "DISPLAY=:0 xrandr --fb 1024x768"
