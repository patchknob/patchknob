#include "video_settings_view.h"

#include <cstdio>

namespace ui {

namespace {
enum { R_CODEC = 0, R_HEIGHT, R_FPS, R_THREADS, R_AUDIO, R_FOLDER, R_COUNT };

const int kHeights[] = { 480, 720, 1080, 1440 };
const int kFps[]     = { 24, 30, 60 };

int cycle(int cur, const int* vals, int n, int dir) {
    for (int i = 0; i < n; ++i)
        if (vals[i] == cur) return vals[(i + n + dir) % n];
    return vals[0];
}
} // namespace

void VideoSettingsView::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);
    if (!cfg) return;

    const int rowH = app.font.ch() + 12;
    const int labX = rect.x + 12;
    const int valX = rect.x + 150;
    int y = rect.y + 10;
    m_nrows = 0;

    // --- rows -------------------------------------------------------------
    auto drawRow = [&](int id, const char* label, const std::string& value,
                       const std::string& note) {
        SDL_Rect rr{ rect.x + 4, y, rect.w - 8, rowH };
        m_rows[m_nrows++] = Row{ rr, id };
        app.font.draw(app.ren, labX, y + 5, label, t.dim);
        app.font.draw(app.ren, valX, y + 5, value, t.text);
        if (!note.empty())
            app.mono.draw(app.ren, valX + app.font.text_w(value) + 12, y + 7,
                          note, t.dim);
        y += rowH;
    };

    char buf[64];
    drawRow(R_CODEC, "Codec", cfg->codec_label(), cfg->codec_note());
    std::snprintf(buf, sizeof buf, "%dp", cfg->height);
    drawRow(R_HEIGHT, "Height", buf, "width follows the window");
    std::snprintf(buf, sizeof buf, "%d fps", cfg->fps);
    drawRow(R_FPS, "Frame rate", buf, "");
    std::snprintf(buf, sizeof buf, "%d", cfg->threads);
    drawRow(R_THREADS, "Encoder threads", buf,
            cfg->threads == 1 ? "1 keeps it off the audio thread" : "");
    drawRow(R_AUDIO, "Mixer audio", cfg->captureAudio ? "on" : "off",
            "taken from the master tap, cannot drift");
    drawRow(R_FOLDER, "Folder",
            cfg->folder.empty() ? std::string("(working directory)") : cfg->folder,
            "click to change");

    y += 6;
    const std::string ff = ffmpeg_path ? ffmpeg_path() : std::string();
    if (ff.empty())
        app.mono.draw(app.ren, labX, y, "ffmpeg not found - recording disabled", t.accent);
    else if (recording && recording())
        app.mono.draw(app.ren, labX, y, "recording; settings apply to the next take", t.accent);
    else
        app.mono.draw(app.ren, labX, y, ("ffmpeg: " + ff), t.dim);
}

bool VideoSettingsView::on_mouse(App& app, const MouseEv& e) {
    if (!cfg || !e.pressed) return false;
    for (int i = 0; i < m_nrows; ++i) {
        const SDL_Rect& r = m_rows[i].r;
        if (e.x < r.x || e.x >= r.x + r.w || e.y < r.y || e.y >= r.y + r.h) continue;
        // Left click steps forward, right click steps back.
        const int dir = (e.button == SDL_BUTTON_RIGHT) ? -1 : 1;
        switch (m_rows[i].id) {
            case R_CODEC:
                cfg->codec = (cfg->codec + pkrec::CODEC_COUNT + dir) % pkrec::CODEC_COUNT;
                break;
            case R_HEIGHT:
                cfg->height = cycle(cfg->height, kHeights,
                                    (int)(sizeof kHeights / sizeof kHeights[0]), dir);
                break;
            case R_FPS:
                cfg->fps = cycle(cfg->fps, kFps,
                                 (int)(sizeof kFps / sizeof kFps[0]), dir);
                break;
            case R_THREADS:
                cfg->threads += dir;
                if (cfg->threads < 1) cfg->threads = 8;
                if (cfg->threads > 8) cfg->threads = 1;
                break;
            case R_AUDIO:
                cfg->captureAudio = !cfg->captureAudio;
                break;
            case R_FOLDER: {
                std::string p = cfg->folder;
                if (on_pick_folder && on_pick_folder(p)) cfg->folder = p;
                break;
            }
            default: break;
        }
        app.request_redraw();
        return true;
    }
    return false;
}

} // namespace ui
