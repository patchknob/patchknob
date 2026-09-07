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
//  You should have received a copy of the GNU General Public License
//  along with PatchKnob; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//-----------------------------------------------------------------------------

#include "optionsfile.h"
#include <iostream>

optionsfile::optionsfile( string a_name ) :
    configfile( a_name )
{
}

optionsfile::~optionsfile( )
{

}



/*  optionsfile is the SINGLE owner of the legacy MIDI/transport config
    sections: [midi-control], [midi-clock], [midi-clock-mod-ticks],
    [midi-input], [keyboard-control], [jack-transport], [manual-alsa-ports].
    Nothing else may read or write them (userfile.cpp's dead duplicate parser
    was deleted for exactly this reason).

    HONEST STATUS: nothing in the SDL app constructs an optionsfile, so this
    parser does not currently run -- the modern config lives in the project
    file (sdlui/project_io.cpp).  It is kept because it is the persistence
    format for the legacy sections and the only definition of them.  If it is
    ever wired back up, it must stay the only wiring. */
bool
optionsfile::parse( perform *a_perf )
{

    /* file size */
    int file_size = 0;

    /* open binary file */
    ifstream file ( m_name.c_str(), ios::in | ios::ate );

    if( ! file.is_open() )
        return false;
    
    file_size = file.tellg();

    /* run to start */
    file.seekg( 0, ios::beg );

    line_after( &file, "[midi-control]" );

    int sequences = 0;
    sscanf( m_line, "%d", &sequences );
    next_data_line( &file );

    for ( int i=0; i<sequences; ++i ){

        int sequence = 0;
        
        sscanf( m_line, "%d [ %d %d %ld %ld %ld %ld ] [ %d %d %ld %ld %ld %ld ] [ %d %d %ld %ld %ld %ld ]",
                
                &sequence,
                
                &a_perf->get_midi_control_toggle(i)->m_active,
                &a_perf->get_midi_control_toggle(i)->m_inverse_active,
                &a_perf->get_midi_control_toggle(i)->m_status,
                &a_perf->get_midi_control_toggle(i)->m_data,
                &a_perf->get_midi_control_toggle(i)->m_min_value,
                &a_perf->get_midi_control_toggle(i)->m_max_value,
                
                &a_perf->get_midi_control_on(i)->m_active,
                &a_perf->get_midi_control_on(i)->m_inverse_active,
                &a_perf->get_midi_control_on(i)->m_status,
                &a_perf->get_midi_control_on(i)->m_data,
                &a_perf->get_midi_control_on(i)->m_min_value,
                &a_perf->get_midi_control_on(i)->m_max_value,
                
                &a_perf->get_midi_control_off(i)->m_active,
                &a_perf->get_midi_control_off(i)->m_inverse_active,
                &a_perf->get_midi_control_off(i)->m_status,
                &a_perf->get_midi_control_off(i)->m_data,
                &a_perf->get_midi_control_off(i)->m_min_value,
                &a_perf->get_midi_control_off(i)->m_max_value );
         
        next_data_line( &file );
    }

    line_after( &file, "[midi-clock]" );
    long buses = 0;
    sscanf( m_line, "%ld", &buses );
    next_data_line( &file );

    for ( int i=0; i<buses; ++i ){

        /* initialised + checked: a short/garbled line used to leave these
           indeterminate and then index the bus arrays with them */
        long bus_on = 0, bus = -1;
        if ( sscanf( m_line, "%ld %ld", &bus, &bus_on ) == 2 &&
             bus >= 0 && bus < c_maxBuses )
            a_perf->get_master_midi_bus( )->set_clock( (unsigned char) bus,
                                                       (clock_e) bus_on );
        next_data_line( &file );
    }


    line_after( &file, "[keyboard-control]" );
    long keys = 0;
    sscanf( m_line, "%ld", &keys );
    next_data_line( &file );

    a_perf->key_events.clear();
    
    
    for ( int i=0; i<keys; ++i ){
        
        long key = 0, seq = 0;
        sscanf( m_line, "%ld %ld", &key, &seq );
        a_perf->key_events[key] = seq;
        next_data_line( &file );
    }

    sscanf( m_line, "%ld %ld", &a_perf->m_key_bpm_up,
                             &a_perf->m_key_bpm_dn );
    next_data_line( &file );

    sscanf( m_line, "%ld %ld", &a_perf->m_key_screenset_up,
                             &a_perf->m_key_screenset_dn );
    next_data_line( &file );

    sscanf( m_line, "%ld %ld %ld %ld",
            &a_perf->m_key_replace,
            &a_perf->m_key_queue,
            &a_perf->m_key_snapshot_1,
            &a_perf->m_key_snapshot_2 );

    line_after( &file, "[jack-transport]" );
    long flag = 0;
    
    sscanf( m_line, "%ld", &flag );
    global_with_jack_transport = (bool) flag;
    
    next_data_line( &file );
    sscanf( m_line, "%ld", &flag );
    global_with_jack_master = (bool) flag;
    
    next_data_line( &file );
    sscanf( m_line, "%ld", &flag );
    global_with_jack_master_cond = (bool) flag;
    
    next_data_line( &file );
    sscanf( m_line, "%ld", &flag );
    global_jack_start_mode = (bool) flag;


    line_after( &file, "[midi-input]" );
    buses = 0;
    sscanf( m_line, "%ld", &buses );
    next_data_line( &file );

    for ( int i=0; i<buses; ++i ){

        long bus_on = 0, bus = -1;
        if ( sscanf( m_line, "%ld %ld", &bus, &bus_on ) == 2 &&
             bus >= 0 && bus < c_maxBuses )
            a_perf->get_master_midi_bus( )->set_input( (unsigned char) bus,
                                                       (bool) bus_on );
        next_data_line( &file );
    }

    /*  midi clock mod -- THE ONLY reader of [midi-clock-mod-ticks] in the
        tree, matched by the only writer (optionsfile::write below).
        userfile.cpp used to carry an identical second parser; it was deleted
        so load order can no longer decide this value.

        line_after() cannot report a missing section (it just runs to EOF and
        leaves m_line holding whatever was last read), so trust the value only
        when sscanf actually converted one; otherwise leave midibus's default
        standing instead of writing a garbage or stale number into the clock. */
    {
        long ticks = 0;
        line_after( &file, "[midi-clock-mod-ticks]" );
        if ( sscanf( m_line, "%ld", &ticks ) == 1 )
            midibus::set_clock_mod( (int) ticks );
    }


    /* manual alsa ports */
    line_after( &file, "[manual-alsa-ports]" );
    sscanf( m_line, "%ld", &flag );
    global_manual_alsa_ports = (bool) flag;

    file.close();

    return true;
    
}


bool 
optionsfile::write( perform *a_perf  )
{
    /* open binary file */
 
    ofstream file ( m_name.c_str(), ios::out | ios::trunc  );
    char outs[1024];

    if( ! file.is_open() )
        return false;
    
 
	
    /* midi control */

    file << "#\n";
    file << "# Seq 24 Init File\n";
    file << "#\n\n\n";
    
    file << "[midi-control]\n";
    file <<  c_midi_controls << "\n";

    for (int i=0; i< c_midi_controls; i++ ){


        switch( i ){

            case c_midi_control_bpm_up       :  file << "# bpm up\n"; break;
            case c_midi_control_bpm_dn       :  file << "# bpm down\n"; break;
            case c_midi_control_ss_up        :  file << "# screen set up\n"; break;
            case c_midi_control_ss_dn        :  file << "# screen set down\n"; break;
            case c_midi_control_mod_replace  :  file << "# mod replace\n"; break;
            case c_midi_control_mod_snapshot :  file << "# mod snapshot\n"; break;
            case c_midi_control_mod_queue    :  file << "# mod queue\n"; break;

            default: break;
        }
        
        sprintf( outs, "%d [%1d %1d %3ld %3ld %3ld %3ld] [%1d %1d %3ld %3ld %3ld %3ld] [%1d %1d %3ld %3ld %3ld %3ld]",

                 i,
                 
                 a_perf->get_midi_control_toggle(i)->m_active,
                 a_perf->get_midi_control_toggle(i)->m_inverse_active,
                 a_perf->get_midi_control_toggle(i)->m_status,
                 a_perf->get_midi_control_toggle(i)->m_data,
                 a_perf->get_midi_control_toggle(i)->m_min_value,
                 a_perf->get_midi_control_toggle(i)->m_max_value,
                 
                 a_perf->get_midi_control_on(i)->m_active,
                 a_perf->get_midi_control_on(i)->m_inverse_active,
                 a_perf->get_midi_control_on(i)->m_status,
                 a_perf->get_midi_control_on(i)->m_data,
                 a_perf->get_midi_control_on(i)->m_min_value,
                 a_perf->get_midi_control_on(i)->m_max_value,
                 
                 a_perf->get_midi_control_off(i)->m_active,
                 a_perf->get_midi_control_off(i)->m_inverse_active,
                 a_perf->get_midi_control_off(i)->m_status,
                 a_perf->get_midi_control_off(i)->m_data,
                 a_perf->get_midi_control_off(i)->m_min_value,
                 a_perf->get_midi_control_off(i)->m_max_value );
        

        file << string(outs) << "\n";
    }

 

    /* bus mute/unmute data */
    int buses = a_perf->get_master_midi_bus( )->get_num_out_buses();

    file << "\n\n\n[midi-clock]\n";
    file << buses << "\n";
    
    for (int i=0; i< buses; i++ ){


        file << "# " << a_perf->get_master_midi_bus( )->get_midi_out_bus_name(i) << "\n";
        sprintf( outs, "%d %d", i, (char) a_perf->get_master_midi_bus( )->get_clock(i));
        file << outs << "\n";
    }

    /* midi clock mod -- the matching single writer for the single reader in
       ::parse.  Keep these two the only places that touch this section. */
    file << "\n\n[midi-clock-mod-ticks]\n";
    file << midibus::get_clock_mod() << "\n";


    

    /* bus input data */
    buses = a_perf->get_master_midi_bus( )->get_num_in_buses();

    file << "\n\n\n[midi-input]\n";
    file << buses << "\n";
    
    for (int i=0; i< buses; i++ ){


        file << "# " << a_perf->get_master_midi_bus( )->get_midi_in_bus_name(i) << "\n";
        sprintf( outs, "%d %d", i, (char) a_perf->get_master_midi_bus( )->get_input(i));
        file << outs << "\n";
    }


    /* manual alsa ports */
    file << "\n\n\n[manual-alsa-ports]\n";
    file << "# set to 1 if you want PatchKnob to create its own alsa ports and\n";
    file << "# not connect to other clients\n";
    file << global_manual_alsa_ports << "\n";
    
 

    file << "\n\n\n[keyboard-control]\n";
    file << "# Key #, Sequence # \n";
    file << c_seqs_in_set << "\n";

    for( std::map<long,long>::iterator i = a_perf->key_events.begin();
         i != a_perf->key_events.end(); ++i ){
        
        sprintf( outs, "%ld  %ld", i->first, i->second );
        file << string(outs) << "\n";
    }

    file << "# bpm up, down\n"
         << a_perf->m_key_bpm_up
         << " "
         << a_perf->m_key_bpm_dn << "\n";

    file << "# screen set up, down\n"
         << a_perf->m_key_screenset_up
         << " "
         << a_perf->m_key_screenset_dn
         << "\n";

    file << "# replace, queue, snapshot_1, snapshot 2\n"
         << a_perf->m_key_replace << " "
         << a_perf->m_key_queue << " "
         << a_perf->m_key_snapshot_1 << " "
         << a_perf->m_key_snapshot_2 << "\n";

    file << "\n\n\n[jack-transport]\n\n"

         
         << "# jack_transport - Enable sync with JACK Transport.\n" 
         << global_with_jack_transport << "\n\n"

         << "# jack_master - PatchKnob will attempt to serve as JACK Master.\n"
         << global_with_jack_master << "\n\n"

         << "# jack_master_cond -  PatchKnob will fail to be master if there is already a master set.\n"
         << global_with_jack_master_cond  << "\n\n"

         << "# jack_start_mode\n"
         << "# 0 = Playback will be in live mode.  Use this to allow muting and unmuting of loops.\n" 
         << "# 1 = Playback will use the song editors data.\n"
         << global_jack_start_mode << "\n\n";

    
    file.close();
    return true;

}
