//==============================================================================
//
//  OvenMediaEngine
//
//  Created by getroot
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#pragma once

#include <base/ovlibrary/ovlibrary.h>

#include <srtp2/srtp.h>

class SrtpAdapter
{
public:
	SrtpAdapter();
	virtual ~SrtpAdapter();
	
	bool	Release();

	bool	SetKey(srtp_ssrc_type_t type, uint64_t crypto_suite, std::shared_ptr<ov::Data> key);

	bool	ProtectRtp(std::shared_ptr<ov::Data> data);

    bool	ProtectRtcp(std::shared_ptr<ov::Data> data);
    bool	UnprotectRtcp(const std::shared_ptr<ov::Data> &data);

private:
	srtp_ctx_t_* 	_session;
	uint32_t 		_rtp_auth_tag_len;
    uint32_t 		_rtcp_auth_tag_len;
};
