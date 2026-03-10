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
// DependencyPropertyChanged.  Singleton pattern — at most one active
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

// Forward declarations for focus detection.
static const Noesis::DependencyProperty* sIsFocusedProp = nullptr;
static const Noesis::DependencyProperty* sFocusedElementProp = nullptr;
static const Noesis::DependencyProperty* sIsSelectedProp = nullptr;
static const Noesis::DependencyProperty* sDataContextProp = nullptr;
static const Noesis::TypeClass* sListBoxItemType = nullptr;
void InitFocusProperties(Noesis::FrameworkElement* root);
Noesis::UIElement* TryFocusManager(Noesis::Visual* elem, int depth,
                                    Noesis::DependencyObject** outScopeRoot = nullptr);
Noesis::UIElement* FindFocusedInTree(Noesis::Visual* elem, int depth);
Noesis::UIElement* FindSelectedTabInTree(Noesis::Visual* elem, int depth);

// Forward declaration — defined below, after InitFocusProperties.
Noesis::UIElement* GetFocusedElement();

// GlobalFocusMonitor: per-frame focus/selection change detector.
// Runs Strategies 1+2 (focus) and Strategy 3 (selection) independently.
// Tracks focus and selection SEPARATELY so neither suppresses the other.
// Selection changes take priority (tab switch); when the tab is stable,
// focus changes drive the callback (d-pad navigation, button focus).
//
// RECYCLING DETECTION: Noesis carousels virtualise ListBoxItems — the
// same element object gets reused with swapped DataContext when the user
// presses RB/LB.  Pointer comparison alone would miss the change.
// We also compare the selected element's DataContext pointer each frame.
//
// No routed event subscriptions — zero HashMap corruption risk.
class GlobalFocusMonitor
{
public:
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

        // Strategies 1+2: focus detection.
        // Also capture the focus scope root for Strategy 3 scoping.
        Noesis::UIElement* focused = nullptr;
        Noesis::DependencyObject* scopeRoot = nullptr;
        if (sFocusedElementProp) {
            focused = TryFocusManager(root, 20, &scopeRoot);
        }
        if (!focused && sIsFocusedProp) {
            focused = FindFocusedInTree(root, 20);
        }

        // Strategy 3: tab/selection detection — scoped to the current
        // focus scope.  Searching from scopeRoot (not the global root)
        // excludes stale IsSelected items in other panels (e.g., main
        // menu ListBoxItems behind the Options overlay).
        // Only matches ListBoxItem-derived types (carousel tabs).
        Noesis::UIElement* selected = nullptr;
        if (sIsSelectedProp && sListBoxItemType) {
            auto searchRoot = scopeRoot
                ? static_cast<Noesis::Visual*>(scopeRoot)
                : static_cast<Noesis::Visual*>(root);
            selected = FindSelectedTabInTree(searchRoot, 20);
        }

        // Read the selected element's DataContext pointer.
        // Carousels recycle ListBoxItem containers — the same element
        // pointer is reused with a different DataContext when the user
        // presses RB/LB.  Comparing only the element pointer would miss
        // the tab switch entirely.
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

        // Track focus and selection independently — always update both.
        bool focusChanged = (focused != lastFocused_);
        // Selection changed if EITHER the element pointer OR its
        // DataContext changed (handles virtualised/recycled items).
        bool selectionChanged = (selected != lastSelected_)
                             || (selectedDC != lastSelectedDC_);

        // ---------------------------------------------------------------
        // DIAGNOSTIC LOGGING — fires only when something changes.
        // Shows all key values so we can see exactly what each strategy
        // found and why we fire (or don't fire) the callback.
        // ---------------------------------------------------------------
        if (focusChanged || selectionChanged || forced) {
            WARN("[BG3Access] Tick: focused=%p scopeRoot=%p selected=%p selDC=%p prevSelDC=%p focChg=%d selChg=%d forced=%d",
                focused, scopeRoot, selected, selectedDC, lastSelectedDC_, focusChanged, selectionChanged, forced);
            if (selected) {
                // Log the type of the selected element so we can verify it's a ListBoxItem.
                auto selType = selected->GetClassType();
                auto selTypeId = selType ? selType->GetTypeId() : Noesis::Symbol::Null();
                WARN("[BG3Access]   selected type=%s", selTypeId.Str() ? selTypeId.Str() : "(null)");
            }
            if (!selected && (sIsSelectedProp && sListBoxItemType)) {
                WARN("[BG3Access]   FindSelectedTabInTree returned NULL (scopeRoot=%p root=%p searched=%s)",
                    scopeRoot, root, scopeRoot ? "scopeRoot" : "root");
            }
        }

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

        // Fire callback: selection changes take priority (tab switch via
        // carousel's IsSelected).  When the tab is stable, focus changes
        // drive the callback (d-pad option navigation, button focus).
        if (selectionChanged && selected) {
            WARN("[BG3Access]   -> FIRE selection (tab)");
            FireCallback(selected);
        } else if (focusChanged && focused) {
            WARN("[BG3Access]   -> FIRE focus");
            FireCallback(focused);
        } else if (forced) {
            auto best = selected ? selected : focused;
            if (best) {
                WARN("[BG3Access]   -> FIRE forced");
                FireCallback(best);
            }
        }
    }

    void Reset()
    {
        lastFocused_ = nullptr;
        lastFocusedRef_.Reset();
        lastSelected_ = nullptr;
        lastSelectedRef_.Reset();
        lastSelectedDC_ = nullptr;
    }

private:
    void FireCallback(Noesis::UIElement* elem)
    {
        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                pin->GetDeferredUIEvents().OnPropertyChanged(
                    callback_, elem, Noesis::Symbol("FocusedElement"));
            }
        }
    }

    Noesis::UIElement* lastFocused_ = nullptr;
    Noesis::Ptr<Noesis::UIElement> lastFocusedRef_;
    Noesis::UIElement* lastSelected_ = nullptr;
    Noesis::Ptr<Noesis::UIElement> lastSelectedRef_;
    const void* lastSelectedDC_ = nullptr;    // DataContext ptr — detects recycling
    lua::PersistentRegistryEntry callback_;
    bool forceNext_ = false;
};

// ---------------------------------------------------------------------------
// GetFocusedElement: multi-strategy focus detection.
// ---------------------------------------------------------------------------
static bool sFocusPropsInitialized = false;

void InitFocusProperties(Noesis::FrameworkElement* root)
{
    if (sFocusPropsInitialized) return;
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

    // DataContext — used to detect ListBoxItem recycling (carousel
    // virtualisation reuses the same element with swapped data).
    sDataContextProp = Noesis::TypeHelpers::GetDependencyProperty(
        root->GetClassType(), bg3se::FixedString("DataContext"));

    // Diagnostic: show what resolved.
    WARN("[BG3Access] InitFocusProperties: IsFocused=%p FocusedElement=%p IsSelected=%p DataContext=%p ListBoxItemType=%p",
        sIsFocusedProp, sFocusedElementProp, sIsSelectedProp, sDataContextProp, sListBoxItemType);
}

// Strategy 1: Walk tree checking FocusManager.FocusedElement.
Noesis::UIElement* TryFocusManager(Noesis::Visual* elem, int depth,
                                    Noesis::DependencyObject** outScopeRoot)
{
    if (!elem || depth <= 0) return nullptr;

    auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
    auto val = sFocusedElementProp->GetValue(depObj);
    if (val) {
        auto focused = *reinterpret_cast<Noesis::UIElement* const*>(val);
        if (focused) {
            if (outScopeRoot) {
                *outScopeRoot = const_cast<Noesis::DependencyObject*>(depObj);
            }
            return focused;
        }
    }

    auto count = elem->GetVisualChildrenCount();
    for (uint32_t i = 0; i < count; i++) {
        auto child = elem->GetVisualChild(i);
        auto result = TryFocusManager(child, depth - 1, outScopeRoot);
        if (result) return result;
    }

    return nullptr;
}

// Strategy 2: Walk tree checking UIElement.IsFocused.
Noesis::UIElement* FindFocusedInTree(Noesis::Visual* elem, int depth)
{
    if (!elem || depth <= 0) return nullptr;

    auto depObj = static_cast<Noesis::DependencyObject const*>(elem);
    auto val = sIsFocusedProp->GetValue(depObj);
    if (val && *static_cast<const bool*>(val)) {
        return static_cast<Noesis::UIElement*>(const_cast<Noesis::Visual*>(elem));
    }

    auto count = elem->GetVisualChildrenCount();
    for (uint32_t i = 0; i < count; i++) {
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
// carousel tab — which sits above the content area in the visual tree.
//
// Content-area option navigation is handled by Strategies 1+2 (focus).
Noesis::UIElement* FindSelectedTabInTree(Noesis::Visual* elem, int depth)
{
    if (!elem || depth <= 0) return nullptr;

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

// Combined: try FocusManager first, then IsFocused fallback, then IsSelected.
Noesis::UIElement* GetFocusedElement()
{
    auto root = GetRoot();
    if (!root) return nullptr;

    InitFocusProperties(root);

    // Strategy 1: FocusManager.FocusedElement (works without keyboard focus)
    if (sFocusedElementProp) {
        auto focused = TryFocusManager(root, 20);
        if (focused) return focused;
    }

    // Strategy 2: IsFocused tree walk (keyboard focus required)
    if (sIsFocusedProp) {
        auto focused = FindFocusedInTree(root, 20);
        if (focused) return focused;
    }

    // Strategy 3: IsSelected tree walk (for ListBoxItem-derived tab items).
    // Catches selected tabs that don't have keyboard focus.
    if (sIsSelectedProp && sListBoxItemType) {
        auto focused = FindSelectedTabInTree(root, 20);
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

// Free functions wrapping GlobalFocusMonitor singleton — called from
// LuaClient.cpp which is a different translation unit.
void TickGlobalFocusMonitor()
{
    GlobalFocusMonitor::Instance().Tick();
}

void ResetGlobalFocusMonitor()
{
    GlobalFocusMonitor::Instance().Reset();
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
    MODULE_FUNCTION(SubscribeGlobalFocusChanged)
    MODULE_FUNCTION(UnsubscribeGlobalFocusChanged)
    MODULE_FUNCTION(ForceGlobalFocusUpdate)
    MODULE_FUNCTION(HasProperty)
    MODULE_FUNCTION(GetFocusedElement)
    END_MODULE()
}

END_NS()
