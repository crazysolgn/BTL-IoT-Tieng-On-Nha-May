/*
 * HỆ THỐNG IoT GIÁM SÁT VÀ LỌC TIẾNG ỒN TRONG NHÀ MÁY (AN TOÀN LAO ĐỘNG)
 * Phiên bản cho mạch ESP32-2432S028R ("Cheap Yellow Display" - CYD, màn hình 2.8" 320x240)
 * --------------------------------------------------------------------------------------
 * Cảm biến 1: Micro MAX4466  OUT -> GPIO35 (cổng P3, chân "35")   VCC -> 3V3 (cổng CN1)
 * Cảm biến 2: DHT11          DATA -> GPIO27 (cổng CN1, chân "27") VCC -> 3V3 (cổng CN1)
 * Có sẵn trên mạch:
 *   - Màn hình TFT 2.8" (SPI: SCK 14, MOSI 13, MISO 12, CS 15, DC 2, đèn nền 21)
 *   - LED RGB (R=GPIO4, G=GPIO16, B=GPIO17, tích cực mức THẤP)
 *   - Loa: cổng P4 (SPEAK), điều khiển bằng GPIO26 qua mạch khuếch đại
 *   - Nút BOOT (GPIO0): GIỮ 2 GIÂY để đặt lại liều ồn khi bắt đầu ca mới
 *
 * Xử lý: đo dB mỗi 200 ms -> lọc trung vị 5 mẫu -> lọc EMA (alpha 0.2)
 *        -> mỗi 10 s tính LAeq, Lmax, liều ồn (QCVN 24:2016/BYT: 85 dBA/8 h, quy tắc 3 dB, max 115 dBA)
 *        -> hiển thị màn hình, LED RGB, loa cảnh báo, gửi Firebase
 *
 * THƯ VIỆN (Arduino IDE > Library Manager):
 *   "Adafruit GFX Library", "Adafruit ILI9341", "Adafruit ST7735 and ST7789 Library",
 *   "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor"
 * Board: "ESP32 Dev Module"
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_ST7789.h>
#include <DHT.h>
#include <math.h>

// ============================ CẤU HÌNH ============================
#define SIM_MODE        1      // Wokwi: biến trở trên GPIO35 thay micro
#define CAL_MODE        0      // 1 = chế độ hiệu chuẩn micro (chỉ hiện Vpp, không gửi Firebase)

#define WIFI_SSID       "Wokwi-GUEST"
#define WIFI_PASSWORD   ""
#define FIREBASE_URL    "https://TEN-PROJECT-default-rtdb.asia-southeast1.firebasedatabase.app"  // không có "/" cuối
#define FIREBASE_AUTH   ""                      // để "" nếu Rules ở chế độ test
#define DEVICE_ID       "cyd-xuong-dap-01"

// --- Màn hình: CYD bản 1 cổng micro-USB thường là ILI9341;
//     bản 2 cổng (micro-USB + USB-C) thường là ST7789. Chạy test_cyd_phan_cung để biết chắc.
#define DISPLAY_DRIVER  1      // 1 = ILI9341, 2 = ST7789
#define INVERT_COLORS   0      // đổi 1 nếu màu bị đảo (nền đen thành trắng...)
#define ROTATION        1      // 1 hoặc 3: màn hình ngang; đổi nếu bị lộn ngược

// --- Chân (đã đúng cho CYD, không cần sửa) ---
#define SOUND_PIN       35
#define DHT_PIN         27
#define DHT_TYPE        DHT22       // Wokwi không có DHT11
#define LED_R           4
#define LED_G           16
#define LED_B           17
#define SPEAKER_PIN     26
#define BOOT_BTN        0
#define TFT_SCK 14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_CS 15
#define TFT_DC 2
#define TFT_BL 21
#define SOUND_ALARM     1      // 0 = tắt loa (chỉ dùng LED + màn hình)

// --- Hiệu chuẩn micro: xem hướng dẫn, làm với CAL_MODE = 1 ---
#define CAL_DB          85.0
#define CAL_VPP         0.50

// --- Thông số đo và ngưỡng ---
#define SAMPLE_MS       200
#define WINDOW_MS       50
#define SEND_MS         10000
#define MEDIAN_N        5
#define EMA_ALPHA       0.2
#define LIMIT_DB        85.0
#define WARN_DB         80.0
#define MAX_DB          115.0
#define EXCHANGE_DB     3.0
#define DOSE_THRESHOLD  80.0
#define TEMP_MIN 18.0
#define TEMP_MAX 32.0
#define HUMI_MIN 40.0
#define HUMI_MAX 80.0
// ==================================================================

SPIClass hspi(HSPI);
#if DISPLAY_DRIVER == 2
Adafruit_ST7789 tft(&hspi, TFT_CS, TFT_DC, -1);
#else
Adafruit_ILI9341 tft(&hspi, TFT_DC, TFT_CS, -1);
#endif
DHT dht(DHT_PIN, DHT_TYPE);

// Màu RGB565
#define C_BG     0x0000
#define C_TEXT   0xFFFF
#define C_GREY   0x8410
#define C_SAFE   0x07E0
#define C_WARN   0xFFE0
#define C_DANGER 0xF800
#define C_CRIT   0x8000
#define C_BLUE   0x5D1F
#define C_GRID   0x2104

float medBuf[MEDIAN_N]; int medIdx = 0, medCount = 0;
float ema = NAN;
double energySum = 0; int nSamples = 0;
float lmax = 0, lastRaw = 0, lastVpp = 0;
double dose = 0;
unsigned long lastSample = 0, lastSend = 0, lastWifiTry = 0, lastLive = 0, btnDown = 0;
String lastStatus = "";
bool doseAlerted = false, lastHeatBad = false;
#define TREND_N 60                                   // 60 điểm x 10 s = 10 phút gần nhất
float trend[TREND_N]; int trendCount = 0;
float lastLeq = NAN, lastT = NAN, lastH = NAN;

// =============================== WI-FI ===============================
void connectWiFi() {
  Serial.printf("Ket noi Wi-Fi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("\nChua ket noi Wi-Fi - van do va canh bao tai cho");
}

// =========================== ĐO VÀ LỌC ===========================
float readRawDb() {
#if SIM_MODE
  float base = 40.0 + analogRead(SOUND_PIN) * 70.0 / 4095.0;      // biến trở -> 40..110 dB
  float db = base + random(-30, 31) / 10.0;                        // dao động ±3 dB
  if (random(100) < 8) db += random(100, 251) / 10.0;              // 8 % xung va đập
  return db;
#else
  unsigned long start = millis();
  int vmax = 0, vmin = 4095;
  while (millis() - start < WINDOW_MS) {                           // biên độ đỉnh-đỉnh 50 ms
    int v = analogRead(SOUND_PIN);
    if (v > vmax) vmax = v;
    if (v < vmin) vmin = v;
  }
  float vpp = (vmax - vmin) * 3.3 / 4095.0;
  if (vpp < 0.001) vpp = 0.001;
  lastVpp = vpp;
  return CAL_DB + 20.0 * log10(vpp / CAL_VPP);
#endif
}

float medianFilter(float x) {
  medBuf[medIdx] = x; medIdx = (medIdx + 1) % MEDIAN_N;
  if (medCount < MEDIAN_N) medCount++;
  float t[MEDIAN_N];
  for (int i = 0; i < medCount; i++) t[i] = medBuf[i];
  for (int i = 1; i < medCount; i++) { float k = t[i]; int j = i - 1; while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; } t[j + 1] = k; }
  return t[medCount / 2];
}
float emaFilter(float x) { ema = isnan(ema) ? x : EMA_ALPHA * x + (1 - EMA_ALPHA) * ema; return ema; }

void addDose(float L, float dtSec) {
  if (L < DOSE_THRESHOLD) return;
  double allowedSec = 8.0 * 3600.0 / pow(2.0, (L - LIMIT_DB) / EXCHANGE_DB);
  dose += dtSec / allowedSec;
}

// =========================== CẢNH BÁO ===========================
String classify(float leq, float lmaxv) {
  if (lmaxv > MAX_DB) return "KHAN_CAP";
  if (leq >= LIMIT_DB) return "NGUY_HIEM";
  if (leq >= WARN_DB)  return "CANH_BAO";
  return "AN_TOAN";
}
uint16_t statusColor(const String& st) {
  if (st == "KHAN_CAP") return C_CRIT; if (st == "NGUY_HIEM") return C_DANGER;
  if (st == "CANH_BAO") return C_WARN; return C_SAFE;
}
const char* statusText(const String& st) {
  if (st == "KHAN_CAP") return "KHAN CAP";  if (st == "NGUY_HIEM") return "NGUY HIEM";
  if (st == "CANH_BAO") return "CANH BAO";  return "AN TOAN";
}
void setRgb(bool r, bool g, bool b) {          // LED RGB trên CYD tích cực mức THẤP
  digitalWrite(LED_R, r ? LOW : HIGH); digitalWrite(LED_G, g ? LOW : HIGH); digitalWrite(LED_B, b ? LOW : HIGH);
}
void setAlarm(const String& st) {
  if (st == "AN_TOAN")       setRgb(false, true, false);   // xanh lá
  else if (st == "CANH_BAO") setRgb(true, true, false);    // vàng (đỏ + xanh lá)
  else                       setRgb(true, false, false);   // đỏ
#if SOUND_ALARM
  if (st == "KHAN_CAP")       tone(SPEAKER_PIN, 2000, 1500);   // kêu dài
  else if (st == "NGUY_HIEM") tone(SPEAKER_PIN, 1500, 200);    // bíp ngắn mỗi 10 s
  else                        noTone(SPEAKER_PIN);
#endif
}

// =========================== MÀN HÌNH ===========================
// Bố cục 320x240: [0..39] thanh trạng thái | [40..129] số liệu | [130..239] đồ thị 10 phút
void drawStatic() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(6, 46);   tft.print("LAeq 10s (dBA)");
  tft.setCursor(170, 46); tft.print("Lmax (dBA)");
  tft.setCursor(170, 76); tft.print("Lieu on ca (%)");
  tft.setCursor(170, 106); tft.print("Nhiet do / Do am");
  tft.setCursor(6, 106);  tft.print("Do truc tiep:");
  tft.setCursor(6, 132);  tft.print("10 phut gan nhat  (- - 85 dBA)");
}
void drawStatus(const String& st) {
  uint16_t c = statusColor(st);
  tft.fillRect(0, 0, 320, 38, c);
  tft.setTextColor(st == "CANH_BAO" ? C_BG : C_TEXT); tft.setTextSize(3);
  tft.setCursor(8, 8); tft.print(statusText(st));
  tft.setTextSize(1); tft.setCursor(200, 6);
  tft.print(WiFi.status() == WL_CONNECTED ? "WiFi: OK" : "WiFi: mat");
  tft.setCursor(200, 20);
  if (st == "AN_TOAN") tft.print("Lam viec binh thuong");
  else if (st == "CANH_BAO") tft.print("Nen deo nut tai");
  else if (st == "NGUY_HIEM") tft.print("BAT BUOC bao ho tai");
  else tft.print("ROI KHU VUC NGAY");
}
void drawNumbers(float leq, float lmaxv, float t, float h) {
  tft.fillRect(0, 56, 165, 46, C_BG);
  tft.setTextSize(5); tft.setTextColor(statusColor(classify(leq, 0)));
  tft.setCursor(6, 58); tft.printf("%.1f", leq);
  tft.fillRect(170, 56, 150, 18, C_BG); tft.fillRect(170, 86, 150, 18, C_BG); tft.fillRect(170, 116, 150, 14, C_BG);
  tft.setTextSize(2); tft.setTextColor(lmaxv > MAX_DB ? C_DANGER : C_TEXT);
  tft.setCursor(170, 57); tft.printf("%.1f", lmaxv);
  tft.setTextColor(dose >= 1.0 ? C_DANGER : C_TEXT);
  tft.setCursor(170, 87); tft.printf("%.1f", dose * 100);
  bool heatBad = !isnan(t) && (t > TEMP_MAX || t < TEMP_MIN || h > HUMI_MAX || h < HUMI_MIN);
  tft.setTextColor(heatBad ? C_DANGER : C_TEXT);
  tft.setCursor(170, 117);
  if (isnan(t)) tft.print("-- C  -- %"); else tft.printf("%.0fC  %.0f%%", t, h);
}
void drawLive(float filtered) {
  tft.fillRect(84, 104, 80, 12, C_BG);
  tft.setTextSize(1); tft.setTextColor(C_BLUE); tft.setCursor(84, 106); tft.printf("%.1f dB", filtered);
}
void drawTrend() {
  const int x0 = 30, y0 = 142, w = 284, hgt = 92;           // khung đồ thị
  const float dbMin = 50, dbMax = 110;
  tft.fillRect(0, y0, 320, hgt + 6, C_BG);
  tft.setTextSize(1); tft.setTextColor(C_GREY);
  for (int d = 60; d <= 100; d += 20) {
    int y = y0 + hgt - (d - dbMin) * hgt / (dbMax - dbMin);
    tft.drawFastHLine(x0, y, w, C_GRID); tft.setCursor(4, y - 3); tft.print(d);
  }
  int y85 = y0 + hgt - (LIMIT_DB - dbMin) * hgt / (dbMax - dbMin);
  for (int x = x0; x < x0 + w; x += 8) tft.drawFastHLine(x, y85, 4, C_DANGER);
  tft.drawRect(x0, y0, w, hgt, C_GREY);
  for (int i = 1; i < trendCount; i++) {
    int xa = x0 + (i - 1) * (w - 1) / (TREND_N - 1), xb = x0 + i * (w - 1) / (TREND_N - 1);
    float a = constrain(trend[i - 1], dbMin, dbMax), b = constrain(trend[i], dbMin, dbMax);
    int ya = y0 + hgt - (a - dbMin) * hgt / (dbMax - dbMin), yb = y0 + hgt - (b - dbMin) * hgt / (dbMax - dbMin);
    uint16_t c = trend[i] >= LIMIT_DB ? C_DANGER : (trend[i] >= WARN_DB ? C_WARN : C_SAFE);
    tft.drawLine(xa, ya, xb, yb, c); tft.drawLine(xa, ya + 1, xb, yb + 1, c);
  }
}
void pushTrend(float v) {
  if (trendCount < TREND_N) trend[trendCount++] = v;
  else { for (int i = 1; i < TREND_N; i++) trend[i - 1] = trend[i]; trend[TREND_N - 1] = v; }
}

// =========================== FIREBASE ===========================
bool firebaseSend(const String& path, const String& method, const String& json) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String(FIREBASE_URL) + path + ".json";
  if (strlen(FIREBASE_AUTH) > 0) url += "?auth=" + String(FIREBASE_AUTH);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int code = (method == "PUT") ? http.PUT(json) : http.POST(json);
  http.end();
  Serial.printf("  %s %s -> HTTP %d\n", method.c_str(), path.c_str(), code);
  return code == 200;
}

// =========================== SETUP ===========================
void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setRgb(false, false, true);                                     // xanh dương = đang khởi động
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);            // bật đèn nền
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  dht.begin();
  randomSeed(esp_random());

  hspi.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
#if DISPLAY_DRIVER == 2
  tft.init(240, 320);
#else
  tft.begin();
#endif
  tft.setRotation(ROTATION);
  tft.invertDisplay(INVERT_COLORS);
  tft.fillScreen(C_BG);
  tft.setTextColor(C_TEXT); tft.setTextSize(2);
  tft.setCursor(10, 90);  tft.print("Giam sat tieng on");
  tft.setCursor(10, 115); tft.print("Dang ket noi Wi-Fi...");
  connectWiFi();
  drawStatic(); drawStatus("AN_TOAN"); drawTrend();
}

// =========================== LOOP ===========================
void loop() {
#if CAL_MODE
  // ---- HIỆU CHUẨN: đặt app đo dB sát micro, nguồn ồn ổn định; ghi Vpp TB và dB của app ----
  static unsigned long tc = 0; static float sumV = 0; static int nV = 0;
  readRawDb(); sumV += lastVpp; nV++;
  if (millis() - tc > 1000) {
    tc = millis();
    float v = sumV / nV, d = CAL_DB + 20.0 * log10(v / CAL_VPP);
    Serial.printf("Vpp TB = %.3f V | dB (theo CAL hien tai) = %.1f\n", v, d);
    tft.fillScreen(C_BG); tft.setTextColor(C_WARN); tft.setTextSize(2);
    tft.setCursor(10, 20); tft.print("CHE DO HIEU CHUAN");
    tft.setTextColor(C_TEXT); tft.setTextSize(3);
    tft.setCursor(10, 70);  tft.printf("Vpp: %.3f V", v);
    tft.setCursor(10, 120); tft.printf("dB : %.1f", d);
    tft.setTextSize(1); tft.setCursor(10, 180); tft.print("Ghi Vpp + so dB tren app -> CAL_VPP, CAL_DB");
    tft.setCursor(10, 195); tft.print("Vpp khong duoc cham ~3.2V (giam gain MAX4466)");
    sumV = 0; nV = 0;
  }
  return;
#endif

  // ---- Wi-Fi: thử lại mỗi 30 s, không chặn ----
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 30000) {
    lastWifiTry = millis(); WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // ---- Nút BOOT giữ 2 s: đặt lại liều ồn (ca mới) ----
  if (digitalRead(BOOT_BTN) == LOW) {
    if (btnDown == 0) btnDown = millis();
    if (millis() - btnDown > 2000) {
      dose = 0; doseAlerted = false; btnDown = 0;
      Serial.println("Da dat lai lieu on - bat dau ca moi");
      tft.fillRect(0, 0, 320, 38, C_BLUE); tft.setTextSize(2); tft.setTextColor(C_TEXT);
      tft.setCursor(8, 12); tft.print("DA DAT LAI CA MOI"); delay(800);
      drawStatus(lastStatus.length() ? lastStatus : "AN_TOAN");
    }
  } else btnDown = 0;

  // ---- Lấy mẫu + lọc mỗi 200 ms ----
  if (millis() - lastSample >= SAMPLE_MS) {
    float dt = (lastSample == 0) ? SAMPLE_MS / 1000.0 : min((millis() - lastSample) / 1000.0, 15.0);
    lastSample = millis();
    float raw = constrain(readRawDb(), 30.0, 130.0);
    float filtered = emaFilter(medianFilter(raw));
    lastRaw = raw;
    if (raw > lmax) lmax = raw;
    energySum += pow(10.0, filtered / 10.0);
    nSamples++;
    addDose(filtered, dt);
  }

  // ---- Hiện giá trị trực tiếp mỗi 1 s ----
  if (millis() - lastLive >= 1000 && !isnan(ema)) { lastLive = millis(); drawLive(ema); }

  // ---- Mỗi 10 s: tổng hợp, hiển thị, cảnh báo, gửi Firebase ----
  if (millis() - lastSend >= SEND_MS && nSamples > 0) {
    lastSend = millis();
    float leq = 10.0 * log10(energySum / nSamples);
    float t = dht.readTemperature(), h = dht.readHumidity();
    bool heatBad = !isnan(t) && (t > TEMP_MAX || t < TEMP_MIN || h > HUMI_MAX || h < HUMI_MIN);
    String st = classify(leq, lmax);
    lastLeq = leq; lastT = t; lastH = h;

    setAlarm(st);
    pushTrend(leq);
    drawStatus(st); drawNumbers(leq, lmax, t, h); drawTrend();

    Serial.printf("LAeq=%.1f | Lmax=%.1f | raw=%.1f | loc=%.1f | lieu=%.1f%% | T=%.1f H=%.0f | %s\n",
                  leq, lmax, lastRaw, ema, dose * 100, t, h, st.c_str());

    String json = String("{\"device\":\"") + DEVICE_ID + "\"" +
      ",\"leq\":"      + String(leq, 1) +
      ",\"lmax\":"     + String(lmax, 1) +
      ",\"raw\":"      + String(lastRaw, 1) +
      ",\"filtered\":" + String(ema, 1) +
      ",\"dose\":"     + String(dose * 100, 2) +
      ",\"temperature\":" + (isnan(t) ? "null" : String(t, 1)) +
      ",\"humidity\":"    + (isnan(h) ? "null" : String(h, 1)) +
      ",\"status\":\"" + st + "\",\"heatAlert\":" + (heatBad ? "true" : "false") +
      ",\"timestamp\":{\".sv\":\"timestamp\"}}";

    if (WiFi.status() == WL_CONNECTED) {
      firebaseSend("/factory/latest", "PUT", json);
      firebaseSend("/factory/history", "POST", json);
      if ((st == "NGUY_HIEM" || st == "KHAN_CAP") && st != lastStatus) {
        String msg = st == "KHAN_CAP" ? "Lmax vuot 115 dBA" : "LAeq vuot 85 dBA - bat buoc deo bao ho";
        firebaseSend("/factory/alerts", "POST", "{\"type\":\"" + st + "\",\"value\":" + String(st == "KHAN_CAP" ? lmax : leq, 1) +
                     ",\"message\":\"" + msg + "\",\"timestamp\":{\".sv\":\"timestamp\"}}");
      }
      if (dose >= 1.0 && !doseAlerted) {
        doseAlerted = true;
        firebaseSend("/factory/alerts", "POST", "{\"type\":\"LIEU_ON\",\"value\":100,\"message\":\"Lieu on dat 100% - can nghi/luan chuyen vi tri\",\"timestamp\":{\".sv\":\"timestamp\"}}");
      }
      if (heatBad && !lastHeatBad) {                     // vi khí hậu vừa vượt chuẩn QCVN 26:2016
        firebaseSend("/factory/alerts", "POST", "{\"type\":\"NHIET\",\"value\":" + String(t, 1) +
                     ",\"message\":\"Vi khi hau ngoai 18-32 C / 40-80 %\",\"timestamp\":{\".sv\":\"timestamp\"}}");
      }
    } else {
      Serial.println("  Mat Wi-Fi - bo qua gui Firebase");
    }
    lastStatus = st; lastHeatBad = heatBad;
    energySum = 0; nSamples = 0; lmax = 0;
  }
}
