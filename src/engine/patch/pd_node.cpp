//----------------------------------------------------------------------------
//  src/engine/patch/pd_node.cpp -- embedded Pure Data (libpd) patch node.
//----------------------------------------------------------------------------
#include "pd_node.h"

#include "z_libpd.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>

namespace PatchKnob { namespace engine { namespace patch {

// libpd_init() sets up the global Pd system exactly once (message thread).
static std::once_flag g_pdInitOnce;
static void pd_global_init() { std::call_once(g_pdInitOnce, [] { libpd_init(); }); }

// OS temp directory for the transient .pd libpd loads from (never the project dir).
static std::string pd_temp_dir() {
    const char* t = std::getenv("TEMP"); if (!t || !*t) t = std::getenv("TMP");
    if (!t || !*t) t = std::getenv("TMPDIR"); if (!t || !*t) t = ".";
    return std::string(t);
}

// Scan .pd TEXT for the highest adc~ / dac~ channel referenced so the node can size
// its audio in/out ports to match (bare "adc~"/"dac~" == 2 channels).  A simple text
// scan -- libpd itself resolves the actual routing.
static void scan_pd_channels(const std::string& content, int& inCh, int& outCh) {
    inCh = 2; outCh = 2;
    std::istringstream f(content);
    int maxIn = 0, maxOut = 0;
    auto scan1 = [](const std::string& line, const char* tok, int& mx) {
        const size_t tl = std::strlen(tok);
        size_t p = 0;
        while ((p = line.find(tok, p)) != std::string::npos) {
            const bool okpre  = (p == 0) || line[p - 1] == ' ';
            const size_t aft  = p + tl;
            const bool okpost = (aft >= line.size()) || line[aft] == ' ' ||
                                line[aft] == ';' || line[aft] == ',';
            p = aft;
            if (!okpre || !okpost) continue;
            // Collect the creation args up to the record terminator.
            std::istringstream ss(line.substr(aft));
            std::string t; std::vector<std::string> args;
            while (ss >> t) { if (t == ";" || t == ",") break; args.push_back(t); }
            int localMax = 0;
            if (!args.empty() && args[0] == "-m") {
                // multichannel: [adc~ -m <count> <start>] -> channels start..start+count-1
                int count = args.size() > 1 ? std::atoi(args[1].c_str()) : 2;
                int start = args.size() > 2 ? std::atoi(args[2].c_str()) : 1;
                if (count < 1) count = 1;
                if (start < 1) start = 1;
                localMax = start + count - 1;
            } else {
                // explicit channel list: [adc~ 3 4] -> channels 3 and 4
                for (const std::string& a : args) {
                    char* e = nullptr; long v = std::strtol(a.c_str(), &e, 10);
                    if (e && *e == '\0' && v > 0 && v < 256 && (int)v > localMax) localMax = (int)v;
                }
            }
            if (localMax == 0) localMax = 2;         // bare adc~ / dac~ == 2 channels
            if (localMax > mx) mx = localMax;
        }
    };
    std::string line;
    while (std::getline(f, line)) { scan1(line, "adc~", maxIn); scan1(line, "dac~", maxOut); }
    inCh  = maxIn  > 0 ? std::max(2, maxIn)  : 2;
    outCh = maxOut > 0 ? std::max(2, maxOut) : 2;
}

void PdNode::sendFloat(const std::string& recv, float v) {
    if (recv.empty() || recv == "empty") return;
    std::lock_guard<std::mutex> lk(msgMx_);
    msgQ_.push_back(PdMsg{recv, v, false});
}
void PdNode::sendBang(const std::string& recv) {
    if (recv.empty() || recv == "empty") return;
    std::lock_guard<std::mutex> lk(msgMx_);
    msgQ_.push_back(PdMsg{recv, 0.f, true});
}

// libpd's message hooks are global C callbacks with no user-data, so route them to
// the PdNode currently touching libpd (set around process()/loadPatchText on that
// thread).  They fire synchronously while libpd runs, i.e. inside process().
static thread_local PdNode* g_fbNode = nullptr;
static void pd_fb_bang (const char* recv)           { if (g_fbNode) g_fbNode->pushFb(recv, 0.f, true); }
static void pd_fb_float(const char* recv, float x)  { if (g_fbNode) g_fbNode->pushFb(recv, x,  false); }

void PdNode::pushFb(const char* recv, float v, bool bang) {
    if (!recv) return;
    std::lock_guard<std::mutex> lk(fbMx_);
    if (fbQ_.size() < 4096) fbQ_.push_back(GuiFb{recv, v, bang});   // bound queue
}
void PdNode::subscribeGui(const std::string& sendSym) {
    if (!pd_ || sendSym.empty() || sendSym == "empty") return;
    libpd_set_instance(pd_);
    if (void* p = libpd_bind(sendSym.c_str())) binds_.push_back(p);
}
void PdNode::clearGuiBinds() {
    if (!pd_) return;
    libpd_set_instance(pd_);
    for (void* p : binds_) libpd_unbind(p);
    binds_.clear();
}

// MIDI-out hooks: the patch's [noteout]/[ctlout]/... call these on the audio thread
// (structured), and [midiout] calls the raw byte hook -- all route to g_fbNode.
static void pd_mo_noteon (int ch,int p,int v)  { if(g_fbNode) g_fbNode->pushMidiOut((unsigned char)(0x90|(ch&15)),(unsigned char)p,(unsigned char)v); }
static void pd_mo_ctl    (int ch,int c,int v)  { if(g_fbNode) g_fbNode->pushMidiOut((unsigned char)(0xB0|(ch&15)),(unsigned char)c,(unsigned char)v); }
static void pd_mo_pgm    (int ch,int p)        { if(g_fbNode) g_fbNode->pushMidiOut((unsigned char)(0xC0|(ch&15)),(unsigned char)p,0); }
static void pd_mo_bend   (int ch,int b)        { const int v=b+8192; if(g_fbNode) g_fbNode->pushMidiOut((unsigned char)(0xE0|(ch&15)),(unsigned char)(v&0x7F),(unsigned char)((v>>7)&0x7F)); }
static void pd_mo_touch  (int ch,int v)        { if(g_fbNode) g_fbNode->pushMidiOut((unsigned char)(0xD0|(ch&15)),(unsigned char)v,0); }
static void pd_mo_ptouch (int ch,int p,int v)  { if(g_fbNode) g_fbNode->pushMidiOut((unsigned char)(0xA0|(ch&15)),(unsigned char)p,(unsigned char)v); }
static void pd_mo_byte   (int,int byte)        { if(g_fbNode) g_fbNode->pushMidiByte(byte); }

void PdNode::pushMidiOut(unsigned char status, unsigned char d1, unsigned char d2) {
    std::lock_guard<std::mutex> lk(midiMx_);
    if (midiOutStage_.size() < 4096) midiOutStage_.push_back(MidiEvent{ 0, status, d1, d2 });
}
// Reassemble [midiout]'s raw byte stream into complete channel-voice messages.
void PdNode::pushMidiByte(int b) {
    const unsigned char byte = (unsigned char)(b & 0xFF);
    if (byte >= 0xF8u) return;                       // realtime bytes: ignore
    if (byte & 0x80u) {                              // status byte
        if (byte >= 0xF0u) { rawStatus_ = 0; rawNeed_ = rawGot_ = 0; return; }  // sysex/common: skip
        rawStatus_ = byte;
        rawNeed_ = ((byte & 0xF0u) == 0xC0u || (byte & 0xF0u) == 0xD0u) ? 1 : 2;
        rawGot_  = 0;
        return;
    }
    if (!rawStatus_) return;                          // data with no running status
    if (rawGot_ < 2) rawData_[rawGot_++] = byte;
    if (rawGot_ >= rawNeed_) {
        pushMidiOut(rawStatus_, rawData_[0], rawNeed_ > 1 ? rawData_[1] : 0);
        rawGot_ = 0;                                  // running status persists
    }
}
int PdNode::drainGui(GuiFb* out, int cap) {
    if (!out || cap <= 0) return 0;
    std::lock_guard<std::mutex> lk(fbMx_);
    int n = (int)fbQ_.size(); if (n > cap) n = cap;
    for (int i = 0; i < n; ++i) out[i] = fbQ_[i];
    fbQ_.erase(fbQ_.begin(), fbQ_.begin() + n);
    return n;
}

static void pd_dsp_on(int on) {
    libpd_start_message(1);
    libpd_add_float(on ? 1.0f : 0.0f);
    libpd_finish_message("pd", "dsp");
}

PdNode::PdNode() {
    pd_global_init();
    pd_ = libpd_new_instance();
}

PdNode::~PdNode() {
    release();
    if (pd_) { libpd_free_instance(pd_); pd_ = nullptr; }
}

bool PdNode::prepare(double sampleRate, int maxBlock) {
    sr_    = sampleRate > 0 ? sampleRate : 48000.0;
    block_ = maxBlock > 0 ? maxBlock : 512;
    if (!pd_) return false;
    libpd_set_instance(pd_);
    pdBlock_ = libpd_blocksize();               // 64
    in_.assign((size_t)block_ * (size_t)std::max(1, audioInCh_), 0.0f);
    out_.assign((size_t)block_ * (size_t)std::max(1, audioOutCh_), 0.0f);
    libpd_init_audio(audioInCh_, audioOutCh_, (int)sr_);
    libpd_set_banghook(pd_fb_bang);              // GUI feedback hooks (this instance)
    libpd_set_floathook(pd_fb_float);
    libpd_set_noteonhook(pd_mo_noteon);          // MIDI-out hooks (this instance)
    libpd_set_controlchangehook(pd_mo_ctl);
    libpd_set_programchangehook(pd_mo_pgm);
    libpd_set_pitchbendhook(pd_mo_bend);
    libpd_set_aftertouchhook(pd_mo_touch);
    libpd_set_polyaftertouchhook(pd_mo_ptouch);
    libpd_set_midibytehook(pd_mo_byte);
    pd_dsp_on(1);
    return true;
}

void PdNode::release() {
    if (pd_ && file_) { libpd_set_instance(pd_); libpd_closefile(file_); file_ = nullptr; }
}

bool PdNode::loadPatchText(const std::string& text) {
    if (!pd_) return false;
    patchText_ = text;                          // the source of truth (saved w/ project)
    libpd_set_instance(pd_);
    g_fbNode = this;                            // route loadbang-time GUI feedback here
    for (void* p : binds_) libpd_unbind(p);     // drop the old patch's GUI subscriptions
    binds_.clear();
    if (file_) { libpd_closefile(file_); file_ = nullptr; }
    // Size the node's audio ports to the patch's adc~/dac~ channels, then re-init
    // libpd's audio to that width so multichannel adc~/dac~ route to the ports.
    int nin = 2, nout = 2; scan_pd_channels(text, nin, nout);
    if (nin != audioInCh_ || nout != audioOutCh_) {
        audioInCh_ = nin; audioOutCh_ = nout;
        in_.assign((size_t)block_ * (size_t)std::max(1, audioInCh_), 0.0f);
        out_.assign((size_t)block_ * (size_t)std::max(1, audioOutCh_), 0.0f);
        libpd_init_audio(audioInCh_, audioOutCh_, (int)sr_);
    }
    // libpd opens from a FILE, so materialise a transient temp .pd, open it, then
    // delete it -- the canonical patch stays in memory (nothing lands in the project).
    static std::atomic<unsigned long long> s_seq{0};
    const unsigned long long uid =
        ((unsigned long long)std::time(nullptr) << 20) ^
        (s_seq.fetch_add(1) + (unsigned long long)(uintptr_t)this);
    const std::string dir  = pd_temp_dir();
    const std::string name = "pkpd_" + std::to_string(uid) + ".pd";
    const std::string full = dir + "/" + name;
    { std::ofstream out(full.c_str(), std::ios::binary | std::ios::trunc); if (out) out << text; }
    file_ = libpd_openfile(name.c_str(), dir.c_str());
    std::remove(full.c_str());                  // patch is instantiated; drop the temp file
    const int on = dspOn_.load(std::memory_order_relaxed);
    pd_dsp_on(on); dspApplied_ = on;
    return file_ != nullptr;
}

// Import an EXTERNAL .pd file: read it into memory, then load from text.
bool PdNode::loadPatch(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    patchPath_ = path;
    return loadPatchText(text);
}

void PdNode::process(const NodeProcessContext& ctx) {
    const int n = ctx.nframes;
    for (int b = 0; b < ctx.numAudioOut; ++b)            // clear every out port
        for (int c = 0; c < ctx.audioOut[b].channels; ++c)
            std::memset(ctx.audioOut[b].chans[c], 0, sizeof(float) * (size_t)n);
    for (int b = 0; b < ctx.numMidiOut; ++b) ctx.midiOut[b].count = 0;   // clear MIDI-out plug
    if (!pd_) return;

    libpd_set_instance(pd_);
    g_fbNode = this;              // GUI + MIDI-out hooks fire here during this block
    { std::lock_guard<std::mutex> lk(midiMx_); midiOutStage_.clear(); }

    // --- deliver queued UI control messages (toggles / sliders / bangs) ------
    // Sent from the editor's RUN mode to a GUI atom's receive symbol: the atom
    // updates AND outputs, so downstream objects ([metro], ...) react live.
    {
        std::vector<PdMsg> pending;
        { std::lock_guard<std::mutex> lk(msgMx_); pending.swap(msgQ_); }
        for (const PdMsg& m : pending) {
            if (m.bang) libpd_bang(m.recv.c_str());
            else        libpd_float(m.recv.c_str(), m.val);
        }
    }

    // --- apply the DSP switch if the UI toggled it --------------------------
    const int wantDsp = dspOn_.load(std::memory_order_relaxed);
    if (wantDsp != dspApplied_) { pd_dsp_on(wantDsp); dspApplied_ = wantDsp; }

    // --- feed MIDI into the patch ([notein]/[ctlin]/...) --------------------
    if (ctx.numMidiIn > 0) {
        const MidiBuffer& mb = ctx.midiIn[0];
        const int filt = midiChannel_.load(std::memory_order_relaxed);
        for (int i = 0; i < mb.count; ++i) {
            const MidiEvent& m = mb.ev[i];
            const unsigned char hi = m.status & 0xF0u;
            const int ch = m.status & 0x0Fu;
            // channel filter: when assigned to a clip's channel, ignore others
            if (filt >= 0 && hi >= 0x80u && hi < 0xF0u && ch != filt) continue;
            // STRUCTURED injection drives the parsed objects ([notein]/[ctlin]/...);
            // libpd's raw byte path (inmidi_byte) does NOT parse to them.
            switch (hi) {
                case 0x90: libpd_noteon(ch, m.data1, m.data2); break;      // note on (vel 0 = off)
                case 0x80: libpd_noteon(ch, m.data1, 0); break;            // note off
                case 0xA0: libpd_polyaftertouch(ch, m.data1, m.data2); break;
                case 0xB0: libpd_controlchange(ch, m.data1, m.data2); break;
                case 0xC0: libpd_programchange(ch, m.data1); break;
                case 0xD0: libpd_aftertouch(ch, m.data1); break;
                case 0xE0: libpd_pitchbend(ch, ((m.data2 << 7) | m.data1) - 8192); break;
                default: break;
            }
            // ALSO feed raw bytes so [midiin] (raw MIDI stream) receives it too.
            libpd_midibyte(0, m.status);
            libpd_midibyte(0, m.data1);
            if (hi != 0xC0u && hi != 0xD0u) libpd_midibyte(0, m.data2);   // 1-data msgs: no data2
        }
    }

    // --- run libpd in 64-sample ticks; interleave <-> planar ----------------
    const int ticks = (pdBlock_ > 0) ? (n / pdBlock_) : 0;
    const int nn = ticks * pdBlock_;            // frames processed (multiple of 64)
    if (nn > 0) {
        const int inCh  = std::max(1, audioInCh_);
        const int outCh = std::max(1, audioOutCh_);

        // Interleave the node's stereo audio-IN ports into libpd's input: adc~ channel
        // c comes from in-port (c/2), lane (c&1).
        for (int i = 0; i < nn; ++i)
            for (int c = 0; c < inCh; ++c) {
                float v = 0.f;
                const int bus = c / 2, lane = c & 1;
                if (bus < ctx.numAudioIn) {
                    const AudioBus& ai = ctx.audioIn[bus];
                    const int lc = (lane < ai.channels) ? lane : ai.channels - 1;
                    if (lc >= 0) v = ai.chans[lc][i];
                }
                in_[(size_t)i * inCh + c] = v;
            }

        libpd_process_float(ticks, in_.data(), out_.data());

        // De-interleave libpd's output to the stereo audio-OUT ports: dac~ channel c
        // goes to out-port (c/2), lane (c&1).
        for (int b = 0; b < ctx.numAudioOut; ++b) {
            AudioBus& ao = ctx.audioOut[b];
            for (int lane = 0; lane < ao.channels; ++lane) {
                const int c = b * 2 + lane;
                if (c >= outCh) continue;
                for (int i = 0; i < nn && i < n; ++i)
                    ao.chans[lane][i] = out_[(size_t)i * outCh + c];
            }
        }
    }

    // --- emit MIDI the patch produced ([noteout]/[ctlout]/[midiout]/...) -----
    // Staged by the libpd MIDI hooks during this block; flush to the MIDI-out port.
    if (ctx.numMidiOut > 0) {
        MidiBuffer& mo = ctx.midiOut[0];
        std::lock_guard<std::mutex> lk(midiMx_);
        int k = 0;
        for (const MidiEvent& e : midiOutStage_) { if (k >= mo.capacity) break; mo.ev[k++] = e; }
        mo.count = k;
        midiOutStage_.clear();
    }
}

}}} // namespace PatchKnob::engine::patch
