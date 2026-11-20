// The launcher's HTTPS request on macOS and iOS (launcher_platform.h): the
// update notice's one GET (update_check.h, Issue #365), through NSURLSession.
#import <Foundation/Foundation.h>

#include "launcher_platform.h"

namespace sfr::launcher {
std::optional<std::string> fetch_https(const std::string& url, const std::string& user_agent, int timeout_ms) {
    @autoreleasepool {
        NSURL* address = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!address || ![address.scheme isEqualToString:@"https"]) return std::nullopt;
        // Ephemeral: no cookies, cache or credentials are kept or sent.
        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.timeoutIntervalForRequest = timeout_ms / 1000.0;
        configuration.timeoutIntervalForResource = timeout_ms / 1000.0;
        configuration.HTTPShouldSetCookies = NO;
        configuration.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
        configuration.URLCache = nil;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration];
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:address];
        [request setValue:[NSString stringWithUTF8String:user_agent.c_str()] forHTTPHeaderField:@"User-Agent"];
        [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];

        __block std::optional<std::string> body;
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        NSURLSessionDataTask* task = [session dataTaskWithRequest:request
                                                completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
            NSHTTPURLResponse* http = [response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)response : nil;
            if (!error && http && http.statusCode == 200 && data && data.length <= (4u << 20))
                body = std::string(static_cast<const char*>(data.bytes), data.length);
            dispatch_semaphore_signal(finished);
        }];
        [task resume];
        // The session's own timeouts end the task; this wait only backs them up.
        const int64_t wait_ns = int64_t(timeout_ms + 2000) * int64_t(NSEC_PER_MSEC);
        if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, wait_ns)) != 0) {
            [task cancel];
            dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);  // the handler still runs once
        }
        [session finishTasksAndInvalidate];
        return body;
    }
}
}
