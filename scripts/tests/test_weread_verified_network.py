"""Compile the real verified HTTP path; inject failures only at ESP SDK boundaries."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VerifiedNetworkTest(unittest.TestCase):
    def test_failure_diagnostics_and_cleanup(self):
        source = (ROOT / "lib/WeReadWebApi/src/WeReadHttpClient.cpp").read_text(encoding="utf-8")
        signature = source.index("esp_http_client_handle_t& client, char* sessionHost")
        start = source.rfind("WeReadHttpClient::Result runRequest(", 0, signature)
        end = source.index("\n}\n", signature) + 3
        request = source[start:end]
        start = source.index("Result requestVerified(")
        verified = source[start:source.index("\n}\n", start) + 3]
        boundary = r'''
#include "WeReadHttpClient.h"
#include <cassert>
#include <cstring>
using esp_err_t = int;
using esp_http_client_method_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=258, ESP_ERR_NO_MEM=257;
constexpr int HTTP_METHOD_POST=1, HTTP_METHOD_GET=0, HTTP_RX_BUF=1024, HTTP_TX_BUF=512;
#define CROSSPOINT_VERSION "offline"
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
struct SimEspHttpClient {} instance;
int failing=0, cleaned=0, initialized=0, opened=0, writes=0, reads=0;
bool online=true;
struct esp_http_client_config_t {
 const char* url; int buffer_size, buffer_size_tx, timeout_ms;
 void* crt_bundle_attach; int method; bool keep_alive_enable;
 void* event_handler; void* user_data;
};
void* esp_crt_bundle_attach=nullptr;
void* onRequestEvent=nullptr;
struct RequestEventContext { const WeReadHttpClient::HeaderCallback* header; };
bool copyHttpsUrlParts(const char* url, char* host, size_t, const char*& path) {
 if(!url) return false;
 std::strcpy(host,"fixture"); path="/"; return true;
}
const char* esp_err_to_name(int) { return "fixture"; }
void cleanupClient(esp_http_client_handle_t& c) { if(c) { ++cleaned; c=nullptr; } }
int esp_http_client_get_errno(esp_http_client_handle_t c) { assert(c); return 111; }
int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c,int* error,int* flags) {
 assert(c); *error=-0x1234; *flags=failing==11 ? 7 : 0; return -1;
}
int esp_http_client_set_url(esp_http_client_handle_t,const char*) { return 0; }
int esp_http_client_set_method(esp_http_client_handle_t,int) { return 0; }
int esp_http_client_set_timeout_ms(esp_http_client_handle_t,int) { return 0; }
int esp_http_client_set_user_data(esp_http_client_handle_t,void*) { return 0; }
esp_http_client_handle_t esp_http_client_init(esp_http_client_config_t*) {
 ++initialized; return failing==3 ? nullptr : &instance;
}
int esp_http_client_set_header(esp_http_client_handle_t,const char*,const char*) { return failing==4 ? -1 : 0; }
int esp_http_client_open(esp_http_client_handle_t,int) { ++opened; return failing==5 || failing==11 ? -88 : 0; }
int esp_http_client_write(esp_http_client_handle_t,const char*,int n) { ++writes; return failing==6 ? -99 : n; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) { return failing==7 ? -77 : 0; }
int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
int esp_http_client_read(esp_http_client_handle_t,char*,int) { ++reads; return failing==8 ? -66 : 0; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) { return failing!=9; }
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t) { return true; }
uint32_t millis() { static uint32_t tick=0; return ++tick; }
namespace WeReadHttpClient { bool networkReady() { return online; } }
'''
        main = r'''
int main() {
 using namespace WeReadHttpClient;
 uint8_t buffer[128]; NetworkDiagnostic d; RequestOptions o;
 o.readBuffer=buffer; o.readBufferSize=sizeof(buffer); o.diagnostic=&d;
 for(int failure=1;failure<=11;++failure) {
   failing=failure; cleaned=initialized=opened=writes=reads=0;
   online=failure!=1; o.method=failure==6 ? "POST" : "GET";
   o.body=failure==6 ? buffer : nullptr; o.bodySize=failure==6 ? 1 : 0;
   int status=42;
   auto result=requestVerified(failure==2 ? nullptr : "https://fixture/",o,{}, {},status);
   if(failure==10) {
     assert(result==Result::Ok && d.stage==NetworkDiagnostic::Stage::Complete && status==200);
   } else {
     assert(result==Result::NetworkError);
     auto expected=failure==11 ? NetworkDiagnostic::Stage::Open : static_cast<NetworkDiagnostic::Stage>(failure);
     assert(d.stage==expected);
     if(failure<8 || failure==11) assert(status==-1);
   }
   assert(opened<=1 && writes<=1); // Absolutely no SDK or HTTP write retry.
   assert(cleaned==(initialized && failure!=3 ? 1 : 0));
   if(failure==11) assert(d.verify==7 && d.tls==-0x1234 && !d.transientReadFailure());
   if(failure==5) assert(d.error==-88 && d.socket==111 && d.transientReadFailure());
   if(failure==7) assert(d.error==-77 && d.transientReadFailure());
 }
}
'''
        with tempfile.TemporaryDirectory(prefix="weread-network-") as tmp:
            path = Path(tmp)
            cpp = path / "network.cpp"
            exe = path / ("network.exe" if os.name == "nt" else "network")
            cpp.write_text(boundary + request + "\nnamespace WeReadHttpClient {\n" + verified + "\n}\n" + main,
                           encoding="utf-8")
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "lib/WeReadWebApi/src"),
                "-I" + str(ROOT / "test/weread_webapi/time_cloud_stubs"), str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
