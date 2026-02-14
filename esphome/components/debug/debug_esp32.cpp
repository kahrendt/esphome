#include "debug_component.h"

#ifdef USE_ESP32
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <esp_sleep.h>
#include <esp_idf_version.h>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_partition.h>

#ifdef USE_ARDUINO
#include <Esp.h>
#endif

#include "sdkconfig.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const UBaseType_t STATS_TASK_PRIORIY = 3;
static const uint32_t STATS_DELAY_MS = 5000;
static const uint32_t ARRAY_SIZE_OFFSET = 5;  // Increase this if print_real_time_stats returns ESP_ERR_INVALID_SIZE
static const uint32_t FREERTOS_NUMBER_OF_CORES = 2;

static esp_err_t print_real_time_stats(TickType_t xTicksToWait) {
  TaskStatus_t *start_array = nullptr, *end_array = nullptr;
  UBaseType_t start_array_size, end_array_size;
  uint32_t start_run_time, end_run_time;
  esp_err_t ret;

  // Allocate array to store current task states
  start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
  size_t size = start_array_size * sizeof(TaskStatus_t);
  start_array = static_cast<TaskStatus_t *>(malloc(size));
  if (start_array == NULL) {
    ret = ESP_ERR_NO_MEM;
    free(start_array);
    free(end_array);
    return ret;
  }
  // Get current task states
  start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
  if (start_array_size == 0) {
    ret = ESP_ERR_INVALID_SIZE;
    free(start_array);
    free(end_array);
    return ret;
  }

  vTaskDelay(xTicksToWait);

  // Allocate array to store tasks states post delay
  end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
  end_array = static_cast<TaskStatus_t *>(malloc(sizeof(TaskStatus_t) * end_array_size));
  if (end_array == NULL) {
    ret = ESP_ERR_NO_MEM;
    free(start_array);
    free(end_array);
    return ret;
  }
  // Get post delay task states
  end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
  if (end_array_size == 0) {
    ret = ESP_ERR_INVALID_SIZE;
    free(start_array);
    free(end_array);
    return ret;
  }

  // Calculate total_elapsed_time in units of run time stats clock period.
  uint32_t total_elapsed_time = (end_run_time - start_run_time);
  if (total_elapsed_time == 0) {
    ret = ESP_ERR_INVALID_STATE;
    free(start_array);
    free(end_array);
    return ret;
  }

  printf("| Task | Run Time | Percentage\n");
  // Match each task in start_array to those in the end_array
  for (int i = 0; i < start_array_size; i++) {
    int k = -1;
    for (int j = 0; j < end_array_size; j++) {
      if (start_array[i].xHandle == end_array[j].xHandle) {
        k = j;
        // Mark that task have been matched by overwriting their handles
        start_array[i].xHandle = NULL;
        end_array[j].xHandle = NULL;
        break;
      }
    }
    // Check if matching task found
    if (k >= 0) {
      uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
      uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * FREERTOS_NUMBER_OF_CORES);
      printf("| %s | %" PRIu32 " | %" PRIu32 "%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
    }
  }

  // Print unmatched tasks
  for (int i = 0; i < start_array_size; i++) {
    if (start_array[i].xHandle != NULL) {
      printf("| %s | Deleted\n", start_array[i].pcTaskName);
    }
  }
  for (int i = 0; i < end_array_size; i++) {
    if (end_array[i].xHandle != NULL) {
      printf("| %s | Created\n", end_array[i].pcTaskName);
    }
  }
  ret = ESP_OK;

  // exit:  // Common return path
  free(start_array);
  free(end_array);
  return ret;
}

static void stats_task(void *arg) {
  // Print real time stats periodically
  while (1) {
    printf("\n\nGetting real time stats over %" PRIu32 " ms\n", STATS_DELAY_MS);
    esp_err_t err = print_real_time_stats(pdMS_TO_TICKS(STATS_DELAY_MS));
    if (err == ESP_OK) {
      printf("Real time stats obtained\n");
    } else {
      printf("Error getting real time stats\n");
      printf("Error: %s", esp_err_to_name(err));
    }
  }
}
#endif  // CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

namespace esphome {
namespace debug {

static const char *const TAG = "debug";

// index by values returned by esp_reset_reason

static const char *const RESET_REASONS[] = {
    "unknown source",
    "power-on event",
    "external pin",
    "software via esp_restart",
    "exception/panic",
    "interrupt watchdog",
    "task watchdog",
    "other watchdogs",
    "exiting deep sleep mode",
    "brownout",
    "SDIO",
    "USB peripheral",
    "JTAG",
    "efuse error",
    "power glitch detected",
    "CPU lock up",
};

static const char *const REBOOT_KEY = "reboot_source";
static const size_t REBOOT_MAX_LEN = 24;

// on shutdown, store the source of the reboot request
void DebugComponent::on_shutdown() {
  auto *component = App.get_current_component();
  char buffer[REBOOT_MAX_LEN]{};
  auto pref = global_preferences->make_preference(REBOOT_MAX_LEN,
                                                  fnv1_hash_extend(fnv1_hash(REBOOT_KEY), App.get_name().c_str()));
  if (component != nullptr) {
    strncpy(buffer, LOG_STR_ARG(component->get_component_log_str()), REBOOT_MAX_LEN - 1);
    buffer[REBOOT_MAX_LEN - 1] = '\0';
  }
  ESP_LOGD(TAG, "Storing reboot source: %s", buffer);
  pref.save(&buffer);
  global_preferences->sync();
}

const char *DebugComponent::get_reset_reason_(std::span<char, RESET_REASON_BUFFER_SIZE> buffer) {
  char *buf = buffer.data();
  const size_t size = RESET_REASON_BUFFER_SIZE;

  unsigned reason = esp_reset_reason();
  if (reason < sizeof(RESET_REASONS) / sizeof(RESET_REASONS[0])) {
    if (reason == ESP_RST_SW) {
      auto pref = global_preferences->make_preference(REBOOT_MAX_LEN,
                                                      fnv1_hash_extend(fnv1_hash(REBOOT_KEY), App.get_name().c_str()));
      char reboot_source[REBOOT_MAX_LEN]{};
      if (pref.load(&reboot_source)) {
        reboot_source[REBOOT_MAX_LEN - 1] = '\0';
        snprintf(buf, size, "Reboot request from %s", reboot_source);
      } else {
        snprintf(buf, size, "%s", RESET_REASONS[reason]);
      }
    } else {
      snprintf(buf, size, "%s", RESET_REASONS[reason]);
    }
  } else {
    snprintf(buf, size, "unknown source");
  }
  return buf;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
static const char *const WAKEUP_CAUSES[] = {
    "undefined",                       // ESP_SLEEP_WAKEUP_UNDEFINED (0)
    "undefined",                       // ESP_SLEEP_WAKEUP_ALL (1)
    "external signal using RTC_IO",    // ESP_SLEEP_WAKEUP_EXT0 (2)
    "external signal using RTC_CNTL",  // ESP_SLEEP_WAKEUP_EXT1 (3)
    "timer",                           // ESP_SLEEP_WAKEUP_TIMER (4)
    "touchpad",                        // ESP_SLEEP_WAKEUP_TOUCHPAD (5)
    "ULP program",                     // ESP_SLEEP_WAKEUP_ULP (6)
    "GPIO",                            // ESP_SLEEP_WAKEUP_GPIO (7)
    "UART",                            // ESP_SLEEP_WAKEUP_UART (8)
    "UART1",                           // ESP_SLEEP_WAKEUP_UART1 (9)
    "UART2",                           // ESP_SLEEP_WAKEUP_UART2 (10)
    "WIFI",                            // ESP_SLEEP_WAKEUP_WIFI (11)
    "COCPU int",                       // ESP_SLEEP_WAKEUP_COCPU (12)
    "COCPU crash",                     // ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG (13)
    "BT",                              // ESP_SLEEP_WAKEUP_BT (14)
    "VAD",                             // ESP_SLEEP_WAKEUP_VAD (15)
    "VBAT under voltage",              // ESP_SLEEP_WAKEUP_VBAT_UNDER_VOLT (16)
};
#else
static const char *const WAKEUP_CAUSES[] = {
    "undefined",                       // ESP_SLEEP_WAKEUP_UNDEFINED (0)
    "undefined",                       // ESP_SLEEP_WAKEUP_ALL (1)
    "external signal using RTC_IO",    // ESP_SLEEP_WAKEUP_EXT0 (2)
    "external signal using RTC_CNTL",  // ESP_SLEEP_WAKEUP_EXT1 (3)
    "timer",                           // ESP_SLEEP_WAKEUP_TIMER (4)
    "touchpad",                        // ESP_SLEEP_WAKEUP_TOUCHPAD (5)
    "ULP program",                     // ESP_SLEEP_WAKEUP_ULP (6)
    "GPIO",                            // ESP_SLEEP_WAKEUP_GPIO (7)
    "UART",                            // ESP_SLEEP_WAKEUP_UART (8)
    "WIFI",                            // ESP_SLEEP_WAKEUP_WIFI (9)
    "COCPU int",                       // ESP_SLEEP_WAKEUP_COCPU (10)
    "COCPU crash",                     // ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG (11)
    "BT",                              // ESP_SLEEP_WAKEUP_BT (12)
};
#endif

const char *DebugComponent::get_wakeup_cause_(std::span<char, WAKEUP_CAUSE_BUFFER_SIZE> buffer) {
  static constexpr auto NUM_CAUSES = sizeof(WAKEUP_CAUSES) / sizeof(WAKEUP_CAUSES[0]);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  // IDF 6.0+ returns a bitmap of all wakeup sources
  uint32_t causes = esp_sleep_get_wakeup_causes();
  if (causes == 0) {
    return WAKEUP_CAUSES[0];  // "undefined"
  }
  char *p = buffer.data();
  char *end = p + buffer.size();
  *p = '\0';
  const char *sep = "";
  for (unsigned i = 0; i < NUM_CAUSES && p < end; i++) {
    if (causes & (1U << i)) {
      size_t needed = strlen(sep) + strlen(WAKEUP_CAUSES[i]);
      if (p + needed >= end) {
        break;
      }
      p += snprintf(p, end - p, "%s%s", sep, WAKEUP_CAUSES[i]);
      sep = ", ";
    }
  }
  return buffer.data();
#else
  unsigned reason = esp_sleep_get_wakeup_cause();
  if (reason < NUM_CAUSES) {
    return WAKEUP_CAUSES[reason];
  }
  return "unknown source";
#endif
}

void DebugComponent::setup() {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  if (this->log_cpu_usage_) {
    xTaskCreate(stats_task, "stats", 4096, nullptr, STATS_TASK_PRIORIY, nullptr);
  }
#endif  // CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
}

void DebugComponent::log_partition_info_() {
  ESP_LOGCONFIG(TAG,
                "Partition table:\n"
                "  %-12s %-4s %-8s %-10s %-10s",
                "Name", "Type", "Subtype", "Address", "Size");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL) {
    const esp_partition_t *partition = esp_partition_get(it);
    ESP_LOGCONFIG(TAG, "  %-12s %-4d %-8d 0x%08" PRIX32 " 0x%08" PRIX32, partition->label, partition->type,
                  partition->subtype, partition->address, partition->size);
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
}

uint32_t DebugComponent::get_free_heap_() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

struct ChipFeature {
  int bit;
  const char *name;
};

static constexpr ChipFeature CHIP_FEATURES[] = {
    {CHIP_FEATURE_BLE, "BLE"},
    {CHIP_FEATURE_BT, "BT"},
    {CHIP_FEATURE_EMB_FLASH, "EMB Flash"},
    {CHIP_FEATURE_EMB_PSRAM, "EMB PSRAM"},
    {CHIP_FEATURE_WIFI_BGN, "2.4GHz WiFi"},
};

size_t DebugComponent::get_device_info_(std::span<char, DEVICE_INFO_BUFFER_SIZE> buffer, size_t pos) {
  constexpr size_t size = DEVICE_INFO_BUFFER_SIZE;
  char *buf = buffer.data();

#if defined(USE_ARDUINO)
  const char *flash_mode;
  switch (ESP.getFlashChipMode()) {  // NOLINT(readability-static-accessed-through-instance)
    case FM_QIO:
      flash_mode = "QIO";
      break;
    case FM_QOUT:
      flash_mode = "QOUT";
      break;
    case FM_DIO:
      flash_mode = "DIO";
      break;
    case FM_DOUT:
      flash_mode = "DOUT";
      break;
    case FM_FAST_READ:
      flash_mode = "FAST_READ";
      break;
    case FM_SLOW_READ:
      flash_mode = "SLOW_READ";
      break;
    default:
      flash_mode = "UNKNOWN";
  }
  uint32_t flash_size = ESP.getFlashChipSize() / 1024;       // NOLINT
  uint32_t flash_speed = ESP.getFlashChipSpeed() / 1000000;  // NOLINT
  pos = buf_append_printf(buf, size, pos, "|Flash: %" PRIu32 "kB Speed:%" PRIu32 "MHz Mode:%s", flash_size, flash_speed,
                          flash_mode);
#endif

  esp_chip_info_t info;
  esp_chip_info(&info);
  const char *model = ESPHOME_VARIANT;

  // Build features string
  pos = buf_append_str(buf, size, pos, "|Chip: ");
  pos = buf_append_str(buf, size, pos, model);
  pos = buf_append_str(buf, size, pos, " Features:");
  bool first_feature = true;
  for (const auto &feature : CHIP_FEATURES) {
    if (info.features & feature.bit) {
      pos = buf_append_str(buf, size, pos, first_feature ? "" : ", ");
      pos = buf_append_str(buf, size, pos, feature.name);
      first_feature = false;
      info.features &= ~feature.bit;
    }
  }
  if (info.features != 0) {
    pos = buf_append_str(buf, size, pos, first_feature ? "" : ", ");
    pos = buf_append_printf(buf, size, pos, "Other:0x%" PRIx32, info.features);
  }
  pos = buf_append_printf(buf, size, pos, " Cores:%u Revision:%u", info.cores, info.revision);

  uint32_t cpu_freq_mhz = arch_get_cpu_freq_hz() / 1000000;
  pos = buf_append_printf(buf, size, pos, "|CPU Frequency: %" PRIu32 " MHz", cpu_freq_mhz);

  char reset_buffer[RESET_REASON_BUFFER_SIZE];
  char wakeup_buffer[WAKEUP_CAUSE_BUFFER_SIZE];
  const char *reset_reason = get_reset_reason_(std::span<char, RESET_REASON_BUFFER_SIZE>(reset_buffer));
  const char *wakeup_cause = get_wakeup_cause_(std::span<char, WAKEUP_CAUSE_BUFFER_SIZE>(wakeup_buffer));

  uint8_t mac[6];
  get_mac_address_raw(mac);

  ESP_LOGD(TAG,
           "ESP32 debug info:\n"
           "  Chip: %s\n"
           "  Cores: %u\n"
           "  Revision: %u\n"
           "  CPU Frequency: %" PRIu32 " MHz\n"
           "  ESP-IDF Version: %s\n"
           "  EFuse MAC: %02X:%02X:%02X:%02X:%02X:%02X\n"
           "  Reset Reason: %s\n"
           "  Wakeup Cause: %s",
           model, info.cores, info.revision, cpu_freq_mhz, esp_get_idf_version(), mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5], reset_reason, wakeup_cause);
#if defined(USE_ARDUINO)
  ESP_LOGD(TAG, "  Flash: Size=%" PRIu32 "kB Speed=%" PRIu32 "MHz Mode=%s", flash_size, flash_speed, flash_mode);
#endif
  // Framework detection
#ifdef USE_ARDUINO
  ESP_LOGD(TAG, "  Framework: Arduino");
  pos = buf_append_str(buf, size, pos, "|Framework: Arduino");
#else
  ESP_LOGD(TAG, "  Framework: ESP-IDF");
  pos = buf_append_str(buf, size, pos, "|Framework: ESP-IDF");
#endif

  pos = buf_append_str(buf, size, pos, "|ESP-IDF: ");
  pos = buf_append_str(buf, size, pos, esp_get_idf_version());
  pos = buf_append_printf(buf, size, pos, "|EFuse MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                          mac[4], mac[5]);
  pos = buf_append_str(buf, size, pos, "|Reset: ");
  pos = buf_append_str(buf, size, pos, reset_reason);
  pos = buf_append_str(buf, size, pos, "|Wakeup: ");
  pos = buf_append_str(buf, size, pos, wakeup_cause);

  return pos;
}

void DebugComponent::update_platform_() {
#ifdef USE_SENSOR
  uint32_t max_alloc = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (this->block_sensor_ != nullptr) {
    this->block_sensor_->publish_state(max_alloc);
  }
  if (this->min_free_sensor_ != nullptr) {
    this->min_free_sensor_->publish_state(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  }
  if (this->fragmentation_sensor_ != nullptr) {
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_heap > 0) {
      float fragmentation = 100.0f - (100.0f * max_alloc / free_heap);
      this->fragmentation_sensor_->publish_state(fragmentation);
    }
  }
  if (this->psram_sensor_ != nullptr) {
    this->psram_sensor_->publish_state(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
#endif
}

}  // namespace debug
}  // namespace esphome
#endif  // USE_ESP32
