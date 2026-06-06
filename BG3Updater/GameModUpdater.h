#pragma once

#include <string>
#include <optional>
#include "Manifest.h"
#include "HttpFetcher.h"
#include "Config.h"
#include "Result.h"
#include "GameVersion.h"

BEGIN_SE()

// Updater for resources of Type=GameMod.  Downloads a signed .package
// file from the manifest URL, verifies its signature against the
// embedded public key, then extracts the contents into a folder
// relative to the BG3 install root.
//
// Lifecycle is one-shot: construct, call Update() per GameMod entry,
// destruct.  No state beyond references to the shared HttpFetcher and
// UpdaterConfig.
//
// Atomicity: extraction goes into <installPath>.staging first.  On
// success the existing install (if any) is renamed to .backup, then
// staging is moved into the install location, then .backup is removed.
// Any interrupted run leaves cruft (a .staging or .backup folder)
// that gets cleaned by SweepLeftoverState() on the next launch.
//
// All file I/O uses Win32 APIs.  Paths are wide strings.  Errors are
// reported through OperationResult; the caller decides whether to
// surface them to the user.
class GameModUpdater
{
public:
    GameModUpdater(HttpFetcher& fetcher, UpdaterConfig const& config);

    // Apply an update for a single GameMod resource.  Reads the
    // installed-version marker, compares to the latest manifest
    // version, downloads + verifies + installs if newer.  Returns
    // OperationSuccessful if no update was needed OR the update
    // completed.  Returns an error reason on download/verify/extract
    // failure -- caller should surface this to the user.
    OperationResult Update(Manifest::Resource const& resource,
                           Manifest::ResourceVersion const& version);

    // BG3 install root: parent of the bin/ folder that contains
    // bg3.exe.  Cached after first lookup.  Empty wstring on failure.
    static std::wstring GetBG3InstallRoot();

    // %LOCALAPPDATA%\BG3ScriptExtender\Updates\.  Created on demand.
    // Holds in-flight .package downloads.  Wiped by sweep on next
    // launch if anything is left over.
    static std::wstring GetUpdatesTempDir();

    // Reads .bg3access_version.txt from the install path.  Returns
    // empty optional when the file is missing (fresh install or
    // never updated by this flow -- caller treats as 0.0.0.0).
    static std::optional<VersionNumber> ReadInstalledVersion(
        std::wstring const& installPath);

    // Wipes leftover state from interrupted updates: the Updates
    // temp folder contents AND any .staging or .backup folders
    // adjacent to the install paths of GameMod resources in the
    // manifest.  Call once per launch before invoking Update().
    static void SweepLeftoverState(Manifest const& manifest);

private:
    HttpFetcher& fetcher_;
    UpdaterConfig const& config_;

    OperationResult DownloadPackage(std::string const& url,
                                    std::wstring const& destPath);
    OperationResult ExtractPackage(std::wstring const& packagePath,
                                   std::wstring const& destFolder);
    OperationResult AtomicSwap(std::wstring const& installPath,
                               std::wstring const& stagingPath,
                               std::wstring const& backupPath);

    bool WriteVersionFile(std::wstring const& installPath,
                          VersionNumber const& version);
    // Writes the update notice JSON into the BG3 user profile path
    // that Ext.IO.LoadFile (default "user" context) reads from.  Lua
    // can't read files inside Data/Mods directly so the notice can't
    // live there.  Path: <LOCALAPPDATA>\Larian Studios\Baldur's Gate 3
    // \Script Extender\<resourceName>\update_notice.json
    bool WriteNoticeFile(std::string const& resourceName,
                         VersionNumber const& from,
                         VersionNumber const& to,
                         std::string const& notice);

    // BG3's user profile root: <LOCALAPPDATA>\Larian Studios\Baldur's
    // Gate 3\Script Extender\.  Different from the extender's own
    // cache at <LOCALAPPDATA>\BG3ScriptExtender\ -- this one is where
    // game-side script extender storage lives, which Ext.IO uses.
    static std::wstring GetBG3UserProfileScriptExtenderDir();

    // Verifies the freshly-extracted staging dir contains the
    // expected mod files before we commit to swapping it in.
    // Currently checks: meta.lsx must exist somewhere in the tree.
    // Mods without meta.lsx would never load in BG3, so missing
    // meta.lsx is a strong "extraction is broken" signal.
    static bool ValidateStaging(std::wstring const& stagingPath);

    static bool DeleteFolderRecursive(std::wstring const& path);
};

END_SE()
