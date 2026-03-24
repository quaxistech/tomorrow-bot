#!/usr/bin/env bash
# run_production.sh — Запуск Tomorrow Bot в production-режиме
# ВНИМАНИЕ: этот скрипт запускает РЕАЛЬНУЮ торговлю!
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

# Загрузка переменных окружения
if [[ -f .env ]]; then
    set -a
    source .env
    set +a
    echo "✓ Переменные окружения загружены из .env"
else
    echo "✗ ОШИБКА: Файл .env не найден в $PROJECT_ROOT"
    echo "  Создайте .env с API-ключами Bitget (см. deploy/env/production.env.template)"
    exit 1
fi

# Проверка наличия API-ключей
if [[ -z "${BITGET_API_KEY:-}" ]]; then
    echo "✗ ОШИБКА: BITGET_API_KEY не задан в .env"
    exit 1
fi

# Поиск бинарника (release > debug)
BINARY=""
if [[ -x "build-release/src/app/tomorrow-bot" ]]; then
    BINARY="build-release/src/app/tomorrow-bot"
    echo "✓ Используется Release-сборка"
elif [[ -x "build-check/src/app/tomorrow-bot" ]]; then
    BINARY="build-check/src/app/tomorrow-bot"
    echo "⚠ Используется Debug-сборка (рекомендуется Release для production)"
elif [[ -x "build-debug/src/app/tomorrow-bot" ]]; then
    BINARY="build-debug/src/app/tomorrow-bot"
    echo "⚠ Используется Debug-сборка (рекомендуется Release для production)"
else
    echo "✗ ОШИБКА: Бинарник не найден. Соберите проект:"
    echo "  cmake -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j\$(nproc)"
    exit 1
fi

# Создание директории для логов
mkdir -p logs

CONFIG="configs/production.yaml"

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║  ⚠  PRODUCTION MODE — РЕАЛЬНЫЕ ДЕНЬГИ   ║"
echo "╠══════════════════════════════════════════╣"
echo "║  Конфиг: $CONFIG"
echo "║  Бинарник: $BINARY"
echo "║  Логи: logs/production.log"
echo "╚══════════════════════════════════════════╝"
echo ""
echo "Запуск через 3 секунды... (Ctrl+C для отмены)"
sleep 3

exec "$BINARY" -c "$CONFIG"
