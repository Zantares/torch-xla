#ifndef XLA_TORCH_XLA_CSRC_IR_H_
#define XLA_TORCH_XLA_CSRC_IR_H_

#include <ATen/core/interned_strings.h>
#include <torch/csrc/lazy/core/hash.h>
#include <torch/csrc/lazy/core/ir.h>
#include <torch/csrc/lazy/core/ir_builder.h>
#include <torch/csrc/lazy/core/ir_metadata.h>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "absl/hash/hash.h"
#include "absl/types/span.h"
#include "torch_xla/csrc/runtime/types.h"
#include "xla/client/xla_builder.h"

namespace torch_xla {

static const uint32_t default_hash_seed = (uint32_t)0x5a2d296e9;

class XlaNode;
class LoweringContext;

using XlaOpVector = absl::InlinedVector<xla::XlaOp, 1>;

template <typename T>
using OutputMap =
    std::unordered_map<torch::lazy::Output, T, torch::lazy::Output::Hasher>;

template <typename T, typename... Args>
torch::lazy::NodePtr MakeNode(Args&&... args) {
  torch::lazy::NodePtr res = std::make_shared<T>(std::forward<Args>(args)...);
  return res;
}

// A node in the graph. Nodes for operations which requires extra data to be
// stored for lowering, should inherit from this class and add operation
// specific member there. For example, a constant might create a new
// NodeConstant class (inheriting from XlaNode) with an extra xla::Literal
// field, or a tensor value might create a new NodeTensor with computation
// client data handle in it.
class XlaNode : public torch::lazy::Node {
 public:
  // Creates a new node with the given op name. The op is a unique identifier
  // for the operation. The num_outputs tells how many outputs a given operation
  // generates.
  XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
          std::vector<torch::lazy::Shape>&& shapes, xla::Shape xla_shape,
          size_t num_outputs = 1,
          torch::lazy::hash_t hash_seed = default_hash_seed);

  XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
          std::vector<torch::lazy::Shape>&& shapes,
          const std::function<xla::Shape()>& xla_shape_fn,
          size_t num_outputs = 1,
          torch::lazy::hash_t hash_seed = default_hash_seed);

  XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
          torch::lazy::Shape shape, xla::Shape xla_shape,
          size_t num_outputs = 1,
          torch::lazy::hash_t hash_seed = default_hash_seed);

  // Legacy constructor that does not handle torch::lazy::shape.
  XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
          xla::Shape shape, size_t num_outputs = 1,
          torch::lazy::hash_t hash_seed = default_hash_seed);

  // Same as the constructor above, but the shape is generated by a function,
  // only if needed (shape cache miss).
  XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
          const std::function<torch::lazy::Shape()>& shape_fn,
          const std::function<xla::Shape()>& xla_shape_fn,
          size_t num_outputs = 1,
          torch::lazy::hash_t hash_seed = default_hash_seed);

  // Legacy constructor that does not handle torch::lazy::shape.
  XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
          const std::function<xla::Shape()>& xla_shape_fn,
          size_t num_outputs = 1,
          torch::lazy::hash_t hash_seed = default_hash_seed);

  // Contructor used to create leaf nodes.
  XlaNode(torch::lazy::OpKind op, torch::lazy::Shape shape,
          xla::Shape xla_shape, size_t num_outputs,
          torch::lazy::hash_t hash_seed);

  // Legacy constructor that does not handle torch::lazy::shape.
  XlaNode(torch::lazy::OpKind op, xla::Shape xla_shape, size_t num_outputs,
          torch::lazy::hash_t hash_seed);

  virtual ~XlaNode();

  // Retrieves the full shape of the IR XlaNode. Note that if this is a
  // multi-output node, the returned shape will be a tuple.
  const xla::Shape& xla_shape() const { return xla_shape_; }

  // Retrieves the shape of the output at a given index. If the node is not a
  // multi-output node, output_index must be zero.
  const xla::Shape& xla_shape(size_t output_index) const;

  virtual torch::lazy::NodePtr Clone(torch::lazy::OpList operands) const;

  virtual XlaOpVector Lower(LoweringContext* loctx) const;

  XlaOpVector ReturnOp(xla::XlaOp op, LoweringContext* loctx) const;

  XlaOpVector ReturnOps(absl::Span<const xla::XlaOp> ops,
                        LoweringContext* loctx) const;

  torch::lazy::hash_t node_hash() const { return node_hash_; }

  torch::lazy::hash_t hash() const override {
    if (sharding_hash_ != 0) {
      return torch::lazy::HashCombine(dag_hash_, sharding_hash_);
    }
    return dag_hash_;
  }

  torch::lazy::hash_t shapeHash() const override { return dag_hash_; }

  torch::lazy::hash_t shardingHash() const { return sharding_hash_; }

  // The node's outputs get assigned the same HLO sharding
  const std::shared_ptr<xla::OpSharding> GetSharding(size_t index) const {
    if (output_shardings_.size() == 0) {
      return nullptr;
    }
    return output_shardings_[index];
  }

  void SetSharding(const xla::OpSharding& sharding, size_t index);

  void ClearSharding() {
    output_shardings_.clear();
    sharding_hash_ = 0;
  }

  std::string ToString() const override;

  void MarkDynamicDimension(uint32_t dim) {
    unbounded_dynamic_dims_.insert(dim);
  }

  const std::unordered_set<uint32_t>& dynamic_dims() const {
    return unbounded_dynamic_dims_;
  }

  std::shared_ptr<torch::lazy::UserMetaData> SetUserMetadataForSubGraph(
      std::shared_ptr<torch::lazy::UserMetaData> user_meta);

 protected:
  std::unordered_set<uint32_t> unbounded_dynamic_dims_;

 private:
  xla::Shape GetOpShape(const std::function<xla::Shape()>& shape_fn) const;

  static torch::lazy::hash_t GetOpHash(torch::lazy::OpKind op,
                                       const xla::Shape& shape,
                                       torch::lazy::hash_t hash_seed);

  static std::vector<torch::lazy::SourceLocation> GetFrameInfo();

  void UpdateShardingHash();

  xla::Shape xla_shape_;
  torch::lazy::hash_t node_hash_ = 0;
  torch::lazy::hash_t dag_hash_;
  torch::lazy::hash_t sharding_hash_ = 0;

  // Experimental sharding annotations attached to the IR node.
  std::vector<std::shared_ptr<xla::OpSharding>> output_shardings_;
};

inline std::ostream& operator<<(std::ostream& stream, const XlaNode& node) {
  stream << node.ToString();
  return stream;
}

const xla::Shape& GetXlaShape(const torch::lazy::Value& value);

template <typename T>
T* NodeCast(const torch::lazy::Node* node, torch::lazy::OpKind op) {
  if (op != node->op()) {
    return nullptr;
  }
  const T* casted;
#ifdef NDEBUG
  casted = static_cast<const T*>(node);
#else
  casted = &dynamic_cast<const T&>(*node);
#endif
  return const_cast<T*>(casted);
}

struct CustomOpNameMetaData : public torch::lazy::UserMetaData {
  CustomOpNameMetaData(const std::string& input_op_name_prefix,
                       int input_max_stack_depth)
      : op_name_prefix(input_op_name_prefix),
        max_stack_depth(input_max_stack_depth) {}
  std::string op_name_prefix;
  size_t max_stack_depth;
};

}  // namespace torch_xla

#endif  // XLA_TORCH_XLA_CSRC_IR_H_
