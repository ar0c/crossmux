// Compiled only by the explicit host acceptance profile; absent from firmware.
#if defined(SIMULATOR) && defined(CROSSPOINT_MANAGED_ACCEPTANCE)
#include <Arduino.h>
#include <HalStorage.h>
#include <curl/curl.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "WeReadClient.h"

namespace WeReadHttpClient {
Result acceptanceRequest(const char* url, const RequestOptions& options, const DataCallback& data,
                         const HeaderCallback& header, int& status) {
  const std::string origin = "https://wesync.ar0c.com";
  const char* fixture = std::getenv("CROSSPOINT_MANAGED_FIXTURE");
  if (!fixture || !std::string(fixture).starts_with("http://127.0.0.1:") ||
      !std::string(url).starts_with(origin + "/api/"))
    return Result::NetworkError;
  const std::string target = std::string(fixture) + std::string(url).substr(origin.size());
  CURL* curl = curl_easy_init();
  if (!curl) return Result::NetworkError;
  struct curl_slist* headers = nullptr;
  for (size_t i = 0; i < options.headerCount; i++)
    headers =
        curl_slist_append(headers, (std::string(options.headers[i].name) + ": " + options.headers[i].value).c_str());
  std::string response;
  uint32_t cursor = UINT32_MAX;
  curl_easy_setopt(curl, CURLOPT_URL, target.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION, +[](char* b, size_t n, size_t m, void* raw) -> size_t {
        auto& out = *static_cast<std::string*>(raw);
        if (out.size() + n * m > 8 * 1024 * 1024) return 0;
        out.append(b, n * m);
        return n * m;
      });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(
      curl, CURLOPT_HEADERFUNCTION, +[](char* b, size_t n, size_t m, void* raw) -> size_t {
        std::string h(b, n * m);
        if (h.starts_with("X-Next-Cursor: "))
          *static_cast<uint32_t*>(raw) = unsigned(std::strtoul(h.c_str() + 15, nullptr, 10));
        return n * m;
      });
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cursor);
  if (options.body) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, options.body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(options.bodySize));
  }
  const auto result = curl_easy_perform(curl);
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  status = int(http);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) return Result::NetworkError;
  if (cursor != UINT32_MAX && header) {
    const auto value = std::to_string(cursor);
    header("X-Next-Cursor", value.c_str());
  }
  for (size_t i = 0; i < response.size(); i += 7)
    if (!data(reinterpret_cast<const uint8_t*>(response.data() + i), std::min(size_t(7), response.size() - i)))
      return Result::Aborted;
  return Result::Ok;
}
}  // namespace WeReadHttpClient
static void complete(WeReadClient::Operation& operation) {
  for (unsigned i = 0; i < 1000; i++) {
    const auto event = operation.step();
    if (event == WeReadClient::Operation::Event::Complete || event == WeReadClient::Operation::Event::DetailReady)
      return;
    if (event == WeReadClient::Operation::Event::Failed) {
      std::cerr << "Managed operation failed: " << int(operation.error()) << '\n';
      std::_Exit(2);
    }
    if (event == WeReadClient::Operation::Event::QrReady) {
      std::cerr << "Unexpected device QR fallback\n";
      std::_Exit(3);
    }
    delay(10);
  }
  std::cerr << "Managed operation exceeded bounded steps\n";
  std::_Exit(4);
}
void runManagedWeReadAcceptance() {
  assert(Storage.begin());
  // Production Operation, parsers, decoder, SD writers and EPUB packager.
  // Only OS HTTP transport is mapped to a synthetic loopback backend.
  WeReadClient::Operation operation;
  assert(operation.begin(WeReadClient::Operation::Kind::Sync));
  complete(operation);
  WeReadStore::ShelfRecord book;
  std::strcpy(book.bookId, "123");
  std::strcpy(book.title, "Managed fixture");
  assert(operation.begin(WeReadClient::Operation::Kind::Detail, &book));
  complete(operation);
  WeReadClient::DownloadOptions options;
  options.imagePolicy = WeReadStore::ImagePolicy::Exclude;
  assert(operation.begin(WeReadClient::Operation::Kind::Download, &book, options));
  complete(operation);
  assert(Storage.exists(operation.finalPath()));
  assert(operation.beginBrowseCache(WeReadStore::bookRecord(book)));
  complete(operation);
  WeReadClient::ProgressSyncInput position;
  position.localOffsetBasis = WeReadClient::LocalOffsetBasis::VisibleText;
  assert(operation.beginProgressSync("123", position, WeReadClient::ProgressSyncMode::Compare));
  complete(operation);
  assert(operation.progressSyncResult().outcome == WeReadClient::ProgressSyncOutcome::AlreadySynced);

  std::cout << "MANAGED_ACCEPTANCE_PASS shelf detail catalog two chapters decoder EPUB package; no device QR or real "
               "writes\n";
  std::cout << "Operation bytes=" << sizeof(operation) << std::endl;
  std::_Exit(0);
}
#endif
