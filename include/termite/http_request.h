#pragma once

#if termite_CURL

// Make Async HTTP requests
#include "bx/bx.h"
#include "bxx/string.h"

#define TEE_HTTP_OPERATION_TIMEOUT 28
#define TEE_HTTP_CERT_ERROR 58
#define TEE_HTTP_FAILED -1

// Fwd declare: Include "restclient-cpp/connection.h" to use this class
namespace RestClient
{
    class Connection;
    struct Response_t;
    typedef Response_t Response;
}

namespace tee
{
    struct HttpHeaderField
    {
        const char* name;
        const char* value;
    };

    // Http response callback. Gets called in the caller thread
    typedef void (*HttpResponseCallback)(int code, const char* body, const HttpHeaderField* headers, int numHeaders, 
                                         void* userData);
    typedef void (*HttpDownloadCallback)(int code, const MemoryBlock* mem, const char* filename, void* userData);

    // Note: return false to abort the download/upload
    typedef bool (*HttpProgressCallback)(size_t curSize, size_t totalSize, void* userData);

    // Connection callback is used for advanced requests , which you should include "restclient-cpp/connection.h" and use the methods
    // This function is called within async worker thread, so the user should only use 'conn' methods and work on userData in a thread-safe manner
    // User should return conn response back to async worker thread that will be reported in HttpResponseCallback
    // Example: conn->SetCertFile, conn->Set...., return conn->get(..);
    typedef const RestClient::Response& (*HttpConnectionCallback)(RestClient::Connection* conn, void* userData);

    namespace http {
        // Config
        TEE_API void setCert(const char* filepath, bool insecure = false);
        TEE_API void setKey(const char* filepath, const char* passphrase = nullptr);
        TEE_API void setTimeout(int timeoutSecs);
        TEE_API void setBaseUrl(const char* url);
        TEE_API void setDownloadBaseUrl(const char* url);
        TEE_API bool isRequestFailed(int code);
        TEE_API void setProgress(HttpProgressCallback progressFn, void* userData = nullptr);

        // Async requests
        TEE_API void get(const char* url, HttpResponseCallback responseFn, void* userData = nullptr,
                         HttpProgressCallback progressFn = nullptr, void* progressUserData = nullptr);
        TEE_API void post(const char* url, const char* contentType, const char* data, HttpResponseCallback responseFn, 
                          void* userData = nullptr,
                          HttpProgressCallback progressFn = nullptr, void* progressUserData = nullptr);
        TEE_API void post(const char* url, const char* contentType, const char* binaryData, const uint32_t dataSize,
                          HttpResponseCallback responseFn, void* userData = nullptr,
                          HttpProgressCallback progressFn = nullptr, void* progressUserData = nullptr);
        TEE_API void put(const char* url, const char* contentType, const char* data, 
                         HttpResponseCallback responseFn, void* userData = nullptr,
                         HttpProgressCallback progressFn = nullptr, void* progressUserData = nullptr);
        TEE_API void del(const char* url, HttpResponseCallback responseFn, void* userData = nullptr);
        TEE_API void head(const char* url, HttpResponseCallback responseFn, void* userData = nullptr);
        TEE_API void request(const char* url, HttpConnectionCallback connFn, 
                             HttpResponseCallback responseFn, void* userData = nullptr,
                             HttpProgressCallback progressFn = nullptr, void* progressUserData = nullptr);

        // Note: Download's base url is different from other request's base url, see `setDownloadBaseUrl`
        TEE_API void download(const char* url, HttpDownloadCallback downloadFn, void* userData = nullptr,
                              HttpProgressCallback progressFn = nullptr, void* progressUserData = nullptr);

        // Blocking (Sync) requests
        TEE_API void getSync(const char* url, HttpResponseCallback responseFn, void* userData = nullptr);
        TEE_API void postSync(const char* url, const char* contentType, const char* data, HttpResponseCallback responseFn, void* userData);
        TEE_API void putSync(const char* url, const char* contentType, const char* data, HttpResponseCallback responseFn,
                             void* userData = nullptr);

        TEE_API void delSync(const char* url, HttpResponseCallback responseFn, void* userData);
        TEE_API void headSync(const char* url, HttpResponseCallback responseFn, void* userData);
        TEE_API void requestSync(const char* url, HttpConnectionCallback connFn, HttpResponseCallback responseFn, void* userData);
    }
}

#endif
