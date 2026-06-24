// Standalone unit test for snap_to_scale (SCALE-MASTER / SCALE-FOLLOW).
// The tables and the function below are copied verbatim from src/globals.h so
// the test exercises exactly the shipped algorithm without dragging in gtkmm.
//
// build: g++ -std=c++11 -O2 -o test_snap test_snap.cpp && ./test_snap

#include <cstdio>

enum c_music_scales { c_scale_off, c_scale_major, c_scale_minor, c_scale_size };

const bool c_scales_policy[c_scale_size][12] = {
    { true,true,true,true,true,true,true,true,true,true,true,true},
    { true,false,true,false,true,true,false,true,false,true,false,true},
    { true,false,true,true,false,true,false,true,true,false,true,false},
};

inline int
snap_to_scale( int note, int master_key, int master_scale )
{
    if ( master_scale <= c_scale_off || master_scale >= c_scale_size )
        return note;
    if ( note < 0 || note > 127 )
        return note;
    int pc = ( ( note % 12 ) - master_key + 12 ) % 12;
    if ( c_scales_policy[master_scale][pc] )
        return note;
    for ( int d = 1; d <= 6; d++ ){
        int dn_pc = ( pc - d + 12 ) % 12;
        if ( c_scales_policy[master_scale][dn_pc] ){
            int snapped = note - d;
            if ( snapped < 0 ) snapped += 12;
            return snapped;
        }
        int up_pc = ( pc + d ) % 12;
        if ( c_scales_policy[master_scale][up_pc] ){
            int snapped = note + d;
            if ( snapped > 127 ) snapped -= 12;
            return snapped;
        }
    }
    return note;
}

static int failures = 0;
static void check(const char* name, int got, int want){
    bool ok = (got == want);
    if(!ok) failures++;
    printf("[%s] %-46s got=%3d want=%3d\n", ok?"PASS":"FAIL", name, got, want);
}

// MIDI: C4=60. 60=C 61=C# 62=D 63=D# 64=E 65=F 66=F# 67=G 68=G# 69=A 70=A# 71=B
int main(){
    // ---- C major (key=0): white keys; every black key snaps down 1 ----
    check("Cmaj: C  passthrough",      snap_to_scale(60,0,c_scale_major), 60);
    check("Cmaj: C# -> C",             snap_to_scale(61,0,c_scale_major), 60);
    check("Cmaj: D  passthrough",      snap_to_scale(62,0,c_scale_major), 62);
    check("Cmaj: D# -> D",             snap_to_scale(63,0,c_scale_major), 62);
    check("Cmaj: E  passthrough",      snap_to_scale(64,0,c_scale_major), 64);
    check("Cmaj: F# -> F",             snap_to_scale(66,0,c_scale_major), 65);
    check("Cmaj: G# -> G",             snap_to_scale(68,0,c_scale_major), 67);
    check("Cmaj: A# -> A",             snap_to_scale(70,0,c_scale_major), 69);
    check("Cmaj: B  passthrough",      snap_to_scale(71,0,c_scale_major), 71);

    // ---- C natural minor: C D Eb F G Ab Bb (pc 0,2,3,5,7,8,10) ----
    check("Cmin: Eb passthrough",      snap_to_scale(63,0,c_scale_minor), 63);
    check("Cmin: E  -> Eb",            snap_to_scale(64,0,c_scale_minor), 63);
    check("Cmin: A  -> Ab",            snap_to_scale(69,0,c_scale_minor), 68);

    // ---- D major (key=2): D E F# G A B C# ----
    check("Dmaj: F# passthrough",      snap_to_scale(66,2,c_scale_major), 66);
    check("Dmaj: F  -> E",             snap_to_scale(65,2,c_scale_major), 64);
    check("Dmaj: D  passthrough",      snap_to_scale(62,2,c_scale_major), 62);
    check("Dmaj: C  -> B (tie, snap down)", snap_to_scale(60,2,c_scale_major), 59);

    // ---- scale off => chromatic, no change ----
    check("off: C# unchanged",         snap_to_scale(61,0,c_scale_off), 61);

    // ---- range/clamp ----
    check("low edge: note 0 Cmaj",     snap_to_scale(0,0,c_scale_major), 0);
    check("hi edge: note 127 Cmaj",    snap_to_scale(127,0,c_scale_major), 127);

    // ---- every output is in-scale, all roots ----
    int bad=0;
    for(int key=0; key<12; key++)
        for(int scale=c_scale_major; scale<c_scale_size; scale++)
            for(int n=0;n<128;n++){
                int s = snap_to_scale(n,key,scale);
                if(s<0||s>127){ bad++; continue; }
                if(!c_scales_policy[scale][((s%12)-key+12)%12]) bad++;
            }
    check("ALL keys/scales/notes snap in-scale & in-range", bad, 0);

    printf("\n%s (%d failures)\n", failures? "FAILED":"ALL PASS", failures);
    return failures?1:0;
}
