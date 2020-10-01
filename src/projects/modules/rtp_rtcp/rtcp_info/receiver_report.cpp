#include "receiver_report.h"
#include "rtcp_private.h"
#include <base/ovlibrary/byte_io.h>

// RTCP receiver report (RFC 3550).
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P|    RC   |   PT=RR=201   |             length            |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                     SSRC of packet sender                     |
//  +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//  |                         report block(s)                       |
//  |                            ....                               |

bool ReceiverReport::Parse(const RtcpPacket &packet)
{
	const uint8_t *payload = packet.GetPayload();
	size_t payload_size = packet.GetPayloadSize();

	if(payload_size < static_cast<size_t>(4 /*sender ssrc size*/ + (packet.GetReportCount() * RTCP_REPORT_BLOCK_SIZE)))
	{
		logtd("Payload is too small to parse receiver report");
		return false;
	}

	SetSenderSsrc(ByteReader<uint32_t>::ReadBigEndian(&payload[0]));

	// Report blocks
	size_t offset = 4; // sender ssrc size
	for(int i=0; i<packet.GetReportCount(); i++)
	{
		auto report_block = std::make_shared<ReportBlock>();
		if(report_block->Parse(payload + offset, RTCP_REPORT_BLOCK_SIZE) == false)
		{
			return false;
		}

		_report_blocks.push_back(report_block);

		offset += RTCP_REPORT_BLOCK_SIZE;
	}

	return true;
}

// RtcpInfo must provide raw data
std::shared_ptr<ov::Data> ReceiverReport::GetData() const
{
	return nullptr;
}

void ReceiverReport::DebugPrint()
{
	for(size_t i=0; i<GetReportBlockCount(); i++)
	{
		auto report = GetReportBlock(i);
		if(report == nullptr)
		{
			return;
		}

		report->Print();
	}
}