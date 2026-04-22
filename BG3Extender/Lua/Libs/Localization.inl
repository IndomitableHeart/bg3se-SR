BEGIN_SE()

RuntimeStringHandle::RuntimeStringHandle()
    : Handle(GFS.strUnknownTSHandle),
    Version(0)
{}

std::optional<StringView> TranslatedStringRepository::GetTranslatedString(RuntimeStringHandle const& handle)
{
    Lock.ReadLock();
    auto text = TranslatedStrings[0]->Texts.try_get(handle);
    if (!text) {
        text = VersionedFallbackPool->Texts.try_get(handle);
        if (!text) {
            text = FallbackPool->Texts.try_get(handle);
        }
    }
    Lock.ReadUnlock();

    return text ? *text : std::optional<StringView>{};
}

void TranslatedStringRepository::UpdateTranslatedString(RuntimeStringHandle const& handle, StringView translated)
{
    auto text = GameAlloc<STDString>(translated);
    Lock.WriteLock();
    TranslatedStrings[0]->Strings.push_back(text);
    TranslatedStrings[0]->Texts.set(handle, LSStringView(text->data(), text->size()));
    Lock.WriteUnlock();
}

END_SE()

/// <lua_module>Loca</lua_module>
BEGIN_NS(lua::loca)

STDString GetTranslatedString(FixedString handle, std::optional<char const*> fallbackText)
{
    auto repo = GetStaticSymbols().GetTranslatedStringRepository();
    if (repo) {
        auto text = repo->GetTranslatedString(RuntimeStringHandle(handle, 0));
        if (text) {
            return STDString(*text);
        }
    }

    return fallbackText ? *fallbackText : "";
}

unsigned NextDynamicStringHandleId{ 1 };

bool UpdateTranslatedString(FixedString handle, char const* value)
{
    auto repo = GetStaticSymbols().GetTranslatedStringRepository();
    if (!repo) return false;

    repo->UpdateTranslatedString(RuntimeStringHandle(handle, 0), value);
    return true;
}

// Resolve a slug / key (e.g. "CRA_Beach_SUB") to its localized string.
// Larian's UI passes slugs to SetSubRegionName, and the repository's
// TextToStringKey map bridges those slugs to RuntimeStringHandles.
// This convenience chains both lookups in one call so Lua can go
// straight from slug to displayed text.
//
// Note: the declaration in ScriptHelpers.h for a similarly named
// helper is never defined (dead header entry from an older SE
// revision), so we perform the lookup inline against the repository
// HashMap directly.  Empty string returned when the slug isn't
// registered or the resulting handle has no localization entry.
STDString GetTranslatedStringFromKey(FixedString key, std::optional<char const*> fallbackText)
{
    auto repo = GetStaticSymbols().GetTranslatedStringRepository();
    if (repo) {
        auto handle = repo->TextToStringKey.try_get(key);
        if (handle) {
            auto text = repo->GetTranslatedString(*handle);
            if (text) {
                return STDString(*text);
            }
        }
    }

    return fallbackText ? *fallbackText : "";
}

void RegisterLocalizationLib()
{
    DECLARE_MODULE(Loca, Both)
    BEGIN_MODULE()
    MODULE_FUNCTION(GetTranslatedString)
    MODULE_FUNCTION(GetTranslatedStringFromKey)
    MODULE_FUNCTION(UpdateTranslatedString)
    END_MODULE()
}

END_NS()
