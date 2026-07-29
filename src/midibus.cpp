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
//  Windows (MINGW64) port: RtMidi (WinMM) backend.
//
//  This replaces the original ALSA snd_seq_t implementation with RtMidi, using
//  the WinMM (Windows MultiMedia) backend (-D__WINDOWS_MM__, -lwinmm).  The
//  public midibus / mastermidibus interface (src/midibus.h) is unchanged, so
//  perform / sequence are unaffected.
//
//  Key differences from the ALSA backend (see vendor/rtmidi/INTEGRATION.md):
//    * No shared sequencer handle / output queue.  Each output midibus owns one
//      RtMidiOut; each input midibus owns one RtMidiIn.
//    * flush() is a no-op (WinMM sends immediately).
//    * openVirtualPort() is unsupported on WinMM, so the *_sub (manual virtual
//      port) entry points are no-ops that report failure.
//    * Input is polled via RtMidiIn::getMessage() (non-blocking), not poll(fd).
//    * No port hot-plug events; ports are enumerated once at init().
//
//-----------------------------------------------------------------------------

#include "midibus.h"
#include "RtMidi.h"
#include "audio_app.h"

#include <cstdio>
#include <vector>
#include <deque>
#include <thread>
#include <chrono>

//=============================================================================
//  midibus
//=============================================================================

int midibus::m_clock_mod = 16 * 4;

midibus::midibus( int a_localclient,
                  int a_destclient,
                  int a_destport,
                  const char *a_client_name,
                  const char *a_port_name,
                  char a_id,
                  int a_queue )
{
    m_id               = a_id;
    m_clock_type       = e_clock_off;
    m_inputing         = false;
    m_dest_addr_client = a_destclient;
    m_dest_addr_port   = a_destport;
    m_local_addr_client= a_localclient;
    m_local_addr_port  = -1;
    m_queue            = a_queue;
    m_lasttick         = 0;

    m_rtmidi_out       = NULL;
    m_rtmidi_in        = NULL;
    /* m_dest_addr_port doubles as the RtMidi port index for this bus */
    m_port_index       = a_destport;

    char tmp[128];
    snprintf( tmp, sizeof(tmp), "[%d] %s %s",
              (int) m_id,
              a_client_name ? a_client_name : "midi",
              a_port_name   ? a_port_name   : "" );
    m_name = tmp;
}

midibus::midibus( int a_localclient,
                  char a_id,
                  int a_queue )
{
    m_id               = a_id;
    m_clock_type       = e_clock_off;
    m_inputing         = false;
    m_dest_addr_client = -1;
    m_dest_addr_port   = -1;
    m_local_addr_client= a_localclient;
    m_local_addr_port  = -1;
    m_queue            = a_queue;
    m_lasttick         = 0;

    m_rtmidi_out       = NULL;
    m_rtmidi_in        = NULL;
    m_port_index       = -1;

    char tmp[128];
    snprintf( tmp, sizeof(tmp), "[%d] PatchKnob %d", (int) m_id, (int) m_id );
    m_name = tmp;
}

midibus::~midibus()
{
    if ( m_rtmidi_out != NULL )
    {
        try { m_rtmidi_out->closePort(); } catch ( ... ) {}
        delete m_rtmidi_out;
        m_rtmidi_out = NULL;
    }
    if ( m_rtmidi_in != NULL )
    {
        try { m_rtmidi_in->closePort(); } catch ( ... ) {}
        delete m_rtmidi_in;
        m_rtmidi_in = NULL;
    }
}

/* Open this bus as an output port (m_port_index into RtMidiOut enumeration). */
bool
midibus::init_out()
{
    try
    {
        m_rtmidi_out = new RtMidiOut( RtMidi::WINDOWS_MM, "PatchKnob" );
        m_rtmidi_out->openPort( (unsigned int) m_port_index, m_name );
    }
    catch ( RtMidiError &error )
    {
        printf( "midibus::init_out() failed for \"%s\": %s\n",
                m_name.c_str(), error.getMessage().c_str() );
        if ( m_rtmidi_out != NULL ) { delete m_rtmidi_out; m_rtmidi_out = NULL; }
        return false;
    }
    return true;
}

/* Open this bus as an input port. */
bool
midibus::init_in()
{
    try
    {
        m_rtmidi_in = new RtMidiIn( RtMidi::WINDOWS_MM, "PatchKnob", 256 );
        m_rtmidi_in->openPort( (unsigned int) m_port_index, m_name );
        /* receive everything; don't ignore sysex/timing/active-sense */
        m_rtmidi_in->ignoreTypes( false, false, false );
    }
    catch ( RtMidiError &error )
    {
        printf( "midibus::init_in() failed for \"%s\": %s\n",
                m_name.c_str(), error.getMessage().c_str() );
        if ( m_rtmidi_in != NULL ) { delete m_rtmidi_in; m_rtmidi_in = NULL; }
        return false;
    }
    return true;
}

bool
midibus::deinit_in()
{
    if ( m_rtmidi_in != NULL )
    {
        try { m_rtmidi_in->closePort(); } catch ( ... ) {}
        delete m_rtmidi_in;
        m_rtmidi_in = NULL;
    }
    return true;
}

/* WinMM has no app-creatable virtual ports; openVirtualPort() throws. */
bool midibus::init_out_sub()   { return false; }
bool midibus::init_in_sub()    { return false; }

void midibus::print()
{
    printf( "%s\n", m_name.c_str() );
}

std::string midibus::get_name() { return m_name; }
int         midibus::get_id()   { return m_id;   }

void
midibus::play( event *a_e24, unsigned char a_channel, long a_tick )
{
    /* a_tick (absolute due-time in sequencer ticks, -1 = "now") is carried
       for interface symmetry with mastermidibus::play, which forwards it to
       the audio engine.  This legacy WinMM path sends immediately: the
       sample-accurate hardware route is the patcher's MidiOutNode drain. */
    (void) a_tick;

    if ( m_rtmidi_out == NULL )
        return;

    lock();

    unsigned char status = a_e24->get_status();
    /* status from event has channel cleared (high nibble only); OR in channel,
       unless it is a system message (>= 0xF0). */
    unsigned char message_status = status;
    if ( status < 0xF0 )
        message_status = ( status & 0xF0 ) | ( a_channel & 0x0F );

    unsigned char d0, d1;
    a_e24->get_data( &d0, &d1 );

    std::vector<unsigned char> message;
    message.push_back( message_status );

    /* program change (0xC0) and channel pressure (0xD0) take ONE data byte;
       everything else here (note on/off, CC, pitch bend, aftertouch) takes two. */
    unsigned char type = status & 0xF0;
    if ( type == EVENT_PROGRAM_CHANGE || type == EVENT_CHANNEL_PRESSURE )
    {
        message.push_back( d0 );
    }
    else
    {
        message.push_back( d0 );
        message.push_back( d1 );
    }

    try { m_rtmidi_out->sendMessage( &message ); }
    catch ( RtMidiError & ) {}

    unlock();
}

void
midibus::sysex( event *a_e24 )
{
    if ( m_rtmidi_out == NULL )
        return;

    lock();

    unsigned char *bytes = a_e24->get_sysex();
    long size            = a_e24->get_size();

    if ( bytes != NULL && size > 0 )
    {
        std::vector<unsigned char> message( bytes, bytes + size );
        try { m_rtmidi_out->sendMessage( &message ); }
        catch ( RtMidiError & ) {}
    }

    unlock();
}

/* send a single realtime / system byte */
void
midibus::send_byte( unsigned char a_byte )
{
    if ( m_rtmidi_out == NULL )
        return;

    std::vector<unsigned char> message;
    message.push_back( a_byte );
    try { m_rtmidi_out->sendMessage( &message ); }
    catch ( RtMidiError & ) {}
}

void
midibus::start()
{
    m_lasttick = 0;
    lock();
    send_byte( 0xFA );   /* MIDI Start */
    unlock();
}

void
midibus::stop()
{
    m_lasttick = 0;
    lock();
    send_byte( 0xFC );   /* MIDI Stop */
    unlock();
}

/*  Emit MIDI clock (0xF8) bytes for each tick boundary crossed since the last
    call: one clock every (c_ppqn / 24) ticks.  Boundaries are computed
    arithmetically -- the original per-tick m_lasttick++ walk was O(delta) on
    the output thread's hot path (at 500 BPM the clock period is shorter than
    the loop period, so every call crossed boundaries). */
void
midibus::clock( long a_tick )
{
    lock();

    if ( m_clock_type != e_clock_off && a_tick > m_lasttick )
    {
        const long interval = c_ppqn / 24;

        /* first multiple of interval STRICTLY after m_lasttick (floor
           division: m_lasttick may be -1 after continue_from/init_clock,
           and boundary 0 must still fire) */
        long q = m_lasttick / interval;
        if ( m_lasttick < 0 && ( m_lasttick % interval ) != 0 )
            q--;
        long boundary = ( q + 1 ) * interval;

        /* one 0xF8 per boundary in (m_lasttick, a_tick] */
        for ( ; boundary <= a_tick; boundary += interval )
            send_byte( 0xF8 );   /* MIDI Clock */

        m_lasttick = a_tick;
    }

    unlock();
}

void
midibus::continue_from( long a_tick )
{
    /* tell the device where we are, then continue */
    long pp16th    = ( c_ppqn / 4 );
    long leftover  = ( a_tick % pp16th );
    long beats     = ( a_tick / pp16th );
    long starttick = a_tick - leftover;

    m_lasttick = starttick - 1;

    lock();
    if ( m_clock_type != e_clock_off )
    {
        /* Song Position Pointer (0xF2 lsb msb), in MIDI beats (16th notes) */
        std::vector<unsigned char> spp;
        spp.push_back( 0xF2 );
        spp.push_back( (unsigned char) ( beats & 0x7F ) );
        spp.push_back( (unsigned char) ( ( beats >> 7 ) & 0x7F ) );
        if ( m_rtmidi_out != NULL )
        {
            try { m_rtmidi_out->sendMessage( &spp ); }
            catch ( RtMidiError & ) {}
        }
        send_byte( 0xFB );   /* MIDI Continue */
    }
    unlock();
}

void
midibus::init_clock( long a_tick )
{
    if ( m_clock_type == e_clock_pos && a_tick != 0 )
    {
        continue_from( a_tick );
    }
    else if ( m_clock_type == e_clock_mod || a_tick == 0 )
    {
        start();

        /* the clock mod doubles the number of ticks per quarter note so the
           device's clock count aligns to the modulus boundary */
        long clock_mod_ticks = ( c_ppqn / 4 ) * m_clock_mod;
        long leftover        = ( a_tick % clock_mod_ticks );
        long starttick       = a_tick - leftover;
        if ( leftover != 0 )
            starttick += clock_mod_ticks;

        m_lasttick = starttick - 1;
    }
}

void    midibus::set_clock( clock_e a_clocking ) { m_clock_type = a_clocking; }
clock_e midibus::get_clock( )                    { return m_clock_type; }

void midibus::set_input( bool a_inputing ) { m_inputing = a_inputing; }
bool midibus::get_input( )                 { return m_inputing; }

/* WinMM sends immediately; nothing to drain. */
void midibus::flush() { }

void midibus::lock()   { m_mutex.lock();   }
void midibus::unlock() { m_mutex.unlock(); }

void midibus::set_clock_mod( int a_clock_mod )
{
    if ( a_clock_mod != 0 )
        m_clock_mod = a_clock_mod;
}
int  midibus::get_clock_mod( ) { return m_clock_mod; }

//=============================================================================
//  mastermidibus
//=============================================================================

mastermidibus::mastermidibus()
{
    m_num_out_buses = 0;
    m_num_in_buses  = 0;
    m_bus_announce  = NULL;
    m_queue         = 0;
    m_ppqn          = c_ppqn;
    m_bpm           = c_bpm;
    m_dumping_input = false;
    m_seq           = NULL;

    for ( int i = 0; i < c_maxBuses; ++i )
    {
        m_buses_out[i]        = NULL;
        m_buses_in[i]         = NULL;
        m_buses_out_active[i] = false;
        m_buses_in_active[i]  = false;
        m_buses_out_init[i]   = false;
        m_buses_in_init[i]    = false;
        m_init_clock[i]       = e_clock_off;
        m_init_input[i]       = false;
    }
}

mastermidibus::~mastermidibus()
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        delete m_buses_out[i];
    for ( int i = 0; i < m_num_in_buses; ++i )
        delete m_buses_in[i];
}

void
mastermidibus::init()
{
    /* ----- enumerate output ports ----- */
    unsigned int out_count = 0;
    try
    {
        RtMidiOut probe( RtMidi::WINDOWS_MM, "PatchKnob" );
        out_count = probe.getPortCount();

        for ( unsigned int i = 0;
              i < out_count && m_num_out_buses < c_maxBuses; ++i )
        {
            std::string pname;
            try { pname = probe.getPortName( i ); }
            catch ( RtMidiError & ) { pname = "unknown"; }

            int bus = m_num_out_buses;
            /* destport == port index i */
            m_buses_out[bus] = new midibus( 0, 0, (int) i,
                                            "out", pname.c_str(),
                                            (char) bus, m_queue );
            if ( m_buses_out[bus]->init_out() )
            {
                m_buses_out_active[bus] = true;
                m_buses_out_init[bus]   = true;
                m_buses_out[bus]->set_clock( m_init_clock[bus] );
                m_num_out_buses++;
            }
            else
            {
                delete m_buses_out[bus];
                m_buses_out[bus] = NULL;
            }
        }
    }
    catch ( RtMidiError &error )
    {
        printf( "mastermidibus::init() RtMidiOut probe failed: %s\n",
                error.getMessage().c_str() );
    }

    /* ----- enumerate input ports ----- */
    unsigned int in_count = 0;
    try
    {
        RtMidiIn probe( RtMidi::WINDOWS_MM, "PatchKnob", 256 );
        in_count = probe.getPortCount();

        for ( unsigned int i = 0;
              i < in_count && m_num_in_buses < c_maxBuses; ++i )
        {
            std::string pname;
            try { pname = probe.getPortName( i ); }
            catch ( RtMidiError & ) { pname = "unknown"; }

            int bus = m_num_in_buses;
            m_buses_in[bus] = new midibus( 0, 0, (int) i,
                                           "in", pname.c_str(),
                                           (char) bus, m_queue );
            /* create the bus but only open the input port when enabled; the
               original ALSA backend likewise enabled inputs on demand.  We do
               open it here so it can record immediately if enabled. */
            m_buses_in_active[bus] = true;
            m_buses_in_init[bus]   = true;

            if ( m_init_input[bus] )
            {
                if ( m_buses_in[bus]->init_in() )
                    m_buses_in[bus]->set_input( true );
            }
            m_num_in_buses++;
        }
    }
    catch ( RtMidiError &error )
    {
        printf( "mastermidibus::init() RtMidiIn probe failed: %s\n",
                error.getMessage().c_str() );
    }

    printf( "mastermidibus::init(): %d MIDI output bus(es), %d input bus(es)\n",
            m_num_out_buses, m_num_in_buses );
    for ( int i = 0; i < m_num_out_buses; ++i )
        printf( "  out %s\n", m_buses_out[i]->get_name().c_str() );
    for ( int i = 0; i < m_num_in_buses; ++i )
        printf( "  in  %s\n", m_buses_in[i]->get_name().c_str() );

    set_bpm( c_bpm );
    set_ppqn( c_ppqn );
}

int mastermidibus::get_num_out_buses() { return m_num_out_buses; }
int mastermidibus::get_num_in_buses()  { return m_num_in_buses;  }

/* BPM stays double end-to-end (fractional / externally-synced tempi); only
   the wire format (0xF8 boundary ticks) quantizes, and that is integer tick
   math downstream. */
void mastermidibus::set_bpm( double a_bpm ) { m_bpm  = a_bpm;  }
void mastermidibus::set_ppqn( int a_ppqn )  { m_ppqn = a_ppqn; }

std::string mastermidibus::get_midi_out_bus_name( int a_bus )
{
    if ( a_bus >= 0 && a_bus < m_num_out_buses && m_buses_out[a_bus] )
        return m_buses_out[a_bus]->get_name();
    return std::string( "missing" );
}

std::string mastermidibus::get_midi_in_bus_name( int a_bus )
{
    if ( a_bus >= 0 && a_bus < m_num_in_buses && m_buses_in[a_bus] )
        return m_buses_in[a_bus]->get_name();
    return std::string( "missing" );
}

void mastermidibus::print()
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out[i] ) m_buses_out[i]->print();
}

void mastermidibus::flush()
{
    /* no-op on WinMM (immediate send) */
}

// Automatic sequencer -> hardware MIDI output.  DEFAULT OFF: MIDI routing is now
// done in the patcher (MIDI-out nodes), PipeWire-style, so the sequencer no
// longer blasts every event to every hardware port.  Toggle if legacy direct
// hardware output is wanted.
static bool s_hw_midi_out_enabled = false;
void mastermidibus_set_hw_output( bool on ) { s_hw_midi_out_enabled = on; }
bool mastermidibus_hw_output()              { return s_hw_midi_out_enabled; }

/*  The clock / transport realtime sends below (0xF8/0xFA/0xFB/0xFC/SPP) are
    gated on s_hw_midi_out_enabled, symmetric with ::play.  These run off the
    wall-clock output thread; in the patcher configuration the ONE clock
    source is the sample-accurate MasterMixer clock routed through MIDI-out
    nodes, and this legacy second time base must stay silent. */

void mastermidibus::start()
{
    if ( !s_hw_midi_out_enabled )
        return;
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->start();
}

void mastermidibus::stop()
{
    if ( !s_hw_midi_out_enabled )
        return;
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->stop();
}

void mastermidibus::clock( long a_tick )
{
    if ( !s_hw_midi_out_enabled )
        return;
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->clock( a_tick );
}

void mastermidibus::continue_from( long a_tick )
{
    if ( !s_hw_midi_out_enabled )
        return;
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->continue_from( a_tick );
}

void mastermidibus::init_clock( long a_tick )
{
    /* init_clock reaches hardware through midibus::start()/continue_from(),
       so it is gated with the rest of the legacy clock path */
    if ( !s_hw_midi_out_enabled )
        return;
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->init_clock( a_tick );
}

/*  Poll all input buses for queued MIDI.  Returns >0 if any bus has at least
    one message waiting.  The pulled message is buffered so get_midi_event()
    can return it next.  Non-blocking. */
int
mastermidibus::poll_for_midi()
{
    lock();

    for ( int i = 0; i < m_num_in_buses; ++i )
    {
        if ( !m_buses_in_active[i] || !m_buses_in[i] )
            continue;

        RtMidiIn *in = m_buses_in[i]->m_rtmidi_in;
        if ( in == NULL )
            continue;

        std::vector<unsigned char> msg;
        try { in->getMessage( &msg ); }
        catch ( RtMidiError & ) { continue; }

        if ( !msg.empty() )
        {
            m_in_queue.push_back( msg );
        }
    }

    int count = (int) m_in_queue.size();
    unlock();

    if ( count == 0 )
    {
        // The ALSA backend blocked in poll(); RtMidi's getMessage() is non-
        // blocking, so without this the perform input thread (input_func) spins
        // a full CPU core.  Throttle to ~1 kHz: sub-millisecond MIDI-in latency,
        // negligible CPU.
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    return count;
}

bool
mastermidibus::is_more_input()
{
    lock();
    bool more = !m_in_queue.empty();
    unlock();
    return more;
}

/*  Pop the next buffered input message and convert it into a PatchKnob event.
    Replicates the ALSA backend's Note-On-velocity-0 -> Note-Off fixup. */
bool
mastermidibus::get_midi_event( event *a_in )
{
    lock();

    if ( m_in_queue.empty() )
    {
        unlock();
        return false;
    }

    std::vector<unsigned char> msg = m_in_queue.front();
    m_in_queue.pop_front();

    unlock();

    if ( msg.empty() )
        return false;

    unsigned char status = msg[0];

    /* sysex */
    if ( status == EVENT_SYSEX )
    {
        a_in->set_status( EVENT_SYSEX );
        a_in->start_sysex();
        a_in->append_sysex( &msg[0], (long) msg.size() );
        return true;
    }

    a_in->set_status( status );

    unsigned char d0 = ( msg.size() > 1 ) ? msg[1] : 0;
    unsigned char d1 = ( msg.size() > 2 ) ? msg[2] : 0;
    a_in->set_data( d0, d1 );

    /* Note-On with velocity 0 is a Note-Off */
    if ( a_in->is_note_on() && d1 == 0 )
    {
        a_in->set_status( EVENT_NOTE_OFF );
        a_in->set_data( d0, d1 );
    }

    a_in->set_size( msg.size() );

    return true;
}

void
mastermidibus::set_sequence_input( bool a_state, sequence *a_seq )
{
    lock();
    m_seq           = a_seq;
    m_dumping_input = a_state;
    unlock();
}

void
mastermidibus::sysex( event *a_event )
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->sysex( a_event );
    flush();
}

void mastermidibus::port_start( int /*a_client*/, int /*a_port*/ ) { }
void mastermidibus::port_exit( int /*a_client*/, int /*a_port*/ )  { }

void mastermidibus::play( unsigned char a_bus, event *a_e24, unsigned char a_channel,
                          long a_tick )
{
    /* hardware / external MIDI out -- only when explicitly enabled */
    if ( s_hw_midi_out_enabled && a_bus < m_num_out_buses && m_buses_out_active[a_bus] )
        m_buses_out[a_bus]->play( a_e24, a_channel, a_tick );

    /* also route this event to the bus's hosted VST instrument (track == bus).
       Channel voice messages only (status < 0xF0). Lock-free; safe on the
       output thread.  a_tick (absolute sequencer tick, -1 = "now") rides the
       ring so the audio thread can place the event at its exact sample offset
       instead of the block edge. */
    unsigned char status = a_e24->get_status();
    if ( status < 0xF0 )
    {
        unsigned char msg = ( status & 0xF0 ) | ( a_channel & 0x0F );
        unsigned char d0, d1;
        a_e24->get_data( &d0, &d1 );
        PatchKnob::app::audio_app_route_midi( (int) a_bus, msg, d0, d1,
                                          (long long) a_tick );
    }
}

void mastermidibus::set_clock( unsigned char a_bus, clock_e a_clock_type )
{
    if ( a_bus < c_maxBuses )
    {
        m_init_clock[a_bus] = a_clock_type;
        if ( a_bus < m_num_out_buses && m_buses_out[a_bus] )
            m_buses_out[a_bus]->set_clock( a_clock_type );
    }
}

clock_e mastermidibus::get_clock( unsigned char a_bus )
{
    if ( a_bus < m_num_out_buses && m_buses_out[a_bus] )
        return m_buses_out[a_bus]->get_clock();
    return e_clock_off;
}

void mastermidibus::set_input( unsigned char a_bus, bool a_inputing )
{
    if ( a_bus < c_maxBuses )
    {
        m_init_input[a_bus] = a_inputing;

        if ( a_bus < m_num_in_buses && m_buses_in[a_bus] )
        {
            if ( a_inputing )
            {
                /* open the input port on demand if not already open */
                if ( m_buses_in[a_bus]->m_rtmidi_in == NULL )
                    m_buses_in[a_bus]->init_in();
                m_buses_in[a_bus]->set_input( true );
            }
            else
            {
                m_buses_in[a_bus]->set_input( false );
                m_buses_in[a_bus]->deinit_in();
            }
        }
    }
}

bool mastermidibus::get_input( unsigned char a_bus )
{
    if ( a_bus < m_num_in_buses && m_buses_in[a_bus] )
        return m_buses_in[a_bus]->get_input();
    return false;
}

void mastermidibus::lock()   { m_mutex.lock();   }
void mastermidibus::unlock() { m_mutex.unlock(); }
