# BG3 Native UI Interception V6: Static Discovery Report

## Result

Static analysis found several useful semantic interception points, but not one
universal UI hub.

The strongest first trace is:

- Candidate: `D-NOESIS-ROUTEDCOMMAND-EXECUTE`
- Program: `bg3_dx11.exe`
- RVA: `0x464CAB0`
- Recovered role:
  `Noesis::RoutedCommand::Execute(BaseComponent*, UIElement*) const`
- Confidence: high
- Masked signature matches in the exact binary: one
- Entry payload: command pointer, parameter pointer, target element pointer
- Expected rate: low and action-driven

This satisfies the V6 trace-only prototype gate. It does not establish runtime
semantics, broad coverage, or production acceptance. The maximum current claim
is Level 1.

Direct integration in an isolated modified-BG3SE directory is the preferred
trace location. It reuses the loaded-module lifecycle, Microsoft Detours,
existing logging, and automatic updater boundary without a second loader or a
new user installation step.

Vulkan was not searched, opened, analyzed, or validated.

## Evidence boundary

- Exact DX11 filesystem size: `104363072` bytes
- Exact DX11 SHA-256:
  `e899c67cb90b9c6b0f052e3f758ba8615bc2012e52561c2af2e6a1e06ef61a2f`
- Product version: `4.1.1.7398727`
- Ghidra: 12.1.2 PUBLIC
- Project/program: `bg3_accessibility` / `bg3_dx11.exe`
- Initial full analysis: precomputed before V6
- V6 full Auto Analysis rerun: no
- V6 access: read-only, `-noanalysis`, bounded headless scripts
- Runtime trace: not performed
- Evidence level: Level 1, trace candidate found

The authoritative size above is the filesystem byte count. Ghidra's import
summary `# of Bytes` value `106644048` was not used as file size.

## Integration-location decision

### Preferred: integrated modified BG3SE trace

Use an isolated directory under:

`BG3Extender\Extender\Client\AccessibilityTrace\`

Reasons:

- The user's modified BG3SE is already the required native host and updater.
- `CoreLib\Wrappers.h` supplies the repository's Detours-backed wrapped-function
  machinery.
- `BG3Extender\GameHooks\DataLibraries.cpp` and
  `BG3Extender\GameHooks\BinaryMappings.xml` demonstrate existing symbol and
  module-resolution ownership.
- Client startup/shutdown already provides a bounded hook lifetime.
- `BG3A_LOG` is the existing accessibility diagnostic channel.
- The installed artifact remains `BG3ScriptExtender.dll`; no second proxy or
  manual install is introduced.

True minimal dependency closure for the trace:

- existing BG3Extender client startup/shutdown;
- loaded `bg3_dx11.exe` module base;
- one isolated masked scanner over executable PE sections;
- one `WrappedFunction` or equivalent repository Detours attachment;
- exact binary identity verification;
- Win32 thread ID and the existing `BG3A_LOG` macro.

The trace does not need Lua, Tolk, Noesis tree APIs, ECS, networking, a queue,
another logger, a new loader, or copied mapper projects.

### Permitted later: BG3SE-loaded companion

An SE-loaded companion could isolate future hook churn, but the current fork
does not already expose a companion load/ownership contract. Adding one for a
single trace would enlarge the service, updater, signing, packaging, and
license surface. It is not the minimal first proof.

If a future multi-hook subsystem becomes independently versioned, the companion
would still have to be loaded, version-gated, signed, delivered, and removed by
this modified BG3SE. It must not become a competing proxy loader.

### Diagnostic only: standalone DLL

A standalone trace DLL would duplicate injection and lifecycle concerns and
would not satisfy the fixed production boundary. It is unnecessary here.

## Candidate dossier A: widget and screen lifecycle

### A-UIWIDGET-CLOSE-COMMIT

- RVA: `0x2110840`
- Ghidra: `FUN_142110840`
- Recovered role: inferred `ls.UIWidget` close cleanup and `WidgetClosing`
  routed-event raise
- Confidence: high
- Calling convention: Microsoft x64; `RCX` is the UIWidget pointer
- Direct caller: manager-like owner at `0x20CB780`
- Important callees: routed-event dispatch, reference cleanup, focus/context
  dependency-property teardown
- RTTI/reflection evidence: `ls.UIWidget` registration body at `0x210E000`;
  `WidgetClosing` storage at `0x5F9F150`

Bounded semantic pseudocode:

```text
close_widget(widget):
  raise WidgetClosing on widget
  enter widget teardown region
  clear focus and context-menu references
  release retained objects
```

- Payload: concrete widget pointer and close phase; no screen name or
  DataContext at entry
- Expected thread: client UI thread, statically inferred but unverified
- Lock risk: own critical section follows the event; an outer lock is unknown
- Frequency/noise: low, once per concrete UIWidget close path
- Coverage: Larian UIWidget closes; not generic opens and not proof of every
  screen close
- Hook safety: traceable in principle, but lower proof value than the command
  candidate and not selected first
- Relocation: conventional prolog; no detour is approved until its exact trace
  need and instruction span are independently reviewed
- Signature:
  `48 89 5C 24 08 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 50 48 8B F1 E8 ?? ?? ?? ?? 48 8B F8`
- Exact-binary uniqueness: one
- Update failure: signature missing/ambiguous; fail closed and rediscover
- Potential later reduction: close inference, stale panel-handler cleanup, and
  some visibility polling
- Still required: open discovery, screen identity, DataContext/content reads,
  and controls not represented by UIWidget
- Recommendation: blocked pending runtime trace and paired open evidence

### A-NOESIS-UNLOADED-TRANSITION

- RVA: `0x45AFAE0`
- Ghidra: `FUN_1445AFAE0`
- Recovered role: inherited Noesis FrameworkElement unload transition
- Confidence: high
- Calling convention: Microsoft x64; `RCX` is FrameworkElement
- Caller shape: 172 caller rows across inherited controls
- Important callees: Unloaded routed-event dispatch and inherited-context
  clearing
- Vtable/property evidence: Unloaded storage `0x5F7ACE0`; inherited lifecycle
  slot observed from TextBlock-related table evidence

Bounded semantic pseudocode:

```text
transition_unloaded(element):
  if inherited state requires it, clear DataContext
  raise FrameworkElement.Unloaded
  propagate inherited lifecycle cleanup
```

- Payload: element pointer and unload phase
- Expected thread: Noesis UI/layout phase; exact affinity unverified
- Lock risk: high enough to forbid tree/property inspection from a detour
- Frequency/noise: potentially high because template/filler descendants unload
- Coverage: broad FrameworkElement lifecycle, not semantic screens
- Hook safety: pointer-only trace may be technically possible, but it is noisy,
  redundant with the existing class handler, and not justified
- Relocation: conventional prolog; unused because the semantic gate fails
- Signature:
  `48 89 5C 24 08 48 89 74 24 10 57 B8 40 00 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B 15 ?? ?? ?? ?? 44 8D 40 C8 48 8B F9`
- Exact-binary uniqueness: one
- Update failure: fail closed on scan drift
- Potential later reduction: none beyond what the existing Loaded/Unloaded
  class handler already provides
- Still required: semantic screen identity and content extraction
- Recommendation: rejected as redundant and lock-sensitive

### Lifecycle conclusion

No semantic screen manager with a verified operation equivalent to
`InventoryOpened(name, DataContext)` was recovered. Searches for widget manager,
screen manager, UI manager, screen stack, and panel stack did not converge on a
runtime hub carrying screen identity. Exact XAML widget names used by BG3Access
also did not appear as useful binary anchors.

The recovered manager-like caller at `0x20CB780` can close a set of UIWidgets,
but no paired semantic open operation or stable screen-name argument was proven.
The existing class-wide Loaded/Unloaded handling remains the strongest open
mechanism for this milestone.

## Candidate dossier B: focus, selection, and software cursor

### B-LSLISTBOX-LOCAL-FOCUS-COMMIT

- RVA: `0x2150550`
- Ghidra: `FUN_142150550`
- Recovered role: `ls.LSListBox` LocalFocus commit and
  LocalFocusChanged raise
- Confidence: high
- Arguments: `RCX` listbox; `RDX` proposed focus object; old focus at the
  object's `+0x10` field before commit
- Call evidence: four caller rows; downstream property write and event raise
- Reflection evidence: LocalFocus `0x5F9F720`; LocalFocusChanged `0x5F9F728`

Bounded semantic pseudocode:

```text
set_local_focus(listbox, proposed):
  normalized = normalize(proposed)
  old = listbox.LocalFocus
  if old != normalized:
    retain normalized; commit LocalFocus; release old
    raise LocalFocusChanged(old, normalized)
```

- Expected thread: client UI input/property phase; unverified
- Lock risk: reference and property propagation occur inside the function;
  entry-only pointer capture is the safe trace boundary
- Frequency: selection/focus changes, expected low to moderate
- Coverage: Larian LSListBox only; useful for many conventional list menus
- Hook safety: promising pointer-only trace; not first because command execute
  has broader low-rate proof value
- Relocation: conventional prolog; exact span must be reviewed before use
- Signature:
  `40 53 55 48 83 EC 78 48 8B D9 48 89 BC 24 A0 00 00 00 33 C9 48 8B FA E8 ?? ?? ?? ?? 48 8B D7 48 8B C8 E8 ?? ?? ?? ??`
- Exact-binary uniqueness: one
- Update failure: fail closed and rediscover type/property storage
- Potential later reduction: radial/list polling, selected-container searches,
  some retained focus pointers and settle-triggered selection scans
- Still required: non-LSListBox controls, final label/content reads, and
  SoftwareCursor
- Recommendation: strong second trace candidate

### B-LSTREEVIEW-LOCAL-FOCUS-COMMIT

- RVA: `0x2171080`
- Ghidra: `FUN_142171080`
- Role: parallel `ls.LSTreeView` LocalFocus commit and event raise
- Confidence: high
- Arguments: `RCX` tree; `RDX` proposed focus; old focus available before
  commit
- Reflection evidence: LocalFocus `0x5F9F9F8`; LocalFocusChanged `0x5F9F9E8`

Bounded semantic pseudocode:

```text
set_tree_local_focus(tree, proposed):
  normalized = normalize(proposed)
  if tree.LocalFocus != normalized:
    replace retained focus
    publish property and LocalFocusChanged
```

- Expected thread/locks: UI property phase; runtime lock state unverified
- Frequency: selection changes, expected low to moderate
- Coverage: LSTreeView only
- Hook safety: bounded entry pointers; viable later trace
- Relocation: conventional prolog, not yet installed
- Signature:
  `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 48 8B F9 48 8D A9 58 FD FF FF 33 C9 48 8B F2 E8 ?? ?? ?? ??`
- Exact-binary uniqueness: one
- Update failure: fail closed
- Potential later reduction: tree selection searches and polling for this type
- Still required: other selector/control families and rendered label reads
- Recommendation: strong type-specific later trace

### B-NOESIS-SELECTOR-PROPERTY-CHANGE

- RVA: `0x4641B20`
- Ghidra: `FUN_144641B20`
- Recovered role: inferred `Noesis::Selector::OnPropertyChanged`
- Confidence: high
- Arguments: `RCX` selector; `RDX` DependencyPropertyChangedEventArgs
- Call evidence: branches on SelectedIndex `0x5F7BBB8` and SelectedItem
  `0x5F7BBC0`, then constructs SelectionChanged `0x5F7BBE0`

Bounded semantic pseudocode:

```text
selector_property_changed(selector, args):
  call base property handler
  if property is SelectedIndex or SelectedItem:
    reconcile index/item
    construct and route SelectionChanged
```

- Expected thread: Noesis dependency-property propagation
- Lock/reentrancy: significant; no Lua, logging-time inspection, or tree access
- Frequency: moderate, immediately filterable by property identity
- Coverage: broad Noesis Selector controls
- Hook safety: possible scalar trace, but the existing SelectionChanged class
  handler already exposes the semantic event at a safer layer
- Relocation: conventional prolog; not needed for V6
- Signature:
  `48 89 5C 24 08 48 89 74 24 10 57 B8 70 00 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0`
- Exact-binary uniqueness: one
- Update failure: fail closed
- Potential later reduction: only justified if it proves old/new values that the
  current class handler cannot safely preserve
- Still required: label/content recovery and non-Selector mechanisms
- Recommendation: blocked until incremental value over the existing handler is
  demonstrated

### B-SOFTWARE-CURSOR-TARGET-COMMIT

- RVA: `0x2174980`
- Ghidra: `FUN_142174980`
- Recovered role: `ls.SoftwareCursor` TopFocusedElement refresh/commit
- Confidence: medium
- Entry argument: `RCX` SoftwareCursor
- Dataflow: computes the new element in a local, writes TopFocusedElement
  storage `0x5F9FAD0`, and updates hover/tooltip state
- Call evidence: callers at `0x2173F60` and property callback `0x2175260`;
  cleanup at `0x2174D30`; clear path at `0x2173D60`

Bounded semantic pseudocode:

```text
refresh_software_cursor(cursor):
  candidate = perform bounded hit/focus selection
  update hover and tooltip state
  cursor.TopFocusedElement = candidate
```

- Expected thread: UI input/hit-test phase
- Lock risk: meaningful; computed target is not available at entry
- Frequency: may be per cursor refresh rather than only target change
- Coverage: SoftwareCursor, including the likely powers-menu path
- Hook safety: an entry detour cannot report the semantic target; a reviewed
  post-call/call-site detour or posthook would be needed
- Relocation: prolog is conventional, but entry relocation does not solve the
  payload problem
- Signature:
  `40 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 68 4C 8B F1 45 33 ED 44 89 6D 50 4C 39 A9 F8 02 00 00`
- Exact-binary uniqueness: one
- Update failure: fail closed; re-establish the exact commit call site
- Potential later reduction: powers `ReadElementPath` polling and some tooltip
  target polling
- Still required: safely deferred DataContext.Power read and target text
- Recommendation: blocked pending a bounded call-site or posthook design

### Focus/selection conclusion

There is no single universal Larian focus setter in the evidence. There are
strong type-specific Larian commits for LSListBox and LSTreeView, plus the
generic Noesis Selector property/event path. The existing GotFocus and
SelectionChanged class handlers remain broadly useful.

The SoftwareCursor target commitment exists, but its new target is a local,
not an entry argument. It is a valuable later candidate, not a safe first
detour.

## Candidate dossier C: ViewModel property publication

### C-LS-VIEWMODEL-INPC-TYPE-LEAD

- RVA: `0x2106190`
- Ghidra: `FUN_142106190`
- Recovered role: `ls.ViewModel` reflection initializer adding
  INotifyPropertyChanged
- Confidence: low as a runtime hook, high as type-contract evidence
- Arguments/payload: type registration only; no instance, property, or value
- Xrefs: INotify type storage `0x60071E0` has 526 registration/cast references;
  ls.ViewModel type storage `0x60D2798` has 360

Bounded semantic pseudocode:

```text
register_ls_viewmodel(type):
  resolve reflected base/interface metadata
  add INotifyPropertyChanged interface
```

- Thread/frequency: one-time reflection setup, not runtime publication
- Lock risk: irrelevant to desired events because semantics are absent
- Coverage: proves a shared interface, not a shared publishing implementation
- Hook safety: no reason to hook
- Signature:
  `40 53 48 83 EC 20 48 8B D9 33 C9 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 21`
- Exact-binary uniqueness: two, at `0x2106190` and `0x21617F0`
- Update failure: ambiguity remains a hard blocker
- Potential later reduction: none
- Still required: existing per-instance INPC subscription and safe-state reads
- Recommendation: rejected; nonunique and not a runtime hub

### ViewModel conclusion

No shared `SetPropertyAndNotify(object, property, value)` or
`RaisePropertyChanged(object, property)` implementation carrying stable runtime
arguments was recovered.

Target/combat metadata at `0x16888D0`, constructor-like code at `0x1689680`,
and owner/caller `0x1689C40` confirm properties including
PreviewDescription, AoOWarning, CanExecute, TargetCanBeHealed, and related
fields. They do not establish one shared publisher.

The safe architecture remains:

- subscribe to each relevant DataContext's INPC;
- mark property names/instances dirty in the callback;
- read values later in the known client-update safe state;
- accumulate eventually consistent combat fields until the existing stability
  contract is met.

## Candidate dossier D: command execution

### D-LS-TICKBEHAVIOR-COMMAND-DISPATCH

- RVA: `0x157E440`
- Ghidra: `FUN_14157E440`
- Role: inferred `ls.TickBehavior` command dispatch callback
- Confidence: high
- Entry argument: `RCX` TickBehavior
- Internal data: Command `0x5F9E968`, CommandParameter `0x5F9E970`, and
  CommandTarget `0x5F9E978` are read into locals
- Call path: branches between generic ICommand vtable calls and RoutedCommand
  CanExecute `0x464C9E0` / Execute `0x464CAB0`

Bounded semantic pseudocode:

```text
tick_behavior_activate(behavior):
  command = behavior.Command
  parameter = behavior.CommandParameter
  target = behavior.CommandTarget
  if command is RoutedCommand:
    if command.CanExecute(parameter, target): command.Execute(parameter, target)
  else:
    if command.CanExecute(parameter): command.Execute(parameter)
```

- Thread: UI behavior/input phase, unverified
- Lock risk: property reads and dispatch occur inside the function
- Frequency: low, activation-driven
- Coverage: TickBehavior routed and non-routed commands only
- Hook safety: entry lacks useful semantic values; a hook would have to perform
  dependency-property reads in unknown context
- Relocation: conventional prolog, but payload blocker controls
- Signature:
  `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 30 0F 29 74 24 20 4C 8B F1 33 F6 48 81 C1 20 01 00 00`
- Exact-binary uniqueness: one
- Update failure: fail closed
- Potential later reduction: activation inference for TickBehavior templates
- Still required: direct-button and other ICommand routes
- Recommendation: blocked because entry payload is insufficient

### D-NOESIS-ROUTEDCOMMAND-EXECUTE

- RVA: `0x464CAB0`
- Absolute address in this image only: `0x14464CAB0`
- Ghidra: `FUN_14464CAB0`
- Recovered role:
  `Noesis::RoutedCommand::Execute(BaseComponent*, UIElement*) const`
- Confidence: high
- Calling convention: Microsoft x64
- `RCX`: RoutedCommand object
- `RDX`: command parameter BaseComponent
- `R8`: target UIElement
- Representative callers: TickBehavior `0x157E440`, Larian UI path
  `0x214E8A0`, and additional Noesis control paths
- Paired callee/path: CanExecute `0x464C9E0`; constructs Executed routed-event
  arguments and routes them to the target

Bounded semantic pseudocode:

```text
routed_command_execute(command, parameter, target):
  if target is null: return
  args = ExecutedRoutedEventArgs(command, parameter)
  target.RaiseEvent(args)
```

- Thread: UI input/command phase, runtime ID unverified
- Lock/reentrancy: routed-event dispatch may reenter UI code; the trace must log
  only bounded entry scalars and must call the original exactly once
- Frequency: low, one event per routed command execution
- Coverage: RoutedCommand across several callers; excludes arbitrary ICommand
  implementations
- Hook safety: strongest V6 trace candidate because all useful values are entry
  arguments and no Noesis read is required
- Relocation: inspected entry begins with a target-null test and relative branch,
  followed by normal frame setup. The repository's Microsoft Detours machinery
  is designed to relocate such x64 control-flow instructions. Installation
  remains conditioned on exact identity and a unique scan.
- Signature:
  `4D 85 C0 0F 84 ?? ?? ?? ?? 53 B8 50 00 00 00 E8 ?? ?? ?? ?? 48 2B E0 49 8B D8 48 89 5C 24 20 48 8B 05 ?? ?? ?? ??`
- Mask: exact bytes except the conditional displacement, allocator call
  displacement, and RIP-relative storage displacement
- Exact-binary uniqueness: one
- Update failure: missing or multiple scan hits; log explicit failure and do not
  attach
- Potential later reduction: command-activation inference and some post-action
  rediscovery, once runtime semantics and accessible object ownership are proven
- Still required: accessible labels, generic ICommand paths, and Lua policy
- Recommendation: trace first, then reassess

### Command conclusion

There is a central Noesis RoutedCommand execution path carrying command and
parameter pointers at entry. It is not a universal ICommand dispatcher. It is
nevertheless the smallest safe proof because it is semantic, action-driven,
unique, and does not require calling into Noesis from the detour.

## Candidate dossier E: final text/content fallback

### E-NOESIS-BINDINGEXPRESSION-UPDATE-TARGET

- RVA: `0x465B530`
- Ghidra: `FUN_14465B530`
- Recovered role: `Noesis::BindingExpression::UpdateTarget() const`
- Confidence: high
- Vtable evidence: BindingExpression table `0x5322448`, slot 9
- Entry argument: `RCX` BindingExpression; target/property/value are internal
- Callees: invalidation/evaluation paths `0x465EAA0` and `0x465EA00`

Bounded semantic pseudocode:

```text
binding_update_target(expression):
  if target and binding are live:
    invalidate/evaluate expression
    apply resulting value to target property
```

- Thread: binding engine phase
- Lock/reentrancy: high and unknown
- Frequency: potentially very high across all bindings
- Coverage: broad bindings, but without safe entry semantics
- Hook safety: rejected; early useful filtering requires internal dereferences
- Relocation: conventional prolog; not relevant because hot-path blocker applies
- Signature:
  `40 53 B8 20 00 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B D9 48 8B 49 18 48 85 C9 74 37 48 83 7B 20 00 74 30`
- Exact-binary uniqueness: one
- Update failure: fail closed
- Potential later reduction: none without a proven safe filter/value ABI
- Still required: targeted rendered text readers
- Recommendation: rejected as hot and entry-payload insufficient

### E-NOESIS-TEXTBLOCK-PROPERTY-CHANGE

- RVA: `0x464F990`
- Ghidra: `FUN_14464F990`
- Recovered role:
  `Noesis::TextBlock::OnPropertyChanged(DependencyPropertyChangedEventArgs const&)`
- Confidence: high
- Vtable evidence: TextBlock table `0x531EBA8`, slot 7
- Arguments: `RCX` TextBlock; `RDX` changed args
- Property evidence: explicit branch on Text storage `0x5F7BE10`

Bounded semantic pseudocode:

```text
textblock_property_changed(textblock, args):
  call inherited property handler
  if args.property is Text:
    accept changed final Text value
    dirty layout/format state
```

- Thread: dependency-property/layout phase
- Lock/reentrancy: unknown; string decoding/allocation in-hook is not approved
- Frequency: potentially high, but property identity is immediately filterable
- Coverage: final TextBlock text only; excludes Run/content/control-specific
  composition that does not commit through TextBlock.Text
- Hook safety: a possible later narrow fallback after ABI and lock proof; not a
  first trace
- Relocation: conventional prolog; no installation approved
- Signature:
  `48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 B8 20 00 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 84 C0`
- Exact-binary uniqueness: one
- Update failure: fail closed and re-establish Text property storage
- Potential later reduction: bounded child-TextBlock fallback scans for proven
  widget types
- Still required: semantic owner association, tooltip structure, content and
  parameterized-string cases
- Recommendation: blocked pending filtered trace and safe value-copy proof

### Final-text conclusion

A practical narrow fallback may exist at TextBlock property change, but it is
lower-level and noisier than semantic lifecycle, focus, property, or command
events. BindingExpression UpdateTarget is too hot and carries too little entry
context. Neither should be instrumented merely to claim a general solution.

## Direct answers to the fifteen V6 questions

### 1. First trace location

Direct integration in the modified BG3SE is best. It is smaller, uses the
mandatory host, and preserves one install/update boundary. An SE-loaded
companion is permitted later only if an independently versioned hook subsystem
justifies its added loader and packaging contract.

### 2. Reused infrastructure and minimal closure

Reuse client startup/shutdown, existing Detours wrappers, module-base access,
and `BG3A_LOG`. Keep the scanner and detour isolated. Do not import Lua/Tolk,
Noesis traversal, ECS, networking, updater logic, or a second logging queue into
the hook.

`BinaryMappings.xml` is useful precedent and may be the eventual production
home for a reviewed stable mapping. For this trace, a self-contained exact
masked scan is safer because it is compile-time disabled and must fail closed
without modifying global mappings.

### 3. Semantic widget/screen manager

Not proven. A real UIWidget close commit exists, and a manager-like owner calls
it, but no paired open hub carrying stable screen identity/DataContext was
recovered. Existing Loaded/Unloaded class handling remains necessary.

### 4. Central focus or selection setter

Not one universal Larian setter. Strong commits exist for LSListBox and
LSTreeView. A broader Noesis Selector property path exists, but the current
SelectionChanged class handler already captures its semantic event.

### 5. SoftwareCursor target setter

The TopFocusedElement commit path is at `0x2174980`, and it is relevant to the
Illithid Powers menu. The new target is a local, so an entry hook cannot report
it. A bounded call-site or posthook must be proven before instrumentation.

### 6. Shared ViewModel publisher

Not found. `ls.ViewModel` shares the INotifyPropertyChanged interface, but the
recovered function is registration only and its signature is nonunique. Existing
per-instance INPC subscription plus safe-state accumulation remains correct.

### 7. Central command path

Yes for RoutedCommand: `0x464CAB0` carries command, parameter, and target at
entry. No for all ICommand implementations. TickBehavior shows both routes but
does not carry its loaded values at entry.

### 8. Final text/content fallback

TextBlock OnPropertyChanged is a plausible narrow fallback after immediate Text
property filtering and safe string-copy proof. BindingExpression UpdateTarget is
too broad and hot. Neither is production-ready.

### 9. Can a small hook set eliminate normal tree walks on DX11?

Not established. Static evidence supports a possible small layer made of
UIWidget close, LSListBox/LSTreeView local focus, SoftwareCursor commitment,
RoutedCommand execution, per-instance INPC, and a narrow TextBlock fallback.
Open lifecycle and general semantic labeling remain gaps. Level 3 coverage
cannot be claimed without runtime matrices.

### 10. Exact Module.inl work that proven hooks could simplify

Only after runtime and coverage proof:

- tracked-widget visibility polling and first-seed/legacy gathering at
  `Module.inl:2016-2058`;
- five-stable/30-ceiling lifecycle settle at `Module.inl:2082-2176`;
- post-settle namescope/template rediscovery at `Module.inl:2816-2851`;
- forced/initial/post-settle focus and selected-tab searches at
  `Module.inl:2272-2478`;
- selected-container traversal in `ClassSelectionDelegate` at
  `Module.inl:883-1043`;
- parent-chain widget recovery in `GotFocusDelegate` at
  `Module.inl:1091-1118`;
- cached-pointer probe/fallback focus walk at `Module.inl:3438-3493`;
- widget/DataContext rediscovery before INPC reads at
  `Module.inl:4022-4104`;
- radial/context LocalFocus polling at `Module.inl:3225-3274`;
- hotbar Tag-address polling at `Module.inl:3310-3415`;
- SoftwareCursor `TopFocusedElement.DataContext.Power` polling at
  `Module.inl:11139-11182`;
- child TextBlock fallback scanning at `Module.inl:10039-10057`;
- some raw pointer retention at `Module.inl:188-210,265-293,4176-4187`.

V6 changes none of these production paths.

### 11. Targeted reads that remain

- names, labels, descriptions, costs, state, disabled reasons, and tooltip
  structure;
- DataContext reads after a safely owned instance/property event;
- final localized text where command/focus pointers have no accessible label;
- nested/filler-menu special handling not represented by LSListBox/LSTreeView;
- Illithid power identity after a safe SoftwareCursor target handoff;
- context-menu content and radial-specific semantics;
- generic ICommand activation;
- combat TargetInfo/Cursor VM fields and authoritative ECS/camera identity.

### 12. Combat target details

The camera/ECS target identity is authoritative first. TargetInfo fields are
eventually consistent and can arrive across multiple property changes. A proven
property publication hook could replace expensive polling with dirty-property
accumulation, but it would not make the data atomic or immediately final.

Because no shared publisher was recovered, the current per-instance INPC dirty
accumulation and stability window remain necessary. The RoutedCommand trace does
not change combat targeting.

### 13. First trace candidate

Trace `D-NOESIS-ROUTEDCOMMAND-EXECUTE` at RVA `0x464CAB0` first.

It has all relevant values in registers at entry, a unique masked signature,
bounded action-driven frequency, several independent callers, a meaningful
semantic operation, and no requirement to call Lua, Tolk, or Noesis APIs from
the hook.

### 14. Smallest safe proof

Compile the isolated trace explicitly, opt in at runtime, verify the exact game
hash and one signature match, attach one detour, and emit pointer/scalar-only
`[BG3A-HOOK]` lines. In DX11, execute and cancel one bounded menu command. Prove:

- one startup identity line;
- one resolved/installed line;
- one `command_execute` line per expected routed command;
- stable thread ID and bounded volume;
- no extra speech, hang, crash, recursion, or behavior change;
- exact original call count/order is preserved.

Anything beyond that is a later trace phase.

### 15. Updater, build, and release files

Direct integration requires source/project inclusion in:

- `BG3Extender\BG3Extender.vcxproj`
- `BG3Extender\BG3Extender.vcxproj.filters`
- isolated client initialization/shutdown call sites in
  `BG3Extender\Extender\Client\ScriptExtenderClient.cpp`

Existing mapping/hook infrastructure inspected:

- `CoreLib\Wrappers.h`
- `BG3Extender\GameHooks\DataLibraries.cpp`
- `BG3Extender\GameHooks\BinaryMappings.xml`

Updater ownership inspected:

- `BG3Updater\Defines.h`
- `BG3Updater\Updater.cpp`
- `BG3Updater\GameModUpdater.cpp`
- `BG3Updater\Manifest.h`
- `BG3Updater\Manifest.cpp`
- `BG3Updater\EmbeddedResources\LocalManifest.json`
- `BG3Updater\EmbeddedResources\BlankManifest.json`

Release flow inspected:

- `publish.ps1`, which still describes the upstream S3/CloudFront publication
  flow and is not treated as the authoritative current fork release recipe;
- `republish-manifest.bat`;
- `D:\Repositories\bg3se-SR-manifest\Channels\Release\Manifest.json`;
- `D:\Repositories\bg3se-SR-manifest\Channels\Beta\Manifest.json`.

The release manifest already pairs a signed ScriptExtender package with a signed
BG3Access GameMod package. A future direct hook ships inside the ordinary
ScriptExtender asset, while compatible Lua remains the GameMod asset. The same
manifest revision must point at the intended pair. No updater code change or new
user step is needed for the V6 trace source itself.

An SE-loaded companion would additionally require a signed companion asset or
inclusion in the ScriptExtender package, an explicit BG3SE loader/version check,
manifest/package ownership, and atomic cleanup. Those costs are why it is not
the first trace.

## Existing behavior and service boundary

The trace decision does not authorize production migration. The following stay
unchanged:

- GlobalFocusMonitor and class handlers;
- widget tracking and settle windows;
- menu, combat, powers, context, tooltip, and navigation readers;
- WorldNav and its beacon;
- INPC subscription and dirty accumulation;
- Lua EventRouter, Dispatcher, SpeechData, Scheduler, and Tolk policy;
- native-to-Lua owned snapshot structures;
- automatic native/Lua updater delivery.

The fixed responsibility boundary remains:

```text
native C++ observes and transports owned semantic facts
  -> existing BG3Access Lua decides what, when, and how to speak
```

## Risks and blockers

- Runtime thread affinity is not yet measured.
- Noesis lock ownership and reentrancy are unknown at most candidates.
- Raw Noesis object lifetime is unsafe beyond the immediate hook unless an
  explicit ownership transfer is proven.
- Signatures will drift on game updates and must fail closed.
- Pointer identities are not accessible labels.
- RoutedCommand does not cover generic ICommand.
- SoftwareCursor needs a different interception shape to expose its computed
  target.
- ViewModel publication remains fragmented or unrecovered.
- Text/binding candidates can be hot and must not be instrumented without early
  filtering and ABI proof.
- Static analysis cannot prove runtime frequency, ordering, coverage, or lack of
  recursion.

No candidate touches anti-cheat, networking, DRM, or multiplayer state. No
candidate is represented as Vulkan or renderer-independent evidence.

## Trace gate decision

Gate: passed for one trace-only candidate.

Authorized scope:

- integrated, isolated source;
- compile-time disabled by default;
- explicit runtime opt-in;
- exact DX11 identity plus unique signature verification;
- one RoutedCommand Execute detour;
- pointer/scalar-only direct `BG3A_LOG` lines;
- no Lua, Tolk, Noesis calls, tree walks, speech, queue, or production reader
  changes;
- explicit fail-closed startup diagnostics;
- removable without touching unrelated behavior.

Production recommendation: blocked pending the user's bounded runtime trace.
