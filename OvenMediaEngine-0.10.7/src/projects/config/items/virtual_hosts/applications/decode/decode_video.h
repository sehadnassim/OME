//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

namespace cfg
{
	struct DecodeVideo : public Item
	{
	protected:
		void MakeParseList() override
		{
			RegisterValue("HWAcceleration", &_hw_acceleration);
		}

		ov::String _hw_acceleration;
	};
}  // namespace cfg