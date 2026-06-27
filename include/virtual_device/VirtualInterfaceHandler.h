#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "DeviceHandler/TransferOperator.h"
#include "InterfaceHandler/InterfaceHandler.h"
#include "protocol.h"

namespace usbipdcpp {

class VirtualDeviceHandler;

/**
 * @brief Endpoint request queue, manages transfer requests by endpoint address (pure data container, no locking)
 *
 * Used to manage pending IN transfer requests for each endpoint.
 * Note: All methods are not locked; callers must manage the mutex themselves.
 */
class EndpointRequestQueue {
public:
    struct Request {
        std::uint32_t seqnum;
        std::uint32_t length;
        TransferHandle transfer;
    };

    /**
     * @brief Enqueue a request to the specified endpoint
     * @note Caller must already hold the mutex
     */
    void enqueue(std::uint8_t ep_address, Request request) {
        queues_[ep_address].push_back(std::move(request));
    }

    /**
     * @brief Dequeue a request from the specified endpoint
     * @note Caller must already hold the mutex
     */
    std::optional<Request> dequeue(std::uint8_t ep_address) {
        auto it = queues_.find(ep_address);
        if (it == queues_.end() || it->second.empty()) {
            return std::nullopt;
        }
        auto req = std::move(it->second.front());
        it->second.pop_front();
        return req;
    }

    /**
     * @brief Dequeue a request from any endpoint that has one (returns endpoint address and request)
     * @return pair<endpoint address, request>; returns nullopt if all queues are empty
     * @note Caller must already hold the mutex
     */
    std::optional<std::pair<std::uint8_t, Request>> dequeue_any() {
        for (auto &[ep, queue]: queues_) {
            if (!queue.empty()) {
                auto req = std::move(queue.front());
                queue.pop_front();
                return std::make_pair(ep, std::move(req));
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Peek at the first request in the specified endpoint queue (without dequeuing)
     * @note Caller must already hold the mutex
     */
    Request *peek(std::uint8_t ep_address) {
        auto it = queues_.find(ep_address);
        if (it == queues_.end() || it->second.empty()) {
            return nullptr;
        }
        return &it->second.front();
    }

    /**
     * @brief Check whether the specified endpoint queue is empty
     * @note Caller must already hold the mutex
     */
    bool empty(std::uint8_t ep_address) const {
        auto it = queues_.find(ep_address);
        return it == queues_.end() || it->second.empty();
    }

    /**
     * @brief Cancel a request by seqnum (for UNLINK)
     * @return true if the request was found and removed
     * @note Caller must already hold the mutex
     */
    bool cancel_by_seqnum(std::uint32_t unlink_seqnum) {
        for (auto &[ep, queue]: queues_) {
            auto it = std::find_if(queue.begin(), queue.end(),
                                   [unlink_seqnum](const Request &r) { return r.seqnum == unlink_seqnum; });
            if (it != queue.end()) {
                queue.erase(it);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Clear all queues
     * @note Caller must already hold the mutex
     */
    void clear() {
        queues_.clear();
    }

private:
    std::unordered_map<std::uint8_t, std::deque<Request>> queues_;
};

class USBIPDCPP_API VirtualInterfaceHandler : public AbstInterfaceHandler {
public:
    explicit VirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                     std::unique_ptr<TransferOperator> op = nullptr) :
        AbstInterfaceHandler(handle_interface), string_pool(string_pool),
        transfer_op_(op ? std::move(op) : std::make_unique<GenericTransferOperator>()) {

        string_interface = string_pool.new_string(L"Usbipdcpp Virtual Interface");
    }

    // ========== Connection lifecycle API ==========

    /**
     * @brief Set the owning DeviceHandler
     * @param handler DeviceHandler pointer
     */
    void set_device_handler(VirtualDeviceHandler *handler) {
        device_handler = handler;
    }

    /** Callback at the end of setup_interface_handlers; device_handler is set at this point, subclasses can initialize here */
    virtual void on_setup_interface_handlers() {
    }

    /**
     * @brief Called when a new client connects
     * @param current_session
     * @param ec Error code that occurred
     * @note Subclasses overriding this must call the parent implementation at the start; the parent sets the session pointer
     */
    void on_new_connection(Session &current_session, error_code &ec) override {
        session = &current_session;
    }

    /**
     * @brief Called when a transfer must be completely terminated due to errors, client detach, server shutdown, etc. No messages may be submitted after this call.
     * @note Subclasses overriding this must call the parent implementation at the end; the parent clears the session pointer
     */
    void on_disconnection(error_code &ec) override {
        session = nullptr;
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    virtual void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                      std::uint32_t transfer_buffer_length, TransferHandle transfer, error_code &ec);
    virtual void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                           std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                           error_code &ec);
    virtual void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                             std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                             int num_iso_packets, error_code &ec);

    virtual void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                              std::uint32_t transfer_flags,
                                                              std::uint32_t transfer_buffer_length,
                                                              const SetupPacket &setup, TransferHandle transfer,
                                                              std::error_code &ec);
    virtual void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                          std::uint32_t transfer_flags,
                                                                          std::uint32_t transfer_buffer_length,
                                                                          const SetupPacket &setup,
                                                                          TransferHandle transfer, std::error_code &ec);

    // ========== Virtual functions that subclasses must implement ==========

    virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) = 0;
    virtual void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                std::uint32_t *p_status) = 0;

    virtual std::uint8_t request_get_interface(std::uint32_t *p_status) = 0;
    virtual void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) = 0;

    virtual std::uint16_t request_get_status(std::uint32_t *p_status) = 0;
    virtual std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) = 0;

    /**
     * @brief this function is not necessary for all device,
     * HID device is required to implement this function
     * @param type
     * @param language_id
     * @param descriptor_length
     * @param p_status
     * @return
     */
    virtual data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                             std::uint16_t descriptor_length, std::uint32_t *p_status);

    virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) = 0;
    virtual void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                              std::uint32_t *p_status) = 0;

    /**
     * @brief Only use for isochronous transfer, so give a default empty implement.
     * @param ep_address
     * @param p_status
     * @return
     */
    virtual std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address, std::uint32_t *p_status) {
        return 0;
    }


    [[nodiscard]] virtual data_type get_class_specific_descriptor() = 0;

    // ========== TransferOperator ==========

    TransferOperator *get_transfer_operator() {
        return transfer_op_.get();
    }

    void set_transfer_operator(std::unique_ptr<TransferOperator> op) {
        transfer_op_ = std::move(op);
    }

    // ========== Utility functions ==========

    [[nodiscard]] virtual std::uint8_t get_string_interface_value() const {
        return string_interface;
    }

    [[nodiscard]] virtual std::wstring get_string_interface() const {
        return string_pool.get_string(string_interface).value_or(L"");
    }

    void change_string_interface(const std::wstring &new_str) {
        string_pool.change_string(string_interface, new_str);
    }

    /// Synchronize iInterface with another handler (after USBCCGP removes IAD, interfaces with the same function are grouped by iInterface)
    void sync_string_interface_from(const VirtualInterfaceHandler &other) {
        string_interface = other.string_interface;
    }

protected:
    Session *session = nullptr;
    VirtualDeviceHandler *device_handler = nullptr;
    std::unique_ptr<TransferOperator> transfer_op_;

    std::uint8_t string_interface;

    StringPool &string_pool;

    /**
     * @brief Mutex protecting endpoint_requests_
     */
    mutable std::mutex endpoint_requests_mutex_;

    /**
     * @brief General-purpose endpoint request queue for managing IN transfer requests
     *
     * Subclasses can use this queue directly without implementing their own.
     * Must hold endpoint_requests_mutex_ when operating.
     */
    EndpointRequestQueue endpoint_requests_;
};

} // namespace usbipdcpp
