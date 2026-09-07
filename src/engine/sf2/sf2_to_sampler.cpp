//----------------------------------------------------------------------------
//  src/engine/sf2/sf2_to_sampler.cpp -- see sf2_to_sampler.h for the contract.
//----------------------------------------------------------------------------
#include "sf2_to_sampler.h"

#include <set>
#include "../sampler/sampler_instrument.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace PatchKnob { namespace engine { namespace sf2 {

namespace {

//! Resolve the key a sample should be interned under, and whether it pairs
//! into a stereo buffer.
//!
//! Mono, or any sample whose link is broken, interns under its OWN index.
//! A good link interns under the SMALLER of the two indices, so both the left
//! and the right zone entry (SF2 stores a stereo instrument as two separate
//! zones, one per channel) land on the same intern entry regardless of which
//! one is processed first.
//!
//! "Broken" is the reader's samplesFormStereoPair() predicate -- ONE shared
//! definition, judged from the shdr headers, so the verdict here always
//! matches the set of partners readPresetPcm() decoded, and stays stable even
//! after a sample's staging PCM has been handed off to the sampler (see the
//! move in the import loop below).  Any broken link falls back to mono rather
//! than failing the zone.
int internKeyFor(const SoundFont& font, int sampleIndex, bool& outStereo, int& outPartner) {
    outStereo = false;
    outPartner = -1;
    int link = -1;
    if (!samplesFormStereoPair(font, sampleIndex, link)) return sampleIndex;
    outStereo = true;
    outPartner = link;
    return std::min(sampleIndex, link);
}

//! Interleave a linked mono pair into one stereo buffer, L/R identified by
//! sampleType (4 = left, 2 = right) rather than by which one triggered the
//! merge, so the result is the same no matter which half of the pair a preset
//! happens to list first.
SharedPcm interleavePair(const Sample& a, const Sample& b) {
    const Sample& L = (a.sampleType == 4) ? a : b;
    const Sample& R = (a.sampleType == 2) ? a : b;
    const size_t n = L.pcm.size();
    auto buf = std::make_shared<std::vector<float>>();
    buf->resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        (*buf)[2 * i]     = L.pcm[i];
        (*buf)[2 * i + 1] = R.pcm[i];
    }
    return buf;
}

void appendWarning(std::string& warning, int count, const char* what) {
    if (count <= 0) return;
    if (!warning.empty()) warning += "; ";
    warning += std::to_string(count) + " zone(s) " + what;
}

} // namespace

//----------------------------------------------------------------------------
//  PHASE 1 -- decode + intern + resolve, no sampler anywhere.  This is the
//  old import loop with every sampler call replaced by "emit a PreparedZone";
//  the skip logic, interning and stereo handling are byte-for-byte the same
//  decisions in the same order, which is what keeps the capped/collapsed
//  bookkeeping (and the tests over it) meaningful.
//----------------------------------------------------------------------------
bool preparePresetZones(const std::string& fontPath, SoundFont& font,
                        int presetIndex, const ImportOptions& opts,
                        PreparedPreset& out, std::string& error,
                        const ImportProgressFn& progress) {
    out = PreparedPreset{};
    error.clear();

    if (presetIndex < 0 || presetIndex >= (int)font.presets.size()) {
        error = "preset index out of range";
        return false;
    }

    const Preset& preset = font.presets[(size_t)presetIndex];

    // `font` may be a headers-only read (no Sample::pcm yet); pull just the
    // PCM this import will actually LOAD, once, before touching any of it.
    // Not "the preset's PCM": a capped preview (opts.maxZones) or a collapsed
    // import must not pay I/O for the zones it is about to skip, so this
    // pre-pass walks the zones with the SAME four skip checks, in the SAME
    // order, as the zone loop below -- missing, mirror-half, collapsed,
    // capped -- and marks only the survivors' samples (plus their stereo
    // partners) for decode.  If you change a skip rule in one walk, change it
    // in the other or capped/collapsed counts will misattribute.
    //
    // A sample already carrying pcm is left alone, so this is safe to call
    // when the caller already did readPresetPcm().  NOTE that the zone loop
    // below then CONSUMES the staging PCM (moves it into the shared buffers
    // it emits) -- preparing from the same `font` object again simply re-runs
    // this fetch, it does not find the old staging data.
    int samplesTotal = 0;   // what readSamplesPcm will actually decode
    {
        std::vector<uint8_t> want(font.samples.size(), 0);
        std::vector<char> consumed(font.samples.size(), 0);
        struct R { int loKey, hiKey, loVel, hiVel; };
        std::vector<R> ranges;
        int willLoad = 0;
        for (const Zone& z : preset.zones) {
            if (z.sampleIndex < 0 || z.sampleIndex >= (int)font.samples.size()) continue;
            const Sample& s = font.samples[(size_t)z.sampleIndex];
            if (s.pcm.empty() && s.end <= s.start) continue;     // the loop calls this "missing"
            if (consumed[(size_t)z.sampleIndex]) continue;       // mirror half of a pair
            if (opts.collapseVelocityLayers) {
                bool covered = false;
                for (const R& r : ranges)
                    if (r.loKey == z.loKey && r.hiKey == z.hiKey &&
                        z.loVel >= r.loVel && z.hiVel <= r.hiVel) { covered = true; break; }
                if (covered) continue;
            }
            if (opts.maxZones > 0 && willLoad >= opts.maxZones) break;  // nothing further loads
            int partner = -1;
            if (samplesFormStereoPair(font, z.sampleIndex, partner)) {
                want[(size_t)partner]     = 1;
                consumed[(size_t)partner] = 1;
            }
            want[(size_t)z.sampleIndex] = 1;
            ranges.push_back({z.loKey, z.hiKey, z.loVel, z.hiVel});
            ++willLoad;
        }
        for (size_t i = 0; i < want.size(); ++i)
            if (want[i] && font.samples[i].pcm.empty()) ++samplesTotal;
        if (!readSamplesPcm(fontPath, font, want, error, progress)) return false;
    }

    // ---- interning: DISTINCT buffers only, shared across every zone that
    // references them. Keyed by internKeyFor() so a stereo pair's two zone
    // entries collapse onto the SAME entry regardless of processing order.
    // A flat vector indexed by sample index (the keys ARE sample indices), not
    // a map: O(1), no node allocations in a loop that runs thousands of times.
    struct Interned { SharedPcm pcm; int frames = 0; bool stereo = false; };
    std::vector<Interned> interned(font.samples.size());
    // A sample index that has been folded into an EARLIER zone as the other
    // half of a stereo pair must not also become its own (redundant, mono)
    // zone when its own zone entry is reached later in the list.
    std::vector<char> consumedAsPartner(font.samples.size(), 0);
    // (loKey, hiKey, loVel, hiVel) of every zone actually emitted so far, for
    // collapseVelocityLayers's "same key range, subset velocity range" check.
    struct Range { int loKey, hiKey, loVel, hiVel; };
    std::vector<Range> loadedRanges;

    int missingCount = 0, cappedCount = 0, collapsedCount = 0, fallbackCount = 0;
    int zoneIdx = 0;

    for (const Zone& z : preset.zones) {
        // I/O is done, but interleaving thousands of zones still takes real
        // time -- keep cancellation responsive through this phase too.
        if (progress && (zoneIdx++ & 127) == 0 &&
            !progress(samplesTotal, samplesTotal)) {
            error = "cancelled";
            return false;
        }
        if (z.sampleIndex < 0 || z.sampleIndex >= (int)font.samples.size()) {
            ++out.stats.zonesSkipped; ++missingCount; continue;
        }
        Sample& samp0 = font.samples[(size_t)z.sampleIndex];

        bool stereo = false; int partner = -1;
        int key = internKeyFor(font, z.sampleIndex, stereo, partner);

        // "Missing" means NOTHING could ever be available for this zone: no
        // staging PCM, no interned buffer from an earlier zone, and headers
        // that promise no audio either.  Two legitimate ways a live zone has
        // empty staging: the PCM was MOVED into an interned buffer by an
        // earlier zone over the same sample (the interned entry proves the
        // audio exists), or the decode pre-pass above ruled this zone out --
        // capped, collapsed, mirror half -- in which case the checks below
        // skip it under its REAL reason, not as missing.
        if (samp0.pcm.empty() && !interned[(size_t)key].pcm && samp0.end <= samp0.start) {
            ++out.stats.zonesSkipped; ++missingCount; continue;
        }
        if (consumedAsPartner[(size_t)z.sampleIndex]) continue;   // the mirror half; already merged

        if (opts.collapseVelocityLayers) {
            bool covered = false;
            for (const Range& r : loadedRanges) {
                if (r.loKey == z.loKey && r.hiKey == z.hiKey &&
                    z.loVel >= r.loVel && z.hiVel <= r.hiVel) { covered = true; break; }
            }
            if (covered) { ++out.stats.zonesSkipped; ++collapsedCount; continue; }
        }

        if (opts.maxZones > 0 && (int)out.zones.size() >= opts.maxZones) {
            ++out.stats.zonesSkipped; ++cappedCount; continue;
        }

        // The headers say "pair", but interleaving still needs BOTH halves
        // decoded (a caller may hand-fill a font with only one half's PCM).
        // A half-decoded pair falls back to mono under the zone's OWN index,
        // never under the pair key, so the partner's own zone can still load
        // its side as mono instead of silently borrowing the wrong buffer.
        if (stereo && !interned[(size_t)key].pcm &&
            (samp0.pcm.empty() || font.samples[(size_t)partner].pcm.size() != samp0.pcm.size())) {
            stereo = false; partner = -1; key = z.sampleIndex;
            if (samp0.pcm.empty() && !interned[(size_t)key].pcm) {
                ++out.stats.zonesSkipped; ++missingCount; continue;
            }
        }
        Interned& entry = interned[(size_t)key];
        if (!entry.pcm) {
            if (stereo) {
                Sample& sampP = font.samples[(size_t)partner];
                entry.pcm    = interleavePair(samp0, sampP);
                entry.frames = (int)(entry.pcm->size() / 2);
                entry.stereo = true;
                ++out.stats.stereoPairs;
                // The halves' staging copies just became redundant with the
                // interleaved buffer; dropping them here keeps peak memory at
                // ~1x the patch's PCM instead of 2x (85 MB extra on the
                // measured Concert Grand).  A later zone over either half
                // lands on this interned entry, never on Sample::pcm.
                samp0.pcm.clear(); samp0.pcm.shrink_to_fit();
                sampP.pcm.clear(); sampP.pcm.shrink_to_fit();
            } else {
                // MOVE, don't copy: this is the only consumer of the staging
                // buffer, and every other zone over this sample shares the
                // interned pointer.  (readPresetPcm() transparently re-decodes
                // from sourcePath if this font object is ever imported from
                // again -- Sample::pcm is a staging area, not an archive.)
                entry.pcm    = std::make_shared<const std::vector<float>>(std::move(samp0.pcm));
                entry.frames = (int)entry.pcm->size();
                entry.stereo = false;
                samp0.pcm.clear(); samp0.pcm.shrink_to_fit();   // moved-from: make the state explicit
            }
            ++out.stats.samplesInterned;
        }
        // The interned entry is AUTHORITATIVE about the buffer it holds --
        // frames and channel count describe what was actually interleaved or
        // moved, not what the headers promised -- so the zone is emitted from
        // the entry, never from the local pairing verdict.
        if (entry.stereo && partner >= 0) consumedAsPartner[(size_t)partner] = 1;
        if (samp0.isStereoPair() && !entry.stereo) ++fallbackCount;   // fell back to mono

        if (entry.frames <= 0) { ++out.stats.zonesSkipped; ++missingCount; continue; }

        PreparedZone pz;
        pz.pcm        = entry.pcm;
        pz.numFrames  = entry.frames;
        pz.stereo     = entry.stereo;
        pz.sampleRate = (int)samp0.sampleRate;
        pz.z          = z;
        pz.name       = samp0.name;
        out.zones.push_back(std::move(pz));

        loadedRanges.push_back({z.loKey, z.hiKey, z.loVel, z.hiVel});
    }

    appendWarning(out.stats.warning, missingCount,   "skipped (no sample)");
    appendWarning(out.stats.warning, cappedCount,    "skipped (over the zone cap)");
    appendWarning(out.stats.warning, collapsedCount, "skipped (velocity layer collapsed)");
    appendWarning(out.stats.warning, fallbackCount,  "fell back to mono (broken stereo link)");

    return true;
}

//----------------------------------------------------------------------------
//  PHASE 2 -- message thread only.  Everything expensive already happened in
//  phase 1; what is left is one clear + the setter calls, which is the part
//  that MUST be on the message thread (the sampler's zone API contract).
//----------------------------------------------------------------------------
bool installPreparedPreset(IPluginInstance* sampler, const PreparedPreset& prepared,
                           const ImportOptions& opts, ImportResult& out,
                           std::string& error) {
    out = prepared.stats;
    error.clear();
    if (!sampler) { error = "no sampler instance to import into"; return false; }

    // CLEAR THE OLD PATCH FIRST (see the header for why replace is the
    // default): slot numbering restarts at 1 for every import, so without
    // this a smaller patch loaded over a larger one leaves the tail of the
    // previous patch in place -- leaked memory, layered envelopes, and a
    // voice allocator thrashing over stale overlapping zones.
    //
    // Deliberately AFTER prepare, not before: the old patch stays installed
    // and playable for the whole (possibly long, possibly cold-disk) prepare
    // phase, and an import that is superseded or fails never tears anything
    // down.  The transient cost is both patches' PCM resident at once.
    if (opts.replaceExisting) {
        // ONE call, one lock.  Clearing slot-by-slot is O(N^2) -- each clearSlot
        // erases from the zone vector and rebuilds the key index (measured
        // 458 ms..8.7 s per 2976-zone replace depending on index state).
        out.zonesReplaced = sampler_zone_count(sampler);
        sampler_clear_all_zones(sampler);
    }

    // See sf2_to_sampler.h: every zone gets its OWN unique slot (a simple
    // 1-based counter) at level 0.
    int nextSlot = 1;
    const bool freshSlots = opts.replaceExisting;

    for (const PreparedZone& pz : prepared.zones) {
        const Zone& z = pz.z;
        const int slot = nextSlot++;
        const int level = 0;

        sampler_load_sample_shared(sampler, slot, level, pz.pcm, pz.numFrames, pz.stereo,
                                   z.rootKey, pz.sampleRate,
                                   (int)z.loopStart, (int)z.loopEnd, z.loop,
                                   z.loKey, z.hiKey, z.loVel, z.hiVel,
                                   /*noteOffLayer=*/false, /*keyToPitch=*/true,
                                   /*velToVol=*/true, /*overlapMode=*/0,
                                   pz.name.c_str());

        SamplerZoneEnv amp{};
        amp.delay = z.delayVol; amp.attack = z.attackVol; amp.hold = z.holdVol;
        amp.decay = z.decayVol; amp.sustain = z.sustainVol; amp.release = z.releaseVol;
        amp.enabled = 1;
        sampler_set_zone_env(sampler, slot, level, /*env=*/0, &amp);

        SamplerZoneEnv mod{};
        mod.delay = z.delayMod; mod.attack = z.attackMod; mod.hold = z.holdMod;
        mod.decay = z.decayMod; mod.sustain = z.sustainMod; mod.release = z.releaseMod;
        mod.enabled = 1;
        sampler_set_zone_env(sampler, slot, level, /*env=*/1, &mod);

        // Each setter below takes the instrument mutex and does a linear
        // (slot, level) lookup, so with thousands of zones every avoided call
        // matters.  A setter may be skipped ONLY when both hold:
        //   * the value being set is exactly what a freshly-constructed zone
        //     already contains (see SampleZone's defaults), so skipping is
        //     observationally identical -- read-back included; and
        //   * the zone is KNOWN fresh.  replaceExisting cleared every slot
        //     above, so each slot our counter hands out was just created by
        //     loadZoneShared's push_back with default parameters.  Without the
        //     clear (layering imports), loadZoneShared may have overwritten an
        //     existing zone and deliberately KEPT its old parameters -- then
        //     every setter must run to stamp out the stale values.
        // The two envelope setters above are never skipped: the importer marks
        // envelopes enabled, which a fresh zone's are not.
        if (!freshSlots || z.modEnvToPitchCents != 0.f || z.modEnvToFilterCents != 0.f)
            sampler_set_zone_modroute(sampler, slot, level, z.modEnvToPitchCents, z.modEnvToFilterCents);
        if (!freshSlots || z.cutoffHz != 0.f || z.resonanceDb != 0.f)
            sampler_set_zone_filter(sampler, slot, level, z.cutoffHz, z.resonanceDb);
        if (!freshSlots || z.coarseTune != 0 || z.fineTune != 0 || z.scaleTuning != 100)
            sampler_set_zone_tuning(sampler, slot, level, z.coarseTune, z.fineTune, z.scaleTuning);
        if (!freshSlots || z.pan != 0.f || z.attenuationDb != 0.f)
            sampler_set_zone_level(sampler, slot, level, z.pan, z.attenuationDb);
        if (!freshSlots || z.exclusiveClass != 0)
            sampler_set_zone_exclusive(sampler, slot, level, z.exclusiveClass);

        ++out.zonesLoaded;
    }

    return true;
}

//----------------------------------------------------------------------------
bool importPresetIntoSampler(const std::string& fontPath, SoundFont& font,
                             int presetIndex, IPluginInstance* sampler,
                             const ImportOptions& opts, ImportResult& out,
                             std::string& error) {
    out = ImportResult{};
    error.clear();

    if (!sampler) { error = "no sampler instance to import into"; return false; }

    PreparedPreset prepared;
    if (!preparePresetZones(fontPath, font, presetIndex, opts, prepared, error))
        return false;
    return installPreparedPreset(sampler, prepared, opts, out, error);
}

}}} // namespace PatchKnob::engine::sf2
