#include "audio_engine.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace PatchKnob { namespace engine {

struct AndroidAudioStream {
    SDL_AudioDeviceID device=0;
    int channels=2;
    std::vector<float> planar;
    std::vector<float*> planes;
};

static void android_audio_callback(void* userdata, Uint8* bytes, int len) {
    AudioEngine* engine=static_cast<AudioEngine*>(userdata);
    const int channels=(int)engine->numChannels();
    const int frames=len/(int)(sizeof(float)*std::max(1,channels));
    AndroidAudioStream* stream=
        static_cast<AndroidAudioStream*>(engine->androidStream());
    if(!stream || frames<1) { std::memset(bytes,0,(size_t)len); return; }
    for(int c=0;c<channels;++c)
        stream->planes[(size_t)c]=stream->planar.data()+(size_t)c*frames;
    engine->render_into(stream->planes.data(),(unsigned long)frames,0);
    float* dst=reinterpret_cast<float*>(bytes);
    for(int i=0;i<frames;++i)
        for(int c=0;c<channels;++c)
            dst[(size_t)i*channels+c]=stream->planes[(size_t)c][i];
}

bool AudioEngine::ensureInit() {
    if(paInited_) return true;
    // SDL maps this request to Android's low-latency AAudio backend where
    // available and falls back to OpenSL ES on older devices.
    SDL_SetHint(SDL_HINT_AUDIODRIVER,"aaudio");
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)!=0) {
        lastError_=SDL_GetError(); return false;
    }
    paInited_=true; return true;
}

AudioEngine::AudioEngine(){ensureInit();}
AudioEngine::~AudioEngine(){
    close();
    if(paInited_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    delete render_.exchange(nullptr,std::memory_order_seq_cst);
}
void* AudioEngine::androidStream() const { return stream_; }

void AudioEngine::setRenderCallback(RenderCallback cb) {
    RenderHolder* fresh=cb?new RenderHolder{std::move(cb)}:nullptr;
    RenderHolder* old=render_.exchange(fresh,std::memory_order_seq_cst);
    while(inCallback_.load(std::memory_order_seq_cst)) std::this_thread::yield();
    delete old;
}

std::vector<AudioHostApiInfo> AudioEngine::enumerateHostApis() {
    return {{0,"Android AAudio / OpenSL ES",SDL_GetNumAudioDevices(0),true}};
}
void AudioEngine::selectHostApi(int){hostApi_=0;}
std::vector<AudioDeviceInfo> AudioEngine::enumerateOutputDevices() {
    std::vector<AudioDeviceInfo> out;
    const int n=SDL_GetNumAudioDevices(0);
    for(int i=0;i<n;++i) out.push_back(
        {(unsigned)i+1,SDL_GetAudioDeviceName(i,0),2,48000,i==0,0});
    if(out.empty()) out.push_back({1,"Android default output",2,48000,true,0});
    return out;
}
std::vector<AudioDeviceInfo> AudioEngine::enumerateInputDevices() {
    std::vector<AudioDeviceInfo> out;
    const int n=SDL_GetNumAudioDevices(1);
    for(int i=0;i<n;++i) out.push_back(
        {(unsigned)i+1,SDL_GetAudioDeviceName(i,1),2,48000,i==0,0});
    return out;
}
unsigned int AudioEngine::defaultOutputDeviceId(){return 1;}
void AudioEngine::selectDevice(unsigned int id){selectedDeviceId_=id;}

bool AudioEngine::open(unsigned int rate,unsigned int block,unsigned int channels) {
    if(!ensureInit()||isOpen()||channels<1) return false;
    SDL_AudioSpec want{},have{};
    want.freq=(int)rate; want.format=AUDIO_F32SYS; want.channels=(Uint8)channels;
    want.samples=(Uint16)std::clamp<unsigned>(
        bufferFrames_?bufferFrames_:block,64,1024);
    want.callback=android_audio_callback; want.userdata=this;
    const char* name=nullptr;
    if(selectedDeviceId_>0) name=SDL_GetAudioDeviceName((int)selectedDeviceId_-1,0);
    SDL_AudioDeviceID id=SDL_OpenAudioDevice(name,0,&want,&have,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE|SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if(!id){lastError_=SDL_GetError();return false;}
    auto* s=new AndroidAudioStream;
    s->device=id; s->channels=have.channels;
    s->planar.resize((size_t)have.channels*have.samples);
    s->planes.resize(have.channels);
    stream_=s; sampleRate_=(unsigned)have.freq; blockSize_=have.samples;
    numChannels_=have.channels; selectedDeviceId_=selectedDeviceId_?selectedDeviceId_:1;
    deviceLost_=false; lastError_.clear(); return true;
}
bool AudioEngine::start(){
    auto* s=static_cast<AndroidAudioStream*>(stream_);
    if(!s){lastError_="stream not open";return false;}
    SDL_PauseAudioDevice(s->device,0); return true;
}
void AudioEngine::stop(){
    auto* s=static_cast<AndroidAudioStream*>(stream_);
    if(s) SDL_PauseAudioDevice(s->device,1);
}
void AudioEngine::close(){
    auto* s=static_cast<AndroidAudioStream*>(stream_);
    if(!s)return;
    SDL_CloseAudioDevice(s->device); delete s; stream_=nullptr;
}
bool AudioEngine::isRunning() const {
    auto* s=static_cast<AndroidAudioStream*>(stream_);
    return s&&SDL_GetAudioDeviceStatus(s->device)==SDL_AUDIO_PLAYING;
}
void AudioEngine::stream_finished(){deviceLost_=true;}
bool AudioEngine::tryRecover(){
    close(); selectedDeviceId_=0;
    return open(sampleRate_,blockSize_,numChannels_)&&start();
}

int AudioEngine::render_into(void* output,unsigned long frames,unsigned long) {
    ScopedNoDenormals guard;
    float** out=static_cast<float**>(output);
    const int ch=(int)numChannels_,n=(int)frames;
    callbackCount_.fetch_add(1,std::memory_order_relaxed);
    inCallback_=true;
    const RenderHolder* cb=render_.load(std::memory_order_seq_cst);
    try {
        if(cb&&cb->fn) cb->fn(out,ch,n,(double)sampleRate_);
        else for(int c=0;c<ch;++c) std::fill(out[c],out[c]+n,0.f);
    } catch(...) {
        for(int c=0;c<ch;++c) std::fill(out[c],out[c]+n,0.f);
    }
    float peak=0.f; double sum=0.;
    for(int c=0;c<ch;++c) for(int i=0;i<n;++i) {
        peak=std::max(peak,std::fabs(out[c][i])); sum+=(double)out[c][i]*out[c][i];
    }
    masterPeak_=peak; masterRms_=(ch*n)?(float)std::sqrt(sum/(ch*n)):0.f;
    inCallback_=false; return 0;
}

}} // namespace PatchKnob::engine
