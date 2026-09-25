// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// What Setup changes on the machine, kept apart from the wizard that asks.
//
// Every change records what was there before it, and every undo only touches
// values that still point at this install -- so if something else has taken a
// setting over in the meantime, removing Calculator leaves it alone.

#pragma once

#include <windows.h>

#include "SetupUpdate.h"

#include <memory>
#include <string>
#include <vector>

namespace Setup
{
    enum Option : DWORD
    {
        OptionRedirectCalc = 1, // calc.exe and win32calc.exe open this build (administrator)
        OptionCalculatorKey = 2, // the keyboard's Calculator key
        OptionStartMenu = 4, // a Start menu entry, so search finds it
        OptionReplaceStore = 8, // unregister the Store app and take the calculator: protocol

        OptionDefaults = OptionRedirectCalc | OptionCalculatorKey | OptionStartMenu,
    };

    enum class Outcome
    {
        Done,
        Skipped,
        Failed,
    };

    struct StepResult
    {
        Outcome outcome = Outcome::Done;
        std::wstring note; // shown to the user when the step did not simply succeed
    };

    struct Plan;

    // A plain function pointer rather than std::function: every step works on
    // the plan alone, and the type erasure cost a few KB for nothing.
    struct Step
    {
        std::wstring title;
        StepResult (*run)(Plan& plan);
    };

    // A run of steps and the state they share.
    struct Plan
    {
        std::vector<Step> steps;
        HWND owner = nullptr; // parent for the administrator prompt
        std::wstring installDir;
        std::wstring targetExe;
        DWORD applied = 0;
        bool removing = false;
        bool complete = false; // an uninstall that left nothing behind
        bool removeSelfOnExit = false; // Setup.exe is running from the directory it removed

        // Set only for an update download.
        bool downloading = false;
        Release release;
        std::wstring downloadedPath;
    };

    struct Installed
    {
        bool present = false;
        bool byScript = false; // installed with Install-Calculator.ps1 rather than Setup
        DWORD options = 0;
        std::wstring dir;
        std::wstring version;
    };

    std::wstring DefaultInstallDir();
    Installed QueryInstalled();
    unsigned long long PayloadBytes();

    std::unique_ptr<Plan> PlanInstall(DWORD chosen, HWND owner);
    std::unique_ptr<Plan> PlanUninstall(HWND owner);
    std::unique_ptr<Plan> PlanDownload(const Release& release, HWND owner);

    // After the wizard closes: deletes a Setup.exe that could not delete itself.
    void ScheduleRemoval(const Plan& plan);
    void ScheduleDeletion(const std::wstring& file, const std::wstring& directory);
    bool Launch(const std::wstring& exe, const wchar_t* parameters = nullptr);
    std::wstring ThisExe();

    // The only part that runs elevated: Setup.exe /ifeo apply|restore "<exe>".
    int RunElevatedIfeo(const std::wstring& verb, const std::wstring& target);
}
