//----------------------------------------------------------------------------
//  apptheme.h  -- seq24 Windows port: runtime, switchable global GTK theme.
//
//  seq24 uses gtkmm-2.4 (GTK+ 2.24).  GTK+2 is themed at runtime through its RC
//  mechanism, so the entire application chrome -- menus, buttons, entries,
//  scrollbars, notebook tabs, scales, check buttons -- can be re-skinned live by
//  parsing an RC string with gtk_rc_parse_string() and then re-realizing widget
//  styles with gtk_rc_reset_styles().
//
//  This module provides a small THEME SYSTEM with switchable monochrome modes.
//  Each mode drives BOTH surfaces of the app in lockstep:
//    * the GTK chrome, via an RC string parsed here, and
//    * the custom-drawn widgets, via the runtime palette in ui/palette.h.
//
//  MODES (all strictly monochrome -- exactly one hue family, never a foreign
//  color):
//    ANCIENT  : grayscale black & white, early-90s workstation / Motif look.
//    MIDNIGHT : green-phosphor CRT terminal -- near-black background, phosphor
//               green foreground / text / borders, bright-green selection.
//
//  Typical use (done by the coordinator, not here):
//    at startup, once Gtk::Main exists and before the main window is built:
//        seq24::theme::set_mode(seq24::theme::ANCIENT);
//    from a "View > Theme" menu at any time:
//        seq24::theme::set_mode(seq24::theme::MIDNIGHT);   // live restyle
//----------------------------------------------------------------------------
#ifndef SEQ24_APPTHEME_H
#define SEQ24_APPTHEME_H

namespace seq24 { namespace theme {

//  Theme modes.  Values are kept numerically identical to synth::PaletteMode
//  (ui/palette.h) so a Mode can be passed straight through to the palette.
enum Mode
{
    ANCIENT  = 0,   // grayscale black & white (default)
    MIDNIGHT = 1    // green-phosphor CRT terminal
};

//  Switch the whole app to mode m: set the custom-draw palette table AND reload
//  the GTK chrome RC, re-realizing existing widgets so the change is live.
//  Call once at startup (before the main window) and again on any user toggle.
void set_mode(Mode m);

//  The currently active mode (defaults to ANCIENT until set_mode() is called).
Mode current_mode();

//  Reload only the GTK chrome RC for mode m and re-realize widget styles.
//  set_mode() calls this after updating the palette; exposed for completeness.
void apply_theme(Mode m);

//  Back-compat convenience: identical to apply_theme(ANCIENT) (chrome only).
void apply_ancient_theme();

}} // namespace seq24::theme

#endif // SEQ24_APPTHEME_H
