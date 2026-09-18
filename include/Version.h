#pragma once

namespace pixelos {

// Keep the release label and outbound HTTP identity in one place. Provider
// clients, the status API, the boot log, and packaged artifact names must all
// describe the same firmware build.
static constexpr char kFirmwareVersion[] = "0.22.0-rc6";
static constexpr char kUserAgent[] = "Ulanzi-PikOS/0.22.0-rc6";

}  // namespace pixelos
