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
void InitFocusProperties(Noesis::FrameworkElement* root);
Noesis::UIElement* TryFocusManager(Noesis::Visual* elem, int depth,
                                    Noesis::DependencyObject** outScopeRoot = nullptr);
Noesis::UIElement* FindFocusedInTree(Noesis::Visual* elem, int depth);

// Forward declaration — defined below, after InitFocusProperties.
Noesis::UIElement* GetFocusedElement();

// GlobalFocusMonitor: per-frame focus change detector.
// Checks GetFocusedElement() once per frame during ClientState::OnUpdate.
// If the focused element changed since last frame, fires a Lua callback
// via DeferredUIEvents (thread-safe, dispatched in PostUpdate).
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
        return true;
    }

    void Unsubscribe()
    {
        callback_ = {};
        lastFocused_ = nullptr;
        lastFocusedRef_.Reset();
    }

    bool IsActive() const { return callback_.operator bool(); }

    void ForceNextFire() { forceNext_ = true; }

    void Tick()
    {
        if (!callback_) return;

        auto focused = GetFocusedElement();

        bool changed = (focused != lastFocused_);
        bool forced = forceNext_;
        forceNext_ = false;

        if (changed) {
            lastFocused_ = focused;
            if (focused) {
                lastFocusedRef_ = Noesis::Ptr<Noesis::UIElement>(focused);
            } else {
                lastFocusedRef_.Reset();
            }
        }

        if (changed || forced) {
            FireCallback(focused);
        }
    }

    void Reset()
    {
        lastFocused_ = nullptr;
        lastFocusedRef_.Reset();
    }

private:
    void FireCallback(Noesis::UIElement* focused)
    {
        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin pin(gExtender->GetClient().GetExtensionState());
            if (pin) {
                pin->GetDeferredUIEvents().OnPropertyChanged(
                    callback_, focused, Noesis::Symbol("FocusedElement"));
            }
        }
    }

    Noesis::UIElement* lastFocused_ = nullptr;
    Noesis::Ptr<Noesis::UIElement> lastFocusedRef_;
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

// Combined: try FocusManager first, then IsFocused fallback.
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
