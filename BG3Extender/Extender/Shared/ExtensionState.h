#pragma once

#include <Extender/Shared/ScriptExtenderBase.h>
#include "ExtensionHelpers.h"
#include "Lua/LuaBinding.h"
#include <random>
#include <unordered_set>

namespace Json { class Value; }

namespace bg3se
{
    class FileReaderPin;

    struct ExtensionModConfig
    {
        uint32_t MinimumVersion{ 0 };
        // Name to use in Lua Mods global table (>= v43)
        STDString ModTable;
        std::unordered_set<STDString> FeatureFlags;
    };

    enum class ExtensionStateContext
    {
        Uninitialized,
        // Game context; created when the engine leaves LoadModule state
        // and all static game data is loaded
        Game,
        // Load context; created when the engine enters LoadModule
        Load,
        Max = Load
    };

    char const* ContextToString(ExtensionStateContext ctx);

    class ExtensionStateBase : Noncopyable<ExtensionStateBase>
    {
    public:
        using PostResetCallback = std::function<void()>;
        std::mt19937_64 OsiRng;

        static ExtensionStateBase& FromLua(lua_State* L);

        ExtensionStateBase(bool isServer);
        virtual ~ExtensionStateBase();

        virtual void Reset();
        virtual lua::State * GetLua() = 0;
        virtual ModManager * GetModManager() = 0;
        virtual LevelManager* GetLevelManager() = 0;
        virtual char const * GetBootstrapFileName() = 0;

        void LoadConfigs();
        bool LoadConfig(Module const & mod, STDString const & configText, ExtensionModConfig & config);
        bool LoadConfig(Module const & mod, Json::Value & json, ExtensionModConfig & config);
        bool HasFeatureFlag(char const * flag) const;

        inline ExtensionStateContext Context() const
        {
            return context_;
        }

        inline bool WasStatLoadTriggered() const
        {
            return StatLoadTriggered;
        }

        inline DWORD GetOwningThread() const
        {
            return owningThread_;
        }

        inline GameTime const& Time() const
        {
            return time_;
        }

        virtual void OnGameSessionLoading();
        virtual void OnGameSessionLoaded();
        void OnModuleLoadStarted();
        void OnStatsLoaded();
        void OnModuleResume();
        void OnResetCompleted();
        virtual void OnUpdate(GameTime const& time);

        inline void EnteredCheck() const
        {
            se_assert(luaRefs_ > 0);
            se_assert(owningThread_ == GetCurrentThreadId());
        }

        void IncLuaRefs();
        void DecLuaRefs(bool releaseVm);
        void RequestLuaReset(ExtensionStateContext nextContext, bool startup);
        void RequestLuaReset(bool startup);
        void AddPostResetCallback(PostResetCallback callback);

        std::optional<STDString> ResolveModScriptPath(STDString const& modNameGuid, STDString const& fileName);
        STDString ResolveModScriptPath(Module const& mod, STDString const& fileName);

        std::optional<int> LuaLoadExternalFile(STDString const & path);
        std::optional<int> LuaLoadGameFile(FileReaderPin & reader, STDString const & scriptName, int globalsIdx = 0);
        std::optional<int> LuaLoadGameFile(STDString const & path, STDString const & scriptName, 
            bool warnOnError = true, int globalsIdx = 0);
        std::optional<int> LuaLoadModScript(STDString const & modNameGuid, STDString const & fileName, 
            bool warnOnError = true, int globalsIdx = 0);
        std::optional<int> LuaLoadBuiltinFile(STDString const& fileName, bool warnOnError = true, int globalsIdx = 0);
        std::optional<int> LuaLoadFile(STDString const& path, STDString const& scriptName, bool warnOnError = true, int globalsIdx = 0);

        inline std::unordered_map<FixedString, ExtensionModConfig> const& GetConfigs() const
        {
            return modConfigs_;
        }

        inline std::unordered_map<STDString, STDString> const& GetLoadedFiles() const
        {
            return loadedFiles_;
        }

        inline std::unordered_map<STDString, STDString> const& GetLoadedFileFullPaths() const
        {
            return loadedFileFullPaths_;
        }

        inline UserVariableManager& GetUserVariables()
        {
            return userVariables_;
        }

        inline ModVariableManager& GetModVariables()
        {
            return modVariables_;
        }

        inline uint32_t GetEnterCount() const
        {
            return luaRefs_;
        }

        inline bool IsServer() const
        {
            return server_;
        }

    protected:
        friend class LuaVirtualPin;
        static std::unordered_set<std::string_view> sAllFeatureFlags;

        ExtensionModConfig MergedConfig;
        Module const* HighestVersionMod{ nullptr };
        std::unordered_map<FixedString, ExtensionModConfig> modConfigs_;

        std::recursive_mutex luaMutex_;
        std::atomic<uint32_t> luaRefs_{ 0 };
        DWORD owningThread_{ 0 };
        std::vector<PostResetCallback> luaPostResetCallbacks_;
        bool LuaPendingDelete{ false };
        bool LuaPendingStartup{ false };
        bool StatLoadTriggered{ false };
        bool server_{ false };
        ExtensionStateContext context_{ ExtensionStateContext::Uninitialized };
        ExtensionStateContext nextContext_{ ExtensionStateContext::Uninitialized };

        // Keep track of the list of loaded files so we can pass them to the debugger
        std::unordered_map<STDString, STDString> loadedFiles_;
        std::unordered_map<STDString, STDString> loadedFileFullPaths_;

        UserVariableManager userVariables_;
        ModVariableManager modVariables_;
        GameTime time_;

        void LuaResetInternal();
        virtual void ShutdownLuaState() = 0;
        virtual void InitializeLuaState() = 0;
        virtual void BootstrapLua();
        void LuaLoadGameBootstrap(ExtensionModConfig const& config, Module const& mod);
        void LuaLoadPreinitBootstrap(ExtensionModConfig const& config, Module const& mod);
    };

    ExtensionStateBase* GetCurrentExtensionState();


    class LuaVirtualPin
    {
    public:
        inline LuaVirtualPin(ExtensionStateBase& state)
            : state_(&state)
        {
            if (state_) state_->IncLuaRefs();
        }

        inline LuaVirtualPin(ExtensionStateBase* state)
            : state_(state)
        {
            if (state_) state_->IncLuaRefs();
        }

        inline LuaVirtualPin()
            : state_(GetCurrentExtensionState())
        {
            if (state_) state_->IncLuaRefs();
        }

        inline ~LuaVirtualPin()
        {
            if (state_) state_->DecLuaRefs(true);
        }

        inline operator bool() const
        {
            return state_ 
                && !state_->LuaPendingDelete
                && state_->GetLua();
        }

        inline lua::State & Get() const
        {
            se_assert(*this);
            return *state_->GetLua();
        }

        inline lua::State & operator *() const
        {
            se_assert(*this);
            return *state_->GetLua();
        }

        inline lua::State * operator ->() const
        {
            se_assert(*this);
            return state_->GetLua();
        }

    private:
        ExtensionStateBase* state_;
    };


    template <class T, class TLua>
    class LuaStatePin
    {
    public:
        inline LuaStatePin(T & state)
            : state_(state)
        {
            se_assert((state_.IsServer() ? ContextType::Server : ContextType::Client) == GetCurrentContextType());
            state_.IncLuaRefs();
        }

        inline ~LuaStatePin()
        {
            state_.DecLuaRefs(true);
            se_assert((state_.IsServer() ? ContextType::Server : ContextType::Client) == GetCurrentContextType());
        }

        LuaStatePin(LuaStatePin const& state) = delete;
        LuaStatePin& operator =(LuaStatePin const& state) = delete;

        inline operator bool() const
        {
            return !state_.LuaPendingDelete
                && state_.Lua;
        }

        inline TLua & Get() const
        {
            se_assert(*this);
            return *state_.Lua;
        }

        inline TLua & operator *() const
        {
            se_assert(*this);
            return *state_.Lua;
        }

        inline TLua * operator ->() const
        {
            se_assert(*this);
            return state_.Lua.get();
        }

    private:
        T & state_;
    };

}