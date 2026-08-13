# BG3Access Shared AI Rules

This file contains project rules that any coding assistant may obey when working on BG3Access. Agent-specific behavior belongs in that agent's own file, such as `AGENTS.md` for Codex or `CLAUDE.md` for Claude.

## Core Architecture

- C++ is the sensor/snapshot layer. It observes Noesis/UI state, handles pointer lifetime, SEH protection, widget/focus/selection/tooltip detection, and sends plain snapshot data to Lua.
- Lua is the speech/policy layer. It interprets snapshots, chooses what to speak, manages handler state, and routes speech through SpeechData/Tolk.
- Lua must not hold Noesis object references across frames. Treat Noesis objects as tick-local only; prefer snapshot data and stable plain data.
- Do not add menu-specific or character-creation-specific logic to C++ unless the mechanism is generic sensor plumbing.
- Prefer BG3SE APIs such as `Ext.StaticData`, `Ext.Stats`, and `Ext.Entity` over Noesis text extraction when an API source exists.

## SpeechData Role Architecture

SpeechData is BG3Access's semantic speech layer, similar in spirit to ARIA roles. C++ provides snapshots and structured tooltip entries; Lua maps those facts into SpeechData core fields and properties. SpeechData owns ordering, verbosity, deltas, tooltip cross-off, deduplication, and final Tolk calls.

Rules:

- All speech output goes through `SpeechData:Speak()` or `SpeechData.Alert(...)`.
- Do not call `Ext.Tolk.Speak()` directly outside SpeechData.
- `Ext.Tolk.Silence()` remains acceptable for explicit silence/cancel behavior.
- Do not invent ad-hoc SpeechData core fields.
- Use the fixed core field vocabulary in `Client\SpeechData.lua`.
- Use `AddProperty(label, value, tier)` for flexible game-stat data.
- Use `SpeechData.FromTooltip(...)` for structured tooltip `{role, text}` entries instead of per-handler text/role matching, except for documented empty-role exceptions.

## C++ Safety Rules

Any C++ code touching Noesis pointers must follow the established SEH inner/invoke/wrapper pattern:

- `_Inner` does the work and may use C++ objects with destructors.
- `_Invoke` has no destructors and forwards through pointers.
- the wrapper uses `__try`/`__except` around `_Invoke` and logs faults.

Do not put locally scoped C++ objects with destructors inside `__try` functions.

## Lua Structure Rules

- Keep handler ownership clear. Use `Dispatcher.lua` routing patterns instead of reintroducing per-menu routing drift.
- Use the existing SpeechData, Scheduler, Logger, Helpers, and dispatcher patterns before adding new abstractions.
- For one-shot deferred work, prefer `Client\Scheduler.lua` over ad-hoc `Ext.Events.Tick` counters or uncancelable timer callbacks.
- Keep C++ snapshots plain and Lua policy-rich.

## Build And Test Expectations

- The user builds the Script Extender in Visual Studio 2022 unless explicitly requested otherwise.
- Do not run command-line C++ builds by default.
- For behavior changes, provide an in-game test plan that names the menu, screen, controller action, or narration path to verify.
- Preserve working accessible behavior unless the task explicitly changes it.

## Useful Context Paths

- C++ extender: `D:\Repositories\bg3se-SR`
- Lua mod: `D:\SteamLibrary\steamapps\common\Baldurs Gate 3\Data\Mods\BG3Access_a8cddf0c-2e61-1b7c-5c0c-275d46073949\ScriptExtender\lua`
- Claude memory index: `C:\Users\jlove\.claude\projects\D--Repositories-bg3se-SR\memory\MEMORY.md`

## Handoff Expectations

Before editing, an assistant should be able to state:

- what files were read
- what the current behavior appears to be
- which files are proposed for editing
- which invariants must not be broken
- how the user can test the change in BG3
- what remains uncertain