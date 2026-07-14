// LiteWallPaper bitstream format (.lwp)
//
// A .lwp file contains a fixed header, an access-unit (AU) index table, and
// a raw H.264 Annex B byte stream. The maker produces the stream with NO
// B-frames (decode order == display order) and pre-splits it into AUs so the
// player can play frame-by-frame without any NAL parsing of its own.
//
// Layout (all integers little-endian):
//
//   offset  size  field
//   0       4     magic            = 'L' 'W' 'P' '1'
//   4       2     header_size      = sizeof(LwpHeader) (currently 64)
//   6       2     width            (coded, pixels)
//   8       2     height           (coded, pixels)
//   10      2     fps_num          (frame-rate numerator, e.g. 30)
//   12      2     fps_den          (frame-rate denominator, e.g. 1)
//   14      4     frame_count      (total pictures = AU count)
//   18      4     sps_offset       (byte offset of SPS NAL inside payload)
//   22      2     sps_size
//   24      4     pps_offset       (byte offset of PPS NAL inside payload)
//   28      2     pps_size
//   30      4     payload_size     (bytes of H.264 Annex B data)
//   34      2     profile_idc      (copied from SPS for convenience)
//   36      1     level_idc
//   37      1     bit_depth_luma
//   38      1     bit_depth_chroma
//   39      1     reserved1
//   40      4     au_table_offset  (byte offset of AU index table, file-absolute)
//   44      2     au_entry_size    (bytes per AU entry; currently 8)
//   46      2     reserved2
//   48      16    sha128_of_payload (optional, 0 = not computed)
//
// AU index table (at au_table_offset, frame_count entries):
//   Each entry = AuEntry { uint32_t offset; uint32_t size; }
//   offset = byte offset of the AU's first start code, relative to payload start
//   size   = byte length of the AU (from first start code to next AU's first
//            start code, or end of payload for the last AU)
//
// Payload (raw H.264 Annex B) starts right after the AU index table.
#pragma once
#include <cstdint>

namespace lwp {

constexpr uint32_t kMagic = 0x3150574Cu; // 'L','W','P','1' little-endian
constexpr uint16_t kHeaderSize = 64;

// One entry in the AU index table. offset/size are relative to payload start.
#pragma pack(push, 1)
struct AuEntry {
    uint32_t offset;
    uint32_t size;
};
static_assert(sizeof(AuEntry) == 8, "AuEntry layout");
#pragma pack(pop)

#pragma pack(push, 1)
struct LwpHeader {
    uint32_t magic;           // 0
    uint16_t header_size;     // 4
    uint16_t width;           // 6
    uint16_t height;          // 8
    uint16_t fps_num;         // 10
    uint16_t fps_den;         // 12
    uint32_t frame_count;     // 14
    uint32_t sps_offset;      // 18  (relative to payload start)
    uint16_t sps_size;        // 22
    uint32_t pps_offset;      // 24
    uint16_t pps_size;        // 28
    uint32_t payload_size;    // 30
    uint16_t profile_idc;     // 34
    uint8_t  level_idc;       // 36
    uint8_t  bit_depth_luma;  // 37
    uint8_t  bit_depth_chroma;// 38
    uint8_t  reserved1;       // 39
    uint32_t au_table_offset; // 40  (file-absolute offset of AU index table)
    uint16_t au_entry_size;   // 44  (bytes per AU entry; = sizeof(AuEntry))
    uint16_t reserved2;       // 46
    uint8_t  sha128[16];      // 48
};
static_assert(sizeof(LwpHeader) == kHeaderSize, "LwpHeader layout");
#pragma pack(pop)

} // namespace lwp
