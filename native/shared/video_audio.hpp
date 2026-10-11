// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Video audio through the sound mixer (sound_api.cpp): one stream slot plays the ring of the player
// that owns audio output. Render thread only: the mixer takes commands from one thread.
#pragma once

namespace hui::audio {
class StreamRing;
}

namespace video_audio {
// Opens the audio output if no sound did yet; false leaves video silent.
bool start();
// Plays `ring` from now on, or nothing (null): the mixer then stops reading the previous ring within a
// grain, but may read it until then.
void route(hui::audio::StreamRing* ring);
void set_gain(float gain);
} // namespace video_audio
