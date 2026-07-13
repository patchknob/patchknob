//----------------------------------------------------------------------------
//  apptheme.cpp  -- seq24 Windows port: runtime, switchable monochrome theme.
//
//  Implements the seq24::theme API declared in apptheme.h:
//      set_mode(Mode) / current_mode() / apply_theme(Mode) / apply_ancient_theme()
//
//  HOW IT WORKS
//  ------------
//  gtkmm-2.4 wraps GTK+ 2.24, whose widget appearance is driven by the RC
//  resource system.  Feeding an RC string to gtk_rc_parse_string() and then
//  calling gtk_rc_reset_styles() re-skins the whole application chrome live:
//  menu bars, menus, buttons, entries, check buttons, scrollbars, scales,
//  notebook tabs and tooltips -- everything the default GTK engine draws.
//
//  Each mode's RC uses the SAME style names ("app_default", "app_menuitem",
//  "app_entry", "app_tooltip").  Re-parsing another mode's string therefore
//  OVERWRITES those named styles' color/thickness/engine properties in place,
//  and gtk_rc_reset_styles() re-realizes existing widgets against the new
//  values -- giving a clean live theme switch.
//
//  Both modes:
//    * bind to class "*" at the HIGHEST priority so they beat whatever theme
//      the MSYS2 GTK runtime installs (typically the colored native
//      "ms-windows"/wimp engine);
//    * select GTK's plain built-in engine via `engine "" { }`, which honours
//      our color arrays literally and gives the flat/beveled retro look;
//    * are strictly MONOCHROME -- ANCIENT uses only neutral greys (R==G==B),
//      MIDNIGHT uses only phosphor greens (R,B well below G).  GTK auto-derives
//      its light/dark/mid bevel shades from bg[]; since bg stays within the one
//      hue family, so do the derived shades -- no foreign color can leak in.
//
//  This module also owns the coupling to the custom-draw palette: set_mode()
//  updates synth::set_palette() (ui/palette.h) in the same call, so chrome and
//  custom drawing always agree on the active mode.
//----------------------------------------------------------------------------

#include <gtk/gtk.h>   // gtk_rc_parse_string(), gtk_rc_reset_styles()
#include <string>
#include <sstream>

#include "apptheme.h"
#include "ui/palette.h"

namespace seq24 { namespace theme {

namespace {

//  One mode's chrome colors, as "#rrggbb" strings.  fg/bg/base/text are given
//  for all five widget states (Normal/Active/Prelight/Selected/Insensitive).
struct chrome_colors
{
    // fg  : foreground drawn on bg (labels, button text)
    const char *fg_n, *fg_a, *fg_p, *fg_s, *fg_i;
    // bg  : widget background / panels / buttons
    const char *bg_n, *bg_a, *bg_p, *bg_s, *bg_i;
    // base: background of text-entry / list / canvas areas
    const char *base_n, *base_a, *base_p, *base_s, *base_i;
    // text: text drawn on base
    const char *text_n, *text_a, *text_p, *text_s, *text_i;
    // hovered/open menu item = inverted bar (bg) with contrasting text (fg)
    const char *mi_bg, *mi_fg;
    // text-entry field background + tooltip card
    const char *entry_bg;
    const char *tip_bg, *tip_fg;
};

// ----- LIGHT ("ancient"): WHITE background, black/greyscale widgets ----------
const chrome_colors k_ancient =
{
    /* fg   */ "#000000", "#000000", "#000000", "#ffffff", "#8a8a8a",
    /* bg   */ "#f4f4f4", "#d6d6d6", "#ffffff", "#000000", "#ececec",
    /* base */ "#ffffff", "#000000", "#ffffff", "#000000", "#f2f2f2",
    /* text */ "#000000", "#ffffff", "#000000", "#ffffff", "#9a9a9a",
    /* mi   */ "#000000", "#ffffff",
    /* entry*/ "#ffffff",
    /* tip  */ "#ffffff", "#000000"
};

// ----- MIDNIGHT: green-phosphor CRT terminal --------------------------------
//  DEEP BLACK backgrounds with bright, HIGH-CONTRAST phosphor-green text so the
//  green pops hard off the black.  Buttons lift only a hair on hover so the
//  bevel is still perceptible; selection inverts to a bright-green bar with pure
//  black text (classic CRT highlight).
const chrome_colors k_midnight =
{
    /* fg   */ "#48ff80", "#9fffc0", "#9fffc0", "#000000", "#1c8f42",
    /* bg   */ "#030d06", "#010603", "#0a1f0f", "#33ff66", "#050d08",
    /* base */ "#000000", "#33ff66", "#000000", "#33ff66", "#04120a",
    /* text */ "#48ff80", "#000000", "#9fffc0", "#000000", "#1c8f42",
    /* mi   */ "#33ff66", "#000000",
    /* entry*/ "#000000",
    /* tip  */ "#000000", "#48ff80"
};

//  Build a full RC string for one mode.  Style NAMES are identical across modes
//  so re-parsing overwrites the previous mode's definitions in place.
std::string build_rc( const chrome_colors & c )
{
    std::ostringstream o;

    // ----- base style: applied to every widget (class "*") ------------------
    o <<
    "style \"app_default\"\n"
    "{\n"
    "    font_name = \"Monospace 9\"\n"
    "    xthickness = 1\n"
    "    ythickness = 1\n"
    "    GtkButton::relief               = GTK_RELIEF_NORMAL\n"
    "    GtkButton::default_border       = { 1, 1, 1, 1 }\n"
    "    GtkButton::inner_border         = { 2, 2, 1, 1 }\n"
    "    GtkButton::child_displacement_x = 1\n"
    "    GtkButton::child_displacement_y = 1\n"
    "    GtkButton::focus_padding        = 0\n"
    "    GtkButton::focus_line_width     = 1\n"
    "    GtkWidget::interior_focus       = 1\n"
    "    GtkWidget::focus_line_width     = 1\n"
    "    GtkWidget::focus_padding        = 0\n"
    "    GtkRange::slider_width          = 14\n"
    "    GtkRange::stepper_size          = 14\n"
    "    GtkRange::trough_border         = 1\n"
    "    GtkRange::stepper_spacing       = 0\n"
    "    GtkScrollbar::min_slider_length = 20\n"
    "    GtkScale::slider_length         = 18\n"
    "    GtkCheckButton::indicator_size  = 13\n"
    "    GtkMenuBar::internal_padding    = 1\n"
    "    fg[NORMAL]        = \"" << c.fg_n   << "\"\n"
    "    fg[ACTIVE]        = \"" << c.fg_a   << "\"\n"
    "    fg[PRELIGHT]      = \"" << c.fg_p   << "\"\n"
    "    fg[SELECTED]      = \"" << c.fg_s   << "\"\n"
    "    fg[INSENSITIVE]   = \"" << c.fg_i   << "\"\n"
    "    bg[NORMAL]        = \"" << c.bg_n   << "\"\n"
    "    bg[ACTIVE]        = \"" << c.bg_a   << "\"\n"
    "    bg[PRELIGHT]      = \"" << c.bg_p   << "\"\n"
    "    bg[SELECTED]      = \"" << c.bg_s   << "\"\n"
    "    bg[INSENSITIVE]   = \"" << c.bg_i   << "\"\n"
    "    base[NORMAL]      = \"" << c.base_n << "\"\n"
    "    base[ACTIVE]      = \"" << c.base_a << "\"\n"
    "    base[PRELIGHT]    = \"" << c.base_p << "\"\n"
    "    base[SELECTED]    = \"" << c.base_s << "\"\n"
    "    base[INSENSITIVE] = \"" << c.base_i << "\"\n"
    "    text[NORMAL]      = \"" << c.text_n << "\"\n"
    "    text[ACTIVE]      = \"" << c.text_a << "\"\n"
    "    text[PRELIGHT]    = \"" << c.text_p << "\"\n"
    "    text[SELECTED]    = \"" << c.text_s << "\"\n"
    "    text[INSENSITIVE] = \"" << c.text_i << "\"\n"
    "    engine \"\" { }\n"
    "}\n\n";

    // ----- menu items: hovered/open item = inverted bar --------------------
    o <<
    "style \"app_menuitem\" = \"app_default\"\n"
    "{\n"
    "    xthickness = 1\n"
    "    ythickness = 2\n"
    "    bg[PRELIGHT]   = \"" << c.mi_bg << "\"\n"
    "    fg[PRELIGHT]   = \"" << c.mi_fg << "\"\n"
    "    base[PRELIGHT] = \"" << c.mi_bg << "\"\n"
    "    text[PRELIGHT] = \"" << c.mi_fg << "\"\n"
    "}\n\n";

    // ----- entries: crisp field, thin frame --------------------------------
    o <<
    "style \"app_entry\" = \"app_default\"\n"
    "{\n"
    "    xthickness = 2\n"
    "    ythickness = 2\n"
    "    bg[NORMAL]   = \"" << c.entry_bg << "\"\n"
    "    base[NORMAL] = \"" << c.entry_bg << "\"\n"
    "    text[NORMAL] = \"" << c.text_n   << "\"\n"
    "}\n\n";

    // ----- tooltips --------------------------------------------------------
    o <<
    "style \"app_tooltip\" = \"app_default\"\n"
    "{\n"
    "    bg[NORMAL] = \"" << c.tip_bg << "\"\n"
    "    fg[NORMAL] = \"" << c.tip_fg << "\"\n"
    "}\n\n";

    // ----- bindings: highest priority so we beat the native/system theme ---
    o <<
    "class \"*\"            style : highest \"app_default\"\n"
    "class \"GtkEntry\"     style : highest \"app_entry\"\n"
    "class \"GtkMenuItem\"  style : highest \"app_menuitem\"\n"
    "widget \"gtk-tooltip*\" style : highest \"app_tooltip\"\n"
    "widget \"gtk-tooltips\" style : highest \"app_tooltip\"\n";

    return o.str();
}

Mode g_current = ANCIENT;

} // anonymous namespace

void apply_theme( Mode m )
{
    const std::string rc = build_rc( m == MIDNIGHT ? k_midnight : k_ancient );

    //  Parse the mode's RC (overwriting the same-named styles from any previous
    //  mode) and re-realize existing widget styles so the switch is live.
    gtk_rc_parse_string( rc.c_str() );

    if ( GtkSettings * settings = gtk_settings_get_default() )
        gtk_rc_reset_styles( settings );
}

void set_mode( Mode m )
{
    g_current = m;

    //  Custom-draw palette first (cheap, no GTK), then the GTK chrome RC.
    synth::set_palette( static_cast<int>( m ) );
    apply_theme( m );
}

Mode current_mode()
{
    return g_current;
}

void apply_ancient_theme()
{
    apply_theme( ANCIENT );
}

}} // namespace seq24::theme
