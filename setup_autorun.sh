#!/usr/bin/env bash
#
# setup_autorun.sh
# ----------------------------------------------------------------------------
# Configures ATLAS 2.0 teleop to auto-run on a Raspberry Pi 4 via systemd.
#
# NOTE: This is a Linux shell script (.sh), NOT a Windows .bat file.
#       The Raspberry Pi runs Linux, so batch scripts do not apply.
#
# What it does (safe to re-run; idempotent):
#   1. Installs python3-pip and the evdev + pyserial libraries
#   2. Adds the run user to the 'input' and 'dialout' groups
#      (so the script can read the controller and the Arduino serial port
#       without root)
#   3. Writes a systemd service that waits for devices and auto-restarts
#   4. Enables + starts the service so it launches on every boot
#
# Usage (on the Pi):
#   chmod +x setup_autorun.sh
#   sudo ./setup_autorun.sh
# ----------------------------------------------------------------------------

set -euo pipefail

# --- Configuration (edit if your setup differs) -----------------------------

# The Linux user the service runs as. On older images this is 'pi';
# newer Raspberry Pi OS images may use a custom name. Auto-detect a sensible
# default, but you can override by exporting RUN_USER before running.
RUN_USER="${RUN_USER:-${SUDO_USER:-pi}}"

# Where atlas_teleop.py lives. Defaults to the directory this script is in.
SCRIPT_DIR="${SCRIPT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
TELEOP_PATH="${SCRIPT_DIR}/atlas_teleop.py"

SERVICE_NAME="atlas-teleop"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

# --- Pre-flight checks ------------------------------------------------------

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: please run with sudo:  sudo ./setup_autorun.sh" >&2
  exit 1
fi

if [[ ! -f "${TELEOP_PATH}" ]]; then
  echo "ERROR: cannot find teleop script at: ${TELEOP_PATH}" >&2
  echo "       Put atlas_teleop.py next to this script, or set SCRIPT_DIR." >&2
  exit 1
fi

if ! id "${RUN_USER}" &>/dev/null; then
  echo "ERROR: user '${RUN_USER}' does not exist." >&2
  echo "       Set the correct user:  sudo RUN_USER=yourname ./setup_autorun.sh" >&2
  exit 1
fi

echo "==> Configuring ATLAS teleop auto-run"
echo "    User          : ${RUN_USER}"
echo "    Script        : ${TELEOP_PATH}"
echo "    Service file  : ${SERVICE_FILE}"
echo

# --- Step 1: dependencies ---------------------------------------------------

echo "==> [1/4] Installing dependencies (pip, evdev, pyserial)..."
apt-get update -y
apt-get install -y python3-pip
# Install libs for the run user so they're importable when the service runs
# as that user. --break-system-packages is needed on newer (PEP 668) images;
# it is ignored/benign on older pip versions.
sudo -u "${RUN_USER}" pip3 install --user evdev pyserial \
  || sudo -u "${RUN_USER}" pip3 install --user --break-system-packages evdev pyserial

# --- Step 2: device permissions ---------------------------------------------

echo "==> [2/4] Adding '${RUN_USER}' to 'input' and 'dialout' groups..."
usermod -aG input "${RUN_USER}"
usermod -aG dialout "${RUN_USER}"

# --- Step 3: systemd service ------------------------------------------------

echo "==> [3/4] Writing systemd service to ${SERVICE_FILE}..."
cat > "${SERVICE_FILE}" <<EOF
[Unit]
Description=ATLAS 2.0 Xbox teleop
# Wait until the system and Bluetooth stack are up before starting.
After=multi-user.target bluetooth.target

[Service]
Type=simple
User=${RUN_USER}
WorkingDirectory=${SCRIPT_DIR}
ExecStart=/usr/bin/python3 ${TELEOP_PATH}

# Keep it alive: if the script ever exits or crashes, systemd relaunches it.
Restart=always
RestartSec=3

# Route the script's output to the system journal for debugging.
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

# --- Step 4: enable + start -------------------------------------------------

echo "==> [4/4] Enabling and starting the service..."
systemctl daemon-reload
systemctl enable "${SERVICE_NAME}"
systemctl restart "${SERVICE_NAME}"

echo
echo "==> Done. The teleop service is installed and running."
echo
echo "    It will sit idle ('Waiting for: ...') until BOTH the Arduino and"
echo "    the Xbox controller are connected, then start driving automatically."
echo "    It relaunches on boot and after any crash."
echo
echo "  Useful commands:"
echo "    systemctl status ${SERVICE_NAME}        # is it running?"
echo "    journalctl -u ${SERVICE_NAME} -f        # live log output"
echo "    sudo systemctl restart ${SERVICE_NAME}  # after editing the script"
echo "    sudo systemctl stop ${SERVICE_NAME}     # stop it"
echo "    sudo systemctl disable ${SERVICE_NAME}  # stop auto-start on boot"
echo
echo "  IMPORTANT:"
echo "    * Group changes (input/dialout) fully apply after a reboot."
echo "      Reboot now if this is the first setup:  sudo reboot"
echo "    * A Bluetooth controller must be PAIRED + TRUSTED once via"
echo "      'bluetoothctl' before it will auto-reconnect on boot."

