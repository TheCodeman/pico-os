/// @file audio.h
/// @brief PC stand-in for the HDMI sound (WAV editor tests).
#pragma once
#include <stdbool.h>
#include <stdint.h>
void audio_set_source(bool (*fn)(int32_t *left, int32_t *right));  ///< the source
bool (*audio_get_source(void))(int32_t *left, int32_t *right);    ///< the source now
