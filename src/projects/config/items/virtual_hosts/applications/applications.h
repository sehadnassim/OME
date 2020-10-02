//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "application.h"

namespace cfg
{
	struct Applications : public Item
	{
		CFG_DECLARE_REF_GETTER_OF(GetApplicationList, _application_list)

	protected:
		void MakeParseList() override
		{
			RegisterValue<Optional>("Application", &_application_list);
		}

		std::vector<Application> _application_list;
	};
}  // namespace cfg