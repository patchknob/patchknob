#include "claude_client.h"

#include "cli_backend.h"
#include "csound_check.h"
#include "http_client.h"
#include "json.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace PatchKnob { namespace ai {

const char* const kModelOpus   = "claude-opus-5";
const char* const kModelSonnet = "claude-sonnet-5";
const char* const kModelHaiku  = "claude-haiku-4-5";

namespace {
const char* const kUrl     = "https://api.anthropic.com/v1/messages";
const char* const kVersion = "anthropic-version: 2023-06-01";

//  Truncate from the FRONT for source, keeping the tail: when a buffer is too
//  big to send whole, the part the user is working on is almost always the end.
std::string clampSource(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    return "[... " + std::to_string(s.size() - maxBytes) +
           " earlier bytes omitted ...]\n" + s.substr(s.size() - maxBytes);
}
} // namespace

ClaudeClient::ClaudeClient() : model_(kModelOpus) {}
ClaudeClient::~ClaudeClient() { cancel_.store(true); join(); }

void ClaudeClient::join() {
    if (worker_.joinable()) worker_.join();
}

std::string ClaudeClient::buildSystemPrompt(const std::string& orc,
                                            const std::string& errors) {
    std::string s =
        "You are embedded in PatchKnob, a digital audio workstation, as the "
        "assistant inside its Csound editor window. The user is writing Csound "
        "and hears the result immediately in the DAW.\n"
        "\n"
        "How to answer:\n"
        "- Emit Csound in fenced blocks tagged `csound`. The editor's \"Apply\" "
        "button takes the code from those fences, so anything you want the user "
        "to run must be inside one, and anything you do NOT want pasted into "
        "their document must be outside one.\n"
        "- When asked for a whole document, write a complete .csd: "
        "<CsoundSynthesizer>, <CsOptions>, <CsInstruments> with sr/ksmps/nchnls/"
        "0dbfs, <CsScore>, and the closing tags.\n"
        "- When asked for one instrument, emit just that instr block so it can "
        "be dropped into the existing orchestra.\n"
        "- Keep the user's existing sr, ksmps, nchnls and 0dbfs unless they ask "
        "otherwise, and match the naming and style already in their buffer.\n"
        "- Guard against blowing up monitors: scale oscillator output sensibly "
        "against 0dbfs and give every instrument an envelope so notes do not "
        "click.\n"
        "- Be brief in prose. The code is the deliverable.\n"
        "\n"
        "HOW PATCHKNOB RUNS YOUR INSTRUMENT -- match this or it will not play:\n"
        "- Instruments are driven by LIVE MIDI from the host, not by score notes.\n"
        "  The host opens Csound with -M0 and feeds it real MIDI bytes, so an\n"
        "  instrument the user can play from a keyboard or a clip must be\n"
        "  MIDI-activated. Write `massign <channel>, <instr>` and take pitch and\n"
        "  velocity from MIDI: `icps cpsmidi` and `iamp ampmidi <scale>`.\n"
        "- `massign 0, N` claims EVERY MIDI channel for instrument N. In a\n"
        "  document with more than one instrument that silently steals the others'\n"
        "  input -- the last massign wins. With several instruments give each its\n"
        "  own channel: `massign 1, 1` / `massign 2, 2`.\n"
        "- Use MIDI-aware envelopes so a note ends when the key is released:\n"
        "  `madsr` / `mxadsr` respond to note-off, and `xtratim` holds the voice\n"
        "  long enough for the release tail. A plain `linseg` against p3 does not\n"
        "  end on note-off, so the note hangs.\n"
        "- The host is STEREO: keep `nchnls = 2`, `nchnls_i = 2`, `0dbfs = 1`, and\n"
        "  write output with `outs aL, aR`. For an EFFECT, read the host's input\n"
        "  with `ins` (or `inch 1` / `inch 2`) rather than generating sound.\n"
        "- If you emit a whole document, its <CsScore> needs `f 0 86400`. Without\n"
        "  it performance ends at once and a live MIDI instrument never sounds.\n"
        "- The host forces the sample rate, so do not rely on `sr` being what you\n"
        "  wrote. `ksmps = 32` is the host default.\n"
        "- Score p-fields still work for testing, but if you use p4/p5 for pitch\n"
        "  and amplitude, ALSO provide the MIDI path, or the instrument is silent\n"
        "  when the user plays it.\n"
        "\n"
        "Your code is checked automatically after you send it: every opcode is "
        "validated against the Csound manual's own signatures, and the result is "
        "then COMPILED by Csound. If either finds a problem you will be shown it "
        "and asked to fix it, so getting it right matters more than getting it "
        "fast.\n"
        "\n"
        "You have WebSearch and WebFetch. Use them when the DSP is the hard part "
        "rather than the syntax -- filter topologies, FM ratios and index "
        "scaling, reverb and delay network design, antialiasing, envelope "
        "shapes, or any opcode whose argument meaning you are not certain of. "
        "Looking it up and getting the coefficients right is better than "
        "producing something that compiles and sounds wrong.\n";

    if (!orc.empty()) {
        s += "\nThe editor currently contains this Csound document. Treat it as "
             "the source of truth for what the user is looking at:\n\n"
             "```csound\n" + clampSource(orc, 120000) + "\n```\n";
        //  Spelling out the taken instrument numbers matters more than it looks:
        //  the default document defines instr 1, so a model that writes `instr 1`
        //  -- the obvious choice -- fails to compile every single time.
        s += describeHostDocument(orc);
    } else {
        s += "\nThe editor is currently empty.\n";
    }
    if (!errors.empty()) {
        s += "\nThe most recent compile produced this output. If it contains "
             "errors, diagnose them against the document above and give the "
             "corrected code:\n\n```\n" + clampSource(errors, 16000) + "\n```\n";
    }
    return s;
}

std::string ClaudeClient::buildRequestBody(const std::vector<ChatMessage>& history,
                                           const std::string& orc,
                                           const std::string& errors) const {
    std::string b = "{";
    b += "\"model\":" + Json::quote(model_);
    b += ",\"max_tokens\":" + std::to_string(maxTokens_);
    b += ",\"stream\":true";
    if (thinking_) b += ",\"thinking\":{\"type\":\"adaptive\"}";

    //  System prompt as a block so it can carry cache_control: the Csound
    //  buffer is re-sent with every turn and is by far the biggest part of the
    //  request, so caching it is the difference between a cheap follow-up and
    //  paying for the whole document again each time.
    b += ",\"system\":[{\"type\":\"text\",\"text\":";
    b += Json::quote(buildSystemPrompt(orc, errors));
    b += ",\"cache_control\":{\"type\":\"ephemeral\"}}]";

    b += ",\"messages\":[";
    bool first = true;
    for (size_t i = 0; i < history.size(); ++i) {
        if (history[i].text.empty()) continue;
        if (!first) b += ",";
        first = false;
        b += "{\"role\":";
        b += history[i].fromUser ? "\"user\"" : "\"assistant\"";
        b += ",\"content\":" + Json::quote(history[i].text) + "}";
    }
    b += "]}";
    return b;
}

void ClaudeClient::decodeSseChunk(const std::string& chunk,
                                  std::string& carry,
                                  std::vector<ChatEvent>& out) {
    carry += chunk;
    //  SSE frames are line-oriented and a chunk can split one anywhere, so
    //  only complete lines are consumed and the remainder stays in `carry`.
    size_t start = 0;
    for (;;) {
        const size_t nl = carry.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = carry.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.compare(0, 6, "data: ") != 0) continue;      // skip "event:" etc

        const std::string payload = line.substr(6);
        if (payload == "[DONE]") continue;
        const Json j = Json::parse(payload);
        const std::string type = j["type"].str();

        if (type == "content_block_delta") {
            const Json& d = j["delta"];
            const std::string dt = d["type"].str();
            if (dt == "text_delta") {
                ChatEvent e; e.kind = EventKind::Delta; e.text = d["text"].str();
                if (!e.text.empty()) out.push_back(e);
            } else if (dt == "thinking_delta") {
                ChatEvent e; e.kind = EventKind::Thinking; e.text = d["thinking"].str();
                if (!e.text.empty()) out.push_back(e);
            }
        } else if (type == "message_delta") {
            const std::string sr = j["delta"]["stop_reason"].str();
            if (!sr.empty()) {
                //  Surface a truncated reply instead of letting it look
                //  finished: a max_tokens cut mid-instrument is the one failure
                //  that silently produces uncompilable code.
                if (sr == "max_tokens") {
                    ChatEvent e; e.kind = EventKind::Error;
                    e.text = "The reply hit the max_tokens limit and was cut off. "
                             "Ask for a smaller piece, or raise the limit in settings.";
                    out.push_back(e);
                }
                ChatEvent e; e.kind = EventKind::Done; e.stopReason = sr;
                out.push_back(e);
            }
        } else if (type == "error") {
            ChatEvent e; e.kind = EventKind::Error;
            const std::string m = j["error"]["message"].str();
            e.text = m.empty() ? "The API reported an error." : m;
            out.push_back(e);
        }
    }
    carry.erase(0, start);
}

std::string ClaudeClient::describeHostDocument(const std::string& doc) {
    if (doc.empty()) return std::string();

    //  Collect the instrument numbers already defined. A model that writes
    //  `instr 1` into a document that has one gets "instr 1 redefined" -- a
    //  hard compile failure, and the default PatchKnob document DOES define
    //  instr 1, so this is the common case rather than an edge case.
    std::vector<long> used;
    bool hasKeepAlive = false, hasMassign = false;
    std::istringstream is(doc);
    std::string line;
    while (std::getline(is, line)) {
        //  strip a comment so `; instr 3` is not mistaken for a definition
        const size_t sc = line.find(';');
        std::string t = sc == std::string::npos ? line : line.substr(0, sc);
        size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        t = t.substr(a);
        if (t.compare(0, 6, "instr ") == 0) {
            std::istringstream ns(t.substr(6));
            std::string tok;
            while (std::getline(ns, tok, ',')) {
                const size_t b = tok.find_first_not_of(" \t");
                if (b == std::string::npos) continue;
                const long n = std::strtol(tok.c_str() + b, nullptr, 10);
                if (n > 0) used.push_back(n);
            }
        } else if (t.compare(0, 7, "massign") == 0) {
            hasMassign = true;
        } else if (!t.empty() && (t[0] == 'f' || t[0] == 'i')) {
            //  a score line; `f 0 <big>` is the keep-alive
            if (t[0] == 'f' && t.find(" 0 ") != std::string::npos) hasKeepAlive = true;
        }
    }
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());

    std::string s = "\nFACTS ABOUT THE DOCUMENT IN THE EDITOR:\n";
    if (used.empty()) {
        s += "- It defines no instruments yet.\n";
    } else {
        s += "- Instrument numbers ALREADY DEFINED: ";
        for (size_t i = 0; i < used.size(); ++i) {
            if (i) s += ", ";
            s += std::to_string(used[i]);
        }
        s += ".\n  Do NOT reuse any of these unless the user asked you to REPLACE that\n"
             "  instrument -- `instr N redefined` is a hard compile error, not a warning.\n";
        long next = 1;
        for (size_t i = 0; i < used.size(); ++i) if (used[i] == next) ++next;
        s += "  The lowest free number is " + std::to_string(next) + ".\n";
    }
    s += hasKeepAlive
       ? "- The score has an `f 0` keep-alive, so live MIDI instruments keep running.\n"
       : "- The score has NO `f 0` keep-alive. If you emit a whole document, include\n"
         "  `f 0 86400` in <CsScore> or performance ends immediately and a live MIDI\n"
         "  instrument never sounds.\n";
    if (hasMassign)
        s += "- The document already has `massign` routing; extend it, do not replace it.\n";
    return s;
}

std::string ClaudeClient::makeCompilable(const std::string& code,
                                         const std::string& doc) {
    //  A whole document compiles as-is.
    if (code.find("<CsoundSynthesizer>") != std::string::npos) return code;

    //  A bare `instr` block is not a compilable unit. Splice it into the
    //  editor's own document so it is compiled against the sr/ksmps/nchnls and
    //  the tables the user actually has -- compiling it against invented
    //  defaults would report errors the user would never see, and miss ones
    //  they would.
    const std::string tag = "</CsInstruments>";
    const size_t at = doc.find(tag);
    if (at != std::string::npos)
        return doc.substr(0, at) + "\n" + code + "\n" + doc.substr(at);

    //  No usable host document: wrap it in a minimal one. Header values match
    //  the assistant's own defaults so a fragment is judged the same way.
    return "<CsoundSynthesizer>\n<CsOptions>\n-n -d\n</CsOptions>\n"
           "<CsInstruments>\nsr = 48000\nksmps = 32\nnchnls = 2\n0dbfs = 1\n"
           + code +
           "\n</CsInstruments>\n<CsScore>\n</CsScore>\n</CsoundSynthesizer>\n";
}

bool ClaudeClient::ready(std::string* why) const {
    if (backend_ == Backend::Cli) {
        std::string ver;
        if (!cliAvailable(&ver, cliExe_)) {
            if (why) *why = "Claude Code was not found on your PATH. Install it, "
                            "or switch to the API backend in Settings.";
            return false;
        }
        return true;
    }
    if (apiKey_.empty()) {
        if (why) *why = "No Anthropic API key is set. Open Settings to add one, "
                        "or switch to the Claude Code backend.";
        return false;
    }
    if (!available()) {
        if (why) *why = "This build has no working HTTPS backend.";
        return false;
    }
    return true;
}

void ClaudeClient::decodeCliLine(const std::string& line, std::vector<ChatEvent>& out) {
    const Json j = Json::parse(line);
    const std::string type = j["type"].str();

    if (type == "stream_event") {
        //  The CLI forwards ordinary API events inside an envelope. Re-emit the
        //  inner object through the SSE decoder so exactly one implementation
        //  understands deltas, thinking and stop reasons.
        const Json& ev = j["event"];
        const std::string et = ev["type"].str();
        if (et == "content_block_delta") {
            const Json& d = ev["delta"];
            const std::string dt = d["type"].str();
            if (dt == "text_delta") {
                ChatEvent e; e.kind = EventKind::Delta; e.text = d["text"].str();
                if (!e.text.empty()) {
                    //  Real output has begun: retire the progress line.
                    ChatEvent clear; clear.kind = EventKind::Status;
                    out.push_back(clear);
                    out.push_back(e);
                }
            } else if (dt == "thinking_delta") {
                ChatEvent e; e.kind = EventKind::Thinking; e.text = d["thinking"].str();
                if (!e.text.empty()) out.push_back(e);
            }
        } else if (et == "content_block_start") {
            if (ev["content_block"]["type"].str() == "thinking") {
                ChatEvent e; e.kind = EventKind::Status; e.text = "Thinking...";
                out.push_back(e);
            }
        } else if (et == "message_delta") {
            const std::string sr = ev["delta"]["stop_reason"].str();
            if (sr == "max_tokens") {
                ChatEvent e; e.kind = EventKind::Error;
                e.text = "The reply hit the model's output limit and was cut off. "
                         "Ask for a smaller piece.";
                out.push_back(e);
            }
        }
        return;
    }

    if (type == "result") {
        //  Terminal line. `is_error` is the CLI's own verdict; a rate-limit or
        //  auth failure arrives here rather than as an API error frame.
        if (j["is_error"].boolean()) {
            ChatEvent e; e.kind = EventKind::Error;
            const std::string r = j["result"].str();
            e.text = r.empty() ? "The Claude CLI reported an error." : r;
            out.push_back(e);
        }
        ChatEvent d; d.kind = EventKind::Done;
        d.stopReason = j["stop_reason"].str();
        if (d.stopReason.empty()) d.stopReason = "end_turn";
        out.push_back(d);
        return;
    }

    if (type == "system") {
        const std::string sub = j["subtype"].str();
        if (sub == "status" && j["status"].str() == "requesting") {
            ChatEvent e; e.kind = EventKind::Status;
            e.text = "Contacting Claude Code...";
            out.push_back(e);
        } else if (sub == "thinking_tokens") {
            //  The ONLY progress signal available during a long reasoning
            //  phase: the thinking TEXT is encrypted and arrives empty, so
            //  this running token estimate is all there is to show. Without
            //  it the panel sat silent for the whole think and looked hung.
            const int n = (int)j["estimated_tokens"].num(0.0);
            ChatEvent e; e.kind = EventKind::Status;
            e.text = n > 0 ? "Thinking... (~" + std::to_string(n) + " tokens)"
                           : "Thinking...";
            out.push_back(e);
        }
        return;
    }

    if (type == "rate_limit_event") {
        const Json& info = j["rate_limit_info"];
        if (info["status"].str() == "rejected" ||
            info["status"].str() == "blocked") {
            ChatEvent e; e.kind = EventKind::Error;
            e.text = "Claude Code is rate limited right now. Wait for the limit "
                     "to reset, or switch to the API backend in Settings.";
            out.push_back(e);
        }
        return;
    }
}

bool ClaudeClient::send(const std::vector<ChatMessage>& history,
                        const std::string& orc,
                        const std::string& errors,
                        std::string* why) {
    if (busy_.load()) {
        if (why) *why = "A reply is already in progress.";
        return false;
    }
    if (!ready(why)) return false;

    join();                      // reap the previous worker before starting one
    cancel_.store(false);
    busy_.store(true);

    const std::string key       = apiKey_;
    const bool        useCli     = (backend_ == Backend::Cli);
    const std::string cliExe     = cliExe_;
    const std::string modelName  = model_;
    const std::string manualDir = manualDir_;
    CompileCheck compileFn = compile_;
    std::string  hostDoc   = document_ ? document_() : orc;
    const int         rounds    = autoFixRounds_;
    std::vector<ChatMessage> convo = history;
    const std::string orcCopy = orc, errCopy = errors;

    worker_ = std::thread([this, convo, orcCopy, errCopy, key, manualDir, rounds,
                           useCli, cliExe, modelName, compileFn, hostDoc]() mutable {
        std::vector<std::string> headers;
        headers.push_back(std::string("x-api-key: ") + key);
        headers.push_back(kVersion);

        auto push = [this](const ChatEvent& e) {
            std::lock_guard<std::mutex> lk(qm_);
            queue_.push_back(e);
        };

        //  --- Claude Code subprocess: spends the SUBSCRIPTION, not credits ---
        auto runOnceCli = [&](const std::vector<ChatMessage>& turns,
                              std::string& reply) -> bool {
            CliOptions o;
            o.exe          = cliExe;
            o.model        = modelName;
            o.systemPrompt = buildSystemPrompt(orcCopy, errCopy);
            //  The CLI is stateless here (--no-session-persistence), so prior
            //  turns are flattened into the prompt rather than replayed as a
            //  message array.
            std::string prompt;
            for (size_t i = 0; i < turns.size(); ++i) {
                if (turns[i].text.empty()) continue;
                if (turns.size() > 1)
                    prompt += turns[i].fromUser ? "\n\n[user]\n" : "\n\n[assistant]\n";
                prompt += turns[i].text;
            }

            bool sawError = false;
            const std::string err = runClaudeCli(o, prompt,
                [&](const std::string& line) -> bool {
                    if (cancel_.load()) return false;
                    std::vector<ChatEvent> evs;
                    decodeCliLine(line, evs);
                    for (size_t i = 0; i < evs.size(); ++i) {
                        if (evs[i].kind == EventKind::Delta) reply += evs[i].text;
                        if (evs[i].kind == EventKind::Error) sawError = true;
                        if (evs[i].kind != EventKind::Done) push(evs[i]);
                    }
                    return true;
                }, &cancel_);

            if (!err.empty() && err != "cancelled") {
                ChatEvent e; e.kind = EventKind::Error; e.text = err; push(e);
                return false;
            }
            return err.empty() && !sawError;
        };

        //  One request/response round. Returns false if the turn failed or was
        //  cancelled; `reply` accumulates the assistant text either way.
        auto runOnce = [&](const std::string& body, std::string& reply) -> bool {
            std::string carry, errorBody;
            bool failed = false;

            HttpSink sink = [&](const char* data, size_t len) -> bool {
                if (cancel_.load()) return false;
                std::vector<ChatEvent> evs;
                decodeSseChunk(std::string(data, len), carry, evs);
                for (size_t i = 0; i < evs.size(); ++i) {
                    if (evs[i].kind == EventKind::Delta) reply += evs[i].text;
                    if (evs[i].kind == EventKind::Error) failed = true;
                    //  Done is emitted by the caller once the whole exchange
                    //  (including any correction rounds) has finished.
                    if (evs[i].kind != EventKind::Done) push(evs[i]);
                }
                return true;
            };

            HttpResponse r = postJson(kUrl, headers, body, sink, &cancel_);
            if (r.ok() && !failed) return true;

            std::string msg;
            if (r.error == "cancelled") {
                msg.clear();
            } else if (!r.error.empty()) {
                msg = "Network error: " + r.error;
            } else if (!r.ok()) {
                const Json j = Json::parse(carry.empty() ? errorBody : carry);
                std::string detail = j["error"]["message"].str();
                if (detail.empty()) detail = j["message"].str();
                char code[32];
                std::snprintf(code, sizeof code, "HTTP %ld", r.status);
                msg = std::string(code) + (detail.empty() ? "" : ": " + detail);
                if (r.status == 401)      msg += "\nThe API key was rejected. Check it in Settings.";
                else if (r.status == 429) msg += "\nRate limited -- wait a moment and try again.";
                else if (r.status == 529) msg += "\nThe API is overloaded. Try again shortly.";
            }
            if (!msg.empty()) { ChatEvent e; e.kind = EventKind::Error; e.text = msg; push(e); }
            return false;
        };

        std::string stop = "end_turn";
        for (int round = 0; ; ++round) {
            std::string reply;
            const bool turnOk = useCli
                ? runOnceCli(convo, reply)
                : runOnce(buildRequestBody(convo, orcCopy, errCopy), reply);
            if (!turnOk) {
                stop = cancel_.load() ? "cancelled" : "error";
                break;
            }
            if (cancel_.load()) { stop = "cancelled"; break; }

            //  ---- automatic opcode audit ------------------------------------
            //  Every instrument the model writes is checked against the shipped
            //  manual before the user is invited to apply it.
            const std::vector<std::string> blocks = extractCodeBlocks(reply, "csound");
            if (manualDir.empty() || blocks.empty()) break;

            { ChatEvent e; e.kind = EventKind::Checking;
              e.text = "Checking opcode types and usage against the Csound manual...";
              push(e); }

            const OpcodeIndex& ix = OpcodeIndex::shared(manualDir);
            std::vector<CheckIssue> issues;
            for (size_t b = 0; b < blocks.size(); ++b) {
                const std::vector<CheckIssue> found = checkCsound(blocks[b], ix);
                issues.insert(issues.end(), found.begin(), found.end());
            }

            {
                ChatEvent e; e.kind = EventKind::Checked;
                e.issues = (int)issues.size();
                if (!ix.loaded())
                    e.text = "The Csound manual could not be found, so opcode "
                             "usage was NOT checked.";
                else if (issues.empty())
                    e.text = "Opcode types and usage check out against the manual.";
                else
                    e.text = formatIssuesForModel(issues);
                push(e);
            }

            //  ---- COMPILE THE RESULT FOR REAL ------------------------------
            //  The manual audit is static; the compiler is the authority. Only
            //  run it once the audit is clean, so the model is never handed two
            //  unrelated classes of complaint at once.
            if ((issues.empty() || !ix.loaded()) && compileFn) {
                { ChatEvent e; e.kind = EventKind::Compiling;
                  e.text = "Compiling with Csound..."; push(e); }

                std::string cerr;
                for (size_t b = 0; b < blocks.size() && cerr.empty(); ++b)
                    cerr = compileFn(makeCompilable(blocks[b], hostDoc));

                if (cerr.empty()) {
                    ChatEvent e; e.kind = EventKind::Compiled; e.issues = 0;
                    e.text = "Compiles cleanly.";
                    push(e);
                    break;                       // audit clean AND it compiles
                }

                { ChatEvent e; e.kind = EventKind::Compiled; e.issues = 1;
                  e.text = cerr; push(e); }

                if (round >= rounds) {
                    ChatEvent e; e.kind = EventKind::Error;
                    e.text = "The code still does not compile after "
                           + std::to_string(rounds + 1) + " attempt(s). "
                             "It is shown above unchanged -- check it before applying.";
                    push(e);
                    break;
                }
                { ChatEvent e; e.kind = EventKind::Retry;
                  e.text = "Fixing the compile errors..."; push(e); }
                ChatMessage a; a.fromUser = false; a.text = reply; convo.push_back(a);
                ChatMessage u; u.fromUser = true;
                u.text = "That code does not compile. Csound reported:\n\n```\n"
                       + cerr + "\n```\n\n"
                         "Fix it and return the corrected code. If the error is "
                         "unfamiliar, look up the opcode in the Csound manual or "
                         "search the web for how it is used before guessing. "
                         "Return the whole corrected block, not a diff.";
                convo.push_back(u);
                continue;
            }

            if (issues.empty() || !ix.loaded()) break;
            if (round >= rounds) {
                ChatEvent e; e.kind = EventKind::Error;
                e.text = "The code still does not match the manual after "
                         + std::to_string(rounds + 1) + " attempt(s). "
                         "It is shown above unchanged -- check it before applying.";
                push(e);
                break;
            }

            //  Hand the mismatches straight back to the model.
            { ChatEvent e; e.kind = EventKind::Retry;
              e.text = "Asking for a correction..."; push(e); }
            ChatMessage a; a.fromUser = false; a.text = reply;      convo.push_back(a);
            ChatMessage u; u.fromUser = true;
            u.text = formatIssuesForModel(issues);                  convo.push_back(u);
        }

        { ChatEvent d; d.kind = EventKind::Done; d.stopReason = stop; push(d); }
        busy_.store(false);
    });
    return true;
}

void ClaudeClient::poll(const std::function<void(const ChatEvent&)>& onEvent) {
    std::deque<ChatEvent> drained;
    {
        std::lock_guard<std::mutex> lk(qm_);
        drained.swap(queue_);
    }
    for (std::deque<ChatEvent>::const_iterator it = drained.begin();
         it != drained.end(); ++it) {
        if (onEvent) onEvent(*it);
    }
}

std::vector<std::string> ClaudeClient::extractCodeBlocks(const std::string& reply,
                                                         const std::string& lang) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < reply.size()) {
        const size_t open = reply.find("```", p);
        if (open == std::string::npos) break;
        const size_t tagEnd = reply.find('\n', open);
        if (tagEnd == std::string::npos) break;
        std::string tag = reply.substr(open + 3, tagEnd - open - 3);
        while (!tag.empty() && (tag[tag.size()-1] == '\r' || tag[tag.size()-1] == ' '))
            tag.erase(tag.size() - 1);
        for (size_t i = 0; i < tag.size(); ++i)
            tag[i] = (char)((tag[i] >= 'A' && tag[i] <= 'Z') ? tag[i] + 32 : tag[i]);

        const size_t close = reply.find("```", tagEnd + 1);
        if (close == std::string::npos) break;         // unterminated: ignore it
        if (lang.empty() || tag == lang)
            out.push_back(reply.substr(tagEnd + 1, close - tagEnd - 1));
        p = close + 3;
    }
    return out;
}

}} // namespace PatchKnob::ai
