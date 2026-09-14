// The sample app: a two-screen React Native app, instrumented.
//
// Every SDK call an app needs is here and nowhere else in the app's own
// logic -- that is the point of `instrumentation.js`. The profiler is a
// module the app imports, not something the app is built around, and deleting
// the import leaves a working app.
//
// This file is NOT built or run in this repository: it needs React Native,
// which means npm, Gradle and Xcode. README.md says exactly which parts of
// this sample have been executed and which have not. What *is* executed is
// `instrumentation.js`, against a live host, by `verify.mjs`.
import React, { useEffect, useState } from 'react';
import { Button, FlatList, Text, View } from 'react-native';
import { NavigationContainer } from '@react-navigation/native';
import { createNativeStackNavigator } from '@react-navigation/native-stack';

import {
  interactionFinished,
  interactionStarted,
  onNavigationState,
  screenLifecycle,
  startProfiling,
  stopProfiling,
  timedFetch,
} from './instrumentation';

const Stack = createNativeStackNavigator();

// The endpoint and token are printed by `mpi record --sdk`. They are read
// from the environment rather than committed: a token in source is a token
// in everyone's build, and this one grants write access to the capture.
const PROFILER = {
  endpoint: process.env.MPI_ENDPOINT,
  token: process.env.MPI_TOKEN,
  app: {
    app_identifier: 'io.example.sample',
    app_version: '1.0.0',
    build_configuration: __DEV__ ? 'Debug' : 'Release',
    js_engine: global.HermesInternal == null ? 'jsc' : 'hermes',
    // A build id the host can bind a source map to. Without it a stack is
    // symbolised against whatever map happens to be lying around, which is
    // how a trace ends up blaming the wrong line.
    js_bundle_id: process.env.MPI_BUNDLE_ID,
  },
};

function FeedScreen({ navigation }) {
  const [items, setItems] = useState([]);

  // One line per screen. The returned function is the unmount marker, which
  // is the shape React already expects -- so a mount cannot be reported
  // without its unmount being wired up at the same time.
  useEffect(() => screenLifecycle('Feed'), []);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      // `timedFetch` reports the app's own view of the request: DNS, TLS, a
      // cold radio and time queued behind other requests are all inside it.
      // DET-11 says so on every finding rather than blaming the server.
      const response = await timedFetch('https://example.invalid/feed');
      const json = await response.json().catch(() => ({ items: [] }));
      if (!cancelled) setItems(json.items ?? []);
    })().catch(() => {});
    return () => { cancelled = true; };
  }, []);

  return (
    <View>
      <FlatList
        data={items}
        keyExtractor={(item) => String(item.id)}
        renderItem={({ item }) => <Text>{item.title}</Text>}
      />
      <Button
        title="Checkout"
        onPress={() => {
          // The interaction is marked around the work it starts, so a stall
          // has something to be attributed to. A tap with no marker is a
          // stall with no name.
          interactionStarted('tap:checkout');
          navigation.navigate('Checkout');
          interactionFinished('tap:checkout');
        }}
      />
    </View>
  );
}

function CheckoutScreen() {
  useEffect(() => screenLifecycle('Checkout'), []);
  return <Text>Checkout</Text>;
}

export default function App() {
  const [routeName, setRouteName] = useState(null);

  useEffect(() => {
    void startProfiling(PROFILER);
    // Flushed on teardown: the buffer holds the last screen of the session,
    // and a capture missing it looks complete.
    return () => { void stopProfiling(); };
  }, []);

  return (
    <NavigationContainer
      onStateChange={(state) => {
        // React Navigation has no explicit cancel, so a route that leaves the
        // state without ever becoming active is reported as cancelled --
        // which is what it was. "Cancelled" and "never finished" are
        // different facts and the marker contract keeps them apart.
        setRouteName(onNavigationState(state, routeName));
      }}
    >
      <Stack.Navigator>
        <Stack.Screen name="Feed" component={FeedScreen} />
        <Stack.Screen name="Checkout" component={CheckoutScreen} />
      </Stack.Navigator>
    </NavigationContainer>
  );
}
