// The in-app SDK for React Native.
//
// It reports the things no device provider can see: which screen mounted,
// which navigation was cancelled, which interaction the user started, and what
// build the JS bundle came from. Without it those fields stay empty, because
// guessing a screen from a function name is forbidden (spec section 11).
//
// Plain ES module JavaScript with no dependencies and no build step: an app
// should be able to drop this in and delete it again. It runs on anything with
// `fetch`, which React Native has.
//
// Four rules shape the implementation, and all four are about not lying to the
// host:
//
//   * It never throws into the app. A profiler that crashes the app it is
//     measuring is worse than no profiler.
//   * Its buffer is bounded and it *counts what it drops*, reporting the count
//     with the next batch. A marker the SDK threw away must not look like a
//     thing that did not happen.
//   * Every batch carries a sequence number, so a batch lost in transit
//     becomes a recorded gap on the host rather than silence.
//   * It sends nothing until `connect()` succeeds, and it never retries
//     forever: on repeated failure it stops and says so through `status()`.
//
// Usage:
//
//   import { MpiSdk } from './mpi-sdk.js';
//
//   const mpi = new MpiSdk({
//     endpoint: 'http://127.0.0.1:59244',   // printed by `mpi record --sdk`
//     token: '35eeefcb...',                 // printed alongside it
//     app: {
//       app_identifier: 'io.example.app',
//       app_version: '4.2.0',
//       build_configuration: 'Staging',
//       js_engine: 'hermes',
//       js_bundle_id: __BUNDLE_ID__,
//       js_dev_mode: __DEV__,
//     },
//   });
//   await mpi.connect();
//   mpi.screenMount('Checkout');
//
// On Android the device reaches the host through `adb reverse`; `mpi record`
// prints that command. On the iOS simulator loopback is already shared. There
// is no wireless path on purpose.

const DEFAULTS = {
  // How many markers to hold before dropping the oldest. A screen-heavy
  // session produces a few per interaction, so this is minutes of headroom.
  bufferSize: 2048,
  // Markers per request. The host refuses larger batches.
  batchSize: 256,
  flushIntervalMs: 2000,
  requestTimeoutMs: 5000,
  // Consecutive failures before the SDK gives up. It stops rather than
  // retrying forever, and `status()` says why.
  maxConsecutiveFailures: 5,
  sdkVersion: '0.1.0',
};

// The kinds the host accepts. Sending anything else is rejected there, so the
// list is enforced here too: a typo should fail loudly in development, not
// become a marker nobody ever queries.
const KINDS = new Set([
  'screen_mount',
  'screen_unmount',
  'navigation_begin',
  'navigation_end',
  'navigation_cancel',
  'interaction_begin',
  'interaction_end',
  'async_span_begin',
  'async_span_end',
  'react_commit',
  'network_request',
  'runtime_reload',
]);

// A URL with its query string removed. Query strings carry tokens, ids and
// search terms, and none of that belongs in a performance trace (spec J01).
function redactUrl(url) {
  if (typeof url !== 'string') return '';
  const cut = url.indexOf('?');
  const withoutQuery = cut === -1 ? url : `${url.slice(0, cut)}?<redacted>`;
  const hash = withoutQuery.indexOf('#');
  return hash === -1 ? withoutQuery : withoutQuery.slice(0, hash);
}

function nowNs() {
  // A monotonic clock where one exists: wall time jumps when the device
  // adjusts it, and a jump inside a capture would reorder markers.
  if (typeof globalThis.performance?.now === 'function') {
    return Math.round(globalThis.performance.now() * 1e6);
  }
  return Date.now() * 1e6;
}

export class MpiSdk {
  constructor(options) {
    const opts = { ...DEFAULTS, ...(options || {}) };
    this._endpoint = String(opts.endpoint || '').replace(/\/+$/, '');
    this._token = String(opts.token || '');
    this._app = opts.app || {};
    this._opts = opts;
    this._fetch = opts.fetch || globalThis.fetch;

    this._buffer = [];
    this._droppedByApp = 0;
    this._sequence = 1;
    this._connected = false;
    this._stopped = false;
    this._failures = 0;
    this._lastError = '';
    this._sending = false;
    this._timer = null;
    this._clockDomain =
      typeof globalThis.performance?.now === 'function'
        ? 'app.performance.now.ns'
        : 'app.wallclock.ns';
  }

  // Handshakes with the host. Resolves false rather than throwing: a failed
  // connection is a fact for the app to log, not an exception in a render.
  async connect() {
    if (!this._endpoint || !this._token) {
      this._lastError = 'endpoint and token are required';
      return false;
    }
    const body = {
      ...this._app,
      sdk_version: this._opts.sdkVersion,
      clock_domain: this._clockDomain,
      app_clock_ns: nowNs(),
    };
    const result = await this._post('/sdk/v1/hello', body);
    if (!result.ok) {
      this._lastError = result.error;
      return false;
    }
    this._connected = true;
    this._failures = 0;
    if (typeof result.json?.next_sequence === 'number') {
      // Resynchronise with the host, which may have seen batches from a
      // previous runtime of this app.
      this._sequence = result.json.next_sequence;
    }
    this._startTimer();
    return true;
  }

  // Stops the flush timer and sends whatever is buffered.
  async disconnect() {
    this._stopTimer();
    await this.flush();
    this._connected = false;
  }

  // What the SDK is doing, for an app that wants to show or log it. It never
  // reports success it cannot back up.
  status() {
    return {
      connected: this._connected,
      stopped: this._stopped,
      // A flush already in flight. Without this, `flush()` returning false
      // for "busy" looked identical to "the host refused it".
      sending: this._sending,
      buffered: this._buffer.length,
      droppedByApp: this._droppedByApp,
      nextSequence: this._sequence,
      lastError: this._lastError,
      clockDomain: this._clockDomain,
    };
  }

  // --- markers ---------------------------------------------------------------

  screenMount(screen, payload) {
    this._add('screen_mount', { screen, payload });
  }
  screenUnmount(screen, payload) {
    this._add('screen_unmount', { screen, payload });
  }
  navigationBegin(screen, payload) {
    this._add('navigation_begin', { screen, payload });
  }
  navigationEnd(screen, durationNs, payload) {
    this._add('navigation_end', { screen, durationNs, payload });
  }
  // A cancelled navigation is its own marker. "Cancelled" and "never
  // finished" are different facts, and collapsing them would make an
  // abandoned screen look like a hang (spec G07).
  navigationCancel(screen, payload) {
    this._add('navigation_cancel', { screen, payload });
  }
  interactionBegin(interaction, payload) {
    this._add('interaction_begin', { interaction, payload });
  }
  interactionEnd(interaction, durationNs, payload) {
    this._add('interaction_end', { interaction, durationNs, payload });
  }
  asyncSpanBegin(name, payload) {
    this._add('async_span_begin', { interaction: name, payload });
  }
  asyncSpanEnd(name, durationNs, payload) {
    this._add('async_span_end', { interaction: name, durationNs, payload });
  }
  // React's own commit data, which a sampling profiler cannot produce:
  // DET-10 needs render counts, not stacks that happen to be in React.
  reactCommit({ screen, component, commitCount, durationNs }) {
    this._add('react_commit', {
      screen,
      interaction: component,
      durationNs,
      payload: { commit_count: commitCount },
    });
  }
  // The URL is redacted before it leaves the app.
  networkRequest({ url, method, status, durationNs, screen }) {
    this._add('network_request', {
      screen,
      durationNs,
      payload: {
        url: redactUrl(url),
        method: typeof method === 'string' ? method : '',
        status: typeof status === 'number' ? status : null,
      },
    });
  }
  // A reload or Fast Refresh: markers before and after came from different
  // bundles and are not interchangeable (spec G04, G05).
  runtimeReload(bundleId) {
    this._add('runtime_reload', { payload: { js_bundle_id: bundleId || '' } });
  }

  // --- internals -------------------------------------------------------------

  _add(kind, { screen, interaction, durationNs, payload } = {}) {
    if (this._stopped) return;
    if (!KINDS.has(kind)) {
      this._lastError = `unknown marker kind '${kind}'`;
      return;
    }
    const marker = { kind, timestamp_ns: nowNs() };
    if (typeof screen === 'string' && screen) marker.screen = screen;
    if (typeof interaction === 'string' && interaction) {
      marker.interaction = interaction;
    }
    if (typeof durationNs === 'number' && durationNs >= 0) {
      marker.duration_ns = Math.round(durationNs);
    }
    if (payload && typeof payload === 'object') marker.payload = payload;

    if (this._buffer.length >= this._opts.bufferSize) {
      // The oldest goes, and the loss is counted so the host learns about it
      // with the next batch. A dropped marker must never look like an event
      // that did not happen.
      this._buffer.shift();
      this._droppedByApp += 1;
    }
    this._buffer.push(marker);
    if (this._buffer.length >= this._opts.batchSize) {
      // Fire and forget: flushing must not make the caller wait.
      void this.flush();
    }
  }

  async flush() {
    if (this._stopped || !this._connected) return false;
    if (this._sending || this._buffer.length === 0) return false;
    this._sending = true;
    try {
      const batch = this._buffer.slice(0, this._opts.batchSize);
      const dropped = this._droppedByApp;
      const body = {
        sequence: this._sequence,
        markers: batch,
      };
      if (dropped > 0) body.dropped_by_app = dropped;

      const result = await this._post('/sdk/v1/markers', body);
      if (result.ok) {
        // Only now are they gone from the buffer: a failed send keeps them
        // for the next attempt rather than losing them quietly.
        this._buffer.splice(0, batch.length);
        this._droppedByApp -= dropped;
        this._failures = 0;
        this._sequence =
          typeof result.json?.next_sequence === 'number'
            ? result.json.next_sequence
            : this._sequence + 1;
        return true;
      }

      this._lastError = result.error;
      if (result.status === 429) {
        // The host is telling us to back off, with how long.
        const wait = Number(result.retryAfterMs) || 1000;
        this._restartTimer(wait);
        return false;
      }
      this._failures += 1;
      if (this._failures >= this._opts.maxConsecutiveFailures) {
        // Stop rather than retry forever. An app that cannot reach the host
        // should not spend its main thread on it.
        this._stopped = true;
        this._stopTimer();
        this._lastError =
          `stopped after ${this._failures} consecutive failures: ${result.error}`;
      }
      return false;
    } finally {
      this._sending = false;
    }
  }

  async _post(path, body) {
    const fetchFn = this._fetch;
    if (typeof fetchFn !== 'function') {
      return { ok: false, status: 0, error: 'no fetch implementation available' };
    }
    let timer = null;
    try {
      const controller =
        typeof AbortController === 'function' ? new AbortController() : null;
      if (controller) {
        timer = setTimeout(() => controller.abort(), this._opts.requestTimeoutMs);
      }
      const response = await fetchFn(`${this._endpoint}${path}`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          // The token the host generated for this capture. Loopback alone is
          // not authentication: any local process could reach the endpoint.
          Authorization: `Bearer ${this._token}`,
        },
        body: JSON.stringify(body),
        signal: controller ? controller.signal : undefined,
      });
      const text = await response.text();
      let json = null;
      try {
        json = text ? JSON.parse(text) : null;
      } catch {
        json = null;
      }
      if (!response.ok) {
        return {
          ok: false,
          status: response.status,
          error: json?.error || `HTTP ${response.status}`,
          retryAfterMs: response.headers?.get?.('Retry-After-Ms'),
          json,
        };
      }
      return { ok: true, status: response.status, json };
    } catch (err) {
      // Any transport failure, including the abort above.
      return { ok: false, status: 0, error: String(err && err.message ? err.message : err) };
    } finally {
      if (timer) clearTimeout(timer);
    }
  }

  _startTimer() {
    this._stopTimer();
    if (this._stopped) return;
    this._timer = setInterval(() => {
      void this.flush();
    }, this._opts.flushIntervalMs);
    // Do not hold the process open in Node; harmless in React Native.
    if (typeof this._timer?.unref === 'function') this._timer.unref();
  }

  _restartTimer(delayMs) {
    this._stopTimer();
    if (this._stopped) return;
    this._timer = setTimeout(() => {
      this._startTimer();
      void this.flush();
    }, delayMs);
    if (typeof this._timer?.unref === 'function') this._timer.unref();
  }

  _stopTimer() {
    if (this._timer) {
      clearInterval(this._timer);
      clearTimeout(this._timer);
      this._timer = null;
    }
  }
}

// --- trackNetwork(): Inspect for a release build -----------------------------
//
// A release build runs no inspector, so DevX's Inspect tab has nothing to
// attach to. This wraps the app's own `fetch` and `XMLHttpRequest` and sends
// what they did to DevX as the Network events a debugger would have received,
// with the bodies a debugger would have asked for. DevX shows them exactly as
// it shows a debug build's requests.
//
//   import { trackNetwork } from './mpi-sdk.js';
//
//   if (Config.DEVX_INSPECT === '1') {            // never in a store build
//     trackNetwork({
//       endpoint: 'http://127.0.0.1:47081',     // shown in DevX's Inspect tab
//       token: 'devx-...',                      // shown beside it
//       app: { app_identifier: 'com.example', app_version: '9.1',
//              platform: Platform.OS, device: DeviceInfo.getModel() },
//     });
//   }
//
// What it does and does not do, so nothing here surprises an app:
//
//   * It records only while DevX is listening. Until then it asks every few
//     seconds whether DevX is there and records nothing, so a request made
//     while nobody is looking is not kept anywhere.
//   * With `detail` (the default) headers and bodies are sent verbatim,
//     Authorization included -- the point is to see what the app sent. They go
//     to the endpoint given here and nowhere else.
//   * Only JavaScript requests: a WebView, a native networking module and the
//     image loader are not fetch or XMLHttpRequest.
//   * It never throws into the app, and every request goes through untouched:
//     a failure inside the tracker loses a record, never a request.
//   * `stop()` puts fetch and XMLHttpRequest back.

const NETWORK_DEFAULTS = {
  detail: true,
  // Per body. React Native's own debugger stops at the same size.
  maxBodyBytes: 512 * 1024,
  flushIntervalMs: 500,
  // How often to ask whether DevX is listening, while it is not.
  probeIntervalMs: 5000,
  requestTimeoutMs: 5000,
  // Events held while a batch is in flight; the oldest go first, counted.
  bufferEvents: 4000,
  // One batch. The host refuses a request over 4 MB.
  maxBatchBytes: 3 * 1024 * 1024,
  // URLs not to record: strings (prefixes) or RegExps.
  ignore: [],
  sdkVersion: '0.2.0',
};

const TEXT_TYPES = /json|text|xml|graphql|javascript|x-www-form-urlencoded/i;

function nowSeconds() {
  if (typeof globalThis.performance?.now === 'function') {
    return globalThis.performance.now() / 1000;
  }
  return Date.now() / 1000;
}

function capBody(text, max) {
  if (typeof text !== 'string') return undefined;
  if (text.length <= max) return text;
  return `${text.slice(0, max)}\n... (cut by the DevX SDK at ${max} of ${text.length} characters)`;
}

// Headers in any of the shapes fetch accepts, as a plain object.
function headerObject(headers) {
  const out = {};
  if (!headers) return out;
  try {
    if (typeof headers.forEach === 'function' && !Array.isArray(headers)) {
      headers.forEach((value, key) => {
        out[key] = String(value);
      });
    } else if (Array.isArray(headers)) {
      for (const pair of headers) {
        if (Array.isArray(pair) && pair.length >= 2) out[String(pair[0])] = String(pair[1]);
      }
    } else if (typeof headers === 'object') {
      for (const key of Object.keys(headers)) out[key] = String(headers[key]);
    }
  } catch {
    // A header object we cannot read is left out, not guessed at.
  }
  return out;
}

function parseRawHeaders(raw) {
  const out = {};
  if (typeof raw !== 'string') return out;
  for (const line of raw.split(/\r?\n/)) {
    const at = line.indexOf(':');
    if (at > 0) out[line.slice(0, at).trim()] = line.slice(at + 1).trim();
  }
  return out;
}

function headerValue(headers, name) {
  const want = name.toLowerCase();
  for (const key of Object.keys(headers)) {
    if (key.toLowerCase() === want) return headers[key];
  }
  return '';
}

// A request body as text, when it is text. React Native's FormData keeps its
// parts in `_parts` / `getParts()`; it is written out as multipart so a GraphQL
// upload's `operations` part reads as the operation it is. A file part shows
// its name and type, never its bytes. Anything binary is left out (undefined):
// a stand-in string would read as the body the app sent.
function requestBodyText(body, max) {
  if (body === undefined || body === null) return undefined;
  if (typeof body === 'string') return capBody(body, max);
  if (typeof URLSearchParams === 'function' && body instanceof URLSearchParams) {
    return capBody(body.toString(), max);
  }
  const parts =
    typeof body.getParts === 'function' ? body.getParts() : Array.isArray(body._parts) ? body._parts : null;
  if (parts) {
    const boundary = '----devx-sdk';
    let out = '';
    for (const part of parts) {
      // getParts(): { string | uri, headers }; _parts: [name, value].
      let name = '';
      let value = part;
      if (Array.isArray(part)) {
        [name, value] = part;
      } else if (part && part.headers) {
        const disposition = String(part.headers['content-disposition'] || '');
        const m = /name="([^"]*)"/.exec(disposition);
        name = m ? m[1] : '';
        value = part.string !== undefined ? part.string : part;
      }
      out += `--${boundary}\r\ncontent-disposition: form-data; name="${name}"`;
      if (value && typeof value === 'object') {
        const file = value.name || value.fileName || value.uri || 'file';
        out += `; filename="${file}"\r\ncontent-type: ${value.type || 'application/octet-stream'}\r\n\r\n[file ${value.uri || file}]\r\n`;
      } else {
        out += `\r\n\r\n${String(value)}\r\n`;
      }
    }
    return capBody(`${out}--${boundary}--\r\n`, max);
  }
  return undefined;
}

class NetworkTracker {
  constructor(options) {
    const opts = { ...NETWORK_DEFAULTS, ...(options || {}) };
    this._opts = opts;
    this._endpoint = String(opts.endpoint || '').replace(/\/+$/, '');
    this._token = String(opts.token || '');
    this._global = opts.global || globalThis;
    // Captured before anything is wrapped, so the tracker's own sends are
    // never recorded as the app's.
    this._fetch = this._global.fetch;
    this._XHR = this._global.XMLHttpRequest;
    this._connected = false;
    this._stopped = false;
    this._events = [];
    this._dropped = 0;
    this._sequence = 1;
    this._sending = false;
    this._nextId = 1;
    this._internal = 0;     // > 0 while the tracker itself is sending
    this._insideFetch = 0;  // > 0 while app fetch is creating its XHR
    this._lastError = '';
    this._restore = [];
    this._timer = null;
    this._probe = null;
  }

  start() {
    if (!this._endpoint || !this._token) {
      this._lastError = 'endpoint and token are required';
      return this;
    }
    this._wrapFetch();
    this._wrapXHR();
    this._timer = setInterval(() => {
      void this._flush();
    }, this._opts.flushIntervalMs);
    if (typeof this._timer?.unref === 'function') this._timer.unref();
    void this._hello();
    return this;
  }

  async stop() {
    if (this._stopped) return;
    await this._flush();
    this._stopped = true;
    this._connected = false;
    for (const undo of this._restore.reverse()) {
      try {
        undo();
      } catch {
        // Put back what can be put back.
      }
    }
    this._restore = [];
    if (this._timer) clearInterval(this._timer);
    if (this._probe) clearTimeout(this._probe);
    this._events = [];
  }

  status() {
    return {
      connected: this._connected,
      stopped: this._stopped,
      buffered: this._events.length,
      dropped: this._dropped,
      lastError: this._lastError,
    };
  }

  // --- recording -------------------------------------------------------------

  _ignored(url) {
    if (typeof url !== 'string') return true;
    if (url.startsWith(this._endpoint)) return true;
    for (const rule of this._opts.ignore || []) {
      if (typeof rule === 'string' && url.startsWith(rule)) return true;
      if (rule instanceof RegExp && rule.test(url)) return true;
    }
    return false;
  }

  _emit(method, params) {
    if (!this._connected || this._stopped) return;
    if (this._events.length >= this._opts.bufferEvents) {
      this._events.shift();
      this._dropped += 1;
    }
    this._events.push({ method, params });
  }

  _begin(method, url, headers, bodyText) {
    const requestId = `devx-${this._nextId++}`;
    const request = { method, url };
    if (this._opts.detail) {
      request.headers = headers;
      if (bodyText !== undefined) request.postData = bodyText;
    }
    this._emit('Network.requestWillBeSent', {
      requestId,
      timestamp: nowSeconds(),
      wallTime: Date.now() / 1000,
      request,
    });
    return requestId;
  }

  _response(requestId, url, status, headers) {
    const response = {
      url,
      status,
      mimeType: headerValue(headers, 'content-type').split(';')[0].trim(),
    };
    if (this._opts.detail) response.headers = headers;
    this._emit('Network.responseReceived', { requestId, timestamp: nowSeconds(), response });
  }

  _finished(requestId) {
    // -1: the size on the wire is not known from JavaScript.
    this._emit('Network.loadingFinished', {
      requestId,
      timestamp: nowSeconds(),
      encodedDataLength: -1,
    });
  }

  _failed(requestId, errorText) {
    this._emit('Network.loadingFailed', {
      requestId,
      timestamp: nowSeconds(),
      errorText: errorText || 'failed',
    });
  }

  _body(requestId, text, unavailable) {
    if (!this._opts.detail) return;
    if (typeof text === 'string') {
      this._emit('DevX.responseBody', {
        requestId,
        body: capBody(text, this._opts.maxBodyBytes),
        base64Encoded: false,
      });
    } else {
      this._emit('DevX.responseBody', { requestId, unavailable });
    }
  }

  _wrapFetch() {
    const g = this._global;
    const original = g.fetch;
    if (typeof original !== 'function') return;
    const tracker = this;
    const wrapped = function devxFetch(input, init) {
      let requestId = null;
      let url = '';
      try {
        url = typeof input === 'string' ? input : String(input?.url ?? input);
        if (tracker._connected && tracker._internal === 0 && !tracker._ignored(url)) {
          const method = String(init?.method || input?.method || 'GET').toUpperCase();
          const headers = headerObject(init?.headers || input?.headers);
          const body = requestBodyText(init?.body, tracker._opts.maxBodyBytes);
          requestId = tracker._begin(method, url, headers, body);
        }
      } catch {
        requestId = null;
      }
      let promise;
      // A fetch built on XMLHttpRequest (React Native's is) creates and sends
      // its XHR synchronously, inside this call: the counter keeps that XHR
      // from being recorded a second time.
      tracker._insideFetch += 1;
      try {
        promise = original.apply(this, arguments);
      } finally {
        tracker._insideFetch -= 1;
      }
      if (requestId === null || !promise || typeof promise.then !== 'function') return promise;
      return promise.then(
        (response) => {
          try {
            const headers = headerObject(response.headers);
            tracker._response(requestId, response.url || url, response.status, headers);
            tracker._readFetchBody(requestId, response, headers);
          } catch {
            tracker._finished(requestId);
          }
          return response;
        },
        (error) => {
          try {
            tracker._failed(requestId, String(error?.message || error));
          } catch {
            // nothing to do
          }
          throw error;
        },
      );
    };
    g.fetch = wrapped;
    this._restore.push(() => {
      if (g.fetch === wrapped) g.fetch = original;
    });
  }

  _readFetchBody(requestId, response, headers) {
    const type = headerValue(headers, 'content-type');
    if (!this._opts.detail) {
      this._finished(requestId);
      return;
    }
    if (type && !TEXT_TYPES.test(type)) {
      this._finished(requestId);
      this._body(requestId, undefined, `a ${type} response, which is not text`);
      return;
    }
    let copy;
    try {
      copy = response.clone();
    } catch {
      this._finished(requestId);
      this._body(requestId, undefined, 'the response could not be copied to read it');
      return;
    }
    copy.text().then(
      (text) => {
        this._finished(requestId);
        this._body(requestId, text);
      },
      (error) => {
        this._failed(requestId, String(error?.message || error));
      },
    );
  }

  _wrapXHR() {
    const XHR = this._XHR;
    const proto = XHR?.prototype;
    if (!proto || typeof proto.open !== 'function' || typeof proto.send !== 'function') return;
    const tracker = this;
    const open = proto.open;
    const send = proto.send;
    const setRequestHeader = proto.setRequestHeader;

    proto.open = function devxOpen(method, url) {
      try {
        // Created by an app fetch already recorded, or by the tracker itself.
        const skip = tracker._insideFetch > 0 || tracker._internal > 0 || tracker._ignored(String(url));
        this.__devx = skip ? null : { method: String(method || 'GET').toUpperCase(), url: String(url), headers: {} };
      } catch {
        this.__devx = null;
      }
      return open.apply(this, arguments);
    };
    proto.setRequestHeader = function devxSetRequestHeader(key, value) {
      try {
        if (this.__devx) this.__devx.headers[String(key)] = String(value);
      } catch {
        // nothing to do
      }
      return setRequestHeader.apply(this, arguments);
    };
    proto.send = function devxSend(body) {
      try {
        const r = this.__devx;
        if (r && tracker._connected) {
          tracker._trackXHR(this, r, body);
        }
      } catch {
        // The request still goes.
      }
      return send.apply(this, arguments);
    };
    this._restore.push(() => {
      if (proto.open.name === 'devxOpen') proto.open = open;
      if (proto.send.name === 'devxSend') proto.send = send;
      if (proto.setRequestHeader.name === 'devxSetRequestHeader') proto.setRequestHeader = setRequestHeader;
    });
  }

  _trackXHR(xhr, r, body) {
    const requestId = this._begin(r.method, r.url, r.headers, requestBodyText(body, this._opts.maxBodyBytes));
    let responded = false;
    let settled = false;
    const respond = () => {
      if (responded) return;
      responded = true;
      let headers = {};
      try {
        headers = parseRawHeaders(xhr.getAllResponseHeaders());
      } catch {
        headers = {};
      }
      this._response(requestId, xhr.responseURL || r.url, xhr.status, headers);
    };
    const settle = (fn) => {
      if (settled) return;
      settled = true;
      try {
        fn();
      } catch {
        // A lost record, not a lost request.
      }
    };
    const listen = (name, fn) => {
      if (typeof xhr.addEventListener === 'function') xhr.addEventListener(name, fn);
    };
    listen('readystatechange', () => {
      if (xhr.readyState >= 2) respond();
    });
    listen('load', () =>
      settle(() => {
        respond();
        this._finished(requestId);
        const type = xhr.responseType || '';
        if (type === '' || type === 'text') {
          this._body(requestId, xhr.responseText);
        } else if (type === 'json') {
          this._body(requestId, JSON.stringify(xhr.response));
        } else {
          this._body(requestId, undefined, `a ${type} response, which is not text`);
        }
      }),
    );
    listen('error', () => settle(() => this._failed(requestId, 'network error')));
    listen('timeout', () => settle(() => this._failed(requestId, 'timed out')));
    listen('abort', () => settle(() => this._failed(requestId, 'aborted')));
  }

  // --- transport -------------------------------------------------------------

  async _hello() {
    if (this._stopped) return;
    const app = this._opts.app || {};
    const result = await this._post('/sdk/v1/network/hello', {
      ...app,
      sdk_version: this._opts.sdkVersion,
      detail: Boolean(this._opts.detail),
    });
    if (this._stopped) return;
    if (result.ok) {
      this._connected = true;
      this._sequence = typeof result.json?.next_sequence === 'number' ? result.json.next_sequence : 1;
      this._lastError = '';
      return;
    }
    this._lastError = result.error;
    this._probe = setTimeout(() => {
      void this._hello();
    }, this._opts.probeIntervalMs);
    if (typeof this._probe?.unref === 'function') this._probe.unref();
  }

  _disconnect(error) {
    // DevX stopped listening. What was buffered was for that session; it is
    // not kept for a later one.
    this._connected = false;
    this._events = [];
    this._lastError = error;
    if (!this._stopped) {
      this._probe = setTimeout(() => {
        void this._hello();
      }, this._opts.probeIntervalMs);
      if (typeof this._probe?.unref === 'function') this._probe.unref();
    }
  }

  async _flush() {
    if (!this._connected || this._sending || this._events.length === 0) return false;
    this._sending = true;
    try {
      const batch = [];
      let bytes = 0;
      for (const event of this._events) {
        let size = 0;
        try {
          size = JSON.stringify(event).length;
        } catch {
          size = 0;
        }
        if (batch.length > 0 && (bytes + size > this._opts.maxBatchBytes || batch.length >= 1000)) break;
        batch.push(event);
        bytes += size;
      }
      const dropped = this._dropped;
      const body = { sequence: this._sequence, events: batch };
      if (dropped > 0) body.dropped_by_app = dropped;
      const result = await this._post('/sdk/v1/network', body);
      if (result.ok) {
        this._events.splice(0, batch.length);
        this._dropped -= dropped;
        this._sequence =
          typeof result.json?.next_sequence === 'number' ? result.json.next_sequence : this._sequence + 1;
        return true;
      }
      this._disconnect(result.error);
      return false;
    } finally {
      this._sending = false;
    }
  }

  async _post(path, body) {
    const fetchFn = this._fetch;
    if (typeof fetchFn !== 'function') {
      return { ok: false, status: 0, error: 'no fetch implementation available' };
    }
    let timer = null;
    let promise;
    try {
      const controller = typeof AbortController === 'function' ? new AbortController() : null;
      if (controller) timer = setTimeout(() => controller.abort(), this._opts.requestTimeoutMs);
      // Marked as the tracker's own for as long as fetch is creating its
      // XHR, which is synchronous; the URL check covers the rest.
      this._internal += 1;
      try {
        promise = fetchFn.call(this._global, `${this._endpoint}${path}`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${this._token}` },
          body: JSON.stringify(body),
          signal: controller ? controller.signal : undefined,
        });
      } finally {
        this._internal -= 1;
      }
      const response = await promise;
      const text = await response.text();
      let json = null;
      try {
        json = text ? JSON.parse(text) : null;
      } catch {
        json = null;
      }
      if (!response.ok) {
        return { ok: false, status: response.status, error: json?.error || `HTTP ${response.status}`, json };
      }
      return { ok: true, status: response.status, json };
    } catch (err) {
      return { ok: false, status: 0, error: String(err && err.message ? err.message : err) };
    } finally {
      if (timer) clearTimeout(timer);
    }
  }
}

// Starts recording the app's fetch and XMLHttpRequest for DevX. Returns the
// tracker: `status()` says whether DevX is listening, `stop()` undoes it all.
// Never throws.
function trackNetwork(options) {
  try {
    return new NetworkTracker(options).start();
  } catch (err) {
    return {
      stop: async () => {},
      status: () => ({ connected: false, stopped: true, lastError: String(err) }),
    };
  }
}

export { redactUrl, KINDS, trackNetwork };
