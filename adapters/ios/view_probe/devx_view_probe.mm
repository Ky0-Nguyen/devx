// The layout probe: a dylib `mpi layout` injects into an app on an iOS
// simulator, so the app's real UIView tree can be read without anything being
// added to the app.
//
// How it gets in: `xcrun simctl launch` passes every `SIMCTL_CHILD_*`
// variable to the launched app, so `SIMCTL_CHILD_DYLD_INSERT_LIBRARIES` loads
// this file into the app's process at launch. The app bundle is not touched
// and nothing is installed on the simulator; this file stays on the Mac. It
// works only on a simulator: on a device, code signing refuses the insert.
//
// What it does: nothing, unless `DEVX_PROBE_SOCKET` names a socket path,
// which only `mpi` sets. Then it listens on that Unix socket, owner-only, and
// answers each connection with one JSON document describing every window's
// view tree and view-controller tree. The tree is read on the main thread,
// because UIKit is not thread-safe, so the app's UI pauses for as long as the
// walk takes; the report says it was taken with code injected, and it is never
// a performance measurement.
//
// Bounded: at most kMaxViews views are described, and a cut-off tree says so,
// because a screen with more than that is a finding in itself and a silently
// shortened tree would undercount it.
#import <UIKit/UIKit.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

namespace {

constexpr NSInteger kProbeVersion = 1;
constexpr NSInteger kMaxViews = 60000;
constexpr NSUInteger kMaxIdentifier = 120;

struct Walk {
  NSInteger views = 0;
  bool truncated = false;
};

NSString* Clip(NSString* s) {
  if (s.length <= kMaxIdentifier) return s;
  return [s substringToIndex:kMaxIdentifier];
}

NSArray* Rect(CGRect r) {
  return @[ @(r.origin.x), @(r.origin.y), @(r.size.width), @(r.size.height) ];
}

// An identity for a view controller that its root view can name too, so the
// two trees can be joined exactly: four controllers of one class are common
// (a tab bar's), and a class name alone would join the wrong one.
NSString* ControllerId(UIViewController* vc) {
  return [NSString stringWithFormat:@"%p", (__bridge void*)vc];
}

// Root views of the controllers described so far, so the view walk can tag
// each with its controller. Built from the controllers' side (`viewIfLoaded`)
// rather than from the view's next responder: a SwiftUI hosting controller's
// view does not answer to the responder chain the way UIKit's own do, and
// that join silently dropped every SwiftUI screen.
using ControllerByView = NSMutableDictionary<NSValue*, UIViewController*>;

NSValue* ViewKey(UIView* v) { return [NSValue valueWithNonretainedObject:v]; }

NSDictionary* DescribeView(UIView* v, UIWindow* window, NSInteger depth,
                           ControllerByView* owners, Walk* walk) {
  walk->views++;
  NSMutableDictionary* node = [NSMutableDictionary dictionary];
  node[@"class"] = NSStringFromClass(v.class);
  node[@"frame"] = Rect([v convertRect:v.bounds toView:window]);
  if (v.hidden) node[@"hidden"] = @YES;
  if (v.alpha < 1.0) node[@"alpha"] = @(v.alpha);
  if (UIViewController* vc = owners[ViewKey(v)]) {
    node[@"controller"] = NSStringFromClass(vc.class);
    node[@"controller_id"] = ControllerId(vc);
  }
  if (v.accessibilityIdentifier.length > 0) {
    node[@"identifier"] = Clip(v.accessibilityIdentifier);
  }
  NSArray<UIView*>* subviews = v.subviews;
  if (subviews.count > 0) {
    NSMutableArray* children = [NSMutableArray arrayWithCapacity:subviews.count];
    for (UIView* s in subviews) {
      if (walk->views >= kMaxViews) {
        walk->truncated = true;
        break;
      }
      [children addObject:DescribeView(s, window, depth + 1, owners, walk)];
    }
    node[@"children"] = children;
  }
  return node;
}

NSString* ControllerKind(UIViewController* vc) {
  if ([vc isKindOfClass:UINavigationController.class]) return @"navigation";
  if ([vc isKindOfClass:UITabBarController.class]) return @"tab";
  if ([vc isKindOfClass:UISplitViewController.class]) return @"split";
  if ([vc isKindOfClass:UIPageViewController.class]) return @"page";
  return @"content";
}

NSDictionary* DescribeController(UIViewController* vc, NSMutableSet* seen,
                                 ControllerByView* owners) {
  NSValue* key = [NSValue valueWithNonretainedObject:vc];
  if ([seen containsObject:key]) return nil;
  [seen addObject:key];
  if (UIView* root = vc.viewIfLoaded) owners[ViewKey(root)] = vc;

  NSMutableDictionary* node = [NSMutableDictionary dictionary];
  node[@"class"] = NSStringFromClass(vc.class);
  node[@"id"] = ControllerId(vc);
  node[@"kind"] = ControllerKind(vc);
  node[@"view_loaded"] = @(vc.isViewLoaded);
  node[@"visible"] = @(vc.isViewLoaded && vc.view.window != nil &&
                       !vc.view.hidden);
  if ([vc isKindOfClass:UINavigationController.class]) {
    NSMutableArray* stack = [NSMutableArray array];
    for (UIViewController* c in ((UINavigationController*)vc).viewControllers) {
      [stack addObject:NSStringFromClass(c.class)];
    }
    node[@"stack"] = stack;
  }
  if ([vc isKindOfClass:UITabBarController.class]) {
    node[@"selected_index"] = @(((UITabBarController*)vc).selectedIndex);
  }
  NSMutableArray* children = [NSMutableArray array];
  for (UIViewController* c in vc.childViewControllers) {
    if (NSDictionary* d = DescribeController(c, seen, owners)) {
      [children addObject:d];
    }
  }
  if (children.count > 0) node[@"children"] = children;
  // Only the controller that presented it reports a presented controller, so
  // a modal is described once, under the screen it covers.
  UIViewController* presented = vc.presentedViewController;
  if (presented != nil && presented.presentingViewController == vc) {
    if (NSDictionary* d = DescribeController(presented, seen, owners)) {
      node[@"presented"] = d;
    }
  }
  return node;
}

NSData* Snapshot() {
  __block NSData* data = nil;
  dispatch_sync(dispatch_get_main_queue(), ^{
    Walk walk;
    NSMutableArray* windows = [NSMutableArray array];
    NSMutableSet* seen = [NSMutableSet set];
    ControllerByView* owners = [NSMutableDictionary dictionary];
    CGRect screen = CGRectZero;
    CGFloat scale = 0;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if (![scene isKindOfClass:UIWindowScene.class]) continue;
      UIWindowScene* ws = (UIWindowScene*)scene;
      screen = ws.screen.bounds;
      scale = ws.screen.scale;
      for (UIWindow* w in ws.windows) {
        NSMutableDictionary* win = [NSMutableDictionary dictionary];
        win[@"class"] = NSStringFromClass(w.class);
        win[@"key"] = @(w.isKeyWindow);
        win[@"hidden"] = @(w.hidden);
        win[@"level"] = @(w.windowLevel);
        win[@"frame"] = Rect(w.frame);
        if (w.rootViewController != nil) {
          if (NSDictionary* c =
                  DescribeController(w.rootViewController, seen, owners)) {
            win[@"root_controller"] = c;
          }
        }
        win[@"root"] = DescribeView(w, w, 0, owners, &walk);
        [windows addObject:win];
      }
    }
    NSDictionary* doc = @{
      @"probe_version" : @(kProbeVersion),
      @"bundle_id" : NSBundle.mainBundle.bundleIdentifier ?: @"",
      @"pid" : @(getpid()),
      @"taken_at_ms" : @((long long)(NSDate.date.timeIntervalSince1970 * 1000)),
      @"screen" : @{
        @"width" : @(screen.size.width),
        @"height" : @(screen.size.height),
        @"scale" : @(scale)
      },
      @"max_views" : @(kMaxViews),
      @"views" : @(walk.views),
      @"truncated" : @(walk.truncated),
      @"windows" : windows,
    };
    data = [NSJSONSerialization dataWithJSONObject:doc options:0 error:nil];
  });
  return data;
}

void WriteAll(int fd, const void* bytes, size_t len) {
  const char* p = static_cast<const char*>(bytes);
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n <= 0) return;
    p += n;
    len -= static_cast<size_t>(n);
  }
}

void Serve(std::string path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) {
    NSLog(@"[devx-probe] socket path too long: %s", path.c_str());
    close(fd);
    return;
  }
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  unlink(path.c_str());
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
      chmod(path.c_str(), 0600) != 0 || listen(fd, 4) != 0) {
    NSLog(@"[devx-probe] cannot listen on %s: errno %d", path.c_str(), errno);
    close(fd);
    return;
  }
  NSLog(@"[devx-probe] listening on %s", path.c_str());
  for (;;) {
    int client = accept(fd, nullptr, nullptr);
    if (client < 0) continue;
    @autoreleasepool {
      NSData* data = Snapshot();
      if (data != nil) WriteAll(client, data.bytes, data.length);
    }
    close(client);
  }
}

}  // namespace

__attribute__((constructor)) static void DevxProbeStart() {
  const char* path = getenv("DEVX_PROBE_SOCKET");
  if (path == nullptr || path[0] == '\0') return;
  std::string copy(path);
  // Not passed on to anything this app launches: the probe is for this
  // process only.
  unsetenv("DYLD_INSERT_LIBRARIES");
  unsetenv("DEVX_PROBE_SOCKET");
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    Serve(copy);
  });
}
