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

// Uncomment to generate list of time zones to be updated in TimeZones.h
//#define TZ_UPDATE

#include "firebird.h"
#include "../common/TimeZoneUtil.h"
#include "../common/StatusHolder.h"
#include "../common/unicode_util.h"
#include "../common/classes/timestamp.h"
#include "../common/classes/GenericMap.h"
#include "unicode/ucal.h"

#ifdef TZ_UPDATE
#include "../common/classes/objects_array.h"
#endif

using namespace Firebird;

//-------------------------------------

namespace
{
	struct TimeZoneDesc
	{
		const char* asciiName;
		const UChar* icuName;
	};
}	// namespace

#include "./TimeZones.h"

//-------------------------------------

static const TimeZoneDesc* getDesc(USHORT timeZone);
static inline bool isOffset(USHORT timeZone);
static USHORT makeFromOffset(int sign, unsigned tzh, unsigned tzm);
static USHORT makeFromRegion(const char* str, unsigned strLen);
static inline SSHORT offsetZoneToDisplacement(USHORT timeZone);
static int parseNumber(const char*& p, const char* end);
static void skipSpaces(const char*& p, const char* end);

//-------------------------------------

namespace
{
	struct TimeZoneStartup
	{
		TimeZoneStartup(MemoryPool& pool)
			: systemTimeZone(TimeZoneUtil::GMT_ZONE),
			  nameIdMap(pool)
		{
#if defined DEV_BUILD && defined TZ_UPDATE
			tzUpdate();
#endif

			for (USHORT i = 0; i < FB_NELEM(TIME_ZONE_LIST); ++i)
			{
				string s(TIME_ZONE_LIST[i].asciiName);
				s.upper();
				nameIdMap.put(s, i);
			}

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

			int32_t len = icuLib.ucalGetTimeZoneID(icuCalendar, buffer, FB_NELEM(buffer), &icuErrorCode);

			if (!U_FAILURE(icuErrorCode))
			{
				bool error;
				string bufferStrUnicode(reinterpret_cast<const char*>(buffer), len * sizeof(USHORT));
				string bufferStrAscii(IntlUtil::convertUtf16ToAscii(bufferStrUnicode, &error));
				found = getId(bufferStrAscii, systemTimeZone);
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

		bool getId(string name, USHORT& id)
		{
			USHORT index;
			name.upper();

			if (nameIdMap.get(name, index))
			{
				id = MAX_USHORT - index;
				return true;
			}
			else
				return false;
		}

#if defined DEV_BUILD && defined TZ_UPDATE
		void tzUpdate()
		{
			SortedObjectsArray<string> currentZones, icuZones;

			for (unsigned i = 0; i < FB_NELEM(TIME_ZONE_LIST); ++i)
				currentZones.push(TIME_ZONE_LIST[i].asciiName);

			Jrd::UnicodeUtil::ConversionICU& icuLib = Jrd::UnicodeUtil::getConversionICU();
			UErrorCode icuErrorCode = U_ZERO_ERROR;

			UEnumeration* uenum = icuLib.ucalOpenTimeZones(&icuErrorCode);
			int32_t length;

			while (const UChar* str = icuLib.uenumUnext(uenum, &length, &icuErrorCode))
			{
				char buffer[256];

				for (int i = 0; i <= length; ++i)
					buffer[i] = (char) str[i];

				icuZones.push(buffer);
			}

			icuLib.uenumClose(uenum);

			for (auto const& zone : currentZones)
			{
				FB_SIZE_T pos;

				if (icuZones.find(zone, pos))
					icuZones.remove(pos);
				else
					printf("--> %s does not exist in ICU.\n", zone.c_str());
			}

			ObjectsArray<string> newZones;

			for (int i = 0; i < FB_NELEM(TIME_ZONE_LIST); ++i)
				newZones.push(TIME_ZONE_LIST[i].asciiName);

			for (auto const& zone : icuZones)
				newZones.push(zone);

			printf("// The content of this file is generated with help of macro TZ_UPDATE.\n\n");

			int index = 0;

			for (auto const& zone : newZones)
			{
				printf("static const UChar TZSTR_%d[] = {", index);

				for (int i = 0; i < zone.length(); ++i)
					printf("'%c', ", zone[i]);

				printf("'\\0'};\n");

				++index;
			}

			printf("\n");

			printf("// Do not change order of items in this array! The index corresponds to a TimeZone ID, which must be fixed!\n");
			printf("static const TimeZoneDesc TIME_ZONE_LIST[] = {");

			index = 0;

			for (auto const& zone : newZones)
			{
				printf("%s\n\t{\"%s\", TZSTR_%d}", (index == 0 ? "" : ","), zone.c_str(), index);
				++index;
			}

			printf("\n");
			printf("};\n\n");
		}
#endif	// defined DEV_BUILD && defined TZ_UPDATE

		USHORT systemTimeZone;

	private:
		GenericMap<Pair<Left<string, USHORT> > > nameIdMap;
	};
}	// namespace

//-------------------------------------

static InitInstance<TimeZoneStartup> timeZoneStartup;

//-------------------------------------

// Return the current user's time zone.
USHORT TimeZoneUtil::getSystemTimeZone()
{
	return timeZoneStartup().systemTimeZone;
}

void TimeZoneUtil::iterateRegions(std::function<void  (USHORT, const char*)> func)
{
	for (USHORT i = 0; i < FB_NELEM(TIME_ZONE_LIST); ++i)
		func(MAX_USHORT - i, TIME_ZONE_LIST[i].asciiName);
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
		strncpy(buffer, getDesc(timeZone)->asciiName, bufferSize);

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

	if (isOffset(timeStampTz.time_zone))
		displacement = offsetZoneToDisplacement(timeStampTz.time_zone);
	else
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;

		Jrd::UnicodeUtil::ConversionICU& icuLib = Jrd::UnicodeUtil::getConversionICU();

		UCalendar* icuCalendar = icuLib.ucalOpen(
			getDesc(timeStampTz.time_zone)->icuName, -1, NULL, UCAL_GREGORIAN, &icuErrorCode);

		if (!icuCalendar)
			status_exception::raise(Arg::Gds(isc_random) << "Error calling ICU's ucal_open.");

		SINT64 ticks = timeStampTz.utc_timestamp.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY +
			timeStampTz.utc_timestamp.timestamp_time;

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
ISC_TIME TimeZoneUtil::timeTzToTime(const ISC_TIME_TZ& timeTz, USHORT toTimeZone, Callbacks* cb)
{
	ISC_TIMESTAMP_TZ tempTimeStampTz;
	tempTimeStampTz.utc_timestamp.timestamp_date = cb->getLocalDate();
	tempTimeStampTz.utc_timestamp.timestamp_time = 0;
	tempTimeStampTz.time_zone = cb->getSessionTimeZone();
	TimeZoneUtil::localTimeStampToUtc(tempTimeStampTz);

	tempTimeStampTz.utc_timestamp.timestamp_time = timeTz.utc_time;
	tempTimeStampTz.time_zone = timeTz.time_zone;

	return timeStampTzToTimeStamp(tempTimeStampTz, toTimeZone).timestamp_time;
}

// Converts a timestamp-tz to a timestamp in a given zone.
ISC_TIMESTAMP TimeZoneUtil::timeStampTzToTimeStamp(const ISC_TIMESTAMP_TZ& timeStampTz, USHORT toTimeZone)
{
	ISC_TIMESTAMP_TZ tempTimeStampTz = timeStampTz;
	tempTimeStampTz.time_zone = toTimeZone;

	struct tm times;
	int fractions;
	decodeTimeStamp(tempTimeStampTz, &times, &fractions);

	return TimeStamp::encode_timestamp(&times, fractions);
}

// Converts a time from local to UTC.
void TimeZoneUtil::localTimeToUtc(ISC_TIME& time, Callbacks* cb)
{
	ISC_TIME_TZ timeTz;
	timeTz.utc_time = time;
	timeTz.time_zone = cb->getSessionTimeZone();
	localTimeToUtc(timeTz, cb);

	time = timeTz.utc_time;
}

// Converts a time from local to UTC.
void TimeZoneUtil::localTimeToUtc(ISC_TIME_TZ& timeTz, Callbacks* cb)
{
	ISC_TIMESTAMP_TZ tempTimeStampTz;
	tempTimeStampTz.utc_timestamp.timestamp_date = cb->getCurrentTimeStampUtc().timestamp_date;
	tempTimeStampTz.utc_timestamp.timestamp_time = timeTz.utc_time;
	tempTimeStampTz.time_zone = timeTz.time_zone;
	localTimeStampToUtc(tempTimeStampTz);

	timeTz.utc_time = tempTimeStampTz.utc_timestamp.timestamp_time;
}

// Converts a timestamp from its local datetime fields to UTC.
void TimeZoneUtil::localTimeStampToUtc(ISC_TIMESTAMP& timeStamp, Callbacks* cb)
{
	ISC_TIMESTAMP_TZ tempTimeStampTz;
	tempTimeStampTz.utc_timestamp.timestamp_date = timeStamp.timestamp_date;
	tempTimeStampTz.utc_timestamp.timestamp_time = timeStamp.timestamp_time;
	tempTimeStampTz.time_zone = cb->getSessionTimeZone();

	localTimeStampToUtc(tempTimeStampTz);

	timeStamp.timestamp_date = tempTimeStampTz.utc_timestamp.timestamp_date;
	timeStamp.timestamp_time = tempTimeStampTz.utc_timestamp.timestamp_time;
}

// Converts a timestamp from its local datetime fields to UTC.
void TimeZoneUtil::localTimeStampToUtc(ISC_TIMESTAMP_TZ& timeStampTz)
{
	int displacement;

	if (isOffset(timeStampTz.time_zone))
		displacement = offsetZoneToDisplacement(timeStampTz.time_zone);
	else
	{
		tm times;
		TimeStamp::decode_timestamp(*(ISC_TIMESTAMP*) &timeStampTz, &times, nullptr);

		UErrorCode icuErrorCode = U_ZERO_ERROR;

		Jrd::UnicodeUtil::ConversionICU& icuLib = Jrd::UnicodeUtil::getConversionICU();

		UCalendar* icuCalendar = icuLib.ucalOpen(
			getDesc(timeStampTz.time_zone)->icuName, -1, NULL, UCAL_GREGORIAN, &icuErrorCode);

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

	SINT64 ticks = timeStampTz.utc_timestamp.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY +
		timeStampTz.utc_timestamp.timestamp_time - (displacement * 60 * ISC_TIME_SECONDS_PRECISION);

	timeStampTz.utc_timestamp.timestamp_date = ticks / TimeStamp::ISC_TICKS_PER_DAY;
	timeStampTz.utc_timestamp.timestamp_time = ticks % TimeStamp::ISC_TICKS_PER_DAY;
}

void TimeZoneUtil::decodeTime(const ISC_TIME_TZ& timeTz, Callbacks* cb, struct tm* times, int* fractions)
{
	ISC_TIMESTAMP_TZ timeStampTz = cvtTimeTzToTimeStampTz(timeTz, cb);
	decodeTimeStamp(timeStampTz, times, fractions);
}

void TimeZoneUtil::decodeTimeStamp(const ISC_TIMESTAMP_TZ& timeStampTz, struct tm* times, int* fractions)
{
	SINT64 ticks = timeStampTz.utc_timestamp.timestamp_date * TimeStamp::ISC_TICKS_PER_DAY +
		timeStampTz.utc_timestamp.timestamp_time;
	int displacement;

	if (isOffset(timeStampTz.time_zone))
		displacement = offsetZoneToDisplacement(timeStampTz.time_zone);
	else
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;

		Jrd::UnicodeUtil::ConversionICU& icuLib = Jrd::UnicodeUtil::getConversionICU();

		UCalendar* icuCalendar = icuLib.ucalOpen(
			getDesc(timeStampTz.time_zone)->icuName, -1, NULL, UCAL_GREGORIAN, &icuErrorCode);

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

ISC_TIMESTAMP_TZ TimeZoneUtil::getCurrentTimeStampUtc()
{
	TimeStamp now = TimeStamp::getCurrentTimeStamp();

	ISC_TIMESTAMP_TZ tsTz;
	tsTz.utc_timestamp = now.value();
	tsTz.time_zone = getSystemTimeZone();
	localTimeStampToUtc(tsTz);

	return tsTz;
}

void TimeZoneUtil::validateTimeStampUtc(NoThrowTimeStamp& ts)
{
	if (ts.isEmpty())
		ts.value() = getCurrentTimeStampUtc().utc_timestamp;
}

// Converts a time to timestamp-tz.
ISC_TIMESTAMP_TZ TimeZoneUtil::cvtTimeToTimeStampTz(const ISC_TIME& time, Callbacks* cb)
{
	// SQL: source => TIMESTAMP WITHOUT TIME ZONE => TIMESTAMP WITH TIME ZONE

	ISC_TIMESTAMP_TZ tsTz;
	tsTz.utc_timestamp.timestamp_date = cb->getLocalDate();
	tsTz.utc_timestamp.timestamp_time = time;
	tsTz.time_zone = cb->getSessionTimeZone();
	localTimeStampToUtc(tsTz);

	return tsTz;
}

// Converts a time to time-tz.
ISC_TIME_TZ TimeZoneUtil::cvtTimeToTimeTz(const ISC_TIME& time, Callbacks* cb)
{
	ISC_TIMESTAMP_TZ tsTz = cvtTimeToTimeStampTz(time, cb);

	ISC_TIME_TZ timeTz;
	timeTz.utc_time = tsTz.utc_timestamp.timestamp_time;
	timeTz.time_zone = tsTz.time_zone;

	return timeTz;
}

// Converts a time-tz to timestamp-tz.
ISC_TIMESTAMP_TZ TimeZoneUtil::cvtTimeTzToTimeStampTz(const ISC_TIME_TZ& timeTz, Callbacks* cb)
{
	// SQL: Copy date fields from CURRENT_DATE and time and time zone fields from the source.

	ISC_TIMESTAMP_TZ tsTz;
	tsTz.utc_timestamp.timestamp_date = cb->getLocalDate();
	tsTz.utc_timestamp.timestamp_time = timeTzToTime(timeTz, cb->getSessionTimeZone(), cb);
	tsTz.time_zone = cb->getSessionTimeZone();
	localTimeStampToUtc(tsTz);
	tsTz.time_zone = timeTz.time_zone;

	return tsTz;
}

// Converts a time-tz to timestamp.
ISC_TIMESTAMP TimeZoneUtil::cvtTimeTzToTimeStamp(const ISC_TIME_TZ& timeTz, Callbacks* cb)
{
	// SQL: source => TIMESTAMP WITH TIME ZONE => TIMESTAMP WITHOUT TIME ZONE

	ISC_TIMESTAMP_TZ tsTz = cvtTimeTzToTimeStampTz(timeTz, cb);

	return timeStampTzToTimeStamp(tsTz, cb->getSessionTimeZone());
}

// Converts a time-tz to time.
ISC_TIME TimeZoneUtil::cvtTimeTzToTime(const ISC_TIME_TZ& timeTz, Callbacks* cb)
{
	return timeTzToTime(timeTz, cb->getSessionTimeZone(), cb);
}

// Converts a timestamp-tz to timestamp.
ISC_TIMESTAMP TimeZoneUtil::cvtTimeStampTzToTimeStamp(const ISC_TIMESTAMP_TZ& timeStampTz, Callbacks* cb)
{
	return timeStampTzToTimeStamp(timeStampTz, cb->getSessionTimeZone());
}

// Converts a timestamp-tz to time-tz.
ISC_TIME_TZ TimeZoneUtil::cvtTimeStampTzToTimeTz(const ISC_TIMESTAMP_TZ& timeStampTz)
{
	ISC_TIME_TZ timeTz;
	timeTz.utc_time = timeStampTz.utc_timestamp.timestamp_time;
	timeTz.time_zone = timeStampTz.time_zone;

	return timeTz;
}

// Converts a timestamp to timestamp-tz.
ISC_TIMESTAMP_TZ TimeZoneUtil::cvtTimeStampToTimeStampTz(const ISC_TIMESTAMP& timeStamp, Callbacks* cb)
{
	// SQL: Copy time and time zone fields from the source.

	ISC_TIMESTAMP_TZ tsTz;
	tsTz.utc_timestamp = timeStamp;
	tsTz.time_zone = cb->getSessionTimeZone();

	localTimeStampToUtc(tsTz);

	return tsTz;
}

// Converts a timestamp to time-tz.
ISC_TIME_TZ TimeZoneUtil::cvtTimeStampToTimeTz(const ISC_TIMESTAMP& timeStamp, Callbacks* cb)
{
	// SQL: source => TIMESTAMP WITH TIME ZONE => TIME WITH TIME ZONE

	ISC_TIMESTAMP_TZ tsTz = cvtTimeStampToTimeStampTz(timeStamp, cb);

	ISC_TIME_TZ timeTz;
	timeTz.utc_time = tsTz.utc_timestamp.timestamp_time;
	timeTz.time_zone = tsTz.time_zone;

	return timeTz;
}

// Converts a date to timestamp-tz.
ISC_TIMESTAMP_TZ TimeZoneUtil::cvtDateToTimeStampTz(const ISC_DATE& date, Callbacks* cb)
{
	// SQL: source => TIMESTAMP WITHOUT TIME ZONE => TIMESTAMP WITH TIME ZONE

	ISC_TIMESTAMP_TZ tsTz;
	tsTz.utc_timestamp.timestamp_date = date;
	tsTz.utc_timestamp.timestamp_time = 0;
	tsTz.time_zone = cb->getSessionTimeZone();
	localTimeStampToUtc(tsTz);

	return tsTz;
}

//-------------------------------------

static const TimeZoneDesc* getDesc(USHORT timeZone)
{
	if (MAX_USHORT - timeZone < FB_NELEM(TIME_ZONE_LIST))
		return &TIME_ZONE_LIST[MAX_USHORT - timeZone];

	status_exception::raise(Arg::Gds(isc_random) << "Invalid time zone id");	//// TODO:
	return nullptr;
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

	while (str < end &&
		((*str >= 'a' && *str <= 'z') ||
		 (*str >= 'A' && *str <= 'Z') ||
		 *str == '_' ||
		 *str == '/') ||
		 (str != start && *str >= '0' && *str <= '9') ||
		 (str != start && *str == '+') ||
		 (str != start && *str == '-'))
	{
		++str;
	}

	unsigned len = str - start;

	skipSpaces(str, end);

	if (str == end)
	{
		string s(start, len);
		USHORT id;

		if (timeZoneStartup().getId(s, id))
			return id;
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
