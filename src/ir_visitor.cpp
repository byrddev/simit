#include "ir_visitor.h"

#include "ir.h"

namespace simit {
namespace ir {

// class IRVisitor
IRVisitor::~IRVisitor() {
}

void IRVisitor::visit(const Function *op) {
  for (auto &argument : op->getArguments()) {
    argument.accept(this);
  }
  for (auto &result : op->getResults()) {
    result.accept(this);
  }
  op->getBody().accept(this);
}

void IRVisitor::visit(const Literal *op) {
}

void IRVisitor::visit(const Variable *op) {
}

void IRVisitor::visit(const Result *op) {
  op->producer.accept(this);
}

void IRVisitor::visit(const FieldRead *op) {
  op->elementOrSet.accept(this);
}

void IRVisitor::visit(const TensorRead *op) {
  op->tensor.accept(this);
  for (auto &index : op->indices) {
    index.accept(this);
  }
}

void IRVisitor::visit(const TupleRead *op) {
  op->tuple.accept(this);
  op->index.accept(this);
}

void IRVisitor::visit(const Map *op) {
  op->target.accept(this);
  op->neighbors.accept(this);
}

void IRVisitor::visit(const IndexedTensor *op) {
  op->tensor.accept(this);
}

void IRVisitor::visit(const IndexExpr *op) {
  op->expr.accept(this);
}

void IRVisitor::visit(const Call *op) {
  // TODO
}

void IRVisitor::visit(const Neg *op) {
  op->a.accept(this);
}

void IRVisitor::visit(const Add *op) {
  op->a.accept(this);
  op->b.accept(this);
}

void IRVisitor::visit(const Sub *op) {
  op->a.accept(this);
  op->b.accept(this);
}

void IRVisitor::visit(const Mul *op) {
  op->a.accept(this);
  op->b.accept(this);
}

void IRVisitor::visit(const Div *op) {
  op->a.accept(this);
  op->b.accept(this);
}

void IRVisitor::visit(const AssignStmt *op) {
  op->rhs.accept(this);
}

void IRVisitor::visit(const FieldWrite *op) {
  op->elementOrSet.accept(this);
  op->value.accept(this);
}

void IRVisitor::visit(const TensorWrite *op) {
  op->tensor.accept(this);
  for (auto &index : op->indices) {
    index.accept(this);
  }
  op->value.accept(this);
}

void IRVisitor::visit(const For *op) {
  op->body.accept(this);
}

void IRVisitor::visit(const IfThenElse *op) {
  op->condition.accept(this);
  op->thenBody.accept(this);
  op->elseBody.accept(this);
}

void IRVisitor::visit(const Block *op) {
  op->first.accept(this);
  op->rest.accept(this);
}

void IRVisitor::visit(const Pass *op) {
}

}} // namespace simit::ir
