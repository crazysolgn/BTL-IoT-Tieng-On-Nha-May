/*
 * TEST RIÊNG CẢM BIẾN DHT11 VỚI ESP32
 * Nối dây: VCC -> 3V3, GND -> GND, DATA -> GPIO15 (D15)
 * Loại DHT11 rời 4 chân: thêm điện trở 10 kΩ giữa VCC và DATA
 * Serial Monitor: 115200 baud
 */
#include <DHT.h>
#define DHT_PIN  15
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

void setup() {
  Serial.begin(115200);
  dht.begin();
  Serial.println("Test DHT11...");
}

void loop() {
  delay(2000);                       // DHT11 đọc tối đa 1 lần/giây
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("Loi doc DHT11 - kiem tra day DATA (D15), nguon 3V3, dien tro 10k neu la loai 4 chan");
  } else {
    Serial.printf("Nhiet do: %.1f C | Do am: %.0f %%\n", t, h);
  }
}
