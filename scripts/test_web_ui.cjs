// Run the actual embedded UI script with a small DOM/XHR harness.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../src/main.cpp'), 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
const element = id => {
  if (!elements.has(id)) elements.set(id, {textContent:'', innerHTML:'', files:[], disabled:false, value:0});
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
  document:{getElementById:element},
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
