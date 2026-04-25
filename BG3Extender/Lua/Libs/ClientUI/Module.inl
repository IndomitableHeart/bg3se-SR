#include <Lua/Libs/ClientUI/Builtins.inl>
#include <Lua/Libs/ClientUI/NsHelpers.inl>
#include <Lua/Libs/ClientUI/CustomProperties.inl>
#include <Lua/Client/UIEvents.h>
#include <NsGui/UIElementCollection.h>
#include <NsGui/IList.h>
#include <NsGui/INotifyPropertyChanged.h>
#include <NsGui/Enums.h>
#include <NsGui/Binding.h>
#include <NsGui/BindingExpression.h>
#include <NsGui/PropertyPath.h>
#include <NsGui/NameScope.h>
#include <NsGui/Selector.h>
#include <NsCore/Nullable.h>
#include <GameDefinitions/DragDrop.h>
#include <GameDefinitions/Picking.h>

BEGIN_NS(lua)

// Runtime trace-logging flag declared extern in stdafx.h and gated by
// BG3A_TRACE macro.  Toggled from Lua via Ext.UI.SetTraceLogging(bool),
// which is called by Logger.lua's CycleLogLevel so L3+R3 (in-game) or
// `bg3a_log` (SE console) flips both Lua and C++ trace output together.
#ifdef BG3ACCESS_VERBOSE
bool sBG3A_TraceEnabled = false;
#endif

#define FOR_NOESIS_TYPE(T) if (typeName == Noesis::StaticSymbol<T>()) { \
    MakeDirectObjectRef(L, static_cast<T*>(obj), lifetime); return; \
}

void NoesisPush(lua_State* L, Noesis::BaseObject* obj, LifetimeHandle lifetime)
{
    Noesis::gStaticSymbols.Initialize();

    auto cls = obj->GetClassType();

    do {
        auto typeName = cls->GetTypeId();
        FOR_EACH_NOESIS_TYPE()
        cls = cls->GetBase();
    } while (cls != nullptr);

    MakeDirectObjectRef(L, obj, lifetime);
}

#undef FOR_NOESIS_TYPE

#define FOR_NOESIS_TYPE(T) void MakePolymorphicRef(lua_State* L, T* value, LifetimeHandle lifetime) { \
    NoesisPush(L, value, lifetime); \
}

FOR_EACH_NOESIS_TYPE()
#undef FOR_NOESIS_TYPE

void MakePolymorphicRef(lua_State* L, Noesis::RoutedEventArgs* value, LifetimeHandle lifetime)
{
    auto evtName = value->routedEvent->GetName();
    auto const& events = Noesis::gStaticSymbols.Events;

    #define DEFN_EVENT(e, args) if (evtName == events.e) { MakeDirectObjectRef(L, static_cast<Noesis::args##Args*>(value), lifetime); return; }
    #include <Lua/Libs/ClientUI/Events.inl>
    #undef DEFN_EVENT

    MakeDirectObjectRef(L, value, lifetime);
}

END_NS()

/// <lua_module>UI</lua_module>
BEGIN_NS(ecl::lua::ui)

// Forward declarations for static helpers defined later in the file.
static std::string ReadTextBlockText(Noesis::FrameworkElement* elem, bool skipToString = false);
static void CollectNamedTextsFromWidget(
    Noesis::FrameworkElement* widgetElem,
    std::vector<std::pair<std::string, std::string>>& namedTexts);
static void TryCollectNamedTexts(
    Noesis::FrameworkElement* widgetElem,
    std::vector<std::pair<std::string, std::string>>& namedTexts);
static void GatherVisibleTextBlocks(
    Noesis::FrameworkElement* root,
    std::vector<std::string>& outTexts,
    int maxDepth = 12,
    int maxNodes = 256);
static void PollTooltip(
    Noesis::Visual* trueRoot, Noesis::Visual* contentChild,
    ecl::lua::TickSnapshot* snapshot);

Noesis::FrameworkElement* GetRoot()
{
    Noesis::gStaticSymbols.Initialize();
    return (*GetStaticSymbols().ls__gGlobalResourceManager)->UIManager->field_88.Canvas;
}

// ---------------------------------------------------------------------------
// Accessibility: event monitors for INotifyPropertyChanged and
// DependencyPropertyChanged.  Singleton pattern -- at most one active
// subscription each.  Callbacks defer to DeferredUIEvents for thread-safe
// Lua invocation during PostUpdate().
// ---------------------------------------------------------------------------

// Legacy INPCMonitor and DPMonitor deleted -- snapshot system handles all
// property change detection via dirty flags and delta comparison in Tick().

// Forward declarations for focus detection.
static const Noesis::DependencyProperty* sIsFocusedProp = nullptr;
static const Noesis::DependencyProperty* sFocusedElementProp = nullptr;
static const Noesis::DependencyProperty* sIsSelectedProp = nullptr;
static const Noesis::DependencyProperty* sDataContextProp = nullptr;
static const Noesis::DependencyProperty* sIsVisibleProp = nullptr;
static const Noesis::DependencyProperty* sVisibilityProp = nullptr;
static const Noesis::DependencyProperty* sIsHitTestVisibleProp = nullptr;
static const Noesis::DependencyProperty* sFontSizeProp = nullptr;
// Larian's custom controller focus system (ls:MoveFocus.IsFocused attached property).
// Used by all controller menus -- d-pad navigation, state-machine transitions,
// overlay popups (CrossplayDisabledWarning, InviteCode, Filters, etc.).
// Note: ls:MoveFocus has NO FocusedElement property.  The XAML's
// SetMoveFocusAction.FocusElement is an attribute of the action class,
// not an attached DP on ls.MoveFocus.  Focus is tracked per-element via IsFocused.
static const Noesis::DependencyProperty* sLSMoveFocusIsFocusedProp = nullptr;
// ls:MoveFocus.Focusable -- static declaration that an element participates
// in Larian's controller focus system.  Set via Style setters on all
// FocusableContentControlStyle variants.  Unlike IsFocused (runtime toggle),
// this doesn't change -- it's a permanent marker of interactivity.
static const Noesis::DependencyProperty* sLSMoveFocusFocusableProp = nullptr;
// NameScope attached DP -- looked up at runtime since the Indie SDK
// doesn't export NameScope::NameScopeProperty.
static const Noesis::DependencyProperty* sNameScopeProp = nullptr;
// FrameworkElement.Tag -- used by ActionRadials XAML to propagate the
// focused radial slot's DataContext (VMHotBarSlot) via a Blend behavior.
// Polling this property detects radial slot focus changes without needing
// a custom strategy for ls:Radial.LocalFocus.
static const Noesis::DependencyProperty* sTagProp = nullptr;
static const Noesis::TypeClass* sListBoxItemType = nullptr;
static const Noesis::TypeClass* sUIWidgetType = nullptr;
static const Noesis::TypeClass* sDCWidgetType = nullptr;
// SelectionChanged routed event -- looked up at runtime from Selector's
// UIElementData metadata (the Indie SDK doesn't export Selector::SelectionChangedEvent).
// Used for event-driven tab detection (replaces Strategy 3 per-frame tree walk).
static Noesis::RoutedEvent* sSelectionChangedEvent = nullptr;
// GotFocus routed event -- discovered at runtime from UIElement's UIElementData.
// Class handler fires for controller d-pad navigation (logical focus, not keyboard).
// GotKeyboardFocus does NOT fire for controller input -- removed.
static Noesis::RoutedEvent* sGotFocusEvent = nullptr;
// GotFocus class handler outputs.  Read and cleared in Tick().
// Single-threaded: Noesis events fire on the main thread, stable during Tick.
// sLastFocusedElement: persistent "current focused element" pointer.
// Set by GotFocusDelegate alongside sGotFocusSourceElement and never
// cleared per-tick -- replaced when a NEW GotFocus fires, cleared on
// game state changes / resets.  Used by readers (ClassSelectionDelegate
// gate, INPC cached-pointer optimization) that need the focused element
// across multiple ticks, not just the one tick where GotFocus fired.
//
// sLastFocusedWidget: the UIWidget containing sLastFocusedElement.
// Computed once in GotFocusDelegate (parent walk).  Used by the inline
// carousel SelectionChanged gate to verify the source ListBox is in
// the same widget as the focused element -- catches the legitimate
// case (Face cycling: source ListBox is a sibling of focused
// ContentControl, both inside the CC widget) AND rejects bleed (Options
// menu init: source ListBoxes are inside Options_c widget while
// previously focused MainMenu button was inside MainMenu_c).
//
// sGotFocusSourceElement (kept below) is the per-event signal -- consumed
// and cleared in Tick when used as the new tick's `focused` local.
static Noesis::BaseComponent* sLastFocusedElement = nullptr;
static Noesis::Visual* sLastFocusedWidget = nullptr;
// sGotFocusSourceElement persists through settle windows (the tree is stable
// during settle, so the pointer remains valid until consumed post-settle).
static bool sGotFocusDirtyFlag = false;
static Noesis::BaseComponent* sGotFocusSourceElement = nullptr;
// SelectionChanged event handler outputs.  Read and cleared in Tick().
// Single-threaded: Noesis events fire on the main thread, stable during Tick.
static bool sSelectionDirtyFlag = false;
// Static mirror of suppressTick_ for use by class handler delegates,
// which are defined before the GlobalFocusMonitor class.
static bool sSuppressCallbacks = false;
// Address of the newly selected element from the last SelectionChanged event.
// Stored as uintptr_t -- NEVER cast back to a pointer.  Used only to check
// whether the selected item is a ListBoxItem by finding it fresh in the tree.
static uintptr_t sSelectionChangedItemAddr = 0;
// SelectionChanged class handler: element pointer + DC address for direct use.
// Persists through settle windows (tree is stable during settle).
// Cleared after post-settle consumption or on widget set change.
static Noesis::BaseComponent* sClassSelectionItem = nullptr;
static uintptr_t sClassSelectionDCAddr = 0;
// Inline carousel value capture (event-driven).  When ClassSelectionDelegate
// fires for a non-ListBoxItem selection (Face, Skin Colour, Eye Colour,
// Tattoo, Genitals, Scarring, etc.), the new item's Name/ColorName/Title is
// read from the event args directly into sInlineCarouselText and sInlineCarouselDirty
// is set.  Tick() consumes the dirty flag and copies into lastInlineCarouselText_.
// Eliminates the per-tick BFS that previously read the carousel value.
//
// Capture is gated on the source element being a descendant of the currently
// focused element (sGotFocusSourceElement), via a bounded parent-chain walk
// in the handler.  Without the gate, initial SelectionChanged events fired
// during widget init bleed into the focus context (e.g., "Friends only" on
// Options open, where many nested ListBoxes fire their initial selections).
static bool sInlineCarouselDirty = false;
static char sInlineCarouselText[128] = {0};
// UIColor hex from the SelectedItem (e.g. "#FFFFF0E6" for skin/hair/eye
// color swatches).  Captured alongside the display name so Lua can
// convert hex to a spoken color description via HSL binning.
static char sInlineCarouselColorHex[16] = {0};
// Tooltip fingerprint reset: set when focus changes so PollTooltip
// re-enters the stabilization path instead of firing immediate deltas
// while tooltip bindings are still resolving for the new element.
static bool sTooltipFingerprintReset = false;

// Source ListBox for deferred hex retry.  Brush bindings may not
// resolve on the tick SelectionChanged fires; Tick() re-reads
// SelectedItem from this ListBox on the next frame.  Storing the
// ListBox (a visual tree element) is safe across ticks; storing
// the SelectedItem (a VM object) would not be.
static Noesis::FrameworkElement* sInlineCarouselSourceListBox = nullptr;

// ToolTip presence flag (event-driven gate for PollTooltip).
//
// ToolTip.Opened fires when a tooltip becomes visible -- but its content
// tree's bindings haven't propagated yet, so reading content there is
// useless (returns empty in practice).  Instead, we use Opened/Closed
// purely as PRESENCE SIGNALS to gate when PollTooltip runs:
//
//   ToolTip.Opened  -> sToolTipIsOpen = true  -> PollTooltip starts
//   ToolTip.Closed  -> sToolTipIsOpen = false -> PollTooltip stops
//
// PollTooltip runs every tick WHILE a tooltip is open (its stabilization
// logic handles binding lag well -- waits for the fingerprint to stabilize
// before dispatching).  When no tooltip is open, no per-tick BFS happens.
//
// Net effect: zero per-tick walks during normal navigation.  Per-tick
// walks only during the brief window a tooltip is actually visible.
static Noesis::TypeClass const* sToolTipType = nullptr;
static Noesis::RoutedEvent const* sToolTipOpenedEvent = nullptr;
static Noesis::RoutedEvent const* sToolTipClosedEvent = nullptr;
static bool sToolTipHandlersRegistered = false;
static bool sToolTipIsOpen = false;
// Event-driven widget discovery: Loaded/Unloaded class handlers track
// ls.UIWidget lifecycle without per-tick GetVisualChildrenCount/GetVisualChild
// virtual calls (which deadlock against the Noesis rendering thread during
// loading-to-gameplay transitions).
static Noesis::RoutedEvent* sLoadedEvent = nullptr;
static Noesis::RoutedEvent* sUnloadedEvent = nullptr;
static bool sWidgetHandlersRegistered = false;
// Tracked widget array: populated by Loaded/Unloaded class handlers.
// Read in Tick() to build the per-tick widget set.  Max 32 (kMaxWidgets).
static Noesis::Visual* sTrackedWidgets[32] = {};
static uint32_t sTrackedWidgetCount = 0;
static bool sWidgetArrayDirty = false;
// Seed flag: true after the first GatherWidgets_SEH populates the array.
// Existing widgets were loaded before handlers were registered, so the
// first tick after Subscribe() does one GatherWidgets_SEH to seed.
// Always during stable main menu, never during loading transitions.
static bool sWidgetArraySeeded = false;

// No widget info cache needed -- when a widget becomes invisible, it's still
// in sTrackedWidgets (Unloaded hasn't fired yet).  We can read its DC type
// and Name via safe DP reads at the moment of disappearance.
void InitFocusProperties(Noesis::FrameworkElement* root);
Noesis::UIElement* TryFocusManager(Noesis::Visual* elem, int depth,
                                    Noesis::DependencyObject** outScopeRoot = nullptr);
Noesis::UIElement* FindFocusedInTree(Noesis::Visual* elem, int depth);
Noesis::UIElement* FindSelectedTabInTree(Noesis::Visual* elem, int depth);
static bool IsVisibleDP(Noesis::Visual const* elem);
static bool IsUIWidgetType(Noesis::Visual const* elem);
static Noesis::Visual* FindWidgetContainer(Noesis::Visual* root);

// Loading tip buffer: persists across Lua VM resets.  Read during
// loading states by BufferLoadingTips / CaptureLoadingHintFromINPC,
// delivered on first tick after Lua VM reconnects.
static std::vector<std::string> sBufferedLoadingTips;
static int sLastBufferedHintIndex = -2;  // -2 = uninitialized

// Persistent set of tip texts already delivered to Lua this session.
// Prevents the same tip text from being spoken twice, even across
// multi-phase boot sequences and load cycles.  Never cleared -- tips
// rotate between loads, so the set stays small.
static std::vector<std::string> sDeliveredTipTexts;

// Forward declarations -- defined below, after InitFocusProperties.
Noesis::UIElement* GetFocusedElement();
static void TryDiscoverFocusedElementProp(Noesis::Visual* const* widgets, uint32_t count);
Noesis::FrameworkElement* FindNameInWidgetScoped(char const* name, Noesis::Visual* widget);
static void PollRadialLocalFocus(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, ecl::lua::TickSnapshot* snapshot);
static void PollActiveSearchLocalFocus(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, ecl::lua::TickSnapshot* snapshot);
static void PollContextMenu(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, Noesis::UIElement* focusedElement,
    Noesis::Visual* trueRoot, Noesis::Visual* contentChild,
    ecl::lua::TickSnapshot* snapshot);
static bool PollContextMenu_Unsafe(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, Noesis::UIElement* focusedElement,
    Noesis::Visual* trueRoot, Noesis::Visual* contentChild,
    Noesis::FrameworkElement** outHighlightedItem,
    Noesis::FrameworkElement** outTextBlock);

// SEH-guarded Noesis read helpers.  Each function does ONLY raw pointer
// reads (no C++ objects with destructors) inside __try/__except.
// Text extraction (std::string) happens in the caller, outside SEH.
static uint32_t GatherWidgets_SEH(Noesis::Visual* container,
    Noesis::Visual** outWidgets, bool* outVisible, uint32_t maxWidgets);
static void CollectWidgetDCTypes_SEH(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, std::vector<std::string>& outDCTypes,
    std::vector<std::string>& outAddrs,
    std::vector<std::string>& outNames);
static uintptr_t ReadDCAddress_SEH(Noesis::UIElement* elem);
static Noesis::UIElement* FindFocusedElement_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount,
    Noesis::FrameworkElement* root, int maxDepth,
    Noesis::DependencyObject** outScopeRoot);
static Noesis::UIElement* FindSelectedTab_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount,
    Noesis::FrameworkElement* root,
    Noesis::DependencyObject* scopeRoot, int maxDepth);
static Noesis::UIElement* FindIsFocused_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount,
    Noesis::FrameworkElement* root,
    int maxDepth, Noesis::DependencyObject** outScopeRoot);
// Event-driven widget discovery and lightweight polling helpers.
static Noesis::UIElement* ReadFocusManagerDP_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount);
static Noesis::FrameworkElement* FindNameInWidgets_SEH(
    const char* name,
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount);
static Noesis::UIElement* FindSelectedInListBox_SEH(
    Noesis::FrameworkElement* listBox);
static uint32_t ReadTrackedWidgets_SEH(
    Noesis::Visual** outWidgets, bool* outVisible, uint32_t maxWidgets);
static bool DetectWidgetRemoval_SEH(
    uintptr_t const* oldAddrs, bool const* oldVisible, uint32_t oldCount,
    Noesis::Visual* const* newWidgets, bool const* newVisible, uint32_t newCount,
    ecl::lua::FocusEventData* outRemovedData);
static int SafeReadExpanderState_SEH(Noesis::Visual* elem);
static int SafeReadToggleIsChecked_SEH(Noesis::FrameworkElement* elem);
static void ReadElementName_SEH(Noesis::FrameworkElement* elem, char* buf, size_t bufSize);
static float SafeGetFontSize_SEH(Noesis::FrameworkElement* elem);

// Forward declarations for data extraction (defined after ExtractElementInfo).
static void ExtractElementData(FocusEventData& out, Noesis::FrameworkElement* elem);
static void CollectDCProperties_Inner(FocusEventData& out, Noesis::BaseObject* dc);
static void CollectDCProperties(FocusEventData& out, Noesis::BaseObject* dc);
static std::string ReadPropertyAsString(Noesis::BaseObject const* obj, const char* propName);
static void ExtractBindingInfo(FocusEventData& out, Noesis::FrameworkElement* elem);
static bool IsOverlayDCType(const char* dcTypeName);

// GlobalFocusMonitor: per-frame focus/selection change detector.
//
// Strategies (in priority order):
//   1. FocusManager.FocusedElement (FAST PATH) -- O(1) read on each
//      widget, recurses only if not set at widget level.
//   2. IsFocused + ls:MoveFocus.IsFocused tree walk (SAFETY NET) --
//      per visible widget, with IsVisible pruning.  ls:MoveFocus has
//      no FocusedElement property -- focus is tracked per-element via
//      IsFocused only, so the tree walk is required to find it.
//   3. IsSelected tree walk -- for ListBoxItem carousel tabs.
//   4. Widget set change -- detects overlay/dialog appearance when no
//      element has focus (e.g. quit dialog, mod manager splash screen).
//
// Selection changes take priority (tab switch); when the tab is stable,
// focus changes drive the callback (d-pad navigation, button focus).
//
// RECYCLING DETECTION: Noesis carousels virtualise ListBoxItems -- the
// same element object gets reused with swapped DataContext when the user
// presses RB/LB.  Pointer comparison alone would miss the change.
// We also compare the selected element's DataContext pointer each frame.
//
// No routed event subscriptions -- zero HashMap corruption risk.

// Probe a UIElement pointer for validity by touching its vtable.
// Returns true if the pointer dereferences successfully, false if it
// faults (dangling/freed memory).  Must be a standalone function (no
// C++ objects with destructors) for MSVC SEH compatibility.
static bool ProbeUIElement(Noesis::UIElement* elem)
{
    __try {
        (void)elem->GetClassType();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Targeted probe for GetVisualChildrenCount vtable slot, which can be
// independently corrupted on freed elements even when GetClassType passes.
// Use before GetVisualChildrenCount/GetVisualChild on elements obtained
// from tree walks (not on the element itself at function entry -- use
// ProbeUIElement for that).
static bool ProbeVisualChildren(Noesis::Visual* elem)
{
    __try {
        (void)elem->GetVisualChildrenCount();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Reusable SEH helpers for Noesis pointer dereferences.
// Every raw dereference of a pointer obtained from Noesis APIs is a crash
// risk if the object was freed.  These helpers isolate the dangerous
// operations into standalone functions (no C++ objects with destructors)
// so __try/__except is MSVC-compatible.
// ---------------------------------------------------------------------------

// Read DataContext from a DependencyObject.  Combines GetValue + dereference.
// Returns BaseComponent* or nullptr on fault/missing.
static Noesis::BaseComponent* SafeReadDC_SEH(Noesis::DependencyObject const* depObj)
{
    __try {
        if (!sDataContextProp) return nullptr;
        auto dcVal = sDataContextProp->GetValue(depObj);
        if (!dcVal) return nullptr;
        return *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Read a DependencyProperty value from a DependencyObject.
// Returns raw void* or nullptr on fault.
static const void* SafeGetDPValue_SEH(
    Noesis::DependencyProperty const* prop,
    Noesis::DependencyObject const* depObj)
{
    __try {
        return prop->GetValue(depObj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Dereference a void* from GetValue as BaseObject*.
// GetValue returns a pointer to the stored value; for object DPs the
// stored value is itself a pointer, so we need a double dereference.
// Returns BaseObject* or nullptr on fault.
static Noesis::BaseObject* SafeDerefDPObject_SEH(const void* dpVal)
{
    __try {
        if (!dpVal) return nullptr;
        return *reinterpret_cast<Noesis::BaseObject* const*>(dpVal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// GetClassType()->GetName() on any BaseObject.  Returns nullptr on fault.
static const char* SafeBaseObjectTypeName_SEH(Noesis::BaseObject const* obj)
{
    __try {
        auto classType = obj->GetClassType();
        if (!classType) return nullptr;
        return classType->GetName();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// GetClassType() on any BaseObject, returning the Type* for class hierarchy
// walks.  Returns nullptr on fault.
static Noesis::TypeClass const* SafeGetClassType_SEH(Noesis::BaseObject const* obj)
{
    __try {
        return obj->GetClassType();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// DynamicCast to INotifyPropertyChanged.  Returns nullptr on fault.
static Noesis::INotifyPropertyChanged* SafeDynamicCastINPC_SEH(
    Noesis::BaseComponent* dc)
{
    __try {
        return Noesis::DynamicCast<
            Noesis::INotifyPropertyChanged*, Noesis::BaseComponent*>(dc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Forward declaration for SEH-safe INPC subscription helpers below.
// __single_inheritance tells MSVC to use a compact member function pointer
// (8 bytes) instead of the 16-byte "most general" representation it uses
// for incomplete types.  Noesis::Delegate's internal buffer is sized for
// single-inheritance pointers and static_asserts on overflow.
class __single_inheritance GlobalFocusMonitor;

// SEH-safe INPC subscription.  PropertyChanged().Add() mutates the DC
// object's internal delegate list.  If the DC VM is partially constructed
// (e.g., ActiveRoll during widget setup), the delegate list may be in an
// inconsistent state.  An unguarded Add() can corrupt the heap or trigger
// a CRT debug assertion (abort), both of which bypass the top-level SEH.
//
// Inner function: MakeDelegate creates a temporary with a destructor,
// preventing __try in the same scope.
static void SubscribeINPC_Inner(
    Noesis::INotifyPropertyChanged* notifies,
    GlobalFocusMonitor* monitor,
    void (GlobalFocusMonitor::*handler)(Noesis::BaseComponent*,
        const Noesis::PropertyChangedEventArgs&))
{
    notifies->PropertyChanged().Add(
        Noesis::MakeDelegate(monitor, handler));
}

static bool SafeSubscribeINPC_SEH(
    Noesis::INotifyPropertyChanged* notifies,
    GlobalFocusMonitor* monitor,
    void (GlobalFocusMonitor::*handler)(Noesis::BaseComponent*,
        const Noesis::PropertyChangedEventArgs&))
{
    __try {
        SubscribeINPC_Inner(notifies, monitor, handler);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] INPC subscribe faulted -- DC partially constructed?");
        return false;
    }
}

// TypeProperty::Get() on an object.  Returns raw void* or nullptr on fault.
static const void* SafeTypePropertyGet_SEH(
    Noesis::TypeProperty const* prop, Noesis::BaseObject const* obj)
{
    __try {
        return prop->Get(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// DependencyProperty::GetValue() on a DependencyObject.  Returns raw
// const void* or nullptr on fault.
static const void* SafeDepPropertyGetValue_SEH(
    Noesis::DependencyProperty const* dp,
    Noesis::DependencyObject const* obj)
{
    __try {
        return dp->GetValue(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Read DataContext via class cache DP lookup (Indie SDK exports
// neither GetDataContext() nor DataContextProperty).
static Noesis::BaseObject* SafeGetDataContext_SEH(
    Noesis::FrameworkElement* elem)
{
    auto classType = SafeGetClassType_SEH(elem);
    if (!classType) return nullptr;
    static auto sDataContextKey = FixedString("DataContext");
    auto const& cls = Noesis::gClassCache.GetClass(classType);
    auto dcProp = cls.Names.try_get(sDataContextKey);
    if (!dcProp || !dcProp->DepProperty) return nullptr;
    auto val = SafeDepPropertyGetValue_SEH(dcProp->DepProperty, elem);
    if (!val) return nullptr;
    return *reinterpret_cast<Noesis::BaseObject* const*>(val);
}

// TypeProperty::GetCopy() for pointer types.  Writes to outPtr, returns
// true on success.  Returns false on fault.
static bool SafeTypePropertyGetCopy_SEH(
    Noesis::TypeProperty const* prop, Noesis::BaseObject const* obj,
    void* outPtr)
{
    __try {
        prop->GetCopy(obj, outPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Inner function: calls GetComponent (returns Ptr<> with destructor).
// Extracts raw pointer with AddRef so caller can Release() when done.
// Must NOT contain __try (Ptr<> has destructor).
static Noesis::BaseComponent* GetComponentRaw(
    Noesis::TypeProperty const* prop, Noesis::BaseObject* owner)
{
    auto component = prop->GetComponent(owner);
    auto raw = component.GetPtr();
    if (raw) raw->AddReference();
    return raw;
}

// SEH wrapper: calls GetComponentRaw inside __try.
// Returns raw BaseComponent* with extra ref, or nullptr on fault.
static Noesis::BaseComponent* SafeGetComponent_SEH(
    Noesis::TypeProperty const* prop, Noesis::BaseObject* owner)
{
    __try {
        return GetComponentRaw(prop, owner);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Forward declaration for SafeCollectionCount (defined later, used by ReadTextBlockText).
static int SafeCollectionCount(Noesis::BaseCollection* collection);

// Inner function: calls BaseCollection::GetComponent (returns Ptr<>).
// Must NOT contain __try.
static Noesis::BaseComponent* CollectionGetItemRaw(
    Noesis::BaseCollection* collection, uint32_t index)
{
    auto itemPtr = collection->GetComponent(index);
    auto raw = itemPtr.GetPtr();
    if (raw) raw->AddReference();
    return raw;
}

// SEH wrapper: calls CollectionGetItemRaw inside __try.
// Returns raw BaseComponent* with extra ref, or nullptr on fault.
static Noesis::BaseComponent* SafeCollectionGetItem_SEH(
    Noesis::BaseCollection* collection, uint32_t index)
{
    __try {
        return CollectionGetItemRaw(collection, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Forward declarations for post-processors defined after CollectDCProperties.
static void TryCollectSelectionFlyOutTitle(FocusEventData& out, Noesis::BaseObject* dc);
static void TryCollectFinalResult(FocusEventData& out, Noesis::BaseObject* dataContext);

// Forward declaration: defined later in file (full definition is the
// SEH-wrapped version that handles stale type pointers).
static std::string ReadTypePropertyAsString(Noesis::BaseObject const* obj,
                                             Noesis::TypeProperty const* prop);

// Forward declaration: defined later in file.  SEH-safe parent-chain
// walk used by the inline-carousel descendant gate in
// ClassSelectionDelegate.
static Noesis::Visual* SafeGetVisualParent_SEH(Noesis::Visual* visual);

// Forward declaration: defined later in file.  SEH-safe UIWidget type
// check (returns true if the Visual is an ls.UIWidget).
static bool SafeIsUIWidgetType_SEH(Noesis::Visual* visual);

// FindContainingUIWidget_SEH: walk up the visual parent chain from a
// starting element until we hit a UIWidget ancestor.  Returns the
// UIWidget pointer, or nullptr if no UIWidget found within the depth
// cap.  Used to compare "same UIWidget" between the SelectionChanged
// source and the currently focused element.
static Noesis::Visual* FindContainingUIWidget_SEH(
    Noesis::Visual* start, int maxDepth)
{
    auto current = start;
    for (int depth = 0; depth < maxDepth && current; depth++) {
        if (SafeIsUIWidgetType_SEH(current)) return current;
        current = SafeGetVisualParent_SEH(current);
    }
    return nullptr;
}

// CaptureInlineCarouselName_Inner: reads the new SelectedItem's Name,
// ColorName, or Title from a SelectionChanged event's addedItem and stores
// it in sInlineCarouselText.  Called by ClassSelectionDelegate when the
// Forward declaration: SafeToString_SEH is defined later in the file
// but needed here for Brush color extraction.
static bool SafeToString_SEH(Noesis::BaseObject* obj, char* outBuf, size_t bufSize);

// TryCaptureCarouselColorHex: read the Brush property from a carousel
// SelectedItem and store the hex in sInlineCarouselColorHex.  Shared by
// CaptureInlineCarouselName_Inner (at event time) and Tick() (deferred
// retry when the brush binding hadn't resolved at event time).
// Uses FixedString (destructor) -- must be called through a __try caller.
static void TryCaptureCarouselColorHex(Noesis::BaseObject* item)
{
    sInlineCarouselColorHex[0] = 0;
    if (!item) return;
    auto itemClassType = SafeGetClassType_SEH(item);
    if (!itemClassType) return;
    auto const& itemClass = Noesis::gClassCache.GetClass(itemClassType);
    const char* brushPropNames[] = {"Colour", "Color", "UIColor"};
    for (auto brushPropName : brushPropNames) {
        auto brushPropKey = FixedString(brushPropName);
        auto brushPropInfo = itemClass.Names.try_get(brushPropKey);
        if (!brushPropInfo || !brushPropInfo->Property) continue;
        BG3A_TRACE("[BG3Access] COLOR: found prop '%s'", brushPropName);
        auto brushRaw = SafeGetComponent_SEH(
            brushPropInfo->Property, item);
        if (!brushRaw) {
            BG3A_TRACE("[BG3Access] COLOR: GetComponent returned null");
            continue;
        }
        auto brushTypeName = SafeBaseObjectTypeName_SEH(brushRaw);
        BG3A_TRACE("[BG3Access] COLOR: obj type=%s",
                 brushTypeName ? brushTypeName : "(null)");
        if (brushTypeName
            && strstr(brushTypeName, "SolidColorBrush")) {
            char colorBuf[32];
            if (SafeToString_SEH(brushRaw, colorBuf, sizeof(colorBuf))) {
                BG3A_TRACE("[BG3Access] COLOR: ToString=%s", colorBuf);
                if (colorBuf[0] == '#') {
                    strncpy_s(sInlineCarouselColorHex, colorBuf,
                              _TRUNCATE);
                }
            }
        }
        brushRaw->Release();
        break;
    }
}

// Forward declaration (defined after FixedString init section).
static Noesis::BaseObject* GetListBoxSelectedItem(Noesis::FrameworkElement* listBox);

// RetryCarouselColorHex: SEH-wrapped deferred hex capture.  Reads the
// live SelectedItem from the source ListBox and retries the brush read.
// Three-layer SEH: Inner uses FixedString (destructor via
// GetListBoxSelectedItem) and std::string (via TryCaptureCarouselColorHex).
static void RetryCarouselColorHex_Inner(Noesis::FrameworkElement* listBox)
{
    auto liveItem = GetListBoxSelectedItem(listBox);
    if (liveItem) {
        TryCaptureCarouselColorHex(liveItem);
    }
}
static void RetryCarouselColorHex_Invoke(Noesis::FrameworkElement* listBox)
{
    RetryCarouselColorHex_Inner(listBox);
}
static void RetryCarouselColorHex_SEH(Noesis::FrameworkElement* listBox)
{
    __try { RetryCarouselColorHex_Invoke(listBox); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        sInlineCarouselColorHex[0] = 0;
    }
}

// Forward declaration (defined after FixedString init section).
static std::string TryReadInlineCarouselText(Noesis::FrameworkElement* listBox);

// ReadInlineCarouselState: read carousel text + color hex on first
// arrival at a carousel row.  Extracts from ListBox SelectedItem
// (primary) or selectionName TextBlock (fallback).
// Three-layer SEH: Inner uses std::string (TryReadInlineCarouselText,
// ReadTextBlockText) and FixedString (GetListBoxSelectedItem).
static void ReadInlineCarouselState_Inner(
    Noesis::FrameworkElement* listBox,
    Noesis::FrameworkElement* selectionNameTextBlock,
    std::string& lastCarouselText,
    std::string& lastCarouselColorHex)
{
    std::string carouselText;
    if (listBox) {
        carouselText.assign(TryReadInlineCarouselText(listBox));
    }
    if (carouselText.empty() && selectionNameTextBlock) {
        carouselText.assign(ReadTextBlockText(selectionNameTextBlock));
    }

    if (!carouselText.empty()) {
        if (carouselText != lastCarouselText) {
            lastCarouselText.assign(carouselText);
            if (listBox) {
                auto selectedItem = GetListBoxSelectedItem(listBox);
                if (selectedItem) {
                    TryCaptureCarouselColorHex(selectedItem);
                    lastCarouselColorHex.assign(
                        sInlineCarouselColorHex);
                }
            }
            BG3A_TRACE("[BG3Access]   -> Inline carousel changed: %s",
                carouselText.c_str());
        }
    } else {
        if (!lastCarouselText.empty()) {
            lastCarouselText.clear();
        }
    }
}
static void ReadInlineCarouselState_Invoke(
    Noesis::FrameworkElement* listBox,
    Noesis::FrameworkElement* selectionNameTextBlock,
    std::string* lastCarouselText,
    std::string* lastCarouselColorHex)
{
    ReadInlineCarouselState_Inner(
        listBox, selectionNameTextBlock,
        *lastCarouselText, *lastCarouselColorHex);
}
static void ReadInlineCarouselState_SEH(
    Noesis::FrameworkElement* listBox,
    Noesis::FrameworkElement* selectionNameTextBlock,
    std::string& lastCarouselText,
    std::string& lastCarouselColorHex)
{
    auto textPtr = &lastCarouselText;
    auto hexPtr = &lastCarouselColorHex;
    __try {
        ReadInlineCarouselState_Invoke(
            listBox, selectionNameTextBlock, textPtr, hexPtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] ReadInlineCarouselState_SEH: fault");
    }
}

// selection is NOT a ListBoxItem (i.e., it's an inline-carousel selection
// like a Face head, Skin Colour swatch, Eye Colour, Tattoo, etc.).
//
// Cannot live inside the handler's __try block because FixedString has a
// destructor (MSVC C2712).  Called THROUGH the handler's __try so SEH
// faults from stale pointers still get caught.
static void CaptureInlineCarouselName_Inner(Noesis::BaseComponent* item)
{
    if (!item) return;
    auto itemClassType = SafeGetClassType_SEH(item);
    if (!itemClassType) return;
    auto const& itemClass = Noesis::gClassCache.GetClass(itemClassType);
    const char* candidatePropNames[] = {"Name", "ColorName", "Title"};
    for (auto candidatePropName : candidatePropNames) {
        auto propKey = FixedString(candidatePropName);
        auto propInfo = itemClass.Names.try_get(propKey);
        if (propInfo && propInfo->Property) {
            auto text = ReadTypePropertyAsString(item, propInfo->Property);
            if (!text.empty()) {
                strncpy_s(sInlineCarouselText, text.c_str(), _TRUNCATE);
                sInlineCarouselDirty = true;
                break;
            }
        }
    }
    // Capture color hex from Brush property.
    TryCaptureCarouselColorHex(item);
}

// Class-level handler for SelectionChanged with invokeHandledEvents=true.
// Uses EventHandler signature (EventArgs&) instead of RoutedEventHandler
// (RoutedEventArgs&) because UIElementData class handlers use EventHandler.
// Fires at the SOURCE element before instance handlers, so this fires
// before any child ListBox can set Handled=true.  Covers: CC carousels,
// Options tabs, Multiplayer tabs, Dialog choices, Difficulty presets.
// Replaces per-widget instance subscriptions (SubscribeSelectionChangedOnWidgets).
struct ClassSelectionDelegate
{
    void Handler(Noesis::BaseComponent* source, const Noesis::EventArgs& args)
    {
        if (sSuppressCallbacks) return;
        __try {
            auto& selectionArgs = static_cast<const Noesis::SelectionChangedEventArgs&>(args);
            if (selectionArgs.addedItems.Size() == 0) return;

            auto addedItem = selectionArgs.addedItems[0].GetPtr();
            if (!addedItem) return;

            // Classify: ListBoxItem (tab carousel, dialog choices) vs
            // non-ListBoxItem (inline appearance carousels: Face, Skin
            // Colour, Eye Colour, Tattoo, Genitals, Scarring, etc.).
            bool isListBoxItem = false;
            if (sListBoxItemType) {
                auto itemClass = addedItem->GetClassType();
                while (itemClass) {
                    if (itemClass == sListBoxItemType) {
                        isListBoxItem = true;
                        break;
                    }
                    itemClass = itemClass->GetBase();
                }
            }
            if (!isListBoxItem) {
                // Inline carousel selection: capture the new item's
                // Name/ColorName/Title from the event args -- but ONLY
                // if the focused element is INSIDE the source ListBox.
                //
                // XAML ground truth (CCLib_c.xaml AppearanceCarousel
                // template at line 4929): the ListBox's template root is
                // a ContentControl named "base" with ls:MoveFocus.Focusable
                // = true.  When the user navigates to the carousel, focus
                // lands on the ContentControl, which is a child of the
                // ListBox via the template.  So source (ListBox) is a
                // visual ANCESTOR of sLastFocusedElement (ContentControl),
                // not the other way around.  Walk UP from focused until
                // we hit source -- if found, this is the user's carousel.
                bool focusedInSource = false;
                if (sLastFocusedElement) {
                    auto sourceVisual = static_cast<Noesis::Visual*>(
                        static_cast<Noesis::UIElement*>(source));
                    auto current = static_cast<Noesis::Visual*>(
                        static_cast<Noesis::UIElement*>(sLastFocusedElement));
                    for (int depth = 0; depth < 10 && current; depth++) {
                        if (current == sourceVisual) {
                            focusedInSource = true;
                            break;
                        }
                        current = SafeGetVisualParent_SEH(current);
                    }
                    if (focusedInSource) {
                        CaptureInlineCarouselName_Inner(addedItem);
                        // Store source ListBox for deferred hex retry
                        // (brush may not have resolved yet).
                        sInlineCarouselSourceListBox =
                            static_cast<Noesis::FrameworkElement*>(
                                static_cast<Noesis::UIElement*>(source));
                    }
                }
                return;
            }

            sSelectionChangedItemAddr = reinterpret_cast<uintptr_t>(addedItem);

            // Store the element pointer for direct use in Tick() (no tree walk).
            // Valid through settle windows because the tree is stable during settle.
            sClassSelectionItem = addedItem;

            // Read the DataContext address from the ListBoxItem.
            auto frameworkElement = static_cast<Noesis::FrameworkElement*>(
                static_cast<Noesis::UIElement*>(addedItem));
            auto dataContext = SafeReadDC_SEH(
                static_cast<Noesis::DependencyObject const*>(frameworkElement));
            sClassSelectionDCAddr = dataContext
                ? reinterpret_cast<uintptr_t>(dataContext) : 0;

            sSelectionDirtyFlag = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
};

static ClassSelectionDelegate* const kClassSelectionPtr =
    reinterpret_cast<ClassSelectionDelegate*>(static_cast<uintptr_t>(0xACC5E2));

// GotFocus class handler -- fires for logical focus changes (controller d-pad).
// Replaces per-frame Strategies 1+2 (FocusManager.FocusedElement tree walk
// and IsFocused/ls:MoveFocus.IsFocused tree walk).
// GotFocus bubbles up through ancestors (15-20 calls per focus change).
// The dirty flag captures only the first (the actual source element).
struct GotFocusDelegate
{
    void Handler(Noesis::BaseComponent* source, const Noesis::EventArgs& args)
    {
        if (sSuppressCallbacks) return;
        if (sGotFocusDirtyFlag) return;  // Already captured first in bubble chain
        __try {
            sGotFocusSourceElement = source;
            sLastFocusedElement = source;  // persistent across ticks
            sGotFocusDirtyFlag = true;
            // Compute and cache the containing UIWidget so the
            // SelectionChanged gate can compare without a per-event walk.
            sLastFocusedWidget = FindContainingUIWidget_SEH(
                static_cast<Noesis::Visual*>(
                    static_cast<Noesis::UIElement*>(source)),
                30);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
};

static GotFocusDelegate* const kGotFocusPtr =
    reinterpret_cast<GotFocusDelegate*>(static_cast<uintptr_t>(0xACC5E3));

// Loaded class handler for ls.UIWidget -- tracks widget lifecycle.
// Does NOT check sSuppressCallbacks -- must track widgets during loading
// to have accurate state when Tick() resumes after loading ends.
// Loaded fires on the main thread (same as Tick()), so no synchronization.
struct WidgetLoadedDelegate
{
    void Handler(Noesis::BaseComponent* source, const Noesis::EventArgs& args)
    {
        __try {
            auto visual = static_cast<Noesis::Visual*>(
                static_cast<Noesis::UIElement*>(source));
            // Check for duplicate before adding.
            for (uint32_t i = 0; i < sTrackedWidgetCount; i++) {
                if (sTrackedWidgets[i] == visual) return;
            }
            if (sTrackedWidgetCount < 32) {
                sTrackedWidgets[sTrackedWidgetCount++] = visual;
                sWidgetArrayDirty = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
};

static WidgetLoadedDelegate* const kWidgetLoadedPtr =
    reinterpret_cast<WidgetLoadedDelegate*>(static_cast<uintptr_t>(0xACC5E4));

// ToolTip Opened/Closed class handlers -- presence signals only.
// Set sToolTipIsOpen to gate PollTooltip per-tick polling.  See state
// declarations above for the design rationale.
struct ToolTipOpenedDelegate
{
    void Handler(Noesis::BaseComponent* source, const Noesis::EventArgs& args)
    {
        __try {
            if (sSuppressCallbacks) return;
            sToolTipIsOpen = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
};

static ToolTipOpenedDelegate* const kToolTipOpenedPtr =
    reinterpret_cast<ToolTipOpenedDelegate*>(static_cast<uintptr_t>(0xACC5E6));

struct ToolTipClosedDelegate
{
    void Handler(Noesis::BaseComponent* source, const Noesis::EventArgs& args)
    {
        __try {
            if (sSuppressCallbacks) return;
            sToolTipIsOpen = false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
};

static ToolTipClosedDelegate* const kToolTipClosedPtr =
    reinterpret_cast<ToolTipClosedDelegate*>(static_cast<uintptr_t>(0xACC5E7));

// Unloaded class handler for ls.UIWidget -- removes widget from tracked array.
// Does NOT check sSuppressCallbacks for the same reason as WidgetLoadedDelegate.
struct WidgetUnloadedDelegate
{
    void Handler(Noesis::BaseComponent* source, const Noesis::EventArgs& args)
    {
        __try {
            auto visual = static_cast<Noesis::Visual*>(
                static_cast<Noesis::UIElement*>(source));
            for (uint32_t i = 0; i < sTrackedWidgetCount; i++) {
                if (sTrackedWidgets[i] == visual) {
                    // Shift remaining elements down.
                    for (uint32_t j = i; j < sTrackedWidgetCount - 1; j++) {
                        sTrackedWidgets[j] = sTrackedWidgets[j + 1];
                    }
                    sTrackedWidgetCount--;
                    sTrackedWidgets[sTrackedWidgetCount] = nullptr;
                    sWidgetArrayDirty = true;
                    return;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
};

static WidgetUnloadedDelegate* const kWidgetUnloadedPtr =
    reinterpret_cast<WidgetUnloadedDelegate*>(static_cast<uintptr_t>(0xACC5E5));

// SEH-guarded ToString evaluation.
// ToString() triggers binding evaluation which crashes on stale subtrees
// (e.g., inspect panel ContentPresenters mid-rebuild).
// Inner function uses std::string (destructor), SEH wrapper uses POD buffer.
static void SafeToString_Inner(Noesis::BaseObject* obj, char* outBuf, size_t bufSize)
{
    outBuf[0] = '\0';
    auto str = Noesis::ObjectHelpers::ToString(obj);
    if (!str.empty()) {
        size_t len = (str.size() < bufSize - 1) ? str.size() : (bufSize - 1);
        memcpy(outBuf, str.data(), len);
        outBuf[len] = '\0';
    }
}

static bool SafeToString_SEH(Noesis::BaseObject* obj, char* outBuf, size_t bufSize)
{
    outBuf[0] = '\0';
    __try {
        if (!obj) return false;
        SafeToString_Inner(obj, outBuf, bufSize);
        return outBuf[0] != '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outBuf[0] = '\0';
        return false;
    }
}

// SEH-safe visual child count.  Returns 0 on fault.
static uint32_t SafeGetVisualChildrenCount_SEH(Noesis::Visual* visual)
{
    __try {
        return visual->GetVisualChildrenCount();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// SEH-safe visual child access.  Returns nullptr on fault.
static Noesis::Visual* SafeGetVisualChild_SEH(Noesis::Visual* visual, uint32_t index)
{
    __try {
        return visual->GetVisualChild(index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// SEH-safe visual parent access.  Returns mVisualParent or nullptr on fault.
static Noesis::Visual* SafeGetVisualParent_SEH(Noesis::Visual* visual)
{
    __try {
        return visual->mVisualParent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// SEH-safe UIWidget type check.  Returns true if the Visual is an
// ls.UIWidget, false on fault or non-match.
static bool SafeIsUIWidgetType_SEH(Noesis::Visual* visual)
{
    __try {
        return IsUIWidgetType(visual);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-safe validation: check if a UIElement's mRoutedEventHandlers HashMap
// is accessible before attempting to subscribe.  A widget that is mid-
// construction or partially destroyed will fault on the Find call.
// Returns true if safe to subscribe, false if the widget should be skipped.
static bool SafeValidateWidgetHandlers_SEH(
    Noesis::UIElement* uiElement, Noesis::RoutedEvent* event)
{
    __try {
        // Touch the HashMap -- if the widget is stale, this faults.
        uiElement->mRoutedEventHandlers.Find(event);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Forward declarations for free functions used by GlobalFocusMonitor::Tick().
static std::string ShallowChildTextScan(Noesis::FrameworkElement* elem);
static std::string ExtractTabName(Noesis::FrameworkElement* elem);

// Forward declaration for ReadTypePropertyAsString (defined later in file).
static std::string ReadTypePropertyAsString(Noesis::BaseObject const* obj,
                                             Noesis::TypeProperty const* prop);

// Inner SEH-safe function: finds the SelectedItem's BaseObject pointer
// from a ListBox.  No C++ objects with destructors -- SEH compatible.
// Resolve the SelectedItem FixedString once, outside SEH.
static FixedString sSelectedItemKey;

static Noesis::BaseObject* GetListBoxSelectedItem(Noesis::FrameworkElement* listBox)
{
    if (!sSelectedItemKey) {
        sSelectedItemKey = FixedString("SelectedItem");
    }
    auto listBoxClassType = SafeGetClassType_SEH(listBox);
    if (!listBoxClassType) return nullptr;

    auto const& listBoxClass = Noesis::gClassCache.GetClass(listBoxClassType);
    auto selectedItemProp = listBoxClass.Names.try_get(sSelectedItemKey);
    if (!selectedItemProp) return nullptr;

    // Try TypeProperty first (direct getter).
    if (selectedItemProp->Property) {
        auto selectedItemRaw = SafeTypePropertyGet_SEH(
            selectedItemProp->Property, listBox);
        if (selectedItemRaw) {
            return *reinterpret_cast<Noesis::BaseObject* const*>(
                selectedItemRaw);
        }
    }

    // Fallback: DependencyProperty::GetValue.  The Indie SDK does not
    // register SelectedItem as a TypeProperty on ListBox -- only as a
    // DependencyProperty.  Without this fallback, GetListBoxSelectedItem
    // returns null for all ListBox instances.
    if (selectedItemProp->DepProperty) {
        auto depVal = SafeDepPropertyGetValue_SEH(
            selectedItemProp->DepProperty, listBox);
        if (depVal) {
            return *reinterpret_cast<Noesis::BaseObject* const*>(
                depVal);
        }
    }

    return nullptr;
}

// Outer function: reads Name/ColorName/Title from the SelectedItem.
// Uses std::string so cannot contain __try.
static std::string TryReadInlineCarouselText(Noesis::FrameworkElement* listBox)
{
    auto selectedItem = GetListBoxSelectedItem(listBox);
    if (!selectedItem) return {};

    // Validate the item pointer before class cache lookup.
    auto selectedItemClassType = SafeGetClassType_SEH(selectedItem);
    if (!selectedItemClassType) return {};
    auto const& selectedItemClass = Noesis::gClassCache.GetClass(
        selectedItemClassType);
    const char* propertyNames[] = {"Name", "ColorName", "Title"};
    for (auto candidatePropName : propertyNames) {
        auto propKey = FixedString(candidatePropName);
        auto propInfo = selectedItemClass.Names.try_get(propKey);
        if (propInfo && propInfo->Property) {
            auto text = ReadTypePropertyAsString(selectedItem, propInfo->Property);
            if (!text.empty()) return text;
        }
    }
    return {};
}

// DP lookup helpers -- FixedString has a destructor so these MUST be
// separate from any __try function.
static const Noesis::DependencyProperty* LookupContextMenuDP(
    Noesis::TypeClass const* classType)
{
    return Noesis::TypeHelpers::GetDependencyProperty(
        classType, bg3se::FixedString("ContextMenu"));
}

static const Noesis::DependencyProperty* LookupIsOpenDP(
    Noesis::TypeClass const* classType)
{
    return Noesis::TypeHelpers::GetDependencyProperty(
        classType, bg3se::FixedString("IsOpen"));
}

static const Noesis::DependencyProperty* LookupIsHighlightedDP(
    Noesis::TypeClass const* classType)
{
    return Noesis::TypeHelpers::GetDependencyProperty(
        classType, bg3se::FixedString("IsHighlighted"));
}

// Inner: looks up FinalResult DP.  FixedString has a destructor so
// this function cannot contain __try (MSVC C2712).
static const Noesis::DependencyProperty* LookupFinalResultDP_Inner(
    Noesis::TypeClass const* classType)
{
    return Noesis::TypeHelpers::GetDependencyProperty(
        classType, bg3se::FixedString("FinalResult"));
}

// SEH wrapper: classType may be stale.
static const Noesis::DependencyProperty* LookupFinalResultDP(
    Noesis::TypeClass const* classType)
{
    __try {
        return LookupFinalResultDP_Inner(classType);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Read FinalResult int32 DP from a DCActiveRoll DataContext.
// Returns -1 on failure (FinalResult is always >= 0 for valid rolls).
static int32_t ReadFinalResult_SEH(
    Noesis::DependencyObject const* depObj,
    Noesis::DependencyProperty const* finalResultDP)
{
    __try {
        auto dpValue = finalResultDP->GetValue(depObj);
        if (!dpValue) return -1;
        return *static_cast<int32_t const*>(dpValue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}


// Read a bool DP value.  Returns false on null or fault.
static bool SafeReadBoolDP_SEH(
    const Noesis::DependencyProperty* prop,
    Noesis::DependencyObject const* depObj)
{
    __try {
        if (!prop) return false;
        auto val = prop->GetValue(depObj);
        if (!val) return false;
        return *reinterpret_cast<bool const*>(val);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// BFS_CollectByType_SEH: generic BFS that collects elements matching a type
// name substring.  Consolidates FindElementsByType_SEH and
// BFS_CollectTextBlocks_SEH into one function.
//
// Parameters:
//   root:               starting element for the BFS.
//   typeName:           substring to match against element class type name
//                       (e.g. "TextBlock", "ContextMenuItem").
//   outElements:        output buffer for matched element pointers.
//   startIndex:         index in outElements to start writing at.
//   maxOut:             maximum total entries in outElements.
//   checkVisibility:    if true, skip invisible branches (IsVisibleDP).
//   skipMatchedChildren: if true, don't recurse into matched elements
//                        (e.g. TextBlock children are Inlines, not useful).
//
// SEH-safe (no C++ objects with destructors).
// Returns the number of matched elements written (starting at startIndex).
// ---------------------------------------------------------------------------
static uint32_t BFS_CollectByType_SEH(
    Noesis::Visual* root, const char* typeName,
    Noesis::FrameworkElement** outElements, uint32_t startIndex,
    uint32_t maxOut, bool checkVisibility = false,
    bool skipMatchedChildren = false)
{
    uint32_t found = startIndex;
    __try {
        Noesis::Visual* queue[512];
        int head = 0, tail = 0;
        queue[tail++] = root;
        while (head < tail && found < maxOut) {
            auto node = queue[head++];
            if (!node) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(node)))
                continue;
            if (checkVisibility && !IsVisibleDP(node)) continue;

            auto classType = static_cast<Noesis::UIElement*>(
                node)->GetClassType();
            if (classType && strstr(classType->GetName(), typeName)) {
                outElements[found++] = static_cast<
                    Noesis::FrameworkElement*>(node);
                if (skipMatchedChildren) continue;
            }

            if (!ProbeVisualChildren(
                    static_cast<Noesis::UIElement*>(node)))
                continue;
            auto childCount = node->GetVisualChildrenCount();
            for (uint32_t i = 0; i < childCount
                 && tail < 510; i++) {
                auto child = node->GetVisualChild(i);
                if (child) queue[tail++] = child;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// GetVisualTreeRoot_SEH: walk UP via mVisualParent (direct field access,
// same pattern as IsConnectedToWidget) to find the true visual tree root.
// Returns the topmost Visual, or the input element if walk fails.
static Noesis::Visual* GetVisualTreeRoot_SEH(Noesis::Visual* element)
{
    __try {
        auto current = element;
        for (int depth = 0; depth < 64; depth++) {
            auto parent = current->mVisualParent;
            if (!parent) break;
            current = parent;
        }
        return current;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return element;
    }
}

// FindContentChildOfRoot_SEH: walks up from an element via mVisualParent
// to find the direct child of trueRoot that contains it.  Used to
// identify the content tree so Source 3 can skip it.
static Noesis::Visual* FindContentChildOfRoot_SEH(
    Noesis::Visual* element, Noesis::Visual* trueRoot)
{
    __try {
        auto current = element;
        for (int depth = 0; depth < 64 && current; depth++) {
            auto parent = current->mVisualParent;
            if (parent == trueRoot) return current;
            current = parent;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// GetPopupRoots_SEH: returns the visual roots of open Noesis popups.
// Popups (ContextMenu, Tooltip, ComboBox dropdown, etc.) live as
// children of the true visual root alongside the content tree.
// This function returns all non-content children, which are popup roots.
//
// trueRoot and contentChild are cached by GlobalFocusMonitor
// (discovered once per root change).  Per-tick cost: one
// GetVisualChildrenCount + one GetVisualChild per child.
//
// Returns the number of popup roots found (up to maxOut).
static uint32_t GetPopupRoots_SEH(
    Noesis::Visual* trueRoot, Noesis::Visual* contentChild,
    Noesis::Visual** outPopupRoots, uint32_t maxOut)
{
    uint32_t found = 0;
    __try {
        if (!trueRoot) return 0;
        if (!ProbeVisualChildren(
                static_cast<Noesis::UIElement*>(trueRoot)))
            return 0;
        auto childCount = trueRoot->GetVisualChildrenCount();
        for (uint32_t i = 0; i < childCount && found < maxOut; i++) {
            auto child = trueRoot->GetVisualChild(i);
            if (!child || child == contentChild) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(child)))
                continue;
            outPopupRoots[found++] = child;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] GetPopupRoots_SEH: fault");
    }
    return found;
}

// PushWidgetDCType: appends a DC type string to widgetDCTypes if not
// already present.  Inner/Invoke/SEH pattern because
// std::vector<std::string>::push_back involves constructors/destructors.
static void PushWidgetDCType_Inner(
    std::vector<std::string>& widgetDCTypes, const char* dcTypeName)
{
    for (const auto& existingType : widgetDCTypes) {
        if (existingType == dcTypeName) return;
    }
    widgetDCTypes.push_back(dcTypeName);
}
static void PushWidgetDCType_Invoke(
    std::vector<std::string>* widgetDCTypes, const char* dcTypeName) {
    PushWidgetDCType_Inner(*widgetDCTypes, dcTypeName);
}
static void PushWidgetDCType_SEH(
    std::vector<std::string>& widgetDCTypes, const char* dcTypeName) {
    __try {
        PushWidgetDCType_Invoke(&widgetDCTypes, dcTypeName);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] PushWidgetDCType_SEH: fault");
    }
}

// PushWidgetDCTypeAndAddr: mid-tick append of a (DC type, address,
// x:Name) triple into the parallel widgetDCTypes / widgetAddrs /
// widgetNames snapshot vectors.  Unlike PushWidgetDCType, this does
// NOT dedup by type: two widgets with the same DC are distinct
// entities at different addresses, so both entries must be preserved
// for identity-based handler anchoring to work.  Inner/Invoke/SEH
// pattern for the same destructor reasons as above.
static void PushWidgetDCTypeAndAddr_Inner(
    std::vector<std::string>& widgetDCTypes,
    std::vector<std::string>& widgetAddrs,
    std::vector<std::string>& widgetNames,
    const char* dcTypeName, const char* addrStr,
    const char* nameStr)
{
    widgetDCTypes.push_back(dcTypeName);
    widgetAddrs.push_back(addrStr);
    widgetNames.push_back(nameStr ? nameStr : "");
}
static void PushWidgetDCTypeAndAddr_Invoke(
    std::vector<std::string>* widgetDCTypes,
    std::vector<std::string>* widgetAddrs,
    std::vector<std::string>* widgetNames,
    const char* dcTypeName, const char* addrStr,
    const char* nameStr) {
    PushWidgetDCTypeAndAddr_Inner(
        *widgetDCTypes, *widgetAddrs, *widgetNames,
        dcTypeName, addrStr, nameStr);
}
static void PushWidgetDCTypeAndAddr_SEH(
    std::vector<std::string>& widgetDCTypes,
    std::vector<std::string>& widgetAddrs,
    std::vector<std::string>& widgetNames,
    const char* dcTypeName, const char* addrStr,
    const char* nameStr) {
    __try {
        PushWidgetDCTypeAndAddr_Invoke(
            &widgetDCTypes, &widgetAddrs, &widgetNames,
            dcTypeName, addrStr, nameStr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] PushWidgetDCTypeAndAddr_SEH: fault");
    }
}

class GlobalFocusMonitor
{
public:
    static constexpr uint32_t kMaxWidgets = 32;
    // Max recursion depth for tree walks.  IsVisible pruning eliminates
    // collapsed branches, so only visible nodes are visited.  100 is safe
    // (~6KB stack) and prevents silent truncation in deeply templated menus
    // (Larian's ControlTemplates often nest 10+ Grids/Borders per control).
    static constexpr int kMaxTreeDepth = 100;

    static GlobalFocusMonitor& Instance() { static GlobalFocusMonitor inst; return inst; }

    // Public read-only accessors for cached tree roots.  Used by the
    // Lua-facing GetTooltipPopupRoot primitive (which needs to hand
    // these to GetPopupRoots_SEH).  Both may be nullptr between root
    // transitions -- callers must null-check.
    Noesis::Visual* GetTrueRoot() const { return cachedTrueRoot_; }
    Noesis::Visual* GetContentChild() const { return cachedContentChild_; }

    bool Subscribe(lua_State* L, lua::RegistryEntry&& callback)
    {
        Unsubscribe();
        callback.Push(L);
        callback_ = lua::PersistentRegistryEntry(L, -1);
        lua_pop(L, 1);
        lastFocusedAddr_ = 0;
        lastSelectedAddr_ = 0;
        lastSelectedDCAddr_ = 0;
        cachedRootAddr_ = 0;
        widgetContainer_ = nullptr;
        forcedNullCount_ = 0;
        prevWidgetCount_ = 0;
        hadFocusBefore_ = false;

        initialSelectionDone_ = false;
        selectionFiredDuringSettle_ = false;
        sSelectionDirtyFlag = false;
        sGotFocusDirtyFlag = false;
        sGotFocusSourceElement = nullptr;
        sLastFocusedElement = nullptr;
        sLastFocusedWidget = nullptr;
        sClassSelectionItem = nullptr;
        sClassSelectionDCAddr = 0;
        lastRadialTagAddr_ = 0;
        radialWasVisible_ = false;
        inpcSubscribedDCAddr_ = 0;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgetAddrs_[i] = 0;
            prevWidgetVisible_[i] = false;
            settleBaselineWidgetAddrs_[i] = 0;
            settleBaselineWidgetVisible_[i] = false;
        }
        settleBaselineWidgetCount_ = 0;
        // Reset event-driven widget tracking for fresh discovery.
        // Don't clear sTrackedWidgets -- Loaded/Unloaded handlers maintain
        // it across Lua VM resets.  Just mark unseeded so the first tick
        // seeds from the tracked array (no GatherWidgets_SEH if handlers
        // are registered and the array is already populated by events).
        sWidgetArraySeeded = false;
        sWidgetArrayDirty = false;
        dialogPollActive_ = false;
        return true;
    }

    void Unsubscribe()
    {
        callback_ = {};
        lastFocusedAddr_ = 0;
        lastSelectedAddr_ = 0;
        lastSelectedDCAddr_ = 0;
        widgetInpcDCAddr_ = 0;
        widgetInpcWidgetAddr_ = 0;
        inpcSubscribedDCAddr_ = 0;
        sGotFocusDirtyFlag = false;
        sGotFocusSourceElement = nullptr;
        sClassSelectionItem = nullptr;
        sClassSelectionDCAddr = 0;
    }

    bool IsActive() const { return callback_.operator bool(); }
    bool TipsAlreadyDelivered() const { return tipsAlreadyDelivered_; }
    bool ConsumeHintINPCFlag() {
        bool was = hintIndexChangedViaINPC_;
        hintIndexChangedViaINPC_ = false;
        if (was) {
            BG3A_LOG("[BG3Access] INPC hint flag consumed (safe state)");
        }
        return was;
    }

    void ForceNextFire() { forceNext_ = true; }

    // Suppress all Noesis tree walking during loading states.
    // Set from Lua via SuppressGlobalFocusTick() when the game enters
    // a loading state.  Prevents deadlocks caused by our Tick()
    // calling Noesis virtual functions while the loading thread
    // constructs/destroys UI objects under a Noesis internal mutex.
    //
    // Suppresses from frame 1 -- no grace period.  Loading tips are
    // buffered by BufferLoadingTips_SEH (called from TickGlobalFocusMonitor
    // during loading states) and delivered on the first tick after the
    // new Lua VM subscribes.
    void SetSuppressTick(bool suppress) {
        suppressTick_ = suppress;
        sSuppressCallbacks = suppress;
        if (suppress) {
            // Reset delivery flag so INPC and BufferLoadingTips can
            // capture fresh tips during this loading phase.
            //
            // Do NOT clear sBufferedLoadingTips or sLastBufferedHintIndex.
            // The buffer may contain INPC-captured tips from BEFORE
            // the Lua VM reset (CaptureLoadingHintFromINPC fires during
            // UnloadLevel).  Clearing here destroys them before the
            // new VM's first tick can deliver.  The index tracker
            // prevents BufferLoadingTips from re-reading the same hint
            // during multi-phase boot sequences (Menu -> StartLoading
            // -> Idle -> InitConnection -> StartLoading -> Idle).
            tipsAlreadyDelivered_ = false;
            // Clear event-driven pointers.  Loading genuinely destroys
            // elements -- these pointers would be dangling after the load.
            // sSuppressCallbacks prevents the handlers from storing new
            // pointers during the load.
            sGotFocusDirtyFlag = false;
            sGotFocusSourceElement = nullptr;
            sLastFocusedElement = nullptr;
            sLastFocusedWidget = nullptr;
            sClassSelectionItem = nullptr;
            sClassSelectionDCAddr = 0;
            sSelectionDirtyFlag = false;
        }
    }

    void Tick()
    {
        if (!callback_) return;
        if (suppressTick_) return;

        // Decrement INPC cooldown each tick.
        if (inpcCooldown_ > 0) inpcCooldown_--;

        // Guard: Noesis visual trees are single-threaded.  Larian's job
        // system may dispatch OnUpdate on a worker thread during async
        // operations (e.g. lobby loading).  Skip Tick() if we're not on
        // the thread that initialized the monitor.
        if (mainThreadId_ == 0) {
            mainThreadId_ = GetCurrentThreadId();
        } else if (GetCurrentThreadId() != mainThreadId_) {
            return;
        }

        auto root = GetRoot();
        if (!root) return;
        InitFocusProperties(root);

        // ----- Dynamic widget container discovery -----
        auto rootAddr = reinterpret_cast<uintptr_t>(root);
        if (rootAddr != cachedRootAddr_) {
            cachedRootAddr_ = rootAddr;
            widgetContainer_ = FindWidgetContainer(root);
            // Discover true visual root and content child for popup
            // detection.  Only done once per root change.
            cachedTrueRoot_ = GetVisualTreeRoot_SEH(root);
            if (cachedTrueRoot_ && cachedTrueRoot_ != root) {
                cachedContentChild_ = FindContentChildOfRoot_SEH(
                    root, cachedTrueRoot_);
            } else {
                cachedTrueRoot_ = nullptr;
                cachedContentChild_ = nullptr;
            }

            BG3A_LOG("[BG3Access] Root changed: root=%p container=%p",
                root, widgetContainer_);
        }
        if (!widgetContainer_) {
            widgetContainer_ = FindWidgetContainer(root);
        }

        // ----- Gather current widget set -----
        // Event-driven mode: use tracked widget array populated by
        // Loaded/Unloaded class handlers.  Only IsVisibleDP reads per tick.
        // Fallback: per-tick GatherWidgets_SEH (child enumeration).
        Noesis::Visual* widgets[kMaxWidgets] = {};
        bool widgetVisible[kMaxWidgets] = {};
        uint32_t widgetCount = 0;

        if (sWidgetHandlersRegistered) {
            if (!sWidgetArraySeeded) {
                // First tick after Subscribe(): seed the tracked array from
                // GatherWidgets_SEH.  Existing widgets were loaded before
                // handlers were registered.  This runs once during stable
                // main menu, not during loading transitions.
                widgetCount = GatherWidgets_SEH(
                    widgetContainer_, widgets, widgetVisible, kMaxWidgets);
                for (uint32_t widgetIndex = 0; widgetIndex < widgetCount; widgetIndex++) {
                    if (!widgets[widgetIndex]) continue;
                    bool alreadyTracked = false;
                    for (uint32_t trackedIndex = 0;
                         trackedIndex < sTrackedWidgetCount; trackedIndex++) {
                        if (sTrackedWidgets[trackedIndex] == widgets[widgetIndex]) {
                            alreadyTracked = true;
                            break;
                        }
                    }
                    if (!alreadyTracked && sTrackedWidgetCount < kMaxWidgets) {
                        sTrackedWidgets[sTrackedWidgetCount++] = widgets[widgetIndex];
                    }
                }
                sWidgetArraySeeded = true;
                BG3A_LOG("[BG3Access] Widget array seeded: %u from container, %u tracked total",
                    widgetCount, sTrackedWidgetCount);
            } else {
                // Normal tick: read tracked widgets + IsVisibleDP.
                // Zero GetVisualChildrenCount/GetVisualChild calls.
                widgetCount = ReadTrackedWidgets_SEH(
                    widgets, widgetVisible, kMaxWidgets);
            }
        } else {
            // Fallback: per-tick child enumeration (handlers not registered).
            widgetCount = GatherWidgets_SEH(
                widgetContainer_, widgets, widgetVisible, kMaxWidgets);
        }

        // ----- Quick widget set change check -----
        // Detects pointer changes AND visibility changes (dialogs are
        // pre-created hidden children that become visible on trigger).
        bool widgetSetJustChanged = false;
        if (widgetCount != prevWidgetCount_) {
            widgetSetJustChanged = true;
        } else if (widgetCount > 0) {
            for (uint32_t i = 0; i < widgetCount; i++) {
                if (reinterpret_cast<uintptr_t>(widgets[i]) != prevWidgetAddrs_[i]) {
                    widgetSetJustChanged = true;
                    break;
                }
                if (widgetVisible[i] != prevWidgetVisible_[i]) {
                    widgetSetJustChanged = true;
                    break;
                }
            }
        }

        // ----- Dynamic tree settle -----
        // Instead of a fixed frame count, wait until NOTHING changes for
        // 3 consecutive frames.  "Change" = SelectionChanged event OR
        // widget set change.  This adapts to however long Noesis takes
        // to finish building -- no guessing, no double-settle chaining.
        // Hard cap at 30 frames (~500ms) to prevent infinite settling.
        //
        // IMPORTANT: Capture the pre-update widget state BEFORE the
        // widgetSetJustChanged block updates prevWidgetAddrs_.  When
        // SelectionChanged and a widget swap happen on the same tick,
        // the baseline must reflect the state BEFORE the swap, not after.
        uint32_t preUpdateWidgetCount = prevWidgetCount_;
        uintptr_t preUpdateWidgetAddrs[kMaxWidgets];
        bool preUpdateWidgetVisible[kMaxWidgets];
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            preUpdateWidgetAddrs[i] = prevWidgetAddrs_[i];
            preUpdateWidgetVisible[i] = prevWidgetVisible_[i];
        }

        bool somethingChanged = false;
        if (sSelectionDirtyFlag) {
            sSelectionDirtyFlag = false;
            sSelectionChangedItemAddr = 0;
            somethingChanged = true;
            selectionFiredDuringSettle_ = true;
        }
        if (widgetSetJustChanged && hadFocusBefore_) {
            // Update widget baseline so next tick can detect further changes.
            prevWidgetCount_ = widgetCount;
            for (uint32_t i = 0; i < kMaxWidgets; i++) {
                prevWidgetAddrs_[i] = (i < widgetCount)
                    ? reinterpret_cast<uintptr_t>(widgets[i]) : 0;
                prevWidgetVisible_[i] = (i < widgetCount) ? widgetVisible[i] : false;
            }
            somethingChanged = true;
            // Do NOT clear event-driven pointers here.  Widget set changes
            // during settle trigger new Noesis events that overwrite stale
            // pointers with fresh ones pointing to the rebuilt elements.
            // Clearing them here destroys pointers from events that fired
            // AFTER the last widget change, forcing unnecessary tree walks
            // post-settle.  sSuppressCallbacks (loading states) prevents
            // handlers from firing when elements are genuinely destroyed.
        }

        if (somethingChanged) {
            if (!settling_) {
                BG3A_TRACE("[BG3Access] Settle: started (waiting for stability)");
                settling_ = true;
                settleStableCount_ = 0;
                settleTotalCount_ = 0;
                // Save the pre-update widget state as baseline.  This is
                // from BEFORE widgetSetJustChanged updated prevWidgetAddrs_,
                // so it reflects the state before any widget swaps on this tick.
                settleBaselineWidgetCount_ = preUpdateWidgetCount;
                for (uint32_t i = 0; i < kMaxWidgets; i++) {
                    settleBaselineWidgetAddrs_[i] = preUpdateWidgetAddrs[i];
                    settleBaselineWidgetVisible_[i] = preUpdateWidgetVisible[i];
                }
            } else {
                // Something changed during settle -- reset stability counter.
                settleStableCount_ = 0;
            }
        }

        if (settling_) {
            settleTotalCount_++;

            if (somethingChanged) {
                settleStableCount_ = 0;
            } else {
                settleStableCount_++;
            }

            if (settleStableCount_ >= 5 || settleTotalCount_ >= 30) {
                // Stable for 5 frames or hard cap reached.
                BG3A_TRACE("[BG3Access] Settle: complete after %u frames (%u stable)",
                     settleTotalCount_, settleStableCount_);
                settling_ = false;
                settleStableCount_ = 0;
                settleTotalCount_ = 0;
                forceNext_ = true;
                postSettle_ = true;
                // Preserve selectionFiredDuringSettle_ -- consumed
                // by shouldRunStrategy3 below, then cleared.
                lastFocusedAddr_ = 0;
                lastFocusedDCAddr_ = 0;
                lastSelectedAddr_ = 0;
                lastSelectedDCAddr_ = 0;
            } else {
                // Still settling -- skip tick body.
                return;
            }
        }

        // ----- Normal tick: tree is stable, safe to walk -----

        // Per-tick snapshot: heap-allocated to reduce Tick() stack frame.
        auto snapshotPtr = std::make_unique<ecl::lua::TickSnapshot>();
        auto snapshot = snapshotPtr.get();

        // ----- Per-tick widgetDCTypes collection (cached) -----
        // Widget DC types don't change between ticks -- only when the
        // widget set itself changes.  Cache the result and reuse it on
        // unchanged ticks.  This eliminates per-tick Noesis DP reads
        // (SafeReadDC_SEH, ProbeUIElement) that can deadlock against
        // the Noesis rendering thread.  SEH cannot catch deadlocks.
        if (widgetSetJustChanged || cachedWidgetDCTypes_.empty()) {
            // TICK[A] breadcrumb removed -- per-tick logging floods the log.
            cachedWidgetDCTypes_.clear();
            cachedWidgetAddrs_.clear();
            cachedWidgetNames_.clear();
            CollectWidgetDCTypes_SEH(
                widgets, widgetVisible, widgetCount,
                cachedWidgetDCTypes_, cachedWidgetAddrs_,
                cachedWidgetNames_);
        }
        snapshot->widgetDCTypes = cachedWidgetDCTypes_;
        snapshot->widgetAddrs = cachedWidgetAddrs_;
        snapshot->widgetNames = cachedWidgetNames_;

        // ----- Deliver buffered loading tips -----
        // Check the buffer directly every tick instead of using a flag.
        // Tips can be buffered by BufferLoadingTips_SEH during safe
        // states AFTER Subscribe() runs, so a flag set at subscribe
        // time misses late-buffered tips.
        if (!sBufferedLoadingTips.empty()) {
            snapshot->widgetAdded = true;
            snapshot->widgetEvents.emplace_back();
            auto& tipEvent = snapshot->widgetEvents.back();
            tipEvent.dcType = "ls.LoadingScreen";
            tipEvent.eventType = "WidgetAdded";
            int tipNumber = 0;
            for (auto& tipText : sBufferedLoadingTips) {
                // Record in the persistent delivered set so the same
                // text is never spoken again this session.
                sDeliveredTipTexts.push_back(tipText);
                tipNumber++;
                std::string key = "_loadingHint_"
                    + std::to_string(tipNumber);
                tipEvent.namedTexts.push_back(
                    {std::move(key), tipText});
            }
            BG3A_LOG("[BG3Access] Delivering %d buffered loading tips",
                     tipNumber);
            sBufferedLoadingTips.clear();
            // Do NOT reset sLastBufferedHintIndex here.  Keeping it
            // at its last value prevents BufferLoadingTips from
            // re-reading the same VisibileHintIndex on a subsequent
            // loading phase within the same boot sequence.
            tipsAlreadyDelivered_ = true;
        }

        // ----- Lazy FocusedElement property discovery -----
        if (!sFocusedElementProp && widgetCount > 0) {
            TryDiscoverFocusedElementProp(widgets, widgetCount);
        }

        // ----- Focus + Selection detection -----
        // Two modes gated by useEventDrivenFocus_:
        // EVENT-DRIVEN (default): GotFocus class handler provides the
        //   focused element; SelectionChanged class handler provides
        //   the selected element.  Zero per-frame tree walks.
        //   Fallback to tree walks on forced/post-settle ticks to
        //   discover current state when no event fired.
        // TREE-WALK (legacy): FindFocusedElement_SEH (Strategies 1+2)
        //   and FindSelectedTab_SEH (Strategy 3) every applicable tick.
        Noesis::DependencyObject* scopeRoot = nullptr;
        Noesis::UIElement* focused = nullptr;
        Noesis::UIElement* selected = nullptr;
        // Track whether event-driven mode skipped detection (no event,
        // not forced).  Used to suppress false focusChanged/selectionChanged.
        bool eventDrivenFocusSkip = false;
        bool eventDrivenSelectionSkip = false;

        if (useEventDrivenFocus_) {
            // ----- Event-driven focus (replaces Strategies 1+2) -----
            if (sGotFocusDirtyFlag) {
                // GotFocus fired this frame (or during settle).
                // Source is the first element in the bubble chain.
                focused = static_cast<Noesis::UIElement*>(sGotFocusSourceElement);
                sGotFocusDirtyFlag = false;
                sGotFocusSourceElement = nullptr;
                auto focusTypeName = focused
                    ? SafeBaseObjectTypeName_SEH(focused) : "null";
                BG3A_TRACE("[BG3Access] FOCUS_SRC=EVENT elem=%p type=%s",
                    focused, focusTypeName ? focusTypeName : "?");
            } else if (forceNext_) {
                // Post-settle or initial: discover current focus state.
                // Strategy 1 (FocusManager), then Strategy 2 (IsFocused).
                focused = FindFocusedElement_SEH(
                    widgets, widgetVisible, widgetCount,
                    root, kMaxTreeDepth, &scopeRoot);
                if (!focused) {
                    focused = FindIsFocused_SEH(
                        widgets, widgetVisible, widgetCount,
                        root, kMaxTreeDepth, &scopeRoot);
                }
                BG3A_TRACE("[BG3Access] FOCUS_SRC=TREE_WALK (forced) elem=%p", focused);
            } else {
                // No GotFocus event, not forced.
                // Dialogue choice d-pad changes IsSelected without firing
                // GotFocus or SelectionChanged.  Poll Strategy 3 only when:
                // 1. DCDialogue is in the visible widget set
                // 2. Nothing has focus (lastFocusedAddr_ == 0)
                //
                // When menus are open, GotFocus fires and sets
                // lastFocusedAddr_.  When dialogue is active, GotFocus
                // never fires and lastFocusedAddr_ stays 0.  This
                // correctly distinguishes "in dialogue" from "in menu
                // with Dialogue_c also loaded in background."
                bool hasDialogueWidget = false;
                for (auto& widgetDCType : cachedWidgetDCTypes_) {
                    if (widgetDCType.find("DCDialogue") != std::string::npos) {
                        hasDialogueWidget = true;
                        break;
                    }
                }
                // Dialogue poll: Lua signals via SetDialoguePollActive()
                // when dialogue is active.  No menu has focus during
                // dialogue (lastFocusedAddr_ stays 0).  Zero cost when
                // dialogue is inactive (dialogPollActive_ == false).
                bool shouldPollDialogue = dialogPollActive_
                    && lastFocusedAddr_ == 0;
                if (shouldPollDialogue && sIsSelectedProp && sListBoxItemType) {
                    // Try fast path: NameScope lookup for answerList ListBox
                    // + shallow descent to find IsSelected item.
                    auto answerList = FindNameInWidgets_SEH(
                        "answerList", widgets, widgetVisible, widgetCount);
                    if (answerList) {
                        selected = FindSelectedInListBox_SEH(answerList);
                    }
                    // Fallback: full tree walk if fast path missed.
                    // Only runs during active dialogue, not per-frame.
                    if (!selected) {
                        selected = FindSelectedTab_SEH(
                            widgets, widgetVisible, widgetCount,
                            root, scopeRoot, kMaxTreeDepth);
                    }
                    if (selected) {
                        // Only log when the selected element actually changed.
                        if (reinterpret_cast<uintptr_t>(selected) != lastSelectedAddr_) {
                            BG3A_TRACE("[BG3Access] SEL_SRC=DIALOGUE_POLL elem=%p", selected);
                        }
                        eventDrivenSelectionSkip = false;
                    }
                }
                // Expander toggle detection: on event-skip ticks where
                // something previously had focus AND the last dispatched
                // element was in an expander context (isChecked >= 0),
                // do a lightweight FocusManager DP read to get a fresh
                // pointer and check if isChecked changed.  Total cost:
                // N DP reads (one per visible widget) + 1 isChecked read.
                if (!focused && lastFocusedAddr_ != 0
                    && lastDispatchedIsChecked_ >= 0) {
                    auto expanderCandidate = ReadFocusManagerDP_SEH(
                        widgets, widgetVisible, widgetCount);
                    // Fallback: if FocusManager didn't return the element
                    // (Larian's MoveFocus system, not FocusManager), probe
                    // the cached pointer directly.
                    if (!expanderCandidate
                        || reinterpret_cast<uintptr_t>(expanderCandidate)
                            != lastFocusedAddr_) {
                        auto cached = reinterpret_cast<Noesis::UIElement*>(
                            lastFocusedAddr_);
                        if (ProbeUIElement(cached)) {
                            expanderCandidate = cached;
                        }
                    }
                    if (expanderCandidate
                        && reinterpret_cast<uintptr_t>(expanderCandidate)
                            == lastFocusedAddr_) {
                        int currentIsChecked = SafeReadExpanderState_SEH(
                            expanderCandidate);
                        // Fallback: standalone CheckBox (no Expander ancestor).
                        if (currentIsChecked < 0) {
                            currentIsChecked = SafeReadToggleIsChecked_SEH(
                                static_cast<Noesis::FrameworkElement*>(
                                    expanderCandidate));
                        }
                        if (currentIsChecked >= 0
                            && currentIsChecked != lastDispatchedIsChecked_) {
                            // isChecked changed -- provide the element so
                            // ExtractElementData runs and the existing
                            // expander comparison in snapshot finalization
                            // triggers valueChanged.
                            focused = expanderCandidate;
                        }
                    }
                }
                if (!focused) {
                    eventDrivenFocusSkip = true;
                }
            }

            // ----- Event-driven selection (replaces Strategy 3) -----
            // The class handler stores the selected ListBoxItem pointer
            // directly -- no tree walk needed.  sSelectionDirtyFlag was
            // consumed at the top of Tick() for settle detection; the
            // stored pointer (sClassSelectionItem) persists through settle.
            if (sClassSelectionItem) {
                selected = static_cast<Noesis::UIElement*>(sClassSelectionItem);
                sClassSelectionItem = nullptr;
                sSelectionChangedItemAddr = 0;
                BG3A_TRACE("[BG3Access] SEL_SRC=EVENT elem=%p dc=0x%llx",
                    selected, (unsigned long long)sClassSelectionDCAddr);
            } else if (forceNext_ && sIsSelectedProp && sListBoxItemType) {
                // Post-settle or initial: discover current selection.
                selected = FindSelectedTab_SEH(
                    widgets, widgetVisible, widgetCount,
                    root, scopeRoot, kMaxTreeDepth);
                BG3A_TRACE("[BG3Access] SEL_SRC=TREE_WALK (forced) elem=%p", selected);
            } else if (!selected) {
                // No selection from event, forced walk, or dialogue poll.
                eventDrivenSelectionSkip = true;
                sSelectionChangedItemAddr = 0;
            }
        } else {
            // ----- Legacy tree-walk mode -----
            BG3A_TRACE("[BG3Access] FOCUS_SRC=LEGACY_WALK");
            // Strategy 1: FocusManager.FocusedElement.
            focused = FindFocusedElement_SEH(
                widgets, widgetVisible, widgetCount,
                root, kMaxTreeDepth, &scopeRoot);
            // Strategy 2: IsFocused + ls:MoveFocus.IsFocused tree walk.
            if (!focused) {
                focused = FindIsFocused_SEH(
                    widgets, widgetVisible, widgetCount,
                    root, kMaxTreeDepth, &scopeRoot);
            }

            // Strategy 3: IsSelected tree walk (event-gated).
            if (widgetSetJustChanged && widgetCount > prevWidgetCount_) {
                initialSelectionDone_ = false;
            }
            bool shouldRunStrategy3 = !sSelectionChangedEvent
                                   || !initialSelectionDone_
                                   || selectionFiredDuringSettle_
                                   || postSettle_;
            selectionFiredDuringSettle_ = false;

            if (sIsSelectedProp && sListBoxItemType && shouldRunStrategy3) {
                strategy3Runs_++;
                initialSelectionDone_ = true;
                sSelectionChangedItemAddr = 0;
                selected = FindSelectedTab_SEH(
                    widgets, widgetVisible, widgetCount,
                    root, scopeRoot, kMaxTreeDepth);
                if (reinterpret_cast<uintptr_t>(selected) != lastSelectedAddr_) strategy3Changes_++;
            } else if (sIsSelectedProp && sListBoxItemType) {
                sSelectionChangedItemAddr = 0;
            }
        }

        // Track whether selected was found fresh this tick (safe to
        // dereference) vs not found (skipped or nothing selected).
        bool selectedIsFresh = (selected != nullptr);
        auto selectedAddr = reinterpret_cast<uintptr_t>(selected);

        // When detection was skipped, use the stored address for
        // comparison only (selected pointer is null, cannot dereference).
        uintptr_t effectiveSelectedAddr = selectedIsFresh ? selectedAddr : lastSelectedAddr_;

        // ----- DataContext for recycling detection (SEH-guarded) -----
        uintptr_t selectedDCAddr = 0;
        if (selected && selectedIsFresh) {
            // Class handler stored the DC addr; use it if available.
            if (useEventDrivenFocus_ && sClassSelectionDCAddr != 0) {
                selectedDCAddr = sClassSelectionDCAddr;
                sClassSelectionDCAddr = 0;
            } else {
                selectedDCAddr = ReadDCAddress_SEH(selected);
            }
        }

        // ----- Focused element DataContext for DC-swap detection -----
        // Same pattern as selDC: address-only, never dereferenced.
        // Catches content-area items (e.g. Skills) whose container is
        // reused with a new ViewModel a few ticks after focus arrives.
        uintptr_t focusedDCAddr = 0;
        if (focused) {
            focusedDCAddr = ReadDCAddress_SEH(focused);
        }

        bool forced = forceNext_;
        forceNext_ = false;

        // Track focus and selection independently.
        // When event-driven and no event fired (skip), suppress change detection.
        bool focusChanged;
        if (eventDrivenFocusSkip) {
            focusChanged = false;
        } else {
            focusChanged = (reinterpret_cast<uintptr_t>(focused) != lastFocusedAddr_);
            if (!focusChanged && focused
                && focusedDCAddr != lastFocusedDCAddr_
                && focusedDCAddr != 0 && lastFocusedDCAddr_ != 0) {
                // Same element, DC swapped to a different non-null value
                focusChanged = true;
            }
        }
        // Selection change fires when:
        // - DataContext address changed (carousel recycling: same element,
        //   new DC -- fires once).
        // - Element address changed (static tabs like multiplayer: different
        //   ListBoxItem, but DC may be null for all of them).
        // - Selection appeared/disappeared (null transitions).
        //
        // Widget rebuild creates a new element for the same DC.  When DC is
        // non-null, the DC check catches recycling.  When DC is null, the
        // element address check catches the tab switch.
        // Detect genuine selection changes:
        // - DC changed to a DIFFERENT non-null value (carousel recycling)
        // - Element address changed (different tab selected)
        // - New selection appeared (null -> non-null element)
        //
        // IGNORE: DC going null while the element stays the same.
        // This happens every other frame during options scrolling --
        // the carousel tab's DC pointer temporarily clears and returns.
        // Without this guard, the false selectionChanged suppresses
        // focus callbacks, making d-pad navigation through options silent.
        bool selectionChanged = false;
        if (eventDrivenSelectionSkip) {
            // No selection event and not forced -- no change.
        } else if (effectiveSelectedAddr != lastSelectedAddr_) {
            // Different element (or appeared/disappeared) -- real change
            selectionChanged = true;
        } else if (selectedDCAddr != lastSelectedDCAddr_
                   && selectedDCAddr != 0 && lastSelectedDCAddr_ != 0) {
            // Same element, DC changed to a different non-null value
            // (carousel recycling: same ListBoxItem, swapped ViewModel)
            selectionChanged = true;
        }

        // ----- Strategy 4: Widget set change detection -----
        // Compare current widget addresses against stored addresses.
        // On post-settle ticks, compare against the PRE-SETTLE baseline
        // (saved when settle started).  During settle, prevWidgetAddrs_
        // is updated each frame for stability detection, which destroys
        // the original baseline.  Without this, widgets added during
        // settle (dialogs, overlays) would never be detected as "new."
        uint32_t oldWidgetCount;
        uintptr_t oldWidgetAddrs[kMaxWidgets] = {};
        bool oldWidgetVisible[kMaxWidgets] = {};
        if (postSettle_) {
            oldWidgetCount = settleBaselineWidgetCount_;
            for (uint32_t i = 0; i < oldWidgetCount && i < kMaxWidgets; i++) {
                oldWidgetAddrs[i] = settleBaselineWidgetAddrs_[i];
                oldWidgetVisible[i] = settleBaselineWidgetVisible_[i];
            }
        } else {
            oldWidgetCount = prevWidgetCount_;
            for (uint32_t i = 0; i < oldWidgetCount && i < kMaxWidgets; i++) {
                oldWidgetAddrs[i] = prevWidgetAddrs_[i];
                oldWidgetVisible[i] = prevWidgetVisible_[i];
            }
        }

        bool widgetSetChanged = false;
        if (widgetCount != oldWidgetCount) {
            widgetSetChanged = true;
        } else {
            for (uint32_t i = 0; i < widgetCount; i++) {
                if (reinterpret_cast<uintptr_t>(widgets[i]) != oldWidgetAddrs[i]) {
                    widgetSetChanged = true;
                    break;
                }
                if (widgetVisible[i] != oldWidgetVisible[i]) {
                    widgetSetChanged = true;
                    break;
                }
            }
        }

        // When widgets change, reset selection tracking.
        if (widgetSetChanged) {
            initialSelectionDone_ = false;
        }

        // ----- Widget removal detection -----
        // When a previously-visible widget becomes invisible, report it
        // to Lua so the menu handler can deactivate.  Only checks on
        // widgetSetChanged ticks (no extra cost on stable ticks).
        if (widgetSetChanged) {
            bool removalDetected = DetectWidgetRemoval_SEH(
                oldWidgetAddrs, oldWidgetVisible, oldWidgetCount,
                widgets, widgetVisible, widgetCount,
                &snapshot->removedWidgetData);
            if (removalDetected) {
                snapshot->widgetRemoved = true;
            }
        }

        // Update cached widget addresses and visibility.  Stored as
        // uintptr_t -- NEVER cast back to pointers.  Used only for
        // equality comparison to detect widget set changes between ticks.
        prevWidgetCount_ = widgetCount;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgetAddrs_[i] = (i < widgetCount) ? reinterpret_cast<uintptr_t>(widgets[i]) : 0;
            prevWidgetVisible_[i] = (i < widgetCount) ? widgetVisible[i] : false;
        }

        // ----- Diagnostic logging -----
        // Suppress log for forced-only no-op ticks (nothing found, nothing
        // changed).  These spam the log at 60fps during loading screens
        // when widgets exist but nothing has focus yet.
        bool forcedNoOp = forced && !focusChanged && !selectionChanged
                        && !widgetSetChanged && !focused && !selected;
        if ((focusChanged || selectionChanged || forced || widgetSetChanged)
            && !forcedNoOp) {
            BG3A_TRACE("[BG3Access] Tick: focused=%p scopeRoot=%p selected=%p selDC=0x%llx prevSelDC=0x%llx focDC=0x%llx prevFocDC=0x%llx focChg=%d selChg=%d forced=%d wChg=%d widgets=%u",
                focused, scopeRoot, selected,
                (unsigned long long)selectedDCAddr, (unsigned long long)lastSelectedDCAddr_,
                (unsigned long long)focusedDCAddr, (unsigned long long)lastFocusedDCAddr_,
                focusChanged, selectionChanged, forced, widgetSetChanged, widgetCount);

            // (Per-widget diagnostic dump removed -- no longer needed for
            // radial/overlay detection; DCHotBar filter handles it.)
        }


        // ----- Update last known state (addresses only) -----
        if (focusChanged) {
            lastFocusedAddr_ = reinterpret_cast<uintptr_t>(focused);
            lastFocusedDCAddr_ = focusedDCAddr;
        }

        if (selectionChanged) {
            lastSelectedAddr_ = effectiveSelectedAddr;
            lastSelectedDCAddr_ = selectedDCAddr;
            // Cache tab name for carousel text-change detection.
            // selected is a fresh pointer (obtained this frame), safe to dereference.
            if (selected && selectedIsFresh) {
                lastSelectedElemText_ = ExtractTabName(
                    static_cast<Noesis::FrameworkElement*>(selected));
            } else {
                lastSelectedElemText_.clear();
            }
        }

        // ----- Schedule deferred namedTexts on tab switch -----
        // When a tab changes, Noesis hasn't updated Visibility states yet
        // in the same frame.  Schedule a re-collection after a settle delay
        // so IsElementVisible can reliably filter cross-tab TextBlocks.
        if (selectionChanged && selected && selectedIsFresh) {
            auto selData = static_cast<Noesis::FrameworkElement*>(selected);
            // Check isTab by type
            bool isTab = false;
            if (sListBoxItemType) {
                auto cls = SafeGetClassType_SEH(selData);
                while (cls) {
                    if (cls == sListBoxItemType) { isTab = true; break; }
                    cls = cls->GetBase();
                }
            }
            (void)isTab;  // Used only for diagnostic/future expansion.
        }

        // ----- Fire callbacks (priority: selection > focus > forced) -----
        // Only dereference elements that were found FRESH this tick.
        // focused is always fresh (GetFocusedElement runs every tick).
        // selected is only fresh when Strategy 3 actually ran.
        // Tag selection changes as "TabChange" so PostUpdate() can
        // drop them when an inline carousel fires in the same frame.
        if (selectionChanged && selected && selectedIsFresh) {
            BG3A_LOG("[BG3Access]   -> FIRE selection (tab)");

            SubscribeElementINPC(selected);
        } else if (focusChanged && focused) {
            BG3A_LOG("[BG3Access]   -> FIRE focus");

            SubscribeElementINPC(focused);
        } else if (forced) {
            // For forced re-fire, only use pointers that are fresh.
            auto best = (selected && selectedIsFresh) ? selected : focused;
            if (best) {
                BG3A_LOG("[BG3Access]   -> FIRE forced");

                SubscribeElementINPC(best);
                forcedNullCount_ = 0;
            } else if (!useEventDrivenFocus_) {
                // Legacy mode: retry a few times for UI rebuilds
                // (Cross-Play tab), then stop.  In event-driven mode,
                // GotFocus fires when focus returns -- no retries needed.
                forcedNullCount_++;
                if (forcedNullCount_ < 5) {
                    forceNext_ = true;
                }
            }
        }

        // ----- Strategy 4: New widget detection (dialog/overlay) -----
        // Fires whenever a NEW widget is ADDED to the visual tree.
        // Decoupled from focus state -- a dialog can appear while the
        // background menu still has focus (e.g., quit dialog over main menu).
        //
        // "Added" means: widgetSetChanged AND there is at least one widget
        // pointer in the current set that was NOT in the previous set.
        // This avoids firing on widget REMOVAL (menu closing).
        // Allow widgetAdded when focus arrives WITH the new widget
        // (focusChanged + new widget on same tick, e.g. SelectionFlyOut).
        if (widgetSetChanged && widgetCount > 0
            && (hadFocusBefore_ || focusChanged || !suppressTick_)) {
            bool widgetAdded = false;
            if (widgetCount > oldWidgetCount) {
                widgetAdded = true;
            } else {
                // Same count -- check for new addresses OR newly visible
                for (uint32_t i = 0; i < widgetCount; i++) {
                    auto currentAddr = reinterpret_cast<uintptr_t>(widgets[i]);
                    bool foundAddr = false;
                    bool wasVisible = false;
                    for (uint32_t j = 0; j < oldWidgetCount; j++) {
                        if (currentAddr == oldWidgetAddrs[j]) {
                            foundAddr = true;
                            wasVisible = oldWidgetVisible[j];
                            break;
                        }
                    }
                    // New pointer, or was hidden and is now visible
                    if (!foundAddr || (widgetVisible[i] && !wasVisible)) {
                        widgetAdded = true;
                        break;
                    }
                }
            }

            if (widgetAdded) {
                // Widget changes now always go through the settle window
                // at the top of Tick().  By the time we reach here, the tree
                // is stable (post-settle).  Fire for EACH new/newly-visible
                // widget.  widgets[] array contains fresh pointers from THIS tick.
                bool fired = false;
                for (int i = (int)widgetCount - 1; i >= 0; i--) {
                    if (!widgets[i] || !widgetVisible[i]) continue;
                    auto currentAddr = reinterpret_cast<uintptr_t>(widgets[i]);
                    bool isNew = false;
                    bool foundAddr = false;
                    bool wasVisible = false;
                    for (uint32_t j = 0; j < oldWidgetCount; j++) {
                        if (currentAddr == oldWidgetAddrs[j]) {
                            foundAddr = true;
                            wasVisible = oldWidgetVisible[j];
                            break;
                        }
                    }
                    // Fire for new pointers or widgets that just became visible
                    if (!foundAddr || !wasVisible) {
                        isNew = true;
                    }
                    if (isNew) {
                        BG3A_LOG("[BG3Access]   -> FIRE widgetAdded (widget[%d])", i);
                        ExtractWidgetData(
                            static_cast<Noesis::UIElement*>(
                                const_cast<Noesis::Visual*>(widgets[i])),
                            *snapshot);
                        fired = true;
                    }
                }
                if (!fired) {
                    // All addresses swapped -- fire topmost visible as fallback.
                    for (int i = (int)widgetCount - 1; i >= 0; i--) {
                        if (!widgets[i] || !widgetVisible[i]) continue;
                        BG3A_LOG("[BG3Access]   -> FIRE widgetAdded fallback (widget[%d])", i);
                        ExtractWidgetData(
                            static_cast<Noesis::UIElement*>(
                                const_cast<Noesis::Visual*>(widgets[i])),
                            *snapshot);
                        break;
                    }
                }
            }
        }

        // ----- Post-settle: always extract widget data -----
        // After a settle window, grab widget data regardless of whether
        // the widget is "new."  Tab switches within the same widget
        // (e.g., Options tabs) don't change the widget address, but the
        // NameScope content (ListTitle, etc.) does change.  The settle
        // window ensures the tree is stable; just grab everything.
        // Only runs ONCE on the actual post-settle tick, not on every
        // forced tick (forced re-arms when nothing has focus).
        if (postSettle_ && widgetCount > 0) {
            // Collect NameScope texts (ListTitle, etc.) from ALL visible
            // widgets into focusedElement.namedTexts.  The Options menu
            // has 8 widgets and ListTitle lives in the main content widget,
            // not the topmost overlay.  Iterate all of them.
            for (int i = (int)widgetCount - 1; i >= 0; i--) {
                if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                auto widgetElem = static_cast<Noesis::FrameworkElement*>(
                    const_cast<Noesis::Visual*>(widgets[i]));
                // (Post-settle NameScope diagnostic removed)
                TryCollectNamedTexts(widgetElem, snapshot->focusedElement.namedTexts);
            }

            // Subscribe widget DC INPC on the first non-overlay content
            // widget.  Does NOT set widgetAdded -- the INPC handler
            // fires widgetAdded when the DC actually changes, avoiding
            // stale DC reads from widgets the game hasn't swapped yet.
            for (uint32_t i = 0; i < widgetCount; i++) {
                if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                auto widgetElem = static_cast<Noesis::FrameworkElement*>(
                    const_cast<Noesis::Visual*>(widgets[i]));
                auto dataContext = SafeReadDC_SEH(
                    static_cast<Noesis::DependencyObject const*>(widgetElem));
                if (!dataContext) continue;
                auto dcTypeName = SafeBaseObjectTypeName_SEH(dataContext);
                if (!dcTypeName) continue;
                if (IsOverlayDCType(dcTypeName)) continue;
                BG3A_LOG("[BG3Access]   -> Post-settle INPC subscribe (widget[%u] DC=%s)", i, dcTypeName);
                SubscribeWidgetINPC(dataContext, widgetElem);
                break;
            }

        }
        // ----- Per-tick widgetDCTypes collection -----
        // TICK[B]-[E] breadcrumbs removed -- per-tick logging floods the log.
        bool wasPostSettle = postSettle_;
        postSettle_ = false;

        // Track whether we've ever had focus (for Strategy 4 guard).
        if (focused || selected) hadFocusBefore_ = true;

        // ----- Initial widget scan (splash screen, loading text) -----
        // When widgets exist but nothing has focus yet (hadFocusBefore_
        // is false), the normal detection paths are blocked.  Scan
        // visible widgets to extract text from screens that have no
        // focusable elements (e.g., "Press any key to continue").
        //
        // Runs once per STABLE widget set.  When the widget set changes
        // (loading screen -> splash screen), the stability counter resets
        // and we scan the new set after it stabilizes.  Uses a simple
        // fingerprint (widget count + first widget address) to detect
        // whether the set has changed since the last scan.
        // Also scan when focus is lost (in-game loading screens after
        // menus had focus).  The fingerprint prevents re-scanning the
        // same widget set, so this only fires on genuine changes.
        // In event-driven mode, focused/selected are null on skip ticks
        // by design.  The initial scan is for splash/loading screens where
        // nothing has EVER had focus.  Don't run it when we simply skipped
        // detection because no event fired.
        if (!focused && !selected && widgetCount > 0
            && !eventDrivenFocusSkip && !eventDrivenSelectionSkip) {
            // Fingerprint: count + first widget address.
            uintptr_t firstWidgetAddr = reinterpret_cast<uintptr_t>(widgets[0]);
            bool setChanged = (widgetCount != lastScanWidgetCount_
                            || firstWidgetAddr != lastScanFirstAddr_);
            if (setChanged) {
                initialWidgetScanDelay_ = 0;
                lastScanWidgetCount_ = widgetCount;
                lastScanFirstAddr_ = firstWidgetAddr;
            }
            initialWidgetScanDelay_++;
            if (initialWidgetScanDelay_ == 10) {
                BG3A_LOG("[BG3Access] Initial widget scan (%u widgets)", widgetCount);

                for (int i = (int)widgetCount - 1; i >= 0; i--) {
                    if (!widgets[i]) continue;

                    // Log every widget: index, visibility, DC type.
                    bool isVisible = IsVisibleDP(widgets[i]);
                    const char* scanDCType = "(none)";
                    bool isOverlay = false;
                    auto widgetElem = static_cast<Noesis::FrameworkElement*>(
                        const_cast<Noesis::Visual*>(widgets[i]));
                    auto scanDC = SafeReadDC_SEH(
                        static_cast<Noesis::DependencyObject const*>(widgetElem));
                    if (scanDC) {
                        auto typeName = SafeBaseObjectTypeName_SEH(scanDC);
                        if (typeName) {
                            scanDCType = typeName;
                            isOverlay = IsOverlayDCType(scanDCType);
                        }
                    }
                    BG3A_LOG("[BG3Access]   widget[%d] vis=%d DC=%s%s", i,
                        isVisible ? 1 : 0, scanDCType,
                        isOverlay ? " (overlay)" : "");

                    if (!isVisible || isOverlay) continue;

                    // Only set widgetData if Strategy 4 didn't already
                    // fire on this tick.  The initial scan processes ALL
                    // widgets (bottom-up), so the last ExtractWidgetData
                    // call overwrites widgetData with the least interesting
                    // widget (e.g., DCOverheads).  When Strategy 4 already
                    // identified the actual new widget (e.g., shortcutsMenu
                    // with DCGameMenu), preserve that data.
                    if (!snapshot->widgetAdded) {
                        ExtractWidgetData(widgetElem, *snapshot);
                    }
                    TryCollectNamedTexts(widgetElem, snapshot->focusedElement.namedTexts);

                    // Fallback: if NameScope found no text, BFS the visual
                    // tree for TextBlocks inside ControlTemplates (e.g.,
                    // splash screen "Press any key to continue").
                    if (snapshot->focusedElement.namedTexts.empty()) {
                        std::vector<std::string> visualTexts;
                        GatherVisibleTextBlocks(widgetElem, visualTexts);
                        // Use indexed keys (_visualText_1, _visualText_2, ...)
                        // so Lua tables don't silently overwrite duplicate keys.
                        int visualIndex = 0;
                        for (auto& visualText : visualTexts) {
                            BG3A_TRACE("[BG3Access]     Visual text: %s", visualText.c_str());
                            std::string key = "_visualText_" + std::to_string(++visualIndex);
                            snapshot->focusedElement.namedTexts.push_back(
                                {std::move(key), std::move(visualText)});
                        }
                        // Record which widget these visual texts came from
                        // so Lua can route to the correct handler.
                        if (!visualTexts.empty()) {
                            snapshot->visualTextWidgetName =
                                ReadPropertyAsString(widgetElem, "Name");
                        }
                    }
                }
            }
        }

        // Keep polling when NOTHING is focused or selected (give new
        // widgets time to settle and acquire focus).  Cap at 30 ticks.
        //
        // In event-driven mode this poll is DISABLED.  GotFocus and
        // SelectionChanged class handlers signal all focus/selection
        // changes.  Widget additions are detected by Strategy 4 (widget
        // set change).  Polling only adds tree walks that find nothing
        // (30 per scene transition) and can cause hangs during loading.
        //
        // In legacy mode, the poll retries tree walks to discover focus
        // that arrives after the widget set stabilizes.
        if (!useEventDrivenFocus_) {
            bool nothingFound = !focused
                && !(selected && selectedIsFresh);
            if (nothingFound) {
                if (widgetSetChanged) overlayPollCount_ = 0;
                if (overlayPollCount_ < 30) {
                    forceNext_ = true;
                    overlayPollCount_++;
                }
            } else if (focused || (selected && selectedIsFresh)) {
                overlayPollCount_ = 0;
            }
        }

        // ----- Batched INPC: fire at most once per frame -----
        // OnINPCChanged / OnWidgetINPCChanged just set dirty flags.
        // Fire one callback each here, with fresh DC properties.
        //
        // Suppress element INPC on selection-change ticks.  The focus
        // callback (SubscribeElementINPC) already carries the full DC data
        // for the newly selected element.  Firing INPC on the same tick
        // would double-speak the exact same text.
        // Element INPC: the ViewModel notified us that a property changed.
        // Trust the notification and set valueChanged in the snapshot.
        // The delta comparison on dcScalarProps misses sub-object changes
        // (e.g. SelectedItem in comboboxes), so INPC is the reliable trigger.
        if (inpcDirty_) {
            // Don't clear yet -- post-settle processing below may
            // re-trigger INPC.  Clear after dispatch instead.
            // Suppress stray INPC echoes that fire on the same tick as
            // (or 1-2 ticks after) a focus/selection dispatch.  These
            // cause VALUE events that interrupt descriptions the screen
            // reader is still speaking.
            if (!focusChanged && !selectionChanged && inpcCooldown_ <= 0) {
                snapshot->valueChanged = true;
            }
        }

        if (widgetDCDirty_ && callback_ && widgetInpcWidgetAddr_ != 0) {
            // Suppress widget DC echoes that fire 1-2 ticks after a
            // focus/selection change.  Same pattern as element INPC above.
            // Without this, moving between difficulty presets (or any menu
            // where the widget DC updates on focus change) causes the
            // widget handler to re-speak the title, interrupting the
            // preset name+description that the focus handler just spoke.
            if (focusChanged || selectionChanged || inpcCooldown_ > 0) {
                widgetDCDirty_ = false;
            } else {
            widgetDCDirty_ = false;

            // Re-discover the widget from the already-gathered widgets[]
            // array using its stored address.  NEVER dereference stored
            // addresses directly -- match against fresh pointers only.
            Noesis::FrameworkElement* freshWidget = nullptr;
            for (uint32_t widgetIndex = 0; widgetIndex < widgetCount; widgetIndex++) {
                if (reinterpret_cast<uintptr_t>(widgets[widgetIndex])
                    == widgetInpcWidgetAddr_) {
                    freshWidget = static_cast<Noesis::FrameworkElement*>(
                        widgets[widgetIndex]);
                    break;
                }
            }

            // Validate before virtual calls -- element may be stale.
            if (freshWidget && !ProbeUIElement(static_cast<Noesis::UIElement*>(freshWidget))) {
                freshWidget = nullptr;
            }
            if (freshWidget) {
                // Re-read DC from the fresh widget pointer (obtained this frame).
                auto freshDC = SafeReadDC_SEH(
                    static_cast<Noesis::DependencyObject const*>(freshWidget));

                if (freshDC) {
                    auto freshDCTypeName = SafeBaseObjectTypeName_SEH(freshDC);
                    if (!freshDCTypeName) freshDCTypeName = "";

                    // Write widget DC data directly into snapshot.
                    auto widgetDCData = std::make_unique<FocusEventData>();
                    widgetDCData->eventType = "WidgetDCChanged";
                    widgetDCData->dcType = freshDCTypeName;
                    CollectDCProperties(*widgetDCData, freshDC);

                    // Collect namedTexts from the fresh widget.
                    // (INPC namedTexts diagnostic removed)
                    TryCollectNamedTexts(freshWidget, widgetDCData->namedTexts);

                    // Merge into snapshot: namedTexts are appended (not replaced)
                    // so post-settle NameScope data isn't overwritten.
                    // DC props fill in if focusedElement has none.
                    for (auto& namedTextEntry : widgetDCData->namedTexts) {
                        snapshot->focusedElement.namedTexts.push_back(std::move(namedTextEntry));
                    }
                    if (snapshot->focusedElement.dcScalarProps.empty()) {
                        snapshot->focusedElement.dcScalarProps = std::move(widgetDCData->dcScalarProps);
                        snapshot->focusedElement.dcType = widgetDCData->dcType;
                    }

                    // Set widgetAdded so Lua can detect DC changes (e.g.,
                    // DCControllerOptions for interactive controller mode).
                    // This only fires when the DC actually changes (INPC),
                    // not on stale reads.
                    if (!snapshot->widgetAdded) {
                        snapshot->widgetAdded = true;
                        snapshot->widgetEvents.emplace_back();
                        auto& dcChangedEvent = snapshot->widgetEvents.back();
                        dcChangedEvent.eventType = "WidgetDCChanged";
                        dcChangedEvent.dcType = freshDCTypeName;
                        CollectDCProperties(dcChangedEvent, freshDC);
                    }
                }
            }

            // Carousel text-change detection: re-extract tab name using
            // a fresh selected element from this tick's Strategy 3 results.
            // Only possible when selected is a fresh pointer (not stale address).
            if (selected && selectedIsFresh && !selectionChanged && !focusChanged) {
                auto freshText = ExtractTabName(
                    static_cast<Noesis::FrameworkElement*>(selected));
                if (!freshText.empty() && freshText != lastSelectedElemText_) {
                    lastSelectedElemText_ = freshText;
                    BG3A_TRACE("[BG3Access]   -> Carousel text changed: %s", freshText.c_str());
                }
            }
            }  // end else (not suppressed by cooldown)
        } else {
            widgetDCDirty_ = false;
        }

        // ----- Inline carousel detection (event-driven) -----
        // Appearance rows have child ListBoxes (face, skin colour, etc.).
        // Subsequent value changes are captured by ClassSelectionDelegate
        // (it reads the new SelectedItem.Name/ColorName from the event
        // args directly -- no tree walk).  The BFS below runs ONLY on
        // focus change, to capture the row's INITIAL value when the user
        // focuses it.  After that, the event handler updates
        // sInlineCarouselText for every left/right press.
        //
        // Clear previous carousel text on focus change so the new element's
        // value is always detected as a change.  Also clear the dirty
        // flag so any pending SelectionChanged from BEFORE the focus
        // move (captured for the previous element) is discarded.
        if (focusChanged) {
            lastInlineCarouselText_.clear();
            sInlineCarouselDirty = false;
            sInlineCarouselText[0] = '\0';
        }
        if (focused && focusChanged) {
            auto focusedElement = static_cast<Noesis::FrameworkElement*>(focused);
            // BFS for a child TextBlock named "selectionName" (the
            // AppearanceCarousel template's value display).  Also look
            // for ListBox to try SelectedItem.Name as fallback.
            // Max 6 levels deep, 64 nodes to reach through template internals.
            std::vector<Noesis::Visual*> searchQueue(64);
            int searchHead = 0, searchTail = 0;
            searchQueue[searchTail++] = focusedElement;
            Noesis::FrameworkElement* foundSelectionName = nullptr;
            Noesis::FrameworkElement* foundListBox = nullptr;

            // Walk UP first: the focused ContentControl ("base") is
            // a template child of the ListBox.  The downward BFS won't
            // find the ListBox because it's an ancestor, not a descendant.
            {
                auto ancestor = SafeGetVisualParent_SEH(focusedElement);
                for (int upDepth = 0; upDepth < 5 && ancestor; upDepth++) {
                    auto ancestorTypeName = SafeBaseObjectTypeName_SEH(ancestor);
                    if (ancestorTypeName
                        && strstr(ancestorTypeName, "ListBox")
                        && !strstr(ancestorTypeName, "ListBoxItem")) {
                        foundListBox = static_cast<Noesis::FrameworkElement*>(
                            ancestor);
                        break;
                    }
                    ancestor = SafeGetVisualParent_SEH(ancestor);
                }
            }

            while (searchHead < searchTail && searchHead < 64) {
                auto currentNode = searchQueue[searchHead++];
                if (!currentNode) continue;
                // Validate before virtual calls -- element may be stale.
                if (!ProbeUIElement(static_cast<Noesis::UIElement*>(currentNode))) continue;

                auto nodeTypeName = SafeBaseObjectTypeName_SEH(currentNode);

                // Check for TextBlock named "selectionName".  Do NOT
                // break on finding it -- we also need to find the
                // ListBox (which may come later in BFS order) so the
                // SelectedItem.Name read can override the stale-binding
                // TextBlock text.  Continue until both are found or the
                // BFS cap is hit.
                if (!foundSelectionName && nodeTypeName
                    && strstr(nodeTypeName, "TextBlock")) {
                    auto nodeName = ReadPropertyAsString(
                        static_cast<Noesis::FrameworkElement*>(currentNode), "Name");
                    if (nodeName == "selectionName") {
                        foundSelectionName = static_cast<Noesis::FrameworkElement*>(currentNode);
                    }
                }

                // Track ListBox (the source-of-truth for carousel value).
                if (!foundListBox && nodeTypeName
                    && strstr(nodeTypeName, "ListBox")
                    && !strstr(nodeTypeName, "ListBoxItem")) {
                    foundListBox = static_cast<Noesis::FrameworkElement*>(currentNode);
                }

                // Stop once both are found -- no point traversing further.
                if (foundSelectionName && foundListBox) break;

                // Add children to queue -- SEH-guarded.
                auto childCount = SafeGetVisualChildrenCount_SEH(currentNode);
                for (uint32_t childIdx = 0; childIdx < childCount && searchTail < 64; childIdx++) {
                    auto child = SafeGetVisualChild_SEH(currentNode, childIdx);
                    if (child) searchQueue[searchTail++] = child;
                }
            }

            // Read SelectedItem.Name from the ListBox FIRST.  The
            // selectionName TextBlock displays a binding to
            // ListBox.SelectedItem.Name; that binding may not have
            // propagated by the time Tick() runs after an INPC fires,
            // so reading the TextBlock can return a stale value
            // (the previous selection).  ListBox.SelectedItem is the
            // source-of-truth: a direct property access that always
            // reflects the current selection.  Without this order,
            // appearance carousels (Face, Skin Colour, Eye Colour,
            // Tattoo Style/Colour, Genitals) would report the same
            // value across consecutive d-pad presses because the
            // TextBlock binding hadn't caught up yet.
            ReadInlineCarouselState_SEH(
                foundListBox, foundSelectionName,
                lastInlineCarouselText_, lastInlineCarouselColorHex_);
        }

        // (Focus-change clear moved above carousel scan so the value
        // is detected as a change on the same tick as the focus change.)

        // ----- Radial LocalFocus polling (RT shortcuts menu) -----
        // Delegated to PollRadialLocalFocus (SEH-guarded, no C++ destructors).
        // Populates snapshot->radialSlotChanged and related fields.
        //
        // All three polling functions are wrapped in try/catch in
        // addition to their internal SEH.  SEH catches hardware faults
        // (access violations) but NOT C++ exceptions thrown by Noesis
        // SDK internals (FindNodeName, GetVisualChildrenCount, etc.).
        // Uncaught C++ exceptions call std::terminate -> abort(),
        // killing the game.  try/catch(...) catches those.
        //
        // Polling stability gate: defer FindNameInWidgetScoped-based
        // polling until the widget set has been stable for 10 frames.
        // During post-load transitions, widgets appear over several
        // ticks and BFS on partially-constructed widgets can deadlock
        // against the Noesis rendering thread.  The counter resets on
        // every widget change, stays at 10 during normal gameplay.
        if (widgetSetJustChanged) {
            pollStableFrames_ = 0;
        } else if (pollStableFrames_ < 10) {
            pollStableFrames_++;
        }
        bool pollingAllowed = (pollStableFrames_ >= 10);

        if (pollingAllowed && !focused && !selected && widgetCount > 0 && sDataContextProp && sTagProp) {
            try {
                PollRadialLocalFocus(widgets, widgetVisible, widgetCount, snapshot);
            } catch (...) {
                BG3A_LOG("[BG3Access] PollRadialLocalFocus: C++ exception caught");
            }
        }

        // ----- ActiveSearch LocalFocus polling (hold-A item list) -----
        if (pollingAllowed && !focused && !selected && widgetCount > 0 && sDataContextProp
            && !snapshot->radialSlotChanged) {
            try {
                PollActiveSearchLocalFocus(widgets, widgetVisible, widgetCount, snapshot);
            } catch (...) {
                BG3A_LOG("[BG3Access] PollActiveSearchLocalFocus: C++ exception caught");
            }
        }

        // ----- Context menu polling (WorldContextMenu popup) -----
        if (pollingAllowed && widgetCount > 0 && !snapshot->focusChanged) {
            try {
                PollContextMenu(widgets, widgetVisible, widgetCount,
                                focused, cachedTrueRoot_, cachedContentChild_,
                                snapshot);
            } catch (...) {
                BG3A_LOG("[BG3Access] PollContextMenu: C++ exception caught");
            }
        }

        // ----- Tooltip polling (gated on event-driven presence flag) -----
        // ToolTip.Opened/Closed handlers set sToolTipIsOpen.  PollTooltip
        // ONLY runs while a tooltip is currently visible -- when no
        // tooltip is open, zero per-tick BFS happens.  When a tooltip
        // opens, polling kicks in and PollTooltip's stabilization logic
        // handles the binding-lag (waits for content to settle before
        // dispatching).  When the tooltip closes, polling stops.
        //
        // Net: per-tick BFS only during the brief window a tooltip is
        // actually on screen.  Eliminates wasteful walks during normal
        // navigation.
        if (focusChanged) {
            sTooltipFingerprintReset = true;
        }
        if (sToolTipIsOpen
            && hadFocusBefore_ && cachedTrueRoot_ && cachedContentChild_) {
            PollTooltip(cachedTrueRoot_, cachedContentChild_, snapshot);
        }

        // Tooltip-close edge: ToolTip.Closed flipped sToolTipIsOpen to
        // false since the previous tick.  Emit a single-shot
        // tooltipChanged event with no texts so Lua can invalidate any
        // tooltip-derived state (compare stash, inspect cache, etc.).
        // Content-arrival events are distinguished by non-empty
        // tooltipTexts.
        if (lastTooltipOpen_ && !sToolTipIsOpen) {
            snapshot->tooltipChanged = true;
            snapshot->tooltipTexts.clear();
            BG3A_TRACE("[BG3Access] TOOLTIP: close event emitted to Lua");
        }
        lastTooltipOpen_ = sToolTipIsOpen;

        // ----- HotBar action radial: Tag polling on visible widgets -----
        // The XAML sets ActionRadials.Tag = LocalFocus.DataContext whenever
        // the radial pointer moves to a different slot.  Poll each visible
        // widget for a non-null Tag DP.  When the tag changes and is a
        // VMHotBarSlot, populate snapshot with Content.Name (action title).
        // Skip if PollRadialLocalFocus already set radialSlotChanged (RT menu).
        if (sTagProp && sDataContextProp && widgetCount > 0 && !snapshot->radialSlotChanged) {
            bool foundHotBarWidget = false;
            bool hotBarHasTag = false;
            for (uint32_t widgetIdx = 0; widgetIdx < widgetCount; widgetIdx++) {
                if (!widgets[widgetIdx] || !widgetVisible[widgetIdx]) continue;
                auto widgetElement = static_cast<Noesis::FrameworkElement*>(
                    const_cast<Noesis::Visual*>(widgets[widgetIdx]));
                auto widgetDepObj = static_cast<Noesis::DependencyObject const*>(widgetElement);

                // Only process the DCHotBar widget (ActionRadials).
                // Other widgets may have Tags for unrelated purposes.
                auto widgetDC = SafeReadDC_SEH(widgetDepObj);
                if (!widgetDC) continue;
                auto widgetDCTypeName = SafeBaseObjectTypeName_SEH(widgetDC);
                if (!widgetDCTypeName || !strstr(widgetDCTypeName, "DCHotBar")) continue;
                foundHotBarWidget = true;

                // Read Tag property from the HotBar widget.
                auto tagVal = SafeGetDPValue_SEH(sTagProp, widgetDepObj);
                if (!tagVal) { break; }
                auto tagObject = SafeDerefDPObject_SEH(tagVal);
                if (!tagObject) { break; }
                hotBarHasTag = true;
                auto tagAddr = reinterpret_cast<uintptr_t>(tagObject);

                if (tagAddr != lastRadialTagAddr_) {
                    lastRadialTagAddr_ = tagAddr;

                    // Tag holds a ViewModel (BaseObject), NOT a UIElement.
                    // Use SafeBaseObjectTypeName_SEH instead of ProbeUIElement.
                    auto tagTypeName = SafeBaseObjectTypeName_SEH(tagObject);
                    if (!tagTypeName) continue;

                    // HotBar action radial: Tag is VMHotBarSlot.
                    // C++ extracts the slot's identifying properties (Name,
                    // StatsId, SpellId, etc.) and passes them to Lua.
                    // Lua uses Ext.Stats APIs for descriptions (API-first).
                    if (tagTypeName && strstr(tagTypeName, "HotBarSlot")) {
                        FocusEventData tagData;
                        CollectDCProperties(tagData, tagObject);

                        // Extract Content sub-object properties: Name for
                        // the display title, plus any identifier (StatsId,
                        // SpellId, OriginatorId) that Lua needs for API lookup.
                        std::string actionName;
                        std::string actionTag;
                        for (auto const& subObj : tagData.dcObjectProps) {
                            if (subObj.propName == "Content") {
                                for (auto const& prop : subObj.props) {
                                    if (prop.first == "Name" && actionName.empty()) {
                                        actionName = prop.second;
                                    }
                                }
                                // Build a tag string with ALL Content props
                                // so Lua can pick the right API identifier.
                                // Format: "key1=val1;key2=val2;..."
                                for (auto const& prop : subObj.props) {
                                    if (!prop.second.empty()) {
                                        if (!actionTag.empty()) actionTag += ";";
                                        actionTag += prop.first;
                                        actionTag += "=";
                                        actionTag += prop.second;
                                    }
                                }
                                break;
                            }
                        }

                        if (!actionName.empty()) {
                            snapshot->radialSlotChanged = true;
                            snapshot->radialSlotType = "HotBar";
                            snapshot->radialTitleText = std::move(actionName);
                            // Description left empty -- Lua resolves via API.
                            snapshot->radialDescriptionText.clear();
                            snapshot->radialSlotTag = std::move(actionTag);

                            BG3A_TRACE("[BG3Access] HOTBAR RADIAL: title=%s tag=%s",
                                 snapshot->radialTitleText.c_str(),
                                 snapshot->radialSlotTag.c_str());
                        }
                    }
                }
            }
            // When the HotBar widget exists but its Tag is null (stick at
            // center rest), reset the tracker so returning to the same
            // slot is detected as a new change.  XAML sets Tag = null
            // when LocalFocus goes null (LocalFocusChanged event fires
            // with LocalFocus.DataContext = null on center rest).
            if (foundHotBarWidget && !hotBarHasTag && lastRadialTagAddr_ != 0) {
                lastRadialTagAddr_ = 0;
            }
            // When the HotBar widget is entirely gone (radial closed
            // and widget removed), reset the tracker so the first
            // slot on the next open fires a fresh radialSlotChanged
            // event.  Without this, if the game reuses the same VM
            // object pointer on reopen, the `tagAddr != lastRadialTagAddr_`
            // check in the change-detection block above would fail and
            // the initial slot would be silent.
            if (!foundHotBarWidget && lastRadialTagAddr_ != 0) {
                lastRadialTagAddr_ = 0;
            }
        }

        // ===== SNAPSHOT FINALIZATION =====
        // The snapshot has been populated throughout Tick().
        // Finalize change flags, run delta comparison, and dispatch.
        {
            // Change flags from detection above
            snapshot->focusChanged = focusChanged;
            snapshot->selectionChanged = selectionChanged || snapshot->selectionChanged;

            // Focused element data: ONLY use elements obtained THIS frame.
            // In event-driven mode, focused/selected may be null on skip ticks.
            // When INPC fires on a skip tick, do a targeted tree walk to get
            // the current focused element for data extraction.  This only
            // happens when the user actually changes a value (rare), not per-frame.
            Noesis::UIElement* snapshotElement = focused;
            if (!snapshotElement && selected && selectedIsFresh) {
                snapshotElement = selected;
            }
            if (!snapshotElement && useEventDrivenFocus_ && snapshot->valueChanged) {
                // Try the cached "last focused element" pointer first.
                // GotFocusDelegate stores it in sLastFocusedElement
                // (persistent across ticks; sGotFocusSourceElement is
                // the per-event signal, consumed and cleared earlier
                // in Tick).  On subsequent INPC ticks (skip ticks, no
                // new GotFocus), the cached pointer is still valid
                // because the user hasn't moved focus -- so we use it
                // directly without walking the tree.  ProbeUIElement
                // guards against staleness (carousel template recycling
                // destroyed the element); if stale, fall back to the
                // walks below.
                if (sLastFocusedElement) {
                    auto cachedElem = static_cast<Noesis::UIElement*>(
                        sLastFocusedElement);
                    if (ProbeUIElement(cachedElem)) {
                        snapshotElement = cachedElem;
                    }
                }
                // Stale cache (or never set): fall back to tree walks.
                // Strategy 1 (FocusManager DP reads), then Strategy 2
                // (IsFocused tree walk -- character sheet inventory uses
                // ls:MoveFocus.IsFocused which FocusManager doesn't see).
                if (!snapshotElement) {
                    Noesis::DependencyObject* inpcScopeRoot = nullptr;
                    snapshotElement = FindFocusedElement_SEH(
                        widgets, widgetVisible, widgetCount,
                        root, kMaxTreeDepth, &inpcScopeRoot);
                    if (!snapshotElement) {
                        snapshotElement = FindIsFocused_SEH(
                            widgets, widgetVisible, widgetCount,
                            root, kMaxTreeDepth, &inpcScopeRoot);
                    }
                    BG3A_LOG("[BG3Access] INPC tree walk for value extraction "
                         "(cache stale): elem=%p", snapshotElement);
                }
            }
            if (snapshotElement) {
                ExtractElementData(snapshot->focusedElement,
                    static_cast<Noesis::FrameworkElement*>(snapshotElement));
            }

            // Selected element: when Strategy 3 found a ListBoxItem that
            // differs from the focused element, send its data too.
            // Lua needs both: focused for item text, selected for section
            // label DC (VMSelectableRace, VMSelectableClass, etc.).
            if (selected && selectedIsFresh && selected != snapshotElement) {
                ExtractElementData(snapshot->selectedElementData,
                    static_cast<Noesis::FrameworkElement*>(selected));
            }

            // Consume event-driven inline carousel updates: the
            // ClassSelectionDelegate captured the new SelectedItem name
            // into sInlineCarouselText when the user changed an inline
            // carousel.  Copy to lastInlineCarouselText_ so the snapshot
            // sees the fresh value.  Cleared after consumption.
            if (sInlineCarouselDirty) {
                lastInlineCarouselText_ = sInlineCarouselText;
                lastInlineCarouselColorHex_ = sInlineCarouselColorHex;
                sInlineCarouselDirty = false;
                // Don't set valueChanged here -- INPC fires on the
                // next tick for the same change and sets it naturally.
                // Setting it here causes a double-fire (two value
                // snapshots per press).  The carousel text and hex are
                // ready for when the INPC tick arrives.
            } else if (lastInlineCarouselColorHex_.empty()
                       && sInlineCarouselSourceListBox
                       && !lastInlineCarouselText_.empty()) {
                // Deferred hex retry: brush binding hadn't resolved at
                // event time.  Re-read SelectedItem from the source
                // ListBox (live pointer, safe across ticks) and retry
                // the brush capture.  Own SEH so a fault here doesn't
                // kill the entire tick.
                RetryCarouselColorHex_SEH(sInlineCarouselSourceListBox);
                if (sInlineCarouselColorHex[0]) {
                    lastInlineCarouselColorHex_ =
                        sInlineCarouselColorHex;
                }
                sInlineCarouselSourceListBox = nullptr;
            }

            // Inline carousel value: always available (current carousel
            // state) regardless of whether it changed this tick.
            snapshot->inlineCarouselValue = lastInlineCarouselText_;
            snapshot->inlineCarouselColorHex = lastInlineCarouselColorHex_;
            snapshot->inlineCarouselChanged =
                !lastInlineCarouselText_.empty() &&
                lastInlineCarouselText_ != previousSnapshotCarousel_;

            // Value changes are detected solely through INPC (inpcDirty_).
            // A redundant dcScalarProps diff was here previously but it
            // caused duplicate VALUE dispatches that interrupted speech.
            // INPC is the single source of truth for value changes.

            // Expander toggle detection: if isChecked changed on the same
            // element (no focus change), the user pressed A to expand/collapse.
            // Set valueChanged so Lua re-speaks the header with updated state.
            if (!focusChanged && snapshot->focusedElement.isChecked >= 0
                && snapshot->focusedElement.isChecked != lastDispatchedIsChecked_) {
                snapshot->valueChanged = true;
            }

            // Widget, widgetDC, and namedTexts data were written directly
            // into snapshot by ExtractWidgetData() and the widgetDCDirty
            // handler above.  No accumulator reads needed.

            // Commit pending INPC subscription immediately.
            // The old settleFramesRemaining_ mechanism delayed this for
            // 3 frames after tab changes, but also suppressed focus
            // dispatches (lobby focus on multiplayer entry).  The 6-frame
            // tree settle window already handles timing; no second settle.
            // Use snapshotElement which may include the INPC tree-walk result.
            if (pendingINPCSubscription_) {
                auto inpcTarget = snapshotElement
                    ? snapshotElement
                    : (focused ? focused : selected);
                CommitINPCSubscription(inpcTarget);
            }

            // Proactive INPC re-subscription: if the focused element's
            // DC changed from what we last subscribed to (carousel
            // recycling: same element, swapped ViewModel), re-subscribe
            // without waiting for a snapshot dispatch.  This ensures the
            // INPC subscription tracks the CURRENT DC even when the
            // change detection path hasn't triggered a full dispatch yet.
            if (focused && focusedDCAddr != 0
                && focusedDCAddr != inpcSubscribedDCAddr_) {
                CommitINPCSubscription(focused);
            }

            // Delta comparison: has anything meaningful changed?
            bool shouldDispatch = false;
            if (snapshot->focusChanged) {
                shouldDispatch = true;
            }
            if (snapshot->inlineCarouselChanged) {
                shouldDispatch = true;
            }
            if (snapshot->valueChanged && !snapshot->focusChanged) {
                shouldDispatch = true;
            }
            if (snapshot->selectionChanged) {
                shouldDispatch = true;
            }
            if (snapshot->widgetAdded) {
                shouldDispatch = true;
            }
            if (snapshot->radialSlotChanged) {
                shouldDispatch = true;
            }
            if (snapshot->contextMenuChanged) {
                shouldDispatch = true;
            }
            if (snapshot->tooltipChanged) {
                shouldDispatch = true;
            }
            if (!snapshot->focusedElement.namedTexts.empty()) {
                shouldDispatch = true;
            }

            // After settle, wait for focus to arrive before dispatching.
            // On menus like multiplayer, the tab selection settles first
            // but focus on the first item takes one extra tick.  Without
            // this, the screen entry dispatches without the focused item,
            // then the focused item dispatches separately and interrupts.
            // When focus arrives, selectionChanged is re-detected (lastSelectedAddr_
            // was wiped) so the snapshot contains BOTH sel=1 and focus=1.
            // If a SelectionChanged event fired DURING this tick (carousel
            // recycling, binding updates), the tree isn't truly stable yet.
            // Suppress this dispatch and re-arm settle so the next post-settle
            // tick gets complete data (e.g. resolved ListTitle bindings).
            if (shouldDispatch && sSelectionDirtyFlag) {
                BG3A_LOG("[BG3Access] SelectionChanged during tick -- re-arming settle, suppressing dispatch");
                sSelectionDirtyFlag = false;
                sSelectionChangedItemAddr = 0;
                settling_ = true;
                settleStableCount_ = 0;
                settleTotalCount_ = 0;
                return;
            }

            // Dispatch snapshot to Lua
            if (shouldDispatch && callback_) {
                // Count namedTexts in focusedElement and across widgetEvents.
                int focusNamedCount = static_cast<int>(
                    snapshot->focusedElement.namedTexts.size());
                int widgetNamedCount = 0;
                for (auto const& widgetEvent : snapshot->widgetEvents) {
                    widgetNamedCount += static_cast<int>(
                        widgetEvent.namedTexts.size());
                }

                BG3A_TRACE("[BG3Access] SNAPSHOT: focus=%d sel=%d val=%d carousel=%d "
                     "widget=%d(%d) postSettle=%d elemId=%s dcType=%s "
                     "focusNT=%d widgetNT=%d carVal=%s",
                     snapshot->focusChanged, snapshot->selectionChanged,
                     snapshot->valueChanged, snapshot->inlineCarouselChanged,
                     snapshot->widgetAdded,
                     static_cast<int>(snapshot->widgetEvents.size()),
                     wasPostSettle ? 1 : 0,
                     snapshot->focusedElement.elemId.c_str(),
                     snapshot->focusedElement.dcType.empty()
                         ? "(none)" : snapshot->focusedElement.dcType.c_str(),
                     focusNamedCount, widgetNamedCount,
                     snapshot->inlineCarouselValue.c_str());

                // Log namedTexts keys+values so we can see what data arrived.
                for (auto const& pair : snapshot->focusedElement.namedTexts) {
                    BG3A_TRACE("[BG3Access]   focusNT: %s = %s",
                         pair.first.c_str(), pair.second.c_str());
                }
                for (auto const& widgetEvent : snapshot->widgetEvents) {
                    for (auto const& pair : widgetEvent.namedTexts) {
                        BG3A_TRACE("[BG3Access]   widgetNT: %s = %s",
                             pair.first.c_str(), pair.second.c_str());
                    }
                }

                // Update delta cache
                previousSnapshotElemId_ = snapshot->focusedElement.elemId;
                previousSnapshotCarousel_ = snapshot->inlineCarouselValue;
                previousSnapshotDCProps_ = snapshot->focusedElement.dcScalarProps;
                lastDispatchedIsChecked_ = snapshot->focusedElement.isChecked;

                // Set INPC cooldown to suppress stray echoes for 2 ticks
                // after a REAL focus/selection change (different element
                // pointer).  Don't set cooldown on re-dispatches of the
                // same element -- that kills INPC during carousel cycling
                // where the element pointer stays the same but its
                // displayed content changes via DC property updates.
                // Compare by element address, not elemId string (elemId
                // includes elemText which changes with carousel content).
                if ((snapshot->focusChanged || snapshot->selectionChanged)
                    && reinterpret_cast<uintptr_t>(snapshotElement)
                        != lastDispatchedElemAddr_) {
                    inpcCooldown_ = 2;
                }
                lastDispatchedElemAddr_ =
                    reinterpret_cast<uintptr_t>(snapshotElement);

                // Clear INPC dirty flags AFTER dispatch so any
                // re-triggering during post-settle NameScope walks
                // is consumed rather than causing a second dispatch.
                inpcDirty_ = false;
                widgetDCDirty_ = false;

                ContextGuardAnyThread ctx(ContextType::Client);
                if (gExtender->GetClient().HasExtensionState()) {
                    LuaClientPin pin(gExtender->GetClient().GetExtensionState());
                    if (pin) {
                        pin->GetDeferredUIEvents().OnTickSnapshot(
                            callback_, std::move(*snapshotPtr));
                    }
                }
            } else {
                // Even when not dispatching, clear INPC flags to prevent
                // stale dirty state from firing on the next tick.
                inpcDirty_ = false;
                widgetDCDirty_ = false;
            }

        }
    }

    void Reset()
    {
        lastFocusedAddr_ = 0;
        lastSelectedAddr_ = 0;
        lastSelectedDCAddr_ = 0;
        pendingINPCSubscription_ = false;
        lastSelectedElemText_.clear();
        lastInlineCarouselText_.clear();
        cachedRootAddr_ = 0;
        widgetContainer_ = nullptr;
        forcedNullCount_ = 0;
        prevWidgetCount_ = 0;
        hadFocusBefore_ = false;
        lastTooltipOpen_ = false;

        // NOTE: scan tracking fields (initialWidgetScanDelay_,
        // lastScanWidgetCount_, lastScanFirstAddr_) intentionally NOT
        // reset here.  GameStateChanged fires multiple times during load
        // and resetting causes the same tip to repeat on each transition.
        initialSelectionDone_ = false;
        selectionFiredDuringSettle_ = false;
        sSelectionDirtyFlag = false;
        sGotFocusDirtyFlag = false;
        sGotFocusSourceElement = nullptr;
        sLastFocusedElement = nullptr;
        sLastFocusedWidget = nullptr;
        sClassSelectionItem = nullptr;
        sClassSelectionDCAddr = 0;
        inpcDirty_ = false;
        widgetDCDirty_ = false;
        widgetInpcDCAddr_ = 0;
        widgetInpcWidgetAddr_ = 0;
        inpcSubscribedDCAddr_ = 0;
        cachedWidgetDCTypes_.clear();
        cachedWidgetAddrs_.clear();
        cachedWidgetNames_.clear();
        pollStableFrames_ = 0;
        settling_ = false;
        settleStableCount_ = 0;
        settleTotalCount_ = 0;
        settleBaselineWidgetCount_ = 0;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgetAddrs_[i] = 0;
            prevWidgetVisible_[i] = false;
            settleBaselineWidgetAddrs_[i] = 0;
            settleBaselineWidgetVisible_[i] = false;
        }
    }

private:
    // Subscribe INPC on the focused element's DataContext and the
    // widget's DataContext.  Snapshot handles all data extraction and
    // dispatch -- this function only manages subscriptions.
    void SubscribeElementINPC(Noesis::UIElement* elem)
    {
        if (!elem) return;
        // Don't dereference the element here -- during tab switches it
        // may be in a half-destroyed state.  Just set a flag so Tick()
        // re-discovers the focused element and subscribes INPC on a
        // fresh pointer after the UI has settled.
        pendingINPCSubscription_ = true;
        BG3A_TRACE("[BG3Access]   SubscribeElementINPC: deferred (pending re-discovery)");
    }

    // Re-discover the focused element and subscribe INPC on it.
    // Called from Tick() after settling, using fresh pointers from
    // the current frame's focus strategies.
    void CommitINPCSubscription(Noesis::UIElement* freshElement)
    {
        pendingINPCSubscription_ = false;
        if (!freshElement) return;

        BG3A_TRACE("[BG3Access]   CommitINPCSubscription: elem=%p", freshElement);
        auto frameworkElem = static_cast<Noesis::FrameworkElement*>(freshElement);

        // No UnsubscribeINPC needed -- fire-and-forget pattern.
        // Previous subscription's delegate just sets inpcDirty_ (harmless).
        auto dataContext = SafeReadDC_SEH(
            static_cast<Noesis::DependencyObject const*>(frameworkElem));
        if (dataContext) {
            SubscribeINPC(dataContext);
            inpcSubscribedDCAddr_ = reinterpret_cast<uintptr_t>(dataContext);
        }
    }

public:
    // -----------------------------------------------------------------
    // CollectLoadingHints: targeted extraction of loading tip text from
    // the LoadingHints ItemsControl inside an ls.LoadingScreen widget.
    //
    // The XAML structure is:
    //   widget root -> ... -> ItemsControl "LoadingHints"
    //     -> ItemsPresenter -> Grid -> ContentPresenter -> TextBlock
    //
    // Each TextBlock's Inlines contain the resolved translation text
    // (populated by CtxTransStringRunGeneratorBehavior at template time).
    // TextBlocks have Opacity=0 after loading but their text persists.
    //
    // Uses BFS to find the ItemsControl by Name, then walks its visual
    // children to find TextBlocks.  All operations use SEH helpers.
    // -----------------------------------------------------------------

    // Inner: uses std::string from ReadTextBlockText (has destructor).
    static void CollectLoadingHints_Inner(
        Noesis::FrameworkElement* widgetRoot,
        std::vector<std::pair<std::string, std::string>>& outTexts)
    {
        // BFS to find "LoadingHints" ItemsControl (within 6 levels).
        Noesis::Visual* queue[128];
        int queueFront = 0, queueBack = 0;
        queue[queueBack++] = widgetRoot;

        Noesis::FrameworkElement* hintsControl = nullptr;
        int nodesVisited = 0;

        while (queueFront < queueBack && nodesVisited < 128) {
            auto node = queue[queueFront++];
            nodesVisited++;
            if (!node) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(node)))
                continue;

            // Check Name property for "LoadingHints".
            auto nodeFramework = static_cast<Noesis::FrameworkElement*>(node);
            auto nodeName = ReadPropertyAsString(nodeFramework, "Name");
            if (nodeName == "LoadingHints") {
                hintsControl = nodeFramework;
                break;
            }

            // Enqueue children (bounded depth via node limit).
            auto childCount = SafeGetVisualChildrenCount_SEH(node);
            for (uint32_t childIndex = 0;
                 childIndex < childCount && queueBack < 128;
                 childIndex++) {
                auto child = SafeGetVisualChild_SEH(node, childIndex);
                if (child) queue[queueBack++] = child;
            }
        }

        if (!hintsControl) {
            BG3A_TRACE("[BG3Access] CollectLoadingHints: LoadingHints element not found");
            return;
        }

        BG3A_TRACE("[BG3Access] CollectLoadingHints: found LoadingHints at %p",
                 hintsControl);

        // Walk the ItemsControl's visual subtree for TextBlocks.
        // The structure is: ItemsPresenter -> Grid -> ContentPresenter -> TextBlock.
        // BFS again, bounded to 64 nodes (ItemsControl subtree is small).
        Noesis::Visual* textQueue[64];
        int textFront = 0, textBack = 0;
        textQueue[textBack++] = hintsControl;

        int hintIndex = 0;
        while (textFront < textBack && hintIndex < 20) {
            auto textNode = textQueue[textFront++];
            if (!textNode) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(textNode)))
                continue;

            auto typeName = SafeBaseObjectTypeName_SEH(textNode);
            if (typeName && strstr(typeName, "TextBlock")) {
                auto text = ReadTextBlockText(
                    static_cast<Noesis::FrameworkElement*>(textNode));
                if (!text.empty()
                    && text.find("[ForceUpdate]") == std::string::npos
                    && text.find("s_HandleUnknown") == std::string::npos) {
                    hintIndex++;
                    std::string key = "_loadingHint_"
                        + std::to_string(hintIndex);
                    outTexts.push_back(
                        {std::move(key), std::move(text)});
                    BG3A_TRACE("[BG3Access]   Loading hint %d: %s",
                             hintIndex,
                             outTexts.back().second.c_str());
                }
                continue;  // Don't recurse into TextBlock children.
            }

            auto childCount = SafeGetVisualChildrenCount_SEH(textNode);
            for (uint32_t childIndex = 0;
                 childIndex < childCount && textBack < 64;
                 childIndex++) {
                auto child = SafeGetVisualChild_SEH(textNode, childIndex);
                if (child) textQueue[textBack++] = child;
            }
        }

        BG3A_TRACE("[BG3Access] CollectLoadingHints: found %d hints", hintIndex);
    }

    // SEH wrapper: outTexts reference is a pointer (no destructor in
    // this frame).  Inner function's std::string locals are abandoned
    // on fault (minor leak, non-fatal).
    static void CollectLoadingHints(
        Noesis::FrameworkElement* widgetRoot,
        std::vector<std::pair<std::string, std::string>>& outTexts)
    {
        __try {
            CollectLoadingHints_Inner(widgetRoot, outTexts);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            BG3A_LOG("[BG3Access] CollectLoadingHints: SEH fault");
        }
    }

    // Fire a WIDGET event: extract data from the widget and push as a
    // data table (no Noesis elements cross to Lua).  Phase 3 replacement
    // for the old OnPropertyChanged path.
    void ExtractWidgetData(Noesis::UIElement* elem, ecl::lua::TickSnapshot& snapshot)
    {
        if (!elem) return;
        snapshot.widgetAdded = true;

        // Push a new entry for this widget.  Each widget that fires
        // this tick gets its own complete data set -- no last-wins.
        snapshot.widgetEvents.emplace_back();
        auto& data = snapshot.widgetEvents.back();
        data.eventType = "WidgetAdded";

        auto frameworkElem = static_cast<Noesis::FrameworkElement*>(elem);
        auto classType = SafeGetClassType_SEH(frameworkElem);
        if (!classType) {
            BG3A_TRACE("[BG3Access]   FireWidgetCallback: GetClassType returned null for %p, skipping", elem);
            return;
        }
        auto elemTypeName = SafeBaseObjectTypeName_SEH(frameworkElem);
        data.elemType = elemTypeName ? elemTypeName : "Unknown";
        data.elemName = ReadPropertyAsString(frameworkElem, "Name");
        BG3A_TRACE("[BG3Access]   FireWidgetCallback: elem=%p type=%s name=%s",
             elem, data.elemType.c_str(), data.elemName.c_str());

        // Widget root ID (the widget itself IS the root for widget-added)
        char ptrBuf[32];
        snprintf(ptrBuf, sizeof(ptrBuf), "%p", static_cast<void*>(elem));
        data.widgetRootId = ptrBuf;

        // Collect ALL DC properties from the widget's DataContext.
        {
            auto dataContext = SafeReadDC_SEH(
                static_cast<Noesis::DependencyObject const*>(frameworkElem));
            if (dataContext) {
                auto dcTypeName = SafeBaseObjectTypeName_SEH(dataContext);
                BG3A_TRACE("[BG3Access]   FireWidgetCallback: DC=%s",
                     dcTypeName ? dcTypeName : "(null)");
                if (dcTypeName) {
                    data.dcType = dcTypeName;
                    // Push into widgetDCTypes / widgetAddrs /
                    // widgetNames in lockstep via SEH-wrapped helper
                    // so a fault here stays scoped to the push and
                    // does not kill the whole Tick frame.  The
                    // cached scan may have missed this widget if it
                    // just became visible on this tick.  No dedup --
                    // two widgets with the same DC at different
                    // addresses are distinct entities.  data.elemName
                    // was populated above (line ~3488) via the
                    // SEH-wrapped ReadPropertyAsString.
                    PushWidgetDCTypeAndAddr_SEH(
                        snapshot.widgetDCTypes, snapshot.widgetAddrs,
                        snapshot.widgetNames,
                        dcTypeName, ptrBuf, data.elemName.c_str());

                    BG3A_TRACE("[BG3Access]   EWD[1] CollectDCProperties");
                    CollectDCProperties(data, dataContext);
                    BG3A_TRACE("[BG3Access]   EWD[2] TryCollectSelectionFlyOutTitle");
                    TryCollectSelectionFlyOutTitle(data, dataContext);
                    BG3A_TRACE("[BG3Access]   EWD[3] TryCollectFinalResult");
                    TryCollectFinalResult(data, dataContext);

                    // Subscribe widget DC INPC for property change tracking.
                    BG3A_TRACE("[BG3Access]   EWD[4] SubscribeWidgetINPC");
                    SubscribeWidgetINPC(dataContext, frameworkElem);
                }
            }
        }

        BG3A_TRACE("[BG3Access]   EWD[5] ExtractBindingInfo");
        ExtractBindingInfo(data, frameworkElem);

        // Collect named TextBlock texts from the widget via NameScope lookup.
        BG3A_TRACE("[BG3Access]   EWD[6] TryCollectNamedTexts");
        TryCollectNamedTexts(frameworkElem, data.namedTexts);
        BG3A_TRACE("[BG3Access]   EWD[7] done");

        // Loading hints are handled exclusively by the buffer path:
        // BufferLoadingTips_SEH (safe-state polling) and
        // CaptureLoadingHintFromINPC (INPC on VisibileHintIndex).
        // Do NOT call CollectLoadingHints here -- ExtractWidgetData
        // fires for the loading screen widget on every phase of the
        // multi-phase boot sequence and on post-load Idle transitions,
        // reading stale TextBlock Inlines that persist at Opacity=0.
    }

    // ----- Auto-INPC subscription (Phase 2) -----
    // Subscribes to INotifyPropertyChanged on a DataContext ViewModel.
    // Fire-and-forget: subscribe the delegate, then forget the pointer.
    // NEVER call Remove() -- the old object may already be dead.  If the
    // VM is destroyed, Noesis cleans up its delegate list automatically.
    // Our delegate callback (OnINPCChanged) just sets a dirty flag --
    // harmless if it fires during destruction.
    void SubscribeINPC(Noesis::BaseComponent* dataContext)
    {
        auto notifies = SafeDynamicCastINPC_SEH(dataContext);
        if (!notifies) return;

        // Fire-and-forget: subscribe and let Noesis manage the lifetime.
        // The delegate may fire redundantly if we subscribe twice on the
        // same object, but that just sets inpcDirty_ = true again (idempotent).
        SafeSubscribeINPC_SEH(notifies, this,
            &GlobalFocusMonitor::OnINPCChanged);
    }

    // UnsubscribeINPC removed -- fire-and-forget pattern means there is
    // nothing to unsubscribe.  The delegate fires harmlessly (sets a flag).
    // Noesis cleans up delegate lists when objects are destroyed.

    void OnINPCChanged(Noesis::BaseComponent* sender,
                       const Noesis::PropertyChangedEventArgs& args)
    {
        // Batch per frame: just set dirty flag.  The tick fires ONE
        // PropertyChanged event instead of one per property change.
        inpcDirty_ = true;
    }

    // ----- Widget DC INPC subscription (Phase 3) -----
    // Monitors the WIDGET's DataContext for property changes.  Fire-and-forget:
    // subscribe the delegate, store address for duplicate avoidance and
    // re-discovery, then never call Remove().  Noesis manages cleanup.
    void SubscribeWidgetINPC(Noesis::BaseComponent* dataContext,
                             Noesis::FrameworkElement* widgetElem = nullptr)
    {
        auto dcAddr = reinterpret_cast<uintptr_t>(dataContext);

        // Skip if same DC object already subscribed (address comparison).
        if (dcAddr == widgetInpcDCAddr_ && dcAddr != 0)
            return;

        auto notifies = SafeDynamicCastINPC_SEH(dataContext);
        if (!notifies) return;

        // Fire-and-forget: subscribe and store addresses for re-discovery.
        // Previous subscription's delegate may still fire -- that just sets
        // widgetDCDirty_ = true (idempotent).
        widgetInpcDCAddr_ = dcAddr;
        widgetInpcWidgetAddr_ = reinterpret_cast<uintptr_t>(widgetElem);

        SafeSubscribeINPC_SEH(notifies, this,
            &GlobalFocusMonitor::OnWidgetINPCChanged);
    }

    // UnsubscribeWidgetINPC removed -- fire-and-forget pattern.
    // Noesis cleans up delegate lists when objects are destroyed.

    void OnWidgetINPCChanged(Noesis::BaseComponent* sender,
                              const Noesis::PropertyChangedEventArgs& args)
    {
        // Batch per frame: just set dirty flag.  The tick fires ONE
        // WidgetDCChanged event instead of one per property change.
        widgetDCDirty_ = true;

        // Loading tip capture: when VisibileHintIndex changes on an
        // ls.LoadingScreen DC, set a flag so BufferLoadingTips_SEH
        // (which runs only during safe states) reads the tip text.
        //
        // Do NOT walk the tree here.  This INPC callback fires from
        // Noesis's property change processing, which can happen during
        // dangerous states (LoadLevel, SwapLevel) when the loading
        // thread is active.  GetRoot() / FindWidgetContainer() /
        // GetVisualChild() can deadlock against the loading thread.
        // SEH cannot catch deadlocks.
        if (!tipsAlreadyDelivered_
            && args.propertyName.Str()
            && strcmp(args.propertyName.Str(), "VisibileHintIndex") == 0) {
            // Just set the flag.  Do NOT log here -- this callback fires
            // during dangerous states (LoadLevel, UnloadLevel) when the
            // SE logging system may contend with the loading thread.
            hintIndexChangedViaINPC_ = true;
        }
    }

    // Focus/selection tracking.
    // All cross-frame element references stored as uintptr_t ADDRESS VALUES.
    // NEVER cast back to a pointer for dereference.  Used ONLY for
    // equality comparison to detect state changes between ticks.
    // All actual element access uses fresh pointers obtained THIS frame.
    uintptr_t lastFocusedAddr_ = 0;
    uintptr_t lastSelectedAddr_ = 0;
    bool pendingINPCSubscription_ = false;  // deferred INPC subscription
    DWORD mainThreadId_ = 0;  // thread that initialized the monitor
    uintptr_t lastSelectedDCAddr_ = 0;  // DataContext address for carousel recycling detection
    uintptr_t lastFocusedDCAddr_ = 0;   // DataContext address for focused-element DC-swap detection
    std::string lastSelectedElemText_;  // tab name text for carousel text-change detection
    std::string lastInlineCarouselText_;  // Name/ColorName from inline carousel SelectedItem
    std::string lastInlineCarouselColorHex_;  // UIColor hex from color swatch items

    // Radial wheel tracking (DCHotBar widget Tag polling).
    // The XAML sets ActionRadials.Tag = LocalFocus.DataContext on every
    // radial slot focus change.  Polling Tag detects slot navigation.
    uintptr_t lastRadialTagAddr_ = 0;      // Tag object address, comparison only
    bool radialWasVisible_ = false;         // tracks radial open/close transitions

    // INPC auto-subscription tracking (focused element DC).
    // Fire-and-forget: subscribe, then CLEAR the pointer.  Never call
    // Remove() on a potentially dead object.  If the VM is destroyed,
    // Noesis already cleaned up its delegate list.  Our delegate
    // callback (OnINPCChanged) just sets a dirty flag -- harmless if
    // fired on a dead object's cleanup path.
    // inpcSubscribedDCAddr_ tracks the DC address last subscribed so
    // the tick can detect when the focused DC changes (carousel
    // recycling) and force re-subscription without waiting for a
    // snapshot dispatch.
    uintptr_t inpcSubscribedDCAddr_ = 0;   // DC address of last INPC subscription (comparison only)

    // Widget DC INPC subscription tracking (Phase 3).
    // Fire-and-forget like element INPC above.  The widget DC address
    // is stored as uintptr_t for duplicate-subscription avoidance only.
    // The widget address is stored for re-discovery from the live widget
    // list when the dirty flag fires.
    uintptr_t widgetInpcDCAddr_ = 0;       // DC address, comparison only (never dereferenced)
    uintptr_t widgetInpcWidgetAddr_ = 0;   // widget address, re-discovered from live widgets before use

    // Per-frame batching: INPC handlers set dirty flags instead of
    // firing immediately.  The tick fires at most ONE callback per type
    // per frame, eliminating the multi-property-change event storm.
    bool inpcDirty_ = false;
    bool widgetDCDirty_ = false;
    bool tipsAlreadyDelivered_ = false; // blocks BufferLoadingTips after delivery until next suppress(true)
    bool hintIndexChangedViaINPC_ = false; // set by OnWidgetINPCChanged, consumed by BufferLoadingTips_SEH

    // Widget container tracking (Strategy 4).
    // All stored as uintptr_t -- comparison only, never dereferenced.
    // widgetContainer_ is the exception: it's re-discovered each tick
    // via FindWidgetContainer() when cachedRootAddr_ changes.
    uintptr_t cachedRootAddr_ = 0;
    Noesis::Visual* widgetContainer_ = nullptr;  // re-discovered from root each tick when root changes
    Noesis::Visual* cachedTrueRoot_ = nullptr;   // true visual root (above content), for popup detection
    Noesis::Visual* cachedContentChild_ = nullptr; // direct child of trueRoot that contains widgets
    uint32_t forcedNullCount_ = 0;  // limits forced re-arms when focus is null
    uint32_t prevWidgetCount_ = 0;
    uintptr_t prevWidgetAddrs_[kMaxWidgets] = {};
    bool prevWidgetVisible_[kMaxWidgets] = {};

    // Pre-settle baseline: saved when settle STARTS so that post-settle
    // Strategy 4 can compare against the state BEFORE the settle window.
    // Without this, prevWidgetAddrs_/prevWidgetVisible_ are updated
    // during settle (for stability detection), destroying the baseline.
    // Dialogs/overlays that appear or become visible during settle
    // would never be detected as "new."
    uint32_t settleBaselineWidgetCount_ = 0;
    uintptr_t settleBaselineWidgetAddrs_[kMaxWidgets] = {};
    bool settleBaselineWidgetVisible_[kMaxWidgets] = {};

    lua::PersistentRegistryEntry callback_;
    bool forceNext_ = false;
    bool postSettle_ = false;           // true on the ONE tick after settle expires
    bool hadFocusBefore_ = false;       // true once any focus/selection was found
    bool suppressTick_ = false;         // skip all Noesis calls during loading
    // Previous tick's tooltip-open state.  Used to emit a single-shot
    // tooltipChanged event on the true->false transition so Lua can
    // invalidate tooltip-derived state (compare stash, inspect cache)
    // when a tooltip disappears without new content arriving.  The
    // per-tick tooltipTexts vector stays empty in this case, which is
    // how Lua distinguishes close from new-content events.
    bool lastTooltipOpen_ = false;
    // Event-driven focus mode (default true).  When true, GotFocus and
    // SelectionChanged class handlers provide focused/selected elements
    // directly -- no per-frame tree walks.  When false, falls back to the
    // original Strategies 1+2+3 tree walks.  One-line rollback safety net.
    bool useEventDrivenFocus_ = true;
    // Lua-driven dialogue poll gate.  Set by Ext.UI.SetDialoguePollActive()
    // when the Cutscene module detects an active dialogue.  When false, the
    // dialogue per-tick tree walk never runs (zero cost during exploration).
    bool dialogPollActive_ = false;
    int inpcCooldown_ = 0;              // ticks since last focus/selection dispatch; suppresses stray INPC echoes
    uintptr_t lastDispatchedElemAddr_ = 0;  // element address at last dispatch (for cooldown gating)
    int initialWidgetScanDelay_ = 0;      // stability counter for pre-focus scan
    uint32_t lastScanWidgetCount_ = 0;   // fingerprint: widget count at last scan
    uintptr_t lastScanFirstAddr_ = 0;    // fingerprint: first widget addr at last scan
    int overlayPollCount_ = 0;

    // Strategy 3: event-driven selection detection.
    // When sSelectionChangedEvent is resolved, Strategy 3 only runs when
    // the event fires (dirty flag) or on initial load, instead of every frame.
    bool initialSelectionDone_ = false;

    // Deferred namedTexts re-collection after tab switch.
    // Waits for Noesis to update Visibility states before collecting
    // so IsElementVisible can reliably filter cross-tab TextBlocks.

    // ----- Tick Snapshot (one-per-frame state package) -----
    // Populated throughout Tick(), dispatched once at the end.
    ecl::lua::TickSnapshot tickSnapshot_;
    std::string previousSnapshotElemId_;    // Delta: last sent elemId
    std::string previousSnapshotCarousel_;  // Delta: last sent inline carousel value
    std::vector<std::pair<std::string, std::string>> previousSnapshotDCProps_; // Delta: last sent DC props
    int lastDispatchedIsChecked_ = -1;     // Delta: last dispatched isChecked for expander toggle detection

    // Tree settle: number of frames to skip ALL tree walks after a
    // selection change or widget set change.  Noesis tears down and
    // rebuilds parts of the visual tree during tab switches; walking
    // it mid-rebuild crashes on destroyed nodes.  This gives the layout
    // engine time to finish before we re-enter the tree.
    bool settling_ = false;
    uint32_t settleStableCount_ = 0;
    uint32_t settleTotalCount_ = 0;
    bool selectionFiredDuringSettle_ = false;

    // (Accumulators removed -- snapshot is a local in Tick(), populated directly.)

    // Polling stability counter: FindNameInWidgetScoped-based polling
    // (context menu, radial, active search) is deferred until the widget
    // set has been stable for 10 consecutive frames.  Resets on any
    // widget set change.  This prevents deadlocks from BFS on partially-
    // constructed widgets during the post-load transition without
    // affecting the settle window (which would batch widgets and break
    // radial/party menu ordering).
    int pollStableFrames_ = 0;

    // Cached widget DC type names.  Refreshed only when the widget set
    // changes, avoiding per-tick Noesis DP reads that can deadlock
    // against the rendering thread.
    std::vector<std::string> cachedWidgetDCTypes_;
    std::vector<std::string> cachedWidgetAddrs_;
    std::vector<std::string> cachedWidgetNames_;

    // Strategy 3 performance counters (diagnostic -- remove before shipping).
    uint32_t strategy3Runs_ = 0;        // total times FindSelectedTabInTree ran
    uint32_t strategy3Changes_ = 0;     // times it found a DIFFERENT result
};

// ---------------------------------------------------------------------------
// GetFocusedElement: multi-strategy focus detection.
// ---------------------------------------------------------------------------
static bool sFocusPropsInitialized = false;

void InitFocusProperties(Noesis::FrameworkElement* root)
{
    if (sFocusPropsInitialized) {
        // Retry any properties that failed to resolve on first call.
        // FocusManager may not be registered in reflection early in startup.
        if (!sFocusedElementProp) {
            auto fmType = Noesis::Reflection::GetType(Noesis::Symbol("FocusManager"));
            if (fmType) {
                auto fmClass = static_cast<Noesis::TypeClass const*>(fmType);
                sFocusedElementProp = Noesis::TypeHelpers::GetDependencyProperty(
                    fmClass, bg3se::FixedString("FocusedElement"));
                if (sFocusedElementProp) {
                    BG3A_LOG("[BG3Access] FocusedElement resolved on retry: %p", sFocusedElementProp);
                }
            }
        }
        if (!sUIWidgetType) {
            auto widgetType = Noesis::Reflection::GetType(Noesis::Symbol("ls.UIWidget"));
            if (widgetType) {
                sUIWidgetType = static_cast<Noesis::TypeClass const*>(widgetType);
                BG3A_LOG("[BG3Access] UIWidgetType resolved on retry: %p", sUIWidgetType);
            }
        }
        if (!sDCWidgetType) {
            auto dcType = Noesis::Reflection::GetType(Noesis::Symbol("ls.DCWidget"));
            if (dcType) {
                sDCWidgetType = static_cast<Noesis::TypeClass const*>(dcType);
                BG3A_LOG("[BG3Access] DCWidgetType resolved on retry: %p", sDCWidgetType);
            }
        }
        if (!sLSMoveFocusIsFocusedProp) {
            auto mfType = Noesis::Reflection::GetType(Noesis::Symbol("ls.MoveFocus"));
            if (mfType) {
                auto mfClass = static_cast<Noesis::TypeClass const*>(mfType);
                sLSMoveFocusIsFocusedProp = Noesis::TypeHelpers::GetDependencyProperty(
                    mfClass, bg3se::FixedString("IsFocused"));
                if (sLSMoveFocusIsFocusedProp) {
                    BG3A_LOG("[BG3Access] ls:MoveFocus.IsFocused resolved on retry: %p",
                        sLSMoveFocusIsFocusedProp);
                }
            }
        }
        return;
    }
    sFocusPropsInitialized = true;
    Noesis::gStaticSymbols.Initialize();

    // Validate root before using its ClassType for DP lookups.
    auto rootClassType = SafeGetClassType_SEH(root);
    if (!rootClassType) return;

    sIsFocusedProp = Noesis::TypeHelpers::GetDependencyProperty(
        rootClassType, bg3se::FixedString("IsFocused"));

    auto fmType = Noesis::Reflection::GetType(Noesis::Symbol("FocusManager"));
    if (fmType) {
        auto fmClass = static_cast<Noesis::TypeClass const*>(fmType);
        sFocusedElementProp = Noesis::TypeHelpers::GetDependencyProperty(
            fmClass, bg3se::FixedString("FocusedElement"));
    }

    auto selectorType = Noesis::Reflection::GetType(Noesis::Symbol("Selector"));
    if (selectorType) {
        auto selectorClass = static_cast<Noesis::TypeClass const*>(selectorType);
        sIsSelectedProp = Noesis::TypeHelpers::GetDependencyProperty(
            selectorClass, bg3se::FixedString("IsSelected"));
    }

    auto lbiType = Noesis::Reflection::GetType(Noesis::Symbol("ListBoxItem"));
    if (lbiType) {
        sListBoxItemType = static_cast<Noesis::TypeClass const*>(lbiType);
    }

    // FontSize DP on TextElement (inherited by TextBlock).  Used by
    // PollTooltip to distinguish title from description text when
    // tooltip templates lack x:Names.
    auto textElementType = Noesis::Reflection::GetType(
        Noesis::Symbol("TextElement"));
    if (textElementType) {
        sFontSizeProp = Noesis::TypeHelpers::GetDependencyProperty(
            static_cast<Noesis::TypeClass const*>(textElementType),
            bg3se::FixedString("FontSize"));
    }
    if (!sFontSizeProp) {
        // Fallback: try TextBlock directly.
        auto textBlockType = Noesis::Reflection::GetType(
            Noesis::Symbol("TextBlock"));
        if (textBlockType) {
            sFontSizeProp = Noesis::TypeHelpers::GetDependencyProperty(
                static_cast<Noesis::TypeClass const*>(textBlockType),
                bg3se::FixedString("FontSize"));
        }
    }

    // UIWidget type -- for dynamic widget container discovery.
    auto widgetType = Noesis::Reflection::GetType(Noesis::Symbol("ls.UIWidget"));
    if (widgetType) {
        sUIWidgetType = static_cast<Noesis::TypeClass const*>(widgetType);
    }

    // DCWidget type -- some Larian widgets use this instead of UIWidget.
    auto dcWidgetType = Noesis::Reflection::GetType(Noesis::Symbol("ls.DCWidget"));
    if (dcWidgetType) {
        sDCWidgetType = static_cast<Noesis::TypeClass const*>(dcWidgetType);
    }

    // DataContext -- used to detect ListBoxItem recycling (carousel
    // virtualisation reuses the same element with swapped data).
    sDataContextProp = Noesis::TypeHelpers::GetDependencyProperty(
        rootClassType, bg3se::FixedString("DataContext"));

    // IsVisible -- read-only computed DP.  Used internally for tree walk pruning.
    sIsVisibleProp = Noesis::TypeHelpers::GetDependencyProperty(
        rootClassType, bg3se::FixedString("IsVisible"));
    // Visibility -- settable enum DP (Collapsed/Hidden/Visible).  Used by
    // IsElementVisible to walk ancestors via VisualTreeHelper::GetParent()
    // and detect Collapsed/Hidden parents (the IsVisible DP read does not
    // coerce through ancestors in the Indie SDK).
    sVisibilityProp = Noesis::TypeHelpers::GetDependencyProperty(
        rootClassType, bg3se::FixedString("Visibility"));
    sIsHitTestVisibleProp = Noesis::TypeHelpers::GetDependencyProperty(
        rootClassType, bg3se::FixedString("IsHitTestVisible"));

    // ls:MoveFocus -- Larian's custom controller focus system.
    // All controller menus use ls:MoveFocus.IsFocused to track which element
    // has controller focus.  SetMoveFocusAction sets IsFocused on individual
    // elements (fired from XAML EventTriggers on IsVisibleChanged).
    auto moveFocusType = Noesis::Reflection::GetType(Noesis::Symbol("ls.MoveFocus"));
    if (moveFocusType) {
        auto mfClass = static_cast<Noesis::TypeClass const*>(moveFocusType);
        sLSMoveFocusIsFocusedProp = Noesis::TypeHelpers::GetDependencyProperty(
            mfClass, bg3se::FixedString("IsFocused"));
        sLSMoveFocusFocusableProp = Noesis::TypeHelpers::GetDependencyProperty(
            mfClass, bg3se::FixedString("Focusable"));
    }

    // NameScope attached DP -- used by CollectNamedTextsFromWidget to read
    // the NameScope from widget elements (replaces the non-exported
    // NameScope::GetNameScope static method).
    auto nameScopeType = Noesis::Reflection::GetType(Noesis::Symbol("NameScope"));
    if (nameScopeType) {
        auto nameScopeClass = static_cast<Noesis::TypeClass const*>(nameScopeType);
        sNameScopeProp = Noesis::TypeHelpers::GetDependencyProperty(
            nameScopeClass, bg3se::FixedString("NameScope"));
    }

    // FrameworkElement.Tag -- standard WPF property, always resolvable.
    sTagProp = Noesis::TypeHelpers::GetDependencyProperty(
        rootClassType, bg3se::FixedString("Tag"));


    // Discover Selector.SelectionChanged routed event at runtime.
    // Register class handler with invokeHandledEvents=true on the Selector
    // type's UIElementData.  Class handlers fire at the SOURCE element
    // before instance handlers, so this fires before any child ListBox
    // can set Handled=true.  Replaces per-widget instance subscriptions.
    auto selectorReflType = Noesis::Reflection::GetType(Noesis::Symbol("Selector"));
    if (selectorReflType) {
        auto selectorMeta = static_cast<Noesis::TypeMeta const*>(selectorReflType);
        for (auto* metaEntry : selectorMeta->mMetaData) {
            if (!metaEntry) continue;
            auto metaTypeName = SafeBaseObjectTypeName_SEH(metaEntry);
            if (!metaTypeName) continue;
            if (strstr(metaTypeName, "UIElementData")) {
                auto elementData = static_cast<Noesis::UIElementData const*>(metaEntry);
                sSelectionChangedEvent = Noesis::UIElementDataHelpers::GetEvent(
                    elementData, Noesis::Symbol("SelectionChanged"));

                if (sSelectionChangedEvent) {
                    auto mutableData = const_cast<Noesis::UIElementData*>(elementData);
                    Noesis::EventHandlerInfo selectionInfo;
                    selectionInfo.handler = Noesis::EventHandler(
                        kClassSelectionPtr,
                        &ClassSelectionDelegate::Handler);
                    selectionInfo.invokeHandledEvents = true;
                    mutableData->mEventHandlers.Insert(
                        sSelectionChangedEvent, selectionInfo);
                    BG3A_LOG("[BG3Access] Registered class SelectionChanged handler on Selector (invokeHandledEvents=true)");
                }

                break;
            }
        }
    }
    BG3A_TRACE("[BG3Access] SelectionChanged event: selectorType=%p event=%p",
        selectorReflType, sSelectionChangedEvent);

    // Discover GotFocus event from UIElement type's UIElementData.
    // Register class handler with invokeHandledEvents=true.
    // GotFocus fires for controller d-pad navigation (logical focus).
    // GotKeyboardFocus does NOT fire for controller -- not registered.
    auto uiElementReflType = Noesis::Reflection::GetType(Noesis::Symbol("UIElement"));
    if (uiElementReflType) {
        auto uiElementMeta = static_cast<Noesis::TypeMeta const*>(uiElementReflType);
        for (auto* metaEntry : uiElementMeta->mMetaData) {
            if (!metaEntry) continue;
            auto metaTypeName = SafeBaseObjectTypeName_SEH(metaEntry);
            if (!metaTypeName) continue;
            if (strstr(metaTypeName, "UIElementData")) {
                auto elementData = const_cast<Noesis::UIElementData*>(
                    static_cast<Noesis::UIElementData const*>(metaEntry));

                sGotFocusEvent = Noesis::UIElementDataHelpers::GetEvent(
                    elementData, Noesis::Symbol("GotFocus"));

                BG3A_LOG("[BG3Access] GotFocus=%p", sGotFocusEvent);

                if (sGotFocusEvent) {
                    Noesis::EventHandlerInfo focusInfo;
                    focusInfo.handler = Noesis::EventHandler(
                        kGotFocusPtr,
                        &GotFocusDelegate::Handler);
                    focusInfo.invokeHandledEvents = true;
                    elementData->mEventHandlers.Insert(sGotFocusEvent, focusInfo);
                    BG3A_LOG("[BG3Access] Registered GotFocus class handler on UIElement");
                }

                break;
            }
        }
    }

    // Diagnostic: show what resolved.
    BG3A_LOG("[BG3Access] InitFocusProperties: IsFocused=%p FocusedElement=%p IsSelected=%p DataContext=%p IsVisible=%p Visibility=%p ListBoxItemType=%p UIWidgetType=%p DCWidgetType=%p LSMoveFocusIsFocused=%p LSMoveFocusFocusable=%p NameScope=%p Tag=%p GotFocus=%p",
        sIsFocusedProp, sFocusedElementProp, sIsSelectedProp, sDataContextProp, sIsVisibleProp, sVisibilityProp, sListBoxItemType, sUIWidgetType, sDCWidgetType, sLSMoveFocusIsFocusedProp, sLSMoveFocusFocusableProp, sNameScopeProp, sTagProp, sGotFocusEvent);

    // Event-driven widget discovery: register Loaded/Unloaded class handlers
    // on ls.UIWidget (or ls.DCWidget) so widget lifecycle is tracked without
    // per-tick GetVisualChildrenCount/GetVisualChild virtual calls.
    //
    // Step 1: Get Loaded/Unloaded RoutedEvent* from FrameworkElement's
    //         UIElementData (where they're defined).
    // Step 2: Find ls.UIWidget's (or ls.DCWidget's) UIElementData and
    //         insert handlers there -- scopes to UIWidget instances only.
    if (!sWidgetHandlersRegistered) {
        Noesis::RoutedEvent* loadedEvent = nullptr;
        Noesis::RoutedEvent* unloadedEvent = nullptr;

        auto frameworkElementReflType = Noesis::Reflection::GetType(
            Noesis::Symbol("FrameworkElement"));
        if (frameworkElementReflType) {
            auto frameworkElementMeta = static_cast<Noesis::TypeMeta const*>(
                frameworkElementReflType);
            for (auto* metaEntry : frameworkElementMeta->mMetaData) {
                if (!metaEntry) continue;
                auto metaTypeName = SafeBaseObjectTypeName_SEH(metaEntry);
                if (!metaTypeName) continue;
                if (strstr(metaTypeName, "UIElementData")) {
                    auto elementData = static_cast<Noesis::UIElementData const*>(
                        metaEntry);
                    loadedEvent = Noesis::UIElementDataHelpers::GetEvent(
                        elementData, Noesis::Symbol("Loaded"));
                    unloadedEvent = Noesis::UIElementDataHelpers::GetEvent(
                        elementData, Noesis::Symbol("Unloaded"));
                    BG3A_LOG("[BG3Access] FrameworkElement events: Loaded=%p Unloaded=%p",
                        loadedEvent, unloadedEvent);
                    break;
                }
            }
        }

        if (loadedEvent && unloadedEvent) {
            sLoadedEvent = loadedEvent;
            sUnloadedEvent = unloadedEvent;

            // Try to find UIElementData on ls.UIWidget, then ls.DCWidget.
            // Registering on the widget type's UIElementData scopes the
            // handler to fire ONLY for widget instances (not all FEs).
            Noesis::UIElementData* targetElementData = nullptr;
            const char* targetTypeName = nullptr;

            // Try ls.UIWidget first.
            if (sUIWidgetType) {
                auto widgetMeta = static_cast<Noesis::TypeMeta const*>(sUIWidgetType);
                for (auto* metaEntry : widgetMeta->mMetaData) {
                    if (!metaEntry) continue;
                    auto typeName = SafeBaseObjectTypeName_SEH(metaEntry);
                    if (!typeName) continue;
                    if (strstr(typeName, "UIElementData")) {
                        targetElementData = const_cast<Noesis::UIElementData*>(
                            static_cast<Noesis::UIElementData const*>(metaEntry));
                        targetTypeName = "ls.UIWidget";
                        break;
                    }
                }
            }

            // Fallback: try ls.DCWidget.
            if (!targetElementData && sDCWidgetType) {
                auto dcWidgetMeta = static_cast<Noesis::TypeMeta const*>(sDCWidgetType);
                for (auto* metaEntry : dcWidgetMeta->mMetaData) {
                    if (!metaEntry) continue;
                    auto typeName = SafeBaseObjectTypeName_SEH(metaEntry);
                    if (!typeName) continue;
                    if (strstr(typeName, "UIElementData")) {
                        targetElementData = const_cast<Noesis::UIElementData*>(
                            static_cast<Noesis::UIElementData const*>(metaEntry));
                        targetTypeName = "ls.DCWidget";
                        break;
                    }
                }
            }

            if (targetElementData) {
                // Register Loaded handler.
                Noesis::EventHandlerInfo loadedInfo;
                loadedInfo.handler = Noesis::EventHandler(
                    kWidgetLoadedPtr, &WidgetLoadedDelegate::Handler);
                loadedInfo.invokeHandledEvents = true;
                targetElementData->mEventHandlers.Insert(sLoadedEvent, loadedInfo);

                // Register Unloaded handler.
                Noesis::EventHandlerInfo unloadedInfo;
                unloadedInfo.handler = Noesis::EventHandler(
                    kWidgetUnloadedPtr, &WidgetUnloadedDelegate::Handler);
                unloadedInfo.invokeHandledEvents = true;
                targetElementData->mEventHandlers.Insert(sUnloadedEvent, unloadedInfo);

                sWidgetHandlersRegistered = true;
                BG3A_LOG("[BG3Access] Registered Loaded/Unloaded class handlers on %s",
                    targetTypeName);
            } else {
                BG3A_LOG("[BG3Access] WARNING: No UIElementData found on ls.UIWidget "
                    "or ls.DCWidget -- using per-tick GatherWidgets_SEH fallback");
            }
        } else {
            BG3A_LOG("[BG3Access] WARNING: Loaded/Unloaded events not found on "
                "FrameworkElement -- using per-tick GatherWidgets_SEH fallback");
        }
    }

    // Event-driven tooltip detection: register Opened/Closed class handlers
    // on Noesis::ToolTip so PollTooltip's per-tick BFS can be skipped while
    // a tooltip is open.  Falls back to PollTooltip for non-ToolTip popups
    // (BG3 may use bespoke Popup-based tooltips in some places).
    if (!sToolTipHandlersRegistered) {
        if (!sToolTipType) {
            auto tooltipReflType = Noesis::Reflection::GetType(
                Noesis::Symbol("ToolTip"));
            if (tooltipReflType) {
                sToolTipType = static_cast<Noesis::TypeClass const*>(
                    tooltipReflType);
            }
        }

        if (sToolTipType) {
            // Step 1: get the OpenedEvent / ClosedEvent from ToolTip's
            // UIElementData (where they're declared).
            auto tooltipMeta = static_cast<Noesis::TypeMeta const*>(sToolTipType);
            Noesis::UIElementData* tooltipElementData = nullptr;
            for (auto* metaEntry : tooltipMeta->mMetaData) {
                if (!metaEntry) continue;
                auto typeName = SafeBaseObjectTypeName_SEH(metaEntry);
                if (!typeName) continue;
                if (strstr(typeName, "UIElementData")) {
                    tooltipElementData = const_cast<Noesis::UIElementData*>(
                        static_cast<Noesis::UIElementData const*>(metaEntry));
                    sToolTipOpenedEvent = Noesis::UIElementDataHelpers::GetEvent(
                        tooltipElementData, Noesis::Symbol("Opened"));
                    sToolTipClosedEvent = Noesis::UIElementDataHelpers::GetEvent(
                        tooltipElementData, Noesis::Symbol("Closed"));
                    BG3A_LOG("[BG3Access] ToolTip events: Opened=%p Closed=%p",
                        sToolTipOpenedEvent, sToolTipClosedEvent);
                    break;
                }
            }

            // Step 2: register handlers if events found.
            if (tooltipElementData
                && sToolTipOpenedEvent && sToolTipClosedEvent) {
                Noesis::EventHandlerInfo openedInfo;
                openedInfo.handler = Noesis::EventHandler(
                    kToolTipOpenedPtr, &ToolTipOpenedDelegate::Handler);
                openedInfo.invokeHandledEvents = true;
                tooltipElementData->mEventHandlers.Insert(
                    sToolTipOpenedEvent, openedInfo);

                Noesis::EventHandlerInfo closedInfo;
                closedInfo.handler = Noesis::EventHandler(
                    kToolTipClosedPtr, &ToolTipClosedDelegate::Handler);
                closedInfo.invokeHandledEvents = true;
                tooltipElementData->mEventHandlers.Insert(
                    sToolTipClosedEvent, closedInfo);

                sToolTipHandlersRegistered = true;
                BG3A_LOG("[BG3Access] Registered ToolTip Opened/Closed class handlers");
            } else {
                BG3A_LOG("[BG3Access] WARNING: ToolTip Opened/Closed events not found"
                    " -- falling back to per-tick PollTooltip");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Runtime discovery of FocusedElement DependencyProperty.
//
// FocusManager is a helper class (not a visual element).  Its reflection
// entry may not be registered via Noesis::Reflection::GetType() in the
// Indie SDK.  Instead, scan a live UIWidget's mValues for an attached
// property named "FocusedElement" and cache the DependencyProperty* once
// found.  This is called lazily from Tick() when widgets are available.
// ---------------------------------------------------------------------------
// SEH helper: scan mValues for a named DependencyProperty.
// Returns the DP* or nullptr on fault.
static Noesis::DependencyProperty const* SafeScanMValuesForDP_SEH(
    Noesis::DependencyObject const* depObj, Noesis::Symbol targetName)
{
    __try {
        for (auto& prop : depObj->mValues) {
            if (prop.key->GetName() == targetName) {
                return prop.key;
            }
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void TryDiscoverFocusedElementProp(Noesis::Visual* const* widgets, uint32_t count)
{
    if (sFocusedElementProp) return;  // already resolved
    static const Noesis::Symbol sFocusedElementSym("FocusedElement");

    for (uint32_t i = 0; i < count; i++) {
        if (!widgets[i]) continue;
        auto depObj = static_cast<Noesis::DependencyObject const*>(widgets[i]);
        auto found = SafeScanMValuesForDP_SEH(depObj, sFocusedElementSym);
        if (found) {
            sFocusedElementProp = found;
            BG3A_LOG("[BG3Access] FocusedElement discovered from widget mValues: %p", sFocusedElementProp);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers for optimized focus detection
// ---------------------------------------------------------------------------

// Check computed IsVisible via DependencyProperty.
// Returns true if IsVisible is true or if the property couldn't be resolved.
static bool IsVisibleDP(Noesis::Visual const* elem)
{
    if (!sIsVisibleProp) return true;
    auto val = SafeGetDPValue_SEH(sIsVisibleProp,
        static_cast<Noesis::DependencyObject const*>(elem));
    return !val || *static_cast<const bool*>(val);
}

// Check if an element's class derives from ls.UIWidget.
static bool IsUIWidgetType(Noesis::Visual const* elem)
{
    auto cls = SafeGetClassType_SEH(elem);
    while (cls) {
        if (sUIWidgetType && cls == sUIWidgetType) return true;
        if (sDCWidgetType && cls == sDCWidgetType) return true;
        cls = cls->GetBase();
    }
    return false;
}

// ---------------------------------------------------------------------------
// PollRadialLocalFocus: SEH-guarded radial focus detection.
// Scans visible widgets for a DCGameMenu widget (RT shortcuts radial),
// finds its MenuRadial element, reads LocalFocus to detect which slot
// the LS is pointing at, and populates the snapshot with title/description.
// Extracted to a standalone function for SEH compatibility (no C++ objects
// with destructors).
// ---------------------------------------------------------------------------
// LookupLocalFocusDP: separated so the FixedString temporary
// (which has a destructor) does not live in the __try function.
static const Noesis::DependencyProperty* LookupLocalFocusDP(
    Noesis::TypeClass const* classType)
{
    return Noesis::TypeHelpers::GetDependencyProperty(
        classType, bg3se::FixedString("LocalFocus"));
}

// Read the Noesis class type name from a raw DataContext void* value.
// The DC object may have been freed after the pointer was obtained, so both
// the dereference and the vtable call are wrapped in SEH.
// Returns the type name string (static storage, valid for the tick), or nullptr.
static const char* ReadWidgetDCTypeName_SEH(const void* widgetDCVal)
{
    __try {
        auto widgetDC = *reinterpret_cast<Noesis::BaseComponent* const*>(widgetDCVal);
        if (!widgetDC) return nullptr;
        auto classType = widgetDC->GetClassType();
        if (!classType) return nullptr;
        return classType->GetName();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] AV in ReadWidgetDCTypeName_SEH -- stale DC pointer skipped");
        return nullptr;
    }
}

// PollRadialLocalFocus_Unsafe: SEH-guarded inner function.
// Only does raw pointer reads -- no C++ objects with destructors.
// Returns the focused item element and the widget visual via out params.
// Returns true if a new radial slot was detected.
static bool PollRadialLocalFocus_Unsafe(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount,
    Noesis::FrameworkElement** outFocusedItem,
    Noesis::Visual** outWidgetVisual)
{
    // LocalFocus DP cache.  Retries on every call until a successful
    // lookup populates the cache -- the previous sLocalFocusLookupDone
    // latch could stick in the "failed forever" state if the first
    // lookup happened on a transient element whose class chain wasn't
    // fully wired yet.  Same bug pattern as the context menu latches.
    static const Noesis::DependencyProperty* sLocalFocusProp = nullptr;
    static uintptr_t sLastLocalFocusAddr = 0;

    *outFocusedItem = nullptr;
    *outWidgetVisual = nullptr;

    __try {
        for (uint32_t widgetIdx = 0; widgetIdx < widgetCount; widgetIdx++) {
            if (!widgets[widgetIdx] || !widgetVisible[widgetIdx]) continue;
            auto widgetElement = static_cast<Noesis::FrameworkElement*>(
                const_cast<Noesis::Visual*>(widgets[widgetIdx]));
            auto widgetDepObj = static_cast<Noesis::DependencyObject const*>(widgetElement);

            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(widgetElement))) continue;
            auto widgetDCVal = sDataContextProp->GetValue(widgetDepObj);
            if (!widgetDCVal) continue;
            // DC object may be freed after GetValue returns -- use SEH helper.
            auto widgetDCTypeName = ReadWidgetDCTypeName_SEH(widgetDCVal);
            if (!widgetDCTypeName || !strstr(widgetDCTypeName, "DCGameMenu")) continue;

            auto menuRadial = FindNameInWidgetScoped(
                "MenuRadial", widgets[widgetIdx]);
            if (!menuRadial) continue;

            if (!sLocalFocusProp) {
                sLocalFocusProp = LookupLocalFocusDP(menuRadial->GetClassType());
                if (sLocalFocusProp) {
                    BG3A_LOG("[BG3Access] LocalFocus DP found on %s",
                         menuRadial->GetClassType()->GetName());
                }
            }
            if (!sLocalFocusProp) continue;

            auto localFocusDepObj = static_cast<Noesis::DependencyObject const*>(
                static_cast<Noesis::FrameworkElement*>(menuRadial));
            auto localFocusVal = sLocalFocusProp->GetValue(localFocusDepObj);
            if (!localFocusVal) {
                sLastLocalFocusAddr = 0;
                continue;
            }
            auto localFocusObj = *reinterpret_cast<Noesis::BaseObject* const*>(localFocusVal);
            if (!localFocusObj) {
                sLastLocalFocusAddr = 0;
                continue;
            }

            auto localFocusAddr = reinterpret_cast<uintptr_t>(localFocusObj);
            if (localFocusAddr != sLastLocalFocusAddr) {
                sLastLocalFocusAddr = localFocusAddr;
                if (ProbeUIElement(reinterpret_cast<Noesis::UIElement*>(localFocusObj))) {
                    *outFocusedItem = static_cast<Noesis::FrameworkElement*>(
                        reinterpret_cast<Noesis::UIElement*>(localFocusObj));
                    *outWidgetVisual = widgets[widgetIdx];
                    return true;
                }
            }
            break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] PollRadialLocalFocus: access fault (SEH caught)");
    }
    return false;
}

// PollRadialLocalFocus: outer wrapper that does text extraction
// (std::string operations) outside the SEH block.
static void PollRadialLocalFocus(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, ecl::lua::TickSnapshot* snapshot)
{
    Noesis::FrameworkElement* focusedItem = nullptr;
    Noesis::Visual* widgetVisual = nullptr;

    if (!PollRadialLocalFocus_Unsafe(widgets, widgetVisible, widgetCount,
                                     &focusedItem, &widgetVisual)) {
        return;
    }

    // Text extraction (uses std::string -- safe outside __try).
    auto tagString = ReadPropertyAsString(focusedItem, "Tag");

    std::string titleText;
    std::string descriptionText;
    auto actionTitleElem = FindNameInWidgetScoped("ActionTitle", widgetVisual);
    if (actionTitleElem) {
        titleText = ReadTextBlockText(actionTitleElem);
    }
    auto descriptionElem = FindNameInWidgetScoped("Description", widgetVisual);
    if (descriptionElem) {
        descriptionText = ReadTextBlockText(descriptionElem);
    }

    snapshot->radialSlotChanged = true;
    snapshot->radialSlotTag = std::move(tagString);
    snapshot->radialTitleText = std::move(titleText);
    snapshot->radialDescriptionText = std::move(descriptionText);
    snapshot->radialSlotType = "ShortcutsMenu";

    BG3A_LOG("[BG3Access] RADIAL FOCUS: tag=%s title=%s",
         snapshot->radialSlotTag.c_str(),
         snapshot->radialTitleText.empty()
             ? "(none)" : snapshot->radialTitleText.c_str());
}

// ---------------------------------------------------------------------------
// PollActiveSearchLocalFocus: SEH-guarded LocalFocus polling for the
// ActiveSearch panel (hold A on world items) and its X actions submenu.
// These use Larian's LocalFocus DP on the OptionsContainer LSListBox,
// not Noesis keyboard focus, so the three focus strategies miss them.
// Follows the same two-layer SEH pattern as PollRadialLocalFocus.
// ---------------------------------------------------------------------------

// PollActiveSearchLocalFocus_Unsafe: SEH-guarded inner function.
// Finds the ActiveSearch widget (DCActiveSearch DC type), looks up its
// OptionsContainer LSListBox via FindNameInWidgetScoped, and polls the
// LocalFocus DP for changes.  Returns the focused item element and
// widget visual via out params.  Returns true if a new focus was detected.
static bool PollActiveSearchLocalFocus_Unsafe(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount,
    Noesis::FrameworkElement** outFocusedItem,
    Noesis::Visual** outWidgetVisual)
{
    // ActiveSearch LocalFocus DP cache.  Retries until a successful
    // lookup populates the pointer.  Previous sActiveSearchLocalFocusLookupDone
    // latch could stick in "failed forever" if the first lookup happened
    // on a transient element -- same bug pattern as the context menu
    // and radial latches.
    static const Noesis::DependencyProperty* sActiveSearchLocalFocusProp = nullptr;
    static uintptr_t sLastActiveSearchLocalFocusAddr = 0;

    *outFocusedItem = nullptr;
    *outWidgetVisual = nullptr;

    __try {
        bool foundActiveSearchWidget = false;
        for (uint32_t widgetIdx = 0; widgetIdx < widgetCount; widgetIdx++) {
            if (!widgets[widgetIdx] || !widgetVisible[widgetIdx]) continue;
            auto widgetElement = static_cast<Noesis::FrameworkElement*>(
                const_cast<Noesis::Visual*>(widgets[widgetIdx]));
            auto widgetDepObj = static_cast<Noesis::DependencyObject const*>(widgetElement);

            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(widgetElement))) continue;
            auto widgetDCVal = sDataContextProp->GetValue(widgetDepObj);
            if (!widgetDCVal) continue;
            auto widgetDCTypeName = ReadWidgetDCTypeName_SEH(widgetDCVal);
            if (!widgetDCTypeName || !strstr(widgetDCTypeName, "DCActiveSearch")) continue;

            foundActiveSearchWidget = true;
            BG3A_TRACE("[BG3Access] ActiveSearch: found widget[%u] DC=%s", widgetIdx, widgetDCTypeName);

            // Found the ActiveSearch widget.  Look up OptionsContainer.
            auto optionsContainer = FindNameInWidgetScoped(
                "OptionsContainer", widgets[widgetIdx]);
            if (!optionsContainer) {
                BG3A_TRACE("[BG3Access] ActiveSearch: OptionsContainer NOT found in widget");
                continue;
            }
            BG3A_TRACE("[BG3Access] ActiveSearch: OptionsContainer found, class=%s",
                     optionsContainer->GetClassType()->GetName());

            // LocalFocus DP discovery on the OptionsContainer element.
            // Retries until successful; does not latch on failure.
            if (!sActiveSearchLocalFocusProp) {
                sActiveSearchLocalFocusProp = LookupLocalFocusDP(
                    optionsContainer->GetClassType());
                if (sActiveSearchLocalFocusProp) {
                    BG3A_LOG("[BG3Access] ActiveSearch LocalFocus DP found on %s",
                             optionsContainer->GetClassType()->GetName());
                } else {
                    BG3A_TRACE("[BG3Access] ActiveSearch: LocalFocus DP NOT found on %s",
                             optionsContainer->GetClassType()->GetName());
                }
            }
            if (!sActiveSearchLocalFocusProp) continue;

            auto containerDepObj = static_cast<Noesis::DependencyObject const*>(
                static_cast<Noesis::FrameworkElement*>(optionsContainer));
            auto localFocusVal = sActiveSearchLocalFocusProp->GetValue(containerDepObj);
            if (!localFocusVal) {
                BG3A_TRACE("[BG3Access] ActiveSearch: LocalFocus value is null");
                sLastActiveSearchLocalFocusAddr = 0;
                continue;
            }
            auto localFocusObj = *reinterpret_cast<Noesis::BaseObject* const*>(localFocusVal);
            if (!localFocusObj) {
                BG3A_TRACE("[BG3Access] ActiveSearch: LocalFocus deref is null");
                sLastActiveSearchLocalFocusAddr = 0;
                continue;
            }

            auto localFocusAddr = reinterpret_cast<uintptr_t>(localFocusObj);
            BG3A_TRACE("[BG3Access] ActiveSearch: LocalFocus addr=0x%llx prev=0x%llx",
                     localFocusAddr, sLastActiveSearchLocalFocusAddr);
            if (localFocusAddr != sLastActiveSearchLocalFocusAddr) {
                sLastActiveSearchLocalFocusAddr = localFocusAddr;
                if (ProbeUIElement(reinterpret_cast<Noesis::UIElement*>(localFocusObj))) {
                    *outFocusedItem = static_cast<Noesis::FrameworkElement*>(
                        reinterpret_cast<Noesis::UIElement*>(localFocusObj));
                    *outWidgetVisual = widgets[widgetIdx];
                    return true;
                }
                BG3A_TRACE("[BG3Access] ActiveSearch: LocalFocus element failed ProbeUIElement");
            }
            break;
        }
        if (!foundActiveSearchWidget) {
            // Only log once per "session" to avoid spam -- the condition
            // (!focused && !selected) fires every frame during world nav.
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] PollActiveSearchLocalFocus: access fault (SEH caught)");
    }
    return false;
}

// PollActiveSearchLocalFocus: outer wrapper that does text extraction
// (std::string operations) outside the SEH block.
static void PollActiveSearchLocalFocus(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, ecl::lua::TickSnapshot* snapshot)
{
    Noesis::FrameworkElement* focusedItem = nullptr;
    Noesis::Visual* widgetVisual = nullptr;

    if (!PollActiveSearchLocalFocus_Unsafe(widgets, widgetVisible, widgetCount,
                                           &focusedItem, &widgetVisual)) {
        return;
    }

    // Extract full element data so Lua gets DC properties, type, etc.
    // This populates focusedElement with everything the Lua handler needs.
    snapshot->focusChanged = true;
    ExtractElementData(snapshot->focusedElement,
        static_cast<Noesis::FrameworkElement*>(focusedItem));

    BG3A_LOG("[BG3Access] ACTIVESEARCH FOCUS: type=%s dc=%s",
             snapshot->focusedElement.elemType.c_str(),
             snapshot->focusedElement.dcType.c_str());
}

// ---------------------------------------------------------------------------
// PollContextMenu: detects the highlighted item in a Noesis ContextMenu
// popup (e.g. WorldContextMenu's right-click/X-button actions).
//
// Noesis ContextMenu popups live in a separate rendering layer invisible
// to visual tree walking from GetRoot().  We reach them through the
// ContextMenu DP on the WorldContextEntity element inside the
// WorldContextMenu widget.
//
// Inner function (SEH): finds WorldContextEntity, checks IsOpen, walks
// the popup tree for ContextMenuItems, finds the highlighted one and
// its first TextBlock.  Returns results via out params.
// Outer function: does std::string text extraction and populates snapshot.
// ---------------------------------------------------------------------------

// Shared IsHighlighted DP cache for ContextMenuItems.
// Used by both CheckContextMenuOnElement and Source 3 popup search.
// Cache persists the FIRST SUCCESSFUL lookup; failed lookups do not
// latch, so a transient/unready first item cannot disable highlight
// detection for the session.
static const Noesis::DependencyProperty* sCtxMenuIsHighlightedProp = nullptr;

// FindHighlightedContextMenuItem: given an array of ContextMenuItems,
// finds the one with IsHighlighted=true and its first TextBlock child.
// Shared by CheckContextMenuOnElement (Sources 1/2) and Source 3.
static bool FindHighlightedContextMenuItem(
    Noesis::FrameworkElement** menuItems, uint32_t itemCount,
    Noesis::FrameworkElement** outHighlightedItem,
    Noesis::FrameworkElement** outTextBlock)
{
    if (itemCount == 0) return false;

    // IsHighlighted DP lookup.  Retries on every call until a
    // successful lookup populates the cache.
    if (!sCtxMenuIsHighlightedProp) {
        auto itemClassType = SafeGetClassType_SEH(menuItems[0]);
        if (itemClassType) {
            sCtxMenuIsHighlightedProp =
                LookupIsHighlightedDP(itemClassType);
        }
    }
    if (!sCtxMenuIsHighlightedProp) return false;

    for (uint32_t mi = 0; mi < itemCount; mi++) {
        auto itemDepObj = static_cast<Noesis::DependencyObject const*>(
            menuItems[mi]);
        if (SafeReadBoolDP_SEH(sCtxMenuIsHighlightedProp, itemDepObj)) {
            *outHighlightedItem = menuItems[mi];
            Noesis::FrameworkElement* textBlocks[4];
            auto tbCount = BFS_CollectByType_SEH(
                menuItems[mi], "TextBlock", textBlocks, 0, 4);
            if (tbCount > 0) {
                *outTextBlock = textBlocks[0];
            }
            return true;
        }
    }
    return false;
}

// CheckContextMenuOnElement: checks if a given element has a ContextMenu
// DP with IsOpen=true, and if so, finds the highlighted ContextMenuItem.
// Shared logic for both WorldContextEntity and ActiveSearch item paths.
// DP caches are static -- one-time lookups, persist across frames.
//
// Latch discipline: the DP caches persist the FIRST SUCCESSFUL lookup
// across frames.  A failed lookup (SafeGetClassType returned null, or
// the class type didn't carry the DP) does NOT latch -- it's retried
// on the next call, because the next call may arrive with a properly
// initialized element whose class chain does expose the DP.  Without
// this retry the very first call with a transient/unready element
// would disable ContextMenu detection for the entire session.
static bool CheckContextMenuOnElement(
    Noesis::FrameworkElement* element,
    Noesis::FrameworkElement** outHighlightedItem,
    Noesis::FrameworkElement** outTextBlock)
{
    static const Noesis::DependencyProperty* sContextMenuProp = nullptr;
    static const Noesis::DependencyProperty* sIsOpenProp = nullptr;

    if (!element) return false;
    if (!ProbeUIElement(static_cast<Noesis::UIElement*>(element)))
        return false;

  __try {

    // ContextMenu DP lookup.  Retries on every call until a successful
    // lookup populates the cache -- then stays cached for the session.
    if (!sContextMenuProp) {
        auto elemType = SafeGetClassType_SEH(element);
        if (elemType) {
            sContextMenuProp = LookupContextMenuDP(elemType);
        }
    }
    if (!sContextMenuProp) return false;

    // Read ContextMenu object.
    auto cmVal = SafeGetDPValue_SEH(sContextMenuProp,
        static_cast<Noesis::DependencyObject const*>(element));
    if (!cmVal) return false;
    auto cmObj = SafeDerefDPObject_SEH(cmVal);
    if (!cmObj) return false;

    // IsOpen DP lookup on the ContextMenu.  Same retry-until-success
    // discipline as the ContextMenu DP lookup above.
    if (!sIsOpenProp) {
        auto cmClassType = SafeGetClassType_SEH(cmObj);
        if (cmClassType) {
            sIsOpenProp = LookupIsOpenDP(cmClassType);
        }
    }
    if (!sIsOpenProp) return false;

    // Check IsOpen.
    auto cmDepObj = static_cast<Noesis::DependencyObject const*>(
        static_cast<Noesis::FrameworkElement*>(
            reinterpret_cast<Noesis::UIElement*>(cmObj)));
    if (!SafeReadBoolDP_SEH(sIsOpenProp, cmDepObj)) return false;

    // Menu is open.  Find ContextMenuItems and the highlighted one.
    auto cmElem = reinterpret_cast<Noesis::Visual*>(
        reinterpret_cast<Noesis::UIElement*>(cmObj));
    Noesis::FrameworkElement* menuItems[16];
    auto itemCount = BFS_CollectByType_SEH(
        cmElem, "ContextMenuItem", menuItems, 0, 16);
    return FindHighlightedContextMenuItem(
        menuItems, itemCount, outHighlightedItem, outTextBlock);

  } __except (EXCEPTION_EXECUTE_HANDLER) {
    BG3A_LOG("[BG3Access] CheckContextMenuOnElement: fault (SEH caught)");
    *outHighlightedItem = nullptr;
    *outTextBlock = nullptr;
    return false;
  }
}


// PollContextMenu_Unsafe: tries multiple element sources for an open
// context menu.  No C++ objects with destructors.
// Returns true if an open context menu with a highlighted item was found.
//
// Source ordering and gating notes (the bug this avoids):
//
// Sources 1 and 2 read the ContextMenu DP DIRECTLY on a known element
// (WorldContextEntity in a widget NameScope, or the currently focused
// element).  They do NOT need popup-root walking, and they MUST run
// unconditionally on every poll -- the previous implementation put a
// GetPopupRoots_SEH gate at the top of the function that short-circuited
// out when no Noesis popup roots were found under the cached trueRoot.
//
// That gate fails silently in the "pure world X-press" case: when the
// player opens a world context menu without any other popup visible,
// ls:ContextMenu's popup layer is NOT guaranteed to surface as a child
// of our cached trueRoot (either because cachedTrueRoot_ resolved to
// nullptr on an early frame, or because ls:ContextMenu's rendering
// plants its popup on a different layer).  The gate returns 0 and the
// whole function bails -- even though Source 1 could successfully read
// the ContextMenu DP on WorldContextEntity in one cheap lookup.
//
// The symptom was the VERY FIRST world context menu of a session going
// silent with nothing at all in the log (no detection, no "closed"
// tracker reset).  The workaround was to trigger ANY other popup
// (SelectionFlyOut, tooltip, etc.) to populate popup roots, at which
// point the gate passed and Source 1 started working.
//
// Fix: run Source 1 and Source 2 unconditionally (both are cheap -- a
// NameScope lookup + one DP read per element, no BFS).  Only compute
// popup roots lazily if Source 1 and Source 2 both fail, since Source 3
// is the only branch that actually needs them.
static bool PollContextMenu_Unsafe(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, Noesis::UIElement* focusedElement,
    Noesis::Visual* trueRoot, Noesis::Visual* contentChild,
    Noesis::FrameworkElement** outHighlightedItem,
    Noesis::FrameworkElement** outTextBlock)
{
    *outHighlightedItem = nullptr;
    *outTextBlock = nullptr;

  __try {
    // Source 1: WorldContextEntity (direct X on world item).  Runs
    // unconditionally -- no popup-root dependency.  Iterates every
    // visible widget looking for the named entity.  Does NOT break
    // early when one widget yields a WorldContextEntity with an
    // un-open menu -- a stale widget during a transition could host
    // the wrong entity, so we keep checking until we find one with
    // an open menu or exhaust the list.
    for (uint32_t i = 0; i < widgetCount; i++) {
        if (!widgets[i] || !widgetVisible[i]) continue;
        if (!ProbeUIElement(static_cast<Noesis::UIElement*>(
                const_cast<Noesis::Visual*>(widgets[i])))) continue;
        auto entity = FindNameInWidgetScoped(
            "WorldContextEntity", widgets[i]);
        if (!entity) continue;
        if (CheckContextMenuOnElement(entity,
                outHighlightedItem, outTextBlock)) {
            return true;
        }
        // Keep looping -- don't break.  Multiple widgets may transiently
        // host a WorldContextEntity during screen transitions, and the
        // wrong one may appear first in widgets[].
    }

    // Source 2: focused element (ActiveSearch item ContextMenu).
    // Also runs unconditionally -- no popup-root dependency.
    if (focusedElement && ProbeUIElement(focusedElement)) {
        if (CheckContextMenuOnElement(
                static_cast<Noesis::FrameworkElement*>(focusedElement),
                outHighlightedItem, outTextBlock)) {
            return true;
        }
    }

    // Source 3: search popup roots for ContextMenuItems directly.
    // This is the only branch that actually needs popupRoots, so the
    // popup scan is lazy -- only runs if Sources 1 and 2 both failed.
    // GetPopupRoots_SEH returns 0 (and this branch is a no-op) if
    // cachedTrueRoot_ is nullptr or no popups exist.
    Noesis::Visual* popupRoots[8];
    auto popupCount = GetPopupRoots_SEH(
        trueRoot, contentChild, popupRoots, 8);
    for (uint32_t pi = 0; pi < popupCount; pi++) {
        Noesis::FrameworkElement* popupMenuItems[16];
        auto popupItemCount = BFS_CollectByType_SEH(
            popupRoots[pi], "ContextMenuItem", popupMenuItems, 0, 16);
        if (FindHighlightedContextMenuItem(
                popupMenuItems, popupItemCount,
                outHighlightedItem, outTextBlock)) {
            return true;
        }
    }

    return false;

  } __except (EXCEPTION_EXECUTE_HANDLER) {
    BG3A_LOG("[BG3Access] PollContextMenu_Unsafe: fault (SEH caught)");
    *outHighlightedItem = nullptr;
    *outTextBlock = nullptr;
    return false;
  }
}

// PollContextMenu: outer wrapper.  Calls the inner function,
// then does std::string text extraction and populates the snapshot.
static void PollContextMenu(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, Noesis::UIElement* focusedElement,
    Noesis::Visual* trueRoot, Noesis::Visual* contentChild,
    ecl::lua::TickSnapshot* snapshot)
{
    static uintptr_t sLastContextMenuHighlightAddr = 0;

    Noesis::FrameworkElement* highlightedItem = nullptr;
    Noesis::FrameworkElement* textBlock = nullptr;

    if (!PollContextMenu_Unsafe(widgets, widgetVisible, widgetCount,
                               focusedElement, trueRoot, contentChild,
                               &highlightedItem, &textBlock)) {
        // Context menu closed or no highlighted item.
        // Reset tracker so re-opening detects the first item.
        if (sLastContextMenuHighlightAddr != 0) {
            BG3A_LOG("[BG3Access] CONTEXT MENU: closed (resetting tracker)");
        }
        sLastContextMenuHighlightAddr = 0;
        return;
    }

    // Change detection: only fire when a different item is highlighted.
    auto highlightAddr = reinterpret_cast<uintptr_t>(highlightedItem);
    if (highlightAddr == sLastContextMenuHighlightAddr) return;
    sLastContextMenuHighlightAddr = highlightAddr;

    // Extract text from the TextBlock child.
    std::string itemText;
    if (textBlock) {
        itemText = ReadTextBlockText(textBlock);
    }

    // Populate snapshot with dedicated context menu fields.
    snapshot->contextMenuChanged = true;
    snapshot->contextMenuItemText = std::move(itemText);

    BG3A_LOG("[BG3Access] CONTEXT MENU: %s",
             snapshot->contextMenuItemText.empty()
                 ? "(empty)" : snapshot->contextMenuItemText.c_str());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// FindTooltipTextBlocks_SEH: scans popup roots for TextBlock elements.
// Skips popup roots that contain ContextMenuItems (those are context menus).
// ---------------------------------------------------------------------------
static uint32_t FindTooltipTextBlocks_SEH(
    Noesis::Visual* trueRoot, Noesis::Visual* contentChild,
    Noesis::FrameworkElement** outTextBlocks, uint32_t maxOut)
{
    uint32_t found = 0;
  __try {
    Noesis::Visual* popupRoots[8];
    auto popupCount = GetPopupRoots_SEH(
        trueRoot, contentChild, popupRoots, 8);
    if (popupCount == 0) return 0;

    for (uint32_t pi = 0; pi < popupCount && found < maxOut; pi++) {
        auto popupRoot = popupRoots[pi];
        if (!popupRoot) continue;

        Noesis::FrameworkElement* cmCheck[1];
        if (BFS_CollectByType_SEH(popupRoot, "ContextMenuItem", cmCheck, 0, 1) > 0)
            continue;

        auto prevFound = found;
        found = BFS_CollectByType_SEH(popupRoot, "TextBlock",
            outTextBlocks, found, maxOut, true, true);

        // If we found TextBlocks in this popup, stop (first tooltip wins).
        if (found > prevFound) break;
    }

    return found;

  } __except (EXCEPTION_EXECUTE_HANDLER) {
    BG3A_LOG("[BG3Access] FindTooltipTextBlocks_SEH: fault");
    return found;
  }
}

// ---------------------------------------------------------------------------
// CollectTooltipEntries_Inner: extract role + text + parentRole +
// fontSize + typeId from each TextBlock in the array into TooltipEntry
// records.
//
// Shared by PollTooltip (tooltip popup path) and the Lua-facing
// ReadElementStructuredTextBlocks primitive -- single source of
// truth for tooltip role extraction.
//
// Uses std::string/std::vector (destructors) so it CANNOT live inside
// __try directly (MSVC C2712).  Every Noesis-touching operation goes
// through an SEH-guarded helper:
//   - ReadTextBlockText     (three-layer inner/invoke/wrapper)
//   - ReadElementName_SEH   (__try-wrapped)
//   - SafeGetVisualParent_SEH  (__try-wrapped field access)
//   - SafeGetFontSize_SEH   (__try-wrapped)
//   - SafeGetDataContext_SEH   (__try-wrapped)
//   - SafeGetClassType_SEH     (__try-wrapped)
//   - ReadTypePropertyAsString (has its own SEH protection)
// No raw Noesis pointer dereferences.
//
// Callers should invoke CollectTooltipEntries_SEH (the outer wrapper
// below) for defense-in-depth against faults outside the helpers'
// coverage (e.g. during std::move/vector growth).
// ---------------------------------------------------------------------------
static void CollectTooltipEntries_Inner(
    Noesis::FrameworkElement** textBlocks, uint32_t textBlockCount,
    std::vector<ecl::lua::TickSnapshot::TooltipEntry>& entries)
{
    for (uint32_t i = 0; i < textBlockCount; i++) {
        auto text = ReadTextBlockText(textBlocks[i]);
        if (text.empty()) continue;
        if (text.find("[ForceUpdate]") != std::string::npos) continue;
        if (text.find("s_HandleUnknown") != std::string::npos) continue;

        ecl::lua::TickSnapshot::TooltipEntry entry;
        entry.text = std::move(text);

        // x:Name of the TextBlock itself.
        {
            static char nameBuf[128];
            nameBuf[0] = 0;
            ReadElementName_SEH(textBlocks[i], nameBuf, sizeof(nameBuf));
            if (nameBuf[0]) entry.role = nameBuf;
        }

        // Parent element x:Name (fallback context).  Use the SEH-safe
        // parent accessor instead of raw mVisualParent dereference --
        // the pointer is same-tick but we still protect every Noesis
        // access per the mandatory SEH rule.
        auto visualParent = SafeGetVisualParent_SEH(textBlocks[i]);
        if (visualParent) {
            static char parentBuf[128];
            parentBuf[0] = 0;
            ReadElementName_SEH(
                static_cast<Noesis::FrameworkElement*>(visualParent),
                parentBuf, sizeof(parentBuf));
            if (parentBuf[0]) entry.parentRole = parentBuf;
        }

        // If element has no x:Name, promote parent to role
        // (preserves existing tooltip behavior).
        if (entry.role.empty() && !entry.parentRole.empty()) {
            entry.role = entry.parentRole;
        }

        // FontSize: distinguishes title from body text when the
        // template has no x:Names (e.g. NameAndDescTooltipContent).
        entry.fontSize = SafeGetFontSize_SEH(textBlocks[i]);

        // TypeId: read from parent container's DataContext for
        // PropertyText entries (distinguishes Range vs ZoneRadius
        // vs other property types).  The parent StackPanel
        // (PropertyContainer) has a DC with TypeId/SubtypeId.
        // Reuse visualParent captured above; no second raw access.
        if (entry.role == "PropertyText" && visualParent) {
            auto parentElem = static_cast<Noesis::FrameworkElement*>(
                visualParent);
            auto parentDC = SafeGetDataContext_SEH(parentElem);
            if (parentDC) {
                auto parentDCType = SafeGetClassType_SEH(parentDC);
                if (parentDCType) {
                    auto const& parentDCClass =
                        Noesis::gClassCache.GetClass(parentDCType);
                    static auto sTypeIdKey = FixedString("TypeId");
                    auto typeIdProp =
                        parentDCClass.Names.try_get(sTypeIdKey);
                    if (typeIdProp && typeIdProp->Property) {
                        auto typeIdStr = ReadTypePropertyAsString(
                            parentDC, typeIdProp->Property);
                        if (!typeIdStr.empty()) {
                            entry.typeId = std::move(typeIdStr);
                        }
                    }
                }
            }
        }

        entries.emplace_back(std::move(entry));
    }
}

// ---------------------------------------------------------------------------
// CollectTooltipEntries_SEH: outer __try wrapper for CollectTooltipEntries
// _Inner.  This function has NO local C++ objects with destructors (the
// `entries` parameter is a reference -- a pointer at the ABI level -- and
// has no scope-end unwinding here), so __try is legal per MSVC C2712.
//
// Same pattern as ReadWidgetTexts_SEH (line ~9170): wrap the destructor-
// heavy inner function in a __try to catch faults that escape individual
// Noesis-helper SEH (e.g. during vector growth or emplace_back).
// ---------------------------------------------------------------------------
static void CollectTooltipEntries_SEH(
    Noesis::FrameworkElement** textBlocks, uint32_t textBlockCount,
    std::vector<ecl::lua::TickSnapshot::TooltipEntry>& entries)
{
    __try {
        CollectTooltipEntries_Inner(textBlocks, textBlockCount, entries);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG(
            "[BG3Access] CollectTooltipEntries_SEH: fault after %d entries",
            (int)entries.size());
    }
}

// PollTooltip: outer wrapper.  Calls the SEH-guarded finder to get
// TextBlock pointers, then reads their text outside SEH (ReadTextBlockText
// uses std::string which has a destructor).  Delta-compares against
// previous tooltip text to avoid re-firing.
// Sends individual texts as an array so Lua can identify title vs description.
//
// focusedItemName / focusedItemDesc: the focused element's DC "Text" and
// "Description" properties.  Used to validate tooltip ownership -- on
// revisit, a recycled popup may briefly show stale template-internal
// TextBlocks.  The item description is the reliable marker: it appears
// in the fully-resolved tooltip but not the stale wave.
static void PollTooltip(
    Noesis::Visual* trueRoot, Noesis::Visual* contentChild,
    ecl::lua::TickSnapshot* snapshot)
{
    static std::string sLastTooltipFingerprint;
    // Reopen stabilization: when a tooltip closes and reopens, a recycled
    // popup may briefly show stale TextBlocks from a previous tooltip.
    // After reopen, require the fingerprint to be stable for one tick
    // before firing.  This adds exactly one tick of delay on reopen only.
    // Also triggered on focus change: tooltip stays open but content
    // swaps to the new element's tooltip, resolving across multiple ticks.
    static bool sWaitingForStable = false;
    static std::string sPendingFingerprint;
    static std::vector<ecl::lua::TickSnapshot::TooltipEntry> sPendingTexts;

    // Focus changed: clear fingerprint so the tooltip content for the
    // new element goes through stabilization instead of firing as
    // immediate deltas while bindings resolve.
    if (sTooltipFingerprintReset) {
        sLastTooltipFingerprint.clear();
        sWaitingForStable = false;
        sPendingFingerprint.clear();
        sPendingTexts.clear();
        sTooltipFingerprintReset = false;
    }

    // Phase 1: find TextBlocks in tooltip popups (SEH-guarded, no C++ objects).
    Noesis::FrameworkElement* textBlocks[32];
    auto textBlockCount = FindTooltipTextBlocks_SEH(
        trueRoot, contentChild, textBlocks, 32);

    if (textBlockCount == 0) {
        if (!sLastTooltipFingerprint.empty()) {
            BG3A_TRACE("[BG3Access] TOOLTIP: closed (resetting tracker)");
        }
        sLastTooltipFingerprint.clear();
        sWaitingForStable = false;
        sPendingFingerprint.clear();
        sPendingTexts.clear();
        return;
    }

    // Phase 2: extract role + text + parentRole + fontSize + typeId
    // from each TextBlock via the shared SEH-wrapped helper (same
    // extraction used by ReadElementStructuredTextBlocks for the
    // inspect-panel path).
    std::vector<ecl::lua::TickSnapshot::TooltipEntry> texts;
    CollectTooltipEntries_SEH(textBlocks, textBlockCount, texts);

    // Build fingerprint for delta-compare from the extracted texts.
    std::string fingerprint;
    for (auto const& entry : texts) {
        if (!fingerprint.empty()) fingerprint += '|';
        fingerprint += entry.text;
    }

    if (texts.empty()) return;

    // Reopen detection: fingerprint was empty (tooltip was closed or
    // focus changed), now we have content.  Enter stabilization: store
    // but don't fire.  Wait for 2 consecutive stable ticks to ensure
    // all popup content (spell info + warning overlays) has resolved.
    static int sStableTickCount = 0;
    if (sLastTooltipFingerprint.empty()) {
        sWaitingForStable = true;
        sStableTickCount = 0;
        sPendingFingerprint = fingerprint;
        sPendingTexts = std::move(texts);
        sLastTooltipFingerprint = fingerprint;
        BG3A_TRACE("[BG3Access] TOOLTIP: reopen detected, waiting for stable (%d texts)",
                 (int)sPendingTexts.size());
        return;
    }

    // Stabilization: waiting for fingerprint to settle after reopen.
    // Require 2 consecutive matching ticks: spell tooltips with warning
    // overlays resolve across multiple ticks (spell info first, then
    // warning popup appears one tick later).
    if (sWaitingForStable) {
        if (fingerprint == sPendingFingerprint) {
            sStableTickCount++;
            if (sStableTickCount < 2) {
                // Not yet stable enough -- wait another tick.
                return;
            }
            // Stable for 2 ticks.  Fire the pending data.
            sWaitingForStable = false;
            sStableTickCount = 0;
            BG3A_TRACE("[BG3Access] TOOLTIP: stable after reopen (%d texts)",
                     (int)sPendingTexts.size());
            // Fall through to normal dispatch with pending data.
            // Update sLastTooltipFingerprint (already set).
            snapshot->tooltipChanged = true;
            snapshot->tooltipTexts = std::move(sPendingTexts);
            sPendingFingerprint.clear();
            sPendingTexts.clear();

            BG3A_TRACE("[BG3Access] TOOLTIP: %d entries", (int)snapshot->tooltipTexts.size());
            for (auto const& entry : snapshot->tooltipTexts) {
                BG3A_TRACE("[BG3Access]   TT: [%s] %s (%.0f)",
                         entry.role.empty() ? "?" : entry.role.c_str(),
                         entry.text.c_str(), entry.fontSize);
            }
            return;
        } else {
            // Fingerprint changed -- bindings still resolving.  Update
            // pending, reset stable count, and wait for a fresh run of
            // consecutive stable ticks.
            sStableTickCount = 0;
            sPendingFingerprint = fingerprint;
            sPendingTexts = std::move(texts);
            sLastTooltipFingerprint = fingerprint;
            BG3A_TRACE("[BG3Access] TOOLTIP: still resolving after reopen (%d texts)",
                     (int)sPendingTexts.size());
            return;
        }
    }

    // Normal path (tooltip already open, not reopening): delta compare.
    if (fingerprint == sLastTooltipFingerprint) return;
    sLastTooltipFingerprint = fingerprint;

    snapshot->tooltipChanged = true;
    snapshot->tooltipTexts = std::move(texts);

    BG3A_TRACE("[BG3Access] TOOLTIP: %d entries", (int)snapshot->tooltipTexts.size());
    for (auto const& entry : snapshot->tooltipTexts) {
        BG3A_TRACE("[BG3Access]   TT: [%s] %s (%.0f)",
                 entry.role.empty() ? "?" : entry.role.c_str(),
                 entry.text.c_str(), entry.fontSize);
    }
}

// Find the widget container: drill down from root following first children
// until we find an element whose children are UIWidgets.
// The BG3 tree structure is: UICanvas -> Viewbox -> Decorator -> Grid -> UIWidget(s)
// but this search is dynamic and doesn't hardcode the depth.
static Noesis::Visual* FindWidgetContainer(Noesis::Visual* root)
{
    if (!sUIWidgetType) return nullptr;
    // BFS search for the visual node whose children are UIWidget instances.
    // The original linear-descent (first-child-only) approach missed menus
    // where the UIWidget wasn't at child index 0 (e.g. character creation
    // after a state transition reorders children).
    //
    // We use a simple iterative approach: check each level's children.
    // If ANY child at a level is a UIWidget, that level's parent is the
    // container.  If none are, descend into children breadth-first.
    std::vector<Noesis::Visual*> queue(64);
    int queueHead = 0;
    int queueTail = 0;
    queue[queueTail++] = root;

    while (queueHead < queueTail) {
        auto node = queue[queueHead++];
        auto childCount = SafeGetVisualChildrenCount_SEH(node);
        for (uint32_t i = 0; i < childCount; i++) {
            auto child = SafeGetVisualChild_SEH(node, i);
            if (!child) continue;
            if (SafeIsUIWidgetType_SEH(child))
                return node;  // This node is the container
        }
        // No UIWidget children at this level -- enqueue children for
        // next level.  Limit total nodes to prevent runaway searches.
        for (uint32_t i = 0; i < childCount && queueTail < 60; i++) {
            auto child = SafeGetVisualChild_SEH(node, i);
            if (child) queue[queueTail++] = child;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// GatherStateMachineWidgets: SEH-guarded access to UIStateMachine.
// Walks the active state tree and collects UIWidget pointers that are NOT
// already in the Canvas-based widget array.  These are widgets on other
// Noesis layers (Pause, MessageBox) like the RT ShortcutsMenu.
//
// Extracted to a standalone function because __try/__except cannot coexist
// with C++ objects that have destructors in the same function.
//
// outWidgets: array to fill with discovered widgets (caller-owned).
// outCount:   set to number of widgets found.
// maxOut:     capacity of outWidgets.
// existingWidgets/existingCount: the Canvas widgets to exclude (already tracked).
// ---------------------------------------------------------------------------
// Strategy 1: Check FocusManager.FocusedElement on focus scopes.
//
// O(1) property read per node -- no tree walk needed when set on widgets.
// Recurses into children only if the property is not set at the current
// node (handles nested focus scopes).
//
// Children are walked in REVERSE order so that overlay/dialog widgets
// (rendered last, displayed on top) are checked before the underlying
// menu widget.
Noesis::UIElement* TryFocusManager(Noesis::Visual* elem, int depth,
                                    Noesis::DependencyObject** outScopeRoot)
{
    if (!elem || depth <= 0) return nullptr;

    // Validate the element pointer before any virtual calls.  A child
    // obtained from GetVisualChild may have been freed by Noesis between
    // frames, leaving a dangling pointer with a corrupt vtable.
    if (!ProbeUIElement(static_cast<Noesis::UIElement*>(elem))) {
        BG3A_TRACE("[BG3Access] TryFocusManager: stale element pointer %p -- skipping subtree", elem);
        return nullptr;
    }

    auto depObj = static_cast<Noesis::DependencyObject const*>(elem);

    // Standard WPF focus manager.
    if (sFocusedElementProp) {
        auto val = sFocusedElementProp->GetValue(depObj);
        if (val) {
            auto focused = *reinterpret_cast<Noesis::UIElement* const*>(val);
            if (focused) {
                // Validate the pointer by probing its vtable.  When the UI
                // destroys an element, the attached property storage may still
                // hold a dangling pointer.  Dereferencing garbage crashes the
                // process.  ProbeUIElement catches access violations safely.
                if (!ProbeUIElement(focused)) {
                    BG3A_TRACE("[BG3Access] TryFocusManager: stale FocusedElement pointer %p -- skipping", focused);
                    return nullptr;
                }
                if (outScopeRoot) *outScopeRoot = const_cast<Noesis::DependencyObject*>(depObj);
                return focused;
            }
        }
    }

    if (!ProbeVisualChildren(elem)) return nullptr;
    auto count = elem->GetVisualChildrenCount();
    for (int i = (int)count - 1; i >= 0; i--) {
        auto child = elem->GetVisualChild(i);
        if (!child) continue;
        auto result = TryFocusManager(child, depth - 1, outScopeRoot);
        if (result) return result;
    }

    return nullptr;
}

// Strategy 2 (safety net): Walk tree checking ls:MoveFocus.IsFocused and IsFocused.
// Only runs when Strategy 1 (FocusedElement fast path) found nothing.
// ls:MoveFocus.IsFocused is Larian's custom controller focus -- checked first.
// Standard IsFocused is fallback for non-ls:MoveFocus elements.
// Reverse order so overlays (rendered last) are checked before underlying menus.
Noesis::UIElement* FindFocusedInTree(Noesis::Visual* elem, int depth)
{
    if (!elem || depth <= 0) return nullptr;

    // Validate before virtual calls -- child from GetVisualChild may be stale.
    if (!ProbeUIElement(static_cast<Noesis::UIElement*>(elem))) return nullptr;

    // Prune invisible branches -- collapsed/hidden subtrees cannot have focus.
    if (!IsVisibleDP(elem)) return nullptr;

    auto depObj = static_cast<Noesis::DependencyObject const*>(elem);

    // Larian controller focus (ls:MoveFocus.IsFocused) -- checked first.
    if (sLSMoveFocusIsFocusedProp) {
        auto val = sLSMoveFocusIsFocusedProp->GetValue(depObj);
        if (val && *static_cast<const bool*>(val)) {
            return static_cast<Noesis::UIElement*>(const_cast<Noesis::Visual*>(elem));
        }
    }

    // Standard WPF keyboard focus (UIElement.IsFocused) -- fallback.
    if (sIsFocusedProp) {
        auto val = sIsFocusedProp->GetValue(depObj);
        if (val && *static_cast<const bool*>(val)) {
            return static_cast<Noesis::UIElement*>(const_cast<Noesis::Visual*>(elem));
        }
    }

    if (!ProbeVisualChildren(elem)) return nullptr;
    auto count = elem->GetVisualChildrenCount();
    for (int i = (int)count - 1; i >= 0; i--) {
        auto child = elem->GetVisualChild(i);
        auto result = FindFocusedInTree(child, depth - 1);
        if (result) return result;
    }

    return nullptr;
}

// Helper: check if an element's class derives from ListBoxItem.
static bool IsListBoxItemType(Noesis::Visual const* elem)
{
    if (!sListBoxItemType) return false;
    auto cls = SafeGetClassType_SEH(elem);
    while (cls) {
        if (cls == sListBoxItemType) return true;
        cls = cls->GetBase();
    }
    return false;
}

// Strategy 3: Walk tree checking Selector.IsSelected, filtered to
// ListBoxItem-derived types only.  Returns FIRST match.
//
// WHY filter by type?  In the Options menu, BOTH the carousel tabs
// (ListBoxItem / LSListBoxItem) AND the content-area options (also
// wrapped in ListBoxItems by their parent Selector) have IsSelected.
// Content options from *previously-visited* tabs keep stale IsSelected
// values.  By returning the FIRST ListBoxItem match and scoping the
// search to the current focus scope (see Tick), we reliably get the
// carousel tab -- which sits above the content area in the visual tree.
//
// Content-area option navigation is handled by Strategies 1+2 (focus).
Noesis::UIElement* FindSelectedTabInTree(Noesis::Visual* elem, int depth)
{
    if (!elem || depth <= 0) return nullptr;

    // Validate before virtual calls -- child from GetVisualChild may be stale.
    if (!ProbeUIElement(static_cast<Noesis::UIElement*>(elem))) return nullptr;

    // Prune invisible branches.
    if (!IsVisibleDP(elem)) return nullptr;

    auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
    auto val = sIsSelectedProp->GetValue(depObj);
    if (val && *static_cast<const bool*>(val)) {
        if (IsListBoxItemType(elem)) {
            return static_cast<Noesis::UIElement*>(const_cast<Noesis::Visual*>(elem));
        }
    }

    if (!ProbeVisualChildren(elem)) return nullptr;
    auto count = elem->GetVisualChildrenCount();
    for (uint32_t i = 0; i < count; i++) {
        auto child = elem->GetVisualChild(i);
        auto result = FindSelectedTabInTree(child, depth - 1);
        if (result) return result;
    }

    return nullptr;
}

// Return the topmost visible UIWidget.  Walks the widget container's
// children in reverse Z-order (topmost = last rendered = checked first).
// Used by Lua timer callbacks that need to find the active overlay/menu
// when GetFocusedElement() might return an element in a background widget.
Noesis::UIElement* GetTopmostWidget()
{
    __try {
        auto root = GetRoot();
        if (!root) return nullptr;

        InitFocusProperties(root);
        auto container = FindWidgetContainer(root);
        if (!container) return nullptr;

        auto count = container->GetVisualChildrenCount();
        for (int i = (int)count - 1; i >= 0; i--) {
            auto child = container->GetVisualChild(i);
            if (!child) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(child))) continue;
            if (IsUIWidgetType(child) && IsVisibleDP(child)) {
                return static_cast<Noesis::UIElement*>(child);
            }
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] GetTopmostWidget: SEH fault");
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// SEH-guarded Noesis read helpers.
// Each function does ONLY raw pointer reads (no C++ objects with destructors)
// inside __try/__except.  Text extraction (std::string) happens in the
// caller, outside the SEH block.
// ---------------------------------------------------------------------------

// ReadFocusManagerDP_SEH: reads FocusManager.FocusedElement attached DP
// from visible widgets.  Returns the focused element, or nullptr.
// ONLY does DP reads (no child enumeration, no tree walking).
// Used for lightweight per-tick expander toggle detection.
static Noesis::UIElement* ReadFocusManagerDP_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount)
{
    __try {
        if (!sFocusedElementProp) return nullptr;
        for (int widgetIndex = (int)widgetCount - 1; widgetIndex >= 0; widgetIndex--) {
            if (!widgets[widgetIndex] || !widgetVisible[widgetIndex]) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(
                    widgets[widgetIndex]))) continue;
            auto depObj = static_cast<Noesis::DependencyObject const*>(
                widgets[widgetIndex]);
            auto val = sFocusedElementProp->GetValue(depObj);
            if (val) {
                auto focused = *reinterpret_cast<Noesis::UIElement* const*>(val);
                if (focused && ProbeUIElement(focused)) return focused;
            }
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// DescendFirstChildAndFindName: shared 5-level first-child descent for
// NameScope lookup.  Walks the first-child chain from a widget root,
// calling FindNodeName at each level.  O(1) hash lookup per level.
// Returns the found FrameworkElement or nullptr.
//
// No SEH block -- callers (FindNameInWidgets_SEH, FindNameInWidget)
// provide their own __try/__except.
// ---------------------------------------------------------------------------
static Noesis::FrameworkElement* DescendFirstChildAndFindName(
    Noesis::Visual* widgetRoot, const char* name)
{
    // Return type is a raw pointer (no destructor), so __try is safe here.
    __try {
        Noesis::Visual* current = widgetRoot;
        for (int depth = 0; depth < 5; depth++) {
            auto childCount = current->GetVisualChildrenCount();
            if (childCount == 0) break;
            auto child = current->GetVisualChild(0);
            if (!child) break;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(child)))
                break;
            auto childFrameworkElement =
                static_cast<Noesis::FrameworkElement*>(child);
            auto found = Noesis::FrameworkElementHelpers::FindNodeName(
                childFrameworkElement, name);
            if (found) {
                return static_cast<Noesis::FrameworkElement*>(found);
            }
            current = child;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// FindNameInWidgets_SEH: like FindNameInWidget but uses a pre-gathered
// widgets array instead of re-discovering the container.  Avoids the
// GetVisualChildrenCount/GetVisualChild calls on the container.
// Descends 5 levels per widget to find the NameScope, then does an O(1)
// hash lookup via FindNodeName.
static Noesis::FrameworkElement* FindNameInWidgets_SEH(
    const char* name,
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount)
{
    __try {
        for (int widgetIndex = (int)widgetCount - 1; widgetIndex >= 0; widgetIndex--) {
            if (!widgets[widgetIndex] || !widgetVisible[widgetIndex]) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(
                    widgets[widgetIndex]))) continue;

            auto found = DescendFirstChildAndFindName(
                widgets[widgetIndex], name);
            if (found) return found;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// FindSelectedInListBox_SEH: finds the IsSelected=true ListBoxItem within
// a ListBox by descending through the template visual tree (Border ->
// ScrollViewer -> ScrollContentPresenter -> ItemsPresenter -> StackPanel)
// and then checking each item.  Shallow descent (8 levels max), NOT a
// full tree walk.  Typically 2-5 dialogue choices checked.
static Noesis::UIElement* FindSelectedInListBox_SEH(
    Noesis::FrameworkElement* listBox)
{
    __try {
        if (!listBox || !sIsSelectedProp || !sListBoxItemType) return nullptr;
        Noesis::Visual* current = listBox;
        for (int depth = 0; depth < 8; depth++) {
            auto childCount = SafeGetVisualChildrenCount_SEH(current);
            if (childCount == 0) break;
            // Check if children at this level are ListBoxItems.
            auto firstChild = SafeGetVisualChild_SEH(current, 0);
            if (!firstChild) break;
            bool childrenAreListBoxItems = false;
            if (sListBoxItemType) {
                auto childClassType = SafeGetClassType_SEH(firstChild);
                while (childClassType) {
                    if (childClassType == sListBoxItemType) {
                        childrenAreListBoxItems = true;
                        break;
                    }
                    childClassType = childClassType->GetBase();
                }
            }
            if (childrenAreListBoxItems) {
                // Found the item panel.  Check each child for IsSelected.
                for (uint32_t itemIndex = 0; itemIndex < childCount; itemIndex++) {
                    auto child = SafeGetVisualChild_SEH(current, itemIndex);
                    if (!child) continue;
                    if (!ProbeUIElement(static_cast<Noesis::UIElement*>(child)))
                        continue;
                    auto depObj = static_cast<Noesis::DependencyObject const*>(
                        child);
                    auto val = sIsSelectedProp->GetValue(depObj);
                    if (val && *static_cast<const bool*>(val)) {
                        return static_cast<Noesis::UIElement*>(child);
                    }
                }
                return nullptr;
            }
            // Not ListBoxItems -- descend into first child.
            current = firstChild;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// DetectWidgetRemoval: finds a widget that was visible in the old set
// but invisible or missing in the new set.  Reads its DC type and name
// via DP reads (no tree walk) and populates removedWidgetData.
// Only reports the FIRST removal per tick.
// Inner function: uses std::string (ReadPropertyAsString), must NOT
// contain __try.
static bool DetectWidgetRemoval_Inner(
    uintptr_t const* oldAddrs, bool const* oldVisible, uint32_t oldCount,
    Noesis::Visual* const* newWidgets, bool const* newVisible, uint32_t newCount,
    ecl::lua::FocusEventData& outRemovedData)
{
    for (uint32_t oldIndex = 0; oldIndex < oldCount; oldIndex++) {
        if (!oldVisible[oldIndex] || oldAddrs[oldIndex] == 0) continue;
        // Check if this widget is still visible in the new set.
        bool stillVisible = false;
        Noesis::Visual* freshPointer = nullptr;
        for (uint32_t newIndex = 0; newIndex < newCount; newIndex++) {
            if (reinterpret_cast<uintptr_t>(newWidgets[newIndex])
                    == oldAddrs[oldIndex]) {
                if (newVisible[newIndex]) {
                    stillVisible = true;
                } else {
                    freshPointer = newWidgets[newIndex];
                }
                break;
            }
        }
        if (stillVisible) continue;
        // Widget was visible, now invisible or gone.
        if (freshPointer) {
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(
                    freshPointer))) continue;
            auto frameworkElement = static_cast<Noesis::FrameworkElement*>(
                freshPointer);
            auto dataContext = SafeReadDC_SEH(
                static_cast<Noesis::DependencyObject const*>(
                    frameworkElement));
            if (dataContext) {
                auto dcTypeName = SafeBaseObjectTypeName_SEH(dataContext);
                if (dcTypeName) {
                    outRemovedData.dcType = dcTypeName;
                }
            }
            outRemovedData.elemName = ReadPropertyAsString(
                frameworkElement, "Name");
            BG3A_TRACE("[BG3Access] Widget removal detected: addr=%p dc=%s name=%s",
                freshPointer,
                outRemovedData.dcType.c_str(),
                outRemovedData.elemName.c_str());
            return true;
        }
        // Widget completely gone (not in new array).
        BG3A_TRACE("[BG3Access] Widget removal detected: addr=0x%llx (gone, no data)",
            (unsigned long long)oldAddrs[oldIndex]);
        outRemovedData.dcType = "Unknown";
        outRemovedData.elemName = "";
        return true;
    }
    return false;
}
// Invoke bridge: POD params only (pointer to FocusEventData).
static bool DetectWidgetRemoval_Invoke(
    uintptr_t const* oldAddrs, bool const* oldVisible, uint32_t oldCount,
    Noesis::Visual* const* newWidgets, bool const* newVisible, uint32_t newCount,
    ecl::lua::FocusEventData* outRemovedData)
{
    return DetectWidgetRemoval_Inner(
        oldAddrs, oldVisible, oldCount,
        newWidgets, newVisible, newCount, *outRemovedData);
}
// SEH wrapper: no C++ objects with destructors.
static bool DetectWidgetRemoval_SEH(
    uintptr_t const* oldAddrs, bool const* oldVisible, uint32_t oldCount,
    Noesis::Visual* const* newWidgets, bool const* newVisible, uint32_t newCount,
    ecl::lua::FocusEventData* outRemovedData)
{
    __try {
        return DetectWidgetRemoval_Invoke(
            oldAddrs, oldVisible, oldCount,
            newWidgets, newVisible, newCount, outRemovedData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] DetectWidgetRemoval_SEH: fault");
        return false;
    }
}

// ReadTrackedWidgets_SEH: reads the event-driven sTrackedWidgets array.
// For each tracked widget, probes validity and reads IsVisibleDP.
// Returns widget count.  No GetVisualChildrenCount/GetVisualChild calls.
static uint32_t ReadTrackedWidgets_SEH(
    Noesis::Visual** outWidgets, bool* outVisible, uint32_t maxWidgets)
{
    __try {
        uint32_t widgetCount = 0;
        for (uint32_t i = 0; i < sTrackedWidgetCount && widgetCount < maxWidgets; i++) {
            auto widget = sTrackedWidgets[i];
            if (!widget) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(widget))) continue;
            outWidgets[widgetCount] = widget;
            outVisible[widgetCount] = IsVisibleDP(widget);
            widgetCount++;
        }
        return widgetCount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] ReadTrackedWidgets_SEH: fault");
        for (uint32_t i = 0; i < maxWidgets; i++) {
            outWidgets[i] = nullptr;
            outVisible[i] = false;
        }
        return 0;
    }
}

// GatherWidgets_SEH: reads widget children from the widget container.
// Returns widget count (0 on failure).  outWidgets and outVisible are
// zeroed on failure.
static uint32_t GatherWidgets_SEH(
    Noesis::Visual* container,
    Noesis::Visual** outWidgets,
    bool* outVisible,
    uint32_t maxWidgets)
{
    __try {
        if (!container) return 0;
        auto count = container->GetVisualChildrenCount();
        uint32_t widgetCount = (count < maxWidgets) ? count : maxWidgets;
        for (uint32_t i = 0; i < widgetCount; i++) {
            outWidgets[i] = container->GetVisualChild(i);
            if (!outWidgets[i]) { outVisible[i] = false; continue; }
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(outWidgets[i]))) {
                outWidgets[i] = nullptr; outVisible[i] = false; continue;
            }
            outVisible[i] = IsVisibleDP(outWidgets[i]);
        }
        return widgetCount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] GatherWidgets_SEH: fault on container %p", container);
        for (uint32_t i = 0; i < maxWidgets; i++) {
            outWidgets[i] = nullptr;
            outVisible[i] = false;
        }
        return 0;
    }
}

// CollectWidgetDCTypes: reads DC type names, widget pointer addresses,
// AND widget x:Names from all visible widgets.  Populates widgetDCTypes
// so Lua's panel close detection always knows which panels are present,
// widgetAddrs so handlers with generic (ls.Widget) top-level DCs can be
// anchored by identity, and widgetNames so generic-DC widgets can be
// routed by x:Name (e.g. JournalCombatLog_c whose runtime DC is the
// generic ls.Widget).  All three vectors stay parallel:
// outDCTypes[i] / outAddrs[i] / outNames[i] describe the same widget.
// Inner/Invoke/SEH pattern because std::vector<std::string> has
// destructors; the additional ReadPropertyAsString call has its own
// internal SEH wrapper (SafeReadPropertyAsString_SEH at line ~7108)
// so a fault reading a single widget's Name returns empty string and
// stays scoped to that widget rather than aborting the whole pass.
static void CollectWidgetDCTypes_Inner(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, std::vector<std::string>& outDCTypes,
    std::vector<std::string>& outAddrs,
    std::vector<std::string>& outNames)
{
    for (uint32_t widgetIndex = 0;
         widgetIndex < widgetCount; widgetIndex++) {
        if (!widgets[widgetIndex]
            || !widgetVisible[widgetIndex]) continue;
        if (!ProbeUIElement(static_cast<Noesis::UIElement*>(
                const_cast<Noesis::Visual*>(
                    widgets[widgetIndex])))) continue;
        auto widgetDC = SafeReadDC_SEH(
            static_cast<Noesis::DependencyObject const*>(
                static_cast<Noesis::FrameworkElement*>(
                    const_cast<Noesis::Visual*>(
                        widgets[widgetIndex]))));
        if (!widgetDC) continue;
        auto widgetDCTypeName =
            SafeBaseObjectTypeName_SEH(widgetDC);
        if (widgetDCTypeName) {
            outDCTypes.push_back(widgetDCTypeName);
            char addrBuf[32];
            snprintf(addrBuf, sizeof(addrBuf), "%p",
                static_cast<void*>(
                    const_cast<Noesis::Visual*>(widgets[widgetIndex])));
            outAddrs.push_back(addrBuf);
            // Read the widget's x:Name on the same probed pointer.
            // ReadPropertyAsString has its own SEH; on fault it
            // returns empty, which we still push so the three
            // parallel vectors stay aligned by index.
            auto widgetName = ReadPropertyAsString(
                static_cast<Noesis::FrameworkElement*>(
                    const_cast<Noesis::Visual*>(widgets[widgetIndex])),
                "Name");
            outNames.push_back(widgetName);
        }
    }
}
static void CollectWidgetDCTypes_Invoke(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, std::vector<std::string>* outDCTypes,
    std::vector<std::string>* outAddrs,
    std::vector<std::string>* outNames) {
    CollectWidgetDCTypes_Inner(
        widgets, widgetVisible, widgetCount,
        *outDCTypes, *outAddrs, *outNames);
}
static void CollectWidgetDCTypes_SEH(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, std::vector<std::string>& outDCTypes,
    std::vector<std::string>& outAddrs,
    std::vector<std::string>& outNames) {
    __try {
        CollectWidgetDCTypes_Invoke(
            widgets, widgetVisible, widgetCount,
            &outDCTypes, &outAddrs, &outNames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] CollectWidgetDCTypes_SEH: fault");
    }
}

// ReadDCAddress_SEH: reads DataContext pointer address from an element.
// Returns the raw address (never dereferences the DC), or 0 on failure.
// Reusable for all DC-based change detection (recycling, DC swaps).
static uintptr_t ReadDCAddress_SEH(Noesis::UIElement* elem)
{
    __try {
        if (!elem || !sDataContextProp) return 0;
        auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
        auto dcVal = sDataContextProp->GetValue(depObj);
        if (!dcVal) return 0;
        auto dataContext = *reinterpret_cast<const void* const*>(dcVal);
        return reinterpret_cast<uintptr_t>(dataContext);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// FindFocusedElement_SEH: runs Strategy 1 (FocusManager.FocusedElement)
// with SEH protection.  Strategy 2 (IsFocused walk) is now in the separate
// FindIsFocused_SEH function, called only when dialogue is visible.
// Returns the focused element, or nullptr on failure/fault.
static Noesis::UIElement* FindFocusedElement_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount,
    Noesis::FrameworkElement* root, int maxDepth,
    Noesis::DependencyObject** outScopeRoot)
{
    *outScopeRoot = nullptr;
    __try {
        // Strategy 1: FocusManager.FocusedElement (fast path)
        Noesis::UIElement* focused = nullptr;
        if (widgetCount > 0) {
            for (int i = (int)widgetCount - 1; i >= 0; i--) {
                if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                focused = TryFocusManager(widgets[i], maxDepth, outScopeRoot);
                if (focused) return focused;
            }
        } else if (root) {
            focused = TryFocusManager(root, maxDepth, outScopeRoot);
            if (focused) return focused;
        }

        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] FindFocusedElement_SEH: fault during focus walk");
        *outScopeRoot = nullptr;
        return nullptr;
    }
}

// FindIsFocused_SEH: runs ONLY Strategy 2 (IsFocused + ls:MoveFocus.IsFocused
// tree walk) with SEH protection.  Used as a per-tick fallback in event-driven
// mode for cases where GotFocus doesn't fire (dialogue choice navigation).
// Only called when a dialogue widget is visible.  Walks visible widgets in
// reverse Z-order (topmost first) to match overlay priority.
// Falls back to walking from root when widgetCount=0 (container lost after
// root change -- dialogue elements exist under root but FindWidgetContainer
// can't find the Canvas during some transitions).
static Noesis::UIElement* FindIsFocused_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount,
    Noesis::FrameworkElement* root,
    int maxDepth, Noesis::DependencyObject** outScopeRoot)
{
    *outScopeRoot = nullptr;
    __try {
        if (!sIsFocusedProp && !sLSMoveFocusIsFocusedProp) return nullptr;
        if (widgetCount > 0) {
            for (int i = (int)widgetCount - 1; i >= 0; i--) {
                if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                auto focused = FindFocusedInTree(widgets[i], maxDepth);
                if (focused) {
                    *outScopeRoot = static_cast<Noesis::DependencyObject*>(widgets[i]);
                    return focused;
                }
            }
        } else if (root) {
            // No widget container -- walk from application root.
            auto focused = FindFocusedInTree(root, maxDepth);
            if (focused) {
                *outScopeRoot = static_cast<Noesis::DependencyObject*>(root);
                return focused;
            }
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] FindIsFocused_SEH: fault during IsFocused walk");
        *outScopeRoot = nullptr;
        return nullptr;
    }
}

// FindSelectedTab_SEH: runs Strategy 3 (IsSelected tree walk) with SEH
// protection.  Returns the selected ListBoxItem, or nullptr on failure/fault.
static Noesis::UIElement* FindSelectedTab_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount,
    Noesis::FrameworkElement* root,
    Noesis::DependencyObject* scopeRoot, int maxDepth)
{
    __try {
        if (!sIsSelectedProp || !sListBoxItemType) return nullptr;

        if (scopeRoot) {
            auto result = FindSelectedTabInTree(
                static_cast<Noesis::Visual*>(scopeRoot), maxDepth);
            if (result) return result;
        }
        if (widgetCount > 0) {
            for (int i = (int)widgetCount - 1; i >= 0; i--) {
                if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                auto result = FindSelectedTabInTree(widgets[i], maxDepth);
                if (result) return result;
            }
        }
        return FindSelectedTabInTree(root, maxDepth);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] FindSelectedTab_SEH: fault during selection tree walk");
        return nullptr;
    }
}

// Find a named element within visible widgets by entering their NameScopes.
//
// UIWidget elements sit ABOVE the XAML template's NameScope boundary, so
// calling FindNodeName from a UIWidget (or anything above it) returns null
// for names defined inside the template.  This function walks INTO each
// visible widget's visual children to reach an element INSIDE the NameScope,
// then calls FindNodeName from there.
//
// Searches all visible widgets in reverse Z-order (topmost first).
// Returns the first match, or nullptr if not found in any widget.
//
// Typical visual tree inside a widget:
//   UIWidget  (above NameScope)
//     -> ContentPresenter  (template boundary)
//       -> Grid "Root"  (NameScope owner -- FindNodeName works here)
//         -> ... all XAML content with x:Name elements
Noesis::FrameworkElement* FindNameInWidget(char const* name)
{
    __try {
        auto root = GetRoot();
        if (!root) return nullptr;

        InitFocusProperties(root);
        auto container = FindWidgetContainer(root);
        if (!container) return nullptr;

        auto widgetCount = container->GetVisualChildrenCount();

        for (int wi = (int)widgetCount - 1; wi >= 0; wi--) {
            auto widget = container->GetVisualChild(wi);
            if (!widget) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(widget))) continue;
            if (!IsUIWidgetType(widget) || !IsVisibleDP(widget))
                continue;

            auto found = DescendFirstChildAndFindName(widget, name);
            if (found) return found;
        }

        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] FindNameInWidget: SEH fault for '%s'", name ? name : "(null)");
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// QueryNamedElement: find a named element and return ALL useful data as
// a Lua table.  Generic -- no CC or menu knowledge.  Lua passes the
// x:Name, C++ returns dcType, elemType, elemText, isVisible, elemAddr.
// Returns nil if element not found.
// ---------------------------------------------------------------------------
// QueryNamedElement_Inner: extracts all useful data from a named element.
// Uses std::string (ReadTextBlockText, ReadPropertyAsString) -- must NOT
// be inside __try (MSVC C2712).
static void QueryNamedElement_Inner(lua_State* L,
    Noesis::FrameworkElement* elem, int propsTableIndex)
{
    lua_newtable(L);

    // elemType
    auto elemTypeName = SafeBaseObjectTypeName_SEH(elem);
    if (elemTypeName) {
        lua_pushstring(L, "elemType");
        lua_pushstring(L, elemTypeName);
        lua_settable(L, -3);
    }

    // elemAddr
    {
        char addrBuf[20];
        snprintf(addrBuf, sizeof(addrBuf), "%p", elem);
        lua_pushstring(L, "elemAddr");
        lua_pushstring(L, addrBuf);
        lua_settable(L, -3);
    }

    // dcType
    auto dc = SafeReadDC_SEH(
        static_cast<Noesis::DependencyObject const*>(elem));
    if (dc) {
        auto dcTypeStr = SafeBaseObjectTypeName_SEH(dc);
        if (dcTypeStr) {
            lua_pushstring(L, "dcType");
            lua_pushstring(L, dcTypeStr);
            lua_settable(L, -3);
        }
    }

    // isVisible
    lua_pushstring(L, "isVisible");
    lua_pushboolean(L, IsVisibleDP(elem));
    lua_settable(L, -3);

    // elemText (for TextBlocks)
    auto text = ReadTextBlockText(elem);
    if (!text.empty()
        && text.find("[ForceUpdate]") == std::string::npos) {
        lua_pushstring(L, "elemText");
        lua_pushstring(L, text.c_str());
        lua_settable(L, -3);
    }

    // Optional property reads: if Lua passed a table of property
    // names as arg 2, read each via ReadPropertyAsString and
    // include directly on the result table.
    if (propsTableIndex > 0 && lua_istable(L, propsTableIndex)) {
        int propsLen = (int)lua_rawlen(L, propsTableIndex);
        for (int pi = 1; pi <= propsLen; pi++) {
            lua_rawgeti(L, propsTableIndex, pi);
            if (lua_isstring(L, -1)) {
                const char* propName = lua_tostring(L, -1);
                auto propVal = ReadPropertyAsString(elem, propName);
                if (!propVal.empty()) {
                    lua_pushstring(L, propName);
                    lua_pushstring(L, propVal.c_str());
                    lua_settable(L, -4);  // result table
                }
            }
            lua_pop(L, 1);  // pop the property name from rawgeti
        }
    }
}

// QueryNamedElement_Invoke: no destructors, SEH-compatible.
static void QueryNamedElement_Invoke(lua_State* L,
    Noesis::FrameworkElement* elem, int propsTableIndex)
{
    QueryNamedElement_Inner(L, elem, propsTableIndex);
}

// QueryNamedElement(name [, propsTable]): find a named element across
// visible widgets and return a table with all useful data.
// Generic -- no CC or menu knowledge.
//   name: x:Name string to find
//   propsTable: optional array of property name strings to read
// Returns table {elemType, elemAddr, dcType, isVisible, elemText,
//   props={propName=value, ...}} or nil if not found.
UserReturn QueryNamedElement(lua_State* L)
{
    auto name = luaL_checkstring(L, 1);
    if (!name || !name[0]) {
        lua_pushnil(L);
        return 1;
    }

    int propsTableIndex = lua_istable(L, 2) ? 2 : 0;

    auto elem = FindNameInWidget(name);
    if (!elem) {
        lua_pushnil(L);
        return 1;
    }

    // Record stack top before the call.  If _Invoke faults
    // partway through (table partially built), restore the
    // stack to its pre-call state and push nil instead.
    int stackTop = lua_gettop(L);

    __try {
        QueryNamedElement_Invoke(L, elem, propsTableIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_settop(L, stackTop);
        lua_pushnil(L);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// FindNameInWidgetScoped: like FindNameInWidget but searches a SINGLE widget
// element instead of all visible widgets.  The widget parameter is the
// ls.UIWidget (or ls.DCWidget) Visual* received from the WidgetAdded callback,
// or any element whose subtree should be searched.
//
// BFS through the first 5 levels of the visual tree, calling FindNodeName
// at each node.  FindNodeName is O(1) (hash lookup in the NameScope), so
// the cost is negligible.  The BFS ensures we find the NameScope owner
// even when it is not on the first-child path (e.g. content widgets where
// the NameScope root is a sibling, not the first child).  5 levels matches
// the depth used by the global FindNameInWidget's first-child walk.
//
// Returns the found FrameworkElement or nullptr.
// ---------------------------------------------------------------------------
// Inner SEH-safe function for FindNameInWidgetScoped.
// Uses a fixed-size array instead of std::vector (no C++ destructors).
static Noesis::FrameworkElement* FindNameInWidgetScoped_Unsafe(
    char const* name, Noesis::Visual* widget)
{
    __try {
        auto widgetFE = static_cast<Noesis::FrameworkElement*>(widget);
        auto found = Noesis::FrameworkElementHelpers::FindNodeName(widgetFE, name);
        if (found) return static_cast<Noesis::FrameworkElement*>(found);

        // BFS through visual children, max 5 levels deep, max 256 nodes.
        Noesis::Visual* queue[256];
        int front = 0, back = 0;

        auto seedCount = widget->GetVisualChildrenCount();
        for (uint32_t i = 0; i < seedCount && back < 256; i++) {
            auto child = widget->GetVisualChild(i);
            if (child) queue[back++] = child;
        }

        for (int level = 0; level < 5 && front < back; level++) {
            int levelEnd = back;
            while (front < levelEnd) {
                auto cur = queue[front++];
                if (!ProbeUIElement(static_cast<Noesis::UIElement*>(cur))) continue;
                auto curFE = static_cast<Noesis::FrameworkElement*>(cur);
                found = Noesis::FrameworkElementHelpers::FindNodeName(curFE, name);
                if (found) return static_cast<Noesis::FrameworkElement*>(found);

                auto childCount = cur->GetVisualChildrenCount();
                for (uint32_t i = 0; i < childCount && back < 256; i++) {
                    auto child = cur->GetVisualChild(i);
                    if (child) queue[back++] = child;
                }
            }
        }

        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] FindNameInWidgetScoped: SEH fault for '%s'",
             name ? name : "(null)");
        return nullptr;
    }
}

Noesis::FrameworkElement* FindNameInWidgetScoped(char const* name,
                                                  Noesis::Visual* widget)
{
    if (!name || !widget) return nullptr;
    return FindNameInWidgetScoped_Unsafe(name, widget);
}

// Diagnostic: dump all ViewModel properties from visible widgets.
// Enumerates every TypeProperty on each widget's DataContext, printing
// name and type.  For string properties (const char*), also prints the
// value.  This reveals what the game's ViewModels actually expose, so
// we can read content directly from the data layer instead of guessing
// at named UI elements.
//
// Temporary test function -- delete after validation.


// Combined: try FocusManager fast path, then IsFocused tree walk, then IsSelected.
Noesis::UIElement* GetFocusedElement()
{
    __try {
        auto root = GetRoot();
        if (!root) return nullptr;

        InitFocusProperties(root);

        // Strategy 1: FocusManager.FocusedElement fast path
        auto focused = TryFocusManager(root, GlobalFocusMonitor::kMaxTreeDepth);
        if (focused) return focused;

        // Strategy 2: IsFocused + ls:MoveFocus.IsFocused tree walk
        if (sIsFocusedProp || sLSMoveFocusIsFocusedProp) {
            focused = FindFocusedInTree(root, GlobalFocusMonitor::kMaxTreeDepth);
            if (focused) return focused;
        }

        // Strategy 3: IsSelected tree walk (ListBoxItem tabs)
        if (sIsSelectedProp && sListBoxItemType) {
            auto selected = FindSelectedTabInTree(root, GlobalFocusMonitor::kMaxTreeDepth);
            if (selected) return selected;
        }

        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] GetFocusedElement: SEH fault");
        return nullptr;
    }
}

bg3se::ui::UIStateMachine* GetStateMachine()
{
    // Stubbed out.  The ls.StateMachine instance is not a direct field
    // in UIManager or EoCClient -- it's a XAML-instantiated Noesis
    // component.  Searching for it via the Noesis tree from Lua instead.
    return nullptr;
}

using FireStateEventProc = void(bg3se::ui::UIStateMachine*, bg3se::ui::UIStateMachine::EventResult&, bg3se::ui::UIStateMachine::EntityContext const&, bg3se::ui::UIStateMachine::EventArgs const&);

void SetState(lua_State* L, FixedString state, std::optional<FixedString> subState, std::optional<bool> clearState, std::optional<int16_t> playerId)
{
    ERR("Ext.UI.SetState(): Deprecated");
}

bool RegisterType(lua_State* L, StringView name, HashMap<FixedString, bg3se::ui::CustomPropertyDefn> properties,
    std::optional<StringView> wrappedContextType)
{
    Noesis::gStaticSymbols.Initialize();
    auto clsName = ClassDefinitionBuilder::MakeFullName(name);

    // Fixup names
    for (auto& prop : properties) {
        prop.Value().Name = prop.Key();
    }

    // Name conflicts with an existing Noesis type?
    if (Noesis::Reflection::GetType(clsName) != nullptr) {
        
        auto dynClass = gDynamicClasses.try_get(FixedString(clsName.Str()));
        if (!dynClass) {
            // Not an SE type, cannot replace
            luaL_error(L, "A Noesis type already exists with this name: %s", name.data());
            return false;
        }

        // If the definition didn't change, just replace the handlers without modifying the class defn
        if ((*dynClass)->MatchesDefinition(properties, wrappedContextType)) {
            (*dynClass)->UpdateHandlers(properties);
            return true;
        }

        if (gExtender->GetConfig().DeveloperMode) {
            WARN("Re-registering Noesis type '%s' with different definition - this is only supported in developer mode!", clsName.Str());
        } else {
            luaL_error(L, "Attempted to re-register Noesis type '%s' with different definition", clsName.Str());
            return false;
        }
    }

    return ClassDefinitionBuilder::RegisterNew(L, clsName, properties, wrappedContextType);
}

Noesis::BaseComponent* Instantiate(lua_State* L, STDString name, std::optional<Noesis::BaseComponent*> wrappedContext)
{
    if (name.substr(0, 4) != "se::") {
        name = "se::" + name;
    }

    auto cls = gDynamicClasses.try_get(FixedString(name));
    if (!cls) {
        luaL_error(L, "No custom class found with name '%s'", name.c_str());
        return nullptr;
    }

    auto inst = (*cls)->Construct(wrappedContext.value_or(nullptr));
    if (!inst) {
        luaL_error(L, "Unable to construct data context '%s' - invalid parameters", name.c_str());
        return nullptr;
    }

    return inst;
}

PlayerPickingHelper* GetPickingHelper(uint16_t playerIndex)
{
    auto picking = ecl::ExtensionState::Get().GetClientLua()->GetEntitySystemHelpers()->GetSystem<ecl::PickingHelperManager>();
    auto it = picking->PlayerHelpers.find(playerIndex);
    if (it != picking->PlayerHelpers.end()) {
        return it.Value();
    }
    else {
        return nullptr;
    }
}

ecl::CursorControl* GetCursorControl()
{
    auto cc = GetStaticSymbols().ecl__gCursorControl;
    if (cc && *cc) {
        return *cc;
    } else {
        return nullptr;
    }
}

ecl::PlayerDragData* GetDragDrop(uint16_t playerId)
{
    auto dragDrop = GetStaticSymbols().ls__gDragDropManager;
    if (dragDrop && *dragDrop) {
        return (*dragDrop)->PlayerData.try_get(playerId);
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Accessibility Lua API functions
// ---------------------------------------------------------------------------

// Legacy SubscribePropertyChanged, UnsubscribePropertyChanged,
// SubscribeDPChanged, UnsubscribeDPChanged deleted -- snapshot handles everything.

bool SubscribeGlobalFocusChanged(lua_State* L, lua::RegistryEntry callback)
{
    auto root = GetRoot();
    if (!root) return false;
    InitFocusProperties(root);
    return GlobalFocusMonitor::Instance().Subscribe(L, std::move(callback));
}

void UnsubscribeGlobalFocusChanged()
{
    GlobalFocusMonitor::Instance().Unsubscribe();
}

void ForceGlobalFocusUpdate()
{
    GlobalFocusMonitor::Instance().ForceNextFire();
}

void SuppressGlobalFocusTick(bool suppress)
{
    GlobalFocusMonitor::Instance().SetSuppressTick(suppress);
}

void SetDialoguePollActive(bool active)
{
    GlobalFocusMonitor::Instance().dialogPollActive_ = active;
    BG3A_LOG("[BG3Access] SetDialoguePollActive(%s)", active ? "true" : "false");
}

// SetTraceLogging: runtime toggle for BG3A_TRACE-gated noise
// (widget enumeration, NameScope reads, tick dumps, etc.).  Lua calls
// this from Logger.lua's CycleLogLevel so L3+R3 (in-game chord) or
// `bg3a_log` (SE console command) flips Lua's Log.Debug and C++
// BG3A_TRACE together.  No-op in release builds where BG3ACCESS_VERBOSE
// isn't defined (the underlying flag doesn't exist).
void SetTraceLogging(bool enabled)
{
#ifdef BG3ACCESS_VERBOSE
    sBG3A_TraceEnabled = enabled;
    BG3A_LOG("[BG3Access] SetTraceLogging(%s)",
             enabled ? "true" : "false");
#else
    (void)enabled;
#endif
}

bool HasProperty(Noesis::BaseObject const* o, bg3se::FixedString const& name)
{
    __try {
        auto const& cls = gClassCache.GetClass(o->GetClassType());
        return cls.Names.try_get(name) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Check if a DependencyProperty has been explicitly set (locally) on a
// specific element instance.  Returns true if the property was written
// in XAML or set programmatically on THIS element, false if it's using
// the registered default or an inherited value.
//
// Primary use: detecting whether Larian attached ls:MoveFocus.IsFocused
// to an element (making it part of the controller focus system) vs
// static containers that never receive focus.  This is the universal,
// property-based alternative to name-based checks like Name:find("Button").
//
// Searches DependencyObject::mValues directly.  Both type-owned DPs and
// attached DPs end up in mValues when set locally (via XAML or code).
// This avoids DynamicCast<DependencyProperty*> and GetLocalValue(), both
// of which require symbols not exported from the Noesis Indie SDK.
bool HasLocalValue(Noesis::BaseObject* target, bg3se::FixedString const& propName)
{
    __try {
        if (!target) return false;
        auto depObj = static_cast<Noesis::DependencyObject*>(target);
        if (!depObj) return false;

        Noesis::Symbol sym(propName.GetString());
        for (auto& prop : depObj->mValues) {
            if (prop.key && prop.key->GetName() == sym) {
                return true;
            }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Walks the LOGICAL parent chain from the element up to the root,
// checking the Visibility DP on each ancestor via GetValue() (which
// resolves triggers, styles, and default values -- not just mValues).
//
// Returns false if the element itself or ANY ancestor has effective
// Visibility="Collapsed" or "Hidden".
//
// Uses the logical tree (FrameworkElement::mParent) instead of the
// visual tree (Visual::mVisualParent) because Collapsed containers
// SEVER the visual tree for their children -- mVisualParent terminates
// after 2-3 hops without reaching the Collapsed ancestor.  The logical
// tree is always intact regardless of Visibility state.
//
// Logical parent chains are short (5-8 hops to widget root) compared
// to visual tree chains (20+ hops through template internals).
//
// Exposed to Lua as Ext.UI.IsElementVisible(element).
bool IsElementVisible(Noesis::BaseObject* target)
{
    __try {
        if (!sVisibilityProp) {
            if (!sIsVisibleProp) return true;
            auto depObj = static_cast<Noesis::DependencyObject*>(target);
            if (!depObj) return true;
            auto val = sIsVisibleProp->GetValue(depObj);
            return !val || *static_cast<const bool*>(val);
        }

        auto cls = target->GetClassType();
        if (!Noesis::TypeHelpers::IsDescendantOf(
                cls, Noesis::gStaticSymbols.TypeClasses.FrameworkElement.Type)) {
            return true;
        }

        auto ancestor = static_cast<Noesis::FrameworkElement*>(target);
        constexpr int kMaxDepth = 32;
        for (int i = 0; i < kMaxDepth && ancestor; i++) {
            auto depObj = static_cast<Noesis::DependencyObject*>(ancestor);
            auto val = sVisibilityProp->GetValue(depObj);
            if (val) {
                auto vis = *static_cast<const Noesis::Visibility*>(val);
                if (vis != Noesis::Visibility_Visible) return false;
            }
            ancestor = ancestor->mParent;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;  // Assume visible on fault (safe default).
    }
}

// Checks whether an element is connected to the live visual tree by
// walking mVisualParent up to a UIWidget ancestor.  Collapsed containers
// sever the visual tree: children exist in the NameScope but their
// VisualParent chain terminates without reaching a UIWidget.  Connected
// elements always reach one.
//
// No DP reads, no type reflection beyond IsUIWidgetType -- just pointer
// chasing.  O(depth) where depth is typically 8-15 for connected elements,
// 2-3 for detached ones.
//
// Exposed to Lua as Ext.UI.IsConnectedToWidget(element).
bool IsConnectedToWidget(Noesis::BaseObject* target)
{
    __try {
        auto cls = target->GetClassType();
        if (!Noesis::TypeHelpers::IsDescendantOf(
                cls, Noesis::gStaticSymbols.TypeClasses.Visual.Type)) {
            return false;
        }

        auto visual = static_cast<Noesis::Visual*>(target);
        constexpr int kMaxDepth = 64;
        for (int i = 0; i < kMaxDepth; i++) {
            visual = visual->mVisualParent;
            if (!visual) return false;
            if (IsUIWidgetType(visual)) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Returns the computed IsHitTestVisible state via DependencyProperty
// value lookup.  Same pattern as IsVisibleDP -- avoids calling
// UIElement::GetIsHitTestVisible() which is declared in the header
// but NOT exported from the Noesis Indie SDK DLL.
//
// Used to filter false positives in HasNavigableContent: Larian hides
// inactive tab content with IsHitTestVisible=false rather than
// Visibility=Collapsed (to avoid layout recalculation).
bool IsHitTestVisible(Noesis::BaseObject* target)
{
    __try {
        if (!sIsHitTestVisibleProp) return true;
        auto depObj = static_cast<Noesis::DependencyObject*>(target);
        if (!depObj) return true;
        auto val = sIsHitTestVisibleProp->GetValue(depObj);
        return !val || *static_cast<const bool*>(val);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// Returns the DataContext of a FrameworkElement by reading the
// DependencyProperty directly.  This bypasses the Lua bridge's
// StoredValue path which returns nil for inherited/expression DP
// values (DataContext is inherited from parent ItemsControl, so
// GetProperty("DataContext") from Lua always returns nil).
//
// The C++ DependencyProperty::GetValue() reads the resolved
// effective value -- same approach GlobalFocusMonitor uses for
// carousel recycling detection (DataContext pointer comparison).
Noesis::BaseComponent* GetDataContext(Noesis::BaseObject* target)
{
    __try {
        if (!target) return nullptr;

        auto root = GetRoot();
        if (root) InitFocusProperties(root);
        if (!sDataContextProp) return nullptr;

        auto depObj = static_cast<Noesis::DependencyObject*>(target);
        if (!depObj) return nullptr;

        auto dcVal = sDataContextProp->GetValue(depObj);
        if (!dcVal) return nullptr;

        return *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Loading tip buffering: reads tips DURING loading states when the Lua VM
// is dead.  The loading screen widget is stable (not rebuilt by the loading
// thread), so targeted reads on it are safe.
//
// The buffer persists across VM resets.  On Subscribe() (new Lua VM), any
// buffered tips are delivered as a synthetic WidgetAdded snapshot with
// _loadingHint_N keys in namedTexts.
// ---------------------------------------------------------------------------

// Inner: find the loading screen widget, read the currently visible hint.
// Uses std::string (destructor), cannot contain __try.
static void BufferLoadingTips_Inner()
{
    // Skip if tips were already delivered this loading cycle.
    // tipsAlreadyDelivered_ is reset by SetSuppressTick(true).
    if (GlobalFocusMonitor::Instance().TipsAlreadyDelivered()) return;

    auto root = GetRoot();
    if (!root) return;

    // Ensure focus properties are initialized (needed for DC reads).
    InitFocusProperties(root);

    // Find widget container (same as Tick()).
    auto container = FindWidgetContainer(root);
    if (!container) return;

    auto widgetCount = SafeGetVisualChildrenCount_SEH(container);

    // Find the loading screen widget by DC type.
    Noesis::FrameworkElement* loadingWidget = nullptr;
    for (int widgetIndex = (int)widgetCount - 1; widgetIndex >= 0; widgetIndex--) {
        auto widget = SafeGetVisualChild_SEH(container, widgetIndex);
        if (!widget) continue;
        if (!ProbeUIElement(static_cast<Noesis::UIElement*>(widget))) continue;

        auto widgetDC = SafeReadDC_SEH(
            static_cast<Noesis::DependencyObject const*>(widget));
        if (!widgetDC) continue;
        auto dcTypeName = SafeBaseObjectTypeName_SEH(widgetDC);
        if (dcTypeName && strstr(dcTypeName, "LoadingScreen")) {
            loadingWidget = static_cast<Noesis::FrameworkElement*>(widget);

            // Check whether the hint index changed since the last read.
            // Two sources: (1) direct read of VisibileHintIndex here
            // during safe states, (2) the INPC flag set by
            // OnWidgetINPCChanged during any state (including dangerous).
            // The INPC flag bypasses the index dedup because INPC already
            // confirmed the property changed.
            bool hintChangedViaINPC =
                GlobalFocusMonitor::Instance().ConsumeHintINPCFlag();
            if (!hintChangedViaINPC) {
                auto dcClassType = SafeGetClassType_SEH(widgetDC);
                if (!dcClassType) break;
                auto const& cls =
                    Noesis::gClassCache.GetClass(dcClassType);
                bg3se::FixedString fsHintIndex("VisibileHintIndex");
                auto hintIndexProp = cls.Names.try_get(fsHintIndex);
                if (hintIndexProp && hintIndexProp->Property) {
                    auto indexStr = ReadTypePropertyAsString(
                        widgetDC, hintIndexProp->Property);
                    int hintIndex = indexStr.empty()
                        ? -1 : atoi(indexStr.c_str());
                    if (hintIndex < 0
                        || hintIndex == sLastBufferedHintIndex) {
                        return;
                    }
                    sLastBufferedHintIndex = hintIndex;
                }
            }
            break;
        }
    }

    if (!loadingWidget) return;

    // Read the current hint text from the LoadingHints ItemsControl.
    std::vector<std::pair<std::string, std::string>> hintTexts;
    GlobalFocusMonitor::CollectLoadingHints(loadingWidget, hintTexts);

    for (auto& hint : hintTexts) {
        // Dedup against both the pending buffer and the persistent
        // delivered set.  Prevents re-buffering tips that were already
        // spoken in a previous loading phase.
        bool duplicate = false;
        for (auto& existing : sBufferedLoadingTips) {
            if (existing == hint.second) { duplicate = true; break; }
        }
        if (!duplicate) {
            for (auto& delivered : sDeliveredTipTexts) {
                if (delivered == hint.second) {
                    duplicate = true; break;
                }
            }
        }
        if (!duplicate) {
            BG3A_LOG("[BG3Access] Buffered loading tip: %s",
                     hint.second.c_str());
            sBufferedLoadingTips.push_back(std::move(hint.second));
        }
    }
}

// SEH wrapper.
static void BufferLoadingTips_SEH()
{
    __try {
        BufferLoadingTips_Inner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] BufferLoadingTips: SEH fault");
    }
}

// Free functions wrapping GlobalFocusMonitor singleton -- called from
// LuaClient.cpp which is a different translation unit.
void TickGlobalFocusMonitor()
{
    // Do NOT tick during loading/teardown states.  Noesis UI elements are
    // torn down and rebuilt during loads; walking stale element pointers can
    // hang the thread (pointer lands on memory the loader is paging in, so
    // SEH never triggers -- it's a deadlock, not a fault).
    //
    // EXCEPTION: the loading screen widget itself is stable (not rebuilt
    // by the loading thread).  BufferLoadingTips reads ONLY from this
    // widget to capture tip text during the VM-dead period.
    auto clientState = GetStaticSymbols().GetClientState();
    if (clientState) {
        switch (*clientState) {
        // DANGEROUS: loading thread actively modifies Noesis objects.
        // Any virtual call can deadlock against the loader's mutex.
        case ecl::GameState::SwapLevel:
        case ecl::GameState::LoadLevel:
        case ecl::GameState::LoadModule:
        case ecl::GameState::LoadSession:
        case ecl::GameState::UnloadLevel:
        case ecl::GameState::UnloadModule:
        case ecl::GameState::UnloadSession:
            return;

        // SAFE: loading thread is not yet active or already done.
        // The loading screen widget is stable, OK to read tips.
        case ecl::GameState::LoadMenu:
        case ecl::GameState::StartLoading:
        case ecl::GameState::StopLoading:
        case ecl::GameState::StartServer:
            BufferLoadingTips_SEH();
            return;
        default:
            break;
        }
    }

    // Top-level SEH guard: catches ANY Noesis crash in the entire Tick()
    // body, including deeply nested tree walks, DC reads, and text extraction.
    // Individual sections have their own SEH for diagnostics; this is the
    // safety net that prevents the game from dying.
    __try {
        GlobalFocusMonitor::Instance().Tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] CRASH in Tick() caught by top-level SEH -- frame skipped");
    }
}

void ResetGlobalFocusMonitor()
{
    GlobalFocusMonitor::Instance().Reset();
    // Force re-resolution of dependency properties on next tick.
    // Needed because some types (FocusManager) may not be in the
    // reflection system when InitFocusProperties first runs.
    sFocusPropsInitialized = false;
}

// Returns true if the element has ls:MoveFocus.Focusable=true.
// This is the universal marker for Larian controller interactivity.
// Set via Style setters (FocusableContentControlStyle variants), so
// it's NOT in mValues (local values).  GetValue returns the effective
// value including style setters, which is exactly what we need.
//
// Used by Lua text gatherers to skip interactive elements whose labels
// are spoken on focus, not dumped as body text.
bool IsMoveFocusFocusable(Noesis::BaseObject* target)
{
    __try {
        if (!sLSMoveFocusFocusableProp) return false;
        auto depObj = static_cast<Noesis::DependencyObject*>(target);
        if (!depObj) return false;
        auto val = sLSMoveFocusFocusableProp->GetValue(depObj);
        return val && *static_cast<const bool*>(val);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Phase 1: C++ data extraction functions for the accessibility refactor.
//
// These functions read Noesis property values WITHOUT going through Lua,
// returning std::string or pushing Lua tables directly.  They form the
// foundation for ExtractElementInfo / GetFocusedElementInfo which will
// eventually replace the current approach of passing Noesis objects to Lua.
// ---------------------------------------------------------------------------

// Unwrap TypeConst to get the underlying content type.
static Noesis::Type const* UnwrapType(Noesis::Type const* type)
{
    auto& types = Noesis::gStaticSymbols.Types;
    while (type && type->GetClassType() == types.TypeConst.Type) {
        type = static_cast<Noesis::TypeConst const*>(type)->GetContentType();
    }
    return type;
}

// ---------------------------------------------------------------------------
// ConvertRawValueToString: shared value-to-string conversion for all
// Noesis scalar types.  Given a raw pointer to stored data and the
// unwrapped Noesis type, returns the string representation.
//
// Handles: String, CStringPtr, LocaString, Bool, Int32, UInt32, Int64,
// Single, Double, Enum.  Returns empty for unrecognized types (Ptr, object).
//
// Pointer semantics (same for TypeProperty::Get and StoredValue):
//   rawVal points to where the data lives:
//   - String: rawVal is Noesis::String const*
//   - CStringPtr: rawVal is char const* const* (pointer to the char pointer)
//   - LocaString: rawVal is TranslatedString const*
//   - Bool/Int/Float: rawVal points to the scalar value
//   - Enum: rawVal points to the enum integer (read as int64_t)
//
// Float/Double use %g (matches TypeProperty behavior).
// Enum values are resolved to symbolic names via TypeEnum::mValues.
// Unregistered enum values are logged once per (type, value) pair.
// ---------------------------------------------------------------------------
// _Inner: dereferences raw Noesis data pointers that could be stale.
// Uses std::string (destructor) -- cannot live inside __try.
static std::string ConvertRawValueToString_Inner(void const* rawVal,
                                                  Noesis::Type const* type)
{
    if (!rawVal || !type) return {};
    auto& types = Noesis::gStaticSymbols.Types;

    if (type == types.String.Type) {
        auto str = reinterpret_cast<Noesis::String const*>(rawVal);
        if (str && str->Size() > 0) return str->Str();
    } else if (type == types.CStringPtr.Type) {
        auto charPtrStorage = reinterpret_cast<char const* const*>(rawVal);
        if (charPtrStorage) {
            auto charPtr = *charPtrStorage;
            if (charPtr && charPtr[0] != '\0') return charPtr;
        }
    } else if (type == types.LocaString.Type) {
        auto translatedString = reinterpret_cast<TranslatedString const*>(rawVal);
        if (translatedString) {
            auto resolved = translatedString->Get();
            if (resolved && !resolved->empty()
                && resolved->find("s_HandleUnknown") == std::string::npos) {
                return std::string(resolved->data(), resolved->size());
            }
        }
    } else if (type == types.Bool.Type) {
        return *static_cast<bool const*>(rawVal) ? "On" : "Off";
    } else if (type == types.Int32.Type) {
        return std::to_string(*static_cast<int32_t const*>(rawVal));
    } else if (type == types.UInt32.Type) {
        return std::to_string(*static_cast<uint32_t const*>(rawVal));
    } else if (type == types.Int64.Type) {
        return std::to_string(*static_cast<int64_t const*>(rawVal));
    } else if (type == types.Single.Type) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%g",
                 *static_cast<float const*>(rawVal));
        return buffer;
    } else if (type == types.Double.Type) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%g",
                 *static_cast<double const*>(rawVal));
        return buffer;
    } else if (Noesis::TypeHelpers::IsDescendantOf(
                   type->GetClassType(),
                   Noesis::gStaticSymbols.TypeClasses.TypeEnum.Type)) {
        auto enumValue = *static_cast<int64_t const*>(rawVal);
        auto enumType = static_cast<Noesis::TypeEnum const*>(type);
        for (auto const& entry : enumType->mValues) {
            if (entry.second == enumValue) {
                return std::string(entry.first.Str());
            }
        }
        // Enum value not registered in Noesis.  Larian's enum registrations
        // are incomplete -- they only register values used in XAML triggers.
        // Log once per (type, value) pair to avoid per-tick spam.
        auto enumTypeName = enumType->GetName();
        {
            struct EnumGapEntry { const char* typeName; int64_t value; };
            static EnumGapEntry sLoggedGaps[32];
            static uint32_t sLoggedGapCount = 0;
            bool alreadyLogged = false;
            for (uint32_t gapIndex = 0; gapIndex < sLoggedGapCount; gapIndex++) {
                if (sLoggedGaps[gapIndex].value == enumValue
                    && sLoggedGaps[gapIndex].typeName == enumTypeName) {
                    alreadyLogged = true;
                    break;
                }
            }
            if (!alreadyLogged && sLoggedGapCount < 32) {
                sLoggedGaps[sLoggedGapCount++] = { enumTypeName, enumValue };
                BG3A_LOG("[BG3Access] Unresolved enum: type=%s value=%lld",
                         enumTypeName ? enumTypeName : "?",
                         (long long)enumValue);
            }
        }
        return std::to_string(enumValue);
    }
    return {};
}

// _Invoke: no destructors, SEH-compatible.
static void ConvertRawValueToString_Invoke(void const* rawVal,
    Noesis::Type const* type, std::string* outStr)
{
    *outStr = ConvertRawValueToString_Inner(rawVal, type);
}
// SEH wrapper: no C++ objects with destructors in this frame.
static bool SafeConvertRawValueToString_SEH(void const* rawVal,
    Noesis::Type const* type, std::string* outStr)
{
    __try {
        ConvertRawValueToString_Invoke(rawVal, type, outStr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Public API: owns the std::string, calls SEH wrapper.
static std::string ConvertRawValueToString(void const* rawVal,
                                            Noesis::Type const* type)
{
    std::string result;
    if (!SafeConvertRawValueToString_SEH(rawVal, type, &result))
        result.clear();
    return result;
}

// IsScalarValueType: returns true if the type is a scalar that
// ConvertRawValueToString can handle.  Used by DC property enumeration
// to decide whether to read a property as a string vs. recurse into
// sub-objects.
static bool IsScalarValueType(Noesis::Type const* type)
{
    if (!type) return false;
    auto& types = Noesis::gStaticSymbols.Types;
    if (type == types.String.Type
        || type == types.CStringPtr.Type
        || type == types.LocaString.Type
        || type == types.Bool.Type
        || type == types.Int32.Type
        || type == types.UInt32.Type
        || type == types.Int64.Type
        || type == types.Single.Type
        || type == types.Double.Type) {
        return true;
    }
    return Noesis::TypeHelpers::IsDescendantOf(
        type->GetClassType(),
        Noesis::gStaticSymbols.TypeClasses.TypeEnum.Type);
}

// Read a named TypeProperty as std::string.  Handles String, CStringPtr,
// LocaString (TranslatedString), bool, int, float.  Returns empty for
// unrecognised or object types.
static std::string ReadTypePropertyAsString_Inner(Noesis::BaseObject const* obj,
                                                    Noesis::TypeProperty const* prop)
{
    auto& types = Noesis::gStaticSymbols.Types;
    auto type = UnwrapType(prop->GetContentType());
    if (!type) return {};

    if (type == types.String.Type) {
        auto value = reinterpret_cast<Noesis::String const*>(
            SafeTypePropertyGet_SEH(prop, obj));
        if (value && value->Size() > 0) return value->Str();
    } else if (type == types.CStringPtr.Type) {
        char const* value = nullptr;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &value)) return {};
        if (value && value[0] != '\0') return value;
    } else if (type == types.LocaString.Type) {
        auto ts = reinterpret_cast<TranslatedString const*>(
            SafeTypePropertyGet_SEH(prop, obj));
        if (ts) {
            auto resolved = ts->Get();
            if (resolved && !resolved->empty()
                && resolved->find("s_HandleUnknown") == std::string::npos) {
                return std::string(resolved->data(), resolved->size());
            }
            // Do NOT fall back to raw handle strings -- they are not
            // user-facing text.  Return empty so callers try other sources.
        }
    } else if (type == types.Bool.Type) {
        bool value = false;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &value)) return {};
        return value ? "On" : "Off";
    } else if (type == types.Int32.Type) {
        int32_t value = 0;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &value)) return {};
        return std::to_string(value);
    } else if (type == types.UInt32.Type) {
        uint32_t value = 0;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &value)) return {};
        return std::to_string(value);
    } else if (type == types.Int64.Type) {
        int64_t value = 0;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &value)) return {};
        return std::to_string(value);
    } else if (type == types.Single.Type) {
        float value = 0;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &value)) return {};
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", value);
        return buf;
    } else if (type == types.Double.Type) {
        double value = 0;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &value)) return {};
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", value);
        return buf;
    } else if (Noesis::TypeHelpers::IsDescendantOf(
                   type->GetClassType(),
                   Noesis::gStaticSymbols.TypeClasses.TypeEnum.Type)) {
        // Enum properties (e.g. Ability=Strength, Skill=Deception).
        // Read the underlying integer, then delegate to shared resolver.
        int64_t enumValue = 0;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &enumValue)) return {};
        return ConvertRawValueToString(&enumValue, type);
    }
    // Pointer / Ptr / object types -- not convertible to string here.
    return {};
}

static void ReadTypePropertyAsString_Invoke(Noesis::BaseObject const* obj,
    Noesis::TypeProperty const* prop, std::string* outStr) {
    *outStr = ReadTypePropertyAsString_Inner(obj, prop);
}
static bool SafeReadTypePropertyAsString_SEH(Noesis::BaseObject const* obj,
    Noesis::TypeProperty const* prop, std::string* outStr) {
    __try { ReadTypePropertyAsString_Invoke(obj, prop, outStr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static std::string ReadTypePropertyAsString(Noesis::BaseObject const* obj,
                                             Noesis::TypeProperty const* prop) {
    std::string result;
    if (!SafeReadTypePropertyAsString_SEH(obj, prop, &result)) result.clear();
    return result;
}

// Read a named DependencyProperty as std::string.
//
// Primary path: scan mValues for the DP and read through StoredValue.
// StoredValue::ComplexValue::base holds the cached binding result, so
// this correctly reads bound properties (Content, Text with converters).
//
// Fallback: DependencyProperty::GetValue() for inherited/default values
// not present in mValues.
static std::string ReadDepPropertyAsString_Inner(Noesis::DependencyObject const* depObj,
                                            Noesis::DependencyProperty const* dp)
{
    auto& types = Noesis::gStaticSymbols.Types;
    auto type = UnwrapType(dp->GetType());
    if (!type) return {};

    // --- Primary path: mValues StoredValue (reads cached binding results) ---
    // StoredValue::ComplexValue::base holds the cached result of evaluating
    // a binding expression.  This is the same path the Lua bridge uses.
    auto mutableObj = const_cast<Noesis::DependencyObject*>(depObj);
    auto entry = mutableObj->mValues.Find(dp);
    if (entry != mutableObj->mValues.End()) {
        auto storedVal = entry->value;
        if (storedVal->flags.isInitialized) {
            // Get the raw value pointer, following ComplexValue::base for bindings
            void* rawVal = nullptr;
            if (storedVal->flags.isComplex) {
                rawVal = storedVal->value.complex->base;
            } else {
                rawVal = storedVal->value.simple;
            }

            if (rawVal) {
                // Scalar + enum types via shared converter (handles all
                // types including CStringPtr, LocaString, UInt32, Int64,
                // Double, Enum that were previously missing from this path).
                auto scalarResult = ConvertRawValueToString(rawVal, type);
                if (!scalarResult.empty()) return scalarResult;

                // Object-typed DP (Content, etc.): rawVal IS the object pointer.
                // The binding converter cached its result as the base value.
                // Only attempt ToString if the DP type actually descends from
                // BaseObject -- otherwise rawVal is a raw data pointer for an
                // unhandled value type (struct, Thickness, etc.) and
                // reinterpret_casting it as BaseObject* would crash.
                auto& classes = Noesis::gStaticSymbols.TypeClasses;
                if (Noesis::TypeHelpers::IsDescendantOf(type, classes.BaseObject.Type)) {
                    auto obj = reinterpret_cast<Noesis::BaseObject*>(rawVal);
                    if (obj) {
                        char strBuf[512];
                        if (SafeToString_SEH(obj, strBuf, sizeof(strBuf))) {
                            std::string str(strBuf);
                            if (str.find("Noesis::") != 0
                                && str.find("ls.") != 0
                                && str.find("[ForceUpdate]") == std::string::npos) {
                                return str;
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Fallback: GetValue (inherited/default values not in mValues) ---
    auto val = dp->GetValue(depObj);
    if (!val) return {};

    // Shared converter handles all scalar + enum types.
    return ConvertRawValueToString(val, type);
}

static void ReadDepPropertyAsString_Invoke(Noesis::DependencyObject const* depObj,
    Noesis::DependencyProperty const* dp, std::string* outStr) {
    *outStr = ReadDepPropertyAsString_Inner(depObj, dp);
}
static bool SafeReadDepPropertyAsString_SEH(Noesis::DependencyObject const* depObj,
    Noesis::DependencyProperty const* dp, std::string* outStr) {
    __try { ReadDepPropertyAsString_Invoke(depObj, dp, outStr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static std::string ReadDepPropertyAsString(Noesis::DependencyObject const* depObj,
                                            Noesis::DependencyProperty const* dp) {
    std::string result;
    if (!SafeReadDepPropertyAsString_SEH(depObj, dp, &result)) result.clear();
    return result;
}

// Read a DP value directly from mValues by scanning for a matching DP name.
// Bypasses the class cache entirely -- works for inherited DPs like Content
// on ContentControl subclasses that may not be in the subclass's Names map.
// Returns the string value from StoredValue::ComplexValue::base (cached
// binding result) or StoredValue::simple.
static std::string ReadDPFromMValues_Inner(Noesis::DependencyObject const* depObj,
                                            const char* dpName)
{
    auto mutableObj = const_cast<Noesis::DependencyObject*>(depObj);
    Noesis::Symbol targetSym(dpName);

    for (auto& entry : mutableObj->mValues) {
        if (!entry.key || entry.key->GetName() != targetSym) continue;

        auto storedVal = entry.value;
        if (!storedVal || !storedVal->flags.isInitialized) continue;

        void* rawVal = storedVal->flags.isComplex
            ? storedVal->value.complex->base
            : storedVal->value.simple;
        if (!rawVal) continue;

        auto dpType = UnwrapType(entry.key->GetType());
        if (!dpType) continue;

        auto& types = Noesis::gStaticSymbols.Types;

        // Scalar + enum types via shared converter.
        auto scalarResult = ConvertRawValueToString(rawVal, dpType);
        if (!scalarResult.empty()) return scalarResult;

        // Object-typed DP: rawVal is the cached binding result.
        // Only attempt ToString if the DP type descends from BaseObject.
        auto& classes = Noesis::gStaticSymbols.TypeClasses;
        if (Noesis::TypeHelpers::IsDescendantOf(dpType, classes.BaseObject.Type)) {
            auto cachedObj = reinterpret_cast<Noesis::BaseObject*>(rawVal);
            if (cachedObj) {
                char strBuf[512];
                if (SafeToString_SEH(cachedObj, strBuf, sizeof(strBuf))) {
                    std::string str(strBuf);
                    if (str.find("Noesis::") != 0
                        && str.find("ls.") != 0
                        && str.find("[ForceUpdate]") == std::string::npos) {
                        return str;
                    }
                }
            }
        }
    }

    return {};
}

// SEH wrapper: mValues iteration can fault on stale DependencyObjects.
// Invoke bridges Inner (has std::string) to SEH (no C++ objects).
static void ReadDPFromMValues_Invoke(Noesis::DependencyObject const* depObj,
                                      const char* dpName, std::string& outStr)
{
    outStr = ReadDPFromMValues_Inner(depObj, dpName);
}
static bool SafeReadDPFromMValues_SEH(Noesis::DependencyObject const* depObj,
                                       const char* dpName, std::string& outStr)
{
    __try {
        ReadDPFromMValues_Invoke(depObj, dpName, outStr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static std::string ReadDPFromMValues(Noesis::DependencyObject const* depObj,
                                      const char* dpName)
{
    std::string result;
    if (!SafeReadDPFromMValues_SEH(depObj, dpName, result)) {
        result.clear();
    }
    return result;
}

// Read a named property from a Noesis object, returning std::string.
// Checks TypeProperties, then DependencyProperties (class cache), then
// scans mValues directly (catches inherited DPs not in subclass cache).
//
// This is the foundational helper for all C++ text extraction.
static std::string ReadPropertyAsString_Inner(Noesis::BaseObject const* obj,
                                                const char* propName)
{
    if (!obj) return {};

    auto classType = SafeGetClassType_SEH(obj);
    if (!classType) return {};
    auto const& cls = Noesis::gClassCache.GetClass(classType);
    bg3se::FixedString fsName(propName);
    auto prop = cls.Names.try_get(fsName);

    if (prop) {
        if (prop->Property) {
            auto result = ReadTypePropertyAsString(obj, prop->Property);
            if (!result.empty()) return result;
        }

        if (prop->DepProperty) {
            auto depObj = static_cast<Noesis::DependencyObject const*>(obj);
            auto result = ReadDepPropertyAsString(depObj, prop->DepProperty);
            if (!result.empty()) return result;
        }
    }

    // Final fallback: scan mValues directly by DP name.
    // Catches DPs inherited from base classes (e.g. Content on LSButton
    // from ContentControl) that the subclass cache may not map.
    auto depObj = static_cast<Noesis::DependencyObject const*>(obj);
    return ReadDPFromMValues(depObj, propName);
}

static void ReadPropertyAsString_Invoke(Noesis::BaseObject const* obj,
    const char* propName, std::string* outStr) {
    *outStr = ReadPropertyAsString_Inner(obj, propName);
}
static bool SafeReadPropertyAsString_SEH(Noesis::BaseObject const* obj,
    const char* propName, std::string* outStr) {
    __try { ReadPropertyAsString_Invoke(obj, propName, outStr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static std::string ReadPropertyAsString(Noesis::BaseObject const* obj,
                                         const char* propName) {
    std::string result;
    if (!SafeReadPropertyAsString_SEH(obj, propName, &result)) result.clear();
    return result;
}

// Read expanded state for a ToggleButton inside an Expander template.
// Walks up via mVisualParent to find an Expander ancestor, then reads
// its IsExpanded DP (regular bool, not Nullable).  Returns 1 (expanded),
// 0 (collapsed), or -1 (not found / fault).
// No C++ objects needing destructors -- safe for __try.
static int ReadExpanderState_Invoke(Noesis::Visual* elem)
{
    // Walk up to find the Expander ancestor (typically 3-4 hops).
    Noesis::Visual* current = elem;
    for (int i = 0; i < 8 && current; i++) {
        auto parent = current->mVisualParent;
        if (!parent) break;
        auto parentTypeName = parent->GetClassType()->GetName();
        if (parentTypeName && strstr(parentTypeName, "Expander")) {
            // Found the Expander -- read IsExpanded via GetValue.
            auto expanderDepObj = static_cast<Noesis::DependencyObject const*>(parent);
            // Look up IsExpanded DP from the Expander's class.
            auto expanderClass = parent->GetClassType();
            auto& cls = Noesis::gClassCache.GetClass(expanderClass);
            bg3se::FixedString fsName("IsExpanded");
            auto prop = cls.Names.try_get(fsName);
            if (prop && prop->DepProperty) {
                auto val = prop->DepProperty->GetValue(expanderDepObj);
                if (val) {
                    return *static_cast<const bool*>(val) ? 1 : 0;
                }
            }
            return -1;
        }
        current = parent;
    }
    return -1;
}
static int SafeReadExpanderState_SEH(Noesis::Visual* elem)
{
    __try { return ReadExpanderState_Invoke(elem); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ---------------------------------------------------------------------------
// ReadToggleIsChecked: reads ToggleButton.IsChecked DP directly from a
// CheckBox or ToggleButton element (not an ancestor walk like the Expander
// reader).  IsChecked is Nullable<bool> in Noesis -- the first byte of
// the stored value contains the bool state.  SEH protects against
// unexpected memory layout.
// Returns 1 (checked), 0 (unchecked), or -1 (missing / fault).
// ---------------------------------------------------------------------------
static int ReadToggleIsChecked_Invoke(Noesis::FrameworkElement* elem)
{
    auto classType = elem->GetClassType();
    if (!classType) return -1;
    auto& cls = Noesis::gClassCache.GetClass(classType);
    bg3se::FixedString fsIsChecked("IsChecked");
    auto prop = cls.Names.try_get(fsIsChecked);
    if (prop && prop->DepProperty) {
        auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
        auto val = prop->DepProperty->GetValue(depObj);
        if (val) {
            // ToggleButton.IsChecked is Nullable<bool>.  Memory layout:
            //   byte 0: mHasValue (BaseNullable)
            //   byte 1: mValue (the actual bool)
            // Cast to Nullable<bool>* to read correctly.
            auto nullable = static_cast<Noesis::Nullable<bool> const*>(val);
            if (nullable->HasValue()) {
                return nullable->GetValue() ? 1 : 0;
            }
            return -1;  // indeterminate (null)
        }
    }
    return -1;
}
static int SafeReadToggleIsChecked_SEH(Noesis::FrameworkElement* elem)
{
    __try { return ReadToggleIsChecked_Invoke(elem); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ReadElementName: reads the "Name" property (x:Name) from a
// FrameworkElement into a caller-provided buffer.  SEH-wrapped
// via inner/invoke pattern (ReadPropertyAsString returns std::string).
static void ReadElementName_Invoke(Noesis::FrameworkElement* elem,
        char* buf, size_t bufSize)
{
    auto nameStr = ReadPropertyAsString(elem, "Name");
    strncpy_s(buf, bufSize, nameStr.c_str(), _TRUNCATE);
}
static void ReadElementName_SEH(Noesis::FrameworkElement* elem,
        char* buf, size_t bufSize)
{
    __try { ReadElementName_Invoke(elem, buf, bufSize); }
    __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; }
}

// SafeGetFontSize: read FontSize via the cached sFontSizeProp DP using
// GetValue() which resolves the full provider chain (including Style
// values).  No C++ objects with destructors -- SEH-safe directly.
static float SafeGetFontSize_SEH(Noesis::FrameworkElement* elem)
{
    __try {
        if (!sFontSizeProp) return 0.0f;
        auto val = sFontSizeProp->GetValue(
            static_cast<Noesis::DependencyObject const*>(elem));
        if (!val) return 0.0f;
        return *static_cast<float const*>(val);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

// ---------------------------------------------------------------------------
// ReadTextBlockText: three-step TextBlock text extraction in C++.
//
// 1. Inlines collection iteration (Run.Text + LineBreak spacing)
// 2. GetProperty("Text") -- fallback for local/non-bound values
// 3. ToString() fallback
//
// Inlines-first because CtxTransStringRunGeneratorBehavior (used by
// Larian for parameterized descriptions like Darkvision) populates
// Inlines with Run objects that contain the fully-resolved, unit-
// converted text.  GetProperty("Text") on these TextBlocks returns
// only the last parameter value (e.g., "40ft") instead of the full
// sentence ("Can see in the dark up to 40ft."), causing partial reads.
// For simple TextBlocks without Inlines, the collection is empty and
// the function falls through to GetProperty -- same result as before.
// ---------------------------------------------------------------------------
static std::string ReadTextBlockText_Inner(Noesis::FrameworkElement* elem, bool skipToString)
{
    if (!elem) return {};

    // Step 1: Iterate Inlines collection (Run.Text + LineBreak spacing).
    {
        auto elemClassType = SafeGetClassType_SEH(elem);
        if (!elemClassType) return {};
        auto const& cls = Noesis::gClassCache.GetClass(elemClassType);
        bg3se::FixedString fsInlines("Inlines");
        auto prop = cls.Names.try_get(fsInlines);

        if (prop && prop->Property) {
            auto& types = Noesis::gStaticSymbols.Types;
            auto& classes = Noesis::gStaticSymbols.TypeClasses;
            auto type = UnwrapType(prop->Property->GetContentType());
            auto typeOfType = type ? type->GetClassType() : nullptr;

            Noesis::BaseCollection* coll = nullptr;

            // Collection could be stored as Ptr<T>, T*, or direct BaseObject*.
            if (typeOfType == types.TypePtr.Type) {
                auto ptrVal = reinterpret_cast<Noesis::Ptr<Noesis::BaseRefCounted>*>(
                    const_cast<void*>(prop->Property->Get(elem)));
                if (ptrVal) coll = static_cast<Noesis::BaseCollection*>(
                    static_cast<Noesis::BaseObject*>(ptrVal->GetPtr()));
            } else if (typeOfType == types.TypePointer.Type) {
                Noesis::BaseObject* raw = nullptr;
                prop->Property->GetCopy(elem, &raw);
                if (raw) coll = static_cast<Noesis::BaseCollection*>(raw);
            } else if (type && Noesis::TypeHelpers::IsDescendantOf(
                           type, classes.BaseCollection.Type)) {
                coll = static_cast<Noesis::BaseCollection*>(
                    const_cast<Noesis::BaseObject*>(
                        reinterpret_cast<Noesis::BaseObject const*>(
                            prop->Property->Get(elem))));
            }

            if (coll) {
                int count = SafeCollectionCount(coll);
                if (count > 0) {
                    std::string parts;
                    for (int i = 0; i < count; i++) {
                        auto inlineObj = SafeCollectionGetItem_SEH(coll, (uint32_t)i);
                        if (!inlineObj) continue;

                        auto typeName = SafeBaseObjectTypeName_SEH(inlineObj);
                        if (!typeName) { inlineObj->Release(); continue; }

                        if (strstr(typeName, "Run")) {
                            auto runText = ReadPropertyAsString(inlineObj, "Text");
                            if (!runText.empty()
                                && runText.find("[ForceUpdate]") == std::string::npos) {
                                parts += runText;
                            }
                        } else if (strstr(typeName, "LineBreak")) {
                            parts += " ";
                        } else {
                            // Hyperlink, Bold, Italic, Span, etc.
                            // These contain child Runs in their own Inlines
                            // collection.  Access via the same class cache.
                            auto spanClassType = SafeGetClassType_SEH(inlineObj);
                            if (!spanClassType) { inlineObj->Release(); continue; }
                            auto& spanCls = Noesis::gClassCache.GetClass(spanClassType);
                            bg3se::FixedString fsSpanInlines("Inlines");
                            auto spanProp = spanCls.Names.try_get(fsSpanInlines);
                            if (spanProp && spanProp->Property) {
                                Noesis::BaseCollection* spanColl = nullptr;
                                auto spanType = UnwrapType(
                                    spanProp->Property->GetContentType());
                                auto spanTypeOfType = spanType
                                    ? spanType->GetClassType() : nullptr;
                                if (spanTypeOfType == types.TypePtr.Type) {
                                    auto ptrVal = reinterpret_cast<
                                        Noesis::Ptr<Noesis::BaseRefCounted>*>(
                                        const_cast<void*>(
                                            spanProp->Property->Get(inlineObj)));
                                    if (ptrVal) spanColl =
                                        static_cast<Noesis::BaseCollection*>(
                                            static_cast<Noesis::BaseObject*>(
                                                ptrVal->GetPtr()));
                                } else if (spanTypeOfType == types.TypePointer.Type) {
                                    Noesis::BaseObject* raw = nullptr;
                                    spanProp->Property->GetCopy(inlineObj, &raw);
                                    if (raw) spanColl =
                                        static_cast<Noesis::BaseCollection*>(raw);
                                } else if (spanType && Noesis::TypeHelpers::IsDescendantOf(
                                               spanType, classes.BaseCollection.Type)) {
                                    spanColl = static_cast<Noesis::BaseCollection*>(
                                        const_cast<Noesis::BaseObject*>(
                                            reinterpret_cast<Noesis::BaseObject const*>(
                                                spanProp->Property->Get(inlineObj))));
                                }
                                if (spanColl) {
                                    int spanCount = SafeCollectionCount(spanColl);
                                    for (int si = 0; si < spanCount; si++) {
                                        auto spanChild = SafeCollectionGetItem_SEH(
                                            spanColl, (uint32_t)si);
                                        if (!spanChild) continue;
                                        auto childType =
                                            SafeBaseObjectTypeName_SEH(spanChild);
                                        if (!childType) {
                                            spanChild->Release();
                                            continue;
                                        }
                                        if (strstr(childType, "Run")) {
                                            auto childText = ReadPropertyAsString(
                                                spanChild, "Text");
                                            if (!childText.empty()
                                                && childText.find("[ForceUpdate]")
                                                    == std::string::npos) {
                                                parts += childText;
                                            }
                                        } else if (strstr(childType, "Span")) {
                                            // Nested Span (CtxTransStringRunGeneratorBehavior
                                            // wraps static text in child Spans containing Runs).
                                            // Recurse one more level to find the Runs.
                                            auto innerClassType = SafeGetClassType_SEH(spanChild);
                                            if (innerClassType) {
                                                auto& innerCls = Noesis::gClassCache.GetClass(innerClassType);
                                                bg3se::FixedString fsInner("Inlines");
                                                auto innerProp = innerCls.Names.try_get(fsInner);
                                                if (innerProp && innerProp->Property) {
                                                    Noesis::BaseCollection* innerColl = nullptr;
                                                    auto innerType = UnwrapType(innerProp->Property->GetContentType());
                                                    auto innerTypeOfType = innerType ? innerType->GetClassType() : nullptr;
                                                    if (innerTypeOfType == types.TypePtr.Type) {
                                                        auto pv = reinterpret_cast<Noesis::Ptr<Noesis::BaseRefCounted>*>(
                                                            const_cast<void*>(innerProp->Property->Get(spanChild)));
                                                        if (pv) innerColl = static_cast<Noesis::BaseCollection*>(
                                                            static_cast<Noesis::BaseObject*>(pv->GetPtr()));
                                                    } else if (innerTypeOfType == types.TypePointer.Type) {
                                                        Noesis::BaseObject* raw = nullptr;
                                                        innerProp->Property->GetCopy(spanChild, &raw);
                                                        if (raw) innerColl = static_cast<Noesis::BaseCollection*>(raw);
                                                    } else if (innerType && Noesis::TypeHelpers::IsDescendantOf(
                                                                   innerType, classes.BaseCollection.Type)) {
                                                        innerColl = static_cast<Noesis::BaseCollection*>(
                                                            const_cast<Noesis::BaseObject*>(
                                                                reinterpret_cast<Noesis::BaseObject const*>(
                                                                    innerProp->Property->Get(spanChild))));
                                                    }
                                                    if (innerColl) {
                                                        int innerCount = SafeCollectionCount(innerColl);
                                                        for (int ii = 0; ii < innerCount; ii++) {
                                                            auto innerChild = SafeCollectionGetItem_SEH(innerColl, (uint32_t)ii);
                                                            if (!innerChild) continue;
                                                            auto innerChildType = SafeBaseObjectTypeName_SEH(innerChild);
                                                            if (innerChildType && strstr(innerChildType, "Run")) {
                                                                auto innerRunText = ReadPropertyAsString(innerChild, "Text");
                                                                if (!innerRunText.empty()
                                                                    && innerRunText.find("[ForceUpdate]") == std::string::npos) {
                                                                    parts += innerRunText;
                                                                }
                                                            }
                                                            innerChild->Release();
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        spanChild->Release();
                                    }
                                }
                            }
                        }
                        inlineObj->Release();
                    }
                    // Collapse multiple spaces.
                    std::string result;
                    bool lastWasSpace = true;  // trim leading
                    for (char c : parts) {
                        if (c == ' ') {
                            if (!lastWasSpace) { result += c; lastWasSpace = true; }
                        } else {
                            result += c;
                            lastWasSpace = false;
                        }
                    }
                    // Trim trailing space.
                    if (!result.empty() && result.back() == ' ') result.pop_back();
                    if (!result.empty()) return result;
                }
            }
        }
    }

    // Step 2: GetProperty("Text") -- fallback for simple TextBlocks
    // with a local (non-bound) Text value and no Inlines.
    {
        auto text = ReadPropertyAsString(elem, "Text");
        if (!text.empty() && text.find("[ForceUpdate]") == std::string::npos) {
            return text;
        }
    }

    // Step 3: ToString() fallback.  Skip when called from NameScope
    // collection -- ToString() evaluates bindings at runtime, which crashes
    // on TextBlocks whose bindings haven't resolved yet.  Steps 1+2 read
    // stored values only, which is safe.
    if (!skipToString) {
        char strBuf[512];
        if (SafeToString_SEH(elem, strBuf, sizeof(strBuf))) {
            std::string str(strBuf);
            if (str.find("TextBlock") == std::string::npos
                && str.find("[ForceUpdate]") == std::string::npos) {
                return str;
            }
        }
    }

    return {};
}

static void ReadTextBlockText_Invoke(Noesis::FrameworkElement* elem, bool skipToString, std::string* outStr) {
    *outStr = ReadTextBlockText_Inner(elem, skipToString);
}
static bool SafeReadTextBlockText_SEH(Noesis::FrameworkElement* elem, bool skipToString, std::string* outStr) {
    __try { ReadTextBlockText_Invoke(elem, skipToString, outStr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static std::string ReadTextBlockText(Noesis::FrameworkElement* elem, bool skipToString) {
    std::string result;
    if (!SafeReadTextBlockText_SEH(elem, skipToString, &result)) result.clear();
    return result;
}

// ---------------------------------------------------------------------------
// NameScope TextBlock collector: iterates a widget's NameScope HashMap to
// find ALL authored TextBlocks with resolved text.  Generic -- works for
// any menu without hardcoded names.
//
// Strategy:
// 1. Walk the first-child chain from the widget element to find the
//    NameScope owner (attached DP lookup, same descent as FindNameInWidget).
// 2. Iterate NameScope::mNamedObjects (now public) using the HashMap's
//    template Begin/End iterators (inline code, no link dependency).
// 3. For each entry whose value is a TextBlock, read text via the
//    three-step extraction (GetProperty, Inlines, ToString).
//
// Used for LocaString-bound text that lives in authored XAML TextBlocks
// rather than in any ViewModel DataContext property.
// ---------------------------------------------------------------------------

// SEH-guarded ReadTextBlockText wrapper.  Two-function pattern because
// MSVC error C2712 forbids __try in functions with C++ objects (std::string).
//
// Inner function: does the actual text extraction (has C++ objects).
// Outer function: probes the element with __try, then calls the inner
// function.  If the probe crashes, we skip.  The inner function itself
// may still crash on deeper access (Inlines, ToString), but the probe
// catches the most common failure (partially-constructed elements).
//
// The probe catches elements in a bad state (vtable corrupt, etc.).
// For elements that pass the probe but crash inside ReadTextBlockText
// (e.g. unresolved bindings), the inner function's std::string is valid
// C++ -- it won't crash on construction, only on Noesis API calls.
// Those Noesis crashes are caught by the probe pattern: if the element
// is accessible enough to pass probe, ReadTextBlockText should be safe.
static bool ProbeTextBlockElement(Noesis::FrameworkElement* elem)
{
    __try {
        (void)elem->GetClassType()->GetName();
        (void)elem->GetVisualChildrenCount();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Top-level SEH guard for CollectNamedTextsFromWidget.  If any TextBlock
// crashes ReadTextBlockText (unresolved binding, corrupt element, etc.),
// we lose NameScope texts for this widget but don't crash the game.
// No C++ locals with destructors in this function (MSVC C2712).
static void TryCollectNamedTexts(
    Noesis::FrameworkElement* widgetElem,
    std::vector<std::pair<std::string, std::string>>& namedTexts)
{
    __try {
        CollectNamedTextsFromWidget(widgetElem, namedTexts);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_TRACE("[BG3Access]   NameScope: CRASH in CollectNamedTextsFromWidget, skipping");
    }
}

// ---------------------------------------------------------------------------
// BFS_VisitTextBlocks: shared BFS core for walking visual subtrees and
// visiting each TextBlock found.  Replaces three separate BFS functions
// (GatherVisibleTextBlocks, ShallowChildTextScan, ReadWidgetTexts) that
// had identical detection/reading/filtering logic.
//
// Parameters:
//   root: starting element for the BFS.
//   maxDepth: maximum tree depth to explore (0 = unlimited).
//   maxNodes: maximum nodes to process (queue capacity).
//   skipUIWidgets: skip child UIWidget elements (they have their own scope).
//   visitor: called for each TextBlock with valid text.
//            Receives the TextBlock element, its text (moved), and context.
//            Return true to continue, false to stop.
//   context: opaque pointer forwarded to visitor.
//
// Safety: ProbeUIElement + IsVisibleDP on every node.  ReadTextBlockText
// is proven safe.  No Noesis pointers escape this function.
// This is an _Inner function (uses C++ objects with destructors: vector,
// string from ReadTextBlockText).  Must NOT live inside __try.
// ---------------------------------------------------------------------------
typedef bool (*TextBlockVisitorFn)(Noesis::FrameworkElement* textBlock,
                                    std::string&& text, void* context);

// _Inner: uses std::vector and std::string (destructors) -- cannot
// live inside __try (MSVC C2712).
static void BFS_VisitTextBlocks_Inner(
    Noesis::Visual* root,
    int maxDepth,
    int maxNodes,
    bool skipUIWidgets,
    TextBlockVisitorFn visitor,
    void* context)
{
    if (!root) return;

    struct QueueEntry {
        Noesis::Visual* node;
        int depth;
    };
    std::vector<QueueEntry> queue;
    queue.reserve(maxNodes > 0 ? maxNodes : 512);
    queue.push_back({root, 0});
    int processed = 0;
    int nodeLimit = maxNodes > 0 ? maxNodes : 512;

    while (processed < (int)queue.size() && processed < nodeLimit) {
        auto entry = queue[processed++];
        if (!entry.node) continue;
        if (maxDepth > 0 && entry.depth > maxDepth) continue;
        if (!ProbeUIElement(static_cast<Noesis::UIElement*>(entry.node)))
            continue;
        if (!IsVisibleDP(entry.node)) continue;

        // Skip child UIWidgets -- they have their own scope.
        if (skipUIWidgets && entry.node != root
            && SafeIsUIWidgetType_SEH(entry.node)) continue;

        auto typeName = SafeBaseObjectTypeName_SEH(entry.node);
        if (!typeName) continue;

        // Found a TextBlock: read text immediately (before finding more
        // TextBlocks, so binding evaluation can't destabilize unfound nodes).
        // Don't recurse into TextBlock children (Inline objects handled by
        // ReadTextBlockText).
        if (strstr(typeName, "TextBlock")) {
            auto text = ReadTextBlockText(
                static_cast<Noesis::FrameworkElement*>(entry.node));
            if (!text.empty()
                && text.find("[ForceUpdate]") == std::string::npos
                && text.find("s_HandleUnknown") == std::string::npos) {
                if (!visitor(
                        static_cast<Noesis::FrameworkElement*>(entry.node),
                        std::move(text), context)) {
                    return;  // Visitor says stop
                }
            }
            continue;
        }

        // Enqueue visible children for BFS -- SEH-guarded.
        auto childCount = SafeGetVisualChildrenCount_SEH(entry.node);
        for (uint32_t i = 0; i < childCount
             && (int)queue.size() < nodeLimit; i++) {
            auto child = SafeGetVisualChild_SEH(entry.node, i);
            if (child) queue.push_back({child, entry.depth + 1});
        }
    }
}

// _Invoke: no destructors, SEH-compatible.  All parameters are POD.
static void BFS_VisitTextBlocks_Invoke(
    Noesis::Visual* root, int maxDepth, int maxNodes,
    bool skipUIWidgets, TextBlockVisitorFn visitor, void* context)
{
    BFS_VisitTextBlocks_Inner(root, maxDepth, maxNodes,
                              skipUIWidgets, visitor, context);
}

// Public wrapper with SEH guard.
static void BFS_VisitTextBlocks(
    Noesis::Visual* root, int maxDepth, int maxNodes,
    bool skipUIWidgets, TextBlockVisitorFn visitor, void* context)
{
    __try {
        BFS_VisitTextBlocks_Invoke(root, maxDepth, maxNodes,
                                   skipUIWidgets, visitor, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] BFS_VisitTextBlocks: SEH fault");
    }
}

// ---------------------------------------------------------------------------
// BFS visitor callbacks
// ---------------------------------------------------------------------------

// Collects all TextBlock texts into a vector (no limit).
static bool CollectAllTextsVisitor(Noesis::FrameworkElement*,
                                    std::string&& text, void* context)
{
    auto outTexts = static_cast<std::vector<std::string>*>(context);
    outTexts->push_back(std::move(text));
    return true;
}

// Collects TextBlock texts up to a maximum count.
struct BoundedCollectContext {
    std::vector<std::string>& texts;
    size_t maxTexts;
};
static bool BoundedCollectVisitor(Noesis::FrameworkElement*,
                                   std::string&& text, void* context)
{
    auto ctx = static_cast<BoundedCollectContext*>(context);
    ctx->texts.push_back(std::move(text));
    return ctx->texts.size() < ctx->maxTexts;
}

// Takes the first TextBlock text and stops.
struct FirstTextContext {
    std::string result;
};
static bool FirstTextVisitor(Noesis::FrameworkElement*,
                              std::string&& text, void* context)
{
    auto ctx = static_cast<FirstTextContext*>(context);
    ctx->result = std::move(text);
    return false;  // Stop after first
}

// ---------------------------------------------------------------------------
// GatherVisibleTextBlocks: collects all visible TextBlock texts from a
// visual subtree.  Skips child UIWidgets (separate scope).  Bounded by
// maxDepth and maxNodes.
//
// Three-layer SEH pattern: _Inner -> _Invoke -> public wrapper.
// ---------------------------------------------------------------------------
static void GatherVisibleTextBlocks_Inner(
    Noesis::FrameworkElement* root,
    std::vector<std::string>& outTexts,
    int maxDepth,
    int maxNodes)
{
    BFS_VisitTextBlocks(root, maxDepth, maxNodes, true,
                        CollectAllTextsVisitor, &outTexts);
}

static void GatherVisibleTextBlocks_Invoke(
    Noesis::FrameworkElement* root,
    std::vector<std::string>* outTexts,
    int maxDepth,
    int maxNodes)
{
    GatherVisibleTextBlocks_Inner(root, *outTexts, maxDepth, maxNodes);
}

static void GatherVisibleTextBlocks(
    Noesis::FrameworkElement* root,
    std::vector<std::string>& outTexts,
    int maxDepth,
    int maxNodes)
{
    __try {
        GatherVisibleTextBlocks_Invoke(root, &outTexts, maxDepth, maxNodes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] GatherVisibleTextBlocks: SEH fault");
    }
}

// ---------------------------------------------------------------------------
// ShallowChildTextScan: returns the FIRST TextBlock text found within
// 5 levels and 64 nodes.  Used by ExtractElementData for ContentControl/
// Control elements whose displayed text is in template-generated children.
// ---------------------------------------------------------------------------
static std::string ShallowChildTextScan(Noesis::FrameworkElement* elem)
{
    FirstTextContext context;
    BFS_VisitTextBlocks(elem, 5, 64, false, FirstTextVisitor, &context);
    return std::move(context.result);
}

// Guarded wrapper: probes the element before scanning.  The BFS
// operates on the currently focused element (alive this tick), so it
// is lower risk than NameScope walks.  Probe catches the most obvious
// corruption (vtable, child count) before entering the scan.
static std::string TryShallowChildTextScan(Noesis::FrameworkElement* elem)
{
    if (!ProbeTextBlockElement(elem)) return {};
    return ShallowChildTextScan(elem);
}

// DataContext type exclusion list for overlay widgets whose TextBlocks
// have unresolved bindings.  These widgets are NOT content pages -- they
// are download progress overlays, notification popups, etc.  Skipping
// them avoids crashes in ReadTextBlockText without losing any user-facing
// text (overlays use DC properties, not authored NameScope TextBlocks).
static bool IsOverlayDCType(const char* dcTypeName)
{
    if (!dcTypeName) return false;
    // DCNotifications: download/mod notification overlays
    if (strstr(dcTypeName, "DCNotifications")) return true;
    // DCModBrowserDownload: active mod download progress
    if (strstr(dcTypeName, "DCModBrowserDownload")) return true;
    // DCModDownloadProgress: download progress bars
    if (strstr(dcTypeName, "DCModDownloadProgress")) return true;
    // DCCharacterCreation: god-object DC with 43+ NameScope entries.
    // Walking NameScope during race/class switches crashes because
    // TextBlocks are destroyed mid-rebuild.  Character creation uses
    // DC properties and BFS child scan instead of NameScope texts.
    if (strstr(dcTypeName, "DCCharacterCreation")) return true;
    return false;
}

static void CollectNamedTextsFromWidget(
    Noesis::FrameworkElement* widgetElem,
    std::vector<std::pair<std::string, std::string>>& namedTexts)
{
    if (!widgetElem) return;
    if (!sNameScopeProp) return;

    // Part 3: DC type filter -- skip overlay widgets whose TextBlocks
    // have unresolved bindings that crash ReadTextBlockText.
    {
        auto dataContext = SafeReadDC_SEH(
            static_cast<Noesis::DependencyObject const*>(widgetElem));
        if (dataContext) {
            auto dcTypeName = SafeBaseObjectTypeName_SEH(dataContext);
            if (dcTypeName && IsOverlayDCType(dcTypeName)) {
                BG3A_TRACE("[BG3Access]   NameScope: skipping overlay widget DC=%s", dcTypeName);
                return;
            }
        }
    }

    // Walk first-child chain to find the page-level NameScope (the one with
    // all the x:Name'd elements).  Multiple NameScopes exist at different
    // depths: the UIWidget has an outer scope (1 entry), the page Grid
    // "Root" has the main scope (many entries), and deeper control templates
    // may have smaller scopes.  Use the scope with the MOST entries.
    Noesis::NameScope* nameScope = nullptr;
    uint32_t bestSize = 0;
    Noesis::Visual* current = widgetElem;
    for (int depth = 0; depth < 6; depth++) {
        auto depObj = static_cast<Noesis::DependencyObject const*>(current);

        auto storedValue = sNameScopeProp->GetValue(depObj);
        if (storedValue) {
            auto candidate = *reinterpret_cast<Noesis::NameScope* const*>(storedValue);
            if (candidate) {
                auto candidateSize = candidate->mNamedObjects.Size();
                // (Diagnostic removed: NameScope depth/address/size)
                if (candidateSize > bestSize) {
                    nameScope = candidate;
                    bestSize = candidateSize;
                }
            }
        }

        auto childCount = SafeGetVisualChildrenCount_SEH(current);
        if (childCount == 0) break;
        auto child = SafeGetVisualChild_SEH(current, 0);
        if (!child) break;
        current = child;
    }

    if (!nameScope) return;

    // (Diagnostic removed: NameScope iteration count)

    for (auto iterator = nameScope->mNamedObjects.Begin();
         iterator != nameScope->mNamedObjects.End();
         ++iterator) {
        auto component = iterator->value;
        if (!component) continue;

        auto elementName = iterator->key.Str();

        // Only interested in TextBlock elements.
        auto typeName = component->GetClassType()->GetName();
        if (!strstr(typeName, "TextBlock")) continue;

        // Look up by name through FindNameInWidgetScoped which returns a
        // properly cast FrameworkElement* via FindNodeName (exported, safe).
        auto textBlockElem = FindNameInWidgetScoped(elementName, widgetElem);
        if (!textBlockElem) continue;

        // Part 1: ancestor visibility check.  Replaces IsVisibleDP which
        // doesn't coerce through ancestors in the Indie SDK.  This filters
        // out TextBlocks in collapsed tab panels (e.g. CrossPlayWarningTitle
        // when on the Online Community tab).
        if (!IsElementVisible(static_cast<Noesis::BaseObject*>(textBlockElem)))
            continue;

        // Part 2: SEH probe.  Some NameScope entries point to partially-
        // constructed elements that crash on property access (e.g. PreviewName
        // in the Options downloads overlay).  Skip if the element is unsafe.
        if (!ProbeTextBlockElement(textBlockElem)) {
            BG3A_TRACE("[BG3Access]     NameScope: UNSAFE element '%s', skipping", elementName);
            continue;
        }

        BG3A_TRACE("[BG3Access]     NameScope: reading '%s' (elem=%p)", elementName, textBlockElem);
        auto text = ReadTextBlockText(textBlockElem, true);
        if (text.empty()) continue;
        if (text.find("[ForceUpdate]") != std::string::npos) continue;
        if (text.find("s_HandleUnknown") != std::string::npos) continue;

        BG3A_TRACE("[BG3Access]     NameScope: %s = %s", elementName, text.c_str());
        namedTexts.push_back({elementName, text});
    }
}

// ---------------------------------------------------------------------------
// PushDCProperties: enumerates ALL readable scalar properties on a
// DataContext ViewModel and pushes them as a flat Lua table.
//
// No hardcoded property names.  Iterates every TypeProperty on the DC's
// class, reads each one via ReadTypePropertyAsString, and adds non-empty
// results to the table.  Lua decides which properties matter and how to
// format them for speech.
//
// For object-typed properties (e.g. SelectedItem), attempts to read
// scalar sub-properties and pushes them as a nested table.
//
// Skips [ForceUpdate] binding placeholders.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TryReadSelectedBonusAbility: reads BonusAbilities[SelectedIndex].Ability
// from a DC that has both properties (the CC god-object).
//
// BonusAbilities doesn't go through the sub-object extraction path in
// PushDCProperties (its TypeProperty type isn't recognized as a pointer).
// This targeted function finds the collection via TypeProperty::Get(),
// casts to BaseCollection, and reads the selected item's Ability property.
//
// Called as post-processing after PushDCProperties.  Pushes
// "SelectedBonusAbility" = "Strength" (etc.) into the dcProps table.
// ---------------------------------------------------------------------------

// SEH helper: BaseCollection::Count() on a bad pointer.
static int SafeCollectionCount(Noesis::BaseCollection* collection)
{
    __try {
        return collection->Count();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Struct-based version: stores SelectedBonusAbility into dcScalarProps.
// Called from ExtractElementData path (INPC snapshots).
static void TryCollectSelectedBonusAbility(
    FocusEventData& out, Noesis::BaseObject* dc)
{
    auto dcClassType = SafeGetClassType_SEH(dc);
    if (!dcClassType) return;
    auto const& cls = Noesis::gClassCache.GetClass(dcClassType);

    Noesis::TypeProperty const* bonusProp = nullptr;
    Noesis::TypeProperty const* indexProp = nullptr;

    for (auto& entry : cls.Names) {
        if (!entry.Value().Property) continue;
        auto name = entry.Key().GetString();
        if (strcmp(name, "BonusAbilities") == 0)
            bonusProp = entry.Value().Property;
        else if (strcmp(name, "SelectedIndex") == 0)
            indexProp = entry.Value().Property;
    }
    if (!bonusProp || !indexProp) return;

    auto indexVal = ReadTypePropertyAsString(dc, indexProp);
    if (indexVal.empty()) return;
    int32_t selectedIndex = atoi(indexVal.c_str());
    if (selectedIndex < 0) return;

    auto collectionRaw = SafeGetComponent_SEH(bonusProp, dc);
    if (!collectionRaw) return;

    auto collection = static_cast<Noesis::BaseCollection*>(collectionRaw);
    int count = SafeCollectionCount(collection);
    if (selectedIndex >= count) {
        collectionRaw->Release();
        return;
    }

    auto item = SafeCollectionGetItem_SEH(collection, (uint32_t)selectedIndex);
    collectionRaw->Release();
    if (!item) return;

    auto itemClassType = SafeGetClassType_SEH(item);
    if (!itemClassType) { item->Release(); return; }

    auto const& itemCls = Noesis::gClassCache.GetClass(itemClassType);
    for (auto& entry : itemCls.Names) {
        if (!entry.Value().Property) continue;
        if (strcmp(entry.Key().GetString(), "Ability") != 0) continue;

        auto abilityVal = ReadTypePropertyAsString(
            static_cast<Noesis::BaseObject*>(item),
            entry.Value().Property);
        if (!abilityVal.empty()) {
            out.dcScalarProps.emplace_back("SelectedBonusAbility",
                std::move(abilityVal));
            item->Release();
            return;
        }
    }
    item->Release();
}

// ---------------------------------------------------------------------------
// TryCollectSelectionFlyOutTitle: reads the Title from the first group in
// ObjectCollectionList for DCSelectionFlyOut widgets.
//
// The XAML renders {Binding Title} on each group inside the outer LSListBox,
// so the title text (e.g. "Search Results") is buried inside a collection
// sub-object and not reachable by the generic CollectDCProperties path.
// This post-processor surfaces it as a top-level "CollectionTitle" scalar.
//
// SEH helpers isolate every raw pointer dereference.  __try cannot coexist
// with C++ objects that have destructors (Ptr<>), so the dangerous reads
// are extracted into standalone functions that operate on raw pointers only.
// ---------------------------------------------------------------------------

static void TryCollectSelectionFlyOutTitle(
    FocusEventData& out, Noesis::BaseObject* dc)
{
    // Only run for DCSelectionFlyOut.
    if (out.dcType.find("DCSelectionFlyOut") == std::string::npos) return;

    auto dcClassType = SafeGetClassType_SEH(dc);
    if (!dcClassType) return;
    auto const& cls = Noesis::gClassCache.GetClass(dcClassType);

    Noesis::TypeProperty const* collectionListProp = nullptr;
    for (auto& entry : cls.Names) {
        if (!entry.Value().Property) continue;
        if (strcmp(entry.Key().GetString(), "ObjectCollectionList") == 0) {
            collectionListProp = entry.Value().Property;
            break;
        }
    }
    if (!collectionListProp) return;

    // Get the collection -- SEH-guarded.
    auto collectionRaw = SafeGetComponent_SEH(collectionListProp, dc);
    if (!collectionRaw) return;

    auto collection = static_cast<Noesis::BaseCollection*>(collectionRaw);
    int count = SafeCollectionCount(collection);
    if (count <= 0) {
        collectionRaw->Release();
        return;
    }

    // Get the first group item -- SEH-guarded.
    auto item = SafeCollectionGetItem_SEH(collection, 0);
    collectionRaw->Release();
    if (!item) return;

    // Get the item's type name -- SEH-guarded.
    auto itemTypeName = SafeBaseObjectTypeName_SEH(item);
    if (!itemTypeName) {
        item->Release();
        return;
    }

    // Read Title from the item using the class cache (safe: class cache
    // lookups and ReadTypePropertyAsString are used throughout the codebase
    // on validated objects from GetComponent).
    auto const& itemCls = Noesis::gClassCache.GetClass(
        item->GetClassType());
    for (auto& entry : itemCls.Names) {
        if (!entry.Value().Property) continue;
        if (strcmp(entry.Key().GetString(), "Title") != 0) continue;

        auto titleVal = ReadTypePropertyAsString(
            static_cast<Noesis::BaseObject*>(item),
            entry.Value().Property);
        if (!titleVal.empty()
            && titleVal.find("[ForceUpdate]") == std::string::npos) {
            out.dcScalarProps.emplace_back("CollectionTitle",
                std::move(titleVal));
            break;
        }
    }

    item->Release();
}

// ---------------------------------------------------------------------------
// TryCollectFinalResult: reads FinalResult DependencyProperty from
// DCActiveRoll DataContext.  FinalResult is a DP (used in XAML
// DataTriggers for crit detection) but NOT a TypeProperty, so
// CollectDCProperties misses it.  Contains the final rolled number.
// ---------------------------------------------------------------------------
static void TryCollectFinalResult_Inner(
    FocusEventData& out, Noesis::BaseObject* dataContext)
{
    if (out.dcType.find("DCActiveRoll") == std::string::npos) return;

    auto dcClassType = SafeGetClassType_SEH(dataContext);
    if (!dcClassType) return;

    auto finalResultDP = LookupFinalResultDP(dcClassType);
    if (!finalResultDP) return;

    auto depObj = static_cast<Noesis::DependencyObject const*>(
        static_cast<Noesis::BaseComponent const*>(dataContext));
    int32_t finalResult = ReadFinalResult_SEH(depObj, finalResultDP);
    if (finalResult < 0) return;

    char resultBuffer[16];
    snprintf(resultBuffer, sizeof(resultBuffer), "%d", finalResult);
    out.dcScalarProps.emplace_back("FinalResult", std::string(resultBuffer));
}

static void TryCollectFinalResult(
    FocusEventData& out, Noesis::BaseObject* dataContext)
{
    __try {
        TryCollectFinalResult_Inner(out, dataContext);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // FinalResult unavailable -- non-fatal.
    }
}

// ---------------------------------------------------------------------------
// TryReadFinalResultDP: Lua-table version of TryCollectFinalResult.
// Pushes FinalResult into the dcProps table on the Lua stack.
// Called as post-processing after PushDCProperties.
// ---------------------------------------------------------------------------
static void TryReadFinalResultDP_Inner(
    Noesis::BaseObject* dataContext, lua_State* L, int dcPropsTableIndex)
{
    auto dcTypeName = SafeBaseObjectTypeName_SEH(dataContext);
    if (!dcTypeName || !strstr(dcTypeName, "DCActiveRoll")) return;

    auto dcClassType = SafeGetClassType_SEH(dataContext);
    if (!dcClassType) return;

    auto finalResultDP = LookupFinalResultDP(dcClassType);
    if (!finalResultDP) return;

    auto depObj = static_cast<Noesis::DependencyObject const*>(
        static_cast<Noesis::BaseComponent const*>(dataContext));
    int32_t finalResult = ReadFinalResult_SEH(depObj, finalResultDP);
    if (finalResult < 0) return;

    char resultBuffer[16];
    snprintf(resultBuffer, sizeof(resultBuffer), "%d", finalResult);
    lua_pushstring(L, "FinalResult");
    lua_pushstring(L, resultBuffer);
    lua_settable(L, dcPropsTableIndex);
}

static void TryReadFinalResultDP(
    Noesis::BaseObject* dataContext, lua_State* L, int dcPropsTableIndex)
{
    __try {
        TryReadFinalResultDP_Inner(dataContext, L, dcPropsTableIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // FinalResult unavailable -- non-fatal.
    }
}

// Lua-table version: pushes SelectedBonusAbility into dcProps table.
// Called from ExtractElementInfo path (Lua API calls).
// Read the selected ability name from BonusAbilities[SelectedIndex].
// Uses TypeProperty::GetComponent to properly unwrap Ptr<> wrappers.
// No SEH needed -- GetComponent and Count are safe when called on
// valid TypeProperties from the class cache.
// Uses SafeGetComponent_SEH and SafeCollectionGetItem_SEH for all
// collection access (GetComponent returns Ptr<> with destructor,
// preventing __try in the calling function).
static void TryReadSelectedBonusAbility(
    Noesis::BaseObject* dc, lua_State* L, int dcPropsIdx)
{
    auto dcClassType = SafeGetClassType_SEH(dc);
    if (!dcClassType) return;
    auto const& cls = Noesis::gClassCache.GetClass(dcClassType);

    Noesis::TypeProperty const* bonusProp = nullptr;
    Noesis::TypeProperty const* indexProp = nullptr;

    for (auto& entry : cls.Names) {
        if (!entry.Value().Property) continue;
        auto name = entry.Key().GetString();
        if (strcmp(name, "BonusAbilities") == 0)
            bonusProp = entry.Value().Property;
        else if (strcmp(name, "SelectedIndex") == 0)
            indexProp = entry.Value().Property;
    }
    if (!bonusProp || !indexProp) return;

    // Read SelectedIndex.
    auto indexVal = ReadTypePropertyAsString(dc, indexProp);
    if (indexVal.empty()) return;
    int32_t selectedIndex = atoi(indexVal.c_str());
    if (selectedIndex < 0) return;

    // Get the collection via SafeGetComponent_SEH (SEH-guarded).
    auto collectionRaw = SafeGetComponent_SEH(bonusProp, dc);
    if (!collectionRaw) return;

    auto collection = static_cast<Noesis::BaseCollection*>(collectionRaw);
    int count = SafeCollectionCount(collection);
    if (selectedIndex >= count) {
        collectionRaw->Release();
        return;
    }

    // Get the selected item via SafeCollectionGetItem_SEH (SEH-guarded).
    auto item = SafeCollectionGetItem_SEH(collection, (uint32_t)selectedIndex);
    collectionRaw->Release();
    if (!item) return;

    // Read Ability TypeProperty from the item.
    auto itemClassType = SafeGetClassType_SEH(item);
    if (!itemClassType) {
        item->Release();
        return;
    }
    auto const& itemCls = Noesis::gClassCache.GetClass(itemClassType);
    for (auto& entry : itemCls.Names) {
        if (!entry.Value().Property) continue;
        if (strcmp(entry.Key().GetString(), "Ability") != 0) continue;

        auto abilityVal = ReadTypePropertyAsString(
            static_cast<Noesis::BaseObject*>(item),
            entry.Value().Property);
        if (!abilityVal.empty()) {
            lua_pushstring(L, "SelectedBonusAbility");
            lua_pushstring(L, abilityVal.c_str());
            lua_settable(L, dcPropsIdx);
            item->Release();
            return;
        }
    }
    item->Release();
}


static void PushDCProperties(lua_State* L, Noesis::BaseObject* dc);

// SEH-safe wrapper: calls PushDCProperties and catches faults from stale
// DC pointers (e.g., inspect panel ContentPresenters with deallocated VMs).
// Saves the Lua stack top before the call; on fault, restores the stack
// and pushes nil so the caller gets a clean dcProps=nil instead of losing
// the entire tick frame.
static bool SafePushDCProperties_SEH(lua_State* L, Noesis::BaseObject* dc,
                                      int savedTop)
{
    __try {
        PushDCProperties(L, dc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Restore Lua stack to pre-call state and push nil.
        lua_settop(L, savedTop);
        lua_pushnil(L);
        return false;
    }
}

// PushDCProperties: delegates to CollectDCProperties_Inner (single source
// of truth for DC property enumeration) and converts the result to Lua.
// Fixes the SEH gap in the old implementation: all property reads now go
// through SafeTypePropertyGet_SEH via CollectDCProperties_Inner.
static void PushDCProperties(lua_State* L, Noesis::BaseObject* dc)
{
    if (!dc) {
        lua_pushnil(L);
        return;
    }

    // Collect all properties into a FocusEventData struct.
    FocusEventData tempData;
    CollectDCProperties_Inner(tempData, dc);

    // Convert to Lua table: scalars as flat key=value pairs.
    lua_newtable(L);

    for (auto const& scalar : tempData.dcScalarProps) {
        lua_pushstring(L, scalar.first.c_str());
        lua_pushstring(L, scalar.second.c_str());
        lua_settable(L, -3);
    }

    // Sub-objects as nested tables with _type field.
    for (auto const& subObj : tempData.dcObjectProps) {
        lua_newtable(L);
        lua_pushstring(L, "_type");
        lua_pushstring(L, subObj.typeName.c_str());
        lua_settable(L, -3);
        for (auto const& prop : subObj.props) {
            lua_pushstring(L, prop.first.c_str());
            lua_pushstring(L, prop.second.c_str());
            lua_settable(L, -3);
        }
        lua_pushstring(L, subObj.propName.c_str());
        lua_insert(L, -2);
        lua_settable(L, -3);
    }

    // Collections as indexed arrays of sub-tables.
    for (auto const& coll : tempData.dcCollectionProps) {
        lua_newtable(L);
        int luaArrayIndex = 1;
        for (auto const& item : coll.items) {
            lua_newtable(L);
            lua_pushstring(L, "_type");
            lua_pushstring(L, item.typeName.c_str());
            lua_settable(L, -3);
            for (auto const& prop : item.props) {
                lua_pushstring(L, prop.first.c_str());
                lua_pushstring(L, prop.second.c_str());
                lua_settable(L, -3);
            }
            lua_rawseti(L, -2, luaArrayIndex++);
        }
        lua_pushstring(L, coll.propName.c_str());
        lua_insert(L, -2);
        lua_settable(L, -3);
    }
}

// ---------------------------------------------------------------------------
// ExtractTabName: tab name extraction in C++.
//
// Mirrors the Lua ExtractTabName function with 6 fallback approaches:
// 1. DataContext properties (Title, Text, Name, Label, Header)
// 2. Content property
// 3. Header property
// 4. Visual tree walk for TextBlocks
// 5. ToString() on element
// 6. Element Name cleanup (last resort)
// ---------------------------------------------------------------------------
static std::string ExtractTabName_Inner(Noesis::FrameworkElement* elem)
{
    if (!elem) return {};

    // Try 1: DataContext properties -- enumerate TypeProperties but only
    // accept title-like names (Title, Text, Name, Label, Header).
    // Uses ReadTypePropertyAsString (safe, from class cache) NOT
    // ReadPropertyAsString (name-based lookup, crashes on some DCs).
    // Filtering by name prevents random DC properties like "LobbyMessage"
    // from being returned as tab names when ListBoxItems inherit the
    // parent widget's DC.
    if (sDataContextProp) {
        auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
        auto dataContext = SafeReadDC_SEH(depObj);
        if (dataContext) {
            auto dcClassType = SafeGetClassType_SEH(dataContext);
            if (dcClassType) {
                auto& types = Noesis::gStaticSymbols.Types;
                auto const& cls = Noesis::gClassCache.GetClass(dcClassType);
                for (auto& entry : cls.Names) {
                    if (!entry.Value().Property) continue;
                    // Only accept title-like property names.
                    auto propName = entry.Key().GetString();
                    if (strcmp(propName, "Title") != 0
                        && strcmp(propName, "Text") != 0
                        && strcmp(propName, "Name") != 0
                        && strcmp(propName, "Label") != 0
                        && strcmp(propName, "Header") != 0)
                        continue;
                    auto type = UnwrapType(entry.Value().Property->GetContentType());
                    if (!type) continue;
                    if (type == types.String.Type
                        || type == types.CStringPtr.Type
                        || type == types.LocaString.Type) {
                        auto val = ReadTypePropertyAsString(dataContext, entry.Value().Property);
                        if (!val.empty()
                            && val.find("[ForceUpdate]") == std::string::npos) {
                            return val;
                        }
                    }
                }
            }
        }
    }

    // Try 2: Content property.
    auto content = ReadPropertyAsString(elem, "Content");
    if (!content.empty()) return content;

    // Try 3: Header property.
    auto header = ReadPropertyAsString(elem, "Header");
    if (!header.empty()) return header;

    // Try 4: Visual tree walk for TextBlocks.
    // Walk up to 50 levels deep, return first TextBlock text found.
    {
        // BFS through visual children looking for TextBlocks.
        std::vector<Noesis::Visual*> queue(256);
        int front = 0, back = 0;
        auto seedCount = SafeGetVisualChildrenCount_SEH(elem);
        for (uint32_t i = 0; i < seedCount && back < 256; i++) {
            auto child = SafeGetVisualChild_SEH(elem, i);
            if (child) queue[back++] = child;
        }
        for (int level = 0; level < 10 && front < back; level++) {
            int levelEnd = back;
            while (front < levelEnd) {
                auto cur = queue[front++];
                auto curTypeName = SafeBaseObjectTypeName_SEH(cur);
                if (!curTypeName) continue;
                if (strstr(curTypeName, "TextBlock")) {
                    auto tbText = ReadTextBlockText(
                        static_cast<Noesis::FrameworkElement*>(cur));
                    if (!tbText.empty()) return tbText;
                }
                // Enqueue children -- SEH-guarded.
                auto cc = SafeGetVisualChildrenCount_SEH(cur);
                for (uint32_t i = 0; i < cc && back < 256; i++) {
                    auto child = SafeGetVisualChild_SEH(cur, i);
                    if (child) queue[back++] = child;
                }
            }
        }
    }

    // Try 5: ToString() on element -- SEH-guarded.
    {
        char strBuf[512];
        if (SafeToString_SEH(elem, strBuf, sizeof(strBuf))) {
            std::string str(strBuf);
            auto tn = SafeBaseObjectTypeName_SEH(elem);
            const char* typeName = tn ? tn : "";
            if (str != typeName
                && str.find("Noesis::") != 0
                && str.find("ls.") != 0) {
                return str;
            }
        }
    }

    // Try 6: Element Name cleanup (last resort).
    {
        auto name = ReadPropertyAsString(elem, "Name");
        if (!name.empty()) {
            // Remove Button/Btn/Tab suffixes, insert spaces before capitals.
            std::string cleaned = name;
            // Remove common suffixes.
            auto removeSuffix = [&](const char* suffix) {
                auto slen = strlen(suffix);
                if (cleaned.size() > slen
                    && cleaned.compare(cleaned.size() - slen, slen, suffix) == 0) {
                    cleaned.erase(cleaned.size() - slen);
                }
            };
            removeSuffix("Button");
            removeSuffix("Btn");
            removeSuffix("Tab");
            // Insert spaces before uppercase letters (camelCase -> words).
            std::string spaced;
            for (size_t i = 0; i < cleaned.size(); i++) {
                if (i > 0 && isupper(cleaned[i]) && islower(cleaned[i - 1])) {
                    spaced += ' ';
                }
                spaced += cleaned[i];
            }
            if (!spaced.empty()) return spaced;
        }
    }

    return {};
}

static void ExtractTabName_Invoke(Noesis::FrameworkElement* elem, std::string* outStr) {
    *outStr = ExtractTabName_Inner(elem);
}
static bool SafeExtractTabName_SEH(Noesis::FrameworkElement* elem, std::string* outStr) {
    __try { ExtractTabName_Invoke(elem, outStr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static std::string ExtractTabName(Noesis::FrameworkElement* elem) {
    std::string result;
    if (!SafeExtractTabName_SEH(elem, &result)) result.clear();
    return result;
}

// ---------------------------------------------------------------------------
// FindWidgetRootId: walks the visual parent chain (max 64 hops) to find
// the nearest UIWidget ancestor.  Returns a hex pointer string for use
// as a stable widget identity token.  Returns empty if no UIWidget found.
// SEH-guarded: SafeGetVisualParent_SEH / SafeIsUIWidgetType_SEH handle
// stale pointers during UI rebuilds.
// ---------------------------------------------------------------------------
// _Inner: uses std::string (destructor), cannot live inside __try.
static std::string FindWidgetRootId_Inner(Noesis::Visual* startElement)
{
    Noesis::Visual* current = startElement;
    for (int hop = 0; hop < 64 && current; hop++) {
        if (SafeIsUIWidgetType_SEH(current)) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%p",
                     static_cast<void*>(current));
            return std::string(buffer);
        }
        current = SafeGetVisualParent_SEH(current);
    }
    return {};
}
// _Invoke: no destructors, SEH-compatible.
static void FindWidgetRootId_Invoke(Noesis::Visual* startElement,
                                     std::string* outResult)
{
    *outResult = FindWidgetRootId_Inner(startElement);
}
// SEH wrapper: no C++ objects with destructors in this frame.
static bool SafeFindWidgetRootId_SEH(Noesis::Visual* startElement,
                                      std::string* outResult)
{
    __try {
        FindWidgetRootId_Invoke(startElement, outResult);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Public API: owns the std::string, calls SEH wrapper.
static std::string FindWidgetRootId(Noesis::Visual* startElement)
{
    std::string result;
    if (!SafeFindWidgetRootId_SEH(startElement, &result))
        result.clear();
    return result;
}

// ---------------------------------------------------------------------------
// ExtractElementInfo: builds a Lua table with all fields from a focused
// element.  Called during Tick() when the element is alive.
//
// Pushes a table onto the Lua stack with:
//   elemType, elemName, elemId, isTab, isOption,
//   text, tabName, dcType, dcBody, widgetRootId, isFocusable
// ---------------------------------------------------------------------------
static void ExtractElementInfo_Inner(lua_State* L, Noesis::FrameworkElement* elem)
{
    if (!elem) {
        lua_pushnil(L);
        return;
    }

    lua_newtable(L);

    // elemType: GetClassType()->GetName() -- SEH-guarded.
    auto typeName = SafeBaseObjectTypeName_SEH(elem);
    if (!typeName) typeName = "Unknown";
    lua_pushstring(L, "elemType");
    lua_pushstring(L, typeName);
    lua_settable(L, -3);

    // elemName: x:Name
    auto elemName = ReadPropertyAsString(elem, "Name");
    lua_pushstring(L, "elemName");
    if (!elemName.empty()) {
        lua_pushstring(L, elemName.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // isTab: IsListBoxItemType check
    bool isTab = IsListBoxItemType(elem);
    lua_pushstring(L, "isTab");
    lua_pushboolean(L, isTab);
    lua_settable(L, -3);

    // isFocusable: ls:MoveFocus.Focusable -- SEH-guarded.
    bool isFocusable = false;
    if (sLSMoveFocusFocusableProp) {
        auto depObj = static_cast<Noesis::DependencyObject*>(elem);
        auto val = SafeGetDPValue_SEH(sLSMoveFocusFocusableProp, depObj);
        isFocusable = val && *static_cast<const bool*>(val);
    }
    lua_pushstring(L, "isFocusable");
    lua_pushboolean(L, isFocusable);
    lua_settable(L, -3);

    // DataContext reading -- SEH-guarded at each step.
    Noesis::BaseComponent* dataContext = nullptr;
    std::string dcTypeName;
    if (sDataContextProp) {
        auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
        dataContext = SafeReadDC_SEH(depObj);
        if (dataContext) {
            auto dcTypeNamePtr = SafeBaseObjectTypeName_SEH(dataContext);
            if (dcTypeNamePtr) {
                dcTypeName = dcTypeNamePtr;
            } else {
                // Type name unreadable -- DC pointer may be stale.
                dataContext = nullptr;
            }
        }
    }

    // dcType
    lua_pushstring(L, "dcType");
    if (!dcTypeName.empty()) {
        lua_pushstring(L, dcTypeName.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // dcProps: ALL readable DC properties as a flat table.
    // No hardcoded property names -- C++ enumerates everything on the
    // ViewModel, Lua decides which properties matter and how to format.
    // SEH-guarded: stale DC pointers (e.g., inspect panel) produce nil
    // instead of crashing the entire tick frame.
    lua_pushstring(L, "dcProps");
    if (dataContext) {
        int prePropsTop = lua_gettop(L);
        if (SafePushDCProperties_SEH(L, dataContext, prePropsTop)) {
            // Post-process: read selected ability name for Ability Bonus selector.
            // BonusAbilities collection isn't handled by the generic sub-object
            // path (its TypeProperty type isn't recognized as a pointer).
            TryReadSelectedBonusAbility(dataContext, L, lua_gettop(L));
            TryReadFinalResultDP(dataContext, L, lua_gettop(L));
        }
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // elemText: text from the element itself (TextBlock, Content, ToString).
    // Separate from dcProps so Lua has both the ViewModel data AND any
    // rendered text from the visual element.
    std::string elemText;
    if (strstr(typeName, "TextBlock")) {
        elemText = ReadTextBlockText(elem);
    }
    if (elemText.empty()) {
        elemText = ReadPropertyAsString(elem, "Content");
    }
    if (elemText.empty()) {
        char strBuf[512];
        if (SafeToString_SEH(elem, strBuf, sizeof(strBuf))) {
            std::string str(strBuf);
            if (str != typeName
                && str.find("Noesis::") != 0
                && str.find("ls.") != 0
                && str.find("[ForceUpdate]") == std::string::npos) {
                elemText = str;
            }
        }
    }
    lua_pushstring(L, "elemText");
    if (!elemText.empty()) {
        lua_pushstring(L, elemText.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // tabName: only for tabs.
    lua_pushstring(L, "tabName");
    if (isTab) {
        auto tn = ExtractTabName(elem);
        if (!tn.empty()) {
            lua_pushstring(L, tn.c_str());
        } else {
            lua_pushnil(L);
        }
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // widgetRootId: shared parent walk to nearest UIWidget ancestor.
    lua_pushstring(L, "widgetRootId");
    {
        auto widgetRootId = FindWidgetRootId(elem);
        if (!widgetRootId.empty()) {
            lua_pushstring(L, widgetRootId.c_str());
        } else {
            lua_pushnil(L);
        }
    }
    lua_settable(L, -3);

    // elemId: "TypeName::Name::Text" composite identifier.
    lua_pushstring(L, "elemId");
    {
        std::string id = typeName;
        if (!elemName.empty()) {
            id += "::" + elemName;
        }
        if (!elemText.empty()) {
            // Truncate text for the ID to keep it manageable.
            auto truncText = elemText.substr(0, 60);
            id += "::" + truncText;
        }
        lua_pushstring(L, id.c_str());
    }
    lua_settable(L, -3);
}

// ---------------------------------------------------------------------------
// CollectDCProperties: fills FocusEventData's dcScalarProps and dcObjectProps
// from a DataContext object.  Pure C++ -- no Lua stack interaction.
//
// This is the SINGLE source of truth for DC property enumeration.
// PushDCProperties (Lua output) delegates to this function.
//
// Uses IsScalarValueType for consistent type handling at all levels
// (top-level, sub-object, collection item).
// Uses SafeTypePropertyGet_SEH for all pointer dereferences.
// ---------------------------------------------------------------------------
static void CollectDCProperties_Inner(FocusEventData& out, Noesis::BaseObject* dc)
{
    if (!dc) return;

    auto dcClassType = SafeGetClassType_SEH(dc);
    if (!dcClassType) return;
    auto const& cls = Noesis::gClassCache.GetClass(dcClassType);
    auto& types = Noesis::gStaticSymbols.Types;

    for (auto& entry : cls.Names) {
        auto propInfo = &entry.Value();
        if (!propInfo->Property) continue;

        auto type = UnwrapType(propInfo->Property->GetContentType());
        if (!type) continue;

        auto typeOfType = type->GetClassType();

        // Scalar types (including enums): read as string
        if (IsScalarValueType(type)) {
            auto val = ReadTypePropertyAsString(dc, propInfo->Property);
            if (!val.empty()
                && val.find("[ForceUpdate]") == std::string::npos) {
                out.dcScalarProps.emplace_back(
                    entry.Key().GetString(), std::move(val));
            }
            continue;
        }

        // Object types: try to read scalar sub-properties
        bool isPtr = (typeOfType == types.TypePtr.Type)
                  || (typeOfType == types.TypePointer.Type)
                  || Noesis::TypeHelpers::IsDescendantOf(
                         type, Noesis::gStaticSymbols.TypeClasses.BaseObject.Type);
        if (isPtr) {
            Noesis::BaseObject* subObj = nullptr;
            if (typeOfType == types.TypePtr.Type) {
                auto ptrVal = reinterpret_cast<Noesis::Ptr<Noesis::BaseRefCounted>*>(
                    const_cast<void*>(SafeTypePropertyGet_SEH(propInfo->Property, dc)));
                if (ptrVal) subObj = static_cast<Noesis::BaseObject*>(ptrVal->GetPtr());
            } else if (typeOfType == types.TypePointer.Type) {
                if (!SafeTypePropertyGetCopy_SEH(propInfo->Property, dc, &subObj))
                    subObj = nullptr;
            } else {
                subObj = reinterpret_cast<Noesis::BaseObject*>(
                    const_cast<void*>(SafeTypePropertyGet_SEH(propInfo->Property, dc)));
            }

            if (subObj) {
                auto subClassType = SafeGetClassType_SEH(subObj);
                if (!subClassType) continue;

                // Collection types: enumerate items instead of reading
                // the collection object's own TypeProperties.
                // Produces dcCollectionProps[propName] = array of item
                // sub-tables, each with their own scalar properties.
                if (Noesis::TypeHelpers::IsDescendantOf(
                        subClassType,
                        Noesis::gStaticSymbols.TypeClasses.BaseCollection.Type)) {
                    auto collection = static_cast<Noesis::BaseCollection*>(subObj);
                    int itemCount = SafeCollectionCount(collection);
                    if (itemCount > 0) {
                        FocusEventData::CollectionProperty collectionProp;
                        collectionProp.propName = entry.Key().GetString();
                        int itemLimit = (itemCount > 20) ? 20 : itemCount;
                        for (int itemIndex = 0; itemIndex < itemLimit; itemIndex++) {
                            auto collectionItem = SafeCollectionGetItem_SEH(
                                collection, static_cast<uint32_t>(itemIndex));
                            if (!collectionItem) continue;
                            auto itemClassType = SafeGetClassType_SEH(collectionItem);
                            if (!itemClassType) {
                                collectionItem->Release();
                                continue;
                            }
                            FocusEventData::CollectionItem itemData;
                            itemData.typeName = itemClassType->GetName();
                            auto const& itemClass = Noesis::gClassCache.GetClass(itemClassType);
                            for (auto& itemEntry : itemClass.Names) {
                                if (!itemEntry.Value().Property) continue;
                                auto itemType = UnwrapType(
                                    itemEntry.Value().Property->GetContentType());
                                if (!IsScalarValueType(itemType)) continue;
                                auto itemValue = ReadTypePropertyAsString(
                                    static_cast<Noesis::BaseObject*>(collectionItem),
                                    itemEntry.Value().Property);
                                if (!itemValue.empty()
                                    && itemValue.find("[ForceUpdate]") == std::string::npos) {
                                    itemData.props.emplace_back(
                                        itemEntry.Key().GetString(),
                                        std::move(itemValue));
                                }
                            }
                            collectionItem->Release();
                            if (!itemData.props.empty()) {
                                collectionProp.items.push_back(std::move(itemData));
                            }
                        }
                        if (!collectionProp.items.empty()) {
                            out.dcCollectionProps.push_back(std::move(collectionProp));
                        }
                    }
                    continue;
                }

                // Regular sub-object: read scalar sub-properties.
                auto const& subCls = Noesis::gClassCache.GetClass(subClassType);
                FocusEventData::SubObject subData;
                subData.propName = entry.Key().GetString();
                subData.typeName = subClassType->GetName();

                for (auto& subEntry : subCls.Names) {
                    if (!subEntry.Value().Property) continue;
                    auto subType = UnwrapType(subEntry.Value().Property->GetContentType());
                    if (!IsScalarValueType(subType)) continue;
                    auto subVal = ReadTypePropertyAsString(subObj, subEntry.Value().Property);
                    if (!subVal.empty()
                        && subVal.find("[ForceUpdate]") == std::string::npos) {
                        subData.props.emplace_back(
                            subEntry.Key().GetString(), std::move(subVal));
                    }
                }

                if (!subData.props.empty()) {
                    out.dcObjectProps.push_back(std::move(subData));
                }
            }
        }
    }
}

// SEH wrapper: CollectDCProperties iterates TypeProperties which can
// fault on stale DataContext objects.
static void CollectDCProperties(FocusEventData& out, Noesis::BaseObject* dc)
{
    __try {
        CollectDCProperties_Inner(out, dc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // DC properties remain incomplete/empty -- safe fallback.
    }
}

// ---------------------------------------------------------------------------
// ExtractBindingInfo: extracts ALL binding expressions from the element's
// mValues.  For each DP that has a binding, reports the DP name, binding
// path, and cached resolved value.
//
// No filtering -- C++ reports everything, Lua decides what's relevant
// based on the binding path (e.g., Path="Title" IS a title).
//
// Single pass through mValues.  No tree walking.  No Noesis API calls
// that require exported symbols -- all data accessed via direct member
// reads (same pattern as mValues, mVisualParent throughout this codebase).
// ---------------------------------------------------------------------------
// SEH wrapper: reads a single binding entry's path string and resolved value.
// Isolated from C++ objects so __try is legal.  Returns false on access violation.
static bool TryReadBindingEntry(
    Noesis::DependencyObject* depObj,
    Noesis::DependencyProperty const* depProp,
    Noesis::StoredValue* storedVal,
    const char** outDepPropName,
    const char** outBindingPath,
    uint32_t* outPathLen,
    std::string* outResolvedValue)
{
    __try {
        auto expression = storedVal->value.complex->expression.GetPtr();
        if (!expression) return false;

        auto bindingExpr = static_cast<Noesis::BaseBindingExpression*>(expression);
        auto baseBinding = bindingExpr->mBinding.GetPtr();
        if (!baseBinding) return false;

        auto binding = static_cast<Noesis::Binding*>(baseBinding);
        auto propertyPath = binding->mPath.GetPtr();
        if (!propertyPath) return false;

        auto& bindingPathStr = propertyPath->mPath;
        *outPathLen = bindingPathStr.Size();
        if (*outPathLen == 0) return false;

        *outBindingPath = bindingPathStr.Str();
        *outDepPropName = depProp->GetName().Str();
        if (!*outDepPropName) return false;

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Stale binding pointer -- harmless, skip silently.
        return false;
    }
}

static void ExtractBindingInfo_Inner(FocusEventData& out, Noesis::FrameworkElement* elem)
{
    if (!elem) return;

    auto depObj = static_cast<Noesis::DependencyObject*>(elem);

    for (auto entry = depObj->mValues.Begin(); entry != depObj->mValues.End(); ++entry) {
        auto depProp = entry->key;
        if (!depProp) continue;

        auto storedVal = entry->value;
        if (!storedVal || !storedVal->flags.isInitialized) continue;
        if (!storedVal->flags.isExpression || !storedVal->flags.isComplex) continue;

        const char* depPropName = nullptr;
        const char* bindingPath = nullptr;
        uint32_t pathLen = 0;
        std::string resolvedValue;

        if (!TryReadBindingEntry(depObj, depProp, storedVal,
                &depPropName, &bindingPath, &pathLen, &resolvedValue)) {
            continue;
        }

        // Read the resolved value (outside SEH -- uses std::string).
        resolvedValue = ReadDepPropertyAsString(depObj, depProp);
        if (resolvedValue.find("[ForceUpdate]") != std::string::npos) continue;

        FocusEventData::BindingInfo info;
        info.propertyName = depPropName;
        info.bindingPath = std::string(bindingPath);
        info.resolvedValue = std::move(resolvedValue);
        out.bindings.push_back(std::move(info));
    }
}

// SEH wrapper: mValues iteration can fault on stale elements.
static void ExtractBindingInfo(FocusEventData& out, Noesis::FrameworkElement* elem)
{
    __try {
        ExtractBindingInfo_Inner(out, elem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Bindings remain incomplete/empty -- safe fallback.
    }
}

// ---------------------------------------------------------------------------
// ReadTemplatedParentTag_SEH: SEH-guarded read of the TemplatedParent's Tag
// property.  Returns the raw Tag object pointer so the caller can call
// ToString outside SEH (ToString returns std::string which has a destructor).
// No C++ objects with destructors in this function.
// ---------------------------------------------------------------------------
static Noesis::BaseObject* ReadTemplatedParentTag_SEH(
    Noesis::FrameworkElement* elem,
    const Noesis::DependencyProperty* tagDp)
{
    __try {
        if (!elem || !tagDp) return nullptr;
        auto templatedParent = elem->mTemplatedParent;
        if (!templatedParent) return nullptr;
        if (!ProbeUIElement(static_cast<Noesis::UIElement*>(templatedParent)))
            return nullptr;

        // Read the Tag DP value from the TemplatedParent.
        auto depObj = static_cast<Noesis::DependencyObject*>(templatedParent);
        auto entry = depObj->mValues.Find(tagDp);
        if (entry == depObj->mValues.End()) return nullptr;

        auto storedVal = entry->value;
        if (!storedVal || !storedVal->flags.isInitialized) return nullptr;

        void* rawVal = nullptr;
        if (storedVal->flags.isComplex) {
            rawVal = storedVal->value.complex->base;
        } else {
            rawVal = storedVal->value.simple;
        }
        if (!rawVal) return nullptr;

        return reinterpret_cast<Noesis::BaseObject*>(rawVal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] ReadTemplatedParentTag_SEH: fault");
        return nullptr;
    }
}

// SEH wrapper: protects Lua stack from crashes during element data extraction.
static void ExtractElementInfo(lua_State* L, Noesis::FrameworkElement* elem)
{
    int top = lua_gettop(L);
    __try {
        ExtractElementInfo_Inner(L, elem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_settop(L, top);
        lua_pushnil(L);
    }
}

// ---------------------------------------------------------------------------
// ExtractElementData: fills a FocusEventData struct from a FrameworkElement.
// Called during Tick() when the element is alive.  Pure C++ -- no Lua.
// Mirrors ExtractElementInfo but stores into the struct.
// ---------------------------------------------------------------------------
static void ExtractElementData_Inner(FocusEventData& out, Noesis::FrameworkElement* elem)
{
    if (!elem) return;

    // elemType
    auto elemClassTypeName = SafeBaseObjectTypeName_SEH(elem);
    if (!elemClassTypeName) return;
    out.elemType = elemClassTypeName;

    // elemAddr: stable element pointer (survives text/DC changes)
    {
        char addrBuf[20];
        snprintf(addrBuf, sizeof(addrBuf), "%p", elem);
        out.elemAddr = addrBuf;
    }

    // elemName
    out.elemName = ReadPropertyAsString(elem, "Name");

    // isTab
    out.isTab = IsListBoxItemType(elem);

    // isFocusable
    if (sLSMoveFocusFocusableProp) {
        auto depObj = static_cast<Noesis::DependencyObject*>(elem);
        auto val = SafeGetDPValue_SEH(sLSMoveFocusFocusableProp, depObj);
        out.isFocusable = val && *static_cast<const bool*>(val);
    }

    // isChecked: for ToggleButton-derived elements (CheckBox, Expander toggle).
    // First try the Expander ancestor walk (reads IsExpanded DP on the
    // parent Expander).  If no Expander found (-1), fall back to reading
    // IsChecked directly on the element itself (standalone CheckBox).
    {
        auto typeName = SafeBaseObjectTypeName_SEH(elem);
        if (typeName && (strstr(typeName, "ToggleButton")
            || strstr(typeName, "CheckBox"))) {
            out.isChecked = SafeReadExpanderState_SEH(elem);
            // Fallback: standalone CheckBox (no Expander ancestor).
            if (out.isChecked < 0) {
                out.isChecked = SafeReadToggleIsChecked_SEH(elem);
            }
        }
    }

    // DataContext
    auto dataContext = SafeReadDC_SEH(
        static_cast<Noesis::DependencyObject const*>(elem));
    if (dataContext) {
        auto dcTypeName = SafeBaseObjectTypeName_SEH(dataContext);
        if (dcTypeName) {
            out.dcType = dcTypeName;
        }
    }

    // DC properties
    if (dataContext) {
        CollectDCProperties(out, dataContext);
        // Post-process: read selected ability name from BonusAbilities
        // collection if present.  Adds SelectedBonusAbility to dcScalarProps.
        TryCollectSelectedBonusAbility(out, dataContext);
        // Post-process: read ObjectCollectionList[0].Title for
        // DCSelectionFlyOut.  Adds CollectionTitle to dcScalarProps.
        TryCollectSelectionFlyOutTitle(out, dataContext);
        // Post-process: read FinalResult DP from DCActiveRoll.
        // FinalResult is a DP, not a TypeProperty, so CollectDCProperties
        // misses it.  Contains the final rolled number.
        TryCollectFinalResult(out, dataContext);
    }

    // TemplatedParent Tag: in DataTemplate-hosted elements (e.g. Examine
    // panel stat rows), the label comes from the ContentPresenter's Tag
    // property, bound via RelativeSource TemplatedParent.  Read it here
    // so Lua can use it as the stat label without hardcoded DC type maps.
    if (sTagProp) {
        auto tagObj = ReadTemplatedParentTag_SEH(elem, sTagProp);
        if (tagObj) {
            char tagBuf[512];
            if (SafeToString_SEH(tagObj, tagBuf, sizeof(tagBuf))) {
                std::string tagStr(tagBuf);
                if (!tagStr.empty()
                    && tagStr.find("[ForceUpdate]") == std::string::npos
                    && tagStr.find("s_HandleUnknown") == std::string::npos
                    && tagStr.find("Noesis::") != 0
                    && tagStr.find("ls.") != 0) {
                    out.dcScalarProps.push_back(std::make_pair(std::string("TemplatedParentTag"), std::move(tagStr)));
                }
            }
        }
    }

    // elemText: primary extraction -- direct property reads.
    if (strstr(out.elemType.c_str(), "TextBlock")) {
        out.elemText = ReadTextBlockText(elem);
    }
    // Content property: for buttons and controls with text Content set
    // directly (not via a visual child template).
    if (out.elemText.empty()) {
        out.elemText = ReadPropertyAsString(elem, "Content");
        // Filter out container element type names returned when Content
        // holds a visual child (Grid, StackPanel, etc.) instead of text.
        // ToString on these returns the container type name, not useful text.
        // Clearing lets TryShallowChildTextScan find the actual TextBlock.
        if (!out.elemText.empty()) {
            static const char* containerTypeNames[] = {
                "Grid", "StackPanel", "DockPanel", "Canvas", "Border",
                "WrapPanel", "UniformGrid", "VirtualizingStackPanel",
                "ScrollViewer", "Viewbox", "ContentPresenter",
                nullptr
            };
            for (auto name = containerTypeNames; *name; ++name) {
                if (out.elemText == *name) {
                    out.elemText.clear();
                    break;
                }
            }
        }
    }
    // ToString on the element itself.  Since the Norbyte upstream rebase,
    // ContentControl.ToString() returns type descriptors like
    // "ContentControl: Grid" instead of rendered text.  Filter by checking
    // if the result starts with the element's own type name.
    if (out.elemText.empty()) {
        char strBuf[512];
        if (SafeToString_SEH(elem, strBuf, sizeof(strBuf))) {
            std::string str(strBuf);
            if (str != out.elemType
                && str.find(out.elemType) != 0
                && str.find("Noesis::") != 0
                && str.find("ls.") != 0
                && str.find("[ForceUpdate]") == std::string::npos) {
                out.elemText = str;
            }
        }
    }

    // elemText: fallback -- bounded BFS through visual children.
    // For ContentControl/Control elements (carousel selectors, template-
    // driven controls), the displayed text lives in template-generated
    // TextBlock children, not in any direct property.  Walk up to 5 levels
    // deep with a 64-node queue to find the first meaningful TextBlock.
    // Same pattern as ExtractTabName Try 4 but with tighter bounds.
    // SEH-guarded because visual children may be partially destroyed
    // during UI rebuilds (race/class tab switches in character creation).
    if (out.elemText.empty()) {
        out.elemText = TryShallowChildTextScan(elem);
    }

    // templateTexts: all visible TextBlock texts from template children.
    // Bounded walk (8 levels, 64 nodes) captures converter outputs and
    // DataTrigger-shown labels without needing C++ enum resolution.
    // Gives Lua the full rendered text of the element's template.
    // Only run when DC exists -- transient elements during loading have
    // no DC and their visual trees may be partially constructed.
    // SEH-guarded: probe element before walking its visual children.
    if (dataContext && ProbeTextBlockElement(elem)) {
        GatherVisibleTextBlocks(elem, out.templateTexts, 8, 64);
    }

    // tabName
    if (out.isTab) {
        out.tabName = ExtractTabName(elem);
    }

    // widgetRootId -- shared parent walk to nearest UIWidget ancestor.
    out.widgetRootId = FindWidgetRootId(elem);

    // ancestorContext: walk a few parents looking for x:Name containing
    // "Melee" or "Ranged".  Qualifies stats like "Attack Bonus" so Lua
    // can prefix with the attack type.  Own SEH block -- parent Names
    // are separate from the widgetRootId walk above.
    {
        Noesis::Visual* cur = elem;
        for (int i = 0; i < 6 && cur; i++) {
            auto parent = SafeGetVisualParent_SEH(cur);
            if (!parent) break;
            auto parentFE = static_cast<Noesis::FrameworkElement*>(parent);
            std::string parentName;
            if (SafeReadPropertyAsString_SEH(parentFE, "Name", &parentName)
                && !parentName.empty()) {
                if (parentName.find("Melee") != std::string::npos) {
                    out.ancestorContext = "Melee";
                    break;
                }
                if (parentName.find("Ranged") != std::string::npos) {
                    out.ancestorContext = "Ranged";
                    break;
                }
            }
            cur = parent;
        }
    }

    // Binding metadata: semantic roles from XAML binding paths.
    ExtractBindingInfo(out, elem);

    // elemId: composite identifier for dedup.
    // For unnamed elements, include DC text to distinguish items with
    // the same type (e.g. ContentPresenters in Options menu).
    // When multiple elements share the same type::name::text, the DC
    // pointer address is appended to make the ID unique per DC object
    // (e.g. save game entries all named "Tav" but with different VMs).
    {
        std::string id = out.elemType;
        if (!out.elemName.empty()) {
            id += "::" + out.elemName;
        }
        // Use DC text for identity (matches Lua GetElementId behavior)
        std::string dcText;
        for (auto const& kv : out.dcScalarProps) {
            if (kv.first == "Text") { dcText = kv.second; break; }
        }
        if (dcText.empty()) {
            for (auto const& kv : out.dcScalarProps) {
                if (kv.first == "Title") { dcText = kv.second; break; }
            }
        }
        if (!dcText.empty()) {
            auto trunc = dcText.substr(0, 60);
            id += "::" + trunc;
        } else if (!out.elemText.empty()) {
            auto trunc = out.elemText.substr(0, 60);
            id += "::" + trunc;
        }
        // Append DC pointer address to ensure uniqueness when multiple
        // elements have the same type, name, and text (e.g. save game
        // ExpanderButtons all showing "Tav" for the same character).
        auto dc = GetDataContext(elem);
        if (dc) {
            char addrBuf[20];
            snprintf(addrBuf, sizeof(addrBuf), "@%p", dc);
            id += addrBuf;
        }
        out.elemId = std::move(id);
    }
}

// SEH wrapper: ExtractElementData touches stale DCs, mValues, visual trees.
static void ExtractElementData(FocusEventData& out, Noesis::FrameworkElement* elem)
{
    __try {
        ExtractElementData_Inner(out, elem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Whatever fields succeeded remain intact.
    }
}

// ---------------------------------------------------------------------------
// GetFocusedElementInfo: Lua-exposed API that returns the data table for
// the currently focused element.  Uses all three focus strategies, then
// calls ExtractElementInfo to build the table.
//
// Returns a table on success, nil if nothing is focused.
// ---------------------------------------------------------------------------
UserReturn GetFocusedElementInfo(lua_State* L)
{
    // Find the focused element using all three strategies.
    // Wrapped in __try because this is a Lua API (MODULE_FUNCTION)
    // called from outside TickGlobalFocusMonitor's top-level SEH.
    Noesis::UIElement* focused = nullptr;
    __try {
        auto root = GetRoot();
        if (!root) {
            lua_pushnil(L);
            return 1;
        }

        InitFocusProperties(root);

        // Strategy 1: FocusManager.FocusedElement
        focused = TryFocusManager(root, GlobalFocusMonitor::kMaxTreeDepth);
        if (!focused && (sIsFocusedProp || sLSMoveFocusIsFocusedProp)) {
            // Strategy 2: IsFocused tree walk
            focused = FindFocusedInTree(root, GlobalFocusMonitor::kMaxTreeDepth);
        }
        if (!focused && sIsSelectedProp && sListBoxItemType) {
            // Strategy 3: IsSelected tree walk
            focused = FindSelectedTabInTree(root, GlobalFocusMonitor::kMaxTreeDepth);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] GetFocusedElementInfo: SEH fault in focus detection");
        focused = nullptr;
    }

    if (!focused) {
        lua_pushnil(L);
        return 1;
    }

    // ExtractElementInfo uses std::string internally (inner/outer SEH
    // inside ExtractElementInfo itself handles faults there).
    auto focusedElem = static_cast<Noesis::FrameworkElement*>(focused);
    ExtractElementInfo(L, focusedElem);
    return 1;
}

// ---------------------------------------------------------------------------
// ReadWidgetTextBlocks: on-demand BFS text reader for any named widget.
// Finds the widget by name, BFS's its visual tree for TextBlocks, reads
// their text, and returns a Lua array of strings.
// Architecture: inner function uses std::string (has destructor), SEH
// wrapper returns a POD pointer.  Same pattern as FindHUDWidgets.
// ---------------------------------------------------------------------------

// Inner: find a widget by x:Name.  Uses std::string from ReadPropertyAsString
// so it CANNOT live inside __try.
static Noesis::Visual* FindWidgetByName_Inner(const char* widgetName)
{
    auto root = GetRoot();
    if (!root) return nullptr;

    InitFocusProperties(root);
    auto container = FindWidgetContainer(root);
    if (!container) return nullptr;

    auto widgetCount = SafeGetVisualChildrenCount_SEH(container);
    for (int widgetIndex = (int)widgetCount - 1; widgetIndex >= 0; widgetIndex--) {
        auto widget = SafeGetVisualChild_SEH(container, widgetIndex);
        if (!widget) continue;
        if (!SafeIsUIWidgetType_SEH(widget) || !IsVisibleDP(widget))
            continue;
        auto name = ReadPropertyAsString(
            static_cast<Noesis::FrameworkElement*>(widget), "Name");
        if (name == widgetName)
            return widget;
    }
    return nullptr;
}

// SEH wrapper: returns POD pointer (no destructors).
static Noesis::Visual* FindWidgetByName_SEH(const char* widgetName)
{
    __try {
        return FindWidgetByName_Inner(widgetName);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] FindWidgetByName_SEH: fault for '%s'",
                 widgetName ? widgetName : "(null)");
        return nullptr;
    }
}

// ReadWidgetTexts_Inner: BFS a visual subtree collecting TextBlock texts.
// Uses the shared BFS_VisitTextBlocks core.  Reads text AS each node is
// found (not after collecting pointers) to avoid the stale-pointer problem
// where reading one TextBlock's bindings destabilizes siblings.
// Uses std::string (destructor) so it CANNOT live inside __try.
static void ReadWidgetTexts_Inner(
    Noesis::Visual* root, std::vector<std::string>& outTexts)
{
    BoundedCollectContext context{outTexts, 64};
    BFS_VisitTextBlocks(root, 0, 512, false,
                        BoundedCollectVisitor, &context);
}

// SEH wrapper for ReadWidgetTexts_Inner.  The outTexts reference is a
// pointer under the hood (no destructor in this frame), so __try is safe.
// If the inner function faults, outTexts retains partial results.
static void ReadWidgetTexts_SEH(
    Noesis::Visual* root, std::vector<std::string>& outTexts)
{
    __try {
        ReadWidgetTexts_Inner(root, outTexts);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] ReadWidgetTexts_SEH: fault after %d texts",
                 (int)outTexts.size());
    }
}

// ---------------------------------------------------------------------------
// ReadFocusedTextBlocks: reads TextBlock texts from the currently focused
// element's visual subtree.  For inspect panel d-pad navigation where
// each side panel is a focusable ContentPresenter with TextBlock children.
// Returns a Lua array of strings.
// ---------------------------------------------------------------------------
UserReturn ReadFocusedTextBlocks(lua_State* L)
{
    auto focused = GetFocusedElement();
    if (!focused) {
        lua_createtable(L, 0, 0);
        return 1;
    }

    std::vector<std::string> texts;
    ReadWidgetTexts_SEH(static_cast<Noesis::Visual*>(focused), texts);

    lua_createtable(L, (int)texts.size(), 0);
    for (int i = 0; i < (int)texts.size(); i++) {
        lua_pushstring(L, texts[i].c_str());
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// PushStructuredTextBlocksToLua_Inner: shared C++ helper behind the
// Lua-facing ReadElementStructuredTextBlocks primitive.
//
// Takes a Visual* root, BFS's its subtree for TextBlocks (SEH-safe),
// runs CollectTooltipEntries_SEH for role/parentRole/fontSize/typeId
// extraction, and pushes a Lua array of {role, parentRole, text,
// fontSize, typeId} tables.  Matches the snapshot.tooltipTexts shape
// that SpeechData.FromTooltip already consumes.
//
// Null root -> empty array (harmless).  Always returns 1 (one table
// pushed to the Lua stack).
//
// Uses std::vector (destructor) -- can't live in __try directly
// (MSVC C2712).  Every Noesis-touching operation goes through an
// SEH-guarded helper:
//   - BFS_CollectByType_SEH    (__try-wrapped)
//   - CollectTooltipEntries_SEH (three-layer inner/outer)
// Lua stack ops (lua_createtable, lua_pushstring, etc.) are pure Lua
// C API, not Noesis.
//
// Callers should invoke PushStructuredTextBlocksToLua_SEH (the outer
// wrapper below) for defense-in-depth against faults that escape
// individual helpers' coverage.
// ---------------------------------------------------------------------------
static int PushStructuredTextBlocksToLua_Inner(
    lua_State* L, Noesis::Visual* root)
{
    if (!root) {
        lua_createtable(L, 0, 0);
        return 1;
    }

    Noesis::FrameworkElement* textBlocks[64];
    auto textBlockCount = BFS_CollectByType_SEH(
        root, "TextBlock", textBlocks, 0, 64,
        /*checkVisibility*/ true,
        /*skipMatchedChildren*/ true);

    std::vector<ecl::lua::TickSnapshot::TooltipEntry> entries;
    CollectTooltipEntries_SEH(textBlocks, textBlockCount, entries);

    lua_createtable(L, (int)entries.size(), 0);
    for (int i = 0; i < (int)entries.size(); i++) {
        lua_createtable(L, 0, 5);

        lua_pushstring(L, entries[i].role.c_str());
        lua_setfield(L, -2, "role");

        lua_pushstring(L, entries[i].parentRole.c_str());
        lua_setfield(L, -2, "parentRole");

        lua_pushstring(L, entries[i].text.c_str());
        lua_setfield(L, -2, "text");

        lua_pushnumber(L, (double)entries[i].fontSize);
        lua_setfield(L, -2, "fontSize");

        if (!entries[i].typeId.empty()) {
            lua_pushstring(L, entries[i].typeId.c_str());
            lua_setfield(L, -2, "typeId");
        }

        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// PushStructuredTextBlocksToLua_SEH: outer __try wrapper for
// PushStructuredTextBlocksToLua_Inner.  No local C++ objects with
// destructors (lua_State*, Visual*, int only), so __try is legal per
// MSVC C2712.
//
// On fault:
//   - Restore the Lua stack to its pre-call top so no partial table
//     entries pollute caller state.
//   - Push a valid empty table so callers always see a table return
//     (maintains the Lua contract).
//   - Log the fault.
// ---------------------------------------------------------------------------
static int PushStructuredTextBlocksToLua_SEH(
    lua_State* L, Noesis::Visual* root)
{
    int initialStackTop = lua_gettop(L);
    __try {
        return PushStructuredTextBlocksToLua_Inner(L, root);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_settop(L, initialStackTop);
        lua_createtable(L, 0, 0);
        BG3A_LOG(
            "[BG3Access] PushStructuredTextBlocksToLua_SEH: fault");
        return 1;
    }
}

// Lua API: read structured TextBlocks from an arbitrary element's
// subtree.  The only primitive for structured reading -- Lua composes
// with GetFocusedElement, FindNameInWidget, FindNameInWidgetScoped,
// or any other element accessor to target any node in the tree
// without needing more C++ bindings.
//
// Mixed lua_State* + typed signature: LuaWrapFunction auto-converts
// the element userdata from Lua stack arg 1 into the typed Visual*;
// we use L to push the structured result table.
UserReturn ReadElementStructuredTextBlocks(
    lua_State* L, Noesis::FrameworkElement* elem)
{
    return PushStructuredTextBlocksToLua_SEH(L,
        static_cast<Noesis::Visual*>(elem));
}

// ---------------------------------------------------------------------------
// GetTooltipPopupRoot: returns the Noesis popup root element for the
// currently open tooltip, or nullptr when no tooltip is visible.
//
// Lua composes this with FindNameInWidgetScoped + ReadElementStructuredTextBlocks
// to read structured tooltip content from specific template regions
// (e.g. HoveredItemPanel vs EquippedItemPanel in inventory compare mode).
// Keeps XAML-template-specific names out of C++ -- C++ exposes the popup
// entry point, Lua composes the template-aware logic on top.
//
// Event-gated by sToolTipIsOpen (set by ToolTip.Opened/Closed routed
// event handlers).  When no tooltip is open, returns nullptr immediately
// with no tree walking.
//
// When a tooltip is open, traverses the popup roots (non-content children
// of the true visual root) and returns the first one that:
//   - does NOT contain a ContextMenuItem (context menus are also popups)
//   - DOES contain at least one TextBlock (= a real tooltip with content)
//
// SEH-protected: any fault during traversal returns nullptr; Lua caller
// treats it as "no compare data available this tick" and skips cleanly.
// ---------------------------------------------------------------------------
Noesis::FrameworkElement* GetTooltipPopupRoot()
{
    if (!sToolTipIsOpen) return nullptr;

    __try {
        auto& monitor = GlobalFocusMonitor::Instance();
        auto trueRoot = monitor.GetTrueRoot();
        auto contentChild = monitor.GetContentChild();
        if (!trueRoot) return nullptr;

        Noesis::Visual* popupRoots[8];
        auto popupCount = GetPopupRoots_SEH(
            trueRoot, contentChild, popupRoots, 8);
        if (popupCount == 0) return nullptr;

        // Same filter as FindTooltipTextBlocks_SEH: skip context-menu
        // popups, return the first popup root that actually contains
        // TextBlocks (i.e. a tooltip).
        for (uint32_t i = 0; i < popupCount; i++) {
            auto popupRoot = popupRoots[i];
            if (!popupRoot) continue;

            Noesis::FrameworkElement* cmCheck[1];
            if (BFS_CollectByType_SEH(popupRoot, "ContextMenuItem",
                    cmCheck, 0, 1) > 0) continue;

            Noesis::FrameworkElement* tbCheck[1];
            if (BFS_CollectByType_SEH(popupRoot, "TextBlock",
                    tbCheck, 0, 1, true, true) > 0) {
                return static_cast<Noesis::FrameworkElement*>(popupRoot);
            }
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] GetTooltipPopupRoot: SEH fault");
        return nullptr;
    }
}

UserReturn ReadWidgetTextBlocks(lua_State* L)
{
    auto widgetName = luaL_checkstring(L, 1);

    auto widgetVisual = FindWidgetByName_SEH(widgetName);
    if (!widgetVisual) {
        lua_createtable(L, 0, 0);
        return 1;
    }

    // BFS + text reading in one pass (SEH-guarded).  Reads text as each
    // TextBlock is found to avoid stale pointers from binding cascades.
    std::vector<std::string> texts;
    ReadWidgetTexts_SEH(widgetVisual, texts);

    lua_createtable(L, (int)texts.size(), 0);
    for (int i = 0; i < (int)texts.size(); i++) {
        lua_pushstring(L, texts[i].c_str());
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// ReadHUDInfo: on-demand HUD text reader for accessibility.
//
// Walks visible widgets, finds PartyLine_c, TargetInfo_c, and CursorText_c
// by their Name property, then enters each widget's NameScope to read
// specific named TextBlocks.
//
// Returns a Lua table:
//   { characterName="Tav", characterInfo="Lv 1 High Elf Rogue",
//     targetName="Mind Flayer Pod", actionText="Use" }
//
// All fields are strings (empty string if not found / not visible).
// SEH-guarded throughout -- never crashes, just returns empty fields.
// ---------------------------------------------------------------------------

// POD struct for HUD widget pointers.  No destructors -- safe inside __try.
struct HUDWidgetPointers {
    Noesis::Visual* partyLine;
    Noesis::Visual* targetInfo;
    Noesis::Visual* cursorText;
};

// Inner function: single-pass widget lookup for all HUD widgets.
// Uses std::string (C++ destructor) so it CANNOT live inside __try.
static HUDWidgetPointers FindHUDWidgets_Inner()
{
    HUDWidgetPointers result = {nullptr, nullptr, nullptr};

    auto root = GetRoot();
    if (!root) return result;

    InitFocusProperties(root);
    auto container = FindWidgetContainer(root);
    if (!container) return result;

    auto widgetCount = SafeGetVisualChildrenCount_SEH(container);
    int foundCount = 0;

    for (int widgetIndex = (int)widgetCount - 1;
         widgetIndex >= 0 && foundCount < 3; widgetIndex--) {
        auto widget = SafeGetVisualChild_SEH(container, widgetIndex);
        if (!widget) continue;
        if (!SafeIsUIWidgetType_SEH(widget) || !IsVisibleDP(widget)) continue;

        auto name = ReadPropertyAsString(
            static_cast<Noesis::FrameworkElement*>(widget), "Name");
        if (name == "PartyLine_c")  { result.partyLine  = widget; foundCount++; }
        else if (name == "TargetInfo_c") { result.targetInfo = widget; foundCount++; }
        else if (name == "CursorText_c") { result.cursorText = widget; foundCount++; }
    }
    return result;
}

// SEH wrapper: HUDWidgetPointers is POD (no destructors).
static HUDWidgetPointers FindHUDWidgets_SEH()
{
    __try {
        return FindHUDWidgets_Inner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] FindHUDWidgets_SEH: fault");
        HUDWidgetPointers empty = {nullptr, nullptr, nullptr};
        return empty;
    }
}

// Inner function: find a named element in a widget and read its text.
// Writes into caller-owned char buffer (no C++ objects cross into SEH).
static void ReadNamedTextInWidget_Inner(Noesis::Visual* widget,
                                         const char* elementName,
                                         char* outBuf, size_t outBufSize)
{
    outBuf[0] = '\0';
    auto found = FindNameInWidgetScoped_Unsafe(elementName, widget);
    if (!found) return;
    if (!ProbeUIElement(static_cast<Noesis::UIElement*>(found))) return;

    auto typeName = found->GetClassType()->GetName();
    if (!typeName) return;

    auto text = ReadTextBlockText(found, false);

    if (text.find("[ForceUpdate]") != std::string::npos) return;
    if (text.find("s_HandleUnknown") != std::string::npos) return;

    if (!text.empty()) {
        size_t copyLen = (text.size() < outBufSize - 1)
            ? text.size() : (outBufSize - 1);
        memcpy(outBuf, text.data(), copyLen);
        outBuf[copyLen] = '\0';
    }
}

// SEH wrapper: no C++ objects with destructors.
static void ReadNamedTextInWidget_SEH(Noesis::Visual* widget,
                                       const char* elementName,
                                       char* outBuf, size_t outBufSize)
{
    outBuf[0] = '\0';
    __try {
        ReadNamedTextInWidget_Inner(widget, elementName, outBuf, outBufSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BG3A_LOG("[BG3Access] ReadNamedTextInWidget_SEH: fault for '%s'",
             elementName ? elementName : "(null)");
        outBuf[0] = '\0';
    }
}

// Helper: read a named TextBlock from a widget and push to Lua stack.
// Pushes empty string if widget is null or text not found.
static void PushNamedWidgetText(lua_State* L, Noesis::Visual* widget,
                                 const char* elementName, const char* luaField,
                                 char* textBuf, size_t textBufSize)
{
    if (widget) {
        ReadNamedTextInWidget_SEH(widget, elementName, textBuf, textBufSize);
        lua_pushstring(L, textBuf);
    } else {
        lua_pushstring(L, "");
    }
    lua_setfield(L, -2, luaField);
}

UserReturn ReadHUDInfo(lua_State* L)
{
    static const size_t kTextBufSize = 512;
    char textBuf[kTextBufSize];

    lua_createtable(L, 0, 4);

    // Single-pass widget lookup.
    auto widgets = FindHUDWidgets_SEH();

    PushNamedWidgetText(L, widgets.partyLine,  "ExtraInfoName",
                        "characterName", textBuf, kTextBufSize);
    PushNamedWidgetText(L, widgets.partyLine,  "ExtraInfoMisc",
                        "characterInfo", textBuf, kTextBufSize);
    PushNamedWidgetText(L, widgets.targetInfo,  "Name",
                        "targetName",    textBuf, kTextBufSize);
    PushNamedWidgetText(L, widgets.cursorText, "TaskDescription",
                        "actionText",    textBuf, kTextBufSize);
    return 1;
}

// ---------------------------------------------------------------------------

void NoesisErrorHandler(const char* file, uint32_t line, const char* message, bool fatal)
{
    ERR("[Noesis] %s", message);
}

void EnableErrorReporting(bool enable)
{
    auto handler = (Noesis::ErrorHandler*)GetStaticSymbols().Noesis__gErrorHandler;
    if (enable) {
        *handler = &NoesisErrorHandler;
    } else {
        *handler = nullptr;
    }
}

void RegisterUILib()
{
    DECLARE_MODULE(UI, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(GetRoot)
    MODULE_FUNCTION(GetStateMachine)
    MODULE_FUNCTION(SetState)
    MODULE_FUNCTION(RegisterType)
    MODULE_FUNCTION(Instantiate)
    MODULE_FUNCTION(GetPickingHelper)
    MODULE_FUNCTION(GetCursorControl)
    MODULE_FUNCTION(GetDragDrop)
    MODULE_FUNCTION(EnableErrorReporting)
    // Accessibility
    MODULE_FUNCTION(SubscribeGlobalFocusChanged)
    MODULE_FUNCTION(UnsubscribeGlobalFocusChanged)
    MODULE_FUNCTION(ForceGlobalFocusUpdate)
    MODULE_FUNCTION(SuppressGlobalFocusTick)
    MODULE_FUNCTION(SetDialoguePollActive)
    MODULE_FUNCTION(SetTraceLogging)
    MODULE_FUNCTION(HasProperty)
    MODULE_FUNCTION(HasLocalValue)
    MODULE_FUNCTION(IsElementVisible)
    MODULE_FUNCTION(IsConnectedToWidget)
    MODULE_FUNCTION(IsHitTestVisible)
    MODULE_FUNCTION(IsMoveFocusFocusable)
    MODULE_FUNCTION(GetDataContext)
    MODULE_FUNCTION(GetFocusedElement)
    MODULE_FUNCTION(GetTopmostWidget)
    MODULE_FUNCTION(FindNameInWidget)
    MODULE_FUNCTION(FindNameInWidgetScoped)
    MODULE_FUNCTION(QueryNamedElement)
    // Phase 1 refactor: C++ data extraction test API
    MODULE_FUNCTION(GetFocusedElementInfo)
    // Widget/element text readers (on-demand BFS for TextBlocks)
    MODULE_FUNCTION(ReadWidgetTextBlocks)
    MODULE_FUNCTION(ReadFocusedTextBlocks)
    MODULE_FUNCTION(ReadElementStructuredTextBlocks)
    // Tooltip popup root access (for on-demand Lua-side template-scoped reads
    // like inventory compare mode HoveredItemPanel / EquippedItemPanel).
    MODULE_FUNCTION(GetTooltipPopupRoot)
    // HUD info reader (on-demand, called from RS direction handler)
    MODULE_FUNCTION(ReadHUDInfo)
    END_MODULE()
}

END_NS()
