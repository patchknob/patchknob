//----------------------------------------------------------------------------
//
//  This file is part of seq24.
//
//  seq24 is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  seq24 is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//-----------------------------------------------------------------------------
//
//  Windows (MINGW64) port: ALSA-free stub backend.
//
//  This is a compile-and-run stub that satisfies the midibus/mastermidibus
//  interface so the GTK frontend and the perform/sequence engine build and
//  launch on Windows.  It performs NO real MIDI I/O yet.  Phase 1 of the port
//  replaces these bodies with RtMidi (WinMM) calls.  Search for "PHASE 1" to
//  find the spots that need real implementations.
//
//-----------------------------------------------------------------------------

#include "midibus.h"
#include <cstdio>

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

    char tmp[128];
    snprintf( tmp, sizeof(tmp), "[%d] seq24 %d", (int) m_id, (int) m_id );
    m_name = tmp;
}

midibus::~midibus()
{
}

bool midibus::init_out()       { return true; }   // PHASE 1: open RtMidi out port
bool midibus::init_in()        { return true; }   // PHASE 1: open RtMidi in port
bool midibus::deinit_in()      { return true; }
bool midibus::init_out_sub()   { return true; }
bool midibus::init_in_sub()    { return true; }

void midibus::print()
{
    printf( "%s\n", m_name.c_str() );
}

std::string midibus::get_name() { return m_name; }
int         midibus::get_id()   { return m_id;   }

void midibus::play( event * /*a_e24*/, unsigned char /*a_channel*/ )
{
    // PHASE 1: encode the event and send via RtMidi.
}

void midibus::sysex( event * /*a_e24*/ )
{
    // PHASE 1: send sysex bytes via RtMidi.
}

void midibus::start()                       { m_lasttick = 0; }
void midibus::stop()                        { m_lasttick = 0; }
void midibus::clock( long /*a_tick*/ )      { }   // PHASE 1: emit MIDI clock
void midibus::continue_from( long a_tick )  { m_lasttick = a_tick; }
void midibus::init_clock( long a_tick )     { m_lasttick = a_tick; }

void    midibus::set_clock( clock_e a_clocking ) { m_clock_type = a_clocking; }
clock_e midibus::get_clock( )                    { return m_clock_type; }

void midibus::set_input( bool a_inputing ) { m_inputing = a_inputing; }
bool midibus::get_input( )                 { return m_inputing; }

void midibus::flush() { }   // PHASE 1: drain RtMidi output

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

void mastermidibus::init()
{
    // PHASE 1: enumerate RtMidi output/input ports and create a midibus per
    // port.  For now create a single stub output bus so the GUI has a target.
    m_num_out_buses = 1;
    m_buses_out[0] = new midibus( 0, 0, 0, "seq24", "stub out", 0, m_queue );
    m_buses_out[0]->init_out();
    m_buses_out_active[0] = true;
    m_buses_out_init[0]   = true;

    m_num_in_buses = 0;

    set_bpm( c_bpm );
    set_ppqn( c_ppqn );
}

int mastermidibus::get_num_out_buses() { return m_num_out_buses; }
int mastermidibus::get_num_in_buses()  { return m_num_in_buses;  }

void mastermidibus::set_bpm( int a_bpm )   { m_bpm  = a_bpm;  }   // PHASE 1: retempo clock
void mastermidibus::set_ppqn( int a_ppqn ) { m_ppqn = a_ppqn; }

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
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->flush();
}

void mastermidibus::start()
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->start();
}

void mastermidibus::stop()
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->stop();
}

void mastermidibus::clock( long a_tick )
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->clock( a_tick );
}

void mastermidibus::continue_from( long a_tick )
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->continue_from( a_tick );
}

void mastermidibus::init_clock( long a_tick )
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->init_clock( a_tick );
}

int  mastermidibus::poll_for_midi( ) { return 0;     }  // PHASE 1: poll RtMidi in
bool mastermidibus::is_more_input( ) { return false; }
bool mastermidibus::get_midi_event( event * /*a_in*/ ) { return false; }

void mastermidibus::set_sequence_input( bool a_state, sequence *a_seq )
{
    lock();
    m_seq           = a_seq;
    m_dumping_input = a_state;
    unlock();
}

void mastermidibus::sysex( event *a_event )
{
    for ( int i = 0; i < m_num_out_buses; ++i )
        if ( m_buses_out_active[i] ) m_buses_out[i]->sysex( a_event );
    flush();
}

void mastermidibus::port_start( int /*a_client*/, int /*a_port*/ ) { }
void mastermidibus::port_exit( int /*a_client*/, int /*a_port*/ )  { }

void mastermidibus::play( unsigned char a_bus, event *a_e24, unsigned char a_channel )
{
    if ( a_bus < m_num_out_buses && m_buses_out_active[a_bus] )
        m_buses_out[a_bus]->play( a_e24, a_channel );
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
            m_buses_in[a_bus]->set_input( a_inputing );
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
