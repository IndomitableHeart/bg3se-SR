#pragma once

#include <GameDefinitions/Base/Base.h>

BEGIN_NS(interrupt)

struct ActionStateComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptActionState, "eoc::interrupt::ActionStateComponent")

    InterruptEvent Event;
    Array<ActionEntry> Actions;
    Guid SpellCastGuid;
};

struct ConditionallyDisabledComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptConditionallyDisabled, "eoc::interrupt::ConditionallyDisabledComponent")

    uint8_t Dummy;
};

struct ContainerComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptContainer, "eoc::interrupt::ContainerComponent")

    Array<EntityHandle> Interrupts;
};

struct DataComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptData, "eoc::interrupt::DataComponent")

    FixedString Interrupt;
    uint8_t field_4;
    [[bg3::legacy(field_8)]] EntityHandle InterruptEntity;
    [[bg3::legacy(field_10)]] EntityHandle Caster;
    [[bg3::legacy(field_18)]] FixedString Spell;
};

struct DecisionComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptDecision, "eoc::interrupt::DecisionComponent")

    HashMap<InterruptEvent, uint8_t> Decisions;
};

struct PreferencesComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptPreferences, "eoc::interrupt::PreferencesComponent")

    HashMap<FixedString, uint8_t> Preferences;
};

struct PreparedComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptPrepared, "eoc::interrupt::PreparedComponent")

    uint8_t Dummy;
};

struct ZoneComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptZone, "eoc::interrupt::ZoneComponent")

    Guid field_0;
};

struct ZoneParticipantComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptZoneParticipant, "eoc::interrupt::ZoneParticipantComponent")

    HashMap<EntityHandle, uint8_t> field_0;
};

struct ZoneSourceComponent : public BaseComponent
{
    DEFINE_COMPONENT(InterruptZoneSource, "eoc::interrupt::ZoneSourceComponent")

    uint8_t Dummy;
};

END_NS()

BEGIN_NS(esv::interrupt)

struct StartRequestData
{
    Guid SpellCastGuid;
    bg3se::interrupt::InterruptEvent Event;
    HashMap<EntityHandle, HashSet<EntityHandle>> Interruptors;
    uint8_t field_158{ 0 };
};

struct StopRequestData
{
    Guid SpellCastGuid;
    bg3se::interrupt::InterruptEvent Event;
    uint8_t field_118{ 0 };
};

struct UpdateInterruptorsRequestData
{
    Guid SpellCastGuid;
    HashMap<EntityHandle, HashSet<EntityHandle>> Interruptors;
};

struct CombatLogRequestData
{
    EntityHandle field_0;
    Array<stats::ActionResourceCost> UseCosts;
    HashMap<EntityHandle, HashMap<bg3se::interrupt::InterruptEvent, bg3se::interrupt::InterruptUsageEntry>> field_18;
};

struct InterruptDataComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerSpellCastInterrupt, "esv::spell_cast::InterruptDataComponent")

    Guid SpellCastGuid;
    std::optional<bg3se::interrupt::UndecidedEvent> Event;
    int32_t NextInterruptIndex;
    Array<bg3se::interrupt::PausedAnimationEvent> PausedAnimationEvents;
    Array<bg3se::interrupt::AnimationInterruptData> AnimationInterrupts;
    int32_t AnimationIndex;
};

END_NS()

BEGIN_NS(esv::spell_cast)

struct InterruptRequestsComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerSpellInterruptRequests, "esv::spell_cast::InterruptRequestsComponent")

    Array<interrupt::StartRequestData> StartRequests;
    Array<interrupt::StopRequestData> StopRequests;
    Array<interrupt::CombatLogRequestData> CombatLogRequests;
    Array<interrupt::UpdateInterruptorsRequestData> UpdateInterruptorsRequests;
};

struct FunctorConditional
{
    Guid FunctorUuid;
    EntityHandle Target;
    stats::ConditionId Conditions;
    bool WasAdjusted{ false };
    ConditionRoll Roll;
};

struct InterruptResultsComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerSpellInterruptResults, "esv::spell_cast::InterruptResultsComponent")

    HashMap<bg3se::interrupt::DamageFunctorKey, bg3se::interrupt::DamageRollAdjustments> Results;
    [[bg3::legacy(Results2)]] Array<FunctorConditional> Conditionals;
};

struct InterruptsUsedOneFrameComponent : public BaseComponent
{
    DEFINE_ONEFRAME_COMPONENT(SpellCastInterruptsUsed, "esv::spell_cast::InterruptsUsedOneFrameComponent")

    Array<stats::ActionResourceCost> Costs;
    HashMap<EntityHandle, HashMap<bg3se::interrupt::InterruptEvent, bg3se::interrupt::InterruptUsageEntry>> Interrupts;
};

END_NS()

BEGIN_NS(esv::interrupt)

struct AddRemoveRequestsComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerInterruptAddRemoveRequests, "esv::interrupt::AddRemoveRequestsComponent")

    HashMap<EntityHandle, uint8_t> Requests;
};

struct ActionRequestsComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerInterruptActionRequests, "esv::interrupt::ActionRequestsComponent")

    Array<StartRequestData> StartRequests;
    Array<StopRequestData> StopRequests;
    Array<CombatLogRequestData> CombatLogRequests;
};

struct ZoneRequestsComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerInterruptZoneRequests, "esv::interrupt::ZoneRequestsComponent")

    Array<StartRequestData> StartRequests;
    Array<StopRequestData> StopRequests;
};

struct InitialParticipantData
{
    StartRequestData StartRequest;
    HashSet<EntityHandle> Entities;
};

struct InitialParticipantsComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerInterruptInitialParticipants, "esv::interrupt::InitialParticipantsComponent")

    HashMap<EntityHandle, InitialParticipantData> Participants;
};

struct TurnOrderInZoneComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerInterruptTurnOrderInZone, "esv::interrupt::TurnOrderInZoneComponent")
    
    HashSet<EntityHandle> InZone;
};

struct DataSingletonComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerInterruptDataSingleton, "esv::interrupt::DataSingletonComponent")

    Array<bg3se::interrupt::UndecidedEventWithId> Events;
    Array<bg3se::interrupt::DelayedTargetHitInterruptEvent> TargetHitInterrupts;
};

struct UsedOneFrameComponent : public BaseComponent
{
    DEFINE_ONEFRAME_COMPONENT(ServerInterruptUsed, "esv::interrupt::UsedOneFrameComponent")

    Array<stats::ActionResourceCost> Costs;
    HashMap<EntityHandle, HashMap<bg3se::interrupt::InterruptEvent, bg3se::interrupt::InterruptUsageEntry>> Interrupts;
};

struct DecisionSystem : public BaseSystem
{
    DEFINE_SYSTEM(ServerInterruptDecision, "esv::interrupt::DecisionSystem")

    HashMap<EntityHandle, uint8_t> Decisions;
};

struct InterruptRequest
{
    bool IsRemoval{ false };
    FixedString Interrupt;
    InterruptRequestSource Source{ InterruptRequestSource::Debug };
    EntityHandle Boost;
    FixedString Spell;
};

struct RequestsSystem : public BaseSystem
{
    DEFINE_SYSTEM(ServerInterruptRequests, "esv::interrupt::RequestsSystem")

    [[bg3::hidden]] void* SpellPrototypeManager;
    HashMap<EntityHandle, Array<InterruptRequest>> Requests;
};

struct ManagementSystem : public BaseSystem
{
    DEFINE_SYSTEM(ServerInterruptManagement, "esv::interrupt::ManagementSystem")

    [[bg3::hidden]] void* InterruptPrototypeManager;
    [[bg3::hidden]] UnknownFunction field_18;
    HashSet<EntityHandle> RemoveContainers;
    HashMap<EntityHandle, bool> IsAsk;
    HashMap<EntityHandle, bool> IsEnabled;
};


END_NS()

BEGIN_NS(ecl::interrupt)

struct PlayerDecisionComponent : public BaseComponent
{
    DEFINE_COMPONENT(ClientInterruptPlayerDecision, "ecl::interrupt::PlayerDecisionComponent")

    HashMap<EntityHandle, uint8_t> Decisions;
};

END_NS()
