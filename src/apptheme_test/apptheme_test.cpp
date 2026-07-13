//----------------------------------------------------------------------------
//  apptheme_test.cpp  -- standalone eyeball test for the switchable theme.
//
//  Builds a tiny gtkmm-2.4 window exercising BOTH surfaces the theme system
//  drives:
//    * the GTK chrome  -- a menu bar, push buttons, a text entry, a check
//      button, a horizontal scale, a scrollbar and a notebook, and
//    * a custom-drawn swatch area that paints rectangles using the runtime
//      palette (synth::cBg / cPanel / cNote / cNoteSel / cActive / cAccent /
//      cDim) so you can confirm the custom draw path flips modes too.
//
//  A "TOGGLE THEME" button calls seq24::theme::set_mode() to switch between
//  ANCIENT (grayscale) and MIDNIGHT (green CRT) live.  seq24::theme::set_mode()
//  is invoked BEFORE any widget is built, exactly as seq24 will do at startup.
//
//  It auto-quits after a few seconds so it can run unattended in the background
//  (SEQ24_THEME_TEST_SECONDS; 0 = run until closed).  SEQ24_THEME_TEST_MODE
//  (0=ancient, 1=midnight) picks the startup mode -- used to screenshot each.
//
//  Build (from an MSYS2 MINGW64 shell, mingw64 bin on PATH):
//
//    g++ -std=c++17 apptheme_test.cpp ../apptheme.cpp ../palette.cpp -I.. \
//        $(pkg-config --cflags --libs gtkmm-2.4) -o apptheme_test.exe
//
//  or with the sibling CMakeLists.txt:  cmake -S . -B build -G Ninja && ...
//----------------------------------------------------------------------------

#include <cstdlib>
#include <gtkmm.h>

#include "apptheme.h"     // seq24::theme::set_mode / current_mode / Mode
#include "ui/palette.h"   // synth::cBg ... + set_source / rounded_rect

using seq24::theme::Mode;
using seq24::theme::ANCIENT;
using seq24::theme::MIDNIGHT;

// ---- custom-drawn swatch: proves the runtime palette flips with the mode ----
class SwatchArea : public Gtk::DrawingArea
{
protected:
    bool on_expose_event(GdkEventExpose *) override
    {
        Glib::RefPtr<Gdk::Window> win = get_window();
        if (!win) return false;
        Cairo::RefPtr<Cairo::Context> cr = win->create_cairo_context();

        Gtk::Allocation a = get_allocation();
        const double w = a.get_width();
        const double h = a.get_height();

        // window/canvas background
        synth::set_source(cr, synth::cBg);
        cr->rectangle(0, 0, w, h);
        cr->fill();

        // top panel strip
        synth::set_source(cr, synth::cPanel);
        cr->rectangle(0, 0, w, 22);
        cr->fill();

        // a plain note body and a selected note
        synth::set_source(cr, synth::cNote);
        synth::rounded_rect(cr, 12, 36, 150, 26, 5);
        cr->fill();

        synth::set_source(cr, synth::cNoteSel);
        synth::rounded_rect(cr, 12, 72, 150, 26, 5);
        cr->fill();

        // accent block (root row / selection chrome)
        synth::set_source(cr, synth::cAccent);
        cr->rectangle(180, 36, 70, 26);
        cr->fill();

        // dim gridline
        synth::set_source(cr, synth::cDim, 0.85);
        cr->rectangle(0, 112, w, 2);
        cr->fill();

        // playhead (active)
        synth::set_source(cr, synth::cActive);
        cr->rectangle(270, 8, 3, h - 16);
        cr->fill();

        return true;
    }
};

// ---- widgets we need to poke on a theme toggle ------------------------------
static SwatchArea *g_swatch    = nullptr;
static Gtk::Label *g_mode_lbl  = nullptr;

static void refresh_mode_label()
{
    if (g_mode_lbl)
        g_mode_lbl->set_text(
            seq24::theme::current_mode() == MIDNIGHT ? "MODE: MIDNIGHT"
                                                     : "MODE: ANCIENT");
}

static void on_toggle_clicked()
{
    Mode next = (seq24::theme::current_mode() == ANCIENT) ? MIDNIGHT : ANCIENT;
    seq24::theme::set_mode(next);   // flips chrome (RC) AND palette in one call
    refresh_mode_label();
    if (g_swatch) g_swatch->queue_draw();   // repaint custom area with new palette
}

int main(int argc, char *argv[])
{
    Gtk::Main kit(argc, argv);

    //  Pick startup mode, then install it BEFORE any widget exists.
    Mode start = ANCIENT;
    if (const char *mv = std::getenv("SEQ24_THEME_TEST_MODE"))
        start = (std::atoi(mv) == 1) ? MIDNIGHT : ANCIENT;
    seq24::theme::set_mode(start);

    Gtk::Window window;
    window.set_title("seq24 theme -- monochrome mode switch test");
    window.set_default_size(520, 460);

    Gtk::VBox *root = Gtk::manage(new Gtk::VBox(false, 0));
    window.add(*root);

    // ---- menu bar ---------------------------------------------------------
    Gtk::MenuBar *menubar = Gtk::manage(new Gtk::MenuBar());
    {
        using namespace Gtk::Menu_Helpers;

        Gtk::Menu *file_menu = Gtk::manage(new Gtk::Menu());
        file_menu->items().push_back(MenuElem("_New"));
        file_menu->items().push_back(MenuElem("_Open..."));
        file_menu->items().push_back(SeparatorElem());
        file_menu->items().push_back(MenuElem("_Quit",
            sigc::ptr_fun(&Gtk::Main::quit)));

        Gtk::Menu *view_menu = Gtk::manage(new Gtk::Menu());
        view_menu->items().push_back(MenuElem("_Toggle Theme",
            sigc::ptr_fun(&on_toggle_clicked)));
        view_menu->items().push_back(CheckMenuElem("_Snap to grid"));

        Gtk::MenuItem *file_item = Gtk::manage(new Gtk::MenuItem("_File", true));
        file_item->set_submenu(*file_menu);
        Gtk::MenuItem *view_item = Gtk::manage(new Gtk::MenuItem("_View", true));
        view_item->set_submenu(*view_menu);
        Gtk::MenuItem *help_item = Gtk::manage(new Gtk::MenuItem("_Help", true));

        menubar->items().push_back(*file_item);
        menubar->items().push_back(*view_item);
        menubar->items().push_back(*help_item);
    }
    root->pack_start(*menubar, Gtk::PACK_SHRINK);

    Gtk::VBox *body = Gtk::manage(new Gtk::VBox(false, 6));
    body->set_border_width(8);
    root->pack_start(*body, Gtk::PACK_EXPAND_WIDGET);

    // ---- toggle button + mode label --------------------------------------
    Gtk::HBox *top_row = Gtk::manage(new Gtk::HBox(false, 6));
    Gtk::Button *b_toggle = Gtk::manage(new Gtk::Button("TOGGLE THEME"));
    b_toggle->signal_clicked().connect(sigc::ptr_fun(&on_toggle_clicked));
    g_mode_lbl = Gtk::manage(new Gtk::Label());
    top_row->pack_start(*b_toggle, Gtk::PACK_SHRINK);
    top_row->pack_start(*g_mode_lbl, Gtk::PACK_SHRINK);
    body->pack_start(*top_row, Gtk::PACK_SHRINK);

    // ---- transport buttons ------------------------------------------------
    Gtk::HBox *btn_row = Gtk::manage(new Gtk::HBox(false, 6));
    Gtk::Button *b_play = Gtk::manage(new Gtk::Button("PLAY"));
    Gtk::Button *b_stop = Gtk::manage(new Gtk::Button("STOP"));
    Gtk::Button *b_rec  = Gtk::manage(new Gtk::Button("REC"));
    b_rec->set_sensitive(false);   // show the INSENSITIVE color
    btn_row->pack_start(*b_play);
    btn_row->pack_start(*b_stop);
    btn_row->pack_start(*b_rec);
    body->pack_start(*btn_row, Gtk::PACK_SHRINK);

    // ---- entry + check button --------------------------------------------
    Gtk::HBox *ent_row = Gtk::manage(new Gtk::HBox(false, 6));
    Gtk::Entry *entry = Gtk::manage(new Gtk::Entry());
    entry->set_text("PATTERN 01");
    Gtk::CheckButton *chk = Gtk::manage(new Gtk::CheckButton("LOOP"));
    chk->set_active(true);
    ent_row->pack_start(*entry, Gtk::PACK_EXPAND_WIDGET);
    ent_row->pack_start(*chk, Gtk::PACK_SHRINK);
    body->pack_start(*ent_row, Gtk::PACK_SHRINK);

    // ---- scale + scrollbar ------------------------------------------------
    Gtk::HScale *scale = Gtk::manage(new Gtk::HScale(0.0, 128.0, 1.0));
    scale->set_value(96.0);
    scale->set_value_pos(Gtk::POS_RIGHT);
    body->pack_start(*scale, Gtk::PACK_SHRINK);

    Gtk::Adjustment *adj = Gtk::manage(new Gtk::Adjustment(30, 0, 100, 1, 10, 20));
    Gtk::HScrollbar *scroll = Gtk::manage(new Gtk::HScrollbar(*adj));
    body->pack_start(*scroll, Gtk::PACK_SHRINK);

    // ---- custom-drawn swatch inside a notebook ----------------------------
    Gtk::Notebook *nb = Gtk::manage(new Gtk::Notebook());
    SwatchArea *swatch = Gtk::manage(new SwatchArea());
    g_swatch = swatch;
    Gtk::Label *p2 = Gtk::manage(new Gtk::Label("\n  monochrome page TWO  \n"));
    nb->append_page(*swatch, "DRAW");
    nb->append_page(*p2, "MIX");
    body->pack_start(*nb, Gtk::PACK_EXPAND_WIDGET);

    refresh_mode_label();

    window.show_all_children();
    window.show();
    window.present();

    // ---- auto-quit so the harness can run us unattended -------------------
    int seconds = 6;
    if (const char *env = std::getenv("SEQ24_THEME_TEST_SECONDS"))
        seconds = std::atoi(env);
    if (seconds > 0)
        Glib::signal_timeout().connect_seconds_once(
            sigc::ptr_fun(&Gtk::Main::quit), seconds);

    Gtk::Main::run(window);
    return 0;
}
