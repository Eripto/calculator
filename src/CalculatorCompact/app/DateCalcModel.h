// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <string>

namespace CalcCompact
{
    struct CivilDate
    {
        int year = 1970;
        int month = 1; // 1-12
        int day = 1;   // 1-31

        bool operator==(const CivilDate& other) const
        {
            return year == other.year && month == other.month && day == other.day;
        }
        bool operator<(const CivilDate& other) const
        {
            if (year != other.year) return year < other.year;
            if (month != other.month) return month < other.month;
            return day < other.day;
        }
    };

    struct DateDifference
    {
        int years = 0;
        int months = 0;
        int weeks = 0;
        int days = 0;      // remainder after weeks
        long long totalDays = 0;
    };

    // Date arithmetic for the Date Calculation mode.
    //
    // The shipping app defers to Windows.Globalization.Calendar so it can honour
    // non-Gregorian calendar systems; this works in the proleptic Gregorian
    // calendar, which is what that resolves to for the overwhelming majority of
    // installs. The greedy year -> month -> week -> day decomposition and the
    // end-of-month clamping both follow the shipping implementation.
    namespace DateMath
    {
        bool IsLeapYear(int year);
        int DaysInMonth(int year, int month);

        // Days since 1970-01-01, valid for any year in range.
        long long ToDayNumber(const CivilDate& date);
        CivilDate FromDayNumber(long long dayNumber);

        CivilDate Today();

        // Clamps the day to the target month's length, as the calendar does.
        CivilDate AddMonths(const CivilDate& date, long long months);
        CivilDate AddDays(const CivilDate& date, long long days);

        DateDifference Difference(const CivilDate& a, const CivilDate& b);

        // "Tuesday, 3 June 2025" style, matching the app's long date format.
        std::wstring FormatLong(const CivilDate& date);
        std::wstring FormatShort(const CivilDate& date);
        const wchar_t* MonthName(int month);
        const wchar_t* WeekdayName(const CivilDate& date);

        // Renders a difference the way the app's result line reads.
        std::wstring DescribeDifference(const DateDifference& difference);
    }

    // State behind the Date Calculation mode.
    class DateCalcModel
    {
    public:
        enum class Mode
        {
            Difference,
            AddSubtract
        };

        DateCalcModel();

        Mode CurrentMode() const { return m_mode; }
        void SetMode(Mode mode) { m_mode = mode; }

        CivilDate& From() { return m_from; }
        CivilDate& To() { return m_to; }
        CivilDate& Start() { return m_start; }
        const CivilDate& From() const { return m_from; }
        const CivilDate& To() const { return m_to; }
        const CivilDate& Start() const { return m_start; }

        bool IsAdding() const { return m_adding; }
        void SetAdding(bool adding) { m_adding = adding; }

        int& OffsetYears() { return m_offsetYears; }
        int& OffsetMonths() { return m_offsetMonths; }
        int& OffsetDays() { return m_offsetDays; }
        int OffsetYears() const { return m_offsetYears; }
        int OffsetMonths() const { return m_offsetMonths; }
        int OffsetDays() const { return m_offsetDays; }

        DateDifference Difference() const;
        CivilDate Result() const;

    private:
        Mode m_mode = Mode::Difference;
        CivilDate m_from;
        CivilDate m_to;
        CivilDate m_start;
        bool m_adding = true;
        int m_offsetYears = 0;
        int m_offsetMonths = 0;
        int m_offsetDays = 0;
    };
}
