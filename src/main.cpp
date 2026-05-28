#include <Arduino.h>
#include <WiFi.h>
#include "ThingSpeak.h"

const char* WIFI_SSID     = "anhdt";
const char* WIFI_PASSWORD = "11111111";

const unsigned long CHANNEL_ID     = 3394500;
const char*         WRITE_API_KEY  = "VWKF6KF6SDXNIC3Y";
const char*         READ_API_KEY   = "I6YRL6YOAS32M5TF";

#define MOISTURE_PIN  36
#define RELAY_PIN     26

const int ADC_DRY = 3300;
const int ADC_WET = 1500;

const int MOISTURE_LOW  = 30;
const int MOISTURE_HIGH = 60;

const unsigned long SEND_INTERVAL_MS = 20000;
const unsigned long READ_INTERVAL_MS = 15000;

enum PumpMode {
  AUTO       = 0,
  MANUAL_ON  = 1,
  MANUAL_OFF = 2
};

WiFiClient    client;
bool          pumpRunning    = false;
PumpMode      pumpMode       = AUTO;
unsigned long lastSendTime   = 0;
unsigned long lastReadTime   = 0;

void connectWiFi() {
  Serial.printf("\n[WiFi] Đang kết nối tới \"%s\"", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs > 15000) {
      Serial.println("\n[WiFi] Timeout! Thử lại sau 5s...");
      delay(5000);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      startMs = millis();
    }
    Serial.print(".");
    delay(500);
  }

  Serial.printf("\n[WiFi] Đã kết nối! IP: %s\n", WiFi.localIP().toString().c_str());
}

int readMoisturePercent() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(MOISTURE_PIN);
    delay(5);
  }
  int adcValue = sum / 10;

  int percent = map(adcValue, ADC_DRY, ADC_WET, 0, 100);
  percent = constrain(percent, 0, 100);

  Serial.printf("[Sensor] ADC raw: %d → Độ ẩm: %d%%\n", adcValue, percent);
  return percent;
}

void readPumpCommand() {
  if (WiFi.status() != WL_CONNECTED) return;

  long raw = ThingSpeak.readLongField(CHANNEL_ID, 3, READ_API_KEY);
  int  httpCode = ThingSpeak.getLastReadStatus();

  if (httpCode != 200) {
    Serial.printf("[ThingSpeak] Đọc lệnh thất bại: HTTP %d\n", httpCode);
    return;
  }

  PumpMode newMode = AUTO;
  if (raw == 1) newMode = MANUAL_ON;
  else if (raw == 2) newMode = MANUAL_OFF;

  if (newMode != pumpMode) {
    const char* modeNames[] = {"AUTO", "MANUAL_ON", "MANUAL_OFF"};
    Serial.printf("[ThingSpeak] Đổi chế độ: %s → %s\n",
                  modeNames[pumpMode], modeNames[newMode]);
    pumpMode = newMode;
  } else {
    Serial.printf("[ThingSpeak] Chế độ giữ nguyên: %s\n",
                  (pumpMode == AUTO ? "AUTO" : (pumpMode == MANUAL_ON ? "MANUAL_ON" : "MANUAL_OFF")));
  }
}

void controlPump(int moisturePercent) {
  bool shouldRun = pumpRunning;

  if (pumpMode == MANUAL_ON) {
    shouldRun = true;
    if (!pumpRunning) {
      Serial.println("[Pump] BẬT (thủ công từ ThingSpeak)");
    }
  } else if (pumpMode == MANUAL_OFF) {
    shouldRun = false;
    if (pumpRunning) {
      Serial.println("[Pump] TẮT (thủ công từ ThingSpeak)");
    }
  } else {
    if (!pumpRunning && moisturePercent < MOISTURE_LOW) {
      shouldRun = true;
      Serial.printf("[Pump] BẬT (tự động) — độ ẩm %d%% < ngưỡng %d%%\n",
                    moisturePercent, MOISTURE_LOW);
    } else if (pumpRunning && moisturePercent >= MOISTURE_HIGH) {
      shouldRun = false;
      Serial.printf("[Pump] TẮT (tự động) — độ ẩm %d%% ≥ ngưỡng %d%%\n",
                    moisturePercent, MOISTURE_HIGH);
    }
  }

  if (shouldRun != pumpRunning) {
    digitalWrite(RELAY_PIN, shouldRun ? HIGH : LOW);
    pumpRunning = shouldRun;
  }
}

void sendToThingSpeak(int moisturePercent) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ThingSpeak] WiFi mất kết nối, đang kết nối lại...");
    connectWiFi();
  }

  ThingSpeak.setField(1, moisturePercent);
  ThingSpeak.setField(2, pumpRunning ? 1 : 0);
  ThingSpeak.setField(3, (int)pumpMode);

  int result = ThingSpeak.writeFields(CHANNEL_ID, WRITE_API_KEY);
  if (result == 200) {
    Serial.printf("[ThingSpeak] Gửi thành công: ẩm=%d%%, bơm=%s, chế độ=%d\n",
                  moisturePercent, pumpRunning ? "BẬT" : "TẮT", (int)pumpMode);
  } else {
    Serial.printf("[ThingSpeak] Lỗi: HTTP %d\n", result);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  analogSetAttenuation(ADC_11db);
  analogSetWidth(12);

  connectWiFi();
  ThingSpeak.begin(client);

  Serial.println("[System] Khởi động hoàn tất!\n");
  Serial.println("[System] Điều khiển thủ công: ghi Field 3 trên ThingSpeak");
  Serial.println("         0 = Tự động  |  1 = Bật bơm  |  2 = Tắt bơm\n");
}

void loop() {
  unsigned long now = millis();

  if (now - lastReadTime >= READ_INTERVAL_MS) {
    readPumpCommand();
    lastReadTime = now;
  }

  int moisture = readMoisturePercent();

  controlPump(moisture);

  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    sendToThingSpeak(moisture);
    lastSendTime = now;
  }

  delay(1000);
}
