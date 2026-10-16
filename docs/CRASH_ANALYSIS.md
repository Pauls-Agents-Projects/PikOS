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
