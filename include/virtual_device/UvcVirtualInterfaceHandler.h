#pragma once

#include <memory>
#include <vector>

#include "Export.h"
#include "virtual_device/UvcConstants.h"
#include "virtual_device/VirtualDeviceHandler.h"
#include "virtual_device/VirtualInterfaceHandler.h"
#include "virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// PROBE/COMMIT negotiation structure (UVC 1.5, 48 bytes)
struct UvcStreamingControl {
    std::uint16_t bmHint = 0;
    std::uint8_t bFormatIndex = 1;
    std::uint8_t bFrameIndex = 1;
    std::uint32_t dwFrameInterval = 333333;
    std::uint16_t wKeyFrameRate = 0;
    std::uint16_t wPFrameRate = 0;
    std::uint16_t wCompQuality = 0;
    std::uint16_t wCompWindowSize = 0;
    std::uint16_t wDelay = 0;
    std::uint32_t dwMaxVideoFrameSize = 0;
    std::uint32_t dwMaxPayloadTransferSize = 0;
    std::uint32_t dwClockFrequency = 27000000;
    std::uint8_t bmFramingInfo = 0x03;
    std::uint8_t bPreferredVersion = 1;
    std::uint8_t bMinVersion = 1;
    std::uint8_t bMaxVersion = 1;
    // UVC 1.5 additional fields
    std::uint8_t bUsage = 0;
    std::uint8_t bBitDepthLuma = 0;
    std::uint8_t bmSettings = 0;
    std::uint8_t bMaxNumberOfRefFramesPlus1 = 0;
    std::uint16_t bmRateControlModes = 0;
    std::uint64_t bmLayoutPerStream = 0;

    static constexpr std::size_t SIZE = 48; // UVC 1.5 full size

    data_type serialize() const;
    void deserialize(const std::uint8_t *data, std::size_t len);
};

class UvcVideoStreamingHandler;

/// VideoControl interface handler — handles PU attribute queries/settings + coordination with VS interface
class USBIPDCPP_API UvcVideoControlHandler : public VirtualInterfaceHandler {
public:
    explicit UvcVideoControlHandler(UsbInterface &handle_interface, StringPool &string_pool);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;
    std::uint16_t request_get_status(std::uint32_t *p_status) override;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

    void set_vs_handler(UvcVideoStreamingHandler *handler) {
        vs_handler_ = handler;
    }

private:
    void build_class_descriptor();
    void send_vc_status(data_type status);

    data_type class_desc_;
    bool desc_built_ = false;
    bool power_on_ = true;
    UvcVideoStreamingHandler *vs_handler_ = nullptr;

    std::deque<data_type> pending_status_;
    mutable std::mutex status_mutex_;
};

/// VideoStreaming interface handler — PROBE/COMMIT + ISO stream push
class USBIPDCPP_API UvcVideoStreamingHandler : public VirtualInterfaceHandler {
public:
    UvcVideoStreamingHandler(UsbInterface &handle_interface, StringPool &string_pool,
                             std::unique_ptr<VideoSource> source);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer, int num_iso_packets,
                                     std::error_code &ec) override;
    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;
    std::uint16_t request_get_status(std::uint32_t *p_status) override;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;

    VideoSource *get_source() {
        return source_.get();
    }

    void set_vc_handler(UvcVideoControlHandler *handler) {
        vc_handler_ = handler;
    }

    /// VC handler notifies to stop streaming (e.g. Video Power Mode off)
    void notify_stop_streaming() {
        streaming_ = false;
    }

private:
    void build_class_descriptor();

    UvcVideoControlHandler *vc_handler_ = nullptr;

    std::unique_ptr<VideoSource> source_;
    data_type class_desc_;
    UvcStreamingControl probe_data_{};
    bool committed_ = false;
    bool streaming_ = false;

    // Frame buffer
    std::vector<std::uint8_t> frame_buffer_;
    std::size_t frame_offset_ = 0;
    bool current_fid_ = false;
};

/// UVC device helper class — registers VC/VS interface handlers on device and sets descriptors
class USBIPDCPP_API UvcDeviceHelper {
public:
    /// Inject UVC interface handlers into device.
    /// device must already have two interfaces (VC + VS), and the second interface must contain an ISO IN endpoint
    static void setup(std::shared_ptr<UsbDevice> device, StringPool &string_pool, std::unique_ptr<VideoSource> source);
};

} // namespace usbipdcpp
