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

//// TODO: Configure ICU time zone data files.
//// TODO: Update Windows ICU.
#include "firebird.h"
#include "../common/TimeZoneUtil.h"
#include "../common/StatusHolder.h"
#include "../common/unicode_util.h"
#include "../common/classes/timestamp.h"
#include "unicode/ucal.h"

using namespace Firebird;

//-------------------------------------

struct TimeZoneDesc
{
	const char* abbr;	//// FIXME: remove
	const UChar* icuName;
};

//-------------------------------------

static const TimeZoneDesc& getDesc(USHORT timeZone);
static inline bool isOffset(USHORT timeZone);
static USHORT makeFromOffset(int sign, unsigned tzh, unsigned tzm);
static USHORT makeFromRegion(const char* str, unsigned strLen);
static inline SSHORT offsetZoneToDisplacement(USHORT timeZone);
static int parseNumber(const char*& p, const char* end);
static void skipSpaces(const char*& p, const char* end);

//-------------------------------------

static const UChar TZSTR_GMT[] = {
	'G', 'M', 'T', '\0'};
static const UChar TZSTR_AMERICA_SAO_PAULO[] = {
	'A', 'm', 'e', 'r', 'i', 'c', 'a', '/', 'S', 'a', 'o', '_', 'P', 'a', 'u', 'l', 'o', '\0'};
static const UChar TZSTR_AMERICA_LOS_ANGELES[] = {
	'A', 'm', 'e', 'r', 'i', 'c', 'a', '/', 'L', 'o', 's', '_', 'A', 'n', 'g', 'e', 'l', 'e', 's', '\0'};

// Do not change order of items in this array! The index corresponds to a TimeZone ID, which must be fixed!
static const TimeZoneDesc TIME_ZONE_LIST[] = {	//// FIXME:
	{"GMT", TZSTR_GMT},
	{"America/Sao_Paulo", TZSTR_AMERICA_SAO_PAULO},
	{"America/Los_Angeles", TZSTR_AMERICA_LOS_ANGELES}
};

//-------------------------------------

struct TimeZoneStartup
{
	TimeZoneStartup(MemoryPool& p)
		: systemTimeZone(TimeZoneUtil::UTC_ZONE)
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;

		Jrd::UnicodeUtil::ConversionICU& icuLib = Jrd::UnicodeUtil::getConversionICU();
		UCalendar* icuCalendar = icuLib.ucalOpen(NULL, -1, NULL, UCAL_GREGORIAN, &icuErrorCode);

		if (!icuCalendar)
		{
			gds__log("ICU's ucal_open error opening the default callendar.");
			return;
		}

		UChar buffer[TimeZoneUtil::MAX_SIZE];
		bool found = false;

		icuLib.ucalGetTimeZoneID(icuCalendar, buffer, FB_NELEM(buffer), &icuErrorCode);

		if (!U_FAILURE(icuErrorCode))
		{
			for (unsigned i = 0; i < FB_NELEM(TIME_ZONE_LIST) && !found; ++i)
			{
				if (icuLib.ustrcmp(TIME_ZONE_LIST[i].icuName, buffer) == 0)
				{
					systemTimeZone = MAX_USHORT - i;
					found = true;
				}
			}
		}
		else
			icuErrorCode = U_ZERO_ERROR;

		if (found)
		{
			icuLib.ucalClose(icuCalendar);
			return;
		}

		gds__log("ICU error retrieving the system time zone: %d. Fallbacking to displacement.", int(icuErrorCode));

		int32_t displacement = (icuLib.ucalGet(icuCalendar, UCAL_ZONE_OFFSET, &icuErrorCode) +
			icuLib.ucalGet(icuCalendar, UCAL_DST_OFFSET, &icuErrorCode)) / U_MILLIS_PER_MINUTE;

		icuLib.ucalClose(icuCalendar);

		if (!U_FAILURE(icuErrorCode))
		{
			int sign = displacement < 0 ? -1 : 1;
			unsigned tzh = (unsigned) abs(int(displacement / 60));
			unsigned tzm = (unsigned) abs(int(displacement % 60));
			systemTimeZone = makeFromOffset(sign, tzh, tzm);
		}
		else
			gds__log("Cannot retrieve the system time zone: %d.", int(icuErrorCode));
	}

	USHORT systemTimeZone;
};

static InitInstance<TimeZoneStartup> timeZoneStartup;

//-------------------------------------

// Return the current user's time zone.
USHORT TimeZoneUtil::getSystemTimeZone()
{
	return timeZoneStartup().systemTimeZone;
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
		strncpy(buffer, getDesc(timeZone).abbr, bufferSize);

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
		UErrorCode icuErrorCode = U_ZERO_ERROR;

		Jrd::UnicodeUtil::ConversionICU& icuLib = Jrd::UnicodeUtil::getConversionICU();

		UCalendar* icuCalendar = icuLib.ucalOpen(
			getDesc(timeStampTz.timestamp_zone).icuName, -1, NULL, UCAL_GREGORIAN, &icuErrorCode);

		if (!icuCalendar)
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_open.");

		SINT64 ticks = timeStampTz.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY + timeStampTz.timestamp_time;

		icuLib.ucalSetMillis(icuCalendar, (ticks - (40587 * TimeStamp::ISC_TICKS_PER_DAY)) / 10, &icuErrorCode);

		if (U_FAILURE(icuErrorCode))
		{
			icuLib.ucalClose(icuCalendar);
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_setMillis.");
		}

		displacement = (icuLib.ucalGet(icuCalendar, UCAL_ZONE_OFFSET, &icuErrorCode) +
			icuLib.ucalGet(icuCalendar, UCAL_DST_OFFSET, &icuErrorCode)) / U_MILLIS_PER_MINUTE;

		if (U_FAILURE(icuErrorCode))
		{
			icuLib.ucalClose(icuCalendar);
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_get.");
		}

		icuLib.ucalClose(icuCalendar);
	}

	*sign = displacement < 0 ? -1 : 1;
	displacement = displacement < 0 ? -displacement : displacement;

	*tzh = displacement / 60;
	*tzm = displacement % 60;
}

// Converts a time-tz to a time in a given zone.
ISC_TIME TimeZoneUtil::timeTzToTime(const ISC_TIME_TZ& timeTz, USHORT toTimeZone)
{
	//// TODO:
	ISC_TIMESTAMP_TZ tempTimeStampTz;
	tempTimeStampTz.timestamp_date = TimeStamp::getCurrentTimeStamp().value().timestamp_date;
	tempTimeStampTz.timestamp_time = timeTz.time_time;
	tempTimeStampTz.timestamp_zone = timeTz.time_zone;

	return timeStampTzToTimeStamp(tempTimeStampTz, toTimeZone).timestamp_time;
}

// Converts a timestamp-tz to a timestamp in a given zone.
ISC_TIMESTAMP TimeZoneUtil::timeStampTzToTimeStamp(const ISC_TIMESTAMP_TZ& timeStampTz, USHORT toTimeZone)
{
	ISC_TIMESTAMP_TZ tempTimeStampTz = timeStampTz;
	tempTimeStampTz.timestamp_zone = toTimeZone;

	struct tm times;
	int fractions;
	decodeTimeStamp(tempTimeStampTz, &times, &fractions);

	return TimeStamp::encode_timestamp(&times, fractions);
}

// Converts a time from local to UTC.
void TimeZoneUtil::localTimeToUtc(ISC_TIME& time, Callbacks* cb)
{
	//// TODO:
	ISC_TIMESTAMP_TZ tempTimeStampTz;
	tempTimeStampTz.timestamp_date = TimeStamp::getCurrentTimeStamp().value().timestamp_date;
	tempTimeStampTz.timestamp_time = time;
	tempTimeStampTz.timestamp_zone = cb->getSessionTimeZone();

	localTimeStampToUtc(tempTimeStampTz);

	time = tempTimeStampTz.timestamp_time;
}

// Converts a time from local to UTC.
void TimeZoneUtil::localTimeToUtc(ISC_TIME_TZ& timeTz)
{
	//// TODO:
	ISC_TIMESTAMP_TZ tempTimeStampTz;
	tempTimeStampTz.timestamp_date = TimeStamp::getCurrentTimeStamp().value().timestamp_date;
	tempTimeStampTz.timestamp_time = timeTz.time_time;
	tempTimeStampTz.timestamp_zone = timeTz.time_zone;

	localTimeStampToUtc(tempTimeStampTz);

	timeTz.time_time = tempTimeStampTz.timestamp_time;
}

// Converts a timestamp from its local datetime fields to UTC.
void TimeZoneUtil::localTimeStampToUtc(ISC_TIMESTAMP& timeStamp, Callbacks* cb)
{
	ISC_TIMESTAMP_TZ tempTimeStampTz;
	tempTimeStampTz.timestamp_date = timeStamp.timestamp_date;
	tempTimeStampTz.timestamp_time = timeStamp.timestamp_time;
	tempTimeStampTz.timestamp_zone = cb->getSessionTimeZone();

	localTimeStampToUtc(tempTimeStampTz);

	timeStamp.timestamp_date = tempTimeStampTz.timestamp_date;
	timeStamp.timestamp_time = tempTimeStampTz.timestamp_time;
}

// Converts a timestamp from its local datetime fields to UTC.
void TimeZoneUtil::localTimeStampToUtc(ISC_TIMESTAMP_TZ& timeStampTz)
{
	int displacement;

	if (isOffset(timeStampTz.timestamp_zone))
		displacement = offsetZoneToDisplacement(timeStampTz.timestamp_zone);
	else
	{
		tm times;
		TimeStamp::decode_timestamp(*(ISC_TIMESTAMP*) &timeStampTz, &times, nullptr);

		UErrorCode icuErrorCode = U_ZERO_ERROR;

		Jrd::UnicodeUtil::ConversionICU& icuLib = Jrd::UnicodeUtil::getConversionICU();

		UCalendar* icuCalendar = icuLib.ucalOpen(
			getDesc(timeStampTz.timestamp_zone).icuName, -1, NULL, UCAL_GREGORIAN, &icuErrorCode);

		if (!icuCalendar)
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_open.");

		icuLib.ucalSetDateTime(icuCalendar, 1900 + times.tm_year, times.tm_mon, times.tm_mday,
			times.tm_hour, times.tm_min, times.tm_sec, &icuErrorCode);

		if (U_FAILURE(icuErrorCode))
		{
			icuLib.ucalClose(icuCalendar);
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_setDateTime.");
		}

		displacement = (icuLib.ucalGet(icuCalendar, UCAL_ZONE_OFFSET, &icuErrorCode) +
			icuLib.ucalGet(icuCalendar, UCAL_DST_OFFSET, &icuErrorCode)) / U_MILLIS_PER_MINUTE;

		if (U_FAILURE(icuErrorCode))
		{
			icuLib.ucalClose(icuCalendar);
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_get.");
		}

		icuLib.ucalClose(icuCalendar);
	}

	SINT64 ticks = timeStampTz.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY + timeStampTz.timestamp_time -
		(displacement * 60 * ISC_TIME_SECONDS_PRECISION);

	timeStampTz.timestamp_date = ticks / TimeStamp::ISC_TICKS_PER_DAY;
	timeStampTz.timestamp_time = ticks % TimeStamp::ISC_TICKS_PER_DAY;
}

void TimeZoneUtil::decodeTime(const ISC_TIME_TZ& timeTz, struct tm* times, int* fractions)
{
	//// TODO:
	ISC_TIMESTAMP_TZ timeStampTz;
	timeStampTz.timestamp_date = TimeStamp::getCurrentTimeStamp().value().timestamp_date;
	timeStampTz.timestamp_time = timeTz.time_time;
	timeStampTz.timestamp_zone = timeTz.time_zone;

	decodeTimeStamp(timeStampTz, times, fractions);
}

void TimeZoneUtil::decodeTimeStamp(const ISC_TIMESTAMP_TZ& timeStampTz, struct tm* times, int* fractions)
{
	SINT64 ticks = timeStampTz.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY + timeStampTz.timestamp_time;
	int displacement;

	if (isOffset(timeStampTz.timestamp_zone))
		displacement = offsetZoneToDisplacement(timeStampTz.timestamp_zone);
	else
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;

		Jrd::UnicodeUtil::ConversionICU& icuLib = Jrd::UnicodeUtil::getConversionICU();

		UCalendar* icuCalendar = icuLib.ucalOpen(
			getDesc(timeStampTz.timestamp_zone).icuName, -1, NULL, UCAL_GREGORIAN, &icuErrorCode);

		if (!icuCalendar)
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_open.");

		icuLib.ucalSetMillis(icuCalendar, (ticks - (40587 * TimeStamp::ISC_TICKS_PER_DAY)) / 10, &icuErrorCode);

		if (U_FAILURE(icuErrorCode))
		{
			icuLib.ucalClose(icuCalendar);
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_setMillis.");
		}

		displacement = (icuLib.ucalGet(icuCalendar, UCAL_ZONE_OFFSET, &icuErrorCode) +
			icuLib.ucalGet(icuCalendar, UCAL_DST_OFFSET, &icuErrorCode)) / U_MILLIS_PER_MINUTE;

		if (U_FAILURE(icuErrorCode))
		{
			icuLib.ucalClose(icuCalendar);
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_get.");
		}

		icuLib.ucalClose(icuCalendar);
	}

	ticks += displacement * 60 * ISC_TIME_SECONDS_PRECISION;

	ISC_TIMESTAMP ts;
	ts.timestamp_date = ticks / TimeStamp::ISC_TICKS_PER_DAY;
	ts.timestamp_time = ticks % TimeStamp::ISC_TICKS_PER_DAY;

	TimeStamp::decode_timestamp(ts, times, fractions);
}

//-------------------------------------

static const TimeZoneDesc& getDesc(USHORT timeZone)
{
	if (MAX_USHORT - timeZone < FB_NELEM(TIME_ZONE_LIST))
		return TIME_ZONE_LIST[MAX_USHORT - timeZone];

	status_exception::raise(Arg::Gds(isc_random) << "Invalid time zone id");	//// TODO:
	return *(TimeZoneDesc*) nullptr;
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

	while (str < end && ((*str >= 'a' && *str <= 'z') || (*str >= 'A' && *str <= 'Z') || *str == '_' || *str == '/'))
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
