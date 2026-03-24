#!/bin/bash
# Запуск Tomorrow Bot в бумажном торговом режиме (Paper Trading)
# Использование: ./scripts/run_paper.sh [дополнительные аргументы]
#
# ТРЕБОВАНИЯ:
# - Собрать проект: ./scripts/build_debug.sh или build_release.sh
# - Задать переменные окружения или создать .env файл

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Ищем собранный бинарник (сначала release, потом debug)
if [ -f "$PROJECT_DIR/build-release/src/app/tomorrow-bot" ]; then
    BINARY="$PROJECT_DIR/build-release/src/app/tomorrow-bot"
elif [ -f "$PROJECT_DIR/build-debug/src/app/tomorrow-bot" ]; then
    BINARY="$PROJECT_DIR/build-debug/src/app/tomorrow-bot"
else
    echo "ОШИБКА: tomorrow-bot не найден. Выполните сборку:"
    echo "  ./scripts/build_debug.sh"
    exit 1
fi

CONFIG="$PROJECT_DIR/configs/paper.yaml"

# Загрузка переменных окружения из .env (если существует)
if [ -f "$PROJECT_DIR/.env" ]; then
    echo "Загрузка переменных окружения из .env..."
    set -a
    source "$PROJECT_DIR/.env"
    set +a
else
    echo "ПРЕДУПРЕЖДЕНИЕ: файл .env не найден."
    echo "Скопируйте: cp deploy/env/paper.env.template .env и заполните значения"
fi

echo "=== Запуск Tomorrow Bot (Paper Mode) ==="
echo "Бинарник:      $BINARY"
echo "Конфигурация:  $CONFIG"
echo ""

exec "$BINARY" --config="$CONFIG" "$@"
