#include <Lua/Shared/Proxies/LuaCppObjectProxy.h>
#include <Extender/ScriptExtender.h>

BEGIN_NS(lua)

int CppObjectProxyHelpers::Next(lua_State* L, GenericPropertyMap const& pm, void* object, LifetimeHandle lifetime, FixedStringId const& key)
{
    if (!key) {
        if (!pm.IterableProperties.empty()) {
            StackCheck _(L, 2);
            auto it = pm.IterableProperties.begin();
            push(L, it.Key());
            auto const& prop = pm.Properties.values()[it.Value()];
            if (pm.GetRawProperty(L, lifetime, object, prop) != PropertyOperationResult::Success) {
                push(L, nullptr);
            }

            return 2;
        }
    } else {
        auto const& k = reinterpret_cast<FixedStringUnhashed const&>(key);
        auto it = pm.IterableProperties.find(k);
        if (it != pm.IterableProperties.end()) {
            ++it;
            if (it != pm.IterableProperties.end()) {
                StackCheck _(L, 2);
                push(L, it.Key());
                auto const& prop = pm.Properties.values()[it.Value()];
                if (pm.GetRawProperty(L, lifetime, object, prop) != PropertyOperationResult::Success) {
                    push(L, nullptr);
                }

                return 2;
            }
        }
    }

    if (pm.FallbackNext) {
        return pm.FallbackNext(L, lifetime, object, key);
    }

    return 0;
}

CppMetatableManager::CppMetatableManager()
{
    metatables_.resize((int)MetatableTag::Max + 1);
}

void CppMetatableManager::RegisterMetatable(MetatableTag tag, CMetatable* mt)
{
    se_assert(tag <= MetatableTag::Max);
    metatables_[(int)tag] = mt;
}

CMetatable* CppMetatableManager::GetMetatable(MetatableTag tag)
{
    se_assert(tag <= MetatableTag::Max);
    return metatables_[(int)tag];
}

CppMetatableManager& CppMetatableManager::FromLua(lua_State* L)
{
    return State::FromLua(L)->GetMetatableManager();
}

void* ObjectProxy::GetRaw(lua_State* L, int index, GenericPropertyMap const& pm)
{
    auto meta = lua_get_lightcppany(L, index);

    if (meta.MetatableTag == MetatableTag::ImguiObject) {
        // Temporary jank to support imgui objects
        auto obj = ImguiObjectProxyMetatable::GetGeneric(L, index, meta);
        auto& objPm = obj->GetRTTI();
        if (!objPm.IsA(pm.RegistryIndex)) {
            luaL_error(L, "Argument %d: Expected object of type '%s', got '%s'", index,
                pm.Name.GetString(), objPm.Name.GetString());
        }

        return obj;

    } else if (meta.MetatableTag == MetatableTag::ObjectRef) {
        auto& objPm = *gStructRegistry.Get(meta.PropertyMapTag);
        if (!objPm.IsA(pm.RegistryIndex)) {
            luaL_error(L, "Argument %d: Expected object of type '%s', got '%s'", index,
                pm.Name.GetString(), objPm.Name.GetString());
        }

        if (!meta.Lifetime.IsAlive(L)) {
            luaL_error(L, "Attempted to fetch '%s' whose lifetime has expired", pm.Name.GetString());
        }

        return meta.Ptr;
    } else {
        luaL_error(L, "Argument %d: Expected %s, got %s", index,
            pm.Name.GetString(), GetDebugName(meta));
        return nullptr;
    }
}

GenericPropertyMap& LightObjectProxyMetatable::GetPropertyMap(CppObjectMetadata const& meta)
{
    se_assert(meta.MetatableTag == MetaTag);
    return *gStructRegistry.Get(meta.PropertyMapTag);
}


void* LightObjectProxyMetatable::TryGetGeneric(lua_State* L, int index, int propertyMapIndex)
{
    CppObjectMetadata meta;
    if (lua_try_get_lightcppobject(L, index, MetaTag, meta)) {
        auto& pm = *gStructRegistry.Get(meta.PropertyMapTag);
        if (pm.IsA(meta.PropertyMapTag) && meta.Lifetime.IsAlive(L)) {
            return meta.Ptr;
        }
    }

    return nullptr;
}

void* LightObjectProxyMetatable::GetGeneric(lua_State* L, int index, int propertyMapIndex)
{
    CppObjectMetadata meta;
    if (lua_try_get_lightcppobject(L, index, meta)) {
        auto& pm = *gStructRegistry.Get(meta.PropertyMapTag);
        if (pm.IsA(propertyMapIndex)) {
            if (!meta.Lifetime.IsAlive(L)) {
                luaL_error(L, "Attempted to fetch '%s' whose lifetime has expired", GetTypeName(L, meta));
                return 0;
            }

            return meta.Ptr;
        } else {
            luaL_error(L, "Argument %d: Expected object of type '%s', got '%s'", index,
                gStructRegistry.Get(propertyMapIndex)->Name.GetString(),
                pm.Name.GetString());
            return nullptr;
        }
    } else {
        luaL_error(L, "Argument %d: Expected object of type '%s', got '%s'", index,
            gStructRegistry.Get(propertyMapIndex)->Name.GetString(),
            GetDebugName(L, index));
        return nullptr;
    }
}

int LightObjectProxyMetatable::Index(lua_State* L, CppObjectOpaque* self)
{
    auto pm = gStructRegistry.Get(lua_get_opaque_property_map(self));
    auto prop = get<FixedStringNoRef>(L, 2);
    auto result = pm->GetRawProperty(L, lua_get_opaque_lifetime(self), lua_get_opaque_ptr(self), prop);
    switch (result) {
    case PropertyOperationResult::Success:
        break;

    case PropertyOperationResult::NoSuchProperty:
        luaL_error(L, "Property does not exist: %s::%s - property does not exist", GetTypeName(L, self), get<char const*>(L, 2));
        push(L, nullptr);
        break;

    case PropertyOperationResult::Unknown:
    default:
        luaL_error(L, "Cannot get property %s::%s - unknown error", GetTypeName(L, self), get<char const*>(L, 2));
        push(L, nullptr);
        break;
    }

    return 1;
}

int LightObjectProxyMetatable::NewIndex(lua_State* L, CppObjectOpaque* self)
{
    auto pm = gStructRegistry.Get(lua_get_opaque_property_map(self));
    auto prop = get<FixedStringNoRef>(L, 2);
    auto result = pm->SetRawProperty(L, lua_get_opaque_ptr(self), prop, 3);
    switch (result) {
    case PropertyOperationResult::Success:
        break;

    case PropertyOperationResult::NoSuchProperty:
        luaL_error(L, "Cannot set property %s::%s - property does not exist", GetTypeName(L, self), get<char const*>(L, 2));
        break;

    case PropertyOperationResult::ReadOnly:
        luaL_error(L, "Cannot set property %s::%s - property is read-only", GetTypeName(L, self), get<char const*>(L, 2));
        break;

    case PropertyOperationResult::UnsupportedType:
        luaL_error(L, "Cannot set property %s::%s - cannot write properties of this type", GetTypeName(L, self), get<char const*>(L, 2));
        break;

    case PropertyOperationResult::Unknown:
    default:
        luaL_error(L, "Cannot set property %s::%s - unknown error", GetTypeName(L, self), get<char const*>(L, 2));
        break;
    }

    return 0;
}

int LightObjectProxyMetatable::ToString(lua_State* L, CppObjectMetadata& self)
{
    char entityName[200];
    if (self.Lifetime.IsAlive(L)) {
        _snprintf_s(entityName, std::size(entityName) - 1, "%s (%p)", GetTypeName(L, self), self.Ptr);
    } else {
        _snprintf_s(entityName, std::size(entityName) - 1, "%s (%p, DEAD REFERENCE)", GetTypeName(L, self), self.Ptr);
    }

    push(L, entityName);
    return 1;
}

int LightObjectProxyMetatable::GC(lua_State* L, CppObjectMetadata& self)
{
    auto pm = gStructRegistry.Get(self.PropertyMapTag);
    pm->Destroy(self.Ptr);
    return 0;
}

bool LightObjectProxyMetatable::IsEqual(lua_State* L, CppObjectMetadata& self, CppObjectMetadata& other)
{
    return self.Ptr == other.Ptr && self.PropertyMapTag == other.PropertyMapTag;
}

int LightObjectProxyMetatable::Next(lua_State* L, CppObjectOpaque* self)
{
    auto pm = gStructRegistry.Get(lua_get_opaque_property_map(self));
    auto object = lua_get_opaque_ptr(self);
    auto lifetime = lua_get_opaque_lifetime(self);
    if (lua_type(L, 2) == LUA_TNIL) {
        return CppObjectProxyHelpers::Next(L, *pm, object, lifetime, FixedStringId{});
    } else {
        auto key = get<FixedStringNoRef>(L, 2);
        return CppObjectProxyHelpers::Next(L, *pm, object, lifetime, key);
    }
}

char const* LightObjectProxyMetatable::GetTypeName(lua_State* L, CppObjectMetadata& self)
{
    auto pm = gStructRegistry.Get(self.PropertyMapTag);
    return pm->Name.GetString();
}

char const* LightObjectProxyMetatable::GetTypeName(lua_State* L, CppObjectOpaque* self)
{
    auto pm = gStructRegistry.Get(lua_get_opaque_property_map(self));
    return pm->Name.GetString();
}

END_NS()
