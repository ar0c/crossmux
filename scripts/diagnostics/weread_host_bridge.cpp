#include <iostream>
#include <iterator>
#include <string>

#include "WeReadHostBridge.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
using namespace WeReadTime;
int main(int argc, char** argv) {
  try {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) return 2;
    std::vector<uint8_t> bytes;
    char part[4096];
    while (std::cin.read(part, sizeof(part)) || std::cin.gcount()) {
      if (bytes.size() + size_t(std::cin.gcount()) > 4 * 1024 * 1024) return 3;
      bytes.insert(bytes.end(), part, part + std::cin.gcount());
    }
    HostBridge bridge;
    if (!bridge.open(bytes)) return 4;
    const auto& id = bridge.ledger().identity();
    const std::string action = argv[1];
    if (action == "inspect" && argc == 2) {
      std::cout << "{\"account\":\"" << id.account << "\",\"book\":\"" << id.book << "\",\"source\":\"" << id.source
                << "\",\"day\":" << id.day << ",\"state\":" << unsigned(bridge.ledger().state())
                << ",\"remaining\":" << bridge.ledger().remaining() << ",\"verified\":" << bridge.ledger().verified()
                << ",\"batch\":" << bridge.ledger().batchSeconds()
                << ",\"isolated\":" << bridge.ledger().quarantinedSeconds() << "}";
      return 0;
    }
    const auto number = [&](int i) {
      std::string text = argv[i];
      if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw 1;
      return std::stoull(text);
    };
    AccountSnapshot snapshot;
    std::memcpy(snapshot.account, id.account, sizeof(snapshot.account));
    snapshot.complete = snapshot.monthComplete = true;
    if (action == "audit" && argc == 7) {
      snapshot.sampledAt = number(2);
      snapshot.month = number(3);
      snapshot.day = number(4);
      snapshot.monthSeconds = number(5);
      snapshot.daySeconds = number(6);
      std::cout << "{\"confirmation_eligible\":" << (bridge.canConfirm(snapshot) ? "true" : "false")
                << ",\"state\":" << unsigned(bridge.ledger().state())
                << ",\"verified_seconds\":" << bridge.ledger().verified()
                << ",\"pending_seconds\":" << bridge.ledger().batchSeconds()
                << ",\"remaining_seconds\":" << bridge.ledger().remaining()
                << ",\"journal_modified\":false,\"time_reports_sent\":0}";
      return 0;
    }
    if (action == "isolate" && argc == 3) {
      if (!bridge.isolate(number(2))) return 11;
    } else if (action == "reserve" && argc == 8) {
      snapshot.sampledAt = number(2);
      snapshot.month = number(3);
      snapshot.day = number(4);
      snapshot.monthSeconds = number(5);
      snapshot.daySeconds = number(6);
      if (!bridge.reserve(snapshot, snapshot.sampledAt, number(7))) return 5;
    } else if (action == "settle" && argc == 9) {
      if (number(2) > 1) return 6;
      snapshot.sampledAt = number(4);
      snapshot.month = number(5);
      snapshot.day = number(6);
      snapshot.monthSeconds = number(7);
      snapshot.daySeconds = number(8);
      if (!bridge.settle(number(2) == 1, number(3), snapshot)) return 7;
    } else
      return 8;
    std::cout.write(reinterpret_cast<const char*>(bridge.bytes().data()), bridge.bytes().size());
    return std::cout ? 0 : 9;
  } catch (...) {
    return 10;
  }
}
