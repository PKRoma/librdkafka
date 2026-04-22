/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2026 Confluent Inc.
 * All rights reserved.
 *
 * BSD-2-Clause (see file header in project sources).
 */

#include "mock_http_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace {
/* Native-path project memory (project_aws_outbound_federation.md) notes
 * that Linux kills test processes with SIGPIPE when writing to a half-
 * closed socket — macOS silently tolerates. Ignoring SIGPIPE once per
 * process lets the server be robust across both. */
void IgnoreSigpipeOnce() {
        static bool done = false;
        if (!done) {
                std::signal(SIGPIPE, SIG_IGN);
                done = true;
        }
}
}  // namespace

MockHttpServer::MockHttpServer() {
        IgnoreSigpipeOnce();
}

MockHttpServer::~MockHttpServer() {
        Stop();
}

bool MockHttpServer::Start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0)
                return false;

        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0; /* let kernel assign */

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr),
                   sizeof(addr)) < 0) {
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_,
                          reinterpret_cast<struct sockaddr *>(&addr),
                          &len) < 0) {
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
        }
        port_ = ntohs(addr.sin_port);

        if (::listen(listen_fd_, 4) < 0) {
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
        }

        running_       = true;
        accept_thread_ = std::thread(&MockHttpServer::AcceptLoop, this);
        return true;
}

void MockHttpServer::Stop() {
        if (!running_.exchange(false))
                return;
        if (listen_fd_ >= 0) {
                ::shutdown(listen_fd_, SHUT_RDWR);
                ::close(listen_fd_);
                listen_fd_ = -1;
        }
        if (accept_thread_.joinable())
                accept_thread_.join();
}

std::string MockHttpServer::Endpoint() const {
        std::ostringstream oss;
        oss << "http://127.0.0.1:" << port_;
        return oss.str();
}

void MockHttpServer::SetResponse(int status,
                                  const std::string &content_type,
                                  const std::string &body) {
        std::lock_guard<std::mutex> lk(mu_);
        status_       = status;
        content_type_ = content_type;
        body_         = body;
}

int MockHttpServer::RequestCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return request_count_;
}

std::string MockHttpServer::LastRequestBody() const {
        std::lock_guard<std::mutex> lk(mu_);
        return last_body_;
}

void MockHttpServer::AcceptLoop() {
        while (running_) {
                /* Short-timeout select so shutdown is responsive. */
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(listen_fd_, &rfds);
                struct timeval tv{0, 100 * 1000}; /* 100 ms */

                int r = ::select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv);
                if (r <= 0)
                        continue;
                if (!FD_ISSET(listen_fd_, &rfds))
                        continue;

                int client = ::accept(listen_fd_, nullptr, nullptr);
                if (client < 0)
                        continue;

                /* Reader timeout so a hung client can't hang the test. */
                struct timeval rto{2, 0};
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rto,
                           sizeof(rto));

                HandleOne(client);
                ::close(client);
        }
}

void MockHttpServer::HandleOne(int client_fd) {
        /* Read until we see "\r\n\r\n" marking end of headers, then
         * read Content-Length bytes of body. Naive but sufficient
         * for aws-sdk-cpp's well-formed STS POST. */
        std::string buf;
        buf.reserve(4096);
        char tmp[2048];
        size_t header_end = std::string::npos;

        while (header_end == std::string::npos) {
                ssize_t n = ::recv(client_fd, tmp, sizeof(tmp), 0);
                if (n <= 0)
                        return;
                buf.append(tmp, static_cast<size_t>(n));
                header_end = buf.find("\r\n\r\n");
                if (buf.size() > 1 * 1024 * 1024)
                        return; /* defensive: don't grow unbounded */
        }

        size_t body_start = header_end + 4;
        std::string headers = buf.substr(0, header_end);

        /* Parse Content-Length. */
        size_t content_length = 0;
        size_t p              = 0;
        while (p < headers.size()) {
                size_t eol = headers.find("\r\n", p);
                if (eol == std::string::npos)
                        eol = headers.size();
                std::string line = headers.substr(p, eol - p);
                if (line.size() >= 15 &&
                    ::strncasecmp(line.c_str(), "Content-Length:", 15) == 0) {
                        content_length = static_cast<size_t>(
                            ::strtoul(line.c_str() + 15, nullptr, 10));
                        break;
                }
                p = eol + 2;
        }

        while (buf.size() - body_start < content_length) {
                ssize_t n = ::recv(client_fd, tmp, sizeof(tmp), 0);
                if (n <= 0)
                        break;
                buf.append(tmp, static_cast<size_t>(n));
        }

        /* Build canned response. */
        std::ostringstream resp;
        int status;
        std::string ctype, body;
        {
                std::lock_guard<std::mutex> lk(mu_);
                status = status_;
                ctype  = content_type_;
                body   = body_;
                last_body_ =
                    buf.substr(body_start,
                               std::min(content_length,
                                        buf.size() - body_start));
                ++request_count_;
        }

        const char *reason =
            (status == 200) ? "OK" :
            (status == 400) ? "Bad Request" :
            (status == 403) ? "Forbidden" :
            (status == 500) ? "Internal Server Error" : "Error";
        resp << "HTTP/1.1 " << status << " " << reason << "\r\n"
             << "Content-Type: " << ctype << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << body;
        std::string s = resp.str();

        size_t sent = 0;
        while (sent < s.size()) {
#ifdef MSG_NOSIGNAL
                ssize_t n = ::send(client_fd, s.data() + sent, s.size() - sent,
                                   MSG_NOSIGNAL);
#else
                ssize_t n =
                    ::send(client_fd, s.data() + sent, s.size() - sent, 0);
#endif
                if (n <= 0)
                        break;
                sent += static_cast<size_t>(n);
        }
}
