//----------------------------------------------------------------------------
//  PatchKnob — AudioClipPlayer implementation. See audio_clip_player.h
//  for the design, the RCU schedule-snapshot scheme, and the record model.
//----------------------------------------------------------------------------
#include "audio_clip_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
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
        // Which region currently drives this cursor.  One warp map can back
        // several regions; without an owner they interleave into one stretcher.
        //
        // Keyed by the region's STABLE ID, never by its address.  The audio
        // thread sees regions inside a schedule SNAPSHOT, and publishSchedule()
        // writes each new snapshot into a DIFFERENT ring slot -- so an owner
        // held as `const ScheduledClip*` stopped matching after the very first
        // edit, the owner check rejected the region for ever, and the clip fell
        // back to raw playback (i.e. silence past the source for a stretched
        // region).  0 == unowned.
        uint64_t ownerId = 0;
        // How far readCursor RUNS AHEAD of the source position the next output
        // sample belongs to, in source frames.  A phase vocoder cannot emit the
        // output for source position X until it has been fed up to
        // X + inputLatency + rate*outputLatency; outputSeek() sets that pipeline
        // up in one go (see the discontinuity path in renderWarped), and this is
        // the lead it consumed.  0 until the first prime.
        int64_t leadIn = 0;
        // Per-entry scratch, sized on the message thread: the render used
        // function-local thread_local vectors that assign()ed on first use and
        // on every growth, i.e. allocated on the audio thread.
        std::vector<float> outL, outR;
        std::vector<float> primeL, primeR;

        // Largest source-frames-per-dst-frame ratio the prime scratch is sized
        // for.  A warp map can in principle ask for any ratio; the scratch has
        // to be a fixed message-thread allocation, so an absurd segment primes
        // with a clamped lead rather than allocating on the audio thread.
        static constexpr double kMaxPrimeRate = 8.0;

        // MESSAGE THREAD: (re)size the render + prime scratch.
        //
        // primeL/primeR must hold outputSeek()'s WHOLE input window, which is
        // outputSeekLength(rate) == inputLatency() + rate*outputLatency()
        // SOURCE frames -- far more than the old reset()+seek() path's bare
        // inputLatency().  `rate` comes from the steepest segment of this
        // clip's own warp map (capped), so a 1:1 map does not reserve for an
        // 8x one.  Call AFTER markers and st are set.
        void sizeScratch(int maxBlock) {
            const int nb = maxBlock > 0 ? maxBlock : 4096;
            outL.assign((size_t)nb, 0.f);
            outR.assign((size_t)nb, 0.f);

            double maxRate = 1.0;
            for (size_t i = 1; i < markers.size(); ++i) {
                const double dDst = (double)(markers[i].dstSample - markers[i - 1].dstSample);
                const double dSrc = (double)(markers[i].srcSample - markers[i - 1].srcSample);
                if (dDst > 0.0 && dSrc > 0.0) maxRate = std::max(maxRate, dSrc / dDst);
            }
            if (maxRate > kMaxPrimeRate) maxRate = kMaxPrimeRate;

            size_t need = 1;
            if (st) {
                const int sl = st->outputSeekLength((float)maxRate);
                if (sl > 0) need = (size_t)sl;
            }
            primeL.assign(need, 0.f);
            primeR.assign(need, 0.f);
        }
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

    // std::atomic has no default member initialiser here: zero the lock-free
    // warp registry before the audio thread can ever scan it.
    for (int i = 0; i < kMaxWarpedClips; ++i)
        warpedClips_[i].store(nullptr, std::memory_order_relaxed);
    warpedCount_.store(0, std::memory_order_relaxed);

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
    return addRegion(clip,startSample,gain)!=0;
}

uint64_t AudioClipPlayer::addRegion(const AudioClip* clip, int64_t startSample, float gain) {
    if (!clip || clip->empty()) return 0;
    if (clip->ch[1].size() != clip->ch[0].size()) return 0;
    if ((int)edit_.size() >= kMaxClips) return 0;
    ScheduledClip sc;
    sc.regionId    = nextRegionId_++;
    if(nextRegionId_==0)nextRegionId_=1;
    sc.clip        = clip;
    sc.startSample = startSample;
    sc.gain        = gain;
    edit_.push_back(sc);
    publishSchedule();
    return sc.regionId;
}

int AudioClipPlayer::regionIndex(uint64_t id) const {
    if(!id)return -1;
    for(int i=0;i<(int)edit_.size();++i)if(edit_[i].regionId==id)return i;
    return -1;
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

bool AudioClipPlayer::setSchedule(const std::vector<ScheduledClip>& regions) {
    std::vector<ScheduledClip> next;
    next.reserve(regions.size());
    for (const ScheduledClip& sc : regions) {
        if (!sc.clip || sc.clip->empty()) continue;
        if (sc.clip->ch[1].size() != sc.clip->ch[0].size()) continue;
        next.push_back(sc);
    }
    if ((int)next.size() > kMaxClips) return false;
    // Keep the caller's nonzero ids; mint fresh ones for the rest, and keep
    // nextRegionId_ strictly above everything now scheduled so future adds
    // can never collide with a preserved id.
    for (ScheduledClip& sc : next) {
        if (sc.regionId == 0) {
            sc.regionId = nextRegionId_++;
            if (nextRegionId_ == 0) nextRegionId_ = 1;
        } else if (sc.regionId >= nextRegionId_) {
            nextRegionId_ = sc.regionId + 1;
            if (nextRegionId_ == 0) nextRegionId_ = 1;
        }
    }
    edit_.swap(next);
    publishSchedule();
    return true;
}

bool AudioClipPlayer::setClipFades(int index, int64_t fadeInFrames, int64_t fadeOutFrames,
                                   float fadeInTension, float fadeOutTension) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    ScheduledClip& sc = edit_[index];
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

bool AudioClipPlayer::setClipFadeShapes(int index, int inShape, int outShape,
                                        int inSlope, int outSlope, int link) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    ScheduledClip& sc = edit_[index];
    auto clampShape = [](int s) { return (uint8_t)(s < 0 ? 0 : (s > 8 ? 8 : s)); };
    sc.fadeInShape  = clampShape(inShape);
    sc.fadeOutShape = clampShape(outShape);
    sc.fadeInSlope  = (uint8_t)(inSlope  == 1 ? 1 : 0);
    sc.fadeOutSlope = (uint8_t)(outSlope == 1 ? 1 : 0);
    if (link >= 0) sc.xfadeLink = (uint8_t)(link > 2 ? 2 : link);
    publishSchedule();
    return true;
}

void AudioClipPlayer::setAutoFadeFrames(int64_t frames) {
    if (frames < 0) frames = 0;
    autoFadeFrames_.store(frames, std::memory_order_release);
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
    // Do NOT clamp `length` into the source here.  regionLength() already
    // clamps dynamically, so the only thing this achieved was DISCARDING the
    // caller's requested length permanently: setting a long looped region
    // before its loop flag (as the project loader does) truncated it to the
    // source, and toggling loop off then on lost the length for good.
    (void)nf;
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
    ScheduledClip& sc = edit_[index];
    // Capture the loop PERIOD as looping is switched on.  Up to this point
    // `length` is the region's trimmed source span; from here on it is the
    // TIMELINE span the user drags out to fill, so the trim would otherwise be
    // lost and each pass would play the whole rest of the source instead of the
    // bar that was trimmed out of it.
    if (loop && !sc.loop) {
        const int64_t nf    = sc.clip ? sc.clip->safeFrames() : 0;
        const int64_t off   = sc.effectiveSourceOffset();
        const int64_t avail = nf - off > 0 ? nf - off : 0;
        sc.loopLength = (sc.length > 0 && sc.length < avail) ? sc.length : 0;
    }
    sc.loop = loop;
    publishSchedule();
    return true;
}

bool AudioClipPlayer::setClipLoopLength(int index, int64_t frames) {
    if (index < 0 || index >= (int)edit_.size()) return false;
    edit_[index].loopLength = frames > 0 ? frames : 0;   // 0 == to the source end
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
    // warp_dst_to_src() walks the map assuming both fields ascend; an unsorted
    // map silently produced negative spans and read cursors that ran backwards.
    std::sort(e.markers.begin(), e.markers.end(),
              [](const WarpMarker& a, const WarpMarker& b) {
                  return a.dstSample != b.dstSample ? a.dstSample < b.dstSample
                                                    : a.srcSample < b.srcSample;
              });
    e.st.reset(new signalsmith::stretch::SignalsmithStretch<float>());
    e.st->presetDefault(2, (float)sampleRate_);
    e.readCursor = 0;
    e.lastDstEnd = -1;                      // force reset+prime on the next block
    e.ownerId    = 0;
    e.leadIn     = 0;
    // Size the render scratch HERE (message thread).  maxBlock_ bounds any block
    // the host may hand us; outputSeekLength() bounds the prime buffer.  Sizing
    // it here is what keeps outputSeek() allocation-free on the audio thread.
    e.sizeScratch(maxBlock_);
    // WARM the stretcher's own internal pre-roll buffer on THIS thread.  A
    // freshly configure()d instance already sized tmpPreRollBuffer/
    // tmpProcessBuffer/stashedInput/stashedOutput, but do one throwaway
    // outputSeek() over silence anyway so every lazily-touched vector inside
    // the library has reached its final capacity before the audio thread ever
    // calls it.  (The following real block re-primes from lastDstEnd == -1.)
    {
        float* zin[2] = { e.primeL.data(), e.primeR.data() };
        const int warmLen = (int)e.primeL.size();
        if (warmLen > e.st->inputLatency()) e.st->outputSeek(zin, warmLen);
        e.st->reset();
    }
    publishWarpedClips();
}

void AudioClipPlayer::clearWarp(const AudioClip* clip) {
    if (!warp_) return;
    std::lock_guard<std::mutex> lk(warp_->mtx);
    // clearWarp(nullptr) is the "purge everything" call the project teardown
    // makes.  It used to bail out on the null check and clear NOTHING, so warp
    // entries -- keyed on a raw AudioClip* -- outlived the clips that owned
    // them, and the next clip allocated at that recycled address inherited a
    // dead clip's stretch (its own audio never played).
    if (clip) warp_->entries.erase(clip);
    else      warp_->entries.clear();
    publishWarpedClips();
}

// Message thread, called with warp_->mtx held: republish the lock-free list of
// warped clips the audio thread consults when it cannot take the mutex.
void AudioClipPlayer::publishWarpedClips() {
    int n = 0;
    if (warp_) {
        for (std::map<const AudioClip*, WarpState::Entry>::const_iterator it = warp_->entries.begin();
             it != warp_->entries.end() && n < kMaxWarpedClips; ++it) {
            if (it->second.markers.size() < 2) continue;
            warpedClips_[n++].store(it->first, std::memory_order_release);
        }
    }
    for (int i = n; i < kMaxWarpedClips; ++i)
        warpedClips_[i].store(nullptr, std::memory_order_release);
    warpedCount_.store(n, std::memory_order_release);
}

// Audio thread: is this clip warp-driven?  No lock, no allocation -- just a scan
// of the published pointer array.
bool AudioClipPlayer::clipIsWarped(const AudioClip* clip) const {
    if (!clip) return false;
    const int n = warpedCount_.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxWarpedClips; ++i)
        if (warpedClips_[i].load(std::memory_order_acquire) == clip) return true;
    return false;
}

// Audio thread: this clip's warp anchor, appended to the cache on first use.
// A short linear scan of the audio thread's OWN memory -- no lock, no atomics,
// and no allocation (the array is fixed at kMaxWarpedClips, the same bound the
// lock-free warp registry uses).
AudioClipPlayer::WarpAnchor* AudioClipPlayer::warpAnchor(const AudioClip* clip, bool create) {
    if (!clip) return nullptr;
    for (int i = 0; i < warpAnchorCount_; ++i)
        if (warpAnchors_[i].clip == clip) return &warpAnchors_[i];
    if (!create || warpAnchorCount_ >= kMaxWarpedClips) return nullptr;
    WarpAnchor* wa = &warpAnchors_[warpAnchorCount_++];
    wa->clip = clip;
    return wa;
}

// Audio thread, warp mutex NOT held: render one region-block from the last warp
// mapping renderWarped() recorded for this clip -- a linear continuation of the
// map segment it was in, read out of the source with linear interpolation.
// Timeline placement is the same as the stretched path (that is what the anchor
// pins down), the pitch is varispeed rather than stretched for as long as the
// message thread holds the map.  That is a far smaller artefact than the block
// of SILENCE this replaces, and it is not the raw path either: the region does
// not jump to a different point in the source.
bool AudioClipPlayer::renderWarpCached(const ScheduledClip& sc, int64_t winStart, int n,
                                       float* outL, float* outR, float chGain) {
    const WarpAnchor* wa = warpAnchor(sc.clip, false);
    if (!wa || wa->warpedLen <= 0) return false;   // never warped a block yet
    const AudioClip* c = sc.clip;
    const int64_t nf = c ? c->safeFrames() : 0;
    if (nf <= 0) return false;

    // Same extents the stretched path uses, so the fallback covers exactly the
    // frames the region would otherwise have filled.
    const int64_t regStart = sc.startSample;
    const int64_t regLen   = sc.loop ? sc.regionLength()
                                     : (sc.length > 0 ? sc.length : wa->warpedLen);
    const int64_t a = std::max(winStart, regStart);
    const int64_t b = std::min<int64_t>(winStart + n, regStart + regLen);
    if (a >= b) return true;                       // no overlap this block

    float g = sc.gain;
    if (!std::isfinite(g)) g = 0.f;
    const bool fades = sc.fadeInFrames > 0 || sc.fadeOutFrames > 0;
    const float* sl = c->ch[0].data();
    const float* sr = c->ch[1].empty() ? c->ch[0].data() : c->ch[1].data();
    const int64_t off = sc.effectiveSourceOffset();

    for (int64_t s = a; s < b; ++s) {
        const int64_t dstFull  = s - regStart;
        const int64_t localDst = sc.loop ? (dstFull % wa->warpedLen) : dstFull;
        const double  pos      = wa->srcAnchor
                               + (double)(localDst - wa->dstAnchor) * wa->ratio
                               + (double)off;
        const int64_t i0 = (int64_t)std::floor(pos);
        if (i0 < 0 || i0 >= nf) continue;          // outside the source
        const int64_t i1 = (i0 + 1 < nf) ? i0 + 1 : i0;
        const float   fr = (float)(pos - (double)i0);
        const int     oi = (int)(s - winStart);
        const float   e  = (fades ? g * sc.fadeGain(dstFull, regLen) : g) * chGain;
        outL[oi] += (sl[i0] + (sl[i1] - sl[i0]) * fr) * e;
        outR[oi] += (sr[i0] + (sr[i1] - sr[i0]) * fr) * e;
    }
    return true;
}

// Realtime warp render for one region-block.  Returns true if this clip has a
// warp (handled here, so the caller skips the raw path); false = no warp.
bool AudioClipPlayer::renderWarped(const ScheduledClip& sc, int64_t winStart, int n,
                                   float* outL, float* outR, float chGain) {
    if (!warp_ || !sc.clip) return false;
    // COMMON CASE: this clip has no warp map and never rendered one.  Answer
    // that from the lock-free registry + the audio thread's own anchor table
    // (the same two sources the contended path below trusts) instead of taking
    // the mutex: the try_lock was paid PER REGION PER BLOCK even in projects
    // that never touch warp.  No new race window: while setWarp() is inserting
    // an entry it holds the mutex, so in that instant the old code lost the
    // try_lock and consulted this same registry anyway.
    if (!clipIsWarped(sc.clip) && !warpAnchor(sc.clip, false)) return false;
    std::unique_lock<std::mutex> lk(warp_->mtx, std::try_to_lock);
    if (!lk.owns_lock()) {
        // The warp map is being edited.  Falling through to the RAW path here
        // swaps a stretched region for its unstretched source mid-block -- a
        // jump in pitch AND timing.  But reporting the region "handled" with
        // nothing written (what this used to do) is a whole block of SILENCE,
        // i.e. a dropout every time a marker is dragged while the transport
        // rolls.  Do neither: carry the region on from the last warp mapping we
        // saw, which keeps it where it belongs on the timeline, and let the
        // next block that wins the lock resync the stretcher (e.lastDstEnd no
        // longer matches, so it resets and re-primes at the exact dst position).
        if (renderWarpCached(sc, winStart, n, outL, outR, chGain)) return true;
        // Nothing cached: this clip has never rendered a warped block, so there
        // is no mapping to continue.  Hold as before rather than swap in raw.
        return clipIsWarped(sc.clip);
    }
    std::map<const AudioClip*, WarpState::Entry>::iterator it = warp_->entries.find(sc.clip);
    if (it == warp_->entries.end()) return false;
    WarpState::Entry& e = it->second;
    if (e.markers.size() < 2 || !e.st) return false;

    const int64_t warpedLen = e.markers.back().dstSample;
    // A degenerate map used to return true, which told the caller "handled" and
    // left the region SILENT.  Fall back to raw playback instead.
    if (warpedLen <= 0) return false;

    // The warp cursor is keyed by CLIP, but the same AudioClip can back several
    // regions.  Two regions sharing one clip both advanced e.readCursor in the
    // same block and each got a torn stream.  Only the region that owns the
    // cursor warps; the others fall back to raw rather than corrupt it.
    if (e.ownerId && e.ownerId != sc.regionId) return false;

    const int64_t regStart = sc.startSample;
    // Honour the region's own trim: warped playback used regStart+warpedLen and
    // ignored sc.length entirely, so a trimmed warped region played past its end.
    // A LOOPING region fills sc.regionLength() (which does NOT clamp to one
    // warp-map pass when sc.loop is set -- see AudioClip::regionLength) by
    // repeating the warp map every warpedLen destination samples; previously
    // regLen was unconditionally clamped to warpedLen, so a looped warped clip
    // played one pass and then fell silent for the rest of its length.
    // A NON-looping region that is longer than its map is not clamped to
    // warpedLen either: that clamp made `a >= b` below true for the whole tail,
    // which reports the region handled and renders NOTHING -- the region simply
    // went silent at the last marker.  warp_dst_to_src() extrapolates its final
    // segment past the last marker, so the tail carries on at the rate the map
    // ended with (and stops at the source end exactly as raw playback does).
    const int64_t regLen = sc.loop
        ? sc.regionLength()
        : (sc.length > 0 ? sc.length : warpedLen);
    const int64_t winEnd = winStart + n;
    const int64_t a = std::max(winStart, regStart);
    const int64_t b = std::min(winEnd, regStart + regLen);
    // Genuinely outside the region now: there is nothing to render on ANY path,
    // and the caller must not fall back to raw (that would play the region's
    // unwarped source past its warped end).
    if (a >= b) { e.ownerId = 0; return true; }       // no overlap this block
    e.ownerId = sc.regionId;

    const AudioClip* c = sc.clip;
    const int64_t nf = c->safeFrames();
    if (nf <= 0) return false;
    const float* sl = c->ch[0].data();
    const float* sr = c->ch[1].empty() ? c->ch[0].data() : c->ch[1].data();

    float g = sc.gain;
    if (!std::isfinite(g)) g = 0.f;
    // Warped regions got sc.gain only -- no fade envelope at all, so every
    // warped region edge was a hard cut regardless of its fade settings.
    const bool fades = sc.fadeInFrames > 0 || sc.fadeOutFrames > 0;

    // Render in sub-segments that never cross a loop-iteration boundary. Each
    // segment's warp-map lookup uses the LOCAL dst position (full region
    // position modulo warpedLen), so a block that spans a loop wrap (e.g.
    // position 4800 with warpedLen 5000 and 512 frames still to render) is
    // split at the wrap instead of asking warp_dst_to_src for a position past
    // the end of the map. Fade gain still uses the FULL (unwrapped) region
    // position, so a loop's fade envelope spans the whole scheduled region,
    // not each individual pass.
    int64_t segStart = a;
    while (segStart < b) {
        const int64_t dstAFull  = segStart - regStart;
        const int64_t localDstA = sc.loop ? (dstAFull % warpedLen) : dstAFull;
        const int64_t segEnd    = sc.loop
            ? std::min<int64_t>(b, segStart + (warpedLen - localDstA))
            : b;
        const int64_t localDstB = localDstA + (segEnd - segStart);
        // Both ends of this segment through the map, WITHOUT the region's source
        // offset: they drive the read cursor below and the anchor a contended
        // block falls back on (see renderWarpCached), which is shared by every
        // region using this clip and so must stay in the map's own coordinates.
        const double srcMapA = warp_dst_to_src(e.markers, (double)localDstA);
        const double srcMapB = warp_dst_to_src(e.markers, (double)localDstB);

        // discontinuity (first block / seek / loop wrap) -> restart the
        // stretcher AT this dst position.  At an exact loop wrap the previous
        // segment ended with lastDstEnd == warpedLen and this one starts at
        // localDstA == 0, so the mismatch below fires and correctly restarts
        // the stretcher at the top of the warp map for the new pass.
        //
        // The restart is outputSeek(), not reset()+seek().  seek() only
        // re-fills the INPUT history (inputLatency frames), so after a reset
        // the phase vocoder still owed outputLatency() frames of output before
        // anything real came through -- ~45 ms of SILENCE ramping back in at
        // the head of every pass of a transport loop, at every locate, and at
        // the start of playback.  outputSeek() consumes a window of
        // outputSeekLength(rate) source frames STARTING at the target source
        // position and pre-rolls the output pipeline so the very next
        // process() sample is that position (see SignalsmithStretch::exact for
        // the canonical call sequence).  primeL/primeR were sized for exactly
        // this window on the message thread (Entry::sizeScratch), and the
        // setWarp()/prepare() warm-up already ran one outputSeek() so every
        // buffer the library touches here is at final capacity: no allocation.
        if (localDstA != e.lastDstEnd) {
            // Local stretch rate (source frames per dst frame) at the restart
            // point, which outputSeek() infers from the window length.
            double rate = (localDstB > localDstA)
                        ? (srcMapB - srcMapA) / (double)(localDstB - localDstA)
                        : 1.0;
            if (!(rate > 0.0)) rate = 1.0;         // degenerate/backward segment
            // Warped playback ignored the region's start-offset, so slipping or
            // left-trimming a warped region changed nothing about what you heard.
            int64_t at = (int64_t)std::floor(srcMapA) + sc.effectiveSourceOffset();
            if (at < 0) at = 0;
            int seekLen = e.st->outputSeekLength((float)rate);
            // A map segment steeper than the scratch was sized for primes with
            // a clamped window rather than allocating on the audio thread.
            if (seekLen > (int)e.primeL.size()) seekLen = (int)e.primeL.size();
            if (seekLen > e.st->inputLatency()) {
                // The window can run past the source end (a stretch near the
                // tail): zero-pad instead of reading off the buffer.
                for (int i = 0; i < seekLen; ++i) {
                    const int64_t si = at + i;
                    const bool ok = si < nf;
                    e.primeL[(size_t)i] = ok ? sl[si] : 0.f;
                    e.primeR[(size_t)i] = ok ? sr[si] : 0.f;
                }
                float* pin[2] = { e.primeL.data(), e.primeR.data() };
                e.st->outputSeek(pin, seekLen);    // resets internally
                // outputSeek consumed [at, at+seekLen): the input stream
                // continues there, running `seekLen` frames AHEAD of the
                // source position the next output sample belongs to.  Record
                // that lead so the per-block feed below keeps inLen/outLen at
                // the map's local rate instead of starving the first blocks.
                e.readCursor = at + seekLen;
                e.leadIn     = seekLen;
            } else {
                // Scratch too small for a real prime (absurd map): the old
                // restart -- input history only, onset degraded but bounded.
                e.st->reset();
                e.readCursor = at;
                e.leadIn     = 0;
                int lat = e.st->inputLatency();
                int64_t pStart = at - lat;
                if (pStart < 0) { lat = (int)at; pStart = 0; }
                if (lat > 0 && (int)e.primeL.size() >= lat) {
                    for (int i = 0; i < lat; ++i) {
                        const int64_t si = pStart + i;
                        const bool ok = si >= 0 && si < nf;
                        e.primeL[(size_t)i] = ok ? sl[si] : 0.f;
                        e.primeR[(size_t)i] = ok ? sr[si] : 0.f;
                    }
                    float* pin[2] = { e.primeL.data(), e.primeR.data() };
                    e.st->seek(pin, lat, 1.0);
                }
            }
        }

        // The input stream stays `leadIn` frames ahead of the output's source
        // position (the pipeline outputSeek set up), so the target feed point
        // is the map position PLUS that lead: each block then feeds
        // ~rate*outLen frames, the ratio the vocoder stretches by.
        int64_t srcEnd = (int64_t)std::floor(srcMapB) + sc.effectiveSourceOffset()
                       + e.leadIn;
        int inLen = (int)(srcEnd - e.readCursor);
        if (inLen < 0) inLen = 0;
        if (e.readCursor >= nf) inLen = 0;
        else if (e.readCursor + inLen > nf) inLen = (int)(nf - e.readCursor);
        const int outLen = (int)(segEnd - segStart);

        // Pre-sized in prepare() to maxBlock_; this only grows if a host hands us a
        // block bigger than it promised, which must not happen on the audio thread.
        if ((int)e.outL.size() < outLen) { e.outL.assign((size_t)outLen, 0.f);
                                           e.outR.assign((size_t)outLen, 0.f); }
        const int64_t rc = (e.readCursor >= 0 && e.readCursor < nf) ? e.readCursor : 0;
        float* in[2] = { const_cast<float*>(sl + rc), const_cast<float*>(sr + rc) };
        float* op[2] = { e.outL.data(), e.outR.data() };
        e.st->process(in, inLen, op, outLen);

        for (int i = 0; i < outLen; ++i) {
            const float f = (fades ? g * sc.fadeGain(dstAFull + i, regLen) : g) * chGain;
            outL[(segStart - winStart) + i] += e.outL[(size_t)i] * f;
            outR[(segStart - winStart) + i] += e.outR[(size_t)i] * f;
        }
        e.readCursor += inLen;
        e.lastDstEnd = localDstB;

        // Remember where this segment left the map so a block that loses the
        // try_lock can carry on from here instead of going silent.
        if (WarpAnchor* wa = warpAnchor(sc.clip, true)) {
            wa->warpedLen = warpedLen;
            wa->dstAnchor = localDstB;
            wa->srcAnchor = srcMapB;
            wa->ratio     = (localDstB > localDstA)
                          ? (srcMapB - srcMapA) / (double)(localDstB - localDstA)
                          : wa->ratio;
        }
        segStart = segEnd;
    }
    return true;
}

void AudioClipPlayer::publishSchedule() {
    // Announce the publish BEFORE a slot is chosen, written or flipped.
    // acquireSnapshot() reads this counter either side of stamping the slot it
    // is about to walk: a counter bumped at the END of this function (or no
    // counter at all) would let the reader conclude "nobody raced me" while we
    // were in the middle of copying into the very slot it had just picked up.
    publishSeq_.fetch_add(1, std::memory_order_seq_cst);

    // Build the new snapshot into the next ring slot, then atomically flip.
    // More than two slots matters when the UI rapidly trims/moves/clones clips:
    // the audio thread may still be rendering an older snapshot while several
    // message-thread mutations are published in the same audio block.
    const int cur = activeSlot_.load(std::memory_order_relaxed);
    int target    = (cur + 1) % kSnapshotSlots;

    // Slots are RECYCLED, and nothing stopped the message thread from lapping a
    // reader: kSnapshotSlots rapid edits inside one audio block wrapped around
    // and rewrote the very snapshot process() was walking, so the audio thread
    // could see a clip's fields tear mid-block.  The reader stamps the slot it
    // is using; if we are about to overwrite that one, skip to the next.
    //
    // seq_cst here and on the reader's stamp: one of the two orders must win --
    // either we see its stamp and skip the slot, or it sees the fetch_add above
    // and re-stamps against the snapshot we are about to publish.  With plain
    // acquire/release both sides could miss each other (a store followed by a
    // load of another location is exactly what neither order constrains), which
    // is how a slot could be handed back to a reader that had already loaded
    // liveSchedule_ but had not yet stamped it.
    const ScheduleSnapshot* inUse = readingSnapshot_.load(std::memory_order_seq_cst);
    if (&snapStore_[target] == inUse)
        target = (target + 1) % kSnapshotSlots;
    ScheduleSnapshot& out = snapStore_[target];

    const int count = std::min((int)edit_.size(), kMaxClips);
    for (int i = 0; i < count; ++i) out.items[i] = edit_[i];
    out.count = count;

    liveSchedule_.store(&out, std::memory_order_release);
    activeSlot_.store(target, std::memory_order_relaxed);
}

// Audio thread: take the live snapshot and stamp the slot it lives in as IN
// USE, so publishSchedule() cannot recycle it while the block renders.
//
// The stamp has to go down BEFORE the pointer is trusted.  Stamping after the
// load (what process() used to do) left a window in which the publisher saw a
// null stamp for a reader that had ALREADY picked up the pointer and was about
// to walk it, and handed that very slot back to the message thread.  So: stamp,
// then check that no publish started across the stamp; if one did, take the
// snapshot it published and stamp again.  The check is on publishSeq_ and not
// on the pointer because the ring wraps -- kSnapshotSlots publishes bring the
// SAME pointer back, and a pointer compare would call that "unchanged".
AudioClipPlayer::ScheduleSnapshot* AudioClipPlayer::acquireSnapshot() {
    ScheduleSnapshot* snap = liveSchedule_.load(std::memory_order_acquire);
    // Bounded, so this stays wait-free.  The message thread is the only
    // publisher, so at most one publish is ever in flight and a retry costs one
    // more pass of three atomic ops; exhausting the loop needs a whole publish
    // (a copy of the entire schedule) to land inside each of those passes.
    for (int attempt = 0; ; ++attempt) {
        const uint64_t seq = publishSeq_.load(std::memory_order_seq_cst);
        readingSnapshot_.store(snap, std::memory_order_seq_cst);
        if (publishSeq_.load(std::memory_order_seq_cst) == seq) break;
        // Out of retries: keep the snapshot we have STAMPED rather than pick up
        // a fresher one we have not.  It is at worst a few edits stale -- which
        // is exactly what an RCU snapshot is allowed to be -- and it is the one
        // slot every subsequent publish now skips.
        if (attempt + 1 >= kSnapshotSlots) break;
        snap = liveSchedule_.load(std::memory_order_acquire);
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Record mode
// ---------------------------------------------------------------------------

// Message thread: do not touch the record storage until the audio thread has
// provably left any block that could still be writing it.  The engine's
// convention for this is patch_nodes' retirement rule -- a parked buffer is
// freed only once the block in flight at the swap has exited AND a full block
// has since started (two generations).  captureBlock() additionally publishes
// `capBusy_` around its writes, so the common case resolves immediately instead
// of waiting out two block periods, and a stopped audio thread (rtGen_ never
// advances -- headless tests, an idle device) cannot stall the UI: the wait is
// bounded and the handshake alone is already sufficient.
void AudioClipPlayer::awaitRecordGrace() {
    // seq_cst: pairs with captureBlock's store/load of the same two flags.  One
    // of the two orders must win -- either we see capBusy_ and wait it out, or
    // the audio thread sees recording_ == false and never touches the storage.
    recording_.store(false, std::memory_order_seq_cst);
    const uint64_t target = rtGen_.load(std::memory_order_acquire) + 2;
    for (int i = 0; i < 40; ++i) {
        const bool busy = capBusy_.load(std::memory_order_seq_cst);
        const bool settled = rtGen_.load(std::memory_order_acquire) >= target;
        if (!busy && (settled || i >= 20)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void AudioClipPlayer::startRecord(double maxSeconds) {
    // Re-arming while already armed used to clear() the buffers the audio
    // thread was still push_back-ing into.  Disarm first and WAIT OUT any block
    // that is still capturing before the storage is resized.
    awaitRecordGrace();

    if (maxSeconds < 0.0) maxSeconds = 0.0;
    int64_t cap = (int64_t)(maxSeconds * sampleRate_ + 0.5);
    if (cap < 0) cap = 0;
    // FULLY SIZE the buffers rather than reserve()+push_back.  push_back mutates
    // vector::size(), which captureBlock() then READ to find its write position
    // -- a torn read against the message thread, and a data race by any
    // definition.  Now the storage is fixed and recFrames_ alone is the cursor.
    for (int c = 0; c < 2; ++c) recCh_[c].assign((size_t)cap, 0.0f);
    recCapacity_.store(cap, std::memory_order_release);
    recFrames_.store(0, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
}

std::shared_ptr<AudioClip> AudioClipPlayer::stopRecord(const std::string& name) {
    // Disarm and wait the audio thread out: the buffers are FREED below, and a
    // block that had already passed the `recording_` check was still writing
    // into them.  "The host must ensure a block boundary has passed" was never
    // anything the host could actually guarantee.
    awaitRecordGrace();

    auto clip = std::make_shared<AudioClip>();
    clip->name             = name;
    clip->sampleRate       = sampleRate_;
    clip->sourceSampleRate = sampleRate_;

    int64_t frames = recFrames_.load(std::memory_order_acquire);
    // Never trust the cursor past the storage: a capacity change racing a
    // late in-flight block could otherwise walk begin()+frames off the end.
    const int64_t have = (int64_t)std::min(recCh_[0].size(), recCh_[1].size());
    if (frames < 0)    frames = 0;
    if (frames > have) frames = have;
    clip->ch[0].assign(recCh_[0].begin(), recCh_[0].begin() + (size_t)frames);
    clip->ch[1].assign(recCh_[1].begin(), recCh_[1].begin() + (size_t)frames);

    // Reset the capture buffer.
    for (int c = 0; c < 2; ++c) { recCh_[c].clear(); recCh_[c].shrink_to_fit(); }
    recCapacity_.store(0, std::memory_order_release);
    recFrames_.store(0, std::memory_order_release);
    return clip;
}

void AudioClipPlayer::captureBlock(const float* const* in, int nframes, int numChannels) {
    if (nframes <= 0) return;
    if (!in || !in[0]) return;

    // Announce the capture BEFORE reading `recording_` (and with seq_cst on
    // both), so awaitRecordGrace() can never miss an in-flight writer: either it
    // sees capBusy_ and waits, or we see the disarm and touch nothing.  Two
    // atomic stores, no lock and no allocation -- audio-thread safe.
    capBusy_.store(true, std::memory_order_seq_cst);
    if (!recording_.load(std::memory_order_seq_cst)) {
        capBusy_.store(false, std::memory_order_release);
        return;
    }
    struct BusyGuard {
        std::atomic<bool>& f;
        ~BusyGuard() { f.store(false, std::memory_order_release); }
    } guard{ capBusy_ };

    // Write by INDEX into pre-sized storage: no push_back, so vector::size() is
    // never mutated on the audio thread and never read across threads.
    const int64_t pos = recFrames_.load(std::memory_order_relaxed);
    const int64_t cap = recCapacity_.load(std::memory_order_acquire);
    const int64_t lim = std::min<int64_t>(cap,
                            (int64_t)std::min(recCh_[0].size(), recCh_[1].size()));
    int room = (int)std::min<int64_t>(nframes, lim - pos);
    if (room <= 0) return;

    // in[1] was dereferenced unconditionally.  process() hands us blk.audioIn
    // straight through, so on a MONO input bus that read one pointer PAST the
    // end of the host's channel array.
    const float* l = in[0];
    const float* r = (numChannels > 1 && in[1]) ? in[1] : in[0];
    for (int i = 0; i < room; ++i) {
        recCh_[0][(size_t)(pos + i)] = l[i];
        recCh_[1][(size_t)(pos + i)] = r[i];
    }
    recFrames_.store(pos + room, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// IPluginInstance lifecycle
// ---------------------------------------------------------------------------

bool AudioClipPlayer::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_   = maxBlockSize > 0 ? maxBlockSize : 0;
    // ~2 ms, the same window the sampler's declick uses: long enough to kill the
    // step, short enough to leave a transient sounding like a transient.
    declickFrames_ = (int64_t)(0.002 * sampleRate_ + 0.5);
    wasPlaying_ = false; tailArmed_ = false; tailPos_ = 0; tailDone_ = 0;

    // Existing warp entries were built with presetDefault() at the OLD rate and
    // their scratch sized to the OLD block, so a device/rate change left every
    // warped clip stretching against a stale rate.  Rebuild them here.
    if (warp_) {
        std::lock_guard<std::mutex> lk(warp_->mtx);
        warp_->sr = sampleRate_;
        for (auto& kv : warp_->entries) {
            WarpState::Entry& e = kv.second;
            if (e.markers.size() < 2) continue;
            e.st.reset(new signalsmith::stretch::SignalsmithStretch<float>());
            e.st->presetDefault(2, (float)sampleRate_);
            e.readCursor = 0;
            e.lastDstEnd = -1;
            e.ownerId    = 0;
            e.leadIn     = 0;
            // Size for outputSeek's whole window (not just inputLatency: the
            // audio thread's discontinuity restart primes with outputSeek and
            // must never find the scratch short), and warm the library's
            // lazily-sized internals on THIS thread, exactly as setWarp does.
            e.sizeScratch(maxBlock_);
            float* zin[2] = { e.primeL.data(), e.primeR.data() };
            const int warmLen = (int)e.primeL.size();
            if (warmLen > e.st->inputLatency()) e.st->outputSeek(zin, warmLen);
            e.st->reset();
        }
        publishWarpedClips();
    }
    return true;
}

void AudioClipPlayer::setActive(bool active) { active_ = active; }

void AudioClipPlayer::release() { active_ = false; }

// ---------------------------------------------------------------------------
// Realtime render
// ---------------------------------------------------------------------------

void AudioClipPlayer::mixWindow(ScheduleSnapshot* snap, int64_t winStart, int n,
                                float* outL, float* outR, float chGain) {
    if (!snap) return;
    const int64_t winEnd = winStart + n;
    for (int c = 0; c < snap->count; ++c) {
        const ScheduledClip& sc = snap->items[c];
        const AudioClip* clip = sc.clip;
        if (!clip || sc.muted) continue;   // muted region: silent but kept

        // A clip is message-thread owned and NON-OWNING here, so it can
        // be edited after it was scheduled.  addClip() validates the
        // channels at insert time, but that says nothing about now: a
        // clip left with an empty or short ch[1] gave cr = nullptr (or a
        // short buffer) and the loop below read straight off the end.
        const int64_t nf = clip->safeFrames();
        if (nf <= 0) continue;

        // REALTIME WARP: if this clip has a live warp map, time-stretch it
        // here and skip the raw copy (so you hear warp edits as you drag).
        if (renderWarped(sc, winStart, n, outL, outR, chGain)) continue;

        // Region occupies timeline [regStart, regStart+regLen) and plays
        // SOURCE frames [sourceOffset, sourceOffset+regLen).
        const int64_t regLen  = sc.regionLength();
        const int64_t regStart = sc.startSample;
        const int64_t regEnd   = regStart + regLen;

        // Overlap of the region with this block's window.
        const int64_t a = std::max(winStart, regStart);
        const int64_t b = std::min(winEnd,   regEnd);
        if (a >= b) continue;

        float g = sc.gain;
        if (!std::isfinite(g)) continue;   // a NaN gain poisons the whole mix
        const float* cl = clip->ch[0].data();
        const float* cr = clip->ch[1].data();
        // Read through the SAME clamped offset regionLength() used.
        const int64_t off = sc.effectiveSourceOffset();
        const bool fades = sc.fadeInFrames > 0 || sc.fadeOutFrames > 0;

        // ---- DECLICK -------------------------------------------------------
        // Nothing here used to ramp anything: a trimmed region started and
        // ended on a raw step, and a looped one spliced source frame nf-1
        // straight back to `off` every pass.  Both now get the sampler's
        // smoothstep window (~2 ms).  A loop wrap is a true CROSSFADE whenever
        // the source has material in front of the loop start to fade in from
        // (which is the continuous signal the loop point interrupts); when it
        // has none -- a whole-file loop -- it falls back to ramping out and
        // back in across the splice.
        const int64_t declick   = declickFrames_;
        const int64_t loopSpan  = sc.loop ? sc.loopSpan() : 0;
        const int64_t wrapWin   = sc.loop ? ScheduledClip::declickWindow(declick, loopSpan) : 0;
        const bool    wrapXfade = wrapWin > 0 && off - wrapWin >= 0;
        // AutoFades (PT p752): the user preference widens the edge window past
        // the fixed declick.  Only region EDGES -- loop-wrap splices keep the
        // short declick, and edges that carry a real fade are skipped below.
        const int64_t autoF = autoFadeFrames_.load(std::memory_order_acquire);
        // Hoisted out of the per-sample loop (it is a division), and the loop
        // position is carried incrementally rather than re-taking ri % span for
        // every frame.  Both match ScheduledClip::edgeDeclickGain() exactly --
        // that is the helper the self-test builds its reference from.
        const int64_t edgeWin = ScheduledClip::declickWindow(
            autoF > declick ? autoF : declick, regLen);
        const bool    edgeIn  = edgeWin > 0 && sc.fadeInFrames  <= 0;
        const bool    edgeOut = edgeWin > 0 && sc.fadeOutFrames <= 0;
        int64_t r = (sc.loop && loopSpan > 0)
                      ? (((a - regStart) % loopSpan) + loopSpan) % loopSpan
                      : 0;

        for (int64_t s = a; s < b; ++s) {
            const int     oi = (int)(s - winStart);
            const int64_t ri = s - regStart;               // region-relative
            int64_t ci = off + ri;                         // source frame
            float   dg = 1.0f;                             // declick envelope
            int64_t xi = -1;                               // crossfade partner
            float   xw = 0.0f;                             // its weight
            if (sc.loop) {                                 // wrap the source window
                if (loopSpan <= 0) continue;
                const int64_t rc = r;                      // position of THIS frame
                if (++r >= loopSpan) r = 0;                // ... and the next one's
                ci = off + rc;
                if (wrapWin > 0 && rc >= loopSpan - wrapWin) {
                    // Approaching the splice: fade into the frames that PRECEDE
                    // the loop start, so the last sample of the pass and the
                    // first of the next are one continuous waveform.
                    const float w = ScheduledClip::declickShape(
                        (float)(rc - (loopSpan - wrapWin)) / (float)wrapWin);
                    if (wrapXfade) { xi = off + rc - loopSpan; xw = w; }
                    else           { dg *= 1.0f - w; }
                } else if (wrapWin > 0 && !wrapXfade && rc < wrapWin && ri >= loopSpan) {
                    // No lead-in material: ramp the new pass back up.
                    dg *= ScheduledClip::declickShape((float)rc / (float)wrapWin);
                }
            }
            if (ci < 0 || ci >= nf) continue;              // outside the source
            if (edgeIn  && ri < edgeWin)
                dg *= ScheduledClip::declickShape((float)ri / (float)edgeWin);
            if (edgeOut && ri >= regLen - edgeWin)
                dg *= ScheduledClip::declickShape((float)(regLen - ri) / (float)edgeWin);
            // Fade env only where it matters (the unity middle stays g).
            const float e = (fades ? g * sc.fadeGain(ri, regLen) : g) * chGain * dg;
            float sl = cl[(size_t)ci], sr = cr[(size_t)ci];
            if (xi >= 0 && xi < nf) {
                sl = (1.0f - xw) * sl + xw * cl[(size_t)xi];
                sr = (1.0f - xw) * sr + xw * cr[(size_t)xi];
            }
            outL[oi] += sl * e;
            outR[oi] += sr * e;
        }
    }
}

void AudioClipPlayer::process(const ProcessBlock& blk) {
    const int n = blk.nframes;
    if (n <= 0) return;
    if (!blk.audioOut || blk.numAudioOut < 1 || !blk.audioOut[0]) return;

    const bool stereoOut = blk.numAudioOut > 1 && blk.audioOut[1];
    float* outL = blk.audioOut[0];
    float* outR = stereoOut ? blk.audioOut[1] : blk.audioOut[0];
    // On a MONO bus outR aliases outL, so writing both channels summed L+R into
    // one buffer -- 6 dB hot, and clipping on any centred material.  Fold at
    // -6 dB instead so a mono bus matches the stereo one in level.
    const float chGain = stereoOut ? 1.0f : 0.5f;

    // Always start from silence: as a well-behaved instrument we own our output.
    for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }

    // Mix scheduled clips only while the transport is rolling. One acquire-load
    // of the published snapshot for the whole block (RCU): never half-edited.
    if (blk.isPlaying) {
        // Load AND stamp the slot in one handshake, so a burst of edits cannot
        // recycle it mid-block (see acquireSnapshot/publishSchedule).
        ScheduleSnapshot* snap = acquireSnapshot();
        mixWindow(snap, blk.playPositionSamples, n, outL, outR, chGain);
        readingSnapshot_.store(nullptr, std::memory_order_release);
        // Where a stop would have to pick the audio up, and the ramp it gets.
        wasPlaying_ = true;
        tailPos_    = blk.playPositionSamples + n;
        tailDone_   = 0;
    } else {
        // TRANSPORT STOP DECLICK.  Stopping used to hand back a buffer of zeros
        // whatever the waveform was doing -- a full-scale step on any sustained
        // material.  Keep rendering the timeline where it left off for one
        // declick window, ramping out over the same smoothstep the region edges
        // use, then go quiet.
        // Only a genuine play->stop transition owes a tail: a player that has
        // never rolled (or has already finished its ramp) stays exactly silent.
        if (wasPlaying_) { wasPlaying_ = false; tailArmed_ = true; tailDone_ = 0; }
        const int64_t D = declickFrames_;
        if (tailArmed_ && D > 0 && tailDone_ < D) {
            const int m = (int)std::min<int64_t>(n, D - tailDone_);
            ScheduleSnapshot* snap = acquireSnapshot();
            mixWindow(snap, tailPos_, m, outL, outR, chGain);
            readingSnapshot_.store(nullptr, std::memory_order_release);
            for (int i = 0; i < m; ++i) {
                const float rmp = ScheduledClip::declickShape(
                    (float)(D - (tailDone_ + i)) / (float)D);
                outL[i] *= rmp;
                outR[i] *= rmp;
            }
            tailPos_  += m;
            tailDone_ += m;
            if (tailDone_ >= D) tailArmed_ = false;
        } else {
            tailArmed_ = false;
        }
    }

    // Record/monitor: when armed, capture the incoming audio for this block.
    // Pass the real channel count so a mono bus is not read as stereo.
    if (recording_.load(std::memory_order_acquire) && blk.audioIn) {
        captureBlock(blk.audioIn, n, blk.numAudioIn);
    }

    // Block generation, for the record-buffer keep-alive (see awaitRecordGrace).
    rtGen_.fetch_add(1, std::memory_order_release);
}

}} // namespace PatchKnob::engine
