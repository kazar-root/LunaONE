// SPDX-License-Identifier: GPL-3.0-only

#include "core/http_client.hpp"
#include "core/runtime_journal.hpp"

#import <Foundation/Foundation.h>
#import <Network/Network.h>

#if !__has_feature(objc_arc)
#error "The Apple HTTPS client requires ARC for objects shared with URLSession callbacks."
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>

@interface LunaHTTPSDelegate : NSObject <NSURLSessionTaskDelegate>
@end

@implementation LunaHTTPSDelegate
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                      newRequest:(NSURLRequest*)request
               completionHandler:(void (^)(NSURLRequest*))completionHandler
{
    NSURL* origin = response.URL;
    NSURL* destination = request.URL;
    const BOOL sameOrigin = [destination.scheme isEqualToString:@"https"] &&
        [origin.host.lowercaseString isEqualToString:destination.host.lowercaseString] &&
        (origin.port ?: @443).integerValue == (destination.port ?: @443).integerValue &&
        !destination.user && !destination.password;
    completionHandler(sameOrigin ? request : nil);
}
@end

namespace lunanx
{
namespace
{
NSString* Text(std::string_view value)
{
    return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

NSURL* HTTPSURL(const std::string& url)
{
    NSURL* result = [NSURL URLWithString:Text(url)];
    if (![result.scheme isEqualToString:@"https"] || result.host.length == 0 ||
        result.user || result.password)
        throw std::runtime_error("LunaCore requires an HTTPS URL.");
    return result;
}

NSURLSession* Session()
{
    static NSURLSession* session = [] {
        auto config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.HTTPCookieStorage = nil;
        config.HTTPShouldSetCookies = NO;
        config.URLCredentialStorage = nil;
        config.URLCache = nil;
        config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        return [NSURLSession sessionWithConfiguration:config
                                            delegate:[LunaHTTPSDelegate new] delegateQueue:nil];
    }();
    return session;
}

NSMutableURLRequest* MakeRequest(const std::string& method, const std::string& url,
    const std::string& agent, const std::vector<std::string>& headers, long timeout)
{
    auto request = [NSMutableURLRequest requestWithURL:HTTPSURL(url)
        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:timeout];
    request.HTTPMethod = Text(method);
    request.HTTPShouldHandleCookies = NO;
    [request setValue:Text(agent) forHTTPHeaderField:@"User-Agent"];
    for (const auto& header : headers)
    {
        const auto colon = header.find(':');
        if (colon == std::string::npos || colon == 0)
            continue;
        const std::string_view name(header.data(), colon);
        std::string_view value(header.data() + colon + 1, header.size() - colon - 1);
        const auto start = value.find_first_not_of(" \t");
        value = start != std::string_view::npos ? value.substr(start) : std::string_view {};
        [request setValue:Text(value) forHTTPHeaderField:Text(name)];
    }
    return request;
}

std::runtime_error TransportError(NSError* error)
{
    return std::runtime_error("HTTPS request failed (Apple network error " +
                              std::to_string(error.code) + ").");
}

void Wait(dispatch_semaphore_t done, NSURLSessionTask* task, long seconds,
    const std::function<void(long long, long long)>& progress = {},
    const std::function<bool()>& cancelled = {})
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    for (;;)
    {
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) == 0)
            return;
        const bool abort = cancelled && cancelled();
        if (abort || Clock::now() >= deadline)
        {
            [task cancel];
            // A download callback may already be moving the file. Wait for it to finish.
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            throw std::runtime_error(abort ? "Download cancelled." : "HTTPS request timed out.");
        }
        if (progress)
            progress(task.countOfBytesReceived, task.countOfBytesExpectedToReceive);
    }
}

} // namespace

HttpResponse HttpClient::Request(const std::string& method, const std::string& url,
    const std::string& user_agent, const std::vector<std::string>& headers, const std::string& body,
    long connect_timeout_seconds, long timeout_seconds) const
{
    @autoreleasepool
    {
        const auto started = std::chrono::steady_clock::now();
        const auto safe_url = SanitizeRuntimeUrl(url);
        const auto operation = BeginRuntimeOperation("http", method, "url=" + safe_url);
        auto request = MakeRequest(method, url, user_agent, headers, std::min(connect_timeout_seconds, timeout_seconds));
        if (!body.empty())
            request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];

        __block NSData* bytes = nil;
        __block NSHTTPURLResponse* response = nil;
        __block NSError* failure = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        auto task = [Session() dataTaskWithRequest:request
            completionHandler:^(NSData* data, NSURLResponse* result, NSError* error) {
                bytes = data;
                if ([result isKindOfClass:NSHTTPURLResponse.class])
                    response = (NSHTTPURLResponse*)result;
                failure = error;
                dispatch_semaphore_signal(done);
            }];
        [task resume];
        try
        {
            Wait(done, task, timeout_seconds);
            if (failure)
                throw TransportError(failure);
            if (!response)
                throw std::runtime_error("An HTTPS response was expected.");
        }
        catch (const std::exception& ex)
        {
            EndRuntimeOperation(operation, "http", method, "error", ex.what());
            throw;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        EndRuntimeOperation(operation, "http", method, "ok", "url=" + safe_url +
            " status=" + std::to_string(response.statusCode) + " bytes=" + std::to_string(bytes.length) +
            " elapsed_ms=" + std::to_string(elapsed));
        NSString* errorType = [response valueForHTTPHeaderField:@"x-amzn-ErrorType"];
        HttpResponse result;
        result.status_code = response.statusCode;
        if (bytes.length)
            result.body.assign(static_cast<const char*>(bytes.bytes), bytes.length);
        if (errorType.UTF8String)
            result.error_type = errorType.UTF8String;
        return result;
    }
}

HttpResponse HttpClient::Get(const std::string& url, const std::string& agent,
    const std::vector<std::string>& headers, long connect_timeout_seconds, long timeout_seconds) const
{
    return Request("GET", url, agent, headers, {}, connect_timeout_seconds, timeout_seconds);
}

HttpResponse HttpClient::Post(const std::string& url, const std::string& agent,
    const std::vector<std::string>& headers, const std::string& body) const
{
    return Request("POST", url, agent, headers, body);
}

void HttpClient::DownloadToFile(const std::string& url, const std::string& destination,
    const std::string& agent, const std::vector<std::string>& headers,
    const std::function<void(long long, long long)>& on_progress,
    const std::function<bool()>& is_cancelled) const
{
    @autoreleasepool
    {
        auto request = MakeRequest("GET", url, agent, headers, 30);
        auto target = [NSURL fileURLWithPath:Text(destination)];
        __block NSError* failure = nil;
        __block NSInteger status = 0;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        auto task = [Session() downloadTaskWithRequest:request
            completionHandler:^(NSURL* location, NSURLResponse* response, NSError* error) {
                failure = error;
                if ([response isKindOfClass:NSHTTPURLResponse.class])
                    status = [(NSHTTPURLResponse*)response statusCode];
                if (!error && status >= 200 && status < 300 &&
                    (!location || std::rename(location.fileSystemRepresentation, target.fileSystemRepresentation) != 0))
                    failure = [NSError errorWithDomain:NSPOSIXErrorDomain code:location ? errno : EIO userInfo:nil];
                dispatch_semaphore_signal(done);
            }];
        [task resume];
        Wait(done, task, 120, on_progress, is_cancelled);
        if (failure)
            throw TransportError(failure);
        if (status < 200 || status >= 300)
            throw std::runtime_error("HTTPS download failed (HTTP " + std::to_string(status) + ").");
    }
}

int HttpClient::MeasureTcpHandshakeLatencyMs(const std::string& url, long timeout_ms) const noexcept
{
    try
    {
        @autoreleasepool
        {
            const auto address = HTTPSURL(url);
            auto endpoint = nw_endpoint_create_host(address.host.UTF8String, (address.port ?: @443).stringValue.UTF8String);
            auto parameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
            auto connection = nw_connection_create(endpoint, parameters);
            if (!connection)
                return -1;
            auto queue = dispatch_queue_create("app.lunacore.tcp-probe", DISPATCH_QUEUE_SERIAL);
            nw_connection_set_queue(connection, queue);
            auto done = dispatch_semaphore_create(0);
            const auto start = std::chrono::steady_clock::now();
            __block int milliseconds = -1;
            nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t error) {
                if (state == nw_connection_state_ready)
                {
                    milliseconds = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count());
                    dispatch_semaphore_signal(done);
                }
                else if (state == nw_connection_state_failed)
                    dispatch_semaphore_signal(done);
            });
            nw_connection_start(connection);
            const int result = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, timeout_ms * NSEC_PER_MSEC)) == 0
                ? milliseconds : -1;
            nw_connection_cancel(connection);
            return result;
        }
    }
    catch (...)
    {
        return -1;
    }
}

} // namespace lunanx
