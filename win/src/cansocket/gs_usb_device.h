#pragma once


#include <cansocket/cansocket.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;


namespace can::detail {


// A gs_usb USB-CAN adapter (candleLight firmware), channel 0, opened through
// libusb: the same protocol the Linux gs_usb driver speaks, so it works the
// same way on Windows (WinUSB) and Linux.
class GsUsbDevice {
public:
    struct Options {
        // receive only: no acknowledgements, no error flags on the bus
        bool listen_only{false};
        // the controller receives what it sends; with listen_only nothing
        // reaches the bus, a self-test of the adapter and the USB path
        bool loopback{false};
        // hand error frames (CAN_ERR_FLAG) to recv(); SocketCAN raw sockets
        // get none unless they ask, so can::Socket does not
        bool error_frames{false};
    };

    // Descriptions of the gs_usb adapters on USB, in the order open() counts them.
    static std::vector<std::string> list();

    // Opens the index-th gs_usb adapter on USB and starts its channel 0 at bitrate.
    static std::expected<std::unique_ptr<GsUsbDevice>, Status> open(size_t index, uint32_t bitrate,
                                                                    const Options& options);

    // Stops the channel and hands the adapter back to the system.
    ~GsUsbDevice();
    GsUsbDevice(const GsUsbDevice&) = delete;
    GsUsbDevice& operator=(const GsUsbDevice&) = delete;

    Status send(const can_frame& frame);
    // Waits up to timeout for a frame received from the bus.
    Status recv(can_frame& frame, std::chrono::milliseconds timeout);

    // False once the adapter is gone (unplugged).
    bool good() const { return !_lost; }

private:
    GsUsbDevice() = default;
    Status _start(uint32_t bitrate);
    void _on_rx(libusb_transfer* transfer);
    void _mark_lost();
    void _run_events();

    Options _options;
    libusb_context* _ctx{nullptr};
    libusb_device_handle* _handle{nullptr};
    bool _claimed{false};
    bool _started{false};
    int _interface{0};
    uint8_t _ep_in{0};
    uint8_t _ep_out{0};
    int _rx_buf_size{0};

    std::vector<libusb_transfer*> _rx_transfers;
    std::vector<std::vector<uint8_t>> _rx_buffers;
    std::atomic<int> _rx_active{0};
    std::atomic<bool> _stopping{false};
    std::atomic<bool> _lost{false};
    int _rx_errors_in_row{0};
    bool _overflow_reported{false};
    std::thread _event_thread;

    std::mutex _queue_mtx;
    std::condition_variable _queue_cv;
    std::deque<can_frame> _queue;
    bool _queue_full_reported{false};
};


} // namespace can::detail
