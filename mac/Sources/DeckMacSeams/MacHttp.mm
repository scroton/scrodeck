// mac/Sources/DeckMacSeams/MacHttp.mm — core::Http on NSURLSession: one GET, 15 s at most (lib/usage.js's
// AbortController), the answer handed to the inbox so `done` runs on the deck's thread at the next pump.
// The ONE Objective-C++ file of the seams. UNBUILT, UNTESTED: written on Windows, never compiled.
// Windows counterpart: WinHttpGet in windows/host/winhttp.{hpp,cpp}. JS mirrored: `fetch` as lib/usage.js and
// lib/cloud.js use it (status, headers lower-cased, the body as text; a network failure is `error`, status 0).
// NOT WIRED BY DEFAULT (see MacCredentials.cpp).
#include "MacSeams.hpp"

#include <atomic>

#import <Foundation/Foundation.h>

namespace deck { namespace mac {

namespace {
class UrlSessionHttp : public core::Http {
 public:
  explicit UrlSessionHttp(std::shared_ptr<db::Inbox> inbox) : inbox_(std::move(inbox)) {
    NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];   // no cookies, no disk cache
    cfg.timeoutIntervalForRequest = 15; cfg.timeoutIntervalForResource = 15;
    cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    session_ = [NSURLSession sessionWithConfiguration:cfg];
  }
  ~UrlSessionHttp() override {
    alive_->store(false);                               // a request still in flight answers nobody
    [session_ invalidateAndCancel];
  }
  void get(const std::string& url, const std::map<std::string, std::string>& headers, std::function<void(const core::HttpResponse&)> done) override {
    std::shared_ptr<db::Inbox> inbox = inbox_;
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!u) { core::HttpResponse r; r.error = "fetch failed"; inbox->task([done, r] { done(r); }); return; }
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
    req.HTTPMethod = @"GET";
    for (const auto& kv : headers) [req setValue:[NSString stringWithUTF8String:kv.second.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:kv.first.c_str()]];
    NSURLSessionDataTask* task = [session_ dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
      core::HttpResponse r;
      if (error || ![response isKindOfClass:[NSHTTPURLResponse class]]) {
        r.error = "fetch failed";                       // lib/usage.js reads only "it failed"; the reason stays out of state files
      } else {
        NSHTTPURLResponse* h = (NSHTTPURLResponse*)response;
        r.status = (int)h.statusCode;
        for (NSString* k in h.allHeaderFields) {
          id v = h.allHeaderFields[k];
          if ([k isKindOfClass:[NSString class]] && [v isKindOfClass:[NSString class]]) r.headers[std::string([[k lowercaseString] UTF8String])] = std::string([(NSString*)v UTF8String]);
        }
        if (data.length) r.body.assign((const char*)data.bytes, (size_t)data.length);
      }
      // (URLSession's own queue) -> the deck's thread
      inbox->task([done, r, alive] { if (alive->load()) done(r); });
    }];
    [task resume];
  }
 private:
  std::shared_ptr<db::Inbox> inbox_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  NSURLSession* session_;                               // ARC (SwiftPM compiles .mm with -fobjc-arc — VERIFY)
};
}  // namespace

std::unique_ptr<core::Http> makeUrlSessionHttp(std::shared_ptr<db::Inbox> inbox) { return std::unique_ptr<core::Http>(new UrlSessionHttp(std::move(inbox))); }

}}  // namespace deck::mac
