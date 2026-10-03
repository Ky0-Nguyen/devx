cask "devx" do
  version "0.2.0"
  sha256 "164ecd9d303fd9888361af72cd03093bc619d2529648f52590a2c189a2cf8c96"

  url "https://github.com/Ky0-Nguyen/devx/releases/download/#{version}/DevX-#{version}.dmg"
  name "DevX"
  desc "Mobile performance profiler for React Native apps on iOS and Android"
  homepage "https://github.com/Ky0-Nguyen/devx"

  depends_on arch: :arm64
  depends_on macos: :sonoma

  app "DevX.app"
  binary "#{appdir}/DevX.app/Contents/MacOS/mpi"

  zap trash: [
    "~/.mpi",
    "~/Library/Preferences/com.devx.plist",
    "~/Library/Saved Application State/com.devx.savedState",
  ]

  caveats <<~EOS
    This release is signed ad-hoc and is not notarized, so macOS blocks both
    DevX and `mpi` at first launch. Right-click DevX in Applications and
    choose Open; if `mpi` is still stopped, use System Settings > Privacy &
    Security > Open Anyway.
  EOS
end
