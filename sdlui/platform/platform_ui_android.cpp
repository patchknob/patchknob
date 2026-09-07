#include "platform_ui.h"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace ui { namespace platform {
bool mobile(){return true;}
void prepare_sdl(){SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS,"1");SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS,"0");SDL_SetHint(SDL_HINT_ORIENTATIONS,"LandscapeLeft LandscapeRight");}
Uint32 window_flags(){return SDL_WINDOW_FULLSCREEN_DESKTOP|SDL_WINDOW_ALLOW_HIGHDPI;}
void configure_display(SDL_Renderer* r,int& w,int& h,float& scale){int pw=1280,ph=720;SDL_GetRendererOutputSize(r,&pw,&ph);scale=ph>0?std::max(.5f,(float)ph/720.f):1.f;h=720;w=std::max(960,(int)std::lround((float)pw/scale));}
void resize_display(SDL_Renderer* r,int& w,int& h,float& scale){configure_display(r,w,h,scale);}
float load_ui_scale(){float v=1.25f;if(char* p=SDL_GetPrefPath("PatchKnob","PatchKnob")){std::ifstream f(std::string(p)+"mobile_ui_scale.txt");f>>v;SDL_free(p);}return std::clamp(v,1.f,1.75f);}
void save_ui_scale(float v){if(char* p=SDL_GetPrefPath("PatchKnob","PatchKnob")){std::ofstream(std::string(p)+"mobile_ui_scale.txt",std::ios::trunc)<<v<<"\n";SDL_free(p);}}
float default_ui_scale(){return 1.25f;}
int menu_row_height(int h){return std::max(48,h+14);}
int menu_separator_height(){return 10;}
int menu_bar_height(float s){return std::max(44,(int)std::lround(42.f*s));}
int window_title_height(float s){return std::max(40,(int)std::lround(36.f*s));}
int taskbar_height(float s){return std::max(44,(int)std::lround(40.f*s));}
int csound_compile_button_width(){return 116;}
const char* csound_editor_help(){return "Hold = double click  |  pinch = zoom";}
bool patcher_plain_wheel_zooms(){return true;}
bool tracker_accepts_text_input(){return true;}
float rack_default_zoom(){return 1.15f;}
int rack_jack_hit_radius(int scaled,int){return std::max(scaled,26);}
int rack_knob_hit_radius(int r){return std::max(r+4,20);}
int rack_cable_hit_radius(int scaled,int){return std::max(scaled,24);}
bool rack_drag_whole_faceplate(){return true;}
bool rack_draw_touch_halos(){return true;}
int rack_context_row_height(){return 48;}
int rack_context_width(){return 190;}
int rack_palette_width(){return 480;}
int rack_palette_rows(){return 4;}
int rack_palette_card_height(){return 128;}
} }
