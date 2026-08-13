# BG3 Native UI Interception V6: Pre-Edit Findings

## Scope and evidence level

This document records the required read-only orientation before any production
C++ or Lua change.

- Static source inspection: complete for the task-relevant paths listed below.
- Ghidra project preflight: complete in read-only, no-analysis mode.
- Targeted reverse engineering: not yet started when this report was written.
- Runtime tracing: not performed.
- Highest evidence level in this report: source evidence and binary identity
  evidence only.

## Exact Git state

Native repository root:

`D:\Repositories\bg3se-SR`

State immediately before creating the task branch:

- Branch: `feature/tolk-bindings`
- HEAD: `f80b9a9fbcf27ccd2ba7df4fa3925cfd95bb6f6f`
- Tree: `6457a4173667690f2cff0fdafe163c39bcdb95b1`
- First parent: `af007ddca845c41003a9c1033f46a3939ba6aded`
- Second parent: `c1c7503d4923c16e324a7bc4ac6103d410315fa7`
- Subject: `Merge upstream main into feature/tolk-bindings`
- Tracking state: `origin/feature/tolk-bindings`, ahead by 25 commits
- Tags at HEAD: none
- Configured Git submodules: none; no tracked `.gitmodules` and no gitlink
  entries
- Tracked worktree and index: clean

Preserved pre-existing untracked entries:

- `.claude/`
- `AGENTS.md`
- `AI_SHARED_RULES.md`
- Git-quoted path `"D\357\200\272test_write.txt"`
- `settings.VisualStudio.json`

Remotes:

- `origin`: `https://github.com/IndomitableHeart/bg3se-SR.git`
- `upstream`: `https://github.com/Norbyte/bg3se.git`

Task branch created after orientation:

- Branch: `bg3-native-ui-interception-discovery`
- Branch point: `f80b9a9fbcf27ccd2ba7df4fa3925cfd95bb6f6f`
- No commit had been created when this report was written.

## Intentional source roots

Native C++ repository, writable for this task:

`D:\Repositories\bg3se-SR`

Live BG3Access Lua root, read-only for this task:

`D:\SteamLibrary\steamapps\common\Baldurs Gate 3\Data\Mods\BG3Access_a8cddf0c-2e61-1b7c-5c0c-275d46073949\ScriptExtender\lua`

The live root contained 37 files, including 35 Lua files, during orientation.
No file in that root is an edit target for V6.

## Ghidra and executable identity

Ghidra installation:

- Path: `D:\Repositories\bg3se-SR\tools\ghidra_12.1.2`
- Version: `12.1.2 PUBLIC`
- Version source: `Ghidra\application.properties:13,17`
- Java: Oracle JDK `21.0.7`, 64-bit
- Java path: `C:\Program Files\Java\jdk-21`

Precomputed project:

- Project root: `D:\Repositories\bg3se-SR\tools\ghidra_projects`
- Project name: `bg3_accessibility`
- Project marker: `tools\ghidra_projects\bg3_accessibility.gpr`
- Project data: `tools\ghidra_projects\bg3_accessibility.rep`
- Program: `/bg3_dx11.exe`
- Initial full Auto Analysis was completed before this task resumed.
- V6 will not rerun the completed full initial Auto Analysis.
- A headless preflight successfully opened the program with `-readOnly
  -noanalysis`.

Authoritative DX11 executable checkpoint, corrected and explicitly approved by
the user:

- Path: `D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe`
- Filesystem size: `104363072` bytes
- SHA-256:
  `e899c67cb90b9c6b0f052e3f758ba8615bc2012e52561c2af2e6a1e06ef61a2f`
- Product version: `4.1.1.7398727`
- File version: `1.0.0.0`
- PE kind: PE32+
- Machine: x86-64, `0x8664`
- PE timestamp: `0x6A21507E`, `2026-06-04T10:16:30Z`
- Sections: 7

The precomputed Ghidra Import Results recorded the same executable path and
SHA-256. The former V6 size `106644048` was Ghidra's import-summary `# of
Bytes`, not the filesystem file size. It is not used as an identity gate.

Only DX11 is in scope. Vulkan was not analyzed or validated.

## Current native-to-Lua event flow

The current flow is a C++ sensor followed by a plain-data Lua policy layer.

1. BG3SE resolves and wraps the client game-state update function in
   `BG3Extender\Extender\Client\ScriptExtenderClient.cpp:80-81`.
2. The post-update wrapper reaches the client extension state in
   `ScriptExtenderClient.cpp:298-313`.
3. Client Lua update processing calls the global focus monitor, replication
   post-update, then deferred UI delivery in
   `BG3Extender\Lua\Client\LuaClient.cpp:110-115`.
4. `GlobalFocusMonitor::Tick` begins at
   `BG3Extender\Lua\Libs\ClientUI\Module.inl:1821` and obtains the root at
   `Module.inl:1983`.
5. It gathers event flags and selected Noesis values into `TickSnapshot`.
   The data contract starts at `BG3Extender\Lua\Client\UIEvents.h:157-162`.
6. `FocusEventData` and `TickSnapshot` use owned `std::string`, vectors, and
   nested value records; see `UIEvents.h:19-116` and `UIEvents.h:162-300`.
7. `DeferredUIEvents::OnTickSnapshot` moves the snapshot into a queue at
   `BG3Extender\Lua\Client\UIEvents.inl:464-467`.
8. `DeferredUIEvents::PostUpdate` swaps queues and pushes Lua tables at
   `UIEvents.inl:378-429`.
9. Live Lua subscribes once through
   `Client\EventRouter.lua:1028-1056` and routes `TickSnapshot` records through
   `HandleTickSnapshot` at `EventRouter.lua:118-180`.
10. `Client\Dispatcher.lua:319-516` owns handler activation, liveness,
    stacking, re-instantiation handling, and snapshot dispatch.
11. Handlers build `SpeechData`; formatting, interrupt/queue policy, dedup
    state, verbosity, and Tolk emission remain in
    `Client\SpeechData.lua:907-1043` and `SpeechData.lua:1135-1159`.

The desired native interception layer must preserve this division: C++
observes and transports owned semantic facts; Lua retains policy and speech.

## Existing tree walks, rediscovery, reads, polling, and waits

### Widget discovery and lifecycle inference

- Loaded/Unloaded handlers maintain a 32-entry tracked widget array, declared
  around `Module.inl:265-293`.
- The first stable tick seeds it with `GatherWidgets_SEH`; the event-driven
  path then calls `ReadTrackedWidgets_SEH`, while a legacy fallback gathers
  every tick. See `Module.inl:2016-2058`.
- Visibility remains a per-tick DependencyProperty read even in the tracked
  path at `Module.inl:2016-2017`.
- A dynamic settle gate snapshots widget pointers and visibility, waits for
  five stable frames, and has a 30-frame ceiling at
  `Module.inl:2082-2176`.
- Post-settle extraction walks namescope/template content again at
  `Module.inl:2816-2851`.

### Focus and selection

- Event-driven GotFocus and SelectionChanged are the default, but forced,
  initial, post-settle, dialogue, and legacy paths still call
  `FindFocusedElement_SEH`, `FindIsFocused_SEH`, and
  `FindSelectedTab_SEH`; see `Module.inl:2272-2478`.
- `ClassSelectionDelegate` can run a bounded selected-container tree search
  for virtualized lists at `Module.inl:883-1043`.
- `GotFocusDelegate` performs a parent-chain walk to recover the containing
  UIWidget at `Module.inl:1091-1118`.
- Snapshot finalization may probe a cached pointer and fall back to another
  focus tree walk at `Module.inl:3438-3493`.

### DataContext and rendered-content reads

- INPC subscription is installed through `SafeSubscribeINPC_SEH` at
  `Module.inl:537-569`; callbacks mark dirty state and safe-state Tick code
  performs the read.
- Widget DataContexts are rediscovered from fresh widget lists before
  post-INPC extraction around `Module.inl:4022-4104`.
- `FindHUDWidgets_Inner` uses tracked top-level widget names
  `PartyLine_c`, `TargetInfo_c`, and `CursorText_c`, with container fallback,
  at `Module.inl:10537-10583`.
- `ReadDCPath` traverses reflected TypeProperty/DependencyProperty paths at
  `Module.inl:10668-11031`.
- `ReadTargetHudDCSnapshot` reads the target and cursor VM field set at
  `Module.inl:11044-11135`.
- `ReadElementPath` supports paths such as
  `TopFocusedElement.DataContext.Power` at `Module.inl:11139-11182`.
- Tooltip and generic text extraction still perform bounded rendered-tree
  scans, including the child TextBlock fallback at
  `Module.inl:10039-10057` and structured tooltip reads around
  `Module.inl:10273-10468`.

### Polling

- Radial LocalFocus, ActiveSearch LocalFocus, and context-menu state are
  polled after ten stable frames at `Module.inl:3225-3274`.
- ToolTip Opened/Closed class events only gate continued content polling;
  binding stabilization still happens inside `PollTooltip`, as documented at
  `Module.inl:3278-3308`.
- Hotbar radial selection is inferred by polling the `Tag` object address at
  `Module.inl:3310-3415`.
- Dialogue-specific selected-item polling is enabled from Lua and runs at
  `Module.inl:2346-2365`.

### Pointer retention

- GotFocus and SelectionChanged handlers retain raw Noesis pointers in static
  globals across the settle window at `Module.inl:188-210`.
- The widget tracker retains raw widget pointers at `Module.inl:265-293`.
- Widget identity and settle baselines retain pointer values as integer
  addresses at `Module.inl:4176-4187`.
- Comments assert same-thread stability, but runtime lock and destruction
  ordering have not yet been proven from the game binary.
- The Lua-facing snapshot converts identities to strings and does not expose a
  live Noesis reference across frames.

## Combat target timing split

Combat target selection demonstrates why native semantic events must not be
treated as equivalent to fully settled rendered data.

- Authoritative identity comes from camera/ECS state and the target VM.
- Cursor/TargetInfo UI properties are eventually consistent and update in
  stages.
- Live Lua records a pre-press camera UUID, CurrentTarget identity, and cursor
  hash in `Client\TargetSelect.lua:1574-1741`.
- Per-frame logic waits for camera, target identity, or cursor data to advance
  at `TargetSelect.lua:1826-1881`.
- It then requires 250 ms of unchanged cursor-info state, bounded by a 900 ms
  hard ceiling, at `TargetSelect.lua:1883-1936`.
- The observed pipeline documented in source is roughly 120 ms for action
  transition, roughly 320 ms for hit chance/warnings, and roughly 360 ms for a
  final adjustment; see `TargetSelect.lua:42-51` and
  `TargetSelect.lua:1915-1928`.
- The eventual read uses the plain table returned by
  `Ext.UI.ReadTargetHudDCSnapshot`; see `TargetSelect.lua:330-355`.

A deeper property-publication hook could make accumulation event-driven, but
it cannot assume the first property mutation is the final combat description.

## Existing class-wide interception

Current C++ already inserts class handlers directly into Noesis metadata:

- Selector `SelectionChanged`: discovered and registered at
  `Module.inl:4430-4456`.
- UIElement `GotFocus`: discovered and registered at
  `Module.inl:4527-4554`.
- `ls.UIWidget` Loaded/Unloaded: registered at
  `Module.inl:4566-4665`.
- FrameworkElement content Loaded/Unloaded: registered at
  `Module.inl:4686-4693`.
- Expander expanded/collapsed handlers: registered at
  `Module.inl:4495-4513`.
- ToolTip/LSTooltip Opened/Closed handlers gate tooltip polling around
  `Module.inl:1152-1254`.

The existing native symbol map also resolves generic Noesis visual-child
operations:

- Symbols: `BG3Extender\GameDefinitions\Symbols.h:155-156`
- Registration: `BG3Extender\GameHooks\DataLibrariesBG3Game.cpp:219-220`
- Mapping targets: `BG3Extender\GameHooks\BinaryMappings.xml:996-1010`

Resolution alone does not prove that these hot, generic functions carry enough
semantic context to hook safely.

## Existing safe C++ to plain-Lua boundaries

- `FocusEventData` contains owned strings and value collections, not live
  Noesis references: `BG3Extender\Lua\Client\UIEvents.h:19-116`.
- `TickSnapshot` owns every event and aggregate it sends:
  `UIEvents.h:157-300`.
- Moving into the deferred queue happens before Lua delivery:
  `UIEvents.inl:464-467`.
- Lua tables are materialized only in safe client post-update processing:
  `UIEvents.inl:378-429`.
- Target HUD and element-path helpers return plain strings/tables before Lua
  retains them; the live target path explicitly depends on that contract at
  `Client\TargetSelect.lua:18-20,53-55`.

The older generic command/property subscription queues in
`UIEvents.h:303-351` can retain `Noesis::Ptr` values until post-update. They are
not evidence that a new unknown-lock native hook may safely call Lua or retain
arbitrary game objects.

## Current hang-risk surfaces

- Noesis tree/property operations run from the client game-update path. Prior
  project evidence associates visual-tree reads during concurrent Noesis
  binding/render work with lock contention and LAN hangs.
- `ClassSelectionDelegate` performs classification, parent checks, and a
  bounded selected-container search while handling a routed event
  (`Module.inl:883-1043`). Unknown Noesis lock ownership makes added logging or
  tree calls there risky.
- `GotFocusDelegate` walks parents inside the routed event
  (`Module.inl:1091-1118`).
- `SafeSubscribeINPC_SEH` mutates the ViewModel event list
  (`Module.inl:537-569`); SEH prevents a process exception from escaping but
  does not prevent deadlock or reentrancy.
- Tooltip, context-menu, focus, selection, and namescope scans can overlap
  binding propagation. The settle windows reduce timing exposure but do not
  establish a lock-order guarantee.
- Raw Noesis pointers are trusted across several frames based on a
  same-thread/stable-tree assumption. Destruction or recycling before
  consumption remains a lifetime risk.
- A low-level Visual add/remove or property-set hub may be extremely hot.
  Logging before semantic filtering could create excessive volume or reenter
  facilities used by the UI thread.
- Calling Lua, Tolk, or any Noesis tree API from an unknown hook context would
  add unacceptable lock, thread-affinity, and reentrancy risk.

## Source-level semantic operations deeper hooks might replace

These are candidate operation classes, not yet proven hook sites:

- Widget/screen manager open, close, activate, deactivate, mount, and unmount
  operations carrying a stable widget identity and semantic screen name.
  A proven hub could replace visibility polling, first-seed gathering,
  post-settle widget rediscovery, and stale Lua handler inference.
- Central controller focus or selection assignment carrying old/new element,
  selected item, owning widget, and input cause. A proven hub could replace
  most forced focus/selection tree walks and reduce pointer retention.
- Software-cursor target assignment carrying old/new
  `TopFocusedElement`, cursor owner, and hit-test result. A proven hub could
  drive Illithid Powers and similar cursor screens without repeated
  `ReadElementPath` polling.
- Shared ViewModel property publication carrying object/type, property symbol,
  and new value or dirty notification. A proven hub could replace repeated
  DataContext rediscovery and accumulate target details until stable.
- Central command execution carrying command identity, parameter, target, and
  execution result. A proven hub could report activation intent without
  intercepting controller input in Lua.
- Final text/content commit after XAML binding/conversion, carrying owner,
  property, and committed text. This is a fallback for content that is not
  semantically available before rendering and could reduce tooltip TextBlock
  walks.

The final design may require multiple hubs. A false single-hook architecture is
not assumed.

## Integration and dependency baseline

The modified BG3SE remains the mandatory host, service layer, deployment
boundary, and update authority.

- BG3Extender already embeds `BinaryMappings.xml` and loads it in
  `BG3Extender\GameHooks\DataLibraries.cpp:42-68`.
- It already exposes `WrappableFunction`/Detours support in
  `CoreLib\Wrappers.h:61-76,127-376`.
- A direct integrated trace can therefore reuse the existing loaded-module
  identity, symbol resolver, hook lifecycle, `BG3A_LOG`, and client update
  queue without introducing another proxy loader.
- The Game Release extender target links CoreLib and Detours and produces
  `BG3ScriptExtender.dll`; see
  `BG3Extender\BG3Extender.vcxproj:91-126`.
- The updater fetches the fork manifest from
  `BG3Updater\Defines.h:3-15`, updates GameMod resources independently at
  `BG3Updater\Updater.cpp:155-165,388-423`, verifies signed mod packages at
  `BG3Updater\GameModUpdater.cpp:503-508`, and swaps staging/backup/install at
  `GameModUpdater.cpp:336-370,511-544`.
- The release-manifest worktree pairs a ScriptExtender release asset with a
  BG3Access GameMod release asset. No new user installation step is required
  for direct extender integration.
- The root source license is MIT plus Commons Clause. Bundled Noesis and
  Detours have their own license files. Direct integration copies no code out
  of the licensed module and creates no new attribution surface. A companion
  that copied mapper/hook sources would require a separate dependency and
  attribution review.

## Hard uncertainties before targeted Ghidra work

Static source inspection cannot yet answer any of the following:

- Whether BG3 has a semantic screen/widget manager above generic Noesis
  visual-child operations.
- Whether there is one central controller-focus or selection setter, or
  several screen-specific paths.
- Which function assigns SoftwareCursor `TopFocusedElement` and whether its
  arguments distinguish target changes from unrelated hit-test traffic.
- Whether a shared Larian ViewModel publication hub exposes property identity
  and value safely, or only a generic high-frequency notification.
- Whether command execution has a stable centralized entry carrying the bound
  parameter and result.
- Whether final Text/Content commit occurs in a hookable function with enough
  owner/property context.
- Exact RVAs, recovered prototypes, calling conventions, register/stack
  arguments, representative callers, and signature uniqueness for all
  candidates.
- Thread affinity, lock ownership, lock order, reentrancy, object lifetime, and
  event frequency at each candidate.
- Whether a nearby string or RTTI name reflects the candidate's semantics or
  merely a neighboring code path.
- How candidate signatures drift after a game update.
- Whether a static candidate behaves as inferred at runtime. No in-game trace
  has been run.
- The checked-in fork does not contain one clearly authoritative script for
  constructing both current GitHub release assets. Existing `publish.ps1`
  still describes the upstream S3/CloudFront flow, even though runtime updater
  ownership and current release-manifest asset URLs are clear.
- The repository orientation rule names `CLAUDE.md`, but that file is absent at
  this branch point. `AGENTS.md`, `AI_SHARED_RULES.md`, relevant native source,
  live Lua, and the task-relevant Claude memory files were read instead.

No production behavior will be edited until the reproducible targeted Ghidra
workflow and static candidate evidence satisfy the V6 trace gate.
