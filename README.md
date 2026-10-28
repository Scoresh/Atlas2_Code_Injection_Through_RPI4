# ATLAS 2.0 — Raspberry Pi Xbox Teleop

Drive the ATLAS 2.0 robot from an Xbox controller connected to a Raspberry Pi 4.
The Pi reads the controller and streams movement commands over USB serial to an
Arduino, which runs the motors.

```
 Xbox controller ──(USB or Bluetooth)──▶ Raspberry Pi 4 ──(USB serial)──▶ Arduino ──▶ drive motors
```

This build is **movement only** — all mining, drum, actuator, and load-cell
logic has been removed. The robot drives straight and performs in-place point
turns.

---

## Files

| File | Runs on | Purpose |
|------|---------|---------|
| `atlas_teleop.py` | Raspberry Pi | Reads the Xbox controller and streams `M`/`T`/`S` commands to the Arduino. Auto-waits for both devices and auto-reconnects. |
| `Atlas2.0_For_Pi_MovementOnly.ino` | Arduino | Movement-only firmware. Open directly in the Arduino IDE. |
| `setup_autorun.sh` | Raspberry Pi | Installs dependencies and configures the teleop to auto-run on boot via systemd. |
| `pair_controller.sh` | Raspberry Pi | One-time helper to pair + trust an Xbox controller over Bluetooth. |
| `Atlas2.0_Archive.txt` | Arduino | Original full firmware (mining + driving). Kept for reference; not used by this setup. |

---

## How it works

The Arduino firmware is a "dumb" motor executor that understands three serial
commands at **115200 baud**:

| Command | Meaning |
|---------|---------|
| `M <v>` | Drive straight at speed `v` (`-1.0` … `1.0`) |
| `T <v>` | Point turn (spin in place) at speed `v` |
| anything else (e.g. `S`) | Stop |

The firmware smooths these targets with its own **non-blocking, jerk-limited
(S-curve) velocity ramp**, so the Pi just streams the latest intent (~30 Hz)
and the Arduino handles smooth acceleration/deceleration.

### Motion smoothing & braking (firmware)

The movement firmware does velocity control with two motor-protecting
behaviors:

- **Jerk-limited S-curve ramp.** Rather than changing speed by a fixed step
  (which snaps acceleration from 0 to full = infinite jerk), the firmware
  ramps *acceleration itself* and limits how fast acceleration can change.
  Acceleration eases in at the start of a move and eases out at the end, so the
  motors never get a sudden torque spike. Once the commanded velocity is
  reached the ramp settles and stops re-writing the motors until a new command
  arrives.
- **Active (proportional) reverse braking.** When a stop is commanded while
  moving, the firmware briefly drives the motors in reverse, scaled to how fast
  the robot is still going, to actively brake instead of coasting. The reverse
  output is jerk-limited (eases on, not a slam) and shrinks as the robot slows,
  releasing to true neutral once stopped.

These are tunable at the top of the firmware:

| Constant | Meaning |
|----------|---------|
| `MAX_ACCEL` | Acceleration cap (max speed change per step) |
| `JERK_STEP` | Jolt limit (max change in acceleration per step) — lower = softer |
| `BRAKE_GAIN` | How aggressive the reverse braking is |
| `MAX_BRAKE` | Hard ceiling on reverse braking output |
| `BRAKE_RELEASE` | Speed below which braking releases to neutral |

> **Note:** there is no encoder, so the firmware brakes against an internal
> *velocity estimate*, not a measured wheel speed. Tune the braking constants
> on hardware — start low and with the wheels off the ground.

### Control scheme

| Input | Action |
|-------|--------|
| Left stick (Y axis) | Drive straight (forward / reverse) → `M` |
| Right stick (X axis) | In-place point turn → `T` |
| Both sticks centered | Stop → `S` |
| **B** button | Panic stop |

Only one command is active per cycle; the point turn takes priority when the
right stick is moved.

### Resilience / safety

- **Limbo state:** on power-up the Pi sits idle (`Waiting for: ...`) until
  **both** the Arduino and the controller are connected, then starts driving
  automatically.
- **Auto-reconnect:** if either device drops (USB unplug, Bluetooth out of
  range, Arduino reset), the robot is stopped and the Pi returns to the limbo
  state until both devices return — no restart needed.
- **Controller-loss safety stop:** if the controller disconnects, the Pi
  immediately commands the robot to stop (sent multiple times for reliability).
  The robot will not keep moving without an operator.

---

## Setup

### 1. Flash the Arduino

1. Open `Atlas2.0_For_Pi_MovementOnly.ino` in the Arduino IDE.
2. Install the **Servo** library (bundled with the IDE).
3. Select your board/port and upload.

Pin usage (movement only):

| Pin | Connection |
|-----|------------|
| 7 | Left drive motor (SparkMax, as servo) |
| 8 | Right drive motor (SparkMax, as servo) |

### 2. Prepare the Raspberry Pi

Copy `atlas_teleop.py`, `setup_autorun.sh`, and `pair_controller.sh` into the
same folder on the Pi (e.g. `/home/pi/atlas/`).

Connect the Arduino to the Pi with the USB cable.

### 3. (Bluetooth controllers only) Pair the controller

Skip this if you use a wired USB controller.

```bash
chmod +x pair_controller.sh
./pair_controller.sh
```

Put the controller in pairing mode first — hold the **Xbox** button, then hold
the small **Sync** button on the top edge until the light flashes rapidly. The
script scans, lets you pick your controller, and pairs + **trusts** it. The
trust step is what lets it auto-reconnect on future boots.

If you already know the controller's MAC address:

```bash
./pair_controller.sh AA:BB:CC:DD:EE:FF
```


### 4. Install auto-run on boot

```bash
chmod +x setup_autorun.sh
sudo ./setup_autorun.sh
sudo reboot        # first time only — applies the device-group permissions
```

This installs `evdev` + `pyserial`, grants the user access to the input and
serial devices, and creates a systemd service that launches the teleop on every
boot and restarts it if it ever crashes.

If your Pi user is not `pi`, the script auto-detects the user that ran `sudo`.
To force a specific user:

```bash
sudo RUN_USER=yourname ./setup_autorun.sh
```

---

## Running manually (for testing)

Before relying on auto-run, test by hand:

```bash
python3 atlas_teleop.py
```

You should see `Waiting for: ...`, then `Teleop running.` once both the Arduino
and controller are connected. Press **Ctrl-C** to quit (the robot is stopped on
exit).

---

## Managing the service

```bash
systemctl status atlas-teleop         # is it running?
journalctl -u atlas-teleop -f         # live log output
sudo systemctl restart atlas-teleop   # restart after editing the script
sudo systemctl stop atlas-teleop      # stop it
sudo systemctl disable atlas-teleop   # stop auto-starting on boot
```

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| Stuck on `Waiting for: controller` | Controller not connected/paired. For Bluetooth, re-run `pair_controller.sh`. For wired, check the USB cable. |
| Stuck on `Waiting for: Arduino serial` | Arduino USB cable unplugged, or a permissions issue — confirm the user is in the `dialout` group (`groups`), then reboot. |
| A joystick does nothing | The axis code may differ on your controller. Run `evtest` on the Pi to find the real code, then edit `AXIS_LEFT_Y` / `AXIS_RIGHT_X` in `atlas_teleop.py`. Some pads report the right stick as `ABS_Z`/`ABS_RZ`. |
| Forward drives backward, or turn is mirrored | Flip the sign on the relevant value in `atlas_teleop.py` (`drive` / `turn`), or adjust `mapSpeed` in the firmware. |
| `pip3 install` fails on newer Pi OS | The setup script retries with `--break-system-packages` automatically (PEP 668). |
| Controller won't re-pair | `bluetoothctl remove <MAC>`, then re-run `pair_controller.sh` in pairing mode. |

---

## Notes

- All status output from the running service goes to the system journal
  (`journalctl -u atlas-teleop`).
- The Pi commands the robot to stop over serial on controller loss. This relies
  on the serial link being alive; it is not a firmware-level watchdog. If you
  later want the Arduino itself to auto-stop when it stops hearing from the Pi,
  that can be added as a firmware heartbeat.
- `atlas_teleop.py` was syntax-checked with `py_compile`; runtime behavior
  (controller axes, serial, Bluetooth) must be verified on the actual Pi
  hardware.
