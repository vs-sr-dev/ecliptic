// Audio output and the Sound Manager stream pump.
#pragma once
#include <cstdint>
#include <vector>

namespace audio {
void init();
void shutdown();
void pump();   // pulls ARM stream callbacks when the output queue runs low
void play_pcm(const std::vector<float>& lr, uint32_t rate, float vol);
}
