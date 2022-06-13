#ifndef AUDIO_H
#define AUDIO_H

void InitAudio();
void InitAY();
void GenerateAY(uint16_t* buffer, int num_samples);

#endif  // AUDIO_H
