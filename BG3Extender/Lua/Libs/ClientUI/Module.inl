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
#include <GameDefinitions/DragDrop.h>
#include <GameDefinitions/Picking.h>

BEGIN_NS(lua)

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

#define FOR_NOESIS_TYPE(T) void LuaPolymorphic<T>::MakeRef(lua_State* L, T* value, LifetimeHandle lifetime) { \
    NoesisPush(L, value, lifetime); \
}

FOR_EACH_NOESIS_TYPE()
#undef FOR_NOESIS_TYPE

void LuaPolymorphic<Noesis::RoutedEventArgs>::MakeRef(lua_State* L, Noesis::RoutedEventArgs* value, LifetimeHandle lifetime)
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
// SelectionChanged event handler outputs.  Read and cleared in Tick().
// Single-threaded: Noesis events fire on the main thread, stable during Tick.
static bool sSelectionDirtyFlag = false;
// Address of the newly selected element from the last SelectionChanged event.
// Stored as uintptr_t -- NEVER cast back to a pointer.  Used only to check
// whether the selected item is a ListBoxItem by finding it fresh in the tree.
static uintptr_t sSelectionChangedItemAddr = 0;
void InitFocusProperties(Noesis::FrameworkElement* root);
Noesis::UIElement* TryFocusManager(Noesis::Visual* elem, int depth,
                                    Noesis::DependencyObject** outScopeRoot = nullptr);
Noesis::UIElement* FindFocusedInTree(Noesis::Visual* elem, int depth);
Noesis::UIElement* FindSelectedTabInTree(Noesis::Visual* elem, int depth);
static bool IsVisibleDP(Noesis::Visual const* elem);
static bool IsUIWidgetType(Noesis::Visual const* elem);
static Noesis::Visual* FindWidgetContainer(Noesis::Visual* root);

// Forward declarations -- defined below, after InitFocusProperties.
Noesis::UIElement* GetFocusedElement();
static void TryDiscoverFocusedElementProp(Noesis::Visual* const* widgets, uint32_t count);
Noesis::FrameworkElement* FindNameInWidgetScoped(char const* name, Noesis::Visual* widget);
static void PollRadialLocalFocus(
    Noesis::Visual* const* widgets, bool const* widgetVisible,
    uint32_t widgetCount, ecl::lua::TickSnapshot* snapshot);

// SEH-guarded Noesis read helpers.  Each function does ONLY raw pointer
// reads (no C++ objects with destructors) inside __try/__except.
// Text extraction (std::string) happens in the caller, outside SEH.
static uint32_t GatherWidgets_SEH(Noesis::Visual* container,
    Noesis::Visual** outWidgets, bool* outVisible, uint32_t maxWidgets);
static uintptr_t ReadDCAddress_SEH(Noesis::UIElement* elem);
static Noesis::UIElement* FindFocusedElement_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount,
    Noesis::FrameworkElement* root, int maxDepth,
    Noesis::DependencyObject** outScopeRoot);
static Noesis::UIElement* FindSelectedTab_SEH(
    Noesis::Visual** widgets, bool* widgetVisible, uint32_t widgetCount,
    Noesis::FrameworkElement* root,
    Noesis::DependencyObject* scopeRoot, int maxDepth);

// Forward declarations for data extraction (defined after ExtractElementInfo).
static void ExtractElementData(FocusEventData& out, Noesis::FrameworkElement* elem);
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

// Handler for Selector.SelectionChanged routed events.  Uses the
// DummyDelegate pattern (same as UIEventHooks): 'this' is a fake pointer
// that is never dereferenced.  Extracts the selected element directly
// from the event args -- zero tree walking needed.
struct SelectionDirtyDelegate
{
    void Handler(Noesis::BaseComponent* source, const Noesis::RoutedEventArgs& args)
    {
        __try {
            // Only fire for tab carousel selections, not inline carousels.
            auto& selArgs = static_cast<const Noesis::SelectionChangedEventArgs&>(args);
            if (selArgs.addedItems.Size() > 0) {
                auto addedItem = selArgs.addedItems[0].GetPtr();
                if (addedItem && sListBoxItemType) {
                    bool isListBoxItem = false;
                    auto itemClass = addedItem->GetClassType();
                    while (itemClass) {
                        if (itemClass == sListBoxItemType) {
                            isListBoxItem = true;
                            break;
                        }
                        itemClass = itemClass->GetBase();
                    }
                    if (!isListBoxItem) return;
                }
                sSelectionChangedItemAddr = reinterpret_cast<uintptr_t>(addedItem);
            } else {
                return;
            }
            sSelectionDirtyFlag = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            WARN("[BG3Access] SelectionChanged handler: SEH fault (stale widget during destruction?)");
        }
    }
};

// Fixed fake 'this' pointer for SelectionDirtyDelegate subscription.
// Non-null, unique, consistent between Subscribe and Remove calls.
static SelectionDirtyDelegate* const kSelectionDirtyDelegatePtr =
    reinterpret_cast<SelectionDirtyDelegate*>(static_cast<uintptr_t>(0xACC5E1));

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
    if (!selectedItemProp || !selectedItemProp->Property) return nullptr;

    auto selectedItemRaw = SafeTypePropertyGet_SEH(
        selectedItemProp->Property, listBox);
    if (!selectedItemRaw) return nullptr;

    return *reinterpret_cast<Noesis::BaseObject* const*>(selectedItemRaw);
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
        lastRadialTagAddr_ = 0;
        radialWasVisible_ = false;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgetAddrs_[i] = 0;
            prevWidgetVisible_[i] = false;
            settleBaselineWidgetAddrs_[i] = 0;
            settleBaselineWidgetVisible_[i] = false;
        }
        settleBaselineWidgetCount_ = 0;
        return true;
    }

    void Unsubscribe()
    {
        UnsubscribeAllSelectionChanged();
        callback_ = {};
        lastFocusedAddr_ = 0;
        lastSelectedAddr_ = 0;
        lastSelectedDCAddr_ = 0;
        widgetInpcDCAddr_ = 0;
        widgetInpcWidgetAddr_ = 0;
    }

    bool IsActive() const { return callback_.operator bool(); }

    void ForceNextFire() { forceNext_ = true; }

    void Tick()
    {
        if (!callback_) return;

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
        }
        if (!widgetContainer_) {
            widgetContainer_ = FindWidgetContainer(root);
        }

        // ----- Gather current widget set (SEH-guarded) -----
        Noesis::Visual* widgets[kMaxWidgets] = {};
        bool widgetVisible[kMaxWidgets] = {};
        uint32_t widgetCount = GatherWidgets_SEH(
            widgetContainer_, widgets, widgetVisible, kMaxWidgets);

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
        }

        if (somethingChanged) {
            if (!settling_) {
                WARN("[BG3Access] Settle: started (waiting for stability)");
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
                WARN("[BG3Access] Settle: complete after %u frames (%u stable)",
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

        // ----- Lazy FocusedElement property discovery -----
        if (!sFocusedElementProp && widgetCount > 0) {
            TryDiscoverFocusedElementProp(widgets, widgetCount);
        }

        // ----- Strategy 1+2: Focus detection (SEH-guarded) -----
        // Strategy 1: FocusManager.FocusedElement (fast path).
        // Strategy 2: IsFocused + ls:MoveFocus.IsFocused tree walk (fallback).
        // Both wrapped in FindFocusedElement_SEH -- returns nullptr on fault.
        Noesis::DependencyObject* scopeRoot = nullptr;
        Noesis::UIElement* focused = FindFocusedElement_SEH(
            widgets, widgetVisible, widgetCount,
            root, kMaxTreeDepth, &scopeRoot);

        // Subscribe SelectionChanged on each widget (subscribe-only, never
        // Remove).  Per-widget because the event may be handled (stopped)
        // before reaching the application root.
        // Only subscribe when UI is stable (has had focus) -- during loading
        // transitions, widgets appear/disappear every tick and subscribing
        // on transient widgets crashes.
        if (sSelectionChangedEvent && hadFocusBefore_) {
            SubscribeSelectionChangedOnWidgets(widgets, widgetCount);
        }

        // ----- Strategy 3: IsSelected (event-gated when available) -----
        // sSelectionDirtyFlag is already consumed at the top of Tick()
        // (it arms the settle window).  By the time we reach here, any
        // selection event has been through settle and the tree is stable.
        // The only ways Strategy 3 runs on this tick are: forceNext_
        // (post-settle), !initialSelectionDone_ (first entry), or no
        // event subscription (polling fallback).

        // Only reset initialSelectionDone_ when widgets are ADDED (new
        // menu entry), not when they're swapped (tab content change).
        if (widgetSetJustChanged && widgetCount > prevWidgetCount_) {
            initialSelectionDone_ = false;
        }

        // Post-settle tick ALWAYS runs Strategy 3 because SelectionChanged
        // subscriptions may not exist during settle (they're created after).
        // selectionFiredDuringSettle_ handles the common case (RB/LB tab
        // switch mid-menu).  postSettle_ handles initial menu entry where
        // subscriptions weren't active during the settle window.
        bool shouldRunStrategy3 = !sSelectionChangedEvent
                               || subscribedWidgetCount_ == 0
                               || !initialSelectionDone_
                               || selectionFiredDuringSettle_
                               || postSettle_;
        selectionFiredDuringSettle_ = false;

        Noesis::UIElement* selected = nullptr;
        if (sIsSelectedProp && sListBoxItemType && shouldRunStrategy3) {
            strategy3Runs_++;
            initialSelectionDone_ = true;
            sSelectionChangedItemAddr = 0;

            // Tree walk (SEH-guarded): find the currently selected
            // ListBoxItem fresh.  The SelectionChanged event told us
            // SOMETHING changed; the walk finds exactly WHAT is now.
            selected = FindSelectedTab_SEH(
                widgets, widgetVisible, widgetCount,
                root, scopeRoot, kMaxTreeDepth);

            if (reinterpret_cast<uintptr_t>(selected) != lastSelectedAddr_) strategy3Changes_++;
        } else if (sIsSelectedProp && sListBoxItemType) {
            // Strategy 3 skipped -- preserve last known address for
            // comparison only.  selected stays null (not safe to dereference).
            // selectedAddr below handles the comparison.
            sSelectionChangedItemAddr = 0;  // consume stale event data
        }

        // Track whether selected was found fresh this tick (safe to
        // dereference) vs not found (Strategy 3 was skipped).
        // selected is always fresh when non-null because it was obtained
        // from this frame's focus strategies or event args.
        bool selectedIsFresh = (selected != nullptr);
        auto selectedAddr = reinterpret_cast<uintptr_t>(selected);

        // When Strategy 3 was skipped, use the stored address for
        // comparison only (selected pointer is null, cannot dereference).
        uintptr_t effectiveSelectedAddr = selectedIsFresh ? selectedAddr : lastSelectedAddr_;

        // ----- DataContext for recycling detection (SEH-guarded) -----
        uintptr_t selectedDCAddr = 0;
        if (selected && selectedIsFresh) {
            selectedDCAddr = ReadDCAddress_SEH(selected);
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
        // Focus fires when element changes OR when the same element gets
        // a new DataContext (DC swap -- container recycling in content areas).
        bool focusChanged = (reinterpret_cast<uintptr_t>(focused) != lastFocusedAddr_);
        if (!focusChanged && focused
            && focusedDCAddr != lastFocusedDCAddr_
            && focusedDCAddr != 0 && lastFocusedDCAddr_ != 0) {
            // Same element, DC swapped to a different non-null value
            focusChanged = true;
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
        if (effectiveSelectedAddr != lastSelectedAddr_) {
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
            WARN("[BG3Access] Tick: focused=%p scopeRoot=%p selected=%p selDC=0x%llx prevSelDC=0x%llx focDC=0x%llx prevFocDC=0x%llx focChg=%d selChg=%d forced=%d wChg=%d widgets=%u",
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
            WARN("[BG3Access]   -> FIRE selection (tab)");

            SubscribeElementINPC(selected);
        } else if (focusChanged && focused) {
            WARN("[BG3Access]   -> FIRE focus");

            SubscribeElementINPC(focused);
        } else if (forced) {
            // For forced re-fire, only use pointers that are fresh.
            auto best = (selected && selectedIsFresh) ? selected : focused;
            if (best) {
                WARN("[BG3Access]   -> FIRE forced");

                SubscribeElementINPC(best);
                forcedNullCount_ = 0;
            } else {
                // Both null -- UI may be rebuilding (Cross-Play tab)
                // or focus moved to a different layer (pause menu).
                // Retry a few times for rebuilds, then stop to avoid
                // spamming when focus is genuinely elsewhere.
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
        if (widgetSetChanged && widgetCount > 0 && hadFocusBefore_) {
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
                        WARN("[BG3Access]   -> FIRE widgetAdded (widget[%d])", i);
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
                        WARN("[BG3Access]   -> FIRE widgetAdded fallback (widget[%d])", i);
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
                WARN("[BG3Access]   -> Post-settle INPC subscribe (widget[%u] DC=%s)", i, dcTypeName);
                SubscribeWidgetINPC(dataContext, widgetElem);
                break;
            }

        }
        bool wasPostSettle = postSettle_;
        postSettle_ = false;

        // ----- Deferred namedTexts re-collection on tab switch -----
        // After a tab switch, wait for Noesis to update Visibility states
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
        if (!focused && !selected && widgetCount > 0) {
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
                WARN("[BG3Access] Initial widget scan (%u widgets)", widgetCount);
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
                    WARN("[BG3Access]   widget[%d] vis=%d DC=%s%s", i,
                        isVisible ? 1 : 0, scanDCType,
                        isOverlay ? " (overlay)" : "");

                    if (!isVisible || isOverlay) continue;

                    ExtractWidgetData(widgetElem, *snapshot);
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
                            WARN("[BG3Access]     Visual text: %s", visualText.c_str());
                            std::string key = "_visualText_" + std::to_string(++visualIndex);
                            snapshot->focusedElement.namedTexts.push_back(
                                {std::move(key), std::move(visualText)});
                        }
                    }
                }
            }
        }

        // Keep polling when nothing is focused (give new widgets time
        // to settle and acquire focus).  Cap at 30 ticks to avoid
        // infinite loops when focus never arrives (e.g., CC cutscene
        // transition where selected stays non-null but focused stays null).
        if (!focused) {
            if (widgetSetChanged) overlayPollCount_ = 0;
            if (overlayPollCount_ < 30) {
                forceNext_ = true;
                overlayPollCount_++;
            }
        } else {
            overlayPollCount_ = 0;
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
            widgetDCDirty_ = false;

            // Re-discover the widget from the live widget list using its
            // stored address.  NEVER dereference stored addresses directly.
            // widgetContainer_ is a cross-tick pointer -- probe before use.
            Noesis::FrameworkElement* freshWidget = nullptr;
            if (widgetContainer_
                && ProbeVisualChildren(widgetContainer_)) {
                auto containerChildCount = widgetContainer_->GetVisualChildrenCount();
                for (uint32_t i = 0; i < containerChildCount; i++) {
                    auto child = widgetContainer_->GetVisualChild(i);
                    if (reinterpret_cast<uintptr_t>(child) == widgetInpcWidgetAddr_) {
                        freshWidget = static_cast<Noesis::FrameworkElement*>(child);
                        break;
                    }
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
                        snapshot->widgetData.eventType = "WidgetDCChanged";
                        snapshot->widgetData.dcType = freshDCTypeName;
                        CollectDCProperties(snapshot->widgetData, freshDC);
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
                    WARN("[BG3Access]   -> Carousel text changed: %s", freshText.c_str());
                }
            }
        } else {
            widgetDCDirty_ = false;
        }

        // ----- Inline carousel detection -----
        // Appearance rows have child ListBoxes (face, skin colour, etc.).
        // When left/right changes the SelectedItem, no focus event fires.
        // Each tick, find the child ListBox under the focused element, read
        // SelectedItem.Name or SelectedItem.ColorName, and include the
        // value in the snapshot if it changed.
        // Runs on EVERY tick with a focused element, including focus changes,
        // so the carousel value arrives in the SAME snapshot as the category.
        // Uses the THIS-FRAME focused pointer (not the stored address).
        //
        // Clear previous carousel text on focus change so the new element's
        // carousel value is always detected as a change.
        if (focusChanged) {
            lastInlineCarouselText_.clear();
        }
        if (focused) {
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

            while (searchHead < searchTail && searchHead < 64) {
                auto currentNode = searchQueue[searchHead++];
                if (!currentNode) continue;
                // Validate before virtual calls -- element may be stale.
                if (!ProbeUIElement(static_cast<Noesis::UIElement*>(currentNode))) continue;

                auto nodeTypeName = SafeBaseObjectTypeName_SEH(currentNode);

                // Check for TextBlock named "selectionName"
                if (nodeTypeName && strstr(nodeTypeName, "TextBlock")) {
                    auto nodeName = ReadPropertyAsString(
                        static_cast<Noesis::FrameworkElement*>(currentNode), "Name");
                    if (nodeName == "selectionName") {
                        foundSelectionName = static_cast<Noesis::FrameworkElement*>(currentNode);
                        break;
                    }
                }

                // Also track ListBox as fallback
                if (!foundListBox && nodeTypeName
                    && strstr(nodeTypeName, "ListBox")
                    && !strstr(nodeTypeName, "ListBoxItem")) {
                    foundListBox = static_cast<Noesis::FrameworkElement*>(currentNode);
                }

                // Add children to queue
                auto childCount = currentNode->GetVisualChildrenCount();
                for (uint32_t childIdx = 0; childIdx < childCount && searchTail < 64; childIdx++) {
                    searchQueue[searchTail++] = currentNode->GetVisualChild(childIdx);
                }
            }

            // Try reading text from the selectionName TextBlock first
            std::string carouselText;
            if (foundSelectionName) {
                carouselText = ReadTextBlockText(foundSelectionName);
            }
            // Fallback: read SelectedItem.Name from ListBox
            if (carouselText.empty() && foundListBox) {
                carouselText = TryReadInlineCarouselText(foundListBox);
            }

            if (!carouselText.empty()) {

                if (!carouselText.empty() && carouselText != lastInlineCarouselText_) {
                    lastInlineCarouselText_ = carouselText;
                    WARN("[BG3Access]   -> Inline carousel changed: %s", carouselText.c_str());
                    // No legacy dispatch -- snapshot builder reads
                    // lastInlineCarouselText_ at end of Tick().
                }
            } else {
                if (!lastInlineCarouselText_.empty()) {
                    lastInlineCarouselText_.clear();
                }
            }
        }

        // (Focus-change clear moved above carousel scan so the value
        // is detected as a change on the same tick as the focus change.)

        // ----- Radial LocalFocus polling (RT shortcuts menu) -----
        // Delegated to PollRadialLocalFocus (SEH-guarded, no C++ destructors).
        // Populates snapshot->radialSlotChanged and related fields.
        if (!focused && !selected && widgetCount > 0 && sDataContextProp && sTagProp) {
            PollRadialLocalFocus(widgets, widgetVisible, widgetCount, snapshot);
        }

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

                            WARN("[BG3Access] HOTBAR RADIAL: title=%s tag=%s",
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
        }

        // ===== SNAPSHOT FINALIZATION =====
        // The snapshot has been populated throughout Tick().
        // Finalize change flags, run delta comparison, and dispatch.
        {
            // Change flags from detection above
            snapshot->focusChanged = focusChanged;
            snapshot->selectionChanged = selectionChanged || snapshot->selectionChanged;

            // Focused element data: ONLY use elements obtained THIS frame.
            Noesis::UIElement* snapshotElement = focused;
            if (!snapshotElement && selected && selectedIsFresh) {
                snapshotElement = selected;
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

            // Inline carousel value (already detected above)
            snapshot->inlineCarouselValue = lastInlineCarouselText_;
            snapshot->inlineCarouselChanged =
                !lastInlineCarouselText_.empty() &&
                lastInlineCarouselText_ != previousSnapshotCarousel_;

            // Value changes are detected solely through INPC (inpcDirty_).
            // A redundant dcScalarProps diff was here previously but it
            // caused duplicate VALUE dispatches that interrupted speech.
            // INPC is the single source of truth for value changes.

            // Widget, widgetDC, and namedTexts data were written directly
            // into snapshot by ExtractWidgetData() and the widgetDCDirty
            // handler above.  No accumulator reads needed.

            // Commit pending INPC subscription immediately.
            // The old settleFramesRemaining_ mechanism delayed this for
            // 3 frames after tab changes, but also suppressed focus
            // dispatches (lobby focus on multiplayer entry).  The 6-frame
            // tree settle window already handles timing; no second settle.
            if (pendingINPCSubscription_) {
                CommitINPCSubscription(focused ? focused : selected);
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
                WARN("[BG3Access] SelectionChanged during tick -- re-arming settle, suppressing dispatch");
                sSelectionDirtyFlag = false;
                sSelectionChangedItemAddr = 0;
                settling_ = true;
                settleStableCount_ = 0;
                settleTotalCount_ = 0;
                return;
            }

            // Dispatch snapshot to Lua
            if (shouldDispatch && callback_) {
                // Count namedTexts in both focusedElement and widgetData.
                int focusNamedCount = static_cast<int>(
                    snapshot->focusedElement.namedTexts.size());
                int widgetNamedCount = 0;
                if (snapshot->widgetAdded)
                    widgetNamedCount = static_cast<int>(
                        snapshot->widgetData.namedTexts.size());

                WARN("[BG3Access] SNAPSHOT: focus=%d sel=%d val=%d carousel=%d "
                     "widget=%d postSettle=%d elemId=%s dcType=%s "
                     "focusNT=%d widgetNT=%d carVal=%s",
                     snapshot->focusChanged, snapshot->selectionChanged,
                     snapshot->valueChanged, snapshot->inlineCarouselChanged,
                     snapshot->widgetAdded, wasPostSettle ? 1 : 0,
                     snapshot->focusedElement.elemId.c_str(),
                     snapshot->focusedElement.dcType.empty()
                         ? "(none)" : snapshot->focusedElement.dcType.c_str(),
                     focusNamedCount, widgetNamedCount,
                     snapshot->inlineCarouselValue.c_str());

                // Log namedTexts keys+values so we can see what data arrived.
                for (auto const& pair : snapshot->focusedElement.namedTexts) {
                    WARN("[BG3Access]   focusNT: %s = %s",
                         pair.first.c_str(), pair.second.c_str());
                }
                if (snapshot->widgetAdded) {
                    for (auto const& pair : snapshot->widgetData.namedTexts) {
                        WARN("[BG3Access]   widgetNT: %s = %s",
                             pair.first.c_str(), pair.second.c_str());
                    }
                }

                // Update delta cache
                previousSnapshotElemId_ = snapshot->focusedElement.elemId;
                previousSnapshotCarousel_ = snapshot->inlineCarouselValue;
                previousSnapshotDCProps_ = snapshot->focusedElement.dcScalarProps;

                // Set INPC cooldown to suppress stray echoes for 2 ticks
                // after any focus/selection dispatch.
                if (snapshot->focusChanged || snapshot->selectionChanged) {
                    inpcCooldown_ = 2;
                }

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
        UnsubscribeAllSelectionChanged();
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
        // NOTE: scan tracking fields (initialWidgetScanDelay_,
        // lastScanWidgetCount_, lastScanFirstAddr_) intentionally NOT
        // reset here.  GameStateChanged fires multiple times during load
        // and resetting causes the same tip to repeat on each transition.
        initialSelectionDone_ = false;
        selectionFiredDuringSettle_ = false;
        sSelectionDirtyFlag = false;
        inpcDirty_ = false;
        widgetDCDirty_ = false;
        widgetInpcDCAddr_ = 0;
        widgetInpcWidgetAddr_ = 0;
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
        WARN("[BG3Access]   SubscribeElementINPC: deferred (pending re-discovery)");
    }

    // Re-discover the focused element and subscribe INPC on it.
    // Called from Tick() after settling, using fresh pointers from
    // the current frame's focus strategies.
    void CommitINPCSubscription(Noesis::UIElement* freshElement)
    {
        pendingINPCSubscription_ = false;
        if (!freshElement) return;

        WARN("[BG3Access]   CommitINPCSubscription: elem=%p", freshElement);
        auto frameworkElem = static_cast<Noesis::FrameworkElement*>(freshElement);

        // No UnsubscribeINPC needed -- fire-and-forget pattern.
        // Previous subscription's delegate just sets inpcDirty_ (harmless).
        auto dataContext = SafeReadDC_SEH(
            static_cast<Noesis::DependencyObject const*>(frameworkElem));
        if (dataContext) {
            SubscribeINPC(dataContext);
        }
    }

    // Fire a WIDGET event: extract data from the widget and push as a
    // data table (no Noesis elements cross to Lua).  Phase 3 replacement
    // for the old OnPropertyChanged path.
    void ExtractWidgetData(Noesis::UIElement* elem, ecl::lua::TickSnapshot& snapshot)
    {
        if (!elem) return;
        snapshot.widgetAdded = true;
        auto& data = snapshot.widgetData;
        data.eventType = "WidgetAdded";

        auto frameworkElem = static_cast<Noesis::FrameworkElement*>(elem);
        WARN("[BG3Access]   FireWidgetCallback: elem=%p", elem);
        auto classType = SafeGetClassType_SEH(frameworkElem);
        if (!classType) {
            WARN("[BG3Access]   FireWidgetCallback: GetClassType returned null for %p, skipping", elem);
            return;
        }
        data.elemType = classType->GetName();
        data.elemName = ReadPropertyAsString(frameworkElem, "Name");

        // Widget root ID (the widget itself IS the root for widget-added)
        char ptrBuf[32];
        snprintf(ptrBuf, sizeof(ptrBuf), "%p", static_cast<void*>(elem));
        data.widgetRootId = ptrBuf;

        // Collect ALL DC properties from the widget's DataContext.
        // This captures ViewModel-driven content: titles, descriptions,
        // dialog text (LSMessageBoxData), status messages, etc.
        {
            auto dataContext = SafeReadDC_SEH(
                static_cast<Noesis::DependencyObject const*>(frameworkElem));
            if (dataContext) {
                auto dcTypeName = SafeBaseObjectTypeName_SEH(dataContext);
                if (dcTypeName) {
                    data.dcType = dcTypeName;
                    CollectDCProperties(data, dataContext);
                    TryCollectSelectionFlyOutTitle(data, dataContext);

                    // NOTE: Actions collection enumeration via DynamicCast<IList*>
                    // crashes the Noesis Indie SDK type registry.  Dialog button
                    // hints are handled in Lua based on dcType instead.

                    // Subscribe widget DC INPC for property change tracking.
                    // Pass widget element for namedTexts collection on DC change.
                    SubscribeWidgetINPC(dataContext, frameworkElem);
                }
            }
        }

        // Extract binding metadata from the widget element's properties.
        ExtractBindingInfo(data, frameworkElem);

        // Collect named TextBlock texts from the widget via NameScope lookup.
        // Captures LocaString-bound titles/descriptions authored in XAML
        // that are not accessible through the ViewModel DataContext.
        // SEH guard at the call site as a last resort -- if an unexpected
        // element crashes ReadTextBlockText, we lose NameScope texts for
        // this widget but don't crash the game.
        TryCollectNamedTexts(frameworkElem, data.namedTexts);
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
        notifies->PropertyChanged().Add(
            Noesis::MakeDelegate(this, &GlobalFocusMonitor::OnINPCChanged));
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

        notifies->PropertyChanged().Add(
            Noesis::MakeDelegate(this, &GlobalFocusMonitor::OnWidgetINPCChanged));
    }

    // UnsubscribeWidgetINPC removed -- fire-and-forget pattern.
    // Noesis cleans up delegate lists when objects are destroyed.

    void OnWidgetINPCChanged(Noesis::BaseComponent* sender,
                              const Noesis::PropertyChangedEventArgs& args)
    {
        // Batch per frame: just set dirty flag.  The tick fires ONE
        // WidgetDCChanged event instead of one per property change.
        widgetDCDirty_ = true;
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
    // No stored pointers -- subscription is one-way.

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

    // Widget container tracking (Strategy 4).
    // All stored as uintptr_t -- comparison only, never dereferenced.
    // widgetContainer_ is the exception: it's re-discovered each tick
    // via FindWidgetContainer() when cachedRootAddr_ changes.
    uintptr_t cachedRootAddr_ = 0;
    Noesis::Visual* widgetContainer_ = nullptr;  // re-discovered from root each tick when root changes
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
    int inpcCooldown_ = 0;              // ticks since last focus/selection dispatch; suppresses stray INPC echoes
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

    // Widgets that already have a SelectionChanged handler.
    // Subscribe-only pattern (never Remove) -- track addresses to avoid
    // adding duplicate handlers.  If a widget is destroyed, its HashMap
    // is torn down (handler gone).  A recycled address just means we
    // skip subscribing (harmless -- the new widget at that address gets
    // subscribed on its next widget set change).
    uintptr_t subscribedWidgetAddrs_[kMaxWidgets] = {};
    uint32_t subscribedWidgetCount_ = 0;

    // ----- Tick Snapshot (one-per-frame state package) -----
    // Populated throughout Tick(), dispatched once at the end.
    ecl::lua::TickSnapshot tickSnapshot_;
    std::string previousSnapshotElemId_;    // Delta: last sent elemId
    std::string previousSnapshotCarousel_;  // Delta: last sent inline carousel value
    std::vector<std::pair<std::string, std::string>> previousSnapshotDCProps_; // Delta: last sent DC props

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

    // Subscribe SelectionChanged on visible widgets.  Skip already-subscribed.
    // Prunes dead entries first: any tracked widget NOT in the current set
    // has been destroyed (its handler was torn down with its HashMap).
    void SubscribeSelectionChangedOnWidgets(Noesis::Visual* const* widgets, uint32_t count)
    {
        if (!sSelectionChangedEvent) return;

        // Prune dead entries: remove tracked addresses not in current set.
        for (uint32_t i = 0; i < subscribedWidgetCount_; ) {
            bool alive = false;
            for (uint32_t j = 0; j < count; j++) {
                if (reinterpret_cast<uintptr_t>(widgets[j]) == subscribedWidgetAddrs_[i]) {
                    alive = true;
                    break;
                }
            }
            if (!alive) {
                // Swap with last and shrink.
                subscribedWidgetAddrs_[i] = subscribedWidgetAddrs_[subscribedWidgetCount_ - 1];
                subscribedWidgetAddrs_[subscribedWidgetCount_ - 1] = 0;
                subscribedWidgetCount_--;
            } else {
                i++;
            }
        }

        // Subscribe on new widgets (using fresh pointers from THIS tick).
        for (uint32_t i = 0; i < count; i++) {
            if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;

            auto uiElement = static_cast<Noesis::UIElement*>(
                const_cast<Noesis::Visual*>(widgets[i]));
            auto elementAddr = reinterpret_cast<uintptr_t>(uiElement);

            bool alreadySubscribed = false;
            for (uint32_t j = 0; j < subscribedWidgetCount_; j++) {
                if (subscribedWidgetAddrs_[j] == elementAddr) {
                    alreadySubscribed = true;
                    break;
                }
            }
            if (alreadySubscribed) continue;
            if (subscribedWidgetCount_ >= kMaxWidgets) continue;

            auto handlers = uiElement->mRoutedEventHandlers.Find(sSelectionChangedEvent);
            if (handlers == uiElement->mRoutedEventHandlers.End()) {
                uiElement->mRoutedEventHandlers.Insert(
                    sSelectionChangedEvent,
                    Noesis::RoutedEventHandler{
                        kSelectionDirtyDelegatePtr,
                        &SelectionDirtyDelegate::Handler});
            } else {
                handlers->value.Add(
                    Noesis::RoutedEventHandler{
                        kSelectionDirtyDelegatePtr,
                        &SelectionDirtyDelegate::Handler});
            }

            subscribedWidgetAddrs_[subscribedWidgetCount_] = elementAddr;
            subscribedWidgetCount_++;
            WARN("[BG3Access] Subscribed SelectionChanged on widget %p", uiElement);
        }
    }

    // Clear tracking array.  Handlers stay in the widgets' HashMaps
    // (subscribe-only).  Destroyed widgets clean up their own maps.
    void UnsubscribeAllSelectionChanged()
    {
        for (uint32_t i = 0; i < subscribedWidgetCount_; i++)
            subscribedWidgetAddrs_[i] = 0;
        subscribedWidgetCount_ = 0;
    }

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
                    WARN("[BG3Access] FocusedElement resolved on retry: %p", sFocusedElementProp);
                }
            }
        }
        if (!sUIWidgetType) {
            auto widgetType = Noesis::Reflection::GetType(Noesis::Symbol("ls.UIWidget"));
            if (widgetType) {
                sUIWidgetType = static_cast<Noesis::TypeClass const*>(widgetType);
                WARN("[BG3Access] UIWidgetType resolved on retry: %p", sUIWidgetType);
            }
        }
        if (!sDCWidgetType) {
            auto dcType = Noesis::Reflection::GetType(Noesis::Symbol("ls.DCWidget"));
            if (dcType) {
                sDCWidgetType = static_cast<Noesis::TypeClass const*>(dcType);
                WARN("[BG3Access] DCWidgetType resolved on retry: %p", sDCWidgetType);
            }
        }
        if (!sLSMoveFocusIsFocusedProp) {
            auto mfType = Noesis::Reflection::GetType(Noesis::Symbol("ls.MoveFocus"));
            if (mfType) {
                auto mfClass = static_cast<Noesis::TypeClass const*>(mfType);
                sLSMoveFocusIsFocusedProp = Noesis::TypeHelpers::GetDependencyProperty(
                    mfClass, bg3se::FixedString("IsFocused"));
                if (sLSMoveFocusIsFocusedProp) {
                    WARN("[BG3Access] ls:MoveFocus.IsFocused resolved on retry: %p",
                        sLSMoveFocusIsFocusedProp);
                }
            }
        }
        return;
    }
    sFocusPropsInitialized = true;
    Noesis::gStaticSymbols.Initialize();

    sIsFocusedProp = Noesis::TypeHelpers::GetDependencyProperty(
        root->GetClassType(), bg3se::FixedString("IsFocused"));

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
        root->GetClassType(), bg3se::FixedString("DataContext"));

    // IsVisible -- read-only computed DP.  Used internally for tree walk pruning.
    sIsVisibleProp = Noesis::TypeHelpers::GetDependencyProperty(
        root->GetClassType(), bg3se::FixedString("IsVisible"));
    // Visibility -- settable enum DP (Collapsed/Hidden/Visible).  Used by
    // IsElementVisible to walk ancestors via VisualTreeHelper::GetParent()
    // and detect Collapsed/Hidden parents (the IsVisible DP read does not
    // coerce through ancestors in the Indie SDK).
    sVisibilityProp = Noesis::TypeHelpers::GetDependencyProperty(
        root->GetClassType(), bg3se::FixedString("Visibility"));
    sIsHitTestVisibleProp = Noesis::TypeHelpers::GetDependencyProperty(
        root->GetClassType(), bg3se::FixedString("IsHitTestVisible"));

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
        root->GetClassType(), bg3se::FixedString("Tag"));

    // Discover Selector.SelectionChanged routed event at runtime.
    // Used for event-driven tab detection.  Subscribe-only pattern:
    // we Insert/Add to mRoutedEventHandlers but NEVER Remove().
    // Same approach as UIEventHooks::Subscribe (which also never removes
    // due to the "HACK - don't set reference" pattern).
    auto selectorReflType = Noesis::Reflection::GetType(Noesis::Symbol("Selector"));
    if (selectorReflType) {
        auto selectorMeta = static_cast<Noesis::TypeMeta const*>(selectorReflType);
        for (auto* metaEntry : selectorMeta->mMetaData) {
            if (!metaEntry) continue;
            auto metaTypeName = metaEntry->GetClassType()->GetName();
            if (strstr(metaTypeName, "UIElementData")) {
                auto elementData = static_cast<Noesis::UIElementData const*>(metaEntry);
                sSelectionChangedEvent = Noesis::UIElementDataHelpers::GetEvent(
                    elementData, Noesis::Symbol("SelectionChanged"));
                break;
            }
        }
    }
    WARN("[BG3Access] SelectionChanged event: selectorType=%p event=%p",
        selectorReflType, sSelectionChangedEvent);

    // Diagnostic: show what resolved.
    WARN("[BG3Access] InitFocusProperties: IsFocused=%p FocusedElement=%p IsSelected=%p DataContext=%p IsVisible=%p Visibility=%p ListBoxItemType=%p UIWidgetType=%p DCWidgetType=%p LSMoveFocusIsFocused=%p LSMoveFocusFocusable=%p NameScope=%p Tag=%p",
        sIsFocusedProp, sFocusedElementProp, sIsSelectedProp, sDataContextProp, sIsVisibleProp, sVisibilityProp, sListBoxItemType, sUIWidgetType, sDCWidgetType, sLSMoveFocusIsFocusedProp, sLSMoveFocusFocusableProp, sNameScopeProp, sTagProp);
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
            WARN("[BG3Access] FocusedElement discovered from widget mValues: %p", sFocusedElementProp);
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
        WARN("[BG3Access] AV in ReadWidgetDCTypeName_SEH -- stale DC pointer skipped");
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
    static const Noesis::DependencyProperty* sLocalFocusProp = nullptr;
    static bool sLocalFocusLookupDone = false;
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

            if (!sLocalFocusLookupDone) {
                sLocalFocusLookupDone = true;
                sLocalFocusProp = LookupLocalFocusDP(menuRadial->GetClassType());
                if (sLocalFocusProp) {
                    WARN("[BG3Access] LocalFocus DP found on %s",
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
        WARN("[BG3Access] PollRadialLocalFocus: access fault (SEH caught)");
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

    WARN("[BG3Access] RADIAL FOCUS: tag=%s title=%s",
         snapshot->radialSlotTag.c_str(),
         snapshot->radialTitleText.empty()
             ? "(none)" : snapshot->radialTitleText.c_str());
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
        // Validate before virtual calls -- element may be stale.
        if (!ProbeUIElement(static_cast<Noesis::UIElement*>(node))) continue;
        auto childCount = node->GetVisualChildrenCount();
        for (uint32_t i = 0; i < childCount; i++) {
            auto child = node->GetVisualChild(i);
            if (!child) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(child)))
                continue;
            if (IsUIWidgetType(child))
                return node;  // This node is the container
        }
        // No UIWidget children at this level -- enqueue children for
        // next level.  Limit total nodes to prevent runaway searches.
        for (uint32_t i = 0; i < childCount && queueTail < 60; i++) {
            auto child = node->GetVisualChild(i);
            if (!child) continue;
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(child)))
                continue;
            queue[queueTail++] = child;
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
        WARN("[BG3Access] TryFocusManager: stale element pointer %p -- skipping subtree", elem);
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
                    WARN("[BG3Access] TryFocusManager: stale FocusedElement pointer %p -- skipping", focused);
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
        WARN("[BG3Access] GetTopmostWidget: SEH fault");
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// SEH-guarded Noesis read helpers.
// Each function does ONLY raw pointer reads (no C++ objects with destructors)
// inside __try/__except.  Text extraction (std::string) happens in the
// caller, outside the SEH block.
// ---------------------------------------------------------------------------

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
        WARN("[BG3Access] GatherWidgets_SEH: fault on container %p", container);
        for (uint32_t i = 0; i < maxWidgets; i++) {
            outWidgets[i] = nullptr;
            outVisible[i] = false;
        }
        return 0;
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
// and Strategy 2 (IsFocused + ls:MoveFocus.IsFocused tree walk) with
// SEH protection.  Returns the focused element, or nullptr on failure/fault.
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

        // Strategy 2: IsFocused + ls:MoveFocus.IsFocused tree walk
        if (sIsFocusedProp || sLSMoveFocusIsFocusedProp) {
            if (widgetCount > 0) {
                for (int i = (int)widgetCount - 1; i >= 0; i--) {
                    if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                    focused = FindFocusedInTree(widgets[i], maxDepth);
                    if (focused) {
                        *outScopeRoot = static_cast<Noesis::DependencyObject*>(widgets[i]);
                        return focused;
                    }
                }
            } else if (root) {
                focused = FindFocusedInTree(root, maxDepth);
                if (focused) {
                    *outScopeRoot = static_cast<Noesis::DependencyObject*>(root);
                    return focused;
                }
            }
        }

        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WARN("[BG3Access] FindFocusedElement_SEH: fault during focus tree walk");
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
        WARN("[BG3Access] FindSelectedTab_SEH: fault during selection tree walk");
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

            Noesis::Visual* cur = widget;
            for (int depth = 0; depth < 5; depth++) {
                auto childCount = cur->GetVisualChildrenCount();
                if (childCount == 0) break;

                auto child = cur->GetVisualChild(0);
                if (!child) break;
                if (!ProbeUIElement(static_cast<Noesis::UIElement*>(child))) break;

                auto childFE = static_cast<Noesis::FrameworkElement*>(child);
                auto found = Noesis::FrameworkElementHelpers::FindNodeName(childFE, name);
                if (found) {
                    return static_cast<Noesis::FrameworkElement*>(found);
                }

                cur = child;
            }
        }

        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WARN("[BG3Access] FindNameInWidget: SEH fault for '%s'", name ? name : "(null)");
        return nullptr;
    }
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
        WARN("[BG3Access] FindNameInWidgetScoped: SEH fault for '%s'",
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
        WARN("[BG3Access] GetFocusedElement: SEH fault");
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
        return (*dragDrop)->PlayerData.try_get_ptr(playerId);
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

// Free functions wrapping GlobalFocusMonitor singleton -- called from
// LuaClient.cpp which is a different translation unit.
void TickGlobalFocusMonitor()
{
    // Do NOT tick during loading/teardown states.  Noesis UI elements are
    // torn down and rebuilt during loads; walking stale element pointers can
    // hang the thread (pointer lands on memory the loader is paging in, so
    // SEH never triggers -- it's a deadlock, not a fault).
    auto clientState = GetStaticSymbols().GetClientState();
    if (clientState) {
        switch (*clientState) {
        case ecl::GameState::SwapLevel:
        case ecl::GameState::LoadLevel:
        case ecl::GameState::LoadModule:
        case ecl::GameState::LoadSession:
        case ecl::GameState::UnloadLevel:
        case ecl::GameState::UnloadModule:
        case ecl::GameState::UnloadSession:
        case ecl::GameState::StartLoading:
        case ecl::GameState::StopLoading:
        case ecl::GameState::StartServer:
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
        WARN("[BG3Access] CRASH in Tick() caught by top-level SEH -- frame skipped");
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

// Read a named TypeProperty as std::string.  Handles String, CStringPtr,
// LocaString (TranslatedString), bool, int, float.  Returns empty for
// unrecognised or object types.
static std::string ReadTypePropertyAsString(Noesis::BaseObject const* obj,
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
        // Read the underlying integer and look up the symbolic name.
        int64_t enumValue = 0;
        if (!SafeTypePropertyGetCopy_SEH(prop, obj, &enumValue)) return {};
        auto enumType = static_cast<Noesis::TypeEnum const*>(type);
        for (auto const& entry : enumType->mValues) {
            if (entry.second == enumValue) {
                return std::string(entry.first.Str());
            }
        }
        // Enum value not found in the mapping -- return the numeric value
        // so callers at least get something.
        return std::to_string(enumValue);
    }
    // Pointer / Ptr / object types -- not convertible to string here.
    return {};
}

// Read a named DependencyProperty as std::string.
//
// Primary path: scan mValues for the DP and read through StoredValue.
// StoredValue::ComplexValue::base holds the cached binding result, so
// this correctly reads bound properties (Content, Text with converters).
//
// Fallback: DependencyProperty::GetValue() for inherited/default values
// not present in mValues.
static std::string ReadDepPropertyAsString(Noesis::DependencyObject const* depObj,
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
                // Scalar types: read directly from the raw value
                if (type == types.String.Type) {
                    auto str = reinterpret_cast<Noesis::String*>(rawVal);
                    if (str && str->Size() > 0) return str->Str();
                } else if (type == types.Bool.Type) {
                    return *static_cast<bool*>(rawVal) ? "On" : "Off";
                } else if (type == types.Int32.Type) {
                    return std::to_string(*static_cast<int32_t*>(rawVal));
                } else if (type == types.Single.Type) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.0f", *static_cast<float*>(rawVal));
                    return buf;
                }

                // Object-typed DP (Content, etc.): rawVal IS the object pointer.
                // The binding converter cached its result as the base value.
                // Only attempt ToString if the DP type actually descends from
                // BaseObject -- otherwise rawVal is a raw data pointer for an
                // unhandled value type (enum, struct, Thickness, etc.) and
                // reinterpret_casting it as BaseObject* would crash.
                auto& classes = Noesis::gStaticSymbols.TypeClasses;
                if (Noesis::TypeHelpers::IsDescendantOf(type, classes.BaseObject.Type)) {
                    auto obj = reinterpret_cast<Noesis::BaseObject*>(rawVal);
                    if (obj) {
                        auto str = Noesis::ObjectHelpers::ToString(obj);
                        if (!str.empty()
                            && str.find("Noesis::") != 0
                            && str.find("ls.") != 0
                            && str.find("[ForceUpdate]") == std::string::npos) {
                            return std::string(str.data(), str.size());
                        }
                    }
                }
            }
        }
    }

    // --- Fallback: GetValue (inherited/default values not in mValues) ---
    auto val = dp->GetValue(depObj);
    if (!val) return {};

    if (type == types.String.Type) {
        auto str = reinterpret_cast<Noesis::String const*>(val);
        if (str && str->Size() > 0) return str->Str();
    } else if (type == types.Bool.Type) {
        return *static_cast<const bool*>(val) ? "On" : "Off";
    } else if (type == types.Int32.Type) {
        return std::to_string(*static_cast<const int32_t*>(val));
    } else if (type == types.Single.Type) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", *static_cast<const float*>(val));
        return buf;
    }

    return {};
}

// Read a DP value directly from mValues by scanning for a matching DP name.
// Bypasses the class cache entirely -- works for inherited DPs like Content
// on ContentControl subclasses that may not be in the subclass's Names map.
// Returns the string value from StoredValue::ComplexValue::base (cached
// binding result) or StoredValue::simple.
static std::string ReadDPFromMValues(Noesis::DependencyObject const* depObj,
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

        if (dpType == types.String.Type) {
            auto str = reinterpret_cast<Noesis::String*>(rawVal);
            if (str && str->Size() > 0) return str->Str();
        } else if (dpType == types.Bool.Type) {
            return *static_cast<bool*>(rawVal) ? "On" : "Off";
        } else if (dpType == types.Int32.Type) {
            return std::to_string(*static_cast<int32_t*>(rawVal));
        } else if (dpType == types.Single.Type) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f", *static_cast<float*>(rawVal));
            return buf;
        }

        // Object-typed DP: rawVal is the cached binding result.
        // Only attempt ToString if the DP type descends from BaseObject.
        auto& classes = Noesis::gStaticSymbols.TypeClasses;
        if (Noesis::TypeHelpers::IsDescendantOf(dpType, classes.BaseObject.Type)) {
            auto cachedObj = reinterpret_cast<Noesis::BaseObject*>(rawVal);
            if (cachedObj) {
                auto str = Noesis::ObjectHelpers::ToString(cachedObj);
                if (!str.empty()
                    && str.find("Noesis::") != 0
                    && str.find("ls.") != 0
                    && str.find("[ForceUpdate]") == std::string::npos) {
                    return std::string(str.data(), str.size());
                }
            }
        }
    }

    return {};
}

// Read a named property from a Noesis object, returning std::string.
// Checks TypeProperties, then DependencyProperties (class cache), then
// scans mValues directly (catches inherited DPs not in subclass cache).
//
// This is the foundational helper for all C++ text extraction.
static std::string ReadPropertyAsString(Noesis::BaseObject const* obj,
                                         const char* propName)
{
    if (!obj) return {};

    auto const& cls = Noesis::gClassCache.GetClass(obj->GetClassType());
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

// ---------------------------------------------------------------------------
// ReadTextBlockText: three-step TextBlock text extraction in C++.
//
// 1. GetProperty("Text") -- works for local/non-bound values
// 2. Inlines collection iteration (Run.Text + LineBreak spacing)
// 3. ToString() fallback
//
// Mirrors the Lua GatherTextBlockTexts logic for a single TextBlock.
// ---------------------------------------------------------------------------
static std::string ReadTextBlockText(Noesis::FrameworkElement* elem, bool skipToString)
{
    if (!elem) return {};

    // Step 1: Try "Text" property directly.
    auto text = ReadPropertyAsString(elem, "Text");
    if (!text.empty() && text.find("[ForceUpdate]") == std::string::npos) {
        return text;
    }

    // Step 2: Iterate Inlines collection (Run.Text + LineBreak spacing).
    {
        auto const& cls = Noesis::gClassCache.GetClass(elem->GetClassType());
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
                int count = coll->Count();
                if (count > 0) {
                    std::string parts;
                    for (int i = 0; i < count; i++) {
                        auto component = coll->GetComponent(i);
                        if (!component) continue;
                        auto inlineObj = component.GetPtr();
                        if (!inlineObj) continue;

                        auto typeName = inlineObj->GetClassType()->GetName();
                        if (strstr(typeName, "Run")) {
                            auto runText = ReadPropertyAsString(inlineObj, "Text");
                            if (!runText.empty()
                                && runText.find("[ForceUpdate]") == std::string::npos) {
                                parts += runText;
                            }
                        } else if (strstr(typeName, "LineBreak")) {
                            parts += " ";
                        }
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

    // Step 3: ToString() fallback.  Skip when called from NameScope
    // collection -- ToString() evaluates bindings at runtime, which crashes
    // on TextBlocks whose bindings haven't resolved yet.  Steps 1+2 read
    // stored values only, which is safe.
    if (!skipToString) {
        auto str = Noesis::ObjectHelpers::ToString(elem);
        if (!str.empty()
            && str.find("TextBlock") == std::string::npos
            && str.find("[ForceUpdate]") == std::string::npos) {
            return std::string(str.data(), str.size());
        }
    }

    return {};
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
        WARN("[BG3Access]   NameScope: CRASH in CollectNamedTextsFromWidget, skipping");
    }
}

// ---------------------------------------------------------------------------
// GatherVisibleTextBlocks: BFS walk of the visual tree to find TextBlocks
// with readable text.  Used as a fallback when NameScope iteration finds
// nothing (e.g., splash screen text inside ControlTemplates).
//
// Safety:
// - Only called on fresh pointers from the current tick
// - IsVisibleDP pruning skips collapsed branches
// - Depth and node count caps prevent runaway walks
// - Does not recurse into child UIWidgets (separate scope)
// - ReadTextBlockText is proven safe (used throughout the codebase)
// - SEH guard at the call site catches unexpected crashes
// ---------------------------------------------------------------------------
static void GatherVisibleTextBlocks(
    Noesis::FrameworkElement* root,
    std::vector<std::string>& outTexts,
    int maxDepth,
    int maxNodes)
{
    if (!root) return;

    struct QueueEntry {
        Noesis::Visual* node;
        int depth;
    };
    // Stack-allocated queue with fixed capacity.
    std::vector<QueueEntry> queue;
    queue.reserve(maxNodes);
    queue.push_back({root, 0});
    int processed = 0;

    while (processed < (int)queue.size() && processed < maxNodes) {
        auto entry = queue[processed++];
        if (!entry.node || entry.depth > maxDepth) continue;
        // Validate before virtual calls -- element may be stale.
        if (!ProbeUIElement(static_cast<Noesis::UIElement*>(entry.node))) continue;
        if (!IsVisibleDP(entry.node)) continue;

        // Do not recurse into child UIWidgets -- they have their own scope.
        if (entry.node != root && IsUIWidgetType(entry.node)) continue;

        auto typeName = entry.node->GetClassType()->GetName();

        // Found a TextBlock: read its text and stop recursing into it
        // (children are Inline objects handled by ReadTextBlockText).
        if (strstr(typeName, "TextBlock")) {
            auto text = ReadTextBlockText(
                static_cast<Noesis::FrameworkElement*>(entry.node));
            if (!text.empty()
                && text.find("[ForceUpdate]") == std::string::npos
                && text.find("s_HandleUnknown") == std::string::npos) {
                outTexts.push_back(std::move(text));
            }
            continue;
        }

        // Enqueue visible children for BFS.
        auto childCount = entry.node->GetVisualChildrenCount();
        for (uint32_t i = 0; i < childCount
             && (int)queue.size() < maxNodes; i++) {
            queue.push_back({entry.node->GetVisualChild(i), entry.depth + 1});
        }
    }
}

// ---------------------------------------------------------------------------
// ShallowChildTextScan: bounded BFS through visual children to find
// TextBlock text.  Used by ExtractElementData for ContentControl/Control
// elements whose displayed text is in template-generated children.
// ---------------------------------------------------------------------------
static std::string ShallowChildTextScan(Noesis::FrameworkElement* elem)
{
    if (!elem) return {};
    std::vector<Noesis::Visual*> childQueue(64);
    int childFront = 0, childBack = 0;
    auto seedCount = elem->GetVisualChildrenCount();
    for (uint32_t i = 0; i < seedCount && childBack < 64; i++) {
        auto child = elem->GetVisualChild(i);
        if (child) childQueue[childBack++] = child;
    }
    for (int level = 0; level < 5 && childFront < childBack; level++) {
        int levelEnd = childBack;
        while (childFront < levelEnd) {
            auto cur = childQueue[childFront++];
            // Validate before virtual calls -- element may be stale.
            if (!ProbeUIElement(static_cast<Noesis::UIElement*>(cur))) continue;
            auto curTypeName = cur->GetClassType()->GetName();
            if (strstr(curTypeName, "TextBlock")) {
                auto tbText = ReadTextBlockText(
                    static_cast<Noesis::FrameworkElement*>(cur));
                if (!tbText.empty()
                    && tbText.find("[ForceUpdate]") == std::string::npos
                    && tbText.find("s_HandleUnknown") == std::string::npos) {
                    return tbText;
                }
            }
            auto childChildCount = cur->GetVisualChildrenCount();
            for (uint32_t i = 0; i < childChildCount && childBack < 64; i++) {
                auto child = cur->GetVisualChild(i);
                if (child) childQueue[childBack++] = child;
            }
        }
    }
    return {};
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
                WARN("[BG3Access]   NameScope: skipping overlay widget DC=%s", dcTypeName);
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

        auto childCount = current->GetVisualChildrenCount();
        if (childCount == 0) break;
        auto child = current->GetVisualChild(0);
        if (!child) break;
        // Validate before virtual calls -- element may be stale.
        if (!ProbeUIElement(static_cast<Noesis::UIElement*>(child))) break;
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
            WARN("[BG3Access]     NameScope: UNSAFE element '%s', skipping", elementName);
            continue;
        }

        WARN("[BG3Access]     NameScope: reading '%s' (elem=%p)", elementName, textBlockElem);
        auto text = ReadTextBlockText(textBlockElem, true);
        if (text.empty()) continue;
        if (text.find("[ForceUpdate]") != std::string::npos) continue;
        if (text.find("s_HandleUnknown") != std::string::npos) continue;

        WARN("[BG3Access]     NameScope: %s = %s", elementName, text.c_str());
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

// Lua-table version: pushes SelectedBonusAbility into dcProps table.
// Called from ExtractElementInfo path (Lua API calls).
// Read the selected ability name from BonusAbilities[SelectedIndex].
// Uses TypeProperty::GetComponent to properly unwrap Ptr<> wrappers.
// No SEH needed -- GetComponent and Count are safe when called on
// valid TypeProperties from the class cache.
static void TryReadSelectedBonusAbility(
    Noesis::BaseObject* dc, lua_State* L, int dcPropsIdx)
{
    auto const& cls = Noesis::gClassCache.GetClass(dc->GetClassType());

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

    // Get the collection via GetComponent (handles Ptr<> unwrapping).
    auto collectionComponent = bonusProp->GetComponent(dc);
    auto collectionRaw = collectionComponent.GetPtr();
    if (!collectionRaw) return;

    auto collection = static_cast<Noesis::BaseCollection*>(collectionRaw);
    int count = SafeCollectionCount(collection);
    if (selectedIndex >= count) return;

    // Get the selected item.
    auto itemPtr = collection->GetComponent((uint32_t)selectedIndex);
    auto item = itemPtr.GetPtr();
    if (!item) return;

    // Read Ability TypeProperty from the item.
    auto const& itemCls = Noesis::gClassCache.GetClass(item->GetClassType());
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
            return;
        }
    }
}


static void PushDCProperties(lua_State* L, Noesis::BaseObject* dc)
{
    if (!dc) {
        lua_pushnil(L);
        return;
    }

    auto const& cls = Noesis::gClassCache.GetClass(dc->GetClassType());
    auto& types = Noesis::gStaticSymbols.Types;
    auto& classes = Noesis::gStaticSymbols.TypeClasses;

    lua_newtable(L);

    for (auto& entry : cls.Names) {
        auto propInfo = &entry.Value();
        if (!propInfo->Property) continue;

        auto type = UnwrapType(propInfo->Property->GetContentType());
        if (!type) continue;

        auto typeOfType = type->GetClassType();

        // Scalar types: read as string and add to table.
        if (type == types.String.Type
            || type == types.CStringPtr.Type
            || type == types.LocaString.Type
            || type == types.Bool.Type
            || type == types.Int32.Type
            || type == types.UInt32.Type
            || type == types.Int64.Type
            || type == types.Single.Type
            || type == types.Double.Type) {

            auto val = ReadTypePropertyAsString(dc, propInfo->Property);
            if (!val.empty()
                && val.find("[ForceUpdate]") == std::string::npos) {
                lua_pushstring(L, entry.Key().GetString());
                lua_pushstring(L, val.c_str());
                lua_settable(L, -3);
            }
            continue;
        }

        // Object types (Ptr<T>, T*): try to read scalar sub-properties
        // as a nested table.  Covers SelectedItem, etc.
        bool isPtr = (typeOfType == types.TypePtr.Type)
                  || (typeOfType == types.TypePointer.Type)
                  || Noesis::TypeHelpers::IsDescendantOf(
                         type, classes.BaseObject.Type);
        if (isPtr) {
            Noesis::BaseObject* subObj = nullptr;
            if (typeOfType == types.TypePtr.Type) {
                auto ptrVal = reinterpret_cast<Noesis::Ptr<Noesis::BaseRefCounted>*>(
                    const_cast<void*>(propInfo->Property->Get(dc)));
                if (ptrVal) subObj = static_cast<Noesis::BaseObject*>(ptrVal->GetPtr());
            } else if (typeOfType == types.TypePointer.Type) {
                propInfo->Property->GetCopy(dc, &subObj);
            } else {
                subObj = reinterpret_cast<Noesis::BaseObject*>(
                    const_cast<void*>(propInfo->Property->Get(dc)));
            }

            if (subObj) {
                // Read scalar sub-properties from the sub-object.
                auto const& subCls = Noesis::gClassCache.GetClass(subObj->GetClassType());
                lua_newtable(L);
                bool hasAny = false;

                // Add the sub-object's type name.
                lua_pushstring(L, "_type");
                lua_pushstring(L, subObj->GetClassType()->GetName());
                lua_settable(L, -3);

                for (auto& subEntry : subCls.Names) {
                    if (!subEntry.Value().Property) continue;
                    auto subType = UnwrapType(subEntry.Value().Property->GetContentType());
                    if (!subType) continue;
                    if (subType == types.String.Type
                        || subType == types.CStringPtr.Type
                        || subType == types.LocaString.Type
                        || subType == types.Bool.Type
                        || subType == types.Int32.Type
                        || subType == types.UInt32.Type
                        || subType == types.Single.Type) {
                        auto subVal = ReadTypePropertyAsString(subObj, subEntry.Value().Property);
                        if (!subVal.empty()
                            && subVal.find("[ForceUpdate]") == std::string::npos) {
                            lua_pushstring(L, subEntry.Key().GetString());
                            lua_pushstring(L, subVal.c_str());
                            lua_settable(L, -3);
                            hasAny = true;
                        }
                    }
                }



                if (hasAny) {
                    lua_pushstring(L, entry.Key().GetString());
                    lua_insert(L, -2);  // swap key and table
                    lua_settable(L, -3);
                } else {
                    lua_pop(L, 1);  // discard empty sub-table
                }
            }
        }
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
static std::string ExtractTabName(Noesis::FrameworkElement* elem)
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
        auto dcVal = sDataContextProp->GetValue(depObj);
        if (dcVal) {
            auto dataContext = *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
            if (dataContext) {
                auto& types = Noesis::gStaticSymbols.Types;
                auto const& cls = Noesis::gClassCache.GetClass(dataContext->GetClassType());
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
        auto seedCount = elem->GetVisualChildrenCount();
        for (uint32_t i = 0; i < seedCount && back < 256; i++) {
            auto child = elem->GetVisualChild(i);
            if (child) queue[back++] = child;
        }
        for (int level = 0; level < 10 && front < back; level++) {
            int levelEnd = back;
            while (front < levelEnd) {
                auto cur = queue[front++];
                // Validate before virtual calls -- element may be stale.
                if (!ProbeUIElement(static_cast<Noesis::UIElement*>(cur))) continue;
                auto typeName = SafeBaseObjectTypeName_SEH(cur);
                if (typeName && strstr(typeName, "TextBlock")) {
                    auto tbText = ReadTextBlockText(
                        static_cast<Noesis::FrameworkElement*>(cur));
                    if (!tbText.empty()) return tbText;
                }
                // Enqueue children.
                auto cc = cur->GetVisualChildrenCount();
                for (uint32_t i = 0; i < cc && back < 256; i++) {
                    auto child = cur->GetVisualChild(i);
                    if (child) queue[back++] = child;
                }
            }
        }
    }

    // Try 5: ToString() on element.
    {
        auto str = Noesis::ObjectHelpers::ToString(elem);
        auto typeName = elem->GetClassType()->GetName();
        if (!str.empty() && str != typeName
            && str.find("Noesis::") != 0
            && str.find("ls.") != 0) {
            return std::string(str.data(), str.size());
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

// ---------------------------------------------------------------------------
// ExtractElementInfo: builds a Lua table with all fields from a focused
// element.  Called during Tick() when the element is alive.
//
// Pushes a table onto the Lua stack with:
//   elemType, elemName, elemId, isTab, isOption,
//   text, tabName, dcType, dcBody, widgetRootId, isFocusable
// ---------------------------------------------------------------------------
static void ExtractElementInfo(lua_State* L, Noesis::FrameworkElement* elem)
{
    if (!elem) {
        lua_pushnil(L);
        return;
    }

    lua_newtable(L);

    // elemType: GetClassType()->GetName()
    auto typeName = elem->GetClassType()->GetName();
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

    // isFocusable: ls:MoveFocus.Focusable
    bool isFocusable = false;
    if (sLSMoveFocusFocusableProp) {
        auto depObj = static_cast<Noesis::DependencyObject*>(elem);
        auto val = sLSMoveFocusFocusableProp->GetValue(depObj);
        isFocusable = val && *static_cast<const bool*>(val);
    }
    lua_pushstring(L, "isFocusable");
    lua_pushboolean(L, isFocusable);
    lua_settable(L, -3);

    // DataContext reading.
    Noesis::BaseComponent* dataContext = nullptr;
    std::string dcTypeName;
    if (sDataContextProp) {
        auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
        auto dcVal = sDataContextProp->GetValue(depObj);
        if (dcVal) {
            dataContext = *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
            if (dataContext) {
                dcTypeName = dataContext->GetClassType()->GetName();
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
    lua_pushstring(L, "dcProps");
    if (dataContext) {
        PushDCProperties(L, dataContext);
        // Post-process: read selected ability name for Ability Bonus selector.
        // BonusAbilities collection isn't handled by the generic sub-object
        // path (its TypeProperty type isn't recognized as a pointer).
        TryReadSelectedBonusAbility(dataContext, L, lua_gettop(L));
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
        auto str = Noesis::ObjectHelpers::ToString(elem);
        if (!str.empty() && str != typeName
            && str.find("Noesis::") != 0
            && str.find("ls.") != 0
            && str.find("[ForceUpdate]") == std::string::npos) {
            elemText = std::string(str.data(), str.size());
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

    // widgetRootId: walk parent chain to UIWidget, stringify pointer.
    lua_pushstring(L, "widgetRootId");
    {
        Noesis::Visual* cur = elem;
        Noesis::Visual* widgetRoot = nullptr;
        for (int i = 0; i < 64 && cur; i++) {
            if (IsUIWidgetType(cur)) {
                widgetRoot = cur;
                break;
            }
            cur = cur->mVisualParent;
        }
        if (widgetRoot) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%p", static_cast<void*>(widgetRoot));
            lua_pushstring(L, buf);
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
// Mirrors PushDCProperties but stores into vectors instead of Lua tables.
// ---------------------------------------------------------------------------
static void CollectDCProperties(FocusEventData& out, Noesis::BaseObject* dc)
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
        bool isEnum = Noesis::TypeHelpers::IsDescendantOf(
            typeOfType, Noesis::gStaticSymbols.TypeClasses.TypeEnum.Type);
        if (type == types.String.Type
            || type == types.CStringPtr.Type
            || type == types.LocaString.Type
            || type == types.Bool.Type
            || type == types.Int32.Type
            || type == types.UInt32.Type
            || type == types.Int64.Type
            || type == types.Single.Type
            || type == types.Double.Type
            || isEnum) {

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
                auto const& subCls = Noesis::gClassCache.GetClass(subClassType);
                FocusEventData::SubObject subData;
                subData.propName = entry.Key().GetString();
                subData.typeName = subClassType->GetName();

                for (auto& subEntry : subCls.Names) {
                    if (!subEntry.Value().Property) continue;
                    auto subType = UnwrapType(subEntry.Value().Property->GetContentType());
                    if (!subType) continue;
                    if (subType == types.String.Type
                        || subType == types.CStringPtr.Type
                        || subType == types.LocaString.Type
                        || subType == types.Bool.Type
                        || subType == types.Int32.Type
                        || subType == types.UInt32.Type
                        || subType == types.Single.Type) {
                        auto subVal = ReadTypePropertyAsString(subObj, subEntry.Value().Property);
                        if (!subVal.empty()
                            && subVal.find("[ForceUpdate]") == std::string::npos) {
                            subData.props.emplace_back(
                                subEntry.Key().GetString(), std::move(subVal));
                        }
                    }
                }

                if (!subData.props.empty()) {
                    out.dcObjectProps.push_back(std::move(subData));
                }
            }
        }
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

static void ExtractBindingInfo(FocusEventData& out, Noesis::FrameworkElement* elem)
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

// ---------------------------------------------------------------------------
// ExtractElementData: fills a FocusEventData struct from a FrameworkElement.
// Called during Tick() when the element is alive.  Pure C++ -- no Lua.
// Mirrors ExtractElementInfo but stores into the struct.
// ---------------------------------------------------------------------------
static void ExtractElementData(FocusEventData& out, Noesis::FrameworkElement* elem)
{
    if (!elem) return;

    // elemType
    auto elemClassTypeName = SafeBaseObjectTypeName_SEH(elem);
    if (!elemClassTypeName) return;
    out.elemType = elemClassTypeName;

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
    }

    // elemText: primary extraction -- direct property reads.
    if (strstr(out.elemType.c_str(), "TextBlock")) {
        out.elemText = ReadTextBlockText(elem);
    }
    if (out.elemText.empty()) {
        out.elemText = ReadPropertyAsString(elem, "Content");
    }
    if (out.elemText.empty()) {
        auto str = Noesis::ObjectHelpers::ToString(elem);
        if (!str.empty() && str != out.elemType.c_str()
            && str.find("Noesis::") != 0
            && str.find("ls.") != 0
            && str.find("[ForceUpdate]") == std::string::npos) {
            out.elemText = std::string(str.data(), str.size());
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

    // tabName
    if (out.isTab) {
        out.tabName = ExtractTabName(elem);
    }

    // widgetRootId
    {
        Noesis::Visual* cur = elem;
        Noesis::Visual* widgetRoot = nullptr;
        for (int i = 0; i < 64 && cur; i++) {
            if (IsUIWidgetType(cur)) {
                widgetRoot = cur;
                break;
            }
            cur = cur->mVisualParent;
        }
        if (widgetRoot) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%p", static_cast<void*>(widgetRoot));
            out.widgetRootId = buf;
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

// ---------------------------------------------------------------------------
// GetFocusedElementInfo: Lua-exposed API that returns the data table for
// the currently focused element.  Uses all three focus strategies, then
// calls ExtractElementInfo to build the table.
//
// Returns a table on success, nil if nothing is focused.
// ---------------------------------------------------------------------------
UserReturn GetFocusedElementInfo(lua_State* L)
{
    auto root = GetRoot();
    if (!root) {
        lua_pushnil(L);
        return 1;
    }

    InitFocusProperties(root);

    // Use the same multi-strategy focus detection as GetFocusedElement.
    Noesis::UIElement* focused = nullptr;

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

    if (!focused) {
        lua_pushnil(L);
        return 1;
    }

    // All visual tree elements are FrameworkElements.
    auto focusedElem = static_cast<Noesis::FrameworkElement*>(focused);
    ExtractElementInfo(L, focusedElem);
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
    // Phase 1 refactor: C++ data extraction test API
    MODULE_FUNCTION(GetFocusedElementInfo)
    END_MODULE()
}

END_NS()
