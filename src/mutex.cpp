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

#include "mutex.h"
#include "config.h"

const pthread_mutex_t PatchKnobMutex::recmutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
const pthread_cond_t condition_var::cond  = PTHREAD_COND_INITIALIZER;

PatchKnobMutex::PatchKnobMutex( )
{
    m_mutex_lock = recmutex;
}

void 
PatchKnobMutex::lock( )
{
    pthread_mutex_lock( &m_mutex_lock );
}


void 
PatchKnobMutex::unlock( )
{      
    pthread_mutex_unlock( &m_mutex_lock );
}

condition_var::condition_var( )
{
    m_cond = cond;
}


void
condition_var::signal( )
{
    pthread_cond_signal( &m_cond );
}

void
condition_var::wait( )
{
    pthread_cond_wait( &m_cond, &m_mutex_lock );
}


