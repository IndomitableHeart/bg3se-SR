#pragma once

#include <GameDefinitions/BaseTypes.h>
#include <GameDefinitions/Enumerations.h>
#include <GameDefinitions/EntitySystem.h>

namespace bg3se
{
	struct ArmorComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Armor;

		int ArmorType;
		int ArmorClass;
		int AbilityModifierCap;
		uint8_t ArmorClassAbility;
		uint8_t Shield;
	};

	struct BaseHpComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::BaseHp;

		int Vitality;
		int VitalityBoost;
	};

	struct CustomStatsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CustomStats;

		Map<FixedString, int> Stats;
	};


	struct DataComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Data;

		float Weight_M;
		FixedString StatsId;
		uint32_t Flags;
		float field_24;
		uint64_t field_28;
		int StepsType;
	};

	struct ExperienceComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Experience;

		int CurrentLevelExperience;
		int NextLevelExperience;
		int TotalExperience;
		int SomeExperience;
		uint8_t field_28;
	};

	struct HealthComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Health;

		std::array<int, 14> PerDamageTypeModifiers;
		int CurrentHealth;
		int CurrentHealth_M;
		int MaxHealth;
		int MaxHealth_M;
		int field_60;
		int field_64;
		std::array<int, 14> PerDamageTypeHealthThresholds;
		bool CannotDamage_M;
	};



	struct StatsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Stats;

		struct EquipmentEntry
		{
			ItemSlot32 Slot;
			EntityHandle ItemHandle;
			uint8_t field_10;
			uint8_t field_11;
		};

		struct ClassInfo
		{
			UUID Class;
			UUID SubClass;
			int32_t Priority;
			int32_t Unknown;
		};

		std::array<int, 8> Abilities;
		int field_38;
		int field_3C;
		int field_40;
		int field_44;
		int field_48;
		int field_4C;
		std::array<int, 18> Skills;
		int32_t Size;
		AbilityId SpellCastingAbility;
		int SpellDC;
		int WeaponActionDC;
		int ProficiencyBonus;
		int field_AC;
		int MaxCarryWeight;
		uint8_t IsRanged;
		VirtualMultiHashMap<ItemSlot32, EquipmentEntry> Equipment;
		int ArmorType;
		int ArmorType_Breast;
		int Level;
		bool Flanked;
		Array<ClassInfo> Classes;
		AbilityId UnarmedAttackAbility;
		AbilityId RangedAttackAbility;
		void* field_140;
	};

	struct StatusImmunitiesComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::StatusImmunities;

		VirtualMultiHashMap<FixedString, UUID> PersonalStatusImmunities;
	};

	struct ValueComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Value;

		int Value;
		uint8_t Rarity;
		bool Unique;
	};

	struct GenderComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Gender;

		Gender Gender1;
		Gender Gender2;
	};

	struct WeaponComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Weapon;

		RefMap<AbilityId, Array<DiceValues>> Rolls;
		RefMap<AbilityId, Array<DiceValues>> Rolls2;
		float WeaponRange;
		float DamageRange;
		__int64 WeaponFunctors;
		uint32_t WeaponProperties;
		uint8_t WeaponGroup;
		AbilityId Ability;
	};

	struct BackgroundComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Background;

		UUID Background;
	};
	
	struct GodComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::God;

		UUID God;
	};


	struct LevelUpUpgrades
	{
		struct AbilityData
		{
			uint8_t field_0;
			UUID Feat;
			UUID Class;
			int field_28;
			int field_2C;
			int field_30;
			UUID Ability;
			Array<uint8_t> Array_b8;
			STDString field_60;
			int field_80;
		};
		
		struct SkillData
		{
			uint8_t field_0;
			UUID field_8;
			UUID field_18;
			int field_28;
			int field_2C;
			int field_30;
			UUID Skill;
			Array<uint8_t> Array_b8;
			STDString field_60;
			int field_80;
		};
		
		struct SpellData
		{
			struct StringPair
			{
				FixedString A;
				FixedString B;
			};

			uint8_t field_0;
			UUID Race;
			UUID field_18;
			int field_28;
			int field_2C;
			int field_30;
			UUID Spell;
			Array<FixedString> Array_FS;
			Array<StringPair> Array_FS2;
			STDString field_78;
		};
		
		struct LevelUpDataInner4
		{
			uint8_t field_0;
			UUID field_8;
			UUID Class;
			int field_28;
			int field_2C;
			int field_30;
			UUID field_38;
			Array<FixedString> Array_FS;
			STDString field_60;
			int field_80;
		};

		Array<AbilityData> Abilities;
		Array<SkillData> Skills;
		Array<SpellData> Spells;
		Array<LevelUpDataInner4> field_48;
	};

	struct LevelUpData3
	{
		FixedString field_0;
		uint8_t field_4;
		uint64_t _Pad;
		UUID field_10;
	};

	struct LevelUpData
	{
		UUID Class;
		UUID SubClass;
		UUID Feat;
		std::array<int, 7> field_30;
		LevelUpUpgrades Upgrades;
		Array<LevelUpData3> field_B0;
	};


	struct LevelUpComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::LevelUp;

		Array<LevelUpData> field_18;
	};

	struct PlayerPrepareSpellComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::PlayerPrepareSpell;

		struct Spell
		{
			FixedString field_0;
			int32_t field_4;
			uint8_t field_8;
			UUID field_10;
		};


		Array<Spell> field_18;
		uint8_t field_30;
	};

	struct CCPrepareSpellComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CCPrepareSpell;

		Array<PlayerPrepareSpellComponent::Spell> field_18;
	};

	struct SpellCastComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::SpellCast;

		FixedString field_18;
		uint8_t field_1C;
		__int64 field_20;
		int field_28;
		int field_2C;
		int field_30;
		int field_34;
		uint8_t field_38;
		uint8_t field_39;
		uint8_t field_3A;
		uint8_t field_3B;
	};

	struct FloatingComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Floating;

		int field_18;
		int field_1C;
	};

	struct VoiceComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Voice;

		UUID Voice;
	};

	struct CustomIconComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CustomIcon;

		ScratchBuffer Buffer;
	};

	struct CharacterCreationStatsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CharacterCreationStats;

		UUID Race;
		UUID SubRace;
		__int64 field_38;
		STDWString Name;
		__int64 field_60;
		uint32_t field_68;
		uint32_t field_6C;
		uint32_t field_70;
		uint32_t field_74;
		int field_78;
	};

	struct DisarmableComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Disarmable;

		int SomeTrapId;
		bool CanDisarm;
	};

	struct ShortRestComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ShortRest;

		uint8_t StateId;
	};

	struct IsSummonComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::IsSummon;

		ComponentHandle Owner_M;
	};

	struct SummonContainerComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::SummonContainer;

		VirtualMultiHashMap<FixedString, Array<EntityHandle>> field_18;
		VirtualMultiHashSet<ComponentHandle> Characters;
		VirtualMultiHashSet<ComponentHandle> Items;
	};

	struct StealthComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Stealth;

		bool SeekHiddenFlag;
		glm::vec3 Position;
		float SeekHiddenTimeout;
		int field_2C;
	};

	struct IsGlobalComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::IsGlobal;
	};

	struct SavegameComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Savegame;

		ComponentHandle field_18;
	};

	struct DisabledEquipmentComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::DisabledEquipment;

		bool ShapeshiftFlag;
	};

	struct LootingStateComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::LootingState;

		EntityHandle Looter_M;
		uint8_t State;
		int field_24;
	};

	struct LootComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Loot;

		uint8_t Flags;
		uint8_t InventoryType;
	};

	struct LockComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Lock;

		FixedString Key_M;
		int LockDC;
		bool Unlocked_M;
	};

	struct SummonLifetimeComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::SummonLifetime;

		float Lifetime;
	};

	struct InvisibilityComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Invisibility;

		uint8_t field_18;
		glm::vec3 field_1C;
		uint8_t field_28;
	};

	struct IconComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Icon;

		FixedString Icon;
	};

	struct HotbarContainerComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::HotbarContainer;

		struct Element
		{
			EntityHandle Item;
			SpellId SpellId;
			FixedString Passive;
		};

		struct Bar
		{
			Array<Element> Elements;
			uint8_t SomeFlag;
		};

		VirtualMultiHashMap<FixedString, Array<Bar>> Containers;
		FixedString ActiveContainer;
	};

	/// <summary>
	/// ///////////////////////// START SERVER
	/// </summary>


	struct RecruitedByComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerRecruitedBy;

		EntityHandle RecruitedBy;
	};

	struct GameTimerComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerGameTimer;

		FixedString field_18;
		EntityHandle field_20;
		int field_28;
		int field_2C;
		int field_30;
		int field_34;
		uint8_t field_38;
	};

	struct ExperienceGaveOutComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerExperienceGaveOut;

		int Experience;
	};


	struct ReplicationDependencyComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerReplicationDependency;

		EntityHandle Dependency;
	};

	struct IsUnsummoningComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerIsUnsummoning;
	};

	struct FleeBlockedComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerFleeBlocked;
	};

	struct ActivationGroupContainerComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerActivationGroupContainer;

		struct ActivationGroup
		{
			FixedString field_0;
			FixedString field_4;
		};

		Array<ActivationGroup> Groups;
	};

	struct AnubisTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerAnubisTag;

		Array<UUID> Tags;
	};

	struct DialogTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerDialogTag;

		Array<UUID> Tags;
	};

	struct DisplayName
	{
		TranslatedString NameKey;
		float Unknown;
		int Unknown2;
		STDWString Name;
	};

	struct DisplayNameListComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerDisplayNameList;

		Array<DisplayName> Names;
		Array<TranslatedString> TranslatedStrings;
	};

	struct IconListComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerIconList;

		struct Icon
		{
			FixedString Icon;
			uint32_t field_4;
		};

		Array<Icon> Icons;
		uint8_t field_30;
	};

	struct PlanTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerPlanTag;

		Array<UUID> Tags;
	};

	struct RaceTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerRaceTag;

		Array<UUID> Tags;
	};

	struct TemplateTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerTemplateTag;

		Array<UUID> Tags;
	};

	struct ToggledPassivesComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerToggledPassives;

		VirtualMultiHashMap<FixedString, bool> Passives;
	};

	struct BoostTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerBoostTag;

		Array<UUID> Tags;
	};

	struct TriggerStateComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerTriggerState;

		VirtualMultiHashSet<UUID> Triggers;
	};

	struct SafePositionComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerSafePosition;

		glm::vec3 Position;
		bool field_24;
	};

	struct AnubisExecutorComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerAnubisExecutor;

		__int64 field_18;
		__int64 field_20;
		uint8_t field_28;
		uint8_t field_29;
	};

	struct DetachedBoostComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerDetachedBoost;

		Array<EntityHandle> Boosts;
	};

	struct LeaderComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerLeader;

		VirtualMultiHashSet<EntityHandle> Followers_M;
	};

	struct BreadcrumbComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerBreadcrumb;

		struct Element
		{
			int field_0;
			uint8_t field_4;
			glm::vec3 field_8;
			glm::vec3 field_14;
		};

		std::array<Element, 8> field_18;
		glm::vec3 field_118;
	};


	struct DelayDeathCauseComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerDelayDeathCause;

		int DelayCount;
		int Blocked_M;
	};


	struct PickpocketComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerPickpocket;

		struct PickpocketEntry
		{
			EntityHandle field_0;
			EntityHandle field_8;
			int field_10;
			bool field_14;
			EntityHandle field_18;
		};

		Array<PickpocketEntry> field_18;
	};

	struct ReplicationDependencyOwnerComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ServerReplicationDependencyOwner;

		Array<EntityHandle> Dependents;
	};

	/// <summary>
	/// //////////////////////////////////////// END SERVER
	/// </summary>


	struct OriginTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::OriginTag;

		Array<UUID> Tags;
	};

	struct PassiveInfo
	{
		uint32_t field_0;
		uint32_t Passive;
	};

	struct OriginPassivesComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::OriginPassives;

		Array<PassiveInfo> field_18;
	};

	struct GodTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::GodTag;

		Array<UUID> Tags;
	};

	struct ClassTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ClassTag;

		Array<UUID> Tags;
	};

	struct BackgroundTagComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::BackgroundTag;

		Array<UUID> Tags;
	};

	struct BackgroundPassivesComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::BackgroundPassives;

		Array<PassiveInfo> field_18;
	};

	struct GlobalShortRestDisabledComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::GlobalShortRestDisabled;
	};

	struct GlobalLongRestDisabledComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::GlobalLongRestDisabled;
	};

	struct StoryShortRestDisabledComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::StoryShortRestDisabled;
	};

	struct FleeCapabilityComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::FleeCapability;

		uint8_t field_18;
		float field_1C;
		float field_20;
	};

	struct CanDoRestComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanDoRest;

		uint8_t field_18;
		uint8_t field_19;
		uint8_t field_1A;
		uint8_t field_1B;
	};

	struct ItemBoostsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ItemBoosts;

		Array<uint64_t> field_18;
	};

	struct ActiveCharacterLightComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ActiveCharacterLight;

		FixedString Light;
	};

	struct VisualComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Visual;

		__int64 field_18;
		uint8_t field_20;
		uint8_t field_21;
	};

	struct AnimationSetComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::AnimationSet;

		__int64 field_18;
		uint8_t field_20;
		uint8_t field_21;
		uint32_t _Pad;
		FixedString field_28;
		uint8_t field_2C;
	};

	struct AnimationBlueprintComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::AnimationBlueprint;

		uint8_t field_18;
		__int64 field_20;
		__int64 field_28;
		__int64 field_30;
		__int64 field_38;
		__int64 field_40;
		uint8_t field_48;
		uint8_t field_49;
	};

	struct CanModifyHealthComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanModifyHealth;
		
		uint16_t Flags;
	};

	struct AddedSpellsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::AddedSpells;

		struct SpellInfo
		{
			FixedString SpellId;
			UUID field_8;
			uint64_t field_18;
			EntityHandle field_20;
			uint8_t field_28;
			uint8_t field_29;
			UUID SomeUUID;
			uint8_t field_40;
			uint8_t Cooldown;
			FixedString field_44;
			uint8_t field_48;
		};

		Array<SpellInfo> Spells;
	};

	struct AvailableLevelComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::AvailableLevel;

		int Level;
	};

	struct CanBeLootedComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanBeLooted;

		uint16_t Flags;
	};

	struct CanDoActionsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanDoActions;

		uint16_t Flags;
	};

	struct CanMoveComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanMove;

		uint16_t Flags;
	};

	struct CanSenseComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanSense;

		uint16_t Flags;
	};

	struct ConcentrationComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Concentration;

		struct Element
		{
			EntityHandle field_0;
			EntityHandle field_8;
			int16_t field_10;
			bool field_12;
		};

		Array<Element> field_18;
		SpellId SpellId;
	};

	struct DarknessComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Darkness;

		uint8_t field_18;
		uint8_t field_19;
		uint8_t field_1A;
		uint8_t field_1B;
		uint8_t field_1C;
		uint8_t field_1D;
		int field_20;
	};

	struct DualWieldingComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::DualWielding;

		uint16_t field_18;
		uint8_t field_1A;
		uint16_t field_1B;
		uint8_t field_1D;
		uint8_t field_1E;
	};

	struct GameObjectVisualData
	{
		ScratchBuffer field_0;
		__int64 field_58;
		__int64 field_60;
		__int64 field_68;
		__int64 field_70;
		__int64 field_78;
		__int64 field_80;
		Array<uint64_t> field_88;
		Array<uint64_t> field_A0;
	};

	struct GameObjectVisualComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::GameObjectVisual;

		FixedString RootTemplateId;
		uint8_t RootTemplateType;
		FixedString Icon;
		float field_24;
		uint8_t field_28;
		GameObjectVisualData VisualData;
	};

	struct InventorySlotComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::InventorySlot;

		uint16_t Slot;
	};

	struct SpellBookCooldowns : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::SpellBookCooldowns;

		struct Cooldown
		{
			SpellId SpellId;
			SpellCooldownType CooldownType;
			uint8_t field_29;
			float Cooldown;
		};

		Array<Cooldown> Cooldowns;
	};

	struct DisplayNameComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::DisplayName;

		TranslatedString NameKey;
		TranslatedString UnknownKey;
		STDWString Name;
	};

	struct EquipableComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Equipable;

		UUID field_18;
		int Slot_M;
	};

	struct GameplayLightComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::GameplayLight;

		int field_18;
		uint8_t field_1C;
		int field_20;
		int field_24;
		int field_28;
	};

	struct ProgressionContainerComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ProgressionContainer;

		Array<Array<EntityHandle>> Progressions;
	};

	struct ProgressionMetaComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ProgressionMeta;

		uint8_t field_18;
		UUID Progression;
		UUID Race;
		int field_40;
		uint8_t field_44;
		int field_48;
		EntityHandle Owner;
	};


	struct RaceComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Race;

		UUID Race;
	};

	struct SightComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Sight;

		float Sight;
	};

	struct CanTravelComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanTravel;

		__int16 field_18;
		__int16 field_1A;
		__int16 field_1C;
	};

	struct CanBeInInventoryComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanBeInInventory;

		uint16_t Flags;
	};

	struct MovementComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Movement;

		float field_18;
		float field_1C;
		float field_20;
		float field_24;
		int field_28;
		int field_2C;
	};

	struct ObjectInteractionComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ObjectInteraction;

		Array<ComponentHandle> field_18;
	};

	struct StaticPhysicsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::StaticPhysics;

		FixedString field_18;
		__int64 field_20;
		__int64 field_28;
		__int16 field_30;
	};

	struct PathingComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Pathing;

		RefMap<uint64_t, uint64_t> field_18;
		RefMap<uint64_t, uint64_t> field_28;
		__int64 field_38;
		int field_40;
		FixedString CurveUUID;
		__int64 field_48;
		__int64 field_50;
		uint8_t field_58;
		uint8_t field_59;
		uint8_t field_5A;
		uint8_t field_5B;
		int field_5C;
	};

	struct SteeringComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Steering;

		float field_18;
		float field_1C;
		uint8_t field_20;
		FixedString field_24;
		float field_28;
		float field_2C;
		float field_30;
		int field_34;
		int field_38;
		int field_3C;
		float field_40;
		float field_44;
		float field_48;
		float field_4C;
		float field_50;
	};

	struct CanDeflectProjectilesComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CanDeflectProjectiles;

		uint16_t Flags;
	};

	struct LearnedSpells : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::LearnedSpells;

		VirtualMultiHashMap<UUID, VirtualMultiHashSet<FixedString>> field_18;
		VirtualMultiHashSet<uint8_t> field_70;
	};

	struct SpellAiConditions : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::SpellAiConditions;

		VirtualMultiHashMap<FixedString, uint64_t> field_18;
	};

	struct ActiveSkeletonSlotsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ActiveSkeletonSlots;

		Array<FixedString> Slots;
	};

	struct NetComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Net;

		FixedString field_18;
		NetId NetID;
	};

	struct PhysicsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Physics;

		__int64 field_18;
	};

	struct SomeSharedServerClientObjId
	{
		__int64 field_0;
		int field_8;
	};


	struct SomeSharedServerClientObjId2 : public SomeSharedServerClientObjId
	{
		__int64 field_10;
		int field_18;
	};


	struct AnubisComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::Anubis;

		struct Inner
		{
			__int64 field_0;
			__int64 field_8;
			__int64 field_10;
			__int64 field_18;
			__int64 field_20;
			__int64 field_28;
			__int64 field_30;
			__int64 field_38;
			__int64 field_40;
			__int64 field_48;
			__int64 field_50;
			__int64 field_58;
			uint8_t TypeId;
		};

		STDString field_18;
		// FIXME - need remapping
		/*int field_38;
		Array<uint64_t> field_40;
		uint8_t field_58;
		Inner field_60;
		uint8_t field_C8;
		__int64 field_D0;
		__int64 field_D8;
		SomeSharedServerClientObjId2 field_E0;*/
	};

	struct FTBParticipantComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::FTBParticipant;

		EntityHandle field_18;
	};


	struct UnsheathInfoComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::UnsheathInfo;

		uint8_t field_18;
		uint8_t field_19;
		uint8_t field_1A;
		uint8_t field_1B;
	};


	struct ApprovalRatingsComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::ApprovalRatings;

		VirtualMultiHashMap<EntityHandle, int> Ratings;
		VirtualMultiHashSet<UUID> field_70;
	};


	struct CharacterCreationAppearanceComponent : public BaseComponent
	{
		static constexpr ExtComponentType ComponentType = ExtComponentType::CharacterCreationAppearance;

		struct AppearanceElement
		{
			UUID Material;
			UUID Color;
			float field_20;
		};

		Array<UUID> Visuals;
		Array<AppearanceElement> Elements;
		UUID SkinColor;
		UUID EyeColor;
		UUID field_68;
	};

}
