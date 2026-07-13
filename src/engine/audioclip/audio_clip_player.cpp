//----------------------------------------------------------------------------
//  seq24 Windows port — AudioClipPlayer implementation. See audio_clip_player.h
//  for the design, the RCU schedule-snapshot scheme, and the record model.
//----------------------------------------------------------------------------
#include "audio_clip_player.h"

#include <algorithm>

namespace seq24 { namespace engine {

AudioClipPlayer::AudioClipPlayer() {
    desc_.format       = PluginFormat::VST2;   // synthetic; not a real plugin
    desc_.name         = "Audio Clip Player";
    desc_.vendor       = "seq24";
    desc_.path         = "";
    desc_.uid          = "builtin.audioclip";
    desc_.isInstrument = true;    // behaves as a Track instrument
    desc_.numAudioIn   = 2;       // accepts input for record/monitor
    desc_.numAudioOut  = 2;       // stereo out

    // Publish an initial empty snapshot so the audio thread always has a valid
    // pointer even before prepare()/any edit (same idiom as Track).
    snapStore_[0].count = 0;
    activeSlot_.store(0, std::memory_order_relaxed);
    liveSchedule_.store(&snapStore_[0], std::memory_order_release);
}

AudioClipPlayer::~AudioClipPlayer() = default;

// ---------------------------------------------------------------------------
// Schedule editing (message thread)
// ---------------------------------------------------------------------------

bool AudioClipPlayer::addClip(const AudioClip* clip, int64_t startSample, float gain) {
    if (!clip || clip->empty()) return false;
    if ((int)edit_.size() >= kMaxClips) return false;
    ScheduledClip sc;
    sc.clip        = clip;
    sc.startSample = startSample;
    sc.gain        = gain;
    edit_.push_back(sc);
    publishSchedule();
    return true;
}

bool AudioClipPlayer::removeClip(int index) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    edit_.erase(edit_.begin() + index);
    publishSchedule();
    return true;
}

void AudioClipPlayer::clearClips() {
    edit_.clear();
    publishSchedule();
}

void AudioClipPlayer::publishSchedule() {
    // Build the new snapshot into the inactive slot, then atomically flip.
    const int cur  = activeSlot_.load(std::memory_order_relaxed);
    const int next = cur ^ 1;
    ScheduleSnapshot& dst = snapStore_[next];

    const int count = std::min((int)edit_.size(), kMaxClips);
    for (int i = 0; i < count; ++i) dst.items[i] = edit_[i];
    dst.count = count;

    liveSchedule_.store(&dst, std::memory_order_release);
    activeSlot_.store(next, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Record mode
// ---------------------------------------------------------------------------

void AudioClipPlayer::startRecord(double maxSeconds) {
    if (maxSeconds < 0.0) maxSeconds = 0.0;
    recCapacity_ = (int64_t)(maxSeconds * sampleRate_ + 0.5);
    for (int c = 0; c < 2; ++c) {
        recCh_[c].clear();
        // Reserve so the audio thread's push_back never reallocates.
        recCh_[c].reserve((size_t)recCapacity_);
    }
    recFrames_.store(0, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
}

std::shared_ptr<AudioClip> AudioClipPlayer::stopRecord(const std::string& name) {
    // Disarm first so the audio thread stops appending. The host must ensure a
    // block boundary has passed (transport/record state is host-owned).
    recording_.store(false, std::memory_order_release);

    auto clip = std::make_shared<AudioClip>();
    clip->name             = name;
    clip->sampleRate       = sampleRate_;
    clip->sourceSampleRate = sampleRate_;

    const int64_t frames = recFrames_.load(std::memory_order_acquire);
    clip->ch[0].assign(recCh_[0].begin(), recCh_[0].begin() + (size_t)frames);
    clip->ch[1].assign(recCh_[1].begin(), recCh_[1].begin() + (size_t)frames);

    // Reset the capture buffer.
    for (int c = 0; c < 2; ++c) { recCh_[c].clear(); recCh_[c].shrink_to_fit(); }
    recCapacity_ = 0;
    recFrames_.store(0, std::memory_order_release);
    return clip;
}

void AudioClipPlayer::captureBlock(const float* const* in, int nframes) {
    if (nframes <= 0) return;
    if (!recording_.load(std::memory_order_acquire)) return;
    if (!in || !in[0]) return;

    // Bound the append to the pre-reserved capacity so we never reallocate on
    // the audio thread; drop anything past the reserve.
    int64_t have = (int64_t)recCh_[0].size();
    int room = (int)std::min<int64_t>(nframes, recCapacity_ - have);
    if (room <= 0) return;

    const float* l = in[0];
    const float* r = in[1] ? in[1] : in[0];   // mono feed -> both channels
    for (int i = 0; i < room; ++i) {
        recCh_[0].push_back(l[i]);   // within reserved capacity: no allocation
        recCh_[1].push_back(r[i]);
    }
    recFrames_.store((int64_t)recCh_[0].size(), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// IPluginInstance lifecycle
// ---------------------------------------------------------------------------

bool AudioClipPlayer::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate;
    maxBlock_   = maxBlockSize;
    return true;
}

void AudioClipPlayer::setActive(bool active) { active_ = active; }

void AudioClipPlayer::release() { active_ = false; }

// ---------------------------------------------------------------------------
// Realtime render
// ---------------------------------------------------------------------------

void AudioClipPlayer::process(const ProcessBlock& blk) {
    const int n = blk.nframes;
    if (n <= 0) return;

    float* outL = blk.audioOut[0];
    float* outR = blk.audioOut[1];

    // Always start from silence: as a well-behaved instrument we own our output.
    for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }

    // Mix scheduled clips only while the transport is rolling. One acquire-load
    // of the published snapshot for the whole block (RCU): never half-edited.
    if (blk.isPlaying) {
        ScheduleSnapshot* snap = liveSchedule_.load(std::memory_order_acquire);
        if (snap) {
            const int64_t winStart = blk.playPositionSamples;
            const int64_t winEnd   = winStart + n;
            for (int c = 0; c < snap->count; ++c) {
                const ScheduledClip& sc = snap->items[c];
                const AudioClip* clip = sc.clip;
                if (!clip) continue;

                const int64_t clipStart = sc.startSample;
                const int64_t clipEnd   = clipStart + clip->numFrames();

                // Overlap of the clip with this block's window.
                const int64_t a = std::max(winStart, clipStart);
                const int64_t b = std::min(winEnd,   clipEnd);
                if (a >= b) continue;

                const float g   = sc.gain;
                const float* cl = clip->ch[0].data();
                const float* cr = clip->ch[1].data();
                for (int64_t s = a; s < b; ++s) {
                    const int   oi = (int)(s - winStart);
                    const int64_t ci = s - clipStart;
                    outL[oi] += cl[(size_t)ci] * g;
                    outR[oi] += cr[(size_t)ci] * g;
                }
            }
        }
    }

    // Record/monitor: when armed, capture the incoming audio for this block.
    if (recording_.load(std::memory_order_acquire) && blk.audioIn) {
        captureBlock(blk.audioIn, n);
    }
}

}} // namespace seq24::engine
