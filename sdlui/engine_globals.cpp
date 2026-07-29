//----------------------------------------------------------------------------
//  sdlui/engine_globals.cpp
//
//  Definitions of the PatchKnob `global_*` configuration variables (declared extern
//  in src/globals.h).  The SDL frontend owns them here instead of compiling the
//  legacy entrypoint.
//----------------------------------------------------------------------------
#include "globals.h"

bool global_manual_alsa_ports = false;
bool global_showmidi          = false;
bool global_priority          = false;
bool global_device_ignore     = false;
int  global_device_ignore_num = 0;
bool global_stats             = false;
bool global_pass_sysex        = false;
std::string global_filename   = "untitled.s24";
bool global_print_keys        = false;

bool global_with_jack_transport   = false;
bool global_with_jack_master      = false;
bool global_with_jack_master_cond = false;
bool global_jack_start_mode       = true;

user_midi_bus_definition   global_user_midi_bus_definitions[c_maxBuses];
user_instrument_definition global_user_instrument_definitions[c_max_instruments];
