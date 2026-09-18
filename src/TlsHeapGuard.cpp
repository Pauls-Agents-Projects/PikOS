#include "TlsHeapGuard.h"

#include <cstdlib>
#include <cstring>
#include <mbedtls/platform.h>
#include <multi_heap.h>

namespace pixelos {
namespace {

// Keep mbedTLS out of the general ESP32 heap entirely. The previous movable
// reserve worked only while the inbound web listener was closed: a browser
// socket accepted during a handshake could occupy the released block and
// split it before BIGNUM requested its large contiguous allocation. A private
// multi_heap arena keeps provider handshakes deterministic. The inbound
// listener is briefly closed during each bounded outbound job so lwIP cannot
// accumulate stale browser sockets at the same time.
// Calendar and Netatmo handshakes peaked at 52.7 KiB in a six-provider live
// sync. The optimized build recovered over 35 KiB of static RAM, so spend a
// small part of it on deterministic TLS headroom instead of falling back into
// the general heap when a certificate chain changes.
constexpr size_t kPreferredTlsArenaBytes = 60U * 1024U;
uint8_t* tlsArena = nullptr;
size_t tlsArenaBytes = 0;
multi_heap_handle_t tlsHeap = nullptr;
StaticSemaphore_t leaseMutexStorage;
SemaphoreHandle_t leaseMutex = nullptr;
volatile size_t fallbackAllocations = 0;
volatile uint32_t leaseContentions = 0;
volatile uint32_t maximumLeaseWaitMs = 0;
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;
size_t lastMinimumFree = SIZE_MAX;

bool inTlsArena(const void* pointer) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  const uintptr_t first = reinterpret_cast<uintptr_t>(tlsArena);
  return tlsArena != nullptr && address >= first &&
         address < first + tlsArenaBytes;
}

void* tlsCalloc(size_t count, size_t size) {
  if (count != 0 && size > SIZE_MAX / count) return nullptr;
  const size_t bytes = count * size;
  void* result =
      tlsHeap == nullptr ? nullptr : multi_heap_malloc(tlsHeap, bytes);
  if (result == nullptr) {
    // The arena is sized for the complete ESP32 handshake. Keep a standards-
    // compliant fallback for unusually large certificates and make it
    // visible in diagnostics instead of failing a provider outright.
    result = calloc(count, size);
    if (result != nullptr) ++fallbackAllocations;
    return result;
  }
  memset(result, 0, bytes);
  return result;
}

void tlsFree(void* pointer) {
  if (pointer == nullptr) return;
  if (inTlsArena(pointer) && tlsHeap != nullptr) {
    multi_heap_free(tlsHeap, pointer);
  } else {
    free(pointer);
  }
}

}  // namespace

void beginTlsHeapGuard() {
  if (leaseMutex == nullptr) {
    leaseMutex = xSemaphoreCreateRecursiveMutexStatic(&leaseMutexStorage);
  }
  if (tlsArena == nullptr) {
    constexpr size_t candidates[] = {
        kPreferredTlsArenaBytes, 56U * 1024U, 52U * 1024U,
        48U * 1024U, 44U * 1024U, 40U * 1024U};
    for (const size_t bytes : candidates) {
      tlsArena = static_cast<uint8_t*>(malloc(bytes));
      if (tlsArena != nullptr) {
        tlsArenaBytes = bytes;
        break;
      }
    }
  }
  if (tlsHeap == nullptr && tlsArena != nullptr) {
    tlsHeap = multi_heap_register(tlsArena, tlsArenaBytes);
  }
  if (tlsHeap == nullptr) {
    Serial.println(F("[tls] Could not initialise private TLS arena"));
    return;
  }
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  multi_heap_info_t info{};
  multi_heap_get_info(tlsHeap, &info);
  Serial.printf("[tls] Private arena ready (%u bytes usable)\n",
                static_cast<unsigned>(info.total_free_bytes));
}

void restoreTlsHeapGuard() {
  if (tlsHeap == nullptr) return;
  multi_heap_info_t info{};
  multi_heap_get_info(tlsHeap, &info);
  if (info.minimum_free_bytes != lastMinimumFree) {
    lastMinimumFree = info.minimum_free_bytes;
    Serial.printf("[tls] arena peak=%u free-min=%u\n",
                  static_cast<unsigned>(tlsArenaBytes -
                                        info.minimum_free_bytes),
                  static_cast<unsigned>(info.minimum_free_bytes));
  }
  if (info.total_allocated_bytes != 0 || fallbackAllocations != 0) {
    Serial.printf(
        "[tls] arena allocated=%u free=%u largest=%u fallback=%u\n",
        static_cast<unsigned>(info.total_allocated_bytes),
        static_cast<unsigned>(info.total_free_bytes),
        static_cast<unsigned>(info.largest_free_block),
        static_cast<unsigned>(fallbackAllocations));
  }
}

void* allocateTlsScratch(size_t bytes) {
  void* result =
      tlsHeap == nullptr ? nullptr : multi_heap_malloc(tlsHeap, bytes);
  return result != nullptr ? result : malloc(bytes);
}

void freeTlsScratch(void* pointer) {
  tlsFree(pointer);
}

uint32_t tlsLeaseContentionCount() { return leaseContentions; }

uint32_t tlsMaximumLeaseWaitMs() { return maximumLeaseWaitMs; }

ScopedTlsHeapLease::ScopedTlsHeapLease() {
  // Outbound providers and icon downloads share the one private TLS arena.
  // Settings saves also take this mutex so their large JSON copies never run
  // concurrently with provider result parsing on the second core.
  if (leaseMutex != nullptr) {
    const uint32_t startedAt = millis();
    locked_ = xSemaphoreTakeRecursive(leaseMutex, portMAX_DELAY) == pdTRUE;
    const uint32_t waitedMs = millis() - startedAt;
    if (waitedMs > 1) {
      portENTER_CRITICAL(&telemetryMux);
      ++leaseContentions;
      if (waitedMs > maximumLeaseWaitMs) maximumLeaseWaitMs = waitedMs;
      portEXIT_CRITICAL(&telemetryMux);
    }
  }
}

ScopedTlsHeapLease::~ScopedTlsHeapLease() {
  if (locked_ && leaseMutex != nullptr) {
    xSemaphoreGiveRecursive(leaseMutex);
  }
  // The online worker deliberately runs at idle priority. TLS and response
  // parsing can otherwise hand control back to the same task repeatedly and
  // prevent IDLE0 from feeding the task watchdog during a multi-feed job.
  taskYIELD();
}

}  // namespace pixelos
