#pragma once

/**
 * @file diversification_filter.hpp
 * @brief Пост-ранжирующий фильтр диверсификации корзины торговых пар.
 *
 * После raw ranking применяет ограничения:
 * - Корреляционный cap: не более max_correlation между парами
 * - Концентрация по сектору: не более max_per_sector из одного сектора
 * - Минимальная глубина ликвидности
 *
 * Жадный алгоритм: берём лучшую пару по score, затем следующую,
 * которая совместима с уже выбранными, и т.д.
 */

#include "pair_scanner_types.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::pair_scanner {

/// Конфигурация фильтра диверсификации
struct DiversificationConfig {
    double max_correlation{0.85};        ///< Макс. корреляция между парами
    int max_per_sector{2};               ///< Макс. пар из одного сектора (по base_coin prefix)
    double min_liquidity_usdt{50'000.0}; ///< Мин. глубина ликвидности
    bool enabled{true};
};

/// Пост-ранжирующий фильтр для диверсификации корзины пар
class DiversificationFilter {
public:
    explicit DiversificationFilter(DiversificationConfig config)
        : config_(std::move(config))
    {}

    /**
     * @brief Применить фильтр диверсификации к отсортированному списку пар.
     *
     * @param ranked_pairs Пары, отсортированные по total_score (убывание)
     * @param candles_map  Свечи для каждой пары (symbol → candles)
     * @param top_n        Количество пар для выбора
     * @return Диверсифицированный набор символов
     */
    std::vector<std::string> apply(
        const std::vector<PairScore>& ranked_pairs,
        const std::unordered_map<std::string, std::vector<CandleData>>& candles_map,
        int top_n) const
    {
        if (!config_.enabled) {
            return select_top_n(ranked_pairs, top_n);
        }

        std::vector<std::string> selected;
        std::unordered_map<std::string, int> sector_count;

        for (const auto& pair : ranked_pairs) {
            if (static_cast<int>(selected.size()) >= top_n) break;
            if (pair.total_score <= 0.0) continue;

            // Проверка ликвидности
            if (pair.quote_volume_24h < config_.min_liquidity_usdt) continue;

            // Проверка концентрации по сектору
            std::string sector = extract_sector(pair.symbol);
            if (sector_count[sector] >= config_.max_per_sector) continue;

            // Проверка корреляции с уже выбранными
            if (!check_correlation(pair.symbol, selected, candles_map)) continue;

            selected.push_back(pair.symbol);
            sector_count[sector]++;
        }

        return selected;
    }

private:
    /// Выбрать top-N без фильтрации (fallback)
    static std::vector<std::string> select_top_n(
        const std::vector<PairScore>& ranked, int top_n)
    {
        std::vector<std::string> result;
        for (int i = 0; i < std::min(top_n, static_cast<int>(ranked.size())); ++i) {
            if (ranked[i].total_score > 0.0) {
                result.push_back(ranked[i].symbol);
            }
        }
        return result;
    }

    /// Извлечь «сектор» по первым символам base coin
    static std::string extract_sector(const std::string& symbol) {
        // Убираем USDT суффикс, берём base coin
        if (symbol.size() > 4 && symbol.substr(symbol.size() - 4) == "USDT") {
            return symbol.substr(0, symbol.size() - 4);
        }
        return symbol;
    }

    /// Проверить корреляцию кандидата со всеми уже выбранными парами
    bool check_correlation(
        const std::string& candidate,
        const std::vector<std::string>& selected,
        const std::unordered_map<std::string, std::vector<CandleData>>& candles_map) const
    {
        auto it_cand = candles_map.find(candidate);
        if (it_cand == candles_map.end()) return true;

        for (const auto& existing : selected) {
            auto it_exist = candles_map.find(existing);
            if (it_exist == candles_map.end()) continue;

            double corr = compute_pearson_correlation(
                it_cand->second, it_exist->second);
            if (std::abs(corr) > config_.max_correlation) {
                return false;
            }
        }
        return true;
    }

    /// Вычислить корреляцию Пирсона на close-ценах двух наборов свечей
    static double compute_pearson_correlation(
        const std::vector<CandleData>& a,
        const std::vector<CandleData>& b)
    {
        size_t n = std::min(a.size(), b.size());
        if (n < 10) return 0.0;

        // Используем returns, а не raw prices
        std::vector<double> ret_a, ret_b;
        ret_a.reserve(n - 1);
        ret_b.reserve(n - 1);

        size_t offset_a = a.size() - n;
        size_t offset_b = b.size() - n;

        for (size_t i = 1; i < n; ++i) {
            double pa_prev = a[offset_a + i - 1].close;
            double pa_curr = a[offset_a + i].close;
            double pb_prev = b[offset_b + i - 1].close;
            double pb_curr = b[offset_b + i].close;

            if (pa_prev <= 0.0 || pb_prev <= 0.0) continue;

            ret_a.push_back((pa_curr - pa_prev) / pa_prev);
            ret_b.push_back((pb_curr - pb_prev) / pb_prev);
        }

        if (ret_a.size() < 5) return 0.0;

        double mean_a = 0.0, mean_b = 0.0;
        for (size_t i = 0; i < ret_a.size(); ++i) {
            mean_a += ret_a[i];
            mean_b += ret_b[i];
        }
        mean_a /= ret_a.size();
        mean_b /= ret_b.size();

        double cov = 0.0, var_a = 0.0, var_b = 0.0;
        for (size_t i = 0; i < ret_a.size(); ++i) {
            double da = ret_a[i] - mean_a;
            double db = ret_b[i] - mean_b;
            cov += da * db;
            var_a += da * da;
            var_b += db * db;
        }

        if (var_a <= 0.0 || var_b <= 0.0) return 0.0;
        return cov / (std::sqrt(var_a) * std::sqrt(var_b));
    }

    DiversificationConfig config_;
};

} // namespace tb::pair_scanner
