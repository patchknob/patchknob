#include "csound_check.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <utility>
#include <sstream>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dirent.h>
#endif

namespace PatchKnob { namespace ai {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b-1] <= ' ') --b;
    return s.substr(a, b - a);
}

bool isIdentChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

Rate rateOfLetter(char c) {
    switch (c) {
        case 'a': return Rate::Audio;
        case 'k': return Rate::Control;
        case 'i': return Rate::Init;
        case 'S': return Rate::String;
        case 'f': return Rate::Fsig;
        case 'w': return Rate::Wsig;
        case 'x': return Rate::Var;
        default:  return Rate::Unknown;
    }
}

//  Keywords and statement forms that are NOT opcode calls. Checking these as if
//  they were opcodes is the fastest way to generate nonsense diagnostics.
const std::set<std::string>& nonOpcodes() {
    static const std::set<std::string> k = {
        "instr","endin","opcode","endop","if","then","else","elseif","endif",
        "while","od","do","enduntil","until","goto","igoto","kgoto","cggoto",
        "tigoto","rireturn","return","break","continue","print","printks",
        "sr","kr","ksmps","nchnls","nchnls_i","0dbfs","A4","seed",
        "#define","#include","#undef","#ifdef","#ifndef","#else","#end","#endif",
        "massign","pgmassign","ctrlinit","strset","gi","gk","ga"
    };
    return k;
}

//  Strip a comment, honouring string literals so a ';' inside "..." survives.
std::string stripComment(const std::string& line) {
    bool inStr = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') { inStr = !inStr; continue; }
        if (inStr) continue;
        if (c == ';') return line.substr(0, i);
        if (c == '/' && i + 1 < line.size() && line[i+1] == '/') return line.substr(0, i);
    }
    return line;
}

//  Split on top-level commas: parentheses and strings hold their contents.
std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    bool inStr = false;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') { inStr = !inStr; cur += c; continue; }
        if (!inStr) {
            if (c == '(' || c == '[') { ++depth; cur += c; continue; }
            if (c == ')' || c == ']') { --depth; cur += c; continue; }
            if (c == ',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); continue; }
        }
        cur += c;
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

//  True if the token is a bare name/literal we can reason about. Anything with
//  an operator, call or subscript is an expression whose rate we do not try to
//  infer -- see the header's note on false positives.
bool isSimpleToken(const std::string& t) {
    if (t.empty()) return false;
    if (t[0] == '"') return true;                         // string literal
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (isIdentChar(c) || c == '.') continue;
        return false;                                     // + - * / ( ) [ ] etc
    }
    return true;
}

bool isNumericLiteral(const std::string& t) {
    if (t.empty()) return false;
    bool digit = false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (std::isdigit((unsigned char)t[i])) { digit = true; continue; }
        if (t[i] == '.') continue;
        return false;
    }
    return digit;
}

const char* rateName(Rate r) {
    switch (r) {
        case Rate::Audio:   return "a-rate (audio)";
        case Rate::Control: return "k-rate (control)";
        case Rate::Init:    return "i-rate (init)";
        case Rate::String:  return "S (string)";
        case Rate::Fsig:    return "f (fsig)";
        case Rate::Wsig:    return "w (wsig)";
        case Rate::Var:     return "x (a, k or i)";
        default:            return "unknown";
    }
}

//! Can a signature slot of rate `want` accept an argument of rate `got`?
bool accepts(Rate want, Rate got) {
    if (want == Rate::Unknown || got == Rate::Unknown) return true;   // be quiet
    if (want == got) return true;
    //  x means "a, k or i" and is the common case for amplitude/frequency.
    if (want == Rate::Var)
        return got == Rate::Audio || got == Rate::Control || got == Rate::Init;
    //  Csound freely promotes i -> k and i -> a where a variable rate is
    //  wanted, and k -> a inside a-rate contexts is legal in most opcodes.
    if (want == Rate::Control && got == Rate::Init) return true;
    if (want == Rate::Audio && (got == Rate::Init || got == Rate::Control)) return true;
    return false;
}

} // namespace

Rate rateOfArgument(const std::string& token) {
    const std::string t = trim(token);
    if (t.empty()) return Rate::Unknown;
    if (t[0] == '"') return Rate::String;
    if (isNumericLiteral(t)) return Rate::Init;
    if (!isSimpleToken(t)) return Rate::Unknown;
    size_t i = 0;
    //  Globals carry a leading 'g'; the rate letter is the one after it.
    if (t.size() > 1 && t[0] == 'g') i = 1;
    //  p-fields (p3, p4...) are i-rate.
    if (t[i] == 'p' && i + 1 < t.size() && std::isdigit((unsigned char)t[i+1]))
        return Rate::Init;
    return rateOfLetter(t[i]);
}

//----------------------------------------------------------------------------
//  Index construction
//----------------------------------------------------------------------------
void OpcodeIndex::addSynopsis(const std::string& opName, const std::string& rawLine) {
    std::string line = trim(rawLine);
    if (line.empty()) return;

    OpcodeSignature sig;
    sig.text = line;

    //  Optional groups are written [, x] or [x] and may nest a trailing "...".
    //  Record where the first optional starts, then drop the brackets so the
    //  argument list can be tokenised uniformly.
    if (line.find("...") != std::string::npos) sig.variadic = true;

    //  The opcode name is the first token that is not a rate-prefixed output.
    //  Outputs precede it and are comma separated; inputs follow.
    std::string flat;
    size_t firstOptional = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '[') { if (firstOptional == std::string::npos) firstOptional = flat.size(); continue; }
        if (line[i] == ']') continue;
        flat += line[i];
    }

    //  Split "outs opcode ins" on whitespace runs.
    std::vector<std::string> words;
    {
        std::istringstream is(flat);
        std::string w;
        while (is >> w) words.push_back(w);
    }
    if (words.empty()) return;

    //  Locate the opcode by NAME. The manual page states it, so there is
    //  nothing to infer -- and inferring it goes wrong immediately on an
    //  opcode with no outputs (`out asig1, asig2`), where the first ARGUMENT
    //  would be mistaken for the name.
    size_t opIdx = std::string::npos;
    for (size_t i = 0; i < words.size(); ++i) {
        std::string w = words[i];
        while (!w.empty() && !isIdentChar(w[w.size()-1])) w.erase(w.size()-1);
        if (w == opName) { opIdx = i; break; }
    }
    if (opIdx == std::string::npos) return;
    const std::string name = opName;

    //  Outputs
    {
        std::string outs;
        for (size_t i = 0; i < opIdx; ++i) { outs += words[i]; outs += " "; }
        const std::vector<std::string> parts = splitArgs(outs);
        for (size_t i = 0; i < parts.size(); ++i)
            sig.outs.push_back(rateOfArgument(parts[i]));
    }
    //  Inputs
    {
        std::string ins;
        for (size_t i = opIdx + 1; i < words.size(); ++i) { ins += words[i]; ins += " "; }
        const std::vector<std::string> parts = splitArgs(ins);
        for (size_t i = 0; i < parts.size(); ++i) {
            const std::string p = trim(parts[i]);
            if (p == "..." || p.empty()) { sig.variadic = true; continue; }
            sig.ins.push_back(rateOfArgument(p));
        }
    }

    //  How many inputs are mandatory: everything before the first '[' in the
    //  ORIGINAL line. Count commas in that prefix.
    if (firstOptional == std::string::npos) {
        sig.minIns = sig.ins.size();
    } else {
        const std::string head = line.substr(0, line.find('['));
        const size_t sp = head.find(name);
        std::string insHead = sp == std::string::npos ? std::string()
                                                      : head.substr(sp + name.size());
        sig.minIns = splitArgs(insHead).size();
        if (sig.minIns > sig.ins.size()) sig.minIns = sig.ins.size();
    }

    ops_[name].push_back(sig);
}

OpcodeIndex OpcodeIndex::fromLines(
        const std::vector<std::pair<std::string, std::string> >& synopses) {
    OpcodeIndex ix;
    for (size_t i = 0; i < synopses.size(); ++i)
        ix.addSynopsis(synopses[i].first, synopses[i].second);
    return ix;
}

const std::vector<OpcodeSignature>* OpcodeIndex::find(const std::string& name) const {
    std::map<std::string, std::vector<OpcodeSignature> >::const_iterator it = ops_.find(name);
    return it == ops_.end() ? nullptr : &it->second;
}

namespace {

//  Pull the synopsis lines out of one manual page. The markup is stable
//  DocBook output: <pre class="synopsis">ares <span..><strong>name</strong>...
void harvestPage(const std::string& html,
                 std::vector<std::pair<std::string, std::string> >& out) {
    const std::string open = "<pre class=\"synopsis\">";
    size_t p = 0;
    while ((p = html.find(open, p)) != std::string::npos) {
        const size_t s = p + open.size();
        const size_t e = html.find("</pre>", s);
        if (e == std::string::npos) break;
        const std::string block = html.substr(s, e - s);
        //  The opcode's own name, straight from the page's <strong> tag.
        std::string opName;
        {
            const size_t a = block.find("<strong>");
            const size_t b = a == std::string::npos ? a : block.find("</strong>", a);
            if (b != std::string::npos) {
                const std::string inner = block.substr(a + 8, b - a - 8);
                for (size_t i = 0; i < inner.size(); ++i)
                    if (inner[i] != '<' && inner[i] != '>') opName += inner[i];
                opName = trim(opName);
            }
        }
        if (opName.empty()) { p = e + 6; continue; }
        //  Strip tags, unescape the handful of entities DocBook emits.
        std::string text;
        bool inTag = false;
        for (size_t i = 0; i < block.size(); ++i) {
            const char c = block[i];
            if (c == '<') { inTag = true; continue; }
            if (c == '>') { inTag = false; continue; }
            if (!inTag) text += c;
        }
        const char* ents[][2] = { {"&lt;","<"}, {"&gt;",">"}, {"&amp;","&"},
                                  {"&quot;","\""}, {"&nbsp;"," "} };
        for (size_t k = 0; k < sizeof(ents)/sizeof(ents[0]); ++k) {
            size_t q = 0;
            while ((q = text.find(ents[k][0], q)) != std::string::npos)
                text.replace(q, std::strlen(ents[k][0]), ents[k][1]);
        }
        //  A synopsis may wrap; collapse whitespace to one line.
        std::string flat;
        bool sp = false;
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if ((unsigned char)c <= ' ') { if (!sp && !flat.empty()) { flat += ' '; sp = true; } }
            else { flat += c; sp = false; }
        }
        out.push_back(std::make_pair(opName, trim(flat)));
        p = e + 6;
    }
}

std::vector<std::string> listHtml(const std::string& dir) {
    std::vector<std::string> files;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    const std::string pat = dir + "*.html";
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(dir + fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".html") == 0)
            files.push_back(dir + n);
    }
    closedir(d);
#endif
    return files;
}

} // namespace

const OpcodeIndex& OpcodeIndex::shared(const std::string& manualDir) {
    //  Built once per directory, on whichever thread asks first. The chat runs
    //  this on its worker, never on the message thread.
    static std::map<std::string, OpcodeIndex> cache;
    std::map<std::string, OpcodeIndex>::iterator it = cache.find(manualDir);
    if (it != cache.end()) return it->second;

    OpcodeIndex ix;
    std::string dir = manualDir;
    if (!dir.empty() && dir[dir.size()-1] != '/' && dir[dir.size()-1] != '\\') dir += '/';
    const std::vector<std::string> files = listHtml(dir);
    std::vector<std::pair<std::string, std::string> > lines;
    for (size_t i = 0; i < files.size(); ++i) {
        std::ifstream f(files[i].c_str(), std::ios::binary);
        if (!f) continue;
        std::string html((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        harvestPage(html, lines);
    }
    for (size_t i = 0; i < lines.size(); ++i)
        ix.addSynopsis(lines[i].first, lines[i].second);
    return cache.insert(std::make_pair(manualDir, ix)).first->second;
}

//----------------------------------------------------------------------------
//  Checking
//----------------------------------------------------------------------------
std::vector<CheckIssue> checkCsound(const std::string& source, const OpcodeIndex& index) {
    std::vector<CheckIssue> issues;
    if (!index.loaded()) return issues;      // no manual -> say nothing

    //  A .csd carries score and options too; only the orchestra is Csound code.
    std::string src = source;
    const size_t oi = src.find("<CsInstruments>");
    if (oi != std::string::npos) {
        const size_t oe = src.find("</CsInstruments>", oi);
        const size_t b = oi + std::strlen("<CsInstruments>");
        src = src.substr(b, oe == std::string::npos ? std::string::npos : oe - b);
    }

    //  User-defined opcodes and macros are declared in the file itself; collect
    //  them so calls to them are not reported as unknown.
    std::set<std::string> userDefined;
    {
        std::istringstream is(src);
        std::string line;
        while (std::getline(is, line)) {
            const std::string t = trim(stripComment(line));
            if (t.compare(0, 7, "opcode ") == 0) {
                const std::vector<std::string> parts = splitArgs(t.substr(7));
                if (!parts.empty()) userDefined.insert(trim(parts[0]));
            } else if (t.compare(0, 8, "#define ") == 0) {
                std::istringstream ds(t.substr(8));
                std::string nm; ds >> nm;
                //  Macro names may carry a parameter list: NAME(a'b')
                const size_t par = nm.find('(');
                userDefined.insert(par == std::string::npos ? nm : nm.substr(0, par));
            }
        }
    }

    std::istringstream is(src);
    std::string raw;
    int lineNo = 0;
    while (std::getline(is, raw)) {
        ++lineNo;
        std::string line = trim(stripComment(raw));
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == '$') continue;      // macro use/define
        if (line[line.size()-1] == ':') continue;            // label
        if (line.find('=') != std::string::npos) {
            //  Assignment, not an opcode call -- unless the '=' is inside a
            //  comparison in a control statement, which we skip anyway.
            const size_t eq = line.find('=');
            if (eq + 1 >= line.size() || line[eq+1] != '=') continue;
        }

        //  Tokenise: [outputs] opcode [inputs]
        std::istringstream ls(line);
        std::vector<std::string> words;
        {
            std::string w;
            while (ls >> w) words.push_back(w);
        }
        if (words.empty()) continue;
        if (nonOpcodes().count(words[0])) continue;

        //  Locate the opcode: the first word that is a bare identifier AND is
        //  known to the manual. Outputs come before it.
        size_t opIdx = std::string::npos;
        std::string name;
        for (size_t i = 0; i < words.size() && i < 3; ++i) {
            std::string w = words[i];
            while (!w.empty() && w[w.size()-1] == ',') w.erase(w.size()-1);
            if (w.empty()) continue;
            if (nonOpcodes().count(w)) { opIdx = std::string::npos; break; }
            if (userDefined.count(w)) { opIdx = std::string::npos; break; }
            if (index.find(w)) { opIdx = i; name = w; break; }
        }
        if (opIdx == std::string::npos || name.empty()) continue;

        const std::vector<OpcodeSignature>* sigs = index.find(name);
        if (!sigs || sigs->empty()) continue;

        //  Gather actual inputs (everything after the opcode word).
        std::string insText;
        for (size_t i = opIdx + 1; i < words.size(); ++i) { insText += words[i]; insText += " "; }
        const std::vector<std::string> actualIns = splitArgs(insText);

        //  Gather actual outputs (everything before it).
        std::string outsText;
        for (size_t i = 0; i < opIdx; ++i) { outsText += words[i]; outsText += " "; }
        const std::vector<std::string> actualOuts = splitArgs(outsText);

        //  Try every documented overload; report only if NONE fits.
        bool matched = false;
        bool countOnlyFailure = true;
        for (size_t s = 0; s < sigs->size() && !matched; ++s) {
            const OpcodeSignature& sig = (*sigs)[s];
            if (actualIns.size() < sig.minIns) continue;
            if (!sig.variadic && actualIns.size() > sig.ins.size()) continue;
            if (sig.outs.size() != actualOuts.size() && !sig.variadic) {
                //  Output arity is exact; a mismatch here is a real error.
                continue;
            }
            countOnlyFailure = false;
            bool ok = true;
            for (size_t i = 0; i < actualIns.size() && i < sig.ins.size() && ok; ++i) {
                if (!isSimpleToken(actualIns[i])) continue;       // expression: skip
                if (!accepts(sig.ins[i], rateOfArgument(actualIns[i]))) ok = false;
            }
            for (size_t i = 0; i < actualOuts.size() && i < sig.outs.size() && ok; ++i) {
                if (!isSimpleToken(actualOuts[i])) continue;
                const Rate got = rateOfArgument(actualOuts[i]);
                //  An output is a declaration: its rate must MATCH, not promote.
                if (sig.outs[i] != Rate::Unknown && got != Rate::Unknown &&
                    sig.outs[i] != Rate::Var && got != sig.outs[i]) ok = false;
            }
            if (ok) matched = true;
        }
        if (matched) continue;

        CheckIssue issue;
        issue.line = lineNo;
        issue.opcode = name;
        for (size_t s = 0; s < sigs->size(); ++s) {
            if (s) issue.documented += "\n";
            issue.documented += (*sigs)[s].text;
        }
        if (countOnlyFailure) {
            issue.message = "wrong number of arguments for `" + name + "`";
        } else {
            issue.message = "argument types do not match any documented form of `"
                          + name + "`";
        }
        issues.push_back(issue);
    }
    return issues;
}

std::string formatIssuesForModel(const std::vector<CheckIssue>& issues) {
    if (issues.empty()) return std::string();
    std::string s =
        "The code you just wrote was checked against the Csound manual's opcode "
        "signatures and these do not match. Fix them and return the corrected "
        "code. If you believe a usage is actually correct, say so and explain "
        "rather than changing it.\n\n";
    for (size_t i = 0; i < issues.size(); ++i) {
        s += "- line " + std::to_string(issues[i].line) + ": " + issues[i].message + "\n";
        if (!issues[i].documented.empty()) {
            s += "  the manual documents:\n";
            std::istringstream ds(issues[i].documented);
            std::string d;
            while (std::getline(ds, d)) s += "    " + d + "\n";
        }
    }
    return s;
}

}} // namespace PatchKnob::ai
