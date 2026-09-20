#pragma once
// Only the opaque handle needed to include WeReadHttpClient.h. Tests replace
// the request boundary, not the production TLS implementation.
struct SimEspHttpClient;
using esp_http_client_handle_t = SimEspHttpClient*;
