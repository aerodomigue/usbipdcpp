#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace usbipdcpp {

/// Format information supported by the video source
struct VideoFormatInfo {
    std::uint32_t fourcc; // FOURCC pixel format (UvcFourCC)
    std::uint16_t width; // Frame width
    std::uint16_t height; // Frame height
    std::uint32_t max_frame_size; // Maximum frame size in bytes
    std::uint32_t default_frame_interval; // Default frame interval (100 ns units)
    /// Shortest supported frame interval (100 ns units), i.e. maximum frame rate.
    /// Shared by the Frame descriptor dwMinFrameInterval + PROBE GET_MAX.
    std::uint32_t min_frame_interval;
    /// Longest supported frame interval (100 ns units), i.e. minimum frame rate.
    /// Shared by the Frame descriptor dwMaxFrameInterval + PROBE GET_MIN.
    /// Constraint: (max - min) % min == 0 (divisibility check by usbvideo.sys).
    std::uint32_t max_frame_interval;
    std::uint8_t bits_per_pixel; // Bits per pixel
};

/// Video frame
struct VideoFrame {
    const std::uint8_t *data; // Frame data pointer (lifetime managed by VideoSource)
    std::size_t size; // Frame data size in bytes
    bool is_keyframe; // Whether this is a keyframe
};

/// Abstract interface for a video frame source
/// Implementing classes are responsible for generating/reading video frames; UvcHandler is responsible for packing them into UVC protocol for sending
class VideoSource {
public:
    virtual ~VideoSource() = default;

    /// Return a list of all formats supported by the source
    virtual std::vector<VideoFormatInfo> supported_formats() const = 0;

    /// Currently negotiated format
    virtual VideoFormatInfo current_format() const = 0;

    /// Switch formats. Only switches the descriptor index; UvcHandler will call this after COMMIT
    virtual bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                            std::uint32_t frame_interval) = 0;

    /// Get the next frame. The data pointer is managed by the source and is valid until the next get_frame call
    virtual bool get_frame(VideoFrame &frame) = 0;

    /// Maximum frame size in the current format (used for allocating ISO transfer buffers)
    virtual std::size_t max_frame_size() const = 0;

    /// Frame interval in the current format (100 ns units), used for ISO transfer scheduling
    virtual std::uint32_t frame_interval() const = 0;
};

} // namespace usbipdcpp
