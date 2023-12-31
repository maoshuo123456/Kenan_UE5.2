//*********************************************************
//
//    Copyright (c) Microsoft. All rights reserved.
//    This code is licensed under the MIT License.
//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
//    ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//    TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//    PARTICULAR PURPOSE AND NONINFRINGEMENT.
//
//*********************************************************
#ifndef __WIL_WRL_INCLUDED
#define __WIL_WRL_INCLUDED

#include "NNEThirdPartyWarningDisabler.h" // WITH_UE
NNE_THIRD_PARTY_INCLUDES_START
#undef check
#undef TEXT

#include <wrl.h>
#include "result.h"
#include "common.h" // wistd type_traits helpers

namespace wil
{

#ifdef WIL_ENABLE_EXCEPTIONS
#pragma region Object construction helpers that throw exceptions

    /** Used to construct a RuntimeClass based object that uses 2 phase construction.
    Construct a RuntimeClass based object that uses 2 phase construction (by implementing
    RuntimeClassInitialize() and returning error codes for failures.
    ~~~~
        // SomeClass uses 2 phase initialization by implementing RuntimeClassInitialize()
        auto someClass = MakeAndInitializeOrThrow<SomeClass>(L"input", true);
    ~~~~ */

    template <typename T, typename... TArgs>
    Microsoft::WRL::ComPtr<T> MakeAndInitializeOrThrow(TArgs&&... args)
    {
        Microsoft::WRL::ComPtr<T> obj;
        THROW_IF_FAILED(Microsoft::WRL::MakeAndInitialize<T>(&obj, Microsoft::WRL::Details::Forward<TArgs>(args)...));
        return obj;
    }

    /** Used to construct an RuntimeClass based object that uses exceptions in its constructor (and does
    not require 2 phase construction).
    ~~~~
        // SomeClass uses exceptions for error handling in its constructor.
        auto someClass = MakeOrThrow<SomeClass>(L"input", true);
    ~~~~ */

    template <typename T, typename... TArgs>
    Microsoft::WRL::ComPtr<T> MakeOrThrow(TArgs&&... args)
    {
        // This is how you can detect the presence of RuntimeClassInitialize() and find dangerous use.
        // Unfortunately this produces false positives as all RuntimeClass derived classes have
        // a RuntimeClassInitialize() method from their base class.
        // static_assert(!std::is_member_function_pointer<decltype(&T::RuntimeClassInitialize)>::value,
        //    "class has a RuntimeClassInitialize member, use MakeAndInitializeOrThrow instead");
        auto obj = Microsoft::WRL::Make<T>(Microsoft::WRL::Details::Forward<TArgs>(args)...);
        THROW_IF_NULL_ALLOC(obj.Get());
        return obj;
    }
#pragma endregion

#else
#ifdef WITH_UE
    template <typename T, typename... TArgs>
    Microsoft::WRL::ComPtr<T> MakeOrThrow(TArgs&&... args)
    {
        return Microsoft::WRL::Make<T>(Microsoft::WRL::Details::Forward<TArgs>(args)...);
    }
#endif // WITH_UE
#endif // WIL_ENABLE_EXCEPTIONS

    /** By default WRL Callback objects are not agile, use this to make an agile one. Replace use of Callback<> with MakeAgileCallback<>.
    Will return null on failure, translate that into E_OUTOFMEMORY using XXX_IF_NULL_ALLOC()
    from wil\result.h to test the result. */
    template<typename TDelegateInterface, typename ...Args>
    ::Microsoft::WRL::ComPtr<TDelegateInterface> MakeAgileCallbackNoThrow(Args&&... args) WI_NOEXCEPT
    {
        using namespace Microsoft::WRL;
        return Callback<Implements<RuntimeClassFlags<ClassicCom>, TDelegateInterface, FtmBase>>(wistd::forward<Args>(args)...);
    }

#ifdef WIL_ENABLE_EXCEPTIONS
    template<typename TDelegateInterface, typename ...Args>
    ::Microsoft::WRL::ComPtr<TDelegateInterface> MakeAgileCallback(Args&&... args)
    {
        auto result = MakeAgileCallbackNoThrow<TDelegateInterface, Args...>(wistd::forward<Args>(args)...);
        THROW_IF_NULL_ALLOC(result);
        return result;
    }
#endif // WIL_ENABLE_EXCEPTIONS
} // namespace wil

NNE_THIRD_PARTY_INCLUDES_END // WITH_UE

#endif // __WIL_WRL_INCLUDED
