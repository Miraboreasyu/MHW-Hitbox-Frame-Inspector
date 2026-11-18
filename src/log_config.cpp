#include "log_config.h"

#include <windows.h>
#include <share.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace app {
namespace {

std::mutex g_logMx;
std::string g_logPath;
std::string g_dllDir;
FILE* g_logFile = nullptr;

void EnsurePaths() {
    if (!g_logPath.empty()) return;
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&EnsurePaths), &self);
    ::GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (slash) {
        *slash = '\0';
        g_dllDir = path;
        g_logPath = g_dllDir + "\\debug_hitbox.log";
    } else {
        g_dllDir = ".";
        g_logPath = "debug_hitbox.log";
    }
}

std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    const std::size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    const std::size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

int ParseVirtualKey(const std::string& raw, int fallback) {
    std::string s = Upper(Trim(raw));
    if (s.empty()) return fallback;
    if (s == "NONE" || s == "0") return 0;
    if (s.size() == 1) {
        const char c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<int>(c);
        if (c == '[') return VK_OEM_4;
        if (c == ']') return VK_OEM_6;
        if (c == '\\') return VK_OEM_5;
        if (c == ';') return VK_OEM_1;
        if (c == '\'') return VK_OEM_7;
        if (c == ',') return VK_OEM_COMMA;
        if (c == '.') return VK_OEM_PERIOD;
        if (c == '/') return VK_OEM_2;
        if (c == '-') return VK_OEM_MINUS;
        if (c == '=') return VK_OEM_PLUS;
        if (c == '`') return VK_OEM_3;
    }
    if (s.size() >= 2 && s[0] == '0' && s[1] == 'X') {
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 16);
        if (end && *end == '\0' && v > 0 && v < 256) return static_cast<int>(v);
    }
    if (s[0] == 'F' && s.size() <= 3) {
        int fn = std::atoi(s.c_str() + 1);
        if (fn >= 1 && fn <= 24) return VK_F1 + (fn - 1);
    }
    if (s == "SPACE") return VK_SPACE;
    if (s == "TAB") return VK_TAB;
    if (s == "PAUSE") return VK_PAUSE;
    if (s == "INSERT" || s == "INS") return VK_INSERT;
    if (s == "DELETE" || s == "DEL") return VK_DELETE;
    if (s == "HOME") return VK_HOME;
    if (s == "END") return VK_END;
    if (s == "PGUP" || s == "PAGEUP") return VK_PRIOR;
    if (s == "PGDN" || s == "PAGEDOWN") return VK_NEXT;
    if (s.rfind("NUMPAD", 0) == 0 && s.size() == 7 && s[6] >= '0' && s[6] <= '9') {
        return VK_NUMPAD0 + (s[6] - '0');
    }
    char* end = nullptr;
    long dec = std::strtol(s.c_str(), &end, 10);
    if (end && *end == '\0' && dec > 0 && dec < 256) return static_cast<int>(dec);
    return fallback;
}

} // namespace

void InitLog() {
    std::lock_guard<std::mutex> lk(g_logMx);
    EnsurePaths();
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logFile = _fsopen(g_logPath.c_str(), "w", _SH_DENYNO);
    if (g_logFile) {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        std::fprintf(g_logFile, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] debug_hitbox plugin attached (dir=%s)\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     g_dllDir.c_str());
        std::fflush(g_logFile);
    }
}

void CloseLog() {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logFile) {
        std::fflush(g_logFile);
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMx);
    EnsurePaths();
    if (!g_logFile) {
        g_logFile = _fsopen(g_logPath.c_str(), "a", _SH_DENYNO);
    }
    if (g_logFile) {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        std::fprintf(g_logFile, "[%02u:%02u:%02u.%03u] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(g_logFile, fmt, ap);
        va_end(ap);
        std::fputc('\n', g_logFile);
        std::fflush(g_logFile);
    }
}

std::string DllDirectory() {
    std::lock_guard<std::mutex> lk(g_logMx);
    EnsurePaths();
    return g_dllDir;
}

Config LoadConfig() {
    Config cfg;
    const std::string iniPath = DllDirectory() + "\\debug_hitbox.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, iniPath.c_str(), "rb") != 0 || !f) {
        Log("config: no debug_hitbox.ini found; using defaults (Vis=U, Freeze=I, World=K, Keep=J, Step=O, Filter=P)");
        return cfg;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string content;
    if (sz > 0 && sz < 65536) {
        content.resize(static_cast<std::size_t>(sz));
        std::fread(&content[0], 1, static_cast<std::size_t>(sz), f);
    }
    std::fclose(f);

    std::size_t pos = 0;
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        pos = 3;
    }

    while (pos < content.size()) {
        std::size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) nl = content.size();
        std::string line = Trim(content.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Upper(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        if (!val.empty() && val[0] == ';') {
            val = ";";
        } else {
            const std::size_t semi = val.find(';');
            if (semi != std::string::npos) val = Trim(val.substr(0, semi));
        }

        if (key == "ENABLED") cfg.enabled = std::atoi(val.c_str()) != 0;
        else if (key == "KEYTOGGLEVIS" || key == "KEY_VIS") cfg.vkToggleVis = ParseVirtualKey(val, cfg.vkToggleVis);
        else if (key == "KEYTOGGLEFREEZE" || key == "KEY_FREEZE") cfg.vkToggleFreeze = ParseVirtualKey(val, cfg.vkToggleFreeze);
        else if (key == "KEYFREEZEOTHERS" || key == "KEY_FREEZE_OTHERS" || key == "KEYWORLDFREEZE")
            cfg.vkFreezeOthers = ParseVirtualKey(val, cfg.vkFreezeOthers);
        else if (key == "KEYPERSISTHITBOX" || key == "KEYKEEPHITBOX" || key == "KEY_KEEP" || key == "KEY_PERSIST")
            cfg.vkPersistHitbox = ParseVirtualKey(val, cfg.vkPersistHitbox);
        else if (key == "KEYSTEPFRAME" || key == "KEY_STEP") cfg.vkStepFrame = ParseVirtualKey(val, cfg.vkStepFrame);
        else if (key == "KEYCYCLEFILTER" || key == "KEY_FILTER") cfg.vkCycleFilter = ParseVirtualKey(val, cfg.vkCycleFilter);
        else if (key == "STARTVISON") cfg.startVisOn = std::atoi(val.c_str()) != 0;
        else if (key == "KEEPHITBOXESACTIVE" || key == "PERSISTHITBOXES" || key == "KEEP_HITBOXES_ACTIVE")
            cfg.keepHitboxesActive = std::atoi(val.c_str()) != 0;
        else if (key == "SHOWHUDBANNER") cfg.showHudBanner = std::atoi(val.c_str()) != 0;
        else if (key == "LINETHICKNESS") {
            float t = static_cast<float>(std::atof(val.c_str()));
            if (t >= 0.8f && t <= 8.0f) cfg.lineThickness = t;
        } else if (key == "OPACITYSCALE") {
            float o = static_cast<float>(std::atof(val.c_str()));
            if (o >= 0.1f && o <= 1.5f) cfg.opacityScale = o;
        } else if (key == "HDRNITS" || key == "HDR_NITS") {
            float n = static_cast<float>(std::atof(val.c_str()));
            if (n == 0.0f || (n >= 40.0f && n <= 1000.0f)) cfg.hdrNits = n;
        }
    }

    Log("config: loaded debug_hitbox.ini (enabled=%d, vkVis=0x%02X, vkFreeze=0x%02X, vkWorld=0x%02X, vkKeep=0x%02X, vkStep=0x%02X, vkFilter=0x%02X)",
        cfg.enabled ? 1 : 0, cfg.vkToggleVis, cfg.vkToggleFreeze, cfg.vkFreezeOthers,
        cfg.vkPersistHitbox, cfg.vkStepFrame, cfg.vkCycleFilter);
    return cfg;
}

const char* FilterModeName(FilterMode m) {
    switch (m) {
        case FilterMode::Combat:  return "COMBAT";
        case FilterMode::Hitbox:  return "HITBOX";
        case FilterMode::Hurtbox: return "HURTBOX";
        case FilterMode::All:     return "ALL";
        default:                  return "COMBAT";
    }
}

bool Readable(std::uintptr_t addr, std::size_t len) {
    if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFFULL) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD rwMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & rwMask) == 0) return false;
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return len <= (end - addr);
}

} // namespace app
