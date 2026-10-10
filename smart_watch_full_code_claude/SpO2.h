#ifndef SPO2_H
#define SPO2_H

// ============================================================
// SpO2 - algorithm from the bare test code, as a header
//
// This file does NOT talk to the sensor. heart_rate.h reads the MAX30102 FIFO
// and feeds every Red/IR sample and every accepted heartbeat into it:
//
//   spo2_begin(ir, red)           finger just placed
//   spo2_sample(ir, red, track)   every sample while the finger is on
//   spo2_cycle_start()            a beat cycle starts (clears the AC min/max)
//   spo2_beat()                   one valid heartbeat finished -> one SpO2 value
//   spo2_reset_signal()           finger lost
//   spo2_reset_all()              new measurement
//
// Method (same as the bare code):
//   DC = EMA(0.95) of Red and IR, AC = sample - DC
//   per beat: amplitude = AC max - AC min (both channels)
//   R = (ACred / DCred) / (ACir / DCir)
//   SpO2 = 103 - 17 * R   (empirical; capped to 100, values < 50 rejected)
// Result = trimmed mean of the last 6 valid beats when they agree.
//
// IMPORTANT: 103 - 17*R is an empirical line. Compare against a reference
// pulse oximeter and tune SPO2_CAL_A / SPO2_CAL_B. Not a medical device.
// ============================================================

#include <Arduino.h>

// ---------------- Calibration (from the bare code) ----------------
#define SPO2_CAL_A            103.0f
#define SPO2_CAL_B            17.0f

// ---------------- Algorithm tuning ----------------
#define SPO2_DC_ALPHA         0.95f   // same EMA constant as the HR baseline
#define SPO2_MIN_AMPLITUDE    50.0f   // AC amplitude (raw counts) needed on both channels
#define SPO2_MIN_VALID        50.0f   // computed SpO2 below this is rejected
#define SPO2_MAX_VALUE        100.0f
#define SPO2_WINDOW           6       // beats kept for the result
#define SPO2_STABLE_SPREAD    3       // middle 4 of 6 must agree within this (% points)
#define SPO2_MIN_LIVE         3       // values needed before a live number is shown

// ---------------- Internal state ----------------
static bool   sp_dcInit = false;
static float  sp_dcIR   = 0;
static float  sp_dcRed  = 0;
static float  sp_irMax  = 0, sp_irMin  = 0;
static float  sp_redMax = 0, sp_redMin = 0;

static float  sp_vals[SPO2_WINDOW];
static int    sp_head   = 0;
static int    sp_count  = 0;

static int    sp_live   = 0;
static bool   sp_done   = false;
static int    sp_result = 0;

static void spo2_cycle_start() {
  sp_irMax = 0; sp_irMin = 0;
  sp_redMax = 0; sp_redMin = 0;
}

// Forget signal history (finger lost / new finger). Keeps a finished result.
static void spo2_reset_signal() {
  sp_dcInit = false;
  sp_dcIR = 0;
  sp_dcRed = 0;
  sp_head = 0;
  sp_count = 0;
  sp_live = 0;
  spo2_cycle_start();
}

// New measurement: forget everything
static void spo2_reset_all() {
  spo2_reset_signal();
  sp_done = false;
  sp_result = 0;
}

// Finger just placed: start the DC baselines at the current level
static void spo2_begin(uint32_t ir, uint32_t red) {
  spo2_reset_signal();
  sp_dcIR = (float)ir;
  sp_dcRed = (float)red;
  sp_dcInit = true;
}

// Every sample while the finger is on. track=false while the baseline settles.
static void spo2_sample(uint32_t ir, uint32_t red, bool track) {
  if (!sp_dcInit) {
    sp_dcIR = (float)ir;
    sp_dcRed = (float)red;
    sp_dcInit = true;
  } else {
    sp_dcIR  = (SPO2_DC_ALPHA * sp_dcIR)  + ((1.0f - SPO2_DC_ALPHA) * ir);
    sp_dcRed = (SPO2_DC_ALPHA * sp_dcRed) + ((1.0f - SPO2_DC_ALPHA) * red);
  }
  if (!track) return;

  float irAC  = ir  - sp_dcIR;
  float redAC = red - sp_dcRed;
  if (irAC  > sp_irMax)  sp_irMax  = irAC;
  if (irAC  < sp_irMin)  sp_irMin  = irAC;
  if (redAC > sp_redMax) sp_redMax = redAC;
  if (redAC < sp_redMin) sp_redMin = redAC;
}

static void spo2_push(float v) {
  sp_vals[sp_head] = v;
  sp_head = (sp_head + 1) % SPO2_WINDOW;
  if (sp_count < SPO2_WINDOW) sp_count++;

  float s[SPO2_WINDOW];
  int n = sp_count;
  for (int i = 0; i < n; i++) s[i] = sp_vals[i];
  for (int i = 1; i < n; i++) {                 // insertion sort
    float x = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
    s[j + 1] = x;
  }

  // Live value = median so far
  if (n >= SPO2_MIN_LIVE) {
    float med = (n % 2) ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
    sp_live = (int)(med + 0.5f);
  }

  // Final: full window, drop lowest and highest, the rest must agree
  if (n == SPO2_WINDOW) {
    float lo = s[1], hi = s[SPO2_WINDOW - 2];
    if ((hi - lo) <= SPO2_STABLE_SPREAD) {
      float sum = 0;
      for (int i = 1; i <= SPO2_WINDOW - 2; i++) sum += s[i];
      sp_result = (int)(sum / (SPO2_WINDOW - 2) + 0.5f);
      sp_live = sp_result;
      sp_done = true;
    }
  }
}

// One valid heartbeat finished: turn the cycle's AC amplitudes into one SpO2 value
static void spo2_beat() {
  float irAmp  = sp_irMax  - sp_irMin;
  float redAmp = sp_redMax - sp_redMin;
  spo2_cycle_start();                           // next cycle starts clean

  if (sp_done) return;
  if (sp_dcIR <= 0 || sp_dcRed <= 0) return;
  if (irAmp <= SPO2_MIN_AMPLITUDE || redAmp <= SPO2_MIN_AMPLITUDE) return;

  float ratio = (redAmp / sp_dcRed) / (irAmp / sp_dcIR);
  float spo2 = SPO2_CAL_A - (SPO2_CAL_B * ratio);

  if (spo2 > SPO2_MAX_VALUE) spo2 = SPO2_MAX_VALUE;
  if (spo2 < SPO2_MIN_VALID) return;            // invalid reading
  spo2_push(spo2);
}

// ---------------- Public getters ----------------
inline int  spo2_live()   { return sp_live; }     // 0 = not enough beats yet
inline bool spo2_done()   { return sp_done; }
inline int  spo2_result() { return sp_result; }   // valid when spo2_done()

#endif // SPO2_H