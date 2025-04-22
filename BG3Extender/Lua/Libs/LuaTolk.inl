#pragma once

#include <lua.hpp>
#include <lauxlib.h>

#include <CoreLib/base/BaseUtilities.h>
#include <Lua/Shared/LuaModule.h>
#include <Lua/Libs/LibraryRegistrationHelpers.h>
#include <Lua/Shared/LuaMethodCallHelpers.h> // For UserReturn
#include <Extender/Shared/Utils.h>

#include <string>
#include <vector>
#include <Windows.h>
#include <stdbool.h>
#include <atomic>
#include <mutex>
#include <exception>
#include <sstream>

/// <lua_module>Tolk</lua_module>
BEGIN_NS(lua::tolk)

enum class TolkState {
    Disabled, // Feature globally disabled via config
    Idle,     // Feature enabled, but Tolk DLL not loaded/initialized
    Loaded,   // Feature enabled, Tolk DLL loaded and initialized
    Failed    // Feature enabled, Tolk DLL load/initialization failed
};

// Global flag controlled externally via SetAccessibilityEnabled based on extender config.
static std::atomic<bool> g_accessibilityEnabled = false;
// Internal operational state of the Tolk integration.
static std::atomic<TolkState> g_tolkState = TolkState::Disabled;

// --- Tolk Function Pointer Type Definitions ---
typedef void (*Tolk_Load_t)();
typedef void (*Tolk_Unload_t)();
typedef bool (*Tolk_Output_t)(const wchar_t* str, bool interrupt);
typedef bool (*Tolk_Speak_t)(const wchar_t* str, bool interrupt);
typedef bool (*Tolk_Silence_t)();

// --- Static Variables for Tolk DLL Handle and Function Pointers ---
static HMODULE g_tolkModule = nullptr;
static Tolk_Load_t g_Tolk_Load = nullptr;
static Tolk_Unload_t g_Tolk_Unload = nullptr;
static Tolk_Output_t g_Tolk_Output = nullptr;
static Tolk_Speak_t g_Tolk_Speak = nullptr;
static Tolk_Silence_t g_Tolk_Silence = nullptr;

// Protects g_tolkModule handle and function pointers during init/shutdown and access from Lua calls.
static std::mutex g_tolkFunctionPtrMutex;

// --- Tolk Function Mapping (Internal Helper) ---
namespace {
    // Maps function names to their corresponding global pointer variables for GetProcAddress.
    struct TolkFunctionMapping {
        const char* name;
        void** pointerAddress;
    };
    TolkFunctionMapping g_tolkFunctionMap[] = {
        {"Tolk_Load", (void**)&g_Tolk_Load},
        {"Tolk_Unload", (void**)&g_Tolk_Unload},
        {"Tolk_Output", (void**)&g_Tolk_Output},
        {"Tolk_Speak", (void**)&g_Tolk_Speak},
        {"Tolk_Silence", (void**)&g_Tolk_Silence}
    };
    constexpr size_t g_numTolkFuncs = sizeof(g_tolkFunctionMap) / sizeof(g_tolkFunctionMap[0]);
} // end anonymous namespace

// --- String Conversion Helpers ---
namespace {
    std::wstring LuaToWStringWin(lua_State* L, int index) {
        size_t len = 0;
        const char* utf8Str = luaL_checklstring(L, index, &len);
        if (!utf8Str || len == 0) return L"";
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str, (int)len, NULL, 0);
        if (wideLen == 0) luaL_error(L, "Tolk: Failed to calculate wide char buffer size");
        std::wstring wideStr;
        wideStr.resize(wideLen);
        int result = MultiByteToWideChar(CP_UTF8, 0, utf8Str, (int)len, &wideStr[0], wideLen);
        if (result == 0) luaL_error(L, "Tolk: Failed to convert UTF-8 to wide char");
        return wideStr;
    }

    void WCharStringToLua(lua_State* L, const wchar_t* wideStr) {
        if (!wideStr || wideStr[0] == L'\0') {
            lua_pushstring(L, "");
            return;
        }
        int wideLen = -1;
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideStr, wideLen, NULL, 0, NULL, NULL);
        if (utf8Len == 0) luaL_error(L, "Tolk: Failed to calculate UTF-8 buffer size");
        std::vector<char> utf8Buffer(utf8Len);
        int result = WideCharToMultiByte(CP_UTF8, 0, wideStr, wideLen, utf8Buffer.data(), utf8Len, NULL, NULL);
        if (result == 0) luaL_error(L, "Tolk: Failed to convert wide char to UTF-8");
        lua_pushlstring(L, utf8Buffer.data(), utf8Len - 1);
    }
} // end anonymous namespace

// --- Global Tolk Initialization/Shutdown (Called by C++) ---

// Should be called during C++ Lua VM initialization (e.g., LuaResetInternal).
// Loads Tolk.dll and calls Tolk_Load() if feature is enabled via config.
void InitializeTolkGlobally() {
    if (!g_accessibilityEnabled.load()) {
        g_tolkState.store(TolkState::Disabled);
        return;
    }
    if (g_tolkState.load() == TolkState::Loaded) return;

    HMODULE tempModule = nullptr;
    Tolk_Load_t temp_Tolk_Load = nullptr;
    bool success = false;
    TolkState finalState = TolkState::Failed;
    std::vector<void*> tempFunctionPointers(g_numTolkFuncs, nullptr);

    tempModule = LoadLibraryW(L"Tolk.dll");
    if (tempModule != nullptr) {
        success = true;
        for (size_t i = 0; i < g_numTolkFuncs; ++i) {
            FARPROC procAddress = GetProcAddress(tempModule, g_tolkFunctionMap[i].name);
            tempFunctionPointers[i] = (void*)procAddress;
            if (tempFunctionPointers[i] == nullptr) {
                success = false;
                break;
            }
            if (strcmp(g_tolkFunctionMap[i].name, "Tolk_Load") == 0) {
                temp_Tolk_Load = (Tolk_Load_t)tempFunctionPointers[i];
            }
        }

        if (success && temp_Tolk_Load) {
            try {
                temp_Tolk_Load();
                finalState = TolkState::Loaded;
            } catch (...) {
                success = false;
            }
        } else {
             success = false;
        }
    } else {
        success = false;
    }

    {
        std::lock_guard<std::mutex> lock(g_tolkFunctionPtrMutex);
        if (success && finalState == TolkState::Loaded) {
            g_tolkModule = tempModule;
            for (size_t i = 0; i < g_numTolkFuncs; ++i) {
                *g_tolkFunctionMap[i].pointerAddress = tempFunctionPointers[i];
            }
        } else {
            if (tempModule != nullptr) {
                FreeLibrary(tempModule);
            }
            g_tolkModule = nullptr;
            for (size_t i = 0; i < g_numTolkFuncs; ++i) {
                *g_tolkFunctionMap[i].pointerAddress = nullptr;
            }
            finalState = TolkState::Failed;
        }
    }
    g_tolkState.store(finalState);
}

// Should be called during C++ Lua VM shutdown (e.g., LuaResetInternal).
// Calls Tolk_Unload() and unloads Tolk.dll if it was loaded.
void ShutdownTolkGlobally() {
    Tolk_Unload_t unloadFunc = nullptr;
    HMODULE moduleHandle = nullptr;
    TolkState currentState = g_tolkState.load();

    if (currentState != TolkState::Loaded) {
         g_tolkState.store(g_accessibilityEnabled.load() ? TolkState::Idle : TolkState::Disabled);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_tolkFunctionPtrMutex);
        unloadFunc = g_Tolk_Unload;
        moduleHandle = g_tolkModule;
        g_tolkModule = nullptr;
        for (size_t i = 0; i < g_numTolkFuncs; ++i) {
            *g_tolkFunctionMap[i].pointerAddress = nullptr;
        }
    }

    if (unloadFunc) {
        try {
            unloadFunc();
        } catch (...) {
            // Exception during unload, nothing critical to do
        }
    }

    if (moduleHandle != nullptr) {
        FreeLibrary(moduleHandle);
    }

    g_tolkState.store(g_accessibilityEnabled.load() ? TolkState::Idle : TolkState::Disabled);
}


// --- Lua Wrapper Functions ---

/// <lua_function>Ext.Tolk.IsLoaded</lua_function>
static UserReturn TolkIsLoaded(lua_State* L) {
    lua_pushboolean(L, g_tolkState.load() == TolkState::Loaded);
    return UserReturn(1);
}

/// <lua_function>Ext.Tolk.GetStatus</lua_function>
static UserReturn TolkGetStatus(lua_State* L) {
    TolkState currentState = g_tolkState.load();
    const char* statusStr = "Unknown";
    switch (currentState) {
        case TolkState::Disabled: statusStr = "Disabled"; break;
        case TolkState::Idle:     statusStr = "Idle";     break;
        case TolkState::Loaded:   statusStr = "Loaded";   break;
        case TolkState::Failed:   statusStr = "Failed";   break;
    }
    lua_pushstring(L, statusStr);
    return UserReturn(1);
}

/// <lua_function>Ext.Tolk.Output</lua_function>
static UserReturn TolkOutput(lua_State* L) {
    if (g_tolkState.load() != TolkState::Loaded) {
        lua_pushboolean(L, false);
        return UserReturn(1);
    }
    std::wstring wstr = LuaToWStringWin(L, 1);
    bool interrupt = lua_toboolean(L, 2);
    bool result = false;

    Tolk_Output_t funcPtr = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_tolkFunctionPtrMutex);
        funcPtr = g_Tolk_Output;
    }

    if (funcPtr) {
        try {
            result = funcPtr(wstr.c_str(), interrupt);
        } catch (...) {
            result = false;
        }
    }
    lua_pushboolean(L, result);
    return UserReturn(1);
}

/// <lua_function>Ext.Tolk.Speak</lua_function>
static UserReturn TolkSpeak(lua_State* L) {
     if (g_tolkState.load() != TolkState::Loaded) {
        lua_pushboolean(L, false);
        return UserReturn(1);
    }
    std::wstring wstr = LuaToWStringWin(L, 1);
    bool interrupt = lua_toboolean(L, 2);
    bool result = false;

    Tolk_Speak_t funcPtr = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_tolkFunctionPtrMutex);
        funcPtr = g_Tolk_Speak;
    }

    if (funcPtr) {
        try {
            result = funcPtr(wstr.c_str(), interrupt);
        } catch (...) {
            result = false;
        }
    }
    lua_pushboolean(L, result);
    return UserReturn(1);
}

/// <lua_function>Ext.Tolk.Silence</lua_function>
static UserReturn TolkSilence(lua_State* L) {
    if (g_tolkState.load() != TolkState::Loaded) {
        lua_pushboolean(L, false);
        return UserReturn(1);
    }
    bool result = false;

    Tolk_Silence_t funcPtr = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_tolkFunctionPtrMutex);
        funcPtr = g_Tolk_Silence;
    }

    if (funcPtr) {
        try {
            result = funcPtr();
        } catch (...) {
            result = false;
        }
    }
    lua_pushboolean(L, result);
    return UserReturn(1);
}

/// <lua_function>Ext.Tolk.IsEnabled</lua_function>
static UserReturn TolkIsEnabled(lua_State* L) {
    lua_pushboolean(L, g_accessibilityEnabled.load());
    return UserReturn(1);
}

// --- Library Registration ---
// Called by LuaSharedLibs (or similar) to register the Ext.Tolk module.
void RegisterTolkLib() {
    DECLARE_MODULE(Tolk, Both)
        BEGIN_MODULE()
        MODULE_NAMED_FUNCTION("IsEnabled", TolkIsEnabled)
        MODULE_NAMED_FUNCTION("IsLoaded", TolkIsLoaded)
        MODULE_NAMED_FUNCTION("GetStatus", TolkGetStatus)
        MODULE_NAMED_FUNCTION("Output", TolkOutput)
        MODULE_NAMED_FUNCTION("Speak", TolkSpeak)
        MODULE_NAMED_FUNCTION("Silence", TolkSilence)
        END_MODULE()
}

// --- External Control ---
// Call this from C++ code that manages extender configuration (e.g., dllmain).
void SetAccessibilityEnabled(bool enabled) {
    bool previous = g_accessibilityEnabled.exchange(enabled);
    if (previous != enabled) {
        // If state changed, update internal state and potentially trigger load/unload.
        if (!enabled && g_tolkState.load() == TolkState::Loaded) {
            ShutdownTolkGlobally(); // Shut down immediately if disabled while loaded
        } else if (enabled && g_tolkState.load() == TolkState::Disabled) {
             g_tolkState.store(TolkState::Idle); // Mark as ready for init on next Lua reset
        } else if (!enabled && g_tolkState.load() != TolkState::Disabled) {
             g_tolkState.store(TolkState::Disabled); // Ensure state reflects disabled status
        }
    }
}

END_NS() // End namespace lua::tolk