# CLAUDE.md

This file provides guidance to Claude Code when working on **BG3Access**.

## Project Overview

BG3Access is a Baldur's Gate 3 accessibility mod that uses Tolk (screen reader bridge) to speak UI elements when navigating with a controller. It has two parts: a C++ Script Extender fork that monitors UI focus, and a Lua mod that interprets focus changes and drives speech output.

## Repositories

- **C++ (Script Extender fork)**: `D:\Repositories\bg3se-SR` -- branch `feature/tolk-bindings` -- remote `IndomitableHeart/bg3se-SR`
- **Lua (BG3 Mod)**: `D:\SteamLibrary\steamapps\common\Baldurs Gate 3\Data\Mods\BG3Access_a8cddf0c-2e61-1b7c-5c0c-275d46073949` -- branch `dev` -- remote `IndomitableHeart/BG3Access`

## Architecture

### C++ Side (bg3se-SR)

Key file: `BG3Extender\Lua\Libs\ClientUI\Module.inl`

`GlobalFocusMonitor` runs on the `IView::Rendering()` hook each frame (not on a separate thread) to avoid render-thread deadlocks. The monitor is primarily **event-driven** with tree walks as fallbacks. Class-level routed-event handlers capture user input at the source element and set dirty flags that Tick() consumes.

**Event-driven inputs (class handlers registered at startup)**:

- **`ls.UIWidget.Loaded`/`Unloaded`** -- maintains `sTrackedWidgets[]` without per-tick `GetVisualChildrenCount`/`GetVisualChild` walks. Replaces the legacy `GatherWidgets_SEH` per-tick enumeration (still kept as one-shot seed and as fallback if handler registration fails).
- **`UIElement.GotFocus`** -- captures focused element in `sGotFocusSourceElement` (per-event) and `sLastFocusedElement` (persistent across ticks, used by INPC cached-pointer lookup and by SelectionChanged gating). Replaces per-tick Strategies 1+2 (`FocusManager.FocusedElement` DP read and `IsFocused` tree walk). Those strategies still exist as fallbacks when the cached pointer is stale.
- **`Selector.SelectionChanged`** (ClassSelectionDelegate) -- one handler covers both tab carousels (ListBoxItem selections -> `sSelectionDirtyFlag`) and inline appearance carousels (non-ListBoxItem selections -> `CaptureInlineCarouselName_Inner`, gated on "focused element is a descendant of the source ListBox" via a bounded `SafeGetVisualParent_SEH` walk). Replaces per-tick Strategy 3 tree walks and per-tick inline carousel BFS.
- **`ToolTip.Opened`/`Closed`** -- presence signals only. The content tree's bindings haven't propagated when `Opened` fires, so we don't walk there. Instead, the flag `sToolTipIsOpen` gates `PollTooltip`: per-tick BFS over popup roots happens ONLY while a tooltip is visible. When no tooltip is open, zero tooltip walks.

**Tick() does NOT walk the tree per frame**. On a normal tick with stable focus, the cost is: `ReadTrackedWidgets_SEH` (visibility DP read per widget, no recursion) + snapshot finalization from event-driven state.

**Focus strategies as fallbacks** (only when cached pointer stale):
- **Strategy 1**: `FocusManager.FocusedElement` attached property walk
- **Strategy 2**: `IsFocused`/`ls:MoveFocus.IsFocused` tree walk (character sheet inventory)
- **Strategy 3**: `IsSelected` tree walk via `FindSelectedTab_SEH` (used when class-level SelectionChanged didn't fire)

Selection changes take priority over focus changes. DataContext pointer comparison detects carousel recycling (same element, swapped ViewModel). When `forced=true` but both focused and selected are null (UI rebuilding), `forceNext_` re-arms to keep retrying.

**Overlay/Dialog detection**: Strategies 1 and 2 walk children in REVERSE order so overlay/dialog widgets (rendered last = on top) are checked before the underlying menu widget. Without this, the main widget's stale FocusedElement would be returned, making dialogs silent.

**Dialog XAML architecture**: Confirmation dialogs use `LSMessageBoxData` (Title, Text, Actions with LSGameCommandData buttons). They are separate `ls:UIWidget` instances, children of the application Canvas. Buttons use `BoundEvent` (UIAccept, UICancel, UIMessageBoxA/B). Template: `MessageBoxTemplate` in `MessageBoxTemplates.xaml`.

**SEH protection is mandatory** for any code that touches Noesis from non-render-thread callbacks. Pattern: three-layer inner/invoke/wrapper, matching `ReadTextBlockText`:
- `_Inner` does the work; may use `std::string`, `std::vector`, `FixedString` (all with destructors)
- `_Invoke` has NO destructors; forwards args through pointers; lives inside `__try` callers
- Wrapper uses `__try`/`__except` around `_Invoke`, logs on fault

MSVC rejects `__try` in any function with locally-scoped C++ objects (C2712). The three-layer pattern is the only way to get both SEH and destructors.

**TickSnapshot** (sent to Lua once per tick, single callback):
- `focusedElement` (FocusEventData) -- currently focused element data
- `selectedElement` (FocusEventData) -- Strategy 3 result when different from focused
- `widgetEvents` (vector<FocusEventData>) -- one entry per widget that was added/changed this tick. Lua iterates; no last-wins overwrite.
- `widgetDCTypes` (vector<string>) -- all visible widget DC types (cached scan, refreshed when widget set changes). Distinct from `widgetEvents`: includes all visible widgets, not just new ones.
- `removedWidgetData` (FocusEventData) -- populated when a previously-visible widget went invisible
- `tooltipTexts` (vector<string>) -- TextBlocks collected from popup roots when `PollTooltip` runs
- `inlineCarouselValue` (string) -- last captured carousel value
- Change flags: `focusChanged`, `selectionChanged`, `valueChanged`, `inlineCarouselChanged`, `widgetAdded`, `radialSlotChanged`, `contextMenuChanged`, `widgetRemoved`, `tooltipChanged`

C++ exposes to Lua via `Ext.UI`:
- `SubscribeGlobalFocusChanged(cb)` -- singleton, returns bool
- `ForceGlobalFocusUpdate()` -- re-arms monitor for next tick
- `SubscribePropertyChanged(target, cb)` / `UnsubscribePropertyChanged()` -- INPC, singleton
- `HasProperty(object, name)` -- checks TypeProperty or DependencyProperty existence
- `GetRoot()` -- returns APPLICATION root (Canvas), NOT widget root (see below)
- `GetFocusedElement()` -- uses all strategies
- `GetTopmostWidget()` -- topmost visible UIWidget
- `IsMoveFocusFocusable(element)` -- checks Larian's controller focus marker
- `SubscribeDPChanged(element, cb)` / `UnsubscribeDPChanged()` -- DP change subscription (persistent elements only)
- `FindNameInWidget(name)` -- finds a named element across visible widgets by entering their NameScopes from C++. Bypasses the NameScope boundary that blocks Lua `Find()`. Returns FrameworkElement valid for current tick, or nil.
- `FindNameInWidgetScoped(name, widget)` -- like FindNameInWidget but searches only the given widget Visual. Same NameScope-crossing logic, no stale results from old widgets.
- `SetDialoguePollActive(bool)` -- gate for dialogue poll work (Cutscene.lua calls true on dialogue start, false on end)
- `SuppressGlobalFocusTick(bool)` -- fully suppresses Tick() during loading to prevent render-thread deadlocks
- `ReadHUDInfo()` -- on-demand HUD text reader (character name, portrait info)
- `ReadFocusedTextBlocks()` -- reads TextBlock texts from the currently focused element's subtree

### Lua Side (BG3 Mod)

Files live under `ScriptExtender/lua/`:

**`BootstrapServer.lua`** -- server-side bootstrap. Runs on the server (Ext.Osiris available). Two responsibilities:
- **Entity classification lookups** (`BG3Access_ClassifyRequest`/`Response` net channels): client sends an array of entity UUIDs, server reads template + stats fields the client can't see (InventoryOwner component, CanBeLooted, template InventoryType/BookType/Stats, stats ObjectCategory/InventoryTab/ItemUseType), responds with classification signals.
- **Combat event relay** (`BG3Access_Combat` net channel): Osiris listeners for TurnStarted, CombatStarted, CombatEnded, CombatRoundStarted, Died, StatusApplied, StatusRemoved, AttackedBy. Resolves GUIDs to translated names. Filters status + damage to party-members-only. Relays as JSON.

**`Client/_Init.lua`** -- module load order.

**`Client/EventRouter.lua`** -- thin dispatcher. Subscribes to `Ext.UI.SubscribeGlobalFocusChanged` and receives `TickSnapshot` tables. Iterates `snapshot.widgetEvents` once per tick and dispatches each event to the handler that cares about it (Cutscene / CC / WorldUI / Menus / overlay). Each handler receives its own event data -- no shared "best" event. Also handles: loading tips, PinnedTooltips_c detection, widget removal tracking, early-menu dispatch for shortcuts that lack a focused element, game-state transitions (suppressSnapshots, routeToWorld flip), debug explore mode toggle (L3+R3). Exports `IsUIActive()` for other modules.

**`Client/Menus.lua`** -- per-menu handler factory for pre-game menus. `CreateMenuHandler(config)` builds handlers with isolated state and a generic pipeline (classification: screen entry / item nav / value-only / carousel-only; dedup; hint-once-per-visit; SpeechData assembly). Instances: OptionsHandler, MultiplayerHandler, SaveLoadHandler, PauseMenuHandler, ShortcutsMenuHandler (widget-name routing, shares gui::DCGameMenu with PauseMenu), DifficultyHandler, ModManagerHandler, MainMenuHandler (default). `HandleWidgetAdded(widgetData)` stashes the event on `handlerState.pendingWidgetEvent` for `HandleSnapshot` to consume. Routing tables: `DC_TYPE_HANDLERS` (by DC type), `WIDGET_NAME_HANDLERS` (overrides, e.g. shortcutsMenu).

**`Client/WorldUI.lua`** -- per-panel handler factory for in-game panels. Same factory pattern as Menus. Instances: CharacterPanelHandler, TradeHandler, ContainerHandler, ExamineHandler, ActiveRollHandler, ReactionHandler, AlchemyHandler, CombineHandler, DonateHandler, PickpocketHandler, LearnSpellsHandler, SpellBookHandler, CampHandler, JournalQuestsHandler, JournalDialoguesHandler, TadpoleHandler, SelectionFlyOutHandler, RewardHandler, SavePopupHandler, HonourHandler, ConnectivityHandler, BookReaderHandler. Also owns: radial menu (RT shortcuts, RB action radial), inspect panel (PinnedTooltips_c right-stick reader), tooltip dispatch, HUD reader, detail view (RS Left virtual property list), RS HUD info cycling.

**`Client/CharCreation.lua`** -- CC-specific monolithic handler (still god-object DC; factory refactor pending). Owns all CC-specific logic: carousel navigation (Origin / Race / Subrace / Class / Subclass / Background / Deity / Feat / Abilities / Skills / Cantrip / Spell / Appearance), god-object property extraction, StaticData API-first description resolution, tab hints, first-entry announcements, guardian-appearance flow, level-up entry, post-naming detection, transition lockouts. Exports: `IsCCSnapshot`, `HandleCCSnapshot`, `HandleWidgetAdded`, `IsInCC`, `ResetCCState`, `ResetCCNavigation`.

**`Client/Cutscene.lua`** -- dialog + overheads + audio description. `HandleDialogWidgetEvent` for DCOverheads/DCDialogue widget events. `HandleDialogAnswerSnapshot`/`HandleDialogAnswerFocus` for answer-choice navigation. `SetDialoguePollActive(true/false)` gates the C++ dialogue poll during active dialogues. AD track playback on new-game flow.

**`Client/CharSheet.lua`** -- extracted from WorldUI for size. Character panel detail view, inventory, equipment, stats cards, ability tooltips. Uses the server classification channel to resolve entity/template/stats questions the client can't answer.

**`Client/SpellBook.lua`** -- SpellBook / JournalQuests / PartyLine handlers. Spell classification, action discovery.

**`Client/WorldNav.lua`** -- "GPS" clock-face navigation via camera position + pathfinding cues. Stick input driven.

**`Client/Combat.lua`** -- receives combat events from server via `BG3Access_Combat` net channel. Dispatches: CombatStarted, CombatEnded, TurnStarted (with prepended round), Died (party -> "X is down", other -> "X died"), StatusApplied/Removed (party only), AttackedBy (damage dealt/missed). Exports: `IsInCombat`, `GetCurrentTurnName`, `GetCurrentRound`, `ReadTurnOrder`, `SpeakTurnOrder`. `ReadTurnOrder` reads `TurnOrder` component on combat entities via `Ext.Entity.GetAllEntitiesWithComponent`.

**`Client/Helpers.lua`** -- shared helpers. SpeechData builder (`CreateSpeechData` with `Add(field, text, tier)` where tier is "brief"/"normal"/"verbose"; `Speak(handlerState, isScreenEntry, nil, userInitiated)` handles dedup). Text extraction (`ExtractTextFromData`, `ExtractTabName`, `FindCarouselAncestor`, `HasNavigableContent`, `GatherTextBlockTexts`, `GatherLogicalTextBlockTexts`). DC property formatters (`FormatDCText`, `FormatDCTextSplit`, `FormatDCValue`, `ExtractStatusText`, `ExtractFromWidgetData`, `ExtractFromNamedTexts`). StaticData lookups (`LookupStaticDataDescription`). Stat description parsing (`ParseDescriptionParam`, `ResolveDescriptionParams`, `ReadStatDescription`). Markup stripping and normalization.

**`Client/Logger.lua`** -- logging helpers (`Log.Info`, `Log.Debug`, `Log.Warn`, `Log.Error`).

**`Shared/_Init.lua`** / **`Shared/ClassInit.lua`** -- shared module setup (runs on both client and server).

## Critical XAML Architecture: Carousel and Content Are Always Siblings

**This is the most important architectural fact for tab-based menus.**

In every BG3 tab-based menu, the tab carousel and the content area are **siblings** under a shared parent -- NOT parent-child. Walking UP from the carousel will NEVER reach the content.

**Options menu** (OptionTemplates_c.xaml -> PreviewOptionsTemplate):
```
Grid
|-- Control template="OptionTopButtons"   <- carousel (HeaderCarouselList) is INSIDE this
|-- Viewbox                               <- content (ItemsControl "Options") is HERE
|-- Control template="OptionBottomButtons"
```

**Multiplayer browser** (LobbyBrowser_c.xaml):
```
Grid
|-- Grid Row="0"                          <- carousel (HeaderCarouselList) is HERE
|-- Grid Row="1" name="MiddleSection"     <- content is HERE
```

**Difficulty selector** (NewGameSettings_c.xaml):
```
GridRoot (3-row Grid)
|-- Row 0: Title + Navigation Controls
|-- Row 1: carouselClipper -> PresetList (ListBox, NOT HeaderCarouselList)
|-- Row 2: Buttons (Confirm, Cancel, etc.)
```
No separate content area. Title + Description are INSIDE each carousel card (RulesetDataTemplate). DataContext type is `gui::VMPreset` with `Title.Str` and `Description.Str` (NOT `Text`).

**Fix pattern**: Use `FindCarouselAncestor()` (walks up visual tree from selected ListBoxItem to find any ListBox ancestor). Then walk up to shared parent, gather text from sibling branches excluding the carousel branch. Generic -- works for HeaderCarouselList, PresetList, or any ListBox-based carousel.

**Tab content detection**: Instead of checking for `DataContext.Text` (fails for VMPreset), use `HasNavigableContent()` to check if the content area has an ItemsControl/ListView/ListBox with items. If yes (Options, Multiplayer), wait for user navigation. If no (Cross-Play text, Difficulty presets), read body text from the tab element itself or sibling content area.

## Critical XAML Architecture: Inline Appearance Carousels

CC's appearance row carousels (Face, Skin Colour, Eye Colour, Tattoo Style/Colour, Genitals, Scarring) use `AppearanceCarousel` template (`CCLib_c.xaml`, `ControlTemplate TargetType="ListBox"`). The visual tree is:

```
ListBox (the carousel -- SelectionChanged source)
|-- ContentControl x:Name="base" (ls:MoveFocus.Focusable=true -- THIS is what gets focus)
    |-- Grid
        |-- leftBtn / rightBtn (LSRepeatButton with BoundEvent="UILeft"/"UIRight")
        |-- title TextBlock
        |-- selectionName TextBlock (bound to ListBox.SelectedItem.Name)
```

Important: the focused element is **INSIDE** the source ListBox, not a sibling, not in a different widget. D-pad left/right fires `Click` on leftBtn/rightBtn which triggers `ls:SelectNextListBoxItem` on the ListBox (`TemplatedParent`), which fires `SelectionChanged` on the ListBox.

C++ gates inline-carousel capture in ClassSelectionDelegate by walking UP from `sLastFocusedElement` looking for the source ListBox. If found within 10 hops, the focused ContentControl is inside this ListBox -> legitimate carousel change -> capture via `ls:SelectedItem.Name/ColorName/Title`. If not found, ignore (catches bleed from unrelated SelectionChanged events during widget init).

## BG3SE UI Lua Bridge -- Critical API Knowledge

Full reference: `BG3SE_UI_API_REFERENCE.md` in repo root. Key facts below.

### GetProperty Returns nil for Data-Bound Values

`GetProperty("Text")` returns **nil** for any property set via a `{Binding}` expression. The C++ bridge checks `isExpression=true` in the StoredValue and returns nil instead of evaluating the binding. This affects most Larian UI text.

### Three-Step Text Extraction (proven working)

For TextBlocks, try in order:
1. **GetProperty("Text")** -- works for local/non-bound values only
2. **Inlines collection** -- formatter-populated text lives as Run objects. Access via `#inlines` for length, `inlines[i]` for items (1-based). Run.Text is a local value that GetProperty can read. Insert spaces for LineBreak elements.
3. **ToString()** -- evaluates simple bindings, returns rendered text. Filter out results matching the type name or containing "[ForceUpdate]".

### Widget Root vs Application Root

- `Ext.UI.GetRoot()` returns the **application** root Canvas -- ABOVE the widget NameScope boundary
- `Find(name)` from application root returns nil for widget-level names
- **Widget root**: walk up the `Parent` chain until Parent is nil (~5 hops from any focused element). Typically a Grid named 'Root'.
- `Find(name)` from widget root works for all authored `x:Name` elements in that widget

### Visual Tree vs Logical Tree

- **Visual tree**: `VisualParent`, `VisualChildrenCount`, `VisualChild(i)` -- full rendered hierarchy including template internals (hundreds of nodes: Borders, Images, Rectangles)
- **Logical tree**: `Parent`, `ChildrenCount`, `Child(i)` -- authored structure only, dramatically cleaner
- **Page-level TextBlocks appear in BOTH trees**. Only template-internal TextBlocks (inside button/control templates) are visual-tree-only.
- `GatherTextBlockTexts` uses visual tree (needed for template-internal text like button labels)
- `GatherLogicalTextBlockTexts` uses logical tree (~80% fewer nodes, used for fallback tab content reading)
- Do NOT recurse into TextBlock's logical children -- those are Inline objects (Run, LineBreak) which the three-step extraction already handles
- **ItemsControl logical children are ViewModels** (ls.VMTickBoxSetting, etc.), NOT ContentPresenters. ContentPresenters are visual-tree-only (generated under ScrollViewer -> ItemsPresenter -> StackPanel).
- For container navigation (finding shared parents, content areas), logical tree or `Find()` is preferred
- Guard against non-element types (e.g. `ls.VMInputEvent`, `Boxed<String>`) when walking logical tree -- pcall all property access

### Collections

BG3SE collections use **array-style** access: `#col` for length, `col[i]` for items (1-based). NOT `.Count` or `:Get()`.

### IsVisible vs Visibility

- `IsVisible` returns a **boolean** (computed, accounts for ancestor Collapsed/Hidden state) -- use this
- `Visibility` returns nil through the bridge (enum, not readable)

## XAML Source Files

Extracted game XAML at `D:\extracted packs`:
- **Pages**: `D:\extracted packs\Mods\MainUI\GUI\Pages` (201 files; `_c` suffix = controller version)
- **Templates**: `D:\extracted packs\Public\Game\GUI\Library` (OptionTemplates_c.xaml, NewOptionTemplates_c.xaml, CCLib_c.xaml, etc.)
- **State machines**: `D:\extracted packs\Mods\MainUI\GUI\StateMachines`

**Always consult the XAML BEFORE proposing fixes for menu behavior.** Do not iterate on guesses when the source of truth is available. Reading 30 lines of XAML takes less time than one wrong build cycle. The visual tree relationship, bound events, selection mechanisms, and template structure are all there.

## What's Working

Pre-game:
- Main menu button navigation (d-pad speaks each button)
- Options menu (tab names, option names + values, INPC value changes, preview panel text, controller bindings interactive mode)
- Multiplayer menu (lobby entries, tab names)
- Cross-Play tab (body text via Inlines + ToString for title)
- LAN tab ("Finding lobbies..." status text)
- Difficulty selection (preset names + descriptions)
- Navigation hints (once per menu visit)
- Carousel recycling detection (RB/LB tab switching)
- Overlay/dialog detection (MessageBox widgets)
- Save/Load menus with expander cycling
- Mod manager

In-game (confirmed by user testing):
- Pause menu, shortcuts menu (RT), action radial (RB)
- Character sheet (stats, abilities, equipment slots, inventory)
- Loot containers (ContainerHandler on chests, corpses, barrels via A)
- Context menu (X-button popup with per-item actions)
- Examine panel (resistances, stats, tooltips)
- Spellbook (as of last test)
- Book reader (BookFullText extraction with page split)
- HUD info reader (RS cycles character/target/resources)
- Detail view (RS Left virtual property list)
- Dialogue choices (full answer text via Cutscene handler)
- Journal quests tab
- Character creation (Origin, Race, Subrace, Class, Subclass, Background, Deity, Feat, Abilities, Skills, Cantrip, Spell, Appearance with inline carousels)
- Level up flow

Combat (unblocked but needs testing):
- Turn announcements with round prepending
- Combat start/end
- Death / downed
- Status applied / removed (party only)
- Damage dealt (party involved)
- Turn order on-demand read

Event-driven performance wins:
- Zero per-tick tree walks during normal navigation (focus stable)
- Tooltip BFS only while a tooltip is visible
- Inline carousel reads via SelectionChanged capture (one BFS per focus arrival, not per tick)
- INPC cached focused pointer (no walk per INPC)

## Variable Naming

- **No abbreviations** -- use descriptive names for ALL variables, parameters, and loop iterators. `dataContext` not `dc`. `frameworkElem` not `fe`. `storedVal` not `sv`. `depProp` not `dp`. `bindingInfo` not `bi`.
- **Applies everywhere** -- function parameters, local variables, loop variables, struct fields. No exceptions for "it's just a short function" or "surrounding code does it."
- **Do not copy bad patterns** -- if existing code uses abbreviations, new code still uses full names. Fix old abbreviations when touching those lines.

## Important Rules

- **Consult the XAML FIRST before debugging UI behavior.** Don't iterate on guesses when the source of truth is available in `D:\extracted packs`. The visual tree relationship, bound events, and template structure are there. Cheaper than build cycles.
- **Event-driven over polling.** Per-tick walks are the main crash/hang surface (stale pointers, render-thread contention, binding instability). Prefer class handlers + persistent caches + Probe-before-use. When adding a feature that seems to require per-tick work, look for a Noesis routed event or DP-changed callback first.
- **SEH protection is mandatory** for any new C++ that touches Noesis from non-render-thread contexts. Use the inner/invoke/wrapper pattern. Never put `__try` in a function that has C++ objects with destructors (MSVC C2712). Always validate pointers with `ProbeUIElement` before dereferencing cached Noesis pointers.
- **API first for all data** -- Use BG3SE APIs (`Ext.StaticData.Get`, `Ext.Stats.GetCachedSpell/GetCachedPassive`, `Ext.Entity` components) as the PRIMARY source for names, descriptions, and values. The focus monitor tells us WHAT the user is on (DC type, properties, GUIDs). The API tells us WHAT TO SAY. Noesis element text extraction (dcProps, elemText, sub-tables) is LAST RESORT for data that genuinely has no API. Available StaticData types: Race, ClassDescription, Background, God, Origin, Feat, FeatDescription, ProgressionDescription, Progression. Entity components: CCCharacterDefinition (abilities, race/class/background GUIDs, LevelUpData), CCState (HasDummy), TurnOrder (combat entities: Participants/Groups).
- **Server channels for data the client can't see** -- `BG3Access_ClassifyRequest`/`Response` for entity/template/stats classification (containers, loot, item types). `BG3Access_Combat` for Osiris-sourced combat events. Client sends JSON over `Ext.ClientNet.PostMessageToServer`, server broadcasts response back.
- **No visual tree walking for text extraction on focused elements** -- NEVER use recursive child walks to get text from focused elements. Use ONLY: dcProps (ViewModel properties), direct property reads (Content/Text via ReadPropertyAsString), or element x:Name cleanup. If none produce text, stay silent. `GatherVisibleTextBlocks` IS used for tooltip content (event-driven gated) and widget NameScope population, but NOT for the focused-element hot path.
- **Noesis element references expire after the tick they're obtained** -- NEVER store across ticks without a `ProbeUIElement` guard on each use. The `sLastFocusedElement` pattern (cache + validate) is the template.
- **`[ForceUpdate]` strings are unresolved Noesis binding placeholders** -- always filter them out
- **`s_HandleUnknown` strings are unresolved LocaString handles** -- filter them out, do not return raw handle strings
- **GetProperty returns nil for bound values** -- use three-step extraction (GetProperty, Inlines, ToString) for TextBlocks. For other elements, use ReadPropertyAsString which tries TypeProperty > DepProperty > mValues scan.
- **Button Content bindings are NOT in mValues** -- the Noesis Indie SDK doesn't expose the internal value provider chain. Bound Content only exists as rendered text in the control template. Use CleanElementName for buttons instead.
- **Use Ext.UI.FindNameInWidget(name) for cross-tick named lookups** -- bypasses NameScope boundary from C++. Lua Find() only works within the same tick from an element inside the NameScope.
- **Options content area uses `ItemsControl` with `ContentPresenter` children** (NOT ListBoxItems) -- `FindSelectedTab_SEH` only finds carousel items, not content items
- **Widget events arrive as an array** -- `snapshot.widgetEvents` has one entry per new/changed widget this tick. Each consumer (CC, Menus, WorldUI) iterates the array to find events it cares about. No single "widgetData" -- don't assume last-wins.
- **Read too much rather than too little** -- user preference
- **Keep `Co-Authored-By` out of commits**
- **Never commit `.wav` files in the Lua repo.** Audio description placeholders are work-in-progress content (user writes scripts + generates with Eleven Labs). They are intentionally not committed. If you see them staged or untracked, do NOT add them to a commit.
- The mod folder UUID suffix is the mod's UUID from `meta.lsx`, not per-machine
- **Do not guess at APIs** -- read the C++ source or consult BG3SE_UI_API_REFERENCE.md
- **Do not run build commands** -- the user builds C++ via Visual Studio 2022, Debug configuration. Make code changes and tell the user when to build.

## Key Collaborator: Gem

For complex architectural decisions or problems unsolved after two iterations, consult Gem via PAL MCP. Gem is a Google Gemini instance used as a second-opinion advisor.

## Known Issues / TODO

- **CC per-page factory refactor pending** -- CharCreation.lua is still monolithic (~2200 lines). Factory pattern (like Menus.lua / WorldUI.lua) would isolate per-page logic, simplify debugging, and enable per-page speech customization. Scheduled for a dedicated session.
- **CC body-type tab classification bug** -- first CC entry and body-type cycling derive tab name from the gender selector's `tabName` ("Male"/"Female"), producing "You are on the Female page" and re-announcing the tab on every cycle. Fix lives with the factory refactor (recognize body-type selectors aren't page tabs).
- **CC intro split feature** -- plan: first entry speaks welcome + nav hint + "Press <button> to continue"; user presses; then speaks Custom origin backstory; then normal navigation. Not yet implemented. To be added as part of the CC factory router.
- **Combat advantages indicator** -- `CursorText_c` widget displays hit chance, distance, advantages/disadvantages list, capability errors, attack-of-opportunity warnings, concentration warnings. Not yet wired into speech. Widget name is `CursorText_c`, DC is `gui::DCMainPanels` (or `CurrentPlayer.UIData`), key lists are `HitChanceDesc.Advantages`/`HitChanceDesc.Disadvantages` collections. Implementation started via Combat.lua + server relay for turn/status/damage events but this particular UI surface is not hooked yet.
- **Dialog button navigation** -- after dialog auto-reads, d-padding between Yes/No should speak each button individually (needs testing with current event-driven focus).
- **Crafting menus (alchemy, combining) -- stub handlers only, not functional.** `AlchemyHandler` / `CombineHandler` exist in WorldUI.lua but are minimal (just name + optional hint, no `customItemFn` or `onWidgetAdded`). They fall through entirely to the generic `CreatePanelHandler` pipeline, which may or may not produce useful speech for recipe/ingredient navigation. Needs in-game testing to identify what the generic pipeline misses, then proper handler implementation (recipe name, ingredient list, result item, etc.).
- **Camp menu -- stub handler only, not functional.** `CampHandler` exists for `gui::DCMakeCamp` but is a minimal stub. Needs testing to characterize what data is on screen (camp supplies, rest options, camp member list) and a proper handler implementation.
- **Journal dialogue history tab not working.** Quests tab reads correctly via `JournalQuestsHandler`. `JournalDialoguesHandler` is registered for `gui::DCJournalDialogues`/`ls.DCJournalDialogues` but dialogue history entries weren't reading on last test. Needs investigation -- likely a classification issue (entries may not be producing extractable text through the generic pipeline) or a missing customItemFn.
- **ActiveRoll dice rolls -- partial.** Roll prompts and bonuses mostly read, but the actual rolled number isn't being extracted reliably. The `FinalResult` DP post-processor needs more work. Additional testing required.
- **World tooltips NOT implemented.** The `WorldTooltips` widget (hover labels over in-world objects -- chests, NPCs, items) is detected at the widget level but has no handler. Would need a dedicated reader for `CurrentPlayer.WorldTooltips` items (Name, Description, ExtraInfo).
- **Handlers registered but not yet tested:** TradeHandler, ReactionHandler, DonateHandler, TadpoleHandler, HonourHandler. May work via generic pipeline, may need customItemFn. Each needs in-game verification.
- **Untested (no user validation yet):** pickpocket, learn spells, selection flyout, reward selection, save popup, overhead subtitles. Add to user's test list on next session.
- **Audio description content pending.** Framework is wired and confirmed playing (tested with opening-cinematic placeholder). Actual AD tracks require scripts written + Eleven Labs TTS generation + timing sync -- content creation work, not code work. The placeholder .wav file in the Lua repo must NOT be committed (see .gitignore / commit rules).

*Last Updated: 2026-04-15*
*Version: 2.0*
