//----------------------------------------------------------------------------
//  PatchKnob — Claude Messages API client for the in-DAW Csound chat.
//
//  Talks straight to POST /v1/messages over HTTPS with the user's own key.
//  (The community "agent SDK" wraps the Claude Code CLI and authenticates
//  through Claude Code credentials, so it cannot use an API key at all -- and
//  it would drag Node and a global npm package onto every user's machine.)
//
//  THREADING
//   * send() returns immediately and runs the request on a worker thread.
//   * Nothing here is ever called from the audio thread.
//   * poll() is the message-thread pump: call it once per frame, it drains
//     whatever the worker has produced and hands it to your callbacks on the
//     MESSAGE thread, so views never touch the worker's memory.
//----------------------------------------------------------------------------
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PatchKnob { namespace ai {

//! Anthropic model ids. Opus is the default: this is code generation, and the
//! csd the model writes has to compile.
extern const char* const kModelOpus;
extern const char* const kModelSonnet;
extern const char* const kModelHaiku;

struct ChatMessage {
    bool        fromUser = true;
    std::string text;
};

//! What the worker produces, drained by poll() on the message thread.
//!
//! `Status` is transient progress with NO content: "contacting Claude Code",
//! "thinking (~1,200 tokens)". It exists because a reasoning model can think
//! for well over a minute before emitting its first word, and the CLI's
//! thinking deltas arrive with EMPTY text (they are encrypted). Without this
//! the panel showed absolutely nothing for the whole thinking phase and looked
//! hung. A Status always REPLACES the previous Status; it is never history.
//!
//! `Checking`/`Checked` bracket the automatic opcode audit: every reply that
//! contains Csound is validated against the shipped manual's signatures before
//! it is offered to the user, and a reply with problems is sent straight back
//! to the model to fix. `Retry` announces that correction round starting, so
//! the panel can show it rather than appearing to stall.
enum class EventKind { Delta, Thinking, Done, Error, Checking, Checked, Retry,
                       Status, Compiling, Compiled };
struct ChatEvent {
    EventKind   kind = EventKind::Delta;
    std::string text;      //!< Delta/Thinking: the new fragment. Error: the message.
    std::string stopReason;//!< Done only
    int         issues = 0;//!< Checked only: how many manual mismatches were found
};

class ClaudeClient {
public:
    ClaudeClient();
    ~ClaudeClient();

    ClaudeClient(const ClaudeClient&) = delete;
    ClaudeClient& operator=(const ClaudeClient&) = delete;

    //! Where the request is sent.
    //!
    //! `Cli` (the DEFAULT) shells out to the locally installed Claude Code and
    //! spends the user's SUBSCRIPTION -- no API key is held, stored or
    //! transmitted by PatchKnob at all, and no metered credits are consumed.
    //! `Api` posts to the Messages API with the user's own key, for people who
    //! do not have Claude Code installed or who prefer to pay per token.
    enum class Backend { Cli, Api };
    void setBackend(Backend b) { backend_ = b; }
    Backend backend() const { return backend_; }

    //! Path to the `claude` executable (Cli backend). Empty means "on PATH".
    void setCliExe(const std::string& exe) { cliExe_ = exe.empty() ? "claude" : exe; }
    const std::string& cliExe() const { return cliExe_; }

    void setApiKey(const std::string& key) { apiKey_ = key; }
    bool hasApiKey() const { return !apiKey_.empty(); }

    //! Can this client actually send? The Cli backend needs Claude Code on the
    //! PATH; the Api backend needs a key. `why` explains a false.
    bool ready(std::string* why = nullptr) const;
    void setModel(const std::string& m) { model_ = m; }
    const std::string& model() const { return model_; }

    //! Extended thinking. On by default -- writing a working Csound instrument
    //! from a one-line description is exactly the kind of task it helps with.
    void setThinking(bool on) { thinking_ = on; }
    bool thinking() const { return thinking_; }

    void setMaxTokens(int n) { maxTokens_ = n > 0 ? n : 8192; }

    //! Point the client at the shipped Csound manual to turn on the automatic
    //! opcode type/usage audit. With this set, every reply containing Csound is
    //! checked against the manual's documented signatures; anything that does
    //! not match is fed back to the model for correction WITHOUT the user
    //! having to ask. Empty disables the audit (and the client says so rather
    //! than silently skipping it).
    void setManualDir(const std::string& dir) { manualDir_ = dir; }
    const std::string& manualDir() const { return manualDir_; }

    //! How many automatic correction rounds to allow before handing the reply
    //! over anyway, flagged. 0 checks and reports but never re-asks.
    void setAutoFixRounds(int n) { autoFixRounds_ = n < 0 ? 0 : n; }
    int  autoFixRounds() const { return autoFixRounds_; }

    //! COMPILE ORACLE. Given a whole Csound document, return the compiler's
    //! complaint, or an EMPTY string if it compiled. Setting this closes the
    //! loop: the assistant's code is compiled for real after the manual audit
    //! passes, and any error is fed back for another attempt. Called on the
    //! worker thread, so the implementation must be safe there (a throwaway
    //! Csound instance is -- see engine/patch/csound_dry_compile.h).
    using CompileCheck = std::function<std::string(const std::string& csd)>;
    void setCompileCheck(CompileCheck c) { compile_ = c; }
    bool hasCompileCheck() const { return (bool)compile_; }

    //! The editor's current document, used to decide what to compile: a reply
    //! that emits a whole .csd is compiled as-is, while a bare `instr` block is
    //! spliced into this document's <CsInstruments> first, because an instr on
    //! its own is not a compilable unit.
    void setDocumentProvider(std::function<std::string()> f) { document_ = f; }

    //! Facts about the editor's document that the model MUST know to write an
    //! instrument that fits: which `instr` numbers are taken (writing an
    //! existing one is `instr N redefined`, a hard compile failure), whether
    //! the score keeps performance alive, and how MIDI is routed. Public for
    //! tests.
    static std::string describeHostDocument(const std::string& doc);

    //! Wrap a fragment into a compilable document using `doc` as the host.
    //! Public for tests. If `code` already looks like a whole document it is
    //! returned unchanged.
    static std::string makeCompilable(const std::string& code,
                                      const std::string& doc);

    bool busy() const { return busy_.load(); }

    //! Fire a request. `history` is the whole conversation so far (oldest
    //! first); `orc` is the current Csound buffer and `errors` the last
    //! compiler output, both folded into the system prompt so the model always
    //! sees what the user is actually looking at. Returns false if a request
    //! is already in flight or no key is set.
    bool send(const std::vector<ChatMessage>& history,
              const std::string& orc,
              const std::string& errors,
              std::string* why = nullptr);

    //! Ask the in-flight request to stop. The worker unwinds and reports Done.
    void cancel() { cancel_.store(true); }

    //! Message-thread pump. Calls `onEvent` for everything the worker queued
    //! since the last call. Safe to call when idle.
    void poll(const std::function<void(const ChatEvent&)>& onEvent);

    //! Pull fenced code blocks out of a reply. Used by "Apply to editor" --
    //! `lang` filters the fence tag ("csound", "orc", "csd"); empty takes any.
    static std::vector<std::string> extractCodeBlocks(const std::string& reply,
                                                      const std::string& lang = std::string());

    //! The system prompt. Exposed so a test can assert it carries the buffer.
    static std::string buildSystemPrompt(const std::string& orc,
                                         const std::string& errors);

    //! Request body for `history`. Exposed for tests: it must stay valid JSON
    //! when the Csound source is full of quotes and backslashes.
    std::string buildRequestBody(const std::vector<ChatMessage>& history,
                                 const std::string& orc,
                                 const std::string& errors) const;

    //! Decode one NDJSON line of `claude -p --output-format stream-json`.
    //! The CLI wraps the ordinary API events as {"type":"stream_event",
    //! "event":{...}}, so this unwraps and reuses the same decoding; it also
    //! understands the CLI's own terminal {"type":"result"} line.
    static void decodeCliLine(const std::string& line, std::vector<ChatEvent>& out);

    //! Feed one SSE chunk to the decoder. Public for tests; the worker uses it
    //! internally. Appends whatever became complete to `out`.
    static void decodeSseChunk(const std::string& chunk,
                               std::string& carry,
                               std::vector<ChatEvent>& out);

private:
    void join();

    Backend           backend_ = Backend::Cli;
    std::string       cliExe_  = "claude";
    std::string       apiKey_;
    std::string       model_;
    bool              thinking_  = true;
    int               maxTokens_ = 8192;
    std::string       manualDir_;
    int               autoFixRounds_ = 4;   //!< compile-fix loops need headroom
    CompileCheck      compile_;
    std::function<std::string()> document_;

    std::thread             worker_;
    std::atomic<bool>       busy_{false};
    std::atomic<bool>       cancel_{false};
    std::mutex              qm_;
    std::deque<ChatEvent>   queue_;
};

}} // namespace PatchKnob::ai
