#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace tb::exchange::bitget {

// Сырые данные тикера от Bitget WebSocket
struct RawTicker {
    std::string symbol;
    std::string last;
    std::string bid;
    std::string ask;
    std::string volume_24h;
    std::string change_24h;
    int64_t ts;
};

// Сырые данные сделки от Bitget WebSocket
struct RawTrade {
    std::string symbol;
    std::string trade_id;
    std::string price;
    std::string size;
    std::string side;  // "buy" или "sell"
    int64_t ts;
};

// Уровень стакана (цена + объём)
struct RawBookLevel {
    std::string price;
    std::string size;
};

// Сырые данные стакана от Bitget WebSocket
struct RawOrderBook {
    std::string symbol;
    std::string action;  // "snapshot" или "update"
    std::vector<RawBookLevel> bids;
    std::vector<RawBookLevel> asks;
    int64_t ts;
    uint64_t sequence;
};

// Сырая свеча от Bitget WebSocket
struct RawCandle {
    std::string symbol;
    std::string interval;
    int64_t ts;
    std::string open;
    std::string high;
    std::string low;
    std::string close;
    std::string volume;
    std::string base_volume;
    bool is_closed;
};

// Тип WebSocket сообщения
enum class WsMsgType {
    Ticker, Trade, OrderBook, Candle,
    Heartbeat, Subscribe, Unsubscribe, Error, Unknown
};

// Сырое WS сообщение до разбора
struct RawWsMessage {
    WsMsgType type;
    std::string raw_payload;
    int64_t received_ns;
};

} // namespace tb::exchange::bitget
