// License TODO ....
#include "tensorflow/compiler/xla/service/intermediate_tensor_splitter.h"

#include "absl/algorithm/container.h"
#include "tensorflow/compiler/xla/service/dfs_hlo_visitor_with_default.h"
#include "tensorflow/compiler/xla/service/hlo_creation_utils.h"
#include "tensorflow/compiler/xla/service/pattern_matcher.h"
#include "tensorflow/compiler/xla/status_macros.h"

namespace xla {

namespace {

namespace m = match;

class IntermediateTensorSplitterVisitor : public DfsHloRewriteVisitor {
  int64 max_intermediate_size;
  int64 target_intermediate_size;
  HloModule* parent_module;

 public:
  explicit IntermediateTensorSplitterVisitor(int64 max_intermediate_size,
                                             int64 target_intermediate_size,
                                             HloModule* parent_module)
      : max_intermediate_size(max_intermediate_size),
        target_intermediate_size(target_intermediate_size),
        parent_module(parent_module) {}

  // Determine if an operand is large enough such that we are
  // interested in splitting it.
  bool OperandShouldBeSplit(HloInstruction* inst);

  // Determine if an operand can be split by traversing it's
  // inputs until a splittable node is found.
  bool OperandCanBeSplit(HloInstruction* inst);

  // Matches any pointwise unary operator which has no side effects.
  bool MatchPointwiseUnary(HloInstruction* inst,
                           HloInstruction** operand = nullptr);

  // Determine the best dimesion to split on, excluding a given one.
  int64 BestSplitDim(HloInstruction* inst, absl::Span<const int64> excluded);

  // Given a split dimension, determine the best possible split
  // size. If no split size is possible, returns -1.
  int64 BestSplitSize(HloInstruction* inst, int64 split_dim);

  // Collect computation for the instruction we want to split
  // and split the parameters. The parameters are returned pre-
  // split such that they can be used verbatim inside a call.
  // The returned instruction is the root instruction of the
  // computation.
  StatusOr<HloInstruction*> BuildComputationAndParameters(
      HloInstruction* inst, int64 split_dim, int64 split_size,
      HloComputation::Builder* builder,
      std::vector<std::vector<HloInstruction*>>* parameters);

  Status HandleDot(HloInstruction* dot) override;
};

}  // namespace

bool IntermediateTensorSplitterVisitor::OperandShouldBeSplit(
    HloInstruction* inst) {
  return ShapeUtil::ElementsIn(inst->shape()) > max_intermediate_size;
}

bool IntermediateTensorSplitterVisitor::MatchPointwiseUnary(
    HloInstruction* inst, HloInstruction** operand) {
  if (inst->IsElementwise() && !inst->HasSideEffect() &&
      inst->operand_count() == 1) {
    if (operand != nullptr) {
      *operand = inst->mutable_operand(0);
    }
    return true;
  } else {
    return false;
  }
}

bool IntermediateTensorSplitterVisitor::OperandCanBeSplit(
    HloInstruction* inst) {
  HloInstruction* next;
  if (Match(inst, m::Dot(m::Op(), m::Op()))) {
    // Base case: A Dot produces this large intermediate tensor
    // TODO: Support more cases (most importantly broadcasts..)
    return true;
  } else if (MatchPointwiseUnary(inst, &next)) {
    return OperandCanBeSplit(next);
  } else if (Match(inst, m::Transpose(m::Op(&next)))) {
    return OperandCanBeSplit(next);
  } else {
    return false;
  }
}

int64 IntermediateTensorSplitterVisitor::BestSplitDim(
    HloInstruction* inst, absl::Span<const int64> excluded) {
  const Shape& shape = inst->shape();
  int64 best_dim = -1, best_size = 0;
  for (int64 i = 0; i < shape.dimensions_size(); i++) {
    if (absl::c_linear_search(excluded, i)) continue;
    if (shape.dimensions(i) > best_size && BestSplitSize(inst, i) != -1) {
      best_size = shape.dimensions(i);
      best_dim = i;
    }
  }
  return best_dim;
}

const int64 primes[64] = {2,   3,   5,   7,   11,  13,  17,  19,  23,  29,  31,
                          37,  41,  43,  47,  53,  59,  61,  67,  71,  73,  79,
                          83,  89,  97,  101, 103, 107, 109, 113, 127, 131, 137,
                          139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193,
                          197, 199, 211, 223, 227, 229, 233, 239, 241, 251, 257,
                          263, 269, 271, 277, 281, 283, 293, 307, 311};

int64 IntermediateTensorSplitterVisitor::BestSplitSize(HloInstruction* inst,
                                                       int64 split_dim) {
  int64 split_size = inst->shape().dimensions(split_dim);
  int64 rest_size = ShapeUtil::ElementsIn(inst->shape()) / split_size;
  int64 factors[64];

  int64 tmp_size = split_size;
  for (int i = 0; i < 64; i++) {
    factors[i] = 0;
    while (tmp_size % primes[i] == 0) {
      factors[i] ++;
      tmp_size /= primes[i];
    }
  }

  for (int i = 0; i < 64; i++)
    while (split_size * rest_size > target_intermediate_size && factors[i]-- > 0)
      split_size /= primes[i];

  return split_size <= max_intermediate_size ? split_size : -1;
}

StatusOr<HloInstruction*>
IntermediateTensorSplitterVisitor::BuildComputationAndParameters(
    HloInstruction* inst, int64 split_dim, int64 split_size,
    HloComputation::Builder* builder,
    std::vector<std::vector<HloInstruction*>>* parameters) {
  HloInstruction *operand, *lhs, *rhs;

  if (Match(inst, m::Dot(m::Op(&lhs), m::Op(&rhs)))) {
    // For the dot we identify the parameter to split and then
    // Generate the final dot operation, as well as the operand
    // vector.
    Shape dot_shape = ShapeUtil::MakeShape(inst->shape().element_type(),
                                           inst->shape().dimensions());
    dot_shape.set_dimensions(split_dim, split_size);

    auto& dnums = inst->dot_dimension_numbers();
    int64 dims_lhs =
        lhs->shape().rank() - dnums.lhs_contracting_dimensions_size();

    HloInstruction *split_op, *join_op;
    bool split_is_lhs;
    if (split_dim < dims_lhs) {
      // We are splitting up the lhs
      split_is_lhs = true;
      split_op = lhs;
      join_op = rhs;
    } else {
      // We are splitting up the rhs
      split_is_lhs = false;
      split_dim -= dims_lhs;
      split_op = rhs;
      join_op = lhs;
    }

    // adjust split dim up for every contraction dimension to it's left
    // TODO: Check if this is robust for multiple indices ...
    for (int64 i = 0; i < dnums.lhs_contracting_dimensions_size(); i++) {
      if (split_dim >= dnums.lhs_contracting_dimensions(i)) split_dim += 1;
    }

    // generate parameters for each split
    int64 dims_done = 0;
    int64 split_parameter_idx, join_parameter_idx;

    Shape split_shape = ShapeUtil::MakeShape(split_op->shape().element_type(),
                                             split_op->shape().dimensions());
    split_shape.set_dimensions(split_dim, split_size);

    std::vector<int64> start, limit, stride;
    for (int64 dim = 0; dim < split_op->shape().dimensions_size(); dim++) {
      start.push_back(0);
      limit.push_back(split_op->shape().dimensions(dim));
      stride.push_back(1);
    }

    for (int64 i = 0; dims_done < split_op->shape().dimensions(split_dim);
         i++, dims_done += split_size) {
      // build split parameter
      split_parameter_idx = parameters->at(i).size();
      start[split_dim] = dims_done;
      limit[split_dim] = dims_done + split_size;
      HloInstruction* slice =
          split_op->parent()->AddInstruction(HloInstruction::CreateSlice(
              split_shape, split_op, absl::MakeSpan(start),
              absl::MakeSpan(limit), absl::MakeSpan(stride)));
      parameters->at(i).push_back(slice);
      // attach join parameter
      join_parameter_idx = parameters->at(i).size();
      parameters->at(i).push_back(join_op);
    }

    // build the final dot
    HloInstruction* split_param =
        builder->AddInstruction(HloInstruction::CreateParameter(
            split_parameter_idx, split_shape, "dot_split_tensor"));
    HloInstruction* join_param =
        builder->AddInstruction(HloInstruction::CreateParameter(
            join_parameter_idx, join_op->shape(), "dot_join_tensor"));

    std::vector<HloInstruction*> ops;
    if (split_is_lhs) {
      ops = {split_param, join_param};
    } else {
      ops = {join_param, split_param};
    }
    return builder->AddInstruction(
        inst->CloneWithNewOperands(dot_shape, absl::MakeSpan(ops)));
  } else if (MatchPointwiseUnary(inst, &operand)) {
    // For a unary operation recursively obtain a new operand and
    // clone the operation.
    TF_ASSIGN_OR_RETURN(
        HloInstruction * new_operand,
        BuildComputationAndParameters(operand, split_dim, split_size, builder,
                                      parameters));
    std::vector<HloInstruction*> ops = {new_operand};
    return builder->AddInstruction(
        inst->CloneWithNewOperands(new_operand->shape(), absl::MakeSpan(ops)));
  } else if (Match(inst, m::Transpose(m::Op(&operand)))) {
    // For a transpose, the transpose might change which dimension is
    // being split. So we obtain the new split dimension and then
    // recursively a new operand to make a clone.
    int64 operand_split_dim = inst->dimensions(split_dim);
    TF_ASSIGN_OR_RETURN(
        HloInstruction * new_operand,
        BuildComputationAndParameters(operand, operand_split_dim, split_size,
                                      builder, parameters));
    std::vector<HloInstruction*> ops = {new_operand};
    return builder->AddInstruction(
        inst->CloneWithNewOperands(new_operand->shape(), absl::MakeSpan(ops)));
  } else {
    // Invariant violation
    // TODO: Is there a more idiomatic way to return a bad status?
    CHECK(false);
  }
}

Status IntermediateTensorSplitterVisitor::HandleDot(HloInstruction* dot) {
  HloInstruction *lhs, *rhs;
  CHECK(Match(dot, m::Dot(m::Op(&lhs), m::Op(&rhs))));
  auto& dnums = dot->dot_dimension_numbers();

  // Check if this dot is large enough to be split
  bool can_split_lhs = OperandShouldBeSplit(lhs) && OperandCanBeSplit(lhs);
  bool can_split_rhs = OperandShouldBeSplit(rhs) && OperandCanBeSplit(rhs);
  if (can_split_lhs || can_split_rhs) {
    LOG(INFO) << "Will attempt to split dot operand"; // FIXME: Remove.

    bool split_is_lhs = can_split_lhs;  // TODO: Is there a reason to prefer one or the other given the choice?
    HloInstruction* split_inst = split_is_lhs ? lhs : rhs;
    int64 split_dim = BestSplitDim(
        split_inst,
        absl::MakeSpan(split_is_lhs ? dnums.lhs_contracting_dimensions()
                                    : dnums.rhs_contracting_dimensions()));
    if (split_dim == -1) {
      // Bail, we can't split this tensor into equally sized parts.
      return Status::OK();
    }

    HloComputation::Builder builder("intermediate_split_tensor_computation");
    std::vector<std::vector<HloInstruction*>> parameters;

    int64 full_size = 0;
    int64 split_size = BestSplitSize(split_inst, split_dim);
    CHECK(split_size != -1);

    while (full_size < split_inst->shape().dimensions(split_dim)) {
      parameters.push_back({});
      full_size += split_size;
    }
    CHECK(full_size = split_inst->shape().dimensions(split_dim));

    TF_ASSIGN_OR_RETURN(
        HloInstruction * comp_root,
        BuildComputationAndParameters(split_inst, split_dim, split_size,
                                      &builder, &parameters));
    HloComputation* comp =
        parent_module->AddEmbeddedComputation(builder.Build(comp_root));

    Shape part_shape = ShapeUtil::MakeShape(dot->shape().element_type(),
                                            dot->shape().dimensions());
    int64 dot_split_dim = split_dim;  // split dimension after dot occured
    if (split_is_lhs) {
      for (int64 c_dim : dnums.lhs_contracting_dimensions()) {
        if (c_dim < dot_split_dim) dot_split_dim--;
      }
    } else {
      for (int64 c_dim : dnums.rhs_contracting_dimensions()) {
        if (c_dim < dot_split_dim) dot_split_dim--;
      }
      dot_split_dim +=
          lhs->shape().rank() - dnums.lhs_contracting_dimensions_size();
    }
    part_shape.set_dimensions(split_dim, split_size);

    std::vector<HloInstruction*> parts;
    for (auto operands : parameters) {
      HloInstruction* call =
          dot->parent()->AddInstruction(HloInstruction::CreateCall(
              comp_root->shape(), absl::MakeSpan(operands), comp));
      std::vector<HloInstruction*> ops;
      if (split_is_lhs) {
        ops = {call, rhs};
      } else {
        ops = {lhs, call};
      }
      HloInstruction* part = dot->parent()->AddInstruction(
          dot->CloneWithNewOperands(part_shape, absl::MakeSpan(ops)));
      parts.push_back(part);
    }

    HloInstruction* concat =
        dot->parent()->AddInstruction(HloInstruction::CreateConcatenate(
            dot->shape(), absl::MakeSpan(parts), dot_split_dim));
    return ReplaceInstruction(dot, concat);
  }
}

StatusOr<bool> IntermediateTensorSplitter::Run(HloModule* module) {
  // TODO: Make the size limit configurable + find a better default
  int64 max_size = 1000 * 1000;
  int64 target_size = 1000 * 200;
  IntermediateTensorSplitterVisitor visitor(max_size, target_size, module);
  return visitor.RunOnModule(module);
}

}  // namespace xla
