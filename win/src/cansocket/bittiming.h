#pragma once


#include <cstdint>
#include <optional>


namespace can::detail {


// What the CAN controller of an adapter allows, as gs_usb reports it.
struct BittimingLimits {
    uint32_t fclk;  // CAN controller clock, Hz
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
};


// One bit is 1 + tseg1 + tseg2 time quanta of brp controller clocks each;
// the bus is sampled after 1 + tseg1 of them.
struct Bittiming {
    uint32_t brp;
    uint32_t tseg1;
    uint32_t tseg2;
    uint32_t sjw;
    uint32_t bitrate;       // what these settings give, bit/s
    uint32_t sample_point;  // per mille of the bit
};


// Picks the settings closest to bitrate, then to the sample point Linux
// aims at for it (87.5 %, 80 % above 500 kbit/s, 75 % above 800 kbit/s), so
// that the adapter samples the bus as SocketCAN would. Fails when no
// setting comes within 1 % of bitrate.
std::optional<Bittiming> calculate_bittiming(uint32_t bitrate, const BittimingLimits& limits);


} // namespace can::detail
