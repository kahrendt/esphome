#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/macros.h"
#include <atomic>
#include <span>

#ifdef USE_ESP32
// Included unconditionally rather than under CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS: the sensor
// setters below must exist whenever the YAML asks for them, and that config option is only
// reachable on the ESP-IDF framework. Without run-time stats the fields are simply never sampled.
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace debug {

static constexpr size_t DEVICE_INFO_BUFFER_SIZE = 256;
static constexpr size_t RESET_REASON_BUFFER_SIZE = 128;
static constexpr size_t WAKEUP_CAUSE_BUFFER_SIZE = 128;

#if defined(USE_ESP32) && defined(USE_SENSOR)
/// How many cores' idle counters are tracked. Sampled for every core regardless of how many
/// per-core sensors the YAML declares, since the overall cpu_idle figure needs all of them.
static constexpr size_t CPU_CORE_COUNT = portNUM_PROCESSORS;

/// Reported for a task that is not pinned to any core (FreeRTOS's tskNO_AFFINITY), narrowed to
/// something that fits alongside the real core IDs in an int8_t.
static constexpr int8_t CORE_UNPINNED = -1;
/// Affinity has not been read yet, so the first read is not reported as a change.
static constexpr int8_t CORE_UNKNOWN = -2;

/// One monitored task. The handle is resolved once (see resolve_task_handle_()) and the run-time
/// counter is then read straight out of that one TCB every sample, so the steady-state cost is a
/// fixed handful of instructions per task rather than a walk of every task list.
struct TaskCpuSensor {
  const char *task_name;
  sensor::Sensor *sensor;
  TaskHandle_t handle{nullptr};
  configRUN_TIME_COUNTER_TYPE last_counter{0};
  /// Attempts spent looking the name up so far; capped by TASK_RESOLVE_MAX_ATTEMPTS because the
  /// lookup itself is the expensive kind of call this component exists to avoid making repeatedly.
  uint8_t resolve_attempts{0};
  /// Affinity as of the last sample, so a task that gets pinned mid-run is reported when it moves.
  /// CORE_UNKNOWN until the first sample; real values are a core index or CORE_UNPINNED.
  int8_t last_core_id{CORE_UNKNOWN};
  std::atomic<float> percentage{NAN};
};

/// Idle percentage of one core, from that core's idle-task run-time counter.
struct CoreCpuSensor {
  uint8_t core_id;
  sensor::Sensor *sensor;
  std::atomic<float> idle_percentage{NAN};
};
#endif

// buf_append_printf is now provided by esphome/core/helpers.h

class DebugComponent : public PollingComponent {
 public:
  void loop() override;
#ifdef USE_ESP32
  void setup() override;
#endif
  void update() override;
  float get_setup_priority() const override;
  void dump_config() override;

#ifdef USE_TEXT_SENSOR
  void set_device_info_sensor(text_sensor::TextSensor *device_info) { device_info_ = device_info; }
  void set_reset_reason_sensor(text_sensor::TextSensor *reset_reason) { reset_reason_ = reset_reason; }
#endif  // USE_TEXT_SENSOR
#ifdef USE_SENSOR
  void set_free_sensor(sensor::Sensor *free_sensor) { free_sensor_ = free_sensor; }
  void set_block_sensor(sensor::Sensor *block_sensor) { block_sensor_ = block_sensor; }
#if (defined(USE_ESP8266) && USE_ARDUINO_VERSION_CODE >= VERSION_CODE(2, 5, 2)) || defined(USE_ESP32)
  void set_fragmentation_sensor(sensor::Sensor *fragmentation_sensor) { fragmentation_sensor_ = fragmentation_sensor; }
#endif
#if defined(USE_ESP32) || defined(USE_LIBRETINY)
  void set_min_free_sensor(sensor::Sensor *min_free_sensor) { min_free_sensor_ = min_free_sensor; }
#endif
  void set_loop_time_sensor(sensor::Sensor *loop_time_sensor) { loop_time_sensor_ = loop_time_sensor; }
#ifdef USE_ESP32
  void set_psram_sensor(sensor::Sensor *psram_sensor) { this->psram_sensor_ = psram_sensor; }
  void set_cpu_idle_sensor(sensor::Sensor *cpu_idle_sensor) { this->cpu_idle_sensor_ = cpu_idle_sensor; }
  void init_task_cpu_sensors(size_t count) { this->task_cpu_sensors_.init(count); }
  void add_task_cpu_sensor(const char *task_name, sensor::Sensor *sensor);
  void init_core_cpu_sensors(size_t count) { this->core_cpu_sensors_.init(count); }
  void add_core_cpu_sensor(uint8_t core_id, sensor::Sensor *sensor);
#endif  // USE_ESP32
  void set_cpu_frequency_sensor(sensor::Sensor *cpu_frequency_sensor) {
    this->cpu_frequency_sensor_ = cpu_frequency_sensor;
  }
#endif  // USE_SENSOR
#ifdef USE_ESP32
  // Outside USE_SENSOR on purpose: log_cpu_usage is a logging-only feature, so `debug:` with no
  // sensor platform at all must still compile the task that services it.
  void set_log_cpu_usage(bool log_cpu_usage) { this->log_cpu_usage_ = log_cpu_usage; }
  bool get_log_cpu_usage() const { return this->log_cpu_usage_; }
  static void stats_task_(void *arg);
  void on_shutdown() override;
#endif  // USE_ESP32
 protected:
  uint32_t free_heap_{};

#ifdef USE_SENSOR
  uint32_t last_loop_timetag_{0};
  uint32_t max_loop_time_{0};

  sensor::Sensor *free_sensor_{nullptr};
  sensor::Sensor *block_sensor_{nullptr};
#if (defined(USE_ESP8266) && USE_ARDUINO_VERSION_CODE >= VERSION_CODE(2, 5, 2)) || defined(USE_ESP32)
  sensor::Sensor *fragmentation_sensor_{nullptr};
#endif
#if defined(USE_ESP32) || defined(USE_LIBRETINY)
  sensor::Sensor *min_free_sensor_{nullptr};
#endif
  sensor::Sensor *loop_time_sensor_{nullptr};
#ifdef USE_ESP32
  sensor::Sensor *psram_sensor_{nullptr};
  sensor::Sensor *cpu_idle_sensor_{nullptr};
  std::atomic<float> cpu_idle_percentage_{NAN};
  FixedVector<TaskCpuSensor *> task_cpu_sensors_;
  FixedVector<CoreCpuSensor *> core_cpu_sensors_;
  /// Previous sample's idle-task counters, one per core, and the run-time counter the whole set was
  /// read at. Owned by the stats task; nothing else touches them.
  configRUN_TIME_COUNTER_TYPE last_idle_counters_[CPU_CORE_COUNT]{};
  configRUN_TIME_COUNTER_TYPE last_sample_counter_{0};

  /// Take one sample of the idle and per-task run-time counters and publish the percentages that
  /// have accumulated since the previous one. The first call only seeds the counters (there is no
  /// previous sample to difference against), which is what `prime` selects.
  void sample_cpu_stats_(bool prime);
  /// Look a monitored task's name up once and cache its handle. Returns false while it is still
  /// unresolved - tasks created after setup() are normal, so this keeps retrying up to a cap.
  bool resolve_task_handle_(TaskCpuSensor *entry);
#endif  // USE_ESP32
  sensor::Sensor *cpu_frequency_sensor_{nullptr};
#endif  // USE_SENSOR

#if defined(USE_ESP32) || defined(USE_ZEPHYR)
  /**
   * @brief Logs information about the device's partition table.
   *
   * This function iterates through the partition table and logs details
   * about each partition, including its name, type, subtype, starting address,
   * and size. The information is useful for diagnosing issues related to flash
   * memory or verifying the partition configuration dynamically at runtime.
   *
   * Only available when compiled for ESP32 and ZEPHYR platforms.
   */
  void log_partition_info_();
#endif

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *device_info_{nullptr};
  text_sensor::TextSensor *reset_reason_{nullptr};
#endif  // USE_TEXT_SENSOR

#ifdef USE_ESP32
  bool log_cpu_usage_;
#endif

  const char *get_reset_reason_(std::span<char, RESET_REASON_BUFFER_SIZE> buffer);
  const char *get_wakeup_cause_(std::span<char, WAKEUP_CAUSE_BUFFER_SIZE> buffer);
  uint32_t get_free_heap_();
  size_t get_device_info_(std::span<char, DEVICE_INFO_BUFFER_SIZE> buffer, size_t pos);
  void update_platform_();
};

}  // namespace debug
}  // namespace esphome
