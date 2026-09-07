//----------------------------------------------------------------------------
//  src/engine/rack/rack_script_module.h
//
//  Interface implemented by the scripting rack modules (Pd / Csound), so the host
//  (panel editor, project I/O) can read and replace their patch text generically
//  via dynamic_cast, without the rack library depending on libpd / Csound.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_RACK_SCRIPT_MODULE_H
#define PATCHKNOB_RACK_SCRIPT_MODULE_H

#include <string>

namespace rackx {

struct IScriptModule {
    virtual ~IScriptModule() = default;
    virtual const char* scriptKind() const = 0;             // "pd" | "csound"
    virtual std::string script() const = 0;                 // current patch text
    virtual void        setScript(const std::string& text) = 0;  // re-scan; ports may change
    virtual int         knobCount()   const = 0;
    virtual int         inJackCount() const = 0;
    virtual int         outJackCount() const = 0;
    //! Match the containing rack node's selected MIDI voice count.
    virtual void        setPolyphony(int voices) { (void)voices; }
    //! Compiler/loader messages from the last setScript (empty on success / n/a).
    virtual std::string lastError() const { return std::string(); }
};

} // namespace rackx

#endif
