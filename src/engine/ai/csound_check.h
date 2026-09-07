//----------------------------------------------------------------------------
//  PatchKnob — Csound opcode type/usage checker.
//
//  Every instrument the assistant writes is checked against the SIGNATURES IN
//  THE SHIPPED CSOUND MANUAL before it is offered to the user, and any problems
//  are handed back to the model to fix.  The manual is the authority: the index
//  is built by parsing the `<pre class="synopsis">` line of every opcode page in
//  vendor/csound-manual (1720 opcodes, overloads included), so it cannot drift
//  from the documentation the user is reading in the help drawer.
//
//  DESIGN BIAS: a false positive is worse than a miss.  A checker that cries
//  wolf on valid Csound trains the user to ignore it and wastes a correction
//  round trip on the model.  So anything this cannot analyse with confidence --
//  expressions, user-defined opcodes, macros, arrays, plugin opcodes -- is
//  SKIPPED rather than guessed at.  Everything reported should be a real defect.
//----------------------------------------------------------------------------
#pragma once
#include <map>
#include <utility>
#include <string>
#include <vector>

namespace PatchKnob { namespace ai {

//! Csound encodes a variable's rate in its first letter. This is the whole
//! type system as far as opcode arguments are concerned.
enum class Rate {
    Unknown,  //!< could not be determined -- never reported as an error
    Init,     //!< i (also p-fields and numeric literals)
    Control,  //!< k
    Audio,    //!< a
    String,   //!< S (and quoted literals)
    Fsig,     //!< f -- streaming phase vocoder
    Wsig,     //!< w -- spectral
    Var       //!< x in a signature: accepts a, k or i
};

struct OpcodeSignature {
    std::vector<Rate> outs;
    std::vector<Rate> ins;
    size_t            minIns  = 0;   //!< args before the first optional group
    bool              variadic = false; //!< signature ended in "..."
    std::string       text;          //!< the manual line, shown to the user
};

class OpcodeIndex {
public:
    //! Build by scanning `manualDir` for *.html. Cheap enough to do lazily on
    //! first use; the result is immutable and shareable.
    static const OpcodeIndex& shared(const std::string& manualDir);

    bool loaded() const { return !ops_.empty(); }
    size_t size() const { return ops_.size(); }
    const std::vector<OpcodeSignature>* find(const std::string& name) const;

    //! Test seam: build from explicit (opcode name, synopsis line) pairs
    //! instead of the filesystem.
    static OpcodeIndex fromLines(
        const std::vector<std::pair<std::string, std::string> >& synopses);

private:
    //! `name` comes from the manual page itself (<strong>), never guessed from
    //! the synopsis: an opcode with no outputs -- `out asig1, asig2` -- would
    //! otherwise have its first ARGUMENT mistaken for its name.
    void addSynopsis(const std::string& name, const std::string& line);
    std::map<std::string, std::vector<OpcodeSignature> > ops_;
};

struct CheckIssue {
    enum class Severity { Error, Warning };
    Severity    severity = Severity::Error;
    int         line = 0;          //!< 1-based line in the checked source
    std::string opcode;
    std::string message;
    std::string documented;        //!< the manual signature(s), when relevant
};

//! Check one orchestra (or a whole .csd -- the <CsInstruments> section is
//! extracted automatically) against the manual.
std::vector<CheckIssue> checkCsound(const std::string& source, const OpcodeIndex& index);

//! Render issues as the correction prompt sent back to the model.
std::string formatIssuesForModel(const std::vector<CheckIssue>& issues);

//! Rate implied by an argument as written. Exposed for tests.
Rate rateOfArgument(const std::string& token);

}} // namespace PatchKnob::ai
