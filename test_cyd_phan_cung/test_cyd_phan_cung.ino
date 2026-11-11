/*
 * TEST PHẦN CỨNG MẠCH ESP32-2432S028R (CYD) - NẠP CHẠY THỬ TRƯỚC TIÊN
 * Kiểm tra lần lượt: màn hình (driver + màu), LED RGB, loa, DHT11 (GPIO27), micro (GPIO35)
 * Thư viện: Adafruit GFX, Adafruit ILI9341, Adafruit ST7735 and ST7789, DHT sensor library
 *
 * CÁCH ĐỌC KẾT QUẢ MÀN HÌNH:
 *   - Màn hình đen/trắng xóa/sọc nhiễu  -> đổi DISPLAY_DRIVER (1 <-> 2) rồi nạp lại
 *   - Hiện chữ nhưng 4 ô màu KHÔNG đúng thứ tự ĐỎ - XANH LÁ - XANH DƯƠNG - TRẮNG (vd đỏ thành xanh ngọc)
 *       -> đổi INVERT_COLORS (0 <-> 1)
 *   - Chữ bị lộn ngược -> đổi ROTATION (1 <-> 3)
 *   Ghi lại 3 giá trị đúng và điền giống hệt vào cyd_noise_monitor.ino
 */
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_ST7789.h>
#include <DHT.h>

#define DISPLAY_DRIVER 1      // 1 = ILI9341, 2 = ST7789
#define INVERT_COLORS  0
#define ROTATION       1

#define TFT_SCK 14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_CS 15
#define TFT_DC 2
#define TFT_BL 21
#define LED_R 4
#define LED_G 16
#define LED_B 17
#define SPEAKER_PIN 26
#define DHT_PIN 27
#define SOUND_PIN 35

SPIClass hspi(HSPI);
#if DISPLAY_DRIVER == 2
Adafruit_ST7789 tft(&hspi, TFT_CS, TFT_DC, -1);
#else
Adafruit_ILI9341 tft(&hspi, TFT_DC, TFT_CS, -1);
#endif
DHT dht(DHT_PIN, DHT11);

void rgb(bool r, bool g, bool b) { digitalWrite(LED_R, r ? LOW : HIGH); digitalWrite(LED_G, g ? LOW : HIGH); digitalWrite(LED_B, b ? LOW : HIGH); }

void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT); rgb(false, false, false);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  analogReadResolution(12); analogSetAttenuation(ADC_11db);
  dht.begin();
  hspi.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
#if DISPLAY_DRIVER == 2
  tft.init(240, 320);
#else
  tft.begin();
#endif
  tft.setRotation(ROTATION);
  tft.invertDisplay(INVERT_COLORS);

  // 1. Màn hình
  tft.fillScreen(0x0000);
  tft.setTextColor(0xFFFF); tft.setTextSize(2);
  tft.setCursor(8, 8);  tft.printf("Driver: %s", DISPLAY_DRIVER == 2 ? "ST7789" : "ILI9341");
  tft.setCursor(8, 30); tft.printf("Invert: %d  Rot: %d", INVERT_COLORS, ROTATION);
  tft.fillRect(8, 60, 70, 50, 0xF800);   tft.fillRect(86, 60, 70, 50, 0x07E0);
  tft.fillRect(164, 60, 70, 50, 0x001F); tft.fillRect(242, 60, 70, 50, 0xFFFF);
  tft.setTextSize(1);
  tft.setCursor(8, 115); tft.print("DO        XANH LA     XANH DUONG    TRANG");
  Serial.println("Man hinh: kiem tra thu tu 4 o mau DO - XANH LA - XANH DUONG - TRANG");

  // 2. LED RGB
  Serial.println("LED RGB: do -> xanh la -> xanh duong");
  rgb(true, false, false); delay(600); rgb(false, true, false); delay(600); rgb(false, false, true); delay(600); rgb(false, false, false);

  // 3. Loa (cắm loa 8 ohm vào cổng SPEAK/P4)
  Serial.println("Loa: 3 tieng bip");
  for (int i = 0; i < 3; i++) { tone(SPEAKER_PIN, 1500, 150); delay(300); }
}

void loop() {
  // 4. DHT11 + 5. micro (biên độ đỉnh-đỉnh trong 50 ms)
  float t = dht.readTemperature(), h = dht.readHumidity();
  int vmax = 0, vmin = 4095; unsigned long s = millis();
  while (millis() - s < 50) { int v = analogRead(SOUND_PIN); if (v > vmax) vmax = v; if (v < vmin) vmin = v; }
  float vpp = (vmax - vmin) * 3.3 / 4095.0;

  tft.fillRect(0, 135, 320, 105, 0x0000);
  tft.setTextSize(2); tft.setTextColor(0xFFE0);
  tft.setCursor(8, 140);
  if (isnan(t)) tft.print("DHT11: LOI (kiem tra IO27)"); else tft.printf("DHT11: %.1fC  %.0f%%", t, h);
  tft.setCursor(8, 170); tft.printf("Micro Vpp: %.3f V", vpp);
  tft.setCursor(8, 200); tft.printf("ADC: %d..%d", vmin, vmax);
  tft.setTextSize(1); tft.setTextColor(0x8410);
  tft.setCursor(8, 226); tft.print("Vo tay gan micro: Vpp phai tang. Im lang: Vpp nho.");
  Serial.printf("DHT11: %.1f C %.0f %% | Micro Vpp=%.3f V (ADC %d..%d)\n", t, h, vpp, vmin, vmax);
  delay(1500);
}
