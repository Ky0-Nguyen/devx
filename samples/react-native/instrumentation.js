// The sample app's instrumentation, in one file.
//
// This is what a real integration looks like: a module the app imports once,
// which owns the SDK instance and exposes the four things an app actually
// calls -- a screen mounted, a navigation started, a navigation ended or was
// cancelled, an interaction happened. Everything else about the SDK stays in
// here.
//
// It is plain JavaScript with no dependencies, which is what makes it
// runnable outside React Native -- and that matters, because it means the
// module the sample app imports is the same module this repository's tests
// execute against a live host. What is *not* exercised here is React Native
// itself: see README.md for exactly which parts of this sample have been run.
import { MpiSdk } from '../../sdk/react-native/mpi-sdk.js';

let sdk = null;
// Navigations in flight, so an end or a cancel can report a duration measured
// rather than estimated. Keyed by screen, because that is what a navigation
// is identified by in the marker contract.
const pendingNavigations = new Map();
const pendingInteractions = new Map();

/// Starts reporting. Returns false when no endpoint was configured, which is
/// the normal case in a build nobody is profiling -- and the app must keep
/// working exactly as before in that case. An SDK that made the app depend on
/// a profiler being attached would be worse than no SDK.
export async function startProfiling({ endpoint, token, app } = {}) {
  if (!endpoint || !token) return false;
  sdk = new MpiSdk({ endpoint, token, app });
  const ok = await sdk.connect();
  if (!ok) {
    // A host that is not listening is not an error in the app. The SDK is
    // dropped and the app carries on exactly as it would without it.
    // eslint-disable-next-line no-console
    console.warn(`[mpi] not reporting: ${sdk.status().lastError}`);
    sdk = null;
  }
  return ok;
}

/// Flushes and disconnects. Awaited, because the last screen of a session is
/// in the buffer and a capture missing it looks whole.
export async function stopProfiling() {
  if (sdk == null) return;
  await sdk.disconnect();
  sdk = null;
}

/// What the SDK is doing, including how many markers it dropped.
///
/// Worth surfacing somewhere in a debug build: the SDK drops rather than
/// growing without bound inside someone else's app, and a dropped marker is
/// evidence the capture will not have. Returning null rather than a
/// zero-filled object keeps "not reporting" distinct from "reporting
/// nothing".
export function profilingStatus() {
  return sdk == null ? null : sdk.status();
}

export function screenMounted(screen, payload) {
  sdk?.screenMount(screen, payload);
}

export function screenUnmounted(screen, payload) {
  sdk?.screenUnmount(screen, payload);
}

export function navigationStarted(screen, payload) {
  pendingNavigations.set(screen, nowNs());
  sdk?.navigationBegin(screen, payload);
}

export function navigationFinished(screen, payload) {
  const started = pendingNavigations.get(screen);
  pendingNavigations.delete(screen);
  // No begin means no duration. Reporting one anyway would be inventing a
  // measurement, so the marker goes without it.
  sdk?.navigationEnd(screen, started == null ? undefined : nowNs() - started,
                     payload);
}

/// A navigation the user abandoned.
///
/// Its own marker, not a missing end: "cancelled" and "never finished" are
/// different facts, and a screen the user backed out of is not a screen that
/// took forever to load.
export function navigationCancelled(screen, payload) {
  pendingNavigations.delete(screen);
  sdk?.navigationCancel(screen, payload);
}

export function interactionStarted(name, payload) {
  pendingInteractions.set(name, nowNs());
  sdk?.interactionBegin(name, payload);
}

export function interactionFinished(name, payload) {
  const started = pendingInteractions.get(name);
  pendingInteractions.delete(name);
  sdk?.interactionEnd(name, started == null ? undefined : nowNs() - started,
                      payload);
}

/// Wraps a fetch so its timing is reported.
///
/// The duration is the app's own view of the request: it includes DNS, the
/// TLS handshake, a cold radio, retries, and time the request spent queued
/// behind others in the app. DET-11 says all of that on every finding, so
/// what is reported here must not pretend to be server time.
export async function timedFetch(url, options) {
  const started = nowNs();
  try {
    const response = await fetch(url, options);
    sdk?.networkRequest({
      url,
      method: options?.method ?? 'GET',
      status: response.status,
      durationNs: nowNs() - started,
    });
    return response;
  } catch (error) {
    // A failed request is still a duration, and its absence would make the
    // slowest requests in a session the ones least likely to be reported.
    // A failed request is still a duration, and leaving it out would make
    // the slowest requests in a session the ones least likely to be
    // reported. There is no status, and `null` is what the marker carries --
    // not 0, which would read as a response.
    sdk?.networkRequest({
      url,
      method: options?.method ?? 'GET',
      durationNs: nowNs() - started,
    });
    throw error;
  }
}

/// A React `useEffect` body for a screen component, as a one-liner:
///
///     useEffect(() => screenLifecycle('Checkout'), []);
///
/// The returned function is the unmount marker, which is the shape React
/// already expects -- so the screen cannot report a mount without its
/// matching unmount being wired up at the same time.
export function screenLifecycle(screen, payload) {
  screenMounted(screen, payload);
  return () => screenUnmounted(screen, payload);
}

/// React Navigation's state, turned into markers.
///
/// Its listener fires on every state change and gives no explicit cancel, so
/// a navigation that disappears from the state without ever becoming the
/// active route is reported as cancelled -- which is what it was.
export function onNavigationState(state, previousRouteName) {
  const current = currentRouteName(state);
  if (current === previousRouteName) return previousRouteName;
  if (previousRouteName != null && pendingNavigations.has(previousRouteName)) {
    navigationCancelled(previousRouteName);
  }
  if (current != null) {
    navigationStarted(current);
    navigationFinished(current);
  }
  return current;
}

function currentRouteName(state) {
  if (state == null || !Array.isArray(state.routes)) return null;
  const route = state.routes[state.index ?? 0];
  if (route == null) return null;
  // A nested navigator's active route is the one the user is looking at.
  if (route.state != null) return currentRouteName(route.state);
  return route.name ?? null;
}

function nowNs() {
  // performance.now() is milliseconds with a fractional part; the marker
  // contract is nanoseconds. This is the app's own clock, which is why the
  // host refuses to compare these against device measurements without a
  // measured mapping.
  return Math.round(
    (typeof performance !== 'undefined' ? performance.now() : Date.now()) * 1e6);
}

// Exposed for the verification script, which drives these same functions.
export const _internals = { pendingNavigations, pendingInteractions };
