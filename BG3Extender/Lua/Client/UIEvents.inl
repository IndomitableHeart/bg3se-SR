#include <Lua/Shared/LuaMethodCallHelpers.h>
#include <GameDefinitions/UI.h>      
#include <GameDefinitions/Base/Base.h> 

// TODO: You MUST include the actual header that defines OsiMsg here.
// Example: #include <Osiris/DebugInterface.h> 
// #include <sstream> 

BEGIN_NS(ecl::lua)

struct DummyDelegate
{
    void Handler(Noesis::BaseComponent* o, const RoutedEventArgs& args)
    {        
        const char* eventNameCStr = "UnknownEvent";
        if (args.routedEvent) {
            const Noesis::Symbol& nameAsSymbol = args.routedEvent->GetName(); 
            if (!nameAsSymbol.IsNull() && nameAsSymbol.Str()) { 
                eventNameCStr = nameAsSymbol.Str();
            } else if (nameAsSymbol.IsNull()) {
                eventNameCStr = "[NullSymbol]";
            }
        }
        const char* targetTypeName = (o && o->GetClassType() && o->GetClassType()->GetName()) ? o->GetClassType()->GetName() : "UnknownTarget";
        OsiMsg("CPP_DummyDelegate::Handler - TargetType: '" << targetTypeName << "' (Ptr: " << reinterpret_cast<void*>(o) << "), EventName: '" << eventNameCStr << "', Handled: " << (args.handled ? "true" : "false"));
        
        ContextGuardAnyThread ctx(ContextType::Client); // Keep this as per latest SE
        
        OsiMsg("CPP_DummyDelegate::Handler - After ContextGuardAnyThread. IntendedCtx: Client");
        LuaClientPin lua(gExtender->GetClient().GetExtensionState());
        if (lua) {
            OsiMsg("CPP_DummyDelegate::Handler - LuaClientPin Acquired. Index: " << (uintptr_t)this);
            auto index = (UIEventHooks::SubscriptionIndex)(uintptr_t)this;
            lua->GetUIEvents().EventFired(index, o, args);
        } else { OsiMsg("[ERROR] CPP_DummyDelegate::Handler - LuaClientPin FAILED!"); }
        OsiMsg("CPP_DummyDelegate::Handler EXIT - Event: '" << eventNameCStr << "'");
    }
};

UIEventHooks::EventHandler::~EventHandler() 
{
    Unsubscribe();
}

// Corrected: noexcept removed, IsActive handled properly
UIEventHooks::EventHandler::EventHandler(EventHandler&& o) 
    : Target(std::move(o.Target)),
    Event(std::move(o.Event)), 
    EventType(o.EventType),
    Handler(std::move(o.Handler)),
    Index(o.Index),
    IsActive(o.IsActive) // Capture moved IsActive state
{
    o.Target.Reset(); // As per original SE code
    o.Index = UIEventHooks::SubscriptionIndex{}; // Clear moved-from index
    o.IsActive = false; // Mark moved-from as inactive
}

// Corrected: noexcept removed, added self-assignment check, Unsubscribe, and proper member handling
UIEventHooks::EventHandler& UIEventHooks::EventHandler::operator = (EventHandler&& o) 
{
    if (this != &o) 
    {
        Unsubscribe(); // Unsubscribe current object's event first
        Target = std::move(o.Target);
        Event = std::move(o.Event); 
        EventType = o.EventType;
        Handler = std::move(o.Handler);
        Index = o.Index;
        IsActive = o.IsActive; // Capture moved IsActive state

        o.Target.Reset(); // As per original SE code
        o.Index = UIEventHooks::SubscriptionIndex{}; // Clear moved-from index
        o.IsActive = false; // Mark moved-from as inactive
    }
    return *this;
}

void UIEventHooks::EventHandler::Unsubscribe() 
{ 
    if (!IsActive) { 
        return; 
    }
    if (Target) { 
        auto event = Target->mRoutedEventHandlers.Find(EventType); 
        if (event != Target->mRoutedEventHandlers.End()) { 
            auto self = (DummyDelegate*)(uintptr_t)(Index); 
            event->value.Remove(RoutedEventHandler{ self, &DummyDelegate::Handler }); 
        } 
        Target.Reset(); 
    } else {
        OsiMsg("[WARN] CPP_EventHandler::Unsubscribe - Target was null for Index: " << static_cast<uint32_t>(Index) << " Event: '" << Event << "'. Noesis listener not removed from target.");
    }
    IsActive = false; 
}

UIEventHooks::UIEventHooks(ClientState& state) : state_(state) {}
UIEventHooks::~UIEventHooks() {}

UIEventHooks::SubscriptionIndex UIEventHooks::Subscribe(
    UIElement* target, 
    RoutedEvent const* event, 
    bg3se::FixedString const& eventName, 
    RegistryEntry&& hook)
{
    SubscriptionIndex index;
    auto sub = subscriptions_.Add(index); 
    if (!sub) {
        OsiMsg("[ERROR] CPP_UIEventHooks::Subscribe - Failed to add subscription to pool.");
        return UIEventHooks::SubscriptionIndex{}; 
    }
    auto selfDelegate = (DummyDelegate*)(uintptr_t)(index);
    auto handlers = target->mRoutedEventHandlers.Find(event);
    if (handlers == target->mRoutedEventHandlers.End()) {
        target->mRoutedEventHandlers.Insert(event, RoutedEventHandler{ selfDelegate, &DummyDelegate::Handler });
    } else {
        handlers->value.Add(RoutedEventHandler{ selfDelegate, &DummyDelegate::Handler });
    }
    sub->Target.Reset(); // Per the HACK in original SE code
    sub->Event = eventName; 
    sub->EventType = event;
    sub->Handler = std::move(hook);
    sub->Index = index;
    sub->IsActive = true;
    OsiMsg("CPP_UIEventHooks::Subscribe - Index: " << static_cast<uint32_t>(index) << ", LuaEventName: '" << eventName << "', TargetPtr: " << reinterpret_cast<void*>(target) << ". Target ref Reset().");
    return index;
}

bool UIEventHooks::Unsubscribe(SubscriptionIndex index) {
    auto sub = subscriptions_.Find(index);
    if (sub == nullptr) { OsiMsg("[WARN] CPP_UIEventHooks::Unsubscribe - Sub not found for Index: " << static_cast<uint32_t>(index)); return false; }
    sub->Unsubscribe(); 
    subscriptions_.Free(index);
    return true;
}

void UIEventHooks::EventFired(SubscriptionIndex index, Noesis::BaseComponent* target, const RoutedEventArgs& args)
{
    OsiMsg("CPP_UIEventHooks::EventFired - Received for Index: " << static_cast<uint32_t>(index));
    auto sub = subscriptions_.Find(index);
    if (sub == nullptr) { OsiMsg("[WARN] CPP_UIEventHooks::EventFired - No sub for Index: " << static_cast<uint32_t>(index)); return; }
    if (!sub->IsActive) { OsiMsg("CPP_UIEventHooks::EventFired - Sub for Index: " << static_cast<uint32_t>(index) << " is not active."); return; }
    
    OsiMsg("CPP_UIEventHooks::EventFired - Preparing Lua call for Index: " << static_cast<uint32_t>(index) << ". StoredClientName: '" << sub->Event << "'"); 
    
    auto L = state_.GetState(); 
    if (!L) { OsiMsg("[ERROR] CPP_UIEventHooks::EventFired - Lua state is NULL!"); return; }

    sub->Handler.Push(L);  
    Ref func(L, lua_absindex(L, -1)); 

    auto eventArgsPtr = const_cast<RoutedEventArgs*>(&args);
    ProtectedFunctionCaller<std::tuple<Noesis::BaseComponent*, RoutedEventArgs*>, void> caller{ func, std::make_tuple(target, eventArgsPtr) };
    
    OsiMsg("CPP_UIEventHooks::EventFired - Calling Lua handler for Index: " << static_cast<uint32_t>(index) << "...");
    caller.Call(L, "UI event dispatch");                                                             
    OsiMsg("CPP_UIEventHooks::EventFired - Lua handler call finished for Index: " << static_cast<uint32_t>(index));
    lua_pop(L, 1); 
}

END_NS()