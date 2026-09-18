#include "TallinnSiriParser.h"

#include <string.h>

namespace pikos {
namespace {

bool isAsciiDigit(char value) {
  return value >= '0' && value <= '9';
}

bool isAsciiAlpha(char value) {
  return (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z');
}

bool isSafeStop(const char* value) {
  const size_t length = strlen(value);
  if (length == 0 || length > 12) return false;
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if (!isAsciiAlpha(byte) && !isAsciiDigit(byte) && byte != '.' &&
        byte != '_' && byte != '-') {
      return false;
    }
  }
  return true;
}

bool isSafeTransport(const char* value) {
  const size_t length = strlen(value);
  if (length == 0 || length > 16) return false;
  for (size_t index = 0; index < length; ++index) {
    if (!isAsciiAlpha(value[index])) return false;
  }
  return true;
}

bool isSafeRoute(const char* value) {
  const size_t length = strlen(value);
  if (length == 0 || length > 8) return false;
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if (!isAsciiAlpha(byte) && !isAsciiDigit(byte) && byte != '+' &&
        byte != '.' && byte != '_' && byte != '-') {
      return false;
    }
  }
  return true;
}

bool isVersion(const char* value) {
  static const char kPrefix[] = "version";
  if (strncmp(value, kPrefix, sizeof(kPrefix) - 1) != 0) return false;
  const char* cursor = value + sizeof(kPrefix) - 1;
  size_t digits = 0;
  while (*cursor != '\0') {
    if (!isAsciiDigit(*cursor)) return false;
    ++digits;
    ++cursor;
  }
  return digits == 8;
}

enum class UnsignedResult : uint8_t { Ok, Invalid, OutOfRange };

UnsignedResult parseUnsigned(const char* value,
                             uint32_t maximum,
                             uint32_t& result) {
  if (*value == '\0') return UnsignedResult::Invalid;
  uint32_t parsed = 0;
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    if (!isAsciiDigit(*cursor)) return UnsignedResult::Invalid;
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (parsed > (maximum - digit) / 10U) {
      return UnsignedResult::OutOfRange;
    }
    parsed = parsed * 10U + digit;
  }
  result = parsed;
  return UnsignedResult::Ok;
}

uint32_t absoluteDifference(uint32_t left, uint32_t right) {
  return left >= right ? left - right : right - left;
}

int32_t normalizedScheduleOffset(uint32_t expected, uint32_t scheduled) {
  int32_t offset = static_cast<int32_t>(scheduled) -
                   static_cast<int32_t>(expected);
  while (offset <= -43200) offset += 86400;
  while (offset > 43200) offset -= 86400;
  return offset;
}

size_t splitCsv(char* line, char** fields, size_t capacity) {
  if (capacity == 0) return 0;
  size_t count = 1;
  fields[0] = line;
  for (char* cursor = line; *cursor != '\0'; ++cursor) {
    if (*cursor != ',') continue;
    *cursor = '\0';
    if (count < capacity) fields[count] = cursor + 1;
    ++count;
  }
  return count;
}

void copyText(char* destination, size_t capacity, const char* source) {
  const size_t length = strlen(source);
  const size_t copyLength = length < capacity - 1 ? length : capacity - 1;
  memcpy(destination, source, copyLength);
  destination[copyLength] = '\0';
}

}  // namespace

TallinnSiriParser::TallinnSiriParser(RecordCallback callback, void* context)
    : callback_(callback), stopCallback_(nullptr), callbackContext_(context) {
  reset();
}

void TallinnSiriParser::reset() {
  error_ = Error::None;
  bodyBytes_ = 0;
  lineCount_ = 0;
  lineLength_ = 0;
  stopBlockCount_ = 0;
  departureCount_ = 0;
  serverSecondOfDay_ = 0;
  sawAnyByte_ = false;
  sawHeader_ = false;
  sawStop_ = false;
  pendingCarriageReturn_ = false;
  finished_ = false;
  currentStopId_[0] = '\0';
  line_[0] = '\0';
}

bool TallinnSiriParser::feed(uint8_t byte) {
  if (finished_) return fail(Error::InvalidState);
  if (error_ != Error::None) return false;

  sawAnyByte_ = true;
  ++bodyBytes_;
  if (bodyBytes_ > kMaximumBodyBytes) return fail(Error::BodyTooLarge);

  if (pendingCarriageReturn_) {
    if (byte != '\n') return fail(Error::InvalidLineEnding);
    pendingCarriageReturn_ = false;
    return completeLine();
  }

  if (byte == '\r') {
    pendingCarriageReturn_ = true;
    return true;
  }
  if (byte == '\n') return completeLine();
  if (byte < 0x20 || byte == 0x7f) {
    return fail(Error::InvalidControlByte);
  }
  if (lineLength_ >= kMaximumLineBytes) return fail(Error::LineTooLong);
  line_[lineLength_++] = static_cast<char>(byte);
  return true;
}

bool TallinnSiriParser::feed(const uint8_t* data, size_t length) {
  if (data == nullptr && length != 0) return fail(Error::InvalidState);
  for (size_t index = 0; index < length; ++index) {
    if (!feed(data[index])) return false;
  }
  return true;
}

bool TallinnSiriParser::feed(const char* data, size_t length) {
  return feed(reinterpret_cast<const uint8_t*>(data), length);
}

bool TallinnSiriParser::finish() {
  if (finished_) return error_ == Error::None;
  if (error_ != Error::None) {
    finished_ = true;
    return false;
  }
  if (!sawAnyByte_) {
    finished_ = true;
    return fail(Error::EmptyBody);
  }
  if (pendingCarriageReturn_) {
    finished_ = true;
    return fail(Error::InvalidLineEnding);
  }
  if (lineLength_ != 0 && !completeLine()) {
    finished_ = true;
    return false;
  }
  finished_ = true;
  if (!sawHeader_) return fail(Error::InvalidHeader);
  if (!sawStop_) return fail(Error::MissingStop);
  return true;
}

bool TallinnSiriParser::completeLine() {
  ++lineCount_;
  if (lineCount_ > kMaximumLines) return fail(Error::TooManyLines);
  if (lineLength_ == 0) return fail(Error::EmptyLine);

  line_[lineLength_] = '\0';
  char* fields[8] = {};
  const size_t fieldCount = splitCsv(line_, fields, 8);
  bool valid = false;
  if (!sawHeader_) {
    valid = parseHeader(fields, fieldCount);
  } else if (fieldCount > 0 && strcmp(fields[0], "stop") == 0) {
    valid = parseStop(fields, fieldCount);
  } else {
    valid = parseDeparture(fields, fieldCount);
  }
  lineLength_ = 0;
  line_[0] = '\0';
  return valid;
}

bool TallinnSiriParser::parseHeader(char* const* fields, size_t fieldCount) {
  if (fieldCount != 6 || strcmp(fields[0], "Transport") != 0 ||
      strcmp(fields[1], "RouteNum") != 0 ||
      strcmp(fields[2], "ExpectedTimeInSeconds") != 0 ||
      strcmp(fields[3], "ScheduleTimeInSeconds") != 0 ||
      !isVersion(fields[5])) {
    return fail(Error::InvalidHeader);
  }
  const UnsignedResult numeric =
      parseUnsigned(fields[4], 86399, serverSecondOfDay_);
  if (numeric == UnsignedResult::Invalid) return fail(Error::InvalidNumber);
  if (numeric == UnsignedResult::OutOfRange) {
    return fail(Error::NumberOutOfRange);
  }
  sawHeader_ = true;
  return true;
}

bool TallinnSiriParser::parseStop(char* const* fields, size_t fieldCount) {
  if (fieldCount != 2 || !isSafeStop(fields[1])) {
    return fail(Error::InvalidStop);
  }
  copyText(currentStopId_, sizeof(currentStopId_), fields[1]);
  sawStop_ = true;
  ++stopBlockCount_;
  if (stopCallback_ != nullptr) {
    stopCallback_(currentStopId_, callbackContext_);
  }
  return true;
}

bool TallinnSiriParser::parseDeparture(char* const* fields,
                                       size_t fieldCount) {
  if (!sawStop_) return fail(Error::DepartureBeforeStop);
  if (fieldCount != 6 && fieldCount != 7) {
    return fail(Error::InvalidFieldCount);
  }
  if (!isSafeTransport(fields[0])) return fail(Error::InvalidTransport);
  if (!isSafeRoute(fields[1])) return fail(Error::InvalidRoute);

  const size_t destinationLength = strlen(fields[4]);
  if (destinationLength == 0 || destinationLength > 64) {
    return fail(Error::InvalidDestination);
  }

  uint32_t expected = 0;
  uint32_t scheduled = 0;
  uint32_t wait = 0;
  const UnsignedResult expectedResult =
      parseUnsigned(fields[2], 172799, expected);
  const UnsignedResult scheduledResult =
      parseUnsigned(fields[3], 172799, scheduled);
  const UnsignedResult waitResult = parseUnsigned(fields[5], 86400, wait);
  if (expectedResult == UnsignedResult::Invalid ||
      scheduledResult == UnsignedResult::Invalid ||
      waitResult == UnsignedResult::Invalid) {
    return fail(Error::InvalidNumber);
  }
  if (expectedResult == UnsignedResult::OutOfRange ||
      scheduledResult == UnsignedResult::OutOfRange ||
      waitResult == UnsignedResult::OutOfRange) {
    return fail(Error::NumberOutOfRange);
  }

  if (fieldCount == 7) {
    if (fields[6][0] == '\0') {
    } else if (strcmp(fields[6], "Z") == 0) {
    } else {
      return fail(Error::InvalidFlag);
    }
  }

  const uint32_t expectedOffset =
      ((expected % 86400U) + 86400U - serverSecondOfDay_) % 86400U;
  const uint32_t discrepancy =
      absoluteDifference(wait, expectedOffset) <
              absoluteDifference(wait, expectedOffset + 86400U)
          ? absoluteDifference(wait, expectedOffset)
          : absoluteDifference(wait, expectedOffset + 86400U);
  if (discrepancy > 120U) return fail(Error::InconsistentWait);

  TallinnSiriDeparture record = {};
  copyText(record.stopId, sizeof(record.stopId), currentStopId_);
  copyText(record.transport, sizeof(record.transport), fields[0]);
  copyText(record.route, sizeof(record.route), fields[1]);
  copyText(record.destination, sizeof(record.destination), fields[4]);
  record.serverSecondOfDay = serverSecondOfDay_;
  record.expectedSecondOfDay = expected;
  record.scheduledSecondOfDay = scheduled;
  record.expectedOffsetSeconds = wait;
  record.scheduledOffsetSeconds =
      normalizedScheduleOffset(expected, scheduled);
  record.realtime = expected != scheduled;

  ++departureCount_;
  if (callback_ != nullptr) callback_(record, callbackContext_);
  return true;
}

bool TallinnSiriParser::fail(Error error) {
  if (error_ == Error::None) error_ = error;
  return false;
}

const char* TallinnSiriParser::errorMessage() const {
  switch (error_) {
    case Error::None:
      return "ok";
    case Error::InvalidState:
      return "invalid parser state";
    case Error::EmptyBody:
      return "empty body";
    case Error::BodyTooLarge:
      return "body too large";
    case Error::TooManyLines:
      return "too many lines";
    case Error::LineTooLong:
      return "line too long";
    case Error::InvalidControlByte:
      return "invalid control byte";
    case Error::InvalidLineEnding:
      return "invalid line ending";
    case Error::EmptyLine:
      return "empty line";
    case Error::InvalidHeader:
      return "invalid header";
    case Error::InvalidStop:
      return "invalid stop";
    case Error::DepartureBeforeStop:
      return "departure before stop";
    case Error::InvalidFieldCount:
      return "invalid field count";
    case Error::InvalidTransport:
      return "invalid transport";
    case Error::InvalidRoute:
      return "invalid route";
    case Error::InvalidDestination:
      return "invalid destination";
    case Error::InvalidNumber:
      return "invalid number";
    case Error::NumberOutOfRange:
      return "number out of range";
    case Error::InconsistentWait:
      return "wait does not match expected time";
    case Error::InvalidFlag:
      return "invalid departure flag";
    case Error::MissingStop:
      return "missing stop block";
  }
  return "unknown parser error";
}

}  // namespace pikos
