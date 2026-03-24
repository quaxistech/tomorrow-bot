#!/bin/bash
# Сборка Tomorrow Bot в режиме Debug с санитайзерами
# Использование: ./scripts/build_debug.sh
# Результат: build-debug/tomorrow-bot

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-debug"

echo "=== Tomorrow Bot: Debug сборка ==="
echo "Директория проекта: $PROJECT_DIR"
echo "Директория сборки:  $BUILD_DIR"
echo ""

# Создаём директорию сборки
mkdir -p "$BUILD_DIR"

# Конфигурация CMake
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DENABLE_LTO=OFF \
    "$@"

echo ""
echo "=== Компиляция ==="

# Определяем количество потоков для сборки
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

cmake --build "$BUILD_DIR" --parallel "$JOBS"

echo ""
echo "=== Сборка завершена ==="
echo "Исполняемый файл: $BUILD_DIR/src/app/tomorrow-bot"
