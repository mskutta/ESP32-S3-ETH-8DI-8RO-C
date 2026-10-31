// Run the actual embedded UI script with a small DOM/XHR harness.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../src/main.cpp'), 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
const element = id => {
  if (!elements.has(id)) elements.set(id, {textContent:'', innerHTML:'', files:[], disabled:false, value:0, classList:{add(){},remove(){}}, closest:()=>null});
  return elements.get(id);
};
const requests = [];
let confirmed = true;
class XHR {
  constructor() { this.upload = {}; this.headers = {}; requests.push(this); }
  open(method, url) { this.method = method; this.url = url; }
  setRequestHeader(name, value) { this.headers[name] = value; }
  send(file) { this.file = file; }
}
const context = vm.createContext({
  document:{getElementById:element, querySelectorAll:()=>[]},
  window:{addEventListener(){}},
  setInterval(){},
  fetch:() => new Promise(() => {}),
  confirm:() => confirmed,
  XMLHttpRequest:XHR,
});
vm.runInContext(script, context);
const upload = () => vm.runInContext('uploadFirmware()', context);
upload();assert.match(element('uploadStatus').textContent, /Choose/);assert.equal(requests.length, 0);
element('firmwareFile').files = [{name:'firmware.bin', size:12345}];
confirmed = false;upload();assert.equal(requests.length, 0);
confirmed = true;upload();
let xhr = requests.at(-1);
assert.equal(xhr.method, 'PUT');assert.equal(xhr.url, '/api/update');
assert.equal(xhr.headers['Content-Type'], 'application/octet-stream');
assert.equal(xhr.file.size, 12345);assert.equal(element('uploadButton').disabled, true);
xhr.upload.onprogress({lengthComputable:true, loaded:50, total:100});
assert.equal(element('uploadProgress').value, 50);
xhr.status=200;xhr.onload();assert.match(element('uploadStatus').textContent, /installed; rebooting/);
assert.equal(element('uploadButton').disabled, false);
upload();xhr=requests.at(-1);xhr.status=400;xhr.responseText='Firmware validation failed';xhr.onload();
assert.match(element('uploadStatus').textContent, /Firmware validation failed/);
assert.equal(element('uploadButton').disabled, false);
upload();requests.at(-1).onerror();assert.match(element('uploadStatus').textContent, /Connection lost/);
upload();requests.at(-1).ontimeout();assert.match(element('uploadStatus').textContent, /timed out/);
console.log('Embedded firmware uploader UI tests passed');

// Exercise the actual form validator, including the reported valid target.
element('oscPort').value='53000';
element('din-0-0-enabled').checked=true;
element('din-0-0-message').value='/cue/construction.trip/go';
element('din-0-0-targetType').value='0';
element('din-0-0-port').value='53000';
for (const address of ['10.81.95.241', '192.168.1.1', '0.0.0.0', '255.255.255.255']) {
  element('din-0-0-target').value=address;
  assert.equal(vm.runInContext('validate()', context), true, address+' should be valid');
}
for (const address of ['', '10.81.95', '10.81.95.241.1', '256.1.1.1', '1.2.3.-1', '1.2.3.4junk', 'host.local', '1..2.3']) {
  element('din-0-0-target').value=address;
  assert.equal(vm.runInContext('validate()', context), false, address+' should be rejected');
}
element('din-0-0-targetType').value='1';
assert.equal(vm.runInContext('validate()', context), true, 'QLab discovery does not require a fixed IPv4 target');
element('din-0-0-targetType').value='0';element('din-0-0-enabled').checked=false;
assert.equal(vm.runInContext('validate()', context), true, 'Disabled messages do not require a target');
console.log('DIN IPv4 form validation regression tests passed');

// Field-specific messages prevent missing ports being reported as malformed OSC.
element('din-1-0-enabled').checked=true;
element('din-1-0-message').value='/cue/bricks.trip/go';
element('din-1-0-targetType').value='0';
element('din-1-0-target').value='10.81.95.241';
for(const port of ['0','','65536','-1','53000.5']){
  element('din-1-0-port').value=port;
  assert.equal(vm.runInContext('validate()', context),false);
  assert.match(element('messages').innerHTML,/Input 2 Open: destination port must be 1–65535/);
}
element('din-1-0-port').value='53000';
assert.equal(vm.runInContext('validate()', context),true);
element('din-1-0-repeat').checked=true;element('din-1-0-repeatIntervalMs').value='0';
assert.equal(vm.runInContext('validate()', context),false);
assert.match(element('messages').innerHTML,/Input 2 Open: repeat interval must be at least 20 ms/);
element('din-1-0-repeatIntervalMs').value='20';
assert.equal(vm.runInContext('validate()', context),true);
element('din-1-0-enabled').checked=false;
element('din-1-1-enabled').checked=true;
element('din-1-1-message').value='invalid';element('din-1-1-targetType').value='1';
assert.equal(vm.runInContext('validate()', context),false);
assert.match(element('messages').innerHTML,/Input 2 Closed: OSC message must start with/);
assert.ok(!element('messages').innerHTML.includes('destination port'));
console.log('DIN field-specific validation tests passed');
