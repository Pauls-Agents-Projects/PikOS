#include "GarminTokenParser.h"

#include <assert.h>

#include <iostream>
#include <string>

namespace {

void testLargeTokenResponseIsDecodedInPlace() {
  const std::string access(5000, 'A');
  const std::string refresh(2500, 'R');
  const std::string ignored(400, 'X');
  std::string payload = "{\"token_type\":\"bearer\",\"access_token\":\"" +
                        access + "\",\"scope\":\"" + ignored +
                        "\",\"refresh_token\":\"" + refresh +
                        "\",\"expires_in\":3600}";

  const char* parsedAccess = nullptr;
  const char* parsedRefresh = nullptr;
  const char* error = nullptr;
  assert(pixelos::parseGarminTokenJson(
      &payload[0], payload.size(), parsedAccess, parsedRefresh, error));
  assert(error == nullptr);
  assert(parsedAccess != nullptr);
  assert(parsedRefresh != nullptr);
  assert(std::string(parsedAccess) == access);
  assert(std::string(parsedRefresh) == refresh);
  assert(parsedAccess >= payload.data());
  assert(parsedAccess < payload.data() + payload.size());
  assert(parsedRefresh >= payload.data());
  assert(parsedRefresh < payload.data() + payload.size());
}

void testMalformedResponseFailsClosed() {
  std::string payload = "{\"access_token\":\"unfinished";
  const char* access = nullptr;
  const char* refresh = nullptr;
  const char* error = nullptr;
  assert(!pixelos::parseGarminTokenJson(
      &payload[0], payload.size(), access, refresh, error));
  assert(error != nullptr);
  assert(access == nullptr);
  assert(refresh == nullptr);
}

}  // namespace

int main() {
  testLargeTokenResponseIsDecodedInPlace();
  testMalformedResponseFailsClosed();
  std::cout << "Garmin token parser tests passed\n";
  return 0;
}
