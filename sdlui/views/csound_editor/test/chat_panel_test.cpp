//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/test/chat_panel_test.cpp
//
//  Headless selftest for the Csound editor's Claude chat panel.  Runs under
//  SDL_VIDEODRIVER=dummy (forced below unless overridden); clear/present stay
//  in the loop -- omitting present produces phantom SDL_DestroyTexture stalls.
//  Everything except the live HTTPS call is exercised: synthetic SSE through
//  ClaudeClient::decodeSseChunk, ChatEvents driven straight into the panel,
//  apply/undo through the editor, and frame times measured idle vs streaming.
//
//    cmake -S sdlui/views/csound_editor/test -B <dir>
//    cmake --build <dir> -j && <dir>/chat_panel_test
//----------------------------------------------------------------------------
#include "gui.h"
#include "views/csound_editor/csound_editor_view.h"
#include "views/csound_editor/chat_panel.h"
#include "views/csound_editor/ai_settings_view.h"
#include "views/csound_editor/ai_prefs.h"
#include "engine/ai/claude_client.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ui;
using PatchKnob::ai::ChatEvent;
using PatchKnob::ai::ClaudeClient;
using PatchKnob::ai::EventKind;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) ++g_pass; else ++g_fail;
}

static void click(App& app, Widget& w, int x, int y) {
    MouseEv down{ x, y, SDL_BUTTON_LEFT, true };
    MouseEv up  { x, y, SDL_BUTTON_LEFT, false };
    w.on_mouse(app, down);
    w.on_mouse(app, up);
}

//----------------------------------------------------------------------------
//  A. apply + undo
//----------------------------------------------------------------------------
static void test_undo(App& app) {
    std::printf("--- apply / undo ---\n");
    CsoundEditorView v;
    v.rect = SDL_Rect{ 0, 0, 800, 560 };

    const std::string orig =
        "<CsoundSynthesizer>\n<CsInstruments>\nsr = 44100\ninstr 1\n"
        " a1 oscili 0.2, 440\n out a1\nendin\n</CsInstruments>\n"
        "</CsoundSynthesizer>";
    v.setText(orig);
    check(v.text() == orig, "setText/text round-trips exactly");
    check(!v.on_undo(app, false), "fresh document: Ctrl+Z falls through to project undo");

    const std::string code = "instr 2\n a2 vco2 0.1, 110\n out a2\nendin\n";
    v.applyAiCode(app, code, /*replaceAll=*/true);
    check(v.text() == "instr 2\n a2 vco2 0.1, 110\n out a2\nendin\n",
          "apply(replace) puts exactly the block in the buffer");
    check(v.on_undo(app, false), "Ctrl+Z after apply is consumed by the editor");
    check(v.text() == orig, "one Ctrl+Z restores EXACTLY the prior buffer");
    check(v.on_undo(app, true), "redo is consumed");
    check(v.text() == "instr 2\n a2 vco2 0.1, 110\n out a2\nendin\n",
          "redo reinstates the applied code");
    v.on_undo(app, false);   // back to orig for the next check

    //  insert at cursor (cursor is at 0,0 after setText)
    v.applyAiCode(app, "; new line\n", /*replaceAll=*/false);
    check(v.text() == "; new line\n" + orig, "apply(insert) lands at the cursor");
    check(v.on_undo(app, false) && v.text() == orig,
          "one Ctrl+Z removes the whole insertion");

    //  typing coalesces into one step
    v.insert("a"); v.insert("b"); v.insert("c");
    check(v.text() == "abc" + orig, "typed characters land");
    check(v.on_undo(app, false) && v.text() == orig,
          "a typing burst undoes as ONE step");

    //  a click breaks the burst ('@'/'#' cannot occur in the document)
    v.insert("@");
    v.on_mouse(app, MouseEv{ 60, 8, SDL_BUTTON_LEFT, true });
    v.on_mouse(app, MouseEv{ 60, 8, SDL_BUTTON_LEFT, false });
    v.insert("#");
    v.on_undo(app, false);
    check(v.text().find('@') != std::string::npos && v.text().find('#') == std::string::npos,
          "a mouse click splits typing into separate undo steps");

    v.setText(orig);
    check(!v.on_undo(app, false), "setText clears the local history (rebind, not edit)");
}

//----------------------------------------------------------------------------
//  B. synthetic SSE -> decodeSseChunk -> panel
//----------------------------------------------------------------------------
static std::vector<ChatEvent> decodeAll(const std::string& sse, size_t chunkLen) {
    std::vector<ChatEvent> evs;
    std::string carry;
    for (size_t i = 0; i < sse.size(); i += chunkLen)
        ClaudeClient::decodeSseChunk(sse.substr(i, chunkLen), carry, evs);
    return evs;
}

static void test_sse_stream(App& app) {
    std::printf("--- synthetic SSE -> panel ---\n");
    CsoundChatPanel p;
    p.setKeyLoaderForTest([]() {
        PatchKnob::ai::KeyLoad kl;
        kl.status = PatchKnob::ai::KeyStatus::Ok;
        kl.key    = "sk-ant-test";
        return kl;
    });
    p.reloadSettings();

    const std::string reply =
        "Here you go:\n\n```csound\ninstr 1\n a1 oscili 0.2, 440\n out a1\nendin\n```\nDone.";
    //  Build the SSE the API would send, deltas split mid-escape on purpose.
    std::string sse;
    sse += "event: content_block_delta\n";
    sse += "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"plan the patch\"}}\n\n";
    for (size_t i = 0; i < reply.size(); i += 17) {
        std::string frag = reply.substr(i, 17);
        std::string esc;
        for (char c : frag) {
            if (c == '"') esc += "\\\"";
            else if (c == '\n') esc += "\\n";
            else if (c == '\\') esc += "\\\\";
            else esc += c;
        }
        sse += "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"" + esc + "\"}}\n";
    }
    sse += "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n";

    //  Feed in awkward 7-byte chunks, as the wire would.
    const std::vector<ChatEvent> evs = decodeAll(sse, 7);
    bool sawDelta = false, sawThink = false, sawDone = false;
    for (const ChatEvent& e : evs) {
        if (e.kind == EventKind::Delta) sawDelta = true;
        if (e.kind == EventKind::Thinking) sawThink = true;
        if (e.kind == EventKind::Done) sawDone = true;
        p.injectEventForTest(e);
    }
    check(sawDelta && sawThink && sawDone, "decoder produced Delta+Thinking+Done");
    check(p.entryCountForTest() == 1, "one assistant entry created");
    check(p.entryTextForTest(0) == reply, "streamed text reassembles exactly");
    check(p.entryThinkingForTest(0) == "plan the patch", "thinking captured separately");
    check(!p.entryStreamingForTest(0), "Done ends the streaming state");

    const SDL_Rect panel{ 0, 0, 380, 500 };
    const std::vector<std::string>& blocks = p.entryCodeBlocksForTest(app, panel, 0);
    const std::vector<std::string> viaEngine = ClaudeClient::extractCodeBlocks(reply, "csound");
    check(blocks.size() == 1 && viaEngine.size() == 1 && blocks[0] == viaEngine[0],
          "panel's Apply block matches ClaudeClient::extractCodeBlocks exactly");
}

static void test_audit_flow(App& app) {
    std::printf("--- audit / retry / unverified flow ---\n");
    CsoundChatPanel p;
    auto delta = [](const std::string& t) {
        ChatEvent e; e.kind = EventKind::Delta; e.text = t; return e;
    };
    //  Round 1: flawed code, 2 issues, engine retries.
    p.injectEventForTest(delta("```csound\ninstr 1\n a1 oscili\nendin\n```\n"));
    { ChatEvent e; e.kind = EventKind::Checking; e.text = "Checking opcode types..."; p.injectEventForTest(e); }
    { ChatEvent e; e.kind = EventKind::Checked; e.issues = 2; e.text = "line 2: oscili needs args"; p.injectEventForTest(e); }
    { ChatEvent e; e.kind = EventKind::Retry; e.text = "Asking for a correction..."; p.injectEventForTest(e); }
    //  Round 2: corrected code, clean, Done.
    p.injectEventForTest(delta("```csound\ninstr 1\n a1 oscili 0.2, 440\n out a1\nendin\n```\n"));
    { ChatEvent e; e.kind = EventKind::Checked; e.issues = 0; e.text = "Opcode types and usage check out against the manual."; p.injectEventForTest(e); }
    { ChatEvent e; e.kind = EventKind::Done; e.stopReason = "end_turn"; p.injectEventForTest(e); }

    check(p.entryCountForTest() == 2, "retry opened a second entry");
    check(p.entrySupersededForTest(0), "the flawed reply is marked superseded");
    check(!p.entrySupersededForTest(1), "the correction is live");
    check(p.entryCheckIssuesForTest(0) == 2, "issue count kept on the flawed reply");
    check(p.entryCheckIssuesForTest(1) == 0, "clean verdict on the correction");
    check(p.entryCheckNoteForTest(1).find("check out") != std::string::npos,
          "the clean audit verdict is shown, not silent");

    const SDL_Rect panel{ 0, 0, 380, 500 };
    check(p.entryCodeBlocksForTest(app, panel, 0).empty(),
          "superseded code is NOT offered for Apply");
    check(p.entryCodeBlocksForTest(app, panel, 1).size() == 1,
          "the corrected code IS offered for Apply");

    //  Exhausted retries: Checked(issues) then Error then Done -- block stays
    //  applyable but flagged unverified (checkIssues > 0).
    CsoundChatPanel q;
    q.injectEventForTest(delta("```csound\ninstr 9\n a1 wrongop 1\nendin\n```\n"));
    { ChatEvent e; e.kind = EventKind::Checked; e.issues = 1; e.text = "line 2: no such opcode"; q.injectEventForTest(e); }
    { ChatEvent e; e.kind = EventKind::Error; e.text = "still does not match the manual"; q.injectEventForTest(e); }
    { ChatEvent e; e.kind = EventKind::Done; e.stopReason = "end_turn"; q.injectEventForTest(e); }
    check(q.entryCheckIssuesForTest(0) == 1 && !q.entrySupersededForTest(0),
          "exhausted retry: reply kept, marked unverified");
    check(q.entryCodeBlocksForTest(app, panel, 0).size() == 1,
          "unverified code still has (flagged) Apply");
    check(q.entryErrorForTest(q.entryCountForTest() - 1),
          "the engine's final Error is shown in the panel");
}

static void setBackendPref(bool cli) {
    aiprefs::Prefs p = aiprefs::load();
    p.useCli = cli;
    aiprefs::save(p);
}

static void test_key_states() {
    std::printf("--- key states (API backend) ---\n");
    //  Key-status notices exist only on the API backend; the default (CLI)
    //  backend never involves a key at all.
    setBackendPref(false);
    CsoundChatPanel p;
    p.setKeyLoaderForTest([]() {
        PatchKnob::ai::KeyLoad kl;
        kl.status = PatchKnob::ai::KeyStatus::ForeignMachine;
        return kl;
    });
    p.reloadSettings();
    const std::string n = p.noticeForTest();
    check(n.find("DIFFERENT machine") != std::string::npos,
          "ForeignMachine is stated plainly, not treated as missing");
    check(n.find("replace") != std::string::npos,
          "...and the notice offers replacement");
    check(n.find("fingerprint") != std::string::npos,
          "...showing this machine's fingerprint");

    CsoundChatPanel m;
    m.setKeyLoaderForTest([]() { return PatchKnob::ai::KeyLoad(); });   // Missing
    m.reloadSettings();
    check(m.noticeForTest().find("No Anthropic API key") != std::string::npos,
          "Missing key notice");

    //  CLI backend (the default): the key status is IRRELEVANT.  Whether or
    //  not `claude` is installed on this box, the notice must never send the
    //  user hunting for an API key.
    std::printf("--- key states (CLI backend) ---\n");
    setBackendPref(true);
    CsoundChatPanel c;
    c.setKeyLoaderForTest([]() { return PatchKnob::ai::KeyLoad(); });  // Missing
    c.reloadSettings();
    const std::string cn = c.noticeForTest();
    check(cn.find("API key") == std::string::npos,
          "CLI backend never mentions API keys (key status irrelevant)");
    check(cn.empty() || cn.find("Claude Code") != std::string::npos,
          "CLI notice is empty (claude found) or names Claude Code as missing");

    CsoundChatPanel f;   // ForeignMachine + CLI: still no key talk
    f.setKeyLoaderForTest([]() {
        PatchKnob::ai::KeyLoad kl;
        kl.status = PatchKnob::ai::KeyStatus::ForeignMachine;
        return kl;
    });
    f.reloadSettings();
    check(f.noticeForTest().find("API key") == std::string::npos,
          "foreign key does not surface on the CLI backend");
}

static void test_cli_stream(App& app) {
    std::printf("--- CLI NDJSON -> panel ---\n");
    //  The CLI backend's worker feeds decodeCliLine; drive the same decoder
    //  here and hand the events to the panel, so the panel is proven against
    //  both wire shapes.
    CsoundChatPanel p;
    const char* lines[] = {
        "{\"type\":\"system\",\"subtype\":\"init\"}",
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
          "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"pick a waveform\"}}}",
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
          "\"delta\":{\"type\":\"text_delta\",\"text\":\"```csound\\ninstr 1\\n\"}}}",
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
          "\"delta\":{\"type\":\"text_delta\",\"text\":\" out oscili(0.2, 440)\\nendin\\n```\\n\"}}}",
        "{\"type\":\"result\",\"is_error\":false,\"stop_reason\":\"end_turn\"}",
    };
    for (const char* l : lines) {
        std::vector<ChatEvent> evs;
        ClaudeClient::decodeCliLine(l, evs);
        for (const ChatEvent& e : evs) p.injectEventForTest(e);
    }
    check(p.entryCountForTest() == 1, "CLI stream lands in one entry");
    check(p.entryThinkingForTest(0) == "pick a waveform", "CLI thinking captured");
    check(!p.entryStreamingForTest(0), "CLI result line ends the stream");
    const SDL_Rect panel{ 0, 0, 380, 500 };
    check(p.entryCodeBlocksForTest(app, panel, 0).size() == 1,
          "CLI-delivered fence gets its Apply block");
}

static void test_rate_limit() {
    std::printf("--- CLI rate limit reads as a wait ---\n");
    CsoundChatPanel p;
    ChatEvent e;
    e.kind = EventKind::Error;
    e.text = "Claude Code is rate limited right now. Wait for the limit to "
             "reset and try again.";
    p.injectEventForTest(e);
    ChatEvent d; d.kind = EventKind::Done; d.stopReason = "error";
    p.injectEventForTest(d);
    check(p.entryErrorForTest(0), "rate limit is still excluded from history");
    check(p.entryRateLimitForTest(0), "...but flagged as a wait, not a crash");
}

//----------------------------------------------------------------------------
//  C. settings view against a fake store
//----------------------------------------------------------------------------
static void test_settings(App& app) {
    std::printf("--- settings view (fake store) ---\n");
    AiSettingsView s;
    std::string stored;
    bool changed = false;
    s.load_key  = [&]() {
        PatchKnob::ai::KeyLoad kl;
        kl.status = stored.empty() ? PatchKnob::ai::KeyStatus::Missing
                                   : PatchKnob::ai::KeyStatus::Ok;
        kl.key = stored;
        return kl;
    };
    s.save_key  = [&](const std::string& k, std::string*) { stored = k; return true; };
    s.clear_key = [&]() { const bool had = !stored.empty(); stored.clear(); return had; };
    s.on_changed = [&]() { changed = true; };

    s.setEntryForTest("hello world");
    s.saveForTest(app);
    check(stored.empty(), "junk is rejected before it is ever stored");
    check(s.feedbackForTest().find("does not look like") != std::string::npos,
          "...with the reason shown");

    s.setEntryForTest("  sk-ant-api03-abcdefghijklmnopqrstuvwxyz0123456789  ");
    s.saveForTest(app);
    check(stored == "sk-ant-api03-abcdefghijklmnopqrstuvwxyz0123456789",
          "a real-looking key is trimmed and saved");
    check(changed, "on_changed fired so the chat reloads");
    check(s.feedbackForTest().find("saved") != std::string::npos, "save confirmed");

    s.clearForTest(app);
    check(stored.empty(), "clear removes the stored key");

    //  prefs round trip (redirected file; the user's real prefs untouched)
    aiprefs::Prefs p;
    p.model = PatchKnob::ai::kModelSonnet;
    p.thinking = false;
    p.maxTokens = 16384;
    check(aiprefs::save(p), "prefs save");
    const aiprefs::Prefs q = aiprefs::load();
    check(q.model == PatchKnob::ai::kModelSonnet && !q.thinking && q.maxTokens == 16384,
          "prefs round-trip (model/thinking/max tokens)");
    aiprefs::Prefs def; aiprefs::save(def);   // leave the test file sane
}

//----------------------------------------------------------------------------
//  D. frame time: idle vs streaming, and long-transcript culling
//----------------------------------------------------------------------------
struct FrameStats { double meanMs, p95Ms, maxMs; };

//! Times the CPU side of the frame -- clear + pump + draw, i.e. the work this
//! panel adds -- while STILL presenting every frame (the dummy driver's
//! software present is vsync-throttled to ~16.7 ms, which would drown any
//! signal, and omitting present produces the known phantom-stall false lead).
template <typename PerFrame>
static FrameStats run_frames(App& app, CsoundEditorView& v, int frames, PerFrame f) {
    std::vector<double> ms;
    ms.reserve((size_t)frames);
    const double freq = (double)SDL_GetPerformanceFrequency();
    for (int i = 0; i < frames; ++i) {
        const Uint64 t0 = SDL_GetPerformanceCounter();
        f(i);
        SDL_SetRenderDrawColor(app.ren, 0, 0, 0, 255);
        SDL_RenderClear(app.ren);
        v.pumpAi(app, true);
        v.draw(app);
        const Uint64 t1 = SDL_GetPerformanceCounter();
        SDL_RenderPresent(app.ren);
        ms.push_back((double)(t1 - t0) * 1000.0 / freq);
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double m : ms) sum += m;
    FrameStats st;
    st.meanMs = sum / (double)ms.size();
    st.p95Ms  = ms[(size_t)((ms.size() - 1) * 95 / 100)];
    st.maxMs  = ms.back();
    return st;
}

static void test_perf(App& app) {
    std::printf("--- frame time: idle vs streaming ---\n");
    CsoundEditorView v;
    v.rect = SDL_Rect{ 0, 0, app.w, app.h };
    std::string csd = "<CsoundSynthesizer>\n<CsInstruments>\n";
    for (int i = 0; i < 200; ++i)
        csd += "; line " + std::to_string(i) + " of a plausible working csd\n";
    csd += "</CsInstruments>\n</CsoundSynthesizer>\n";
    v.setText(csd);
    v.aiChat().setKeyLoaderForTest([]() {
        PatchKnob::ai::KeyLoad kl;
        kl.status = PatchKnob::ai::KeyStatus::Ok;
        kl.key = "sk-ant-test";
        return kl;
    });
    v.aiChat().reloadSettings();

    //  Open the chat panel the way a user would: click its tab.
    const int tabCx = v.rect.x + v.rect.w - 26 + 11;
    const int tabCy = v.rect.y + 4 + 22 + 2 + 11;
    click(app, v, tabCx, tabCy);

    //  Warm-up, then measure IDLE with the panel open (empty transcript).
    run_frames(app, v, 30, [](int) {});
    const FrameStats idleEmpty = run_frames(app, v, 300, [](int) {});

    //  Streaming: one Delta injected per frame -- the worst realistic rate --
    //  into a growing reply with a fenced code block in the middle.
    const FrameStats stream = run_frames(app, v, 300, [&](int i) {
        ChatEvent e;
        e.kind = EventKind::Delta;
        e.text = (i == 100) ? "\n```csound\ninstr 1\n" :
                 (i == 200) ? "endin\n```\n" :
                 "and some more delta text arriving from the model ";
        v.aiChat().injectEventForTest(e);
    });
    { ChatEvent d; d.kind = EventKind::Done; d.stopReason = "end_turn"; v.aiChat().injectEventForTest(d); }

    //  The honest control: the SAME pixels, no reply in flight.  Comparing
    //  streaming against an EMPTY panel would count "there is text on screen
    //  now" as streaming cost.
    const FrameStats idleFull = run_frames(app, v, 300, [](int) {});

    std::printf("  idle (empty panel) : mean %.3f ms  p95 %.3f ms  max %.3f ms\n",
                idleEmpty.meanMs, idleEmpty.p95Ms, idleEmpty.maxMs);
    std::printf("  idle (same content): mean %.3f ms  p95 %.3f ms  max %.3f ms\n",
                idleFull.meanMs, idleFull.p95Ms, idleFull.maxMs);
    std::printf("  streaming          : mean %.3f ms  p95 %.3f ms  max %.3f ms\n",
                stream.meanMs, stream.p95Ms, stream.maxMs);
    check(stream.meanMs < idleFull.meanMs + 0.5,
          "streaming frames are indistinguishable from idle (< +0.5 ms mean vs same content)");
    check(stream.p95Ms < 5.0, "streaming p95 stays under 5 ms");

    //  Long transcript: 300 finished replies, then check culling + cost.
    std::printf("--- long transcript culling ---\n");
    for (int n = 0; n < 300; ++n) {
        ChatEvent e;
        e.kind = EventKind::Delta;
        e.text = "reply " + std::to_string(n) +
                 ": a paragraph of explanation text that wraps across several"
                 " rows in the panel, followed by\n```csound\ninstr 1\n"
                 " a1 oscili 0.2, 440\n out a1\nendin\n```\nmore prose.\n";
        v.aiChat().injectEventForTest(e);
        ChatEvent d; d.kind = EventKind::Done; d.stopReason = "end_turn";
        v.aiChat().injectEventForTest(d);
    }
    const FrameStats longT = run_frames(app, v, 200, [](int) {});
    std::printf("  300-entry transcript: mean %.3f ms  p95 %.3f ms  max %.3f ms  rowsDrawn %d\n",
                longT.meanMs, longT.p95Ms, longT.maxMs, v.aiChat().rowsDrawnForTest());
    check(v.aiChat().rowsDrawnForTest() > 0 &&
          v.aiChat().rowsDrawnForTest() < 120,
          "only visible rows are drawn (culled transcript)");
    check(longT.meanMs < idleFull.meanMs + 1.0,
          "300 entries cost no more than one (+1 ms vs same-viewport idle)");

    //  Hidden-panel pump: events arriving while nothing is shown must still
    //  be drained (no strand) and must not crash or redraw-storm.
    click(app, v, tabCx, tabCy);            // close the panel
    ChatEvent e;
    e.kind = EventKind::Delta;
    e.text = "arrives while hidden";
    v.aiChat().injectEventForTest(e);
    v.pumpAi(app, false);
    ChatEvent d; d.kind = EventKind::Done; d.stopReason = "end_turn";
    v.aiChat().injectEventForTest(d);
    v.pumpAi(app, false);
    const int last = v.aiChat().entryCountForTest() - 1;
    check(v.aiChat().entryTextForTest(last).find("arrives while hidden") != std::string::npos &&
          !v.aiChat().entryStreamingForTest(last),
          "a reply landing with the panel hidden is drained, not stranded");
}

//----------------------------------------------------------------------------

int main(int, char**) {
    //  Headless by default; an externally exported SDL_VIDEODRIVER wins.
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 0);
    //  NEVER touch the user's real prefs from the harness.
    aiprefs::set_path_for_test("chat_panel_test_prefs.tmp");

    App app;
    if (!app.init("chat harness")) {
        std::fprintf(stderr, "App::init failed\n");
        return 2;
    }

    test_undo(app);
    test_sse_stream(app);
    test_audit_flow(app);
    test_key_states();
    test_cli_stream(app);
    test_rate_limit();
    test_settings(app);
    test_perf(app);

    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    const int rc = g_fail == 0 ? 0 : 1;
    std::remove("chat_panel_test_prefs.tmp");   // leave no droppings in cwd
    app.shutdown();
    return rc;
}
