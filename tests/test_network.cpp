#include "WS_OscTcpInput.h"
#include "WS_OscPacket.h"
#include "WS_FirmwareHttp.h"
#include <assert.h>
#include <deque>
#include <memory>
#include <vector>
#include <iostream>
#include <random>

using Bytes = std::vector<uint8_t>;
Bytes slip(const Bytes& packet) {
  Bytes out{0xC0};
  for (auto b : packet) {
    if (b == 0xC0) { out.push_back(0xDB); out.push_back(0xDC); }
    else if (b == 0xDB) { out.push_back(0xDB); out.push_back(0xDD); }
    else out.push_back(b);
  }
  out.push_back(0xC0);
  return out;
}
struct Connection { bool connected=true; std::deque<uint8_t> bytes; };
struct Client {
  std::shared_ptr<Connection> connection;
  explicit operator bool() const { return connected(); }
  bool connected() const { return connection && connection->connected; }
  void stop() { if (connection) connection->connected=false; connection.reset(); }
  size_t available() { return connected() ? connection->bytes.size() : 0; }
  int read() { if (!available()) return -1; int b=connection->bytes.front();connection->bytes.pop_front();return b; }
};
struct Server {
  inline static std::deque<Client> pending;
  inline static uint16_t port=0;
  explicit Server(uint16_t value) { port=value; }
  void begin() {}
  void end() { for(auto& c:pending)c.stop();pending.clear(); }
  Client available() { if(pending.empty())return {};auto c=pending.front();pending.pop_front();return c; }
};
Client connect() { Client c{std::make_shared<Connection>()};Server::pending.push_back(c);return c; }
void append(Client& c, Bytes bytes) { for(auto b:bytes)c.connection->bytes.push_back(b); }
void framing() {
  OscSlipInput parser;
  Bytes packet{'/', 'x', 0, 0, ',', 'i', 0, 0, 0, 0, 0xC0, 0xDB};
  auto encoded=slip(packet);
  size_t n=0;
  for(auto b:encoded)n=parser.feed(b,100);
  assert(n==packet.size() && Bytes(parser.data(),parser.data()+n)==packet);
  parser.reset();
  for(auto b:Bytes{0xC0,1,0xDB,2,0xC0}) assert(parser.feed(b,0)==0);
  for(auto b:encoded)n=parser.feed(b,0);
  assert(n==packet.size());
  for(auto b:Bytes{0xC0,1,0xDB,0xC0})assert(parser.feed(b,0)==0);
  auto oversized=slip(Bytes(1025,'a'));
  for(auto b:oversized)assert(parser.feed(b,0)==0);
  for(auto b:slip(Bytes(1024,'a')))n=parser.feed(b,0);
  assert(n==1024);
  parser.reset();parser.feed(0xC0,0);parser.feed(1,0xFFFFFF00u);
  assert(!parser.expired(0xFFFFFF00u+1999u));assert(parser.expired(0xFFFFFF00u+2000u));
  parser.reset();assert(!parser.expired(10000));

  OscTcpInput<Server,Client> receiver(53000);receiver.begin(53000);
  std::vector<Bytes> received;
  auto receive=[&](const uint8_t* data,size_t size){received.emplace_back(data,data+size);};
  Client a=connect();append(a,Bytes(encoded.begin(),encoded.begin()+5));receiver.poll(0,receive);
  Client b=connect();append(b,encoded);receiver.poll(10,receive);
  assert(received.size()==1 && received.back()==packet);
  append(a,Bytes(encoded.begin()+5,encoded.end()));append(a,encoded);receiver.poll(20,receive);
  assert(received.size()==3);
  Client extra=connect();receiver.poll(30,receive);assert(!extra.connected());
  append(a,{0xC0,1,2});receiver.poll(40,receive);receiver.poll(2040,receive);assert(!a.connected());
  Client fresh=connect();append(fresh,encoded);receiver.poll(2050,receive);assert(received.size()==4);
  append(fresh,{0xC0,1});receiver.poll(2060,receive);fresh.connection->connected=false;
  Client reconnect=connect();append(reconnect,encoded);receiver.poll(2070,receive);assert(received.size()==5);
  receiver.begin(54000);assert(!reconnect.connected() && !b.connected() && Server::port==54000);
  Client afterPort=connect();append(afterPort,encoded);receiver.poll(2100,receive);assert(received.size()==6);
  receiver.stop();assert(!afterPort.connected());
  receiver.begin(53000);assert(Server::port==53000);
  // Per-client read budgets prevent flood starvation.
  Client flood=connect();for(int i=0;i<100;i++)append(flood,encoded);
  receiver.poll(2200,receive);assert(flood.available()>0);receiver.stop();
}
void packetBounds() {
  Bytes noArgs{'/','x',0,0,',',0,0,0};
  assert(OscPacketValid(noArgs.data(),noArgs.size()));
  Bytes arg{'/','x',0,0,',','s',0,0,'a',0,0,0};
  assert(OscPacketValid(arg.data(),arg.size()));
  arg[9]='b';arg[10]='b';arg[11]='b';assert(!OscPacketValid(arg.data(),arg.size()));
  Bytes blob{'/','x',0,0,',','b',0,0,0xFF,0xFF,0xFF,0xFF};
  assert(!OscPacketValid(blob.data(),blob.size()));
  assert(!OscPacketValid(noArgs.data(),3));
  // Exercise arbitrary complete frames through the bounds checker under ASan/UBSan.
  std::mt19937 random(1234);
  for(int i=0;i<20000;i++) {
    Bytes bytes(random()%1025);
    for(auto& b:bytes)b=uint8_t(random());
    if(!bytes.empty() && i%2==0)bytes[0]='/';
    OscPacketValid(bytes.data(),bytes.size());
  }
}
struct Backend : FirmwareBackend {
  bool offOk=true,beginOk=true,writeOk=true,finishOk=true,held=false;
  bool relays[8]{true,true,true,true,true,true,true,true};
  int starts=0,finishes=0,aborts=0,leaves=0;
  size_t written=0;
  bool enterMaintenance() override { held=true;if(offOk)for(auto& r:relays)r=false;return offOk; }
  void leaveMaintenance() override { held=false;++leaves; }
  bool begin(size_t) override { assert(held);++starts;return beginOk; }
  size_t write(const uint8_t*,size_t size) override { assert(held);written+=size;return writeOk?size:0; }
  bool finish() override { assert(held);++finishes;return finishOk; }
  void abort() override { ++aborts; }
};
inline size_t testCapacity=1024;
size_t capacity() { return testCapacity; }
int reboots=0;
void installed() { ++reboots; }
struct Fixture {
  Backend backend;
  FirmwareUpload upload{backend};
  FirmwareHttp http{upload,capacity,installed};
  FirmwareWebServer server;
  HTTPRaw raw{RAW_START};
  Fixture() {
    testNow=0;testCapacity=1024;reboots=0;
    server.headers["Content-Type"]="application/octet-stream";
    server.headers["Content-Length"]="8";server.setLength(8);
    server.socket.state->bytes=std::deque<uint8_t>(8,0xE9);
    assert(http.canHandle(HTTP_PUT,"/api/update"));
    assert(!http.canHandle(HTTP_POST,"/api/update"));
  }
  void event(HTTPRawStatus status,size_t size=0) {raw.status=status;raw.currentSize=size;http.raw(server,"/api/update",raw);}
};
void uploads() {
  uint8_t bytes[9]{};
  // Exercise the actual upload state machine independently of the transport.
  for(int failure=0;failure<8;failure++) {
    Backend backend;FirmwareUpload upload(backend);
    if(failure==0)backend.offOk=false;
    if(failure==1)backend.beginOk=false;
    if(failure==2)backend.writeOk=false;
    if(failure==3)backend.finishOk=false;
    upload.start(8,1024,0);
    if(failure==4)upload.tick(120000);
    else if(failure==5)upload.disconnect();
    else if(failure==6){upload.push(bytes,3,1);upload.complete(2);}
    else if(failure==7)upload.push(bytes,9,1);
    else {upload.push(bytes,8,1);upload.complete(2);}
    assert(!upload.installed() && !upload.active() && !backend.held);
    assert(backend.aborts==1 && backend.leaves==1);
    if(failure==0)assert(backend.starts==0);
    backend.offOk=backend.beginOk=backend.writeOk=backend.finishOk=true;
    assert(upload.start(8,1024,10));
    for(auto r:backend.relays)assert(!r);
    assert(upload.push(bytes,8,11));assert(upload.complete(12));assert(backend.held);
    upload.disconnect();assert(upload.installed() && backend.aborts==1);
  }
  {Backend b;FirmwareUpload u(b);assert(!u.start(0,1024,0));assert(!u.start(9,8,0));assert(!b.starts);}
  {Backend b;FirmwareUpload u(b);u.start(8,1024,0);u.push(bytes,8,1);assert(!u.complete(120000));assert(!u.installed());}
  {Backend b;FirmwareUpload u(b);u.start(8,1024,0xFFFFFF00u);assert(u.push(bytes,8,0xFFFFFF00u+119999u));assert(u.complete(0xFFFFFF00u+119999u));}

  // Real HTTP raw adapter, including its bounded read loop and reboot signal.
  {Fixture f;f.event(RAW_START);assert(f.backend.held && f.upload.active());
    for(auto r:f.backend.relays)assert(!r);
    f.event(RAW_END);assert(f.upload.installed() && f.backend.held);
    assert(f.server.code==200 && reboots==1 && f.backend.finishes==1);
    f.http.handle(f.server,HTTP_PUT,"/api/update");assert(f.server.responses==1);
    f.event(RAW_START);assert(f.server.code==409 && reboots==1);
  }
  for(int failure=0;failure<7;failure++) {
    Fixture f;
    if(failure==0)f.backend.offOk=false;
    if(failure==1)f.backend.beginOk=false;
    if(failure==2)f.backend.writeOk=false;
    if(failure==3)f.backend.finishOk=false;
    if(failure==4)f.server.socket.state->connected=false,f.server.socket.state->bytes.clear();
    if(failure==5)f.server.socket.state->bytes.resize(3); // Incomplete, then stalled.
    if(failure==6){ // One byte every 4 seconds stays below the idle limit, but hits total deadline.
      testCapacity=1024;f.server.headers["Content-Length"]="100";f.server.setLength(100);
      f.server.socket.state->bytes.resize(100);f.server.socket.state->interval=4000;
    }
    f.event(RAW_START);f.event(RAW_END);f.event(RAW_ABORTED);
    assert(!f.upload.installed() && !f.upload.active() && !f.backend.held && reboots==0);
    assert(f.server.code==400 && f.backend.aborts==1 && f.backend.leaves==1);
    if(failure==6)assert(testNow==120000);
  }
  for(auto length:{"", "0", "-1", "+8", "8x", "1025", "99999999999999999999999999"}) {
    Fixture f;f.server.headers["Content-Length"]=length;f.event(RAW_START);
    assert(!f.upload.active() && !f.backend.starts && f.server.socket.state->stopped && reboots==0);
  }
  {Fixture f;f.server.headers["Transfer-Encoding"]="chunked";f.event(RAW_START);assert(f.server.code==400 && !f.backend.starts);}
  {Fixture f;f.server.headers["Content-Type"]="text/plain";f.event(RAW_START);assert(f.server.code==400 && !f.backend.starts);}
  {Fixture f;testCapacity=0;f.event(RAW_START);assert(f.server.code==413 && !f.backend.starts);}
  {Fixture f;f.http.handle(f.server,HTTP_PUT,"/api/update");assert(f.server.code==400 && !reboots);}
  {Fixture f;f.server.setLength(7);f.event(RAW_START);assert(f.server.code==400 && !f.backend.starts);}
  {Fixture f;testNow=0xFFFFFF00u;f.event(RAW_START);f.event(RAW_END);assert(f.upload.installed());}
}
int main() { framing();packetBounds();uploads();std::cout<<"SLIP receiver, OSC bounds, and firmware HTTP/lifecycle tests passed\n"; }
