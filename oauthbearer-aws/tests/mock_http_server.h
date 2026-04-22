/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2026 Confluent Inc.
 * All rights reserved.
 *
 * BSD-2-Clause (see file header in project sources).
 */

/**
 * @brief Minimal in-process HTTP/1.1 server for testing.
 *
 * Binds to 127.0.0.1 on an auto-assigned port; accepts one connection
 * at a time and replies with a pre-set canned response. Designed for
 * single-threaded unit tests that point an HTTP client at it and
 * assert on request/response behaviour.
 *
 * Not production-grade: single-request serialised handling, no HTTPS,
 * minimal HTTP parsing. Sufficient to stand in for AWS STS in tests.
 */

#ifndef _RDKAFKA_OBA_MOCK_HTTP_SERVER_H_
#define _RDKAFKA_OBA_MOCK_HTTP_SERVER_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class MockHttpServer {
       public:
        MockHttpServer();
        ~MockHttpServer();

        /** Bind + start listening. Returns true on success. */
        bool Start();

        /** Stop the accept loop and join the worker thread. */
        void Stop();

        /** Full base URL, e.g. "http://127.0.0.1:54321". */
        std::string Endpoint() const;

        /** Configure the response the next (and subsequent) request(s)
         *  will receive. Thread-safe; may be called between requests. */
        void SetResponse(int status,
                         const std::string &content_type,
                         const std::string &body);

        /** Number of fully-handled requests since start. */
        int RequestCount() const;

        /** Body of the last request received. */
        std::string LastRequestBody() const;

       private:
        void AcceptLoop();
        void HandleOne(int client_fd);

        int listen_fd_{-1};
        int port_{0};
        std::atomic<bool> running_{false};
        std::thread accept_thread_;

        mutable std::mutex mu_;
        int status_{200};
        std::string content_type_{"text/xml"};
        std::string body_;
        int request_count_{0};
        std::string last_body_;
};

#endif /* _RDKAFKA_OBA_MOCK_HTTP_SERVER_H_ */
