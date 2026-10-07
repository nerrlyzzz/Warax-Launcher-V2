// =====================================================================
//  Warax Launcher 2 — C++ / WebView2
//  Интерфейс (HTML/CSS/JS) зашифрован и вшит прямо в .exe,
//  ключи Supabase и ссылка обновлений — тоже зашифрованы.
// =====================================================================
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "secrets.h"
#include "web_pack.h"

using json = nlohmann::json;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

static const wchar_t* kHost = L"https://warax.local/";
static const UINT WM_APP_POST = WM_APP + 1;

static HWND g_hwnd = nullptr;
static ComPtr<ICoreWebView2Environment> g_env;
static ComPtr<ICoreWebView2Controller> g_ctrl;
static ComPtr<ICoreWebView2> g_wv;
static fs::path g_base;
static std::string g_hwid;
static std::string g_win = "10.0";
static HINTERNET g_http = nullptr;
static std::atomic<HANDLE> g_game{nullptr};

// ---------------------------------------------------------------- utils
static std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string P(const fs::path& p) { return U(p.wstring()); }

static std::string dumps(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Расшифровка строк, вшитых при сборке (xorshift-поток + ключ сборки)
static std::string unseal(const unsigned char* d, size_t n, uint32_t seed) {
    std::string out(n, '\0');
    uint32_t s = seed ? seed : 0x9E3779B9u;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        out[i] = (char)(d[i] ^ (uint8_t)(s & 0xFF) ^ WL_K[i % 32]);
    }
    return out;
}
#define UNSEAL(b) unseal((b).d, (b).n, (b).seed)

static std::string hexs(const unsigned char* b, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string r;
    r.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { r.push_back(h[b[i] >> 4]); r.push_back(h[b[i] & 15]); }
    return r;
}

static std::string randHex(size_t bytes) {
    std::vector<unsigned char> b(bytes);
    BCryptGenRandom(nullptr, b.data(), (ULONG)b.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return hexs(b.data(), b.size());
}

// ---------------------------------------------------------------- hashes
struct Hasher {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    DWORD len = 0;
    explicit Hasher(LPCWSTR alg) {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0))) { a = nullptr; return; }
        DWORD cb = 0;
        BCryptGetProperty(a, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof(len), &cb, 0);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0))) h = nullptr;
    }
    void add(const void* d, size_t n) { if (h) BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0); }
    std::string hex() {
        if (!h) return "";
        std::vector<unsigned char> out(len);
        BCryptFinishHash(h, out.data(), len, 0);
        return hexs(out.data(), out.size());
    }
    ~Hasher() {
        if (h) BCryptDestroyHash(h);
        if (a) BCryptCloseAlgorithmProvider(a, 0);
    }
};

static std::string hashStr(LPCWSTR alg, const std::string& s) {
    Hasher x(alg);
    x.add(s.data(), s.size());
    return x.hex();
}

static std::string hashFile(LPCWSTR alg, const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    Hasher x(alg);
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = f.gcount();
        if (n > 0) x.add(buf.data(), (size_t)n);
    }
    return x.hex();
}

// ---------------------------------------------------------------- HWID
static std::string regStr(HKEY root, const wchar_t* sub, const wchar_t* name) {
    wchar_t buf[512];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &sz) == ERROR_SUCCESS)
        return U(buf);
    return "";
}

static std::string computeHwid() {
    std::string s = regStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid");
    s += "|" + regStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    wchar_t win[MAX_PATH] = L"C:\\";
    GetWindowsDirectoryW(win, MAX_PATH);
    wchar_t root[4] = {win[0], L':', L'\\', 0};
    DWORD serial = 0;
    GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0);
    s += "|" + std::to_string(serial) + "|warax-hwid-v1";
    return hashStr(BCRYPT_SHA256_ALGORITHM, s).substr(0, 40);
}

static std::string winVersion() {
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return "10.0";
    auto fn = (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion");
    if (!fn) return "10.0";
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    fn(&v);
    return std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber);
}

// ---------------------------------------------------------------- HTTP
struct HttpResp {
    int status = 0;
    std::string body;
    std::string error;
};
using Sink = std::function<bool(const char*, DWORD)>;

static bool crackUrl(const std::wstring& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port, bool& https) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    std::vector<wchar_t> h(1024), p(16384), x(16384);
    uc.lpszHostName = h.data(); uc.dwHostNameLength = (DWORD)h.size();
    uc.lpszUrlPath = p.data(); uc.dwUrlPathLength = (DWORD)p.size();
    uc.lpszExtraInfo = x.data(); uc.dwExtraInfoLength = (DWORD)x.size();
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    host.assign(uc.lpszHostName, uc.dwHostNameLength);
    path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    port = uc.nPort;
    https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

static HttpResp httpDo(const std::string& method, const std::string& url,
                       const std::vector<std::string>& headers, const std::string& body,
                       const Sink& sink = nullptr, std::atomic<uint64_t>* clen = nullptr,
                       int timeoutMs = 30000) {
    HttpResp r;
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = true;
    if (!crackUrl(W(url), host, path, port, https)) { r.error = "bad url"; return r; }
    HINTERNET c = WinHttpConnect(g_http, host.c_str(), port, 0);
    if (!c) { r.error = "connect " + std::to_string(GetLastError()); return r; }
    HINTERNET q = WinHttpOpenRequest(c, W(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!q) { r.error = "request " + std::to_string(GetLastError()); WinHttpCloseHandle(c); return r; }
    WinHttpSetTimeouts(q, 15000, 15000, timeoutMs, timeoutMs);
    std::wstring hs = L"User-Agent: WaraxLauncher/" + W(WL_VERSION) + L" (warvark)\r\n";
    for (auto& h : headers) hs += W(h) + L"\r\n";
    BOOL ok = WinHttpSendRequest(q, hs.c_str(), (DWORD)-1L,
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(q, nullptr);
    if (!ok) {
        r.error = "network " + std::to_string(GetLastError());
        WinHttpCloseHandle(q); WinHttpCloseHandle(c);
        return r;
    }
    DWORD st = 0, sz = sizeof(st);
    WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &st, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)st;
    if (clen && st < 400) {
        DWORD cl = 0, cs = sizeof(cl);
        if (WinHttpQueryHeaders(q, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &cl, &cs, WINHTTP_NO_HEADER_INDEX))
            *clen += cl;
    }
    std::vector<char> buf(1 << 16);
    for (;;) {
        DWORD n = 0;
        if (!WinHttpReadData(q, buf.data(), (DWORD)buf.size(), &n)) { r.error = "read " + std::to_string(GetLastError()); break; }
        if (n == 0) break;
        if (sink && st < 400) {
            if (!sink(buf.data(), n)) { r.error = "write"; break; }
        } else {
            r.body.append(buf.data(), n);
        }
    }
    WinHttpCloseHandle(q);
    WinHttpCloseHandle(c);
    return r;
}

static std::string downloadTo(const std::string& url, const fs::path& dst,
                              std::atomic<uint64_t>* bytes, std::atomic<uint64_t>* clen) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::path tmp = dst;
    tmp += L".part";
    std::string last;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t got = 0;
        HttpResp r;
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return "не удалось записать " + P(dst);
            r = httpDo("GET", url, {}, "", [&](const char* d, DWORD n) {
                f.write(d, n);
                got += n;
                if (bytes) *bytes += n;
                return (bool)f;
            }, attempt == 0 ? clen : nullptr, 60000);
        }
        if (r.error.empty() && r.status >= 200 && r.status < 300) {
            // антивирус (Defender) часто держит свежий .jar на проверке — ждём и повторяем
            DWORD le = 0;
            for (int k = 0; k < 25; k++) {
                SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return "";
                le = GetLastError();
                if (CopyFileW(tmp.c_str(), dst.c_str(), FALSE)) { fs::remove(tmp, ec); return ""; }
                Sleep(200);
            }
            last = "не удалось сохранить " + P(dst) + " (код " + std::to_string((int)le) + ", файл занят антивирусом?)";
        } else {
            last = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        }
        if (bytes) *bytes -= got;
        fs::remove(tmp, ec);
        if (r.status == 404 || r.status == 403 || r.status == 401) break;
        Sleep(400 * (attempt + 1));
    }
    return last;
}

// ---------------------------------------------------------------- bridge
static void postToWeb(const std::string& s) {
    auto* p = new std::wstring(W(s));
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_POST, 0, (LPARAM)p)) delete p;
}

static void reply(const json& id, bool ok, const json& data) {
    json j = {{"id", id}, {"ok", ok}};
    j[ok ? "result" : "error"] = data;
    postToWeb(dumps(j));
}

static void emitEv(const std::string& ev, const json& data) {
    postToWeb(dumps(json{{"event", ev}, {"data", data}}));
}

static fs::path tempDir() {
    wchar_t t[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, t);
    return n ? fs::path(t) : g_base / L"tmp";
}

static fs::path rp(const std::string& s) {
    if (s.rfind("$TEMP/", 0) == 0) return tempDir() / W(s.substr(6));
    fs::path p(W(s));
    if (p.is_absolute()) return p;
    return g_base / p;
}

static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (auto it = a.begin();; ++it) {
        size_t bs = 0;
        while (it != a.end() && *it == L'\\') { ++it; ++bs; }
        if (it == a.end()) { r.append(bs * 2, L'\\'); break; }
        if (*it == L'"') { r.append(bs * 2 + 1, L'\\'); r.push_back(*it); }
        else { r.append(bs, L'\\'); r.push_back(*it); }
    }
    r.push_back(L'"');
    return r;
}

static DWORD runHidden(std::wstring cl, const fs::path& cwd, bool wait) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cl.begin(), cl.end());
    buf.push_back(0);
    std::wstring wd = cwd.wstring();
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        wd.empty() ? nullptr : wd.c_str(), &si, &pi))
        return (DWORD)-1;
    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static std::string sysDir() {
    wchar_t s[MAX_PATH];
    GetSystemDirectoryW(s, MAX_PATH);
    return U(s);
}

// ---------------------------------------------------------------- Supabase
static bool safeName(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

static json apiCall(const std::string& kind, const std::string& name, json body) {
    std::string base = UNSEAL(WL_SUPA_URL);
    std::string key = UNSEAL(WL_SUPA_KEY);
    if (base.empty() || key.empty()) return {{"status", 0}, {"body", ""}, {"error", "noconfig"}};
    if (!safeName(name)) return {{"status", 0}, {"body", ""}, {"error", "badname"}};
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!body.is_object()) body = json::object();
    std::string url;
    if (kind == "fn") {
        body["hwid"] = g_hwid;
        body["version"] = WL_VERSION;
        url = base + "/functions/v1/" + name;
    } else {
        if (name == "launcher_login" || name == "launcher_check" || name == "launcher_info") {
            body["p_hwid"] = g_hwid;
            body["p_version"] = WL_VERSION;
        }
        url = base + "/rest/v1/rpc/" + name;
    }
    std::vector<std::string> h = {"apikey: " + key, "Content-Type: application/json"};
    if (key.rfind("eyJ", 0) == 0) h.push_back("Authorization: Bearer " + key);
    HttpResp r = httpDo("POST", url, h, dumps(body));
    SecureZeroMemory(&key[0], key.size());
    for (auto& x : h) SecureZeroMemory(&x[0], x.size());
    return {{"status", r.status}, {"body", r.body}, {"error", r.error}};
}

// ---------------------------------------------------------------- DPAPI
static bool secretSave(const std::string& s) {
    DATA_BLOB in{(DWORD)s.size(), (BYTE*)s.data()}, ent{(DWORD)g_hwid.size(), (BYTE*)g_hwid.data()}, out{};
    if (!CryptProtectData(&in, L"warax", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::ofstream f(g_base / L"session.bin", std::ios::binary | std::ios::trunc);
    f.write((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return (bool)f;
}

static std::string secretLoad() {
    std::ifstream f(g_base / L"session.bin", std::ios::binary);
    if (!f) return "";
    std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    DATA_BLOB in{(DWORD)d.size(), (BYTE*)d.data()}, ent{(DWORD)g_hwid.size(), (BYTE*)g_hwid.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string r((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}

// ---------------------------------------------------------------- downloads
struct Task {
    std::string url;
    fs::path path;
    std::string sha1;
    uint64_t size = 0;
    bool quick = false;
};

static bool fileOk(const Task& t) {
    std::error_code ec;
    if (!fs::exists(t.path, ec)) return false;
    uint64_t sz = fs::file_size(t.path, ec);
    if (ec) return false;
    if (t.size && sz != t.size) return false;
    if (sz == 0 && t.size != 0) return false;
    if (t.quick || t.sha1.empty()) return sz > 0 || t.size == 0;
    return _stricmp(hashFile(BCRYPT_SHA1_ALGORITHM, t.path).c_str(), t.sha1.c_str()) == 0;
}

static json cmdDownload(const json& a) {
    std::vector<Task> tasks;
    for (auto& t : a.at("tasks")) {
        Task x;
        x.url = t.value("url", "");
        x.path = rp(t.value("path", ""));
        x.sha1 = t.value("sha1", "");
        x.size = t.value("size", (uint64_t)0);
        x.quick = t.value("quick", false);
        tasks.push_back(std::move(x));
    }
    std::string tag = a.value("tag", "");
    int threads = std::max(1, std::min(64, a.value("threads", 16)));
    std::atomic<size_t> next{0}, done{0};
    std::atomic<uint64_t> bytes{0}, clen{0};
    std::atomic<int> failed{0};
    std::mutex m;
    std::string firstErr;
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; i++) {
        pool.emplace_back([&] {
            for (;;) {
                size_t k = next++;
                if (k >= tasks.size()) break;
                Task& t = tasks[k];
                if (!fileOk(t)) {
                    std::string e = downloadTo(t.url, t.path, &bytes, &clen);
                    if (e.empty() && !t.sha1.empty() && !t.quick &&
                        _stricmp(hashFile(BCRYPT_SHA1_ALGORITHM, t.path).c_str(), t.sha1.c_str()) != 0)
                        e = "повреждён файл (sha1)";
                    if (!e.empty()) {
                        failed++;
                        std::lock_guard<std::mutex> g(m);
                        if (firstErr.empty()) firstErr = e + " — " + t.url;
                    }
                }
                done++;
            }
        });
    }
    while (done.load() < tasks.size()) {
        emitEv("dl", {{"tag", tag}, {"done", done.load()}, {"total", tasks.size()},
                      {"bytes", bytes.load()}, {"clen", clen.load()}});
        Sleep(120);
    }
    for (auto& t : pool) t.join();
    emitEv("dl", {{"tag", tag}, {"done", tasks.size()}, {"total", tasks.size()},
                  {"bytes", bytes.load()}, {"clen", clen.load()}});
    return {{"failed", failed.load()}, {"error", firstErr}};
}

// ---------------------------------------------------------------- game
static json cmdLaunch(const json& a, const json& id) {
    if (g_game.load()) throw std::runtime_error("Игра уже запущена");
    std::wstring exe = rp(a.at("exe").get<std::string>()).wstring();
    std::wstring cl = quoteArg(exe);
    for (auto& x : a.at("args")) cl += L" " + quoteArg(W(x.get<std::string>()));
    fs::path cwd = rp(a.value("cwd", "."));
    fs::path logp = rp(a.value("log", "logs/latest_game.log"));
    std::error_code ec;
    fs::create_directories(cwd, ec);
    fs::create_directories(logp.parent_path(), ec);
    std::vector<fs::path> temps;
    if (a.contains("temp"))
        for (auto& t : a["temp"]) temps.push_back(rp(t.get<std::string>()));

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE lf = CreateFileW(logp.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (lf != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = lf;
        si.hStdError = lf;
        si.hStdInput = nullptr;
    }
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cl.begin(), cl.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             cwd.c_str(), &si, &pi);
    DWORD err = GetLastError();
    if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
    if (!ok) throw std::runtime_error("Не удалось запустить Java (код " + std::to_string(err) + ")");
    CloseHandle(pi.hThread);
    g_game = pi.hProcess;
    DWORD pid = pi.dwProcessId;
    std::thread([pi, temps] {
        auto t0 = std::chrono::steady_clock::now();
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        g_game = nullptr;
        CloseHandle(pi.hProcess);
        for (auto& t : temps) {
            for (int i = 0; i < 10; i++) {
                std::error_code e;
                SetFileAttributesW(t.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!fs::exists(t, e) || fs::remove(t, e)) break;
                Sleep(300);
            }
        }
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        emitEv("gameExit", {{"code", (int)code}, {"seconds", (int64_t)secs}});
    }).detach();
    return {{"pid", pid}};
}

static void cleanTemp() {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(tempDir(), ec)) {
        std::wstring n = e.path().filename().wstring();
        if (n.rfind(L"wx_", 0) == 0 && e.path().extension() == L".jar") {
            SetFileAttributesW(e.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            fs::remove(e.path(), ec);
        }
    }
}

// ---------------------------------------------------------------- self update
static std::wstring psQuote(const std::wstring& s) {
    std::wstring r = L"'";
    for (wchar_t c : s) { if (c == L'\'') r += L"''"; else r += c; }
    return r + L"'";
}

static json cmdSelfUpdate(const json& a) {
    std::string url = a.at("url").get<std::string>();
    if (url.rfind("https://", 0) != 0) throw std::runtime_error("bad url");
    fs::path nf = g_base / L"updates" / L"WaraxLauncher.new.exe";
    std::atomic<uint64_t> bytes{0}, clen{0};
    std::atomic<bool> fin{false};
    std::thread prog([&] {
        while (!fin) {
            emitEv("dl", {{"tag", "update"}, {"done", 0}, {"total", 1}, {"bytes", bytes.load()}, {"clen", clen.load()}});
            Sleep(150);
        }
    });
    std::string e = downloadTo(url, nf, &bytes, &clen);
    fin = true;
    prog.join();
    if (!e.empty()) throw std::runtime_error("Загрузка обновления: " + e);
    std::string want = a.value("sha256", "");
    if (!want.empty() && _stricmp(hashFile(BCRYPT_SHA256_ALGORITHM, nf).c_str(), want.c_str()) != 0) {
        std::error_code ec;
        fs::remove(nf, ec);
        throw std::runtime_error("Файл обновления повреждён (sha256)");
    }
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    std::wstring ps = L"$ErrorActionPreference='SilentlyContinue'; Wait-Process -Id " + std::to_wstring(GetCurrentProcessId()) +
                      L" -Timeout 30; Start-Sleep -Milliseconds 400; Move-Item -Force -LiteralPath " + psQuote(nf.wstring()) +
                      L" -Destination " + psQuote(self) + L"; Start-Process -FilePath " + psQuote(self);
    DWORD n = 0;
    const BYTE* raw = (const BYTE*)ps.data();
    DWORD rawLen = (DWORD)(ps.size() * sizeof(wchar_t));
    CryptBinaryToStringW(raw, rawLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::wstring b64(n, L'\0');
    CryptBinaryToStringW(raw, rawLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &n);
    b64.resize(n);
    std::wstring cl = W(sysDir()) + L"\\WindowsPowerShell\\v1.0\\powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand " + b64;
    if (runHidden(cl, g_base, false) == (DWORD)-1) throw std::runtime_error("Не удалось запустить установщик обновления");
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    return true;
}

// ---------------------------------------------------------------- commands
static json listDir(const fs::path& p) {
    json arr = json::array();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(p, ec)) {
        std::error_code e2;
        bool dir = e.is_directory(e2);
        uint64_t sz = dir ? 0 : e.file_size(e2);
        arr.push_back({{"name", P(e.path().filename())}, {"dir", dir}, {"size", sz}});
    }
    return arr;
}

static std::string readAll(const fs::path& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    ok = (bool)f;
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static json handleAsync(const std::string& cmd, const json& a, const json& id) {
    if (cmd == "http") {
        std::vector<std::string> h;
        if (a.contains("headers"))
            for (auto& x : a["headers"]) h.push_back(x.get<std::string>());
        HttpResp r = httpDo(a.value("method", "GET"), a.at("url").get<std::string>(), h, a.value("body", ""));
        return {{"status", r.status}, {"body", r.body}, {"error", r.error}};
    }
    if (cmd == "api") return apiCall(a.value("kind", "rpc"), a.value("name", ""), a.value("body", json::object()));
    if (cmd == "download") return cmdDownload(a);
    if (cmd == "launch") return cmdLaunch(a, id);
    if (cmd == "selfUpdate") return cmdSelfUpdate(a);
    if (cmd == "fetchMod") {
        json r = apiCall("fn", "get-mod", {{"token", a.value("token", "")}});
        int st = r.value("status", 0);
        if (st == 404) return {{"error", "nofn"}};
        json b = json::parse(r.value("body", ""), nullptr, false);
        if (b.is_discarded() || !b.value("ok", false)) {
            std::string err = b.is_discarded() ? (r.value("error", "").empty() ? "HTTP " + std::to_string(st) : r.value("error", "")) : b.value("error", "denied");
            return {{"error", err}};
        }
        fs::path dst = g_base / L"cache" / W("wx_" + randHex(8) + ".jar");
        std::string e = downloadTo(b.value("url", ""), dst, nullptr, nullptr);
        if (!e.empty()) return {{"error", "download: " + e}};
        SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY);
        return {{"path", P(dst)}};
    }
    if (cmd == "extract") {
        fs::path dest = rp(a.at("dest").get<std::string>());
        std::error_code ec;
        fs::create_directories(dest, ec);
        std::wstring cl = quoteArg(W(sysDir()) + L"\\tar.exe") + L" -xf " + quoteArg(rp(a.at("zip").get<std::string>()).wstring()) +
                          L" -C " + quoteArg(dest.wstring());
        if (a.contains("exclude"))
            for (auto& e : a["exclude"]) cl += L" --exclude " + quoteArg(W(e.get<std::string>()));
        DWORD code = runHidden(cl, dest, true);
        if (code != 0) throw std::runtime_error("Ошибка распаковки (tar " + std::to_string((int)code) + ")");
        return true;
    }
    if (cmd == "findJava") {
        fs::path d = rp(a.at("dir").get<std::string>());
        std::error_code ec;
        if (!fs::exists(d, ec)) return nullptr;
        for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            auto& p = it->path();
            if (_wcsicmp(p.filename().c_str(), L"javaw.exe") == 0 && _wcsicmp(p.parent_path().filename().c_str(), L"bin") == 0)
                return P(p);
        }
        return nullptr;
    }
    if (cmd == "hashFile") return hashFile(a.value("alg", "sha1") == "sha256" ? BCRYPT_SHA256_ALGORITHM : BCRYPT_SHA1_ALGORITHM,
                                            rp(a.at("path").get<std::string>()));
    throw std::runtime_error("unknown command " + cmd);
}

static void openExternal(const std::wstring& u) {
    if (u.rfind(L"https://", 0) == 0 || u.rfind(L"http://", 0) == 0)
        ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void handleMessage(const std::string& msg) {
    json j = json::parse(msg, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    json id = j.value("id", json());
    std::string cmd = j.value("cmd", "");
    json a = j.value("args", json::object());
    try {
        // ---- быстрые команды (UI-поток)
        if (cmd == "init") {
            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            SYSTEM_INFO si{};
            GetNativeSystemInfo(&si);
            std::string arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                             : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86";
            reply(id, true, {{"version", WL_VERSION}, {"base", P(g_base)}, {"ramMb", (uint64_t)(ms.ullTotalPhys / 1048576)},
                             {"hwid", g_hwid}, {"win", g_win}, {"arch", arch},
                             {"configured", !UNSEAL(WL_SUPA_URL).empty()},
                             {"updatePage", UNSEAL(WL_UPDATE_PAGE)}});
            return;
        }
        if (cmd == "drag") { ReleaseCapture(); SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0); return; }
        if (cmd == "min") { ShowWindow(g_hwnd, SW_MINIMIZE); return; }
        if (cmd == "close") { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }
        if (cmd == "hide") { ShowWindow(g_hwnd, SW_HIDE); reply(id, true, true); return; }
        if (cmd == "show") { ShowWindow(g_hwnd, SW_SHOW); ShowWindow(g_hwnd, SW_RESTORE); SetForegroundWindow(g_hwnd); reply(id, true, true); return; }
        if (cmd == "openUrl") { openExternal(W(a.value("url", ""))); reply(id, true, true); return; }
        if (cmd == "openPath") {
            fs::path p = rp(a.value("path", "."));
            std::error_code ec;
            fs::create_directories(p, ec);
            ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            reply(id, true, true);
            return;
        }
        if (cmd == "readText") {
            bool ok = false;
            std::string s = readAll(rp(a.at("path").get<std::string>()), ok);
            reply(id, true, ok ? json(s) : json(nullptr));
            return;
        }
        if (cmd == "writeText") {
            fs::path p = rp(a.at("path").get<std::string>());
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            fs::path tmp = p;
            tmp += L".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                std::string s = a.value("text", "");
                f.write(s.data(), (std::streamsize)s.size());
            }
            MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
            reply(id, true, true);
            return;
        }
        if (cmd == "exists") { std::error_code ec; reply(id, true, fs::exists(rp(a.at("path").get<std::string>()), ec)); return; }
        if (cmd == "list") { reply(id, true, listDir(rp(a.at("path").get<std::string>()))); return; }
        if (cmd == "mkdir") { std::error_code ec; fs::create_directories(rp(a.at("path").get<std::string>()), ec); reply(id, true, true); return; }
        if (cmd == "remove") { std::error_code ec; fs::remove_all(rp(a.at("path").get<std::string>()), ec); reply(id, true, true); return; }
        if (cmd == "rename") {
            BOOL ok = MoveFileExW(rp(a.at("from").get<std::string>()).c_str(), rp(a.at("to").get<std::string>()).c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
            reply(id, true, ok ? true : false);
            return;
        }
        if (cmd == "hash") {
            std::string alg = a.value("alg", "md5");
            LPCWSTR al = alg == "sha1" ? BCRYPT_SHA1_ALGORITHM : alg == "sha256" ? BCRYPT_SHA256_ALGORITHM : BCRYPT_MD5_ALGORITHM;
            reply(id, true, hashStr(al, a.value("text", "")));
            return;
        }
        if (cmd == "secretSave") { reply(id, true, secretSave(a.value("data", ""))); return; }
        if (cmd == "secretLoad") { reply(id, true, secretLoad()); return; }
        if (cmd == "secretClear") { std::error_code ec; fs::remove(g_base / L"session.bin", ec); reply(id, true, true); return; }
        if (cmd == "gameRunning") { reply(id, true, g_game.load() != nullptr); return; }
        if (cmd == "killGame") { HANDLE h = g_game.load(); if (h) TerminateProcess(h, 1); reply(id, true, true); return; }
        if (cmd == "updateInfo") {
            std::thread([id] {
                HttpResp r = httpDo("GET", UNSEAL(WL_UPDATE_API), {"Accept: application/vnd.github+json"}, "");
                reply(id, true, {{"status", r.status}, {"body", r.body}, {"error", r.error}});
            }).detach();
            return;
        }
        // ---- долгие команды (фоновые потоки)
        std::thread([id, cmd, a] {
            try {
                reply(id, true, handleAsync(cmd, a, id));
            } catch (const std::exception& e) {
                reply(id, false, e.what());
            }
        }).detach();
    } catch (const std::exception& e) {
        reply(id, false, e.what());
    }
}

// ---------------------------------------------------------------- web assets
static std::mutex g_webMx;
static std::map<std::string, std::string> g_webCache;

static bool findWeb(const std::string& path, const std::string*& data, const wchar_t*& mime) {
    std::lock_guard<std::mutex> g(g_webMx);
    for (unsigned i = 0; i < WL_FILE_COUNT; i++) {
        if (path == WL_FILES[i].path) {
            auto it = g_webCache.find(path);
            if (it == g_webCache.end())
                it = g_webCache.emplace(path, unseal(WL_FILES[i].d, WL_FILES[i].n, WL_FILES[i].seed)).first;
            data = &it->second;
            mime = WL_FILES[i].mime;
            return true;
        }
    }
    return false;
}

static void fatal(const wchar_t* text) {
    MessageBoxW(g_hwnd, text, L"Warax Launcher", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static void resizeWebView() {
    if (!g_ctrl) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_ctrl->put_Bounds(rc);
}

static void setupWebView() {
    wchar_t local[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    std::wstring udf = std::wstring(local) + L"\\WaraxLauncher\\WebView";
    SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", L"FF07070C");
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(res) || !env) {
                    ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", nullptr, nullptr, SW_SHOWNORMAL);
                    fatal(L"Для работы лаунчера нужен Microsoft Edge WebView2 Runtime.\nСейчас откроется страница загрузки — установи его и запусти лаунчер снова.");
                }
                g_env = env;
                return env->CreateCoreWebView2Controller(g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT r2, ICoreWebView2Controller* c) -> HRESULT {
                            if (FAILED(r2) || !c) fatal(L"Не удалось создать окно WebView2.");
                            g_ctrl = c;
                            g_ctrl->get_CoreWebView2(&g_wv);
                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(g_ctrl.As(&c2))) c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 7, 7, 12});

                            ComPtr<ICoreWebView2Settings> s;
                            g_wv->get_Settings(&s);
#ifdef WL_DEV
                            s->put_AreDevToolsEnabled(TRUE);
#else
                            s->put_AreDevToolsEnabled(FALSE);
                            s->put_AreDefaultContextMenusEnabled(FALSE);
#endif
                            s->put_IsStatusBarEnabled(FALSE);
                            s->put_IsZoomControlEnabled(FALSE);
                            s->put_IsBuiltInErrorPageEnabled(FALSE);
                            ComPtr<ICoreWebView2Settings3> s3;
#ifndef WL_DEV
                            if (SUCCEEDED(s.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
#endif
                            ComPtr<ICoreWebView2Settings4> s4;
                            if (SUCCEEDED(s.As(&s4))) { s4->put_IsPasswordAutosaveEnabled(FALSE); s4->put_IsGeneralAutofillEnabled(FALSE); }

                            EventRegistrationToken tok;
                            g_wv->AddWebResourceRequestedFilter(L"https://warax.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                            g_wv->add_WebResourceRequested(
                                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* ev) -> HRESULT {
                                        ComPtr<ICoreWebView2WebResourceRequest> req;
                                        ev->get_Request(&req);
                                        LPWSTR uri = nullptr;
                                        req->get_Uri(&uri);
                                        std::wstring u = uri ? uri : L"";
                                        CoTaskMemFree(uri);
                                        std::wstring prefix = kHost;
                                        std::string path = u.size() > prefix.size() ? U(u.substr(prefix.size())) : "";
                                        size_t q = path.find_first_of("?#");
                                        if (q != std::string::npos) path = path.substr(0, q);
                                        if (path.empty()) path = "index.html";
                                        const std::string* data = nullptr;
                                        const wchar_t* mime = L"text/plain";
                                        ComPtr<ICoreWebView2WebResourceResponse> resp;
                                        if (!findWeb(path, data, mime)) {
                                            g_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &resp);
                                        } else {
                                            IStream* st = SHCreateMemStream((const BYTE*)data->data(), (UINT)data->size());
                                            std::wstring hdr = L"Content-Type: " + std::wstring(mime) + L"\r\nCache-Control: no-store";
                                            g_env->CreateWebResourceResponse(st, 200, L"OK", hdr.c_str(), &resp);
                                            if (st) st->Release();
                                        }
                                        ev->put_Response(resp.Get());
                                        return S_OK;
                                    }).Get(), &tok);

                            g_wv->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* ev) -> HRESULT {
                                        LPWSTR src = nullptr;
                                        ev->get_Source(&src);
                                        std::wstring s = src ? src : L"";
                                        CoTaskMemFree(src);
                                        if (s.rfind(kHost, 0) != 0) return S_OK;
                                        LPWSTR m = nullptr;
                                        if (FAILED(ev->TryGetWebMessageAsString(&m)) || !m) return S_OK;
                                        std::string msg = U(m);
                                        CoTaskMemFree(m);
                                        handleMessage(msg);
                                        return S_OK;
                                    }).Get(), &tok);

                            g_wv->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* ev) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        ev->get_Uri(&uri);
                                        std::wstring u = uri ? uri : L"";
                                        CoTaskMemFree(uri);
                                        if (u.rfind(kHost, 0) != 0) {
                                            ev->put_Cancel(TRUE);
                                            openExternal(u);
                                        }
                                        return S_OK;
                                    }).Get(), &tok);

                            g_wv->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* ev) -> HRESULT {
                                        ev->put_Handled(TRUE);
                                        LPWSTR uri = nullptr;
                                        ev->get_Uri(&uri);
                                        if (uri) { openExternal(uri); CoTaskMemFree(uri); }
                                        return S_OK;
                                    }).Get(), &tok);

                            resizeWebView();
                            g_wv->Navigate(L"https://warax.local/index.html");
                            return S_OK;
                        }).Get());
            }).Get());
    if (FAILED(hr)) {
        ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", nullptr, nullptr, SW_SHOWNORMAL);
        fatal(L"Для работы лаунчера нужен Microsoft Edge WebView2 Runtime.\nСейчас откроется страница загрузки — установи его и запусти лаунчер снова.");
    }
}

// ---------------------------------------------------------------- window
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SIZE:
        if (g_ctrl) {
            g_ctrl->put_IsVisible(wp != SIZE_MINIMIZED);
            resizeWebView();
        }
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_APP_POST: {
        auto* p = (std::wstring*)lp;
        if (g_wv && p) g_wv->PostWebMessageAsString(p->c_str());
        delete p;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

#ifndef WL_DEV
static void antiDebug() {
    for (;;) {
        BOOL remote = FALSE;
        CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
        if (IsDebuggerPresent() || remote) ExitProcess(0);
        Sleep(1500);
    }
}
#endif

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
#ifndef WL_DEV
    if (IsDebuggerPresent()) return 0;
    std::thread(antiDebug).detach();
#endif
    HANDLE mx = CreateMutexW(nullptr, TRUE, L"Local\\WaraxLauncher2");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"WaraxLauncher2", nullptr);
        if (!other) {  // старая копия закрывается после обновления — ждём
            WaitForSingleObject(mx, 8000);
        } else {
            ShowWindow(other, SW_SHOW);
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
            return 0;
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t appdata[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    g_base = fs::path(appdata) / L".waraxvisuals";
    std::error_code ec;
    fs::create_directories(g_base, ec);
    g_hwid = computeHwid();
    g_win = winVersion();

    g_http = WinHttpOpen(L"WaraxLauncher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_http) g_http = WinHttpOpen(L"WaraxLauncher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_http) fatal(L"Не удалось инициализировать сеть (WinHTTP).");
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(g_http, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof(dec));
    DWORD conns = 64;
    WinHttpSetOption(g_http, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &conns, sizeof(conns));
    std::thread(cleanTemp).detach();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(7, 7, 12));
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"WaraxLauncher2";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = MulDiv(1220, dpi, 96), hgt = MulDiv(760, dpi, 96);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    w = std::min<int>(w, wa.right - wa.left - 20);
    hgt = std::min<int>(hgt, wa.bottom - wa.top - 20);
    int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - hgt) / 2;

    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Warax Launcher",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             x, y, w, hgt, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));      // тёмная рамка
    DWORD corner = 2;
    DwmSetWindowAttribute(g_hwnd, 33, &corner, sizeof(corner));  // скруглённые углы (Win11)
    MARGINS mg{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(g_hwnd, &mg);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    setupWebView();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    HANDLE gh = g_game.load();
    (void)gh;  // игра продолжает работать после закрытия лаунчера
    if (g_http) WinHttpCloseHandle(g_http);
    CoUninitialize();
    if (mx) { ReleaseMutex(mx); CloseHandle(mx); }
    return 0;
}
