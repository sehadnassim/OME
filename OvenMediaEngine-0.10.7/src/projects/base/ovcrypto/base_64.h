//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/ovlibrary/ovlibrary.h>

namespace ov
{
	class Base64
	{
	public:
		static ov::String Encode(const Data &data);
		static ov::String Encode(const std::shared_ptr<const Data> &data);
		static std::shared_ptr<Data> Decode(const ov::String &text);
	};
}
