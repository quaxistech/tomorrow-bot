/**
 * @file wal_writer.cpp
 * @brief Реализация Write-Ahead Logger
 */
#include "persistence/wal_writer.hpp"
#include <boost/json.hpp>
#include <format>
#include <unordered_set>

namespace tb::persistence {

[[nodiscard]] std::string to_string(WalEntryType type) {
    switch (type) {
        case WalEntryType::OrderIntent:       return "OrderIntent";
        case WalEntryType::OrderSent:         return "OrderSent";
        case WalEntryType::OrderCancelled:    return "OrderCancelled";
        case WalEntryType::PositionOpened:    return "PositionOpened";
        case WalEntryType::PositionClosed:    return "PositionClosed";
        case WalEntryType::BalanceSync:       return "BalanceSync";
        case WalEntryType::RecoveryCheckpoint: return "RecoveryCheckpoint";
    }
    return "Unknown";
}

/// Определить JournalEntryType по типу WAL записи
static JournalEntryType journal_type_for(WalEntryType wal_type) {
    switch (wal_type) {
        case WalEntryType::OrderIntent:
        case WalEntryType::OrderSent:
        case WalEntryType::OrderCancelled:
            return JournalEntryType::OrderEvent;
        case WalEntryType::PositionOpened:
        case WalEntryType::PositionClosed:
        case WalEntryType::BalanceSync:
            return JournalEntryType::PortfolioChange;
        case WalEntryType::RecoveryCheckpoint:
            return JournalEntryType::SystemEvent;
    }
    return JournalEntryType::SystemEvent;
}

/// Сформировать JSON-обёртку для WAL записи
static std::string wrap_wal_json(
    uint64_t seq,
    WalEntryType type,
    bool committed,
    const std::string& data)
{
    return std::format(
        R"({{"wal_seq":{},"wal_type":"{}","committed":{},"data":{}}})",
        seq, to_string(type), committed ? "true" : "false", data);
}

// ============================================================

WalWriter::WalWriter(
    std::shared_ptr<EventJournal> journal,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : journal_(std::move(journal))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{}

Result<uint64_t> WalWriter::write_intent(
    WalEntryType type,
    const std::string& payload_json,
    const OrderId& order_id,
    const Symbol& symbol,
    const StrategyId& strategy_id,
    const CorrelationId& correlation_id)
{
    const auto now = clock_->now();

    std::lock_guard lock(mutex_);

    const uint64_t seq = ++wal_sequence_;

    WalEntry entry{
        .wal_sequence = seq,
        .type = type,
        .payload_json = payload_json,
        .correlation_id = correlation_id,
        .order_id = order_id,
        .symbol = symbol,
        .strategy_id = strategy_id,
        .written_at = now,
        .committed = false
    };

    const auto wrapped = wrap_wal_json(seq, type, false, payload_json);

    // Запись в журнал под mutex, чтобы гарантировать порядок записей
    // совпадает с порядком WAL sequence.
    auto result = journal_->append(
        journal_type_for(type),
        wrapped,
        correlation_id,
        strategy_id);

    if (!result) {
        logger_->error("WAL", std::format(
            "Ошибка записи intent seq={} type={}", seq, to_string(type)));
        return std::unexpected(TbError::WalWriteFailed);
    }

    pending_entries_.emplace(seq, std::move(entry));

    metrics_->counter("wal_writes_total")->increment();

    logger_->debug("WAL", std::format(
        "Intent записан: seq={} type={} order={}",
        seq, to_string(type), order_id.get()));

    return seq;
}

VoidResult WalWriter::commit(uint64_t wal_sequence) {
    std::lock_guard lock(mutex_);

    auto it = pending_entries_.find(wal_sequence);
    if (it == pending_entries_.end()) {
        logger_->warn("WAL", std::format(
            "Commit для несуществующего seq={}", wal_sequence));
        return std::unexpected(TbError::WalWriteFailed);
    }

    auto& entry = it->second;
    entry.committed = true;

    const auto wrapped = wrap_wal_json(
        wal_sequence, entry.type, true, entry.payload_json);

    auto result = journal_->append(
        journal_type_for(entry.type),
        wrapped,
        entry.correlation_id,
        entry.strategy_id);

    if (!result) {
        logger_->error("WAL", std::format(
            "Ошибка записи commit seq={}", wal_sequence));
        return std::unexpected(TbError::WalWriteFailed);
    }

    pending_entries_.erase(it);
    metrics_->counter("wal_commits_total")->increment();

    logger_->debug("WAL", std::format("Commit: seq={}", wal_sequence));
    return {};
}

VoidResult WalWriter::rollback(uint64_t wal_sequence, const std::string& reason) {
    std::lock_guard lock(mutex_);

    auto it = pending_entries_.find(wal_sequence);
    if (it == pending_entries_.end()) {
        logger_->warn("WAL", std::format(
            "Rollback для несуществующего seq={}", wal_sequence));
        return std::unexpected(TbError::WalWriteFailed);
    }

    const auto& entry = it->second;

    const auto rollback_json = std::format(
        R"({{"wal_seq":{},"wal_type":"{}","rollback":true,"reason":"{}"}})",
        wal_sequence, to_string(entry.type), reason);

    auto result = journal_->append(
        journal_type_for(entry.type),
        rollback_json,
        entry.correlation_id,
        entry.strategy_id);

    if (!result) {
        logger_->error("WAL", std::format(
            "Ошибка записи rollback seq={}", wal_sequence));
        return std::unexpected(TbError::WalWriteFailed);
    }

    pending_entries_.erase(it);
    metrics_->counter("wal_rollbacks_total")->increment();

    logger_->warn("WAL", std::format(
        "Rollback: seq={} причина: {}", wal_sequence, reason));
    return {};
}

Result<std::vector<WalEntry>> WalWriter::find_uncommitted() {
    std::lock_guard lock(mutex_);

    std::vector<WalEntry> uncommitted;

    // 1. Из in-memory pending (для normal runtime)
    for (const auto& [seq, entry] : pending_entries_) {
        if (!entry.committed) {
            uncommitted.push_back(entry);
        }
    }

    // 2. Сканируем journal на диске — ищем WAL записи с committed=false,
    //    у которых НЕТ последующего commit или rollback.
    //    Это критично для crash-recovery: после рестарта pending_entries_ пуст.
    if (uncommitted.empty()) {
        auto all_entries = journal_->query(
            Timestamp(0), Timestamp(std::numeric_limits<int64_t>::max()),
            std::nullopt);
        if (all_entries.has_value()) {
            // Собираем committed и rolled-back sequences
            std::unordered_set<uint64_t> resolved_seqs;
            for (const auto& je : all_entries.value()) {
                if (je.payload_json.empty()) continue;
                try {
                    auto json = boost::json::parse(je.payload_json);
                    auto& obj = json.as_object();
                    if (!obj.contains("wal_seq")) continue;
                    uint64_t seq = static_cast<uint64_t>(obj.at("wal_seq").as_int64());
                    bool is_committed = obj.contains("committed") && obj.at("committed").as_bool();
                    bool is_rollback = obj.contains("rollback") && obj.at("rollback").as_bool();
                    if (is_committed || is_rollback) {
                        resolved_seqs.insert(seq);
                    }
                } catch (const std::exception& e) {
                    logger_->warn("WAL", "Corrupted WAL entry during resolved-scan",
                        {{"error", e.what()}, {"payload_size", std::to_string(je.payload_json.size())}});
                    ++wal_corruption_count_;
                }
            }
            // Теперь ищем uncommitted без resolved
            for (const auto& je : all_entries.value()) {
                if (je.payload_json.empty()) continue;
                try {
                    auto json = boost::json::parse(je.payload_json);
                    auto& obj = json.as_object();
                    if (!obj.contains("wal_seq") || !obj.contains("wal_type")) continue;
                    uint64_t seq = static_cast<uint64_t>(obj.at("wal_seq").as_int64());
                    bool is_committed = obj.contains("committed") && obj.at("committed").as_bool();
                    bool is_rollback = obj.contains("rollback") && obj.at("rollback").as_bool();
                    if (!is_committed && !is_rollback && resolved_seqs.find(seq) == resolved_seqs.end()) {
                        WalEntry entry;
                        entry.wal_sequence = seq;
                        entry.committed = false;
                        entry.payload_json = je.payload_json;
                        if (obj.contains("data") && obj.at("data").is_object()) {
                            auto& d = obj.at("data").as_object();
                            if (d.contains("symbol"))
                                entry.symbol = Symbol(std::string(d.at("symbol").as_string()));
                            if (d.contains("order_id"))
                                entry.order_id = OrderId(std::string(d.at("order_id").as_string()));
                        }
                        uncommitted.push_back(std::move(entry));
                    }
                } catch (const std::exception& e) {
                    logger_->warn("WAL", "Corrupted WAL entry during uncommitted-scan",
                        {{"error", e.what()}, {"payload_size", std::to_string(je.payload_json.size())}});
                    ++wal_corruption_count_;
                }
            }
        }
    }

    logger_->info("WAL", std::format(
        "Найдено {} незавершённых записей", uncommitted.size()));

    return uncommitted;
}

VoidResult WalWriter::write_checkpoint(const std::string& snapshot_json) {
    // ИСПРАВЛЕНИЕ: Инкремент sequence должен быть под мьютексом для гарантии монотонности
    std::lock_guard lock(mutex_);

    const uint64_t seq = ++wal_sequence_;

    const auto wrapped = wrap_wal_json(
        seq, WalEntryType::RecoveryCheckpoint, true, snapshot_json);

    auto result = journal_->append(
        JournalEntryType::SystemEvent,
        wrapped);

    if (!result) {
        logger_->error("WAL", "Ошибка записи checkpoint");
        return std::unexpected(TbError::WalWriteFailed);
    }

    metrics_->counter("wal_writes_total")->increment();

    logger_->info("WAL", std::format("Checkpoint записан: seq={}", seq));
    return {};
}

VoidResult WalWriter::flush() {
    auto result = journal_->flush();
    if (!result) {
        logger_->error("WAL", "Ошибка flush журнала");
        return std::unexpected(TbError::PersistenceError);
    }
    return {};
}

} // namespace tb::persistence
