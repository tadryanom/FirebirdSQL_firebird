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
 *  The Original Code was created by Alex Peshkov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2018 Adriano dos Santos Fernandes <adrianosf@gmail.com>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "../jrd/TimeZone.h"
#include "../jrd/Record.h"
#include "../jrd/ini.h"
#include "../jrd/tra.h"
#include "gen/ids.h"

using namespace Jrd;
using namespace Firebird;


TimeZoneSnapshot::TimeZoneSnapshot(thread_db* tdbb, MemoryPool& pool)
	: SnapshotData(pool)
{
	RecordBuffer* buffer = allocBuffer(tdbb, pool, rel_time_zones);

	Record* record = buffer->getTempRecord();
	record->nullify();

	TimeZoneUtil::iterateRegions(
		[=]
		(USHORT id, const char* name)
		{
			SINT64 idValue = id;
			putField(tdbb, record, DumpField(f_tz_id, VALUE_INTEGER, sizeof(idValue), &idValue));

			putField(tdbb, record, DumpField(f_tz_name, VALUE_STRING, static_cast<USHORT>(strlen(name)), name));

			buffer->store(record);
		}
	);
}


TimeZonesTableScan::TimeZonesTableScan(CompilerScratch* csb, const Firebird::string& alias,
		StreamType stream, jrd_rel* relation)
	: VirtualTableScan(csb, alias, stream, relation)
{
}

const Format* TimeZonesTableScan::getFormat(thread_db* tdbb, jrd_rel* relation) const
{
	return tdbb->getTransaction()->getTimeZoneSnapshot(tdbb)->getData(relation)->getFormat();
}


bool TimeZonesTableScan::retrieveRecord(thread_db* tdbb, jrd_rel* relation,
	FB_UINT64 position, Record* record) const
{
	return tdbb->getTransaction()->getTimeZoneSnapshot(tdbb)->getData(relation)->fetch(position, record);
}
