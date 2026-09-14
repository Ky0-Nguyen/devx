// End-to-end test for the React Native SDK against the real host endpoint.
//
// It drives the actual client over real HTTP into the actual C++ ingest, via
// `mpi sdk-bridge`. A mock of the host would not exercise the two things that
// only exist as a pair: the sequence numbering that turns a lost batch into a
// recorded gap, and the backpressure the host applies when a session fills up.
//
// Run: node sdk/react-native/test/e2e.mjs [path-to-mpi]
//
// Exits non-zero on the first failed expectation, so it can sit in a smoke
// test without any framework.

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import process from 'node:process';
import { createInterface } from 'node:readline';

import { MpiSdk, redactUrl } from '../mpi-sdk.js';

const MPI = process.argv[2] || 'build/bin/mpi';

let failures = 0;
let checks = 0;

function check(condition, description) {
  checks += 1;
  if (condition) {
    console.log(`  ok   ${description}`);
  } else {
    failures += 1;
    console.log(`  FAIL ${description}`);
  }
}

function equal(actual, expected, description) {
  check(
    actual === expected,
    `${description} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`,
  );
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Starts `mpi sdk-bridge`, returns the endpoint plus a stop() that resolves
// with the bridge's own JSON summary.
async function startBridge(extraArgs = []) {
  const child = spawn(MPI, ['sdk-bridge', ...extraArgs], {
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const stdout = createInterface({ input: child.stdout });
  const lines = [];
  let resolveFirst;
  const first = new Promise((resolve) => {
    resolveFirst = resolve;
  });
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
      // Everything after the header line is the summary document.
      const body = lines.filter((l) => !l.startsWith('endpoint ')).join('\n');
      return JSON.parse(body);
    },
  };
}

async function testHappyPath() {
  console.log('== markers reach the host ==');
  const bridge = await startBridge();
  const sdk = new MpiSdk({
    endpoint: bridge.endpoint,
    token: bridge.token,
    flushIntervalMs: 50,
    app: {
      app_identifier: 'io.example.app',
      app_version: '4.2.0',
      build_configuration: 'Staging',
      js_engine: 'hermes',
      js_bundle_id: 'bundle-abc123',
      js_dev_mode: true,
    },
  });

  check(await sdk.connect(), 'the handshake succeeds');
  sdk.screenMount('Checkout');
  sdk.navigationBegin('Payment');
  sdk.navigationCancel('Payment');
  sdk.interactionBegin('tap-pay');
  sdk.interactionEnd('tap-pay', 42_000_000);
  sdk.reactCommit({ screen: 'Checkout', component: 'CartList', commitCount: 7 });
  sdk.networkRequest({
    url: 'https://api.example.com/cart?token=secret&q=shoes',
    method: 'GET',
    status: 200,
    durationNs: 120_000_000,
  });
  check(await sdk.flush(), 'the batch is accepted');
  await sdk.disconnect();

  const summary = await bridge.stop();
  equal(summary.markers, 7, 'all seven markers arrive');
  equal(summary.missing_batches, 0, 'no gap is reported');
  equal(summary.markers_rejected, 0, 'nothing is rejected');
  equal(summary.capability_status, 'available', 'the capability reads available');
  equal(
    summary.handshake.sdk_version,
    '0.1.0',
    'the host records which SDK produced the stream',
  );
  equal(
    summary.handshake.clock_domain,
    'app.performance.now.ns',
    'the clock domain the app used travels with the handshake',
  );
  // A cancelled navigation is its own kind, not a navigation that never ended.
  equal(summary.kinds.navigation_cancel, 1, 'the cancelled navigation is kept as such');
  equal(summary.kinds.react_commit, 1, 'React commit data arrives as its own kind');
}

async function testQueryStringsNeverLeaveTheApp() {
  console.log('== a URL is redacted before it is sent ==');
  equal(
    redactUrl('https://api.example.com/cart?token=secret&q=shoes'),
    'https://api.example.com/cart?<redacted>',
    'the query string is replaced, not forwarded',
  );
  equal(
    redactUrl('https://api.example.com/cart#frag'),
    'https://api.example.com/cart',
    'the fragment is dropped',
  );
  equal(redactUrl('https://api.example.com/cart'), 'https://api.example.com/cart',
    'a URL with no query survives unchanged');

  // And end to end: what the host stores must not contain the token.
  const bridge = await startBridge();
  const sdk = new MpiSdk({
    endpoint: bridge.endpoint,
    token: bridge.token,
    app: { sdk_version: '0.1.0', app_identifier: 'io.example.app' },
  });
  await sdk.connect();
  sdk.networkRequest({ url: 'https://api.example.com/p?access_token=hunter2' });
  await sdk.flush();
  await sdk.disconnect();
  const summary = await bridge.stop();
  equal(summary.markers, 1, 'the network marker arrives');
  check(
    !JSON.stringify(summary).includes('hunter2'),
    'the secret never appears in what the host reports',
  );
}

async function testALostBatchBecomesAGap() {
  console.log('== a lost batch is a recorded gap, not silence ==');
  const bridge = await startBridge();
  const sdk = new MpiSdk({
    endpoint: bridge.endpoint,
    token: bridge.token,
    app: { sdk_version: '0.1.0', app_identifier: 'io.example.app' },
  });
  await sdk.connect();
  sdk.screenMount('First');
  await sdk.flush();

  // Simulate a batch that left the app and never arrived: skip a sequence
  // number the way a dropped request would.
  sdk._sequence += 1;
  sdk.screenMount('Third');
  await sdk.flush();
  await sdk.disconnect();

  const summary = await bridge.stop();
  equal(summary.markers, 2, 'the two batches that did arrive are kept');
  equal(summary.missing_batches, 1, 'the host records the missing batch');
  check(
    summary.notes.some((n) => n.includes('not an absence of activity')),
    'the note says the gap is missing evidence rather than an idle app',
  );
}

async function testRetryIsNotCountedTwice() {
  console.log('== a retried batch is not stored twice ==');
  const bridge = await startBridge();
  const sdk = new MpiSdk({
    endpoint: bridge.endpoint,
    token: bridge.token,
    app: { sdk_version: '0.1.0', app_identifier: 'io.example.app' },
  });
  await sdk.connect();
  sdk.screenMount('Checkout');
  await sdk.flush();

  // The response was lost, so the client sends the same sequence again.
  sdk._sequence -= 1;
  sdk.screenMount('Checkout');
  await sdk.flush();
  await sdk.disconnect();

  const summary = await bridge.stop();
  equal(summary.markers, 1, 'the marker is stored once');
  equal(summary.duplicate_batches, 1, 'the duplicate is counted');
}

async function testBackpressureStopsTheClient() {
  console.log('== the host applies backpressure and the client obeys ==');
  const bridge = await startBridge(['--max-markers', '2']);
  const sdk = new MpiSdk({
    endpoint: bridge.endpoint,
    token: bridge.token,
    // Larger than what this test sends, so nothing auto-flushes underneath
    // the explicit flushes: a flush that returns false because another is
    // still in flight would look like a refusal.
    batchSize: 64,
    app: { sdk_version: '0.1.0', app_identifier: 'io.example.app' },
  });
  await sdk.connect();
  sdk.screenMount('One');
  sdk.screenMount('Two');
  check(await sdk.flush(), 'the first batch fills the session limit');
  check(!sdk.status().sending, 'no flush is left in flight');

  sdk.screenMount('Three');
  const accepted = await sdk.flush();
  check(!accepted, 'the refused batch is reported as not accepted');
  check(
    sdk.status().lastError.includes('marker limit'),
    `the client knows why it was refused (got "${sdk.status().lastError}")`,
  );
  check(
    sdk.status().buffered > 0,
    'the client keeps the refused markers instead of losing them',
  );
  await sdk.disconnect();

  const summary = await bridge.stop();
  if (process.env.MPI_DEBUG) console.log('   summary:', JSON.stringify(summary));
  equal(summary.markers, 2, 'the host stored only what it accepted');
  check(
    summary.notes.some((n) => n.includes('no SDK evidence')),
    'the capture records that its later part has no SDK evidence',
  );
}

async function testAWrongTokenIsRefused() {
  console.log('== authentication ==');
  const bridge = await startBridge();
  const sdk = new MpiSdk({
    endpoint: bridge.endpoint,
    token: 'not-the-token',
    app: { sdk_version: '0.1.0', app_identifier: 'io.example.app' },
  });
  check(!(await sdk.connect()), 'a wrong token cannot handshake');
  check(
    sdk.status().lastError.length > 0,
    'the client can tell the app why it failed',
  );
  const summary = await bridge.stop();
  equal(summary.markers, 0, 'nothing was stored');
  equal(
    summary.capability_status,
    'unsupported',
    'no handshake means no SDK evidence, not an app with nothing to report',
  );
}

async function testTheClientNeverThrowsAtAnUnreachableHost() {
  console.log('== an unreachable host ==');
  const sdk = new MpiSdk({
    // Nothing is listening here.
    endpoint: 'http://127.0.0.1:1',
    token: 'irrelevant',
    requestTimeoutMs: 300,
    app: { sdk_version: '0.1.0', app_identifier: 'io.example.app' },
  });
  let threw = false;
  try {
    await sdk.connect();
    sdk.screenMount('Checkout');
    await sdk.flush();
  } catch {
    threw = true;
  }
  check(!threw, 'the SDK never throws into the app');
  check(!sdk.status().connected, 'it reports that it is not connected');
}

const tests = [
  testHappyPath,
  testQueryStringsNeverLeaveTheApp,
  testALostBatchBecomesAGap,
  testRetryIsNotCountedTwice,
  testBackpressureStopsTheClient,
  testAWrongTokenIsRefused,
  testTheClientNeverThrowsAtAnUnreachableHost,
];

for (const test of tests) {
  try {
    await test();
  } catch (err) {
    failures += 1;
    console.log(`  FAIL ${test.name} threw: ${err && err.stack ? err.stack : err}`);
  }
}

console.log('');
console.log(`${checks - failures} passed, ${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
