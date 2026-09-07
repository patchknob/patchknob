//----------------------------------------------------------------------------
//  Headless self-test for the ai module. No API key and no network required:
//  the request builder, the SSE decoder, the JSON parser and the code-block
//  extractor are all exercised offline against the shapes the real API emits.
//----------------------------------------------------------------------------
#include "claude_client.h"
#include "json.h"
#include "api_key_store.h"
#include "cli_backend.h"
#include <fcntl.h>
#include <unistd.h>
#include "csound_check.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace PatchKnob::ai;

static int g_fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "  --  ", detail.c_str());
    if (!ok) ++g_fails;
}

int main() {
    // =====================================================================
    std::printf("\n[1] JSON round trip through Csound's worst characters\n");
    {
        //  Real Csound: quotes, backslashes, tabs, newlines -- the exact input
        //  that breaks a substring-based extractor and makes the API reject
        //  the whole request as malformed.
        const std::string nasty =
            "instr 1\n\tSname = \"a \\\"quoted\\\" name\"\n"
            "\tprints \"tab\\there\\n\"\n\ta1 oscili 0.5, 440\nendin\n";
        const std::string quoted = Json::quote(nasty);
        const std::string doc = "{\"text\":" + quoted + "}";
        std::string err;
        const Json j = Json::parse(doc, &err);
        check(err.empty(), "a document built with quote() re-parses", err);
        check(j["text"].str() == nasty, "the string survives the round trip byte for byte");
    }
    {
        std::string err;
        const Json j = Json::parse("{\"a\":[1,2,{\"b\":\"x\"}],\"c\":true,\"d\":null}", &err);
        check(err.empty(), "nested containers parse", err);
        check(j["a"].size() == 3, "array length");
        check(j["a"][2]["b"].str() == "x", "object inside array");
        check(j["c"].boolean() == true, "bool");
        check(j["d"].isNull(), "null");
        check(j["nope"].isNull(), "a missing key reads as null, it does not throw");
        check(j["a"][99].isNull(), "an out-of-range index reads as null");
    }
    {
        //  Control characters must be escaped or the API 400s the request.
        std::string ctrl = "a";
        ctrl += (char)0x01;
        ctrl += "b";
        const std::string q = Json::quote(ctrl);
        check(q.find("\\u0001") != std::string::npos,
              "a raw control character is escaped, not passed through");
        std::string err;
        check(!Json::parse(q + "}", &err).isObject(), "trailing junk does not parse as an object");
    }
    {
        std::string err;
        Json::parse("{\"a\":", &err);
        check(!err.empty(), "a truncated document reports an error rather than half-parsing");
        //  \u escapes, including a surrogate pair (an emoji in a comment).
        const Json j = Json::parse("{\"s\":\"\\u0041\\ud83c\\udfb9\"}");
        check(j["s"].str().substr(0, 1) == "A", "\\u escape decodes");
        check(j["s"].str().size() == 5, "a surrogate pair becomes one 4-byte UTF-8 glyph");
    }

    // =====================================================================
    std::printf("\n[2] request body\n");
    {
        ClaudeClient c;
        check(c.model() == std::string(kModelOpus), "the default model is Opus");

        std::vector<ChatMessage> hist;
        ChatMessage u; u.fromUser = true;  u.text = "make me a pluck";      hist.push_back(u);
        ChatMessage a; a.fromUser = false; a.text = "here you go";           hist.push_back(a);
        ChatMessage u2; u2.fromUser = true; u2.text = "now \"detune\" it";   hist.push_back(u2);

        const std::string orc = "sr = 44100\nksmps = 32\ninstr 1\n a1 oscili \"x\", 440\nendin\n";
        const std::string body = c.buildRequestBody(hist, orc, "error: syntax \\ oops");

        std::string err;
        const Json j = Json::parse(body, &err);
        check(err.empty(), "the request body is valid JSON with a hostile buffer", err);
        check(j["model"].str() == std::string(kModelOpus), "model field");
        check(j["stream"].boolean(), "streaming is on");
        check(j["thinking"]["type"].str() == "adaptive", "adaptive thinking by default");
        check(j["messages"].size() == 3, "every history turn is present");
        check(j["messages"][0]["role"].str() == "user", "roles alternate from the user");
        check(j["messages"][1]["role"].str() == "assistant", "assistant turn kept");
        check(j["messages"][2]["content"].str() == "now \"detune\" it",
              "quotes inside a user turn survive");

        const std::string sys = j["system"][0]["text"].str();
        check(sys.find("oscili") != std::string::npos,
              "the system prompt carries the editor buffer");
        check(sys.find("syntax \\ oops") != std::string::npos,
              "the system prompt carries the compiler output");
        check(j["system"][0]["cache_control"]["type"].str() == "ephemeral",
              "the buffer is cached so follow-up turns are not re-billed for it");

        //  An empty turn must not become an empty message: the API rejects those.
        std::vector<ChatMessage> withBlank = hist;
        ChatMessage blank; blank.fromUser = true; blank.text = "";
        withBlank.push_back(blank);
        const Json j2 = Json::parse(c.buildRequestBody(withBlank, "", ""));
        check(j2["messages"].size() == 3, "an empty turn is dropped, not sent");
    }

    // =====================================================================
    std::printf("\n[3] SSE decoding\n");
    {
        std::string carry;
        std::vector<ChatEvent> ev;
        ClaudeClient::decodeSseChunk(
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"delta\":"
            "{\"type\":\"text_delta\",\"text\":\"hel\"}}\n\n", carry, ev);
        check(ev.size() == 1 && ev[0].text == "hel", "a text delta decodes");

        //  Split a frame mid-JSON: the decoder must hold the fragment, not
        //  emit garbage. This is the normal case on a real socket.
        ev.clear();
        ClaudeClient::decodeSseChunk(
            "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text",
            carry, ev);
        check(ev.empty(), "a half-received frame emits nothing yet");
        ClaudeClient::decodeSseChunk("_delta\",\"text\":\"lo\"}}\n", carry, ev);
        check(ev.size() == 1 && ev[0].text == "lo", "the frame completes on the next chunk");

        ev.clear();
        ClaudeClient::decodeSseChunk(
            "data: {\"type\":\"content_block_delta\",\"delta\":"
            "{\"type\":\"thinking_delta\",\"thinking\":\"hmm\"}}\n", carry, ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Thinking,
              "thinking deltas are tagged separately from the reply");

        ev.clear();
        ClaudeClient::decodeSseChunk(
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n",
            carry, ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Done &&
              ev[0].stopReason == "end_turn", "end_turn closes the turn");

        //  A max_tokens stop is the one that silently yields uncompilable code.
        ev.clear();
        ClaudeClient::decodeSseChunk(
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"}}\n",
            carry, ev);
        check(ev.size() == 2 && ev[0].kind == EventKind::Error,
              "a max_tokens cut is surfaced as an error, not a clean finish");

        ev.clear();
        ClaudeClient::decodeSseChunk(
            "data: {\"type\":\"error\",\"error\":{\"message\":\"overloaded\"}}\n", carry, ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Error &&
              ev[0].text == "overloaded", "an API error frame is reported");

        ev.clear();
        ClaudeClient::decodeSseChunk(": keep-alive comment\n\ndata: [DONE]\n", carry, ev);
        check(ev.empty(), "keep-alive comments and [DONE] produce no events");
    }

    // =====================================================================
    std::printf("\n[4] pulling code out of a reply\n");
    {
        const std::string reply =
            "Sure, here it is:\n\n```csound\ninstr 1\n a1 oscili 0.2, 440\nendin\n```\n"
            "and some shell you should NOT paste:\n```sh\nrm -rf /\n```\n";
        const std::vector<std::string> csound = ClaudeClient::extractCodeBlocks(reply, "csound");
        check(csound.size() == 1, "one csound block found");
        check(csound[0].find("oscili") != std::string::npos, "the block body is the code");
        check(csound[0].find("rm -rf") == std::string::npos,
              "a differently tagged block is NOT collected by the csound filter");
        check(ClaudeClient::extractCodeBlocks(reply).size() == 2, "an empty filter takes both");

        const std::string unterminated = "```csound\ninstr 1\n";
        check(ClaudeClient::extractCodeBlocks(unterminated).empty(),
              "an unterminated fence yields nothing (a stream still in flight)");
        check(ClaudeClient::extractCodeBlocks("no code here").empty(), "prose yields nothing");

        const std::string upper = "```CSOUND\nx\n```";
        check(ClaudeClient::extractCodeBlocks(upper, "csound").size() == 1,
              "the fence tag match is case-insensitive");
    }

    // =====================================================================
    std::printf("\n[5] API key handling\n");
    {
        std::string why;
        check(!looksLikeApiKey("", &why), "an empty key is rejected");
        check(!looksLikeApiKey("hello world", &why), "a non-key string is rejected");
        check(!looksLikeApiKey("sk-ant-short", &why), "a truncated key is rejected");
        check(!looksLikeApiKey("sk-ant-api03-abcdefghijklmnop\n", &why),
              "a key with a trailing newline is rejected");
        check(looksLikeApiKey("sk-ant-api03-abcdefghijklmnopqrstuvwxyz0123456789", &why),
              "a well-formed key is accepted", why);

        const std::string fp = machineFingerprint();
        check(fp.size() == 16, "the machine fingerprint is a fixed-width token");
        check(fp == machineFingerprint(), "the fingerprint is stable across calls");
        check(!apiKeyPath().empty(), "the key path resolves");
    }
    {
        //  A client with no key must refuse to send rather than firing an
        //  unauthenticated request.  PIN THE API BACKEND: the default is now
        //  Cli, where a key is rightly not required -- and on any machine with
        //  Claude Code installed this test would otherwise spawn a REAL
        //  generation as a side effect of running the suite.
        ClaudeClient c;
        c.setBackend(ClaudeClient::Backend::Api);
        std::string why;
        check(!c.hasApiKey(), "a fresh client holds no key");
        check(!c.send(std::vector<ChatMessage>(), "", "", &why),
              "send() refuses without a key");
        check(!why.empty(), "and says why", why);
    }

    // =====================================================================
    std::printf("\n[6] Csound opcode audit against the shipped manual\n");
    {
        //  Built from the manual's own synopsis lines, name given explicitly
        //  (an opcode with no outputs would otherwise lose its name to its
        //  first argument -- the bug this seam exists to pin).
        std::vector<std::pair<std::string, std::string> > syn;
        syn.push_back(std::make_pair("oscili", "ares oscili xamp, xcps[, ifn, iphs]"));
        syn.push_back(std::make_pair("oscili", "kres oscili kamp, kcps[, ifn, iphs]"));
        syn.push_back(std::make_pair("moogladder", "asig moogladder ain, kcf, kres[, istor]"));
        syn.push_back(std::make_pair("madsr", "ares madsr iatt, idec, islev, irel [, idel] [, ireltim]"));
        syn.push_back(std::make_pair("madsr", "kres madsr iatt, idec, islev, irel [, idel] [, ireltim]"));
        syn.push_back(std::make_pair("outs", "outs asig1, asig2"));
        syn.push_back(std::make_pair("out",  "out asig1[, asig2,....]"));
        const OpcodeIndex ix = OpcodeIndex::fromLines(syn);

        check(ix.find("oscili") && ix.find("oscili")->size() == 2,
              "both documented forms of an overloaded opcode are indexed");
        check(ix.find("out") != nullptr,
              "an opcode with NO outputs keeps its own name, not its first argument");

        check(rateOfArgument("asig") == Rate::Audio,   "a-prefix is audio rate");
        check(rateOfArgument("kenv") == Rate::Control, "k-prefix is control rate");
        check(rateOfArgument("iamp") == Rate::Init,    "i-prefix is init rate");
        check(rateOfArgument("gaBus") == Rate::Audio,  "a global keeps its rate letter");
        check(rateOfArgument("p4")    == Rate::Init,   "a p-field is init rate");
        check(rateOfArgument("0.5")   == Rate::Init,   "a literal is init rate");
        check(rateOfArgument("\"name\"") == Rate::String, "a quoted literal is a string");
        check(rateOfArgument("iamp * kenv") == Rate::Unknown,
              "an expression is Unknown -- never guessed at");

        //  FALSE POSITIVES ARE THE FAILURE MODE THAT MATTERS. Valid, idiomatic
        //  Csound must come back completely clean.
        const std::string good =
            "instr 1\n"
            "  iamp = p4\n"
            "  kenv madsr 0.01, 0.1, 0.7, 0.2\n"
            "  asig oscili iamp * kenv, 440\n"
            "  aflt moogladder asig, 2000, 0.4\n"
            "  outs aflt, aflt\n"
            "endin\n";
        check(checkCsound(good, ix).empty(),
              "valid idiomatic Csound produces NO complaints");

        //  A k-rate output declared from the a-rate form.
        const std::string wrongRate = "instr 1\n  kbad oscili asig, 440\nendin\n";
        const std::vector<CheckIssue> r1 = checkCsound(wrongRate, ix);
        check(r1.size() == 1 && r1[0].opcode == "oscili",
              "an a-rate argument feeding a k-rate output form is caught");
        check(!r1.empty() && r1[0].documented.find("kres oscili") != std::string::npos,
              "the report quotes the manual's own signatures");

        const std::string tooFew = "instr 1\n  aout moogladder asig\nendin\n";
        const std::vector<CheckIssue> r2 = checkCsound(tooFew, ix);
        check(r2.size() == 1 && r2[0].message.find("number of arguments") != std::string::npos,
              "too few arguments is caught");

        //  Things the checker must stay quiet about, or it is unusable.
        check(checkCsound("instr 1\n  aout myUdo asig, 3\nendin\n"
                          "opcode myUdo, a, ak\nendop\n", ix).empty(),
              "a user-defined opcode is not reported as unknown");
        check(checkCsound("instr 1\n  aout unknownplugin asig\nendin\n", ix).empty(),
              "an opcode absent from the manual (a plugin) is skipped, not flagged");
        check(checkCsound("instr 1\n  asig oscili 0.5, 440 ; outs junk here\nendin\n", ix).empty(),
              "a comment is not parsed as code");
        check(checkCsound("kx = 3\nilen = 2 + 2\n", ix).empty(),
              "assignments are not treated as opcode calls");
        check(checkCsound("<CsoundSynthesizer>\n<CsScore>\ni1 0 1 0.5 440\n</CsScore>\n"
                          "</CsoundSynthesizer>\n", ix).empty(),
              "score lines in a .csd are not checked as orchestra code");

        const std::string msg = formatIssuesForModel(r2);
        check(msg.find("moogladder") != std::string::npos &&
              msg.find("line 2") != std::string::npos,
              "the correction prompt names the opcode and the line");

        //  With no manual the audit must be silent, never a wall of noise.
        const OpcodeIndex empty = OpcodeIndex::fromLines(
            std::vector<std::pair<std::string, std::string> >());
        check(checkCsound(wrongRate, empty).empty(),
              "with no manual loaded the checker reports nothing at all");
    }
    {
        ClaudeClient c;
        //  Raised from 1 to 4: the loop now runs the manual audit AND a real
        //  Csound compile, so a reply may need several passes (a type fix can
        //  reveal a compile error, and vice versa) before it is genuinely good.
        check(c.autoFixRounds() == 4, "four automatic correction rounds by default");
        check(!c.hasCompileCheck(), "no compile oracle until one is supplied");
        check(c.manualDir().empty(), "the audit is off until a manual is supplied");
        c.setManualDir("vendor/csound-manual");
        check(!c.manualDir().empty(), "and on once it is");
    }

    // =====================================================================
    std::printf("\n[7] Claude Code CLI backend (spends the subscription, not credits)\n");
    {
        ClaudeClient c;
        check(c.backend() == ClaudeClient::Backend::Cli,
              "the CLI backend is the DEFAULT -- no metered API credits by accident");
        check(c.cliExe() == std::string("claude"), "the CLI defaults to `claude` on PATH");

        std::string ver;
        const bool have = cliAvailable(&ver, "claude");
        std::printf("       claude on PATH: %s %s\n", have ? "yes" : "no", ver.c_str());
        if (have) {
            std::string why;
            check(c.ready(&why),
                  "with Claude Code installed the client is ready WITHOUT any key", why);
            check(!c.hasApiKey(), "and it still holds no API key at all");
        }

        //  A bogus executable must fail fast with a useful message, never hang.
        ClaudeClient bad;
        bad.setCliExe("definitely-not-a-real-claude-binary");
        std::string why;
        check(!bad.ready(&why), "a missing CLI is reported, not discovered mid-request");
        check(why.find("PATH") != std::string::npos, "and the message says what to do", why);

        //  Switching to the API backend restores the key requirement.
        ClaudeClient api;
        api.setBackend(ClaudeClient::Backend::Api);
        std::string aw;
        check(!api.ready(&aw), "the API backend still refuses without a key");
        check(aw.find("key") != std::string::npos, "and says so", aw);
    }
    {
        //  The CLI wraps ordinary API events in {"type":"stream_event", ...}.
        std::vector<ChatEvent> ev;
        ClaudeClient::decodeCliLine(
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
            "\"delta\":{\"type\":\"text_delta\",\"text\":\"instr 1\"}}}", ev);
        //  Two events: the progress line is retired, then the token itself.
        //  (See [8] -- a text token is what proves the think is over.)
        check(ev.size() == 2 && ev[1].kind == EventKind::Delta && ev[1].text == "instr 1",
              "a wrapped text delta unwraps to the same Delta the API path yields");
        check(ev[0].kind == EventKind::Status && ev[0].text.empty(),
              "and it clears any progress line first");

        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
            "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hmm\"}}}", ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Thinking,
              "thinking stays distinct from reply text");

        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"result\",\"is_error\":false,\"stop_reason\":\"end_turn\","
            "\"result\":\"done\"}", ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Done &&
              ev[0].stopReason == "end_turn", "the terminal result line closes the turn");

        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"result\",\"is_error\":true,\"result\":\"credit balance too low\"}", ev);
        check(ev.size() == 2 && ev[0].kind == EventKind::Error &&
              ev[0].text.find("credit") != std::string::npos,
              "a CLI-reported failure surfaces as an error AND still closes the turn");

        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"rate_limit_event\",\"rate_limit_info\":{\"status\":\"rejected\"}}", ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Error &&
              ev[0].text.find("rate limited") != std::string::npos,
              "a rate-limit event is explained rather than appearing as a stall");

        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"system\",\"subtype\":\"init\",\"tools\":[]}", ev);
        check(ev.empty(), "the CLI's init banner produces no chat events");
        ClaudeClient::decodeCliLine("not json at all", ev);
        check(ev.empty(), "a non-JSON line is ignored, not shown to the user");
    }

    // =====================================================================
    std::printf("\n[8] progress during a long think (the reported \"hang\")\n");
    {
        //  A reasoning model can think for over a MINUTE before its first word,
        //  and the CLI's thinking deltas arrive with EMPTY text (encrypted).
        //  Dropping those silently meant zero events reached the UI for the
        //  whole think, which read as a hang. These pin the progress signals.
        std::vector<ChatEvent> ev;
        ClaudeClient::decodeCliLine(
            "{\"type\":\"system\",\"subtype\":\"status\",\"status\":\"requesting\"}", ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Status &&
              !ev[0].text.empty(), "the request going out is announced");

        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\","
            "\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}}", ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Status &&
              ev[0].text.find("Thinking") != std::string::npos,
              "a thinking block starting is announced immediately");

        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"system\",\"subtype\":\"thinking_tokens\","
            "\"estimated_tokens\":1200}", ev);
        check(ev.size() == 1 && ev[0].kind == EventKind::Status &&
              ev[0].text.find("1200") != std::string::npos,
              "thinking progress reports a growing token estimate");

        //  An EMPTY thinking delta must not masquerade as content...
        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
            "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"\"}}}", ev);
        check(ev.empty(), "an empty (encrypted) thinking delta adds nothing");

        //  ...and the first real text must retire the progress line.
        ev.clear();
        ClaudeClient::decodeCliLine(
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
            "\"delta\":{\"type\":\"text_delta\",\"text\":\"instr\"}}}", ev);
        check(ev.size() == 2 && ev[0].kind == EventKind::Status && ev[0].text.empty(),
              "the first real token clears the progress line");
        check(ev[1].kind == EventKind::Delta && ev[1].text == "instr",
              "and the token itself follows");
    }

    // =====================================================================
    std::printf("\n[9] the child process inherits NO descriptors above stdio\n");
    {
        //  A fork inherits the DAW's whole fd table -- ALSA sequencer and
        //  raw-MIDI handles, the audio device, open project files. Handing
        //  duplicates to a `claude` process that lives for a minute wrecks live
        //  MIDI input for EVERY destination at once, because the damage is at
        //  the device, below any routing. This is regression cover for that.
        const char* script = std::getenv("PATCHKNOB_AI_TEST_FAKE_CLI");
        const char* fdout  = std::getenv("FDOUT");
        if (script && fdout) {
            int held[6];
            for (int i = 0; i < 6; ++i) held[i] = open("/dev/zero", O_RDONLY);

            CliOptions o;
            o.exe = script;
            o.timeoutSeconds = 20;
            std::string got;
            const std::string err = runClaudeCli(o, "hello",
                [&](const std::string& l){ got += l; return true; }, nullptr);
            check(err.empty(), "the guarded spawn still runs the child", err);

            std::ifstream f(fdout);
            std::string line; int aboveStdio = 0, total = 0;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                ++total;
                const int fd = std::atoi(line.c_str());
                //  255 is the shell's own fd, opened AFTER exec -- not inherited.
                if (fd > 2 && fd != 255) ++aboveStdio;
            }
            check(total > 0, "the child reported its descriptors");
            check(aboveStdio == 0,
                  "the child inherited NOTHING above stdio (no MIDI/audio handles)");
            for (int i = 0; i < 6; ++i) close(held[i]);
        } else {
            std::printf("       (skipped: set PATCHKNOB_AI_TEST_FAKE_CLI and FDOUT)\n");
        }
    }

    // =====================================================================
    std::printf("\n[10] compile-aware fragment wrapping\n");
    {
        //  A bare instr block is not a compilable unit; it has to be spliced
        //  into the user's OWN document so it is judged against their sr/ksmps
        //  and their tables, not invented defaults.
        const std::string doc =
            "<CsoundSynthesizer>\n<CsInstruments>\nsr = 44100\nksmps = 64\n"
            "instr 9\n endin\n</CsInstruments>\n<CsScore>\n</CsScore>\n"
            "</CsoundSynthesizer>\n";
        const std::string frag = "instr 1\n a1 oscili 0.5, 440\n outs a1, a1\nendin\n";

        const std::string wrapped = ClaudeClient::makeCompilable(frag, doc);
        check(wrapped.find("<CsoundSynthesizer>") != std::string::npos,
              "a fragment is wrapped into a whole document");
        check(wrapped.find("sr = 44100") != std::string::npos,
              "it is spliced into the USER's document, keeping their header");
        check(wrapped.find("instr 9") != std::string::npos &&
              wrapped.find("instr 1") != std::string::npos,
              "the existing instruments survive alongside the new one");
        check(wrapped.find("instr 1") < wrapped.find("</CsInstruments>"),
              "the fragment lands inside <CsInstruments>, not after it");

        //  A whole document is compiled as-is.
        const std::string whole =
            "<CsoundSynthesizer>\n<CsInstruments>\nsr=48000\n</CsInstruments>\n"
            "</CsoundSynthesizer>\n";
        check(ClaudeClient::makeCompilable(whole, doc) == whole,
              "a complete document is passed through untouched");

        //  With no host document a fragment still becomes compilable.
        const std::string bare = ClaudeClient::makeCompilable(frag, std::string());
        check(bare.find("<CsInstruments>") != std::string::npos &&
              bare.find("oscili") != std::string::npos,
              "with no host document a minimal one is synthesised");
        check(bare.find("0dbfs") != std::string::npos,
              "the synthesised host carries a header so levels are judged sanely");
    }

    // =====================================================================
    std::printf("\n[11] host contract: instrument numbers and MIDI routing\n");
    {
        //  PatchKnob's DEFAULT Csound document already defines instr 1, so a
        //  model writing the obvious `instr 1` collides and the whole document
        //  fails with "instr 1 redefined" -- verified against the real compiler.
        //  The prompt therefore has to state which numbers are taken.
        const std::string doc =
            "<CsoundSynthesizer>\n<CsInstruments>\nsr=48000\nksmps=32\nnchnls=2\n"
            "instr 1\nendin\n"
            "instr 3\nendin\n"
            "; instr 99 is only a comment\n"
            "</CsInstruments>\n<CsScore>\nf 0 86400\n</CsScore>\n</CsoundSynthesizer>\n";
        const std::string facts = ClaudeClient::describeHostDocument(doc);
        check(facts.find("ALREADY DEFINED") != std::string::npos,
              "the taken instrument numbers are stated");
        check(facts.find("1, 3") != std::string::npos,
              "both defined instruments are listed");
        check(facts.find("99") == std::string::npos,
              "a number inside a COMMENT is not mistaken for a definition");
        check(facts.find("lowest free number is 2") != std::string::npos,
              "the lowest free number is worked out for the model");
        check(facts.find("keep-alive") != std::string::npos,
              "the score keep-alive is reported");

        //  A document with no keep-alive must be called out: without `f 0` a
        //  live MIDI instrument never sounds, which looks like a broken
        //  instrument rather than a missing score line.
        const std::string noAlive =
            "<CsoundSynthesizer>\n<CsInstruments>\ninstr 2\nendin\n"
            "</CsInstruments>\n<CsScore>\n</CsScore>\n</CsoundSynthesizer>\n";
        const std::string f2 = ClaudeClient::describeHostDocument(noAlive);
        check(f2.find("NO `f 0` keep-alive") != std::string::npos,
              "a missing score keep-alive is flagged");
        check(f2.find("lowest free number is 1") != std::string::npos,
              "a gap below the used numbers is offered");

        check(ClaudeClient::describeHostDocument(std::string()).empty(),
              "an empty document produces no facts block");

        //  The MIDI contract has to reach the model, or the instruments it
        //  writes are score-only and silent when the user plays a key.
        const std::string sys = ClaudeClient::buildSystemPrompt(doc, "");
        check(sys.find("cpsmidi") != std::string::npos &&
              sys.find("ampmidi") != std::string::npos,
              "the prompt tells it to take pitch/velocity from MIDI");
        check(sys.find("massign") != std::string::npos,
              "the prompt tells it to route MIDI with massign");
        check(sys.find("claims EVERY MIDI channel") != std::string::npos,
              "the massign 0 multi-instrument hazard is spelled out");
        check(sys.find("madsr") != std::string::npos,
              "the prompt asks for MIDI-aware envelopes so notes end on note-off");
        check(sys.find("nchnls = 2") != std::string::npos &&
              sys.find("outs aL, aR") != std::string::npos,
              "the stereo output contract is stated");
        check(sys.find("f 0 86400") != std::string::npos,
              "the score keep-alive requirement is stated");
        check(sys.find("ALREADY DEFINED") != std::string::npos,
              "and the document's taken instrument numbers ride along with it");
    }

    // =====================================================================
    //  Round trip through the real filesystem. Skipped unless the caller points
    //  HOME at a throwaway directory and opts in -- saving a key here would
    //  otherwise overwrite the user's own credential.
    if (const char* go = std::getenv("PATCHKNOB_AI_TEST_KEYSTORE")) {
        if (std::string(go) == "1") {
            std::printf("\n[6] key store round trip (throwaway HOME)\n");
            const std::string key = "sk-ant-api03-abcdefghijklmnopqrstuvwxyz0123456789";
            std::string err;
            check(saveApiKey(key, &err), "the key saves", err);
            KeyLoad l = loadApiKey();
            check(l.status == KeyStatus::Ok, "it loads back on this machine");
            check(l.key == key, "byte for byte");

            //  Corrupt the machine token: this is what a config copied from
            //  another computer looks like, and it must NOT decode.
            {
                std::ifstream in(apiKeyPath());
                std::string magic, token, hex;
                std::getline(in, magic); std::getline(in, token); std::getline(in, hex);
                in.close();
                std::ofstream out(apiKeyPath(), std::ios::trunc);
                out << magic << "\n" << "0000000000000000" << "\n" << hex << "\n";
            }
            l = loadApiKey();
            check(l.status == KeyStatus::ForeignMachine,
                  "a key written on another machine is refused, not decoded");
            check(l.key.empty(), "and no key material is handed out");

            check(clearApiKey(), "the key clears");
            check(loadApiKey().status == KeyStatus::Missing, "and is then Missing");
        }
    }

    std::printf(g_fails ? "\n=== %d FAILURE(S) ===\n" : "\n=== all checks passed ===\n",
                g_fails);
    return g_fails ? 1 : 0;
}
