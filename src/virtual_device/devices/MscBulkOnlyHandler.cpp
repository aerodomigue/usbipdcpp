#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#endif

#include "virtual_device/devices/MscBulkOnlyHandler.h"

#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

#include "Session.h"
#include "SetupPacket.h"
#include "constant.h"
#include "virtual_device/VirtualDeviceHandler.h"
#include "virtual_device/storage_backends/StorageTransferOperator.h"

using namespace usbipdcpp;

static std::string wstr_to_ascii(const std::wstring &ws, const std::string &fallback) {
    std::string result;
    for (wchar_t c: ws) {
        if (c > 0 && c < 128)
            result += static_cast<char>(c);
    }
    return result.empty() ? fallback : result;
}

MscBulkOnlyHandler::MscBulkOnlyHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                       std::unique_ptr<StorageBackend> backend, MscConfig config, bool read_only) :
    VirtualInterfaceHandler(handle_interface, string_pool, std::make_unique<StorageTransferOperator>(this)),
    backend_(std::move(backend)), read_only_(read_only), config_(std::move(config)) {
}

void MscBulkOnlyHandler::on_setup_interface_handlers() {
    if (config_.vendor.empty())
        config_.vendor = wstr_to_ascii(device_handler->get_string_manufacturer(), "USBIPDC ");
    if (config_.product.empty())
        config_.product = wstr_to_ascii(device_handler->get_string_product(), "USB Flash Drive ");
    if (config_.serial.empty())
        config_.serial = wstr_to_ascii(device_handler->get_string_serial(), "USBIPDCPSN");
    if (config_.revision.empty())
        config_.revision = "1.00";
}

void MscBulkOnlyHandler::on_new_connection(Session &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    state_ = BotState::Idle;
    current_cbw_ = {};
    staging_data_.clear();
    staging_offset_ = 0;
    data_residue_ = 0;
    command_failed_ = false;
    data_out_unmap_ = false;
    read_mmap_base_ = nullptr;
    read_total_size_ = 0;
    write_mmap_base_ = nullptr;
    write_accumulated_ = 0;
}

void MscBulkOnlyHandler::on_disconnection(error_code &ec) {
    state_ = BotState::Idle;
    current_cbw_ = {};
    staging_data_.clear();
    staging_offset_ = 0;
    data_residue_ = 0;
    command_failed_ = false;
    data_out_unmap_ = false;
    read_mmap_base_ = nullptr;
    read_total_size_ = 0;
    write_mmap_base_ = nullptr;
    write_accumulated_ = 0;
    VirtualInterfaceHandler::on_disconnection(ec);
}

void MscBulkOnlyHandler::handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                      std::uint32_t transfer_flags,
                                                                      std::uint32_t transfer_buffer_length,
                                                                      const SetupPacket &setup_packet,
                                                                      TransferHandle transfer, std::error_code &ec) {
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

/** Provide the destination buffer for OUT transfers; called by StorageTransferOperator::alloc_transfer_handle.
 *  Idle: CBW goes to fallback_data (returns nullptr)
 *  DataOut: write data directly into mmap or accumulate to staging
 *  Other states are protocol errors; log a warning */
void *MscBulkOnlyHandler::prepare_out_buffer(std::size_t length, StorageIoTransfer *trx) {
    SPDLOG_DEBUG("MSC::prepare_out len={} state={}", length, static_cast<int>(state_));
    switch (state_) {
        case BotState::Idle:
            SPDLOG_DEBUG("MSC::prepare_out CBW→nullptr");
            return nullptr; // CBW goes to fallback_data

        case BotState::DataOut:
            if (write_mmap_base_) {
                // Zero-copy WRITE: socket uses splice to write directly to file; mmap pointer only used for fallback
                trx->direct_io = true;
                trx->file_lba = write_lba_;
                trx->file_offset = write_accumulated_;
                SPDLOG_DEBUG("MSC::prepare_out WRITE mmap lba={} offset={}", write_lba_, write_accumulated_);
                return static_cast<char *>(write_mmap_base_) + write_accumulated_;
            }
            // Non-mmap WRITE / UNMAP: socket reads directly to the end of staging
            {
                auto old_size = staging_data_.size();
                staging_data_.resize(old_size + length);
                SPDLOG_DEBUG("MSC::prepare_out WRITE staging old={} new={}", old_size, old_size + length);
                return staging_data_.data() + old_size;
            }

        default:
            SPDLOG_WARN("MSC::prepare_out unexpected state={}", static_cast<int>(state_));
            return nullptr;
    }
}

/** BOT protocol state machine: CBW parse → DataIn/DataOut → CSW.
 *  OUT data enters this function via prepare_out_buffer → recv_transfer_data → on_out_data_received.
 *  Clearing staging_data_ is deferred to the next CBW (Idle branch) to prevent the previous IN transfer's
 *  sender thread from still reading it. */
void MscBulkOnlyHandler::on_out_data_received(StorageIoTransfer *trx, std::size_t length) {
    SPDLOG_DEBUG("MSC::on_out_data_recv len={} state={}", length, static_cast<int>(state_));
    switch (state_) {
        case BotState::Idle: {
            // Previous command's sender thread has finished sending (otherwise host would not send a new CBW); safe to clear old staging
            staging_data_.clear();
            read_mmap_base_ = nullptr;
            read_total_size_ = 0;
            write_mmap_base_ = nullptr;
            write_accumulated_ = 0;

            // CBW is in fallback_data
            if (trx->fallback_data.size() < sizeof(CBW)) {
                SPDLOG_ERROR("CBW too short: {} bytes", trx->fallback_data.size());
                command_failed_ = true;
                state_ = BotState::Status;
                return;
            }
            std::memcpy(&current_cbw_, trx->fallback_data.data(), sizeof(CBW));

            if (current_cbw_.dCBWSignature != CBW_SIGNATURE) {
                SPDLOG_ERROR("Invalid CBW signature: 0x{:08X}", current_cbw_.dCBWSignature);
                command_failed_ = true;
                state_ = BotState::Status;
                return;
            }

            std::uint8_t cmd = current_cbw_.CBWCB[0];
            bool is_data_in = (current_cbw_.bmCBWFlags & 0x80) != 0;
            auto transfer_len = current_cbw_.dCBWDataTransferLength;

            SPDLOG_DEBUG("CBW cmd=0x{:02X} dir={} len={}", cmd, is_data_in ? "IN" : "OUT", transfer_len);

            switch (cmd) {
                case 0x00:
                    command_failed_ = !(backend_ != nullptr);
                    state_ = BotState::Status;
                    break;

                case 0x03: {
                    // REQUEST SENSE
                    std::uint8_t sense[18] = {};
                    sense[0] = 0x70;
                    sense[7] = 10;
                    auto len = std::min(transfer_len, std::uint32_t(18));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(sense, sense + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x12: {
                    // INQUIRY (standard or VPD)
                    bool evpd = (current_cbw_.CBWCB[1] & 0x01) != 0;
                    std::uint8_t page = current_cbw_.CBWCB[2];
                    SPDLOG_DEBUG("INQUIRY evpd={} page=0x{:02X} len={}", evpd, page, transfer_len);
                    if (!evpd) {
                        // Standard INQUIRY: vendor(8) + product(16) + revision(4) from config_
                        auto pad = [](const std::string &s, std::size_t n) {
                            std::string r = s;
                            r.resize(n, ' '); // Pad with spaces if short; truncate if long
                            return r;
                        };
                        std::uint8_t inquiry[36] = {};
                        inquiry[0] = 0x00;
                        inquiry[1] = 0x80;
                        inquiry[2] = 0x07;
                        inquiry[3] = 0x12; // HiSup=1, Response Format=2 (SPC-4)
                        inquiry[4] = 31;
                        inquiry[6] |= 0x02; // CmdQue=1
                        std::memcpy(inquiry + 8, pad(config_.vendor, 8).c_str(), 8);
                        std::memcpy(inquiry + 16, pad(config_.product, 16).c_str(), 16);
                        std::memcpy(inquiry + 32, pad(config_.revision, 4).c_str(), 4);
                        auto len = std::min(transfer_len, std::uint32_t(36));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(inquiry, inquiry + len);
                    }
                    else if (page == 0x00) {
                        // Supported VPD Pages：0x00 0x80 0xB0 0xB2
                        std::vector<std::uint8_t> vpd{0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0xB0, 0xB2};
                        auto len = std::min(transfer_len, std::uint32_t(vpd.size()));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(vpd.begin(), vpd.begin() + len);
                    }
                    else if (page == 0x80) {
                        // Unit Serial Number, from config_.serial
                        const auto &sn = config_.serial;
                        std::vector<std::uint8_t> vpd{0x00, 0x80, 0x00, static_cast<std::uint8_t>(sn.size())};
                        vpd.insert(vpd.end(), sn.begin(), sn.end());
                        auto len = std::min(transfer_len, std::uint32_t(vpd.size()));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(vpd.begin(), vpd.begin() + len);
                    }
                    else if (page == 0xB0) {
                        // Block Limits VPD: advertise maximum UNMAP LBA count and granularity
                        std::vector<std::uint8_t> vpd(64, 0);
                        vpd[0] = 0x00;
                        vpd[1] = 0xB0;
                        vpd[2] = 0x00;
                        vpd[3] = 0x3C;
                        // max_unmap_lba_count (bytes 20-23, big-endian): 65536 LBAs = 32 MiB
                        vpd[21] = 0x01;
                        // max_unmap_block_desc_count (bytes 24-27, big-endian): 64
                        vpd[27] = 64;
                        // optimal_unmap_granularity (bytes 28-31, big-endian): 8 LBAs = 4096 B
                        vpd[31] = 8;
                        // unmap_granularity_alignment (bytes 32-35, big-endian), bit31=UGAVALID
                        vpd[32] = 0x80;
                        vpd[35] = 8;
                        auto len = std::min(transfer_len, std::uint32_t(vpd.size()));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(vpd.begin(), vpd.begin() + len);
                    }
                    else if (page == 0xB2) {
                        // Logical Block Provisioning: advertise UNMAP support
                        std::vector<std::uint8_t> vpd{0x00, 0xB2, 0x00, 0x04, 0x00, 0x80, 0x02, 0x00};
                        auto len = std::min(transfer_len, std::uint32_t(vpd.size()));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(vpd.begin(), vpd.begin() + len);
                    }
                    else {
                        // Unsupported VPD page — return empty
                        staging_offset_ = 0;
                        staging_data_.clear();
                    }
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x1A: {
                    // MODE SENSE (6)
                    std::uint8_t mode[4] = {};
                    mode[0] = 3;
                    if (read_only_)
                        mode[2] = 0x80;
                    auto len = std::min(transfer_len, std::uint32_t(4));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(mode, mode + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x1E:
                    state_ = BotState::Status;
                    break;

                case 0x23: {
                    // READ FORMAT CAPACITIES (sent by Windows clients)
                    auto blocks = backend_ ? backend_->block_count() : 0;
                    std::uint32_t bs = backend_ ? backend_->block_size() : 512;
                    std::uint8_t buf[12] = {};
                    buf[3] = 8; // One 8-byte descriptor
                    buf[4] = (blocks >> 24) & 0xFF;
                    buf[5] = (blocks >> 16) & 0xFF;
                    buf[6] = (blocks >> 8) & 0xFF;
                    buf[7] = blocks & 0xFF;
                    buf[8] = 0x02; // formatted media
                    buf[9] = (bs >> 16) & 0xFF;
                    buf[10] = (bs >> 8) & 0xFF;
                    buf[11] = bs & 0xFF;
                    auto len = std::min(transfer_len, std::uint32_t(12));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(buf, buf + len);
                    state_ = BotState::DataIn;
                    break;
                }

                case 0x9E: {
                    // READ CAPACITY (16)
                    SPDLOG_DEBUG("READ CAPACITY (16)");
                    auto last_lba = backend_ ? backend_->block_count() - 1 : 0;
                    std::uint32_t bs = backend_->block_size();
                    std::uint8_t buf[12] = {};
                    buf[0] = (last_lba >> 56) & 0xFF;
                    buf[1] = (last_lba >> 48) & 0xFF;
                    buf[2] = (last_lba >> 40) & 0xFF;
                    buf[3] = (last_lba >> 32) & 0xFF;
                    buf[4] = (last_lba >> 24) & 0xFF;
                    buf[5] = (last_lba >> 16) & 0xFF;
                    buf[6] = (last_lba >> 8) & 0xFF;
                    buf[7] = last_lba & 0xFF;
                    buf[8] = (bs >> 24) & 0xFF;
                    buf[9] = (bs >> 16) & 0xFF;
                    buf[10] = (bs >> 8) & 0xFF;
                    buf[11] = bs & 0xFF;
                    auto len = std::min(transfer_len, std::uint32_t(12));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(buf, buf + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x25: {
                    // READ CAPACITY (10)
                    auto last_lba = backend_ ? backend_->block_count() - 1 : 0;
                    std::uint32_t bs = backend_->block_size();
                    std::uint8_t buf[8] = {};
                    buf[0] = (last_lba >> 24) & 0xFF;
                    buf[1] = (last_lba >> 16) & 0xFF;
                    buf[2] = (last_lba >> 8) & 0xFF;
                    buf[3] = last_lba & 0xFF;
                    buf[4] = (bs >> 24) & 0xFF;
                    buf[5] = (bs >> 16) & 0xFF;
                    buf[6] = (bs >> 8) & 0xFF;
                    buf[7] = bs & 0xFF;
                    auto len = std::min(transfer_len, std::uint32_t(8));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(buf, buf + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x28: // READ (10)
                case 0x2A: {
                    // WRITE (10)
                    auto lba = (std::uint64_t(current_cbw_.CBWCB[2]) << 24) |
                               (std::uint64_t(current_cbw_.CBWCB[3]) << 16) |
                               (std::uint64_t(current_cbw_.CBWCB[4]) << 8) | (std::uint64_t(current_cbw_.CBWCB[5]));
                    auto count = (std::uint16_t(current_cbw_.CBWCB[7]) << 8) | (std::uint16_t(current_cbw_.CBWCB[8]));
                    if (count == 0)
                        count = 256;

                    if (lba + count > (backend_ ? backend_->block_count() : 0)) {
                        SPDLOG_WARN("SCSI cmd 0x{:02X} LBA={} count={} out of range", cmd, lba, count);
                        command_failed_ = true;
                        state_ = BotState::Status;
                        break;
                    }

                    if (cmd == 0x28) {
                        // READ: prefer mmap direct send (sendfile path); fall back to staging otherwise
                        staging_offset_ = 0;
                        read_lba_ = lba;
                        read_mmap_base_ = backend_->get_direct_buffer(lba);
                        if (read_mmap_base_) {
                            read_total_size_ = static_cast<std::size_t>(count) * backend_->block_size();
                            staging_data_.clear();
                        }
                        else {
                            staging_data_.resize(read_total_size_ =
                                                         static_cast<std::size_t>(count) * backend_->block_size());
                            backend_->read(lba, count, staging_data_.data());
                        }
                        state_ = BotState::DataIn;
                    }
                    else if (read_only_) {
                        command_failed_ = true;
                        state_ = BotState::Status;
                    }
                    else {
                        // WRITE: prefer mmap direct write (socket reads directly into mmap); fall back to staging otherwise
                        write_lba_ = lba;
                        write_count_ = count;
                        write_accumulated_ = 0;
                        write_mmap_base_ = backend_->get_direct_buffer(lba);
                        if (!write_mmap_base_) {
                            staging_data_.clear();
                            staging_data_.reserve(static_cast<std::size_t>(count) * backend_->block_size());
                        }
                        state_ = BotState::DataOut;
                    }
                    break;
                }
                case 0x1B:
                case 0x2F:
                    state_ = BotState::Status;
                    break;
                case 0x85:
                    command_failed_ = true;
                    state_ = BotState::Status;
                    break;
                case 0x42: {
                    // UNMAP: use CBW.dCBWDataTransferLength as data length (some kernel CDB parameter lengths are 0)
                    auto data_len = current_cbw_.dCBWDataTransferLength;
                    SPDLOG_DEBUG("UNMAP CBW tag=0x{:08X} dataLen={}", current_cbw_.dCBWTag, data_len);
                    if (read_only_) {
                        command_failed_ = true;
                        state_ = BotState::Status;
                        break;
                    }
                    if (data_len == 0) {
                        state_ = BotState::Status;
                        break;
                    }
                    write_count_ = data_len;
                    data_out_unmap_ = true;
                    staging_offset_ = 0;
                    staging_data_.clear();
                    state_ = BotState::DataOut;
                    break;
                }
                default:
                    SPDLOG_WARN("Unsupported SCSI command: 0x{:02X}", cmd);
                    command_failed_ = true;
                    state_ = BotState::Status;
                    break;
            }
            break;
        }

        case BotState::DataOut: {
            if (data_out_unmap_) {
                if (staging_data_.size() >= write_count_) {
                    auto &d = staging_data_;
                    for (std::size_t i = 8; i + 16 <= d.size(); i += 16) {
                        auto lba = (std::uint64_t(d[i]) << 56) | (std::uint64_t(d[i + 1]) << 48) |
                                   (std::uint64_t(d[i + 2]) << 40) | (std::uint64_t(d[i + 3]) << 32) |
                                   (std::uint64_t(d[i + 4]) << 24) | (std::uint64_t(d[i + 5]) << 16) |
                                   (std::uint64_t(d[i + 6]) << 8) | (std::uint64_t(d[i + 7]));
                        auto cnt = (std::uint32_t(d[i + 8]) << 24) | (std::uint32_t(d[i + 9]) << 16) |
                                   (std::uint32_t(d[i + 10]) << 8) | (std::uint32_t(d[i + 11]));
                        SPDLOG_DEBUG("UNMAP punch lba={} cnt={}", lba, cnt);
                        backend_->punch_hole(lba, cnt);
                    }
                    staging_data_.clear();
                    data_out_unmap_ = false;
                    data_residue_ = 0;
                    state_ = BotState::Status;
                }
            }
            else if (write_mmap_base_) {
                // Zero-copy WRITE: data already read directly into mmap; accumulate offset
                write_accumulated_ += length;
                if (write_accumulated_ >= static_cast<std::size_t>(write_count_) * backend_->block_size()) {
                    write_mmap_base_ = nullptr;
                    write_accumulated_ = 0;
                    data_residue_ = 0;
                    state_ = BotState::Status;
                }
            }
            else {
                // Non-mmap WRITE fallback: accumulate staging then write to disk
                if (write_lba_ + write_count_ <= backend_->block_count()) {
                    if (staging_data_.size() >= static_cast<std::size_t>(write_count_) * backend_->block_size()) {
                        backend_->write(write_lba_, write_count_, staging_data_.data());
                        staging_data_.clear();
                        data_residue_ = 0;
                        state_ = BotState::Status;
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

void MscBulkOnlyHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                              std::error_code &ec) {
    SPDLOG_DEBUG("BULK {} ep={:02x} len={} state={}", ep.is_in() ? "IN" : "OUT", ep.address, transfer_buffer_length,
                 static_cast<int>(state_));

    if (ep.is_in()) {
        switch (state_) {
            case BotState::DataIn: {
                auto total = read_mmap_base_ ? read_total_size_ : staging_data_.size();
                auto remaining = total - staging_offset_;
                auto len = std::min(static_cast<std::size_t>(transfer_buffer_length), remaining);
                if (len > 0) {
                    auto *trx = StorageIoTransfer::from_handle(transfer.get());
                    if (read_mmap_base_) {
                        // Zero-copy send: external_buf points directly to mmap; file_lba/file_offset for send_direct
                        trx->direct_io = true;
                        trx->external_buf = static_cast<char *>(read_mmap_base_) + staging_offset_;
                        trx->file_lba = read_lba_;
                        trx->file_offset = staging_offset_;
                        SPDLOG_DEBUG("MSC::hb IN mmap handle={:p} lba={} offset={} len={}",
                                     static_cast<const void *>(transfer.get()), read_lba_, staging_offset_, len);
                    }
                    else {
                        trx->external_buf = staging_data_.data() + staging_offset_;
                        SPDLOG_DEBUG("MSC::hb IN staging handle={:p} offset={} len={}",
                                     static_cast<const void *>(transfer.get()), staging_offset_, len);
                    }
                    trx->actual_length = len;
                    staging_offset_ += len;
                    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                            static_cast<std::uint32_t>(len), std::move(transfer)));
                }
                else {
                    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0));
                }
                // All sent → Status; do not clear staging/mmap (sender is still queuing sends)
                if (staging_offset_ >= total) {
                    staging_offset_ = 0;
                    data_residue_ = 0;
                    state_ = BotState::Status;
                }
                break;
            }

            case BotState::Status: {
                CSW csw{};
                csw.dCSWSignature = CSW_SIGNATURE;
                csw.dCSWTag = current_cbw_.dCBWTag;
                csw.dCSWDataResidue = data_residue_;
                if (command_failed_) {
                    csw.bCSWStatus = 1;
                    command_failed_ = false;
                }

                auto *trx = StorageIoTransfer::from_handle(transfer.get());
                SPDLOG_DEBUG("MSC::hb Status handle={:p} CSW_tag=0x{:08X} status={}",
                             static_cast<const void *>(transfer.get()), csw.dCSWTag, csw.bCSWStatus);
                trx->fallback_data.resize(sizeof(CSW));
                std::memcpy(trx->fallback_data.data(), &csw, sizeof(CSW));
                trx->external_buf = trx->fallback_data.data();
                trx->actual_length = sizeof(CSW);
                session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                        seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), sizeof(CSW), std::move(transfer)));
                // After CSW is enqueued, switch back to Idle; old staging deferred to be cleared at next CBW's Idle branch,
                // ensuring the sender thread has enough time to finish consuming the external pointer (avoid use-after-free)
                state_ = BotState::Idle;
                break;
            }

            default:
                send_stall(seqnum);
                break;
        }
    }
    else {
        SPDLOG_DEBUG("MSC::hb OUT drop handle={:p}", static_cast<const void *>(transfer.get()));
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
    }
}

void MscBulkOnlyHandler::send_stall(std::uint32_t seqnum) {
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}
