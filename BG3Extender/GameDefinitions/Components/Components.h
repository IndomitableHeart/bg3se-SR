#pragma once

#include <GameDefinitions/Base/Base.h>
#include <GameDefinitions/EntitySystem.h>

BEGIN_SE()

struct RulesetComponent : public BaseComponent
{
    DEFINE_COMPONENT(Ruleset, "eoc::ruleset::RulesetComponent")

    Array<Guid> Rulesets;
    // FIXME: First variant is void
    HashMap<Guid, std::variant<uint8_t, int32_t, float, FixedString, bool>> Modifiers;
    [[bg3::hidden]] uint64_t field_40[2];
};

struct RulesetModifiersComponent : public BaseComponent
{
    DEFINE_COMPONENT(RulesetModifiers, "eoc::ruleset::RulesetModifiersComponent")

    // FIXME: First variant is void
    HashMap<Guid, std::variant<uint8_t, int32_t, float, FixedString, bool>> Modifiers;
};

struct SurfacePathInfluencesComponent : public BaseComponent
{
    DEFINE_COMPONENT(SurfacePathInfluences, "eoc::SurfacePathInfluencesComponent")

    Array<SurfacePathInfluence> PathInfluences;
};

struct WieldingComponent : public BaseComponent
{
    DEFINE_COMPONENT(Wielding, "eoc::WieldingComponent")

    EntityHandle Owner;
};

struct TagComponent : public BaseComponent
{
    DEFINE_COMPONENT(Tag, "eoc::TagComponent")

    Array<Guid> Tags;
};

struct OsirisTagComponent : public BaseComponent
{
    DEFINE_COMPONENT(ServerOsirisTag, "esv::tags::OsirisTagComponent")

    Array<Guid> Tags;
};

struct RelationId
{
    Guid field_0;
    EntityHandle field_10;

    inline bool operator == (RelationId const& o) const
    {
        return field_0 == o.field_0
            && field_10 == o.field_10;
    }
};

struct RelationComponent : public BaseComponent
{
    DEFINE_COMPONENT(Relation, "eoc::relation::RelationComponent")

    HashMap<uint32_t, uint8_t> field_0;
    HashMap<uint32_t, uint8_t> field_40;
    HashMap<uint32_t, uint8_t> field_80;
    HashMap<RelationId, uint8_t> field_C0;
    HashMap<uint32_t, uint8_t> field_100;
    HashSet<uint32_t> field_140;
    HashSet<uint32_t> field_170;
};

struct FactionComponent : public BaseComponent
{
    DEFINE_COMPONENT(Faction, "eoc::relation::FactionComponent")

    EntityHandle field_0;
    Guid field_8;
    Guid field_18;
    [[bg3::legacy(field_28)]] EntityHandle SummonOwner;
};


template <>
inline uint64_t HashMapHash<RelationId>(RelationId const& v)
{
    return HashMulti(v.field_0, v.field_10);
}

struct CanInteractComponent : public BaseComponent
{
    DEFINE_COMPONENT(CanInteract, "eoc::CanInteractComponent")

    CanInteractFlags Flags;
    uint16_t Flags2;
};

struct CanSpeakComponent : public BaseComponent
{
    DEFINE_COMPONENT(CanSpeak, "eoc::CanSpeakComponent")

    uint16_t Flags;
};

struct OriginComponent : public BaseComponent
{
    DEFINE_COMPONENT(Origin, "eoc::OriginComponent")

    Guid field_18;
    FixedString Origin;
};

struct ActiveComponent : public BaseComponent
{
    DEFINE_COMPONENT(Active, "eoc::ActiveComponent")
};

struct MaterialParameterOverride
{
    STDString field_0;
    FixedString field_18;
};

struct MaterialParameterOverrideComponent : public BaseComponent
{
    DEFINE_COMPONENT(MaterialParameterOverride, "eoc::MaterialParameterOverrideComponent")

    Array<Guid> field_0;
    Array<MaterialParameterOverride> field_10;
};

DEFINE_TAG_COMPONENT(eoc, OffStageComponent, OffStage)
DEFINE_TAG_COMPONENT(eoc, PickingStateComponent, PickingState)
DEFINE_TAG_COMPONENT(eoc, PlayerComponent, Player)
DEFINE_TAG_COMPONENT(eoc, SimpleCharacterComponent, SimpleCharacter)
DEFINE_TAG_COMPONENT(eoc, WeaponSetComponent, WeaponSet)
DEFINE_TAG_COMPONENT(eoc::character, CharacterComponent, IsCharacter)

struct SpeakerComponent : public BaseComponent
{
    DEFINE_COMPONENT(Speaker, "eoc::SpeakerComponent")

    TrackedCompactSet<FixedString> field_0;
};


struct CanEnterChasmComponent : public BaseComponent
{
    DEFINE_COMPONENT(CanEnterChasm, "eoc::chasm::CanEnterChasmComponent")

    bool CanEnter;
};


struct DeadByDefaultComponent : public BaseComponent
{
    DEFINE_COMPONENT(DeadByDefault, "eoc::death::DeadByDefaultComponent")

    uint8_t DeadByDefault;
};


struct DeathComponent : public BaseComponent
{
    DEFINE_COMPONENT(Death, "eoc::death::DeathComponent")

    EntityHandle Target;
    EntityHandle field_8;
    Guid field_10;
    uint8_t DeathType;
    uint8_t field_21;
    uint8_t field_22;
    uint8_t field_23;
    uint8_t field_24;
    int field_28;
    uint8_t field_2C;
    glm::vec3 field_30;
    glm::vec3 field_3C;
    uint8_t field_48;
    int field_4C;
    Guid Combat;
    uint8_t field_60;
    uint8_t field_61;
    uint8_t field_62;
    bool LeaveTrace;
    std::optional<glm::vec3> field_64;
};


struct DeathStateComponent : public BaseComponent
{
    DEFINE_COMPONENT(DeathState, "eoc::death::StateComponent")

    uint32_t State;
};


struct DialogStateComponent : public BaseComponent
{
    DEFINE_COMPONENT(DialogState, "eoc::dialog::StateComponent")

    uint8_t field_0;
    uint8_t field_1;
    uint8_t field_2;
    [[bg3::legacy(field_4)]] int DialogId;
    uint8_t field_8;
};


struct IdentityComponent : public BaseComponent
{
    DEFINE_COMPONENT(Identity, "eoc::identity::IdentityComponent")

    uint8_t field_0;
};


struct IdentityStateComponent : public BaseComponent
{
    DEFINE_COMPONENT(IdentityState, "eoc::identity::StateComponent")

    [[bg3::legacy(field_0)]] bool Disguised;
};


struct OriginalIdentityComponent : public BaseComponent
{
    DEFINE_COMPONENT(OriginalIdentity, "eoc::identity::OriginalIdentityComponent")

    uint8_t field_0;
};


struct EquipmentVisualComponent : public BaseComponent
{
    DEFINE_COMPONENT(EquipmentVisual, "eoc::character::EquipmentVisualComponent")

    uint8_t State;
};


struct ReposeStateComponent : public BaseComponent
{
    DEFINE_COMPONENT(Repose, "eoc::repose::StateComponent")

    EntityHandle field_0;
    Guid field_8;
    int field_18;
    glm::vec3 field_1C;
    uint8_t field_28;
};


struct OriginalTemplateComponent : public BaseComponent
{
    DEFINE_COMPONENT(OriginalTemplate, "eoc::templates::OriginalTemplateComponent")

    FixedString OriginalTemplate;
    uint8_t TemplateType;
};


DEFINE_TAG_COMPONENT(eoc::through, CanSeeThroughComponent, CanSeeThrough)
DEFINE_TAG_COMPONENT(eoc::through, CanShootThroughComponent, CanShootThrough)
DEFINE_TAG_COMPONENT(eoc::through, CanWalkThroughComponent, CanWalkThrough)


struct ShootThroughTypeComponent : public BaseComponent
{
    DEFINE_COMPONENT(ShootThroughType, "eoc::through::ShootThroughTypeComponent")

    uint8_t Type;
};


struct UnsheathStateComponent : public BaseComponent
{
    DEFINE_COMPONENT(Unsheath, "eoc::unsheath::StateComponent")

    [[bg3::legacy(field_0)]] EntityHandle MainHandWeapon;
    [[bg3::legacy(field_8)]] EntityHandle OffHandWeapon;
    int field_10;
    [[bg3::legacy(field_14)]] stats::UnsheathState State;
    uint8_t field_18;
    uint8_t field_19;
    uint8_t field_1A;
};

struct UseSocket
{
    int field_0;
    uint8_t field_4;
    uint8_t field_5;
    std::array<float, 10> field_8;
    std::array<float, 10> field_30;
    FixedString field_58;
    FixedString field_5C;
    EntityHandle field_60;
};


struct UseSocketComponent : public BaseComponent
{
    DEFINE_COMPONENT(UseSocket, "eoc::use::SocketComponent")

    HashMap<uint32_t, UseSocket> Sockets;
};


struct UserAvatarComponent : public BaseComponent
{
    DEFINE_COMPONENT(UserAvatar, "eoc::user::AvatarComponent")

    int UserID;
    [[bg3::legacy(field_4)]] FixedString OwnerProfileID;
    uint8_t field_8;
};


struct UserReservedForComponent : public BaseComponent
{
    DEFINE_COMPONENT(UserReservedFor, "eoc::user::ReservedForComponent")

    int UserID;
};

struct ShapeshiftStateInner
{
    FixedString field_0;
    uint32_t field_4;
};

struct ShapeshiftStateComponent : public BaseComponent
{
    DEFINE_COMPONENT(ShapeshiftState, "eoc::shapeshift::StateComponent")

    std::optional<uint8_t> BloodSurfaceType;
    std::optional<FixedString> BloodType;
    std::optional<ShapeshiftStateInner> field_C;
};

struct SoundComponent : public BaseComponent
{
    DEFINE_COMPONENT(Sound, "ls::SoundComponent")

    EntityHandle Entity;
    uint64_t SoundObjectId;
    uint32_t Flags;
    float field_14;
    FixedString field_18;
    int field_1C;
};


END_SE()

BEGIN_SE()

struct UuidComponent : public BaseComponent
{
    DEFINE_COMPONENT(Uuid, "ls::uuid::Component")

    Guid EntityUuid;
};
    
struct UuidToHandleMappingComponent : public BaseComponent
{
    DEFINE_COMPONENT(UuidToHandleMapping, "ls::uuid::ToHandleMappingComponent")

    HashMap<Guid, EntityHandle> Mappings;
};

struct LevelComponent : public BaseComponent
{
    DEFINE_COMPONENT(Level, "ls::LevelComponent")

    EntityHandle field_0;
    FixedString LevelName;
};
    
struct TransformComponent : public BaseComponent
{
    DEFINE_COMPONENT(Transform, "ls::TransformComponent")

    Transform Transform;
};

struct TimeFactorComponent : public BaseComponent
{
    DEFINE_COMPONENT(TimeFactor, "ls::TimeFactorComponent")

    float Value{ 1.0f };
};

END_SE()

BEGIN_NS(sight)

struct SightBaseComponent : public BaseComponent
{
    DEFINE_COMPONENT(Sight, "eoc::sight::BaseComponent")

    [[bg3::legacy(field_0)]] float Sight;
    [[bg3::legacy(field_4)]] float DarkvisionRange;
    [[bg3::legacy(field_8)]] float FOV;
    [[bg3::legacy(field_C)]] float VerticalFOV;
};


struct DataComponent : public BaseComponent
{
    DEFINE_COMPONENT(SightData, "eoc::sight::DataComponent")

    Guid field_0;
    [[bg3::legacy(field_10)]] float DarkvisionRange;
    [[bg3::legacy(field_14)]] float FOV;
    [[bg3::legacy(field_18)]] float VerticalFOV;
    [[bg3::legacy(field_1C)]] float Sight;
    int field_20;
    int field_24;
};


struct EntityViewshedComponent : public BaseComponent
{
    DEFINE_COMPONENT(SightEntityViewshed, "eoc::sight::EntityViewshedComponent")

    HashSet<Guid> field_0;
};


END_NS()
