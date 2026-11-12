#include <ucanopen/server/services/watch/server_watch_service.h>
#include <iostream>
#include <stdexcept>

using namespace ucanopen;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        ObjectDictionary dictionary{
            .config = {.watch_category = "watch", .config_category = "config"},
            .entries = {
                {{0x2000, 1}, {"watch", "temp", "missing", "C", OD_ACCESS_RO, OD_FLOAT32}},
                {{0x2000, 2}, {"watch", "temp", "present", "C", OD_ACCESS_RO, OD_FLOAT32}},
                {{0x3000, 1}, {"config", "sys", "setting", "", OD_ACCESS_RW, OD_UINT32}},
            },
        };
        // No socket: no test operation can send anything to a real device.
        impl::Server server(nullptr, NodeId{1}, "test", dictionary);
        ServerSdoService sdo(server);
        ServerWatchService watch(server, sdo);
        const auto abort = [&](uint16_t index, uint8_t subindex, SdoAbortCode code) {
            AbortSdo message;
            message.cs = sdo_cs_codes::abort;
            message.index = index;
            message.subindex = subindex;
            message.error_code = std::to_underlying(code);
            return sdo.handle_frame(create_frame(Cob::tsdo, NodeId{1}, message.to_payload()));
        };
        require(watch.daq_enabled(0) && watch.daq_enabled(1), "watch initially enabled");
        abort(0x2000, 1, SdoAbortCode::hardware_error);
        require(watch.daq_enabled(0), "temporary errors retain polling");
        abort(0x3000, 1, SdoAbortCode::object_not_found);
        require(watch.daq_enabled(0) && watch.daq_enabled(1), "config abort does not disable Watch");
        require(abort(0x2000, 1, SdoAbortCode::object_not_found) == FrameHandlingStatus::success,
                "real SDO abort payload reaches Watch subscriber");
        require(!watch.daq_enabled(0) && watch.daq_enabled(1), "only missing Watch object disabled");
        require(watch.string_value("temp", "missing") == "не поддерживается", "visible unavailable value");
        watch.set_period(std::chrono::milliseconds(0));
        watch.send(); // cursor 0 skips the absent object, with no socket call
        abort(0x2000, 1, SdoAbortCode::object_not_found);
        require(!watch.daq_enabled(0), "repeated abort remains disabled");
        watch.toggle_daq(0, true);
        require(watch.daq_enabled(0), "user may explicitly retry without editing dictionaries");
        const auto entry = dictionary.entries.find({0x2000, 1});
        watch.handle_sdo(entry, SdoType::response_to_read, ExpeditedSdoData(42.0f));
        require(watch.value("temp", "missing").f32() == 42.0f, "successful retry updates data");
        std::cout << "Watch abort handling passed (no USB device used).\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
