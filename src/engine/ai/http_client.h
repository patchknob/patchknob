//----------------------------------------------------------------------------
//  PatchKnob — minimal HTTPS POST, the one network primitive the DAW needs.
//
//  libcurl on Linux/macOS, WinHTTP on the MinGW build.  Nothing else in
//  PatchKnob talks to the network, and nothing else should: this header is the
//  whole surface, so an audit is one file long.
//
//  Threading: postJson() BLOCKS.  Never call it from the audio thread, and
//  never from the message thread either -- ClaudeClient runs it on a worker.
//----------------------------------------------------------------------------
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace PatchKnob { namespace ai {

struct HttpResponse {
    long        status = 0;      //!< HTTP status, or 0 if the request never went out
    std::string body;            //!< full body (empty when a stream sink consumed it)
    std::string error;           //!< transport-level failure, empty on success
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

//! Called with each chunk as it arrives. Return false to abort the transfer --
//! that is how the UI cancels a generation the user no longer wants.
using HttpSink = std::function<bool(const char* data, size_t len)>;

//! POST `body` as application/json to `url`.
//!
//! `headers` are raw "Name: value" lines. If `sink` is set the body is streamed
//! to it and HttpResponse::body is left empty; otherwise the body is collected.
//! `cancel`, when set and observed true, aborts in progress.
HttpResponse postJson(const std::string& url,
                      const std::vector<std::string>& headers,
                      const std::string& body,
                      const HttpSink& sink = HttpSink(),
                      const std::atomic<bool>* cancel = nullptr,
                      long timeoutSeconds = 600);

//! True if this build has a working HTTPS backend. The chat UI shows a plain
//! explanation instead of failing mysteriously when it is false.
bool available();

}} // namespace PatchKnob::ai
