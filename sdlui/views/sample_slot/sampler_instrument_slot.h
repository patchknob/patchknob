//----------------------------------------------------------------------------
//  sdlui/views/sample_slot/sampler_instrument_slot.h
//
//  Makes the native keyzone Sampler INSTRUMENT an ISampleSlot, so it gets exactly
//  the same waveform editor, disk browser and edit operations as the SMPL-1
//  rack module -- one system, two hosts.
//
//  It edits ONE zone of the instrument at a time (the multisample keyranges
//  stay the instrument's business; the editor only ever sees the zone you
//  picked).  A working copy of the zone's PCM lives here: edits are applied to
//  it through engine/sample_ops.h and then pushed back into the machine with
//  audio_app_sampler_load_ex, which is the only way to replace a zone's audio.
//
//  Slices ARE supported here (unlike the one-shot module): a slice list on a
//  long sample is the natural precursor to chopping it across keys.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_SAMPLER_INSTRUMENT_SLOT_H
#define PATCHKNOB_SDLUI_SAMPLER_INSTRUMENT_SLOT_H

#include "engine/sample_slot.h"
#include "engine/sampler/sampler_instrument.h"

#include <string>
#include <vector>
#include <functional>

namespace ui { struct SamplerZone; }

namespace sampleslot {

class SamplerInstrumentSlot : public PatchKnob::engine::ISampleSlot {
public:
    //! Point at zone `zoneIndex` of sampler node `node`; pulls its audio in.
    //! Returns false when the node/zone does not exist.
    bool bind( int node, int zoneIndex, ui::SamplerZone* model = nullptr,
               std::function<void()> onChange = {} );
    void unbind() { m_node = -1; m_zone = -1; m_model = nullptr; m_onChange = {};
                    m_borrow = false; m_l.clear(); m_r.clear(); }
    bool bound() const { return m_node >= 0 && m_zone >= 0; }
    int  node() const { return m_node; }
    int  zone() const { return m_zone; }

    // ---- ISampleSlot -------------------------------------------------------
    bool sampleLoad( const std::string& path, double sampleRate,
                     std::string* error ) override;
    void sampleClear() override;
    const char* sampleName() const override { return m_info.name.c_str(); }
    int  sampleFrames() const override;   // borrowed or owned length
    double sampleRate() const override { return (double)m_info.sampleRate; }

    int  samplePeaks( int64_t from, int64_t to,
                      float* outMin, float* outMax, int buckets ) const override;

    float sampleMarker( int marker ) const override;
    void  sampleSetMarker( int marker, float value01 ) override;
    bool  sampleMarkerActive( int marker ) const override;
    bool  sampleLoopEnabled() const override { return m_info.loop; }
    void  sampleSetLoopEnabled( bool on ) override;

    bool sampleEditable() const override; // borrowed or owned
    bool sampleOp( int op, int64_t from, int64_t to, float arg ) override;
    bool sampleUndo() override;
    bool sampleRedo() override;
    bool sampleCanUndo() const override { return !m_undo.empty(); }
    bool sampleCanRedo() const override { return !m_redo.empty(); }
    bool sampleStats( int64_t from, int64_t to, float* peak, float* rms ) const override;
    bool sampleSave( const std::string& path, std::string* error ) override;
    bool sampleHasClipboard() const override { return !m_clipL.empty(); }
    int64_t sampleZeroCross( int64_t frame, int dir ) const override;

    // slices: the instrument's editor uses these to chop a long sample
    bool  slicesSupported() const override { return true; }
    int   sliceCount() const override { return (int)m_slices.size(); }
    float sliceAt( int index ) const override;
    int   sliceAdd( float value01 ) override;
    bool  sliceRemove( int index ) override;
    void  sliceSet( int index, float value01 ) override;

private:
    //! Write the zone back.  `audioChanged` false means only markers/flags
    //! moved, which lets the whole-file copy and re-interleave be skipped --
    //! that copy used to run on every mouse-motion event of a marker drag.
    bool push( bool audioChanged = true );
    void snapshot();                   //!< bounded undo push

    // ---- working audio: borrowed vs owned ---------------------------------
    //  Binding used to COPY the zone's whole PCM into m_l/m_r "so it could be
    //  edited" -- ~60 ms and a duplicate buffer for a ten-minute take, paid on
    //  every zone SELECTION even though most selections never edit anything.
    //  The slot now BORROWS the model's clip for all read paths (peaks, stats,
    //  markers, save) and materialises its own copy only when an edit is about
    //  to mutate audio.  The borrow is only ever of m_model->clip, which this
    //  class already holds a pointer to under the existing rebind-on-change
    //  invariant (the shell re-binds whenever the zone vector changes).
    const std::vector<float>& curL() const;
    const std::vector<float>& curR() const;
    void materialize();                //!< borrow -> owned copy (no-op if owned)

    int m_node = -1, m_zone = -1;
    PatchKnob::engine::SamplerZoneInfo m_info;
    bool m_borrow = false;             //!< reads go to m_model->clip, not m_l/m_r
    std::vector<float> m_l, m_r;
    std::vector<float> m_clipL, m_clipR;
    struct Snap {
        std::vector<float> l, r, slices;
        PatchKnob::engine::SamplerZoneInfo info;
        float start = 0.f, end = 1.f;
    };
    std::vector<Snap> m_undo, m_redo;
    std::vector<float> m_slices;       //!< normalised, kept sorted
    float m_start = 0.f, m_end = 1.f;  //!< region (the machine has no such field)
    ui::SamplerZone* m_model = nullptr; //!< authoritative keyzone-window model
    std::function<void()> m_onChange;   //!< republish that model to the engine
};

} // namespace sampleslot

#endif // PATCHKNOB_SDLUI_SAMPLER_INSTRUMENT_SLOT_H
