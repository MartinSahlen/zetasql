//
// Copyright 2019 ZetaSQL Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/testdata/sample_system_variables.h"

#include <vector>

#include "zetasql/public/analyzer.h"
#include "zetasql/public/type.h"
#include "zetasql/testdata/test_schema.pb.h"
#include "absl/strings/str_join.h"
#include "zetasql/base/status.h"

namespace zetasql {

static zetasql_base::Status AddSystemVariable(const std::vector<std::string>& name_path,
                                      const Type* type,
                                      AnalyzerOptions* options) {
  if (!type->IsSupportedType(options->language_options())) {
    // Skip system variable whose type is not supported by the language.  This
    // is to prevent tests which use a reduced feature set (which don't test
    // system variables) from being blocked by failure to set them.
    LOG(INFO) << "Skipping system variable " << absl::StrJoin(name_path, ".")
        << " due to unsupported type: " << type->DebugString();
    return zetasql_base::OkStatus();
  }
    LOG(INFO) << "Adding system variable " << absl::StrJoin(name_path, ".")
        << " of type: " << type->DebugString();
  return options->AddSystemVariable(name_path, type);
}

void SetupSampleSystemVariables(TypeFactory* type_factory,
                                AnalyzerOptions* options) {
  const Type* int32_type = type_factory->get_int32();
  const Type* int64_type = type_factory->get_int64();
  const Type* string_type = type_factory->get_string();

  const StructType* struct_type;
  const StructType* nested_struct_type;
  const StructType* foo_struct_type;
  const ProtoType* proto_type;
  ZETASQL_CHECK_OK(type_factory->MakeStructType({{"a", int32_type}, {"b", string_type}},
                                        &struct_type));
  ZETASQL_CHECK_OK(type_factory->MakeStructType({{"c", int32_type}, {"d", struct_type}},
                                        &nested_struct_type));
  ZETASQL_CHECK_OK(
      type_factory->MakeStructType({{"bar", int32_type}}, &foo_struct_type));
  ZETASQL_CHECK_OK(type_factory->MakeProtoType(
      zetasql_test::KitchenSinkPB::descriptor(), &proto_type));

  ZETASQL_CHECK_OK(AddSystemVariable({"int64_system_variable"}, int64_type, options));
  ZETASQL_CHECK_OK(AddSystemVariable({"struct_system_variable"}, nested_struct_type,
                             options));
  ZETASQL_CHECK_OK(AddSystemVariable({"sysvar_foo"}, foo_struct_type, options));
  ZETASQL_CHECK_OK(AddSystemVariable({"sysvar_foo", "bar"}, int64_type, options));
  ZETASQL_CHECK_OK(
      AddSystemVariable({"sysvar_foo", "bar", "baz"}, int64_type, options));
  ZETASQL_CHECK_OK(AddSystemVariable({"proto_system_variable"}, proto_type, options));
  ZETASQL_CHECK_OK(AddSystemVariable({"error", "message"}, string_type, options));
  ZETASQL_CHECK_OK(AddSystemVariable({"sysvar.with.dots"}, string_type, options));
  ZETASQL_CHECK_OK(AddSystemVariable({"namespace.with.dots", "sysvar"}, string_type,
                             options));
  ZETASQL_CHECK_OK(AddSystemVariable({"sysvar.part1.part2"}, string_type, options));
  ZETASQL_CHECK_OK(
      AddSystemVariable({"sysvar", "part1", "part2"}, string_type, options));
}
}  // namespace zetasql
