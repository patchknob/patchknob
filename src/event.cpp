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
#include "event.h"

#include <cstring>

event::event()
{
    m_column = c_no_column;
    m_timestamp = 0;
    m_status = EVENT_NOTE_OFF;
    m_data[0] = 0;
    m_data[1] = 0;

    m_sysex = NULL;
    m_size = 0;

    m_linked = NULL;
    m_selected = false;
    m_marked = false;
    m_has_link = false;
    m_painted = false;
} 

event::~event()
{
  if ( m_sysex != NULL )
    delete[] m_sysex;
  
  m_sysex = NULL;
}

/* Deep-copy the owned sysex buffer -- see the note in event.h. */
event::event( const event &a_ev )
{
  m_column    = a_ev.m_column;
  m_timestamp = a_ev.m_timestamp;
  m_status    = a_ev.m_status;
  m_data[0]   = a_ev.m_data[0];
  m_data[1]   = a_ev.m_data[1];
  m_linked    = a_ev.m_linked;
  m_has_link  = a_ev.m_has_link;
  m_selected  = a_ev.m_selected;
  m_marked    = a_ev.m_marked;
  m_painted   = a_ev.m_painted;
  m_size      = 0;
  m_sysex     = NULL;

  if ( a_ev.m_sysex != NULL && a_ev.m_size > 0 )
  {
    m_sysex = new unsigned char[a_ev.m_size];
    memcpy( m_sysex, a_ev.m_sysex, a_ev.m_size );
    m_size  = a_ev.m_size;
  }
}

event &
event::operator=( const event &a_ev )
{
  if ( this == &a_ev )
    return *this;

  m_column    = a_ev.m_column;
  m_timestamp = a_ev.m_timestamp;
  m_status    = a_ev.m_status;
  m_data[0]   = a_ev.m_data[0];
  m_data[1]   = a_ev.m_data[1];
  m_linked    = a_ev.m_linked;
  m_has_link  = a_ev.m_has_link;
  m_selected  = a_ev.m_selected;
  m_marked    = a_ev.m_marked;
  m_painted   = a_ev.m_painted;

  /* build the new buffer before releasing the old one, so a throwing
     allocation cannot leave this event holding a freed pointer */
  unsigned char *buf = NULL;
  long           sz  = 0;
  if ( a_ev.m_sysex != NULL && a_ev.m_size > 0 )
  {
    buf = new unsigned char[a_ev.m_size];
    memcpy( buf, a_ev.m_sysex, a_ev.m_size );
    sz  = a_ev.m_size;
  }

  if ( m_sysex != NULL )
    delete[] m_sysex;

  m_sysex = buf;
  m_size  = sz;

  return *this;
}

long 
event::get_timestamp()
{ 
    return m_timestamp; 
}

void 
event::set_timestamp( const unsigned long a_time )
{
    m_timestamp = a_time;
}

void 
event::mod_timestamp( unsigned long a_mod )
{
    /*  a_mod is the pattern length.  At zero this divided by zero, and because
        a_mod is UNSIGNED a negative timestamp was promoted to a huge positive
        one instead of wrapping -- either way the event landed nowhere real.

        The negative check has to be made on a SIGNED view of the field.
        `m_timestamp < 0` is unsigned-compared and therefore always false, so
        the guard below never once fired: a timestamp that had been set from a
        negative long (a live event stamped before the transport origin) still
        wrapped as a huge unsigned value and landed on an arbitrary tick inside
        the pattern -- in range, and in the wrong place, which is worse than out
        of range because nothing downstream can tell. */
    if ( a_mod == 0 )
        return;

    const long signed_ts = (long) m_timestamp;
    if ( signed_ts < 0 ){
        m_timestamp = 0;
        return;
    }
    m_timestamp = (unsigned long)( signed_ts % (long) a_mod );
}

void 
event::set_status( const char a_status  )
{
   /* bitwise AND to clear the channel portion of the status */
    if ( (unsigned char) a_status >= 0xF0 )
      m_status = (char) a_status;
    else
      m_status = (char) (a_status & EVENT_CLEAR_CHAN_MASK);
}

void 
event::make_clock( )
{
    m_status = (unsigned char) EVENT_MIDI_CLOCK;
}

void 
event::set_data( char a_D1  )
{
    m_data[0] = a_D1 & 0x7F;
}

void 
event::set_data( char a_D1, char a_D2 )
{
    m_data[0] = a_D1 & 0x7F;
    m_data[1] = a_D2 & 0x7F;
}

void 
event::increment_data2(void )
{
	m_data[1] = (m_data[1]+1) & 0x7F;
}

void 
event::decrement_data2(void )
{
	m_data[1] = (m_data[1]-1) & 0x7F;
}



void 
event::increment_data1(void )
{
	m_data[0] = (m_data[0]+1) & 0x7F;
}

void 
event::decrement_data1(void )
{
	m_data[0] = (m_data[0]-1) & 0x7F;
}


void 
event::get_data( unsigned char *D0, unsigned char *D1 )
{
    *D0 = m_data[0]; 
    *D1 = m_data[1];
}

unsigned char 
event::get_status( ) 
{ 
    return m_status; 
}


void 
event::start_sysex( void  )
{
  if ( m_sysex != NULL )
    delete[] m_sysex;

  m_sysex = NULL;
  m_size = 0;
}

bool
event::append_sysex( unsigned char *a_data, long a_size )
{
  bool ret = true;

  unsigned char *buffer = new unsigned char[m_size + a_size];

  /* copy old and append */
  memcpy(  buffer        , m_sysex, m_size );
  memcpy( &buffer[m_size], a_data, a_size );

  if ( m_sysex != NULL )
    delete[] m_sysex;

  m_size = m_size + a_size;
  m_sysex = buffer;

  for ( int i=0; i<a_size; i++ ){

    if ( a_data[i] == EVENT_SYSEX_END )
      ret = false;
  }
    
  return ret;

}


unsigned char *
event::get_sysex( void )
{
  return m_sysex;
}



void
event::set_size( long a_size )
{
  m_size = a_size;
}

long
event::get_size( void )
{
  return m_size;
}

void 
event::set_note_velocity( int a_vel )
{
    m_data[1] = a_vel & 0x7F; 
}

bool 
event::is_note_on()
{
    return (m_status == EVENT_NOTE_ON); 
}

bool 
event::is_note_off()
{
    return (m_status == EVENT_NOTE_OFF);
}

unsigned char 
event::get_note()
{
    return m_data[0];
}

void 
event::set_note( char a_note )
{
    m_data[0] = a_note & 0x7F;
}

unsigned char 
event::get_note_velocity()
{
    return m_data[1];
}


void 
event::print()
{
    printf( "[%06ld] [%04lX] %02X ",
	    m_timestamp,
	    m_size,
	    m_status );

    if ( m_status == EVENT_SYSEX ){

      for( int i=0; i<m_size; i++ ){

	if ( i%16 == 0 )
	  printf( "\n    " );

	printf( "%02X ", m_sysex[i] );

      }

      printf( "\n" );
    }
    else {

      printf( "%02X %02X\n",
	      m_data[0],
	      m_data[1] );
    }
}

int 
event::get_rank( void ) const
{
    switch ( m_status )
    {
        case EVENT_NOTE_OFF:
            return 0x080;
        case EVENT_NOTE_ON:
            return 0x090;

        case EVENT_AFTERTOUCH:
        case EVENT_CHANNEL_PRESSURE:
        case EVENT_PITCH_WHEEL: 
            return 0x050;
            
        case EVENT_CONTROL_CHANGE:
            return 0x010;
        case EVENT_PROGRAM_CHANGE:
            return 0x000;
        default:
            return 0;
    }
}

bool 
event::operator>( const event &a_rhsevent ) const
{
    if ( m_timestamp == a_rhsevent.m_timestamp )
    {
        return (get_rank() > a_rhsevent.get_rank());
    }
    else
    {
        return (m_timestamp > a_rhsevent.m_timestamp);
    }
}


bool 
event::operator<( const event &a_rhsevent ) const
{ 
    if ( m_timestamp == a_rhsevent.m_timestamp )
    {
        return (get_rank() < a_rhsevent.get_rank());
    }
    else
    {
        return (m_timestamp < a_rhsevent.m_timestamp);
    }
}

bool 
event::operator<=( const unsigned long &a_rhslong ) const
{ 
    return (m_timestamp <= a_rhslong); 
}   



bool 
event::operator>( const unsigned long &a_rhslong ) const
{ 
    return (m_timestamp > a_rhslong); 
}   

void 
event::link( event *a_event )
{
    m_has_link = true;
    m_linked = a_event;
}

event*
event::get_linked( )
{
    return m_linked;
}

bool
event::is_linked( )
{
    return m_has_link;
}

void
event::clear_link( )
{
    m_has_link = false;
}

void 
event::select( )
{
    m_selected = true;
}

void 
event::unselect( )
{
    m_selected = false;
}

bool 
event::is_selected( )
{
    return m_selected;
}
void 
event::paint( )
{
    m_painted = true;
}

void 
event::unpaint( )
{
    m_painted = false;
}

bool 
event::is_painted( )
{
    return m_painted;
}
    
void 
event::mark( )
{
    m_marked = true;
}

void 
event::unmark( )
{
    m_marked = false;
}

bool 
event::is_marked( )
{
    return m_marked;
}
