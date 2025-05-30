#pragma once

BEGIN_NS(lua)

template <class TArgs, class TReturn>
struct ProtectedFunctionCaller;

template <class T>
class LuaDelegate;

template<class TRet, class ...TArgs>
class LuaDelegate<TRet(TArgs...)>
{
public:
    using Function = TRet(TArgs...);
    using ArgumentTuple = std::tuple<TArgs...>;

    inline LuaDelegate() {}
    inline LuaDelegate(lua_State* L, int index)
        : ref_(L, index)
    {}

    inline LuaDelegate(lua_State* L, Ref const& local)
        : ref_(L, local)
    {}

    inline LuaDelegate(lua_State* L, FunctionRef const& f)
        : ref_(L, f.Index)
    {}

    inline ~LuaDelegate() {}

    inline LuaDelegate(LuaDelegate const& o)
        : ref_(o.ref_)
    {}

    inline LuaDelegate(LuaDelegate && o)
    {
        ref_ = std::move(o.ref_);
    }

    inline LuaDelegate& operator = (LuaDelegate const& o)
    {
        ref_ = o.ref_;
        return *this;
    }
    
    inline LuaDelegate& operator = (LuaDelegate && o)
    {
        ref_ = std::move(o.ref_);
        return *this;
    }

    explicit inline operator bool() const
    {
        return (bool)ref_;
    }

    inline void Push(lua_State* L) const
    {
        ref_.Push(L);
    }

    TRet Call(lua_State* L, TArgs... args)
    {
        if constexpr (std::is_same_v<TRet, void>) {
            Call(L, std::tuple(args...));
        } else {
            return Call(L, std::tuple(args...));
        }
    }

    TRet Call(lua_State* L, ArgumentTuple const& args)
    {
        EnterVMCheck(L);
        StackCheck _(L);

        ref_.Push(L);
        Ref func(L, lua_absindex(L, -1));

        ProtectedFunctionCaller<ArgumentTuple, TRet> caller{ func, args };

        if constexpr (std::is_same_v<TRet, void>) {
            caller.Call(L);
            lua_pop(L, 1);
        } else {
            auto rval = caller.Call(L);
            lua_pop(L, 1);
            return rval;
        }
    }

private:
    RegistryEntry ref_;
};

template <class T>
class DeferredLuaDelegateCall;

template <class TRet, class... TArgs>
class DeferredLuaDelegateCall<TRet (TArgs...)>
{
public:
    DeferredLuaDelegateCall(LuaDelegate<TRet (TArgs...)> delegate, TArgs... args)
        : delegate_(delegate), args_(std::tuple(args...))
    {}

    TRet Call(lua_State* L)
    {
        if constexpr (std::is_same_v<TRet, void>) {
            delegate_.Call(L, args_);
        } else {
            return delegate_.Call(L, args_);
        }
    }

private:
    LuaDelegate<TRet(TArgs...)> delegate_;
    std::tuple<TArgs...> args_;
};

class GenericDeferredLuaDelegateCall
{
public:
    virtual ~GenericDeferredLuaDelegateCall() {}
    virtual void Call(lua_State* L) = 0;
};

template <class TRet, class... TArgs>
class DeferredLuaDelegateCallImpl : public GenericDeferredLuaDelegateCall
{
public:
    DeferredLuaDelegateCallImpl(LuaDelegate<TRet(TArgs...)> const& delegate, TArgs... args)
        : call_(delegate, args...)
    {}

    ~DeferredLuaDelegateCallImpl() override {}

    void Call(lua_State* L) override
    {
        call_.Call(L);
    }

private:
    DeferredLuaDelegateCall<TRet(TArgs...)> call_;
};

class DeferredLuaDelegateQueue
{
public:
    template <class TRet, class... TArgs>
    void Call(LuaDelegate<TRet(TArgs...)> const& delegate, TArgs... args)
    {
        if (!delegate) return;

        auto call = GameAlloc<DeferredLuaDelegateCallImpl<TRet, TArgs...>>(delegate, std::forward<TArgs>(args)...);
        queue_.push_back(call);
    }
    
    template <class TRet, class... TArgs>
    void Call(LuaDelegate<TRet(TArgs...)> && delegate, TArgs... args)
    {
        if (!delegate) return;

        auto call = GameAlloc<DeferredLuaDelegateCallImpl<TRet, TArgs...>>(std::move(delegate), std::forward<TArgs>(args)...);
        queue_.push_back(call);
    }

    void Flush(lua_State* L);
    void Flush();

private:
    Array<GenericDeferredLuaDelegateCall*> queue_;
};

END_NS()
