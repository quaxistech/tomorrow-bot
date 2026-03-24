#!/bin/bash
# Сборка Tomorrow Bot в режиме Release с оптимизациями
# Использование: ./scripts/build_release.sh
# Результат: build-release/tomorrow-bot

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-release"

echo "=== Tomorrow Bot: Release сборка ==="
echo "Директория проекта: $PROJECT_DIR"
echo "Директория сборки:  $BUILD_DIR"
echo ""

mkdir -p "$BUILD_DIR"

# Конфигурация CMake с LTO и оптимизациями
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DENABLE_LTO=ON \
    "$@"

echo ""
echo "=== Компиляция ==="

JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

cmake --build "$BUILD_DIR" --parallel "$JOBS"

echo ""
echo "=== Release сборка завершена ==="
echo "Исполняемый файл: $BUILD_DIR/src/app/tomorrow-bot"
echo "Размер: $(du -sh "$BUILD_DIR/src/app/tomorrow-bot" 2>/dev/null | cut -f1 || echo 'N/A')"
