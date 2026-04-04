#include <Lua/Shared/LuaMethodCallHelpers.h>
#include <GameDefinitions/UI.h>

BEGIN_NS(ecl::lua)

struct DummyDelegate
{
    void Handler(Noesis::BaseComponent* o, const RoutedEventArgs& args)
    {
        ContextGuardAnyThread ctx(ContextType::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            LuaClientPin lua(gExtender->GetClient().GetExtensionState());
            if (lua) {
                auto index = (UIEventHooks::SubscriptionIndex)(uintptr_t)this;
                lua->GetUIEvents().EventFired(index, o, args);
            }
        }
    }
};

UIEventHooks::EventHandler::~EventHandler()
{
    Unsubscribe();
}

UIEventHooks::EventHandler::EventHandler(EventHandler&& o) noexcept
    : Target(std::move(o.Target)),
    Event(o.Event),
    EventType(o.EventType),
    Handler(std::move(o.Handler)),
    Index(o.Index)
{
    o.Target.Reset();
}

UIEventHooks::EventHandler& UIEventHooks::EventHandler::operator = (EventHandler&& o) noexcept
{
    Target = std::move(o.Target);
    Event = o.Event;
    EventType = o.EventType;
    Handler = std::move(o.Handler);
    Index = o.Index;

    o.Target.Reset();
    return *this;
}

void UIEventHooks::EventHandler::Unsubscribe()
{
    IsActive = true;

    if (Target) {
        auto event = Target->mRoutedEventHandlers.Find(EventType);
        if (event != Target->mRoutedEventHandlers.End()) {
            auto self = (DummyDelegate*)(uintptr_t)(Index);
            event->value.Remove(RoutedEventHandler { self, & DummyDelegate::Handler });
        }

        Target.Reset();
    }
}

UIEventHooks::UIEventHooks(ClientState& state)
    : state_(state)
{}

UIEventHooks::~UIEventHooks()
{}

UIEventHooks::SubscriptionIndex UIEventHooks::Subscribe(UIElement* target, RoutedEvent const* event, bg3se::FixedString const& eventName, RegistryEntry&& hook)
{
    SubscriptionIndex index;
    auto sub = subscriptions_.Add(index);
    auto self = (DummyDelegate*)(uintptr_t)(index);

    auto handlers = target->mRoutedEventHandlers.Find(event);
    if (handlers == target->mRoutedEventHandlers.End()) {
        target->mRoutedEventHandlers.Insert(event, RoutedEventHandler{ self, &DummyDelegate::Handler });
    } else {
        handlers->value.Add(RoutedEventHandler{ self, &DummyDelegate::Handler });
    }

    // HACK - don't set reference to avoid dtor crash in patch 8+
    sub->Target.Reset();
    sub->Event = eventName;
    sub->EventType = event;
    sub->Handler = std::move(hook);
    sub->Index = index;
    sub->IsActive = true;

    return index;
}

bool UIEventHooks::Unsubscribe(SubscriptionIndex index)
{
    auto sub = subscriptions_.Find(index);
    if (sub == nullptr) {
        return false;
    }

    sub->Unsubscribe();
    subscriptions_.Free(index);
    return true;
}

void UIEventHooks::EventFired(SubscriptionIndex index, Noesis::BaseComponent* target, const RoutedEventArgs& args)
{
    auto sub = subscriptions_.Find(index);
    if (sub == nullptr || !sub->IsActive) {
        return;
    }

    auto L = state_.GetState();
    sub->Handler.Push(L);
    Ref func(L, lua_absindex(L, -1));

    auto eventArgs = const_cast<RoutedEventArgs*>(&args);
    ProtectedFunctionCaller<std::tuple<Noesis::BaseComponent*, RoutedEventArgs*>, void> caller{ func, std::tuple(target, eventArgs) };
    caller.Call(L, "UI event dispatch");
    lua_pop(L, 1);
}




DeferredUIEvents::DeferredUIEvents(ClientState& state)
    : state_(state)
{}

// ---------------------------------------------------------------------------
// PushFocusEventTable: builds a Lua table on the stack from FocusEventData.
// No Noesis pointers involved -- all data is C++ primitives.
// ---------------------------------------------------------------------------
static void PushFocusEventTable(lua_State* L, FocusEventData const& data)
{
    lua_newtable(L);

    // eventType (nil for FocusChanged, "PropertyChanged" for INPC)
    if (!data.eventType.empty()) {
        lua_pushstring(L, "eventType");
        lua_pushstring(L, data.eventType.c_str());
        lua_settable(L, -3);
    }

    // elemType
    lua_pushstring(L, "elemType");
    lua_pushstring(L, data.elemType.c_str());
    lua_settable(L, -3);

    // elemName
    lua_pushstring(L, "elemName");
    if (!data.elemName.empty()) {
        lua_pushstring(L, data.elemName.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // elemId
    lua_pushstring(L, "elemId");
    lua_pushstring(L, data.elemId.c_str());
    lua_settable(L, -3);

    // isTab
    lua_pushstring(L, "isTab");
    lua_pushboolean(L, data.isTab);
    lua_settable(L, -3);

    // isFocusable
    lua_pushstring(L, "isFocusable");
    lua_pushboolean(L, data.isFocusable);
    lua_settable(L, -3);

    // dcType
    lua_pushstring(L, "dcType");
    if (!data.dcType.empty()) {
        lua_pushstring(L, data.dcType.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // dcProps: flat table with scalar values + nested sub-tables
    lua_pushstring(L, "dcProps");
    if (!data.dcScalarProps.empty() || !data.dcObjectProps.empty()) {
        lua_newtable(L);

        // Scalar properties
        for (auto const& kv : data.dcScalarProps) {
            lua_pushstring(L, kv.first.c_str());
            lua_pushstring(L, kv.second.c_str());
            lua_settable(L, -3);
        }

        // Object properties (nested tables)
        for (auto const& obj : data.dcObjectProps) {
            lua_newtable(L);
            // _type field
            lua_pushstring(L, "_type");
            lua_pushstring(L, obj.typeName.c_str());
            lua_settable(L, -3);
            // Sub-properties
            for (auto const& kv : obj.props) {
                lua_pushstring(L, kv.first.c_str());
                lua_pushstring(L, kv.second.c_str());
                lua_settable(L, -3);
            }
            // Set as dcProps[propName]
            lua_pushstring(L, obj.propName.c_str());
            lua_insert(L, -2);  // swap key and nested table
            lua_settable(L, -3);
        }
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // elemText
    lua_pushstring(L, "elemText");
    if (!data.elemText.empty()) {
        lua_pushstring(L, data.elemText.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // tabName
    lua_pushstring(L, "tabName");
    if (!data.tabName.empty()) {
        lua_pushstring(L, data.tabName.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // widgetRootId
    lua_pushstring(L, "widgetRootId");
    if (!data.widgetRootId.empty()) {
        lua_pushstring(L, data.widgetRootId.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // bindings: array of {property, path, value} tables
    if (!data.bindings.empty()) {
        lua_pushstring(L, "bindings");
        lua_newtable(L);
        int bindingIndex = 1;
        for (auto const& bindingInfo : data.bindings) {
            lua_newtable(L);

            lua_pushstring(L, "property");
            lua_pushstring(L, bindingInfo.propertyName.c_str());
            lua_settable(L, -3);

            lua_pushstring(L, "path");
            lua_pushstring(L, bindingInfo.bindingPath.c_str());
            lua_settable(L, -3);

            lua_pushstring(L, "value");
            if (!bindingInfo.resolvedValue.empty()) {
                lua_pushstring(L, bindingInfo.resolvedValue.c_str());
            } else {
                lua_pushnil(L);
            }
            lua_settable(L, -3);

            lua_rawseti(L, -2, bindingIndex++);
        }
        lua_settable(L, -3);
    }

    // namedTexts: {elementName = resolvedText, ...} from NameScope iteration
    if (!data.namedTexts.empty()) {
        lua_pushstring(L, "namedTexts");
        lua_newtable(L);
        for (auto const& entry : data.namedTexts) {
            lua_pushstring(L, entry.first.c_str());
            lua_pushstring(L, entry.second.c_str());
            lua_settable(L, -3);
        }
        lua_settable(L, -3);
    }

}

// Forward declaration (defined after PostUpdate).
static void PushTickSnapshotTable(lua_State* L, TickSnapshot const& snapshot);

void DeferredUIEvents::PostUpdate()
{
    Array<DeferredCommand> commands;
    Array<DeferredPropertyChange> propertyChanges;
    Array<DeferredFocusChange> focusChanges;
    Array<DeferredTickSnapshot> tickSnapshots;
    // Avoid corruption if events are queued during update
    std::swap(commands, commands_);
    std::swap(propertyChanges, propertyChanges_);
    std::swap(focusChanges, focusChanges_);
    std::swap(tickSnapshots, tickSnapshots_);

    auto L = state_.GetState();
    for (auto const& command : commands) {
        LuaDelegate<void(Noesis::BaseCommand*, Noesis::BaseComponent*)> handler(L, command.Handler.ToRef(L));
        handler.Call(L, { command.Command.GetPtr(), command.Parameter.GetPtr() });
    }

    // Legacy priority filter deleted -- snapshot system handles all
    // event resolution in Tick() via delta comparison.

    // Focus changes: push data table + prop name to Lua callback.
    // The callback receives (table, "FocusedElement") instead of (element, symbol).
    for (auto const& change : focusChanges) {
        if (!change.Handler.TryPush(L)) continue;
        // Stack: handler_function
        PushFocusEventTable(L, change.Data);
        // Stack: handler_function, data_table
        lua_pushstring(L, "FocusedElement");
        // Stack: handler_function, data_table, "FocusedElement"
        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            auto err = lua_tostring(L, -1);
            ERR("[BG3Access] Focus callback error: %s", err ? err : "(unknown)");
            lua_pop(L, 1);
        }
    }

    // Property change events: used by INPCMonitor, DPMonitor, and
    // custom property write callbacks.  Widget-added events now use
    // the focus change path (data tables, no Noesis elements).
    for (auto const& change : propertyChanges) {
        LuaDelegate<void(Noesis::BaseComponent*, Noesis::Symbol)> handler(L, change.Handler.ToRef(L));
        handler.Call(L, { change.Object.GetPtr(), change.Property });
    }

    // Tick snapshots: one-per-frame state packages.
    // The snapshot callback receives (snapshotTable, "TickSnapshot").
    for (auto const& snapshot : tickSnapshots) {
        if (!snapshot.Handler.TryPush(L)) continue;
        PushTickSnapshotTable(L, snapshot.Snapshot);
        lua_pushstring(L, "TickSnapshot");
        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            auto err = lua_tostring(L, -1);
            ERR("[BG3Access] Tick snapshot callback error: %s", err ? err : "(unknown)");
            lua_pop(L, 1);
        }
    }
}

void DeferredUIEvents::OnCommand(lua::PersistentRegistryEntry const& handler, Noesis::BaseCommand* command, Noesis::BaseComponent* parameter)
{
    commands_.push_back(DeferredCommand{
        .Handler = handler,
        .Command = Noesis::Ptr(command),
        .Parameter = Noesis::Ptr(parameter)
    });
}

void DeferredUIEvents::OnPropertyChanged(lua::PersistentRegistryEntry const& handler, Noesis::BaseComponent* object, Noesis::Symbol property)
{
    propertyChanges_.push_back(DeferredPropertyChange{
        .Handler = handler,
        .Object = Noesis::Ptr(object),
        .Property = property
    });
}

void DeferredUIEvents::OnFocusChanged(lua::PersistentRegistryEntry const& handler, FocusEventData&& data)
{
    focusChanges_.push_back(DeferredFocusChange{
        .Handler = handler,
        .Data = std::move(data)
    });
}

void DeferredUIEvents::OnTickSnapshot(lua::PersistentRegistryEntry const& handler, TickSnapshot&& snapshot)
{
    tickSnapshots_.push_back(DeferredTickSnapshot{
        .Handler = handler,
        .Snapshot = std::move(snapshot)
    });
}

// ---------------------------------------------------------------------------
// PushTickSnapshotTable: builds a Lua table from a TickSnapshot.
// Contains change flags + full focused element data + inline carousel value.
// ---------------------------------------------------------------------------
static void PushTickSnapshotTable(lua_State* L, TickSnapshot const& snapshot)
{
    lua_newtable(L);

    // Change flags
    lua_pushstring(L, "focusChanged");
    lua_pushboolean(L, snapshot.focusChanged);
    lua_settable(L, -3);

    lua_pushstring(L, "selectionChanged");
    lua_pushboolean(L, snapshot.selectionChanged);
    lua_settable(L, -3);

    lua_pushstring(L, "valueChanged");
    lua_pushboolean(L, snapshot.valueChanged);
    lua_settable(L, -3);

    lua_pushstring(L, "inlineCarouselChanged");
    lua_pushboolean(L, snapshot.inlineCarouselChanged);
    lua_settable(L, -3);

    lua_pushstring(L, "widgetAdded");
    lua_pushboolean(L, snapshot.widgetAdded);
    lua_settable(L, -3);

    // Inline carousel value
    lua_pushstring(L, "inlineCarouselValue");
    if (!snapshot.inlineCarouselValue.empty()) {
        lua_pushstring(L, snapshot.inlineCarouselValue.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // Focused element data (nested table using existing builder)
    lua_pushstring(L, "focusedElement");
    PushFocusEventTable(L, snapshot.focusedElement);
    lua_settable(L, -3);

    // Selected element data (only when different from focused)
    if (!snapshot.selectedElementData.elemType.empty()) {
        lua_pushstring(L, "selectedElement");
        PushFocusEventTable(L, snapshot.selectedElementData);
        lua_settable(L, -3);
    }

    // Widget data (nested table, only if widgetAdded)
    if (snapshot.widgetAdded) {
        lua_pushstring(L, "widgetData");
        PushFocusEventTable(L, snapshot.widgetData);
        lua_settable(L, -3);
    }

    // Radial slot data (only if radialSlotChanged)
    lua_pushstring(L, "radialSlotChanged");
    lua_pushboolean(L, snapshot.radialSlotChanged);
    lua_settable(L, -3);

    if (snapshot.radialSlotChanged) {
        lua_pushstring(L, "radialTitleText");
        if (!snapshot.radialTitleText.empty()) {
            lua_pushstring(L, snapshot.radialTitleText.c_str());
        } else {
            lua_pushnil(L);
        }
        lua_settable(L, -3);

        lua_pushstring(L, "radialDescriptionText");
        if (!snapshot.radialDescriptionText.empty()) {
            lua_pushstring(L, snapshot.radialDescriptionText.c_str());
        } else {
            lua_pushnil(L);
        }
        lua_settable(L, -3);

        lua_pushstring(L, "radialSlotTag");
        if (!snapshot.radialSlotTag.empty()) {
            lua_pushstring(L, snapshot.radialSlotTag.c_str());
        } else {
            lua_pushnil(L);
        }
        lua_settable(L, -3);

        lua_pushstring(L, "radialSlotType");
        if (!snapshot.radialSlotType.empty()) {
            lua_pushstring(L, snapshot.radialSlotType.c_str());
        } else {
            lua_pushnil(L);
        }
        lua_settable(L, -3);
    }

    // Context menu data (only if contextMenuChanged)
    lua_pushstring(L, "contextMenuChanged");
    lua_pushboolean(L, snapshot.contextMenuChanged);
    lua_settable(L, -3);

    if (snapshot.contextMenuChanged) {
        lua_pushstring(L, "contextMenuItemText");
        if (!snapshot.contextMenuItemText.empty()) {
            lua_pushstring(L, snapshot.contextMenuItemText.c_str());
        } else {
            lua_pushnil(L);
        }
        lua_settable(L, -3);
    }
}

END_NS()
