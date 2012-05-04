#include "TokenTests.h"
#include "Token.h"
#include "Memory.h"
#include "RootSource.h"

namespace magpie
{
  void TokenTests::runTests()
  {
    create();
    is();
  }

  void TokenTests::create()
  {
    gc<String> text = String::create("foo");
    gc<Token> token = new Token(TOKEN_NAME, text, SourcePos("", -1, -1, -1, -1));

    EXPECT_EQUAL(TOKEN_NAME, token->type());
    EXPECT_EQUAL("foo", *token->text());
  }
  
  void TokenTests::is()
  {
    gc<String> text = String::create("foo");
    gc<Token> token = new Token(TOKEN_NAME, text, SourcePos("", -1, -1, -1, -1));
    
    EXPECT(token->is(TOKEN_NAME));
    EXPECT_FALSE(token->is(TOKEN_NUMBER));
  }
}
