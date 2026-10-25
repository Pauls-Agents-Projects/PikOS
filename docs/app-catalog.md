# App catalog direction

PikOS apps are currently native C++ modules linked into one OTA image. Moving
those binaries to GitHub and loading them at run time would not free space in a
safe or reliable way on the TC001:

- the ESP32 cannot dynamically link an arbitrary downloaded C++ app;
- adding a Lua or WebAssembly runtime would consume a meaningful part of the
  flash and RAM that the catalog is intended to save;
- a native downloader would turn a compromised repository or manifest into
  device-level code execution;
- the 4 MB flash is already divided between two OTA slots, persistent data and
  the crash-dump partition. An installed native app would still need its own
  executable storage and a compatible ABI after every OS update.

## Recommended model

A later PikOS catalog should use **declarative apps**, not downloaded native
code. A catalog entry may describe:

- a JSON/HTTP data source and bounded extraction rules;
- one or more 8×8 icon IDs;
- short value, unit, colour-band and chart presentation rules;
- a refresh class handled by the existing central Sync app;
- localized labels and a settings schema made from existing safe controls.

The firmware remains responsible for networking, TLS, parsing limits,
credentials, animation and rendering. Catalog content can therefore add a
simple public-data app without gaining access to memory, GPIO, stored secrets
or other providers.

## Distribution and safety

The device should download a small versioned index from a PikOS GitHub Release
only during central Sync. Every pack should include its size and SHA-256 digest
and be verified against a release manifest signed by a public key embedded in
the firmware. Downloads must be atomic, size limited and cached in SPIFFS; the
last verified copy remains usable offline. Catalog updates must never run in
the display task.

Provider integrations that need OAuth, custom parsers or binary protocols stay
native. If firmware size becomes the limiting factor, PikOS can publish a few
signed build profiles—Core, Home and Connected—rather than introducing an
unsafe plugin ABI. OTA rollback then continues to work exactly as it does now.

## Release-candidate decision

The catalog is deliberately not enabled in the current release candidate.
The first useful milestone is a read-only prototype with one public JSON app,
a storage budget and malformed-manifest tests. Stabilising DNS, rendering and
crash recovery takes priority over adding another remote execution surface.
