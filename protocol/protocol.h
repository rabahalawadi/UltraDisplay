#pragma once
#include <stdint.h>

#pragma pack(push, 1)

// UltraDisplay Protocol Definitions
#define ULTRADISPLAY_MAGIC 0x55444953 // "UDIS"
#define ULTRADISPLAY_VERSION 1
#define ULTRADISPLAY_DEFAULT_PORT 28443

// Packet Types
enum UDisPacketType : uint8_t {
    PKT_HANDSHAKE_REQ   = 0x01, // Client -> Server (Screen specs, capabilities)
    PKT_HANDSHAKE_RESP  = 0x02, // Server -> Client (Encoder config, stream info)
    PKT_FRAME_DATA      = 0x03, // Server -> Client (Encoded H.264/HEVC NAL units)
    PKT_CURSOR_SHAPE    = 0x04, // Server -> Client (Cursor icon, hotspot, alpha)
    PKT_CURSOR_POS      = 0x05, // Server -> Client (Cursor visibility & coordinates)
    PKT_INPUT_SPEN      = 0x10, // Client -> Server (S-Pen pressure, tilt, hover, button)
    PKT_INPUT_TOUCH     = 0x11, // Client -> Server (Multitouch contacts)
    PKT_INPUT_KEYBOARD  = 0x12, // Client -> Server (Key events)
    PKT_HEARTBEAT       = 0xFE, // Bidirectional keep-alive ping/pong
};

// Video Codec Types
enum UDisCodecType : uint8_t {
    CODEC_H264 = 0x01,
    CODEC_HEVC = 0x02, // H.265
    CODEC_AV1  = 0x03,
};

// S-Pen Input Flags
enum UDisSPenFlags : uint16_t {
    SPEN_FLAG_NONE       = 0x0000,
    SPEN_FLAG_INRANGE    = 0x0001, // Hovering within ~1.5cm of screen
    SPEN_FLAG_INCONTACT  = 0x0002, // Pen tip touching glass
    SPEN_FLAG_DOWN       = 0x0004, // Initial touch contact
    SPEN_FLAG_MOVE       = 0x0008, // Drag / hover move
    SPEN_FLAG_UP         = 0x0010, // Lifted off screen
    SPEN_FLAG_BARREL     = 0x0020, // Physical S-Pen side button pressed
    SPEN_FLAG_ERASER     = 0x0040, // Eraser end or eraser mode active
};

// Common Packet Header (8 bytes)
struct UDisHeader {
    uint32_t magic;       // ULTRADISPLAY_MAGIC (0x55444953)
    uint8_t  type;        // UDisPacketType
    uint8_t  version;     // ULTRADISPLAY_VERSION
    uint16_t payloadLen;  // Length of subsequent payload bytes
};

// Handshake Request (Client -> Server)
struct UDisHandshakeReq {
    uint16_t clientWidth;     // e.g. 2560
    uint16_t clientHeight;    // e.g. 1600
    uint16_t densityDpi;      // e.g. 280
    uint8_t  refreshRate;     // e.g. 60 or 120 Hz
    uint8_t  supportedCodecs; // Bitmask: 1=H264, 2=HEVC, 4=AV1
    uint8_t  hasStylus;       // 1 if S-Pen / Active Stylus supported
    uint8_t  maxTouchPoints;  // e.g. 10
    char     deviceName[64];  // UTF-8 device model, e.g. "Samsung Galaxy Tab S9"
};

// Handshake Response (Server -> Client)
struct UDisHandshakeResp {
    uint8_t  status;          // 0 = Success, 1 = Error
    uint8_t  selectedCodec;   // UDisCodecType (e.g. CODEC_H264)
    uint16_t streamWidth;     // Encoded video width
    uint16_t streamHeight;    // Encoded video height
    uint8_t  targetFps;       // Target frame rate (60 or 120)
    uint32_t targetBitrate;   // Bitrate in bps (e.g. 25_000_000 for 25 Mbps)
};

// Frame Data Header (Server -> Client)
struct UDisFrameHeader {
    uint32_t frameIndex;      // Monotonically increasing frame sequence
    uint64_t timestampUs;     // Timestamp in microseconds
    uint8_t  isKeyframe;      // 1 if IDR/Keyframe, 0 otherwise
    uint8_t  reserved[3];     // Padding
    uint32_t nalSize;         // Byte length of NAL payload following header
};

// S-Pen / Stylus Input Event (Client -> Server)
struct UDisSPenEvent {
    uint16_t flags;           // UDisSPenFlags (INRANGE, INCONTACT, BARREL, etc.)
    float    normX;           // 0.0f .. 1.0f (X coordinate normalized to display)
    float    normY;           // 0.0f .. 1.0f (Y coordinate normalized to display)
    uint16_t pressure;        // 0 .. 4095 (High-precision S-Pen pressure)
    int8_t   tiltX;           // -90 .. +90 degrees tilt
    int8_t   tiltY;           // -90 .. +90 degrees tilt
    int16_t  rotation;        // 0 .. 359 degrees azimuth orientation
    uint32_t timestampMs;     // Client event timestamp
};

// Multitouch Contact (Client -> Server)
struct UDisTouchContact {
    uint8_t  contactId;       // Pointer index (0..9)
    uint8_t  state;           // 0=DOWN, 1=MOVE, 2=UP
    float    normX;           // 0.0f .. 1.0f
    float    normY;           // 0.0f .. 1.0f
    float    size;            // Contact area
};

struct UDisTouchEvent {
    uint8_t  count;           // Number of active contacts in this report
    UDisTouchContact contacts[10];
    uint32_t timestampMs;
};

#pragma pack(pop)
