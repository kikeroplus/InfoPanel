// パッシブブザーで鳴らす (env:buzzer)。音の並びを loop() の中で順に鳴らす (待たない)
#include "sound.h"
#if defined(BOARD_BUZZER)
#include <Arduino.h>

// ブザーは LEDC チャンネル 0 (タイマー 0)。バックライトは別のタイマーになる 4 (main.cpp)
#define BZ_CH 0

namespace // この中のものはこのファイルの中だけで使う
{

struct Note
{
  uint16_t freq, ms; // freq 0 は休み
};
const Note N_CLICK[] = {{3200, 12}};
const Note N_BOOT[] = {{1047, 60}, {1568, 60}, {2093, 100}};
const Note N_DONE[] = {{1568, 90}, {0, 30}, {2093, 160}};
const Note N_ALERT[] = {{2600, 150}, {0, 80}, {2600, 150}, {0, 80}, {2600, 150}};
const Note N_CALL[] = {{1760, 220}, {1397, 380}};
const Note N_VOL[] = {{1200, 40}, {0, 30}, {800, 60}};
#define SEQ(s) s, sizeof(s) / sizeof(s[0])

// 音量はデューティ比 (10 ビット、512 が 50% で最大) で変える
const uint16_t VOL_DUTY[VOL_N] = {512, 80, 20, 0};
int volLevel = VOL_HIGH;

const Note *seq = nullptr;
int seqN = 0, seqI = 0;
uint32_t seqAt = 0;

void startNote()
{
  if (seq[seqI].freq)
  {
    ledcWriteTone(BZ_CH, seq[seqI].freq);
    ledcWrite(BZ_CH, VOL_DUTY[volLevel]);
  }
  else
    ledcWrite(BZ_CH, 0);
  seqAt = millis();
}

void playSeq(const Note *s, int n)
{
  if (volLevel == VOL_OFF)
    return;
  seq = s;
  seqN = n;
  seqI = 0;
  startNote();
}

} // namespace

void soundBegin()
{
  ledcSetup(BZ_CH, 2000, 10);
  ledcAttachPin(BUZZER_PIN, BZ_CH);
  ledcWrite(BZ_CH, 0);
}

void soundSetVolume(int level)
{
  volLevel = level;
  if (level == VOL_OFF)
    soundStop();
}

void soundPlay(Sound s)
{
  switch (s)
  {
  case SND_CLICK: playSeq(SEQ(N_CLICK)); break;
  case SND_BOOT: playSeq(SEQ(N_BOOT)); break;
  case SND_DONE: playSeq(SEQ(N_DONE)); break;
  case SND_ALERT: playSeq(SEQ(N_ALERT)); break;
  case SND_CALL: playSeq(SEQ(N_CALL)); break;
  case SND_VOL: playSeq(SEQ(N_VOL)); break;
  case SND_N: break;
  }
}

void soundSay(const char *) {} // ブザーではしゃべれない

void soundStop()
{
  ledcWrite(BZ_CH, 0);
  seq = nullptr;
}

void soundUpdate()
{
  if (!seq || millis() - seqAt < seq[seqI].ms)
    return;
  if (++seqI < seqN)
    startNote();
  else
    soundStop();
}

#endif
