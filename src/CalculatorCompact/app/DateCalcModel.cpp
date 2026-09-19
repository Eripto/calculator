// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "DateCalcModel.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <ctime>

#include "Header Files/NumericString.h"

namespace CalcCompact
{
    namespace
    {
        constexpr const wchar_t* kMonthNames[] = { L"January", L"February", L"March",     L"April",   L"May",      L"June",
                                                   L"July",    L"August",   L"September", L"October", L"November", L"December" };

        constexpr const wchar_t* kWeekdayNames[] = { L"Thursday", L"Friday", L"Saturday", L"Sunday", L"Monday", L"Tuesday", L"Wednesday" };

        void AppendCount(std::wstring& text, long long value, const wchar_t* singular, const wchar_t* plural)
        {
            if (value == 0)
            {
                return;
            }
            if (!text.empty())
            {
                text += L", ";
            }
            text += CalcEngine::NumericString::FromInteger(value);
            text += L' ';
            text += (value == 1) ? singular : plural;
        }
    }

    namespace DateMath
    {
        bool IsLeapYear(int year)
        {
            return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        }

        int DaysInMonth(int year, int month)
        {
            static constexpr int lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            if (month < 1 || month > 12)
            {
                return 30;
            }
            if (month == 2 && IsLeapYear(year))
            {
                return 29;
            }
            return lengths[month - 1];
        }

        // days_from_civil / civil_from_days, the standard branch-free conversion
        // between a proleptic Gregorian date and a day number.
        long long ToDayNumber(const CivilDate& date)
        {
            long long y = date.year;
            const long long m = date.month;
            const long long d = date.day;
            y -= m <= 2;
            const long long era = (y >= 0 ? y : y - 399) / 400;
            const long long yoe = y - era * 400;                                     // [0, 399]
            const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;    // [0, 365]
            const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
            return era * 146097 + doe - 719468;
        }

        CivilDate FromDayNumber(long long dayNumber)
        {
            long long z = dayNumber + 719468;
            const long long era = (z >= 0 ? z : z - 146096) / 146097;
            const long long doe = z - era * 146097;                                          // [0, 146096]
            const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;     // [0, 399]
            const long long y = yoe + era * 400;
            const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                   // [0, 365]
            const long long mp = (5 * doy + 2) / 153;                                        // [0, 11]
            const long long d = doy - (153 * mp + 2) / 5 + 1;                                // [1, 31]
            const long long m = mp + (mp < 10 ? 3 : -9);                                     // [1, 12]

            CivilDate date;
            date.year = static_cast<int>(y + (m <= 2));
            date.month = static_cast<int>(m);
            date.day = static_cast<int>(d);
            return date;
        }

        CivilDate Today()
        {
#if defined(_WIN32)
            // GetLocalTime rather than time() plus localtime_s: the CRT's
            // 64-bit localtime and the secure-parameter handler behind it are
            // ~9KB of the binary, and this is the only call site.
            SYSTEMTIME now{};
            GetLocalTime(&now);
            CivilDate date;
            date.year = now.wYear;
            date.month = now.wMonth;
            date.day = now.wDay;
            return date;
#else
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_r(&now, &local);
            CivilDate date;
            date.year = local.tm_year + 1900;
            date.month = local.tm_mon + 1;
            date.day = local.tm_mday;
            return date;
#endif
        }

        CivilDate AddMonths(const CivilDate& date, long long months)
        {
            long long total = static_cast<long long>(date.year) * 12 + (date.month - 1) + months;
            CivilDate result;
            result.year = static_cast<int>(total >= 0 ? total / 12 : (total - 11) / 12);
            result.month = static_cast<int>(total - static_cast<long long>(result.year) * 12) + 1;
            // The calendar clamps to the end of a shorter month: 31 Jan + 1 month
            // is 28 (or 29) Feb, not 3 March.
            const int limit = DaysInMonth(result.year, result.month);
            result.day = date.day < limit ? date.day : limit;
            return result;
        }

        CivilDate AddDays(const CivilDate& date, long long days)
        {
            return FromDayNumber(ToDayNumber(date) + days);
        }

        DateDifference Difference(const CivilDate& a, const CivilDate& b)
        {
            const CivilDate& start = (a < b || a == b) ? a : b;
            const CivilDate& end = (a < b || a == b) ? b : a;

            DateDifference result;
            result.totalDays = ToDayNumber(end) - ToDayNumber(start);

            // Greedy: whole years, then whole months, then weeks, then days --
            // the same decomposition order the shipping engine uses.
            int years = end.year - start.year;
            if (years > 0 && ToDayNumber(AddMonths(start, static_cast<long long>(years) * 12)) > ToDayNumber(end))
            {
                --years;
            }
            CivilDate pivot = AddMonths(start, static_cast<long long>(years) * 12);

            int months = 0;
            while (months < 12)
            {
                const CivilDate next = AddMonths(pivot, months + 1);
                if (ToDayNumber(next) > ToDayNumber(end))
                {
                    break;
                }
                ++months;
            }
            pivot = AddMonths(pivot, months);

            const long long remaining = ToDayNumber(end) - ToDayNumber(pivot);
            result.years = years;
            result.months = months;
            result.weeks = static_cast<int>(remaining / 7);
            result.days = static_cast<int>(remaining % 7);
            return result;
        }

        const wchar_t* MonthName(int month)
        {
            return (month >= 1 && month <= 12) ? kMonthNames[month - 1] : L"";
        }

        const wchar_t* WeekdayName(const CivilDate& date)
        {
            // 1970-01-01 was a Thursday, which is index 0 in kWeekdayNames.
            long long index = ToDayNumber(date) % 7;
            if (index < 0)
            {
                index += 7;
            }
            return kWeekdayNames[index];
        }

        std::wstring FormatLong(const CivilDate& date)
        {
            std::wstring text = WeekdayName(date);
            text += L", ";
            text += MonthName(date.month);
            text += L' ';
            text += CalcEngine::NumericString::FromInteger(date.day);
            text += L", ";
            text += CalcEngine::NumericString::FromInteger(date.year);
            return text;
        }

        std::wstring FormatShort(const CivilDate& date)
        {
            std::wstring text = CalcEngine::NumericString::FromInteger(date.month);
            text += L'/';
            text += CalcEngine::NumericString::FromInteger(date.day);
            text += L'/';
            text += CalcEngine::NumericString::FromInteger(date.year);
            return text;
        }

        std::wstring DescribeDifference(const DateDifference& difference)
        {
            std::wstring text;
            AppendCount(text, difference.years, L"year", L"years");
            AppendCount(text, difference.months, L"month", L"months");
            AppendCount(text, difference.weeks, L"week", L"weeks");
            AppendCount(text, difference.days, L"day", L"days");
            return text.empty() ? std::wstring(L"Same dates") : text;
        }
    }

    DateCalcModel::DateCalcModel()
    {
        m_from = DateMath::Today();
        m_to = DateMath::Today();
        m_start = DateMath::Today();
    }

    DateDifference DateCalcModel::Difference() const
    {
        return DateMath::Difference(m_from, m_to);
    }

    CivilDate DateCalcModel::Result() const
    {
        const long long sign = m_adding ? 1 : -1;
        CivilDate date = DateMath::AddMonths(m_start, sign * (static_cast<long long>(m_offsetYears) * 12 + m_offsetMonths));
        date = DateMath::AddDays(date, sign * m_offsetDays);
        return date;
    }
}
