//----------------------------------------------------------------------------
//  sdlui/views/audio_settings/audio_settings_view.cpp
//----------------------------------------------------------------------------
#include "audio_settings_view.h"
#include "audio_app.h"

#include <algorithm>
#include <cstdio>

using namespace ui;

namespace audioui {

static std::string fit_text(const ui::Font& font, std::string text, int maxw) {
    if (maxw <= 0) return "";
    if (font.text_w(text) <= maxw) return text;
    const std::string ell = "...";
    if (font.text_w(ell) > maxw) {
        std::string dots = ell;
        while (!dots.empty() && font.text_w(dots) > maxw) dots.pop_back();
        return dots;
    }
    while (!text.empty() && font.text_w(text + ell) > maxw) text.pop_back();
    return text.empty() ? ell : text + ell;
}

static const unsigned kBufs[] = {
    64, 128, 256, 512, 1024, 2048, 4096,
    8192, 16384, 32768, 65536
};

int AudioSettingsView::row_h(App& app) const { return app.font.ch() + 8; }

std::vector<AudioSettingsView::Row> AudioSettingsView::build() const {
    std::vector<Row> rows;
    if (!audio_ok) { rows.push_back({HEADER,0,0,"No audio device",false}); return rows; }

    rows.push_back({HEADER,0,0,"Driver Type / Backend",false});
    int hc = PatchKnob::app::audio_app_hostapi_count();
    for (int i=0;i<hc;++i){ int idx=0; char nm[96]={0}; int cur=0;
        if (PatchKnob::app::audio_app_hostapi_info(i,&idx,nm,sizeof(nm),&cur))
            rows.push_back({BACKEND, idx, 0, nm, cur!=0}); }

    rows.push_back({HEADER,0,0,"Output Device",false});
    int dc = PatchKnob::app::audio_app_device_count();
    for (int i=0;i<dc;++i){ unsigned id=0; char nm[128]={0}; int cur=0;
        if (PatchKnob::app::audio_app_device_info(i,&id,nm,sizeof(nm),&cur))
            rows.push_back({OUTDEV, 0, id, nm, cur!=0}); }

    rows.push_back({HEADER,0,0,"Input Device",false});
    int ic = PatchKnob::app::audio_app_input_device_count();
    if (ic == 0) rows.push_back({INDEV,0,0,"(none)",false});
    for (int i=0;i<ic;++i){ unsigned id=0; char nm[128]={0}; int cur=0;
        if (PatchKnob::app::audio_app_input_device_info(i,&id,nm,sizeof(nm),&cur))
            rows.push_back({INDEV, 0, id, nm, cur!=0}); }

    rows.push_back({HEADER,0,0,"Buffer Size (latency)",false});
    unsigned cb = PatchKnob::app::audio_app_buffer_size();
    for (unsigned b : kBufs){ char lbl[24]; std::snprintf(lbl,sizeof(lbl),"%u frames",b);
        rows.push_back({BUFFER, 0, b, lbl, b==cb}); }
    return rows;
}

void AudioSettingsView::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);
    const int rh = row_h(app);
    const std::vector<Row> rows = build();
    int y = rect.y + 6 - m_scroll;
    for (const Row& r : rows) {
        if (y + rh > rect.y && y < rect.y + rect.h) {   // clip to body
            SDL_Rect rr{ rect.x+4, y, rect.w-8, rh-2 };
            if (r.kind == HEADER) {
                fill_rect(app.ren, rr, t.panel);
                app.font.draw(app.ren, rr.x+4, y+3, fit_text(app.font, r.label, rr.w - 8), t.dim);
            } else {
                if (r.current) { fill_rect(app.ren, rr, t.accent); }
                Color fg = r.current ? t.bg : t.text;
                const char* mark = r.current ? "* " : "  ";
                app.font.draw(app.ren, rr.x+8, y+3,
                              fit_text(app.font, std::string(mark)+r.label, rr.w - 16), fg);
            }
        }
        y += rh;
    }
    frame_rect(app.ren, rect, t.dim);
}

bool AudioSettingsView::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed) return true;
    const int rh = row_h(app);
    const std::vector<Row> rows = build();
    int y = rect.y + 6 - m_scroll;
    for (const Row& r : rows) {
        if (e.y >= y && e.y < y + rh && r.kind != HEADER) {
            switch (r.kind) {
                case BACKEND: PatchKnob::app::audio_app_set_hostapi(r.ival); break;
                case OUTDEV:  PatchKnob::app::audio_app_set_device(r.uval); break;
                case INDEV:   PatchKnob::app::audio_app_set_input_device(r.uval); break;
                case BUFFER:  PatchKnob::app::audio_app_set_buffer_size(r.uval); break;
                default: break;
            }
            app.request_redraw();
            return true;
        }
        y += rh;
    }
    return true;
}

bool AudioSettingsView::on_wheel(App& app, int /*dx*/, int dy) {
    m_scroll -= dy * (row_h(app) * 2);
    if (m_scroll < 0) m_scroll = 0;
    int maxScroll = std::max(0, (int)build().size() * row_h(app) + 12 - rect.h);
    if (m_scroll > maxScroll) m_scroll = maxScroll;
    app.request_redraw();
    return true;
}

} // namespace audioui
