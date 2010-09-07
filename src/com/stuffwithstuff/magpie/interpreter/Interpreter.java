package com.stuffwithstuff.magpie.interpreter;

import java.util.*;

import com.stuffwithstuff.magpie.Identifiers;
import com.stuffwithstuff.magpie.ast.*;
import com.stuffwithstuff.magpie.interpreter.builtin.*;
import com.stuffwithstuff.magpie.parser.Position;

public class Interpreter {
  public Interpreter(InterpreterHost host) {
    mHost = host;
    
    // The class hierarchy is a bit confusing in a language where classes are
    // first class and inheritance is supported. Every object has a class.
    // However, classes are objects, which means they also have a class. In
    // addition, a class may also have a parent class.
    //
    // One key piece that it's important to understand is that the methods on
    // an object *always* come from its class. So, when you call a shared
    // method like "Foo bar", you are actually calling an *instance* method on
    // the class of of the object "Foo", its metaclass: FooClass.
    
    // Create a top-level scope.
    mGlobalScope = new Scope();

    // The class of all class objects. Given a class Foo, it's class will be a
    // singleton instance of its metaclass FooClass. The class of FooClass will
    // in turn be this class, Class. FooClass will also *inherit* from Class, so
    // that all of the methods defined on Class are available in FooClass (i.e.
    // "name", "parent", etc.)
    mClass = new ClassObj("Class", null);
    mGlobalScope.define("Class", mClass);
    
    // Object is the root class of all objects. All parent chains eventually
    // end here. Note that there is no distinct metaclass for class Object: its
    // metaclass is the main metaclass Class.
    mObjectClass = new ClassObj(mClass, "Object", null);
    mGlobalScope.define("Object", mObjectClass);
    
    // Now that both Class and Object exist, wire them up.
    mClass.setParent(mObjectClass);
    
    mArrayClass = createGlobalClass("Array");
    // TODO(bob): Should really be type and not class, I think?
    //mArrayClass.setParent(mClass);
    
    mBoolClass = createGlobalClass("Bool");
    mDynamicClass = createGlobalClass("Dynamic");
    mFnClass = createGlobalClass("Function");
    mIntClass = createGlobalClass("Int");

    mStringClass = createGlobalClass("String");

    // TODO(bob): At some point, may want different tuple types based on the
    // types of the fields.
    mTupleClass = createGlobalClass("Tuple");
    mTupleClass.addMethod("count", new NativeMethod.ClassFieldGetter("count",
        Expr.name("Int")));
    // TODO(bob): Hackish.
    for (int i = 0; i < 20; i++) {
      String name = "_" + Integer.toString(i);
      // TODO(bob): Using dynamic as the type here is lame. Ideally, there would
      // be a separate tuple class for each set of tuple field types and it
      // would have field getters that were typed to match the fields.
      mTupleClass.addMethod(name, new NativeMethod.ClassFieldGetter(name,
          Expr.name("Dynamic")));
    }
    
    mNothingClass = createGlobalClass("Nothing");
    mNothing = mNothingClass.instantiate();

    // "Never" is the evaluated type of an expression that can never yield a
    // result. It's equivalent to the bottom type. More concretely, it's the
    // type of a "return" expression, since a return will always unwind the
    // stack instead of actually yielding a result. In other words, if you do:
    //
    //   foo(return 123)
    //
    // The type of value passed to "foo" is "Never", meaning that in this case,
    // the call to "foo" will never occur since the return will unwind the
    // stack.
    mNeverClass = createGlobalClass("Never");

    // Register the built-in methods.
    BuiltIns.register(ArrayBuiltIns.class, mArrayClass);
    BuiltIns.register(BoolBuiltIns.class, mBoolClass);
    BuiltIns.register(ClassBuiltIns.class, mClass);
    BuiltIns.register(FunctionBuiltIns.class, mFnClass);
    BuiltIns.register(IntBuiltIns.class, mIntClass);
    BuiltIns.register(ObjectBuiltIns.class, mObjectClass);
    BuiltIns.register(StringBuiltIns.class, mStringClass);
  }
  
  public void load(List<Expr> expressions) {
    EvalContext context = createTopLevelContext();
    
    // Evaluate the expressions. This is the load time evaluation.
    for (Expr expr : expressions) {
      evaluate(expr, context);
    }
  }
  
  public String evaluate(Expr expr) {
    EvalContext context = createTopLevelContext();
    Obj result = evaluate(expr, context);
    
    // Convert it to a string.
    result = invokeMethod(expr, result, "toString", mNothing);
    return result.asString();
  }
  
  public Obj evaluateType(Expr expr) {
    // We create a context from the interpreter here because we need to evaluate
    // type expressions in the regular interpreter context where scopes hold
    // values not types.
    EvalContext context = createTopLevelContext();
    return evaluate(expr, context);
  }

  public Obj evaluateFunctionType(FunctionType type) {
    // Create the function type for the function.
    Obj paramType = evaluateType(type.getParamType());
    Obj returnType = evaluateType(type.getReturnType());
    
    return invokeMethod(mFnClass, Identifiers.CALL,
        createTuple(paramType, returnType));
  }
  
  public boolean hasMain() {
    return mGlobalScope.get("main") != null;
  }
  
  public void runMain() {
    EvalContext context = createTopLevelContext();
    Obj main = context.lookUp("main");
    if (main == null) return;
    
    if (!(main instanceof Callable)) {
      throw new InterpreterException("Member \"main\" is not a function.");
    }
    
    Callable mainFn = (Callable)main;
    mainFn.invoke(this, mNothing, mNothing);
  }

  /**
   * Invokes a named method on an object, passing in the given argument.
   * 
   * @param expr      The expression where this method invocation occurs. Just
   *                  used for position information if an error occurs.
   * @param receiver  The object the method is being invoked on.
   * @param name      The name of the method to invoke.
   * @param arg       The argument passed to the method.
   * @return          The result of invoking the method.
   */
  public Obj invokeMethod(Expr expr, Obj receiver, String name, Obj arg) {
    return invokeMethod(expr.getPosition(), receiver, name, arg);
  }

  /**
   * Invokes a named method on an object, passing in the given argument.
   * 
   * @param receiver  The object the method is being invoked on.
   * @param name      The name of the method to invoke.
   * @param arg       The argument passed to the method.
   * @return          The result of invoking the method.
   */
  public Obj invokeMethod(Obj receiver, String name, Obj arg) {
    return invokeMethod(Position.none(), receiver, name, arg);
  }
  
  /**
   * Invokes a named method on an object, passing in nothing as the argument.
   * 
   * @param receiver  The object the method is being invoked on.
   * @param name      The name of the method to invoke.
   * @return          The result of invoking the method.
   */
  public Obj invokeMethod(Obj receiver, String name) {
    return invokeMethod(receiver, name, mNothing);
  }

  public void print(String text) {
    mHost.print(text);
  }
  
  public void runtimeError(Expr expr, String format, Object... args) {
    mHost.runtimeError(expr.getPosition(), String.format(format, args));
  }
  
  public void runtimeError(String format, Object... args) {
    mHost.runtimeError(Position.none(), String.format(format, args));
  }
  
  public EvalContext createTopLevelContext() {
    return new EvalContext(mGlobalScope, mNothing);
  }
  
  public Scope getGlobals() { return mGlobalScope; }
  
  /**
   * Gets the single value () of type Nothing.
   * @return
   */
  public Obj nothing() { return mNothing; }

  public ClassObj getMetaclass() { return mClass; }
  public ClassObj getBoolType() { return mBoolClass; }
  public ClassObj getDynamicType() { return mDynamicClass; }
  public ClassObj getFunctionType() { return mFnClass; }
  public ClassObj getIntType() { return mIntClass; }
  public ClassObj getNothingType() { return mNothingClass; }
  public ClassObj getObjectType() { return mObjectClass; }
  public ClassObj getStringType() { return mStringClass; }
  public ClassObj getTupleType() { return mTupleClass; }
  public ClassObj getNeverType() { return mNeverClass; }
  public ClassObj getArrayType() { return mArrayClass; }
  
  public Obj createArray(List<Obj> elements) {
    return mArrayClass.instantiate(elements);
  }
  
  public Obj createBool(boolean value) {
    return mBoolClass.instantiate(value);
  }

  public Obj createInt(int value) {
    return mIntClass.instantiate(value);
  }
  
  public Obj createString(String value) {
    return mStringClass.instantiate(value);
  }

  public ClassObj createClass(String name, Scope scope) {
    // Create the metaclass. This will hold shared methods on the class.
    ClassObj metaclass = new ClassObj(mClass, name + "Class", mClass);
    
    // Define a method to cheat the type-checker.
    metaclass.addMethod("unsafeCast", new NativeMethod.ClassUnsafeCast(name));

    // Create the class object itself. This will hold the instance methods for
    // objects of the class.
    ClassObj classObj = new ClassObj(metaclass, name, mObjectClass);
    scope.define(name, classObj);
    
    return classObj;
  }
  
  public FnObj createFn(FnExpr expr, Scope closure) {
    // Create a new subclass just for this function so that it's implementation
    // of "call" has the correct return and parameter types.
    // TODO(bob): Figure out a simpler way to do this.
    ClassObj fnClass = new ClassObj(mClass, "FunctionType", mFnClass);
    fnClass.addMethod("call", new NativeMethod.FunctionCall(expr.getType()));
    
    return new FnObj(fnClass, closure, expr);
  }
  
  public Obj createTuple(Obj... fields) {
    return createTuple(Arrays.asList(fields));
  }
  
  public Obj createTuple(List<Obj> fields) {
    // A tuple is an object with fields whose names are zero-based numbers.
    Obj tuple = mTupleClass.instantiate();
    for (int i = 0; i < fields.size(); i++) {
      String name = "_" + Integer.toString(i);
      tuple.setField(name, fields.get(i));
    }
    
    tuple.setField(Identifiers.COUNT, createInt(fields.size()));
    
    return tuple;
  }
  
  public Obj evaluate(Expr expr, EvalContext context) {
    ExprEvaluator evaluator = new ExprEvaluator(this);
    return evaluator.evaluate(expr, context);
  }
  
  private ClassObj createGlobalClass(String name) {
    return createClass(name, mGlobalScope);
  }
  
  public void pushScriptPath(String path) {
    mScriptPaths.push(path);
  }
  
  public String getCurrentScript() {
    return mScriptPaths.peek();
  }
  
  public void popScriptPath() {
    mScriptPaths.pop();
  }

  private Obj invokeMethod(Position position, Obj receiver, String name, Obj arg) {
    // Look up the member.
    Callable method = receiver.getClassObj().findMethod(name);
    if (method != null) {
      // There's a special case we need to handle here. Consider:
      //
      //    foo bar(123)
      //
      // There are actually two ways to interpret it:
      // 1. Call a method 'bar' on foo, passing 123.
      // 2. Call a method 'bar' on foo with no argument, then call 'call' on the
      //    result, passing 123.
      // Scenario 2 comes up when 'bar' is a field getter and the field is
      // a callable, like an array. To avoid having to do foo bar()(123), we'll
      // do something a little sneaky: If the method being called doesn't take
      // an argument, we'll invoke it without one, then immediately call the
      // result with the argument.
      if ((method.getType().getParamNames().size() == 0) &&
          (arg != null) && (arg != mNothing)) {
        Obj result = method.invoke(this, receiver, null);
        
        // Now invoke the result of the method with the argument.
        if (arg != null) {
          result = invokeMethod(position, result, Identifiers.CALL, arg);
        }
        
        return result;
      } else {
        // Just a regular method call.
        return method.invoke(this, receiver, arg);
      }
    }
    
    // If there isn't an actual method, then calling a setter defaults to
    // creating a field with the given name.
    if (Identifiers.isSetter(name)) {
      String field = Identifiers.getSetterBaseName(name);
      receiver.setField(field, arg);
      return arg;
    }
    
    // If that fails, see if we can find a field with the name.
    Obj field = receiver.getField(name);
    if (field != null) {
      // If we have an argument, treat the field like a callable.
      if (arg != null) {
        return invokeMethod(position, field, Identifiers.CALL, arg);
      }
      
      // Otherwise just return the field itself.
      return field;
    }
    
    runtimeError(position,
        "Could not find a variable or method named \"%s\" on %s.",
        name, receiver.getClassObj());
    
    return mNothing;
  }
  
  private void runtimeError(Position position, String format, Object... args) {
    mHost.runtimeError(position, String.format(format, args));
  }

  private final InterpreterHost mHost;
  private Scope mGlobalScope;
  private final ClassObj mClass;
  private final ClassObj mArrayClass;
  private final ClassObj mBoolClass;
  private final ClassObj mDynamicClass;
  private final ClassObj mFnClass;
  private final ClassObj mIntClass;
  private final ClassObj mNothingClass;
  private final ClassObj mNeverClass;
  private final ClassObj mObjectClass;
  private final ClassObj mStringClass;
  private final ClassObj mTupleClass;
  
  private final Obj mNothing;
  private final Stack<String> mScriptPaths = new Stack<String>();
}
