#ifndef AUDIO_H
#define AUDIO_H

void InitAudio();
void InitAY();
void GenerateAY(uint16_t* buffer, int num_samples, int cycles_per_sample);

#endif  // AUDIO_H
