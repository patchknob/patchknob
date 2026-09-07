#include "sample_browser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#ifdef _WIN32
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace sampleslot {

//============================================================================
//  skin::Popup
//============================================================================
namespace skin {

int Popup::m_first_enabled() const {
    for ( size_t i = 0; i < rows.size(); ++i )
        if ( !rows[i].separator && rows[i].enabled ) return (int)i;
    return -1;
}

void Popup::m_step( int dir ) {
    const int n = (int)rows.size();
    if ( n <= 0 ) { m_sel = -1; return; }
    int i = m_sel;
    for ( int guard = 0; guard < n; ++guard ) {
        i += dir;
        if ( i < 0 )  i = n - 1;
        if ( i >= n ) i = 0;
        if ( !rows[(size_t)i].separator && rows[(size_t)i].enabled ) { m_sel = i; return; }
    }
    m_sel = -1;
}

void Popup::layout( const ui::Font& f, const SDL_Rect& bounds ) {
    m_rects.assign( rows.size(), SDL_Rect{ 0, 0, 0, 0 } );
    // Zero the box on the way out too: a stale one from the last time the menu
    // was up would otherwise still swallow clicks and still paint a frame.
    if ( !m_open || rows.empty() || bounds.w <= 0 || bounds.h <= 0 ) {
        m_box = SDL_Rect{ 0, 0, 0, 0 };
        return;
    }

    const int p    = pad( f );
    const int rowH = f.ch() + p + 1;
    const int sepH = std::max( 3, p );
    const int tick = f.cw() * 2;                 // check gutter

    int colW = 0, totalH = 2 * p;
    bool anyCheck = false;
    for ( const MenuRow& m : rows ) {
        if ( m.separator ) { totalH += sepH; continue; }
        colW = std::max( colW, f.text_w( m.label ) );
        anyCheck = anyCheck || m.check;
        totalH += rowH;
    }
    colW += 4 * p + ( anyCheck ? tick : 0 );

    // Wrap into columns only as far as the HOST rect allows.  The old menu
    // computed the column count from the height alone, so a tall menu in a
    // narrow window drew columns off the right-hand edge: those rows were
    // painted over the neighbouring pane and could never be clicked.
    const int maxH = std::max( rowH * 4, bounds.h - 2 * p );
    int cols = std::max( 1, ( totalH + maxH - 1 ) / maxH );
    if ( colW * cols > bounds.w )
        colW = std::max( f.cw() * 6, bounds.w / cols );

    const int w = colW * cols;
    const int h = std::min( maxH, totalH );

    int x = m_x, y = m_y;
    if ( x + w > bounds.x + bounds.w ) x = bounds.x + bounds.w - w;
    if ( y + h > bounds.y + bounds.h ) y = bounds.y + bounds.h - h;
    x = std::max( x, bounds.x ); y = std::max( y, bounds.y );
    m_box = SDL_Rect{ x, y, w, h };

    int cx = x, cy = y + p;
    for ( size_t i = 0; i < rows.size(); ++i ) {
        const int need = rows[i].separator ? sepH : rowH;
        if ( cy + need > y + h && cy > y + p ) { cx += colW; cy = y + p; }
        m_rects[i] = rows[i].separator
                   ? SDL_Rect{ cx, cy, colW, sepH }
                   : SDL_Rect{ cx + 1, cy, colW - 2, rowH };
        cy += need;
    }
}

void Popup::draw( ui::App& app, const ui::Font& f ) {
    if ( !m_open || m_box.w <= 0 ) return;
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = ui::theme();
    const int p = pad( f );

    // A drop shadow is the only cue that says "this floats above the view".
    SDL_Rect sh{ m_box.x + 2, m_box.y + 3, m_box.w, m_box.h };
    fill_round( r, sh, p, ui::Color{ 0, 0, 0, 70 } );
    fill_round( r, m_box, p, t.panel );
    frame_round( r, m_box, p, t.accent );

    int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
    const int tick = f.cw() * 2;
    bool anyCheck = false;
    for ( const MenuRow& m : rows ) anyCheck = anyCheck || m.check;

    for ( size_t i = 0; i < rows.size() && i < m_rects.size(); ++i ) {
        const MenuRow& m = rows[i];
        const SDL_Rect& rr = m_rects[i];
        if ( rr.w <= 0 ) continue;
        if ( m.separator ) {
            ui::hline( r, rr.x + 2 * p, rr.x + rr.w - 2 * p - 1, rr.y + rr.h / 2,
                       ui::Color{ t.dim.r, t.dim.g, t.dim.b, 130 } );
            continue;
        }
        const bool hot = m.enabled &&
                         ( (int)i == m_sel ||
                           ( mx >= rr.x && mx < rr.x + rr.w && my >= rr.y && my < rr.y + rr.h ) );
        if ( hot ) fill_round( r, rr, std::max( 1, p - 1 ), t.accent );
        const ui::Color fg = !m.enabled ? ui::Color{ t.dim.r, t.dim.g, t.dim.b, 190 }
                                        : ( hot ? t.bg : t.text );
        int tx = rr.x + 2 * p;
        if ( anyCheck ) {
            if ( m.check && m.checked ) {
                // A filled block reads as "on" in both themes; a glyph tick
                // would need a font the atlas does not carry.
                SDL_Rect b{ tx, rr.y + rr.h / 2 - f.ch() / 4,
                            f.ch() / 2, f.ch() / 2 };
                fill_round( r, b, 1, fg );
            }
            tx += tick;
        }
        f.draw( r, tx, rr.y + ( rr.h - f.ch() ) / 2,
                fit( f, m.label, rr.x + rr.w - 2 * p - tx ), fg );
    }
}

int Popup::hit( int x, int y ) const {
    if ( !m_open ) return -1;
    for ( size_t i = 0; i < m_rects.size() && i < rows.size(); ++i ) {
        const SDL_Rect& rr = m_rects[i];
        if ( rr.w <= 0 || rows[i].separator || !rows[i].enabled ) continue;
        if ( x >= rr.x && x < rr.x + rr.w && y >= rr.y && y < rr.y + rr.h ) return (int)i;
    }
    return -1;
}

bool Popup::key( SDL_Keycode k, int& activated ) {
    activated = -1;
    if ( !m_open ) return false;
    switch ( k ) {
        case SDLK_ESCAPE:                       close(); return true;
        case SDLK_UP:                           m_step( -1 ); return true;
        case SDLK_DOWN:                         m_step( +1 ); return true;
        case SDLK_HOME: m_sel = -1;             m_step( +1 ); return true;
        case SDLK_END:  m_sel = (int)rows.size();m_step( -1 ); return true;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if ( m_sel >= 0 && m_sel < (int)rows.size() &&
                 !rows[(size_t)m_sel].separator && rows[(size_t)m_sel].enabled )
                activated = m_sel;
            close();
            return true;
        default: return false;
    }
}

} // namespace skin

//============================================================================
//  SampleBrowser
//============================================================================
namespace {
const int kDragPx = 4;      // movement before a press becomes a drag
const Uint32 kTypeAheadMs = 1500;   // a stale filter must not hide the folder

bool is_audio( const std::string& name ) {
    const size_t dot = name.find_last_of( '.' );
    if ( dot == std::string::npos ) return false;
    std::string ext = name.substr( dot + 1 );
    for ( char& c : ext ) c = (char)std::tolower( (unsigned char)c );
    return ext == "wav" || ext == "aif" || ext == "aiff" || ext == "flac" ||
           ext == "ogg" || ext == "mp3";
}

//! Extension gate BEFORE the (cheap, but still a file open) RIFF-signature
//! check -- no reason to probe every file in a folder full of WAVs.
bool is_soundfont_ext( const std::string& name ) {
    const size_t dot = name.find_last_of( '.' );
    if ( dot == std::string::npos ) return false;
    std::string ext = name.substr( dot + 1 );
    for ( char& c : ext ) c = (char)std::tolower( (unsigned char)c );
    return ext == "sf2" || ext == "sf3";
}

std::string lower( std::string s ) {
    for ( char& c : s ) c = (char)std::tolower( (unsigned char)c );
    return s;
}

//! Human file size.  The list carried Item::size all along and never showed it,
//! so "which of these three kicks is the 24-bit one" was unanswerable here.
std::string size_str( long long n ) {
    char b[32];
    if ( n < 1024 )              std::snprintf( b, sizeof b, "%lld B", n );
    else if ( n < 1024 * 1024 )  std::snprintf( b, sizeof b, "%.0f k", (double)n / 1024.0 );
    else if ( n < 1024LL * 1024 * 1024 )
                                 std::snprintf( b, sizeof b, "%.1f M", (double)n / ( 1024.0 * 1024.0 ) );
    else                         std::snprintf( b, sizeof b, "%.1f G", (double)n / ( 1024.0 * 1024.0 * 1024.0 ) );
    return b;
}

// Little theme-driven glyphs.  Two rows that differ only by their text colour
// are hard to scan; a shape in the gutter tells you folder-versus-file before
// you have read a single character.
void folder_glyph( SDL_Renderer* r, const SDL_Rect& q, ui::Color c ) {
    if ( q.w < 4 || q.h < 4 ) return;
    const int h = q.h * 3 / 4, y = q.y + ( q.h - h ) / 2;
    ui::fill_rect( r, SDL_Rect{ q.x, y + 1, q.w, h - 1 }, c );
    ui::fill_rect( r, SDL_Rect{ q.x, y, q.w / 2, 2 }, c );
}
void wave_glyph( SDL_Renderer* r, const SDL_Rect& q, ui::Color c ) {
    if ( q.w < 4 || q.h < 4 ) return;
    const int mid = q.y + q.h / 2;
    static const int kAmp[5] = { 2, 5, 3, 6, 2 };
    const int step = std::max( 1, q.w / 5 );
    for ( int i = 0; i < 5; ++i ) {
        const int a = std::max( 1, kAmp[i] * q.h / 12 );
        ui::fill_rect( r, SDL_Rect{ q.x + i * step, mid - a, 1, a * 2 }, c );
    }
}
//! The disclosure affordance on a soundfont file row: "+" collapsed, "-"
//! expanded -- same silhouette so the row does not jump width when it flips.
void plusminus_glyph( SDL_Renderer* r, const SDL_Rect& q, bool expanded, ui::Color c ) {
    if ( q.w < 4 || q.h < 4 ) return;
    const int cy = q.y + q.h / 2, cx = q.x + q.w / 2;
    const int half = std::max( 1, std::min( q.w, q.h ) / 2 - 1 );
    ui::fill_rect( r, SDL_Rect{ q.x, cy, q.w, 1 }, c );              // the "-"
    if ( !expanded )
        ui::fill_rect( r, SDL_Rect{ cx, q.y + q.h / 2 - half, 1, 2 * half }, c ); // -> "+"
}
} // namespace

void SampleBrowser::scan_drives() {
    m_drives.clear();
#ifdef _WIN32
    // Walking up with ".." can never cross to another volume, so without this
    // the browser is trapped on whichever disk it started on.
    const DWORD mask = ::GetLogicalDrives();
    for ( int i = 0; i < 26; ++i ) {
        if ( !( mask & ( 1u << i ) ) ) continue;
        char root[4] = { (char)( 'A' + i ), ':', '\\', 0 };
        const UINT type = ::GetDriveTypeA( root );
        if ( type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN ) continue;
        Drive d;
        d.label = std::string( 1, (char)( 'A' + i ) ) + ":";
        d.path  = root;
        m_drives.push_back( d );
    }
#else
    Drive root; root.label = "/"; root.path = "/";
    m_drives.push_back( root );
    std::error_code ec;
    if ( const char* home = std::getenv( "HOME" ) ) {
        if ( fs::is_directory( home, ec ) ) {
            Drive h; h.label = "~"; h.path = home;
            m_drives.push_back( h );
        }
    }
#endif
}

void SampleBrowser::set_dir( const std::string& dir ) {
    if ( m_drives.empty() ) scan_drives();
    std::error_code ec;
    fs::path p = dir.empty() ? fs::current_path( ec ) : fs::path( dir );
    if ( ec || !fs::exists( p, ec ) || !fs::is_directory( p, ec ) ) {
        // Do NOT silently bounce to the working directory: a folder that cannot
        // be opened (permissions, unreadable volume) then looked like "clicking
        // does nothing", because the list jumped somewhere unrelated.  Stay put
        // unless we have nowhere at all to be.
        if ( !m_dir.empty() ) {
            m_note = "cannot open that folder";
            if ( on_status ) on_status( m_note );
            return;
        }
        p = fs::current_path( ec );
    }
    m_dir = p.string();
    // -1, not 0: scan() tries to hold the cursor on the file it was already on,
    // and index 0 of the OUTGOING listing is a path from the folder we just
    // left.  Starting from "nothing selected" keeps that stale path out of it.
    m_sel = -1; m_scroll = 0; m_filter.clear(); m_filterMs = 0;
    m_lastIdx = -1; m_hotRow = -1; m_note.clear();
    m_followSel = true;
    scan();
    // Coming back UP: put the cursor on the folder we just left, so walking a
    // tree does not lose your place every time you press Backspace.
    if ( !m_cameFrom.empty() ) {
        for ( size_t i = 0; i < m_items.size(); ++i )
            if ( m_items[i].dir && lower( m_items[i].path ) == lower( m_cameFrom ) )
            { m_sel = (int)i; break; }
        m_cameFrom.clear();
    }
}

void SampleBrowser::set_dir_remember( const std::string& path ) {
    // Only remember when the target is actually our parent -- descending should
    // land on the first row, not on some unrelated leftover.
    std::error_code ec;
    const fs::path here( m_dir );
    if ( here.has_parent_path() && lower( here.parent_path().string() ) == lower( path ) )
        m_cameFrom = m_dir;
    else
        m_cameFrom.clear();
    (void)ec;
    set_dir( path );
}

void SampleBrowser::scan() {
    const std::string keep = ( m_sel >= 0 && m_sel < (int)m_items.size() )
                           ? m_items[(size_t)m_sel].path : std::string();
    m_items.clear();
    std::error_code ec;
    // ".." first so walking back up is always the top row.
    const fs::path here( m_dir );
    if ( here.has_parent_path() && here.parent_path() != here ) {
        Item up; up.name = ".."; up.path = here.parent_path().string(); up.dir = true;
        m_items.push_back( up );
    }
    std::vector<Item> dirs, files;
    for ( fs::directory_iterator it( here, fs::directory_options::skip_permission_denied, ec ),
          end; !ec && it != end; it.increment( ec ) ) {
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        if ( !name.empty() && name[0] == '.' && !m_showHidden ) continue;
        std::error_code e2;
        if ( fs::is_directory( p, e2 ) ) {
            Item d; d.name = name; d.path = p.string(); d.dir = true;
            dirs.push_back( d );
        } else if ( is_audio( name ) ) {
            Item f; f.name = name; f.path = p.string(); f.dir = false;
            f.size = (long long)fs::file_size( p, e2 );
            const auto tw = fs::last_write_time( p, e2 );
            f.mtime = e2 ? 0 : (long long)tw.time_since_epoch().count();
            files.push_back( f );
        } else if ( is_soundfont_ext( name ) ) {
            Item f; f.name = name; f.path = p.string(); f.dir = false;
            f.size = (long long)fs::file_size( p, e2 );
            const auto tw = fs::last_write_time( p, e2 );
            f.mtime = e2 ? 0 : (long long)tw.time_since_epoch().count();
            // A file merely NAMED .sf2 that is not really one (renamed, half
            // downloaded) just loses the "+" -- it still lists like any other
            // file rather than blocking the whole folder scan.
            f.isSoundFont = PatchKnob::engine::sf2::looksLikeSoundFont( f.path );
            files.push_back( f );
        }
    }
    auto byName = []( const Item& a, const Item& b ) { return lower( a.name ) < lower( b.name ); };
    auto bySize = []( const Item& a, const Item& b ) { return a.size > b.size; };
    auto byDate = []( const Item& a, const Item& b ) { return a.mtime > b.mtime; };
    std::sort( dirs.begin(), dirs.end(), byName );          // folders always A-Z
    if ( m_sort == 1 )      std::sort( files.begin(), files.end(), bySize );
    else if ( m_sort == 2 ) std::sort( files.begin(), files.end(), byDate );
    else                    std::sort( files.begin(), files.end(), byName );
    m_items.insert( m_items.end(), dirs.begin(), dirs.end() );
    m_items.insert( m_items.end(), files.begin(), files.end() );

    int matched = 0;
    if ( !m_filter.empty() ) {
        const std::string f = lower( m_filter );
        for ( const Item& i : m_items )
            if ( !i.dir && lower( i.name ).find( f ) != std::string::npos ) ++matched;
        m_items.erase( std::remove_if( m_items.begin(), m_items.end(),
            [&]( const Item& i ) {
                return !i.dir && lower( i.name ).find( f ) == std::string::npos;
            } ), m_items.end() );
    }

    // Splice cached presets in directly under every EXPANDED soundfont file --
    // AFTER the filter, so a filter that drops the file drops its presets with
    // it instead of leaving orphan rows with no parent on screen.  Indices
    // shift as rows are inserted, so this walks with its own cursor rather
    // than a range-for.
    for ( size_t i = 0; i < m_items.size(); ++i ) {
        const Item& parent = m_items[i];
        if ( parent.dir || parent.isPreset || !parent.isSoundFont ) continue;
        if ( !m_sf2Expanded.count( parent.path ) ) continue;
        const auto cacheIt = m_sf2Cache.find( parent.path );
        if ( cacheIt == m_sf2Cache.end() || !cacheIt->second.ok ) continue;
        const auto& presets = cacheIt->second.font.presets;
        std::vector<Item> rows;
        rows.reserve( presets.size() );
        for ( const auto& pr : presets ) {
            Item row;
            row.path = parent.path;                 // the OWNING .sf2 -- not a real file
            row.isPreset = true;
            row.presetBank = pr.bank;
            row.presetProgram = pr.program;
            char hdr[32];
            std::snprintf( hdr, sizeof hdr, "[%d:%d] ", pr.bank, pr.program );
            row.name = std::string( hdr ) + pr.name;
            rows.push_back( std::move( row ) );
        }
        m_items.insert( m_items.begin() + (long)i + 1, rows.begin(), rows.end() );
        i += rows.size();          // resume just past the rows just inserted
    }

    // Hold the cursor on the SAME file across a refresh / sort change.  Sorting
    // by size used to leave the highlight on whatever happened to land at the
    // old index, which reads as the selection jumping at random.
    m_sel = 0;
    if ( !keep.empty() )
        for ( size_t i = 0; i < m_items.size(); ++i )
            if ( m_items[i].path == keep ) { m_sel = (int)i; break; }
    if ( m_sel >= (int)m_items.size() ) m_sel = (int)m_items.size() - 1;
    if ( m_sel < 0 ) m_sel = 0;

    if ( !m_filter.empty() && matched == 0 )
        m_note = "nothing matches \"" + m_filter + "\"";
    else
        m_note.clear();
}

void SampleBrowser::toggle_sf2_expand( int idx ) {
    if ( idx < 0 || idx >= (int)m_items.size() ) return;
    const Item& it = m_items[(size_t)idx];
    if ( it.dir || it.isPreset || !it.isSoundFont ) return;
    const std::string path = it.path;
    if ( m_sf2Expanded.count( path ) ) {
        m_sf2Expanded.erase( path );
        scan();
        return;
    }
    auto cacheIt = m_sf2Cache.find( path );
    if ( cacheIt == m_sf2Cache.end() ) {
        // Headers-only: 8 ms / 6.4 MB even on an 800 MB bank.  Cached per path
        // below, so this is the only time THIS file ever pays for it.
        Sf2Entry entry;
        entry.ok = PatchKnob::engine::sf2::read( path, entry.font, entry.error,
                                                 /*loadPcm=*/false );
        cacheIt = m_sf2Cache.emplace( path, std::move( entry ) ).first;
    }
    if ( !cacheIt->second.ok ) {
        // Degrade quietly: a note on the status line, never a crash, and the
        // row stays collapsed so the user can tell nothing happened.
        if ( on_status ) on_status( "could not read \"" + it.name + "\": " +
                                    cacheIt->second.error );
        return;
    }
    m_sf2Expanded.insert( path );
    scan();
}

//----------------------------------------------------------------------------
//  geometry -- ONE pass shared by the painter and the hit-tests
//----------------------------------------------------------------------------
void SampleBrowser::layout( const ui::Font& f ) {
    const int p    = skin::pad( f );
    m_rowH         = f.ch() + p;
    const int barH = f.ch() + 2 * p;

    m_head     = SDL_Rect{ rect.x, rect.y, rect.w, barH };
    m_crumbBar = m_head;
    int y = rect.y + barH;

    // The drive strip only earns its space when there is something to choose
    // between; on Linux with no $HOME it would otherwise be a blank bar.
    m_driveBar = ( m_drives.size() > 1 )
               ? SDL_Rect{ rect.x, y, rect.w, barH }
               : SDL_Rect{ rect.x, y, rect.w, 0 };
    y += m_driveBar.h;

    m_list = SDL_Rect{ rect.x, y, rect.w, std::max( 0, rect.y + rect.h - y ) };

    // ---- breadcrumb chips --------------------------------------------------
    m_crumbs.clear();
    if ( !m_dir.empty() ) {
        fs::path acc;
        const fs::path here( m_dir );
        for ( const fs::path& part : here ) {
            acc /= part;
            std::string lab = part.string();
            while ( !lab.empty() && ( lab.back() == '\\' || lab.back() == '/' ) )
                lab.pop_back();
            if ( lab.empty() ) lab = "/";
            Crumb c; c.label = lab; c.path = acc.string();
            m_crumbs.push_back( c );
        }
    }
    // ---- filter chip, right aligned ---------------------------------------
    // Placed BEFORE the breadcrumb so the crumbs know how much of the bar is
    // already spoken for; laying it out afterwards let a long path run under
    // the chip, which is how the two used to end up drawn on top of each other.
    const int gap = p;
    if ( !m_filter.empty() ) {
        const int w = f.text_w( "/" + m_filter ) + 2 * p;
        m_filterChip = SDL_Rect{ rect.x + rect.w - p - w, m_crumbBar.y + p / 2,
                                 w, barH - p };
    } else {
        m_filterChip = SDL_Rect{ 0, 0, 0, 0 };
    }
    const int crumbRight = ( m_filterChip.w > 0 ? m_filterChip.x - gap
                                                : rect.x + rect.w - p );

    // Drop leading segments until the trail fits, and turn the first survivor
    // into a "..." chip that still navigates -- a path truncated by character
    // count (what this did before) both overflowed the bar and lost the leaf.
    const int availW = crumbRight - ( rect.x + p );
    size_t first = 0;
    for ( ;; ) {
        int need = 0;
        for ( size_t i = first; i < m_crumbs.size(); ++i )
            need += f.text_w( m_crumbs[i].label ) + 2 * p + gap;
        if ( first > 0 ) need += f.text_w( "..." ) + 2 * p + gap;
        if ( need <= availW || first + 1 >= m_crumbs.size() ) break;
        ++first;
    }
    int cx = rect.x + p;
    for ( size_t i = 0; i < m_crumbs.size(); ++i ) {
        m_crumbs[i].ell = false;
        if ( i < first ) { m_crumbs[i].r = SDL_Rect{ 0, 0, 0, 0 }; continue; }
        m_crumbs[i].ell = ( i == first && first > 0 );
        const int w = f.text_w( m_crumbs[i].ell ? "..." : m_crumbs[i].label ) + 2 * p;
        // Out of room: zero the rect AND stop, so no later chip can slip into
        // the gap and be drawn out of order with its neighbours missing.
        if ( cx + w > crumbRight ) {
            for ( size_t j = i; j < m_crumbs.size(); ++j ) m_crumbs[j].r = SDL_Rect{ 0, 0, 0, 0 };
            break;
        }
        m_crumbs[i].r = SDL_Rect{ cx, m_crumbBar.y + p / 2, w, barH - p };
        cx += w + gap;
    }

    // ---- drive chips -------------------------------------------------------
    // Every chip that does NOT fit gets a zeroed rect.  It used to keep the
    // rect from a frame when the pane was wider, so clicking blank header space
    // teleported you to another volume.
    if ( m_driveBar.h > 0 ) {
        const std::string cur = lower( m_dir );
        int dx = rect.x + p;
        for ( Drive& d : m_drives ) {
            const int w = f.text_w( d.label ) + 2 * p;
            if ( dx + w > rect.x + rect.w - p ) { d.r = SDL_Rect{ 0, 0, 0, 0 }; continue; }
            d.r = SDL_Rect{ dx, m_driveBar.y + p / 2, w, barH - p };
            dx += w + gap;
            (void)cur;
        }
    } else {
        for ( Drive& d : m_drives ) d.r = SDL_Rect{ 0, 0, 0, 0 };
    }
}

int SampleBrowser::visible_rows() const {
    return std::max( 1, m_list.h / std::max( 1, m_rowH ) );
}

int SampleBrowser::row_at( int y ) const {
    if ( y < m_list.y || y >= m_list.y + m_list.h ) return -1;
    const int rh = std::max( 1, m_rowH );
    const int painted = m_list.h / rh;          // rows that are actually DRAWN
    const int slot = ( y - m_list.y ) / rh;
    // The strip left over below the last whole row is not a row.  It used to
    // map to m_scroll+painted, so clicking blank space at the bottom selected
    // (and auditioned) a file you could not see.
    if ( slot < 0 || slot >= painted ) return -1;
    const int i = m_scroll + slot;
    return ( i >= 0 && i < (int)m_items.size() ) ? i : -1;
}

std::string SampleBrowser::selected_path() const {
    if ( m_sel < 0 || m_sel >= (int)m_items.size() ) return std::string();
    const Item& it = m_items[(size_t)m_sel];
    return ( it.dir || it.isPreset ) ? std::string() : it.path;
}

void SampleBrowser::move_sel( int to, bool audition ) {
    const int n = (int)m_items.size();
    if ( n <= 0 ) { m_sel = 0; return; }
    to = std::max( 0, std::min( to, n - 1 ) );
    const bool changed = ( to != m_sel );
    m_sel = to;
    m_followSel = true;
    // Audition only on a REAL move.  on_select decodes the whole file, and it
    // used to fire on every key -- including each letter of the type-ahead and
    // every Down press once you were already on the last row -- which stalled
    // the UI for as long as the decode took.  A preset row or a raw soundfont
    // file is not a decodable audio file either -- auditioning one would try
    // to run an .sf2 through the WAV loader.
    const Item& sel = m_items[(size_t)m_sel];
    if ( changed && audition && on_select && !sel.dir && !sel.isPreset && !sel.isSoundFont )
        on_select( sel.path );
}

//----------------------------------------------------------------------------
//  draw
//----------------------------------------------------------------------------
void SampleBrowser::draw( ui::App& app ) {
    SDL_Renderer* r = app.ren;
    const ui::Font& f = app.mono;
    const ui::Theme& t = ui::theme();
    layout( f );
    const int p = skin::pad( f );

    ui::fill_rect( r, rect, t.bg );
    ui::fill_rect( r, m_head, t.panel );
    if ( m_driveBar.h > 0 ) ui::fill_rect( r, m_driveBar, t.panel );
    ui::hline( r, rect.x, rect.x + rect.w - 1,
               m_list.y - 1, ui::Color{ t.dim.r, t.dim.g, t.dim.b, 140 } );

    // ---- breadcrumb --------------------------------------------------------
    int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
    for ( size_t i = 0; i < m_crumbs.size(); ++i ) {
        const Crumb& c = m_crumbs[i];
        if ( c.r.w <= 0 ) continue;
        const bool leaf = ( i + 1 == m_crumbs.size() );
        const bool hot  = mx >= c.r.x && mx < c.r.x + c.r.w &&
                          my >= c.r.y && my < c.r.y + c.r.h;
        const std::string lab = c.ell ? std::string( "..." ) : c.label;
        skin::pill( r, f, c.r, lab,
                    leaf ? skin::StActive : ( hot ? skin::StHover : skin::StIdle ) );
    }
    if ( m_crumbs.empty() )
        f.draw( r, rect.x + 2 * p, m_head.y + p, "(no folder)", t.dim );

    // The type-ahead used to be painted at the SAME origin as the path, so the
    // two strings overlapped into unreadable pixels.  It gets its own chip.
    if ( m_filterChip.w > 0 ) {
        skin::pill( r, f, m_filterChip, "/" + m_filter, skin::StActive );
    }

    // ---- drive chips -------------------------------------------------------
    if ( m_driveBar.h > 0 ) {
        const std::string cur = lower( m_dir );
        for ( const Drive& d : m_drives ) {
            if ( d.r.w <= 0 ) continue;
            const bool active = cur.rfind( lower( d.path ), 0 ) == 0;
            const bool hot = mx >= d.r.x && mx < d.r.x + d.r.w &&
                             my >= d.r.y && my < d.r.y + d.r.h;
            skin::pill( r, f, d.r, d.label,
                        active ? skin::StActive : ( hot ? skin::StHover : skin::StIdle ) );
        }
    }

    // ---- list --------------------------------------------------------------
    const int rows = visible_rows();
    if ( m_followSel ) {
        if ( m_sel < m_scroll ) m_scroll = m_sel;
        if ( m_sel >= m_scroll + rows ) m_scroll = m_sel - rows + 1;
        m_followSel = false;
    }
    const int maxScroll = std::max( 0, (int)m_items.size() - rows );
    m_scroll = std::max( 0, std::min( m_scroll, maxScroll ) );

    ui::ScopedClip clip( r, m_list );
    const int glyphW = f.ch();
    for ( int i = 0; i < rows; ++i ) {
        const int idx = m_scroll + i;
        if ( idx < 0 || idx >= (int)m_items.size() ) break;
        const Item& it = m_items[(size_t)idx];
        SDL_Rect row{ m_list.x, m_list.y + i * m_rowH, m_list.w, m_rowH };

        // Striping first, then hover, then selection: a long list of same-toned
        // rows is very hard to track across horizontally.
        if ( ( idx & 1 ) == 0 )
            skin::wash_blend( r, row, t.dim, 20 );
        if ( idx == m_hotRow && idx != m_sel )
            skin::wash_blend( r, row, t.accent, 40 );
        if ( idx == m_sel ) {
            skin::fill_round( r, SDL_Rect{ row.x + 1, row.y, row.w - 2, row.h },
                              std::max( 1, p - 1 ),
                              m_focused ? t.accent
                                        : ui::Color{ t.accent.r, t.accent.g, t.accent.b, 90 } );
        }
        const ui::Color fg = ( idx == m_sel && m_focused )
                           ? t.bg : ( it.dir ? t.accent : t.text );

        // A preset row is nested one level under the soundfont it belongs to
        // -- indent its glyph column so it reads as "inside" that file.
        const int indent = it.isPreset ? glyphW + p : 0;
        SDL_Rect gl{ row.x + 2 * p + indent, row.y + ( m_rowH - glyphW ) / 2, glyphW, glyphW };
        if ( it.isPreset ) {
            // No glyph: the "[bank:program]" prefix already marks it as a
            // preset, and a bare row reads as visually subordinate.
        } else if ( it.dir ) {
            folder_glyph( r, gl, fg );
        } else if ( it.isSoundFont ) {
            plusminus_glyph( r, gl, m_sf2Expanded.count( it.path ) != 0, fg );
        } else {
            wave_glyph( r, gl, fg );
        }

        // Size is right-aligned so the digits line up in a column and can be
        // compared down the list instead of read one row at a time.
        int textRight = row.x + row.w - 2 * p;
        if ( !it.dir && it.size > 0 ) {
            const std::string sz = size_str( it.size );
            const int sw = f.text_w( sz );
            if ( row.w > sw + glyphW * 6 ) {
                f.draw( r, row.x + row.w - 2 * p - sw, row.y + ( m_rowH - f.ch() ) / 2, sz,
                        ( idx == m_sel && m_focused ) ? t.bg
                                                      : ui::Color{ t.dim.r, t.dim.g, t.dim.b, 220 } );
                textRight -= sw + 2 * p;
            }
        }
        const int nameX = gl.x + glyphW + p + p / 2;
        f.draw( r, nameX, row.y + ( m_rowH - f.ch() ) / 2,
                skin::fit( f, it.name, textRight - nameX ), fg );
    }

    // ---- empty states ------------------------------------------------------
    // An empty box that just said "(no audio files)" gave no hint that the
    // reason might be a filter you typed three seconds ago.
    if ( m_items.empty() || !m_note.empty() ) {
        int ty = m_list.y + 2 * p + ( m_items.empty() ? m_list.h / 4 : m_list.h - 3 * f.ch() );
        ty = std::min( ty, m_list.y + m_list.h - 2 * f.ch() - p );
        const std::string head = !m_note.empty() ? m_note : std::string( "No audio files here" );
        const std::string hint = !m_note.empty()
                               ? std::string( "Esc clears the filter" )
                               : std::string( "wav aif flac ogg mp3 -- folders still listed" );
        f.draw( r, m_list.x + 2 * p, ty, skin::fit( f, head, m_list.w - 4 * p ), t.text );
        f.draw( r, m_list.x + 2 * p, ty + f.ch() + p / 2,
                skin::fit( f, hint, m_list.w - 4 * p ), t.dim );
    }

    // ---- focus ring --------------------------------------------------------
    // Two panes share the keyboard; without this there was no way to tell which
    // one an arrow key was about to move.
    if ( m_focused )
        skin::frame_round( r, m_list, std::max( 2, p ),
                           ui::Color{ t.accent.r, t.accent.g, t.accent.b, 150 } );
}

void SampleBrowser::draw_overlay( ui::App& app ) {
    const ui::Font& f = app.mono;
    const ui::Theme& t = ui::theme();
    const int p = skin::pad( f );
    if ( m_dragging && !m_dragPayload.path.empty() ) {
        int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
        const std::string leaf = m_dragPayload.kind == DragPayload::Sf2Preset
                                ? m_dragPayload.label
                                : fs::path( m_dragPayload.path ).filename().string();
        const int wpx = f.text_w( leaf ) + 4 * p;
        SDL_Rect g{ mx + 12, my - f.ch(), wpx, f.ch() + 2 * p };
        skin::fill_round( app.ren, SDL_Rect{ g.x + 2, g.y + 2, g.w, g.h }, p,
                          ui::Color{ 0, 0, 0, 70 } );
        skin::fill_round( app.ren, g, p, t.panel );
        skin::frame_round( app.ren, g, p, t.accent );
        f.draw( app.ren, g.x + 2 * p, g.y + p, leaf, t.text );
    }
    if ( m_menu.open() ) {
        m_menu.layout( f, rect );
        m_menu.draw( app, f );
    }
}

//----------------------------------------------------------------------------
//  context menu
//----------------------------------------------------------------------------
void SampleBrowser::build_menu() {
    const bool haveRow = m_menuRow >= 0 && m_menuRow < (int)m_items.size();
    const Item* mi      = haveRow ? &m_items[(size_t)m_menuRow] : nullptr;
    const bool isDir    = mi && mi->dir;
    const bool isPreset = mi && mi->isPreset;
    const bool isSf2    = mi && !isDir && !isPreset && mi->isSoundFont;
    // A preset leaves only via a drag; a raw soundfont file's primary action
    // is expand/collapse, not "load" or "preview" (neither means anything for
    // an .sf2 as a whole).
    const bool isFile   = mi && !isDir && !isPreset && !isSf2;
    std::error_code ec;
    const fs::path here( m_dir );
    const bool canUp = here.has_parent_path() && here.parent_path() != here;
    (void)ec;

    std::string primary = "Load into slot";
    if ( isDir ) primary = "Open folder";
    else if ( isSf2 ) primary = m_sf2Expanded.count( mi->path ) ? "Collapse" : "Expand";

    m_menu.rows.clear();
    m_menu.rows.push_back( skin::MenuRow{ primary, 1, false, haveRow && !isPreset, false, false } );
    m_menu.rows.push_back( skin::MenuRow{ "Preview",       2, false, isFile,  false, false } );
    m_menu.rows.push_back( skin::MenuRow{ "Copy path",     3, false, haveRow, false, false } );
    m_menu.rows.push_back( skin::MenuRow{ "",              0, true,  true,    false, false } );
    m_menu.rows.push_back( skin::MenuRow{ "Go up",         4, false, canUp,   false, false } );
    m_menu.rows.push_back( skin::MenuRow{ "Refresh",       5, false, true,    false, false } );
    m_menu.rows.push_back( skin::MenuRow{ "Clear filter",  6, false, !m_filter.empty(), false, false } );
    m_menu.rows.push_back( skin::MenuRow{ "",              0, true,  true,    false, false } );
    m_menu.rows.push_back( skin::MenuRow{ "Sort by name",  7, false, true, true, m_sort == 0 } );
    m_menu.rows.push_back( skin::MenuRow{ "Sort by size",  8, false, true, true, m_sort == 1 } );
    m_menu.rows.push_back( skin::MenuRow{ "Sort by newest",9, false, true, true, m_sort == 2 } );
    m_menu.rows.push_back( skin::MenuRow{ "Show hidden",  10, false, true, true, m_showHidden } );
}

void SampleBrowser::run_menu( ui::App& app, int op ) {
    const bool haveRow = m_menuRow >= 0 && m_menuRow < (int)m_items.size();
    switch ( op ) {
        case 1:
            if ( haveRow ) {
                const Item it = m_items[(size_t)m_menuRow];
                if ( it.dir ) set_dir_remember( it.path );
                else if ( it.isPreset ) { /* no menu action -- drag it out instead */ }
                else if ( it.isSoundFont ) toggle_sf2_expand( m_menuRow );
                else if ( on_activate ) on_activate( it.path );
            }
            break;
        case 2: if ( haveRow && !m_items[(size_t)m_menuRow].dir &&
                     !m_items[(size_t)m_menuRow].isPreset &&
                     !m_items[(size_t)m_menuRow].isSoundFont && on_select )
                    on_select( m_items[(size_t)m_menuRow].path );
                break;
        case 3: if ( haveRow ) {
                    SDL_SetClipboardText( m_items[(size_t)m_menuRow].path.c_str() );
                    if ( on_status ) on_status( "path copied to the clipboard" );
                }
                break;
        case 4: {
            std::error_code ec; (void)ec;
            const fs::path here( m_dir );
            if ( here.has_parent_path() && here.parent_path() != here )
                set_dir_remember( here.parent_path().string() );
            break;
        }
        case 5: scan(); break;
        case 6: m_filter.clear(); m_filterMs = 0; scan(); break;
        case 7: m_sort = 0; scan(); break;
        case 8: m_sort = 1; scan(); break;
        case 9: m_sort = 2; scan(); break;
        case 10: m_showHidden = !m_showHidden; scan(); break;
        default: break;
    }
    app.request_redraw();
}

//----------------------------------------------------------------------------
//  input
//----------------------------------------------------------------------------
bool SampleBrowser::on_mouse( ui::App& app, const ui::MouseEv& e ) {
    layout( app.mono );
    const bool inside = hit( e.x, e.y );
    const bool downEdge = m_downEdge;
    const bool left  = e.button == SDL_BUTTON_LEFT;
    const bool right = e.button == SDL_BUTTON_RIGHT;

    // ---- context menu owns the pointer while it is up ----------------------
    if ( m_menu.open() ) {
        m_menu.layout( app.mono, rect );
        if ( !e.pressed ) { m_menu.arm(); app.request_redraw(); return true; }
        // Ignore everything until the button that opened the menu has been
        // released.  Motion events carry pressed == true, so without this the
        // menu closed (and fired a row) the moment the mouse twitched.
        if ( !m_menu.armed() ) { app.request_redraw(); return true; }
        if ( downEdge ) {
            const int row = m_menu.hit( e.x, e.y );
            if ( row >= 0 ) {
                const int op = m_menu.rows[(size_t)row].op;
                m_menu.close();
                run_menu( app, op );
            } else if ( !m_menu.inside( e.x, e.y ) ) {
                m_menu.close();               // click outside just dismisses
            }
            // A click that landed on a greyed row or a separator leaves the
            // menu up: closing it there meant a mis-aim cost you the whole
            // menu and you had to right-click again to find your place.
            app.request_redraw();
        }
        return true;
    }

    if ( downEdge && inside && right ) {
        // Right-click is a MENU, not a second left button.  It used to fall
        // through the same path as a left press: it selected the row, started a
        // drag, and two quick right-clicks opened a folder.
        m_menuRow = row_at( e.y );
        if ( m_menuRow >= 0 ) { m_sel = m_menuRow; m_followSel = true; }
        build_menu();
        m_menu.show( e.x, e.y );
        m_menu.layout( app.mono, rect );
        app.request_redraw();
        return true;
    }

    // Hover feedback: the row under the pointer lights before you commit to it.
    if ( !e.pressed || !m_press ) {
        const int hot = inside ? row_at( e.y ) : -1;
        if ( hot != m_hotRow ) { m_hotRow = hot; app.request_redraw(); }
    }

    if ( downEdge && inside && left ) {
        for ( size_t i = 0; i < m_crumbs.size(); ++i ) {
            const SDL_Rect& cr = m_crumbs[i].r;
            if ( cr.w > 0 && e.x >= cr.x && e.x < cr.x + cr.w &&
                 e.y >= cr.y && e.y < cr.y + cr.h ) {
                if ( lower( m_crumbs[i].path ) != lower( m_dir ) )
                    set_dir_remember( m_crumbs[i].path );
                app.request_redraw();
                return true;
            }
        }
        if ( m_filterChip.w > 0 && e.x >= m_filterChip.x &&
             e.x < m_filterChip.x + m_filterChip.w &&
             e.y >= m_filterChip.y && e.y < m_filterChip.y + m_filterChip.h ) {
            m_filter.clear(); m_filterMs = 0; scan();     // click the chip to clear
            app.request_redraw();
            return true;
        }
        for ( const Drive& d : m_drives )
            if ( d.r.w > 0 && e.x >= d.r.x && e.x < d.r.x + d.r.w &&
                 e.y >= d.r.y && e.y < d.r.y + d.r.h ) {
                m_cameFrom.clear();
                set_dir( d.path );
                app.request_redraw();
                return true;
            }
        const int idx = row_at( e.y );
        if ( idx >= 0 ) {
            const Item& it = m_items[(size_t)idx];
            // The "+"/"-" affordance lives in the glyph gutter of a soundfont
            // FILE row (never a spliced-in preset row).  A hit there just
            // (un)expands it -- like a folder's disclosure arrow, it neither
            // moves the selection nor arms a drag.
            if ( !it.dir && !it.isPreset && it.isSoundFont ) {
                const int p = skin::pad( app.mono );
                const int glyphGutter = 2 * p + app.mono.ch() + p;
                if ( e.x < m_list.x + glyphGutter ) {
                    toggle_sf2_expand( idx );
                    app.request_redraw();
                    return true;
                }
            }
            // A real double-click: same row, same BUTTON, and the pointer must
            // not have travelled.  Time alone meant two fast clicks on two
            // different rows counted as a double-click on the second one.
            const Uint32 now = SDL_GetTicks();
            const bool dbl = ( idx == m_lastIdx ) && ( now - m_lastClickMs < 350 ) &&
                             std::abs( e.x - m_lastClickX ) <= kDragPx &&
                             std::abs( e.y - m_lastClickY ) <= kDragPx;
            m_lastIdx = idx; m_lastClickMs = now;
            m_lastClickX = e.x; m_lastClickY = e.y;
            move_sel( idx, false );
            if ( it.dir ) {
                if ( dbl ) { set_dir_remember( it.path ); m_lastIdx = -1; }
            } else if ( it.isPreset ) {
                // Not a playable file: no audition, no direct load -- the only
                // way it leaves the browser is a drag (an Sf2Preset payload).
                m_press = true; m_pressX = e.x; m_pressY = e.y;
                m_dragPayload = DragPayload{ DragPayload::Sf2Preset, it.path,
                                             it.presetBank, it.presetProgram, it.name };
            } else if ( it.isSoundFont ) {
                // Double-click on the soundfont itself expands it too, same as
                // hitting the affordance directly.
                m_press = true; m_pressX = e.x; m_pressY = e.y;
                m_dragPayload = DragPayload{ DragPayload::File, it.path, 0, 0, it.name };
                if ( dbl ) { toggle_sf2_expand( idx ); m_lastIdx = -1; }
            } else {
                m_press = true; m_pressX = e.x; m_pressY = e.y;
                m_dragPayload = DragPayload{ DragPayload::File, it.path, 0, 0, it.name };
                if ( dbl && on_activate ) on_activate( it.path );
                else if ( on_select )     on_select( it.path );
            }
            app.request_redraw();
            return true;
        }
        return inside;
    }

    if ( m_press && e.pressed ) {                        // motion with button down
        if ( !m_dragging &&
             ( std::abs( e.x - m_pressX ) > kDragPx || std::abs( e.y - m_pressY ) > kDragPx ) )
            m_dragging = true;
        if ( m_dragging ) { app.request_redraw(); return true; }
    }

    if ( !e.pressed ) {
        const bool wasDragging = m_dragging;
        const DragPayload payload = m_dragPayload;
        m_press = false; m_dragging = false; m_dragPayload = DragPayload();
        if ( wasDragging && !payload.path.empty() ) {
            if ( on_drag_drop ) on_drag_drop( payload, e.x, e.y );  // shell routes it
            app.request_redraw();
            return true;
        }
    }
    return false;
}

bool SampleBrowser::on_wheel( ui::App& app, int dx, int dy ) {
    (void)dx;
    layout( app.mono );                        // visible_rows() clamps against it
    if ( dy == 0 ) return false;
    if ( m_menu.open() ) return true;          // do not scroll under an open menu
    m_scroll -= dy * 3;
    const int maxScroll = std::max( 0, (int)m_items.size() - visible_rows() );
    m_scroll = std::max( 0, std::min( m_scroll, maxScroll ) );
    m_followSel = false;               // the wheel wins until the selection moves
    app.request_redraw();
    return true;
}

bool SampleBrowser::on_key( ui::App& app, SDL_Keycode k ) {
    layout( app.mono );
    if ( m_menu.open() ) {
        int row = -1;
        if ( m_menu.key( k, row ) ) {
            if ( row >= 0 ) run_menu( app, m_menu.rows[(size_t)row].op );
            app.request_redraw();
            return true;
        }
        return true;                            // menu swallows everything else
    }

    const int n = (int)m_items.size();
    const auto* keys = SDL_GetKeyboardState( nullptr );
    const bool ctrl  = keys && ( keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL] );
    const Uint32 now = SDL_GetTicks();
    // A filter typed a minute ago and forgotten looks exactly like an empty
    // folder.  Anything typed after a pause starts a NEW search instead of
    // extending a stale one.
    if ( !m_filter.empty() && m_filterMs && now - m_filterMs > kTypeAheadMs * 8 ) {
        m_filter.clear(); m_filterMs = 0; scan();
    }

    if ( k == SDLK_UP )             move_sel( m_sel - 1, true );
    else if ( k == SDLK_DOWN )      move_sel( m_sel + 1, true );
    else if ( k == SDLK_PAGEUP )    move_sel( m_sel - visible_rows(), true );
    else if ( k == SDLK_PAGEDOWN )  move_sel( m_sel + visible_rows(), true );
    else if ( k == SDLK_HOME )      move_sel( 0, true );          // jump to the top
    else if ( k == SDLK_END )       move_sel( n - 1, true );      // ... and the end
    else if ( k == SDLK_F5 )        { scan(); }
    else if ( k == SDLK_SPACE ) {
        // Re-audition the row you are already on, without moving.  Holding an
        // arrow through a folder is how you hunt by ear; Space is how you hear
        // the one you stopped on again.
        if ( m_sel >= 0 && m_sel < n && !m_items[(size_t)m_sel].dir &&
             !m_items[(size_t)m_sel].isPreset && !m_items[(size_t)m_sel].isSoundFont && on_select )
            on_select( m_items[(size_t)m_sel].path );
    }
    else if ( k == SDLK_BACKSPACE ) {
        if ( !m_filter.empty() ) { m_filter.pop_back(); m_filterMs = now; scan(); }
        else if ( !m_items.empty() && m_items[0].name == ".." )
            set_dir_remember( m_items[0].path );
    }
    else if ( k == SDLK_RETURN || k == SDLK_KP_ENTER ) {
        if ( m_sel >= 0 && m_sel < n ) {
            const Item it = m_items[(size_t)m_sel];
            if ( it.dir ) set_dir_remember( it.path );
            else if ( it.isPreset ) { /* leaves only via a drag */ }
            else if ( it.isSoundFont ) toggle_sf2_expand( m_sel );
            else if ( on_activate ) on_activate( it.path );
        }
    }
    else if ( k == SDLK_ESCAPE ) {
        if ( m_filter.empty() ) return false;   // let the host close the window
        m_filter.clear(); m_filterMs = 0; scan();
    }
    else if ( !ctrl && k >= 32 && k < 127 ) {
        m_filter.push_back( (char)k ); m_filterMs = now; scan();
    }
    else return false;
    app.request_redraw();
    return true;
}

void SampleBrowser::cancel_interaction( ui::App& app ) {
    // A gesture the host took away (window drag, focus loss, a modal opening)
    // used to leave m_press latched, so the NEXT click was interpreted as the
    // continuation of the old drag and dropped a file nobody had picked up.
    const bool had = m_press || m_dragging || m_menu.open();
    cancel_press();
    m_menu.close();
    m_hotRow = -1;
    if ( had ) app.request_redraw();
}

} // namespace sampleslot
