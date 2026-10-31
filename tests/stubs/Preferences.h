#pragma once
#include "Arduino.h"
#include <vector>
class Preferences {
 public:
  using Blob=std::vector<uint8_t>;
  inline static std::map<std::string,std::map<std::string,Blob>> storage;
  inline static bool failWrite=false,configPartition=true;
  bool begin(const char* name,bool readonly=false,const char* partition=nullptr) {
    if(partition && std::string(partition)=="config" && !configPartition)return false;
    partition_=partition?partition:"nvs";
    name_=partition_+":"+name;
    readonly_=readonly;
    return !readonly || storage.count(name_);
  }
  size_t getBytesLength(const char* key){return storage[name_][key].size();}
  size_t getBytes(const char* key,void* out,size_t size){auto& b=storage[name_][key];if(b.size()!=size)return 0;memcpy(out,b.data(),size);return size;}
  size_t putBytes(const char* key,const void* data,size_t size) {
    if(readonly_ || failWrite)return 0;
    auto& old=storage[name_][key];
    auto pages=[](size_t n){return (n+3999)/4000;};
    size_t capacity=partition_=="config"?65536:20480;
    if((pages(size)+pages(old.size())+1)*4096>capacity)return 0;
    auto* bytes=static_cast<const uint8_t*>(data);old.assign(bytes,bytes+size);return size;
  }
  void end(){}
 private:
  std::string name_,partition_;
  bool readonly_=false;
};
