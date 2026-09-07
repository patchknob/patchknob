#include "http_client.h"

#if defined(_WIN32)
  #include <windows.h>
  #include <winhttp.h>
  #include <vector>
#else
  #include <curl/curl.h>
#endif

namespace PatchKnob { namespace ai {

#if !defined(_WIN32)
//============================================================================
//  libcurl backend
//============================================================================
namespace {

struct SinkCtx {
    const HttpSink*           sink;
    std::string*              body;
    const std::atomic<bool>*  cancel;
    bool                      aborted = false;
};

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* user) {
    SinkCtx* c = (SinkCtx*)user;
    const size_t n = size * nmemb;
    if (c->cancel && c->cancel->load()) { c->aborted = true; return 0; }
    if (c->sink && *c->sink) {
        if (!(*c->sink)(ptr, n)) { c->aborted = true; return 0; }
    } else if (c->body) {
        c->body->append(ptr, n);
    }
    return n;
}

//  curl_global_init is not thread-safe and must run once before any handle is
//  created.  A function-local static gives us exactly that, ordered.
struct CurlGlobal {
    CurlGlobal()  { code = curl_global_init(CURL_GLOBAL_DEFAULT); }
    CURLcode code = CURLE_OK;
};
bool curlReady() { static CurlGlobal g; return g.code == CURLE_OK; }

} // namespace

bool available() { return curlReady(); }

HttpResponse postJson(const std::string& url,
                      const std::vector<std::string>& headers,
                      const std::string& body,
                      const HttpSink& sink,
                      const std::atomic<bool>* cancel,
                      long timeoutSeconds) {
    HttpResponse res;
    if (!curlReady()) { res.error = "libcurl failed to initialise"; return res; }

    CURL* h = curl_easy_init();
    if (!h) { res.error = "could not create a libcurl handle"; return res; }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "content-type: application/json");
    for (size_t i = 0; i < headers.size(); ++i)
        hdrs = curl_slist_append(hdrs, headers[i].c_str());

    SinkCtx ctx{ &sink, &res.body, cancel };
    char errbuf[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
    //  Certificate verification stays ON.  An API key is going up this pipe.
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);

    if (rc != CURLE_OK) {
        if (ctx.aborted) res.error = "cancelled";
        else res.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return res;
}

#else
//============================================================================
//  WinHTTP backend (MinGW).  No extra dependency: winhttp ships with Windows.
//============================================================================
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

} // namespace

bool available() { return true; }

HttpResponse postJson(const std::string& url,
                      const std::vector<std::string>& headers,
                      const std::string& body,
                      const HttpSink& sink,
                      const std::atomic<bool>* cancel,
                      long timeoutSeconds) {
    HttpResponse res;

    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        res.error = "could not parse the request URL"; return res;
    }

    HINTERNET session = WinHttpOpen(L"PatchKnob/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { res.error = "WinHttpOpen failed"; return res; }

    const DWORD ms = (DWORD)(timeoutSeconds > 0 ? timeoutSeconds * 1000 : 0);
    WinHttpSetTimeouts(session, 30000, 30000, (int)ms, (int)ms);

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(session); res.error = "WinHttpConnect failed"; return res; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", path, nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        res.error = "WinHttpOpenRequest failed"; return res;
    }

    std::wstring hdrBlock = L"content-type: application/json\r\n";
    for (size_t i = 0; i < headers.size(); ++i) hdrBlock += widen(headers[i]) + L"\r\n";

    BOOL ok = WinHttpSendRequest(req, hdrBlock.c_str(), (DWORD)hdrBlock.size(),
                                 (LPVOID)body.data(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        res.error = "the HTTPS request failed";
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return res;
    }

    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    res.status = (long)status;

    std::vector<char> buf(16384);
    for (;;) {
        if (cancel && cancel->load()) { res.error = "cancelled"; break; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        DWORD got = 0;
        const DWORD want = avail < (DWORD)buf.size() ? avail : (DWORD)buf.size();
        if (!WinHttpReadData(req, buf.data(), want, &got) || got == 0) break;
        if (sink) { if (!sink(buf.data(), (size_t)got)) { res.error = "cancelled"; break; } }
        else      { res.body.append(buf.data(), (size_t)got); }
    }

    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    return res;
}
#endif

}} // namespace PatchKnob::ai
