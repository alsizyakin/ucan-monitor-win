#include <cansocket/cansocket.h>

#include "gs_usb_device.h"

#include <algorithm>
#include <fstream>


namespace can {


namespace {


// the last interface and bitrate connected, next to log.txt
const std::filesystem::path settings_file = "cansocket.cfg";


} // namespace


Socket::Socket() {
    std::ifstream file(settings_file);
    std::string interface;
    std::string bitrate;
    if (!(file >> interface >> bitrate)) {
        bsclog::info("No saved CAN connection: connect in the options window.");
        return;
    }

    bsclog::info("Restoring the CAN connection: {}, {} bit/s.", interface, bitrate);
    if (connect(interface, bitrate) != Status::ok) {
        bsclog::warning("Failed to restore the CAN connection: connect in the options window.");
    }
}


Socket::~Socket() {
    if (_device) {
        disconnect();
    }
}


Status Socket::connect(const std::string& interface, const std::string& bitrate) {
    const auto interface_it = std::ranges::find(detail::interface_list, interface);
    if (interface_it == detail::interface_list.end()
            || std::ranges::find(detail::bitrate_list, bitrate) == detail::bitrate_list.end()) {
        return Status::invalid_argument;
    }

    std::lock_guard<std::mutex> lock1(_send_mtx);
    std::lock_guard<std::mutex> lock2(_recv_mtx);

    _connected = false;
    _device.reset();

    bsclog::info("Opening {} at {} bit/s...", interface, bitrate);
    const auto index = static_cast<size_t>(interface_it - detail::interface_list.begin());
    auto device = detail::GsUsbDevice::open(index, static_cast<uint32_t>(std::stoul(bitrate)), {});
    if (!device) {
        bsclog::error("Failed to open {}. Error code: {}", interface, std::to_underlying(device.error()));
        return device.error();
    }

    _device = std::move(*device);
    _connected = true;
    bsclog::success("Opened {} at {} bit/s.", interface, bitrate);

    std::ofstream(settings_file) << interface << ' ' << bitrate << '\n';
    return Status::ok;
}


Status Socket::disconnect() {
    std::lock_guard<std::mutex> lock1(_send_mtx);
    std::lock_guard<std::mutex> lock2(_recv_mtx);

    if (!_device) {
        bsclog::error("Failed to close CAN socket: no socket.");
        return Status::socket_closed;
    }

    _connected = false;
    _device.reset();
    bsclog::success("Closed socket.");
    return Status::ok;
}


Status Socket::send(const can_frame& frame) {
    std::lock_guard<std::mutex> lock(_send_mtx);
    if (!_device) {
        return Status::socket_closed;
    }

    const Status status = _device->send(frame);
    if (!_device->good()) {
        _connected = false;
    }
    return status;
}


Status Socket::recv(can_frame& frame) {
    std::lock_guard<std::mutex> lock(_recv_mtx);
    if (!_device) {
        return Status::socket_closed;
    }

    const Status status = _device->recv(frame, _recv_timeout);
    if (!_device->good()) {
        _connected = false;
    }
    return status;
}


} // namespace can
