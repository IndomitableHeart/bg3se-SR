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
{}

UIEventHooks::EventHandler& UIEventHooks::EventHandler::operator = (EventHandler&& o) noexcept
{
    Target = std::move(o.Target);
    Event = o.Event;
    EventType = o.EventType;
    Handler = std::move(o.Handler);
    Index = o.Index;

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

    // elemAddr
    lua_pushstring(L, "elemAddr");
    lua_pushstring(L, data.elemAddr.c_str());
    lua_settable(L, -3);

    // isTab
    lua_pushstring(L, "isTab");
    lua_pushboolean(L, data.isTab);
    lua_settable(L, -3);

    // isFocusable
    lua_pushstring(L, "isFocusable");
    lua_pushboolean(L, data.isFocusable);
    lua_settable(L, -3);

    // isChecked (only for ToggleButton-derived elements)
    if (data.isChecked >= 0) {
        lua_pushstring(L, "isChecked");
        lua_pushboolean(L, data.isChecked == 1);
        lua_settable(L, -3);
    }

    // dcType
    lua_pushstring(L, "dcType");
    if (!data.dcType.empty()) {
        lua_pushstring(L, data.dcType.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_settable(L, -3);

    // dcProps: flat table with scalar values + nested sub-tables + collections
    lua_pushstring(L, "dcProps");
    if (!data.dcScalarProps.empty() || !data.dcObjectProps.empty()
        || !data.dcCollectionProps.empty()) {
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
            // Scalar sub-properties
            for (auto const& kv : obj.props) {
                lua_pushstring(L, kv.first.c_str());
                lua_pushstring(L, kv.second.c_str());
                lua_settable(L, -3);
            }
            // Nested collections (e.g. SelectedItem.Participants).
            // Same shape as top-level collection arrays.
            for (auto const& nestedColl : obj.collections) {
                lua_newtable(L);
                int nestedArrayIndex = 1;
                for (auto const& nestedItem : nestedColl.items) {
                    lua_newtable(L);
                    lua_pushstring(L, "_type");
                    lua_pushstring(L, nestedItem.typeName.c_str());
                    lua_settable(L, -3);
                    for (auto const& kv : nestedItem.props) {
                        lua_pushstring(L, kv.first.c_str());
                        lua_pushstring(L, kv.second.c_str());
                        lua_settable(L, -3);
                    }
                    lua_rawseti(L, -2, nestedArrayIndex++);
                }
                lua_pushstring(L, nestedColl.propName.c_str());
                lua_insert(L, -2);
                lua_settable(L, -3);
            }
            // Set as dcProps[propName]
            lua_pushstring(L, obj.propName.c_str());
            lua_insert(L, -2);  // swap key and nested table
            lua_settable(L, -3);
        }

        // Collection properties (arrays of item sub-tables)
        for (auto const& collectionProp : data.dcCollectionProps) {
            lua_newtable(L);
            int luaArrayIndex = 1;
            for (auto const& collectionItem : collectionProp.items) {
                lua_newtable(L);
                lua_pushstring(L, "_type");
                lua_pushstring(L, collectionItem.typeName.c_str());
                lua_settable(L, -3);
                for (auto const& kv : collectionItem.props) {
                    lua_pushstring(L, kv.first.c_str());
                    lua_pushstring(L, kv.second.c_str());
                    lua_settable(L, -3);
                }
                // Sub-objects on the collection item (e.g.
                // DialogueLines[i].Speaker).  Each becomes a nested
                // table with _type + scalars.
                for (auto const& nestedSub : collectionItem.subObjects) {
                    lua_newtable(L);
                    lua_pushstring(L, "_type");
                    lua_pushstring(L, nestedSub.typeName.c_str());
                    lua_settable(L, -3);
                    for (auto const& subKv : nestedSub.props) {
                        lua_pushstring(L, subKv.first.c_str());
                        lua_pushstring(L, subKv.second.c_str());
                        lua_settable(L, -3);
                    }
                    lua_pushstring(L, nestedSub.propName.c_str());
                    lua_insert(L, -2);
                    lua_settable(L, -3);
                }
                lua_rawseti(L, -2, luaArrayIndex++);
            }
            lua_pushstring(L, collectionProp.propName.c_str());
            lua_insert(L, -2);
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

    // templateTexts: array of all visible TextBlock texts from template children
    if (!data.templateTexts.empty()) {
        lua_pushstring(L, "templateTexts");
        lua_newtable(L);
        for (size_t i = 0; i < data.templateTexts.size(); i++) {
            lua_pushstring(L, data.templateTexts[i].c_str());
            lua_rawseti(L, -2, (int)(i + 1));
        }
        lua_settable(L, -3);
    }

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

    // ancestorContext
    if (!data.ancestorContext.empty()) {
        lua_pushstring(L, "ancestorContext");
        lua_pushstring(L, data.ancestorContext.c_str());
        lua_settable(L, -3);
    }

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

    // Inline carousel color hex (for skin/hair/eye color descriptions)
    if (!snapshot.inlineCarouselColorHex.empty()) {
        lua_pushstring(L, "inlineCarouselColorHex");
        lua_pushstring(L, snapshot.inlineCarouselColorHex.c_str());
        lua_settable(L, -3);
    }

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

    // Widget events: array of tables, one per new/changed widget this tick.
    // Each entry is a complete FocusEventData with dcType, elemName, etc.
    if (!snapshot.widgetEvents.empty()) {
        lua_pushstring(L, "widgetEvents");
        lua_createtable(L, static_cast<int>(snapshot.widgetEvents.size()), 0);
        for (size_t eventIndex = 0; eventIndex < snapshot.widgetEvents.size(); eventIndex++) {
            PushFocusEventTable(L, snapshot.widgetEvents[eventIndex]);
            lua_rawseti(L, -2, static_cast<int>(eventIndex) + 1);
        }
        lua_settable(L, -3);
    }

    // Widget removal data (only if widgetRemoved)
    lua_pushstring(L, "widgetRemoved");
    lua_pushboolean(L, snapshot.widgetRemoved);
    lua_settable(L, -3);

    if (snapshot.widgetRemoved) {
        lua_pushstring(L, "removedWidgetData");
        PushFocusEventTable(L, snapshot.removedWidgetData);
        lua_settable(L, -3);
    }

    // All visible widget DC types this tick (from cached scan).
    // Includes ALL visible widgets, not just new/changed ones.
    // Lua uses this for presence checks (e.g., "is a menu still open?").
    if (!snapshot.widgetDCTypes.empty()) {
        lua_pushstring(L, "widgetDCTypes");
        lua_createtable(L, static_cast<int>(snapshot.widgetDCTypes.size()), 0);
        for (size_t typeIndex = 0; typeIndex < snapshot.widgetDCTypes.size(); typeIndex++) {
            lua_pushstring(L, snapshot.widgetDCTypes[typeIndex].c_str());
            lua_rawseti(L, -2, static_cast<int>(typeIndex) + 1);
        }
        lua_settable(L, -3);
    }

    // Parallel to widgetDCTypes: widget element pointer addresses
    // (hex strings).  widgetAddrs[i] is the address of the widget
    // whose DC type is widgetDCTypes[i].  Lua anchors handlers by
    // identity using these addresses.
    if (!snapshot.widgetAddrs.empty()) {
        lua_pushstring(L, "widgetAddrs");
        lua_createtable(L, static_cast<int>(snapshot.widgetAddrs.size()), 0);
        for (size_t addrIndex = 0; addrIndex < snapshot.widgetAddrs.size(); addrIndex++) {
            lua_pushstring(L, snapshot.widgetAddrs[addrIndex].c_str());
            lua_rawseti(L, -2, static_cast<int>(addrIndex) + 1);
        }
        lua_settable(L, -3);
    }

    // Parallel to widgetDCTypes / widgetAddrs: widget x:Name strings.
    // widgetNames[i] is the x:Name of the widget whose DC type is
    // widgetDCTypes[i] and whose address is widgetAddrs[i].  Lua
    // routes generic-DC widgets (ls.Widget) by x:Name when DC-type
    // routing alone can't identify the right handler.
    if (!snapshot.widgetNames.empty()) {
        lua_pushstring(L, "widgetNames");
        lua_createtable(L, static_cast<int>(snapshot.widgetNames.size()), 0);
        for (size_t nameIndex = 0; nameIndex < snapshot.widgetNames.size(); nameIndex++) {
            lua_pushstring(L, snapshot.widgetNames[nameIndex].c_str());
            lua_rawseti(L, -2, static_cast<int>(nameIndex) + 1);
        }
        lua_settable(L, -3);
    }

    // All-tracked widget arrays: same shape as the visibility-filtered
    // arrays above, but include widgets whose IsVisible is currently
    // false.  Lua liveness checks use these so animation-frame
    // visibility flicker doesn't masquerade as a permanent close.
    if (!snapshot.allWidgetDCTypes.empty()) {
        lua_pushstring(L, "allWidgetDCTypes");
        lua_createtable(L, static_cast<int>(snapshot.allWidgetDCTypes.size()), 0);
        for (size_t i = 0; i < snapshot.allWidgetDCTypes.size(); i++) {
            lua_pushstring(L, snapshot.allWidgetDCTypes[i].c_str());
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        lua_settable(L, -3);
    }
    if (!snapshot.allWidgetAddrs.empty()) {
        lua_pushstring(L, "allWidgetAddrs");
        lua_createtable(L, static_cast<int>(snapshot.allWidgetAddrs.size()), 0);
        for (size_t i = 0; i < snapshot.allWidgetAddrs.size(); i++) {
            lua_pushstring(L, snapshot.allWidgetAddrs[i].c_str());
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        lua_settable(L, -3);
    }
    if (!snapshot.allWidgetNames.empty()) {
        lua_pushstring(L, "allWidgetNames");
        lua_createtable(L, static_cast<int>(snapshot.allWidgetNames.size()), 0);
        for (size_t i = 0; i < snapshot.allWidgetNames.size(); i++) {
            lua_pushstring(L, snapshot.allWidgetNames[i].c_str());
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
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

    // Visual text source widget name
    if (!snapshot.visualTextWidgetName.empty()) {
        lua_pushstring(L, "visualTextWidgetName");
        lua_pushstring(L, snapshot.visualTextWidgetName.c_str());
        lua_settable(L, -3);
    }

    // CC visible page title
    if (!snapshot.activePageTitle.empty()) {
        lua_pushstring(L, "activePageTitle");
        lua_pushstring(L, snapshot.activePageTitle.c_str());
        lua_settable(L, -3);
    }

    // Tooltip data (only if tooltipChanged)
    lua_pushstring(L, "tooltipChanged");
    lua_pushboolean(L, snapshot.tooltipChanged);
    lua_settable(L, -3);

    if (snapshot.tooltipChanged && !snapshot.tooltipTexts.empty()) {
        lua_pushstring(L, "tooltipTexts");
        lua_newtable(L);
        for (size_t i = 0; i < snapshot.tooltipTexts.size(); i++) {
            auto const& entry = snapshot.tooltipTexts[i];
            lua_newtable(L);

            lua_pushstring(L, "role");
            if (!entry.role.empty()) {
                lua_pushstring(L, entry.role.c_str());
            } else {
                lua_pushnil(L);
            }
            lua_settable(L, -3);

            lua_pushstring(L, "text");
            lua_pushstring(L, entry.text.c_str());
            lua_settable(L, -3);

            lua_pushstring(L, "parentRole");
            if (!entry.parentRole.empty()) {
                lua_pushstring(L, entry.parentRole.c_str());
            } else {
                lua_pushnil(L);
            }
            lua_settable(L, -3);

            lua_pushstring(L, "fontSize");
            lua_pushnumber(L, entry.fontSize);
            lua_settable(L, -3);

            if (!entry.typeId.empty()) {
                lua_pushstring(L, "typeId");
                lua_pushstring(L, entry.typeId.c_str());
                lua_settable(L, -3);
            }

            lua_rawseti(L, -2, (int)(i + 1));
        }
        lua_settable(L, -3);
    }
}

END_NS()

BEGIN_NS(ecl::lua::ui)

void ReleasePropertyChangeHandlers(lua_State* L);

END_NS()
