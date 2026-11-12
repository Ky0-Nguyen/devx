import Foundation

/// Inspect for a release build: the endpoint and token the app's
/// `trackNetwork()` is built with, and what to tell the reader about the app
/// that connected.
///
/// The port and token are kept between launches. A release build has them
/// built in, so a new pair every run would mean a new build every run.
enum SdkNetwork {
    static let defaultPort = 47081
    static let portKey = "devx.sdkNetwork.port"
    static let tokenKey = "devx.sdkNetwork.token"

    /// A new token: "devx-" and 24 hex digits, which the listener accepts
    /// (16 to 128 letters, digits, '-' or '_').
    static func newToken() -> String {
        var bytes = [UInt8](repeating: 0, count: 12)
        for i in bytes.indices { bytes[i] = UInt8.random(in: 0...255) }
        return "devx-" + bytes.map { String(format: "%02x", $0) }.joined()
    }

    /// The listener's own rule, checked here so a hand-edited preference
    /// fails before anything is started.
    static func isValidToken(_ token: String) -> Bool {
        (16...128).contains(token.count) && token.allSatisfy {
            $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_")
        }
    }

    static func isValidPort(_ port: Int) -> Bool { (1024...65535).contains(port) }

    static func endpoint(port: Int) -> String { "http://127.0.0.1:\(port)" }

    static func adbReverse(port: Int) -> String { "adb reverse tcp:\(port) tcp:\(port)" }

    /// What goes into the app, ready to paste.
    static func snippet(port: Int, token: String) -> String {
        """
        import { Platform } from 'react-native';
        import { trackNetwork } from './mpi-sdk';

        // Only in a build made for inspecting: it sends headers and bodies,
        // tokens included, to DevX on this Mac.
        if (process.env.EXPO_PUBLIC_DEVX_INSPECT === '1') {
          trackNetwork({
            endpoint: '\(endpoint(port: port))',
            token: '\(token)',
            app: { app_identifier: 'com.example.app', platform: Platform.OS },
          });
        }
        """
    }

    /// The connected app, in one line: "com.kfc.app 9.1 · android · Pixel 8".
    static func appLine(_ app: JSON) -> String {
        guard !app.isNull else { return "" }
        let parts = [
            [app["app_identifier"].text, app["app_version"].text]
                .filter { !$0.isEmpty }.joined(separator: " "),
            app["build_configuration"].text,
            app["platform"].text,
            app["device"].text,
        ].filter { !$0.isEmpty }
        return parts.joined(separator: " · ")
    }
}
