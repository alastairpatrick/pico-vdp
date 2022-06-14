#ifndef AUDIO_H
#define AUDIO_H

void InitAudio();
void InitAY();
void GenerateAY(uint16_t* buffer, int num_samples, int volume);
void ChangeVolume(int delta);

#endif  // AUDIO_H
