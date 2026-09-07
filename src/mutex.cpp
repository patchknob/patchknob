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

PatchKnobMutex::PatchKnobMutex( )
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m_mutex_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

PatchKnobMutex::~PatchKnobMutex()
{
    pthread_mutex_destroy(&m_mutex_lock);
}

// RECURSIVE (see the constructor).  That is deliberate -- the sequence methods
// nest -- but it also means an unbalanced lock() is silent for the thread that
// leaked it and fatal for every other thread.  sequence::play() did exactly that
// and froze the GUI; if a hang like that ever recurs, temporarily have lock()
// trylock, and on failure print the owning thread id recorded at lock time.
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
    pthread_cond_init(&m_cond, nullptr);
}

condition_var::~condition_var()
{
    pthread_cond_destroy(&m_cond);
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

