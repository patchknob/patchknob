#include "screen_recorder.h"
#include "audio_app.h"
#include <cmath>          // std::lround -- SDL2 pulled this in transitively

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <atomic>
#include <iomanip>
#ifdef _WIN32
#include <windows.h>          // SearchPathA / GetFileAttributesA (no shell)
#ifndef PATCHKNOB_SDL3
#include <SDL_syswm.h>
#endif
#include <io.h>               // _open_osfhandle
#include <fcntl.h>            // _O_WRONLY / _O_BINARY
#else
#include <unistd.h>           // access()
#endif

namespace fs = std::filesystem;

namespace pkrec {

#ifdef _WIN32
static HWND recorder_hwnd(SDL_Renderer* ren)
{
    SDL_Window* window = ren ? SDL_RenderGetWindow(ren) : nullptr;
    if (!window) return nullptr;
#ifdef PATCHKNOB_SDL3
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    return (HWND)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm)) return nullptr;
    return wm.subsystem == SDL_SYSWM_WINDOWS ? wm.info.win.window : nullptr;
#endif
}
#endif

// ---------------------------------------------------------------------------
//  Settings
// ---------------------------------------------------------------------------
const char* Settings::codec_name() const {
    switch (codec) {
        case CODEC_X264RGB_LIGHT:return "libx264rgb";
        case CODEC_UTVIDEO:    return "utvideo";
        case CODEC_FFV1:       return "ffv1";
        case CODEC_X264_LOSSY: return "libx264";
        case CODEC_YOUTUBE:    return "libx264";
        default:               return "libx264rgb";
    }
}
const char* Settings::pix_fmt() const {
    switch (codec) {
        case CODEC_X264RGB_LIGHT:return "gbrp";
        case CODEC_UTVIDEO:    return "gbrp";
        case CODEC_FFV1:       return "rgb24";
        case CODEC_X264_LOSSY: return "yuv420p";
        case CODEC_YOUTUBE:    return "yuv420p";
        default:               return "rgb24";
    }
}
const char* Settings::codec_label() const {
    switch (codec) {
        case CODEC_X264RGB_LIGHT:return "x264 RGB light (editor only)";
        case CODEC_UTVIDEO:    return "Ut Video (lossless)";
        case CODEC_FFV1:       return "FFV1 (lossless, plays in VLC)";
        case CODEC_X264_LOSSY: return "H.264 (plays everywhere)";
        case CODEC_YOUTUBE:    return "YouTube MP4 (~1 GB/hr)";
        default:               return "x264 RGB lossless (editor only)";
    }
}
const char* Settings::codec_note() const {
    // Measured on this machine at 720p30: see the header for the full table.
    switch (codec) {
        case CODEC_X264RGB_LIGHT:return "~1 GB/hr - crisp RGB; will NOT open in VLC";
        case CODEC_UTVIDEO:    return "~39 GB/hr - imports anywhere";
        case CODEC_FFV1:       return "~1.8 GB/hr - lossless AND playable; 3x CPU";
        case CODEC_X264_LOSSY: return "~0.15 GB/hr - opens in any player";
        case CODEC_YOUTUBE:    return "H.264 + AAC - ready to upload";
        default:               return "~2.8 GB/hr - pixel exact; will NOT open in VLC";
    }
}

// ---------------------------------------------------------------------------
//  ffmpeg discovery
// ---------------------------------------------------------------------------
namespace {
std::string probe_ffmpeg() {
    // LOOK for ffmpeg; do not RUN it.
    //
    // This used to shell out to `ffmpeg -version` through std::system().  From a
    // GUI-subsystem process that spawns cmd.exe, which flashes a real console
    // window on screen -- and since the transport bar asks whether recording is
    // available while drawing, it happened during the first frame: a console
    // popping up every time the app started.
    //
    // Presence on disk is all the answer the greyed-out REC chip needs; if the
    // binary is there but broken, start() reports that when it is actually used.
#ifdef _WIN32
    // PATH first, resolved by the loader rather than by a shell.
    {
        char found[MAX_PATH] = {};
        if (SearchPathA(nullptr, "ffmpeg.exe", nullptr,
                        (DWORD)sizeof found, found, nullptr) > 0)
            return std::string(found);
    }
    const char* candidates[] = {
        "C:/msys64/mingw64/bin/ffmpeg.exe",
        "C:/Program Files/ffmpeg/bin/ffmpeg.exe",
    };
    for (const char* c : candidates) {
        const DWORD a = GetFileAttributesA(c);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
            return c;
    }
#else
    const char* candidates[] = { "/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg" };
    for (const char* c : candidates)
        if (::access(c, X_OK) == 0) return c;
#endif
    return std::string();
}
std::string g_ffmpeg;
bool        g_probed = false;

#ifdef _WIN32
// Launch ffmpeg WITHOUT a shell.
//
// _popen and std::system both run the command through `cmd.exe /c`, which for a
// GUI-subsystem process means Windows creates a console window for the child.
// For a screen recorder that console is not merely ugly -- it lands in the
// recording.  It also means the command string has to survive cmd's quoting
// rules, and any `2>file` in it is SHELL syntax that only cmd understands.
//
// CreateProcess takes the arguments directly and stderr is redirected with a
// real handle, so neither problem exists.  Returns the process handle, and
// (when stdinPipe is non-null) a FILE* on the child's stdin so existing
// fwrite/setvbuf code keeps working unchanged.
HANDLE spawn_ffmpeg(const std::string& args, const std::string& logPath,
                    FILE** stdinPipe) {
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;

    HANDLE log = CreateFileA(logPath.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) log = nullptr;

    HANDLE rd = nullptr, wr = nullptr;
    if (stdinPipe) {
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) {
            if (log) CloseHandle(log);
            return nullptr;
        }
        // The child must NOT inherit our write end, or it never sees EOF.
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si{}; si.cb = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = rd ? rd : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log ? log : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = log ? log : GetStdHandle(STD_ERROR_HANDLE);

    std::vector<char> line(args.begin(), args.end());
    line.push_back('\0');

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr,
                                   TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi);
    if (rd)  CloseHandle(rd);          // the child owns it now
    if (log) CloseHandle(log);
    if (!ok) {
        if (wr) CloseHandle(wr);
        return nullptr;
    }
    CloseHandle(pi.hThread);

    if (stdinPipe) {
        const int fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_BINARY);
        *stdinPipe = (fd >= 0) ? _fdopen(fd, "wb") : nullptr;
        if (!*stdinPipe) { CloseHandle(wr); CloseHandle(pi.hProcess); return nullptr; }
    }
    return pi.hProcess;
}
#endif

std::string timestamp_name() {
    static std::atomic<unsigned> sequence{0};
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[96];
    // MM-DD-YY-HHMMSS, so takes sort by session and read at a glance.
    std::strftime(buf, sizeof buf, "%m-%d-%y-%H%M%S", &tmv);
    std::ostringstream name;
    name << buf << '-' << std::setw(3) << std::setfill('0')
         << (sequence.fetch_add(1, std::memory_order_relaxed) % 1000);
    return name.str();
}
} // namespace

const std::string& ScreenRecorder::ffmpeg_path() {
    if (!g_probed) { g_ffmpeg = probe_ffmpeg(); g_probed = true; }
    return g_ffmpeg;
}
bool ScreenRecorder::available() { return !ffmpeg_path().empty(); }

double ScreenRecorder::elapsed() const {
    if (!m_proc || m_startTick == 0) return 0.0;
    return (double)(SDL_GetPerformanceCounter() - m_startTick)
         / (double)SDL_GetPerformanceFrequency();
}

// ---------------------------------------------------------------------------
//  start / frame / stop
// ---------------------------------------------------------------------------
bool ScreenRecorder::start(SDL_Renderer* ren, int windowW, int windowH,
                           const Settings& cfg) {
    if (m_proc) return true;
    m_err.clear();
    if (!ren || windowW < 16 || windowH < 16) { m_err = "no window to capture"; return false; }
    if (!available()) { m_err = "ffmpeg not found"; return false; }

    m_cfg  = cfg;
#ifdef _WIN32
    m_nativeWindow = recorder_hwnd(ren);
#endif
    m_srcW = windowW; m_srcH = windowH;
    m_fps  = std::max(1, cfg.fps);

    // Height is the setting; width follows the window's aspect.  BOTH must be
    // even -- H.264 rejects odd dimensions.
    m_h = std::max(16, cfg.height) & ~1;
    m_w = (int)std::lround((double)windowW * (double)m_h / (double)windowH) & ~1;
    if (m_w < 16) m_w = 16;

    fs::path dir = cfg.folder.empty() ? fs::current_path() : fs::path(cfg.folder);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        m_err = "recording folder is not writable: " + dir.string();
        return false;
    }
    const std::string base = timestamp_name();
    const bool youtube=m_cfg.codec==CODEC_YOUTUBE;
    m_outPath   = (dir / (base + (youtube?".mp4":".mkv"))).string();
    m_videoPath = (dir / (base + (youtube?".video.mp4":".video.mkv"))).string();
    m_wavPath   = (dir / (base + ".wav")).string();
    m_logPath   = (dir / (base + ".ffmpeg.log")).string();

    std::ostringstream command;
    command << '"' << ffmpeg_path() << "\" -hide_banner -loglevel error -y "
            << "-f rawvideo -pixel_format rgb24 -video_size " << m_w << 'x' << m_h
            << " -framerate " << m_fps << " -i pipe:0 -map 0:v:0 -an -c:v "
            << m_cfg.codec_name() << ' ';
    command << (
        // qp 0 == mathematically lossless; -g 1 == all-intra so an editor can
        // scrub without decoding a group of pictures per seek.
        m_cfg.codec == CODEC_X264RGB   ? "-preset ultrafast -qp 0 -g 1" :
        m_cfg.codec == CODEC_X264RGB_LIGHT ? "-preset ultrafast -crf 10 -g 30" :
        // -profile:v high + yuv420p is the combination every player decodes;
        // ultrafast keeps a DAW's CPU free.  crf 16 holds up on screen text,
        // where 4:2:0 chroma is the weak point rather than the bitrate.
        m_cfg.codec == CODEC_X264_LOSSY? "-preset ultrafast -crf 16 -g 30 -profile:v high" :
        m_cfg.codec == CODEC_YOUTUBE   ? "-preset ultrafast -b:v 2000k -maxrate 2500k -bufsize 5000k -g 60" :
        m_cfg.codec == CODEC_FFV1      ? "-level 3 -g 1" : "");
    const bool directYoutube=youtube;
    command << " -pix_fmt " << m_cfg.pix_fmt() << " -threads "
            << std::max(1,m_cfg.threads)
            << (directYoutube?" -movflags +faststart -f mp4 \"":" -f matroska \"")
            << (m_cfg.captureAudio?m_videoPath:m_outPath) << '"';
    // NOTE: no `2>logfile` here -- that is shell syntax.  stderr is redirected
    // with a real handle in spawn_ffmpeg (POSIX keeps the shell form below).

#ifdef _WIN32
    m_child = spawn_ffmpeg(command.str(), m_logPath, &m_proc);
    if (!m_child) { m_err = "could not launch ffmpeg"; return false; }
#else
    m_proc = popen((command.str()+" 2>\""+m_logPath+"\"").c_str(), "w");
#endif
    if (!m_proc) { m_err = "could not launch ffmpeg"; return false; }
    std::setvbuf(m_proc,nullptr,_IOFBF,1024*1024);

    const size_t srcPitch = ((size_t)m_srcW * 3 + 3) & ~size_t(3);
    m_rgb.assign(srcPitch * (size_t)m_srcH, 0);
    m_scaled.assign((size_t)m_w * m_h * 3, 0);
    m_lastFrame.clear();
    m_frames = 0;
    m_startTick = SDL_GetPerformanceCounter();
    m_nextFrameNumber = 0;

    // Audio straight off the master tap, not the sound card: the two can then
    // never drift.  Failure here is not fatal -- a silent take beats no take.
    m_audio = false;
    if (m_cfg.captureAudio)
        // Ten minutes is a bounded safety ceiling for the legacy in-memory
        // master tap.  The old one-hour request reserved ~1.38 GiB at start.
        // Video continues if this fills; muxing no longer truncates to audio.
        m_audio = PatchKnob::app::audio_app_capture_begin(10.0 * 60.0);
    return true;
}

void ScreenRecorder::capture_frame(SDL_Renderer* ren) {
    if (!m_proc || !ren) return;

    // Self-throttle: the UI may redraw far faster than the capture rate, and
    // RenderReadPixels is a GPU->CPU stall we do not want to pay per redraw.
    const Uint64 now = SDL_GetPerformanceCounter();
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 due = ((now - m_startTick) * (Uint64)m_fps) / freq + 1;
    if (m_nextFrameNumber >= due) return;

    int currentW=0,currentH=0;
#ifdef _WIN32
    if (m_nativeWindow) {
        RECT cr{};
        if (GetClientRect((HWND)m_nativeWindow, &cr)) {
            currentW = cr.right - cr.left;
            currentH = cr.bottom - cr.top;
        }
    }
#endif
    if (currentW < 1 || currentH < 1)
    if (SDL_GetRendererOutputSize(ren,&currentW,&currentH)!=0 || currentW<1 || currentH<1) {
        m_err = std::string("cannot query capture surface: ") + SDL_GetError();
        return;
    }
    if(currentW!=m_srcW || currentH!=m_srcH) {
        m_srcW=currentW;m_srcH=currentH;
        const size_t pitch=((size_t)m_srcW*3+3)&~size_t(3);
        m_rgb.assign(pitch*(size_t)m_srcH,0);
    }
    const size_t srcPitch=((size_t)m_srcW*3+3)&~size_t(3);

    bool captured = false;
#ifdef _WIN32
    // Capture the composited client, not merely SDL's backbuffer. Embedded VST
    // editors are child HWNDs and therefore only exist in this OS-level image.
    if (m_nativeWindow) {
        HDC srcDC = GetDC((HWND)m_nativeWindow);
        HDC memDC = srcDC ? CreateCompatibleDC(srcDC) : nullptr;
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = m_srcW; bi.bmiHeader.biHeight = -m_srcH;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = memDC ? CreateDIBSection(srcDC, &bi, DIB_RGB_COLORS,
                                               &bits, nullptr, 0) : nullptr;
        HGDIOBJ old = (memDC && bmp) ? SelectObject(memDC, bmp) : nullptr;
        if (old && BitBlt(memDC, 0, 0, m_srcW, m_srcH, srcDC, 0, 0,
                          SRCCOPY | CAPTUREBLT)) {
            const unsigned char* bgra = static_cast<const unsigned char*>(bits);
            for (int y=0; y<m_srcH; ++y) {
                unsigned char* dst = m_rgb.data() + (size_t)y * srcPitch;
                const unsigned char* row = bgra + (size_t)y * m_srcW * 4;
                for (int x=0; x<m_srcW; ++x) {
                    dst[x*3] = row[x*4+2]; dst[x*3+1] = row[x*4+1];
                    dst[x*3+2] = row[x*4];
                }
            }
            captured = true;
        }
        if (old) SelectObject(memDC, old);
        if (bmp) DeleteObject(bmp);
        if (memDC) DeleteDC(memDC);
        if (srcDC) ReleaseDC((HWND)m_nativeWindow, srcDC);
    }
#endif
    if (!captured) {
        SDL_Rect src{ 0, 0, m_srcW, m_srcH };
        if (SDL_RenderReadPixels(ren, &src, SDL_PIXELFORMAT_RGB24,
                                 m_rgb.data(), (int)srcPitch) != 0) {
            m_err = std::string("screen readback failed: ") + SDL_GetError();
            return;
        }
    }

    const unsigned char* out = m_rgb.data();
    if (m_w != m_srcW || m_h != m_srcH) {
        // Box-average down to the capture size.  Averaging rather than nearest
        // keeps small UI text legible instead of dropping whole pixel rows.
        for (int y = 0; y < m_h; ++y) {
            const int sy0 = y * m_srcH / m_h;
            const int sy1 = std::max(sy0+1,(y+1)*m_srcH/m_h);
            for (int x = 0; x < m_w; ++x) {
                const int sx0 = x * m_srcW / m_w;
                const int sx1 = std::max(sx0+1,(x+1)*m_srcW/m_w);
                int r = 0, g = 0, b = 0, n = 0;
                for (int sy=sy0;sy<std::min(sy1,m_srcH);++sy) {
                    const unsigned char* row = m_rgb.data() + (size_t)sy * srcPitch;
                    for (int sx=sx0;sx<std::min(sx1,m_srcW);++sx) {
                        r += row[sx * 3]; g += row[sx * 3 + 1]; b += row[sx * 3 + 2];
                        ++n;
                    }
                }
                unsigned char* d = m_scaled.data() + ((size_t)y * m_w + x) * 3;
                d[0] = (unsigned char)(r / n);
                d[1] = (unsigned char)(g / n);
                d[2] = (unsigned char)(b / n);
            }
        }
        out = m_scaled.data();
    }

    const size_t frameBytes=(size_t)m_w*m_h*3;
    m_lastFrame.assign(out,out+frameBytes);
    // Preserve wall-clock duration when rendering stalls.  Bound catch-up per
    // paint so a long debugger pause cannot freeze the UI writing thousands.
    const Uint64 target=std::min(due,m_nextFrameNumber+(Uint64)8);
    while(m_nextFrameNumber<target) {
        if (std::fwrite(m_lastFrame.data(),1,frameBytes,m_proc)!=frameBytes) {
            m_err = "encoder pipe closed (see " + m_logPath + ')';
            stop();
            return;
        }
        ++m_nextFrameNumber;
        ++m_frames;
    }
}

void ScreenRecorder::stop() {
    if (!m_proc) return;
    int encoderStatus=0;
#ifdef _WIN32
    // Close our end of the pipe so ffmpeg sees EOF and FINALIZES the container,
    // then wait for it to actually exit.  _pclose used to do both; with a real
    // process we have to, and the wait is what makes the file playable -- an
    // MKV whose index was never written opens in nothing.
    std::fclose(m_proc);
    m_proc = nullptr;
    if (m_child) {
        WaitForSingleObject((HANDLE)m_child, 30000);
        DWORD code = 1;
        GetExitCodeProcess((HANDLE)m_child, &code);
        encoderStatus = (int)code;
        CloseHandle((HANDLE)m_child);
        m_child = nullptr;
    }
#else
    encoderStatus=pclose(m_proc);
    m_proc = nullptr;
#endif

    std::error_code ec;
    const bool videoOk=encoderStatus==0&&m_frames>0&&fs::exists(
        m_cfg.captureAudio?m_videoPath:m_outPath,ec)&&
        fs::file_size(m_cfg.captureAudio?m_videoPath:m_outPath,ec)>0;
    if(!videoOk) {
        if(m_audio)PatchKnob::app::audio_app_capture_cancel();
        m_audio=false;
        if(m_err.empty()) {
            std::ifstream log(m_logPath);std::stringstream ss;if(log)ss<<log.rdbuf();
            m_err="screen encoder failed";
            const std::string detail=ss.str();
            if(!detail.empty())m_err+=": "+detail.substr(0,240);
        }
    } else if (m_cfg.captureAudio) {
        if(m_audio)PatchKnob::app::audio_app_capture_end_wav(m_wavPath.c_str());
        m_audio = false;
        // Also finalizes the silent temporary video if master capture could not
        // start; a failed audio tap must never strand a .video.* sidecar.
        mux_audio();
    }
    if(videoOk)fs::remove(m_logPath,ec);
    m_startTick = 0;
}

void ScreenRecorder::mux_audio() {
    std::error_code ec;
    if (!fs::exists(m_wavPath, ec) || !fs::exists(m_videoPath, ec)) {
        // No audio arrived -- keep the silent video as the take rather than
        // losing it.
        if (fs::exists(m_videoPath, ec)) fs::rename(m_videoPath, m_outPath, ec);
        return;
    }
    // Stream-copy the video (no re-encode) and hand the WAV in beside it.
    std::ostringstream command;
    const bool youtube=m_cfg.codec==CODEC_YOUTUBE;
    command << '"' << ffmpeg_path() << "\" -hide_banner -loglevel error -y -i \""
            << m_videoPath << "\" -i \"" << m_wavPath
            << "\" -map 0:v:0 -map 1:a:0 -c:v copy "
            << (youtube?"-c:a aac -b:a 128k -movflags +faststart ":"-c:a flac ")
            << '"'
            << m_outPath << '"';
    // Again: no `2>logfile`.  That is shell syntax, and passing it as an ARGUMENT
    // made ffmpeg reject its own command line -- which is why the mux produced a
    // zero-byte output while the .video and .wav sidecars both survived intact.
#ifdef _WIN32
    int rc = -1;
    if (HANDLE child = spawn_ffmpeg(command.str(), m_logPath, nullptr)) {
        WaitForSingleObject(child, 120000);
        DWORD code = 1;
        GetExitCodeProcess(child, &code);
        rc = (int)code;
        CloseHandle(child);
    }
#else
    const int rc = std::system((command.str()+" 2>\""+m_logPath+"\"").c_str());
#endif
    if (rc == 0 && fs::exists(m_outPath, ec) && fs::file_size(m_outPath,ec)>0) {
        fs::remove(m_videoPath, ec);
        fs::remove(m_wavPath, ec);
        fs::remove(m_logPath, ec);
    } else {
        // Muxing failed: leave both parts on disk rather than deleting a take.
        //
        // Delete the OUTPUT though.  ffmpeg creates the container before it
        // fails, so a failed mux left a zero-byte .mkv sitting next to the two
        // good sidecars -- and that empty file is the one a player opens, which
        // makes a recoverable take look like a broken recorder.
        if (fs::exists(m_outPath, ec) && fs::file_size(m_outPath, ec) == 0)
            fs::remove(m_outPath, ec);
        std::ifstream log(m_logPath);std::stringstream detail;if(log)detail<<log.rdbuf();
        m_err = "muxing failed; video and wav kept separately";
        if(!detail.str().empty())m_err += ": " + detail.str().substr(0,240);
        m_outPath = m_videoPath;
    }
}

} // namespace pkrec
