// Chromatron - macOS platform layer of the About dialog (compiled with -fobjc-arc).

#include "core/AboutPlatform.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace chromatron {
namespace platform {

bool httpsGet(const char* url, const char* userAgent, std::string& body, int& status, std::string& error)
{
    @autoreleasepool {
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:@(url)]];
        [request setValue:@(userAgent) forHTTPHeaderField:@"User-Agent"];
        [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
        request.timeoutInterval = 8;
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForResource = 10;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:config];

        // The completion runs on the session's own queue, so waiting here (even on the main
        // thread) cannot deadlock.
        __block NSData* data = nil;
        __block NSInteger code = 0;
        __block NSString* failure = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURLSessionDataTask* task =
            [session dataTaskWithRequest:request
                       completionHandler:^(NSData* d, NSURLResponse* response, NSError* err) {
                         data = d;
                         if ([response isKindOfClass:[NSHTTPURLResponse class]])
                             code = ((NSHTTPURLResponse*)response).statusCode;
                         if (err) failure = err.localizedDescription;
                         dispatch_semaphore_signal(done);
                       }];
        [task resume];
        const bool finished = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC)) == 0;
        [session finishTasksAndInvalidate];
        if (!finished) {
            [task cancel];
            error = "the request to GitHub timed out";
            return false;
        }
        if (failure) {
            error = failure.UTF8String;
            return false;
        }
        status = (int)code;
        body.assign((const char*)data.bytes, data.length);
        return true;
    }
}

void openUrl(const char* url)
{
    @autoreleasepool {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@(url)]];
    }
}

bool ask(const std::string& title, const std::string& message, const char* button1, const char* button2, int& result)
{
    // NSAlert must run on the main thread.
    __block NSModalResponse response = 0;
    void (^run)(void) = ^{
      @autoreleasepool {
          NSAlert* alert = [[NSAlert alloc] init];
          alert.messageText = @(title.c_str());
          alert.informativeText = @(message.c_str());
          [alert addButtonWithTitle:@(button1 ? button1 : "OK")];
          if (button2) [alert addButtonWithTitle:@(button2)];
          response = [alert runModal];
      }
    };
    if ([NSThread isMainThread]) run();
    else dispatch_sync(dispatch_get_main_queue(), run);
    result = response == NSAlertFirstButtonReturn ? 1 : response == NSAlertSecondButtonReturn ? 2 : 0;
    return true;
}

}  // namespace platform
}  // namespace chromatron
