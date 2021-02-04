/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Common code generator for translating Yul / inline assembly to EVM and EVM1.5.
 */

#include <libyul/backends/evm/EVMCodeTransform.h>

#include <libyul/optimiser/NameCollector.h>
#include <libyul/AsmAnalysisInfo.h>
#include <libyul/AST.h>
#include <libyul/Utilities.h>

#include <liblangutil/Exceptions.h>

#include <boost/range/adaptor/reversed.hpp>

#include <range/v3/algorithm/any_of.hpp>
#include <range/v3/to_container.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/transform.hpp>

#include <utility>
#include <variant>

using namespace std;
using namespace solidity;
using namespace solidity::yul;
using namespace solidity::util;

void VariableReferenceCounter::operator()(Identifier const& _identifier)
{
	increaseRefIfFound(_identifier.name);
}

void VariableReferenceCounter::operator()(FunctionDefinition const& _function)
{
	Scope* originalScope = m_scope;

	yulAssert(m_info.virtualBlocks.at(&_function), "");
	m_scope = m_info.scopes.at(m_info.virtualBlocks.at(&_function).get()).get();
	yulAssert(m_scope, "Variable scope does not exist.");

	for (auto const& v: _function.returnVariables)
		increaseRefIfFound(v.name);

	VariableReferenceCounter{m_context, m_info}(_function.body);

	m_scope = originalScope;
}

void VariableReferenceCounter::operator()(ForLoop const& _forLoop)
{
	Scope* originalScope = m_scope;
	// Special scoping rules.
	m_scope = m_info.scopes.at(&_forLoop.pre).get();

	walkVector(_forLoop.pre.statements);
	visit(*_forLoop.condition);
	(*this)(_forLoop.body);
	(*this)(_forLoop.post);

	m_scope = originalScope;
}

void VariableReferenceCounter::operator()(Block const& _block)
{
	Scope* originalScope = m_scope;
	m_scope = m_info.scopes.at(&_block).get();

	ASTWalker::operator()(_block);

	m_scope = originalScope;
}

void VariableReferenceCounter::increaseRefIfFound(YulString _variableName)
{
	m_scope->lookup(_variableName, GenericVisitor{
		[&](Scope::Variable const& _var)
		{
			++m_context.variableReferences[&_var];
		},
		[](Scope::Function const&) { }
	});
}

CodeTransform::CodeTransform(
	AbstractAssembly& _assembly,
	AsmAnalysisInfo& _analysisInfo,
	Block const& _block,
	bool _allowStackOpt,
	EVMDialect const& _dialect,
	BuiltinContext& _builtinContext,
	bool _evm15,
	ExternalIdentifierAccess _identifierAccess,
	bool _useNamedLabelsForFunctions,
	shared_ptr<Context> _context,
	vector<YulString> _delayedReturnVariables
):
	m_assembly(_assembly),
	m_info(_analysisInfo),
	m_dialect(_dialect),
	m_builtinContext(_builtinContext),
	m_allowStackOpt(_allowStackOpt),
	m_evm15(_evm15),
	m_useNamedLabelsForFunctions(_useNamedLabelsForFunctions),
	m_identifierAccess(move(_identifierAccess)),
	m_context(move(_context)),
	m_delayedReturnVariables(move(_delayedReturnVariables))
{
	if (!m_context)
	{
		// initialize
		m_context = make_shared<Context>();
		if (m_allowStackOpt)
			VariableReferenceCounter{*m_context, m_info}(_block);
	}
}

void CodeTransform::decreaseReference(YulString, Scope::Variable const& _var)
{
	if (!m_allowStackOpt)
		return;

	unsigned& ref = m_context->variableReferences.at(&_var);
	yulAssert(ref >= 1, "");
	--ref;
	if (ref == 0)
		m_variablesScheduledForDeletion.insert(&_var);
}

bool CodeTransform::unreferenced(Scope::Variable const& _var) const
{
	return !m_context->variableReferences.count(&_var) || m_context->variableReferences[&_var] == 0;
}

void CodeTransform::freeUnusedVariables(bool _popUnusedSlotsAtStackTop)
{
	if (!m_allowStackOpt)
		return;

	for (auto const& identifier: m_scope->identifiers)
		if (holds_alternative<Scope::Variable>(identifier.second))
		{
			Scope::Variable const& var = std::get<Scope::Variable>(identifier.second);
			if (m_variablesScheduledForDeletion.count(&var))
				deleteVariable(var);
		}

	if (_popUnusedSlotsAtStackTop)
		while (m_unusedStackSlots.count(m_assembly.stackHeight() - 1))
		{
			yulAssert(m_unusedStackSlots.erase(m_assembly.stackHeight() - 1), "");
			m_assembly.appendInstruction(evmasm::Instruction::POP);
		}
}

void CodeTransform::deleteVariable(Scope::Variable const& _var)
{
	yulAssert(m_allowStackOpt, "");
	yulAssert(m_context->variableStackHeights.count(&_var) > 0, "");
	m_unusedStackSlots.insert(static_cast<int>(m_context->variableStackHeights[&_var]));
	m_context->variableStackHeights.erase(&_var);
	m_context->variableReferences.erase(&_var);
	m_variablesScheduledForDeletion.erase(&_var);
}

void CodeTransform::operator()(VariableDeclaration const& _varDecl)
{
	yulAssert(m_scope, "");

	size_t const numVariables = _varDecl.variables.size();
	auto heightAtStart = static_cast<size_t>(m_assembly.stackHeight());
	if (_varDecl.value)
	{
		std::visit(*this, *_varDecl.value);
		expectDeposit(static_cast<int>(numVariables), static_cast<int>(heightAtStart));
		freeUnusedVariables(false);
	}
	else
	{
		m_assembly.setSourceLocation(_varDecl.location);
		size_t variablesLeft = numVariables;
		while (variablesLeft--)
			m_assembly.appendConstant(u256(0));
	}

	m_assembly.setSourceLocation(_varDecl.location);
	bool atTopOfStack = true;
	for (size_t varIndex = 0; varIndex < numVariables; ++varIndex)
	{
		size_t varIndexReverse = numVariables - 1 - varIndex;
		YulString varName = _varDecl.variables[varIndexReverse].name;
		auto& var = std::get<Scope::Variable>(m_scope->identifiers.at(varName));
		m_context->variableStackHeights[&var] = heightAtStart + varIndexReverse;
		if (!m_allowStackOpt)
			continue;

		if (unreferenced(var))
		{
			if (atTopOfStack)
			{
				m_context->variableStackHeights.erase(&var);
				m_assembly.appendInstruction(evmasm::Instruction::POP);
			}
			else
				m_variablesScheduledForDeletion.insert(&var);
		}
		else if (m_unusedStackSlots.empty())
			atTopOfStack = false;
		else
		{
			auto slot = static_cast<size_t>(*m_unusedStackSlots.begin());
			m_unusedStackSlots.erase(m_unusedStackSlots.begin());
			m_context->variableStackHeights[&var] = slot;
			if (size_t heightDiff = variableHeightDiff(var, varName, true))
				m_assembly.appendInstruction(evmasm::swapInstruction(static_cast<unsigned>(heightDiff - 1)));
			m_assembly.appendInstruction(evmasm::Instruction::POP);
		}
	}
}

void CodeTransform::stackError(StackTooDeepError _error, int _targetStackHeight)
{
	m_assembly.appendInstruction(evmasm::Instruction::INVALID);
	// Correct the stack.
	while (m_assembly.stackHeight() > _targetStackHeight)
		m_assembly.appendInstruction(evmasm::Instruction::POP);
	while (m_assembly.stackHeight() < _targetStackHeight)
		m_assembly.appendConstant(u256(0));
	// Store error.
	m_stackErrors.emplace_back(std::move(_error));
	m_assembly.markAsInvalid();
}

void CodeTransform::operator()(Assignment const& _assignment)
{
	int height = m_assembly.stackHeight();
	std::visit(*this, *_assignment.value);
	expectDeposit(static_cast<int>(_assignment.variableNames.size()), height);

	m_assembly.setSourceLocation(_assignment.location);
	generateMultiAssignment(_assignment.variableNames);
}

void CodeTransform::operator()(ExpressionStatement const& _statement)
{
	m_assembly.setSourceLocation(_statement.location);
	std::visit(*this, _statement.expression);
}

void CodeTransform::operator()(FunctionCall const& _call)
{
	yulAssert(m_scope, "");

	if (BuiltinFunctionForEVM const* builtin = m_dialect.builtin(_call.functionName.name))
		builtin->generateCode(_call, m_assembly, m_builtinContext, [&](Expression const& _expression) {
			visitExpression(_expression);
		});
	else
	{
		m_assembly.setSourceLocation(_call.location);
		EVMAssembly::LabelID returnLabel(numeric_limits<EVMAssembly::LabelID>::max()); // only used for evm 1.0
		if (!m_evm15)
		{
			returnLabel = m_assembly.newLabelId();
			m_assembly.appendLabelReference(returnLabel);
		}

		Scope::Function* function = nullptr;
		yulAssert(m_scope->lookup(_call.functionName.name, GenericVisitor{
			[](Scope::Variable&) { yulAssert(false, "Expected function name."); },
			[&](Scope::Function& _function) { function = &_function; }
		}), "Function name not found.");
		yulAssert(function, "");
		yulAssert(function->arguments.size() == _call.arguments.size(), "");
		for (auto const& arg: _call.arguments | boost::adaptors::reversed)
			visitExpression(arg);
		m_assembly.setSourceLocation(_call.location);
		if (m_evm15)
			m_assembly.appendJumpsub(
				functionEntryID(_call.functionName.name, *function),
				static_cast<int>(function->arguments.size()),
				static_cast<int>(function->returns.size())
			);
		else
		{
			m_assembly.appendJumpTo(
				functionEntryID(_call.functionName.name, *function),
				static_cast<int>(function->returns.size() - function->arguments.size()) - 1,
				AbstractAssembly::JumpType::IntoFunction
			);
			m_assembly.appendLabel(returnLabel);
		}
	}
}

void CodeTransform::operator()(Identifier const& _identifier)
{
	m_assembly.setSourceLocation(_identifier.location);
	// First search internals, then externals.
	yulAssert(m_scope, "");
	if (m_scope->lookup(_identifier.name, GenericVisitor{
		[&](Scope::Variable& _var)
		{
			// TODO: opportunity for optimization: Do not DUP if this is the last reference
			// to the top most element of the stack
			if (size_t heightDiff = variableHeightDiff(_var, _identifier.name, false))
				m_assembly.appendInstruction(evmasm::dupInstruction(static_cast<unsigned>(heightDiff)));
			else
				// Store something to balance the stack
				m_assembly.appendConstant(u256(0));
			decreaseReference(_identifier.name, _var);
		},
		[](Scope::Function&)
		{
			yulAssert(false, "Function not removed during desugaring.");
		}
	}))
	{
		return;
	}
	yulAssert(
		m_identifierAccess.generateCode,
		"Identifier not found and no external access available."
	);
	m_identifierAccess.generateCode(_identifier, IdentifierContext::RValue, m_assembly);
}

void CodeTransform::operator()(Literal const& _literal)
{
	m_assembly.setSourceLocation(_literal.location);
	m_assembly.appendConstant(valueOfLiteral(_literal));
}

void CodeTransform::operator()(If const& _if)
{
	visitExpression(*_if.condition);
	m_assembly.setSourceLocation(_if.location);
	m_assembly.appendInstruction(evmasm::Instruction::ISZERO);
	AbstractAssembly::LabelID end = m_assembly.newLabelId();
	m_assembly.appendJumpToIf(end);
	(*this)(_if.body);
	m_assembly.setSourceLocation(_if.location);
	m_assembly.appendLabel(end);
}

void CodeTransform::operator()(Switch const& _switch)
{
	//@TODO use JUMPV in EVM1.5?

	visitExpression(*_switch.expression);
	int expressionHeight = m_assembly.stackHeight();
	map<Case const*, AbstractAssembly::LabelID> caseBodies;
	AbstractAssembly::LabelID end = m_assembly.newLabelId();
	for (Case const& c: _switch.cases)
	{
		if (c.value)
		{
			(*this)(*c.value);
			m_assembly.setSourceLocation(c.location);
			AbstractAssembly::LabelID bodyLabel = m_assembly.newLabelId();
			caseBodies[&c] = bodyLabel;
			yulAssert(m_assembly.stackHeight() == expressionHeight + 1, "");
			m_assembly.appendInstruction(evmasm::dupInstruction(2));
			m_assembly.appendInstruction(evmasm::Instruction::EQ);
			m_assembly.appendJumpToIf(bodyLabel);
		}
		else
			// default case
			(*this)(c.body);
	}
	m_assembly.setSourceLocation(_switch.location);
	m_assembly.appendJumpTo(end);

	size_t numCases = caseBodies.size();
	for (auto const& c: caseBodies)
	{
		m_assembly.setSourceLocation(c.first->location);
		m_assembly.appendLabel(c.second);
		(*this)(c.first->body);
		// Avoid useless "jump to next" for the last case.
		if (--numCases > 0)
		{
			m_assembly.setSourceLocation(c.first->location);
			m_assembly.appendJumpTo(end);
		}
	}

	m_assembly.setSourceLocation(_switch.location);
	m_assembly.appendLabel(end);
	m_assembly.appendInstruction(evmasm::Instruction::POP);
}

void CodeTransform::operator()(FunctionDefinition const& _function)
{
	yulAssert(m_scope, "");
	yulAssert(m_scope->identifiers.count(_function.name), "");
	Scope::Function& function = std::get<Scope::Function>(m_scope->identifiers.at(_function.name));

	size_t height = m_evm15 ? 0 : 1;
	yulAssert(m_info.scopes.at(&_function.body), "");
	Scope* varScope = m_info.scopes.at(m_info.virtualBlocks.at(&_function).get()).get();
	yulAssert(varScope, "");
	for (auto const& v: _function.parameters | boost::adaptors::reversed)
	{
		auto& var = std::get<Scope::Variable>(varScope->identifiers.at(v.name));
		m_context->variableStackHeights[&var] = height++;
	}

	m_assembly.setSourceLocation(_function.location);
	int const stackHeightBefore = m_assembly.stackHeight();

	if (m_evm15)
		m_assembly.appendBeginsub(functionEntryID(_function.name, function), static_cast<int>(_function.parameters.size()));
	else
		m_assembly.appendLabel(functionEntryID(_function.name, function));

	m_assembly.setStackHeight(static_cast<int>(height));

	vector<YulString> deferredReturnVariables;

	if (m_allowStackOpt)
		deferredReturnVariables = _function.returnVariables |
			ranges::views::transform([](auto const& _var) { return _var.name; }) |
			ranges::to<vector<YulString>>;
	else
		for (auto const& v: _function.returnVariables)
		{
				auto& var = std::get<Scope::Variable>(varScope->identifiers.at(v.name));
				m_context->variableStackHeights[&var] = height++;
				// Preset stack slots for return variables to zero.
				m_assembly.appendConstant(u256(0));
		}

	m_context->functionExitPoints.push(
		CodeTransformContext::JumpInfo{
			m_assembly.newLabelId(),
			m_assembly.stackHeight() + static_cast<int>(deferredReturnVariables.size())
		}
	);
	CodeTransform subTransform(
		m_assembly,
		m_info,
		_function.body,
		m_allowStackOpt,
		m_dialect,
		m_builtinContext,
		m_evm15,
		m_identifierAccess,
		m_useNamedLabelsForFunctions,
		m_context,
		move(deferredReturnVariables)
	);
	subTransform(_function.body);
	if (!subTransform.m_stackErrors.empty())
	{
		m_assembly.markAsInvalid();
		for (StackTooDeepError& stackError: subTransform.m_stackErrors)
		{
			if (stackError.functionName.empty())
				stackError.functionName = _function.name;
			m_stackErrors.emplace_back(std::move(stackError));
		}
	}

	if (!subTransform.m_delayedReturnVariables.empty())
	{
		// Can only happen for functions with straight control flow (in particular no jump to the exit label)
		// that never reads from or writes to return variables.

		// We should have allocated slots either for all or for none of the return variables.
		yulAssert(subTransform.m_delayedReturnVariables.size() == _function.returnVariables.size(), "");

		// Already pop all arguments to make the stack shuffling below easier.
		while (m_assembly.stackHeight() > m_evm15 ? 0 : 1)
			m_assembly.appendInstruction(evmasm::Instruction::POP);

		for (auto const& _returnVariable: _function.returnVariables)
		{
			auto* var = std::get_if<Scope::Variable>(&varScope->identifiers.at(_returnVariable.name));
			yulAssert(var, "Return variable not in scope.");
			m_context->variableStackHeights[var] = static_cast<size_t>(m_assembly.stackHeight());
			// Set unassigned return variables to zero.
			m_assembly.appendConstant(u256(0));
		}
	}

	m_assembly.appendLabel(m_context->functionExitPoints.top().label);
	m_context->functionExitPoints.pop();

	{
		// The stack layout here is:
		// <return label>? <arguments...> <return values...>
		// But we would like it to be:
		// <return values...> <return label>?
		// So we have to append some SWAP and POP instructions.
		// Note that <arguments...> is not the full list of function arguments, since some arguments may already have
		// been popped before the first slot for return values was assigned.
		// Also the return values are not necessarily contiguous. However, their slots always strictly increasing order.

		// This vector holds the desired target positions of all stack slots and is
		// modified parallel to the actual stack.
		vector<int> stackLayout(static_cast<size_t>(m_assembly.stackHeight()), -1);
		if (!m_evm15)
			stackLayout[0] = static_cast<int>(_function.returnVariables.size()); // Move return label to the top
		for (auto&& [_n, _returnVariable]: ranges::views::enumerate(_function.returnVariables))
			stackLayout.at(m_context->variableStackHeights.at(
				&std::get<Scope::Variable>(varScope->identifiers.at(_returnVariable.name))
			)) = static_cast<int>(_n);

		if (stackLayout.size() > 17)
		{
			StackTooDeepError error(
				_function.name,
				YulString{},
				static_cast<int>(stackLayout.size()) - 17,
				"The function " +
				_function.name.str() +
				" has " +
				to_string(stackLayout.size() - 17) +
				" parameters or return variables too many to fit the stack size."
			);
			stackError(std::move(error), m_assembly.stackHeight() - static_cast<int>(_function.parameters.size()));
		}
		else
		{
			while (!stackLayout.empty() && stackLayout.back() != static_cast<int>(stackLayout.size() - 1))
				if (stackLayout.back() < 0)
				{
					m_assembly.appendInstruction(evmasm::Instruction::POP);
					stackLayout.pop_back();
				}
				else
				{
					m_assembly.appendInstruction(evmasm::swapInstruction(static_cast<unsigned>(stackLayout.size()) - static_cast<unsigned>(stackLayout.back()) - 1u));
					swap(stackLayout[static_cast<size_t>(stackLayout.back())], stackLayout.back());
				}
			for (size_t i = 0; i < stackLayout.size(); ++i)
				yulAssert(i == static_cast<size_t>(stackLayout[i]), "Error reshuffling stack.");
		}
	}
	if (m_evm15)
		m_assembly.appendReturnsub(static_cast<int>(_function.returnVariables.size()), stackHeightBefore);
	else
		m_assembly.appendJump(
			stackHeightBefore - static_cast<int>(_function.returnVariables.size()),
			AbstractAssembly::JumpType::OutOfFunction
		);
	m_assembly.setStackHeight(stackHeightBefore);
}

void CodeTransform::operator()(ForLoop const& _forLoop)
{
	Scope* originalScope = m_scope;
	// We start with visiting the block, but not finalizing it.
	m_scope = m_info.scopes.at(&_forLoop.pre).get();
	int stackStartHeight = m_assembly.stackHeight();

	visitStatements(_forLoop.pre.statements);

	AbstractAssembly::LabelID loopStart = m_assembly.newLabelId();
	AbstractAssembly::LabelID postPart = m_assembly.newLabelId();
	AbstractAssembly::LabelID loopEnd = m_assembly.newLabelId();

	m_assembly.setSourceLocation(_forLoop.location);
	m_assembly.appendLabel(loopStart);

	visitExpression(*_forLoop.condition);
	m_assembly.setSourceLocation(_forLoop.location);
	m_assembly.appendInstruction(evmasm::Instruction::ISZERO);
	m_assembly.appendJumpToIf(loopEnd);

	int const stackHeightBody = m_assembly.stackHeight();
	m_context->forLoopStack.emplace(Context::ForLoopLabels{ {postPart, stackHeightBody}, {loopEnd, stackHeightBody} });
	(*this)(_forLoop.body);

	m_assembly.setSourceLocation(_forLoop.location);
	m_assembly.appendLabel(postPart);

	(*this)(_forLoop.post);

	m_assembly.setSourceLocation(_forLoop.location);
	m_assembly.appendJumpTo(loopStart);
	m_assembly.appendLabel(loopEnd);

	finalizeBlock(_forLoop.pre, stackStartHeight);
	m_context->forLoopStack.pop();
	m_scope = originalScope;
}

int CodeTransform::appendPopUntil(int _targetDepth)
{
	int const stackDiffAfter = m_assembly.stackHeight() - _targetDepth;
	for (int i = 0; i < stackDiffAfter; ++i)
		m_assembly.appendInstruction(evmasm::Instruction::POP);
	return stackDiffAfter;
}

void CodeTransform::operator()(Break const& _break)
{
	yulAssert(!m_context->forLoopStack.empty(), "Invalid break-statement. Requires surrounding for-loop in code generation.");
	m_assembly.setSourceLocation(_break.location);

	Context::JumpInfo const& jump = m_context->forLoopStack.top().done;
	m_assembly.appendJumpTo(jump.label, appendPopUntil(jump.targetStackHeight));
}

void CodeTransform::operator()(Continue const& _continue)
{
	yulAssert(!m_context->forLoopStack.empty(), "Invalid continue-statement. Requires surrounding for-loop in code generation.");
	m_assembly.setSourceLocation(_continue.location);

	Context::JumpInfo const& jump = m_context->forLoopStack.top().post;
	m_assembly.appendJumpTo(jump.label, appendPopUntil(jump.targetStackHeight));
}

void CodeTransform::operator()(Leave const& _leaveStatement)
{
	yulAssert(!m_context->functionExitPoints.empty(), "Invalid leave-statement. Requires surrounding function in code generation.");
	m_assembly.setSourceLocation(_leaveStatement.location);

	Context::JumpInfo const& jump = m_context->functionExitPoints.top();
	m_assembly.appendJumpTo(jump.label, appendPopUntil(jump.targetStackHeight));
}

void CodeTransform::operator()(Block const& _block)
{
	Scope* originalScope = m_scope;
	m_scope = m_info.scopes.at(&_block).get();

	int blockStartStackHeight = m_assembly.stackHeight() + static_cast<int>(m_delayedReturnVariables.size());
	visitStatements(_block.statements);
	if (!m_delayedReturnVariables.empty())
		blockStartStackHeight -= static_cast<int>(m_delayedReturnVariables.size());

	finalizeBlock(_block, blockStartStackHeight);
	m_scope = originalScope;
}

AbstractAssembly::LabelID CodeTransform::functionEntryID(YulString _name, Scope::Function const& _function)
{
	if (!m_context->functionEntryIDs.count(&_function))
	{
		AbstractAssembly::LabelID id =
			m_useNamedLabelsForFunctions ?
			m_assembly.namedLabel(_name.str()) :
			m_assembly.newLabelId();
		m_context->functionEntryIDs[&_function] = id;
	}
	return m_context->functionEntryIDs[&_function];
}

void CodeTransform::visitExpression(Expression const& _expression)
{
	int height = m_assembly.stackHeight();
	std::visit(*this, _expression);
	expectDeposit(1, height);
}

void CodeTransform::visitStatements(vector<Statement> const& _statements)
{
	std::optional<AbstractAssembly::LabelID> jumpTarget = std::nullopt;

	for (auto const& statement: _statements)
	{
		freeUnusedVariables();

		if (!m_delayedReturnVariables.empty() && (
			holds_alternative<VariableDeclaration>(statement) ||
			holds_alternative<Leave>(statement) ||
			holds_alternative<ForLoop>(statement) ||
			holds_alternative<Block>(statement) ||
			holds_alternative<Switch>(statement) ||
			holds_alternative<If>(statement) ||
			[&](){
				ReferencesCounter referencesCounter{ReferencesCounter::CountWhat::OnlyVariables};
				referencesCounter.visit(statement);
				return ranges::any_of(
					m_delayedReturnVariables,
					[&](YulString _returnVariableName) { return referencesCounter.references().count(_returnVariableName); }
				);
			}()
		))
		{
			for (YulString _returnVariableName: m_delayedReturnVariables)
			{
				auto* var = std::get_if<Scope::Variable>(m_scope->lookup(_returnVariableName));
				yulAssert(var, "Return variable not in scope.");
				m_context->variableStackHeights[var] = static_cast<size_t>(m_assembly.stackHeight());
				// Preset stack slots for return variables to zero.
				m_assembly.appendConstant(u256(0));
				if (!m_unusedStackSlots.empty())
				{
					auto slot = static_cast<size_t>(*m_unusedStackSlots.begin());
					m_unusedStackSlots.erase(m_unusedStackSlots.begin());
					m_context->variableStackHeights[var] = slot;
					if (size_t heightDiff = variableHeightDiff(*var, _returnVariableName, true))
						m_assembly.appendInstruction(evmasm::swapInstruction(static_cast<unsigned>(heightDiff - 1)));
					m_assembly.appendInstruction(evmasm::Instruction::POP);
				}
				++m_context->variableReferences[var];
			}
			m_delayedReturnVariables.clear();
		}

		auto const* functionDefinition = std::get_if<FunctionDefinition>(&statement);
		if (functionDefinition && !jumpTarget)
		{
			m_assembly.setSourceLocation(locationOf(statement));
			jumpTarget = m_assembly.newLabelId();
			m_assembly.appendJumpTo(*jumpTarget, 0);
		}
		else if (!functionDefinition && jumpTarget)
		{
			m_assembly.appendLabel(*jumpTarget);
			jumpTarget = std::nullopt;
		}

		std::visit(*this, statement);
	}
	// we may have a leftover jumpTarget
	if (jumpTarget)
		m_assembly.appendLabel(*jumpTarget);

	freeUnusedVariables();
}

void CodeTransform::finalizeBlock(Block const& _block, int blockStartStackHeight)
{
	m_assembly.setSourceLocation(_block.location);

	freeUnusedVariables();

	// pop variables
	yulAssert(m_info.scopes.at(&_block).get() == m_scope, "");
	for (auto const& id: m_scope->identifiers)
		if (holds_alternative<Scope::Variable>(id.second))
		{
			Scope::Variable const& var = std::get<Scope::Variable>(id.second);
			if (m_allowStackOpt)
			{
				yulAssert(!m_context->variableStackHeights.count(&var), "");
				yulAssert(!m_context->variableReferences.count(&var), "");
			}
			else
				m_assembly.appendInstruction(evmasm::Instruction::POP);
		}

	int deposit = m_assembly.stackHeight() - blockStartStackHeight;
	yulAssert(deposit == 0, "Invalid stack height at end of block: " + to_string(deposit));
}

void CodeTransform::generateMultiAssignment(vector<Identifier> const& _variableNames)
{
	yulAssert(m_scope, "");
	for (auto const& variableName: _variableNames | boost::adaptors::reversed)
		generateAssignment(variableName);
}

void CodeTransform::generateAssignment(Identifier const& _variableName)
{
	yulAssert(m_scope, "");
	if (auto var = m_scope->lookup(_variableName.name))
	{
		Scope::Variable const& _var = std::get<Scope::Variable>(*var);
		if (size_t heightDiff = variableHeightDiff(_var, _variableName.name, true))
			m_assembly.appendInstruction(evmasm::swapInstruction(static_cast<unsigned>(heightDiff - 1)));
		m_assembly.appendInstruction(evmasm::Instruction::POP);
		decreaseReference(_variableName.name, _var);
	}
	else
	{
		yulAssert(
			m_identifierAccess.generateCode,
			"Identifier not found and no external access available."
		);
		m_identifierAccess.generateCode(_variableName, IdentifierContext::LValue, m_assembly);
	}
}

size_t CodeTransform::variableHeightDiff(Scope::Variable const& _var, YulString _varName, bool _forSwap)
{
	yulAssert(m_context->variableStackHeights.count(&_var), "");
	size_t heightDiff = static_cast<size_t>(m_assembly.stackHeight()) - m_context->variableStackHeights[&_var];
	yulAssert(heightDiff > (_forSwap ? 1 : 0), "Negative stack difference for variable.");
	size_t limit = _forSwap ? 17 : 16;
	if (heightDiff > limit)
	{
		m_stackErrors.emplace_back(
			_varName,
			heightDiff - limit,
			"Variable " +
			_varName.str() +
			" is " +
			to_string(heightDiff - limit) +
			" slot(s) too deep inside the stack."
		);
		m_assembly.markAsInvalid();
		return _forSwap ? 2 : 1;
	}
	return heightDiff;
}

void CodeTransform::expectDeposit(int _deposit, int _oldHeight) const
{
	yulAssert(m_assembly.stackHeight() == _oldHeight + _deposit, "Invalid stack deposit.");
}
