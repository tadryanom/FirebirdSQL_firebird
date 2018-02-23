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

struct TimeZoneDesc
{
	USHORT id;
	const char* abbr;
};

//-------------------------------------

static SSHORT getDisplacement(const ISC_TIMESTAMP_TZ& timeStampTz);
static SSHORT getDisplacement(const ISC_TIMESTAMP& timeStampUtc, USHORT timeZone);
static inline bool isOffset(USHORT timeZone);
static USHORT makeFromOffset(int sign, unsigned tzh, unsigned tzm);
static USHORT makeFromRegion(const char* str, unsigned strLen);
static inline SSHORT offsetZoneToDisplacement(USHORT timeZone);
static int parseNumber(const char*& p, const char* end);
static void skipSpaces(const char*& p, const char* end);

//-------------------------------------

static const TimeZoneDesc TIME_ZONE_LIST[] = {	//// FIXME:
	{0, "BRT"},
	{1, "BRST"}
};

//-------------------------------------

//// FIXME: Windows and others ports.
// Return the current user's time zone.
USHORT TimeZoneUtil::getCurrent()
{
	//// FIXME: Return the time zone region instead of the offset.
	time_t rawtime;
	time(&rawtime);

	struct tm tm1;
	if (!localtime_r(&rawtime, &tm1))
		system_call_failed::raise("localtime_r");

	int sign = tm1.tm_gmtoff < 0 ? -1 : 1;
	unsigned tzh = (unsigned) abs(int(tm1.tm_gmtoff / 60 / 60));
	unsigned tzm = (unsigned) abs(int(tm1.tm_gmtoff / 60 % 60));

	return makeFromOffset(sign, tzh, tzm);
}

// Parses a time zone, offset- or region-based.
USHORT TimeZoneUtil::parse(const char* str, unsigned strLen)
{
	const char* end = str + strLen;
	const char* p = str;

	skipSpaces(p, end);

	int sign = 1;
	bool signPresent = false;

	if (*p == '-' || *p == '+')
	{
		signPresent = true;
		sign = *p == '-' ? -1 : 1;
		++p;
		skipSpaces(p, end);
	}

	if (signPresent || (*p >= '0' && *p <= '9'))
	{
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

		return makeFromOffset(sign, tzh, tzm);
	}
	else
		return makeFromRegion(p, str + strLen - p);
}

// Format a time zone to string, as offset or region.
unsigned TimeZoneUtil::format(char* buffer, size_t bufferSize, USHORT timeZone)
{
	char* p = buffer;

	if (isOffset(timeZone))
	{
		SSHORT displacement = offsetZoneToDisplacement(timeZone);

		*p++ = displacement < 0 ? '-' : '+';

		if (displacement < 0)
			displacement = -displacement;

		p += fb_utils::snprintf(p, bufferSize - 1, "%2.2d:%2.2d", displacement / 60, displacement % 60);
	}
	else
	{
		if (MAX_USHORT - timeZone < FB_NELEM(TIME_ZONE_LIST))
			strncpy(buffer, TIME_ZONE_LIST[MAX_USHORT - timeZone].abbr, bufferSize);
		else
		{
			fb_assert(false);
			strncpy(buffer, "*Invalid*", bufferSize);
		}

		p += strlen(buffer);
	}

	return p - buffer;
}

// Returns if the offsets are valid.
bool TimeZoneUtil::isValidOffset(int sign, unsigned tzh, unsigned tzm)
{
	fb_assert(sign >= -1 && sign <= 1);
	return tzm <= 59 && (tzh < 14 || (tzh == 14 && tzm == 0));
}

// Extracts the offsets from a offset- or region-based datetime with time zone.
void TimeZoneUtil::extractOffset(const ISC_TIMESTAMP_TZ& timeStampTz, int* sign, unsigned* tzh, unsigned* tzm)
{
	SSHORT displacement;

	if (isOffset(timeStampTz.timestamp_zone))
		displacement = offsetZoneToDisplacement(timeStampTz.timestamp_zone);
	else
	{
		ISC_TIMESTAMP ts1 = *(ISC_TIMESTAMP*) &timeStampTz;
		ISC_TIMESTAMP ts2 = timeStampTzAtZone(timeStampTz, UTC_ZONE);

		displacement =
			((ts1.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY + ts1.timestamp_time) -
			 (ts2.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY + ts2.timestamp_time)) /
			(ISC_TIME_SECONDS_PRECISION * 60);
	}

	*sign = displacement < 0 ? -1 : 1;
	displacement = displacement < 0 ? -displacement : displacement;

	*tzh = displacement / 60;
	*tzm = displacement % 60;
}

// Moves a time from one time zone to another.
ISC_TIME TimeZoneUtil::timeTzAtZone(const ISC_TIME_TZ& timeTz, USHORT atTimeZone)
{
	ISC_TIMESTAMP_TZ tempTimeStampTz;
	tempTimeStampTz.timestamp_date = TimeStamp::getCurrentTimeStamp().value().timestamp_date;
	tempTimeStampTz.timestamp_time = timeTz.time_time;
	tempTimeStampTz.timestamp_zone = timeTz.time_zone;

	return timeStampTzAtZone(tempTimeStampTz, atTimeZone).timestamp_time;
}

// Moves a timestamp from one time zone to another.
ISC_TIMESTAMP TimeZoneUtil::timeStampTzAtZone(const ISC_TIMESTAMP_TZ& timeStampTz, USHORT atTimeZone)
{
	SSHORT timeDisplacement = getDisplacement(timeStampTz);

	SINT64 ticks = timeStampTz.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY + timeStampTz.timestamp_time -
		(timeDisplacement * 60 * ISC_TIME_SECONDS_PRECISION);

	SSHORT atDisplacement;

	if (isOffset(atTimeZone))
		atDisplacement = offsetZoneToDisplacement(atTimeZone);
	else
	{
		ISC_TIMESTAMP tempTimeStampUtc;
		tempTimeStampUtc.timestamp_date = ticks / TimeStamp::ISC_TICKS_PER_DAY;
		tempTimeStampUtc.timestamp_time = ticks % TimeStamp::ISC_TICKS_PER_DAY;

		atDisplacement = getDisplacement(tempTimeStampUtc, atTimeZone);
	}

	ticks -= -atDisplacement * 60 * ISC_TIME_SECONDS_PRECISION;

	ISC_TIMESTAMP ts;
	ts.timestamp_date = ticks / TimeStamp::ISC_TICKS_PER_DAY;
	ts.timestamp_time = ticks % TimeStamp::ISC_TICKS_PER_DAY;

	return ts;
}

//-------------------------------------

// Gets the displacement that a timestamp (with time zone) is from UTC.
static SSHORT getDisplacement(const ISC_TIMESTAMP_TZ& timeStampTz)
{
	const USHORT timeZone = timeStampTz.timestamp_zone;

	if (isOffset(timeZone))
		return offsetZoneToDisplacement(timeZone);
	else
	{
		//// FIXME:
		switch (MAX_USHORT - timeZone)
		{
			case 0:	// BRT
				return -(3 * 60);

			case 1:	// BRST
				return -(2 * 60);

			default:
				fb_assert(false);
				return 0;
		}
	}
}

// Gets the displacement necessary to convert a UTC timestamp to another time zone.
static SSHORT getDisplacement(const ISC_TIMESTAMP& timeStampUtc, USHORT timeZone)
{
	if (isOffset(timeZone))
		return offsetZoneToDisplacement(timeZone);
	else
	{
		//// FIXME:
		switch (MAX_USHORT - timeZone)
		{
			case 0:	// BRT
				return -(3 * 60);

			case 1:	// BRST
				return -(2 * 60);

			default:
				fb_assert(false);
				return 0;
		}
	}
}

// Returns true if the time zone is offset-based or false if region-based.
static inline bool isOffset(USHORT timeZone)
{
	return timeZone <= TimeZoneUtil::ONE_DAY * 2;
}

// Makes a time zone id from offsets.
static USHORT makeFromOffset(int sign, unsigned tzh, unsigned tzm)
{
	if (!TimeZoneUtil::isValidOffset(sign, tzh, tzm))
		status_exception::raise(Arg::Gds(isc_random) << "Invalid time zone offset");	//// TODO:

	return (USHORT)((tzh * 60 + tzm) * sign + TimeZoneUtil::ONE_DAY);
}

// Makes a time zone id from a region.
static USHORT makeFromRegion(const char* str, unsigned strLen)
{
	const char* end = str + strLen;

	skipSpaces(str, end);

	const char* start = str;

	while (str < end && ((*str >= 'a' && *str <= 'z') || (*str >= 'A' && *str <= 'Z')))
		++str;

	unsigned len = str - start;

	skipSpaces(str, end);

	if (str == end)
	{
		//// FIXME:
		for (unsigned i = 0; i < FB_NELEM(TIME_ZONE_LIST); ++i)
		{
			const char* abbr = TIME_ZONE_LIST[i].abbr;

			if (len == strlen(abbr) && fb_utils::strnicmp(start, abbr, len) == 0)
				return MAX_USHORT - i;
		}
	}

	status_exception::raise(Arg::Gds(isc_random) << "Invalid time zone region");	//// TODO:
	return 0;
}

// Gets the displacement from a offset-based time zone id.
static inline SSHORT offsetZoneToDisplacement(USHORT timeZone)
{
	fb_assert(isOffset(timeZone));

	return (SSHORT) (int(timeZone) - TimeZoneUtil::ONE_DAY);
}

// Parses a integer number.
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

// Skip spaces and tabs.
static void skipSpaces(const char*& p, const char* end)
{
	while (p < end && (*p == ' ' || *p == '\t'))
		++p;
}
