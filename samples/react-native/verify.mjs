// Drives the sample's instrumentation against a live host.
//
// The point is narrow and worth stating plainly: `App.js` is not built in this
// repository -- it needs React Native, npm, Gradle and Xcode. But
// `instrumentation.js`, the module it imports and where every SDK call lives,
// is plain JavaScript with no dependencies, so it can be exercised for real.
// This script calls the same functions the sample's screens call, against a
// `mpi sdk-bridge` it starts itself, and checks what the host received.
//
// What that verifies: the marker contract, the handshake, the transport, the
// navigation-cancel path, and -- first, because it is the case in every build
// nobody is profiling -- that a missing host leaves the app working.
//
// What it does not verify: anything about React Native, rendering, a device,
// or `App.js` itself.
//
//   node samples/react-native/verify.mjs [path-to-mpi]
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  interactionFinished,
  interactionStarted,
  navigationCancelled,
  navigationStarted,
  onNavigationState,
  profilingStatus,
  screenLifecycle,
  screenMounted,
  startProfiling,
  stopProfiling,
} from './instrumentation.js';

const here = dirname(fileURLToPath(import.meta.url));
const MPI = process.argv[2] ?? resolve(here, '../../build/bin/mpi');

let failures = 0;
let passed = 0;
function check(condition, label, detail = '') {
  if (condition) {
    passed += 1;
    console.log(`  ok   ${label}`);
  } else {
    failures += 1;
    console.log(`  FAIL ${label}${detail ? `\n       ${detail}` : ''}`);
  }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function startBridge() {
  const child = spawn(MPI, ['sdk-bridge'], { stdio: ['pipe', 'pipe', 'pipe'] });
  const stdout = createInterface({ input: child.stdout });
  const lines = [];
  let resolveFirst;
  const first = new Promise((r) => { resolveFirst = r; });
  stdout.on('line', (line) => {
    lines.push(line);
    if (line.startsWith('endpoint ')) resolveFirst(line);
  });
  child.stderr.on('data', () => {});
  const header = await Promise.race([first, sleep(5000).then(() => null)]);
  if (!header) throw new Error('the bridge did not print its endpoint');
  const [, endpoint, , token] = header.split(' ');
  return {
    endpoint,
    token,
    async stop() {
      child.stdin.end('stop\n');
      await once(child, 'exit');
      const body = lines.filter((l) => !l.startsWith('endpoint ')).join('\n');
      return JSON.parse(body);
    },
  };
}

// --- an app with no profiler attached keeps working -------------------------
console.log('== the instrumentation is inert without a host ==');
check((await startProfiling({})) === false,
      'no endpoint means no profiling, and no throw');
check(profilingStatus() === null,
      'status is null when not reporting, not a zero-filled object');
screenMounted('ScreenWithNoProfiler');
navigationStarted('Nowhere');
interactionStarted('tap:nothing');
check(true, 'every call is inert with no SDK attached');
await stopProfiling();
check(true, 'stopping when never started is not an error');

// --- connected --------------------------------------------------------------
console.log('== the sample\'s own calls reach the host ==');
const bridge = await startBridge();
const connected = await startProfiling({
  endpoint: bridge.endpoint,
  token: bridge.token,
  app: {
    app_identifier: 'io.example.sample',
    app_version: '1.0.0',
    build_configuration: 'Debug',
    js_engine: 'hermes',
    js_bundle_id: 'sample-bundle-1',
  },
});
check(connected === true, 'the handshake succeeds against a live host',
      connected ? '' : JSON.stringify(profilingStatus()));

if (connected) {
  const status = profilingStatus();
  check(status != null && status.connected === true, 'status reports connected');

  // A screen's whole lifecycle, the way a React effect does it.
  const unmountFeed = screenLifecycle('Feed');
  check(typeof unmountFeed === 'function',
        'screenLifecycle returns the unmount marker');

  interactionStarted('tap:checkout');
  await sleep(15);
  interactionFinished('tap:checkout');

  // The React Navigation adapter: Feed becomes active, then a state change
  // leaves a pending Checkout navigation behind.
  let route = onNavigationState({ index: 0, routes: [{ name: 'Feed' }] }, null);
  check(route === 'Feed', 'the active route is read from navigation state');
  navigationStarted('Checkout');
  route = onNavigationState({ index: 0, routes: [{ name: 'Feed' }] }, 'Checkout');
  check(route === 'Feed', 'a state change away from a pending route is seen');

  navigationStarted('Settings');
  navigationCancelled('Settings');

  const nested = onNavigationState({
    index: 0,
    routes: [{ name: 'Tabs',
               state: { index: 1, routes: [{ name: 'A' }, { name: 'B' }] } }],
  }, 'Feed');
  check(nested === 'B',
        `a nested navigator resolves to its own active route: got ${nested}`);

  unmountFeed();
  await stopProfiling();
  check(profilingStatus() === null, 'stopping clears the SDK');
}

// --- what the host received -------------------------------------------------
console.log('== the host\'s own account of it ==');
const summary = await bridge.stop();
const kinds = summary.marker_kinds ?? summary.kinds ?? {};
check(summary.handshake?.app_identifier === 'io.example.sample',
      'the host recorded the app that handshook',
      JSON.stringify(summary.handshake));
check(summary.handshake?.js_bundle_id === 'sample-bundle-1',
      'the bundle id travelled, so a source map can be bound to it');
check(summary.markers > 0, `markers arrived: ${summary.markers}`);
check((summary.missing_batches ?? 0) === 0,
      `no batch went missing: ${summary.missing_batches}`);
check((summary.markers_rejected ?? 0) === 0,
      `nothing was rejected: ${summary.markers_rejected}`,
      JSON.stringify(summary.notes));

for (const kind of ['screen_mount', 'screen_unmount', 'navigation_begin',
                    'navigation_end', 'interaction_begin',
                    'interaction_end']) {
  check((kinds[kind] ?? 0) > 0, `the host received ${kind}`,
        JSON.stringify(kinds));
}
// The cancel is worth checking on its own: a cancelled navigation arriving as
// a missing end would look like a screen that never finished loading.
check((kinds.navigation_cancel ?? 0) >= 2,
      'both cancels arrive as cancels, not as missing ends',
      JSON.stringify(kinds));

console.log(`\n${passed} passed, ${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
