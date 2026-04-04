#include <GameDefinitions/Symbols.h>
#include <GameDefinitions/Sound.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

/// <lua_module>Audio</lua_module>
BEGIN_NS(ecl::lua::audio)

WwiseManager* GetSoundManager()
{
    auto resourceMgr = GetStaticSymbols().ls__gGlobalResourceManager;
    if (resourceMgr != nullptr && *resourceMgr != nullptr && (*resourceMgr)->SoundManager != nullptr) {
        return (*resourceMgr)->SoundManager;
    }

    LuaError("Sound manager is not available!");
    return nullptr;
}

SoundObjectId GetSoundObjectId(lua_State* L, int idx)
{
    auto snd = GetSoundManager();
    if (!snd) return InvalidSoundObjectId;

    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return InvalidSoundObjectId;

    case LUA_TSTRING:
    {
        auto name = get<STDString>(L, idx);

        unsigned playerIndex = 0;
        if (name.size() > 1 && *name.rbegin() >= '1' && *name.rbegin() <= '4') {
            playerIndex = (*name.rbegin() - '1');
            name = name.substr(0, name.size() - 1);
        }

        if (name == "Global") {
            return InvalidSoundObjectId;
        } else if (name == "Music") {
            return snd->MusicHandle;
        } else if (name == "ControllerSpeakerListener") {
            return snd->ControllerSpeakerListener[playerIndex];
        } else if (name == "Listener") {
            return snd->Listener[playerIndex];
        } else if (name == "RumbleListener") {
            return snd->RumbleListener[playerIndex];
        } else if (name == "PlayerEmitter") {
            return snd->PlayerEmitter[playerIndex];
        } else if (name == "Ambient") {
            return snd->Ambient[playerIndex];
        } else if (name == "HUD") {
            return snd->HUD[playerIndex];
        } else if (name == "CineHUD") {
            return snd->CineHUD[playerIndex];
        } else {
            luaL_error(L, "Unknown built-in sound object name: %s", name);
            return InvalidSoundObjectId;
        }
    }

    case LUA_TLIGHTCPPOBJECT:
    {
        auto entity = get<EntityHandle>(L, idx);
        auto sound = State::FromLua(L)->GetEntitySystemHelpers()->GetComponent<SoundComponent>(entity);
        if (sound && sound->ActiveData && sound->ActiveData->SoundObjectId != 0xffffffffffffffffull) {
            return SoundObjectId(sound->ActiveData->SoundObjectId);
        } else {
            return InvalidSoundObjectId;
        }
    }

    default:
        luaL_error(L, "Must specify nil, entity handle or built-in name as sound object");
        return InvalidSoundObjectId;
    }
}

END_NS()

BEGIN_NS(lua)

LuaSoundObjectId do_get(lua_State* L, int index, Overload<LuaSoundObjectId>)
{
    return LuaSoundObjectId{ ecl::lua::audio::GetSoundObjectId(L, index) };
}

END_NS()

BEGIN_NS(ecl::lua::audio)

using namespace bg3se::lua;

bool SetSwitch(LuaSoundObjectId soundObject, char const* switchGroup, char const* state)
{
    return GetSoundManager()->SetSwitch(switchGroup, state, (SoundObjectId)soundObject);
}

bool SetState(char const* stateGroup, char const* state)
{
    return GetSoundManager()->SetState(stateGroup, state);
}

bool SetRTPC(LuaSoundObjectId soundObject, char const* rtpcName, float value, std::optional<bool> bypassInternalValueInterpolation)
{
    return GetSoundManager()->SetRTPCValue((SoundObjectId)soundObject, rtpcName, value, bypassInternalValueInterpolation ? *bypassInternalValueInterpolation : false);
}

float GetRTPC(LuaSoundObjectId soundObject, char const* rtpcName)
{
    return GetSoundManager()->GetRTPCValue((SoundObjectId)soundObject, rtpcName);
}

void ResetRTPC(LuaSoundObjectId soundObject, char const* rtpcName)
{
    GetSoundManager()->ResetRTPCValue((SoundObjectId)soundObject, rtpcName);
}

void Stop(std::optional<LuaSoundObjectId> soundObject)
{
    auto snd = GetSoundManager();
    if (!snd) {
        return;
    }

    if (soundObject) {
        snd->StopSounds((SoundObjectId)*soundObject);
    } else {
        snd->StopAllSounds();
    }
}

void PauseAllSounds()
{
    auto snd = GetSoundManager();
    if (!snd) {
        return;
    }

    snd->PauseAllSounds();
}

void ResumeAllSounds()
{
    auto snd = GetSoundManager();
    if (!snd) {
        return;
    }

    snd->ResumeAllSounds();
}

bool PostEvent(LuaSoundObjectId soundObject, char const* eventName, std::optional<float> positionSec)
{
    return GetSoundManager()->PostEventByName((SoundObjectId)soundObject, eventName, positionSec.value_or(0.0f), false, nullptr);
}

bool LoadEvent(char const* eventName)
{
    SoundNameId eventId = GetSoundManager()->GetIDFromString(eventName);
    return GetSoundManager()->LoadEvent(eventId);
}

bool UnloadEvent(char const* eventName)
{
    SoundNameId eventId = GetSoundManager()->GetIDFromString(eventName);
    return GetSoundManager()->UnloadEvent(eventId);
}

bool PlayExternalSound(LuaSoundObjectId soundObject, char const* eventName, char const* path, AudioCodec codec, std::optional<float> positionSec)
{
    STDString lsPath = GetStaticSymbols().ToPath(path, PathRootType::Data);
    auto eventId = GetSoundManager()->GetIDFromString(eventName);
    return GetSoundManager()->PlayExternalSound((SoundObjectId)soundObject, eventId, lsPath, (uint8_t)codec, positionSec.value_or(0.0f), false, nullptr);
}

// ---------------------------------------------------------------------------
// MCI-based file playback -- plays a WAV file through the Windows audio
// pipeline alongside the game audio.  Supports play, pause, resume, stop.
// Path is relative to the game Data directory.
// ---------------------------------------------------------------------------

static bool gFileIsPlaying = false;
static std::wstring gPlayingFilePath;

bool PlayFile(char const* path)
{
    // Stop any previous playback.
    if (gFileIsPlaying) {
        BG3A_LOG("[BG3Access] PlayFile: stopping previous playback before new file");
        PlaySoundW(nullptr, nullptr, 0);
        gFileIsPlaying = false;
        gPlayingFilePath.clear();
    }

    STDString lsPath = GetStaticSymbols().ToPath(path, PathRootType::Data);
    std::wstring widePath(lsPath.begin(), lsPath.end());

    BG3A_LOG("[BG3Access] PlayFile: calling PlaySoundW for %s", path);
    BOOL result = PlaySoundW(widePath.c_str(), nullptr,
        SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    if (!result) {
        ERR("PlayFile failed for: %ls", widePath.c_str());
        return false;
    }

    BG3A_LOG("[BG3Access] PlayFile: started successfully");
    gFileIsPlaying = true;
    gPlayingFilePath = widePath;
    return true;
}

void PauseFile()
{
    BG3A_LOG("[BG3Access] PauseFile: called (isPlaying=%d)", (int)gFileIsPlaying);
    // PlaySoundW does not support pause.  Stop instead.
    if (gFileIsPlaying) {
        PlaySoundW(nullptr, nullptr, 0);
        BG3A_LOG("[BG3Access] PauseFile: stopped (pause not supported)");
    }
}

void ResumeFile()
{
    // PlaySoundW does not support resume.  Restart from beginning.
    BG3A_LOG("[BG3Access] ResumeFile: called (isPlaying=%d, hasPath=%d)",
        (int)gFileIsPlaying, (int)!gPlayingFilePath.empty());
    if (gFileIsPlaying && !gPlayingFilePath.empty()) {
        BG3A_LOG("[BG3Access] ResumeFile: restarting playback");
        PlaySoundW(gPlayingFilePath.c_str(), nullptr,
            SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
}

void StopFile()
{
    BG3A_LOG("[BG3Access] StopFile: called (isPlaying=%d)", (int)gFileIsPlaying);
    if (gFileIsPlaying) {
        PlaySoundW(nullptr, nullptr, 0);
        gFileIsPlaying = false;
        gPlayingFilePath.clear();
        BG3A_LOG("[BG3Access] StopFile: stopped and cleared");
    }
}

bool IsFilePlaying()
{
    return gFileIsPlaying;
}

bool LoadBank(const char* bankName)
{
    SoundNameId bankId{ -1 };
    GetSoundManager()->LoadBank(bankId, bankName);
    return bankId != -1;
}

bool UnloadBank(const char* bankName)
{
    SoundNameId bankId = GetSoundManager()->GetIDFromString(bankName);
    return GetSoundManager()->UnloadBank(bankId);
}

bool PrepareBank(const char* bankName)
{
    SoundNameId bankId{ -1 };
    GetSoundManager()->PrepareBank(bankId, bankName);
    return bankId != -1;
}

bool UnprepareBank(const char* bankName)
{
    SoundNameId bankId = GetSoundManager()->GetIDFromString(bankName);
    return GetSoundManager()->UnloadBank(bankId);
}

void RegisterAudioLib()
{
    DECLARE_MODULE(Audio, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(SetSwitch)
    MODULE_FUNCTION(SetState)
    MODULE_FUNCTION(SetRTPC)
    MODULE_FUNCTION(GetRTPC)
    MODULE_FUNCTION(ResetRTPC)
    MODULE_FUNCTION(Stop)
    MODULE_FUNCTION(PauseAllSounds)
    MODULE_FUNCTION(ResumeAllSounds)
    MODULE_FUNCTION(PostEvent)
    MODULE_FUNCTION(LoadEvent)
    MODULE_FUNCTION(UnloadEvent)
    MODULE_FUNCTION(PlayExternalSound)
    MODULE_FUNCTION(PlayFile)
    MODULE_FUNCTION(PauseFile)
    MODULE_FUNCTION(ResumeFile)
    MODULE_FUNCTION(StopFile)
    MODULE_FUNCTION(IsFilePlaying)
    MODULE_FUNCTION(LoadBank)
    MODULE_FUNCTION(UnloadBank)
    MODULE_FUNCTION(PrepareBank)
    MODULE_FUNCTION(UnprepareBank)
    END_MODULE()
}

END_NS()
