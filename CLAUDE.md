# CLAUDE.md

This file provides guidance to Claude Code when working on **BG3Access**.

## Project Overview

BG3Access is a Baldur's Gate 3 accessibility mod that uses Tolk (screen reader bridge) to speak UI elements when navigating with a controller. It has two parts: a C++ Script Extender fork that monitors UI focus, and a Lua mod that interprets focus changes and drives speech output.

## Repositories

- **C++ (Script Extender fork)**: `D:\Repositories\bg3se-SR` — branch `feature/tolk-bindings` — remote `IndomitableHeart/bg3se-SR`
- **Lua (BG3 Mod)**: `D:\SteamLibrary\steamapps\common\Baldurs Gate 3\Data\Mods\BG3Access_a8cddf0c-2e61-1b7c-5c0c-275d46073949` — branch `dev` — remote `IndomitableHeart/BG3Access`

## Architecture

### C++ Side (bg3se-SR)

Key file: `BG3Extender\Lua\Libs\ClientUI\Module.inl`

`GlobalFocusMonitor` runs every frame with 3 strategies:
- **Strategy 1**: `FocusManager.FocusedElement` (attached property walk)
- **Strategy 2**: `IsFocused` tree walk (keyboard focus)
- **Strategy 3**: `IsSelected` tree walk via `FindSelectedTabInTree()` — finds first selected ListBoxItem (carousel tabs)

Selection changes take priority over focus changes. DataContext pointer comparison detects carousel recycling (same element, swapped ViewModel). When `forced=true` but both focused and selected are null (UI rebuilding), it re-arms `forceNext_` to keep retrying.

**Overlay/Dialog detection**: Strategies 1 and 2 walk children in REVERSE order so overlay/dialog widgets (rendered last = on top) are checked before the underlying menu widget. Without this, the main widget's stale FocusedElement would be returned, making dialogs completely silent.

**Dialog XAML architecture**: Confirmation dialogs use `LSMessageBoxData` (Title, Text, Actions with LSGameCommandData buttons). They are separate `ls:UIWidget` instances, children of the application Canvas. Buttons use `BoundEvent` (UIAccept, UICancel, UIMessageBoxA/B). Template: `MessageBoxTemplate` in `MessageBoxTemplates.xaml`.

C++ exposes to Lua via `Ext.UI`:
- `SubscribeGlobalFocusChanged(cb)` -- singleton, returns bool
- `ForceGlobalFocusUpdate()` -- re-arms monitor for next tick
- `SubscribePropertyChanged(target, cb)` / `UnsubscribePropertyChanged()` -- INPC, singleton
- `HasProperty(object, name)` -- checks TypeProperty or DependencyProperty existence
- `GetRoot()` -- returns APPLICATION root (Canvas), NOT widget root (see below)
- `GetFocusedElement()` -- uses all 3 focus strategies
- `GetTopmostWidget()` -- topmost visible UIWidget
- `IsMoveFocusFocusable(element)` -- checks Larian's controller focus marker
- `SubscribeDPChanged(element, cb)` / `UnsubscribeDPChanged()` -- DP change subscription (persistent elements only)
- `FindNameInWidget(name)` -- finds a named element across visible widgets by entering their NameScopes from C++. Bypasses the NameScope boundary that blocks Lua Find(). Returns FrameworkElement valid for current tick, or nil.
- `FindNameInWidgetScoped(name, widget)` -- like FindNameInWidget but searches only the given widget Visual (from WidgetAdded callback). Same NameScope-crossing logic, no stale results from old widgets.

### Lua Side (BG3 Mod)

**`ScriptExtender/lua/Client/AccessibilityManager.lua`** — Main manager. Subscribes to GlobalFocusMonitor callback. ALL events arrive as data tables (no Noesis elements in Lua). Dispatches on `eventType`:
- **FocusChanged** (nil): `HandleFocusChange` determines element type from data table fields.
- **PropertyChanged**: `HandleINPC` speaks value-only changes ("On"/"Off" not "Show Tutorials: On").
- **WidgetAdded**: `HandleWidgetChanged` processes new widget DC properties for dialog/overlay text or tab content.
- **WidgetDCChanged**: `HandleWidgetDCChanged` processes widget DC updates after tab content loads (replaces WaitFrames timer).

`HandleFocusChange` determines element type:
- **Option items** (ContentPresenter with DataContext.Text + Value/SelectedItem) → speak name + value
- **Tab items** (ListBoxItem type) → speak tab name; content arrives via WidgetAdded/WidgetDCChanged events from C++ widget DC INPC monitoring.
- **Everything else** (buttons etc.) → speak normally.
- Has text-based dedup fallback for elements with identical elemIds.
- **Navigation hint**: speaks once per Options visit after first option found.
- **Dialog/overlay detection**: handled by WidgetAdded events carrying widget DC properties (Title, Text, Message).
- State resets on GameStateChanged.

**`ScriptExtender/lua/Client/helpers/AccessibilityHelpers.lua`** — Helper functions:
- `ExtractTextFromElement` -- main text extraction (DataContext, visual tree, name fallback)
- `ExtractTabName` -- 6 fallback approaches for carousel tab names
- `FindCarouselAncestor` -- walks up visual tree from ListBoxItem to find ListBox ancestor (generic, replaces hardcoded Find("HeaderCarouselList"))
- `HasNavigableContent` -- checks if a content subtree has ItemsControl/ListView/ListBox with items (universal focusability check)
- `FindFirstOptionItem` -- legacy wrapper using HasNavigableContent + carousel ancestor detection
- `GatherTextBlockTexts` -- recursive three-step TextBlock text extraction via VISUAL tree (GetProperty, Inlines, ToString)
- `GatherLogicalTextBlockTexts` -- same extraction via LOGICAL tree (~80% fewer nodes, skips non-element types)
- `ReadDataContextText` -- ViewModel "Text" or "Title" + value/description reader, LocaString-safe, handles VMPreset
- `ReadDataContextValue` -- value-only reader for INPC callback
- `GatherTabContentText` -- scoped text from tab content area via FindCarouselAncestor + sibling-walk
- `GatherTabBodyText` -- tab-embedded description text (for self-contained tabs like difficulty presets)
- `GatherDialogText` -- all TextBlock text from a widget root (for dialog/overlay detection)
- `GetWidgetRoot` -- walks parent chain to top-level widget element
- `FindSelectedTabLua` -- finds first selected ListBoxItem in visual tree
- `DebugVisualTree` -- diagnostic tree dump

## Critical XAML Architecture: Carousel and Content Are Always Siblings

**This is the most important architectural fact in this codebase.**

In every BG3 menu, the tab carousel and the content area are **siblings** under a shared parent — NOT parent-child. Walking UP from the carousel will NEVER reach the content.

**Options menu** (OptionTemplates_c.xaml → PreviewOptionsTemplate):
```
Grid
├── Control template="OptionTopButtons"   <- carousel (HeaderCarouselList) is INSIDE this
├── Viewbox                               <- content (ItemsControl "Options") is HERE
└── Control template="OptionBottomButtons"
```

**Multiplayer browser** (LobbyBrowser_c.xaml):
```
Grid
├── Grid Row="0"                          <- carousel (HeaderCarouselList) is HERE
├── Grid Row="1" name="MiddleSection"     <- content is HERE
```

**Difficulty selector** (NewGameSettings_c.xaml):
```
GridRoot (3-row Grid)
+-- Row 0: Title + Navigation Controls
+-- Row 1: carouselClipper -> PresetList (ListBox, NOT HeaderCarouselList)
+-- Row 2: Buttons (Confirm, Cancel, etc.)
```
No separate content area. Title + Description are INSIDE each carousel card (RulesetDataTemplate). DataContext type is `gui::VMPreset` with `Title.Str` and `Description.Str` (NOT `Text`).

**Fix pattern**: Use `FindCarouselAncestor()` (walks up visual tree from selected ListBoxItem to find any ListBox ancestor). Then walk up to shared parent, gather text from sibling branches excluding the carousel branch. This is generic -- works for HeaderCarouselList, PresetList, or any ListBox-based carousel.

**Tab content detection**: Instead of checking for `DataContext.Text` (fails for VMPreset), use `HasNavigableContent()` to check if the content area has an ItemsControl/ListView/ListBox with items. If yes (Options, Multiplayer), wait for user navigation. If no (Cross-Play text, Difficulty presets), read body text from the tab element itself or sibling content area.

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
- **Page-level TextBlocks appear in BOTH trees** (e.g. CrossPlayWarningTitle, CrossPlayWarningBody). Only template-internal TextBlocks (inside button/control templates) are visual-tree-only.
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
- **Templates**: `D:\extracted packs\Public\Game\GUI\Library` (OptionTemplates_c.xaml, NewOptionTemplates_c.xaml, etc.)
- **State machines**: `D:\extracted packs\Mods\MainUI\GUI\StateMachines`

**Always consult the XAML before coding solutions involving menu structure.** Do not guess at tree relationships.

## What's Working

- Main menu button navigation (d-pad speaks each button)
- Options menu: tab names, option names + values, INPC value changes
- Multiplayer menu: lobby entries (individual navigation), tab names
- Cross-Play tab: body text via Inlines extraction + ToString for title
- LAN tab: "Finding lobbies..." status text
- Difficulty selection: preset names + descriptions (VMPreset Title.Str + Description.Str)
- Navigation hint on first Options entry
- Carousel recycling detection (RB/LB tab switching)
- Scoped tab content fallback (GatherTabContentText via carousel ancestor + sibling-walk)
- Overlay/dialog detection (widget root change + text gathering)
- Universal carousel lookup (FindCarouselAncestor, not hardcoded to HeaderCarouselList)

## Variable Naming

- **No abbreviations** -- use descriptive names for ALL variables, parameters, and loop iterators. `dataContext` not `dc`. `frameworkElem` not `fe`. `storedVal` not `sv`. `depProp` not `dp`. `bindingInfo` not `bi`.
- **Applies everywhere** -- function parameters, local variables, loop variables, struct fields. No exceptions for "it's just a short function" or "surrounding code does it."
- **Do not copy bad patterns** -- if existing code uses abbreviations, new code still uses full names. Fix old abbreviations when touching those lines.

## Important Rules

- **API first for all data** -- Use BG3SE APIs (`Ext.StaticData.Get`, `Ext.Stats.GetCachedSpell/GetCachedPassive`, `Ext.Entity` components) as the PRIMARY source for names, descriptions, and values. The focus monitor tells us WHAT the user is on (DC type, properties, GUIDs). The API tells us WHAT TO SAY. Noesis element text extraction (dcProps, elemText, sub-tables) is LAST RESORT for data that genuinely has no API. Available StaticData types: Race, ClassDescription, Background, God, Origin, Feat, FeatDescription, ProgressionDescription, Progression. Entity components: CCCharacterDefinition (abilities, race/class/background GUIDs, LevelUpData), CCState (HasDummy).
- **No visual tree walking for text extraction** -- NEVER use GatherTextBlockTexts, GatherVisualTreeText, or any recursive child walk to get text from focused elements. Tree walking grabs wrong text from collapsed states, tooltips, decorative elements, and sibling branches. Use ONLY: dcProps (ViewModel properties), direct property reads (Content/Text via ReadPropertyAsString), or element x:Name cleanup (CleanElementName). If none produce text, stay silent.
- **Noesis element references expire after the tick they're obtained** -- NEVER store across ticks
- **`[ForceUpdate]` strings are unresolved Noesis binding placeholders** -- always filter them out
- **`s_HandleUnknown` strings are unresolved LocaString handles** -- filter them out, do not return raw handle strings
- **GetProperty returns nil for bound values** -- use three-step extraction (GetProperty, Inlines, ToString) for TextBlocks. For other elements, use ReadPropertyAsString which tries TypeProperty > DepProperty > mValues scan.
- **Button Content bindings are NOT in mValues** -- the Noesis Indie SDK doesn't expose the internal value provider chain. Bound Content only exists as rendered text in the control template. Use CleanElementName for buttons instead.
- **Use Ext.UI.FindNameInWidget(name) for cross-tick named lookups** -- bypasses NameScope boundary from C++. Lua Find() only works within the same tick from an element inside the NameScope.
- **Options content area uses `ItemsControl` with `ContentPresenter` children** (NOT ListBoxItems) -- `FindSelectedTabInTree` only finds carousel items, not content items
- **Read too much rather than too little** -- user preference
- **Keep `Co-Authored-By` out of commits**
- The mod folder UUID suffix is the mod's UUID from `meta.lsx`, not per-machine
- **Do not guess at APIs** -- read the C++ source or consult BG3SE_UI_API_REFERENCE.md
- **Do not run build commands** -- the user builds C++ via Visual Studio 2022, Debug configuration. Make code changes and tell the user when to build.

## Key Collaborator: Gem

For complex architectural decisions or problems unsolved after two iterations, consult Gem via PAL MCP. Gem is a Google Gemini instance used as a second-opinion advisor.

## Known Issues / TODO

- **Character creation** -- fallback text reading not yet implemented
- **Dialog button navigation** -- after dialog auto-reads, d-padding between Yes/No should speak each button individually (needs testing)

*Last Updated: 2026-03-13*
*Version: 1.2*