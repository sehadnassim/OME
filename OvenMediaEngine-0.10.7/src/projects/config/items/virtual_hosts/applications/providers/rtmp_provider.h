//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "provider.h"

namespace cfg
{
	struct RtmpProvider : public Provider
	{
		CFG_DECLARE_OVERRIDED_GETTER_OF(GetType, ProviderType::Rtmp)

		CFG_DECLARE_GETTER_OF(IsBlockDuplicateStreamName, _is_block_duplicate_stream_name)

	protected:
		void MakeParseList() override
		{
			Provider::MakeParseList();

			RegisterValue<Optional>("BlockDuplicateStreamName", &_is_block_duplicate_stream_name);
		}

		// true: block(disconnect) new incoming stream
		// false: don't block new incoming stream
		bool _is_block_duplicate_stream_name = true;
	};
}  // namespace cfg