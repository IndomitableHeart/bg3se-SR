#pragma once

#include <NsGui/UIElement.h>
#include <Lua/Shared/LuaReference.h>
#include <string>
#include <vector>
#include <utility>

BEGIN_NS(ecl::lua)

using namespace Noesis;
using namespace bg3se::lua;

// ---------------------------------------------------------------------------
// FocusEventData: all data extracted from a focused element during Tick().
// Contains ONLY C++ primitives -- no Noesis pointers.  This is the
// fundamental unit that crosses the C++/Lua boundary.
// ---------------------------------------------------------------------------
struct FocusEventData
{
    // Event type: "FocusChanged" (default), "WidgetAdded", "WidgetDCChanged"
    std::string eventType;

    // Element identity
    std::string elemType;       // GetClassType()->GetName()
    std::string elemName;       // x:Name or empty
    std::string elemId;         // "Type::Name::DCText" composite identifier

    // Element classification
    bool isTab = false;         // IsListBoxItemType
    bool isFocusable = false;   // ls:MoveFocus.Focusable

    // DataContext (ViewModel) data
    std::string dcType;         // DC class name or empty

    // DC scalar properties: {name, value} pairs
    std::vector<std::pair<std::string, std::string>> dcScalarProps;

    // DC object properties (e.g. SelectedItem): sub-object with its own props
    struct SubObject
    {
        std::string propName;   // property name on parent DC
        std::string typeName;   // sub-object class type name
        std::vector<std::pair<std::string, std::string>> props;
    };
    std::vector<SubObject> dcObjectProps;

    // Element's own text (TextBlock text, Content, ToString)
    std::string elemText;

    // Tab name (only for isTab=true)
    std::string tabName;

    // Widget root pointer as string (for widget identity tracking)
    std::string widgetRootId;

    // Binding metadata: semantic roles from XAML binding paths.
    // Each entry maps a visual property to its binding source path,
    // providing deterministic classification (e.g., Path="Title" IS a title).
    struct BindingInfo
    {
        std::string propertyName;   // DP name on the element ("Text", "Content", etc.)
        std::string bindingPath;    // Binding.Path string ("Title", "Description", etc.)
        std::string resolvedValue;  // The cached binding result (actual text)
    };
    std::vector<BindingInfo> bindings;

    // Named TextBlock texts from the widget's NameScope.
    // Key = x:Name, Value = resolved text from three-step extraction.
    // Used for authored XAML TextBlocks whose text is LocaString-bound
    // (not in any ViewModel DC), e.g. "Select your difficulty" title.
    std::vector<std::pair<std::string, std::string>> namedTexts;
};

class UIEventHooks
{
public:
    using SubscriptionIndex = uint32_t;

    UIEventHooks(ClientState& state);
    ~UIEventHooks();

    SubscriptionIndex Subscribe(UIElement* target, RoutedEvent const* event, bg3se::FixedString const& eventName, RegistryEntry&& hook);
    bool Unsubscribe(SubscriptionIndex index);
    void EventFired(SubscriptionIndex index, Noesis::BaseComponent* target, const RoutedEventArgs& args);

private:
    struct EventHandler
    {
        inline EventHandler() {}
        ~EventHandler();

        EventHandler(EventHandler const&) = delete;
        EventHandler(EventHandler &&) noexcept;
        EventHandler& operator = (EventHandler const&) = delete;
        EventHandler& operator = (EventHandler&&) noexcept;

        void Unsubscribe();

        Ptr<UIElement> Target;
        bg3se::FixedString Event;
        RoutedEvent const* EventType;
        RegistryEntry Handler;
        SubscriptionIndex Index;
        bool IsActive{ false };
    };

    ClientState& state_;
    SaltedPool<EventHandler> subscriptions_;
};

// ---------------------------------------------------------------------------
// TickSnapshot: one-per-frame data package sent from C++ to Lua.
// Contains the COMPLETE picture of what the user is looking at, plus
// change flags telling Lua HOW to speak (full announcement vs value update).
// No Noesis pointers -- all C++ primitives.
// ---------------------------------------------------------------------------
struct TickSnapshot
{
    // Change flags -- Lua uses these to decide speech behavior.
    bool focusChanged = false;          // User moved to a different element
    bool selectionChanged = false;      // Tab carousel selection changed
    bool valueChanged = false;          // INPC property changed on focused element DC
    bool inlineCarouselChanged = false; // Inline appearance carousel value changed
    bool widgetAdded = false;           // New dialog/overlay widget appeared
    bool radialSlotChanged = false;     // Radial (RT/RB) slot focus changed via LocalFocus
    bool contextMenuChanged = false;    // Context menu highlight changed (d-pad in popup)

    // Current focused element state (full picture, not just changes).
    FocusEventData focusedElement;

    // Inline carousel value (from child ListBox selectionName TextBlock).
    // Non-empty only when an inline carousel exists under the focused element.
    std::string inlineCarouselValue;

    // Selected element data (from Strategy 3 ListBoxItem when it differs
    // from the focused element).  Carries the VM DC type needed for CC
    // section labels (VMSelectableRace -> "Race", etc.).
    FocusEventData selectedElementData;

    // Widget added data (dialog/overlay detection).
    // Only populated when widgetAdded == true.
    FocusEventData widgetData;

    // Radial slot data (RT shortcuts radial, RB action radial).
    // Only populated when radialSlotChanged == true.
    // titleText: display name from ActionTitle TextBlock (localized).
    // descriptionText: description from Description TextBlock (localized).
    // slotTag: raw Tag string from the LSRadialListItem ("CharacterSheet", etc.)
    //          or Content.Name from VMHotBarSlot ("Main Hand Attack", etc.).
    // slotType: identifies which radial ("ShortcutsMenu" or "HotBar").
    std::string radialTitleText;
    std::string radialDescriptionText;
    std::string radialSlotTag;
    std::string radialSlotType;

    // Context menu data (WorldContextMenu popup).
    // Only populated when contextMenuChanged == true.
    std::string contextMenuItemText;    // Display text of highlighted item
};

class DeferredUIEvents
{
public:
    DeferredUIEvents(ClientState& state);

    void PostUpdate();

    void OnCommand(lua::PersistentRegistryEntry const& handler, Noesis::BaseCommand* command, Noesis::BaseComponent* parameter);

    // Legacy event queues -- kept for backwards compatibility during transition.
    // TODO: remove once snapshot system is fully validated.
    void OnPropertyChanged(lua::PersistentRegistryEntry const& handler, Noesis::BaseComponent* object, Noesis::Symbol property);
    void OnFocusChanged(lua::PersistentRegistryEntry const& handler, FocusEventData&& data);

    // New snapshot dispatch: one call per tick with complete state.
    void OnTickSnapshot(lua::PersistentRegistryEntry const& handler, TickSnapshot&& snapshot);

private:
    struct DeferredCommand
    {
        lua::PersistentRegistryEntry Handler;
        Ptr<Noesis::BaseCommand> Command;
        Ptr<Noesis::BaseComponent> Parameter;
    };

    struct DeferredPropertyChange
    {
        lua::PersistentRegistryEntry Handler;
        Ptr<Noesis::BaseComponent> Object;
        Noesis::Symbol Property;
    };

    struct DeferredFocusChange
    {
        lua::PersistentRegistryEntry Handler;
        FocusEventData Data;
    };

    struct DeferredTickSnapshot
    {
        lua::PersistentRegistryEntry Handler;
        TickSnapshot Snapshot;
    };

    ClientState& state_;
    Array<DeferredCommand> commands_;
    Array<DeferredPropertyChange> propertyChanges_;
    Array<DeferredFocusChange> focusChanges_;
    Array<DeferredTickSnapshot> tickSnapshots_;
};

END_SE()
