# `src/telemetry` — Исследовательская телеметрия

## Назначение

Захват полного контекста каждой торговой decision'и в `TelemetryEnvelope` для post-mortem анализа, ML-тренинга, и инцидент-детекции. Дополняет Prometheus-метрики структурированными snapshots.

## Границы ответственности

* `ResearchTelemetry` — главный sink: capture(envelope), write to backend.
* `TelemetrySink` — interface (file / memory).
* `IncidentDetector` — 6 playbook detector'ов (large drawdown, cascading rejects, divergence, etc.).
* `ObservabilityPanels` — 7 панелей метрик для дашбордов.
* DTO: `TelemetryEnvelope`, `TelemetryEvent`, `Incident`.

## Публичные интерфейсы

* `class ResearchTelemetry`:
  * Конструктор `(TelemetrySink, ILogger)`.
  * `capture(TelemetryEnvelope)`.
  * `flush()`.
* `class TelemetrySink` — interface.
* `class FileTelemetrySink` — JSON-line файл.
* `class MemoryTelemetrySink` — для тестов.
* `class IncidentDetector` — `detect(state)` → optional<Incident>.
* `struct ObservabilityPanels` — 7 panels.

## Внутренние компоненты

* `research_telemetry.hpp/cpp`.
* `telemetry_sink.hpp` — interface.
* `file_telemetry_sink.hpp/cpp`, `memory_telemetry_sink.hpp/cpp`.
* `incident_detector.hpp/cpp`.
* `observability_panels.hpp` — DTO + helpers.
* `telemetry_types.hpp` — все DTO.

## Зависимости

* `logging`, `metrics`, `clock`.
* Большой fan-in: получает snapshot'ы от всех модулей через `TelemetryEnvelope`.

## Потоки данных

```
TradingPipeline после каждой decision:
  envelope = build_TelemetryEnvelope(seq_id, intent, risk, exec_alpha, decision, sizing, ...)
  telemetry_->capture(envelope)
       → sink_.write(envelope)

каждые 10 sec:
  state = collect_pipeline_state(...)
  incident = incident_detector_.detect(state)
  if incident: log + alert
```

## Race conditions

`ResearchTelemetry::capture` — должен быть thread-safe (thread-local buffer + flush, или mutex).

## Ошибки проектирования

* **D-tel-1 (MEDIUM).** `FileTelemetrySink` синхронный — при slow disk блокирует hot-path. Mitigation: async с ring buffer.
* **D-tel-2 (LOW).** Нет ротации файла — при долгом запуске файл растёт.
* **D-tel-3 (LOW).** Incident detector использует heuristics; thresholds hardcoded (требует верификации).

## Контракты

### `capture(envelope)`

* **Pre.** `envelope.seq_id` уникальный (монотонно возрастающий per pipeline).
* **Post.** Envelope записан в sink (flush определяется sink'ом).

## Производственные риски

* **R-tel-1.** Sync file write → backlog WS.
* **R-tel-2.** Disk full → telemetry stops (silently?). Mitigation: alert через metrics.

## Рекомендации

1. Async sink (background thread + bounded queue, drop-oldest).
2. Ротация по размеру (logrotate-aware или встроенная).
3. Incident detector thresholds — из config.
4. Тест: high-throughput capture (10K envelopes/sec), проверка overhead на pipeline.
