//----------------------------------------------------------------------------
//  PatchKnob — Anthropic API key storage.
//
//  RULES THIS FILE EXISTS TO ENFORCE
//   * No key is ever compiled into the binary.  The user supplies their own.
//   * The key lives in the per-USER, per-MACHINE application preferences
//     directory (SDL_GetPrefPath), never in a project file, never next to the
//     source tree, and never in anything the user would hand to someone else.
//   * The stored blob is BOUND TO THE MACHINE it was written on.  Copying the
//     prefs file to another computer -- or running a build compiled elsewhere
//     against a synced home directory -- does NOT carry the account over: the
//     binding check fails and PatchKnob asks for a key again, so every machine
//     is explicitly its own account.
//
//  On Windows the binding is real encryption (DPAPI CryptProtectData, scoped
//  to the current user on the current machine).  On Linux/macOS there is no
//  dependency-free equivalent, so the blob is obfuscated with a key derived
//  from the machine identity and the file is written 0600.  Be clear-eyed
//  about what that is: it stops the key travelling between machines and stops
//  it being read by another user on this one.  It is NOT protection against
//  someone who can already run code as you -- nothing short of a keyring is.
//----------------------------------------------------------------------------
#pragma once
#include <string>

namespace PatchKnob { namespace ai {

enum class KeyStatus {
    Ok,             //!< a key is stored and belongs to this machine + user
    Missing,        //!< nothing stored yet
    ForeignMachine, //!< a key is stored but was written on a different machine
    Unreadable      //!< the file exists but is corrupt
};

struct KeyLoad {
    KeyStatus   status = KeyStatus::Missing;
    std::string key;      //!< only populated when status == Ok
};

//! Absolute path of the key file. Exposed so the settings window can show the
//! user exactly where their credential lives.
std::string apiKeyPath();

//! An opaque, stable identifier for this machine+user pairing, safe to display
//! (it is a hash, not the underlying id). Shown in settings so the user can
//! tell two machines apart.
std::string machineFingerprint();

KeyLoad loadApiKey();
bool     saveApiKey(const std::string& key, std::string* error = nullptr);
bool     clearApiKey();

//! Cheap sanity check before spending a round trip: Anthropic keys start
//! "sk-ant-". Returns false with a reason for anything obviously not a key.
bool looksLikeApiKey(const std::string& key, std::string* reason = nullptr);

}} // namespace PatchKnob::ai
