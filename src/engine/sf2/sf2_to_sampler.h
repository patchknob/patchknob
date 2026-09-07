//----------------------------------------------------------------------------
//  src/engine/sf2/sf2_to_sampler.h -- map a parsed SoundFont preset onto the
//  built-in sampler's zone model.
//
//  This is plumbing, not maths: sf2_reader.h has already converted every SF2
//  generator into the unit the sampler wants (seconds, Hz, cents, dB, -1..+1
//  pan), so importPresetIntoSampler() is mostly field-to-setter. The three
//  things that are NOT plumbing:
//
//   1. INTERNING.  Multisampled instruments reuse one recording across many
//      velocity layers/round-robins, so a `map<sampleIndex, SharedPcm>` (see
//      internSample() in the .cpp) hands the SAME buffer to every zone that
//      references it instead of copying it per zone.
//   2. STEREO.  SF2 stores a stereo instrument as TWO mono samples joined by
//      Sample::sampleLink (sampleType 2 = right, 4 = left). Those must be
//      interleaved into ONE stereo buffer and loaded as a single stereo zone
//      -- never as two mono zones panned hard left/right. A broken link
//      (partner out of range, not the opposite channel, or a differing frame
//      count) falls back to mono rather than failing the whole preset.
//   3. ZONE ADDRESSING.  Zones are addressed (slot, level) exactly as
//      sampler_load_sample_ex() does. Every zone this importer loads gets its
//      OWN unique slot (a simple 1-based counter, one per emitted sampler
//      zone) at level 0: SF2 zones already carry their own complete key/vel
//      range and don't need the level-layering scheme original callers used
//      for round-robin, so a bijection zone<->slot keeps the addressing
//      trivial to reason about and collision-free up to however many zones a
//      preset has.
//
//  THREADING.  Message thread only -- this pulls PCM off disk (readPresetPcm)
//  and calls sampler setters that take the instrument's own lock.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_SF2_TO_SAMPLER_H
#define PATCHKNOB_ENGINE_SF2_TO_SAMPLER_H

#include "sf2_reader.h"
#include "../plugin_api.h"
#include "../sampler/sampler_instrument.h"   // SharedPcm

#include <string>
#include <vector>

namespace PatchKnob { namespace engine { namespace sf2 {

struct ImportOptions {
    //! Hard cap on zones actually loaded. 0 = no cap. A 2976-zone concert
    //! grand is legitimate but heavy; the UI may want a cheaper preview
    //! import.
    int  maxZones = 0;
    //! Drop zones whose velocity range is fully covered by an earlier zone
    //! with the same key range -- round-robin layers that cost RAM without
    //! changing the map. Off by default: it is a lossy convenience, not a
    //! correctness fix.
    bool collapseVelocityLayers = false;
    //! Clear the sampler's existing zones before loading (the default, and what
    //! auditioning one patch after another needs).  Set false only to LAYER a
    //! preset on top of what is already loaded.
    bool replaceExisting = true;
};

struct ImportResult {
    int zonesLoaded    = 0;
    int zonesSkipped   = 0;   //!< over the cap, or no sample
    int samplesInterned= 0;   //!< DISTINCT buffers allocated (not zone count)
    int stereoPairs    = 0;
    int zonesReplaced  = 0;   //!< slots cleared before loading
    std::string warning;      //!< human-readable, empty when nothing to report
};

//! One zone fully PREPARED for installation: PCM decoded, interned and (for
//! stereo pairs) interleaved, every generator resolved.  Plain data with no
//! sampler handle anywhere in it, which is the point: preparing is I/O + CPU
//! and may run on a worker thread; installing is sampler setters and is
//! message-thread only.  The split is what makes an asynchronous import
//! possible without ever calling a sampler setter off the message thread.
struct PreparedZone {
    SharedPcm   pcm;                 //!< interned; shared across zones
    int         numFrames = 0;
    bool        stereo = false;
    int         sampleRate = 44100;
    Zone        z;                   //!< resolved generator values
    std::string name;
};

//! preparePresetZones()'s output: the zones to install plus the statistics
//! the prepare phase alone can know (skips, interned buffers, stereo pairs,
//! warnings).  zonesLoaded/zonesReplaced are filled by the install phase.
struct PreparedPreset {
    std::vector<PreparedZone> zones;
    ImportResult              stats;
};

//! Progress hook for preparePresetZones(): (samplesDecoded, samplesTotal),
//! monotonic; return false to CANCEL (prepare stops at the next sample/zone
//! boundary and fails with error == "cancelled").  After I/O completes it is
//! still polled -- with (total, total) -- between zones, so cancellation stays
//! responsive through the interleave/intern phase too.
using ImportProgressFn = std::function<bool(int done, int total)>;

//! PHASE 1 of an import: decode exactly the PCM the surviving zones need,
//! intern shared buffers, interleave stereo pairs, resolve every parameter.
//! No sampler calls -- THREAD-SAFE for any one (font, fontPath) at a time
//! (it mutates `font`'s staging PCM), so a worker thread may run it while
//! the message thread keeps painting.
bool preparePresetZones(const std::string& fontPath, SoundFont& font,
                        int presetIndex, const ImportOptions& opts,
                        PreparedPreset& out, std::string& error,
                        const ImportProgressFn& progress = {});

//! PHASE 2: land prepared zones in the sampler.  Message thread only (same
//! contract as every sampler setter).  Seeds `out` from prepared.stats, then
//! adds zonesLoaded/zonesReplaced.  Cheap relative to phase 1 -- no I/O, no
//! decode; it clears the old patch (when opts.replaceExisting) and hands the
//! already-shared buffers over, so the message-thread cost is bounded by the
//! setter calls alone.
bool installPreparedPreset(IPluginInstance* sampler, const PreparedPreset& prepared,
                           const ImportOptions& opts, ImportResult& out,
                           std::string& error);

//! Load one preset of `font` into `sampler` as sampler zones.
//! `font` may come from a headers-only read; this function pulls the PCM it
//! needs itself. Message thread only.
//!
//! Sample::pcm is a STAGING AREA, not an archive: the import moves each
//! staged buffer into the shared buffer it hands the sampler (one allocation
//! per distinct sample instead of two), so after a successful import the
//! preset's samples are left with empty pcm. Importing from the same `font`
//! object again is still valid -- the PCM is simply re-fetched from
//! `fontPath` -- but callers keeping their own decoded copies should not
//! expect them to survive an import.
bool importPresetIntoSampler(const std::string& fontPath, SoundFont& font,
                             int presetIndex, IPluginInstance* sampler,
                             const ImportOptions& opts, ImportResult& out,
                             std::string& error);

}}} // namespace PatchKnob::engine::sf2

#endif // PATCHKNOB_ENGINE_SF2_TO_SAMPLER_H
