/**
 * @file metric_tags.hpp
 * @brief Теги метрик для группировки и фильтрации
 * 
 * MetricTags используется для добавления меток к метрикам
 * (аналог labels в Prometheus).
 */
#pragma once

#include <unordered_map>
#include <string>

namespace tb::metrics {

/// Карта меток метрики (label_name -> label_value)
using MetricTags = std::unordered_map<std::string, std::string>;

} // namespace tb::metrics
