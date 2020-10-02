#pragma once
#include "base/ovlibrary/ovlibrary.h"
#include "rtcp_info.h"
#include "../rtcp_packet.h"

class NACK : public RtcpInfo
{
public:
	///////////////////////////////////////////
	// Implement RtcpInfo virtual functions
	///////////////////////////////////////////
	bool Parse(const RtcpPacket &header) override;
	// RtcpInfo must provide raw data
	std::shared_ptr<ov::Data> GetData() const override;
	void DebugPrint() override;

	// RtcpInfo must provide packet type
	RtcpPacketType GetPacketType() const override
	{
		return RtcpPacketType::RTPFB;
	}

	// If the packet type is one of the feedback messages (205, 206) child must provide fmt(format)
	uint8_t GetFmt() const override
	{
		return static_cast<uint8_t>(RTPFBFMT::NACK);
	}
	
	// Feedback
	uint32_t GetSrcSsrc(){return _src_ssrc;}
	void SetSrcSsrc(uint32_t ssrc){_src_ssrc = ssrc;}
	uint32_t GetMediaSsrc(){return _media_ssrc;}
	void SetMediaSsrc(uint32_t ssrc){_media_ssrc = ssrc;}

	size_t GetLostIdCount(){return _lost_ids.size();}
	uint16_t GetLostId(size_t index)
	{
		if(index > GetLostIdCount() - 1)
		{
			return 0;
		}

		return _lost_ids[index];
	}

private:
	uint32_t	_src_ssrc = 0;
	uint32_t	_media_ssrc = 0;

	std::vector<uint16_t> _lost_ids;
};