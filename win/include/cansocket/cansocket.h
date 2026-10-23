// Replaces upstream src/cansocket/cansocket.h.
//
// The class keeps the upstream can::Socket interface, which the client, the
// servers and the options window use. It talks to a candleLight (gs_usb) USB
// adapter instead of SocketCAN. Keep the public part in step with upstream.
#pragma once

#include <linux/can.h>

// The standard headers the upstream one includes: upstream code may rely on
// getting them through it.
#include <cerrno>
#include <cstring>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <bsclog/bsclog.h>


namespace can {


// Same values in the same order as upstream: the options window shows the
// failure code as a number.
enum class Status {
    ok,
    invalid_argument,
    script_not_found,
    device_not_found,
    socketcan_init_failed,
    script_exec_failed,
    socket_creation_failed,
    socket_closing_failed,
    socket_binding_failed,
    socket_closed,
    send_error,
    recv_timeout,
    recv_error,
    interface_retrieving_failed,
};


namespace detail {


// The first and the second candleLight adapter found on USB.
const std::vector<std::string> interface_list = {"candle0", "candle1"};
const std::vector<std::string> bitrate_list = {"125000", "250000", "500000", "1000000"};

class GsUsbDevice;


} // namespace detail


class Socket {
private:
    std::unique_ptr<detail::GsUsbDevice> _device;
    std::atomic<bool> _connected{false};
    static constexpr std::chrono::milliseconds _recv_timeout{1};

    // Diagnostics for log.txt; touched under _recv_mtx only.
    struct Diagnostics {
        std::chrono::steady_clock::time_point period_start;
        std::chrono::steady_clock::time_point last_recv;
        std::chrono::microseconds max_pause{0}; // between recv() calls, this period
        std::chrono::microseconds max_pause_total{0};
        uint64_t sent{0};
        uint64_t unconfirmed{0};
        uint64_t resent{0};
        uint64_t late_echoes{0};
        uint64_t send_failed{0};
        uint64_t send_dropped{0};
        uint64_t received{0};
        uint64_t receive_dropped{0};
        uint64_t bus_errors{0};
    };
    Diagnostics _diag;
    void _report(bool final);

    std::mutex _send_mtx;
    std::mutex _recv_mtx;

public:
    // Connects as last time, if it was saved: SocketCAN keeps an interface up
    // between runs, and upstream connects to it at start.
    Socket();
    ~Socket();
    Socket(const Socket& other) = delete;
    Socket& operator=(const Socket& other) = delete;

    Status connect(const std::string& interface, const std::string& bitrate);
    Status disconnect();

    Status send(const can_frame& frame);
    Status recv(can_frame& frame);

    bool good() const { return _connected; }
};


} // namespace can
