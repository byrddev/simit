#include "llvm_backend.h"

#include <cstdint>
#include <iostream>
#include <stack>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/JIT.h"

#include "llvm_codegen.h"
#include "types.h"
#include "ir.h"
#include "ir_printer.h"
#include "llvm_function.h"
#include "macros.h"
#include "runtime.h"

using namespace std;
using namespace simit::ir;
using namespace simit::internal;

namespace simit {
namespace internal {

const std::string VAL_SUFFIX(".val");
const std::string PTR_SUFFIX(".ptr");
const std::string LEN_SUFFIX(".len");

// class LLVMBackend
bool LLVMBackend::llvmInitialized = false;

LLVMBackend::LLVMBackend() : val(nullptr),
                             builder(new llvm::IRBuilder<>(LLVM_CONTEXT)) {

  // For now we'll assume fields are always dense row major
  this->fieldStorage = TensorStorage::DenseRowMajor;

  if (!llvmInitialized) {
    llvm::InitializeNativeTarget();
    llvmInitialized = true;
  }
}

LLVMBackend::~LLVMBackend() {}

simit::Function *LLVMBackend::compile(Func func) {
  iassert(func.getBody().defined()) << "cannot compile an undefined function";

  this->module = new llvm::Module(func.getName(), LLVM_CONTEXT);
  this->storage = func.getStorage();

  // Create global buffer variables
  map<Var, llvm::Value*> buffers;
  for (auto &var : storage) {
    iassert(var.getType().isTensor());
    llvm::Type *ctype = createLLVMType(var.getType().toTensor()->componentType);
    llvm::PointerType *globalType = llvm::PointerType::getUnqual(ctype);

    llvm::GlobalVariable* buffer =
        new llvm::GlobalVariable(*module, globalType,
                                 false, llvm::GlobalValue::InternalLinkage,
                                 llvm::ConstantPointerNull::get(globalType),
                                 var.getName());
    buffer->setAlignment(8);
    buffers.insert(pair<Var,llvm::Value*>(var,buffer));
  }

  // Create compute function
  llvm::Function *llvmFunc = emitEmptyFunction(func.getName(),
                                               func.getArguments(),
                                               func.getResults());

  for (auto &var : storage) {
    llvm::Value *buffer = buffers[var];
    llvm::Value *llvmTmp = builder->CreateLoad(buffer, buffer->getName());
    symtable.insert(llvmTmp->getName(), llvmTmp);
  }

  compile(func.getBody());
  builder->CreateRetVoid();
  symtable.clear();


  // Declare malloc and free
  llvm::FunctionType *m =
      llvm::FunctionType::get(LLVM_INT8PTR, {LLVM_INT}, false);
  llvm::Function *malloc =
      llvm::Function::Create(m,llvm::Function::ExternalLinkage,"malloc",module);

  llvm::FunctionType *f =
      llvm::FunctionType::get(LLVM_VOID, {LLVM_INT8PTR}, false);
  llvm::Function *free =
      llvm::Function::Create(f,llvm::Function::ExternalLinkage,"free",module);


  // Create initialization function
  emitEmptyFunction(func.getName()+".init",
                    func.getArguments(), func.getResults());

  for (auto &var : storage) {
    Type type = var.getType();
    llvm::Type *llvmType = createLLVMType(type);

    iassert(type.isTensor());
    const TensorType *ttype = type.toTensor();
    llvm::Value *len = emitComputeLen(ttype, storage.get(var));
    unsigned compSize = ttype->componentType.bytes();
    llvm::Value *size = builder->CreateMul(len, llvmInt(compSize));
    llvm::Value *mem = builder->CreateCall(malloc, size);

    mem = builder->CreateCast(llvm::Instruction::CastOps::BitCast,mem,llvmType);
    builder->CreateStore(mem, buffers[var]);
  }
  builder->CreateRetVoid();
  symtable.clear();

  // Create de-initialization function
  emitEmptyFunction(func.getName()+".deinit",
                    func.getArguments(), func.getResults());

  for (auto &var : storage) {
    llvm::Value *tmpPtr = builder->CreateLoad(buffers[var]);
    tmpPtr = builder->CreateCast(llvm::Instruction::CastOps::BitCast,
                                 tmpPtr, LLVM_INT8PTR);
    builder->CreateCall(free, tmpPtr);
  }
  builder->CreateRetVoid();
  symtable.clear();

  bool requiresInit = buffers.size() > 0;
  return new LLVMFunction(func, llvmFunc, requiresInit, module);
}

llvm::Value *LLVMBackend::compile(const Expr &expr) {
  expr.accept(this);
  llvm::Value *tmp = val;
  val = nullptr;
  return tmp;
}

void LLVMBackend::compile(const Stmt &stmt) {
  stmt.accept(this);
  val = nullptr;
}

void LLVMBackend::visit(const FieldRead *op) {
  val = emitFieldRead(op->elementOrSet, op->fieldName);
}

void LLVMBackend::visit(const TensorRead *op) {
  ierror << "No code generation for this type";
}

void LLVMBackend::visit(const TupleRead *op) {
  ierror << "No code generation for this type";
}

void LLVMBackend::visit(const ir::IndexRead *op) {
  // TODO: Add support for different indices (contained in the Set type).
  unsigned int indexLoc = 1 + op->kind;

  iassert(op->edgeSet.type().isSet());
  iassert(op->edgeSet.type().toSet()->endpointSets.size() > 0);

  llvm::Value *edgesValue = compile(op->edgeSet);
  val = builder->CreateExtractValue(edgesValue,{indexLoc},util::toString(*op));
}

void LLVMBackend::visit(const ir::Length *op) {
  val = emitComputeLen(op->indexSet);
}

void LLVMBackend::visit(const Map *op) {
  ierror << "No code generation for this type";
}

void LLVMBackend::visit(const IndexedTensor *op) {
  ierror << "No code generation for this expr: " << util::toString(*op);
}

void LLVMBackend::visit(const IndexExpr *op) {
  ierror << "No code generation for this expr: " << util::toString(*op);
}

void LLVMBackend::visit(const TensorWrite *op) {
  ierror << "No code generation for this expr: " << util::toString(*op);
}

void LLVMBackend::visit(const Literal *op) {
  iassert(op->type.isTensor()) << "Only tensor literals supported for now";
  const TensorType *type = op->type.toTensor();

  if (type->order() == 0) {
    ScalarType ctype = type->componentType;
    switch (ctype.kind) {
      case ScalarType::Int: {
        iassert(ctype.bytes() == 4) << "Only 4-byte ints currently supported";
        val = llvmInt(((int*)op->data)[0]);
        break;
      }
      case ScalarType::Float: {
        iassert(ctype.bytes() == 8) << "Only 8-byte floats currently supported";
        val = llvmFP(((double*)op->data)[0]);
        break;
      }
      case ScalarType::Boolean: {
        iassert(ctype.bytes() == sizeof(bool));
        bool data = ((bool*)op->data)[0];
        val = llvm::ConstantInt::get(LLVM_BOOL, llvm::APInt(1, data, false));
        break;
      }
    }
  }
  else {
    val = llvmPtr(op);
  }
  iassert(val);
}

void LLVMBackend::visit(const VarExpr *op) {
  iassert(symtable.contains(op->var.getName()))
      << op->var << "not found in symbol table";

  val = symtable.get(op->var.getName());

  // Special case: check if the symbol is a scalar and the llvm value is a ptr,
  // in which case we must load the value.  This case arises because we keep
  // many scalars on the stack.  One exceptions to this are loop variables.
  if (isScalar(op->type) && val->getType()->isPointerTy()) {
    string valName = string(val->getName()) + VAL_SUFFIX;
    val = builder->CreateAlignedLoad(val, 8, valName);
  }
}

void LLVMBackend::visit(const ir::Load *op) {
  llvm::Value *buffer = compile(op->buffer);
  llvm::Value *index = compile(op->index);

  string locName = string(buffer->getName()) + PTR_SUFFIX;
  llvm::Value *bufferLoc = builder->CreateInBoundsGEP(buffer, index, locName);

  string valName = string(buffer->getName()) + VAL_SUFFIX;
  val = builder->CreateAlignedLoad(bufferLoc, 8, valName);
}

void LLVMBackend::visit(const Call *op) {
  std::map<Func, llvm::Intrinsic::ID> llvmIntrinsicByName =
                                  {{ir::Intrinsics::sin,llvm::Intrinsic::sin},
                                   {ir::Intrinsics::cos,llvm::Intrinsic::cos},
                                   {ir::Intrinsics::sqrt,llvm::Intrinsic::sqrt},
                                   {ir::Intrinsics::log,llvm::Intrinsic::log},
                                   {ir::Intrinsics::exp,llvm::Intrinsic::exp},
                                   {ir::Intrinsics::pow,llvm::Intrinsic::pow}};
  
  std::vector<llvm::Type*> argTypes;
  std::vector<llvm::Value*> args;
  llvm::Function *fun = nullptr;
  
  // compile arguments first
  for (auto a: op->actuals) {
    //FIX: remove once solve() is no longer needed
    //iassert(isScalar(a.type()));
    argTypes.push_back(createLLVMType(a.type().toTensor()->componentType));
    args.push_back(compile(a));
  }

  // these are intrinsic functions
  // first, see if this is an LLVM intrinsic
  auto foundIntrinsic = llvmIntrinsicByName.find(op->func);
  if (foundIntrinsic != llvmIntrinsicByName.end()) {
    fun = llvm::Intrinsic::getDeclaration(module, foundIntrinsic->second,
      argTypes);
  }
  // now check if it is an intrinsic from libm
  else if (op->func == ir::Intrinsics::atan2 ||
           op->func == ir::Intrinsics::tan   ||
           op->func == ir::Intrinsics::asin  ||
           op->func == ir::Intrinsics::acos    ) {
    auto ftype = llvm::FunctionType::get(LLVM_DOUBLE, argTypes, false);
    fun= llvm::cast<llvm::Function>(module->getOrInsertFunction(
      op->func.getName(),ftype));
  }
   else if (op->func == ir::Intrinsics::norm) {
    iassert(args.size() == 1);
    auto type = op->actuals[0].type().toTensor();
    
    // special case for vec3f
    if (type->dimensions[0].getSize() == 3) {
      llvm::Value *x = args[0];

      llvm::Value *x0 = loadFromArray(x, llvmInt(0));
      llvm::Value *sum = builder->CreateFMul(x0, x0);

      llvm::Value *x1 = loadFromArray(x, llvmInt(1));
      llvm::Value *x1pow = builder->CreateFMul(x1, x1);
      sum = builder->CreateFAdd(sum, x1pow);

      llvm::Value *x2 = loadFromArray(x, llvmInt(2));
      llvm::Value *x2pow = builder->CreateFMul(x2, x2);
      sum = builder->CreateFAdd(sum, x2pow);

      llvm::Function *sqrt= llvm::Intrinsic::getDeclaration(module,
                                                            llvm::Intrinsic::sqrt,
                                                            {LLVM_DOUBLE});
      val = builder->CreateCall(sqrt, sum);
    } else {
      args.push_back(emitComputeLen(type->dimensions[0]));
      val = emitCall("norm", args, LLVM_DOUBLE);
    }
    
    return;
  }
  else if (op->func == ir::Intrinsics::solve) {
    // FIX: compile is making these be LLVM_DOUBLE, but I need
    // LLVM_DOUBLEPTR
    std::vector<llvm::Type*> argTypes2 =
        {LLVM_DOUBLEPTR, LLVM_DOUBLEPTR, LLVM_DOUBLEPTR, LLVM_INT, LLVM_INT};

    auto type = op->actuals[0].type().toTensor();
    args.push_back(emitComputeLen(type->dimensions[0]));
    args.push_back(emitComputeLen(type->dimensions[1]));

    auto ftype = llvm::FunctionType::get(LLVM_DOUBLE, argTypes2, false);
    fun = llvm::cast<llvm::Function>(module->getOrInsertFunction("cMatSolve",
                                                                 ftype));
  }
  else if (op->func == ir::Intrinsics::loc) {
    val = emitCall("loc", args, LLVM_INT);
    return;
  }
  else if (op->func == ir::Intrinsics::dot) {
    // we need to add the vector length to the args
    auto type1 = op->actuals[0].type().toTensor();
    auto type2 = op->actuals[1].type().toTensor();
    uassert(type1->dimensions[0] == type2->dimensions[0]) <<
      "dimension mismatch in dot product";
    args.push_back(emitComputeLen(type1->dimensions[0]));
    val = emitCall("dot", args, LLVM_DOUBLE);
    return;
  }
  else {
    // if not an intrinsic function, try to find it in the module
    fun = module->getFunction(op->func.getName());
  }
  iassert(fun) << "Unsupported function call";

  val = builder->CreateCall(fun, args);
}

void LLVMBackend::visit(const Neg *op) {
  iassert(isScalar(op->type));
  llvm::Value *a = compile(op->a);

  switch (op->type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      val = builder->CreateNeg(a);
      break;
    case ScalarType::Float:
      val = builder->CreateFNeg(a);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot negate a boolean value.";
  }
}

void LLVMBackend::visit(const Add *op) {
  iassert(isScalar(op->type));

  llvm::Value *a = compile(op->a);
  llvm::Value *b = compile(op->b);

  switch (op->type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      val = builder->CreateAdd(a, b);
      break;
    case ScalarType::Float:
      val = builder->CreateFAdd(a, b);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot add boolean values.";
  }
}

void LLVMBackend::visit(const Sub *op) {
  iassert(isScalar(op->type));

  llvm::Value *a = compile(op->a);
  llvm::Value *b = compile(op->b);

  switch (op->type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      val = builder->CreateSub(a, b);
      break;
    case ScalarType::Float:
      val = builder->CreateFSub(a, b);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot subtract boolean values.";
  }
}

void LLVMBackend::visit(const Mul *op) {
  iassert(isScalar(op->type));

  llvm::Value *a = compile(op->a);
  llvm::Value *b = compile(op->b);

  switch (op->type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      val = builder->CreateMul(a, b);
      break;
    case ScalarType::Float:
      val = builder->CreateFMul(a, b);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot multiply boolean values.";
  }
}

void LLVMBackend::visit(const Div *op) {
  iassert(isScalar(op->type));

  llvm::Value *a = compile(op->a);
  llvm::Value *b = compile(op->b);

  switch (op->type.toTensor()->componentType.kind) {
    case ScalarType::Int:
      // TODO: Figure out what's the deal with integer div. Cast to fp, div and
      // truncate?
      not_supported_yet;
      break;
    case ScalarType::Float:
      val = builder->CreateFDiv(a, b);
      break;
    case ScalarType::Boolean:
      iassert(false) << "Cannot divide boolean values.";
  }
}

#define LLVMBACKEND_VISIT_COMPARE_OP(typename, op, float_cmp, int_cmp) \
void LLVMBackend::visit(const ir::typename *op) {\
  iassert(isBoolean(op->type));\
  iassert(isScalar(op->a.type()));\
  iassert(isScalar(op->b.type()));\
\
  llvm::Value *a = compile(op->a);\
  llvm::Value *b = compile(op->b);\
\
  const TensorType *type = op->a.type().toTensor();\
  if (type->componentType == ScalarType::Float) {\
    val = builder->float_cmp(a, b);\
  } else {\
    val = builder->int_cmp(a, b);\
  }\
}

LLVMBACKEND_VISIT_COMPARE_OP(Eq, op, CreateFCmpOEQ, CreateICmpEQ)
LLVMBACKEND_VISIT_COMPARE_OP(Ne, op, CreateFCmpONE, CreateICmpNE)
LLVMBACKEND_VISIT_COMPARE_OP(Gt, op, CreateFCmpOGT, CreateICmpSGT)
LLVMBACKEND_VISIT_COMPARE_OP(Lt, op, CreateFCmpOLT, CreateICmpSLT)
LLVMBACKEND_VISIT_COMPARE_OP(Ge, op, CreateFCmpOGE, CreateICmpSGE)
LLVMBACKEND_VISIT_COMPARE_OP(Le, op, CreateFCmpOLE, CreateICmpSLE)

void LLVMBackend::visit(const ir::And *op) {
  iassert(isBoolean(op->type));
  iassert(isBoolean(op->a.type()));
  iassert(isBoolean(op->b.type()));

  llvm::Value *a = compile(op->a);
  llvm::Value *b = compile(op->b);

  val = builder->CreateAnd(a, b);
}

void LLVMBackend::visit(const ir::Or *op) {
  iassert(isBoolean(op->type));
  iassert(isBoolean(op->a.type()));
  iassert(isBoolean(op->b.type()));

  llvm::Value *a = compile(op->a);
  llvm::Value *b = compile(op->b);

  val = builder->CreateOr(a, b);
}

void LLVMBackend::visit(const ir::Not *op) {
  iassert(isBoolean(op->type));
  iassert(isBoolean(op->a.type()));

  llvm::Value *a = compile(op->a);

  val = builder->CreateNot(a);
}

void LLVMBackend::visit(const ir::Xor *op) {
  iassert(isBoolean(op->type));
  iassert(isBoolean(op->a.type()));
  iassert(isBoolean(op->b.type()));

  llvm::Value *a = compile(op->a);
  llvm::Value *b = compile(op->b);

  val = builder->CreateXor(a, b);
}

void LLVMBackend::visit(const AssignStmt *op) {
  switch (op->cop.kind) {
    case ir::CompoundOperator::None: {
      emitAssign(op->var, op->value);
      return;
    }
    case ir::CompoundOperator::Add: {
      emitAssign(op->var, Add::make(op->var, op->value));
      return;
    }
    default: ierror << "Unknown compound operator type";
  }
}

void LLVMBackend::visit(const FieldWrite *op) {
  /// \todo field writes of scalars to tensors and tensors to tensors should be
  ///       handled by the lowering so that we only write scalars to scalars
  ///       in the backend
//  iassert(isScalar(op->value.type()))
//      << "assignment non-scalars should have been lowered by now";

  iassert(op->value.type().isTensor());
  iassert(getFieldType(op->elementOrSet, op->fieldName).isTensor());
  iassert(op->elementOrSet.type().isSet()||op->elementOrSet.type().isElement());

  Type fieldType = getFieldType(op->elementOrSet, op->fieldName);
  Type valueType = op->value.type();

  // Assigning a scalar to an n-order tensor
  if (fieldType.toTensor()->order() > 0 && valueType.toTensor()->order() == 0) {
    iassert(op->cop.kind == CompoundOperator::None)
        << "Compound write when assigning scalar to n-order tensor";
    if (isa<Literal>(op->value) &&
        ((double*)to<Literal>(op->value)->data)[0] == 0.0) {
      // emit memset 0
      llvm::Value *fieldPtr = emitFieldRead(op->elementOrSet, op->fieldName);

      const TensorType *tensorFieldType = fieldType.toTensor();

      llvm::Value *fieldLen = emitComputeLen(tensorFieldType, fieldStorage);
      unsigned compSize = tensorFieldType->componentType.bytes();
      llvm::Value *fieldSize = builder->CreateMul(fieldLen,llvmInt(compSize));

      builder->CreateMemSet(fieldPtr, llvmInt(0,8), fieldSize, compSize);
    }
    else {
      not_supported_yet;
    }
  }
  else {
    // emit memcpy
    llvm::Value *fieldPtr = emitFieldRead(op->elementOrSet, op->fieldName);
    llvm::Value *valuePtr;
    switch (op->cop.kind) {
      case ir::CompoundOperator::None: {
        valuePtr = compile(op->value);
        break;
      }
      case ir::CompoundOperator::Add: {
        valuePtr = compile(Add::make(
            FieldRead::make(op->elementOrSet, op->fieldName), op->value));
        break;
      }
      default: ierror << "Unknown compound operator type";
    }

    const TensorType *tensorFieldType = fieldType.toTensor();

    llvm::Value *fieldLen = emitComputeLen(tensorFieldType, fieldStorage);
    unsigned elemSize = tensorFieldType->componentType.bytes();
    llvm::Value *fieldSize = builder->CreateMul(fieldLen, llvmInt(elemSize));

    builder->CreateMemCpy(fieldPtr, valuePtr, fieldSize, elemSize);
  }
}

void LLVMBackend::visit(const ir::Store *op) {
  llvm::Value *buffer = compile(op->buffer);
  llvm::Value *index = compile(op->index);
  llvm::Value *value;
  switch (op->cop.kind) {
    case CompoundOperator::None: {
      value = compile(op->value);
      break;
    }
    case CompoundOperator::Add: {
      value = compile(Add::make(Load::make(op->buffer, op->index), op->value));
      break;
    }
    default: ierror << "Unknown compound operator type";
  }

  string locName = string(buffer->getName()) + PTR_SUFFIX;
  llvm::Value *bufferLoc = builder->CreateInBoundsGEP(buffer, index, locName);
  builder->CreateAlignedStore(value, bufferLoc, 8);
}

void LLVMBackend::visit(const For *op) {
  std::string iName = op->var.getName();
  ForDomain domain = op->domain;

  llvm::Value *iNum = nullptr;
  switch (domain.kind) {
    case ForDomain::IndexSet: {
      iNum = emitComputeLen(domain.indexSet);
      break;
    }
    case ForDomain::Endpoints:
      not_supported_yet;
      break;
    case ForDomain::Edges:
      not_supported_yet;
      break;
    case ForDomain::Neighbors:
      not_supported_yet;
      break;
  }
  iassert(iNum);

  llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();

  // Loop Header
  llvm::BasicBlock *entryBlock = builder->GetInsertBlock();

  llvm::BasicBlock *loopBodyStart =
      llvm::BasicBlock::Create(LLVM_CONTEXT, iName+"_loop_body", llvmFunc);
  builder->CreateBr(loopBodyStart);
  builder->SetInsertPoint(loopBodyStart);

  llvm::PHINode *i = builder->CreatePHI(LLVM_INT32, 2, iName);
  i->addIncoming(builder->getInt32(0), entryBlock);

  // Loop Body
  symtable.scope();
  symtable.insert(iName, i);
  compile(op->body);
  symtable.unscope();

  // Loop Footer
  llvm::BasicBlock *loopBodyEnd = builder->GetInsertBlock();
  llvm::Value *i_nxt = builder->CreateAdd(i, builder->getInt32(1),
                                          iName+"_nxt", false, true);
  i->addIncoming(i_nxt, loopBodyEnd);

  llvm::Value *exitCond = builder->CreateICmpSLT(i_nxt, iNum, iName+"_cmp");
  llvm::BasicBlock *loopEnd = llvm::BasicBlock::Create(LLVM_CONTEXT,
                                                       iName+"_loop_end", llvmFunc);
  builder->CreateCondBr(exitCond, loopBodyStart, loopEnd);
  builder->SetInsertPoint(loopEnd);
}

void LLVMBackend::visit(const ir::ForRange *op) {
  std::string iName = op->var.getName();
  
  llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();
  
  // Loop Header
  llvm::BasicBlock *entryBlock = builder->GetInsertBlock();

  llvm::Value *rangeStart = compile(op->start);
  llvm::Value *rangeEnd = compile(op->end);

  llvm::BasicBlock *loopBodyStart =
    llvm::BasicBlock::Create(LLVM_CONTEXT, iName+"_loop_body", llvmFunc);
  builder->CreateBr(loopBodyStart);
  builder->SetInsertPoint(loopBodyStart);

  llvm::PHINode *i = builder->CreatePHI(LLVM_INT32, 2, iName);
  i->addIncoming(rangeStart, entryBlock);

  // Loop Body
  symtable.scope();
  symtable.insert(iName, i);
  compile(op->body);
  symtable.unscope();
  
  // Loop Footer
  llvm::BasicBlock *loopBodyEnd = builder->GetInsertBlock();
  llvm::Value *i_nxt = builder->CreateAdd(i, builder->getInt32(1),
                                          iName+"_nxt", false, true);
  i->addIncoming(i_nxt, loopBodyEnd);

  llvm::Value *exitCond = builder->CreateICmpSLT(i_nxt, rangeEnd,
                                                 iName+"_cmp");
  llvm::BasicBlock *loopEnd = llvm::BasicBlock::Create(LLVM_CONTEXT,
                                                       iName+"_loop_end",
                                                       llvmFunc);
  builder->CreateCondBr(exitCond, loopBodyStart, loopEnd);
  builder->SetInsertPoint(loopEnd);

}

void LLVMBackend::visit(const ir::IfThenElse *op) {
  llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();

  llvm::Value *cond = compile(op->condition);
  llvm::Value *condEval = builder->CreateICmpEQ(builder->getTrue(), cond);


  llvm::BasicBlock *thenBlock = llvm::BasicBlock::Create(LLVM_CONTEXT, "then", llvmFunc);
  llvm::BasicBlock *elseBlock = llvm::BasicBlock::Create(LLVM_CONTEXT, "else");
  llvm::BasicBlock *exitBlock = llvm::BasicBlock::Create(LLVM_CONTEXT, "exit");
  builder->CreateCondBr(condEval, thenBlock, elseBlock);

  builder->SetInsertPoint(thenBlock);
  compile(op->thenBody);
  builder->CreateBr(exitBlock);
  thenBlock = builder->GetInsertBlock();

  llvmFunc->getBasicBlockList().push_back(elseBlock);

  builder->SetInsertPoint(elseBlock);
  compile(op->elseBody);
  builder->CreateBr(exitBlock);
  elseBlock = builder->GetInsertBlock();

  llvmFunc->getBasicBlockList().push_back(exitBlock);
  builder->SetInsertPoint(exitBlock);

}

void LLVMBackend::visit(const While *op) {
  llvm::Function *llvmFunc = builder->GetInsertBlock()->getParent();

  llvm::Value *cond = compile(op->condition);
  llvm::Value *condEval = builder->CreateICmpEQ(builder->getTrue(), cond);


  llvm::BasicBlock *bodyBlock = llvm::BasicBlock::Create(LLVM_CONTEXT, "body", llvmFunc);
  llvm::BasicBlock *checkBlock = llvm::BasicBlock::Create(LLVM_CONTEXT, "check");
  llvm::BasicBlock *exitBlock = llvm::BasicBlock::Create(LLVM_CONTEXT, "exit");
  builder->CreateCondBr(condEval, bodyBlock, exitBlock);

  builder->SetInsertPoint(bodyBlock);
  compile(op->body);
  builder->CreateBr(checkBlock);
  bodyBlock = builder->GetInsertBlock();
  
  llvmFunc->getBasicBlockList().push_back(checkBlock);
  builder->SetInsertPoint(checkBlock);
  llvm::Value *cond2 = compile(op->condition);
  llvm::Value *condEval2 = builder->CreateICmpEQ(builder->getTrue(), cond2);
  builder->CreateCondBr(condEval2, bodyBlock, exitBlock);
  
  llvmFunc->getBasicBlockList().push_back(exitBlock);
  builder->SetInsertPoint(exitBlock);

}

void LLVMBackend::visit(const Block *op) {
  compile(op->first);
  if (op->rest.defined()) {
    compile(op->rest);
  }
}

void LLVMBackend::visit(const Pass *op) {
  // Nothing to do
}

static
void printStmtAddScalarArg(std::string &format,
                           std::vector<llvm::Value*> &args,
                           simit::ir::ScalarType type,
                           llvm::Value *val) {
  switch (type.kind) {
  case simit::ir::ScalarType::Int:
    format.append("%d");
    args.push_back(val);
    break;
  case simit::ir::ScalarType::Float:
    format.append("%f");
    args.push_back(val);
    break;
  case simit::ir::ScalarType::Boolean:
    format.append("%d");
    args.push_back(val);
    break;
  default:
    unreachable << "Unknown ScalarType";
  }
}

void LLVMBackend::visit(const Print *op) {

  llvm::Value *result = compile(op->expr);
  Type type = op->expr.type();

  switch (type.kind()) {
  case Type::Kind::Tensor: {
    const TensorType *tensor = type.toTensor();
    ScalarType scalarType = tensor->componentType;
    size_t order = tensor->order();
    std::string format;
    std::vector<llvm::Value*> args;

    if (order == 0) {
      printStmtAddScalarArg(format, args, scalarType, result);
      format.append("\n");
    } else if (order == 1) {
      not_supported_yet;
    } else {
      not_supported_yet;
    }

    emitPrintf(format, args);
  }
    break;
  case Type::Kind::Element:
  case Type::Kind::Set:
  case Type::Kind::Tuple:
    not_supported_yet;
  default:
    unreachable << "Unknown Type";
  }
}

llvm::Value *LLVMBackend::emitFieldRead(const Expr &elemOrSet,
                                        std::string fieldName) {
  assert(elemOrSet.type().isElement() || elemOrSet.type().isSet());
  const ElementType *elemType = nullptr;
  int fieldsOffset = -1;
  if (elemOrSet.type().isElement()) {
    elemType = elemOrSet.type().toElement();
    fieldsOffset = 0;
  }
  else {
    const SetType *setType = elemOrSet.type().toSet();
    elemType = setType->elementType.toElement();
    fieldsOffset = 1; // jump over set size
    if (setType->endpointSets.size() > 0) {
      fieldsOffset += NUM_EDGE_INDEX_ELEMENTS; // jump over index pointers
    }
  }
  assert(fieldsOffset >= 0);

  llvm::Value *setOrElemValue = compile(elemOrSet);

  assert(elemType->hasField(fieldName));
  unsigned fieldLoc = fieldsOffset + elemType->fieldNames.at(fieldName);
  return builder->CreateExtractValue(setOrElemValue, {fieldLoc},
                                     setOrElemValue->getName()+"."+fieldName);
}

llvm::Value *LLVMBackend::emitComputeLen(const ir::TensorType *tensorType,
                                         const TensorStorage &tensorStorage) {
  if (tensorType->order() == 0) {
    return llvmInt(1);
  }

  llvm::Value *len = nullptr;
  switch (tensorStorage.getKind()) {
    case TensorStorage::DenseRowMajor: {
      auto it = tensorType->dimensions.begin();
      len = emitComputeLen(*it++);
      for (; it != tensorType->dimensions.end(); ++it) {
        len = builder->CreateMul(len, emitComputeLen(*it));
      }
      break;
    }
    case TensorStorage::SystemReduced: {
      llvm::Value *targetSet = compile(tensorStorage.getSystemTargetSet());
      llvm::Value *storageSet = compile(tensorStorage.getSystemStorageSet());

      // Retrieve the size of the neighbor index, which is stored in the last
      // element of neighbor start index.
      llvm::Value *setSize =
          builder->CreateExtractValue(storageSet, {0},
                                      storageSet->getName()+LEN_SUFFIX);
      llvm::Value *neighborStartIndex =
          builder->CreateExtractValue(targetSet, {2}, "neighbors.start");
      llvm::Value *neighborIndexSizeLoc =
          builder->CreateInBoundsGEP(neighborStartIndex, setSize,
                                     "neighbors"+LEN_SUFFIX+PTR_SUFFIX);
      len = builder->CreateAlignedLoad(neighborIndexSizeLoc, 8,
                                       "neighbors"+LEN_SUFFIX);

      // Multiply by block size
      Type blockType = tensorType->blockType();
      if (!isScalar(blockType)) {
        // TODO: The following assumes all blocks are dense row major. The right
        //       way to assign a storage order for every block in the tensor
        //       represented by a TensorStorage
        llvm::Value *blockSize =
            emitComputeLen(blockType.toTensor(), TensorStorage::DenseRowMajor);
        len = builder->CreateMul(len, blockSize);
      }
      break;
    }
    case TensorStorage::SystemUnreduced: {
      not_supported_yet;
      break;
    }
    case TensorStorage::SystemNone:
      ierror << "Attempting to compute size of tensor without storage";
      break;
    case TensorStorage::Undefined:
      ierror << "Attempting to compute size of tensor with undefined storage";
      break;
  }
  iassert(len != nullptr);
  return len;
}

llvm::Value *LLVMBackend::emitComputeLen(const ir::IndexDomain &dom) {
  assert(dom.getIndexSets().size() > 0);

  auto it = dom.getIndexSets().begin();
  llvm::Value *result = emitComputeLen(*it++);
  for (; it != dom.getIndexSets().end(); ++it) {
    result = builder->CreateMul(result, emitComputeLen(*it));
  }
  return result;
}

llvm::Value *LLVMBackend::emitComputeLen(const ir::IndexSet &is) {
  switch (is.getKind()) {
    case IndexSet::Range:
      return llvmInt(is.getSize());
      break;
    case IndexSet::Set: {
      llvm::Value *setValue = compile(is.getSet());
      return builder->CreateExtractValue(setValue, {0},
                                         setValue->getName()+LEN_SUFFIX);
    }
    case IndexSet::Dynamic:
      not_supported_yet;
      break;
  }
  unreachable;
  return nullptr;
}

llvm::Value *LLVMBackend::loadFromArray(llvm::Value *array, llvm::Value *index){
  llvm::Value *loc = builder->CreateGEP(array, index);
  return builder->CreateLoad(loc);
}

llvm::Value *LLVMBackend::emitCall(string name,
                                   initializer_list<llvm::Value*> args,
                                   llvm::Type *returnType) {
  return emitCall(name, vector<llvm::Value*>(args), returnType);
}

llvm::Value *LLVMBackend::emitCall(std::string name,
                                   std::vector<llvm::Value*> args,
                                   llvm::Type *returnType) {
  std::vector<llvm::Type*> argTypes;
  for (auto &arg : args) {
    argTypes.push_back(arg->getType());
  }

  llvm::FunctionType *ftype =
      llvm::FunctionType::get(returnType, argTypes, false);

  llvm::Function *fun =
      llvm::cast<llvm::Function>(module->getOrInsertFunction(name, ftype));
  iassert(fun != nullptr)
      << "could not find" << fun << "with the given signature";

  return builder->CreateCall(fun, std::vector<llvm::Value*>(args));
}

llvm::Function *LLVMBackend::emitEmptyFunction(const string &name,
                                               const vector<ir::Var> &arguments,
                                               const vector<ir::Var> &results) {
  llvm::Function *llvmFunc = createPrototype(name, arguments, results, module);
  auto entry = llvm::BasicBlock::Create(LLVM_CONTEXT, "entry", llvmFunc);
  builder->SetInsertPoint(entry);

  // Add arguments and results to the symbol table
  for (auto &arg : llvmFunc->getArgumentList()) {
    symtable.insert(arg.getName(), &arg);
  }

  return llvmFunc;
}

void LLVMBackend::emitPrintf(std::string format,
                             std::vector<llvm::Value*> args) {
  auto int32Type = llvm::IntegerType::getInt32Ty(LLVM_CONTEXT);
  llvm::Function *printfFunc = module->getFunction("printf");
  if (printfFunc == nullptr) {
    std::vector<llvm::Type*> printfArgTypes;
    printfArgTypes.push_back(llvm::Type::getInt8PtrTy(LLVM_CONTEXT));
    llvm::FunctionType* printfType = llvm::FunctionType::get(int32Type,
                                                             printfArgTypes,
                                                             true);
    printfFunc = llvm::Function::Create(printfType,
                                        llvm::Function::ExternalLinkage,
                                        llvm::Twine("printf"), module);
    printfFunc->setCallingConv(llvm::CallingConv::C);
  }

  auto formatValue = llvm::ConstantDataArray::getString(LLVM_CONTEXT, format);

  auto intType = llvm::IntegerType::get(LLVM_CONTEXT,8);
  auto formatStrType = llvm::ArrayType::get(intType, format.size()+1);

  llvm::GlobalVariable *formatStr =
      new llvm::GlobalVariable(*module, formatStrType, true,
                               llvm::GlobalValue::PrivateLinkage, formatValue,
                               ".str");
  llvm::Constant *zero = llvm::Constant::getNullValue(int32Type);

  std::vector<llvm::Constant*> idx;
  idx.push_back(zero);
  idx.push_back(zero);
  llvm::Constant *str = llvm::ConstantExpr::getGetElementPtr(formatStr, idx);

  std::vector<llvm::Value*> printfArgs;
  printfArgs.push_back(str);
  printfArgs.insert(printfArgs.end(), args.begin(), args.end());

  builder->CreateCall(printfFunc, printfArgs);
}

void LLVMBackend::emitFirstAssign(const std::string& varName,
                                  const ir::Expr& value) {
  ScalarType type = value.type().toTensor()->componentType;
  llvm::Value *var = builder->CreateAlloca(createLLVMType(type));
  symtable.insert(varName, var);
}

void LLVMBackend::emitAssign(Var var, const ir::Expr& value) {
  /// \todo assignment of scalars to tensors and tensors to tensors should be
  ///       handled by the lowering so that we only assign scalars to scalars
  ///       in the backend. Probably requires copy and memset intrinsics.
//  iassert(isScalar(value.type()) &&
//         "assignment non-scalars should have been lowered by now");
  iassert(var.getType().isTensor() && value.type().isTensor());

  std::string varName = var.getName();
  llvm::Value *llvmValue = compile(value);

  // Assigned for the first time
  if (!symtable.contains(varName)) {
    emitFirstAssign(varName, value);
  }
  iassert(symtable.contains(varName));

  llvm::Value *varPtr = symtable.get(varName);
  iassert(varPtr->getType()->isPointerTy());

  const TensorType *varType = var.getType().toTensor();
  const TensorType *valType = value.type().toTensor();

  // Assigning a scalar to a scalar
  if (varType->order() == 0 && valType->order() == 0) {
    builder->CreateStore(llvmValue, varPtr);
    llvmValue->setName(varName + VAL_SUFFIX);
  }
  // Assigning a scalar to an n-order tensor
  else if (varType->order() > 0 && valType->order() == 0) {
    if (isa<Literal>(value) &&
        ((double*)to<Literal>(value)->data)[0] == 0.0) {
      // emit memset 0
      llvm::Value *varLen = emitComputeLen(varType, storage.get(var));
      unsigned compSize = varType->componentType.bytes();
      llvm::Value *varSize = builder->CreateMul(varLen, llvmInt(compSize));
      builder->CreateMemSet(varPtr, llvmInt(0,8), varSize, compSize);
    }
    else {
      not_supported_yet;
    }
  }
  else if (isa<Literal>(value)) {
    llvmValue->setName(varName);
    symtable.insert(varName, llvmValue);
  }
  else {
    ierror;
  }
}

}}  // namespace simit::internal
