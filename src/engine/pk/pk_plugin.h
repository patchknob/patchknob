//----------------------------------------------------------------------------
//  src/engine/pk/pk_plugin.h -- the PatchKnob native plugin ABI (plugin side).
//
//  This is the interface a .pkp module implements.  It is PLAIN C on purpose:
//  a module is a separately-compiled shared object, possibly built by a
//  different compiler (MSVC vs MinGW vs GCC) with a different C++ standard
//  library, so no C++ type may cross this boundary -- no std::string, no
//  std::vector, no virtual dispatch, no exceptions.  Everything is fixed-width
//  integers, plain structs and function pointers.
//
//  WHY THIS EXISTS.  The two built-in effects shipped today declare themselves
//  as PluginFormat::VST2 with a "builtin://" path (src/engine/native/
//  native_effects.cpp) -- our own code masquerading as a VST so it can ride the
//  VST descriptor path.  That costs us the things a first-party format should
//  give for free: real typed parameters (VST2 has only normalised floats),
//  a GUI that matches the host theme without each plugin re-implementing a
//  knob, and a state blob that can be versioned instead of a bag of floats.
//
//  LIFECYCLE (all on the MESSAGE thread except process()):
//      pk_module_entry(PK_ABI_VERSION)      -> const PkModule*
//      module->describe(i)                  -> const PkPluginDesc*
//      module->create(desc->uid, host)      -> PkPlugin*
//      plugin->prepare(sr, max_block)
//      plugin->activate(1)
//        ... plugin->process(...)           <- AUDIO thread, realtime
//      plugin->activate(0)
//      plugin->destroy()
//
//  REALTIME CONTRACT.  process() must not allocate, free, lock, block, do file
//  or network I/O, or call any host service other than those documented
//  RT-safe in pk_host.h.  Everything else -- loading a sample, opening a
//  soundfont, resizing the editor -- happens on the message thread.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_PK_PLUGIN_H
#define PATCHKNOB_PK_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//----------------------------------------------------------------------------
//  ABI version.  MAJOR changes break binary compatibility; the host refuses to
//  load a module whose major differs from its own.  MINOR is additive only:
//  new fields are appended to the END of structs and new entry points get their
//  own slot, so an older module keeps working against a newer host.
//----------------------------------------------------------------------------
#define PK_ABI_MAJOR 1u
#define PK_ABI_MINOR 0u
#define PK_ABI_VERSION ((PK_ABI_MAJOR << 16) | PK_ABI_MINOR)
#define PK_ABI_MAJOR_OF(v) ((v) >> 16)

#if defined(_WIN32)
#  define PK_EXPORT __declspec(dllexport)
#else
#  define PK_EXPORT __attribute__((visibility("default")))
#endif

//----------------------------------------------------------------------------
//  Descriptor
//----------------------------------------------------------------------------
enum PkPluginFlags {
    PK_INSTRUMENT   = 1u << 0,  //!< emits audio in response to MIDI
    PK_EFFECT       = 1u << 1,  //!< processes audio in -> audio out
    PK_CUSTOM_UI    = 1u << 2,  //!< implements ui_draw(); without it the host
                                //!< builds an editor from the parameter list
    PK_MIDI_OUT     = 1u << 3,  //!< emits MIDI (arpeggiators, sequencers)
    PK_STEREO_PAIRS = 1u << 4,  //!< channel count is always even (L/R pairs)
    PK_PER_COLUMN   = 1u << 5   //!< honours tracker note-columns: allocates
                                //!< voices per column and accepts column-scoped
                                //!< parameter changes.  Without it the host
                                //!< collapses every column onto the global
                                //!< value rather than silently dropping edits.
};

//! Voice-allocation policy for ONE tracker note-column.  The tracker's whole
//! idiom -- a column is a monophonic "track" you type notes down -- only works
//! if the plugin allocates per column: two columns playing the same pitch must
//! be two voices, and a note-off in column 3 must not steal column 1's voice.
enum PkColumnMode {
    PK_COLUMN_POLY   = 0,  //!< unlimited voices within the column (default)
    PK_COLUMN_MONO   = 1,  //!< one voice; a new note cuts the previous one
    PK_COLUMN_LEGATO = 2   //!< one voice; a new note re-pitches without retrigger
};

//! A plugin class inside a module.  One .pkp may expose several (a soundfont
//! player and its drum-mapped variant, say), each with its own stable `uid`.
typedef struct PkPluginDesc {
    uint32_t    abi;         //!< PK_ABI_VERSION the module was built against
    const char* uid;         //!< STABLE identity, reverse-dns ("pk.fluidsynth").
                             //!< Saved in projects -- never change it, ever.
    const char* name;        //!< display name ("FluidSynth")
    const char* vendor;
    const char* version;     //!< free-form ("2.6.0")
    uint32_t    flags;       //!< PkPluginFlags
    uint16_t    audio_in;    //!< main-bus input channels  (0 for instruments)
    uint16_t    audio_out;   //!< main-bus output channels
    uint16_t    midi_in;     //!< MIDI input ports  (0 or 1 today)
    uint16_t    midi_out;
} PkPluginDesc;

//----------------------------------------------------------------------------
//  Parameters
//
//  Values cross the ABI in REAL UNITS (hertz, dB, semitones), not normalised
//  0..1.  A normalised-only interface is the single worst thing about hosting
//  VST2: every host has to guess how to display a value, and every plugin has
//  to invent its own mapping.  The host normalises internally for automation
//  using min/max/curve, so nothing is lost.
//----------------------------------------------------------------------------
enum PkParamType {
    PK_PARAM_FLOAT = 0,
    PK_PARAM_INT   = 1,
    PK_PARAM_BOOL  = 2,
    PK_PARAM_ENUM  = 3   //!< integer index into `names`
};

enum PkParamFlags {
    PK_PARAM_AUTOMATABLE = 1u << 0,
    PK_PARAM_LOGARITHMIC = 1u << 1,  //!< frequency-like; host uses a log taper
    PK_PARAM_READ_ONLY   = 1u << 2,  //!< meters/readouts the plugin publishes
    PK_PARAM_HIDDEN      = 1u << 3,  //!< state-only; omit from the auto-editor
    PK_PARAM_PER_COLUMN  = 1u << 4   //!< may be scoped to one tracker column.
                                     //!< The tracker's FX picker lists exactly
                                     //!< these; anything without the flag is
                                     //!< offered as a global-only effect, so
                                     //!< the UI can never promise per-column
                                     //!< control a plugin will not honour.
};

typedef struct PkParamInfo {
    uint32_t     id;          //!< STABLE id, saved in automation. Never reuse.
    const char*  name;        //!< "Reverb Room Size"
    const char*  unit;        //!< "dB", "Hz", "%" or "" -- shown by the host
    uint32_t     type;        //!< PkParamType
    uint32_t     flags;       //!< PkParamFlags
    double       min_value;
    double       max_value;
    double       default_value;
    double       step;        //!< 0 = continuous
    const char* const* names; //!< PK_PARAM_ENUM: NULL-terminated label array
} PkParamInfo;

//! One automation point, sample-accurate within the block.
typedef struct PkParamChange {
    uint32_t id;
    int32_t  frame_offset;
    double   value;           //!< real units, same as get/set_param
    //! Tracker note-column this change applies to, or -1 for the instrument as
    //! a whole.  A column-scoped change on a parameter WITHOUT
    //! PK_PARAM_PER_COLUMN is applied globally -- defined behaviour, so a host
    //! that scopes optimistically degrades instead of dropping the edit.
    int8_t   column;
} PkParamChange;

//----------------------------------------------------------------------------
//  MIDI + transport + the process block
//----------------------------------------------------------------------------
typedef struct PkMidiEvent {
    int32_t frame_offset;     //!< 0..nframes-1
    uint8_t status;           //!< 0x90|ch etc.
    uint8_t data1;
    uint8_t data2;
    //! Tracker note-column (0..N-1), or -1 when untagged (live MIDI input,
    //! piano-roll edits, imported files).
    //!
    //! The column travels ON THE EVENT rather than being stamped ahead of time
    //! by the UI.  The built-in sampler learned this the hard way: it used to
    //! keep a 128-entry pitch-keyed table of "which column owns this note",
    //! so the same pitch played from two columns collapsed onto one column
    //! (last writer won) and the two notes then fought over a single voice.
    //! Carried here, a note-on and its note-off identify their voice exactly,
    //! whatever else shares the pitch.
    int8_t  column;
} PkMidiEvent;

typedef struct PkTransport {
    double  tempo_bpm;
    double  ppq_position;     //!< quarter notes since song start
    int64_t sample_position;
    int32_t numerator, denominator;
    int32_t is_playing;
} PkTransport;

//! Buffers for one process() call.  `audio_in`/`audio_out` are arrays of
//! channel pointers; NEVER index past in_channels/out_channels, whatever the
//! descriptor claims -- the host may run a plugin on a narrower bus.
typedef struct PkProcess {
    const float* const*  audio_in;
    float* const*        audio_out;
    int32_t              in_channels;
    int32_t              out_channels;
    int32_t              nframes;
    const PkMidiEvent*   midi_in;
    int32_t              midi_in_count;
    PkMidiEvent*         midi_out;      //!< host-owned, may be NULL
    int32_t              midi_out_capacity;
    int32_t*             midi_out_count;
    const PkParamChange* param_changes;
    int32_t              param_change_count;
    PkTransport          transport;
    double               sample_rate;
} PkProcess;

//----------------------------------------------------------------------------
//  UI
//
//  A plugin never touches the renderer, SDL, or a window handle.  It draws by
//  calling the widget table the host hands it (see pk_host.h) inside ui_draw().
//  That is what makes a dynamically-loaded module safe: no shared SDL state, no
//  duplicated widget code, and adding a widget to the host never invalidates an
//  already-built .pkp.
//----------------------------------------------------------------------------
typedef struct PkRect { int32_t x, y, w, h; } PkRect;

struct PkHost;      //!< pk_host.h
struct PkUiFrame;   //!< opaque per-frame draw context, host-owned

//----------------------------------------------------------------------------
//  The plugin instance
//----------------------------------------------------------------------------
typedef struct PkPlugin {
    void* self;   //!< the module's own instance pointer, opaque to the host

    // --- lifecycle (message thread) ---
    int32_t (*prepare)(void* self, double sample_rate, int32_t max_block);
    void    (*activate)(void* self, int32_t on);
    void    (*destroy)(void* self);

    // --- realtime (audio thread) ---
    void    (*process)(void* self, const PkProcess* blk);

    // --- parameters (message thread; automation arrives via PkProcess) ---
    int32_t            (*param_count)(void* self);
    const PkParamInfo* (*param_info)(void* self, int32_t index);
    double             (*get_param)(void* self, uint32_t id);
    void               (*set_param)(void* self, uint32_t id, double value);
    //! Optional pretty-printer ("-6.0 dB", "Hall").  NULL = host formats it
    //! from unit + step.  Returns bytes written (excluding the terminator).
    int32_t            (*format_param)(void* self, uint32_t id, double value,
                                       char* buf, int32_t cap);

    // --- state (message thread) ---
    //! Write at most `cap` bytes and return the size REQUIRED.  The host calls
    //! once with cap==0 to size the buffer, then again to fill it -- so a
    //! plugin whose state grew between calls can never overflow.
    size_t  (*save_state)(void* self, void* buf, size_t cap);
    int32_t (*load_state)(void* self, const void* buf, size_t size);

    // --- custom editor (message thread; only with PK_CUSTOM_UI) ---
    void    (*ui_size)(void* self, int32_t* w, int32_t* h);
    void    (*ui_draw)(void* self, struct PkUiFrame* frame, PkRect area);
    //! Called when the editor opens/closes so the plugin can allocate or drop
    //! whatever its panel needs (a waveform cache, a preset list).
    void    (*ui_open)(void* self);
    void    (*ui_close)(void* self);

    // --- tracker note-columns (message thread; only with PK_PER_COLUMN) ------
    //! Set one column's voice-allocation policy (PkColumnMode).  `column` < 0
    //! sets the default applied to every column that has no policy of its own.
    void    (*set_column_mode)(void* self, int32_t column, uint32_t mode);
    //! Voices currently sounding in `column`, or the instrument total when
    //! `column` < 0.  Drives the tracker's per-column activity indicator and
    //! the editor's voice meter.  Must be cheap: the UI polls it per frame.
    int32_t (*voice_count)(void* self, int32_t column);
    //! Silence a column (`column` < 0 = all).  The host calls this on transport
    //! stop, on a pattern jump, and when a column is muted -- releasing notes
    //! individually cannot express "this column is gone now".
    void    (*all_notes_off)(void* self, int32_t column);
} PkPlugin;

//----------------------------------------------------------------------------
//  The module entry point
//
//  ONE exported symbol.  The host passes its own ABI version so a module can
//  refuse politely (return NULL) instead of being loaded and crashing later.
//----------------------------------------------------------------------------
typedef struct PkModule {
    uint32_t abi;
    int32_t             (*count)(void);
    const PkPluginDesc* (*describe)(int32_t index);
    //! Instantiate by uid.  `host` stays valid for the plugin's lifetime.
    PkPlugin*           (*create)(const char* uid, const struct PkHost* host);
} PkModule;

typedef const PkModule* (*PkModuleEntryFn)(uint32_t host_abi);

//! THE exported symbol every .pkp must provide.
PK_EXPORT const PkModule* pk_module_entry(uint32_t host_abi);
#define PK_MODULE_ENTRY_SYMBOL "pk_module_entry"

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // PATCHKNOB_PK_PLUGIN_H
