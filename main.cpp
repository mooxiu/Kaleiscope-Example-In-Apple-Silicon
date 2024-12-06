#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include <iostream>
#include <utility>
#include <vector>
#include <map>
#include "include/KaleidoscopeJIT.h"

/**
 * Kaleidoscope language example
 *
# Compute the x'th fibonacci number.
def fib(x)
  if x < 3 then
    1
  else
    fib(x-1)+fib(x-2)

# This expression will compute the 40th number.
fib(40)
 *
 *
extern sin(arg);
extern cos(arg);
extern atan2(arg1 arg2);

atan2(sin(.4), cos(42))
 */

using namespace llvm;
using namespace llvm::orc;


/**
 * Lexer
 */
enum Token {
    tok_eof = -1,

    // commands
    tok_def = -2,
    tok_extern = -3,

    // primary
    tok_identifier = -4,
    tok_number = -5,
};

static std::string IdentifierStr; // Filled in if tok_identifier
static double NumVal;             // Filled in if tok_number

// gettok return the next token from standard input
static int gettok() {
    static int LastChar = ' ';

    // skip any white space
    while (isspace(LastChar)) {
        LastChar = getchar();
    }

    if (isalpha(LastChar)) {
        IdentifierStr = LastChar;
        // is alphanumeric
        while (isalnum(LastChar = getchar())) {
            IdentifierStr += LastChar;
        }

        if (IdentifierStr == "def") {
            return tok_def;
        }
        if (IdentifierStr == "extern") {
            return tok_extern;
        }
        return tok_identifier;
    }

    // [0-9.]+
    if (isdigit(LastChar) || LastChar == '.') {
        std::string NumStr;
        do {
            NumStr += LastChar;
            LastChar = getchar();
        } while (isdigit(LastChar) || LastChar == '.');

        NumVal = strtod(NumStr.c_str(), nullptr);
        return tok_number;
    }

    // comment
    if (LastChar == '#') {
        do {
            LastChar = getchar();
        } while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');
        if (LastChar != EOF) {
            return gettok();
        }
    }

    // check for end of file.
    if (LastChar == EOF) {
        return tok_eof;
    }

    // could possibly an operator like '+', '-'
    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;
}


/**
 * 2.2 AST
 * https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl02.html
 */

namespace {
// ExprAST - base class for all expression nodes
    class ExprAST {
    public:
        virtual ~ExprAST() = default;

        virtual Value *codegen() = 0;
    };

// NumberExprAST - Expression class for numeric literals
    class NumberExprAST : public ExprAST {
        double Val;
    public:
        NumberExprAST(double Val) : Val(Val) {}

        Value *codegen() override;
    };

// VariableExprAST - Expression class for referencing a variable, like 'a'
    class VariableExprAST : public ExprAST {
        std::string Name;
    public:
        explicit VariableExprAST(const std::string &name) : Name(name) {}

        Value *codegen() override;
    };

// BinaryExprAST - binary operator
    class BinaryExprAST : public ExprAST {
        char Op;
        std::unique_ptr<ExprAST> LHS, RHS;

    public:
        BinaryExprAST(char op,
                      std::unique_ptr<ExprAST> lhs,
                      std::unique_ptr<ExprAST> rhs) :
                Op(op), LHS(std::move(lhs)), RHS(std::move(rhs)) {}

        Value *codegen();
    };

// CallExprAST - expression class for function calls
    class CallExprAST : public ExprAST {
        std::string Callee;
        std::vector<std::unique_ptr<ExprAST>> Args;

    public:
        CallExprAST(const std::string &Callee,
                    std::vector<std::unique_ptr<ExprAST>> Args)
                : Callee(Callee), Args(std::move(Args)) {}

        Value *codegen() override;
    };

// PrototypeAST - represents the "prototype" for the function
// which captures its name, and its argument names
// my understanding is the declaration of a function without function body
    class PrototypeAST {
        std::string Name;
        std::vector<std::string> Args;

    public:
        PrototypeAST(const std::string &name,
                     std::vector<std::string> args)
                : Name(name), Args(std::move(args)) {}

        const std::string &getName() const { return Name; }

        Function *codegen();
    };

// FunctionAST - this class represents a function definition
    class FunctionAST {
        std::unique_ptr<PrototypeAST> Proto;
        std::unique_ptr<ExprAST> Body;
    public:
        FunctionAST(std::unique_ptr<PrototypeAST> proto,
                    std::unique_ptr<ExprAST> body)
                : Proto(std::move(proto)), Body(std::move(body)) {}

        Function *codegen();
    };
}

/**
 * 2.3 Parser Basics
 */

// ...this can make us look one token ahead
static int CurTok;

static int getNextToken() {
    return CurTok = gettok();
}

/// BinopPrecedence
static std::map<char, int> BinopPrecedence;

// GetTokPrecedence
static int GetTokPrecedence() {
    if (!isascii(CurTok)) {
        return -1;
    }
    // make sure it's a declared binop
    int TokPrec = BinopPrecedence[CurTok];
    if (TokPrec <= 0) return -1;
    return TokPrec;
}

// LogError - error handling
// TODO: why returning different types?
std::unique_ptr<ExprAST> LogError(const char *Str) {
    fprintf(stderr, "Error: %s\n", Str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
    LogError(Str);
    return nullptr;
}

/**
 * 2.4 Basic Expression Parsing
 */
static std::unique_ptr<ExprAST> ParseExpression();

// numberexpr :: number
// This function is to be called when current token is a tok_number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
    // takes the current number value, creates a NumberExprAST node
    auto Result = std::make_unique<NumberExprAST>(NumVal);
    // advances the lexer to the next token
    getNextToken();
    return std::move(Result);
}

// parenexp ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
    getNextToken(); // eat '('
    auto V = ParseExpression();
    if (!V) {
        return nullptr;
    }
    if (CurTok != ')') {
        return LogError("expected )");
    }
    getNextToken(); // eat ')'.
    return V;
}

// identifier
// ::= identifier
// ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
    std::string IdName = IdentifierStr;

    getNextToken(); // eat Identifier

    if (CurTok != '(') {
        // Simple variable ref
        return std::make_unique<VariableExprAST>(IdName);
    }

    // Call
    getNextToken(); //eat (
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurTok != ')') {
        while (true) {
            if (auto Arg = ParseExpression()) {
                Args.push_back(std::move(Arg));
            } else {
                return nullptr;
            }

            if (CurTok == ')') {
                break;
            }
            if (CurTok != ',') {
                return LogError("Expected ) or , in argument");
            }
            getNextToken();
        }
    }

    // Eat the ')'.
    getNextToken();
    return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

/**
 * primary
 *      ::= identifierexpr
 *      ::= numberexpr
 *      ::= parenexpr
 */
static std::unique_ptr<ExprAST> ParsePrimary() {
    switch (CurTok) {
        default:
            return LogError("unknown tokenw hen expecting an exp");
        case tok_identifier:
            return ParseIdentifierExpr();
        case tok_number:
            return ParseNumberExpr();
        case '(':
            return ParseParenExpr();
    }
}

/**
 * 2.5. Binary Expression Parsing
 */

// binoprhs
// ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
    while (true) {
        int TokPrec = GetTokPrecedence();
        // pair stream ends when token stream runs out of binary operators
        if (TokPrec < ExprPrec) return LHS;
        // ok, we know this is a binop
        int BinOp = CurTok;
        getNextToken();
        auto RHS = ParsePrimary();
        if (!RHS) {
            return nullptr;
        }

        int NextPrec = GetTokPrecedence();
        if (TokPrec < NextPrec) {
            RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
            if (!RHS) {
                return nullptr;
            }
        }
        LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
    }
}

// expression
//  ::= primary binoprhs
static std::unique_ptr<ExprAST> ParseExpression() {
    auto LHS = ParsePrimary();
    if (!LHS) {
        return nullptr;
    }
    return ParseBinOpRHS(0, std::move(LHS));
}

/**
 * 2.6 Parsing the Rest
 */
// prototype
//  ::= id'('id*')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
    if (CurTok != tok_identifier) {
        return LogErrorP("Expected function name in prototype");
    }
    std::string FnName = IdentifierStr;
    getNextToken();
    if (CurTok != '(') {
        return LogErrorP("Expected '(' in prototype");
    }

    // read the list of argument names
    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_identifier) {
        ArgNames.push_back(IdentifierStr);
    }
    if (CurTok != ')') {
        return LogErrorP("Expected ')' in prototype");
    }
    // success
    getNextToken();
    return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
    getNextToken(); // eat def
    auto Proto = ParsePrototype();
    if (!Proto) return nullptr;
    if (auto E = ParseExpression()) {
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

// toplevel ::=expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
    if (auto E = ParseExpression()) {
        // Make an anonymous proto
        auto Proto = std::make_unique<PrototypeAST>("", std::vector<std::string>());
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
    getNextToken(); // eat extern
    return ParsePrototype();
}


/**
 * Code Generation
 */
// opaque object that owns a lot of core LLVM data structures
// such as the type and constant value tables
static std::unique_ptr<LLVMContext> TheContext;
// contains functions and global variables
static std::unique_ptr<Module> TheModule;
// a helper object that makes it easy to generate LLVM instructions
// instance of IRBuilder class template keep track of the current place
// to insert instructions and has methods to create new instructions
static std::unique_ptr<IRBuilder<>> Builder;
// keeps track of which values are defined in the current scope and what their
// LLVM representation is
static std::map<std::string, Value *> NamedValues;

static std::unique_ptr<KaleidoscopeJIT> TheJIT;
static std::unique_ptr<FunctionPassManager> TheFPM;
static std::unique_ptr<LoopAnalysisManager> TheLAM;
static std::unique_ptr<FunctionAnalysisManager> TheFAM;
static std::unique_ptr<CGSCCAnalysisManager> TheCGAM;
static std::unique_ptr<ModuleAnalysisManager> TheMAM;
static std::unique_ptr<PassInstrumentationCallbacks> ThePIC;
static std::unique_ptr<StandardInstrumentations> TheSI;


Value *LogErrorV(const char *Str) {
    LogError(Str);
    return nullptr;
}

Value *NumberExprAST::codegen() {
    return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
    // Look this variable up in the function
    Value *V = NamedValues[Name];
    if (!V) {
        LogErrorV("Unknown variable name");
    }
    return V;
}

Value *BinaryExprAST::codegen() {
    Value *L = LHS->codegen();
    Value *R = RHS->codegen();
    if (!L || !R) {
        return nullptr;
    }
    switch (Op) {
        case '+':
            return Builder->CreateFAdd(L, R, "addtmp");
        case '-':
            return Builder->CreateFSub(L, R, "subtmp");
        case '*':
            return Builder->CreateFMul(L, R, "multmp");
        case '<':
            L = Builder->CreateFCmpULT(L, R, "cmptmp");
            // convert bool 0/1 to double 0.0 or 1.0
            return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
        default:
            return LogErrorV("invalid binary operator");
    }
}

// it looks like `sin(x)`
Value *CallExprAST::codegen() {
    // look up the name in the global module table
    Function *CalleeF = TheModule->getFunction(Callee);
    if (!CalleeF) {
        return LogErrorV("Unknown function referenced");
    }

    // if argument mismatch error
    if (CalleeF->arg_size() != Args.size()) {
        return LogErrorV("Incorrect # arguments passed");
    }

    std::vector<Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
        ArgsV.push_back(Args[i]->codegen());
        if (!ArgsV.back()) {
            return nullptr;
        }
    }
    return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

/**
 * function code generation
 * this is for all functions including extern function usage, we don't need to insert the body
 */
Function *PrototypeAST::codegen() {
    // make the function type: double(double, double) etc.
    std::vector<Type *> Doubles(Args.size(), Type::getDoubleTy(*TheContext));
    FunctionType *FT = FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);
    Function *F = Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());
    // set names for all arguments
    unsigned Idx = 0;
    for (auto &Arg: F->args()) {
        Arg.setName(Args[Idx++]);
    }
    return F;
}

// function code generation: a real function including body
Function *FunctionAST::codegen() {
    // First, check for an existing function from a previous extern declaration
    Function *TheFunction = TheModule->getFunction(Proto->getName());
    if (!TheFunction) {
        TheFunction = Proto->codegen();
    }
    if (!TheFunction) {
        return nullptr;
    }
    if (!TheFunction->empty()) {
        return (Function *) LogErrorV("Function cannot be redefined");
    }
    // Create a new basic block to start insertion info
    BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
    Builder->SetInsertPoint(BB);
    // Record the function arguments in the NamedValues map
    NamedValues.clear();
    for (auto &Arg: TheFunction->args()) {
        NamedValues[std::string(Arg.getName())] = &Arg;
    }
    if (Value *RetVal = Body->codegen()) {
        // Finish off the function
        Builder->CreateRet(RetVal);
        // Validate the generated code, checking for consistency
        verifyFunction(*TheFunction);

        // Run the optimizer on the function
        TheFPM->run(*TheFunction, *TheFAM);

        return TheFunction;
    }
    // Error reading body, remove function
    TheFunction->eraseFromParent();
    return nullptr;
}


/**
 * Top Level parsing and JIT Driver
 */

static void InitializeModuleAndManagers() {
    // Open a new context and module
    TheContext = std::make_unique<LLVMContext>();
    TheModule = std::make_unique<Module>("KaleidoscopeJIT", *TheContext);
//    TheModule->setDataLayout(TheJIT->getDataLayout());

    // Create a new builder for the module
    Builder = std::make_unique<IRBuilder<>>(*TheContext);

    // Create new pass and analysis managers
    TheFPM = std::make_unique<FunctionPassManager>();
    // 4 analysis managers allow us to add analysis passes
    // that run across the four levels of the IR hierarchy
    TheLAM = std::make_unique<LoopAnalysisManager>();
    TheFAM = std::make_unique<FunctionAnalysisManager>();
    TheCGAM = std::make_unique<CGSCCAnalysisManager>();
    TheMAM = std::make_unique<ModuleAnalysisManager>();
    // PassInstrumentationCallbacks and StandardInstrumentations are required
    // for the pass instrument framework, which allows developers to customize
    // what happens between passes

    // the pass instrumentation callbacks
    ThePIC = std::make_unique<PassInstrumentationCallbacks>();
    // standard instrumentation
    TheSI = std::make_unique<StandardInstrumentations>(*TheContext, true);

    TheSI->registerCallbacks(*ThePIC, TheMAM.get());

    // Add transform pass
    // do simple peehole optimizations and bit-twiddling optzns
    // do pattern matching and simplify
    TheFPM->addPass(InstCombinePass());
    // Reassociate expressions: a * b = b * a
    TheFPM->addPass(ReassociatePass());
    // Eliminate Common subexpressions: Global Value Numbering(GVN)
    TheFPM->addPass(GVNPass());
    // Simplify the control flow graph (deleting unreachable blocks, etc)
    TheFPM->addPass(SimplifyCFGPass());

    // Register analysis passes used in these transform pass
    PassBuilder PB;
    PB.registerModuleAnalyses(*TheMAM);
    PB.registerFunctionAnalyses(*TheFAM);
    PB.crossRegisterProxies(*TheLAM, *TheFAM, *TheCGAM, *TheMAM);
}

// deprecated: replaced by InitializeModuleAndManagers()
static void InitializeModule() {
    // Open a new context and module
    TheContext = std::make_unique<LLVMContext>();
    TheModule = std::make_unique<Module>("my cool jit", *TheContext);

    // create a new builder for the module
    Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Parsed a function definition.\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void HandleExtern() {
    if (auto ProtoAST = ParseExtern()) {
        if (auto *FnIR = ProtoAST->codegen()) {
            fprintf(stderr, "Parsed an extern\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void HandleTopLevelExpression() {
    // Evaluate a top-level expression into an anonymous function.
    if (auto FnAST = ParseTopLevelExpr()) {
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Parsed a top-level expr\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            // remove the anonymous expression
            FnIR->eraseFromParent();
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}


/// top ::= definition | external | expression | ';'
static void MainLoop() {
    while (true) {
        fprintf(stderr, "ready> ");
        switch (CurTok) {
            case tok_eof:
                return;
            case ';':
                getNextToken();
                break;
            case tok_def:
                HandleDefinition();
                break;
            case tok_extern:
                HandleExtern();
                break;
            default:
                HandleTopLevelExpression();
                break;
        }
    }
}


int main() {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
    // Install standard binary operators.
    // 1 is lowest precedence.
    BinopPrecedence['<'] = 10;
    BinopPrecedence['+'] = 20;
    BinopPrecedence['-'] = 20;
    BinopPrecedence['*'] = 40;  // highest.
    // ...
    fprintf(stderr, "ready> ");
    getNextToken();
    // Make the module, which holds all the code
//    InitializeModule();

    InitializeModuleAndManagers();
    MainLoop();

//    TheModule->print(errs(), nullptr);

//    std::array<
    return 0;
}




















