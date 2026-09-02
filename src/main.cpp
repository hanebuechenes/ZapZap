#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- Display Konfiguration ---
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Pin-Definitionen ---
#define PIN_POWER_HOLD  7   // PD7: Selbsthaltung MOSFET Q8/Q9
#define PIN_TOUCH_1     8   // PB0: Touch 1 (Modus / Navigation)
#define PIN_TOUCH_2     9   // PB1: Touch 2 (Power / Long-Press Off)
#define PIN_TOUCH_3     10  // PB2: Touch 3 (Start / Stop Zappen)
#define PIN_POTI        4   // PD4: Poti / Wertanpassung
#define PIN_I_OUT_PWM   5   // PD5: PWM-Ausgang zum OpAmp
#define PIN_CTRL_A      2   // PD2: H-Brücke A
#define PIN_CTRL_B      3   // PD3: H-Brücke B
#define PIN_V_MEAS      A0  // PC0: Spannungssensor Elektroden

// --- Programmzustände ---
enum SystemState {
  STATE_MENU,
  STATE_RUNNING,
  STATE_PAUSED
};

SystemState currentState = STATE_MENU;

// --- Betriebsparameter ---
int targetCurrentmA = 100;     // Zielstrom in µA/mA (je nachdem wie OpAmp skaliert ist)
const float SCHUMANN_HALF_FREQ_MS = 127.71; // 3.915 Hz Halbwelle
unsigned long pressStartTime2 = 0;
unsigned long lastToggleTime = 0;
unsigned long sessionStartTime = 0;
bool hBridgeState = false;

// Taster-Debounce
bool lastT1 = LOW, lastT3 = LOW;

void updateDisplay();
void powerOff();

void setup() {
  // 1. SOFORT Stromversorgung halten
  pinMode(PIN_POWER_HOLD, OUTPUT);
  digitalWrite(PIN_POWER_HOLD, HIGH);

  // 2. Pins initialisieren
  pinMode(PIN_TOUCH_1, INPUT);
  pinMode(PIN_TOUCH_2, INPUT);
  pinMode(PIN_TOUCH_3, INPUT);
  pinMode(PIN_POTI, INPUT);
  
  pinMode(PIN_CTRL_A, OUTPUT);
  pinMode(PIN_CTRL_B, OUTPUT);
  pinMode(PIN_I_OUT_PWM, OUTPUT);

  digitalWrite(PIN_CTRL_A, LOW);
  digitalWrite(PIN_CTRL_B, LOW);
  analogWrite(PIN_I_OUT_PWM, 0);

  Serial.begin(115200);

  // 3. Display starten (I2C-Adresse meist 0x3C)
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 Allokation fehlgeschlagen!"));
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(20, 20);
  display.println(F("ZapZap"));
  display.setTextSize(1);
  display.setCursor(25, 45);
  display.println(F("3.915 Hz CC/CV"));
  display.display();
  delay(1500);
}

void loop() {
  unsigned long now = millis();

  // --- A. POWER-OFF (Touch 2 halten) ---
  if (digitalRead(PIN_TOUCH_2) == HIGH) {
    if (pressStartTime2 == 0) pressStartTime2 = now;
    else if (now - pressStartTime2 >= 2000) powerOff();
  } else {
    pressStartTime2 = 0;
  }

  // --- B. TASTRAD / NAVIGATION (Touch 1 & Touch 3 Einzelklicks) ---
  bool t1 = digitalRead(PIN_TOUCH_1);
  bool t3 = digitalRead(PIN_TOUCH_3);

  // Touch 1: Strom-Sollwert anpassen (im Menü)
  if (t1 == HIGH && lastT1 == LOW) {
    if (currentState == STATE_MENU) {
      targetCurrentmA += 50;
      if (targetCurrentmA > 1000) targetCurrentmA = 50; // Zyklisch 50-1000
    }
  }
  lastT1 = t1;

  // Touch 3: Start / Pause / Stop Toggle
  if (t3 == HIGH && lastT3 == LOW) {
    if (currentState == STATE_MENU) {
      currentState = STATE_RUNNING;
      sessionStartTime = now;
    } else if (currentState == STATE_RUNNING) {
      currentState = STATE_PAUSED;
      digitalWrite(PIN_CTRL_A, LOW);
      digitalWrite(PIN_CTRL_B, LOW);
      analogWrite(PIN_I_OUT_PWM, 0);
    } else if (currentState == STATE_PAUSED) {
      currentState = STATE_RUNNING;
    }
  }
  lastT3 = t3;

  // --- C. ABLAUF-LOGIK JE NACH ZUSTAND ---
  if (currentState == STATE_RUNNING) {
    // 1. Polwechsel mit 3.915 Hz
    if (now - lastToggleTime >= (unsigned long)SCHUMANN_HALF_FREQ_MS) {
      lastToggleTime = now;
      hBridgeState = !hBridgeState;

      if (hBridgeState) {
        digitalWrite(PIN_CTRL_A, HIGH);
        digitalWrite(PIN_CTRL_B, LOW);
      } else {
        digitalWrite(PIN_CTRL_A, LOW);
        digitalWrite(PIN_CTRL_B, HIGH);
      }
    }

    // 2. Stromausgabe setzen (Mapping 0-1000 µA/mA auf PWM 0-255)
    int pwmOut = map(targetCurrentmA, 0, 1000, 0, 255);
    analogWrite(PIN_I_OUT_PWM, pwmOut);
  }

  // --- D. DISPLAY REFRESH (alle 200ms) ---
  static unsigned long lastDisplayUpdate = 0;
  if (now - lastDisplayUpdate >= 200) {
    lastDisplayUpdate = now;
    updateDisplay();
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  // Statuszeile oben
  if (currentState == STATE_MENU) {
    display.println(F("[ BEREIT ]"));
  } else if (currentState == STATE_RUNNING) {
    display.println(F("[ ZAPPEN AKTIV ]"));
  } else {
    display.println(F("[ PAUSE ]"));
  }

  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Frequenz & Soll-Strom
  display.setCursor(0, 18);
  display.print(F("Freq : 3.915 Hz"));

  display.setCursor(0, 30);
  display.print(F("Strom: "));
  display.print(targetCurrentmA);
  display.print(F(" uA"));

  // Spannungsmessung Elektroden
  int vRaw = analogRead(PIN_V_MEAS);
  float vVolt = (vRaw / 1023.0) * 3.3 * 8.27; // Bspw. Teileranpassung
  
  display.setCursor(0, 42);
  display.print(F("U_Out: "));
  display.print(vVolt, 1);
  display.print(F(" V"));

  // Laufzeit
  if (currentState == STATE_RUNNING) {
    unsigned long runSecs = (millis() - sessionStartTime) / 1000;
    display.setCursor(0, 54);
    display.print(F("Zeit : "));
    display.print(runSecs / 60);
    display.print(F("m "));
    display.print(runSecs % 60);
    display.print(F("s"));
  }

  display.display();
}

void powerOff() {
  display.clearDisplay();
  display.setCursor(20, 25);
  display.setTextSize(2);
  display.println(F("GUTE NACHT"));
  display.display();
  delay(1000);

  digitalWrite(PIN_CTRL_A, LOW);
  digitalWrite(PIN_CTRL_B, LOW);
  analogWrite(PIN_I_OUT_PWM, 0);
  digitalWrite(PIN_POWER_HOLD, LOW);

  while (true) delay(100);
}