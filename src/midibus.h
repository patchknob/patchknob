//----------------------------------------------------------------------------
//
//  This file is part of PatchKnob.
//
//  PatchKnob is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  PatchKnob is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//-----------------------------------------------------------------------------
//
//  Windows (MINGW64) port note:
//  The original midibus was ALSA-specific (snd_seq_t).  This header keeps the
//  exact public interface the rest of PatchKnob (perform/sequence) depends on, but
//  removes all ALSA types so it compiles on Windows.  The implementation in
//  midibus.cpp is currently a stub; Phase 1 of the port replaces the internals
//  with RtMidi (WinMM) for real Windows MIDI I/O.
//
//-----------------------------------------------------------------------------

class midibus;
class mastermidibus;

#ifndef PATCHKNOB_MIDIBUS
#define PATCHKNOB_MIDIBUS

#include "event.h"
#include "sequence.h"
#include <string>
#include <vector>
#include <deque>
#include "mutex.h"
#include "globals.h"

/* RtMidi (WinMM) backend.  RtMidi.h is on the include path for all PatchKnob
   sources (see CMakeLists.txt), so include it directly rather than forward-
   declaring (the real classes carry a visibility attribute that makes plain
   forward declarations ambiguous). */
#include "RtMidi.h"

const int c_midibus_output_size = 0x100000;
const int c_midibus_input_size =  0x100000;
const int c_midibus_sysex_chunk = 0x100;

enum clock_e
{
    e_clock_off,
    e_clock_pos,
    e_clock_mod
};

class midibus
{

 private:

    char m_id;

    clock_e m_clock_type;
    bool m_inputing;

    static int m_clock_mod;

    /* address of client (kept for API compatibility; meaning is backend
       defined once RtMidi is wired in) */
    int m_dest_addr_client;
    int m_dest_addr_port;

    int m_local_addr_client;
    int m_local_addr_port;

    /* id of queue */
    int m_queue;

    /* name of bus */
    std::string m_name;

    /* last tick */
    long m_lasttick;

    /* RtMidi (WinMM) backend objects: one of these is non-NULL per bus
       depending on whether it is an output or input bus. */
    RtMidiOut *m_rtmidi_out;
    RtMidiIn  *m_rtmidi_in;

    /* RtMidi port index this bus maps to (into getPortCount() enumeration) */
    int m_port_index;

    /* send a single raw byte (realtime/system messages) */
    void send_byte( unsigned char a_byte );

    /* locking */
    PatchKnobMutex m_mutex;

    void lock();
    void unlock();

 public:

    /* full constructor: local client, destination client/port, bus id, queue.
       (The original took an ALSA snd_seq_t*; removed for the Windows port.) */
    midibus( int a_localclient,
             int a_destclient,
             int a_destport,
             const char *a_client_name,
             const char *a_port_name,
             char a_id,
             int a_queue );

    /* sub/announce-style constructor */
    midibus( int a_localclient,
             char a_id,
             int a_queue );

    ~midibus();

    bool init_out(  );
    bool init_in(  );
    bool deinit_in(  );
    bool init_out_sub(  );
    bool init_in_sub(  );

    void print();

    std::string get_name();
    int get_id();

    /* puts an event in the queue.  a_tick = the event's ABSOLUTE musical
       due-time in sequencer ticks (-1 = "now"): carried through to the audio
       engine so notes land at exact sample offsets instead of block edges. */
    void play( event *a_e24, unsigned char a_channel, long a_tick = -1 );
    void sysex( event *a_e24 );

    /* clock */
    void start();
    void stop();
    void clock(  long a_tick );
    void continue_from( long a_tick );
    void init_clock( long a_tick );
    void set_clock( clock_e a_clocking );
    clock_e get_clock( );

    void set_input( bool a_inputing );
    bool get_input( );

    void flush();

    /* master midi bus sets up the bus */
    friend class mastermidibus;

    int get_client( void ) {  return m_dest_addr_client; };
    int get_port( void ) { return m_dest_addr_port; };

    static void set_clock_mod( int a_clock_mod );
    static int get_clock_mod( void );

};

class mastermidibus
{
 private:

    int m_num_out_buses;
    int m_num_in_buses;

    midibus *m_buses_out[c_maxBuses];
    midibus *m_buses_in[c_maxBuses];
    midibus *m_bus_announce;

    bool m_buses_out_active[c_maxBuses];
    bool m_buses_in_active[c_maxBuses];

    bool m_buses_out_init[c_maxBuses];
    bool m_buses_in_init[c_maxBuses];

    clock_e m_init_clock[c_maxBuses];
    bool m_init_input[c_maxBuses];

    /* id of queue */
    int m_queue;

    int m_ppqn;
    double m_bpm;      /* fractional BPM survives end-to-end (nerf: int truncation) */

    /* for dumping midi input to sequence for recording */
    bool m_dumping_input;
    sequence *m_seq;

    /* buffered raw input messages pulled from RtMidiIn during poll_for_midi,
       drained one at a time by get_midi_event */
    std::deque< std::vector<unsigned char> > m_in_queue;

    /* locking */
    PatchKnobMutex m_mutex;

    void lock();
    void unlock();

 public:

    mastermidibus();
    ~mastermidibus();

    void init();

    int get_num_out_buses();
    int get_num_in_buses();

    void set_bpm(double a_bpm);
    void set_ppqn(int a_ppqn);
    double get_bpm(){ return m_bpm;}
    int get_ppqn(){ return m_ppqn;}

    std::string get_midi_out_bus_name( int a_bus );
    std::string get_midi_in_bus_name( int a_bus );

    void print();
    void flush();

    void start();
    void stop();

    void clock(  long a_tick );
    void continue_from( long a_tick );
    void init_clock( long a_tick );

    int poll_for_midi( );
    bool is_more_input( );
    bool get_midi_event( event *a_in );
    void set_sequence_input( bool a_state, sequence *a_seq );

    bool is_dumping( ) { return m_dumping_input; }
    sequence* get_sequence( ) { return m_seq; }
    void sysex( event *a_event );

    void port_start( int a_client, int a_port );
    void port_exit( int a_client, int a_port );

    /* a_tick: absolute due-time in sequencer ticks (-1 = "now"), forwarded to
       the audio engine for sample-accurate delivery */
    void play( unsigned char a_bus, event *a_e24, unsigned char a_channel,
               long a_tick = -1 );

    void set_clock( unsigned char a_bus, clock_e a_clock_type );
    clock_e get_clock( unsigned char a_bus );

    void set_input( unsigned char a_bus, bool a_inputing );
    bool get_input( unsigned char a_bus );

};

#endif
