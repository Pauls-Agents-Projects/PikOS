#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pikos {

// A validated departure row from Tallinn's siri-stop-departures.php response.
// String storage belongs to this value and remains valid for the duration of
// the callback. The callback must copy records it wants to retain.
struct TallinnSiriDeparture {
  char stopId[13];
  char transport[17];
  char route[9];
  char destination[65];
  uint32_t serverSecondOfDay;
  uint32_t expectedSecondOfDay;
  uint32_t scheduledSecondOfDay;
  uint32_t expectedOffsetSeconds;
  int32_t scheduledOffsetSeconds;
  // `Z` is an accessibility/low-floor marker in the official client, not a
  // prediction flag. Realtime is true only when expected and scheduled times
  // differ.
  bool realtime;
};

class TallinnSiriParser {
 public:
  static constexpr size_t kMaximumBodyBytes = 32768;
  static constexpr size_t kMaximumLines = 512;
  static constexpr size_t kMaximumLineBytes = 160;

  enum class Error : uint8_t {
    None = 0,
    InvalidState,
    EmptyBody,
    BodyTooLarge,
    TooManyLines,
    LineTooLong,
    InvalidControlByte,
    InvalidLineEnding,
    EmptyLine,
    InvalidHeader,
    InvalidStop,
    DepartureBeforeStop,
    InvalidFieldCount,
    InvalidTransport,
    InvalidRoute,
    InvalidDestination,
    InvalidNumber,
    NumberOutOfRange,
    InconsistentWait,
    InvalidFlag,
    MissingStop,
  };

  typedef void (*RecordCallback)(const TallinnSiriDeparture& record,
                                 void* context);
  // Called once for every syntactically valid `stop,<SIRI ID>` block, even
  // when that block has no departures. This lets the provider distinguish a
  // valid quiet stop from an internal/non-SIRI stop code that the endpoint
  // silently ignores.
  typedef void (*StopCallback)(const char* stopId, void* context);

  explicit TallinnSiriParser(RecordCallback callback = nullptr,
                             void* context = nullptr);

  void setStopCallback(StopCallback callback) { stopCallback_ = callback; }

  // Reuses the parser without allocating. The callback and context are kept.
  void reset();

  bool feed(uint8_t byte);
  bool feed(const uint8_t* data, size_t length);
  bool feed(const char* data, size_t length);

  // Completes a final unterminated line and validates the response envelope.
  // Calls are idempotent after a successful finish.
  bool finish();

  Error error() const { return error_; }
  const char* errorMessage() const;
  size_t bodyBytes() const { return bodyBytes_; }
  size_t lineCount() const { return lineCount_; }
  size_t stopBlockCount() const { return stopBlockCount_; }
  size_t departureCount() const { return departureCount_; }
  bool finished() const { return finished_; }

 private:
  bool completeLine();
  bool parseHeader(char* const* fields, size_t fieldCount);
  bool parseStop(char* const* fields, size_t fieldCount);
  bool parseDeparture(char* const* fields, size_t fieldCount);
  bool fail(Error error);

  RecordCallback callback_;
  StopCallback stopCallback_;
  void* callbackContext_;
  Error error_;
  size_t bodyBytes_;
  size_t lineCount_;
  size_t lineLength_;
  size_t stopBlockCount_;
  size_t departureCount_;
  uint32_t serverSecondOfDay_;
  bool sawAnyByte_;
  bool sawHeader_;
  bool sawStop_;
  bool pendingCarriageReturn_;
  bool finished_;
  char currentStopId_[13];
  char line_[kMaximumLineBytes + 1];
};

}  // namespace pikos
