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
    std::string elemAddr;       // Element pointer as hex string (stable
                                // across ticks for the same element even
                                // when text/DC changes)

    // Element classification
    bool isTab = false;         // IsListBoxItemType
    bool isFocusable = false;   // ls:MoveFocus.Focusable
    int isChecked = -1;         // ToggleButton.IsChecked: 1=true, 0=false, -1=N/A

    // DataContext (ViewModel) data
    std::string dcType;         // DC class name or empty

    // DC scalar properties: {name, value} pairs
    std::vector<std::pair<std::string, std::string>> dcScalarProps;

    // DC object properties (e.g. SelectedItem): sub-object with its own props.
    // Forward-declared so CollectionItem can hold a vector of SubObjects.
    struct SubObject;

    // DC collection properties: when a TypeProperty points to a
    // BaseCollection, enumerate its items and read their scalar props
    // AND their sub-objects (one level deep).  Produces an indexed
    // array of sub-tables in Lua (dcProps.PropName[1], etc.).  The
    // sub-objects path is what makes JournalDialogue.DialogueLines[i].
    // Speaker.Name reachable -- without it, we'd have line text but no
    // way to attribute it to a speaker.
    struct CollectionItem
    {
        std::string typeName;   // item class type name
        std::vector<std::pair<std::string, std::string>> props;
        std::vector<SubObject> subObjects;
    };
    struct CollectionProperty
    {
        std::string propName;   // collection property name on parent DC
        std::vector<CollectionItem> items;
    };

    // SubObject definition.  Holds scalar props and nested collections
    // one level deep (so SelectedItem.Participants is reachable -- a
    // JournalDialogue VM exposed as the DC's SelectedItem has a
    // Participants collection of DialogueParticipant VMs we couldn't
    // see otherwise).  Same shape as top-level dcCollectionProps.
    struct SubObject
    {
        std::string propName;   // property name on parent DC
        std::string typeName;   // sub-object class type name
        std::vector<std::pair<std::string, std::string>> props;
        std::vector<CollectionProperty> collections;
    };
    std::vector<SubObject> dcObjectProps;
    std::vector<CollectionProperty> dcCollectionProps;

    // Element's own text (TextBlock text, Content, ToString)
    std::string elemText;

    // All visible TextBlock texts from the element's template children.
    // Populated via bounded GatherVisibleTextBlocks -- includes all text
    // rendered inside the element (converter outputs, DataTrigger-shown
    // labels, etc.).  Gives Lua the full picture without enum resolution.
    std::vector<std::string> templateTexts;

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

    // Ancestor context: first parent x:Name containing "Melee" or "Ranged".
    // Empty if no such ancestor found within a few hops.  Used by Lua to
    // qualify stats like "Attack Bonus" -> "Melee Attack Bonus".
    std::string ancestorContext;

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
        RoutedEvent const* EventType{ nullptr };
        RegistryEntry Handler;
        SubscriptionIndex Index{ 0 };
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
    bool widgetRemoved = false;         // A previously-visible widget became invisible

    // Current focused element state (full picture, not just changes).
    FocusEventData focusedElement;

    // Inline carousel value (from child ListBox selectionName TextBlock).
    // Non-empty only when an inline carousel exists under the focused element.
    std::string inlineCarouselValue;

    // UIColor hex from the carousel SelectedItem (e.g. "#FFFFF0E6").
    // Present only for color swatch carousels (skin, hair, eye).
    // Lua converts to a spoken color description via HSL binning.
    std::string inlineCarouselColorHex;

    // Selected element data (from Strategy 3 ListBoxItem when it differs
    // from the focused element).  Carries the VM DC type needed for CC
    // section labels (VMSelectableRace -> "Race", etc.).
    FocusEventData selectedElementData;

    // Widget events: one entry per new/newly-visible widget this tick.
    // Each entry is a complete FocusEventData with dcType, elemName,
    // dcProps, namedTexts, etc.  Replaces the old single widgetData
    // field (which suffered from last-wins overwrite when multiple
    // widgets fired on the same tick).  Lua iterates all entries to
    // find handler-relevant widgets.
    std::vector<FocusEventData> widgetEvents;

    // All visible widget DC types this tick (cached scan, refreshed
    // only when the widget set changes).  Distinct from widgetEvents:
    // includes ALL visible widgets, not just new/changed ones.  Lua
    // uses this for presence checks (e.g. "is the menu still open?",
    // "is a discovery-only handler's widget still around?").
    std::vector<std::string> widgetDCTypes;

    // Widget element pointer addresses (hex strings, same format as
    // FocusEventData::widgetRootId), parallel to widgetDCTypes[i].
    // Lua uses these to anchor a handler by widget identity when the
    // widget-level DC is generic (ls.Widget) -- record the address at
    // activation time, verify the widget is still in this array each
    // tick.  Immune to DC thrashing on child elements and to transient
    // focus steals by notification banners.
    std::vector<std::string> widgetAddrs;

    // Widget element x:Name strings (e.g. "JournalCombatLog_c",
    // "TargetInfo_c"), parallel to widgetDCTypes[i] / widgetAddrs[i].
    // Lua uses these for widget-name routing when a widget has a
    // generic ls.Widget DC and DC-type routing alone can't identify
    // which handler should activate (e.g. JournalCombatLog_c).
    // Empty string when the widget has no x:Name set.  Reads happen
    // inside the existing CollectWidgetDCTypes_SEH wrapper -- no
    // separate SEH path or Noesis call introduced.
    std::vector<std::string> widgetNames;

    // All TRACKED widget DC types / Addrs / x:Names this tick,
    // including widgets whose computed IsVisible is currently false.
    // Parallel arrays (allWidgetDCTypes[i] / allWidgetAddrs[i] /
    // allWidgetNames[i] all describe the same widget instance).
    //
    // Distinct from widgetDCTypes/Addrs/Names which include only
    // CURRENTLY-VISIBLE widgets.  Use the all* arrays to answer
    // "is this widget loaded?" -- the answer is invariant across
    // animation frames and visibility transitions on ancestors.
    // Use the visibility-filtered arrays to answer "is this widget
    // displayed to the user RIGHT THIS FRAME?"  The two questions
    // are different and conflating them masks transient flicker as
    // permanent close (e.g., menu navigation through animations).
    //
    // sTrackedWidgets is maintained by ls.UIWidget.Loaded/Unloaded
    // class handlers, so the all* arrays change only when widgets
    // actually load or unload, not when their visibility flickers.
    std::vector<std::string> allWidgetDCTypes;
    std::vector<std::string> allWidgetAddrs;
    std::vector<std::string> allWidgetNames;

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

    // Visual text source widget name (from initial widget scan).
    // Identifies which widget the _visualText_N entries came from
    // (e.g. "shortcutsMenu" vs other widgets sharing the same DC type).
    std::string visualTextWidgetName;

    // Context menu data (WorldContextMenu popup).
    // Only populated when contextMenuChanged == true.
    std::string contextMenuItemText;    // Display text of highlighted item

    // Widget removal data (menu close detection).
    // Only populated when widgetRemoved == true.
    // Contains the DC type and element name of the widget that became
    // invisible.  Lua uses this to deactivate the matching handler.
    FocusEventData removedWidgetData;

    // Tooltip data (Examine panel, stat tooltips).
    // Only populated when tooltipChanged == true.
    bool tooltipChanged = false;        // Tooltip text changed (new popup or text delta)

    // CC visible page title: the first large-font TextBlock text
    // inside gameplaySubPanel (e.g. "High Elf Cantrip", "Skill
    // Proficiency", "Abilities").  Reliable across tab changes
    // AND sub-panel transitions within the same tab.
    std::string activePageTitle;

    // Structured tooltip entries.  Each entry carries all data C++
    // can extract from a tooltip TextBlock in one pass -- role,
    // text, font size, parent role -- so Lua never needs to ask
    // C++ for more information about a tooltip element.
    struct TooltipEntry {
        std::string role;       // TextBlock x:Name (e.g. "TitleName",
                                // "Description").  Empty when the
                                // template has no x:Name.
        std::string text;       // Rendered text content.
        std::string parentRole; // Parent element x:Name (fallback
                                // context when role is empty).
        float fontSize;         // TextBlock FontSize (distinguishes
                                // title from body text).
        std::string typeId;     // TypeId from parent container's DC
                                // (e.g. "Range", "ZoneRadius").
                                // Only set for PropertyText entries.
    };
    std::vector<TooltipEntry> tooltipTexts;
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
