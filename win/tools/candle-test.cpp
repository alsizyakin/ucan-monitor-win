// candle-test: checks a candleLight adapter without the GUI.
//
//   candle-test -l                  list the adapters on USB
//   candle-test [-i candle0] [-b 250000] [-t 10] [-m normal|listen|loopback]
//               [-s 123#1122334455667788 [-p 1000] [-c]] [-w trace.csv]
//               [-f 1] [-R 0|1] [-r 0|1] [-P]
//
// Opens the adapter, prints the frames and the bus errors received for -t
// seconds (0: until Ctrl+C) and, with -s, sends a frame every -p ms (1000 by
// default): the id in hex (over 7FF it goes as extended), then up to 8 data
// bytes in hex. Below 100 ms the frames are not printed one by one, only a
// summary each second.
//
// -m listen: receive only, sending no acknowledgements or error flags.
// -m loopback: a self-test; the adapter receives what it sends and nothing
// reaches the bus.
// -c: data bytes 0-3 carry a frame number; in loopback the numbers that come
// back are checked for losses and order, and the round trip is timed.
// -w: write every frame sent, confirmed by the adapter and received to a CSV.
// -f: at most this many frames in the adapter not reported sent (0: any);
// -R: with -f 1, send again what the adapter lost; -r: RAW_IO for receiving
// on Windows; -P: the adapter pads its packets.

#include "../src/cansocket/gs_usb_device.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <utility>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> interrupted{false};

void usage() {
    std::cerr << "usage: candle-test -l\n"
                 "       candle-test [-i candle0] [-b 250000] [-t 10] [-m normal|listen|loopback]\n"
                 "                   [-s 123#1122334455667788 [-p 1000] [-c]] [-w trace.csv]\n"
                 "                   [-f 1] [-R 0|1] [-r 0|1] [-P]\n";
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
    // the error counters, when the firmware fills them in
    if (classes & 0x200) {
        text += std::format(" (tx errors {}, rx errors {})", frame.data[6], frame.data[7]);
    }
    return text;
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
    unsigned period_ms = 1000;
    bool numbered = false;
    can::detail::GsUsbDevice::Options options;
    options.error_frames = true;

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
            } else if (arg == "-p" && has_value) {
                period_ms = static_cast<unsigned>(std::stoul(argv[++i]));
                if (period_ms == 0) {
                    usage();
                    return 2;
                }
            } else if (arg == "-f" && has_value) {
                options.tx_in_flight = static_cast<unsigned>(std::stoul(argv[++i]));
            } else if (arg == "-R" && has_value) {
                options.resend = std::stoul(argv[++i]) != 0;
            } else if (arg == "-r" && has_value) {
                options.raw_io = std::stoul(argv[++i]) != 0;
            } else if (arg == "-P") {
                options.pad_packets = true;
            } else if (arg == "-w" && has_value) {
                options.trace_file = argv[++i];
            } else if (arg == "-c") {
                numbered = true;
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
    if (numbered && tx_frame && tx_frame->len < 4) {
        tx_frame->len = 4; // room for the number
    }

    std::cout << "candle-test " UCAN_WIN_VERSION ".\n";
    auto device = can::detail::GsUsbDevice::open(index, bitrate, options);
    if (!device) {
        return 1;
    }
    auto& dev = **device;

    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::milliseconds(period_ms);
    const bool quiet = tx_frame && period < std::chrono::milliseconds(100);

    std::cout << (options.loopback ? "Self-test, nothing goes to the bus. " : "")
              << (options.listen_only && !options.loopback ? "Listening only. " : "")
              << "Receiving"
              << (tx_frame ? " and sending every " + std::to_string(period_ms) + " ms" : "")
              << (seconds ? " for " + std::to_string(seconds) + " s" : " until Ctrl+C") << ".\n";

    // Ctrl+C ends the loop, so the adapter is closed properly (on Linux it
    // goes back to the gs_usb driver, as can0)
    std::signal(SIGINT, [](int) { interrupted = true; });

    const auto start = Clock::now();
    auto next_send = start;
    auto next_summary = start + std::chrono::seconds(1);
    unsigned received = 0;
    unsigned errors = 0;
    std::string last_error;
    double last_error_printed = -1;
    unsigned sent = 0;
    unsigned send_errors = 0;

    // numbered frames: the next number to send, the next expected back, and
    // when each number in flight was sent
    uint32_t next_number = 0;
    uint32_t expected_back = 0;
    unsigned came_back = 0;
    unsigned lost = 0;
    unsigned out_of_order = 0;
    std::deque<std::pair<uint32_t, Clock::time_point>> in_flight;
    std::chrono::microseconds max_round_trip{0};
    std::chrono::microseconds sum_round_trip{0};

    const auto seconds_since_start = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };

    while (!interrupted && dev.good()) {
        const auto now = Clock::now();
        if (seconds != 0 && now - start >= std::chrono::seconds(seconds)) {
            break;
        }

        if (tx_frame && now >= next_send) {
            next_send += period;
            if (now - next_send > std::chrono::seconds(1)) {
                next_send = now + period; // far behind: no burst to catch up
            }
            can_frame frame = *tx_frame;
            if (numbered) {
                std::memcpy(frame.data, &next_number, sizeof(next_number));
            }
            if (dev.send(frame) == can::Status::ok) {
                ++sent;
                if (numbered) {
                    in_flight.emplace_back(next_number, now);
                }
                if (!quiet) {
                    print_frame("TX", seconds_since_start(), frame);
                }
            } else {
                ++send_errors;
                std::printf("%9.3f  TX  failed: the send queue is full or the adapter is gone\n",
                            seconds_since_start());
            }
            ++next_number;
        }

        if (quiet && now >= next_summary) {
            next_summary += std::chrono::seconds(1);
            std::printf("%9.3f  sent %u, received %u", seconds_since_start(), sent, received);
            if (numbered && options.loopback) {
                std::printf(", came back %u, lost %u, out of order %u", came_back, lost, out_of_order);
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        auto wait = std::chrono::milliseconds(100);
        if (tx_frame) {
            wait = std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(next_send - Clock::now()),
                              std::chrono::milliseconds(0), wait);
        }
        can_frame frame{};
        if (dev.recv(frame, wait) != can::Status::ok) {
            continue;
        }
        const auto arrived = Clock::now();
        const double t = std::chrono::duration<double>(arrived - start).count();

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
            continue;
        }

        ++received;
        if (!quiet) {
            print_frame("RX", t, frame);
        }

        if (numbered && options.loopback && tx_frame && frame.can_id == tx_frame->can_id && frame.len >= 4) {
            uint32_t number;
            std::memcpy(&number, frame.data, sizeof(number));
            ++came_back;
            if (number == expected_back) {
                ++expected_back;
            } else if (number > expected_back) {
                lost += number - expected_back;
                expected_back = number + 1;
            } else {
                ++out_of_order;
            }
            while (!in_flight.empty() && in_flight.front().first < number) {
                in_flight.pop_front();
            }
            if (!in_flight.empty() && in_flight.front().first == number) {
                const auto trip = std::chrono::duration_cast<std::chrono::microseconds>(arrived - in_flight.front().second);
                max_round_trip = std::max(max_round_trip, trip);
                sum_round_trip += trip;
                in_flight.pop_front();
            }
        }
    }

    std::cout << "Received " << received << " frames and " << errors << " error frames, sent " << sent
              << ", send errors " << send_errors << ".\n";
    if (numbered && options.loopback) {
        // frames still on their way when the time ran out are not lost
        std::cout << std::format("Self-test: came back {} of {}, lost {}, out of order {}, "
                                 "round trip {:.2f} ms on average, {:.2f} ms at most.\n",
                                 came_back, sent, lost, out_of_order,
                                 came_back ? sum_round_trip.count() / 1000.0 / came_back : 0.0,
                                 max_round_trip.count() / 1000.0);
    }
    const auto stats = dev.take_stats();
    std::cout << std::format("USB: sent {}, confirmed by the adapter {}, resent {}, unconfirmed {}, late echoes {}, "
                             "failed {}{}, dropped {}, longest send {:.2f} ms, longest send queue {}; "
                             "received {}, dropped {}, adapter overflows {}, bus errors {}.\n",
                             stats.sent, stats.confirmed, stats.resent, stats.unconfirmed, stats.late_echoes,
                             stats.send_failed,
                             stats.last_send_error.empty() ? "" : " (" + stats.last_send_error + ")",
                             stats.send_dropped, stats.max_send_time.count() / 1000.0, stats.max_send_queue,
                             stats.received, stats.receive_dropped, stats.adapter_overflows, stats.bus_errors);
    return dev.good() ? 0 : 1;
}
