#include <stdafx.h>
#include <Extender/Client/AccessibilityTrace/NativeUITrace.h>

#if defined(BG3ACCESS_NATIVE_UI_TRACE)

#if !defined(BG3ACCESS_VERBOSE)
#error BG3ACCESS_NATIVE_UI_TRACE requires the existing BG3ACCESS_VERBOSE logging configuration.
#endif

#include <CoreLib/Wrappers.h>

#include <intrin.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <string>
#include <type_traits>
#include <vector>

BEGIN_NS(ecl)

namespace AccessibilityTrace
{
    namespace
    {
        constexpr char kSessionId[] = "V6-DX11-RC1";
        constexpr char kCandidateId[] = "D-NOESIS-ROUTEDCOMMAND-EXECUTE";
        constexpr char kExtenderBaseCommit[] =
            "f80b9a9fbcf27ccd2ba7df4fa3925cfd95bb6f6f";
        constexpr char kSourceState[] = "uncommitted_v6_trace";
        constexpr wchar_t kExpectedExecutablePath[] =
            L"D:\\SteamLibrary\\steamapps\\common\\Baldurs Gate 3\\bin\\bg3_dx11.exe";
        constexpr char kExpectedExecutablePathUtf8[] =
            "D:\\SteamLibrary\\steamapps\\common\\Baldurs Gate 3\\bin\\bg3_dx11.exe";
        constexpr uint64_t kExpectedFileSize = 104363072;
        constexpr char kExpectedSha256[] =
            "e899c67cb90b9c6b0f052e3f758ba8615bc2012e52561c2af2e6a1e06ef61a2f";
        constexpr char kExpectedProductVersion[] = "4.1.1.7398727";
        constexpr uintptr_t kExpectedRva = 0x464CAB0;

        // Masked Ghidra signature for bg3_dx11.exe product 4.1.1.7398727.
        // A value of -1 is a wildcarded displacement byte.
        constexpr std::array<int16_t, 38> kRoutedCommandExecutePattern{
            0x4D, 0x85, 0xC0, 0x0F, 0x84, -1, -1, -1, -1,
            0x53, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, -1, -1, -1, -1,
            0x48, 0x2B, 0xE0, 0x49, 0x8B, 0xD8, 0x48, 0x89, 0x5C, 0x24,
            0x20, 0x48, 0x8B, 0x05, -1, -1, -1, -1
        };

        struct BinaryIdentity
        {
            bool ReadSucceeded{ false };
            std::wstring Path;
            std::string Utf8Path;
            uint64_t FileSize{ 0 };
            std::string Sha256;
            std::string ProductVersion;
            std::string Error;
        };

        struct SignatureResult
        {
            uint8_t* Address{ nullptr };
            size_t Count{ 0 };
            size_t ImageSize{ 0 };
        };

        struct LanguageCodePage
        {
            WORD Language;
            WORD CodePage;
        };

        struct StartupResult
        {
            bool Captured;
            bool RuntimeOptIn;
            bool IdentityChecked;
            bool IdentityMatched;
            bool SignatureChecked;
            size_t SignatureMatches;
            uintptr_t ResolvedRva;
            bool Installed;
            char const* Reason;
        };

        static_assert(std::is_trivially_default_constructible_v<StartupResult>);
        static_assert(std::is_trivially_destructible_v<StartupResult>);

        using RoutedCommandExecuteFunction = void(*)(void*, void*, void*);

        constinit RoutedCommandExecuteFunction
            gRoutedCommandExecuteOriginal{ nullptr };
        constinit uintptr_t gModuleBase{ 0 };
        constinit uintptr_t gModuleEnd{ 0 };
        constinit std::atomic<uint64_t> gSequence{ 0 };
        constinit bool gInitialized{ false };
        constinit bool gDetourAttachmentMayExist{ false };
        constinit bool gInstalled{ false };
        constinit StartupResult gStartupResult{};
        constinit bool gStartupSummaryLogged{ false };
        constinit char const* gLastSafePoint{ "static_initialization_only" };

        void CaptureStartupResult(bool runtimeOptIn, bool identityChecked,
            bool identityMatched, bool signatureChecked,
            size_t signatureMatches, uintptr_t resolvedRva, bool installed,
            char const* reason)
        {
            gStartupResult.RuntimeOptIn = runtimeOptIn;
            gStartupResult.IdentityChecked = identityChecked;
            gStartupResult.IdentityMatched = identityMatched;
            gStartupResult.SignatureChecked = signatureChecked;
            gStartupResult.SignatureMatches = signatureMatches;
            gStartupResult.ResolvedRva = resolvedRva;
            gStartupResult.Installed = installed;
            gStartupResult.Reason = reason;
            gStartupResult.Captured = true;
        }

        bool HashFileSha256(std::wstring const& path, std::string& hash,
            std::string& error)
        {
            HCRYPTPROV provider{ 0 };
            HCRYPTHASH hashHandle{ 0 };
            HANDLE file{ INVALID_HANDLE_VALUE };

            auto cleanup = [&]() {
                if (hashHandle != 0) {
                    CryptDestroyHash(hashHandle);
                }
                if (provider != 0) {
                    CryptReleaseContext(provider, 0);
                }
                if (file != INVALID_HANDLE_VALUE) {
                    CloseHandle(file);
                }
            };

            if (!CryptAcquireContextW(&provider, nullptr, nullptr,
                PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
                error = "CryptAcquireContextW failed";
                cleanup();
                return false;
            }

            if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hashHandle)) {
                error = "CryptCreateHash(CALG_SHA_256) failed";
                cleanup();
                return false;
            }

            file = CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                error = "CreateFileW failed";
                cleanup();
                return false;
            }

            std::array<BYTE, 64 * 1024> buffer{};
            for (;;) {
                DWORD bytesRead{ 0 };
                if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                    &bytesRead, nullptr)) {
                    error = "ReadFile failed";
                    cleanup();
                    return false;
                }
                if (bytesRead == 0) {
                    break;
                }

                if (!CryptHashData(hashHandle, buffer.data(), bytesRead, 0)) {
                    error = "CryptHashData failed";
                    cleanup();
                    return false;
                }
            }

            std::array<BYTE, 32> digest{};
            DWORD digestLength = static_cast<DWORD>(digest.size());
            if (!CryptGetHashParam(hashHandle, HP_HASHVAL, digest.data(),
                &digestLength, 0)
                || digestLength != static_cast<DWORD>(digest.size())) {
                error = "CryptGetHashParam(HP_HASHVAL) failed";
                cleanup();
                return false;
            }

            static constexpr char digits[] = "0123456789abcdef";
            hash.resize(digest.size() * 2);
            for (size_t index = 0; index < digest.size(); index++) {
                hash[index * 2] = digits[digest[index] >> 4];
                hash[index * 2 + 1] = digits[digest[index] & 0x0F];
            }

            cleanup();
            return true;
        }

        bool ReadProductVersion(std::wstring const& path,
            std::string& productVersion, std::string& error)
        {
            DWORD ignoredHandle{ 0 };
            auto versionInfoSize = GetFileVersionInfoSizeW(path.c_str(),
                &ignoredHandle);
            if (versionInfoSize == 0) {
                error = "GetFileVersionInfoSizeW failed";
                return false;
            }

            std::vector<uint8_t> versionInfo(versionInfoSize);
            if (!GetFileVersionInfoW(path.c_str(), 0, versionInfoSize,
                versionInfo.data())) {
                error = "GetFileVersionInfoW failed";
                return false;
            }

            LanguageCodePage* translations{ nullptr };
            UINT translationsLength{ 0 };
            if (!VerQueryValueW(versionInfo.data(), L"\\VarFileInfo\\Translation",
                reinterpret_cast<void**>(&translations), &translationsLength)
                || translations == nullptr
                || translationsLength < sizeof(LanguageCodePage)) {
                error = "ProductVersion translation table missing";
                return false;
            }

            auto translationCount = translationsLength / sizeof(LanguageCodePage);
            for (UINT index = 0; index < translationCount; index++) {
                std::array<wchar_t, 64> query{};
                auto queryLength = swprintf_s(query.data(), query.size(),
                    L"\\StringFileInfo\\%04x%04x\\ProductVersion",
                    translations[index].Language, translations[index].CodePage);
                if (queryLength <= 0) {
                    continue;
                }

                wchar_t* versionValue{ nullptr };
                UINT versionValueLength{ 0 };
                if (!VerQueryValueW(versionInfo.data(), query.data(),
                    reinterpret_cast<void**>(&versionValue), &versionValueLength)
                    || versionValue == nullptr || versionValueLength == 0) {
                    continue;
                }

                std::wstring wideVersion(versionValue, versionValueLength);
                while (!wideVersion.empty() && wideVersion.back() == L'\0') {
                    wideVersion.pop_back();
                }
                productVersion = ToStdUTF8(wideVersion);
                if (!productVersion.empty()) {
                    return true;
                }
            }

            error = "ProductVersion string missing";
            return false;
        }

        BinaryIdentity ReadBinaryIdentity(HMODULE module)
        {
            BinaryIdentity identity;
            std::array<wchar_t, 32768> pathBuffer{};
            auto pathLength = GetModuleFileNameW(module, pathBuffer.data(),
                static_cast<DWORD>(pathBuffer.size()));
            if (pathLength == 0 || pathLength >= pathBuffer.size()) {
                identity.Error = "GetModuleFileNameW failed or truncated";
                return identity;
            }

            identity.Path.assign(pathBuffer.data(), pathLength);
            identity.Utf8Path = ToStdUTF8(identity.Path);
            if (identity.Utf8Path.empty()) {
                identity.Error = "module path UTF-8 conversion failed";
                return identity;
            }

            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(identity.Path.c_str(), GetFileExInfoStandard,
                &attributes)) {
                identity.Error = "GetFileAttributesExW failed";
                return identity;
            }

            identity.FileSize =
                (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32)
                | attributes.nFileSizeLow;
            if (!HashFileSha256(identity.Path, identity.Sha256, identity.Error)) {
                return identity;
            }
            if (!ReadProductVersion(identity.Path, identity.ProductVersion,
                identity.Error)) {
                return identity;
            }

            identity.ReadSucceeded = true;
            return identity;
        }

        bool RuntimeOptInEnabled()
        {
            std::array<char, 8> value{};
            auto length = GetEnvironmentVariableA("BG3A_NATIVE_UI_TRACE",
                value.data(), static_cast<DWORD>(value.size()));
            return length == 1 && value[0] == '1';
        }

        bool MatchesPattern(uint8_t const* cursor)
        {
            for (size_t index = 0;
                index < kRoutedCommandExecutePattern.size(); index++) {
                auto expected = kRoutedCommandExecutePattern[index];
                if (expected >= 0
                    && cursor[index] != static_cast<uint8_t>(expected)) {
                    return false;
                }
            }

            return true;
        }

        SignatureResult FindRoutedCommandExecute(HMODULE module)
        {
            SignatureResult result;
            auto moduleBase = reinterpret_cast<uint8_t*>(module);
            auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER const*>(moduleBase);
            if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE
                || dosHeader->e_lfanew <= 0 || dosHeader->e_lfanew >= 0x1000) {
                return result;
            }

            auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(
                moduleBase + dosHeader->e_lfanew);
            if (ntHeaders->Signature != IMAGE_NT_SIGNATURE
                || ntHeaders->OptionalHeader.Magic
                    != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                return result;
            }

            auto imageSize = static_cast<size_t>(
                ntHeaders->OptionalHeader.SizeOfImage);
            result.ImageSize = imageSize;
            auto section = IMAGE_FIRST_SECTION(ntHeaders);
            auto sectionTableEnd = reinterpret_cast<uint8_t const*>(section
                + ntHeaders->FileHeader.NumberOfSections);
            if (sectionTableEnd > moduleBase + imageSize) {
                return result;
            }

            for (WORD sectionIndex = 0;
                sectionIndex < ntHeaders->FileHeader.NumberOfSections;
                sectionIndex++, section++) {
                if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0
                    || section->SizeOfRawData == 0
                    || section->Misc.VirtualSize == 0) {
                    continue;
                }

                auto sectionOffset = static_cast<size_t>(section->VirtualAddress);
                auto sectionSize = static_cast<size_t>(std::min(
                    section->SizeOfRawData, section->Misc.VirtualSize));
                if (sectionOffset >= imageSize) {
                    continue;
                }
                sectionSize = std::min(sectionSize, imageSize - sectionOffset);
                if (sectionSize < kRoutedCommandExecutePattern.size()) {
                    continue;
                }

                auto sectionStart = moduleBase + sectionOffset;
                auto lastOffset = sectionSize
                    - kRoutedCommandExecutePattern.size();
                for (size_t offset = 0; offset <= lastOffset; offset++) {
                    auto cursor = sectionStart + offset;
                    if (MatchesPattern(cursor)) {
                        result.Address = cursor;
                        result.Count++;
                    }
                }
            }

            return result;
        }

        __declspec(noinline) void RoutedCommandExecuteHook(void* command, void* parameter,
            void* target)
        {
            auto sequence = gSequence.fetch_add(1,
                std::memory_order_relaxed) + 1;
            auto returnAddress = _ReturnAddress();
            auto returnAddressValue = reinterpret_cast<uintptr_t>(returnAddress);
            auto callerInModule = returnAddressValue >= gModuleBase
                && returnAddressValue < gModuleEnd;
            auto callerRva = callerInModule
                ? returnAddressValue - gModuleBase : 0;
            BG3A_LOG("[BG3A-HOOK] session=%s candidate=%s event=command_execute seq=%llu thread=%lu rva=0x%llX caller=%p caller_in_module=%s caller_rva=0x%llX command=%p parameter=%p target=%p",
                kSessionId, kCandidateId,
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long>(GetCurrentThreadId()),
                static_cast<unsigned long long>(kExpectedRva), returnAddress,
                callerInModule ? "true" : "false",
                static_cast<unsigned long long>(callerRva), command, parameter, target);
            gRoutedCommandExecuteOriginal(command, parameter, target);
        }

        void LogCandidateStatus(bool enabled, bool resolved, bool installed,
            size_t matchCount, uintptr_t resolvedRva, char const* reason)
        {
            BG3A_LOG("[BG3A-HOOK] session=%s candidate=%s status enabled=%s resolved=%s installed=%s signature_matches=%llu resolved_rva=0x%llX expected_rva=0x%llX reason=%s",
                kSessionId, kCandidateId, enabled ? "true" : "false",
                resolved ? "true" : "false", installed ? "true" : "false",
                static_cast<unsigned long long>(matchCount),
                static_cast<unsigned long long>(resolvedRva),
                static_cast<unsigned long long>(kExpectedRva), reason);
        }
    }

    void Initialize()
    {
        if (gInitialized) {
            return;
        }
        gInitialized = true;
        gLastSafePoint = "initialize_entered";

        auto optIn = RuntimeOptInEnabled();
        if (!optIn) {
            CaptureStartupResult(false, false, false, false, 0, 0, false,
                "runtime_opt_in_disabled");
            gLastSafePoint = "runtime_opt_in_disabled_return";
            return;
        }
        gLastSafePoint = "runtime_opt_in_enabled";

        auto module = GetModuleHandleW(L"bg3_dx11.exe");
        if (module == nullptr) {
            CaptureStartupResult(true, false, false, false, 0, 0, false,
                "dx11_module_not_loaded");
            BG3A_LOG("[BG3A-HOOK] session=%s startup trace_compiled=true runtime_opt_in=true extender_base_commit=%s source_state=%s binary_path=unavailable binary_size=0 binary_sha256=unavailable product_version=unavailable identity_match=false enabled_candidate_ids=%s",
                kSessionId, kExtenderBaseCommit, kSourceState, kCandidateId);
            LogCandidateStatus(true, false, false, 0, 0,
                "dx11_module_not_loaded");
            return;
        }

        auto identity = ReadBinaryIdentity(module);
        auto pathMatches = identity.ReadSucceeded
            && _wcsicmp(identity.Path.c_str(), kExpectedExecutablePath) == 0;
        auto sizeMatches = identity.ReadSucceeded
            && identity.FileSize == kExpectedFileSize;
        auto hashMatches = identity.ReadSucceeded
            && identity.Sha256 == kExpectedSha256;
        auto versionMatches = identity.ReadSucceeded
            && identity.ProductVersion == kExpectedProductVersion;
        auto identityMatches = pathMatches && sizeMatches && hashMatches
            && versionMatches;

        BG3A_LOG("[BG3A-HOOK] session=%s startup trace_compiled=true runtime_opt_in=true extender_base_commit=%s source_state=%s binary_path=\"%s\" binary_size=%llu binary_sha256=%s product_version=%s identity_match=%s enabled_candidate_ids=%s",
            kSessionId, kExtenderBaseCommit, kSourceState,
            identity.Utf8Path.empty() ? "unavailable" : identity.Utf8Path.c_str(),
            static_cast<unsigned long long>(identity.FileSize),
            identity.Sha256.empty() ? "unavailable" : identity.Sha256.c_str(),
            identity.ProductVersion.empty()
                ? "unavailable" : identity.ProductVersion.c_str(),
            identityMatches ? "true" : "false", kCandidateId);

        if (!identity.ReadSucceeded) {
            CaptureStartupResult(true, true, false, false, 0, 0, false,
                "identity_read_failed");
            BG3A_LOG("[BG3A-HOOK] session=%s candidate=%s failure=identity_read_failed detail=%s",
                kSessionId, kCandidateId,
                identity.Error.empty() ? "unknown" : identity.Error.c_str());
            LogCandidateStatus(true, false, false, 0, 0,
                "identity_read_failed");
            return;
        }
        if (!identityMatches) {
            CaptureStartupResult(true, true, false, false, 0, 0, false,
                "exact_binary_identity_mismatch");
            LogCandidateStatus(true, false, false, 0, 0,
                "exact_binary_identity_mismatch");
            return;
        }

        auto signature = FindRoutedCommandExecute(module);
        auto resolvedRva = signature.Address == nullptr ? 0
            : static_cast<uintptr_t>(signature.Address
                - reinterpret_cast<uint8_t*>(module));
        auto resolved = signature.Count == 1 && resolvedRva == kExpectedRva;
        if (!resolved) {
            auto reason = signature.Count == 0 ? "signature_missing"
                : signature.Count > 1 ? "signature_ambiguous"
                : "signature_rva_mismatch";
            CaptureStartupResult(true, true, true, true, signature.Count,
                resolvedRva, false, reason);
            LogCandidateStatus(true, false, false, signature.Count,
                resolvedRva, reason);
            return;
        }

        auto beginStatus = DetourTransactionBegin();
        if (beginStatus != NO_ERROR) {
            CaptureStartupResult(true, true, true, true, signature.Count,
                resolvedRva, false, "detour_transaction_begin_failed");
            LogCandidateStatus(true, true, false, signature.Count,
                resolvedRva, "detour_transaction_begin_failed");
            return;
        }

        auto updateStatus = DetourUpdateThread(GetCurrentThread());
        if (updateStatus != NO_ERROR) {
            DetourTransactionAbort();
            CaptureStartupResult(true, true, true, true, signature.Count,
                resolvedRva, false, "detour_update_thread_failed");
            LogCandidateStatus(true, true, false, signature.Count,
                resolvedRva, "detour_update_thread_failed");
            return;
        }

        gRoutedCommandExecuteOriginal =
            reinterpret_cast<RoutedCommandExecuteFunction>(signature.Address);
        gModuleBase = reinterpret_cast<uintptr_t>(module);
        gModuleEnd = gModuleBase + signature.ImageSize;
        auto attachStatus = DetourAttach(
            reinterpret_cast<PVOID*>(&gRoutedCommandExecuteOriginal),
            reinterpret_cast<PVOID>(&RoutedCommandExecuteHook));
        if (attachStatus != NO_ERROR) {
            DetourTransactionAbort();
            gRoutedCommandExecuteOriginal = nullptr;
            gModuleBase = 0;
            gModuleEnd = 0;
            CaptureStartupResult(true, true, true, true, signature.Count,
                resolvedRva, false, "detour_attach_failed");
            LogCandidateStatus(true, true, false, signature.Count,
                resolvedRva, "detour_attach_failed");
            return;
        }

        gRegisteredTrampolines.insert(ResolveRealFunctionAddress(
            reinterpret_cast<void const*>(&RoutedCommandExecuteHook)));
        gDetourAttachmentMayExist = true;
        auto commitStatus = DetourTransactionCommit();
        if (commitStatus != NO_ERROR) {
            CaptureStartupResult(true, true, true, true, signature.Count,
                resolvedRva, false, "detour_transaction_commit_failed");
            LogCandidateStatus(true, true, false, signature.Count,
                resolvedRva, "detour_transaction_commit_failed");
            return;
        }

        gInstalled = true;
        CaptureStartupResult(true, true, true, true, signature.Count,
            resolvedRva, true, "installed");
        LogCandidateStatus(true, true, true, signature.Count, resolvedRva,
            "installed");
    }

    void LogStartupSummary()
    {
        if (!gStartupResult.Captured || gStartupSummaryLogged) {
            return;
        }

        gStartupSummaryLogged = true;
        BG3A_LOG("[BG3A-HOOK] session=%s candidate=%s startup_summary trace_compiled=true runtime_opt_in=%s identity_checked=%s identity_match=%s signature_checked=%s signature_matches=%llu resolved_rva=0x%llX installed=%s reason=%s last_safe_point=%s",
            kSessionId, kCandidateId,
            gStartupResult.RuntimeOptIn ? "true" : "false",
            gStartupResult.IdentityChecked ? "true" : "false",
            gStartupResult.IdentityMatched ? "true" : "false",
            gStartupResult.SignatureChecked ? "true" : "false",
            static_cast<unsigned long long>(
                gStartupResult.SignatureMatches),
            static_cast<unsigned long long>(gStartupResult.ResolvedRva),
            gStartupResult.Installed ? "true" : "false",
            gStartupResult.Reason,
            gLastSafePoint == nullptr ? "unavailable" : gLastSafePoint);
    }

    void Shutdown()
    {
        if (!gDetourAttachmentMayExist
            || gRoutedCommandExecuteOriginal == nullptr) {
            return;
        }

        auto beginStatus = DetourTransactionBegin();
        if (beginStatus != NO_ERROR) {
            BG3A_LOG("[BG3A-HOOK] session=%s candidate=%s shutdown installed=%s detached=false reason=detour_transaction_begin_failed",
                kSessionId, kCandidateId, gInstalled ? "true" : "false");
            return;
        }

        auto updateStatus = DetourUpdateThread(GetCurrentThread());
        if (updateStatus != NO_ERROR) {
            DetourTransactionAbort();
            BG3A_LOG("[BG3A-HOOK] session=%s candidate=%s shutdown installed=%s detached=false reason=detour_update_thread_failed",
                kSessionId, kCandidateId, gInstalled ? "true" : "false");
            return;
        }

        auto detachStatus = DetourDetach(
            reinterpret_cast<PVOID*>(&gRoutedCommandExecuteOriginal),
            reinterpret_cast<PVOID>(&RoutedCommandExecuteHook));
        if (detachStatus != NO_ERROR) {
            DetourTransactionAbort();
            BG3A_LOG("[BG3A-HOOK] session=%s candidate=%s shutdown installed=%s detached=false reason=detour_detach_failed",
                kSessionId, kCandidateId, gInstalled ? "true" : "false");
            return;
        }

        auto commitStatus = DetourTransactionCommit();
        auto detached = commitStatus == NO_ERROR;
        BG3A_LOG("[BG3A-HOOK] session=%s candidate=%s shutdown installed=%s detached=%s reason=%s",
            kSessionId, kCandidateId, gInstalled ? "true" : "false",
            detached ? "true" : "false",
            detached ? "detached" : "detour_transaction_commit_failed");
        if (detached) {
            gRoutedCommandExecuteOriginal = nullptr;
            gModuleBase = 0;
            gModuleEnd = 0;
            gDetourAttachmentMayExist = false;
            gInstalled = false;
        }
    }
}

END_NS()

#endif
