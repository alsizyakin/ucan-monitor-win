// Exercises the actual transmit state machine without opening a USB device.
// Only submission is replaced; USB completions and CAN echoes are independent.
#include "../src/cansocket/gs_usb_device.h"
#include <libusb.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace can::detail {

struct GsUsbDeviceTest {
    using Device = GsUsbDevice;
    using Clock = Device::Clock;
    using Slot = Device::TxSlot;
    static inline std::vector<Slot*> submitted;
    static inline int submit_result = 0;

    static void require(bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    }

    static int submit(libusb_transfer* transfer) {
        if (submit_result == 0) submitted.push_back(static_cast<Slot*>(transfer->user_data));
        return submit_result;
    }

    static std::unique_ptr<Device> device(unsigned window = 1) {
        submitted.clear();
        submit_result = 0;
        auto dev = std::unique_ptr<Device>(new Device);
        dev->_submit_transfer = &submit;
        dev->_options.tx_in_flight = window;
        for (int i = 0; i < 8; ++i) {
            auto slot = std::make_unique<Slot>();
            slot->device = dev.get();
            slot->transfer = libusb_alloc_transfer(0);
            require(slot->transfer != nullptr, "allocate transfer");
            slot->transfer->user_data = slot.get();
            slot->transfer->length = 20;
            dev->_tx_idle.push_back(slot.get());
            dev->_tx_slots.push_back(std::move(slot));
        }
        return dev;
    }

    static uint32_t echo_id(Slot* slot) {
        uint32_t id;
        std::memcpy(&id, slot->frame.bytes.data(), sizeof(id));
        return id;
    }

    static void complete(Device& dev, Slot* slot,
                         libusb_transfer_status status = LIBUSB_TRANSFER_COMPLETED, int bytes = 20) {
        slot->transfer->status = status;
        slot->transfer->actual_length = bytes;
        dev._on_tx(slot);
    }

    static can_frame frame(uint32_t sequence = 0) {
        can_frame f{};
        f.can_id = 0x123;
        f.len = 8;
        std::memcpy(f.data, &sequence, sizeof(sequence));
        return f;
    }

    static void sustained() {
        auto dev = device();
        for (uint32_t i = 0; i < 50000; ++i) {
            require(dev->send(frame(i)) == Status::ok, "sustained send");
            auto* slot = submitted.back();
            const auto echo = echo_id(slot);
            uint32_t payload;
            std::memcpy(&payload, slot->frame.bytes.data() + 12, sizeof(payload));
            require(payload == i, "frame order and data");
            // USB completion can arrive before or after the CAN echo.
            if (i % 2) {
                complete(*dev, slot);
                dev->_on_echo(echo, Clock::now());
            } else {
                dev->_on_echo(echo, Clock::now());
                complete(*dev, slot);
            }
            require(dev->_in_adapter.empty(), "echo releases exactly one credit");
        }
        const auto stats = dev->take_stats();
        require(stats.sent == 50000 && stats.confirmed == 50000, "sustained counts");
        require(stats.resent == 0 && stats.send_dropped == 0, "no duplicates or drops");
        require(dev->_pending.empty() && dev->_tx_queue.empty(), "bounded bookkeeping");
    }

    static void delayed_echo() {
        auto dev = device();
        dev->_options.resend = true; // even a legacy setting cannot resend
        dev->send(frame());
        auto* first = submitted.back();
        const auto id = echo_id(first);
        complete(*dev, first);
        dev->send(frame(1));
        dev->_check_in_adapter(Clock::now() + std::chrono::milliseconds(60));
        dev->_drain_tx();
        require(submitted.size() == 1 && dev->_in_adapter.size() == 1, "USB completion and time are not CAN credits");
        dev->_on_echo(id, Clock::now());
        require(submitted.size() == 2, "matching echo releases queued frame");
        // Duplicate/unknown echoes must not release another frame's credit.
        dev->send(frame(2));
        dev->_on_echo(id, Clock::now());
        dev->_on_echo(1234567, Clock::now());
        require(submitted.size() == 2 && dev->_in_adapter.size() == 1, "unknown echo grants no credit");
    }

    static void missing_echo(unsigned window) {
        auto dev = device(window);
        for (int i = 0; i < 100; ++i) {
            const auto count = submitted.size();
            dev->send(frame(i));
            if (submitted.size() > count) complete(*dev, submitted.back());
        }
        require(submitted.size() == 8 && dev->_in_adapter.size() == 8, "hard cap, including zero/unlimited configuration");
        dev->_check_in_adapter(Clock::now() + std::chrono::seconds(3));
        require(!dev->good() && dev->_stopping, "missing echo faults the connection");
        require(dev->_in_adapter.size() == 8, "timeout never forgets occupied buffers");
        require(dev->_tx_queue.empty(), "fault discards stale queue");
        require(dev->send(frame()) == Status::send_error, "fault rejects new frames");
        dev->_on_echo(0, Clock::now());
        require(submitted.size() == 8, "late echo cannot restart a faulted connection");
        can_frame f{};
        require(dev->recv(f, std::chrono::seconds(10)) == Status::recv_error, "fault wakes receiver immediately");
    }

    static void failed_usb() {
        for (const auto status : {LIBUSB_TRANSFER_TIMED_OUT, LIBUSB_TRANSFER_STALL,
                                  LIBUSB_TRANSFER_CANCELLED, LIBUSB_TRANSFER_NO_DEVICE,
                                  LIBUSB_TRANSFER_COMPLETED}) {
            auto dev = device();
            dev->send(frame());
            auto* slot = submitted.back();
            dev->send(frame(1));
            complete(*dev, slot, status, 10); // partial delivery is ambiguous
            require(!dev->good(), "failed or short USB transfer faults the connection");
            require(submitted.size() == 1 && dev->_in_adapter.size() == 1, "ambiguous transfer cannot grant credit or replay");
        }
        auto dev = device();
        submit_result = LIBUSB_ERROR_IO;
        require(dev->send(frame()) == Status::send_error && !dev->good(), "submission failure is returned");
        require(dev->_tx_active == 0, "failed submission is not active");
    }

    static void transient_congestion() {
        auto dev = device();
        dev->send(frame());
        auto* first = submitted.back();
        const auto id = echo_id(first);
        complete(*dev, first);
        dev->send(frame(1));
        dev->_tx_queue.front().queued -= std::chrono::milliseconds(119);
        dev->_drain_tx();
        require(dev->good() && submitted.size() == 1, "119 ms pause keeps connection and preserves the CAN credit");
        dev->_drain_tx();
        require(dev->take_stats().slow_queue_events == 1, "one diagnostic per congestion episode");
        dev->_on_echo(id, Clock::now());
        require(submitted.size() == 2 && dev->good(), "late CAN confirmation resumes FIFO without reconnect");
        auto* second = submitted.back();
        const auto next_id = echo_id(second);
        complete(*dev, second);
        dev->_on_echo(next_id, Clock::now());
        require(dev->_tx_queue.empty() && dev->_in_adapter.empty(), "recovered queue drains completely");
        require(dev->take_stats().send_dropped == 0, "transient congestion drops no commands");
    }

    static void queue_bound() {
        auto dev = device();
        dev->send(frame());
        complete(*dev, submitted.back());
        for (int i = 0; i < 256; ++i) require(dev->send(frame(i)) == Status::ok, "queue within bound");
        require(dev->send(frame()) == Status::send_error, "full queue returns failure");
        require(dev->_tx_queue.size() == 256 && dev->_pending.size() == 257, "rejected frames do not grow bookkeeping");
    }

    static void run() {
        sustained();
        delayed_echo();
        missing_echo(0);
        missing_echo(100);
        failed_usb();
        transient_congestion();
        queue_bound();
    }
};

} // namespace can::detail

int main() {
    try {
        can::detail::GsUsbDeviceTest::run();
        std::cout << "Transport tests passed (no USB device used).\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
