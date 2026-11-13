// End-to-end test for trackNetwork() against the real host listener.
//
// It drives the actual tracker over real HTTP into `mpi sdk-network`, with a
// real API server for the app's fetch to call, and checks the report the host
// prints -- the same report DevX's Inspect tab renders.
//
// XMLHttpRequest does not exist in Node, so the XHR path runs against a small
// stand-in with React Native's surface (open, setRequestHeader, send, events,
// getAllResponseHeaders). fetch is Node's own.
//
// Run: node sdk/react-native/test/network-e2e.mjs [path-to-mpi]

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import http from 'node:http';
import process from 'node:process';
import { createInterface } from 'node:readline';

import { trackNetwork } from '../mpi-sdk.js';

const MPI = process.argv[2] || 'build/bin/mpi';
const TOKEN = 'devx-e2e-0123456789abcdef';

let failures = 0;
function check(condition, description) {
  if (condition) {
    console.log(`  ok   ${description}`);
  } else {
    failures += 1;
    console.log(`  FAIL ${description}`);
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(predicate, ms = 5000) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (predicate()) return true;
    await sleep(25);
  }
  return false;
}

// The app's API.
async function startApi() {
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (c) => {
      body += c;
    });
    req.on('end', () => {
      if (req.url === '/graphql') {
        const op = JSON.parse(body || '{}').operationName;
        res.writeHead(200, { 'Content-Type': 'application/json', 'X-Op': op || '' });
        res.end(JSON.stringify({ data: { op }, errors: [{ message: 'store closed' }] }));
      } else if (req.url === '/image') {
        res.writeHead(200, { 'Content-Type': 'image/png' });
        res.end(Buffer.from([0x89, 0x50, 0x4e, 0x47]));
      } else {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('nope');
      }
    });
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  return { server, base: `http://127.0.0.1:${server.address().port}` };
}

// Enough of React Native's XMLHttpRequest for the tracker: a request that
// answers after a tick with a fixed response.
class FakeXHR {
  constructor() {
    this.readyState = 0;
    this.status = 0;
    this.responseType = '';
    this.responseText = '';
    this.responseURL = '';
    this._listeners = {};
    this._headers = {};
  }
  addEventListener(name, fn) {
    (this._listeners[name] ||= []).push(fn);
  }
  _fire(name) {
    for (const fn of this._listeners[name] || []) fn();
  }
  open(method, url) {
    this._method = method;
    this.responseURL = url;
    this.readyState = 1;
  }
  setRequestHeader(key, value) {
    this._headers[key] = value;
  }
  getAllResponseHeaders() {
    return 'content-type: application/json\r\nx-from: fake-xhr\r\n';
  }
  send() {
    setTimeout(() => {
      if (this.responseURL.includes('fail')) {
        this._fire('error');
        return;
      }
      this.status = 201;
      this.readyState = 2;
      this._fire('readystatechange');
      this.readyState = 4;
      this.responseText = '{"saved":true}';
      this._fire('readystatechange');
      this._fire('load');
    }, 5);
  }
}

// React Native's fetch is the whatwg-fetch polyfill: it creates and sends an
// XMLHttpRequest synchronously inside fetch(). This XHR really goes over
// HTTP (through Node's own fetch), so the tracker's sends to DevX pass
// through it too, as they do in an app.
class RealXHR {
  constructor() {
    this.readyState = 0;
    this.status = 0;
    this.responseType = '';
    this.responseText = '';
    this._listeners = {};
    this._headers = {};
    this._responseHeaders = '';
  }
  addEventListener(name, fn) {
    (this._listeners[name] ||= []).push(fn);
  }
  _fire(name) {
    for (const fn of this._listeners[name] || []) fn();
  }
  open(method, url) {
    this._method = method;
    this.responseURL = url;
  }
  setRequestHeader(key, value) {
    this._headers[key] = value;
  }
  getAllResponseHeaders() {
    return this._responseHeaders;
  }
  send(body) {
    nodeFetch(this.responseURL, { method: this._method, headers: this._headers, body: body ?? undefined }).then(
      async (res) => {
        this.status = res.status;
        this._responseHeaders = [...res.headers].map(([k, v]) => `${k}: ${v}`).join('\r\n');
        this.readyState = 2;
        this._fire('readystatechange');
        this.responseText = await res.text();
        this.readyState = 4;
        this._fire('readystatechange');
        this._fire('load');
      },
      () => this._fire('error'),
    );
  }
}
const nodeFetch = globalThis.fetch.bind(globalThis);

function xhrFetch(XHR) {
  return function fetch(url, init = {}) {
    return new Promise((resolve, reject) => {
      const xhr = new XHR();
      xhr.open(init.method || 'GET', url);
      for (const [k, v] of Object.entries(init.headers || {})) xhr.setRequestHeader(k, v);
      xhr.addEventListener('load', () => {
        const text = xhr.responseText;
        resolve({
          ok: xhr.status >= 200 && xhr.status < 300,
          status: xhr.status,
          url,
          headers: new Headers(Object.fromEntries(
            xhr.getAllResponseHeaders().split('\r\n').filter(Boolean).map((l) => {
              const at = l.indexOf(':');
              return [l.slice(0, at), l.slice(at + 1).trim()];
            }))),
          text: async () => text,
          json: async () => JSON.parse(text),
          clone() {
            return this;
          },
        });
      });
      xhr.addEventListener('error', () => reject(new TypeError('Network request failed')));
      xhr.send(init.body);
    });
  };
}

async function testFetchOnXHR(api) {
  console.log('== fetch built on XMLHttpRequest, as in React Native ==');
  const listener = await startListener();
  const app = { XMLHttpRequest: RealXHR };
  app.fetch = xhrFetch(RealXHR);
  const tracker = trackNetwork({
    endpoint: listener.endpoint,
    token: listener.token,
    global: app,
    flushIntervalMs: 50,
    probeIntervalMs: 100,
    app: { app_identifier: 'com.example.rn', platform: 'ios' },
  });
  check(await waitFor(() => tracker.status().connected), 'the tracker connects through an XHR-based fetch');
  const res = await app.fetch(`${api.base}/graphql`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ operationName: 'GetStores', query: 'query GetStores { stores { id } }' }),
  });
  check((await res.json()).data.op === 'GetStores', 'the app gets its response');
  await sleep(300);
  await tracker.stop();
  const report = await listener.stop();
  const rows = report.network || [];
  check(rows.length === 1, `one fetch is one row, not a fetch row and an XHR row (got ${rows.length})`);
  check(rows[0] && String(rows[0].response_body).includes('GetStores'), 'with its response body');
}

async function startListener() {
  const child = spawn(MPI, ['sdk-network', '--token', TOKEN], { stdio: ['pipe', 'pipe', 'pipe'] });
  const lines = [];
  let resolveFirst;
  const first = new Promise((resolve) => {
    resolveFirst = resolve;
  });
  createInterface({ input: child.stdout }).on('line', (line) => {
    lines.push(line);
    if (line.startsWith('endpoint ')) resolveFirst(line);
  });
  child.stderr.on('data', () => {});
  const header = await Promise.race([first, sleep(5000).then(() => null)]);
  if (!header) throw new Error('the listener did not print its endpoint');
  const [, endpoint, , token] = header.split(' ');
  return {
    endpoint,
    token,
    async stop() {
      child.stdin.end('stop\n');
      await once(child, 'exit');
      return JSON.parse(lines.filter((l) => !l.startsWith('endpoint ')).join('\n'));
    },
  };
}

async function main() {
  const api = await startApi();
  const listener = await startListener();
  check(listener.token === TOKEN, 'the listener keeps the token it was given');

  const app = { fetch: globalThis.fetch.bind(globalThis), XMLHttpRequest: FakeXHR };
  const originalFetch = app.fetch;
  const originalOpen = FakeXHR.prototype.open;

  // Before anything listens, nothing is recorded.
  const tracker = trackNetwork({
    endpoint: listener.endpoint,
    token: listener.token,
    global: app,
    flushIntervalMs: 50,
    probeIntervalMs: 100,
    ignore: [`${api.base}/ignored`],
    app: { app_identifier: 'com.example.release', app_version: '9.1', platform: 'android', device: 'Pixel 8' },
  });
  check(await waitFor(() => tracker.status().connected), 'the tracker finds DevX listening');

  const query = 'query GetMenu($store: ID!) { menu(store: $store) { id } }';
  const res = await app.fetch(`${api.base}/graphql`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Authorization: 'Bearer secret-token' },
    body: JSON.stringify({ operationName: 'GetMenu', query, variables: { store: 'VN-1' } }),
  });
  const appJson = await res.json();
  check(appJson.data.op === 'GetMenu', 'the app still reads its own response body');
  await app.fetch(`${api.base}/image`).then((r) => r.arrayBuffer());
  await app.fetch(`${api.base}/ignored`).then((r) => r.text());
  await app.fetch('http://127.0.0.1:1/refused').catch(() => null);

  await new Promise((resolve) => {
    const xhr = new app.XMLHttpRequest();
    xhr.open('PUT', `${api.base}/cart`);
    xhr.setRequestHeader('Content-Type', 'application/json');
    xhr.addEventListener('load', resolve);
    xhr.send('{"item":42}');
  });
  await new Promise((resolve) => {
    const xhr = new app.XMLHttpRequest();
    xhr.open('GET', `${api.base}/fail`);
    xhr.addEventListener('error', resolve);
    xhr.send();
  });

  await sleep(300);
  check(tracker.status().buffered === 0, 'everything recorded was sent');
  await tracker.stop();
  check(app.fetch === originalFetch && FakeXHR.prototype.open === originalOpen, 'stop() puts fetch and XHR back');

  const report = await listener.stop();
  api.server.close();

  const rows = report.network || [];
  const byUrl = (part) => rows.find((r) => String(r.url).includes(part));
  check(report.app_id === 'com.example.release', 'the report names the app that connected');
  check(rows.length === 5, `five requests recorded, the ignored one left out (got ${rows.length})`);
  const gql = byUrl('/graphql');
  check(gql && gql.method === 'POST' && gql.status === 200, 'the GraphQL POST and its status');
  check(gql && String(gql.request_body).includes('GetMenu'), 'its request body, verbatim');
  check(gql && String(gql.response_body).includes('store closed'), 'its response body');
  check(gql && (gql.request_headers?.Authorization || '') === 'Bearer secret-token', 'its request headers');
  check(gql && (gql.response_headers?.['x-op'] || '') === 'GetMenu', 'its response headers');
  check(gql && typeof gql.duration_ms === 'number', 'and how long it took');
  const image = byUrl('/image');
  check(image && !image.response_body && String(image.response_body_unavailable || '').includes('image/png'),
    'a binary response is named, not sent');
  const refused = byUrl('/refused');
  check(refused && refused.failed === true, 'a request that never reached a server is a failure');
  const cart = byUrl('/cart');
  check(cart && cart.method === 'PUT' && cart.status === 201 && cart.request_body === '{"item":42}'
    && cart.response_body === '{"saved":true}', 'an XMLHttpRequest, with both bodies');
  const fail = byUrl('/fail');
  check(fail && fail.failed === true, 'a failed XMLHttpRequest');
  check(!rows.some((r) => String(r.url).startsWith(listener.endpoint)), "the tracker's own sends are not recorded");

  const api2 = await startApi();
  await testFetchOnXHR(api2);
  api2.server.close();

  if (failures > 0) {
    console.log(`\n${failures} check(s) failed`);
    process.exit(1);
  }
  console.log('\nall network checks passed');
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
