#include "stdafx.h"
#include "GameModUpdater.h"
#include <CoreLib/Crypto.h>
#include <CoreLib/Utils.h>
#include <CoreLib/Console.h>
#include "ZipLib/ZipFile.h"
#include <ShlObj.h>
#include <shellapi.h>   // SHFILEOPSTRUCTW, SHFileOperationW
#include <Shlwapi.h>    // PathFileExistsW
#include <fstream>
#include <sstream>
#include <algorithm>

BEGIN_SE()

// Version marker file lives INSIDE the install path so the loader
// (which knows InstallPath but not BG3's user profile setup) can
// read it on subsequent launches to compare against the manifest.
// Dot-prefixed so it sorts first in listings and stands out from
// mod content.
static constexpr wchar_t const* VERSION_MARKER_FILE = L".bg3access_version.txt";

// Update notice file lives in the BG3 user profile (see
// GetBG3UserProfileScriptExtenderDir).  Lua reads it via
// Ext.IO.LoadFile("<resourceName>/update_notice.json").  Notice
// filename is plain (no dot prefix) so Lua callers don't need to
// special-case hidden-file syntax.
static constexpr wchar_t const* UPDATE_NOTICE_FILENAME = L"update_notice.json";

// Suffixes used during the atomic swap.  Both live next to the install
// path under the BG3 root.
static constexpr wchar_t const* STAGING_SUFFIX = L".staging";
static constexpr wchar_t const* BACKUP_SUFFIX  = L".backup";

// Content fetch timeout for mod packages.  Same value the extender
// flow uses for its package downloads.
static constexpr long MOD_PACKAGE_FETCH_TIMEOUT = 60000;


GameModUpdater::GameModUpdater(HttpFetcher& fetcher, UpdaterConfig const& config)
    : fetcher_(fetcher), config_(config)
{}


std::wstring GameModUpdater::GetBG3InstallRoot()
{
    // bg3.exe lives in <BG3>\bin\.  Walk up twice to get to <BG3>.
    // GetModuleFileNameW(NULL, ...) returns the path of the process
    // image (bg3.exe) regardless of which DLL is calling.
    wchar_t exePath[MAX_PATH * 2];
    auto len = GetModuleFileNameW(NULL, exePath, std::size(exePath));
    if (len == 0 || len >= std::size(exePath)) {
        return {};
    }
    exePath[len] = L'\0';

    // Strip the exe name: <BG3>\bin\bg3.exe -> <BG3>\bin
    auto sep = wcsrchr(exePath, L'\\');
    if (!sep) {
        return {};
    }
    *sep = L'\0';

    // Strip "bin": <BG3>\bin -> <BG3>
    sep = wcsrchr(exePath, L'\\');
    if (!sep) {
        return {};
    }
    *sep = L'\0';

    return std::wstring(exePath);
}


std::wstring GameModUpdater::GetUpdatesTempDir()
{
    // %LOCALAPPDATA% via SHGetFolderPath -- same place the existing
    // extender cache lives, so we reuse that root directory.  Updates/
    // is a sibling of ScriptExtender/ under it.
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buf) != S_OK) {
        return {};
    }

    std::wstring path = buf;
    path += L"\\BG3ScriptExtender\\Updates\\";

    // Ensure the directory exists.  SHCreateDirectoryExW creates
    // intermediate folders.
    SHCreateDirectoryExW(NULL, path.c_str(), NULL);
    return path;
}


std::wstring GameModUpdater::GetBG3UserProfileScriptExtenderDir()
{
    // BG3 stores its user profile under
    // <LOCALAPPDATA>\Larian Studios\Baldur's Gate 3\, with a Script
    // Extender subfolder reserved for BG3SE storage.  Ext.IO.LoadFile
    // (default "user" context) reads paths relative to this directory.
    // We write notice files here so Lua can read them on session
    // start; the Data/Mods path BG3 loads mods from is not accessible
    // to Ext.IO.
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buf) != S_OK) {
        return {};
    }

    std::wstring path = buf;
    path += L"\\Larian Studios\\Baldur's Gate 3\\Script Extender\\";

    // Ensure the directory exists.  BG3 itself will also create this
    // on first launch, but we may run before BG3 does.
    SHCreateDirectoryExW(NULL, path.c_str(), NULL);
    return path;
}


std::optional<VersionNumber> GameModUpdater::ReadInstalledVersion(
    std::wstring const& installPath)
{
    auto versionFile = installPath + L"\\" + VERSION_MARKER_FILE;
    std::string contents;
    if (!LoadFile(versionFile, contents)) {
        return {};
    }

    // Trim whitespace -- a stray trailing newline shouldn't break
    // version parsing.
    while (!contents.empty()
           && (contents.back() == '\n' || contents.back() == '\r'
               || contents.back() == ' ' || contents.back() == '\t')) {
        contents.pop_back();
    }

    return VersionNumber::FromString(contents.c_str());
}


bool GameModUpdater::DeleteFolderRecursive(std::wstring const& path)
{
    // SHFileOperationW with FO_DELETE handles recursive directory
    // delete.  Path must be double-null-terminated (pFrom is a list).
    std::vector<wchar_t> pathBuf(path.size() + 2, L'\0');
    std::copy(path.begin(), path.end(), pathBuf.begin());

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = pathBuf.data();
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI
              | FOF_SILENT | FOF_NOCONFIRMMKDIR;
    auto result = SHFileOperationW(&op);
    return result == 0 && !op.fAnyOperationsAborted;
}


void GameModUpdater::SweepLeftoverState(Manifest const& manifest)
{
    // 1. Wipe Updates temp dir contents.  Anything left there is
    //    from a previous interrupted run; we never need it again.
    auto updatesDir = GetUpdatesTempDir();
    if (!updatesDir.empty()) {
        WIN32_FIND_DATAW findData;
        auto searchPattern = updatesDir + L"*";
        HANDLE find = FindFirstFileW(searchPattern.c_str(), &findData);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(findData.cFileName, L".") == 0
                    || wcscmp(findData.cFileName, L"..") == 0) {
                    continue;
                }
                auto fullPath = updatesDir + findData.cFileName;
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    DeleteFolderRecursive(fullPath);
                } else {
                    DeleteFileW(fullPath.c_str());
                }
            } while (FindNextFileW(find, &findData));
            FindClose(find);
        }
    }

    // 2. For each GameMod in the manifest, sweep .staging / .backup
    //    folders that may have been left behind next to its
    //    InstallPath.  These shouldn't survive past one launch.
    auto bg3Root = GetBG3InstallRoot();
    if (bg3Root.empty()) {
        return;
    }

    for (auto const& [name, resource] : manifest.Resources) {
        if (resource.Type != Manifest::TypeGameMod || resource.InstallPath.empty()) {
            continue;
        }

        auto installPath = bg3Root + L"\\" + FromStdUTF8(resource.InstallPath);
        // Normalize forward slashes to backslashes.
        std::replace(installPath.begin(), installPath.end(), L'/', L'\\');

        auto stagingPath = installPath + STAGING_SUFFIX;
        auto backupPath  = installPath + BACKUP_SUFFIX;

        if (PathFileExistsW(stagingPath.c_str())) {
            DEBUG("Cleaning leftover staging: %s", ToStdUTF8(stagingPath).c_str());
            DeleteFolderRecursive(stagingPath);
        }
        if (PathFileExistsW(backupPath.c_str())) {
            DEBUG("Cleaning leftover backup: %s", ToStdUTF8(backupPath).c_str());
            DeleteFolderRecursive(backupPath);
        }
    }
}


bool GameModUpdater::ValidateStaging(std::wstring const& stagingPath)
{
    // Look for meta.lsx anywhere in the staging tree.  BG3 loads
    // mods by reading meta.lsx; if it's missing the extraction is
    // broken (truncated download, corrupt zip, etc.) and we should
    // not commit the swap.
    //
    // The mod folder layout is <staging>\<UUID>\meta.lsx or
    // <staging>\meta.lsx depending on how the package was authored.
    // Recursive scan is simplest and runs once per update.
    std::vector<std::wstring> stack{ stagingPath };
    while (!stack.empty()) {
        auto current = stack.back();
        stack.pop_back();

        WIN32_FIND_DATAW findData;
        auto pattern = current + L"\\*";
        HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
        if (find == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (wcscmp(findData.cFileName, L".") == 0
                || wcscmp(findData.cFileName, L"..") == 0) {
                continue;
            }
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(current + L"\\" + findData.cFileName);
            } else if (_wcsicmp(findData.cFileName, L"meta.lsx") == 0) {
                FindClose(find);
                return true;
            }
        } while (FindNextFileW(find, &findData));
        FindClose(find);
    }

    return false;
}


OperationResult GameModUpdater::DownloadPackage(std::string const& url,
                                                std::wstring const& destPath)
{
    DEBUG("Downloading mod package: %s", url.c_str());
    std::vector<char> response;
    fetcher_.TransferCategory = ErrorCategory::UpdateDownload;
    fetcher_.Timeout = MOD_PACKAGE_FETCH_TIMEOUT;
    auto result = fetcher_.Fetch(url, response);
    if (!result) {
        result.error().Message = std::string("Unable to download mod package - ") + result.error().Message;
        return result;
    }

    if (!SaveFile(destPath, std::string_view(response.data(), response.size()))) {
        return ErrorReason{ ErrorCategory::LocalUpdate,
            std::string("Failed to write downloaded package: ") + ToStdUTF8(destPath) };
    }

    return OperationSuccessful{};
}


OperationResult GameModUpdater::ExtractPackage(std::wstring const& packagePath,
                                               std::wstring const& destFolder)
{
    // Create the destination folder.  SHCreateDirectoryExW handles
    // intermediate components.
    SHCreateDirectoryExW(NULL, destFolder.c_str(), NULL);

    auto archive = ZipFile::Open(packagePath);
    if (!archive) {
        return ErrorReason{ ErrorCategory::LocalUpdate,
            "Unable to read mod package archive, file possibly corrupted" };
    }

    auto entries = archive->GetEntriesCount();
    for (size_t i = 0; i < entries; i++) {
        auto entry = archive->GetEntry((int)i);
        if (entry->IsDirectory()) continue;

        auto entryPath = FromStdUTF8(entry->GetFullName());
        std::replace(entryPath.begin(), entryPath.end(), L'/', L'\\');
        auto outPath = destFolder + L"\\" + entryPath;

        // Ensure subdirectories exist before writing.
        auto lastSep = outPath.find_last_of(L'\\');
        if (lastSep != std::wstring::npos) {
            auto parentDir = outPath.substr(0, lastSep);
            SHCreateDirectoryExW(NULL, parentDir.c_str(), NULL);
        }

        std::ofstream out(outPath.c_str(), std::ios::out | std::ios::binary);
        if (!out.good()) {
            return ErrorReason{ ErrorCategory::LocalUpdate,
                std::string("Failed to open extraction target: ") + ToStdUTF8(outPath) };
        }

        auto stream = entry->GetDecompressionStream();
        if (!stream) {
            return ErrorReason{ ErrorCategory::LocalUpdate,
                std::string("Failed to decompress: ") + entry->GetFullName() };
        }

        auto remaining = entry->GetSize();
        char buf[4096];
        while (remaining) {
            auto chunk = std::min(remaining, std::size(buf));
            stream->read(buf, chunk);
            out.write(buf, chunk);
            remaining -= chunk;
        }

        entry->CloseDecompressionStream();
        out.close();
    }

    return OperationSuccessful{};
}


OperationResult GameModUpdater::AtomicSwap(std::wstring const& installPath,
                                           std::wstring const& stagingPath,
                                           std::wstring const& backupPath)
{
    // Phase 1: rename existing install to backup (if present).
    // If no install exists yet (first-ever install via this flow on
    // a manually-placed mod folder is unusual but possible), we just
    // skip this step.
    bool hadExisting = PathFileExistsW(installPath.c_str()) != FALSE;
    if (hadExisting) {
        if (!MoveFileW(installPath.c_str(), backupPath.c_str())) {
            return ErrorReason{ ErrorCategory::LocalUpdate,
                std::string("Failed to back up existing install (file locked?): ")
                + ToStdUTF8(installPath) };
        }
    }

    // Phase 2: move staging into the install location.
    if (!MoveFileW(stagingPath.c_str(), installPath.c_str())) {
        // Restore the backup so we don't leave the install missing.
        if (hadExisting) {
            MoveFileW(backupPath.c_str(), installPath.c_str());
        }
        return ErrorReason{ ErrorCategory::LocalUpdate,
            std::string("Failed to swap staging into install: ")
            + ToStdUTF8(stagingPath) };
    }

    // Phase 3: delete the backup.  If this fails it's not fatal --
    // the new install is live and the sweep on next launch will clean
    // it up.  Just log and continue.
    if (hadExisting) {
        if (!DeleteFolderRecursive(backupPath)) {
            DEBUG("Failed to delete backup folder (will retry on next launch): %s",
                  ToStdUTF8(backupPath).c_str());
        }
    }

    return OperationSuccessful{};
}


bool GameModUpdater::WriteVersionFile(std::wstring const& installPath,
                                      VersionNumber const& version)
{
    auto path = installPath + L"\\" + VERSION_MARKER_FILE;
    return SaveFile(path, version.ToString());
}


bool GameModUpdater::WriteNoticeFile(std::string const& resourceName,
                                     VersionNumber const& from,
                                     VersionNumber const& to,
                                     std::string const& notice)
{
    // Hand-build minimal JSON.  Pulling in rapidjson here would work
    // too, but for a three-field object this is simpler.  Notice text
    // needs string escaping for backslashes and quotes.
    auto escape = [](std::string const& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '\\' || c == '"') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else {
                out += c;
            }
        }
        return out;
    };

    std::ostringstream json;
    json << "{\n";
    json << "  \"from\": \""   << from.ToString() << "\",\n";
    json << "  \"to\": \""     << to.ToString()   << "\",\n";
    json << "  \"notice\": \"" << escape(notice)  << "\"\n";
    json << "}\n";

    auto userProfileDir = GetBG3UserProfileScriptExtenderDir();
    if (userProfileDir.empty()) return false;

    // Per-mod subfolder so multiple mods using this update flow
    // don't collide on update_notice.json.  Resource name is what
    // the loader uses to disambiguate; mirror it here.
    auto resourceDir = userProfileDir + FromStdUTF8(resourceName) + L"\\";
    SHCreateDirectoryExW(NULL, resourceDir.c_str(), NULL);

    auto path = resourceDir + UPDATE_NOTICE_FILENAME;
    return SaveFile(path, json.str());
}


OperationResult GameModUpdater::Update(Manifest::Resource const& resource,
                                       Manifest::ResourceVersion const& version)
{
    if (version.Revoked) {
        return ErrorReason{ ErrorCategory::UpdateDownload,
            std::string("Attempted to download revoked mod resource: ") + version.Digest };
    }

    auto bg3Root = GetBG3InstallRoot();
    if (bg3Root.empty()) {
        return ErrorReason{ ErrorCategory::LocalLoad,
            "Could not determine BG3 install root from process image path" };
    }

    auto installPath = bg3Root + L"\\" + FromStdUTF8(resource.InstallPath);
    std::replace(installPath.begin(), installPath.end(), L'/', L'\\');

    // Dev-workspace safety check.  If the install path contains a
    // .git/ folder, this is almost certainly someone's working
    // directory rather than a plain install.  The atomic swap deletes
    // the .backup copy after the swap, which would wipe their .git
    // history along with any uncommitted files not in the package
    // (DevConfig.lua, ad-hoc experiments, etc.).  That's destructive
    // in a way auto-update should never be on a dev box.
    //
    // We bail with an informational log instead -- the developer is
    // working on the mod and doesn't need auto-update anyway; they
    // deploy their changes manually.  No user-facing error, just a
    // silent skip with a trace in OsiExtenderUpdater.log.
    auto gitDirPath = installPath + L"\\.git";
    if (PathFileExistsW(gitDirPath.c_str())) {
        DEBUG("Mod '%s' install path contains .git/ -- treating as a"
              " dev workspace and skipping auto-update.  Delete .git"
              " or move it elsewhere to enable auto-update.",
              resource.Name.c_str());
        return OperationSuccessful{};
    }

    // Comparison: only proceed if the manifest version is strictly
    // newer than what's installed.  Equal version => already up to
    // date.  Older version in manifest => user might be on a beta;
    // don't downgrade.
    auto installedVersion = ReadInstalledVersion(installPath);
    if (installedVersion && !(*installedVersion < version.Version)) {
        DEBUG("Mod '%s' already at version %s, no update needed",
              resource.Name.c_str(), installedVersion->ToString().c_str());
        return OperationSuccessful{};
    }

    DEBUG("Mod '%s' updating from %s to %s", resource.Name.c_str(),
          installedVersion ? installedVersion->ToString().c_str() : "(none)",
          version.Version.ToString().c_str());

    // 1. Download to Updates temp dir.
    auto updatesDir = GetUpdatesTempDir();
    if (updatesDir.empty()) {
        return ErrorReason{ ErrorCategory::LocalUpdate,
            "Could not determine LOCALAPPDATA Updates folder" };
    }
    auto packagePath = updatesDir + FromStdUTF8(resource.Name) + L"-"
                     + FromStdUTF8(version.Version.ToString()) + L".package";

    auto downloadResult = DownloadPackage(version.URL, packagePath);
    if (!downloadResult) {
        DeleteFileW(packagePath.c_str()); // partial download cleanup
        return downloadResult;
    }

    // 2. Verify signature against embedded public key.
    std::string verifyReason;
    if (!CryptoUtils::VerifySignedFile(packagePath, verifyReason)) {
        DeleteFileW(packagePath.c_str());
        return ErrorReason{ ErrorCategory::LocalUpdate,
            std::string("Mod package signature verification failed: ") + verifyReason };
    }

    // 3. Extract to staging next to the install path.
    auto stagingPath = installPath + STAGING_SUFFIX;
    auto backupPath  = installPath + BACKUP_SUFFIX;

    // Defensive: clean any pre-existing staging from a prior failure.
    if (PathFileExistsW(stagingPath.c_str())) {
        DeleteFolderRecursive(stagingPath);
    }

    auto extractResult = ExtractPackage(packagePath, stagingPath);
    if (!extractResult) {
        DeleteFolderRecursive(stagingPath);
        DeleteFileW(packagePath.c_str());
        return extractResult;
    }

    // 4. Sanity-check the extraction.
    if (!ValidateStaging(stagingPath)) {
        DeleteFolderRecursive(stagingPath);
        DeleteFileW(packagePath.c_str());
        return ErrorReason{ ErrorCategory::LocalUpdate,
            "Extracted mod package is missing meta.lsx -- archive content invalid" };
    }

    // 5. Atomic swap.
    auto swapResult = AtomicSwap(installPath, stagingPath, backupPath);
    if (!swapResult) {
        DeleteFolderRecursive(stagingPath);
        DeleteFileW(packagePath.c_str());
        return swapResult;
    }

    // 6. Write markers AFTER swap (so they end up inside the new
    //    install, not the staging dir which by now has been moved).
    if (!WriteVersionFile(installPath, version.Version)) {
        DEBUG("Warning: failed to write version marker -- next launch will think no install");
    }

    auto fromVersion = installedVersion.value_or(VersionNumber(0, 0, 0, 0));
    if (!WriteNoticeFile(resource.Name, fromVersion, version.Version, version.Notice)) {
        DEBUG("Warning: failed to write update notice -- Lua won't announce the update");
    }

    // 7. Clean up the downloaded package.
    DeleteFileW(packagePath.c_str());

    DEBUG("Mod '%s' updated to %s successfully", resource.Name.c_str(),
          version.Version.ToString().c_str());
    return OperationSuccessful{};
}

END_SE()
