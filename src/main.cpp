#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOSCETH.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "WS_Config.h"
#include "WS_ETH.h"
#include "WS_GPIO.h"
#include "WS_MDNS.h"
#include "WS_Relay.h"
#include "WS_OscText.h"

constexpr uint8_t kDinPins[DIN_COUNT] = {4,5,6,7,8,9,10,11};
constexpr uint16_t kArtNetPort=6454, kSacnPort=5568;
constexpr size_t kOscPacketMax=1024;
constexpr uint32_t kDinPollIntervalMs=10;
constexpr size_t kTcpOscQueueLength=16;
constexpr uint32_t kTcpOscTimeoutMs=200;
constexpr uint32_t kTcpFailureLogIntervalMs=5000;

DeviceConfig gConfig;
bool gDinState[DIN_COUNT]={},gDinCandidate[DIN_COUNT]={};
unsigned long gDinCandidateSince[DIN_COUNT]={},gDinLastSent[DIN_COUNT][DIN_EVENT_COUNT][MAX_DIN_MESSAGES]={},gLastDinPollMs=0;
unsigned long gPulseTimers[RELAY_COUNT]={}; bool gPulseActive[RELAY_COUNT]={};
unsigned long gDmxLastPacketMs=0; bool gDmxEverReceived=false;
WiFiUDP gOscUdp,gArtNetUdp,gSacnUdp; WiFiServer gOscTcpServer(53000); WiFiClient gOscTcpClient;
WebServer gWebServer(80); uint8_t gSacnPriority[RELAY_COUNT]={},gSacnCid[RELAY_COUNT][16]={};
using OscMessage=arduino::osc::message::Message; using OscEncoder=arduino::osc::message::Encoder;

struct TcpOscJob {
  char host[16];
  char qlabHostname[64];
  uint16_t port;
  uint16_t payloadSize;
  bool isQlabTarget;
  uint8_t payload[kOscPacketMax];
};

struct TcpFailureLog {
  char host[16];
  uint16_t port;
  unsigned long lastLogMs;
};

QueueHandle_t gTcpOscQueue=nullptr;
TcpFailureLog gTcpFailureLogs[8]={};

const char kIndexHtml[] PROGMEM=R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SpookIO</title>
<style>
body{font:15px system-ui,sans-serif;margin:0;background:#eef1f5;color:#1e293b}main{max-width:1200px;margin:auto;padding:18px}
header,.card,details{background:#fff;border-radius:10px;padding:16px;margin:12px 0;box-shadow:0 1px 4px #0001}
nav{position:sticky;top:0;background:#fff;padding:10px;border-radius:8px;z-index:2}nav a{margin-right:14px;color:#075985}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:10px}.row{border:1px solid #dbe2ea;border-radius:7px;padding:10px;margin:8px 0}
label{display:block;font-weight:600;margin:6px 0}input,select{box-sizing:border-box;width:100%;height:36px;line-height:20px;padding:7px;border:1px solid #b7c2ce;border-radius:5px;background:white}input[type=checkbox]{width:auto;height:auto;line-height:normal;margin-right:6px}
button{padding:8px 13px;border:0;border-radius:5px;background:#0369a1;color:#fff;cursor:pointer;margin:3px}button.secondary{background:#64748b}button.danger{background:#b91c1c}
.help{font-size:12px;color:#64748b}.hidden{display:none}.error{border-color:#dc2626!important;background:#fff1f2}.errors{background:#fee2e2;color:#991b1b;padding:10px;border-radius:6px;white-space:pre-wrap}
pre{white-space:pre-wrap;overflow:auto;background:#0f172a;color:#dbeafe;padding:12px;border-radius:6px;max-height:600px}.status{white-space:pre-wrap;padding:10px;background:#e2e8f0;border-radius:6px}
</style></head><body><main>
<header><h1>SpookIO</h1><p>Configure digital inputs, relay control, OSC, Art-Net, and sACN. Changes apply only after Save.</p>
<nav><a href="#global">Global</a><a href="#inputs">Digital Inputs</a><a href="#relays">Relays</a><a href="#statusSection">Status</a><a href="#advanced">Advanced JSON</a></nav>
<div><button onclick="saveConfig()">Save configuration</button><button class="secondary" onclick="loadConfig()">Reload</button><button class="danger" onclick="restoreDefaults()">Restore defaults</button></div><div id="messages"><div class="status">Loading configuration…</div></div></header>
<section id="global" class="card"><h2>Global settings</h2><div id="globalForm" class="grid"></div></section>
<section id="inputs"><h2>Digital inputs</h2><p class="help">Each input has one message for Open and one for Closed. An input is Open when it is not connected to ground and Closed when it is connected to ground. Messages use QLab syntax, for example <code>/cue/1/opacity 0.5</code>.</p><div id="inputForms"></div></section>
<section id="relays"><h2>Relays</h2><div id="relayForms"></div></section>
<section id="statusSection" class="card"><h2>Status</h2><div id="status" class="status">Loading...</div></section>
<details id="advanced"><summary>Advanced JSON preview (read-only)</summary><pre id="jsonPreview"></pre></details>
<script>
let config=null;
const $=id=>document.getElementById(id);
const esc=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function selected(v,n){return Number(v)===Number(n)?' selected':''}
function checked(v){return v?' checked':''}
function showNotice(text,isError=false){$('messages').innerHTML='<div class="'+(isError?'errors':'status')+'">'+esc(text)+(isError?' <button class="secondary" onclick="loadConfig()">Reload</button>':'')+'</div>'}
function showFailure(context,error){showNotice(context+': '+(error?.message||error),true)}
async function fetchJson(url){let response=await fetch(url);if(!response.ok)throw new Error(await response.text()||('HTTP '+response.status));return response.json()}
function inputField(id,label,value,type='text',extra=''){return '<label>'+label+'<input id="'+id+'" type="'+type+'" value="'+esc(value)+'" '+extra+'></label>'}
function selectField(id,label,value,options){return '<label>'+label+'<select id="'+id+'">'+options.map(x=>'<option value="'+x[0]+'"'+selected(value,x[0])+'>'+x[1]+'</option>').join('')+'</select></label>'}
function render(){
  let g='<div>'+inputField('oscPort','OSC listen port',config.oscPort,'number','min="1" max="65535"')+inputField('debounceMs','Input debounce (ms)',config.debounceMs,'number','min="0" max="5000"')+inputField('dmxTimeoutMs','DMX timeout (ms)',config.dmxTimeoutMs,'number','min="100" max="60000"')+'</div><div>'+ '<label><input id="qlabDiscovery" type="checkbox"'+checked(config.qlabDiscovery)+'>Enable QLab discovery</label><p class="help">Art-Net uses UDP 6454. sACN uses UDP 5568.</p></div>';
  $('globalForm').innerHTML=g;
  let ih='';
  for(let i=0;i<8;i++){ih+='<details class="card"><summary><b>Input '+(i+1)+'</b> — current: <span id="din-'+i+'-state">'+(config._status?.din?.[i]||'Unknown')+'</span></summary>';
    for(let e=0;e<2;e++){let m=config.din[i][e][0];let name=e?'Closed':'Open';ih+='<div class="row"><h3>'+name+' message</h3><label><input id="din-'+i+'-'+e+'-enabled" type="checkbox"'+checked(m.enabled)+'> Enabled</label>'+inputField('din-'+i+'-'+e+'-message','OSC message',m.message,'text','maxlength="192" placeholder="/cue/1/go"')+'<div class="grid">'+selectField('din-'+i+'-'+e+'-targetType','Target',m.targetType,[['0','IP address'],['1','QLab']])+selectField('din-'+i+'-'+e+'-transport','Transport',m.transport,[['0','UDP'],['1','TCP']])+inputField('din-'+i+'-'+e+'-target','IPv4 target',m.target,'text','maxlength="15"')+inputField('din-'+i+'-'+e+'-port','Port',m.port,'number','min="1" max="65535"')+'</div><label><input id="din-'+i+'-'+e+'-repeat" type="checkbox"'+checked(m.repeat)+'> Repeat while active</label>'+inputField('din-'+i+'-'+e+'-repeatIntervalMs','Repeat interval (ms)',m.repeatIntervalMs,'number','min="20"')+'<button class="secondary" onclick="testDin('+i+','+e+')">Send test</button></div>'}
    ih+='</details>'}
  $('inputForms').innerHTML=ih;
  let rh='';
  for(let i=0;i<8;i++){let src=config.relaySources[i];rh+='<details class="card"><summary><b>Relay '+(i+1)+'</b> — current: '+(config._status?.relays?.[i]?'ON':'OFF')+'</summary>'+selectField('relay-'+i+'-source','Control source',src,[['0','OSC'],['1','Art-Net'],['2','sACN']])+'<div id="relay-'+i+'-dmx" class="'+(src==0?'hidden':'row')+'">'+
      '<div class="grid">'+inputField('relay-'+i+'-universe','Universe',src==1?config.artnet[i].universe:config.sacn[i].universe,'number','min="1"')+inputField('relay-'+i+'-channel','Channel',src==1?config.artnet[i].channel:config.sacn[i].channel,'number','min="1" max="512"')+inputField('relay-'+i+'-on','ON level',src==1?config.artnet[i].onLevel:config.sacn[i].onLevel,'number','min="0" max="255"')+inputField('relay-'+i+'-off','OFF level',src==1?config.artnet[i].offLevel:config.sacn[i].offLevel,'number','min="0" max="255"')+'</div></div>';
    rh+='<div id="relay-'+i+'-rules" class="'+(src!=0?'hidden':'')+'"><h3>OSC rules</h3>';
    for(let n=0;n<3;n++){let r=config.relayOsc[i][n],actions=['OFF','ON','PULSE'];rh+='<details class="row"><summary><b>'+actions[n]+'</b> rule — '+(r.enabled?'Enabled':'Disabled')+'</summary><label><input id="rr-'+i+'-'+n+'-enabled" type="checkbox"'+checked(r.enabled)+'> Enabled</label>'+selectField('rr-'+i+'-'+n+'-matchMode','Match mode',r.matchMode,[['0','Any arguments'],['1','No arguments'],['2','Exact message']])+inputField('rr-'+i+'-'+n+'-matchMessage','Match message',r.matchMessage,'text','maxlength="192" placeholder="/relay/'+(i+1)+' 0"')+(n==2?inputField('rr-'+i+'-'+n+'-pulse','Pulse duration (ms)',r.pulseDurationMs,'number','min="1"'):'')+'<button class="secondary" onclick="testRelay('+i+','+n+')">Run test</button></details>'}
    rh+='</div></details>'}
  $('relayForms').innerHTML=rh;
  document.querySelectorAll('select').forEach(x=>{if(!x.id.endsWith('-source'))x.addEventListener('change',()=>{collect();renderVisibility()})});
  document.querySelectorAll('input').forEach(x=>x.addEventListener('input',()=>{collect();renderVisibility()}));
  for(let i=0;i<8;i++){let source=$('relay-'+i+'-source');source.dataset.current=String(config.relaySources[i]);source.addEventListener('change',()=>changeRelaySource(i))}
  renderVisibility(); updatePreview();
}
function val(id){return $(id)?.value||''}
function num(id){return Number(val(id))}
function collect(){
  if(!config)return;config.oscPort=num('oscPort');config.debounceMs=num('debounceMs');config.dmxTimeoutMs=num('dmxTimeoutMs');config.qlabDiscovery=$('qlabDiscovery').checked;
  for(let i=0;i<8;i++){config.relaySources[i]=num('relay-'+i+'-source');let src=config.relaySources[i];if(src!=0){let map=src==1?config.artnet[i]:config.sacn[i];map.universe=num('relay-'+i+'-universe');map.channel=num('relay-'+i+'-channel');map.onLevel=num('relay-'+i+'-on');map.offLevel=num('relay-'+i+'-off')}
    for(let e=0;e<2;e++){let m=config.din[i][e][0];m.enabled=$('din-'+i+'-'+e+'-enabled').checked;m.message=val('din-'+i+'-'+e+'-message');m.targetType=num('din-'+i+'-'+e+'-targetType');m.transport=num('din-'+i+'-'+e+'-transport');m.target=val('din-'+i+'-'+e+'-target');m.port=num('din-'+i+'-'+e+'-port');m.repeat=$('din-'+i+'-'+e+'-repeat').checked;m.repeatIntervalMs=num('din-'+i+'-'+e+'-repeatIntervalMs')}
    for(let n=0;n<3;n++){let r=config.relayOsc[i][n];r.enabled=$('rr-'+i+'-'+n+'-enabled').checked;r.matchMode=num('rr-'+i+'-'+n+'-matchMode');r.matchMessage=val('rr-'+i+'-'+n+'-matchMessage');if(n==2)r.pulseDurationMs=num('rr-'+i+'-'+n+'-pulse')}
  }
  delete config._status; updatePreview();
}
function changeRelaySource(i){let source=$('relay-'+i+'-source'),oldSource=Number(source.dataset.current),newSource=Number(source.value);if(oldSource!=0){let oldMap=oldSource==1?config.artnet[i]:config.sacn[i];oldMap.universe=num('relay-'+i+'-universe');oldMap.channel=num('relay-'+i+'-channel');oldMap.onLevel=num('relay-'+i+'-on');oldMap.offLevel=num('relay-'+i+'-off')}config.relaySources[i]=newSource;if(newSource!=0){let map=newSource==1?config.artnet[i]:config.sacn[i];$('relay-'+i+'-universe').value=map.universe;$('relay-'+i+'-channel').value=map.channel;$('relay-'+i+'-on').value=map.onLevel;$('relay-'+i+'-off').value=map.offLevel}source.dataset.current=String(newSource);collect();renderVisibility()}
function renderVisibility(){
  for(let i=0;i<8;i++){let src=num('relay-'+i+'-source');$('relay-'+i+'-dmx').className=src==0?'hidden':'row';$('relay-'+i+'-rules').className=src==0?'':'hidden';for(let e=0;e<2;e++){let q=$('din-'+i+'-'+e+'-targetType');if(q){let isQ=Number(q.value)==1;$('din-'+i+'-'+e+'-target').parentElement.className=isQ?'hidden':'';$('din-'+i+'-'+e+'-port').parentElement.className=isQ?'hidden':''}}}
}
function updatePreview(){if($('jsonPreview')&&config){let copy=JSON.parse(JSON.stringify(config));delete copy._status;$('jsonPreview').textContent=JSON.stringify(copy,null,2)}}
function validate(){
  document.querySelectorAll('.error').forEach(x=>x.classList.remove('error'));let errors=[];
  if(num('oscPort')<1||num('oscPort')>65535){$('oscPort').classList.add('error');errors.push('Global: OSC port must be 1–65535')}
  for(let i=0;i<8;i++){let src=num('relay-'+i+'-source');if(src!=0&&num('relay-'+i+'-off')>num('relay-'+i+'-on')){errors.push('Relay '+(i+1)+': OFF level must not exceed ON level');$('relay-'+i+'-off').classList.add('error')}
    for(let e=0;e<2;e++){let p='din-'+i+'-'+e+'-',m=$(p+'message');if($(p+'enabled').checked&&(!m.value.startsWith('/')||m.value.length>192)){m.classList.add('error');errors.push('Input '+(i+1)+' '+(e?'Closed':'Open')+': message must start with / and be at most 192 characters')}if($(p+'enabled').checked&&Number($(p+'targetType').value)==0&&!/^\\d{1,3}(\\.\d{1,3}){3}$/.test($(p+'target').value)){errors.push('Input '+(i+1)+' '+(e?'Closed':'Open')+': enter a valid IPv4 target')}}}
  $('messages').innerHTML=errors.length?'<div class="errors">'+errors.join('\\n')+'</div>':'';return errors.length==0;
}
async function saveConfig(){try{collect();if(!validate())return;let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(config)}),text=await r.text();if(!r.ok)throw new Error(text||('HTTP '+r.status));showNotice(text||'Configuration saved');await loadConfig()}catch(error){showFailure('Save failed',error)}}
async function loadConfig(){showNotice('Loading configuration…');try{config=await fetchJson('/api/config');let statusError=null;try{await loadStatus()}catch(error){statusError=error}render();if(statusError)showFailure('Configuration loaded, but status failed',statusError);else showNotice('Configuration loaded')}catch(error){showFailure('Configuration could not be rendered',error)}}
async function restoreDefaults(){if(!confirm('Restore defaults? This replaces the saved configuration.'))return;try{let r=await fetch('/api/defaults',{method:'POST'}),text=await r.text();if(!r.ok)throw new Error(text||('HTTP '+r.status));showNotice(text||'Defaults restored');await loadConfig()}catch(error){showFailure('Restore defaults failed',error)}}
async function loadStatus(){let s=await fetchJson('/api/status');if(config)config._status=s;if($('status'))$('status').textContent=JSON.stringify(s,null,2);for(let i=0;i<8;i++){let state=$('din-'+i+'-state');if(state)state.textContent=s.din?.[i]||'Unknown'}}
async function testDin(i,e){if(!confirm('Send the configured '+(e?'Closed':'Open')+' message for input '+(i+1)+'?'))return;try{let r=await fetch('/api/test/din?input='+(i+1)+'&event='+(e?'closed':'open'),{method:'POST'}),text=await r.text();if(!r.ok)throw new Error(text||('HTTP '+r.status));showNotice(text)}catch(error){showFailure('DIN test failed',error)}}
async function testRelay(i,n){if(!confirm('Execute relay '+(i+1)+' rule '+(n+1)+'? This may change a physical output.'))return;try{let r=await fetch('/api/test/relay?relay='+(i+1)+'&rule='+(n+1),{method:'POST'}),text=await r.text();if(!r.ok)throw new Error(text||('HTTP '+r.status));showNotice(text)}catch(error){showFailure('Relay test failed',error)}}
window.addEventListener('error',event=>showFailure('Page error',event.error||event.message));window.addEventListener('unhandledrejection',event=>showFailure('Page error',event.reason));
loadConfig();setInterval(()=>loadStatus().catch(error=>showFailure('Status refresh failed',error)),2000);
</script></main></body></html>)HTML";

// The hardware input is active-low: an open contact reads HIGH and a grounded
// contact reads LOW. Keep that electrical polarity internal and expose the
// contact state as Open/Closed to the application and API.
bool readDinClosed(uint8_t pin){return digitalRead(pin)==LOW;}
const char* dinStateName(bool closed){return closed?"Closed":"Open";}
void relaySet(uint8_t r,bool on){if(r>=RELAY_COUNT)return;bool ok=on?Relay_Open(r+1):Relay_Closs(r+1);if(ok)Relay_Flag[r]=on;}
void relayPulse(uint8_t r,uint32_t ms){relaySet(r,true);gPulseActive[r]=true;gPulseTimers[r]=millis()+ms;RGB_Open_Time(0,0,255,200,0);}
void addArg(OscMessage&m,const OscArgumentConfig&a){switch(a.type){case ARG_INT32:m.pushInt32(a.intValue);break;case ARG_FLOAT:m.pushFloat(a.floatValue);break;case ARG_STRING:m.pushString(a.stringValue);break;case ARG_BOOL:m.pushBool(a.boolValue);break;default:break;}}
bool buildMessage(const char*text,OscMessage&m){ParsedOscText parsed;String error;if(!OscText_Parse(text,parsed,error))return false;m.init(parsed.address);for(uint8_t i=0;i<parsed.argumentCount;i++)addArg(m,parsed.args[i]);return true;}
bool sendTcpOscPayload(const char*host,uint16_t port,const uint8_t*payload,size_t payloadSize){
  if(host==nullptr||payload==nullptr||payloadSize==0||payloadSize>kOscPacketMax)return false;
  WiFiClient c;
  c.setTimeout(kTcpOscTimeoutMs);
  if(!c.connect(host,port))return false;
  uint32_t n=payloadSize;
  uint8_t h[4]={(uint8_t)(n>>24),(uint8_t)(n>>16),(uint8_t)(n>>8),(uint8_t)n};
  bool ok=c.write(h,4)==4&&c.write(payload,payloadSize)==payloadSize;
  c.stop();
  return ok;
}
void logTcpFailure(const TcpOscJob&job){
  unsigned long now=millis();
  for(auto&entry:gTcpFailureLogs){
    if(entry.port==job.port&&strcmp(entry.host,job.host)==0){
      if(now-entry.lastLogMs>=kTcpFailureLogIntervalMs){Serial.printf("TCP OSC send failed for %s:%u\n",job.host,job.port);entry.lastLogMs=now;}
      return;
    }
  }
  for(auto&entry:gTcpFailureLogs){
    if(entry.host[0]=='\0'){
      strlcpy(entry.host,job.host,sizeof(entry.host));entry.port=job.port;entry.lastLogMs=now;
      Serial.printf("TCP OSC send failed for %s:%u\n",job.host,job.port);
      return;
    }
  }
}
void tcpOscTask(void*){
  TcpOscJob job;
  for(;;){
    if(xQueueReceive(gTcpOscQueue,&job,portMAX_DELAY)!=pdTRUE)continue;
    if(!ETH_Connected())continue;
    if(job.isQlabTarget&&!MDNS_QlabTargetIsCurrent(job.qlabHostname,job.host,job.port))continue;
    if(!sendTcpOscPayload(job.host,job.port,job.payload,job.payloadSize))logTcpFailure(job);
  }
}
bool queueTcpOsc(const char*host,uint16_t port,const uint8_t*payload,size_t payloadSize,const char*qlabHostname){
  if(gTcpOscQueue==nullptr||host==nullptr||payload==nullptr||payloadSize==0||payloadSize>kOscPacketMax)return false;
  TcpOscJob job={};strlcpy(job.host,host,sizeof(job.host));job.port=port;job.payloadSize=payloadSize;job.isQlabTarget=qlabHostname!=nullptr;
  if(job.isQlabTarget)strlcpy(job.qlabHostname,qlabHostname,sizeof(job.qlabHostname));
  memcpy(job.payload,payload,payloadSize);
  return xQueueSend(gTcpOscQueue,&job,0)==pdTRUE;
}
void sendDinMessage(const DinMessageConfig&cfg){
  if(!cfg.enabled||!ETH_Connected())return;
  OscMessage m;if(!buildMessage(cfg.message,m))return;
  OscEncoder e;e.init().encode(m);if(!e.data()||!e.size()||e.size()>kOscPacketMax)return;
  size_t count=cfg.targetType==TARGET_QLAB?MDNS_QlabCount():1;
  for(size_t i=0;i<count;i++){
    char ip[16]={},qlabHostname[64]={};uint16_t port=cfg.port;const char*hostname=nullptr;
    if(cfg.targetType==TARGET_QLAB){if(!MDNS_QlabTarget(i,ip,sizeof(ip),&port)||!MDNS_QlabHostname(i,qlabHostname,sizeof(qlabHostname)))continue;hostname=qlabHostname;}
    else strlcpy(ip,cfg.target,sizeof(ip));
    if(cfg.transport==OUTPUT_TCP)queueTcpOsc(ip,port,e.data(),e.size(),hostname);
    else{WiFiUDP u;u.beginPacket(ip,port);u.write(e.data(),e.size());u.endPacket();}
  }
}
void sendDinEvent(uint8_t input,uint8_t event){for(uint8_t n=0;n<MAX_DIN_MESSAGES;n++){auto&m=gConfig.din[input][event][n];if(m.enabled){sendDinMessage(m);gDinLastSent[input][event][n]=millis();}}}

bool argMatches(const OscArgumentConfig&e,const OscMessage&m,uint8_t i){if(e.type==ARG_NONE||i>=m.size())return false;if(e.type==ARG_INT32)return m.isInt32(i)&&m.getArgAsInt32(i)==e.intValue;if(e.type==ARG_FLOAT)return m.isFloat(i)&&m.getArgAsFloat(i)==e.floatValue;if(e.type==ARG_STRING)return m.isStr(i)&&m.getArgAsString(i)==e.stringValue;if(e.type==ARG_BOOL)return m.isBool(i)&&m.getArgAsBool(i)==e.boolValue;return false;}
bool ruleMatches(const RelayOscRuleConfig&r,const OscMessage&m){if(!r.enabled)return false;ParsedOscText expected;String error;if(!OscText_Parse(r.matchMessage,expected,error))return false;if(m.address()!=expected.address)return false;if(r.matchMode==MATCH_ANY_ARGS)return true;if(r.matchMode==MATCH_NO_ARGS)return m.size()==0;if(m.size()!=expected.argumentCount)return false;for(uint8_t i=0;i<expected.argumentCount;i++)if(!argMatches(expected.args[i],m,i))return false;return true;}
void handleOscMessage(const OscMessage&m){for(uint8_t r=0;r<RELAY_COUNT;r++){if(gConfig.relaySource[r]!=SOURCE_OSC)continue;for(uint8_t n=0;n<MAX_RELAY_RULES;n++){auto&rule=gConfig.relayOsc[r][n];if(!ruleMatches(rule,m))continue;RelayAction action=RelayRuleAction(n);if(action==RELAY_ACTION_OFF){relaySet(r,false);RGB_Open_Time(255,0,0,200,0);}else if(action==RELAY_ACTION_ON){relaySet(r,true);RGB_Open_Time(0,255,0,200,0);}else relayPulse(r,rule.pulseDurationMs);return;}}}
void handleUdpOsc(){int n;while((n=gOscUdp.parsePacket())>0){if(n>(int)kOscPacketMax){while(gOscUdp.available())gOscUdp.read();continue;}uint8_t b[kOscPacketMax];int len=gOscUdp.read(b,sizeof(b));OscMessage m(b,len);if(m.available())handleOscMessage(m);}}
void handleTcpOsc(){if(!gOscTcpClient||!gOscTcpClient.connected()){WiFiClient c=gOscTcpServer.available();if(c)gOscTcpClient=c;}static uint8_t frame[kOscPacketMax],header[4],hb=0;static uint16_t expected=0,received=0;while(gOscTcpClient&&gOscTcpClient.available()){if(!expected){header[hb++]=gOscTcpClient.read();if(hb<4)continue;expected=(header[2]<<8)|header[3];hb=0;received=0;if(!expected||expected>kOscPacketMax){gOscTcpClient.stop();expected=0;continue;}}while(expected&&received<expected&&gOscTcpClient.available())frame[received++]=gOscTcpClient.read();if(expected&&received==expected){OscMessage m(frame,expected);if(m.available())handleOscMessage(m);expected=received=0;}}}

void applyDmx(uint8_t r,uint8_t level,const RelayDmxConfig&map){if(gConfig.relaySource[r]==SOURCE_OSC)return;bool state=Relay_Flag[r];if(level>=map.onLevel)state=true;else if(level<=map.offLevel)state=false;if(state!=Relay_Flag[r])relaySet(r,state);}
void handleArtNet(){int n;while((n=gArtNetUdp.parsePacket())>0){uint8_t p[600];if(n>(int)sizeof(p)){while(gArtNetUdp.available())gArtNetUdp.read();continue;}int len=gArtNetUdp.read(p,sizeof(p));if(len<18||memcmp(p,"Art-Net\0",8)||p[8]!=0||p[9]!=0x50)continue;uint16_t u=p[14]|(p[15]<<8),dl=(p[16]<<8)|p[17];if(dl>512||18+dl>len)continue;gDmxLastPacketMs=millis();gDmxEverReceived=true;for(uint8_t r=0;r<RELAY_COUNT;r++)if(gConfig.relaySource[r]==SOURCE_ARTNET&&gConfig.artnet[r].universe==u&&gConfig.artnet[r].channel<=dl)applyDmx(r,p[17+gConfig.artnet[r].channel],gConfig.artnet[r]);}}
void handleSacn(){int n;while((n=gSacnUdp.parsePacket())>0){uint8_t p[700];if(n>(int)sizeof(p)){while(gSacnUdp.available())gSacnUdp.read();continue;}int len=gSacnUdp.read(p,sizeof(p));if(len<126||p[0]||p[1]!=0x10||memcmp(p+4,"ASC-E1.17",9))continue;uint16_t u=(p[113]<<8)|p[114],count=((p[123]&15)<<8)|p[124];if(count<2||126+count-1>len)continue;bool accepted=false;for(uint8_t r=0;r<RELAY_COUNT;r++){auto&map=gConfig.sacn[r];if(gConfig.relaySource[r]!=SOURCE_SACN||map.universe!=u||map.channel>count-1)continue;bool same=!memcmp(gSacnCid[r],p+22,16);if(!same&&p[108]<gSacnPriority[r])continue;memcpy(gSacnCid[r],p+22,16);gSacnPriority[r]=p[108];applyDmx(r,p[125+map.channel],map);accepted=true;}if(accepted){gDmxLastPacketMs=millis();gDmxEverReceived=true;}}}

void pollDin(){unsigned long now=millis();if(now-gLastDinPollMs<kDinPollIntervalMs)return;gLastDinPollMs=now;for(uint8_t i=0;i<DIN_COUNT;i++){bool raw=readDinClosed(kDinPins[i]);if(raw!=gDinCandidate[i]){gDinCandidate[i]=raw;gDinCandidateSince[i]=now;}if(gDinCandidate[i]!=gDinState[i]&&now-gDinCandidateSince[i]>=gConfig.debounceMs){gDinState[i]=gDinCandidate[i];sendDinEvent(i,gDinState[i]?1:0);}uint8_t e=gDinState[i]?1:0;for(uint8_t n=0;n<MAX_DIN_MESSAGES;n++){auto&m=gConfig.din[i][e][n];if(m.enabled&&m.repeat&&now-gDinLastSent[i][e][n]>=m.repeatIntervalMs){sendDinMessage(m);gDinLastSent[i][e][n]=now;}}}}
void updateTimers(){unsigned long now=millis();for(uint8_t r=0;r<RELAY_COUNT;r++)if(gPulseActive[r]&&(long)(now-gPulseTimers[r])>=0){gPulseActive[r]=false;relaySet(r,false);}if(gDmxEverReceived&&now-gDmxLastPacketMs>=gConfig.dmxTimeoutMs){for(uint8_t r=0;r<RELAY_COUNT;r++)if(gConfig.relaySource[r]!=SOURCE_OSC)relaySet(r,false);gDmxEverReceived=false;}}

void configToJson(JsonDocument&d){
  d["oscPort"]=gConfig.oscPort; d["debounceMs"]=gConfig.debounceMs; d["dmxTimeoutMs"]=gConfig.dmxTimeoutMs; d["qlabDiscovery"]=gConfig.qlabDiscovery;
  JsonArray sources=d["relaySources"].to<JsonArray>(); for(auto v:gConfig.relaySource)sources.add(v);
  JsonArray art=d["artnet"].to<JsonArray>(), sacn=d["sacn"].to<JsonArray>();
  for(uint8_t i=0;i<RELAY_COUNT;i++){JsonObject a=art.add<JsonObject>();a["universe"]=gConfig.artnet[i].universe;a["channel"]=gConfig.artnet[i].channel;a["onLevel"]=gConfig.artnet[i].onLevel;a["offLevel"]=gConfig.artnet[i].offLevel;JsonObject s=sacn.add<JsonObject>();s["universe"]=gConfig.sacn[i].universe;s["channel"]=gConfig.sacn[i].channel;s["onLevel"]=gConfig.sacn[i].onLevel;s["offLevel"]=gConfig.sacn[i].offLevel;}
  JsonArray din=d["din"].to<JsonArray>(); for(uint8_t i=0;i<DIN_COUNT;i++){JsonArray input=din.add<JsonArray>();for(uint8_t e=0;e<DIN_EVENT_COUNT;e++){JsonArray event=input.add<JsonArray>();auto&m=gConfig.din[i][e][0];JsonObject o=event.add<JsonObject>();o["enabled"]=m.enabled;o["targetType"]=m.targetType;o["transport"]=m.transport;o["port"]=m.port;o["repeat"]=m.repeat;o["repeatIntervalMs"]=m.repeatIntervalMs;o["target"]=m.target;o["message"]=m.message;}}
  JsonArray rules=d["relayOsc"].to<JsonArray>(); for(uint8_t i=0;i<RELAY_COUNT;i++){JsonArray relay=rules.add<JsonArray>();for(uint8_t n=0;n<MAX_RELAY_RULES;n++){auto&r=gConfig.relayOsc[i][n];JsonObject o=relay.add<JsonObject>();o["enabled"]=r.enabled;o["matchMode"]=r.matchMode;o["matchMessage"]=r.matchMessage;if(RelayRuleAction(n)==RELAY_ACTION_PULSE)o["pulseDurationMs"]=r.pulseDurationMs;}}
}
bool configFromJson(JsonDocument&d,DeviceConfig&c,String&e){
  Config_Defaults(c);c.oscPort=d["oscPort"]|53000;c.debounceMs=d["debounceMs"]|50;c.dmxTimeoutMs=d["dmxTimeoutMs"]|2000;c.qlabDiscovery=d["qlabDiscovery"]|true;
  JsonArrayConst sources=d["relaySources"];for(uint8_t i=0;i<RELAY_COUNT&&i<sources.size();i++)c.relaySource[i]=sources[i]|0;
  JsonArrayConst art=d["artnet"],sacn=d["sacn"];for(uint8_t i=0;i<RELAY_COUNT;i++){if(i<art.size()){c.artnet[i].universe=art[i]["universe"]|1;c.artnet[i].channel=art[i]["channel"]|(i+1);c.artnet[i].onLevel=art[i]["onLevel"]|128;c.artnet[i].offLevel=art[i]["offLevel"]|100;}if(i<sacn.size()){c.sacn[i].universe=sacn[i]["universe"]|1;c.sacn[i].channel=sacn[i]["channel"]|(i+1);c.sacn[i].onLevel=sacn[i]["onLevel"]|128;c.sacn[i].offLevel=sacn[i]["offLevel"]|100;}}
  JsonArrayConst din=d["din"];for(uint8_t i=0;i<DIN_COUNT&&i<din.size();i++){JsonArrayConst input=din[i];for(uint8_t e=0;e<DIN_EVENT_COUNT&&e<input.size();e++){JsonArrayConst event=input[e];if(event.size()==0)continue;JsonObjectConst o=event[0];auto&m=c.din[i][e][0];m.enabled=o["enabled"]|false;m.targetType=o["targetType"]|0;m.transport=o["transport"]|0;m.port=o["port"]|0;m.repeat=o["repeat"]|false;m.repeatIntervalMs=o["repeatIntervalMs"]|1000;strlcpy(m.target,o["target"]|"",sizeof(m.target));strlcpy(m.message,o["message"]|"",sizeof(m.message));}
  }
  JsonArrayConst rules=d["relayOsc"];for(uint8_t i=0;i<RELAY_COUNT&&i<rules.size();i++){JsonArrayConst relay=rules[i];for(uint8_t n=0;n<MAX_RELAY_RULES&&n<relay.size();n++){JsonObjectConst o=relay[n];auto&r=c.relayOsc[i][n];r.enabled=o["enabled"]|false;r.matchMode=o["matchMode"]|0;if(RelayRuleAction(n)==RELAY_ACTION_PULSE)r.pulseDurationMs=o["pulseDurationMs"]|1000;strlcpy(r.matchMessage,o["matchMessage"]|"",sizeof(r.matchMessage));}}
  return Config_IsValid(c,e);
}
void getConfig(){JsonDocument d;configToJson(d);String out;serializeJson(d,out);gWebServer.send(200,"application/json",out);}
void restartOscListeners(){gOscUdp.stop();gOscTcpServer.end();gOscUdp.begin(gConfig.oscPort);gOscTcpServer=WiFiServer(gConfig.oscPort);gOscTcpServer.begin();MDNS_Configure(nullptr,gConfig.oscPort);}
void postConfig(){JsonDocument d;auto err=deserializeJson(d,gWebServer.arg("plain"));if(err){gWebServer.send(400,"text/plain",err.c_str());return;}DeviceConfig next;String why;if(!configFromJson(d,next,why)){gWebServer.send(400,"text/plain",why);return;}uint16_t oldPort=gConfig.oscPort;if(!Config_Save(next)){gWebServer.send(500,"text/plain","NVS save failed");return;}gConfig=next;if(oldPort!=gConfig.oscPort)restartOscListeners();MDNS_SetQlabDiscoveryEnabled(gConfig.qlabDiscovery);gWebServer.send(200,"text/plain","Saved and applied");}
void restoreDefaults(){Config_Defaults(gConfig);Config_Save(gConfig);MDNS_SetQlabDiscoveryEnabled(gConfig.qlabDiscovery);gWebServer.send(200,"text/plain","Defaults restored");}
void status(){JsonDocument d;JsonArray a=d["din"].to<JsonArray>(),b=d["relays"].to<JsonArray>();for(bool v:gDinState)a.add(dinStateName(v));for(bool v:Relay_Flag)b.add(v);d["ip"]=ETH_LocalIP().toString();d["qlabCount"]=MDNS_QlabCount();String out;serializeJson(d,out);gWebServer.send(200,"application/json",out);}
void testDin(){int input=gWebServer.arg("input").toInt()-1;String eventName=gWebServer.arg("event");int event=eventName.equalsIgnoreCase("closed")||eventName.equalsIgnoreCase("low")?1:0;if(input<0||input>=DIN_COUNT||!gConfig.din[input][event][0].enabled){gWebServer.send(400,"text/plain","Invalid or disabled DIN message");return;}sendDinMessage(gConfig.din[input][event][0]);gWebServer.send(200,"text/plain","DIN message sent");}
void testRelay(){int relay=gWebServer.arg("relay").toInt()-1;int rule=gWebServer.arg("rule").toInt()-1;if(relay<0||relay>=RELAY_COUNT||rule<0||rule>=MAX_RELAY_RULES||!gConfig.relayOsc[relay][rule].enabled){gWebServer.send(400,"text/plain","Invalid or disabled relay rule");return;}auto&r=gConfig.relayOsc[relay][rule];RelayAction action=RelayRuleAction(rule);if(action==RELAY_ACTION_OFF)relaySet(relay,false);else if(action==RELAY_ACTION_ON)relaySet(relay,true);else relayPulse(relay,r.pulseDurationMs);gWebServer.send(200,"text/plain","Relay rule executed");}

void setup(){delay(1000);Serial.begin(115200);gTcpOscQueue=xQueueCreate(kTcpOscQueueLength,sizeof(TcpOscJob));if(gTcpOscQueue!=nullptr)xTaskCreatePinnedToCore(tcpOscTask,"TcpOscTask",4096,nullptr,1,nullptr,1);Config_Load(gConfig);GPIO_Init();I2C_Init();Relay_Init();for(uint8_t i=0;i<8;i++){pinMode(kDinPins[i],INPUT_PULLUP);gDinState[i]=readDinClosed(kDinPins[i]);gDinCandidate[i]=gDinState[i];}uint64_t id=ESP.getEfuseMac();char hostname[24],instanceName[24];snprintf(hostname,sizeof(hostname),"spookio-%06X",(uint32_t)(id&0xffffff));snprintf(instanceName,sizeof(instanceName),"SpookIO-%06X",(uint32_t)(id&0xffffff));ETH_SetHostname(hostname);MDNS_Configure(hostname,gConfig.oscPort,instanceName);ETH_Init();for(uint16_t i=0;!ETH_Connected()&&i<100;i++){ETH_Loop();delay(100);}gOscUdp.begin(gConfig.oscPort);gArtNetUdp.begin(kArtNetPort);gSacnUdp.begin(kSacnPort);gOscTcpServer=WiFiServer(gConfig.oscPort);gOscTcpServer.begin();gWebServer.on("/",HTTP_GET,[](){gWebServer.send_P(200,"text/html; charset=utf-8",kIndexHtml);});gWebServer.on("/api/config",HTTP_GET,getConfig);gWebServer.on("/api/config",HTTP_POST,postConfig);gWebServer.on("/api/defaults",HTTP_POST,restoreDefaults);gWebServer.on("/api/status",HTTP_GET,status);gWebServer.on("/api/test/din",HTTP_POST,testDin);gWebServer.on("/api/test/relay",HTTP_POST,testRelay);gWebServer.begin();MDNS_SetQlabDiscoveryEnabled(gConfig.qlabDiscovery);Serial.printf("OSC TCP/UDP %u, HTTP 80, Art-Net %u, sACN %u\n",gConfig.oscPort,kArtNetPort,kSacnPort);RGB_Light(0,0,0);Buzzer_Open_Time(150,0);}
void loop(){handleUdpOsc();handleTcpOsc();handleArtNet();handleSacn();gWebServer.handleClient();ETH_Loop();MDNS_Loop();pollDin();updateTimers();}
