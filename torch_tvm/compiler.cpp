#include "compiler.h"
#include "operators.h"

#include <ATen/DLConvertor.h>
#include <torch/csrc/jit/constants.h>
#include <torch/csrc/jit/interpreter.h>
#include <limits>

using namespace torch::jit;

namespace {
constexpr size_t kAlignment = 64;
}

tvm::relay::DataType scalarTypeToTVMType(at::ScalarType pt_type) {
  static const std::unordered_map<at::ScalarType, tvm::relay::DataType> type_mapping = {
    {at::ScalarType::Float, ::tvm::Float(32)},
    {at::ScalarType::Double, ::tvm::Float(64)},
    {at::ScalarType::Int, ::tvm::Int(32)},
    {at::ScalarType::Long, ::tvm::Int(64)},
    {at::ScalarType::Bool, ::tvm::Bool()},
    {at::ScalarType::Char, ::tvm::Int(8)},
    {at::ScalarType::Byte, ::tvm::UInt(8)},
    {at::ScalarType::QInt8, ::tvm::Int(8)},
    {at::ScalarType::QUInt8, ::tvm::UInt(8)},
    {at::ScalarType::QInt32, ::tvm::Int(32)},
  };

  TORCH_CHECK(type_mapping.find(pt_type) != type_mapping.end(),
              "could not handle the type ", pt_type,
              " when creating tensor type node in TVM");
  return type_mapping.at(pt_type);
}

tvm::relay::Var TVMCompiler::convertToRelay(Value* val, TVMContext ctx) {
  auto optional_ivalue = toIValue(val);
  if (optional_ivalue.has_value()) {
    val->inferTypeFrom(optional_ivalue.value().toTensor());
  }
  if (val->isCompleteTensor()) {
    auto pt_t = val->type()->cast<CompleteTensorType>();
    tvm::Array<tvm::relay::IndexExpr> sizes;
    for (const auto& size : pt_t->sizes()) {
      sizes.push_back(tvm::relay::IndexExpr(static_cast<int32_t>(size)));
    }
    at::ScalarType pt_type = pt_t->scalarType();
    auto t = tvm::relay::TensorTypeNode::make(sizes, scalarTypeToTVMType(pt_type));
    auto v = tvm::relay::VarNode::make(
        val->debugName() +
            std::to_string(reinterpret_cast<std::uintptr_t>(val)),
        t);
    return v;
  }
  TORCH_INTERNAL_ASSERT(0);
}

tvm::relay::Expr TVMCompiler::convertToRelay(
    const IValue& val,
    TVMContext ctx) {
  // All doubles are converted to floats
  if (val.isDouble()) {
    auto x = tvm::runtime::NDArray::Empty(
        {}, tvm::runtime::String2TVMType("float32"), ctx);
    auto d = val.toDouble();
    TORCH_CHECK(d <= std::numeric_limits<float>::max());
    TORCH_CHECK(d >= std::numeric_limits<float>::lowest());
    auto f = static_cast<float>(d);
    reinterpret_cast<float*>(x->data)[0] = f;
    auto v = tvm::relay::ConstantNode::make(x);
    return v;
  }
  // All Ints are converted to int32, which may overflow
  if (val.isInt()) {
    auto x = tvm::runtime::NDArray::Empty(
        {}, tvm::runtime::String2TVMType("int32"), ctx);
    auto l = val.toInt();
    TORCH_CHECK(l <= std::numeric_limits<int32_t>::max());
    TORCH_CHECK(l >= std::numeric_limits<int32_t>::lowest());
    reinterpret_cast<int32_t*>(x->data)[0] = l;
    auto v = tvm::relay::ConstantNode::make(x);
    return v;
  }
  if (val.isBool()) {
    auto x = tvm::runtime::NDArray::Empty(
        {}, tvm::runtime::String2TVMType("bool"), ctx);
    reinterpret_cast<bool*>(x->data)[0] = val.toBool();
    auto v = tvm::relay::ConstantNode::make(x);
    return v;
  }
  // TODO Add None type to Relay
  // HACK sentinel value used for None type
  if (val.isNone()) {
    auto x = tvm::runtime::NDArray::Empty(
        {}, tvm::runtime::String2TVMType("uint64"), ctx);
    reinterpret_cast<uint64_t*>(x->data)[0] = getNoneSentinel();
    auto v = tvm::relay::ConstantNode::make(x);
    return v;
  }
  if (val.isIntList()) {
    tvm::Array<tvm::relay::Expr> tuple_elems;
    for (const auto& elem : val.toIntList()) {
      auto x = tvm::runtime::NDArray::Empty(
          {}, tvm::runtime::String2TVMType("int32"), ctx);
      TORCH_CHECK(elem <= std::numeric_limits<int32_t>::max());
      TORCH_CHECK(elem >= std::numeric_limits<int32_t>::lowest());
      reinterpret_cast<int32_t*>(x->data)[0] = elem;
      auto v = tvm::relay::ConstantNode::make(x);
      tuple_elems.push_back(v);
    }
    return tvm::relay::TupleNode::make(tuple_elems);
  }
  TORCH_CHECK(
      0, "Cannot convert value ", val, " to Relay yet.  Please file a bug.\n");
}

tvm::relay::Function TVMCompiler::convertToRelay(
    std::shared_ptr<Graph> subgraph,
    TVMContext ctx,
    std::vector<Value*>* input_values) {
  std::unordered_map<Value*, tvm::relay::Expr> value_map;
  tvm::Array<tvm::relay::Var> input_vars;

  for (const auto& input : subgraph->inputs()) {
    TORCH_INTERNAL_ASSERT(input->isCompleteTensor());
    auto v = convertToRelay(input, ctx);
    input_vars.push_back(v);
    if (input_values) {
      input_values->emplace_back(input);
    }
    value_map[input] = v;
  }

  auto frontier = subgraph->inputs().vec();
  // TODO error handle incorrectly formed graphs (not dominated by frontier)
  while (frontier.size()) {
    std::vector<Value*> new_frontier = {};
    for (const auto& value : frontier) {
      auto uses = value->uses();
      for (const auto& use : uses) {
        tvm::Array<tvm::relay::Expr> relay_inputs;
        auto skip_user = false;
        for (const auto& input : use.user->inputs()) {
          if (value_map.find(input) == value_map.end()) {
            // We may be dealing with a constant, handle that here
            auto optional_ivalue = toIValue(input);
            if (!optional_ivalue.has_value()) {
              skip_user = true;
              break;
            } else {
              if (optional_ivalue.value().isTensor()) {
                if (input_values) {
                  input_values->emplace_back(input);
                }
                auto input_var = convertToRelay(input, ctx);
                input_vars.push_back(input_var);
                value_map[input] = input_var;
              } else {
                value_map[input] = convertToRelay(optional_ivalue.value(), ctx);
              }
            }
          }
          relay_inputs.push_back(value_map[input]);
        }
        if (skip_user) {
          continue;
        }
        // Things like prim::Return
        if (use.user->outputs().size() < 1) {
          continue;
        }
        // if there are 2+ outputs, getOperator returns a tuple
        if (use.user->outputs().size() == 1) {
          value_map[use.user->output()] = getOperator(use.user, relay_inputs);
          new_frontier.emplace_back(use.user->output());
        } else {
          auto tuple = getOperator(use.user, relay_inputs);
          int index = 0;
          for (const auto& output : use.user->outputs()) {
            auto n = tvm::make_node<tvm::relay::TupleGetItemNode>();
            n->tuple = tuple;
            n->index = index;
            value_map[output] = tvm::relay::TupleGetItem(n);
            index++;
            new_frontier.emplace_back(output);
          }
        }
      }
    }
    frontier = new_frontier;
  }

  tvm::NodePtr<tvm::relay::TupleNode> n =
      tvm::make_node<tvm::relay::TupleNode>();
  tvm::Array<tvm::relay::Expr> fields;
  for (const auto& sg_output : subgraph->outputs()) {
    TORCH_INTERNAL_ASSERT(value_map.find(sg_output) != value_map.end());
    fields.push_back(value_map[sg_output]);
  }
  n->fields = std::move(fields);
  auto output = tvm::relay::Tuple(n);

  tvm::Array<tvm::relay::Var> free_vars = tvm::relay::FreeVars(output);
  TORCH_CHECK(
      free_vars.size() <= input_vars.size(),
      "Determined ",
      free_vars.size(),
      " free vars but only ",
      input_vars.size(),
      " inputs");

  return tvm::relay::FunctionNode::make(
      input_vars, output, tvm::relay::Type(), {});
}

TVMCompiler::TVMCompiler(
    const Node* node,
    int opt_level,
    bool strict,
    std::string device_type,
    std::string device,
    std::string host)
    : opt_level_(opt_level),
      strict_(strict),
      device_type_(device_type),
      device_(device),
      host_(host) {
  if (device_type_ == "gpu") {
    ctx_.device_type = kDLGPU;
  } else {
    ctx_.device_type = kDLCPU;
  }
  ctx_.device_id = 0;
  subgraph_ = node->g(attr::Subgraph);
  auto pfb = tvm::runtime::Registry::Get("relay.build_module._BuildModule");
  TORCH_INTERNAL_ASSERT(pfb);
  build_mod_ = (*pfb)();
}

void TVMCompiler::run(Stack& stack) {
  std::unordered_map<Value*, IValue> value_to_ivalue;
  int num_inputs = subgraph_->inputs().size();
  at::ArrayRef<IValue> inputs = last(stack, num_inputs);

  for (auto i = 0; i < inputs.size(); ++i) {
    auto value_input = subgraph_->inputs()[i];
    value_to_ivalue[value_input] = inputs[i];
  }

  CompleteArgumentSpec spec{false, ArrayRef<IValue>(inputs)};

  if (cache_.find(spec) == cache_.end()) {
    for (auto& kv : value_to_ivalue) {
      kv.first->inferTypeFrom(kv.second.toTensor());
    }
    // bail out mechanism: try to convert to Relay, if it fails to convert the
    // graph by any reason(i.e. op difference), depend on the user preference,
    // either throw or fall back to the JIT interpreter for execution
    tvm::relay::Function tvm_func;
    try {
      tvm_func = convertToRelay(subgraph_, ctx_, &cache_[spec].input_values);
    } catch (const std::exception& e) {
      if (strict_) {
        AT_ERROR(
            "Pytorch TVM: fail to convert to relay, exception: ", e.what());
      }
      LOG(WARNING)
          << "Pytorch TVM: fail to convert to relay, falling back to JIT for execution, exception: "
          << e.what() << "\n";
      InterpreterState(Code(subgraph_)).run(stack);
      return;
    }
    auto build_f = build_mod_.GetFunction("build", false);
    auto json_f = build_mod_.GetFunction("get_graph_json", false);
    auto mod_f = build_mod_.GetFunction("get_module", false);
    tvm::Map<tvm::Integer, tvm::Target> target_map = {
        {ctx_.device_type, tvm::Target::Create(device_)}};
    build_f(tvm_func, target_map, tvm::Target::Create(host_));
    std::string json = json_f();
    tvm::runtime::Module mod = mod_f();
    auto pfr = tvm::runtime::Registry::Get("tvm.graph_runtime.create");
    TORCH_INTERNAL_ASSERT(pfr);
    tvm::runtime::Module run_mod =
        (*pfr)(json, mod, (int)ctx_.device_type, (int)ctx_.device_id);
    cache_[spec].set_input = run_mod.GetFunction("set_input", false);
    cache_[spec].set_input_zero_copy = run_mod.GetFunction("set_input_zero_copy", false);
    cache_[spec].kernel = run_mod.GetFunction("run", false);
    cache_[spec].get_output = run_mod.GetFunction("get_output", false);
    auto get_num_outputs = run_mod.GetFunction("get_num_outputs", false);
    int n = get_num_outputs();
    TORCH_CHECK(
        subgraph_->outputs().size() == n,
        "Compiled subgraph with mismatching num outputs");
  }

  for (auto i = 0; i < cache_[spec].input_values.size(); ++i) {
    auto* value = cache_[spec].input_values[i];
    if (!value_to_ivalue.count(value)) {
      auto optional_ivalue = toIValue(value);
      TORCH_INTERNAL_ASSERT(optional_ivalue.has_value());
      value_to_ivalue[value] = optional_ivalue.value();
    }
    auto ivalue = value_to_ivalue.at(cache_[spec].input_values[i]);
    // TODO: support more data types
    auto tensor = ivalue.toTensor().to(at::kFloat);
    auto dl_tensor = at::toDLPack(tensor);
    if (static_cast<size_t>(dl_tensor.data) % kAlignment == 0) {
      cache_[spec].set_input_zero_copy(i, &dl_tensor);
    } else {
      cache_[spec].set_input(i, tvm::runtime::NDArray::FromDLPack(dl_tensor));
    }
  }

  cache_[spec].kernel();

  // clean the stack and add outputs to the stack
  drop(stack, num_inputs);
  int i = 0;
  for (const auto& output : subgraph_->outputs()) {
    tvm::runtime::NDArray ret_val = cache_[spec].get_output(i);
    auto dl_tensor = ret_val.ToDLPack();
    auto tensor = at::fromDLPack(dl_tensor);
    auto var = torch::autograd::make_variable(tensor);
    stack.push_back(IValue(var));
    i++;
  }
}
