#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "virtual_device/MscConstants.h"
#include "virtual_device/VirtualInterfaceHandler.h"
#include "virtual_device/storage_backends/StorageBackend.h"
#include "virtual_device/storage_backends/StorageIoTransfer.h"

namespace usbipdcpp {

class StorageTransferOperator;

/** Identification strings returned by SCSI INQUIRY / VPD.
 *  An empty string means the value is auto-read from VirtualDeviceHandler's USB descriptors. */
struct MscConfig {
    std::string vendor; // INQUIRY 8-byte vendor name
    std::string product; // INQUIRY 16-byte product name
    std::string revision; // INQUIRY 4-byte revision
    std::string serial; // VPD 0x80 serial number
};

/** MSC Bulk-Only Transport protocol handler.
 *
 * BOT is a synchronous protocol (CBW→Data→CSW); all IN data is ready when the CBW is received,
 * so host IN requests can be responded to immediately. Therefore, no EndpointRequestQueue is needed
 * (unlike HID/CDC ACM and other devices that produce data asynchronously and need a queue to hold IN requests
 * until data is ready). */
class USBIPDCPP_API MscBulkOnlyHandler : public VirtualInterfaceHandler {
public:
    MscBulkOnlyHandler(UsbInterface &handle_interface, StringPool &string_pool, std::unique_ptr<StorageBackend> backend,
                       MscConfig config = {}, bool read_only = false);

    /** Bulk-Only Transport core: sends IN data (DataIn/Status); OUT is ack-only (data is already handled in on_out_data_received) */
    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    /** Provide the destination buffer for OUT transfers (Idle→fallback / DataOut→mmap or staging) */
    void *prepare_out_buffer(std::size_t length, StorageIoTransfer *trx);
    /** Callback after OUT data is fully received; drives the BOT state machine: CBW parsing, disk write, or UNMAP */
    void on_out_data_received(StorageIoTransfer *trx, std::size_t length);

    StorageBackend *get_backend() const {
        return backend_.get();
    }

    /** Callback after device_handler is set; fills empty MscConfig fields from USB strings */
    void on_setup_interface_handlers() override;
    /** Reset the BOT state machine on client connection */
    void on_new_connection(Session &current_session, error_code &ec) override;
    /** Reset the BOT state machine on client disconnection */
    void on_disconnection(error_code &ec) override;

private:
    std::unique_ptr<StorageBackend> backend_;
    bool read_only_ = false;
    MscConfig config_; // Empty fields are filled in on_setup_interface_handlers

    /** BOT state machine: Idle → DataIn/DataOut → Status → Idle */
    BotState state_ = BotState::Idle;
    /** Most recently received CBW; the CSW must echo back dCBWTag as-is */
    CBW current_cbw_{};

    /** IN/OUT data staging or zero-copy mmap pointer.
     *  Clearing is deferred until the next CBW (Idle branch) to prevent the previous command's sender thread from still reading */
    std::vector<std::uint8_t> staging_data_;
    std::size_t staging_offset_ = 0; // Bytes already sent during IN transfer (shared by staging / mmap)

    /** Transfer residue: dCBWDataTransferLength - actual bytes transferred; needed by CSW */
    std::uint32_t data_residue_ = 0;
    /** Set to true during CBW parsing phase; cleared after CSW is sent in Status */
    bool command_failed_ = false;

    /** Target LBA and block count for WRITE command (10-byte CDB, LBA is 32-bit) */
    std::uint64_t write_lba_ = 0;
    std::uint16_t write_count_ = 0;
    /** When true, DataOut goes through UNMAP descriptor parsing instead of WRITE disk write */
    bool data_out_unmap_ = false;

    /** READ zero-copy: mmap base address, starting LBA, total byte count */
    std::uint64_t read_lba_ = 0;
    void *read_mmap_base_ = nullptr;
    std::size_t read_total_size_ = 0;
    /** WRITE zero-copy: mmap base address, bytes accumulated so far */
    void *write_mmap_base_ = nullptr;
    std::size_t write_accumulated_ = 0;

    void send_stall(std::uint32_t seqnum);

public:
    // ========== Standard request default implementations ==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    [[nodiscard]] data_type get_class_specific_descriptor() override {
        return {};
    }
};

} // namespace usbipdcpp
