#pragma once
// 機種ごとの違い (ピン・液晶・音) をここだけにまとめる。どちらの機種かは platformio.ini の env で決まる
//   env:buzzer  … 初号機。液晶に BL 端子あり、パッシブブザー、スイッチ 4 個
//   env:speaker … 新しい機体。液晶は TFT-Tester-ESP32S3-240-320-A のもの (BL なし・色反転)、MAX98357A + スピーカー、スイッチ 3 個
// 画面・MQTT・操作は共通 (main.cpp)。機種で分けるのは #if BOARD_SPEAKER のところだけにする
#if !defined(BOARD_BUZZER) && !defined(BOARD_SPEAKER)
#error "platformio.ini の build_flags で -DBOARD_BUZZER か -DBOARD_SPEAKER を指定する"
#endif

// ---- 液晶 (ST7789 240x320) ----
// GND = GND, VCC = 3.3V, SDO(MISO) は未接続。S3 の SPI2 (FSPI) の標準ピンに合わせてある (2 機種とも同じ)
#define TFT_SCK  12 // SCL
#define TFT_MOSI 11 // SDA
#define TFT_MISO -1
#define TFT_CS   10
#define TFT_DC   9
#define TFT_RST  8

// ---- スイッチ (片側 GND、押すと LOW)。-1 はスイッチなし ----
// GPIO13 は SPI2 の MISO の決まったピンなので、pinMode は液晶の begin のあとでする (main.cpp の setup)
#define SW1_PIN 13
#define SW2_PIN 14
#define SW3_PIN 1

#if defined(BOARD_BUZZER)
#define BOARD_NAME   "BUZZER"
#define TFT_BL       7     // BL 端子あり (LEDC で明るさを変える)
#define TFT_IPS      false // 色が反転していたため false
#define TFT_ROTATION 3     // 横置き 320x240。上下が逆なら 1
#define SW4_PIN      2     // SW4 は初号機だけ
#define BUZZER_PIN   6     // パッシブブザー (PWM で鳴らす)

#elif defined(BOARD_SPEAKER)
#define BOARD_NAME   "SPEAKER"
#define TFT_BL       -1    // BL 端子のない液晶 (つながなくても光る)。明るさは変えられない
#define TFT_IPS      true  // TFT-Tester-ESP32S3-240-320-A と同じく反転する。色が反転していたら false
#define TFT_ROTATION 3     // 横置き 320x240。上下が逆なら 1
#define SW4_PIN      -1

// アンプ (MAX98357A。I2S)。Vin = 5V, GND = GND, SD と GAIN は未接続 (左右を混ぜて鳴らす、9dB)
#define AMP_BCLK 15
#define AMP_LRC  16
#define AMP_DIN  17
#define AUDIO_RATE   24000 // 1 秒の標本数。声 (kanavoice) に合わせて 24kHz
#define TONE_VOLUME  0.08f // 音の大きさ (音量 HIGH のとき)。うるさければ下げる
#define VOICE_VOLUME 0.3f  // 声の大きさ (音量 HIGH のとき)。SpeechTest の AUDIO_VOLUME と同じ
#endif
