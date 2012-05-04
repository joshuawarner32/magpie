#pragma once

#include "Macros.h"
#include "Managed.h"

namespace magpie
{
  class Method;
  
  // A single Magpie module.
  class Module
  {
  public:
    Module(gc<Method> body)
    : body_(body)
    {}
    
    void reach();
    
    gc<Method> body() const { return body_; }
    
  private:
    // The code comprosing a module is compiled to a fake method, so that
    // loading a module is basically just executing a function call.
    gc<Method> body_;
    
    NO_COPY(Module);
  };
}