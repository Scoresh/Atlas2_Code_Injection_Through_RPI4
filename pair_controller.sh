#!/usr/bin/env bash
#
# pair_controller.sh
# ----------------------------------------------------------------------------
# Interactive helper to pair + trust + connect an Xbox controller over
# Bluetooth on a Raspberry Pi, so atlas_teleop.py can auto-reconnect to it
# on every boot.
#
# NOTE: Linux shell script (.sh) for the Pi, not a Windows .bat file.
#
# The 'trust' step is the important one for auto-run: it lets the Pi
# reconnect to the controller automatically after reboots without
# re-pairing.
#
# Usage (on the Pi):
#   chmod +x pair_controller.sh
#   ./pair_controller.sh            # guided: scans and lets you pick
#   ./pair_controller.sh AA:BB:CC:DD:EE:FF   # if you already know the MAC
#
# Before running: put the controller in PAIRING MODE --
#   hold the Xbox (Guide) button, then hold the small SYNC button on the
#   top edge until the Xbox light flashes rapidly.
# ----------------------------------------------------------------------------

set -uo pipefail

SCAN_SECONDS=12

# --- Checks -----------------------------------------------------------------

if ! command -v bluetoothctl &>/dev/null; then
  echo "ERROR: bluetoothctl not found. Install it with:" >&2
  echo "       sudo apt install -y bluez" >&2
  exit 1
fi

# Make sure the Bluetooth controller (adapter) is powered on.
echo "==> Powering on the Bluetooth adapter..."
bluetoothctl power on >/dev/null 2>&1 || true
bluetoothctl agent on >/dev/null 2>&1 || true
bluetoothctl default-agent >/dev/null 2>&1 || true

# --- Resolve the target MAC -------------------------------------------------

MAC="${1:-}"

if [[ -z "${MAC}" ]]; then
  echo
  echo "==> Put the controller in PAIRING MODE now:"
  echo "    Hold the Xbox button, then hold the SYNC button (top edge)"
  echo "    until the light flashes RAPIDLY."
  echo
  read -r -p "Press ENTER when the controller light is flashing rapidly... " _

  echo "==> Scanning for ${SCAN_SECONDS}s..."
  # Run a timed scan in the background via bluetoothctl.
  bluetoothctl --timeout "${SCAN_SECONDS}" scan on >/dev/null 2>&1 || true

  echo
  echo "==> Discovered devices:"
  # List devices; try to highlight likely Xbox controllers.
  mapfile -t DEVICES < <(bluetoothctl devices)
  if [[ "${#DEVICES[@]}" -eq 0 ]]; then
    echo "    (none found)"
    echo
    echo "ERROR: No devices discovered. Make sure the controller light is" >&2
    echo "       flashing rapidly (pairing mode) and try again." >&2
    exit 1
  fi

  idx=0
  declare -a MACS=()
  for line in "${DEVICES[@]}"; do
    # Lines look like: Device AA:BB:CC:DD:EE:FF Xbox Wireless Controller
    dev_mac="$(awk '{print $2}' <<<"${line}")"
    dev_name="$(cut -d' ' -f3- <<<"${line}")"
    MACS+=("${dev_mac}")
    marker=""
    if grep -qiE 'xbox|x-box|controller|microsoft|wireless' <<<"${dev_name}"; then
      marker="  <-- likely controller"
    fi
    printf "   [%d] %s  %s%s\n" "${idx}" "${dev_mac}" "${dev_name}" "${marker}"
    idx=$((idx + 1))
  done

  echo
  read -r -p "Enter the number of your controller: " choice
  if ! [[ "${choice}" =~ ^[0-9]+$ ]] || (( choice < 0 || choice >= ${#MACS[@]} )); then
    echo "ERROR: invalid selection." >&2
    exit 1
  fi
  MAC="${MACS[${choice}]}"
fi

echo
echo "==> Target controller: ${MAC}"

# --- Pair, trust, connect ---------------------------------------------------

run_bt() {
  # Feed a single command to bluetoothctl and return its output.
  bluetoothctl "$@" 2>&1
}

echo "==> Pairing..."
run_bt pair "${MAC}" || true

echo "==> Trusting (enables auto-reconnect on boot)..."
run_bt trust "${MAC}" || true

echo "==> Connecting..."
run_bt connect "${MAC}" || true

# --- Verify -----------------------------------------------------------------

echo
echo "==> Verifying..."
INFO="$(bluetoothctl info "${MAC}" 2>&1)"

paired="$(grep -i 'Paired:'    <<<"${INFO}" | awk '{print $2}')"
trusted="$(grep -i 'Trusted:'  <<<"${INFO}" | awk '{print $2}')"
connected="$(grep -i 'Connected:' <<<"${INFO}" | awk '{print $2}')"

echo "    Paired:    ${paired:-unknown}"
echo "    Trusted:   ${trusted:-unknown}"
echo "    Connected: ${connected:-unknown}"
echo

if [[ "${paired}" == "yes" && "${trusted}" == "yes" ]]; then
  echo "SUCCESS: controller paired and trusted."
  echo "It will now auto-reconnect to this Pi on future boots, and"
  echo "atlas_teleop.py will pick it up automatically."
  if [[ "${connected}" != "yes" ]]; then
    echo
    echo "NOTE: not currently 'Connected', but that's fine -- it will"
    echo "      connect when powered on. To force it now:"
    echo "        bluetoothctl connect ${MAC}"
  fi
  exit 0
else
  echo "WARNING: pairing/trust did not fully succeed." >&2
  echo "Try again with the controller in pairing mode (light flashing fast)." >&2
  echo "If it keeps failing, remove any stale entry and retry:" >&2
  echo "    bluetoothctl remove ${MAC}" >&2
  exit 1
fi
