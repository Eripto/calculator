// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// Updates from the repository's GitHub releases.
//
// The installed calculator starts "Setup.exe /checkupdate" at most once a day.
// That asks GitHub for the latest release and, if it is newer than this Setup
// and carries a CalculatorSetup.exe, offers it; otherwise it exits without
// showing anything. Accepting downloads that Setup, checks it, and runs it
// with /update, which reinstalls over the top with the options already chosen.
//
// A repository with no releases answers 404, which reads as up to date.

#pragma once

#include <windows.h>

#include <string>

namespace Setup
{
    // Set by build.sh from the VERSION file.
    const wchar_t* Version();

    struct Release
    {
        std::wstring version; // the tag without its leading "v"
        std::wstring downloadUrl; // the release's CalculatorSetup.exe
        unsigned long long size = 0;
        std::wstring sha256; // lower-case hex; empty if GitHub listed no digest
    };

    enum class CheckResult
    {
        UpToDate,
        UpdateAvailable,
        Failed, // offline, rate limited, or an answer that made no sense
    };

    CheckResult CheckForUpdate(Release& release);

    // Downloads and verifies the release's installer into the temp directory.
    // On failure `error` says why, in words fit to show.
    bool DownloadUpdate(const Release& release, std::wstring& path, std::wstring& error);

    // Numeric, part by part: 1.10 is newer than 1.9. Negative, zero or
    // positive, as strcmp.
    int CompareVersions(const std::wstring& a, const std::wstring& b);

    bool UpdateChecksEnabled();
    void SetUpdateChecksEnabled(bool enabled);
    bool IsVersionSkipped(const std::wstring& version);
    void SkipVersion(const std::wstring& version);
}
