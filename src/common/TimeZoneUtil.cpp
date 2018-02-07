/*
 *	PROGRAM:		Firebird interface.
 *	MODULE:			TimeZoneUtil.h
 *	DESCRIPTION:	Time zone utility functions.
 *
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
#include "../common/TimeZoneUtil.h"
#include "../common/StatusHolder.h"
#include "../common/classes/timestamp.h"

using namespace Firebird;

//-------------------------------------

static int parseNumber(const char*& p, const char* end);
static void skipSpaces(const char*& p, const char* end);

//-------------------------------------

//// FIXME: Windows and others ports.
USHORT TimeZoneUtil::getCurrent()
{
	time_t rawtime;
	time(&rawtime);

	struct tm tm1;
	if (!localtime_r(&rawtime, &tm1))
		system_call_failed::raise("localtime_r");

	return (USHORT) SSHORT(tm1.tm_gmtoff / 60);
}

USHORT TimeZoneUtil::parse(const char* str, unsigned strLen)
{
	const char* end = str + strLen;
	const char* p = str;

	skipSpaces(p, end);

	int sign = 1;

	if (*p == '-' || *p == '+')
	{
		sign = *p == '-' ? -1 : 1;
		++p;
		skipSpaces(p, end);
	}

	int tzh = parseNumber(p, end);
	int tzm = 0;

	skipSpaces(p, end);

	if (*p == ':')
	{
		++p;
		skipSpaces(p, end);
		tzm = (unsigned) parseNumber(p, end);
		skipSpaces(p, end);
	}

	if (p != end)
		status_exception::raise(Arg::Gds(isc_random) << "Invalid time zone offset");	//// TODO:

	if (!isValidOffset(sign, tzh, tzm))
		status_exception::raise(Arg::Gds(isc_random) << "Invalid time zone offset");	//// TODO:

	return (USHORT)(SSHORT) (tzh * 60 + tzm) * sign;
}

unsigned TimeZoneUtil::format(char* buffer, USHORT zone)
{
	char* p = buffer;

	SSHORT displacement = (SSHORT) zone;

	*p++ = displacement < 0 ? '-' : '+';

	if (displacement < 0)
		displacement = -displacement;

	sprintf(p, "%2.2d:%2.2d", displacement / 60, displacement % 60);

	while (*p)
		p++;

	return p - buffer;
}

bool TimeZoneUtil::isValidOffset(int sign, int tzh, unsigned tzm)
{
	fb_assert(sign >= -1 && sign <= 1);
	return tzm <= 59 && (tzh < 14 || (tzh == 14 && tzm == 0));
}

void TimeZoneUtil::extractOffset(const ISC_TIMESTAMP_TZ& timeStampTz, int* sign, unsigned* tzh, unsigned* tzm)
{
	SSHORT offset = (SSHORT) timeStampTz.timestamp_zone;

	*sign = offset < 0 ? -1 : 1;
	offset = offset < 0 ? -offset : offset;

	*tzh = offset / 60;
	*tzm = offset % 60;
}

ISC_TIME TimeZoneUtil::timeTzAtZone(const ISC_TIME_TZ& timeTz, USHORT zone)
{
	SSHORT zoneDisplacement = (SSHORT) zone;

	SLONG ticks = timeTz.time_time -
		((SSHORT) timeTz.time_zone - zoneDisplacement) * 60 * ISC_TIME_SECONDS_PRECISION;

	// Make the result positive
	while (ticks < 0)
		ticks += TimeStamp::ISC_TICKS_PER_DAY;

	// And make it in the range of values for a day
	ticks %= TimeStamp::ISC_TICKS_PER_DAY;

	fb_assert(ticks >= 0 && ticks < TimeStamp::ISC_TICKS_PER_DAY);

	return (ISC_TIME) ticks;
}

ISC_TIMESTAMP TimeZoneUtil::timeStampTzAtZone(const ISC_TIMESTAMP_TZ& timeStampTz, USHORT zone)
{
	SSHORT zoneDisplacement = (SSHORT) zone;

	SINT64 ticks = timeStampTz.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY + timeStampTz.timestamp_time -
		((SSHORT) timeStampTz.timestamp_zone - zoneDisplacement) * 60 * ISC_TIME_SECONDS_PRECISION;

	ISC_TIMESTAMP ts;
	ts.timestamp_date = ticks / TimeStamp::ISC_TICKS_PER_DAY;
	ts.timestamp_time = ticks % TimeStamp::ISC_TICKS_PER_DAY;

	return ts;
}

//-------------------------------------

static void skipSpaces(const char*& p, const char* end)
{
	while (p < end && (*p == ' ' || *p == '\t'))
		++p;
}

static int parseNumber(const char*& p, const char* end)
{
	const char* start = p;
	int n = 0;

	while (p < end && *p >= '0' && *p <= '9')
		n = n * 10 + *p++ - '0';

	if (p == start)
		status_exception::raise(Arg::Gds(isc_random) << "Invalid time zone offset");	//// TODO:

	return n;
}
