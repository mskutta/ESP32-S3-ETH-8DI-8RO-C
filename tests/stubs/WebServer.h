#pragma once
#include <stdint.h>
#include <stddef.h>
#include <map>
#include <string>
#include <memory>
#include <deque>
class String : public std::string {
 public:
  using std::string::string;
  explicit String(int value) : std::string(std::to_string(value)) {}
  String(const std::string& s) : std::string(s) {}
};
inline String operator+(const String& a, const char* b) { return String(std::string(a) + b); }
inline uint32_t testNow = 0;
inline uint32_t millis() { return testNow; }
inline void delay(uint32_t ms) { testNow+=ms; }
enum HTTPMethod { HTTP_GET, HTTP_POST, HTTP_PUT };
enum HTTPRawStatus { RAW_START, RAW_WRITE, RAW_END, RAW_ABORTED };
struct HTTPRaw { HTTPRawStatus status; size_t totalSize=0, currentSize=0; uint8_t buf[1436]{}; };
struct Socket {
  bool stopped=false, connected=true;
  std::deque<uint8_t> bytes;
  uint32_t interval=0, nextByte=0;
};
struct FakeHttpClient {
  std::shared_ptr<Socket> state=std::make_shared<Socket>();
  void stop() { state->stopped=true;state->connected=false; }
  bool connected() { return state->connected; }
  int available() {
    if(state->stopped || (state->interval && int32_t(testNow-state->nextByte)<0))return 0;
    return state->interval ? !state->bytes.empty() : state->bytes.size();
  }
  int read(uint8_t* out,size_t n) {
    size_t count=0;
    while(count<n && available()) { out[count++]=state->bytes.front();state->bytes.pop_front(); }
    state->nextByte=testNow+state->interval;
    return int(count);
  }
};
class WebServer {
 public:
  std::map<std::string,String> headers;
  FakeHttpClient socket;
  int code=0, responses=0;
  String response;
  String header(const char* key) { return headers[key]; }
  bool hasHeader(const char* key) { return headers.count(key) && !headers[key].empty(); }
  int clientContentLength() { return _clientContentLength; }
  void setLength(int value) { _clientContentLength=value; }
  FakeHttpClient client() { return socket; }
  void send(int status, const char*, String body) { code=status; response=body; ++responses; }
 protected:
  int _clientContentLength=0;
};
class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  virtual bool canHandle(HTTPMethod, String) { return false; }
  virtual bool canRaw(String) { return false; }
  virtual bool handle(WebServer&, HTTPMethod, String) { return false; }
  virtual void raw(WebServer&, String, HTTPRaw&) {}
};
