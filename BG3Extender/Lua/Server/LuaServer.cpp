#include <stdafx.h>
#include <Lua/Shared/LuaStats.h>
#include <Lua/Server/LuaBindingServer.h>
#include <Lua/LuaSerializers.h>
#include <OsirisProxy.h>
#include <PropertyMaps.h>
#include <NodeHooks.h>
#include <ScriptHelpers.h>
#include "resource.h"
#include <GameDefinitions/Components/Components.h>
#include <GameDefinitions/Components/Combat.h>
#include <GameDefinitions/Components/Passives.h>
#include <GameDefinitions/Components/Stats.h>
#include <GameDefinitions/Components/Boosts.h>
#include <GameDefinitions/ResourceDefinitions.h>

#include <Lua/LevelIteratorFunctions.inl>
#include <Lua/Shared/LuaShared.inl>
#include <Lua/Server/ServerCharacter.inl>
#include <Lua/Server/ServerItem.inl>
#include <Lua/Server/ServerStatus.inl>
#include <Lua/Server/ServerProjectile.inl>
#include <Lua/Server/ServerSurface.inl>
#include <Lua/Server/ServerCombat.inl>
#include <Lua/Server/ServerNetwork.inl>
#include <Lua/Server/ServerOsirisListeners.inl>



namespace bg3se::esv::lua
{
	using namespace bg3se::lua;


	/*
	* 
	int GetAiGrid(lua_State* L)
	{
		auto level = GetStaticSymbols().GetCurrentServerLevel();
		if (!level || !level->AiGrid) {
			OsiError("Current level not available yet!");
			return 0;
		}

		ObjectProxy<eoc::AiGrid>::New(L, level->AiGrid);
		return 1;
	}

	int ExecuteSkillPropertiesOnTarget(lua_State* L)
	{
		StackCheck _(L, 0);
		auto skillId = checked_get<FixedString>(L, 1);
		auto attacker = GetCharacter(L, 2);
		auto target = GetCharacter(L, 3);
		auto position = checked_get<glm::vec3>(L, 4);
		auto propertyContext = checked_get<CRPGStats_Object_PropertyContext>(L, 5);
		auto isFromItem = checked_get<bool>(L, 6);

		SkillPrototype* skillProto{ nullptr };
		auto skillProtoMgr = GetStaticSymbols().eoc__SkillPrototypeManager;
		if (skillProtoMgr && *skillProtoMgr) {
			auto proto = (*skillProtoMgr)->Prototypes.Find(skillId);
			if (*proto) {
				skillProto = *proto;
			}
		}

		if (!skillProto) {
			LuaError("Couldn't find skill prototype for " << skillId);
			return 0;
		}

		auto exec = GetStaticSymbols().esv__ExecuteCharacterSetExtraProperties;
		auto skillProperties = skillProto->GetStats()->PropertyList.Find(GFS.strSkillProperties);

		if (!skillProperties) {
			LuaError("Skill " << skillId << " has no SkillProperties!");
			return 0;
		}

		if (!attacker || !target || !exec) {
			return 0;
		}

		ObjectHandle attackerHandle;
		attacker->GetObjectHandle(attackerHandle);
		ObjectSet<esv::Character*> targets;
		targets.Add(target);

		exec(*skillProperties, attackerHandle.Handle, targets, position, propertyContext, isFromItem,
			skillProto, nullptr, 0.0f, nullptr, false, 2.4f);
		return 0;
	}

	int ExecuteSkillPropertiesOnPosition(lua_State* L)
	{
		StackCheck _(L, 0);
		auto skillId = checked_get<FixedString>(L, 1);
		auto attacker = GetCharacter(L, 2);
		auto position = checked_get<glm::vec3>(L, 3);
		auto radius = checked_get<float>(L, 4);
		auto propertyContext = checked_get<CRPGStats_Object_PropertyContext>(L, 5);
		auto isFromItem = checked_get<bool>(L, 6);

		SkillPrototype* skillProto{ nullptr };
		auto skillProtoMgr = GetStaticSymbols().eoc__SkillPrototypeManager;
		if (skillProtoMgr && *skillProtoMgr) {
			auto proto = (*skillProtoMgr)->Prototypes.Find(skillId);
			if (*proto) {
				skillProto = *proto;
			}
		}

		if (!skillProto) {
			LuaError("Couldn't find skill prototype for " << skillId);
			return 0;
		}

		auto exec = GetStaticSymbols().esv__ExecutePropertyDataOnPositionOnly;
		auto skillProperties = skillProto->GetStats()->PropertyList.Find(GFS.strSkillProperties);

		if (!skillProperties) {
			LuaError("Skill " << skillId << " has no SkillProperties!");
			return 0;
		}

		if (!attacker || !exec) {
			return 0;
		}

		ObjectHandle attackerHandle;
		attacker->GetObjectHandle(attackerHandle);

		exec(*skillProperties, attackerHandle.Handle, &position, radius, propertyContext, isFromItem, skillProto, nullptr);
		return 0;
	}

	int OsirisIsCallable(lua_State* L)
	{
		StackCheck _(L, 1);
		LuaServerPin lua(ExtensionState::Get());
		bool allowed = gOsirisProxy->IsStoryLoaded()
			&& ((lua->RestrictionFlags & State::RestrictOsiris) == 0);
		push(L, allowed);
		return 1;
	}



	int GetGameState(lua_State* L)
	{
		auto state = GetStaticSymbols().GetServerState();
		if (state) {
			push(L, *state);
		} else {
			lua_pushnil(L);
		}

		return 1;
	}

	*/

	int XDumpBoosts(lua_State* L)
	{
		auto s = checked_get<FixedString>(L, 1);

		auto& helpers = gOsirisProxy->GetServerEntityHelpers();
		auto character = helpers.GetComponent<esv::Character>(s);
		if (character) {
			auto boostContainer = helpers.GetEntityComponent<BoostsContainerComponent>(character->Base.Entity);

			if (boostContainer) {
				std::cout << "Boosts:" << std::endl;
				for (auto const& boostSet : boostContainer->Boosts) {
					for (auto boostHandle : boostSet.Value) {
						auto boost = helpers.GetEntityComponent<BoostInfoComponent>(boostHandle);
						std::cout << EnumInfo<BoostType>::Find(boost->Type)
							<< " Flags=" << boost->Flags_M
							<< " Passive=" << boost->Passive.GetString()
							<< " Arg1=" << boost->Params.field_0.GetString()
							<< " Arg2=" << boost->Params.field_8
							<< " Arg3=" << boost->Params.field_28
							<< std::endl;

					}
				}
			}
		}

		return 0;
	}

	int XGetResource(lua_State* L)
	{
		auto s = checked_get<FixedString>(L, 1);

		auto rsrc = GetStaticSymbols().ResourceDefns;
		if (rsrc && *rsrc) {
			auto const& mgrs = (*rsrc)->DefinitionManagers;
			auto mgr = mgrs[0];
		}

		return 0;
	}

	int XGetTag(lua_State* L)
	{
		auto s = checked_get<UUID>(L, 1);

		auto rsrc = gOsirisProxy->GetServerEntityHelpers().GetResourceManager<TagResource>();
		if (rsrc) {

			auto tag = reinterpret_cast<TagResource*>((*rsrc)->Resources.Values);
			std::cout << tag->Name.GetString() << std::endl;
			std::cout << tag->Icon.GetString() << std::endl;

			auto r2 = (*rsrc)->Resources.Find(s);
			if (r2) {
				std::cout << (*r2)->Name.GetString() << std::endl;
				std::cout << (*r2)->Description << std::endl;
				std::cout << (*r2)->Icon.GetString() << std::endl;
			}
		}

		return 0;
	}

	int XGetByGuid(lua_State* L)
	{
		auto s = checked_get<FixedString>(L, 1);
		auto& types = GetEoCServer()->EntityWorld->Components.Types;
		for (auto i = 0; i < types.Size; i++) {
			auto pool = types[i].Pool;
			if (pool) {
				auto comp = pool->FindComponentByGuid(s);
				if (comp) {
					auto ch = reinterpret_cast<esv::Character*>(comp);
					auto ch2 = reinterpret_cast<esv::Character*>((uint64_t)comp - 8);
					std::cout << "Found in pool " << i << std::endl;

					auto entity = ch2->Base.Entity;

					auto& helpers = gOsirisProxy->GetServerEntityHelpers();
					auto ar = helpers.GetEntityComponent<ActionResourcesComponent>(entity);
					auto armor = helpers.GetEntityComponent<ArmorComponent>(entity);
					auto baseHp = helpers.GetEntityComponent<BaseHpComponent>(entity);
					auto data = helpers.GetEntityComponent<DataComponent>(entity);
					auto exp = helpers.GetEntityComponent<ExperienceComponent>(entity);
					auto health = helpers.GetEntityComponent<HealthComponent>(entity);
					auto passive = helpers.GetEntityComponent<PassiveComponent>(entity);
					auto sense = helpers.GetEntityComponent<SenseComponent>(entity);
					auto spells = helpers.GetEntityComponent<SpellBookComponent>(entity);
					auto stats = helpers.GetEntityComponent<StatsComponent>(entity);
					auto immun = helpers.GetEntityComponent<StatusImmunitiesComponent>(entity);
					auto spi = helpers.GetEntityComponent<SurfacePathInfluencesComponent>(entity);
					auto use = helpers.GetEntityComponent<UseComponent>(entity);
					auto value = helpers.GetEntityComponent<ValueComponent>(entity);
					auto weapon = helpers.GetEntityComponent<WeaponComponent>(entity);
					auto wielding = helpers.GetEntityComponent<WieldingComponent>(entity);
					auto customStats = helpers.GetEntityComponent<CustomStatsComponent>(entity);

					//auto bc = helpers.GetEntityComponent<BoostConditionComponent>(entity);
					auto bcn = helpers.GetEntityComponent<BoostsContainerComponent>(entity);
					auto arm = helpers.GetEntityComponent<ActionResourceConsumeMultiplierBoost>(entity);
					auto cp = helpers.GetEntityComponent<CombatParticipantComponent>(entity);
					auto gc = helpers.GetEntityComponent<GenderComponent>(entity);
					auto sc = helpers.GetEntityComponent<SpellContainerComponent>(entity);
					auto t = helpers.GetEntityComponent<TagComponent>(entity);
					auto ot = helpers.GetEntityComponent<OsirisTagComponent>(entity);
					auto sbp = helpers.GetEntityComponent<SpellBookPrepares>(entity);
					auto cs = helpers.GetEntityComponent<CombatStateComponent>(entity);
					auto tb = helpers.GetEntityComponent<TurnBasedComponent>(entity);
					auto to = helpers.GetEntityComponent<TurnOrderComponent>(entity);
					auto tf = helpers.GetEntityComponent<TransformComponent>(entity);
					auto pc = helpers.GetEntityComponent<PassiveContainerComponent>(entity);

					if (bcn) {
						for (auto const& boostSet : bcn->Boosts) {
							for (auto handle : boostSet.Value) {
								auto bi = helpers.GetEntityComponent<BoostInfoComponent>(handle);
								auto f2c = bi->Passive.GetString();
								auto p2 = bi->Params.field_0.GetString();
								auto ac = helpers.GetEntityComponent<ArmorClassBoost>(handle);
								auto ab = helpers.GetEntityComponent<AbilityBoost>(handle);
								auto rb = helpers.GetEntityComponent<RollBonusBoost>(handle);
								auto ad = helpers.GetEntityComponent<AdvantageBoost>(handle);
								auto ar = helpers.GetEntityComponent<ActionResourceValueBoost>(handle);
								auto ch = helpers.GetEntityComponent<CriticalHitBoost>(handle);
								auto rbb = helpers.GetEntityComponent<ResistanceBoost>(handle);

							}
						}
					}

					return 0;
				}
			}
		}

		std::cout << "NOTFOUND!" << std::endl;
		return 0;
	}

	int XGetByHandle(lua_State* L)
	{
		auto s = checked_get<ObjectHandle>(L, 1);
		auto& types = GetEoCServer()->EntityWorld->Components.Types;
		for (auto i = 0; i < types.Size; i++) {
			auto pool = types[i].Pool;
			if (pool) {
				auto comp = pool->FindComponentByHandle(s);
				if (comp) {
					std::cout << "Found in pool " << i << std::endl;
				}
			}
		}

		return 0;
	}

	int XTest(lua_State* L)
	{
		auto stats = GetStaticSymbols().Stats;
		auto const& functors = (*stats)->StatsFunctors;
		DealDamageFunctor* dd{ nullptr };

		functors.Iterate([&dd](FixedString const& key, StatsFunctorSet* funcs) {
			DEBUG("asdf");
			for (auto const& fun : funcs->FunctorList) {
				if (fun->TypeId == StatsFunctorActionId::DealDamage && fun->Cast<DealDamageFunctor>()) {
					dd = *fun->Cast<DealDamageFunctor>();
				}
			}
		});

		if (dd) {
			ObjectProxy2<DealDamageFunctor>::New(L, dd);
			return 1;
		}

		return 0;
	}

	int NewCall(lua_State* L);
	int NewQuery(lua_State* L);
	int NewEvent(lua_State* L);

	void RegisterOsirisLibrary(lua_State* L)
	{
		static const luaL_Reg extLib[] = {
			{"NewCall", NewCall},
			{"NewQuery", NewQuery},
			{"NewEvent", NewEvent},
			{"RegisterListener", RegisterOsirisListener},
			{0,0}
		};

		lua_getglobal(L, "Ext"); // stack: Ext
		luaL_newlib(L, extLib); // stack: ext, lib
		lua_setfield(L, -2, "Osiris");
		lua_pop(L, 1);
	}

	void ExtensionLibraryServer::RegisterLib(lua_State * L)
	{
		static const luaL_Reg extLib[] = {

			{"XGetByGuid", XGetByGuid},
			{"XGetByHandle", XGetByHandle},
			{"XDumpBoosts", XDumpBoosts},
			{"XGetResource", XGetResource},
			{"XGetTag", XGetTag},
			{"XTest", XTest},

			/*{"GetSurfaceTransformRules", GetSurfaceTransformRules},
			{"UpdateSurfaceTransformRules", UpdateSurfaceTransformRules},
			{"CreateSurfaceAction", CreateSurfaceAction},
			{"ExecuteSurfaceAction", ExecuteSurfaceAction},
			{"CancelSurfaceAction", CancelSurfaceAction},*/

			{"GetAllCharacters", GetAllCharacters},
			{"GetCharactersAroundPosition", GetCharactersAroundPosition},
			/*{"GetAllItems", GetAllItems},
			{"GetItemsAroundPosition", GetItemsAroundPosition},
			{"CreateItemConstructor", CreateItemConstructor},
			*/
			{"GetCharacter", GetCharacter},
			{"GetItem", GetItem},
			{"GetGameObject", GetGameObject},
			/*{"GetStatus", GetStatus},
			{"GetCombat", GetCombat},
			{"GetSurface", GetSurface},
			{"GetAiGrid", GetAiGrid},
			{"NewDamageList", NewDamageList},
			{"GetSurfaceTemplate", GetSurfaceTemplate},
			{"OsirisIsCallable", OsirisIsCallable},*/

			/*{"BroadcastMessage", BroadcastMessage},
			{"PostMessageToClient", PostMessageToClient},
			{"PostMessageToUser", PostMessageToUser},
			{"PlayerHasExtender", PlayerHasExtender},*/

			{0,0}
		};

		luaL_newlib(L, extLib); // stack: lib
		lua_setglobal(L, "Ext"); // stack: -

		RegisterSharedLibraries(L);
		RegisterOsirisLibrary(L);
	}

	void ExtensionLibraryServer::Register(lua_State * L)
	{
		ExtensionLibrary::Register(L);

		/*auto & pm = StaticLuaPropertyMap<DealDamageFunctor>::PropertyMap;
		pm.AddProperty("DamageType", 
			[](lua_State* L, DealDamageFunctor* obj) {
				return LuaWrite(L, obj->Arg1_DamageType) == 1;
			},
			[](lua_State* L, DealDamageFunctor* obj, int index) {
				lua_pushvalue(L, index);
				LuaRead(L, obj->Arg1_DamageType);
				lua_pop(L, 1);
				return true;
			}
		);*/

		/*ObjectProxy<esv::Status>::RegisterMetatable(L);*/
		ObjectProxy<esv::Character>::RegisterMetatable(L);
		ObjectProxy<esv::Item>::RegisterMetatable(L);
		/*ObjectProxy<eoc::ItemDefinition>::RegisterMetatable(L);*/
		//ObjectProxy<esv::Projectile>::RegisterMetatable(L);
		/*ObjectProxy<esv::ShootProjectileHelper>::RegisterMetatable(L);
		ObjectProxy<esv::SurfaceAction>::RegisterMetatable(L);*/

		OsiFunctionNameProxy::RegisterMetatable(L);
		/*StatusHandleProxy::RegisterMetatable(L);
		TurnManagerCombatProxy::RegisterMetatable(L);
		TurnManagerTeamProxy::RegisterMetatable(L);
		ItemConstructor::RegisterMetatable(L);*/
		RegisterNameResolverMetatable(L);
		CreateNameResolver(L);
	}


	ServerState::ServerState(ExtensionState& state)
		: identityAdapters_(gOsirisProxy->GetGlobals()),
		osirisCallbacks_(state)
	{
		StackCheck _(L, 0);
		identityAdapters_.UpdateAdapters();

		library_.Register(L);

		auto baseLib = GetBuiltinLibrary(IDR_LUA_BUILTIN_LIBRARY);
		LoadScript(baseLib, "BuiltinLibrary.lua");
		auto serverLib = GetBuiltinLibrary(IDR_LUA_BUILTIN_LIBRARY_SERVER);
		LoadScript(serverLib, "BuiltinLibraryServer.lua");
		auto gameMathLib = GetBuiltinLibrary(IDR_LUA_GAME_MATH);
		LoadScript(gameMathLib, "Game.Math.lua");
		auto gameTooltipLib = GetBuiltinLibrary(IDR_LUA_GAME_TOOLTIP);
		LoadScript(gameTooltipLib, "Game.Tooltip.lua");

		lua_getglobal(L, "Ext"); // stack: Ext
		stats::StatsExtraDataProxy::New(L); // stack: Ext, "ExtraData", ExtraDataProxy
		lua_setfield(L, -2, "ExtraData"); // stack: Ext
		lua_pop(L, 1); // stack: -
		
		// Ext is not writeable after loading SandboxStartup!
		auto sandbox = GetBuiltinLibrary(IDR_LUA_SANDBOX_STARTUP);
		LoadScript(sandbox, "SandboxStartup.lua");

#if !defined(OSI_NO_DEBUGGER)
		auto debugger = gOsirisProxy->GetLuaDebugger();
		if (debugger) {
			debugger->ServerStateCreated(this);
		}
#endif
	}

	ServerState::~ServerState()
	{
		if (gOsirisProxy) {
#if !defined(OSI_NO_DEBUGGER)
			auto debugger = gOsirisProxy->GetLuaDebugger();
			if (debugger) {
				debugger->ServerStateDeleted();
			}
#endif

			gOsirisProxy->GetCustomFunctionManager().ClearDynamicEntries();
		}
	}


	/*std::optional<int32_t> ServerState::StatusGetEnterChance(esv::Status * status, bool isEnterCheck)
	{
		Restriction restriction(*this, RestrictOsiris);

		PushInternalFunction(L, "_StatusGetEnterChance"); // stack: fn
		auto _{ PushArguments(L,
			std::tuple{Push<ObjectProxy<esv::Status>>(status)}) };
		push(L, isEnterCheck);

		auto result = CheckedCall<std::optional<int32_t>>(L, 2, "Ext.StatusGetEnterChance");
		if (result) {
			return std::get<0>(*result);
		} else {
			return {};
		}
	}


	void PushHit(lua_State* L, HitDamageInfo const& hit)
	{
		lua_newtable(L);
		setfield(L, "Equipment", hit.Equipment);
		setfield(L, "TotalDamageDone", hit.TotalDamage);
		setfield(L, "DamageDealt", hit.DamageDealt);
		setfield(L, "DeathType", hit.DeathType);
		setfield(L, "DamageType", hit.DamageType);
		setfield(L, "AttackDirection", hit.AttackDirection);
		setfield(L, "ArmorAbsorption", hit.ArmorAbsorption);
		setfield(L, "LifeSteal", hit.LifeSteal);
		setfield(L, "EffectFlags", (int64_t)hit.EffectFlags);
		setfield(L, "HitWithWeapon", hit.HitWithWeapon);

		auto luaDamageList = DamageList::New(L);
		for (auto const& dmg : hit.DamageList) {
			luaDamageList->Get().SafeAdd(dmg);
		}
		
		lua_setfield(L, -2, "DamageList");
	}

	bool PopHit(lua_State* L, HitDamageInfo& hit, int index)
	{
		luaL_checktype(L, index, LUA_TTABLE);
		hit.Equipment = checked_getfield<uint32_t>(L, "Equipment", index);
		hit.TotalDamage = checked_getfield<int32_t>(L, "TotalDamageDone", index);
		hit.DamageDealt = checked_getfield<int32_t>(L, "DamageDealt", index);
		hit.DeathType = checked_getfield<DeathType>(L, "DeathType", index);
		hit.DamageType = checked_getfield<DamageType>(L, "DamageType", index);
		hit.AttackDirection = checked_getfield<uint32_t>(L, "AttackDirection", index);
		hit.ArmorAbsorption = checked_getfield<int32_t>(L, "ArmorAbsorption", index);
		hit.LifeSteal = checked_getfield<int32_t>(L, "LifeSteal", index);
		hit.HitWithWeapon = checked_getfield<bool>(L, "HitWithWeapon", index);
		hit.EffectFlags = (HitFlag)checked_getfield<uint32_t>(L, "EffectFlags", index);

		lua_getfield(L, index, "DamageList");
		auto damageList = DamageList::AsUserData(L, -1);
		lua_pop(L, 1);

		if (damageList == nullptr) {
			OsiErrorS("Missing 'DamageList' in Hit table");
			return false;
		} else {
			hit.DamageList.Clear();
			for (auto const& dmg : damageList->Get()) {
				hit.DamageList.AddDamage(dmg.DamageType, dmg.Amount);
			}
			return true;
		}
	}

	void PushPendingHit(lua_State* L, PendingHit const& hit)
	{
		lua_newtable(L);
		setfield(L, "HitId", hit.Id);

		if (hit.CapturedCharacterHit) {
			ObjectProxy<CDivinityStats_Item>::New(L, hit.WeaponStats);
			lua_setfield(L, -2, "Weapon");
			PushHit(L, hit.CharacterHit);
			lua_setfield(L, -2, "Hit");
			setfield(L, "HitType", hit.HitType);
			setfield(L, "NoHitRoll", hit.NoHitRoll);
			setfield(L, "ProcWindWalker", hit.ProcWindWalker);
			setfield(L, "ForceReduceDurability", hit.ForceReduceDurability);
			setfield(L, "HighGround", hit.HighGround);
			setfield(L, "CriticalRoll", hit.CriticalRoll);
		}

		if (hit.Status) {
			StatusHandleProxy::New(L, hit.Status->TargetHandle, hit.Status->StatusHandle);
			lua_setfield(L, -2, "HitStatus");
		}
	}

	void ServerState::OnStatusHitEnter(esv::StatusHit* hit, PendingHit* context)
	{
		StackCheck _(L, 0);
		PushInternalFunction(L, "_StatusHitEnter"); // stack: fn

		StatusHandleProxy::New(L, hit->TargetHandle, hit->StatusHandle);

		if (context) {
			PushPendingHit(L, *context);
		} else {
			lua_newtable(L);
		}

		if (CallWithTraceback(L, 2, 0) != 0) { // stack: succeeded
			LuaError("StatusHitEnter handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}

	bool ServerState::ComputeCharacterHit(CDivinityStats_Character * target,
		CDivinityStats_Character *attacker, CDivinityStats_Item *weapon, DamagePairList *damageList,
		HitType hitType, bool noHitRoll, bool forceReduceDurability, HitDamageInfo *hit,
		CRPGStats_Object_Property_List *skillProperties, HighGroundBonus highGroundFlag, CriticalRoll criticalRoll)
	{
		StackCheck _(L, 0);
		Restriction restriction(*this, RestrictOsiris);

		PushInternalFunction(L, "_ComputeCharacterHit"); // stack: fn

		auto luaTarget = ObjectProxy<CDivinityStats_Character>::New(L, target);
		UnbindablePin _t(luaTarget);
		ItemOrCharacterPushPin luaAttacker(L, attacker);

		ObjectProxy<CDivinityStats_Item> * luaWeapon = nullptr;
		if (weapon != nullptr) {
			luaWeapon = ObjectProxy<CDivinityStats_Item>::New(L, weapon);
		} else {
			lua_pushnil(L);
		}
		UnbindablePin _2(luaWeapon);

		auto luaDamageList = DamageList::New(L);
		for (auto const& dmg : *damageList) {
			luaDamageList->Get().SafeAdd(dmg);
		}

		push(L, hitType);
		push(L, noHitRoll);
		push(L, forceReduceDurability);

		PushHit(L, *hit);

		auto alwaysBackstab = skillProperties != nullptr
			&& skillProperties->Properties.Find(ToFixedString("AlwaysBackstab")) != nullptr;
		push(L, alwaysBackstab);

		push(L, highGroundFlag);
		push(L, criticalRoll);

		if (CallWithTraceback(L, 11, 1) != 0) { // stack: succeeded
			LuaError("ComputeCharacterHit handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
			return false;
		}

		int isnil = lua_isnil(L, -1);

		bool ok;
		if (isnil) {
			ok = false;
		} else if (lua_type(L, -1) == LUA_TTABLE) {
			lua_getfield(L, -1, "EffectFlags");
			auto effectFlags = lua_tointeger(L, -1);
			lua_pop(L, 1);
			lua_getfield(L, -1, "TotalDamageDone");
			auto totalDamageDone = lua_tointeger(L, -1);
			lua_pop(L, 1);
			lua_getfield(L, -1, "ArmorAbsorption");
			auto armorAbsorption = lua_tointeger(L, -1);
			lua_pop(L, 1);
			lua_getfield(L, -1, "LifeSteal");
			auto lifeSteal = lua_tointeger(L, -1);
			lua_pop(L, 1);
			lua_getfield(L, -1, "DamageList");
			auto damageList = DamageList::AsUserData(L, -1);
			lua_pop(L, 1);

			if (damageList == nullptr) {
				OsiErrorS("Missing 'DamageList' in table returned from ComputeCharacterHit");
				ok = false;
			} else {
				hit->EffectFlags = (HitFlag)effectFlags;
				hit->TotalDamage = (int32_t)totalDamageDone;
				hit->ArmorAbsorption = (int32_t)armorAbsorption;
				hit->LifeSteal = (int32_t)lifeSteal;
				hit->DamageList.Clear();
				for (auto const& dmg : damageList->Get()) {
					hit->DamageList.AddDamage(dmg.DamageType, dmg.Amount);
				}
				ok = true;
			}
		} else {
			OsiErrorS("ComputeCharacterHit must return a table");
			ok = false;
		}

		lua_pop(L, 1); // stack: -
		return ok;
	}

	bool ServerState::OnCharacterApplyDamage(esv::Character* target, HitDamageInfo& hit, ObjectHandle attackerHandle,
			CauseType causeType, glm::vec3& impactDirection, PendingHit* context)
	{
		StackCheck _(L, 0);
		Restriction restriction(*this, RestrictOsiris);

		PushInternalFunction(L, "_BeforeCharacterApplyDamage"); // stack: fn

		auto luaTarget = ObjectProxy<esv::Character>::New(L, target);
		UnbindablePin _t(luaTarget);

		CRPGStats_Object* attacker{ nullptr };
		if (attackerHandle) {
			auto attackerChar = GetEntityWorld()->GetCharacter(attackerHandle, false);
			if (attackerChar) {
				attacker = attackerChar->Stats;
			} else {
				auto attackerItem = GetEntityWorld()->GetItem(attackerHandle, false);
				if (attackerItem) {
					attacker = attackerItem->Stats;
				} else {
					OsiError("Could not resolve attacker handle: " << std::hex << attackerHandle.Handle);
				}
			}
		}

		ItemOrCharacterPushPin luaAttacker(L, attacker);

		PushHit(L, hit);

		push(L, causeType);
		push(L, impactDirection); // stack: fn, target, attacker, hit, causeType, impactDirection

		if (context) {
			PushPendingHit(L, *context);
		} else {
			lua_newtable(L);
		}

		if (CallWithTraceback(L, 6, 1) != 0) { // stack: succeeded
			LuaError("BeforeCharacterApplyDamage handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
			return false;
		}

		int top = lua_gettop(L);
		try {
			PopHit(L, hit, -1);
		}
		catch (Exception& e) {
			lua_settop(L, top);
			OsiError("Exception thrown during BeforeCharacterApplyDamage processing: " << e.what());
		}

		lua_pop(L, 1); // stack: -
		return false;
	}

	*/
	void ServerState::OnGameStateChanged(GameState fromState, GameState toState)
	{
		StackCheck _(L, 0);
		PushInternalFunction(L, "_GameStateChanged"); // stack: fn
		push(L, fromState);
		push(L, toState);
		CheckedCall<>(L, 2, "Ext.GameStateChanged");
	}

	/*
	esv::Item* ServerState::OnGenerateTreasureItem(esv::Item* item)
	{
		StackCheck _(L, 0);
		PushInternalFunction(L, "_TreasureItemGenerated"); // stack: fn

		ObjectHandle itemHandle;
		item->GetObjectHandle(itemHandle);
		ObjectProxy<esv::Item>::New(L, itemHandle);

		if (CallWithTraceback(L, 1, 1) != 0) { // stack: succeeded
			LuaError("TreasureItemGenerated handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
			return item;
		}

		auto returnItem = ObjectProxy<esv::Item>::AsUserData(L, -1);
		lua_pop(L, 1);

		if (!returnItem) {
			return item;
		}

		auto returnObj = returnItem->Get(L);
		if (!returnObj) {
			return item;
		}

		if (returnObj->ParentInventoryHandle) {
			OsiError("TreasureItemGenerated must return an item that's not already in an inventory");
			return item;
		}

		if (!returnObj->CurrentLevel || returnObj->CurrentLevel.Str[0]) {
			OsiError("TreasureItemGenerated must return an item that's not in the level");
			return item;
		}

		return returnObj;
	}


	bool ServerState::OnBeforeCraftingExecuteCombination(CraftingStationType craftingStation, ObjectSet<ObjectHandle> const& ingredients,
		esv::Character* character, uint8_t quantity, FixedString const& combinationId)
	{
		StackCheck _(L, 0);
		PushInternalFunction(L, "_BeforeCraftingExecuteCombination"); // stack: fn

		ObjectProxy<esv::Character>::New(L, character);
		push(L, craftingStation);

		lua_newtable(L);
		int32_t index = 1;
		for (auto ingredientHandle : ingredients) {
			auto ingredient = GetEntityWorld()->GetItem(ingredientHandle);
			if (ingredient) {
				push(L, index);
				ObjectProxy<esv::Item>::New(L, ingredient);
				lua_settable(L, -3);
			}
		}

		push(L, quantity);
		push(L, combinationId);

		if (CallWithTraceback(L, 5, 1) != 0) { // stack: succeeded
			LuaError("BeforeCraftingExecuteCombination handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
			return false;
		}

		auto processed = get<bool>(L, -1);
		lua_pop(L, 1);
		if (processed) {
			return processed;
		} else {
			return false;
		}
	}


	void ServerState::OnAfterCraftingExecuteCombination(CraftingStationType craftingStation, ObjectSet<ObjectHandle> const& ingredients,
		esv::Character* character, uint8_t quantity, FixedString const& combinationId, bool succeeded)
	{
		StackCheck _(L, 0);
		PushInternalFunction(L, "_AfterCraftingExecuteCombination"); // stack: fn

		ObjectProxy<esv::Character>::New(L, character);
		push(L, craftingStation);

		lua_newtable(L);
		int32_t index = 1;
		for (auto ingredientHandle : ingredients) {
			auto ingredient = GetEntityWorld()->GetItem(ingredientHandle, false);
			if (ingredient) {
				push(L, index);
				ObjectProxy<esv::Item>::New(L, ingredient);
				lua_settable(L, -3);
			}
		}

		push(L, quantity);
		push(L, combinationId);
		push(L, succeeded);

		if (CallWithTraceback(L, 6, 0) != 0) { // stack: succeeded
			LuaError("AfterCraftingExecuteCombination handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}


	void ServerState::OnBeforeShootProjectile(ShootProjectileHelper* helper)
	{
		StackCheck _(L, 0);
		PushInternalFunction(L, "_OnBeforeShootProjectile");
		UnbindablePin _h(ObjectProxy<ShootProjectileHelper>::New(L, helper));

		if (CallWithTraceback(L, 1, 0) != 0) {
			LuaError("OnBeforeShootProjectile handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}


	void ServerState::OnShootProjectile(Projectile* projectile)
	{
		StackCheck _(L, 0);
		PushInternalFunction(L, "_OnShootProjectile");
		UnbindablePin _p(ObjectProxy<esv::Projectile>::New(L, projectile));

		if (CallWithTraceback(L, 1, 0) != 0) {
			LuaError("OnShootProjectile handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}

	void PushGameObject(lua_State* L, ObjectHandle handle)
	{
		if (!handle) {
			push(L, nullptr);
			return;
		}

		switch (handle.GetType()) {
		case (uint32_t)ObjectType::ServerCharacter:
			ObjectProxy<esv::Character>::New(L, handle);
			break;

		case (uint32_t)ObjectType::ServerItem:
			ObjectProxy<esv::Item>::New(L, handle);
			break;

		case (uint32_t)ObjectType::ServerProjectile:
			ObjectProxy<esv::Projectile>::New(L, handle);
			break;

		default:
			push(L, nullptr);
			LuaError("Don't know how to push handle of type " << handle.GetType());
			break;
		}
	}


	void ServerState::OnProjectileHit(Projectile* projectile, ObjectHandle const& hitObject, glm::vec3 const& position)
	{
		StackCheck _(L, 0);
		PushExtFunction(L, "_OnProjectileHit");
		UnbindablePin _p(ObjectProxy<esv::Projectile>::New(L, projectile));

		PushGameObject(L, hitObject);
		push(L, position);

		if (CallWithTraceback(L, 3, 0) != 0) {
			LuaError("OnProjectileHit handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}


	void ServerState::OnExecutePropertyDataOnGroundHit(glm::vec3& position, ObjectHandle casterHandle, DamagePairList* damageList)
	{
		StackCheck _(L, 0);
		PushExtFunction(L, "_OnGroundHit");

		PushGameObject(L, casterHandle);
		push(L, position);
		auto dmgList = DamageList::New(L);
		if (damageList) {
			dmgList->Get().CopyFrom(*damageList);
		}

		if (CallWithTraceback(L, 3, 0) != 0) {
			LuaError("GroundHit handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}


	void ServerState::ExecutePropertyDataOnTarget(CRPGStats_Object_Property_Extender* prop, ObjectHandle attackerHandle,
		ObjectHandle target, glm::vec3 const& impactOrigin, bool isFromItem, SkillPrototype * skillProto,
		HitDamageInfo const* damageInfo)
	{
		StackCheck _(L, 0);
		PushExtFunction(L, "_ExecutePropertyDataOnTarget");

		LuaSerializer serializer(L, true);
		auto propRef = static_cast<CDivinityStats_Object_Property_Data*>(prop);
		SerializeObjectProperty(serializer, propRef);
		PushGameObject(L, attackerHandle);
		PushGameObject(L, target);
		push(L, impactOrigin);
		push(L, isFromItem);

		if (skillProto) {
			SkillPrototypeProxy::New(L, skillProto, -1);
		} else {
			push(L, nullptr);
		}

		if (damageInfo) {
			PushHit(L, *damageInfo);
		} else {
			push(L, nullptr);
		}

		if (CallWithTraceback(L, 7, 0) != 0) {
			LuaError("ExecutePropertyDataOnTarget handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}


	void ServerState::ExecutePropertyDataOnPosition(CRPGStats_Object_Property_Extender* prop, ObjectHandle attackerHandle,
		glm::vec3 const& position, float areaRadius, bool isFromItem, SkillPrototype * skillPrototype,
		HitDamageInfo const* damageInfo)
	{
		StackCheck _(L, 0);
		PushExtFunction(L, "_ExecutePropertyDataOnPosition");

		LuaSerializer serializer(L, true);
		auto propRef = static_cast<CDivinityStats_Object_Property_Data*>(prop);
		SerializeObjectProperty(serializer, propRef);
		PushGameObject(L, attackerHandle);
		push(L, position);
		push(L, areaRadius);
		push(L, isFromItem);

		if (skillPrototype) {
			SkillPrototypeProxy::New(L, skillPrototype, -1);
		} else {
			push(L, nullptr);
		}

		if (damageInfo) {
			PushHit(L, *damageInfo);
		} else {
			push(L, nullptr);
		}

		if (CallWithTraceback(L, 7, 0) != 0) {
			LuaError("ExecutePropertyDataOnPosition handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}


	bool ServerState::OnUpdateTurnOrder(esv::TurnManager * self, uint8_t combatId)
	{
		StackCheck _(L, 0);
		Restriction restriction(*this, RestrictOsiris);

		auto turnMgr = GetEntityWorld()->GetTurnManager();
		if (!turnMgr) {
			OsiErrorS("Couldn't fetch turn manager");
			return false;
		}

		auto combat = turnMgr->Combats.Find(combatId);
		if (combat == nullptr) {
			OsiError("No combat found with ID " << (unsigned)combatId);
			return false;
		}

		PushExtFunction(L, "_CalculateTurnOrder"); // stack: fn

		TurnManagerCombatProxy::New(L, combatId); // stack: fn, combat
		CombatTeamListToLua(L, combat->NextRoundTeams);

		if (CallWithTraceback(L, 2, 1) != 0) { // stack: retval
			LuaError("OnUpdateTurnOrder handler failed: " << lua_tostring(L, -1));
			lua_pop(L, 1);
			return false;
		}

		int isnil = lua_isnil(L, -1);

		bool ok = false;
		if (!isnil) {
			try {
				UpdateTurnOrder(L, combatId, -1, combat->NextRoundTeams, combat->NextTurnChangeNotificationTeamIds);
				ok = true;
			} catch (Exception &) {
				OsiError("UpdateTurnOrder failed");
			}
		}

		lua_pop(L, 1); // stack: -
		return ok;
	}*/


	std::optional<STDString> ServerState::GetModPersistentVars(STDString const& modTable)
	{
		StackCheck _(L, 0);
		Restriction restriction(*this, RestrictAll);

		PushExtFunction(L, "_GetModPersistentVars");
		push(L, modTable);

		auto ret = CheckedCall<std::optional<char const*>>(L, 1, "Ext.GetModPersistentVars");
		if (ret) {
			return std::get<0>(*ret);
		} else {
			return {};
		}
	}


	void ServerState::RestoreModPersistentVars(STDString const& modTable, STDString const& vars)
	{
		StackCheck _(L, 0);
		Restriction restriction(*this, RestrictAll);

		PushExtFunction(L, "_RestoreModPersistentVars");
		push(L, modTable);
		push(L, vars);

		CheckedCall<>(L, 2, "Ext.RestoreModPersistentVars");
	}


	void ServerState::OnGameSessionLoading()
	{
		identityAdapters_.UpdateAdapters();

		State::OnGameSessionLoading();
	}
}

namespace bg3se::esv
{

	ExtensionState & ExtensionState::Get()
	{
		return gOsirisProxy->GetServerExtensionState();
	}

	lua::State * ExtensionState::GetLua()
	{
		if (Lua) {
			return Lua.get();
		} else {
			return nullptr;
		}
	}

	ModManager * ExtensionState::GetModManager()
	{
		auto server = GetEoCServer();
		if (server) {
			return server->ModManagerServer;
		} else {
			return nullptr;
		}
	}

	void ExtensionState::OnGameSessionLoading()
	{
		dynamicStats_.clear();
		persistentStats_.clear();
		cachedPersistentVars_.clear();
		bg3se::ExtensionStateBase::OnGameSessionLoading();
	}

	void ExtensionState::Reset()
	{
		ExtensionStateBase::Reset();
		//DamageHelpers.Clear();
	}

	void ExtensionState::DoLuaReset()
	{
		Lua.reset();
		Lua = std::make_unique<lua::ServerState>(*this);
		Lua->StoryFunctionMappingsUpdated();
	}

	void ExtensionState::LuaStartup()
	{
		ExtensionStateBase::LuaStartup();

		LuaServerPin lua(*this);
		/*auto gameState = GetStaticSymbols().GetServerState();
		if (gameState
			&& (*gameState == esv::GameState::LoadLevel
				|| (*gameState == esv::GameState::LoadModule && WasStatLoadTriggered())
				|| *gameState == esv::GameState::LoadGMCampaign
				|| *gameState == esv::GameState::LoadSession
				|| *gameState == esv::GameState::Sync
				|| *gameState == esv::GameState::Paused
				|| *gameState == esv::GameState::Running
				|| *gameState == esv::GameState::GameMasterPause)) {
			lua->OnModuleResume();
		}*/
	}

	void ExtensionState::MarkPersistentStat(FixedString const& statId)
	{
		persistentStats_.insert(statId);
	}

	void ExtensionState::UnmarkPersistentStat(FixedString const& statId)
	{
		auto it = persistentStats_.find(statId);
		if (it != persistentStats_.end()) {
			persistentStats_.erase(it);
		}
	}

	void ExtensionState::MarkDynamicStat(FixedString const& statId)
	{
		dynamicStats_.insert(statId);
	}


	std::optional<STDString> ExtensionState::GetModPersistentVars(FixedString const& mod)
	{
		auto modTable = GetModTable(mod);
		if (modTable) {
			LuaServerPin lua(*this);
			if (lua) {
				auto vars = lua->GetModPersistentVars(*modTable);
				if (vars) {
					return vars;
				}
			}
		}

		// If we failed to retrieve vars from the Lua state for some reason, 
		// return the persistent vars we loaded from the savegame instead
		auto cachedIt = cachedPersistentVars_.find(mod);
		if (cachedIt != cachedPersistentVars_.end()) {
			ERR("Persistent variables for mod %s could not be retrieved, saving cached values!", mod.GetString());
			return cachedIt->second;
		} else {
			return {};
		}
	}

	void ExtensionState::RestoreModPersistentVars(FixedString const& mod, STDString const& vars)
	{
		cachedPersistentVars_.insert(std::make_pair(mod, vars));

		auto modTable = GetModTable(mod);
		if (modTable) {
			DEBUG("Restoring persistent vars for mod %s (%ld bytes)", mod.GetString(), vars.size());
			LuaServerPin lua(*this);
			if (lua) {
				lua->RestoreModPersistentVars(*modTable, vars);
			}
		} else {
			ERR("Savegame has persistent variables for mod %s, but it is not loaded or has no ModTable! Variables may be lost on next save!", mod.GetString());
		}
	}

	std::optional<STDString> ExtensionState::GetModTable(FixedString const& mod)
	{
		auto const& configs = GetConfigs();
		auto configIt = configs.find(mod);
		if (configIt != configs.end() && !configIt->second.ModTable.empty()) {
			return configIt->second.ModTable;
		} else {
			return {};
		}
	}

	std::unordered_set<FixedString> ExtensionState::GetPersistentVarMods()
	{
		std::unordered_set<FixedString> names;
		for (auto const& kv : cachedPersistentVars_) {
			names.insert(kv.first);
		}

		for (auto const& config : GetConfigs()) {
			if (config.second.MinimumVersion >= 43 && !config.second.ModTable.empty()) {
				names.insert(config.first);
			}
		}

		return names;
	}

	void ExtensionState::StoryLoaded()
	{
		DEBUG("ExtensionStateServer::StoryLoaded()");
		LuaServerPin lua(*this);
		if (lua) {
			lua->StoryLoaded();
		}
	}

	void ExtensionState::StoryFunctionMappingsUpdated()
	{
		LuaServerPin lua(*this);
		if (lua) {
			lua->StoryFunctionMappingsUpdated();
		}
	}

	void ExtensionState::StorySetMerging(bool isMerging)
	{
		LuaServerPin lua(*this);
		if (lua) {
			lua->StorySetMerging(isMerging);
		}
	}
}
