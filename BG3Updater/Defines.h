#pragma once

// BG3Access fork: serve manifests + signed packages from our own repo.
// Layout under the release-manifest branch:
//   Channels/Release/Manifest.json -- stable, public-friendly builds
//   Channels/Beta/Manifest.json    -- pre-release builds for testers
// Signed package zips live as GitHub release assets; the Manifest entry
// URL field points at the release-download URL of each version.
//
// Channel selection at runtime: default channel below ("Release") can be
// overridden by shipping a ScriptExtenderUpdaterConfig.json alongside
// the loader with "UpdateChannel": "Beta" -- tester builds use that
// override; eventual public releases use this default.
#define UPDATER_MANIFEST_URL "https://raw.githubusercontent.com/IndomitableHeart/bg3se-SR/release-manifest/Channels/"
#define UPDATER_MANIFEST_NAME "Manifest.json"
#define UPDATER_CHANNEL "Release"
#define UPDATER_CHANNEL_GAME ""
#define UPDATER_CHANNEL_EDITOR "Editor"
#define UPDATER_RESOURCE_NAME "ScriptExtender"
#define GAME_DLL L"BG3ScriptExtender.dll"
#define EDITOR_DLL L"BG3EditorScriptExtender.dll"
#define UPDATER_CONFIG_FILE L"ScriptExtenderUpdaterConfig.json"
// Manifest download timeout (ms)
#define MANIFEST_FETCH_TIMEOUT 5000
// Content package download timeout (ms)
#define CONTENT_FETCH_TIMEOUT 120000
