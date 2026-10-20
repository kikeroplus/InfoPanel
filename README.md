# InfoPanel

ESP32-S3 と 240x320 の液晶で作る情報表示端末。WorkingTimer とは別の端末で、タイマーより情報表示が主。

## 部品

- CPU: ESP32-S3-DevKitC-1 N16R8 (Flash 16MB / PSRAM 8MB Octal)
- 液晶: ST7789 240x320 SPI (動作確認は `..\TFT-VUMeterTester-240-320` で ESP32 を使って実施。IPS の色反転は false)

## 配線

| 液晶 | S3 の GPIO | 備考 |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SCL (SCK) | 12 | |
| SDA (MOSI) | 11 | |
| CS | 10 | |
| DC | 9 | |
| RST | 8 | |
| BL | 7 | |
| SDO (MISO) | 未接続 | |

- S3 には GPIO23 がないため、テスト時 (ESP32: SCK18 MOSI23 CS5 DC16 RST17 BL4) とはピンが違う。
- 使わないピン: 0・3・45・46 (起動モード)、19・20 (USB)、35〜37 (Octal PSRAM が使う)、43・44 (シリアル)。

## 書き込み

- 基板の USB 端子は 2 つある。書き込みとシリアルモニタは「COM」(UART) 側を使う。
- COM 番号が決まったら `platformio.ini` の `upload_port` を書く。
- WiFi の設定は `include/secrets.h` (`secrets.example.h` をコピーして記入、`.gitignore` 済み)。

## MQTT

トピックの仕様は WorkingTimer の `MQTT_TOPICS.md` が唯一の正。新しいトピックもそこに書き足す。
クライアント ID は `infopanel-XXXX` (MAC の末尾 4 桁)。
