#include "WS_Config.h"
#include <Preferences.h>
#include <assert.h>
#include <iostream>
void reset(){Preferences::storage.clear();Preferences::failWrite=false;Preferences::configPartition=true;}
void legacy(const DeviceConfig& config){Preferences p;assert(p.begin("relay-config",false));assert(p.putBytes("blob",&config,sizeof(config))==sizeof(config));}
void validationMessages(const DeviceConfig& base){
  DeviceConfig config=base;
  auto& message=config.din[1][0][0];message=base.din[0][0][0];
  String error;
  auto expect=[&](const char* text){assert(!Config_IsValid(config,error));assert(error==String("Input 2 Open: ")+text);};
  message.port=0;expect("destination port must be 1–65535 (QLab usually uses 53000)");
  message.port=53000;message.repeat=true;message.repeatIntervalMs=19;
  expect("repeat interval must be at least 20 ms when Repeat is enabled");
  message.repeat=false;message.targetType=99;expect("select IP address or QLab as the target");
  message.targetType=TARGET_IP;message.transport=99;expect("select UDP or TCP as the transport");
  message.transport=OUTPUT_TCP;strlcpy(message.message,"not OSC",sizeof(message.message));
  expect("OSC message must start with /");
  message.message[0]='/';memset(message.message+1,'a',sizeof(message.message)-1);
  expect("OSC message must be at most 192 characters");
  strlcpy(message.message,"/cue/bricks.trip/go",sizeof(message.message));
  strlcpy(message.target,"256.1.1.1",sizeof(message.target));expect("enter a valid IPv4 target");
  strlcpy(message.target,"10.81.95.241",sizeof(message.target));
  strlcpy(message.message,"/cue/go unquoted",sizeof(message.message));
  assert(!Config_IsValid(config,error));assert(error.find("Input 2 Open: OSC message: ")==0);
  message=base.din[0][0][0];message.targetType=TARGET_QLAB;message.port=0;
  assert(Config_IsValid(config,error));
  config.din[1][1][0]=message;config.din[1][1][0].message[0]=0;message.enabled=false;
  assert(!Config_IsValid(config,error));assert(error=="Input 2 Closed: OSC message must start with /");
  config.din[1][1][0].enabled=false;assert(Config_IsValid(config,error));
}
int main(){
  DeviceConfig config,loaded;Config_Defaults(config);
  auto& message=config.din[0][0][0];message.enabled=true;message.port=53000;message.transport=OUTPUT_TCP;
  strlcpy(message.target,"10.81.95.241",sizeof(message.target));
  strlcpy(message.message,"/cue/construction.trip/go",sizeof(message.message));
  String error;assert(Config_IsValid(config,error));
  validationMessages(config);
  reset();legacy(config);
  auto old=Preferences::storage["nvs:relay-config"]["blob"];
  // Demonstrate why replacement in the old 20 KiB partition fails.
  {Preferences p;assert(p.begin("relay-config",false));assert(!p.putBytes("blob",&config,sizeof(config)));}
  assert(Config_Load(loaded));assert(!memcmp(&config,&loaded,sizeof(config)));
  assert(Preferences::storage["nvs:relay-config"]["blob"]==old);
  assert(Preferences::storage["config:relay-config"]["blob"].size()==sizeof(config));
  for(int i=0;i<20;i++){
    config.debounceMs=uint16_t(50+i);
    assert(Config_Save(config));assert(Config_Load(loaded));assert(!memcmp(&config,&loaded,sizeof(config)));
  }
  Preferences::failWrite=true;DeviceConfig next=config;next.debounceMs=999;
  assert(!Config_Save(next));assert(Config_Load(loaded));assert(!memcmp(&config,&loaded,sizeof(config)));
  // An interrupted/failed migration retains the legacy copy and is retryable.
  reset();legacy(config);Preferences::failWrite=true;
  assert(Config_Load(loaded));assert(!memcmp(&config,&loaded,sizeof(config)));
  assert(Preferences::storage["config:relay-config"]["blob"].empty());
  Preferences::failWrite=false;assert(Config_Load(loaded));assert(Config_Save(loaded));
  reset();legacy(config);Preferences::configPartition=false;
  assert(Config_Load(loaded));assert(!memcmp(&config,&loaded,sizeof(config)));assert(!Config_Save(config));
  reset();assert(!Config_Load(loaded));assert(loaded.oscPort==53000);assert(Config_Load(loaded));
  // Incompatible versions fall back to defaults.
  Preferences::storage["config:relay-config"]["blob"][4]=255;
  assert(!Config_Load(loaded));assert(loaded.version==CONFIG_VERSION);
  std::cout<<"Configuration migration, repeated saves, and failure-preservation tests passed\n";
}
