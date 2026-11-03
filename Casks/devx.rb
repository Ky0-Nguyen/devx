cask "devx" do
  version "0.6.1"
  sha256 "db5aecd2eb1bc12f7c2e3713e0d1694d8c83e8e626d597d1d0ea8dbb3fb3ef0b"

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
end
