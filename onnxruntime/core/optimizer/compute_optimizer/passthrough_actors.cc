// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <onnx/defs/attr_proto_util.h>

#include "core/common/safeint.h"
#include "core/graph/graph_utils.h"
#include "core/optimizer/initializer.h"
#include "core/optimizer/utils.h"
#include "core/optimizer/compute_optimizer/passthrough_actors.h"
#include "core/optimizer/compute_optimizer/compute_optimizer.h"

using namespace ONNX_NAMESPACE;
using namespace ::onnxruntime::common;
using ONNXDimension = ONNX_NAMESPACE::TensorShapeProto_Dimension;
using TensorShapeProto = ONNX_NAMESPACE::TensorShapeProto;
namespace onnxruntime {
namespace optimizer {
namespace compute_optimizer {

enum class DimCompareRet {
  ExactEqual = 0,
  BroadcastableEqual = 1,
  RankTooLow = 2,
  NotEqual = 3,
  DimCompareRetMax = 4,
};

/**
 * @brief Check dimensions are equal or broadcastable before axis.
 *
 * @param full_broadcasted_shape Full brodcasted shape as a baseline to compare.
 * @param axis The axis (inclusive, of full_broadcasted_shape) where we end the comparison.
 * @param target_shape Shape to compare, can have dim value be 1 for broadcastable dimension.
 * @return A pair of bool, bool. The first bool is true if the dimensions are exactly same before and include axis.
 * The second bool is true if the dimension of target_shape has dim value be 1 on axis.
 */
std::pair<DimCompareRet, bool> AreDimsCompatibleBeforeAxisInternal(
    const TensorShapeProto* full_broadcasted_shape, const int axis,
    const TensorShapeProto* target_shape) {
  int full_rank = full_broadcasted_shape->dim_size();
  int target_rank = target_shape->dim_size();

  ORT_ENFORCE(full_rank >= axis && target_rank <= full_rank, "full_rank should bigger than axis and target_rank ",
              axis, " full_rank: ", full_rank, " target_rank: ", target_rank);

  int minimum_rank_to_handle = full_rank - axis;
  if (target_rank < minimum_rank_to_handle) {
    // Skip if target node's input rank is less than minimum rank to handle.
    // Encentially this means the input did not affect the Gather axis.
    // todo skip the adaptor for this case.
    return std::make_pair(DimCompareRet::RankTooLow, false);
  }

  bool exact_equal = true;
  bool broadcastable_equal = true;
  bool dim_be_1_on_axis = false;

  int axis_iter = axis;
  int negative_axis = axis < 0 ? axis : axis - full_rank;
  int target_axis_iter = target_rank + negative_axis;

  for (; axis_iter >= 0 && target_axis_iter >= 0; --axis_iter, --target_axis_iter) {
    auto& dim = full_broadcasted_shape->dim(axis_iter);
    auto& target_dim = target_shape->dim(target_axis_iter);
    if (dim.has_dim_value() && target_dim.has_dim_value()) {
      if (dim.dim_value() != target_dim.dim_value()) {
        exact_equal = false;
        if (target_dim.dim_value() == 1) {
          if (axis_iter == axis) dim_be_1_on_axis = true;
        } else {
          broadcastable_equal = false;
        }
      }
    } else if (dim.has_dim_param() && target_dim.has_dim_param()) {
      if (dim.dim_param() != target_dim.dim_param()) {
        exact_equal = false;
      }
    } else {
      exact_equal = false;
      if (target_dim.has_dim_value() && target_dim.dim_value() == 1) {
        if (axis_iter == axis) dim_be_1_on_axis = true;
      } else {
        broadcastable_equal = false;
      }
    }
  }

  if (exact_equal) {
    return std::make_pair(DimCompareRet::ExactEqual, dim_be_1_on_axis);
  } else if (broadcastable_equal) {
    return std::make_pair(DimCompareRet::BroadcastableEqual, dim_be_1_on_axis);
  } else {
    return std::make_pair(DimCompareRet::NotEqual, dim_be_1_on_axis);
  }
}

/**
 * @brief From given TensorShape, update specified dimension with given value.
 * If no new_dim is provided, the dimension will be removed.
 *
 * @param shape TensorShape used as base shape to modify.
 * @param axis The dimension to be replaced/removed.
 * @param new_dim The new dimension value. If not provided, the dimension will be removed.
 * @return TensorShapeProto A copy of "shape" after modification.
 */
TensorShapeProto UpdateDimValueInternal(const TensorShapeProto* shape, const int axis,
                                        ONNXDimension& new_dim) {
  ORT_ENFORCE(axis >= 0 && axis < shape->dim_size());
  TensorShapeProto output_shape;
  for (int i = 0; i < shape->dim_size(); ++i) {
    auto& dim = shape->dim(i);
    if (i == axis) {
      if (new_dim.has_dim_value()) {
        output_shape.add_dim()->set_dim_value(new_dim.dim_value());
      } else if (new_dim.has_dim_param()) {
        output_shape.add_dim()->set_dim_param(new_dim.dim_param());
      } else {
        // do nothing, unassigned dim will be removed.
      }

      continue;
    }

    if (dim.has_dim_value()) {
      output_shape.add_dim()->set_dim_value(dim.dim_value());
    } else if (dim.has_dim_param()) {
      output_shape.add_dim()->set_dim_param(dim.dim_param());
    } else {
      ORT_THROW("Invalid dim found in UpdateDimValueInternal");
    }
  }

  return output_shape;
}

bool UpdateSliceOutputShape(NodeArg& arg_to_update, int reverse_axis, ONNXDimension& output_dim_on_axis) {
  ORT_ENFORCE(reverse_axis < 0, " reverse_axis should be negative, representing the index from right to left.");
  const TensorShapeProto* shape = arg_to_update.Shape();
  int rank = shape->dim_size();
  if (rank < -reverse_axis) {
    return false;
  }

  int axis_to_update = rank + reverse_axis;
  TensorShapeProto new_output_shape = UpdateDimValueInternal(shape, axis_to_update, output_dim_on_axis);
  arg_to_update.SetShape(new_output_shape);
  return true;
}

Node* InsertItermediateNodeOnDestInput(Graph& graph,
                                       Node& dest_node, int dest_in_index,
                                       int new_node_input_index,
                                       int new_node_output_index,
                                       const std::string& name, const std::string& op_type,
                                       const std::string& description,
                                       const InlinedVector<NodeArg*>& input_args,
                                       const InlinedVector<NodeArg*>& output_args,
                                       const onnxruntime::NodeAttributes& attributes,
                                       const std::string& domain,
                                       const std::string& /*entry_node_output_arg_name*/,
                                       const logging::Logger& logger) {
  LOGS(logger, WARNING) << "Inserting " << op_type << " node on " << dest_node.Name()
                        << " 's " << dest_in_index << "th input " << dest_node.InputDefs()[dest_in_index]->Name()
                        << ", and connect inserted node's " << new_node_output_index << "th output to "
                        << dest_node.Name() << " 's " << dest_in_index << "th input.";

  ORT_ENFORCE(dest_in_index < static_cast<int>(dest_node.InputDefs().size()));
  ORT_ENFORCE(new_node_input_index < static_cast<int>(input_args.size()), "new_node_input_index is out of range.");
  ORT_ENFORCE(new_node_output_index < static_cast<int>(output_args.size()), "new_node_output_index is out of range.");
  ORT_ENFORCE(dest_node.MutableInputDefs()[dest_in_index] == input_args[new_node_input_index],
              "input_args[new_node_input_index] is not the same as dest_node.MutableInputDefs()[dest_in_index].",
              dest_node.MutableInputDefs()[dest_in_index]->Name(), " vs ", input_args[new_node_input_index]->Name());

  // Prepare Input and Outputs for the duplicated Gather/GatherND node.
  NodeArg* src_node_arg = dest_node.MutableInputDefs()[dest_in_index];

  // Create the duplicated Gather/GatherND node.
  Node& new_node = graph.AddNode(name, op_type, description, input_args, output_args, &attributes, domain);
  ORT_ENFORCE(graph.SetOpSchemaFromRegistryForNode(new_node), "Failed to set op schema for " + new_node.Name());

  // Connect dest_node's input node to dumplicated node.
  // Update new node producer and consumer map.
  for (size_t j = 0; j < new_node.MutableOutputDefs().size(); ++j) {
    graph.UpdateProducerNode(new_node.MutableOutputDefs()[j]->Name(), new_node.Index());
  }
  graph.AddConsumerNode(src_node_arg->Name(), &new_node);
  const Node* src_node = graph.GetProducerNode(src_node_arg->Name());
  if (src_node) {
    int src_out_index = optimizer_utils::IndexOfNodeOutput(*src_node, *src_node_arg);
    graph.AddEdge(src_node->Index(), new_node.Index(), src_out_index, new_node_input_index);
  }

  // Remove edge between dest_node and src_node.
  // Be noted, this will remove dest_node's input edges to src_node
  // (and also the src_node's output edges to dest_node).
  std::vector<graph_utils::GraphEdge> input_edge_to_remove;
  input_edge_to_remove.reserve(1);
  for (auto it = dest_node.InputEdgesBegin(), end = dest_node.InputEdgesEnd(); it != end; ++it) {
    LOGS(logger, WARNING) << "dest_node " << dest_node.Name() << " input edge: " << it->GetNode().Name()
                          << " output index: " << it->GetSrcArgIndex() << " input index: " << it->GetDstArgIndex();
    if (it->GetDstArgIndex() == dest_in_index) {
      input_edge_to_remove.push_back(graph_utils::GraphEdge::CreateGraphEdge(dest_node, *it, true));
      break;
    }
  }

  // If the input is graph input or initializer, no edge will be removed.
  if (input_edge_to_remove.size() > 0) {
    graph_utils::GraphEdge::RemoveGraphEdges(graph, input_edge_to_remove);

    // Remove target node from target input arg's consumer list.
    const std::string& src_node_arg_name = src_node_arg->Name();
    int input_use_count_by_dest_node = 0;
    for (size_t i = 0; i < dest_node.InputDefs().size(); ++i) {
      if (dest_node.InputDefs()[i]->Name().compare(src_node_arg_name) == 0) {
        ++input_use_count_by_dest_node;
      }
    }

    if (input_use_count_by_dest_node == 1) {
      graph.RemoveConsumerNode(src_node_arg_name, &dest_node);
    }
  }

  // Connect duplicated gather node to target node's input.
  dest_node.MutableInputDefs()[dest_in_index] = new_node.MutableOutputDefs()[new_node_output_index];
  // Add new edge connecting the duplicated gather with the target node directly.
  // This also updates the destination node's input node args
  graph.AddEdge(new_node.Index(), dest_node.Index(), new_node_output_index, dest_in_index);
  graph.AddConsumerNode(new_node.MutableOutputDefs()[new_node_output_index]->Name(), &dest_node);
  LOGS(logger, WARNING) << "Inserted " << op_type << " node on " << dest_node.Name()
                        << " 's " << dest_in_index << "th input " << dest_node.InputDefs()[dest_in_index]->Name();
  return &new_node;
}

TensorShapeProto CreateTensorShapeInsertDimAtAxis(const TensorShapeProto* src_shape, int axis, int64_t dim_value) {
  ORT_ENFORCE(axis < src_shape->dim_size(), "axis is out of range.", axis, " vs ", src_shape->dim_size());
  TensorShapeProto updated_shape;
  int j = 0;
  for (j = 0; j < axis; ++j) {
    auto dim = src_shape->dim(j);
    if (dim.has_dim_value()) {
      updated_shape.add_dim()->set_dim_value(dim.dim_value());
    } else if (dim.has_dim_param()) {
      updated_shape.add_dim()->set_dim_param(dim.dim_param());
    } else {
      ORT_THROW("Invalid dim found in CreateTensorShapeInsertDimAtAxis");
    }
  }
  updated_shape.add_dim()->set_dim_value(dim_value);
  for (; j < src_shape->dim_size(); ++j) {
    auto dim = src_shape->dim(j);
    if (dim.has_dim_value()) {
      updated_shape.add_dim()->set_dim_value(dim.dim_value());
    } else if (dim.has_dim_param()) {
      updated_shape.add_dim()->set_dim_param(dim.dim_param());
    } else {
      ORT_THROW("Invalid dim found in CreateTensorShapeInsertDimAtAxis");
    }
  }
  return updated_shape;
}

NodeArg* CreateUnsqueezeAxesInitializer(Graph& graph, const std::vector<int64_t>& values) {
  // TODO: support Unsqueeze < 13
  ONNX_NAMESPACE::TensorProto axes_const_tensor;
  axes_const_tensor.set_name(graph.GenerateNodeArgName("axes"));
  axes_const_tensor.set_data_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
  axes_const_tensor.add_dims(values.size());
  axes_const_tensor.set_raw_data(values.data(), values.size() * sizeof(int64_t));
  return &graph_utils::AddInitializer(graph, axes_const_tensor);
}

void AdaptInputAndOutputForScalarSlice(Graph& graph, Node& current_node, int current_node_output_index,
                                       int slice_axis, const std::string& entry_node_name,
                                       const std::unordered_map<int, SlicingInfo>& new_gather_infos,
                                       const logging::Logger& logger) {
  LOGS(logger, WARNING) << "AdaptInputAndOutputForScalarSlice for Node " << current_node.Name() << "("
                        << current_node.OpType() << ")";

  // For each handled inputs, insert Unsqueeze node to get the removed dim back at slice_axis.
  for (auto pair : new_gather_infos) {
    int input_index = pair.first;
    // TODO: support Unsqueeze < 13
    // Be noted, the unsqueeze should happens on the axis of new slice node.
    Node* new_node =
        InsertItermediateNodeOnDestInput(graph,
                                         current_node, input_index,
                                         0 /* new node input index to connect to current_node's input node*/,
                                         0 /* new node output index to connect to current_node*/,
                                         graph.GenerateNodeName(entry_node_name + "_adapt_input"),
                                         "Unsqueeze",
                                         "Unsqueeze node",
                                         {current_node.MutableInputDefs()[input_index],
                                          CreateUnsqueezeAxesInitializer(graph, {pair.second.axis})},
                                         {&graph.GetOrCreateNodeArg(
                                             graph.GenerateNodeArgName("intermidiate_output"),
                                             current_node.MutableInputDefs()[input_index]->TypeAsProto())},
                                         {}, kOnnxDomain,
                                         entry_node_name,
                                         logger);
    new_node->SetExecutionProviderType(current_node.GetExecutionProviderType());
    // Set correct shape for Unsquee node
    const TensorShapeProto* unsqueeze_input_shape = new_node->MutableInputDefs()[0]->Shape();
    new_node->MutableOutputDefs()[0]->SetShape(
        CreateTensorShapeInsertDimAtAxis(unsqueeze_input_shape, pair.second.axis, 1));
  }

  // Find the consumer node of MatMul, and the input index of that node connect to MatMul.
  std::vector<const Node*> consumers =
      graph.GetConsumerNodes(current_node.MutableOutputDefs()[current_node_output_index]->Name());
  ORT_ENFORCE(consumers.size() >= 1, "MatMul should have at least one consumer at this point. " +
                                         std::to_string(consumers.size()) + " consumers found.");
  Node& consumer = *graph.GetNode(consumers[0]->Index());
  int index = -1;
  for (size_t i = 0; i < consumer.InputDefs().size(); ++i) {
    auto input_arg = consumer.InputDefs()[i];
    if (input_arg->Name().compare(current_node.MutableOutputDefs()[current_node_output_index]->Name()) == 0) {
      index = i;
      break;
    }
  }

  // Create Squeeze node connecting MatMul output to consumer node.
  // TODO: support Unsqueeze < 13
  Node* matmul_out_adaptor_node =
      InsertItermediateNodeOnDestInput(graph, consumer, index,
                                       0,
                                       0 /* new node output index*/,
                                       graph.GenerateNodeName(current_node.OpType() + "_output"),
                                       "Squeeze",
                                       "Squeeze node",
                                       {consumer.MutableInputDefs()[index],
                                        CreateUnsqueezeAxesInitializer(graph, {slice_axis})},
                                       {&graph.GetOrCreateNodeArg(
                                           graph.GenerateNodeArgName("intermidiate_output"),
                                           consumer.MutableInputDefs()[index]->TypeAsProto())},
                                       {}, kOnnxDomain,
                                       graph.GenerateNodeName("output_adapter"),
                                       logger);

  matmul_out_adaptor_node->SetExecutionProviderType(current_node.GetExecutionProviderType());

  // Don't need set shape for Squeeze because original MatMul output is used as its output type.
  // Set correct shape for MatMul node
  const TensorShapeProto* matmul_out_shape = matmul_out_adaptor_node->MutableOutputDefs()[0]->Shape();
  current_node.MutableOutputDefs()[0]->SetShape(CreateTensorShapeInsertDimAtAxis(matmul_out_shape, slice_axis, 1));
}

bool DefaultOperatorPassThroughActorBase::PostProcess(Graph& graph, Node& current_node, int current_node_output_index,
                                                      int slice_axis, const std::string& entry_node_name,
                                                      const std::unordered_map<int, SlicingInfo>& new_gather_infos,
                                                      bool input_has_dim_1_for_axis, bool is_slice_scalar,
                                                      int /*input_rank*/,
                                                      ONNX_NAMESPACE::TensorShapeProto_Dimension& /*output_dim_on_axis*/,
                                                      const logging::Logger& logger) {
  LOGS(logger, WARNING) << "DefaultOperatorPassThroughActorBase for Node " << current_node.Name() << "("
                        << current_node.OpType() << ")";
  if (is_slice_scalar && input_has_dim_1_for_axis) {
    AdaptInputAndOutputForScalarSlice(graph, current_node, current_node_output_index, slice_axis,
                                      entry_node_name, new_gather_infos, logger);
  }

  return true;
}

bool SimplePassThroughActor::PreCheck(const Graph& /*graph*/,
                                      const Node& target_node,
                                      const SlicingInfo& info,
                                      std::unordered_map<int, int>& target_node_input_indices,
                                      std::vector<int>& input_dices,
                                      bool& input_has_dim_1_for_axis,
                                      const logging::Logger& logger) {
  auto gathernd_node = info.gather_node;
  int target_node_output_index = optimizer_utils::IndexOfNodeOutput(target_node, *gathernd_node->InputDefs()[0]);
  const NodeArg* gather_data_input_arg = target_node.OutputDefs()[target_node_output_index];
  LOGS(logger, WARNING) << "Enter SimplePassThroughPreCheck for node " << target_node.Name();
  // For each input of target_node, check whether it meets a requirements,
  // 1). either its rank is lower than minimum_rank_to_handle.
  // 2). or the dimension (if exists) before minimum_rank_to_handle is same as target node's output shape.
  // 3). or the dimension (if exists) before minimum_rank_to_handle is 1.
  // Otherwise, we will skip the optimization.
  auto check_shapes =
      [&info, gather_data_input_arg, &logger](const NodeArg* input_arg_to_compare,
                                              bool& fatal_error_found,
                                              bool& dim_1_for_axis_found) -> std::optional<int> {
    fatal_error_found = false;
    auto ret_pair = AreDimsCompatibleBeforeAxisInternal(gather_data_input_arg->Shape(), info.axis,
                                                        input_arg_to_compare->Shape());
    if (ret_pair.first == DimCompareRet::ExactEqual) {
      return info.axis;
    } else if (ret_pair.first == DimCompareRet::RankTooLow) {
      LOGS(logger, WARNING) << "Skip " << input_arg_to_compare->Name() << " because its rank is too low.";
      return std::nullopt;
    } else if (ret_pair.first == DimCompareRet::NotEqual) {
      fatal_error_found = true;
    } else if (ret_pair.first == DimCompareRet::BroadcastableEqual) {
      if (ret_pair.second) {
        LOGS(logger, WARNING) << "Skip " << input_arg_to_compare->Name()
                              << ", whose dim on axis is 1, no need to Gather from.";
        dim_1_for_axis_found = true;
        return std::nullopt;
      }
      return info.axis;
    }

    ORT_THROW("Unexpected return value from AreDimsCompatibleBeforeAxisInternal");
    return std::nullopt;
  };

  target_node_input_indices.clear();
  input_has_dim_1_for_axis = false;
  for (size_t i = 0; i < target_node.InputDefs().size(); ++i) {
    if (input_dices.size() > 0 && std::find(input_dices.begin(), input_dices.end(), i) == input_dices.end()) {
      continue;
    }
    bool fatal_error_found = false;
    auto ret = check_shapes(target_node.InputDefs()[i], fatal_error_found, input_has_dim_1_for_axis);
    if (fatal_error_found) {
      LOGS(logger, WARNING) << "Skip for node " << target_node.Name() << " due to input check failure at index " << i;
      return false;
    } else if (ret.has_value()) {
      target_node_input_indices[static_cast<int>(i)] = ret.value();
    }
  }

  // Make sure once Gather is moved before target node, all its outputs can be correctly be sliced.
  std::unordered_map<int, int> output_dices;
  for (size_t i = 0; i < target_node.OutputDefs().size(); ++i) {
    if (static_cast<int>(i) == target_node_output_index) {
      continue;
    }

    bool fatal_error_found = false;
    bool dim_1_for_axis_found = false;
    auto ret = check_shapes(target_node.OutputDefs()[i], fatal_error_found, dim_1_for_axis_found);
    if (fatal_error_found) {
      LOGS(logger, WARNING) << "Skip for node " << target_node.Name() << " due to output check failure at index " << i;
      return false;
    } else if (ret.has_value()) {
      output_dices[static_cast<int>(i)] = ret.value();
    }
  }
  bool output_check_success = output_dices.size() == target_node.OutputDefs().size() - 1;

  return output_check_success;
}

bool LayerNormPassThroughActor::PreCheck(const Graph& graph, const Node& target_node, const SlicingInfo& info,
                                         std::unordered_map<int, int>& target_node_input_indices,
                                         std::vector<int>& input_dices, bool& input_has_dim_1_for_axis,
                                         const logging::Logger& logger) {
  auto axis = static_cast<int64_t>(target_node.GetAttributes().at("axis").i());
  axis = axis < 0 ? axis + target_node.InputDefs()[0]->Shape()->dim_size() : axis;

  // Make sure layer norm's reduction happens after the axes we want to slice.
  if (axis <= info.axis) {
    return false;
  }

  return SimplePassThroughActor::PreCheck(graph, target_node, info, target_node_input_indices, input_dices, input_has_dim_1_for_axis, logger);
}

bool ReshapePassThroughActor::PreCheck(const Graph& graph, const Node& target_node, const SlicingInfo& info,
                                       std::unordered_map<int, int>& target_node_input_indices,
                                       std::vector<int>& /*input_dices*/, bool& /*input_has_dim_1_for_axis*/,
                                       const logging::Logger& logger) {
  auto data_input_shape = target_node.InputDefs()[0]->Shape();
  auto shape_input_shape = target_node.InputDefs()[1]->Shape();
  auto output_shape = target_node.OutputDefs()[0]->Shape();
  if (data_input_shape == nullptr || shape_input_shape == nullptr || shape_input_shape->dim_size() != 1 || output_shape == nullptr) {
    LOGS(logger, WARNING) << "Reshape input/output node arg shape is not valid.";
    return false;
  }

  if (!graph_utils::IsConstantInitializer(graph, target_node.InputDefs()[1]->Name())) {
    LOGS(logger, WARNING) << "Skip handle the Reshape, because the new shape is not constant.";
    return false;
  }

  auto in_dims = data_input_shape->dim();
  auto out_dims = output_shape->dim();
  int in_rank = in_dims.size();
  int out_rank = out_dims.size();

  int reshape_input_axis = -1;
  // Match from left to right.
  for (int i = 0; i < std::min(in_rank, out_rank); ++i) {
    bool dim_value_eq = in_dims[i].has_dim_value() && out_dims[i].has_dim_value() &&
                        in_dims[i].dim_value() == out_dims[i].dim_value();
    bool dim_param_eq = in_dims[i].has_dim_param() && out_dims[i].has_dim_param() &&
                        in_dims[i].dim_param() == out_dims[i].dim_param();
    if (dim_value_eq || dim_param_eq) {
      if (i == info.axis) {
        reshape_input_axis = i;
        break;
      }
      continue;
    }
  }

  if (reshape_input_axis == -1) {
    // Match from right to left.
    for (int i = 0; i < std::min(in_rank, out_rank); ++i) {
      int in_index = in_rank - 1 - i;
      int out_index = out_rank - 1 - i;
      bool dim_value_eq = in_dims[in_index].has_dim_value() && out_dims[out_index].has_dim_value() &&
                          in_dims[in_index].dim_value() == out_dims[out_index].dim_value();
      bool dim_param_eq = in_dims[in_index].has_dim_param() && out_dims[out_index].has_dim_param() &&
                          in_dims[in_index].dim_param() == out_dims[out_index].dim_param();
      if (dim_value_eq || dim_param_eq) {
        if (out_index == info.axis) {
          reshape_input_axis = in_index;
          break;
        }
        continue;
      }
    }
  }

  if (reshape_input_axis == -1) {
    LOGS(logger, WARNING) << "Cannot find Reshape's input axis for Gather.";
    return false;
  }

  target_node_input_indices[0] = reshape_input_axis;
  return true;
}

bool ReshapePassThroughActor::PostProcess(Graph& graph, Node& current_node, int /*target_node_input_index*/,
                                          int slice_axis, const std::string& entry_node_name,
                                          const std::unordered_map<int, SlicingInfo>& new_gather_infos,
                                          bool /*input_has_dim_1_for_axis*/, bool is_slice_scalar,
                                          int input_rank,
                                          ONNX_NAMESPACE::TensorShapeProto_Dimension& output_dim_on_axis,
                                          const logging::Logger& logger) {
  LOGS(logger, WARNING) << "ReshapePostProcess for Node " << current_node.Name() << "(" << current_node.OpType() << ")";
  InlinedVector<int64_t> new_shape_const_values;
  optimizer_utils::AppendTensorFromInitializer(graph, *current_node.InputDefs()[1], new_shape_const_values, true);

  auto create_new_initializer_from_vector = [&graph](NodeArg* arg_to_be_replaced,
                                                     const InlinedVector<int64_t>& new_values) -> NodeArg* {
    // Create new TensorProto.
    ONNX_NAMESPACE::TensorProto constant_tensor_proto;
    constant_tensor_proto.set_name(graph.GenerateNodeArgName(arg_to_be_replaced->Name()));
    constant_tensor_proto.set_data_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
    auto length = new_values.size();
    constant_tensor_proto.add_dims(length);
    constant_tensor_proto.set_raw_data(new_values.data(), length * sizeof(int64_t));

    // Add initializer into Graph.
    NodeArg* new_shape_arg = &graph_utils::AddInitializer(graph, constant_tensor_proto);
    // Update the output arg shape.
    ONNX_NAMESPACE::TensorShapeProto new_shape;
    new_shape.add_dim()->set_dim_value(length);
    new_shape_arg->SetShape(new_shape);

    return new_shape_arg;
  };

  // Id the shape constant on slice_axis is 0, then it keeps the original dim of input.
  // If it is scalar slice, then we just remove that dim. Otherwise, we don't need to update the dim value.
  if (new_shape_const_values[slice_axis] == 0) {
    if (is_slice_scalar) {
      LOGS(logger, WARNING) << "Removing axis " << slice_axis << " from shape tensor.";
      NodeArg* arg_to_be_replaced = current_node.MutableInputDefs()[1];
      InlinedVector<int64_t> new_values;
      for (int i = 0; i < static_cast<int>(new_shape_const_values.size()); ++i) {
        if (i != slice_axis) {
          new_values.push_back(new_shape_const_values[i]);
        }
      }
      auto new_shape_arg = create_new_initializer_from_vector(arg_to_be_replaced, new_values);
      graph_utils::ReplaceNodeInput(current_node, 1, *new_shape_arg);
    } else {
      LOGS(logger, WARNING) << "Reshape's shape has 0 specified for aixs: " << slice_axis << ", not need update.";
    }
    return true;
  }

  // TODO: add tests for this branch.
  // If it selected shape is dim value, we can update the shape tensor directory.
  if (output_dim_on_axis.has_dim_value()) {
    new_shape_const_values[slice_axis] = output_dim_on_axis.dim_value();
    auto new_shape_arg = create_new_initializer_from_vector(current_node.MutableInputDefs()[1], new_shape_const_values);
    graph_utils::ReplaceNodeInput(current_node, 1, *new_shape_arg);
    return true;
  }

  // TODO: add tests for this branch.
  // If it selected shape is dim param, it requires multiple Slice, one Shape and one Concat to get the updated shape
  // tensor.
  auto slice_shape_func = [&graph](NodeArg* shape, int64_t start, int64_t end) -> Node* {
    InlinedVector<NodeArg*> new_input_args;
    ONNX_NAMESPACE::TensorProto starts_const_tensor;
    starts_const_tensor.set_name(graph.GenerateNodeArgName("starts"));
    starts_const_tensor.set_data_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
    starts_const_tensor.add_dims(1);
    std::vector<int64_t> dim_values{start};
    starts_const_tensor.set_raw_data(dim_values.data(), sizeof(int64_t));
    NodeArg* starts_arg = &graph_utils::AddInitializer(graph, starts_const_tensor);

    ONNX_NAMESPACE::TensorProto ends_const_tensor;
    ends_const_tensor.set_name(graph.GenerateNodeArgName("ends"));
    ends_const_tensor.set_data_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
    ends_const_tensor.add_dims(1);
    std::vector<int64_t> ends_dim_values{end};
    ends_const_tensor.set_raw_data(ends_dim_values.data(), sizeof(int64_t));
    NodeArg* ends_arg = &graph_utils::AddInitializer(graph, ends_const_tensor);

    ONNX_NAMESPACE::TensorProto axes_const_tensor;
    axes_const_tensor.set_name(graph.GenerateNodeArgName("axes"));
    axes_const_tensor.set_data_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
    axes_const_tensor.add_dims(1);
    std::vector<int64_t> axes_dim_values{0};
    axes_const_tensor.set_raw_data(axes_dim_values.data(), sizeof(int64_t));
    NodeArg* axes_arg = &graph_utils::AddInitializer(graph, axes_const_tensor);

    new_input_args.push_back(shape);
    new_input_args.push_back(starts_arg);
    new_input_args.push_back(ends_arg);
    new_input_args.push_back(axes_arg);

    InlinedVector<NodeArg*> new_output_args;
    new_output_args.push_back(&graph.GetOrCreateNodeArg(graph.GenerateNodeArgName("slice_out_compute_opt"),
                                                        shape->TypeAsProto()));

    // Create the duplicated Gather/GatherND node.
    Node& first_slice_node = graph.AddNode(graph.GenerateNodeName("slice_to_compute_opt"),
                                           "Slice",
                                           "Slice op for compute optimization",
                                           new_input_args,
                                           new_output_args);

    auto shape_node = graph.GetProducerNode(shape->Name());
    if (shape_node) {
      first_slice_node.SetExecutionProviderType(shape_node->GetExecutionProviderType());
    }
    return &first_slice_node;
  };

  Node* first_slice = nullptr;
  if (slice_axis > 0) {
    first_slice = slice_shape_func(current_node.MutableInputDefs()[1], 0, slice_axis);
    ONNX_NAMESPACE::TensorShapeProto result_shape;
    result_shape.add_dim()->set_dim_value(slice_axis);
    first_slice->MutableOutputDefs()[0]->SetShape(result_shape);
  }

  Node* second_slice = nullptr;
  if (slice_axis + 1 <= input_rank - 1) {
    second_slice = slice_shape_func(current_node.MutableInputDefs()[1], slice_axis + 1, input_rank);
    ONNX_NAMESPACE::TensorShapeProto result_shape;
    result_shape.add_dim()->set_dim_value(input_rank - slice_axis - 1);
    second_slice->MutableOutputDefs()[0]->SetShape(result_shape);
  }

  Node* third_slice = nullptr;
  if (!is_slice_scalar) {
    int new_gather_input_rank = new_gather_infos.at(0).input_rank;
    NodeArg* shape_out = &graph.GetOrCreateNodeArg(graph.GenerateNodeArgName("shape_out"),
                                                   current_node.MutableInputDefs()[1]->TypeAsProto());
    Node& new_dim_retrieve_node = graph.AddNode(graph.GenerateNodeName("shape_retrieve"),
                                                "Shape",
                                                "Shape op for compute optimization",
                                                {current_node.MutableInputDefs()[0]},
                                                {shape_out});
    // new_dim_retrieve_node.SetExecutionProviderType(target_node.MutableInputDefs()[1]->GetExecutionProviderType());
    ONNX_NAMESPACE::TensorShapeProto result_shape;
    result_shape.add_dim()->set_dim_value(new_gather_input_rank);
    new_dim_retrieve_node.MutableOutputDefs()[0]->SetShape(result_shape);
    third_slice = slice_shape_func(shape_out, new_gather_infos.at(0).axis, new_gather_infos.at(0).axis + 1);
  }

  // Concatenate the two slices.
  InlinedVector<NodeArg*> new_input_args;
  if (first_slice) {
    new_input_args.push_back(first_slice->MutableOutputDefs()[0]);
  }

  if (third_slice) {
    new_input_args.push_back(third_slice->MutableOutputDefs()[0]);
  }

  if (second_slice) {
    new_input_args.push_back(second_slice->MutableOutputDefs()[0]);
  }

  int data_input_index = 1;

  InlinedVector<NodeArg*> output_args;
  output_args.push_back(
      &graph.GetOrCreateNodeArg(graph.GenerateNodeArgName("intermidiate_output"),
                                current_node.MutableInputDefs()[data_input_index]->TypeAsProto()));
  InsertItermediateNodeOnDestInput(graph, current_node, data_input_index, 0, 0 /* new node output index */,
                                   graph.GenerateNodeName("Concat"), "Concat", "Adapter for Reshape data input",
                                   new_input_args, output_args, {}, kOnnxDomain, entry_node_name,
                                   logger);

  return true;
}

bool TransposePassThroughActor::PreCheck(const Graph& /*graph*/, const Node& target_node, const SlicingInfo& info,
                                         std::unordered_map<int, int>& target_node_input_indices,
                                         std::vector<int>& /*input_dices*/, bool& /*input_has_dim_1_for_axis*/,
                                         const logging::Logger& logger) {
  InlinedVector<int64_t> perm;
  if (!graph_utils::GetRepeatedNodeAttributeValues(target_node, "perm", perm)) {
    LOGS(logger, WARNING) << "perm attribute is not set for node " << target_node.Name();
    return false;
  }

  target_node_input_indices[0] = perm[info.axis];
  return true;
}

bool TransposePassThroughActor::PostProcess(Graph& graph, Node& current_node, int current_node_output_index,
                                            int slice_axis, const std::string& entry_node_name,
                                            const std::unordered_map<int, SlicingInfo>& new_gather_infos,
                                            bool /*input_has_dim_1_for_axis*/, bool is_slice_scalar,
                                            int /*input_rank*/,
                                            ONNX_NAMESPACE::TensorShapeProto_Dimension& /*output_dim_on_axis*/,
                                            const logging::Logger& logger) {
  LOGS(logger, WARNING) << "TransposePostProcess for Node " << current_node.Name() << "("
                        << current_node.OpType() << ")";

  // We need keep the original dimension to align with original perm.
  if (is_slice_scalar) {
    AdaptInputAndOutputForScalarSlice(graph, current_node, current_node_output_index, slice_axis,
                                      entry_node_name, new_gather_infos, logger);
  }
  return true;
}

bool MatMulPassThroughActor::PostProcess(Graph& graph, Node& current_node, int current_node_output_index,
                                         int slice_axis, const std::string& entry_node_name,
                                         const std::unordered_map<int, SlicingInfo>& new_gather_infos,
                                         bool /*input_has_dim_1_for_axis*/, bool is_slice_scalar,
                                         int /*input_rank*/,
                                         ONNX_NAMESPACE::TensorShapeProto_Dimension& /*output_dim_on_axis*/,
                                         const logging::Logger& logger) {
  LOGS(logger, WARNING) << "MatMulPassThroughActor for Node " << current_node.Name() << "("
                        << current_node.OpType() << ")";

  // We need keep the original dimension to avoid the matmul inputs cannot be compatible to compute.
  if (is_slice_scalar) {
    AdaptInputAndOutputForScalarSlice(graph, current_node, current_node_output_index, slice_axis,
                                      entry_node_name, new_gather_infos, logger);
  }
  return true;
}

}  // namespace compute_optimizer
}  // namespace optimizer
}  // namespace onnxruntime
