#include "net/tcp_session.h"

#include <asio.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

struct ConnectedSession {
    asio::io_context server_context;
    asio::io_context client_context;
    asio::ip::tcp::socket client_socket{client_context};
    net::TcpSessionPtr session;
    std::thread server_thread;

    ConnectedSession() {
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
        if (session && !session->IsClosed()) {
            session->Close();
        }

        std::error_code ignored;
        client_socket.close(ignored);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
};

std::array<std::uint8_t, 8> MakeRecord(std::uint32_t producer, std::uint32_t sequence) {
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
            for (std::uint32_t sequence = 0; sequence < records_per_producer; ++sequence) {
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
        if (producer >= producer_count || sequence >= records_per_producer) {
            throw std::runtime_error("received a corrupted record");
        }

        const std::size_t index =
            static_cast<std::size_t>(producer) * records_per_producer + sequence;
        if (seen[index]) {
            throw std::runtime_error("received a duplicate record");
        }
        seen[index] = true;
    }
}

void TestSendCloseRace() {
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

    std::thread reader([&]() {
        std::array<std::uint8_t, 4096> buffer;
        std::error_code ec;
        while (!ec) {
            connection.client_socket.read_some(asio::buffer(buffer), ec);
        }
    });

    while (send_attempts.load(std::memory_order_relaxed) < 1000) {
        std::this_thread::yield();
    }
    connection.session->Close();

    for (auto& producer : producers) {
        producer.join();
    }
    reader.join();

    if (!connection.session->IsClosed()) {
        throw std::runtime_error("session did not enter the closed state");
    }
}

} // namespace

int main() {
    try {
        TestConcurrentSend();
        TestSendCloseRace();
        std::cout << "tcp_session_concurrency_test passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "tcp_session_concurrency_test failed: " << e.what() << std::endl;
        return 1;
    }
}
