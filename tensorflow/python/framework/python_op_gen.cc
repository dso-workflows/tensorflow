/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/python/framework/python_op_gen.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "tensorflow/core/framework/api_def.pb.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_def.pb.h"
#include "tensorflow/core/framework/op_def_util.h"
#include "tensorflow/core/framework/op_gen_lib.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/python/framework/python_op_gen_internal.h"

namespace tensorflow {
namespace {

const int kRightMargin = 78;

constexpr char kEagerFallbackSuffix[] = "_eager_fallback";

// Maps C++ dtype enum values to Python DType classes
const std::unordered_map<string, string> dtype_type{
    {"_dtypes.float16", "_dtypes.Float16"},
    {"_dtypes.half", "_dtypes.Half"},
    {"_dtypes.float32", "_dtypes.Float32"},
    {"_dtypes.float64", "_dtypes.Float64"},
    {"_dtypes.bfloat16", "_dtypes.BFloat16"},
    {"_dtypes.complex64", "_dtypes.Complex64"},
    {"_dtypes.complex128", "_dtypes.Complex128"},
    {"_dtypes.int8", "_dtypes.Int8"},
    {"_dtypes.uint8", "_dtypes.UInt8"},
    {"_dtypes.uint16", "_dtypes.UInt16"},
    {"_dtypes.uint32", "_dtypes.UInt32"},
    {"_dtypes.uint64", "_dtypes.UInt64"},
    {"_dtypes.int16", "_dtypes.Int16"},
    {"_dtypes.int32", "_dtypes.Int32"},
    {"_dtypes.int64", "_dtypes.Int64"},
    {"_dtypes.bool", "_dtypes.Bool"},
    {"_dtypes.string", "_dtypes.String"},
    {"_dtypes.qint8", "_dtypes.QInt8"},
    {"_dtypes.quint8", "_dtypes.QUInt8"},
    {"_dtypes.qint16", "_dtypes.QInt16"},
    {"_dtypes.quint16", "_dtypes.QUInt16"},
    {"_dtypes.qint32", "_dtypes.QInt32"},
    {"_dtypes.resource", "_dtypes.Resource"},
    {"_dtypes.variant", "_dtypes.Variant"}};

string AttrVarName(const string& attr_name,
                   std::unordered_map<string, string>* attr_expressions) {
  const string var = strings::StrCat("_attr_", attr_name);
  if (attr_expressions != nullptr) (*attr_expressions)[attr_name] = var;
  return var;
}

void AddInferredAttr(const string& indentation, const string& attr_name,
                     const string& value_expression, string* result,
                     std::unordered_map<string, string>* attr_expressions) {
  strings::StrAppend(result, indentation,
                     AttrVarName(attr_name, attr_expressions), " = ",
                     value_expression, "\n");
}

string VectorToTuple(const std::vector<string>& l) {
  if (l.size() == 1) return strings::StrCat("(", l.front(), ",)");
  string ret = "(";
  for (int i = 0, end = l.size(); i < end; ++i) {
    if (i > 0) {
      strings::StrAppend(&ret, ", ");
    }
    strings::StrAppend(&ret, l[i]);
  }
  strings::StrAppend(&ret, ")");
  return ret;
}

void Unflatten(const string& prefix, const std::vector<string>& output_sizes,
               const string& var, string* result) {
  for (int i = 0, end = output_sizes.size(); i < end; ++i) {
    if (!output_sizes[i].empty()) {
      strings::StrAppend(result, prefix, var, " = ");
      if (i > 0) strings::StrAppend(result, var, "[:", i, "] + ");
      if (i + 1 < end) {
        // Special case i == 0 to avoid "0 +" in the generated code.
        if (i == 0) {
          strings::StrAppend(result, "[", var, "[:", output_sizes[i], "]] + ",
                             var, "[", output_sizes[i], ":]");
        } else {
          strings::StrAppend(result, "[", var, "[", i, ":", i, " + ",
                             output_sizes[i], "]] + ", var, "[", i, " + ",
                             output_sizes[i], ":]");
        }
      } else {
        strings::StrAppend(result, "[", var, "[", i, ":]]");
      }
      strings::StrAppend(result, "\n");
    }
  }
}

string TensorPBString(const TensorProto& pb) {
  // Explicitly not using ShortDebugString, because ShortDebugString should
  // not be used as a format for transporting information (it's e.g. subject
  // to redaction of sensitive information). There is a PrintShortTextProto
  // helper, but it's not feasible to depend on that library).

  std::string message_short_text;

  ::tensorflow::protobuf::TextFormat::Printer printer;
  printer.SetSingleLineMode(true);
  printer.SetExpandAny(true);

  printer.PrintToString(pb, &message_short_text);

  // Note: This gets used in the argument list, and so must survive naive
  // word wrapping.
  return strings::StrCat("\"\"\"", message_short_text, "\"\"\"");
}

class GenEagerPythonOp : public python_op_gen_internal::GenPythonOp {
 public:
  GenEagerPythonOp(const OpDef& op_def, const ApiDef& api_def,
                   const string& function_name, bool add_type_annotations)
      : python_op_gen_internal::GenPythonOp(op_def, api_def, function_name,
                                            add_type_annotations) {
    op_name_ = function_name_;
    absl::ConsumePrefix(&op_name_, "_");
  }
  ~GenEagerPythonOp() override = default;

  string Code() override;

 protected:
  void HandleGraphMode(const string& function_setup,
                       const std::vector<string>& output_sizes);

  string GetEagerNotAllowedError();
  void ExpectListArg(const string& indentation, const string& arg_name,
                     string* output);
  bool GetEagerFunctionSetup(const string& indentation, string* function_setup);
  void GetOutputSizesAndNumOutputsExpr(std::vector<string>* output_sizes,
                                       string* num_outputs_expr);

  void AddEagerFunctionTeardown(const string& indentation,
                                const std::vector<string>& output_sizes,
                                bool execute_record_gradient);

  bool AddEagerFastPathAndGraphCode(
      const string& parameters, const std::vector<string>& output_sizes,
      const string& eager_not_allowed_error,
      const std::unordered_map<string, string>& type_annotations);
  bool AddEagerFallbackCode(
      const string& parameters, const std::vector<string>& output_sizes,
      const string& num_outputs_expr, const string& eager_not_allowed_error,
      const std::unordered_map<string, string>& type_annotations);
  void AddEagerFastPathExecute();

  void AddEagerInferredAttrs(const string& indentation);
  void AddEagerInputCasts(const string& indentation);
  void AddEagerAttrs(const string& indentation);
  void AddEagerExecute(const string& indentation,
                       const string& num_outputs_expr);
  void AddFallbackDispatch(const string& prefix);
  void AddTypeBasedDispatch(const string& prefix);
  void AddTypeBasedDispatcherAlias();

  void AddRawOpExport(const string& parameters);

  std::unordered_map<string, string> GetTypeAnnotations();

  void GenerateTypeVars(
      const std::unordered_map<string, string>& type_annotations);

  void AddReturnTypeAnnotation(
      const std::unordered_map<string, string>& type_annotations);

  void AddAttrForArg(const string& attr, int arg_index) {
    gtl::InsertIfNotPresent(&inferred_attrs_, attr,
                            op_def_.input_arg(arg_index).name());
    auto iter = attr_to_args_.find(attr);
    if (iter == attr_to_args_.end()) {
      attr_to_args_.insert(AttrToArgMap::value_type(attr, {arg_index}));
    } else {
      iter->second.push_back(arg_index);
    }
  }

  // Returns a string expression representing a flattened list of all
  // the inputs given by `*input_indices` (or all inputs if
  // `input_indices` is nullptr).  `*output_sizes` can be used to unflatten.
  string FlattenInputs(const std::vector<int>* input_indices,
                       std::vector<string>* output_sizes) const;

  StringPiece op_name_;
  typedef std::unordered_map<string, std::vector<int>> AttrToArgMap;
  AttrToArgMap attr_to_args_;
  std::unordered_map<string, string> attr_expressions_;
  // This has all the input args followed by those attrs that don't have
  // defaults.
  std::vector<python_op_gen_internal::ParamNames> params_no_default_;
  // The parameters with defaults (these have to be listed after those without).
  // No input args are included, just attrs.
  std::vector<std::pair<python_op_gen_internal::ParamNames, string>>
      params_with_default_;
};

string GetEagerPythonOp(const OpDef& op_def, const ApiDef& api_def,
                        const string& function_name,
                        bool add_type_annotations) {
  return GenEagerPythonOp(op_def, api_def, function_name, add_type_annotations)
      .Code();
}

string GenEagerPythonOp::FlattenInputs(
    const std::vector<int>* input_indices,
    std::vector<string>* output_sizes) const {
  string inputs;
  enum { STARTING, WAS_LIST_INPUT, WAS_SOLO_INPUT } inputs_state = STARTING;
  const int n = input_indices != nullptr ? input_indices->size()
                                         : op_def_.input_arg_size();
  for (int j = 0; j < n; ++j) {
    const int i = input_indices ? (*input_indices)[j] : j;
    const auto& arg(op_def_.input_arg(i));
    const bool is_list =
        !arg.type_list_attr().empty() || !arg.number_attr().empty();
    if (is_list) {
      if (inputs_state == WAS_SOLO_INPUT) {
        strings::StrAppend(&inputs, "] + ");
      } else if (inputs_state == WAS_LIST_INPUT) {
        strings::StrAppend(&inputs, " + ");
      }
      strings::StrAppend(&inputs, "list(", param_names_[i].GetRenameTo(), ")");
      inputs_state = WAS_LIST_INPUT;
      if (output_sizes != nullptr) {
        if (!arg.number_attr().empty()) {
          output_sizes->emplace_back(AttrVarName(arg.number_attr(), nullptr));
        } else {
          output_sizes->emplace_back(
              strings::StrCat("len(", param_names_[i].GetRenameTo(), ")"));
        }
      }
    } else {
      if (inputs_state == WAS_SOLO_INPUT) {
        strings::StrAppend(&inputs, ", ");
      } else if (inputs_state == WAS_LIST_INPUT) {
        strings::StrAppend(&inputs, " + [");
      } else {
        strings::StrAppend(&inputs, "[");
      }
      strings::StrAppend(&inputs, param_names_[i].GetRenameTo());
      inputs_state = WAS_SOLO_INPUT;
      if (output_sizes != nullptr) output_sizes->emplace_back();
    }
  }
  if (inputs_state == STARTING) return "[]";
  if (inputs_state == WAS_SOLO_INPUT) {
    strings::StrAppend(&inputs, "]");
  }
  return inputs;
}

string GenEagerPythonOp::Code() {
  if (api_def_.visibility() == ApiDef::SKIP) {
    return "";
  }

  for (int i = 0; i < api_def_.arg_order_size(); ++i) {
    const auto& arg = *FindInputArg(api_def_.arg_order(i), op_def_);
    const auto& api_def_arg = *FindInputArg(api_def_.arg_order(i), api_def_);
    params_no_default_.emplace_back(api_def_arg.name(),
                                    api_def_arg.rename_to());
    if (!arg.type_attr().empty()) {
      AddAttrForArg(arg.type_attr(), i);
    } else if (!arg.type_list_attr().empty()) {
      AddAttrForArg(arg.type_list_attr(), i);
    }
    if (!arg.number_attr().empty()) {
      AddAttrForArg(arg.number_attr(), i);
    }
  }
  for (int i = 0; i < op_def_.attr_size(); ++i) {
    const auto& attr(op_def_.attr(i));
    const auto& api_def_attr(api_def_.attr(i));
    // Do not add inferred attrs to the Python function signature.
    if (inferred_attrs_.find(attr.name()) == inferred_attrs_.end()) {
      if (api_def_attr.has_default_value()) {
        if (attr.type() == "tensor") {
          params_with_default_.emplace_back(
              python_op_gen_internal::ParamNames(api_def_attr.name(),
                                                 api_def_attr.rename_to()),
              strings::StrCat(
                  "_execute.make_tensor(",
                  TensorPBString(api_def_attr.default_value().tensor()), ", \"",
                  api_def_attr.rename_to(), "\")"));
        } else if (attr.type() == "list(tensor)") {
          std::vector<string> pbtxt;
          for (const auto& pb : api_def_attr.default_value().list().tensor()) {
            pbtxt.emplace_back(TensorPBString(pb));
          }
          params_with_default_.emplace_back(
              python_op_gen_internal::ParamNames(api_def_attr.name(),
                                                 api_def_attr.rename_to()),
              strings::StrCat("[_execute.make_tensor(_pb, \"",
                              api_def_attr.rename_to(), "\") for _pb in ",
                              VectorToTuple(pbtxt), "]"));
        } else {
          params_with_default_.emplace_back(
              python_op_gen_internal::ParamNames(api_def_attr.name(),
                                                 api_def_attr.rename_to()),
              python_op_gen_internal::AttrValueToPython(
                  attr.type(), api_def_attr.default_value(), "_dtypes."));
        }
      } else {
        params_no_default_.emplace_back(api_def_attr.name(),
                                        api_def_attr.rename_to());
      }
    }
  }

  // Save the list of attr parameters (attrs that won't be inferred),
  // those with defaults go at the end.
  // Get the attrs in the order we want by taking the attrs without defaults
  // from the end of params_no_default_, and adding params_no_default_.
  attrs_.reserve(params_no_default_.size() - op_def_.input_arg_size() +
                 params_with_default_.size());
  for (int i = op_def_.input_arg_size(), end = params_no_default_.size();
       i < end; ++i) {
    attrs_.push_back(params_no_default_[i].GetName());
  }
  for (const auto& p : params_with_default_) {
    attrs_.push_back(p.first.GetName());
  }

  // TODO(slebedev): call AvoidPythonReserved on each param?
  param_names_.reserve(params_no_default_.size() + params_with_default_.size());
  param_names_.insert(param_names_.begin(), params_no_default_.begin(),
                      params_no_default_.end());
  for (const auto& param_and_default : params_with_default_) {
    param_names_.push_back(param_and_default.first);
  }

  std::unordered_map<string, string> type_annotations;
  // Only populate map for allowlisted ops
  if (add_type_annotations_) {
    type_annotations = GetTypeAnnotations();
  }

  string parameters;
  // Param can be an input or an attr
  for (const auto& param : params_no_default_) {
    if (!parameters.empty()) strings::StrAppend(&parameters, ", ");
    strings::StrAppend(&parameters, param.GetRenameTo());

    if (type_annotations.find(param.GetName()) != type_annotations.end()) {
      strings::StrAppend(&parameters, ": ",
                         type_annotations.at(param.GetName()));
    }
  }

  string parameters_with_defaults = parameters;
  for (const auto& param_and_default : params_with_default_) {
    if (!parameters.empty()) strings::StrAppend(&parameters, ", ");
    if (!parameters_with_defaults.empty())
      strings::StrAppend(&parameters_with_defaults, ", ");

    strings::StrAppend(&parameters, param_and_default.first.GetRenameTo());
    strings::StrAppend(&parameters_with_defaults,
                       param_and_default.first.GetRenameTo());
    if (type_annotations.find(param_and_default.first.GetName()) !=
        type_annotations.end()) {
      const string param_type =
          type_annotations.at(param_and_default.first.GetName());
      // Append to parameters and parameters_with_defaults because multiple
      // functions are generated by AddEagerFastPathAndGraphCode() and
      // AddEagerFallbackCode()
      strings::StrAppend(&parameters, ": ", param_type);
      strings::StrAppend(&parameters_with_defaults, ":", param_type);
    }

    strings::StrAppend(&parameters_with_defaults, "=",
                       param_and_default.second);
  }

  strings::StrAppend(&parameters, parameters.empty() ? "" : ", ", "name");
  strings::StrAppend(&parameters_with_defaults,
                     parameters_with_defaults.empty() ? "" : ", ", "name=None");

  // Add attr_expressions_ for attrs that are params.
  for (int i = 0, end = attrs_.size(); i < end; ++i) {
    const string& attr_name = attrs_[i];
    const string& attr_api_name =
        param_names_[i + op_def_.input_arg_size()].GetRenameTo();
    attr_expressions_[attr_name] = attr_api_name;
  }
  // Add attr_expressions_ for attrs that are inferred.
  for (int i = 0; i < op_def_.attr_size(); ++i) {
    const auto& attr(op_def_.attr(i));
    if (attr.type() == "int") {
      auto arg_list = attr_to_args_.find(attr.name());
      if (arg_list != attr_to_args_.end()) {
        AttrVarName(attr.name(), &attr_expressions_);
      }
    }
  }

  string num_outputs_expr;
  std::vector<string> output_sizes(num_outs_);
  GetOutputSizesAndNumOutputsExpr(&output_sizes, &num_outputs_expr);

  string eager_not_allowed_error = GetEagerNotAllowedError();

  if (!AddEagerFastPathAndGraphCode(parameters_with_defaults, output_sizes,
                                    eager_not_allowed_error,
                                    type_annotations)) {
    return result_;
  }

  if (!AddEagerFallbackCode(parameters, output_sizes, num_outputs_expr,
                            eager_not_allowed_error, type_annotations)) {
    return result_;
  }

  return prelude_ + result_;
}

std::unordered_map<string, string> GenEagerPythonOp::GetTypeAnnotations() {
  std::unordered_map<string, string> type_annotations;
  // Map attrs to TypeVars
  for (const auto& attr : op_def_.attr()) {
    if (attr.type() == "type") {
      const string type_var_name = "TV_" + op_def_.name() + "_" + attr.name();
      type_annotations[attr.name()] = type_var_name;
    } else if (attr.type() == "bool" || attr.type() == "float" ||
               attr.type() == "int" || attr.type() == "bytes") {
      type_annotations[attr.name()] = attr.type();
    } else if (attr.type() == "string") {
      type_annotations[attr.name()] = "str";
    }
  }

  // Map input Tensors to their types
  for (const auto& arg : op_def_.input_arg()) {
    // TODO(rahulkamat): Add type annotations to args that accept a sequence of
    // Tensors
    if (!arg.number_attr().empty() || !arg.type_list_attr().empty()) continue;
    type_annotations[arg.name()] = GetArgAnnotation(arg, type_annotations);
  }

  // TODO(rahulkamat): Add type annotations to handle return types of a sequence
  // of Tensors. Map output Tensor to its type
  if (op_def_.output_arg_size() == 1) {
    const auto& arg = op_def_.output_arg(0);
    if (arg.number_attr().empty() && arg.type_list_attr().empty())
      type_annotations[arg.name()] = GetArgAnnotation(arg, type_annotations);
  }

  return type_annotations;
}

// Generate TypeVars using attrs
void GenEagerPythonOp::GenerateTypeVars(
    const std::unordered_map<string, string>& type_annotations) {
  bool added_typevar = false;
  for (const auto& attr : op_def_.attr()) {
    if (attr.type() == "type") {
      std::vector<string> allowed_types;
      for (int t : attr.allowed_values().list().type()) {
        DataType dtype = static_cast<DataType>(t);
        const string py_dtype =
            python_op_gen_internal::DataTypeToPython(dtype, "_dtypes.");
        allowed_types.emplace_back(dtype_type.at(py_dtype));
      }

      // When a Tensor does not have any dtypes specified, all dtypes are
      // allowed
      if (allowed_types.empty()) {
        for (std::pair<string, string> map_dtype : dtype_type) {
          allowed_types.emplace_back(map_dtype.second);
        }
      }

      std::sort(allowed_types.begin(), allowed_types.end());

      string typevar_dtypes;
      for (std::vector<string>::iterator it = allowed_types.begin();
           it != allowed_types.end(); ++it) {
        if (!typevar_dtypes.empty()) strings::StrAppend(&typevar_dtypes, ", ");
        strings::StrAppend(&typevar_dtypes, *it);
      }

      const string type_var_name = type_annotations.at(attr.name());
      strings::StrAppend(&result_, type_var_name, " = TypeVar(\"",
                         type_var_name, "\", ", typevar_dtypes, ")\n");
      added_typevar = true;
    }
  }

  if (added_typevar) strings::StrAppend(&result_, "\n");
}

void GenEagerPythonOp::AddReturnTypeAnnotation(
    const std::unordered_map<string, string>& type_annotations) {
  if (op_def_.output_arg_size() == 1) {
    const auto& arg = op_def_.output_arg(0);
    if (arg.number_attr().empty() && arg.type_list_attr().empty()) {
      const string return_type = type_annotations.at(arg.name());
      // TODO(rahulkamat): Modify AddDefLine() to add return type annotation to
      // avoid erasing ":\n" from the end of the def line
      result_.erase(result_.length() - 2);
      strings::StrAppend(&result_, " -> ", return_type, ":\n");
    }
  }
}

void GenEagerPythonOp::HandleGraphMode(
    const string& function_setup, const std::vector<string>& output_sizes) {
  if (api_def_.visibility() == ApiDef::VISIBLE) {
    strings::StrAppend(&result_, "  else:\n");
    AddTypeBasedDispatch("    ");
  }
  strings::StrAppend(&result_, "  # Add nodes to the TensorFlow graph.\n");
  strings::StrAppend(&result_, function_setup);
  if (api_def_.visibility() == ApiDef::VISIBLE) {
    strings::StrAppend(&result_, "  try:\n  ");
  }
  strings::StrAppend(
      &result_, "  _, _, _op, _outputs = _op_def_library._apply_op_helper(\n");
  AddBodyNoReturn(strings::StrCat("        \"", op_def_.name(), "\", "));
  AddFallbackDispatch("  ");

  if (num_outs_ > 0) {
    strings::StrAppend(&result_, "  _result = _outputs[:]\n");
    // Special case handling for stateful op with single list output
    // that might be empty.
    if (num_outs_ == 1 && op_def_.is_stateful() &&
        (!op_def_.output_arg(0).number_attr().empty() ||
         !op_def_.output_arg(0).type_list_attr().empty())) {
      // TODO(josh11b): Can skip this if the number_attr/type_list_attr has
      // a constraint indicating that this can never be empty.
      strings::StrAppend(&result_,
                         "  if not _result:\n"
                         "    return _op\n");
    }

    // Compute graph-mode attrs when we need to record a gradient.
    strings::StrAppend(&result_, "  if _execute.must_record_gradient():\n");
    if (op_def_.attr_size() > 0) {
      string attr_values;
      for (int i = 0; i < op_def_.attr_size(); ++i) {
        if (i > 0) strings::StrAppend(&attr_values, ", ");
        const auto& attr_name(op_def_.attr(i).name());
        if (op_def_.attr(i).type() == "type") {
          strings::StrAppend(&attr_values, "\"", attr_name,
                             "\", _op._get_attr_type(\"", attr_name, "\")");
        } else if (op_def_.attr(i).type() == "bool") {
          strings::StrAppend(&attr_values, "\"", attr_name,
                             "\", _op._get_attr_bool(\"", attr_name, "\")");
        } else if (op_def_.attr(i).type() == "int") {
          strings::StrAppend(&attr_values, "\"", attr_name,
                             "\", _op._get_attr_int(\"", attr_name, "\")");
        } else {
          strings::StrAppend(&attr_values, "\"", attr_name,
                             "\", _op.get_attr(\"", attr_name, "\")");
        }
      }
      strings::StrAppend(&attr_values, ")");
      strings::StrAppend(&result_,
                         WordWrap("    _attrs = (", attr_values, kRightMargin),
                         "\n");

    } else {
      strings::StrAppend(&result_, "    _attrs = ()\n");
    }

    strings::StrAppend(&result_, "    _inputs_flat = _op.inputs\n");
    strings::StrAppend(&result_, "    _execute.record_gradient(\n",
                       "        \"", op_def_.name(),
                       "\", _inputs_flat, _attrs, _result)\n");

    if (num_outs_ == 1 && !output_sizes[0].empty()) {
      // Single list result.
    } else if (num_outs_ == 1) {
      // Execute returns a single-element list which we need to destructure.
      strings::StrAppend(&result_, "  ", "_result, = _result\n");
    } else {
      // Have multiple outputs, so we will need to reformat the return
      // value of execute() to be a list with one entry per op output
      // (that entry will be a list of tensors if that output is of list
      // type).
      // For list outputs, convert the right subrange of _result into a list.
      Unflatten("  ", output_sizes, "_result", &result_);
      // Convert to a named tuple.
      strings::StrAppend(
          &result_, "  _result = _",
          python_op_gen_internal::AvoidPythonReserved(op_def_.name()),
          "Output._make(_result)\n");
    }
    strings::StrAppend(&result_, "  return _result\n\n");
  } else {
    strings::StrAppend(&result_, "  return _op\n");
  }
}

string GenEagerPythonOp::GetEagerNotAllowedError() {
  bool eager_allowed = true;
  string ref_arg;
  for (int i = 0; i < op_def_.input_arg_size(); ++i) {
    const auto& arg = op_def_.input_arg(i);
    if (arg.is_ref()) {
      eager_allowed = false;
      DCHECK_EQ(op_def_.input_arg(i).name(), api_def_.in_arg(i).name());
      ref_arg = api_def_.in_arg(i).rename_to();
    }
  }
  for (int i = 0; i < op_def_.output_arg_size(); ++i) {
    const auto& arg = op_def_.output_arg(i);
    if (arg.is_ref()) {
      eager_allowed = false;
      DCHECK_EQ(op_def_.output_arg(i).name(), api_def_.out_arg(i).name());
      ref_arg = api_def_.out_arg(i).rename_to();
    }
  }

  if (eager_allowed) return "";

  return strings::StrCat("raise RuntimeError(\"", op_name_,
                         " op does not support eager execution. ", "Arg '",
                         ref_arg, "' is a ref.\")\n");
}

void GenEagerPythonOp::ExpectListArg(const string& indentation,
                                     const string& arg_name, string* output) {
  strings::StrAppend(output, indentation, "if not isinstance(", arg_name,
                     ", (list, tuple)):\n", indentation, "  raise TypeError(\n",
                     indentation, "      \"Expected list for '", arg_name,
                     "' argument to \"\n", indentation, "      \"'", op_name_,
                     "' Op, not %r.\" % ", arg_name, ")\n");
}

bool GenEagerPythonOp::GetEagerFunctionSetup(const string& indentation,
                                             string* function_setup) {
  // Validate list inputs, infer length attrs.
  for (int i = 0; i < op_def_.attr_size(); ++i) {
    const auto& attr(op_def_.attr(i));
    if (attr.type() == "int") {
      auto arg_list = attr_to_args_.find(attr.name());
      if (arg_list != attr_to_args_.end()) {
        // Inferred int attrs are the lengths of inputs. Validate those
        // inputs are lists and have the same length.
        for (auto iter = arg_list->second.begin();
             iter != arg_list->second.end(); ++iter) {
          const string& arg_api_name = param_names_[*iter].GetRenameTo();
          ExpectListArg(indentation, arg_api_name, function_setup);
          if (iter == arg_list->second.begin()) {
            AddInferredAttr(indentation, attr.name(),
                            strings::StrCat("len(", arg_api_name, ")"),
                            function_setup, &attr_expressions_);
          } else {
            const auto& attr_var = attr_expressions_[attr.name()];
            strings::StrAppend(
                function_setup, indentation, "if len(", arg_api_name,
                ") != ", attr_var, ":\n", indentation, "  raise ValueError(\n",
                indentation, "      \"List argument '", arg_api_name, "' to '",
                op_name_, "' Op with length %d \"\n", indentation,
                "      \"must match length %d of argument '",
                inferred_attrs_[attr.name()], "'.\" %\n", indentation,
                "      (len(", arg_api_name, "), ", attr_var, "))\n");
          }
        }
      }
    }
  }

  for (int i = 0, end = attrs_.size(); i < end; ++i) {
    const string& attr_name = attrs_[i];
    const auto& param = param_names_[i + op_def_.input_arg_size()];
    const auto& attr = *FindAttr(attr_name, op_def_);
    const string& attr_api_name = param.GetRenameTo();
    StringPiece attr_type = attr.type();
    attr_expressions_[attr_name] = attr_api_name;
    const int default_index = i - (attrs_.size() - params_with_default_.size());
    if (default_index >= 0) {
      const string& default_value = params_with_default_[default_index].second;
      strings::StrAppend(function_setup, indentation, "if ", attr_api_name,
                         " is None:\n");
      strings::StrAppend(function_setup, indentation, "  ", attr_api_name,
                         " = ", default_value, "\n");
    }
    if (absl::StartsWith(attr_type, "list(")) {
      ExpectListArg(indentation, attr_api_name, function_setup);
    }

    if (attr_type == "string") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = _execute.make_str(", attr_api_name, ", \"",
                         attr_api_name, "\")\n");
    } else if (attr_type == "list(string)") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = [_execute.make_str(_s, \"", attr_api_name,
                         "\") for _s in ", attr_api_name, "]\n");
    } else if (attr_type == "int") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = _execute.make_int(", attr_api_name, ", \"",
                         attr_api_name, "\")\n");
    } else if (attr_type == "list(int)") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = [_execute.make_int(_i, \"", attr_api_name,
                         "\") for _i in ", attr_api_name, "]\n");
    } else if (attr_type == "float") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = _execute.make_float(", attr_api_name, ", \"",
                         attr_api_name, "\")\n");
    } else if (attr_type == "list(float)") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = [_execute.make_float(_f, \"", attr_api_name,
                         "\") for _f in ", attr_api_name, "]\n");
    } else if (attr_type == "bool") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = _execute.make_bool(", attr_api_name, ", \"",
                         attr_api_name, "\")\n");
    } else if (attr_type == "list(bool)") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = [_execute.make_bool(_b, \"", attr_api_name,
                         "\") for _b in ", attr_api_name, "]\n");
    } else if (attr_type == "type") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = _execute.make_type(", attr_api_name, ", \"",
                         attr_api_name, "\")\n");
    } else if (attr_type == "list(type)") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = [_execute.make_type(_t, \"", attr_api_name,
                         "\") for _t in ", attr_api_name, "]\n");
    } else if (attr_type == "shape") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = _execute.make_shape(", attr_api_name, ", \"",
                         attr_api_name, "\")\n");
    } else if (attr_type == "list(shape)") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = [_execute.make_shape(_s, \"", attr_api_name,
                         "\") for _s in ", attr_api_name, "]\n");
    } else if (attr_type == "tensor") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = _execute.make_tensor(", attr_api_name, ", \"",
                         attr_api_name, "\")\n");
    } else if (attr_type == "list(tensor)") {
      strings::StrAppend(function_setup, indentation, attr_api_name,
                         " = [_execute.make_tensor(_t, \"", attr_api_name,
                         "\") for _t in ", attr_api_name, "]\n");
    } else if (attr_type != "func" && attr_type != "list(func)") {
      *function_setup =
          strings::StrCat("# No definition for ", function_name_,
                          " since we don't support attrs with type\n"
                          "# '",
                          attr_type, "' right now.\n\n");
      return false;
    }
  }
  return true;
}

// If output i is list output, output_sizes[i] will be set to a
// string with the python expression that will evaluate to its
// length. output_sizes[i] is empty for non-list outputs.
void GenEagerPythonOp::GetOutputSizesAndNumOutputsExpr(
    std::vector<string>* output_sizes, string* num_outputs_expr) {
  // Expression representing the number of outputs.
  int num_fixed_outputs = 0;
  for (int i = 0; i < num_outs_; ++i) {
    const auto& arg(op_def_.output_arg(i));
    if (!arg.number_attr().empty()) {
      if (!num_outputs_expr->empty()) {
        strings::StrAppend(num_outputs_expr, " + ");
      }
      (*output_sizes)[i] = attr_expressions_[arg.number_attr()];
      strings::StrAppend(num_outputs_expr, (*output_sizes)[i]);
    } else if (!arg.type_list_attr().empty()) {
      if (!num_outputs_expr->empty()) {
        strings::StrAppend(num_outputs_expr, " + ");
      }
      // Have to be careful to use an expression that works in both
      // graph and eager paths here.
      const auto iter = inferred_attrs_.find(arg.type_list_attr());
      if (iter == inferred_attrs_.end()) {
        (*output_sizes)[i] = strings::StrCat(
            "len(", attr_expressions_[arg.type_list_attr()], ")");
      } else {
        (*output_sizes)[i] = strings::StrCat("len(", iter->second, ")");
      }
      strings::StrAppend(num_outputs_expr, (*output_sizes)[i]);
    } else {
      ++num_fixed_outputs;
    }
  }
  if (num_fixed_outputs > 0) {
    if (!num_outputs_expr->empty()) {
      strings::StrAppend(num_outputs_expr, " + ");
    }
    strings::StrAppend(num_outputs_expr, num_fixed_outputs);
  } else if (num_outputs_expr->empty()) {
    *num_outputs_expr = "0";
  }
}

void GenEagerPythonOp::AddEagerFunctionTeardown(
    const string& indentation, const std::vector<string>& output_sizes,
    bool execute_record_gradient) {
  if (num_outs_ > 0) {
    if (execute_record_gradient) {
      strings::StrAppend(&result_, indentation,
                         "if _execute.must_record_gradient():\n");
      strings::StrAppend(&result_, indentation, "  _execute.record_gradient(\n",
                         "        \"", op_def_.name(),
                         "\", _inputs_flat, _attrs, _result)\n");
    }
    if (num_outs_ == 1 && !output_sizes[0].empty()) {
      // Single list result.
    } else if (num_outs_ == 1) {
      // Execute returns a single-element list which we need to destructure.
      strings::StrAppend(&result_, indentation, "_result, = _result\n");
    } else {
      // Have multiple outputs, so we will need to reformat the return
      // value of execute() to be a list with one entry per op output
      // (that entry will be a list of tensors if that output is of list
      // type).
      // For list outputs, convert the right subrange of _result into a list.
      Unflatten(indentation, output_sizes, "_result", &result_);
      // Convert to a named tuple.
      strings::StrAppend(
          &result_, indentation, "_result = _",
          python_op_gen_internal::AvoidPythonReserved(op_def_.name()),
          "Output._make(_result)\n");
    }
  } else {
    strings::StrAppend(&result_, indentation, "_result = None\n");
  }
  strings::StrAppend(&result_, indentation, "return _result\n\n");
}

bool GenEagerPythonOp::AddEagerFastPathAndGraphCode(
    const string& parameters, const std::vector<string>& output_sizes,
    const string& eager_not_allowed_error,
    const std::unordered_map<string, string>& type_annotations) {
  if (add_type_annotations_) {
    GenerateTypeVars(type_annotations);
  }
  if (api_def_.visibility() == ApiDef::VISIBLE) {
    strings::StrAppend(&result_, "@_dispatch.add_fallback_dispatch_list\n");
    strings::StrAppend(&result_, "@_dispatch.add_type_based_api_dispatcher\n");
  }

  AddExport();
  AddDefLine(function_name_, parameters);
  if (add_type_annotations_) {
    AddReturnTypeAnnotation(type_annotations);
  }
  AddDocStringDescription();
  AddDocStringArgs();
  AddDocStringInputs();
  AddDocStringAttrs();
  AddDocStringNameArg();
  AddOutputGlobals();  // Added to prelude_
  AddDocStringOutputs();
  strings::StrAppend(&result_, "  \"\"\"\n");

  strings::StrAppend(&result_,
                     "  _ctx = _context._context or _context.context()\n"
                     "  tld = _ctx._thread_local_data\n",
                     "  if tld.is_eager:", "\n");
  if (eager_not_allowed_error.empty()) {
    AddEagerFastPathExecute();
  } else {
    strings::StrAppend(&result_, "    ", eager_not_allowed_error);
  }

  // Handle graph-mode case
  string function_setup;
  if (!GetEagerFunctionSetup("  ", &function_setup)) {
    result_ = function_setup;
    return false;
  }
  HandleGraphMode(function_setup, output_sizes);

  AddRawOpExport(parameters);
  AddTypeBasedDispatcherAlias();
  strings::StrAppend(&result_, "\n\n");
  return true;
}

bool GenEagerPythonOp::AddEagerFallbackCode(
    const string& parameters, const std::vector<string>& output_sizes,
    const string& num_outputs_expr, const string& eager_not_allowed_error,
    const std::unordered_map<string, string>& type_annotations) {
  AddDefLine(
      strings::StrCat(function_name_, kEagerFallbackSuffix),
      strings::StrCat(parameters, parameters.empty() ? "" : ", ", "ctx"));
  if (add_type_annotations_) {
    AddReturnTypeAnnotation(type_annotations);
  }
  if (!eager_not_allowed_error.empty()) {
    strings::StrAppend(&result_, "  ", eager_not_allowed_error);
    return true;
  }

  string function_setup;
  if (!GetEagerFunctionSetup("  ", &function_setup)) {
    result_ = function_setup;
    return false;
  }
  strings::StrAppend(&result_, function_setup);

  AddEagerInferredAttrs("  ");
  AddEagerInputCasts("  ");
  strings::StrAppend(
      &result_, "  _inputs_flat = ", FlattenInputs(nullptr, nullptr), "\n");
  AddEagerAttrs("  ");
  AddEagerExecute("  ", num_outputs_expr);

  AddEagerFunctionTeardown("  ", output_sizes,
                           true /* execute_record_gradient */);

  return true;
}

void GenEagerPythonOp::AddEagerFastPathExecute() {
  string fastpath_execute_params =
      strings::StrCat("_ctx, \"", op_def_.name(), "\", ", "name");
  string fallback_params;

  for (int i = 0; i < api_def_.in_arg_size(); i++) {
    const string param_name = param_names_[i].GetRenameTo();
    strings::StrAppend(&fastpath_execute_params, ", ", param_name);
    if (!fallback_params.empty()) strings::StrAppend(&fallback_params, ", ");
    strings::StrAppend(&fallback_params, param_name);
  }

  for (const auto& attr : api_def_.attr()) {
    if (inferred_attrs_.find(attr.name()) == inferred_attrs_.end()) {
      strings::StrAppend(&fastpath_execute_params, ", \"", attr.name(), "\", ",
                         attr.rename_to());

      if (!fallback_params.empty()) strings::StrAppend(&fallback_params, ", ");
      strings::StrAppend(&fallback_params, attr.rename_to(), "=",
                         attr.rename_to());
    }
  }

  if (!fallback_params.empty()) strings::StrAppend(&fallback_params, ", ");
  strings::StrAppend(&fallback_params, "name=name");

  strings::StrAppend(&result_, "    try:\n");
  strings::StrAppend(
      &result_, "      ", "_result = pywrap_tfe.TFE_Py_FastPathExecute(\n",
      WordWrap(strings::StrCat("        "),
               strings::StrCat(fastpath_execute_params, ")"), kRightMargin),
      "\n");

  if (op_def_.output_arg_size() > 1) {
    const string output_tuple_name = strings::StrCat(
        "_", python_op_gen_internal::AvoidPythonReserved(op_def_.name()),
        "Output");
    strings::StrAppend(&result_, "      ", "_result = ", output_tuple_name,
                       "._make(_result)\n");
  }
  strings::StrAppend(&result_, "      ", "return _result\n");

  // Handle fallback.
  if (!fallback_params.empty()) strings::StrAppend(&fallback_params, ", ");
  strings::StrAppend(&fallback_params, "ctx=_ctx");

  // Any errors thrown from execute need to be unwrapped from
  // _NotOkStatusException.
  strings::StrAppend(&result_, "    ",
                     "except _core._NotOkStatusException as e:\n");
  strings::StrAppend(&result_, "      ",
                     "_ops.raise_from_not_ok_status(e, name)\n");

  strings::StrAppend(&result_, "    ", "except _core._FallbackException:\n");
  strings::StrAppend(&result_, "      pass\n");
  strings::StrAppend(&result_, "    try:\n");
  AddTypeBasedDispatch("      ");
  strings::StrAppend(
      &result_, "      ", "return ", function_name_, kEagerFallbackSuffix,
      "(\n",
      WordWrap(strings::StrCat("          "),
               strings::StrCat(fallback_params, ")"), kRightMargin),
      "\n");
  strings::StrAppend(&result_, "    except _core._SymbolicException:\n");
  strings::StrAppend(&result_,
                     "      pass  # Add nodes to the TensorFlow graph.\n");
  AddFallbackDispatch("    ");
}

void GenEagerPythonOp::AddEagerInferredAttrs(const string& indentation) {
  // Figure out values for inferred attrs, and cast to eager tensors.
  for (int i = 0; i < op_def_.attr_size(); ++i) {
    const auto& attr(op_def_.attr(i));
    const auto& api_def_attr(api_def_.attr(i));
    auto arg_list = attr_to_args_.find(attr.name());
    if (arg_list != attr_to_args_.end()) {
      if (attr.type() == "type") {
        std::vector<string> output_sizes;
        const string flattened =
            FlattenInputs(&arg_list->second, &output_sizes);
        string conversion = strings::StrCat("_execute.args_to_matching_eager(",
                                            flattened, ", ctx");

        strings::StrAppend(&conversion, ", [");
        for (int t : attr.allowed_values().list().type()) {
          DataType dtype = static_cast<DataType>(t);
          const string py_dtype =
              python_op_gen_internal::DataTypeToPython(dtype, "_dtypes.");
          strings::StrAppend(&conversion, py_dtype, ", ");
        }
        strings::StrAppend(&conversion, "]");

        if (attr.has_default_value()) {
          strings::StrAppend(
              &conversion, ", ",
              python_op_gen_internal::AttrValueToPython(
                  attr.type(), api_def_attr.default_value(), "_dtypes."));
        }
        strings::StrAppend(&conversion, ")");
        const string var_name = AttrVarName(attr.name(), &attr_expressions_);
        if (output_sizes.size() == 1) {
          // Avoid creating a temporary variable in the case where
          // we can easily assign to the right value directly.
          const string inputs_var =
              param_names_[arg_list->second.front()].GetRenameTo();
          if (output_sizes.front().empty()) {
            strings::StrAppend(&result_, indentation, var_name, ", (",
                               inputs_var, ",) = ", conversion, "\n");
          } else {
            strings::StrAppend(&result_, indentation, var_name, ", ",
                               inputs_var, " = ", conversion, "\n");
          }
        } else {
          const string inputs_var = strings::StrCat("_inputs_", attr.name());
          strings::StrAppend(&result_, indentation, var_name, ", ", inputs_var,
                             " = ", conversion, "\n");
          // Convert from a flat list of eager tensors back to the
          // parameter variables.
          Unflatten(indentation, output_sizes, inputs_var, &result_);
          std::vector<string> p;
          for (int j : arg_list->second) {
            p.emplace_back(param_names_[j].GetRenameTo());
          }
          strings::StrAppend(&result_, indentation, VectorToTuple(p), " = ",
                             inputs_var, "\n");
        }
      } else if (attr.type() == "list(type)") {
        // NOTE: We ignore default values for these attrs, since it is
        // unclear how you would use it, and the one use case is
        // parse_single_sequence_example which only needs it for
        // backwards compatibility.
        const string var_name = AttrVarName(attr.name(), &attr_expressions_);
        string inputs_var;
        string conversion;
        if (arg_list->second.size() > 1) {
          // If you have more than one list(tensor) argument, their types
          // have to match.
          std::vector<string> lists;
          for (auto iter = arg_list->second.begin();
               iter != arg_list->second.end(); ++iter) {
            lists.push_back(param_names_[*iter].GetRenameTo());
          }
          inputs_var = VectorToTuple(lists);
          conversion = "_execute.args_to_mixed_eager_tensors";
        } else {
          // For one list(tensor) argument, we just convert every
          // element of the list to an eager tensor.
          inputs_var = param_names_[arg_list->second.front()].GetRenameTo();
          conversion = "_execute.convert_to_mixed_eager_tensors";
        }
        strings::StrAppend(&result_, indentation, var_name, ", ", inputs_var,
                           " = ", conversion, "(", inputs_var, ", ctx)\n");
      }
    }
  }
}

void GenEagerPythonOp::AddEagerInputCasts(const string& indentation) {
  // Cast remaining args to eager tensors
  for (int i = 0; i < op_def_.input_arg_size(); ++i) {
    const auto& arg(op_def_.input_arg(i));
    if (!arg.type_attr().empty() || !arg.type_list_attr().empty()) continue;
    const string& param = param_names_[i].GetRenameTo();
    const string fn = arg.number_attr().empty() ? "" : "n_";
    const string dtype =
        python_op_gen_internal::DataTypeToPython(arg.type(), "_dtypes.");
    strings::StrAppend(&result_, indentation, param, " = _ops.convert_", fn,
                       "to_tensor(", param, ", ", dtype, ")\n");
  }
}

void GenEagerPythonOp::AddEagerAttrs(const string& indentation) {
  // Compute eager attrs
  if (op_def_.attr_size() > 0) {
    string attr_values;
    for (int i = 0; i < op_def_.attr_size(); ++i) {
      if (i > 0) strings::StrAppend(&attr_values, ", ");
      const auto& attr_name(op_def_.attr(i).name());
      strings::StrAppend(&attr_values, "\"", attr_name, "\", ",
                         attr_expressions_[attr_name]);
    }
    strings::StrAppend(&attr_values, ")");
    strings::StrAppend(
        &result_,
        WordWrap(indentation, strings::StrCat("_attrs = (", attr_values),
                 kRightMargin),
        "\n");
  } else {
    strings::StrAppend(&result_, indentation, "_attrs = None\n");
  }
}

void GenEagerPythonOp::AddEagerExecute(const string& indentation,
                                       const string& num_outputs_expr) {
  const string return_prefix =
      strings::StrCat(indentation, "_result = _execute.execute(");
  const string return_args = strings::StrCat(
      "b\"", op_def_.name(), "\", ", num_outputs_expr,
      ", inputs=_inputs_flat, attrs=_attrs, ctx=ctx, name=name)");
  strings::StrAppend(&result_,
                     // Wrap the arguments, and indent to the (.
                     WordWrap(return_prefix, return_args, kRightMargin), "\n");
}

void GenEagerPythonOp::AddFallbackDispatch(const string& prefix) {
  if (api_def_.visibility() != ApiDef::VISIBLE) return;

  strings::StrAppend(&result_, prefix, "except (TypeError, ValueError):\n");
  strings::StrAppend(&result_, prefix, "  _result = _dispatch.dispatch(\n");
  AddBodyNoReturn(strings::StrCat(prefix, "        ", function_name_,
                                  ", "
                                  "(), dict("));
  strings::StrAppend(&result_, prefix, "      )\n");
  strings::StrAppend(&result_, prefix,
                     "  if _result is not "
                     "_dispatch.OpDispatcher.NOT_SUPPORTED:\n");
  strings::StrAppend(&result_, prefix, "    return _result\n");
  strings::StrAppend(&result_, prefix, "  raise\n");
}

void GenEagerPythonOp::AddTypeBasedDispatcherAlias() {
  // It's possible for the name of a parameter to be the same as the name of
  // an op, in which case the parameter shadows the op's function.  To avoid
  // this, we add a private variable with the dispatcher, and access that
  // directly.
  if (api_def_.visibility() == ApiDef::VISIBLE) {
    strings::StrAppend(&result_, "_dispatcher_for_", function_name_, " = ",
                       function_name_, "._tf_type_based_dispatcher.Dispatch\n");
  }
}
void GenEagerPythonOp::AddTypeBasedDispatch(const string& prefix) {
  if (api_def_.visibility() != ApiDef::VISIBLE) return;
  std::string args("(");
  for (const auto& name : param_names_) {
    strings::StrAppend(&args, name.GetRenameTo(), ", ");
  }
  strings::StrAppend(&args, "name,), None");

  strings::StrAppend(
      &result_, prefix, "_result = ", "_dispatcher_for_", function_name_, "(\n",
      WordWrap(strings::StrCat(prefix, "    "), args, kRightMargin), ")\n");
  strings::StrAppend(&result_, prefix, "if _result is not NotImplemented:\n",
                     prefix, "  return _result\n");
}

void GenEagerPythonOp::AddRawOpExport(const string& parameters) {
  // Example:
  //
  // Identity = tf_export("raw_ops.Identity")(_ops._to_raw_op(identity))
  const string raw_function_name =
      python_op_gen_internal::AvoidPythonReserved(op_def_.name());
  strings::StrAppend(&result_, raw_function_name, " = tf_export(\"raw_ops.",
                     raw_function_name, "\")", "(_ops.to_raw_op(",
                     function_name_, "))\n");
}

string GetPythonOpsImpl(
    const OpList& ops, const ApiDefMap& api_defs,
    absl::Span<const string> hidden_ops,
    absl::Span<const string> source_file_list,
    const std::unordered_set<string>& type_annotate_ops = {}) {
  string result;
  // Header
  // TODO(josh11b): Mention the library for which wrappers are being generated.
  strings::StrAppend(&result, R"("""Python wrappers around TensorFlow ops.

This file is MACHINE GENERATED! Do not edit.
)");

  // Mention the original source file so someone tracing back through
  // generated Python code will know where to look next.
  if (!source_file_list.empty()) {
    strings::StrAppend(&result, "Original C++ source file: ");
    strings::StrAppend(&result, absl::StrJoin(source_file_list, ", "));
    strings::StrAppend(&result, "\n");
  }

  strings::StrAppend(&result, R"("""

import collections

from tensorflow.python import pywrap_tfe as pywrap_tfe
from tensorflow.python.eager import context as _context
from tensorflow.python.eager import core as _core
from tensorflow.python.eager import execute as _execute
from tensorflow.python.framework import dtypes as _dtypes

from tensorflow.python.framework import op_def_registry as _op_def_registry
from tensorflow.python.framework import ops as _ops
from tensorflow.python.framework import op_def_library as _op_def_library
from tensorflow.python.util.deprecation import deprecated_endpoints
from tensorflow.python.util import dispatch as _dispatch
from tensorflow.python.util.tf_export import tf_export

from typing import TypeVar
)");

  for (const auto& op_def : ops.op()) {
    const auto* api_def = api_defs.GetApiDef(op_def.name());

    if (api_def->visibility() == ApiDef::SKIP) {
      continue;
    }
    // An op is hidden if either its ApiDef visibility is HIDDEN
    // or it is in the hidden_ops list.
    bool is_hidden = api_def->visibility() == ApiDef::HIDDEN;
    bool hidden_by_api_def = is_hidden;
    if (!is_hidden) {
      for (const string& hidden : hidden_ops) {
        if (op_def.name() == hidden) {
          is_hidden = true;
          break;
        }
      }
    }

    string function_name;
    python_op_gen_internal::GenerateLowerCaseOpName(op_def.name(),
                                                    &function_name);
    bool is_reserved = python_op_gen_internal::IsPythonReserved(function_name);

    // Prefix an op with underscore if the op is listed in hidden_ops or
    // name is reserved or it is of the exceptions in IsOpWithUnderscorePrefix.
    // Do not add underscores to ops set to HIDDEN in ApiDef otherwise.
    // TODO(annarev): don't prefix with underscores even if op is in hidden_ops.
    if (is_hidden) {
      if (!hidden_by_api_def || is_reserved ||
          python_op_gen_internal::IsOpWithUnderscorePrefix(function_name)) {
        function_name = strings::StrCat("_", function_name);
      }
    } else if (is_reserved) {
      // When users create custom python wrappers, they may link in the
      // default op registry by accident, and because they can't
      // enumerate all 'hidden' symbols, this guard is to prevent
      // instantiating a python reserved word in their wrapper.
      continue;
    }

    auto iter = type_annotate_ops.find(op_def.name());
    bool add_type_annotations = iter != type_annotate_ops.end();

    strings::StrAppend(&result,
                       GetEagerPythonOp(op_def, *api_def, function_name,
                                        add_type_annotations));
  }

  return result;
}

}  // namespace

string GetPythonOps(const OpList& ops, const ApiDefMap& api_defs,
                    absl::Span<const string> hidden_ops,
                    absl::Span<const string> source_file_list,
                    const std::unordered_set<string>& type_annotate_ops) {
  return GetPythonOpsImpl(ops, api_defs, hidden_ops, source_file_list,
                          type_annotate_ops);
}

void PrintPythonOps(const OpList& ops, const ApiDefMap& api_defs,
                    absl::Span<const string> hidden_ops,
                    absl::Span<const string> source_file_list,
                    const std::unordered_set<string>& type_annotate_ops) {
  printf("%s", GetPythonOpsImpl(ops, api_defs, hidden_ops, source_file_list,
                                type_annotate_ops)
                   .c_str());
}

string GetPythonWrappers(const char* op_list_buf, size_t op_list_len) {
  OpList ops;
  ops.ParseFromArray(op_list_buf, op_list_len);

  ApiDefMap api_def_map(ops);
  return GetPythonOpsImpl(ops, api_def_map, {}, {});
}

string GetArgAnnotation(
    const OpDef::ArgDef& arg,
    const std::unordered_map<string, string>& type_annotations) {
  if (!arg.type_attr().empty()) {
    // Get the correct TypeVar if arg maps to an attr
    return "_ops.Tensor[" + type_annotations.at(arg.type_attr()) + "]";
  } else {
    // Get the dtype of the Tensor
    const string py_dtype =
        python_op_gen_internal::DataTypeToPython(arg.type(), "_dtypes.");
    return "_ops.Tensor[" + dtype_type.at(py_dtype) + "]";
  }

  return "Any";
}

}  // namespace tensorflow
