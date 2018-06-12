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

#ifndef COMMON_TIME_ZONE_UTIL_H
#define COMMON_TIME_ZONE_UTIL_H

#include "../common/classes/fb_string.h"
#include "../common/cvt.h"

 // struct tm declaration
#if defined(TIME_WITH_SYS_TIME)
#include <sys/time.h>
#include <time.h>
#else
#if defined(HAVE_SYS_TIME_H)
#include <sys/time.h>
#else
#include <time.h>
#endif
#endif

namespace Firebird {

class NoThrowTimeStamp;

class TimeZoneUtil
{
public:
	static const unsigned ONE_DAY = 24 * 60 - 1;	// used for offset encoding
	static const USHORT GMT_ZONE = 65535;

	static const unsigned MAX_LEN = 32;
	static const unsigned MAX_SIZE = MAX_LEN + 1;

public:
	static USHORT getSystemTimeZone();

	static USHORT parse(const char* str, unsigned strLen);
	static unsigned format(char* buffer, size_t bufferSize, USHORT timeZone);

	static bool isValidOffset(int sign, unsigned tzh, unsigned tzm);

	static void extractOffset(const ISC_TIMESTAMP_TZ& timeStampTz, int* sign, unsigned* tzh, unsigned* tzm);

	static ISC_TIME timeTzToTime(const ISC_TIME_TZ& timeTz, USHORT toTimeZone, Callbacks* cb);
	static ISC_TIMESTAMP timeStampTzToTimeStamp(const ISC_TIMESTAMP_TZ& timeStampTz, USHORT toTimeZone);

	static void localTimeToUtc(ISC_TIME& time, Callbacks* cb);
	static void localTimeToUtc(ISC_TIME_TZ& timeTz, Callbacks* cb);

	static void localTimeStampToUtc(ISC_TIMESTAMP& timeStamp, Callbacks* cb);
	static void localTimeStampToUtc(ISC_TIMESTAMP_TZ& timeStampTz);

	static void decodeTime(const ISC_TIME_TZ& timeTz, Callbacks* cb, struct tm* times, int* fractions = NULL);
	static void decodeTimeStamp(const ISC_TIMESTAMP_TZ& timeStampTz, struct tm* times, int* fractions = NULL);

	static ISC_TIMESTAMP_TZ getCurrentTimeStampUtc();

	static void validateTimeStampUtc(NoThrowTimeStamp& ts);

	static ISC_TIMESTAMP_TZ cvtTimeToTimeStampTz(const ISC_TIME& time, Callbacks* cb);
	static ISC_TIME_TZ cvtTimeToTimeTz(const ISC_TIME& time, Callbacks* cb);

	static ISC_TIMESTAMP_TZ cvtTimeTzToTimeStampTz(const ISC_TIME_TZ& timeTz, Callbacks* cb);
	static ISC_TIMESTAMP cvtTimeTzToTimeStamp(const ISC_TIME_TZ& timeTz, Callbacks* cb);
	static ISC_TIME cvtTimeTzToTime(const ISC_TIME_TZ& timeTz, Callbacks* cb);

	static ISC_TIME_TZ cvtTimeStampTzToTimeTz(const ISC_TIMESTAMP_TZ& timeStampTz);
	static ISC_TIMESTAMP cvtTimeStampTzToTimeStamp(const ISC_TIMESTAMP_TZ& timeStampTz, Callbacks* cb);

	static ISC_TIMESTAMP_TZ cvtTimeStampToTimeStampTz(const ISC_TIMESTAMP& timeStamp, Callbacks* cb);
	static ISC_TIME_TZ cvtTimeStampToTimeTz(const ISC_TIMESTAMP& timeStamp, Callbacks* cb);

	static ISC_TIMESTAMP_TZ cvtDateToTimeStampTz(const ISC_DATE& date, Callbacks* cb);
};

}	// namespace Firebird

#endif	// COMMON_TIME_ZONE_UTIL_H
