// ****************************************************************************
//  expred.cpp                                                    ELIOT project
// ****************************************************************************
//
//   File Description:
//
//    Information required by the compiler for expression reduction
//
//
//
//
//
//
//
//
// ****************************************************************************
// This document is released under the GNU General Public License, with the
// following clarification and exception.
//
// Linking this library statically or dynamically with other modules is making
// a combined work based on this library. Thus, the terms and conditions of the
// GNU General Public License cover the whole combination.
//
// As a special exception, the copyright holders of this library give you
// permission to link this library with independent modules to produce an
// executable, regardless of the license terms of these independent modules,
// and to copy and distribute the resulting executable under terms of your
// choice, provided that you also meet, for each linked independent module,
// the terms and conditions of the license of that module. An independent
// module is a module which is not derived from or based on this library.
// If you modify this library, you may extend this exception to your version
// of the library, but you are not obliged to do so. If you do not wish to
// do so, delete this exception statement from your version.
//
// See http://www.gnu.org/copyleft/gpl.html and Matthew 25:22 for details
//  (C) 1992-2010 Christophe de Dinechin <christophe@taodyne.com>
//  (C) 2010 Taodyne SAS
// ****************************************************************************

#include "expred.h"
#include "unit.h"
#include "args.h"
#include "types.h"
#include "save.h"
#include "errors.h"
#include "renderer.h"
#include "llvm-crap.h"

ELIOT_BEGIN

using namespace llvm;



// ============================================================================
//
//    Compile an expression
//
// ============================================================================

llvm_value CompileExpression::DoInteger(Integer *what)
// ----------------------------------------------------------------------------
//   Compile an integer constant
// ----------------------------------------------------------------------------
{
    Compiler *compiler = unit->compiler;
    return ConstantInt::get(compiler->integerTy, what->value);
}


llvm_value CompileExpression::DoReal(Real *what)
// ----------------------------------------------------------------------------
//   Compile a real constant
// ----------------------------------------------------------------------------
{
    Compiler *compiler = unit->compiler;
    return ConstantFP::get(compiler->realTy, what->value);
}


llvm_value CompileExpression::DoText(Text *what)
// ----------------------------------------------------------------------------
//   Compile a text constant
// ----------------------------------------------------------------------------
{
    Compiler *compiler = unit->compiler;
    GlobalVariable *global = compiler->TextConstant(what->value);
    if (what->IsCharacter())
        return ConstantInt::get(compiler->characterTy,
                                what->value.length() ? what->value[0] : 0);
    return unit->code->CreateConstGEP2_32(global, 0U, 0U);
}


llvm_value CompileExpression::DoName(Name *what)
// ----------------------------------------------------------------------------
//   Compile a name
// ----------------------------------------------------------------------------
{
    Prefix_p   where;
    Infix_p    rewrite;
    Context   *context  = unit->context;
    Tree      *existing = context->Bound(what, true, &rewrite, &where);

    assert(existing || !"Type checking didn't realize a name is missing");
    Tree *from = RewriteDefined(rewrite->left);
    if (where == context->CurrentScope())
        if (llvm_value result = unit->Known(from))
            return result;

    // Check true and false values
    if (existing == eliot_true)
        return ConstantInt::get(unit->compiler->booleanTy, 1);
    if (existing == eliot_false)
        return ConstantInt::get(unit->compiler->booleanTy, 0);

    // Check if it is a global
    if (llvm_value global = unit->Global(existing))
        return global;
    if (llvm_value global = unit->Global(from))
        return global;

    // If we are in a context building a closure, record dependency
    if (unit->closureTy)
        return unit->NeedClosure(from);

    return DoCall(what);
}


llvm_value CompileExpression::DoInfix(Infix *infix)
// ----------------------------------------------------------------------------
//   Compile infix expressions
// ----------------------------------------------------------------------------
{
    // Sequences
    if (infix->name == "\n" || infix->name == ";")
    {
        llvm_value left = ForceEvaluation(infix->left);
        llvm_value right = ForceEvaluation(infix->right);
        if (right)
            return right;
        if (left)
            return left;
        return NULL;
    }

    // Type casts - REVISIT: may need to do some actual conversion
    if (infix->name == ":" || infix->name == "as")
    {
        return infix->left->Do(this);
    }

    // Declarations: it's too early to define a function just yet,
    // because we don't have the actual argument types.
    if (infix->name == "->")
        return NULL;

    // General case: expression
    return DoCall(infix);
}


llvm_value CompileExpression::DoPrefix(Prefix *what)
// ----------------------------------------------------------------------------
//   Compile prefix expressions
// ----------------------------------------------------------------------------
{
    if (Name *name = what->left->AsName())
        if (name->value == "data" || name->value == "extern")
            return NULL;

    return DoCall(what);
}


llvm_value CompileExpression::DoPostfix(Postfix *what)
// ----------------------------------------------------------------------------
//   Compile postfix expressions
// ----------------------------------------------------------------------------
{
    return DoCall(what);
}


llvm_value CompileExpression::DoBlock(Block *block)
// ----------------------------------------------------------------------------
//   Compile blocks
// ----------------------------------------------------------------------------
{
    return block->child->Do(this);
}


llvm_value CompileExpression::DoCall(Tree *call)
// ----------------------------------------------------------------------------
//   Compile expressions into calls for the right expression
// ----------------------------------------------------------------------------
{
    llvm_value result = NULL;

    rcall_map &rcalls = unit->types->rcalls;
    rcall_map::iterator found = rcalls.find(call);
    assert(found != rcalls.end() || !"Type analysis botched on expression");

    Function *function = unit->function;
    LLVMContext &llvm = unit->llvm;
    RewriteCalls *rc = (*found).second;
    RewriteCandidates &calls = rc->candidates;

    // Optimize the frequent case where we have a single call candidate
    uint i, max = calls.size();
    if (max == 1)
    {
        // We now evaluate in that rewrite's type system
        RewriteCandidate &cand = calls[0];
        Save<Types_p> saveTypes(unit->types, cand.types);

        if (cand.Unconditional())
        {
            result = DoRewrite(cand);
            return result;
        }
    }
    else if (max == 0)
    {
        // If it passed type check and there is no candidate, return tree as is
        result = unit->ConstantTree(call);
        return result;
    }

    // More general case: we need to generate expression reduction
    llvm_block isDone = BasicBlock::Create(llvm, "done", function);
    llvm_builder code = unit->code;
    llvm_value storage = unit->NeedStorage(call);
    llvm_type storageType = unit->ExpressionMachineType(call);

    for (i = 0; i < max; i++)
    {
        Save<value_map> saveComputed(computed, computed);
        
        // Now evaluate in that candidate's type system
        RewriteCandidate &cand = calls[i];
        Save<Types_p> saveTypes(unit->types, cand.types);
        bool conditional = false;
        llvm_block isBad = BasicBlock::Create(llvm, "skip", function);

        // Perform the tests on type kind to see if candidate is valid
        RewriteKinds &kinds = cand.kinds;
        RewriteKinds::iterator k;
        for (k = kinds.begin(); k != kinds.end(); k++)
        {
            llvm_value compare = KindTest((*k).value, (*k).test);
            llvm_block isGood = BasicBlock::Create(llvm, "kindT", function);
            code->CreateCondBr(compare, isGood, isBad);
            code->SetInsertPoint(isGood);
            llvm_value treeValue = Value((*k).value);
            computed[(*k).value] = unit->Autobox(treeValue, (*k).machineType);
            IFTRACE(calltypes)
                llvm::errs() << "Kind test: " << *treeValue
                             << " as " << *computed[(*k).value] << "\n";
            conditional = true;
        }

        
        // Perform the tests to check if this candidate is valid
        RewriteConditions &conds = cand.conditions;
        RewriteConditions::iterator t;
        for (t = conds.begin(); t != conds.end(); t++)
        {
            llvm_value compare = Compare((*t).value, (*t).test);
            llvm_block isGood = BasicBlock::Create(llvm, "condT", function);
            code->CreateCondBr(compare, isGood, isBad);
            code->SetInsertPoint(isGood);
            conditional = true;
        }

        if (conditional)
        {
            result = DoRewrite(cand);
            result = unit->Autobox(result, storageType);
            code->CreateStore(result, storage);
            code->CreateBr(isDone);
            code->SetInsertPoint(isBad);
        }
        else
        {
            // If this particular call was unconditional, we are done
            result = DoRewrite(cand);
            result = unit->Autobox(result, storageType);
            code->CreateStore(result, storage);
            code->CreateBr(isDone);
            code->SetInsertPoint(isBad);
            code->CreateBr(isDone);
            code->SetInsertPoint(isDone);
            result = code->CreateLoad(storage);
            return result;
        }
    }

    // The final call to eliot_form_error if nothing worked
    unit->CallFormError(call);
    code->CreateBr(isDone);
    code->SetInsertPoint(isDone);
    result = code->CreateLoad(storage);
    return result;
}


llvm_value CompileExpression::DoRewrite(RewriteCandidate &cand)
// ----------------------------------------------------------------------------
//   Generate code for a particular rewwrite candidate
// ----------------------------------------------------------------------------
{
    Infix *rw = cand.rewrite;
    llvm_value result = NULL;

    IFTRACE(calltypes)
        std::cerr << "Rewrite: " << rw << "\n";

    // Evaluate parameters
    llvm_values args;
    RewriteBindings &bnds = cand.bindings;
    RewriteBindings::iterator b;
    for (b = bnds.begin(); b != bnds.end(); b++)
    {
        Tree *tree = (*b).value;
        IFTRACE(calltypes)
            std::cerr << "  Arg: " << tree << ": ";
        if (llvm_value closure = (*b).Closure(unit))
        {
            args.push_back(closure);
            IFTRACE(calltypes)
                llvm::errs() << "  closure " << *closure << "\n";
        }
        else if (llvm_value value = Value(tree))
        {
            args.push_back(value);
            llvm_type mtype = value->getType();
            if (unit->compiler->IsClosureType(mtype))
                (*b).closure = value;
            IFTRACE(calltypes)
                llvm::errs() << "  value " << *value
                             << " mtype " << *mtype << "\n";
        }
    }

    // Check if this is an LLVM builtin
    Tree *builtin = NULL;
    if (Tree *value = rw->right)
        if (Prefix *prefix = value->AsPrefix())
            if (Name *name = prefix->left->AsName())
                if (name->value == "opcode")
                    builtin = prefix->right;

    if (builtin)
    {
        llvm_builder bld = unit->code;
        if (Prefix *prefix = builtin->AsPrefix())
        {
            if (Name *name = prefix->left->AsName())
            {
                if (name->value == "data")
                {
                    bld = unit->data;
                    builtin = prefix->right;
                }
            }
        }

        Name *name = builtin->AsName();
        if (!name)
        {
            Ooops("Malformed primitive $1", builtin);
            result = unit->CallFormError(builtin);
        }
        else
        {
            Compiler *compiler = unit->compiler;
            text op = name->value;
            uint sz = args.size();
            llvm_value *a = &args[0];
            result = compiler->Primitive(*unit, bld, op, sz, a);
            if (!result)
                Ooops("Invalid primitive $1", builtin);
            IFTRACE(calltypes)
                llvm::errs() << "  = Primitive: " << *result << "\n";
        }
    }
    else
    {
        llvm_value function = unit->Compile(cand, args);
        IFTRACE(calltypes)
            llvm::errs() << "  < Function: " << *function << "\n";
        if (function)
            result = unit->code->CreateCall(function, LLVMS_ARGS(args));
        IFTRACE(calltypes)
            llvm::errs() << "  =Call: " << *result << "\n";
    }

    return result;
}


llvm_value CompileExpression::Value(Tree *expr)
// ----------------------------------------------------------------------------
//   Evaluate an expression once
// ----------------------------------------------------------------------------
{
    llvm_value value = computed[expr];
    if (!value)
    {
        value = expr->Do(this);
        computed[expr] = value;
    }
    return value;
}


llvm_value CompileExpression::Compare(Tree *valueTree, Tree *testTree)
// ----------------------------------------------------------------------------
//   Perform a comparison between the two values and check if this matches
// ----------------------------------------------------------------------------
{
    CompiledUnit &u = *unit;
    Compiler &c = *u.compiler;

    if (Name *vt = valueTree->AsName())
        if (Name *tt = testTree->AsName())
            if (vt->value == tt->value)
                return ConstantInt::get(c.booleanTy, 1);

    llvm_value value = Value(valueTree);
    llvm_value test = Value(testTree);
    llvm_type valueType = value->getType();
    llvm_type testType = test->getType();

    llvm_builder code = u.code;

    // Comparison of boolean values
    if (testType == c.booleanTy)
    {
        if (valueType == c.treePtrTy || valueType == c.nameTreePtrTy)
        {
            value = u.Autobox(value, c.booleanTy);
            valueType = value->getType();
        }
        if (valueType != c.booleanTy)
            return ConstantInt::get(c.booleanTy, 0);
        return code->CreateICmpEQ(test, value);
    }

    // Comparison of character values
    if (testType == c.characterTy)
    {
        if (valueType == c.textTreePtrTy)
        {
            value = u.Autobox(value, testType);
            valueType = value->getType();
        }
        if (valueType != c.characterTy)
            return ConstantInt::get(c.booleanTy, 0);
        return code->CreateICmpEQ(test, value);
    }

    // Comparison of text constants
    if (testType == c.textTy)
    {
        test = u.Autobox(test, c.charPtrTy);
        testType = test->getType();
    }
    if (testType == c.charPtrTy)
    {
        if (valueType == c.textTreePtrTy)
        {
            value = u.Autobox(value, testType);
            valueType = value->getType();
        }
        if (valueType != c.charPtrTy)
            return ConstantInt::get(c.booleanTy, 0);
        value = code->CreateCall2(c.strcmp_fn, test, value);
        test = ConstantInt::get(value->getType(), 0);
        value = code->CreateICmpEQ(value, test);
        return value;
    }

    // Comparison of integer values
    if (testType->isIntegerTy())
    {
        if (valueType == c.integerTreePtrTy)
        {
            value = u.Autobox(value, c.integerTy);
            valueType = value->getType();
        }
        if (!valueType->isIntegerTy())
            return ConstantInt::get(c.booleanTy, 0);
        if (valueType != c.integerTy)
            value = code->CreateSExt(value, c.integerTy);
        if (testType != c.integerTy)
            test = code->CreateSExt(test, c.integerTy);
        return code->CreateICmpEQ(test, value);
    }

    // Comparison of floating-point values
    if (testType->isFloatingPointTy())
    {
        if (valueType == c.realTreePtrTy)
        {
            value = u.Autobox(value, c.realTy);
            valueType = value->getType();
        }
        if (!valueType->isFloatingPointTy())
            return ConstantInt::get(c.booleanTy, 0);
        if (valueType != testType)
        {
            if (valueType != c.realTy)
            {
                value = code->CreateFPExt(value, c.realTy);
                valueType = value->getType();
            }
            if (testType != c.realTy)
            {
                test = code->CreateFPExt(test, c.realTy);
                testType = test->getType();
            }
            if (valueType != testType)
                return ConstantInt::get(c.booleanTy, 0);
        }
        return code->CreateFCmpOEQ(test, value);
    }

    // Test our special types
    if (testType == c.treePtrTy         ||
        testType == c.integerTreePtrTy  ||
        testType == c.realTreePtrTy     ||
        testType == c.textTreePtrTy     ||
        testType == c.nameTreePtrTy     ||
        testType == c.blockTreePtrTy    ||
        testType == c.infixTreePtrTy    ||
        testType == c.prefixTreePtrTy   ||
        testType == c.postfixTreePtrTy)
    {
        if (testType != c.treePtrTy)
        {
            test = code->CreateBitCast(test, c.treePtrTy);
            testType = test->getType();
        }

        // Convert value to a Tree * if possible
        if (valueType->isIntegerTy() ||
            valueType->isFloatingPointTy() ||
            valueType == c.charPtrTy ||
            valueType == c.textTy ||
            valueType == c.integerTreePtrTy  ||
            valueType == c.realTreePtrTy     ||
            valueType == c.textTreePtrTy     ||
            valueType == c.nameTreePtrTy     ||
            valueType == c.blockTreePtrTy    ||
            valueType == c.infixTreePtrTy    ||
            valueType == c.prefixTreePtrTy   ||
            valueType == c.postfixTreePtrTy)
        {
            value = u.Autobox(value, c.treePtrTy);
            valueType = value->getType();
        }

        if (testType != valueType)
            return ConstantInt::get(c.booleanTy, 0);

        // Call runtime function to perform tree comparison
        return code->CreateCall2(c.eliot_same_shape, value, test);
    }

    // Other comparisons fail for now
    return ConstantInt::get(c.booleanTy, 0);
}


llvm_value CompileExpression::KindTest(Tree *valueTree, kind test)
// ----------------------------------------------------------------------------
//   Perform a comparison between the two values and check if this matches
// ----------------------------------------------------------------------------
{
    CompiledUnit &u = *unit;
    Compiler &c = *u.compiler;
    llvm_builder code = u.code;

    llvm_value value = Value(valueTree);

    llvm_value treeValue = u.Autobox(value, c.treePtrTy);
    llvm_value ptr = code->CreateConstGEP2_32(treeValue, 0,
                                              TAG_INDEX, "tagPtr");
    llvm_value tag = code->CreateLoad(ptr, "tag");
    llvm_value mask = llvm::ConstantInt::get(tag->getType(), Tree::KINDMASK);
    llvm_value kind = code->CreateAnd(tag, mask, "tagAndMask");

    llvm_value ref = llvm::ConstantInt::get(tag->getType(), test);
    llvm_value result = code->CreateICmpEQ(kind, ref);
    return result;
}


llvm_value CompileExpression::ForceEvaluation(Tree *expr)
// ----------------------------------------------------------------------------
//   For top-level expressions, make sure we evaluate closures
// ----------------------------------------------------------------------------
{
    llvm_value result = expr->Do(this);
    if (result)
    {
        llvm_type resTy = result->getType();
        if (unit->compiler->IsClosureType(resTy))
            result = unit->InvokeClosure(result);
    }
    return result;
}


llvm_value CompileExpression::TopLevelEvaluation(Tree *expr)
// ----------------------------------------------------------------------------
//   Evaluate normally, but force evaluation for names
// ----------------------------------------------------------------------------
{
    if (expr->Kind() == NAME)
        return ForceEvaluation(expr);
    return expr->Do(this);
}

ELIOT_END
