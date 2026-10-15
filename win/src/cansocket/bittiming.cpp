#include "bittiming.h"

#include <algorithm>


namespace can::detail {


namespace {


uint32_t nominal_sample_point(uint32_t bitrate) {
    if (bitrate > 800000) {
        return 750;
    }
    if (bitrate > 500000) {
        return 800;
    }
    return 875;
}


uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }


} // namespace


std::optional<Bittiming> calculate_bittiming(uint32_t bitrate, const BittimingLimits& limits) {
    if (bitrate == 0 || limits.fclk == 0 || limits.brp_inc == 0) {
        return std::nullopt;
    }

    const uint32_t sample_point = nominal_sample_point(bitrate);
    const uint32_t tq_max = 1 + limits.tseg1_max + limits.tseg2_max;
    const uint32_t tq_min = 1 + limits.tseg1_min + limits.tseg2_min;

    std::optional<Bittiming> best;
    uint64_t best_rate_error = UINT64_MAX;
    uint64_t best_sp_error = UINT64_MAX;

    // More time quanta per bit first: on a tie the finer setting wins.
    for (uint32_t tq = tq_max; tq >= tq_min && tq > 0; --tq) {
        const uint64_t clocks_per_bit_tq = uint64_t{bitrate} * tq;
        uint64_t brp = (limits.fclk + clocks_per_bit_tq / 2) / clocks_per_bit_tq;
        brp = (brp + limits.brp_inc / 2) / limits.brp_inc * limits.brp_inc;
        if (brp < limits.brp_min || brp > limits.brp_max || brp == 0) {
            continue;
        }

        const uint64_t actual_bitrate = limits.fclk / (brp * tq);
        const uint64_t rate_error = distance(actual_bitrate, bitrate);
        if (rate_error > best_rate_error) {
            continue;
        }

        // sample point = (1 + tseg1) / tq
        uint32_t tseg2 = tq - (sample_point * tq + 500) / 1000;
        tseg2 = std::clamp(tseg2, limits.tseg2_min, limits.tseg2_max);
        uint32_t tseg1 = tq - 1 - tseg2;
        if (tseg1 > limits.tseg1_max) {
            tseg1 = limits.tseg1_max;
        } else if (tseg1 < limits.tseg1_min) {
            tseg1 = limits.tseg1_min;
        }
        tseg2 = tq - 1 - tseg1;
        if (tseg2 < limits.tseg2_min || tseg2 > limits.tseg2_max) {
            continue;
        }

        const uint32_t actual_sp = 1000 * (1 + tseg1) / tq;
        const uint64_t sp_error = distance(actual_sp, sample_point);
        if (rate_error == best_rate_error && sp_error >= best_sp_error) {
            continue;
        }

        best_rate_error = rate_error;
        best_sp_error = sp_error;
        best = Bittiming{
            .brp = static_cast<uint32_t>(brp),
            .tseg1 = tseg1,
            .tseg2 = tseg2,
            // as Linux does by default: half of the phase after the sample point
            .sjw = std::clamp(tseg2 / 2, uint32_t{1}, std::max(limits.sjw_max, uint32_t{1})),
            .bitrate = static_cast<uint32_t>(actual_bitrate),
            .sample_point = actual_sp,
        };
    }

    if (!best || best_rate_error * 100 > bitrate) {
        return std::nullopt;
    }
    return best;
}


} // namespace can::detail
