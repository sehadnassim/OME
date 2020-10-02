#pragma once

#include <stdint.h>
#include <cstdint>
#include <vector>
#include "base/ovlibrary/ovlibrary.h"
#include "rtp_packet.h"
#include "rtp_rtcp_interface.h"
#include "base/common_types.h"

enum class RtpVideoCodecType
{
	None,
	Vp8,
	H264
	// In the future
	// kRtpVideoVp9
};

enum class RtpAudioCodecType
{
	None,
	Opus
};

const int16_t kNoPictureId = -1;
const int16_t kNoTl0PicIdx = -1;
const uint8_t kNoTemporalIdx = 0xFF;
const int kNoKeyIdx = -1;

const size_t kMaxNalusPerPacket = 10;

struct NaluInfo {
	uint8_t type;
	int sps_id;
	int pps_id;
};

enum NaluType : uint8_t {
	kSlice = 1,
	kIdr = 5,
	kSei = 6,
	kSps = 7,
	kPps = 8,
	kAud = 9,
	kEndOfSequence = 10,
	kEndOfStream = 11,
	kFiller = 12,
	kStapA = 24,
	kFuA = 28
};

enum H264PacketizationTypes {
	kH264SingleNalu,
	kH264StapA,
	kH264FuA,
};

struct RTPVideoHeaderH264 {
	uint8_t nalu_type;
	H264PacketizationTypes packetization_type;
	NaluInfo nalus[kMaxNalusPerPacket];
	size_t nalus_length;
	H264PacketizationMode packetization_mode;
};

struct RTPVideoHeaderVP8
{
	void InitRTPVideoHeaderVP8()
	{
		non_reference = false;
		picture_id = kNoPictureId;
		tl0_pic_idx = kNoTl0PicIdx;
		temporal_idx = kNoTemporalIdx;
		layer_sync = false;
		key_idx = kNoKeyIdx;
		partition_id = 0;
		beginning_of_partition = false;
	}

	bool non_reference;            // Frame is discardable.
	int16_t picture_id;            // Picture ID index, 15 bits;
	// kNoPictureId if PictureID does not exist.
	int16_t tl0_pic_idx;                // TL0PIC_IDX, 8 bits;
	// kNoTl0PicIdx means no value provided.
	uint8_t temporal_idx;            // Temporal layer index, or kNoTemporalIdx.
	bool layer_sync;                // This frame is a layer sync frame.
	// Disabled if temporalIdx == kNoTemporalIdx.
	int key_idx;                    // 5 bits; kNoKeyIdx means not used.
	int partition_id;            // VP8 partition ID
	bool beginning_of_partition;    // True if this packet is the first
	// in a VP8 partition. Otherwise false
};

union RTPVideoTypeHeader
{
	RTPVideoHeaderVP8 vp8;
	RTPVideoHeaderH264 h264;
	// In the future
	// RTPVideoHeaderVP9 VP9;
};

struct RTPVideoHeader
{
	uint16_t width;  // size
	uint16_t height;

	uint8_t simulcast_idx; // Extension, 0이면 사용하지 않음
	bool is_first_packet_in_frame;

	RtpVideoCodecType codec;
	RTPVideoTypeHeader codec_header;
};
