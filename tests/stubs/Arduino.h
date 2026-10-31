#pragma once
#include "WebServer.h"
#include <cstring>
#include <cstdio>
#define strlcpy testStrlcpy
inline size_t testStrlcpy(char* out,const char* in,size_t size) {
  size_t n=strlen(in);
  if(size){size_t copy=n<size-1?n:size-1;memcpy(out,in,copy);out[copy]=0;}
  return n;
}
class IPAddress {
 public:
  bool fromString(const char* value) {
    unsigned a,b,c,d;char extra;
    return sscanf(value,"%u.%u.%u.%u%c",&a,&b,&c,&d,&extra)==4 && a<256 && b<256 && c<256 && d<256;
  }
};
