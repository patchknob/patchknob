//----------------------------------------------------------------------------
//  sdlui/views/pd_editor/pd_editor_view.cpp -- SDL2 Pure Data patch editor.
//
//  Pure UI over a .pd file: parse -> edit visually -> save() -> on_changed().
//  No libpd / audio_app include; the host wires on_changed to a patch reload.
//----------------------------------------------------------------------------
#include "pd_editor_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

using ui::Color;
using ui::theme;

// ============================================================================
//  file / string helpers (parser is tolerant: it ignores what it can't read)
// ============================================================================
namespace {

// Split a .pd blob into records on ';'.  A backslash-escaped "\;" (a literal
// semicolon inside a message) does NOT end a record.
std::vector<std::string> split_records( const std::string& s )
{
    std::vector<std::string> out;
    std::string cur;
    for ( std::size_t i = 0; i < s.size(); ++i )
    {
        char c = s[i];
        if ( c == ';' )
        {
            if ( !cur.empty() && cur.back() == '\\' ) cur.push_back( ';' );  // escaped
            else { out.push_back( cur ); cur.clear(); }
        }
        else cur.push_back( c );
    }
    if ( cur.find_first_not_of( " \t\r\n\f\v" ) != std::string::npos )
        out.push_back( cur );                              // trailing junk (no final ;)
    return out;
}

// Whitespace tokenizer (records wrap across newlines in real .pd files).
std::vector<std::string> tokenize( const std::string& r )
{
    std::vector<std::string> t;
    std::string cur;
    for ( std::size_t i = 0; i < r.size(); ++i )
    {
        char c = r[i];
        if ( c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' )
        { if ( !cur.empty() ) { t.push_back( cur ); cur.clear(); } }
        else cur.push_back( c );
    }
    if ( !cur.empty() ) t.push_back( cur );
    return t;
}

// Join tokens[n..] back into one space-separated string (verbatim body text).
std::string join_from( const std::vector<std::string>& t, std::size_t n )
{
    std::string s;
    for ( std::size_t i = n; i < t.size(); ++i ) { if ( i > n ) s += ' '; s += t[i]; }
    return s;
}

int safe_atoi( const std::string& s )
{
    return (int) std::strtol( s.c_str(), 0, 10 );
}

std::string to_lower( std::string s )
{
    for ( std::size_t i = 0; i < s.size(); ++i )
        s[i] = (char) std::tolower( (unsigned char) s[i] );
    return s;
}

bool ci_contains( const std::string& hay, const std::string& needle )
{
    if ( needle.empty() ) return true;
    return to_lower( hay ).find( to_lower( needle ) ) != std::string::npos;
}

std::string first_word( const std::string& text )
{
    std::string w;
    for ( std::size_t i = 0; i < text.size(); ++i )
    {
        char c = text[i];
        if ( c == ' ' || c == '\t' || c == '\n' || c == '\r' ) break;
        w.push_back( c );
    }
    return w;
}

bool in_rect( const SDL_Rect& q, int x, int y )
{
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}

// FULL Pd-vanilla object palette for the picker -- generated from the vendored
// pure-data class registry (every placeable object incl. FFT: fft~/rfft~/rifft~/
// ifft~/framp~, analysis: sigmund~/bonk~/fiddle~/env~, the array/list/text object
// families, MIDI, and every IEMGUI).  Searchable, so a long list is fine.
static const char* const OBJECTS[] = {
    "!=","%","&","&&","*","*~","+","+~",
    "-","-~","/","/~","<","<<","<=","==",
    ">",">=",">>","abs","abs~","adc~","append","array",
    "array define","array get","array max","array min","array quantile","array random","array set","array size",
    "array sum","atan","atan2","bag","bang","bang~","bendin","bendout",
    "biquad~","block~","bng","bob~","bonk~","bp~","catch~","change",
    "choice","clip","clip~","clone","cnv","complex-mod~","cos","cos~",
    "cpole~","cputime","ctlin","ctlout","czero_rev~","czero~","dac~","dbtopow",
    "dbtopow~","dbtorms","dbtorms~","declare","delay","delread4~","delread~","delwrite~",
    "div","drawpolygon","drawtext","env~","exp","expr","expr~","exp~",
    "fexpr~","fft~","fiddle~","file define","float","framp~","ftom","ftom~",
    "fudiformat","fudiparse","get","getsize","hilbert~","hip~","hradio","hsl",
    "ifft~","inlet","int","key","keyname","keyup","line","line~",
    "list append","list fromsymbol","list inlet","list length","list prepend","list split","list store","list tosymbol",
    "list trim","loadbang","log","log~","loop~","lop~","lrshift~","makefilename",
    "makenote","max","max~","metro","midiin","midiout","midirealtimein","min",
    "min~","mod","moses","mtof","mtof~","namecanvas","nbx","netreceive",
    "netsend","noise~","notein","noteout","openpanel","oscformat","oscparse","osc~",
    "outlet","pack","pdcontrol","pd~","pgmin","pgmout","phasor~","pipe",
    "pique","plot","pointer","poly","polytouchin","polytouchout","pow","powtodb",
    "powtodb~","pow~","print","print~","qlist","random","readsf~","realtime",
    "receive","receive~","rev1~","rev2~","rev3~","rfft~","rifft~","rmstodb",
    "rmstodb~","route","rpole~","rsqrt~","rzero_rev~","rzero~","samphold~","samplerate~",
    "savepanel","savestate","scalar define","select","send","send~","set","setsize",
    "sigmund~","sig~","sin","slop~","snapshot~","soundfiler","spigot","sqrt",
    "sqrt~","stdout","stripnote","struct","swap","symbol","sysexin","tabosc4~",
    "tabplay~","tabread","tabread4","tabread4~","tabread~","tabreceive~","tabsend~","tabwrite",
    "tabwrite~","tan","template","text","text define","text delete","text fromlist","text get",
    "text insert","text search","text sequence","text set","text size","text tolist","textfile","tgl",
    "threshold~","throw~","timer","touchin","touchout","trigger","unpack","until",
    "value","vcf~","vline~","vsnapshot~","vu","wrap","wrap~","writesf~",
    "|","||",
    // IEMGUIs + common aliases not surfaced by the raw class scan:
    "vsl","vradio","r","s","sel","t","f","b","i","del","v","table","vd~",
};
const int N_OBJECTS = int( sizeof(OBJECTS) / sizeof(OBJECTS[0]) );

} // anonymous namespace

namespace pdui {

// ============================================================================
//  Inlet / outlet table (the .pd file does not store counts)
// ============================================================================
void PdEditorView::io_for( const std::string& text, int& nin, int& nout )
{
    const std::string w = first_word( text );
    std::vector<std::string> tk = tokenize( text );
    const int nargs = (int) tk.size() - 1;         // creation args after the object name

    // --- objects whose inlet/outlet count depends on their ARGUMENTS ---------
    // MIDI in: a channel arg drops the channel outlet (0 inlets throughout).
    if ( w == "notein" || w == "polytouchin" )  { nin = 0; nout = nargs >= 1 ? 2 : 3; return; }
    if ( w == "ctlin" )    { nin = 0; nout = nargs >= 2 ? 1 : ( nargs == 1 ? 2 : 3 ); return; }
    if ( w == "bendin" || w == "pgmin" || w == "touchin" ) { nin = 0; nout = nargs >= 1 ? 1 : 2; return; }
    // MIDI out: mirror -- a channel arg drops an inlet (0 outlets throughout).
    if ( w == "noteout" || w == "polytouchout" ) { nout = 0; nin = nargs >= 1 ? 2 : 3; return; }
    if ( w == "ctlout" )   { nout = 0; nin = nargs >= 2 ? 1 : ( nargs == 1 ? 2 : 3 ); return; }
    if ( w == "bendout" || w == "pgmout" || w == "touchout" ) { nout = 0; nin = nargs >= 1 ? 1 : 2; return; }
    // glue whose fan-in / fan-out follows the number of args.
    if ( w == "pack" )     { nin  = nargs >= 1 ? nargs : 2; nout = 1; return; }
    if ( w == "unpack" )   { nout = nargs >= 1 ? nargs : 2; nin  = 1; return; }
    if ( w == "trigger" || w == "t" ) { nin = 1; nout = nargs >= 1 ? nargs : 1; return; }
    if ( w == "select" || w == "sel" || w == "route" )
        { nin = ( nargs <= 1 ) ? 2 : 1; nout = ( nargs >= 1 ? nargs : 1 ) + 1; return; }

    struct Entry { const char* key; int nin; int nout; };
    static const Entry TAB[] = {
        { "osc~",2,1 }, { "phasor~",2,1 }, { "*~",2,1 }, { "+~",2,1 }, { "-~",2,1 },
        { "/~",2,1 }, { "dac~",2,0 }, { "adc~",0,2 }, { "line~",2,1 }, { "vline~",3,1 },
        { "sig~",1,1 }, { "noise~",0,1 }, { "lop~",2,1 }, { "hip~",2,1 }, { "bp~",3,1 },
        { "clip~",3,1 }, { "metro",2,1 }, { "delay",2,1 }, { "del",2,1 },
        { "tgl",1,1 }, { "bng",1,1 },
        { "mtof",1,1 }, { "ftom",1,1 },
        { "+",2,1 }, { "-",2,1 }, { "*",2,1 }, { "/",2,1 }, { "mod",2,1 },
        { "moses",2,2 },
        { "float",2,1 }, { "f",2,1 }, { "int",2,1 }, { "i",2,1 },
        { "print",1,0 }, { "r",0,1 }, { "receive",0,1 }, { "s",1,0 }, { "send",1,0 },
    };
    for ( std::size_t i = 0; i < sizeof(TAB)/sizeof(TAB[0]); ++i )
        if ( w == TAB[i].key ) { nin = TAB[i].nin; nout = TAB[i].nout; return; }

    // adc~ / dac~ with explicit channel args: one inlet/outlet per channel.
    if ( w == "adc~" || w == "dac~" )
    {
        int chans = nargs;                         // args after the object name
        if ( chans < 1 ) chans = 2;                // bare adc~/dac~ == 2 channels
        if ( w == "adc~" ) { nin = 0; nout = chans; }
        else               { nin = chans; nout = 0; }
        return;
    }

    nin = 1; nout = 1;                         // default (tgl/bng/hsl/vsl/nbx/... = 1/1)
}

// ============================================================================
//  Pd GUI atoms (tgl / bng / hsl / vsl / nbx / hradio / vradio / knob) -- parsed
//  from the object text so the editor can draw the REAL widget (a slider, a
//  toggle, ...) instead of a plain text box, sized to the .pd's own dimensions.
// ============================================================================
namespace {
struct GuiInfo {
    enum Type { None, Tgl, Bng, Hsl, Vsl, Nbx, Hradio, Vradio, Knob } type = None;
    int    w = 0, h = 0;          // pixel size from the .pd record
    double min = 0.0, max = 1.0;  // slider / nbx range
    double val = 0.0;             // best-effort current value
    int    cells = 1;             // radio: number of cells
    int    valTok = -1;           // token index (after the name) holding the value
    int    recvTok = -1;          // token index of the RECEIVE symbol (-1 = n/a)
    bool   isSlider() const { return type == Hsl || type == Vsl; }
};
// Parse an "obj" record's text into a GuiInfo.  Reliable tokens (type, w, h,
// min, max) come first in every IEMGUI record; the value index differs per type.
GuiInfo gui_info( const std::string& text )
{
    GuiInfo g;
    std::vector<std::string> tk = tokenize( text );
    if ( tk.empty() ) return g;
    const std::string& k = tk[0];
    auto num = [&]( size_t i, double d )->double {
        return ( i < tk.size() ) ? std::atof( tk[i].c_str() ) : d; };
    // valTok is the RAW token index (tk[0] == the type name) of the value field in
    // the Pd-vanilla IEMGUI save record; read + write both use it, so the editor
    // stays self-consistent regardless of Pd-version quirks.
    // recvTok = index of the RECEIVE symbol in the IEMGUI save record (sending a
    // value/bang there makes the atom update AND output -> drives the live patch).
    if      ( k == "tgl" )    { g.type = GuiInfo::Tgl;  g.w = g.h = (int)num(1,15); g.valTok = 13; g.recvTok = 4; g.val = num(g.valTok,0); }
    else if ( k == "bng" )    { g.type = GuiInfo::Bng;  g.w = g.h = (int)num(1,15); g.recvTok = 6; }
    else if ( k == "hsl" )    { g.type = GuiInfo::Hsl;  g.w=(int)num(1,128); g.h=(int)num(2,15);
                                g.min=num(3,0); g.max=num(4,127); g.valTok=17; g.recvTok = 8; }
    else if ( k == "vsl" )    { g.type = GuiInfo::Vsl;  g.w=(int)num(1,15);  g.h=(int)num(2,128);
                                g.min=num(3,0); g.max=num(4,127); g.valTok=17; g.recvTok = 8; }
    else if ( k == "nbx" )    { g.type = GuiInfo::Nbx;  g.w=(int)num(1,5)*8; g.h=(int)num(2,14);
                                g.min=num(3,-1e37); g.max=num(4,1e37); g.valTok=17; g.recvTok = 8; g.val=num(g.valTok,0); }
    else if ( k == "hradio" ) { g.type = GuiInfo::Hradio; g.cells=(int)num(4,8); g.w=(int)num(1,15)*g.cells; g.h=(int)num(1,15); g.valTok=15; g.recvTok = 6; g.val=num(g.valTok,0); }
    else if ( k == "vradio" ) { g.type = GuiInfo::Vradio; g.cells=(int)num(4,8); g.w=(int)num(1,15); g.h=(int)num(1,15)*g.cells; g.valTok=15; g.recvTok = 6; g.val=num(g.valTok,0); }
    else if ( k == "knob" )   { g.type = GuiInfo::Knob; g.w=g.h=(int)num(1,40); g.min=num(3,0); g.max=num(4,127); g.valTok=17; g.val=num(g.valTok,0); }
    if ( g.w < 8 )  g.w = 8;
    if ( g.h < 8 )  g.h = 8;
    // slider value came from a position field (0..(dim-1)*100) -> normalize 0..1
    if ( g.isSlider() ) {
        const int dim = ( g.type == GuiInfo::Hsl ) ? g.w : g.h;
        const double span = std::max( 1, dim - 1 ) * 100.0;
        double pos = ( g.valTok >= 0 && g.valTok < (int)tk.size() ) ? std::atof( tk[g.valTok].c_str() ) : 0.0;
        g.val = ( pos / span );                       // 0..1 along the track
        if ( g.val < 0 ) g.val = 0;
        if ( g.val > 1 ) g.val = 1;
    }
    return g;
}

// Expand a BARE GUI-atom name ("hsl", "tgl", ...) into a full Pd-vanilla IEMGUI
// record so the widget has valid size/range/value fields.  Without this, a
// picker-created bare atom has no width/height and collapses when you interact.
std::string default_gui_record( const std::string& text )
{
    const std::string w = first_word( text );
    if ( text != w ) return text;                   // already has args -> leave it
    if ( w == "hsl" )    return "hsl 128 15 0 127 0 0 empty empty empty -2 -8 0 10 #fcfcfc #000000 #000000 0 1";
    if ( w == "vsl" )    return "vsl 15 128 0 127 0 0 empty empty empty 0 -9 0 10 #fcfcfc #000000 #000000 0 1";
    if ( w == "tgl" )    return "tgl 15 0 empty empty empty 17 7 0 10 #fcfcfc #000000 #000000 0";
    if ( w == "bng" )    return "bng 15 250 50 0 empty empty empty 17 7 0 10 #fcfcfc #000000 #000000";
    if ( w == "nbx" )    return "nbx 5 14 -1e+37 1e+37 0 0 empty empty empty 0 -8 0 10 #fcfcfc #000000 #000000 0 256";
    if ( w == "hradio" ) return "hradio 15 1 0 8 empty empty empty 0 -8 0 10 #fcfcfc #000000 #000000 0";
    if ( w == "vradio" ) return "vradio 15 1 0 8 empty empty empty 0 -8 0 10 #fcfcfc #000000 #000000 0";
    return text;
}

// Replace RAW token `tokIdx` (tk[0] == the type name) of a GUI record's text with
// `value` (%g), padding with zeros if the record is short.
void set_gui_token( std::string& text, int tokIdx, double value )
{
    if ( tokIdx < 1 ) return;
    std::vector<std::string> tk = tokenize( text );
    const int idx = tokIdx;
    while ( (int)tk.size() <= idx ) tk.push_back( "0" );
    char buf[32]; std::snprintf( buf, sizeof(buf), "%g", value );
    tk[(size_t)idx] = buf;
    std::string out;
    for ( size_t i = 0; i < tk.size(); ++i ) { if ( i ) out += ' '; out += tk[i]; }
    text = out;
}

// Set RAW token `tokIdx` to a SYMBOL (string) -- used to stamp a GUI atom's receive
// symbol so the editor can address it live in the running patch.
void set_gui_sym( std::string& text, int tokIdx, const std::string& sym )
{
    if ( tokIdx < 1 || sym.empty() ) return;
    std::vector<std::string> tk = tokenize( text );
    while ( (int)tk.size() <= tokIdx ) tk.push_back( "empty" );
    tk[(size_t)tokIdx] = sym;
    std::string out;
    for ( size_t i = 0; i < tk.size(); ++i ) { if ( i ) out += ' '; out += tk[i]; }
    text = out;
}

// bpatcher: read the referenced abstraction (.pd) to learn its window size (the
// graph-on-parent pixel area) and inlet/outlet count so we can draw it as a real
// embedded window with the right connectors.
void scan_bpatcher( const std::string& dir, const std::string& objText,
                    int& nin, int& nout, int& bw, int& bh )
{
    nin = 0; nout = 0; bw = 0; bh = 0;
    std::vector<std::string> tk = tokenize( objText );   // "bpatcher [-flags] name [args]"
    std::string name;
    for ( size_t i = 1; i < tk.size(); ++i ) {
        if ( tk[i].empty() || tk[i][0] == '-' ) continue;   // skip flags (-hidetext ...)
        name = tk[i]; break;
    }
    if ( name.empty() ) return;
    if ( name.size() < 3 || name.substr( name.size() - 3 ) != ".pd" ) name += ".pd";
    std::ifstream f( ( dir + "/" + name ).c_str() );
    if ( !f ) return;
    std::string line;
    while ( std::getline( f, line ) ) {
        std::vector<std::string> t = tokenize( line );
        if ( t.size() >= 5 && t[0] == "#X" && t[1] == "obj" ) {
            const std::string& w = t[4];
            if      ( w == "inlet"  || w == "inlet~"  ) ++nin;
            else if ( w == "outlet" || w == "outlet~" ) ++nout;
        } else if ( t.size() >= 8 && t[0] == "#X" && t[1] == "coords" ) {
            bw = safe_atoi( t[6] );   // graph-on-parent display width
            bh = safe_atoi( t[7] );   // ... and height
        }
    }
    if ( bw <= 0 ) bw = 120;          // sensible default window if no GOP area
    if ( bh <= 0 ) bh = 60;
}

// Draw a circle outline as short line segments (no circle primitive in gui.h).
void draw_ring( SDL_Renderer* r, int cx, int cy, int rad, ui::Color col )
{
    if ( rad < 1 ) return;
    ui::set_color( r, col );
    const int seg = std::max( 10, rad * 2 );
    int px = 0, py = 0; bool have = false;
    for ( int s = 0; s <= seg; ++s ) {
        const double a = 2.0 * 3.14159265 * s / seg;
        const int x = cx + (int)std::lround( std::cos(a) * rad );
        const int y = cy + (int)std::lround( std::sin(a) * rad );
        if ( have ) SDL_RenderDrawLine( r, px, py, x, y );
        px = x; py = y; have = true;
    }
}

// --- data-structure draw parsing --------------------------------------------
// A template draw arg: a numeric constant, or a symbol = a scalar field name.
PdFieldDesc parse_fd( const std::string& tok )
{
    PdFieldDesc fd;
    char* e = nullptr;
    const double v = std::strtod( tok.c_str(), &e );
    if ( !tok.empty() && e && *e == '\0' ) { fd.isConst = true;  fd.val = (float) v; }
    else                                    { fd.isConst = false; fd.field = tok; }
    return fd;
}
// drawpolygon / drawcurve / filledpolygon / filledcurve:
//   [ -flags ] [ fillcolor (if filled) ] outlinecolor width  x0 y0 x1 y1 ...
PdDraw parse_curve_draw( const std::vector<std::string>& tk, const std::string& cmd )
{
    PdDraw d;
    d.kind   = ( cmd.find( "curve" ) != std::string::npos ) ? PdDraw::Curve : PdDraw::Polygon;
    d.closed = ( cmd.compare( 0, 6, "filled" ) == 0 );
    size_t i = 5;                                     // tk[4] == cmd; args start at 5
    while ( i < tk.size() && !tk[i].empty() && tk[i][0] == '-' ) {  // skip flags
        if ( tk[i] == "-v" ) ++i;                     // -v consumes a following arg
        ++i;
    }
    if ( d.closed && i < tk.size() ) ++i;             // fillcolor (filled* only)
    if ( i < tk.size() ) ++i;                         // outlinecolor
    if ( i < tk.size() ) ++i;                         // width
    for ( ; i < tk.size(); ++i ) {
        if ( tk[i] == ";" || tk[i] == "\\;" ) break;
        d.coords.push_back( parse_fd( tk[i] ) );
    }
    return d;
}
// drawnumber / drawtext / drawsymbol:  [ -flags ] field xoff yoff [color fontsize label]
PdDraw parse_number_draw( const std::vector<std::string>& tk )
{
    PdDraw d; d.kind = PdDraw::Number;
    size_t i = 5;
    while ( i < tk.size() && !tk[i].empty() && tk[i][0] == '-' ) ++i;   // skip flags
    if ( i < tk.size() ) d.field = parse_fd( tk[i++] );
    if ( i < tk.size() ) d.x     = parse_fd( tk[i++] );
    if ( i < tk.size() ) d.y     = parse_fd( tk[i++] );
    return d;
}
} // anonymous namespace

// ============================================================================
//  File I/O
// ============================================================================
void PdEditorView::set_patch_path( const std::string& path )
{
    m_path = path;
    reload_from_disk();
}

void PdEditorView::reload_from_disk()
{
    reset_interaction();
    m_sel = -1;
    parse();
}

void PdEditorView::parse()
{
    // Import path: read the file, then parse its text.  (In normal use the patch
    // arrives from the node via set_patch_text -- there is no source .pd on disk.)
    std::string content;
    if ( !m_path.empty() )
    {
        std::ifstream in( m_path.c_str(), std::ios::binary );
        if ( in ) content.assign( (std::istreambuf_iterator<char>( in )),
                                   std::istreambuf_iterator<char>() );
    }
    parse_text( content );
}

void PdEditorView::parse_text( const std::string& content )
{
    release_inline_edit();                 // text_target may point into objs_
    objs_.clear();
    conns_.clear();
    m_canvas_w = 600;
    m_canvas_h = 400;
    if ( content.empty() ) return;

    // Parse faithfully: track nested-canvas DEPTH so a subpatch's internal records
    // are captured VERBATIM as one passthrough gobj (not leaked to the top level),
    // and preserve every non-gobj record so nothing is lost on save.
    auto trim = []( const std::string& s ) -> std::string {
        const size_t a = s.find_first_not_of( " \t\r\n\f\v" );
        if ( a == std::string::npos ) return std::string();
        const size_t b = s.find_last_not_of( " \t\r\n\f\v" );
        return s.substr( a, b - a + 1 );
    };
    // A non-gobj record (takes no #X connect index): keep it in place -- after the
    // last gobj, or in the preamble if none yet.
    auto stash_nongobj = [&]( const std::string& rec ) {
        const std::string line = trim( rec ) + ";\n";
        if ( objs_.empty() ) m_preamble += line; else objs_.back().post += line;
    };
    // An index-bearing gobj we don't edit (subpatch/graph/array/scalar/atom): store
    // its verbatim record(s) so it round-trips AND holds its slot in the index space.
    auto add_raw_gobj = [&]( const std::string& raw, int x, int y,
                             const std::string& label, int nin, int nout ) {
        PdObj o; o.kind = 'x'; o.raw = raw; o.x = x; o.y = y; o.text = label;
        o.nin = nin; o.nout = nout; objs_.push_back( o );
    };

    bool seen_header = false;
    int  depth = 0;
    std::string block;                 // verbatim capture while inside a nested canvas
    std::vector<std::string> recs = split_records( content );

    for ( std::size_t ri = 0; ri < recs.size(); ++ri )
    {
        const std::string& rec = recs[ri];
        std::vector<std::string> tk = tokenize( rec );
        if ( tk.empty() ) continue;

        // --- inside a nested canvas: capture verbatim until its matching restore ---
        if ( depth > 0 )
        {
            block += trim( rec ) + ";\n";
            if ( tk[0] == "#N" && tk.size() > 1 && tk[1] == "canvas" ) { ++depth; continue; }
            if ( tk[0] == "#X" && tk.size() > 1 && tk[1] == "restore" && --depth == 0 )
            {
                const int x = ( tk.size() > 2 ) ? safe_atoi( tk[2] ) : 0;
                const int y = ( tk.size() > 3 ) ? safe_atoi( tk[3] ) : 0;
                int nin = 0, nout = 0;         // ports = # of inlet(~)/outlet(~) inside
                std::vector<float> arr; int gw = 0, gh = 0; bool hasArray = false;
                std::istringstream bs( block ); std::string ln;
                while ( std::getline( bs, ln ) ) {
                    std::vector<std::string> t2 = tokenize( ln );
                    if ( t2.size() >= 5 && t2[0] == "#X" && t2[1] == "obj" ) {
                        if ( t2[4] == "inlet"  || t2[4] == "inlet~"  ) ++nin;
                        if ( t2[4] == "outlet" || t2[4] == "outlet~" ) ++nout;
                    }
                    else if ( t2.size() >= 4 && t2[0] == "#X" && t2[1] == "array" )
                        hasArray = true;                         // #X array name size type ...
                    else if ( t2.size() >= 3 && t2[0] == "#A" )  // #A onset v0 v1 ... (data)
                        for ( size_t k = 2; k < t2.size(); ++k )
                            arr.push_back( (float) std::atof( t2[k].c_str() ) );
                    else if ( t2.size() >= 7 && t2[0] == "#X" && t2[1] == "coords" )
                        { gw = safe_atoi( t2[5] ); gh = safe_atoi( t2[6] ); }  // display px size
                }
                add_raw_gobj( block, x, y, join_from( tk, 4 ), nin, nout );
                if ( hasArray ) {                                // a graph carrying an array
                    PdObj& gobj = objs_.back();
                    gobj.arr  = std::move( arr );
                    gobj.bp_w = gw > 0 ? gw : 200;               // size the box like the graph
                    gobj.bp_h = gh > 0 ? gh : 130;
                }
                block.clear();
            }
            continue;
        }

        if ( tk.size() < 2 ) { stash_nongobj( rec ); continue; }

        if ( tk[0] == "#N" && tk[1] == "canvas" )
        {
            if ( !seen_header )                // first canvas = our patch header
            {
                m_header = trim( rec ) + ";\n";
                if ( tk.size() > 4 ) m_canvas_w = safe_atoi( tk[4] );
                if ( tk.size() > 5 ) m_canvas_h = safe_atoi( tk[5] );
                seen_header = true;
            }
            else { depth = 1; block = trim( rec ) + ";\n"; }   // a nested canvas begins
            continue;
        }

        if ( tk[0] != "#X" ) { stash_nongobj( rec ); continue; }
        const std::string& r = tk[1];

        if ( r == "obj" || r == "msg" || r == "floatatom" || r == "text" )
        {
            PdObj o;
            o.kind = ( r == "obj" ) ? 'o' : ( r == "msg" ) ? 'm'
                   : ( r == "floatatom" ) ? 'f' : 't';
            o.x   = ( tk.size() > 2 ) ? safe_atoi( tk[2] ) : 0;
            o.y   = ( tk.size() > 3 ) ? safe_atoi( tk[3] ) : 0;
            o.text = join_from( tk, 4 );

            switch ( o.kind )
            {
                case 'o':
                    io_for( o.text, o.nin, o.nout );
                    if ( first_word( o.text ) == "bpatcher" ) {
                        const size_t sp = m_path.find_last_of( "/\\" );
                        const std::string dir = ( sp == std::string::npos ) ? std::string( "." )
                                                                            : m_path.substr( 0, sp );
                        scan_bpatcher( dir, o.text, o.nin, o.nout, o.bp_w, o.bp_h );
                    }
                    break;
                case 'm': o.nin = 1; o.nout = 1; break;      // message box
                case 'f': o.nin = 1; o.nout = 1; break;      // number box
                default:  o.nin = 0; o.nout = 0; break;      // comment
            }
            objs_.push_back( o );
        }
        else if ( r == "connect" )
        {
            if ( tk.size() >= 6 )
            {
                PdConn c;
                c.from   = safe_atoi( tk[2] );
                c.outlet = safe_atoi( tk[3] );
                c.to     = safe_atoi( tk[4] );
                c.inlet  = safe_atoi( tk[5] );
                conns_.push_back( c );
            }
        }
        else if ( r == "symbolatom" || r == "listbox" )     // atom gobjs (1 in / 1 out)
            add_raw_gobj( trim( rec ) + ";\n",
                          tk.size() > 2 ? safe_atoi( tk[2] ) : 0,
                          tk.size() > 3 ? safe_atoi( tk[3] ) : 0, r, 1, 1 );
        else if ( r == "scalar" )                            // data-structure instance
        {
            PdObj o; o.kind = 'x'; o.raw = trim( rec ) + ";\n"; o.text = "scalar";
            o.tmpl = ( tk.size() > 2 ) ? tk[2] : std::string();
            o.nin = o.nout = 0;
            for ( size_t k = 3; k < tk.size(); ++k ) {       // float field values, in order
                if ( tk[k] == ";" || tk[k] == "\\;" ) break;
                char* e = nullptr; double v = std::strtod( tk[k].c_str(), &e );
                o.arr.push_back( ( e && *e == '\0' ) ? (float) v : 0.f );
            }
            objs_.push_back( o );
        }
        else if ( r == "array" )                             // index-bearing, no ports
            add_raw_gobj( trim( rec ) + ";\n", 0, 0, join_from( tk, 1 ), 0, 0 );
        else
            stash_nongobj( rec );              // #X coords / #X f / declare / struct / ...
    }

    sanitize_conns();

    // Bump the auto receive-symbol counter past any "pkui<N>" already in the patch,
    // so newly created GUI atoms never collide with ones saved in a prior session.
    for ( const PdObj& o : objs_ )
    {
        const std::string s = gui_recv_symbol( o );
        if ( s.size() > 4 && s.compare( 0, 4, "pkui" ) == 0 )
        {
            char* end = nullptr;
            unsigned long n = std::strtoul( s.c_str() + 4, &end, 10 );
            if ( end && *end == '\0' && n + 1 > m_recv_serial ) m_recv_serial = (unsigned)( n + 1 );
        }
    }

    // Data structures: (re)build the template registry, then position each scalar
    // from its template's x / y fields so draw_scalar can render it.
    build_templates();
    for ( PdObj& o : objs_ )
    {
        if ( o.kind != 'x' || o.tmpl.empty() ) continue;
        const PdTemplate* t = find_template( o.tmpl );
        if ( !t ) continue;
        for ( size_t k = 0; k < t->fields.size() && k < o.arr.size(); ++k ) {
            if ( t->fields[k] == "x" ) o.x = (int) o.arr[k];
            if ( t->fields[k] == "y" ) o.y = (int) o.arr[k];
        }
    }
}

void PdEditorView::sanitize_conns()
{
    const int n = (int) objs_.size();
    for ( std::size_t i = conns_.size(); i-- > 0; )
    {
        const PdConn& c = conns_[i];
        if ( c.from < 0 || c.from >= n || c.to < 0 || c.to >= n || c.from == c.to )
            conns_.erase( conns_.begin() + i );
        // port indices are clamped when drawn, so mismatches don't drop the wire
    }
}

// Serialise the model to Pd-vanilla .pd text (what the host stores on the node).
std::string PdEditorView::serialize() const
{
    std::ostringstream out;
    if ( !m_header.empty() ) out << m_header;               // preserved canvas header
    else out << "#N canvas 0 0 " << m_canvas_w << ' ' << m_canvas_h << " 12;\n";
    out << m_preamble;                                      // non-gobj records before any gobj
    for ( std::size_t i = 0; i < objs_.size(); ++i )
    {
        const PdObj& o = objs_[i];
        if ( o.kind == 'x' )                                // subpatch/graph/array/atom: verbatim
            out << o.raw;
        else
        {
            const char* rec = ( o.kind == 'm' ) ? "msg"
                            : ( o.kind == 'f' ) ? "floatatom"
                            : ( o.kind == 't' ) ? "text" : "obj";
            out << "#X " << rec << ' ' << o.x << ' ' << o.y;
            if ( !o.text.empty() ) out << ' ' << o.text;
            out << ";\n";
        }
        out << o.post;                                      // non-gobj records that followed it
    }
    for ( std::size_t i = 0; i < conns_.size(); ++i )
    {
        const PdConn& c = conns_[i];
        out << "#X connect " << c.from << ' ' << c.outlet << ' '
            << c.to << ' ' << c.inlet << ";\n";
    }
    return out.str();
}

std::string PdEditorView::patch_text() const { return serialize(); }

void PdEditorView::set_patch_text( const std::string& text )
{
    m_path.clear();
    reset_interaction();
    m_sel = -1; m_sel_wire = -1;
    parse_text( text );
}

// EXPORT the model to a .pd file (external interop only; not the source of truth).
bool PdEditorView::save()
{
    if ( m_path.empty() ) return false;
    std::ofstream out( m_path.c_str(), std::ios::binary | std::ios::trunc );
    if ( !out ) return false;
    out << serialize();
    return out.good();
}

void PdEditorView::commit( ui::App& app )
{
    if ( on_changed ) on_changed();     // host pulls patch_text() and reloads libpd
    app.request_redraw();
}

// End any inline object-text edit whose target lives inside objs_.  Must run
// before objs_ is cleared, erased from, or grown -- a reallocation or a shift
// would otherwise leave App::text_target pointing at freed/moved storage, and
// every keystroke would write through it.
void PdEditorView::release_inline_edit()
{
    if ( !m_edit_ptr ) return;
    const bool was_live = ( m_edit_app && m_edit_app->text_target == m_edit_ptr );
    if ( m_edit_app ) m_edit_app->end_text_if( m_edit_ptr );
    m_edit_ptr = nullptr;
    // Deliberately does NOT commit(): this runs on paths that are about to
    // replace or destroy the object anyway (patch load, delete), and committing
    // there would write the outgoing patch to disk.  The text itself was edited
    // in place, so nothing typed is lost -- only the port recount, which we do
    // here for the object if it is still around.
    const int i = m_edit_obj;
    m_mode = Mode_None;
    m_edit_obj = -1;
    if ( was_live && i >= 0 && i < (int) objs_.size() )
        io_for( objs_[i].text, objs_[i].nin, objs_[i].nout );
}

void PdEditorView::reset_interaction()
{
    release_inline_edit();
    m_move = false; m_move_obj = -1; m_moved = false;
    m_wire = false; m_wire_from = -1; m_wire_to = -1; m_wire_ok = false;
    m_sel_wire = -1;
    m_last_click_obj = -1;
}

// ============================================================================
//  Geometry (Pd coords -> screen)
// ============================================================================
void PdEditorView::obj_screen( const PdObj& o, int& sx, int& sy ) const
{
    sx = rect.x + m_ox + (int) std::lround( o.x * m_zoom );
    sy = rect.y + m_oy + (int) std::lround( o.y * m_zoom );
}

int PdEditorView::box_w( const ui::Font& f, const PdObj& o ) const
{
    if ( o.bp_w > 0 ) return (int) std::lround( o.bp_w * m_zoom );   // bpatcher window
    if ( o.kind == 'o' ) { GuiInfo g = gui_info( o.text );
        if ( g.type != GuiInfo::None ) return (int) std::lround( g.w * m_zoom ); }
    int w = f.text_w( o.text ) + 2 * PADX;
    if ( o.kind == 'm' ) w += FLAG;             // room for the flag notch
    if ( w < MINW ) w = MINW;
    return (int) std::lround( w * m_zoom );     // geometry scales with the zoom
}

int PdEditorView::box_h( const ui::Font& f, const PdObj& o ) const
{
    if ( o.bp_h > 0 ) return (int) std::lround( o.bp_h * m_zoom );   // bpatcher window
    if ( o.kind == 'o' ) { GuiInfo g = gui_info( o.text );
        if ( g.type != GuiInfo::None ) return (int) std::lround( g.h * m_zoom ); }
    return (int) std::lround( ( f.ch() + 2 * PADY ) * m_zoom );
}

bool PdEditorView::outlet_pos( ui::App& app, int idx, int outlet, int& x, int& y ) const
{
    if ( idx < 0 || idx >= (int) objs_.size() ) return false;
    const PdObj& o = objs_[idx];
    const ui::Font& f = app.mono;
    int sx, sy; obj_screen( o, sx, sy );
    int w = box_w( f, o );
    int n = ( o.nout > 0 ) ? o.nout : 1;
    int k = outlet; if ( k < 0 ) k = 0; if ( k >= n ) k = n - 1;
    x = ( n <= 1 ) ? sx + iow() / 2 : sx + iow() / 2 + ( w - iow() ) * k / ( n - 1 );
    y = sy + box_h( f, o ) - 1;
    return true;
}

bool PdEditorView::inlet_pos( ui::App& app, int idx, int inlet, int& x, int& y ) const
{
    if ( idx < 0 || idx >= (int) objs_.size() ) return false;
    const PdObj& o = objs_[idx];
    const ui::Font& f = app.mono;
    int sx, sy; obj_screen( o, sx, sy );
    int w = box_w( f, o );
    int n = ( o.nin > 0 ) ? o.nin : 1;
    int k = inlet; if ( k < 0 ) k = 0; if ( k >= n ) k = n - 1;
    x = ( n <= 1 ) ? sx + iow() / 2 : sx + iow() / 2 + ( w - iow() ) * k / ( n - 1 );
    y = sy;
    return true;
}

// ============================================================================
//  Hit testing (topmost object first == reverse model order)
// ============================================================================
int PdEditorView::obj_at( ui::App& app, int mx, int my ) const
{
    const ui::Font& f = app.mono;
    for ( int i = (int) objs_.size() - 1; i >= 0; --i )
    {
        int sx, sy; obj_screen( objs_[i], sx, sy );
        int w = box_w( f, objs_[i] );
        int bh = box_h( f, objs_[i] );
        if ( mx >= sx && mx < sx + w && my >= sy && my < sy + bh )
            return i;
    }
    return -1;
}

bool PdEditorView::outlet_at( ui::App& app, int mx, int my, int& idx, int& outlet ) const
{
    const ui::Font& f = app.mono;
    for ( int i = (int) objs_.size() - 1; i >= 0; --i )
    {
        const PdObj& o = objs_[i];
        if ( o.nout <= 0 ) continue;
        int sx, sy; obj_screen( o, sx, sy );
        int w = box_w( f, o );
        int oy = sy + box_h( f, o ) - 1;
        for ( int k = 0; k < o.nout; ++k )
        {
            int cx = ( o.nout <= 1 ) ? sx + iow() / 2
                                     : sx + iow() / 2 + ( w - iow() ) * k / ( o.nout - 1 );
            if ( mx >= cx - iow() / 2 - HITPAD && mx <= cx + iow() / 2 + HITPAD &&
                 my >= oy - ioh() - HITPAD     && my <= oy + HITPAD )
            { idx = i; outlet = k; return true; }
        }
    }
    return false;
}

bool PdEditorView::inlet_at( ui::App& app, int mx, int my, int& idx, int& inlet ) const
{
    const ui::Font& f = app.mono;
    for ( int i = (int) objs_.size() - 1; i >= 0; --i )
    {
        const PdObj& o = objs_[i];
        if ( o.nin <= 0 ) continue;
        int sx, sy; obj_screen( o, sx, sy );
        int w = box_w( f, o );
        for ( int k = 0; k < o.nin; ++k )
        {
            int cx = ( o.nin <= 1 ) ? sx + iow() / 2
                                    : sx + iow() / 2 + ( w - iow() ) * k / ( o.nin - 1 );
            if ( mx >= cx - iow() / 2 - HITPAD && mx <= cx + iow() / 2 + HITPAD &&
                 my >= sy - HITPAD           && my <= sy + ioh() + HITPAD )
            { idx = i; inlet = k; return true; }
        }
    }
    return false;
}

// ============================================================================
//  Model mutation
// ============================================================================
bool PdEditorView::add_conn( int from, int outlet, int to, int inlet )
{
    const int n = (int) objs_.size();
    if ( from < 0 || from >= n || to < 0 || to >= n ) return false;
    if ( from == to )                                  return false;   // no self-patch
    if ( outlet < 0 || outlet >= objs_[from].nout )    return false;
    if ( inlet  < 0 || inlet  >= objs_[to].nin )       return false;
    for ( std::size_t i = 0; i < conns_.size(); ++i )                  // no duplicates
        if ( conns_[i].from == from && conns_[i].outlet == outlet &&
             conns_[i].to == to && conns_[i].inlet == inlet )
            return false;

    PdConn c; c.from = from; c.outlet = outlet; c.to = to; c.inlet = inlet;
    conns_.push_back( c );
    return true;
}

void PdEditorView::delete_obj( ui::App& app, int idx )
{
    if ( idx < 0 || idx >= (int) objs_.size() ) return;

    release_inline_edit();                 // text_target may point into objs_
    objs_.erase( objs_.begin() + idx );

    // drop wires touching idx; renumber the rest down past the hole
    for ( std::size_t i = conns_.size(); i-- > 0; )
    {
        PdConn& c = conns_[i];
        if ( c.from == idx || c.to == idx ) { conns_.erase( conns_.begin() + i ); continue; }
        if ( c.from > idx ) --c.from;
        if ( c.to   > idx ) --c.to;
    }

    if ( m_sel == idx )      m_sel = -1;
    else if ( m_sel > idx )  --m_sel;

    m_move = false; m_wire = false; m_last_click_obj = -1;
    commit( app );
}

void PdEditorView::create_object( ui::App& app, const std::string& text, int px, int py )
{
    PdObj o;
    o.kind = 'o';
    o.x = px; o.y = py;
    o.text = default_gui_record( text );   // bare GUI atoms -> full valid IEMGUI record
    // Give interactive GUI atoms a unique RECEIVE symbol so RUN-mode edits drive the
    // live patch (a toggle can start a [metro]).  Baked into the record so libpd sees
    // it after commit() reloads.
    {
        GuiInfo g = gui_info( o.text );
        if ( g.type != GuiInfo::None && g.recvTok > 1 ) {
            const unsigned id = m_recv_serial++;
            std::vector<std::string> tk = tokenize( o.text );
            auto empty_at = [&]( int i ) {
                return i >= (int) tk.size() || tk[(size_t)i].empty() || tk[(size_t)i] == "empty"; };
            // receive symbol (host injects values here) + send symbol (feedback comes
            // back here, so the widget lights up when a wire drives it).
            if ( empty_at( g.recvTok ) )     set_gui_sym( o.text, g.recvTok,     "pkui"  + std::to_string( id ) );
            if ( empty_at( g.recvTok - 1 ) ) set_gui_sym( o.text, g.recvTok - 1, "pkuis" + std::to_string( id ) );
        }
    }
    io_for( o.text, o.nin, o.nout );
    finalize_obj_edit( app );              // push_back may reallocate objs_
    objs_.push_back( o );
    m_sel = (int) objs_.size() - 1;
    commit( app );
    // NOTE: do NOT auto-enter text edit here.  Doing so put the object in Mode_Edit,
    // and the user's very next click (double-click habit) landed on the Mode_Edit
    // branch and finalized it -- so they could never actually type.  Double-click
    // the placed object to edit its text ("osc~" -> "osc~ 440").
}

// ============================================================================
//  Drawing
// ============================================================================
void PdEditorView::draw( ui::App& app )
{
    if ( !visible ) return;
    const ui::Theme& t = theme();
    SDL_Renderer* r = app.ren;

    // Decay bang flashes: any lit bang whose window elapsed drops back to unlit.
    // Per-object, so several bangs lit by a fan-out all animate independently.
    {
        const unsigned now = SDL_GetTicks();
        bool anyLit = false;
        for ( PdObj& o : objs_ )
            if ( o.flash_until )
            {
                if ( now >= o.flash_until ) { o.gui_val = 0.0; o.flash_until = 0; }
                else anyLit = true;
            }
        if ( anyLit ) app.request_redraw();
    }

    ui::fill_rect( r, rect, t.bg );
    ui::frame_rect( r, rect, t.dim );

    SDL_Rect clip = rect;
    {
    ui::ScopedClip clipScope(r,clip);

    // committed wires (behind the boxes); the selected wire is highlighted + thicker.
    for ( std::size_t i = 0; i < conns_.size(); ++i )
    {
        int x1, y1, x2, y2;
        if ( outlet_pos( app, conns_[i].from, conns_[i].outlet, x1, y1 ) &&
             inlet_pos ( app, conns_[i].to,   conns_[i].inlet,  x2, y2 ) )
        {
            const bool sel = ( (int)i == m_sel_wire );
            ui::set_color( r, sel ? t.sel : t.dim );
            SDL_RenderDrawLine( r, x1, y1, x2, y2 );
            if ( sel ) SDL_RenderDrawLine( r, x1, y1 + 1, x2, y2 + 1 );   // 2px emphasis
        }
    }

    // in-progress wire
    if ( m_wire )
    {
        int x1, y1;
        if ( outlet_pos( app, m_wire_from, m_wire_outlet, x1, y1 ) )
        {
            ui::set_color( r, m_wire_ok ? t.active : t.sel );
            SDL_RenderDrawLine( r, x1, y1, m_wire_x, m_wire_y );
        }
    }

    // boxes on top
    for ( std::size_t i = 0; i < objs_.size(); ++i )
        draw_obj( app, (int) i );

    // highlight a valid inlet under an in-progress wire
    if ( m_wire && m_wire_ok )
    {
        int hx, hy;
        if ( inlet_pos( app, m_wire_to, m_wire_inlet, hx, hy ) )
        {
            SDL_Rect hb { hx - iow() / 2 - 1, hy - 1, iow() + 2, ioh() + 2 };
            ui::fill_rect( r, hb, t.active );
        }
    }

    }

    // draggable H/V scroll bars for navigating large patches
    { int cl, cr, ct, cb; content_bounds( app, cl, cr, ct, cb );
      m_scroll.draw( app, rect, cl, cr, ct, cb, m_ox, m_oy ); }

    // EDIT / RUN mode badge (Ctrl+E toggles) so the current mode is always visible.
    {
        const char* ml = m_edit_mode ? "EDIT  (Ctrl+E)" : "RUN  (Ctrl+E)";
        const int tw = app.mono.text_w( ml );
        SDL_Rect badge{ rect.x + rect.w - tw - 14, rect.y + 5, tw + 8, app.mono.ch() + 4 };
        ui::fill_rect( r, badge, m_edit_mode ? t.panel : t.accent );
        ui::frame_rect( r, badge, t.dim );
        app.mono.draw( r, badge.x + 4, badge.y + 2, ml, m_edit_mode ? t.text : t.bg );
    }

    // Standalone .pd interchange lives in this editor, while the in-memory text
    // remains embedded in the parent .s24 project.
    {
        const int h=app.mono.ch()+10;
        SDL_Rect buttons[3]={{rect.x+5,rect.y+5,58,h},{rect.x+67,rect.y+5,58,h},
                             {rect.x+129,rect.y+5,78,h}};
        const char* labels[3]={"OPEN","SAVE","SAVE AS"};
        for(int i=0;i<3;++i){ui::fill_rect(r,buttons[i],t.panel);ui::frame_rect(r,buttons[i],t.dim);
            app.mono.draw_centered(r,buttons[i],labels[i],t.text);}
    }

    if ( m_mode == Mode_Pick )
        draw_picker( app );
    if ( m_mode == Mode_Props )
        draw_props( app );
}

// Bounding box of all objects in CANVAS PIXELS (o.x*zoom .. + box size).
void PdEditorView::content_bounds( ui::App& app, int& cl, int& cr, int& ct, int& cb ) const
{
    cl = cr = ct = cb = 0;
    const ui::Font& f = app.mono;
    bool first = true;
    for ( const auto& o : objs_ )
    {
        int x0 = (int) std::lround( o.x * m_zoom );
        int y0 = (int) std::lround( o.y * m_zoom );
        int x1 = x0 + box_w( f, o );
        int y1 = y0 + box_h( f, o );
        if ( first ) { cl = x0; cr = x1; ct = y0; cb = y1; first = false; }
        else { if ( x0 < cl ) cl = x0; if ( x1 > cr ) cr = x1;
               if ( y0 < ct ) ct = y0; if ( y1 > cb ) cb = y1; }
    }
}

// Scan the whole patch (serialized) for data-structure templates: a [struct]/#N
// struct defines a template; the drawpolygon/drawcurve/drawnumber objects in the
// SAME canvas describe how to draw each scalar of that type.
void PdEditorView::build_templates()
{
    m_templates.clear();
    const std::string text = serialize();
    std::vector<std::string> recs = split_records( text );
    int cur = -1;                                   // current template (per canvas scope)
    for ( const std::string& rec : recs )
    {
        std::vector<std::string> tk = tokenize( rec );
        if ( tk.empty() ) continue;
        if ( tk[0] == "#N" && tk.size() > 1 && tk[1] == "canvas" )  { cur = -1; continue; }
        if ( tk[0] == "#X" && tk.size() > 1 && tk[1] == "restore" ) { cur = -1; continue; }

        int si = -1;                                // index of the "struct" token
        if      ( tk[0] == "#N" && tk.size() > 1 && tk[1] == "struct" ) si = 1;
        else if ( tk[0] == "#X" && tk.size() > 4 && tk[1] == "obj" && tk[4] == "struct" ) si = 4;
        if ( si >= 0 )
        {
            const std::string name = ( (int) tk.size() > si + 1 ) ? tk[(size_t)si + 1] : std::string();
            int idx = -1;
            for ( int k = 0; k < (int) m_templates.size(); ++k )
                if ( m_templates[k].name == name ) { idx = k; break; }
            if ( idx < 0 ) { m_templates.push_back( PdTemplate{} );
                             idx = (int) m_templates.size() - 1; m_templates[idx].name = name; }
            for ( size_t k = (size_t) si + 2; k + 1 < tk.size(); k += 2 )   // "type field" pairs
                m_templates[idx].fields.push_back( tk[k + 1] );
            cur = idx;
            continue;
        }
        if ( tk[0] == "#X" && tk.size() > 4 && tk[1] == "obj" && cur >= 0 )
        {
            const std::string& c = tk[4];
            if ( c == "drawpolygon" || c == "drawcurve" || c == "filledpolygon" || c == "filledcurve" )
                m_templates[cur].draws.push_back( parse_curve_draw( tk, c ) );
            else if ( c == "drawnumber" || c == "drawtext" || c == "drawsymbol" )
                m_templates[cur].draws.push_back( parse_number_draw( tk ) );
        }
    }
}

const PdTemplate* PdEditorView::find_template( const std::string& name ) const
{
    for ( const PdTemplate& t : m_templates ) if ( t.name == name ) return &t;
    return nullptr;
}

// Render a data-structure scalar: run its template's draw instructions with the
// scalar's field values bound (constants pass through; field-name atoms resolve).
void PdEditorView::draw_scalar( ui::App& app, int i )
{
    const PdObj& o = objs_[i];
    const PdTemplate* t = find_template( o.tmpl );
    SDL_Renderer* r = app.ren;
    const ui::Theme& th = theme();
    int sx, sy; obj_screen( o, sx, sy );
    auto fieldval = [&]( const std::string& nm ) -> float {
        if ( !t ) return 0.f;
        for ( size_t k = 0; k < t->fields.size(); ++k )
            if ( t->fields[k] == nm && k < o.arr.size() ) return o.arr[k];
        return 0.f;
    };
    auto resolve = [&]( const PdFieldDesc& fd ) -> float {
        return fd.isConst ? fd.val : fieldval( fd.field ); };

    if ( !t || t->draws.empty() )                    // no drawing -> a small marker
    {
        ui::set_color( r, th.text );
        SDL_RenderDrawLine( r, sx - 3, sy, sx + 3, sy );
        SDL_RenderDrawLine( r, sx, sy - 3, sx, sy + 3 );
        return;
    }
    for ( const PdDraw& d : t->draws )
    {
        if ( d.kind == PdDraw::Number )
        {
            char buf[32]; std::snprintf( buf, sizeof(buf), "%g", resolve( d.field ) );
            app.mono.draw( r, sx + (int)( resolve( d.x ) * m_zoom ),
                              sy + (int)( resolve( d.y ) * m_zoom ), buf, th.text );
        }
        else                                         // Polygon / Curve
        {
            ui::set_color( r, th.accent );
            const int n = (int) d.coords.size() / 2;
            int px0 = 0, py0 = 0;
            for ( int k = 0; k < n; ++k ) {
                const int cx = sx + (int)( resolve( d.coords[(size_t)2*k]   ) * m_zoom );
                const int cy = sy + (int)( resolve( d.coords[(size_t)2*k+1] ) * m_zoom );
                if ( k > 0 ) SDL_RenderDrawLine( r, px0, py0, cx, cy );
                px0 = cx; py0 = cy;
            }
            if ( d.closed && n > 1 )
                SDL_RenderDrawLine( r, px0, py0,
                                    sx + (int)( resolve( d.coords[0] ) * m_zoom ),
                                    sy + (int)( resolve( d.coords[1] ) * m_zoom ) );
        }
    }
}

void PdEditorView::draw_obj( ui::App& app, int i )
{
    if ( i < 0 || i >= (int) objs_.size() ) return;
    const PdObj& o = objs_[i];
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const ui::Font& f = app.mono;

    if ( o.kind == 'x' && !o.tmpl.empty() ) { draw_scalar( app, i ); return; }   // data-structure scalar

    int sx, sy; obj_screen( o, sx, sy );
    int w  = box_w( f, o );
    int bh = box_h( f, o );
    SDL_Rect box { sx, sy, w, bh };

    Color frame = ( i == m_sel ) ? t.accent : t.dim;
    ui::fill_rect( r, box, t.panel );
    ui::frame_rect( r, box, frame );

    GuiInfo g = ( o.kind == 'o' ) ? gui_info( o.text ) : GuiInfo{};
    if ( o.kind == 'x' && !o.arr.empty() )
    {
        // graph/array gobj: render the stored data as an auto-scaled waveform.
        float mn = o.arr[0], mx = o.arr[0];
        for ( float v : o.arr ) { if ( v < mn ) mn = v; if ( v > mx ) mx = v; }
        if ( mx <= mn ) mx = mn + 1.0f;
        const int n = (int) o.arr.size();
        const int denom = box.w > 0 ? box.w : 1;
        ui::set_color( r, t.accent );
        int px0 = box.x, py0 = box.y + box.h / 2;
        for ( int px = 0; px < box.w; ++px )
        {
            long long idx = (long long) px * n / denom;
            if ( idx >= n ) idx = n - 1;
            if ( idx < 0 )  idx = 0;
            const float nv = ( o.arr[(size_t)idx] - mn ) / ( mx - mn );
            const int xx = box.x + px;
            const int yy = box.y + box.h - 1 - (int) ( nv * ( box.h - 2 ) );
            if ( px > 0 ) SDL_RenderDrawLine( r, px0, py0, xx, yy );
            px0 = xx; py0 = yy;
        }
        f.draw_fitted( r, SDL_Rect{ box.x + 3, box.y + 1, std::max( 1, box.w - 6 ), f.ch() + 1 },
                       o.text, t.dim, false, 1.0f );
    }
    else if ( o.bp_w > 0 )
    {
        // bpatcher: draw the embedded-abstraction WINDOW (a nested frame) + name.
        SDL_Rect inner{ box.x + 2, box.y + 2, box.w - 4, box.h - 4 };
        if ( inner.w > 0 && inner.h > 0 ) ui::frame_rect( r, inner, t.dim );
        std::string nm = o.text;                       // "bpatcher [-flags] name ..."
        std::vector<std::string> tk = tokenize( o.text );
        for ( size_t i = 1; i < tk.size(); ++i )
            if ( !tk[i].empty() && tk[i][0] != '-' ) { nm = tk[i]; break; }
        f.draw_fitted( r, SDL_Rect{ box.x + 5, box.y + 4, std::max( 1, box.w - 10 ),
                                    f.ch() + 2 }, nm, t.text, false, std::max( 1.0f, m_zoom ) );
    }
    else if ( g.type != GuiInfo::None )
    {
        // Draw the REAL GUI widget in the object's place (an SDL slider / knob /
        // toggle / ... ) instead of a text box.
        const int pad = std::max( 2, (int)std::lround( 2 * m_zoom ) );
        const int barW = std::max( 2, (int)std::lround( 3 * m_zoom ) );
        const double dv = o.gui_val_set ? o.gui_val : g.val;   // live value
        switch ( g.type )
        {
            case GuiInfo::Tgl:
                if ( dv != 0.0 ) {
                    ui::set_color( r, t.accent );
                    SDL_RenderDrawLine( r, box.x+pad, box.y+pad, box.x+box.w-1-pad, box.y+box.h-1-pad );
                    SDL_RenderDrawLine( r, box.x+box.w-1-pad, box.y+pad, box.x+pad, box.y+box.h-1-pad );
                }
                break;
            case GuiInfo::Bng: {
                const int rad = std::max( 2, std::min(box.w,box.h)/2 - pad );
                draw_ring( r, box.x+box.w/2, box.y+box.h/2, rad, t.text );
                if ( dv != 0.0 )                                   // flash: fill the middle
                    ui::fill_rect( r, SDL_Rect{ box.x+box.w/2 - rad/2, box.y+box.h/2 - rad/2, rad, rad }, t.accent );
                break; }
            case GuiInfo::Hsl: {
                const int tx = box.x + pad + (int)std::lround( dv * (box.w-2*pad-barW) );
                ui::fill_rect( r, SDL_Rect{ tx, box.y+1, barW, box.h-2 }, t.accent );
                break; }
            case GuiInfo::Vsl: {
                const int ty = box.y + box.h - pad - barW - (int)std::lround( dv * (box.h-2*pad-barW) );
                ui::fill_rect( r, SDL_Rect{ box.x+1, ty, box.w-2, barW }, t.accent );
                break; }
            case GuiInfo::Knob: {
                const int cx = box.x+box.w/2, cy = box.y+box.h/2, rad = std::min(box.w,box.h)/2-pad;
                draw_ring( r, cx, cy, std::max(3,rad), t.text );
                const double frac = (g.max>g.min) ? (dv-g.min)/(g.max-g.min) : 0.5;
                const double ang = -3.14159*1.25 + frac*3.14159*1.5;   // 270-deg sweep
                ui::set_color( r, t.accent );
                SDL_RenderDrawLine( r, cx, cy, cx+(int)(std::cos(ang)*rad), cy+(int)(std::sin(ang)*rad) );
                break; }
            case GuiInfo::Nbx: {
                const bool typing = ( m_num_edit == i );
                ui::set_color( r, typing ? t.active : t.text );
                const int tri = std::max( 3, (int)std::lround( 4 * m_zoom ) );
                SDL_RenderDrawLine( r, box.x, box.y+2, box.x+tri, box.y+box.h/2 );   // corner triangle
                SDL_RenderDrawLine( r, box.x, box.y+box.h-2, box.x+tri, box.y+box.h/2 );
                char v[24]; std::snprintf( v, sizeof(v), "%g", dv );
                const std::string shown = typing ? m_num_buf : std::string( v );
                f.draw_fitted( r, SDL_Rect{ box.x+tri+2, box.y, std::max(1,box.w-tri-4), box.h },
                               shown, typing ? t.active : t.text, false, std::max( 1.0f, m_zoom ) );
                if ( typing ) { ui::frame_rect( r, box, t.active ); app.request_redraw(); }
                break; }
            case GuiInfo::Hradio: {
                const int cw = std::max( 1, box.w / std::max(1,g.cells) );
                for ( int c=0;c<g.cells;++c ){ SDL_Rect cell{ box.x+c*cw, box.y, cw, box.h };
                    ui::frame_rect( r, cell, t.dim );
                    if ( c == (int)std::lround(dv) ) ui::fill_rect( r, SDL_Rect{cell.x+pad,cell.y+pad,cell.w-2*pad,cell.h-2*pad}, t.accent ); }
                break; }
            case GuiInfo::Vradio: {
                const int chh = std::max( 1, box.h / std::max(1,g.cells) );
                for ( int c=0;c<g.cells;++c ){ SDL_Rect cell{ box.x, box.y+c*chh, box.w, chh };
                    ui::frame_rect( r, cell, t.dim );
                    if ( c == (int)std::lround(dv) ) ui::fill_rect( r, SDL_Rect{cell.x+pad,cell.y+pad,cell.w-2*pad,cell.h-2*pad}, t.accent ); }
                break; }
            default: break;
        }
    }
    else
    {
        // message boxes: a flag-ish notch on the right edge (scaled with the zoom)
        if ( o.kind == 'm' )
        {
            ui::set_color( r, frame );
            const int fl = std::max( 3, (int)std::lround( FLAG * m_zoom ) );
            int rx = box.x + w - 1;
            SDL_RenderDrawLine( r, rx, box.y,          rx - fl, box.y + bh / 2 );
            SDL_RenderDrawLine( r, rx - fl, box.y + bh / 2, rx, box.y + bh - 1 );
        }
        // body text -- scaled with the zoom and CLIPPED to the box so it never
        // spills out (the old fixed-size draw scattered text across the canvas).
        const int px = std::max( 1, (int)std::lround( PADX * m_zoom ) );
        const bool editing = ( m_mode == Mode_Edit && i == m_edit_obj );
        f.draw_fitted( r, SDL_Rect{ box.x + px, box.y, std::max( 1, box.w - 2*px ), box.h },
                       o.text, t.text, false, std::max( 1.0f, m_zoom ) );
        if ( editing )
        {
            // blinking caret at the end of the text + a bright frame, so it's
            // obvious the object is being typed into.
            ui::frame_rect( r, box, t.active );
            if ( ( SDL_GetTicks() / 500 ) & 1 )
            {
                const int scale = (int)std::max( 1.0f, m_zoom );
                const int cw = std::min( box.w - 2*px, f.text_w( o.text ) * scale );
                ui::vline( r, box.x + px + cw + 1, box.y + 2, box.y + box.h - 2, t.text );
            }
        }
    }

    // inlets along the TOP edge
    for ( int k = 0; k < o.nin; ++k )
    {
        int cx = ( o.nin <= 1 ) ? sx + iow() / 2
                                : sx + iow() / 2 + ( w - iow() ) * k / ( o.nin - 1 );
        SDL_Rect m { cx - iow() / 2, sy, iow(), ioh() };
        ui::fill_rect( r, m, t.text );
    }
    // outlets along the BOTTOM edge
    for ( int k = 0; k < o.nout; ++k )
    {
        int cx = ( o.nout <= 1 ) ? sx + iow() / 2
                                 : sx + iow() / 2 + ( w - iow() ) * k / ( o.nout - 1 );
        SDL_Rect m { cx - iow() / 2, sy + bh - ioh(), iow(), ioh() };
        // armed as the click-to-connect SOURCE outlet -> lit (larger accent mark)
        if ( i == m_link_from && k == m_link_outlet )
        {
            SDL_Rect hl { m.x - 2, m.y - 2, m.w + 4, m.h + 4 };
            ui::fill_rect( r, hl, t.accent );
        }
        else ui::fill_rect( r, m, t.text );
    }
}

// ============================================================================
//  Module picker overlay
// ============================================================================
// Palette categories for the group/filter buttons (index 0 == "All"), laid out in
// a 5x2 grid of buttons above the card grid.
static const char* const PICK_CATS[] =
    { "All", "Sig~", "Ctrl", "Math", "MIDI", "GUI", "List", "Array", "Time", "Data" };
static const int N_PICK_CATS = (int)( sizeof( PICK_CATS ) / sizeof( PICK_CATS[0] ) );
static const int CAT_COLS = 5;
static const int CAT_ROWS = 2;

// Classify an object name into a category (1..N; matches PICK_CATS indices).  Explicit
// families are checked before the "~ == signal" rule so tab*~ etc. land in Array.
static int pd_category( const std::string& n )
{
    auto is = [&]( std::initializer_list<const char*> names ) {
        for ( const char* s : names ) if ( n == s ) return true;
        return false; };
    auto pre = [&]( const char* p ) { return n.rfind( p, 0 ) == 0; };
    if ( is( { "notein","ctlin","bendin","pgmin","touchin","polytouchin","midiin","sysexin",
               "midirealtimein","noteout","ctlout","bendout","pgmout","touchout","polytouchout",
               "midiout","makenote","stripnote","poly" } ) ) return 4;      // MIDI
    if ( is( { "bng","tgl","nbx","hsl","vsl","hradio","vradio","vu","cnv" } ) ) return 5;   // GUI
    if ( pre( "tab" ) || pre( "array" ) || is( { "soundfiler","readsf~","writesf~" } ) ) return 7;  // Array
    if ( pre( "draw" ) || pre( "filled" ) || pre( "struct" ) || pre( "scalar" ) ||
         is( { "plot","pointer","get","set","getsize","setsize","element","template","append" } ) ) return 9;  // Data
    if ( pre( "list" ) || pre( "text" ) || is( { "symbol","makefilename","oscformat","oscparse",
               "fudiformat","fudiparse","qlist","textfile" } ) ) return 6;  // List/text
    if ( is( { "metro","delay","del","line","timer","pipe","until","cputime","realtime" } ) ) return 8;  // Time
    if ( !n.empty() && n.back() == '~' ) return 1;                          // Signal
    if ( is( { "+","-","*","/","div","mod","pow","abs","sqrt","sin","cos","tan","atan","atan2",
               "exp","log","max","min","mtof","ftom","dbtorms","rmstodb","dbtopow","powtodb",
               "random","wrap","==","!=",">","<",">=","<=","&","|","&&","||","<<",">>" } ) ) return 3;  // Math
    return 2;                                                               // Ctrl / glue
}

// Finer TYPE within a category -- drives the sub-group headers in the palette so each
// filter (e.g. Sig~) is itself split into osc / filters / delay / fft / math / ...
static const char* pd_subgroup( const std::string& n )
{
    auto is = [&]( std::initializer_list<const char*> names ) {
        for ( const char* s : names ) if ( n == s ) return true; return false; };
    auto pre = [&]( const char* p ) { return n.rfind( p, 0 ) == 0; };
    switch ( pd_category( n ) )
    {
        case 1:  // Sig~
            if ( is( { "osc~","phasor~","cos~","sig~","noise~" } ) ) return "oscillators";
            if ( pre( "rev" ) ) return "reverb";
            if ( is( { "lop~","hip~","bp~","vcf~","biquad~","cpole~","czero~","rpole~","rzero~",
                       "czero_rev~","rzero_rev~","hilbert~","slop~" } ) ) return "filters";
            if ( pre( "del" ) || n == "vd~" ) return "delay";
            if ( is( { "fft~","ifft~","rfft~","rifft~","framp~","complex-mod~" } ) ) return "ffts";
            if ( is( { "send~","receive~","throw~","catch~","block~" } ) ) return "routing";
            if ( is( { "line~","vline~","snapshot~","vsnapshot~","samphold~","threshold~",
                       "samplerate~","env~","bang~" } ) ) return "control";
            if ( is( { "+~","-~","*~","/~","max~","min~","abs~","exp~","log~","pow~","sqrt~",
                       "rsqrt~","wrap~","clip~","mtof~","ftom~","dbtorms~","rmstodb~",
                       "dbtopow~","powtodb~" } ) ) return "signal math";
            return "misc";
        case 2:  // Ctrl
            if ( is( { "trigger","t","route","select","sel","moses","spigot","change","swap",
                       "until","pack","unpack" } ) ) return "flow";
            if ( is( { "print","print~" } ) ) return "print";
            return "glue";
        case 3:  // Math
            if ( is( { "sin","cos","tan","atan","atan2","exp","log" } ) ) return "trig";
            if ( is( { "==","!=",">","<",">=","<=","&","|","&&","||","<<",">>" } ) ) return "compare";
            if ( is( { "mtof","ftom","dbtorms","rmstodb","dbtopow","powtodb" } ) ) return "convert";
            if ( is( { "random","max","min","wrap" } ) ) return "random";
            return "arith";
        case 4:  // MIDI
            if ( is( { "noteout","ctlout","bendout","pgmout","touchout","polytouchout","midiout" } ) ) return "out";
            if ( is( { "makenote","stripnote","poly" } ) ) return "util";
            return "in";
        case 5:  // GUI
            if ( is( { "bng","tgl" } ) ) return "bang/toggle";
            if ( is( { "hsl","vsl" } ) ) return "slider";
            if ( is( { "hradio","vradio" } ) ) return "radio";
            if ( n == "nbx" ) return "number";
            return "misc";
        case 6:  // List/text
            if ( pre( "list" ) ) return "list";
            if ( pre( "text" ) || is( { "textfile","qlist" } ) ) return "text";
            if ( is( { "symbol","makefilename" } ) ) return "symbol";
            return "osc/fudi";
        case 7:  // Array
            if ( pre( "array" ) ) return "array";
            if ( pre( "tab" ) ) return "table";
            return "soundfile";
        case 8:  // Time
            if ( is( { "cputime","realtime" } ) ) return "system";
            return "clock";
        case 9:  // Data
            if ( pre( "draw" ) || pre( "filled" ) || n == "plot" ) return "draw";
            if ( pre( "struct" ) || pre( "scalar" ) || n == "template" ) return "struct";
            return "access";
        default: return "misc";
    }
}

std::vector<std::string> PdEditorView::filtered() const
{
    std::vector<std::string> out;
    for ( int i = 0; i < N_OBJECTS; ++i )
    {
        if ( !ci_contains( OBJECTS[i], m_search ) ) continue;
        if ( m_pick_cat != 0 && pd_category( OBJECTS[i] ) != m_pick_cat ) continue;   // type filter
        out.push_back( OBJECTS[i] );
    }
    // sort by (category, sub-group) so items cluster under their type headers.
    std::stable_sort( out.begin(), out.end(), []( const std::string& a, const std::string& b ) {
        const int ca = pd_category( a ), cb = pd_category( b );
        if ( ca != cb ) return ca < cb;
        return std::string( pd_subgroup( a ) ) < std::string( pd_subgroup( b ) );
    } );
    return out;
}

// Palette metrics.  The picker is a single-column LIST of draggable objects with
// type SEPARATORS between sub-groups -- but the separators are only shown once the
// (filtered) list is long enough to be worth grouping.
static const int PICK_COLS      = 2;      // (kept: card preview size in draw_obj_preview)
static const int PICK_CARDH     = 46;
static const int PICK_GRIDR     = 6;
static const int PICK_LIST_ROWS = 15;     // visible list rows
static const int PICK_GROUP_MIN = 10;     // show separators only for lists this long

// One list entry: a sub-group HEADER (separator + label) or a draggable object.
struct PickItem { bool header = false; std::string text; };
static std::vector<PickItem> build_pick_list( const std::vector<std::string>& fl )
{
    std::vector<PickItem> items;
    const bool group = ( (int) fl.size() >= PICK_GROUP_MIN );   // separators only when long
    std::string lastSub;
    for ( const std::string& name : fl )
    {
        if ( group )
        {
            const std::string sub = pd_subgroup( name );
            if ( sub != lastSub ) { items.push_back( PickItem{ true, sub } ); lastSub = sub; }
        }
        items.push_back( PickItem{ false, name } );
    }
    return items;
}

SDL_Rect PdEditorView::picker_rect( ui::App& app ) const
{
    const int rowh = app.mono.ch() + 6;
    const int w = PICK_W + 44;
    const int h = rowh + 2 + CAT_ROWS * rowh + 2 + PICK_LIST_ROWS * rowh + 4;
    int x = m_pick_x, y = m_pick_y;
    if ( x + w > app.w ) x = app.w - w;
    if ( y + h > app.h ) y = app.h - h;
    if ( x < 0 ) x = 0;
    if ( y < 0 ) y = 0;
    SDL_Rect q { x, y, w, h };
    return q;
}

// A mini preview of an object drawn inside `box`: the name + a small box with its
// inlet/outlet dots (or a widget hint for GUI atoms) so you can SEE what it is.
void PdEditorView::draw_obj_preview( ui::App& app, const std::string& name, const SDL_Rect& box )
{
    SDL_Renderer* r = app.ren; const ui::Theme& t = theme();
    app.mono.draw_fitted( r, SDL_Rect{ box.x+3, box.y+3, box.w-6, app.mono.ch()+2 }, name, t.text, true );
    int nin = 1, nout = 1; io_for( name, nin, nout );
    SDL_Rect pv{ box.x + box.w/2 - 22, box.y + box.h - 20, 44, 15 };
    ui::fill_rect( r, pv, t.bg ); ui::frame_rect( r, pv, t.dim );
    GuiInfo g = gui_info( name );
    if ( g.type == GuiInfo::Hsl || g.type == GuiInfo::Nbx )
        ui::fill_rect( r, SDL_Rect{ pv.x+pv.w/2, pv.y+2, 3, pv.h-4 }, t.accent );
    else if ( g.type == GuiInfo::Vsl )
        ui::fill_rect( r, SDL_Rect{ pv.x+2, pv.y+pv.h/2, pv.w-4, 3 }, t.accent );
    else if ( g.type == GuiInfo::Tgl || g.type == GuiInfo::Bng )
        draw_ring( r, pv.x+pv.w/2, pv.y+pv.h/2, 4, t.accent );
    for ( int k = 0; k < nin; ++k ) {
        int cx = ( nin<=1 ) ? pv.x+3 : pv.x+3 + (pv.w-6)*k/(nin-1);
        ui::fill_rect( r, SDL_Rect{ cx-1, pv.y, 3, 2 }, t.text );
    }
    for ( int k = 0; k < nout; ++k ) {
        int cx = ( nout<=1 ) ? pv.x+3 : pv.x+3 + (pv.w-6)*k/(nout-1);
        ui::fill_rect( r, SDL_Rect{ cx-1, pv.y+pv.h-2, 3, 2 }, t.text );
    }
}

void PdEditorView::draw_picker( ui::App& app )
{
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const int rowh = app.mono.ch() + 6;

    SDL_Rect p = picker_rect( app );
    ui::fill_rect( r, p, t.panel );
    ui::frame_rect( r, p, t.hi );

    // search box
    SDL_Rect sb { p.x + 2, p.y + 2, p.w - 4, rowh };
    ui::fill_rect( r, sb, t.bg );
    ui::frame_rect( r, sb, t.dim );
    app.mono.draw( r, sb.x + 4, sb.y + 3,
                   m_search.empty() ? std::string("filter...  (drag a card onto the canvas)") : m_search,
                   m_search.empty() ? t.dim : t.text );
    if ( app.editing_text() )                          // caret
    {
        int cx = sb.x + 4 + app.mono.text_w( m_search );
        ui::vline( r, cx, sb.y + 3, sb.y + rowh - 3, t.text );
    }

    // category filter buttons (5x2): click to show only that group.
    const int btnW = ( p.w - 4 ) / CAT_COLS;
    for ( int i = 0; i < N_PICK_CATS; ++i )
    {
        const int row = i / CAT_COLS, col = i % CAT_COLS;
        SDL_Rect b { p.x + 2 + col * btnW, p.y + 2 + rowh + 2 + row * rowh, btnW - 1, rowh - 1 };
        const bool on = ( i == m_pick_cat );
        ui::fill_rect( r, b, on ? t.accent : t.bg );
        ui::frame_rect( r, b, on ? t.accent : t.dim );
        app.mono.draw_fitted( r, SDL_Rect{ b.x + 2, b.y + 2, b.w - 4, b.h - 4 },
                              PICK_CATS[i], on ? t.panel : t.text, true );
    }

    // object LIST with type separators (grouping kicks in only for long lists).
    std::vector<PickItem> items = build_pick_list( filtered() );
    const int listTop = p.y + 2 + rowh + 2 + CAT_ROWS * rowh + 2;
    int maxscroll = (int) items.size() - PICK_LIST_ROWS; if ( maxscroll < 0 ) maxscroll = 0;
    if ( m_pick_scroll > maxscroll ) m_pick_scroll = maxscroll;
    if ( m_pick_scroll < 0 ) m_pick_scroll = 0;

    int mx, my; ui::mouse_logical( app, mx, my );   // logical, not window px
    for ( int row = 0; row < PICK_LIST_ROWS; ++row )
    {
        const int ii = m_pick_scroll + row;
        if ( ii >= (int) items.size() ) break;
        const PickItem& it = items[ii];
        SDL_Rect rr { p.x + 2, listTop + row * rowh, p.w - 4, rowh };
        if ( it.header )                              // sub-group separator + label
        {
            ui::hline( r, rr.x + 2, rr.x + rr.w - 2, rr.y + rowh - 1, t.dim );
            app.mono.draw( r, rr.x + 4, rr.y + 2, it.text, t.accent );
        }
        else
        {
            const bool hover = in_rect( rr, mx, my ) && !m_pick_drag;
            if ( hover ) ui::fill_rect( r, rr, t.sel );
            app.mono.draw( r, rr.x + 10, rr.y + 2, it.text, t.text );
            // small inlet/outlet hint at the right edge
            int nin = 1, nout = 1; io_for( it.text, nin, nout );
            char io[16]; std::snprintf( io, sizeof(io), "%d/%d", nin, nout );
            app.mono.draw( r, rr.x + rr.w - app.mono.text_w( io ) - 4, rr.y + 2, io, t.dim );
        }
    }

    // drag ghost following the cursor (drawn last so it floats above everything)
    if ( m_pick_drag )
    {
        SDL_Rect ghost { m_pick_drag_x - 44, m_pick_drag_y - 12, 88, 24 };
        ui::fill_rect( r, ghost, t.panel );
        ui::frame_rect( r, ghost, t.accent );
        app.mono.draw_fitted( r, SDL_Rect{ ghost.x+4, ghost.y+4, ghost.w-8, ghost.h-8 },
                              m_pick_drag_name, t.text, true );
    }
}

// ============================================================================
//  IEMGUI Properties dialog (right-click a GUI atom) -- edit size / range /
//  send / receive / label; each field writes its RAW token back to the record.
// ============================================================================
SDL_Rect PdEditorView::props_rect( ui::App& app ) const
{
    const int rowh = app.mono.ch() + 8;
    const int w = 260;
    const int h = 4 + rowh + (int) m_props.size() * rowh + rowh + 4;   // title + fields + close
    return SDL_Rect{ rect.x + ( rect.w - w ) / 2, rect.y + 40, w, h };
}

void PdEditorView::open_props( ui::App& app, int oi )
{
    if ( oi < 0 || oi >= (int) objs_.size() ) return;
    GuiInfo g = gui_info( objs_[oi].text );
    if ( g.type == GuiInfo::None || g.recvTok < 1 ) return;   // IEMGUIs only
    m_prop_obj = oi; m_prop_edit = -1; m_props.clear();
    std::vector<std::string> tk = tokenize( objs_[oi].text );
    auto sym_at = [&]( int i ) -> std::string {
        if ( i < 1 || i >= (int) tk.size() ) return std::string();
        return tk[(size_t)i] == "empty" ? std::string() : tk[(size_t)i]; };
    auto num_at = [&]( int i ) -> std::string {
        return ( i >= 1 && i < (int) tk.size() ) ? tk[(size_t)i] : std::string( "0" ); };
    auto add = [&]( const char* nm, const std::string& v, int tok, bool sym ) {
        PropField f; f.name = nm; f.val = v; f.tok = tok; f.sym = sym; m_props.push_back( f ); };

    add( "size", num_at(1), 1, false );                       // token 1 for every IEMGUI
    if ( g.type == GuiInfo::Hsl || g.type == GuiInfo::Vsl || g.type == GuiInfo::Nbx )
    {
        add( "height", num_at(2), 2, false );
        add( "min",    num_at(3), 3, false );
        add( "max",    num_at(4), 4, false );
    }
    else if ( g.type == GuiInfo::Hradio || g.type == GuiInfo::Vradio )
        add( "cells", num_at(4), 4, false );
    // send / receive / label sit uniformly at recvTok-1 / recvTok / recvTok+1.
    add( "send",    sym_at(g.recvTok - 1), g.recvTok - 1, true );
    add( "receive", sym_at(g.recvTok),     g.recvTok,     true );
    add( "label",   sym_at(g.recvTok + 1), g.recvTok + 1, true );
    m_mode = Mode_Props;
    app.request_redraw();
}

void PdEditorView::close_props( ui::App& app )
{
    if ( app.editing_text() ) app.end_text();
    m_mode = Mode_None; m_prop_obj = -1; m_prop_edit = -1; m_props.clear();
    app.request_redraw();
}

void PdEditorView::apply_prop( ui::App& app, int fieldIdx )
{
    if ( m_prop_obj < 0 || m_prop_obj >= (int) objs_.size() ) return;
    if ( fieldIdx < 0 || fieldIdx >= (int) m_props.size() ) return;
    PropField& f = m_props[fieldIdx];
    if ( f.tok < 1 ) return;
    if ( f.sym )
    {
        std::string s = f.val.empty() ? std::string( "empty" ) : f.val;
        for ( char& c : s ) if ( c == ' ' ) c = '_';         // Pd symbols have no spaces
        set_gui_sym( objs_[m_prop_obj].text, f.tok, s );
    }
    else
        set_gui_token( objs_[m_prop_obj].text, f.tok, std::atof( f.val.c_str() ) );
    io_for( objs_[m_prop_obj].text, objs_[m_prop_obj].nin, objs_[m_prop_obj].nout );
    commit( app );                                            // reload libpd with the new record
}

void PdEditorView::draw_props( ui::App& app )
{
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const int rowh = app.mono.ch() + 8;
    SDL_Rect p = props_rect( app );
    ui::fill_rect( r, p, t.panel );
    ui::frame_rect( r, p, t.hi );

    std::string title = "Properties";
    if ( m_prop_obj >= 0 && m_prop_obj < (int) objs_.size() )
        title += ": " + first_word( objs_[m_prop_obj].text );
    app.mono.draw( r, p.x + 6, p.y + 4, title, t.text );

    const int labelW = 74;
    for ( int i = 0; i < (int) m_props.size(); ++i )
    {
        const int ry = p.y + 4 + rowh + i * rowh;
        app.mono.draw( r, p.x + 6, ry + 4, m_props[i].name, t.dim );
        SDL_Rect vb { p.x + 6 + labelW, ry + 2, p.w - 12 - labelW, rowh - 4 };
        const bool ed = ( m_prop_edit == i );
        ui::fill_rect( r, vb, ed ? t.sel : t.bg );
        ui::frame_rect( r, vb, ed ? t.accent : t.dim );
        app.mono.draw( r, vb.x + 4, vb.y + 2, m_props[i].val, t.text );
        if ( ed && app.editing_text() )
        {
            int cx = vb.x + 4 + app.mono.text_w( m_props[i].val );
            ui::vline( r, cx, vb.y + 2, vb.y + vb.h - 2, t.text );
        }
    }
    SDL_Rect cb { p.x + p.w - 60, p.y + p.h - rowh, 54, rowh - 4 };
    ui::fill_rect( r, cb, t.bg ); ui::frame_rect( r, cb, t.dim );
    app.mono.draw( r, cb.x + 10, cb.y + 2, "Close", t.text );
}

bool PdEditorView::props_mouse( ui::App& app, const ui::MouseEv& e, bool downEdge )
{
    (void) downEdge;
    if ( !e.pressed ) return true;
    SDL_Rect p = props_rect( app );
    const int rowh = app.mono.ch() + 8;
    SDL_Rect cb { p.x + p.w - 60, p.y + p.h - rowh, 54, rowh - 4 };
    if ( in_rect( cb, e.x, e.y ) || !in_rect( p, e.x, e.y ) ) { close_props( app ); return true; }

    const int labelW = 74;
    for ( int i = 0; i < (int) m_props.size(); ++i )
    {
        const int ry = p.y + 4 + rowh + i * rowh;
        SDL_Rect vb { p.x + 6 + labelW, ry + 2, p.w - 12 - labelW, rowh - 4 };
        if ( in_rect( vb, e.x, e.y ) )
        {
            if ( app.editing_text() ) app.end_text();
            m_prop_edit = i;
            app.begin_text( &m_props[i].val,
                            [&app]{ app.request_redraw(); },
                            [this, &app, i]( bool ok ) { if ( ok ) apply_prop( app, i );
                                                         m_prop_edit = -1; app.request_redraw(); } );
            app.request_redraw();
            return true;
        }
    }
    return true;
}

// ============================================================================
//  Input
// ============================================================================
bool PdEditorView::on_mouse( ui::App& app, const ui::MouseEv& e )
{
    // maintain left-button held state / down-edge for the whole handler
    bool downEdge = false;
    if ( e.button == SDL_BUTTON_LEFT )
    {
        if ( e.pressed ) { downEdge = !m_ldown; m_ldown = true; }
        else             m_ldown = false;
    }
    if(downEdge){
        const int h=app.mono.ch()+10;
        SDL_Rect buttons[3]={{rect.x+5,rect.y+5,58,h},{rect.x+67,rect.y+5,58,h},
                             {rect.x+129,rect.y+5,78,h}};
        for(int i=0;i<3;++i)if(in_rect(buttons[i],e.x,e.y)){
            if(i==0&&on_open_file)on_open_file();
            if(i==1&&on_save_file)on_save_file();
            if(i==2&&on_save_file_as)on_save_file_as();
            app.request_redraw();return true;
        }
    }

    // ---- editing an object's text: a fresh press commits it ----
    if ( m_mode == Mode_Edit )
    {
        if ( downEdge ) finalize_obj_edit( app );
        app.request_redraw();
        return true;                                   // swallow all mouse while editing
    }

    // ---- module picker open: it eats all mouse ----
    if ( m_mode == Mode_Pick )
        return picker_mouse( app, e, downEdge );

    // ---- properties dialog open: it eats all mouse ----
    if ( m_mode == Mode_Props )
        return props_mouse( app, e, downEdge );

    // ---- scroll bars (bottom + right strips) for large patches ----
    { int cl, cr, ct, cb; content_bounds( app, cl, cr, ct, cb );
      if ( m_scroll.on_mouse( rect, e, downEdge, cl, cr, ct, cb, m_ox, m_oy ) )
      { app.request_redraw(); return true; } }

    // ---- right-click: on an object -> delete it; on empty canvas -> open the
    //      object picker (add), consistent with the patchbay / rack editor. ----
    if ( e.pressed && e.button == SDL_BUTTON_RIGHT )
    {
        m_link_from = -1;                              // cancel any armed outlet
        int oi = obj_at( app, e.x, e.y );
        if ( oi >= 0 )
        {
            // a GUI atom -> Pd-style Properties dialog; a normal object -> delete.
            if ( gui_info( objs_[oi].text ).type != GuiInfo::None )
                open_props( app, oi );
            else
                delete_obj( app, oi );                 // fires commit()
        }
        else open_picker( app, e.x, e.y );             // empty -> add object
        app.request_redraw();
        return true;
    }

    // ---- left button ----
    if ( e.button == SDL_BUTTON_LEFT )
    {
        if ( e.pressed )
            return downEdge ? press_left( app, e.x, e.y )
                            : drag_left ( app, e.x, e.y );
        return release_left( app, e.x, e.y );
    }
    return false;
}

// Apply a GUI-atom interaction, writing the new value into the object's .pd text.
// `press` == the initial click (tgl toggles / relative drags snapshot here).
// Returns true if `oi` is a GUI atom (so the caller consumes the gesture).
bool PdEditorView::gui_interact( ui::App& app, int oi, int mx, int my, bool press )
{
    if ( oi < 0 || oi >= (int) objs_.size() ) return false;
    PdObj& o = objs_[oi];
    if ( o.kind != 'o' ) return false;
    GuiInfo g = gui_info( o.text );
    if ( g.type == GuiInfo::None ) return false;

    int sx, sy; obj_screen( o, sx, sy );
    const ui::Font& f = app.mono;
    const int w = box_w( f, o ), h = box_h( f, o );
    const int pad = std::max( 2, (int)std::lround( 2 * m_zoom ) );
    auto cl01 = []( double v ){ return v < 0.0 ? 0.0 : ( v > 1.0 ? 1.0 : v ); };
    const double cur = o.gui_val_set ? o.gui_val : g.val;   // current live value

    // Update ONLY the model value (o.gui_val); draw_obj renders from it directly.
    // The .pd text is rewritten once on release (flush_gui_value), so dragging is
    // glitch-free -- no per-frame tokenize/re-parse.
    switch ( g.type )
    {
        case GuiInfo::Tgl:
            if ( press ) o.gui_val = ( cur != 0.0 ) ? 0.0 : 1.0;   // toggle on click
            break;
        case GuiInfo::Bng:
            if ( press ) { o.gui_val = 1.0; o.flash_until = SDL_GetTicks() + 150; }  // momentary flash
            break;
        case GuiInfo::Hsl:
            o.gui_val = cl01( (double)( mx - sx - pad ) / std::max( 1, w - 2*pad ) );
            break;
        case GuiInfo::Vsl:
            o.gui_val = cl01( 1.0 - (double)( my - sy - pad ) / std::max( 1, h - 2*pad ) );
            break;
        case GuiInfo::Knob:
            o.gui_val = g.min + cl01( (double)( mx - sx ) / std::max( 1, w ) ) * ( g.max - g.min );
            break;
        case GuiInfo::Nbx: {
            if ( press ) { m_gui_press_val = cur; m_gui_press_y = my; }
            double step = ( g.max > g.min && g.max < 1e36 ) ? ( g.max - g.min ) / 200.0 : 1.0;
            double v = m_gui_press_val + ( m_gui_press_y - my ) * step;
            if ( g.min > -1e36 && v < g.min ) v = g.min;
            if ( g.max <  1e36 && v > g.max ) v = g.max;
            o.gui_val = v; break; }
        case GuiInfo::Hradio: {
            int cw = std::max( 1, w / std::max(1, g.cells) );
            int cell = ( mx - sx ) / cw; o.gui_val = cell < 0 ? 0 : ( cell >= g.cells ? g.cells-1 : cell );
            break; }
        case GuiInfo::Vradio: {
            int chh = std::max( 1, h / std::max(1, g.cells) );
            int cell = ( my - sy ) / chh; o.gui_val = cell < 0 ? 0 : ( cell >= g.cells ? g.cells-1 : cell );
            break; }
        default: break;
    }
    o.gui_val_set = true;
    app.request_redraw();
    return true;
}

// Serialize the live gui_val into the object's .pd text (called once on release /
// click, then commit reloads libpd) -- the value is encoded the way Pd stores it.
void PdEditorView::flush_gui_value( int oi )
{
    if ( oi < 0 || oi >= (int) objs_.size() ) return;
    PdObj& o = objs_[oi];
    GuiInfo g = gui_info( o.text );
    if ( g.type == GuiInfo::None || !o.gui_val_set ) return;
    if ( g.type == GuiInfo::Bng ) return;                       // momentary, nothing to store
    if ( g.type == GuiInfo::Hsl )
        set_gui_token( o.text, g.valTok, (double)std::lround( o.gui_val * std::max(1, g.w-1) * 100 ) );
    else if ( g.type == GuiInfo::Vsl )
        set_gui_token( o.text, g.valTok, (double)std::lround( o.gui_val * std::max(1, g.h-1) * 100 ) );
    else
        set_gui_token( o.text, g.valTok, o.gui_val );          // tgl / nbx / knob / radio
}

// The atom's receive symbol (empty string if none / not an addressable GUI).
std::string PdEditorView::gui_recv_symbol( const PdObj& o ) const
{
    GuiInfo g = gui_info( o.text );
    if ( g.recvTok < 1 ) return std::string();
    std::vector<std::string> tk = tokenize( o.text );
    if ( g.recvTok >= (int)tk.size() ) return std::string();
    const std::string& s = tk[(size_t)g.recvTok];
    return ( s == "empty" ) ? std::string() : s;
}

// The atom's SEND symbol (empty if none) -- token recvTok-1 in the record.
std::string PdEditorView::gui_send_symbol( const PdObj& o ) const
{
    GuiInfo g = gui_info( o.text );
    if ( g.recvTok < 2 ) return std::string();
    std::vector<std::string> tk = tokenize( o.text );
    const int i = g.recvTok - 1;
    if ( i < 1 || i >= (int) tk.size() ) return std::string();
    return tk[(size_t)i] == "empty" ? std::string() : tk[(size_t)i];
}

std::vector<std::string> PdEditorView::gui_send_symbols() const
{
    std::vector<std::string> v;
    for ( const PdObj& o : objs_ )
    {
        if ( o.kind != 'o' ) continue;
        std::string s = gui_send_symbol( o );
        if ( !s.empty() ) v.push_back( s );
    }
    return v;
}

void PdEditorView::apply_gui_feedback( const std::string& sendSym, bool bang, float val )
{
    if ( sendSym.empty() ) return;
    const unsigned now = SDL_GetTicks();
    for ( PdObj& o : objs_ )
    {
        if ( o.kind != 'o' || gui_send_symbol( o ) != sendSym ) continue;
        GuiInfo g = gui_info( o.text );
        if ( g.type == GuiInfo::None ) continue;
        if ( g.type == GuiInfo::Bng || bang )
            { o.gui_val = 1.0; o.gui_val_set = true; o.flash_until = now + 150; }  // flash
        else
        {
            double v = val;
            if ( g.isSlider() && g.max > g.min ) v = ( val - g.min ) / ( g.max - g.min );
            o.gui_val = v; o.gui_val_set = true;
        }
        // no break: a send symbol may be shared by several widgets
    }
}

// Push a GUI atom's live value into the RUNNING patch (via its receive symbol) so
// downstream objects react immediately -- e.g. a toggle starts a connected [metro].
void PdEditorView::live_send_gui( ui::App& /*app*/, int oi )
{
    if ( oi < 0 || oi >= (int)objs_.size() ) return;
    const PdObj& o = objs_[oi];
    GuiInfo g = gui_info( o.text );
    if ( g.type == GuiInfo::None ) return;
    const std::string recv = gui_recv_symbol( o );
    if ( recv.empty() ) return;
    if ( g.type == GuiInfo::Bng ) { if ( on_gui_bang ) on_gui_bang( recv ); return; }
    const double cur = o.gui_val_set ? o.gui_val : g.val;
    // sliders store 0..1 -> map to the widget's value range; others store the value.
    const float v = g.isSlider() ? (float)( g.min + cur * ( g.max - g.min ) ) : (float)cur;
    if ( on_gui_send ) on_gui_send( recv, v );
}

// Number-box keyboard entry: type a value into an nbx, Enter commits / Esc cancels.
// Reuses the app's single-field text editor (the same path object-text edits use).
void PdEditorView::begin_num_edit( ui::App& app, int oi )
{
    if ( oi < 0 || oi >= (int)objs_.size() ) return;
    if ( app.editing_text() ) return;                 // never nest text fields
    m_num_edit = oi;
    m_num_buf.clear();                                // start empty; typing sets the value
    app.begin_text( &m_num_buf,
                    [&app]{ app.request_redraw(); },
                    [this,&app]( bool ok ){ finalize_num_edit( app, ok ); } );
    app.request_redraw();
}

void PdEditorView::finalize_num_edit( ui::App& app, bool commit )
{
    const int oi = m_num_edit; m_num_edit = -1;
    if ( commit && oi >= 0 && oi < (int)objs_.size() && !m_num_buf.empty() )
    {
        double v = std::atof( m_num_buf.c_str() );
        GuiInfo g = gui_info( objs_[oi].text );
        if ( g.min > -1e36 && v < g.min ) v = g.min;
        if ( g.max <  1e36 && v > g.max ) v = g.max;
        objs_[oi].gui_val = v; objs_[oi].gui_val_set = true;
        live_send_gui( app, oi );                     // push the typed value live
        flush_gui_value( oi );
        if ( on_store_text ) on_store_text( patch_text() );
    }
    m_num_buf.clear();
    app.request_redraw();
}

// Nearest wire to (mx,my) within a small pick tolerance, or -1.  Point-to-segment
// distance against each committed connection's drawn line (screen coords).
int PdEditorView::wire_at( ui::App& app, int mx, int my ) const
{
    int best = -1; double bestD2 = 36.0;                       // (6 px)^2 tolerance
    for ( int i = 0; i < (int)conns_.size(); ++i )
    {
        int x1, y1, x2, y2;
        if ( !outlet_pos( app, conns_[i].from, conns_[i].outlet, x1, y1 ) ) continue;
        if ( !inlet_pos ( app, conns_[i].to,   conns_[i].inlet,  x2, y2 ) ) continue;
        const double dx = x2 - x1, dy = y2 - y1, len2 = dx*dx + dy*dy;
        double t = ( len2 > 0.0 ) ? ( ( mx - x1 ) * dx + ( my - y1 ) * dy ) / len2 : 0.0;
        if ( t < 0.0 ) t = 0.0; else if ( t > 1.0 ) t = 1.0;
        const double ex = mx - ( x1 + t*dx ), ey = my - ( y1 + t*dy );
        const double d2 = ex*ex + ey*ey;
        if ( d2 < bestD2 ) { bestD2 = d2; best = i; }
    }
    return best;
}

bool PdEditorView::press_left( ui::App& app, int mx, int my )
{
    m_move = false; m_wire = false; m_gui_drag = false; m_sel_wire = -1;  // fresh gesture
    m_press_x = mx; m_press_y = my;

    // A pending number-box entry commits on the next click anywhere (click-away).
    if ( m_num_edit >= 0 ) { if ( app.editing_text() ) app.end_text(); finalize_num_edit( app, true ); }

    // RUN mode: click/drag drives GUI atoms only.  No wiring, moving, or editing.
    // The value is pushed LIVE into the running patch (via the atom's receive symbol)
    // AND persisted to the .pd -- but WITHOUT a reload, so a toggle starts a connected
    // [metro] and keeps its timing (a reload would restart the whole patch).
    if ( !m_edit_mode )
    {
        int ri = obj_at( app, mx, my );
        m_sel = ri;
        if ( ri >= 0 && gui_interact( app, ri, mx, my, true ) )
        {
            GuiInfo g = gui_info( objs_[ri].text );
            const bool draggable = ( g.type == GuiInfo::Hsl || g.type == GuiInfo::Vsl ||
                                     g.type == GuiInfo::Knob || g.type == GuiInfo::Nbx ||
                                     g.type == GuiInfo::Hradio || g.type == GuiInfo::Vradio );
            live_send_gui( app, ri );                        // drive the running patch
            if ( draggable ) { m_gui_drag = true; m_gui_obj = ri; }
            else { flush_gui_value( ri );                    // tgl/bng: persist, no reload
                   if ( on_store_text ) on_store_text( patch_text() ); }
        }
        app.request_redraw();
        return true;
    }

    // (1) an OUTLET -> arm it (lit) for click-to-connect AND start a drag-wire.
    int oi = -1, port = -1;
    if ( outlet_at( app, mx, my, oi, port ) )
    {
        m_link_from = oi;  m_link_outlet = port;       // armed source outlet
        m_wire = true;  m_wire_from = oi;  m_wire_outlet = port;
        m_wire_x = mx;  m_wire_y = my;     m_wire_ok = false;
        m_sel = oi;
        app.request_redraw();
        return true;
    }
    // (1b) an INLET -> if an outlet is armed, complete the wire here (connect on
    //      press: robust, no dependence on release timing / click jitter).
    {
        int ti = -1, in = -1;
        if ( inlet_at( app, mx, my, ti, in ) )
        {
            if ( m_link_from >= 0 && ti != m_link_from &&
                 add_conn( m_link_from, m_link_outlet, ti, in ) )
                commit( app );
            m_link_from = -1;                          // consume the arm
            m_sel = ti;
            app.request_redraw();
            return true;
        }
    }

    // (2) an object body -> double-click edits, single click moves
    oi = obj_at( app, mx, my );
    if ( oi >= 0 )
    {
        Uint32 now = SDL_GetTicks();
        if ( oi == m_last_click_obj && ( now - m_last_click_ms ) < (Uint32) DBLCLK_MS
             && objs_[oi].kind != 'x' )                 // 'x' (subpatch/array) isn't text-editable
        {
            m_last_click_obj = -1;
            begin_obj_edit( app, oi );
            return true;
        }
        m_last_click_obj = oi;
        m_last_click_ms  = now;
        m_sel = oi;

        // EDIT mode: a single click MOVES the object (GUI atoms only respond to
        // value drags in RUN mode); double-click (above) edits its text.
        m_move = true;  m_move_obj = oi;  m_moved = false;
        int sx, sy; obj_screen( objs_[oi], sx, sy );
        m_move_dx = mx - sx;
        m_move_dy = my - sy;
        app.request_redraw();
        return true;
    }

    // (2.5) a WIRE -> select it so Delete/Backspace removes it.  Checked after
    //       boxes + ports, so clicking an object or connector still wins.
    int wi = wire_at( app, mx, my );
    if ( wi >= 0 )
    {
        m_sel_wire = wi;  m_sel = -1;  m_link_from = -1;
        app.request_redraw();
        return true;
    }

    // (3) empty canvas -> deselect + cancel any armed outlet (right-click here
    //     opens the object picker)
    m_sel = -1;
    m_link_from = -1;
    app.request_redraw();
    return true;
}

bool PdEditorView::drag_left( ui::App& app, int mx, int my )
{
    if ( m_gui_drag )                    // dragging a slider / knob / nbx / radio
    {
        gui_interact( app, m_gui_obj, mx, my, false );
        live_send_gui( app, m_gui_obj ); // drive the running patch as you drag
        return true;
    }
    if ( m_wire )
    {
        m_wire_x = mx; m_wire_y = my;
        int ti = -1, in = -1;
        m_wire_ok = inlet_at( app, mx, my, ti, in ) && ti != m_wire_from;
        if ( m_wire_ok ) { m_wire_to = ti; m_wire_inlet = in; }
        app.request_redraw();
        return true;
    }
    if ( m_move && m_move_obj >= 0 && m_move_obj < (int) objs_.size() )
    {
        PdObj& o = objs_[m_move_obj];
        o.x = (int) std::lround( ( mx - m_move_dx - rect.x - m_ox ) / m_zoom );
        o.y = (int) std::lround( ( my - m_move_dy - rect.y - m_oy ) / m_zoom );
        m_moved = true;
        app.request_redraw();
        return true;
    }
    return true;
}

bool PdEditorView::release_left( ui::App& app, int mx, int my )
{
    if ( m_gui_drag )                    // finished a widget drag -> persist (no reload)
    {
        const int oi = m_gui_obj;
        m_gui_drag = false; m_gui_obj = -1;
        // a CLICK (no drag) on a number box -> type a value instead of nudging it
        const int md = std::abs( mx - m_press_x ) + std::abs( my - m_press_y );
        if ( md <= 3 && oi >= 0 && oi < (int)objs_.size() &&
             gui_info( objs_[oi].text ).type == GuiInfo::Nbx )
        {
            begin_num_edit( app, oi );
            return true;
        }
        live_send_gui( app, oi );        // final value to the running patch
        flush_gui_value( oi );           // write the live value into the model once
        if ( on_store_text ) on_store_text( patch_text() );   // persist, no reload
        return true;
    }
    if ( m_wire )
    {
        m_wire = false;
        int ti = -1, in = -1;
        const int dx = mx - m_press_x, dy = my - m_press_y;
        const int moved = ( dx < 0 ? -dx : dx ) + ( dy < 0 ? -dy : dy );
        if ( inlet_at( app, mx, my, ti, in ) &&
             add_conn( m_wire_from, m_wire_outlet, ti, in ) )
        {
            commit( app );                       // drag completed onto an inlet
            m_link_from = -1;                    // arm consumed
        }
        else if ( moved > 4 )
        {
            m_link_from = -1;                    // dragged to empty: cancel arm
            app.request_redraw();
        }
        else
        {
            // a CLICK on the outlet (no drag) -> leave it ARMED (lit) for the
            // next inlet click to complete the wire.
            app.request_redraw();
        }
        return true;
    }
    if ( m_move )
    {
        m_move = false;
        if ( m_moved ) { m_moved = false; commit( app ); }
        else             app.request_redraw();
        return true;
    }
    return false;
}

bool PdEditorView::on_wheel( ui::App& app, int dx, int dy )
{
    if ( m_mode == Mode_Pick )                         // scroll the list
    {
        m_pick_scroll -= dy;
        int maxscroll = (int) build_pick_list( filtered() ).size() - PICK_LIST_ROWS;
        if ( maxscroll < 0 ) maxscroll = 0;
        if ( m_pick_scroll > maxscroll ) m_pick_scroll = maxscroll;
        if ( m_pick_scroll < 0 )         m_pick_scroll = 0;
        app.request_redraw();
        return true;
    }

    // Plain vertical wheel -> ZOOM (anchored at the cursor).  Shift/Ctrl (or a
    // horizontal wheel) -> PAN.  m_zoom is per-view, so each window zooms alone.
    SDL_Keymod mod = SDL_GetModState();
    if ( dx != 0 ) { m_ox += dx * PAN_STEP; app.request_redraw(); return true; }
    if ( mod & ( KMOD_SHIFT | KMOD_CTRL ) )
    {
        if ( mod & KMOD_SHIFT ) m_ox += dy * PAN_STEP;
        else                    m_oy += dy * PAN_STEP;
        app.request_redraw();
        return true;
    }
    if ( dy != 0 )
    {
        int mxg = 0, myg = 0; ui::mouse_logical( app, mxg, myg );   // logical px
        float wx = ( mxg - rect.x - m_ox ) / m_zoom;   // world point under cursor
        float wy = ( myg - rect.y - m_oy ) / m_zoom;
        float nz = m_zoom * ( dy > 0 ? 1.15f : 1.0f / 1.15f );
        nz = nz < 0.5f ? 0.5f : ( nz > 2.5f ? 2.5f : nz );
        m_zoom = nz;
        m_ox = (int) std::lround( mxg - rect.x - wx * m_zoom );
        m_oy = (int) std::lround( myg - rect.y - wy * m_zoom );
    }
    app.request_redraw();
    return true;
}

bool PdEditorView::on_key( ui::App& app, SDL_Keycode k )
{
    // Properties dialog open (and no field being typed): Escape closes it.
    if ( m_mode == Mode_Props )
    {
        if ( k == SDLK_ESCAPE ) close_props( app );
        return true;
    }
    // Ctrl+E toggles EDIT <-> RUN mode (like Pd) -- only when not mid text-entry.
    if ( k == SDLK_e && ( SDL_GetModState() & KMOD_CTRL ) && m_mode == Mode_None )
    {
        m_edit_mode = !m_edit_mode;
        m_gui_drag = false; m_move = false; m_wire = false; m_link_from = -1;
        app.request_redraw();
        return true;
    }
    if ( m_mode != Mode_None ) return false;           // text editing handles its own keys
    if ( ( k == SDLK_DELETE || k == SDLK_BACKSPACE ) && m_edit_mode )
    {
        // a selected WIRE deletes first (so you can cut a cord without the object);
        // otherwise the selected object (and every wire touching it) goes.
        if ( m_sel_wire >= 0 && m_sel_wire < (int) conns_.size() )
        {
            conns_.erase( conns_.begin() + m_sel_wire );
            m_sel_wire = -1;
            commit( app );
            return true;
        }
        if ( m_sel >= 0 )
        {
            delete_obj( app, m_sel );
            return true;
        }
    }
    return false;
}

// ============================================================================
//  Inline object-text edit
// ============================================================================
void PdEditorView::begin_obj_edit( ui::App& app, int idx )
{
    if ( idx < 0 || idx >= (int) objs_.size() ) return;
    m_mode = Mode_Edit;
    m_edit_obj = idx;
    m_sel = idx;
    m_move = false; m_wire = false;

    m_edit_ptr = &objs_[idx].text;
    m_edit_app = &app;
    app.begin_text( m_edit_ptr,
                    [&app]() { app.request_redraw(); },
                    [this, &app]( bool /*ok*/ ) { finalize_obj_edit( app ); } );
    app.request_redraw();
}

void PdEditorView::finalize_obj_edit( ui::App& app )
{
    if ( m_mode != Mode_Edit ) return;
    if ( app.editing_text() ) app.end_text();          // click-away commit

    int i = m_edit_obj;
    m_mode = Mode_None;
    m_edit_obj = -1;
    m_edit_ptr = nullptr;
    if ( i >= 0 && i < (int) objs_.size() )
        io_for( objs_[i].text, objs_[i].nin, objs_[i].nout );

    commit( app );                                     // save + on_changed + redraw
}

// ============================================================================
//  Module picker interaction
// ============================================================================
void PdEditorView::open_picker( ui::App& app, int mx, int my )
{
    m_mode = Mode_Pick;
    m_search.clear();
    m_pick_scroll = 0;
    m_pick_x = mx;  m_pick_y = my;                     // screen anchor
    m_pick_cx = (int) std::lround( ( mx - rect.x - m_ox ) / m_zoom );  // Pd coords
    m_pick_cy = (int) std::lround( ( my - rect.y - m_oy ) / m_zoom );

    app.begin_text( &m_search,
                    [&app]() { app.request_redraw(); },
                    [this, &app]( bool ok ) { picker_commit( app, ok ); } );
    app.request_redraw();
}

void PdEditorView::close_picker( ui::App& app )
{
    if ( app.editing_text() ) app.end_text();
    m_mode = Mode_None;
    m_search.clear();
    m_pick_scroll = 0;
    app.request_redraw();
}

// Enter (ok=true) / Esc (ok=false).  The run loop already ended text input.
void PdEditorView::picker_commit( ui::App& app, bool ok )
{
    std::string name = m_search;
    m_mode = Mode_None;
    m_search.clear();
    m_pick_scroll = 0;

    if ( ok && !name.empty() )
        create_object( app, name, m_pick_cx, m_pick_cy );   // any Pd object, verbatim
    else
        app.request_redraw();
}

bool PdEditorView::picker_mouse( ui::App& app, const ui::MouseEv& e, bool downEdge )
{
    SDL_Rect p = picker_rect( app );

    // (0) dragging a card OUT of the palette -> ghost tracks the cursor; on release
    //     over the canvas, create the object there (drag-and-drop).
    if ( m_pick_drag )
    {
        m_pick_drag_x = e.x; m_pick_drag_y = e.y;
        if ( !e.pressed )
        {
            m_pick_drag = false;
            const bool onCanvas = !in_rect( p, e.x, e.y );
            std::string name = m_pick_drag_name;
            if ( onCanvas )
            {
                const int px = (int) std::lround( ( e.x - rect.x - m_ox ) / m_zoom );
                const int py = (int) std::lround( ( e.y - rect.y - m_oy ) / m_zoom );
                close_picker( app );                   // end_text + Mode_None
                create_object( app, name, px, py );    // drops into edit mode
                return true;
            }
        }
        app.request_redraw();
        return true;
    }

    if ( !e.pressed ) return true;                     // swallow other releases
    if ( e.button == SDL_BUTTON_RIGHT ) { close_picker( app ); return true; }
    if ( e.button != SDL_BUTTON_LEFT )  return true;
    if ( !downEdge )                    return true;   // ignore drag-motion re-entry

    if ( !in_rect( p, e.x, e.y ) ) { close_picker( app ); return true; }

    // (1) press on a card -> pick it up (start a drag).  The search box (top row)
    //     stays a text field.
    const int rowh = app.mono.ch() + 6;
    // category buttons (5x2): click one to filter the palette to that group.
    const int btnW = ( p.w - 4 ) / CAT_COLS;
    for ( int i = 0; i < N_PICK_CATS; ++i )
    {
        const int row = i / CAT_COLS, col = i % CAT_COLS;
        SDL_Rect b { p.x + 2 + col * btnW, p.y + 2 + rowh + 2 + row * rowh, btnW - 1, rowh - 1 };
        if ( in_rect( b, e.x, e.y ) ) { m_pick_cat = i; m_pick_scroll = 0;
                                        app.request_redraw(); return true; }
    }
    const int listTop = p.y + 2 + rowh + 2 + CAT_ROWS * rowh + 2;
    if ( e.y >= listTop )
    {
        std::vector<PickItem> items = build_pick_list( filtered() );
        const int row = ( e.y - listTop ) / rowh;
        const int ii  = m_pick_scroll + row;
        if ( row >= 0 && row < PICK_LIST_ROWS && ii >= 0 && ii < (int) items.size()
             && !items[ii].header )                   // pick up a draggable object (not a header)
        {
            m_pick_drag = true;  m_pick_drag_name = items[ii].text;
            m_pick_drag_x = e.x; m_pick_drag_y = e.y;
            app.request_redraw();
            return true;
        }
    }
    // click on the search box / empty area: keep the palette open for typing
    app.request_redraw();
    return true;
}

} // namespace pdui
