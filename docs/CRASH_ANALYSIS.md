# Crash analysis and containment

## Evidence collected on 2026-10-16

The four-entry NVS journal from the development TC001 contained four
`ESP_RST_PANIC` resets. Every panic happened in the background Sync context;
none happened in the display task or while rendering an app:

| Recency | Provider near the panic | Uptime |
|---:|---|---:|
| 1 | RSS | 66 h 10 min |
| 2 | Travel | 27 h 52 min |
| 3 | Market | 55 h 52 min |
| 4 | RSS | 43 h 13 min |

The failures span three providers and occur only after long uptimes. The
breadcrumb proves that the shared online worker is the affected boundary, but
does not contain the CPU program counter needed to identify a source line.

## Release hardening

- A provider implicated in a Sync panic is quarantined for the next boot.
  Cached data remains available and all other apps/providers continue.
- A provider that returns ordinary errors three times consecutively is also
  quarantined for that boot. This prevents a broken upstream service from
  monopolising every later Sync cycle.
- The partition table now reserves 128 KiB for ESP-IDF's enabled ELF core-dump
  writer. The SPIFFS offset and size remain unchanged. Installing this partition
  layout requires one serial/full-image flash; an application-only OTA cannot
  rewrite the partition table.

ESP32 applications share one address space, so a CPU panic cannot safely be
caught and restarted like a desktop process. The quarantine is therefore a
circuit breaker after the hardware reset, while the core dump supplies the
exact stack trace needed to remove the underlying defect.

Never commit or attach a complete NVS, full-flash, or raw core dump: task
stacks can contain fragments of Wi-Fi or provider credentials. Decode the core
dump locally against the matching `firmware.elf`, then share only the redacted
backtrace and crash summary.

## Core dump collected on 2026-10-25

The first dump from the new partition matched firmware commit `c94b916` and
contained twelve tasks. The faulting TCP/IP task stopped at
`udp_sendto+31` with a DNS UDP control-block pointer of `0x1`. At the same
instant, PikOS's `online-data` worker was inside `WiFiClientSecure::connect`,
resolving Yahoo for `DataServices::fetchMarket()`.

The failure was below the market parser: Arduino-ESP32 2.x `hostByName()` calls
lwIP's raw `dns_gethostbyname()` from the caller's task, while lwIP's DNS retry
timer owns the same resolver state on the TCP/IP task. PikOS made that race
repeatable by deliberately running every provider on its own core-0 worker.
The previously varied RSS, Travel and Market breadcrumbs are consistent with
this shared DNS boundary rather than three independent app crashes.

PikOS now resolves every outbound TLS hostname through `getaddrinfo()`. Its
thread-safe netconn path serializes resolver work onto lwIP's TCP/IP task. The
resulting IP address is passed to mbedTLS together with the original hostname,
so certificate validation and SNI are unchanged. Both provider and LaMetric
icon clients use this path.

Only the redacted stacks and addresses above were retained. The raw dump was
kept outside the repository for local decoding and removed after verification.
