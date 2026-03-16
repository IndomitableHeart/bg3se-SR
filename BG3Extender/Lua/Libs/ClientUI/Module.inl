#include <Lua/Libs/ClientUI/Builtins.inl>
#include <Lua/Libs/ClientUI/NsHelpers.inl>
#include <Lua/Libs/ClientUI/CustomProperties.inl>
#include <NsGui/UIElementCollection.h>
#include <NsGui/IList.h>
#include <NsGui/INotifyPropertyChanged.h>
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
        targetRef_ = Noesis::Ptr<Noesis::BaseComponent>(target);
        callback.Push(L);
        callback_ = lua::PersistentRegistryEntry(L, -1);
        lua_pop(L, 1);

        notifies->PropertyChanged().Add(
            Noesis::MakeDelegate(this, &INPCMonitor::OnChanged));
        return true;
    }

    void Unsubscribe()
    {
        if (target_) {
            target_->PropertyChanged().Remove(
                Noesis::MakeDelegate(this, &INPCMonitor::OnChanged));
            target_ = nullptr;
            targetRef_.Reset();
            callback_ = {};
        }
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
    Noesis::Ptr<Noesis::BaseComponent> targetRef_;
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
        targetRef_ = Noesis::Ptr<Noesis::DependencyObject>(target);
        callback.Push(L);
        callback_ = lua::PersistentRegistryEntry(L, -1);
        lua_pop(L, 1);

        target->mDependencyPropertyChangedEvent.Add(
            Noesis::MakeDelegate(this, &DPMonitor::OnChanged));
        return true;
    }

    void Unsubscribe()
    {
        if (target_) {
            target_->mDependencyPropertyChangedEvent.Remove(
                Noesis::MakeDelegate(this, &DPMonitor::OnChanged));
            target_ = nullptr;
            targetRef_.Reset();
            callback_ = {};
        }
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
    Noesis::Ptr<Noesis::DependencyObject> targetRef_;
    lua::PersistentRegistryEntry callback_;
};

// Forward declarations for focus detection.
static const Noesis::DependencyProperty* sIsFocusedProp = nullptr;
static const Noesis::DependencyProperty* sFocusedElementProp = nullptr;
static const Noesis::DependencyProperty* sIsSelectedProp = nullptr;
static const Noesis::DependencyProperty* sDataContextProp = nullptr;
static const Noesis::DependencyProperty* sIsVisibleProp = nullptr;
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
static const Noesis::TypeClass* sListBoxItemType = nullptr;
static const Noesis::TypeClass* sUIWidgetType = nullptr;
static const Noesis::TypeClass* sDCWidgetType = nullptr;
void InitFocusProperties(Noesis::FrameworkElement* root);
Noesis::UIElement* TryFocusManager(Noesis::Visual* elem, int depth,
                                    Noesis::DependencyObject** outScopeRoot = nullptr);
Noesis::UIElement* FindFocusedInTree(Noesis::Visual* elem, int depth);
Noesis::UIElement* FindSelectedTabInTree(Noesis::Visual* elem, int depth);
static bool IsElementVisible(Noesis::Visual const* elem);
static bool IsUIWidgetType(Noesis::Visual const* elem);
static Noesis::Visual* FindWidgetContainer(Noesis::Visual* root);

// Forward declarations -- defined below, after InitFocusProperties.
Noesis::UIElement* GetFocusedElement();
static void TryDiscoverFocusedElementProp(Noesis::Visual* const* widgets, uint32_t count);

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
        lastFocusedRef_.Reset();
        lastSelected_ = nullptr;
        lastSelectedRef_.Reset();
        lastSelectedDC_ = nullptr;
        cachedRoot_ = nullptr;
        widgetContainer_ = nullptr;
        prevWidgetCount_ = 0;
        hadFocusBefore_ = false;
        pendingWidgetFire_ = false;
        widgetSettleCount_ = 0;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgets_[i] = nullptr;
            prevWidgetRefs_[i].Reset();
            preAddWidgets_[i] = nullptr;
            preAddWidgetRefs_[i].Reset();
        }
        preAddWidgetCount_ = 0;
        return true;
    }

    void Unsubscribe()
    {
        callback_ = {};
        lastFocused_ = nullptr;
        lastFocusedRef_.Reset();
        lastSelected_ = nullptr;
        lastSelectedRef_.Reset();
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
                if (!widgets[i] || !IsElementVisible(widgets[i])) continue;
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
                if (!widgets[i] || !IsElementVisible(widgets[i])) continue;
                focused = FindFocusedInTree(widgets[i], kMaxTreeDepth);
                if (focused) {
                    scopeRoot = static_cast<Noesis::DependencyObject*>(widgets[i]);
                    break;
                }
            }
        }

        // ----- Strategy 3: IsSelected (scoped, then per-widget) -----
        // If we have a focus scope root, search only that widget to
        // exclude stale IsSelected items in other panels.
        // If no scope root (Strategy 1 unresolved), search ALL visible
        // widgets in reverse Z-order until we find one with IsSelected.
        Noesis::UIElement* selected = nullptr;
        if (sIsSelectedProp && sListBoxItemType) {
            if (scopeRoot) {
                selected = FindSelectedTabInTree(
                    static_cast<Noesis::Visual*>(scopeRoot), kMaxTreeDepth);
            } else if (widgetCount > 0) {
                for (int i = (int)widgetCount - 1; i >= 0; i--) {
                    if (!widgets[i] || !IsElementVisible(widgets[i])) continue;
                    selected = FindSelectedTabInTree(widgets[i], kMaxTreeDepth);
                    if (selected) break;
                }
            }
            if (!selected) {
                selected = FindSelectedTabInTree(root, kMaxTreeDepth);
            }
        }

        // ----- DataContext for recycling detection -----
        const void* selectedDC = nullptr;
        if (selected && sDataContextProp) {
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
        // Selection change is based on DataContext (ViewModel), not the
        // element pointer.  Carousel recycling changes the DC on the same
        // element (one fire).  Widget rebuild creates a new element for the
        // same DC (suppressed -- redundant, Lua already processed this DC).
        // Also fire when selection appears/disappears (null transitions).
        bool selectionChanged = (selectedDC != lastSelectedDC_)
                             || (selected && !lastSelected_)
                             || (!selected && lastSelected_);

        // ----- Strategy 4: Widget set change detection -----
        // Save previous widget set to locals BEFORE updating, so we can
        // identify which widgets are new (added) at fire time.
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

        // Update cached widget set (hold Ptr<> refs to keep them alive).
        prevWidgetCount_ = widgetCount;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgets_[i] = (i < widgetCount) ? widgets[i] : nullptr;
            prevWidgetRefs_[i] = (i < widgetCount && widgets[i])
                ? Noesis::Ptr<Noesis::Visual>(widgets[i])
                : Noesis::Ptr<Noesis::Visual>();
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
            lastFocusedRef_ = focused
                ? Noesis::Ptr<Noesis::UIElement>(focused)
                : Noesis::Ptr<Noesis::UIElement>();
        }

        if (selectionChanged) {
            lastSelected_ = selected;
            lastSelectedRef_ = selected
                ? Noesis::Ptr<Noesis::UIElement>(selected)
                : Noesis::Ptr<Noesis::UIElement>();
            lastSelectedDC_ = selectedDC;
        }

        // ----- Fire callbacks (priority: selection > focus > forced) -----
        if (selectionChanged && selected) {
            WARN("[BG3Access]   -> FIRE selection (tab)");
            FireCallback(selected, Noesis::Symbol("FocusedElement"));
        } else if (focusChanged && focused) {
            WARN("[BG3Access]   -> FIRE focus");
            FireCallback(focused, Noesis::Symbol("FocusedElement"));
        } else if (forced) {
            auto best = selected ? selected : focused;
            if (best) {
                WARN("[BG3Access]   -> FIRE forced");
                FireCallback(best, Noesis::Symbol("FocusedElement"));
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
                    preAddWidgetRefs_[i] = (i < oldWidgetCount && oldWidgets[i])
                        ? Noesis::Ptr<Noesis::Visual>(oldWidgets[i])
                        : Noesis::Ptr<Noesis::Visual>();
                }
            }
        }

        if (pendingWidgetFire_) {
            widgetSettleCount_--;
            if (widgetSettleCount_ <= 0) {
                pendingWidgetFire_ = false;
                // Fire with the NEWEST widget (highest index = last added).
                bool fired = false;
                for (int i = (int)widgetCount - 1; i >= 0; i--) {
                    if (!widgets[i] || !IsElementVisible(widgets[i])) continue;
                    // Check if this widget is actually new (not in pre-addition set)
                    bool isNew = true;
                    for (uint32_t j = 0; j < preAddWidgetCount_; j++) {
                        if (widgets[i] == preAddWidgets_[j]) { isNew = false; break; }
                    }
                    if (isNew) {
                        WARN("[BG3Access]   -> FIRE widgetAdded (widget[%d])", i);
                        FireCallback(
                            static_cast<Noesis::UIElement*>(
                                const_cast<Noesis::Visual*>(widgets[i])),
                            Noesis::Symbol("WidgetAdded"));
                        fired = true;
                        break;
                    }
                }
                if (!fired) {
                    // All pointers swapped -- fire topmost visible as fallback.
                    for (int i = (int)widgetCount - 1; i >= 0; i--) {
                        if (!widgets[i] || !IsElementVisible(widgets[i])) continue;
                        WARN("[BG3Access]   -> FIRE widgetAdded fallback (widget[%d])", i);
                        FireCallback(
                            static_cast<Noesis::UIElement*>(
                                const_cast<Noesis::Visual*>(widgets[i])),
                            Noesis::Symbol("WidgetAdded"));
                        break;
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
        lastFocused_ = nullptr;
        lastFocusedRef_.Reset();
        lastSelected_ = nullptr;
        lastSelectedRef_.Reset();
        lastSelectedDC_ = nullptr;
        cachedRoot_ = nullptr;
        widgetContainer_ = nullptr;
        prevWidgetCount_ = 0;
        hadFocusBefore_ = false;
        pendingWidgetFire_ = false;
        widgetSettleCount_ = 0;
        for (uint32_t i = 0; i < kMaxWidgets; i++) {
            prevWidgets_[i] = nullptr;
            prevWidgetRefs_[i].Reset();
            preAddWidgets_[i] = nullptr;
            preAddWidgetRefs_[i].Reset();
        }
        preAddWidgetCount_ = 0;
    }

private:
    void FireCallback(Noesis::UIElement* elem, Noesis::Symbol propName)
    {
        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                pin->GetDeferredUIEvents().OnPropertyChanged(
                    callback_, elem, propName);
            }
        }
    }

    // Focus/selection tracking
    Noesis::UIElement* lastFocused_ = nullptr;
    Noesis::Ptr<Noesis::UIElement> lastFocusedRef_;
    Noesis::UIElement* lastSelected_ = nullptr;
    Noesis::Ptr<Noesis::UIElement> lastSelectedRef_;
    const void* lastSelectedDC_ = nullptr;

    // Widget container tracking (Strategy 4)
    Noesis::Visual* cachedRoot_ = nullptr;
    Noesis::Visual* widgetContainer_ = nullptr;
    uint32_t prevWidgetCount_ = 0;
    Noesis::Visual* prevWidgets_[kMaxWidgets] = {};
    Noesis::Ptr<Noesis::Visual> prevWidgetRefs_[kMaxWidgets];

    // Pre-addition widget set: saved when a widget addition is first
    // detected, used at fire time to identify which widget is new.
    uint32_t preAddWidgetCount_ = 0;
    Noesis::Visual* preAddWidgets_[kMaxWidgets] = {};
    Noesis::Ptr<Noesis::Visual> preAddWidgetRefs_[kMaxWidgets];

    lua::PersistentRegistryEntry callback_;
    bool forceNext_ = false;
    bool hadFocusBefore_ = false;       // true once any focus/selection was found
    bool pendingWidgetFire_ = false;    // deferred Strategy 4 fire pending
    int widgetSettleCount_ = 0;         // ticks remaining before deferred fire
    int overlayPollCount_ = 0;
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

    // IsVisible -- for overlay detection (static member not exported in Indie SDK).
    sIsVisibleProp = Noesis::TypeHelpers::GetDependencyProperty(
        root->GetClassType(), bg3se::FixedString("IsVisible"));
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

    // Diagnostic: show what resolved.
    WARN("[BG3Access] InitFocusProperties: IsFocused=%p FocusedElement=%p IsSelected=%p DataContext=%p IsVisible=%p ListBoxItemType=%p UIWidgetType=%p DCWidgetType=%p LSMoveFocusIsFocused=%p LSMoveFocusFocusable=%p",
        sIsFocusedProp, sFocusedElementProp, sIsSelectedProp, sDataContextProp, sIsVisibleProp, sListBoxItemType, sUIWidgetType, sDCWidgetType, sLSMoveFocusIsFocusedProp, sLSMoveFocusFocusableProp);
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
static bool IsElementVisible(Noesis::Visual const* elem)
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
    if (!IsElementVisible(elem)) return nullptr;

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
    if (!IsElementVisible(elem)) return nullptr;

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
        if (IsUIWidgetType(child) && IsElementVisible(child)) {
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
        if (!widget || !IsUIWidgetType(widget) || !IsElementVisible(widget))
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

// Returns the computed IsHitTestVisible state via DependencyProperty
// value lookup.  Same pattern as IsElementVisible -- avoids calling
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
    MODULE_FUNCTION(IsHitTestVisible)
    MODULE_FUNCTION(IsMoveFocusFocusable)
    MODULE_FUNCTION(GetDataContext)
    MODULE_FUNCTION(GetFocusedElement)
    MODULE_FUNCTION(GetTopmostWidget)
    MODULE_FUNCTION(FindNameInWidget)
    END_MODULE()
}

END_NS()
