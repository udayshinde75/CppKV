#include "loadgen/load_generator.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cppkv::loadgen {

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kReadChunk = std::size_t{64} * 1024;
constexpr std::size_t kPopulateBatch = 1'000;
constexpr std::size_t kMaxOpenLoopBurst = 64;   // requests sent in one write when behind schedule
constexpr auto kStartDelay = std::chrono::milliseconds{100};
// After the window ends, outstanding replies must arrive within this time, or
// the run is reported as failed instead of waiting forever for a stuck server.
constexpr auto kDrainTimeout = std::chrono::seconds{10};

std::uint64_t to_nanos(Clock::duration d) {
    return static_cast<std::uint64_t>(std::max<std::chrono::nanoseconds::rep>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()));
}

// A blocking TCP connection that reads CRLF-terminated reply lines.
//
// Thread safety: in the open loop one thread calls write() while another calls
// read_line(). Asio documents synchronous send, receive and shutdown on one
// socket object as safe to call concurrently; buffer_ is used by the reader only.
class Connection {
public:
    Connection(asio::io_context& io, const tcp::resolver::results_type& endpoints) : socket_{io} {
        asio::connect(socket_, endpoints);
        socket_.set_option(tcp::no_delay{true});   // send each small request now, not after the previous ACK
    }

    void write(std::string_view bytes) { asio::write(socket_, asio::buffer(bytes.data(), bytes.size())); }

    // The next reply line without its CRLF, valid until the next call. Throws
    // boost::system::system_error on EOF or a socket error.
    std::string_view read_line() {
        for (;;) {
            const std::size_t end = buffer_.find("\r\n", scanned_);
            if (end != std::string::npos) {
                const std::string_view line{buffer_.data() + start_, end - start_};
                start_ = end + 2;
                scanned_ = start_;
                return line;
            }
            scanned_ = buffer_.size() > start_ ? buffer_.size() - 1 : start_;   // a CR may be the last byte
            if (start_ > 0) {   // drop consumed bytes before reading more
                buffer_.erase(0, start_);
                scanned_ -= start_;
                start_ = 0;
            }
            const std::size_t old_size = buffer_.size();
            buffer_.resize(old_size + kReadChunk);
            const std::size_t n = socket_.read_some(asio::buffer(buffer_.data() + old_size, kReadChunk));
            buffer_.resize(old_size + n);
        }
    }

    // Unblocks a thread waiting in read_line() or write(); safe from any thread.
    void shutdown() noexcept {
        boost::system::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
    }

private:
    tcp::socket socket_;
    std::string buffer_;
    std::size_t start_ = 0;     // first byte of the next line
    std::size_t scanned_ = 0;   // bytes before this are known not to start a CRLF
};

// Generates requests. Uses mt19937_64, whose output the C++ standard fixes, and
// plain modulo instead of std::uniform_int_distribution (which differs between
// standard libraries), so one seed produces the same requests on every platform.
class Workload {
public:
    Workload(const LoadOptions& options, std::uint64_t stream)
        : rng_{options.seed * 0x9E3779B97F4A7C15ULL + stream},
          keys_{options.keys},
          read_percent_{options.read_percent},
          value_(options.value_bytes, 'v') {}

    // Appends one request line; returns true for GET, false for SET.
    bool append_request(std::string& out) {
        const bool is_get = rng_() % 100 < read_percent_;
        out += is_get ? "GET " : "SET ";
        append_key(out, rng_() % keys_);
        if (!is_get) {
            out += ' ';
            out += value_;
        }
        out += '\n';
        return is_get;
    }

    void append_set(std::string& out, std::size_t key_index) const {
        out += "SET ";
        append_key(out, key_index);
        out += ' ';
        out += value_;
        out += '\n';
    }

private:
    // "key:0000001234", as in the micro-benchmarks.
    static void append_key(std::string& out, std::uint64_t index) {
        std::array<char, 20> digits{};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), index);
        const auto length = static_cast<std::size_t>(result.ptr - digits.data());
        out += "key:";
        out.append(length < 10 ? 10 - length : 0, '0');
        out.append(digits.data(), length);
    }

    std::mt19937_64 rng_;
    std::uint64_t keys_;
    unsigned read_percent_;
    std::string value_;
};

struct Times {
    Clock::time_point start;          // load begins (warm-up)
    Clock::time_point window_start;   // measuring begins
    Clock::time_point end;            // no request is sent or scheduled from here on
};

// What one connection measured. Merged into the LoadReport at the end.
struct ClientResult {
    LatencyHistogram latency;
    std::uint64_t requests = 0;
    std::uint64_t gets = 0;
    std::uint64_t sets = 0;
    std::uint64_t get_misses = 0;
    std::uint64_t errors = 0;
    std::string failure;          // written by the closed-loop or receiving thread
    // Written by the open-loop sending thread only (distinct members, no race).
    std::string sender_failure;
    LatencyHistogram send_lag;

    // Classifies one reply; `measured` replies are counted and timed.
    void on_reply(bool is_get, std::string_view line, bool measured, std::uint64_t latency_nanos) {
        if (line.starts_with("ERR BUSY")) {
            throw std::runtime_error{"server refused the connection: " + std::string{line}};
        }
        if (!measured) {
            return;
        }
        ++requests;
        latency.record(latency_nanos);
        if (is_get) {
            ++gets;
            if (line == "NIL") {
                ++get_misses;
            } else if (!line.starts_with("VALUE ")) {
                ++errors;
            }
        } else {
            ++sets;
            if (line != "OK") {
                ++errors;
            }
        }
    }
};

void populate(Connection& connection, const LoadOptions& options) {
    const Workload workload{options, 0};
    std::string batch;
    for (std::size_t first = 0; first < options.keys; first += kPopulateBatch) {
        const std::size_t last = std::min(options.keys, first + kPopulateBatch);
        batch.clear();
        for (std::size_t i = first; i < last; ++i) {
            workload.append_set(batch, i);
        }
        connection.write(batch);
        for (std::size_t i = first; i < last; ++i) {
            const std::string_view reply = connection.read_line();
            if (reply != "OK") {
                throw std::runtime_error{"populating the store failed: server replied '" + std::string{reply} + "'"};
            }
        }
    }
}

void closed_loop_client(Connection& connection, Workload& workload, const LoadOptions& options, const Times& times,
                        ClientResult& result) {
    std::this_thread::sleep_until(times.start);
    std::string batch;
    std::vector<bool> is_get(options.pipeline);
    while (Clock::now() < times.end) {
        batch.clear();
        for (unsigned k = 0; k < options.pipeline; ++k) {
            is_get[k] = workload.append_request(batch);
        }
        const Clock::time_point sent = Clock::now();
        connection.write(batch);
        const bool measured = sent >= times.window_start;
        for (unsigned k = 0; k < options.pipeline; ++k) {
            const std::string_view reply = connection.read_line();
            result.on_reply(is_get[k], reply, measured, to_nanos(Clock::now() - sent));
        }
    }
}

struct Scheduled {
    Clock::time_point due;
    bool is_get = false;
    bool end_marker = false;
};

// Open loop, sending half: the schedule decides when to send, never the replies.
void open_loop_sender(Connection& connection, Workload& workload, const Times& times, Clock::duration interval,
                      Clock::duration offset, std::mutex& queue_mutex, std::deque<Scheduled>& queue,
                      LatencyHistogram& send_lag) {
#if defined(__linux__)
    // Linux lets a sleeping thread wake up to 50 us late by default ("timer
    // slack"), to batch wake-ups. For a sender on a schedule that lateness is
    // measured as latency, so ask for 1 ns. (macOS has no such knob.)
    ::prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
#endif
    std::string batch;
    std::vector<Scheduled> burst;
    std::uint64_t n = 0;
    for (;;) {
        const Clock::time_point now = Clock::now();
        Clock::time_point due = times.start + offset + interval * static_cast<Clock::rep>(n);
        if (due >= times.end) {
            break;
        }
        if (due > now) {
            std::this_thread::sleep_until(due);
            continue;
        }
        // Behind schedule (the OS woke us late, or a write blocked): send every
        // request that is already due, up to a limit, in one write. Each keeps
        // its own scheduled time.
        batch.clear();
        burst.clear();
        while (due <= now && due < times.end && burst.size() < kMaxOpenLoopBurst) {
            burst.push_back(Scheduled{due, workload.append_request(batch), false});
            ++n;
            due = times.start + offset + interval * static_cast<Clock::rep>(n);
        }
        {
            // Queued before the write, so a reply can never arrive before its entry.
            const std::lock_guard lock{queue_mutex};
            queue.insert(queue.end(), burst.begin(), burst.end());
        }
        const Clock::time_point written = Clock::now();
        for (const Scheduled& request : burst) {
            if (request.due >= times.window_start) {
                send_lag.record(to_nanos(written - request.due));
            }
        }
        connection.write(batch);
    }
    // PING after the last request: its PONG tells the receiver, in order,
    // that every earlier reply has been read.
    {
        const std::lock_guard lock{queue_mutex};
        queue.push_back(Scheduled{Clock::now(), false, true});
    }
    connection.write("PING\n");
}

// Open loop, receiving half.
void open_loop_receiver(Connection& connection, const Times& times, std::mutex& queue_mutex,
                        std::deque<Scheduled>& queue, ClientResult& result) {
    for (;;) {
        const std::string_view reply = connection.read_line();
        const Clock::time_point now = Clock::now();
        Scheduled request;
        {
            const std::lock_guard lock{queue_mutex};
            if (queue.empty()) {
                throw std::runtime_error{"reply without a request: '" + std::string{reply} + "'"};
            }
            request = queue.front();
            queue.pop_front();
        }
        if (request.end_marker) {
            if (reply != "PONG") {
                throw std::runtime_error{"expected PONG after the last request, got '" + std::string{reply} + "'"};
            }
            return;
        }
        result.on_reply(request.is_get, reply, request.due >= times.window_start, to_nanos(now - request.due));
    }
}

// Counts finished client threads so the main thread can wait with a deadline.
class Completion {
public:
    explicit Completion(std::size_t expected) : remaining_{expected} {}
    void done() {
        const std::lock_guard lock{mutex_};
        --remaining_;
        cv_.notify_all();
    }
    bool wait_until(Clock::time_point deadline) {
        std::unique_lock lock{mutex_};
        return cv_.wait_until(lock, deadline, [this] { return remaining_ == 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t remaining_;
};

std::string fmt_us(std::uint64_t nanos) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.1f", static_cast<double>(nanos) / 1'000.0);
    return text.data();
}

std::string fmt_fixed(double value, int decimals) {
    std::array<char, 48> text{};
    std::snprintf(text.data(), text.size(), "%.*f", decimals, value);
    return text.data();
}

} // namespace

LoadReport run_load(const LoadOptions& options) {
    LoadReport report;
    asio::io_context io;
    std::vector<std::unique_ptr<Connection>> connections;
    try {
        tcp::resolver resolver{io};
        const auto endpoints = resolver.resolve(options.host, std::to_string(options.port));
        if (options.populate) {
            Connection setup{io, endpoints};
            populate(setup, options);
        }
        // Every connection is open before the clock starts: connection set-up
        // is not part of the measured work.
        for (unsigned c = 0; c < options.clients; ++c) {
            connections.push_back(std::make_unique<Connection>(io, endpoints));
        }
    } catch (const std::exception& e) {
        report.failure = e.what();
        return report;
    }

    Times times;
    times.start = Clock::now() + kStartDelay;
    times.window_start = times.start + options.warmup;
    times.end = times.window_start + options.duration;

    const bool open_loop = is_open_loop(options);
    std::vector<ClientResult> results(options.clients);
    std::vector<std::mutex> queue_mutexes(open_loop ? options.clients : 0);
    std::vector<std::deque<Scheduled>> queues(open_loop ? options.clients : 0);
    Completion completion{open_loop ? 2U * options.clients : options.clients};
    // Per connection: rate / clients requests per second, i.e. one every `interval`.
    const auto interval = open_loop ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>{
                                          static_cast<double>(options.clients) / options.rate})
                                    : Clock::duration::zero();

    // A failure in one thread shuts its connection down, so the thread on the
    // other side of it (open loop) stops waiting too. Each thread records its
    // failure in its own string: no two threads write the same one.
    const auto guarded = [&](unsigned c, std::string& failure, auto&& body) {
        return [&, c, body = std::forward<decltype(body)>(body)]() mutable {
            try {
                body();
            } catch (const std::exception& e) {
                failure = e.what();
                connections[c]->shutdown();
            }
            completion.done();
        };
    };

    std::vector<Workload> workloads;
    workloads.reserve(options.clients);
    for (unsigned c = 0; c < options.clients; ++c) {
        workloads.emplace_back(options, std::uint64_t{c} + 1);
    }

    std::vector<std::jthread> threads;
    for (unsigned c = 0; c < options.clients; ++c) {
        Connection& connection = *connections[c];
        if (!open_loop) {
            threads.emplace_back(guarded(c, results[c].failure, [&, c] {
                closed_loop_client(connection, workloads[c], options, times, results[c]);
            }));
            continue;
        }
        const auto offset = interval * static_cast<Clock::rep>(c) / static_cast<Clock::rep>(options.clients);
        threads.emplace_back(guarded(c, results[c].sender_failure, [&, c, offset] {
            open_loop_sender(connection, workloads[c], times, interval, offset, queue_mutexes[c], queues[c],
                             results[c].send_lag);
        }));
        // The receiver writes results[c]; the sender only its sender_failure.
        threads.emplace_back(guarded(c, results[c].failure, [&, c] {
            open_loop_receiver(connection, times, queue_mutexes[c], queues[c], results[c]);
        }));
    }

    if (!completion.wait_until(times.end + kDrainTimeout)) {
        report.failure = "replies still missing " + std::to_string(kDrainTimeout.count()) +
                         " s after the end of the run; giving up";
        for (const auto& connection : connections) {
            connection->shutdown();
        }
    }
    threads.clear();   // joins

    report.window_seconds = std::chrono::duration<double>(options.duration).count();
    for (const ClientResult& r : results) {
        report.latency.merge(r.latency);
        report.send_lag.merge(r.send_lag);
        report.requests += r.requests;
        report.gets += r.gets;
        report.sets += r.sets;
        report.get_misses += r.get_misses;
        report.errors += r.errors;
        for (const std::string* failure : {&r.failure, &r.sender_failure}) {
            if (report.failure.empty() && !failure->empty()) {
                report.failure = *failure;
            }
        }
    }
    return report;
}

std::string format_text(const LoadOptions& o, const LoadReport& r) {
    std::string out = "cppkv-loadgen: " + o.host + ":" + std::to_string(o.port) + ", ";
    if (is_open_loop(o)) {
        out += "open loop at " + fmt_fixed(o.rate, 0) + " req/s, " + std::to_string(o.clients) + " clients";
    } else {
        out += "closed loop, " + std::to_string(o.clients) + " clients x pipeline " + std::to_string(o.pipeline);
    }
    out += ", " + std::to_string(o.read_percent) + "% GET, " + std::to_string(o.keys) + " keys, " +
           std::to_string(o.value_bytes) + "-byte values\n";
    out += "window:     " + fmt_fixed(r.window_seconds, 1) + " s after " +
           fmt_fixed(std::chrono::duration<double>(o.warmup).count(), 1) + " s warm-up\n";
    out += "requests:   " + std::to_string(r.requests) + " (GET " + std::to_string(r.gets) + ", SET " +
           std::to_string(r.sets) + "; GET misses " + std::to_string(r.get_misses) + "; errors " +
           std::to_string(r.errors) + ")\n";
    out += "throughput: " + fmt_fixed(r.throughput(), 0) + " req/s\n";
    const LatencyHistogram& h = r.latency;
    out += "latency us: p50 " + fmt_us(h.percentile(0.50)) + "  p95 " + fmt_us(h.percentile(0.95)) + "  p99 " +
           fmt_us(h.percentile(0.99)) + "  p99.9 " + fmt_us(h.percentile(0.999)) + "  max " + fmt_us(h.max()) +
           "  mean " + fmt_fixed(h.mean() / 1'000.0, 1) + "\n";
    if (is_open_loop(o)) {
        const LatencyHistogram& lag = r.send_lag;
        out += "send lag us: p50 " + fmt_us(lag.percentile(0.50)) + "  p99 " + fmt_us(lag.percentile(0.99)) +
               "  max " + fmt_us(lag.max()) + "  (client delay before sending; included above)\n";
    }
    if (is_open_loop(o) && r.throughput() < 0.95 * o.rate) {
        out += "warning:    achieved throughput is below the target rate: the server (or this client) is "
               "saturated, and the latencies include the resulting queueing\n";
    }
    if (!r.failure.empty()) {
        out += "FAILED:     " + r.failure + "\n";
    }
    return out;
}

std::string csv_header() {
    return "label,mode,clients,pipeline,target_rps,read_pct,keys,value_bytes,duration_s,requests,"
           "throughput_rps,p50_us,p95_us,p99_us,p999_us,max_us,mean_us,lag_p50_us,lag_p99_us,get_misses,errors,failed";
}

std::string format_csv_row(const LoadOptions& o, const LoadReport& r) {
    const LatencyHistogram& h = r.latency;
    const std::string sep = ",";
    return o.label + sep + (is_open_loop(o) ? "open" : "closed") + sep + std::to_string(o.clients) + sep +
           std::to_string(o.pipeline) + sep + fmt_fixed(o.rate, 0) + sep + std::to_string(o.read_percent) + sep +
           std::to_string(o.keys) + sep + std::to_string(o.value_bytes) + sep + fmt_fixed(r.window_seconds, 1) +
           sep + std::to_string(r.requests) + sep + fmt_fixed(r.throughput(), 0) + sep + fmt_us(h.percentile(0.50)) +
           sep + fmt_us(h.percentile(0.95)) + sep + fmt_us(h.percentile(0.99)) + sep + fmt_us(h.percentile(0.999)) +
           sep + fmt_us(h.max()) + sep + fmt_fixed(h.mean() / 1'000.0, 1) + sep + fmt_us(r.send_lag.percentile(0.50)) +
           sep + fmt_us(r.send_lag.percentile(0.99)) + sep + std::to_string(r.get_misses) + sep +
           std::to_string(r.errors) + sep + (r.failure.empty() ? "0" : "1");
}

} // namespace cppkv::loadgen
