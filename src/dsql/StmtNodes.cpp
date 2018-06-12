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
 * Adriano dos Santos Fernandes - refactored from pass1.cpp, gen.cpp, cmp.cpp, par.cpp and exe.cpp
 */

#include "firebird.h"
#include "../common/TimeZoneUtil.h"
#include "../common/classes/BaseStream.h"
#include "../common/classes/MsgPrint.h"
#include "../common/classes/VaryStr.h"
#include "../dsql/BoolNodes.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/StmtNodes.h"
#include "../jrd/align.h"
#include "../jrd/blr.h"
#include "../jrd/tra.h"
#include "../jrd/Function.h"
#include "../jrd/Optimizer.h"
#include "../jrd/RecordSourceNodes.h"
#include "../jrd/VirtualTable.h"
#include "../jrd/extds/ExtDS.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../jrd/recsrc/Cursor.h"
#include "../jrd/trace/TraceManager.h"
#include "../jrd/trace/TraceJrdHelpers.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/dfw_proto.h"
#include "../jrd/dpm_proto.h"
#include "../jrd/evl_proto.h"
#include "../jrd/exe_proto.h"
#include "../jrd/ext_proto.h"
#include "../jrd/idx_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/par_proto.h"
#include "../jrd/rlck_proto.h"
#include "../jrd/tra_proto.h"
#include "../jrd/scl_proto.h"
#include "../dsql/ddl_proto.h"
#include "../dsql/metd_proto.h"
#include "../jrd/vio_proto.h"
#include "../dsql/errd_proto.h"
#include "../dsql/gen_proto.h"
#include "../dsql/make_proto.h"
#include "../dsql/pass1_proto.h"

using namespace Firebird;
using namespace Jrd;


namespace Jrd {

template <typename T> static void dsqlExplodeFields(dsql_rel* relation, Array<NestConst<T> >& fields);
static dsql_par* dsqlFindDbKey(const dsql_req*, const RelationSourceNode*);
static dsql_par* dsqlFindRecordVersion(const dsql_req*, const RelationSourceNode*);
static const dsql_msg* dsqlGenDmlHeader(DsqlCompilerScratch*, RseNode*);
static dsql_ctx* dsqlGetContext(const RecordSourceNode* node);
static void dsqlGetContexts(DsqlContextStack& contexts, const RecordSourceNode* node);
static StmtNode* dsqlNullifyReturning(DsqlCompilerScratch*, StmtNode* input, bool returnList);
static void dsqlFieldAppearsOnce(const Array<NestConst<ValueExprNode> >& values, const char* command);
static ValueListNode* dsqlPassArray(DsqlCompilerScratch*, ValueListNode*);
static dsql_ctx* dsqlPassCursorContext(DsqlCompilerScratch*, const MetaName&, const RelationSourceNode*);
static RseNode* dsqlPassCursorReference(DsqlCompilerScratch*, const MetaName&, RelationSourceNode*);
static VariableNode* dsqlPassHiddenVariable(DsqlCompilerScratch* dsqlScratch, ValueExprNode* expr);
static USHORT dsqlPassLabel(DsqlCompilerScratch* dsqlScratch, bool breakContinue, MetaName* label);
static StmtNode* dsqlProcessReturning(DsqlCompilerScratch*, ReturningClause*, StmtNode*);
static void dsqlSetParameterName(DsqlCompilerScratch*, ExprNode*, const ValueExprNode*, const dsql_rel*);
static void dsqlSetParametersName(DsqlCompilerScratch*, CompoundStmtNode*, const RecordSourceNode*);

static void cleanupRpb(thread_db* tdbb, record_param* rpb);
static void makeValidation(thread_db* tdbb, CompilerScratch* csb, StreamType stream,
	Array<ValidateInfo>& validations);
static StmtNode* pass1ExpandView(thread_db* tdbb, CompilerScratch* csb, StreamType orgStream,
	StreamType newStream, bool remap);
static RelationSourceNode* pass1Update(thread_db* tdbb, CompilerScratch* csb, jrd_rel* relation,
	const TrigVector* trigger, StreamType stream, StreamType updateStream, SecurityClass::flags_t priv,
	jrd_rel* view, StreamType viewStream, StreamType viewUpdateStream);
static void pass1Validations(thread_db* tdbb, CompilerScratch* csb, Array<ValidateInfo>& validations);
static void postTriggerAccess(CompilerScratch* csb, jrd_rel* ownerRelation,
	ExternalAccess::exa_act operation, jrd_rel* view);
static void preModifyEraseTriggers(thread_db* tdbb, TrigVector** trigs,
	StmtNode::WhichTrigger whichTrig, record_param* rpb, record_param* rec, TriggerAction op);
static void preprocessAssignments(thread_db* tdbb, CompilerScratch* csb,
	StreamType stream, CompoundStmtNode* compoundNode, const Nullable<OverrideClause>* insertOverride);
static void validateExpressions(thread_db* tdbb, const Array<ValidateInfo>& validations);

}	// namespace Jrd


namespace
{
	// Node copier that remaps the field id 0 of stream 0 to a given field id.
	class RemapFieldNodeCopier : public NodeCopier
	{
	public:
		RemapFieldNodeCopier(CompilerScratch* aCsb, StreamType* aRemap, USHORT aFldId)
			: NodeCopier(aCsb->csb_pool, aCsb, aRemap),
			  fldId(aFldId)
		{
		}

	protected:
		virtual USHORT getFieldId(const FieldNode* field)
		{
			if (field->byId && field->fieldId == 0 && field->fieldStream == 0)
				return fldId;

			return NodeCopier::getFieldId(field);
		}

	private:
		USHORT fldId;
	};

	class ReturningProcessor
	{
	public:
		// Play with contexts for RETURNING purposes.
		// Its assumed that oldContext is already on the stack.
		// Changes oldContext name to "OLD".
		ReturningProcessor(DsqlCompilerScratch* aScratch, dsql_ctx* aOldContext, dsql_ctx* modContext)
			: scratch(aScratch),
			  oldContext(aOldContext),
			  oldAlias(oldContext->ctx_alias),
			  oldInternalAlias(oldContext->ctx_internal_alias),
			  autoFlags(&oldContext->ctx_flags, oldContext->ctx_flags | CTX_system | CTX_returning),
			  autoScopeLevel(&aScratch->scopeLevel, aScratch->scopeLevel + 1)
		{
			// Clone the modify/old context and push with name "NEW" in a greater scope level.

			dsql_ctx* newContext = FB_NEW_POOL(scratch->getPool()) dsql_ctx(scratch->getPool());

			if (modContext)
			{
				// Push the modify context in the same scope level.
				scratch->context->push(modContext);
				*newContext = *modContext;
				newContext->ctx_flags |= CTX_system;
			}
			else
			{
				// Create the target (= OLD) context and push it on the stack.
				dsql_ctx* targetContext = FB_NEW_POOL(scratch->getPool()) dsql_ctx(scratch->getPool());
				*targetContext = *oldContext;
				targetContext->ctx_flags &= ~CTX_system;	// resolve unqualified fields
				scratch->context->push(targetContext);

				// This is NEW in the context of a DELETE. Mark it as NULL.
				*newContext = *oldContext;
				newContext->ctx_flags |= CTX_null;
			}

			oldContext->ctx_alias = oldContext->ctx_internal_alias = OLD_CONTEXT_NAME;

			newContext->ctx_alias = newContext->ctx_internal_alias = NEW_CONTEXT_NAME;
			newContext->ctx_flags |= CTX_returning;
			newContext->ctx_scope_level = scratch->scopeLevel;
			scratch->context->push(newContext);
		}

		~ReturningProcessor()
		{
			oldContext->ctx_alias = oldAlias;
			oldContext->ctx_internal_alias = oldInternalAlias;

			// Restore the context stack.
			scratch->context->pop();
			scratch->context->pop();
		}

		// Process the RETURNING clause.
		StmtNode* process(ReturningClause* node, StmtNode* stmtNode)
		{
			return dsqlProcessReturning(scratch, node, stmtNode);
		}

		// Clone a RETURNING node without create duplicate parameters.
		static StmtNode* clone(DsqlCompilerScratch* scratch, ReturningClause* unprocessed, StmtNode* processed)
		{
			if (!processed)
				return NULL;

			// nod_returning was already processed
			CompoundStmtNode* processedStmt = nodeAs<CompoundStmtNode>(processed);
			fb_assert(processed);

			// And we create a RETURNING node where the targets are already processed.
			CompoundStmtNode* newNode =
				FB_NEW_POOL(scratch->getPool()) CompoundStmtNode(scratch->getPool());

			NestConst<ValueExprNode>* srcPtr = unprocessed->first->items.begin();
			NestConst<StmtNode>* dstPtr = processedStmt->statements.begin();

			for (const NestConst<ValueExprNode>* const end = unprocessed->first->items.end();
				 srcPtr != end;
				 ++srcPtr, ++dstPtr)
			{
				AssignmentNode* temp = FB_NEW_POOL(scratch->getPool()) AssignmentNode(scratch->getPool());
				temp->asgnFrom = *srcPtr;
				temp->asgnTo = nodeAs<AssignmentNode>(*dstPtr)->asgnTo;
				newNode->statements.add(temp);
			}

			return newNode;
		}

	private:
		DsqlCompilerScratch* scratch;
		dsql_ctx* oldContext;
		string oldAlias, oldInternalAlias;
		AutoSetRestore<USHORT> autoFlags;
		AutoSetRestore<USHORT> autoScopeLevel;
	};

	class SavepointChangeMarker : public Savepoint::ChangeMarker
	{
	public:
		explicit SavepointChangeMarker(jrd_tra* transaction)
			: Savepoint::ChangeMarker(transaction->tra_flags & TRA_system ?
									  NULL : transaction->tra_save_point)
		{}
	};

}	// namespace


//--------------------


namespace Jrd {


StmtNode* SavepointEncloseNode::make(MemoryPool& pool, DsqlCompilerScratch* dsqlScratch, StmtNode* node)
{
	if (dsqlScratch->errorHandlers)
	{
		node = FB_NEW_POOL(pool) SavepointEncloseNode(pool, node);
		node->dsqlPass(dsqlScratch);
	}

	return node;
}

string SavepointEncloseNode::internalPrint(NodePrinter& printer) const
{
	DsqlOnlyStmtNode::internalPrint(printer);

	NODE_PRINT(printer, stmt);

	return "SavepointEncloseNode";
}

void SavepointEncloseNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_begin);
	dsqlScratch->appendUChar(blr_start_savepoint);
	stmt->genBlr(dsqlScratch);
	dsqlScratch->appendUChar(blr_end_savepoint);
	dsqlScratch->appendUChar(blr_end);
}


//--------------------


static RegisterNode<AssignmentNode> regAssignmentNode(blr_assignment);

DmlNode* AssignmentNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	AssignmentNode* node = FB_NEW_POOL(pool) AssignmentNode(pool);
	node->asgnFrom = PAR_parse_value(tdbb, csb);
	node->asgnTo = PAR_parse_value(tdbb, csb);
	return node;
}

void AssignmentNode::validateTarget(CompilerScratch* csb, const ValueExprNode* target)
{
	const FieldNode* fieldNode;

	if ((fieldNode = nodeAs<FieldNode>(target)))
	{
		CompilerScratch::csb_repeat* tail = &csb->csb_rpt[fieldNode->fieldStream];

		// Assignments to the OLD context are prohibited for all trigger types.
		if ((tail->csb_flags & csb_trigger) && fieldNode->fieldStream == OLD_CONTEXT_VALUE)
			ERR_post(Arg::Gds(isc_read_only_field));

		// Assignments to the NEW context are prohibited for post-action triggers.
		if ((tail->csb_flags & csb_trigger) && fieldNode->fieldStream == NEW_CONTEXT_VALUE &&
			(csb->csb_g_flags & csb_post_trigger))
		{
			ERR_post(Arg::Gds(isc_read_only_field));
		}

		// Assignment to cursor fields are always prohibited.
		// But we cannot detect FOR cursors here. They are treated in dsqlPass.
		if (fieldNode->cursorNumber.specified)
			ERR_post(Arg::Gds(isc_read_only_field));
	}
	else if (!(nodeIs<ParameterNode>(target) || nodeIs<VariableNode>(target) || nodeIs<NullNode>(target)))
		ERR_post(Arg::Gds(isc_read_only_field));
}

void AssignmentNode::dsqlValidateTarget(const ValueExprNode* target)
{
	const DerivedFieldNode* fieldNode = nodeAs<DerivedFieldNode>(target);

	if (fieldNode && fieldNode->context &&
		(fieldNode->context->ctx_flags & (CTX_system | CTX_cursor)) == CTX_cursor)
	{
		ERR_post(Arg::Gds(isc_read_only_field));
	}
}

AssignmentNode* AssignmentNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	AssignmentNode* node = FB_NEW_POOL(dsqlScratch->getPool()) AssignmentNode(dsqlScratch->getPool());
	node->asgnFrom = doDsqlPass(dsqlScratch, asgnFrom);
	node->asgnTo = doDsqlPass(dsqlScratch, asgnTo);

	dsqlValidateTarget(node->asgnTo);

	// Try to force asgnFrom to be same type as asgnTo eg: ? = FIELD case
	PASS1_set_parameter_type(dsqlScratch, node->asgnFrom, node->asgnTo, false);

	// Try to force asgnTo to be same type as asgnFrom eg: FIELD = ? case
	// Try even when the above call succeeded, because "asgnTo" may
	// have sub-expressions that should be resolved.
	PASS1_set_parameter_type(dsqlScratch, node->asgnTo, node->asgnFrom, false);

	return node;
}

string AssignmentNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, asgnFrom);
	NODE_PRINT(printer, asgnTo);
	NODE_PRINT(printer, missing);
	NODE_PRINT(printer, missing2);

	return "AssignmentNode";
}

void AssignmentNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_assignment);
	GEN_expr(dsqlScratch, asgnFrom);
	GEN_expr(dsqlScratch, asgnTo);
}

AssignmentNode* AssignmentNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	AssignmentNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) AssignmentNode(*tdbb->getDefaultPool());
	node->asgnFrom = copier.copy(tdbb, asgnFrom);
	node->asgnTo = copier.copy(tdbb, asgnTo);
	node->missing = copier.copy(tdbb, missing);
	node->missing2 = copier.copy(tdbb, missing2);
	return node;
}

AssignmentNode* AssignmentNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	ValueExprNode* sub = asgnFrom;
	FieldNode* fieldNode;
	StreamType stream;
	CompilerScratch::csb_repeat* tail;

	if ((fieldNode = nodeAs<FieldNode>(sub)))
	{
		stream = fieldNode->fieldStream;
		jrd_fld* field = MET_get_field(csb->csb_rpt[stream].csb_relation, fieldNode->fieldId);

		if (field)
			missing2 = field->fld_missing_value;
	}

	sub = asgnTo;

	if ((fieldNode = nodeAs<FieldNode>(sub)))
	{
		stream = fieldNode->fieldStream;
		tail = &csb->csb_rpt[stream];
		jrd_fld* field = MET_get_field(tail->csb_relation, fieldNode->fieldId);

		if (field && field->fld_missing_value)
			missing = field->fld_missing_value;
	}

	doPass1(tdbb, csb, asgnFrom.getAddress());
	doPass1(tdbb, csb, asgnTo.getAddress());
	doPass1(tdbb, csb, missing.getAddress());
	// ASF: No idea why we do not call pass1 for missing2.

	return this;
}

AssignmentNode* AssignmentNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, asgnFrom.getAddress());
	ExprNode::doPass2(tdbb, csb, asgnTo.getAddress());
	ExprNode::doPass2(tdbb, csb, missing.getAddress());
	ExprNode::doPass2(tdbb, csb, missing2.getAddress());

	validateTarget(csb, asgnTo);

	return this;
}

const StmtNode* AssignmentNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		EXE_assignment(tdbb, this);
		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<BlockNode> regBlockNode(blr_block);

DmlNode* BlockNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	BlockNode* node = FB_NEW_POOL(pool) BlockNode(pool);
	node->action = PAR_parse_stmt(tdbb, csb);

	StmtNodeStack stack;

	while (csb->csb_blr_reader.peekByte() != blr_end)
		stack.push(PAR_parse_stmt(tdbb, csb));

	csb->csb_blr_reader.getByte();	// skip blr_end

	node->handlers = PAR_make_list(tdbb, stack);

	return node;
}

StmtNode* BlockNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (!handlers && !dsqlScratch->errorHandlers)
	{
		CompoundStmtNode* node = FB_NEW_POOL(dsqlScratch->getPool()) CompoundStmtNode(dsqlScratch->getPool());
		node->statements.add(action->dsqlPass(dsqlScratch));
		return node;
	}

	BlockNode* node = FB_NEW_POOL(dsqlScratch->getPool()) BlockNode(dsqlScratch->getPool());

	if (handlers)
		++dsqlScratch->errorHandlers;

	node->action = action->dsqlPass(dsqlScratch);

	if (handlers)
	{
		node->handlers = handlers->dsqlPass(dsqlScratch);
		--dsqlScratch->errorHandlers;
	}

	return node;
}

string BlockNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, action);
	NODE_PRINT(printer, handlers);

	return "BlockNode";
}

void BlockNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_block);
	action->genBlr(dsqlScratch);

	if (handlers)
	{
		const NestConst<StmtNode>* const end = handlers->statements.end();

		for (NestConst<StmtNode>* ptr = handlers->statements.begin(); ptr != end; ++ptr)
			(*ptr)->genBlr(dsqlScratch);
	}

	dsqlScratch->appendUChar(blr_end);
}

BlockNode* BlockNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, action.getAddress());
	doPass1(tdbb, csb, handlers.getAddress());
	return this;
}

BlockNode* BlockNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	doPass2(tdbb, csb, action.getAddress(), this);
	doPass2(tdbb, csb, handlers.getAddress(), this);

	impureOffset = CMP_impure(csb, sizeof(SavNumber));

	return this;
}

const StmtNode* BlockNode::execute(thread_db* tdbb, jrd_req* request, ExeState* exeState) const
{
	jrd_tra* transaction = request->req_transaction;
	SavNumber savNumber;

	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
			if (!(transaction->tra_flags & TRA_system))
			{
				const Savepoint* const savepoint = transaction->startSavepoint();
				savNumber = savepoint->getNumber();
				*request->getImpure<SavNumber>(impureOffset) = savNumber;
			}
			return action;

		case jrd_req::req_unwind:
		{
			if (request->req_flags & (req_leave | req_continue_loop))
			{
				// Although the req_operation is set to req_unwind,
				// it's not an error case if req_leave/req_continue_loop bit is set.
				// req_leave/req_continue_loop bit indicates that we hit an EXIT or
				// BREAK/LEAVE/CONTINUE statement in the SP/trigger code.
				// Do not perform the error handling stuff.

				if (!(transaction->tra_flags & TRA_system))
				{
					savNumber = *request->getImpure<SavNumber>(impureOffset);

					while (transaction->tra_save_point &&
						transaction->tra_save_point->getNumber() >= savNumber)
					{
						transaction->rollforwardSavepoint(tdbb);
					}
				}

				return parentStmt;
			}

			const StmtNode* temp = parentStmt;

			if (handlers && handlers->statements.hasData())
			{
				// First of all rollback failed work
				if (!(transaction->tra_flags & TRA_system))
				{
					savNumber = *request->getImpure<SavNumber>(impureOffset);

					// Since there occurred an error (req_unwind), undo all savepoints
					// up to, _but not including_, the savepoint of this block.
					// That's why transaction->rollbackToSavepoint() cannot be used here
					// The savepoint of this block will be dealt with below.
					// Do this only if error handlers exist. If not - leave rollbacking to caller node

					while (transaction->tra_save_point &&
						savNumber < transaction->tra_save_point->getNumber() &&
						transaction->tra_save_point->getNext() &&
						savNumber < transaction->tra_save_point->getNext()->getNumber())
					{
						transaction->rollforwardSavepoint(tdbb);
					}

					// There can be no savepoints above the given one

					if (transaction->tra_save_point && transaction->tra_save_point->getNumber() > savNumber)
						transaction->rollbackSavepoint(tdbb);

					// after that we still have to have our savepoint. If not - CORE-4424/4483 is sneaking around
					fb_assert(transaction->tra_save_point && transaction->tra_save_point->getNumber() == savNumber);
				}

				bool handled = false;
				const NestConst<StmtNode>* ptr = handlers->statements.begin();

				for (const NestConst<StmtNode>* const end = handlers->statements.end();
					 ptr != end;
					 ++ptr)
				{
					const ErrorHandlerNode* const handlerNode = nodeAs<ErrorHandlerNode>(*ptr);

					if (testAndFixupError(tdbb, request, handlerNode->conditions))
					{
						request->req_operation = jrd_req::req_evaluate;
						exeState->errorPending = false;

						// On entering looper exeState->oldRequest etc. are saved.
						// On recursive calling we will loose the actual old
						// request for that invocation of looper. Avoid this.

						{
							Jrd::ContextPoolHolder contextLooper(tdbb, exeState->oldPool);
							tdbb->setRequest(exeState->oldRequest);
							fb_assert(request->req_caller == exeState->oldRequest);
							request->req_caller = NULL;

							// Save the previous state of req_error_handler
							// bit. We need to restore it later. This is
							// necessary if the error handler is deeply nested.

							const ULONG prev_req_error_handler =
								request->req_flags & req_error_handler;
							request->req_flags |= req_error_handler;
							temp = EXE_looper(tdbb, request, handlerNode->action);
							request->req_flags &= ~req_error_handler;
							request->req_flags |= prev_req_error_handler;

							// Re-assign the transaction pointer, as the active transaction
							// could change in the meantime (inside the looper)
							transaction = request->req_transaction;

							// Note: Previously the above call "temp = looper (tdbb, request, temp);"
							// never returned back till the tree was executed completely. Now that
							// the looper has changed its behaviour such that it returns back after
							// handling error. This makes it necessary that the jmpbuf be reset
							// so that looper can proceede with the processing of execution tree.
							// If this is not done then anymore errors will take the engine out of
							// looper there by abruptly terminating the processing.

							exeState->catchDisabled = false;
							tdbb->setRequest(request);
							fb_assert(request->req_caller == NULL);
							request->req_caller = exeState->oldRequest;
							handled = true;
						}
					}
				}
				// The error is dealt with by the application, cleanup
				// this block's savepoint.

				if (handled && !(transaction->tra_flags & TRA_system))
				{
					// Check that exception handlers were executed in context of right savepoint.
					// If not - mirror copy of CORE-4424 or CORE-4483 is around here.
					// Except the case of nested exception handlers and throwing in inner one.
					// In this case execution flow is like this:
					//		inner before (block that rollbacking savepoints above)
					//		outer before
					//		outer after  (this block)
					//		inner after
					// Because of this following assert is commented out
					//fb_assert(transaction->tra_save_point && transaction->tra_save_point->getNumber() == savNumber);

					for (const Savepoint* save_point = transaction->tra_save_point;
							save_point && savNumber <= save_point->getNumber();
							save_point = transaction->tra_save_point)
					{
						transaction->rollforwardSavepoint(tdbb);
					}
				}
			}

			// If the application didn't have an error handler, then
			// the error will still be pending. Leave undo to outer blocks.

			return temp;
		}

		case jrd_req::req_return:
			if (!(transaction->tra_flags & TRA_system))
			{
				savNumber = *request->getImpure<SavNumber>(impureOffset);

				// rollforward all savepoints
				for (const Savepoint* save_point = transaction->tra_save_point;
					 save_point && save_point->getNext() && savNumber <= save_point->getNumber();
					 save_point = transaction->tra_save_point)
				{
					transaction->rollforwardSavepoint(tdbb);
				}
			}

		default:
			return parentStmt;
	}

	fb_assert(false);
	return NULL;
}

// Test for match of current state with list of error conditions. Fix type and code of the exception.
bool BlockNode::testAndFixupError(thread_db* tdbb, jrd_req* request, const ExceptionArray& conditions)
{
	if (tdbb->tdbb_flags & TDBB_sys_error)
		return false;

	Jrd::FbStatusVector* statusVector = tdbb->tdbb_status_vector;

	bool found = false;

	for (USHORT i = 0; i < conditions.getCount(); i++)
	{
		switch (conditions[i].type)
		{
			case ExceptionItem::SQL_CODE:
				{
					const SSHORT sqlcode = gds__sqlcode(statusVector->getErrors());
					if (sqlcode == conditions[i].code)
						found = true;
				}
				break;

			case ExceptionItem::SQL_STATE:
				{
					FB_SQLSTATE_STRING sqlstate;
					fb_sqlstate(sqlstate, statusVector->getErrors());
					if (conditions[i].name == sqlstate)
						found = true;
				}
				break;

			case ExceptionItem::GDS_CODE:
				if (statusVector->getErrors()[1] == conditions[i].code)
					found = true;
				break;

			case ExceptionItem::XCP_CODE:
				// Look at set_error() routine to understand how the
				// exception ID info is encoded inside the status vector.
				if ((statusVector->getErrors()[1] == isc_except) &&
					(statusVector->getErrors()[3] == conditions[i].code))
				{
					found = true;
				}

				break;

			case ExceptionItem::XCP_DEFAULT:
				found = true;
				break;

			default:
				fb_assert(false);
		}

		if (found)
		{
			request->req_last_xcp.init(statusVector);
			fb_utils::init_status(statusVector);
			break;
		}
    }

	return found;
}


//--------------------


static RegisterNode<CompoundStmtNode> regCompoundStmtNode(blr_begin);

DmlNode* CompoundStmtNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	CompoundStmtNode* node = FB_NEW_POOL(pool) CompoundStmtNode(pool);

	if (csb->csb_currentForNode)
		csb->csb_currentForNode->parBlrBeginCnt++;

	while (csb->csb_blr_reader.peekByte() != blr_end)
		node->statements.add(PAR_parse_stmt(tdbb, csb));

	csb->csb_blr_reader.getByte();	// skip blr_end

	return node;
}

CompoundStmtNode* CompoundStmtNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (++dsqlScratch->nestingLevel > DsqlCompilerScratch::MAX_NESTING)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
				  Arg::Gds(isc_imp_exc) <<
				  Arg::Gds(isc_dsql_max_nesting) << Arg::Num(DsqlCompilerScratch::MAX_NESTING));
	}

	CompoundStmtNode* node = FB_NEW_POOL(dsqlScratch->getPool()) CompoundStmtNode(dsqlScratch->getPool());

	for (NestConst<StmtNode>* i = statements.begin(); i != statements.end(); ++i)
	{
		StmtNode* ptr = *i;
		ptr = ptr->dsqlPass(dsqlScratch);
		node->statements.add(ptr);
	}

	--dsqlScratch->nestingLevel;

	return node;
}

string CompoundStmtNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, statements);
	NODE_PRINT(printer, onlyAssignments);

	return "CompoundStmtNode";
}

void CompoundStmtNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_begin);

	for (NestConst<StmtNode>* i = statements.begin(); i != statements.end(); ++i)
		(*i)->genBlr(dsqlScratch);

	dsqlScratch->appendUChar(blr_end);
}

CompoundStmtNode* CompoundStmtNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	CompoundStmtNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) CompoundStmtNode(*tdbb->getDefaultPool());
	node->onlyAssignments = onlyAssignments;

	NestConst<StmtNode>* j = node->statements.getBuffer(statements.getCount());

	for (const NestConst<StmtNode>* i = statements.begin(); i != statements.end(); ++i, ++j)
		*j = copier.copy(tdbb, *i);

	return node;
}

CompoundStmtNode* CompoundStmtNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	for (NestConst<StmtNode>* i = statements.begin(); i != statements.end(); ++i)
		doPass1(tdbb, csb, i->getAddress());

	return this;
}

CompoundStmtNode* CompoundStmtNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	for (NestConst<StmtNode>* i = statements.begin(); i != statements.end(); ++i)
		doPass2(tdbb, csb, i->getAddress(), this);

	impureOffset = CMP_impure(csb, sizeof(impure_state));

	for (NestConst<StmtNode>* i = statements.begin(); i != statements.end(); ++i)
	{
		if (!StmtNode::is<AssignmentNode>(i->getObject()))
			return this;
	}

	onlyAssignments = true;

	return this;
}

const StmtNode* CompoundStmtNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	const NestConst<StmtNode>* end = statements.end();

	if (onlyAssignments)
	{
		if (request->req_operation == jrd_req::req_evaluate)
		{
			for (const NestConst<StmtNode>* i = statements.begin(); i != end; ++i)
			{
				const StmtNode* stmt = i->getObject();

				if (stmt->hasLineColumn)
				{
					request->req_src_line = stmt->line;
					request->req_src_column = stmt->column;
				}

				EXE_assignment(tdbb, static_cast<const AssignmentNode*>(stmt));
			}

			request->req_operation = jrd_req::req_return;
		}

		return parentStmt;
	}

	impure_state* impure = request->getImpure<impure_state>(impureOffset);

	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
			impure->sta_state = 0;
			// fall into

		case jrd_req::req_return:
		case jrd_req::req_sync:
			if (impure->sta_state < statements.getCount())
			{
				request->req_operation = jrd_req::req_evaluate;
				return statements[impure->sta_state++];
			}
			request->req_operation = jrd_req::req_return;
			// fall into

		default:
			return parentStmt;
	}
}


//--------------------


static RegisterNode<ContinueLeaveNode> regContinueLeaveNodeContinue(blr_continue_loop);
static RegisterNode<ContinueLeaveNode> regContinueLeaveNodeLeave(blr_leave);

DmlNode* ContinueLeaveNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	ContinueLeaveNode* node = FB_NEW_POOL(pool) ContinueLeaveNode(pool, blrOp);
	node->labelNumber = csb->csb_blr_reader.getByte();
	return node;
}

ContinueLeaveNode* ContinueLeaveNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	const char* cmd = blrOp == blr_continue_loop ? "CONTINUE" : "BREAK/LEAVE";

	if (!dsqlScratch->loopLevel)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			// Token unknown
			Arg::Gds(isc_token_err) <<
			Arg::Gds(isc_random) << cmd);
	}

	labelNumber = dsqlPassLabel(dsqlScratch, true, dsqlLabelName);

	return this;
}

string ContinueLeaveNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, blrOp);
	NODE_PRINT(printer, labelNumber);
	NODE_PRINT(printer, dsqlLabelName);

	return "ContinueLeaveNode";
}

void ContinueLeaveNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blrOp);
	dsqlScratch->appendUChar(labelNumber);
}

const StmtNode* ContinueLeaveNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		request->req_operation = jrd_req::req_unwind;
		request->req_label = labelNumber;
		request->req_flags |= (blrOp == blr_continue_loop ? req_continue_loop : req_leave);
	}
	return parentStmt;
}


//--------------------


static RegisterNode<CursorStmtNode> regCursorStmtNode(blr_cursor_stmt);

DmlNode* CursorStmtNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	CursorStmtNode* node = FB_NEW_POOL(pool) CursorStmtNode(pool, csb->csb_blr_reader.getByte());
	node->cursorNumber = csb->csb_blr_reader.getWord();

	switch (node->cursorOp)
	{
		case blr_cursor_open:
		case blr_cursor_close:
			break;

		case blr_cursor_fetch_scroll:
			node->scrollOp = csb->csb_blr_reader.getByte();
			node->scrollExpr = PAR_parse_value(tdbb, csb);
			// fall into

		case blr_cursor_fetch:
			csb->csb_g_flags |= csb_reuse_context;
			node->intoStmt = PAR_parse_stmt(tdbb, csb);
			csb->csb_g_flags &= ~csb_reuse_context;
			break;

		default:
			PAR_syntax_error(csb, "cursor operation clause");
	}

	return node;
}

CursorStmtNode* CursorStmtNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	// Verify if we're in an autonomous transaction.
	if (dsqlScratch->flags & DsqlCompilerScratch::FLAG_IN_AUTO_TRANS_BLOCK)
	{
		const char* stmt = NULL;

		switch (cursorOp)
		{
			case blr_cursor_open:
				stmt = "OPEN CURSOR";
				break;

			case blr_cursor_close:
				stmt = "CLOSE CURSOR";
				break;

			case blr_cursor_fetch:
			case blr_cursor_fetch_scroll:
				stmt = "FETCH CURSOR";
				break;
		}

		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
				  Arg::Gds(isc_dsql_unsupported_in_auto_trans) << Arg::Str(stmt));
	}

	// Resolve the cursor.
	cursorNumber = PASS1_cursor_name(dsqlScratch, dsqlName,
		DeclareCursorNode::CUR_TYPE_EXPLICIT, true)->cursorNumber;

	// Process a scroll node, if exists.
	if (scrollExpr)
		scrollExpr = doDsqlPass(dsqlScratch, scrollExpr);

	// Process an assignment node, if exists.
	dsqlIntoStmt = dsqlPassArray(dsqlScratch, dsqlIntoStmt);

	return this;
}

string CursorStmtNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlName);
	NODE_PRINT(printer, dsqlIntoStmt);
	NODE_PRINT(printer, cursorOp);
	NODE_PRINT(printer, cursorNumber);
	NODE_PRINT(printer, scrollOp);
	NODE_PRINT(printer, scrollExpr);
	NODE_PRINT(printer, intoStmt);

	return "CursorStmtNode";
}

void CursorStmtNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_cursor_stmt);
	dsqlScratch->appendUChar(cursorOp);	// open, close, fetch [scroll]
	dsqlScratch->appendUShort(cursorNumber);

	if (cursorOp == blr_cursor_fetch_scroll)
	{
		dsqlScratch->appendUChar(scrollOp);

		if (scrollExpr)
			GEN_expr(dsqlScratch, scrollExpr);
		else
			dsqlScratch->appendUChar(blr_null);
	}

	DeclareCursorNode* cursor = NULL;

	for (Array<DeclareCursorNode*>::iterator itr = dsqlScratch->cursors.begin();
		 itr != dsqlScratch->cursors.end();
		 ++itr)
	{
		if ((*itr)->cursorNumber == cursorNumber)
			cursor = *itr;
	}

	fb_assert(cursor);

	// Assignment.

	if (cursorOp == blr_cursor_fetch || cursorOp == blr_cursor_fetch_scroll)
		dsqlScratch->appendUChar(blr_begin);

	if (dsqlIntoStmt)
	{
		ValueListNode* list = cursor->rse->dsqlSelectList;

		if (list->items.getCount() != dsqlIntoStmt->items.getCount())
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-313) <<
					  Arg::Gds(isc_dsql_count_mismatch));
		}

		NestConst<ValueExprNode>* ptr = list->items.begin();
		NestConst<ValueExprNode>* end = list->items.end();
		NestConst<ValueExprNode>* ptr_to = dsqlIntoStmt->items.begin();

		dsqlScratch->flags |= DsqlCompilerScratch::FLAG_FETCH;

		while (ptr != end)
		{
			dsqlScratch->appendUChar(blr_assignment);
			GEN_expr(dsqlScratch, *ptr++);
			GEN_expr(dsqlScratch, *ptr_to++);
		}

		dsqlScratch->flags &= ~DsqlCompilerScratch::FLAG_FETCH;
	}

	if (cursorOp == blr_cursor_fetch || cursorOp == blr_cursor_fetch_scroll)
		dsqlScratch->appendUChar(blr_end);
}

CursorStmtNode* CursorStmtNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, scrollExpr.getAddress());
	doPass1(tdbb, csb, intoStmt.getAddress());
	return this;
}

CursorStmtNode* CursorStmtNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, scrollExpr.getAddress());
	doPass2(tdbb, csb, intoStmt.getAddress(), this);
	return this;
}

const StmtNode* CursorStmtNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	fb_assert(cursorNumber < request->req_cursors.getCount());
	const Cursor* const cursor = request->req_cursors[cursorNumber];
	bool fetched = false;

	switch (cursorOp)
	{
		case blr_cursor_open:
			if (request->req_operation == jrd_req::req_evaluate)
			{
				cursor->open(tdbb);
				request->req_operation = jrd_req::req_return;
			}
			return parentStmt;

		case blr_cursor_close:
			if (request->req_operation == jrd_req::req_evaluate)
			{
				cursor->close(tdbb);
				request->req_operation = jrd_req::req_return;
			}
			return parentStmt;

		case blr_cursor_fetch:
		case blr_cursor_fetch_scroll:
			switch (request->req_operation)
			{
				case jrd_req::req_evaluate:
					request->req_records_affected.clear();

					if (cursorOp == blr_cursor_fetch)
						fetched = cursor->fetchNext(tdbb);
					else
					{
						fb_assert(cursorOp == blr_cursor_fetch_scroll);

						const dsc* desc = EVL_expr(tdbb, request, scrollExpr);
						const bool unknown = !desc || (request->req_flags & req_null);
						const SINT64 offset = unknown ? 0 : MOV_get_int64(tdbb, desc, 0);

						switch (scrollOp)
						{
							case blr_scroll_forward:
								fetched = cursor->fetchNext(tdbb);
								break;
							case blr_scroll_backward:
								fetched = cursor->fetchPrior(tdbb);
								break;
							case blr_scroll_bof:
								fetched = cursor->fetchFirst(tdbb);
								break;
							case blr_scroll_eof:
								fetched = cursor->fetchLast(tdbb);
								break;
							case blr_scroll_absolute:
								fetched = unknown ? false : cursor->fetchAbsolute(tdbb, offset);
								break;
							case blr_scroll_relative:
								fetched = unknown ? false : cursor->fetchRelative(tdbb, offset);
								break;
							default:
								fb_assert(false);
								fetched = false;
						}
					}

					if (fetched)
					{
						request->req_operation = jrd_req::req_evaluate;
						return intoStmt;
					}

					request->req_operation = jrd_req::req_return;

				default:
					return parentStmt;
			}
			break;
	}

	fb_assert(false);
	return NULL;
}


//--------------------


static RegisterNode<DeclareCursorNode> regDeclareCursorNode(blr_dcl_cursor);

DmlNode* DeclareCursorNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	DeclareCursorNode* node = FB_NEW_POOL(pool) DeclareCursorNode(pool);

	fb_assert(blrOp == blr_dcl_cursor);
	if (blrOp == blr_dcl_cursor)
		node->dsqlCursorType = CUR_TYPE_EXPLICIT;

	node->cursorNumber = csb->csb_blr_reader.getWord();
	node->rse = PAR_rse(tdbb, csb);

	USHORT count = csb->csb_blr_reader.getWord();
	node->refs = PAR_args(tdbb, csb, count, count);

	return node;
}

DeclareCursorNode* DeclareCursorNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	fb_assert(dsqlCursorType != CUR_TYPE_NONE);

	// Make sure the cursor doesn't exist.
	PASS1_cursor_name(dsqlScratch, dsqlName, CUR_TYPE_ALL, false);

	SelectExprNode* dt = FB_NEW_POOL(dsqlScratch->getPool()) SelectExprNode(dsqlScratch->getPool());
	dt->dsqlFlags = RecordSourceNode::DFLAG_DERIVED | RecordSourceNode::DFLAG_CURSOR;
	dt->querySpec = dsqlSelect->dsqlExpr;
	dt->alias = dsqlName.c_str();

	rse = PASS1_derived_table(dsqlScratch, dt, NULL, dsqlSelect->dsqlWithLock);

	// Assign number and store in the dsqlScratch stack.
	cursorNumber = dsqlScratch->cursorNumber++;
	dsqlScratch->cursors.push(this);

	dsqlScratch->putDebugCursor(cursorNumber, dsqlName);

	++dsqlScratch->scopeLevel;

	return this;
}

string DeclareCursorNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlCursorType);
	NODE_PRINT(printer, dsqlScroll);
	NODE_PRINT(printer, dsqlName);
	NODE_PRINT(printer, dsqlSelect);
	NODE_PRINT(printer, rse);
	NODE_PRINT(printer, refs);
	NODE_PRINT(printer, cursorNumber);
	NODE_PRINT(printer, cursor);

	return "DeclareCursorNode";
}

void DeclareCursorNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_dcl_cursor);
	dsqlScratch->appendUShort(cursorNumber);

	if (dsqlScroll)
		dsqlScratch->appendUChar(blr_scrollable);

	GEN_rse(dsqlScratch, rse);

	ValueListNode* temp = rse->dsqlSelectList;
	NestConst<ValueExprNode>* ptr = temp->items.begin();
	NestConst<ValueExprNode>* end = temp->items.end();

	fb_assert(temp->items.getCount() < MAX_USHORT);
	dsqlScratch->appendUShort(temp->items.getCount());

	while (ptr < end)
		GEN_expr(dsqlScratch, *ptr++);
}

DeclareCursorNode* DeclareCursorNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, rse.getAddress());
	doPass1(tdbb, csb, refs.getAddress());
	return this;
}

DeclareCursorNode* DeclareCursorNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	rse->pass2Rse(tdbb, csb);

	ExprNode::doPass2(tdbb, csb, rse.getAddress());
	ExprNode::doPass2(tdbb, csb, refs.getAddress());

	// Finish up processing of record selection expressions.

	RecordSource* const rsb = CMP_post_rse(tdbb, csb, rse.getObject());
	csb->csb_fors.add(rsb);

	cursor = FB_NEW_POOL(*tdbb->getDefaultPool()) Cursor(csb, rsb, rse->rse_invariants,
		(rse->flags & RseNode::FLAG_SCROLLABLE));
	csb->csb_dbg_info->curIndexToName.get(cursorNumber, cursor->name);

	if (cursorNumber >= csb->csb_cursors.getCount())
		csb->csb_cursors.grow(cursorNumber + 1);

	csb->csb_cursors[cursorNumber] = cursor;

	StreamList cursorStreams;
	cursor->getAccessPath()->findUsedStreams(cursorStreams);

	// Activate cursor streams to allow index usage for <cursor>.<field> references, see CORE-4675.
	// It's also useful for correlated sub-queries in the select list, see CORE-4379.
	// Mark cursor streams as unstable, see CORE-5773.

	for (StreamList::const_iterator i = cursorStreams.begin(); i != cursorStreams.end(); ++i)
	{
		csb->csb_rpt[*i].csb_cursor_number = cursorNumber;
		csb->csb_rpt[*i].activate();
		if (dsqlCursorType == CUR_TYPE_EXPLICIT)
			csb->csb_rpt[*i].csb_flags |= csb_unstable;
	}

	return this;
}

const StmtNode* DeclareCursorNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		// Set up the cursors array...
		if (cursorNumber >= request->req_cursors.getCount())
			request->req_cursors.grow(cursorNumber + 1);

		// And store cursor there.
		request->req_cursors[cursorNumber] = cursor;
		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<DeclareSubFuncNode> regDeclareSubFuncNode(blr_subfunc_decl);

DmlNode* DeclareSubFuncNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	const UCHAR /*blrOp*/)
{
	MetaName name;
	csb->csb_blr_reader.getMetaName(name);

	if (csb->csb_g_flags & csb_subroutine)
		PAR_error(csb, Arg::Gds(isc_wish_list) << Arg::Gds(isc_random) << "nested sub function");

	if (csb->subFunctions.exist(name))
		PAR_error(csb, Arg::Gds(isc_random) << "duplicate sub function");

	DeclareSubFuncNode* node = FB_NEW_POOL(pool) DeclareSubFuncNode(pool, name);

	Function* subFunc = node->routine = FB_NEW_POOL(pool) Function(pool);
	subFunc->setName(QualifiedName(name));
	subFunc->setSubRoutine(true);
	subFunc->setImplemented(true);

	{	// scope
		CompilerScratch* const subCsb = node->subCsb =
			FB_NEW_POOL(csb->csb_pool) CompilerScratch(csb->csb_pool, csb);

		subCsb->csb_g_flags |= csb_subroutine | (csb->csb_g_flags & csb_get_dependencies);
		subCsb->csb_blr_reader = csb->csb_blr_reader;

		BlrReader& reader = subCsb->csb_blr_reader;
		ContextPoolHolder context(tdbb, &subCsb->csb_pool);

		UCHAR type = reader.getByte();
		if (type != SUB_ROUTINE_TYPE_PSQL)
			PAR_syntax_error(csb, "sub function type");

		UCHAR deterministic = reader.getByte();
		if (deterministic != 0 && deterministic != 1)
			PAR_syntax_error(csb, "sub function deterministic");

		subFunc->fun_deterministic = deterministic == 1;

		USHORT defaultCount = 0;
		parseParameters(tdbb, pool, subCsb, subFunc->getInputFields(), &defaultCount);
		subFunc->setDefaultCount(defaultCount);

		parseParameters(tdbb, pool, subCsb, subFunc->getOutputFields());

		subFunc->fun_inputs = subFunc->getInputFields().getCount();

		node->blrLength = reader.getLong();
		node->blrStart = reader.getPos();

		subFunc->parseMessages(tdbb, subCsb, BlrReader(reader.getPos(), node->blrLength));

		USHORT count = subFunc->getInputFormat() ? subFunc->getInputFormat()->fmt_count : 0;
		if (subFunc->getInputFields().getCount() * 2 != count)
			PAR_error(csb, Arg::Gds(isc_fun_param_mismatch) << name);

		for (USHORT i = 0; i < count; i += 2u)
		{
			Parameter* parameter = subFunc->getInputFields()[i / 2u];
			parameter->prm_desc = subFunc->getInputFormat()->fmt_desc[i];
		}

		Array<NestConst<Parameter> >& paramArray = subFunc->getOutputFields();

		count = subFunc->getOutputFormat() ? subFunc->getOutputFormat()->fmt_count : 0;
		if (count == 0 || paramArray.getCount() * 2 != count - 1u)
			PAR_error(csb, Arg::Gds(isc_prc_out_param_mismatch) << name);

		for (USHORT i = 0; i < count - 1u; i += 2u)
		{
			Parameter* parameter = paramArray[i / 2u];
			parameter->prm_desc = subFunc->getOutputFormat()->fmt_desc[i];
		}

		DbgInfo* subDbgInfo = NULL;
		if (csb->csb_dbg_info->subFuncs.get(name, subDbgInfo))
		{
			subCsb->csb_dbg_info = subDbgInfo;
			csb->csb_dbg_info->subFuncs.remove(name);
		}
	}

	csb->subFunctions.put(name, node);
	csb->csb_blr_reader.setPos(node->blrStart + node->blrLength);

	return node;
}

void DeclareSubFuncNode::parseParameters(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	Firebird::Array<NestConst<Parameter> >& paramArray, USHORT* defaultCount)
{
	BlrReader& reader = csb->csb_blr_reader;
	USHORT count = reader.getWord();
	FB_SIZE_T pos = paramArray.getCount();
	paramArray.resize(pos + count);

	if (defaultCount)
		*defaultCount = 0;

	for (FB_SIZE_T i = 0; i < count; ++i)
	{
		Parameter* parameter = FB_NEW_POOL(pool) Parameter(pool);
		parameter->prm_number = USHORT(i);
		parameter->prm_fun_mechanism = FUN_value;
		paramArray[pos + parameter->prm_number] = parameter;

		csb->csb_blr_reader.getMetaName(parameter->prm_name);

		UCHAR hasDefault = reader.getByte();

		if (hasDefault == 1)
		{
			if (defaultCount && *defaultCount == 0)
				*defaultCount = paramArray.getCount() - i;

			parameter->prm_default_value = PAR_parse_value(tdbb, csb);
		}
		else if (hasDefault != 0)
			PAR_syntax_error(csb, "0 or 1");
	}
}

string DeclareSubFuncNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, name);
	NODE_PRINT(printer, dsqlDeterministic);
	NODE_PRINT(printer, dsqlBlock);

	return "DeclareSubFuncNode";
}

DeclareSubFuncNode* DeclareSubFuncNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	MemoryPool& pool = dsqlScratch->getPool();

	if (dsqlScratch->flags & DsqlCompilerScratch::FLAG_SUB_ROUTINE)
		ERR_post(Arg::Gds(isc_wish_list) << Arg::Gds(isc_random) << "nested sub function");

	DeclareSubFuncNode* prevDecl = dsqlScratch->getSubFunction(name);
	bool implemetingForward = prevDecl && !prevDecl->dsqlBlock && dsqlBlock;

	dsqlFunction = implemetingForward ? prevDecl->dsqlFunction : FB_NEW_POOL(pool) dsql_udf(pool);

	dsqlFunction->udf_flags = UDF_subfunc;
	dsqlFunction->udf_name.identifier = name;

	fb_assert(dsqlReturns.getCount() == 1);
	const TypeClause* returnType = dsqlReturns[0]->type;

	dsqlFunction->udf_dtype = returnType->dtype;
	dsqlFunction->udf_scale = returnType->scale;
	dsqlFunction->udf_sub_type = returnType->subType;
	dsqlFunction->udf_length = returnType->length;
	dsqlFunction->udf_character_set_id = returnType->charSetId;

	if (dsqlDeterministic)
		dsqlSignature.flags |= Signature::FLAG_DETERMINISTIC;

	SignatureParameter sigRet(pool);
	sigRet.type = 1;
	sigRet.number = -1;
	sigRet.fromType(returnType);
	dsqlSignature.parameters.add(sigRet);

	Array<NestConst<ParameterClause> >& paramArray = dsqlParameters;
	bool defaultFound = false;

	for (NestConst<ParameterClause>* i = paramArray.begin(); i != paramArray.end(); ++i)
	{
		ParameterClause* param = *i;
		const unsigned paramIndex = i - paramArray.begin();

		SignatureParameter sigParam(pool);
		sigParam.type = 0;
		sigParam.number = (SSHORT) dsqlSignature.parameters.getCount();
		sigParam.name = param->name;
		sigParam.fromType(param->type);
		dsqlSignature.parameters.add(sigParam);

		if (!implemetingForward)
		{
			// ASF: dsqlFunction->udf_arguments is only checked for its count for now.
			dsqlFunction->udf_arguments.add(dsc());
		}

		if (param->defaultClause)
		{
			if (prevDecl)
			{
				status_exception::raise(
					Arg::Gds(isc_subfunc_defvaldecl) <<
					name.c_str());
			}

			defaultFound = true;

			if (!implemetingForward && dsqlFunction->udf_def_count == 0)
				dsqlFunction->udf_def_count = paramArray.end() - i;
		}
		else
		{
			if (defaultFound)
			{
				// Parameter without default value after parameters with default.
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
						Arg::Gds(isc_bad_default_value) <<
						Arg::Gds(isc_invalid_clause) << Arg::Str("defaults must be last"));
			}

			if (prevDecl && paramIndex < prevDecl->dsqlParameters.getCount())
				param->defaultClause = prevDecl->dsqlParameters[paramIndex]->defaultClause;
		}
	}

	if (!implemetingForward)
		dsqlScratch->putSubFunction(this);
	else if (dsqlSignature != prevDecl->dsqlSignature)
	{
		status_exception::raise(
			Arg::Gds(isc_subfunc_signat) <<
			name.c_str());
	}

	if (!dsqlBlock)	// forward decl
		return this;

	if (prevDecl)
		dsqlScratch->putSubFunction(this, true);

	DsqlCompiledStatement* statement = FB_NEW_POOL(pool) DsqlCompiledStatement(pool);

	if (dsqlScratch->clientDialect > SQL_DIALECT_V5)
		statement->setBlrVersion(5);
	else
		statement->setBlrVersion(4);

	statement->setSendMsg(FB_NEW_POOL(pool) dsql_msg(pool));
	dsql_msg* message = FB_NEW_POOL(pool) dsql_msg(pool);
	statement->setReceiveMsg(message);
	message->msg_number = 1;

	statement->setType(DsqlCompiledStatement::TYPE_SELECT);

	blockScratch = FB_NEW_POOL(pool) DsqlCompilerScratch(pool,
		dsqlScratch->getAttachment(), dsqlScratch->getTransaction(), statement, dsqlScratch);
	blockScratch->clientDialect = dsqlScratch->clientDialect;
	blockScratch->flags |=
		DsqlCompilerScratch::FLAG_FUNCTION |
		DsqlCompilerScratch::FLAG_SUB_ROUTINE |
		(dsqlScratch->flags & DsqlCompilerScratch::FLAG_DDL);

	dsqlBlock = dsqlBlock->dsqlPass(blockScratch);

	return this;
}

void DeclareSubFuncNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (!dsqlBlock)	// forward decl
		return;

	GEN_request(blockScratch, dsqlBlock);

	dsqlScratch->appendUChar(blr_subfunc_decl);
	dsqlScratch->appendNullString(name.c_str());

	dsqlScratch->appendUChar(SUB_ROUTINE_TYPE_PSQL);
	dsqlScratch->appendUChar(dsqlDeterministic ? 1 : 0);

	genParameters(dsqlScratch, dsqlBlock->parameters);
	genParameters(dsqlScratch, dsqlBlock->returns);

	BlrDebugWriter::BlrData& blrData = blockScratch->getBlrData();
	dsqlScratch->appendULong(ULONG(blrData.getCount()));
	dsqlScratch->appendBytes(blrData.begin(), blrData.getCount());

	dsqlScratch->putDebugSubFunction(this);
}

void DeclareSubFuncNode::genParameters(DsqlCompilerScratch* dsqlScratch,
	Array<NestConst<ParameterClause> >& paramArray)
{
	dsqlScratch->appendUShort(USHORT(paramArray.getCount()));

	for (NestConst<ParameterClause>* i = paramArray.begin(); i != paramArray.end(); ++i)
	{
		ParameterClause* param = *i;
		dsqlScratch->appendNullString(param->name.c_str());

		if (param->defaultClause)
		{
			dsqlScratch->appendUChar(1);
			GEN_expr(dsqlScratch, param->defaultClause->value);
		}
		else
			dsqlScratch->appendUChar(0);
	}
}

DeclareSubFuncNode* DeclareSubFuncNode::pass1(thread_db* /*tdbb*/, CompilerScratch* /*csb*/)
{
	return this;
}

DeclareSubFuncNode* DeclareSubFuncNode::pass2(thread_db* tdbb, CompilerScratch* /*csb*/)
{
	// scope needed here?
	{	// scope
		ContextPoolHolder context(tdbb, &subCsb->csb_pool);
		PAR_blr(tdbb, NULL, blrStart, blrLength, NULL, &subCsb, NULL, false, 0);
	}

	return this;
}

const StmtNode* DeclareSubFuncNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	// Nothing to execute. This is the declaration node.

	if (request->req_operation == jrd_req::req_evaluate)
		request->req_operation = jrd_req::req_return;

	return parentStmt;
}


//--------------------


static RegisterNode<DeclareSubProcNode> regDeclareSubProcNode(blr_subproc_decl);

DmlNode* DeclareSubProcNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	MetaName name;
	csb->csb_blr_reader.getMetaName(name);

	if (csb->csb_g_flags & csb_subroutine)
		PAR_error(csb, Arg::Gds(isc_wish_list) << Arg::Gds(isc_random) << "nested sub procedure");

	if (csb->subProcedures.exist(name))
		PAR_error(csb, Arg::Gds(isc_random) << "duplicate sub procedure");

	DeclareSubProcNode* node = FB_NEW_POOL(pool) DeclareSubProcNode(pool, name);

	jrd_prc* subProc = node->routine = FB_NEW_POOL(pool) jrd_prc(pool);
	subProc->setName(QualifiedName(name));
	subProc->setSubRoutine(true);
	subProc->setImplemented(true);

	{	// scope
		CompilerScratch* const subCsb = node->subCsb =
			FB_NEW_POOL(csb->csb_pool) CompilerScratch(csb->csb_pool, csb);

		subCsb->csb_g_flags |= csb_subroutine | (csb->csb_g_flags & csb_get_dependencies);
		subCsb->csb_blr_reader = csb->csb_blr_reader;

		BlrReader& reader = subCsb->csb_blr_reader;
		ContextPoolHolder context(tdbb, &subCsb->csb_pool);

		UCHAR type = reader.getByte();
		if (type != SUB_ROUTINE_TYPE_PSQL)
			PAR_syntax_error(csb, "sub routine type");

		type = reader.getByte();
		if (type != 0 && type != 1)
			PAR_syntax_error(csb, "sub procedure type");

		subProc->prc_type = type == 1 ? prc_selectable : prc_executable;

		USHORT defaultCount = 0;
		parseParameters(tdbb, pool, subCsb, subProc->getInputFields(), &defaultCount);
		subProc->setDefaultCount(defaultCount);

		parseParameters(tdbb, pool, subCsb, subProc->getOutputFields());

		node->blrLength = reader.getLong();
		node->blrStart = reader.getPos();

		subProc->parseMessages(tdbb, subCsb, BlrReader(reader.getPos(), node->blrLength));

		USHORT count = subProc->getInputFormat() ? subProc->getInputFormat()->fmt_count : 0;
		if (subProc->getInputFields().getCount() * 2 != count)
			PAR_error(csb, Arg::Gds(isc_prcmismat) << name);

		for (USHORT i = 0; i < count; i += 2u)
		{
			Parameter* parameter = subProc->getInputFields()[i / 2u];
			parameter->prm_desc = subProc->getInputFormat()->fmt_desc[i];
		}

		Array<NestConst<Parameter> >& paramArray = subProc->getOutputFields();

		count = subProc->getOutputFormat() ? subProc->getOutputFormat()->fmt_count : 0;
		if (count == 0 || paramArray.getCount() * 2 != count - 1u)
			PAR_error(csb, Arg::Gds(isc_prc_out_param_mismatch) << name);

		Format* format = Format::newFormat(pool, paramArray.getCount());
		subProc->prc_record_format = format;
		format->fmt_length = FLAG_BYTES(format->fmt_count);

		for (USHORT i = 0; i < count - 1u; i += 2u)
		{
			Parameter* parameter = paramArray[i / 2u];
			parameter->prm_desc = subProc->getOutputFormat()->fmt_desc[i];

			dsc& fmtDesc = format->fmt_desc[i / 2u];
			fmtDesc = parameter->prm_desc;

			if (fmtDesc.dsc_dtype >= dtype_aligned)
				format->fmt_length = FB_ALIGN(format->fmt_length, type_alignments[fmtDesc.dsc_dtype]);

			fmtDesc.dsc_address = (UCHAR*)(IPTR) format->fmt_length;
			format->fmt_length += fmtDesc.dsc_length;
		}

		DbgInfo* subDbgInfo = NULL;
		if (csb->csb_dbg_info->subProcs.get(name, subDbgInfo))
		{
			subCsb->csb_dbg_info = subDbgInfo;
			csb->csb_dbg_info->subProcs.remove(name);
		}
	}

	csb->subProcedures.put(name, node);
	csb->csb_blr_reader.setPos(node->blrStart + node->blrLength);

	return node;
}

void DeclareSubProcNode::parseParameters(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	Array<NestConst<Parameter> >& paramArray, USHORT* defaultCount)
{
	BlrReader& reader = csb->csb_blr_reader;

	paramArray.resize(reader.getWord());

	if (defaultCount)
		*defaultCount = 0;

	for (FB_SIZE_T i = 0; i < paramArray.getCount(); ++i)
	{
		Parameter* parameter = FB_NEW_POOL(pool) Parameter(pool);
		parameter->prm_number = USHORT(i);
		paramArray[parameter->prm_number] = parameter;

		csb->csb_blr_reader.getMetaName(parameter->prm_name);

		UCHAR hasDefault = reader.getByte();

		if (hasDefault == 1)
		{
			if (defaultCount && *defaultCount == 0)
				*defaultCount = paramArray.getCount() - i;

			parameter->prm_default_value = PAR_parse_value(tdbb, csb);
		}
		else if (hasDefault != 0)
			PAR_syntax_error(csb, "0 or 1");
	}
}

string DeclareSubProcNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, name);
	NODE_PRINT(printer, dsqlBlock);

	return "DeclareSubProcNode";
}

DeclareSubProcNode* DeclareSubProcNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	MemoryPool& pool = dsqlScratch->getPool();

	if (dsqlScratch->flags & DsqlCompilerScratch::FLAG_SUB_ROUTINE)
		ERR_post(Arg::Gds(isc_wish_list) << Arg::Gds(isc_random) << "nested sub procedure");

	DeclareSubProcNode* prevDecl = dsqlScratch->getSubProcedure(name);
	bool implemetingForward = prevDecl && !prevDecl->dsqlBlock && dsqlBlock;

	dsqlProcedure = implemetingForward ? prevDecl->dsqlProcedure : FB_NEW_POOL(pool) dsql_prc(pool);

	dsqlProcedure->prc_flags = PRC_subproc;
	dsqlProcedure->prc_name.identifier = name;
	dsqlProcedure->prc_in_count = USHORT(dsqlParameters.getCount());
	dsqlProcedure->prc_out_count = USHORT(dsqlReturns.getCount());

	if (dsqlParameters.hasData())
	{
		Array<NestConst<ParameterClause> >& paramArray = dsqlParameters;
		bool defaultFound = false;

		dsqlProcedure->prc_inputs = paramArray.front()->type;

		for (NestConst<ParameterClause>* i = paramArray.begin(); i != paramArray.end(); ++i)
		{
			ParameterClause* param = *i;
			const unsigned paramIndex = i - paramArray.begin();

			SignatureParameter sigParam(pool);
			sigParam.type = 0;	// input
			sigParam.number = (SSHORT) dsqlSignature.parameters.getCount();
			sigParam.name = param->name;
			sigParam.fromType(param->type);
			dsqlSignature.parameters.add(sigParam);

			if (param->defaultClause)
			{
				if (prevDecl)
				{
					status_exception::raise(
						Arg::Gds(isc_subproc_defvaldecl) <<
						name.c_str());
				}

				defaultFound = true;

				if (!implemetingForward && dsqlProcedure->prc_def_count == 0)
					dsqlProcedure->prc_def_count = paramArray.end() - i;
			}
			else
			{
				if (defaultFound)
				{
					// Parameter without default value after parameters with default.
					ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
							Arg::Gds(isc_bad_default_value) <<
							Arg::Gds(isc_invalid_clause) << Arg::Str("defaults must be last"));
				}

				if (prevDecl && paramIndex < prevDecl->dsqlParameters.getCount())
					param->defaultClause = prevDecl->dsqlParameters[paramIndex]->defaultClause;
			}
		}
	}

	if (dsqlReturns.hasData())
	{
		Array<NestConst<ParameterClause> >& paramArray = dsqlReturns;

		dsqlProcedure->prc_outputs = paramArray.front()->type;

		for (NestConst<ParameterClause>* i = paramArray.begin(); i != paramArray.end(); ++i)
		{
			ParameterClause* param = *i;
			const unsigned paramIndex = i - paramArray.begin();

			SignatureParameter sigParam(pool);
			sigParam.type = 1;	// output
			sigParam.number = (SSHORT) dsqlSignature.parameters.getCount();
			sigParam.name = param->name;
			sigParam.fromType(param->type);
			dsqlSignature.parameters.add(sigParam);
		}
	}

	if (!implemetingForward)
		dsqlScratch->putSubProcedure(this);
	else if (dsqlSignature != prevDecl->dsqlSignature)
	{
		status_exception::raise(
			Arg::Gds(isc_subproc_signat) <<
			name.c_str());
	}

	if (!dsqlBlock)	// forward decl
		return this;

	if (prevDecl)
		dsqlScratch->putSubProcedure(this, true);

	DsqlCompiledStatement* statement = FB_NEW_POOL(pool) DsqlCompiledStatement(pool);

	if (dsqlScratch->clientDialect > SQL_DIALECT_V5)
		statement->setBlrVersion(5);
	else
		statement->setBlrVersion(4);

	statement->setSendMsg(FB_NEW_POOL(pool) dsql_msg(pool));
	dsql_msg* message = FB_NEW_POOL(pool) dsql_msg(pool);
	statement->setReceiveMsg(message);
	message->msg_number = 1;

	statement->setType(DsqlCompiledStatement::TYPE_SELECT);

	blockScratch = FB_NEW_POOL(pool) DsqlCompilerScratch(pool,
		dsqlScratch->getAttachment(), dsqlScratch->getTransaction(), statement, dsqlScratch);
	blockScratch->clientDialect = dsqlScratch->clientDialect;
	blockScratch->flags |= DsqlCompilerScratch::FLAG_PROCEDURE | DsqlCompilerScratch::FLAG_SUB_ROUTINE;
	blockScratch->flags |= dsqlScratch->flags & DsqlCompilerScratch::FLAG_DDL;

	dsqlBlock = dsqlBlock->dsqlPass(blockScratch);

	return this;
}

void DeclareSubProcNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (!dsqlBlock)	// forward decl
		return;

	GEN_request(blockScratch, dsqlBlock);

	dsqlScratch->appendUChar(blr_subproc_decl);
	dsqlScratch->appendNullString(name.c_str());

	dsqlScratch->appendUChar(SUB_ROUTINE_TYPE_PSQL);

	dsqlScratch->appendUChar(
		blockScratch->getStatement()->getFlags() & DsqlCompiledStatement::FLAG_SELECTABLE ? 1 : 0);

	genParameters(dsqlScratch, dsqlBlock->parameters);
	genParameters(dsqlScratch, dsqlBlock->returns);

	BlrDebugWriter::BlrData& blrData = blockScratch->getBlrData();
	dsqlScratch->appendULong(ULONG(blrData.getCount()));
	dsqlScratch->appendBytes(blrData.begin(), blrData.getCount());

	dsqlScratch->putDebugSubProcedure(this);
}

void DeclareSubProcNode::genParameters(DsqlCompilerScratch* dsqlScratch,
	Array<NestConst<ParameterClause> >& paramArray)
{
	dsqlScratch->appendUShort(USHORT(paramArray.getCount()));

	for (NestConst<ParameterClause>* i = paramArray.begin(); i != paramArray.end(); ++i)
	{
		ParameterClause* param = *i;
		dsqlScratch->appendNullString(param->name.c_str());

		if (param->defaultClause)
		{
			dsqlScratch->appendUChar(1);
			GEN_expr(dsqlScratch, param->defaultClause->value);
		}
		else
			dsqlScratch->appendUChar(0);
	}
}

DeclareSubProcNode* DeclareSubProcNode::pass1(thread_db* /*tdbb*/, CompilerScratch* /*csb*/)
{
	return this;
}

DeclareSubProcNode* DeclareSubProcNode::pass2(thread_db* tdbb, CompilerScratch* /*csb*/)
{
	ContextPoolHolder context(tdbb, &subCsb->csb_pool);
	PAR_blr(tdbb, NULL, blrStart, blrLength, NULL, &subCsb, NULL, false, 0);

	return this;
}

const StmtNode* DeclareSubProcNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	// Nothing to execute. This is the declaration node.

	if (request->req_operation == jrd_req::req_evaluate)
		request->req_operation = jrd_req::req_return;

	return parentStmt;
}


//--------------------


static RegisterNode<DeclareVariableNode> regDeclareVariableNode(blr_dcl_variable);

DmlNode* DeclareVariableNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	DeclareVariableNode* node = FB_NEW_POOL(pool) DeclareVariableNode(pool);

	node->varId = csb->csb_blr_reader.getWord();

	ItemInfo itemInfo;
	PAR_desc(tdbb, csb, &node->varDesc, &itemInfo);

	csb->csb_variables = vec<DeclareVariableNode*>::newVector(
		*tdbb->getDefaultPool(), csb->csb_variables, node->varId + 1);

	if (itemInfo.isSpecial())
	{
		csb->csb_dbg_info->varIndexToName.get(node->varId, itemInfo.name);
		csb->csb_map_item_info.put(Item(Item::TYPE_VARIABLE, node->varId), itemInfo);
	}

	if (itemInfo.explicitCollation)
	{
		CompilerScratch::Dependency dependency(obj_collation);
		dependency.number = INTL_TEXT_TYPE(node->varDesc);
		csb->csb_dependencies.push(dependency);
	}

	return node;
}

DeclareVariableNode* DeclareVariableNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

string DeclareVariableNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlDef);
	NODE_PRINT(printer, varId);
	NODE_PRINT(printer, varDesc);

	return "DeclareVariableNode";
}

void DeclareVariableNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}

DeclareVariableNode* DeclareVariableNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	DeclareVariableNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) DeclareVariableNode(*tdbb->getDefaultPool());
	node->varId = varId + copier.csb->csb_remap_variable;
	node->varDesc = varDesc;

	copier.csb->csb_variables = vec<DeclareVariableNode*>::newVector(*tdbb->getDefaultPool(),
		copier.csb->csb_variables, node->varId + 1);

	return node;
}

DeclareVariableNode* DeclareVariableNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	vec<DeclareVariableNode*>* vector = csb->csb_variables = vec<DeclareVariableNode*>::newVector(
		*tdbb->getDefaultPool(), csb->csb_variables, varId + 1);
	fb_assert(!(*vector)[varId]);
	(*vector)[varId] = this;

	return this;
}

DeclareVariableNode* DeclareVariableNode::pass2(thread_db* /*tdbb*/, CompilerScratch* csb)
{
	impureOffset = CMP_impure(csb, sizeof(impure_value));
	return this;
}

const StmtNode* DeclareVariableNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		impure_value* variable = request->getImpure<impure_value>(impureOffset);
		variable->vlu_desc = varDesc;
		variable->vlu_desc.clearFlags();

		if (variable->vlu_desc.dsc_dtype <= dtype_varying)
		{
			if (!variable->vlu_string)
			{
				const USHORT len = variable->vlu_desc.dsc_length;
				variable->vlu_string = FB_NEW_RPT(*tdbb->getDefaultPool(), len) VaryingString();
				variable->vlu_string->str_length = len;
			}

			variable->vlu_desc.dsc_address = variable->vlu_string->str_data;
		}
		else
			variable->vlu_desc.dsc_address = (UCHAR*) &variable->vlu_misc;

		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<EraseNode> regEraseNode(blr_erase);

DmlNode* EraseNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	const USHORT n = csb->csb_blr_reader.getByte();

	if (n >= csb->csb_rpt.getCount() || !(csb->csb_rpt[n].csb_flags & csb_used))
		PAR_error(csb, Arg::Gds(isc_ctxnotdef));

	EraseNode* node = FB_NEW_POOL(pool) EraseNode(pool);
	node->stream = csb->csb_rpt[n].csb_stream;

	return node;
}

StmtNode* EraseNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	thread_db* tdbb = JRD_get_thread_data(); //necessary?

	NestConst<RelationSourceNode> relation = dsqlRelation;

	EraseNode* node = FB_NEW_POOL(dsqlScratch->getPool()) EraseNode(dsqlScratch->getPool());

	if (dsqlCursorName.hasData() && dsqlScratch->isPsql())
	{
		node->dsqlContext = dsqlPassCursorContext(dsqlScratch, dsqlCursorName, relation);

		// Process old context values.
		dsqlScratch->context->push(node->dsqlContext);
		++dsqlScratch->scopeLevel;

		node->statement = dsqlProcessReturning(dsqlScratch, dsqlReturning, statement);

		--dsqlScratch->scopeLevel;
		dsqlScratch->context->pop();

		return SavepointEncloseNode::make(dsqlScratch->getPool(), dsqlScratch, node);
	}

	dsqlScratch->getStatement()->setType(dsqlCursorName.hasData() ?
		DsqlCompiledStatement::TYPE_DELETE_CURSOR : DsqlCompiledStatement::TYPE_DELETE);

	// Generate record selection expression.

	RseNode* rse;

	if (dsqlCursorName.hasData())
		rse = dsqlPassCursorReference(dsqlScratch, dsqlCursorName, relation);
	else
	{
		rse = FB_NEW_POOL(dsqlScratch->getPool()) RseNode(dsqlScratch->getPool());

		rse->dsqlStreams = FB_NEW_POOL(dsqlScratch->getPool()) RecSourceListNode(dsqlScratch->getPool(), 1);
		doDsqlPass(dsqlScratch, rse->dsqlStreams->items[0], relation, false);

		if (dsqlBoolean)
			rse->dsqlWhere = doDsqlPass(dsqlScratch, dsqlBoolean, false);

		if (dsqlPlan)
			rse->rse_plan = doDsqlPass(dsqlScratch, dsqlPlan, false);

		if (dsqlOrder)
			rse->dsqlOrder = PASS1_sort(dsqlScratch, dsqlOrder, NULL);

		if (dsqlRows)
			PASS1_limit(dsqlScratch, dsqlRows->length, dsqlRows->skip, rse);
	}

	if (dsqlReturning || statement)
		rse->dsqlFlags |= RecordSourceNode::DFLAG_SINGLETON;

	node->dsqlRse = rse;
	node->dsqlRelation = nodeAs<RelationSourceNode>(rse->dsqlStreams->items[0]);

	node->statement = dsqlProcessReturning(dsqlScratch, dsqlReturning, statement);

	StmtNode* ret = dsqlNullifyReturning(dsqlScratch, node, true);

	dsqlScratch->context->pop();

	return SavepointEncloseNode::make(dsqlScratch->getPool(), dsqlScratch, ret);
}

string EraseNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlRelation);
	NODE_PRINT(printer, dsqlBoolean);
	NODE_PRINT(printer, dsqlPlan);
	NODE_PRINT(printer, dsqlOrder);
	NODE_PRINT(printer, dsqlRows);
	NODE_PRINT(printer, dsqlCursorName);
	NODE_PRINT(printer, dsqlReturning);
	NODE_PRINT(printer, dsqlRse);
	NODE_PRINT(printer, dsqlContext);
	NODE_PRINT(printer, statement);
	NODE_PRINT(printer, subStatement);
	NODE_PRINT(printer, stream);

	return "EraseNode";
}

void EraseNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	const dsql_msg* message = dsqlGenDmlHeader(dsqlScratch, dsqlRse);
	const dsql_ctx* context;

	if (dsqlContext)
	{
		context = dsqlContext;

		if (statement)
		{
			dsqlScratch->appendUChar(blr_begin);
			statement->genBlr(dsqlScratch);
			dsqlScratch->appendUChar(blr_erase);
			GEN_stuff_context(dsqlScratch, context);
			dsqlScratch->appendUChar(blr_end);
		}
		else
		{
			dsqlScratch->appendUChar(blr_erase);
			GEN_stuff_context(dsqlScratch, context);
		}
	}
	else
	{
		context = dsqlRelation->dsqlContext;

		if (statement)
		{
			dsqlScratch->appendUChar(blr_begin);
			statement->genBlr(dsqlScratch);
			dsqlScratch->appendUChar(blr_erase);
			GEN_stuff_context(dsqlScratch, context);
			dsqlScratch->appendUChar(blr_end);
		}
		else
		{
			dsqlScratch->appendUChar(blr_erase);
			GEN_stuff_context(dsqlScratch, context);
		}
	}

	if (message)
		dsqlScratch->appendUChar(blr_end);
}

EraseNode* EraseNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	pass1Erase(tdbb, csb, this);

	doPass1(tdbb, csb, statement.getAddress());
	doPass1(tdbb, csb, subStatement.getAddress());

	return this;
}

// Checkout an erase statement. If it references a view, and is kosher, fix it up.
void EraseNode::pass1Erase(thread_db* tdbb, CompilerScratch* csb, EraseNode* node)
{
	// If updateable views with triggers are involved, there maybe a recursive call to be ignored.

	if (node->subStatement)
		return;

	// To support nested views, loop until we hit a table or a view with user-defined triggers
	// (which means no update).

	jrd_rel* parent = NULL;
	jrd_rel* view = NULL;
	StreamType parentStream;

	for (;;)
	{
		StreamType newStream = node->stream;
		const StreamType stream = newStream;

		CompilerScratch::csb_repeat* const tail = &csb->csb_rpt[stream];
		tail->csb_flags |= csb_erase;

		jrd_rel* const relation = tail->csb_relation;
		view = relation->rel_view_rse ? relation : view;

		if (!parent)
		{
			parent = tail->csb_view;
			parentStream = tail->csb_view_stream;
		}

		postTriggerAccess(csb, relation, ExternalAccess::exa_delete, view);

		// Check out delete. If this is a delete thru a view, verify the view by checking for read
		// access on the base table. If field-level select privileges are implemented, this needs
		// to be enhanced.

		SecurityClass::flags_t priv = SCL_delete;

		if (parent)
			priv |= SCL_select;

		RefPtr<const TrigVector> trigger(relation->rel_pre_erase ?
			relation->rel_pre_erase : relation->rel_post_erase);

		// If we have a view with triggers, let's expand it.

		if (relation->rel_view_rse && trigger)
		{
			newStream = csb->nextStream();
			node->stream = newStream;
			CMP_csb_element(csb, newStream)->csb_relation = relation;

			node->statement = pass1ExpandView(tdbb, csb, stream, newStream, false);
		}

		// Get the source relation, either a table or yet another view.

		RelationSourceNode* source = pass1Update(tdbb, csb, relation, trigger, stream, newStream,
												 priv, parent, parentStream, parentStream);

		if (!source)
			return;	// no source means we're done

		parent = relation;
		parentStream = stream;

		// Remap the source stream.

		StreamType* map = tail->csb_map;

		if (trigger)
		{
			// ASF: This code is responsible to make view's WITH CHECK OPTION to work as constraints.
			// I don't see how it could run for delete statements under normal conditions.

			// Set up the new target stream.

			EraseNode* viewNode = FB_NEW_POOL(*tdbb->getDefaultPool()) EraseNode(*tdbb->getDefaultPool());
			viewNode->stream = node->stream;

			node->subStatement = viewNode;

			// Substitute the original delete node with the newly created one.
			node = viewNode;
		}
		else
		{
			// This relation is not actually being updated as this operation
			// goes deeper (we have a naturally updatable view).
			csb->csb_rpt[newStream].csb_flags &= ~csb_view_update;
		}

		// Let's reset the target stream.
		newStream = source->getStream();
		node->stream = map[newStream];
	}
}

EraseNode* EraseNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	doPass2(tdbb, csb, statement.getAddress(), this);
	doPass2(tdbb, csb, subStatement.getAddress(), this);

	impureOffset = CMP_impure(csb, sizeof(SLONG));
	csb->csb_rpt[stream].csb_flags |= csb_update;

	return this;
}

const StmtNode* EraseNode::execute(thread_db* tdbb, jrd_req* request, ExeState* exeState) const
{
	const StmtNode* retNode;

	if (request->req_operation == jrd_req::req_unwind)
		retNode = parentStmt;
	else if (request->req_operation == jrd_req::req_return && subStatement)
	{
		if (!exeState->topNode)
		{
			exeState->topNode = this;
			exeState->whichEraseTrig = PRE_TRIG;
		}

		exeState->prevNode = this;
		retNode = erase(tdbb, request, exeState->whichEraseTrig);

		if (exeState->whichEraseTrig == PRE_TRIG)
		{
			retNode = subStatement;
			fb_assert(retNode->parentStmt == this);
			///retNode->parentStmt = exeState->prevNode;
		}

		if (exeState->topNode == this && exeState->whichEraseTrig == POST_TRIG)
		{
			exeState->topNode = NULL;
			exeState->whichEraseTrig = ALL_TRIGS;
		}
		else
			request->req_operation = jrd_req::req_evaluate;
	}
	else
	{
		exeState->prevNode = this;
		retNode = erase(tdbb, request, ALL_TRIGS);

		if (!subStatement && exeState->whichEraseTrig == PRE_TRIG)
			exeState->whichEraseTrig = POST_TRIG;
	}

	return retNode;
}

// Perform erase operation.
const StmtNode* EraseNode::erase(thread_db* tdbb, jrd_req* request, WhichTrigger whichTrig) const
{
	jrd_tra* transaction = request->req_transaction;
	record_param* rpb = &request->req_rpb[stream];
	jrd_rel* relation = rpb->rpb_relation;

	if (rpb->rpb_number.isBof() || (!relation->rel_view_rse && !rpb->rpb_number.isValid()))
		ERR_post(Arg::Gds(isc_no_cur_rec));

	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
		{
			request->req_records_affected.bumpModified(false);

			if (!statement)
				break;

			const Format* format = MET_current(tdbb, rpb->rpb_relation);
			Record* record = VIO_record(tdbb, rpb, format, tdbb->getDefaultPool());

			rpb->rpb_address = record->getData();
			rpb->rpb_length = format->fmt_length;
			rpb->rpb_format_number = format->fmt_version;

			return statement;
		}

		case jrd_req::req_return:
			break;

		default:
			return parentStmt;
	}

	request->req_operation = jrd_req::req_return;
	RLCK_reserve_relation(tdbb, transaction, relation, true);

	// If the stream was sorted, the various fields in the rpb are probably junk.
	// Just to make sure that everything is cool, refetch and release the record.

	if (rpb->rpb_runtime_flags & RPB_refetch)
	{
		VIO_refetch_record(tdbb, rpb, transaction, false, false);
		rpb->rpb_runtime_flags &= ~RPB_refetch;
	}

	if (rpb->rpb_runtime_flags & RPB_undo_deleted)
		return parentStmt;

	SavepointChangeMarker scMarker(transaction);

	// Handle pre-operation trigger.
	preModifyEraseTriggers(tdbb, &relation->rel_pre_erase, whichTrig, rpb, NULL, TRIGGER_DELETE);

	if (relation->rel_file)
		EXT_erase(rpb, transaction);
	else if (relation->isVirtual())
		VirtualTable::erase(tdbb, rpb);
	else if (!relation->rel_view_rse)
		VIO_erase(tdbb, rpb, transaction);

	// Handle post operation trigger.
	if (relation->rel_post_erase && whichTrig != PRE_TRIG)
	{
		EXE_execute_triggers(tdbb, &relation->rel_post_erase, rpb, NULL, TRIGGER_DELETE, POST_TRIG);
	}

	// Call IDX_erase (which checks constraints) after all post erase triggers have fired.
	// This is required for cascading referential integrity, which can be implemented as
	// post_erase triggers.

	if (!relation->rel_file && !relation->rel_view_rse && !relation->isVirtual())
		IDX_erase(tdbb, rpb, transaction);

	if (!relation->rel_view_rse || (whichTrig == ALL_TRIGS || whichTrig == POST_TRIG))
	{
		request->req_records_deleted++;
		request->req_records_affected.bumpModified(true);
	}

	rpb->rpb_number.setValid(false);

	return parentStmt;
}


//--------------------


static RegisterNode<ErrorHandlerNode> regErrorHandlerNode(blr_error_handler);

DmlNode* ErrorHandlerNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	ErrorHandlerNode* node = FB_NEW_POOL(pool) ErrorHandlerNode(pool);

	const USHORT n = csb->csb_blr_reader.getWord();

	for (unsigned i = 0; i < n; i++)
	{
		const USHORT codeType = csb->csb_blr_reader.getByte();
		ExceptionItem& item = node->conditions.add();

		switch (codeType)
		{
			case blr_sql_code:
				item.type = ExceptionItem::SQL_CODE;
				item.code = (SSHORT) csb->csb_blr_reader.getWord();
				break;

			case blr_sql_state:
				item.type = ExceptionItem::SQL_STATE;
				csb->csb_blr_reader.getString(item.name);
				break;

			case blr_gds_code:
				item.type = ExceptionItem::GDS_CODE;
				csb->csb_blr_reader.getString(item.name);
				item.name.lower();
				if (!(item.code = PAR_symbol_to_gdscode(item.name)))
					PAR_error(csb, Arg::Gds(isc_codnotdef) << item.name);
				break;

			case blr_exception:
			{
				csb->csb_blr_reader.getString(item.name);
				if (!MET_load_exception(tdbb, item))
					PAR_error(csb, Arg::Gds(isc_xcpnotdef) << item.name);

				CompilerScratch::Dependency dependency(obj_exception);
				dependency.number = item.code;
				csb->csb_dependencies.push(dependency);
				break;
			}

			case blr_default_code:
				item.type = ExceptionItem::XCP_DEFAULT;
				item.code = 0;
				break;

			default:
				fb_assert(FALSE);
				break;
		}
	}

	node->action = PAR_parse_stmt(tdbb, csb);

	return node;
}

ErrorHandlerNode* ErrorHandlerNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	ErrorHandlerNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ErrorHandlerNode(dsqlScratch->getPool());
	node->conditions = conditions;
	node->action = action->dsqlPass(dsqlScratch);
	return node;
}

string ErrorHandlerNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, action);
	NODE_PRINT(printer, conditions);

	return "ErrorHandlerNode";
}

void ErrorHandlerNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_error_handler);
	fb_assert(conditions.getCount() < MAX_USHORT);
	dsqlScratch->appendUShort(USHORT(conditions.getCount()));

	for (ExceptionArray::iterator i = conditions.begin(); i != conditions.end(); ++i)
	{
		switch (i->type)
		{
			case ExceptionItem::SQL_CODE:
				dsqlScratch->appendUChar(blr_sql_code);
				dsqlScratch->appendUShort(i->code);
				break;

			case ExceptionItem::SQL_STATE:
				dsqlScratch->appendUChar(blr_sql_state);
				dsqlScratch->appendNullString(i->name.c_str());
				break;

			case ExceptionItem::GDS_CODE:
				dsqlScratch->appendUChar(blr_gds_code);
				dsqlScratch->appendNullString(i->name.c_str());
				break;

			case ExceptionItem::XCP_CODE:
				dsqlScratch->appendUChar(blr_exception);
				dsqlScratch->appendNullString(i->name.c_str());
				break;

			case ExceptionItem::XCP_DEFAULT:
				dsqlScratch->appendUChar(blr_default_code);
				break;

			default:
				fb_assert(false);
				break;
		}
	}

	action->genBlr(dsqlScratch);
}

ErrorHandlerNode* ErrorHandlerNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, action.getAddress());
	return this;
}

ErrorHandlerNode* ErrorHandlerNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	doPass2(tdbb, csb, action.getAddress(), this);
	return this;
}

const StmtNode* ErrorHandlerNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* exeState) const
{
	if ((request->req_flags & req_error_handler) && !exeState->errorPending)
	{
		fb_assert(request->req_caller == exeState->oldRequest);
		request->req_caller = NULL;
		exeState->exit = true;
		return this;
	}

	const StmtNode* retNode = parentStmt;
	retNode = retNode->parentStmt;

	if (request->req_operation == jrd_req::req_unwind)
		retNode = retNode->parentStmt;

	request->req_last_xcp.clear();

	return retNode;
}


//--------------------


static RegisterNode<ExecProcedureNode> regExecProcedureNodeProc(blr_exec_proc);
static RegisterNode<ExecProcedureNode> regExecProcedureNodeProc2(blr_exec_proc2);
static RegisterNode<ExecProcedureNode> regExecProcedureNodePid(blr_exec_pid);
static RegisterNode<ExecProcedureNode> regExecProcedureNodeSubProc(blr_exec_subproc);

// Parse an execute procedure reference.
DmlNode* ExecProcedureNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	SET_TDBB(tdbb);

	jrd_prc* procedure = NULL;
	QualifiedName name;

	if (blrOp == blr_exec_pid)
	{
		const USHORT pid = csb->csb_blr_reader.getWord();
		if (!(procedure = MET_lookup_procedure_id(tdbb, pid, false, false, 0)))
			name.identifier.printf("id %d", pid);
	}
	else
	{
		if (blrOp == blr_exec_proc2)
			csb->csb_blr_reader.getMetaName(name.package);

		csb->csb_blr_reader.getMetaName(name.identifier);

		if (blrOp == blr_exec_subproc)
		{
			DeclareSubProcNode* declareNode;

			for (auto curCsb = csb; curCsb && !procedure; curCsb = curCsb->mainCsb)
			{
				if (curCsb->subProcedures.get(name.identifier, declareNode))
					procedure = declareNode->routine;
			}
		}
		else
			procedure = MET_lookup_procedure(tdbb, name, false);
	}

	if (!procedure)
		PAR_error(csb, Arg::Gds(isc_prcnotdef) << Arg::Str(name.toString()));

	ExecProcedureNode* node = FB_NEW_POOL(pool) ExecProcedureNode(pool);
	node->procedure = procedure;

	PAR_procedure_parms(tdbb, csb, procedure, node->inputMessage.getAddress(),
		node->inputSources.getAddress(), node->inputTargets.getAddress(), true);
	PAR_procedure_parms(tdbb, csb, procedure, node->outputMessage.getAddress(),
		node->outputSources.getAddress(), node->outputTargets.getAddress(), false);

	if (!procedure->isSubRoutine())
	{
		CompilerScratch::Dependency dependency(obj_procedure);
		dependency.procedure = procedure;
		csb->csb_dependencies.push(dependency);
	}

	return node;
}

ExecProcedureNode* ExecProcedureNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	dsql_prc* procedure = NULL;

	if (dsqlName.package.isEmpty())
	{
		DeclareSubProcNode* subProcedure = dsqlScratch->getSubProcedure(dsqlName.identifier);
		procedure = subProcedure ? subProcedure->dsqlProcedure : NULL;
	}

	if (!procedure)
		procedure = METD_get_procedure(dsqlScratch->getTransaction(), dsqlScratch, dsqlName);

	if (!procedure)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
				  Arg::Gds(isc_dsql_procedure_err) <<
				  Arg::Gds(isc_random) <<
				  Arg::Str(dsqlName.toString()));
	}

	if (!dsqlScratch->isPsql())
		dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_EXEC_PROCEDURE);

	ExecProcedureNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ExecProcedureNode(dsqlScratch->getPool(), dsqlName);
	node->dsqlProcedure = procedure;

	if (node->dsqlName.package.isEmpty() && procedure->prc_name.package.hasData())
		node->dsqlName.package = procedure->prc_name.package;

	// Handle input parameters.

	const USHORT count = inputSources ? inputSources->items.getCount() : 0;
	if (count > procedure->prc_in_count || count < procedure->prc_in_count - procedure->prc_def_count)
		ERRD_post(Arg::Gds(isc_prcmismat) << Arg::Str(dsqlName.toString()));

	node->inputSources = doDsqlPass(dsqlScratch, inputSources);

	if (count)
	{
		// Initialize this stack variable, and make it look like a node.
		dsc desc_node;

		NestConst<ValueExprNode>* ptr = node->inputSources->items.begin();
		const NestConst<ValueExprNode>* end = node->inputSources->items.end();

		for (const dsql_fld* field = procedure->prc_inputs; ptr != end; ++ptr, field = field->fld_next)
		{
			DEV_BLKCHK(field, dsql_type_fld);
			DEV_BLKCHK(*ptr, dsql_type_nod);
			MAKE_desc_from_field(&desc_node, field);
			PASS1_set_parameter_type(dsqlScratch, *ptr, &desc_node, false);
		}
	}

	// Handle output parameters.

	if (dsqlScratch->isPsql())
	{
		const USHORT outCount = outputSources ? outputSources->items.getCount() : 0;

		if (outCount != procedure->prc_out_count)
			ERRD_post(Arg::Gds(isc_prc_out_param_mismatch) << Arg::Str(dsqlName.toString()));

		node->outputSources = dsqlPassArray(dsqlScratch, outputSources);
	}
	else
	{
		if (outputSources)
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
					  // Token unknown
					  Arg::Gds(isc_token_err) <<
					  Arg::Gds(isc_random) << Arg::Str("RETURNING_VALUES"));
		}

		node->outputSources = explodeOutputs(dsqlScratch, procedure);
	}

	if (node->outputSources)
	{
		for (const NestConst<ValueExprNode>* i = node->outputSources->items.begin();
			 i != node->outputSources->items.end();
			 ++i)
		{
			AssignmentNode::dsqlValidateTarget(*i);
		}
	}

	return node;
}

// Generate a parameter list to correspond to procedure outputs.
ValueListNode* ExecProcedureNode::explodeOutputs(DsqlCompilerScratch* dsqlScratch,
	const dsql_prc* procedure)
{
	DEV_BLKCHK(dsqlScratch, dsql_type_req);
	DEV_BLKCHK(procedure, dsql_type_prc);

	const USHORT count = procedure->prc_out_count;
	ValueListNode* output = FB_NEW_POOL(dsqlScratch->getPool()) ValueListNode(dsqlScratch->getPool(), count);
	NestConst<ValueExprNode>* ptr = output->items.begin();

	for (const dsql_fld* field = procedure->prc_outputs; field; field = field->fld_next, ++ptr)
	{
		DEV_BLKCHK(field, dsql_type_fld);

		ParameterNode* paramNode = FB_NEW_POOL(dsqlScratch->getPool()) ParameterNode(dsqlScratch->getPool());
		*ptr = paramNode;

		dsql_par* parameter = paramNode->dsqlParameter = MAKE_parameter(
			dsqlScratch->getStatement()->getReceiveMsg(), true, true, 0, NULL);
		paramNode->dsqlParameterIndex = parameter->par_index;

		MAKE_desc_from_field(&parameter->par_desc, field);
		parameter->par_name = parameter->par_alias = field->fld_name.c_str();
		parameter->par_rel_name = procedure->prc_name.identifier.c_str();
		parameter->par_owner_name = procedure->prc_owner.c_str();
	}

	return output;
}

string ExecProcedureNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlName);
	NODE_PRINT(printer, inputSources);
	NODE_PRINT(printer, inputTargets);
	NODE_PRINT(printer, inputMessage);
	NODE_PRINT(printer, outputSources);
	NODE_PRINT(printer, outputTargets);
	NODE_PRINT(printer, outputMessage);

	return "ExecProcedureNode";
}

void ExecProcedureNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	const dsql_msg* message = NULL;

	if (dsqlScratch->getStatement()->getType() == DsqlCompiledStatement::TYPE_EXEC_PROCEDURE)
	{
		if ((message = dsqlScratch->getStatement()->getReceiveMsg()))
		{
			dsqlScratch->appendUChar(blr_begin);
			dsqlScratch->appendUChar(blr_send);
			dsqlScratch->appendUChar(message->msg_number);
		}
	}

	if (dsqlName.package.hasData())
	{
		dsqlScratch->appendUChar(blr_exec_proc2);
		dsqlScratch->appendMetaString(dsqlName.package.c_str());
	}
	else
	{
		dsqlScratch->appendUChar(
			(dsqlProcedure->prc_flags & PRC_subproc) ? blr_exec_subproc : blr_exec_proc);
	}

	dsqlScratch->appendMetaString(dsqlName.identifier.c_str());

	// Input parameters.
	if (inputSources)
	{
		dsqlScratch->appendUShort(inputSources->items.getCount());
		NestConst<ValueExprNode>* ptr = inputSources->items.begin();
		const NestConst<ValueExprNode>* end = inputSources->items.end();

		while (ptr < end)
			GEN_expr(dsqlScratch, *ptr++);
	}
	else
		dsqlScratch->appendUShort(0);

	// Output parameters.
	if (outputSources)
	{
		dsqlScratch->appendUShort(outputSources->items.getCount());
		NestConst<ValueExprNode>* ptr = outputSources->items.begin();

		for (const NestConst<ValueExprNode>* end = outputSources->items.end(); ptr != end; ++ptr)
			GEN_expr(dsqlScratch, *ptr);
	}
	else
		dsqlScratch->appendUShort(0);

	if (message)
		dsqlScratch->appendUChar(blr_end);
}

ExecProcedureNode* ExecProcedureNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	if (!procedure->isSubRoutine())
	{
		// Post access to procedure.
		CMP_post_procedure_access(tdbb, csb, procedure);
		CMP_post_resource(&csb->csb_resources, procedure, Resource::rsc_procedure, procedure->getId());
	}

	doPass1(tdbb, csb, inputSources.getAddress());
	doPass1(tdbb, csb, inputTargets.getAddress());
	doPass1(tdbb, csb, inputMessage.getAddress());
	doPass1(tdbb, csb, outputSources.getAddress());
	doPass1(tdbb, csb, outputTargets.getAddress());
	doPass1(tdbb, csb, outputMessage.getAddress());

	return this;
}

ExecProcedureNode* ExecProcedureNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, inputSources.getAddress());
	ExprNode::doPass2(tdbb, csb, inputTargets.getAddress());
	doPass2(tdbb, csb, inputMessage.getAddress(), this);
	ExprNode::doPass2(tdbb, csb, outputSources.getAddress());
	ExprNode::doPass2(tdbb, csb, outputTargets.getAddress());
	doPass2(tdbb, csb, outputMessage.getAddress(), this);

	if (outputTargets)
	{
		for (const NestConst<ValueExprNode>* i = outputTargets->items.begin();
			 i != outputTargets->items.end();
			 ++i)
		{
			AssignmentNode::validateTarget(csb, *i);
		}
	}

	return this;
}

const StmtNode* ExecProcedureNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		executeProcedure(tdbb, request);
		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}

// Execute a stored procedure. Begin by assigning the input parameters.
// End by assigning the output parameters.
void ExecProcedureNode::executeProcedure(thread_db* tdbb, jrd_req* request) const
{
	if (!procedure->isImplemented())
	{
		status_exception::raise(
			Arg::Gds(isc_proc_pack_not_implemented) <<
				Arg::Str(procedure->getName().identifier) << Arg::Str(procedure->getName().package));
	}

	ULONG inMsgLength = 0;
	UCHAR* inMsg = NULL;

	if (inputMessage)
	{
		inMsgLength = inputMessage->format->fmt_length;
		inMsg = request->getImpure<UCHAR>(inputMessage->impureOffset);
	}

	const Format* format = NULL;
	ULONG outMsgLength = 0;
	UCHAR* outMsg = NULL;
	Array<UCHAR> tempBuffer;

	if (outputMessage)
	{
		format = outputMessage->format;
		outMsgLength = format->fmt_length;
		outMsg = request->getImpure<UCHAR>(outputMessage->impureOffset);
	}
	else
	{
		format = procedure->getOutputFormat();
		outMsgLength = format->fmt_length;
		outMsg = tempBuffer.getBuffer(outMsgLength + FB_DOUBLE_ALIGN - 1);
		outMsg = FB_ALIGN(outMsg, FB_DOUBLE_ALIGN);
	}

	if (inputSources)
	{
		const NestConst<ValueExprNode>* const sourceEnd = inputSources->items.end();
		const NestConst<ValueExprNode>* sourcePtr = inputSources->items.begin();
		const NestConst<ValueExprNode>* targetPtr = inputTargets->items.begin();

		for (; sourcePtr != sourceEnd; ++sourcePtr, ++targetPtr)
			EXE_assignment(tdbb, *sourcePtr, *targetPtr);
	}

	jrd_tra* transaction = request->req_transaction;
	const SavNumber savNumber = transaction->tra_save_point ?
		transaction->tra_save_point->getNumber() : 0;

	jrd_req* procRequest = procedure->getStatement()->findRequest(tdbb);

	// trace procedure execution start
	TraceProcExecute trace(tdbb, procRequest, request, inputTargets);

	// Catch errors so we can unwind cleanly.

	try
	{
		procRequest->req_timestamp_utc = request->req_timestamp_utc;

		EXE_start(tdbb, procRequest, transaction);

		if (inputMessage)
			EXE_send(tdbb, procRequest, 0, inMsgLength, inMsg);

		EXE_receive(tdbb, procRequest, 1, outMsgLength, outMsg);

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
		trace.finish(false,
			noPriv ? Firebird::ITracePlugin::RESULT_UNAUTHORIZED : ITracePlugin::RESULT_FAILED);

		EXE_unwind(tdbb, procRequest);
		procRequest->req_attachment = NULL;
		procRequest->req_flags &= ~(req_in_use | req_proc_fetch);
		throw;
	}

	// trace procedure execution finish
	trace.finish(false, ITracePlugin::RESULT_SUCCESS);

	EXE_unwind(tdbb, procRequest);
	procRequest->req_attachment = NULL;
	procRequest->req_flags &= ~(req_in_use | req_proc_fetch);

	if (outputSources)
	{
		const NestConst<ValueExprNode>* const sourceEnd = outputSources->items.end();
		const NestConst<ValueExprNode>* sourcePtr = outputSources->items.begin();
		const NestConst<ValueExprNode>* targetPtr = outputTargets->items.begin();

		for (; sourcePtr != sourceEnd; ++sourcePtr, ++targetPtr)
			EXE_assignment(tdbb, *sourcePtr, *targetPtr);
	}
}


//--------------------


static RegisterNode<ExecStatementNode> regExecStatementSql(blr_exec_sql);
static RegisterNode<ExecStatementNode> regExecStatementInto(blr_exec_into);
static RegisterNode<ExecStatementNode> regExecStatementStmt(blr_exec_stmt);

DmlNode* ExecStatementNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	ExecStatementNode* node = FB_NEW_POOL(pool) ExecStatementNode(pool);
	node->traScope = EDS::traCommon;

	switch (blrOp)
	{
		case blr_exec_sql:
			node->sql = PAR_parse_value(tdbb, csb);
			break;

		case blr_exec_into:
		{
			const unsigned outputs = csb->csb_blr_reader.getWord();

			node->sql = PAR_parse_value(tdbb, csb);

			if (csb->csb_blr_reader.getByte() == 0)	// not singleton flag
				node->innerStmt = PAR_parse_stmt(tdbb, csb);

			node->outputs = PAR_args(tdbb, csb, outputs, outputs);
			break;
		}

		case blr_exec_stmt:
		{
			unsigned inputs = 0;
			unsigned outputs = 0;

			while (true)
			{
				const UCHAR code = csb->csb_blr_reader.getByte();

				switch (code)
				{
					case blr_exec_stmt_inputs:
						inputs = csb->csb_blr_reader.getWord();
						break;

					case blr_exec_stmt_outputs:
						outputs = csb->csb_blr_reader.getWord();
						break;

					case blr_exec_stmt_sql:
						node->sql = PAR_parse_value(tdbb, csb);
						break;

					case blr_exec_stmt_proc_block:
						node->innerStmt = PAR_parse_stmt(tdbb, csb);
						break;

					case blr_exec_stmt_data_src:
						node->dataSource = PAR_parse_value(tdbb, csb);
						break;

					case blr_exec_stmt_user:
						node->userName = PAR_parse_value(tdbb, csb);
						break;

					case blr_exec_stmt_pwd:
						node->password = PAR_parse_value(tdbb, csb);
						break;

					case blr_exec_stmt_role:
						node->role = PAR_parse_value(tdbb, csb);
						break;

					case blr_exec_stmt_tran:
						PAR_syntax_error(csb, "external transaction parameters");
						break;

					case blr_exec_stmt_tran_clone:
						node->traScope = static_cast<EDS::TraScope>(csb->csb_blr_reader.getByte());
						break;

					case blr_exec_stmt_privs:
						node->useCallerPrivs = true;
						break;

					case blr_exec_stmt_in_params:
					case blr_exec_stmt_in_params2:
					{
						node->inputs = FB_NEW_POOL(pool) ValueListNode(pool, inputs);
						NestConst<ValueExprNode>* const end = node->inputs->items.end();

						for (NestConst<ValueExprNode>* ptr = node->inputs->items.begin();
							 ptr != end;
							 ++ptr)
						{
							if (code == blr_exec_stmt_in_params2)
							{
								MetaName name;
								csb->csb_blr_reader.getMetaName(name);

								if (name.hasData())
								{
									MemoryPool& pool = csb->csb_pool;

									if (!node->inputNames)
										node->inputNames = FB_NEW_POOL(pool) EDS::ParamNames(pool);

									MetaName* newName = FB_NEW_POOL(pool) MetaName(pool, name);
									node->inputNames->add(newName);
								}
							}

							*ptr = PAR_parse_value(tdbb, csb);
						}

						break;
					}

					case blr_exec_stmt_out_params:
						node->outputs = PAR_args(tdbb, csb, outputs, outputs);
						break;

					case blr_end:
						break;

					default:
						PAR_syntax_error(csb, "unknown EXECUTE STATEMENT option");
				}

				if (code == blr_end)
					break;
			}

			break;
		}

		default:
			fb_assert(false);
	}

	return node;
}

StmtNode* ExecStatementNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	ExecStatementNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ExecStatementNode(dsqlScratch->getPool());

	node->sql = doDsqlPass(dsqlScratch, sql);
	node->inputs = doDsqlPass(dsqlScratch, inputs);
	node->inputNames = inputNames;

	// Check params names uniqueness, if present.

	if (node->inputNames)
	{
		const FB_SIZE_T count = node->inputNames->getCount();
		StrArray names(*getDefaultMemoryPool(), count);

		for (FB_SIZE_T i = 0; i != count; ++i)
		{
			const MetaName* name = (*node->inputNames)[i];

			FB_SIZE_T pos;
			if (names.find(name->c_str(), pos))
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-637) <<
						  Arg::Gds(isc_dsql_duplicate_spec) << *name);
			}

			names.insert(pos, name->c_str());
		}
	}

	node->outputs = dsqlPassArray(dsqlScratch, outputs);

	if (node->outputs)
	{
		for (const NestConst<ValueExprNode>* i = node->outputs->items.begin();
			 i != node->outputs->items.end();
			 ++i)
		{
			AssignmentNode::dsqlValidateTarget(*i);
		}
	}

	if (innerStmt)
	{
		++dsqlScratch->loopLevel;
		node->dsqlLabelNumber = dsqlPassLabel(dsqlScratch, false, dsqlLabelName);
		node->innerStmt = innerStmt->dsqlPass(dsqlScratch);
		--dsqlScratch->loopLevel;
		dsqlScratch->labels.pop();
	}

	// Process various optional arguments.

	node->dataSource = doDsqlPass(dsqlScratch, dataSource);
	node->userName = doDsqlPass(dsqlScratch, userName);
	node->password = doDsqlPass(dsqlScratch, password);
	node->role = doDsqlPass(dsqlScratch, role);
	node->traScope = traScope;
	node->useCallerPrivs = useCallerPrivs;

	return SavepointEncloseNode::make(dsqlScratch->getPool(), dsqlScratch, node);
}

string ExecStatementNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlLabelName);
	NODE_PRINT(printer, dsqlLabelNumber);
	NODE_PRINT(printer, sql);
	NODE_PRINT(printer, dataSource);
	NODE_PRINT(printer, userName);
	NODE_PRINT(printer, password);
	NODE_PRINT(printer, role);
	NODE_PRINT(printer, innerStmt);
	NODE_PRINT(printer, inputs);
	NODE_PRINT(printer, outputs);
	NODE_PRINT(printer, useCallerPrivs);
	NODE_PRINT(printer, traScope);
	NODE_PRINT(printer, inputNames);

	return "ExecStatementNode";
}

void ExecStatementNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (innerStmt)
	{
		dsqlScratch->appendUChar(blr_label);
		dsqlScratch->appendUChar(dsqlLabelNumber);
	}

	// If no new features of EXECUTE STATEMENT are used, lets generate old BLR.
	if (!dataSource && !userName && !password && !role && !useCallerPrivs && !inputs &&
		 traScope == EDS::traNotSet)
	{
		if (outputs)
		{
			dsqlScratch->appendUChar(blr_exec_into);
			dsqlScratch->appendUShort(outputs->items.getCount());

			GEN_expr(dsqlScratch, sql);

			if (innerStmt)
			{
				dsqlScratch->appendUChar(0); // Non-singleton.
				innerStmt->genBlr(dsqlScratch);
			}
			else
				dsqlScratch->appendUChar(1); // Singleton.

			for (FB_SIZE_T i = 0; i < outputs->items.getCount(); ++i)
				GEN_expr(dsqlScratch, outputs->items[i]);
		}
		else
		{
			dsqlScratch->appendUChar(blr_exec_sql);
			GEN_expr(dsqlScratch, sql);
		}
	}
	else
	{
		dsqlScratch->appendUChar(blr_exec_stmt);

		// Counts of input and output parameters.
		if (inputs)
		{
			dsqlScratch->appendUChar(blr_exec_stmt_inputs);
			dsqlScratch->appendUShort(inputs->items.getCount());
		}

		if (outputs)
		{
			dsqlScratch->appendUChar(blr_exec_stmt_outputs);
			dsqlScratch->appendUShort(outputs->items.getCount());
		}

		// Query expression.
		dsqlScratch->appendUChar(blr_exec_stmt_sql);
		GEN_expr(dsqlScratch, sql);

		// Proc block body.
		if (innerStmt)
		{
			dsqlScratch->appendUChar(blr_exec_stmt_proc_block);
			innerStmt->genBlr(dsqlScratch);
		}

		// External data source, user, password and role.
		genOptionalExpr(dsqlScratch, blr_exec_stmt_data_src, dataSource);
		genOptionalExpr(dsqlScratch, blr_exec_stmt_user, userName);
		genOptionalExpr(dsqlScratch, blr_exec_stmt_pwd, password);
		genOptionalExpr(dsqlScratch, blr_exec_stmt_role, role);

		// dsqlScratch's transaction behavior.
		if (traScope != EDS::traNotSet)
		{
			// Transaction parameters equal to current transaction.
			dsqlScratch->appendUChar(blr_exec_stmt_tran_clone);
			dsqlScratch->appendUChar(UCHAR(traScope));
		}

		// Inherit caller's privileges?
		if (useCallerPrivs)
			dsqlScratch->appendUChar(blr_exec_stmt_privs);

		// Inputs.
		if (inputs)
		{
			if (inputNames)
				dsqlScratch->appendUChar(blr_exec_stmt_in_params2);
			else
				dsqlScratch->appendUChar(blr_exec_stmt_in_params);

			NestConst<ValueExprNode>* ptr = inputs->items.begin();
			MetaName* const* name = inputNames ? inputNames->begin() : NULL;

			for (const NestConst<ValueExprNode>* end = inputs->items.end(); ptr != end; ++ptr, ++name)
			{
				if (inputNames)
					dsqlScratch->appendNullString((*name)->c_str());

				GEN_expr(dsqlScratch, *ptr);
			}
		}

		// Outputs.
		if (outputs)
		{
			dsqlScratch->appendUChar(blr_exec_stmt_out_params);

			for (FB_SIZE_T i = 0; i < outputs->items.getCount(); ++i)
				GEN_expr(dsqlScratch, outputs->items[i]);
		}

		dsqlScratch->appendUChar(blr_end);
	}
}

void ExecStatementNode::genOptionalExpr(DsqlCompilerScratch* dsqlScratch, const UCHAR code,
	ValueExprNode* node)
{
	if (node)
	{
		dsqlScratch->appendUChar(code);
		GEN_expr(dsqlScratch, node);
	}
}

ExecStatementNode* ExecStatementNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, sql.getAddress());
	doPass1(tdbb, csb, dataSource.getAddress());
	doPass1(tdbb, csb, userName.getAddress());
	doPass1(tdbb, csb, password.getAddress());
	doPass1(tdbb, csb, role.getAddress());
	doPass1(tdbb, csb, innerStmt.getAddress());
	doPass1(tdbb, csb, inputs.getAddress());
	doPass1(tdbb, csb, outputs.getAddress());
	return this;
}

ExecStatementNode* ExecStatementNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, sql.getAddress());
	ExprNode::doPass2(tdbb, csb, dataSource.getAddress());
	ExprNode::doPass2(tdbb, csb, userName.getAddress());
	ExprNode::doPass2(tdbb, csb, password.getAddress());
	ExprNode::doPass2(tdbb, csb, role.getAddress());
	doPass2(tdbb, csb, innerStmt.getAddress(), this);
	ExprNode::doPass2(tdbb, csb, inputs.getAddress());
	ExprNode::doPass2(tdbb, csb, outputs.getAddress());

	if (outputs)
	{
		for (const NestConst<ValueExprNode>* i = outputs->items.begin();
			 i != outputs->items.end();
			 ++i)
		{
			AssignmentNode::validateTarget(csb, *i);
		}
	}

	impureOffset = CMP_impure(csb, sizeof(EDS::Statement*));

	return this;
}

const StmtNode* ExecStatementNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	EDS::Statement** stmtPtr = request->getImpure<EDS::Statement*>(impureOffset);
	EDS::Statement* stmt = *stmtPtr;

	if (request->req_operation == jrd_req::req_evaluate)
	{
		fb_assert(!*stmtPtr);

		string sSql;
		getString(tdbb, request, sql, sSql, true);

		string sDataSrc;
		getString(tdbb, request, dataSource, sDataSrc);

		string sUser;
		getString(tdbb, request, userName, sUser);

		string sPwd;
		getString(tdbb, request, password, sPwd);

		string sRole;
		getString(tdbb, request, role, sRole);

		EDS::Connection* conn = EDS::Manager::getConnection(tdbb, sDataSrc, sUser, sPwd, sRole, traScope);

		stmt = conn->createStatement(sSql);
		stmt->bindToRequest(request, stmtPtr);
		stmt->setCallerPrivileges(useCallerPrivs);

		EDS::Transaction* tran = EDS::Transaction::getTransaction(tdbb, stmt->getConnection(), traScope);

		const MetaName* const* inpNames = inputNames ? inputNames->begin() : NULL;
		stmt->prepare(tdbb, tran, sSql, inputNames != NULL);

		const TimeoutTimer* timer = tdbb->getTimeoutTimer();
		if (timer)
			stmt->setTimeout(tdbb, timer->timeToExpire());

		if (stmt->isSelectable())
			stmt->open(tdbb, tran, inpNames, inputs, !innerStmt);
		else
			stmt->execute(tdbb, tran, inpNames, inputs, outputs);

		request->req_operation = jrd_req::req_return;
	}  // jrd_req::req_evaluate

	if (request->req_operation == jrd_req::req_return || request->req_operation == jrd_req::req_sync)
	{
		fb_assert(stmt);

		if (stmt->isSelectable())
		{
			if (stmt->fetch(tdbb, outputs))
			{
				request->req_operation = jrd_req::req_evaluate;
				return innerStmt;
			}

			request->req_operation = jrd_req::req_return;
		}
	}

	if (request->req_operation == jrd_req::req_unwind)
	{
		const LabelNode* label = StmtNode::as<LabelNode>(parentStmt.getObject());

		if (label && request->req_label == label->labelNumber &&
			(request->req_flags & req_continue_loop))
		{
			request->req_flags &= ~req_continue_loop;
			request->req_operation = jrd_req::req_sync;
			return this;
		}
	}

	if (stmt)
		stmt->close(tdbb);

	return parentStmt;
}

void ExecStatementNode::getString(thread_db* tdbb, jrd_req* request, const ValueExprNode* node,
	string& str, bool useAttCS) const
{
	MoveBuffer buffer;
	UCHAR* p = NULL;
	int len = 0;
	const dsc* dsc = node ? EVL_expr(tdbb, request, node) : NULL;

	if (dsc && !(request->req_flags & req_null))
	{
		const Jrd::Attachment* att = tdbb->getAttachment();
		len = MOV_make_string2(tdbb, dsc, (useAttCS ? att->att_charset : dsc->getTextType()),
			&p, buffer, false);
	}

	str.assign((char*) p, len);
	str.trim();
}


//--------------------


static RegisterNode<IfNode> regIfNode(blr_if);

DmlNode* IfNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	IfNode* node = FB_NEW_POOL(pool) IfNode(pool);

	node->condition = PAR_parse_boolean(tdbb, csb);
	node->trueAction = PAR_parse_stmt(tdbb, csb);

	if (csb->csb_blr_reader.peekByte() == blr_end)
		csb->csb_blr_reader.getByte(); // skip blr_end
	else
		node->falseAction = PAR_parse_stmt(tdbb, csb);

	return node;
}

IfNode* IfNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	IfNode* node = FB_NEW_POOL(dsqlScratch->getPool()) IfNode(dsqlScratch->getPool());
	node->condition = doDsqlPass(dsqlScratch, condition);
	node->trueAction = trueAction->dsqlPass(dsqlScratch);
	if (falseAction)
		node->falseAction = falseAction->dsqlPass(dsqlScratch);
	return node;
}

string IfNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, condition);
	NODE_PRINT(printer, trueAction);
	NODE_PRINT(printer, falseAction);

	return "IfNode";
}

void IfNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_if);
	GEN_expr(dsqlScratch, condition);
	trueAction->genBlr(dsqlScratch);

	if (falseAction)
		falseAction->genBlr(dsqlScratch);
	else
		dsqlScratch->appendUChar(blr_end);
}

IfNode* IfNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, condition.getAddress());
	doPass1(tdbb, csb, trueAction.getAddress());
	doPass1(tdbb, csb, falseAction.getAddress());
	return this;
}

IfNode* IfNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, condition.getAddress());
	doPass2(tdbb, csb, trueAction.getAddress(), this);
	doPass2(tdbb, csb, falseAction.getAddress(), this);
	return this;
}

const StmtNode* IfNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		if (condition->execute(tdbb, request))
		{
			request->req_operation = jrd_req::req_evaluate;
			return trueAction;
		}

		if (falseAction)
		{
			request->req_operation = jrd_req::req_evaluate;
			return falseAction;
		}

		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<InAutonomousTransactionNode> regInAutonomousTransactionNode(blr_auto_trans);

DmlNode* InAutonomousTransactionNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	const UCHAR /*blrOp*/)
{
	InAutonomousTransactionNode* node = FB_NEW_POOL(pool) InAutonomousTransactionNode(pool);

	if (csb->csb_blr_reader.getByte() != 0)	// Reserved for future improvements. Should be 0 for now.
		PAR_syntax_error(csb, "0");

	node->action = PAR_parse_stmt(tdbb, csb);

	return node;
}

InAutonomousTransactionNode* InAutonomousTransactionNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	const bool autoTrans = dsqlScratch->flags & DsqlCompilerScratch::FLAG_IN_AUTO_TRANS_BLOCK;
	dsqlScratch->flags |= DsqlCompilerScratch::FLAG_IN_AUTO_TRANS_BLOCK;

	InAutonomousTransactionNode* node = FB_NEW_POOL(dsqlScratch->getPool()) InAutonomousTransactionNode(
		dsqlScratch->getPool());
	node->action = action->dsqlPass(dsqlScratch);

	if (!autoTrans)
		dsqlScratch->flags &= ~DsqlCompilerScratch::FLAG_IN_AUTO_TRANS_BLOCK;

	return node;
}

string InAutonomousTransactionNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, action);
	NODE_PRINT(printer, impureOffset);

	return "InAutonomousTransactionNode";
}

void InAutonomousTransactionNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_auto_trans);
	dsqlScratch->appendUChar(0);	// to extend syntax in the future
	action->genBlr(dsqlScratch);
}

InAutonomousTransactionNode* InAutonomousTransactionNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, action.getAddress());
	return this;
}

InAutonomousTransactionNode* InAutonomousTransactionNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	impureOffset = CMP_impure(csb, sizeof(Impure));
	doPass2(tdbb, csb, action.getAddress(), this);
	return this;
}

const StmtNode* InAutonomousTransactionNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	Database* const dbb = tdbb->getDatabase();
	Jrd::Attachment* const attachment = tdbb->getAttachment();

	Impure* const impure = request->getImpure<Impure>(impureOffset);

	if (request->req_operation == jrd_req::req_evaluate)
	{
		// Force unconditional reschedule. It prevents new transactions being
		// started after an attachment or a database shutdown has been initiated.
		JRD_reschedule(tdbb, 0, true);

		jrd_tra* const org_transaction = request->req_transaction;
		fb_assert(tdbb->getTransaction() == org_transaction);

		jrd_tra* const transaction = TRA_start(tdbb, org_transaction->tra_flags,
											   org_transaction->tra_lock_timeout,
											   org_transaction);

		{
			AutoSetRestore2<jrd_req*, thread_db> autoNullifyRequest(
				tdbb, &thread_db::getRequest, &thread_db::setRequest, NULL);

			// run ON TRANSACTION START triggers
			JRD_run_trans_start_triggers(tdbb, transaction);
		}

		TRA_attach_request(transaction, request);
		tdbb->setTransaction(transaction);

		request->req_auto_trans.push(org_transaction);
		impure->traNumber = transaction->tra_number;

		const Savepoint* const savepoint = transaction->startSavepoint();
		impure->savNumber = savepoint->getNumber();

		return action;
	}

	jrd_tra* transaction = request->req_transaction;
	fb_assert(transaction && !(transaction->tra_flags & TRA_system));

	if (!impure->traNumber)
		return parentStmt;

	fb_assert(transaction->tra_number == impure->traNumber);

	switch (request->req_operation)
	{
	case jrd_req::req_return:
		if (!(attachment->att_flags & ATT_no_db_triggers))
		{
			// run ON TRANSACTION COMMIT triggers
			EXE_execute_db_triggers(tdbb, transaction, TRIGGER_TRANS_COMMIT);
		}

		if (transaction->tra_save_point &&
			transaction->tra_save_point->isSystem() &&
			transaction->tra_save_point->isChanging())
		{
			transaction->rollforwardSavepoint(tdbb);
		}

		{ // scope
			AutoSetRestore2<jrd_req*, thread_db> autoNullifyRequest(
				tdbb, &thread_db::getRequest, &thread_db::setRequest, NULL);
			TRA_commit(tdbb, transaction, false);
		} // end scope
		break;

	case jrd_req::req_unwind:
		if (request->req_flags & (req_leave | req_continue_loop))
		{
			try
			{
				if (!(attachment->att_flags & ATT_no_db_triggers))
				{
					// run ON TRANSACTION COMMIT triggers
					EXE_execute_db_triggers(tdbb, transaction, TRIGGER_TRANS_COMMIT);
				}

				if (transaction->tra_save_point &&
					transaction->tra_save_point->isSystem() &&
					transaction->tra_save_point->isChanging())
				{
					transaction->rollforwardSavepoint(tdbb);
				}

				AutoSetRestore2<jrd_req*, thread_db> autoNullifyRequest(
					tdbb, &thread_db::getRequest, &thread_db::setRequest, NULL);
				TRA_commit(tdbb, transaction, false);
			}
			catch (...)
			{
				request->req_flags &= ~(req_leave | req_continue_loop);
				throw;
			}
		}
		else
		{
			ThreadStatusGuard temp_status(tdbb);

			if (!(attachment->att_flags & ATT_no_db_triggers))
			{
				try
				{
					// run ON TRANSACTION ROLLBACK triggers
					EXE_execute_db_triggers(tdbb, transaction, TRIGGER_TRANS_ROLLBACK);
				}
				catch (const Exception&)
				{
					if (dbb->dbb_flags & DBB_bugcheck)
						throw;
				}
			}

			try
			{
				AutoSetRestore2<jrd_req*, thread_db> autoNullifyRequest(
					tdbb, &thread_db::getRequest, &thread_db::setRequest, NULL);

				TRA_rollback(tdbb, transaction, false, false);
			}
			catch (const Exception&)
			{
				if (dbb->dbb_flags & DBB_bugcheck)
					throw;
			}
		}
		break;

	default:
		fb_assert(false);
	}

	impure->traNumber = impure->savNumber = 0;
	transaction = request->req_auto_trans.pop();

	TRA_attach_request(transaction, request);
	tdbb->setTransaction(transaction);

	return parentStmt;
}


//--------------------


static RegisterNode<InitVariableNode> regInitVariableNode(blr_init_variable);

DmlNode* InitVariableNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	InitVariableNode* node = FB_NEW_POOL(pool) InitVariableNode(pool);
	node->varId = csb->csb_blr_reader.getWord();

	vec<DeclareVariableNode*>* vector = csb->csb_variables;

	if (!vector || node->varId >= vector->count())
		PAR_error(csb, Arg::Gds(isc_badvarnum));

	return node;
}

InitVariableNode* InitVariableNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

string InitVariableNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, varId);
	NODE_PRINT(printer, varDecl);
	NODE_PRINT(printer, varInfo);

	return "InitVariableNode";
}

void InitVariableNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}

InitVariableNode* InitVariableNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	InitVariableNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) InitVariableNode(*tdbb->getDefaultPool());
	node->varId = varId + copier.csb->csb_remap_variable;
	node->varDecl = varDecl;
	node->varInfo = varInfo;
	return node;
}

InitVariableNode* InitVariableNode::pass1(thread_db* /*tdbb*/, CompilerScratch* csb)
{
	vec<DeclareVariableNode*>* vector = csb->csb_variables;

	if (!vector || varId >= vector->count() || !(varDecl = (*vector)[varId]))
		PAR_error(csb, Arg::Gds(isc_badvarnum));

	return this;
}

InitVariableNode* InitVariableNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	varInfo = CMP_pass2_validation(tdbb, csb, Item(Item::TYPE_VARIABLE, varId));
	return this;
}

const StmtNode* InitVariableNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		if (varInfo)
		{
			dsc* toDesc = &request->getImpure<impure_value>(varDecl->impureOffset)->vlu_desc;
			toDesc->dsc_flags |= DSC_null;

			MapFieldInfo::ValueType fieldInfo;

			if (varInfo->fullDomain &&
				request->getStatement()->mapFieldInfo.get(varInfo->field, fieldInfo) &&
				fieldInfo.defaultValue)
			{
				dsc* value = EVL_expr(tdbb, request, fieldInfo.defaultValue);

				if (value && !(request->req_flags & req_null))
				{
					toDesc->dsc_flags &= ~DSC_null;
					MOV_move(tdbb, value, toDesc);
				}
			}
		}

		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


ExecBlockNode* ExecBlockNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	DsqlCompiledStatement* const statement = dsqlScratch->getStatement();

	if (returns.hasData())
		statement->setType(DsqlCompiledStatement::TYPE_SELECT_BLOCK);
	else
		statement->setType(DsqlCompiledStatement::TYPE_EXEC_BLOCK);

	dsqlScratch->flags |= DsqlCompilerScratch::FLAG_BLOCK;

	ExecBlockNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ExecBlockNode(dsqlScratch->getPool());

	for (NestConst<ParameterClause>* param = parameters.begin(); param != parameters.end(); ++param)
	{
		PsqlChanger changer(dsqlScratch, false);

		node->parameters.add(*param);
		ParameterClause* newParam = node->parameters.back();

		newParam->parameterExpr = doDsqlPass(dsqlScratch, newParam->parameterExpr);

		if (newParam->defaultClause)
			newParam->defaultClause->value = doDsqlPass(dsqlScratch, newParam->defaultClause->value);

		newParam->type->resolve(dsqlScratch);
		newParam->type->fld_id = param - parameters.begin();

		{ // scope
			ValueExprNode* temp = newParam->parameterExpr;

			// Initialize this stack variable, and make it look like a node
			dsc desc_node;

			newParam->type->flags |= FLD_nullable;
			MAKE_desc_from_field(&desc_node, newParam->type);
			PASS1_set_parameter_type(dsqlScratch, temp, &desc_node, false);
		} // end scope

		if (param != parameters.begin())
			node->parameters.end()[-2]->type->fld_next = newParam->type;
	}

	node->returns = returns;

	for (FB_SIZE_T i = 0; i < node->returns.getCount(); ++i)
	{
		node->returns[i]->type->resolve(dsqlScratch);
		node->returns[i]->type->fld_id = i;

		if (i != 0)
			node->returns[i - 1]->type->fld_next = node->returns[i]->type;
	}

	node->localDeclList = localDeclList;
	node->body = body;

	const FB_SIZE_T count = node->parameters.getCount() + node->returns.getCount() +
		(node->localDeclList ? node->localDeclList->statements.getCount() : 0);

	if (count != 0)
	{
		StrArray names(*getDefaultMemoryPool(), count);

		// Hand-made PASS1_check_unique_fields_names for arrays of ParameterClause

		Array<NestConst<ParameterClause> > params(parameters);
		params.add(returns.begin(), returns.getCount());

		for (FB_SIZE_T i = 0; i < params.getCount(); ++i)
		{
			ParameterClause* parameter = params[i];

			FB_SIZE_T pos;
			if (!names.find(parameter->name.c_str(), pos))
				names.insert(pos, parameter->name.c_str());
			else
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-637) <<
						  Arg::Gds(isc_dsql_duplicate_spec) << Arg::Str(parameter->name));
			}
		}

		PASS1_check_unique_fields_names(names, node->localDeclList);
	}

	return node;
}

string ExecBlockNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, parameters);
	NODE_PRINT(printer, returns);
	NODE_PRINT(printer, localDeclList);
	NODE_PRINT(printer, body);

	return "ExecBlockNode";
}

void ExecBlockNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	thread_db* tdbb = JRD_get_thread_data();

	dsqlScratch->beginDebug();

	// Sub routine needs a different approach from EXECUTE BLOCK.
	// EXECUTE BLOCK needs "ports", which creates DSQL messages using the client charset.
	// Sub routine doesn't need ports and should generate BLR as declared in its metadata.
	const bool subRoutine = dsqlScratch->flags & DsqlCompilerScratch::FLAG_SUB_ROUTINE;

	unsigned returnsPos;

	if (!subRoutine)
	{
		// Now do the input parameters.
		for (FB_SIZE_T i = 0; i < parameters.getCount(); ++i)
		{
			ParameterClause* parameter = parameters[i];

			dsqlScratch->makeVariable(parameter->type, parameter->name.c_str(),
				dsql_var::TYPE_INPUT, 0, (USHORT) (2 * i), i);
		}

		returnsPos = dsqlScratch->variables.getCount();

		// Now do the output parameters.
		for (FB_SIZE_T i = 0; i < returns.getCount(); ++i)
		{
			ParameterClause* parameter = returns[i];

			dsqlScratch->makeVariable(parameter->type, parameter->name.c_str(),
				dsql_var::TYPE_OUTPUT, 1, (USHORT) (2 * i), parameters.getCount() + i);
		}
	}

	DsqlCompiledStatement* const statement = dsqlScratch->getStatement();

	dsqlScratch->appendUChar(blr_begin);

	if (parameters.hasData())
	{
		revertParametersOrder(statement->getSendMsg()->msg_parameters);
		if (!subRoutine)
			GEN_port(dsqlScratch, statement->getSendMsg());
	}
	else
		statement->setSendMsg(NULL);

	for (Array<dsql_var*>::const_iterator i = dsqlScratch->outputVariables.begin();
		 i != dsqlScratch->outputVariables.end();
		 ++i)
	{
		VariableNode* varNode = FB_NEW_POOL(*tdbb->getDefaultPool()) VariableNode(*tdbb->getDefaultPool());
		varNode->dsqlVar = *i;

		dsql_par* param = MAKE_parameter(statement->getReceiveMsg(), true, true,
			(i - dsqlScratch->outputVariables.begin()) + 1, varNode);
		param->par_node = varNode;
		MAKE_desc(dsqlScratch, &param->par_desc, varNode);
		param->par_desc.dsc_flags |= DSC_nullable;
	}

	// Set up parameter to handle EOF
	dsql_par* param = MAKE_parameter(statement->getReceiveMsg(), false, false, 0, NULL);
	statement->setEof(param);
	param->par_desc.dsc_dtype = dtype_short;
	param->par_desc.dsc_scale = 0;
	param->par_desc.dsc_length = sizeof(SSHORT);

	revertParametersOrder(statement->getReceiveMsg()->msg_parameters);
	if (!subRoutine)
		GEN_port(dsqlScratch, statement->getReceiveMsg());

	if (subRoutine)
	{
		dsqlScratch->genParameters(parameters, returns);
		returnsPos = dsqlScratch->variables.getCount() - dsqlScratch->outputVariables.getCount();
	}

	if (parameters.hasData())
	{
		dsqlScratch->appendUChar(blr_receive);
		dsqlScratch->appendUChar(0);
	}

	dsqlScratch->appendUChar(blr_begin);

	if (subRoutine)
	{
		// This validation is needed only for subroutines. Standard EXECUTE BLOCK moves input
		// parameters to variables and are then validated.

		for (unsigned i = 0; i < returnsPos; ++i)
		{
			const dsql_var* variable = dsqlScratch->variables[i];
			const TypeClause* field = variable->field;

			if (field->fullDomain || field->notNull)
			{
				dsqlScratch->appendUChar(blr_assignment);
				dsqlScratch->appendUChar(blr_parameter2);
				dsqlScratch->appendUChar(0);
				dsqlScratch->appendUShort(variable->msgItem);
				dsqlScratch->appendUShort(variable->msgItem + 1);
				dsqlScratch->appendUChar(blr_null);
			}
		}
	}

	Array<dsql_var*>& variables = subRoutine ? dsqlScratch->outputVariables : dsqlScratch->variables;

	for (Array<dsql_var*>::const_iterator i = variables.begin(); i != variables.end(); ++i)
		dsqlScratch->putLocalVariable(*i, 0, NULL);

	dsqlScratch->setPsql(true);

	dsqlScratch->putLocalVariables(localDeclList,
		USHORT((subRoutine ? 0 : parameters.getCount()) + returns.getCount()));

	dsqlScratch->loopLevel = 0;

	StmtNode* stmtNode = body->dsqlPass(dsqlScratch);
	GEN_hidden_variables(dsqlScratch);

	dsqlScratch->appendUChar(blr_stall);
	// Put a label before body of procedure, so that
	// any exit statement can get out
	dsqlScratch->appendUChar(blr_label);
	dsqlScratch->appendUChar(0);

	stmtNode->genBlr(dsqlScratch);

	if (returns.hasData())
		statement->setType(DsqlCompiledStatement::TYPE_SELECT_BLOCK);
	else
		statement->setType(DsqlCompiledStatement::TYPE_EXEC_BLOCK);

	dsqlScratch->appendUChar(blr_end);
	dsqlScratch->genReturn(true);
	dsqlScratch->appendUChar(blr_end);

	dsqlScratch->endDebug();
}

// Revert parameters order for EXECUTE BLOCK statement
void ExecBlockNode::revertParametersOrder(Array<dsql_par*>& parameters)
{
	int start = 0;
	int end = int(parameters.getCount()) - 1;

	while (start < end)
	{
		dsql_par* temp = parameters[start];
		parameters[start] = parameters[end];
		parameters[end] = temp;
		++start;
		--end;
	}
}


//--------------------


static RegisterNode<ExceptionNode> regExceptionNode(blr_abort);

DmlNode* ExceptionNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
	const UCHAR /*blrOp*/)
{
	ExceptionNode* node = FB_NEW_POOL(pool) ExceptionNode(pool);
	const UCHAR type = csb->csb_blr_reader.peekByte();
	const USHORT codeType = csb->csb_blr_reader.getByte();

	// Don't create ExceptionItem if blr_raise is used.
	if (codeType != blr_raise)
	{
		ExceptionItem* const item = FB_NEW_POOL(pool) ExceptionItem(pool);

		switch (codeType)
		{
			case blr_gds_code:
				item->type = ExceptionItem::GDS_CODE;
				csb->csb_blr_reader.getString(item->name);
				item->name.lower();
				if (!(item->code = PAR_symbol_to_gdscode(item->name)))
					PAR_error(csb, Arg::Gds(isc_codnotdef) << item->name);
				break;

			case blr_exception:
			case blr_exception_msg:
			case blr_exception_params:
				{
					csb->csb_blr_reader.getString(item->name);
					if (!MET_load_exception(tdbb, *item))
						PAR_error(csb, Arg::Gds(isc_xcpnotdef) << item->name);

					CompilerScratch::Dependency dependency(obj_exception);
					dependency.number = item->code;
					csb->csb_dependencies.push(dependency);
				}
				break;

			default:
				fb_assert(false);
				break;
		}

		node->exception = item;
	}

	if (type == blr_exception_params)
	{
		const USHORT count = csb->csb_blr_reader.getWord();
		node->parameters = PAR_args(tdbb, csb, count, count);
	}
	else if (type == blr_exception_msg)
		node->messageExpr = PAR_parse_value(tdbb, csb);

	return node;
}

StmtNode* ExceptionNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (parameters && parameters->items.getCount() > MsgFormat::SAFEARG_MAX_ARG)
	{
		status_exception::raise(
			Arg::Gds(isc_dsql_max_exception_arguments) <<
				Arg::Num(parameters->items.getCount()) <<
				Arg::Num(MsgFormat::SAFEARG_MAX_ARG));
	}

	ExceptionNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ExceptionNode(dsqlScratch->getPool());
	if (exception)
		node->exception = FB_NEW_POOL(dsqlScratch->getPool()) ExceptionItem(dsqlScratch->getPool(), *exception);
	node->messageExpr = doDsqlPass(dsqlScratch, messageExpr);
	node->parameters = doDsqlPass(dsqlScratch, parameters);

	return SavepointEncloseNode::make(dsqlScratch->getPool(), dsqlScratch, node);
}

string ExceptionNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, messageExpr);
	NODE_PRINT(printer, parameters);
	NODE_PRINT(printer, exception);

	return "ExceptionNode";
}

void ExceptionNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_abort);

	// If exception is NULL, it means we have re-initiate semantics here,
	// so blr_raise verb should be generated.
	if (!exception)
	{
		dsqlScratch->appendUChar(blr_raise);
		return;
	}

	// If exception value is defined, it means we have user-defined exception message
	// here, so blr_exception_msg verb should be generated.
	if (parameters)
		dsqlScratch->appendUChar(blr_exception_params);
	else if (messageExpr)
		dsqlScratch->appendUChar(blr_exception_msg);
	else if (exception->type == ExceptionItem::GDS_CODE)
		dsqlScratch->appendUChar(blr_gds_code);
	else	// Otherwise go usual way, i.e. generate blr_exception.
		dsqlScratch->appendUChar(blr_exception);

	dsqlScratch->appendNullString(exception->name.c_str());

	// If exception parameters or value is defined, generate appropriate BLR verbs.
	if (parameters)
	{
		dsqlScratch->appendUShort(parameters->items.getCount());

		NestConst<ValueExprNode>* ptr = parameters->items.begin();
		const NestConst<ValueExprNode>* end = parameters->items.end();
		while (ptr < end)
			GEN_expr(dsqlScratch, *ptr++);
	}
	else if (messageExpr)
		GEN_expr(dsqlScratch, messageExpr);
}

ExceptionNode* ExceptionNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, messageExpr.getAddress());
	doPass1(tdbb, csb, parameters.getAddress());

	if (exception)
	{
		CMP_post_access(tdbb, csb, exception->secName, 0,
						SCL_usage, SCL_object_exception, exception->name);
	}

	return this;
}

ExceptionNode* ExceptionNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, messageExpr.getAddress());
	ExprNode::doPass2(tdbb, csb, parameters.getAddress());
	return this;
}

const StmtNode* ExceptionNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		if (exception)
		{
			// PsqlException is defined, so throw an exception.
			setError(tdbb);
		}
		else if (!request->req_last_xcp.success())
		{
			// PsqlException is undefined, but there was a known exception before,
			// so re-initiate it.
			setError(tdbb);
		}
		else
		{
			// PsqlException is undefined and there weren't any exceptions before,
			// so just do nothing.
			request->req_operation = jrd_req::req_return;
		}
	}

	return parentStmt;
}

// Set status vector according to specified error condition and jump to handle error accordingly.
void ExceptionNode::setError(thread_db* tdbb) const
{
	SET_TDBB(tdbb);

	jrd_req* request = tdbb->getRequest();

	if (!exception)
	{
		// Retrieve the status vector and punt.
		request->req_last_xcp.copyTo(tdbb->tdbb_status_vector);
		request->req_last_xcp.clear();
		ERR_punt();
	}

	MetaName exName;
	MetaName relationName;
	string message;

	if (messageExpr)
	{
		// Evaluate exception message and convert it to string.
		const dsc* const desc = EVL_expr(tdbb, request, messageExpr);

		if (desc && !(request->req_flags & req_null))
		{
			MoveBuffer temp;
			UCHAR* string = NULL;
			const USHORT length = MOV_make_string2(tdbb, desc, CS_METADATA, &string, temp);
			message.assign(string, MIN(length, XCP_MESSAGE_LENGTH));
		}
	}

	const SLONG xcpCode = exception->code;

	switch (exception->type)
	{
		case ExceptionItem::GDS_CODE:
			if (xcpCode == isc_check_constraint)
			{
				MET_lookup_cnstrt_for_trigger(tdbb, exName, relationName,
					request->getStatement()->triggerName);
				ERR_post(Arg::Gds(xcpCode) << Arg::Str(exName) << Arg::Str(relationName));
			}
			else
				ERR_post(Arg::Gds(xcpCode));

		case ExceptionItem::XCP_CODE:
		{
			string tempStr;
			const TEXT* s;

			// CVC: If we have the exception name, use it instead of the number.
			// Solves SF Bug #494981.
			MET_lookup_exception(tdbb, xcpCode, exName, &tempStr);

			if (message.hasData())
				s = message.c_str();
			else if (tempStr.hasData())
				s = tempStr.c_str();
			else
				s = NULL;

			Arg::StatusVector status;
			ISC_STATUS msgCode = parameters ? isc_formatted_exception : isc_random;

			if (s && exName.hasData())
			{
				status << Arg::Gds(isc_except) << Arg::Num(xcpCode) <<
						  Arg::Gds(isc_random) << Arg::Str(exName) <<
						  Arg::Gds(msgCode);
			}
			else if (s)
				status << Arg::Gds(isc_except) << Arg::Num(xcpCode) <<
						  Arg::Gds(msgCode);
			else if (exName.hasData())
			{
				ERR_post(Arg::Gds(isc_except) << Arg::Num(xcpCode) <<
						 Arg::Gds(isc_random) << Arg::Str(exName));
			}
			else
				ERR_post(Arg::Gds(isc_except) << Arg::Num(xcpCode));

			// Preallocate objects, because Arg::StatusVector store pointers.
			string formattedMsg;
			ObjectsArray<string> paramsStr;

			if (parameters)
			{
				for (const NestConst<ValueExprNode>* parameter = parameters->items.begin();
					 parameter != parameters->items.end(); ++parameter)
				{
					const dsc* value = EVL_expr(tdbb, request, *parameter);

					if (!value || (request->req_flags & req_null))
						paramsStr.push(NULL_STRING_MARK);
					else
					{
						// Usage of NONE here should be reviewed when exceptions are stored using
						// the metadata character set.
						paramsStr.push(MOV_make_string2(tdbb, value, ttype_none));
					}
				}

				// And add the values to the args and status vector only after they are all created
				// and will not move in paramsStr.

				MsgFormat::SafeArg arg;
				for (FB_SIZE_T i = 0; i < parameters->items.getCount(); ++i)
					arg << paramsStr[i].c_str();

				MsgFormat::StringRefStream stream(formattedMsg);
				MsgFormat::MsgPrint(stream, s, arg, true);

				status << formattedMsg;

				for (FB_SIZE_T i = 0; i < parameters->items.getCount(); ++i)
					status << paramsStr[i];
			}
			else
				status << s;	// add the exception text

			ERR_post(status);
		}

		default:
			fb_assert(false);
	}
}


//--------------------


string ExitNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);
	return "ExitNode";
}

void ExitNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_leave);
	dsqlScratch->appendUChar(0);
}


//--------------------


static RegisterNode<ForNode> regForNode(blr_for);

DmlNode* ForNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	ForNode* node = FB_NEW_POOL(pool) ForNode(pool);

	if (csb->csb_blr_reader.peekByte() == (UCHAR) blr_stall)
		node->stall = PAR_parse_stmt(tdbb, csb);

	AutoSetRestore<ForNode*> autoCurrentForNode(&csb->csb_currentForNode, node);

	if (csb->csb_blr_reader.peekByte() == (UCHAR) blr_rse ||
		csb->csb_blr_reader.peekByte() == (UCHAR) blr_singular ||
		csb->csb_blr_reader.peekByte() == (UCHAR) blr_scrollable)
	{
		node->rse = PAR_rse(tdbb, csb);
	}
	else
		node->rse = PAR_rse(tdbb, csb, blrOp);

	fb_assert(node->parBlrBeginCnt == 0);

	node->statement = PAR_parse_stmt(tdbb, csb);

	return node;
}

ForNode* ForNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	ForNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ForNode(dsqlScratch->getPool());

	node->dsqlCursor = dsqlCursor;

	const DsqlContextStack::iterator base(*dsqlScratch->context);

	if (dsqlCursor)
	{
		fb_assert(dsqlCursor->dsqlCursorType != DeclareCursorNode::CUR_TYPE_NONE);
		PASS1_cursor_name(dsqlScratch, dsqlCursor->dsqlName, DeclareCursorNode::CUR_TYPE_ALL, false);

		SelectExprNode* dt = FB_NEW_POOL(dsqlScratch->getPool()) SelectExprNode(dsqlScratch->getPool());
		dt->dsqlFlags = RecordSourceNode::DFLAG_DERIVED | RecordSourceNode::DFLAG_CURSOR;
		dt->querySpec = dsqlSelect->dsqlExpr;
		dt->alias = dsqlCursor->dsqlName.c_str();

		node->rse = PASS1_derived_table(dsqlScratch, dt, NULL, dsqlSelect->dsqlWithLock);

		dsqlCursor->rse = node->rse;
		dsqlCursor->cursorNumber = dsqlScratch->cursorNumber++;
		dsqlScratch->cursors.push(dsqlCursor);

		// ASF: We cannot write this cursor name in debug info, as dsqlScratch->cursorNumber is
		// decremented below. But for now we don't need it.
	}
	else
		node->rse = dsqlSelect->dsqlPass(dsqlScratch)->dsqlRse;

	node->dsqlInto = dsqlPassArray(dsqlScratch, dsqlInto);

	if (statement)
	{
		++dsqlScratch->scopeLevel;

		// CVC: Let's add the ability to BREAK the for_select same as the while,
		// but only if the command is FOR SELECT, otherwise we have singular SELECT
		++dsqlScratch->loopLevel;
		node->dsqlLabelNumber = dsqlPassLabel(dsqlScratch, false, dsqlLabelName);
		node->statement = statement->dsqlPass(dsqlScratch);
		--dsqlScratch->loopLevel;
		dsqlScratch->labels.pop();

		--dsqlScratch->scopeLevel;
	}

	dsqlScratch->context->clear(base);

	if (dsqlCursor)
	{
		dsqlScratch->cursorNumber--;
		dsqlScratch->cursors.pop();
	}

	return node;
}

string ForNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlSelect);
	NODE_PRINT(printer, dsqlInto);
	NODE_PRINT(printer, dsqlCursor);
	NODE_PRINT(printer, dsqlLabelName);
	NODE_PRINT(printer, dsqlLabelNumber);
	NODE_PRINT(printer, dsqlForceSingular);
	NODE_PRINT(printer, stall);
	NODE_PRINT(printer, rse);
	NODE_PRINT(printer, statement);
	NODE_PRINT(printer, cursor);
	NODE_PRINT(printer, parBlrBeginCnt);

	return "ForNode";
}

void ForNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	// CVC: Only put a label if this is not singular; otherwise,
	// what loop is the user trying to abandon?
	if (statement)
	{
		dsqlScratch->appendUChar(blr_label);
		dsqlScratch->appendUChar(dsqlLabelNumber);
	}

	// Generate FOR loop

	dsqlScratch->appendUChar(blr_for);

	if (!statement || dsqlForceSingular)
		dsqlScratch->appendUChar(blr_singular);

	GEN_rse(dsqlScratch, rse);
	dsqlScratch->appendUChar(blr_begin);

	// Build body of FOR loop

	ValueListNode* list = rse->dsqlSelectList;

	if (dsqlInto)
	{
		if (list->items.getCount() != dsqlInto->items.getCount())
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-313) <<
					  Arg::Gds(isc_dsql_count_mismatch));
		}

		NestConst<ValueExprNode>* ptr = list->items.begin();
		NestConst<ValueExprNode>* ptr_to = dsqlInto->items.begin();

		for (const NestConst<ValueExprNode>* const end = list->items.end(); ptr != end; ++ptr, ++ptr_to)
		{
			dsqlScratch->appendUChar(blr_assignment);
			GEN_expr(dsqlScratch, *ptr);
			GEN_expr(dsqlScratch, *ptr_to);
		}
	}

	if (statement)
		statement->genBlr(dsqlScratch);

	dsqlScratch->appendUChar(blr_end);
}

StmtNode* ForNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, stall.getAddress());
	doPass1(tdbb, csb, rse.getAddress());
	doPass1(tdbb, csb, statement.getAddress());
	return this;
}

StmtNode* ForNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	rse->pass2Rse(tdbb, csb);

	doPass2(tdbb, csb, stall.getAddress(), this);
	ExprNode::doPass2(tdbb, csb, rse.getAddress());
	doPass2(tdbb, csb, statement.getAddress(), this);

	// Finish up processing of record selection expressions.

	RecordSource* const rsb = CMP_post_rse(tdbb, csb, rse.getObject());
	csb->csb_fors.add(rsb);

	cursor = FB_NEW_POOL(*tdbb->getDefaultPool()) Cursor(csb, rsb, rse->rse_invariants,
		(rse->flags & RseNode::FLAG_SCROLLABLE));
	// ASF: We cannot define the name of the cursor here, but this is not a problem,
	// as implicit cursors are always positioned in a valid record, and the name is
	// only used to raise isc_cursor_not_positioned.

	impureOffset = CMP_impure(csb, sizeof(SavNumber));

	return this;
}

const StmtNode* ForNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	jrd_tra* transaction = request->req_transaction;

	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
			*request->getImpure<SavNumber>(impureOffset) = 0;
			if (!(transaction->tra_flags & TRA_system) &&
				transaction->tra_save_point &&
				transaction->tra_save_point->hasChanges())
			{
				const Savepoint* const savepoint = transaction->startSavepoint();
				*request->getImpure<SavNumber>(impureOffset) = savepoint->getNumber();
			}
			cursor->open(tdbb);
			request->req_records_affected.clear();
			// fall into

		case jrd_req::req_return:
			if (stall)
				return stall;
			// fall into

		case jrd_req::req_sync:
			if (cursor->fetchNext(tdbb))
			{
				request->req_operation = jrd_req::req_evaluate;
				return statement;
			}
			request->req_operation = jrd_req::req_return;
			// fall into

		case jrd_req::req_unwind:
		{
			const LabelNode* label = StmtNode::as<LabelNode>(parentStmt.getObject());

			if (label && request->req_label == label->labelNumber &&
				(request->req_flags & req_continue_loop))
			{
				request->req_flags &= ~req_continue_loop;
				request->req_operation = jrd_req::req_sync;
				return this;
			}

			// fall into
		}

		default:
		{
			const SavNumber savNumber = *request->getImpure<SavNumber>(impureOffset);

			if (savNumber)
			{
				while (transaction->tra_save_point &&
					transaction->tra_save_point->getNumber() >= savNumber)
				{
					if (transaction->tra_save_point->isChanging()) // we must rollback this savepoint
						transaction->rollbackSavepoint(tdbb);
					else
						transaction->rollforwardSavepoint(tdbb);
				}
			}

			cursor->close(tdbb);
			return parentStmt;
		}
	}

	fb_assert(false); // unreachable code
	return NULL;
}


//--------------------


static RegisterNode<HandlerNode> regHandlerNode(blr_handler);

DmlNode* HandlerNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	HandlerNode* node = FB_NEW_POOL(pool) HandlerNode(pool);
	node->statement = PAR_parse_stmt(tdbb, csb);
	return node;
}

HandlerNode* HandlerNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

string HandlerNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, statement);

	return "HandlerNode";
}

void HandlerNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}

HandlerNode* HandlerNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, statement.getAddress());
	return this;
}

HandlerNode* HandlerNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	doPass2(tdbb, csb, statement.getAddress(), this);
	return this;
}

const StmtNode* HandlerNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
			return statement;

		case jrd_req::req_unwind:
			if (!request->req_label)
				request->req_operation = jrd_req::req_return;

		default:
			return parentStmt;
	}
}


//--------------------


static RegisterNode<LabelNode> regLabelNode(blr_label);

DmlNode* LabelNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	LabelNode* node = FB_NEW_POOL(pool) LabelNode(pool);

	node->labelNumber = csb->csb_blr_reader.getByte();
	node->statement = PAR_parse_stmt(tdbb, csb);

	return node;
}

LabelNode* LabelNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

string LabelNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, statement);
	NODE_PRINT(printer, labelNumber);

	return "LabelNode";
}

void LabelNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}

LabelNode* LabelNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, statement.getAddress());
	return this;
}

LabelNode* LabelNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	doPass2(tdbb, csb, statement.getAddress(), this);
	return this;
}

const StmtNode* LabelNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
			return statement;

		case jrd_req::req_unwind:
			fb_assert(!(request->req_flags & req_continue_loop));

			if (request->req_label == labelNumber &&
				(request->req_flags & (req_leave | req_error_handler)))
			{
				request->req_flags &= ~req_leave;
				request->req_operation = jrd_req::req_return;
			}
			// fall into

		default:
			return parentStmt;
	}
}


//--------------------


string LineColumnNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, statement);

	return "LineColumnNode";
}

LineColumnNode* LineColumnNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	statement = statement->dsqlPass(dsqlScratch);
	return this;
}

void LineColumnNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->putDebugSrcInfo(line, column);
	statement->genBlr(dsqlScratch);
}


//--------------------


static RegisterNode<LoopNode> regLoopNode(blr_loop);

DmlNode* LoopNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	LoopNode* node = FB_NEW_POOL(pool) LoopNode(pool);
	node->statement = PAR_parse_stmt(tdbb, csb);
	return node;
}

LoopNode* LoopNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	LoopNode* node = FB_NEW_POOL(dsqlScratch->getPool()) LoopNode(dsqlScratch->getPool());

	node->dsqlExpr = doDsqlPass(dsqlScratch, dsqlExpr);

	// CVC: Loop numbers should be incremented before analyzing the body
	// to preserve nesting <==> increasing level number.
	++dsqlScratch->loopLevel;
	node->dsqlLabelNumber = dsqlPassLabel(dsqlScratch, false, dsqlLabelName);
	node->statement = statement->dsqlPass(dsqlScratch);
	--dsqlScratch->loopLevel;
	dsqlScratch->labels.pop();

	return node;
}

string LoopNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlLabelName);
	NODE_PRINT(printer, dsqlLabelNumber);
	NODE_PRINT(printer, dsqlExpr);
	NODE_PRINT(printer, statement);

	return "LoopNode";
}

void LoopNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_label);
	fb_assert(dsqlLabelNumber < MAX_UCHAR);
	dsqlScratch->appendUChar((UCHAR) dsqlLabelNumber);
	dsqlScratch->appendUChar(blr_loop);
	dsqlScratch->appendUChar(blr_begin);
	dsqlScratch->appendUChar(blr_if);
	GEN_expr(dsqlScratch, dsqlExpr);
	statement->genBlr(dsqlScratch);
	dsqlScratch->appendUChar(blr_leave);
	dsqlScratch->appendUChar((UCHAR) dsqlLabelNumber);
	dsqlScratch->appendUChar(blr_end);
}

LoopNode* LoopNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, statement.getAddress());
	return this;
}

LoopNode* LoopNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	doPass2(tdbb, csb, statement.getAddress(), this);
	return this;
}

const StmtNode* LoopNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
		case jrd_req::req_return:
			request->req_operation = jrd_req::req_evaluate;
			return statement;

		case jrd_req::req_unwind:
		{
			const LabelNode* label = StmtNode::as<LabelNode>(parentStmt.getObject());

			if (label && request->req_label == label->labelNumber &&
				(request->req_flags & req_continue_loop))
			{
				request->req_flags &= ~req_continue_loop;
				request->req_operation = jrd_req::req_evaluate;
				return statement;
			}
			// fall into
		}

		default:
			return parentStmt;
	}
}


//--------------------


StmtNode* MergeNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	// Puts a blr_send before blr_for in DSQL statements.
	class MergeSendNode : public TypedNode<DsqlOnlyStmtNode, StmtNode::TYPE_MERGE_SEND>
	{
	public:
		MergeSendNode(MemoryPool& pool, StmtNode* aStmt)
			: TypedNode<DsqlOnlyStmtNode, StmtNode::TYPE_MERGE_SEND>(pool),
			  stmt(aStmt)
		{
		}

	public:
		virtual string internalPrint(NodePrinter& printer) const
		{
			DsqlOnlyStmtNode::internalPrint(printer);

			NODE_PRINT(printer, stmt);

			return "MergeSendNode";
		}

		// Do not make dsqlPass to process 'stmt'. It's already processed.

		virtual void genBlr(DsqlCompilerScratch* dsqlScratch)
		{
			dsql_msg* message = dsqlScratch->getStatement()->getReceiveMsg();

			if (!dsqlScratch->isPsql() && message)
			{
				dsqlScratch->appendUChar(blr_send);
				dsqlScratch->appendUChar(message->msg_number);
			}

			stmt->genBlr(dsqlScratch);
		}

	private:
		StmtNode* stmt;
	};

	thread_db* tdbb = JRD_get_thread_data();
	MemoryPool& pool = *tdbb->getDefaultPool();

	RecordSourceNode* source = usingClause;		// USING
	RelationSourceNode* target = relation;		// INTO

	// Build a join between USING and INTO tables.
	RseNode* join = FB_NEW_POOL(pool) RseNode(pool);
	join->dsqlExplicitJoin = true;
	join->dsqlFrom = FB_NEW_POOL(pool) RecSourceListNode(pool, 2);

	join->dsqlFrom->items[0] = source;

	// Left join if WHEN NOT MATCHED is present.
	if (whenNotMatched.hasData())
		join->rse_jointype = blr_left;

	join->dsqlFrom->items[1] = target;
	join->dsqlWhere = condition;

	RseNode* querySpec = FB_NEW_POOL(pool) RseNode(pool);
	querySpec->dsqlFrom = FB_NEW_POOL(pool) RecSourceListNode(pool, 1);
	querySpec->dsqlFrom->items[0] = join;

	BoolExprNode* matchedConditions = NULL;
	BoolExprNode* notMatchedConditions = NULL;

	for (ObjectsArray<Matched>::iterator matched = whenMatched.begin();
		 matched != whenMatched.end();
		 ++matched)
	{
		if (matched->condition)
			matchedConditions = PASS1_compose(matchedConditions, matched->condition, blr_or);
		else
		{
			matchedConditions = NULL;
			break;
		}
	}

	for (ObjectsArray<NotMatched>::iterator notMatched = whenNotMatched.begin();
		 notMatched != whenNotMatched.end();
		 ++notMatched)
	{
		if (notMatched->condition)
			notMatchedConditions = PASS1_compose(notMatchedConditions, notMatched->condition, blr_or);
		else
		{
			notMatchedConditions = NULL;
			break;
		}
	}

	if (matchedConditions || notMatchedConditions)
	{
		const char* targetName = target->alias.nullStr();
		if (!targetName)
			targetName = target->dsqlName.c_str();

		if (whenMatched.hasData())	// WHEN MATCHED
		{
			MissingBoolNode* missingNode = FB_NEW_POOL(pool) MissingBoolNode(
				pool, FB_NEW_POOL(pool) RecordKeyNode(pool, blr_dbkey, targetName));

			querySpec->dsqlWhere = FB_NEW_POOL(pool) NotBoolNode(pool, missingNode);

			if (matchedConditions)
				querySpec->dsqlWhere = PASS1_compose(querySpec->dsqlWhere, matchedConditions, blr_and);
		}

		if (whenNotMatched.hasData())	// WHEN NOT MATCHED
		{
			BoolExprNode* temp = FB_NEW_POOL(pool) MissingBoolNode(pool,
				FB_NEW_POOL(pool) RecordKeyNode(pool, blr_dbkey, targetName));

			if (notMatchedConditions)
				temp = PASS1_compose(temp, notMatchedConditions, blr_and);

			querySpec->dsqlWhere = PASS1_compose(querySpec->dsqlWhere, temp, blr_or);
		}
	}

	SelectExprNode* select_expr = FB_NEW_POOL(dsqlScratch->getPool()) SelectExprNode(dsqlScratch->getPool());
	select_expr->querySpec = querySpec;

	// build a FOR SELECT node
	ForNode* forNode = FB_NEW_POOL(pool) ForNode(pool);
	forNode->dsqlSelect = FB_NEW_POOL(pool) SelectNode(pool);
	forNode->dsqlSelect->dsqlExpr = select_expr;
	forNode->statement = FB_NEW_POOL(pool) CompoundStmtNode(pool);

	forNode = forNode->dsqlPass(dsqlScratch);

	if (returning)
		forNode->dsqlForceSingular = true;

	// Get the already processed relations.
	RseNode* processedRse = nodeAs<RseNode>(forNode->rse->dsqlStreams->items[0]);
	source = processedRse->dsqlStreams->items[0];
	target = nodeAs<RelationSourceNode>(processedRse->dsqlStreams->items[1]);

	DsqlContextStack usingCtxs;
	dsqlGetContexts(usingCtxs, source);

	StmtNode* processedRet = NULL;
	StmtNode* nullRet = NULL;

	StmtNode* update = NULL;
	IfNode* lastIf = NULL;

	for (ObjectsArray<Matched>::iterator matched = whenMatched.begin();
		 matched != whenMatched.end();
		 ++matched)
	{
		IfNode* thisIf = FB_NEW_POOL(pool) IfNode(pool);

		if (matched->assignments)
		{
			// Get the assignments of the UPDATE dsqlScratch.
			CompoundStmtNode* stmts = matched->assignments;
			Array<NestConst<ValueExprNode> > orgValues, newValues;

			// Separate the new and org values to process in correct contexts.
			for (FB_SIZE_T i = 0; i < stmts->statements.getCount(); ++i)
			{
				AssignmentNode* const assign = nodeAs<AssignmentNode>(stmts->statements[i]);
				fb_assert(assign);
				orgValues.add(assign->asgnFrom);
				newValues.add(assign->asgnTo);
			}

			// Build the MODIFY node.
			ModifyNode* modify = FB_NEW_POOL(pool) ModifyNode(pool);
			thisIf->trueAction = modify;

			dsql_ctx* const oldContext = dsqlGetContext(target);

			modify->dsqlContext = oldContext;

			++dsqlScratch->scopeLevel;	// Go to the same level of source and target contexts.

			for (DsqlContextStack::iterator itr(usingCtxs); itr.hasData(); ++itr)
				dsqlScratch->context->push(itr.object());	// push the USING contexts

			dsqlScratch->context->push(oldContext);	// process old context values

			if (matched->condition)
				thisIf->condition = doDsqlPass(dsqlScratch, matched->condition, false);

			NestConst<ValueExprNode>* ptr;

			for (ptr = orgValues.begin(); ptr != orgValues.end(); ++ptr)
				*ptr = doDsqlPass(dsqlScratch, *ptr, false);

			// And pop the contexts.
			dsqlScratch->context->pop();
			dsqlScratch->context->pop();
			--dsqlScratch->scopeLevel;

			// Process relation.
			modify->dsqlRelation = PASS1_relation(dsqlScratch, relation);
			dsql_ctx* modContext = dsqlGetContext(modify->dsqlRelation);

			// Process new context values.
			for (ptr = newValues.begin(); ptr != newValues.end(); ++ptr)
				*ptr = doDsqlPass(dsqlScratch, *ptr, false);

			dsqlScratch->context->pop();

			if (returning)
			{
				StmtNode* updRet = ReturningProcessor::clone(dsqlScratch, returning, processedRet);

				// Repush the source contexts.
				++dsqlScratch->scopeLevel;	// Go to the same level of source and target contexts.

				for (DsqlContextStack::iterator itr(usingCtxs); itr.hasData(); ++itr)
					dsqlScratch->context->push(itr.object());	// push the USING contexts

				dsqlScratch->context->push(oldContext);	// process old context values

				modContext->ctx_scope_level = oldContext->ctx_scope_level;

				processedRet = modify->statement2 = ReturningProcessor(
					dsqlScratch, oldContext, modContext).process(returning, updRet);

				if (!nullRet)
					nullRet = dsqlNullifyReturning(dsqlScratch, modify, false);

				// And pop them.
				dsqlScratch->context->pop();
				dsqlScratch->context->pop();
				--dsqlScratch->scopeLevel;
			}

			// Recreate the list of assignments.

			CompoundStmtNode* assignStatements = FB_NEW_POOL(pool) CompoundStmtNode(pool);
			modify->statement = assignStatements;

			assignStatements->statements.resize(stmts->statements.getCount());

			for (FB_SIZE_T i = 0; i < assignStatements->statements.getCount(); ++i)
			{
				if (!PASS1_set_parameter_type(dsqlScratch, orgValues[i], newValues[i], false))
					PASS1_set_parameter_type(dsqlScratch, newValues[i], orgValues[i], false);

				AssignmentNode* assign = FB_NEW_POOL(pool) AssignmentNode(pool);
				assign->asgnFrom = orgValues[i];
				assign->asgnTo = newValues[i];
				assignStatements->statements[i] = assign;
			}

			// We do not allow cases like UPDATE SET f1 = v1, f2 = v2, f1 = v3...
			dsqlFieldAppearsOnce(newValues, "MERGE");
		}
		else
		{
			// Build the DELETE node.
			EraseNode* erase = FB_NEW_POOL(pool) EraseNode(pool);
			thisIf->trueAction = erase;

			dsql_ctx* context = dsqlGetContext(target);
			erase->dsqlContext = context;

			++dsqlScratch->scopeLevel;	// Go to the same level of source and target contexts.

			for (DsqlContextStack::iterator itr(usingCtxs); itr.hasData(); ++itr)
				dsqlScratch->context->push(itr.object());	// push the USING contexts

			dsqlScratch->context->push(context);	// process old context values

			if (matched->condition)
				thisIf->condition = doDsqlPass(dsqlScratch, matched->condition, false);

			if (returning)
			{
				StmtNode* delRet = ReturningProcessor::clone(dsqlScratch, returning, processedRet);

				processedRet = erase->statement = ReturningProcessor(
					dsqlScratch, context, NULL).process(returning, delRet);

				if (!nullRet)
					nullRet = dsqlNullifyReturning(dsqlScratch, erase, false);
			}

			// And pop the contexts.
			dsqlScratch->context->pop();
			dsqlScratch->context->pop();
			--dsqlScratch->scopeLevel;
		}

		if (lastIf)
			lastIf->falseAction = thisIf->condition ? thisIf : thisIf->trueAction;
		else
			update = thisIf->condition ? thisIf : thisIf->trueAction;

		lastIf = thisIf;

		// If a statement executes unconditionally, no other will ever execute.
		if (!thisIf->condition)
			break;
	}

	StmtNode* insert = NULL;
	lastIf = NULL;

	for (ObjectsArray<NotMatched>::iterator notMatched = whenNotMatched.begin();
		 notMatched != whenNotMatched.end();
		 ++notMatched)
	{
		IfNode* thisIf = FB_NEW_POOL(pool) IfNode(pool);

		++dsqlScratch->scopeLevel;	// Go to the same level of the source context.

		for (DsqlContextStack::iterator itr(usingCtxs); itr.hasData(); ++itr)
			dsqlScratch->context->push(itr.object());	// push the USING contexts

		// The INSERT relation should be processed in a higher level than the source context.
		++dsqlScratch->scopeLevel;

		// Build the INSERT node.
		StoreNode* store = FB_NEW_POOL(pool) StoreNode(pool);
		store->dsqlRelation = relation;
		store->dsqlFields = notMatched->fields;
		store->dsqlValues = notMatched->values;
		store->overrideClause = notMatched->overrideClause;

		bool needSavePoint; // unused
		thisIf->trueAction = store = nodeAs<StoreNode>(store->internalDsqlPass(dsqlScratch, false, needSavePoint));
		fb_assert(store);

		if (notMatched->condition)
			thisIf->condition = doDsqlPass(dsqlScratch, notMatched->condition, false);

		// Restore the scope level.
		--dsqlScratch->scopeLevel;

		if (returning)
		{
			StmtNode* insRet = ReturningProcessor::clone(dsqlScratch, returning, processedRet);

			dsql_ctx* const oldContext = dsqlGetContext(target);
			dsqlScratch->context->push(oldContext);

			dsql_ctx* context = dsqlGetContext(store->dsqlRelation);
			context->ctx_scope_level = oldContext->ctx_scope_level;

			processedRet = store->statement2 = ReturningProcessor(
				dsqlScratch, oldContext, context).process(returning, insRet);

			if (!nullRet)
				nullRet = dsqlNullifyReturning(dsqlScratch, store, false);

			dsqlScratch->context->pop();
		}

		// Pop the USING context.
		dsqlScratch->context->pop();
		--dsqlScratch->scopeLevel;

		if (lastIf)
			lastIf->falseAction = thisIf->condition ? thisIf : thisIf->trueAction;
		else
			insert = thisIf->condition ? thisIf : thisIf->trueAction;

		lastIf = thisIf;

		// If a statement executes unconditionally, no other will ever execute.
		if (!thisIf->condition)
			break;
	}

	// Build a IF (target.RDB$DB_KEY IS NULL).
	IfNode* action = FB_NEW_POOL(pool) IfNode(pool);

	RecordKeyNode* dbKeyNode = FB_NEW_POOL(pool) RecordKeyNode(pool, blr_dbkey);
	dbKeyNode->dsqlRelation = target;

	action->condition = FB_NEW_POOL(pool) MissingBoolNode(pool, dbKeyNode);

	if (insert)
	{
		action->trueAction = insert;	// then INSERT
		action->falseAction = update;	// else UPDATE/DELETE
	}
	else
	{
		// Negate the condition -> IF (target.RDB$DB_KEY IS NOT NULL).
		action->condition = FB_NEW_POOL(pool) NotBoolNode(pool, action->condition);
		action->trueAction = update;	// then UPDATE/DELETE
	}

	if (!dsqlScratch->isPsql())
	{
		// Describe it as EXECUTE_PROCEDURE if RETURNING is present or as INSERT otherwise.
		if (returning)
			dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_EXEC_PROCEDURE);
		else
			dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_INSERT);

		dsqlScratch->flags |= DsqlCompilerScratch::FLAG_MERGE;
	}

	// Insert the IF inside the FOR SELECT.
	forNode->statement = action;

	StmtNode* mergeStmt = forNode;

	// Setup the main node.

	if (nullRet)
	{
		CompoundStmtNode* temp = FB_NEW_POOL(pool) CompoundStmtNode(pool);
		temp->statements.add(nullRet);
		temp->statements.add(forNode);
		mergeStmt = temp;
	}

	StmtNode* sendNode = (FB_NEW_POOL(pool) MergeSendNode(pool, mergeStmt))->dsqlPass(dsqlScratch);

	return SavepointEncloseNode::make(dsqlScratch->getPool(), dsqlScratch, sendNode);
}

string MergeNode::internalPrint(NodePrinter& printer) const
{
	DsqlOnlyStmtNode::internalPrint(printer);

	NODE_PRINT(printer, relation);
	NODE_PRINT(printer, usingClause);
	NODE_PRINT(printer, condition);
	//// FIXME-PRINT: NODE_PRINT(printer, whenMatched);
	//// FIXME-PRINT: NODE_PRINT(printer, whenNotMatched);
	NODE_PRINT(printer, returning);

	return "MergeNode";
}

void MergeNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}


//--------------------


static RegisterNode<MessageNode> regMessageNode(blr_message);

// Parse a message declaration, including operator byte.
DmlNode* MessageNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	MessageNode* node = FB_NEW_POOL(pool) MessageNode(pool);

	// Parse the BLR and finish the node creation.
	USHORT message = csb->csb_blr_reader.getByte();
	USHORT count = csb->csb_blr_reader.getWord();
	node->setup(tdbb, csb, message, count);

	return node;
}

void MessageNode::setup(thread_db* tdbb, CompilerScratch* csb, USHORT message, USHORT count)
{
	// Register message number in the compiler scratch block, and
	// allocate a node to represent the message.

	CompilerScratch::csb_repeat* tail = CMP_csb_element(csb, message);

	tail->csb_message = this;
	messageNumber = message;

	if (message > csb->csb_msg_number)
		csb->csb_msg_number = message;

	USHORT padField;
	bool shouldPad = csb->csb_message_pad.get(messageNumber, padField);

	// Get the number of parameters in the message and prepare to fill out the format block.

	format = Format::newFormat(*tdbb->getDefaultPool(), count);
	USHORT maxAlignment = 0;
	ULONG offset = 0;

	Format::fmt_desc_iterator desc, end;
	USHORT index = 0;

	for (desc = format->fmt_desc.begin(), end = desc + count; desc < end; ++desc, ++index)
	{
		ItemInfo itemInfo;
		const USHORT alignment = setupDesc(tdbb, csb, index, &*desc, &itemInfo);
		if (alignment)
			offset = FB_ALIGN(offset, alignment);

		desc->dsc_address = (UCHAR*)(IPTR) offset;
		offset += desc->dsc_length;

		maxAlignment = MAX(maxAlignment, alignment);

		if (maxAlignment && shouldPad && index + 1 == padField)
			offset = FB_ALIGN(offset, maxAlignment);

		// ASF: Odd indexes are the nullable flag.
		// So we only check even indexes, which is the actual parameter.
		if (itemInfo.isSpecial() && index % 2 == 0)
		{
			csb->csb_dbg_info->argInfoToName.get(ArgumentInfo(csb->csb_msg_number, index / 2), itemInfo.name);
			csb->csb_map_item_info.put(Item(Item::TYPE_PARAMETER, message, index), itemInfo);
		}
	}

	format->fmt_length = offset;
}

USHORT MessageNode::setupDesc(thread_db* tdbb, CompilerScratch* csb, USHORT /*index*/,
	dsc* desc, ItemInfo* itemInfo)
{
	return PAR_desc(tdbb, csb, desc, itemInfo);
}

MessageNode* MessageNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

string MessageNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, messageNumber);
	NODE_PRINT(printer, format);
	NODE_PRINT(printer, impureFlags);

	return "MessageNode";
}

void MessageNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}

MessageNode* MessageNode::copy(thread_db* tdbb, NodeCopier& /*copier*/) const
{
	MessageNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) MessageNode(*tdbb->getDefaultPool());
	node->messageNumber = messageNumber;
	node->format = format;
	node->impureFlags = impureFlags;
	return node;
}

MessageNode* MessageNode::pass1(thread_db* /*tdbb*/, CompilerScratch* /*csb*/)
{
	return this;
}

MessageNode* MessageNode::pass2(thread_db* /*tdbb*/, CompilerScratch* csb)
{
	fb_assert(format);

	impureOffset = CMP_impure(csb, FB_ALIGN(format->fmt_length, 2));
	impureFlags = CMP_impure(csb, sizeof(USHORT) * format->fmt_count);

	return this;
}

const StmtNode* MessageNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		USHORT* flags = request->getImpure<USHORT>(impureFlags);
		memset(flags, 0, sizeof(USHORT) * format->fmt_count);
		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<ModifyNode> regModifyNode(blr_modify);
static RegisterNode<ModifyNode> regModifyNode2(blr_modify2);

// Parse a modify statement.
DmlNode* ModifyNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	// Parse the original and new contexts.

	USHORT context = (unsigned int) csb->csb_blr_reader.getByte();

	if (context >= csb->csb_rpt.getCount() || !(csb->csb_rpt[context].csb_flags & csb_used))
		PAR_error(csb, Arg::Gds(isc_ctxnotdef));

	const StreamType orgStream = csb->csb_rpt[context].csb_stream;
	const StreamType newStream = csb->nextStream(false);

	if (newStream >= MAX_STREAMS)
		PAR_error(csb, Arg::Gds(isc_too_many_contexts));

	context = csb->csb_blr_reader.getByte();

	// Make sure the compiler scratch block is big enough to hold everything.

	CompilerScratch::csb_repeat* tail = CMP_csb_element(csb, context);
	tail->csb_stream = newStream;
	tail->csb_flags |= csb_used;

	tail = CMP_csb_element(csb, newStream);
	tail->csb_relation = csb->csb_rpt[orgStream].csb_relation;

	// Make the node and parse the sub-expression.

	ModifyNode* node = FB_NEW_POOL(pool) ModifyNode(pool);
	node->orgStream = orgStream;
	node->newStream = newStream;

	AutoSetRestore<StmtNode*> autoCurrentDMLNode(&csb->csb_currentDMLNode, node);

	node->statement = PAR_parse_stmt(tdbb, csb);

	if (blrOp == blr_modify2)
		node->statement2 = PAR_parse_stmt(tdbb, csb);

	return node;
}


StmtNode* ModifyNode::internalDsqlPass(DsqlCompilerScratch* dsqlScratch, bool updateOrInsert)
{
	thread_db* tdbb = JRD_get_thread_data(); // necessary?
	MemoryPool& pool = dsqlScratch->getPool();

	// Separate old and new context references.

	Array<NestConst<ValueExprNode> > orgValues, newValues;

	CompoundStmtNode* assignments = nodeAs<CompoundStmtNode>(statement);
	fb_assert(assignments);

	for (FB_SIZE_T i = 0; i < assignments->statements.getCount(); ++i)
	{
		AssignmentNode* const assign = nodeAs<AssignmentNode>(assignments->statements[i]);
		fb_assert(assign);
		orgValues.add(assign->asgnFrom);
		newValues.add(assign->asgnTo);
	}

	NestConst<RelationSourceNode> relation = nodeAs<RelationSourceNode>(dsqlRelation);
	fb_assert(relation);

	NestConst<ValueExprNode>* ptr;

	ModifyNode* node = FB_NEW_POOL(pool) ModifyNode(pool);

	if (dsqlCursorName.hasData() && dsqlScratch->isPsql())
	{
		node->dsqlContext = dsqlPassCursorContext(dsqlScratch, dsqlCursorName, relation);

		// Process old context values.
		dsqlScratch->context->push(node->dsqlContext);
		++dsqlScratch->scopeLevel;

		for (ptr = orgValues.begin(); ptr != orgValues.end(); ++ptr)
			*ptr = doDsqlPass(dsqlScratch, *ptr, false);

		--dsqlScratch->scopeLevel;
		dsqlScratch->context->pop();

		// Process relation.
		doDsqlPass(dsqlScratch, node->dsqlRelation, relation, false);

		// Process new context values.
		for (ptr = newValues.begin(); ptr != newValues.end(); ++ptr)
			*ptr = doDsqlPass(dsqlScratch, *ptr, false);

		dsqlScratch->context->pop();

		dsql_ctx* oldContext = node->dsqlContext;
		dsql_ctx* modContext = dsqlGetContext(node->dsqlRelation);

		dsqlScratch->context->push(oldContext);	// process old context values
		++dsqlScratch->scopeLevel;

		node->statement2 = ReturningProcessor(dsqlScratch, oldContext, modContext).process(
			dsqlReturning, statement2);

		--dsqlScratch->scopeLevel;
		dsqlScratch->context->pop();

		// Recreate list of assignments.

		CompoundStmtNode* assignStatements = FB_NEW_POOL(pool) CompoundStmtNode(pool);
		node->statement = assignStatements;

		assignStatements->statements.resize(assignments->statements.getCount());

		for (FB_SIZE_T i = 0; i < assignStatements->statements.getCount(); ++i)
		{
			AssignmentNode* assign = FB_NEW_POOL(pool) AssignmentNode(pool);
			assign->asgnFrom = orgValues[i];
			assign->asgnTo = newValues[i];
			assignStatements->statements[i] = assign;
		}

		// We do not allow cases like UPDATE T SET f1 = v1, f2 = v2, f1 = v3...
		dsqlFieldAppearsOnce(newValues, "UPDATE");

		return node;
	}

	dsqlScratch->getStatement()->setType(dsqlCursorName.hasData() ?
		DsqlCompiledStatement::TYPE_UPDATE_CURSOR : DsqlCompiledStatement::TYPE_UPDATE);

	doDsqlPass(dsqlScratch, node->dsqlRelation, relation, false);
	dsql_ctx* mod_context = dsqlGetContext(node->dsqlRelation);

	// Process new context values.
	for (ptr = newValues.begin(); ptr != newValues.end(); ++ptr)
		*ptr = doDsqlPass(dsqlScratch, *ptr, false);

	dsqlScratch->context->pop();

	// Generate record selection expression

	RseNode* rse;
	dsql_ctx* old_context;

	if (dsqlCursorName.hasData())
	{
		rse = dsqlPassCursorReference(dsqlScratch, dsqlCursorName, relation);
		old_context = rse->dsqlStreams->items[0]->dsqlContext;
	}
	else
	{
		rse = FB_NEW_POOL(pool) RseNode(pool);
		rse->dsqlFlags = dsqlRseFlags;

		if (dsqlReturning || statement2)
			rse->dsqlFlags |= RecordSourceNode::DFLAG_SINGLETON;

		rse->dsqlStreams = FB_NEW_POOL(pool) RecSourceListNode(pool, 1);
		doDsqlPass(dsqlScratch, rse->dsqlStreams->items[0], relation, false);
		old_context = dsqlGetContext(rse->dsqlStreams->items[0]);

		if (dsqlBoolean)
			rse->dsqlWhere = doDsqlPass(dsqlScratch, dsqlBoolean, false);

		if (dsqlPlan)
			rse->rse_plan = doDsqlPass(dsqlScratch, dsqlPlan, false);

		if (dsqlOrder)
			rse->dsqlOrder = PASS1_sort(dsqlScratch, dsqlOrder, NULL);

		if (dsqlRows)
			PASS1_limit(dsqlScratch, dsqlRows->length, dsqlRows->skip, rse);
	}

	if (dsqlReturning || statement2)
	{
		node->statement2 = ReturningProcessor(dsqlScratch, old_context, mod_context).process(
			dsqlReturning, statement2);
	}

	node->dsqlRse = rse;

	// Process old context values.
	for (ptr = orgValues.begin(); ptr != orgValues.end(); ++ptr)
		*ptr = doDsqlPass(dsqlScratch, *ptr, false);

	dsqlScratch->context->pop();

	// Recreate list of assignments.

	CompoundStmtNode* assignStatements = FB_NEW_POOL(pool) CompoundStmtNode(pool);
	node->statement = assignStatements;

	assignStatements->statements.resize(assignments->statements.getCount());

	for (FB_SIZE_T j = 0; j < assignStatements->statements.getCount(); ++j)
	{
		ValueExprNode* const sub1 = orgValues[j];
		ValueExprNode* const sub2 = newValues[j];

		if (!PASS1_set_parameter_type(dsqlScratch, sub1, sub2, false))
			PASS1_set_parameter_type(dsqlScratch, sub2, sub1, false);

		AssignmentNode* assign = FB_NEW_POOL(pool) AssignmentNode(pool);
		assign->asgnFrom = sub1;
		assign->asgnTo = sub2;
		assignStatements->statements[j] = assign;
	}

	// We do not allow cases like UPDATE T SET f1 = v1, f2 = v2, f1 = v3...
	dsqlFieldAppearsOnce(newValues, "UPDATE");

	dsqlSetParametersName(dsqlScratch, assignStatements, node->dsqlRelation);

	StmtNode* ret = node;
	if (!updateOrInsert)
		ret = dsqlNullifyReturning(dsqlScratch, node, true);

	return ret;
}

StmtNode* ModifyNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return SavepointEncloseNode::make(dsqlScratch->getPool(), dsqlScratch, internalDsqlPass(dsqlScratch, false));
}

string ModifyNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlRelation);
	NODE_PRINT(printer, dsqlBoolean);
	NODE_PRINT(printer, dsqlPlan);
	NODE_PRINT(printer, dsqlOrder);
	NODE_PRINT(printer, dsqlRows);
	NODE_PRINT(printer, dsqlCursorName);
	NODE_PRINT(printer, dsqlReturning);
	NODE_PRINT(printer, dsqlRseFlags);
	NODE_PRINT(printer, dsqlRse);
	NODE_PRINT(printer, dsqlContext);
	NODE_PRINT(printer, statement);
	NODE_PRINT(printer, statement2);
	NODE_PRINT(printer, subMod);
	//// FIXME-PRINT: NODE_PRINT(printer, validations);
	NODE_PRINT(printer, mapView);
	NODE_PRINT(printer, orgStream);
	NODE_PRINT(printer, newStream);

	return "ModifyNode";
}

void ModifyNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	RseNode* rse = nodeAs<RseNode>(dsqlRse);

	const dsql_msg* message = dsqlGenDmlHeader(dsqlScratch, rse);

	dsqlScratch->appendUChar(statement2 ? blr_modify2 : blr_modify);

	const dsql_ctx* context;

	if (dsqlContext)
		context = dsqlContext;
	else
	{
		fb_assert(rse);
		context = rse->dsqlStreams->items[0]->dsqlContext;
	}

	GEN_stuff_context(dsqlScratch, context);
	context = dsqlRelation->dsqlContext;
	GEN_stuff_context(dsqlScratch, context);
	statement->genBlr(dsqlScratch);

	if (statement2)
		statement2->genBlr(dsqlScratch);

	if (message)
		dsqlScratch->appendUChar(blr_end);
}

ModifyNode* ModifyNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	preprocessAssignments(tdbb, csb, newStream, nodeAs<CompoundStmtNode>(statement), NULL);

	pass1Modify(tdbb, csb, this);

	doPass1(tdbb, csb, statement.getAddress());
	doPass1(tdbb, csb, subMod.getAddress());
	pass1Validations(tdbb, csb, validations);
	doPass1(tdbb, csb, mapView.getAddress());

	AutoSetRestore<bool> autoReturningExpr(&csb->csb_returning_expr, true);
	doPass1(tdbb, csb, statement2.getAddress());

	return this;
}

// Process a source for a modify statement. This can get a little tricky if the relation is a view.
void ModifyNode::pass1Modify(thread_db* tdbb, CompilerScratch* csb, ModifyNode* node)
{
	// If updateable views with triggers are involved, there maybe a recursive call to be ignored.

	if (node->subMod)
		return;

	jrd_rel* parent = NULL;
	jrd_rel* view = NULL;
	StreamType parentStream, parentNewStream;

	// To support nested views, loop until we hit a table or a view with user-defined triggers
	// (which means no update).

	for (;;)
	{
		StreamType stream = node->orgStream;
		StreamType newStream = node->newStream;

		CompilerScratch::csb_repeat* const tail = &csb->csb_rpt[stream];
		CompilerScratch::csb_repeat* const new_tail = &csb->csb_rpt[newStream];
		new_tail->csb_flags |= csb_modify;

		jrd_rel* const relation = tail->csb_relation;
		view = relation->rel_view_rse ? relation : view;

		if (!parent)
		{
			fb_assert(tail->csb_view == new_tail->csb_view);
			parent = new_tail->csb_view;
			parentStream = tail->csb_view_stream;
			parentNewStream = new_tail->csb_view_stream;
		}

		postTriggerAccess(csb, relation, ExternalAccess::exa_update, view);

		// Check out update. If this is an update thru a view, verify the view by checking for read
		// access on the base table. If field-level select privileges are implemented, this needs
		// to be enhanced.

		SecurityClass::flags_t priv = SCL_update;

		if (parent)
			priv |= SCL_select;

		RefPtr<const TrigVector> trigger(relation->rel_pre_modify ?
			relation->rel_pre_modify : relation->rel_post_modify);

		// If we have a view with triggers, let's expand it.

		if (relation->rel_view_rse && trigger)
			node->mapView = pass1ExpandView(tdbb, csb, stream, newStream, false);

		// Get the source relation, either a table or yet another view.

		RelationSourceNode* source = pass1Update(tdbb, csb, relation, trigger, stream, newStream,
												 priv, parent, parentStream, parentNewStream);

		if (!source)
		{
			// No source means we're done.
			if (!relation->rel_view_rse)
			{
				// Apply validation constraints.
				makeValidation(tdbb, csb, newStream, node->validations);
			}

			return;
		}

		parent = relation;
		parentStream = stream;
		parentNewStream = newStream;

		// Remap the source stream.

		StreamType* map = tail->csb_map;

		stream = source->getStream();
		stream = map[stream];

		// Copy the view source.

		map = CMP_alloc_map(tdbb, csb, node->newStream);
		NodeCopier copier(csb->csb_pool, csb, map);
		source = source->copy(tdbb, copier);

		if (trigger)
		{
			// ASF: This code is responsible to make view's WITH CHECK OPTION to work as constraints.

			// Set up the new target stream.

			const StreamType viewStream = newStream;
			newStream = source->getStream();
			fb_assert(newStream <= MAX_STREAMS);
			map[viewStream] = newStream;

			ModifyNode* viewNode = FB_NEW_POOL(*tdbb->getDefaultPool()) ModifyNode(*tdbb->getDefaultPool());
			viewNode->statement = pass1ExpandView(tdbb, csb, viewStream, newStream, true);

			node->subMod = viewNode;

			node = viewNode;
		}
		else
		{
			// This relation is not actually being updated as this operation
			// goes deeper (we have a naturally updatable view).
			csb->csb_rpt[newStream].csb_flags &= ~csb_view_update;
		}

		// Let's reset streams to represent the mapped source and target.
		node->orgStream = stream;
		node->newStream = source->getStream();
	}
}

ModifyNode* ModifyNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	// AB: Mark the streams involved with UPDATE statements active.
	// So that the optimizer can use indices for eventually used sub-selects.
	StreamList streams;
	streams.add(orgStream);
	streams.add(newStream);

	StreamStateHolder stateHolder(csb, streams);
	stateHolder.activate();

	doPass2(tdbb, csb, statement.getAddress(), this);
	doPass2(tdbb, csb, statement2.getAddress(), this);
	doPass2(tdbb, csb, subMod.getAddress(), this);

	for (Array<ValidateInfo>::iterator i = validations.begin(); i != validations.end(); ++i)
	{
		ExprNode::doPass2(tdbb, csb, i->boolean.getAddress());
		ExprNode::doPass2(tdbb, csb, i->value.getAddress());
	}

	doPass2(tdbb, csb, mapView.getAddress(), this);

	csb->csb_rpt[orgStream].csb_flags |= csb_update;

	const Format* format = CMP_format(tdbb, csb, orgStream);
	Format::fmt_desc_const_iterator desc = format->fmt_desc.begin();

	for (ULONG id = 0; id < format->fmt_count; ++id, ++desc)
	{
		if (desc->dsc_dtype)
			SBM_SET(tdbb->getDefaultPool(), &csb->csb_rpt[orgStream].csb_fields, id);
	}

	impureOffset = CMP_impure(csb, sizeof(impure_state));

	return this;
}

const StmtNode* ModifyNode::execute(thread_db* tdbb, jrd_req* request, ExeState* exeState) const
{
	impure_state* impure = request->getImpure<impure_state>(impureOffset);
	const StmtNode* retNode;

	if (request->req_operation == jrd_req::req_unwind)
		return parentStmt;

	if (request->req_operation == jrd_req::req_return && !impure->sta_state && subMod)
	{
		if (!exeState->topNode)
		{
			exeState->topNode = this;
			exeState->whichModTrig = PRE_TRIG;
		}

		exeState->prevNode = this;
		retNode = modify(tdbb, request, exeState->whichModTrig);

		if (exeState->whichModTrig == PRE_TRIG)
		{
			retNode = subMod;
			fb_assert(retNode->parentStmt == exeState->prevNode);
			///retNode->nod_parent = exeState->prevNode;
		}

		if (exeState->topNode == exeState->prevNode && exeState->whichModTrig == POST_TRIG)
		{
			exeState->topNode = NULL;
			exeState->whichModTrig = ALL_TRIGS;
		}
		else
			request->req_operation = jrd_req::req_evaluate;
	}
	else
	{
		exeState->prevNode = this;
		retNode = modify(tdbb, request, ALL_TRIGS);

		if (!subMod && exeState->whichModTrig == PRE_TRIG)
			exeState->whichModTrig = POST_TRIG;
	}

	return retNode;
}

// Execute a MODIFY statement.
const StmtNode* ModifyNode::modify(thread_db* tdbb, jrd_req* request, WhichTrigger whichTrig) const
{
	jrd_tra* transaction = request->req_transaction;
	impure_state* impure = request->getImpure<impure_state>(impureOffset);

	record_param* orgRpb = &request->req_rpb[orgStream];
	jrd_rel* relation = orgRpb->rpb_relation;

	if (orgRpb->rpb_number.isBof() || (!relation->rel_view_rse && !orgRpb->rpb_number.isValid()))
		ERR_post(Arg::Gds(isc_no_cur_rec));

	record_param* newRpb = &request->req_rpb[newStream];

	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
			request->req_records_affected.bumpModified(false);
			break;

		case jrd_req::req_return:
			if (impure->sta_state == 1)
			{
				impure->sta_state = 0;
				Record* orgRecord = orgRpb->rpb_record;
				const Record* newRecord = newRpb->rpb_record;
				orgRecord->copyDataFrom(newRecord, true);
				request->req_operation = jrd_req::req_evaluate;
				return statement;
			}

			if (impure->sta_state == 0)
			{
				// CVC: This call made here to clear the record in each NULL field and
				// varchar field whose tail may contain garbage.
				cleanupRpb(tdbb, newRpb);

				SavepointChangeMarker scMarker(transaction);

				preModifyEraseTriggers(tdbb, &relation->rel_pre_modify, whichTrig, orgRpb, newRpb,
					TRIGGER_UPDATE);

				if (validations.hasData())
					validateExpressions(tdbb, validations);

				if (relation->rel_file)
					EXT_modify(orgRpb, newRpb, transaction);
				else if (relation->isVirtual())
					VirtualTable::modify(tdbb, orgRpb, newRpb);
				else if (!relation->rel_view_rse)
				{
					VIO_modify(tdbb, orgRpb, newRpb, transaction);
					IDX_modify(tdbb, orgRpb, newRpb, transaction);
				}

				newRpb->rpb_number = orgRpb->rpb_number;
				newRpb->rpb_number.setValid(true);

				if (relation->rel_post_modify && whichTrig != PRE_TRIG)
				{
					EXE_execute_triggers(tdbb, &relation->rel_post_modify, orgRpb, newRpb,
						TRIGGER_UPDATE, POST_TRIG);
				}

				// Now call IDX_modify_check_constrints after all post modify triggers
				// have fired.  This is required for cascading referential integrity,
				// which can be implemented as post_erase triggers.

				if (!relation->rel_file && !relation->rel_view_rse && !relation->isVirtual())
					IDX_modify_check_constraints(tdbb, orgRpb, newRpb, transaction);

				if (!relation->rel_view_rse ||
					(!subMod && (whichTrig == ALL_TRIGS || whichTrig == POST_TRIG)))
				{
					request->req_records_updated++;
					request->req_records_affected.bumpModified(true);
				}

				if (statement2)
				{
					impure->sta_state = 2;
					request->req_operation = jrd_req::req_evaluate;
					return statement2;
				}
			}

			if (whichTrig != PRE_TRIG)
			{
				Record* orgRecord = orgRpb->rpb_record;
				orgRpb->rpb_record = newRpb->rpb_record;
				newRpb->rpb_record = orgRecord;
			}

		default:
			return parentStmt;
	}

	impure->sta_state = 0;
	RLCK_reserve_relation(tdbb, transaction, relation, true);

	// If the stream was sorted, the various fields in the rpb are
	// probably junk.  Just to make sure that everything is cool,
	// refetch and release the record.

	if (orgRpb->rpb_runtime_flags & RPB_refetch)
	{
		VIO_refetch_record(tdbb, orgRpb, transaction, false, false);
		orgRpb->rpb_runtime_flags &= ~RPB_refetch;
	}

	if (orgRpb->rpb_runtime_flags & RPB_undo_deleted)
	{
		request->req_operation = jrd_req::req_return;
		return parentStmt;
	}

	// Fall thru on evaluate to set up for modify before executing sub-statement.
	// This involves finding the appropriate format, making sure a record block
	// exists for the stream and is big enough, and copying fields from the
	// original record to the new record.

	const Format* const newFormat = MET_current(tdbb, newRpb->rpb_relation);
	Record* newRecord = VIO_record(tdbb, newRpb, newFormat, tdbb->getDefaultPool());
	newRpb->rpb_address = newRecord->getData();
	newRpb->rpb_length = newFormat->fmt_length;
	newRpb->rpb_format_number = newFormat->fmt_version;

	if (!orgRpb->rpb_record)
	{
		const Format* const orgFormat = newFormat;
		Record* const orgRecord = VIO_record(tdbb, orgRpb, orgFormat, tdbb->getDefaultPool());
		orgRpb->rpb_address = orgRecord->getData();
		orgRpb->rpb_length = orgFormat->fmt_length;
		orgRpb->rpb_format_number = orgFormat->fmt_version;
	}

	// Copy the original record to the new record

	VIO_copy_record(tdbb, orgRpb, newRpb);

	newRpb->rpb_number = orgRpb->rpb_number;
	newRpb->rpb_number.setValid(true);

	if (mapView)
	{
		impure->sta_state = 1;
		return mapView;
	}

	return statement;
}


//--------------------


static RegisterNode<PostEventNode> regPostEventNode1(blr_post);
static RegisterNode<PostEventNode> regPostEventNode2(blr_post_arg);

DmlNode* PostEventNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	PostEventNode* node = FB_NEW_POOL(pool) PostEventNode(pool);

	node->event = PAR_parse_value(tdbb, csb);
	if (blrOp == blr_post_arg)
		node->argument = PAR_parse_value(tdbb, csb);

	return node;
}

PostEventNode* PostEventNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	PostEventNode* node = FB_NEW_POOL(dsqlScratch->getPool()) PostEventNode(dsqlScratch->getPool());

	node->event = doDsqlPass(dsqlScratch, event);
	node->argument = doDsqlPass(dsqlScratch, argument);

	return node;
}

string PostEventNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, event);
	NODE_PRINT(printer, argument);

	return "PostEventNode";
}

void PostEventNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	if (argument)
	{
		dsqlScratch->appendUChar(blr_post_arg);
		GEN_expr(dsqlScratch, event);
		GEN_expr(dsqlScratch, argument);
	}
	else
	{
		dsqlScratch->appendUChar(blr_post);
		GEN_expr(dsqlScratch, event);
	}
}

PostEventNode* PostEventNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, event.getAddress());
	doPass1(tdbb, csb, argument.getAddress());
	return this;
}

PostEventNode* PostEventNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, event.getAddress());
	ExprNode::doPass2(tdbb, csb, argument.getAddress());
	return this;
}

const StmtNode* PostEventNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		jrd_tra* transaction = request->req_transaction;

		DeferredWork* work = DFW_post_work(transaction, dfw_post_event,
			EVL_expr(tdbb, request, event), 0);

		if (argument)
			DFW_post_work_arg(transaction, work, EVL_expr(tdbb, request, argument), 0);

		// For an autocommit transaction, events can be posted without any updates.

		if (transaction->tra_flags & TRA_autocommit)
			transaction->tra_flags |= TRA_perform_autocommit;

		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<ReceiveNode> regReceiveNode(blr_receive);
static RegisterNode<ReceiveNode> regReceiveNodeBatch(blr_receive_batch);

DmlNode* ReceiveNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	ReceiveNode* node = FB_NEW_POOL(pool) ReceiveNode(pool);

	USHORT n = csb->csb_blr_reader.getByte();
	node->message = csb->csb_rpt[n].csb_message;
	node->statement = PAR_parse_stmt(tdbb, csb);
	node->batchFlag = (blrOp == blr_receive_batch);

	return node;
}

ReceiveNode* ReceiveNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

string ReceiveNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, statement);
	NODE_PRINT(printer, message);
	NODE_PRINT(printer, batchFlag);

	return "ReceiveNode";
}

void ReceiveNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}

ReceiveNode* ReceiveNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, statement.getAddress());
	// Do not call message pass1.
	return this;
}

ReceiveNode* ReceiveNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	doPass2(tdbb, csb, statement.getAddress(), this);
	// Do not call message pass2.
	return this;
}

// Execute a RECEIVE statement. This can be entered either with "req_evaluate" (ordinary receive
// statement) or "req_proceed" (select statement).
// In the latter case, the statement isn't every formalled evaluated.
const StmtNode* ReceiveNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	switch (request->req_operation)
	{
		case jrd_req::req_return:
			if (!(request->req_batch && batchFlag))
				break;
			// fall into

		case jrd_req::req_evaluate:
			request->req_operation = jrd_req::req_receive;
			request->req_message = message;
			request->req_flags |= req_stall;
			return this;

		case jrd_req::req_proceed:
			request->req_operation = jrd_req::req_evaluate;
			return statement;

		default:
			break;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<StoreNode> regStoreNode(blr_store);
static RegisterNode<StoreNode> regStoreNode2(blr_store2);
static RegisterNode<StoreNode> regStoreNode3(blr_store3);

// Parse a store statement.
DmlNode* StoreNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR blrOp)
{
	StoreNode* node = FB_NEW_POOL(pool) StoreNode(pool);

	AutoSetRestore<StmtNode*> autoCurrentDMLNode(&csb->csb_currentDMLNode, node);

	if (blrOp == blr_store3)
	{
		node->overrideClause = static_cast<OverrideClause>(csb->csb_blr_reader.getByte());

		switch (node->overrideClause.value)
		{
			case OverrideClause::USER_VALUE:
			case OverrideClause::SYSTEM_VALUE:
				break;

			default:
				PAR_syntax_error(csb, "invalid blr_store3 override clause");
		}
	}

	const UCHAR* blrPos = csb->csb_blr_reader.getPos();

	node->relationSource = nodeAs<RelationSourceNode>(PAR_parseRecordSource(tdbb, csb));

	if (!node->relationSource)
	{
		csb->csb_blr_reader.setPos(blrPos);
		PAR_syntax_error(csb, "relation source");
	}

	node->statement = PAR_parse_stmt(tdbb, csb);

	if (blrOp == blr_store2)
		node->statement2 = PAR_parse_stmt(tdbb, csb);
	else if (blrOp == blr_store3)
	{
		if (csb->csb_blr_reader.peekByte() == blr_null)
			csb->csb_blr_reader.getByte();
		else
			node->statement2 = PAR_parse_stmt(tdbb, csb);
	}

	return node;
}

StmtNode* StoreNode::internalDsqlPass(DsqlCompilerScratch* dsqlScratch,
	bool updateOrInsert, bool& needSavePoint)
{
	thread_db* tdbb = JRD_get_thread_data(); // necessary?
	DsqlContextStack::AutoRestore autoContext(*dsqlScratch->context);

	dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_INSERT);

	StoreNode* node = FB_NEW_POOL(dsqlScratch->getPool()) StoreNode(dsqlScratch->getPool());
	node->overrideClause = overrideClause;

	// Process SELECT expression, if present

	ValueListNode* values;

	if (dsqlRse)
	{
		SelectExprNode* selExpr = nodeAs<SelectExprNode>(dsqlRse);
		fb_assert(selExpr);

		if (dsqlReturning || statement2)
			selExpr->dsqlFlags |= RecordSourceNode::DFLAG_SINGLETON;

		RseNode* rse = PASS1_rse(dsqlScratch, selExpr, false);
		node->dsqlRse = rse;
		values = rse->dsqlSelectList;
		needSavePoint = false;
	}
	else
	{
		values = doDsqlPass(dsqlScratch, dsqlValues, false);
		needSavePoint = SubSelectFinder::find(dsqlScratch->getPool(), values);
	}

	// Process relation

	node->dsqlRelation = PASS1_relation(dsqlScratch, dsqlRelation);
	dsql_ctx* context = node->dsqlRelation->dsqlContext;
	dsql_rel* relation = context->ctx_relation;

	// If there isn't a field list, generate one

	Array<NestConst<ValueExprNode> > fields;

	if (dsqlFields.hasData())
	{
		for (NestConst<FieldNode>* i = dsqlFields.begin(); i != dsqlFields.end(); ++i)
		{
			fields.add(NULL);
			doDsqlPass(dsqlScratch, fields.back(), *i, false);
		}

		// We do not allow cases like INSERT INTO T (f1, f2, f1)...
		dsqlFieldAppearsOnce(fields, "INSERT");

		// begin IBO hack
		// 02-May-2004, Nickolay Samofatov. Do not constify ptr further e.g. to
		// const dsql_nod* const* .... etc. It chokes GCC 3.4.0
		NestConst<ValueExprNode>* ptr = fields.begin();
		for (const NestConst<ValueExprNode>* const end = fields.end(); ptr != end; ++ptr)
		{
			const ValueExprNode* temp2 = *ptr;

			const dsql_ctx* tmp_ctx = NULL;
			const TEXT* tmp_name = NULL;
			const FieldNode* fieldNode;
			const DerivedFieldNode* derivedField;

			if ((fieldNode = nodeAs<FieldNode>(temp2)))
			{
				tmp_ctx = fieldNode->dsqlContext;
				if (fieldNode->dsqlField)
					tmp_name = fieldNode->dsqlField->fld_name.c_str();
			}
			else if ((derivedField = nodeAs<DerivedFieldNode>(temp2)))
			{
				tmp_ctx = derivedField->context;
				tmp_name = derivedField->name.nullStr();
			}

			if (tmp_ctx &&
				((tmp_ctx->ctx_relation && relation->rel_name != tmp_ctx->ctx_relation->rel_name) ||
				 tmp_ctx->ctx_context != context->ctx_context))
			{
				const dsql_rel* bad_rel = tmp_ctx->ctx_relation;

				PASS1_field_unknown((bad_rel ? bad_rel->rel_name.c_str() : NULL),
					tmp_name, dsqlFields[ptr - fields.begin()]);
			}
		}
		// end IBO hack
	}
	else
	{
		dsqlExplodeFields(relation, fields);

		for (NestConst<ValueExprNode>* i = fields.begin(); i != fields.end(); ++i)
			*i = doDsqlPass(dsqlScratch, *i, false);
	}

	// Match field fields and values

	CompoundStmtNode* assignStatements = FB_NEW_POOL(dsqlScratch->getPool()) CompoundStmtNode(dsqlScratch->getPool());
	node->statement = assignStatements;

	if (values)
	{
		if (fields.getCount() != values->items.getCount())
		{
			// count of column list and value list don't match
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-804) <<
					  Arg::Gds(isc_dsql_var_count_err));
		}

		NestConst<ValueExprNode>* ptr = fields.begin();
		NestConst<ValueExprNode>* ptr2 = values->items.begin();
		for (const NestConst<ValueExprNode>* end = fields.end(); ptr != end; ++ptr, ++ptr2)
		{
			// *ptr2 is NULL for DEFAULT

			if (!*ptr2)
			{
				const FieldNode* field = nodeAs<FieldNode>(*ptr);

				if (field && field->dsqlField)
				{
					*ptr2 = FB_NEW_POOL(dsqlScratch->getPool()) DefaultNode(dsqlScratch->getPool(),
						relation->rel_name, field->dsqlField->fld_name);
					*ptr2 = doDsqlPass(dsqlScratch, *ptr2, false);
				}
			}

			if (*ptr2)
			{
				AssignmentNode* temp = FB_NEW_POOL(dsqlScratch->getPool()) AssignmentNode(dsqlScratch->getPool());
				temp->asgnFrom = *ptr2;
				temp->asgnTo = *ptr;
				assignStatements->statements.add(temp);

				PASS1_set_parameter_type(dsqlScratch, *ptr2, temp->asgnTo, false);
			}
		}
	}

	if (updateOrInsert)
	{
		// Clone the insert context, push with name "OLD" in the same scope level and
		// marks it with CTX_null so all fields be resolved to NULL constant.
		dsql_ctx* old_context = FB_NEW_POOL(dsqlScratch->getPool()) dsql_ctx(dsqlScratch->getPool());
		*old_context = *context;
		old_context->ctx_alias = old_context->ctx_internal_alias = OLD_CONTEXT_NAME;
		old_context->ctx_flags |= CTX_system | CTX_null | CTX_returning;
		dsqlScratch->context->push(old_context);

		// clone the insert context and push with name "NEW" in a greater scope level
		dsql_ctx* new_context = FB_NEW_POOL(dsqlScratch->getPool()) dsql_ctx(dsqlScratch->getPool());
		*new_context = *context;
		new_context->ctx_scope_level = ++dsqlScratch->scopeLevel;
		new_context->ctx_alias = new_context->ctx_internal_alias = NEW_CONTEXT_NAME;
		new_context->ctx_flags |= CTX_system | CTX_returning;
		dsqlScratch->context->push(new_context);
	}

	node->statement2 = dsqlProcessReturning(dsqlScratch, dsqlReturning, statement2);

	if (updateOrInsert)
	{
		--dsqlScratch->scopeLevel;
		dsqlScratch->context->pop();
		dsqlScratch->context->pop();
	}

	dsqlSetParametersName(dsqlScratch, assignStatements, node->dsqlRelation);

	StmtNode* ret = node;
	if (!updateOrInsert)
		ret = dsqlNullifyReturning(dsqlScratch, node, true);

	dsqlScratch->context->pop();

	return ret;
}

StmtNode* StoreNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	bool needSavePoint;
	StmtNode* node = SavepointEncloseNode::make(dsqlScratch->getPool(), dsqlScratch,
		internalDsqlPass(dsqlScratch, false, needSavePoint));

	if (!needSavePoint || nodeIs<SavepointEncloseNode>(node))
		return node;

	return FB_NEW SavepointEncloseNode(dsqlScratch->getPool(), node);
}

string StoreNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlRelation);
	NODE_PRINT(printer, dsqlFields);
	NODE_PRINT(printer, dsqlValues);
	NODE_PRINT(printer, dsqlReturning);
	NODE_PRINT(printer, dsqlRse);
	NODE_PRINT(printer, statement);
	NODE_PRINT(printer, statement2);
	NODE_PRINT(printer, subStore);
	//// FIXME-PRINT: NODE_PRINT(printer, validations);
	NODE_PRINT(printer, relationSource);

	return "StoreNode";
}

void StoreNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	const dsql_msg* message = dsqlGenDmlHeader(dsqlScratch, nodeAs<RseNode>(dsqlRse));

	dsqlScratch->appendUChar(overrideClause.specified ? blr_store3 : (statement2 ? blr_store2 : blr_store));

	if (overrideClause.specified)
		dsqlScratch->appendUChar(UCHAR(overrideClause.value));

	GEN_expr(dsqlScratch, dsqlRelation);

	statement->genBlr(dsqlScratch);

	if (statement2)
		statement2->genBlr(dsqlScratch);
	else if (overrideClause.specified)
		dsqlScratch->appendUChar(blr_null);

	if (message)
		dsqlScratch->appendUChar(blr_end);
}

StoreNode* StoreNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	preprocessAssignments(tdbb, csb, relationSource->getStream(), nodeAs<CompoundStmtNode>(statement), &overrideClause);

	if (pass1Store(tdbb, csb, this))
		makeDefaults(tdbb, csb);

	doPass1(tdbb, csb, statement.getAddress());
	doPass1(tdbb, csb, statement2.getAddress());
	doPass1(tdbb, csb, subStore.getAddress());
	pass1Validations(tdbb, csb, validations);

	return this;
}

// Process a source for a store statement. This can get a little tricky if the relation is a view.
bool StoreNode::pass1Store(thread_db* tdbb, CompilerScratch* csb, StoreNode* node)
{
	// If updateable views with triggers are involved, there may be a recursive call to be ignored.

	if (node->subStore)
		return false;

	jrd_rel* parent = NULL;
	jrd_rel* view = NULL;
	StreamType parentStream;

	// To support nested views, loop until we hit a table or a view with user-defined triggers
	// (which means no update).

	for (;;)
	{
		RelationSourceNode* relSource = node->relationSource;
		const StreamType stream = relSource->getStream();

		CompilerScratch::csb_repeat* const tail = &csb->csb_rpt[stream];
		tail->csb_flags |= csb_store;

		jrd_rel* const relation = tail->csb_relation;
		view = relation->rel_view_rse ? relation : view;

		if (!parent)
		{
			parent = tail->csb_view;
			parentStream = tail->csb_view_stream;
		}

		postTriggerAccess(csb, relation, ExternalAccess::exa_insert, view);

		RefPtr<const TrigVector> trigger(relation->rel_pre_store ?
			relation->rel_pre_store : relation->rel_post_store);

		// Check out insert. If this is an insert thru a view, verify the view by checking for read
		// access on the base table. If field-level select privileges are implemented, this needs
		// to be enhanced.

		SecurityClass::flags_t priv = SCL_insert;

		if (parent)
			priv |= SCL_select;

		// Get the source relation, either a table or yet another view.

		relSource = pass1Update(tdbb, csb, relation, trigger, stream, stream,
								priv, parent, parentStream, parentStream);

		if (!relSource)
		{
			CMP_post_resource(&csb->csb_resources, relation, Resource::rsc_relation, relation->rel_id);

			if (!relation->rel_view_rse)
			{
				// Apply validation constraints.
				makeValidation(tdbb, csb, stream, node->validations);
			}

			return true;
		}

		parent = relation;
		parentStream = stream;

		StreamType* map = CMP_alloc_map(tdbb, csb, stream);
		NodeCopier copier(csb->csb_pool, csb, map);

		if (trigger)
		{
			// ASF: This code is responsible to make view's WITH CHECK OPTION to work as constraints.

			CMP_post_resource(&csb->csb_resources, relation, Resource::rsc_relation, relation->rel_id);

			// Set up the new target stream.

			relSource = relSource->copy(tdbb, copier);

			const StreamType newStream = relSource->getStream();
			StoreNode* viewNode = FB_NEW_POOL(*tdbb->getDefaultPool()) StoreNode(*tdbb->getDefaultPool());

			viewNode->relationSource = relSource;
			viewNode->statement = pass1ExpandView(tdbb, csb, stream, newStream, true);

			node->subStore = viewNode;

			// Substitute the original update node with the newly created one.
			node = viewNode;
		}
		else
		{
			// This relation is not actually being updated as this operation
			// goes deeper (we have a naturally updatable view).
			csb->csb_rpt[stream].csb_flags &= ~csb_view_update;
			node->relationSource = relSource->copy(tdbb, copier);
		}
	}
}

// Build a default value assignments.
void StoreNode::makeDefaults(thread_db* tdbb, CompilerScratch* csb)
{
	const StreamType stream = relationSource->getStream();
	jrd_rel* relation = csb->csb_rpt[stream].csb_relation;

	vec<jrd_fld*>* vector = relation->rel_fields;
	if (!vector)
		return;

	StreamMap localMap;
	StreamType* map = csb->csb_rpt[stream].csb_map;

	if (!map)
	{
		map = localMap.getBuffer(STREAM_MAP_LENGTH);
		fb_assert(stream <= MAX_STREAMS);
		map[0] = stream;
		map[1] = 1;
		map[2] = 2;
	}

	StmtNodeStack stack;
	USHORT fieldId = 0;
	vec<jrd_fld*>::iterator ptr1 = vector->begin();

	for (const vec<jrd_fld*>::const_iterator end = vector->end(); ptr1 < end; ++ptr1, ++fieldId)
	{
		ValueExprNode* value;

		if (!*ptr1 || !((*ptr1)->fld_generator_name.hasData() || (value = (*ptr1)->fld_default_value)))
			continue;

		CompoundStmtNode* compoundNode = StmtNode::as<CompoundStmtNode>(statement.getObject());
		fb_assert(compoundNode);

		if (compoundNode)
		{
			bool inList = false;

			for (FB_SIZE_T i = 0; i < compoundNode->statements.getCount(); ++i)
			{
				const AssignmentNode* assign = StmtNode::as<AssignmentNode>(
					compoundNode->statements[i].getObject());
				fb_assert(assign);

				if (assign)
				{
					const FieldNode* fieldNode = nodeAs<FieldNode>(assign->asgnTo);
					fb_assert(fieldNode);

					if (fieldNode && fieldNode->fieldStream == stream && fieldNode->fieldId == fieldId)
					{
						inList = true;
						break;
					}
				}
			}

			if (inList)
				continue;

			AssignmentNode* assign = FB_NEW_POOL(*tdbb->getDefaultPool()) AssignmentNode(
				*tdbb->getDefaultPool());
			assign->asgnTo = PAR_gen_field(tdbb, stream, fieldId);
			assign->asgnFrom = DefaultNode::createFromField(tdbb, csb, map, *ptr1);

			stack.push(assign);
		}
	}

	if (stack.isEmpty())
		return;

	// We have some default - add the original statement and make a list out of the whole mess.
	stack.push(statement);
	statement = PAR_make_list(tdbb, stack);
}

StoreNode* StoreNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	// AB: Mark the streams involved with INSERT statements active.
	// So that the optimizer can use indices for eventually used sub-selects.

	StreamList streams;
	streams.add(relationSource->getStream());

	StreamStateHolder stateHolder(csb, streams);
	stateHolder.activate();

	doPass2(tdbb, csb, statement.getAddress(), this);
	doPass2(tdbb, csb, statement2.getAddress(), this);
	doPass2(tdbb, csb, subStore.getAddress(), this);

	for (Array<ValidateInfo>::iterator i = validations.begin(); i != validations.end(); ++i)
	{
		ExprNode::doPass2(tdbb, csb, i->boolean.getAddress());
		ExprNode::doPass2(tdbb, csb, i->value.getAddress());
	}

	impureOffset = CMP_impure(csb, sizeof(impure_state));

	return this;
}

const StmtNode* StoreNode::execute(thread_db* tdbb, jrd_req* request, ExeState* exeState) const
{
	impure_state* impure = request->getImpure<impure_state>(impureOffset);
	const StmtNode* retNode;

	if (request->req_operation == jrd_req::req_return && !impure->sta_state && subStore)
	{
		if (!exeState->topNode)
		{
			exeState->topNode = this;
			exeState->whichStoTrig = PRE_TRIG;
		}

		exeState->prevNode = this;
		retNode = store(tdbb, request, exeState->whichStoTrig);

		if (exeState->whichStoTrig == PRE_TRIG)
		{
			retNode = subStore;
			fb_assert(retNode->parentStmt == exeState->prevNode);
			///retNode->nod_parent = exeState->prevNode;
		}

		if (exeState->topNode == exeState->prevNode && exeState->whichStoTrig == POST_TRIG)
		{
			exeState->topNode = NULL;
			exeState->whichStoTrig = ALL_TRIGS;
		}
		else
			request->req_operation = jrd_req::req_evaluate;
	}
	else
	{
		exeState->prevNode = this;
		retNode = store(tdbb, request, ALL_TRIGS);

		if (!subStore && exeState->whichStoTrig == PRE_TRIG)
			exeState->whichStoTrig = POST_TRIG;
	}

	return retNode;
}

// Execute a STORE statement.
const StmtNode* StoreNode::store(thread_db* tdbb, jrd_req* request, WhichTrigger whichTrig) const
{
	jrd_tra* transaction = request->req_transaction;
	impure_state* impure = request->getImpure<impure_state>(impureOffset);

	const StreamType stream = relationSource->getStream();
	record_param* rpb = &request->req_rpb[stream];
	jrd_rel* relation = rpb->rpb_relation;

	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
			if (!nodeIs<ForNode>(parentStmt))
				request->req_records_affected.clear();

			request->req_records_affected.bumpModified(false);
			impure->sta_state = 0;
			RLCK_reserve_relation(tdbb, transaction, relation, true);
			break;

		case jrd_req::req_return:
			if (!impure->sta_state)
			{
				SavepointChangeMarker scMarker(transaction);

				if (relation->rel_pre_store && whichTrig != POST_TRIG)
				{
					EXE_execute_triggers(tdbb, &relation->rel_pre_store, NULL, rpb,
						TRIGGER_INSERT, PRE_TRIG);
				}

				if (validations.hasData())
					validateExpressions(tdbb, validations);

				// For optimum on-disk record compression, zero all unassigned
				// fields. In addition, zero the tail of assigned varying fields
				// so that previous remnants don't defeat compression efficiency.

				// CVC: The code that was here was moved to its own routine: cleanupRpb()
				// and replaced by the call shown below.

				cleanupRpb(tdbb, rpb);

				if (relation->rel_file)
					EXT_store(tdbb, rpb);
				else if (relation->isVirtual())
					VirtualTable::store(tdbb, rpb);
				else if (!relation->rel_view_rse)
				{
					VIO_store(tdbb, rpb, transaction);
					IDX_store(tdbb, rpb, transaction);
				}

				rpb->rpb_number.setValid(true);

				if (relation->rel_post_store && whichTrig != PRE_TRIG)
				{
					EXE_execute_triggers(tdbb, &relation->rel_post_store, NULL, rpb,
						TRIGGER_INSERT, POST_TRIG);
				}

				if (!relation->rel_view_rse ||
					(!subStore && (whichTrig == ALL_TRIGS || whichTrig == POST_TRIG)))
				{
					request->req_records_inserted++;
					request->req_records_affected.bumpModified(true);
				}

				if (statement2)
				{
					impure->sta_state = 1;
					request->req_operation = jrd_req::req_evaluate;
					return statement2;
				}
			}
			// fall into

		default:
			return parentStmt;
	}

	// Fall thru on evaluate to set up for store before executing sub-statement.
	// This involves finding the appropriate format, making sure a record block
	// exists for the stream and is big enough, and initialize all null flags
	// to "missing."

	const Format* format = MET_current(tdbb, relation);
	Record* record = VIO_record(tdbb, rpb, format, tdbb->getDefaultPool());

	rpb->rpb_address = record->getData();
	rpb->rpb_length = format->fmt_length;
	rpb->rpb_format_number = format->fmt_version;

	// dimitr:	fake an invalid record number so that it could be evaluated to NULL
	// 			even if the valid stream marker is present for OLD/NEW trigger contexts
	rpb->rpb_number.setValue(BOF_NUMBER);

	// CVC: This small block added by Ann Harrison to
	// start with a clean empty buffer and so avoid getting
	// new record buffer with misleading information. Fixes
	// bug with incorrect blob sharing during insertion in
	// a stored procedure.

	record->nullify();

	return statement;
}


//--------------------


static RegisterNode<UserSavepointNode> regUserSavepointNode(blr_user_savepoint);

DmlNode* UserSavepointNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	UserSavepointNode* node = FB_NEW_POOL(pool) UserSavepointNode(pool);

	node->command = (Command) csb->csb_blr_reader.getByte();
	csb->csb_blr_reader.getMetaName(node->name);

	return node;
}

UserSavepointNode* UserSavepointNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	fb_assert(!(dsqlScratch->flags & DsqlCompilerScratch::FLAG_BLOCK));
	dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_SAVEPOINT);
	return this;
}

string UserSavepointNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, command);
	NODE_PRINT(printer, name);

	return "UserSavepointNode";
}

void UserSavepointNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_user_savepoint);
	dsqlScratch->appendUChar((UCHAR) command);
	dsqlScratch->appendNullString(name.c_str());
}

UserSavepointNode* UserSavepointNode::pass1(thread_db* /*tdbb*/, CompilerScratch* /*csb*/)
{
	return this;
}

UserSavepointNode* UserSavepointNode::pass2(thread_db* /*tdbb*/, CompilerScratch* /*csb*/)
{
	return this;
}

const StmtNode* UserSavepointNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	jrd_tra* transaction = request->req_transaction;

	if (request->req_operation == jrd_req::req_evaluate &&
		!(transaction->tra_flags & TRA_system))
	{
		// Skip the savepoint created by EXE_start
		Savepoint* const previous = transaction->tra_save_point;

		// Find savepoint
		Savepoint* savepoint = NULL;

		for (Savepoint::Iterator iter(previous); *iter; ++iter)
		{
			Savepoint* const current = *iter;

			if (current == previous)
				continue;

			if (current->isSystem())
				break;

			if (current->getName() == name)
			{
				savepoint = current;
				break;
			}
		}

		if (!savepoint && command != CMD_SET)
			ERR_post(Arg::Gds(isc_invalid_savepoint) << Arg::Str(name));

		switch (command)
		{
			case CMD_SET:
				// Release the savepoint
				if (savepoint)
					savepoint->rollforward(tdbb, previous);

				// Use the savepoint created by EXE_start
				transaction->tra_save_point->setName(name);
				break;

			case CMD_RELEASE_ONLY:
			{
				// Release the savepoint
				savepoint->rollforward(tdbb, previous);
				break;
			}

			case CMD_RELEASE:
			{
				const SavNumber savNumber = savepoint->getNumber();

				// Release the savepoint and all subsequent ones
				while (transaction->tra_save_point &&
					transaction->tra_save_point->getNumber() >= savNumber)
				{
					transaction->rollforwardSavepoint(tdbb);
				}

				// Restore the savepoint initially created by EXE_start
				transaction->startSavepoint();
				break;
			}

			case CMD_ROLLBACK:
			{
				transaction->rollbackToSavepoint(tdbb, savepoint->getNumber());

				// Now set the savepoint again to allow to return to it later
				Savepoint* const savepoint = transaction->startSavepoint();
				savepoint->setName(name);
				break;
			}

			default:
				SOFT_BUGCHECK(232);	// msg 232 EVL_expr: invalid operation
				break;
		}

		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<SelectNode> regSelectNode(blr_select);

DmlNode* SelectNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	SelectNode* node = FB_NEW_POOL(pool) SelectNode(pool);

	while (csb->csb_blr_reader.peekByte() != blr_end)
	{
		if (csb->csb_blr_reader.peekByte() != blr_receive)
			PAR_syntax_error(csb, "blr_receive");
		node->statements.add(PAR_parse_stmt(tdbb, csb));
	}

	csb->csb_blr_reader.getByte();	// skip blr_end

	return node;
}

SelectNode* SelectNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	SelectNode* node = FB_NEW_POOL(dsqlScratch->getPool()) SelectNode(dsqlScratch->getPool());
	node->dsqlForUpdate = dsqlForUpdate;

	const DsqlContextStack::iterator base(*dsqlScratch->context);
	node->dsqlRse = PASS1_rse(dsqlScratch, dsqlExpr, dsqlWithLock);
	dsqlScratch->context->clear(base);

	if (dsqlForUpdate)
	{
		dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_SELECT_UPD);
		dsqlScratch->getStatement()->addFlags(DsqlCompiledStatement::FLAG_NO_BATCH);
	}
	else
	{
		// If there is a union without ALL or order by or a select distinct buffering is OK even if
		// stored procedure occurs in the select list. In these cases all of stored procedure is
		// executed under savepoint for open cursor.

		RseNode* rseNode = nodeAs<RseNode>(node->dsqlRse);

		if (rseNode->dsqlOrder || rseNode->dsqlDistinct)
		{
			dsqlScratch->getStatement()->setFlags(
				dsqlScratch->getStatement()->getFlags() & ~DsqlCompiledStatement::FLAG_NO_BATCH);
		}
	}

	return node;
}

string SelectNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlExpr);
	NODE_PRINT(printer, dsqlForUpdate);
	NODE_PRINT(printer, dsqlWithLock);
	NODE_PRINT(printer, dsqlRse);
	NODE_PRINT(printer, statements);

	return "SelectNode";
}

// Generate BLR for a SELECT statement.
void SelectNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	RseNode* const rse = nodeAs<RseNode>(dsqlRse);
	fb_assert(rse);

	DsqlCompiledStatement* const statement = dsqlScratch->getStatement();

	// Set up parameter for things in the select list.
	ValueListNode* list = rse->dsqlSelectList;
	NestConst<ValueExprNode>* ptr = list->items.begin();
	for (const NestConst<ValueExprNode>* const end = list->items.end(); ptr != end; ++ptr)
	{
		dsql_par* parameter = MAKE_parameter(statement->getReceiveMsg(), true, true, 0, *ptr);
		parameter->par_node = *ptr;
		MAKE_desc(dsqlScratch, &parameter->par_desc, *ptr);
	}

	// Set up parameter to handle EOF.

	dsql_par* const parameterEof = MAKE_parameter(statement->getReceiveMsg(), false, false, 0, NULL);
	statement->setEof(parameterEof);
	parameterEof->par_desc.dsc_dtype = dtype_short;
	parameterEof->par_desc.dsc_scale = 0;
	parameterEof->par_desc.dsc_length = sizeof(SSHORT);

	// Save DBKEYs for possible update later.

	GenericMap<NonPooled<dsql_par*, dsql_ctx*> > paramContexts(*getDefaultMemoryPool());
	dsql_ctx* context;

	if (dsqlForUpdate && !rse->dsqlDistinct)
	{
		RecSourceListNode* streamList = rse->dsqlStreams;
		NestConst<RecordSourceNode>* ptr2 = streamList->items.begin();

		for (const NestConst<RecordSourceNode>* const end2 = streamList->items.end(); ptr2 != end2; ++ptr2)
		{
			RecordSourceNode* const item = *ptr2;
			RelationSourceNode* relNode;

			if (item && (relNode = nodeAs<RelationSourceNode>(item)))
			{
				context = relNode->dsqlContext;
				const dsql_rel* const relation = context->ctx_relation;

				if (relation)
				{
					// Set up dbkey.
					dsql_par* parameter = MAKE_parameter(
						statement->getReceiveMsg(), false, false, 0, NULL);

					parameter->par_dbkey_relname = relation->rel_name;
					paramContexts.put(parameter, context);

					parameter->par_desc.dsc_dtype = dtype_text;
					parameter->par_desc.dsc_ttype() = ttype_binary;
					parameter->par_desc.dsc_length = relation->rel_dbkey_length;

					// Set up record version.
					parameter = MAKE_parameter(statement->getReceiveMsg(), false, false, 0, NULL);
					parameter->par_rec_version_relname = relation->rel_name;
					paramContexts.put(parameter, context);

					parameter->par_desc.dsc_dtype = dtype_text;
					parameter->par_desc.dsc_ttype() = ttype_binary;
					parameter->par_desc.dsc_length = sizeof(SINT64);
				}
			}
		}
	}

	// Generate definitions for the messages.

	GEN_port(dsqlScratch, statement->getReceiveMsg());
	dsql_msg* message = statement->getSendMsg();
	if (message->msg_parameter)
		GEN_port(dsqlScratch, message);
	else
		statement->setSendMsg(NULL);

	// If there is a send message, build a RECEIVE.

	if ((message = statement->getSendMsg()) != NULL)
	{
		dsqlScratch->appendUChar(blr_receive);
		dsqlScratch->appendUChar(message->msg_number);
	}

	// Generate FOR loop.

	message = statement->getReceiveMsg();

	dsqlScratch->appendUChar(blr_for);
	dsqlScratch->appendUChar(blr_stall);
	GEN_rse(dsqlScratch, dsqlRse);

	dsqlScratch->appendUChar(blr_send);
	dsqlScratch->appendUChar(message->msg_number);
	dsqlScratch->appendUChar(blr_begin);

	// Build body of FOR loop.

	SSHORT constant;
	dsc constant_desc;
	constant_desc.makeShort(0, &constant);

	// Add invalid usage here.

	dsqlScratch->appendUChar(blr_assignment);
	constant = 1;
	LiteralNode::genConstant(dsqlScratch, &constant_desc, false);
	GEN_parameter(dsqlScratch, statement->getEof());

	for (FB_SIZE_T i = 0; i < message->msg_parameters.getCount(); ++i)
	{
		dsql_par* const parameter = message->msg_parameters[i];

		if (parameter->par_node)
		{
			dsqlScratch->appendUChar(blr_assignment);
			GEN_expr(dsqlScratch, parameter->par_node);
			GEN_parameter(dsqlScratch, parameter);
		}

		if (parameter->par_dbkey_relname.hasData() && paramContexts.get(parameter, context))
		{
			dsqlScratch->appendUChar(blr_assignment);
			dsqlScratch->appendUChar(blr_dbkey);
			GEN_stuff_context(dsqlScratch, context);
			GEN_parameter(dsqlScratch, parameter);
		}

		if (parameter->par_rec_version_relname.hasData() && paramContexts.get(parameter, context))
		{
			dsqlScratch->appendUChar(blr_assignment);
			dsqlScratch->appendUChar(blr_record_version);
			GEN_stuff_context(dsqlScratch, context);
			GEN_parameter(dsqlScratch, parameter);
		}
	}

	dsqlScratch->appendUChar(blr_end);
	dsqlScratch->appendUChar(blr_send);
	dsqlScratch->appendUChar(message->msg_number);
	dsqlScratch->appendUChar(blr_assignment);
	constant = 0;
	LiteralNode::genConstant(dsqlScratch, &constant_desc, false);
	GEN_parameter(dsqlScratch, statement->getEof());
}

SelectNode* SelectNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	for (NestConst<StmtNode>* i = statements.begin(); i != statements.end(); ++i)
		doPass1(tdbb, csb, i->getAddress());
	return this;
}

SelectNode* SelectNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	for (NestConst<StmtNode>* i = statements.begin(); i != statements.end(); ++i)
		doPass2(tdbb, csb, i->getAddress(), this);
	return this;
}

// Execute a SELECT statement. This is more than a little obscure.
// We first set up the SELECT statement as the "message" and stall on receive (waiting for user send).
// EXE_send will then loop thru the sub-statements of select looking for the appropriate RECEIVE
// statement. When (or if) it finds it, it will set it up the next statement to be executed.
// The RECEIVE, then, will be entered with the operation "req_proceed".
const StmtNode* SelectNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
			request->req_message = this;
			request->req_operation = jrd_req::req_receive;
			request->req_flags |= req_stall;
			return this;

		default:
			return parentStmt;
	}
}


//--------------------


// This is only for GPRE's cmp_set_generator().
static RegisterNode<SetGeneratorNode> regSetGeneratorNode(blr_set_generator);

DmlNode* SetGeneratorNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	MetaName name;
	csb->csb_blr_reader.getMetaName(name);

	SetGeneratorNode* const node = FB_NEW_POOL(pool) SetGeneratorNode(pool, name);

	bool sysGen = false;
	if (!MET_load_generator(tdbb, node->generator, &sysGen))
		PAR_error(csb, Arg::Gds(isc_gennotdef) << Arg::Str(name));

	if (sysGen)
		PAR_error(csb, Arg::Gds(isc_cant_modify_sysobj) << "generator" << name);

	node->value = PAR_parse_value(tdbb, csb);

	return node;
}

string SetGeneratorNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, generator);
	NODE_PRINT(printer, value);

	return "SetGeneratorNode";
}

SetGeneratorNode* SetGeneratorNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, value.getAddress());

	CMP_post_access(tdbb, csb, generator.secName, 0,
					SCL_usage, SCL_object_generator, generator.name);

	return this;
}

SetGeneratorNode* SetGeneratorNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, value.getAddress());
	return this;
}

const StmtNode* SetGeneratorNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	if (request->req_operation == jrd_req::req_evaluate)
	{
		jrd_tra* const transaction = request->req_transaction;

		DdlNode::executeDdlTrigger(tdbb, transaction, DdlNode::DTW_BEFORE,
			DDL_TRIGGER_ALTER_SEQUENCE, generator.name, NULL, *request->getStatement()->sqlText);

		dsc* const desc = EVL_expr(tdbb, request, value);
		DPM_gen_id(tdbb, generator.id, true, MOV_get_int64(tdbb, desc, 0));

		DdlNode::executeDdlTrigger(tdbb, transaction, DdlNode::DTW_AFTER,
			DDL_TRIGGER_ALTER_SEQUENCE, generator.name, NULL, *request->getStatement()->sqlText);

		request->req_operation = jrd_req::req_return;
	}

	return parentStmt;
}


//--------------------


static RegisterNode<StallNode> regStallNode(blr_stall);

DmlNode* StallNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* /*csb*/, const UCHAR /*blrOp*/)
{
	StallNode* node = FB_NEW_POOL(pool) StallNode(pool);
	return node;
}

StallNode* StallNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

string StallNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);
	return "StallNode";
}

void StallNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}

StallNode* StallNode::pass1(thread_db* /*tdbb*/, CompilerScratch* /*csb*/)
{
	return this;
}

StallNode* StallNode::pass2(thread_db* /*tdbb*/, CompilerScratch* /*csb*/)
{
	return this;
}

// Execute a stall statement. This is like a blr_receive, except that there is no need for a
// gds__send () from the user (i.e. EXE_send () in the engine).
// A gds__receive () will unblock the user.
const StmtNode* StallNode::execute(thread_db* /*tdbb*/, jrd_req* request, ExeState* /*exeState*/) const
{
	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
		case jrd_req::req_return:
			request->req_message = this;
			request->req_operation = jrd_req::req_return;
			request->req_flags |= req_stall;
			return this;

		case jrd_req::req_proceed:
			request->req_operation = jrd_req::req_return;
			return parentStmt;

		default:
			return parentStmt;
	}
}


//--------------------


static RegisterNode<SuspendNode> regSuspendNode(blr_send);

DmlNode* SuspendNode::parse(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, const UCHAR /*blrOp*/)
{
	SuspendNode* node = FB_NEW_POOL(pool) SuspendNode(pool);

	USHORT n = csb->csb_blr_reader.getByte();
	node->message = csb->csb_rpt[n].csb_message;
	node->statement = PAR_parse_stmt(tdbb, csb);

	return node;
}

SuspendNode* SuspendNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	DsqlCompiledStatement* const statement = dsqlScratch->getStatement();

	if (dsqlScratch->flags & (DsqlCompilerScratch::FLAG_TRIGGER | DsqlCompilerScratch::FLAG_FUNCTION))
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  // Token unknown
				  Arg::Gds(isc_token_err) <<
				  Arg::Gds(isc_random) << Arg::Str("SUSPEND"));
	}

	if (dsqlScratch->flags & DsqlCompilerScratch::FLAG_IN_AUTO_TRANS_BLOCK)	// autonomous transaction
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
				  Arg::Gds(isc_dsql_unsupported_in_auto_trans) << Arg::Str("SUSPEND"));
	}

	statement->addFlags(DsqlCompiledStatement::FLAG_SELECTABLE);

	return this;
}

string SuspendNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, message);
	NODE_PRINT(printer, statement);

	return "SuspendNode";
}

void SuspendNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->genReturn();
}

SuspendNode* SuspendNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, statement.getAddress());
	return this;
}

SuspendNode* SuspendNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	doPass2(tdbb, csb, statement.getAddress(), this);
	return this;
}

// Execute a SEND statement.
const StmtNode* SuspendNode::execute(thread_db* tdbb, jrd_req* request, ExeState* /*exeState*/) const
{
	switch (request->req_operation)
	{
		case jrd_req::req_evaluate:
		{
			// ASF: If this is the send in the tail of a procedure and the procedure was called
			// with a SELECT, don't run all the send statements. It may make validations fail when
			// the procedure didn't return any rows. See CORE-2204.
			// But we should run the last assignment, as it's the one who make the procedure stop.

			if (!(request->req_flags & req_proc_fetch))
				return statement;

			const CompoundStmtNode* list = nodeAs<CompoundStmtNode>(parentStmt);

			if (list && !list->parentStmt && list->statements[list->statements.getCount() - 1] == this)
			{
				list = nodeAs<CompoundStmtNode>(statement);

				if (list && list->onlyAssignments && list->statements.hasData())
				{
					// This is the assignment that sets the EOS parameter.
					const AssignmentNode* assign = static_cast<const AssignmentNode*>(
						list->statements[list->statements.getCount() - 1].getObject());
					EXE_assignment(tdbb, assign);
				}
				else
					return statement;
			}
			else
				return statement;

			// fall into
		}

		case jrd_req::req_return:
			request->req_operation = jrd_req::req_send;
			request->req_message = message;
			request->req_flags |= req_stall;
			return this;

		case jrd_req::req_proceed:
			request->req_operation = jrd_req::req_return;
			return parentStmt;

		default:
			return parentStmt;
	}
}


//--------------------


ReturnNode* ReturnNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	if (!(dsqlScratch->flags & DsqlCompilerScratch::FLAG_FUNCTION))
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  // Token unknown
				  Arg::Gds(isc_token_err) <<
				  Arg::Gds(isc_random) << Arg::Str("RETURN"));
	}

	if (dsqlScratch->flags & DsqlCompilerScratch::FLAG_IN_AUTO_TRANS_BLOCK)	// autonomous transaction
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
				  Arg::Gds(isc_dsql_unsupported_in_auto_trans) << Arg::Str("RETURN"));
	}

	ReturnNode* node = FB_NEW_POOL(dsqlScratch->getPool()) ReturnNode(dsqlScratch->getPool());
	node->value = doDsqlPass(dsqlScratch, value);

	return node;
}

string ReturnNode::internalPrint(NodePrinter& printer) const
{
	DsqlOnlyStmtNode::internalPrint(printer);

	NODE_PRINT(printer, value);

	return "ReturnNode";
}

void ReturnNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_begin);
	dsqlScratch->appendUChar(blr_assignment);
	GEN_expr(dsqlScratch, value);
	dsqlScratch->appendUChar(blr_variable);
	dsqlScratch->appendUShort(0);
	dsqlScratch->genReturn();
	dsqlScratch->appendUChar(blr_leave);
	dsqlScratch->appendUChar(0);
	dsqlScratch->appendUChar(blr_end);
}


//--------------------


static RegisterNode<SavePointNode> regSavePointNodeStart(blr_start_savepoint);
static RegisterNode<SavePointNode> regSavePointNodeEnd(blr_end_savepoint);

DmlNode* SavePointNode::parse(thread_db* /*tdbb*/, MemoryPool& pool, CompilerScratch* /*csb*/, const UCHAR blrOp)
{
	SavePointNode* node = FB_NEW_POOL(pool) SavePointNode(pool, blrOp);
	return node;
}

SavePointNode* SavePointNode::dsqlPass(DsqlCompilerScratch* /*dsqlScratch*/)
{
	return this;
}

string SavePointNode::internalPrint(NodePrinter& printer) const
{
	StmtNode::internalPrint(printer);

	NODE_PRINT(printer, blrOp);

	return "SavePointNode";
}

void SavePointNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blrOp);
}

const StmtNode* SavePointNode::execute(thread_db* tdbb, jrd_req* request, ExeState* exeState) const
{
	jrd_tra* transaction = request->req_transaction;

	switch (blrOp)
	{
		case blr_start_savepoint:
			if (request->req_operation == jrd_req::req_evaluate)
			{
				// Start a save point.
				if (!(transaction->tra_flags & TRA_system))
					transaction->startSavepoint();

				request->req_operation = jrd_req::req_return;
			}
			break;

		case blr_end_savepoint:
			if (request->req_operation == jrd_req::req_evaluate ||
				request->req_operation == jrd_req::req_unwind)
			{
				// If any requested modify/delete/insert ops have completed, forget them.
				if (!(transaction->tra_flags & TRA_system))
				{
					// If an error is still pending when the savepoint is supposed to end, then the
					// application didn't handle the error and the savepoint should be undone.
					if (exeState->errorPending)
						transaction->rollbackSavepoint(tdbb);
					else
						transaction->rollforwardSavepoint(tdbb);
				}

				if (request->req_operation == jrd_req::req_evaluate)
					request->req_operation = jrd_req::req_return;
			}
			break;

		default:
			fb_assert(false);
	}

	return parentStmt;
}


//--------------------


SetTransactionNode* SetTransactionNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_START_TRANS);

	// Generate tpb for set transaction. Use blr string of dsqlScratch.
	// If a value is not specified, default is not stuffed, let the engine handle it.

	fb_assert(dsqlScratch->getBlrData().getCount() == 0);

	// Find out isolation level - if specified. This is required for
	// specifying the correct lock level in reserving clause.
	const USHORT lockLevel = isoLevel.specified && isoLevel.value == ISO_LEVEL_CONSISTENCY ?
		isc_tpb_protected : isc_tpb_shared;

	// Stuff some version info.
	dsqlScratch->appendUChar(isc_tpb_version1);

	if (readOnly.specified)
		dsqlScratch->appendUChar(readOnly.value ? isc_tpb_read : isc_tpb_write);

	if (wait.specified)
		dsqlScratch->appendUChar(wait.value ? isc_tpb_wait : isc_tpb_nowait);

	if (isoLevel.specified)
	{
		if (isoLevel.value == ISO_LEVEL_CONCURRENCY)
			dsqlScratch->appendUChar(isc_tpb_concurrency);
		else if (isoLevel.value == ISO_LEVEL_CONSISTENCY)
			dsqlScratch->appendUChar(isc_tpb_consistency);
		else
		{
			dsqlScratch->appendUChar(isc_tpb_read_committed);

			if (isoLevel.value == ISO_LEVEL_READ_COMMITTED_REC_VERSION)
				dsqlScratch->appendUChar(isc_tpb_rec_version);
			else
				dsqlScratch->appendUChar(isc_tpb_no_rec_version);
		}
	}

	if (noAutoUndo.specified)
		dsqlScratch->appendUChar(isc_tpb_no_auto_undo);

	if (ignoreLimbo.specified)
		dsqlScratch->appendUChar(isc_tpb_ignore_limbo);

	if (restartRequests.specified)
		dsqlScratch->appendUChar(isc_tpb_restart_requests);

	if (autoCommit.specified)
		dsqlScratch->appendUChar(isc_tpb_autocommit);

	if (lockTimeout.specified)
	{
		dsqlScratch->appendUChar(isc_tpb_lock_timeout);
		dsqlScratch->appendUChar(2);
		dsqlScratch->appendUShort(lockTimeout.value);
	}

	for (RestrictionOption** i = reserveList.begin(); i != reserveList.end(); ++i)
		genTableLock(dsqlScratch, **i, lockLevel);

	if (dsqlScratch->getBlrData().getCount() > 1)	// 1 -> isc_tpb_version1
		tpb.add(dsqlScratch->getBlrData().begin(), dsqlScratch->getBlrData().getCount());

	return this;
}

void SetTransactionNode::execute(thread_db* tdbb, dsql_req* request, jrd_tra** transaction) const
{
	JRD_start_transaction(tdbb, &request->req_transaction, request->req_dbb->dbb_attachment,
		tpb.getCount(), tpb.begin());
	*transaction = request->req_transaction;
}

// Generate tpb for table lock.
// If lock level is specified, it overrrides the transaction lock level.
void SetTransactionNode::genTableLock(DsqlCompilerScratch* dsqlScratch,
	const RestrictionOption& tblLock, USHORT lockLevel)
{
	if (tblLock.tables->isEmpty())
		return;

	if (tblLock.lockMode & LOCK_MODE_PROTECTED)
		lockLevel = isc_tpb_protected;
	else if (tblLock.lockMode & LOCK_MODE_SHARED)
		lockLevel = isc_tpb_shared;

	const USHORT lockMode = (tblLock.lockMode & LOCK_MODE_WRITE) ?
		isc_tpb_lock_write : isc_tpb_lock_read;

	for (ObjectsArray<MetaName>::iterator i = tblLock.tables->begin();
		 i != tblLock.tables->end();
		 ++i)
	{
		dsqlScratch->appendUChar(lockMode);
		dsqlScratch->appendNullString(i->c_str());	// stuff table name
		dsqlScratch->appendUChar(lockLevel);
	}
}


//--------------------


void SessionResetNode::execute(thread_db* tdbb, dsql_req* request, jrd_tra** traHandle) const
{
	SET_TDBB(tdbb);
	Attachment* const attachment = tdbb->getAttachment();
	attachment->resetSession(tdbb, traHandle);
}


//--------------------


void SetRoleNode::execute(thread_db* tdbb, dsql_req* request, jrd_tra** /*traHandle*/) const
{
	SET_TDBB(tdbb);
	Attachment* const attachment = tdbb->getAttachment();
	UserId* user = attachment->att_user;
	fb_assert(user);

	if (trusted)
		user->setRoleTrusted();
	else
	{
		if (!SCL_role_granted(tdbb, *user, roleName.c_str()))
			(Arg::Gds(isc_set_invalid_role) << roleName).raise();

		user->setSqlRole(roleName.c_str());
	}

	SCL_release_all(attachment->att_security_classes);
}


//--------------------


namespace
{

struct TextCode
{
	const char* name;
	USHORT val;
};

//#define FB_TEXTCODE(x) { STRINGIZE(x), x }
#define FB_TEXTCODE(x) { #x, x }

const TextCode roundModes[] = {
	FB_TEXTCODE(DEC_ROUND_CEILING),
	FB_TEXTCODE(DEC_ROUND_UP),
	FB_TEXTCODE(DEC_ROUND_HALF_UP),
	FB_TEXTCODE(DEC_ROUND_HALF_EVEN),
	FB_TEXTCODE(DEC_ROUND_HALF_DOWN),
	FB_TEXTCODE(DEC_ROUND_DOWN),
	FB_TEXTCODE(DEC_ROUND_FLOOR),
	{ "DEC_ROUND_REROUND", DEC_ROUND_05UP },
	{ NULL, 0 }
};

//DEC_ROUND_
//0123456789
const unsigned FB_RMODE_OFFSET = 10;

const TextCode ieeeTraps[] = {
	FB_TEXTCODE(DEC_IEEE_754_Division_by_zero),
	FB_TEXTCODE(DEC_IEEE_754_Inexact),
	FB_TEXTCODE(DEC_IEEE_754_Invalid_operation),
	FB_TEXTCODE(DEC_IEEE_754_Overflow),
	FB_TEXTCODE(DEC_IEEE_754_Underflow),
	{ NULL, 0 }
};

//DEC_IEEE_754_
//0123456789012
const unsigned FB_TRAPS_OFFSET = 13;

#undef FB_TEXTCODE

const TextCode* getCodeByText(const MetaName& text, const TextCode* textCode, unsigned offset)
{
	NoCaseString name(text.c_str(), text.length());

	for (const TextCode* tc = textCode; tc->name; ++tc)
	{
		if (name == &tc->name[offset])
			return tc;
	}

	return nullptr;
}

}


//--------------------


SetRoundNode::SetRoundNode(MemoryPool& pool, Firebird::MetaName* name)
	: SessionManagementNode(pool)
{
	fb_assert(name);
	const TextCode* mode = getCodeByText(*name, roundModes, FB_RMODE_OFFSET);
	if (!mode)
		(Arg::Gds(isc_decfloat_round) << *name).raise();
	rndMode = mode->val;
}

void SetRoundNode::execute(thread_db* tdbb, dsql_req* /*request*/, jrd_tra** /*traHandle*/) const
{
	SET_TDBB(tdbb);
	Attachment* const attachment = tdbb->getAttachment();
	attachment->att_dec_status.roundingMode = rndMode;
}


//--------------------


void SetTrapsNode::trap(Firebird::MetaName* name)
{
	fb_assert(name);
	const TextCode* trap = getCodeByText(*name, ieeeTraps, FB_TRAPS_OFFSET);
	if (!trap)
		(Arg::Gds(isc_decfloat_trap) << *name).raise();
	traps |= trap->val;
}

void SetTrapsNode::execute(thread_db* tdbb, dsql_req* /*request*/, jrd_tra** /*traHandle*/) const
{
	SET_TDBB(tdbb);
	Attachment* const attachment = tdbb->getAttachment();
	attachment->att_dec_status.decExtFlag = traps;
}


//--------------------


void SetBindNode::execute(thread_db* tdbb, dsql_req* /*request*/, jrd_tra** /*traHandle*/) const
{
	SET_TDBB(tdbb);
	Attachment* const attachment = tdbb->getAttachment();
	attachment->att_dec_binding = bind;
}


//--------------------


SetSessionNode::SetSessionNode(MemoryPool& pool, Type aType, ULONG aVal, UCHAR blr_timepart)
	: SessionManagementNode(pool),
	  m_type(aType),
	  m_value(0)
{
	// TYPE_IDLE_TIMEOUT should be set in seconds
	// TYPE_STMT_TIMEOUT should be set in milliseconds

	ULONG mult = 1;

	switch (blr_timepart)
	{
	case blr_extract_hour:
		mult = (aType == TYPE_IDLE_TIMEOUT) ? 3660 : 3660000;
		break;

	case blr_extract_minute:
		mult = (aType == TYPE_IDLE_TIMEOUT) ? 60 : 60000;
		break;

	case blr_extract_second:
		mult = (aType == TYPE_IDLE_TIMEOUT) ? 1 : 1000;
		break;

	case blr_extract_millisecond:
		if (aType == TYPE_IDLE_TIMEOUT)
			Arg::Gds(isc_invalid_extractpart_time).raise();
		mult = 1;
		break;

	default:
		Arg::Gds(isc_invalid_extractpart_time).raise();
		break;
	}

	m_value = aVal * mult;
}

string SetSessionNode::internalPrint(NodePrinter& printer) const
{
	Node::internalPrint(printer);

	NODE_PRINT(printer, m_type);
	NODE_PRINT(printer, m_value);

	return "SetSessionNode";
}

void SetSessionNode::execute(thread_db* tdbb, dsql_req* request, jrd_tra** /*traHandle*/) const
{
	Attachment* att = tdbb->getAttachment();

	switch (m_type)
	{
	case TYPE_IDLE_TIMEOUT:
		att->setIdleTimeout(m_value);
		break;

	case TYPE_STMT_TIMEOUT:
		att->setStatementTimeout(m_value);
		break;
	}
}


//--------------------


void SetTimeZoneNode::execute(thread_db* tdbb, dsql_req* request, jrd_tra** /*traHandle*/) const
{
	Attachment* const attachment = tdbb->getAttachment();

	if (local)
		attachment->att_current_timezone = attachment->att_original_timezone;
	else
		attachment->att_current_timezone = TimeZoneUtil::parse(str.c_str(), str.length());
}


//--------------------


StmtNode* UpdateOrInsertNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	thread_db* tdbb = JRD_get_thread_data(); // necessary?
	MemoryPool& pool = dsqlScratch->getPool();

	if (!dsqlScratch->isPsql())
		dsqlScratch->flags |= DsqlCompilerScratch::FLAG_UPDATE_OR_INSERT;

	const MetaName& relation_name = nodeAs<RelationSourceNode>(relation)->dsqlName;
	MetaName base_name = relation_name;

	bool needSavePoint;

	// Build the INSERT node.
	StoreNode* insert = FB_NEW_POOL(pool) StoreNode(pool);
	insert->dsqlRelation = relation;
	insert->dsqlFields = fields;
	insert->dsqlValues = values;
	insert->dsqlReturning = returning;
	insert->overrideClause = overrideClause;
	insert = nodeAs<StoreNode>(insert->internalDsqlPass(dsqlScratch, true, needSavePoint));
	fb_assert(insert);

	dsql_ctx* context = insert->dsqlRelation->dsqlContext;
	DEV_BLKCHK(context, dsql_type_ctx);

	dsql_rel* ctxRelation = context->ctx_relation;
	Array<NestConst<FieldNode> > fieldsCopy = fields;

	// If a field list isn't present, build one using the same rules of INSERT INTO table VALUES ...
	if (fieldsCopy.isEmpty())
		dsqlExplodeFields(ctxRelation, fieldsCopy);

	// Maintain a pair of view's field name / base field name.
	MetaNamePairMap view_fields;

	if ((ctxRelation->rel_flags & REL_view) && matching.isEmpty())
	{
		dsql_rel* base_rel = METD_get_view_base(dsqlScratch->getTransaction(), dsqlScratch,
			relation_name.c_str(), view_fields);

		// Get the base table name if there is only one.
		if (base_rel)
			base_name = base_rel->rel_name;
		else
			ERRD_post(Arg::Gds(isc_upd_ins_with_complex_view));
	}

	Array<NestConst<FieldNode> > matchingCopy = matching;
	UCHAR equality_type;

	if (matching.hasData())
	{
		equality_type = blr_equiv;

		dsqlScratch->context->push(context);
		++dsqlScratch->scopeLevel;

		Array<NestConst<ValueExprNode> > matchingFields;

		for (NestConst<FieldNode>* i = matchingCopy.begin(); i != matchingCopy.end(); ++i)
		{
			PsqlChanger changer(dsqlScratch, false);
			matchingFields.add((*i)->dsqlPass(dsqlScratch));
		}

		--dsqlScratch->scopeLevel;
		dsqlScratch->context->pop();

		dsqlFieldAppearsOnce(matchingFields, "UPDATE OR INSERT");
	}
	else
	{
		equality_type = blr_eql;

		METD_get_primary_key(dsqlScratch->getTransaction(), base_name.c_str(), matchingCopy);

		if (matchingCopy.isEmpty())
			ERRD_post(Arg::Gds(isc_primary_key_required) << base_name);
	}

	// Build a boolean to use in the UPDATE dsqlScratch.
	BoolExprNode* match = NULL;
	USHORT matchCount = 0;

	CompoundStmtNode* list = FB_NEW_POOL(pool) CompoundStmtNode(pool);

	CompoundStmtNode* assignments = FB_NEW_POOL(pool) CompoundStmtNode(pool);
	NestConst<FieldNode>* fieldPtr = fieldsCopy.begin();
	NestConst<ValueExprNode>* valuePtr = values->items.begin();

	Array<NestConst<StmtNode> >& insertStatements =
		nodeAs<CompoundStmtNode>(insert->statement)->statements;

	for (; fieldPtr != fieldsCopy.end(); ++fieldPtr, ++valuePtr)
	{
		AssignmentNode* assign = FB_NEW_POOL(pool) AssignmentNode(pool);

		if (!(assign->asgnFrom = *valuePtr))	// it's NULL for DEFAULT
			assign->asgnFrom = FB_NEW_POOL(pool) DefaultNode(pool, relation_name, (*fieldPtr)->dsqlName);

		assign->asgnTo = *fieldPtr;
		assignments->statements.add(assign);

		// When relation is a view and MATCHING was not specified, fieldName
		// stores the base field name that is what we should find in the primary
		// key of base table.
		MetaName fieldName;

		if ((ctxRelation->rel_flags & REL_view) && matching.isEmpty())
			view_fields.get((*fieldPtr)->dsqlName, fieldName);
		else
			fieldName = (*fieldPtr)->dsqlName;

		if (fieldName.hasData())
		{
			for (NestConst<FieldNode>* matchingPtr = matchingCopy.begin();
				 matchingPtr != matchingCopy.end();
				 ++matchingPtr)
			{
				const MetaName testField = (*matchingPtr)->dsqlName;

				if (testField == fieldName)
				{
					if (!*valuePtr)	// it's NULL for DEFAULT
						ERRD_post(Arg::Gds(isc_upd_ins_cannot_default) << fieldName);

					++matchCount;

					const FB_SIZE_T fieldPos = fieldPtr - fieldsCopy.begin();
					AssignmentNode* assign2 = nodeAs<AssignmentNode>(insertStatements[fieldPos]);
					NestConst<ValueExprNode>& expr = assign2->asgnFrom;
					ValueExprNode* var = dsqlPassHiddenVariable(dsqlScratch, expr);

					if (var)
					{
						AssignmentNode* varAssign = FB_NEW_POOL(pool) AssignmentNode(pool);
						varAssign->asgnFrom = expr;
						varAssign->asgnTo = var;

						list->statements.add(varAssign);

						assign2->asgnFrom = expr = var;
					}
					else
						var = *valuePtr;

					ComparativeBoolNode* eqlNode = FB_NEW_POOL(pool) ComparativeBoolNode(pool,
						equality_type, *fieldPtr, var);

					match = PASS1_compose(match, eqlNode, blr_and);
				}
			}
		}
	}

	// check if implicit or explicit MATCHING is valid
	if (matchCount != matchingCopy.getCount())
	{
		if (matching.hasData())
			ERRD_post(Arg::Gds(isc_upd_ins_doesnt_match_matching));
		else
			ERRD_post(Arg::Gds(isc_upd_ins_doesnt_match_pk) << base_name);
	}

	// build the UPDATE node
	ModifyNode* update = FB_NEW_POOL(pool) ModifyNode(pool);
	update->dsqlRelation = relation;
	update->statement = assignments;
	update->dsqlBoolean = match;

	if (returning)
	{
		update->dsqlRseFlags = RecordSourceNode::DFLAG_SINGLETON;
		update->statement2 = ReturningProcessor::clone(
			dsqlScratch, returning, insert->statement2);
	}

	update = nodeAs<ModifyNode>(update->internalDsqlPass(dsqlScratch, true));
	fb_assert(update);

	// test if ROW_COUNT = 0

	NestConst<BoolExprNode> eqlNode = FB_NEW_POOL(pool) ComparativeBoolNode(pool, blr_eql,
		FB_NEW_POOL(pool) InternalInfoNode(pool, MAKE_const_slong(INFO_TYPE_ROWS_AFFECTED)),
		MAKE_const_slong(0));

	const ULONG save_flags = dsqlScratch->flags;
	dsqlScratch->flags |= DsqlCompilerScratch::FLAG_BLOCK;	// to compile ROW_COUNT
	eqlNode = doDsqlPass(dsqlScratch, eqlNode);
	dsqlScratch->flags = save_flags;

	// If (ROW_COUNT = 0) then INSERT.
	IfNode* ifNode = FB_NEW_POOL(pool) IfNode(pool);
	ifNode->condition = eqlNode;
	ifNode->trueAction = insert;

	// Build the temporary vars / UPDATE / IF nodes.

	list->statements.add(update);
	list->statements.add(ifNode);

	// If RETURNING is present, type is already DsqlCompiledStatement::TYPE_EXEC_PROCEDURE.
	if (!returning)
		dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_INSERT);

	StmtNode* ret = SavepointEncloseNode::make(dsqlScratch->getPool(), dsqlScratch, list);
	if (!needSavePoint || nodeIs<SavepointEncloseNode>(ret))
		return ret;

	return FB_NEW SavepointEncloseNode(dsqlScratch->getPool(), ret);
}

string UpdateOrInsertNode::internalPrint(NodePrinter& printer) const
{
	DsqlOnlyStmtNode::internalPrint(printer);

	NODE_PRINT(printer, relation);
	NODE_PRINT(printer, fields);
	NODE_PRINT(printer, values);
	NODE_PRINT(printer, matching);
	NODE_PRINT(printer, returning);

	return "UpdateOrInsertNode";
}

void UpdateOrInsertNode::genBlr(DsqlCompilerScratch* /*dsqlScratch*/)
{
}


//--------------------


// Generate a field list that correspond to table fields.
template <typename T>
static void dsqlExplodeFields(dsql_rel* relation, Array<NestConst<T> >& fields)
{
	thread_db* tdbb = JRD_get_thread_data();
	MemoryPool& pool = *tdbb->getDefaultPool();

	for (dsql_fld* field = relation->rel_fields; field; field = field->fld_next)
	{
		// CVC: Ann Harrison requested to skip COMPUTED fields in INSERT w/o field list.
		// ASF: But not for views - CORE-5454
		if (!(relation->rel_flags & REL_view) && (field->flags & FLD_computed))
			continue;

		FieldNode* fieldNode = FB_NEW_POOL(pool) FieldNode(pool);
		fieldNode->dsqlName = field->fld_name.c_str();
		fields.add(fieldNode);
	}
}

// Find dbkey for named relation in statement's saved dbkeys.
static dsql_par* dsqlFindDbKey(const dsql_req* request, const RelationSourceNode* relation_name)
{
	DEV_BLKCHK(request, dsql_type_req);
	DEV_BLKCHK(relation_name, dsql_type_nod);

	const dsql_msg* message = request->getStatement()->getReceiveMsg();
	dsql_par* candidate = NULL;
	const MetaName& relName = relation_name->dsqlName;

	for (FB_SIZE_T i = 0; i < message->msg_parameters.getCount(); ++i)
	{
		dsql_par* parameter = message->msg_parameters[i];

		if (parameter->par_dbkey_relname.hasData() && parameter->par_dbkey_relname == relName)
		{
			if (candidate)
				return NULL;

			candidate = parameter;
		}
	}

	return candidate;
}

// Find record version for relation in statement's saved record version.
static dsql_par* dsqlFindRecordVersion(const dsql_req* request, const RelationSourceNode* relation_name)
{
	DEV_BLKCHK(request, dsql_type_req);

	const dsql_msg* message = request->getStatement()->getReceiveMsg();
	dsql_par* candidate = NULL;
	const MetaName& relName = relation_name->dsqlName;

	for (FB_SIZE_T i = 0; i < message->msg_parameters.getCount(); ++i)
	{
		dsql_par* parameter = message->msg_parameters[i];

		if (parameter->par_rec_version_relname.hasData() &&
			parameter->par_rec_version_relname == relName)
		{
			if (candidate)
				return NULL;

			candidate = parameter;
		}
	}

	return candidate;
}

// Generate DML header for INSERT/UPDATE/DELETE.
static const dsql_msg* dsqlGenDmlHeader(DsqlCompilerScratch* dsqlScratch, RseNode* dsqlRse)
{
	const dsql_msg* message = NULL;
	const bool innerSend = !dsqlRse || (dsqlScratch->flags & DsqlCompilerScratch::FLAG_UPDATE_OR_INSERT);
	const bool merge = dsqlScratch->flags & DsqlCompilerScratch::FLAG_MERGE;

	if (dsqlScratch->getStatement()->getType() == DsqlCompiledStatement::TYPE_EXEC_PROCEDURE &&
		!innerSend && !merge)
	{
		if ((message = dsqlScratch->getStatement()->getReceiveMsg()))
		{
			dsqlScratch->appendUChar(blr_send);
			dsqlScratch->appendUChar(message->msg_number);
		}
	}

	if (dsqlRse)
	{
		dsqlScratch->appendUChar(blr_for);
		GEN_expr(dsqlScratch, dsqlRse);
	}

	if (dsqlScratch->getStatement()->getType() == DsqlCompiledStatement::TYPE_EXEC_PROCEDURE)
	{
		if ((message = dsqlScratch->getStatement()->getReceiveMsg()))
		{
			dsqlScratch->appendUChar(blr_begin);

			if (innerSend && !merge)
			{
				dsqlScratch->appendUChar(blr_send);
				dsqlScratch->appendUChar(message->msg_number);
			}
		}
	}

	return message;
}

// Get the context of a relation, procedure or derived table.
static dsql_ctx* dsqlGetContext(const RecordSourceNode* node)
{
	const ProcedureSourceNode* procNode;
	const RelationSourceNode* relNode;
	const RseNode* rseNode;

	if ((procNode = nodeAs<ProcedureSourceNode>(node)))
		return procNode->dsqlContext;

	if ((relNode = nodeAs<RelationSourceNode>(node)))
		return relNode->dsqlContext;

	if ((rseNode = nodeAs<RseNode>(node)))
		return rseNode->dsqlContext;

	fb_assert(false);
	return NULL;
}

// Get the contexts of a relation, procedure, derived table or a list of joins.
static void dsqlGetContexts(DsqlContextStack& contexts, const RecordSourceNode* node)
{
	const ProcedureSourceNode* procNode;
	const RelationSourceNode* relNode;
	const RseNode* rseNode;

	if ((procNode = nodeAs<ProcedureSourceNode>(node)))
		contexts.push(procNode->dsqlContext);
	else if ((relNode = nodeAs<RelationSourceNode>(node)))
		contexts.push(relNode->dsqlContext);
	else if ((rseNode = nodeAs<RseNode>(node)))
	{
		if (rseNode->dsqlContext)	// derived table
			contexts.push(rseNode->dsqlContext);
		else	// joins
		{
			NestConst<RecSourceListNode> streamList = rseNode->dsqlStreams;

			for (NestConst<RecordSourceNode>* ptr = streamList->items.begin();
				 ptr != streamList->items.end();
				 ++ptr)
			{
				dsqlGetContexts(contexts, *ptr);
			}
		}
	}
	else
	{
		fb_assert(false);
	}
}

// Create a compound statement to initialize returning parameters.
static StmtNode* dsqlNullifyReturning(DsqlCompilerScratch* dsqlScratch, StmtNode* input, bool returnList)
{
	thread_db* tdbb = JRD_get_thread_data();
	MemoryPool& pool = *tdbb->getDefaultPool();

	StmtNode* returning = NULL;
	EraseNode* eraseNode;
	ModifyNode* modifyNode;
	StoreNode* storeNode;

	if (eraseNode = nodeAs<EraseNode>(input))
		returning = eraseNode->statement;
	else if (modifyNode = nodeAs<ModifyNode>(input))
		returning = modifyNode->statement2;
	else if (storeNode = nodeAs<StoreNode>(input))
		returning = storeNode->statement2;
	else
	{
		fb_assert(false);
	}

	if (dsqlScratch->isPsql() || !returning)
		return returnList ? input : NULL;

	// If this is a RETURNING in DSQL, we need to initialize the output
	// parameters with NULL, to return in case of empty resultset.
	// Note: this may be changed in the future, i.e. return empty resultset
	// instead of NULLs. In this case, I suppose this function could be
	// completely removed.

	// nod_returning was already processed
	CompoundStmtNode* returningStmt = nodeAs<CompoundStmtNode>(returning);
	fb_assert(returningStmt);

	CompoundStmtNode* nullAssign = FB_NEW_POOL(pool) CompoundStmtNode(pool);

	NestConst<StmtNode>* ret_ptr = returningStmt->statements.begin();
	NestConst<StmtNode>* null_ptr = nullAssign->statements.getBuffer(returningStmt->statements.getCount());

	for (const NestConst<StmtNode>* const end = ret_ptr + returningStmt->statements.getCount();
		 ret_ptr != end;
		 ++ret_ptr, ++null_ptr)
	{
		AssignmentNode* assign = FB_NEW_POOL(pool) AssignmentNode(pool);
		assign->asgnFrom = FB_NEW_POOL(pool) NullNode(pool);
		assign->asgnTo = nodeAs<AssignmentNode>(*ret_ptr)->asgnTo;
		*null_ptr = assign;
	}

	// If asked for, return a compound statement with the initialization and the
	// original statement.
	if (returnList)
	{
		CompoundStmtNode* list = FB_NEW_POOL(pool) CompoundStmtNode(pool);
		list->statements.add(nullAssign);
		list->statements.add(input);
		return list;
	}

	return nullAssign;	// return the initialization statement.
}

// Check that a field is named only once in INSERT or UPDATE statements.
static void dsqlFieldAppearsOnce(const Array<NestConst<ValueExprNode> >& values, const char* command)
{
	for (FB_SIZE_T i = 0; i < values.getCount(); ++i)
	{
		const FieldNode* field1 = nodeAs<FieldNode>(values[i]);
		if (!field1)
			continue;

		const MetaName& name1 = field1->dsqlField->fld_name;

		for (FB_SIZE_T j = i + 1; j < values.getCount(); ++j)
		{
			const FieldNode* field2 = nodeAs<FieldNode>(values[j]);
			if (!field2)
				continue;

			const MetaName& name2 = field2->dsqlField->fld_name;

			if (name1 == name2)
			{
				string str = field1->dsqlContext->ctx_relation->rel_name.c_str();
				str += ".";
				str += name1.c_str();

				//// FIXME: line/column is not very accurate for MERGE ... INSERT.
				ERRD_post(
					Arg::Gds(isc_sqlerr) << Arg::Num(-206) <<
					Arg::Gds(isc_dsql_no_dup_name) << str << command <<
					Arg::Gds(isc_dsql_line_col_error) <<
						Arg::Num(values[j]->line) << Arg::Num(values[j]->column));
			}
		}
	}
}

static ValueListNode* dsqlPassArray(DsqlCompilerScratch* dsqlScratch, ValueListNode* input)
{
	if (!input)
		return NULL;

	MemoryPool& pool = dsqlScratch->getPool();
	ValueListNode* output = FB_NEW_POOL(pool) ValueListNode(pool, input->items.getCount());
	NestConst<ValueExprNode>* ptr = input->items.begin();
	NestConst<ValueExprNode>* ptr2 = output->items.begin();

	for (const NestConst<ValueExprNode>* const end = input->items.end(); ptr != end; ++ptr, ++ptr2)
		*ptr2 = Node::doDsqlPass(dsqlScratch, *ptr);

	return output;
}

// Turn a cursor reference into a record selection expression.
static dsql_ctx* dsqlPassCursorContext(DsqlCompilerScratch* dsqlScratch, const MetaName& cursor,
	const RelationSourceNode* relation_name)
{
	DEV_BLKCHK(dsqlScratch, dsql_type_req);

	const MetaName& relName = relation_name->dsqlName;

	// this function must throw an error if no cursor was found
	const DeclareCursorNode* node = PASS1_cursor_name(dsqlScratch, cursor,
		DeclareCursorNode::CUR_TYPE_ALL, true);
	fb_assert(node);

	const RseNode* nodeRse = nodeAs<RseNode>(node->rse);
	fb_assert(nodeRse);

	if (nodeRse->dsqlDistinct)
	{
		// cursor with DISTINCT is not updatable
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-510) <<
				  Arg::Gds(isc_dsql_cursor_update_err) << cursor);
	}

	NestConst<RecSourceListNode> temp = nodeRse->dsqlStreams;
	dsql_ctx* context = NULL;

	NestConst<RecordSourceNode>* ptr = temp->items.begin();

	for (const NestConst<RecordSourceNode>* const end = temp->items.end(); ptr != end; ++ptr)
	{
		RecordSourceNode* r_node = *ptr;
		RelationSourceNode* relNode = nodeAs<RelationSourceNode>(r_node);

		if (relNode)
		{
			dsql_ctx* candidate = relNode->dsqlContext;
			DEV_BLKCHK(candidate, dsql_type_ctx);
			const dsql_rel* relation = candidate->ctx_relation;

			if (relation->rel_name == relName)
			{
				if (context)
				{
					ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-504) <<
							  Arg::Gds(isc_dsql_cursor_err) <<
							  Arg::Gds(isc_dsql_cursor_rel_ambiguous) << Arg::Str(relName) <<
																		 cursor);
				}
				else
					context = candidate;
			}
		}
		else if (nodeAs<AggregateSourceNode>(r_node))
		{
			// cursor with aggregation is not updatable
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-510) <<
					  Arg::Gds(isc_dsql_cursor_update_err) << cursor);
		}
		// note that UnionSourceNode and joins will cause the error below,
		// as well as derived tables. Some cases deserve fixing in the future
	}

	if (!context)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-504) <<
				  Arg::Gds(isc_dsql_cursor_err) <<
				  Arg::Gds(isc_dsql_cursor_rel_not_found) << Arg::Str(relName) << cursor);
	}

	return context;
}

// Turn a cursor reference into a record selection expression.
static RseNode* dsqlPassCursorReference(DsqlCompilerScratch* dsqlScratch, const MetaName& cursor,
	RelationSourceNode* relation_name)
{
	DEV_BLKCHK(dsqlScratch, dsql_type_req);

	thread_db* tdbb = JRD_get_thread_data();
	MemoryPool& pool = *tdbb->getDefaultPool();

	// Lookup parent dsqlScratch

	dsql_req* const* const symbol = dsqlScratch->getAttachment()->dbb_cursors.get(cursor.c_str());

	if (!symbol)
	{
		// cursor is not found
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-504) <<
				  Arg::Gds(isc_dsql_cursor_err) <<
				  Arg::Gds(isc_dsql_cursor_not_found) << cursor);
	}

	dsql_req* parent = *symbol;

	// Verify that the cursor is appropriate and updatable

	dsql_par* source = dsqlFindDbKey(parent, relation_name);
	dsql_par* rv_source = dsqlFindRecordVersion(parent, relation_name);

	if (!source || !rv_source)
	{
		// cursor is not updatable
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-510) <<
				  Arg::Gds(isc_dsql_cursor_update_err) << cursor);
	}

	DsqlCompiledStatement* const statement = dsqlScratch->getStatement();

	statement->setParentRequest(parent);
	statement->setParentDbKey(source);
	statement->setParentRecVersion(rv_source);
	parent->cursors.add(statement);

	// Build record selection expression

	RseNode* rse = FB_NEW_POOL(pool) RseNode(pool);
	rse->dsqlStreams = FB_NEW_POOL(pool) RecSourceListNode(pool, 1);
	RelationSourceNode* relation_node = nodeAs<RelationSourceNode>(PASS1_relation(dsqlScratch, relation_name));
	rse->dsqlStreams->items[0] = relation_node;

	RecordKeyNode* dbKeyNode = FB_NEW_POOL(pool) RecordKeyNode(pool, blr_dbkey);
	dbKeyNode->dsqlRelation = relation_node;

	dsql_par* parameter = MAKE_parameter(statement->getSendMsg(), false, false, 0, NULL);
	statement->setDbKey(parameter);

	ParameterNode* paramNode = FB_NEW_POOL(pool) ParameterNode(pool);
	paramNode->dsqlParameterIndex = parameter->par_index;
	paramNode->dsqlParameter = parameter;
	parameter->par_desc = source->par_desc;

	ComparativeBoolNode* eqlNode1 =
		FB_NEW_POOL(pool) ComparativeBoolNode(pool, blr_eql, dbKeyNode, paramNode);

	dbKeyNode = FB_NEW_POOL(pool) RecordKeyNode(pool, blr_record_version);
	dbKeyNode->dsqlRelation = relation_node;

	parameter = MAKE_parameter(statement->getSendMsg(), false, false, 0, NULL);
	statement->setRecVersion(parameter);

	paramNode = FB_NEW_POOL(pool) ParameterNode(pool);
	paramNode->dsqlParameterIndex = parameter->par_index;
	paramNode->dsqlParameter = parameter;
	parameter->par_desc = rv_source->par_desc;

	ComparativeBoolNode* eqlNode2 =
		FB_NEW_POOL(pool) ComparativeBoolNode(pool, blr_eql, dbKeyNode, paramNode);

	rse->dsqlWhere = PASS1_compose(eqlNode1, eqlNode2, blr_and);

	return rse;
}

// Create (if necessary) a hidden variable to store a temporary value.
static VariableNode* dsqlPassHiddenVariable(DsqlCompilerScratch* dsqlScratch, ValueExprNode* expr)
{
	thread_db* tdbb = JRD_get_thread_data();

	// For some node types, it's better to not create temporary value.
	switch (expr->type)
	{
		case ExprNode::TYPE_CURRENT_DATE:
		case ExprNode::TYPE_CURRENT_TIME:
		case ExprNode::TYPE_CURRENT_TIMESTAMP:
		case ExprNode::TYPE_CURRENT_ROLE:
		case ExprNode::TYPE_CURRENT_USER:
		case ExprNode::TYPE_FIELD:
		case ExprNode::TYPE_INTERNAL_INFO:
		case ExprNode::TYPE_LITERAL:
		case ExprNode::TYPE_NULL:
		case ExprNode::TYPE_PARAMETER:
		case ExprNode::TYPE_RECORD_KEY:
		case ExprNode::TYPE_VARIABLE:
			return NULL;
	}

	VariableNode* varNode = FB_NEW_POOL(*tdbb->getDefaultPool()) VariableNode(*tdbb->getDefaultPool());
	varNode->dsqlVar = dsqlScratch->makeVariable(NULL, "", dsql_var::TYPE_HIDDEN,
		0, 0, dsqlScratch->hiddenVarsNumber++);

	MAKE_desc(dsqlScratch, &varNode->dsqlVar->desc, expr);
	varNode->nodDesc = varNode->dsqlVar->desc;

	return varNode;
}

// Process loop interruption.
static USHORT dsqlPassLabel(DsqlCompilerScratch* dsqlScratch, bool breakContinue, MetaName* label)
{
	// look for a label, if specified

	USHORT position = 0;

	if (label)
	{
		int index = dsqlScratch->loopLevel;

		for (Stack<MetaName*>::iterator stack(dsqlScratch->labels); stack.hasData(); ++stack)
		{
			const MetaName* obj = stack.object();
			if (obj && *label == *obj)
			{
				position = index;
				break;
			}

			--index;
		}
	}

	USHORT number = 0;

	if (breakContinue)
	{
		if (position > 0)
		{
			// break/continue the specified loop
			number = position;
		}
		else if (label)
		{
			// ERROR: Label %s is not found in the current scope
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
					  Arg::Gds(isc_dsql_command_err) <<
					  Arg::Gds(isc_dsql_invalid_label) << *label <<
														  Arg::Str("is not found"));
		}
		else
		{
			// break/continue the current loop
			number = dsqlScratch->loopLevel;
		}
	}
	else
	{
		if (position > 0)
		{
			// ERROR: Label %s already exists in the current scope
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
					  Arg::Gds(isc_dsql_command_err) <<
					  Arg::Gds(isc_dsql_invalid_label) << *label <<
					  Arg::Str("already exists"));
		}
		else
		{
			// store label name, if specified
			dsqlScratch->labels.push(label);
			number = dsqlScratch->loopLevel;
		}
	}

	fb_assert(number > 0 && number <= dsqlScratch->loopLevel);

	return number;
}

// Compile a RETURNING clause (nod_returning or not).
static StmtNode* dsqlProcessReturning(DsqlCompilerScratch* dsqlScratch, ReturningClause* input,
	StmtNode* stmt)
{
	thread_db* tdbb = JRD_get_thread_data();

	if (stmt)
	{
		const bool isPsql = dsqlScratch->isPsql();

		PsqlChanger changer(dsqlScratch, false);
		stmt = stmt->dsqlPass(dsqlScratch);

		if (!isPsql)
			dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_EXEC_PROCEDURE);

		return stmt;
	}

	if (!input)
		return NULL;

	MemoryPool& pool = *tdbb->getDefaultPool();

	ValueListNode* const source = Node::doDsqlPass(dsqlScratch, input->first, false);

	dsqlScratch->flags |= DsqlCompilerScratch::FLAG_RETURNING_INTO;
	ValueListNode* target = dsqlPassArray(dsqlScratch, input->second);
	dsqlScratch->flags &= ~DsqlCompilerScratch::FLAG_RETURNING_INTO;

	if (!dsqlScratch->isPsql() && target)
	{
		// RETURNING INTO is not allowed syntax for DSQL
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  // Token unknown
				  Arg::Gds(isc_token_err) <<
				  Arg::Gds(isc_random) << Arg::Str("INTO"));
	}
	else if (dsqlScratch->isPsql() && !target)
	{
		// This trick because we don't copy lexer positions when copying lists.
		const ValueListNode* errSrc = input->first;
		// RETURNING without INTO is not allowed for PSQL
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  // Unexpected end of command
				  Arg::Gds(isc_command_end_err2) << Arg::Num(errSrc->line) <<
													Arg::Num(errSrc->column));
	}

	const unsigned int count = source->items.getCount();
	fb_assert(count);

	CompoundStmtNode* node = FB_NEW_POOL(pool) CompoundStmtNode(pool);

	if (target)
	{
		// PSQL case
		fb_assert(dsqlScratch->isPsql());

		if (count != target->items.getCount())
		{
			// count of column list and value list don't match
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-804) <<
					  Arg::Gds(isc_dsql_var_count_err));
		}

		NestConst<ValueExprNode>* src = source->items.begin();
		NestConst<ValueExprNode>* dst = target->items.begin();

		for (const NestConst<ValueExprNode>* const end = source->items.end(); src != end; ++src, ++dst)
		{
			AssignmentNode* temp = FB_NEW_POOL(pool) AssignmentNode(pool);
			temp->asgnFrom = *src;
			temp->asgnTo = *dst;

			node->statements.add(temp);
		}
	}
	else
	{
		// DSQL case
		fb_assert(!dsqlScratch->isPsql());

		NestConst<ValueExprNode>* src = source->items.begin();

		for (const NestConst<ValueExprNode>* const end = source->items.end(); src != end; ++src)
		{
			dsql_par* parameter = MAKE_parameter(dsqlScratch->getStatement()->getReceiveMsg(),
				true, true, 0, *src);
			parameter->par_node = *src;
			MAKE_desc(dsqlScratch, &parameter->par_desc, *src);
			parameter->par_desc.dsc_flags |= DSC_nullable;

			ParameterNode* paramNode = FB_NEW_POOL(*tdbb->getDefaultPool()) ParameterNode(
				*tdbb->getDefaultPool());
			paramNode->dsqlParameterIndex = parameter->par_index;
			paramNode->dsqlParameter = parameter;

			AssignmentNode* temp = FB_NEW_POOL(pool) AssignmentNode(pool);
			temp->asgnFrom = *src;
			temp->asgnTo = paramNode;

			node->statements.add(temp);
		}
	}

	if (!dsqlScratch->isPsql())
		dsqlScratch->getStatement()->setType(DsqlCompiledStatement::TYPE_EXEC_PROCEDURE);

	return node;
}

// Setup parameter name.
// This function was added as a part of array data type support for InterClient. It is called when
// either "insert" or "update" statements are parsed. If the statements have input parameters, then
// the parameter is assigned the name of the field it is being inserted (or updated). The same goes
// to the name of a relation.
// The names are assigned to the parameter only if the field is of array data type.
static void dsqlSetParameterName(DsqlCompilerScratch* dsqlScratch, ExprNode* exprNode, const ValueExprNode* fld_node,
	const dsql_rel* relation)
{
	DEV_BLKCHK(fld_node, dsql_type_nod);
	DEV_BLKCHK(relation, dsql_type_dsql_rel);

	if (!exprNode)
		return;

	const FieldNode* fieldNode = nodeAs<FieldNode>(fld_node);
	fb_assert(fieldNode);	// Could it be something else ???

	if (fieldNode->nodDesc.dsc_dtype != dtype_array)
		return;

	switch (exprNode->type)
	{
		case ExprNode::TYPE_ARITHMETIC:
		case ExprNode::TYPE_CONCATENATE:
		case ExprNode::TYPE_EXTRACT:
		case ExprNode::TYPE_NEGATE:
		case ExprNode::TYPE_STR_CASE:
		case ExprNode::TYPE_STR_LEN:
		case ExprNode::TYPE_SUBSTRING:
		case ExprNode::TYPE_SUBSTRING_SIMILAR:
		case ExprNode::TYPE_TRIM:
		{
			NodeRefsHolder holder(dsqlScratch->getPool());
			exprNode->getChildren(holder, true);

			for (auto ref : holder.refs)
				dsqlSetParameterName(dsqlScratch, ref->getExpr(), fld_node, relation);

			break;
		}

		case ExprNode::TYPE_PARAMETER:
		{
			ParameterNode* paramNode = nodeAs<ParameterNode>(exprNode);
			dsql_par* parameter = paramNode->dsqlParameter;
			parameter->par_name = fieldNode->dsqlField->fld_name.c_str();
			parameter->par_rel_name = relation->rel_name.c_str();
			break;
		}
	}
}

// Setup parameter parameters name.
static void dsqlSetParametersName(DsqlCompilerScratch* dsqlScratch, CompoundStmtNode* statements,
	const RecordSourceNode* relNode)
{
	const dsql_ctx* context = relNode->dsqlContext;
	DEV_BLKCHK(context, dsql_type_ctx);
	const dsql_rel* relation = context->ctx_relation;

	FB_SIZE_T count = statements->statements.getCount();
	NestConst<StmtNode>* ptr = statements->statements.begin();

	for (NestConst<StmtNode>* const end = ptr + count; ptr != end; ++ptr)
	{
		AssignmentNode* assign = nodeAs<AssignmentNode>(*ptr);

		if (assign)
			dsqlSetParameterName(dsqlScratch, assign->asgnFrom, assign->asgnTo, relation);
		else
			fb_assert(false);
	}
}

// Perform cleaning of rpb, zeroing unassigned fields and the impure tail of varying fields that
// we don't want to carry when the RLE algorithm is applied.
static void cleanupRpb(thread_db* tdbb, record_param* rpb)
{
	Record* const record = rpb->rpb_record;
	const Format* const format = record->getFormat();

	SET_TDBB(tdbb); // Is it necessary?

	/*
    Starting from the format, walk through its
    array of descriptors.  If the descriptor has
    no address, its a computed field and we shouldn't
    try to fix it.  Get a pointer to the actual data
    and see if that field is null by indexing into
    the null flags between the record header and the
    record data.
	*/

	for (USHORT n = 0; n < format->fmt_count; n++)
	{
		const dsc* desc = &format->fmt_desc[n];

		if (!desc->dsc_address)
			continue;

		UCHAR* const p = record->getData() + (IPTR) desc->dsc_address;

		if (record->isNull(n))
		{
			USHORT length = desc->dsc_length;

			if (length)
				memset(p, 0, length);
		}
		else if (desc->dsc_dtype == dtype_varying)
		{
			vary* varying = reinterpret_cast<vary*>(p);
			USHORT length = desc->dsc_length - sizeof(USHORT);

			if (length > varying->vary_length)
			{
				char* trail = varying->vary_string + varying->vary_length;
				length -= varying->vary_length;
				memset(trail, 0, length);
			}
		}
	}
}

// Build a validation list for a relation, if appropriate.
static void makeValidation(thread_db* tdbb, CompilerScratch* csb, StreamType stream,
	Array<ValidateInfo>& validations)
{
	SET_TDBB(tdbb);

	DEV_BLKCHK(csb, type_csb);

	jrd_rel* relation = csb->csb_rpt[stream].csb_relation;

	vec<jrd_fld*>* vector = relation->rel_fields;
	if (!vector)
		return;

	StreamMap localMap;
	StreamType* map = csb->csb_rpt[stream].csb_map;

	if (!map)
	{
		map = localMap.getBuffer(STREAM_MAP_LENGTH);
		fb_assert(stream <= MAX_STREAMS);
		map[0] = stream;
	}

	USHORT fieldId = 0;
	vec<jrd_fld*>::iterator ptr1 = vector->begin();

	for (const vec<jrd_fld*>::const_iterator end = vector->end(); ptr1 < end; ++ptr1, ++fieldId)
	{
		BoolExprNode* validation;

		if (*ptr1 && (validation = (*ptr1)->fld_validation))
		{
			AutoSetRestore<USHORT> autoRemapVariable(&csb->csb_remap_variable,
				(csb->csb_variables ? csb->csb_variables->count() : 0) + 1);

			RemapFieldNodeCopier copier(csb, map, fieldId);

			validation = copier.copy(tdbb, validation);

			ValidateInfo validate;
			validate.boolean = validation;
			validate.value = PAR_gen_field(tdbb, stream, fieldId);
			validations.add(validate);
		}

		if (*ptr1 && (validation = (*ptr1)->fld_not_null))
		{
			AutoSetRestore<USHORT> autoRemapVariable(&csb->csb_remap_variable,
				(csb->csb_variables ? csb->csb_variables->count() : 0) + 1);

			RemapFieldNodeCopier copier(csb, map, fieldId);

			validation = copier.copy(tdbb, validation);

			ValidateInfo validate;
			validate.boolean = validation;
			validate.value = PAR_gen_field(tdbb, stream, fieldId);
			validations.add(validate);
		}
	}
}

// Process a view update performed by a trigger.
static StmtNode* pass1ExpandView(thread_db* tdbb, CompilerScratch* csb, StreamType orgStream,
	StreamType newStream, bool remap)
{
	SET_TDBB(tdbb);

	DEV_BLKCHK(csb, type_csb);

	StmtNodeStack stack;
	jrd_rel* relation = csb->csb_rpt[orgStream].csb_relation;
	vec<jrd_fld*>* fields = relation->rel_fields;

	dsc desc;
	USHORT id = 0, newId = 0;
	vec<jrd_fld*>::iterator ptr = fields->begin();

	for (const vec<jrd_fld*>::const_iterator end = fields->end(); ptr < end; ++ptr, ++id)
	{
		if (*ptr)
		{
			if (remap)
			{
				const jrd_fld* field = MET_get_field(relation, id);

				if (field->fld_source)
					newId = nodeAs<FieldNode>(field->fld_source)->fieldId;
				else
					newId = id;
			}
			else
				newId = id;

			const Format* const format = CMP_format(tdbb, csb, newStream);
			if (newId >= format->fmt_count || !format->fmt_desc[newId].dsc_address)
				continue;

			AssignmentNode* const assign =
				FB_NEW_POOL(*tdbb->getDefaultPool()) AssignmentNode(*tdbb->getDefaultPool());
			assign->asgnTo = PAR_gen_field(tdbb, newStream, newId);
			assign->asgnFrom = PAR_gen_field(tdbb, orgStream, id);

			stack.push(assign);
		}
	}

	return PAR_make_list(tdbb, stack);
}

// Check out a prospective update to a relation. If it fails security check, bounce it.
// If it's a view update, make sure the view is updatable, and return the view source for redirection.
// If it's a simple relation, return NULL.
static RelationSourceNode* pass1Update(thread_db* tdbb, CompilerScratch* csb, jrd_rel* relation,
	const TrigVector* trigger, StreamType stream, StreamType updateStream, SecurityClass::flags_t priv,
	jrd_rel* view, StreamType viewStream, StreamType viewUpdateStream)
{
	SET_TDBB(tdbb);

	DEV_BLKCHK(csb, type_csb);
	DEV_BLKCHK(relation, type_rel);
	DEV_BLKCHK(view, type_rel);

	// unless this is an internal request, check access permission

	CMP_post_access(tdbb, csb, relation->rel_security_name, (view ? view->rel_id : 0),
		priv, SCL_object_table, relation->rel_name);

	// ensure that the view is set for the input streams,
	// so that access to views can be checked at the field level

	fb_assert(viewStream <= MAX_STREAMS);
	CMP_csb_element(csb, stream)->csb_view = view;
	CMP_csb_element(csb, stream)->csb_view_stream = viewStream;

	if (stream != updateStream)
	{
		fb_assert(viewUpdateStream <= MAX_STREAMS);
		CMP_csb_element(csb, updateStream)->csb_view = view;
		CMP_csb_element(csb, updateStream)->csb_view_stream = viewUpdateStream;
	}

	// if we're not a view, everything's cool

	RseNode* rse = relation->rel_view_rse;
	if (!rse)
		return NULL;

	// a view with triggers is always updatable

	if (trigger)
	{
		bool userTriggers = false;

		for (FB_SIZE_T i = 0; i < trigger->getCount(); i++)
		{
			if (!(*trigger)[i].sys_trigger)
			{
				userTriggers = true;
				break;
			}
		}

		if (userTriggers)
		{
			csb->csb_rpt[updateStream].csb_flags |= csb_view_update;
			return NULL;
		}
	}

	// we've got a view without triggers, let's check whether it's updateable

	if (rse->rse_relations.getCount() != 1 || rse->rse_projection || rse->rse_sorted ||
		rse->rse_relations[0]->type != RelationSourceNode::TYPE)
	{
		ERR_post(Arg::Gds(isc_read_only_view) << Arg::Str(relation->rel_name));
	}

	// for an updateable view, return the view source

	csb->csb_rpt[updateStream].csb_flags |= csb_view_update;

	return static_cast<RelationSourceNode*>(rse->rse_relations[0].getObject());
}

// The csb->csb_validate_expr becomes true if an ancestor of the current node (the one being
// passed in) in the parse tree is a validation. "Ancestor" does not include the current node
// being passed in as an argument.
// If we are in a "validate subtree" (as determined by the csb->csb_validate_expr), we must not
// post update access to the fields involved in the validation clause.
// (See the call for CMP_post_access in this function.)
static void pass1Validations(thread_db* tdbb, CompilerScratch* csb, Array<ValidateInfo>& validations)
{
	AutoSetRestore<bool> autoValidateExpr(&csb->csb_validate_expr, true);

	for (Array<ValidateInfo>::iterator i = validations.begin(); i != validations.end(); ++i)
	{
		DmlNode::doPass1(tdbb, csb, i->boolean.getAddress());
		DmlNode::doPass1(tdbb, csb, i->value.getAddress());
	}
}

// Inherit access to triggers to be fired.
//
// When we detect that a trigger could be fired by a request,
// then we add the access list for that trigger to the access
// list for this request.  That way, when we check access for
// the request we also check access for any other objects that
// could be fired off by the request.
//
// Note that when we add the access item, we specify that
//    Trigger X needs access to resource Y.
// In the access list we parse here, if there is no "accessor"
// name then the trigger must access it directly.  If there is
// an "accessor" name, then something accessed by this trigger
// must require the access.
//
// CVC: The code no longer matches this comment.
// CVC: The third parameter is the owner of the triggers vector
// and was added to avoid triggers posting access checks to
// their base tables, since it's nonsense and causes weird
// messages about false REFERENCES right failures.
static void postTriggerAccess(CompilerScratch* csb, jrd_rel* ownerRelation,
	ExternalAccess::exa_act operation, jrd_rel* view)
{
	DEV_BLKCHK(csb, type_csb);
	DEV_BLKCHK(view, type_rel);

	// allow all access to internal requests
	if (csb->csb_g_flags & (csb_internal | csb_ignore_perm))
		return;

	// Post trigger access
	ExternalAccess temp(operation, ownerRelation->rel_id, view ? view->rel_id : 0);
	FB_SIZE_T i;

	if (!csb->csb_external.find(temp, i))
		csb->csb_external.insert(i, temp);
}

// Perform operation's pre-triggers, storing active rpb in chain.
static void preModifyEraseTriggers(thread_db* tdbb, TrigVector** trigs,
	StmtNode::WhichTrigger whichTrig, record_param* rpb, record_param* rec, TriggerAction op)
{
	if (!tdbb->getTransaction()->tra_rpblist)
	{
		tdbb->getTransaction()->tra_rpblist =
			FB_NEW_POOL(*tdbb->getTransaction()->tra_pool) traRpbList(*tdbb->getTransaction()->tra_pool);
	}

	const int rpblevel = tdbb->getTransaction()->tra_rpblist->PushRpb(rpb);

	if (*trigs && whichTrig != StmtNode::POST_TRIG)
	{
		try
		{
			EXE_execute_triggers(tdbb, trigs, rpb, rec, op, StmtNode::PRE_TRIG);
		}
		catch (const Exception&)
		{
			tdbb->getTransaction()->tra_rpblist->PopRpb(rpb, rpblevel);
			throw;
		}
	}

	tdbb->getTransaction()->tra_rpblist->PopRpb(rpb, rpblevel);
}

// 1. Remove assignments of DEFAULT to computed fields.
// 2. Remove assignments to identity column when OVERRIDING USER VALUE is specified in INSERT.
static void preprocessAssignments(thread_db* tdbb, CompilerScratch* csb,
	StreamType stream, CompoundStmtNode* compoundNode, const Nullable<OverrideClause>* insertOverride)
{
	if (!compoundNode)
		return;

	jrd_rel* relation = csb->csb_rpt[stream].csb_relation;

	fb_assert(relation);
	if (!relation)
		return;

	Nullable<IdentityType> identityType;

	for (size_t i = compoundNode->statements.getCount(); i--; )
	{
		const AssignmentNode* assign = nodeAs<AssignmentNode>(compoundNode->statements[i]);
		fb_assert(assign);
		if (!assign)
			continue;

		const ExprNode* assignFrom = assign->asgnFrom;
		const FieldNode* assignToField = nodeAs<FieldNode>(assign->asgnTo);

		if (assignToField)
		{
			int fieldId = assignToField->fieldId;
			jrd_fld* fld;

			while (true)
			{
				if (assignToField->fieldStream == stream &&
					relation->rel_fields &&
					(fld = (*relation->rel_fields)[fieldId]))
				{
					if (insertOverride && fld->fld_identity_type.specified)
					{
						if (insertOverride->specified || !nodeIs<DefaultNode>(assignFrom))
							identityType = fld->fld_identity_type;

						if (*insertOverride == OverrideClause::USER_VALUE)
						{
							compoundNode->statements.remove(i);
							break;
						}
					}

					if (fld->fld_computation)
					{
						if (nodeIs<DefaultNode>(assignFrom))
							compoundNode->statements.remove(i);
					}
					else if (relation->rel_view_rse && fld->fld_source_rel_field.first.hasData())
					{
						relation = MET_lookup_relation(tdbb, fld->fld_source_rel_field.first);

						fb_assert(relation);
						if (!relation)
							return;

						if ((fieldId = MET_lookup_field(tdbb, relation, fld->fld_source_rel_field.second)) >= 0)
							continue;
					}
				}

				break;
			}
		}
	}

	if (!insertOverride)
		return;

	if (insertOverride->specified)
	{
		if (!identityType.specified)
			ERR_post(Arg::Gds(isc_overriding_without_identity) << relation->rel_name);

		if (identityType == IDENT_TYPE_BY_DEFAULT && *insertOverride == OverrideClause::SYSTEM_VALUE)
			ERR_post(Arg::Gds(isc_overriding_system_invalid) << relation->rel_name);

		if (identityType == IDENT_TYPE_ALWAYS && *insertOverride == OverrideClause::USER_VALUE)
			ERR_post(Arg::Gds(isc_overriding_user_invalid) << relation->rel_name);
	}
	else
	{
		if (identityType == IDENT_TYPE_ALWAYS)
			ERR_post(Arg::Gds(isc_overriding_system_missing) << relation->rel_name);
	}
}

// Execute a list of validation expressions.
static void validateExpressions(thread_db* tdbb, const Array<ValidateInfo>& validations)
{
	SET_TDBB(tdbb);

	Array<ValidateInfo>::const_iterator end = validations.end();

	for (Array<ValidateInfo>::const_iterator i = validations.begin(); i != end; ++i)
	{
		jrd_req* request = tdbb->getRequest();

		if (!i->boolean->execute(tdbb, request) && !(request->req_flags & req_null))
		{
			// Validation error -- report result
			const char* value;
			VaryStr<128> temp;

			const dsc* desc = EVL_expr(tdbb, request, i->value);
			const USHORT length = (desc && !(request->req_flags & req_null)) ?
				MOV_make_string(tdbb, desc, ttype_dynamic, &value, &temp, sizeof(temp) - 1) : 0;

			if (!desc || (request->req_flags & req_null))
				value = NULL_STRING_MARK;
			else if (!length)
				value = "";
			else
				const_cast<char*>(value)[length] = 0;	// safe cast - data is actually on the stack

			string name;
			const FieldNode* fieldNode = nodeAs<FieldNode>(i->value);

			if (fieldNode)
			{
				const jrd_rel* relation = request->req_rpb[fieldNode->fieldStream].rpb_relation;
				const vec<jrd_fld*>* vector = relation->rel_fields;
				const jrd_fld* field;

				if (vector && fieldNode->fieldId < vector->count() &&
					(field = (*vector)[fieldNode->fieldId]))
				{
					if (!relation->rel_name.isEmpty())
						name.printf("\"%s\".\"%s\"", relation->rel_name.c_str(), field->fld_name.c_str());
					else
						name.printf("\"%s\"", field->fld_name.c_str());
				}
			}

			if (name.isEmpty())
				name = UNKNOWN_STRING_MARK;

			ERR_post(Arg::Gds(isc_not_valid) << Arg::Str(name) << Arg::Str(value));
		}
	}
}


}	// namespace Jrd
