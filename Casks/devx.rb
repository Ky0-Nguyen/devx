cask "devx" do
  version "0.3.1"
  sha256 "6e1f58226f69a9b472b89e74714b4daaed69b48bb0753f3d78f914ee021237a6"

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
