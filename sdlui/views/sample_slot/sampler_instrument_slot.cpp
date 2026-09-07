#include "sampler_instrument_slot.h"

#include "audio_app.h"
#include "engine/sample_ops.h"
#include "engine/audioclip/audio_clip.h"
#include "engine/audioclip/wav_loader.h"
#include "views/sampler_editor/sampler_editor_view.h"

#include <algorithm>
#include <cmath>

namespace sampleslot {

using namespace PatchKnob::engine;

//----------------------------------------------------------------------------
//  borrowed vs owned audio (see the header).  All READS go through curL/curR;
//  anything about to MUTATE audio calls materialize() first.
//----------------------------------------------------------------------------
const std::vector<float>& SamplerInstrumentSlot::curL() const {
    if ( m_borrow && m_model ) return m_model->clip.ch[0];
    return m_l;
}
const std::vector<float>& SamplerInstrumentSlot::curR() const {
    if ( m_borrow && m_model ) {
        const auto& c = m_model->clip;
        if ( c.ch[1].size() == c.ch[0].size() && !c.ch[1].empty() ) return c.ch[1];
        return c.ch[0];                       // mono: right mirrors left
    }
    return m_r.empty() ? m_l : m_r;
}
void SamplerInstrumentSlot::materialize() {
    if ( !m_borrow ) return;
    m_l = curL();
    m_r = curR();
    m_borrow = false;
}

int  SamplerInstrumentSlot::sampleFrames() const { return (int)curL().size(); }
bool SamplerInstrumentSlot::sampleEditable() const { return !curL().empty(); }

bool SamplerInstrumentSlot::bind( int node, int zoneIndex, ui::SamplerZone* model,
                                  std::function<void()> onChange ) {
    m_node = -1; m_zone = -1;
    m_borrow = false;
    m_l.clear(); m_r.clear(); m_undo.clear(); m_redo.clear(); m_slices.clear();
    m_start = model ? model->start : 0.f;
    m_end = model ? model->end : 1.f;
    m_model = model; m_onChange = std::move( onChange );

    SamplerZoneInfo info;
    // A keyzone model owns the authoritative PCM -- when it is RESIDENT.  Since
    // the shell went metadata-only (rebuild_zones_from_engine no longer copies
    // every zone's audio), a freshly imported zone's clip is EMPTY until someone
    // fetches it: that someone is this bind, for exactly the one zone being
    // opened.  The engine copy is a derived playback copy (trim/gain/reverse
    // baked in), so it can stand in for the source only while the zone's
    // transform is still identity -- which is always true for an untouched
    // import, the only way an empty clip arises.
    const bool identity = m_start <= 0.f && m_end >= 1.f &&
                          model && !model->reverse &&
                          std::fabs( model->gain - 1.f ) < 1e-6f;
    const bool wantFetch = model && model->clip.ch[0].empty() && identity;
    const bool got = ( model && !wantFetch )
        ? PatchKnob::app::audio_app_sampler_get_zone_meta(node,zoneIndex,info)
        : PatchKnob::app::audio_app_sampler_get_zone(node,zoneIndex,info);
    if(!got)
        return false;
    m_info = info;
    m_node = node; m_zone = zoneIndex;

    if ( wantFetch && info.numFrames > 0 ) {
        // Materialise the MODEL's clip from the engine copy (identity
        // transform, so the derived copy IS the source), then borrow it below
        // like any resident zone.
        const int ch = info.stereo ? 2 : 1;
        model->clip.sampleRate = info.sampleRate;
        model->clip.ch[0].assign( (size_t)info.numFrames, 0.f );
        model->clip.ch[1].assign( info.stereo ? (size_t)info.numFrames : 0, 0.f );
        for ( int f = 0; f < info.numFrames; ++f ) {
            model->clip.ch[0][(size_t)f] = info.pcm[(size_t)f * ch];
            if ( info.stereo ) model->clip.ch[1][(size_t)f] = info.pcm[(size_t)f * ch + 1];
        }
    }

    // When mounted in the keyzone window, BORROW the authoritative source clip
    // instead of copying it: selection is a read, not an edit.  The engine PCM
    // path below (no model bound) still de-interleaves into an owned copy.
    size_t frames = 0;
    if (model && !model->clip.ch[0].empty()) {
        m_borrow = true;
        frames = model->clip.ch[0].size();
        m_info.sampleRate=(int)(model->clip.sampleRate>0?model->clip.sampleRate:info.sampleRate);
        m_info.name=model->name;
    } else if (!model) {
        const int ch = info.stereo ? 2 : 1;
        frames = ch > 0 ? info.pcm.size() / (size_t)ch : 0;
        m_l.resize( frames ); m_r.resize( frames );
        for ( size_t i = 0; i < frames; ++i ) {
            m_l[i] = info.pcm[i * (size_t)ch];
            m_r[i] = info.stereo ? info.pcm[i * 2 + 1] : m_l[i];
        }
    }
    if (model) {
        m_info.loopStart = (int)(std::max(0.f,std::min(1.f,model->loopStart)) * frames);
        m_info.loopEnd = (int)(std::max(0.f,std::min(1.f,model->loopEnd)) * frames);
        m_info.loop = model->loop;
        m_slices = model->slices;
    }
    return true;
}

bool SamplerInstrumentSlot::push( bool audioChanged ) {
    if ( !bound() || curL().empty() ) return false;
    // An audio push writes m_l/m_r back into the model; a borrowed read set has
    // no m_l yet, so own it first (no-op when an edit already materialised).
    if ( audioChanged ) materialize();
    const int n = (int)curL().size();
    int ls = m_info.loopStart, le = m_info.loopEnd;
    ls = std::max( 0, std::min( ls, n - 1 ) );
    le = std::max( ls + 1, std::min( le, n ) );
    m_info.numFrames = n;
    m_info.loopStart = ls; m_info.loopEnd = le; m_info.stereo = true;

    // The keyzone editor owns the project-side model.  Keep it synchronized
    // before publishing; otherwise its next Apply restores the pre-edit PCM.
    if ( m_model ) {
        // Dragging a loop point does not alter a single audio sample, but these
        // two assignments copy the ENTIRE file -- and they ran once per
        // mouse-MOTION event.  On a thirty-second stereo sample that is
        // megabytes of memcpy per pixel of pointer travel, which is exactly why
        // the loop markers felt like they were dragging through treacle.
        // Marker-only pushes now skip both the copy and the re-interleave.
        if ( audioChanged ) {
            m_model->clip.ch[0] = m_l;
            m_model->clip.ch[1] = m_r;
            m_model->clip.sampleRate = m_info.sampleRate;
            m_model->clip.sourceSampleRate = m_info.sampleRate;
        }
        m_model->name = m_info.name;
        m_model->loop = m_info.loop;
        m_model->start = m_start;
        m_model->end = m_end;
        m_model->loopStart = n > 0 ? (float)ls / (float)n : 0.f;
        m_model->loopEnd = n > 0 ? (float)le / (float)n : 1.f;
        m_model->slices = m_slices;
        if ( m_onChange ) { m_onChange(); return true; }
    }

    // Re-interleave and hand the zone back.  load_ex REPLACES the zone's audio,
    // which is the only route the machine exposes -- there is no in-place edit.
    // Built HERE and not at the top of the function: the keyzone-model path
    // above returns without ever looking at it, so building it first meant a
    // full copy of the sample allocated and thrown away on every call.
    const std::vector<float>& L = curL();
    const std::vector<float>& R = curR();
    std::vector<float> inter( L.size() * 2 );
    for ( size_t i = 0; i < L.size(); ++i )
    { inter[i * 2] = L[i]; inter[i * 2 + 1] = R[i]; }

    return PatchKnob::app::audio_app_sampler_load_ex(
        m_node, m_info.slot, m_info.level, inter.data(), n, 1,
        m_info.rootKey, m_info.sampleRate, ls, le, m_info.loop ? 1 : 0,
        m_info.loKey, m_info.hiKey, m_info.loVel, m_info.hiVel,
        m_info.noteOffLayer ? 1 : 0, m_info.keyToPitch ? 1 : 0,
        m_info.velToVol ? 1 : 0, m_info.overlapMode, m_info.name.c_str() );
}

void SamplerInstrumentSlot::snapshot() {
    // From the CURRENT audio, wherever it lives -- a borrowed slot must not
    // snapshot its empty owned buffers.
    m_undo.push_back( Snap{ curL(), curR(), m_slices, m_info, m_start, m_end } );
    constexpr size_t kUndoPcmBudget=256u*1024u*1024u;
    auto bytes=[](const Snap& s){return(s.l.size()+s.r.size())*sizeof(float);};
    size_t used=0;for(const Snap& s:m_undo)used+=bytes(s);
    while(m_undo.size()>1&&(m_undo.size()>16||used>kUndoPcmBudget)){
        used-=bytes(m_undo.front());m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
}

void SamplerInstrumentSlot::sampleSetLoopEnabled( bool on ) {
    if ( m_info.loop == on ) return;
    snapshot();
    m_info.loop = on;
    // The flag flips; not one audio sample changes.  push(true) here copied the
    // whole file back into the model on every loop toggle.
    push( false );
}

bool SamplerInstrumentSlot::sampleLoad( const std::string& path, double sampleRate,
                                        std::string* error ) {
    if ( !bound() ) { if ( error ) *error = "no zone selected"; return false; }
    AudioClip clip;
    if ( !loadWav( path, sampleRate, clip, error ) ) return false;
    if ( clip.empty() ) { if ( error ) *error = "sample is empty"; return false; }

    const bool mono = clip.ch[1].empty() || clip.ch[1].size() != clip.ch[0].size();
    // Dropping a file ON a loaded zone REPLACES its audio, which is every bit as
    // destructive as a trim -- and it was the one edit that wiped the undo stack
    // on its way in, so the sample it displaced (and every edit made to it) was
    // unrecoverable.  It snapshots through the same path as sampleOp/sampleClear.
    // Taken AFTER the decode succeeded, so a failed load leaves no phantom entry.
    if ( !curL().empty() ) snapshot();
    m_l = std::move( clip.ch[0] );
    m_r = mono ? m_l : std::move( clip.ch[1] );
    m_borrow = false;                     // the slot owns the replacement
    m_redo.clear(); m_slices.clear();
    m_start = 0.f; m_end = 1.f;
    m_info.sampleRate = (int)( clip.sampleRate > 0.0 ? clip.sampleRate : sampleRate );
    m_info.numFrames  = (int)m_l.size();
    m_info.loopStart  = 0;
    m_info.loopEnd    = (int)m_l.size();
    const size_t slash = path.find_last_of( "/\\" );
    m_info.name = ( slash == std::string::npos ) ? path : path.substr( slash + 1 );
    if ( !push() ) { if ( error ) *error = "could not push the sample to the instrument";
                     return false; }
    return true;
}

void SamplerInstrumentSlot::sampleClear() {
    if ( !bound() ) return;
    // Throwing a zone's audio away is the most destructive thing in here, and
    // it was the one edit that could NOT be undone: the undo stack was wiped
    // on the way out, so a mis-click cost you the sample and every edit made
    // to it.  It snapshots like any other operation now.
    if ( !curL().empty() ) snapshot();
    PatchKnob::app::audio_app_sampler_clear( m_node, m_info.slot );
    m_borrow = false;
    m_l.clear(); m_r.clear(); m_info.name.clear();
    if ( m_model ) {
        m_model->clip = AudioClip{}; m_model->name.clear();
        if ( m_onChange ) m_onChange();
    }
    m_redo.clear(); m_slices.clear();
}

int SamplerInstrumentSlot::samplePeaks( int64_t from, int64_t to,
                                        float* outMin, float* outMax,
                                        int buckets ) const {
    if ( !outMin || !outMax || buckets <= 0 ) return 0;
    const std::vector<float>& L = curL();
    const std::vector<float>& R = curR();
    const int64_t n = (int64_t)L.size();
    if ( n <= 0 ) return 0;
    from = std::max<int64_t>( 0, std::min( from, n ) );
    to   = std::max<int64_t>( 0, std::min( to, n ) );
    if ( to <= from ) return 0;
    const double per = (double)( to - from ) / (double)buckets;
    for ( int b = 0; b < buckets; ++b ) {
        int64_t s = from + (int64_t)( per * b );
        int64_t e = from + (int64_t)( per * ( b + 1 ) );
        if ( e <= s ) e = s + 1;
        if ( e > n ) e = n;
        float mn = 1.f, mx = -1.f;
        for ( int64_t i = s; i < e; ++i ) {
            const float v = 0.5f * ( L[(size_t)i] + R[(size_t)i] );
            mn = std::min( mn, v ); mx = std::max( mx, v );
        }
        if ( mx < mn ) mn = mx = 0.f;
        outMin[b] = mn; outMax[b] = mx;
    }
    return buckets;
}

float SamplerInstrumentSlot::sampleMarker( int marker ) const {
    const float n = (float)std::max<size_t>( 1, curL().size() );
    switch ( marker ) {
        case SM_START:      return m_start;
        case SM_END:        return m_end;
        case SM_LOOP_START: return (float)m_info.loopStart / n;
        case SM_LOOP_END:   return (float)m_info.loopEnd / n;
        default: return 0.f;
    }
}

void SamplerInstrumentSlot::sampleSetMarker( int marker, float v ) {
    v = v < 0.f ? 0.f : ( v > 1.f ? 1.f : v );
    const int n = (int)curL().size();
    // Every one of these is a NO-OP push when the value has not moved, and a
    // pointer sitting still on a pixel delivers motion events forever.  The
    // editor's clamp_markers() also writes all four markers back after every
    // drag step, so three of the four calls were always redundant.  Bail out
    // before touching the engine, and tell push() the audio is untouched.
    switch ( marker ) {
        case SM_START: if ( m_start == v ) return; m_start = v; push( false ); break;
        case SM_END:   if ( m_end   == v ) return; m_end   = v; push( false ); break;
        case SM_LOOP_START: {
            const int f = (int)( v * (float)n );
            if ( m_info.loopStart == f ) return;
            m_info.loopStart = f; push( false ); break;
        }
        case SM_LOOP_END: {
            const int f = (int)( v * (float)n );
            if ( m_info.loopEnd == f ) return;
            m_info.loopEnd = f; push( false ); break;
        }
        default: break;
    }
}

bool SamplerInstrumentSlot::sampleMarkerActive( int marker ) const {
    if ( marker == SM_LOOP_START || marker == SM_LOOP_END ) return m_info.loop;
    return true;
}

bool SamplerInstrumentSlot::sampleOp( int op, int64_t from, int64_t to, float arg ) {
    if ( curL().empty() ) return false;
    snapshot();
    materialize();                        // ops mutate m_l/m_r in place
    const bool changed = apply_sample_op( op, m_l, m_r, (double)m_info.sampleRate,
                                          from, to, arg, m_clipL, m_clipR );
    if ( !changed ) { m_undo.pop_back(); return op == SOP_COPY; }
    push();
    return true;
}

bool SamplerInstrumentSlot::sampleUndo() {
    if ( m_undo.empty() ) return false;
    m_redo.push_back( Snap{ curL(), curR(), m_slices, m_info, m_start, m_end } );
    const Snap& s = m_undo.back();
    m_l = s.l; m_r = s.r; m_slices = s.slices; m_info = s.info;
    m_start = s.start; m_end = s.end;
    m_borrow = false;
    m_undo.pop_back();
    push();
    return true;
}

bool SamplerInstrumentSlot::sampleRedo() {
    if ( m_redo.empty() ) return false;
    m_undo.push_back( Snap{ curL(), curR(), m_slices, m_info, m_start, m_end } );
    const Snap& s = m_redo.back();
    m_l = s.l; m_r = s.r; m_slices = s.slices; m_info = s.info;
    m_start = s.start; m_end = s.end;
    m_borrow = false;
    m_redo.pop_back();
    push();
    return true;
}

bool SamplerInstrumentSlot::sampleStats( int64_t from, int64_t to,
                                         float* peak, float* rms ) const {
    const std::vector<float>& L = curL();
    const std::vector<float>& R = curR();
    const int64_t n = (int64_t)L.size();
    if ( n <= 0 ) return false;
    from = std::max<int64_t>( 0, std::min( from, n ) );
    to   = std::max<int64_t>( 0, std::min( to, n ) );
    if ( to <= from ) return false;
    float pk = 0.f; double acc = 0.0;
    for ( int64_t i = from; i < to; ++i ) {
        const float m = 0.5f * ( L[(size_t)i] + R[(size_t)i] );
        pk = std::max( pk, std::fabs( m ) );
        acc += (double)m * (double)m;
    }
    if ( peak ) *peak = pk;
    if ( rms )  *rms  = (float)std::sqrt( acc / (double)( to - from ) );
    return true;
}

bool SamplerInstrumentSlot::sampleSave( const std::string& path, std::string* error ) {
    if ( curL().empty() ) { if ( error ) *error = "nothing to save"; return false; }
    AudioClip clip;
    clip.ch[0] = curL(); clip.ch[1] = curR();
    clip.sampleRate = clip.sourceSampleRate = (double)m_info.sampleRate;
    clip.name = m_info.name;
    return saveWav16( path, clip, error );
}

int64_t SamplerInstrumentSlot::sampleZeroCross( int64_t frame, int dir ) const {
    return find_zero_cross( curL(), frame, dir );
}

float SamplerInstrumentSlot::sliceAt( int index ) const {
    if ( index < 0 || index >= (int)m_slices.size() ) return 0.f;
    return m_slices[(size_t)index];
}

int SamplerInstrumentSlot::sliceAdd( float v ) {
    if ( m_slices.size() >= 128 ) return -1;
    v = v < 0.f ? 0.f : ( v > 1.f ? 1.f : v );
    // Refuse a slice landing on one that is already there.  Double-clicking a
    // spot twice used to stack invisible duplicates: the lane looked unchanged,
    // the count crept towards the 128 limit, and removing "the" slice left
    // another one exactly underneath it.
    const size_t nFrames = curL().size();
    const float minGap = ( nFrames > 1 ) ? 8.f / (float)nFrames : 1e-4f;
    for ( float s : m_slices ) if ( std::fabs( s - v ) < minGap ) return -1;
    const auto at = std::lower_bound( m_slices.begin(), m_slices.end(), v );
    const int idx = (int)( at - m_slices.begin() );
    m_slices.insert( at, v );
    if (m_model) { m_model->slices=m_slices; if(m_onChange)m_onChange(); }
    return idx;
}

bool SamplerInstrumentSlot::sliceRemove( int index ) {
    if ( index < 0 || index >= (int)m_slices.size() ) return false;
    m_slices.erase( m_slices.begin() + index );
    if (m_model) { m_model->slices=m_slices; if(m_onChange)m_onChange(); }
    return true;
}

void SamplerInstrumentSlot::sliceSet( int index, float v ) {
    if ( index < 0 || index >= (int)m_slices.size() ) return;
    m_slices[(size_t)index] = v < 0.f ? 0.f : ( v > 1.f ? 1.f : v );
    std::sort( m_slices.begin(), m_slices.end() );
    if (m_model) { m_model->slices=m_slices; if(m_onChange)m_onChange(); }
}

} // namespace sampleslot
