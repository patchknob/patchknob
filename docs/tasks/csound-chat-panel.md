# Csound Chat Panel — pinned contract

The engine layer is **done, built and tested** (58 checks, `src/engine/ai/`).
This document pins the API so the UI work cannot drift from it.

Build the tests with:

    cmake -S src/engine/ai -B <dir> -DPATCHKNOB_AI_TESTS=ON && cmake --build <dir> && <dir>/ai_test

## What exists

`src/engine/ai/` — the ONLY part of PatchKnob that touches the network.

### `claude_client.h`

```cpp
extern const char* const kModelOpus;    // "claude-opus-5"  -- the default
extern const char* const kModelSonnet;  // "claude-sonnet-5"
extern const char* const kModelHaiku;   // "claude-haiku-4-5"

struct ChatMessage { bool fromUser = true; std::string text; };

enum class EventKind { Delta, Thinking, Done, Error };
struct ChatEvent { EventKind kind; std::string text; std::string stopReason; };

class ClaudeClient {
    void setApiKey(const std::string&);   bool hasApiKey() const;
    void setModel(const std::string&);    const std::string& model() const;
    void setThinking(bool);               bool thinking() const;   // default true
    void setMaxTokens(int);
    bool busy() const;

    // Returns immediately; runs on a worker thread. false => *why explains.
    bool send(const std::vector<ChatMessage>& history,
              const std::string& orc, const std::string& errors,
              std::string* why = nullptr);

    void cancel();

    // MESSAGE-THREAD PUMP. Call once per frame. Drains the worker's queue and
    // invokes onEvent on the message thread.
    void poll(const std::function<void(const ChatEvent&)>& onEvent);

    static std::vector<std::string> extractCodeBlocks(const std::string& reply,
                                                      const std::string& lang = "");
};
```

`send()` takes the WHOLE conversation each turn. The current editor buffer and
the last compiler output go in as `orc` and `errors` — they are folded into a
cached system prompt, so do not also paste them into a user turn.

### `api_key_store.h`

```cpp
enum class KeyStatus { Ok, Missing, ForeignMachine, Unreadable };
struct KeyLoad { KeyStatus status; std::string key; };

std::string apiKeyPath();          // show the user where it lives
std::string machineFingerprint();  // 16 hex chars, safe to display
KeyLoad     loadApiKey();
bool        saveApiKey(const std::string&, std::string* error = nullptr);
bool        clearApiKey();
bool        looksLikeApiKey(const std::string&, std::string* reason = nullptr);
```

`ForeignMachine` means the stored key was written on a **different machine or
user**. Say so plainly and offer to replace it — never silently fall back to
"missing", and never try to use it.

### `http_client.h`
`bool available()` — false means no HTTPS backend. Say so instead of failing
mysteriously.

## What the UI must do

1. **Chat panel in the Csound editor, docked on the RIGHT**, mirroring the
   existing help drawer. Toggle button on both, so you can move between the
   manual and the chat.
2. **Streaming.** `poll()` every frame; append `Delta` text live. `Thinking`
   text is separate — collapsed by default. `Error` shows in the panel, it must
   never be a modal or a crash. `Done` re-enables the composer.
3. **Apply to editor.** Fenced `csound` blocks in a reply get an Apply button.
   Applying goes through the editor's existing undo, so one Ctrl+Z takes it
   back. Offer replace-whole-document and insert-at-cursor.
4. **Send the compiler output.** After a failed compile, one click should ask
   for a fix, passing the error text as `errors`.
5. **Settings window** for the API key: masked entry, Save, Clear, a link to
   where the key is stored, the machine fingerprint, and the model picker
   (default Opus). Never log or display the key itself after saving.
6. **Cancel** a generation in flight.

## Hard rules

- The key is NEVER compiled in, never written to a project file, never logged.
- No network call and no `poll()` from the audio thread.
- Colours from `ui::theme()`; both Light and Midnight must look right.
- Cross-platform: Linux and the Windows MinGW build.
- No git commands that write history.
