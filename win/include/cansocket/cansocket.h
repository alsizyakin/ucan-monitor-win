// Replaces upstream src/cansocket/cansocket.h.
//
// The class keeps the upstream can::Socket interface, which the client, the
// servers and the options window use. It talks to a candleLight (gs_usb) USB
// adapter instead of SocketCAN. Keep the public part in step with upstream.
#pragma once

#include <linux/can.h>

#include <cerrno>
#include <cstring>

#include <array>
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


} // namespace detail


class Socket {
private:
    bool _open{false};

    std::mutex _send_mtx;
    std::mutex _recv_mtx;

public:
    Socket();
    ~Socket();
    Socket(const Socket& other) = delete;
    Socket& operator=(const Socket& other) = delete;

    Status connect(const std::string& interface, const std::string& bitrate);
    Status disconnect();

    Status send(const can_frame& frame);
    Status recv(can_frame& frame);

    bool good() const { return _open; }
};


} // namespace can
