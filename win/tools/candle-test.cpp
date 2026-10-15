// candle-test: checks a candleLight adapter without the GUI.
//
//   candle-test -l                  list the adapters on USB
//   candle-test [-i candle0] [-b 250000] [-t 10] [-m normal|listen|loopback]
//               [-s 123#1122334455667788]
//
// Opens the adapter, prints the frames and the bus errors received for -t
// seconds (0: until Ctrl+C) and, with -s, sends a frame once a second: the id
// in hex (over 7FF it goes as extended), then up to 8 data bytes in hex.
//
// -m listen: receive only, sending no acknowledgements or error flags.
// -m loopback: a self-test; the adapter receives what it sends and nothing
// reaches the bus.

#include "../src/cansocket/gs_usb_device.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> interrupted{false};

void usage() {
    std::cerr << "usage: candle-test -l\n"
                 "       candle-test [-i candle0] [-b 250000] [-t 10] [-m normal|listen|loopback]\n"
                 "                   [-s 123#1122334455667788]\n";
}

std::optional<can_frame> parse_frame(const std::string& text) {
    const auto hash = text.find('#');
    if (hash == std::string::npos || hash == 0 || (text.size() - hash - 1) % 2 != 0
            || text.size() - hash - 1 > 2 * CAN_MAX_DLEN) {
        return std::nullopt;
    }
    can_frame frame{};
    try {
        frame.can_id = static_cast<canid_t>(std::stoul(text.substr(0, hash), nullptr, 16));
        frame.len = static_cast<uint8_t>((text.size() - hash - 1) / 2);
        for (size_t i = 0; i < frame.len; ++i) {
            frame.data[i] = static_cast<uint8_t>(std::stoul(text.substr(hash + 1 + 2 * i, 2), nullptr, 16));
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (frame.can_id > CAN_EFF_MASK) {
        return std::nullopt;
    }
    if (frame.can_id > CAN_SFF_MASK) {
        frame.can_id |= CAN_EFF_FLAG;
    }
    return frame;
}

// An error frame in words; the classes and bits of Linux <linux/can/error.h>,
// which gs_usb error frames follow.
std::string describe_error(const can_frame& frame) {
    std::string text;
    const auto add = [&](std::string_view what) {
        if (!text.empty()) {
            text += ", ";
        }
        text += what;
    };

    const canid_t classes = frame.can_id & CAN_EFF_MASK;
    if (classes & 0x001) add("tx timeout");
    if (classes & 0x002) add("lost arbitration");
    if (classes & 0x004) {
        const uint8_t c = frame.data[1];
        if (c & 0x01) add("rx overflow");
        if (c & 0x02) add("tx overflow");
        if (c & 0x04) add("rx warning");
        if (c & 0x08) add("tx warning");
        if (c & 0x10) add("rx error passive");
        if (c & 0x20) add("tx error passive");
        if (c & 0x40) add("error active again");
    }
    if (classes & 0x008) {
        const uint8_t p = frame.data[2];
        if (p & 0x01) add("bit error");
        if (p & 0x02) add("form error");
        if (p & 0x04) add("stuff error");
        if (p & 0x08) add("cannot send dominant bit");
        if (p & 0x10) add("cannot send recessive bit");
        if (p & 0x20) add("bus overload");
        if (p & 0x80) add("while sending");
        if (p == 0) add("protocol error");
    }
    if (classes & 0x010) add("transceiver");
    if (classes & 0x020) add("no acknowledgement");
    if (classes & 0x040) add("bus off");
    if (classes & 0x080) add("bus error");
    if (classes & 0x100) add("restarted");
    if (text.empty()) {
        text = "error";
    }
    return text + std::format(" (tx errors {}, rx errors {})", frame.data[6], frame.data[7]);
}

void print_frame(const char* direction, double seconds, const can_frame& frame) {
    if (frame.can_id & CAN_ERR_FLAG) {
        std::printf("%9.3f  ERR %s\n", seconds, describe_error(frame).c_str());
        std::fflush(stdout);
        return;
    }
    if (frame.can_id & CAN_EFF_FLAG) {
        std::printf("%9.3f  %s  %08X  [%u]", seconds, direction, frame.can_id & CAN_EFF_MASK, frame.len);
    } else {
        std::printf("%9.3f  %s       %03X  [%u]", seconds, direction, frame.can_id & CAN_SFF_MASK, frame.len);
    }
    if (frame.can_id & CAN_RTR_FLAG) {
        std::printf("  remote request");
    } else {
        for (unsigned i = 0; i < frame.len; ++i) {
            std::printf(" %02X", frame.data[i]);
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    bsclog::add_sink(std::shared_ptr<std::ostream>(&std::cout, [](void*) {}));

    std::string interface = "candle0";
    uint32_t bitrate = 250000;
    unsigned seconds = 10;
    std::optional<can_frame> tx_frame;
    can::detail::GsUsbDevice::Options options{.error_frames = true};

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        try {
            if (arg == "-l") {
                const auto adapters = can::detail::GsUsbDevice::list();
                if (adapters.empty()) {
                    std::cout << "No candleLight adapters on USB.\n";
                }
                for (const auto& adapter : adapters) {
                    std::cout << adapter << '\n';
                }
                return 0;
            } else if (arg == "-i" && has_value) {
                interface = argv[++i];
            } else if (arg == "-b" && has_value) {
                bitrate = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "-t" && has_value) {
                seconds = static_cast<unsigned>(std::stoul(argv[++i]));
            } else if (arg == "-m" && has_value) {
                const std::string mode = argv[++i];
                if (mode == "listen") {
                    options.listen_only = true;
                } else if (mode == "loopback") {
                    options.listen_only = true; // nothing on the bus
                    options.loopback = true;
                } else if (mode != "normal") {
                    usage();
                    return 2;
                }
            } else if (arg == "-s" && has_value) {
                tx_frame = parse_frame(argv[++i]);
                if (!tx_frame) {
                    std::cerr << "bad frame: " << argv[i] << '\n';
                    return 2;
                }
            } else {
                usage();
                return 2;
            }
        } catch (const std::exception&) {
            usage();
            return 2;
        }
    }

    if (interface.rfind("candle", 0) != 0 || interface.size() == 6) {
        std::cerr << "bad interface: " << interface << " (candle0, candle1, ...)\n";
        return 2;
    }
    const size_t index = std::stoul(interface.substr(6));

    auto device = can::detail::GsUsbDevice::open(index, bitrate, options);
    if (!device) {
        return 1;
    }
    std::cout << (options.loopback ? "Self-test, nothing goes to the bus. " : "")
              << (options.listen_only && !options.loopback ? "Listening only. " : "")
              << "Receiving" << (tx_frame ? " and sending once a second" : "")
              << (seconds ? " for " + std::to_string(seconds) + " s" : " until Ctrl+C") << ".\n";

    // Ctrl+C ends the loop, so the adapter is closed properly (on Linux it
    // goes back to the gs_usb driver, as can0)
    std::signal(SIGINT, [](int) { interrupted = true; });

    const auto start = std::chrono::steady_clock::now();
    auto next_send = start;
    unsigned received = 0;
    unsigned errors = 0;
    std::string last_error;
    double last_error_printed = -1;
    unsigned sent = 0;
    unsigned send_errors = 0;

    while (!interrupted && (*device)->good()) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        if (seconds != 0 && elapsed >= seconds) {
            break;
        }

        if (tx_frame && now >= next_send) {
            next_send += std::chrono::seconds(1);
            if ((*device)->send(*tx_frame) == can::Status::ok) {
                ++sent;
                print_frame("TX", elapsed, *tx_frame);
            } else {
                ++send_errors;
                std::printf("%9.3f  TX  failed (no other node acknowledges? wrong bitrate?)\n", elapsed);
            }
        }

        can_frame frame{};
        if ((*device)->recv(frame, std::chrono::milliseconds(100)) != can::Status::ok) {
            continue;
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (frame.can_id & CAN_ERR_FLAG) {
            // a bus at the wrong bitrate gives hundreds a second: print each
            // kind at most once a second
            ++errors;
            const std::string error = describe_error(frame);
            if (error != last_error || t - last_error_printed >= 1.0) {
                print_frame("RX", t, frame);
                last_error = error;
                last_error_printed = t;
            }
        } else {
            ++received;
            print_frame("RX", t, frame);
        }
    }

    std::cout << "Received " << received << " frames and " << errors << " error frames, sent " << sent
              << ", send errors " << send_errors << ".\n";
    return (*device)->good() ? 0 : 1;
}
