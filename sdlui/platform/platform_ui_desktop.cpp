#include "platform_ui.h"
#include <algorithm>
#include <cstdlib>

namespace ui { namespace platform {
bool mobile(){return false;}
void prepare_sdl(){}
Uint32 window_flags(){return SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI;}
void configure_display(SDL_Renderer* r,int& w,int& h,float& scale){
    scale=1.f;if(const char* s=std::getenv("PATCHKNOBSDL_SCALE")){float v=(float)std::atof(s);if(v>=.5f&&v<=4.f)scale=v;}
    else{int pw,ph,ww,wh;SDL_GetRendererOutputSize(r,&pw,&ph);SDL_GetWindowSize(SDL_RenderGetWindow(r),&ww,&wh);if(ww>0){float a=(float)pw/ww;if(a>=1.f&&a<=4.f)scale=a;}}
    int pw,ph;SDL_GetRendererOutputSize(r,&pw,&ph);w=(int)(pw/scale+.5f);h=(int)(ph/scale+.5f);
}
void resize_display(SDL_Renderer* r,int& w,int& h,float& scale){int pw,ph;SDL_GetRendererOutputSize(r,&pw,&ph);w=(int)(pw/scale+.5f);h=(int)(ph/scale+.5f);}
float load_ui_scale(){return 1.f;}
void save_ui_scale(float){}
float default_ui_scale(){return 1.f;}
int menu_row_height(int h){return h+8;}
int menu_separator_height(){return 6;}
int menu_bar_height(float){return 24;}
int window_title_height(float){return 22;}
int taskbar_height(float){return 22;}
int csound_compile_button_width(){return 96;}
const char* csound_editor_help(){return "Ctrl+E compile  |  Ctrl+C/X/V copy/cut/paste  |  drag to select";}
bool patcher_plain_wheel_zooms(){return false;}
bool tracker_accepts_text_input(){return false;}
float rack_default_zoom(){return .8f;}
int rack_jack_hit_radius(int scaled,int desktop){return std::max(scaled,desktop);}
int rack_knob_hit_radius(int r){return r+2;}
int rack_cable_hit_radius(int scaled,int desktop){return std::max(scaled,desktop);}
bool rack_drag_whole_faceplate(){return false;}
bool rack_draw_touch_halos(){return false;}
int rack_context_row_height(){return 20;}
int rack_context_width(){return 110;}
int rack_palette_width(){return 292;}
int rack_palette_rows(){return 5;}
int rack_palette_card_height(){return 104;}
} }
