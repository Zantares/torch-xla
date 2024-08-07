#include "torch_xla/csrc/ops/all_reduce.h"

#include <torch/csrc/lazy/core/util.h>

#include "absl/strings/str_join.h"
#include "torch_xla/csrc/lowering_context.h"
#include "torch_xla/csrc/ops/xla_ops.h"
#include "torch_xla/csrc/runtime/util.h"
#include "xla/shape_util.h"

namespace torch_xla {
namespace {

xla::Shape NodeOutputShape(c10::ArrayRef<torch::lazy::Value> operands,
                           const torch::lazy::Value& token) {
  std::vector<xla::Shape> tuple_shapes;
  tuple_shapes.reserve(operands.size() + 1);
  for (auto& operand : operands) {
    tuple_shapes.push_back(GetXlaShape(operand));
  }
  tuple_shapes.push_back(GetXlaShape(token));
  return xla::ShapeUtil::MakeTupleShape(tuple_shapes);
}

}  // namespace

AllReduce::AllReduce(AllReduceType reduce_type,
                     c10::ArrayRef<torch::lazy::Value> operands,
                     const torch::lazy::Value& token, double scale,
                     std::vector<std::vector<int64_t>> groups, bool pin_layout)
    : XlaNode(
          xla_cross_replica_sum, GetOperandListWithToken(operands, token),
          [&]() { return NodeOutputShape(operands, token); },
          /*num_outputs=*/operands.size() + 1,
          torch::lazy::MHash(torch::lazy::GetEnumValue(reduce_type), scale,
                             groups, pin_layout)),
      reduce_type_(reduce_type),
      scale_(scale),
      groups_(std::move(groups)),
      pin_layout_(pin_layout) {}

AllReduce::AllReduce(AllReduceType reduce_type, torch::lazy::Value operand,
                     double scale, std::vector<std::vector<int64_t>> groups)
    : XlaNode(xla_cross_replica_sum, {operand}, GetXlaShape(operand),
              /*num_outputs=*/1,
              torch::lazy::MHash(torch::lazy::GetEnumValue(reduce_type), scale,
                                 groups)),
      reduce_type_(reduce_type),
      scale_(scale),
      groups_(std::move(groups)),
      pin_layout_(false),
      has_token_(false) {}

torch::lazy::NodePtr AllReduce::Clone(torch::lazy::OpList operands) const {
  std::vector<torch::lazy::Value> operand_list(operands.begin(),
                                               operands.end() - 1);
  return torch_xla::MakeNode<AllReduce>(reduce_type_, operand_list,
                                        operands.back(), scale_, groups_,
                                        pin_layout_);
}

XlaOpVector AllReduce::Lower(LoweringContext* loctx) const {
  if (!has_token_) {
    auto result = BuildAllReduce(
        reduce_type_, loctx->GetOutputOp(operands()[0]), scale_, groups_);
    return ReturnOp(result, loctx);
  }

  auto& operand_list = operands();
  std::vector<xla::XlaOp> inputs;
  inputs.reserve(operand_list.size());
  for (size_t i = 0; i + 1 < operand_list.size(); ++i) {
    inputs.push_back(loctx->GetOutputOp(operand_list[i]));
  }
  xla::XlaOp token = loctx->GetOutputOp(operand_list.back());
  return ReturnOps(
      BuildAllReduce(reduce_type_, inputs, token, scale_, groups_, pin_layout_),
      loctx);
}

std::string AllReduce::ToString() const {
  std::stringstream ss;
  ss << XlaNode::ToString()
     << ", reduce_type=" << torch::lazy::GetEnumValue(reduce_type_)
     << ", scale=" << scale_ << ", pin_layout=" << pin_layout_ << ", groups=(";
  for (size_t i = 0; i < groups_.size(); ++i) {
    ss << (i == 0 ? "(" : ",(");
    ss << absl::StrJoin(groups_[i], ", ") << ")";
  }
  ss << ")";
  return ss.str();
}

}  // namespace torch_xla
