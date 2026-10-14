// Stand-in for the Linux <linux/can.h>, which Windows does not have.
//
// The upstream code takes can_frame and canid_t from <linux/can.h>. This
// directory comes first on the include path, so every upstream
// #include <linux/can.h> lands here. The layout matches the Linux header.
#pragma once

#include <cstdint>

using canid_t = uint32_t;

#define CAN_EFF_FLAG 0x80000000U // extended frame format (29-bit id)
#define CAN_RTR_FLAG 0x40000000U // remote transmission request
#define CAN_ERR_FLAG 0x20000000U // error frame

#define CAN_SFF_MASK 0x000007FFU // standard frame format (11-bit id)
#define CAN_EFF_MASK 0x1FFFFFFFU // extended frame format (29-bit id)

#define CAN_MAX_DLEN 8

struct can_frame {
    canid_t can_id; // id plus the EFF/RTR/ERR flags
    union {
        uint8_t len;
        uint8_t can_dlc; // deprecated name of len, still used upstream
    };
    uint8_t pad;
    uint8_t res0;
    uint8_t len8_dlc;
    alignas(8) uint8_t data[CAN_MAX_DLEN];
};

static_assert(sizeof(can_frame) == 16);
