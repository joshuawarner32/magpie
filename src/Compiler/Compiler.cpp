#include "Ast.h"
#include "Compiler.h"
#include "ErrorReporter.h"
#include "Method.h"
#include "Module.h"
#include "Object.h"
#include "Resolver.h"
#include "VM.h"

namespace magpie
{
  Module* Compiler::compileModule(VM& vm, gc<ModuleAst> moduleAst,
                                  ErrorReporter& reporter)
  {
    Module* module = new Module();
    
    // TODO(bob): Doing this here is hackish. Need to figure out when a module's
    // imports are resolved.
    module->imports().add(vm.coreModule());
    
    // Declare the definitions.
    for (int i = 0; i < moduleAst->defs().count(); i++)
    {
      // TODO(bob): Handle non-method defs when they exist.
      MethodDef* method = moduleAst->defs()[i]->asMethodDef();
      gc<String> signature = SignatureBuilder::build(*method);
      vm.methods().declare(signature);
    }
    
    // TODO(bob): This will need some work when modules and imports are
    // supported. We will need to forward declare all of the modules, handle
    // imported/exported names, and *then* go back and define all of them.
    // Now define them to allow mutual recursion.
    for (int i = 0; i < moduleAst->defs().count(); i++)
    {
      // TODO(bob): Handle non-method defs when they exist.
      MethodDef* method = moduleAst->defs()[i]->asMethodDef();
      gc<String> signature = SignatureBuilder::build(*method);
      gc<Method> compiled = compileMethod(vm, module, *method, reporter);
      vm.methods().define(signature, compiled);
    }
    
    // Wrap the body in a shell method and compile it.
    gc<Expr> body = moduleAst->body();
    gc<Pattern> nothing = new ValuePattern(body->pos(),
        new NothingExpr(body->pos()));
    MethodDef* method = new MethodDef(body->pos(),
        nothing, String::create("<module>"), nothing, body);
    
    module->bindBody(compileMethod(vm, module, *method, reporter));
    
    return module;
  }

  gc<Method> Compiler::compileMethod(VM& vm, Module* module,
                                     MethodDef& method,
                                     ErrorReporter& reporter)
  {
    Compiler compiler(vm, reporter, module);
    return compiler.compile(method);
  }
  
  Compiler::Compiler(VM& vm, ErrorReporter& reporter, Module* module)
  : ExprVisitor(),
    vm_(vm),
    reporter_(reporter),
    method_(new Method(module)),
    code_(),
    numLocals_(0),
    numTemps_(0),
    maxSlots_(0)
  {}

  gc<Method> Compiler::compile(MethodDef& method)
  {
    Resolver::resolve(reporter_, *method_->module(), method);
    
    numLocals_ = method.maxLocals();
    maxSlots_ = numLocals_;
    
    // Create a fake local for the argument and result value.
    int result = 0;

    // Evaluate the method's parameter patterns.
    compilePattern(method.leftParam(), result);
    compilePattern(method.rightParam(), result);

    method.body()->accept(*this, result);
    write(OP_RETURN, result);

    method_->setCode(code_, maxSlots_);
    
    return method_;
  }
  
  void Compiler::visit(AndExpr& expr, int dest)
  {
    expr.left()->accept(*this, dest);
    
    // Leave a space for the test and jump instruction.
    int jumpToEnd = startJump();
    
    expr.right()->accept(*this, dest);
    
    endJump(jumpToEnd, OP_JUMP_IF_FALSE, dest);
  }
  
  void Compiler::visit(BinaryOpExpr& expr, int dest)
  {
    int a = compileExpressionOrConstant(*expr.left());
    int b = compileExpressionOrConstant(*expr.right());

    OpCode op;
    bool negate = false;
    switch (expr.type())
    {
      case TOKEN_PLUS:   op = OP_ADD; break;
      case TOKEN_MINUS:  op = OP_SUBTRACT; break;
      case TOKEN_STAR:   op = OP_MULTIPLY; break;
      case TOKEN_SLASH:  op = OP_DIVIDE; break;
      case TOKEN_EQEQ:   op = OP_EQUAL; break;
      case TOKEN_NEQ:    op = OP_EQUAL; negate = true; break;
      case TOKEN_LT:     op = OP_LESS_THAN; break;
      case TOKEN_LTE:    op = OP_GREATER_THAN; negate = true; break;
      case TOKEN_GT:     op = OP_GREATER_THAN; break;
      case TOKEN_GTE:    op = OP_LESS_THAN; negate = true; break;

      default:
        ASSERT(false, "Unknown infix operator.");
    }
    
    write(op, a, b, dest);
    
    if (negate) write(OP_NOT, dest);
    
    if (IS_REGISTER(a)) releaseTemp();
    if (IS_REGISTER(b)) releaseTemp();
  }

  void Compiler::visit(BoolExpr& expr, int dest)
  {
    write(OP_BUILT_IN, expr.value() ? BUILT_IN_TRUE : BUILT_IN_FALSE, dest);
  }

  void Compiler::visit(CallExpr& expr, int dest)
  {
    gc<String> signature = SignatureBuilder::build(expr);

    int method = vm_.methods().find(signature);
    
    if (method == -1)
    {
      reporter_.error(expr.pos(),
                      "Could not find a method with signature '%s'.",
                      signature->cString());
    
      // Just pick a method so we can keep compiling to report later errors.
      method = 0;
    }
    
    ASSERT(expr.leftArg().isNull() || expr.rightArg().isNull(),
           "Calls with left and right args aren't implemented yet.");

    // Compile the argument(s).
    // TODO(bob): This is going to need work. Basically, it needs to destructure
    // the left and right arguments to figure out how many actual arguments
    // there are, allocate the right amount of temporaries, compile the args
    // to those, and then call. (For cases where there is just a total of one
    // argument, we can just use the one existing dest register, though.
    // Likewise, the method prelude code needs to handle multiple arguments.
    // For now, since we don't have records, we only support postfix or prefix
    // calls, but not infix. That ensures we only ever need one register.

    if (!expr.leftArg().isNull())
    {
      expr.leftArg()->accept(*this, dest);
    }
    
    if (!expr.rightArg().isNull())
    {
      expr.rightArg()->accept(*this, dest);
    }
    
    write(OP_CALL, method, dest);
  }
  
  void Compiler::visit(CatchExpr& expr, int dest)
  {
    // Register the catch handler.
    int enter = startJump();
    
    // Compile the block body.
    expr.body()->accept(*this, dest);
    
    // Complete the catch handler.
    write(OP_EXIT_TRY);
    
    // Jump past it if an exception is not thrown.
    int jumpPastCatch = startJump();
    endJump(enter, OP_ENTER_TRY);
    
    // Compile the catch handlers.
    // TODO(bob): Handle multiple catches, compile their patterns, pattern
    // match, etc. For now, just compile the body.
    ASSERT(expr.catches().count() == 1, "Multiple catch clauses not impl.");
    expr.catches()[0].body()->accept(*this, dest);
    
    endJump(jumpPastCatch, OP_JUMP);
  }
    
  void Compiler::visit(DoExpr& expr, int dest)
  {
    expr.body()->accept(*this, dest);
  }

  void Compiler::visit(IfExpr& expr, int dest)
  {
    // Compile the condition.
    expr.condition()->accept(*this, dest);

    // Leave a space for the test and jump instruction.
    int jumpToElse = startJump();

    // Compile the then arm.
    expr.thenArm()->accept(*this, dest);
    
    // Leave a space for the then arm to jump over the else arm.
    int jumpPastElse = startJump();

    // Compile the else arm.
    endJump(jumpToElse, OP_JUMP_IF_FALSE, dest);

    if (!expr.elseArm().isNull())
    {
      expr.elseArm()->accept(*this, dest);
    }
    else
    {
      // A missing 'else' arm is implicitly 'nothing'.
      write(OP_BUILT_IN, BUILT_IN_NOTHING, dest);
    }

    endJump(jumpPastElse, OP_JUMP);
  }
  
  void Compiler::visit(IsExpr& expr, int dest)
  {
    expr.value()->accept(*this, dest);
    
    int type = makeTemp();
    expr.type()->accept(*this, type);

    write(OP_IS, dest, type, dest);

    releaseTemp(); // type
  }
  
  void Compiler::visit(MatchExpr& expr, int dest)
  {
    // Compile the value.
    expr.value()->accept(*this, dest);
    
    Array<int> endJumps;
    
    // Compile each case.
    for (int i = 0; i < expr.cases().count(); i++)
    {
      const MatchClause& clause = expr.cases()[i];
      bool lastPattern = i == expr.cases().count() - 1;
      
      // Compile the pattern.
      PatternCompiler compiler(*this, !lastPattern);
      clause.pattern()->accept(compiler, dest);
      
      // Compile the body if the match succeeds.
      clause.body()->accept(*this, dest);
      
      // Then jump past the other cases.
      if (!lastPattern)
      {
        endJumps.add(startJump());
        
        // If this pattern fails, make it jump to the next case.
        compiler.endJumps();
      }
    }
    
    // Patch all the jumps now that we know where the end is.
    for (int i = 0; i < endJumps.count(); i++)
    {
      endJump(endJumps[i], OP_JUMP);
    }
  }
  
  void Compiler::visit(NameExpr& expr, int dest)
  {
    ASSERT(expr.resolved().isResolved(),
           "Names should be resolved before compiling.");
    
    if (expr.resolved().isLocal())
    {
      write(OP_MOVE, expr.resolved().index(), dest);
    }
    else
    {
      write(OP_GET_MODULE, expr.resolved().import(), expr.resolved().index(),
            dest);
    }
  }
  
  void Compiler::visit(NotExpr& expr, int dest)
  {
    expr.value()->accept(*this, dest);
    write(OP_NOT, dest);
  }
  
  void Compiler::visit(NothingExpr& expr, int dest)
  {
    write(OP_BUILT_IN, BUILT_IN_NOTHING, dest);
  }

  void Compiler::visit(NumberExpr& expr, int dest)
  {
    int index = compileConstant(expr);
    write(OP_CONSTANT, index, dest);
  }
  
  void Compiler::visit(OrExpr& expr, int dest)
  {
    expr.left()->accept(*this, dest);
    
    // Leave a space for the test and jump instruction.
    int jumpToEnd = startJump();
    
    expr.right()->accept(*this, dest);
    
    endJump(jumpToEnd, OP_JUMP_IF_TRUE, dest);
  }
  
  void Compiler::visit(RecordExpr& expr, int dest)
  {
    // TODO(bob): Hack. This assumes that the fields in the expression are in
    // the same order that the type expects. Eventually, the type needs to sort
    // them so that it understands (x: 1, y: 2) and (y: 2, x: 1) are the same
    // shape. When that happens, this will need to take that into account.
    
    Array<int> names;
    
    // Compile the fields.
    int firstField = -1;
    for (int i = 0; i < expr.fields().count(); i++)
    {
      int fieldSlot = makeTemp();
      if (i == 0) firstField = fieldSlot;
      
      expr.fields()[i].value->accept(*this, fieldSlot);
      names.add(vm_.addSymbol(expr.fields()[i].name));
    }
    
    // TODO(bob): Need to sort field names.
    
    // Create the record type.
    int type = vm_.addRecordType(names);
    
    // Create the record.
    write(OP_RECORD, firstField, type, dest);

    for (int i = 0; i < expr.fields().count(); i++)
    {
      releaseTemp();
    }
  }
  
  void Compiler::visit(ReturnExpr& expr, int dest)
  {
    // Compile the return value.
    if (expr.value().isNull())
    {
      // No value, so implicitly "nothing".
      write(OP_BUILT_IN, BUILT_IN_NOTHING, dest);
    }
    else
    {
      expr.value()->accept(*this, dest);
    }

    write(OP_RETURN, dest);
  }
  
  void Compiler::visit(SequenceExpr& expr, int dest)
  {
    for (int i = 0; i < expr.expressions().count(); i++)
    {
      // TODO(bob): Could compile all but the last expression with a special
      // sigil dest that means "won't use" and some exprs could check that to
      // omit some unnecessary instructions.
      expr.expressions()[i]->accept(*this, dest);
    }
  }

  void Compiler::visit(StringExpr& expr, int dest)
  {
    int index = compileConstant(expr);
    write(OP_CONSTANT, index, dest);
  }
  
  void Compiler::visit(ThrowExpr& expr, int dest)
  {
    // Compile the error object.
    expr.value()->accept(*this, dest);
    
    // Throw it.
    write(OP_THROW, dest);
  }
  
  void Compiler::visit(VariableExpr& expr, int dest)
  {
    // Compile the value.
    expr.value()->accept(*this, dest);

    // TODO(bob): Handle mutable variables.

    // Now pattern match on it.
    compilePattern(expr.pattern(), dest);
  }
  
  void Compiler::compilePattern(gc<Pattern> pattern, int dest)
  {
    if (pattern.isNull()) return;
    
    PatternCompiler compiler(*this);
    pattern->accept(compiler, dest);
  }

  int Compiler::compileExpressionOrConstant(Expr& expr)
  {
    const NumberExpr* number = expr.asNumberExpr();
    if (number != NULL)
    {
      return MAKE_CONSTANT(compileConstant(*number));
    }

    const StringExpr* string = expr.asStringExpr();
    if (string != NULL)
    {
      return MAKE_CONSTANT(compileConstant(*string));
    }

    int dest = makeTemp();

    expr.accept(*this, dest);
    return dest;
  }

  int Compiler::compileConstant(const NumberExpr& expr)
  {
    return method_->addConstant(new NumberObject(expr.value()));
  }

  int Compiler::compileConstant(const StringExpr& expr)
  {
    return method_->addConstant(new StringObject(expr.value()));
  }

  void Compiler::write(OpCode op, int a, int b, int c)
  {
    ASSERT_INDEX(a, 256);
    ASSERT_INDEX(b, 256);
    ASSERT_INDEX(c, 256);

    code_.add(MAKE_ABC(a, b, c, op));
  }

  int Compiler::startJump()
  {
    // Just write a dummy op to leave a space for the jump instruction.
    write(OP_MOVE);
    return code_.count() - 1;
  }

  void Compiler::endJump(int from, OpCode op, int a, int b)
  {
    int c;
    int offset = code_.count() - from - 1;
    
    // Add the offset as the last operand.
    if (a == -1)
    {
      a = offset;
      b = 0xff;
      c = 0xff;
    }
    else if (b == -1)
    {
      b = offset;
      c = 0xff;
    }
    else
    {
      c = offset;
    }
    
    code_[from] = MAKE_ABC(a, b, c, op);
  }

  int Compiler::makeTemp()
  {
    numTemps_++;
    if (maxSlots_ < numLocals_ + numTemps_)
    {
      maxSlots_ = numLocals_ + numTemps_;
    }
    
    return numLocals_ + numTemps_ - 1;
  }

  void Compiler::releaseTemp()
  {
    ASSERT(numTemps_ > 0, "No temp to release.");
    numTemps_--;
  }
  
  void PatternCompiler::endJumps()
  {
    // Since this isn't the last case, then every match failure should just
    // jump to the next case.
    for (int j = 0; j < tests_.count(); j++)
    {
      const MatchTest& test = tests_[j];
      
      if (test.slot == -1)
      {
        // This test is a field destructure, so just set the offset.
        compiler_.endJump(test.position, OP_JUMP); 
      }
      else
      {
        // A normal test.
        compiler_.endJump(test.position, OP_JUMP_IF_FALSE, test.slot); 
      }
    }
  }
  
  void PatternCompiler::visit(RecordPattern& pattern, int value)
  {
    // Recurse into the fields.
    for (int i = 0; i < pattern.fields().count(); i++)
    {
      // Test and destructure the field. This takes two instructions to encode
      // all of the operands.
      int field = compiler_.makeTemp();
      int symbol = compiler_.vm_.addSymbol(pattern.fields()[i].name);
      
      if (jumpOnFailure_)
      {
        compiler_.write(OP_TEST_FIELD, value, symbol, field);
        tests_.add(MatchTest(compiler_.code_.count(), -1));
        compiler_.startJump();
      }
      else
      {
        compiler_.write(OP_GET_FIELD, value, symbol, field);
      }
      
      // Recurse into the pattern, using that field.
      pattern.fields()[i].value->accept(*this, field);
      
      compiler_.releaseTemp();
    }
  }
  
  void PatternCompiler::visit(TypePattern& pattern, int value)
  {
    // Evaluate the expected type.
    int expected = compiler_.makeTemp();
    pattern.type()->accept(compiler_, expected);
    
    // Test if the value matches the expected type.
    compiler_.write(OP_IS, value, expected, expected);
    writeTest(expected);
    
    compiler_.releaseTemp();
  }
  
  void PatternCompiler::visit(ValuePattern& pattern, int value)
  {
    // Evaluate the expected value.
    int expected = compiler_.makeTemp();
    pattern.value()->accept(compiler_, expected);
    
    // Test if the value matches the expected one.
    compiler_.write(OP_EQUAL, value, expected, expected);
    writeTest(expected);
    
    compiler_.releaseTemp();
  }
  
  void PatternCompiler::visit(VariablePattern& pattern, int value)
  {
    ASSERT(pattern.resolved().isResolved(),
           "Must resolve before compiling.");
    
    // Copy the value into the new variable.
    compiler_.write(OP_MOVE, value, pattern.resolved().index());
    
    // Compile the inner pattern.
    if (!pattern.pattern().isNull())
    {
      pattern.pattern()->accept(*this, value);
    }
  }
  
  void PatternCompiler::visit(WildcardPattern& pattern, int value)
  {
    // Nothing to do.
  }
  
  void PatternCompiler::writeTest(int expected)
  {
    if (jumpOnFailure_)
    {
      tests_.add(MatchTest(compiler_.code_.count(), expected));
    }
    compiler_.write(OP_TEST_MATCH, expected);
  }

  gc<String> SignatureBuilder::build(const CallExpr& expr)
  {
    // 1 foo                 -> ()foo
    // 1 foo()               -> ()foo
    // 1 foo(2)              -> ()foo()
    // foo(1)                -> foo()
    // (1, 2) foo            -> (,)foo
    // foo(1, b: 2, 3, e: 4) -> foo(,b,,e)
    SignatureBuilder builder;
    
    if (!expr.leftArg().isNull())
    {
      builder.writeArg(expr.leftArg());
      builder.add(" ");
    }
    
    builder.add(expr.name()->cString());

    if (!expr.rightArg().isNull())
    {
      builder.add(" ");
      builder.writeArg(expr.rightArg());
    }
    
    return String::create(builder.signature_, builder.length_);
  }
  
  gc<String> SignatureBuilder::build(const MethodDef& method)
  {
    // def (a) foo               -> ()foo
    // def (a) foo()             -> ()foo
    // def (a) foo(b)            -> ()foo()
    // def foo(b)                -> foo()
    // def (a, b) foo            -> (,)foo
    // def foo(a, b: c, d, e: f) -> foo(,b,,e)
    SignatureBuilder builder;
    
    if (!method.leftParam().isNull())
    {
      builder.writeParam(method.leftParam());
      builder.add(" ");
    }
    
    builder.add(method.name()->cString());
    
    if (!method.rightParam().isNull())
    {
      builder.add(" ");
      builder.writeParam(method.rightParam());
    }
    
    return String::create(builder.signature_, builder.length_);
  }
  
  void SignatureBuilder::writeArg(gc<Expr> expr)
  {
    // TODO(bob): Clean up. Redundant with build().
    // If it's a record, destructure it into the signature.
    RecordExpr* record = expr->asRecordExpr();
    if (record != NULL)
    {
      for (int i = 0; i < record->fields().count(); i++)
      {
        add(record->fields()[i].name);
        add(":");
      }
      
      return;
    }
    
    // Right now, all other exprs mean "some arg goes here".
    add("0:");
  }

  void SignatureBuilder::writeParam(gc<Pattern> pattern)
  {
    // If it's a record, destructure it into the signature.
    RecordPattern* record = pattern->asRecordPattern();
    if (record != NULL)
    {
      for (int i = 0; i < record->fields().count(); i++)
      {
        add(record->fields()[i].name);
        add(":");
      }
      
      return;
    }
    
    // Any other pattern is implicitly a single-field record.
    add("0:");
  }
  
  void SignatureBuilder::add(gc<String> text)
  {
    add(text->cString());
  }
  
  void SignatureBuilder::add(const char* text)
  {
    int length = strlen(text);
    ASSERT(length_ + length < MAX_LENGTH, "Signature too long.");
    
    strcpy(signature_ + length_, text);
    length_ += strlen(text);
  }
}