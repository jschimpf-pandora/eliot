// ****************************************************************************
//  opcodes.cpp                     (C) 1992-2009 Christophe de Dinechin (ddd)
//                                                                 XL2 project
// ****************************************************************************
//
//   File Description:
//
//    Opcodes are native trees generated as part of compilation/optimization
//    to speed up execution. They represent a step in the evaluation of
//    the code.
//
//
//
//
//
//
// ****************************************************************************
// This document is released under the GNU General Public License.
// See http://www.gnu.org/copyleft/gpl.html and Matthew 25:22 for details
// ****************************************************************************
// * File       : $RCSFile$
// * Revision   : $Revision$
// * Date       : $Date$
// ****************************************************************************

#include "opcodes.h"
#include "basics.h"
#include <typeinfo>

XL_BEGIN

// ============================================================================
// 
//    Class Native
// 
// ============================================================================

Tree *Action::DoNative(Native *what)
// ----------------------------------------------------------------------------
//   Default is simply to invoke 'Do'
// ----------------------------------------------------------------------------
{
    return Do(what);
}


Tree *Native::Do(Action *action)
// ----------------------------------------------------------------------------
//   For native nodes, default actions will do
// ----------------------------------------------------------------------------
{
    return action->DoNative(this);
}


Tree *Native::Run(Stack *stack)
// ----------------------------------------------------------------------------
//    Running a native node returns the native itself
// ----------------------------------------------------------------------------
{
    return NULL;
}


text Native::TypeName()
// ----------------------------------------------------------------------------
//   The name of a native tree is its type
// ----------------------------------------------------------------------------
{
    return typeid(*this).name();
}


Tree *Native::Append(Tree *tail)
// ----------------------------------------------------------------------------
//   Append another tree to a native tree
// ----------------------------------------------------------------------------
{
    // Find end of opcode chain. If end is not native, optimize away
    Native *prev = this;
    while (Native *next = dynamic_cast<Native *> (prev->next))
        prev = next;

    // String right opcode at end
    prev->next = tail;

    return this;
}


Tree *Variable::Run(Stack *stack)
// ----------------------------------------------------------------------------
//   Return the variable at given index in the stack
// ----------------------------------------------------------------------------
{
    Tree *value = stack->Get(id);
    return value;
}


Tree *NonLocalVariable::Run(Stack *stack)
// ----------------------------------------------------------------------------
//   Return the variable at given index in the stack
// ----------------------------------------------------------------------------
{
    return stack->Get(id, frame);
}


Tree *EvaluateArgument::Run(Stack *stack)
// ----------------------------------------------------------------------------
//   Evaluate argument in given slot if it has not been evaluated yet
// ----------------------------------------------------------------------------
{
    Tree *value = stack->GetLocal(id);
    if (!value)
    {
        value = stack->Run(code);
        stack->SetLocal(id, value);
    }
    return value;
}


Tree *Invoke::Run(Stack *stack)
// ----------------------------------------------------------------------------
//    Run the child in the local arguments stack
// ----------------------------------------------------------------------------
{
    // Evaluate arguments that have not been evaluated yet
    ulong i, max = values.size();
    tree_list args;
    for (i = 0; i < max; i++)
    {
        Tree *value = values[max + ~i];
        value = stack->Run(value);
        args.push_back(value);
    }

    // Allocate stack frame and copy args there
    ulong argsSize = args.size();
    ulong oldFrame = stack->Grow(args);
    args.clear();
        
    // Invoke the called tree
    Tree *result = stack->Run(invoked);

    // Free stack frame and return result
    stack->Shrink(oldFrame, argsSize);
    return result;
}


void Invoke::AddArgument(Tree *value)
// ----------------------------------------------------------------------------
//   Add an argument to the value list (for immediate invokation)
// ----------------------------------------------------------------------------
{
    values.push_back(value);
}


struct TreeMatch : Action
// ----------------------------------------------------------------------------
//   Check if two trees match in structure
// ----------------------------------------------------------------------------
{
    TreeMatch (Tree *t): test(t) {}
    Tree *DoInteger(Integer *what)
    {
        if (Integer *it = dynamic_cast<Integer *> (test))
            if (it->value == what->value)
                return what;
        return NULL;
    }
    Tree *DoReal(Real *what)
    {
        if (Real *rt = dynamic_cast<Real *> (test))
            if (rt->value == what->value)
                return what;
        return NULL;
    }
    Tree *DoText(Text *what)
    {
        if (Text *tt = dynamic_cast<Text *> (test))
            if (tt->value == what->value)
                return what;
        return NULL;
    }
    Tree *DoName(Name *what)
    {
        if (Name *nt = dynamic_cast<Name *> (test))
            if (nt->value == what->value)
                return what;
        return NULL;
    }

    Tree *DoBlock(Block *what)
    {
        // Test if we exactly match the block, i.e. the reference is a block
        if (Block *bt = dynamic_cast<Block *> (test))
        {
            if (bt->Opening() == what->Opening() &&
                bt->Closing() == what->Closing())
            {
                test = bt->child;
                Tree *br = what->child->Do(this);
                test = bt;
                if (br)
                    return br;
            }
        }
        return NULL;
    }
    Tree *DoInfix(Infix *what)
    {
        if (Infix *it = dynamic_cast<Infix *> (test))
        {
            // Check if we match the tree, e.g. A+B vs 2+3
            if (it->name == what->name)
            {
                test = it->left;
                Tree *lr = what->left->Do(this);
                test = it;
                if (!lr)
                    return NULL;
                test = it->right;
                Tree *rr = what->right->Do(this);
                test = it;
                if (!rr)
                    return NULL;
                return what;
            }
        }
        return NULL;
    }
    Tree *DoPrefix(Prefix *what)
    {
        if (Prefix *pt = dynamic_cast<Prefix *> (test))
        {
            // Check if we match the tree, e.g. f(A) vs. f(2)
            test = pt->left;
            Tree *lr = what->left->Do(this);
            test = pt;
            if (!lr)
                return NULL;
            test = pt->right;
            Tree *rr = what->right->Do(this);
            test = pt;
            if (!rr)
                return NULL;
            return what;
        }
        return NULL;
    }
    Tree *DoPostfix(Postfix *what)
    {
        if (Postfix *pt = dynamic_cast<Postfix *> (test))
        {
            // Check if we match the tree, e.g. A! vs 2!
            test = pt->right;
            Tree *rr = what->right->Do(this);
            test = pt;
            if (!rr)
                return NULL;
            test = pt->left;
            Tree *lr = what->left->Do(this);
            test = pt;
            if (!lr)
                return NULL;
            return what;
        }
        return NULL;
    }
    Tree *Do(Tree *what)
    {
        return NULL;
    }

    Tree *      test;
};


Tree *EqualityTest::Run (Stack *stack)
// ----------------------------------------------------------------------------
//   Check equality of two trees
// ----------------------------------------------------------------------------
{
    Tree *code = stack->Run(test);
    Tree *ref = stack->Run(value);
    condition = false;
    TreeMatch compareForEquality(ref);
    if (code->Do(compareForEquality))
        condition = true;
    return NULL;
}


Tree *TypeTest::Run (Stack *stack)
// ----------------------------------------------------------------------------
//   Check if the code being tested has the given type value
// ----------------------------------------------------------------------------
{
    Tree *code = stack->Run(test);
    Tree *ref = stack->Run(type_value);
    condition = false;
    if (TypeExpression *typeChecker = dynamic_cast<TypeExpression *>(ref))
        if (typeChecker->TypeCheck(stack, code))
            condition = true;
    return NULL;
}



// ============================================================================
// 
//   Helper functions
// 
// ============================================================================

longlong integer_arg(Stack *stack, ulong index)
// ----------------------------------------------------------------------------
//    Return an integer value 
// ----------------------------------------------------------------------------
{
    Tree *value = stack->Get(index);
    if (Integer *ival = dynamic_cast<Integer *> (value))
        return ival->value;
    stack->Error("Value '$1' is not an integer", value);
    return 0;
}


double real_arg(Stack *stack, ulong index)
// ----------------------------------------------------------------------------
//    Return a real value 
// ----------------------------------------------------------------------------
{
    Tree *value = stack->Get(index);
    if (Real *rval = dynamic_cast<Real *> (value))
        return rval->value;
    stack->Error("Value '$1' is not a real", value);
    return 0.0;
}


text text_arg(Stack *stack, ulong index)
// ----------------------------------------------------------------------------
//    Return a text value 
// ----------------------------------------------------------------------------
{
    Tree *value = stack->Get(index);
    if (Text *tval = dynamic_cast<Text *> (value))
        return tval->value;
    stack->Error("Value '$1' is not a text", value);
    return "";
}


bool boolean_arg(Stack *stack, ulong index)
// ----------------------------------------------------------------------------
//    Return a boolean truth value 
// ----------------------------------------------------------------------------
{
    Tree *value = stack->Get(index);
    if (value == true_name)
        return true;
    else if (value == false_name)
        return false;
    stack->Error("Value '$1' is not a boolean value", value);
    return false;
}


Tree *anything_arg(Stack *stack, ulong index)
// ----------------------------------------------------------------------------
//    Return a boolean truth value 
// ----------------------------------------------------------------------------
{
    Tree *value = stack->Get(index);
    return value;
}


Tree *AddParameter(Tree *existing, Tree *append)
// ----------------------------------------------------------------------------
//   Create a comma-separated parameter list
// ----------------------------------------------------------------------------
{
    Tree *arglist = existing;
    if (!arglist)
        return append;

    Infix *parent = NULL;
    while (Infix *infix = dynamic_cast<Infix *> (existing))
    {
        parent = infix;
        existing = parent->right;
    }
    if (!parent)
        return new Infix(",", existing, append);

    parent->right = new Infix(",", parent->right, append);
    return arglist;
}


XL_END
