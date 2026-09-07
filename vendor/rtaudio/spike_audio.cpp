// spike_audio.cpp - standalone RtAudio WASAPI spike for the seq24 audio engine.
//
// Goals:
//   1. Enumerate audio output devices (name, output channels, sample rates).
//   2. Open the default output at 48000 Hz, 512-frame stereo float32 buffer.
//   3. Run a callback synthesizing a 440 Hz sine for ~1.5 s, then close cleanly.
//
// Build (MINGW64):
//   g++ -std=c++17 -O2 -D__WINDOWS_WASAPI__ spike_audio.cpp RtAudio.cpp \
//       -o spike_audio.exe -lole32 -lwinmm -lksuser -lmfplat -lmfuuid \
//       -lwmcodecdspuuid
//
// This callback is the model for the seq24 real-time audio thread that will
// later pull rendered audio from hosted VST3 plugins.

#include "RtAudio.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr unsigned int kSampleRate   = 48000;
constexpr unsigned int kBufferFrames = 512;     // requested; RtAudio may adjust
constexpr unsigned int kChannels     = 2;       // stereo
constexpr double       kFreqHz       = 440.0;
constexpr double       kAmplitude    = 0.2;      // keep it gentle
constexpr double       kTwoPi        = 6.283185307179586476925286766559;

struct SineState {
    double            phase    = 0.0;
    double            phaseInc = 0.0;             // radians per frame
    std::atomic<uint64_t> framesRendered{0};
    std::atomic<uint64_t> underflowCount{0};
    std::atomic<uint64_t> callbackCount{0};
};

// Real-time audio callback. Runs on RtAudio's dedicated WASAPI thread.
// Must be non-blocking, lock-free, and allocation-free.
int audioCallback( void* outputBuffer,
                   void* /*inputBuffer*/,
                   unsigned int nFrames,
                   double /*streamTime*/,
                   RtAudioStreamStatus status,
                   void* userData )
{
    auto* st  = static_cast<SineState*>( userData );
    auto* out = static_cast<float*>( outputBuffer );

    if ( status & RTAUDIO_OUTPUT_UNDERFLOW )
        st->underflowCount.fetch_add( 1, std::memory_order_relaxed );

    st->callbackCount.fetch_add( 1, std::memory_order_relaxed );

    for ( unsigned int i = 0; i < nFrames; ++i ) {
        const float s = static_cast<float>( kAmplitude * std::sin( st->phase ) );
        st->phase += st->phaseInc;
        if ( st->phase >= kTwoPi ) st->phase -= kTwoPi;
        // Interleaved stereo: same sample to both channels.
        for ( unsigned int ch = 0; ch < kChannels; ++ch )
            *out++ = s;
    }

    st->framesRendered.fetch_add( nFrames, std::memory_order_relaxed );
    return 0; // keep running
}

void errorCallback( RtAudioErrorType type, const std::string& msg )
{
    std::fprintf( stderr, "[RtAudio error %d] %s\n", (int)type, msg.c_str() );
}

void enumerateDevices( RtAudio& dac )
{
    std::vector<unsigned int> ids = dac.getDeviceIds();
    unsigned int defaultOut = dac.getDefaultOutputDevice();

    std::printf( "=== Audio devices (API: %s) ===\n",
                 RtAudio::getApiDisplayName( dac.getCurrentApi() ).c_str() );
    std::printf( "Found %zu device(s); default output id = %u\n\n",
                 ids.size(), defaultOut );

    for ( unsigned int id : ids ) {
        RtAudio::DeviceInfo info = dac.getDeviceInfo( id );
        if ( info.outputChannels == 0 )
            continue; // output-focused spike: skip input-only devices

        std::printf( "Device id %u: \"%s\"%s\n",
                     info.ID, info.name.c_str(),
                     info.isDefaultOutput ? "  [DEFAULT OUTPUT]" : "" );
        std::printf( "    output channels : %u\n", info.outputChannels );
        std::printf( "    input channels  : %u\n", info.inputChannels );
        std::printf( "    preferred rate  : %u Hz\n", info.preferredSampleRate );
        std::printf( "    current rate    : %u Hz\n", info.currentSampleRate );
        std::printf( "    sample rates    : " );
        for ( size_t i = 0; i < info.sampleRates.size(); ++i )
            std::printf( "%u%s", info.sampleRates[i],
                         i + 1 < info.sampleRates.size() ? ", " : "" );
        std::printf( "\n\n" );
    }
}

} // namespace

int main()
{
    RtAudio dac( RtAudio::WINDOWS_WASAPI, &errorCallback );

    if ( dac.getDeviceCount() == 0 ) {
        std::fprintf( stderr, "No audio devices found.\n" );
        return 1;
    }

    enumerateDevices( dac );

    unsigned int outDevice = dac.getDefaultOutputDevice();
    if ( outDevice == 0 ) {
        // Fall back to the first device that has output channels.
        for ( unsigned int id : dac.getDeviceIds() ) {
            if ( dac.getDeviceInfo( id ).outputChannels > 0 ) { outDevice = id; break; }
        }
    }

    RtAudio::StreamParameters oParams;
    oParams.deviceId     = outDevice;
    oParams.nChannels    = kChannels;
    oParams.firstChannel = 0;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_SCHEDULE_REALTIME; // ask for RT-priority callback thread

    SineState state;
    state.phaseInc = kTwoPi * kFreqHz / static_cast<double>( kSampleRate );

    unsigned int bufferFrames = kBufferFrames; // RtAudio may modify this

    RtAudioErrorType err = dac.openStream(
        &oParams, /*inputParameters=*/nullptr,
        RTAUDIO_FLOAT32, kSampleRate, &bufferFrames,
        &audioCallback, &state, &options );

    if ( err != RTAUDIO_NO_ERROR ) {
        std::fprintf( stderr, "openStream failed: %s\n", dac.getErrorText().c_str() );
        return 1;
    }

    std::printf( "Opened stream on device id %u\n", outDevice );
    std::printf( "    requested buffer frames : %u\n", kBufferFrames );
    std::printf( "    actual    buffer frames : %u\n", bufferFrames );
    std::printf( "    sample rate             : %u Hz\n", kSampleRate );
    std::printf( "    format                  : float32, %u ch interleaved\n", kChannels );

    err = dac.startStream();
    if ( err != RTAUDIO_NO_ERROR ) {
        std::fprintf( stderr, "startStream failed: %s\n", dac.getErrorText().c_str() );
        dac.closeStream();
        return 1;
    }

    std::printf( "\nPlaying 440 Hz sine for ~1.5 seconds...\n" );
    std::this_thread::sleep_for( std::chrono::milliseconds( 1500 ) );

    if ( dac.isStreamRunning() )
        dac.stopStream();
    if ( dac.isStreamOpen() )
        dac.closeStream();

    const uint64_t frames    = state.framesRendered.load();
    const uint64_t callbacks = state.callbackCount.load();
    const uint64_t underflow = state.underflowCount.load();

    std::printf( "\n=== Results ===\n" );
    std::printf( "callbacks invoked : %llu\n", (unsigned long long)callbacks );
    std::printf( "frames rendered   : %llu (~%.2f s of audio)\n",
                 (unsigned long long)frames,
                 (double)frames / (double)kSampleRate );
    std::printf( "underflows (xrun) : %llu\n", (unsigned long long)underflow );
    std::printf( "Stream closed cleanly.\n" );

    return underflow == 0 ? 0 : 2;
}
