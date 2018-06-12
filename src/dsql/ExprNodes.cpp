/*
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
 * Adriano dos Santos Fernandes - refactored from pass1.cpp, gen.cpp, cmp.cpp, par.cpp and evl.cpp
 */

#include "firebird.h"
#include <math.h>
#include <ctype.h>
#include "../common/TimeZoneUtil.h"
#include "../common/classes/FpeControl.h"
#include "../common/classes/VaryStr.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/BoolNodes.h"
#include "../dsql/StmtNodes.h"
#include "../jrd/align.h"
#include "../jrd/blr.h"
#include "../jrd/tra.h"
#include "../jrd/Function.h"
#include "../jrd/SysFunction.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../jrd/recsrc/Cursor.h"
#include "../jrd/Optimizer.h"
#include "../jrd/recsrc/Cursor.h"
#include "../jrd/blb_proto.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/cvt_proto.h"
#include "../jrd/dpm_proto.h"
#include "../common/dsc_proto.h"
#include "../jrd/evl_proto.h"
#include "../jrd/exe_proto.h"
#include "../jrd/fun_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/pag_proto.h"
#include "../jrd/par_proto.h"
#include "../jrd/cvt2_proto.h"
#include "../dsql/ddl_proto.h"
#include "../dsql/errd_proto.h"
#include "../dsql/gen_proto.h"
#include "../dsql/make_proto.h"
#include "../dsql/metd_proto.h"
#include "../dsql/pass1_proto.h"
#include "../dsql/utld_proto.h"
#include "../dsql/DSqlDataTypeUtil.h"
#include "../jrd/DataTypeUtil.h"
#include "../jrd/Collation.h"
#include "../jrd/trace/TraceManager.h"
#include "../jrd/trace/TraceObjects.h"
#include "../jrd/trace/TraceJrdHelpers.h"

using namespace Firebird;
using namespace Jrd;

namespace
{
	// Expand DBKEY for view
	void expandViewNodes(thread_db* tdbb, CompilerScratch* csb, StreamType stream,
						 ValueExprNodeStack& stack, UCHAR blrOp)
	{
		const CompilerScratch::csb_repeat* const csb_tail = &csb->csb_rpt[stream];

		// If the stream's dbkey should be ignored, do so

		if (csb_tail->csb_flags & csb_no_dbkey)
			return;

		// If the stream references a view, follow map

		const StreamType* map = csb_tail->csb_map;
		if (map)
		{
			++map;

			while (*map)
				expandViewNodes(tdbb, csb, *map++, stack, blrOp);

			return;
		}

		// Relation is primitive - make DBKEY node

		if (csb_tail->csb_relation)
		{
			RecordKeyNode* node = FB_NEW_POOL(csb->csb_pool) RecordKeyNode(csb->csb_pool, blrOp);
			node->recStream = stream;
			stack.push(node);
		}
	}

	// Try to expand the given stream. If it's a view reference, collect its base streams
	// (the ones directly residing in the FROM clause) and recurse.
	void expandViewStreams(CompilerScratch* csb, StreamType stream, SortedStreamList& streams)
	{
		const CompilerScratch::csb_repeat* const csb_tail = &csb->csb_rpt[stream];

		const RseNode* const rse =
			csb_tail->csb_relation ? csb_tail->csb_relation->rel_view_rse : NULL;

		// If we have a view, collect its base streams and remap/expand them.

		if (rse)
		{
			const StreamType* const map = csb_tail->csb_map;
			fb_assert(map);

			StreamList viewStreams;
			rse->computeRseStreams(viewStreams);

			for (StreamType* iter = viewStreams.begin(); iter != viewStreams.end(); ++iter)
			{
				// Remap stream and expand it recursively
				expandViewStreams(csb, map[*iter], streams);
			}

			return;
		}

		// Otherwise, just add the current stream to the list.

		if (!streams.exist(stream))
			streams.add(stream);
	}

	// Look at all RSEs which are lower in scope than the RSE which this field is
	// referencing, and mark them as variant - the rule is that if a field from one RSE is
	// referenced within the scope of another RSE, the inner RSE can't be invariant.
	// This won't optimize all cases, but it is the simplest operating assumption for now.
	void markVariant(CompilerScratch* csb, StreamType stream)
	{
		if (csb->csb_current_nodes.isEmpty())
			return;

		for (ExprNode** node = csb->csb_current_nodes.end() - 1;
			 node >= csb->csb_current_nodes.begin(); --node)
		{
			RseNode* const rseNode = nodeAs<RseNode>(*node);

			if (rseNode)
			{
				if (rseNode->containsStream(stream))
					break;

				rseNode->flags |= RseNode::FLAG_VARIANT;
			}
			else if (*node)
				(*node)->nodFlags &= ~ExprNode::FLAG_INVARIANT;
		}
	}
}

namespace Jrd {


static const long LONG_POS_MAX = 2147483647;
static const SINT64 MAX_INT64_LIMIT = MAX_SINT64 / 10;
static const SINT64 MIN_INT64_LIMIT = MIN_SINT64 / 10;
static const SINT64 SECONDS_PER_DAY = TimeStamp::SECONDS_PER_DAY;
static const SINT64 ISC_TICKS_PER_DAY = TimeStamp::ISC_TICKS_PER_DAY;
static const SCHAR DIALECT_3_TIMESTAMP_SCALE = -9;
static const SCHAR DIALECT_1_TIMESTAMP_SCALE = 0;

static bool couldBeDate(const dsc desc);
static SINT64 getDayFraction(const dsc* d);
static SINT64 getTimeStampToIscTicks(thread_db* tdbb, const dsc* d);
static bool isDateAndTime(const dsc& d1, const dsc& d2);
static void setParameterInfo(dsql_par* parameter, const dsql_ctx* context);


//--------------------


void NodeRef::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	internalPass2(tdbb, csb);

	ExprNode* node = getExpr();

	// Bind values of invariant nodes to top-level RSE (if present)
	if (node && (node->nodFlags & ExprNode::FLAG_INVARIANT))
	{
		if (csb->csb_current_nodes.hasData())
		{
			RseNode* topRseNode = nodeAs<RseNode>(csb->csb_current_nodes[0]);
			fb_assert(topRseNode);

			if (!topRseNode->rse_invariants)
			{
				topRseNode->rse_invariants =
					FB_NEW_POOL(*tdbb->getDefaultPool()) VarInvariantArray(*tdbb->getDefaultPool());
			}

			topRseNode->rse_invariants->add(node->impureOffset);
		}
	}
}


//--------------------


void Printable::print(NodePrinter& printer) const
{
	NodePrinter subPrinter(printer.getIndent() + 1);
	Firebird::string tag(internalPrint(subPrinter));
	printer.begin(tag);
	printer.append(subPrinter);
	printer.end();
}


//--------------------


string Node::internalPrint(NodePrinter& printer) const
{
	NODE_PRINT(printer, line);
	NODE_PRINT(printer, column);

	return "Node";
}


//--------------------


string ExprNode::internalPrint(NodePrinter& printer) const
{
	Node::internalPrint(printer);

	NODE_PRINT(printer, nodFlags);
	NODE_PRINT(printer, impureOffset);

	return "ExprNode";
}


bool ExprNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (other->type != type)
		return false;

	NodeRefsHolder thisHolder(dsqlScratch->getPool());
	getChildren(thisHolder, true);

	NodeRefsHolder otherHolder(dsqlScratch->getPool());
	other->getChildren(otherHolder, true);

	size_t count = thisHolder.refs.getCount();
	if (otherHolder.refs.getCount() != count)
		return false;

	const auto* j = otherHolder.refs.begin();

	for (const auto& i : thisHolder.refs)
	{
		if (!*i != !**j || !PASS1_node_match(dsqlScratch, i->getExpr(), (*j)->getExpr(), ignoreMapCast))
			return false;

		++j;
	}

	return true;
}

bool ExprNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (other->type != type)
		return false;

	NodeRefsHolder thisHolder(csb->csb_pool);
	getChildren(thisHolder, false);

	NodeRefsHolder otherHolder(csb->csb_pool);
	other->getChildren(otherHolder, false);

	size_t count = thisHolder.refs.getCount();
	if (otherHolder.refs.getCount() != count)
		return false;

	const auto* j = otherHolder.refs.begin();

	for (const auto& i : thisHolder.refs)
	{
		if (!*i && !**j)
			continue;

		if (!*i || !**j || !i->getExpr()->sameAs(csb, (*j)->getExpr(), ignoreStreams))
			return false;

		++j;
	}

	return true;
}

bool ExprNode::possiblyUnknown(OptimizerBlk* opt)
{
	NodeRefsHolder holder(opt->getPool());
	getChildren(holder, false);

	for (NodeRef** i = holder.refs.begin(); i != holder.refs.end(); ++i)
	{
		if (**i && (*i)->getExpr()->possiblyUnknown(opt))
			return true;
	}

	return false;
}

bool ExprNode::unmappable(CompilerScratch* csb, const MapNode* mapNode, StreamType shellStream)
{
	NodeRefsHolder holder(csb->csb_pool);
	getChildren(holder, false);

	for (NodeRef** i = holder.refs.begin(); i != holder.refs.end(); ++i)
	{
		if (**i && !(*i)->getExpr()->unmappable(csb, mapNode, shellStream))
			return false;
	}

	return true;
}

void ExprNode::collectStreams(CompilerScratch* csb, SortedStreamList& streamList) const
{
	NodeRefsHolder holder(csb->csb_pool);
	getChildren(holder, false);

	for (const NodeRef* const* i = holder.refs.begin(); i != holder.refs.end(); ++i)
	{
		if (**i)
			(*i)->getExpr()->collectStreams(csb, streamList);
	}
}

bool ExprNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	NodeRefsHolder holder(csb->csb_pool);
	getChildren(holder, false);

	for (auto& i : holder.refs)
	{
		if (*i && !i->getExpr()->computable(csb, stream, allowOnlyCurrentStream))
			return false;
	}

	return true;
}

void ExprNode::findDependentFromStreams(const OptimizerRetrieval* optRet, SortedStreamList* streamList)
{
	NodeRefsHolder holder(optRet->getPool());
	getChildren(holder, false);

	for (auto& i : holder.refs)
	{
		if (*i)
			i->getExpr()->findDependentFromStreams(optRet, streamList);
	}
}

ExprNode* ExprNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	NodeRefsHolder holder(csb->csb_pool);
	getChildren(holder, false);

	for (auto& i : holder.refs)
	{
		if (*i)
			i->pass1(tdbb, csb);
	}

	return this;
}

ExprNode* ExprNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	NodeRefsHolder holder(csb->csb_pool);
	getChildren(holder, false);

	for (auto& i : holder.refs)
	{
		if (*i)
			i->pass2(tdbb, csb);
	}

	return this;
}


//--------------------


string ValueExprNode::internalPrint(NodePrinter& printer) const
{
	ExprNode::internalPrint(printer);

	NODE_PRINT(printer, nodScale);
	NODE_PRINT(printer, nodDesc);

	return "ValueExprNode";
}


//--------------------


Firebird::string ValueListNode::internalPrint(NodePrinter& printer) const
{
	ListExprNode::internalPrint(printer);

	NODE_PRINT(printer, items);

	return "ValueListNode";
}


//--------------------


Firebird::string RecSourceListNode::internalPrint(NodePrinter& printer) const
{
	ListExprNode::internalPrint(printer);

	NODE_PRINT(printer, items);

	return "RecSourceListNode";
}


//--------------------


static RegisterNode<ArithmeticNode> regArithmeticNodeAdd(blr_add);
static RegisterNode<ArithmeticNode> regArithmeticNodeSubtract(blr_subtract);
static RegisterNode<ArithmeticNode> regArithmeticNodeMultiply(blr_multiply);
static RegisterNode<ArithmeticNode> regArithmeticNodeDivide(blr_divide);

ArithmeticNode::ArithmeticNode(MemoryPool& pool, UCHAR aBlrOp, bool aDialect1,
			ValueExprNode* aArg1, ValueExprNode* aArg2)
	: TypedNode<ValueExprNode, ExprNode::TYPE_ARITHMETIC>(pool),
	  blrOp(aBlrOp),
	  dialect1(aDialect1),
	  label(pool),
	  arg1(aArg1),
	  arg2(aArg2)
{
	label = getCompatDialectVerb();
	label.upper();
}

DmlNode* ArithmeticNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	ArithmeticNode* node = FB_NEW_POOL(pool) ArithmeticNode(
		pool, blrOp, (csb->blrVersion == 4));
	node->arg1 = PAR_parse_value(tdbb, csb);
	node->arg2 = PAR_parse_value(tdbb, csb);
	return node;
}

string ArithmeticNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, blrOp);
	NODE_PRINT(printer, dialect1);
	NODE_PRINT(printer, label);
	NODE_PRINT(printer, arg1);
	NODE_PRINT(printer, arg2);

	return "ArithmeticNode";
}

void ArithmeticNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = label;
}

bool ArithmeticNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, arg1, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, arg2, desc, forceVarChar);
}

void ArithmeticNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blrOp);
	GEN_expr(dsqlScratch, arg1);
	GEN_expr(dsqlScratch, arg2);
}

namespace
{

const UCHAR DSC_ZTYPE_FLT64 = 0;
const UCHAR DSC_ZTYPE_FLT128 = 1;
const UCHAR DSC_ZTYPE_FIXED = 2;
const UCHAR DSC_ZTYPE_INT = 3;
const UCHAR DSC_ZTYPE_OTHER = 4;
const UCHAR DSC_ZTYPE_BAD = 5;

const UCHAR decimalDescTable[5][5] = {
/*							 DSC_ZTYPE_FLT64	DSC_ZTYPE_FLT128	DSC_ZTYPE_FIXED		DSC_ZTYPE_INT		DSC_ZTYPE_OTHER	*/
/*	DSC_ZTYPE_FLT64		*/	{DSC_ZTYPE_FLT64,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128},
/*	DSC_ZTYPE_FLT128	*/	{DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128},
/*	DSC_ZTYPE_FIXED		*/	{DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FIXED,	DSC_ZTYPE_FIXED,	DSC_ZTYPE_FLT128},
/*	DSC_ZTYPE_INT		*/	{DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FIXED,	DSC_ZTYPE_BAD,		DSC_ZTYPE_BAD},
/*	DSC_ZTYPE_OTHER		*/	{DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_FLT128,	DSC_ZTYPE_BAD,		DSC_ZTYPE_BAD}
};

UCHAR getFType(const dsc& desc)
{
	switch (desc.dsc_dtype)
	{
	case dtype_dec64:
		return DSC_ZTYPE_FLT64;
	case dtype_dec128:
		return DSC_ZTYPE_FLT128;
	case dtype_dec_fixed:
		return DSC_ZTYPE_FIXED;
	}

	if (DTYPE_IS_EXACT(desc.dsc_dtype))
		return DSC_ZTYPE_INT;

	return DSC_ZTYPE_OTHER;
}

enum Scaling { SCALE_MIN, SCALE_SUM };

unsigned setDecDesc(dsc* desc, const dsc& desc1, const dsc& desc2, Scaling sc, SCHAR* nodScale = nullptr)
{
	UCHAR zipType = decimalDescTable[getFType(desc1)][getFType(desc2)];
	fb_assert(zipType <= DSC_ZTYPE_FIXED);
	if (zipType > DSC_ZTYPE_FIXED)
		zipType = DSC_ZTYPE_FLT128;		// In production case fallback to Decimal128

	desc->dsc_dtype = zipType == DSC_ZTYPE_FLT64 ? dtype_dec64 :
		zipType == DSC_ZTYPE_FLT128 ? dtype_dec128 : dtype_dec_fixed;
	desc->dsc_sub_type = 0;
	desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;
	desc->dsc_scale = 0;

	if (zipType == DSC_ZTYPE_FIXED)
	{
		switch (sc)
		{
		case SCALE_MIN:
			desc->dsc_scale = MIN(NUMERIC_SCALE(desc1), NUMERIC_SCALE(desc2));
			break;
		case SCALE_SUM:
			desc->dsc_scale = NUMERIC_SCALE(desc1) + NUMERIC_SCALE(desc2);
			break;
		}
	}

	if (nodScale)
		*nodScale = desc->dsc_scale;

	desc->dsc_length = zipType == DSC_ZTYPE_FLT64 ? sizeof(Decimal64) :
		zipType == DSC_ZTYPE_FLT128 ? sizeof(Decimal128) : sizeof(DecimalFixed);

	return zipType == DSC_ZTYPE_FIXED ? ExprNode::FLAG_DECFIXED : ExprNode::FLAG_DECFLOAT;
}

} // anon namespace

void ArithmeticNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	dsc desc1, desc2;

	MAKE_desc(dsqlScratch, &desc1, arg1);
	MAKE_desc(dsqlScratch, &desc2, arg2);

	if (desc1.isNull())
	{
		desc1 = desc2;
		desc1.setNull();
	}

	if (desc2.isNull())
	{
		desc2 = desc1;
		desc2.setNull();
	}

	if (nodeIs<NullNode>(arg1) && nodeIs<NullNode>(arg2))
	{
		// NULL + NULL = NULL of INT
		desc->makeLong(0);
		desc->setNullable(true);
	}
	else if (dialect1)
		makeDialect1(desc, desc1, desc2);
	else
		makeDialect3(desc, desc1, desc2);
}

void ArithmeticNode::makeDialect1(dsc* desc, dsc& desc1, dsc& desc2)
{
	USHORT dtype, dtype1, dtype2;

	switch (blrOp)
	{
		case blr_add:
		case blr_subtract:
			dtype1 = desc1.dsc_dtype;
			if (dtype_int64 == dtype1 || DTYPE_IS_TEXT(dtype1))
				dtype1 = dtype_double;

			dtype2 = desc2.dsc_dtype;
			if (dtype_int64 == dtype2 || DTYPE_IS_TEXT(dtype2))
				dtype2 = dtype_double;

			dtype = MAX(dtype1, dtype2);

			if (DTYPE_IS_BLOB(dtype))
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
						  Arg::Gds(isc_dsql_no_blob_array));
			}

			desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;

			switch (dtype)
			{
				case dtype_sql_time:
				case dtype_sql_time_tz:
				case dtype_sql_date:
					// CVC: I don't see how this case can happen since dialect 1 doesn't accept
					// DATE or TIME
					// Forbid <date/time> +- <string>
					if (DTYPE_IS_TEXT(desc1.dsc_dtype) || DTYPE_IS_TEXT(desc2.dsc_dtype))
					{
						ERRD_post(Arg::Gds(isc_expression_eval_err) <<
									Arg::Gds(isc_dsql_nodateortime_pm_string));
					}
					// fall into

				case dtype_timestamp:
				case dtype_timestamp_tz:

					// Allow <timestamp> +- <string> (historical)
					if (couldBeDate(desc1) && couldBeDate(desc2))
					{
						if (blrOp == blr_subtract)
						{
							// <any date> - <any date>

							// Legal permutations are:
							// <timestamp> - <timestamp>
							// <timestamp> - <date>
							// <date> - <date>
							// <date> - <timestamp>
							// <time> - <time>
							// <timestamp> - <string>
							// <string> - <timestamp>
							// <string> - <string>

							if (DTYPE_IS_TEXT(desc1.dsc_dtype))
								dtype = dtype_timestamp;
							else if (DTYPE_IS_TEXT(desc2.dsc_dtype))
								dtype = dtype_timestamp;
							else if (desc1.dsc_dtype == desc2.dsc_dtype)
								dtype = desc1.dsc_dtype;
							else if (desc1.isTime() && dtype2 == dtype_sql_time)
								dtype = dtype1;
							else if (desc2.isTime() && dtype1 == dtype_sql_time)
								dtype = dtype2;
							else if (desc1.isTimeStamp() && dtype2 == dtype_timestamp)
								dtype = dtype1;
							else if (desc2.isTimeStamp() && dtype1 == dtype_timestamp)
								dtype = dtype2;
							else if (desc1.isTimeStamp() && desc2.dsc_dtype == dtype_sql_date)
								dtype = desc1.dsc_dtype;
							else if (desc2.isTimeStamp() && desc1.dsc_dtype == dtype_sql_date)
								dtype = desc2.dsc_dtype;
							else
							{
								ERRD_post(Arg::Gds(isc_expression_eval_err) <<
										  Arg::Gds(isc_dsql_invalid_datetime_subtract));
							}

							if (dtype == dtype_sql_date)
							{
								desc->dsc_dtype = dtype_long;
								desc->dsc_length = sizeof(SLONG);
								desc->dsc_scale = 0;
							}
							else if (dtype == dtype_sql_time || dtype == dtype_sql_time_tz)
							{
								desc->dsc_dtype = dtype_long;
								desc->dsc_length = sizeof(SLONG);
								desc->dsc_scale = ISC_TIME_SECONDS_PRECISION_SCALE;
								desc->dsc_sub_type = dsc_num_type_numeric;
							}
							else
							{
								fb_assert(dtype == dtype_timestamp || dtype == dtype_timestamp_tz);
								desc->dsc_dtype = dtype_double;
								desc->dsc_length = sizeof(double);
								desc->dsc_scale = 0;
							}
						}
						else if (isDateAndTime(desc1, desc2))
						{
							// <date> + <time>
							// <time> + <date>
							desc->dsc_dtype = desc1.isDateTimeTz() || desc2.isDateTimeTz() ?
								dtype_timestamp_tz : dtype_timestamp;
							desc->dsc_length = type_lengths[desc->dsc_dtype];
							desc->dsc_scale = 0;
						}
						else
						{
							// <date> + <date>
							// <time> + <time>
							// CVC: Hard to see it, since we are in dialect 1.
							ERRD_post(Arg::Gds(isc_expression_eval_err) <<
									  Arg::Gds(isc_dsql_invalid_dateortime_add));
						}
					}
					else if (DTYPE_IS_DATE(desc1.dsc_dtype) || blrOp == blr_add)
					{
						// <date> +/- <non-date>
						// <non-date> + <date>
						desc->dsc_dtype = desc1.dsc_dtype;
						if (!DTYPE_IS_DATE(desc->dsc_dtype))
							desc->dsc_dtype = desc2.dsc_dtype;
						fb_assert(DTYPE_IS_DATE(desc->dsc_dtype));
						desc->dsc_length = type_lengths[desc->dsc_dtype];
						desc->dsc_scale = 0;
					}
					else
					{
						// <non-date> - <date>
						fb_assert(blrOp == blr_subtract);
						ERRD_post(Arg::Gds(isc_expression_eval_err) <<
								  Arg::Gds(isc_dsql_invalid_type_minus_date));
					}
					break;

				case dtype_varying:
				case dtype_cstring:
				case dtype_text:
				case dtype_double:
				case dtype_real:
					desc->dsc_dtype = dtype_double;
					desc->dsc_sub_type = 0;
					desc->dsc_scale = 0;
					desc->dsc_length = sizeof(double);
					break;

				case dtype_dec64:
				case dtype_dec128:
				case dtype_dec_fixed:
					setDecDesc(desc, desc1, desc2, SCALE_MIN);
					break;

				default:
					desc->dsc_dtype = dtype_long;
					desc->dsc_sub_type = 0;
					desc->dsc_length = sizeof(SLONG);
					desc->dsc_scale = MIN(NUMERIC_SCALE(desc1), NUMERIC_SCALE(desc2));
					break;
			}

			break;

		case blr_multiply:
			// Arrays and blobs can never partipate in multiplication
			if (DTYPE_IS_BLOB(desc1.dsc_dtype) || DTYPE_IS_BLOB(desc2.dsc_dtype))
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
						  Arg::Gds(isc_dsql_no_blob_array));
			}

			dtype = DSC_multiply_blr4_result[desc1.dsc_dtype][desc2.dsc_dtype];
			desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;

			switch (dtype)
			{
				case dtype_dec128:
				case dtype_dec_fixed:
					setDecDesc(desc, desc1, desc2, SCALE_SUM);
					break;

				case dtype_double:
					desc->dsc_dtype = dtype_double;
					desc->dsc_sub_type = 0;
					desc->dsc_scale = 0;
					desc->dsc_length = sizeof(double);
					break;

				case dtype_long:
					desc->dsc_dtype = dtype_long;
					desc->dsc_sub_type = 0;
					desc->dsc_length = sizeof(SLONG);
					desc->dsc_scale = NUMERIC_SCALE(desc1) + NUMERIC_SCALE(desc2);
					break;

				default:
					ERRD_post(Arg::Gds(isc_expression_eval_err) <<
							  Arg::Gds(isc_dsql_invalid_type_multip_dial1));
			}

			break;

		case blr_divide:
			// Arrays and blobs can never partipate in division
			if (DTYPE_IS_BLOB(desc1.dsc_dtype) || DTYPE_IS_BLOB(desc2.dsc_dtype))
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
						  Arg::Gds(isc_dsql_no_blob_array));
			}

			dtype1 = desc1.dsc_dtype;
			if (dtype_int64 == dtype1 || DTYPE_IS_TEXT(dtype1))
				dtype1 = dtype_double;

			dtype2 = desc2.dsc_dtype;
			if (dtype_int64 == dtype2 || DTYPE_IS_TEXT(dtype2))
				dtype2 = dtype_double;

			dtype = MAX(dtype1, dtype2);

			if (DTYPE_IS_DECFLOAT(dtype))
			{
				setDecDesc(desc, desc1, desc2, SCALE_SUM);
				break;
			}

			if (!DTYPE_IS_NUMERIC(dtype))
			{
				ERRD_post(Arg::Gds(isc_expression_eval_err) <<
						  Arg::Gds(isc_dsql_mustuse_numeric_div_dial1));
			}

			desc->dsc_dtype = dtype_double;
			desc->dsc_length = sizeof(double);
			desc->dsc_scale = 0;
			desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;
			break;
	}
}

void ArithmeticNode::makeDialect3(dsc* desc, dsc& desc1, dsc& desc2)
{
	USHORT dtype, dtype1, dtype2;

	switch (blrOp)
	{
		case blr_add:
		case blr_subtract:
			dtype1 = desc1.dsc_dtype;
			dtype2 = desc2.dsc_dtype;

			// Arrays and blobs can never partipate in addition/subtraction
			if (DTYPE_IS_BLOB(dtype1) || DTYPE_IS_BLOB(dtype2))
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
						  Arg::Gds(isc_dsql_no_blob_array));
			}

			// In Dialect 2 or 3, strings can never partipate in addition / sub
			// (use a specific cast instead)
			if (DTYPE_IS_TEXT(dtype1) || DTYPE_IS_TEXT(dtype2))
			{
				ERRD_post(Arg::Gds(isc_expression_eval_err) <<
						  Arg::Gds(isc_dsql_nostring_addsub_dial3));
			}

			// Determine the TYPE of arithmetic to perform, store it
			// in dtype.  Note:  this is different from the result of
			// the operation, as <timestamp>-<timestamp> uses
			// <timestamp> arithmetic, but returns a <double>
			if (DTYPE_IS_EXACT(dtype1) && DTYPE_IS_EXACT(dtype2))
			{
				if (desc1.isDecFixed() || desc2.isDecFixed())
					dtype = dtype_dec_fixed;
				else
					dtype = dtype_int64;
			}
			else if (desc1.isDecOrInt() && desc2.isDecOrInt())
				dtype = dtype_dec128;
			else if (DTYPE_IS_NUMERIC(dtype1) && DTYPE_IS_NUMERIC(dtype2))
			{
				fb_assert(DTYPE_IS_APPROX(dtype1) || DTYPE_IS_APPROX(dtype2));
				dtype = dtype_double;
			}
			else
			{
				// mixed numeric and non-numeric:

				// The MAX(dtype) rule doesn't apply with dtype_int64

				if (dtype1 == dtype_int64)
					dtype1 = dtype_double;
				if (dtype2 == dtype_int64)
					dtype2 = dtype_double;

				dtype = CVT2_compare_priority[dtype1] > CVT2_compare_priority[dtype2] ? dtype1 : dtype2;
			}

			desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;

			switch (dtype)
			{
				case dtype_sql_time:
				case dtype_sql_time_tz:
				case dtype_sql_date:
				case dtype_timestamp:
				case dtype_timestamp_tz:
					if ((DTYPE_IS_DATE(dtype1) || dtype1 == dtype_unknown) &&
						(DTYPE_IS_DATE(dtype2) || dtype2 == dtype_unknown))
					{
						if (blrOp == blr_subtract)
						{
							// <any date> - <any date>
							// Legal permutations are:
							// <timestamp> - <timestamp>
							// <timestamp> - <date>
							// <date> - <date>
							// <date> - <timestamp>
							// <time> - <time>

							if (dtype1 == dtype2)
								dtype = dtype1;
							else if (desc1.isTime() && dtype2 == dtype_sql_time)
								dtype = dtype1;
							else if (desc2.isTime() && dtype1 == dtype_sql_time)
								dtype = dtype2;
							else if (desc1.isTimeStamp() && dtype2 == dtype_timestamp)
								dtype = dtype1;
							else if (desc2.isTimeStamp() && dtype1 == dtype_timestamp)
								dtype = dtype2;
							else if (desc1.isTimeStamp() && dtype2 == dtype_sql_date)
								dtype = dtype1;
							else if (desc2.isTimeStamp() && dtype1 == dtype_sql_date)
								dtype = dtype2;
							else
							{
								ERRD_post(Arg::Gds(isc_expression_eval_err) <<
										  Arg::Gds(isc_dsql_invalid_datetime_subtract));
							}

							if (dtype == dtype_sql_date)
							{
								desc->dsc_dtype = dtype_long;
								desc->dsc_length = sizeof(SLONG);
								desc->dsc_scale = 0;
							}
							else if (dtype == dtype_sql_time || dtype == dtype_sql_time_tz)
							{
								desc->dsc_dtype = dtype_long;
								desc->dsc_length = sizeof(SLONG);
								desc->dsc_scale = ISC_TIME_SECONDS_PRECISION_SCALE;
								desc->dsc_sub_type = dsc_num_type_numeric;
							}
							else
							{
								fb_assert(dtype == dtype_timestamp || dtype == dtype_timestamp_tz);
								desc->dsc_dtype = dtype_int64;
								desc->dsc_length = sizeof(SINT64);
								desc->dsc_scale = -9;
								desc->dsc_sub_type = dsc_num_type_numeric;
							}
						}
						else if (isDateAndTime(desc1, desc2))
						{
							// <date> + <time>
							// <time> + <date>
							desc->dsc_dtype = desc1.isDateTimeTz() || desc2.isDateTimeTz() ?
								dtype_timestamp_tz : dtype_timestamp;
							desc->dsc_length = type_lengths[desc->dsc_dtype];
							desc->dsc_scale = 0;
						}
						else
						{
							// <date> + <date>
							// <time> + <time>
							ERRD_post(Arg::Gds(isc_expression_eval_err) <<
									  Arg::Gds(isc_dsql_invalid_dateortime_add));
						}
					}
					else if (DTYPE_IS_DATE(desc1.dsc_dtype) || blrOp == blr_add)
					{
						// <date> +/- <non-date>
						// <non-date> + <date>
						desc->dsc_dtype = desc1.dsc_dtype;
						if (!DTYPE_IS_DATE(desc->dsc_dtype))
							desc->dsc_dtype = desc2.dsc_dtype;
						fb_assert(DTYPE_IS_DATE(desc->dsc_dtype));
						desc->dsc_length = type_lengths[desc->dsc_dtype];
						desc->dsc_scale = 0;
					}
					else
					{
						// <non-date> - <date>
						fb_assert(blrOp == blr_subtract);
						ERRD_post(Arg::Gds(isc_expression_eval_err) <<
								  Arg::Gds(isc_dsql_invalid_type_minus_date));
					}
					break;

				case dtype_varying:
				case dtype_cstring:
				case dtype_text:
				case dtype_double:
				case dtype_real:
					desc->dsc_dtype = dtype_double;
					desc->dsc_sub_type = 0;
					desc->dsc_scale = 0;
					desc->dsc_length = sizeof(double);
					break;

				case dtype_dec64:
				case dtype_dec128:
				case dtype_dec_fixed:
					setDecDesc(desc, desc1, desc2, SCALE_MIN);
					break;

				case dtype_short:
				case dtype_long:
				case dtype_int64:
					desc->dsc_dtype = dtype_int64;
					desc->dsc_sub_type = 0;
					desc->dsc_length = sizeof(SINT64);

					// The result type is int64 because both operands are
					// exact numeric: hence we don't need the NUMERIC_SCALE
					// macro here.
					fb_assert(desc1.dsc_dtype == dtype_unknown || DTYPE_IS_EXACT(desc1.dsc_dtype));
					fb_assert(desc2.dsc_dtype == dtype_unknown || DTYPE_IS_EXACT(desc2.dsc_dtype));

					desc->dsc_scale = MIN(desc1.dsc_scale, desc2.dsc_scale);
					break;

				default:
					// a type which cannot participate in an add or subtract
					ERRD_post(Arg::Gds(isc_expression_eval_err) <<
							  Arg::Gds(isc_dsql_invalid_type_addsub_dial3));
			}

			break;

		case blr_multiply:
			// In Dialect 2 or 3, strings can never partipate in multiplication
			// (use a specific cast instead)
			if (DTYPE_IS_TEXT(desc1.dsc_dtype) || DTYPE_IS_TEXT(desc2.dsc_dtype))
			{
				ERRD_post(Arg::Gds(isc_expression_eval_err) <<
						  Arg::Gds(isc_dsql_nostring_multip_dial3));
			}

			// Arrays and blobs can never partipate in multiplication
			if (DTYPE_IS_BLOB(desc1.dsc_dtype) || DTYPE_IS_BLOB(desc2.dsc_dtype))
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
						  Arg::Gds(isc_dsql_no_blob_array));
			}

			dtype = DSC_multiply_result[desc1.dsc_dtype][desc2.dsc_dtype];
			desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;

			switch (dtype)
			{
				case dtype_dec_fixed:
				case dtype_dec128:
					setDecDesc(desc, desc1, desc2, SCALE_SUM);
					break;

				case dtype_double:
					desc->dsc_dtype = dtype_double;
					desc->dsc_sub_type = 0;
					desc->dsc_scale = 0;
					desc->dsc_length = sizeof(double);
					break;

				case dtype_int64:
					desc->dsc_dtype = dtype_int64;
					desc->dsc_sub_type = 0;
					desc->dsc_length = sizeof(SINT64);
					desc->dsc_scale = NUMERIC_SCALE(desc1) + NUMERIC_SCALE(desc2);
					break;

				default:
					ERRD_post(Arg::Gds(isc_expression_eval_err) <<
							  Arg::Gds(isc_dsql_invalid_type_multip_dial3));
			}

			break;

		case blr_divide:
			// In Dialect 2 or 3, strings can never partipate in division
			// (use a specific cast instead)
			if (DTYPE_IS_TEXT(desc1.dsc_dtype) || DTYPE_IS_TEXT(desc2.dsc_dtype))
			{
				ERRD_post(Arg::Gds(isc_expression_eval_err) <<
						  Arg::Gds(isc_dsql_nostring_div_dial3));
			}

			// Arrays and blobs can never partipate in division
			if (DTYPE_IS_BLOB(desc1.dsc_dtype) || DTYPE_IS_BLOB(desc2.dsc_dtype))
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
						  Arg::Gds(isc_dsql_no_blob_array));
			}

			dtype = DSC_multiply_result[desc1.dsc_dtype][desc2.dsc_dtype];
			desc->dsc_dtype = static_cast<UCHAR>(dtype);
			desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;

			switch (dtype)
			{
				case dtype_int64:
					desc->dsc_length = sizeof(SINT64);
					desc->dsc_scale = NUMERIC_SCALE(desc1) + NUMERIC_SCALE(desc2);
					break;

				case dtype_double:
					desc->dsc_length = sizeof(double);
					desc->dsc_scale = 0;
					break;

				case dtype_dec128:
				case dtype_dec_fixed:
					setDecDesc(desc, desc1, desc2, SCALE_SUM);
					break;

				default:
					ERRD_post(Arg::Gds(isc_expression_eval_err) <<
							  Arg::Gds(isc_dsql_invalid_type_div_dial3));
			}

			break;
	}
}

void ArithmeticNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	dsc desc1, desc2;

	arg1->getDesc(tdbb, csb, &desc1);
	arg2->getDesc(tdbb, csb, &desc2);

	if (desc1.isNull())
	{
		desc1 = desc2;
		desc1.setNull();
	}

	if (desc2.isNull())
	{
		desc2 = desc1;
		desc2.setNull();
	}

	if (dialect1)
		getDescDialect1(tdbb, desc, desc1, desc2);
	else
		getDescDialect3(tdbb, desc, desc1, desc2);
}

void ArithmeticNode::getDescDialect1(thread_db* /*tdbb*/, dsc* desc, dsc& desc1, dsc& desc2)
{
	USHORT dtype = 0;

	switch (blrOp)
	{
		case blr_add:
		case blr_subtract:
		{
			/* 92/05/29 DAVES - don't understand why this is done for ONLY
				dtype_text (eg: not dtype_cstring or dtype_varying) Doesn't
				appear to hurt.

				94/04/04 DAVES - NOW I understand it!  QLI will pass floating
				point values to the engine as text.  All other numeric constants
				it turns into either integers or longs (with scale). */

			USHORT dtype1 = desc1.dsc_dtype;
			if (dtype1 == dtype_int64)
				dtype1 = dtype_double;

			USHORT dtype2 = desc2.dsc_dtype;
			if (dtype2 == dtype_int64)
				dtype2 = dtype_double;

			if (dtype1 == dtype_text || dtype2 == dtype_text)
				dtype = MAX(MAX(dtype1, dtype2), (UCHAR) DEFAULT_DOUBLE);
			else
				dtype = MAX(dtype1, dtype2);

			switch (dtype)
			{
				case dtype_short:
					desc->dsc_dtype = dtype_long;
					desc->dsc_length = sizeof(SLONG);
					if (DTYPE_IS_TEXT(desc1.dsc_dtype) || DTYPE_IS_TEXT(desc2.dsc_dtype))
						desc->dsc_scale = 0;
					else
						desc->dsc_scale = MIN(desc1.dsc_scale, desc2.dsc_scale);

					nodScale = desc->dsc_scale;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				case dtype_sql_date:
				case dtype_sql_time:
				case dtype_sql_time_tz:
					if (DTYPE_IS_TEXT(desc1.dsc_dtype) || DTYPE_IS_TEXT(desc2.dsc_dtype))
						ERR_post(Arg::Gds(isc_expression_eval_err));
					// fall into

				case dtype_timestamp:
				case dtype_timestamp_tz:
					nodFlags |= FLAG_DATE;

					fb_assert(DTYPE_IS_DATE(desc1.dsc_dtype) || DTYPE_IS_DATE(desc2.dsc_dtype));

					if (couldBeDate(desc1) && couldBeDate(desc2))
					{
						if (blrOp == blr_subtract)
						{
							// <any date> - <any date>

							/* Legal permutations are:
								<timestamp> - <timestamp>
								<timestamp> - <date>
								<date> - <date>
								<date> - <timestamp>
								<time> - <time>
								<timestamp> - <string>
								<string> - <timestamp>
								<string> - <string>   */

							if (DTYPE_IS_TEXT(dtype1))
								dtype = dtype_timestamp;
							else if (DTYPE_IS_TEXT(dtype2))
								dtype = dtype_timestamp;
							else if (dtype1 == dtype2)
								dtype = dtype1;
							else if (desc1.isTime() && dtype2 == dtype_sql_time)
								dtype = dtype1;
							else if (desc2.isTime() && dtype1 == dtype_sql_time)
								dtype = dtype2;
							else if (desc1.isTimeStamp() && dtype2 == dtype_timestamp)
								dtype = dtype1;
							else if (desc2.isTimeStamp() && dtype1 == dtype_timestamp)
								dtype = dtype2;
							else if (desc1.isTimeStamp() && dtype2 == dtype_sql_date)
								dtype = dtype1;
							else if (desc2.isTimeStamp() && dtype1 == dtype_sql_date)
								dtype = dtype2;
							else
								ERR_post(Arg::Gds(isc_expression_eval_err));

							if (dtype == dtype_sql_date)
							{
								desc->dsc_dtype = dtype_long;
								desc->dsc_length = type_lengths[desc->dsc_dtype];
								desc->dsc_scale = 0;
								desc->dsc_sub_type = 0;
								desc->dsc_flags = 0;
							}
							else if (dtype == dtype_sql_time || dtype == dtype_sql_time_tz)
							{
								desc->dsc_dtype = dtype_long;
								desc->dsc_length = type_lengths[desc->dsc_dtype];
								desc->dsc_scale = ISC_TIME_SECONDS_PRECISION_SCALE;
								desc->dsc_sub_type = 0;
								desc->dsc_flags = 0;
							}
							else
							{
								fb_assert(dtype == dtype_timestamp || dtype == dtype_timestamp_tz);
								desc->dsc_dtype = DEFAULT_DOUBLE;
								desc->dsc_length = type_lengths[desc->dsc_dtype];
								desc->dsc_scale = 0;
								desc->dsc_sub_type = 0;
								desc->dsc_flags = 0;
							}
						}
						else if (isDateAndTime(desc1, desc2))
						{
							// <date> + <time>
							// <time> + <date>
							desc->dsc_dtype = desc1.isDateTimeTz() || desc2.isDateTimeTz() ?
								dtype_timestamp_tz : dtype_timestamp;
							desc->dsc_length = type_lengths[desc->dsc_dtype];
							desc->dsc_scale = 0;
							desc->dsc_sub_type = 0;
							desc->dsc_flags = 0;
						}
						else
						{
							// <date> + <date>
							ERR_post(Arg::Gds(isc_expression_eval_err));
						}
					}
					else if (DTYPE_IS_DATE(desc1.dsc_dtype) || blrOp == blr_add)
					{
						// <date> +/- <non-date> || <non-date> + <date>
						desc->dsc_dtype = desc1.dsc_dtype;
						if (!DTYPE_IS_DATE(desc->dsc_dtype))
							desc->dsc_dtype = desc2.dsc_dtype;

						fb_assert(DTYPE_IS_DATE(desc->dsc_dtype));
						desc->dsc_length = type_lengths[desc->dsc_dtype];
						desc->dsc_scale = 0;
						desc->dsc_sub_type = 0;
						desc->dsc_flags = 0;
					}
					else
					{
						// <non-date> - <date>
						ERR_post(Arg::Gds(isc_expression_eval_err));
					}
					return;

				case dtype_text:
				case dtype_cstring:
				case dtype_varying:
				case dtype_long:
				case dtype_real:
				case dtype_double:
					nodFlags |= FLAG_DOUBLE;
					desc->dsc_dtype = DEFAULT_DOUBLE;
					desc->dsc_length = sizeof(double);
					desc->dsc_scale = 0;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				case dtype_dec64:
				case dtype_dec128:
				case dtype_dec_fixed:
					nodFlags |= setDecDesc(desc, desc1, desc2, SCALE_MIN, &nodScale);
					break;

				case dtype_unknown:
					desc->dsc_dtype = dtype_unknown;
					desc->dsc_length = 0;
					desc->dsc_scale = 0;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				case dtype_quad:
				case dtype_blob:
				case dtype_array:
					break;

				default:
					fb_assert(false);
			}

			break;
		}

		case blr_multiply:
			dtype = DSC_multiply_blr4_result[desc1.dsc_dtype][desc2.dsc_dtype];

			switch (dtype)
			{
				case dtype_long:
					desc->dsc_dtype = dtype_long;
					desc->dsc_length = sizeof(SLONG);
					desc->dsc_scale = nodScale = NUMERIC_SCALE(desc1) + NUMERIC_SCALE(desc2);
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				case dtype_double:
					nodFlags |= FLAG_DOUBLE;
					desc->dsc_dtype = DEFAULT_DOUBLE;
					desc->dsc_length = sizeof(double);
					desc->dsc_scale = 0;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				case dtype_dec128:
				case dtype_dec_fixed:
					nodFlags |= setDecDesc(desc, desc1, desc2, SCALE_SUM, &nodScale);
					break;

				case dtype_unknown:
					desc->dsc_dtype = dtype_unknown;
					desc->dsc_length = 0;
					desc->dsc_scale = 0;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				default:
					fb_assert(false);
					// FALLINTO

				case DTYPE_CANNOT:
					// break to error reporting code
					break;
			}

			break;

		case blr_divide:
			// for compatibility with older versions of the product, we accept
			// text types for division in blr_version4 (dialect <= 1) only
			if (!(DTYPE_IS_NUMERIC(desc1.dsc_dtype) || DTYPE_IS_TEXT(desc1.dsc_dtype)))
			{
				if (desc1.dsc_dtype != dtype_unknown)
					break;	// error, dtype not supported by arithmetic
			}

			if (!(DTYPE_IS_NUMERIC(desc2.dsc_dtype) || DTYPE_IS_TEXT(desc2.dsc_dtype)))
			{
				if (desc2.dsc_dtype != dtype_unknown)
					break;	// error, dtype not supported by arithmetic
			}

			desc->dsc_dtype = DEFAULT_DOUBLE;
			desc->dsc_length = sizeof(double);
			desc->dsc_scale = 0;
			desc->dsc_sub_type = 0;
			desc->dsc_flags = 0;
			return;
	}

	if (dtype == dtype_quad)
		IBERROR(224);	// msg 224 quad word arithmetic not supported

	ERR_post(Arg::Gds(isc_datype_notsup));	// data type not supported for arithmetic
}

void ArithmeticNode::getDescDialect3(thread_db* /*tdbb*/, dsc* desc, dsc& desc1, dsc& desc2)
{
	USHORT dtype;

	switch (blrOp)
	{
		case blr_add:
		case blr_subtract:
		{
			USHORT dtype1 = desc1.dsc_dtype;
			USHORT dtype2 = desc2.dsc_dtype;

			// In Dialect 2 or 3, strings can never participate in addition / sub
			// (use a specific cast instead)
			if (DTYPE_IS_TEXT(dtype1) || DTYPE_IS_TEXT(dtype2))
				ERR_post(Arg::Gds(isc_expression_eval_err));

			// Because dtype_int64 > dtype_double, we cannot just use the MAX macro to set
			// the result dtype. The rule is that two exact numeric operands yield an int64
			// result, while an approximate numeric and anything yield a double result.

			if (DTYPE_IS_EXACT(dtype1) && DTYPE_IS_EXACT(dtype2))
			{
				if (desc1.isDecFixed() || desc2.isDecFixed())
					dtype = dtype_dec_fixed;
				else
					dtype = dtype_int64;
			}
			else if (desc1.isDecOrInt() && desc2.isDecOrInt())
				dtype = dtype_dec128;
			else if (DTYPE_IS_NUMERIC(desc1.dsc_dtype) && DTYPE_IS_NUMERIC(desc2.dsc_dtype))
				dtype = dtype_double;
			else
			{
				// mixed numeric and non-numeric:

				fb_assert(couldBeDate(desc1) || couldBeDate(desc2));

				// the MAX(dtype) rule doesn't apply with dtype_int64

				if (dtype_int64 == dtype1)
					dtype1 = dtype_double;

				if (dtype_int64 == dtype2)
					dtype2 = dtype_double;

				dtype = MAX(dtype1, dtype2);
			}

			switch (dtype)
			{
				case dtype_timestamp:
				case dtype_timestamp_tz:
				case dtype_sql_date:
				case dtype_sql_time:
				case dtype_sql_time_tz:
					nodFlags |= FLAG_DATE;

					fb_assert(DTYPE_IS_DATE(desc1.dsc_dtype) || DTYPE_IS_DATE(desc2.dsc_dtype));

					if ((DTYPE_IS_DATE(dtype1) || dtype1 == dtype_unknown) &&
						(DTYPE_IS_DATE(dtype2) || dtype2 == dtype_unknown))
					{
						if (blrOp == blr_subtract)
						{
							// <any date> - <any date>

							/* Legal permutations are:
							   <timestamp> - <timestamp>
							   <timestamp> - <date>
							   <date> - <date>
							   <date> - <timestamp>
							   <time> - <time> */

							if (dtype1 == dtype_unknown)
								dtype1 = dtype2;
							else if (dtype2 == dtype_unknown)
								dtype2 = dtype1;

							if (dtype1 == dtype2)
								dtype = dtype1;
							else if (desc1.isTime() && dtype2 == dtype_sql_time)
								dtype = dtype1;
							else if (desc2.isTime() && dtype1 == dtype_sql_time)
								dtype = dtype2;
							else if (desc1.isTimeStamp() && dtype2 == dtype_timestamp)
								dtype = dtype1;
							else if (desc2.isTimeStamp() && dtype1 == dtype_timestamp)
								dtype = dtype2;
							else if (desc1.isTimeStamp() && dtype2 == dtype_sql_date)
								dtype = dtype1;
							else if (desc2.isTimeStamp() && dtype1 == dtype_sql_date)
								dtype = dtype2;
							else
								ERR_post(Arg::Gds(isc_expression_eval_err));

							if (dtype == dtype_sql_date)
							{
								desc->dsc_dtype = dtype_long;
								desc->dsc_length = type_lengths[desc->dsc_dtype];
								desc->dsc_scale = 0;
								desc->dsc_sub_type = 0;
								desc->dsc_flags = 0;
							}
							else if (dtype == dtype_sql_time || dtype == dtype_sql_time_tz)
							{
								desc->dsc_dtype = dtype_long;
								desc->dsc_length = type_lengths[desc->dsc_dtype];
								desc->dsc_scale = ISC_TIME_SECONDS_PRECISION_SCALE;
								desc->dsc_sub_type = 0;
								desc->dsc_flags = 0;
							}
							else
							{
								fb_assert(dtype == dtype_timestamp || dtype == dtype_timestamp_tz ||
									dtype == dtype_unknown);
								desc->dsc_dtype = DEFAULT_DOUBLE;
								desc->dsc_length = type_lengths[desc->dsc_dtype];
								desc->dsc_scale = 0;
								desc->dsc_sub_type = 0;
								desc->dsc_flags = 0;
							}
						}
						else if (isDateAndTime(desc1, desc2))
						{
							// <date> + <time>
							// <time> + <date>
							desc->dsc_dtype = desc1.isDateTimeTz() || desc2.isDateTimeTz() ?
								dtype_timestamp_tz : dtype_timestamp;
							desc->dsc_length = type_lengths[desc->dsc_dtype];
							desc->dsc_scale = 0;
							desc->dsc_sub_type = 0;
							desc->dsc_flags = 0;
						}
						else
						{
							// <date> + <date>
							ERR_post(Arg::Gds(isc_expression_eval_err));
						}
					}
					else if (DTYPE_IS_DATE(desc1.dsc_dtype) || blrOp == blr_add)
					{
						// <date> +/- <non-date> || <non-date> + <date>
						desc->dsc_dtype = desc1.dsc_dtype;
						if (!DTYPE_IS_DATE(desc->dsc_dtype))
							desc->dsc_dtype = desc2.dsc_dtype;
						fb_assert(DTYPE_IS_DATE(desc->dsc_dtype));
						desc->dsc_length = type_lengths[desc->dsc_dtype];
						desc->dsc_scale = 0;
						desc->dsc_sub_type = 0;
						desc->dsc_flags = 0;
					}
					else
					{
						// <non-date> - <date>
						ERR_post(Arg::Gds(isc_expression_eval_err));
					}
					return;

				case dtype_text:
				case dtype_cstring:
				case dtype_varying:
				case dtype_real:
				case dtype_double:
					nodFlags |= FLAG_DOUBLE;
					desc->dsc_dtype = DEFAULT_DOUBLE;
					desc->dsc_length = sizeof(double);
					desc->dsc_scale = 0;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				case dtype_dec64:
				case dtype_dec128:
				case dtype_dec_fixed:
					nodFlags |= setDecDesc(desc, desc1, desc2, SCALE_MIN, &nodScale);
					return;

				case dtype_short:
				case dtype_long:
				case dtype_int64:
					desc->dsc_dtype = dtype_int64;
					desc->dsc_length = sizeof(SINT64);
					if (DTYPE_IS_TEXT(desc1.dsc_dtype) || DTYPE_IS_TEXT(desc2.dsc_dtype))
						desc->dsc_scale = 0;
					else
						desc->dsc_scale = MIN(desc1.dsc_scale, desc2.dsc_scale);
					nodScale = desc->dsc_scale;
					desc->dsc_sub_type = MAX(desc1.dsc_sub_type, desc2.dsc_sub_type);
					desc->dsc_flags = 0;
					return;

				case dtype_unknown:
					desc->dsc_dtype = dtype_unknown;
					desc->dsc_length = 0;
					desc->dsc_scale = 0;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				case dtype_quad:
				case dtype_blob:
				case dtype_array:
					break;

				default:
					fb_assert(false);
			}

			break;
		}

		case blr_multiply:
		case blr_divide:
			dtype = DSC_multiply_result[desc1.dsc_dtype][desc2.dsc_dtype];

			switch (dtype)
			{
				case dtype_double:
					nodFlags |= FLAG_DOUBLE;
					desc->dsc_dtype = DEFAULT_DOUBLE;
					desc->dsc_length = sizeof(double);
					desc->dsc_scale = 0;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				case dtype_dec128:
				case dtype_dec_fixed:
					nodFlags |= setDecDesc(desc, desc1, desc2, SCALE_SUM, &nodScale);
					return;

				case dtype_int64:
					desc->dsc_dtype = dtype_int64;
					desc->dsc_length = sizeof(SINT64);
					desc->dsc_scale = nodScale = NUMERIC_SCALE(desc1) + NUMERIC_SCALE(desc2);
					desc->dsc_sub_type = MAX(desc1.dsc_sub_type, desc2.dsc_sub_type);
					desc->dsc_flags = 0;
					return;

				case dtype_unknown:
					desc->dsc_dtype = dtype_unknown;
					desc->dsc_length = 0;
					desc->dsc_scale = 0;
					desc->dsc_sub_type = 0;
					desc->dsc_flags = 0;
					return;

				default:
					fb_assert(false);
					// FALLINTO

				case DTYPE_CANNOT:
					// break to error reporting code
					break;
			}

			break;
	}

	if (dtype == dtype_quad)
		IBERROR(224);	// msg 224 quad word arithmetic not supported

	ERR_post(Arg::Gds(isc_datype_notsup));	// data type not supported for arithmetic
}

ValueExprNode* ArithmeticNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	ArithmeticNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) ArithmeticNode(*tdbb->getDefaultPool(),
		blrOp, dialect1);
	node->nodScale = nodScale;
	node->arg1 = copier.copy(tdbb, arg1);
	node->arg2 = copier.copy(tdbb, arg2);
	return node;
}

bool ArithmeticNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const ArithmeticNode* o = nodeAs<ArithmeticNode>(other);
	fb_assert(o);

	return dialect1 == o->dialect1 && blrOp == o->blrOp;
}

bool ArithmeticNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	const ArithmeticNode* const otherNode = nodeAs<ArithmeticNode>(other);

	if (!otherNode || blrOp != otherNode->blrOp || dialect1 != otherNode->dialect1)
		return false;

	if (arg1->sameAs(csb, otherNode->arg1, ignoreStreams) &&
		arg2->sameAs(csb, otherNode->arg2, ignoreStreams))
	{
		return true;
	}

	if (blrOp == blr_add || blrOp == blr_multiply)
	{
		// A + B is equivalent to B + A, ditto for A * B and B * A.
		// Note: If one expression is A + B + C, but the other is B + C + A we won't
		// necessarily match them.
		if (arg1->sameAs(csb, otherNode->arg2, ignoreStreams) &&
			arg2->sameAs(csb, otherNode->arg1, ignoreStreams))
		{
			return true;
		}
	}

	return false;
}

ValueExprNode* ArithmeticNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* ArithmeticNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);

	request->req_flags &= ~req_null;

	// Evaluate arguments.  If either is null, result is null, but in
	// any case, evaluate both, since some expressions may later depend
	// on mappings which are developed here

	const dsc* desc1 = EVL_expr(tdbb, request, arg1);
	const ULONG flags = request->req_flags;
	request->req_flags &= ~req_null;

	const dsc* desc2 = EVL_expr(tdbb, request, arg2);

	// restore saved NULL state

	if (flags & req_null)
		request->req_flags |= req_null;

	if (request->req_flags & req_null)
		return NULL;

	EVL_make_value(tdbb, desc1, impure);

	if (dialect1)	// dialect-1 semantics
	{
		switch (blrOp)
		{
			case blr_add:
			case blr_subtract:
				return add(tdbb, desc2, impure, this, blrOp);

			case blr_divide:
			{
				const double divisor = MOV_get_double(tdbb, desc2);

				if (divisor == 0)
				{
					ERR_post(Arg::Gds(isc_arith_except) <<
							 Arg::Gds(isc_exception_float_divide_by_zero));
				}

				impure->vlu_misc.vlu_double = MOV_get_double(tdbb, desc1) / divisor;

				if (isinf(impure->vlu_misc.vlu_double))
				{
					ERR_post(Arg::Gds(isc_arith_except) <<
							 Arg::Gds(isc_exception_float_overflow));
				}

				impure->vlu_desc.dsc_dtype = DEFAULT_DOUBLE;
				impure->vlu_desc.dsc_length = sizeof(double);
				impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc;

				return &impure->vlu_desc;
			}

			case blr_multiply:
				return multiply(desc2, impure);
		}
	}
	else	// with dialect-3 semantics
	{
		switch (blrOp)
		{
			case blr_add:
			case blr_subtract:
				return add2(tdbb, desc2, impure, this, blrOp);

			case blr_multiply:
				return multiply2(desc2, impure);

			case blr_divide:
				return divide2(desc2, impure);
		}
	}

	SOFT_BUGCHECK(232);	// msg 232 EVL_expr: invalid operation
	return NULL;
}

// Add (or subtract) the contents of a descriptor to value block, with dialect-1 semantics.
// This function can be removed when dialect-3 becomes the lowest supported dialect. (Version 7.0?)
dsc* ArithmeticNode::add(thread_db* tdbb, const dsc* desc, impure_value* value, const ValueExprNode* node,
	const UCHAR blrOp)
{
	const ArithmeticNode* arithmeticNode = nodeAs<ArithmeticNode>(node);

#ifdef DEV_BUILD
	const SubQueryNode* subQueryNode = nodeAs<SubQueryNode>(node);
	fb_assert(
		(arithmeticNode && arithmeticNode->dialect1 &&
			(arithmeticNode->blrOp == blr_add || arithmeticNode->blrOp == blr_subtract)) ||
		nodeIs<AggNode>(node) ||
		(subQueryNode && (subQueryNode->blrOp == blr_total || subQueryNode->blrOp == blr_average)));
#endif

	dsc* const result = &value->vlu_desc;

	// Handle date arithmetic

	if (node->nodFlags & FLAG_DATE)
	{
		fb_assert(arithmeticNode);
		return arithmeticNode->addDateTime(tdbb, desc, value);
	}

	// Handle decimal arithmetic

	if (node->nodFlags & FLAG_DECFLOAT)
	{
		const Decimal128 d1 = MOV_get_dec128(tdbb, desc);
		const Decimal128 d2 = MOV_get_dec128(tdbb, &value->vlu_desc);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec128 = (blrOp == blr_subtract) ? d2.sub(decSt, d1) : d1.add(decSt, d2);

		result->dsc_dtype = dtype_dec128;
		result->dsc_length = sizeof(Decimal128);
		result->dsc_scale = 0;
		result->dsc_sub_type = 0;
		result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec128;

		return result;
	}

	if (node->nodFlags & FLAG_DECFIXED)
	{
		const DecimalFixed d1 = MOV_get_dec_fixed(tdbb, desc, node->nodScale);
		const DecimalFixed d2 = MOV_get_dec_fixed(tdbb, &value->vlu_desc, node->nodScale);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec_fixed = (blrOp == blr_subtract) ? d2.sub(decSt, d1) : d1.add(decSt, d2);

		result->dsc_dtype = dtype_dec_fixed;
		result->dsc_length = sizeof(DecimalFixed);
		result->dsc_scale = node->nodScale;
		result->dsc_sub_type = 0;
		result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec_fixed;

		return result;
	}

	// Handle floating arithmetic

	if (node->nodFlags & FLAG_DOUBLE)
	{
		const double d1 = MOV_get_double(tdbb, desc);
		const double d2 = MOV_get_double(tdbb, &value->vlu_desc);

		value->vlu_misc.vlu_double = (blrOp == blr_subtract) ? d2 - d1 : d1 + d2;

		if (isinf(value->vlu_misc.vlu_double))
			ERR_post(Arg::Gds(isc_arith_except) << Arg::Gds(isc_exception_float_overflow));

		result->dsc_dtype = DEFAULT_DOUBLE;
		result->dsc_length = sizeof(double);
		result->dsc_scale = 0;
		result->dsc_sub_type = 0;
		result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_double;

		return result;
	}

	// Everything else defaults to longword

	// CVC: Maybe we should upgrade the sum to double if it doesn't fit?
	// This is what was done for multiplicaton in dialect 1.

	const SLONG l1 = MOV_get_long(tdbb, desc, node->nodScale);
	const SINT64 l2 = MOV_get_long(tdbb, &value->vlu_desc, node->nodScale);
	const SINT64 rc = (blrOp == blr_subtract) ? l2 - l1 : l2 + l1;

	if (rc < MIN_SLONG || rc > MAX_SLONG)
		ERR_post(Arg::Gds(isc_exception_integer_overflow));

	value->make_long(rc, node->nodScale);

	return result;
}

// Add (or subtract) the contents of a descriptor to value block, with dialect-3 semantics, as in
// the blr_add, blr_subtract, and blr_agg_total verbs following a blr_version5.
dsc* ArithmeticNode::add2(thread_db* tdbb, const dsc* desc, impure_value* value, const ValueExprNode* node,
	const UCHAR blrOp)
{
	const ArithmeticNode* arithmeticNode = nodeAs<ArithmeticNode>(node);

	fb_assert(
		(arithmeticNode && !arithmeticNode->dialect1 &&
			(arithmeticNode->blrOp == blr_add || arithmeticNode->blrOp == blr_subtract)) ||
		nodeIs<AggNode>(node));

	dsc* result = &value->vlu_desc;

	// Handle date arithmetic

	if (node->nodFlags & FLAG_DATE)
	{
		fb_assert(arithmeticNode);
		return arithmeticNode->addDateTime(tdbb, desc, value);
	}

	// Handle decimal arithmetic

	if (node->nodFlags & FLAG_DECFLOAT)
	{
		const Decimal128 d1 = MOV_get_dec128(tdbb, desc);
		const Decimal128 d2 = MOV_get_dec128(tdbb, &value->vlu_desc);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec128 = (blrOp == blr_subtract) ? d2.sub(decSt, d1) : d1.add(decSt, d2);

		result->dsc_dtype = dtype_dec128;
		result->dsc_length = sizeof(Decimal128);
		result->dsc_scale = 0;
		result->dsc_sub_type = 0;
		result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec128;

		return result;
	}

	if (node->nodFlags & FLAG_DECFIXED)
	{
		const DecimalFixed d1 = MOV_get_dec_fixed(tdbb, desc, node->nodScale);
		const DecimalFixed d2 = MOV_get_dec_fixed(tdbb, &value->vlu_desc, node->nodScale);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec_fixed = (blrOp == blr_subtract) ? d2.sub(decSt, d1) : d1.add(decSt, d2);

		result->dsc_dtype = dtype_dec_fixed;
		result->dsc_length = sizeof(DecimalFixed);
		result->dsc_scale = node->nodScale;
		result->dsc_sub_type = 0;
		result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec_fixed;

		return result;
	}

	// Handle floating arithmetic

	if (node->nodFlags & FLAG_DOUBLE)
	{
		const double d1 = MOV_get_double(tdbb, desc);
		const double d2 = MOV_get_double(tdbb, &value->vlu_desc);

		value->vlu_misc.vlu_double = (blrOp == blr_subtract) ? d2 - d1 : d1 + d2;

		if (isinf(value->vlu_misc.vlu_double))
			ERR_post(Arg::Gds(isc_arith_except) << Arg::Gds(isc_exception_float_overflow));

		result->dsc_dtype = DEFAULT_DOUBLE;
		result->dsc_length = sizeof(double);
		result->dsc_scale = 0;
		result->dsc_sub_type = 0;
		result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_double;

		return result;
	}

	// Everything else defaults to int64

	SINT64 i1 = MOV_get_int64(tdbb, desc, node->nodScale);
	const SINT64 i2 = MOV_get_int64(tdbb, &value->vlu_desc, node->nodScale);

	result->dsc_dtype = dtype_int64;
	result->dsc_length = sizeof(SINT64);
	result->dsc_scale = node->nodScale;
	value->vlu_misc.vlu_int64 = (blrOp == blr_subtract) ? i2 - i1 : i1 + i2;
	result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_int64;
	result->dsc_sub_type = MAX(desc->dsc_sub_type, value->vlu_desc.dsc_sub_type);

	/* If the operands of an addition have the same sign, and their sum has
	the opposite sign, then overflow occurred.  If the two addends have
	opposite signs, then the result will lie between the two addends, and
	overflow cannot occur.
	If this is a subtraction, note that we invert the sign bit, rather than
	negating the argument, so that subtraction of MIN_SINT64, which is
	unchanged by negation, will be correctly treated like the addition of
	a positive number for the purposes of this test.

	Test cases for a Gedankenexperiment, considering the sign bits of the
	operands and result after the inversion below:                L  Rt  Sum

		MIN_SINT64 - MIN_SINT64 ==          0, with no overflow  1   0   0
	   -MAX_SINT64 - MIN_SINT64 ==          1, with no overflow  1   0   0
		1          - MIN_SINT64 == overflow                      0   0   1
	   -1          - MIN_SINT64 == MAX_SINT64, no overflow       1   0   0
	*/

	if (blrOp == blr_subtract)
		i1 ^= MIN_SINT64;		// invert the sign bit

	if ((i1 ^ i2) >= 0 && (i1 ^ value->vlu_misc.vlu_int64) < 0)
		ERR_post(Arg::Gds(isc_exception_integer_overflow));

	return result;
}

// Multiply two numbers, with SQL dialect-1 semantics.
// This function can be removed when dialect-3 becomes the lowest supported dialect. (Version 7.0?)
dsc* ArithmeticNode::multiply(const dsc* desc, impure_value* value) const
{
	thread_db* tdbb = JRD_get_thread_data();
	DEV_BLKCHK(node, type_nod);

	// Handle decimal arithmetic

	if (nodFlags & FLAG_DECFLOAT)
	{
		const Decimal128 d1 = MOV_get_dec128(tdbb, desc);
		const Decimal128 d2 = MOV_get_dec128(tdbb, &value->vlu_desc);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec128 = d1.mul(decSt, d2);

		value->vlu_desc.dsc_dtype = dtype_dec128;
		value->vlu_desc.dsc_length = sizeof(Decimal128);
		value->vlu_desc.dsc_scale = 0;
		value->vlu_desc.dsc_sub_type = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec128;

		return &value->vlu_desc;
	}

	if (nodFlags & FLAG_DECFIXED)
	{
		const DecimalFixed d1 = MOV_get_dec_fixed(tdbb, desc, nodScale);
		const DecimalFixed d2 = MOV_get_dec_fixed(tdbb, &value->vlu_desc, nodScale);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec_fixed = d1.mul(decSt, d2);

		value->vlu_desc.dsc_dtype = dtype_dec_fixed;
		value->vlu_desc.dsc_length = sizeof(DecimalFixed);
		value->vlu_desc.dsc_scale = nodScale;
		value->vlu_desc.dsc_sub_type = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec_fixed;

		return &value->vlu_desc;
	}

	// Handle floating arithmetic

	if (nodFlags & FLAG_DOUBLE)
	{
		const double d1 = MOV_get_double(tdbb, desc);
		const double d2 = MOV_get_double(tdbb, &value->vlu_desc);
		value->vlu_misc.vlu_double = d1 * d2;

		if (isinf(value->vlu_misc.vlu_double))
		{
			ERR_post(Arg::Gds(isc_arith_except) <<
					 Arg::Gds(isc_exception_float_overflow));
		}

		value->vlu_desc.dsc_dtype = DEFAULT_DOUBLE;
		value->vlu_desc.dsc_length = sizeof(double);
		value->vlu_desc.dsc_scale = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_double;

		return &value->vlu_desc;
	}

	// Everything else defaults to longword

	/* CVC: With so many problems cropping with dialect 1 and multiplication,
			I decided to close this Pandora box by incurring in INT64 performance
			overhead (if noticeable) and try to get the best result. When I read it,
			this function didn't bother even to check for overflow! */

#define FIREBIRD_AVOID_DIALECT1_OVERFLOW
	// SLONG l1, l2;
	//{
	const SSHORT scale = NUMERIC_SCALE(value->vlu_desc);
	const SINT64 i1 = MOV_get_long(tdbb, desc, nodScale - scale);
	const SINT64 i2 = MOV_get_long(tdbb, &value->vlu_desc, scale);
	value->vlu_desc.dsc_dtype = dtype_long;
	value->vlu_desc.dsc_length = sizeof(SLONG);
	value->vlu_desc.dsc_scale = nodScale;
	const SINT64 rc = i1 * i2;
	if (rc < MIN_SLONG || rc > MAX_SLONG)
	{
#ifdef FIREBIRD_AVOID_DIALECT1_OVERFLOW
		value->vlu_misc.vlu_int64 = rc;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_int64;
		value->vlu_desc.dsc_dtype = dtype_int64;
		value->vlu_desc.dsc_length = sizeof(SINT64);
		value->vlu_misc.vlu_double = MOV_get_double(tdbb, &value->vlu_desc);
		/* This is the Borland solution instead of the five lines above.
		d1 = MOV_get_double (desc);
		d2 = MOV_get_double (&value->vlu_desc);
		value->vlu_misc.vlu_double = d1 * d2; */
		value->vlu_desc.dsc_dtype = DEFAULT_DOUBLE;
		value->vlu_desc.dsc_length = sizeof(double);
		value->vlu_desc.dsc_scale = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_double;
#else
		ERR_post(Arg::Gds(isc_exception_integer_overflow));
#endif
	}
	else
	{
		value->vlu_misc.vlu_long = (SLONG) rc; // l1 * l2;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_long;
	}
	//}

	return &value->vlu_desc;
}

// Multiply two numbers, with dialect-3 semantics, implementing blr_version5 ... blr_multiply.
dsc* ArithmeticNode::multiply2(const dsc* desc, impure_value* value) const
{
	thread_db* tdbb = JRD_get_thread_data();
	DEV_BLKCHK(node, type_nod);

	// Handle decimal arithmetic

	if (nodFlags & FLAG_DECFLOAT)
	{
		const Decimal128 d1 = MOV_get_dec128(tdbb, desc);
		const Decimal128 d2 = MOV_get_dec128(tdbb, &value->vlu_desc);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec128 = d1.mul(decSt, d2);

		value->vlu_desc.dsc_dtype = dtype_dec128;
		value->vlu_desc.dsc_length = sizeof(Decimal128);
		value->vlu_desc.dsc_scale = 0;
		value->vlu_desc.dsc_sub_type = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec128;

		return &value->vlu_desc;
	}

	if (nodFlags & FLAG_DECFIXED)
	{
		const SSHORT scale = NUMERIC_SCALE(*desc);
		const DecimalFixed d1 = MOV_get_dec_fixed(tdbb, desc, scale);
		const DecimalFixed d2 = MOV_get_dec_fixed(tdbb, &value->vlu_desc, nodScale - scale);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec_fixed = d1.mul(decSt, d2);

		value->vlu_desc.dsc_dtype = dtype_dec_fixed;
		value->vlu_desc.dsc_length = sizeof(DecimalFixed);
		value->vlu_desc.dsc_scale = nodScale;
		value->vlu_desc.dsc_sub_type = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec_fixed;

		return &value->vlu_desc;
	}

	// Handle floating arithmetic

	if (nodFlags & FLAG_DOUBLE)
	{
		const double d1 = MOV_get_double(tdbb, desc);
		const double d2 = MOV_get_double(tdbb, &value->vlu_desc);
		value->vlu_misc.vlu_double = d1 * d2;

		if (isinf(value->vlu_misc.vlu_double))
		{
			ERR_post(Arg::Gds(isc_arith_except) <<
					 Arg::Gds(isc_exception_float_overflow));
		}

		value->vlu_desc.dsc_dtype = DEFAULT_DOUBLE;
		value->vlu_desc.dsc_length = sizeof(double);
		value->vlu_desc.dsc_scale = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_double;

		return &value->vlu_desc;
	}

	// Everything else defaults to int64

	const SSHORT scale = NUMERIC_SCALE(value->vlu_desc);
	const SINT64 i1 = MOV_get_int64(tdbb, desc, nodScale - scale);
	const SINT64 i2 = MOV_get_int64(tdbb, &value->vlu_desc, scale);

	/*
	We need to report an overflow if
	   (i1 * i2 < MIN_SINT64) || (i1 * i2 > MAX_SINT64)
	which is equivalent to
	   (i1 < MIN_SINT64 / i2) || (i1 > MAX_SINT64 / i2)

	Unfortunately, a trial division to see whether the multiplication will
	overflow is expensive: fortunately, we only need perform one division and
	test for one of the two cases, depending on whether the factors have the
	same or opposite signs.

	Unfortunately, in C it is unspecified which way division rounds
	when one or both arguments are negative.  (ldiv() is guaranteed to
	round towards 0, but the standard does not yet require an lldiv()
	or whatever for 64-bit operands.  This makes the problem messy.
	We use FB_UINT64s for the checking, thus ensuring that our division rounds
	down.  This means that we have to check the sign of the product first
	in order to know whether the maximum abs(i1*i2) is MAX_SINT64 or
	(MAX_SINT64+1).

	Of course, if a factor is 0, the product will also be 0, and we don't
	need a trial-division to be sure the multiply won't overflow.
	*/

	const FB_UINT64 u1 = (i1 >= 0) ? i1 : -i1;	// abs(i1)
	const FB_UINT64 u2 = (i2 >= 0) ? i2 : -i2;	// abs(i2)
	// largest product
	const FB_UINT64 u_limit = ((i1 ^ i2) >= 0) ? MAX_SINT64 : (FB_UINT64) MAX_SINT64 + 1;

	if ((u1 != 0) && ((u_limit / u1) < u2)) {
		ERR_post(Arg::Gds(isc_exception_integer_overflow));
	}

	value->vlu_desc.dsc_dtype = dtype_int64;
	value->vlu_desc.dsc_length = sizeof(SINT64);
	value->vlu_desc.dsc_scale = nodScale;
	value->vlu_misc.vlu_int64 = i1 * i2;
	value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_int64;

	return &value->vlu_desc;
}

// Divide two numbers, with SQL dialect-3 semantics, as in the blr_version5 ... blr_divide or
// blr_version5 ... blr_average ....
dsc* ArithmeticNode::divide2(const dsc* desc, impure_value* value) const
{
	thread_db* tdbb = JRD_get_thread_data();
	DEV_BLKCHK(node, type_nod);

	// Handle decimal arithmetic

	if (nodFlags & FLAG_DECFLOAT)
	{
		const Decimal128 d1 = MOV_get_dec128(tdbb, desc);
		const Decimal128 d2 = MOV_get_dec128(tdbb, &value->vlu_desc);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec128 = d2.div(decSt, d1);

		value->vlu_desc.dsc_dtype = dtype_dec128;
		value->vlu_desc.dsc_length = sizeof(Decimal128);
		value->vlu_desc.dsc_scale = 0;
		value->vlu_desc.dsc_sub_type = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec128;

		return &value->vlu_desc;
	}

	if (nodFlags & FLAG_DECFIXED)
	{
		const SSHORT scale = NUMERIC_SCALE(*desc);
		const DecimalFixed d2 = MOV_get_dec_fixed(tdbb, desc, scale);
		const DecimalFixed d1 = MOV_get_dec_fixed(tdbb, &value->vlu_desc, nodScale - scale);

		DecimalStatus decSt = tdbb->getAttachment()->att_dec_status;
		value->vlu_misc.vlu_dec_fixed = d1.div(decSt, d2, scale * 2);

		value->vlu_desc.dsc_dtype = dtype_dec_fixed;
		value->vlu_desc.dsc_length = sizeof(DecimalFixed);
		value->vlu_desc.dsc_scale = nodScale;
		value->vlu_desc.dsc_sub_type = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_dec_fixed;

		return &value->vlu_desc;
	}

	// Handle floating arithmetic

	if (nodFlags & FLAG_DOUBLE)
	{
		const double d2 = MOV_get_double(tdbb, desc);
		if (d2 == 0.0)
		{
			ERR_post(Arg::Gds(isc_arith_except) <<
					 Arg::Gds(isc_exception_float_divide_by_zero));
		}
		const double d1 = MOV_get_double(tdbb, &value->vlu_desc);
		value->vlu_misc.vlu_double = d1 / d2;
		if (isinf(value->vlu_misc.vlu_double))
		{
			ERR_post(Arg::Gds(isc_arith_except) <<
					 Arg::Gds(isc_exception_float_overflow));
		}
		value->vlu_desc.dsc_dtype = DEFAULT_DOUBLE;
		value->vlu_desc.dsc_length = sizeof(double);
		value->vlu_desc.dsc_scale = 0;
		value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_double;
		return &value->vlu_desc;
	}

	// Everything else defaults to int64

	/*
	 * In the SQL standard, the precision and scale of the quotient of exact
	 * numeric dividend and divisor are implementation-defined: we have defined
	 * the precision as 18 (in other words, an SINT64), and the scale as the
	 * sum of the scales of the two operands.  To make this work, we have to
	 * multiply by pow(10, -2* (scale of divisor)).
	 *
	 * To see this, consider the operation n1 / n2, and represent the numbers
	 * by ordered pairs (v1, s1) and (v2, s2), representing respectively the
	 * integer value and the scale of each operation, so that
	 *     n1 = v1 * pow(10, s1), and
	 *     n2 = v2 * pow(10, s2)
	 * Then the quotient is ...
	 *
	 *     v1 * pow(10,s1)
	 *     ----------------- = (v1/v2) * pow(10, s1-s2)
	 *     v2 * pow(10,s2)
	 *
	 * But we want the scale of the result to be (s1+s2), not (s1-s2)
	 * so we need to multiply by 1 in the form
	 *         pow(10, -2 * s2) * pow(20, 2 * s2)
	 * which, after regrouping, gives us ...
	 *   =  ((v1 * pow(10, -2*s2))/v2) * pow(10, 2*s2) * pow(10, s1-s2)
	 *   =  ((v1 * pow(10, -2*s2))/v2) * pow(10, 2*s2 + s1 - s2)
	 *   =  ((v1 * pow(10, -2*s2))/v2) * pow(10, s1 + s2)
	 * or in our ordered-pair notation,
	 *      ( v1 * pow(10, -2*s2) / v2, s1 + s2 )
	 *
	 * To maximize the amount of information in the result, we scale up
	 * the dividend as far as we can without causing overflow, then we perform
	 * the division, then do any additional required scaling.
	 *
	 * Who'da thunk that 9th-grade algebra would prove so useful.
	 *                                      -- Chris Jewell, December 1998
	 */
	SINT64 i2 = MOV_get_int64(tdbb, desc, desc->dsc_scale);
	if (i2 == 0)
	{
		ERR_post(Arg::Gds(isc_arith_except) <<
				 Arg::Gds(isc_exception_integer_divide_by_zero));
	}

	SINT64 i1 = MOV_get_int64(tdbb, &value->vlu_desc, nodScale - desc->dsc_scale);

	// MIN_SINT64 / -1 = (MAX_SINT64 + 1), which overflows in SINT64.
	if ((i1 == MIN_SINT64) && (i2 == -1))
		ERR_post(Arg::Gds(isc_exception_integer_overflow));

	// Scale the dividend by as many of the needed powers of 10 as possible
	// without causing an overflow.
	int addl_scale = 2 * desc->dsc_scale;
	if (i1 >= 0)
	{
		while ((addl_scale < 0) && (i1 <= MAX_INT64_LIMIT))
		{
			i1 *= 10;
			++addl_scale;
		}
	}
	else
	{
		while ((addl_scale < 0) && (i1 >= MIN_INT64_LIMIT))
		{
			i1 *= 10;
			++addl_scale;
		}
	}

	// If we couldn't use up all the additional scaling by multiplying the
	// dividend by 10, but there are trailing zeroes in the divisor, we can
	// get the same effect by dividing the divisor by 10 instead.
	while ((addl_scale < 0) && (0 == (i2 % 10)))
	{
		i2 /= 10;
		++addl_scale;
	}

	value->vlu_desc.dsc_dtype = dtype_int64;
	value->vlu_desc.dsc_length = sizeof(SINT64);
	value->vlu_desc.dsc_scale = nodScale;
	value->vlu_misc.vlu_int64 = i1 / i2;
	value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc.vlu_int64;

	// If we couldn't do all the required scaling beforehand without causing
	// an overflow, do the rest of it now.  If we get an overflow now, then
	// the result is really too big to store in a properly-scaled SINT64,
	// so report the error. For example, MAX_SINT64 / 1.00 overflows.
	if (value->vlu_misc.vlu_int64 >= 0)
	{
		while ((addl_scale < 0) && (value->vlu_misc.vlu_int64 <= MAX_INT64_LIMIT))
		{
			value->vlu_misc.vlu_int64 *= 10;
			addl_scale++;
		}
	}
	else
	{
		while ((addl_scale < 0) && (value->vlu_misc.vlu_int64 >= MIN_INT64_LIMIT))
		{
			value->vlu_misc.vlu_int64 *= 10;
			addl_scale++;
		}
	}

	if (addl_scale < 0)
	{
		ERR_post(Arg::Gds(isc_arith_except) <<
				 Arg::Gds(isc_numeric_out_of_range));
	}

	return &value->vlu_desc;
}

// Vector out to one of the actual datetime addition routines.
dsc* ArithmeticNode::addDateTime(thread_db* tdbb, const dsc* desc, impure_value* value) const
{
	BYTE dtype;					// Which addition routine to use?

	fb_assert(nodFlags & FLAG_DATE);

	// Value is the LHS of the operand.  desc is the RHS

	if (blrOp == blr_add)
		dtype = DSC_add_result[value->vlu_desc.dsc_dtype][desc->dsc_dtype];
	else
	{
		fb_assert(blrOp == blr_subtract);
		dtype = DSC_sub_result[value->vlu_desc.dsc_dtype][desc->dsc_dtype];

		/* Is this a <date type> - <date type> construct?
		   chose the proper routine to do the subtract from the
		   LHS of expression
		   Thus:   <TIME> - <TIMESTAMP> uses TIME arithmetic
		   <DATE> - <TIMESTAMP> uses DATE arithmetic
		   <TIMESTAMP> - <DATE> uses TIMESTAMP arithmetic */
		if (DTYPE_IS_NUMERIC(dtype))
			dtype = value->vlu_desc.dsc_dtype;

		// Handle historical <timestamp> = <string> - <value> case
		if (!DTYPE_IS_DATE(dtype) &&
			(DTYPE_IS_TEXT(value->vlu_desc.dsc_dtype) || DTYPE_IS_TEXT(desc->dsc_dtype)))
		{
			dtype = dtype_timestamp;
		}
	}

	switch (dtype)
	{
		case dtype_sql_time:
		case dtype_sql_time_tz:
			return addSqlTime(tdbb, desc, value);

		case dtype_sql_date:
			return addSqlDate(desc, value);

		case DTYPE_CANNOT:
			ERR_post(Arg::Gds(isc_expression_eval_err) << Arg::Gds(isc_invalid_type_datetime_op));
			break;

		case dtype_timestamp:
		case dtype_timestamp_tz:
		default:
			// This needs to handle a dtype_sql_date + dtype_sql_time
			// For historical reasons prior to V6 - handle any types for timestamp arithmetic
			return addTimeStamp(tdbb, desc, value);
	}

	return NULL;
}

// Perform date arithmetic.
// DATE -   DATE	   Result is SLONG
// DATE +/- NUMERIC   Numeric is interpreted as days DECIMAL(*,0).
// NUMERIC +/- TIME   Numeric is interpreted as days DECIMAL(*,0).
dsc* ArithmeticNode::addSqlDate(const dsc* desc, impure_value* value) const
{
	DEV_BLKCHK(node, type_nod);
	fb_assert(blrOp == blr_add || blrOp == blr_subtract);

	dsc* result = &value->vlu_desc;
	thread_db* tdbb = JRD_get_thread_data();

	fb_assert(value->vlu_desc.dsc_dtype == dtype_sql_date || desc->dsc_dtype == dtype_sql_date);

	SINT64 d1;
	// Coerce operand1 to a count of days
	bool op1_is_date = false;
	if (value->vlu_desc.dsc_dtype == dtype_sql_date)
	{
		d1 = *((GDS_DATE*) value->vlu_desc.dsc_address);
		op1_is_date = true;
	}
	else
		d1 = MOV_get_int64(tdbb, &value->vlu_desc, 0);

	SINT64 d2;
	// Coerce operand2 to a count of days
	bool op2_is_date = false;
	if (desc->dsc_dtype == dtype_sql_date)
	{
		d2 = *((GDS_DATE*) desc->dsc_address);
		op2_is_date = true;
	}
	else
		d2 = MOV_get_int64(tdbb, desc, 0);

	if (blrOp == blr_subtract && op1_is_date && op2_is_date)
	{
		d2 = d1 - d2;
		value->make_int64(d2);
		return result;
	}

	fb_assert(op1_is_date || op2_is_date);
	fb_assert(!(op1_is_date && op2_is_date));

	// Perform the operation

	if (blrOp == blr_subtract)
	{
		fb_assert(op1_is_date);
		d2 = d1 - d2;
	}
	else
		d2 = d1 + d2;

	value->vlu_misc.vlu_sql_date = d2;

	if (!TimeStamp::isValidDate(value->vlu_misc.vlu_sql_date))
		ERR_post(Arg::Gds(isc_date_range_exceeded));

	result->dsc_dtype = dtype_sql_date;
	result->dsc_length = type_lengths[result->dsc_dtype];
	result->dsc_scale = 0;
	result->dsc_sub_type = 0;
	result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_sql_date;
	return result;

}

// Perform time arithmetic.
// TIME - TIME			Result is SLONG, scale -4
// TIME +/- NUMERIC		Numeric is interpreted as seconds DECIMAL(*,4).
// NUMERIC +/- TIME		Numeric is interpreted as seconds DECIMAL(*,4).
dsc* ArithmeticNode::addSqlTime(thread_db* tdbb, const dsc* desc, impure_value* value) const
{
	fb_assert(blrOp == blr_add || blrOp == blr_subtract);

	dsc* result = &value->vlu_desc;
	Attachment* const attachment = tdbb->getAttachment();

	fb_assert(value->vlu_desc.isTime() || desc->isTime());

	const dsc* op1_desc = &value->vlu_desc;
	const dsc* op2_desc = desc;

	bool op1_is_time = op1_desc->isTime();
	bool op2_is_time = op2_desc->isTime();

	Nullable<USHORT> op1_tz, op2_tz;

	if (op1_desc->dsc_dtype == dtype_sql_time_tz)
		op1_tz = ((ISC_TIME_TZ*) op1_desc->dsc_address)->time_zone;

	if (op2_desc->dsc_dtype == dtype_sql_time_tz)
		op2_tz = ((ISC_TIME_TZ*) op2_desc->dsc_address)->time_zone;

	dsc op1_tz_desc, op2_tz_desc;
	ISC_TIME_TZ op1_time_tz, op2_time_tz;

	if (op1_desc->dsc_dtype == dtype_sql_time && op2_is_time && op2_tz.specified)
	{
		op1_tz_desc.makeTimeTz(&op1_time_tz);
		MOV_move(tdbb, const_cast<dsc*>(op1_desc), &op1_tz_desc);
		op1_desc = &op1_tz_desc;
	}

	if (op2_desc->dsc_dtype == dtype_sql_time && op1_is_time && op1_tz.specified)
	{
		op2_tz_desc.makeTimeTz(&op2_time_tz);
		MOV_move(tdbb, const_cast<dsc*>(op2_desc), &op2_tz_desc);
		op2_desc = &op2_tz_desc;
	}

	// Coerce operand1 to a count of seconds
	SINT64 d1;

	if (op1_is_time)
	{
		d1 = *(GDS_TIME*) op1_desc->dsc_address;
		fb_assert(d1 >= 0 && d1 < ISC_TICKS_PER_DAY);
	}
	else
		d1 = MOV_get_int64(tdbb, op1_desc, ISC_TIME_SECONDS_PRECISION_SCALE);

	// Coerce operand2 to a count of seconds
	SINT64 d2;

	if (op2_is_time)
	{
		d2 = *(GDS_TIME*) op2_desc->dsc_address;
		fb_assert(d2 >= 0 && d2 < ISC_TICKS_PER_DAY);
	}
	else
		d2 = MOV_get_int64(tdbb, op2_desc, ISC_TIME_SECONDS_PRECISION_SCALE);

	if (blrOp == blr_subtract && op1_is_time && op2_is_time)
	{
		d2 = d1 - d2;
		// Overflow cannot occur as the range of supported TIME values
		// is less than the range of INTEGER
		value->vlu_misc.vlu_long = d2;
		result->dsc_dtype = dtype_long;
		result->dsc_length = sizeof(SLONG);
		result->dsc_scale = ISC_TIME_SECONDS_PRECISION_SCALE;
		result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_long;
		return result;
	}

	fb_assert(op1_is_time || op2_is_time);
	fb_assert(!(op1_is_time && op2_is_time));

	// Perform the operation

	if (blrOp == blr_subtract)
	{
		fb_assert(op1_is_time);
		d2 = d1 - d2;
	}
	else
		d2 = d1 + d2;

	// Make sure to use modulo 24 hour arithmetic

	// Make the result positive
	while (d2 < 0)
		d2 += (ISC_TICKS_PER_DAY);

	// And make it in the range of values for a day
	d2 %= (ISC_TICKS_PER_DAY);

	fb_assert(d2 >= 0 && d2 < ISC_TICKS_PER_DAY);

	value->vlu_misc.vlu_sql_time_tz.utc_time = d2;

	result->dsc_dtype = op1_tz.specified || op2_tz.specified ? dtype_sql_time_tz : dtype_sql_time;
	result->dsc_length = type_lengths[result->dsc_dtype];
	result->dsc_scale = 0;
	result->dsc_sub_type = 0;
	result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_sql_time_tz;

	fb_assert(!(op1_tz.specified && op2_tz.specified));

	if (op1_tz.specified)
		value->vlu_misc.vlu_sql_time_tz.time_zone = op1_tz.value;
	else if (op2_tz.specified)
		value->vlu_misc.vlu_sql_time_tz.time_zone = op2_tz.value;

	return result;
}

// Perform date&time arithmetic.
// TIMESTAMP - TIMESTAMP	Result is INT64
// TIMESTAMP +/- NUMERIC   Numeric is interpreted as days DECIMAL(*,*).
// NUMERIC +/- TIMESTAMP   Numeric is interpreted as days DECIMAL(*,*).
// DATE + TIME
// TIME + DATE
dsc* ArithmeticNode::addTimeStamp(thread_db* tdbb, const dsc* desc, impure_value* value) const
{
	fb_assert(blrOp == blr_add || blrOp == blr_subtract);

	const dsc* op1_desc = &value->vlu_desc;
	const dsc* op2_desc = desc;

	Nullable<USHORT> op1_tz, op2_tz;

	if (op1_desc->dsc_dtype == dtype_sql_time_tz)
		op1_tz = ((ISC_TIME_TZ*) op1_desc->dsc_address)->time_zone;
	else if (op1_desc->dsc_dtype == dtype_timestamp_tz)
		op1_tz = ((ISC_TIMESTAMP_TZ*) op1_desc->dsc_address)->time_zone;

	if (op2_desc->dsc_dtype == dtype_sql_time_tz)
		op2_tz = ((ISC_TIME_TZ*) op2_desc->dsc_address)->time_zone;
	else if (op2_desc->dsc_dtype == dtype_timestamp_tz)
		op2_tz = ((ISC_TIMESTAMP_TZ*) op2_desc->dsc_address)->time_zone;

	dsc op1_tz_desc, op2_tz_desc;
	ISC_TIMESTAMP_TZ op1_timestamp_tz, op2_timestamp_tz;
	ISC_TIME_TZ op1_time_tz, op2_time_tz;

	if ((op1_desc->dsc_dtype == dtype_sql_time || op1_desc->dsc_dtype == dtype_timestamp) &&
		op2_desc->isDateTime() && op2_tz.specified)
	{
		if (op1_desc->dsc_dtype == dtype_sql_time)
			op1_tz_desc.makeTimeTz(&op1_time_tz);
		else
			op1_tz_desc.makeTimestampTz(&op1_timestamp_tz);

		MOV_move(tdbb, const_cast<dsc*>(op1_desc), &op1_tz_desc);
		op1_desc = &op1_tz_desc;
	}

	if ((op2_desc->dsc_dtype == dtype_sql_time || op2_desc->dsc_dtype == dtype_timestamp) &&
		op1_desc->isDateTime() && op1_tz.specified)
	{
		if (op2_desc->dsc_dtype == dtype_sql_time)
			op2_tz_desc.makeTimeTz(&op2_time_tz);
		else
			op2_tz_desc.makeTimestampTz(&op2_timestamp_tz);

		MOV_move(tdbb, const_cast<dsc*>(op2_desc), &op2_tz_desc);
		op2_desc = &op2_tz_desc;
	}

	SINT64 d1, d2;

	dsc* result = &value->vlu_desc;

	// Operand 1 is Value -- Operand 2 is desc

	if (op1_desc->dsc_dtype == dtype_sql_date)
	{
		// DATE + TIME
		if (op2_desc->isTime() && blrOp == blr_add)
		{
			value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_date = *(GDS_DATE*) op1_desc->dsc_address;
			value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_time = *(GDS_TIME*) op2_desc->dsc_address;
		}
		else
			ERR_post(Arg::Gds(isc_expression_eval_err) << Arg::Gds(isc_onlycan_add_timetodate));
	}
	else if (op2_desc->dsc_dtype == dtype_sql_date)
	{
		// TIME + DATE
		if (op1_desc->isTime() && blrOp == blr_add)
		{
			value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_time = *(GDS_TIME*) op1_desc->dsc_address;
			value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_date = *(GDS_DATE*) op2_desc->dsc_address;
		}
		else
			ERR_post(Arg::Gds(isc_expression_eval_err) << Arg::Gds(isc_onlycan_add_datetotime));
	}
	else
	{
		/* For historical reasons (behavior prior to V6),
		there are times we will do timestamp arithmetic without a
		timestamp being involved.
		In such an event we need to convert a text type to a timestamp when
		we don't already have one.
		We assume any text string must represent a timestamp value. */

		/* If we're subtracting, and the 2nd operand is a timestamp, or
		something that looks & smells like it could be a timestamp, then
		we must be doing <timestamp> - <timestamp> subtraction.
		Notes that this COULD be as strange as <string> - <string>, but
		because FLAG_DATE is set in the nod_flags we know we're supposed
		to use some form of date arithmetic */

		if (blrOp == blr_subtract &&
			(op2_desc->isTimeStamp() || DTYPE_IS_TEXT(op2_desc->dsc_dtype)))
		{
			/* Handle cases of
			   <string>    - <string>
			   <string>    - <timestamp>
			   <timestamp> - <string>
			   <timestamp> - <timestamp>
			   in which cases we assume the string represents a timestamp value */

			// If the first operand couldn't represent a timestamp, bomb out

			if (!(op1_desc->isTimeStamp() || DTYPE_IS_TEXT(op1_desc->dsc_dtype)))
				ERR_post(Arg::Gds(isc_expression_eval_err) << Arg::Gds(isc_onlycansub_tstampfromtstamp));

			d1 = getTimeStampToIscTicks(tdbb, op1_desc);
			d2 = getTimeStampToIscTicks(tdbb, op2_desc);

			d2 = d1 - d2;

			if (!dialect1)
			{
				/* multiply by 100,000 so that we can have the result as decimal (18,9)
				 * We have 10 ^-4; to convert this to 10^-9 we need to multiply by
				 * 100,000. Of course all this is true only because we are dividing
				 * by SECONDS_PER_DAY
				 * now divide by the number of seconds per day, this will give us the
				 * result as a int64 of type decimal (18, 9) in days instead of
				 * seconds.
				 *
				 * But SECONDS_PER_DAY has 2 trailing zeroes (because it is 24 * 60 *
				 * 60), so instead of calculating (X * 100000) / SECONDS_PER_DAY,
				 * use (X * (100000 / 100)) / (SECONDS_PER_DAY / 100), which can be
				 * simplified to (X * 1000) / (SECONDS_PER_DAY / 100)
				 * Since the largest possible difference in timestamps is about 3E11
				 * seconds or 3E15 isc_ticks, the product won't exceed approximately
				 * 3E18, which fits into an INT64.
				 */
				// 09-Apr-2004, Nickolay Samofatov. Adjust number before division to
				// make sure we don't lose a tick as a result of remainder truncation

				if (d2 >= 0)
					d2 = (d2 * 1000 + (SECONDS_PER_DAY / 200)) / (SINT64) (SECONDS_PER_DAY / 100);
				else
					d2 = (d2 * 1000 - (SECONDS_PER_DAY / 200)) / (SINT64) (SECONDS_PER_DAY / 100);

				value->vlu_misc.vlu_int64 = d2;
				result->dsc_dtype = dtype_int64;
				result->dsc_length = sizeof(SINT64);
				result->dsc_scale = DIALECT_3_TIMESTAMP_SCALE;
				result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_int64;

				return result;
			}

			// This is dialect 1 subtraction returning double as before
			value->vlu_misc.vlu_double = (double) d2 / ((double) ISC_TICKS_PER_DAY);
			result->dsc_dtype = dtype_double;
			result->dsc_length = sizeof(double);
			result->dsc_scale = DIALECT_1_TIMESTAMP_SCALE;
			result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_double;

			return result;
		}

		/* From here we know our result must be a <timestamp>.  The only
		legal cases are:
		<timestamp> +/-  <numeric>
		<numeric>   +    <timestamp>
		However, the FLAG_DATE flag might have been set on any type of
		nod_add / nod_subtract equation -- so we must detect any invalid
		operands.   Any <string> value is assumed to be convertable to
		a timestamp */

		// Coerce operand1 to a count of microseconds
		bool op1_is_timestamp = op1_desc->isTimeStamp() || DTYPE_IS_TEXT(op1_desc->dsc_dtype);

		// Coerce operand2 to a count of microseconds
		bool op2_is_timestamp = op2_desc->isTimeStamp() || DTYPE_IS_TEXT(op2_desc->dsc_dtype);

		// Exactly one of the operands must be a timestamp or
		// convertable into a timestamp, otherwise it's one of
		//    <numeric>   +/- <numeric>
		// or <timestamp> +/- <timestamp>
		// or <string>    +/- <string>
		// which are errors

		if (op1_is_timestamp == op2_is_timestamp)
			ERR_post(Arg::Gds(isc_expression_eval_err) << Arg::Gds(isc_onlyoneop_mustbe_tstamp));

		if (op1_is_timestamp)
		{
			d1 = getTimeStampToIscTicks(tdbb, op1_desc);
			d2 = getDayFraction(op2_desc);
		}
		else
		{
			fb_assert(blrOp == blr_add);
			fb_assert(op2_is_timestamp);
			d1 = getDayFraction(op1_desc);
			d2 = getTimeStampToIscTicks(tdbb, op2_desc);
		}

		// Perform the operation

		if (blrOp == blr_subtract)
		{
			fb_assert(op1_is_timestamp);
			d2 = d1 - d2;
		}
		else
			d2 = d1 + d2;

		// Convert the count of microseconds back to a date / time format

		value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_date = d2 / (ISC_TICKS_PER_DAY);
		value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_time = (d2 % ISC_TICKS_PER_DAY);

		// Make sure the TIME portion is non-negative

		if ((SLONG) value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_time < 0)
		{
			value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_time =
				((SLONG) value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_time) + ISC_TICKS_PER_DAY;
			--value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_date;
		}

		if (!TimeStamp::isValidTimeStamp(*(ISC_TIMESTAMP*) &value->vlu_misc.vlu_timestamp_tz))
			ERR_post(Arg::Gds(isc_datetime_range_exceeded));
	}

	fb_assert(value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_time >= 0 &&
		value->vlu_misc.vlu_timestamp_tz.utc_timestamp.timestamp_time < ISC_TICKS_PER_DAY);

	fb_assert(!(op1_tz.specified && op2_tz.specified));

	result->dsc_dtype = op1_tz.specified || op2_tz.specified ? dtype_timestamp_tz : dtype_timestamp;
	result->dsc_length = type_lengths[result->dsc_dtype];
	result->dsc_scale = 0;
	result->dsc_sub_type = 0;
	result->dsc_address = (UCHAR*) &value->vlu_misc.vlu_timestamp_tz;

	if (op1_tz.specified)
		value->vlu_misc.vlu_timestamp_tz.time_zone = op1_tz.value;
	else if (op2_tz.specified)
		value->vlu_misc.vlu_timestamp_tz.time_zone = op2_tz.value;

	return result;
}

ValueExprNode* ArithmeticNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return FB_NEW_POOL(dsqlScratch->getPool()) ArithmeticNode(dsqlScratch->getPool(),
		blrOp, dialect1, doDsqlPass(dsqlScratch, arg1), doDsqlPass(dsqlScratch, arg2));
}


//--------------------


ArrayNode::ArrayNode(MemoryPool& pool, FieldNode* aField)
	: TypedNode<ValueExprNode, ExprNode::TYPE_ARRAY>(pool),
	  field(aField)
{
}

string ArrayNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, field);

	return "ArrayNode";
}

ValueExprNode* ArrayNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (dsqlScratch->isPsql())
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  Arg::Gds(isc_dsql_invalid_array));
	}

	return field->internalDsqlPass(dsqlScratch, NULL);
}


//--------------------


static RegisterNode<AtNode> regAtNode(blr_at);

AtNode::AtNode(MemoryPool& pool, ValueExprNode* aDateTimeArg, ValueExprNode* aZoneArg)
	: TypedNode<ValueExprNode, ExprNode::TYPE_AT>(pool),
	  dateTimeArg(aDateTimeArg),
	  zoneArg(aZoneArg)
{
}

DmlNode* AtNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	AtNode* node = FB_NEW_POOL(pool) AtNode(pool);

	node->dateTimeArg = PAR_parse_value(tdbb, csb);

	switch (csb->csb_blr_reader.getByte())
	{
		default:
			fb_assert(false);
			// fall into

		case blr_at_local:
			node->zoneArg = NULL;
			break;

		case blr_at_zone:
			node->zoneArg = PAR_parse_value(tdbb, csb);
			break;
	}

	return node;
}

string AtNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, dateTimeArg);
	NODE_PRINT(printer, zoneArg);

	return "AtNode";
}

ValueExprNode* AtNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	AtNode* node = FB_NEW_POOL(dsqlScratch->getPool()) AtNode(dsqlScratch->getPool(),
		doDsqlPass(dsqlScratch, dateTimeArg), doDsqlPass(dsqlScratch, zoneArg));
	node->setParameterType(dsqlScratch, NULL, false);
	return node;
}

void AtNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "AT";
}

bool AtNode::setParameterType(DsqlCompilerScratch* dsqlScratch, const dsc* desc, bool forceVarChar)
{
	dsc zoneDesc;
	zoneDesc.makeText(TimeZoneUtil::MAX_LEN, ttype_ascii);
	zoneDesc.setNullable(true);

	return PASS1_set_parameter_type(dsqlScratch, dateTimeArg, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, zoneArg, &zoneDesc, forceVarChar);
}

void AtNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_at);

	GEN_expr(dsqlScratch, dateTimeArg);

	if (zoneArg)
	{
		dsqlScratch->appendUChar(blr_at_zone);
		GEN_expr(dsqlScratch, zoneArg);
	}
	else
		dsqlScratch->appendUChar(blr_at_local);
}

void AtNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	dsc dateTimeDesc, zoneDesc;
	MAKE_desc(dsqlScratch, &dateTimeDesc, dateTimeArg);

	if (zoneArg)
		MAKE_desc(dsqlScratch, &zoneDesc, zoneArg);
	else
	{
		zoneDesc.clear();
		zoneDesc.setNullable(false);
	}

	if (dateTimeDesc.isTime())
		desc->makeTimeTz();
	else if (dateTimeDesc.isTimeStamp())
		desc->makeTimestampTz();
	else
		ERRD_post(Arg::Gds(isc_expression_eval_err));	//// TODO: more info

	desc->setNullable(dateTimeDesc.isNullable() || (zoneArg && zoneDesc.isNullable()));
}

void AtNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	dsc dateTimeDesc, zoneDesc;

	dateTimeArg->getDesc(tdbb, csb, &dateTimeDesc);

	if (zoneArg)
		zoneArg->getDesc(tdbb, csb, &zoneDesc);

	if (dateTimeDesc.isTime())
		desc->makeTimeTz();
	else if (dateTimeDesc.isTimeStamp())
		desc->makeTimestampTz();

	desc->setNullable(dateTimeDesc.isNullable() || (zoneArg && zoneDesc.isNullable()));
}

ValueExprNode* AtNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	AtNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) AtNode(*tdbb->getDefaultPool());
	node->dateTimeArg = copier.copy(tdbb, dateTimeArg);
	node->zoneArg = copier.copy(tdbb, zoneArg);
	return node;
}

ValueExprNode* AtNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* AtNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	dsc* dateTimeDesc = EVL_expr(tdbb, request, dateTimeArg);

	if (!dateTimeDesc || (request->req_flags & req_null))
		return NULL;

	dsc* zoneDesc = zoneArg ? EVL_expr(tdbb, request, zoneArg) : NULL;

	if (zoneArg && (!zoneDesc || (request->req_flags & req_null)))
		return NULL;

	USHORT zone;

	if (zoneArg)
	{
		MoveBuffer zoneBuffer;
		UCHAR* zoneStr;
		unsigned zoneLen = MOV_make_string2(tdbb, zoneDesc, CS_ASCII, &zoneStr, zoneBuffer);

		zone = TimeZoneUtil::parse((char*) zoneStr, zoneLen);
	}
	else
		zone = tdbb->getAttachment()->att_current_timezone;

	if (dateTimeDesc->isTimeStamp())
	{
		impure->vlu_desc.makeTimestampTz(&impure->vlu_misc.vlu_timestamp_tz);
		MOV_move(tdbb, dateTimeDesc, &impure->vlu_desc);
		impure->vlu_misc.vlu_timestamp_tz.time_zone = zone;
	}
	else if (dateTimeDesc->isTime())
	{
		impure->vlu_desc.makeTimeTz(&impure->vlu_misc.vlu_sql_time_tz);
		MOV_move(tdbb, dateTimeDesc, &impure->vlu_desc);
		impure->vlu_misc.vlu_sql_time_tz.time_zone = zone;
	}
	else
		ERR_post(Arg::Gds(isc_expression_eval_err));	//// TODO: more info

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<BoolAsValueNode> regBoolAsValueNode(blr_bool_as_value);

BoolAsValueNode::BoolAsValueNode(MemoryPool& pool, BoolExprNode* aBoolean)
	: TypedNode<ValueExprNode, ExprNode::TYPE_BOOL_AS_VALUE>(pool),
	  boolean(aBoolean)
{
}

DmlNode* BoolAsValueNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	BoolAsValueNode* node = FB_NEW_POOL(pool) BoolAsValueNode(pool);
	node->boolean = PAR_parse_boolean(tdbb, csb);
	return node;
}

string BoolAsValueNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, boolean);

	return "BoolAsValueNode";
}

ValueExprNode* BoolAsValueNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	BoolAsValueNode* node = FB_NEW_POOL(dsqlScratch->getPool()) BoolAsValueNode(
		dsqlScratch->getPool(), doDsqlPass(dsqlScratch, boolean));

	return node;
}

void BoolAsValueNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_bool_as_value);
	GEN_expr(dsqlScratch, boolean);
}

void BoolAsValueNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	desc->makeBoolean();
	desc->setNullable(true);
}

void BoolAsValueNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->makeBoolean();
	desc->setNullable(true);
}

ValueExprNode* BoolAsValueNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	BoolAsValueNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) BoolAsValueNode(*tdbb->getDefaultPool());
	node->boolean = copier.copy(tdbb, boolean);
	return node;
}

ValueExprNode* BoolAsValueNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* BoolAsValueNode::execute(thread_db* tdbb, jrd_req* request) const
{
	UCHAR booleanVal = (UCHAR) boolean->execute(tdbb, request);

	if (request->req_flags & req_null)
		return NULL;

	impure_value* impure = request->getImpure<impure_value>(impureOffset);

	dsc desc;
	desc.makeBoolean(&booleanVal);
	EVL_make_value(tdbb, &desc, impure);

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<CastNode> regCastNode(blr_cast);

CastNode::CastNode(MemoryPool& pool, ValueExprNode* aSource, dsql_fld* aDsqlField)
	: TypedNode<ValueExprNode, ExprNode::TYPE_CAST>(pool),
	  dsqlAlias("CAST"),
	  dsqlField(aDsqlField),
	  source(aSource),
	  itemInfo(NULL),
	  artificial(false)
{
	castDesc.clear();
}

// Parse a datatype cast.
DmlNode* CastNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	CastNode* node = FB_NEW_POOL(pool) CastNode(pool);

	ItemInfo itemInfo;
	PAR_desc(tdbb, csb, &node->castDesc, &itemInfo);

	node->source = PAR_parse_value(tdbb, csb);

	if (itemInfo.isSpecial())
		node->itemInfo = FB_NEW_POOL(*tdbb->getDefaultPool()) ItemInfo(*tdbb->getDefaultPool(), itemInfo);

	if (itemInfo.explicitCollation)
	{
		CompilerScratch::Dependency dependency(obj_collation);
		dependency.number = INTL_TEXT_TYPE(node->castDesc);
		csb->csb_dependencies.push(dependency);
	}

	return node;
}

string CastNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlAlias);
	NODE_PRINT(printer, dsqlField);
	NODE_PRINT(printer, castDesc);
	NODE_PRINT(printer, source);
	NODE_PRINT(printer, itemInfo);

	return "CastNode";
}

ValueExprNode* CastNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	CastNode* node = FB_NEW_POOL(dsqlScratch->getPool()) CastNode(dsqlScratch->getPool());
	node->dsqlAlias = dsqlAlias;
	node->source = doDsqlPass(dsqlScratch, source);
	node->dsqlField = dsqlField;

	DDL_resolve_intl_type(dsqlScratch, node->dsqlField, NULL);
	node->setParameterType(dsqlScratch, NULL, false);

	MAKE_desc_from_field(&node->castDesc, node->dsqlField);
	MAKE_desc(dsqlScratch, &node->source->nodDesc, node->source);

	node->castDesc.dsc_flags = node->source->nodDesc.dsc_flags & DSC_nullable;

	return node;
}

void CastNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = dsqlAlias;
}

bool CastNode::setParameterType(DsqlCompilerScratch* /*dsqlScratch*/,
	const dsc* /*desc*/, bool /*forceVarChar*/)
{
	// ASF: Attention: CastNode::dsqlPass calls us with NULL node.

	ParameterNode* paramNode = nodeAs<ParameterNode>(source);

	if (paramNode)
	{
		dsql_par* parameter = paramNode->dsqlParameter;

		if (parameter)
		{
			parameter->par_node = source;
			MAKE_desc_from_field(&parameter->par_desc, dsqlField);
			if (!dsqlField->fullDomain)
				parameter->par_desc.setNullable(true);
			return true;
		}
	}

	return false;
}

// Generate BLR for a data-type cast operation.
void CastNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_cast);
	dsqlScratch->putDtype(dsqlField, true);
	GEN_expr(dsqlScratch, source);
}

void CastNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	*desc = castDesc;
}

void CastNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	// ASF: Commented out code here appears correct and makes the expression
	// "1 + NULLIF(NULL, 0)" (failing since v2.1) to work. While this is natural as others
	// nodes calling getDesc on sub nodes, it's causing some problem with contexts of
	// views.

	dsc desc1;
	////source->getDesc(tdbb, csb, &desc1);

	*desc = castDesc;

	if ((desc->dsc_dtype <= dtype_any_text && !desc->dsc_length) ||
		(desc->dsc_dtype == dtype_varying && desc->dsc_length <= sizeof(USHORT)))
	{
		// Remove this call if enable the one above.
		source->getDesc(tdbb, csb, &desc1);

		desc->dsc_length = DSC_string_length(&desc1);

		if (desc->dsc_dtype == dtype_cstring)
			desc->dsc_length++;
		else if (desc->dsc_dtype == dtype_varying)
			desc->dsc_length += sizeof(USHORT);
	}

	////if (desc1.isNull())
	////	desc->setNull();
}

ValueExprNode* CastNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	CastNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) CastNode(*tdbb->getDefaultPool());

	node->source = copier.copy(tdbb, source);
	node->castDesc = castDesc;
	node->itemInfo = itemInfo;

	return node;
}

bool CastNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const CastNode* o = nodeAs<CastNode>(other);
	fb_assert(o);

	return dsqlField == o->dsqlField;
}

bool CastNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const CastNode* const otherNode = nodeAs<CastNode>(other);
	fb_assert(otherNode);

	return DSC_EQUIV(&castDesc, &otherNode->castDesc, true);
}

ValueExprNode* CastNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass1(tdbb, csb);

	const USHORT ttype = INTL_TEXT_TYPE(castDesc);

	// Are we using a collation?
	if (TTYPE_TO_COLLATION(ttype) != 0)
	{
		CMP_post_resource(&csb->csb_resources, INTL_texttype_lookup(tdbb, ttype),
			Resource::rsc_collation, ttype);
	}

	return this;
}

ValueExprNode* CastNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

// Cast from one datatype to another.
dsc* CastNode::execute(thread_db* tdbb, jrd_req* request) const
{
	dsc* value = EVL_expr(tdbb, request, source);

	if (request->req_flags & req_null)
		value = NULL;

	// If validation is not required and the source value is either NULL
	// or already in the desired data type, simply return it "as is"

	if (!itemInfo && (!value || DSC_EQUIV(value, &castDesc, true)))
		return value;

	impure_value* impure = request->getImpure<impure_value>(impureOffset);

	impure->vlu_desc = castDesc;
	impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc;

	if (DTYPE_IS_TEXT(impure->vlu_desc.dsc_dtype))
	{
		USHORT length = DSC_string_length(&impure->vlu_desc);

		if (length <= 0 && value)
		{
			// cast is a subtype cast only

			length = DSC_string_length(value);

			if (impure->vlu_desc.dsc_dtype == dtype_cstring)
				++length;	// for NULL byte
			else if (impure->vlu_desc.dsc_dtype == dtype_varying)
				length += sizeof(USHORT);

			impure->vlu_desc.dsc_length = length;
		}

		length = impure->vlu_desc.dsc_length;

		// Allocate a string block of sufficient size.

		VaryingString* string = impure->vlu_string;

		if (string && string->str_length < length)
		{
			delete string;
			string = NULL;
		}

		if (!string)
		{
			string = impure->vlu_string = FB_NEW_RPT(*tdbb->getDefaultPool(), length) VaryingString();
			string->str_length = length;
		}

		impure->vlu_desc.dsc_address = string->str_data;
	}

	EVL_validate(tdbb, Item(Item::TYPE_CAST), itemInfo,
		value, value == NULL || (value->dsc_flags & DSC_null));

	if (!value)
		return NULL;

	if (DTYPE_IS_BLOB(value->dsc_dtype) || DTYPE_IS_BLOB(impure->vlu_desc.dsc_dtype))
		blb::move(tdbb, value, &impure->vlu_desc);
	else
		MOV_move(tdbb, value, &impure->vlu_desc);

	if (impure->vlu_desc.dsc_dtype == dtype_text)
		INTL_adjust_text_descriptor(tdbb, &impure->vlu_desc);

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<CoalesceNode> regCoalesceNode(blr_coalesce);

DmlNode* CoalesceNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	CoalesceNode* node = FB_NEW_POOL(pool) CoalesceNode(pool);
	node->args = PAR_args(tdbb, csb);
	return node;
}

string CoalesceNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, args);

	return "CoalesceNode";
}

ValueExprNode* CoalesceNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	CoalesceNode* node = FB_NEW_POOL(dsqlScratch->getPool()) CoalesceNode(
		dsqlScratch->getPool(), doDsqlPass(dsqlScratch, args));
	node->make(dsqlScratch, &node->nodDesc);	// Set descriptor for output node.
	node->setParameterType(dsqlScratch, &node->nodDesc, false);
	return node;
}

void CoalesceNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "COALESCE";
}

bool CoalesceNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool /*forceVarChar*/)
{
	bool ret = false;

	for (auto& item : args->items)
		ret |= PASS1_set_parameter_type(dsqlScratch, item, desc, false);

	return ret;
}

void CoalesceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsc desc;
	make(dsqlScratch, &desc);
	dsqlScratch->appendUChar(blr_cast);
	GEN_descriptor(dsqlScratch, &desc, true);

	dsqlScratch->appendUChar(blr_coalesce);
	dsqlScratch->appendUChar(args->items.getCount());

	for (auto& item : args->items)
		GEN_expr(dsqlScratch, item);
}

void CoalesceNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc_from_list(dsqlScratch, desc, args, "COALESCE");
}

void CoalesceNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	Array<dsc> descs;
	descs.resize(args->items.getCount());

	unsigned i = 0;
	Array<const dsc*> descPtrs;
	descPtrs.resize(args->items.getCount());

	for (auto& item : args->items)
	{
		item->getDesc(tdbb, csb, &descs[i]);
		descPtrs[i] = &descs[i];
		++i;
	}

	DataTypeUtil(tdbb).makeFromList(desc, "COALESCE", descPtrs.getCount(), descPtrs.begin());
}

ValueExprNode* CoalesceNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	CoalesceNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) CoalesceNode(*tdbb->getDefaultPool());
	node->args = copier.copy(tdbb, args);
	return node;
}

ValueExprNode* CoalesceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* CoalesceNode::execute(thread_db* tdbb, jrd_req* request) const
{
	for (auto& item : args->items)
	{
		dsc* desc = EVL_expr(tdbb, request, item);

		if (desc && !(request->req_flags & req_null))
			return desc;
	}

	return NULL;
}


//--------------------


CollateNode::CollateNode(MemoryPool& pool, ValueExprNode* aArg, const Firebird::MetaName& aCollation)
	: TypedNode<ValueExprNode, ExprNode::TYPE_COLLATE>(pool),
	  arg(aArg),
	  collation(pool, aCollation)
{
}

string CollateNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, arg);
	NODE_PRINT(printer, collation);

	return "CollateNode";
}

// Turn a collate clause into a cast clause.
// If the source is not already text, report an error. (SQL 92: Section 13.1, pg 308, item 11).
ValueExprNode* CollateNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	ValueExprNode* nod = doDsqlPass(dsqlScratch, arg);
	return pass1Collate(dsqlScratch, nod, collation);
}

ValueExprNode* CollateNode::pass1Collate(DsqlCompilerScratch* dsqlScratch, ValueExprNode* input,
	const MetaName& collation)
{
	thread_db* tdbb = JRD_get_thread_data();
	MemoryPool& pool = *tdbb->getDefaultPool();

	dsql_fld* field = FB_NEW_POOL(pool) dsql_fld(pool);
	CastNode* castNode = FB_NEW_POOL(pool) CastNode(pool, input, field);

	MAKE_desc(dsqlScratch, &input->nodDesc, input);

	if (input->nodDesc.dsc_dtype <= dtype_any_text ||
		(input->nodDesc.dsc_dtype == dtype_blob && input->nodDesc.dsc_sub_type == isc_blob_text))
	{
		assignFieldDtypeFromDsc(field, &input->nodDesc);
		field->charLength = 0;
	}
	else
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
				  Arg::Gds(isc_dsql_datatype_err) <<
				  Arg::Gds(isc_collation_requires_text));
	}

	DDL_resolve_intl_type(dsqlScratch, field, collation);
	MAKE_desc_from_field(&castNode->castDesc, field);

	return castNode;
}

// Set a field's descriptor from a DSC.
// (If dsql_fld* is ever redefined this can be removed)
void CollateNode::assignFieldDtypeFromDsc(dsql_fld* field, const dsc* desc)
{
	DEV_BLKCHK(field, dsql_type_fld);

	field->dtype = desc->dsc_dtype;
	field->scale = desc->dsc_scale;
	field->subType = desc->dsc_sub_type;
	field->length = desc->dsc_length;

	if (desc->dsc_dtype <= dtype_any_text)
	{
		field->collationId = DSC_GET_COLLATE(desc);
		field->charSetId = DSC_GET_CHARSET(desc);
	}
	else if (desc->dsc_dtype == dtype_blob)
	{
		field->charSetId = desc->dsc_scale;
		field->collationId = desc->dsc_flags >> 8;
	}

	if (desc->dsc_flags & DSC_nullable)
		field->flags |= FLD_nullable;
}


//--------------------


static RegisterNode<ConcatenateNode> regConcatenateNode(blr_concatenate);

ConcatenateNode::ConcatenateNode(MemoryPool& pool, ValueExprNode* aArg1, ValueExprNode* aArg2)
	: TypedNode<ValueExprNode, ExprNode::TYPE_CONCATENATE>(pool),
	  arg1(aArg1),
	  arg2(aArg2)
{
}

DmlNode* ConcatenateNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	ConcatenateNode* node = FB_NEW_POOL(pool) ConcatenateNode(pool);
	node->arg1 = PAR_parse_value(tdbb, csb);
	node->arg2 = PAR_parse_value(tdbb, csb);
	return node;
}

string ConcatenateNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, arg1);
	NODE_PRINT(printer, arg2);

	return "ConcatenateNode";
}

void ConcatenateNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "CONCATENATION";
}

bool ConcatenateNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, arg1, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, arg2, desc, forceVarChar);
}

void ConcatenateNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_concatenate);
	GEN_expr(dsqlScratch, arg1);
	GEN_expr(dsqlScratch, arg2);
}

void ConcatenateNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	dsc desc1, desc2;

	MAKE_desc(dsqlScratch, &desc1, arg1);
	MAKE_desc(dsqlScratch, &desc2, arg2);

	if (desc1.isNull())
	{
		desc1.makeText(0, desc2.getTextType());
		desc1.setNull();
	}

	if (desc2.isNull())
	{
		desc2.makeText(0, desc1.getTextType());
		desc2.setNull();
	}

	DSqlDataTypeUtil(dsqlScratch).makeConcatenate(desc, &desc1, &desc2);
}

void ConcatenateNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	dsc desc1, desc2;

	arg1->getDesc(tdbb, csb, &desc1);
	arg2->getDesc(tdbb, csb, &desc2);

	DataTypeUtil(tdbb).makeConcatenate(desc, &desc1, &desc2);
}

ValueExprNode* ConcatenateNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	ConcatenateNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) ConcatenateNode(*tdbb->getDefaultPool());
	node->arg1 = copier.copy(tdbb, arg1);
	node->arg2 = copier.copy(tdbb, arg2);
	return node;
}

ValueExprNode* ConcatenateNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* ConcatenateNode::execute(thread_db* tdbb, jrd_req* request) const
{
	const dsc* value1 = EVL_expr(tdbb, request, arg1);
	const ULONG flags = request->req_flags;
	const dsc* value2 = EVL_expr(tdbb, request, arg2);

	// restore saved NULL state

	if (flags & req_null)
		request->req_flags |= req_null;

	if (request->req_flags & req_null)
		return NULL;

	impure_value* impure = request->getImpure<impure_value>(impureOffset);
	dsc desc;

	if (value1->dsc_dtype == dtype_dbkey && value2->dsc_dtype == dtype_dbkey)
	{
		if ((ULONG) value1->dsc_length + (ULONG) value2->dsc_length > MAX_STR_SIZE)
		{
			ERR_post(Arg::Gds(isc_concat_overflow));
			return NULL;
		}

		desc.dsc_dtype = dtype_dbkey;
		desc.dsc_length = value1->dsc_length + value2->dsc_length;
		desc.dsc_address = NULL;

		VaryingString* string = NULL;
		if (value1->dsc_address == impure->vlu_desc.dsc_address ||
			value2->dsc_address == impure->vlu_desc.dsc_address)
		{
			string = impure->vlu_string;
			impure->vlu_string = NULL;
		}

		EVL_make_value(tdbb, &desc, impure);
		UCHAR* p = impure->vlu_desc.dsc_address;

		memcpy(p, value1->dsc_address, value1->dsc_length);
		p += value1->dsc_length;
		memcpy(p, value2->dsc_address, value2->dsc_length);

		delete string;

		return &impure->vlu_desc;
	}

	DataTypeUtil(tdbb).makeConcatenate(&desc, value1, value2);

	// Both values are present; build the concatenation

	MoveBuffer temp1;
	UCHAR* address1 = NULL;
	USHORT length1 = 0;

	if (!value1->isBlob())
		length1 = MOV_make_string2(tdbb, value1, desc.getTextType(), &address1, temp1);

	MoveBuffer temp2;
	UCHAR* address2 = NULL;
	USHORT length2 = 0;

	if (!value2->isBlob())
		length2 = MOV_make_string2(tdbb, value2, desc.getTextType(), &address2, temp2);

	if (address1 && address2)
	{
		fb_assert(desc.dsc_dtype == dtype_varying);

		if ((ULONG) length1 + (ULONG) length2 > MAX_STR_SIZE)
		{
			ERR_post(Arg::Gds(isc_concat_overflow));
			return NULL;
		}

		desc.dsc_dtype = dtype_text;
		desc.dsc_length = length1 + length2;
		desc.dsc_address = NULL;

		VaryingString* string = NULL;
		if (value1->dsc_address == impure->vlu_desc.dsc_address ||
			value2->dsc_address == impure->vlu_desc.dsc_address)
		{
			string = impure->vlu_string;
			impure->vlu_string = NULL;
		}

		EVL_make_value(tdbb, &desc, impure);
		UCHAR* p = impure->vlu_desc.dsc_address;

		if (length1)
		{
			memcpy(p, address1, length1);
			p += length1;
		}

		if (length2)
			memcpy(p, address2, length2);

		delete string;
	}
	else
	{
		fb_assert(desc.isBlob());

		desc.dsc_address = (UCHAR*)&impure->vlu_misc.vlu_bid;

		blb* newBlob = blb::create(tdbb, tdbb->getRequest()->req_transaction,
			&impure->vlu_misc.vlu_bid);

		HalfStaticArray<UCHAR, BUFFER_SMALL> buffer;

		if (address1)
			newBlob->BLB_put_data(tdbb, address1, length1);	// first value is not a blob
		else
		{
			UCharBuffer bpb;
			BLB_gen_bpb_from_descs(value1, &desc, bpb);

			blb* blob = blb::open2(tdbb, tdbb->getRequest()->req_transaction,
				reinterpret_cast<bid*>(value1->dsc_address), bpb.getCount(), bpb.begin());

			while (!(blob->blb_flags & BLB_eof))
			{
				SLONG len = blob->BLB_get_data(tdbb, buffer.begin(), buffer.getCapacity(), false);

				if (len)
					newBlob->BLB_put_data(tdbb, buffer.begin(), len);
			}

			blob->BLB_close(tdbb);
		}

		if (address2)
			newBlob->BLB_put_data(tdbb, address2, length2);	// second value is not a blob
		else
		{
			UCharBuffer bpb;
			BLB_gen_bpb_from_descs(value2, &desc, bpb);

			blb* blob = blb::open2(tdbb, tdbb->getRequest()->req_transaction,
				reinterpret_cast<bid*>(value2->dsc_address), bpb.getCount(), bpb.begin());

			while (!(blob->blb_flags & BLB_eof))
			{
				SLONG len = blob->BLB_get_data(tdbb, buffer.begin(), buffer.getCapacity(), false);

				if (len)
					newBlob->BLB_put_data(tdbb, buffer.begin(), len);
			}

			blob->BLB_close(tdbb);
		}

		newBlob->BLB_close(tdbb);

		EVL_make_value(tdbb, &desc, impure);
	}

	return &impure->vlu_desc;
}

ValueExprNode* ConcatenateNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return FB_NEW_POOL(dsqlScratch->getPool()) ConcatenateNode(dsqlScratch->getPool(),
		doDsqlPass(dsqlScratch, arg1), doDsqlPass(dsqlScratch, arg2));
}


//--------------------


static RegisterNode<CurrentDateNode> regCurrentDateNode(blr_current_date);

DmlNode* CurrentDateNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* /*csb*/,
								const UCHAR /*blrOp*/)
{
	return FB_NEW_POOL(pool) CurrentDateNode(pool);
}

string CurrentDateNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	return "CurrentDateNode";
}

void CurrentDateNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "CURRENT_DATE";
}

void CurrentDateNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_current_date);
}

void CurrentDateNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	desc->dsc_dtype = dtype_sql_date;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

void CurrentDateNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->dsc_dtype = dtype_sql_date;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

ValueExprNode* CurrentDateNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) CurrentDateNode(*tdbb->getDefaultPool());
}

ValueExprNode* CurrentDateNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* CurrentDateNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	// Use the request timestamp.
	fb_assert(!request->req_timestamp_utc.isEmpty());

	ISC_TIMESTAMP_TZ timeStampTz;
	timeStampTz.utc_timestamp = request->req_timestamp_utc.value();
	timeStampTz.time_zone = TimeZoneUtil::UTC_ZONE;

	impure->vlu_misc.vlu_sql_date = TimeZoneUtil::timeStampTzToTimeStamp(
		timeStampTz, request->req_attachment->att_current_timezone).timestamp_date;

	memset(&impure->vlu_desc, 0, sizeof(impure->vlu_desc));
	impure->vlu_desc.dsc_dtype = dtype_sql_date;
	impure->vlu_desc.dsc_length = type_lengths[dtype_sql_date];
	impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_sql_date;

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<CurrentTimeNode> regCurrentTimeNode(blr_current_time);
static RegisterNode<CurrentTimeNode> regCurrentTimeNode2(blr_current_time2);

DmlNode* CurrentTimeNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	unsigned precision = DEFAULT_TIME_PRECISION;

	fb_assert(blrOp == blr_current_time || blrOp == blr_current_time2);

	if (blrOp == blr_current_time2)
	{
		precision = csb->csb_blr_reader.getByte();

		if (precision > MAX_TIME_PRECISION)
			ERR_post(Arg::Gds(isc_invalid_time_precision) << Arg::Num(MAX_TIME_PRECISION));
	}

	return FB_NEW_POOL(pool) CurrentTimeNode(pool, precision);
}

string CurrentTimeNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, precision);

	return "CurrentTimeNode";
}

void CurrentTimeNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "CURRENT_TIME";
}

void CurrentTimeNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (precision == DEFAULT_TIME_PRECISION)
		dsqlScratch->appendUChar(blr_current_time);
	else
	{
		dsqlScratch->appendUChar(blr_current_time2);
		dsqlScratch->appendUChar(precision);
	}
}

void CurrentTimeNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	desc->dsc_dtype = dtype_sql_time_tz;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

void CurrentTimeNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->dsc_dtype = dtype_sql_time_tz;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

ValueExprNode* CurrentTimeNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) CurrentTimeNode(*tdbb->getDefaultPool(), precision);
}

ValueExprNode* CurrentTimeNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

ValueExprNode* CurrentTimeNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	if (precision > MAX_TIME_PRECISION)
		ERRD_post(Arg::Gds(isc_invalid_time_precision) << Arg::Num(MAX_TIME_PRECISION));

	return this;
}

dsc* CurrentTimeNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	// Use the request timestamp.
	fb_assert(!request->req_timestamp_utc.isEmpty());

	ISC_TIME time = request->req_timestamp_utc.value().timestamp_time;
	TimeStamp::round_time(time, precision);

	impure->vlu_desc.dsc_dtype = dtype_sql_time_tz;
	impure->vlu_desc.dsc_length = type_lengths[dtype_sql_time_tz];
	impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_sql_time_tz;

	impure->vlu_misc.vlu_sql_time_tz.utc_time = time;
	impure->vlu_misc.vlu_sql_time_tz.time_zone = TimeZoneUtil::getSystemTimeZone();

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<CurrentTimeStampNode> regCurrentTimeStampNode(blr_current_timestamp);
static RegisterNode<CurrentTimeStampNode> regCurrentTimeStampNode2(blr_current_timestamp2);

DmlNode* CurrentTimeStampNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb,
	const UCHAR blrOp)
{
	unsigned precision = DEFAULT_TIMESTAMP_PRECISION;

	fb_assert(blrOp == blr_current_timestamp || blrOp == blr_current_timestamp2);

	if (blrOp == blr_current_timestamp2)
	{
		precision = csb->csb_blr_reader.getByte();

		if (precision > MAX_TIME_PRECISION)
			ERR_post(Arg::Gds(isc_invalid_time_precision) << Arg::Num(MAX_TIME_PRECISION));
	}

	return FB_NEW_POOL(pool) CurrentTimeStampNode(pool, precision);
}

string CurrentTimeStampNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, precision);

	return "CurrentTimeStampNode";
}

void CurrentTimeStampNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "CURRENT_TIMESTAMP";
}

void CurrentTimeStampNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (precision == DEFAULT_TIMESTAMP_PRECISION)
		dsqlScratch->appendUChar(blr_current_timestamp);
	else
	{
		dsqlScratch->appendUChar(blr_current_timestamp2);
		dsqlScratch->appendUChar(precision);
	}
}

void CurrentTimeStampNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	desc->dsc_dtype = dtype_timestamp_tz;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

void CurrentTimeStampNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->dsc_dtype = dtype_timestamp_tz;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

ValueExprNode* CurrentTimeStampNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) CurrentTimeStampNode(*tdbb->getDefaultPool(), precision);
}

ValueExprNode* CurrentTimeStampNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

ValueExprNode* CurrentTimeStampNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	if (precision > MAX_TIME_PRECISION)
		ERRD_post(Arg::Gds(isc_invalid_time_precision) << Arg::Num(MAX_TIME_PRECISION));

	return this;
}

dsc* CurrentTimeStampNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	// Use the request timestamp.
	fb_assert(!request->req_timestamp_utc.isEmpty());
	ISC_TIMESTAMP encTimes = request->req_timestamp_utc.value();

	memset(&impure->vlu_desc, 0, sizeof(impure->vlu_desc));
	impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_timestamp_tz;

	TimeStamp::round_time(encTimes.timestamp_time, precision);

	impure->vlu_desc.dsc_dtype = dtype_timestamp_tz;
	impure->vlu_desc.dsc_length = type_lengths[dtype_timestamp_tz];

	impure->vlu_misc.vlu_timestamp_tz.utc_timestamp = encTimes;
	impure->vlu_misc.vlu_timestamp_tz.time_zone = tdbb->getAttachment()->att_current_timezone;

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<CurrentRoleNode> regCurrentRoleNode(blr_current_role);

DmlNode* CurrentRoleNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* /*csb*/,
								const UCHAR /*blrOp*/)
{
	return FB_NEW_POOL(pool) CurrentRoleNode(pool);
}

string CurrentRoleNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	return "CurrentRoleNode";
}

void CurrentRoleNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "ROLE";
}

void CurrentRoleNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_current_role);
}

void CurrentRoleNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	desc->dsc_dtype = dtype_varying;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_ttype() = ttype_metadata;
	desc->dsc_length = USERNAME_LENGTH + sizeof(USHORT);
}

void CurrentRoleNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->dsc_dtype = dtype_text;
	desc->dsc_ttype() = ttype_metadata;
	desc->dsc_length = USERNAME_LENGTH;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
}

ValueExprNode* CurrentRoleNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) CurrentRoleNode(*tdbb->getDefaultPool());
}

ValueExprNode* CurrentRoleNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

// CVC: Current role will get a validated role; IE one that exists.
dsc* CurrentRoleNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	impure->vlu_desc.dsc_dtype = dtype_text;
	impure->vlu_desc.dsc_sub_type = 0;
	impure->vlu_desc.dsc_scale = 0;
	impure->vlu_desc.setTextType(ttype_metadata);
	const char* curRole = NULL;

	if (tdbb->getAttachment()->att_user)
	{
		curRole = tdbb->getAttachment()->att_user->getSqlRole().c_str();
		impure->vlu_desc.dsc_address = reinterpret_cast<UCHAR*>(const_cast<char*>(curRole));
	}

	if (curRole)
		impure->vlu_desc.dsc_length = static_cast<USHORT>(strlen(curRole));
	else
		impure->vlu_desc.dsc_length = 0;

	return &impure->vlu_desc;
}

ValueExprNode* CurrentRoleNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return FB_NEW_POOL(dsqlScratch->getPool()) CurrentRoleNode(dsqlScratch->getPool());
}


//--------------------


static RegisterNode<CurrentUserNode> regCurrentUserNode(blr_user_name);

DmlNode* CurrentUserNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* /*csb*/,
								const UCHAR /*blrOp*/)
{
	return FB_NEW_POOL(pool) CurrentUserNode(pool);
}

string CurrentUserNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	return "CurrentUserNode";
}

void CurrentUserNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "USER";
}

void CurrentUserNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_user_name);
}

void CurrentUserNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	desc->dsc_dtype = dtype_varying;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_ttype() = ttype_metadata;
	desc->dsc_length = USERNAME_LENGTH + sizeof(USHORT);
}

void CurrentUserNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->dsc_dtype = dtype_text;
	desc->dsc_ttype() = ttype_metadata;
	desc->dsc_length = USERNAME_LENGTH;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
}

ValueExprNode* CurrentUserNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) CurrentUserNode(*tdbb->getDefaultPool());
}

ValueExprNode* CurrentUserNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* CurrentUserNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	impure->vlu_desc.dsc_dtype = dtype_text;
	impure->vlu_desc.dsc_sub_type = 0;
	impure->vlu_desc.dsc_scale = 0;
	impure->vlu_desc.setTextType(ttype_metadata);
	const char* curUser = NULL;

	if (tdbb->getAttachment()->att_user)
	{
		curUser = tdbb->getAttachment()->att_user->getUserName().c_str();
		impure->vlu_desc.dsc_address = reinterpret_cast<UCHAR*>(const_cast<char*>(curUser));
	}

	if (curUser)
		impure->vlu_desc.dsc_length = static_cast<USHORT>(strlen(curUser));
	else
		impure->vlu_desc.dsc_length = 0;

	return &impure->vlu_desc;
}

ValueExprNode* CurrentUserNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return FB_NEW_POOL(dsqlScratch->getPool()) CurrentUserNode(dsqlScratch->getPool());
}


//--------------------


static RegisterNode<DecodeNode> regDecodeNode(blr_decode);

DmlNode* DecodeNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	DecodeNode* node = FB_NEW_POOL(pool) DecodeNode(pool);
	node->test = PAR_parse_value(tdbb, csb);
	node->conditions = PAR_args(tdbb, csb);
	node->values = PAR_args(tdbb, csb);
	return node;
}

string DecodeNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, label);
	NODE_PRINT(printer, test);
	NODE_PRINT(printer, conditions);
	NODE_PRINT(printer, values);

	return "DecodeNode";
}

ValueExprNode* DecodeNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	DecodeNode* node = FB_NEW_POOL(dsqlScratch->getPool()) DecodeNode(dsqlScratch->getPool(),
		doDsqlPass(dsqlScratch, test), doDsqlPass(dsqlScratch, conditions), doDsqlPass(dsqlScratch, values));
	node->label = label;
	node->make(dsqlScratch, &node->nodDesc);	// Set descriptor for output node.
	node->setParameterType(dsqlScratch, &node->nodDesc, false);

	// Workaround for DECODE/CASE supporting only 255 items - see CORE-5366.

	const static unsigned MAX_COUNT = 254;

	if (node->values->items.getCount() > MAX_COUNT)
	{
		NestValueArray conditions(node->conditions->items.getCount() - MAX_COUNT);
		conditions.push(node->conditions->items.begin() + MAX_COUNT, conditions.getCapacity());
		node->conditions->items.shrink(MAX_COUNT);

		NestValueArray values(node->values->items.getCount() - MAX_COUNT);
		values.push(node->values->items.begin() + MAX_COUNT, values.getCapacity());
		node->values->items.shrink(MAX_COUNT + 1);

		DecodeNode* innerNode = node;
		bool hasElse = values.getCount() != conditions.getCount();
		unsigned index = 0;
		bool last;

		do
		{
			unsigned count = conditions.getCount() - index;
			last = count <= MAX_COUNT;
			unsigned valuesCount = MIN(MAX_COUNT, count) + (last && hasElse ? 1 : 0);
			unsigned conditionsCount = MIN(MAX_COUNT, count);

			if (last && conditionsCount == 0)
			{
				fb_assert(valuesCount == 1);
				innerNode->values->items.back() = values[index];
			}
			else
			{
				DecodeNode* newNode = FB_NEW_POOL(dsqlScratch->getPool()) DecodeNode(dsqlScratch->getPool(),
					doDsqlPass(dsqlScratch, test),
					FB_NEW_POOL(dsqlScratch->getPool()) ValueListNode(dsqlScratch->getPool(), conditionsCount),
					FB_NEW_POOL(dsqlScratch->getPool()) ValueListNode(dsqlScratch->getPool(),
						valuesCount + (last ? 0 : 1)));

				newNode->conditions->items.assign(conditions.begin() + index, conditionsCount);
				newNode->values->items.assign(values.begin() + index, valuesCount);

				if (!last)
					newNode->values->items.push(NULL);

				innerNode->values->items.back() = newNode;
				innerNode = newNode;

				index += conditionsCount;
			}
		} while (!last);
	}

	return node;
}

void DecodeNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = label;
}

bool DecodeNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool /*forceVarChar*/)
{
	// Check if there is a parameter in the test/conditions.
	bool setParameters = nodeIs<ParameterNode>(test);

	if (!setParameters)
	{
		for (auto& condition : conditions->items)
		{
			if (nodeIs<ParameterNode>(condition))
			{
				setParameters = true;
				break;
			}
		}
	}

	if (setParameters)
	{
		// Build list to make describe information for the test and conditions expressions.
		AutoPtr<ValueListNode> node1(FB_NEW_POOL(dsqlScratch->getPool()) ValueListNode(dsqlScratch->getPool(),
			conditions->items.getCount() + 1));

		dsc node1Desc;
		node1Desc.clear();

		unsigned i = 0;

		node1->items[i++] = test;

		for (auto& condition : conditions->items)
		{
			node1->items[i] = condition;
			++i;
		}

		MAKE_desc_from_list(dsqlScratch, &node1Desc, node1, label.c_str());

		if (!node1Desc.isUnknown())
		{
			// Set parameter describe information.
			PASS1_set_parameter_type(dsqlScratch, test, &node1Desc, false);

			for (auto& condition : conditions->items)
			{
				PASS1_set_parameter_type(dsqlScratch, condition, &node1Desc, false);
			}
		}
	}

	bool ret = false;

	for (auto& value : values->items)
		ret |= PASS1_set_parameter_type(dsqlScratch, value, desc, false);

	return ret;
}

void DecodeNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_decode);
	GEN_expr(dsqlScratch, test);

	dsqlScratch->appendUChar(conditions->items.getCount());

	for (auto& condition : conditions->items)
	{
		condition->genBlr(dsqlScratch);
	}

	dsqlScratch->appendUChar(values->items.getCount());

	for (auto& value : values->items)
		value->genBlr(dsqlScratch);
}

void DecodeNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc_from_list(dsqlScratch, desc, values, label.c_str());
	desc->setNullable(true);
}

void DecodeNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	Array<dsc> descs;
	descs.resize(values->items.getCount());

	unsigned i = 0;
	Array<const dsc*> descPtrs;
	descPtrs.resize(values->items.getCount());

	for (auto& value : values->items)
	{
		value->getDesc(tdbb, csb, &descs[i]);
		descPtrs[i] = &descs[i];
		++i;
	}

	DataTypeUtil(tdbb).makeFromList(desc, label.c_str(), descPtrs.getCount(), descPtrs.begin());

	desc->setNullable(true);
}

ValueExprNode* DecodeNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	DecodeNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) DecodeNode(*tdbb->getDefaultPool());
	node->test = copier.copy(tdbb, test);
	node->conditions = copier.copy(tdbb, conditions);
	node->values = copier.copy(tdbb, values);
	return node;
}

ValueExprNode* DecodeNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* DecodeNode::execute(thread_db* tdbb, jrd_req* request) const
{
	dsc* testDesc = EVL_expr(tdbb, request, test);

	// The comparisons are done with "equal" operator semantics, so if the test value is
	// NULL we have nothing to compare.
	if (testDesc && !(request->req_flags & req_null))
	{
		const NestConst<ValueExprNode>* valuesPtr = values->items.begin();

		for (auto& condition : conditions->items)
		{
			dsc* desc = EVL_expr(tdbb, request, condition);

			if (desc && !(request->req_flags & req_null) && MOV_compare(tdbb, testDesc, desc) == 0)
				return EVL_expr(tdbb, request, *valuesPtr);

			++valuesPtr;
		}
	}

	if (values->items.getCount() > conditions->items.getCount())
		return EVL_expr(tdbb, request, values->items.back());

	return NULL;
}


//--------------------


static RegisterNode<DefaultNode> regDefaultNode(blr_default);

DefaultNode::DefaultNode(MemoryPool& pool, const Firebird::MetaName& aRelationName,
		const Firebird::MetaName& aFieldName)
	: DsqlNode<DefaultNode, ExprNode::TYPE_DEFAULT>(pool),
	  relationName(aRelationName),
	  fieldName(aFieldName),
	  field(NULL)
{
}

DmlNode* DefaultNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	MetaName relationName, fieldName;
	csb->csb_blr_reader.getMetaName(relationName);
	csb->csb_blr_reader.getMetaName(fieldName);

	CompilerScratch::Dependency dependency(obj_relation);
	dependency.relation = MET_lookup_relation(tdbb, relationName);
	dependency.subName = FB_NEW_POOL(pool) MetaName(fieldName);
	csb->csb_dependencies.push(dependency);

	jrd_fld* fld = NULL;

	while (true)
	{
		jrd_rel* relation = MET_lookup_relation(tdbb, relationName);

		if (relation && relation->rel_fields)
		{
			int fieldId = MET_lookup_field(tdbb, relation, fieldName);

			if (fieldId >= 0)
			{
				fld = (*relation->rel_fields)[fieldId];

				if (fld)
				{
					if (fld->fld_source_rel_field.first.hasData())
					{
						relationName = fld->fld_source_rel_field.first;
						fieldName = fld->fld_source_rel_field.second;
						continue;
					}
					else
					{
						DefaultNode* node = FB_NEW_POOL(pool) DefaultNode(pool, relationName, fieldName);
						node->field = fld;
						return node;
					}
				}
			}
		}

		return FB_NEW_POOL(pool) NullNode(pool);
	}
}

ValueExprNode* DefaultNode::createFromField(thread_db* tdbb, CompilerScratch* csb,
	StreamType* map, jrd_fld* fld)
{
	if (fld->fld_generator_name.hasData())
	{
		// Make a (next value for <generator name>) expression.

		GenIdNode* const genNode = FB_NEW_POOL(csb->csb_pool) GenIdNode(
			csb->csb_pool, (csb->blrVersion == 4), fld->fld_generator_name, NULL, true, true);

		bool sysGen = false;
		if (!MET_load_generator(tdbb, genNode->generator, &sysGen, &genNode->step))
			PAR_error(csb, Arg::Gds(isc_gennotdef) << Arg::Str(fld->fld_generator_name));

		if (sysGen)
			PAR_error(csb, Arg::Gds(isc_cant_modify_sysobj) << "generator" << fld->fld_generator_name);

		return genNode;
	}
	else if (fld->fld_default_value)
	{
		StreamMap localMap;
		if (!map)
			map = localMap.getBuffer(STREAM_MAP_LENGTH);

		return NodeCopier(csb->csb_pool, csb, map).copy(tdbb, fld->fld_default_value);
	}
	else
		return FB_NEW_POOL(csb->csb_pool) NullNode(csb->csb_pool);
}

string DefaultNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, relationName);
	NODE_PRINT(printer, fieldName);

	return "DefaultNode";
}

ValueExprNode* DefaultNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	DefaultNode* node = FB_NEW_POOL(dsqlScratch->getPool()) DefaultNode(dsqlScratch->getPool(),
		relationName, fieldName);
	return node;
}

void DefaultNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "DEFAULT";
}

bool DefaultNode::setParameterType(DsqlCompilerScratch* /*dsqlScratch*/, const dsc* /*desc*/, bool /*forceVarChar*/)
{
	return false;
}

void DefaultNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_default);
	dsqlScratch->appendMetaString(relationName.c_str());
	dsqlScratch->appendMetaString(fieldName.c_str());
}

void DefaultNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* /*desc*/)
{
}

bool DefaultNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const DefaultNode* o = nodeAs<DefaultNode>(other);
	fb_assert(o);

	return relationName == o->relationName && fieldName == o->fieldName;
}

ValueExprNode* DefaultNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode* node = createFromField(tdbb, csb, NULL, field);
	doPass1(tdbb, csb, &node);
	return node;
}


//--------------------


static RegisterNode<DerivedExprNode> regDerivedExprNode(blr_derived_expr);

DmlNode* DerivedExprNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	DerivedExprNode* node = FB_NEW_POOL(pool) DerivedExprNode(pool);

	// CVC: bottleneck
	const StreamType streamCount = csb->csb_blr_reader.getByte();

	for (StreamType i = 0; i < streamCount; ++i)
	{
		const USHORT n = csb->csb_blr_reader.getByte();
		node->internalStreamList.add(csb->csb_rpt[n].csb_stream);
	}

	node->arg = PAR_parse_value(tdbb, csb);

	return node;
}

void DerivedExprNode::collectStreams(CompilerScratch* csb, SortedStreamList& streamList) const
{
	arg->collectStreams(csb, streamList);

	for (const auto i : internalStreamList)
	{
		if (!streamList.exist(i))
			streamList.add(i);
	}
}

bool DerivedExprNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	if (!arg->computable(csb, stream, allowOnlyCurrentStream))
		return false;

	SortedStreamList argStreams;
	arg->collectStreams(csb, argStreams);

	for (const auto n : internalStreamList)
	{
		if (argStreams.exist(n))
		{
			// We've already checked computability of the argument,
			// so any stream it refers to is known to be active.
			continue;
		}

		if (allowOnlyCurrentStream)
		{
			if (n != stream && !(csb->csb_rpt[n].csb_flags & csb_sub_stream))
				return false;
		}
		else
		{
			if (n == stream)
				return false;
		}

		if (!(csb->csb_rpt[n].csb_flags & csb_active))
			return false;
	}

	return true;
}

void DerivedExprNode::findDependentFromStreams(const OptimizerRetrieval* optRet,
	SortedStreamList* streamList)
{
	arg->findDependentFromStreams(optRet, streamList);

	for (const auto derivedStream : internalStreamList)
	{
		if (derivedStream != optRet->stream &&
			(optRet->csb->csb_rpt[derivedStream].csb_flags & csb_active))
		{
			if (!streamList->exist(derivedStream))
				streamList->add(derivedStream);
		}
	}
}

void DerivedExprNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	arg->getDesc(tdbb, csb, desc);
}

ValueExprNode* DerivedExprNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	DerivedExprNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) DerivedExprNode(*tdbb->getDefaultPool());
	node->arg = copier.copy(tdbb, arg);
	node->internalStreamList = internalStreamList;

	if (copier.remap)
	{
#ifdef CMP_DEBUG
		csb->dump("remap nod_derived_expr:\n");
		for (const auto i : node->internalStreamList)
			csb->dump("\t%d: %d -> %d\n", i, i, copier.remap[i]);
#endif

		for (auto& i : node->internalStreamList)
			i = copier.remap[i];
	}

	fb_assert(!cursorNumber.specified);

	return node;
}

ValueExprNode* DerivedExprNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass1(tdbb, csb);

#ifdef CMP_DEBUG
	csb->dump("expand nod_derived_expr:");
	for (const auto i : internalStreamList)
		csb->dump(" %d", i);
	csb->dump("\n");
#endif

	SortedStreamList newStreams;

	for (const auto i : internalStreamList)
	{
		markVariant(csb, i);
		expandViewStreams(csb, i, newStreams);
	}

#ifdef CMP_DEBUG
	for (const auto i : newStreams)
		csb->dump(" %d", i);
	csb->dump("\n");
#endif

	internalStreamList.assign(newStreams.begin(), newStreams.getCount());

	return this;
}

ValueExprNode* DerivedExprNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	// As all streams belongs to the same cursor, we use only the first.
	cursorNumber = csb->csb_rpt[internalStreamList[0]].csb_cursor_number;

	return this;
}

dsc* DerivedExprNode::execute(thread_db* tdbb, jrd_req* request) const
{
	if (cursorNumber.specified)
		request->req_cursors[cursorNumber.value]->checkState(request);

	dsc* value = NULL;

	for (const auto i : internalStreamList)
	{
		if (request->req_rpb[i].rpb_number.isValid())
		{
			value = EVL_expr(tdbb, request, arg);

			if (request->req_flags & req_null)
				value = NULL;

			break;
		}
	}

	return value;
}


//--------------------


string DomainValidationNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, domDesc);

	return "DomainValidationNode";
}

ValueExprNode* DomainValidationNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (dsqlScratch->domainValue.isUnknown())
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
				  Arg::Gds(isc_dsql_domain_err));
	}

	DomainValidationNode* node = FB_NEW_POOL(dsqlScratch->getPool()) DomainValidationNode(dsqlScratch->getPool());
	node->domDesc = dsqlScratch->domainValue;

	return node;
}

void DomainValidationNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_fid);
	dsqlScratch->appendUChar(0);		// context
	dsqlScratch->appendUShort(0);		// field id
}

void DomainValidationNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	*desc = domDesc;
}

void DomainValidationNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	*desc = domDesc;
}

ValueExprNode* DomainValidationNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	DomainValidationNode* node =
		FB_NEW_POOL(*tdbb->getDefaultPool()) DomainValidationNode(*tdbb->getDefaultPool());
	node->domDesc = domDesc;
	return node;
}

ValueExprNode* DomainValidationNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* DomainValidationNode::execute(thread_db* /*tdbb*/, jrd_req* request) const
{
	if (request->req_domain_validation == NULL ||
		(request->req_domain_validation->dsc_flags & DSC_null))
	{
		return NULL;
	}

	return request->req_domain_validation;
}


//--------------------


static RegisterNode<ExtractNode> regExtractNode(blr_extract);

ExtractNode::ExtractNode(MemoryPool& pool, UCHAR aBlrSubOp, ValueExprNode* aArg)
	: TypedNode<ValueExprNode, ExprNode::TYPE_EXTRACT>(pool),
	  blrSubOp(aBlrSubOp),
	  arg(aArg)
{
}

DmlNode* ExtractNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	UCHAR blrSubOp = csb->csb_blr_reader.getByte();

	ExtractNode* node = FB_NEW_POOL(pool) ExtractNode(pool, blrSubOp);
	node->arg = PAR_parse_value(tdbb, csb);
	return node;
}

string ExtractNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, blrSubOp);
	NODE_PRINT(printer, arg);

	return "ExtractNode";
}

ValueExprNode* ExtractNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	// Figure out the data type of the sub parameter, and make
	// sure the requested type of information can be extracted.

	ValueExprNode* sub1 = doDsqlPass(dsqlScratch, arg);
	MAKE_desc(dsqlScratch, &sub1->nodDesc, sub1);

	switch (blrSubOp)
	{
		case blr_extract_year:
		case blr_extract_month:
		case blr_extract_day:
		case blr_extract_weekday:
		case blr_extract_yearday:
		case blr_extract_week:
			if (!nodeIs<NullNode>(sub1) &&
				sub1->nodDesc.dsc_dtype != dtype_sql_date &&
				!sub1->nodDesc.isTimeStamp())
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-105) <<
						  Arg::Gds(isc_extract_input_mismatch));
			}
			break;

		case blr_extract_hour:
		case blr_extract_minute:
		case blr_extract_second:
		case blr_extract_millisecond:
			if (!nodeIs<NullNode>(sub1) &&
				!sub1->nodDesc.isTime() &&
				!sub1->nodDesc.isTimeStamp())
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-105) <<
						  Arg::Gds(isc_extract_input_mismatch));
			}
			break;

		case blr_extract_timezone_hour:
		case blr_extract_timezone_minute:
			if (!nodeIs<NullNode>(sub1) &&
				!sub1->nodDesc.isTime() &&
				!sub1->nodDesc.isTimeStamp())
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-105) <<
						  Arg::Gds(isc_extract_input_mismatch));
			}
			break;

		default:
			fb_assert(false);
			break;
	}

	return FB_NEW_POOL(dsqlScratch->getPool()) ExtractNode(dsqlScratch->getPool(), blrSubOp, sub1);
}

void ExtractNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "EXTRACT";
}

bool ExtractNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, arg, desc, forceVarChar);
}

void ExtractNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_extract);
	dsqlScratch->appendUChar(blrSubOp);
	GEN_expr(dsqlScratch, arg);
}

void ExtractNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	dsc desc1;
	MAKE_desc(dsqlScratch, &desc1, arg);

	switch (blrSubOp)
	{
		case blr_extract_second:
			// QUADDATE - maybe this should be DECIMAL(6,4)
			desc->makeLong(ISC_TIME_SECONDS_PRECISION_SCALE);
			break;

		case blr_extract_millisecond:
			desc->makeLong(ISC_TIME_SECONDS_PRECISION_SCALE + 3);
			break;

		default:
			desc->makeShort(0);
			break;
	}

	desc->setNullable(desc1.isNullable());
}

void ExtractNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	switch (blrSubOp)
	{
		case blr_extract_second:
			// QUADDATE - SECOND returns a float, or scaled!
			desc->makeLong(ISC_TIME_SECONDS_PRECISION_SCALE);
			break;

		case blr_extract_millisecond:
			desc->makeLong(ISC_TIME_SECONDS_PRECISION_SCALE + 3);
			break;

		default:
			desc->makeShort(0);
			break;
	}
}

ValueExprNode* ExtractNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	ExtractNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) ExtractNode(*tdbb->getDefaultPool(), blrSubOp);
	node->arg = copier.copy(tdbb, arg);
	return node;
}

bool ExtractNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const ExtractNode* o = nodeAs<ExtractNode>(other);
	fb_assert(o);

	return blrSubOp == o->blrSubOp;
}

bool ExtractNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const ExtractNode* const otherNode = nodeAs<ExtractNode>(other);
	fb_assert(otherNode);

	return blrSubOp == otherNode->blrSubOp;
}

ValueExprNode* ExtractNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

// Handles EXTRACT(part FROM date/time/timestamp)
dsc* ExtractNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	dsc* value = EVL_expr(tdbb, request, arg);

	if (!value || (request->req_flags & req_null))
		return NULL;

	impure->vlu_desc.makeShort(0, &impure->vlu_misc.vlu_short);

	tm times = {0};
	int fractions;
	ISC_TIMESTAMP_TZ timeStampTz;

	switch (value->dsc_dtype)
	{
		case dtype_sql_time:
			switch (blrSubOp)
			{
				case blr_extract_hour:
				case blr_extract_minute:
				case blr_extract_second:
				case blr_extract_millisecond:
					TimeStamp::decode_time(*(GDS_TIME*) value->dsc_address,
						&times.tm_hour, &times.tm_min, &times.tm_sec, &fractions);
					break;

				case blr_extract_timezone_hour:
				case blr_extract_timezone_minute:
				{
					dsc tempDsc;
					tempDsc.makeTimestampTz(&timeStampTz);
					MOV_move(tdbb, value, &tempDsc);
					break;
				}

				default:
					ERR_post(Arg::Gds(isc_expression_eval_err) <<
							 Arg::Gds(isc_invalid_extractpart_time));
			}
			break;

		case dtype_sql_time_tz:
			switch (blrSubOp)
			{
				case blr_extract_hour:
				case blr_extract_minute:
				case blr_extract_second:
				case blr_extract_millisecond:
					TimeZoneUtil::decodeTime(*(ISC_TIME_TZ*) value->dsc_address,
						&EngineCallbacks::instance, &times, &fractions);
					break;

				case blr_extract_timezone_hour:
				case blr_extract_timezone_minute:
					timeStampTz.utc_timestamp.timestamp_date = EngineCallbacks::instance->getLocalDate();
					timeStampTz.utc_timestamp.timestamp_time = ((ISC_TIME_TZ*) value->dsc_address)->utc_time;
					timeStampTz.time_zone = ((ISC_TIME_TZ*) value->dsc_address)->time_zone;
					break;

				default:
					ERR_post(Arg::Gds(isc_expression_eval_err) <<
							 Arg::Gds(isc_invalid_extractpart_time));
			}
			break;

		case dtype_sql_date:
			switch (blrSubOp)
			{
				case blr_extract_hour:
				case blr_extract_minute:
				case blr_extract_second:
				case blr_extract_millisecond:
				case blr_extract_timezone_hour:
				case blr_extract_timezone_minute:
					ERR_post(Arg::Gds(isc_expression_eval_err) <<
							 Arg::Gds(isc_invalid_extractpart_date));
					break;

				default:
					TimeStamp::decode_date(*(GDS_DATE*) value->dsc_address, &times);
			}
			break;

		case dtype_timestamp:
			switch (blrSubOp)
			{
				case blr_extract_timezone_hour:
				case blr_extract_timezone_minute:
				{
					dsc tempDsc;
					tempDsc.makeTimestampTz(&timeStampTz);
					MOV_move(tdbb, value, &tempDsc);
					break;
				}

				default:
					TimeStamp::decode_timestamp(*(GDS_TIMESTAMP*) value->dsc_address, &times, &fractions);
			}
			break;

		case dtype_timestamp_tz:
			switch (blrSubOp)
			{
				case blr_extract_timezone_hour:
				case blr_extract_timezone_minute:
					timeStampTz = *(ISC_TIMESTAMP_TZ*) value->dsc_address;
					break;

				default:
					TimeZoneUtil::decodeTimeStamp(*(ISC_TIMESTAMP_TZ*) value->dsc_address, &times, &fractions);
			}
			break;

		default:
			ERR_post(Arg::Gds(isc_expression_eval_err) <<
					 Arg::Gds(isc_invalidarg_extract));
			break;
	}

	if (blrSubOp == blr_extract_timezone_hour || blrSubOp == blr_extract_timezone_minute)
	{
		int tzSign;
		unsigned tzh, tzm;
		TimeZoneUtil::extractOffset(timeStampTz, &tzSign, &tzh, &tzm);

		switch (blrSubOp)
		{
			case blr_extract_timezone_hour:
				*(SSHORT*) impure->vlu_desc.dsc_address = tzSign * int(tzh);
				return &impure->vlu_desc;

			case blr_extract_timezone_minute:
				*(SSHORT*) impure->vlu_desc.dsc_address = tzSign * int(tzm);
				return &impure->vlu_desc;
		}
	}

	USHORT part;

	switch (blrSubOp)
	{
		case blr_extract_year:
			part = times.tm_year + 1900;
			break;

		case blr_extract_month:
			part = times.tm_mon + 1;
			break;

		case blr_extract_day:
			part = times.tm_mday;
			break;

		case blr_extract_hour:
			part = times.tm_hour;
			break;

		case blr_extract_minute:
			part = times.tm_min;
			break;

		case blr_extract_second:
			impure->vlu_desc.dsc_dtype = dtype_long;
			impure->vlu_desc.dsc_length = sizeof(ULONG);
			impure->vlu_desc.dsc_scale = ISC_TIME_SECONDS_PRECISION_SCALE;
			impure->vlu_desc.dsc_address = reinterpret_cast<UCHAR*>(&impure->vlu_misc.vlu_long);
			*(ULONG*) impure->vlu_desc.dsc_address = times.tm_sec * ISC_TIME_SECONDS_PRECISION + fractions;
			return &impure->vlu_desc;

		case blr_extract_millisecond:
			impure->vlu_desc.dsc_dtype = dtype_long;
			impure->vlu_desc.dsc_length = sizeof(ULONG);
			impure->vlu_desc.dsc_scale = ISC_TIME_SECONDS_PRECISION_SCALE + 3;
			impure->vlu_desc.dsc_address = reinterpret_cast<UCHAR*>(&impure->vlu_misc.vlu_long);
			(*(ULONG*) impure->vlu_desc.dsc_address) = fractions;
			return &impure->vlu_desc;

		case blr_extract_week:
		{
			// Algorithm for Converting Gregorian Dates to ISO 8601 Week Date by Rick McCarty, 1999
			// http://personal.ecu.edu/mccartyr/ISOwdALG.txt

			const int y = times.tm_year + 1900;
			const int dayOfYearNumber = times.tm_yday + 1;

			// Find the jan1Weekday for y (Monday=1, Sunday=7)
			const int yy = (y - 1) % 100;
			const int c = (y - 1) - yy;
			const int g = yy + yy / 4;
			const int jan1Weekday = 1 + (((((c / 100) % 4) * 5) + g) % 7);

			// Find the weekday for y m d
			const int h = dayOfYearNumber + (jan1Weekday - 1);
			const int weekday = 1 + ((h - 1) % 7);

			// Find if y m d falls in yearNumber y-1, weekNumber 52 or 53
			int yearNumber, weekNumber;

			if ((dayOfYearNumber <= (8 - jan1Weekday)) && (jan1Weekday > 4))
			{
				yearNumber = y - 1;
				weekNumber = ((jan1Weekday == 5) || ((jan1Weekday == 6) &&
					TimeStamp::isLeapYear(yearNumber))) ? 53 : 52;
			}
			else
			{
				yearNumber = y;

				// Find if y m d falls in yearNumber y+1, weekNumber 1
				int i = TimeStamp::isLeapYear(y) ? 366 : 365;

				if ((i - dayOfYearNumber) < (4 - weekday))
				{
					yearNumber = y + 1;
					weekNumber = 1;
				}
			}

			// Find if y m d falls in yearNumber y, weekNumber 1 through 53
			if (yearNumber == y)
			{
				int j = dayOfYearNumber + (7 - weekday) + (jan1Weekday - 1);
				weekNumber = j / 7;
				if (jan1Weekday > 4)
					weekNumber--;
			}

			part = weekNumber;
			break;
		}

		case blr_extract_yearday:
			part = times.tm_yday;
			break;

		case blr_extract_weekday:
			part = times.tm_wday;
			break;

		default:
			fb_assert(false);
			part = 0;
	}

	*(USHORT*) impure->vlu_desc.dsc_address = part;

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<FieldNode> regFieldNodeFid(blr_fid);
static RegisterNode<FieldNode> regFieldNodeField(blr_field);

FieldNode::FieldNode(MemoryPool& pool, dsql_ctx* context, dsql_fld* field, ValueListNode* indices)
	: TypedNode<ValueExprNode, ExprNode::TYPE_FIELD>(pool),
	  dsqlQualifier(pool),
	  dsqlName(pool),
	  dsqlContext(context),
	  dsqlField(field),
	  dsqlIndices(indices),
	  fieldStream(0),
	  format(NULL),
	  fieldId(0),
	  byId(false),
	  dsqlCursorField(false)
{
}

FieldNode::FieldNode(MemoryPool& pool, StreamType stream, USHORT id, bool aById)
	: TypedNode<ValueExprNode, ExprNode::TYPE_FIELD>(pool),
	  dsqlQualifier(pool),
	  dsqlName(pool),
	  dsqlContext(NULL),
	  dsqlField(NULL),
	  dsqlIndices(NULL),
	  fieldStream(stream),
	  format(NULL),
	  fieldId(id),
	  byId(aById),
	  dsqlCursorField(false)
{
}

// Parse a field.
DmlNode* FieldNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	const USHORT context = csb->csb_blr_reader.getByte();

	// check if this is a VALUE of domain's check constraint
	if (!csb->csb_domain_validation.isEmpty() && context == 0 &&
		(blrOp == blr_fid || blrOp == blr_field))
	{
		if (blrOp == blr_fid)
		{
#ifdef DEV_BUILD
			SSHORT id =
#endif
				csb->csb_blr_reader.getWord();
			fb_assert(id == 0);
		}
		else
		{
			MetaName name;
			csb->csb_blr_reader.getMetaName(name);
		}

		DomainValidationNode* domNode = FB_NEW_POOL(pool) DomainValidationNode(pool);
		MET_get_domain(tdbb, csb->csb_pool, csb->csb_domain_validation, &domNode->domDesc, NULL);

		// Cast to the target type - see CORE-3545.
		CastNode* castNode = FB_NEW_POOL(pool) CastNode(pool);
		castNode->castDesc = domNode->domDesc;
		castNode->source = domNode;

		return castNode;
	}

	if (context >= csb->csb_rpt.getCount())/* ||
		!(csb->csb_rpt[context].csb_flags & csb_used) )

		dimitr:	commented out to support system triggers implementing
				WITH CHECK OPTION. They reference the relation stream (#2)
				directly, without a DSQL context. It breaks the layering,
				but we must support legacy BLR.
		*/
	{
		PAR_error(csb, Arg::Gds(isc_ctxnotdef));
	}

	MetaName name;
	SSHORT id;
	const StreamType stream = csb->csb_rpt[context].csb_stream;
	bool is_column = false;
	bool byId = false;

	if (blrOp == blr_fid)
	{
		id = csb->csb_blr_reader.getWord();
		byId = true;
		is_column = true;
	}
	else if (blrOp == blr_field)
	{
		CompilerScratch::csb_repeat* tail = &csb->csb_rpt[stream];
		const jrd_prc* procedure = tail->csb_procedure;

		// make sure procedure has been scanned before using it

		if (procedure && !procedure->isSubRoutine() &&
			(!(procedure->flags & Routine::FLAG_SCANNED) ||
				(procedure->flags & Routine::FLAG_BEING_SCANNED) ||
				(procedure->flags & Routine::FLAG_BEING_ALTERED)))
		{
			const jrd_prc* scan_proc = MET_procedure(tdbb, procedure->getId(), false, 0);

			if (scan_proc != procedure)
				procedure = NULL;
		}

		if (procedure)
		{
			csb->csb_blr_reader.getMetaName(name);

			if ((id = PAR_find_proc_field(procedure, name)) == -1)
			{
				PAR_error(csb, Arg::Gds(isc_fldnotdef2) <<
					Arg::Str(name) << Arg::Str(procedure->getName().toString()));
			}
		}
		else
		{
			jrd_rel* relation = tail->csb_relation;
			if (!relation)
				PAR_error(csb, Arg::Gds(isc_ctxnotdef));

			// make sure relation has been scanned before using it

			if (!(relation->rel_flags & REL_scanned) || (relation->rel_flags & REL_being_scanned))
				MET_scan_relation(tdbb, relation);

			csb->csb_blr_reader.getMetaName(name);

			if ((id = MET_lookup_field(tdbb, relation, name)) < 0)
			{
				if (csb->csb_g_flags & csb_validation)
				{
					id = 0;
					byId = true;
					is_column = true;
				}
				else
				{
					if (relation->rel_flags & REL_system)
						return FB_NEW_POOL(pool) NullNode(pool);

 					if (tdbb->getAttachment()->isGbak())
					{
						PAR_warning(Arg::Warning(isc_fldnotdef) << Arg::Str(name) <<
																   Arg::Str(relation->rel_name));
					}
					else if (!(relation->rel_flags & REL_deleted))
					{
						PAR_error(csb, Arg::Gds(isc_fldnotdef) << Arg::Str(name) <<
																  Arg::Str(relation->rel_name));
					}
					else
						PAR_error(csb, Arg::Gds(isc_ctxnotdef));
				}
			}
		}
	}

	// check for dependencies -- if a field name was given,
	// use it because when restoring the database the field
	// id's may not be valid yet

	if (csb->csb_g_flags & csb_get_dependencies)
	{
		if (blrOp == blr_fid)
			PAR_dependency(tdbb, csb, stream, id, "");
		else
			PAR_dependency(tdbb, csb, stream, id, name);
	}

	if (is_column)
	{
		const jrd_rel* const temp_rel = csb->csb_rpt[stream].csb_relation;

		if (temp_rel)
		{
			fb_assert(id >= 0);

			if (!temp_rel->rel_fields || id >= (int) temp_rel->rel_fields->count() ||
				!(*temp_rel->rel_fields)[id])
			{
				if (temp_rel->rel_flags & REL_system)
					return FB_NEW_POOL(pool) NullNode(pool);
			}
		}
	}

	return PAR_gen_field(tdbb, stream, id, byId);
}

string FieldNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlQualifier);
	NODE_PRINT(printer, dsqlName);
	NODE_PRINT(printer, dsqlContext);
	NODE_PRINT(printer, dsqlField);
	NODE_PRINT(printer, dsqlIndices);
	NODE_PRINT(printer, fieldStream);
	NODE_PRINT(printer, format);
	NODE_PRINT(printer, fieldId);
	NODE_PRINT(printer, byId);
	NODE_PRINT(printer, dsqlCursorField);
	NODE_PRINT(printer, cursorNumber);

	return "FieldNode";
}

ValueExprNode* FieldNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (dsqlContext)
	{
		// AB: This is an already processed node. This could be done in expand_select_list.
		return this;
	}

	if (dsqlScratch->isPsql() && !dsqlQualifier.hasData())
	{
		VariableNode* node = FB_NEW_POOL(dsqlScratch->getPool()) VariableNode(dsqlScratch->getPool());
		node->line = line;
		node->column = column;
		node->dsqlName = dsqlName;
		return node->dsqlPass(dsqlScratch);
	}
	else
		return internalDsqlPass(dsqlScratch, NULL);
}

// Resolve a field name to an available context.
// If list is true, then this function can detect and return a relation node if there is no name.
// This is used for cases of "SELECT <table_name>. ...".
// CVC: The function attempts to detect if an unqualified field appears in more than one context
// and hence it returns the number of occurrences. This was added to allow the caller to detect
// ambiguous commands like select  from t1 join t2 on t1.f = t2.f order by common_field.
// While inoffensive on inner joins, it changes the result on outer joins.
ValueExprNode* FieldNode::internalDsqlPass(DsqlCompilerScratch* dsqlScratch, RecordSourceNode** list)
{
	thread_db* tdbb = JRD_get_thread_data();

	if (list)
		*list = NULL;

    /* CVC: PLEASE READ THIS EXPLANATION IF YOU NEED TO CHANGE THIS CODE.
       You should ensure that this function:
       1.- Never returns NULL. In such case, it such fall back to an invocation
       to PASS1_field_unknown() near the end of this function. None of the multiple callers
       of this function (inside this same module) expect a null pointer, hence they
       will crash the engine in such case.
       2.- Doesn't allocate more than one field in "node". Either you put a break,
       keep the current "continue" or call ALLD_release if you don't want nor the
       continue neither the break if node is already allocated. If it isn't evident,
       but this variable is initialized to zero in the declaration above. You
       may write an explicit line to set it to zero here, before the loop.

       3.- Doesn't waste cycles if qualifier is not null. The problem is not the cycles
       themselves, but the fact that you'll detect an ambiguity that doesn't exist: if
       the field appears in more than one context but it's always qualified, then
       there's no ambiguity. There's PASS1_make_context() that prevents a context's
       alias from being reused. However, other places in the code don't check that you
       don't create a join or subselect with the same context without disambiguating it
       with different aliases. This is the place where resolveContext() is called for
       that purpose. In the future, it will be fine if we force the use of the alias as
       the only allowed qualifier if the alias exists. Hopefully, we will eliminate
       some day this construction: "select table.field from table t" because it
       should be "t.field" instead.

       AB: 2004-01-09
       The explained query directly above doesn't work anymore, thus the day has come ;-)
	   It's allowed to use the same fieldname between different scope levels (sub-queries)
	   without being hit by the ambiguity check. The field uses the first match starting
	   from it's own level (of course ambiguity-check on each level is done).

       4.- Doesn't verify code derived automatically from check constraints. They are
       ill-formed by nature but making that code generation more orthodox is not a
       priority. Typically, they only check a field against a contant. The problem
       appears when they check a field against a subselect, for example. For now,
       allow the user to write ambiguous subselects in check() statements.
       Claudio Valderrama - 2001.1.29.
    */

	// Try to resolve field against various contexts;
	// if there is an alias, check only against the first matching

	ValueExprNode* node = NULL; // This var must be initialized.
	DsqlContextStack ambiguousCtxStack;

	bool resolveByAlias = true;
	const bool relaxedAliasChecking = Config::getRelaxedAliasChecking();

	while (true)
	{
		// AB: Loop through the scope_levels starting by its own.
		bool done = false;
		USHORT currentScopeLevel = dsqlScratch->scopeLevel + 1;
		for (; currentScopeLevel > 0 && !done; --currentScopeLevel)
		{
			// If we've found a node we're done.
			if (node)
				break;

			for (DsqlContextStack::iterator stack(*dsqlScratch->context); stack.hasData(); ++stack)
			{
				dsql_ctx* context = stack.object();

				if (context->ctx_scope_level != currentScopeLevel - 1 ||
					((context->ctx_flags & CTX_cursor) && dsqlQualifier.isEmpty()) ||
					(!(context->ctx_flags & CTX_cursor) && dsqlCursorField))
				{
					continue;
				}

				dsql_fld* field = resolveContext(dsqlScratch, dsqlQualifier, context, resolveByAlias);

				// AB: When there's no relation and no procedure then we have a derived table.
				const bool isDerivedTable =
					(!context->ctx_procedure && !context->ctx_relation && context->ctx_rse);

				if (field)
				{
					// If there's no name then we have most probable an asterisk that
					// needs to be exploded. This should be handled by the caller and
					// when the caller can handle this, list is true.
					if (dsqlName.isEmpty())
					{
						if (list)
						{
							dsql_ctx* stackContext = stack.object();

							if (context->ctx_relation)
							{
								RelationSourceNode* relNode = FB_NEW_POOL(*tdbb->getDefaultPool())
									RelationSourceNode(*tdbb->getDefaultPool());
								relNode->dsqlContext = stackContext;
								*list = relNode;
							}
							else if (context->ctx_procedure)
							{
								ProcedureSourceNode* procNode = FB_NEW_POOL(*tdbb->getDefaultPool())
									ProcedureSourceNode(*tdbb->getDefaultPool());
								procNode->dsqlContext = stackContext;
								*list = procNode;
							}

							fb_assert(*list);
							return NULL;
						}

						break;
					}

					NestConst<ValueExprNode> usingField = NULL;

					for (; field; field = field->fld_next)
					{
						if (field->fld_name == dsqlName.c_str())
						{
							if (dsqlQualifier.isEmpty())
							{
								if (!context->getImplicitJoinField(field->fld_name, usingField))
								{
									field = NULL;
									break;
								}

								if (usingField)
									field = NULL;
							}

							ambiguousCtxStack.push(context);
							break;
						}
					}

					if ((context->ctx_flags & CTX_view_with_check_store) && !field)
					{
						node = FB_NEW_POOL(*tdbb->getDefaultPool()) NullNode(*tdbb->getDefaultPool());
						node->line = line;
						node->column = column;
					}
					else if (dsqlQualifier.hasData() && !field)
					{
						if (!(context->ctx_flags & CTX_view_with_check_modify))
						{
							// If a qualifier was present and we didn't find
							// a matching field then we should stop searching.
							// Column unknown error will be raised at bottom of function.
							done = true;
							break;
						}
					}
					else if (field || usingField)
					{
						// Intercept any reference to a field with datatype that
						// did not exist prior to V6 and post an error

						// CVC: Stop here if this is our second or third iteration.
						// Anyway, we can't report more than one ambiguity to the status vector.
						// AB: But only if we're on different scope level, because a
						// node inside the same context should have priority.
						if (node)
							continue;

						ValueListNode* indices = dsqlIndices ?
							doDsqlPass(dsqlScratch, dsqlIndices, false) : NULL;

						if (context->ctx_flags & CTX_null)
							node = FB_NEW_POOL(*tdbb->getDefaultPool()) NullNode(*tdbb->getDefaultPool());
						else if (field)
							node = MAKE_field(context, field, indices);
						else
							node = list ? usingField.getObject() : doDsqlPass(dsqlScratch, usingField, false);

						node->line = line;
						node->column = column;
					}
				}
				else if (isDerivedTable)
				{
					// if an qualifier is present check if we have the same derived
					// table else continue;
					if (dsqlQualifier.hasData())
					{
						if (context->ctx_alias.hasData())
						{
							if (dsqlQualifier != context->ctx_alias)
								continue;
						}
						else
							continue;
					}

					// If there's no name then we have most probable a asterisk that
					// needs to be exploded. This should be handled by the caller and
					// when the caller can handle this, list is true.
					if (dsqlName.isEmpty())
					{
						if (list)
						{
							// Return node which PASS1_expand_select_node() can deal with it.
							*list = context->ctx_rse;
							return NULL;
						}

						break;
					}

					// Because every select item has an alias we can just walk
					// through the list and return the correct node when found.
					ValueListNode* rseItems = context->ctx_rse->dsqlSelectList;

					for (auto& rseItem : rseItems->items)
					{
						DerivedFieldNode* selectItem = nodeAs<DerivedFieldNode>(rseItem);

						// select-item should always be a alias!
						if (selectItem)
						{
							NestConst<ValueExprNode> usingField = NULL;

							if (dsqlQualifier.isEmpty())
							{
								if (!context->getImplicitJoinField(dsqlName, usingField))
									break;
							}

							if (dsqlName == selectItem->name || usingField)
							{
								// This is a matching item so add the context to the ambiguous list.
								ambiguousCtxStack.push(context);

								// Stop here if this is our second or more iteration.
								if (node)
									break;

								node = usingField ? usingField : rseItem;
								break;
							}
						}
						else
						{
							// Internal dsql error: alias type expected by pass1_field
							ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
									  Arg::Gds(isc_dsql_command_err) <<
									  Arg::Gds(isc_dsql_derived_alias_field));
						}
					}

					if (!node && dsqlQualifier.hasData())
					{
						// If a qualifier was present and we didn't find
						// a matching field then we should stop searching.
						// Column unknown error will be raised at bottom of function.
						done = true;
						break;
					}
				}
			}
		}

		if (node)
			break;

		if (resolveByAlias && !dsqlScratch->checkConstraintTrigger && relaxedAliasChecking)
			resolveByAlias = false;
		else
			break;
	}

	// CVC: We can't return blindly if this is a check constraint, because there's
	// the possibility of an invalid field that wasn't found. The multiple places that
	// call this function pass1_field() don't expect a NULL pointer, hence will crash.
	// Don't check ambiguity if we don't have a field.

	if (node && dsqlName.hasData())
		PASS1_ambiguity_check(dsqlScratch, dsqlName, ambiguousCtxStack);

	// Clean up stack
	ambiguousCtxStack.clear();

	if (node)
		return node;

	PASS1_field_unknown(dsqlQualifier.nullStr(), dsqlName.nullStr(), this);

	// CVC: PASS1_field_unknown() calls ERRD_post() that never returns, so the next line
	// is only to make the compiler happy.
	return NULL;
}

// Attempt to resolve field against context. Return first field in context if successful, NULL if not.
dsql_fld* FieldNode::resolveContext(DsqlCompilerScratch* dsqlScratch, const MetaName& qualifier,
	dsql_ctx* context, bool resolveByAlias)
{
	// CVC: Warning: the second param, "name" is not used anymore and
	// therefore it was removed. Thus, the local variable "table_name"
	// is being stripped here to avoid mismatches due to trailing blanks.

	DEV_BLKCHK(dsqlScratch, dsql_type_req);
	DEV_BLKCHK(context, dsql_type_ctx);

	if ((dsqlScratch->flags & DsqlCompilerScratch::FLAG_RETURNING_INTO) &&
		(context->ctx_flags & CTX_returning))
	{
		return NULL;
	}

	dsql_rel* relation = context->ctx_relation;
	dsql_prc* procedure = context->ctx_procedure;
	if (!relation && !procedure)
		return NULL;

	// if there is no qualifier, then we cannot match against
	// a context of a different scoping level
	// AB: Yes we can, but the scope level where the field is has priority.
	/***
	if (qualifier.isEmpty() && context->ctx_scope_level != dsqlScratch->scopeLevel)
		return NULL;
	***/

	// AB: If this context is a system generated context as in NEW/OLD inside
	// triggers, the qualifier by the field is mandatory. While we can't
	// fall back from a higher scope-level to the NEW/OLD contexts without
	// the qualifier present.
	// An exception is a check-constraint that is allowed to reference fields
	// without the qualifier.
	if (!dsqlScratch->checkConstraintTrigger && (context->ctx_flags & CTX_system) && qualifier.isEmpty())
		return NULL;

	const TEXT* table_name = NULL;
	if (context->ctx_internal_alias.hasData() && resolveByAlias)
		table_name = context->ctx_internal_alias.c_str();

	// AB: For a check constraint we should ignore the alias if the alias
	// contains the "NEW" alias. This is because it is possible
	// to reference a field by the complete table-name as alias
	// (see EMPLOYEE table in examples for a example).
	if (dsqlScratch->checkConstraintTrigger && table_name)
	{
		// If a qualifier is present and it's equal to the alias then we've already the right table-name
		if (!(qualifier.hasData() && qualifier == table_name))
		{
			if (strcmp(table_name, NEW_CONTEXT_NAME) == 0)
				table_name = NULL;
			else if (strcmp(table_name, OLD_CONTEXT_NAME) == 0)
			{
				// Only use the OLD context if it is explicit used. That means the
				// qualifer should hold the "OLD" alias.
				return NULL;
			}
		}
	}

	if (!table_name)
	{
		if (relation)
			table_name = relation->rel_name.c_str();
		else
			table_name = procedure->prc_name.identifier.c_str();
	}

	// If a context qualifier is present, make sure this is the proper context
	if (qualifier.hasData() && qualifier != table_name)
		return NULL;

	// Lookup field in relation or procedure

	return relation ? relation->rel_fields : procedure->prc_outputs;
}

bool FieldNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	if (visitor.deepestLevel < dsqlContext->ctx_scope_level)
		visitor.deepestLevel = dsqlContext->ctx_scope_level;

	return false;
}

bool FieldNode::dsqlAggregate2Finder(Aggregate2Finder& /*visitor*/)
{
	return false;
}

bool FieldNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	// Wouldn't it be better to call an error from this point where return is true?
	// Then we could give the fieldname that's making the trouble.

	// If we come here then this field is used inside a aggregate-function. The
	// ctx_scope_level gives the info how deep the context is inside the statement.

	// If the context-scope-level from this field is lower or the same as the scope-level
	// from the given context then it is an invalid field.
	if (dsqlContext->ctx_scope_level == visitor.context->ctx_scope_level)
	{
		// Return true (invalid) if this field isn't inside the GROUP BY clause, that
		// should already been seen in the match_node test in that routine start.
		return true;
	}

	return false;
}

bool FieldNode::dsqlSubSelectFinder(SubSelectFinder& /*visitor*/)
{
	return false;
}

bool FieldNode::dsqlFieldFinder(FieldFinder& visitor)
{
	visitor.field = true;

	switch (visitor.matchType)
	{
		case FIELD_MATCH_TYPE_EQUAL:
			return dsqlContext->ctx_scope_level == visitor.checkScopeLevel;

		case FIELD_MATCH_TYPE_LOWER:
			return dsqlContext->ctx_scope_level < visitor.checkScopeLevel;

		case FIELD_MATCH_TYPE_LOWER_EQUAL:
			return dsqlContext->ctx_scope_level <= visitor.checkScopeLevel;

		///case FIELD_MATCH_TYPE_HIGHER:
		///	return dsqlContext->ctx_scope_level > visitor.checkScopeLevel;

		///case FIELD_MATCH_TYPE_HIGHER_EQUAL:
		///	return dsqlContext->ctx_scope_level >= visitor.checkScopeLevel;

		default:
			fb_assert(false);
	}

	return false;
}

ValueExprNode* FieldNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	if (dsqlContext->ctx_scope_level == visitor.context->ctx_scope_level)
		return PASS1_post_map(visitor.dsqlScratch, this, visitor.context, visitor.windowNode);

	return this;
}

void FieldNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = dsqlField->fld_name.c_str();
	setParameterInfo(parameter, dsqlContext);
}

// Generate blr for a field - field id's are preferred but not for trigger or view blr.
void FieldNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (dsqlIndices)
		dsqlScratch->appendUChar(blr_index);

	if (DDL_ids(dsqlScratch))
	{
		dsqlScratch->appendUChar(blr_fid);
		GEN_stuff_context(dsqlScratch, dsqlContext);
		dsqlScratch->appendUShort(dsqlField->fld_id);
	}
	else
	{
		dsqlScratch->appendUChar(blr_field);
		GEN_stuff_context(dsqlScratch, dsqlContext);
		dsqlScratch->appendMetaString(dsqlField->fld_name.c_str());
	}

	if (dsqlIndices)
	{
		dsqlScratch->appendUChar(dsqlIndices->items.getCount());

		for (auto& index : dsqlIndices->items)
			GEN_expr(dsqlScratch, index);
	}
}

void FieldNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	if (nodDesc.dsc_dtype)
		*desc = nodDesc;
	else
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-203) <<
				  Arg::Gds(isc_dsql_field_ref));
	}
}

bool FieldNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const FieldNode* o = nodeAs<FieldNode>(other);
	fb_assert(o);

	if (dsqlField != o->dsqlField || dsqlContext != o->dsqlContext)
		return false;

	if (dsqlIndices || o->dsqlIndices)
		return PASS1_node_match(dsqlScratch, dsqlIndices, o->dsqlIndices, ignoreMapCast);

	return true;
}

bool FieldNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const FieldNode* const otherNode = nodeAs<FieldNode>(other);
	fb_assert(otherNode);

	return fieldId == otherNode->fieldId &&
		(ignoreStreams || fieldStream == otherNode->fieldStream);
}

bool FieldNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	if (allowOnlyCurrentStream)
	{
		if (fieldStream != stream && !(csb->csb_rpt[fieldStream].csb_flags & csb_sub_stream))
			return false;
	}
	else
	{
		if (fieldStream == stream)
			return false;
	}

	return csb->csb_rpt[fieldStream].csb_flags & csb_active;
}

void FieldNode::findDependentFromStreams(const OptimizerRetrieval* optRet, SortedStreamList* streamList)
{
	// dimitr: OLD/NEW contexts shouldn't create any stream dependencies.

	if (fieldStream != optRet->stream &&
		(optRet->csb->csb_rpt[fieldStream].csb_flags & csb_active) &&
		!(optRet->csb->csb_rpt[fieldStream].csb_flags & csb_trigger))
	{
		if (!streamList->exist(fieldStream))
			streamList->add(fieldStream);
	}
}

void FieldNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	const Format* const format = CMP_format(tdbb, csb, fieldStream);

	if (fieldId >= format->fmt_count)
	{
		desc->clear();
	}
	else
	{
		*desc = format->fmt_desc[fieldId];
		desc->dsc_address = NULL;

		// Fix UNICODE_FSS wrong length used in system tables.
		jrd_rel* relation = csb->csb_rpt[fieldStream].csb_relation;

		if (relation && (relation->rel_flags & REL_system) &&
			desc->isText() && desc->getCharSet() == CS_UNICODE_FSS)
		{
			USHORT adjust = 0;

			if (desc->dsc_dtype == dtype_varying)
				adjust = sizeof(USHORT);
			else if (desc->dsc_dtype == dtype_cstring)
				adjust = 1;

			desc->dsc_length -= adjust;
			desc->dsc_length *= 3;
			desc->dsc_length += adjust;
		}
	}
}

ValueExprNode* FieldNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	USHORT fldId = copier.getFieldId(this);
	StreamType stream = fieldStream;

	fldId = copier.remapField(stream, fldId);

	if (copier.remap)
	{
#ifdef CMP_DEBUG
		csb->dump("remap nod_field: %d -> %d\n", stream, copier.remap[stream]);
#endif
		stream = copier.remap[stream];
	}

	fb_assert(!cursorNumber.specified);
	return PAR_gen_field(tdbb, stream, fldId, byId);
}

ValueExprNode* FieldNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	StreamType stream = fieldStream;

	markVariant(csb, stream);

	CompilerScratch::csb_repeat* tail = &csb->csb_rpt[stream];
	jrd_rel* relation = tail->csb_relation;
	jrd_fld* field;

	if (!relation || !(field = MET_get_field(relation, fieldId)))
		return ValueExprNode::pass1(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);

	const USHORT ttype = INTL_TEXT_TYPE(desc);

	// Are we using a collation?
	if (TTYPE_TO_COLLATION(ttype) != 0)
	{
		Collation* collation = NULL;

		try
		{
			ThreadStatusGuard local_status(tdbb);
			collation = INTL_texttype_lookup(tdbb, ttype);
		}
		catch (Exception&)
		{
			// ASF: Swallow the exception if we fail to load the collation here.
			// This allows us to backup databases when the collation isn't available.
			if (!tdbb->getAttachment()->isGbak())
				throw;
		}

		if (collation)
			CMP_post_resource(&csb->csb_resources, collation, Resource::rsc_collation, ttype);
	}

	// if this is a modify or store, check REFERENCES access to any foreign keys

	/* CVC: This is against the SQL standard. REFERENCES should be enforced only at the
		time the FK is defined in DDL, not when a DML is going to be executed.
	if (((tail->csb_flags & csb_modify) || (tail->csb_flags & csb_store)) &&
		!(relation->rel_view_rse || relation->rel_file))
	{
		IDX_check_access(tdbb, csb, tail->csb_view, relation);
	}
	*/

	// posting the required privilege access to the current relation and field

	// If this is in a "validate_subtree" then we must not
	// post access checks to the table and the fields in the table.
	// If any node of the parse tree is a nod_validate type node,
	// the nodes in the subtree are involved in a validation
	// clause only, the subtree is a validate_subtree in our notation.

	SLONG ssRelationId = tail->csb_view ?
		tail->csb_view->rel_id : (csb->csb_view ? csb->csb_view->rel_id : 0);

	if (tail->csb_flags & csb_modify)
	{
		if (!csb->csb_validate_expr)
		{
			SecurityClass::flags_t priv = csb->csb_returning_expr ?
				SCL_select : SCL_update;
			CMP_post_access(tdbb, csb, relation->rel_security_name, ssRelationId,
				priv, SCL_object_table, relation->rel_name);
			CMP_post_access(tdbb, csb, field->fld_security_name, ssRelationId,
				priv, SCL_object_column, field->fld_name, relation->rel_name);
		}
	}
	else if (tail->csb_flags & csb_erase)
	{
		CMP_post_access(tdbb, csb, relation->rel_security_name, ssRelationId,
			SCL_delete, SCL_object_table, relation->rel_name);
	}
	else if (tail->csb_flags & csb_store)
	{
		CMP_post_access(tdbb, csb, relation->rel_security_name, ssRelationId,
			SCL_insert, SCL_object_table, relation->rel_name);
		CMP_post_access(tdbb, csb, field->fld_security_name, ssRelationId,
			SCL_insert, SCL_object_column, field->fld_name, relation->rel_name);
	}
	else
	{
		CMP_post_access(tdbb, csb, relation->rel_security_name, ssRelationId,
			SCL_select, SCL_object_table, relation->rel_name);
		CMP_post_access(tdbb, csb, field->fld_security_name, ssRelationId,
			SCL_select, SCL_object_column, field->fld_name, relation->rel_name);
	}

	ValueExprNode* sub;

	if (!(sub = field->fld_computation) && !(sub = field->fld_source))
	{

		if (!relation->rel_view_rse)
			return ValueExprNode::pass1(tdbb, csb);

		// Msg 364 "cannot access column %s in view %s"
		ERR_post(Arg::Gds(isc_no_field_access) << Arg::Str(field->fld_name) <<
												  Arg::Str(relation->rel_name));
	}

	// The previous test below is an apparent temporary fix
	// put in by Root & Harrison in Summer/Fall 1991.
	// Old Code:
	// if (tail->csb_flags & (csb_view_update | csb_trigger))
	//   return ValueExprNode::pass1(tdbb, csb);
	// If the field is a computed field - we'll go on and make
	// the substitution.
	// Comment 1994-August-08 David Schnepper

	if (tail->csb_flags & (csb_view_update | csb_trigger))
	{
		// dimitr:	added an extra check for views, because we don't
		//			want their old/new contexts to be substituted
		if (relation->rel_view_rse || !field->fld_computation)
			return ValueExprNode::pass1(tdbb, csb);
	}

	StreamMap localMap;
	StreamType* map = tail->csb_map;

	if (!map)
	{
		map = localMap.getBuffer(STREAM_MAP_LENGTH);
		fb_assert(stream + 2u <= MAX_STREAMS);
		map[0] = stream;
		map[1] = stream + 1;
		map[2] = stream + 2;
	}

	AutoSetRestore<USHORT> autoRemapVariable(&csb->csb_remap_variable,
		(csb->csb_variables ? csb->csb_variables->count() : 0) + 1);

	sub = NodeCopier::copy(tdbb, csb, sub, map);

	bool computingField = false;

	// If this is a computed field, cast the computed expression to the field type if required.
	// See CORE-5097.
	if (field->fld_computation && !relation->rel_view_rse)
	{
		FB_SIZE_T pos;

		if (csb->csb_computing_fields.find(field, pos))
			ERR_post(Arg::Gds(isc_circular_computed));
		else
		{
			csb->csb_computing_fields.insert(pos, field);
			computingField = true;
		}

		CastNode* cast = FB_NEW_POOL(*tdbb->getDefaultPool()) CastNode(
			*tdbb->getDefaultPool());
		cast->source = sub;
		cast->castDesc = desc;
		cast->artificial = true;

		sub = cast;
	}

	AutoSetRestore<jrd_rel*> autoRelationStream(&csb->csb_parent_relation,
		relation->rel_ss_definer.value ? relation : NULL);

	if (relation->rel_view_rse)
	{
		// dimitr:	if we reference view columns, we need to pass them
		//			as belonging to a view (in order to compute the access
		//			permissions properly).
		AutoSetRestore<jrd_rel*> autoView(&csb->csb_view, relation);
		AutoSetRestore<StreamType> autoViewStream(&csb->csb_view_stream, stream);

		// ASF: If the view field doesn't reference any of the view streams,
		// evaluate it based on the view dbkey - CORE-1245.
		SortedStreamList streams;
		sub->collectStreams(csb, streams);

		bool view_refs = false;
		for (FB_SIZE_T i = 0; i < streams.getCount(); i++)
		{
			const CompilerScratch::csb_repeat* const sub_tail = &csb->csb_rpt[streams[i]];

			if (sub_tail->csb_view && sub_tail->csb_view_stream == stream)
			{
				view_refs = true;
				break;
			}
		}

		if (!view_refs)
		{
			DerivedExprNode* derivedNode =
				FB_NEW_POOL(*tdbb->getDefaultPool()) DerivedExprNode(*tdbb->getDefaultPool());
			derivedNode->arg = sub;
			derivedNode->internalStreamList.add(stream);

			sub = derivedNode;
		}

		doPass1(tdbb, csb, &sub);	// note: scope of AutoSetRestore
	}
	else
	{
		DerivedExprNode* derivedNode =
			FB_NEW_POOL(*tdbb->getDefaultPool()) DerivedExprNode(*tdbb->getDefaultPool());
		derivedNode->arg = sub;
		derivedNode->internalStreamList.add(stream);

		sub = derivedNode;

		doPass1(tdbb, csb, &sub);
	}

	if (computingField)
	{
		FB_SIZE_T pos;

		if (csb->csb_computing_fields.find(field, pos))
			csb->csb_computing_fields.remove(pos);
		else
			fb_assert(false);
	}

	return sub;
}

ValueExprNode* FieldNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	// SMB_SET uses ULONG, not USHORT
	SBM_SET(tdbb->getDefaultPool(), &csb->csb_rpt[fieldStream].csb_fields, fieldId);

	if (csb->csb_rpt[fieldStream].csb_relation || csb->csb_rpt[fieldStream].csb_procedure)
		format = CMP_format(tdbb, csb, fieldStream);

	impureOffset = CMP_impure(csb, sizeof(impure_value_ex));
	cursorNumber = csb->csb_rpt[fieldStream].csb_cursor_number;

	return this;
}

dsc* FieldNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);

	if (cursorNumber.specified)
		request->req_cursors[cursorNumber.value]->checkState(request);

	record_param& rpb = request->req_rpb[fieldStream];
	Record* record = rpb.rpb_record;
	jrd_rel* relation = rpb.rpb_relation;

	// In order to "map a null to a default" value (in EVL_field()), the relation block is referenced.
	// Reference: Bug 10116, 10424

	if (!EVL_field(relation, record, fieldId, &impure->vlu_desc))
		return NULL;

	// ASF: CORE-1432 - If the record is not on the latest format, upgrade it.
	// AP: for fields that are missing in original format use record's one.
	if (format &&
		record->getFormat()->fmt_version != format->fmt_version &&
		fieldId < format->fmt_desc.getCount() &&
		!format->fmt_desc[fieldId].isUnknown() &&
		!DSC_EQUIV(&impure->vlu_desc, &format->fmt_desc[fieldId], true))
	{
		dsc desc = impure->vlu_desc;
		impure->vlu_desc = format->fmt_desc[fieldId];

		if (impure->vlu_desc.isText())
		{
			// Allocate a string block of sufficient size.
			VaryingString* string = impure->vlu_string;

			if (string && string->str_length < impure->vlu_desc.dsc_length)
			{
				delete string;
				string = NULL;
			}

			if (!string)
			{
				string = impure->vlu_string = FB_NEW_RPT(*tdbb->getDefaultPool(),
					impure->vlu_desc.dsc_length) VaryingString();
				string->str_length = impure->vlu_desc.dsc_length;
			}

			impure->vlu_desc.dsc_address = string->str_data;
		}
		else
			impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc;

		MOV_move(tdbb, &desc, &impure->vlu_desc);
	}

	if (impure->vlu_desc.dsc_dtype == dtype_text)
		INTL_adjust_text_descriptor(tdbb, &impure->vlu_desc);

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<GenIdNode> regGenIdNode(blr_gen_id);
static RegisterNode<GenIdNode> regGenIdNode2(blr_gen_id2);

GenIdNode::GenIdNode(MemoryPool& pool, bool aDialect1,
					 const Firebird::MetaName& name,
					 ValueExprNode* aArg,
					 bool aImplicit, bool aIdentity)
	: TypedNode<ValueExprNode, ExprNode::TYPE_GEN_ID>(pool),
	  dialect1(aDialect1),
	  generator(pool, name),
	  arg(aArg),
	  step(0),
	  sysGen(false),
	  implicit(aImplicit),
	  identity(aIdentity)
{
}

DmlNode* GenIdNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	MetaName name;
	csb->csb_blr_reader.getMetaName(name);

	ValueExprNode* explicitStep = (blrOp == blr_gen_id2) ? NULL : PAR_parse_value(tdbb, csb);
	GenIdNode* const node =
		FB_NEW_POOL(pool) GenIdNode(pool, (csb->blrVersion == 4), name, explicitStep,
								(blrOp == blr_gen_id2), false);

	// This check seems faster than ==, but assumes the special generator is named ""
	if (name.length() == 0) //(name == MASTER_GENERATOR)
	{
		fb_assert(!MASTER_GENERATOR[0]);
		if (!(csb->csb_g_flags & csb_internal))
			PAR_error(csb, Arg::Gds(isc_gennotdef) << Arg::Str(name));

		node->generator.id = 0;
	}
	else if (!MET_load_generator(tdbb, node->generator, &node->sysGen, &node->step))
		PAR_error(csb, Arg::Gds(isc_gennotdef) << Arg::Str(name));

	if (csb->csb_g_flags & csb_get_dependencies)
	{
		CompilerScratch::Dependency dependency(obj_generator);
		dependency.number = node->generator.id;
		csb->csb_dependencies.push(dependency);
	}

	return node;
}

string GenIdNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, dialect1);
	NODE_PRINT(printer, generator);
	NODE_PRINT(printer, arg);
	NODE_PRINT(printer, step);
	NODE_PRINT(printer, sysGen);
	NODE_PRINT(printer, implicit);
	NODE_PRINT(printer, identity);

	return "GenIdNode";
}

ValueExprNode* GenIdNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	GenIdNode* const node = FB_NEW_POOL(dsqlScratch->getPool()) GenIdNode(dsqlScratch->getPool(),
		dialect1, generator.name, doDsqlPass(dsqlScratch, arg), implicit, identity);
	node->generator = generator;
	node->step = step;
	node->sysGen = sysGen;
	return node;
}

void GenIdNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = (implicit ? "NEXT_VALUE" : "GEN_ID");
}

bool GenIdNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, arg, desc, forceVarChar);
}

void GenIdNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (implicit)
	{
		dsqlScratch->appendUChar(blr_gen_id2);
		dsqlScratch->appendNullString(generator.name.c_str());
	}
	else
	{
		dsqlScratch->appendUChar(blr_gen_id);
		dsqlScratch->appendNullString(generator.name.c_str());
		GEN_expr(dsqlScratch, arg);
	}
}

void GenIdNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	if (!implicit)
	{
		dsc desc1;
		MAKE_desc(dsqlScratch, &desc1, arg);
	}

	if (dialect1)
		desc->makeLong(0);
	else
		desc->makeInt64(0);

	desc->setNullable(!implicit); // blr_gen_id2 cannot return NULL
}

void GenIdNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	if (dialect1)
		desc->makeLong(0);
	else
		desc->makeInt64(0);
}

ValueExprNode* GenIdNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	GenIdNode* const node = FB_NEW_POOL(*tdbb->getDefaultPool())
		GenIdNode(*tdbb->getDefaultPool(), dialect1, generator.name,
				  copier.copy(tdbb, arg), implicit, identity);
	node->generator = generator;
	node->step = step;
	node->sysGen = sysGen;
	return node;
}

bool GenIdNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const GenIdNode* o = nodeAs<GenIdNode>(other);
	fb_assert(o);

	// I'm not sure if I should include "implicit" in the comparison, but it means different BLR code
	// and nullable v/s not nullable.
	return dialect1 == o->dialect1 && generator.name == o->generator.name &&
		implicit == o->implicit;
}

bool GenIdNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const GenIdNode* const otherNode = nodeAs<GenIdNode>(other);
	fb_assert(otherNode);

	// I'm not sure if I should include "implicit" in the comparison, but it means different BLR code
	// and nullable v/s not nullable.
	return dialect1 == otherNode->dialect1 && generator.id == otherNode->generator.id &&
		implicit == otherNode->implicit;
}

ValueExprNode* GenIdNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass1(tdbb, csb);

	if (!identity)
	{
		CMP_post_access(tdbb, csb, generator.secName, 0,
						SCL_usage, SCL_object_generator, generator.name);
	}

	return this;
}

ValueExprNode* GenIdNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* GenIdNode::execute(thread_db* tdbb, jrd_req* request) const
{
	request->req_flags &= ~req_null;

	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	SINT64 change = step;

	if (!implicit)
	{
		const dsc* const value = EVL_expr(tdbb, request, arg);

		if (request->req_flags & req_null)
			return NULL;

		change = MOV_get_int64(tdbb, value, 0);
	}

	if (sysGen && change != 0)
	{
		if (!request->hasInternalStatement() && !tdbb->getAttachment()->isRWGbak())
			status_exception::raise(Arg::Gds(isc_cant_modify_sysobj) << "generator" << generator.name);
	}

	const SINT64 new_val = DPM_gen_id(tdbb, generator.id, false, change);

	if (dialect1)
		impure->make_long((SLONG) new_val);
	else
		impure->make_int64(new_val);

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<InternalInfoNode> regInternalInfoNode(blr_internal_info);

// CVC: If this list changes, gpre will need to be updated
const InternalInfoNode::InfoAttr InternalInfoNode::INFO_TYPE_ATTRIBUTES[MAX_INFO_TYPE] =
{
	{"<UNKNOWN>", 0},
	{"CURRENT_CONNECTION", 0},
	{"CURRENT_TRANSACTION", 0},
	{"GDSCODE", DsqlCompilerScratch::FLAG_BLOCK},
	{"SQLCODE", DsqlCompilerScratch::FLAG_BLOCK},
	{"ROW_COUNT", DsqlCompilerScratch::FLAG_BLOCK},
	{"INSERTING/UPDATING/DELETING", DsqlCompilerScratch::FLAG_TRIGGER},
	{"SQLSTATE", DsqlCompilerScratch::FLAG_BLOCK},
	{"EXCEPTION", DsqlCompilerScratch::FLAG_BLOCK},
	{"MESSAGE", DsqlCompilerScratch::FLAG_BLOCK}
};

InternalInfoNode::InternalInfoNode(MemoryPool& pool, ValueExprNode* aArg)
	: TypedNode<ValueExprNode, ExprNode::TYPE_INTERNAL_INFO>(pool),
	  arg(aArg)
{
}

DmlNode* InternalInfoNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	InternalInfoNode* node = FB_NEW_POOL(pool) InternalInfoNode(pool);

	const UCHAR* blrOffset = csb->csb_blr_reader.getPos();

	node->arg = PAR_parse_value(tdbb, csb);

	LiteralNode* literal = nodeAs<LiteralNode>(node->arg);

	if (!literal || literal->litDesc.dsc_dtype != dtype_long)
	{
		csb->csb_blr_reader.setPos(blrOffset + 1);	// PAR_syntax_error seeks 1 backward.
        PAR_syntax_error(csb, "integer literal");
	}

	return node;
}

string InternalInfoNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, arg);

	return "InternalInfoNode";
}

void InternalInfoNode::setParameterName(dsql_par* parameter) const
{
	SLONG infoType = nodeAs<LiteralNode>(arg)->getSlong();
	parameter->par_name = parameter->par_alias = INFO_TYPE_ATTRIBUTES[infoType].alias;
}

void InternalInfoNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_internal_info);
	GEN_expr(dsqlScratch, arg);
}

void InternalInfoNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	const InfoType infoType = static_cast<InfoType>(nodeAs<LiteralNode>(arg)->getSlong());

	switch (infoType)
	{
		case INFO_TYPE_SQLSTATE:
			desc->makeText(FB_SQLSTATE_LENGTH, ttype_ascii);
			break;

		case INFO_TYPE_EXCEPTION:
			desc->makeVarying(MAX_SQL_IDENTIFIER_LEN, ttype_metadata);
			break;

		case INFO_TYPE_ERROR_MSG:
			desc->makeVarying(MAX_ERROR_MSG_LENGTH, ttype_utf8);
			break;

		case INFO_TYPE_CONNECTION_ID:
		case INFO_TYPE_TRANSACTION_ID:
		case INFO_TYPE_ROWS_AFFECTED:
			desc->makeInt64(0);
			break;

		case INFO_TYPE_GDSCODE:
		case INFO_TYPE_SQLCODE:
		case INFO_TYPE_TRIGGER_ACTION:
			desc->makeLong(0);
			break;

		default:
			fb_assert(false);
	}
}

void InternalInfoNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	fb_assert(nodeIs<LiteralNode>(arg));

	dsc argDesc;
	arg->getDesc(tdbb, csb, &argDesc);
	fb_assert(argDesc.dsc_dtype == dtype_long);

	const InfoType infoType = static_cast<InfoType>(*reinterpret_cast<SLONG*>(argDesc.dsc_address));

	switch (infoType)
	{
		case INFO_TYPE_SQLSTATE:
			desc->makeText(FB_SQLSTATE_LENGTH, ttype_ascii);
			break;

		case INFO_TYPE_EXCEPTION:
			desc->makeVarying(MAX_SQL_IDENTIFIER_LEN, ttype_metadata);
			break;

		case INFO_TYPE_ERROR_MSG:
			desc->makeVarying(MAX_ERROR_MSG_LENGTH, ttype_utf8);
			break;

		case INFO_TYPE_CONNECTION_ID:
		case INFO_TYPE_TRANSACTION_ID:
		case INFO_TYPE_ROWS_AFFECTED:
			desc->makeInt64(0);
			break;

		case INFO_TYPE_GDSCODE:
		case INFO_TYPE_SQLCODE:
		case INFO_TYPE_TRIGGER_ACTION:
			desc->makeLong(0);
			break;

		default:
			fb_assert(false);
	}
}

ValueExprNode* InternalInfoNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	InternalInfoNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) InternalInfoNode(*tdbb->getDefaultPool());
	node->arg = copier.copy(tdbb, arg);
	return node;
}

ValueExprNode* InternalInfoNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

// Return a given element of the internal engine data.
dsc* InternalInfoNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	const dsc* value = EVL_expr(tdbb, request, arg);
	if (request->req_flags & req_null)
		return NULL;

	fb_assert(value->dsc_dtype == dtype_long);
	const InfoType infoType = static_cast<InfoType>(*reinterpret_cast<SLONG*>(value->dsc_address));

	if (infoType == INFO_TYPE_SQLSTATE)
	{
		FB_SQLSTATE_STRING sqlstate;
		request->req_last_xcp.as_sqlstate(sqlstate);

		dsc desc;
		desc.makeText(FB_SQLSTATE_LENGTH, ttype_ascii, (UCHAR*) sqlstate);
		EVL_make_value(tdbb, &desc, impure);

		return &impure->vlu_desc;
	}
	else if (infoType == INFO_TYPE_EXCEPTION)
	{
		if (request->req_last_xcp.success())
			return NULL;

		const SLONG xcpCode = request->req_last_xcp.as_xcpcode();

		if (!xcpCode)
			return NULL;

		MetaName xcpName;
		MET_lookup_exception(tdbb, xcpCode, xcpName, NULL);

		if (xcpName.isEmpty())
			return NULL;

		dsc desc;
		desc.makeText(xcpName.length(), ttype_metadata, (UCHAR*) xcpName.c_str());
		EVL_make_value(tdbb, &desc, impure);

		return &impure->vlu_desc;
	}
	else if (infoType == INFO_TYPE_ERROR_MSG)
	{
		if (request->req_last_xcp.success())
			return NULL;

		const string errorText = request->req_last_xcp.as_text();

		dsc desc;
		desc.makeText(errorText.length(), ttype_utf8, (UCHAR*) errorText.c_str());
		EVL_make_value(tdbb, &desc, impure);

		return &impure->vlu_desc;
	}

	SLONG result32 = 0;
	SINT64 result64 = 0;

	switch (infoType)
	{
		case INFO_TYPE_CONNECTION_ID:
			result64 = PAG_attachment_id(tdbb);
			break;
		case INFO_TYPE_TRANSACTION_ID:
			result64 = tdbb->getTransaction()->tra_number;
			break;
		case INFO_TYPE_GDSCODE:
			result32 = request->req_last_xcp.as_gdscode();
			break;
		case INFO_TYPE_SQLCODE:
			result32 = request->req_last_xcp.as_sqlcode();
			break;
		case INFO_TYPE_ROWS_AFFECTED:
			result64 = request->req_records_affected.getCount();
			break;
		case INFO_TYPE_TRIGGER_ACTION:
			result32 = request->req_trigger_action;
			break;
		default:
			SOFT_BUGCHECK(232);	// msg 232 EVL_expr: invalid operation
	}

	dsc desc;

	if (result64)
		desc.makeInt64(0, &result64);
	else
		desc.makeLong(0, &result32);

	EVL_make_value(tdbb, &desc, impure);
	return &impure->vlu_desc;
}

ValueExprNode* InternalInfoNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	SLONG infoType = nodeAs<LiteralNode>(arg)->getSlong();
	const InfoAttr& attr = INFO_TYPE_ATTRIBUTES[infoType];

	if (attr.mask && !(dsqlScratch->flags & attr.mask))
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			// Token unknown
			Arg::Gds(isc_token_err) <<
			Arg::Gds(isc_random) << attr.alias);
	}

	return FB_NEW_POOL(dsqlScratch->getPool()) InternalInfoNode(dsqlScratch->getPool(), doDsqlPass(dsqlScratch, arg));
}


//--------------------


static RegisterNode<LiteralNode> regLiteralNode(blr_literal);

LiteralNode::LiteralNode(MemoryPool& pool)
	: TypedNode<ValueExprNode, ExprNode::TYPE_LITERAL>(pool),
	  dsqlStr(NULL)
{
	litDesc.clear();
}

// Parse a literal value.
DmlNode* LiteralNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	LiteralNode* node = FB_NEW_POOL(pool) LiteralNode(pool);

	PAR_desc(tdbb, csb, &node->litDesc);

	USHORT l = node->litDesc.dsc_length;
	USHORT dataLen = l;
	if (node->litDesc.dsc_dtype == dtype_double && dataLen < sizeof(Decimal128))
		dataLen = sizeof(Decimal128);
	UCHAR* p = FB_NEW_POOL(csb->csb_pool) UCHAR[dataLen];
	node->litDesc.dsc_address = p;
	node->litDesc.dsc_flags = 0;
	const UCHAR* q = csb->csb_blr_reader.getPos();

	switch (node->litDesc.dsc_dtype)
	{
		case dtype_short:
			l = 2;
			*(SSHORT*) p = (SSHORT) gds__vax_integer(q, l);
			break;

		case dtype_long:
		case dtype_sql_date:
		case dtype_sql_time:
			l = 4;
			*(SLONG*) p = gds__vax_integer(q, l);
			break;

		case dtype_sql_time_tz:
			l = 6;
			*(SLONG*) p = gds__vax_integer(q, 4);
			p += 4;
			q += 4;
			*(SLONG*) p = gds__vax_integer(q, 2);
			break;

		case dtype_timestamp:
			l = 8;
			*(SLONG*) p = gds__vax_integer(q, 4);
			p += 4;
			q += 4;
			*(SLONG*) p = gds__vax_integer(q, 4);
			break;

		case dtype_timestamp_tz:
			l = 10;
			*(SLONG*) p = gds__vax_integer(q, 4);
			p += 4;
			q += 4;
			*(SLONG*) p = gds__vax_integer(q, 4);
			p += 4;
			q += 4;
			*(SLONG*) p = gds__vax_integer(q, 2);
			break;

		case dtype_int64:
			l = sizeof(SINT64);
			*(SINT64*) p = isc_portable_integer(q, l);
			break;

		case dtype_double:
		case dtype_dec128:
		{
			SSHORT scale;
			UCHAR dtype;

			// The double literal could potentially be used for any numeric literal - the value is
			// passed as if it were a text string. Convert the numeric string to its binary value
			// (int64, long or double as appropriate).

			l = csb->csb_blr_reader.getWord();
			q = csb->csb_blr_reader.getPos();
			dtype = CVT_get_numeric(q, l, &scale, p);
			node->litDesc.dsc_dtype = dtype;

			switch (dtype)
			{
				case dtype_double:
					node->litDesc.dsc_length = sizeof(double);
					break;
				case dtype_dec128:
					node->litDesc.dsc_length = sizeof(Decimal128);
					break;
				case dtype_long:
					node->litDesc.dsc_length = sizeof(SLONG);
					node->litDesc.dsc_scale = (SCHAR) scale;
					break;
				default:
					node->litDesc.dsc_length = sizeof(SINT64);
					node->litDesc.dsc_scale = (SCHAR) scale;
			}
			break;
		}

		case dtype_text:
			memcpy(p, q, l);
			break;

		case dtype_boolean:
			l = 1;
			*p = *q;
			break;

		default:
			fb_assert(FALSE);
	}

	csb->csb_blr_reader.seekForward(l);

	return node;
}

// Generate BLR for a constant.
void LiteralNode::genConstant(DsqlCompilerScratch* dsqlScratch, const dsc* desc, bool negateValue)
{
	SLONG value;
	SINT64 i64value;

	dsqlScratch->appendUChar(blr_literal);

	const UCHAR* p = desc->dsc_address;

	switch (desc->dsc_dtype)
	{
		case dtype_short:
			GEN_descriptor(dsqlScratch, desc, true);
			value = *(SSHORT*) p;
			if (negateValue)
				value = -value;
			dsqlScratch->appendUShort(value);
			break;

		case dtype_long:
			GEN_descriptor(dsqlScratch, desc, true);
			value = *(SLONG*) p;
			if (negateValue)
				value = -value;
			dsqlScratch->appendUShort(value);
			dsqlScratch->appendUShort(value >> 16);
			break;

		case dtype_sql_time:
		case dtype_sql_date:
			GEN_descriptor(dsqlScratch, desc, true);
			value = *(SLONG*) p;
			dsqlScratch->appendUShort(value);
			dsqlScratch->appendUShort(value >> 16);
			break;

		case dtype_sql_time_tz:
			GEN_descriptor(dsqlScratch, desc, true);
			value = *(SLONG*) p;
			dsqlScratch->appendUShort(value);
			dsqlScratch->appendUShort(value >> 16);
			value = *(SSHORT*) (p + 4);
			dsqlScratch->appendUShort(value);
			break;

		case dtype_double:
		case dtype_dec128:
		{
			// this is used for approximate/large numeric literal
			// which is transmitted to the engine as a string.

			GEN_descriptor(dsqlScratch, desc, true);

			// Length of string literal - keep it in sub_type which is unused
			const USHORT l = desc->dsc_sub_type;

			if (negateValue)
			{
				dsqlScratch->appendUShort(l + 1);
				dsqlScratch->appendUChar('-');
			}
			else
				dsqlScratch->appendUShort(l);

			if (l)
				dsqlScratch->appendBytes(p, l);

			break;
		}

		case dtype_int64:
			i64value = *(SINT64*) p;

			if (negateValue)
				i64value = -i64value;
			else if (i64value == MIN_SINT64)
			{
				// UH OH!
				// yylex correctly recognized the digits as the most-negative
				// possible INT64 value, but unfortunately, there was no
				// preceding '-' (a fact which the lexer could not know).
				// The value is too big for a positive INT64 value, and it
				// didn't contain an exponent so it's not a valid DOUBLE
				// PRECISION literal either, so we have to bounce it.

				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
						  Arg::Gds(isc_arith_except) <<
						  Arg::Gds(isc_numeric_out_of_range));
			}

			// We and the lexer both agree that this is an SINT64 constant,
			// and if the value needed to be negated, it already has been.
			// If the value will fit into a 32-bit signed integer, generate
			// it that way, else as an INT64.

			if ((i64value >= (SINT64) MIN_SLONG) && (i64value <= (SINT64) MAX_SLONG))
			{
				dsqlScratch->appendUChar(blr_long);
				dsqlScratch->appendUChar(desc->dsc_scale);
				dsqlScratch->appendUShort(i64value);
				dsqlScratch->appendUShort(i64value >> 16);
			}
			else
			{
				dsqlScratch->appendUChar(blr_int64);
				dsqlScratch->appendUChar(desc->dsc_scale);
				dsqlScratch->appendUShort(i64value);
				dsqlScratch->appendUShort(i64value >> 16);
				dsqlScratch->appendUShort(i64value >> 32);
				dsqlScratch->appendUShort(i64value >> 48);
			}
			break;

		case dtype_quad:
		case dtype_blob:
		case dtype_array:
		case dtype_timestamp:
			GEN_descriptor(dsqlScratch, desc, true);
			value = *(SLONG*) p;
			dsqlScratch->appendUShort(value);
			dsqlScratch->appendUShort(value >> 16);
			value = *(SLONG*) (p + 4);
			dsqlScratch->appendUShort(value);
			dsqlScratch->appendUShort(value >> 16);
			break;

		case dtype_timestamp_tz:
			GEN_descriptor(dsqlScratch, desc, true);
			value = *(SLONG*) p;
			dsqlScratch->appendUShort(value);
			dsqlScratch->appendUShort(value >> 16);
			value = *(SLONG*) (p + 4);
			dsqlScratch->appendUShort(value);
			dsqlScratch->appendUShort(value >> 16);
			value = *(SSHORT*) (p + 8);
			dsqlScratch->appendUShort(value);
			break;

		case dtype_text:
		{
			const USHORT length = desc->dsc_length;

			GEN_descriptor(dsqlScratch, desc, true);
			if (length)
				dsqlScratch->appendBytes(p, length);

			break;
		}

		case dtype_boolean:
			GEN_descriptor(dsqlScratch, desc, false);
			dsqlScratch->appendUChar(*p != 0);
			break;

		default:
			// gen_constant: datatype not understood
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-103) <<
					  Arg::Gds(isc_dsql_constant_err));
	}
}

string LiteralNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlStr);
	NODE_PRINT(printer, litDesc);

	return "LiteralNode";
}

// Turn an international string reference into internal subtype ID.
ValueExprNode* LiteralNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	thread_db* tdbb = JRD_get_thread_data();

	if (dsqlScratch->inOuterJoin)
		litDesc.dsc_flags = DSC_nullable;

	if (litDesc.dsc_dtype > dtype_any_text)
		return this;

	LiteralNode* constant = FB_NEW_POOL(dsqlScratch->getPool()) LiteralNode(dsqlScratch->getPool());
	constant->dsqlStr = dsqlStr;
	constant->litDesc = litDesc;

	if (dsqlStr && dsqlStr->getCharSet().hasData())
	{
		const dsql_intlsym* resolved = METD_get_charset(dsqlScratch->getTransaction(),
			dsqlStr->getCharSet().length(), dsqlStr->getCharSet().c_str());

		if (!resolved)
		{
			// character set name is not defined
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-504) <<
					  Arg::Gds(isc_charset_not_found) << dsqlStr->getCharSet());
		}

		constant->litDesc.setTextType(resolved->intlsym_ttype);
	}
	else
	{
		const MetaName charSetName = METD_get_charset_name(
			dsqlScratch->getTransaction(), constant->litDesc.getCharSet());

		const dsql_intlsym* sym = METD_get_charset(dsqlScratch->getTransaction(),
			charSetName.length(), charSetName.c_str());
		fb_assert(sym);

		if (sym)
			constant->litDesc.setTextType(sym->intlsym_ttype);
	}

	USHORT adjust = 0;

	if (constant->litDesc.dsc_dtype == dtype_varying)
		adjust = sizeof(USHORT);
	else if (constant->litDesc.dsc_dtype == dtype_cstring)
		adjust = 1;

	constant->litDesc.dsc_length -= adjust;

	CharSet* charSet = INTL_charset_lookup(tdbb, INTL_GET_CHARSET(&constant->litDesc));

	if (!charSet->wellFormed(dsqlStr->getString().length(), constant->litDesc.dsc_address, NULL))
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  Arg::Gds(isc_malformed_string));
	}
	else
	{
		ULONG charLength = charSet->length(
			dsqlStr->getString().length(), constant->litDesc.dsc_address, true);

		if (charLength > MAX_STR_SIZE / charSet->maxBytesPerChar())
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
					  Arg::Gds(isc_dsql_string_char_length) <<
					  Arg::Num(charLength) <<
					  Arg::Num(MAX_STR_SIZE / charSet->maxBytesPerChar()) <<
					  METD_get_charset_name(dsqlScratch->getTransaction(), constant->litDesc.getCharSet()));
		}
		else
			constant->litDesc.dsc_length = charLength * charSet->maxBytesPerChar();
	}

	constant->litDesc.dsc_length += adjust;

	return constant;
}

void LiteralNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "CONSTANT";
}

bool LiteralNode::setParameterType(DsqlCompilerScratch* /*dsqlScratch*/,
	const dsc* /*desc*/, bool /*forceVarChar*/)
{
	return false;
}

void LiteralNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (litDesc.dsc_dtype == dtype_text)
		litDesc.dsc_length = dsqlStr->getString().length();

	genConstant(dsqlScratch, &litDesc, false);
}

void LiteralNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	*desc = litDesc;
}

void LiteralNode::getDesc(thread_db* tdbb, CompilerScratch* /*csb*/, dsc* desc)
{
	*desc = litDesc;

	// ASF: I expect only dtype_text could occur here.
	// But I'll treat all string types for sure.
	if (DTYPE_IS_TEXT(desc->dsc_dtype))
	{
		const UCHAR* p;
		USHORT adjust = 0;

		if (desc->dsc_dtype == dtype_varying)
		{
			p = desc->dsc_address + sizeof(USHORT);
			adjust = sizeof(USHORT);
		}
		else
		{
			p = desc->dsc_address;

			if (desc->dsc_dtype == dtype_cstring)
				adjust = 1;
		}

		// Do the same thing which DSQL does.
		// Increase descriptor size to evaluate dependent expressions correctly.
		CharSet* cs = INTL_charset_lookup(tdbb, desc->getCharSet());
		desc->dsc_length = (cs->length(desc->dsc_length - adjust, p, true) *
			cs->maxBytesPerChar()) + adjust;
	}
}

ValueExprNode* LiteralNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	LiteralNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) LiteralNode(*tdbb->getDefaultPool());
	node->litDesc = litDesc;

	UCHAR* p = FB_NEW_POOL(*tdbb->getDefaultPool()) UCHAR[node->litDesc.dsc_length];
	node->litDesc.dsc_address = p;

	memcpy(p, litDesc.dsc_address, litDesc.dsc_length);

	return node;
}

bool LiteralNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const LiteralNode* o = nodeAs<LiteralNode>(other);
	fb_assert(o);

	if (!DSC_EQUIV(&litDesc, &o->litDesc, true))
		return false;

	const USHORT len = (litDesc.dsc_dtype == dtype_text) ?
		(USHORT) dsqlStr->getString().length() : litDesc.dsc_length;
	return memcmp(litDesc.dsc_address, o->litDesc.dsc_address, len) == 0;
}

bool LiteralNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const LiteralNode* const otherNode = nodeAs<LiteralNode>(other);
	fb_assert(otherNode);
	thread_db* tdbb = JRD_get_thread_data();

	try
	{
		return MOV_compare(tdbb, &litDesc, &otherNode->litDesc) == 0;
	}
	catch (const status_exception&)
	{
		fb_utils::init_status(tdbb->tdbb_status_vector);
		return false;
	}
}

ValueExprNode* LiteralNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* LiteralNode::execute(thread_db* /*tdbb*/, jrd_req* /*request*/) const
{
	return const_cast<dsc*>(&litDesc);
}

void LiteralNode::fixMinSInt64(MemoryPool& pool)
{
	// MIN_SINT64 should be stored as BIGINT, not DECFLOAT

	const UCHAR* s = litDesc.dsc_address;
	const char* minSInt64 = "9223372036854775808";
	bool hasDot = false;
	int scale = 0;

	for (const UCHAR* s = litDesc.dsc_address; *s; ++s)
	{
		if (*s == '.')
		{
			if (hasDot)
				return;
			hasDot = true;
		}
		else if (*s == *minSInt64++)
		{
			if (hasDot)
				scale--;
		}
		else
			return;
	}

	if (*minSInt64)
		return;

	SINT64* valuePtr = FB_NEW_POOL(pool) SINT64(QUADCONST(0x8000000000000000));
	litDesc.dsc_dtype = dtype_int64;
	litDesc.dsc_length = sizeof(SINT64);
	litDesc.dsc_scale = scale;
	litDesc.dsc_sub_type = 0;
	litDesc.dsc_address = reinterpret_cast<UCHAR*>(valuePtr);
}


//--------------------


static RegisterNode<LocalTimeNode> regLocalTimeNode(blr_local_time);

DmlNode* LocalTimeNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	unsigned precision = csb->csb_blr_reader.getByte();

	if (precision > MAX_TIME_PRECISION)
		ERR_post(Arg::Gds(isc_invalid_time_precision) << Arg::Num(MAX_TIME_PRECISION));

	return FB_NEW_POOL(pool) LocalTimeNode(pool, precision);
}

string LocalTimeNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, precision);

	return "LocalTimeNode";
}

void LocalTimeNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "LOCALTIME";
}

void LocalTimeNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_local_time);
	dsqlScratch->appendUChar(precision);
}

void LocalTimeNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	desc->dsc_dtype = dtype_sql_time;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

void LocalTimeNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->dsc_dtype = dtype_sql_time;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

ValueExprNode* LocalTimeNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) LocalTimeNode(*tdbb->getDefaultPool(), precision);
}

ValueExprNode* LocalTimeNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

ValueExprNode* LocalTimeNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	if (precision > MAX_TIME_PRECISION)
		ERRD_post(Arg::Gds(isc_invalid_time_precision) << Arg::Num(MAX_TIME_PRECISION));

	return this;
}

dsc* LocalTimeNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	// Use the request timestamp.
	fb_assert(!request->req_timestamp_utc.isEmpty());

	ISC_TIMESTAMP_TZ timeStampTz;
	timeStampTz.utc_timestamp = request->req_timestamp_utc.value();
	timeStampTz.time_zone = TimeZoneUtil::UTC_ZONE;

	impure->vlu_misc.vlu_sql_time = TimeZoneUtil::timeStampTzToTimeStamp(
		timeStampTz, request->req_attachment->att_current_timezone).timestamp_time;

	TimeStamp::round_time(impure->vlu_misc.vlu_sql_time, precision);

	memset(&impure->vlu_desc, 0, sizeof(impure->vlu_desc));
	impure->vlu_desc.dsc_dtype = dtype_sql_time;
	impure->vlu_desc.dsc_length = type_lengths[dtype_sql_time];
	impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_sql_time;

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<LocalTimeStampNode> regLocalTimeStampNode(blr_local_timestamp);

DmlNode* LocalTimeStampNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	unsigned precision = csb->csb_blr_reader.getByte();

	if (precision > MAX_TIME_PRECISION)
		ERR_post(Arg::Gds(isc_invalid_time_precision) << Arg::Num(MAX_TIME_PRECISION));

	return FB_NEW_POOL(pool) LocalTimeStampNode(pool, precision);
}

string LocalTimeStampNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, precision);

	return "LocalTimeStampNode";
}

void LocalTimeStampNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "LOCALTIMESTAMP";
}

void LocalTimeStampNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_local_timestamp);
	dsqlScratch->appendUChar(precision);
}

void LocalTimeStampNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	desc->dsc_dtype = dtype_timestamp;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

void LocalTimeStampNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->dsc_dtype = dtype_timestamp;
	desc->dsc_sub_type = 0;
	desc->dsc_scale = 0;
	desc->dsc_flags = 0;
	desc->dsc_length = type_lengths[desc->dsc_dtype];
}

ValueExprNode* LocalTimeStampNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) LocalTimeStampNode(*tdbb->getDefaultPool(), precision);
}

ValueExprNode* LocalTimeStampNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

ValueExprNode* LocalTimeStampNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	if (precision > MAX_TIME_PRECISION)
		ERRD_post(Arg::Gds(isc_invalid_time_precision) << Arg::Num(MAX_TIME_PRECISION));

	return this;
}

dsc* LocalTimeStampNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	// Use the request timestamp.
	fb_assert(!request->req_timestamp_utc.isEmpty());

	impure->vlu_misc.vlu_timestamp = request->getLocalTimeStamp().value();
	TimeStamp::round_time(impure->vlu_misc.vlu_timestamp.timestamp_time, precision);

	memset(&impure->vlu_desc, 0, sizeof(impure->vlu_desc));
	impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_timestamp;
	impure->vlu_desc.dsc_dtype = dtype_timestamp;
	impure->vlu_desc.dsc_length = type_lengths[dtype_timestamp];

	return &impure->vlu_desc;
}


//--------------------


string DsqlAliasNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, name);
	NODE_PRINT(printer, value);
	NODE_PRINT(printer, implicitJoin);

	return "DsqlAliasNode";
}

ValueExprNode* DsqlAliasNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	DsqlAliasNode* node = FB_NEW_POOL(dsqlScratch->getPool()) DsqlAliasNode(dsqlScratch->getPool(), name,
		doDsqlPass(dsqlScratch, value));
	MAKE_desc(dsqlScratch, &node->value->nodDesc, node->value);
	return node;
}

void DsqlAliasNode::setParameterName(dsql_par* parameter) const
{
	value->setParameterName(parameter);
	parameter->par_alias = name;
}

void DsqlAliasNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	GEN_expr(dsqlScratch, value);
}

void DsqlAliasNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc(dsqlScratch, desc, value);
}


//--------------------


DsqlMapNode::DsqlMapNode(MemoryPool& pool, dsql_ctx* aContext, dsql_map* aMap)
	: TypedNode<ValueExprNode, ExprNode::TYPE_MAP>(pool),
	  context(aContext),
	  map(aMap)
{
}

string DsqlMapNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, context);
	NODE_PRINT(printer, map);

	return "DsqlMapNode";
}

ValueExprNode* DsqlMapNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return FB_NEW_POOL(dsqlScratch->getPool()) DsqlMapNode(dsqlScratch->getPool(), context, map);
}

bool DsqlMapNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	if (visitor.window)
		return false;

	if (context->ctx_scope_level == visitor.dsqlScratch->scopeLevel)
		return true;

	return visitor.visit(map->map_node);
}

bool DsqlMapNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	return visitor.visit(map->map_node);
}

bool DsqlMapNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	// If that map is of the current scopeLevel, we prevent the visiting of the aggregate
	// expression. This is because a field embedded in an aggregate function is valid even
	// not being in the group by list. Examples:
	//   select count(n) from table group by m
	//   select count(n) from table

	AutoSetRestore<bool> autoInsideOwnMap(&visitor.insideOwnMap,
		context->ctx_scope_level == visitor.context->ctx_scope_level);

	// If the context scope is greater than our own, someone should have already inspected
	// nested aggregates, so set insideHigherMap to true.

	AutoSetRestore<bool> autoInsideHigherMap(&visitor.insideHigherMap,
		context->ctx_scope_level > visitor.context->ctx_scope_level);

	return visitor.visit(map->map_node);
}

bool DsqlMapNode::dsqlSubSelectFinder(SubSelectFinder& /*visitor*/)
{
	return false;
}

bool DsqlMapNode::dsqlFieldFinder(FieldFinder& visitor)
{
	return visitor.visit(map->map_node);
}

ValueExprNode* DsqlMapNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	if (context->ctx_scope_level != visitor.context->ctx_scope_level)
	{
		AutoSetRestore<USHORT> autoCurrentLevel(&visitor.currentLevel, context->ctx_scope_level);
		doDsqlFieldRemapper(visitor, map->map_node);
	}

	if (visitor.window && context->ctx_scope_level == visitor.context->ctx_scope_level)
		return PASS1_post_map(visitor.dsqlScratch, this, visitor.context, visitor.windowNode);

	return this;
}

void DsqlMapNode::setParameterName(dsql_par* parameter) const
{
	const ValueExprNode* nestNode = map->map_node;
	const DsqlMapNode* mapNode;

	while ((mapNode = nodeAs<DsqlMapNode>(nestNode)))
	{
		// Skip all the DsqlMapNodes.
		nestNode = mapNode->map->map_node;
	}

	const char* nameAlias = NULL;
	const FieldNode* fieldNode = NULL;
	const ValueExprNode* alias;

	const AggNode* aggNode;
	const DsqlAliasNode* aliasNode;
	const LiteralNode* literalNode;
	const DerivedFieldNode* derivedField;
	const RecordKeyNode* dbKeyNode;

	if ((aggNode = nodeAs<AggNode>(nestNode)))
		aggNode->setParameterName(parameter);
	else if ((aliasNode = nodeAs<DsqlAliasNode>(nestNode)))
	{
		parameter->par_alias = aliasNode->name;
		alias = aliasNode->value;
		fieldNode = nodeAs<FieldNode>(alias);
	}
	else if ((literalNode = nodeAs<LiteralNode>(nestNode)))
		literalNode->setParameterName(parameter);
	else if ((dbKeyNode = nodeAs<RecordKeyNode>(nestNode)))
		nameAlias = dbKeyNode->getAlias(false);
	else if ((derivedField = nodeAs<DerivedFieldNode>(nestNode)))
	{
		parameter->par_alias = derivedField->name;
		alias = derivedField->value;
		fieldNode = nodeAs<FieldNode>(alias);
	}
	else if ((fieldNode = nodeAs<FieldNode>(nestNode)))
		nameAlias = fieldNode->dsqlField->fld_name.c_str();

	const dsql_ctx* context = NULL;
	const dsql_fld* field;

	if (fieldNode)
	{
		context = fieldNode->dsqlContext;
		field = fieldNode->dsqlField;
		parameter->par_name = field->fld_name.c_str();
	}

	if (nameAlias)
		parameter->par_name = parameter->par_alias = nameAlias;

	setParameterInfo(parameter, context);
}

void DsqlMapNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_fid);

	if (map->map_window)
		dsqlScratch->appendUChar(map->map_window->context);
	else
		GEN_stuff_context(dsqlScratch, context);

	dsqlScratch->appendUShort(map->map_position);
}

void DsqlMapNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc(dsqlScratch, desc, map->map_node);

	// ASF: We should mark nod_agg_count as nullable when it's in an outer join - CORE-2660.
	if (context->ctx_flags & CTX_outer_join)
		desc->setNullable(true);
}

bool DsqlMapNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	const DsqlMapNode* o = nodeAs<DsqlMapNode>(other);
	return o && PASS1_node_match(dsqlScratch, map->map_node, o->map->map_node, ignoreMapCast);
}


//--------------------


DerivedFieldNode::DerivedFieldNode(MemoryPool& pool, const MetaName& aName, USHORT aScope,
			ValueExprNode* aValue)
	: TypedNode<ValueExprNode, ExprNode::TYPE_DERIVED_FIELD>(pool),
	  name(aName),
	  scope(aScope),
	  value(aValue),
	  context(NULL)
{
}

string DerivedFieldNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, name);
	NODE_PRINT(printer, scope);
	NODE_PRINT(printer, value);
	NODE_PRINT(printer, context);

	return "DerivedFieldNode";
}

ValueExprNode* DerivedFieldNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

bool DerivedFieldNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	// This is a derived table, so don't look further, but don't forget to check for the
	// deepest scope level.

	if (visitor.deepestLevel < scope)
		visitor.deepestLevel = scope;

	return false;
}

bool DerivedFieldNode::dsqlAggregate2Finder(Aggregate2Finder& /*visitor*/)
{
	return false;
}

bool DerivedFieldNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	if (scope == visitor.context->ctx_scope_level)
		return true;

	if (visitor.context->ctx_scope_level < scope)
		return visitor.visit(value);

	return false;
}

bool DerivedFieldNode::dsqlSubSelectFinder(SubSelectFinder& /*visitor*/)
{
	return false;
}

bool DerivedFieldNode::dsqlFieldFinder(FieldFinder& visitor)
{
	// This is a "virtual" field
	visitor.field = true;

	const USHORT dfScopeLevel = scope;

	switch (visitor.matchType)
	{
		case FIELD_MATCH_TYPE_EQUAL:
			return dfScopeLevel == visitor.checkScopeLevel;

		case FIELD_MATCH_TYPE_LOWER:
			return dfScopeLevel < visitor.checkScopeLevel;

		case FIELD_MATCH_TYPE_LOWER_EQUAL:
			return dfScopeLevel <= visitor.checkScopeLevel;

		///case FIELD_MATCH_TYPE_HIGHER:
		///	return dfScopeLevel > visitor.checkScopeLevel;

		///case FIELD_MATCH_TYPE_HIGHER_EQUAL:
		///	return dfScopeLevel >= visitor.checkScopeLevel;

		default:
			fb_assert(false);
	}

	return false;
}

ValueExprNode* DerivedFieldNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	// If we got a field from a derived table we should not remap anything
	// deeper in the alias, but this "virtual" field should be mapped to
	// the given context (of course only if we're in the same scope-level).

	if (scope == visitor.context->ctx_scope_level)
		return PASS1_post_map(visitor.dsqlScratch, this, visitor.context, visitor.windowNode);
	else if (visitor.context->ctx_scope_level < scope)
		doDsqlFieldRemapper(visitor, value);

	return this;
}

void DerivedFieldNode::setParameterName(dsql_par* parameter) const
{
	const dsql_ctx* context = NULL;
	const FieldNode* fieldNode = NULL;
	const RecordKeyNode* dbKeyNode = NULL;

	const DerivedFieldNode* drvField = nodeAs<DerivedFieldNode>(value);

	while (drvField)
	{
		if (fieldNode = nodeAs<FieldNode>(drvField->value))
			break;

		if (dbKeyNode = nodeAs<RecordKeyNode>(drvField->value))
			break;

		drvField = nodeAs<DerivedFieldNode>(drvField->value);
	}

	if (fieldNode || (fieldNode = nodeAs<FieldNode>(value)))
	{
		parameter->par_name = fieldNode->dsqlField->fld_name.c_str();
		context = fieldNode->dsqlContext;
	}
	else if (dbKeyNode || (dbKeyNode = nodeAs<RecordKeyNode>(value)))
		dbKeyNode->setParameterName(parameter);

	parameter->par_alias = name;
	setParameterInfo(parameter, context);

	parameter->par_rel_alias = this->context->ctx_alias;
}

void DerivedFieldNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	// ASF: If we are not referencing a field, we should evaluate the expression based on
	// a set (ORed) of contexts. If any of them are in a valid position the expression is
	// evaluated, otherwise a NULL will be returned. This is fix for CORE-1246.
	// Note that the field may be enclosed by an alias.

	ValueExprNode* val = value;

	while (nodeIs<DsqlAliasNode>(val))
		val = nodeAs<DsqlAliasNode>(val)->value;

	if (!nodeIs<FieldNode>(val) && !nodeIs<DerivedFieldNode>(val) &&
		!nodeIs<RecordKeyNode>(val) && !nodeIs<DsqlMapNode>(val))
	{
		if (context->ctx_main_derived_contexts.hasData())
		{
			HalfStaticArray<USHORT, 4> derivedContexts;

			for (DsqlContextStack::const_iterator stack(context->ctx_main_derived_contexts);
				 stack.hasData(); ++stack)
			{
				const dsql_ctx* const derivedContext = stack.object();

				if (derivedContext->ctx_win_maps.hasData())
				{
					for (auto& winMap : derivedContext->ctx_win_maps)
					{
						// bottleneck
						fb_assert(winMap->context <= MAX_UCHAR);
						derivedContexts.add(winMap->context);
					}
				}
				else
				{
					// bottleneck
					fb_assert(derivedContext->ctx_context <= MAX_UCHAR);
					derivedContexts.add(derivedContext->ctx_context);
				}
			}

			const FB_SIZE_T derivedContextsCount = derivedContexts.getCount();

			if (derivedContextsCount > MAX_UCHAR)
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
						  Arg::Gds(isc_imp_exc) <<
						  Arg::Gds(isc_ctx_too_big));
			}

			dsqlScratch->appendUChar(blr_derived_expr);
			dsqlScratch->appendUChar(derivedContextsCount);

			for (FB_SIZE_T i = 0; i < derivedContextsCount; i++)
				dsqlScratch->appendUChar(derivedContexts[i]);
		}
	}
	else if (!(dsqlScratch->flags & DsqlCompilerScratch::FLAG_FETCH) &&
			 !(context->ctx_flags & CTX_system) &&
			 (context->ctx_flags & CTX_cursor) &&
			 nodeIs<FieldNode>(val))
	{
		// ASF: FieldNode::execute does not verify rpb_number.isValid(), and due to system triggers
		// and also singular queries, we cannot start to do it. So to fix CORE-4488, we introduce
		// the usage of blr_derived_expr for cursor fields, which in practice prefixes the
		// FieldNode::execute by a test of rpb_number.isValid().
		dsqlScratch->appendUChar(blr_derived_expr);
		dsqlScratch->appendUChar(1);
		GEN_stuff_context(dsqlScratch, nodeAs<FieldNode>(val)->dsqlContext);
	}

	GEN_expr(dsqlScratch, value);
}

void DerivedFieldNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc(dsqlScratch, desc, value);
}


//--------------------


static RegisterNode<NegateNode> regNegateNode(blr_negate);

NegateNode::NegateNode(MemoryPool& pool, ValueExprNode* aArg)
	: TypedNode<ValueExprNode, ExprNode::TYPE_NEGATE>(pool),
	  arg(aArg)
{
	LiteralNode* literal = nodeAs<LiteralNode>(arg);
	if (literal && literal->litDesc.dsc_dtype == dtype_dec128)
		literal->fixMinSInt64(pool);
}

DmlNode* NegateNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	NegateNode* node = FB_NEW_POOL(pool) NegateNode(pool);
	node->arg = PAR_parse_value(tdbb, csb);
	return node;
}

string NegateNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, arg);

	return "NegateNode";
}

void NegateNode::setParameterName(dsql_par* parameter) const
{
	// CVC: For this to be a thorough check, we need to recurse over all nodes.
	// This means we should separate the code that sets aliases from
	// the rest of the functionality here in MAKE_parameter_names().
	// Otherwise, we need to test here for most of the other node types.
	// However, we need to be recursive only if we agree things like -gen_id()
	// should be given the GEN_ID alias, too.
	int level = 0;
	const ValueExprNode* innerNode = arg;
	const NegateNode* innerNegateNode;

	while ((innerNegateNode = nodeAs<NegateNode>(innerNode)))
	{
		innerNode = innerNegateNode->arg;
		++level;
	}

	if (nodeIs<NullNode>(innerNode) || nodeIs<LiteralNode>(innerNode))
		parameter->par_name = parameter->par_alias = "CONSTANT";
	else if (!level)
	{
		const ArithmeticNode* arithmeticNode = nodeAs<ArithmeticNode>(innerNode);

		if (arithmeticNode && (
			/*arithmeticNode->blrOp == blr_add ||
			arithmeticNode->blrOp == blr_subtract ||*/
			arithmeticNode->blrOp == blr_multiply ||
			arithmeticNode->blrOp == blr_divide))
		{
			parameter->par_name = parameter->par_alias = arithmeticNode->label.c_str();
		}
	}
}

bool NegateNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, arg, desc, forceVarChar);
}

void NegateNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	LiteralNode* literal = nodeAs<LiteralNode>(arg);

	if (literal && DTYPE_IS_NUMERIC(literal->litDesc.dsc_dtype))
		LiteralNode::genConstant(dsqlScratch, &literal->litDesc, true);
	else
	{
		dsqlScratch->appendUChar(blr_negate);
		GEN_expr(dsqlScratch, arg);
	}
}

void NegateNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc(dsqlScratch, desc, arg);

	if (nodeIs<NullNode>(arg))
	{
		// -NULL = NULL of INT
		desc->makeLong(0);
		desc->setNullable(true);
	}
	else
	{
		// In Dialect 2 or 3, a string can never partipate in negation
		// (use a specific cast instead)
		if (DTYPE_IS_TEXT(desc->dsc_dtype))
		{
			if (dsqlScratch->clientDialect >= SQL_DIALECT_V6_TRANSITION)
			{
				ERRD_post(Arg::Gds(isc_expression_eval_err) <<
						  Arg::Gds(isc_dsql_nostring_neg_dial3));
			}

			desc->dsc_dtype = dtype_double;
			desc->dsc_length = sizeof(double);
		}
		else if (DTYPE_IS_BLOB(desc->dsc_dtype))	// Forbid blobs and arrays
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
					  Arg::Gds(isc_dsql_no_blob_array));
		}
		else if (!DTYPE_IS_NUMERIC(desc->dsc_dtype))	// Forbid other not numeric datatypes
		{
			ERRD_post(Arg::Gds(isc_expression_eval_err) <<
					  Arg::Gds(isc_dsql_invalid_type_neg));
		}
	}
}

void NegateNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	arg->getDesc(tdbb, csb, desc);
	nodFlags = arg->nodFlags & (FLAG_DOUBLE | FLAG_DECFLOAT);

	if (desc->dsc_dtype == dtype_quad)
		IBERROR(224);	// msg 224 quad word arithmetic not supported
}

ValueExprNode* NegateNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	NegateNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) NegateNode(*tdbb->getDefaultPool());
	node->arg = copier.copy(tdbb, arg);
	return node;
}

ValueExprNode* NegateNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* NegateNode::execute(thread_db* tdbb, jrd_req* request) const
{
	request->req_flags &= ~req_null;

	const dsc* desc = EVL_expr(tdbb, request, arg);
	if (request->req_flags & req_null)
		return NULL;

	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	EVL_make_value(tdbb, desc, impure);

	switch (impure->vlu_desc.dsc_dtype)
	{
		case dtype_short:
			if (impure->vlu_misc.vlu_short == MIN_SSHORT)
				ERR_post(Arg::Gds(isc_exception_integer_overflow));
			impure->vlu_misc.vlu_short = -impure->vlu_misc.vlu_short;
			break;

		case dtype_long:
			if (impure->vlu_misc.vlu_long == MIN_SLONG)
				ERR_post(Arg::Gds(isc_exception_integer_overflow));
			impure->vlu_misc.vlu_long = -impure->vlu_misc.vlu_long;
			break;

		case dtype_real:
			impure->vlu_misc.vlu_float = -impure->vlu_misc.vlu_float;
			break;

		case DEFAULT_DOUBLE:
			impure->vlu_misc.vlu_double = -impure->vlu_misc.vlu_double;
			break;

		case dtype_dec64:
			impure->vlu_misc.vlu_dec64 = impure->vlu_misc.vlu_dec64.neg();
			break;

		case dtype_dec128:
			impure->vlu_misc.vlu_dec128 = impure->vlu_misc.vlu_dec128.neg();
			break;

		case dtype_dec_fixed:
			impure->vlu_misc.vlu_dec_fixed = impure->vlu_misc.vlu_dec_fixed.neg();
			break;

		case dtype_int64:
			if (impure->vlu_misc.vlu_int64 == MIN_SINT64)
				ERR_post(Arg::Gds(isc_exception_integer_overflow));
			impure->vlu_misc.vlu_int64 = -impure->vlu_misc.vlu_int64;
			break;

		default:
			impure->vlu_misc.vlu_double = -MOV_get_double(tdbb, &impure->vlu_desc);
			impure->vlu_desc.dsc_dtype = DEFAULT_DOUBLE;
			impure->vlu_desc.dsc_length = sizeof(double);
			impure->vlu_desc.dsc_scale = 0;
			impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_double;
	}

	return &impure->vlu_desc;
}

ValueExprNode* NegateNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return FB_NEW_POOL(dsqlScratch->getPool()) NegateNode(dsqlScratch->getPool(), doDsqlPass(dsqlScratch, arg));
}


//--------------------


static RegisterNode<NullNode> regNullNode(blr_null);

DmlNode* NullNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* /*csb*/,
	const UCHAR /*blrOp*/)
{
	return FB_NEW_POOL(pool) NullNode(pool);
}

string NullNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	return "NullNode";
}

void NullNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "CONSTANT";
}

void NullNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_null);
}

void NullNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	// This occurs when SQL statement specifies a literal NULL, eg:
	//  SELECT NULL FROM TABLE1;
	// As we don't have a <dtype_null, SQL_NULL> datatype pairing,
	// we don't know how to map this NULL to a host-language
	// datatype.  Therefore we now describe it as a
	// CHAR(1) CHARACTER SET NONE type.
	// No value will ever be sent back, as the value of the select
	// will be NULL - this is only for purposes of DESCRIBING
	// the statement.  Note that this mapping could be done in dsql.cpp
	// as part of the DESCRIBE statement - but I suspect other areas
	// of the code would break if this is declared dtype_unknown.
	//
	// ASF: We have SQL_NULL now, but don't use it here.

	desc->makeNullString();
}

void NullNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	desc->makeLong(0);
	desc->setNull();
}

ValueExprNode* NullNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) NullNode(*tdbb->getDefaultPool());
}

ValueExprNode* NullNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* NullNode::execute(thread_db* /*tdbb*/, jrd_req* /*request*/) const
{
	return NULL;
}


//--------------------


OrderNode::OrderNode(MemoryPool& pool, ValueExprNode* aValue)
	: DsqlNode(pool),
	  value(aValue),
	  descending(false),
	  nullsPlacement(NULLS_DEFAULT)
{
}

string OrderNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, value);
	NODE_PRINT(printer, descending);
	NODE_PRINT(printer, nullsPlacement);

	return "OrderNode";
}

OrderNode* OrderNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	OrderNode* node = FB_NEW_POOL(dsqlScratch->getPool()) OrderNode(dsqlScratch->getPool(),
		doDsqlPass(dsqlScratch, value));
	node->descending = descending;
	node->nullsPlacement = nullsPlacement;
	return node;
}

bool OrderNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const OrderNode* o = nodeAs<OrderNode>(other);

	return o && descending == o->descending && nullsPlacement == o->nullsPlacement;
}


//--------------------


bool WindowClause::Frame::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const Frame* const otherNode = nodeAs<Frame>(other);
	fb_assert(otherNode);

	return bound == otherNode->bound;
}

WindowClause::Frame* WindowClause::Frame::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ListExprNode::pass1(tdbb, csb);
	return this;
}

WindowClause::Frame* WindowClause::Frame::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ListExprNode::pass2(tdbb, csb);
	return this;
}

WindowClause::Frame* WindowClause::Frame::copy(thread_db* tdbb, NodeCopier& copier) const
{
	Frame* node = FB_NEW_POOL(*tdbb->getDefaultPool()) Frame(*tdbb->getDefaultPool(), bound);
	node->value = copier.copy(tdbb, value);
	return node;
}

//--------------------

bool WindowClause::FrameExtent::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const FrameExtent* const otherNode = nodeAs<FrameExtent>(other);
	fb_assert(otherNode);

	return unit == otherNode->unit;
}

WindowClause::FrameExtent* WindowClause::FrameExtent::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (frame1 && frame2)
	{
		if (frame1->bound == Frame::Bound::CURRENT_ROW && frame2->bound == Frame::Bound::PRECEDING)
		{
			status_exception::raise(
				Arg::Gds(isc_dsql_window_incompat_frames) << "CURRENT ROW" << "PRECEDING");
		}

		if (frame1->bound == Frame::Bound::FOLLOWING && frame2->bound != Frame::Bound::FOLLOWING)
		{
			status_exception::raise(
				Arg::Gds(isc_dsql_window_incompat_frames) <<
					"FOLLOWING" << "PRECEDING or CURRENT ROW");
		}
	}

	return FB_NEW_POOL(dsqlScratch->getPool()) FrameExtent(dsqlScratch->getPool(), unit,
		doDsqlPass(dsqlScratch, frame1),
		doDsqlPass(dsqlScratch, frame2));
}

WindowClause::FrameExtent* WindowClause::FrameExtent::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ListExprNode::pass1(tdbb, csb);
	return this;
}

WindowClause::FrameExtent* WindowClause::FrameExtent::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ListExprNode::pass2(tdbb, csb);
	return this;
}

WindowClause::FrameExtent* WindowClause::FrameExtent::copy(thread_db* tdbb, NodeCopier& copier) const
{
	FrameExtent* node = FB_NEW_POOL(*tdbb->getDefaultPool()) FrameExtent(
		*tdbb->getDefaultPool(), unit);
	node->frame1 = copier.copy(tdbb, frame1);
	node->frame2 = copier.copy(tdbb, frame2);
	return node;
}

//--------------------

WindowClause* WindowClause::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	NestConst<WindowClause> window;

	if (name)
	{
		fb_assert(dsqlScratch->context->hasData());
		dsql_ctx* context = dsqlScratch->context->object();

		if (!context->ctx_named_windows.get(*name, window))
		{
			ERRD_post(
				Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
				Arg::Gds(isc_dsql_window_not_found) << *name);
		}

		if (partition)
		{
			ERRD_post(
				Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
				Arg::Gds(isc_dsql_window_cant_overr_part) << *name);
		}

		if (order && window->order)
		{
			ERRD_post(
				Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
				Arg::Gds(isc_dsql_window_cant_overr_order) << *name);
		}

		if (window->extent)
		{
			ERRD_post(
				Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
				Arg::Gds(isc_dsql_window_cant_overr_frame) << *name);
		}
	}
	else
		window = this;

	WindowClause* node = FB_NEW_POOL(dsqlScratch->getPool()) WindowClause(dsqlScratch->getPool(),
		window->name,
		doDsqlPass(dsqlScratch, window->partition),
		doDsqlPass(dsqlScratch, window->order),
		doDsqlPass(dsqlScratch, window->extent),
		window->exclusion);

	if (node->order && node->extent && node->extent->unit == FrameExtent::Unit::RANGE &&
		(node->extent->frame1->value || (node->extent->frame2 && node->extent->frame2->value)))
	{
		if (node->order->items.getCount() > 1)
		{
			status_exception::raise(
				Arg::Gds(isc_dsql_window_range_multi_key));
		}
		else
		{
			OrderNode* key = nodeAs<OrderNode>(node->order->items[0]);
			fb_assert(key);

			dsc desc;
			MAKE_desc(dsqlScratch, &desc, key->value);

			if (!desc.isDateTime() && !desc.isNumeric())
			{
				status_exception::raise(
					Arg::Gds(isc_dsql_window_range_inv_key_type));
			}
		}
	}

	if (node->extent)
	{
		for (unsigned i = 0; i < 2; ++i)
		{
			WindowClause::Frame* frame = i == 0 ? node->extent->frame1 : node->extent->frame2;

			if (frame && frame->value)
			{
				dsc desc;
				MAKE_desc(dsqlScratch, &desc, frame->value);

				if (!desc.isNumeric())
				{
					status_exception::raise(
						Arg::Gds(isc_dsql_window_frame_value_inv_type));
				}
			}
		}
	}

	return node;
}


//--------------------


OverNode::OverNode(MemoryPool& pool, AggNode* aAggExpr, const MetaName* aWindowName)
	: TypedNode<ValueExprNode, ExprNode::TYPE_OVER>(pool),
	  aggExpr(aAggExpr),
	  windowName(aWindowName),
	  window(NULL)
{
}

OverNode::OverNode(MemoryPool& pool, AggNode* aAggExpr, WindowClause* aWindow)
	: TypedNode<ValueExprNode, ExprNode::TYPE_OVER>(pool),
	  aggExpr(aAggExpr),
	  windowName(NULL),
	  window(aWindow)
{
}

string OverNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, aggExpr);
	NODE_PRINT(printer, window);

	return "OverNode";
}

bool OverNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	bool aggregate = false;
	const bool wereWindow = visitor.window;
	AutoSetRestore<bool> autoWindow(&visitor.window, false);

	if (!wereWindow)
	{
		NodeRefsHolder holder(visitor.getPool());
		aggExpr->getChildren(holder, true);

		for (auto& child : holder.refs)
			aggregate |= visitor.visit(child->getExpr());
	}
	else
		aggregate |= visitor.visit(aggExpr);

	aggregate |= visitor.visit(window);

	return aggregate;
}

bool OverNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	bool found = false;

	{	// scope
		AutoSetRestore<bool> autoWindowOnly(&visitor.windowOnly, false);
		found |= visitor.visit(aggExpr);
	}

	found |= visitor.visit(window);

	return found;
}

bool OverNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	bool invalid = false;

	// It's allowed to use an aggregate function of our context inside window functions.
	AutoSetRestore<bool> autoInsideHigherMap(&visitor.insideHigherMap, true);

	invalid |= visitor.visit(aggExpr);
	invalid |= visitor.visit(window);

	return invalid;
}

bool OverNode::dsqlSubSelectFinder(SubSelectFinder& /*visitor*/)
{
	return false;
}

ValueExprNode* OverNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	// Save the values to restore them in the end.
	AutoSetRestore<WindowClause*> autoWindowNode(&visitor.windowNode, visitor.windowNode);

	if (Aggregate2Finder::find(visitor.getPool(), visitor.context->ctx_scope_level, FIELD_MATCH_TYPE_EQUAL,
			true, window))
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  Arg::Gds(isc_dsql_agg_nested_err));
	}

	visitor.windowNode = window;

	// Before remap, aggExpr must always be an AggNode;
	AggNode* aggNode = static_cast<AggNode*>(aggExpr.getObject());

	NodeRefsHolder holder(visitor.getPool());
	aggNode->getChildren(holder, true);

	for (auto& child : holder.refs)
	{
		if (Aggregate2Finder::find(visitor.getPool(), visitor.context->ctx_scope_level, FIELD_MATCH_TYPE_EQUAL,
				true, child->getExpr()))
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
					  Arg::Gds(isc_dsql_agg_nested_err));
		}
	}

	AggregateFinder aggFinder(visitor.getPool(), visitor.dsqlScratch, false);
	aggFinder.deepestLevel = visitor.dsqlScratch->scopeLevel;
	aggFinder.currentLevel = visitor.currentLevel;

	if (aggFinder.visit(aggNode))
	{
		if (!visitor.window)
		{
			AutoSetRestore<WindowClause*> autoWindowNode2(&visitor.windowNode, NULL);

			NodeRefsHolder holder(visitor.getPool());
			aggNode->getChildren(holder, true);

			for (auto& child : holder.refs)
				child->remap(visitor);

			doDsqlFieldRemapper(visitor, window);
		}
		else if (visitor.dsqlScratch->scopeLevel == aggFinder.deepestLevel)
		{
			return PASS1_post_map(visitor.dsqlScratch, aggNode, visitor.context,
				visitor.windowNode);
		}
	}

	return this;
}

void OverNode::setParameterName(dsql_par* parameter) const
{
	MAKE_parameter_names(parameter, aggExpr);
}

void OverNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	GEN_expr(dsqlScratch, aggExpr);
}

void OverNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc(dsqlScratch, desc, aggExpr);
	desc->setNullable(true);
}

void OverNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* /*desc*/)
{
	fb_assert(false);
}

ValueExprNode* OverNode::copy(thread_db* /*tdbb*/, NodeCopier& /*copier*/) const
{
	fb_assert(false);
	return NULL;
}

dsc* OverNode::execute(thread_db* /*tdbb*/, jrd_req* /*request*/) const
{
	fb_assert(false);
	return NULL;
}

ValueExprNode* OverNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	NestConst<WindowClause> refWindow;

	if (windowName)
	{
		fb_assert(dsqlScratch->context->hasData());
		dsql_ctx* context = dsqlScratch->context->object();

		if (!context->ctx_named_windows.get(*windowName, refWindow))
		{
			ERRD_post(
				Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
				Arg::Gds(isc_dsql_window_not_found) << *windowName);
		}
	}
	else
		refWindow = window;

	OverNode* node = FB_NEW_POOL(dsqlScratch->getPool()) OverNode(dsqlScratch->getPool(),
		static_cast<AggNode*>(doDsqlPass(dsqlScratch, aggExpr)), doDsqlPass(dsqlScratch, refWindow));

	const AggNode* aggNode = nodeAs<AggNode>(node->aggExpr);

	if (node->window &&
		node->window->extent &&
		aggNode &&
		(aggNode->getCapabilities() & AggNode::CAP_RESPECTS_WINDOW_FRAME) !=
			AggNode::CAP_RESPECTS_WINDOW_FRAME)
	{
		node->window->extent = WindowClause::FrameExtent::createDefault(dsqlScratch->getPool());
		node->window->exclusion = WindowClause::Exclusion::NO_OTHERS;
	}

	return node;
}


//--------------------


static RegisterNode<ParameterNode> regParameterNode(blr_parameter);
static RegisterNode<ParameterNode> regParameterNode2(blr_parameter2);
static RegisterNode<ParameterNode> regParameterNode3(blr_parameter3);

ParameterNode::ParameterNode(MemoryPool& pool)
	: TypedNode<ValueExprNode, ExprNode::TYPE_PARAMETER>(pool),
	  dsqlParameterIndex(0),
	  dsqlParameter(NULL),
	  message(NULL),
	  argNumber(0),
	  argFlag(NULL),
	  argIndicator(NULL),
	  argInfo(NULL)
{
}

DmlNode* ParameterNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	MessageNode* message = NULL;
	USHORT n = csb->csb_blr_reader.getByte();

	if (n >= csb->csb_rpt.getCount() || !(message = csb->csb_rpt[n].csb_message))
		PAR_error(csb, Arg::Gds(isc_badmsgnum));

	ParameterNode* node = FB_NEW_POOL(pool) ParameterNode(pool);

	node->message = message;
	node->argNumber = csb->csb_blr_reader.getWord();

	const Format* format = message->format;

	if (node->argNumber >= format->fmt_count)
		PAR_error(csb, Arg::Gds(isc_badparnum));

	if (blrOp != blr_parameter)
	{
		ParameterNode* flagNode = FB_NEW_POOL(pool) ParameterNode(pool);
		flagNode->message = message;
		flagNode->argNumber = csb->csb_blr_reader.getWord();

		if (flagNode->argNumber >= format->fmt_count)
			PAR_error(csb, Arg::Gds(isc_badparnum));

		node->argFlag = flagNode;
	}

	if (blrOp == blr_parameter3)
	{
		ParameterNode* indicatorNode = FB_NEW_POOL(pool) ParameterNode(pool);
		indicatorNode->message = message;
		indicatorNode->argNumber = csb->csb_blr_reader.getWord();

		if (indicatorNode->argNumber >= format->fmt_count)
			PAR_error(csb, Arg::Gds(isc_badparnum));

		node->argIndicator = indicatorNode;
	}

	return node;
}

string ParameterNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlParameterIndex);
	NODE_PRINT(printer, dsqlParameter);
	NODE_PRINT(printer, message);
	NODE_PRINT(printer, argNumber);
	NODE_PRINT(printer, argFlag);
	NODE_PRINT(printer, argIndicator);
	NODE_PRINT(printer, argInfo);

	return "ParameterNode";
}

ValueExprNode* ParameterNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (dsqlScratch->isPsql())
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
					Arg::Gds(isc_dsql_command_err));
	}

	dsql_msg* tempMsg = dsqlParameter ?
		dsqlParameter->par_message : dsqlScratch->getStatement()->getSendMsg();

	ParameterNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ParameterNode(dsqlScratch->getPool());
	node->dsqlParameter = MAKE_parameter(tempMsg, true, true, dsqlParameterIndex, NULL);
	node->dsqlParameterIndex = dsqlParameterIndex;

	return node;
}

bool ParameterNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	thread_db* tdbb = JRD_get_thread_data();

	const dsc oldDesc = dsqlParameter->par_desc;

	if (!desc)
		dsqlParameter->par_desc.makeNullString();
	else
	{
		dsqlParameter->par_desc = *desc;

		if (tdbb->getCharSet() != CS_NONE && tdbb->getCharSet() != CS_BINARY)
		{
			const USHORT fromCharSet = dsqlParameter->par_desc.getCharSet();
			const USHORT toCharSet = (fromCharSet == CS_NONE || fromCharSet == CS_BINARY) ?
				fromCharSet : tdbb->getCharSet();

			if (dsqlParameter->par_desc.dsc_dtype <= dtype_any_text)
			{
				int diff = 0;

				switch (dsqlParameter->par_desc.dsc_dtype)
				{
					case dtype_varying:
						diff = sizeof(USHORT);
						break;
					case dtype_cstring:
						diff = 1;
						break;
				}

				dsqlParameter->par_desc.dsc_length -= diff;

				if (toCharSet != fromCharSet)
				{
					const USHORT fromCharSetBPC = METD_get_charset_bpc(
						dsqlScratch->getTransaction(), fromCharSet);
					const USHORT toCharSetBPC = METD_get_charset_bpc(
						dsqlScratch->getTransaction(), toCharSet);

					dsqlParameter->par_desc.setTextType(INTL_CS_COLL_TO_TTYPE(toCharSet,
						(fromCharSet == toCharSet ? INTL_GET_COLLATE(&dsqlParameter->par_desc) : 0)));

					dsqlParameter->par_desc.dsc_length = UTLD_char_length_to_byte_length(
						dsqlParameter->par_desc.dsc_length / fromCharSetBPC, toCharSetBPC, diff);
				}

				dsqlParameter->par_desc.dsc_length += diff;
			}
			else if (dsqlParameter->par_desc.dsc_dtype == dtype_blob &&
				dsqlParameter->par_desc.dsc_sub_type == isc_blob_text &&
				fromCharSet != CS_NONE && fromCharSet != CS_BINARY)
			{
				dsqlParameter->par_desc.setTextType(toCharSet);
			}
		}
	}

	if (!dsqlParameter)
	{
		dsqlParameter = MAKE_parameter(dsqlScratch->getStatement()->getSendMsg(), true, true,
			dsqlParameterIndex, NULL);
		dsqlParameterIndex = dsqlParameter->par_index;
	}

	// In case of RETURNING in MERGE and UPDATE OR INSERT, a single parameter is used in
	// more than one place. So we save it to use below.
	const bool hasOldDesc = dsqlParameter->par_node != NULL;

	dsqlParameter->par_node = this;

	// Parameters should receive precisely the data that the user
	// passes in.  Therefore for text strings lets use varying
	// strings to insure that we don't add trailing blanks.

	// However, there are situations this leads to problems - so
	// we use the forceVarChar parameter to prevent this
	// datatype assumption from occuring.

	if (forceVarChar)
	{
		if (dsqlParameter->par_desc.dsc_dtype == dtype_text)
		{
			dsqlParameter->par_desc.dsc_dtype = dtype_varying;
			// The error msgs is inaccurate, but causing dsc_length
			// to be outsise range can be worse.
			if (dsqlParameter->par_desc.dsc_length > MAX_VARY_COLUMN_SIZE)
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
							//Arg::Gds(isc_dsql_datatype_err)
							Arg::Gds(isc_imp_exc));
							//Arg::Gds(isc_field_name) << Arg::Str(parameter->par_name)
			}

			dsqlParameter->par_desc.dsc_length += sizeof(USHORT);
		}
		else if (!dsqlParameter->par_desc.isText() && !dsqlParameter->par_desc.isBlob())
		{
			const USHORT toCharSetBPC = METD_get_charset_bpc(
				dsqlScratch->getTransaction(), tdbb->getCharSet());

			// The LIKE & similar parameters must be varchar type
			// strings - so force this parameter to be varchar
			// and take a guess at a good length for it.
			dsqlParameter->par_desc.dsc_dtype = dtype_varying;
			dsqlParameter->par_desc.dsc_length = LIKE_PARAM_LEN * toCharSetBPC + sizeof(USHORT);
			dsqlParameter->par_desc.dsc_sub_type = 0;
			dsqlParameter->par_desc.dsc_scale = 0;
			dsqlParameter->par_desc.setTextType(tdbb->getCharSet());
		}
	}

	if (hasOldDesc)
	{
		dsc thisDesc = dsqlParameter->par_desc;
		const dsc* args[] = {&oldDesc, &thisDesc};
		DSqlDataTypeUtil(dsqlScratch).makeFromList(&dsqlParameter->par_desc,
			dsqlParameter->par_name.c_str(), 2, args);
	}

	return true;
}

void ParameterNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	GEN_parameter(dsqlScratch, dsqlParameter);
}

void ParameterNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	// We don't actually know the datatype of a parameter -
	// we have to guess it based on the context that the
	// parameter appears in. (This is done is pass1.c::set_parameter_type())
	// However, a parameter can appear as part of an expression.
	// As MAKE_desc is used for both determination of parameter
	// types and for expression type checking, we just continue.

	if (dsqlParameter->par_desc.dsc_dtype)
		*desc = dsqlParameter->par_desc;
}

bool ParameterNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool /*ignoreMapCast*/) const
{
	const ParameterNode* o = nodeAs<ParameterNode>(other);

	return o && dsqlParameter->par_index == o->dsqlParameter->par_index;
}

void ParameterNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	*desc = message->format->fmt_desc[argNumber];
	// Must reset dsc_address because it's used in others places to read literals, but here it was
	// an offset in the message.
	desc->dsc_address = NULL;
}

ValueExprNode* ParameterNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	ParameterNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) ParameterNode(*tdbb->getDefaultPool());
	node->argNumber = argNumber;

	// dimitr:	IMPORTANT!!!
	// nod_message copying must be done in the only place
	// (the nod_procedure code). Hence we don't call
	// copy() here to keep argument->nod_arg[e_arg_message]
	// and procedure->nod_arg[e_prc_in_msg] in sync. The
	// message is passed to copy() as a parameter. If the
	// passed message is NULL, it means nod_argument is
	// cloned outside nod_procedure (e.g. in the optimizer)
	// and we must keep the input message.
	// ASF: We should only use "message" if its number matches the number
	// in nod_argument. If it doesn't, it may be an input parameter cloned
	// in RseBoolNode::convertNeqAllToNotAny - see CORE-3094.

	if (copier.message && copier.message->messageNumber == message->messageNumber)
		node->message = copier.message;
	else
		node->message = message;

	node->argFlag = copier.copy(tdbb, argFlag);
	node->argIndicator = copier.copy(tdbb, argIndicator);

	return node;
}

ValueExprNode* ParameterNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	argInfo = CMP_pass2_validation(tdbb, csb,
		Item(Item::TYPE_PARAMETER, message->messageNumber, argNumber));

	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, (nodFlags & FLAG_VALUE) ? sizeof(impure_value_ex) : sizeof(dsc));

	return this;
}

dsc* ParameterNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	const dsc* desc;

	if (argFlag)
	{
		desc = EVL_expr(tdbb, request, argFlag);
		if (MOV_get_long(tdbb, desc, 0))
			request->req_flags |= req_null;
	}

	desc = &message->format->fmt_desc[argNumber];

	impure->vlu_desc.dsc_address = request->getImpure<UCHAR>(
		message->impureOffset + (IPTR) desc->dsc_address);
	impure->vlu_desc.dsc_dtype = desc->dsc_dtype;
	impure->vlu_desc.dsc_length = desc->dsc_length;
	impure->vlu_desc.dsc_scale = desc->dsc_scale;
	impure->vlu_desc.dsc_sub_type = desc->dsc_sub_type;

	if (impure->vlu_desc.dsc_dtype == dtype_text)
		INTL_adjust_text_descriptor(tdbb, &impure->vlu_desc);

	USHORT* impure_flags = request->getImpure<USHORT>(
		message->impureFlags + (sizeof(USHORT) * argNumber));

	if (!(*impure_flags & VLU_checked))
	{
		if (!(request->req_flags & req_null))
		{
			USHORT maxLen = desc->dsc_length;	// not adjusted length
			desc = &impure->vlu_desc;

			if (DTYPE_IS_TEXT(desc->dsc_dtype))
			{
				const UCHAR* p = desc->dsc_address;
				USHORT len;

				switch (desc->dsc_dtype)
				{
					case dtype_cstring:
						len = strnlen((const char*) p, maxLen);
						--maxLen;
						break;

					case dtype_text:
						len = desc->dsc_length;
						break;

					case dtype_varying:
						len = reinterpret_cast<const vary*>(p)->vary_length;
						p += sizeof(USHORT);
						maxLen -= sizeof(USHORT);
						break;
				}

				CharSet* charSet = INTL_charset_lookup(tdbb, DSC_GET_CHARSET(desc));

				EngineCallbacks::instance->validateData(charSet, len, p);
				EngineCallbacks::instance->validateLength(charSet, len, p, maxLen);
			}
			else if (desc->isBlob())
			{
				const bid* const blobId = reinterpret_cast<bid*>(desc->dsc_address);

				if (!blobId->isEmpty())
				{
					if (!request->hasInternalStatement())
						tdbb->getTransaction()->checkBlob(tdbb, blobId);

					if (desc->getCharSet() != CS_NONE && desc->getCharSet() != CS_BINARY)
					{
						AutoBlb blob(tdbb, blb::open(tdbb, tdbb->getTransaction(), blobId));
						blob.getBlb()->BLB_check_well_formed(tdbb, desc);
					}
				}
			}
		}

		if (argInfo)
		{
			EVL_validate(tdbb, Item(Item::TYPE_PARAMETER, message->messageNumber, argNumber),
				argInfo, &impure->vlu_desc, request->req_flags & req_null);
		}

		*impure_flags |= VLU_checked;
	}

	return (request->req_flags & req_null) ? NULL : &impure->vlu_desc;
}


//--------------------


static RegisterNode<RecordKeyNode> regRecordKeyNodeDbKey(blr_dbkey);
static RegisterNode<RecordKeyNode> regRecordKeyNodeRecordVersion(blr_record_version);
static RegisterNode<RecordKeyNode> regRecordKeyNodeRecordVersion2(blr_record_version2);

RecordKeyNode::RecordKeyNode(MemoryPool& pool, UCHAR aBlrOp, const MetaName& aDsqlQualifier)
	: TypedNode<ValueExprNode, ExprNode::TYPE_RECORD_KEY>(pool),
	  blrOp(aBlrOp),
	  dsqlQualifier(pool, aDsqlQualifier),
	  dsqlRelation(NULL),
	  recStream(0),
	  aggregate(false)
{
	fb_assert(blrOp == blr_dbkey || blrOp == blr_record_version || blrOp == blr_record_version2);
}

DmlNode* RecordKeyNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	RecordKeyNode* node = FB_NEW_POOL(pool) RecordKeyNode(pool, blrOp);

	node->recStream = csb->csb_blr_reader.getByte();

	if (node->recStream >= csb->csb_rpt.getCount() || !(csb->csb_rpt[node->recStream].csb_flags & csb_used))
		PAR_error(csb, Arg::Gds(isc_ctxnotdef));

	node->recStream = csb->csb_rpt[node->recStream].csb_stream;

	return node;
}

string RecordKeyNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, blrOp);
	NODE_PRINT(printer, dsqlQualifier);
	NODE_PRINT(printer, dsqlRelation);
	NODE_PRINT(printer, recStream);
	NODE_PRINT(printer, aggregate);

	return "RecordKeyNode";
}

// Resolve a dbkey to an available context.
ValueExprNode* RecordKeyNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	thread_db* tdbb = JRD_get_thread_data();

	if (dsqlQualifier.isEmpty())
	{
		DsqlContextStack contexts;

		for (DsqlContextStack::iterator stack(*dsqlScratch->context); stack.hasData(); ++stack)
		{
			dsql_ctx* context = stack.object();
			if ((context->ctx_flags & (CTX_system | CTX_returning)) == CTX_system ||
				context->ctx_scope_level != dsqlScratch->scopeLevel)
			{
				continue;
			}

			if (context->ctx_relation)
				contexts.push(context);
		}

		if (contexts.hasData())
		{
			dsql_ctx* context = contexts.object();

			if (!context->ctx_relation)
				raiseError(context);

			if (context->ctx_flags & CTX_null)
				return FB_NEW_POOL(*tdbb->getDefaultPool()) NullNode(*tdbb->getDefaultPool());

			PASS1_ambiguity_check(dsqlScratch, getAlias(true), contexts);

			RelationSourceNode* relNode = FB_NEW_POOL(dsqlScratch->getPool()) RelationSourceNode(
				dsqlScratch->getPool());
			relNode->dsqlContext = context;

			RecordKeyNode* node = FB_NEW_POOL(dsqlScratch->getPool()) RecordKeyNode(dsqlScratch->getPool(), blrOp);
			node->dsqlRelation = relNode;

			return node;
		}
	}
	else
	{
		const bool cfgRlxAlias = Config::getRelaxedAliasChecking();
		bool rlxAlias = false;

		for (;;)
		{
			for (DsqlContextStack::iterator stack(*dsqlScratch->context); stack.hasData(); ++stack)
			{
				dsql_ctx* context = stack.object();

				if ((!context->ctx_relation ||
						context->ctx_relation->rel_name != dsqlQualifier ||
						!rlxAlias && context->ctx_internal_alias.hasData()) &&
					(context->ctx_internal_alias.isEmpty() ||
						strcmp(dsqlQualifier.c_str(), context->ctx_internal_alias.c_str()) != 0))
				{
					continue;
				}

				if (!context->ctx_relation)
					raiseError(context);

				if (context->ctx_flags & CTX_null)
					return FB_NEW_POOL(*tdbb->getDefaultPool()) NullNode(*tdbb->getDefaultPool());

				RelationSourceNode* relNode = FB_NEW_POOL(dsqlScratch->getPool()) RelationSourceNode(
					dsqlScratch->getPool());
				relNode->dsqlContext = context;

				RecordKeyNode* node = FB_NEW_POOL(dsqlScratch->getPool()) RecordKeyNode(dsqlScratch->getPool(), blrOp);
				node->dsqlRelation = relNode;

				return node;
			}

			if (rlxAlias == cfgRlxAlias)
				break;

			rlxAlias = cfgRlxAlias;
		}
	}

	// Field unresolved.
	PASS1_field_unknown(dsqlQualifier.nullStr(), getAlias(false), this);

	return NULL;
}

bool RecordKeyNode::dsqlAggregate2Finder(Aggregate2Finder& /*visitor*/)
{
	return false;
}

bool RecordKeyNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	if (dsqlRelation)
	{
		if (dsqlRelation->dsqlContext &&
			dsqlRelation->dsqlContext->ctx_scope_level == visitor.context->ctx_scope_level)
		{
			return true;
		}
	}

	return false;
}

bool RecordKeyNode::dsqlSubSelectFinder(SubSelectFinder& /*visitor*/)
{
	return false;
}

bool RecordKeyNode::dsqlFieldFinder(FieldFinder& /*visitor*/)
{
	return false;
}

ValueExprNode* RecordKeyNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	return PASS1_post_map(visitor.dsqlScratch, this, visitor.context, visitor.windowNode);
}

void RecordKeyNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = getAlias(false);
	setParameterInfo(parameter, dsqlRelation->dsqlContext);
}

void RecordKeyNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsql_ctx* context = dsqlRelation->dsqlContext;
	dsqlScratch->appendUChar(blrOp);
	GEN_stuff_context(dsqlScratch, context);
}

void RecordKeyNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	fb_assert(blrOp == blr_dbkey || blrOp == blr_record_version2);
	fb_assert(dsqlRelation);

	// Fix for bug 10072 check that the target is a relation
	dsql_rel* relation = dsqlRelation->dsqlContext->ctx_relation;

	if (relation)
	{
		USHORT dbKeyLength = (relation->rel_flags & REL_creating ? 8 : relation->rel_dbkey_length);

		if (blrOp == blr_dbkey)
		{
			desc->dsc_dtype = dtype_text;
			desc->dsc_length = dbKeyLength;
			desc->dsc_flags = DSC_nullable;
			desc->dsc_ttype() = ttype_binary;
		}
		else	// blr_record_version2
		{
			if (dbKeyLength == 8)
			{
				desc->makeInt64(0);
				desc->setNullable(true);
			}
			else
				raiseError(dsqlRelation->dsqlContext);
		}
	}
	else
		raiseError(dsqlRelation->dsqlContext);
}

bool RecordKeyNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	if (allowOnlyCurrentStream)
	{
		if (recStream != stream && !(csb->csb_rpt[recStream].csb_flags & csb_sub_stream))
			return false;
	}
	else
	{
		if (recStream == stream)
			return false;
	}

	return csb->csb_rpt[recStream].csb_flags & csb_active;
}

void RecordKeyNode::findDependentFromStreams(const OptimizerRetrieval* optRet, SortedStreamList* streamList)
{
	if (recStream != optRet->stream && (optRet->csb->csb_rpt[recStream].csb_flags & csb_active))
	{
		if (!streamList->exist(recStream))
			streamList->add(recStream);
	}
}

void RecordKeyNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	switch (blrOp)
	{
		case blr_dbkey:
			desc->dsc_dtype = dtype_dbkey;
			desc->dsc_length = type_lengths[dtype_dbkey];
			desc->dsc_scale = 0;
			desc->dsc_flags = 0;
			break;

		case blr_record_version:
			desc->dsc_dtype = dtype_text;
			desc->dsc_ttype() = ttype_binary;
			desc->dsc_length = sizeof(SINT64);
			desc->dsc_scale = 0;
			desc->dsc_flags = 0;
			break;

		case blr_record_version2:
			desc->makeInt64(0);
			break;
	}
}

ValueExprNode* RecordKeyNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	RecordKeyNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) RecordKeyNode(*tdbb->getDefaultPool(), blrOp);
	node->recStream = recStream;
	node->aggregate = aggregate;

	if (copier.remap)
	{
#ifdef CMP_DEBUG
		csb->dump("remap RecordKeyNode: %d -> %d\n", node->recStream, copier.remap[node->recStream]);
#endif
		node->recStream = copier.remap[node->recStream];
	}

	return node;
}

bool RecordKeyNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const RecordKeyNode* o = nodeAs<RecordKeyNode>(other);
	fb_assert(o);

	return blrOp == o->blrOp;
}

bool RecordKeyNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const RecordKeyNode* const otherNode = nodeAs<RecordKeyNode>(other);
	fb_assert(otherNode);

	return blrOp == otherNode->blrOp && (ignoreStreams || recStream == otherNode->recStream);
}

ValueExprNode* RecordKeyNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass1(tdbb, csb);

	markVariant(csb, recStream);

	if (!csb->csb_rpt[recStream].csb_map)
		return this;

	ValueExprNodeStack stack;
	expandViewNodes(tdbb, csb, recStream, stack, blrOp);

#ifdef CMP_DEBUG
	csb->dump("expand RecordKeyNode: %d\n", recStream);
#endif

	if (stack.hasData())
	{
		const size_t stackCount = stack.getCount();

		// If that is a DB_KEY of a view, it's possible (in case of
		// outer joins) that some sub-stream have a NULL DB_KEY.
		// In this case, we build a COALESCE(DB_KEY, _OCTETS x"0000000000000000"),
		// for the concatenation of sub DB_KEYs not result in NULL.
		if (blrOp == blr_dbkey && stackCount > 1)
		{
			ValueExprNodeStack stack2;

			for (ValueExprNodeStack::iterator i(stack); i.hasData(); ++i)
			{
#ifdef CMP_DEBUG
				csb->dump(" %d", nodeAs<RecordKeyNode>(i.object())->recStream);
#endif

				ValueIfNode* valueIfNode = FB_NEW_POOL(csb->csb_pool) ValueIfNode(csb->csb_pool);

				MissingBoolNode* missingNode = FB_NEW_POOL(csb->csb_pool) MissingBoolNode(csb->csb_pool);
				missingNode->arg = i.object();

				NotBoolNode* notNode = FB_NEW_POOL(csb->csb_pool) NotBoolNode(csb->csb_pool);
				notNode->arg = missingNode;

				// build an IF (RDB$DB_KEY IS NOT NULL)
				valueIfNode->condition = notNode;

				valueIfNode->trueValue = i.object();	// THEN

				LiteralNode* literal = FB_NEW_POOL(csb->csb_pool) LiteralNode(csb->csb_pool);
				literal->litDesc.dsc_dtype = dtype_text;
				literal->litDesc.dsc_ttype() = CS_BINARY;
				literal->litDesc.dsc_scale = 0;
				literal->litDesc.dsc_length = 8;
				literal->litDesc.dsc_address = reinterpret_cast<UCHAR*>(
					const_cast<char*>("\0\0\0\0\0\0\0\0"));	// safe const_cast

				valueIfNode->falseValue = literal;

				stack2.push(valueIfNode);
			}

			stack.clear();

			// stack2 is in reverse order, pushing everything in stack
			// will correct the order.
			for (ValueExprNodeStack::iterator i2(stack2); i2.hasData(); ++i2)
				stack.push(i2.object());
		}

		ValueExprNode* node = catenateNodes(tdbb, stack);

		if (blrOp == blr_dbkey && stackCount > 1)
		{
			// ASF: If the view is in null state (with outer joins) we need to transform
			// the view RDB$KEY to NULL. (CORE-1245)

			ValueIfNode* valueIfNode = FB_NEW_POOL(csb->csb_pool) ValueIfNode(csb->csb_pool);

			ComparativeBoolNode* eqlNode = FB_NEW_POOL(csb->csb_pool) ComparativeBoolNode(
				csb->csb_pool, blr_eql);

			// build an IF (RDB$DB_KEY = '')
			valueIfNode->condition = eqlNode;

			eqlNode->arg1 = NodeCopier::copy(tdbb, csb, node, NULL);

			LiteralNode* literal = FB_NEW_POOL(csb->csb_pool) LiteralNode(csb->csb_pool);
			literal->litDesc.dsc_dtype = dtype_text;
			literal->litDesc.dsc_ttype() = CS_BINARY;
			literal->litDesc.dsc_scale = 0;
			literal->litDesc.dsc_length = 0;
			literal->litDesc.dsc_address = reinterpret_cast<UCHAR*>(
				const_cast<char*>(""));	// safe const_cast

			eqlNode->arg2 = literal;

			// THEN: NULL
			valueIfNode->trueValue = FB_NEW_POOL(csb->csb_pool) NullNode(csb->csb_pool);

			// ELSE: RDB$DB_KEY
			valueIfNode->falseValue = node;

			node = valueIfNode;
		}

#ifdef CMP_DEBUG
		csb->dump("\n");
#endif

		return node;
	}

#ifdef CMP_DEBUG
	csb->dump("\n");
#endif

	// The user is asking for the dbkey/record version of an aggregate.
	// Humor him with a key filled with zeros.

	RecordKeyNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) RecordKeyNode(*tdbb->getDefaultPool(), blrOp);
	node->recStream = recStream;
	node->aggregate = true;

	return node;
}

ValueExprNode* RecordKeyNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* RecordKeyNode::execute(thread_db* /*tdbb*/, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	const record_param* rpb = &request->req_rpb[recStream];

	if (blrOp == blr_dbkey)
	{
		// Make up a dbkey for a record stream. A dbkey is expressed as an 8 byte character string.

		const jrd_rel* relation = rpb->rpb_relation;

		// If it doesn't point to a valid record, return NULL
		if (!rpb->rpb_number.isValid() || rpb->rpb_number.isBof() || !relation)
		{
			request->req_flags |= req_null;
			return NULL;
		}

		// Format dbkey as vector of relation id, record number

		// Initialize first 32 bits of DB_KEY
		impure->vlu_misc.vlu_dbkey[0] = 0;

		// Now, put relation ID into first 16 bits of DB_KEY
		// We do not assign it as SLONG because of big-endian machines.
		*(USHORT*) impure->vlu_misc.vlu_dbkey = relation->rel_id;

		// Encode 40-bit record number. Before that, increment the value
		// because users expect the numbering to start with one.
		RecordNumber temp(rpb->rpb_number.getValue() + 1);
		temp.bid_encode(reinterpret_cast<RecordNumber::Packed*>(impure->vlu_misc.vlu_dbkey));

		// Initialize descriptor

		impure->vlu_desc.dsc_address = (UCHAR*) impure->vlu_misc.vlu_dbkey;
		impure->vlu_desc.dsc_dtype = dtype_dbkey;
		impure->vlu_desc.dsc_length = type_lengths[dtype_dbkey];
		impure->vlu_desc.dsc_ttype() = ttype_binary;
	}
	else if (blrOp == blr_record_version)
	{
		// Make up a record version for a record stream. The tid of the record will be used.
		// This will be returned as a 4 byte character string.

		// If the current transaction has updated the record, the record version
		// coming in from DSQL will have the original transaction # (or current
		// transaction if the current transaction updated the record in a different
		// request).  In these cases, mark the request so that the boolean
		// to check equality of record version will be forced to evaluate to true.

		if (request->req_transaction->tra_number == rpb->rpb_transaction_nr)
			request->req_flags |= req_same_tx_upd;
		else
		{
			// If the transaction is a commit retain, check if the record was
			// last updated in one of its own prior transactions

			if (request->req_transaction->tra_commit_sub_trans)
			{
				if (request->req_transaction->tra_commit_sub_trans->test(rpb->rpb_transaction_nr))
					 request->req_flags |= req_same_tx_upd;
			}
		}

		// Initialize descriptor

		impure->vlu_misc.vlu_int64 = rpb->rpb_transaction_nr;
		impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_int64;
		impure->vlu_desc.dsc_dtype = dtype_text;
		impure->vlu_desc.dsc_length = sizeof(SINT64);
		impure->vlu_desc.dsc_ttype() = ttype_binary;
	}
	else if (blrOp == blr_record_version2)
	{
		const jrd_rel* relation = rpb->rpb_relation;

		// If it doesn't point to a valid record, return NULL.
		if (!rpb->rpb_number.isValid() || !relation || relation->isVirtual() || relation->rel_file)
		{
			request->req_flags |= req_null;
			return NULL;
		}

		impure->vlu_misc.vlu_int64 = rpb->rpb_transaction_nr;
		impure->vlu_desc.makeInt64(0, &impure->vlu_misc.vlu_int64);
	}

	return &impure->vlu_desc;
}

// Take a stack of nodes and turn them into a tree of concatenations.
ValueExprNode* RecordKeyNode::catenateNodes(thread_db* tdbb, ValueExprNodeStack& stack)
{
	SET_TDBB(tdbb);

	ValueExprNode* node1 = stack.pop();

	if (stack.isEmpty())
		return node1;

	ConcatenateNode* concatNode = FB_NEW_POOL(*tdbb->getDefaultPool()) ConcatenateNode(
		*tdbb->getDefaultPool());
	concatNode->arg1 = node1;
	concatNode->arg2 = catenateNodes(tdbb, stack);

	return concatNode;
}

void RecordKeyNode::raiseError(dsql_ctx* context) const
{
	if (blrOp != blr_record_version2)
	{
		status_exception::raise(
			Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
			Arg::Gds(isc_dsql_dbkey_from_non_table));
	}

	string name = context->getObjectName();
	const string& alias = context->ctx_internal_alias;

	if (alias.hasData() && name != alias)
	{
		if (name.hasData())
			name += " (alias " + alias + ")";
		else
			name = alias;
	}

	status_exception::raise(
		Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
		Arg::Gds(isc_dsql_record_version_table) << name);
}


//--------------------


static RegisterNode<ScalarNode> regScalarNode1(blr_index);

DmlNode* ScalarNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	ScalarNode* node = FB_NEW_POOL(pool) ScalarNode(pool);
	node->field = PAR_parse_value(tdbb, csb);
	node->subscripts = PAR_args(tdbb, csb);
	return node;
}

void ScalarNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* csb, dsc* desc)
{
	FieldNode* fieldNode = nodeAs<FieldNode>(field);
	fb_assert(fieldNode);

	jrd_rel* relation = csb->csb_rpt[fieldNode->fieldStream].csb_relation;
	const jrd_fld* field = MET_get_field(relation, fieldNode->fieldId);
	const ArrayField* array;

	if (!field || !(array = field->fld_array))
	{
		IBERROR(223);	// msg 223 argument of scalar operation must be an array
		return;
	}

	*desc = array->arr_desc.iad_rpt[0].iad_desc;

	if (array->arr_desc.iad_dimensions > MAX_ARRAY_DIMENSIONS)
		IBERROR(306); // Found array data type with more than 16 dimensions
}

ValueExprNode* ScalarNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	ScalarNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) ScalarNode(*tdbb->getDefaultPool());
	node->field = copier.copy(tdbb, field);
	node->subscripts = copier.copy(tdbb, subscripts);
	return node;
}

ValueExprNode* ScalarNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

// Evaluate a scalar item from an array.
dsc* ScalarNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	const dsc* desc = EVL_expr(tdbb, request, field);

	if (request->req_flags & req_null)
		return NULL;

	if (desc->dsc_dtype != dtype_array)
		IBERROR(261);	// msg 261 scalar operator used on field which is not an array

	if (subscripts->items.getCount() > MAX_ARRAY_DIMENSIONS)
		ERR_post(Arg::Gds(isc_array_max_dimensions) << Arg::Num(MAX_ARRAY_DIMENSIONS));

	SLONG numSubscripts[MAX_ARRAY_DIMENSIONS];
	int iter = 0;

	for (const auto& subscript : subscripts->items)
	{
		const dsc* temp = EVL_expr(tdbb, request, subscript);

		if (temp && !(request->req_flags & req_null))
			numSubscripts[iter++] = MOV_get_long(tdbb, temp, 0);
		else
			return NULL;
	}

	blb::scalar(tdbb, request->req_transaction, reinterpret_cast<bid*>(desc->dsc_address),
		subscripts->items.getCount(), numSubscripts, impure);

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<StmtExprNode> regStmtExprNode(blr_stmt_expr);

DmlNode* StmtExprNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	StmtExprNode* node = FB_NEW_POOL(pool) StmtExprNode(pool);

	node->stmt = PAR_parse_stmt(tdbb, csb);
	node->expr = PAR_parse_value(tdbb, csb);

	// Avoid blr_stmt_expr in a BLR expression header
	CompoundStmtNode* const stmt = nodeAs<CompoundStmtNode>(node->stmt);

	if (stmt)
	{
		if (stmt->statements.getCount() != 2 ||
			!nodeIs<DeclareVariableNode>(stmt->statements[0]) ||
			!nodeIs<AssignmentNode>(stmt->statements[1]))
		{
			return node->expr;
		}
	}
	else if (!nodeIs<AssignmentNode>(node->stmt))
		return node->expr;

	return node;
}

void StmtExprNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	fb_assert(false);
}

ValueExprNode* StmtExprNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	fb_assert(false);
	return NULL;
}

ValueExprNode* StmtExprNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	fb_assert(false);
	return NULL;
}

ValueExprNode* StmtExprNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	fb_assert(false);
	return NULL;
}

dsc* StmtExprNode::execute(thread_db* tdbb, jrd_req* request) const
{
	fb_assert(false);
	return NULL;
}


//--------------------


static RegisterNode<StrCaseNode> regStrCaseNodeLower(blr_lowcase);
static RegisterNode<StrCaseNode> regStrCaseNodeUpper(blr_upcase);

StrCaseNode::StrCaseNode(MemoryPool& pool, UCHAR aBlrOp, ValueExprNode* aArg)
	: TypedNode<ValueExprNode, ExprNode::TYPE_STR_CASE>(pool),
	  blrOp(aBlrOp),
	  arg(aArg)
{
}

DmlNode* StrCaseNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	StrCaseNode* node = FB_NEW_POOL(pool) StrCaseNode(pool, blrOp);
	node->arg = PAR_parse_value(tdbb, csb);
	return node;
}

string StrCaseNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, blrOp);
	NODE_PRINT(printer, arg);

	return "StrCaseNode";
}

ValueExprNode* StrCaseNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return FB_NEW_POOL(dsqlScratch->getPool()) StrCaseNode(dsqlScratch->getPool(), blrOp, doDsqlPass(dsqlScratch, arg));
}

void StrCaseNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = (blrOp == blr_lowcase ? "LOWER" : "UPPER");
}

bool StrCaseNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, arg, desc, forceVarChar);
}

void StrCaseNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blrOp);
	GEN_expr(dsqlScratch, arg);
}

void StrCaseNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc(dsqlScratch, desc, arg);

	if (desc->dsc_dtype > dtype_any_text && desc->dsc_dtype != dtype_blob)
	{
		desc->dsc_length = static_cast<int>(sizeof(USHORT)) + DSC_string_length(desc);
		desc->dsc_dtype = dtype_varying;
		desc->dsc_scale = 0;
		desc->dsc_ttype() = ttype_ascii;
		desc->dsc_flags = desc->dsc_flags & DSC_nullable;
	}
}

void StrCaseNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	arg->getDesc(tdbb, csb, desc);

	if (desc->dsc_dtype > dtype_any_text && desc->dsc_dtype != dtype_blob)
	{
		desc->dsc_length = DSC_convert_to_text_length(desc->dsc_dtype);
		desc->dsc_dtype = dtype_text;
		desc->dsc_ttype() = ttype_ascii;
		desc->dsc_scale = 0;
		desc->dsc_flags = 0;
	}
}

ValueExprNode* StrCaseNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	StrCaseNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) StrCaseNode(*tdbb->getDefaultPool(), blrOp);
	node->arg = copier.copy(tdbb, arg);
	return node;
}

bool StrCaseNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const StrCaseNode* o = nodeAs<StrCaseNode>(other);
	fb_assert(o);

	return blrOp == o->blrOp;
}

bool StrCaseNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const StrCaseNode* const otherNode = nodeAs<StrCaseNode>(other);
	fb_assert(otherNode);

	return blrOp == otherNode->blrOp;
}

ValueExprNode* StrCaseNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

// Low/up case a string.
dsc* StrCaseNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	const dsc* value = EVL_expr(tdbb, request, arg);

	if (request->req_flags & req_null)
		return NULL;

	TextType* textType = INTL_texttype_lookup(tdbb, value->getTextType());
	auto intlFunction = (blrOp == blr_lowcase ? &TextType::str_to_lower : &TextType::str_to_upper);

	if (value->isBlob())
	{
		EVL_make_value(tdbb, value, impure);

		if (value->dsc_sub_type != isc_blob_text)
			return &impure->vlu_desc;

		CharSet* charSet = textType->getCharSet();

		blb* blob = blb::open(tdbb, tdbb->getRequest()->req_transaction,
			reinterpret_cast<bid*>(value->dsc_address));

		HalfStaticArray<UCHAR, BUFFER_SMALL> buffer;

		if (charSet->isMultiByte())
			buffer.getBuffer(blob->blb_length);	// alloc space to put entire blob in memory

		blb* newBlob = blb::create(tdbb, tdbb->getRequest()->req_transaction,
			&impure->vlu_misc.vlu_bid);

		while (!(blob->blb_flags & BLB_eof))
		{
			SLONG len = blob->BLB_get_data(tdbb, buffer.begin(), buffer.getCapacity(), false);

			if (len)
			{
				len = (textType->*intlFunction)(len, buffer.begin(), len, buffer.begin());
				newBlob->BLB_put_data(tdbb, buffer.begin(), len);
			}
		}

		newBlob->BLB_close(tdbb);
		blob->BLB_close(tdbb);
	}
	else
	{
		UCHAR* ptr;
		VaryStr<32> temp;
		USHORT ttype;

		dsc desc;
		desc.dsc_length = MOV_get_string_ptr(tdbb, value, &ttype, &ptr, &temp, sizeof(temp));
		desc.dsc_dtype = dtype_text;
		desc.dsc_address = NULL;
		desc.setTextType(ttype);
		EVL_make_value(tdbb, &desc, impure);

		ULONG len = (textType->*intlFunction)(desc.dsc_length,
			ptr, desc.dsc_length, impure->vlu_desc.dsc_address);

		if (len == INTL_BAD_STR_LENGTH)
			status_exception::raise(Arg::Gds(isc_arith_except));

		impure->vlu_desc.dsc_length = (USHORT) len;
	}

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<StrLenNode> regStrLenNode(blr_strlen);

StrLenNode::StrLenNode(MemoryPool& pool, UCHAR aBlrSubOp, ValueExprNode* aArg)
	: TypedNode<ValueExprNode, ExprNode::TYPE_STR_LEN>(pool),
	  blrSubOp(aBlrSubOp),
	  arg(aArg)
{
}

DmlNode* StrLenNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	UCHAR blrSubOp = csb->csb_blr_reader.getByte();

	StrLenNode* node = FB_NEW_POOL(pool) StrLenNode(pool, blrSubOp);
	node->arg = PAR_parse_value(tdbb, csb);
	return node;
}

string StrLenNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, blrSubOp);
	NODE_PRINT(printer, arg);

	return "StrLenNode";
}

ValueExprNode* StrLenNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return FB_NEW_POOL(dsqlScratch->getPool()) StrLenNode(dsqlScratch->getPool(),
		blrSubOp, doDsqlPass(dsqlScratch, arg));
}

void StrLenNode::setParameterName(dsql_par* parameter) const
{
	const char* alias;

	switch (blrSubOp)
	{
		case blr_strlen_bit:
			alias = "BIT_LENGTH";
			break;

		case blr_strlen_char:
			alias = "CHAR_LENGTH";
			break;

		case blr_strlen_octet:
			alias = "OCTET_LENGTH";
			break;

		default:
			alias = "";
			fb_assert(false);
			break;
	}

	parameter->par_name = parameter->par_alias = alias;
}

bool StrLenNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return false;
}

void StrLenNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_strlen);
	dsqlScratch->appendUChar(blrSubOp);
	GEN_expr(dsqlScratch, arg);
}

void StrLenNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	dsc desc1;
	MAKE_desc(dsqlScratch, &desc1, arg);

	if (desc1.isBlob())
		desc->makeInt64(0);
	else
		desc->makeLong(0);

	desc->setNullable(desc1.isNullable());
}

void StrLenNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	dsc desc1;
	arg->getDesc(tdbb, csb, &desc1);

	if (desc1.isBlob())
		desc->makeInt64(0);
	else
		desc->makeLong(0);
}

ValueExprNode* StrLenNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	StrLenNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) StrLenNode(*tdbb->getDefaultPool(), blrSubOp);
	node->arg = copier.copy(tdbb, arg);
	return node;
}

bool StrLenNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const StrLenNode* o = nodeAs<StrLenNode>(other);
	fb_assert(o);

	return blrSubOp == o->blrSubOp;
}

bool StrLenNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const StrLenNode* const otherNode = nodeAs<StrLenNode>(other);
	fb_assert(otherNode);

	return blrSubOp == otherNode->blrSubOp;
}

ValueExprNode* StrLenNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

// Handles BIT_LENGTH(s), OCTET_LENGTH(s) and CHAR[ACTER]_LENGTH(s)
dsc* StrLenNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	const dsc* value = EVL_expr(tdbb, request, arg);

	impure->vlu_desc.makeInt64(0, &impure->vlu_misc.vlu_int64);

	if (!value || (request->req_flags & req_null))
		return NULL;

	FB_UINT64 length;

	if (value->isBlob())
	{
		blb* blob = blb::open(tdbb, tdbb->getRequest()->req_transaction,
			reinterpret_cast<bid*>(value->dsc_address));

		switch (blrSubOp)
		{
			case blr_strlen_bit:
				length = (FB_UINT64) blob->blb_length * 8;
				break;

			case blr_strlen_octet:
				length = blob->blb_length;
				break;

			case blr_strlen_char:
			{
				CharSet* charSet = INTL_charset_lookup(tdbb, value->dsc_blob_ttype());

				if (charSet->isMultiByte())
				{
					HalfStaticArray<UCHAR, BUFFER_LARGE> buffer;

					length = blob->BLB_get_data(tdbb, buffer.getBuffer(blob->blb_length),
						blob->blb_length, false);
					length = charSet->length(length, buffer.begin(), true);
				}
				else
					length = blob->blb_length / charSet->maxBytesPerChar();

				break;
			}

			default:
				fb_assert(false);
				length = 0;
		}

		if (length > MAX_SINT64)
		{
			ERR_post(Arg::Gds(isc_arith_except) <<
					 Arg::Gds(isc_numeric_out_of_range));
		}

		*(FB_UINT64*) impure->vlu_desc.dsc_address = length;

		blob->BLB_close(tdbb);

		return &impure->vlu_desc;
	}

	VaryStr<32> temp;
	USHORT ttype;
	UCHAR* p;

	length = MOV_get_string_ptr(tdbb, value, &ttype, &p, &temp, sizeof(temp));

	switch (blrSubOp)
	{
		case blr_strlen_bit:
			length *= 8;
			break;

		case blr_strlen_octet:
			break;

		case blr_strlen_char:
		{
			CharSet* charSet = INTL_charset_lookup(tdbb, ttype);
			length = charSet->length(length, p, true);
			break;
		}

		default:
			fb_assert(false);
			length = 0;
	}

	*(FB_UINT64*) impure->vlu_desc.dsc_address = length;

	return &impure->vlu_desc;
}


//--------------------


// Only blr_via is generated by DSQL.
static RegisterNode<SubQueryNode> regSubQueryNodeVia(blr_via);
static RegisterNode<SubQueryNode> regSubQueryNodeFrom(blr_from);
static RegisterNode<SubQueryNode> regSubQueryNodeAverage(blr_average);
static RegisterNode<SubQueryNode> regSubQueryNodeCount(blr_count);
static RegisterNode<SubQueryNode> regSubQueryNodeMaximum(blr_maximum);
static RegisterNode<SubQueryNode> regSubQueryNodeMinimum(blr_minimum);
static RegisterNode<SubQueryNode> regSubQueryNodeTotal(blr_total);

SubQueryNode::SubQueryNode(MemoryPool& pool, UCHAR aBlrOp, RecordSourceNode* aDsqlRse,
			ValueExprNode* aValue1, ValueExprNode* aValue2)
	: TypedNode<ValueExprNode, ExprNode::TYPE_SUBQUERY>(pool),
	  blrOp(aBlrOp),
	  ownSavepoint(true),
	  dsqlRse(aDsqlRse),
	  value1(aValue1),
	  value2(aValue2),
	  subQuery(NULL)
{
}

DmlNode* SubQueryNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	// We treat blr_from as blr_via after parse.
	SubQueryNode* node = FB_NEW_POOL(pool) SubQueryNode(pool, (blrOp == blr_from ? blr_via : blrOp));

	node->rse = PAR_rse(tdbb, csb);

	if (blrOp != blr_count)
		node->value1 = PAR_parse_value(tdbb, csb);

	if (blrOp == blr_via)
	{
		node->value2 = PAR_parse_value(tdbb, csb);

		if (csb->csb_currentForNode && csb->csb_currentForNode->parBlrBeginCnt <= 1)
			node->ownSavepoint = false;

		if (csb->csb_currentDMLNode)
			node->ownSavepoint = false;
	}

	return node;
}

void SubQueryNode::getChildren(NodeRefsHolder& holder, bool dsql) const
{
	ValueExprNode::getChildren(holder, dsql);

	if (dsql)
		holder.add(dsqlRse);
	else
		holder.add(rse);

	holder.add(value1);
	holder.add(value2);
}

string SubQueryNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, blrOp);
	NODE_PRINT(printer, ownSavepoint);
	NODE_PRINT(printer, dsqlRse);
	NODE_PRINT(printer, rse);
	NODE_PRINT(printer, value1);
	NODE_PRINT(printer, value2);
	NODE_PRINT(printer, subQuery);

	return "SubQueryNode";
}

ValueExprNode* SubQueryNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (dsqlScratch->flags & DsqlCompilerScratch::FLAG_VIEW_WITH_CHECK)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-607) <<
				  Arg::Gds(isc_subquery_err));
	}

	const DsqlContextStack::iterator base(*dsqlScratch->context);

	RseNode* rse = PASS1_rse(dsqlScratch, nodeAs<SelectExprNode>(dsqlRse), false);

	SubQueryNode* node = FB_NEW_POOL(dsqlScratch->getPool()) SubQueryNode(dsqlScratch->getPool(), blrOp, rse,
		rse->dsqlSelectList->items[0], FB_NEW_POOL(dsqlScratch->getPool()) NullNode(dsqlScratch->getPool()));

	// Finish off by cleaning up contexts.
	dsqlScratch->context->clear(base);

	return node;
}

void SubQueryNode::setParameterName(dsql_par* parameter) const
{
	MAKE_parameter_names(parameter, value1);
}

void SubQueryNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blrOp);
	GEN_expr(dsqlScratch, dsqlRse);
	GEN_expr(dsqlScratch, value1);
	GEN_expr(dsqlScratch, value2);
}

void SubQueryNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc(dsqlScratch, desc, value1);

	// Set the descriptor flag as nullable. The select expression may or may not return this row
	// based on the WHERE clause. Setting this flag warns the client to expect null values.
	// (bug 10379)
	desc->dsc_flags |= DSC_nullable;
}

bool SubQueryNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	return !visitor.ignoreSubSelects && visitor.visit(dsqlRse);
}

bool SubQueryNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	return visitor.visit(dsqlRse);	// Pass only the rse.
}

bool SubQueryNode::dsqlSubSelectFinder(SubSelectFinder& /*visitor*/)
{
	return true;
}

bool SubQueryNode::dsqlFieldFinder(FieldFinder& visitor)
{
	return visitor.visit(dsqlRse);	// Pass only the rse.
}

ValueExprNode* SubQueryNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	doDsqlFieldRemapper(visitor, dsqlRse);
	value1 = nodeAs<RseNode>(dsqlRse)->dsqlSelectList->items[0];
	return this;
}

void SubQueryNode::collectStreams(CompilerScratch* csb, SortedStreamList& streamList) const
{
	if (rse)
		rse->collectStreams(csb, streamList);

	if (value1)
		value1->collectStreams(csb, streamList);
}

bool SubQueryNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	if (value2 && !value2->computable(csb, stream, allowOnlyCurrentStream))
		return false;

	return rse->computable(csb, stream, allowOnlyCurrentStream, value1);
}

void SubQueryNode::findDependentFromStreams(const OptimizerRetrieval* optRet,
	SortedStreamList* streamList)
{
	if (value2)
		value2->findDependentFromStreams(optRet, streamList);

	rse->findDependentFromStreams(optRet, streamList);

	// Check value expression, if any.
	if (value1)
		value1->findDependentFromStreams(optRet, streamList);
}

void SubQueryNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	if (blrOp == blr_count)
		desc->makeLong(0);
	else if (value1)
		value1->getDesc(tdbb, csb, desc);

	if (blrOp == blr_average)
	{
		if (DTYPE_IS_DECFLOAT(desc->dsc_dtype))
		{
			desc->dsc_dtype = dtype_dec128;
			desc->dsc_length = sizeof(Decimal128);
			desc->dsc_scale = 0;
			desc->dsc_sub_type = 0;
			desc->dsc_flags = 0;
			nodFlags |= FLAG_DECFLOAT;
			return;
		}

		if (!(DTYPE_IS_NUMERIC(desc->dsc_dtype) || DTYPE_IS_TEXT(desc->dsc_dtype)))
		{
			if (desc->dsc_dtype != dtype_unknown)
				return;
		}

		desc->dsc_dtype = DEFAULT_DOUBLE;
		desc->dsc_length = sizeof(double);
		desc->dsc_scale = 0;
		desc->dsc_sub_type = 0;
		desc->dsc_flags = 0;
	}
	else if (blrOp == blr_total)
	{
		const USHORT dtype = desc->dsc_dtype;

		switch (dtype)
		{
			case dtype_short:
				desc->dsc_dtype = dtype_long;
				desc->dsc_length = sizeof(SLONG);
				nodScale = desc->dsc_scale;
				desc->dsc_sub_type = 0;
				desc->dsc_flags = 0;
				return;

			case dtype_unknown:
				desc->dsc_dtype = dtype_unknown;
				desc->dsc_length = 0;
				nodScale = 0;
				desc->dsc_sub_type = 0;
				desc->dsc_flags = 0;
				return;

			case dtype_long:
			case dtype_int64:
			case dtype_real:
			case dtype_double:
			case dtype_text:
			case dtype_cstring:
			case dtype_varying:
				desc->dsc_dtype = DEFAULT_DOUBLE;
				desc->dsc_length = sizeof(double);
				desc->dsc_scale = 0;
				desc->dsc_sub_type = 0;
				desc->dsc_flags = 0;
				nodFlags |= FLAG_DOUBLE;
				return;

			case dtype_dec64:
			case dtype_dec128:
			case dtype_dec_fixed:
				desc->dsc_dtype = dtype_dec128;
				desc->dsc_length = sizeof(Decimal128);
				desc->dsc_scale = 0;
				desc->dsc_sub_type = 0;
				desc->dsc_flags = 0;
				nodFlags |= FLAG_DECFLOAT;
				return;

			case dtype_sql_time:
			case dtype_sql_date:
			case dtype_timestamp:
			case dtype_quad:
			case dtype_blob:
			case dtype_array:
			case dtype_dbkey:
				// break to error reporting code
				break;

			default:
				fb_assert(false);
		}

		if (dtype == dtype_quad)
			IBERROR(224);	// msg 224 quad word arithmetic not supported

		ERR_post(Arg::Gds(isc_datype_notsup));	// data type not supported for arithmetic
	}
}

ValueExprNode* SubQueryNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	SubQueryNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) SubQueryNode(*tdbb->getDefaultPool(), blrOp);
	node->nodScale = nodScale;
	node->ownSavepoint = this->ownSavepoint;
	node->rse = copier.copy(tdbb, rse);
	node->value1 = copier.copy(tdbb, value1);
	node->value2 = copier.copy(tdbb, value2);

	return node;
}

bool SubQueryNode::sameAs(CompilerScratch* /*csb*/, const ExprNode* /*other*/, bool /*ignoreStreams*/) const
{
	return false;
}

ValueExprNode* SubQueryNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	rse->ignoreDbKey(tdbb, csb);
	doPass1(tdbb, csb, rse.getAddress());

	csb->csb_current_nodes.push(rse.getObject());

	doPass1(tdbb, csb, value1.getAddress());
	doPass1(tdbb, csb, value2.getAddress());

	csb->csb_current_nodes.pop();

	return this;
}

ValueExprNode* SubQueryNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	if (!rse)
		ERR_post(Arg::Gds(isc_wish_list));

	if (!(rse->flags & RseNode::FLAG_VARIANT))
	{
		nodFlags |= FLAG_INVARIANT;
		csb->csb_invariants.push(&impureOffset);
	}

	rse->pass2Rse(tdbb, csb);

	ValueExprNode::pass2(tdbb, csb);

	impureOffset = CMP_impure(csb, sizeof(impure_value_ex));

	{
		dsc desc;
		getDesc(tdbb, csb, &desc);
	}
	if (blrOp == blr_average && !(nodFlags & FLAG_DECFLOAT))
		nodFlags |= FLAG_DOUBLE;

	// Bind values of invariant nodes to top-level RSE (if present).
	if ((nodFlags & FLAG_INVARIANT) && csb->csb_current_nodes.hasData())
	{
		RseNode* topRseNode = nodeAs<RseNode>(csb->csb_current_nodes[0]);
		fb_assert(topRseNode);

		if (!topRseNode->rse_invariants)
		{
			topRseNode->rse_invariants =
				FB_NEW_POOL(*tdbb->getDefaultPool()) VarInvariantArray(*tdbb->getDefaultPool());
		}

		topRseNode->rse_invariants->add(impureOffset);
	}

	// Finish up processing of record selection expressions.

	RecordSource* const rsb = CMP_post_rse(tdbb, csb, rse);
	csb->csb_fors.add(rsb);

	subQuery = FB_NEW_POOL(*tdbb->getDefaultPool()) SubQuery(rsb, rse->rse_invariants);

	return this;
}

// Evaluate a subquery expression.
dsc* SubQueryNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	dsc* desc = &impure->vlu_desc;
	USHORT* invariant_flags = NULL;

	if (nodFlags & FLAG_INVARIANT)
	{
		invariant_flags = &impure->vlu_flags;

		if (*invariant_flags & VLU_computed)
		{
			// An invariant node has already been computed.

			if (*invariant_flags & VLU_null)
				request->req_flags |= req_null;
			else
				request->req_flags &= ~req_null;

			return (request->req_flags & req_null) ? NULL : desc;
		}
	}

	impure->vlu_misc.vlu_long = 0;
	impure->vlu_desc.dsc_dtype = dtype_long;
	impure->vlu_desc.dsc_length = sizeof(SLONG);
	impure->vlu_desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_long;

	ULONG flag = req_null;

	try
	{
		StableCursorSavePoint savePoint(tdbb, request->req_transaction,
			blrOp == blr_via && ownSavepoint);

		subQuery->open(tdbb);

		SLONG count = 0;
		double d;

		// Handle each variety separately
		switch (blrOp)
		{
			case blr_count:
				flag = 0;
				while (subQuery->fetch(tdbb))
					++impure->vlu_misc.vlu_long;
				break;

			case blr_minimum:
			case blr_maximum:
				while (subQuery->fetch(tdbb))
				{
					dsc* value = EVL_expr(tdbb, request, value1);
					if (request->req_flags & req_null)
						continue;

					int result;

					if (flag || ((result = MOV_compare(tdbb, value, desc)) < 0 && blrOp == blr_minimum) ||
						(blrOp != blr_minimum && result > 0))
					{
						flag = 0;
						EVL_make_value(tdbb, value, impure);
					}
				}
				break;

			case blr_average:	// total or average with dialect-1 semantics
			case blr_total:
				while (subQuery->fetch(tdbb))
				{
					desc = EVL_expr(tdbb, request, value1);
					if (request->req_flags & req_null)
						continue;

					// Note: if the field being SUMed or AVERAGEd is short or long,
					// impure will stay long, and the first add() will
					// set the correct scale; if it is approximate numeric,
					// the first add() will convert impure to double.
					ArithmeticNode::add(tdbb, desc, impure, this, blr_add);

					++count;
				}

				desc = &impure->vlu_desc;

				if (blrOp == blr_total)
				{
					flag = 0;
					break;
				}

				if (!count)
					break;

				d = MOV_get_double(tdbb, &impure->vlu_desc);
				impure->vlu_misc.vlu_double = d / count;
				impure->vlu_desc.dsc_dtype = DEFAULT_DOUBLE;
				impure->vlu_desc.dsc_length = sizeof(double);
				impure->vlu_desc.dsc_scale = 0;
				flag = 0;
				break;

			case blr_via:
				if (subQuery->fetch(tdbb))
					desc = EVL_expr(tdbb, request, value1);
				else
				{
					if (value2)
						desc = EVL_expr(tdbb, request, value2);
					else
						ERR_post(Arg::Gds(isc_from_no_match));
				}

				flag = request->req_flags;
				break;

			default:
				SOFT_BUGCHECK(233);	// msg 233 eval_statistical: invalid operation
		}
	}
	catch (const Exception&)
	{
		// Close stream, ignoring any error during it to keep the original error.
		try
		{
			subQuery->close(tdbb);
			request->req_flags &= ~req_null;
			request->req_flags |= flag;
		}
		catch (const Exception&)
		{
		}

		throw;
	}

	// Close stream and return value.

	subQuery->close(tdbb);
	request->req_flags &= ~req_null;
	request->req_flags |= flag;

	// If this is an invariant node, save the return value. If the descriptor does not point to the
	// impure area for this node then point this node's descriptor to the correct place;
	// Copy the whole structure to be absolutely sure.

	if (nodFlags & FLAG_INVARIANT)
	{
		*invariant_flags |= VLU_computed;

		if (request->req_flags & req_null)
			*invariant_flags |= VLU_null;
		if (desc && (desc != &impure->vlu_desc))
			impure->vlu_desc = *desc;
	}

	return (request->req_flags & req_null) ? NULL : desc;
}


//--------------------


static RegisterNode<SubstringNode> regSubstringNode(blr_substring);

SubstringNode::SubstringNode(MemoryPool& pool, ValueExprNode* aExpr, ValueExprNode* aStart,
			ValueExprNode* aLength)
	: TypedNode<ValueExprNode, ExprNode::TYPE_SUBSTRING>(pool),
	  expr(aExpr),
	  start(aStart),
	  length(aLength)
{
}

DmlNode* SubstringNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	const UCHAR /*blrOp*/)
{
	SubstringNode* node = FB_NEW_POOL(pool) SubstringNode(pool);
	node->expr = PAR_parse_value(tdbb, csb);
	node->start = PAR_parse_value(tdbb, csb);
	node->length = PAR_parse_value(tdbb, csb);
	return node;
}

string SubstringNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, expr);
	NODE_PRINT(printer, start);
	NODE_PRINT(printer, length);

	return "SubstringNode";
}

ValueExprNode* SubstringNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	SubstringNode* node = FB_NEW_POOL(dsqlScratch->getPool()) SubstringNode(dsqlScratch->getPool(),
		doDsqlPass(dsqlScratch, expr),
		doDsqlPass(dsqlScratch, start),
		doDsqlPass(dsqlScratch, length));

	return node;
}

void SubstringNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "SUBSTRING";
}

bool SubstringNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, expr, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, start, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, length, desc, forceVarChar);
}

void SubstringNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_substring);

	GEN_expr(dsqlScratch, expr);
	GEN_expr(dsqlScratch, start);

	if (length)
		GEN_expr(dsqlScratch, length);
	else
	{
		dsqlScratch->appendUChar(blr_literal);
		dsqlScratch->appendUChar(blr_long);
		dsqlScratch->appendUChar(0);
		dsqlScratch->appendUShort(LONG_POS_MAX & 0xFFFF);
		dsqlScratch->appendUShort(LONG_POS_MAX >> 16);
	}
}

void SubstringNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	dsc desc1, desc2, desc3;

	MAKE_desc(dsqlScratch, &desc1, expr);
	MAKE_desc(dsqlScratch, &desc2, start);

	if (length)
	{
		MAKE_desc(dsqlScratch, &desc3, length);

		if (!nodeIs<LiteralNode>(length))
			desc3.dsc_address = NULL;
	}

	DSqlDataTypeUtil(dsqlScratch).makeSubstr(desc, &desc1, &desc2, &desc3);
}

void SubstringNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	DSC desc0, desc1, desc2, desc3;

	expr->getDesc(tdbb, csb, &desc0);

	ValueExprNode* offsetNode = start;
	ValueExprNode* decrementNode = NULL;
	ArithmeticNode* arithmeticNode = nodeAs<ArithmeticNode>(offsetNode);

	// ASF: This code is very strange. The DSQL node is created as dialect 1, but only the dialect
	// 3 is verified here. Also, this task seems unnecessary here, as it must be done during
	// execution anyway.

	if (arithmeticNode && arithmeticNode->blrOp == blr_subtract && !arithmeticNode->dialect1)
	{
		// This node is created by the DSQL layer, but the system BLR code bypasses it and uses
		// zero-based string offsets instead.
		decrementNode = arithmeticNode->arg2;
		decrementNode->getDesc(tdbb, csb, &desc3);
		offsetNode = arithmeticNode->arg1;
	}

	offsetNode->getDesc(tdbb, csb, &desc1);
	length->getDesc(tdbb, csb, &desc2);

	DataTypeUtil(tdbb).makeSubstr(desc, &desc0, &desc1, &desc2);

	if (desc1.dsc_flags & DSC_null || desc2.dsc_flags & DSC_null)
		desc->dsc_flags |= DSC_null;
	else
	{
		if (nodeIs<LiteralNode>(length) && desc2.dsc_dtype == dtype_long)
		{
			const SLONG len = MOV_get_long(tdbb, &desc2, 0);

			if (len < 0)
				ERR_post(Arg::Gds(isc_bad_substring_length) << Arg::Num(len));
		}
	}
}

ValueExprNode* SubstringNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	SubstringNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) SubstringNode(
		*tdbb->getDefaultPool());
	node->expr = copier.copy(tdbb, expr);
	node->start = copier.copy(tdbb, start);
	node->length = copier.copy(tdbb, length);
	return node;
}

ValueExprNode* SubstringNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* SubstringNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* impure = request->getImpure<impure_value>(impureOffset);

	// Run all expression arguments.

	const dsc* exprDesc = EVL_expr(tdbb, request, expr);
	exprDesc = (request->req_flags & req_null) ? NULL : exprDesc;

	const dsc* startDesc = EVL_expr(tdbb, request, start);
	startDesc = (request->req_flags & req_null) ? NULL : startDesc;

	const dsc* lengthDesc = EVL_expr(tdbb, request, length);
	lengthDesc = (request->req_flags & req_null) ? NULL : lengthDesc;

	if (exprDesc && startDesc && lengthDesc)
		return perform(tdbb, impure, exprDesc, startDesc, lengthDesc);

	// If any of them is NULL, return NULL.
	return NULL;
}

dsc* SubstringNode::perform(thread_db* tdbb, impure_value* impure, const dsc* valueDsc,
	const dsc* startDsc, const dsc* lengthDsc)
{
	SINT64 sStart = MOV_get_long(tdbb, startDsc, 0);
	SINT64 sLength = MOV_get_long(tdbb, lengthDsc, 0);

	if (sLength < 0)
		status_exception::raise(Arg::Gds(isc_bad_substring_length) << Arg::Num(sLength));

	if (sStart < 0)
	{
		sLength = MAX(sLength + sStart, 0);
		sStart = 0;
	}

	FB_UINT64 start = FB_UINT64(sStart);
	FB_UINT64 length = FB_UINT64(sLength);

	dsc desc;
	DataTypeUtil(tdbb).makeSubstr(&desc, valueDsc, startDsc, lengthDsc);

	if (desc.isText() && length > MAX_STR_SIZE)
		length = MAX_STR_SIZE;

	ULONG dataLen;

	if (valueDsc->isBlob())
	{
		// Source string is a blob, things get interesting.

		fb_assert(desc.dsc_dtype == dtype_blob);

		desc.dsc_address = (UCHAR*) &impure->vlu_misc.vlu_bid;

		blb* newBlob = blb::create(tdbb, tdbb->getRequest()->req_transaction, &impure->vlu_misc.vlu_bid);

		blb* blob = blb::open(tdbb, tdbb->getRequest()->req_transaction,
			reinterpret_cast<bid*>(valueDsc->dsc_address));

		HalfStaticArray<UCHAR, BUFFER_LARGE> buffer;
		CharSet* charSet = INTL_charset_lookup(tdbb, valueDsc->getCharSet());

		const FB_UINT64 byte_offset = start * charSet->maxBytesPerChar();
		const FB_UINT64 byte_length = length * charSet->maxBytesPerChar();

		if (charSet->isMultiByte())
		{
			buffer.getBuffer(MIN(blob->blb_length, byte_offset + byte_length));
			dataLen = blob->BLB_get_data(tdbb, buffer.begin(), buffer.getCount(), false);

			HalfStaticArray<UCHAR, BUFFER_LARGE> buffer2;
			buffer2.getBuffer(dataLen);

			dataLen = charSet->substring(dataLen, buffer.begin(),
				buffer2.getCapacity(), buffer2.begin(), start, length);
			newBlob->BLB_put_data(tdbb, buffer2.begin(), dataLen);
		}
		else if (byte_offset < blob->blb_length)
		{
			start = byte_offset;
			length = MIN(blob->blb_length, byte_length);

			while (!(blob->blb_flags & BLB_eof) && start)
			{
				// Both cases are the same for now. Let's see if we can optimize in the future.
				ULONG l1 = blob->BLB_get_data(tdbb, buffer.begin(),
					MIN(buffer.getCapacity(), start), false);
				start -= l1;
			}

			while (!(blob->blb_flags & BLB_eof) && length)
			{
				dataLen = blob->BLB_get_data(tdbb, buffer.begin(),
					MIN(length, buffer.getCapacity()), false);
				length -= dataLen;

				newBlob->BLB_put_data(tdbb, buffer.begin(), dataLen);
			}
		}

		blob->BLB_close(tdbb);
		newBlob->BLB_close(tdbb);

		EVL_make_value(tdbb, &desc, impure);
	}
	else
	{
		fb_assert(desc.isText());

		desc.dsc_dtype = dtype_text;

		// CVC: I didn't bother to define a larger buffer because:
		//		- Native types when converted to string don't reach 31 bytes plus terminator.
		//		- String types do not need and do not use the buffer ("temp") to be pulled.
		//		- The types that can cause an error() issued inside the low level MOV/CVT
		//		routines because the "temp" is not enough are blob and array but at this time
		//		they aren't accepted, so they will cause error() to be called anyway.
		VaryStr<32> temp;
		USHORT ttype;
		desc.dsc_length = MOV_get_string_ptr(tdbb, valueDsc, &ttype, &desc.dsc_address,
			&temp, sizeof(temp));
		desc.setTextType(ttype);

		// CVC: Why bother? If the start is greater or equal than the length in bytes,
		// it's impossible that the start be less than the length in an international charset.
		if (start >= desc.dsc_length || !length)
		{
			desc.dsc_length = 0;
			EVL_make_value(tdbb, &desc, impure);
		}
		// CVC: God save the king if the engine doesn't protect itself against buffer overruns,
		//		because intl.h defines UNICODE as the type of most system relations' string fields.
		//		Also, the field charset can come as 127 (dynamic) when it comes from system triggers,
		//		but it's resolved by INTL_obj_lookup() to UNICODE_FSS in the cases I observed. Here I cannot
		//		distinguish between user calls and system calls. Unlike the original ASCII substring(),
		//		this one will get correctly the amount of UNICODE characters requested.
		else if (ttype == ttype_ascii || ttype == ttype_none || ttype == ttype_binary)
		{
			/* Redundant.
			if (start >= desc.dsc_length)
				desc.dsc_length = 0;
			else */
			desc.dsc_address += start;
			desc.dsc_length -= start;
			if (length < desc.dsc_length)
				desc.dsc_length = length;
			EVL_make_value(tdbb, &desc, impure);
		}
		else
		{
			// CVC: ATTENTION:
			// I couldn't find an appropriate message for this failure among current registered
			// messages, so I will return empty.
			// Finally I decided to use arithmetic exception or numeric overflow.
			const UCHAR* p = desc.dsc_address;
			const USHORT pcount = desc.dsc_length;

			CharSet* charSet = INTL_charset_lookup(tdbb, desc.getCharSet());

			desc.dsc_address = NULL;
			const ULONG totLen = MIN(MAX_STR_SIZE, length * charSet->maxBytesPerChar());
			desc.dsc_length = totLen;
			EVL_make_value(tdbb, &desc, impure);

			dataLen = charSet->substring(pcount, p, totLen,
				impure->vlu_desc.dsc_address, start, length);
			impure->vlu_desc.dsc_length = static_cast<USHORT>(dataLen);
		}
	}

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<SubstringSimilarNode> regSubstringSimilarNode(blr_substring_similar);

SubstringSimilarNode::SubstringSimilarNode(MemoryPool& pool, ValueExprNode* aExpr,
			ValueExprNode* aPattern, ValueExprNode* aEscape)
	: TypedNode<ValueExprNode, ExprNode::TYPE_SUBSTRING_SIMILAR>(pool),
	  expr(aExpr),
	  pattern(aPattern),
	  escape(aEscape)
{
}

DmlNode* SubstringSimilarNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	const UCHAR /*blrOp*/)
{
	SubstringSimilarNode* node = FB_NEW_POOL(pool) SubstringSimilarNode(pool);
	node->expr = PAR_parse_value(tdbb, csb);
	node->pattern = PAR_parse_value(tdbb, csb);
	node->escape = PAR_parse_value(tdbb, csb);
	return node;
}

string SubstringSimilarNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, expr);
	NODE_PRINT(printer, pattern);
	NODE_PRINT(printer, escape);

	return "SubstringSimilarNode";
}

void SubstringSimilarNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "SUBSTRING";
}

bool SubstringSimilarNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, expr, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, pattern, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, escape, desc, forceVarChar);
}

void SubstringSimilarNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_substring_similar);
	GEN_expr(dsqlScratch, expr);
	GEN_expr(dsqlScratch, pattern);
	GEN_expr(dsqlScratch, escape);
}

void SubstringSimilarNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	MAKE_desc(dsqlScratch, desc, expr);
	desc->setNullable(true);
}

void SubstringSimilarNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	expr->getDesc(tdbb, csb, desc);

	dsc tempDesc;
	pattern->getDesc(tdbb, csb, &tempDesc);
	escape->getDesc(tdbb, csb, &tempDesc);
}

ValueExprNode* SubstringSimilarNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	SubstringSimilarNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) SubstringSimilarNode(
		*tdbb->getDefaultPool());
	node->expr = copier.copy(tdbb, expr);
	node->pattern = copier.copy(tdbb, pattern);
	node->escape = copier.copy(tdbb, escape);
	return node;
}

ValueExprNode* SubstringSimilarNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, expr.getAddress());

	// We need to take care of invariantness expressions to be able to pre-compile the pattern.
	nodFlags |= FLAG_INVARIANT;
	csb->csb_current_nodes.push(this);

	doPass1(tdbb, csb, pattern.getAddress());
	doPass1(tdbb, csb, escape.getAddress());

	csb->csb_current_nodes.pop();

	// If there is no top-level RSE present and patterns are not constant, unmark node as invariant
	// because it may be dependent on data or variables.
	if ((nodFlags & FLAG_INVARIANT) && (!nodeIs<LiteralNode>(pattern) || !nodeIs<LiteralNode>(escape)))
	{
		for (const auto& ctxNode : csb->csb_current_nodes)
		{
			if (nodeAs<RseNode>(ctxNode))
				return this;
		}

		nodFlags &= ~FLAG_INVARIANT;
	}

	return this;
}

ValueExprNode* SubstringSimilarNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	if (nodFlags & FLAG_INVARIANT)
		csb->csb_invariants.push(&impureOffset);

	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* SubstringSimilarNode::execute(thread_db* tdbb, jrd_req* request) const
{
	// Run all expression arguments.

	const dsc* exprDesc = EVL_expr(tdbb, request, expr);
	exprDesc = (request->req_flags & req_null) ? NULL : exprDesc;

	const dsc* patternDesc = EVL_expr(tdbb, request, pattern);
	patternDesc = (request->req_flags & req_null) ? NULL : patternDesc;

	const dsc* escapeDesc = EVL_expr(tdbb, request, escape);
	escapeDesc = (request->req_flags & req_null) ? NULL : escapeDesc;

	// If any of them is NULL, return NULL.
	if (!exprDesc || !patternDesc || !escapeDesc)
		return NULL;

	USHORT textType = exprDesc->getTextType();
	Collation* collation = INTL_texttype_lookup(tdbb, textType);
	CharSet* charSet = collation->getCharSet();

	MoveBuffer exprBuffer;
	UCHAR* exprStr;
	int exprLen = MOV_make_string2(tdbb, exprDesc, textType, &exprStr, exprBuffer);

	MoveBuffer patternBuffer;
	UCHAR* patternStr;
	int patternLen = MOV_make_string2(tdbb, patternDesc, textType, &patternStr, patternBuffer);

	MoveBuffer escapeBuffer;
	UCHAR* escapeStr;
	int escapeLen = MOV_make_string2(tdbb, escapeDesc, textType, &escapeStr, escapeBuffer);

	// Verify the correctness of the escape character.
	if (escapeLen == 0 || charSet->length(escapeLen, escapeStr, true) != 1)
		ERR_post(Arg::Gds(isc_escape_invalid));

	impure_value* impure = request->getImpure<impure_value>(impureOffset);

	AutoPtr<BaseSubstringSimilarMatcher> autoEvaluator;	// deallocate non-invariant evaluator
	BaseSubstringSimilarMatcher* evaluator;

	if (nodFlags & FLAG_INVARIANT)
	{
		if (!(impure->vlu_flags & VLU_computed))
		{
			delete impure->vlu_misc.vlu_invariant;

			impure->vlu_misc.vlu_invariant = evaluator = collation->createSubstringSimilarMatcher(
				*tdbb->getDefaultPool(), patternStr, patternLen, escapeStr, escapeLen);

			impure->vlu_flags |= VLU_computed;
		}
		else
		{
			evaluator = static_cast<BaseSubstringSimilarMatcher*>(impure->vlu_misc.vlu_invariant);
			evaluator->reset();
		}
	}
	else
	{
		autoEvaluator = evaluator = collation->createSubstringSimilarMatcher(*tdbb->getDefaultPool(),
			patternStr, patternLen, escapeStr, escapeLen);
	}

	evaluator->process(exprStr, exprLen);

	if (evaluator->result())
	{
		// Get the byte bounds of the matched substring.
		unsigned start = 0;
		unsigned length = 0;
		evaluator->getResultInfo(&start, &length);

		dsc desc;
		desc.makeText((USHORT) exprLen, textType);
		EVL_make_value(tdbb, &desc, impure);

		// And return it.
		memcpy(impure->vlu_desc.dsc_address, exprStr + start, length);
		impure->vlu_desc.dsc_length = length;

		return &impure->vlu_desc;
	}
	else
		return NULL;	// No match. Return NULL.
}

ValueExprNode* SubstringSimilarNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	SubstringSimilarNode* node = FB_NEW_POOL(dsqlScratch->getPool()) SubstringSimilarNode(dsqlScratch->getPool(),
		doDsqlPass(dsqlScratch, expr),
		doDsqlPass(dsqlScratch, pattern),
		doDsqlPass(dsqlScratch, escape));

	// ? SIMILAR FIELD case.
	PASS1_set_parameter_type(dsqlScratch, node->expr, node->pattern, true);

	// FIELD SIMILAR ? case.
	PASS1_set_parameter_type(dsqlScratch, node->pattern, node->expr, true);

	// X SIMILAR Y ESCAPE ? case.
	PASS1_set_parameter_type(dsqlScratch, node->escape, node->pattern, true);

	return node;
}


//--------------------


static RegisterNode<SysFuncCallNode> regSysFuncCallNode(blr_sys_function);

SysFuncCallNode::SysFuncCallNode(MemoryPool& pool, const MetaName& aName, ValueListNode* aArgs)
	: TypedNode<ValueExprNode, ExprNode::TYPE_SYSFUNC_CALL>(pool),
	  name(pool, aName),
	  dsqlSpecialSyntax(false),
	  args(aArgs),
	  function(NULL)
{
}

DmlNode* SysFuncCallNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	const UCHAR /*blrOp*/)
{
	MetaName name;
	csb->csb_blr_reader.getMetaName(name);

	const USHORT count = name.length();

	SysFuncCallNode* node = FB_NEW_POOL(pool) SysFuncCallNode(pool, name);
	node->function = SysFunction::lookup(name);

	if (!node->function)
	{
		csb->csb_blr_reader.seekBackward(count);
		PAR_error(csb, Arg::Gds(isc_funnotdef) << Arg::Str(name));
	}

	node->args = PAR_args(tdbb, csb);

	return node;
}

string SysFuncCallNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, name);
	NODE_PRINT(printer, dsqlSpecialSyntax);
	NODE_PRINT(printer, args);

	return "SysFuncCallNode";
}

void SysFuncCallNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = name;
}

void SysFuncCallNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (args->items.getCount() > MAX_UCHAR)
	{
		status_exception::raise(
			Arg::Gds(isc_max_args_exceeded) << Arg::Num(MAX_UCHAR) << function->name);
	}

	dsqlScratch->appendUChar(blr_sys_function);
	dsqlScratch->appendMetaString(function->name.c_str());
	dsqlScratch->appendUChar(args->items.getCount());

	for (auto& arg : args->items)
		GEN_expr(dsqlScratch, arg);
}

void SysFuncCallNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	Array<const dsc*> argsArray;

	for (auto& arg : args->items)
	{
		MAKE_desc(dsqlScratch, &arg->nodDesc, arg);
		argsArray.add(&arg->nodDesc);
	}

	DSqlDataTypeUtil dataTypeUtil(dsqlScratch);
	function->checkArgsMismatch(argsArray.getCount());
	function->makeFunc(&dataTypeUtil, function, desc, argsArray.getCount(), argsArray.begin());
}

void SysFuncCallNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	Array<const dsc*> argsArray;

	for (auto& arg : args->items)
	{
		dsc* targetDesc = FB_NEW_POOL(*tdbb->getDefaultPool()) dsc();
		argsArray.push(targetDesc);
		arg->getDesc(tdbb, csb, targetDesc);

		// dsc_address is verified in makeFunc to get literals. If the node is not a
		// literal, set it to NULL, to prevent wrong interpretation of offsets as
		// pointers - CORE-2612.
		if (!nodeIs<LiteralNode>(arg))
			targetDesc->dsc_address = NULL;
	}

	DataTypeUtil dataTypeUtil(tdbb);
	function->makeFunc(&dataTypeUtil, function, desc, argsArray.getCount(), argsArray.begin());

	for (const auto& pArgs : argsArray)
		delete pArgs;
}

ValueExprNode* SysFuncCallNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	SysFuncCallNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) SysFuncCallNode(
		*tdbb->getDefaultPool(), name);
	node->args = copier.copy(tdbb, args);
	node->function = function;
	return node;
}

bool SysFuncCallNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const SysFuncCallNode* otherNode = nodeAs<SysFuncCallNode>(other);

	return name == otherNode->name;
}

bool SysFuncCallNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const SysFuncCallNode* const otherNode = nodeAs<SysFuncCallNode>(other);
	fb_assert(otherNode);

	return function && function == otherNode->function;
}

ValueExprNode* SysFuncCallNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	function->checkArgsMismatch(args->items.getCount());

	return this;
}

dsc* SysFuncCallNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* impure = request->getImpure<impure_value>(impureOffset);
	return function->evlFunc(tdbb, function, args->items, impure);
}

ValueExprNode* SysFuncCallNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	QualifiedName qualifName(name);

	if (!dsqlSpecialSyntax && METD_get_function(dsqlScratch->getTransaction(), dsqlScratch, qualifName))
	{
		UdfCallNode* node = FB_NEW_POOL(dsqlScratch->getPool()) UdfCallNode(dsqlScratch->getPool(), qualifName, args);
		return node->dsqlPass(dsqlScratch);
	}

	SysFuncCallNode* node = FB_NEW_POOL(dsqlScratch->getPool()) SysFuncCallNode(dsqlScratch->getPool(), name,
		doDsqlPass(dsqlScratch, args));
	node->dsqlSpecialSyntax = dsqlSpecialSyntax;

	node->function = SysFunction::lookup(name);

	if (node->function && node->function->setParamsFunc)
	{
		ValueListNode* inList = node->args;
		Array<dsc*> argsArray;

		for (unsigned int i = 0; i < inList->items.getCount(); ++i)
		{
			ValueExprNode* p = inList->items[i];
			MAKE_desc(dsqlScratch, &p->nodDesc, p);
			argsArray.add(&p->nodDesc);
		}

		DSqlDataTypeUtil dataTypeUtil(dsqlScratch);
		node->function->setParamsFunc(&dataTypeUtil, node->function,
			argsArray.getCount(), argsArray.begin());

		for (unsigned int i = 0; i < inList->items.getCount(); ++i)
		{
			ValueExprNode* p = inList->items[i];
			PASS1_set_parameter_type(dsqlScratch, p, &p->nodDesc, false);
		}
	}

	return node;
}


//--------------------


static RegisterNode<TrimNode> regTrimNode(blr_trim);

TrimNode::TrimNode(MemoryPool& pool, UCHAR aWhere, ValueExprNode* aValue, ValueExprNode* aTrimChars)
	: TypedNode<ValueExprNode, ExprNode::TYPE_TRIM>(pool),
	  where(aWhere),
	  value(aValue),
	  trimChars(aTrimChars)
{
}

DmlNode* TrimNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	UCHAR where = csb->csb_blr_reader.getByte();
	UCHAR what = csb->csb_blr_reader.getByte();

	TrimNode* node = FB_NEW_POOL(pool) TrimNode(pool, where);

	if (what == blr_trim_characters)
		node->trimChars = PAR_parse_value(tdbb, csb);

	node->value = PAR_parse_value(tdbb, csb);

	return node;
}

string TrimNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, where);
	NODE_PRINT(printer, value);
	NODE_PRINT(printer, trimChars);

	return "TrimNode";
}

ValueExprNode* TrimNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	TrimNode* node = FB_NEW_POOL(dsqlScratch->getPool()) TrimNode(dsqlScratch->getPool(), where,
		doDsqlPass(dsqlScratch, value), doDsqlPass(dsqlScratch, trimChars));

	// Try to force trimChars to be same type as value: TRIM(? FROM FIELD)
	PASS1_set_parameter_type(dsqlScratch, node->trimChars, node->value, false);

	return node;
}

void TrimNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "TRIM";
}

bool TrimNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, value, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, trimChars, desc, forceVarChar);
}

void TrimNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_trim);
	dsqlScratch->appendUChar(where);

	if (trimChars)
	{
		dsqlScratch->appendUChar(blr_trim_characters);
		GEN_expr(dsqlScratch, trimChars);
	}
	else
		dsqlScratch->appendUChar(blr_trim_spaces);

	GEN_expr(dsqlScratch, value);
}

void TrimNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	dsc desc1, desc2;

	MAKE_desc(dsqlScratch, &desc1, value);

	if (trimChars)
		MAKE_desc(dsqlScratch, &desc2, trimChars);
	else
		desc2.dsc_flags = 0;

	if (desc1.dsc_dtype == dtype_blob)
	{
		*desc = desc1;
		desc->dsc_flags |= (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;
	}
	else if (desc1.dsc_dtype <= dtype_any_text)
	{
		*desc = desc1;
		desc->dsc_dtype = dtype_varying;
		desc->dsc_length = static_cast<int>(sizeof(USHORT)) + DSC_string_length(&desc1);
		desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;
	}
	else
	{
		desc->dsc_dtype = dtype_varying;
		desc->dsc_scale = 0;
		desc->dsc_ttype() = ttype_ascii;
		desc->dsc_length = static_cast<int>(sizeof(USHORT)) + DSC_string_length(&desc1);
		desc->dsc_flags = (desc1.dsc_flags | desc2.dsc_flags) & DSC_nullable;
	}
}

void TrimNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	value->getDesc(tdbb, csb, desc);

	if (trimChars)
	{
		dsc desc1;
		trimChars->getDesc(tdbb, csb, &desc1);
		desc->dsc_flags |= desc1.dsc_flags & DSC_null;
	}

	if (desc->dsc_dtype != dtype_blob)
	{
		USHORT length = DSC_string_length(desc);

		if (!DTYPE_IS_TEXT(desc->dsc_dtype))
		{
			desc->dsc_ttype() = ttype_ascii;
			desc->dsc_scale = 0;
		}

		desc->dsc_dtype = dtype_varying;
		desc->dsc_length = length + sizeof(USHORT);
	}
}

ValueExprNode* TrimNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	TrimNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) TrimNode(*tdbb->getDefaultPool(), where);
	node->value = copier.copy(tdbb, value);
	if (trimChars)
		node->trimChars = copier.copy(tdbb, trimChars);
	return node;
}

bool TrimNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const TrimNode* o = nodeAs<TrimNode>(other);
	fb_assert(o);

	return where == o->where;
}

bool TrimNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const TrimNode* const otherNode = nodeAs<TrimNode>(other);
	fb_assert(otherNode);

	return where == otherNode->where;
}

ValueExprNode* TrimNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

// Perform trim function = TRIM([where what FROM] string).
dsc* TrimNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* impure = request->getImpure<impure_value>(impureOffset);
	request->req_flags &= ~req_null;

	dsc* trimCharsDesc = (trimChars ? EVL_expr(tdbb, request, trimChars) : NULL);
	if (request->req_flags & req_null)
		return NULL;

	dsc* valueDesc = EVL_expr(tdbb, request, value);
	if (request->req_flags & req_null)
		return NULL;

	USHORT ttype = INTL_TEXT_TYPE(*valueDesc);
	TextType* tt = INTL_texttype_lookup(tdbb, ttype);
	CharSet* cs = tt->getCharSet();

	const UCHAR* charactersAddress;
	MoveBuffer charactersBuffer;
	ULONG charactersLength;

	if (trimCharsDesc)
	{
		UCHAR* tempAddress = NULL;

		if (trimCharsDesc->isBlob())
		{
			UCharBuffer bpb;
			CharSet* charsCharSet;

			if (trimCharsDesc->getBlobSubType() == isc_blob_text)
			{
				BLB_gen_bpb_from_descs(trimCharsDesc, valueDesc, bpb);
				charsCharSet = INTL_charset_lookup(tdbb, trimCharsDesc->getCharSet());
			}
			else
				charsCharSet = cs;

			blb* blob = blb::open2(tdbb, request->req_transaction,
				reinterpret_cast<bid*>(trimCharsDesc->dsc_address), bpb.getCount(), bpb.begin());

			// Go simple way and always read entire blob in memory.

			unsigned maxLen = blob->blb_length / charsCharSet->minBytesPerChar() *
				cs->maxBytesPerChar();

			tempAddress = charactersBuffer.getBuffer(maxLen);
			charactersLength = blob->BLB_get_data(tdbb, tempAddress, maxLen, true);
		}
		else
		{
			charactersLength = MOV_make_string2(tdbb, trimCharsDesc, ttype,
				&tempAddress, charactersBuffer);
		}

		charactersAddress = tempAddress;
	}
	else
	{
		charactersLength = tt->getCharSet()->getSpaceLength();
		charactersAddress = tt->getCharSet()->getSpace();
	}

	HalfStaticArray<UCHAR, BUFFER_SMALL> charactersCanonical;
	charactersCanonical.getBuffer(charactersLength / tt->getCharSet()->minBytesPerChar() *
		tt->getCanonicalWidth());
	const SLONG charactersCanonicalLen = tt->canonical(charactersLength, charactersAddress,
		charactersCanonical.getCount(), charactersCanonical.begin()) * tt->getCanonicalWidth();

	MoveBuffer valueBuffer;
	UCHAR* valueAddress;
	ULONG valueLength;

	if (valueDesc->isBlob())
	{
		// Source string is a blob, things get interesting.
		blb* blob = blb::open(tdbb, request->req_transaction,
			reinterpret_cast<bid*>(valueDesc->dsc_address));

		// It's very difficult (and probably not very efficient) to trim a blob in chunks.
		// So go simple way and always read entire blob in memory.
		valueAddress = valueBuffer.getBuffer(blob->blb_length);
		valueLength = blob->BLB_get_data(tdbb, valueAddress, blob->blb_length, true);
	}
	else
		valueLength = MOV_make_string2(tdbb, valueDesc, ttype, &valueAddress, valueBuffer);

	HalfStaticArray<UCHAR, BUFFER_SMALL> valueCanonical;
	valueCanonical.getBuffer(valueLength / cs->minBytesPerChar() * tt->getCanonicalWidth());
	const SLONG valueCanonicalLen = tt->canonical(valueLength, valueAddress,
		valueCanonical.getCount(), valueCanonical.begin()) * tt->getCanonicalWidth();

	SLONG offsetLead = 0;
	SLONG offsetTrail = valueCanonicalLen;

	// CVC: Avoid endless loop with zero length trim chars.
	if (charactersCanonicalLen)
	{
		if (where == blr_trim_both || where == blr_trim_leading)
		{
			// CVC: Prevent surprises with offsetLead < valueCanonicalLen; it may fail.
			for (; offsetLead + charactersCanonicalLen <= valueCanonicalLen;
				 offsetLead += charactersCanonicalLen)
			{
				if (memcmp(charactersCanonical.begin(), &valueCanonical[offsetLead],
						charactersCanonicalLen) != 0)
				{
					break;
				}
			}
		}

		if (where == blr_trim_both || where == blr_trim_trailing)
		{
			for (; offsetTrail - charactersCanonicalLen >= offsetLead;
				 offsetTrail -= charactersCanonicalLen)
			{
				if (memcmp(charactersCanonical.begin(),
						&valueCanonical[offsetTrail - charactersCanonicalLen],
						charactersCanonicalLen) != 0)
				{
					break;
				}
			}
		}
	}

	if (valueDesc->isBlob())
	{
		// We have valueCanonical already allocated.
		// Use it to get the substring that will be written to the new blob.
		const ULONG len = cs->substring(valueLength, valueAddress,
			valueCanonical.getCapacity(), valueCanonical.begin(),
			offsetLead / tt->getCanonicalWidth(),
			(offsetTrail - offsetLead) / tt->getCanonicalWidth());

		EVL_make_value(tdbb, valueDesc, impure);

		blb* newBlob = blb::create(tdbb, tdbb->getRequest()->req_transaction, &impure->vlu_misc.vlu_bid);
		newBlob->BLB_put_data(tdbb, valueCanonical.begin(), len);
		newBlob->BLB_close(tdbb);
	}
	else
	{
		dsc desc;
		desc.makeText(valueLength, ttype);
		EVL_make_value(tdbb, &desc, impure);

		impure->vlu_desc.dsc_length = cs->substring(valueLength, valueAddress,
			impure->vlu_desc.dsc_length, impure->vlu_desc.dsc_address,
			offsetLead / tt->getCanonicalWidth(),
			(offsetTrail - offsetLead) / tt->getCanonicalWidth());
	}

	return &impure->vlu_desc;
}


//--------------------


static RegisterNode<UdfCallNode> regUdfCallNode1(blr_function);
static RegisterNode<UdfCallNode> regUdfCallNode2(blr_function2);
static RegisterNode<UdfCallNode> regUdfCallNode3(blr_subfunc);

UdfCallNode::UdfCallNode(MemoryPool& pool, const QualifiedName& aName, ValueListNode* aArgs)
	: TypedNode<ValueExprNode, ExprNode::TYPE_UDF_CALL>(pool),
	  name(pool, aName),
	  args(aArgs),
	  function(NULL),
	  dsqlFunction(NULL),
	  isSubRoutine(false)
{
}

DmlNode* UdfCallNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	const UCHAR blrOp)
{
	const UCHAR* savePos = csb->csb_blr_reader.getPos();

	QualifiedName name;

	if (blrOp == blr_function2)
		csb->csb_blr_reader.getMetaName(name.package);

	csb->csb_blr_reader.getMetaName(name.identifier);

	const USHORT count = name.package.length() + name.identifier.length();

	UdfCallNode* node = FB_NEW_POOL(pool) UdfCallNode(pool, name);

	if (blrOp == blr_function &&
		(name.identifier == "RDB$GET_CONTEXT" || name.identifier == "RDB$SET_CONTEXT"))
	{
		csb->csb_blr_reader.setPos(savePos);
		return SysFuncCallNode::parse(tdbb, pool, csb, blr_sys_function);
	}

	if (blrOp == blr_subfunc)
	{
		DeclareSubFuncNode* declareNode;

		for (auto curCsb = csb; curCsb && !node->function; curCsb = curCsb->mainCsb)
		{
			if (curCsb->subFunctions.get(name.identifier, declareNode))
				node->function = declareNode->routine;
		}
	}

	Function* function = node->function;

	if (!function)
		function = node->function = Function::lookup(tdbb, name, false);

	if (function)
	{
		if (function->isImplemented() && !function->isDefined())
		{
			if (tdbb->getAttachment()->isGbak())
			{
				PAR_warning(Arg::Warning(isc_funnotdef) << Arg::Str(name.toString()) <<
							Arg::Warning(isc_modnotfound));
			}
			else
			{
				csb->csb_blr_reader.seekBackward(count);
				PAR_error(csb, Arg::Gds(isc_funnotdef) << Arg::Str(name.toString()) <<
						   Arg::Gds(isc_modnotfound));
			}
		}
	}
	else
	{
		csb->csb_blr_reader.seekBackward(count);
		PAR_error(csb, Arg::Gds(isc_funnotdef) << Arg::Str(name.toString()));
	}

	node->isSubRoutine = function->isSubRoutine();

	const UCHAR argCount = csb->csb_blr_reader.getByte();

	// Check to see if the argument count matches.
	if (argCount < function->fun_inputs - function->getDefaultCount() || argCount > function->fun_inputs)
		PAR_error(csb, Arg::Gds(isc_funmismat) << name.toString());

	node->args = PAR_args(tdbb, csb, argCount, function->fun_inputs);

	for (USHORT i = argCount; i < function->fun_inputs; ++i)
	{
		Parameter* const parameter = function->getInputFields()[i];
		node->args->items[i] = CMP_clone_node(tdbb, csb, parameter->prm_default_value);
	}

	// CVC: I will track ufds only if a function is not being dropped.
	if (!function->isSubRoutine() && (csb->csb_g_flags & csb_get_dependencies))
	{
		CompilerScratch::Dependency dependency(obj_udf);
		dependency.function = function;
		csb->csb_dependencies.push(dependency);
	}

	return node;
}

string UdfCallNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, name);
	NODE_PRINT(printer, args);

	return "UdfCallNode";
}

void UdfCallNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = dsqlFunction->udf_name.identifier;
}

void UdfCallNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (dsqlFunction->udf_name.package.isEmpty())
		dsqlScratch->appendUChar((dsqlFunction->udf_flags & UDF_subfunc) ? blr_subfunc : blr_function);
	else
	{
		dsqlScratch->appendUChar(blr_function2);
		dsqlScratch->appendMetaString(dsqlFunction->udf_name.package.c_str());
	}

	dsqlScratch->appendMetaString(dsqlFunction->udf_name.identifier.c_str());
	dsqlScratch->appendUChar(args->items.getCount());

	for (auto& arg : args->items)
		GEN_expr(dsqlScratch, arg);
}

void UdfCallNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	desc->dsc_dtype = static_cast<UCHAR>(dsqlFunction->udf_dtype);
	desc->dsc_length = dsqlFunction->udf_length;
	desc->dsc_scale = static_cast<SCHAR>(dsqlFunction->udf_scale);
	// CVC: Setting flags to zero obviously impeded DSQL to acknowledge
	// the fact that any UDF can return NULL simply returning a NULL
	// pointer.
	desc->setNullable(true);

	if (desc->dsc_dtype <= dtype_any_text)
		desc->dsc_ttype() = dsqlFunction->udf_character_set_id;
	else
		desc->dsc_ttype() = dsqlFunction->udf_sub_type;
}

void UdfCallNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	// Null value for the function indicates that the function was not
	// looked up during parsing the BLR. This is true if the function
	// referenced in the procedure BLR was dropped before dropping the
	// procedure itself. Ignore the case because we are currently trying
	// to drop the procedure.
	// For normal requests, function would never be null. We would have
	// created a valid block while parsing.
	if (function)
		*desc = function->getOutputFields()[0]->prm_desc;
	else
		desc->clear();
}

ValueExprNode* UdfCallNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	UdfCallNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) UdfCallNode(*tdbb->getDefaultPool(), name);
	node->args = copier.copy(tdbb, args);
	node->function = isSubRoutine ? function : Function::lookup(tdbb, name, false);
	return node;
}

bool UdfCallNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	if (!ExprNode::dsqlMatch(dsqlScratch, other, ignoreMapCast))
		return false;

	const UdfCallNode* otherNode = nodeAs<UdfCallNode>(other);

	return name == otherNode->name;
}

bool UdfCallNode::sameAs(CompilerScratch* csb, const ExprNode* other, bool ignoreStreams) const
{
	if (!ExprNode::sameAs(csb, other, ignoreStreams))
		return false;

	const UdfCallNode* const otherNode = nodeAs<UdfCallNode>(other);
	fb_assert(otherNode);

	return function && function == otherNode->function;
}

ValueExprNode* UdfCallNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass1(tdbb, csb);

	if (!function->isSubRoutine())
	{
		if (!(csb->csb_g_flags & (csb_internal | csb_ignore_perm)))
		{
			if (function->getName().package.isEmpty())
			{
				SLONG ssRelationId = csb->csb_view ? csb->csb_view->rel_id : 0;

				if (!ssRelationId && csb->csb_parent_relation)
				{
					fb_assert(csb->csb_parent_relation->rel_ss_definer.value);
					ssRelationId = csb->csb_parent_relation->rel_id;
				}

				CMP_post_access(tdbb, csb, function->getSecurityName(), ssRelationId,
					SCL_execute, SCL_object_function, function->getName().identifier);
			}
			else
			{
				CMP_post_access(tdbb, csb, function->getSecurityName(),
					(csb->csb_view ? csb->csb_view->rel_id : 0),
					SCL_execute, SCL_object_package, function->getName().package);
			}

			ExternalAccess temp(ExternalAccess::exa_function, function->getId());
			FB_SIZE_T idx;
			if (!csb->csb_external.find(temp, idx))
				csb->csb_external.insert(idx, temp);
		}

		CMP_post_resource(&csb->csb_resources, function, Resource::rsc_function, function->getId());
	}

	return this;
}

ValueExprNode* UdfCallNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	if (function->fun_deterministic && !function->fun_inputs)
	{
		// Deterministic function without input arguments is expected to be
		// always returning the same result, so it can be marked as invariant
		nodFlags |= FLAG_INVARIANT;
		csb->csb_invariants.push(&impureOffset);
	}

	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);

	impureOffset = CMP_impure(csb, sizeof(Impure));

	if (function->isDefined() && !function->fun_entrypoint)
	{
		if (function->getInputFormat() && function->getInputFormat()->fmt_count)
		{
			fb_assert(function->getInputFormat()->fmt_length);
			CMP_impure(csb, function->getInputFormat()->fmt_length);
		}

		fb_assert(function->getOutputFormat()->fmt_count == 3);

		fb_assert(function->getOutputFormat()->fmt_length);
		CMP_impure(csb, function->getOutputFormat()->fmt_length);
	}

	return this;
}

dsc* UdfCallNode::execute(thread_db* tdbb, jrd_req* request) const
{
	UCHAR* impure = request->getImpure<UCHAR>(impureOffset);
	Impure* impureArea = request->getImpure<Impure>(impureOffset);
	impure_value* value = &impureArea->value;

	USHORT& invariantFlags = value->vlu_flags;

	// If the function is known as being both deterministic and invariant,
	// check whether it has already been evaluated

	if (nodFlags & FLAG_INVARIANT)
	{
		if (invariantFlags & VLU_computed)
		{
			if (invariantFlags & VLU_null)
				request->req_flags |= req_null;
			else
				request->req_flags &= ~req_null;

			return (request->req_flags & req_null) ? NULL : &value->vlu_desc;
		}
	}

	if (!function->isImplemented())
	{
		status_exception::raise(
			Arg::Gds(isc_func_pack_not_implemented) <<
				Arg::Str(function->getName().identifier) << Arg::Str(function->getName().package));
	}
	else if (!function->isDefined())
	{
		status_exception::raise(
			Arg::Gds(isc_funnotdef) << Arg::Str(function->getName().toString()) <<
			Arg::Gds(isc_modnotfound));
	}

	// Evaluate the function.

	if (function->fun_entrypoint)
	{
		const Parameter* const returnParam = function->getOutputFields()[0];
		value->vlu_desc = returnParam->prm_desc;

		// If the return data type is any of the string types, allocate space to hold value.

		if (value->vlu_desc.dsc_dtype <= dtype_varying)
		{
			const USHORT retLength = value->vlu_desc.dsc_length;
			VaryingString* string = value->vlu_string;

			if (string && string->str_length < retLength)
			{
				delete string;
				string = NULL;
			}

			if (!string)
			{
				string = FB_NEW_RPT(*tdbb->getDefaultPool(), retLength) VaryingString;
				string->str_length = retLength;
				value->vlu_string = string;
			}

			value->vlu_desc.dsc_address = string->str_data;
		}
		else
			value->vlu_desc.dsc_address = (UCHAR*) &value->vlu_misc;

		if (!impureArea->temp)
		{
			impureArea->temp =
				FB_NEW_POOL(*tdbb->getDefaultPool()) Array<UCHAR>(*tdbb->getDefaultPool());
		}

		FUN_evaluate(tdbb, function, args->items, value, *impureArea->temp);
	}
	else
	{
		Jrd::Attachment* attachment = tdbb->getAttachment();

		const ULONG inMsgLength = function->getInputFormat() ? function->getInputFormat()->fmt_length : 0;
		const ULONG outMsgLength = function->getOutputFormat()->fmt_length;
		UCHAR* const inMsg = FB_ALIGN(impure + sizeof(impure_value), FB_ALIGNMENT);
		UCHAR* const outMsg = FB_ALIGN(inMsg + inMsgLength, FB_ALIGNMENT);

		if (function->fun_inputs != 0)
		{
			const dsc* fmtDesc = function->getInputFormat()->fmt_desc.begin();

			for (auto& source : args->items)
			{
				const ULONG argOffset = (IPTR) fmtDesc[0].dsc_address;
				const ULONG nullOffset = (IPTR) fmtDesc[1].dsc_address;

				dsc argDesc = fmtDesc[0];
				argDesc.dsc_address = inMsg + argOffset;

				SSHORT* const nullPtr = reinterpret_cast<SSHORT*>(inMsg + nullOffset);

				dsc* const srcDesc = EVL_expr(tdbb, request, source);
				if (srcDesc && !(request->req_flags & req_null))
				{
					*nullPtr = 0;
					MOV_move(tdbb, srcDesc, &argDesc);
				}
				else
					*nullPtr = -1;

				fmtDesc += 2;
			}
		}

		jrd_tra* transaction = request->req_transaction;
		const SavNumber savNumber = transaction->tra_save_point ?
			transaction->tra_save_point->getNumber() : 0;

		jrd_req* funcRequest = function->getStatement()->findRequest(tdbb);

		// trace function execution start
		TraceFuncExecute trace(tdbb, funcRequest, request, inMsg, inMsgLength);

		// Catch errors so we can unwind cleanly.

		try
		{
			Jrd::ContextPoolHolder context(tdbb, funcRequest->req_pool);	// Save the old pool.

			funcRequest->req_timestamp_utc = request->req_timestamp_utc;

			EXE_start(tdbb, funcRequest, transaction);

			if (inMsgLength != 0)
				EXE_send(tdbb, funcRequest, 0, inMsgLength, inMsg);

			EXE_receive(tdbb, funcRequest, 1, outMsgLength, outMsg);

			// Clean up all savepoints started during execution of the procedure.

			if (!(transaction->tra_flags & TRA_system))
			{
				while (transaction->tra_save_point &&
					transaction->tra_save_point->getNumber() > savNumber)
				{
					transaction->rollforwardSavepoint(tdbb);
				}
			}
		}
		catch (const Exception& ex)
		{
			ex.stuffException(tdbb->tdbb_status_vector);
			const bool noPriv = (tdbb->tdbb_status_vector->getErrors()[1] == isc_no_priv);
			trace.finish(noPriv ? ITracePlugin::RESULT_UNAUTHORIZED : ITracePlugin::RESULT_FAILED);

			EXE_unwind(tdbb, funcRequest);
			funcRequest->req_attachment = NULL;
			funcRequest->req_flags &= ~(req_in_use | req_proc_fetch);
			funcRequest->req_timestamp_utc.invalidate();
			throw;
		}

		const dsc* fmtDesc = function->getOutputFormat()->fmt_desc.begin();
		const ULONG nullOffset = (IPTR) fmtDesc[1].dsc_address;
		SSHORT* const nullPtr = reinterpret_cast<SSHORT*>(outMsg + nullOffset);

		if (*nullPtr)
		{
			request->req_flags |= req_null;
			trace.finish(ITracePlugin::RESULT_SUCCESS);
		}
		else
		{
			request->req_flags &= ~req_null;

			const ULONG argOffset = (IPTR) fmtDesc[0].dsc_address;
			value->vlu_desc = *fmtDesc;
			value->vlu_desc.dsc_address = outMsg + argOffset;

			trace.finish(ITracePlugin::RESULT_SUCCESS, &value->vlu_desc);
		}

		EXE_unwind(tdbb, funcRequest);

		funcRequest->req_attachment = NULL;
		funcRequest->req_flags &= ~(req_in_use | req_proc_fetch);
		funcRequest->req_timestamp_utc.invalidate();
	}

	if (!(request->req_flags & req_null))
		INTL_adjust_text_descriptor(tdbb, &value->vlu_desc);

	// If the function is declared as invariant, mark it as computed.
	if (nodFlags & FLAG_INVARIANT)
	{
		invariantFlags |= VLU_computed;

		if (request->req_flags & req_null)
			invariantFlags |= VLU_null;
	}

	return (request->req_flags & req_null) ? NULL : &value->vlu_desc;
}

ValueExprNode* UdfCallNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	UdfCallNode* node = FB_NEW_POOL(dsqlScratch->getPool()) UdfCallNode(dsqlScratch->getPool(), name,
		doDsqlPass(dsqlScratch, args));

	if (name.package.isEmpty())
	{
		DeclareSubFuncNode* subFunction = dsqlScratch->getSubFunction(name.identifier);
		node->dsqlFunction = subFunction ? subFunction->dsqlFunction : NULL;
	}

	if (!node->dsqlFunction)
		node->dsqlFunction = METD_get_function(dsqlScratch->getTransaction(), dsqlScratch, name);

	if (!node->dsqlFunction)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-804) <<
				  Arg::Gds(isc_dsql_function_err) <<
				  Arg::Gds(isc_random) << Arg::Str(name.toString()));
	}

	const USHORT arg_count = node->dsqlFunction->udf_arguments.getCount();
	const USHORT count = node->args->items.getCount();
	if (count > arg_count || count < arg_count - node->dsqlFunction->udf_def_count)
		ERRD_post(Arg::Gds(isc_fun_param_mismatch) << Arg::Str(name.toString()));

	unsigned pos = 0;

	for (auto& arg : node->args->items)
	{
		if (pos < node->dsqlFunction->udf_arguments.getCount())
			PASS1_set_parameter_type(dsqlScratch, arg, &node->dsqlFunction->udf_arguments[pos], false);
		else
		{
			// We should complain here in the future! The parameter is
			// out of bounds or the function doesn't declare input params.
		}

		++pos;
	}

	return node;
}


//--------------------


static RegisterNode<ValueIfNode> regValueIfNode(blr_value_if);

ValueIfNode::ValueIfNode(MemoryPool& pool, BoolExprNode* aCondition, ValueExprNode* aTrueValue,
			ValueExprNode* aFalseValue)
	: TypedNode<ValueExprNode, ExprNode::TYPE_VALUE_IF>(pool),
	  condition(aCondition),
	  trueValue(aTrueValue),
	  falseValue(aFalseValue)
{
}

DmlNode* ValueIfNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	ValueIfNode* node = FB_NEW_POOL(pool) ValueIfNode(pool);
	node->condition = PAR_parse_boolean(tdbb, csb);
	node->trueValue = PAR_parse_value(tdbb, csb);
	node->falseValue = PAR_parse_value(tdbb, csb);

	// Get rid of blr_stmt_expr expressions.

	// Coalesce.
	MissingBoolNode* missing = nodeAs<MissingBoolNode>(node->condition);
	if (missing)
	{
		StmtExprNode* stmtExpr = nodeAs<StmtExprNode>(missing->arg);
		if (!stmtExpr)
			return node;

		bool firstAssign = true;
		AssignmentNode* assignStmt;
		Array<USHORT> nullVariables;

		do
		{
			CompoundStmtNode* stmt = nodeAs<CompoundStmtNode>(stmtExpr->stmt);
			VariableNode* var = NULL;

			if (stmt)
			{
				DeclareVariableNode* declStmt;

				if (stmt->statements.getCount() != 2 ||
					!(declStmt = nodeAs<DeclareVariableNode>(stmt->statements[0])) ||
					!(assignStmt = nodeAs<AssignmentNode>(stmt->statements[1])) ||
					!(var = nodeAs<VariableNode>(assignStmt->asgnTo)) ||
					var->varId != declStmt->varId)
				{
					return node;
				}
			}
			else if (!(assignStmt = nodeAs<AssignmentNode>(stmtExpr->stmt)) ||
				!(var = nodeAs<VariableNode>(assignStmt->asgnTo)))
			{
				return node;
			}

			nullVariables.add(var->varId);

			if (firstAssign)
			{
				firstAssign = false;

				VariableNode* var2 = nodeAs<VariableNode>(node->falseValue);

				if (!var2 || var->varId != var2->varId)
					return node;
			}

			stmtExpr = nodeAs<StmtExprNode>(assignStmt->asgnFrom);
		} while (stmtExpr);

		CoalesceNode* coalesceNode = FB_NEW_POOL(pool) CoalesceNode(pool);
		coalesceNode->args = FB_NEW_POOL(pool) ValueListNode(pool, 2);
		coalesceNode->args->items[0] = assignStmt->asgnFrom;
		coalesceNode->args->items[1] = node->trueValue;

		// Variables known to be NULL may be removed from the coalesce. This is not only an optimization!
		// If not removed, error will happen as they correspondents declare nodes were removed.
		if (CoalesceNode* subCoalesceNode = nodeAs<CoalesceNode>(node->trueValue))
		{
			NestValueArray& childItems = subCoalesceNode->args->items;

			for (int i = childItems.getCount() - 1; i >= 0; --i)
			{
				if (VariableNode* childVar = nodeAs<VariableNode>(childItems[i]))
				{
					if (nullVariables.exist(childVar->varId))
						childItems.remove(i);
				}
			}
		}

		return coalesceNode;
	}

	// Decode.
	ComparativeBoolNode* cmp = nodeAs<ComparativeBoolNode>(node->condition);
	if (cmp && cmp->blrOp == blr_eql)
	{
		StmtExprNode* cmpCond = nodeAs<StmtExprNode>(cmp->arg1);
		if (!cmpCond)
			return node;

		CompoundStmtNode* stmt = nodeAs<CompoundStmtNode>(cmpCond->stmt);
		DeclareVariableNode* declStmt = NULL;
		AssignmentNode* assignStmt;

		if (stmt)
		{
			if (stmt->statements.getCount() != 2 ||
				!(declStmt = nodeAs<DeclareVariableNode>(stmt->statements[0])) ||
				!(assignStmt = nodeAs<AssignmentNode>(stmt->statements[1])))
			{
				return node;
			}
		}
		else if (!(assignStmt = nodeAs<AssignmentNode>(cmpCond->stmt)))
			return node;

		VariableNode* var = nodeAs<VariableNode>(assignStmt->asgnTo);

		if (!var || (declStmt && declStmt->varId != var->varId))
			return node;

		DecodeNode* decodeNode = FB_NEW_POOL(pool) DecodeNode(pool);
		decodeNode->test = assignStmt->asgnFrom;
		decodeNode->conditions = FB_NEW_POOL(pool) ValueListNode(pool, 0u);
		decodeNode->values = FB_NEW_POOL(pool) ValueListNode(pool, 0u);

		decodeNode->conditions->add(cmp->arg2);
		decodeNode->values->add(node->trueValue);

		ValueExprNode* last = node->falseValue;
		while ((node = nodeAs<ValueIfNode>(last)))
		{
			ComparativeBoolNode* cmp = nodeAs<ComparativeBoolNode>(node->condition);
			if (!cmp || cmp->blrOp != blr_eql)
				break;

			VariableNode* var2 = nodeAs<VariableNode>(cmp->arg1);

			if (!var2 || var2->varId != var->varId)
				break;

			decodeNode->conditions->add(cmp->arg2);
			decodeNode->values->add(node->trueValue);

			last = node->falseValue;
		}

		decodeNode->values->add(last);

		return decodeNode;
	}

	return node;
}

string ValueIfNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, condition);
	NODE_PRINT(printer, trueValue);
	NODE_PRINT(printer, falseValue);

	return "ValueIfNode";
}

ValueExprNode* ValueIfNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	ValueIfNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ValueIfNode(dsqlScratch->getPool(),
		doDsqlPass(dsqlScratch, condition),
		doDsqlPass(dsqlScratch, trueValue),
		doDsqlPass(dsqlScratch, falseValue));

	PASS1_set_parameter_type(dsqlScratch, node->trueValue, node->falseValue, false);
	PASS1_set_parameter_type(dsqlScratch, node->falseValue, node->trueValue, false);

	return node;
}

void ValueIfNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = "CASE";
}

bool ValueIfNode::setParameterType(DsqlCompilerScratch* dsqlScratch,
	const dsc* desc, bool forceVarChar)
{
	return PASS1_set_parameter_type(dsqlScratch, trueValue, desc, forceVarChar) |
		PASS1_set_parameter_type(dsqlScratch, falseValue, desc, forceVarChar);
}

void ValueIfNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsc desc;
	make(dsqlScratch, &desc);
	dsqlScratch->appendUChar(blr_cast);
	GEN_descriptor(dsqlScratch, &desc, true);

	dsqlScratch->appendUChar(blr_value_if);
	GEN_expr(dsqlScratch, condition);
	GEN_expr(dsqlScratch, trueValue);
	GEN_expr(dsqlScratch, falseValue);
}

void ValueIfNode::make(DsqlCompilerScratch* dsqlScratch, dsc* desc)
{
	Array<const dsc*> args;

	MAKE_desc(dsqlScratch, &trueValue->nodDesc, trueValue);
	args.add(&trueValue->nodDesc);

	MAKE_desc(dsqlScratch, &falseValue->nodDesc, falseValue);
	args.add(&falseValue->nodDesc);

	DSqlDataTypeUtil(dsqlScratch).makeFromList(desc, "CASE", args.getCount(), args.begin());
}

void ValueIfNode::getDesc(thread_db* tdbb, CompilerScratch* csb, dsc* desc)
{
	ValueExprNode* val = nodeIs<NullNode>(trueValue) ? falseValue : trueValue;
	val->getDesc(tdbb, csb, desc);
}

ValueExprNode* ValueIfNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	ValueIfNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) ValueIfNode(*tdbb->getDefaultPool());
	node->condition = copier.copy(tdbb, condition);
	node->trueValue = copier.copy(tdbb, trueValue);
	node->falseValue = copier.copy(tdbb, falseValue);
	return node;
}

ValueExprNode* ValueIfNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass2(tdbb, csb);

	dsc desc;
	getDesc(tdbb, csb, &desc);
	impureOffset = CMP_impure(csb, sizeof(impure_value));

	return this;
}

dsc* ValueIfNode::execute(thread_db* tdbb, jrd_req* request) const
{
	return EVL_expr(tdbb, request, (condition->execute(tdbb, request) ? trueValue : falseValue));
}


//--------------------


static RegisterNode<VariableNode> regVariableNode(blr_variable);

VariableNode::VariableNode(MemoryPool& pool)
	: TypedNode<ValueExprNode, ExprNode::TYPE_VARIABLE>(pool),
	  dsqlName(pool),
	  dsqlVar(NULL),
	  varId(0),
	  varDecl(NULL),
	  varInfo(NULL)
{
}

DmlNode* VariableNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	const USHORT n = csb->csb_blr_reader.getWord();
	vec<DeclareVariableNode*>* vector = csb->csb_variables;

	if (!vector || n >= vector->count())
		PAR_error(csb, Arg::Gds(isc_badvarnum));

	VariableNode* node = FB_NEW_POOL(pool) VariableNode(pool);
	node->varId = n;

	return node;
}

string VariableNode::internalPrint(NodePrinter& printer) const
{
	ValueExprNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlName);
	NODE_PRINT(printer, dsqlVar);
	NODE_PRINT(printer, varId);
	NODE_PRINT(printer, varDecl);
	NODE_PRINT(printer, varInfo);

	return "VariableNode";
}

ValueExprNode* VariableNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	VariableNode* node = FB_NEW_POOL(dsqlScratch->getPool()) VariableNode(dsqlScratch->getPool());
	node->dsqlName = dsqlName;
	node->dsqlVar = dsqlVar ? dsqlVar.getObject() : dsqlScratch->resolveVariable(dsqlName);

	if (!node->dsqlVar)
		PASS1_field_unknown(NULL, dsqlName.c_str(), this);

	return node;
}

void VariableNode::setParameterName(dsql_par* parameter) const
{
	parameter->par_name = parameter->par_alias = dsqlVar->field->fld_name.c_str();
}

void VariableNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	bool execBlock = (dsqlScratch->flags & DsqlCompilerScratch::FLAG_BLOCK) &&
		!(dsqlScratch->flags &
		  (DsqlCompilerScratch::FLAG_PROCEDURE |
		   DsqlCompilerScratch::FLAG_TRIGGER |
		   DsqlCompilerScratch::FLAG_FUNCTION));

	if (dsqlVar->type == dsql_var::TYPE_INPUT && !execBlock)
	{
		dsqlScratch->appendUChar(blr_parameter2);
		dsqlScratch->appendUChar(dsqlVar->msgNumber);
		dsqlScratch->appendUShort(dsqlVar->msgItem);
		dsqlScratch->appendUShort(dsqlVar->msgItem + 1);
	}
	else
	{
		// If this is an EXECUTE BLOCK input parameter, use the internal variable.
		dsqlScratch->appendUChar(blr_variable);
		dsqlScratch->appendUShort(dsqlVar->number);
	}
}

void VariableNode::make(DsqlCompilerScratch* /*dsqlScratch*/, dsc* desc)
{
	*desc = dsqlVar->desc;
}

bool VariableNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool /*ignoreMapCast*/) const
{
	const VariableNode* o = nodeAs<VariableNode>(other);
	if (!o)
		return false;

	if (dsqlVar->field != o->dsqlVar->field ||
		dsqlVar->field->fld_name != o->dsqlVar->field->fld_name ||
		dsqlVar->number != o->dsqlVar->number ||
		dsqlVar->msgItem != o->dsqlVar->msgItem ||
		dsqlVar->msgNumber != o->dsqlVar->msgNumber)
	{
		return false;
	}

	return true;
}

void VariableNode::getDesc(thread_db* /*tdbb*/, CompilerScratch* /*csb*/, dsc* desc)
{
	*desc = varDecl->varDesc;
}

ValueExprNode* VariableNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	VariableNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) VariableNode(*tdbb->getDefaultPool());
	node->varId = copier.csb->csb_remap_variable + varId;
	node->varDecl = varDecl;
	node->varInfo = varInfo;

	return node;
}

ValueExprNode* VariableNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode::pass1(tdbb, csb);

	vec<DeclareVariableNode*>* vector = csb->csb_variables;

	if (!vector || varId >= vector->count() || !(varDecl = (*vector)[varId]))
		PAR_error(csb, Arg::Gds(isc_badvarnum));

	return this;
}

ValueExprNode* VariableNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	varInfo = CMP_pass2_validation(tdbb, csb, Item(Item::TYPE_VARIABLE, varId));

	ValueExprNode::pass2(tdbb, csb);

	impureOffset = CMP_impure(csb, (nodFlags & FLAG_VALUE) ? sizeof(impure_value_ex) : sizeof(dsc));

	return this;
}

dsc* VariableNode::execute(thread_db* tdbb, jrd_req* request) const
{
	impure_value* const impure = request->getImpure<impure_value>(impureOffset);
	impure_value* impure2 = request->getImpure<impure_value>(varDecl->impureOffset);

	request->req_flags &= ~req_null;

	if (impure2->vlu_desc.dsc_flags & DSC_null)
		request->req_flags |= req_null;

	impure->vlu_desc = impure2->vlu_desc;

	if (impure->vlu_desc.dsc_dtype == dtype_text)
		INTL_adjust_text_descriptor(tdbb, &impure->vlu_desc);

	if (!(impure2->vlu_flags & VLU_checked))
	{
		if (varInfo)
		{
			EVL_validate(tdbb, Item(Item::TYPE_VARIABLE, varId), varInfo,
				&impure->vlu_desc, (impure->vlu_desc.dsc_flags & DSC_null));
		}

		impure2->vlu_flags |= VLU_checked;
	}

	return (request->req_flags & req_null) ? NULL : &impure->vlu_desc;
}


//--------------------


Firebird::string RowsClause::internalPrint(NodePrinter& printer) const
{
	NODE_PRINT(printer, length);
	NODE_PRINT(printer, skip);

	return "RowsClause";
}


//--------------------


Firebird::string GeneratorItem::internalPrint(NodePrinter& printer) const
{
	NODE_PRINT(printer, id);
	NODE_PRINT(printer, name);
	NODE_PRINT(printer, secName);

	return "GeneratorItem";
}


//--------------------


// Firebird provides transparent conversion from string to date in
// contexts where it makes sense.  This macro checks a descriptor to
// see if it is something that *could* represent a date value

static bool couldBeDate(const dsc desc)
{
	return DTYPE_IS_DATE(desc.dsc_dtype) || desc.dsc_dtype <= dtype_any_text;
}

// Take the input number, assume it represents a fractional count of days.
// Convert it to a count of microseconds.
static SINT64 getDayFraction(const dsc* d)
{
	dsc result;
	double result_days;
	thread_db* tdbb = JRD_get_thread_data();

	result.dsc_dtype = dtype_double;
	result.dsc_scale = 0;
	result.dsc_flags = 0;
	result.dsc_sub_type = 0;
	result.dsc_length = sizeof(double);
	result.dsc_address = reinterpret_cast<UCHAR*>(&result_days);

	// Convert the input number to a double
	CVT_move(d, &result, tdbb->getAttachment()->att_dec_status);

	// There's likely some loss of precision here due to rounding of number

	// 08-Apr-2004, Nickolay Samofatov. Loss of precision manifested itself as bad
	// result returned by the following query:
	//
	// select (cast('01.01.2004 10:01:00' as timestamp)
	//   -cast('01.01.2004 10:00:00' as timestamp))
	//   +cast('01.01.2004 10:00:00' as timestamp) from rdb$database
	//
	// Let's use llrint where it is supported and offset number for other platforms
	// in hope that compiler rounding mode doesn't get in.

#ifdef HAVE_LLRINT
	return llrint(result_days * ISC_TICKS_PER_DAY);
#else
	const double eps = 0.49999999999999;
	if (result_days >= 0)
		return (SINT64)(result_days * ISC_TICKS_PER_DAY + eps);

	return (SINT64) (result_days * ISC_TICKS_PER_DAY - eps);
#endif
}

// Take the input value, which is either a timestamp or a string representing a timestamp.
// Convert it to a timestamp, and then return that timestamp as a count of isc_ticks since the base
// date and time in MJD time arithmetic.
// ISC_TICKS or isc_ticks are actually deci - milli seconds or tenthousandth of seconds per day.
// This is derived from the ISC_TIME_SECONDS_PRECISION.
static SINT64 getTimeStampToIscTicks(thread_db* tdbb, const dsc* d)
{
	dsc result;
	ISC_TIMESTAMP_TZ result_timestamp;

	result.dsc_dtype = d->isDateTimeTz() ? dtype_timestamp_tz : dtype_timestamp;
	result.dsc_scale = 0;
	result.dsc_flags = 0;
	result.dsc_sub_type = 0;
	result.dsc_length = d->isDateTimeTz() ? sizeof(ISC_TIMESTAMP_TZ) : sizeof(ISC_TIMESTAMP);
	result.dsc_address = reinterpret_cast<UCHAR*>(&result_timestamp);

	CVT_move(d, &result, tdbb->getAttachment()->att_dec_status);

	SINT64 delta = 0;

	return ((SINT64) result_timestamp.utc_timestamp.timestamp_date) * ISC_TICKS_PER_DAY +
		(SINT64) result_timestamp.utc_timestamp.timestamp_time - delta;
}

// One of d1, d2 is time, the other is date
static bool isDateAndTime(const dsc& d1, const dsc& d2)
{
	return ((d1.isTime() && d2.isDate()) || (d2.isTime() && d1.isDate()));
}

// Set parameter info based on a context.
static void setParameterInfo(dsql_par* parameter, const dsql_ctx* context)
{
	if (!context)
		return;

	if (context->ctx_relation)
	{
		parameter->par_rel_name = context->ctx_relation->rel_name.c_str();
		parameter->par_owner_name = context->ctx_relation->rel_owner.c_str();
	}
	else if (context->ctx_procedure)
	{
		parameter->par_rel_name = context->ctx_procedure->prc_name.identifier.c_str();
		parameter->par_owner_name = context->ctx_procedure->prc_owner.c_str();
	}

	parameter->par_rel_alias = context->ctx_alias.c_str();
}


}	// namespace Jrd
