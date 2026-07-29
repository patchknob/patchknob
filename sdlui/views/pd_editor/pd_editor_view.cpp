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

// Searchable palette of ~60 common Pd objects (names only) for the picker.
static const char* const OBJECTS[] = {
    "osc~","phasor~","*~","+~","-~","/~","dac~","adc~","sig~","noise~",
    "line~","vline~","lop~","hip~","bp~","vcf~","clip~","tabosc~","tabread~","tabwrite~",
    "delread~","delwrite~","send~","receive~","throw~","catch~",
    "metro","delay","line","pack","unpack","trigger","sel","moses","spigot","route",
    "change","float","int","symbol","list","mtof","ftom","dbtorms","rmstodb","random",
    "notein","ctlin","bendin","pgmin","noteout","ctlout","print","bng","tgl","nbx",
    "hsl","vsl","loadbang","r","s","send","receive","+","-","*","/","mod","pow","expr","expr~"
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

    struct Entry { const char* key; int nin; int nout; };
    static const Entry TAB[] = {
        { "osc~",2,1 }, { "phasor~",2,1 }, { "*~",2,1 }, { "+~",2,1 }, { "-~",2,1 },
        { "/~",2,1 }, { "dac~",2,0 }, { "adc~",0,2 }, { "line~",2,1 }, { "vline~",3,1 },
        { "sig~",1,1 }, { "noise~",0,1 }, { "lop~",2,1 }, { "hip~",2,1 }, { "bp~",3,1 },
        { "clip~",3,1 }, { "metro",2,1 }, { "delay",2,1 }, { "del",2,1 },
        { "tgl",1,1 }, { "bng",1,1 }, { "notein",0,3 }, { "ctlin",0,3 },
        { "mtof",1,1 }, { "ftom",1,1 },
        { "+",2,1 }, { "-",2,1 }, { "*",2,1 }, { "/",2,1 }, { "mod",2,1 },
        { "moses",2,2 }, { "sel",1,2 }, { "pack",2,2 }, { "unpack",1,2 },
        { "trigger",1,2 }, { "t",1,2 },
        { "float",2,1 }, { "f",2,1 }, { "int",2,1 }, { "i",2,1 },
        { "print",1,0 }, { "r",0,1 }, { "receive",0,1 }, { "s",1,0 }, { "send",1,0 },
    };
    for ( std::size_t i = 0; i < sizeof(TAB)/sizeof(TAB[0]); ++i )
        if ( w == TAB[i].key ) { nin = TAB[i].nin; nout = TAB[i].nout; return; }

    // adc~ / dac~ with explicit channel args: one inlet/outlet per channel.
    if ( w == "adc~" || w == "dac~" )
    {
        std::vector<std::string> tk = tokenize( text );
        int chans = (int)tk.size() - 1;            // args after the object name
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
    if      ( k == "tgl" )    { g.type = GuiInfo::Tgl;  g.w = g.h = (int)num(1,15); g.valTok = 13; g.val = num(g.valTok,0); }
    else if ( k == "bng" )    { g.type = GuiInfo::Bng;  g.w = g.h = (int)num(1,15); }
    else if ( k == "hsl" )    { g.type = GuiInfo::Hsl;  g.w=(int)num(1,128); g.h=(int)num(2,15);
                                g.min=num(3,0); g.max=num(4,127); g.valTok=17; }
    else if ( k == "vsl" )    { g.type = GuiInfo::Vsl;  g.w=(int)num(1,15);  g.h=(int)num(2,128);
                                g.min=num(3,0); g.max=num(4,127); g.valTok=17; }
    else if ( k == "nbx" )    { g.type = GuiInfo::Nbx;  g.w=(int)num(1,5)*8; g.h=(int)num(2,14);
                                g.min=num(3,-1e37); g.max=num(4,1e37); g.valTok=17; g.val=num(g.valTok,0); }
    else if ( k == "hradio" ) { g.type = GuiInfo::Hradio; g.cells=(int)num(4,8); g.w=(int)num(1,15)*g.cells; g.h=(int)num(1,15); g.valTok=15; g.val=num(g.valTok,0); }
    else if ( k == "vradio" ) { g.type = GuiInfo::Vradio; g.cells=(int)num(4,8); g.w=(int)num(1,15); g.h=(int)num(1,15)*g.cells; g.valTok=15; g.val=num(g.valTok,0); }
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
    objs_.clear();
    conns_.clear();
    m_canvas_w = 600;
    m_canvas_h = 400;

    if ( m_path.empty() ) return;

    std::ifstream in( m_path.c_str(), std::ios::binary );
    if ( !in ) return;                         // missing file -> stay empty
    std::string content( (std::istreambuf_iterator<char>( in )),
                          std::istreambuf_iterator<char>() );

    bool seen_canvas = false;
    std::vector<std::string> recs = split_records( content );
    for ( std::size_t ri = 0; ri < recs.size(); ++ri )
    {
        std::vector<std::string> tk = tokenize( recs[ri] );
        if ( tk.size() < 2 ) continue;

        if ( tk[0] == "#N" && tk[1] == "canvas" )
        {
            if ( !seen_canvas )                // first canvas = our patch header
            {
                if ( tk.size() > 4 ) m_canvas_w = safe_atoi( tk[4] );
                if ( tk.size() > 5 ) m_canvas_h = safe_atoi( tk[5] );
                seen_canvas = true;
            }
            continue;                          // ignore nested subpatch canvases
        }

        if ( tk[0] != "#X" ) continue;
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
        // everything else (arrays, coords, restore, ...) is ignored
    }

    sanitize_conns();
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

bool PdEditorView::save()
{
    if ( m_path.empty() ) return false;

    std::ofstream out( m_path.c_str(), std::ios::binary | std::ios::trunc );
    if ( !out ) return false;

    out << "#N canvas 0 0 " << m_canvas_w << ' ' << m_canvas_h << " 12;\n";

    for ( std::size_t i = 0; i < objs_.size(); ++i )
    {
        const PdObj& o = objs_[i];
        const char* rec = ( o.kind == 'm' ) ? "msg"
                        : ( o.kind == 'f' ) ? "floatatom"
                        : ( o.kind == 't' ) ? "text" : "obj";
        out << "#X " << rec << ' ' << o.x << ' ' << o.y;
        if ( !o.text.empty() ) out << ' ' << o.text;
        out << ";\n";
    }
    for ( std::size_t i = 0; i < conns_.size(); ++i )
    {
        const PdConn& c = conns_[i];
        out << "#X connect " << c.from << ' ' << c.outlet << ' '
            << c.to << ' ' << c.inlet << ";\n";
    }
    return out.good();
}

void PdEditorView::commit( ui::App& app )
{
    bool ok = save();
    if ( ok && on_changed ) on_changed();
    app.request_redraw();
}

void PdEditorView::reset_interaction()
{
    m_move = false; m_move_obj = -1; m_moved = false;
    m_wire = false; m_wire_from = -1; m_wire_to = -1; m_wire_ok = false;
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
    io_for( o.text, o.nin, o.nout );
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

    // Let a bang's flash decay: after its window elapses, drop it back to unlit.
    if ( m_bng_obj >= 0 )
    {
        if ( SDL_GetTicks() >= m_bng_flash_ms )
        {
            if ( m_bng_obj < (int) objs_.size() ) objs_[m_bng_obj].gui_val = 0.0;
            m_bng_obj = -1;
        }
        else app.request_redraw();     // keep animating until the flash ends
    }

    ui::fill_rect( r, rect, t.bg );
    ui::frame_rect( r, rect, t.dim );

    SDL_Rect clip = rect;
    SDL_RenderSetClipRect( r, &clip );

    // committed wires (behind the boxes)
    ui::set_color( r, t.dim );
    for ( std::size_t i = 0; i < conns_.size(); ++i )
    {
        int x1, y1, x2, y2;
        if ( outlet_pos( app, conns_[i].from, conns_[i].outlet, x1, y1 ) &&
             inlet_pos ( app, conns_[i].to,   conns_[i].inlet,  x2, y2 ) )
            SDL_RenderDrawLine( r, x1, y1, x2, y2 );
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

    SDL_RenderSetClipRect( r, nullptr );

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

    if ( m_mode == Mode_Pick )
        draw_picker( app );
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

void PdEditorView::draw_obj( ui::App& app, int i )
{
    if ( i < 0 || i >= (int) objs_.size() ) return;
    const PdObj& o = objs_[i];
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const ui::Font& f = app.mono;

    int sx, sy; obj_screen( o, sx, sy );
    int w  = box_w( f, o );
    int bh = box_h( f, o );
    SDL_Rect box { sx, sy, w, bh };

    Color frame = ( i == m_sel ) ? t.accent : t.dim;
    ui::fill_rect( r, box, t.panel );
    ui::frame_rect( r, box, frame );

    GuiInfo g = ( o.kind == 'o' ) ? gui_info( o.text ) : GuiInfo{};
    if ( o.bp_w > 0 )
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
                ui::set_color( r, t.text );
                const int tri = std::max( 3, (int)std::lround( 4 * m_zoom ) );
                SDL_RenderDrawLine( r, box.x, box.y+2, box.x+tri, box.y+box.h/2 );   // corner triangle
                SDL_RenderDrawLine( r, box.x, box.y+box.h-2, box.x+tri, box.y+box.h/2 );
                char v[24]; std::snprintf( v, sizeof(v), "%g", dv );
                f.draw_fitted( r, SDL_Rect{ box.x+tri+2, box.y, std::max(1,box.w-tri-4), box.h },
                               v, t.text, false, std::max( 1.0f, m_zoom ) );
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
            app.request_redraw();                          // keep the caret blinking
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
std::vector<std::string> PdEditorView::filtered() const
{
    std::vector<std::string> out;
    for ( int i = 0; i < N_OBJECTS; ++i )
        if ( ci_contains( OBJECTS[i], m_search ) )
            out.push_back( OBJECTS[i] );
    return out;
}

// Grid metrics for the graphical card palette (2 columns of draggable cards).
static const int PICK_COLS  = 2;
static const int PICK_CARDH = 46;
static const int PICK_GRIDR = 6;    // visible card rows

SDL_Rect PdEditorView::picker_rect( ui::App& app ) const
{
    const int rowh = app.mono.ch() + 6;
    const int w = PICK_W + 44;                                  // wider for cards
    const int h = rowh + 4 + PICK_GRIDR * ( PICK_CARDH + 4 ) + 4;
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

    // card grid -- each card is a draggable preview of an object
    std::vector<std::string> fl = filtered();
    const int cardW  = ( p.w - 4 - ( PICK_COLS + 1 ) * 4 ) / PICK_COLS;
    const int gridTop = p.y + 2 + rowh + 4;
    const int totalRows = ( (int)fl.size() + PICK_COLS - 1 ) / PICK_COLS;
    int maxscroll = totalRows - PICK_GRIDR; if ( maxscroll < 0 ) maxscroll = 0;
    if ( m_pick_scroll > maxscroll ) m_pick_scroll = maxscroll;
    if ( m_pick_scroll < 0 ) m_pick_scroll = 0;

    int mx, my; SDL_GetMouseState( &mx, &my );
    for ( int row = 0; row < PICK_GRIDR; ++row )
        for ( int col = 0; col < PICK_COLS; ++col )
        {
            const int fi = ( m_pick_scroll + row ) * PICK_COLS + col;
            if ( fi >= (int) fl.size() ) continue;
            SDL_Rect card { p.x + 4 + col * ( cardW + 4 ), gridTop + row * ( PICK_CARDH + 4 ),
                            cardW, PICK_CARDH };
            const bool hover = in_rect( card, mx, my ) && !m_pick_drag;
            ui::fill_rect( r, card, hover ? t.sel : t.bg );
            ui::frame_rect( r, card, hover ? t.accent : t.dim );
            draw_obj_preview( app, fl[fi], card );
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
        if ( oi >= 0 ) delete_obj( app, oi );          // fires commit()
        else           open_picker( app, e.x, e.y );   // empty -> add object
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
            if ( press ) { o.gui_val = 1.0; m_bng_obj = oi;        // momentary flash
                           m_bng_flash_ms = SDL_GetTicks() + 150; }
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

bool PdEditorView::press_left( ui::App& app, int mx, int my )
{
    m_move = false; m_wire = false; m_gui_drag = false;   // fresh gesture: clean slate
    m_press_x = mx; m_press_y = my;

    // RUN mode: click/drag drives GUI atoms only.  No wiring, moving, or editing.
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
            if ( draggable ) { m_gui_drag = true; m_gui_obj = ri; }
            else { flush_gui_value( ri ); commit( app ); }   // tgl/bng: apply on the click
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
        if ( oi == m_last_click_obj && ( now - m_last_click_ms ) < (Uint32) DBLCLK_MS )
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
    if ( m_gui_drag )                    // finished a widget drag -> persist + reload
    {
        const int oi = m_gui_obj;
        m_gui_drag = false; m_gui_obj = -1;
        flush_gui_value( oi );           // write the live value into the .pd once
        commit( app );
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
        int maxscroll = (int) filtered().size() - PICK_ROWS; if ( maxscroll < 0 ) maxscroll = 0;
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
        int mxg = 0, myg = 0; SDL_GetMouseState( &mxg, &myg );
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
    // Ctrl+E toggles EDIT <-> RUN mode (like Pd) -- only when not mid text-entry.
    if ( k == SDLK_e && ( SDL_GetModState() & KMOD_CTRL ) && m_mode == Mode_None )
    {
        m_edit_mode = !m_edit_mode;
        m_gui_drag = false; m_move = false; m_wire = false; m_link_from = -1;
        app.request_redraw();
        return true;
    }
    if ( m_mode != Mode_None ) return false;           // text editing handles its own keys
    if ( ( k == SDLK_DELETE || k == SDLK_BACKSPACE ) && m_sel >= 0 && m_edit_mode )
    {
        delete_obj( app, m_sel );
        return true;
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

    app.begin_text( &objs_[idx].text,
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
    const int cardW  = ( p.w - 4 - ( PICK_COLS + 1 ) * 4 ) / PICK_COLS;
    const int gridTop = p.y + 2 + rowh + 4;
    if ( e.y >= gridTop )
    {
        std::vector<std::string> fl = filtered();
        const int row = ( e.y - gridTop ) / ( PICK_CARDH + 4 );
        const int col = ( e.x - ( p.x + 4 ) ) / std::max( 1, cardW + 4 );
        if ( row >= 0 && row < PICK_GRIDR && col >= 0 && col < PICK_COLS )
        {
            const int fi = ( m_pick_scroll + row ) * PICK_COLS + col;
            if ( fi >= 0 && fi < (int) fl.size() )
            {
                m_pick_drag = true;  m_pick_drag_name = fl[fi];
                m_pick_drag_x = e.x; m_pick_drag_y = e.y;
                app.request_redraw();
                return true;
            }
        }
    }
    // click on the search box / empty area: keep the palette open for typing
    app.request_redraw();
    return true;
}

} // namespace pdui
