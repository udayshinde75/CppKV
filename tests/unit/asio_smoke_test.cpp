// Smoke tests proving that Boost.Asio is found, compiles with our flags and
// can do real loopback networking on this platform. Server code comes in Phase 3.

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace asio = boost::asio;
using asio::ip::tcp;

namespace {

TEST(AsioSmoke, PostedHandlerRunsOnlyInsideRun) {
    asio::io_context io;
    bool ran = false;

    // post() queues the handler; it never runs inside post() itself.
    asio::post(io, [&ran] { ran = true; });
    EXPECT_FALSE(ran);

    // run() executes queued handlers on the calling thread and returns when
    // no work is left.
    const auto handlers_run = io.run();
    EXPECT_TRUE(ran);
    EXPECT_EQ(handlers_run, 1U);
}

TEST(AsioSmoke, LoopbackRoundTripOnEphemeralPort) {
    asio::io_context io;

    // Port 0 asks the OS for any free port, so parallel test runs never collide.
    tcp::acceptor acceptor{io, tcp::endpoint{asio::ip::address_v4::loopback(), 0}};
    const tcp::endpoint server_endpoint = acceptor.local_endpoint();
    ASSERT_NE(server_endpoint.port(), 0);

    // A blocking connect succeeds before accept() is called: the kernel
    // completes the TCP handshake and queues the connection in the listen
    // backlog. That is why this works on a single thread.
    tcp::socket client{io};
    client.connect(server_endpoint);
    tcp::socket server = acceptor.accept();

    asio::write(client, asio::buffer(std::string_view{"PING\n"}));

    std::string received;
    const std::size_t line_length = asio::read_until(server, asio::dynamic_buffer(received), '\n');

    // read_until may read past the delimiter; here the client sent exactly one line.
    EXPECT_EQ(line_length, 5U);
    EXPECT_EQ(received, "PING\n");
}

} // namespace
