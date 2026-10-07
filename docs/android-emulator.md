# Android emulators, without Android Studio

DevX's **Emulator** tab and `mpi emulator` install, create, run and show
Android virtual devices on a Mac with nothing else installed: no Android
Studio, no Java, no package manager.

```bash
mpi emulator sdk                                    # where the SDK is, what is in it, what runs
mpi emulator catalog                                # what can be installed for this Mac
mpi emulator license android-sdk-license            # read a license ...
mpi emulator license android-sdk-license --accept   # ... and accept it, yourself
mpi emulator install emulator
mpi emulator install "system-images;android-35;google_apis_playstore;arm64-v8a"
mpi emulator presets                                # standard screen sizes
mpi emulator create --name "Pixel 9" --image "system-images;android-35;google_apis_playstore;arm64-v8a" --preset pixel_9
mpi emulator start Pixel_9
mpi emulator screenshot Pixel_9 --out pixel9.png
mpi emulator stop Pixel_9
```

## What "nothing installed" means

An emulator cannot be written from scratch, so DevX runs **Google's own
emulator**, the one Android Studio runs. What changes is who fetches it:

- **The SDK.** An existing one is used when there is one (`ANDROID_HOME`,
  `ANDROID_SDK_ROOT`, then Android Studio's `~/Library/Android/sdk`).
  Otherwise DevX keeps its own in
  `~/Library/Application Support/DevX/android-sdk`.
- **The catalog** is Google's SDK manifests on `dl.google.com`, the same ones
  `sdkmanager` and Android Studio's SDK Manager read. DevX shows the stable
  channel, and only system images this Mac runs with acceleration (arm64 on
  Apple Silicon).
- **Installing** downloads with `/usr/bin/curl` (resuming a partial file),
  verifies the SHA-1 that Google's manifest states, unpacks with
  `/usr/bin/ditto`, and writes the `package.xml` that `sdkmanager` would, so
  Android Studio sees the package as installed.
- **Licenses** are never accepted on anyone's behalf. A package whose license
  has not been accepted is refused. The Install images panel and
  `mpi emulator license` show the full text, and only an explicit accept
  records it, as `sdkmanager` does: the SHA-1 of the trimmed text, appended
  to `<sdk>/licenses/<id>`. Google revises license wording from time to time,
  and a revised text needs accepting again.
- **Sizes**: the emulator is about 400 MB, platform-tools 15 MB, and a system
  image 1.4–2.3 GB.

## Virtual devices and screen sizes

An AVD is two plain files under `~/.android/avd`, so a device made in DevX
appears in Android Studio's Device Manager, and the other way round.

Presets come in two groups:

- **Reference sizes** for Android's window size classes: Small Phone
  (360×640 dp), Medium Phone (411×914 dp), Foldable unfolded (673×841 dp),
  Medium Tablet (1280×800 dp), Desktop (1920×1080 dp).
- **Real devices** whose pixel sizes are the SDK's own device skins: Pixel 9,
  9 Pro, 9 Pro XL, 8a, 6a, Pixel Tablet, and a 7" tablet.

Each preset shows its size in dp and its window size class in both
orientations: compact below 600 dp, medium below 840 dp, expanded above.

There are two ways to change a size:

| | how | restart |
|---|---|---|
| **Override now** | `adb shell wm size` / `wm density` | no; the hardware is unchanged, the app sees the new size |
| **Change the hardware** | edits `hw.lcd.width/height/density` | the next start, which is made a **cold boot** once, because a snapshot holds the old screen |

## Running and showing a device

`start` launches the emulator the way Android Studio's embedded mode does: its
own window hidden (`-qt-hide-window`), gRPC on a free loopback port behind a
token made for that launch (`-grpc-use-token`). It then waits until Android
reports itself booted. The emulator's output goes to
`<avd>/devx-emulator.log`, and a launch that fails quotes it.

The screen is drawn in DevX through the emulator's gRPC API
(`emulator/lib/emulator_controller.proto` in the SDK):

- **Frames** use the API's shared-memory transport: DevX owns a file, the
  emulator writes each RGBA frame into it, and the stream itself carries a
  few bytes per frame. Measured at about 47 frames a second for a 1080×2400
  device, against 539 MB per 5 s when the pixels travel in the stream.
- **Input**: a click is a tap, a drag is a swipe, scrolling scrolls, typing
  types, and Esc is Back. Input is sent off the UI thread.
- **Toolbar**: Back, Home, Recents, rotation, volume, power, screen size, a
  screenshot (kept as an observation for AI tools), and **Extended Controls**.
  That last one is the emulator's own panel (location, cellular, battery,
  camera, phone, D-pad, microphone, fingerprint, virtual sensors, bug report,
  record and playback), opened over gRPC. It is the same panel Android
  Studio opens, so nothing in it had to be rewritten.
- If the emulator stops or restarts under the view, DevX says so and
  reconnects when it is running again.
- **Several devices at once.** Each device shown is its own session (its
  own stream, frame file and input queue), so a phone and a tablet, or two
  Android versions, sit side by side, each with its own toolbar. *show* adds
  a device and *hide* takes it off screen while it keeps running. Measured
  with a Pixel 6a and a Pixel 9 streaming together.

The gRPC client, including HTTP/2, HPACK and protobuf, is written in this
repository (`core/net/`, `core/util/protobuf.*`) like everything else
(ADR-0002). HPACK is tested against RFC 7541's worked examples, and was also
checked against Node's nghttp2 for every printable character.

A running emulator is an ordinary adb device, so **Live, Record, Inspect and
Layout work on it** with nothing else to set up.

## Limits

- **Emulators started by Android Studio** use JWT authentication, which DevX
  cannot sign for. They are listed but cannot be shown. Stop one there and
  start it from DevX.
- **`stop`** flushes and pauses briefly before stopping, as `adb emu kill`
  is immediate. A change made in the last instant before stopping can still
  be lost if the next boot is cold, as it can from Android Studio.
- **Scrolling** is translated into a drag, which is close to, but not the
  same as, a mouse wheel on a device.
- **Rotated input** maps a click to device pixels in the frame's current
  orientation. That mapping was not exercised in landscape on every preset.
- **Apple Silicon only** for the system images DevX lists: arm64-v8a. On an
  Intel Mac it lists x86_64.
