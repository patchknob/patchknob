#include "screen_recorder.h"
#include "audio_app.h"
#include <SDL.h>
#include <cstdio>
#include <filesystem>
#include <cmath>

// Screen audio is deliberately disabled in this process-level encoder test.
// Stubs keep the test independent of the complete audio graph.
namespace PatchKnob { namespace app {
bool audio_app_capture_begin(double) { return true; }
bool audio_app_capture_end_wav(const char* path) {
    FILE* f=std::fopen(path,"wb");if(!f)return false;
    const unsigned rate=48000,frames=rate*6/5,dataBytes=frames*4;
    auto w32=[&](unsigned v){unsigned char b[4]={(unsigned char)v,(unsigned char)(v>>8),(unsigned char)(v>>16),(unsigned char)(v>>24)};return std::fwrite(b,1,4,f)==4;};
    auto w16=[&](unsigned v){unsigned char b[2]={(unsigned char)v,(unsigned char)(v>>8)};return std::fwrite(b,1,2,f)==2;};
    bool ok=std::fwrite("RIFF",1,4,f)==4&&w32(36+dataBytes)&&
      std::fwrite("WAVEfmt ",1,8,f)==8&&w32(16)&&w16(1)&&w16(2)&&
      w32(rate)&&w32(rate*4)&&w16(4)&&w16(16)&&
      std::fwrite("data",1,4,f)==4&&w32(dataBytes);
    for(unsigned i=0;ok&&i<frames;++i){const int s=(int)(std::sin(i*6.283185307179586*440.0/rate)*8000.0);ok=w16((unsigned)s&0xffff)&&w16((unsigned)s&0xffff);}
    ok=std::fclose(f)==0&&ok;return ok;
}
void audio_app_capture_cancel() {}
}}

int main(int argc,char** argv) {
    const char* folder=argc>1?argv[1]:"screen-recorder-selftest";
    std::error_code ec;
    std::filesystem::create_directories(folder,ec);
    if(ec) return 10;
    SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0) return 11;
    SDL_Window* win=SDL_CreateWindow("recorder-test",SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,641,359,SDL_WINDOW_HIDDEN);
    SDL_Renderer* ren=win?SDL_CreateRenderer(win,-1,SDL_RENDERER_SOFTWARE):nullptr;
    if(!ren) { if(win)SDL_DestroyWindow(win);SDL_Quit();return 12; }
    pkrec::Settings cfg;cfg.captureAudio=true;cfg.folder=folder;cfg.height=360;
    cfg.fps=30;cfg.codec=pkrec::CODEC_YOUTUBE;
    pkrec::ScreenRecorder rec;
    if(!rec.start(ren,641,359,cfg)) {
        std::fprintf(stderr,"start: %s\n",rec.error().c_str());return 13;
    }
    for(int i=0;i<36;++i) {
        SDL_SetRenderDrawColor(ren,(Uint8)(i*7),20,(Uint8)(255-i*5),255);
        SDL_RenderClear(ren);
        SDL_Rect box{20+i*8,100,80,80};
        SDL_SetRenderDrawColor(ren,255,240,30,255);SDL_RenderFillRect(ren,&box);
        rec.capture_frame(ren); // before present: the production ordering
        SDL_RenderPresent(ren);
        SDL_Delay(34);
    }
    rec.stop();
    const std::string out=rec.output_path();
    const std::string err=rec.error();
    const bool ok=err.empty()&&!out.empty()&&std::filesystem::exists(out,ec)&&
                  std::filesystem::file_size(out,ec)>0&&rec.frames()>=30;
    std::printf("path=%s\nframes=%lld\nerror=%s\n",out.c_str(),rec.frames(),err.c_str());
    SDL_DestroyRenderer(ren);SDL_DestroyWindow(win);SDL_Quit();
    return ok?0:14;
}
