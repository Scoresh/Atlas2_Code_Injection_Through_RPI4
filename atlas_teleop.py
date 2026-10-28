#!/usr/bin/env python3
"""
ATLAS 2.0 - Raspberry Pi teleop

Reads an Xbox controller (wired USB or Bluetooth) and drives the Arduino
over USB serial.

Control scheme (matches the Arduino firmware's M/T protocol):
    Left stick  (Y axis) -> drive straight   -> "M <v>"
    Right stick (X axis)  -> in-place turn    -> "T <v>"
    Both centered         -> stop             -> "S"
    B button              -> panic stop

The Arduino smooths these targets with its own non-blocking ramp,
so this script just streams the latest intent at a fixed rate.

Resilience:
    A supervisor continuously checks that BOTH the controller and the
    Arduino serial link are present. If either drops (USB unplug, a
    Bluetooth controller going out of range / powering off, the Arduino
    resetting), the robot is stopped and the script waits and
    auto-reconnects to whatever reappears -- without exiting.

Dependencies:
    pip3 install evdev pyserial
"""

import sys
import time
import glob
import serial
from evdev import InputDevice, ecodes, list_devices

# --- Configuration ---------------------------------------------------------

SERIAL_PORT_CANDIDATES = ["/dev/ttyACM0", "/dev/ttyACM1",
                          "/dev/ttyUSB0", "/dev/ttyUSB1"]
BAUD_RATE = 115200

DEADZONE = 0.15          # fraction of stick travel ignored around center
SEND_HZ = 30             # how often to push a command to the Arduino
SEND_INTERVAL = 1.0 / SEND_HZ
CHANGE_THRESHOLD = 0.02  # min change in value before resending same mode

RECONNECT_INTERVAL = 1.0  # seconds between connection retry attempts

# evdev axis codes (standard Xbox layout)
AXIS_LEFT_Y = ecodes.ABS_Y    # left stick vertical  -> straight drive
AXIS_RIGHT_X = ecodes.ABS_RX  # right stick horizontal -> point turn
BTN_PANIC = ecodes.BTN_EAST   # "B" button -> panic stop

# Substrings used to recognize an Xbox-style gamepad by name (lower-cased).
CONTROLLER_NAME_HINTS = ("xbox", "x-box", "microsoft", "gamepad",
                         "controller", "x360", "wireless")


# --- Controller discovery (works for both USB and Bluetooth) ----------------

def find_controller():
    """
    Return the input device that best looks like an Xbox gamepad.

    A Bluetooth controller enumerates the same way as a wired one (a
    /dev/input/event* node with EV_ABS axes) but may also expose extra
    nodes (motion / consumer-control) that carry no sticks. We therefore
    require real stick axes (ABS_X + ABS_Y) and prefer a device whose name
    looks like a gamepad. Paths are never cached by the caller, because a
    reconnecting BT controller can land on a different event number.
    """
    best = None
    for path in list_devices():
        try:
            dev = InputDevice(path)
        except (OSError, PermissionError):
            continue
        caps = dev.capabilities()
        abs_codes = {code for code, _ in caps.get(ecodes.EV_ABS, [])}
        # Must have the two main stick axes to be a usable gamepad.
        if ecodes.ABS_X not in abs_codes or ecodes.ABS_Y not in abs_codes:
            continue
        name = (dev.name or "").lower()
        looks_like_pad = any(h in name for h in CONTROLLER_NAME_HINTS)
        if looks_like_pad:
            print(f"Using controller: {dev.name} ({dev.path})")
            return dev
        # Fall back to the first axis-bearing device if nothing is named.
        if best is None:
            best = dev
    if best is not None:
        print(f"Using controller: {best.name} ({best.path})")
    return best


def axis_ranges(dev):
    """Read each axis's min/max so we can normalize to -1..1."""
    ranges = {}
    for code, absinfo in dev.capabilities().get(ecodes.EV_ABS, []):
        ranges[code] = (absinfo.min, absinfo.max)
    return ranges


# --- Serial discovery --------------------------------------------------------

def open_serial():
    """Open the first available Arduino serial port (re-scans every call)."""
    candidates = SERIAL_PORT_CANDIDATES + sorted(glob.glob("/dev/ttyACM*")) \
                                        + sorted(glob.glob("/dev/ttyUSB*"))
    seen = set()
    for port in candidates:
        if port in seen:
            continue
        seen.add(port)
        try:
            ser = serial.Serial(port, BAUD_RATE, timeout=0.1)
            time.sleep(2.0)  # allow the Arduino to reset after port opens
            print(f"Connected to Arduino on {port}")
            return ser
        except (serial.SerialException, OSError):
            continue
    return None


def serial_alive(ser):
    """Best-effort check that the serial port is still physically present."""
    if ser is None:
        return False
    try:
        # A reset_output_buffer touches the underlying fd; it raises if the
        # device has been unplugged. Cheap and side-effect-free for the robot.
        ser.reset_output_buffer()
        return True
    except (serial.SerialException, OSError):
        return False


def controller_alive(dev):
    """Best-effort check that the controller device still exists."""
    if dev is None:
        return False
    try:
        # Accessing the fd path / capabilities raises OSError if gone.
        return dev.path in list_devices()
    except OSError:
        return False


# --- Supervisor: continuously ensure BOTH connections exist ------------------

def wait_for_connections(dev, ser):
    """
    Block until BOTH a controller and a serial link are available,
    (re)acquiring whichever one is missing. Returns (dev, ser).

    Safe to call at startup (dev/ser None) and after any disconnect.
    """
    announced = False
    while True:
        if not controller_alive(dev):
            if dev is not None:
                try:
                    dev.ungrab()
                except Exception:
                    pass
                dev = None
            dev = find_controller()

        if not serial_alive(ser):
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
            ser = open_serial()

        if dev is not None and ser is not None:
            return dev, ser

        if not announced:
            missing = []
            if dev is None:
                missing.append("controller")
            if ser is None:
                missing.append("Arduino serial")
            print(f"Waiting for: {', '.join(missing)} ...")
            announced = True
        time.sleep(RECONNECT_INTERVAL)


# --- Helpers -----------------------------------------------------------------

def normalize(raw, lo, hi):
    """Map a raw axis value in [lo, hi] to [-1.0, 1.0]."""
    if hi == lo:
        return 0.0
    val = (raw - lo) / (hi - lo)   # 0..1
    val = val * 2.0 - 1.0          # -1..1
    if val > 1.0:
        val = 1.0
    if val < -1.0:
        val = -1.0
    return val


def apply_deadzone(val):
    """Zero out small values and rescale so motion starts at the edge."""
    if abs(val) < DEADZONE:
        return 0.0
    # rescale [DEADZONE, 1] -> [0, 1], preserving sign
    sign = 1.0 if val > 0 else -1.0
    return sign * (abs(val) - DEADZONE) / (1.0 - DEADZONE)


# --- Safety ------------------------------------------------------------------

def emergency_stop(ser, attempts=3):
    """
    Force the robot to stop. Sends the stop command several times so a
    single dropped byte can't leave the robot moving. Returns True if at
    least one write succeeded.

    This is the guaranteed safety action whenever the controller is lost:
    we never want the robot to keep moving without an operator.
    """
    if ser is None:
        print("EMERGENCY STOP: no serial link - cannot reach robot!")
        return False
    ok = False
    for _ in range(attempts):
        try:
            ser.write(b"S\n")
            ser.flush()  # push it out of the OS buffer immediately
            ok = True
        except (serial.SerialException, OSError):
            break
        time.sleep(0.02)
    if ok:
        print("EMERGENCY STOP sent (robot commanded to halt).")
    else:
        print("EMERGENCY STOP: serial write failed - robot unreachable!")
    return ok


# --- Teleop session (runs while both devices are connected) ------------------

def run_teleop(dev, ser):
    """
    Drive the robot from controller input until either device drops.
    Returns normally on disconnect so the supervisor can reconnect.
    """
    ranges = axis_ranges(dev)
    ly_lo, ly_hi = ranges.get(AXIS_LEFT_Y, (-32768, 32767))
    rx_lo, rx_hi = ranges.get(AXIS_RIGHT_X, (-32768, 32767))

    left_y_raw = (ly_lo + ly_hi) / 2.0   # start centered
    right_x_raw = (rx_lo + rx_hi) / 2.0

    last_sent_cmd = None
    last_sent_val = 0.0
    last_send_time = 0.0

    def send(cmd):
        ser.write((cmd + "\n").encode())

    print("Teleop running. Ctrl-C to quit.")
    dev.grab()  # exclusive access so the OS desktop doesn't also react
    try:
        for event in dev.read_loop():
            now = time.time()

            if event.type == ecodes.EV_ABS:
                if event.code == AXIS_LEFT_Y:
                    left_y_raw = event.value
                elif event.code == AXIS_RIGHT_X:
                    right_x_raw = event.value

            elif event.type == ecodes.EV_KEY:
                if event.code == BTN_PANIC and event.value == 1:
                    send("S")
                    last_sent_cmd, last_sent_val = "S", 0.0
                    print("PANIC STOP")
                    continue

            if now - last_send_time < SEND_INTERVAL:
                continue
            last_send_time = now

            # Y axis: up on the stick is negative, so invert for forward = +
            drive = apply_deadzone(-normalize(left_y_raw, ly_lo, ly_hi))
            turn = apply_deadzone(normalize(right_x_raw, rx_lo, rx_hi))

            # Single command per cycle; point turn takes priority.
            if turn != 0.0:
                cmd, val = "T", turn
            elif drive != 0.0:
                cmd, val = "M", drive
            else:
                cmd, val = "S", 0.0

            changed = (cmd != last_sent_cmd or
                       abs(val - last_sent_val) > CHANGE_THRESHOLD)
            if not changed:
                continue

            if cmd == "S":
                send("S")
            else:
                send(f"{cmd} {val:.2f}")
            last_sent_cmd, last_sent_val = cmd, val

    except OSError:
        # Controller unplugged / out of Bluetooth range.
        # SAFETY: stop the robot immediately before anything else.
        print("\nController disconnected -> stopping robot.")
        emergency_stop(ser)
    except (serial.SerialException,):
        # Arduino serial link lost. The robot can't be reached over this
        # link anymore, but attempt a stop in case it briefly recovers.
        print("\nArduino serial link lost.")
        emergency_stop(ser)
    finally:
        try:
            dev.ungrab()
        except Exception:
            pass


# --- Main: supervise connections forever -------------------------------------

def main():
    dev = None
    ser = None
    try:
        while True:
            dev, ser = wait_for_connections(dev, ser)
            run_teleop(dev, ser)
            # run_teleop returned -> a device dropped. The robot was already
            # commanded to stop inside run_teleop; re-assert it here as a
            # backup in case the serial link is still (or again) alive.
            emergency_stop(ser)
            print("Reconnecting ...")
    except KeyboardInterrupt:
        print("\nShutting down.")
    finally:
        # Final safety stop + cleanup.
        emergency_stop(ser)
        try:
            if ser is not None:
                time.sleep(0.05)
                ser.close()
        except Exception:
            pass
        try:
            if dev is not None:
                dev.ungrab()
        except Exception:
            pass
        print("Stopped and disconnected.")


if __name__ == "__main__":
    main()
