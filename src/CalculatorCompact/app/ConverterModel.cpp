// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "ConverterModel.h"

#include <algorithm>

#include "Command.h"
#include "ConverterData.generated.h"

using namespace UnitConversionManager;

namespace CalcCompact
{
    namespace
    {
        const ConverterCategory* FindCategory(int id)
        {
            for (const auto& category : kConverterCategories)
            {
                if (category.id == id)
                {
                    return &category;
                }
            }
            return nullptr;
        }

        // Feeds the engine the generated tables. The shipping app's loader does
        // the same job from its resource files; the ratios are derived exactly as
        // it derives them, so conversions agree unit for unit.
        class GeneratedDataLoader final : public IConverterDataLoader
        {
        public:
            void LoadData() override
            {
            }

            std::vector<Category> GetOrderedCategories() override
            {
                std::vector<Category> categories;
                categories.reserve(kConverterCategories.size());
                for (const auto& entry : kConverterCategories)
                {
                    categories.emplace_back(entry.id, std::wstring(entry.name), entry.supportsNegative);
                }
                return categories;
            }

            std::vector<Unit> GetOrderedUnits(const Category& category) override
            {
                std::vector<Unit> units;
                const ConverterCategory* entry = FindCategory(category.id);
                if (entry == nullptr)
                {
                    return units;
                }
                units.reserve(entry->unitCount);
                for (size_t i = 0; i < entry->unitCount; ++i)
                {
                    const ConverterUnit& unit = entry->units[i];
                    units.emplace_back(
                        unit.id,
                        unit.name,
                        std::wstring(unit.abbreviation),
                        unit.isConversionSource,
                        unit.isConversionTarget,
                        unit.isWhimsical);
                }
                return units;
            }

            std::unordered_map<Unit, ConversionData, UnitHash> LoadOrderedRatios(const Unit& unit) override
            {
                std::unordered_map<Unit, ConversionData, UnitHash> ratios;
                const ConverterCategory* entry = CategoryForUnit(unit.id);
                if (entry == nullptr)
                {
                    return ratios;
                }

                const ConverterUnit* source = UnitById(*entry, unit.id);
                for (size_t i = 0; i < entry->unitCount; ++i)
                {
                    const ConverterUnit& target = entry->units[i];
                    const Unit key(
                        target.id,
                        target.name,
                        std::wstring(target.abbreviation),
                        target.isConversionSource,
                        target.isConversionTarget,
                        target.isWhimsical);

                    if (entry->explicitCount != 0)
                    {
                        // Temperature: ratio and offset are tabulated per pair.
                        const ConverterExplicitConversion* match = nullptr;
                        for (size_t k = 0; k < entry->explicitCount; ++k)
                        {
                            const ConverterExplicitConversion& row = entry->explicitConversions[k];
                            if (row.fromId == unit.id && row.toId == target.id)
                            {
                                match = &row;
                                break;
                            }
                        }
                        if (match != nullptr)
                        {
                            ratios.emplace(key, ConversionData(match->ratio, match->offset, match->offsetFirst));
                        }
                        continue;
                    }

                    if (source != nullptr && target.factor != 0.0)
                    {
                        ratios.emplace(key, ConversionData(source->factor / target.factor, 0.0, false));
                    }
                }
                return ratios;
            }

            bool SupportsCategory(const Category& target) override
            {
                return FindCategory(target.id) != nullptr;
            }

        private:
            static const ConverterCategory* CategoryForUnit(int unitId)
            {
                for (const auto& category : kConverterCategories)
                {
                    if (UnitById(category, unitId) != nullptr)
                    {
                        return &category;
                    }
                }
                return nullptr;
            }

            static const ConverterUnit* UnitById(const ConverterCategory& category, int unitId)
            {
                for (size_t i = 0; i < category.unitCount; ++i)
                {
                    if (category.units[i].id == unitId)
                    {
                        return &category.units[i];
                    }
                }
                return nullptr;
            }
        };
    }

    ConverterModel::ConverterModel()
    {
        m_converter = std::make_shared<UnitConverter>(std::make_shared<GeneratedDataLoader>());
        m_converter->Initialize();
        m_converter->SetViewModelCallback(std::shared_ptr<IUnitConverterVMCallback>(this, [](IUnitConverterVMCallback*) {
            // Non-owning: the model outlives the converter it owns.
        }));
        m_categories = m_converter->GetCategories();
        if (!m_categories.empty())
        {
            SetCategory(m_categories.front().id);
        }
    }

    void ConverterModel::DisplayCallback(const std::wstring& from, const std::wstring& to)
    {
        // UnitConverter::SwitchActive swaps its own from/to, so while the second
        // readout is being edited the engine reports the pair the other way
        // round. Undo that here so the model always speaks in the UI's terms.
        if (m_secondActive)
        {
            m_toValue = from;
            m_fromValue = to;
        }
        else
        {
            m_fromValue = from;
            m_toValue = to;
        }
    }

    void ConverterModel::SuggestedValueCallback(const std::vector<std::tuple<std::wstring, Unit>>& suggestedValues)
    {
        m_suggestions.clear();
        m_suggestions.reserve(suggestedValues.size());
        for (const auto& entry : suggestedValues)
        {
            m_suggestions.push_back({ std::get<0>(entry), std::get<1>(entry).abbreviation, std::get<1>(entry).name });
        }
    }

    void ConverterModel::MaxDigitsReached()
    {
        m_maxDigitsReached = true;
    }

    void ConverterModel::SetCategory(int categoryId)
    {
        const auto match = std::find_if(m_categories.begin(), m_categories.end(), [categoryId](const Category& c) { return c.id == categoryId; });
        if (match == m_categories.end())
        {
            return;
        }

        m_currentCategory = *match;
        auto selection = m_converter->SetCurrentCategory(m_currentCategory);
        m_units = std::get<0>(selection);
        m_fromUnit = std::get<1>(selection);
        m_toUnit = std::get<2>(selection);
        m_secondActive = false;
        m_converter->Calculate();
    }

    void ConverterModel::SetUnits(int fromId, int toId)
    {
        const auto find = [this](int id, const Unit& fallback) {
            const auto match = std::find_if(m_units.begin(), m_units.end(), [id](const Unit& u) { return u.id == id; });
            return match != m_units.end() ? *match : fallback;
        };
        m_fromUnit = find(fromId, m_fromUnit);
        m_toUnit = find(toId, m_toUnit);
        if (m_secondActive)
        {
            m_converter->SetCurrentUnitTypes(m_toUnit, m_fromUnit);
        }
        else
        {
            m_converter->SetCurrentUnitTypes(m_fromUnit, m_toUnit);
        }
    }

    void ConverterModel::Send(Command command)
    {
        m_converter->SendCommand(command);
    }

    void ConverterModel::Clear()
    {
        m_converter->SendCommand(Command::Clear);
    }

    void ConverterModel::SetActiveField(bool second)
    {
        if (m_secondActive == second)
        {
            return;
        }
        const std::wstring& valueBecomingActive = second ? m_toValue : m_fromValue;
        m_secondActive = second;
        // SwitchActive swaps the engine's unit pair and re-seeds it with the
        // value now being edited.
        m_converter->SwitchActive(valueBecomingActive);
    }

    void ConverterModel::SwapUnits()
    {
        const int from = m_fromUnit.id;
        SetUnits(m_toUnit.id, from);
    }

    Command ConverterCommandForChar(wchar_t character, wchar_t decimalSeparator)
    {
        if (character >= L'0' && character <= L'9')
        {
            return static_cast<Command>(static_cast<int>(Command::Zero) + (character - L'0'));
        }
        if (character == decimalSeparator || character == L'.')
        {
            return Command::Decimal;
        }
        if (character == L'-')
        {
            return Command::Negate;
        }
        if (character == L'\b')
        {
            return Command::Backspace;
        }
        if (character == 27)
        {
            return Command::Clear;
        }
        return Command::None;
    }
}
