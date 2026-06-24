//----------------------------------------------------------------------------
//  audio_app.h  -- seq24 Windows port audio/VST subsystem bootstrap.
//
//  Thin C-style entry points so seq24.cpp (gtkmm) can start/stop the hand-rolled
//  audio engine + mixer graph + plugin host without pulling in engine headers.
//  The heavy lifting (and the engine singletons) live in audio_app.cpp.
//----------------------------------------------------------------------------
#ifndef SEQ24_AUDIO_APP_H
#define SEQ24_AUDIO_APP_H

namespace seq24 { namespace app {

// Open the default audio output device and start the (initially silent) master
// mixer graph.  Returns false if no audio device could be opened (the app still
// runs as a pure MIDI sequencer in that case).
bool audio_app_init();

// Stop audio and tear down the engine.  Safe to call if init failed.
void audio_app_shutdown();

// True once the audio engine is running.
bool audio_app_running();

}} // namespace seq24::app

#endif // SEQ24_AUDIO_APP_H
