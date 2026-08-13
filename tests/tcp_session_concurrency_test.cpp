#include "net/tcp_session.h"

#include <asio.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct ConnectedSession {
    typedef std::function<void(ConnectedSession&)> Setup;

    asio::io_context server_context;
    asio::io_context client_context;
    asio::ip::tcp::socket client_socket{client_context};
    net::TcpSessionPtr session;
    std::thread server_thread;

    explicit ConnectedSession(const Setup& setup = Setup()) {
        asio::ip::tcp::acceptor acceptor(
            server_context,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));

        std::thread connect_thread([this, &acceptor]() {
            client_socket.connect(acceptor.local_endpoint());
        });

        asio::ip::tcp::socket server_socket(server_context);
        acceptor.accept(server_socket);
        connect_thread.join();

        session = std::make_shared<net::TcpSession>(std::move(server_socket), 0);
        if (setup) {
            setup(*this);
        }

        std::promise<void> started;
        std::future<void> started_future = started.get_future();
        asio::post(server_context, [this, &started]() {
            session->Start();
            started.set_value();
        });
        server_thread = std::thread([this]() { server_context.run(); });
        started_future.get();
    }

    ~ConnectedSession() {
        std::error_code ignored;
        client_socket.close(ignored);
        if (session && !session->IsClosed()) {
            try {
                session->Close(std::chrono::milliseconds(100));
            } catch (...) {
            }
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
};

std::array<std::uint8_t, 8> MakeRecord(
        std::uint32_t producer, std::uint32_t sequence) {
    std::array<std::uint8_t, 8> record;
    for (int i = 0; i < 4; ++i) {
        record[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(producer >> ((3 - i) * 8));
        record[static_cast<std::size_t>(i + 4)] =
            static_cast<std::uint8_t>(sequence >> ((3 - i) * 8));
    }
    return record;
}

std::uint32_t ReadUint32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void TestConcurrentSend() {
    const std::uint32_t producer_count = 8;
    const std::uint32_t records_per_producer = 1000;
    const std::size_t record_size = 8;
    const std::size_t total_records =
        static_cast<std::size_t>(producer_count) * records_per_producer;

    ConnectedSession connection;
    std::vector<std::thread> producers;
    for (std::uint32_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer]() {
            for (std::uint32_t sequence = 0;
                 sequence < records_per_producer; ++sequence) {
                const auto record = MakeRecord(producer, sequence);
                connection.session->Send(record.data(), record.size());
            }
        });
    }

    std::vector<std::uint8_t> received(total_records * record_size);
    asio::read(connection.client_socket, asio::buffer(received));
    for (auto& producer : producers) {
        producer.join();
    }

    std::vector<bool> seen(total_records, false);
    for (std::size_t offset = 0; offset < received.size(); offset += record_size) {
        const std::uint32_t producer = ReadUint32(&received[offset]);
        const std::uint32_t sequence = ReadUint32(&received[offset + 4]);
        Require(producer < producer_count && sequence < records_per_producer,
                "received a corrupted record");
        const std::size_t index =
            static_cast<std::size_t>(producer) * records_per_producer + sequence;
        Require(!seen[index], "received a duplicate record");
        seen[index] = true;
    }
    Require(connection.session->Close(std::chrono::seconds(1)),
            "concurrent sends did not drain");
}

void TestActiveCloseDrainsAndSuppressesCallbacks() {
    std::atomic<int> message_calls{0};
    std::atomic<int> error_calls{0};
    std::atomic<int> close_calls{0};
    std::atomic<int> internal_close_calls{0};

    ConnectedSession connection([&](ConnectedSession& c) {
        c.session->SetOnMessage([&](net::TcpSessionPtr, const uint8_t*, std::size_t) {
            message_calls.fetch_add(1, std::memory_order_relaxed);
        });
        c.session->SetOnError([&](net::TcpSessionPtr, const std::error_code&) {
            error_calls.fetch_add(1, std::memory_order_relaxed);
        });
        c.session->SetOnClose([&](net::TcpSessionPtr) {
            close_calls.fetch_add(1, std::memory_order_relaxed);
        });
        c.session->SetInternalCloseHandler([&](net::TcpSessionPtr) {
            internal_close_calls.fetch_add(1, std::memory_order_relaxed);
        });
    });

    const std::vector<std::uint8_t> payload(2 * 1024 * 1024, 0x5a);
    connection.session->Send(payload.data(), payload.size());

    std::vector<std::uint8_t> received(payload.size());
    std::future<void> reader = std::async(std::launch::async, [&]() {
        asio::read(connection.client_socket, asio::buffer(received));
    });

    Require(connection.session->Close(std::chrono::seconds(2)),
            "active close unexpectedly forced the connection");
    reader.get();
    Require(received == payload, "active close returned before payload drained");
    Require(message_calls.load() == 0 && error_calls.load() == 0 && close_calls.load() == 0,
            "active close invoked a user callback");
    Require(internal_close_calls.load() == 1,
            "active close did not run internal cleanup exactly once");

    const int callbacks_after_close = message_calls.load() + error_calls.load() + close_calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Require(callbacks_after_close ==
                message_calls.load() + error_calls.load() + close_calls.load(),
            "a user callback ran after Close returned");
}

void TestCloseWaitsForRunningCallback() {
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_entered = false;
    bool release_callback = false;
    std::atomic<int> close_calls{0};

    ConnectedSession connection([&](ConnectedSession& c) {
        c.session->SetOnMessage([&](net::TcpSessionPtr, const uint8_t*, std::size_t) {
            std::unique_lock<std::mutex> lock(mutex);
            callback_entered = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return release_callback; });
        });
        c.session->SetOnClose([&](net::TcpSessionPtr) {
            close_calls.fetch_add(1, std::memory_order_relaxed);
        });
    });

    const std::uint8_t byte = 1;
    asio::write(connection.client_socket, asio::buffer(&byte, 1));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Require(cv.wait_for(lock, std::chrono::seconds(1), [&]() {
                    return callback_entered;
                }), "on_message did not start");
    }

    std::future<bool> closing = std::async(std::launch::async, [&]() {
        return connection.session->Close(std::chrono::milliseconds(20));
    });
    Require(closing.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
            "Close returned while on_message was still running");

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_callback = true;
    }
    cv.notify_all();
    Require(closing.get(), "callback barrier close did not drain");
    Require(close_calls.load() == 0, "active close invoked on_close");
}

void TestCloseFromIoThreadIsRejected() {
    std::promise<bool> rejected;
    std::atomic<bool> promise_set{false};

    ConnectedSession connection([&](ConnectedSession& c) {
        c.session->SetOnMessage([&](net::TcpSessionPtr session,
                                    const uint8_t*, std::size_t) {
            bool threw = false;
            try {
                session->Close(std::chrono::milliseconds(10));
            } catch (const std::logic_error&) {
                threw = true;
            }
            if (!promise_set.exchange(true)) {
                rejected.set_value(threw && !session->IsClosed());
            }
        });
    });

    const std::uint8_t byte = 2;
    asio::write(connection.client_socket, asio::buffer(&byte, 1));
    Require(rejected.get_future().get(),
            "Close from the session IO thread was not safely rejected");
    Require(connection.session->Close(std::chrono::seconds(1)),
            "external Close failed after rejected IO-thread Close");
}

void TestConcurrentCloseSharesOneBarrier() {
    std::atomic<int> internal_close_calls{0};
    ConnectedSession connection([&](ConnectedSession& c) {
        c.session->SetInternalCloseHandler([&](net::TcpSessionPtr) {
            internal_close_calls.fetch_add(1, std::memory_order_relaxed);
        });
    });

    std::array<bool, 8> results{{false, false, false, false, false, false, false, false}};
    std::vector<std::thread> closers;
    for (std::size_t i = 0; i < results.size(); ++i) {
        closers.emplace_back([&, i]() {
            results[i] = connection.session->Close(std::chrono::seconds(1));
        });
    }
    for (auto& closer : closers) {
        closer.join();
    }
    for (bool result : results) {
        Require(result, "concurrent Close callers observed inconsistent result");
    }
    Require(internal_close_calls.load() == 1,
            "concurrent Close ran internal cleanup more than once");
}

void TestConcurrentSendCloseRace() {
    ConnectedSession connection;
    std::atomic<std::size_t> send_attempts{0};
    std::vector<std::thread> producers;
    for (std::size_t producer = 0; producer < 8; ++producer) {
        producers.emplace_back([&]() {
            const std::array<std::uint8_t, 64> payload{{0}};
            for (std::size_t i = 0; i < 4000; ++i) {
                connection.session->Send(payload.data(), payload.size());
                send_attempts.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::future<void> reader = std::async(std::launch::async, [&]() {
        std::array<std::uint8_t, 4096> buffer;
        std::error_code ec;
        while (!ec) {
            connection.client_socket.read_some(asio::buffer(buffer), ec);
        }
    });

    while (send_attempts.load(std::memory_order_relaxed) < 1000) {
        std::this_thread::yield();
    }
    Require(connection.session->Close(std::chrono::seconds(2)),
            "accepted sends did not drain during concurrent Close");

    for (auto& producer : producers) {
        producer.join();
    }
    reader.get();
    Require(connection.session->IsClosed(),
            "concurrent Send/Close did not close the session");
}

void TestCloseTimeoutForcesSlowPeer() {
    ConnectedSession connection([&](ConnectedSession& c) {
        c.session->SetMaxSendQueueSize(64 * 1024 * 1024);
        c.session->GetSocket().set_option(asio::socket_base::send_buffer_size(4096));
        c.client_socket.set_option(asio::socket_base::receive_buffer_size(4096));
    });

    const std::vector<std::uint8_t> block(1024 * 1024, 0x7f);
    for (int i = 0; i < 32; ++i) {
        connection.session->Send(block.data(), block.size());
    }

    const auto begin = std::chrono::steady_clock::now();
    const bool drained = connection.session->Close(std::chrono::milliseconds(30));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    Require(!drained, "slow peer unexpectedly drained the full send queue");
    Require(elapsed < std::chrono::seconds(2), "forced Close did not complete promptly");
}

void TestNetworkCloseKeepsCallbacks() {
    std::mutex mutex;
    std::condition_variable cv;
    int error_calls = 0;
    int close_calls = 0;
    int internal_close_calls = 0;

    ConnectedSession connection([&](ConnectedSession& c) {
        c.session->SetOnError([&](net::TcpSessionPtr, const std::error_code&) {
            std::lock_guard<std::mutex> lock(mutex);
            ++error_calls;
            cv.notify_all();
        });
        c.session->SetOnClose([&](net::TcpSessionPtr) {
            std::lock_guard<std::mutex> lock(mutex);
            ++close_calls;
            cv.notify_all();
        });
        c.session->SetInternalCloseHandler([&](net::TcpSessionPtr) {
            std::lock_guard<std::mutex> lock(mutex);
            ++internal_close_calls;
            cv.notify_all();
        });
    });

    std::error_code ignored;
    connection.client_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    connection.client_socket.close(ignored);

    std::unique_lock<std::mutex> lock(mutex);
    Require(cv.wait_for(lock, std::chrono::seconds(1), [&]() {
                return internal_close_calls == 1;
            }), "network close did not finish");
    Require(error_calls == 1, "network close did not invoke on_error exactly once");
    Require(close_calls == 1, "network close did not invoke on_close exactly once");
}

} // namespace

int main() {
    try {
        TestConcurrentSend();
        TestActiveCloseDrainsAndSuppressesCallbacks();
        TestCloseWaitsForRunningCallback();
        TestCloseFromIoThreadIsRejected();
        TestConcurrentCloseSharesOneBarrier();
        TestConcurrentSendCloseRace();
        TestCloseTimeoutForcesSlowPeer();
        TestNetworkCloseKeepsCallbacks();
        std::cout << "tcp_session_concurrency_test passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "tcp_session_concurrency_test failed: " << e.what() << std::endl;
        return 1;
    }
}
