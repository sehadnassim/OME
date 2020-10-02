/*
 * We referenced this code from WEBRTC NATIVE CODE.

[WebRTC Native Code License]

Copyright (c) 2011, The WebRTC project authors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google nor the names of its contributors may
    be used to endorse or promote products derived from this software
    without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "rtp_packetizer_h264.h"
#include "base/ovlibrary/byte_io.h"

RtpPacketizerH264::RtpPacketizerH264()
	: _num_packets_left(0)
{
}

RtpPacketizerH264::~RtpPacketizerH264() 
{
}

size_t RtpPacketizerH264::SetPayloadData(size_t max_payload_len, size_t last_packet_reduction_len, const RTPVideoTypeHeader *rtp_type_header, FrameType frame_type,
										const uint8_t* payload_data, size_t payload_size, const FragmentationHeader* fragmentation) 
{
	_max_payload_len = max_payload_len;
	_last_packet_reduction_len = last_packet_reduction_len;
	_packetization_mode = rtp_type_header->h26X.packetization_mode;

	for(size_t i = 0; i < fragmentation->GetCount(); ++i) 
	{
		const uint8_t* buffer = &payload_data[fragmentation->fragmentation_offset[i]];
		size_t length = fragmentation->fragmentation_length[i];
		_input_fragments.push_back(Fragment(buffer, length));
	}

	if(!GeneratePackets()) 
	{
		_num_packets_left = 0;
		while (!_packets.empty()) 
		{
			_packets.pop();
		}
		return 0;
	}
	return _num_packets_left;
}

bool RtpPacketizerH264::GeneratePackets() 
{
	for (size_t i = 0; i < _input_fragments.size();) 
	{
		switch (_packetization_mode) 
		{
			case H26XPacketizationMode::SingleNalUnit:
				if (!PacketizeSingleNalu(i))
				{
					return false;
				}
				++i;
				break;
			case H26XPacketizationMode::NonInterleaved:
				size_t fragment_len = _input_fragments[i].length;

				if (i + 1 == _input_fragments.size()) 
				{
					fragment_len += _last_packet_reduction_len;
				}
				if (fragment_len > _max_payload_len) 
				{
					PacketizeFuA(i);
					++i;
				} 
				else 
				{
					i = PacketizeStapA(i);
				}
				break;
		}
	}
	return true;
}


void RtpPacketizerH264::PacketizeFuA(size_t fragment_index) 
{
	const Fragment& fragment = _input_fragments[fragment_index];
	bool is_last_fragment = fragment_index + 1 == _input_fragments.size();
	size_t payload_left = fragment.length - kNalHeaderSize;
	size_t offset = kNalHeaderSize;
	size_t per_packet_capacity = _max_payload_len - kFuAHeaderSize;

	size_t extra_len = is_last_fragment ? _last_packet_reduction_len : 0;

	size_t num_packets = (payload_left + extra_len + (per_packet_capacity - 1)) / per_packet_capacity;
	size_t payload_per_packet = (payload_left + extra_len) / num_packets;
	size_t num_larger_packets = (payload_left + extra_len) % num_packets;

	_num_packets_left += num_packets;
	while (payload_left > 0) 
	{
		if (num_packets == num_larger_packets)
		{
			++payload_per_packet;
		}
		size_t packet_length = payload_per_packet;
		if (payload_left <= packet_length) 
		{
			packet_length = payload_left;
			if (num_packets == 2) 
			{
				--packet_length;
			}
		}
		_packets.push(PacketUnit(Fragment(fragment.buffer + offset, packet_length),
		                         offset - kNalHeaderSize == 0,
		                         payload_left == packet_length, false,
		                         fragment.buffer[0]));
		offset += packet_length;
		payload_left -= packet_length;
		--num_packets;
	}
}

size_t RtpPacketizerH264::PacketizeStapA(size_t fragment_index) 
{
	// Aggregate fragments into one packet (STAP-A).
	size_t payload_size_left = _max_payload_len;
	int aggregated_fragments = 0;
	size_t fragment_headers_length = 0;
	const Fragment* fragment = &_input_fragments[fragment_index];
	++_num_packets_left;

	while (payload_size_left >= fragment->length + fragment_headers_length &&
	       (fragment_index + 1 < _input_fragments.size() ||
	        payload_size_left >= fragment->length + fragment_headers_length + _last_packet_reduction_len)) 
	{
		_packets.push(PacketUnit(*fragment, aggregated_fragments == 0, false, true, fragment->buffer[0]));
		payload_size_left -= fragment->length;
		payload_size_left -= fragment_headers_length;

		fragment_headers_length = kLengthFieldSize;

		if (aggregated_fragments == 0)
		{	
			fragment_headers_length += kNalHeaderSize + kLengthFieldSize;
		}
		++aggregated_fragments;

		// Next fragment.
		++fragment_index;
		if (fragment_index == _input_fragments.size())
		{
			break;
		}
		fragment = &_input_fragments[fragment_index];
	}
	_packets.back().last_fragment = true;
	return fragment_index;
}

bool RtpPacketizerH264::PacketizeSingleNalu(size_t fragment_index) 
{
	// Add a single NALU to the queue, no aggregation.
	size_t payload_size_left = _max_payload_len;
	if (fragment_index + 1 == _input_fragments.size())
	{	
		payload_size_left -= _last_packet_reduction_len;
	}
	
	const Fragment* fragment = &_input_fragments[fragment_index];
	if (payload_size_left < fragment->length) 
	{
		return false;
	}
	
	_packets.push(PacketUnit(*fragment, true /* first */, true /* last */, false /* aggregated */, fragment->buffer[0]));
	++_num_packets_left;
	return true;
}

bool RtpPacketizerH264::NextPacket(RtpPacket* rtp_packet) 
{
	if (_packets.empty()) 
	{
		return false;
	}

	PacketUnit packet = _packets.front();
	
	if (packet.first_fragment && packet.last_fragment) 
	{
		// Single NAL unit packet.
		size_t bytes_to_send = packet.source_fragment.length;
		uint8_t* buffer = rtp_packet->AllocatePayload(bytes_to_send);
		memcpy(buffer, packet.source_fragment.buffer, bytes_to_send);
		_packets.pop();
		_input_fragments.pop_front();
	} 
	else if (packet.aggregated) 
	{
		bool is_last_packet = _num_packets_left == 1;
		NextAggregatePacket(rtp_packet, is_last_packet);
	} 
	else 
	{
		NextFragmentPacket(rtp_packet);
	}
	
	rtp_packet->SetMarker(_packets.empty());
	--_num_packets_left;
	
	return true;
}

void RtpPacketizerH264::NextAggregatePacket(RtpPacket* rtp_packet, bool last) 
{
	uint8_t* buffer = rtp_packet->AllocatePayload(last ? _max_payload_len - _last_packet_reduction_len : _max_payload_len);
	PacketUnit* packet = &_packets.front();

	// STAP-A NALU header.
	buffer[0] = (packet->header & (kFBit | kNriMask)) | NaluType::kStapA;
	size_t index = kNalHeaderSize;
	bool is_last_fragment = packet->last_fragment;
	
	while (packet->aggregated) 
	{
		const Fragment& fragment = packet->source_fragment;
		// Add NAL unit length field.
		ByteWriter<uint16_t>::WriteBigEndian(&buffer[index], fragment.length);
		index += kLengthFieldSize;
		// Add NAL unit.
		memcpy(&buffer[index], fragment.buffer, fragment.length);
		index += fragment.length;

		_packets.pop();

		_input_fragments.pop_front();

		if (is_last_fragment)
		{	
			break;
		}
		packet = &_packets.front();
		is_last_fragment = packet->last_fragment;
	}
	rtp_packet->SetPayloadSize(index);
}

void RtpPacketizerH264::NextFragmentPacket(RtpPacket* rtp_packet) 
{
	PacketUnit* packet = &_packets.front();
	uint8_t fu_indicator = (packet->header & (kFBit | kNriMask)) | NaluType::kFuA;
	uint8_t fu_header = 0;

	fu_header |= (packet->first_fragment ? kSBit : 0);
	fu_header |= (packet->last_fragment ? kEBit : 0);
	uint8_t type = packet->header & kTypeMask;
	fu_header |= type;

	const Fragment& fragment = packet->source_fragment;
	uint8_t* buffer = rtp_packet->AllocatePayload(kFuAHeaderSize + fragment.length);
	buffer[0] = fu_indicator;
	buffer[1] = fu_header;
	memcpy(buffer + kFuAHeaderSize, fragment.buffer, fragment.length);
	if (packet->last_fragment)
	{
		_input_fragments.pop_front();
	}
	_packets.pop();
}
