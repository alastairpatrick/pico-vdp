#ifndef AUDIO_H
#define AUDIO_H

void InitAudio();
void InitAY();
int GenerateAY(int volume);
void ChangeVolume(int delta);

#endif  // AUDIO_H
