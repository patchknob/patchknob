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

#ifndef PATCHKNOB_USERFILE
#define PATCHKNOB_USERFILE

#include "perform.h"
#include "configfile.h"
#include <fstream>
#include <string>
#include <list>

class userfile  : public configfile
{

 public:

    userfile( string a_name );
    ~userfile( );
    
    bool parse( perform *a_perf );
    bool write( perform *a_perf );

};


#endif 
