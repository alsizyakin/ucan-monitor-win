#include "gs_usb_device.h"

#include "bittiming.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

#ifdef _WIN32
#ifndef NOMINMAX // MinGW's libstdc++ may have it already
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif
#include <libusb.h>


namespace can::detail {


namespace {


// USB ids the Linux gs_usb driver takes for candleLight adapters
struct UsbId {
    uint16_t vid;
    uint16_t pid;
};

constexpr UsbId gs_usb_ids[] = {
    {0x1d50, 0x606f}, // candleLight, CANable with candleLight firmware
    {0x1209, 0x2323}, // candleLight (pid.codes)
};


// gs_usb protocol: vendor requests to the interface set the channel up,
// frames go over the bulk endpoints. Everything is little-endian.
static_assert(std::endian::native == std::endian::little);

enum Request : uint8_t {
    request_host_format = 0,
    request_bittiming = 1,
    request_mode = 2,
    request_bt_const = 4,
    request_device_config = 5,
};

struct HostConfig {
    uint32_t byte_order;
};

struct DeviceConfig {
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t reserved3;
    uint8_t icount; // channel count - 1
    uint32_t sw_version;
    uint32_t hw_version;
};

struct BtConst {
    uint32_t feature;
    uint32_t fclk_can;
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
};

struct DeviceBittiming {
    uint32_t prop_seg;
    uint32_t phase_seg1;
    uint32_t phase_seg2;
    uint32_t sjw;
    uint32_t brp;
};

struct DeviceMode {
    uint32_t mode;
    uint32_t flags;
};

constexpr uint32_t mode_reset = 0;
constexpr uint32_t mode_start = 1;

// bits of BtConst::feature, and of DeviceMode::flags to use them
constexpr uint32_t feature_listen_only = 1 << 0;
constexpr uint32_t feature_loop_back = 1 << 1;
constexpr uint32_t feature_berr_reporting = 1 << 12;

// A classic CAN frame on the bulk endpoints
struct HostFrame {
    uint32_t echo_id;
    uint32_t can_id; // id and flags as in SocketCAN
    uint8_t can_dlc;
    uint8_t channel;
    uint8_t flags;
    uint8_t reserved;
    uint8_t data[8];
};

static_assert(sizeof(HostConfig) == 4);
static_assert(sizeof(DeviceConfig) == 12);
static_assert(sizeof(BtConst) == 40);
static_assert(sizeof(DeviceBittiming) == 20);
static_assert(sizeof(DeviceMode) == 8);
static_assert(sizeof(HostFrame) == 20);

// echo_id of a frame from the bus; any other value is the echo of a frame we
// sent, which SocketCAN does not hand back to the sending socket either
constexpr uint32_t rx_echo_id = 0xFFFFFFFF;
constexpr uint8_t frame_flag_overflow = 1 << 0;

// vendor requests to the interface, each way
constexpr uint8_t request_type_out = static_cast<uint8_t>(LIBUSB_ENDPOINT_OUT)
                                   | static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR)
                                   | static_cast<uint8_t>(LIBUSB_RECIPIENT_INTERFACE);
constexpr uint8_t request_type_in = static_cast<uint8_t>(LIBUSB_ENDPOINT_IN)
                                  | static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR)
                                  | static_cast<uint8_t>(LIBUSB_RECIPIENT_INTERFACE);

constexpr uint16_t channel = 0;
constexpr unsigned control_timeout_ms = 1000;
constexpr int rx_transfer_count = 16;
constexpr size_t queue_limit = 10000;
constexpr int rx_errors_to_give_up = 100;
// frames in USB transfers at once, as many as the Linux gs_usb driver keeps
// in flight (10) or fewer
constexpr int tx_transfer_count = 8;
// frames waiting for a transfer; SocketCAN queues 10 before it refuses
constexpr size_t tx_queue_limit = 256;
// a frame the adapter took but has not reported sent on the bus in this time
// is counted as lost in the adapter
constexpr std::chrono::seconds echo_timeout{1};
// "no echo id" in the trace
constexpr uint32_t no_echo_id = rx_echo_id;


bool is_gs_usb(const libusb_device_descriptor& desc) {
    return std::ranges::any_of(gs_usb_ids, [&](const UsbId& id) {
        return id.vid == desc.idVendor && id.pid == desc.idProduct;
    });
}


template <typename T>
int control_out(libusb_device_handle* handle, Request request, uint16_t value, int interface, const T& data) {
    return libusb_control_transfer(handle,
                                   request_type_out,
                                   request,
                                   value,
                                   static_cast<uint16_t>(interface),
                                   reinterpret_cast<unsigned char*>(const_cast<T*>(&data)),
                                   sizeof(T),
                                   control_timeout_ms);
}


template <typename T>
int control_in(libusb_device_handle* handle, Request request, uint16_t value, int interface, T& data) {
    return libusb_control_transfer(handle,
                                   request_type_in,
                                   request,
                                   value,
                                   static_cast<uint16_t>(interface),
                                   reinterpret_cast<unsigned char*>(&data),
                                   sizeof(T),
                                   control_timeout_ms);
}


std::string error_text(int error) {
    return std::format("{} ({})", libusb_strerror(error), libusb_error_name(error));
}


std::string transfer_status_text(int status) {
    switch (status) {
    case LIBUSB_TRANSFER_ERROR: return "transfer error";
    case LIBUSB_TRANSFER_TIMED_OUT: return "timed out";
    case LIBUSB_TRANSFER_CANCELLED: return "cancelled";
    case LIBUSB_TRANSFER_STALL: return "endpoint stalled";
    case LIBUSB_TRANSFER_NO_DEVICE: return "adapter gone";
    case LIBUSB_TRANSFER_OVERFLOW: return "overflow";
    default: return std::format("status {}", status);
    }
}


void log_open_hint(int error) {
    if (error == LIBUSB_ERROR_ACCESS) {
#ifdef _WIN32
        bsclog::error("No access to the adapter: it may be open in another program.");
#else
        bsclog::error("No access to the adapter: run as root or install the udev rule (see README).");
#endif
    } else if (error == LIBUSB_ERROR_NOT_SUPPORTED || error == LIBUSB_ERROR_NOT_FOUND) {
        bsclog::error("The adapter has no WinUSB driver: replug it, or bind WinUSB with Zadig.");
    }
}


} // namespace


std::vector<std::string> GsUsbDevice::list() {
    std::vector<std::string> result;

    libusb_context* ctx = nullptr;
    if (libusb_init_context(&ctx, nullptr, 0) != 0) {
        return result;
    }

    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &devices);
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(devices[i], &desc) != 0 || !is_gs_usb(desc)) {
            continue;
        }

        std::string line = std::format("candle{}: {:04x}:{:04x}, bus {}, address {}",
                                       result.size(),
                                       desc.idVendor,
                                       desc.idProduct,
                                       libusb_get_bus_number(devices[i]),
                                       libusb_get_device_address(devices[i]));

        libusb_device_handle* handle = nullptr;
        const int error = libusb_open(devices[i], &handle);
        if (error == 0) {
            std::array<unsigned char, 128> text{};
            if (desc.iProduct != 0
                    && libusb_get_string_descriptor_ascii(handle, desc.iProduct, text.data(), text.size()) > 0) {
                line += std::format(", {}", reinterpret_cast<const char*>(text.data()));
            }
            if (desc.iSerialNumber != 0
                    && libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, text.data(), text.size()) > 0) {
                line += std::format(", serial {}", reinterpret_cast<const char*>(text.data()));
            }
            libusb_close(handle);
        } else {
            line += std::format(", cannot open: {}", error_text(error));
        }
        result.push_back(line);
    }

    if (count >= 0) {
        libusb_free_device_list(devices, 1);
    }
    libusb_exit(ctx);
    return result;
}


std::expected<std::unique_ptr<GsUsbDevice>, Status> GsUsbDevice::open(size_t index, uint32_t bitrate,
                                                                     const Options& options) {
    // Every early return below runs the destructor, which undoes what was done.
    std::unique_ptr<GsUsbDevice> dev(new GsUsbDevice);
    dev->_options = options;

    if (!options.trace_file.empty()) {
        dev->_trace_out.open(options.trace_file);
        if (dev->_trace_out) {
            // S sent by the program, E reported sent on the bus by the adapter,
            // M no such report within 1 s, F lost in a failed USB transfer,
            // D refused (send queue full), R received, X bus error
            dev->_trace_out << "time_ms,event,echo_id,can_id,len,data\n";
            dev->_trace_start = dev->_trace_flushed = Clock::now();
            bsclog::info("CAN trace: writing {}.", options.trace_file.string());
        } else {
            bsclog::warning("CAN trace: cannot write {}.", options.trace_file.string());
        }
    }

    int error = libusb_init_context(&dev->_ctx, nullptr, 0);
    if (error != 0) {
        dev->_ctx = nullptr;
        bsclog::error("Failed to initialize libusb: {}.", error_text(error));
        return std::unexpected(Status::socketcan_init_failed);
    }

    /* FIND ADAPTER */
    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(dev->_ctx, &devices);
    if (count < 0) {
        bsclog::error("Failed to list USB devices: {}.", error_text(static_cast<int>(count)));
        return std::unexpected(Status::device_not_found);
    }

    libusb_device* found = nullptr;
    size_t gs_usb_count = 0;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(devices[i], &desc) == 0 && is_gs_usb(desc)) {
            if (gs_usb_count == index) {
                found = devices[i];
            }
            ++gs_usb_count;
        }
    }

    if (found == nullptr) {
        libusb_free_device_list(devices, 1);
        bsclog::error("Failed to find candle{}: {} candleLight adapter(s) on USB.", index, gs_usb_count);
        return std::unexpected(Status::device_not_found);
    }

    // the gs_usb interface: vendor class, one bulk endpoint each way
    libusb_config_descriptor* config = nullptr;
    error = libusb_get_active_config_descriptor(found, &config);
    if (error == 0) {
        for (int i = 0; i < config->bNumInterfaces && dev->_ep_in == 0; ++i) {
            const libusb_interface& interface = config->interface[i];
            for (int alt = 0; alt < interface.num_altsetting && dev->_ep_in == 0; ++alt) {
                const libusb_interface_descriptor& desc = interface.altsetting[alt];
                if (desc.bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC) {
                    continue;
                }
                uint8_t ep_in = 0;
                uint8_t ep_out = 0;
                int in_size = 0;
                for (int e = 0; e < desc.bNumEndpoints; ++e) {
                    const libusb_endpoint_descriptor& ep = desc.endpoint[e];
                    if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) {
                        continue;
                    }
                    if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                        ep_in = ep.bEndpointAddress;
                        in_size = ep.wMaxPacketSize;
                    } else {
                        ep_out = ep.bEndpointAddress;
                    }
                }
                if (ep_in != 0 && ep_out != 0) {
                    dev->_interface = desc.bInterfaceNumber;
                    dev->_ep_in = ep_in;
                    dev->_ep_out = ep_out;
                    dev->_rx_buf_size = std::max(in_size, 64);
                }
            }
        }
        libusb_free_config_descriptor(config);
    }

    if (dev->_ep_in == 0) {
        libusb_free_device_list(devices, 1);
        bsclog::error("Failed to open candle{}: no gs_usb interface ({}).",
                      index, error == 0 ? "not found" : error_text(error));
        return std::unexpected(Status::device_not_found);
    }

    /* OPEN */
    error = libusb_open(found, &dev->_handle);
    libusb_free_device_list(devices, 1);
    if (error != 0) {
        dev->_handle = nullptr;
        bsclog::error("Failed to open candle{}: {}.", index, error_text(error));
        log_open_hint(error);
        return std::unexpected(Status::socket_creation_failed);
    }

    // Linux: take the adapter from the gs_usb driver (can0) until it is
    // released; elsewhere there is nothing to detach.
    libusb_set_auto_detach_kernel_driver(dev->_handle, 1);

    error = libusb_claim_interface(dev->_handle, dev->_interface);
    if (error != 0) {
        bsclog::error("Failed to claim candle{}: {}.", index, error_text(error));
        if (error == LIBUSB_ERROR_BUSY) {
            bsclog::error("The adapter is in use by another program.");
        }
        return std::unexpected(Status::socket_binding_failed);
    }
    dev->_claimed = true;

    const Status status = dev->_start(bitrate);
    if (status != Status::ok) {
        return std::unexpected(status);
    }
    return dev;
}


Status GsUsbDevice::_start(uint32_t bitrate) {
    /* CONFIGURE */
    // Older firmware wants to know the host byte order; newer ignores it.
    control_out(_handle, request_host_format, 1, _interface, HostConfig{0x0000beef});

    DeviceConfig device_config{};
    int result = control_in(_handle, request_device_config, 1, _interface, device_config);
    if (result != sizeof(device_config)) {
        bsclog::error("Failed to read the adapter configuration: {}.", error_text(result));
        return Status::socketcan_init_failed;
    }
    bsclog::info("candleLight: firmware {}, hardware {}, {} channel(s).",
                 device_config.sw_version, device_config.hw_version, device_config.icount + 1);

    BtConst bt_const{};
    result = control_in(_handle, request_bt_const, channel, _interface, bt_const);
    if (result != sizeof(bt_const)) {
        bsclog::error("Failed to read the adapter bit timing limits: {}.", error_text(result));
        return Status::socketcan_init_failed;
    }

    uint32_t mode_flags = 0;
    if (_options.listen_only) {
        if (!(bt_const.feature & feature_listen_only)) {
            bsclog::error("The adapter cannot listen only.");
            return Status::socketcan_init_failed;
        }
        mode_flags |= feature_listen_only;
    }
    if (_options.loopback) {
        if (!(bt_const.feature & feature_loop_back)) {
            bsclog::error("The adapter cannot loop back.");
            return Status::socketcan_init_failed;
        }
        mode_flags |= feature_loop_back;
    }
    // newer firmware reports bus errors only when asked; older always does
    if ((_options.error_frames || _trace_out.is_open()) && (bt_const.feature & feature_berr_reporting)) {
        mode_flags |= feature_berr_reporting;
    }

    const auto timing = calculate_bittiming(bitrate, BittimingLimits{
        .fclk = bt_const.fclk_can,
        .tseg1_min = bt_const.tseg1_min,
        .tseg1_max = bt_const.tseg1_max,
        .tseg2_min = bt_const.tseg2_min,
        .tseg2_max = bt_const.tseg2_max,
        .sjw_max = bt_const.sjw_max,
        .brp_min = bt_const.brp_min,
        .brp_max = bt_const.brp_max,
        .brp_inc = bt_const.brp_inc,
    });
    if (!timing) {
        bsclog::error("Failed to set {} bit/s: the adapter clock ({} Hz) cannot make it within 1 %.",
                      bitrate, bt_const.fclk_can);
        return Status::socketcan_init_failed;
    }
    bsclog::info("Bit timing for {} bit/s: clock {} Hz, brp {}, tseg1 {}, tseg2 {}, sjw {}, sample point {:.1f} %.",
                 timing->bitrate, bt_const.fclk_can, timing->brp, timing->tseg1, timing->tseg2, timing->sjw,
                 timing->sample_point / 10.0);

    // a channel left started (by a program that crashed) takes no new bit timing
    control_out(_handle, request_mode, channel, _interface, DeviceMode{mode_reset, 0});

    const DeviceBittiming device_bittiming{
        .prop_seg = timing->tseg1 / 2,
        .phase_seg1 = timing->tseg1 - timing->tseg1 / 2,
        .phase_seg2 = timing->tseg2,
        .sjw = timing->sjw,
        .brp = timing->brp,
    };
    result = control_out(_handle, request_bittiming, channel, _interface, device_bittiming);
    if (result != sizeof(device_bittiming)) {
        bsclog::error("Failed to set the bit timing: {}.", error_text(result));
        return Status::socketcan_init_failed;
    }

    /* RECEIVE */
    // Several transfers wait for frames at once, so none is lost while the
    // program is busy; the event thread completes them into the queue.
    for (int i = 0; i < rx_transfer_count; ++i) {
        libusb_transfer* transfer = libusb_alloc_transfer(0);
        if (transfer == nullptr) {
            bsclog::error("Failed to allocate a USB transfer.");
            return Status::socketcan_init_failed;
        }
        _rx_transfers.push_back(transfer);
        _rx_buffers.emplace_back(_rx_buf_size);
        libusb_fill_bulk_transfer(transfer, _handle, _ep_in,
                                  _rx_buffers.back().data(), _rx_buf_size,
                                  [](libusb_transfer* t) { static_cast<GsUsbDevice*>(t->user_data)->_on_rx(t); },
                                  this, 0);
    }

    /* SEND */
    // Transfers waiting idle; send() fills one, or queues the frame for the
    // next transfer to complete.
    for (int i = 0; i < tx_transfer_count; ++i) {
        auto slot = std::make_unique<TxSlot>();
        slot->device = this;
        slot->transfer = libusb_alloc_transfer(0);
        if (slot->transfer == nullptr) {
            bsclog::error("Failed to allocate a USB transfer.");
            return Status::socketcan_init_failed;
        }
        libusb_fill_bulk_transfer(slot->transfer, _handle, _ep_out,
                                  slot->frame.bytes.data(), static_cast<int>(slot->frame.bytes.size()),
                                  [](libusb_transfer* t) {
                                      auto* s = static_cast<TxSlot*>(t->user_data);
                                      s->device->_on_tx(s);
                                  },
                                  slot.get(), 0);
        _tx_idle.push_back(slot.get());
        _tx_slots.push_back(std::move(slot));
    }

    DeviceMode start{mode_start, mode_flags};
    result = control_out(_handle, request_mode, channel, _interface, start);
    if (result != sizeof(start)) {
        bsclog::error("Failed to start the CAN channel: {}.", error_text(result));
        return Status::socketcan_init_failed;
    }
    _started = true;

    for (libusb_transfer* transfer : _rx_transfers) {
        result = libusb_submit_transfer(transfer);
        if (result != 0) {
            bsclog::error("Failed to start receiving: {}.", error_text(result));
            break;
        }
        ++_rx_active;
    }
    if (_rx_active == 0) {
        return Status::socketcan_init_failed;
    }
    _event_thread = std::thread(&GsUsbDevice::_run_events, this);

#ifdef _WIN32
    // Waits for a frame are 1 ms, as with SocketCAN; the default Windows
    // timer would stretch them to 15.6 ms.
    timeBeginPeriod(1);
#endif
    return Status::ok;
}


GsUsbDevice::~GsUsbDevice() {
    if (_event_thread.joinable()) {
        _stopping = true;
        libusb_interrupt_event_handler(_ctx);
        _event_thread.join();
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    }

    if (_started) {
        control_out(_handle, request_mode, channel, _interface, DeviceMode{mode_reset, 0});
    }
    if (_claimed) {
        // Linux: hands the adapter back to the gs_usb driver
        libusb_release_interface(_handle, _interface);
    }
    for (libusb_transfer* transfer : _rx_transfers) {
        libusb_free_transfer(transfer);
    }
    for (const auto& slot : _tx_slots) {
        libusb_free_transfer(slot->transfer);
    }
    if (_handle != nullptr) {
        libusb_close(_handle);
    }
    if (_ctx != nullptr) {
        libusb_exit(_ctx);
    }
}


void GsUsbDevice::_run_events() {
    // All transfer callbacks run here: nothing else handles libusb events.
    while (_rx_active > 0 || _tx_active > 0) {
        if (_stopping) {
            // repeated each turn: a transfer may be resubmitted after a cancel
            for (libusb_transfer* transfer : _rx_transfers) {
                libusb_cancel_transfer(transfer);
            }
            for (const auto& slot : _tx_slots) {
                libusb_cancel_transfer(slot->transfer);
            }
        }
        timeval timeout{0, 100000};
        libusb_handle_events_timeout_completed(_ctx, &timeout, nullptr);
    }
}


void GsUsbDevice::_on_rx(libusb_transfer* transfer) {
    // Runs on the event thread.
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED: {
        _rx_errors_in_row = 0;
        if (transfer->actual_length < static_cast<int>(sizeof(HostFrame))) {
            break;
        }
        HostFrame host_frame;
        std::memcpy(&host_frame, transfer->buffer, sizeof(host_frame));
        if (host_frame.channel != channel) {
            break;
        }
        if (host_frame.echo_id != rx_echo_id) {
            _on_echo(host_frame.echo_id, Clock::now());
            break;
        }
        if (host_frame.flags & frame_flag_overflow) {
            std::lock_guard<std::mutex> lock(_stats_mtx);
            ++_stats.adapter_overflows;
        }
        if ((host_frame.flags & frame_flag_overflow) && !_overflow_reported) {
            _overflow_reported = true;
            bsclog::warning("candleLight: the adapter dropped received frames (overflow).");
        }

        can_frame frame{};
        frame.can_id = host_frame.can_id;
        frame.len = std::min<uint8_t>(host_frame.can_dlc, CAN_MAX_DLEN);
        std::memcpy(frame.data, host_frame.data, CAN_MAX_DLEN);

        if (frame.can_id & CAN_ERR_FLAG) {
            {
                std::lock_guard<std::mutex> stats_lock(_stats_mtx);
                ++_stats.bus_errors;
            }
            _trace('X', no_echo_id, frame);
            if (!_options.error_frames) {
                break;
            }
        } else {
            _trace('R', no_echo_id, frame);
        }
        {
            std::lock_guard<std::mutex> stats_lock(_stats_mtx);
            ++_stats.received;
        }
        {
            std::lock_guard<std::mutex> lock(_queue_mtx);
            if (_queue.size() >= queue_limit) {
                _queue.pop_front();
                {
                    std::lock_guard<std::mutex> stats_lock(_stats_mtx);
                    ++_stats.receive_dropped;
                }
                if (!_queue_full_reported) {
                    _queue_full_reported = true;
                    bsclog::warning("candleLight: received frames are not read, dropping the oldest.");
                }
            }
            _queue.push_back(frame);
        }
        _queue_cv.notify_one();
        break;
    }
    case LIBUSB_TRANSFER_CANCELLED:
        --_rx_active;
        return;
    case LIBUSB_TRANSFER_NO_DEVICE:
        _mark_lost();
        --_rx_active;
        return;
    default: // error, stall, overflow, timeout
        if (++_rx_errors_in_row >= rx_errors_to_give_up) {
            _mark_lost();
            --_rx_active;
            return;
        }
        break;
    }

    if (_stopping || libusb_submit_transfer(transfer) != 0) {
        if (!_stopping) {
            _mark_lost();
        }
        --_rx_active;
    }
}


void GsUsbDevice::_mark_lost() {
    if (!_lost.exchange(true)) {
        bsclog::error("candleLight: the adapter is gone (unplugged?).");
    }
    _queue_cv.notify_all();
}


Status GsUsbDevice::send(const can_frame& frame) {
    if (_lost) {
        return Status::send_error;
    }

    HostFrame host_frame{};
    host_frame.can_id = frame.can_id;
    host_frame.can_dlc = std::min<uint8_t>(frame.len, CAN_MAX_DLEN);
    host_frame.channel = channel;
    std::memcpy(host_frame.data, frame.data, host_frame.can_dlc);

    std::lock_guard<std::mutex> lock(_tx_mtx);
    if (_tx_queue.size() >= tx_queue_limit) {
        {
            std::lock_guard<std::mutex> stats_lock(_stats_mtx);
            ++_stats.send_dropped;
        }
        _trace('D', no_echo_id, frame);
        return Status::send_error;
    }

    // numbered, so that the adapter's echo of each frame can be matched
    host_frame.echo_id = _next_echo_id++;
    if (_next_echo_id == rx_echo_id) {
        _next_echo_id = 0;
    }

    TxFrame tx;
    static_assert(sizeof(host_frame) == std::tuple_size_v<decltype(tx.bytes)>);
    std::memcpy(tx.bytes.data(), &host_frame, sizeof(host_frame));
    tx.queued = Clock::now();
    {
        std::lock_guard<std::mutex> echo_lock(_echo_mtx);
        _pending.push_back(PendingEcho{.echo_id = host_frame.echo_id, .queued = tx.queued, .frame = frame});
    }
    _trace('S', host_frame.echo_id, frame);

    _tx_queue.push_back(tx);
    _drain_tx();
    return Status::ok;
}


void GsUsbDevice::_drain_tx() {
    // with _tx_mtx held: the queue goes into idle transfers, in order
    while (!_tx_idle.empty() && !_tx_queue.empty() && !_stopping && !_lost) {
        TxSlot* slot = _tx_idle.back();
        _tx_idle.pop_back();
        const TxFrame frame = _tx_queue.front();
        _tx_queue.pop_front();
        if (!_submit_tx(slot, frame)) {
            _tx_idle.push_back(slot); // the frame is counted as failed
            uint32_t echo_id;
            std::memcpy(&echo_id, frame.bytes.data(), sizeof(echo_id));
            _mark_failed(echo_id);
        }
    }
    std::lock_guard<std::mutex> lock(_stats_mtx);
    _stats.max_send_queue = std::max(_stats.max_send_queue, _tx_queue.size());
}


bool GsUsbDevice::_submit_tx(TxSlot* slot, const TxFrame& frame) {
    // with _tx_mtx held
    slot->frame = frame;
    const int result = libusb_submit_transfer(slot->transfer);
    if (result != 0) {
        {
            std::lock_guard<std::mutex> lock(_stats_mtx);
            ++_stats.send_failed;
            _stats.last_send_error = error_text(result);
        }
        if (result == LIBUSB_ERROR_NO_DEVICE) {
            _mark_lost();
        }
        return false;
    }
    ++_tx_active;
    return true;
}


void GsUsbDevice::_on_tx(TxSlot* slot) {
    // Runs on the event thread.
    const libusb_transfer* transfer = slot->transfer;
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED && transfer->actual_length == transfer->length) {
        const auto time = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - slot->frame.queued);
        std::lock_guard<std::mutex> lock(_stats_mtx);
        ++_stats.sent;
        _stats.max_send_time = std::max(_stats.max_send_time, time);
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        {
            std::lock_guard<std::mutex> lock(_stats_mtx);
            ++_stats.send_failed;
            _stats.last_send_error = transfer->status == LIBUSB_TRANSFER_COMPLETED
                    ? std::format("short transfer ({} of {} bytes)", transfer->actual_length, transfer->length)
                    : transfer_status_text(transfer->status);
        }
        uint32_t echo_id;
        std::memcpy(&echo_id, slot->frame.bytes.data(), sizeof(echo_id));
        _mark_failed(echo_id);
    }
    if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        _mark_lost();
    }

    std::lock_guard<std::mutex> lock(_tx_mtx);
    --_tx_active;
    _tx_idle.push_back(slot);
    _drain_tx();
}


void GsUsbDevice::_on_echo(uint32_t echo_id, Clock::time_point now) {
    // Runs on the event thread.
    std::lock_guard<std::mutex> lock(_echo_mtx);
    const auto it = std::ranges::find(_pending, echo_id, &PendingEcho::echo_id);
    if (it != _pending.end() && !it->confirmed) {
        it->confirmed = true;
        {
            std::lock_guard<std::mutex> stats_lock(_stats_mtx);
            ++_stats.confirmed;
        }
        _trace('E', echo_id, it->frame);
    } else {
        // an echo of a frame given up on, or a second echo
        _trace('E', echo_id, can_frame{});
    }
    _expire_echoes(now);
}


void GsUsbDevice::_expire_echoes(Clock::time_point now) {
    // with _echo_mtx held
    while (!_pending.empty()) {
        const PendingEcho& front = _pending.front();
        if (!front.confirmed && !front.failed) {
            if (now - front.queued < echo_timeout) {
                break;
            }
            {
                std::lock_guard<std::mutex> stats_lock(_stats_mtx);
                ++_stats.unconfirmed;
            }
            _trace('M', front.echo_id, front.frame);
        }
        _pending.pop_front();
    }
}


void GsUsbDevice::_mark_failed(uint32_t echo_id) {
    // with _echo_mtx not held
    std::lock_guard<std::mutex> lock(_echo_mtx);
    const auto it = std::ranges::find(_pending, echo_id, &PendingEcho::echo_id);
    if (it != _pending.end()) {
        it->failed = true;
        _trace('F', echo_id, it->frame);
    }
}


void GsUsbDevice::_trace(char event, uint32_t echo_id, const can_frame& frame) {
    if (!_trace_out.is_open()) {
        return;
    }
    const auto now = Clock::now();
    std::string data;
    for (unsigned i = 0; i < std::min<unsigned>(frame.len, CAN_MAX_DLEN); ++i) {
        data += std::format("{:02X}", frame.data[i]);
    }
    const std::string echo = echo_id == no_echo_id ? std::string{} : std::to_string(echo_id);

    std::lock_guard<std::mutex> lock(_trace_mtx);
    _trace_out << std::format("{:.3f},{},{},{:X},{},{}\n",
                              std::chrono::duration<double, std::milli>(now - _trace_start).count(),
                              event, echo, frame.can_id, frame.len, data);
    if (now - _trace_flushed >= std::chrono::seconds(1)) {
        _trace_out.flush();
        _trace_flushed = now;
    }
}


GsUsbDevice::Stats GsUsbDevice::take_stats() {
    {
        std::lock_guard<std::mutex> lock(_echo_mtx);
        _expire_echoes(Clock::now());
    }
    std::lock_guard<std::mutex> lock(_stats_mtx);
    return std::exchange(_stats, Stats{});
}


Status GsUsbDevice::recv(can_frame& frame, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_queue_mtx);
    if (!_queue_cv.wait_for(lock, timeout, [this] { return !_queue.empty() || _lost; })) {
        return Status::recv_timeout;
    }
    if (_queue.empty()) {
        return Status::recv_error; // adapter gone
    }
    frame = _queue.front();
    _queue.pop_front();
    return Status::ok;
}


} // namespace can::detail
