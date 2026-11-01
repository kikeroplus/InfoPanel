#pragma once
// 音を鳴らす部品。main.cpp は「何の音か」だけを渡し、どう鳴らすかは機種ごとのファイルに任せる
//   sound_buzzer.cpp  … パッシブブザー (PWM の矩形波)
//   sound_speaker.cpp … MAX98357A + スピーカー (チャイムの音を合成、声は kanavoice)
// 音を足すときは Sound に名前を足して、両方のファイルに鳴らし方を書く
#include "board.h"

enum Sound
{
  SND_CLICK, // スイッチ
  SND_BOOT,  // 起動
  SND_DONE,  // ジョブ完了
  SND_ALERT, // 切断・ComfyUI の異常
  SND_CALL,  // 呼び出しベル
  SND_VOL,   // 音量を変えたときの試し鳴らし
  SND_N
};

// 音量。OFF は何も鳴らさない
enum
{
  VOL_HIGH,
  VOL_MID,
  VOL_LOW,
  VOL_OFF,
  VOL_N
};

void soundBegin();
void soundSetVolume(int level);
void soundPlay(Sound s);
// かなの文をしゃべる。鳴っている音が終わってからしゃべる。声を出せない機種では何もしない
void soundSay(const char *kana);
void soundStop();
void soundUpdate(); // loop() から呼ぶ
