// Checks calculate_bittiming() against settings worked out by hand.

#include "../src/cansocket/bittiming.h"

#include <cstdio>

using can::detail::BittimingLimits;
using can::detail::calculate_bittiming;

namespace {

int failures = 0;

void expect(uint32_t bitrate, const BittimingLimits& limits,
            uint32_t brp, uint32_t tseg1, uint32_t tseg2, uint32_t sjw, uint32_t sample_point) {
    const auto t = calculate_bittiming(bitrate, limits);
    if (!t) {
        std::printf("FAIL %u bit/s: no setting\n", bitrate);
        ++failures;
        return;
    }
    const bool ok = t->brp == brp && t->tseg1 == tseg1 && t->tseg2 == tseg2 && t->sjw == sjw
                 && t->sample_point == sample_point && t->bitrate == bitrate;
    std::printf("%s %u bit/s: brp %u, tseg1 %u, tseg2 %u, sjw %u, sample point %u, bitrate %u\n",
                ok ? "ok  " : "FAIL", bitrate, t->brp, t->tseg1, t->tseg2, t->sjw, t->sample_point, t->bitrate);
    failures += ok ? 0 : 1;
}

void expect_none(uint32_t bitrate, const BittimingLimits& limits) {
    const auto t = calculate_bittiming(bitrate, limits);
    std::printf("%s %u bit/s: %s\n", t ? "FAIL" : "ok  ", bitrate, t ? "got a setting" : "refused");
    failures += t ? 1 : 0;
}

} // namespace

int main() {
    // bxCAN of the STM32F042/F072 in candleLight, as its firmware reports it
    const BittimingLimits stm32f0{
        .fclk = 48000000,
        .tseg1_min = 1, .tseg1_max = 16,
        .tseg2_min = 1, .tseg2_max = 8,
        .sjw_max = 4,
        .brp_min = 1, .brp_max = 1024, .brp_inc = 1,
    };

    // 16 time quanta: 87.5 % = (1 + 13) / 16, 75 % = (1 + 11) / 16
    expect(125000, stm32f0, 24, 13, 2, 1, 875);
    expect(250000, stm32f0, 12, 13, 2, 1, 875);
    expect(500000, stm32f0, 6, 13, 2, 1, 875);
    expect(1000000, stm32f0, 3, 11, 4, 2, 750);

    // 990 kbit/s: the nearest 48 MHz can make is 1 Mbit/s, 1.01 % off
    expect_none(990000, stm32f0);
    // a clock too slow for the bitrate
    expect_none(1000000, BittimingLimits{.fclk = 1000000, .tseg1_min = 1, .tseg1_max = 16,
                                         .tseg2_min = 1, .tseg2_max = 8, .sjw_max = 4,
                                         .brp_min = 1, .brp_max = 1024, .brp_inc = 1});

    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
