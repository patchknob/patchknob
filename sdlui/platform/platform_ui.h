#pragma once
#include <SDL.h>

namespace ui { namespace platform {

bool mobile();
void prepare_sdl();
Uint32 window_flags();
void configure_display(SDL_Renderer* renderer,int& w,int& h,float& scale);
void resize_display(SDL_Renderer* renderer,int& w,int& h,float& scale);
float load_ui_scale();
void save_ui_scale(float value);
float default_ui_scale();
int menu_row_height(int fontHeight);
int menu_separator_height();
int menu_bar_height(float uiScale);
int window_title_height(float uiScale);
int taskbar_height(float uiScale);
int csound_compile_button_width();
const char* csound_editor_help();
bool patcher_plain_wheel_zooms();
bool tracker_accepts_text_input();

// Rack-editor interaction policy. Geometry/model code stays shared; only the
// presentation and finger-vs-pointer tolerances differ by selected source file.
float rack_default_zoom();
int rack_jack_hit_radius(int scaledRadius, int desktopRadius);
int rack_knob_hit_radius(int drawnRadius);
int rack_cable_hit_radius(int scaledRadius, int desktopRadius);
bool rack_drag_whole_faceplate();
bool rack_draw_touch_halos();
int rack_context_row_height();
int rack_context_width();
int rack_palette_width();
int rack_palette_rows();
int rack_palette_card_height();

} }
