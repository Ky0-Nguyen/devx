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

export { redactUrl, KINDS };
