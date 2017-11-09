/*
 *	PROGRAM:	Client/Server Common Code
 *	MODULE:		timestamp.cpp
 *	DESCRIPTION:	Date/time handling class
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 *
 * NS: The code now contains much of the logic from original gds.c
 *     this is why we need to use IPL license for it
 */

#include "firebird.h"
#include "fb_exception.h"
#include "../common/gdsassert.h"

#ifdef HAVE_SYS_TIMES_H
#include <sys/times.h>
#endif
#ifdef HAVE_SYS_TIMEB_H
#include <sys/timeb.h>
#endif

#include "../common/classes/timestamp.h"

namespace Firebird {

void TimeStamp::report_error(const char* msg)
{
	system_call_failed::raise(msg);
}

TimeStamp TimeStamp::getCurrentTimeStamp()
{
	const char* error = NULL;
	TimeStamp result(NoThrowTimeStamp::getCurrentTimeStamp(&error));

	if (error)
	{
		report_error(error);
	}

	return result;
}

//// FIXME: Windows and others ports.
SSHORT TimeStamp::getCurrentTimeZone()
{
	time_t rawtime;
	time(&rawtime);

	struct tm tm1;
	if (!localtime_r(&rawtime, &tm1))
		report_error("localtime_r");

	return tm1.tm_gmtoff / 60;
}

} // namespace
