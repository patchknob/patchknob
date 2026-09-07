//----------------------------------------------------------------------------
//  PatchKnob — run the local Claude Code CLI as a subprocess.
//
//  WHY THIS EXISTS: it spends the user's existing Claude Code subscription
//  instead of metered API credits. Authentication is handled entirely by the
//  local `claude` install (it reads ~/.claude), so PatchKnob never holds,
//  stores or transmits a key on this path.
//
//  FLAGS THAT ARE NOT OPTIONAL, each learned the hard way from `claude 2.1.233`:
//   * `--output-format stream-json` is REJECTED unless `--verbose` is also
//     passed ("requires --verbose"), which in unattended mode is a hard error
//     rather than a warning.
//   * `--bare` must NEVER be used here. It looks attractive (it strips hooks,
//     skills and CLAUDE.md) but its documented behaviour is "Anthropic auth is
//     strictly ANTHROPIC_API_KEY or apiKeyHelper -- OAuth and keychain are
//     never read". It would therefore defeat the entire point of this backend
//     and fall back to the paid key we are avoiding. Context is trimmed with
//     --strict-mcp-config / --disable-slash-commands / --system-prompt instead.
//   * There is no `--max-turns` in this CLI version; tool loops are prevented
//     by denying tools outright.
//----------------------------------------------------------------------------
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace PatchKnob { namespace ai {

struct CliOptions {
    std::string exe = "claude";     //!< resolved on PATH unless absolute
    std::string model;              //!< full id or alias; empty = CLI default
    std::string systemPrompt;       //!< replaces the default system prompt
    std::string workingDir;         //!< run here; keep it free of CLAUDE.md
    int         timeoutSeconds = 600;
};

//! One NDJSON line from the CLI. Return false to abort the run.
using CliLineSink = std::function<bool(const std::string& line)>;

//! Run `claude -p`, feeding `prompt` on stdin (no ARG_MAX limit, no shell
//! quoting) and delivering stdout line by line. Returns an empty string on
//! success, or a human-readable failure.
std::string runClaudeCli(const CliOptions& opts,
                         const std::string& prompt,
                         const CliLineSink& onLine,
                         const std::atomic<bool>* cancel = nullptr);

//! Is a usable `claude` on PATH? Fills `version` when it is. Cheap, cached.
bool cliAvailable(std::string* version = nullptr,
                  const std::string& exe = "claude");

}} // namespace PatchKnob::ai
