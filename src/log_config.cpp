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
std::mutex g_hotCfgMx;
std::string g_logPath;
std::string g_dllDir;
FILE* g_logFile = nullptr;

struct IniStamp {
    bool          initialized = false;
    bool          exists      = false;
    std::uint32_t mtimeLo     = 0;
    std::uint32_t mtimeHi     = 0;
    std::uint32_t sizeLo      = 0;
    std::uint32_t sizeHi      = 0;

    bool SameAs(const IniStamp& o) const noexcept {
        return initialized == o.initialized && exists == o.exists &&
               mtimeLo == o.mtimeLo && mtimeHi == o.mtimeHi &&
               sizeLo == o.sizeLo && sizeHi == o.sizeHi;
    }
};
IniStamp g_lastIniStamp{};

IniStamp QueryIniStamp(const std::string& path) noexcept {
    IniStamp s{};
    s.initialized = true;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad) &&
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        s.exists  = true;
        s.mtimeLo = fad.ftLastWriteTime.dwLowDateTime;
        s.mtimeHi = fad.ftLastWriteTime.dwHighDateTime;
        s.sizeLo  = fad.nFileSizeLow;
        s.sizeHi  = fad.nFileSizeHigh;
    }
    return s;
}

std::string ParseChatCmdValue(const std::string& rawTrimmed) {
    std::string v = rawTrimmed;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v = v.substr(1, v.size() - 2);
    } else {
        const std::size_t semi = v.find(';');
        if (semi != std::string::npos) {
            std::size_t end = semi;
            while (end > 0 && (v[end - 1] == ' ' || v[end - 1] == '\t')) --end;
            v = v.substr(0, end);
        }
    }
    if (v.empty()) return {};
    if (v.size() == 4 &&
        (v[0] == 'N' || v[0] == 'n') && (v[1] == 'O' || v[1] == 'o') &&
        (v[2] == 'N' || v[2] == 'n') && (v[3] == 'E' || v[3] == 'e')) {
        return {};
    }
    return v;
}

bool TryApplyChatOrPadKey(const std::string& key,
                          const std::string& rawValTrimmed,
                          const std::string& strippedVal,
                          ChatCommandConfig& chat,
                          bool& gamepadPanel) {
    if (key == "CHATCOMMANDS" || key == "CHAT_COMMANDS" || key == "CHATHOTKEYS") {
        chat.enabled = std::atoi(strippedVal.c_str()) != 0;
        return true;
    }
    if (key == "CHATCASESENSITIVE" || key == "CHAT_CASE_SENSITIVE") {
        chat.caseSensitive = std::atoi(strippedVal.c_str()) != 0;
        return true;
    }
    if (key == "CHATCMDVIS" || key == "CHAT_CMD_VIS" || key == "CHATVIS" || key == "CHAT_U" || key == "CHATU") {
        chat.cmdVis = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDFREEZE" || key == "CHAT_CMD_FREEZE" || key == "CHATFREEZE" || key == "CHAT_I" || key == "CHATI") {
        chat.cmdFreeze = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDWORLDFREEZE" || key == "CHAT_CMD_WORLD_FREEZE" || key == "CHATWORLDFREEZE" ||
        key == "CHATCMDFREEZEOTHERS" || key == "CHAT_K" || key == "CHATK") {
        chat.cmdWorldFreeze = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDSTEP" || key == "CHAT_CMD_STEP" || key == "CHATSTEP" ||
        key == "CHATCMDSTEPFRAME" || key == "CHAT_O" || key == "CHATO") {
        chat.cmdStep = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDKEEPHITBOX" || key == "CHAT_CMD_KEEP_HITBOX" || key == "CHATKEEPHITBOX" ||
        key == "CHATKEEP" || key == "CHATCMDPERSISTHITBOX" || key == "CHAT_J" || key == "CHATJ") {
        chat.cmdKeepHitbox = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDPANEL" || key == "CHAT_CMD_PANEL" || key == "CHATPANEL" ||
        key == "CHATCMDFILTER" || key == "CHATFILTER" || key == "CHAT_P" || key == "CHATP") {
        chat.cmdPanel = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDOVERLAY" || key == "CHAT_CMD_OVERLAY" || key == "CHATOVERLAY" ||
        key == "CHATSHOWOVERLAY" || key == "CHATCMDHUD" || key == "CHATHUD") {
        chat.cmdOverlay = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDFREEZEBURST" || key == "CHAT_CMD_FREEZE_BURST" || key == "CHATFREEZEBURST" ||
        key == "CHATCMDBURST" || key == "CHATBURST") {
        chat.cmdFreezeBurst = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDSTEPBACK" || key == "CHAT_CMD_STEP_BACK" || key == "CHATSTEPBACK" ||
        key == "CHAT_L" || key == "CHATL") {
        chat.cmdStepBack = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "CHATCMDHISTSTACK" || key == "CHAT_CMD_HIST_STACK" || key == "CHATHISTSTACK" ||
        key == "CHATCMDSTACK" || key == "CHATSTACK" || key == "CHATCMDTRAIL") {
        chat.cmdHistStack = ParseChatCmdValue(rawValTrimmed);
        return true;
    }
    if (key == "GAMEPADPANEL" || key == "GAMEPAD_PANEL" || key == "CONTROLLERPANEL") {
        gamepadPanel = std::atoi(strippedVal.c_str()) != 0;
        return true;
    }
    return false;
}

bool ReadIniFileContent(const std::string& iniPath, std::string& outContent) {
    outContent.clear();
    FILE* f = _fsopen(iniPath.c_str(), "rb", _SH_DENYNO);
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 65536) {
        outContent.resize(static_cast<std::size_t>(sz));
        const std::size_t got = std::fread(&outContent[0], 1, static_cast<std::size_t>(sz), f);
        outContent.resize(got);
    }
    std::fclose(f);
    return true;
}

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
        g_logPath = g_dllDir + "\\mhw_hfi.log";
    } else {
        g_dllDir = ".";
        g_logPath = "mhw_hfi.log";
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
    if (s == "BACKSLASH" || s == "OEM_5") return VK_OEM_5;
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
    // Preserve previous non-empty session log as mhw_hfi.prev.log before truncating
    const std::string prevPath = (g_dllDir.empty() || g_dllDir == ".")
                                     ? std::string("mhw_hfi.prev.log")
                                     : (g_dllDir + "\\mhw_hfi.prev.log");
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExA(g_logPath.c_str(), GetFileExInfoStandard, &fad) &&
        (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 0)) {
        ::MoveFileExA(g_logPath.c_str(), prevPath.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
    g_logFile = _fsopen(g_logPath.c_str(), "w", _SH_DENYNO);
    if (g_logFile) {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        std::fprintf(g_logFile, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] mhw_hfi plugin attached (dir=%s)\n",
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
    const std::string iniPath = DllDirectory() + "\\mhw_hfi.ini";
    {
        std::lock_guard<std::mutex> lk(g_hotCfgMx);
        g_lastIniStamp = QueryIniStamp(iniPath);
    }
    std::string content;
    if (!ReadIniFileContent(iniPath, content)) {
        Log("config: no mhw_hfi.ini found; using defaults (Vis=U, Freeze=I, World=K, Keep=J, Step=O, Filter=P; chat U=\"%s\", I=\"%s\", K=\"%s\", O=\"%s\", J=\"%s\", P=\"%s\")",
            cfg.chat.cmdVis.c_str(), cfg.chat.cmdFreeze.c_str(), cfg.chat.cmdWorldFreeze.c_str(),
            cfg.chat.cmdStep.c_str(), cfg.chat.cmdKeepHitbox.c_str(), cfg.chat.cmdPanel.c_str());
        return cfg;
    }

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
        const std::string rawVal = Trim(line.substr(eq + 1));
        std::string val = rawVal;
        if (!val.empty() && val[0] == ';') {
            val = ";";
        } else {
            const std::size_t semi = val.find(';');
            if (semi != std::string::npos) val = Trim(val.substr(0, semi));
        }

        if (TryApplyChatOrPadKey(key, rawVal, val, cfg.chat, cfg.gamepadPanel)) continue;
        if (key == "ENABLED") cfg.enabled = std::atoi(val.c_str()) != 0;
        else if (key == "KEYTOGGLEVIS" || key == "KEY_VIS") cfg.vkToggleVis = ParseVirtualKey(val, cfg.vkToggleVis);
        else if (key == "KEYTOGGLEFREEZE" || key == "KEY_FREEZE") cfg.vkToggleFreeze = ParseVirtualKey(val, cfg.vkToggleFreeze);
        else if (key == "KEYFREEZEOTHERS" || key == "KEY_FREEZE_OTHERS" || key == "KEYWORLDFREEZE")
            cfg.vkFreezeOthers = ParseVirtualKey(val, cfg.vkFreezeOthers);
        else if (key == "KEYPERSISTHITBOX" || key == "KEYKEEPHITBOX" || key == "KEY_KEEP" || key == "KEY_PERSIST")
            cfg.vkPersistHitbox = ParseVirtualKey(val, cfg.vkPersistHitbox);
        else if (key == "KEYSTEPFRAME" || key == "KEY_STEP") cfg.vkStepFrame = ParseVirtualKey(val, cfg.vkStepFrame);
        else if (key == "KEYSTEPBACK" || key == "KEY_STEP_BACK" || key == "KEYBACKFRAME")
            cfg.vkStepBack = ParseVirtualKey(val, cfg.vkStepBack);
        else if (key == "KEYHISTSTACK" || key == "KEY_HIST_STACK" || key == "KEYSTACK" || key == "KEYTRAIL")
            cfg.vkHistStack = ParseVirtualKey(val, cfg.vkHistStack);
        else if (key == "KEYCYCLEFILTER" || key == "KEY_FILTER") cfg.vkCycleFilter = ParseVirtualKey(val, cfg.vkCycleFilter);
        else if (key == "KEYTOGGLEOVERLAY" || key == "KEY_OVERLAY" || key == "KEYOVERLAY" || key == "KEYSHOWOVERLAY" || key == "KEYTOGGLEHUD")
            cfg.vkToggleOverlay = ParseVirtualKey(val, cfg.vkToggleOverlay);
        else if (key == "STARTVISON") cfg.startVisOn = std::atoi(val.c_str()) != 0;
        else if (key == "KEEPHITBOXESACTIVE" || key == "PERSISTHITBOXES" || key == "KEEP_HITBOXES_ACTIVE")
            cfg.keepHitboxesActive = std::atoi(val.c_str()) != 0;
        else if (key == "WORLDFREEZEDELAYEDBURST" || key == "WORLD_FREEZE_DELAYED_BURST" || key == "FREEZEBURST" ||
                 key == "DELAYEDBURST")
            cfg.worldFreezeDelayedBurst = std::atoi(val.c_str()) != 0;
        else if (key == "BLOCKCAMERAKEYS" || key == "DISABLECAMERAKEYS" || key == "BLOCK_CAMERA_KEYS")
            cfg.blockCameraKeys = std::atoi(val.c_str()) != 0;
        else if (key == "SHOWHUDBANNER") cfg.showHudBanner = std::atoi(val.c_str()) != 0;
        else if (key == "LINETHICKNESS") {
            float t = static_cast<float>(std::atof(val.c_str()));
            if (t >= 0.8f && t <= 8.0f) cfg.lineThickness = t;
        } else if (key == "OPACITYSCALE") {
            float o = static_cast<float>(std::atof(val.c_str()));
            if (o >= 0.1f && o <= 1.5f) cfg.opacityScale = o;
        } else if (key == "SBCSURFACEOPACITY" || key == "SBC_SURFACE_OPACITY" || key == "SBCSURFOPACITY") {
            float so = static_cast<float>(std::atof(val.c_str()));
            if (so >= 0.1f && so <= 3.0f) cfg.sbcSurfaceOpacity = so;
        } else if (key == "HDRNITS" || key == "HDR_NITS") {
            float n = static_cast<float>(std::atof(val.c_str()));
            if (n == 0.0f || (n >= 40.0f && n <= 1000.0f)) cfg.hdrNits = n;
        } else if (key == "CATEGORYMASK" || key == "DEFAULTMASK" || key == "VISMASK") {
            long m = std::strtol(val.c_str(), nullptr, 0);
            if (m >= 0 && m <= 0xFF) cfg.defaultVisMask = static_cast<std::uint32_t>(m);
        } else if (key == "SBCRADIUSMETERS" || key == "SBC_RADIUS_METERS") {
            float r = static_cast<float>(std::atof(val.c_str()));
            if (r <= 18.0f) cfg.defaultSbcRadiusIdx = 0;
            else if (r <= 35.0f) cfg.defaultSbcRadiusIdx = 1;
            else if (r <= 60.0f) cfg.defaultSbcRadiusIdx = 2;
            else cfg.defaultSbcRadiusIdx = 3;
        } else if (key == "STARTHIDEMONSTER" || key == "HIDEMONSTERMESH") {
            cfg.startHideMonster = std::atoi(val.c_str()) != 0;
        } else if (key == "STARTHIDESTAGE" || key == "HIDESTAGEMESH" || key == "HIDEMAPMESH") {
            int s = std::atoi(val.c_str());
            if (s >= 0 && s < static_cast<int>(StageHideMode::Count)) {
                cfg.startStageHide = static_cast<StageHideMode>(s);
            } else if (s != 0) {
                cfg.startStageHide = StageHideMode::KeepInteract;
            }
        } else if (key == "STARTBACKDROP" || key == "BACKDROPMODE") {
            int b = std::atoi(val.c_str());
            if (b >= 0 && b < static_cast<int>(BackdropMode::Count)) {
                cfg.startBackdrop = static_cast<BackdropMode>(b);
            }
        } else if (key == "DEPTHGUIDEMODE" || key == "STARTDEPTHMODE" || key == "SBCSTYLE") {
            int d = std::atoi(val.c_str());
            if (d >= 0 && d < static_cast<int>(DepthGuideMode::Count)) {
                cfg.startDepthMode = static_cast<DepthGuideMode>(d);
            }
        } else if (key == "SBCMESHMODE" || key == "SBCMESHTIER" || key == "SBC_MESH_TIER" || key == "SBCTIER") {
            int tm = std::atoi(val.c_str());
            if (tm >= 0 && tm < static_cast<int>(SbcMeshTier::Count)) {
                cfg.startSbcTier = static_cast<SbcMeshTier>(tm);
            }
        } else if (key == "SPECIALSBCSTYLE" || key == "SPECIAL_SBC_STYLE" || key == "SPECIALSBC") {
            int sp = std::atoi(val.c_str());
            if (sp >= 0 && sp < static_cast<int>(SpecialSbcStyle::Count)) {
                cfg.startSpecialSbc = static_cast<SpecialSbcStyle>(sp);
            }
        } else if (key == "EMPARTMODE" || key == "EM_PART_MODE" || key == "MONSTERPARTMODE") {
            int pm = std::atoi(val.c_str());
            if (pm >= 0 && pm < static_cast<int>(EmPartMode::Count)) {
                cfg.startEmPartMode = static_cast<EmPartMode>(pm);
            }
        } else if (key == "EMUNSELECTEDSTYLE" || key == "EM_UNSELECTED_STYLE") {
            int us = std::atoi(val.c_str());
            if (us >= 0 && us < static_cast<int>(EmUnselectedStyle::Count)) {
                cfg.startEmUnsStyle = static_cast<EmUnselectedStyle>(us);
            }
        } else if (key == "EMPARTLABELS" || key == "EM_PART_LABELS") {
            int lb = std::atoi(val.c_str());
            if (lb >= 0 && lb < static_cast<int>(EmPartLabelMode::Count)) {
                cfg.startEmLabelMode = static_cast<EmPartLabelMode>(lb);
            }
        } else if (key == "ENTITYLINESCALE" || key == "ENTITY_LINE_SCALE" || key == "MONSTERLINESCALE") {
            float sc = static_cast<float>(std::atof(val.c_str()));
            if (sc >= 0.25f && sc <= 4.0f) {
                cfg.defaultEntityLineIdx = NearestLineScaleIdx(sc);
            }
        } else if (key == "SBCLINESCALE" || key == "SBC_LINE_SCALE" || key == "TERRAINLINESCALE" || key == "RAWLINESCALE") {
            float sc = static_cast<float>(std::atof(val.c_str()));
            if (sc >= 0.25f && sc <= 4.0f) {
                cfg.defaultSbcLineIdx = NearestLineScaleIdx(sc);
            }
        } else if (key == "HISTORYRECORDMODE" || key == "HISTRECORDMODE" || key == "HISTORY_RECORD_MODE") {
            int rm = std::atoi(val.c_str());
            if (rm >= 0 && rm < static_cast<int>(HistRecordMode::Count)) {
                cfg.startHistRecMode = static_cast<HistRecordMode>(rm);
            }
        } else if (key == "HISTORYSTACKFRAMES" || key == "HISTSTACKFRAMES" || key == "HISTORY_STACK_FRAMES" ||
                   key == "STACKFRAMES") {
            cfg.startHistStackIdx = NearestHistStackIdx(std::atoi(val.c_str()));
        } else if (key == "HISTORYSTACKSCOPE" || key == "HISTSTACKSCOPE" || key == "HISTORY_STACK_SCOPE" ||
                   key == "STACKSCOPE") {
            int ss = std::atoi(val.c_str());
            if (ss >= 0 && ss < static_cast<int>(HistStackScope::Count)) {
                cfg.startHistStackScope = static_cast<HistStackScope>(ss);
            }
        }
    }

    Log("config: loaded mhw_hfi.ini (enabled=%d, vkVis=0x%02X, vkFreeze=0x%02X, vkWorld=0x%02X, vkKeep=0x%02X, vkStep=0x%02X, vkBack=0x%02X, vkStack=0x%02X, vkPanel=0x%02X, vkOverlay=0x%02X, mask=0x%02X, burst=%d, histRec=%d, histStack=%dF, histScope=%d; chat en=%d cs=%d U=\"%s\" I=\"%s\" K=\"%s\" O=\"%s\" L=\"%s\" ;=\"%s\" J=\"%s\" P=\"%s\" \\=\"%s\" burstCmd=\"%s\" pad=%d)",
        cfg.enabled ? 1 : 0, cfg.vkToggleVis, cfg.vkToggleFreeze, cfg.vkFreezeOthers,
        cfg.vkPersistHitbox, cfg.vkStepFrame, cfg.vkStepBack, cfg.vkHistStack, cfg.vkCycleFilter, cfg.vkToggleOverlay, cfg.defaultVisMask,
        cfg.worldFreezeDelayedBurst ? 1 : 0,
        static_cast<int>(cfg.startHistRecMode), HistStackFramesFromIdx(cfg.startHistStackIdx), static_cast<int>(cfg.startHistStackScope),
        cfg.chat.enabled ? 1 : 0, cfg.chat.caseSensitive ? 1 : 0,
        cfg.chat.cmdVis.c_str(), cfg.chat.cmdFreeze.c_str(), cfg.chat.cmdWorldFreeze.c_str(),
        cfg.chat.cmdStep.c_str(), cfg.chat.cmdStepBack.c_str(), cfg.chat.cmdHistStack.c_str(),
        cfg.chat.cmdKeepHitbox.c_str(), cfg.chat.cmdPanel.c_str(),
        cfg.chat.cmdOverlay.c_str(), cfg.chat.cmdFreezeBurst.c_str(), cfg.gamepadPanel ? 1 : 0);
    return cfg;
}

bool HotReloadRuntimeConfig(ChatCommandConfig& ioChat, bool& ioGamepadPanel) {
    const std::string iniPath = DllDirectory() + "\\mhw_hfi.ini";
    const IniStamp curStamp = QueryIniStamp(iniPath);

    std::lock_guard<std::mutex> lk(g_hotCfgMx);
    if (g_lastIniStamp.initialized && curStamp.SameAs(g_lastIniStamp)) {
        return false;
    }

    ChatCommandConfig nextChat{};
    bool nextGamepad = true;

    std::string content;
    if (curStamp.exists) {
        if (!ReadIniFileContent(iniPath, content)) {
            // Editor may still be flushing file; keep old stamp and retry on next event
            return false;
        }
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
            const std::string rawVal = Trim(line.substr(eq + 1));
            std::string val = rawVal;
            if (!val.empty() && val[0] == ';') {
                val = ";";
            } else {
                const std::size_t semi = val.find(';');
                if (semi != std::string::npos) val = Trim(val.substr(0, semi));
            }
            TryApplyChatOrPadKey(key, rawVal, val, nextChat, nextGamepad);
        }
    }

    g_lastIniStamp = curStamp;
    ioChat         = nextChat;
    ioGamepadPanel = nextGamepad;

    Log("config: hot-reloaded mhw_hfi.ini (%s; chat en=%d cs=%d U=\"%s\" I=\"%s\" K=\"%s\" O=\"%s\" L=\"%s\" ;=\"%s\" J=\"%s\" P=\"%s\" \\=\"%s\" burstCmd=\"%s\" pad=%d)",
        curStamp.exists ? "updated" : "removed -> defaults",
        nextChat.enabled ? 1 : 0, nextChat.caseSensitive ? 1 : 0,
        nextChat.cmdVis.c_str(), nextChat.cmdFreeze.c_str(), nextChat.cmdWorldFreeze.c_str(),
        nextChat.cmdStep.c_str(), nextChat.cmdStepBack.c_str(), nextChat.cmdHistStack.c_str(),
        nextChat.cmdKeepHitbox.c_str(), nextChat.cmdPanel.c_str(),
        nextChat.cmdOverlay.c_str(), nextChat.cmdFreezeBurst.c_str(), nextGamepad ? 1 : 0);
    return true;
}

std::uint32_t PresetMask(PresetId p) noexcept {
    switch (p) {
        case PresetId::Combat:   return kCatMaskCombat;
        case PresetId::PlEmOnly: return kCatMaskPlEmOnly;
        case PresetId::MapOnly:  return kCatMaskMapOnly;
        case PresetId::Hitbox:   return kCatMaskHitbox;
        case PresetId::Hurtbox:  return kCatMaskHurtbox;
        case PresetId::All:      return kCatMaskAll;
        default:                 return kCatMaskCombat;
    }
}

const char* PresetName(PresetId p) noexcept {
    switch (p) {
        case PresetId::Combat:   return "PL+EM COMBAT";
        case PresetId::PlEmOnly: return "PL+EM ONLY";
        case PresetId::MapOnly:  return "MAP ONLY";
        case PresetId::Hitbox:   return "HITBOX ONLY";
        case PresetId::Hurtbox:  return "HURTBOX ONLY";
        case PresetId::All:      return "ALL (8/8)";
        default:                 return "PL+EM COMBAT";
    }
}

const char* MatchPresetName(std::uint32_t visMask) noexcept {
    const std::uint32_t m = visMask & kCatMaskAll;
    if (m == kCatMaskCombat)   return "PL+EM COMBAT";
    if (m == kCatMaskPlEmOnly) return "PL+EM ONLY";
    if (m == kCatMaskMapOnly)  return "MAP ONLY";
    if (m == kCatMaskHitbox)   return "HITBOX ONLY";
    if (m == kCatMaskHurtbox)  return "HURTBOX ONLY";
    if (m == kCatMaskAll)      return "ALL (8/8)";
    if (m == 0)                return "NONE (0/8)";
    return "CUSTOM";
}

const char* StageHideModeName(StageHideMode m) noexcept {
    switch (m) {
        case StageHideMode::Off:          return "OFF";
        case StageHideMode::KeepInteract: return "KEEP INTERACT";
        case StageHideMode::HideAll:      return "HIDE ALL MAP";
        default:                          return "OFF";
    }
}

const char* BackdropModeName(BackdropMode m) noexcept {
    switch (m) {
        case BackdropMode::Off:       return "OFF";
        case BackdropMode::HidePlVfx: return "HIDE PL/VFX";
        case BackdropMode::HideAll3D: return "HIDE ALL 3D";
        case BackdropMode::DimScrim:  return "DIM 45%";
        default:                      return "OFF";
    }
}

const char* DepthGuideModeName(DepthGuideMode m) noexcept {
    switch (m) {
        case DepthGuideMode::Auto:          return "SEMI 3D+RINGS";
        case DepthGuideMode::Opaque:        return "OPAQUE 3D+RING";
        case DepthGuideMode::OpaqueNoRings: return "OPAQUE 3D ONLY";
        case DepthGuideMode::AlwaysRings:   return "SEMI+RINGS ON";
        case DepthGuideMode::NoRings:       return "SEMI 3D ONLY";
        case DepthGuideMode::RawWire:       return "RAW WIREFRAME";
        default:                            return "SEMI 3D+RINGS";
    }
}

const char* SbcMeshTierName(SbcMeshTier t) noexcept {
    switch (t) {
        case SbcMeshTier::LowOnly:  return "LOW ONLY";
        case SbcMeshTier::Both:     return "LOW + HIGH";
        case SbcMeshTier::HighOnly: return "HIGH ONLY";
        default:                    return "LOW + HIGH";
    }
}

const char* SpecialSbcStyleName(SpecialSbcStyle s) noexcept {
    switch (s) {
        case SpecialSbcStyle::Off:    return "OFF";
        case SpecialSbcStyle::Ghost:  return "GHOST (96% T)";
        case SpecialSbcStyle::Light:  return "LIGHT (93% T)";
        case SpecialSbcStyle::Medium: return "MED (85% T)";
        case SpecialSbcStyle::Vivid:  return "VIVID (72% T)";
        case SpecialSbcStyle::Opaque: return "OPAQUE (0% T)";
        default:                      return "LIGHT (93% T)";
    }
}

const char* EmPartModeName(EmPartMode m) noexcept {
    switch (m) {
        case EmPartMode::Off:        return "OFF";
        case EmPartMode::Hitzone:    return "HITZONE (FINE)";
        case EmPartMode::ClutchZone: return "CLUTCH (SOFTEN)";
        default:                     return "OFF";
    }
}

const char* EmUnselectedStyleName(EmUnselectedStyle s) noexcept {
    switch (s) {
        case EmUnselectedStyle::Hide:  return "HIDE";
        case EmUnselectedStyle::Ghost: return "GHOST OUTLINE";
        default:                       return "HIDE";
    }
}

const char* EmPartLabelModeName(EmPartLabelMode l) noexcept {
    switch (l) {
        case EmPartLabelMode::Full:     return "PHYS + ELEM + EXT";
        case EmPartLabelMode::PhysOnly: return "PHYS + EXTRACT";
        case EmPartLabelMode::IdOnly:   return "ID + EXTRACT";
        case EmPartLabelMode::Off:      return "OFF";
        default:                        return "PHYS + ELEM + EXT";
    }
}

const char* HistRecordModeName(HistRecordMode m) noexcept {
    switch (m) {
        case HistRecordMode::Off:           return "OFF";
        case HistRecordMode::LiveAndSteps:  return "LIVE + STEPS";
        case HistRecordMode::AllInclWfMove: return "ALL (+WF MOVE)";
        default:                            return "LIVE + STEPS";
    }
}

const char* HistStackScopeName(HistStackScope s) noexcept {
    switch (s) {
        case HistStackScope::HitboxOnly:   return "ATK HITBOX";
        case HistStackScope::AtkAndPlHurt: return "ATK + PL HURT";
        case HistStackScope::HurtboxOnly:  return "HURTBOXES";
        case HistStackScope::AllEntities:  return "ALL ENTITIES";
        default:                           return "ATK HITBOX";
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
