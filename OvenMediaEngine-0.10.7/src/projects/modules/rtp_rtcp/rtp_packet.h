#pragma once

#include <vector>
#include <memory>
#include <base/ovlibrary/ovlibrary.h>

#define RTP_VERSION					2
#define FIXED_HEADER_SIZE			12
#define RED_HEADER_SIZE				1
#define ONE_BYTE_EXTENSION_ID		0xBEDE
#define ONE_BYTE_HEADER_SIZE		1
#define DEFAULT_MAX_PACKET_SIZE		1472

//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier            |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |            Contributing source (CSRC) identifiers             |
// |                             ....                              |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |One-byte eXtensions id = 0xbede|       length in 32bits        |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                          Extensions                           |
// |                             ....                              |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |RED(Optional)|            Payload                             |
// |             ....              :  padding...                   |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |               padding         | Padding size  |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

class RtpPacket
{
public:
	RtpPacket();
	RtpPacket(const std::shared_ptr<ov::Data> &data);
	RtpPacket(RtpPacket &src);
	virtual ~RtpPacket();

	// Getter
	bool		Marker() const;
	uint8_t		PayloadType() const;
	// For FEC Payload
	bool		IsUlpfec() const;
	uint8_t 	OriginPayloadType() const;
	uint16_t	SequenceNumber() const;
	uint32_t	Timestamp() const;
	uint32_t	Ssrc() const;
	std::vector<uint32_t> Csrcs() const;
	uint8_t*	Buffer() const;

	// Setter
	void		SetMarker(bool marker_bit);
	void		SetPayloadType(uint8_t payload_type);
	// For FEC Payload
	void 		SetUlpfec(bool is_fec, uint8_t origin_payload_type);
	void		SetSequenceNumber(uint16_t seq_no);
	void		SetTimestamp(uint32_t timestamp);
	void		SetSsrc(uint32_t ssrc);
	
	// 버퍼에 남은 공간이 충분하고 extension, Payload, padding이 들어가기 전에 호출되어야 함
	void		SetCsrcs(const std::vector<uint32_t>& csrcs);

	size_t		HeadersSize() const;
	size_t		PayloadSize() const;
	size_t		PaddingSize() const;
	size_t 		ExtensionSize() const;

	// Payload
	bool 		SetPayload(const uint8_t *payload, size_t payload_size);
	uint8_t*	SetPayloadSize(size_t size_bytes);
	uint8_t*	AllocatePayload(size_t size_bytes);
	uint8_t*	Header() const;
	uint8_t*	Payload() const;

	// Data
	std::shared_ptr<ov::Data> GetData();

protected:
	size_t		_payload_offset = 0;	// Header Start Point (Header size)
	uint8_t		_cc = 0;
	bool		_marker = false;
	uint8_t		_payload_type = 0;
	bool		_is_fec = false;
	uint8_t 	_origin_payload_type = 0;
	uint8_t		_padding_size = 0;
	uint16_t	_sequence_number = 0;
	uint32_t	_timestamp = 0;
	uint32_t	_ssrc = 0;
	size_t		_payload_size = 0;		// Payload Size

	//TODO: Extension은 향후 확장
	size_t		_extension_size;

	// BYTE로 변환된 헤더
	// std::vector<uint8_t>	_buffer;
	uint8_t *					_buffer = nullptr;
	std::shared_ptr<ov::Data>	_data = nullptr;
};

