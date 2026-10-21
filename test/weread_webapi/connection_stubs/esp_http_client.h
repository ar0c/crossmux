#pragma once

#include <cstdint>
#include <cstdlib>

// This fixture exercises the real wolfSSL path. Accidentally selecting the
// certificate-verified transport must fail locally rather than make a request.
using esp_err_t = int;
inline constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 1, ESP_ERR_NO_MEM = 2;
using esp_http_client_handle_t = void*;
enum esp_http_client_method_t { HTTP_METHOD_GET, HTTP_METHOD_POST };
inline constexpr int HTTP_EVENT_ON_HEADER = 1;
struct esp_http_client_event_t {
  int event_id;
  void* user_data;
  const char* header_key;
  const char* header_value;
};
struct esp_http_client_config_t {
  const char* url = nullptr;
  int buffer_size = 0, buffer_size_tx = 0, timeout_ms = 0;
  esp_err_t (*crt_bundle_attach)(void*) = nullptr;
  esp_http_client_method_t method = HTTP_METHOD_GET;
  bool keep_alive_enable = false;
  esp_err_t (*event_handler)(esp_http_client_event_t*) = nullptr;
  void* user_data = nullptr;
};
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*) { std::abort(); }
inline int esp_http_client_set_url(void*, const char*) { return ESP_FAIL; }
inline int esp_http_client_set_method(void*, esp_http_client_method_t) { return ESP_FAIL; }
inline int esp_http_client_set_timeout_ms(void*, int) { return ESP_FAIL; }
inline int esp_http_client_set_user_data(void*, void*) { return ESP_FAIL; }
inline int esp_http_client_set_header(void*, const char*, const char*) { return ESP_FAIL; }
inline int esp_http_client_open(void*, int) { return ESP_FAIL; }
inline int esp_http_client_write(void*, const char*, int) { return ESP_FAIL; }
inline int64_t esp_http_client_fetch_headers(void*) { return ESP_FAIL; }
inline int esp_http_client_get_status_code(void*) { return 0; }
inline int esp_http_client_read(void*, char*, int) { return ESP_FAIL; }
inline bool esp_http_client_is_complete_data_received(void*) { return false; }
inline bool esp_http_client_is_persistent_connection(void*) { return false; }
inline int esp_http_client_get_errno(void*) { return 0; }
inline int esp_http_client_get_and_clear_last_tls_error(void*, int*, int*) { return ESP_OK; }
inline int esp_http_client_cleanup(void*) { return ESP_OK; }
