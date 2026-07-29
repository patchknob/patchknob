//----------------------------------------------------------------------------
//  PatchKnob — AudioClipPlayer implementation. See audio_clip_player.h
//  for the design, the RCU schedule-snapshot scheme, and the record model.
//----------------------------------------------------------------------------
#include "audio_clip_player.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#include "signalsmith-stretch.h"

namespace PatchKnob { namespace engine {

// Piecewise-linear inverse warp map: a TIMELINE (dst) sample -> its SOURCE sample.
static double warp_dst_to_src(const std::vector<WarpMarker>& m, double d) {
    if (m.size() < 2) return d;
    auto interp = [](double v, double a0, double a1, double b0, double b1) {
        return (a1 > a0) ? b0 + (v - a0) * (b1 - b0) / (a1 - a0) : b0;
    };
    if (d <= (double)m[1].dstSample)
        return interp(d, m[0].dstSample, m[1].dstSample, m[0].srcSample, m[1].srcSample);
    for (size_t i = 1; i + 1 < m.size(); ++i)
        if (d <= (double)m[i + 1].dstSample)
            return interp(d, m[i].dstSample, m[i + 1].dstSample, m[i].srcSample, m[i + 1].srcSample);
    const size_t k = m.size();
    return interp(d, m[k - 2].dstSample, m[k - 1].dstSample, m[k - 2].srcSample, m[k - 1].srcSample);
}

// Per-clip realtime-warp state.  One signalsmith instance per warped clip, fed
// contiguously as the play head advances; reset + re-primed on a discontinuity.
struct AudioClipPlayer::WarpState {
    std::mutex mtx;
    struct Entry {
        std::vector<WarpMarker> markers;
        std::unique_ptr<signalsmith::stretch::SignalsmithStretch<float>> st;
        int64_t readCursor = 0;    // next source sample to feed
        int64_t lastDstEnd = -1;   // dst-relative end of the previous block
    };
    std::map<const AudioClip*, Entry> entries;
    double sr = 48000.0;
};

AudioClipPlayer::AudioClipPlayer() {
    desc_.format       = PluginFormat::VST2;   // synthetic; not a real plugin
    desc_.name         = "Audio Clip Player";
    desc_.vendor       = "PatchKnob";
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
    if (clip->ch[1].size() != clip->ch[0].size()) return false;
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

bool AudioClipPlayer::setClipFades(int index, int64_t fadeInFrames, int64_t fadeOutFrames,
                                   float fadeInTension, float fadeOutTension) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    ScheduledClip& sc = edit_[index];
    const int64_t nf = sc.clip ? sc.clip->numFrames() : 0;
    if (fadeInFrames  < 0) fadeInFrames  = 0;
    if (fadeOutFrames < 0) fadeOutFrames = 0;
    int64_t regionFrames = sc.regionLength();
    if (regionFrames < 0) regionFrames = 0;
    if (fadeInFrames  > regionFrames) fadeInFrames  = regionFrames;
    if (fadeOutFrames > regionFrames) fadeOutFrames = regionFrames;
    sc.fadeInFrames   = fadeInFrames;
    sc.fadeOutFrames  = fadeOutFrames;
    sc.fadeInTension  = fadeInTension  < -1.f ? -1.f : (fadeInTension  > 1.f ? 1.f : fadeInTension);
    sc.fadeOutTension = fadeOutTension < -1.f ? -1.f : (fadeOutTension > 1.f ? 1.f : fadeOutTension);
    publishSchedule();
    return true;
}

bool AudioClipPlayer::setClipRegion(int index, int64_t startSample,
                                    int64_t sourceOffset, int64_t length) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    ScheduledClip& sc = edit_[index];
    const int64_t nf = sc.clip ? sc.clip->numFrames() : 0;
    if (startSample  < 0) startSample = 0;
    if (sourceOffset < 0) sourceOffset = 0;
    if (sourceOffset > nf) sourceOffset = nf;
    if (length < 0) length = 0;                          // 0 == to source end
    if (!sc.loop && length > 0 && sourceOffset + length > nf)
        length = nf - sourceOffset;
    sc.startSample  = startSample;
    sc.sourceOffset = sourceOffset;
    sc.length       = length;
    publishSchedule();
    return true;
}

bool AudioClipPlayer::setClipGain(int index, float gain) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    edit_[index].gain = gain;                   // Ardour: unconditional (neg = invert)
    publishSchedule();
    return true;
}

bool AudioClipPlayer::setClipMuted(int index, bool muted) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    edit_[index].muted = muted;
    publishSchedule();
    return true;
}

bool AudioClipPlayer::setClipLoop(int index, bool loop) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    edit_[index].loop = loop;
    publishSchedule();
    return true;
}

void AudioClipPlayer::setWarp(const AudioClip* clip, const std::vector<WarpMarker>& markers) {
    if (!clip) return;
    if (!warp_) warp_.reset(new WarpState());
    std::lock_guard<std::mutex> lk(warp_->mtx);
    warp_->sr = sampleRate_;
    if (markers.size() < 2) { warp_->entries.erase(clip); return; }
    WarpState::Entry& e = warp_->entries[clip];
    e.markers = markers;
    e.st.reset(new signalsmith::stretch::SignalsmithStretch<float>());
    e.st->presetDefault(2, (float)sampleRate_);
    e.readCursor = 0;
    e.lastDstEnd = -1;                      // force reset+prime on the next block
}

void AudioClipPlayer::clearWarp(const AudioClip* clip) {
    if (!warp_ || !clip) return;
    std::lock_guard<std::mutex> lk(warp_->mtx);
    warp_->entries.erase(clip);
}

// Realtime warp render for one region-block.  Returns true if this clip has a
// warp (handled here, so the caller skips the raw path); false = no warp.
bool AudioClipPlayer::renderWarped(const ScheduledClip& sc, int64_t winStart, int n,
                                   float* outL, float* outR) {
    if (!warp_ || !sc.clip) return false;
    std::unique_lock<std::mutex> lk(warp_->mtx, std::try_to_lock);
    if (!lk.owns_lock()) return false;               // being edited -> raw fallback
    std::map<const AudioClip*, WarpState::Entry>::iterator it = warp_->entries.find(sc.clip);
    if (it == warp_->entries.end()) return false;
    WarpState::Entry& e = it->second;
    if (e.markers.size() < 2 || !e.st) return false;

    const int64_t warpedLen = e.markers.back().dstSample;
    if (warpedLen <= 0) return true;
    const int64_t regStart = sc.startSample;
    const int64_t winEnd = winStart + n;
    const int64_t a = std::max(winStart, regStart);
    const int64_t b = std::min(winEnd, regStart + warpedLen);
    if (a >= b) return true;                          // no overlap this block

    const AudioClip* c = sc.clip;
    const int64_t nf = c->numFrames();
    const float* sl = c->ch[0].data();
    const float* sr = c->ch[1].empty() ? c->ch[0].data() : c->ch[1].data();
    const int64_t dstA = a - regStart, dstB = b - regStart;

    // discontinuity (first block / seek) -> reset the stretcher + prime lead-in.
    if (dstA != e.lastDstEnd) {
        e.st->reset();
        e.readCursor = (int64_t)std::floor(warp_dst_to_src(e.markers, (double)dstA));
        if (e.readCursor < 0) e.readCursor = 0;
        int lat = e.st->inputLatency();
        int64_t pStart = e.readCursor - lat;
        if (pStart < 0) { lat = (int)e.readCursor; pStart = 0; }
        if (lat > 0) {
            static thread_local std::vector<float> pl, pr;
            if ((int)pl.size() < lat) { pl.assign((size_t)lat, 0.f); pr.assign((size_t)lat, 0.f); }
            // readCursor can map past the source end (a stretch), so bounds-check
            // each prime index against nf instead of reading off the end.
            for (int i = 0; i < lat; ++i) {
                const int64_t si = pStart + i;
                const bool ok = si >= 0 && si < nf;
                pl[(size_t)i] = ok ? sl[si] : 0.f;
                pr[(size_t)i] = ok ? sr[si] : 0.f;
            }
            float* pin[2] = { pl.data(), pr.data() };
            e.st->seek(pin, lat, 1.0);
        }
    }

    int64_t srcEnd = (int64_t)std::floor(warp_dst_to_src(e.markers, (double)dstB));
    int inLen = (int)(srcEnd - e.readCursor);
    if (inLen < 0) inLen = 0;
    if (e.readCursor >= nf) inLen = 0;
    else if (e.readCursor + inLen > nf) inLen = (int)(nf - e.readCursor);
    const int outLen = (int)(b - a);

    static thread_local std::vector<float> ol, orr;
    if ((int)ol.size() < outLen) { ol.assign((size_t)outLen, 0.f); orr.assign((size_t)outLen, 0.f); }
    const int64_t rc = (e.readCursor < nf) ? e.readCursor : 0;
    float* in[2] = { const_cast<float*>(sl + rc), const_cast<float*>(sr + rc) };
    float* op[2] = { ol.data(), orr.data() };
    e.st->process(in, inLen, op, outLen);

    const float g = sc.gain;
    for (int i = 0; i < outLen; ++i) {
        outL[(a - winStart) + i] += ol[(size_t)i] * g;
        outR[(a - winStart) + i] += orr[(size_t)i] * g;
    }
    e.readCursor += inLen;
    e.lastDstEnd = dstB;
    return true;
}

void AudioClipPlayer::publishSchedule() {
    // Build the new snapshot into the next ring slot, then atomically flip.
    // More than two slots matters when the UI rapidly trims/moves/clones clips:
    // the audio thread may still be rendering an older snapshot while several
    // message-thread mutations are published in the same audio block.
    const int cur  = activeSlot_.load(std::memory_order_relaxed);
    const int next = (cur + 1) % kSnapshotSlots;
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
    if (!blk.audioOut || blk.numAudioOut < 1 || !blk.audioOut[0]) return;

    float* outL = blk.audioOut[0];
    float* outR = (blk.numAudioOut > 1 && blk.audioOut[1]) ? blk.audioOut[1] : blk.audioOut[0];

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
                if (!clip || sc.muted) continue;   // muted region: silent but kept

                // REALTIME WARP: if this clip has a live warp map, time-stretch it
                // here and skip the raw copy (so you hear warp edits as you drag).
                if (renderWarped(sc, blk.playPositionSamples, n, outL, outR)) continue;

                // Region occupies timeline [regStart, regStart+regLen) and plays
                // SOURCE frames [sourceOffset, sourceOffset+regLen).
                const int64_t regLen  = sc.regionLength();
                const int64_t regStart = sc.startSample;
                const int64_t regEnd   = regStart + regLen;

                // Overlap of the region with this block's window.
                const int64_t a = std::max(winStart, regStart);
                const int64_t b = std::min(winEnd,   regEnd);
                if (a >= b) continue;

                const float g   = sc.gain;
                const float* cl = clip->ch[0].data();
                const float* cr = clip->ch[1].data();
                const int64_t nf = clip->numFrames();
                const bool fades = sc.fadeInFrames > 0 || sc.fadeOutFrames > 0;
                for (int64_t s = a; s < b; ++s) {
                    const int     oi = (int)(s - winStart);
                    const int64_t ri = s - regStart;               // region-relative
                    int64_t ci = sc.sourceOffset + ri;             // source frame
                    if (sc.loop) {                                 // wrap the source window
                        const int64_t span = nf - sc.sourceOffset;
                        if (span > 0) ci = sc.sourceOffset + ((ri % span + span) % span);
                    }
                    if (ci < 0 || ci >= nf) continue;              // outside the source
                    // Fade env only where it matters (the unity middle stays g).
                    const float e = fades ? g * sc.fadeGain(ri, regLen) : g;
                    outL[oi] += cl[(size_t)ci] * e;
                    outR[oi] += cr[(size_t)ci] * e;
                }
            }
        }
    }

    // Record/monitor: when armed, capture the incoming audio for this block.
    if (recording_.load(std::memory_order_acquire) && blk.audioIn) {
        captureBlock(blk.audioIn, n);
    }
}

}} // namespace PatchKnob::engine
