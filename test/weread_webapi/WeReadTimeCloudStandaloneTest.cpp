#include "WeReadTimeCloud.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using WeReadTimeCloud::Response;

bool parse(Response& response, const std::string& json, size_t chunk = 1) {
  for (size_t offset = 0; offset < json.size(); offset += chunk) {
    if (!response.feed(reinterpret_cast<const uint8_t*>(json.data() + offset),
                       std::min(chunk, json.size() - offset))) return false;
  }
  return response.complete();
}

int main(int argc, char** argv) {
  Response response;
  for (size_t chunk : {1U, 7U, 4096U}) {
    response.reset(Response::Mode::Key);
    assert(parse(response, R"({"apikey":"wrk-test_only","errcode":0})", chunk));
    assert(strcmp(response.key(), "wrk-test_only") == 0);
    response.reset(Response::Mode::Stats, 1788192000, 1789401600);
    assert(parse(response, R"({"baseTime":1788192000,"totalReadTime":16234,"readTimes":{"1789401600":2947},"ignored":[{"baseTime":0}]})", chunk));
    assert(response.snapshot().monthSeconds == 16234);
    assert(response.snapshot().daySeconds == 2947 && response.snapshot().hasDay);
  }
  for (const char* json : {
           R"({"baseTime":0,"totalReadTime":1,"readTimes":{}})",
           R"({"baseTime":1788192000,"totalReadTime":null,"readTimes":{}})",
           R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":[]})",
           R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":{"1789401600":null}})",
           R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":{"1789401600":2}})",
           R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":{"1789401600":1,"1789401600":1}})",
           R"({"baseTime":1788192000,"totalReadTime":1,"totalReadTime":1,"readTimes":{}})",
           R"({"baseTime":1788192000,"totalReadTime":18446744073709551616,"readTimes":{}})",
           R"({"baseTime":1788192000,"totalReadTime":1.0,"readTimes":{}})",
           R"({"baseTime":1788192000,"totalReadTime":-1,"readTimes":{}})",
           R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":{},"errcode":-1})",
           R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":{},"upgrade_info":null})",
           R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":{})",
           R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":{}}{})",
           R"({"data":{"baseTime":1788192000,"totalReadTime":1,"readTimes":{}}})"}) {
    response.reset(Response::Mode::Stats, 1788192000, 1789401600);
    assert(!parse(response, json));
  }
  response.reset(Response::Mode::Stats, 1788192000, 1789401600);
  assert(parse(response, R"({"baseTime":1788192000,"totalReadTime":1,"readTimes":{}})"));
  assert(!response.snapshot().hasDay);  // Missing != explicit zero.
  for (const char* json : {R"({"apikey":"wrk-a","apikey":"wrk-b"})",
                           R"({"apikey":"wrk-evil\r\n"})", R"({"apikey":null})",
                           R"({"apikey":"wrk-a","errcode":1})"}) {
    response.reset(Response::Mode::Key);
    assert(!parse(response, json));
  }
  response.reset(Response::Mode::Key);
  assert(!parse(response, "{\"apikey\":\"wrk-" + std::string(600, 'x') + "\"}"));
  if (argc == 2) {
    // Optional private, read-only 2026-09-15 monthly API capture. Never store
    // credentials in a fixture or print arbitrary API fields from it.
    std::ifstream input(argv[1], std::ios::binary);
    assert(input.good());
    response.reset(Response::Mode::Stats, 1788192000, 1789401600);
    char chunk[257];
    size_t bytes = 0;
    while (input.read(chunk, sizeof(chunk)) || input.gcount()) {
      bytes += static_cast<size_t>(input.gcount());
      assert(bytes <= 1024 * 1024);
      assert(response.feed(reinterpret_cast<const uint8_t*>(chunk), static_cast<size_t>(input.gcount())));
    }
    assert(input.eof() && response.complete());
    std::cout << "Live response decoded: bytes=" << bytes << " month=" << response.snapshot().monthSeconds
              << " today=" << response.snapshot().daySeconds << " hasDay=" << response.snapshot().hasDay << '\n';
  }
  std::cout << "Time cloud response tests passed\n";
}
