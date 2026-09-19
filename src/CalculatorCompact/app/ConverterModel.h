// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "UnitConverter.h"

namespace CalcCompact
{
    // Drives CalcManager's UnitConverter with the unit tables generated from the
    // shipping app's data loader. This is the converter's whole behaviour with
    // no UI attached, so it can be exercised by the engine tests directly.
    class ConverterModel final : public UnitConversionManager::IUnitConverterVMCallback
    {
    public:
        struct Suggestion
        {
            std::wstring value;
            std::wstring abbreviation;
            std::wstring name;
        };

        ConverterModel();
        ~ConverterModel() override = default;

        // IUnitConverterVMCallback
        void DisplayCallback(const std::wstring& from, const std::wstring& to) override;
        void SuggestedValueCallback(const std::vector<std::tuple<std::wstring, UnitConversionManager::Unit>>& suggestedValues) override;
        void MaxDigitsReached() override;

        const std::vector<UnitConversionManager::Category>& Categories() const
        {
            return m_categories;
        }

        // Category ids match NavCategory's serialization ids, so they are stable
        // across the nav menu, the converter and saved state.
        void SetCategory(int categoryId);
        int CategoryId() const
        {
            return m_currentCategory.id;
        }
        const std::wstring& CategoryName() const
        {
            return m_currentCategory.name;
        }

        const std::vector<UnitConversionManager::Unit>& Units() const
        {
            return m_units;
        }
        void SetUnits(int fromId, int toId);
        int FromUnitId() const
        {
            return m_fromUnit.id;
        }
        int ToUnitId() const
        {
            return m_toUnit.id;
        }
        const std::wstring& FromAbbreviation() const
        {
            return m_fromUnit.abbreviation;
        }
        const std::wstring& ToAbbreviation() const
        {
            return m_toUnit.abbreviation;
        }
        const std::wstring& FromUnitName() const
        {
            return m_fromUnit.name;
        }
        const std::wstring& ToUnitName() const
        {
            return m_toUnit.name;
        }

        void Send(UnitConversionManager::Command command);
        void Clear();

        // Which of the two readouts the keypad edits.
        void SetActiveField(bool second);
        bool IsSecondFieldActive() const
        {
            return m_secondActive;
        }
        void SwapUnits();

        const std::wstring& FromValue() const
        {
            return m_fromValue;
        }
        const std::wstring& ToValue() const
        {
            return m_toValue;
        }
        const std::vector<Suggestion>& Suggestions() const
        {
            return m_suggestions;
        }
        bool SupportsNegative() const
        {
            return m_currentCategory.supportsNegative;
        }
        bool MaxDigitsWasReached()
        {
            const bool reached = m_maxDigitsReached;
            m_maxDigitsReached = false;
            return reached;
        }

    private:
        std::shared_ptr<UnitConversionManager::UnitConverter> m_converter;
        std::vector<UnitConversionManager::Category> m_categories;
        std::vector<UnitConversionManager::Unit> m_units;
        UnitConversionManager::Category m_currentCategory;
        UnitConversionManager::Unit m_fromUnit;
        UnitConversionManager::Unit m_toUnit;
        std::wstring m_fromValue = L"0";
        std::wstring m_toValue = L"0";
        std::vector<Suggestion> m_suggestions;
        bool m_secondActive = false;
        bool m_maxDigitsReached = false;
    };

    // Maps a keyboard character to a converter command, or Command::None.
    UnitConversionManager::Command ConverterCommandForChar(wchar_t character, wchar_t decimalSeparator);
}
