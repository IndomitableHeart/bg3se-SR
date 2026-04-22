#pragma once

#include <GameDefinitions/Base/WindowsSlim.h>

#include <memory>
#include <cstdint>
#include <array>
#include <vector>
#include <set>
#include <map>
#include <string>
#include <sstream>
#include <cassert>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <variant>
#include <span>

#include <Extender/BuildInfo.h>
#include <Extender/Shared/Utils.h>
#include <GameDefinitions/Base/Base.h>
#include <Extender/Shared/Optick.h>

// BG3Access diagnostic logging macros.
//
// Two tiers, both gated on BG3ACCESS_VERBOSE at compile time so game
// release builds compile everything out (zero runtime cost).  Within a
// VERBOSE-compiled dev build:
//
//   BG3A_LOG   - feature-level events (speech decisions, state
//                transitions, SEH faults).  Always fires.
//
//   BG3A_TRACE - high-volume diagnostic noise (widget enumeration,
//                tick dumps, NameScope reads, focus-source details).
//                Gated on the runtime sBG3A_TraceEnabled flag set
//                via Ext.UI.SetTraceLogging(bool) from Lua.
//                Flip on just before reproducing a specific issue,
//                flip off after.  L3+R3 in-game cycles the flag
//                alongside Lua's Log.Debug level (Logger.lua).
//
// The trace flag lives inside this #ifdef so release builds don't even
// declare the extern symbol.
#ifdef BG3ACCESS_VERBOSE
extern bool sBG3A_TraceEnabled;
#define BG3A_LOG(...)   WARN(__VA_ARGS__)
#define BG3A_TRACE(...) do { if (sBG3A_TraceEnabled) WARN(__VA_ARGS__); } while(0)
#else
#define BG3A_LOG(...)   ((void)0)
#define BG3A_TRACE(...) ((void)0)
#endif
