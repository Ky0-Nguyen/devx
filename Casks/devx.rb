cask "devx" do
  version "0.5.3"
  sha256 "659390fb65001422b02f0c658507c34c8631897e8b66f7f395177d1fa72e89ad"

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
