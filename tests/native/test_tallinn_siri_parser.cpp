#include "TallinnSiriParser.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>

using pikos::TallinnSiriDeparture;
using pikos::TallinnSiriParser;

namespace {

const char kHeader[] =
    "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,46548,"
    "version20201024\n";

void collect(const TallinnSiriDeparture& record, void* context) {
  static_cast<std::vector<TallinnSiriDeparture>*>(context)->push_back(record);
}

struct CollectedResponse {
  std::vector<TallinnSiriDeparture> departures;
  std::vector<std::string> stops;
};

void collectResponseDeparture(const TallinnSiriDeparture& record,
                              void* context) {
  static_cast<CollectedResponse*>(context)->departures.push_back(record);
}

void collectStop(const char* stopId, void* context) {
  static_cast<CollectedResponse*>(context)->stops.push_back(stopId);
}

TallinnSiriParser::Error parseError(const std::string& body) {
  TallinnSiriParser parser;
  parser.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  parser.finish();
  return parser.error();
}

void testIncrementalRealSample() {
  const std::string body =
      std::string(kHeader) + "stop,1234\n" +
      "tram,T2,46962,46932,Kopli,414,Z\n"
      "tram,T1,47236,47226,Kopli,688,Z\n"
      "tram,T5,47526,47526,Kopli,978,";
  std::vector<TallinnSiriDeparture> records;
  TallinnSiriParser parser(collect, &records);
  for (size_t index = 0; index < body.size(); ++index) {
    assert(parser.feed(static_cast<uint8_t>(body[index])));
  }
  assert(parser.finish());
  assert(parser.finish());
  assert(parser.bodyBytes() == body.size());
  assert(parser.lineCount() == 5);
  assert(parser.stopBlockCount() == 1);
  assert(parser.departureCount() == 3);
  assert(records.size() == 3);
  assert(strcmp(records[0].stopId, "1234") == 0);
  assert(strcmp(records[0].transport, "tram") == 0);
  assert(strcmp(records[0].route, "T2") == 0);
  assert(strcmp(records[0].destination, "Kopli") == 0);
  assert(records[0].serverSecondOfDay == 46548);
  assert(records[0].expectedSecondOfDay == 46962);
  assert(records[0].scheduledSecondOfDay == 46932);
  assert(records[0].expectedOffsetSeconds == 414);
  assert(records[0].scheduledOffsetSeconds == -30);
  assert(records[0].realtime);
  assert(!records[2].realtime);
}

void testMultipleStopsAndCrLf() {
  std::string body(kHeader);
  size_t position = 0;
  while ((position = body.find('\n', position)) != std::string::npos) {
    body.replace(position, 1, "\r\n");
    position += 2;
  }
  body += "stop,A-1\r\nbus,8,46608,46608,Kesklinn,60,\r\n";
  body += "stop,B_2\r\ntram,T2,46668,46658,Kopli,120,Z\r\n";

  std::vector<TallinnSiriDeparture> records;
  TallinnSiriParser parser(collect, &records);
  size_t cursor = 0;
  const size_t chunks[] = {1, 2, 7, 3, 11};
  size_t chunkIndex = 0;
  while (cursor < body.size()) {
    size_t length = chunks[chunkIndex++ % 5];
    if (cursor + length > body.size()) length = body.size() - cursor;
    assert(parser.feed(body.data() + cursor, length));
    cursor += length;
  }
  assert(parser.finish());
  assert(records.size() == 2);
  assert(strcmp(records[0].stopId, "A-1") == 0);
  assert(strcmp(records[1].stopId, "B_2") == 0);
  assert(parser.stopBlockCount() == 2);
}

void testMidnightOffsetsAndUtf8Destination() {
  const std::string body =
      "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,86370,"
      "version20201024\n"
      "stop,TEST\n"
      "bus,1,30,86390,P\xC3\xA4rnu mnt,60,Z\n";
  std::vector<TallinnSiriDeparture> records;
  TallinnSiriParser parser(collect, &records);
  assert(parser.feed(body.data(), body.size()));
  assert(parser.finish());
  assert(records.size() == 1);
  assert(records[0].expectedOffsetSeconds == 60);
  assert(records[0].scheduledOffsetSeconds == -40);
  assert(strcmp(records[0].destination, "P\xC3\xA4rnu mnt") == 0);
}

void testRealtimeComesFromPredictionNotAccessibilityFlag() {
  const std::string body =
      std::string(kHeader) + "stop,TEST\n" +
      "bus,1,46608,46608,Accessible,60,Z\n"
      "bus,2,46668,46658,Predicted,120,\n";
  std::vector<TallinnSiriDeparture> records;
  TallinnSiriParser parser(collect, &records);
  assert(parser.feed(body.data(), body.size()));
  assert(parser.finish());
  assert(records.size() == 2);
  assert(!records[0].realtime);
  assert(records[1].realtime);
}

void testStopCallbacksIncludeEmptyBlocks() {
  const std::string body =
      std::string(kHeader) + "stop,1234\n" +
      "tram,T2,46608,46608,Kopli,60,\n"
      "stop,1235\n";
  CollectedResponse response;
  TallinnSiriParser parser(collectResponseDeparture, &response);
  parser.setStopCallback(collectStop);
  assert(parser.feed(body.data(), body.size()));
  assert(parser.finish());
  assert(response.departures.size() == 1);
  assert(response.stops.size() == 2);
  assert(response.stops[0] == "1234");
  assert(response.stops[1] == "1235");
}

void testEnvelopeAndGrammarFailures() {
  assert(parseError("") == TallinnSiriParser::Error::EmptyBody);
  assert(parseError("<html>error</html>") ==
         TallinnSiriParser::Error::InvalidHeader);
  assert(parseError(std::string(kHeader)) ==
         TallinnSiriParser::Error::MissingStop);
  assert(parseError(std::string(kHeader) + "\n") ==
         TallinnSiriParser::Error::EmptyLine);
  assert(parseError(std::string(kHeader) + "bus,1,46608,46608,X,60,\n") ==
         TallinnSiriParser::Error::DepartureBeforeStop);
  assert(parseError(std::string(kHeader) + "stop,bad stop\n") ==
         TallinnSiriParser::Error::InvalidStop);
  assert(parseError(std::string(kHeader) + "stop,TEST\nbu5,1,46608,46608,X,60,\n") ==
         TallinnSiriParser::Error::InvalidTransport);
  assert(parseError(std::string(kHeader) + "stop,TEST\nbus,bad route,46608,46608,X,60,\n") ==
         TallinnSiriParser::Error::InvalidRoute);
  assert(parseError(std::string(kHeader) + "stop,TEST\nbus,1,no,46608,X,60,\n") ==
         TallinnSiriParser::Error::InvalidNumber);
  assert(parseError(std::string(kHeader) + "stop,TEST\nbus,1,46608,46608,X,181,\n") ==
         TallinnSiriParser::Error::InconsistentWait);
  assert(parseError(std::string(kHeader) + "stop,TEST\nbus,1,46608,46608,X,60,Q\n") ==
         TallinnSiriParser::Error::InvalidFlag);
  assert(parseError(std::string(kHeader) + "stop,TEST\nbus,1,46608,46608,X,60,,extra\n") ==
         TallinnSiriParser::Error::InvalidFieldCount);
}

void testControlLineAndNumberBounds() {
  std::string nulBody = std::string(kHeader) + "stop,TEST\n";
  nulBody.push_back('\0');
  assert(parseError(nulBody) ==
         TallinnSiriParser::Error::InvalidControlByte);

  const std::string bareCarriageReturn = std::string(kHeader) + "stop,TEST\rX";
  assert(parseError(bareCarriageReturn) ==
         TallinnSiriParser::Error::InvalidLineEnding);

  const std::string longDestination(145, 'X');
  assert(parseError(std::string(kHeader) + "stop,TEST\nbus,1,46608,46608," +
                    longDestination + ",60,\n") ==
         TallinnSiriParser::Error::LineTooLong);

  assert(parseError(
             "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,"
             "86400,version20201024\nstop,TEST\n") ==
         TallinnSiriParser::Error::NumberOutOfRange);
  assert(parseError(std::string(kHeader) +
                    "stop,TEST\nbus,1,172800,46608,X,60,\n") ==
         TallinnSiriParser::Error::NumberOutOfRange);
}

void testLineAndBodyCaps() {
  std::string tooManyLines =
      "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,100,"
      "version20201024\nstop,TEST\n";
  for (size_t index = 0; index < 511; ++index) {
    tooManyLines += "bus,1,100,100,X,0,\n";
  }
  assert(tooManyLines.size() < TallinnSiriParser::kMaximumBodyBytes);
  assert(parseError(tooManyLines) == TallinnSiriParser::Error::TooManyLines);

  std::string tooLarge =
      "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,100,"
      "version20201024\nstop,TEST\n";
  // Keep each row below the line cap but above the average size needed to hit
  // the body cap before the independent 512-line cap.
  const std::string destination(55, 'X');
  const std::string row = "bus,1,100,100," + destination + ",0,\n";
  while (tooLarge.size() <= TallinnSiriParser::kMaximumBodyBytes) {
    tooLarge += row;
  }
  assert(parseError(tooLarge) == TallinnSiriParser::Error::BodyTooLarge);
}

void testResetReusesStorageAndCallback() {
  const std::string first = std::string(kHeader) + "stop,A\n";
  const std::string second = std::string(kHeader) +
                             "stop,B\nbus,2,46608,46608,Center,60,\n";
  std::vector<TallinnSiriDeparture> records;
  TallinnSiriParser parser(collect, &records);
  assert(parser.feed(first.data(), first.size()));
  assert(parser.finish());
  parser.reset();
  assert(parser.feed(second.data(), second.size()));
  assert(parser.finish());
  assert(records.size() == 1);
  assert(strcmp(records[0].stopId, "B") == 0);
}

}  // namespace

int main() {
  testIncrementalRealSample();
  testMultipleStopsAndCrLf();
  testMidnightOffsetsAndUtf8Destination();
  testRealtimeComesFromPredictionNotAccessibilityFlag();
  testStopCallbacksIncludeEmptyBlocks();
  testEnvelopeAndGrammarFailures();
  testControlLineAndNumberBounds();
  testLineAndBodyCaps();
  testResetReusesStorageAndCallback();
  std::cout << "TallinnSiriParser native tests passed\n";
  return 0;
}
