#pragma once

#include <Arduino.h>
#include <esp_system.h>

#include "DataServices.h"

namespace pixelos {

// A deliberately small set of contexts is retained in RTC slow memory. The
// breadcrumb is diagnostic evidence, not a crash dump: it identifies the app,
// provider, and broad subsystem that was active near the previous reset.
enum class CrashContext : uint8_t {
  Startup = 0,
  Runtime,
  Sync,
  Web,
  Reboot,
  FirmwareUpdate,
};

struct CrashBreadcrumbSnapshot {
  bool available = false;
  bool crashRelated = false;
  esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
  uint32_t sequence = 0;
  uint32_t uptimeMs = 0;
  char app[12]{};
  char provider[12]{};
  CrashContext context = CrashContext::Startup;
};

// Call once, before the current boot starts overwriting RTC state.
void beginCrashBreadcrumb(esp_reset_reason_t resetReason);

// Writes one of two alternating RTC records. No heap or flash is used. Normal
// runtime calls are rate-limited by the caller; exceptional contexts may be
// recorded immediately before entering a risky operation.
void recordCrashBreadcrumb(const char* app, OnlineProvider provider,
                           CrashContext context, uint32_t uptimeMs);

CrashBreadcrumbSnapshot previousCrashBreadcrumb();
// Returns the most recent crash-related resets first. The bounded journal is
// persisted only after panic, watchdog, or brownout resets, so ordinary
// runtime breadcrumbs do not add flash wear.
size_t recentCrashBreadcrumbs(CrashBreadcrumbSnapshot* output,
                              size_t capacity);
const char* crashContextName(CrashContext context);
const char* crashProviderName(OnlineProvider provider);

}  // namespace pixelos
