/*
 * HỆ THỐNG IoT GIÁM SÁT VÀ LỌC TIẾNG ỒN TRONG NHÀ MÁY (AN TOÀN LAO ĐỘNG)
 * ----------------------------------------------------------------------
 * Phần cứng : ESP32 DevKit V1
 * Cảm biến 1: Micro khuếch đại MAX4466 (hoặc KY-038, chân AO) -> GPIO34 (ADC1_CH6)
 * Cảm biến 2: DHT11/DHT22 (nhiệt độ, độ ẩm - vi khí hậu)     -> GPIO15
 * Cảnh báo  : LED xanh GPIO25, LED vàng GPIO26, LED đỏ GPIO27, còi buzzer GPIO14
 * Nền tảng  : Firebase Realtime Database (REST API qua HTTPS)
 *
 * Xử lý tín hiệu:
 *   1. Đo mức âm (dB) mỗi 200 ms bằng phương pháp biên độ đỉnh-đỉnh trong cửa sổ 50 ms
 *   2. LỌC TRUNG VỊ 5 mẫu  -> loại xung nhiễu đột biến (va đập, nhiễu điện)
 *   3. LỌC EMA (alpha=0.2)  -> làm mượt, bỏ dao động nhanh
 *   4. Tính LAeq (mức âm tương đương) theo năng lượng, Lmax trong mỗi chu kỳ 10 s
 *   5. Tính LIỀU ỒN (%) theo QCVN 24:2016/BYT: 85 dBA/8 giờ, quy tắc 3 dB
 *
 * Ngưỡng cảnh báo (theo QCVN 24:2016/BYT):
 *   LAeq < 80 dBA       : AN TOÀN    (LED xanh)
 *   80 <= LAeq < 85 dBA : CẢNH BÁO   (LED vàng) - khuyến nghị đeo nút tai
 *   LAeq >= 85 dBA      : NGUY HIỂM  (LED đỏ + còi ngắt quãng) - bắt buộc bảo hộ thính giác
 *   Lmax > 115 dBA      : KHẨN CẤP   (LED đỏ + còi liên tục) - vượt mức tối đa cho phép
 * Vi khí hậu (QCVN 26:2016/BYT, lao động trung bình): 18-32 °C, độ ẩm 40-80 %
 *
 * PHIÊN BẢN CHẠY PHẦN CỨNG THẬT (SIM_MODE = 0).
 * Trình tự: (1) sửa Wi-Fi + FIREBASE_URL  (2) nạp với CAL_MODE = 1 để hiệu chuẩn micro
 *           (3) điền CAL_DB, CAL_VPP  (4) đặt CAL_MODE = 0 và nạp lại để chạy chính thức.
 * Thư viện: "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor"
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <math.h>

// ===================== CẤU HÌNH =====================
#define SIM_MODE        0                      // 0 = micro thật (1 = mô phỏng biến trở)
#define CAL_MODE        0                      // 1 = chế độ hiệu chuẩn: chỉ in Vpp/dB ra Serial, không gửi Firebase

#define WIFI_SSID       "TEN_WIFI_CUA_BAN"     // Wi-Fi 2.4 GHz (ESP32 không bắt được 5 GHz)
#define WIFI_PASSWORD   "MAT_KHAU_WIFI"
#define FIREBASE_URL    "https://TEN-PROJECT-default-rtdb.asia-southeast1.firebasedatabase.app"   // không có "/" ở cuối
#define FIREBASE_AUTH   ""                     // Database secret, để "" nếu Rules ở chế độ test
#define DEVICE_ID       "xuong-dap-01"         // tên vị trí đặt thiết bị

#define SOUND_PIN       34
#define DHT_PIN         15
#define DHT_TYPE        DHT11                  // đổi DHT22 nếu bạn mua DHT22
#define LED_GREEN       25
#define LED_YELLOW      26
#define LED_RED         27
#define BUZZER_PIN      14
#define BUZZER_ACTIVE   0                      // 0 = còi thụ động (passive, cần tone), 1 = còi chủ động (active, chỉ cần HIGH)

// Hiệu chuẩn micro thật: đặt điện thoại (app đo dB) cạnh micro ở nguồn ồn ổn định,
// ghi lại Vpp (in ra Serial) và mức dB của app -> điền vào 2 hằng dưới
#define CAL_DB          85.0                   // mức dB đo bằng thiết bị chuẩn
#define CAL_VPP         0.50                   // Vpp (V) ESP32 đọc được tại mức đó

#define SAMPLE_MS       200                    // chu kỳ lấy mẫu mức âm
#define WINDOW_MS       50                     // cửa sổ đo đỉnh-đỉnh
#define SEND_MS         10000                  // chu kỳ gửi Firebase
#define MEDIAN_N        5                      // số mẫu lọc trung vị
#define EMA_ALPHA       0.2                    // hệ số lọc EMA

#define LIMIT_DB        85.0                   // QCVN 24:2016: 85 dBA trong 8 giờ
#define WARN_DB         80.0
#define MAX_DB          115.0                  // mức tối đa không được vượt
#define EXCHANGE_DB     3.0                    // quy tắc 3 dB
#define DOSE_THRESHOLD  80.0                   // dưới mức này không tính vào liều
#define TEMP_MIN 18.0
#define TEMP_MAX 32.0
#define HUMI_MIN 40.0
#define HUMI_MAX 80.0
// ====================================================

DHT dht(DHT_PIN, DHT_TYPE);

float medBuf[MEDIAN_N]; int medIdx = 0, medCount = 0;
float ema = NAN;
double energySum = 0; int nSamples = 0;       // cộng dồn năng lượng để tính LAeq
float lmax = 0, lastRaw = 0;
double dose = 0;                               // liều ồn (1.0 = 100 %)
unsigned long lastSample = 0, lastSend = 0, lastWifiTry = 0;
String lastStatus = "";
bool doseAlerted = false;

// ---------- Wi-Fi ----------
// Chờ tối đa 15 s; nếu không được vẫn chạy tiếp để cảnh báo tại chỗ hoạt động khi mất mạng
void connectWiFi() {
  Serial.printf("Ket noi Wi-Fi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("\nChua ket noi duoc Wi-Fi - van do va canh bao tai cho, se thu lai sau");
}

// ---------- 1. Đo mức âm thô (dB) ----------
float lastVpp = 0;                             // Vpp gần nhất, dùng khi hiệu chuẩn
float readRawDb() {
#if SIM_MODE
  // Biến trở: 0..4095 -> 40..110 dB; cộng dao động ±3 dB và 8 % xác suất xung va đập +10..25 dB
  float base = 40.0 + analogRead(SOUND_PIN) * 70.0 / 4095.0;
  float db = base + random(-30, 31) / 10.0;
  if (random(100) < 8) db += random(100, 251) / 10.0;
  return db;
#else
  unsigned long start = millis();
  int vmax = 0, vmin = 4095;
  while (millis() - start < WINDOW_MS) {       // biên độ đỉnh-đỉnh trong 50 ms
    int v = analogRead(SOUND_PIN);
    if (v > vmax) vmax = v;
    if (v < vmin) vmin = v;
  }
  float vpp = (vmax - vmin) * 3.3 / 4095.0;
  if (vpp < 0.001) vpp = 0.001;
  lastVpp = vpp;
  return CAL_DB + 20.0 * log10(vpp / CAL_VPP); // dB = dB_chuẩn + 20 log10(Vpp / Vpp_chuẩn)
#endif
}

// ---------- 2. Lọc trung vị ----------
float medianFilter(float x) {
  medBuf[medIdx] = x; medIdx = (medIdx + 1) % MEDIAN_N;
  if (medCount < MEDIAN_N) medCount++;
  float t[MEDIAN_N];
  for (int i = 0; i < medCount; i++) t[i] = medBuf[i];
  for (int i = 1; i < medCount; i++) {         // sắp xếp chèn
    float k = t[i]; int j = i - 1;
    while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
    t[j + 1] = k;
  }
  return t[medCount / 2];
}

// ---------- 3. Lọc EMA ----------
float emaFilter(float x) {
  ema = isnan(ema) ? x : EMA_ALPHA * x + (1 - EMA_ALPHA) * ema;
  return ema;
}

// ---------- 5. Liều ồn: thời gian cho phép T(L) = 8h / 2^((L-85)/3) ----------
void addDose(float L, float dtSec) {
  if (L < DOSE_THRESHOLD) return;
  double allowedSec = 8.0 * 3600.0 / pow(2.0, (L - LIMIT_DB) / EXCHANGE_DB);
  dose += dtSec / allowedSec;
}

// ---------- Cảnh báo tại chỗ ----------
String classify(float leq, float lmaxv) {
  if (lmaxv > MAX_DB) return "KHAN_CAP";
  if (leq >= LIMIT_DB) return "NGUY_HIEM";
  if (leq >= WARN_DB)  return "CANH_BAO";
  return "AN_TOAN";
}
void setAlarm(const String& st) {
  digitalWrite(LED_GREEN,  st == "AN_TOAN");
  digitalWrite(LED_YELLOW, st == "CANH_BAO");
  digitalWrite(LED_RED,    st == "NGUY_HIEM" || st == "KHAN_CAP");
#if BUZZER_ACTIVE
  if (st == "KHAN_CAP")       digitalWrite(BUZZER_PIN, HIGH);                                    // kêu liên tục
  else if (st == "NGUY_HIEM") { digitalWrite(BUZZER_PIN, HIGH); delay(300); digitalWrite(BUZZER_PIN, LOW); }
  else                        digitalWrite(BUZZER_PIN, LOW);
#else
  if (st == "KHAN_CAP")       tone(BUZZER_PIN, 2000);        // còi liên tục
  else if (st == "NGUY_HIEM") tone(BUZZER_PIN, 1500, 300);   // tiếng bíp ngắn mỗi chu kỳ
  else                        noTone(BUZZER_PIN);
#endif
}

// ---------- Firebase REST ----------
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

void setup() {
  Serial.begin(115200);
  pinMode(LED_GREEN, OUTPUT); pinMode(LED_YELLOW, OUTPUT); pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  dht.begin();
  randomSeed(esp_random());
  connectWiFi();
}

void loop() {
#if CAL_MODE
  // ===== CHẾ ĐỘ HIỆU CHUẨN =====
  // Đặt điện thoại (app đo dB, ví dụ "Sound Meter") sát micro, bật một nguồn ồn ổn định (quạt, nhạc to, máy chạy).
  // Ghi lại Vpp trung bình bên dưới và số dB trên app -> điền CAL_VPP, CAL_DB rồi đặt CAL_MODE = 0.
  static unsigned long tc = 0; static float sumV = 0; static int nV = 0;
  readRawDb(); sumV += lastVpp; nV++;
  if (millis() - tc > 1000) {
    tc = millis();
    Serial.printf("Vpp TB = %.3f V  | dB (theo CAL hien tai) = %.1f | ADC hien tai = %d\n",
                  sumV / nV, CAL_DB + 20.0 * log10((sumV / nV) / CAL_VPP), analogRead(SOUND_PIN));
    sumV = 0; nV = 0;
  }
  return;
#endif
  // Thử kết nối lại Wi-Fi mỗi 30 s, không chặn vòng lặp đo
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 30000) {
    lastWifiTry = millis();
    WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // ----- Lấy mẫu và lọc mỗi 200 ms -----
  if (millis() - lastSample >= SAMPLE_MS) {
    // dt thực tế: lúc gửi HTTPS vòng lặp bị dừng 1-3 s, tính bù để liều ồn không bị thiếu
    float dt = (lastSample == 0) ? SAMPLE_MS / 1000.0 : min((millis() - lastSample) / 1000.0, 15.0);
    lastSample = millis();
    float raw = constrain(readRawDb(), 30.0, 130.0);
    float filtered = emaFilter(medianFilter(raw));
    lastRaw = raw;
    if (raw > lmax) lmax = raw;                          // Lmax lấy trên tín hiệu thô (không lọc mất đỉnh thật)
    energySum += pow(10.0, filtered / 10.0);             // cộng năng lượng âm
    nSamples++;
    addDose(filtered, dt);
  }

  // ----- Mỗi 10 s: tổng hợp, cảnh báo, gửi Firebase -----
  if (millis() - lastSend >= SEND_MS && nSamples > 0) {
    lastSend = millis();
    float leq = 10.0 * log10(energySum / nSamples);      // LAeq = 10 log10( (1/N) Σ 10^(L/10) )
    float t = dht.readTemperature(), h = dht.readHumidity();
    bool heatBad = !isnan(t) && (t > TEMP_MAX || t < TEMP_MIN || h > HUMI_MAX || h < HUMI_MIN);
    String st = classify(leq, lmax);
    setAlarm(st);

    Serial.printf("LAeq=%.1f dB | Lmax=%.1f | raw=%.1f | loc=%.1f | lieu=%.1f%% | T=%.1fC H=%.0f%% | %s\n",
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

    if (WiFi.status() != WL_CONNECTED) {               // mất mạng: vẫn cảnh báo tại chỗ, bỏ qua gửi
      Serial.println("  Mat Wi-Fi - bo qua gui Firebase");
      lastStatus = st; energySum = 0; nSamples = 0; lmax = 0;
      return;
    }
    firebaseSend("/factory/latest", "PUT", json);
    firebaseSend("/factory/history", "POST", json);

    // Ghi nhật ký khi chuyển sang trạng thái nguy hiểm / khẩn cấp
    if ((st == "NGUY_HIEM" || st == "KHAN_CAP") && st != lastStatus) {
      String msg = st == "KHAN_CAP" ? "Lmax vuot 115 dBA" : "LAeq vuot 85 dBA - bat buoc deo bao ho";
      firebaseSend("/factory/alerts", "POST", "{\"type\":\"" + st + "\",\"value\":" + String(st == "KHAN_CAP" ? lmax : leq, 1) +
                   ",\"message\":\"" + msg + "\",\"timestamp\":{\".sv\":\"timestamp\"}}");
    }
    if (dose >= 1.0 && !doseAlerted) {                    // lần đầu chạm 100 %
      doseAlerted = true;
      firebaseSend("/factory/alerts", "POST", "{\"type\":\"LIEU_ON\",\"value\":100,\"message\":\"Lieu on dat 100% - can nghi/luan chuyen vi tri\",\"timestamp\":{\".sv\":\"timestamp\"}}");
    }
    lastStatus = st;

    energySum = 0; nSamples = 0; lmax = 0;               // bắt đầu chu kỳ mới
  }
}
