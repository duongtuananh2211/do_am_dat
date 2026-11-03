#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include "ThingSpeak.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Thiếu include/secrets.h — sao chép từ include/secrets.h.example rồi điền giá trị"
#endif

// ============================================================
//  CẤU HÌNH
// ============================================================

// 1 = in log đo thời gian (chu kỳ loop, thời gian request, độ trễ)
// 0 = tắt cho bản nộp
#define DEBUG_TIMING 1

#if DEBUG_TIMING
#define TLOG(...) Serial.printf(__VA_ARGS__)
#else
#define TLOG(...) ((void)0)
#endif

const char*         WIFI_SSID           = WIFI_SSID_VALUE;
const char*         WIFI_PASSWORD       = WIFI_PASSWORD_VALUE;
const unsigned long CHANNEL_ID          = TS_CHANNEL_ID;
const char*         WRITE_API_KEY       = TS_WRITE_API_KEY;
const unsigned long TALKBACK_ID         = TS_TALKBACK_ID;
const char*         TALKBACK_API_KEY    = TS_TALKBACK_API_KEY;

#define MOISTURE_PIN  36
#define RELAY_PIN     26

// Hiệu chuẩn ADC — TODO: thay bằng giá trị đo thật (trung bình nhiều lần đo,
// xem log "[Sensor] ADC raw"): ADC_DRY = đầu dò ngoài không khí,
// ADC_WET = đầu dò nhúng nước. Yêu cầu ADC_DRY > ADC_WET.
const int ADC_DRY = 3300;
const int ADC_WET = 1500;
// ADC lệch ra ngoài [ADC_WET - margin, ADC_DRY + margin] → coi là cảm biến lỗi
const int SENSOR_FAULT_MARGIN = 300;

const int MOISTURE_LOW  = 30;
const int MOISTURE_HIGH = 60;

const unsigned long SEND_INTERVAL_MS    = 20000;  // chu kỳ gửi dữ liệu
const unsigned long MIN_WRITE_GAP_MS    = 15500;  // giới hạn ghi ThingSpeak free (15 s)
const unsigned long READ_INTERVAL_MS    = 5000;   // chu kỳ hỏi lệnh TalkBack

const unsigned long WIFI_SETUP_WAIT_MS  = 10000;  // chờ WiFi tối đa trong setup()
const unsigned long WIFI_RETRY_MS       = 10000;  // khoảng giữa 2 lần reconnect
const unsigned long OFFLINE_FALLBACK_MS = 60000;  // mất mạng quá lâu → về AUTO

const unsigned long MANUAL_ON_MAX_MS    = 5UL * 60 * 1000;  // 5 phút ≈ 9 L
const unsigned long AUTO_ON_MAX_MS      = 3UL * 60 * 1000;  // AUTO bơm 3 phút...
const int           AUTO_MIN_RISE       = 5;                // ...mà ẩm tăng < 5 % → lỗi

const uint32_t      WDT_TIMEOUT_S       = 30;

enum PumpMode {
  AUTO       = 0,
  MANUAL_ON  = 1,
  MANUAL_OFF = 2
};

// Mã lỗi gửi lên Field 4
enum ErrorCode {
  ERR_NONE         = 0,
  ERR_SENSOR       = 1,  // ADC ngoài dải hợp lệ (rời dây, hỏng cảm biến)
  ERR_PUMP_NO_RISE = 2   // bơm chạy mà đất không ẩm lên (hết nước, tắc ống)
};

const char* MODE_NAMES[] = {"AUTO", "MANUAL_ON", "MANUAL_OFF"};

// ============================================================
//  TRẠNG THÁI
// ============================================================
WiFiClient    client;
bool          pumpRunning      = false;
PumpMode      pumpMode         = AUTO;
unsigned long lastSendTime     = 0;
unsigned long lastReadTime     = 0;
bool          sentOnce         = false;  // đã thử gửi lần đầu chưa
bool          firstWriteOk     = false;  // đã có lần writeFields thành công chưa
bool          forceSend        = false;  // gửi sớm khi chế độ thay đổi

unsigned long lastWifiAttempt  = 0;
unsigned long offlineSince     = 0;
bool          wasConnected     = false;

unsigned long manualOnSince    = 0;

bool          sensorFault      = false;
bool          pumpFault        = false;  // chốt cho tới khi có lệnh mới từ dashboard
bool          autoCheckArmed   = false;
unsigned long autoCheckSince   = 0;
int           autoCheckMoisture = 0;

unsigned long lastLoopMs       = 0;

// ============================================================
//  WIFI (không chặn)
// ============================================================
void startWiFi() {
  Serial.printf("\n[WiFi] Đang kết nối tới \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();

  // Chờ tối đa WIFI_SETUP_WAIT_MS, sau đó chạy tiếp dù chưa có mạng
  while (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt < WIFI_SETUP_WAIT_MS) {
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Đã kết nối! IP: %s\n", WiFi.localIP().toString().c_str());
    wasConnected = true;
  } else {
    Serial.println("\n[WiFi] Chưa có mạng — chạy offline, sẽ thử lại nền");
    offlineSince = millis();
  }
}

// Gọi mỗi vòng loop(): thử reconnect định kỳ, không bao giờ chờ
void maintainWiFi() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      Serial.printf("[WiFi] Đã kết nối lại! IP: %s (offline %lu ms)\n",
                    WiFi.localIP().toString().c_str(), now - offlineSince);
      wasConnected = true;
    }
    return;
  }

  if (wasConnected) {
    Serial.println("[WiFi] Mất kết nối!");
    wasConnected = false;
    offlineSince = now;
  }

  if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
    Serial.println("[WiFi] Thử kết nối lại...");
    WiFi.reconnect();
    lastWifiAttempt = now;
  }

  // Mất mạng quá lâu khi đang ở chế độ thủ công → về AUTO để vẫn tưới an toàn
  if (pumpMode != AUTO && now - offlineSince >= OFFLINE_FALLBACK_MS) {
    Serial.printf("[WiFi] Offline > %lu s → chuyển %s → AUTO\n",
                  OFFLINE_FALLBACK_MS / 1000, MODE_NAMES[pumpMode]);
    pumpMode = AUTO;
    autoCheckArmed = false;
    forceSend = true;
  }
}

// ============================================================
//  CẢM BIẾN
// ============================================================
// Trả về % độ ẩm, hoặc -1 nếu cảm biến lỗi
int readMoisturePercent() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(MOISTURE_PIN);
    delay(5);
  }
  int adcValue = sum / 10;

  bool fault = adcValue < ADC_WET - SENSOR_FAULT_MARGIN ||
               adcValue > ADC_DRY + SENSOR_FAULT_MARGIN;
  if (fault != sensorFault) {
    Serial.printf("[Sensor] %s (ADC %d)\n",
                  fault ? "LỖI cảm biến — ADC ngoài dải hợp lệ" : "Cảm biến hoạt động lại",
                  adcValue);
    sensorFault = fault;
  }
  if (fault) {
    Serial.printf("[Sensor] ADC raw: %d → LỖI\n", adcValue);
    return -1;
  }

  int percent = map(adcValue, ADC_DRY, ADC_WET, 0, 100);
  percent = constrain(percent, 0, 100);

  Serial.printf("[Sensor] ADC raw: %d → Độ ẩm: %d%%\n", adcValue, percent);
  return percent;
}

ErrorCode currentError() {
  if (sensorFault) return ERR_SENSOR;
  if (pumpFault)   return ERR_PUMP_NO_RISE;
  return ERR_NONE;
}

// ============================================================
//  LỆNH TỪ XA (ThingSpeak TalkBack)
// ============================================================
void applyMode(PumpMode newMode, const char* source) {
  // Có lệnh mới từ người dùng → xóa lỗi bơm đã chốt
  if (pumpFault) {
    Serial.println("[Pump] Xóa cờ lỗi bơm (có lệnh mới)");
    pumpFault = false;
  }

  if (newMode == MANUAL_ON) manualOnSince = millis();  // nhận lại ON → gia hạn 5 phút

  if (newMode != pumpMode) {
    Serial.printf("[%s] Đổi chế độ: %s → %s\n", source, MODE_NAMES[pumpMode], MODE_NAMES[newMode]);
    TLOG("[T] mode_change %s t=%lu\n", MODE_NAMES[newMode], millis());
    pumpMode = newMode;
    autoCheckArmed = false;
  } else {
    Serial.printf("[%s] Chế độ giữ nguyên: %s\n", source, MODE_NAMES[pumpMode]);
  }
  forceSend = true;  // báo trạng thái mới lên dashboard sớm nhất có thể
}

void readPumpCommand() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (TALKBACK_ID == 0) return;  // chưa cấu hình TalkBack

  // POST /talkbacks/<id>/commands/execute: trả lệnh kế tiếp và xóa nó khỏi hàng đợi;
  // hàng đợi rỗng → body rỗng
  String url = String("http://api.thingspeak.com/talkbacks/") + TALKBACK_ID + "/commands/execute";
  WiFiClient tbClient;
  HTTPClient http;
  http.setTimeout(5000);

  unsigned long t0 = millis();
  if (!http.begin(tbClient, url)) {
    Serial.println("[TalkBack] Không khởi tạo được HTTP");
    return;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int    httpCode = http.POST(String("api_key=") + TALKBACK_API_KEY);
  String body     = httpCode == 200 ? http.getString() : String();
  http.end();
  TLOG("[T] talkback_execute %lu ms (HTTP %d)\n", millis() - t0, httpCode);

  if (httpCode != 200) {
    Serial.printf("[TalkBack] Đọc lệnh thất bại: HTTP %d\n", httpCode);
    return;
  }

  body.trim();
  body.toUpperCase();
  if (body.length() == 0) return;  // không có lệnh mới

  if      (body == "AUTO") applyMode(AUTO, "TalkBack");
  else if (body == "ON")   applyMode(MANUAL_ON, "TalkBack");
  else if (body == "OFF")  applyMode(MANUAL_OFF, "TalkBack");
  else Serial.printf("[TalkBack] Lệnh không hợp lệ: \"%s\"\n", body.c_str());
}

// ============================================================
//  ĐIỀU KHIỂN BƠM
// ============================================================
void setPump(bool on, const char* reason) {
  if (on == pumpRunning) return;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  pumpRunning = on;
  Serial.printf("[Pump] %s (%s)\n", on ? "BẬT" : "TẮT", reason);
  TLOG("[T] pump_%s t=%lu\n", on ? "on" : "off", millis());
  if (!on) autoCheckArmed = false;
}

void controlPump(int moisturePercent) {
  unsigned long now = millis();

  // Giới hạn thời gian bật thủ công
  if (pumpMode == MANUAL_ON && now - manualOnSince >= MANUAL_ON_MAX_MS) {
    Serial.printf("[Pump] Hết %lu phút thủ công → về AUTO\n", MANUAL_ON_MAX_MS / 60000);
    pumpMode = AUTO;
    autoCheckArmed = false;
    forceSend = true;
  }

  if (pumpMode == MANUAL_ON) {
    setPump(true, "thủ công");
    return;
  }
  if (pumpMode == MANUAL_OFF) {
    setPump(false, "thủ công");
    return;
  }

  // ---- AUTO ----
  if (sensorFault) {
    setPump(false, "tự động — cảm biến lỗi");
    return;
  }
  if (pumpFault) {
    setPump(false, "tự động — lỗi bơm, chờ lệnh từ dashboard");
    return;
  }

  if (!pumpRunning && moisturePercent < MOISTURE_LOW) {
    char reason[96];
    snprintf(reason, sizeof(reason), "tự động — độ ẩm %d%% < ngưỡng %d%%", moisturePercent, MOISTURE_LOW);
    setPump(true, reason);
  } else if (pumpRunning && moisturePercent >= MOISTURE_HIGH) {
    char reason[96];
    snprintf(reason, sizeof(reason), "tự động — độ ẩm %d%% ≥ ngưỡng %d%%", moisturePercent, MOISTURE_HIGH);
    setPump(false, reason);
    return;
  }

  // Bơm đang chạy ở AUTO: mỗi AUTO_ON_MAX_MS độ ẩm phải tăng ≥ AUTO_MIN_RISE
  if (pumpRunning) {
    if (!autoCheckArmed) {
      autoCheckArmed    = true;
      autoCheckSince    = now;
      autoCheckMoisture = moisturePercent;
    } else if (now - autoCheckSince >= AUTO_ON_MAX_MS) {
      if (moisturePercent - autoCheckMoisture < AUTO_MIN_RISE) {
        Serial.printf("[Pump] LỖI: bơm %lu phút, độ ẩm %d%% → %d%% (tăng < %d%%) — hết nước/tắc ống?\n",
                      AUTO_ON_MAX_MS / 60000, autoCheckMoisture, moisturePercent, AUTO_MIN_RISE);
        pumpFault = true;
        forceSend = true;
        setPump(false, "tự động — bơm không hiệu quả");
      } else {
        autoCheckSince    = now;
        autoCheckMoisture = moisturePercent;
      }
    }
  }
}

// ============================================================
//  GỬI DỮ LIỆU
// ============================================================
void sendToThingSpeak(int moisturePercent) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ThingSpeak] Offline — bỏ qua lần gửi này");
    return;
  }

  ErrorCode err = currentError();

  // Cảm biến lỗi → không gửi Field 1 (để trống thay vì số sai)
  if (moisturePercent >= 0) ThingSpeak.setField(1, moisturePercent);
  ThingSpeak.setField(2, pumpRunning ? 1 : 0);
  ThingSpeak.setField(3, (int)pumpMode);
  ThingSpeak.setField(4, (int)err);

  unsigned long t0 = millis();
  int result = ThingSpeak.writeFields(CHANNEL_ID, WRITE_API_KEY);
  TLOG("[T] write_fields %lu ms (HTTP %d)\n", millis() - t0, result);

  if (result == 200) {
    Serial.printf("[ThingSpeak] Gửi thành công: ẩm=%d%%, bơm=%s, chế độ=%d, lỗi=%d\n",
                  moisturePercent, pumpRunning ? "BẬT" : "TẮT", (int)pumpMode, (int)err);
    if (!firstWriteOk) {
      firstWriteOk = true;
      TLOG("[T] first_write_ok t=%lu\n", millis());
    }
  } else {
    Serial.printf("[ThingSpeak] Lỗi: HTTP %d\n", result);
  }
}

// ============================================================
//  WATCHDOG
// ============================================================
void setupWatchdog() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Arduino-ESP32 3.x (ESP-IDF 5): cấu hình bằng struct; TWDT thường đã được core khởi tạo
  esp_task_wdt_config_t cfg = {
    .timeout_ms     = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  // Arduino-ESP32 2.x (ESP-IDF 4): (timeout giây, panic → reset)
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);  // theo dõi task loop()
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Relay khởi tạo LOW → bơm tắt an toàn sau mỗi lần reset
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  analogSetAttenuation(ADC_11db);
  analogSetWidth(12);

  startWiFi();
  ThingSpeak.begin(client);
  setupWatchdog();

  Serial.println("[System] Khởi động hoàn tất!");
  Serial.println("[System] Điều khiển từ xa qua ThingSpeak TalkBack: AUTO | ON | OFF");
  if (TALKBACK_ID == 0) Serial.println("[System] ⚠️ Chưa cấu hình TS_TALKBACK_ID trong secrets.h");
  Serial.println();
  TLOG("[T] setup_done t=%lu\n", millis());
}

void loop() {
  esp_task_wdt_reset();

  unsigned long now = millis();
  TLOG("[T] loop t=%lu dt=%lu\n", now, lastLoopMs ? now - lastLoopMs : 0);
  lastLoopMs = now;

  maintainWiFi();

  if (now - lastReadTime >= READ_INTERVAL_MS) {
    readPumpCommand();
    lastReadTime = now;
  }

  int moisture = readMoisturePercent();
  controlPump(moisture);

  bool online  = WiFi.status() == WL_CONNECTED;
  bool sendDue = (!sentOnce && online) ||
                 now - lastSendTime >= SEND_INTERVAL_MS ||
                 (forceSend && online && now - lastSendTime >= MIN_WRITE_GAP_MS);
  if (sendDue) {
    sendToThingSpeak(moisture);
    lastSendTime = now;
    if (online) {
      sentOnce  = true;
      forceSend = false;
    }
  }

  delay(1000);
}
