//----------------------------------------------------------------------------
//  sdlui/screen_recorder.h -- lossless screen capture with the mixer's audio.
//
//  Video: SDL_RenderReadPixels -> raw RGB piped straight to ffmpeg's stdin,
//  encoded with libx264rgb at qp 0.  That combination was chosen by measurement
//  on this machine, not by habit:
//
//    * there is NO usable hardware encoder here -- h264_amf fails to create its
//      component on this GPU, so AMF/NVENC/QSV are all out.
//    * libx264rgb takes RGB directly, so the frame never goes through a
//      colourspace conversion: the capture is pixel-exact and small text on the
//      UI stays crisp (4:2:0 visibly softens coloured text edges).
//    * -g 1 makes it ALL-INTRA, which is what an editor wants -- scrubbing does
//      not have to decode a group of pictures per seek.  It also measured
//      slightly FASTER than long-GOP, for ~1.8x the size.
//    * -threads 1 keeps it on one core (~16% of it, content-independent) so the
//      encoder can never contend with the audio thread.  Extra threads buy
//      nothing: the work is the same, just spread wider.
//
//  Audio comes from the engine's existing master capture tap rather than from
//  the sound card, so picture and sound cannot drift apart.
//
//  Container is MKV: it stays playable if the app dies mid-take, whereas an MP4
//  whose moov atom was never written is simply lost.  Transcode afterwards.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_SCREEN_RECORDER_H
#define PATCHKNOB_SDLUI_SCREEN_RECORDER_H

#include <SDL.h>

#include <cstdio>
#include <string>
#include <vector>

namespace pkrec {

//! Capture codecs, ordered as the settings dialog lists them.  All are lossless
//! and all-intra; they differ in how big they get and how widely NLEs take them.
enum Codec {
    // NOTE on the two RGB entries: libx264rgb encodes High 4:4:4 Predictive with
    // a planar-RGB pixel format.  That is pixel-exact and MLT/Kdenlive read it
    // natively, but VLC's H.264 decoder does not implement 4:4:4 RGB, so those
    // files will not open in the player most people reach for first.  They are
    // editor-only formats and are labelled as such.
    CODEC_X264RGB = 0,  //!< libx264rgb qp0 -- pixel-exact, ~2.8 GB/hr, EDITOR ONLY
    CODEC_X264RGB_LIGHT,//!< libx264rgb crf10 -- RGB-preserving, ~1 GB/hr, EDITOR ONLY
    CODEC_UTVIDEO,      //!< utvideo    -- ~39 GB/hr, imports essentially anywhere
    CODEC_FFV1,         //!< ffv1 lvl 3 -- smallest (~1.8 GB/hr) but ~3x the CPU
    CODEC_X264_LOSSY,   //!< libx264 crf 18 -- when size matters more than fidelity
    CODEC_YOUTUBE,      //!< H.264 4:2:0 + AAC MP4, upload-ready at ~1 GB/hr
    CODEC_COUNT
};

struct Settings {
    // Default to the format that PLAYS.  A screen recorder whose output will not
    // open in VLC is broken in practice however good it looks in an NLE, so the
    // out-of-the-box codec is ordinary 4:2:0 H.264; the lossless/RGB options are
    // still one click away for editing work.
    int         codec      = CODEC_X264_LOSSY;
    int         height     = 720;    //!< capture height; width follows the window
    int         fps        = 30;
    int         threads    = 1;      //!< 1 keeps the encoder off the audio thread
    bool        captureAudio = true; //!< mux the mixer output into the take
    std::string folder;              //!< "" == next to the project

    //! ffmpeg codec name for `codec`.
    const char* codec_name() const;
    //! Pixel format the encoder wants.
    const char* pix_fmt() const;
    //! Human label for the dialog.
    const char* codec_label() const;
    //! Rough size estimate shown in the dialog.
    const char* codec_note() const;
};

class ScreenRecorder {
public:
    ~ScreenRecorder() { stop(); }

    //! Is an ffmpeg we can drive actually present?  Resolved once, cached.
    static bool available();
    //! Path to the ffmpeg we found ("" when none).
    static const std::string& ffmpeg_path();

    bool recording() const { return m_proc != nullptr; }
    //! Seconds since the take started (0 when idle).
    double elapsed() const;
    //! Frames handed to the encoder so far.
    long long frames() const { return m_frames; }
    //! Last error, for the status line.
    const std::string& error() const { return m_err; }
    //! Where the finished take was written ("" until stop()).
    const std::string& output_path() const { return m_outPath; }

    //! Begin a take.  `dir` is where the .mkv lands.  Capture height is fixed at
    //! `targetH` (720 by default) and the width follows the window's aspect,
    //! rounded to an even number because H.264 requires it.
    bool start(SDL_Renderer* ren, int windowW, int windowH, const Settings& cfg);

    //! Call once per rendered frame; it self-throttles to the capture rate, so
    //! a 144 Hz UI still records 30 fps and the readback cost is bounded.
    void capture_frame(SDL_Renderer* ren);

    //! Finish: close the pipe, write the WAV, and mux them into one file.
    void stop();

private:
    void mux_audio();

    FILE*       m_proc   = nullptr;      // ffmpeg stdin
    //! Windows: the ffmpeg process handle.  ffmpeg is launched with
    //! CreateProcess rather than _popen, because _popen goes through cmd.exe and
    //! a GUI-subsystem app spawning cmd gets a real console window on screen --
    //! which, for a SCREEN recorder, then appears in the footage.
    void*       m_child  = nullptr;
    int         m_w = 0, m_h = 0;        // encode size
    int         m_srcW = 0, m_srcH = 0;  // window size
    int         m_fps = 30;
    Uint64      m_startTick = 0;
    // Rational frame clock.  Integer ticks-per-frame truncates on every frame
    // and steadily pulls the video ahead of the audio.
    Uint64      m_nextFrameNumber = 0;
    long long   m_frames = 0;
    Settings    m_cfg;
    std::string m_outPath, m_videoPath, m_wavPath, m_logPath, m_err;
    std::vector<unsigned char> m_rgb;    // readback scratch (source size)
    std::vector<unsigned char> m_scaled; // box-filtered to encode size
    std::vector<unsigned char> m_native; // native compositor BGRA scratch
    std::vector<unsigned char> m_lastFrame; // timestamp fill when the UI misses a frame
    SDL_Texture* m_target = nullptr;     // reserved for future render-to-texture
    bool        m_audio = false;         // engine capture actually started
    void*       m_nativeWindow = nullptr; // HWND on Windows; null => SDL fallback
};

} // namespace pkrec

#endif // PATCHKNOB_SDLUI_SCREEN_RECORDER_H
