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

// INPCMonitor: subscribes to INotifyPropertyChanged on a ViewModel.
// Used to detect value changes (tickbox toggle, slider adjust, combo select).
class INPCMonitor
{
public:
    static INPCMonitor& Instance() { static INPCMonitor inst; return inst; }

    bool Subscribe(lua_State* L, Noesis::BaseComponent* target, lua::RegistryEntry&& callback)
    {
        Unsubscribe();
        auto notifies = Noesis::DynamicCast<Noesis::INotifyPropertyChanged*, Noesis::BaseComponent*>(target);
        if (!notifies) return false;

        target_ = notifies;
        callback.Push(L);
        callback_ = lua::PersistentRegistryEntry(L, -1);
        lua_pop(L, 1);

        notifies->PropertyChanged().Add(
            Noesis::MakeDelegate(this, &INPCMonitor::OnChanged));
        return true;
    }

    void Unsubscribe()
    {
        // Don't call Remove on a potentially dead object.
        target_ = nullptr;
        callback_ = {};
    }

private:
    void OnChanged(Noesis::BaseComponent* sender,
                   const Noesis::PropertyChangedEventArgs& args)
    {
        if (!callback_) return;
        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                pin->GetDeferredUIEvents().OnPropertyChanged(
                    callback_, sender, args.propertyName);
            }
        }
    }

    Noesis::INotifyPropertyChanged* target_ = nullptr;
    lua::PersistentRegistryEntry callback_;
};

// DPMonitor: subscribes to DependencyPropertyChanged on a UI element.
// Used to detect when a data-bound property (e.g. TextBlock.Text) resolves.
// Mirrors INPCMonitor's singleton pattern and deferred callback mechanism.
class DPMonitor
{
public:
    static DPMonitor& Instance() { static DPMonitor inst; return inst; }

    bool Subscribe(lua_State* L, Noesis::DependencyObject* target, lua::RegistryEntry&& callback)
    {
        Unsubscribe();
        if (!target) return false;

        target_ = target;
        callback.Push(L);
        callback_ = lua::PersistentRegistryEntry(L, -1);
        lua_pop(L, 1);

        target->mDependencyPropertyChangedEvent.Add(
            Noesis::MakeDelegate(this, &DPMonitor::OnChanged));
        return true;
    }

    void Unsubscribe()
    {
        // Don't call Remove on a potentially dead object.
        target_ = nullptr;
        callback_ = {};
    }

private:
    void OnChanged(Noesis::BaseComponent* sender,
                   const Noesis::DependencyPropertyChangedEventArgs& args)
    {
        if (!callback_) return;
        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                // Reuse OnPropertyChanged -- same signature (handler, sender, propName).
                pin->GetDeferredUIEvents().OnPropertyChanged(
                    callback_, sender, args.prop->GetName());
            }
        }
    }

    Noesis::DependencyObject* target_ = nullptr;
    lua::PersistentRegistryEntry callback_;
};

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
// The newly selected element from the last SelectionChanged event.
// May be a ListBoxItem (container) or a ViewModel (data object).
// Strategy 3 uses this directly instead of tree-walking.
static Noesis::BaseComponent* sSelectionChangedItem = nullptr;
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

// Forward declarations for data extraction (defined after ExtractElementInfo).
static void ExtractElementData(FocusEventData& out, Noesis::FrameworkElement* elem);
static void CollectDCProperties(FocusEventData& out, Noesis::BaseObject* dc);
static std::string ReadPropertyAsString(Noesis::BaseObject const* obj, const char* propName);
static void ExtractBindingInfo(FocusEventData& out, Noesis::FrameworkElement* elem);

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

// Handler for Selector.SelectionChanged routed events.  Uses the
// DummyDelegate pattern (same as UIEventHooks): 'this' is a fake pointer
// that is never dereferenced.  Extracts the selected element directly
// from the event args -- zero tree walking needed.
struct SelectionDirtyDelegate
{
    void Handler(Noesis::BaseComponent* source, const Noesis::RoutedEventArgs& args)
    {
        sSelectionDirtyFlag = true;
        // Extract the newly selected item from SelectionChangedEventArgs.
        auto& selArgs = static_cast<const Noesis::SelectionChangedEventArgs&>(args);
        if (selArgs.addedItems.Size() > 0) {
            sSelectionChangedItem = selArgs.addedItems[0].GetPtr();
        } else {
            sSelectionChangedItem = nullptr;
        }
    }
};

// Fixed fake 'this' pointer for SelectionDirtyDelegate subscription.
// Non-null, unique, consistent between Subscribe and Remove calls.
static SelectionDirtyDelegate* const kSelectionDirtyDelegatePtr =
    reinterpret_cast<SelectionDirtyDelegate*>(static_cast<uintptr_t>(0xACC5E1));

class GlobalFocusMonitor
{
public:
    static constexpr uint32_t kMaxWidgets = 16;
    static constexpr int kWidgetSettleTicks = 10;  // ~150ms at 60fps, ~70ms at 144fps
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
        lastFocused_ = nullptr;
        lastSelected_ = nullptr;
        lastSelectedDC_ = nullptr;
        cachedRoot_ = nullptr;
        widgetContainer_ = nullptr;
        prevWidgetCount_ = 0;
        hadFocusBefore_ = false;
        pendingWidgetFire_ = false;
        widgetSettleCount_ = 0;
        initialSelectionDone_ = false;
        sSelectionDirtyFlag = false;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgets_[i] = nullptr;
            preAddWidgets_[i] = nullptr;
        }
        preAddWidgetCount_ = 0;
        return true;
    }

    void Unsubscribe()
    {
        UnsubscribeINPC();
        UnsubscribeWidgetINPC();
        UnsubscribeAllSelectionChanged();
        callback_ = {};
        lastFocused_ = nullptr;
        lastSelected_ = nullptr;
        lastSelectedDC_ = nullptr;
    }

    bool IsActive() const { return callback_.operator bool(); }

    void ForceNextFire() { forceNext_ = true; }

    void Tick()
    {
        if (!callback_) return;

        auto root = GetRoot();
        if (!root) return;
        InitFocusProperties(root);

        // ----- Dynamic widget container discovery -----
        // When root changes (state transition rebuilds the entire tree),
        // re-discover the container.  Also retry if previously null.
        if (root != cachedRoot_) {
            cachedRoot_ = root;
            widgetContainer_ = FindWidgetContainer(root);
        }
        if (!widgetContainer_) {
            widgetContainer_ = FindWidgetContainer(root);
        }

        // ----- Gather current widget set -----
        uint32_t widgetCount = 0;
        Noesis::Visual* widgets[kMaxWidgets] = {};
        if (widgetContainer_) {
            auto count = widgetContainer_->GetVisualChildrenCount();
            widgetCount = (count < kMaxWidgets) ? count : kMaxWidgets;
            for (uint32_t i = 0; i < widgetCount; i++)
                widgets[i] = widgetContainer_->GetVisualChild(i);
        }

        // ----- Lazy FocusedElement property discovery -----
        // FocusManager type may not resolve via Reflection::GetType() in
        // the Indie SDK.  Scan widget mValues to find the attached property.
        if (!sFocusedElementProp && widgetCount > 0) {
            TryDiscoverFocusedElementProp(widgets, widgetCount);
        }

        // ----- Strategy 1: FocusManager.FocusedElement (FAST PATH) -----
        // Checks FocusManager.FocusedElement directly on each widget.
        // O(1) per widget -- no tree walk.
        // TryFocusManager recurses into children only if the property
        // is not set at the widget level (handles nested focus scopes).
        Noesis::UIElement* focused = nullptr;
        Noesis::DependencyObject* scopeRoot = nullptr;
        if (widgetCount > 0) {
            for (int i = (int)widgetCount - 1; i >= 0; i--) {
                if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                focused = TryFocusManager(widgets[i], kMaxTreeDepth, &scopeRoot);
                if (focused) break;
            }
        }

        // ----- Strategy 2: IsFocused + ls:MoveFocus.IsFocused tree walk -----
        // FALLBACK for when no FocusedElement property is set (safety net).
        // Walks the visual tree checking ls:MoveFocus.IsFocused first
        // (controller focus), then UIElement.IsFocused (keyboard focus).
        // IsVisible pruning skips collapsed branches for speed.
        if (!focused && (sIsFocusedProp || sLSMoveFocusIsFocusedProp) && widgetCount > 0) {
            for (int i = (int)widgetCount - 1; i >= 0; i--) {
                if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                focused = FindFocusedInTree(widgets[i], kMaxTreeDepth);
                if (focused) {
                    scopeRoot = static_cast<Noesis::DependencyObject*>(widgets[i]);
                    break;
                }
            }
        }

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
        bool selectionEventFired = sSelectionDirtyFlag;
        sSelectionDirtyFlag = false;

        // Quick widget change check for Strategy 3 gating (full detection
        // is in Strategy 4 below).  New widgets may have pre-selected tabs.
        bool widgetsLikelyChanged = (widgetCount != prevWidgetCount_);
        if (!widgetsLikelyChanged && widgetCount > 0) {
            // Check first and last widget pointers as a fast heuristic.
            if (widgets[0] != prevWidgets_[0]
                || widgets[widgetCount - 1] != prevWidgets_[widgetCount - 1])
                widgetsLikelyChanged = true;
        }
        // Only reset initialSelectionDone_ when widgets are ADDED (new
        // menu entry), not when they're swapped (tab content change).
        // Tab switches within the same menu fire SelectionChanged which
        // already triggers Strategy 3 via the dirty flag -- resetting
        // initialSelectionDone_ would cause a redundant second run.
        if (widgetsLikelyChanged && widgetCount > prevWidgetCount_) {
            initialSelectionDone_ = false;
        }

        // When event subscription is active, only run tree walk when needed.
        // Without subscription, falls back to polling every frame.
        bool shouldRunStrategy3 = !sSelectionChangedEvent
                               || subscribedWidgetCount_ == 0
                               || selectionEventFired
                               || !initialSelectionDone_
                               || forceNext_;

        Noesis::UIElement* selected = nullptr;
        if (sIsSelectedProp && sListBoxItemType && shouldRunStrategy3) {
            strategy3Runs_++;
            initialSelectionDone_ = true;

            // FAST PATH: use the element from SelectionChanged event args.
            // The event handler already extracted addedItems[0] -- use it
            // directly if it's a ListBoxItem.  Zero tree walking.
            if (selectionEventFired && sSelectionChangedItem) {
                auto itemCls = sSelectionChangedItem->GetClassType();
                bool isListBoxItem = false;
                while (itemCls) {
                    if (itemCls == sListBoxItemType) { isListBoxItem = true; break; }
                    itemCls = itemCls->GetBase();
                }
                if (isListBoxItem) {
                    selected = static_cast<Noesis::UIElement*>(sSelectionChangedItem);
                    WARN("[BG3Access]   Strategy3: got selected from event args: %p", selected);
                }
            }
            sSelectionChangedItem = nullptr;

            // SLOW PATH: tree walk fallback for initial detection (no event
            // fired yet) or when the event item wasn't a ListBoxItem.
            if (!selected) {
                if (scopeRoot) {
                    selected = FindSelectedTabInTree(
                        static_cast<Noesis::Visual*>(scopeRoot), kMaxTreeDepth);
                } else if (widgetCount > 0) {
                    for (int i = (int)widgetCount - 1; i >= 0; i--) {
                        if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                        selected = FindSelectedTabInTree(widgets[i], kMaxTreeDepth);
                        if (selected) break;
                    }
                }
                if (!selected) {
                    selected = FindSelectedTabInTree(root, kMaxTreeDepth);
                }
            }

            if (selected != lastSelected_) strategy3Changes_++;
            if (strategy3Runs_ % 300 == 0) {
                WARN("[BG3Access] Strategy3 stats: %u runs, %u changes (%.1f%% wasted)",
                    strategy3Runs_, strategy3Changes_,
                    strategy3Runs_ > 0
                        ? 100.0 * (1.0 - (double)strategy3Changes_ / strategy3Runs_)
                        : 0.0);
            }
        } else if (sIsSelectedProp && sListBoxItemType) {
            // Strategy 3 skipped -- preserve last known state for
            // VALUE COMPARISON ONLY.  Do not dereference this pointer.
            selected = lastSelected_;
            sSelectionChangedItem = nullptr;  // consume stale event data
        }

        // Track whether selected was found fresh this tick (safe to
        // dereference) vs preserved from a previous tick (comparison only).
        bool selectedIsFresh = (selected != nullptr && selected != lastSelected_)
                            || (selected != nullptr && shouldRunStrategy3);

        // ----- DataContext for recycling detection -----
        const void* selectedDC = nullptr;
        if (selected && selectedIsFresh && sDataContextProp) {
            auto depObj = static_cast<Noesis::DependencyObject const*>(selected);
            auto dcVal = sDataContextProp->GetValue(depObj);
            if (dcVal) {
                selectedDC = *reinterpret_cast<const void* const*>(dcVal);
            }
        }

        bool forced = forceNext_;
        forceNext_ = false;

        // Track focus and selection independently.
        bool focusChanged = (focused != lastFocused_);
        // Selection change fires when:
        // - DataContext pointer changed (carousel recycling: same element,
        //   new DC -- fires once).
        // - Element pointer changed (static tabs like multiplayer: different
        //   ListBoxItem, but DC may be null for all of them).
        // - Selection appeared/disappeared (null transitions).
        //
        // Widget rebuild creates a new element for the same DC.  When DC is
        // non-null, the DC check catches recycling.  When DC is null, the
        // element pointer check catches the tab switch.
        bool selectionChanged = (selectedDC != lastSelectedDC_)
                             || (selected != lastSelected_);

        // ----- Strategy 4: Widget set change detection -----
        uint32_t oldWidgetCount = prevWidgetCount_;
        Noesis::Visual* oldWidgets[kMaxWidgets] = {};
        for (uint32_t i = 0; i < oldWidgetCount && i < kMaxWidgets; i++)
            oldWidgets[i] = prevWidgets_[i];

        bool widgetSetChanged = false;
        if (widgetCount != oldWidgetCount) {
            widgetSetChanged = true;
        } else {
            for (uint32_t i = 0; i < widgetCount; i++) {
                if (widgets[i] != oldWidgets[i]) {
                    widgetSetChanged = true;
                    break;
                }
            }
        }

        // When widgets change, cancel any pending deferred work that
        // references elements from the old widget set.  Without this,
        // pendingNamedTexts_ from a previous menu's tab switch would fire
        // after the old widgets are destroyed, hitting freed pointers.
        if (widgetSetChanged) {
            pendingNamedTexts_ = false;
            namedTextsSettleCount_ = 0;
            pendingNamedTextsWidget_ = nullptr;
            initialSelectionDone_ = false;
        }

        // Update cached widget set.  Raw pointers only -- do NOT hold
        // Ptr<> refs.  Noesis owns the widget lifecycle and may destroy
        // widgets during internal layout teardown, bypassing ref counting.
        // Calling Release() on an already-dead widget crashes.  We only
        // use these pointers for value comparison (detecting widget set
        // changes), never for dereferencing.
        prevWidgetCount_ = widgetCount;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgets_[i] = (i < widgetCount) ? widgets[i] : nullptr;
        }

        // ----- Diagnostic logging -----
        // Suppress log for forced-only no-op ticks (nothing found, nothing
        // changed).  These spam the log at 60fps during loading screens
        // when widgets exist but nothing has focus yet.
        bool forcedNoOp = forced && !focusChanged && !selectionChanged
                        && !widgetSetChanged && !focused && !selected;
        if ((focusChanged || selectionChanged || forced || widgetSetChanged)
            && !forcedNoOp) {
            WARN("[BG3Access] Tick: focused=%p scopeRoot=%p selected=%p selDC=%p prevSelDC=%p focChg=%d selChg=%d forced=%d wChg=%d widgets=%u",
                focused, scopeRoot, selected, selectedDC, lastSelectedDC_,
                focusChanged, selectionChanged, forced, widgetSetChanged, widgetCount);
        }

        // ----- Update last known state -----
        if (focusChanged) {
            lastFocused_ = focused;
        }

        if (selectionChanged) {
            lastSelected_ = selected;
            lastSelectedDC_ = selectedDC;
        }

        // ----- Schedule deferred namedTexts on tab switch -----
        // When a tab changes, Noesis hasn't updated Visibility states yet
        // in the same frame.  Schedule a re-collection after a settle delay
        // so IsElementVisible can reliably filter cross-tab TextBlocks.
        if (selectionChanged && selected) {
            auto selData = static_cast<Noesis::FrameworkElement*>(selected);
            // Check isTab by type
            bool isTab = false;
            if (sListBoxItemType) {
                auto cls = selData->GetClassType();
                while (cls) {
                    if (cls == sListBoxItemType) { isTab = true; break; }
                    cls = cls->GetBase();
                }
            }
            if (isTab) {
                pendingNamedTexts_ = true;
                namedTextsSettleCount_ = 10;  // fallback only; WidgetDCChanged is the primary path
                // Store the widget root address for deferred collection.
                // Walk up from the selected element NOW (it's valid this
                // tick) and save the address.  The deferred path will
                // re-find this widget in the live widget list.
                pendingNamedTextsWidget_ = nullptr;
                Noesis::Visual* cur = static_cast<Noesis::Visual*>(selected);
                for (int depth = 0; depth < 64 && cur; depth++) {
                    if (IsUIWidgetType(cur)) {
                        pendingNamedTextsWidget_ = cur;
                        break;
                    }
                    cur = cur->mVisualParent;
                }
            }
        }

        // ----- Fire callbacks (priority: selection > focus > forced) -----
        // Only dereference elements that were found FRESH this tick.
        // focused is always fresh (GetFocusedElement runs every tick).
        // selected is only fresh when Strategy 3 actually ran.
        if (selectionChanged && selected && selectedIsFresh) {
            WARN("[BG3Access]   -> FIRE selection (tab)");
            FireFocusCallback(selected);
        } else if (focusChanged && focused) {
            WARN("[BG3Access]   -> FIRE focus");
            FireFocusCallback(focused);
        } else if (forced) {
            // For forced re-fire, only use pointers that are fresh.
            auto best = (selected && selectedIsFresh) ? selected : focused;
            if (best) {
                WARN("[BG3Access]   -> FIRE forced");
                FireFocusCallback(best);
            } else {
                // Both null -- UI is rebuilding (e.g. Cross-Play tab
                // destroys and recreates the ListBoxItem tree).
                forceNext_ = true;
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
                // Same count but different pointers -- check if any are new
                for (uint32_t i = 0; i < widgetCount; i++) {
                    bool found = false;
                    for (uint32_t j = 0; j < oldWidgetCount; j++) {
                        if (widgets[i] == oldWidgets[j]) { found = true; break; }
                    }
                    if (!found) { widgetAdded = true; break; }
                }
            }

            if (widgetAdded) {
                pendingWidgetFire_ = true;
                widgetSettleCount_ = kWidgetSettleTicks;
                // Save the pre-addition widget set for comparison at fire time.
                preAddWidgetCount_ = oldWidgetCount;
                for (uint32_t i = 0; i < kMaxWidgets; i++) {
                    preAddWidgets_[i] = (i < oldWidgetCount) ? oldWidgets[i] : nullptr;
                }
            }
        }

        if (pendingWidgetFire_) {
            widgetSettleCount_--;
            if (widgetSettleCount_ <= 0) {
                pendingWidgetFire_ = false;
                // Fire for EACH new visible widget (not just the first).
                // Lua handler uses a guard to act on the first widget that
                // yields convention content, ignoring subsequent calls.
                bool fired = false;
                for (int i = (int)widgetCount - 1; i >= 0; i--) {
                    if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                    bool isNew = true;
                    for (uint32_t j = 0; j < preAddWidgetCount_; j++) {
                        if (widgets[i] == preAddWidgets_[j]) { isNew = false; break; }
                    }
                    if (isNew) {
                        WARN("[BG3Access]   -> FIRE widgetAdded (widget[%d])", i);
                        FireWidgetCallback(
                            static_cast<Noesis::UIElement*>(
                                const_cast<Noesis::Visual*>(widgets[i])));
                        fired = true;
                        // No break -- fire for each new widget so Lua can
                        // pick the one that has convention content.
                    }
                }
                if (!fired) {
                    // All pointers swapped -- fire topmost visible as fallback.
                    for (int i = (int)widgetCount - 1; i >= 0; i--) {
                        if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;
                        WARN("[BG3Access]   -> FIRE widgetAdded fallback (widget[%d])", i);
                        FireWidgetCallback(
                            static_cast<Noesis::UIElement*>(
                                const_cast<Noesis::Visual*>(widgets[i])));
                        break;
                    }
                }
            }
        }

        // ----- Deferred namedTexts re-collection on tab switch -----
        // After a tab switch, wait for Noesis to update Visibility states
        // then re-collect namedTexts from the widget.  Fires a dedicated
        // "TabNamedTexts" event so Lua can speak tab-specific authored text.
        // ----- Deferred tab speech fallback -----
        // After a tab switch, Lua stores pendingTabData and waits for
        // WidgetDCChanged (which carries fresh namedTexts) to assemble
        // speech.  This timer is a fallback for menus where WidgetDCChanged
        // doesn't fire (e.g. difficulty selection).  Sends an EMPTY
        // TabNamedTexts event -- no stale NameScope collection.  Lua
        // speaks with whatever data pendingTabData already has (tab name,
        // dcBody, etc.).
        if (pendingNamedTexts_) {
            namedTextsSettleCount_--;
            if (namedTextsSettleCount_ <= 0) {
                pendingNamedTexts_ = false;
                FocusEventData namedData;
                namedData.eventType = "TabNamedTexts";
                WARN("[BG3Access]   -> FIRE TabNamedTexts (fallback, 0 entries)");
                ContextGuardAnyThread ctx(ContextType::Client);
                if (gExtender->GetClient().HasExtensionState()) {
                    LuaClientPin pin(gExtender->GetClient().GetExtensionState());
                    if (pin) {
                        pin->GetDeferredUIEvents().OnFocusChanged(
                            callback_, std::move(namedData));
                    }
                }
            }
        }

        // Track whether we've ever had focus (for Strategy 4 guard).
        if (focused || selected) hadFocusBefore_ = true;

        // Keep polling when nothing is focused (give new widgets time
        // to settle and acquire focus).
        if (!focused && !selected) {
            if (widgetSetChanged) overlayPollCount_ = 0;
            if (overlayPollCount_ < 30) {
                forceNext_ = true;
                overlayPollCount_++;
            }
        } else {
            overlayPollCount_ = 0;
        }
    }

    void Reset()
    {
        UnsubscribeINPC();
        UnsubscribeWidgetINPC();
        UnsubscribeAllSelectionChanged();
        lastFocused_ = nullptr;
        lastSelected_ = nullptr;
        lastSelectedDC_ = nullptr;
        cachedRoot_ = nullptr;
        widgetContainer_ = nullptr;
        prevWidgetCount_ = 0;
        hadFocusBefore_ = false;
        pendingWidgetFire_ = false;
        widgetSettleCount_ = 0;
        initialSelectionDone_ = false;
        sSelectionDirtyFlag = false;
        pendingNamedTexts_ = false;
        namedTextsSettleCount_ = 0;
        pendingNamedTextsWidget_ = nullptr;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgets_[i] = nullptr;
            preAddWidgets_[i] = nullptr;
        }
        preAddWidgetCount_ = 0;
    }

private:
    // Fire a FOCUS event: extract all data from the element NOW (it is
    // alive during Tick), store as FocusEventData, queue the struct.
    // Lua receives a table of strings/bools -- no Noesis objects.
    //
    // Also auto-subscribes INPC on the element's DataContext (Phase 2).
    // When a ViewModel property changes, re-reads DC properties and fires
    // a "PropertyChanged" event through the same callback.
    void FireFocusCallback(Noesis::UIElement* elem)
    {
        if (!elem) return;
        WARN("[BG3Access]   FireFocusCallback: elem=%p", elem);
        auto frameworkElem = static_cast<Noesis::FrameworkElement*>(elem);
        FocusEventData data;
        ExtractElementData(data, frameworkElem);

        // NOTE: namedTexts re-collection on tab switch is deferred, not
        // done here.  IsElementVisible cannot reliably filter cross-tab
        // TextBlocks during the same frame as the selection change (Noesis
        // hasn't updated Visibility states yet).  The deferred collection
        // fires after kWidgetSettleTicks via pendingNamedTexts_ in Tick().

        // Auto-subscribe INPC on the DataContext.
        UnsubscribeINPC();
        if (sDataContextProp) {
            auto depObj = static_cast<Noesis::DependencyObject const*>(frameworkElem);
            auto dcVal = sDataContextProp->GetValue(depObj);
            if (dcVal) {
                auto dataContext = *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
                if (dataContext) {
                    SubscribeINPC(dataContext);
                }
            }
        }

        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                pin->GetDeferredUIEvents().OnFocusChanged(
                    callback_, std::move(data));
            }
        }
    }

    // Fire a WIDGET event: extract data from the widget and push as a
    // data table (no Noesis elements cross to Lua).  Phase 3 replacement
    // for the old OnPropertyChanged path.
    void FireWidgetCallback(Noesis::UIElement* elem)
    {
        if (!elem) return;
        FocusEventData data;
        data.eventType = "WidgetAdded";

        auto frameworkElem = static_cast<Noesis::FrameworkElement*>(elem);
        WARN("[BG3Access]   FireWidgetCallback: elem=%p", elem);
        auto classType = frameworkElem->GetClassType();
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
        if (sDataContextProp) {
            auto depObj = static_cast<Noesis::DependencyObject const*>(frameworkElem);
            auto dcVal = sDataContextProp->GetValue(depObj);
            if (dcVal) {
                auto dataContext = *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
                if (dataContext) {
                    data.dcType = dataContext->GetClassType()->GetName();
                    CollectDCProperties(data, dataContext);

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

        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                pin->GetDeferredUIEvents().OnFocusChanged(
                    callback_, std::move(data));
            }
        }
    }

    // ----- Auto-INPC subscription (Phase 2) -----
    // Subscribes to INotifyPropertyChanged on a DataContext ViewModel.
    // When a property changes, re-reads ALL DC properties and fires a
    // "PropertyChanged" event to Lua through the global focus callback.
    void SubscribeINPC(Noesis::BaseComponent* dataContext)
    {
        auto notifies = Noesis::DynamicCast<
            Noesis::INotifyPropertyChanged*, Noesis::BaseComponent*>(dataContext);
        if (!notifies) return;

        // Unsubscribe from previous target if still alive.
        // No Ptr<> ref -- if the old VM was destroyed, Noesis already
        // cleared its delegate list, so we just reset our pointer.
        if (inpcTarget_ && inpcTarget_ != notifies) {
            inpcTarget_->PropertyChanged().Remove(
                Noesis::MakeDelegate(this, &GlobalFocusMonitor::OnINPCChanged));
        }

        inpcTarget_ = notifies;

        notifies->PropertyChanged().Add(
            Noesis::MakeDelegate(this, &GlobalFocusMonitor::OnINPCChanged));
    }

    void UnsubscribeINPC()
    {
        // Don't call Remove on a potentially dead object.
        // Just clear the pointer.  If the VM is still alive, the
        // delegate fires harmlessly (OnINPCChanged checks callback_).
        // If dead, Noesis already cleaned up its delegate list.
        inpcTarget_ = nullptr;
    }

    void OnINPCChanged(Noesis::BaseComponent* sender,
                       const Noesis::PropertyChangedEventArgs& args)
    {
        if (!callback_) return;

        // Re-read all DC properties from the sender (ViewModel is alive).
        FocusEventData data;
        data.eventType = "PropertyChanged";
        if (sender) {
            data.dcType = sender->GetClassType()->GetName();
            CollectDCProperties(data, sender);
        }

        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                pin->GetDeferredUIEvents().OnFocusChanged(
                    callback_, std::move(data));
            }
        }
    }

    // ----- Widget DC INPC subscription (Phase 3) -----
    // Monitors the WIDGET's DataContext for property changes.  Fires a
    // "WidgetDCChanged" event when the widget ViewModel updates (e.g.,
    // after tab content loads and bindings resolve).  Replaces the fragile
    // Lua WaitFrames timer with event-driven detection.
    void SubscribeWidgetINPC(Noesis::BaseComponent* dataContext,
                             Noesis::FrameworkElement* widgetElem = nullptr)
    {
        // Skip if same DC object already subscribed
        if (widgetInpcTarget_ && widgetInpcTarget_ ==
            Noesis::DynamicCast<Noesis::INotifyPropertyChanged*, Noesis::BaseComponent*>(dataContext))
            return;

        // Unsubscribe from previous target if still alive.
        if (widgetInpcTarget_) {
            widgetInpcTarget_->PropertyChanged().Remove(
                Noesis::MakeDelegate(this, &GlobalFocusMonitor::OnWidgetINPCChanged));
        }

        auto notifies = Noesis::DynamicCast<
            Noesis::INotifyPropertyChanged*, Noesis::BaseComponent*>(dataContext);
        if (!notifies) return;

        widgetInpcTarget_ = notifies;
        widgetInpcWidgetAddr_ = widgetElem;

        notifies->PropertyChanged().Add(
            Noesis::MakeDelegate(this, &GlobalFocusMonitor::OnWidgetINPCChanged));
    }

    void UnsubscribeWidgetINPC()
    {
        // Don't call Remove on a potentially dead object.
        widgetInpcTarget_ = nullptr;
        widgetInpcWidgetAddr_ = nullptr;
    }

    void OnWidgetINPCChanged(Noesis::BaseComponent* sender,
                              const Noesis::PropertyChangedEventArgs& args)
    {
        if (!callback_) return;

        // Re-read ALL DC properties from the widget's ViewModel.
        FocusEventData data;
        data.eventType = "WidgetDCChanged";
        if (sender) {
            data.dcType = sender->GetClassType()->GetName();
            CollectDCProperties(data, sender);
        }

        // Cancel the fallback timer -- WidgetDCChanged is the primary path.
        pendingNamedTexts_ = false;

        // Collect namedTexts from the widget if we have a valid address.
        // Verify against the live widget list before dereferencing.
        if (widgetInpcWidgetAddr_ && widgetContainer_) {
            auto count = widgetContainer_->GetVisualChildrenCount();
            bool found = false;
            for (uint32_t i = 0; i < count; i++) {
                auto child = widgetContainer_->GetVisualChild(i);
                if (child == static_cast<Noesis::Visual*>(widgetInpcWidgetAddr_)) {
                    WARN("[BG3Access]   INPC: collecting namedTexts from widget %p", widgetInpcWidgetAddr_);
                    TryCollectNamedTexts(widgetInpcWidgetAddr_, data.namedTexts);
                    found = true;
                    break;
                }
            }
            if (!found) {
                WARN("[BG3Access]   INPC: widget %p not found in %u children, skipping namedTexts",
                    widgetInpcWidgetAddr_, count);
            }
        } else {
            WARN("[BG3Access]   INPC: no widget addr (%p) or container (%p), skipping namedTexts",
                widgetInpcWidgetAddr_, widgetContainer_);
        }

        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                pin->GetDeferredUIEvents().OnFocusChanged(
                    callback_, std::move(data));
            }
        }
    }

    // Focus/selection tracking.
    // Raw pointers only -- NO Ptr<> refs.  These are compared by value
    // to detect changes, then dereferenced ONLY within the same tick
    // they were obtained (via GetFocusedElement / FindSelectedTabInTree).
    // Deferred paths must re-find elements from the live widget list.
    Noesis::UIElement* lastFocused_ = nullptr;
    Noesis::UIElement* lastSelected_ = nullptr;
    const void* lastSelectedDC_ = nullptr;

    // INPC auto-subscription tracking (focused element DC)
    // Raw pointers only -- no Ptr<> refs.  If the VM is destroyed,
    // Noesis clears its delegate list.  Our stale pointer is harmless.
    Noesis::INotifyPropertyChanged* inpcTarget_ = nullptr;

    // Widget DC INPC subscription tracking (Phase 3)
    Noesis::INotifyPropertyChanged* widgetInpcTarget_ = nullptr;
    Noesis::FrameworkElement* widgetInpcWidgetAddr_ = nullptr;  // raw address, verified before use

    // Widget container tracking (Strategy 4)
    Noesis::Visual* cachedRoot_ = nullptr;
    Noesis::Visual* widgetContainer_ = nullptr;
    uint32_t prevWidgetCount_ = 0;
    Noesis::Visual* prevWidgets_[kMaxWidgets] = {};

    // Pre-addition widget set: saved when a widget addition is first
    // detected, used at fire time to identify which widget is new.
    uint32_t preAddWidgetCount_ = 0;
    Noesis::Visual* preAddWidgets_[kMaxWidgets] = {};

    lua::PersistentRegistryEntry callback_;
    bool forceNext_ = false;
    bool hadFocusBefore_ = false;       // true once any focus/selection was found
    bool pendingWidgetFire_ = false;    // deferred Strategy 4 fire pending
    int widgetSettleCount_ = 0;         // ticks remaining before deferred fire
    int overlayPollCount_ = 0;

    // Strategy 3: event-driven selection detection.
    // When sSelectionChangedEvent is resolved, Strategy 3 only runs when
    // the event fires (dirty flag) or on initial load, instead of every frame.
    bool initialSelectionDone_ = false;

    // Deferred namedTexts re-collection after tab switch.
    // Waits for Noesis to update Visibility states before collecting
    // so IsElementVisible can reliably filter cross-tab TextBlocks.
    bool pendingNamedTexts_ = false;
    int namedTextsSettleCount_ = 0;
    Noesis::Visual* pendingNamedTextsWidget_ = nullptr;  // raw address, verified against live widgets before use

    // Widgets that already have a SelectionChanged handler.
    // Subscribe-only pattern (never Remove) -- track pointers to avoid
    // adding duplicate handlers.  Raw pointers: if a widget is destroyed,
    // its HashMap is torn down (handler gone).  A recycled address just
    // means we skip subscribing (harmless -- the new widget at that
    // address gets subscribed on its next widget set change).
    Noesis::UIElement* subscribedWidgets_[kMaxWidgets] = {};
    uint32_t subscribedWidgetCount_ = 0;

    // Subscribe SelectionChanged on visible widgets.  Skip already-subscribed.
    // Prunes dead entries first: any tracked widget NOT in the current set
    // has been destroyed (its handler was torn down with its HashMap).
    void SubscribeSelectionChangedOnWidgets(Noesis::Visual* const* widgets, uint32_t count)
    {
        if (!sSelectionChangedEvent) return;

        // Prune dead entries: remove tracked widgets not in current set.
        for (uint32_t i = 0; i < subscribedWidgetCount_; ) {
            bool alive = false;
            for (uint32_t j = 0; j < count; j++) {
                if (widgets[j] == static_cast<Noesis::Visual*>(subscribedWidgets_[i])) {
                    alive = true;
                    break;
                }
            }
            if (!alive) {
                // Swap with last and shrink.
                subscribedWidgets_[i] = subscribedWidgets_[subscribedWidgetCount_ - 1];
                subscribedWidgets_[subscribedWidgetCount_ - 1] = nullptr;
                subscribedWidgetCount_--;
            } else {
                i++;
            }
        }

        // Subscribe on new widgets.
        for (uint32_t i = 0; i < count; i++) {
            if (!widgets[i] || !IsVisibleDP(widgets[i])) continue;

            auto uiElement = static_cast<Noesis::UIElement*>(
                const_cast<Noesis::Visual*>(widgets[i]));

            bool alreadySubscribed = false;
            for (uint32_t j = 0; j < subscribedWidgetCount_; j++) {
                if (subscribedWidgets_[j] == uiElement) {
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

            subscribedWidgets_[subscribedWidgetCount_] = uiElement;
            subscribedWidgetCount_++;
            WARN("[BG3Access] Subscribed SelectionChanged on widget %p", uiElement);
        }
    }

    // Clear tracking array.  Handlers stay in the widgets' HashMaps
    // (subscribe-only).  Destroyed widgets clean up their own maps.
    void UnsubscribeAllSelectionChanged()
    {
        for (uint32_t i = 0; i < subscribedWidgetCount_; i++)
            subscribedWidgets_[i] = nullptr;
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
    WARN("[BG3Access] InitFocusProperties: IsFocused=%p FocusedElement=%p IsSelected=%p DataContext=%p IsVisible=%p Visibility=%p ListBoxItemType=%p UIWidgetType=%p DCWidgetType=%p LSMoveFocusIsFocused=%p LSMoveFocusFocusable=%p NameScope=%p",
        sIsFocusedProp, sFocusedElementProp, sIsSelectedProp, sDataContextProp, sIsVisibleProp, sVisibilityProp, sListBoxItemType, sUIWidgetType, sDCWidgetType, sLSMoveFocusIsFocusedProp, sLSMoveFocusFocusableProp, sNameScopeProp);
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
static void TryDiscoverFocusedElementProp(Noesis::Visual* const* widgets, uint32_t count)
{
    if (sFocusedElementProp) return;  // already resolved
    static const Noesis::Symbol sFocusedElementSym("FocusedElement");

    for (uint32_t i = 0; i < count; i++) {
        if (!widgets[i]) continue;
        auto depObj = static_cast<Noesis::DependencyObject const*>(widgets[i]);
        for (auto& prop : depObj->mValues) {
            if (prop.key->GetName() == sFocusedElementSym) {
                sFocusedElementProp = prop.key;
                WARN("[BG3Access] FocusedElement discovered from widget mValues: %p", sFocusedElementProp);
                return;
            }
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
    auto val = sIsVisibleProp->GetValue(
        static_cast<Noesis::DependencyObject const*>(elem));
    return !val || *static_cast<const bool*>(val);
}

// Check if an element's class derives from ls.UIWidget.
static bool IsUIWidgetType(Noesis::Visual const* elem)
{
    auto cls = elem->GetClassType();
    while (cls) {
        if (sUIWidgetType && cls == sUIWidgetType) return true;
        if (sDCWidgetType && cls == sDCWidgetType) return true;
        cls = cls->GetBase();
    }
    return false;
}

// Find the widget container: drill down from root following first children
// until we find an element whose children are UIWidgets.
// The BG3 tree structure is: UICanvas -> Viewbox -> Decorator -> Grid -> UIWidget(s)
// but this search is dynamic and doesn't hardcode the depth.
static Noesis::Visual* FindWidgetContainer(Noesis::Visual* root)
{
    if (!sUIWidgetType) return nullptr;
    Noesis::Visual* cur = root;
    for (int depth = 0; depth < 10 && cur; depth++) {
        auto count = cur->GetVisualChildrenCount();
        if (count == 0) return nullptr;
        auto firstChild = cur->GetVisualChild(0);
        if (firstChild && IsUIWidgetType(firstChild))
            return cur;
        cur = firstChild;
    }
    return nullptr;
}

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

    auto count = elem->GetVisualChildrenCount();
    for (int i = (int)count - 1; i >= 0; i--) {
        auto child = elem->GetVisualChild(i);
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
    auto cls = elem->GetClassType();
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

    // Prune invisible branches.
    if (!IsVisibleDP(elem)) return nullptr;

    auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
    auto val = sIsSelectedProp->GetValue(depObj);
    if (val && *static_cast<const bool*>(val)) {
        if (IsListBoxItemType(elem)) {
            return static_cast<Noesis::UIElement*>(const_cast<Noesis::Visual*>(elem));
        }
    }

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
    auto root = GetRoot();
    if (!root) return nullptr;

    InitFocusProperties(root);
    auto container = FindWidgetContainer(root);
    if (!container) return nullptr;

    auto count = container->GetVisualChildrenCount();
    for (int i = (int)count - 1; i >= 0; i--) {
        auto child = container->GetVisualChild(i);
        if (!child) continue;
        if (IsUIWidgetType(child) && IsVisibleDP(child)) {
            return static_cast<Noesis::UIElement*>(child);
        }
    }
    return nullptr;
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
    auto root = GetRoot();
    if (!root) return nullptr;

    InitFocusProperties(root);
    auto container = FindWidgetContainer(root);
    if (!container) return nullptr;

    auto widgetCount = container->GetVisualChildrenCount();

    // Try each visible widget in reverse Z-order (topmost first).
    for (int wi = (int)widgetCount - 1; wi >= 0; wi--) {
        auto widget = container->GetVisualChild(wi);
        if (!widget || !IsUIWidgetType(widget) || !IsVisibleDP(widget))
            continue;

        // Walk down the first-child chain to enter the NameScope.
        // NameScope owner is typically 2 levels below the UIWidget.
        // Try FindNodeName at each level; first success wins.
        Noesis::Visual* cur = widget;
        for (int depth = 0; depth < 5; depth++) {
            auto childCount = cur->GetVisualChildrenCount();
            if (childCount == 0) break;

            auto child = cur->GetVisualChild(0);
            if (!child) break;

            // All visual tree nodes in Noesis UI are FrameworkElements
            // (Grid, Border, ContentPresenter, etc.).  static_cast is
            // safe here -- same pattern as FindSelectedTabInTree.
            auto childFE = static_cast<Noesis::FrameworkElement*>(child);
            auto found = Noesis::FrameworkElementHelpers::FindNodeName(childFE, name);
            if (found) {
                // Named XAML elements (x:Name) are always FrameworkElements.
                return static_cast<Noesis::FrameworkElement*>(found);
            }

            cur = child;
        }
    }

    return nullptr;
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
Noesis::FrameworkElement* FindNameInWidgetScoped(char const* name,
                                                  Noesis::Visual* widget)
{
    if (!name || !widget) return nullptr;

    // Check the widget itself first.
    auto widgetFE = static_cast<Noesis::FrameworkElement*>(widget);
    auto found = Noesis::FrameworkElementHelpers::FindNodeName(widgetFE, name);
    if (found) return static_cast<Noesis::FrameworkElement*>(found);

    // BFS through visual children, max 5 levels deep, max 256 nodes.
    // NameScope owners are typically 2-5 levels below the UIWidget root.
    Noesis::Visual* queue[256];
    int front = 0, back = 0;

    // Seed with widget's direct children (level 1).
    auto seedCount = widget->GetVisualChildrenCount();
    for (uint32_t i = 0; i < seedCount && back < 256; i++) {
        auto child = widget->GetVisualChild(i);
        if (child) queue[back++] = child;
    }

    // Process up to 5 levels of children.
    for (int level = 0; level < 5 && front < back; level++) {
        int levelEnd = back;
        while (front < levelEnd) {
            auto cur = queue[front++];
            auto curFE = static_cast<Noesis::FrameworkElement*>(cur);
            found = Noesis::FrameworkElementHelpers::FindNodeName(curFE, name);
            if (found) return static_cast<Noesis::FrameworkElement*>(found);

            // Enqueue children for the next level.
            auto cc = cur->GetVisualChildrenCount();
            for (uint32_t i = 0; i < cc && back < 256; i++) {
                auto child = cur->GetVisualChild(i);
                if (child) queue[back++] = child;
            }
        }
    }

    return nullptr;
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
    auto root = GetRoot();
    if (!root) return nullptr;

    InitFocusProperties(root);

    // Strategy 1: FocusManager.FocusedElement fast path
    auto focused = TryFocusManager(root, GlobalFocusMonitor::kMaxTreeDepth);
    if (focused) return focused;

    // Strategy 2: IsFocused + ls:MoveFocus.IsFocused tree walk (safety net)
    if (sIsFocusedProp || sLSMoveFocusIsFocusedProp) {
        focused = FindFocusedInTree(root, GlobalFocusMonitor::kMaxTreeDepth);
        if (focused) return focused;
    }

    // Strategy 3: IsSelected tree walk (for ListBoxItem-derived tab items).
    // Catches selected tabs that don't have keyboard focus.
    if (sIsSelectedProp && sListBoxItemType) {
        auto focused = FindSelectedTabInTree(root, GlobalFocusMonitor::kMaxTreeDepth);
        if (focused) return focused;
    }

    return nullptr;
}

bg3se::ui::UIStateMachine* GetStateMachine()
{
    Noesis::gStaticSymbols.Initialize();
    return nullptr; // FIXME - not handled yet!
    // return (*GetStaticSymbols().ls__gGlobalResourceManager)->UIManager->field_3B8.StateMachine;
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

bool SubscribePropertyChanged(lua_State* L, Noesis::BaseComponent* target, lua::RegistryEntry callback)
{
    return INPCMonitor::Instance().Subscribe(L, target, std::move(callback));
}

void UnsubscribePropertyChanged()
{
    INPCMonitor::Instance().Unsubscribe();
}

bool SubscribeDPChanged(lua_State* L, Noesis::DependencyObject* target, lua::RegistryEntry callback)
{
    return DPMonitor::Instance().Subscribe(L, target, std::move(callback));
}

void UnsubscribeDPChanged()
{
    DPMonitor::Instance().Unsubscribe();
}

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
    auto const& cls = gClassCache.GetClass(o->GetClassType());
    return cls.Names.try_get(name) != nullptr;
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
    if (!target) return false;
    // static_cast is safe: Lua only passes UI elements which are all
    // DependencyObject subclasses.  DynamicCast<DependencyObject*> requires
    // DependencyObject::StaticGetClassType which is not exported.
    auto depObj = static_cast<Noesis::DependencyObject*>(target);
    if (!depObj) return false;

    Noesis::Symbol sym(propName.GetString());
    for (auto& prop : depObj->mValues) {
        if (prop.key && prop.key->GetName() == sym) {
            return true;
        }
    }
    return false;
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
    if (!sVisibilityProp) {
        // Fallback: if Visibility DP not resolved, try IsVisible DP.
        if (!sIsVisibleProp) return true;
        auto depObj = static_cast<Noesis::DependencyObject*>(target);
        if (!depObj) return true;
        auto val = sIsVisibleProp->GetValue(depObj);
        return !val || *static_cast<const bool*>(val);
    }

    // Type-check: target must be a FrameworkElement to walk mParent.
    auto cls = target->GetClassType();
    if (!Noesis::TypeHelpers::IsDescendantOf(
            cls, Noesis::gStaticSymbols.TypeClasses.FrameworkElement.Type)) {
        return true;  // Not a FrameworkElement -- can't check ancestors.
    }

    // Walk element + logical ancestors, checking effective Visibility.
    auto ancestor = static_cast<Noesis::FrameworkElement*>(target);
    constexpr int kMaxDepth = 32;  // logical chains are short
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
    if (!sIsHitTestVisibleProp) return true;
    // static_cast -- same rationale as HasLocalValue above.
    auto depObj = static_cast<Noesis::DependencyObject*>(target);
    if (!depObj) return true;
    auto val = sIsHitTestVisibleProp->GetValue(depObj);
    return !val || *static_cast<const bool*>(val);
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
    if (!target) return nullptr;

    // Ensure sDataContextProp is initialized.
    auto root = GetRoot();
    if (root) InitFocusProperties(root);
    if (!sDataContextProp) return nullptr;

    auto depObj = static_cast<Noesis::DependencyObject*>(target);
    if (!depObj) return nullptr;

    auto dcVal = sDataContextProp->GetValue(depObj);
    if (!dcVal) return nullptr;

    return *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
}

// Free functions wrapping GlobalFocusMonitor singleton -- called from
// LuaClient.cpp which is a different translation unit.
void TickGlobalFocusMonitor()
{
    GlobalFocusMonitor::Instance().Tick();
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
    if (!sLSMoveFocusFocusableProp) return false;
    auto depObj = static_cast<Noesis::DependencyObject*>(target);
    if (!depObj) return false;
    auto val = sLSMoveFocusFocusableProp->GetValue(depObj);
    return val && *static_cast<const bool*>(val);
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
        auto value = reinterpret_cast<Noesis::String const*>(prop->Get(obj));
        if (value && value->Size() > 0) return value->Str();
    } else if (type == types.CStringPtr.Type) {
        char const* value = nullptr;
        prop->GetCopy(obj, &value);
        if (value && value[0] != '\0') return value;
    } else if (type == types.LocaString.Type) {
        auto ts = reinterpret_cast<TranslatedString const*>(prop->Get(obj));
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
        prop->GetCopy(obj, &value);
        return value ? "On" : "Off";
    } else if (type == types.Int32.Type) {
        int32_t value = 0;
        prop->GetCopy(obj, &value);
        return std::to_string(value);
    } else if (type == types.UInt32.Type) {
        uint32_t value = 0;
        prop->GetCopy(obj, &value);
        return std::to_string(value);
    } else if (type == types.Int64.Type) {
        int64_t value = 0;
        prop->GetCopy(obj, &value);
        return std::to_string(value);
    } else if (type == types.Single.Type) {
        float value = 0;
        prop->GetCopy(obj, &value);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", value);
        return buf;
    } else if (type == types.Double.Type) {
        double value = 0;
        prop->GetCopy(obj, &value);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", value);
        return buf;
    }
    // Pointer / Ptr / object / enum types -- not convertible to string here.
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
    if (sDataContextProp) {
        auto depObj = static_cast<Noesis::DependencyObject const*>(widgetElem);
        auto dcVal = sDataContextProp->GetValue(depObj);
        if (dcVal) {
            auto dataContext = *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
            if (dataContext) {
                auto dcTypeName = dataContext->GetClassType()->GetName();
                if (IsOverlayDCType(dcTypeName)) {
                    WARN("[BG3Access]   NameScope: skipping overlay widget DC=%s", dcTypeName);
                    return;
                }
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
                WARN("[BG3Access]   NameScope at depth %d: %p (%u entries)",
                    depth, candidate, candidateSize);
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
        current = child;
    }

    if (!nameScope) return;

    WARN("[BG3Access]   NameScope: iterating %u entries",
        nameScope->mNamedObjects.Size());

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
        Noesis::Visual* queue[256];
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
                auto typeName = cur->GetClassType()->GetName();
                if (strstr(typeName, "TextBlock")) {
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

    auto const& cls = Noesis::gClassCache.GetClass(dc->GetClassType());
    auto& types = Noesis::gStaticSymbols.Types;

    for (auto& entry : cls.Names) {
        auto propInfo = &entry.Value();
        if (!propInfo->Property) continue;

        auto type = UnwrapType(propInfo->Property->GetContentType());
        if (!type) continue;

        auto typeOfType = type->GetClassType();

        // Scalar types: read as string
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
                    const_cast<void*>(propInfo->Property->Get(dc)));
                if (ptrVal) subObj = static_cast<Noesis::BaseObject*>(ptrVal->GetPtr());
            } else if (typeOfType == types.TypePointer.Type) {
                propInfo->Property->GetCopy(dc, &subObj);
            } else {
                subObj = reinterpret_cast<Noesis::BaseObject*>(
                    const_cast<void*>(propInfo->Property->Get(dc)));
            }

            if (subObj) {
                auto const& subCls = Noesis::gClassCache.GetClass(subObj->GetClassType());
                FocusEventData::SubObject subData;
                subData.propName = entry.Key().GetString();
                subData.typeName = subObj->GetClassType()->GetName();

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

        // Get the expression from StoredValue::ComplexValue::expression.
        // No API calls needed -- direct member access.
        auto expression = storedVal->value.complex->expression.GetPtr();
        if (!expression) continue;

        // static_cast to BaseBindingExpression to access mBinding.
        // Safe: isExpression flag guarantees this is a binding expression.
        auto bindingExpr = static_cast<Noesis::BaseBindingExpression*>(expression);
        auto baseBinding = bindingExpr->mBinding.GetPtr();
        if (!baseBinding) continue;

        // static_cast to Binding (concrete type with mPath).
        // BG3 XAML uses standard {Binding}, not MultiBinding.
        auto binding = static_cast<Noesis::Binding*>(baseBinding);
        auto propertyPath = binding->mPath.GetPtr();
        if (!propertyPath) continue;

        auto& bindingPathStr = propertyPath->mPath;
        if (bindingPathStr.Size() == 0) continue;

        // Read the resolved value from the cached binding result.
        auto resolvedValue = ReadDepPropertyAsString(depObj, depProp);
        if (resolvedValue.find("[ForceUpdate]") != std::string::npos) continue;

        auto depPropName = depProp->GetName().Str();
        if (!depPropName) continue;

        FocusEventData::BindingInfo info;
        info.propertyName = depPropName;
        info.bindingPath = std::string(bindingPathStr.Str());
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
    out.elemType = elem->GetClassType()->GetName();

    // elemName
    out.elemName = ReadPropertyAsString(elem, "Name");

    // isTab
    out.isTab = IsListBoxItemType(elem);

    // isFocusable
    if (sLSMoveFocusFocusableProp) {
        auto depObj = static_cast<Noesis::DependencyObject*>(elem);
        auto val = sLSMoveFocusFocusableProp->GetValue(depObj);
        out.isFocusable = val && *static_cast<const bool*>(val);
    }

    // DataContext
    Noesis::BaseComponent* dataContext = nullptr;
    if (sDataContextProp) {
        auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
        auto dcVal = sDataContextProp->GetValue(depObj);
        if (dcVal) {
            dataContext = *reinterpret_cast<Noesis::BaseComponent* const*>(dcVal);
            if (dataContext) {
                out.dcType = dataContext->GetClassType()->GetName();
            }
        }
    }

    // DC properties
    if (dataContext) {
        CollectDCProperties(out, dataContext);
    }

    // elemText: direct property reads only.  No tree walking.
    // For bound Content (buttons), elemText stays empty -- Lua falls back
    // to cleaning up elemName ("NewGameButton" -> "New Game").
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
    MODULE_FUNCTION(SubscribePropertyChanged)
    MODULE_FUNCTION(UnsubscribePropertyChanged)
    MODULE_FUNCTION(SubscribeDPChanged)
    MODULE_FUNCTION(UnsubscribeDPChanged)
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
