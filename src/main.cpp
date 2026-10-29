#include <Arduino.h>
#include <SoftwareSerial.h>
#include <max6675.h>

// =============================================================
// ARDUINO NANO SENSOR AGGREGATOR & TELEMETRY SENDER
// =============================================================
// Flow Sensors (SEN-HZ21WA)
const int FLOW_SENSOR_1_PIN = 2; // Pin D2 (Hardware Interrupt 0)
const int FLOW_SENSOR_2_PIN = 3; // Pin D3 (Hardware Interrupt 1)

// MAX6675 Thermocouples
const int THERMO_SO   = 4;  // Pin D4 (Shared Data Out)
const int THERMO1_CS  = 5;  // Pin D5 (Dedicated CS #1)
const int THERMO_SCK  = 6;  // Pin D6 (Shared Clock)
const int THERMO2_CS  = 7;  // Pin D7 (Dedicated CS #2)

// Dedicated SoftwareSerial link to ESP32: TX on Pin D8, RX on Pin D9
SoftwareSerial espSerial(9, 8); // RX on D9, TX on D8

const int LED_PIN = 13;

// Initialize MAX6675 instances
MAX6675 thermo1(THERMO_SCK, THERMO1_CS, THERMO_SO);
MAX6675 thermo2(THERMO_SCK, THERMO2_CS, THERMO_SO);

// Flow sensor variables
volatile unsigned long pulseCount1 = 0;
volatile unsigned long pulseCount2 = 0;

unsigned long lastTime = 0;
float flowRate1LPM = 0.0;
float flowRate2LPM = 0.0;
float totalVolume1ML = 0.0;
float totalVolume2ML = 0.0;

// Interrupt Service Routines
void pulseCounter1() { pulseCount1++; }
void pulseCounter2() { pulseCount2++; }

void setup() {
  Serial.begin(115200);      // For PC USB Monitor
  espSerial.begin(9600);     // Dedicated link to ESP32 (Pin D8 TX)
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  pinMode(FLOW_SENSOR_1_PIN, INPUT_PULLUP);
  pinMode(FLOW_SENSOR_2_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_1_PIN), pulseCounter1, FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_2_PIN), pulseCounter2, FALLING);

  Serial.println(F("\n[NANO] Dual Flow & Dual Thermal Aggregator Ready."));
  Serial.println(F("[NANO] Streaming data to USB Serial and ESP32 Link (Pin D8)..."));
  lastTime = millis();
}

void loop() {
  unsigned long currentTime = millis();

  if (currentTime - lastTime >= 1000) {
    unsigned long interval = currentTime - lastTime;
    lastTime = currentTime;

    // Retrieve and reset pulse counts safely
    noInterrupts();
    unsigned long pulses1 = pulseCount1;
    unsigned long pulses2 = pulseCount2;
    pulseCount1 = 0;
    pulseCount2 = 0;
    interrupts();

    // Pulse conversion formula: F (Hz) = 10 * Q (L/min)
    float freq1Hz = (pulses1 * 1000.0) / interval;
    float freq2Hz = (pulses2 * 1000.0) / interval;
    flowRate1LPM = freq1Hz / 10.0;
    flowRate2LPM = freq2Hz / 10.0;

    totalVolume1ML += (flowRate1LPM / 60.0) * interval;
    totalVolume2ML += (flowRate2LPM / 60.0) * interval;

    // Read Thermocouples
    float t1 = thermo1.readCelsius();
    delay(80);
    float t2 = thermo2.readCelsius();

    // Sanitize disconnected / invalid values
    if (isnan(t1) || t1 <= 0.0) t1 = 0.0;
    if (isnan(t2) || t2 <= 0.0) t2 = 0.0;

    // Toggle onboard LED heartbeat
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));

    // Construct JSON packet for ESP32
    String jsonPacket = "{\"f1\":";
    jsonPacket += String(flowRate1LPM, 2);
    jsonPacket += ",\"v1\":";
    jsonPacket += String(totalVolume1ML, 0);
    jsonPacket += ",\"f2\":";
    jsonPacket += String(flowRate2LPM, 2);
    jsonPacket += ",\"v2\":";
    jsonPacket += String(totalVolume2ML, 0);
    jsonPacket += ",\"t1\":";
    jsonPacket += String(t1, 2);
    jsonPacket += ",\"t2\":";
    jsonPacket += String(t2, 2);
    jsonPacket += "}";

    // Transmit to ESP32 over Pin D8
    espSerial.println(jsonPacket);

    // Also display on PC Serial Monitor
    Serial.print(F("[NANO Telemetry] "));
    Serial.println(jsonPacket);
  }
}
