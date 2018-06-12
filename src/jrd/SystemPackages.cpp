/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Adriano dos Santos Fernandes
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2018 Adriano dos Santos Fernandes <adrianosf@gmail.com>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "../jrd/SystemPackages.h"
#include "../jrd/ods.h"
#include "../jrd/ini.h"
#include "../jrd/TimeZone.h"

using namespace Firebird;
using namespace Jrd;


std::initializer_list<SystemPackage> SystemPackage::LIST =
{
	// packages
	{
		"RDB$TIME_ZONE_UTIL",
		ODS_13_0,
		{
			// procedures
			{
				"TRANSITIONS",
				[]
				(ThrowStatusExceptionWrapper* status, IExternalContext* /*context*/,
					IRoutineMetadata* /*metadata*/, IMetadataBuilder* inBuilder, IMetadataBuilder* outBuilder)
				{
					return FB_NEW TimeZoneTransitionsProcedure(status, inBuilder, outBuilder);
				},
				prc_selectable,
				{	// input parameters
					{"TIME_ZONE_NAME", fld_tz_name, false},
					{"FROM_TIMESTAMP", fld_timestamp_tz, false},
					{"TO_TIMESTAMP", fld_timestamp_tz, false}
				},
				{	// output parameters
					{"START_TIMESTAMP", fld_timestamp_tz, false},
					{"END_TIMESTAMP", fld_timestamp_tz, false},
					{"ZONE_OFFSET", fld_tz_offset, false},
					{"DST_OFFSET", fld_tz_offset, false},
					{"EFFECTIVE_OFFSET", fld_tz_offset, false}
				}
			},
		},
		{
			// functions
			{
				"DATABASE_VERSION",
				[]
				(ThrowStatusExceptionWrapper* status, IExternalContext* /*context*/,
					IRoutineMetadata* /*metadata*/, IMetadataBuilder* inBuilder, IMetadataBuilder* outBuilder)
				{
					return FB_NEW TimeZoneDatabaseVersionFunction(status, inBuilder, outBuilder);
				},
				{	// parameters
				},
				{fld_tz_db_version, false}
			}
		}
	}
};
