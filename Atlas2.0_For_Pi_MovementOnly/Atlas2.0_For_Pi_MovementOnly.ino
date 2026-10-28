#include <Servo.h>

// --- Pin Definitions ---
#define LEFT_PIN 7
#define RIGHT_PIN 8

// --- Objects & State ---
Servo leftMotor;
Servo rightMotor;

float currentSpeed = 0; // Current ramped velocity, -1.0 to 1.0
float targetSpeed  = 0; // Commanded target velocity, -1.0 to 1.0
float currentAccel = 0; // Current acceleration (velocity change per step)

// Drive intent for the acceleration ramp
#define MODE_LINEAR 1
#define MODE_TURN   2
int currentMode = MODE_LINEAR;

// --- Jerk-limited (S-curve) velocity ramp ---
// This is velocity control: targetSpeed is the commanded velocity and the
// motor tracks toward it. To avoid jerking the motors we do NOT change the
// velocity by a fixed step (that would snap acceleration from 0 to full =
// infinite jerk). Instead we ramp ACCELERATION itself and limit how fast
// acceleration may change. That rate-of-change-of-acceleration limit is the
// jerk/jolt limit, giving a smooth S-curve that eases in and eases out.
const float MAX_ACCEL  = 0.010;  // max |velocity change| per step (accel cap)
const float JERK_STEP  = 0.0010; // max change in accel per step (jolt limit)
const float SPEED_EPS  = 0.004;  // "close enough" to target velocity

// --- Proportional reverse braking (active brake) ---
// When a stop (target 0) is commanded while moving, we don't just coast to
// neutral: we actively drive the motor the OTHER way, proportional to how
// fast it's still going. That reverse current brakes the robot. Because the
// reverse command is scaled to the (shrinking) velocity estimate, it eases
// off on its own as the robot slows, so there's no violent reverse slam.
// We also ramp the brake output through the same jerk limit for smoothness.
const float BRAKE_GAIN   = 0.60; // reverse output as a fraction of current velocity
const float MAX_BRAKE    = 0.60; // hard cap on reverse braking output magnitude
const float BRAKE_RELEASE = 0.03; // velocity below which braking ends -> neutral

// Non-blocking ramp timing
const unsigned long ACCEL_INTERVAL = 20; // ms between steps
unsigned long lastRampTime = 0;

bool settled = true; // true once velocity has reached target; ramp idles
bool braking = false; // true while actively reverse-braking to a stop
float brakeOutput = 0; // current reverse output sent to motors while braking

void setup() {
  Serial.begin(115200);
  currentSpeed = 0;
  targetSpeed  = 0;
  currentAccel = 0;
  currentMode  = MODE_LINEAR;
  settled      = true;
  braking      = false;
  brakeOutput  = 0;

  // Attach SparkMax as Servos
  leftMotor.attach(LEFT_PIN);
  rightMotor.attach(RIGHT_PIN);

  // Initialize Motors to Neutral (Stop)
  stopMotors();
}

void loop() {
  // Always service serial first so new commands interrupt the ramp immediately
  if (Serial.available() > 0) {
    String data = Serial.readStringUntil('\n');
    parseCommand(data);
  }

  // Advance the acceleration ramp without blocking
  updateRamp();
}

void parseCommand(String cmd) {
  if (cmd.startsWith("M")) {
    Serial.println("Moving Straight");
    currentMode = MODE_LINEAR;
    targetSpeed = cmd.substring(2).toFloat();
  }
  else if (cmd.startsWith("T")) {
    Serial.println("Turning");
    currentMode = MODE_TURN;
    targetSpeed = cmd.substring(2).toFloat();
  }
  else {
    Serial.println("stopping");
    targetSpeed = 0; // ramp velocity estimate down to a stop
  }

  // Clamp incoming target
  if (targetSpeed > 1.0) targetSpeed = 1.0;
  if (targetSpeed < -1.0) targetSpeed = -1.0;

  // Decide whether this command should engage active reverse braking.
  // We brake only when told to stop (target ~0) AND we're actually moving.
  if (fabs(targetSpeed) < SPEED_EPS && fabs(currentSpeed) > BRAKE_RELEASE) {
    braking = true;
  } else {
    // Any real drive command cancels braking and resumes normal velocity control.
    braking = false;
    brakeOutput = 0;
  }

  // A new command may change the target, so wake the ramp back up.
  settled = false;
}

// --- Non-blocking jerk-limited (S-curve) velocity ramp ---
void updateRamp() {
  // Once we've reached the target velocity, the ramp idles. We don't keep
  // re-writing the motors every loop; we only wake up when a new command
  // clears the settled flag. This fixes the "never stops ramping" behavior.
  if (settled) return;

  // Only step once per ACCEL_INTERVAL (fixed-rate integration).
  if (millis() - lastRampTime < ACCEL_INTERVAL) return;
  lastRampTime = millis();

  // --- Active reverse-braking path ---
  if (braking) {
    updateBrake();
    return;
  }

  float error = targetSpeed - currentSpeed;

  // Are we basically there? Snap, zero the accel, and settle.
  if (fabs(error) <= SPEED_EPS && fabs(currentAccel) <= JERK_STEP) {
    currentSpeed = targetSpeed;
    currentAccel = 0;
    settled = true;
    applyDrive();
    return;
  }

  // Desired direction of travel toward the target velocity.
  float dir = (error > 0) ? 1.0 : -1.0;

  // Stopping distance: how much velocity we'd cover if we started easing
  // acceleration back to zero right now, decreasing it by JERK_STEP each
  // step. This is the classic (v^2)/(2a) style term adapted to discrete
  // jerk steps, and it tells us WHEN to start decelerating so we arrive at
  // the target without overshooting.
  float stoppingDist = (currentAccel * currentAccel) / (2.0 * JERK_STEP);

  if (fabs(error) <= stoppingDist) {
    // Close enough that we must start easing acceleration toward zero
    // (eases OUT of the move -> smooth arrival, no jerk at the end).
    if (currentAccel > 0)      currentAccel -= JERK_STEP;
    else if (currentAccel < 0) currentAccel += JERK_STEP;
  } else {
    // Still far from target: ramp acceleration toward the capped value in
    // the travel direction (eases IN -> smooth launch, no jerk at the start).
    float desiredAccel = dir * MAX_ACCEL;
    if (currentAccel < desiredAccel) {
      currentAccel += JERK_STEP;
      if (currentAccel > desiredAccel) currentAccel = desiredAccel;
    } else if (currentAccel > desiredAccel) {
      currentAccel -= JERK_STEP;
      if (currentAccel < desiredAccel) currentAccel = desiredAccel;
    }
  }

  // Clamp acceleration to its cap.
  if (currentAccel > MAX_ACCEL)  currentAccel = MAX_ACCEL;
  if (currentAccel < -MAX_ACCEL) currentAccel = -MAX_ACCEL;

  // Integrate acceleration into velocity.
  currentSpeed += currentAccel;

  // Don't sail past the target: if this step would overshoot, clamp to it.
  if ((dir > 0 && currentSpeed > targetSpeed) ||
      (dir < 0 && currentSpeed < targetSpeed)) {
    currentSpeed = targetSpeed;
    currentAccel = 0;
    settled = true;
  }

  // Clamp velocity to the valid range.
  if (currentSpeed > 1.0) currentSpeed = 1.0;
  if (currentSpeed < -1.0) currentSpeed = -1.0;

  applyDrive();
}

// --- Active reverse braking (proportional, jerk-limited) ---
// We keep a velocity ESTIMATE in currentSpeed and ramp it toward 0. The motor
// output, however, is driven in the OPPOSITE direction of travel, scaled by
// BRAKE_GAIN, to actively brake. The reverse output is itself slew/jerk
// limited so the reverse current eases in rather than slamming the motors.
void updateBrake() {
  // The direction the robot is still moving. Reverse output opposes this.
  float movingDir = (currentSpeed > 0) ? 1.0 : -1.0;

  // Once slow enough, stop braking and settle at true neutral.
  if (fabs(currentSpeed) <= BRAKE_RELEASE) {
    currentSpeed = 0;
    currentAccel = 0;
    brakeOutput  = 0;
    braking = false;
    settled = true;
    applyDrive(); // writes neutral (mapSpeed snaps near-zero to 90)
    return;
  }

  // Target reverse output: opposite the motion, proportional to remaining
  // speed, capped so it can never exceed MAX_BRAKE.
  float desiredBrake = -movingDir * fabs(currentSpeed) * BRAKE_GAIN;
  if (desiredBrake >  MAX_BRAKE) desiredBrake =  MAX_BRAKE;
  if (desiredBrake < -MAX_BRAKE) desiredBrake = -MAX_BRAKE;

  // Slew the actual reverse output toward the target by at most MAX_ACCEL
  // per step. This keeps the onset of reverse current smooth (jerk-limited).
  if (brakeOutput < desiredBrake) {
    brakeOutput += MAX_ACCEL;
    if (brakeOutput > desiredBrake) brakeOutput = desiredBrake;
  } else if (brakeOutput > desiredBrake) {
    brakeOutput -= MAX_ACCEL;
    if (brakeOutput < desiredBrake) brakeOutput = desiredBrake;
  }

  // Bleed the velocity estimate down. Because we have no encoder, we model
  // the brake decelerating the robot: the stronger the brake output, the
  // faster the estimate decays toward 0.
  currentSpeed -= movingDir * fabs(brakeOutput);
  // Don't let the estimate cross through zero (that would flip movingDir).
  if ((movingDir > 0 && currentSpeed < 0) ||
      (movingDir < 0 && currentSpeed > 0)) {
    currentSpeed = 0;
  }

  // Send the reverse braking output to the motors.
  applyBrakeOutput(brakeOutput);
}

// Write the current speed to the motors according to the active mode
void applyDrive() {
  if (currentMode == MODE_LINEAR) executeLinear(currentSpeed);
  else if (currentMode == MODE_TURN) executePointTurn(currentSpeed);
}

// Write a braking output to the motors using the active mode's geometry.
// This reuses the same drive functions so the reverse current is applied in
// the correct direction for both linear driving and point turns.
void applyBrakeOutput(float out) {
  if (currentMode == MODE_LINEAR) executeLinear(out);
  else if (currentMode == MODE_TURN) executePointTurn(out);
}

// --- Motor Logic ---

// Converts -1.0 to 1.0 into 0 to 180 for SparkMax
int mapSpeed(float val) {
  // Guarantee a true dead-stop at neutral: any velocity at/near zero maps
  // exactly to 90 (neutral) so the motors fully stop when input is 0.
  if (fabs(val) < SPEED_EPS) return 90;
  return map(val * 100, -100, 100, 45,135); // changed because of motor opp. installation
}

void executeLinear(float val) {
  int servoVal = mapSpeed(val);
  leftMotor.write((180 - servoVal));
  rightMotor.write(servoVal);
}

void executePointTurn(float val) {
//motors are installed in opposite directions: if both motors at the same value they spin opposite
  int leftVal = mapSpeed(val);
  int rightVal = mapSpeed(val);
  leftMotor.write((180 - (leftVal))); 
  rightMotor.write((180 - (rightVal)));
}

void stopMotors() {
  currentSpeed = 0;
  targetSpeed = 0;
  currentAccel = 0;
  braking = false;
  brakeOutput = 0;
  settled = true;
  leftMotor.write(90);
  rightMotor.write(90);
}
