#pragma once

#include <Arduino.h>

namespace pixelos {

// Keep one mbedTLS-sized block out of the general allocator while idle. A
// scoped lease temporarily returns it for one outbound provider operation and
// reserves it again after all provider-local response buffers are destroyed.
void beginTlsHeapGuard();

// Rebuild the largest possible reserve after a provider's temporary config,
// parser and response objects have all left scope. This prevents one smaller
// fallback block from becoming the permanent limit for later handshakes.
void restoreTlsHeapGuard();

// Borrow the private arena for large non-TLS scratch buffers while a
// ScopedTlsHeapLease is held. This is used by the atomic settings writer,
// which never runs concurrently with an outbound TLS operation.
void* allocateTlsScratch(size_t bytes);
void freeTlsScratch(void* pointer);
uint32_t tlsLeaseContentionCount();
uint32_t tlsMaximumLeaseWaitMs();

class ScopedTlsHeapLease {
 public:
  ScopedTlsHeapLease();
  ~ScopedTlsHeapLease();

 ScopedTlsHeapLease(const ScopedTlsHeapLease&) = delete;
  ScopedTlsHeapLease& operator=(const ScopedTlsHeapLease&) = delete;

 private:
  bool locked_ = false;
};

}  // namespace pixelos
