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

#ifndef JRD_SYSTEM_PACKAGES_H
#define JRD_SYSTEM_PACKAGES_H

#include "firebird.h"
#include "../common/status.h"
#include "../jrd/constants.h"
#include "firebird/Interface.h"
#include <initializer_list>
#include <functional>

namespace Jrd
{
	struct SystemProcedureParameter
	{
		const char* name;
		USHORT fieldId;
		bool nullable;
	};

	struct SystemProcedure
	{
		const char* name;
		std::function<Firebird::IExternalProcedure* (
				Firebird::ThrowStatusExceptionWrapper*,
				Firebird::IExternalContext*,
				Firebird::IRoutineMetadata*,
				Firebird::IMetadataBuilder*,
				Firebird::IMetadataBuilder*
			)> factory;
		prc_t type;
		std::initializer_list<SystemProcedureParameter> inputParameters;
		std::initializer_list<SystemProcedureParameter> outputParameters;
	};

	struct SystemFunctionParameter
	{
		const char* name;
		USHORT fieldId;
		bool nullable;
	};

	struct SystemFunctionReturnType
	{
		USHORT fieldId;
		bool nullable;
	};

	struct SystemFunction
	{
		const char* name;
		std::function<Firebird::IExternalFunction* (
				Firebird::ThrowStatusExceptionWrapper*,
				Firebird::IExternalContext*,
				Firebird::IRoutineMetadata*,
				Firebird::IMetadataBuilder*,
				Firebird::IMetadataBuilder*
			)> factory;
		std::initializer_list<SystemFunctionParameter> parameters;
		SystemFunctionReturnType returnType;
	};

	struct SystemPackage
	{
		const char* name;
		USHORT odsVersion;
		std::initializer_list<SystemProcedure> procedures;
		std::initializer_list<SystemFunction> functions;

		static std::initializer_list<SystemPackage> LIST;
	};
}	// namespace Jrd

#endif	// JRD_SYSTEM_PACKAGES_H
