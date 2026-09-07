#include "cli_backend.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <errno.h>
  #include <poll.h>
  #include <signal.h>
  #include <fcntl.h>
  #include <spawn.h>
  #include <sys/syscall.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace PatchKnob { namespace ai {

namespace {

//! The argument vector shared by both platforms. Built once so the two
//! backends can never drift into passing different flags.
std::vector<std::string> buildArgs(const CliOptions& o) {
    std::vector<std::string> a;
    a.push_back(o.exe);
    a.push_back("-p");                        // non-interactive, single shot
    a.push_back("--output-format");
    a.push_back("stream-json");
    a.push_back("--verbose");                 // REQUIRED with stream-json
    a.push_back("--include-partial-messages");// token-by-token deltas
    a.push_back("--no-session-persistence");  // do not litter ~/.claude
    a.push_back("--disable-slash-commands");  // skip skill loading
    a.push_back("--strict-mcp-config");       // no MCP servers, no their tokens
    //  Deny every tool: this is text generation, and an unattended run that
    //  stops to ask permission can never be answered.
    //  RESEARCH IS ALLOWED, THE FILESYSTEM IS NOT.
    //  WebSearch/WebFetch let the model look up DSP technique and opcode usage
    //  before guessing -- the difference between a plausible filter and a
    //  correct one. Everything that could touch the user's machine stays
    //  denied: this runs unattended, where a permission prompt can never be
    //  answered, so an allowed tool is one we are content to see used without
    //  anyone watching.
    a.push_back("--allowedTools");
    a.push_back("WebSearch,WebFetch");
    a.push_back("--disallowedTools");
    a.push_back("Bash,Edit,Write,Read,Glob,Grep,Task,NotebookEdit,Skill,"
                "SendMessage,TaskOutput,TaskStop");
    if (!o.model.empty())        { a.push_back("--model");         a.push_back(o.model); }
    if (!o.systemPrompt.empty()) { a.push_back("--system-prompt"); a.push_back(o.systemPrompt); }
    return a;
}

} // namespace

#if !defined(_WIN32)
//============================================================================
//  POSIX: pipe + fork + exec. popen() cannot do this -- the prompt goes in on
//  stdin (avoiding ARG_MAX and shell quoting entirely) while stdout streams
//  back, and popen is unidirectional.
//============================================================================
namespace {

bool probeVersion(const std::string& exe, std::string& out) {
    //  Deliberately NOT popen(): popen forks with the DAW's whole fd table
    //  inherited, which hands the child our ALSA MIDI and audio descriptors.
    //  Same reason the main run path closes everything above stdio.
    int fds[2];
    if (pipe(fds) != 0) return false;
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, 3, ~0U, 0) != 0)
#endif
        {
            long maxFd = sysconf(_SC_OPEN_MAX);
            if (maxFd < 0 || maxFd > 65536) maxFd = 65536;
            for (int fd = 3; fd < (int)maxFd; ++fd) close(fd);
        }
        const char* argv[] = { exe.c_str(), "--version", nullptr };
        execvp(argv[0], (char* const*)argv);
        _exit(127);
    }
    close(fds[1]);
    char buf[256] = {0};
    const ssize_t n = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    out.assign(buf, (size_t)n);
    while (!out.empty() && (out[out.size()-1] == '\n' || out[out.size()-1] == '\r'))
        out.erase(out.size() - 1);
    return !out.empty();
}

} // namespace

bool cliAvailable(std::string* version, const std::string& exe) {
    static std::string cachedExe, cachedVer;
    static bool cachedOk = false, probed = false;
    if (!probed || cachedExe != exe) {
        cachedExe = exe;
        cachedVer.clear();
        cachedOk = probeVersion(exe, cachedVer);
        probed = true;
    }
    if (version) *version = cachedVer;
    return cachedOk;
}

std::string runClaudeCli(const CliOptions& opts,
                         const std::string& prompt,
                         const CliLineSink& onLine,
                         const std::atomic<bool>* cancel) {
    int inPipe[2]  = {-1,-1};
    int outPipe[2] = {-1,-1};
    if (pipe(inPipe) != 0)  return "could not create a pipe for the prompt";
    if (pipe(outPipe) != 0) { close(inPipe[0]); close(inPipe[1]);
                              return "could not create a pipe for the reply"; }

    const std::vector<std::string> argStr = buildArgs(opts);
    std::vector<char*> argv;
    for (size_t i = 0; i < argStr.size(); ++i) argv.push_back((char*)argStr[i].c_str());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
        return "could not start a process for the Claude CLI";
    }
    if (pid == 0) {
        //  --- child ---
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        //  stderr to /dev/null: the CLI's progress chatter is not ours to show,
        //  and a full stderr pipe nobody drains would deadlock the child.
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(inPipe[0]);  close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        if (!opts.workingDir.empty()) { if (chdir(opts.workingDir.c_str()) != 0) { /* run where we are */ } }

        //  CLOSE EVERY INHERITED DESCRIPTOR ABOVE stdio BEFORE exec.
        //
        //  A fork inherits the DAW's entire fd table: the ALSA sequencer and
        //  raw-MIDI handles, the audio device, open project files, sockets.
        //  Handing duplicates of those to a `claude` process that then lives
        //  for a minute or more corrupts live MIDI input for every
        //  destination at once -- VST, Csound, racks, all of it -- because the
        //  damage is at the DEVICE, below any routing. (Reported as "midi
        //  input is all fucked up"; this is the cause.)
        //
        //  close_range() where it exists, an explicit loop where it does not.
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, 3, ~0U, 0) != 0)
#endif
        {
            long maxFd = sysconf(_SC_OPEN_MAX);
            if (maxFd < 0 || maxFd > 65536) maxFd = 65536;
            for (int fd = 3; fd < (int)maxFd; ++fd) close(fd);
        }

        execvp(argv[0], argv.data());
        _exit(127);                       // exec failed
    }

    //  --- parent ---
    close(inPipe[0]);
    close(outPipe[1]);

    //  A child that dies early turns our write into SIGPIPE, which would take
    //  the whole DAW down. Ignore it for the duration and read the error from
    //  the exit status instead.
    struct sigaction ign, prev;
    std::memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &prev);

    //  Feed the prompt. Non-blocking-ish: the pipe buffer is 64 KB and a big
    //  Csound document can exceed it, so interleave writing with reading.
    std::string pending = prompt;
    std::string carry, error;
    bool wroteAll = false;
    bool aborted = false;

    const int64_t deadlineMs = (int64_t)opts.timeoutSeconds * 1000;
    int64_t elapsedMs = 0;
    const int stepMs = 100;

    while (true) {
        if (cancel && cancel->load()) { aborted = true; break; }
        if (elapsedMs > deadlineMs) {
            error = "the Claude CLI did not finish within "
                  + std::to_string(opts.timeoutSeconds) + " seconds";
            aborted = true;
            break;
        }

        struct pollfd fds[2];
        int n = 0;
        fds[n].fd = outPipe[0]; fds[n].events = POLLIN; fds[n].revents = 0; ++n;
        if (!wroteAll) { fds[n].fd = inPipe[1]; fds[n].events = POLLOUT; fds[n].revents = 0; ++n; }

        const int pr = poll(fds, (nfds_t)n, stepMs);
        if (pr < 0) { if (errno == EINTR) continue; error = "poll failed"; break; }
        if (pr == 0) { elapsedMs += stepMs; continue; }

        //  Write side
        if (!wroteAll && n == 2 && (fds[1].revents & POLLOUT)) {
            const ssize_t w = write(inPipe[1], pending.data(),
                                    pending.size() > 32768 ? 32768 : pending.size());
            if (w > 0) pending.erase(0, (size_t)w);
            if (w < 0 && errno != EAGAIN && errno != EINTR) pending.clear();
            if (pending.empty()) { wroteAll = true; close(inPipe[1]); inPipe[1] = -1; }
        }

        //  Read side
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[8192];
            const ssize_t r = read(outPipe[0], buf, sizeof buf);
            if (r > 0) {
                carry.append(buf, (size_t)r);
                size_t start = 0;
                for (;;) {
                    const size_t nl = carry.find('\n', start);
                    if (nl == std::string::npos) break;
                    std::string line = carry.substr(start, nl - start);
                    start = nl + 1;
                    if (!line.empty() && line[line.size()-1] == '\r') line.erase(line.size()-1);
                    if (!line.empty() && onLine && !onLine(line)) { aborted = true; break; }
                }
                carry.erase(0, start);
                if (aborted) break;
            } else if (r == 0) {
                break;                     // child closed stdout: it is done
            } else if (errno != EAGAIN && errno != EINTR) {
                break;
            }
        }
    }

    if (inPipe[1] >= 0) close(inPipe[1]);
    if (aborted) kill(pid, SIGTERM);
    close(outPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    sigaction(SIGPIPE, &prev, nullptr);

    if (!error.empty()) return error;
    if (aborted) return cancel && cancel->load() ? std::string("cancelled") : error;
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        if (code == 127)
            return "could not run `" + opts.exe + "`. Is Claude Code installed "
                   "and on your PATH?";
        if (code != 0)
            return "the Claude CLI exited with status " + std::to_string(code);
    }
    return std::string();
}

#else
//============================================================================
//  Windows: CreateProcessW with redirected stdin/stdout. CREATE_NO_WINDOW so
//  no console flashes over the DAW; NOT CREATE_NEW_PROCESS_GROUP, which can
//  break stdin piping.
//============================================================================
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

//! Quote one argument for the CRT's command-line parser.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == L'\\') { ++slashes; continue; }
        if (a[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); slashes = 0; out += L'"'; continue; }
        out.append(slashes, L'\\'); slashes = 0;
        out += a[i];
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

} // namespace

bool cliAvailable(std::string* version, const std::string& exe) {
    //  Probing by running --version would flash a console; look for the
    //  executable on PATH instead, which is what actually matters.
    const std::wstring w = widen(exe.empty() ? std::string("claude") : exe);
    wchar_t found[MAX_PATH] = {0};
    const DWORD n = SearchPathW(nullptr, w.c_str(), L".exe", MAX_PATH, found, nullptr);
    if (n == 0 || n >= MAX_PATH) {
        const DWORD n2 = SearchPathW(nullptr, w.c_str(), L".cmd", MAX_PATH, found, nullptr);
        if (n2 == 0 || n2 >= MAX_PATH) return false;
    }
    if (version) *version = "found on PATH";
    return true;
}

std::string runClaudeCli(const CliOptions& opts,
                         const std::string& prompt,
                         const CliLineSink& onLine,
                         const std::atomic<bool>* cancel) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = nullptr; sa.bInheritHandle = TRUE;

    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0))  return "could not create a pipe for the prompt";
    if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW);
                                             return "could not create a pipe for the reply"; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    const std::vector<std::string> argStr = buildArgs(opts);
    std::wstring cmd;
    for (size_t i = 0; i < argStr.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quoteArg(widen(argStr[i]));
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = inR;
    si.hStdOutput = outW;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);

    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    const std::wstring wdir = widen(opts.workingDir);

    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr,
                                   wdir.empty() ? nullptr : wdir.c_str(), &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    if (!ok) {
        CloseHandle(inW); CloseHandle(outR);
        return "could not run `" + opts.exe + "`. Is Claude Code installed and on your PATH?";
    }

    //  Feed the prompt, then close stdin so the CLI knows the input ended.
    {
        size_t off = 0;
        while (off < prompt.size()) {
            DWORD wrote = 0;
            const DWORD want = (DWORD)((prompt.size() - off) > 32768 ? 32768 : (prompt.size() - off));
            if (!WriteFile(inW, prompt.data() + off, want, &wrote, nullptr) || wrote == 0) break;
            off += wrote;
        }
    }
    CloseHandle(inW);

    std::string carry, error;
    bool aborted = false;
    const DWORD start = GetTickCount();
    for (;;) {
        if (cancel && cancel->load()) { aborted = true; break; }
        if (opts.timeoutSeconds > 0 &&
            (GetTickCount() - start) > (DWORD)opts.timeoutSeconds * 1000u) {
            error = "the Claude CLI did not finish within "
                  + std::to_string(opts.timeoutSeconds) + " seconds";
            aborted = true;
            break;
        }
        char buf[8192];
        DWORD got = 0;
        if (!ReadFile(outR, buf, sizeof buf, &got, nullptr) || got == 0) break;
        carry.append(buf, got);
        size_t s = 0;
        for (;;) {
            const size_t nl = carry.find('\n', s);
            if (nl == std::string::npos) break;
            std::string line = carry.substr(s, nl - s);
            s = nl + 1;
            if (!line.empty() && line[line.size()-1] == '\r') line.erase(line.size()-1);
            if (!line.empty() && onLine && !onLine(line)) { aborted = true; break; }
        }
        carry.erase(0, s);
        if (aborted) break;
    }

    if (aborted) TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(outR);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!error.empty()) return error;
    if (aborted) return cancel && cancel->load() ? std::string("cancelled") : error;
    if (code != 0) return "the Claude CLI exited with status " + std::to_string(code);
    return std::string();
}
#endif

}} // namespace PatchKnob::ai
