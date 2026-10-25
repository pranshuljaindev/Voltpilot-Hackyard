
/*
  VoltPilot V2 - Robust Serial-Only Firmware
  Board: ESP32-S3 Dev Module
  OLED: SH1106 128x64 I2C, address 0x3C or 0x3D
  Libraries:
    - Adafruit GFX Library
    - Adafruit SH110X

  Serial Monitor: 115200 baud, Newline

  IMPORTANT:
  - Relays are manually controlled only.
  - No WiFi and no automatic source switching.
  - Verify relay polarity and hardware OFF state before use.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// =====================================================
// HARDWARE CONFIGURATION
// =====================================================

const uint8_t V_PIN[4] = {1, 4, 6, 10};
const char* V_NAME[4] = {
  "Solar", "Grid", "Load", "Battery"
};

const uint8_t I_PIN[3] = {2, 5, 7};
const char* I_NAME[3] = {
  "Solar", "Grid", "Load"
};

const uint8_t RELAY_PIN[3] = {15, 16, 17};
const char* RELAY_NAME[3] = {
  "Source", "Load", "Charger"
};

const uint8_t PIN_SDA = 8;
const uint8_t PIN_SCL = 9;

#define RELAY_ACTIVE_LEVEL LOW

const uint32_t SAMPLE_INTERVAL_MS = 300;
const uint32_t OLED_INTERVAL_MS = 750;
const uint32_t PAGE_INTERVAL_MS = 4000;
const uint32_t PRINT_INTERVAL_MS = 3000;
const uint32_t RELAY_MIN_MS = 1500;
const uint32_t OLED_RETRY_MS = 5000;

const float DEFAULT_V_RATIO[4] = {
  5.0f, 5.0f, 5.0f, 2.0f
};

const float V_MAX[4] = {
  15.0f, 15.0f, 15.0f, 4.3f
};

const float ACS_SENS_MV_PER_A = 185.0f;
const float ADC_CLIP_MV = 3100.0f;
const float BAT_FULL_BLOCK_V = 4.15f;
const float BAT_CAPACITY_MAH = 2000.0f;
const float BOOST_EFF = 0.85f;

const uint8_t OLED_ADDR_1 = 0x3C;
const uint8_t OLED_ADDR_2 = 0x3D;

// =====================================================
// GLOBAL STATE
// =====================================================

Adafruit_SH1106G display(128, 64, &Wire, -1);
Preferences prefs;

float vRatio[4] = {
  5.0f, 5.0f, 5.0f, 2.0f
};

float vMv[4] = {0};
float vVal[4] = {0};
bool vOk[4] = {false};
bool vCal[4] = {false};

float iZero[3] = {
  2500.0f, 2500.0f, 2500.0f
};

float iMv[3] = {0};
float iVal[3] = {0};
bool iOk[3] = {false};
bool iZeroSet[3] = {false};

bool rOn[3] = {false, false, false};
bool rSwitched[3] = {false, false, false};
uint32_t rLast[3] = {0, 0, 0};

bool oledOK = false;
bool autoPrint = false;
bool autoPage = true;

uint8_t oledPage = 0;
uint8_t oledAddress = 0;

uint32_t tSample = 0;
uint32_t tOled = 0;
uint32_t tPage = 0;
uint32_t tPrint = 0;
uint32_t tOLEDCheck = 0;
uint32_t zeroArmedAt = 0;

String inputLine = "";

// =====================================================
// SAFE RELAY OUTPUTS
// =====================================================

uint8_t relayOffLevel() {
  return RELAY_ACTIVE_LEVEL == HIGH ? LOW : HIGH;
}

void relayInit() {
  // Set the inactive output latch before enabling outputs.
  for (int i = 0; i < 3; i++) {
    digitalWrite(RELAY_PIN[i], relayOffLevel());
    pinMode(RELAY_PIN[i], OUTPUT);
    digitalWrite(RELAY_PIN[i], relayOffLevel());

    rOn[i] = false;
    rSwitched[i] = false;
  }
}

void relaySet(int i, bool on) {
  if (i < 0 || i > 2) return;

  if (rOn[i] == on) {
    Serial.printf("K%d already %s\n",
                  i + 1, on ? "ON" : "OFF");
    return;
  }

  if (rSwitched[i] &&
      millis() - rLast[i] < RELAY_MIN_MS) {
    Serial.printf(
      "K%d rejected: wait before switching again\n",
      i + 1
    );
    return;
  }

  // Fail-safe check for the charger relay.
  if (i == 2 && on) {
    if (!vOk[3]) {
      Serial.println(
        "K3 ON refused: battery voltage reading invalid"
      );
      return;
    }

    if (vVal[3] >= BAT_FULL_BLOCK_V) {
      Serial.printf(
        "K3 ON refused: battery %.2f V\n",
        vVal[3]
      );
      return;
    }
  }

  digitalWrite(
    RELAY_PIN[i],
    on ? RELAY_ACTIVE_LEVEL : relayOffLevel()
  );

  rOn[i] = on;
  rSwitched[i] = true;
  rLast[i] = millis();

  Serial.printf(
    "K%d (%s) -> %s, GPIO %d\n",
    i + 1,
    RELAY_NAME[i],
    on ? "ON" : "OFF",
    RELAY_PIN[i]
  );
}

void relayAllOff() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(RELAY_PIN[i], relayOffLevel());

    rOn[i] = false;
    rSwitched[i] = true;
    rLast[i] = millis();
  }

  Serial.println("ALL RELAYS COMMANDED OFF");
}

// =====================================================
// SENSOR ACQUISITION
// =====================================================

float readMv(uint8_t pin) {
  uint32_t sum = 0;

  for (int k = 0; k < 16; k++) {
    sum += analogReadMilliVolts(pin);
  }

  return (float)sum / 16.0f;
}

void updateAll() {
  for (int i = 0; i < 4; i++) {
    vMv[i] = readMv(V_PIN[i]);
    vVal[i] = vMv[i] / 1000.0f * vRatio[i];

    vOk[i] =
      vMv[i] >= 10.0f &&
      vMv[i] < ADC_CLIP_MV &&
      vVal[i] <= V_MAX[i];
  }

  for (int i = 0; i < 3; i++) {
    iMv[i] = readMv(I_PIN[i]);

    iVal[i] =
      (iMv[i] - iZero[i]) / ACS_SENS_MV_PER_A;

    iOk[i] =
      iMv[i] >= 50.0f &&
      iMv[i] < ADC_CLIP_MV;
  }
}

// =====================================================
// BATTERY ESTIMATES
// =====================================================

float socOf(float v) {
  const float T[10][2] = {
    {3.0f, 0},
    {3.3f, 5},
    {3.5f, 15},
    {3.6f, 30},
    {3.7f, 50},
    {3.8f, 65},
    {3.9f, 78},
    {4.0f, 88},
    {4.1f, 95},
    {4.2f, 100}
  };

  if (v <= 3.0f) return 0;
  if (v >= 4.2f) return 100;

  for (int k = 1; k < 10; k++) {
    if (v <= T[k][0]) {
      return T[k - 1][1] +
        (T[k][1] - T[k - 1][1]) *
        (v - T[k - 1][0]) /
        (T[k][0] - T[k - 1][0]);
    }
  }

  return 100;
}

float backupHours() {
  if (!vOk[3]) return -1;

  float wh =
    (BAT_CAPACITY_MAH / 1000.0f) *
    3.7f *
    socOf(vVal[3]) / 100.0f *
    BOOST_EFF;

  float watts = 2.1f;

  if (vOk[2] && iOk[2]) {
    float measured = vVal[2] * iVal[2];

    if (measured > 0.5f) {
      watts = measured;
    }
  }

  return wh / watts;
}

// =====================================================
// PERSISTENT CALIBRATION
// =====================================================

void loadCal() {
  prefs.begin("vpilot", false);

  for (int i = 0; i < 4; i++) {
    String kc = "vc" + String(i);
    String kr = "vr" + String(i);

    if (prefs.getBool(kc.c_str(), false)) {
      float saved = prefs.getFloat(
        kr.c_str(), DEFAULT_V_RATIO[i]
      );

      if (saved >= 1.0f && saved <= 30.0f) {
        vRatio[i] = saved;
        vCal[i] = true;
      }
    }
  }

  for (int i = 0; i < 3; i++) {
    String ks = "zs" + String(i);
    String kz = "z" + String(i);

    if (prefs.getBool(ks.c_str(), false)) {
      float saved = prefs.getFloat(
        kz.c_str(), 2500.0f
      );

      if (saved >= 2000.0f && saved <= 3000.0f) {
        iZero[i] = saved;
        iZeroSet[i] = true;
      }
    }
  }
}

void doZero() {
  Serial.println("Starting ACS712 zero calibration...");

  for (int i = 0; i < 3; i++) {
    float sum = 0;
    float mn = 100000.0f;
    float mx = -100000.0f;

    for (int b = 0; b < 12; b++) {
      float mv = readMv(I_PIN[i]);

      sum += mv;
      if (mv < mn) mn = mv;
      if (mv > mx) mx = mv;

      delay(5);
    }

    float mean = sum / 12.0f;

    if (mx - mn > 40.0f) {
      Serial.printf(
        "%s refused: unstable reading\n", I_NAME[i]
      );
      continue;
    }

    if (mean < 2000.0f || mean > 3000.0f) {
      Serial.printf(
        "%s refused: %.0f mV; check wiring\n",
        I_NAME[i], mean
      );
      continue;
    }

    iZero[i] = mean;
    iZeroSet[i] = true;

    String kz = "z" + String(i);
    String ks = "zs" + String(i);

    prefs.putFloat(kz.c_str(), mean);
    prefs.putBool(ks.c_str(), true);

    Serial.printf(
      "%s zero = %.1f mV, saved\n",
      I_NAME[i], mean
    );
  }

  updateAll();
}

// =====================================================
// SERIAL TELEMETRY
// =====================================================

void printReadings() {
  Serial.println("\n========== VOLTPILOT ==========");

  for (int i = 0; i < 4; i++) {
    Serial.printf(
      "%-8s GPIO%-2d ADC=%6.0f mV  V=%7.3f  %s%s\n",
      V_NAME[i],
      V_PIN[i],
      vMv[i],
      vVal[i],
      vOk[i] ? "OK" : "CHECK",
      vCal[i] ? "" : " [DEFAULT RATIO]"
    );
  }

  for (int i = 0; i < 3; i++) {
    Serial.printf(
      "%-8s GPIO%-2d ADC=%6.0f mV  I=%+7.3f A  %s%s\n",
      I_NAME[i],
      I_PIN[i],
      iMv[i],
      iVal[i],
      iOk[i] ? "OK" : "CHECK",
      iZeroSet[i] ? "" : " [ZERO NOT CALIBRATED]"
    );
  }

  if (vOk[3]) {
    Serial.printf(
      "Battery SoC estimate: %.0f%%\n",
      socOf(vVal[3])
    );

    float hours = backupHours();

    if (hours >= 0) {
      Serial.printf(
        "Backup estimate: %.2f hours\n", hours
      );
    }
  }

  Serial.printf(
    "Relays: K1=%s K2=%s K3=%s\n",
    rOn[0] ? "ON" : "OFF",
    rOn[1] ? "ON" : "OFF",
    rOn[2] ? "ON" : "OFF"
  );

  Serial.printf(
    "OLED: %s | Page: %u\n",
    oledOK ? "READY" : "UNAVAILABLE",
    oledPage + 1
  );

  Serial.println("===============================\n");
}

void printHelp() {
  Serial.println("\n--- VOLTPILOT COMMANDS ---");
  Serial.println("help              Show commands");
  Serial.println("read              Refresh and print sensors");
  Serial.println("auto on           Telemetry every 3 seconds");
  Serial.println("auto off          Stop automatic telemetry");
  Serial.println("k1 on/off         Source relay");
  Serial.println("k2 on/off         Load relay");
  Serial.println("k3 on/off         Charger relay");
  Serial.println("alloff            Command all relays OFF");
  Serial.println("page              Next OLED page");
  Serial.println("pages auto        Enable automatic page cycling");
  Serial.println("pages manual      Disable automatic page cycling");
  Serial.println("zero              Arm current sensor calibration");
  Serial.println("zero confirm      Calibrate with no current flowing");
  Serial.println("vcal solar 5.02   Calibrate solar voltage");
  Serial.println("vcal grid 5.02    Calibrate grid voltage");
  Serial.println("vcal load 5.02    Calibrate load voltage");
  Serial.println("vcal bat 3.80     Calibrate battery voltage");
  Serial.println("resetcal          Erase saved calibration");
  Serial.println("--------------------------\n");
}

// =====================================================
// OLED INITIALIZATION AND RECOVERY
// =====================================================

bool oledProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool oledInit() {
  oledOK = false;

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  uint8_t addr = 0;

  if (oledProbe(OLED_ADDR_1)) {
    addr = OLED_ADDR_1;
  } else if (oledProbe(OLED_ADDR_2)) {
    addr = OLED_ADDR_2;
  }

  if (addr == 0) {
    Serial.println("OLED not detected at 0x3C or 0x3D");
    return false;
  }

  if (!display.begin(addr, true)) {
    Serial.println("OLED initialization failed");
    return false;
  }

  oledAddress = addr;
  oledOK = true;

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 20);
  display.println("VoltPilot");
  display.setTextSize(1);
  display.setCursor(0, 48);
  display.println("System starting...");
  display.display();

  Serial.printf(
    "OLED ready at I2C address 0x%02X\n",
    oledAddress
  );

  return true;
}

void oledRecovery() {
  Serial.println("Attempting OLED/I2C recovery...");

  oledOK = false;

  Wire.end();
  delay(20);

  oledInit();

  if (oledOK) {
    drawOled();
    Serial.println("OLED reinitialized");
  } else {
    Serial.println(
      "OLED still unavailable; retrying later"
    );
  }
}

// =====================================================
// OLED RENDERING
// =====================================================

void drawOled() {
  if (!oledOK) return;

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  if (oledPage <= 2) {
    int k = oledPage;
    int vi = k;

    display.println(
      k == 0 ? "SOLAR MONITOR" :
      k == 1 ? "GRID MONITOR" : "LOAD MONITOR"
    );

    display.drawLine(0, 10, 127, 10, SH110X_WHITE);

    display.setTextSize(2);
    display.setCursor(0, 17);
    display.print("V ");

    if (vOk[vi]) {
      display.print(vVal[vi], 2);
      display.println("V");
    } else {
      display.println("ERR");
    }

    display.setCursor(0, 41);
    display.print("I ");

    if (iOk[k]) {
      display.print(iVal[k], 2);
      display.println("A");
    } else {
      display.println("ERR");
    }
  }

  else if (oledPage == 3) {
    display.println("BATTERY STATUS");
    display.drawLine(0, 10, 127, 10, SH110X_WHITE);

    display.setTextSize(2);
    display.setCursor(0, 16);

    if (vOk[3]) {
      display.print(vVal[3], 2);
      display.println("V");

      display.setCursor(0, 39);
      display.print(socOf(vVal[3]), 0);
      display.println("%");
    } else {
      display.println("VOLT ERR");
    }
  }

  else {
    display.println("RELAY STATUS");
    display.drawLine(0, 10, 127, 10, SH110X_WHITE);

    display.setTextSize(2);

    display.setCursor(0, 15);
    display.print("K1 ");
    display.println(rOn[0] ? "ON" : "OFF");

    display.setCursor(0, 31);
    display.print("K2 ");
    display.println(rOn[1] ? "ON" : "OFF");

    display.setCursor(0, 47);
    display.print("K3 ");
    display.println(rOn[2] ? "ON" : "OFF");
  }

  // I2C calls have a configured timeout.
  display.display();
}

// =====================================================
// COMMAND PARSING
// =====================================================

String tok(const String &s, int n) {
  int start = 0;

  for (int k = 0; k < n; k++) {
    int sp = s.indexOf(' ', start);

    if (sp < 0) return "";

    start = sp + 1;

    while (start < (int)s.length() &&
           s[start] == ' ') {
      start++;
    }
  }

  int end = s.indexOf(' ', start);

  if (end < 0) end = s.length();

  return s.substring(start, end);
}

void handleCommand(String line) {
  line.trim();
  line.toLowerCase();

  if (line.length() == 0) return;

  String a = tok(line, 0);
  String b = tok(line, 1);
  String c = tok(line, 2);

  if (a == "help") {
    printHelp();
  }

  else if (a == "read") {
    updateAll();
    printReadings();
  }

  else if (a == "auto") {
    if (b == "on") {
      autoPrint = true;
      Serial.println("Automatic telemetry ON");
    } else if (b == "off") {
      autoPrint = false;
      Serial.println("Automatic telemetry OFF");
    } else {
      Serial.println("Usage: auto on/off");
    }
  }

  else if (a == "alloff") {
    relayAllOff();
  }

  else if (a == "page") {
    oledPage = (oledPage + 1) % 5;
    tPage = millis();
    drawOled();
  }

  else if (a == "pages") {
    if (b == "auto") {
      autoPage = true;
      Serial.println("Automatic OLED pages ON");
    } else if (b == "manual") {
      autoPage = false;
      Serial.println("Automatic OLED pages OFF");
    } else {
      Serial.println("Usage: pages auto/manual");
    }
  }

  else if (a == "k1" || a == "k2" || a == "k3") {
    int i = a[1] - '1';

    if (b == "on") {
      relaySet(i, true);
    } else if (b == "off") {
      relaySet(i, false);
    } else {
      Serial.println("Usage: k1 on/off");
    }
  }

  else if (a == "zero") {
    if (b == "confirm") {
      if (zeroArmedAt == 0 ||
          millis() - zeroArmedAt > 20000) {
        Serial.println("Type zero first");
        zeroArmedAt = 0;
        return;
      }

      zeroArmedAt = 0;

      for (int i = 0; i < 3; i++) {
        if (rOn[i]) {
          Serial.printf(
            "REFUSED: K%d is ON; use alloff first\n",
            i + 1
          );
          return;
        }
      }

      Serial.println(
        "Ensure every ACS712 has zero current flowing."
      );

      doZero();
    } else {
      zeroArmedAt = millis();

      Serial.println(
        "Calibration armed for 20 seconds."
      );
      Serial.println(
        "Use alloff; ensure zero current; then zero confirm."
      );
    }
  }

  else if (a == "vcal") {
    int idx = -1;

    if (b == "solar") idx = 0;
    else if (b == "grid") idx = 1;
    else if (b == "load") idx = 2;
    else if (b == "bat") idx = 3;

    float actual = c.toFloat();

    if (idx < 0 || actual <= 0.05f) {
      Serial.println(
        "Usage: vcal solar/grid/load/bat <multimeter volts>"
      );
      return;
    }

    updateAll();

    if (vMv[idx] < 100.0f ||
        vMv[idx] >= ADC_CLIP_MV) {
      Serial.println("Calibration refused: ADC invalid");
      return;
    }

    float ratio = actual * 1000.0f / vMv[idx];

    if (ratio < 1.0f || ratio > 30.0f) {
      Serial.println("Calibration ratio implausible");
      return;
    }

    vRatio[idx] = ratio;
    vCal[idx] = true;

    String kr = "vr" + String(idx);
    String kc = "vc" + String(idx);

    prefs.putFloat(kr.c_str(), ratio);
    prefs.putBool(kc.c_str(), true);

    Serial.printf(
      "%s calibration saved: ratio %.4f\n",
      V_NAME[idx], ratio
    );
  }

  else if (a == "resetcal") {
    prefs.clear();

    for (int i = 0; i < 4; i++) {
      vRatio[i] = DEFAULT_V_RATIO[i];
      vCal[i] = false;
    }

    for (int i = 0; i < 3; i++) {
      iZero[i] = 2500.0f;
      iZeroSet[i] = false;
    }

    Serial.println("Saved calibration erased");
  }

  else {
    Serial.println("Unknown command. Type help");
  }
}

void handleSerial() {
  while (Serial.available()) {
    char ch = Serial.read();

    if (ch == '\n' || ch == '\r') {
      if (inputLine.length() > 0) {
        handleCommand(inputLine);
        inputLine = "";
      }
    }

    else if (inputLine.length() < 100) {
      inputLine += ch;
    }
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  // Relay outputs are initialized before other peripherals.
  relayInit();

  Serial.begin(115200);
  delay(500);

  Serial.println("\n==============================");
  Serial.println("VoltPilot V2 Firmware");
  Serial.println("Manual relay control only");
  Serial.println("==============================");

  analogReadResolution(12);

  for (int i = 0; i < 4; i++) {
    analogSetPinAttenuation(V_PIN[i], ADC_11db);
  }

  for (int i = 0; i < 3; i++) {
    analogSetPinAttenuation(I_PIN[i], ADC_11db);
  }

  loadCal();

  oledInit();

  updateAll();
  printReadings();
  printHelp();

  uint32_t now = millis();

  tSample = now;
  tOled = now;
  tPage = now;
  tPrint = now;
  tOLEDCheck = now;
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  handleSerial();

  uint32_t now = millis();

  if (now - tSample >= SAMPLE_INTERVAL_MS) {
    tSample = now;
    updateAll();
  }

  if (autoPage &&
      now - tPage >= PAGE_INTERVAL_MS) {
    tPage = now;
    oledPage = (oledPage + 1) % 5;
  }

  if (now - tOled >= OLED_INTERVAL_MS) {
    tOled = now;
    drawOled();
  }

  if (autoPrint &&
      now - tPrint >= PRINT_INTERVAL_MS) {
    tPrint = now;
    printReadings();
  }

  // Retry if the display is absent or its bus disappears.
  // A display that still ACKs but has a hardware fault
  // cannot always be detected automatically.
  if (now - tOLEDCheck >= OLED_RETRY_MS) {
    tOLEDCheck = now;

    if (!oledOK) {
      oledInit();
    } else if (!oledProbe(oledAddress)) {
      oledRecovery();
    }
  }
}
