//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "stun_attribute.h"

#include "modules/ice/stun/stun_datastructure.h"

class StunMappedAddressAttribute : public StunAttribute
{
public:
	StunMappedAddressAttribute();
	StunMappedAddressAttribute(int length);
	virtual ~StunMappedAddressAttribute();

	virtual bool Parse(ov::ByteStream &stream) override;

	StunAddressFamily GetFamily() const;
	uint16_t GetPort() const;
	const ov::SocketAddress &GetAddress() const;
	int GetAddressLength() const;

	bool SetParameters(const ov::SocketAddress &address);

	bool Serialize(ov::ByteStream &stream) const noexcept override;

	virtual ov::String ToString() const override;

protected:
	StunMappedAddressAttribute(StunAttributeType type, int length);

	ov::String ToString(const char *class_name) const;

	ov::SocketAddress _address;
};
