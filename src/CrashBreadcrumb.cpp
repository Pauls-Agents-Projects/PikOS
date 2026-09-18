#include "CrashBreadcrumb.h"

#include <cstring>
#include <esp_attr.h>
#include <Preferences.h>

namespace pixelos {

namespace {

constexpr uint32_t kMagic = 0x50494B42UL;  // "PIKB"
constexpr uint16_t kVersion = 1;
constexpr uint32_t kHistoryMagic = 0x50494B48UL;  // "PIKH"
constexpr uint16_t kHistoryVersion = 1;
constexpr uint8_t kHistoryCapacity = 4;
constexpr char kHistoryNamespace[] = "pikos-crash";
constexpr char kHistoryKey[] = "journal";

struct RetainedRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t bytes;
  uint32_t sequence;
  uint32_t uptimeMs;
  uint8_t context;
  uint8_t provider;
  uint8_t reserved[2];
  char app[12];
  uint32_t checksum;
};

struct HistoryEntry {
  uint32_t sequence;
  uint32_t uptimeMs;
  uint8_t resetReason;
  uint8_t context;
  uint8_t provider;
  uint8_t reserved;
  char app[12];
};

struct HistoryRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t count;
  uint8_t next;
  HistoryEntry entries[kHistoryCapacity];
  uint32_t checksum;
};

static_assert(sizeof(RetainedRecord) == 36,
              "RTC breadcrumb size changed unexpectedly");

RTC_NOINIT_ATTR RetainedRecord retainedRecords[2];
CrashBreadcrumbSnapshot previousSnapshot;
HistoryRecord crashHistoryCache{};
bool crashHistoryLoaded = false;
uint32_t nextSequence = 1;
uint8_t nextSlot = 0;

uint32_t checksum(const RetainedRecord& record) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  constexpr size_t kChecksumOffset = offsetof(RetainedRecord, checksum);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < kChecksumOffset; ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t historyChecksum(const HistoryRecord& record) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  constexpr size_t kChecksumOffset = offsetof(HistoryRecord, checksum);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < kChecksumOffset; ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

bool valid(const RetainedRecord& record) {
  return record.magic == kMagic && record.version == kVersion &&
         record.bytes == sizeof(RetainedRecord) &&
         record.context <= static_cast<uint8_t>(CrashContext::FirmwareUpdate) &&
         record.provider <= static_cast<uint8_t>(OnlineProvider::Activity) &&
         record.checksum == checksum(record);
}

bool validHistory(const HistoryRecord& history) {
  if (history.magic != kHistoryMagic || history.version != kHistoryVersion ||
      history.count > kHistoryCapacity || history.next >= kHistoryCapacity ||
      history.checksum != historyChecksum(history)) {
    return false;
  }
  for (uint8_t index = 0; index < history.count; ++index) {
    const HistoryEntry& entry = history.entries[index];
    if (entry.context > static_cast<uint8_t>(CrashContext::FirmwareUpdate) ||
        entry.provider > static_cast<uint8_t>(OnlineProvider::Activity)) {
      return false;
    }
  }
  return true;
}

bool newer(uint32_t left, uint32_t right) {
  return static_cast<int32_t>(left - right) > 0;
}

bool crashRelatedReset(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
         reason == ESP_RST_BROWNOUT;
}

void copyText(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) return;
  if (source == nullptr) source = "unknown";
  strncpy(destination, source, capacity - 1);
  destination[capacity - 1] = '\0';
}

HistoryRecord loadHistory(Preferences& preferences) {
  HistoryRecord history{};
  if (preferences.isKey(kHistoryKey) &&
      preferences.getBytesLength(kHistoryKey) == sizeof(history) &&
      preferences.getBytes(kHistoryKey, &history, sizeof(history)) ==
          sizeof(history) &&
      validHistory(history)) {
    return history;
  }
  history.magic = kHistoryMagic;
  history.version = kHistoryVersion;
  return history;
}

void loadCrashHistoryCache() {
  if (crashHistoryLoaded) return;
  Preferences preferences;
  if (preferences.begin(kHistoryNamespace, false)) {
    crashHistoryCache = loadHistory(preferences);
    preferences.end();
  } else {
    crashHistoryCache = HistoryRecord{};
    crashHistoryCache.magic = kHistoryMagic;
    crashHistoryCache.version = kHistoryVersion;
  }
  crashHistoryLoaded = true;
}

void persistCrashHistory(const CrashBreadcrumbSnapshot& snapshot) {
  if (!snapshot.available || !snapshot.crashRelated) return;
  loadCrashHistoryCache();
  Preferences preferences;
  if (!preferences.begin(kHistoryNamespace, false)) return;
  HistoryRecord& history = crashHistoryCache;
  HistoryEntry& entry = history.entries[history.next];
  entry = HistoryEntry{};
  entry.sequence = snapshot.sequence;
  entry.uptimeMs = snapshot.uptimeMs;
  entry.resetReason = static_cast<uint8_t>(snapshot.resetReason);
  entry.context = static_cast<uint8_t>(snapshot.context);
  for (uint8_t provider = 0;
       provider <= static_cast<uint8_t>(OnlineProvider::Activity);
       ++provider) {
    if (strcmp(snapshot.provider,
               crashProviderName(static_cast<OnlineProvider>(provider))) ==
        0) {
      entry.provider = provider;
      break;
    }
  }
  copyText(entry.app, sizeof(entry.app), snapshot.app);
  history.next = static_cast<uint8_t>((history.next + 1) % kHistoryCapacity);
  history.count = history.count < kHistoryCapacity
                      ? static_cast<uint8_t>(history.count + 1)
                      : kHistoryCapacity;
  history.checksum = historyChecksum(history);
  if (preferences.putBytes(kHistoryKey, &history, sizeof(history)) !=
      sizeof(history)) {
    Serial.println(F("[crash] Could not persist reboot journal"));
  }
  preferences.end();
}

}  // namespace

const char* crashContextName(CrashContext context) {
  switch (context) {
    case CrashContext::Runtime: return "runtime";
    case CrashContext::Sync: return "sync";
    case CrashContext::Web: return "web";
    case CrashContext::Reboot: return "reboot";
    case CrashContext::FirmwareUpdate: return "firmware-update";
    case CrashContext::Startup:
    default: return "startup";
  }
}

const char* crashProviderName(OnlineProvider provider) {
  switch (provider) {
    case OnlineProvider::Rss: return "rss";
    case OnlineProvider::Netatmo: return "netatmo";
    case OnlineProvider::Market: return "market";
    case OnlineProvider::Energy: return "energy";
    case OnlineProvider::Calendar: return "calendar";
    case OnlineProvider::Travel: return "travel";
    case OnlineProvider::Reddit: return "reddit";
    case OnlineProvider::Activity: return "activity";
    case OnlineProvider::None:
    default: return "none";
  }
}

void beginCrashBreadcrumb(esp_reset_reason_t resetReason) {
  previousSnapshot = CrashBreadcrumbSnapshot{};
  previousSnapshot.resetReason = resetReason;

  const bool firstValid = valid(retainedRecords[0]);
  const bool secondValid = valid(retainedRecords[1]);
  int8_t newestSlot = -1;
  if (firstValid && secondValid) {
    newestSlot = newer(retainedRecords[1].sequence,
                       retainedRecords[0].sequence)
                     ? 1
                     : 0;
  } else if (firstValid) {
    newestSlot = 0;
  } else if (secondValid) {
    newestSlot = 1;
  }

  // A true power-on makes stale RTC contents meaningless. Every other reset
  // can use the last checksum-valid record, including an intentional reboot.
  if (newestSlot >= 0 && resetReason != ESP_RST_POWERON) {
    const RetainedRecord& record = retainedRecords[newestSlot];
    previousSnapshot.available = true;
    previousSnapshot.crashRelated = crashRelatedReset(resetReason);
    previousSnapshot.sequence = record.sequence;
    previousSnapshot.uptimeMs = record.uptimeMs;
    previousSnapshot.context = static_cast<CrashContext>(record.context);
    copyText(previousSnapshot.app, sizeof(previousSnapshot.app), record.app);
    copyText(previousSnapshot.provider, sizeof(previousSnapshot.provider),
             crashProviderName(static_cast<OnlineProvider>(record.provider)));
    nextSequence = record.sequence + 1;
    nextSlot = static_cast<uint8_t>(newestSlot ^ 1);
  } else {
    nextSequence = 1;
    nextSlot = 0;
  }

  loadCrashHistoryCache();
  persistCrashHistory(previousSnapshot);
  recordCrashBreadcrumb("startup", OnlineProvider::None,
                        CrashContext::Startup, 0);
}

void recordCrashBreadcrumb(const char* app, OnlineProvider provider,
                           CrashContext context, uint32_t uptimeMs) {
  RetainedRecord record{};
  record.magic = kMagic;
  record.version = kVersion;
  record.bytes = sizeof(RetainedRecord);
  record.sequence = nextSequence++;
  record.uptimeMs = uptimeMs;
  record.context = static_cast<uint8_t>(context);
  record.provider = static_cast<uint8_t>(provider);
  copyText(record.app, sizeof(record.app), app);
  record.checksum = checksum(record);

  // Copy the checksum last. If a panic interrupts this tiny write, validation
  // rejects the partial slot and the alternating previous slot remains valid.
  RetainedRecord& destination = retainedRecords[nextSlot];
  destination.checksum = 0;
  memcpy(&destination, &record, offsetof(RetainedRecord, checksum));
  destination.checksum = record.checksum;
  nextSlot ^= 1;
}

CrashBreadcrumbSnapshot previousCrashBreadcrumb() {
  return previousSnapshot;
}

size_t recentCrashBreadcrumbs(CrashBreadcrumbSnapshot* output,
                              size_t capacity) {
  if (output == nullptr || capacity == 0) return 0;
  loadCrashHistoryCache();
  const HistoryRecord& history = crashHistoryCache;
  const size_t count = capacity < history.count ? capacity : history.count;
  for (size_t index = 0; index < count; ++index) {
    const uint8_t slot = static_cast<uint8_t>(
        (history.next + kHistoryCapacity - 1 - index) % kHistoryCapacity);
    const HistoryEntry& entry = history.entries[slot];
    CrashBreadcrumbSnapshot& snapshot = output[index];
    snapshot = CrashBreadcrumbSnapshot{};
    snapshot.available = true;
    snapshot.crashRelated = true;
    snapshot.resetReason =
        static_cast<esp_reset_reason_t>(entry.resetReason);
    snapshot.sequence = entry.sequence;
    snapshot.uptimeMs = entry.uptimeMs;
    snapshot.context = static_cast<CrashContext>(entry.context);
    copyText(snapshot.app, sizeof(snapshot.app), entry.app);
    copyText(snapshot.provider, sizeof(snapshot.provider),
             crashProviderName(static_cast<OnlineProvider>(entry.provider)));
  }
  return count;
}

}  // namespace pixelos
