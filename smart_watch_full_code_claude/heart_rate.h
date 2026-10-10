#ifndef HEART_RATE_H
#define HEART_RATE_H

// ============================================================
// HEART RATE (MAX30102) - header version of the bare test code
//
// Needs the library: "SparkFun MAX3010x Pulse and Proximity Sensor Library"
// (Arduino Library Manager). Wire.begin(SDA, SCL) must be called first.
//
// Algorithm = the bare code, unchanged in spirit:
//   IR sample -> EMA DC baseline (alpha 0.95) -> AC = IR - DC
//   -> beat = falling zero-crossing of AC -> BPM = 60000 / beat interval
//   -> intervals outside 300..2000 ms are rejected
// Differences (all non-blocking / robustness only):
//   * no delay(), no early return: poll heart_update() from loop()
//   * beat timestamps are back-dated per FIFO sample (loop latency can't skew BPM)
//   * zero-crossing is interpolated between two samples (sub-sample timing)
//   * a short settle time after the finger is detected
//   * result = trimmed mean of the last 6 beats, accepted when they agree
// SpO2: this file reads Red + IR from the FIFO and feeds SpO2.h (the SpO2
// algorithm lives there). Heart rate behaves exactly as before.
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"   // SparkFun library (works for MAX30102)
#include "SpO2.h"

// ---------------- Sensor configuration (same as the bare code) ----------------
#ifndef HEART_I2C_SPEED
#define HEART_I2C_SPEED        I2C_SPEED_FAST   // 400 kHz, as in the bare test
#endif
#define HEART_LED_BRIGHTNESS   60     // ~12 mA LED current
#define HEART_SAMPLE_AVERAGE   4      // average 4 samples
#define HEART_LED_MODE         2      // Red + IR
#define HEART_SAMPLE_RATE      100    // 100 samples/s before averaging
#define HEART_PULSE_WIDTH      411    // 18-bit ADC
#define HEART_ADC_RANGE        4096

// Effective output rate = 100 / 4 = 25 samples/s -> 40 ms per FIFO sample
#define HEART_SAMPLE_PERIOD_MS (1000.0f * HEART_SAMPLE_AVERAGE / HEART_SAMPLE_RATE)

// ---------------- Algorithm tuning ----------------
#define HEART_FINGER_THRESHOLD   50000   // IR below this = no finger (as in the bare code)
#define HEART_DC_ALPHA           0.95f   // EMA constant for the DC baseline
#define HEART_MIN_BEAT_MS        300     // reject beats faster than 200 BPM
#define HEART_MAX_BEAT_MS        2000    // reject beats slower than 30 BPM
#define HEART_SETTLE_SAMPLES     25      // ~1 s after finger detected before counting beats
#define HEART_WINDOW             6       // beats kept for the result
#define HEART_STABLE_SPREAD_BPM  8       // middle 4 of 6 beats must agree within this
#define HEART_MIN_LIVE_BEATS     3       // beats needed before a live value is shown

// ---------------- Internal state ----------------
static MAX30105 hrSensor;
static bool   hr_sensor_ok  = false;
static bool   hr_running    = false;
static bool   hr_finger     = false;

static float  hr_dcIR       = 0;
static float  hr_prevAC     = 0;
static bool   hr_pulseUp    = false;
static int    hr_settle     = 0;

static bool   hr_haveLast   = false;
static double hr_lastBeatMs = 0;

static float  hr_beats[HEART_WINDOW];
static int    hr_head       = 0;
static int    hr_count      = 0;

static int    hr_live       = 0;
static bool   hr_done       = false;
static int    hr_result     = 0;

// Forget the signal history (finger lost / new finger / new measurement)
static void hr_reset_signal() {
  hr_dcIR = 0;
  hr_prevAC = 0;
  hr_pulseUp = false;
  hr_settle = 0;
  hr_haveLast = false;
  hr_lastBeatMs = 0;
  hr_head = 0;
  hr_count = 0;
  hr_live = 0;
  spo2_reset_signal();
}

static void hr_reset_all() {
  hr_reset_signal();
  spo2_reset_all();
  hr_finger = false;
  hr_done = false;
  hr_result = 0;
}

// Sorted copy of the beats currently in the window
static int hr_sorted(float *s) {
  int n = hr_count;
  for (int i = 0; i < n; i++) s[i] = hr_beats[i];   // order in the ring doesn't matter
  for (int i = 1; i < n; i++) {                      // insertion sort
    float v = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
    s[j + 1] = v;
  }
  return n;
}

static void hr_push_beat(float bpm) {
  hr_beats[hr_head] = bpm;
  hr_head = (hr_head + 1) % HEART_WINDOW;
  if (hr_count < HEART_WINDOW) hr_count++;

  float s[HEART_WINDOW];
  int n = hr_sorted(s);

  // Live value = median of the beats so far (robust against one bad beat)
  if (n >= HEART_MIN_LIVE_BEATS) {
    float med = (n % 2) ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
    hr_live = (int)(med + 0.5f);
  }

  // Final result: full window, drop the lowest and highest beat, the rest must agree
  if (n == HEART_WINDOW) {
    float lo = s[1], hi = s[HEART_WINDOW - 2];
    if ((hi - lo) <= HEART_STABLE_SPREAD_BPM) {
      float sum = 0;
      for (int i = 1; i <= HEART_WINDOW - 2; i++) sum += s[i];
      hr_result = (int)(sum / (HEART_WINDOW - 2) + 0.5f);
      hr_live = hr_result;
      hr_done = true;
    }
  }
}

// One IR + Red sample at time tMs (millis scale)
static void hr_process_sample(uint32_t ir, uint32_t red, double tMs) {
  // 1. Finger detection
  if (ir < HEART_FINGER_THRESHOLD) {
    if (hr_finger) hr_reset_signal();   // also clears the SpO2 signal history
    hr_finger = false;
    return;
  }
  if (!hr_finger) {                 // finger just placed: start clean
    hr_reset_signal();
    hr_finger = true;
    hr_dcIR = ir;
    hr_settle = HEART_SETTLE_SAMPLES;
    spo2_begin(ir, red);
    return;
  }

  // 2. DC baseline (EMA) and AC component
  hr_dcIR = (HEART_DC_ALPHA * hr_dcIR) + ((1.0f - HEART_DC_ALPHA) * ir);
  float ac = ir - hr_dcIR;

  bool settled = (hr_settle == 0);
  spo2_sample(ir, red, settled);    // SpO2 tracks its own DC; AC min/max only after settling

  if (!settled) {                   // let the baseline settle
    hr_settle--;
    hr_prevAC = ac;
    return;
  }

  // 3. Beat = falling zero-crossing of AC
  if (ac > 0) {
    hr_pulseUp = true;
  } else if (ac < 0 && hr_pulseUp) {
    hr_pulseUp = false;

    // Interpolate the crossing time between the previous and this sample
    float denom = hr_prevAC - ac;
    float frac = (denom > 0.0f) ? (hr_prevAC / denom) : 1.0f;
    double tCross = (tMs - HEART_SAMPLE_PERIOD_MS) + (double)frac * HEART_SAMPLE_PERIOD_MS;

    if (!hr_haveLast) {
      hr_haveLast = true;
      hr_lastBeatMs = tCross;
      spo2_cycle_start();           // first cycle boundary
    } else {
      double dt = tCross - hr_lastBeatMs;
      if (dt > HEART_MIN_BEAT_MS && dt < HEART_MAX_BEAT_MS) {
        hr_lastBeatMs = tCross;
        if (!hr_done) hr_push_beat((float)(60000.0 / dt));   // HR result stays frozen once done
        spo2_beat();                                         // one SpO2 value per valid beat
      } else if (dt >= HEART_MAX_BEAT_MS) {
        hr_lastBeatMs = tCross;     // too long: restart timing from here
        spo2_cycle_start();
      }
      // dt <= MIN: noise, keep the previous beat as reference
    }
  }
  hr_prevAC = ac;
}

// ============================================================
// PUBLIC API
// ============================================================

// Call once in setup() after Wire.begin(). Leaves the sensor in shutdown.
inline bool init_HEART() {
  hr_sensor_ok = hrSensor.begin(Wire, HEART_I2C_SPEED);
  if (!hr_sensor_ok) {
    Serial.println("DEBUG: [HR] MAX30102 not found - check wiring/power");
    return false;
  }
  hrSensor.setup(HEART_LED_BRIGHTNESS, HEART_SAMPLE_AVERAGE, HEART_LED_MODE,
                 HEART_SAMPLE_RATE, HEART_PULSE_WIDTH, HEART_ADC_RANGE);
  hrSensor.shutDown();
  hr_running = false;
  return true;
}

// Wake the sensor and start a fresh measurement
inline void start_HEART() {
  if (!hr_sensor_ok) return;
  hr_reset_all();
  hrSensor.setup(HEART_LED_BRIGHTNESS, HEART_SAMPLE_AVERAGE, HEART_LED_MODE,
                 HEART_SAMPLE_RATE, HEART_PULSE_WIDTH, HEART_ADC_RANGE);  // soft reset + configure
  hrSensor.clearFIFO();
  hr_running = true;
}

// LEDs off, sensor in shutdown (safe to call at any time)
inline void stop_HEART() {
  if (hr_sensor_ok) hrSensor.shutDown();
  hr_running = false;
}

// Non-blocking: call every loop() pass while measuring
inline void heart_update() {
  if (!hr_running || (hr_done && spo2_done())) return;   // both results ready: nothing left to do

  hrSensor.check();                     // read the sensor FIFO
  int n = hrSensor.available();
  if (n <= 0) return;

  uint32_t now = millis();
  for (int k = 0; k < n; k++) {
    uint32_t ir  = hrSensor.getFIFOIR();
    uint32_t red = hrSensor.getFIFORed();
    hrSensor.nextSample();
    // Newest sample ~ now; older ones are one sample period earlier each
    double t = (double)now - (double)(n - 1 - k) * HEART_SAMPLE_PERIOD_MS;
    hr_process_sample(ir, red, t);
    if (hr_done && spo2_done()) break;
  }
}

inline bool heart_sensor_ok()       { return hr_sensor_ok; }
inline bool heart_finger_present()  { return hr_finger; }
inline int  heart_live_bpm()        { return hr_live; }     // 0 = not enough beats yet
inline bool heart_done()            { return hr_done; }
inline int  heart_result()          { return hr_result; }   // valid when heart_done()

// Legacy helper (old API): returns the live BPM, 0 if none
inline int readHEART() {
  heart_update();
  return hr_live;
}

#endif // HEART_RATE_H





//Original Example Code (TESTED and WORKING)
/*
#include <Wire.h>
#include "MAX30105.h" // SparkFun library (works for MAX30102)

MAX30105 particleSensor;

// Signal Processing Variables
const int bufferLength = 100;
uint32_t irBuffer[bufferLength];
uint32_t redBuffer[bufferLength];
int bufferIndex = 0;

// DC Filter Variables
float dcRed = 0;
float dcIR = 0;
const float dcAlpha = 0.95; // EMA filter constant for DC baseline

// Pulse Detection
uint32_t lastBeatTime = 0;
bool pulseDetected = false;
float irAC_max = 0, irAC_min = 0;
float redAC_max = 0, redAC_min = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin(11,12); // ESP32-S3 default I2C pins (SDA, SCL)

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 was not found. Please check wiring/power.");
    while (1);
  }

  // --- Sensor Configuration ---
  byte ledBrightness = 60; // ~12mA LED current
  byte sampleAverage = 4;  // Average 4 samples to reduce noise
  byte ledMode = 2;        // Red + IR mode for SpO2
  int sampleRate = 100;    // 100 samples per second
  int pulseWidth = 411;    // 411us for 18-bit ADC resolution
  int adcRange = 4096;     // ADC full scale range

  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
  Serial.println("MAX30102 Initialized. Place your finger on the sensor.");
}

void loop() {
  particleSensor.check(); // Check the sensor FIFO
  
  while (particleSensor.available()) {
    uint32_t irValue = particleSensor.getFIFOIR();
    uint32_t redValue = particleSensor.getFIFORed();
    particleSensor.nextSample();

    // 1. Finger Detection (Signal Quality Check)
    if (irValue < 50000) { 
      Serial.println("Signal Quality: POOR - Finger not detected");
      delay(500);
      return;
    }

    // 2. DC Baseline Tracking (Exponential Moving Average)
    if (dcIR == 0) {
      dcIR = irValue;
      dcRed = redValue;
    } else {
      dcIR = (dcAlpha * dcIR) + ((1.0 - dcAlpha) * irValue);
      dcRed = (dcAlpha * dcRed) + ((1.0 - dcAlpha) * redValue);
    }

    // 3. Extract AC Component
    float irAC = irValue - dcIR;
    float redAC = redValue - dcRed;

    // Track AC Min/Max for amplitude calculation
    if (irAC > irAC_max) irAC_max = irAC;
    if (irAC < irAC_min) irAC_min = irAC;
    if (redAC > redAC_max) redAC_max = redAC;
    if (redAC < redAC_min) redAC_min = redAC;

    // 4. Simple Pulse Detection (Zero-crossing of AC signal on falling edge)
    if (irAC > 0) {
      pulseDetected = true;
    } 
    else if (irAC < 0 && pulseDetected) {
      pulseDetected = false; // Pulse peak passed
      
      uint32_t currentTime = millis();
      uint32_t deltaT = currentTime - lastBeatTime;
      
      // Reject noise/invalid beats (BPM outside 30-200 range)
      if (deltaT > 300 && deltaT < 2000) {
        float bpm = 60000.0 / deltaT;
        lastBeatTime = currentTime;

        // 5. SpO2 Calculation
        float irAmplitude = irAC_max - irAC_min;
        float redAmplitude = redAC_max - redAC_min;

        // Ensure we have a valid amplitude to prevent division by zero or noise errors
        if (irAmplitude > 50 && redAmplitude > 50) {
          // Calculate R ratio: (AC_red / DC_red) / (AC_IR / DC_IR)
          float ratio = (redAmplitude / dcRed) / (irAmplitude / dcIR);
          
          // Apply user's empirical calibration formula
          // n_spo2_calc = 103.0 - (17.0 * n_ratio_average / 100.0) -> scaled for standard R
          float spo2 = 103.0 - (17.0 * ratio); 

          // Cap SpO2 to realistic bounds
          if (spo2 > 100.0) spo2 = 100.0;
          if (spo2 < 50.0) spo2 = 0.0; // Invalid reading

          if (spo2 > 0) {
            Serial.print("Signal Quality: GOOD | ");
            Serial.print("BPM: ");
            Serial.print(bpm, 1);
            Serial.print(" | SpO2: ");
            Serial.print(spo2, 1);
            Serial.println("%");
          }
        }
        
        // Reset amplitudes for the next pulse cycle
        irAC_max = 0; irAC_min = 0;
        redAC_max = 0; redAC_min = 0;
      } else if (deltaT >= 2000) {
         lastBeatTime = currentTime; // Reset timeout
      }
    }
  }
}
*/