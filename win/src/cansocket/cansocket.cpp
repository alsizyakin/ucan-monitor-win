#include <cansocket/cansocket.h>

#include <algorithm>


namespace can {


// Stub: the candleLight backend is not written yet. The program runs with the
// CAN bus unavailable, as upstream does when can0 is down.


Socket::Socket() {
    bsclog::warning("candleLight backend is not implemented yet: CAN bus is unavailable.");
}


Socket::~Socket() {
    if (_open) {
        disconnect();
    }
}


Status Socket::connect(const std::string& interface, const std::string& bitrate) {
    if (std::ranges::find(detail::interface_list, interface) == detail::interface_list.end()
            || std::ranges::find(detail::bitrate_list, bitrate) == detail::bitrate_list.end()) {
        return Status::invalid_argument;
    }

    bsclog::error("Failed to open {}: candleLight backend is not implemented yet.", interface);
    return Status::device_not_found;
}


Status Socket::disconnect() {
    if (!_open) {
        bsclog::error("Failed to close CAN socket: no socket.");
        return Status::socket_closed;
    }

    std::lock_guard<std::mutex> lock1(_send_mtx);
    std::lock_guard<std::mutex> lock2(_recv_mtx);
    _open = false;
    bsclog::success("Closed socket.");
    return Status::ok;
}


Status Socket::send([[maybe_unused]] const can_frame& frame) {
    return Status::socket_closed;
}


Status Socket::recv([[maybe_unused]] can_frame& frame) {
    return Status::socket_closed;
}


} // namespace can
