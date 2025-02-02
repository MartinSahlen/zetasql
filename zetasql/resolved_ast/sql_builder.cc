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

// SQLBuilder principles.
//
// - The ResolvedTree is traversed depth first and processed bottom up.
//
// - As we traverse the tree, for each ResolvedNode, we return a QueryFragment.
//   A QueryFragment is either a std::string (of SQL text) or a QueryExpression. A
//   QueryExpression is a data structure representing a partial query, with
//   fields for various clauses (select, from, where, ...) filled in as strings.
//
// - Each call to Visit will return a new QueryFragment. As the Accept/Visit
//   methods cannot return a value, we maintain a QueryFragment stack that
//   parallels the call stack. Each call to Visit will push its return value
//   onto that stack, and each caller of Accept will pop that return value of
//   that stack. This is encapsulated inside PushQueryFragment and ProcessNode.
//
// - When visiting a Scan or Statement node we build its corresponding
//   QueryExpression and push that as part of the returned QueryFragment. While
//   building a QueryExpression we can either fill something onto an existing
//   QueryExpression (e.g. filling a where clause) or wrap the QueryExpression
//   produced by the input scan (if any). Refer to QueryExpression::Wrap().

#include "zetasql/resolved_ast/sql_builder.h"

#include <algorithm>
#include <set>
#include <type_traits>

#include "zetasql/base/atomic_sequence_num.h"
#include "zetasql/base/logging.h"
#include "google/protobuf/descriptor.h"
#include "zetasql/public/analyzer.h"
#include "zetasql/public/builtin_function.pb.h"
#include "zetasql/public/catalog.h"
#include "zetasql/public/constant.h"
#include "zetasql/public/function.h"
#include "zetasql/public/functions/date_time_util.h"
#include "zetasql/public/functions/datetime.pb.h"
#include "zetasql/public/functions/normalize_mode.pb.h"
#include "zetasql/public/procedure.h"
#include "zetasql/public/strings.h"
#include "zetasql/public/table_valued_function.h"
#include "zetasql/resolved_ast/resolved_ast.h"
#include "zetasql/resolved_ast/resolved_ast_enums.pb.h"
#include "zetasql/resolved_ast/resolved_node_kind.pb.h"
#include "absl/memory/memory.h"
#include "absl/strings/ascii.h"
#include "zetasql/base/case.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "zetasql/base/map_util.h"
#include "zetasql/base/source_location.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

// Commonly used SQL keywords.
static const char kFrom[] = " FROM ";

// Used as a "magic alias" in some places.
static const char kEmptyAlias[] = "``";

std::string SQLBuilder::GetColumnPath(const ResolvedColumn& column) {
  if (zetasql_base::ContainsKey(column_paths_, column.column_id())) {
    return zetasql_base::FindOrDie(column_paths_, column.column_id());
  }
  return ToIdentifierLiteral(GetColumnAlias(column));
}

std::string SQLBuilder::GetColumnAlias(const ResolvedColumn& column) {
  if (zetasql_base::ContainsKey(computed_column_alias_, column.column_id())) {
    return zetasql_base::FindOrDie(computed_column_alias_, column.column_id());
  }

  const std::string alias = absl::StrCat("a_", GetUniqueId());
  zetasql_base::InsertOrDie(&computed_column_alias_, column.column_id(), alias);
  return alias;
}

std::string SQLBuilder::UpdateColumnAlias(const ResolvedColumn& column) {
  auto it = computed_column_alias_.find(column.column_id());
  CHECK(it != computed_column_alias_.end())
      << "Alias does not exist for " << column.DebugString();
  computed_column_alias_.erase(it);
  return GetColumnAlias(column);
}

SQLBuilder::SQLBuilder(const SQLBuilderOptions& options) : options_(options) {}

namespace {
// In some cases ZetaSQL name resolution rules cause table names to be
// resolved as column names. See the test in sql_builder.test that is tagged
// "resolution_conflict" for an example. Currently this only happens in
// flattend FilterScans.
//
// To avoid such conflicts, process the AST in two passes. First, collect all
// column names into a set. Then, while generating SQL, create aliases for all
// tables whose names are in the collected set.  The columns are obtained from
// two places:
//   1. all columns of tables from ResolvedTableScan
//   2. all resolved columns elsewhere in the query.
// The latter is probably not necessary, but can't hurt. One might ask why we
// bother with this instead of generating aliases for all tables. We need to
// collect the set of column names to do that too, otherwise generated aliases
// might conflict with column names. And since we have to collect the set, we
// might as well use it to only generate aliases when needed.
class ColumnNameCollector : public ResolvedASTVisitor {
 public:
  explicit ColumnNameCollector(absl::flat_hash_set<std::string>* col_ref_names)
      : col_ref_names_(col_ref_names) {}

 private:
  void Register(const std::string& col_name) {
    std::string name = absl::AsciiStrToLower(col_name);
    // Value tables do not currently participate in FilterScan flattening, so
    // avoid complexities and don't worry about refs to them.
    if (!zetasql_base::ContainsKey(value_table_names_, name)) {
      col_ref_names_->insert(name);
    }
  }
  zetasql_base::Status VisitResolvedColumnRef(const ResolvedColumnRef* node) override {
    Register(node->column().name());
    return zetasql_base::OkStatus();
  }

  zetasql_base::Status VisitResolvedTableScan(const ResolvedTableScan* node) override {
    const Table* t = node->table();
    if (t->IsValueTable()) {
      value_table_names_.insert(absl::AsciiStrToLower(node->table()->Name()));
    }
    for (int i = 0; i < t->NumColumns(); i++) {
      Register(t->GetColumn(i)->Name());
    }
    return zetasql_base::OkStatus();
  }

  // Default implementation for ProjectScan visits expr list before input
  // scan (al others look at input_scan first) which prevents us from seeing
  // Value tables before ColumnRefs. Our version visits input_scan first.
  zetasql_base::Status VisitResolvedProjectScan(
      const ResolvedProjectScan* node) override {
    if (node->input_scan() != nullptr) {
      ZETASQL_RETURN_IF_ERROR(node->input_scan()->Accept(this));
    }
    for (const auto& elem : node->expr_list()) {
      ZETASQL_RETURN_IF_ERROR(elem->Accept(this));
    }
    return zetasql_base::OkStatus();
  }

  absl::flat_hash_set<std::string>* const col_ref_names_;
  absl::flat_hash_set<std::string> value_table_names_;
};
}  // namespace

zetasql_base::Status SQLBuilder::Process(const ResolvedNode& ast) {
  ast.ClearFieldsAccessed();
  ColumnNameCollector name_collector(&col_ref_names_);
  zetasql_base::Status s = ast.Accept(&name_collector);
  if (!s.ok()) {
    return s;
  }
  ast.ClearFieldsAccessed();
  return ast.Accept(this);
}

std::string SQLBuilder::QueryFragment::GetSQL() const {
  if (query_expression != nullptr) {
    // At this stage all QueryExpression parts (SELECT list, FROM clause,
    // etc.) have already had std::string fragments generated for them, so this
    // step of concatenating all the std::string pieces is not sensitive to
    // ProductMode.
    return query_expression->GetSQLQuery();
  }
  return text;
}

SQLBuilder::~SQLBuilder() {}

void SQLBuilder::DumpQueryFragmentStack() {
}

void SQLBuilder::PushQueryFragment(
    std::unique_ptr<QueryFragment> query_fragment) {
  DCHECK(query_fragment != nullptr);
  // We generate sql_ at once in the end, i.e. after the complete traversal of
  // the given ResolvedAST. Whenever a new QueryFragment is pushed, that means
  // we would need to incorporate it in the final sql which renders any
  // previously generated sql irrelevant.
  sql_.clear();
  query_fragments_.push_back(std::move(query_fragment));
}

void SQLBuilder::PushQueryFragment(const ResolvedNode* node,
                                   const std::string& text) {
  PushQueryFragment(absl::make_unique<QueryFragment>(node, text));
}

void SQLBuilder::PushQueryFragment(const ResolvedNode* node,
                                   QueryExpression* query_expression) {
  PushQueryFragment(absl::make_unique<QueryFragment>(node, query_expression));
}

std::unique_ptr<SQLBuilder::QueryFragment> SQLBuilder::PopQueryFragment() {
  DCHECK(!query_fragments_.empty());
  std::unique_ptr<SQLBuilder::QueryFragment> f =
      std::move(query_fragments_.back());
  query_fragments_.pop_back();
  return f;
}

zetasql_base::StatusOr<std::unique_ptr<SQLBuilder::QueryFragment>>
SQLBuilder::ProcessNode(const ResolvedNode* node) {
  ZETASQL_RET_CHECK(node != nullptr);
  ZETASQL_RETURN_IF_ERROR(node->Accept(this));
  ZETASQL_RET_CHECK_EQ(query_fragments_.size(), 1);
  return PopQueryFragment();
}

static std::string AddExplicitCast(const std::string& sql, const Type* type,
                              ProductMode mode) {
  return absl::StrCat("CAST(", sql, " AS ", type->TypeName(mode), ")");
}

// Usually we add explicit casts to ensure that typed literals match their
// resolved type.  But <is_constant_value> is a hack to work around a
// limitation in the run_analyzer_test infrastructure.  This hack is necessary
// because the run_analyzer_test infrastructure doesn't know how to properly
// compare the 'shapes' of query OPTIONS/HINTS when the original query
// option is a constant while the unparsed query options is a CAST expression.
// If we resolve the run_analyzer_test infrastructure problem, we could
// remove <is_constant_value> from here.
zetasql_base::StatusOr<std::string> SQLBuilder::GetSQL(const Value& value,
                                          ProductMode mode,
                                          bool is_constant_value) {
  const Type* type = value.type();

  if (value.is_null()) {
    if (is_constant_value) {
      // To handle NULL literals in ResolvedOption, where we would not want to
      // print them as a casted literal.
      return std::string("NULL");
    }
    return value.GetSQL(mode);
  }

  if (type->IsTimestamp() || type->IsCivilDateOrTimeType()) {
    if (is_constant_value) {
      return ToStringLiteral(value.DebugString());
    }
    return value.GetSQL(mode);
  }

  if (type->IsSimpleType()) {
    // To handle simple types, where we would not want to print them as cast
    // expressions.
    if (is_constant_value) {
      return value.DebugString();
    }
    return value.GetSQL(mode);
  }

  if (type->IsEnum()) {
    // Special cases to handle DateTimePart and NormalizeMode enums which are
    // treated as identifiers or expressions in the parser (not literals).
    const std::string& enum_full_name =
        type->AsEnum()->enum_descriptor()->full_name();
    if (enum_full_name ==
        functions::DateTimestampPart_descriptor()->full_name()) {
      return std::string(functions::DateTimestampPartToSQL(value.enum_value()));
    }
    if (enum_full_name == functions::NormalizeMode_descriptor()->full_name()) {
      return ToIdentifierLiteral(value.DebugString());
    }

    // For typed hints, we can't print a CAST for enums because the parser
    // won't accept it back in.
    if (is_constant_value) {
      return ToStringLiteral(value.DebugString());
    }
    return value.GetSQL(mode);
  }
  if (type->IsProto()) {
    // Cannot use Value::GetSQL() for proto types as it returns bytes (casted as
    // proto) instead of strings.
    // TODO: May have an issue here with EXTERNAL mode.
    const std::string proto_str = value.DebugString();
    // Strip off the curly braces encapsulating the proto value, so that it
    // could be recognized as a std::string literal in the unparsed sql, which could
    // be coerced to a proto literal later.
    ZETASQL_RET_CHECK_GT(proto_str.size(), 1);
    ZETASQL_RET_CHECK_EQ(proto_str[0], '{');
    ZETASQL_RET_CHECK_EQ(proto_str[proto_str.size() - 1], '}');
    const std::string literal_str =
        ToStringLiteral(proto_str.substr(1, proto_str.size() - 2));
    // For typed hints, we can't print a CAST for protos because the parser
    // won't accept it back in.
    if (is_constant_value) {
      return literal_str;
    }
    return AddExplicitCast(literal_str, type, mode);
  }
  if (type->IsStruct()) {
    // Once STRUCT<...>(...) syntax is supported in hints, and CASTs work in
    // hints, we could just use GetSQL always.
    const StructType* struct_type = type->AsStruct();
    std::vector<std::string> fields_sql;
    for (const auto& field_value : value.fields()) {
      ZETASQL_ASSIGN_OR_RETURN(const std::string result,
                       GetSQL(field_value, mode, is_constant_value));
      fields_sql.push_back(result);
    }
    // If any of the fields have names (are not anonymous) then we need to add
    // them in the returned SQL.
    std::vector<std::string> field_types;
    bool has_explicit_field_name = false;
    for (const StructField& field_type : type->AsStruct()->fields()) {
      std::string field_name;
      if (!field_type.name.empty()) {
        has_explicit_field_name = true;
        field_name = absl::StrCat(field_type.name, " ");
      }
      field_types.push_back(
          absl::StrCat(field_name, field_type.type->TypeName(mode)));
    }
    DCHECK_EQ(type->AsStruct()->num_fields(), fields_sql.size());
    if (has_explicit_field_name) {
      return absl::StrCat("STRUCT<", absl::StrJoin(field_types, ", "), ">(",
                          absl::StrJoin(fields_sql, ", "), ")");
    }
    return absl::StrCat(struct_type->TypeName(mode), "(",
                        absl::StrJoin(fields_sql, ", "), ")");
  }
  if (type->IsArray()) {
    std::vector<std::string> elements_sql;
    for (const auto& elem : value.elements()) {
      ZETASQL_ASSIGN_OR_RETURN(const std::string result,
                       GetSQL(elem, mode, is_constant_value));
      elements_sql.push_back(result);
    }
    return absl::StrCat(type->TypeName(mode), "[",
                        absl::StrJoin(elements_sql, ", "), "]");
  }

  return ::zetasql_base::InvalidArgumentErrorBuilder()
         << "Value has unknown type: " << type->DebugString();
}

zetasql_base::Status SQLBuilder::VisitResolvedExpressionColumn(
    const ResolvedExpressionColumn* node) {
  PushQueryFragment(node, ToIdentifierLiteral(node->name()));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedLiteral(const ResolvedLiteral* node) {
  ZETASQL_ASSIGN_OR_RETURN(const std::string result, GetSQL(node->value(),
                                               options_.product_mode));
  PushQueryFragment(node, result);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedConstant(const ResolvedConstant* node) {
  PushQueryFragment(
      node, absl::StrJoin(node->constant()->name_path(), ".",
                          [](std::string* out, const std::string& part) {
                            absl::StrAppend(out, ToIdentifierLiteral(part));
                          }));
  return ::zetasql_base::OkStatus();
}

// Call Function::GetSQL(), and also add the "SAFE." prefix if
// <function_call> is in SAFE_ERROR_MODE.
static std::string GetFunctionCallSQL(
    const ResolvedFunctionCallBase* function_call,
    const std::vector<std::string>& inputs) {
  std::string sql = function_call->function()->GetSQL(inputs);
  if (function_call->error_mode() ==
      ResolvedNonScalarFunctionCallBase::SAFE_ERROR_MODE) {
    sql = absl::StrCat("SAFE.", sql);
  }
  return sql;
}

zetasql_base::Status SQLBuilder::VisitResolvedFunctionCall(
    const ResolvedFunctionCall* node) {
  std::vector<std::string> inputs;
  if (node->function()->IsZetaSQLBuiltin() &&
      node->function()->GetSignature(0)->context_id() == FN_MAKE_ARRAY) {
    // For MakeArray function we explicitly prepend the array type to the
    // function sql, and is passed as a part of the inputs.
    inputs.push_back(node->type()->TypeName(options_.product_mode));
  }
  for (const auto& argument : node->argument_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(argument.get()));
    inputs.push_back(result->GetSQL());
  }

  // Getting the SQL for a function given std::string arguments is not itself
  // sensitive to the ProductMode.
  PushQueryFragment(node, GetFunctionCallSQL(node, inputs));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAggregateFunctionCall(
    const ResolvedAggregateFunctionCall* node) {
  std::vector<std::string> inputs;
  if (node->argument_list_size() > 0) {
    for (const auto& argument : node->argument_list()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(argument.get()));
      inputs.push_back(result->GetSQL());
    }

    if (node->distinct()) {
      inputs[0] = absl::StrCat("distinct ", inputs[0]);
    }
  }

  switch (node->null_handling_modifier()) {
    case ResolvedNonScalarFunctionCallBaseEnums::DEFAULT_NULL_HANDLING:
      break;  // skip

    case ResolvedNonScalarFunctionCallBaseEnums::IGNORE_NULLS:
      absl::StrAppend(&inputs.back(), " IGNORE NULLS");
      break;

    case ResolvedNonScalarFunctionCallBaseEnums::RESPECT_NULLS:
      absl::StrAppend(&inputs.back(), " RESPECT NULLS");
      break;
  }

  if (node->having_modifier() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->having_modifier()));
    // This modifier can apply to COUNT(*), in which case there won't be any
    // inputs. Push it into the vector as a dummy input.
    if (inputs.empty()) {
      inputs.push_back(result->GetSQL());
    } else {
      absl::StrAppend(&inputs.back(), result->GetSQL());
    }
  }

  if (!node->order_by_item_list().empty()) {
    std::vector<std::string> order_by_arguments;
    for (const auto& order_by_item : node->order_by_item_list()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(order_by_item.get()));
      order_by_arguments.push_back(result->GetSQL());
    }
    ZETASQL_RET_CHECK(!inputs.empty());
    absl::StrAppend(&inputs.back(), " ORDER BY ",
                    absl::StrJoin(order_by_arguments, ", "));
  }

  if (node->limit() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->limit()));
    absl::StrAppend(&inputs.back(), " LIMIT ", result->GetSQL());
  }

  PushQueryFragment(node, GetFunctionCallSQL(node, inputs));
  return ::zetasql_base::OkStatus();
}

class SQLBuilder::AnalyticFunctionInfo {
 public:
  explicit AnalyticFunctionInfo(const std::string& function) : function_(function) {}
  AnalyticFunctionInfo(const AnalyticFunctionInfo&) = delete;
  AnalyticFunctionInfo operator=(const AnalyticFunctionInfo&) = delete;

  std::string GetSQL() const;

  void set_partition_by(const std::string& partition_by) {
    partition_by_ = partition_by;
  }

  void set_order_by(const std::string& order_by) { order_by_ = order_by; }

  void set_window(const std::string& window) { window_ = window; }

 private:
  const std::string function_;
  std::string partition_by_;
  std::string order_by_;
  std::string window_;
};

std::string SQLBuilder::AnalyticFunctionInfo::GetSQL() const {
  std::vector<std::string> over_clause;
  if (!partition_by_.empty()) {
    over_clause.push_back(partition_by_);
  }
  if (!order_by_.empty()) {
    over_clause.push_back(order_by_);
  }
  if (!window_.empty()) {
    over_clause.push_back(window_);
  }
  return absl::StrCat(function_, " OVER (", absl::StrJoin(over_clause, " "),
                      ")");
}

zetasql_base::Status SQLBuilder::VisitResolvedAnalyticFunctionCall(
    const ResolvedAnalyticFunctionCall* node) {
  std::vector<std::string> inputs;
  if (node->argument_list_size() > 0) {
    for (const auto& argument : node->argument_list()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(argument.get()));
      inputs.push_back(result->GetSQL());
    }
    if (node->distinct()) {
      inputs[0] = absl::StrCat("distinct ", inputs[0]);
    }
    ZETASQL_ASSIGN_OR_RETURN(absl::string_view result,
                     GetNullHandlingModifier(node->null_handling_modifier()));
    absl::StrAppend(&inputs.back(), result);
  }

  std::unique_ptr<AnalyticFunctionInfo> analytic_function_info(
      new AnalyticFunctionInfo(GetFunctionCallSQL(node, inputs)));
  if (node->window_frame() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->window_frame()));
    analytic_function_info->set_window(result->GetSQL());
  }

  ZETASQL_RET_CHECK(pending_analytic_function_ == nullptr);
  pending_analytic_function_ = std::move(analytic_function_info);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAnalyticFunctionGroup(
    const ResolvedAnalyticFunctionGroup* node) {
  std::string partition_by;
  if (node->partition_by() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> partition_result,
                     ProcessNode(node->partition_by()));
    partition_by = partition_result->GetSQL();
  }

  std::string order_by;
  if (node->order_by() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> order_by_result,
                     ProcessNode(node->order_by()));
    order_by = order_by_result->GetSQL();
  }

  for (const auto& analytic_function : node->analytic_function_list()) {
    ZETASQL_RET_CHECK_EQ(analytic_function->expr()->node_kind(),
                 RESOLVED_ANALYTIC_FUNCTION_CALL);
    ZETASQL_RETURN_IF_ERROR(analytic_function->Accept(this));
    // We expect analytic_function->Accept(...) will store its corresponding
    // AnalyticFunctionInfo in pending_analytic_function_.
    ZETASQL_RET_CHECK(pending_analytic_function_ != nullptr);

    pending_analytic_function_->set_partition_by(partition_by);
    pending_analytic_function_->set_order_by(order_by);
    zetasql_base::InsertOrDie(&pending_columns_, analytic_function->column().column_id(),
                     pending_analytic_function_->GetSQL());

    pending_analytic_function_.reset();
  }
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedWindowPartitioning(
    const ResolvedWindowPartitioning* node) {
  std::vector<std::string> partition_by_list_sql;
  for (const auto& column_ref : node->partition_by_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(column_ref.get()));
    partition_by_list_sql.push_back(result->GetSQL());
  }

  std::string sql = "PARTITION";
  if (!node->hint_list().empty()) {
    absl::StrAppend(&sql, " ");
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
  }
  absl::StrAppend(&sql, " BY ", absl::StrJoin(partition_by_list_sql, ", "));

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedWindowOrdering(
    const ResolvedWindowOrdering* node) {
  std::vector<std::string> order_by_list_sql;
  for (const auto& order_by_item : node->order_by_item_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(order_by_item.get()));
    order_by_list_sql.push_back(result->GetSQL());
  }

  std::string sql = "ORDER";
  if (!node->hint_list().empty()) {
    absl::StrAppend(&sql, " ");
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
  }
  absl::StrAppend(&sql, " BY ", absl::StrJoin(order_by_list_sql, ", "));

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedWindowFrame(
    const ResolvedWindowFrame* node) {
  const std::string frame_unit_sql =
      ResolvedWindowFrame::FrameUnitToString(node->frame_unit());
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> start_expr_result,
                   ProcessNode(node->start_expr()));
  const std::string start_expr_sql = start_expr_result->GetSQL();
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> end_expr_result,
                   ProcessNode(node->end_expr()));
  const std::string end_expr_sql = end_expr_result->GetSQL();

  const bool is_frame_unbounded_on_both_sides =
      node->start_expr()->boundary_type() ==
          ResolvedWindowFrameExpr::UNBOUNDED_PRECEDING &&
      node->end_expr()->boundary_type() ==
          ResolvedWindowFrameExpr::UNBOUNDED_FOLLOWING;

  // We don't need a window frame if the range (in the resolved tree) is
  // unbounded on both sides of the current row.
  const std::string sql = is_frame_unbounded_on_both_sides
                         ? ""
                         : absl::StrCat(frame_unit_sql, " BETWEEN ",
                                        start_expr_sql, " AND ", end_expr_sql);

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedWindowFrameExpr(
    const ResolvedWindowFrameExpr* node) {
  std::string sql;
  switch (node->boundary_type()) {
    case ResolvedWindowFrameExpr::UNBOUNDED_PRECEDING: {
      ZETASQL_RET_CHECK(node->expression() == nullptr);
      absl::StrAppend(&sql, "UNBOUNDED PRECEDING");
      break;
    }
    case ResolvedWindowFrameExpr::OFFSET_PRECEDING: {
      ZETASQL_RET_CHECK(node->expression() != nullptr);
      // In case the offset is a casted expression as a result of parameter or
      // literal coercion, we remove the cast as only literals and parameters
      // are allowed in window frame expressions.
      const ResolvedExpr* expr = node->expression();
      if (expr->node_kind() == RESOLVED_CAST) {
        expr = expr->GetAs<ResolvedCast>()->expr();
      }
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(expr));
      absl::StrAppend(&sql, result->GetSQL(), " PRECEDING");
      break;
    }
    case ResolvedWindowFrameExpr::CURRENT_ROW: {
      ZETASQL_RET_CHECK(node->expression() == nullptr);
      absl::StrAppend(&sql, "CURRENT ROW");
      break;
    }
    case ResolvedWindowFrameExpr::OFFSET_FOLLOWING: {
      ZETASQL_RET_CHECK(node->expression() != nullptr);
      const ResolvedExpr* expr = node->expression();
      if (expr->node_kind() == RESOLVED_CAST) {
        expr = expr->GetAs<ResolvedCast>()->expr();
      }
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(expr));
      absl::StrAppend(&sql, result->GetSQL(), " FOLLOWING");
      break;
    }
    case ResolvedWindowFrameExpr::UNBOUNDED_FOLLOWING: {
      ZETASQL_RET_CHECK(node->expression() == nullptr);
      absl::StrAppend(&sql, "UNBOUNDED FOLLOWING");
    }
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

namespace {
// If 'field_descriptor' represents an extension field, appends its
// parenthesized full name to 'text'. Otherwise it is just a regular proto
// field, so we just append its name.
void AppendFieldOrParenthesizedExtensionName(
    const google::protobuf::FieldDescriptor* field_descriptor, std::string* text) {
  const std::string field_name = ToIdentifierLiteral(field_descriptor->name());
  if (!field_descriptor->is_extension()) {
    absl::StrAppend(text, field_name);
  } else {
    // If the extension is scoped within a message we generate a name of the
    // form (`package.ProtoName`.ExtensionField).
    // If the extension is a top level one we generate a name of the form
    // (package.ExtensionField), where package can be multiple possibly
    // quoted identifiers separated by dots if the package has multiple
    // levels.
    if (field_descriptor->extension_scope() != nullptr) {
      absl::StrAppend(
          text, "(",
          ToIdentifierLiteral(field_descriptor->extension_scope()->full_name()),
          ".", field_name, ")");
    } else {
      std::string package_prefix;
      const std::string& package = field_descriptor->file()->package();
      if (!package.empty()) {
        for (const auto& package_name : absl::StrSplit(package, '.')) {
          absl::StrAppend(&package_prefix, ToIdentifierLiteral(package_name),
                          ".");
        }
      }
      absl::StrAppend(text, "(", package_prefix, field_name, ")");
    }
  }
}
}  // namespace

static bool IsAmbiguousFieldExtraction(
    const google::protobuf::FieldDescriptor& field_descriptor,
    const ResolvedExpr* resolved_parent, bool get_has_bit) {
  const google::protobuf::Descriptor* message_descriptor =
      resolved_parent->type()->AsProto()->descriptor();
  if (get_has_bit) {
    return ProtoType::FindFieldByNameIgnoreCase(
               message_descriptor,
               absl::StrCat("has_", field_descriptor.name())) != nullptr;
  } else {
    return absl::StartsWithIgnoreCase(field_descriptor.name(), "has_") &&
           ProtoType::FindFieldByNameIgnoreCase(
               message_descriptor, field_descriptor.name().substr(4)) !=
               nullptr;
  }
}

static zetasql_base::StatusOr<bool> IsRawFieldExtraction(
    const ResolvedGetProtoField* node) {
  ZETASQL_RET_CHECK(!node->get_has_bit());
  const Type* type_with_annotations;
  TypeFactory type_factory;
  ZETASQL_RET_CHECK_OK(type_factory.GetProtoFieldType(node->field_descriptor(),
                                              &type_with_annotations));
  // We know this is a RAW extraction if the field type, respecting annotations,
  // is different than the return type of this node or if the field is primitive
  // and is annotated with (zetasql.use_defaults = false), yet the default
  // value of this node is not null.
  return !type_with_annotations->Equals(node->type()) ||
         (node->type()->IsSimpleType() &&
          node->expr()->type()->AsProto()->descriptor()->file()->syntax() !=
              google::protobuf::FileDescriptor::SYNTAX_PROTO3 &&
             !ProtoType::GetUseDefaultsExtension(node->field_descriptor()) &&
             node->default_value().is_valid() &&
             !node->default_value().is_null());
}

zetasql_base::Status SQLBuilder::VisitResolvedGetProtoField(
    const ResolvedGetProtoField* node) {
  // Dummy access on the default_value and format and fields so as to pass the
  // final CheckFieldsAccessed() on a statement level before building the sql.
  node->default_value();
  node->format();

  std::string text;
  if (node->return_default_value_when_unset()) {
    absl::StrAppend(&text, "PROTO_DEFAULT_IF_NULL(");
  }
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->expr()));
  const std::string result_sql = result->GetSQL();
  // When proto_expr is an empty identifier, we directly use the field_name to
  // access the proto field (in the generated sql). This shows up for in-scope
  // expression column fields where the expression column is anonymous.
  if (result_sql != kEmptyAlias) {
    absl::StrAppend(&text, result_sql, ".");
  }

  if (node->get_has_bit()) {
    if (result_sql == kEmptyAlias ||
        (!IsAmbiguousFieldExtraction(*node->field_descriptor(), node->expr(),
                                     node->get_has_bit()) &&
         !node->field_descriptor()->is_extension())) {
      absl::StrAppend(&text, ToIdentifierLiteral(absl::StrCat(
                                 "has_", node->field_descriptor()->name())));
    } else {
      std::string field_name;
      if (node->field_descriptor()->is_extension()) {
        field_name =
            absl::StrCat("(", node->field_descriptor()->full_name(), ")");
      } else {
        field_name = ToIdentifierLiteral(node->field_descriptor()->name());
      }
      text =
          absl::StrCat("EXTRACT(HAS(", field_name, ") FROM ", result_sql, ")");
    }
  } else {
    ZETASQL_ASSIGN_OR_RETURN(bool is_raw_extraction, IsRawFieldExtraction(node));
    std::string field_name;
    if (node->field_descriptor()->is_extension()) {
      field_name =
          absl::StrCat("(", node->field_descriptor()->full_name(), ")");
    } else {
      field_name = ToIdentifierLiteral(node->field_descriptor()->name());
    }
    if (is_raw_extraction) {
      text =
          absl::StrCat("EXTRACT(RAW(", field_name, ") FROM ", result_sql, ")");
    } else if (node->field_descriptor()->is_extension() ||
               result_sql == kEmptyAlias ||
               !IsAmbiguousFieldExtraction(*node->field_descriptor(),
                                           node->expr(), node->get_has_bit())) {
      AppendFieldOrParenthesizedExtensionName(node->field_descriptor(), &text);
    } else {
      text = absl::StrCat("EXTRACT(FIELD(", field_name, ") FROM ", result_sql,
                          ")");
    }
  }
  if (node->return_default_value_when_unset()) {
    absl::StrAppend(&text, ")");
  }

  PushQueryFragment(node, text);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedReplaceField(
    const ResolvedReplaceField* node) {
  std::string text;
  absl::StrAppend(&text, "REPLACE_FIELDS(");
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> proto_expr,
                   ProcessNode(node->expr()));
  absl::StrAppend(&text, proto_expr->GetSQL(), ",");
  std::string replace_field_item_sql;
  for (const std::unique_ptr<const ResolvedReplaceFieldItem>& replace_item :
       node->replace_field_item_list()) {
    if (!replace_field_item_sql.empty()) {
      absl::StrAppend(&replace_field_item_sql, ",");
    }
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> modified_value,
                     ProcessNode(replace_item->expr()));
    absl::StrAppend(&replace_field_item_sql, modified_value->GetSQL(), " AS ");
    std::string field_path_sql;
    const StructType* current_struct_type = node->expr()->type()->AsStruct();
    for (const int field_index : replace_item->struct_index_path()) {
      if (!field_path_sql.empty()) {
        absl::StrAppend(&field_path_sql, ".");
      }
      ZETASQL_RET_CHECK_LT(field_index, current_struct_type->num_fields());
      absl::StrAppend(&field_path_sql,
                      current_struct_type->field(field_index).name);
      current_struct_type =
          current_struct_type->field(field_index).type->AsStruct();
    }
    for (const google::protobuf::FieldDescriptor* field :
         replace_item->proto_field_path()) {
      if (!field_path_sql.empty()) {
        absl::StrAppend(&field_path_sql, ".");
      }
      if (field->is_extension()) {
        absl::StrAppend(&field_path_sql, "(");
      }
      absl::StrAppend(&field_path_sql, field->is_extension()
                                           ? field->full_name()
                                           : field->name());
      if (field->is_extension()) {
        absl::StrAppend(&field_path_sql, ")");
      }
    }
    absl::StrAppend(&replace_field_item_sql, field_path_sql);
  }
  absl::StrAppend(&text, replace_field_item_sql, ")");

  PushQueryFragment(node, text);
  return zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedColumnRef(const ResolvedColumnRef* node) {
  const ResolvedColumn& column = node->column();
  if (zetasql_base::ContainsKey(pending_columns_, column.column_id())) {
    PushQueryFragment(node,
                      zetasql_base::FindOrDie(pending_columns_, column.column_id()));
  } else {
    PushQueryFragment(node, GetColumnPath(node->column()));
  }
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCast(const ResolvedCast* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->expr()));
  PushQueryFragment(
      node, absl::StrCat(node->return_null_on_error() ? "SAFE_CAST(" : "CAST(",
                         result->GetSQL(), " AS ",
                         node->type()->TypeName(options_.product_mode), ")"));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedSubqueryExpr(
    const ResolvedSubqueryExpr* node) {
  // While resolving a subquery we should start with a fresh scope, i.e. it
  // should not see any columns (except which are correlated) outside the query.
  // To ensure that, we clear the pending_columns_ after maintaining a copy of
  // it locally. We then copy it back once we have processed the subquery.
  // NOTE: For correlated aggregate columns we are expected to print the column
  // path and not the sql to compute the column. So clearing the
  // pending_aggregate_columns here would not have any side effects.
  std::map<int, std::string> previous_pending_aggregate_columns;
  previous_pending_aggregate_columns.swap(pending_columns_);

  std::string text;
  switch (node->subquery_type()) {
    case ResolvedSubqueryExpr::SCALAR:
      break;
    case ResolvedSubqueryExpr::ARRAY:
      absl::StrAppend(&text, "ARRAY");
      break;
    case ResolvedSubqueryExpr::EXISTS:
      absl::StrAppend(&text, "EXISTS");
      break;
    case ResolvedSubqueryExpr::IN:
      ZETASQL_RET_CHECK(node->in_expr() != nullptr)
          << "ResolvedSubqueryExpr of IN type does not have an associated "
             "in_expr:\n" << node->DebugString();
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(node->in_expr()));
      absl::StrAppend(&text, "((", result->GetSQL(), ") IN");
      break;
  }

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->subquery()));
  std::unique_ptr<QueryExpression> subquery_result(
      result->query_expression.release());
  ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(node->subquery()->column_list(),
                                        subquery_result.get()));
  absl::StrAppend(&text, "(", subquery_result->GetSQLQuery(), ")",
                  node->in_expr() == nullptr ? "" : ")");

  // Dummy access on the parameter list so as to pass the final
  // CheckFieldsAccessed() on a statement level before building the sql.
  if (!node->parameter_list().empty()) {
    for (const auto& parameter : node->parameter_list()) {
      parameter->column();
    }
  }

  PushQueryFragment(node, text);
  previous_pending_aggregate_columns.swap(pending_columns_);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::AppendColumnSchema(
    const Type* type, bool is_hidden,
    const ResolvedColumnAnnotations* annotations,
    const ResolvedGeneratedColumnInfo* generated_column_info, std::string* text) {
  ZETASQL_RET_CHECK(text != nullptr);
  if (type != nullptr) {
    if (type->IsStruct()) {
      const StructType* struct_type = type->AsStruct();
      absl::StrAppend(text, "STRUCT<");
      for (int i = 0; i < struct_type->num_fields(); ++i) {
        const StructField& field = struct_type->field(i);
        if (i != 0) absl::StrAppend(text, ", ");
        if (!field.name.empty()) {
          absl::StrAppend(text, ToIdentifierLiteral(field.name), " ");
        }
        const ResolvedColumnAnnotations* child_annotations =
            annotations != nullptr && i < annotations->child_list_size()
                ? annotations->child_list(i)
                : nullptr;
        ZETASQL_RETURN_IF_ERROR(AppendColumnSchema(
            field.type, /*is_hidden=*/false, child_annotations,
            /*generated_column_info=*/nullptr, text));
      }
      absl::StrAppend(text, ">");
    } else if (type->IsArray()) {
      const ArrayType* array_type = type->AsArray();
      absl::StrAppend(text, "ARRAY<");
      const ResolvedColumnAnnotations* child_annotations =
          annotations != nullptr && !annotations->child_list().empty()
              ? annotations->child_list(0)
              : nullptr;
      ZETASQL_RETURN_IF_ERROR(AppendColumnSchema(array_type->element_type(),
                                         /*is_hidden=*/false, child_annotations,
                                         /*generated_column_info=*/nullptr,
                                         text));
      absl::StrAppend(text, ">");
    } else {
      absl::StrAppend(text, type->TypeName(options_.product_mode));
    }
  }
  if (generated_column_info != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(generated_column_info->expression()));

    absl::StrAppend(text, " AS ", result->GetSQL());

    if (generated_column_info->is_stored()) {
      absl::StrAppend(text, " STORED");
    }
  }
  if (is_hidden) {
    absl::StrAppend(text, " HIDDEN");
  }
  if (annotations != nullptr) {
    if (annotations->not_null()) {
      absl::StrAppend(text, " NOT NULL");
    }
    if (!annotations->option_list().empty()) {
      ZETASQL_ASSIGN_OR_RETURN(const std::string col_options_string,
                       GetHintListString(annotations->option_list()));
      absl::StrAppend(text, " OPTIONS(", col_options_string, ")");
    }
  }
  return ::zetasql_base::OkStatus();
}

zetasql_base::StatusOr<std::string> SQLBuilder::GetHintListString(
    const std::vector<std::unique_ptr<const ResolvedOption>>& hint_list) {
  if (hint_list.empty()) {
    return std::string() /* no hints */;
  }

  std::vector<std::string> hint_list_sql;
  for (const auto& hint : hint_list) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(hint.get()));
    hint_list_sql.push_back(result->GetSQL());
  }

  return absl::StrJoin(hint_list_sql, ", ");
}

zetasql_base::Status SQLBuilder::AppendOptions(
    const std::vector<std::unique_ptr<const ResolvedOption>>& option_list,
    std::string* sql) {
  ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                   GetHintListString(option_list));
  absl::StrAppend(sql, " OPTIONS(", options_string, ")");
  return zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::AppendOptionsIfPresent(
    const std::vector<std::unique_ptr<const ResolvedOption>>& option_list,
    std::string* sql) {
  if (!option_list.empty()) {
    ZETASQL_RETURN_IF_ERROR(AppendOptions(option_list, sql));
  }
  return zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::AppendHintsIfPresent(
    const std::vector<std::unique_ptr<const ResolvedOption>>& hint_list,
    std::string* text) {
  ZETASQL_RET_CHECK(text != nullptr);
  if (!hint_list.empty()) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string result, GetHintListString(hint_list));
    absl::StrAppend(text, "@{ ", result, " }");
  }
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedOption(const ResolvedOption* node) {
  std::string text;
  if (!node->qualifier().empty()) {
    absl::StrAppend(&text, ToIdentifierLiteral(node->qualifier()), ".");
  }
  absl::StrAppend(&text, ToIdentifierLiteral(node->name()), "=");

  // If we have a CAST, strip it off and just print the value.  CASTs are
  // not allowed as part of the hint syntax so they must have been added
  // implicitly.
  const ResolvedExpr* value_expr = node->value();
  if (value_expr->node_kind() == RESOLVED_CAST) {
    value_expr = value_expr->GetAs<ResolvedCast>()->expr();
  }

  if (value_expr->node_kind() == RESOLVED_LITERAL) {
    const ResolvedLiteral* literal = value_expr->GetAs<ResolvedLiteral>();
    ZETASQL_ASSIGN_OR_RETURN(const std::string result,
                     GetSQL(literal->value(), options_.product_mode,
                            true /* is_constant_value */));
    absl::StrAppend(&text, result);
  } else {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(value_expr));

    // Wrap the result sql in parentheses so that, when the generated text is
    // parsed, identifier expressions get treated as identifiers, rather than
    // std::string literals.
    absl::StrAppend(&text, "(", result->GetSQL(), ")");
  }
  PushQueryFragment(node, text);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedSystemVariable(
    const ResolvedSystemVariable* node) {
  std::string full_name = absl::StrJoin(
      node->name_path(), ".", [](std::string* out, const std::string& input) {
        absl::StrAppend(out, ToIdentifierLiteral(input));
      });
  PushQueryFragment(node, absl::StrCat("@@", full_name));
  return zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedParameter(const ResolvedParameter* node) {
  if (node->name().empty()) {
    std::string param_str;
    switch (options_.positional_parameter_mode) {
      case SQLBuilderOptions::kQuestionMark:
        param_str = "?";
        break;
      case SQLBuilderOptions::kNamed:
        param_str = absl::StrCat("@param", node->position());
        break;
    }
    if (node->position() - 1 <
        options_.undeclared_positional_parameters.size()) {
      param_str =
          AddExplicitCast(param_str, node->type(), options_.product_mode);
    }
    PushQueryFragment(node, param_str);
  } else {
    std::string param_str = absl::StrCat("@", ToIdentifierLiteral(node->name()));
    if (zetasql_base::ContainsKey(options_.undeclared_parameters, node->name())) {
      param_str =
          AddExplicitCast(param_str, node->type(), options_.product_mode);
    }
    PushQueryFragment(node, param_str);
  }
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedMakeProto(const ResolvedMakeProto* node) {
  const std::string& proto_name = node->type()->AsProto()->descriptor()->full_name();

  std::string text;
  absl::StrAppend(&text, "NEW ", ToIdentifierLiteral(proto_name), "(");
  bool first = true;
  for (const auto& field : node->field_list()) {
    if (!first) absl::StrAppend(&text, ", ");
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(field.get()));
    absl::StrAppend(&text, result->GetSQL());
    first = false;
  }
  absl::StrAppend(&text, ")");

  PushQueryFragment(node, text);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedMakeProtoField(
    const ResolvedMakeProtoField* node) {
  // Dummy access on the format field so as to pass the final
  // CheckFieldsAccessed() on a statement level, before building the sql.
  node->format();

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->expr()));

  std::string text = result->GetSQL();
  absl::StrAppend(&text, " AS ");
  AppendFieldOrParenthesizedExtensionName(node->field_descriptor(), &text);

  PushQueryFragment(node, text);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedMakeStruct(
    const ResolvedMakeStruct* node) {
  const StructType* struct_type = node->type()->AsStruct();
  std::string text;
  absl::StrAppend(&text, struct_type->TypeName(options_.product_mode), "(");
  if (struct_type->num_fields() != node->field_list_size()) {
    return ::zetasql_base::InvalidArgumentErrorBuilder()
           << "Number of fields of ResolvedMakeStruct and its corresponding "
              "StructType, do not match\n:"
           << node->DebugString() << "\nStructType:\n"
           << struct_type->DebugString();
  }
  for (int i = 0; i < struct_type->num_fields(); ++i) {
    if (i > 0) absl::StrAppend(&text, ", ");
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->field_list(i)));
    absl::StrAppend(&text, result->GetSQL());
  }
  absl::StrAppend(&text, ")");
  PushQueryFragment(node, text);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedGetStructField(
    const ResolvedGetStructField* node) {
  std::string text;
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->expr()));
  std::string result_sql = result->GetSQL();

  // When struct_expr is an empty identifier, we directly use the field_name to
  // access the struct field (in the generated sql). This shows up for in-scope
  // expression column fields where the expression column is anonymous.
  if (result_sql != kEmptyAlias) {
    if (node->expr()->node_kind() == RESOLVED_CONSTANT ||
        node->expr()->node_kind() == RESOLVED_SYSTEM_VARIABLE) {
      // Enclose <result_sql> in parentheses to ensure that "(foo).bar"
      // (accessing field bar of constant foo) does not get unparsed as
      // "foo.bar" (accessing named constant bar in namespace foo).
      result_sql = absl::StrCat("(", result_sql, ")");
    }
    absl::StrAppend(&text, result_sql, ".");
  }
  const std::string& field_name =
      node->expr()->type()->AsStruct()->field(node->field_idx()).name;
  absl::StrAppend(&text, ToIdentifierLiteral(field_name));
  PushQueryFragment(node, text);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::WrapQueryExpression(
    const ResolvedScan* node, QueryExpression* query_expression) {
  const std::string alias = GetScanAlias(node);
  ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(node->column_list(), query_expression));
  query_expression->Wrap(alias);
  SetPathForColumnList(node->column_list(), alias);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::GetSelectList(
    const ResolvedColumnList& column_list,
    const std::map<int64_t, const ResolvedExpr*>& col_to_expr_map,
    const ResolvedScan* parent_scan,
    QueryExpression* query_expression,
    std::vector<std::pair<std::string, std::string>>* select_list) {
  DCHECK(select_list != nullptr);

  std::map<int, std::string>* group_by_list = query_expression->MutableGroupByList();
  std::set<int /* column_id */> has_alias;
  std::set<std::string> seen_aliases;
  for (int i = 0; i < column_list.size(); ++i) {
    const ResolvedColumn& col = column_list[i];
    std::string alias;
    // When assigning select_alias to column which occurs in column_list more
    // than once:
    //  - We assign unique alias to each of those occurrences.
    //  - Only one of those aliases is registered to refer to that column later
    //    in the query. See GetColumnAlias().
    // This avoids ambiguity in selecting that column outside the subquery
    if (zetasql_base::ContainsKey(has_alias, col.column_id())) {
      alias = absl::StrCat("a_", GetUniqueId());
    } else {
      alias = GetColumnAlias(col);
      // This alias has already been used in the SELECT, so create a new one.
      if (zetasql_base::ContainsKey(seen_aliases, alias)) {
        alias = UpdateColumnAlias(col);
      }
      has_alias.insert(col.column_id());
    }
    seen_aliases.insert(alias);

    if (zetasql_base::ContainsKey(col_to_expr_map, col.column_id())) {
      const ResolvedExpr* expr =
          zetasql_base::FindOrDie(col_to_expr_map, col.column_id());
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(expr));
      select_list->push_back(std::make_pair(result->GetSQL(), alias));
    } else {
      if (zetasql_base::ContainsKey(*group_by_list, col.column_id())) {
        select_list->push_back(std::make_pair(
            zetasql_base::FindOrDie(*group_by_list, col.column_id()), alias));
      } else if (zetasql_base::ContainsKey(pending_columns_, col.column_id())) {
        select_list->push_back(std::make_pair(
            zetasql_base::FindOrDie(pending_columns_, col.column_id()), alias));
      } else {
        select_list->push_back(std::make_pair(GetColumnPath(col), alias));
      }
    }
  }
  pending_columns_.clear();

  // If any group_by column is computed in the select list, then update the sql
  // text for those columns in group_by_list inside the QueryExpression,
  // reflecting the ordinal position of select clause.
  for (int pos = 0; pos < column_list.size(); ++pos) {
    const ResolvedColumn& col = column_list[pos];
    if (zetasql_base::ContainsKey(*group_by_list, col.column_id())) {
      zetasql_base::InsertOrUpdate(
          group_by_list, col.column_id(),
          absl::StrCat(pos + 1) /* select list ordinal position */);
    }
  }

  if (column_list.empty()) {
    ZETASQL_RET_CHECK_EQ(select_list->size(), 0);
    // Add a dummy value "NULL" to the select list if the column list is empty.
    // This ensures that we form a valid query for scans with empty columns.
    select_list->push_back(std::make_pair("NULL", "" /* no select alias */));
  }

  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::GetSelectList(
    const ResolvedColumnList& column_list, QueryExpression* query_expression,
    std::vector<std::pair<std::string, std::string>>* select_list) {
  return GetSelectList(column_list, {} /* empty col_to_expr_map */,
                       nullptr /* parent_scan */, query_expression,
                       select_list);
}

zetasql_base::Status SQLBuilder::AddSelectListIfNeeded(
    const ResolvedColumnList& column_list, QueryExpression* query_expression) {
  if (!query_expression->CanFormSQLQuery()) {
    std::vector<std::pair<std::string, std::string>> select_list;
    ZETASQL_RETURN_IF_ERROR(GetSelectList(column_list, query_expression, &select_list));
    ZETASQL_RET_CHECK(query_expression->TrySetSelectClause(select_list,
                                                   "" /* no select hints */));
  }
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedTableScan(const ResolvedTableScan* node) {
  std::unique_ptr<QueryExpression> query_expression(new QueryExpression);
  std::string from;
  absl::StrAppend(&from, ToIdentifierLiteral(node->table()->Name()));
  if (node->hint_list_size() > 0) {
    absl::StrAppend(&from, " ");
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &from));
  }
  // To avoid name resolution conflicts between a table and a column that have
  // the same name.
  std::string table_alias;
  if (zetasql_base::ContainsKey(col_ref_names_,
                       absl::AsciiStrToLower(node->table()->Name()))) {
    table_alias = GetTableAlias(node->table());
    absl::StrAppend(&from, " AS ", table_alias);
  } else {
    table_alias = ToIdentifierLiteral(node->table()->Name());
  }
  if (node->for_system_time_expr() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->for_system_time_expr()));
    absl::StrAppend(&from, " FOR SYSTEM_TIME AS OF ", result->GetSQL());
  }
  ZETASQL_RET_CHECK(query_expression->TrySetFromClause(from));

  std::vector<std::pair<std::string, std::string>> select_list;
  for (int i = 0; i < node->column_list_size(); ++i) {
    const ResolvedColumn& column = node->column_list(i);
    if (node->table()->IsValueTable() && node->column_index_list(i) == 0) {
      // Use the table name instead for selecting value table column.
      select_list.push_back(
          std::make_pair(table_alias, GetColumnAlias(column)));
    } else {
      select_list.push_back(std::make_pair(
          absl::StrCat(table_alias, ".", ToIdentifierLiteral(column.name())),
          GetColumnAlias(column)));
    }
  }
  ZETASQL_RET_CHECK(query_expression->TrySetSelectClause(select_list, ""));

  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedSingleRowScan(
    const ResolvedSingleRowScan* node) {
  std::unique_ptr<QueryExpression> query_expression(new QueryExpression);
  // We generate a dummy subquery "select 1" for SingleRowScan, so that we
  // can build valid sql for scans having SinleRowScan as input.
  ZETASQL_RET_CHECK(query_expression->TrySetSelectClause(
      {std::make_pair("1", "" /* no select_alias */)}, "" /* no hints */));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedProjectScan(
    const ResolvedProjectScan* node) {
  std::unique_ptr<QueryExpression> query_expression;
  // No sql to add if the input scan is ResolvedSingleRowScan.
  if (node->input_scan()->node_kind() == RESOLVED_SINGLE_ROW_SCAN) {
    query_expression = absl::make_unique<QueryExpression>();
  } else {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->input_scan()));
    query_expression = std::move(result->query_expression);
  }

  if (query_expression->CanFormSQLQuery()) {
    ZETASQL_RETURN_IF_ERROR(
        WrapQueryExpression(node->input_scan(), query_expression.get()));
  }

  std::map<int64_t /* column_id */, const ResolvedExpr*> col_to_expr_map;
  for (const auto& expr : node->expr_list()) {
    zetasql_base::InsertIfNotPresent(&col_to_expr_map, expr->column().column_id(),
                            expr->expr());
  }
  std::vector<std::pair<std::string, std::string>> select_list;
  ZETASQL_RETURN_IF_ERROR(GetSelectList(node->column_list(), col_to_expr_map,
                                node, query_expression.get(), &select_list));

  std::string select_hints;
  ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &select_hints));

  ZETASQL_RET_CHECK(query_expression->TrySetSelectClause(select_list, select_hints));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

// This is a convenience function to return some debugging information for the
// computed_column_alias_ map.
static std::string ComputedColumnAliasDebugString(
    const std::map<int, std::string>& computed_column_alias) {
  std::vector<std::string> pairs;
  pairs.reserve(computed_column_alias.size());
  for (const auto& kv : computed_column_alias) {
    pairs.push_back(absl::StrCat("(", kv.first, ", ", kv.second, ")"));
  }
  return absl::StrCat("[", absl::StrJoin(pairs, ", "), "]");
}

zetasql_base::Status SQLBuilder::VisitResolvedTVFScan(const ResolvedTVFScan* node) {
  std::unique_ptr<QueryExpression> query_expression(new QueryExpression);
  std::string from;
  absl::StrAppend(&from, node->tvf()->FullName());
  absl::StrAppend(&from, "(");

  // Visit the TVF arguments and build SQL from them.
  std::vector<std::string> argument_list;
  argument_list.reserve(node->argument_list_size());
  std::vector<std::string> output_aliases;
  for (int arg_idx = 0; arg_idx < node->argument_list_size(); ++arg_idx) {
    const ResolvedTVFArgument* argument = node->argument_list(arg_idx);
    // If this is a scalar expression, generate SQL for it and continue.
    if (argument->expr() != nullptr) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(argument->expr()));
      argument_list.push_back(result->GetSQL());
      continue;
    }

    if (argument->model() != nullptr) {
      const std::string model_alias =
          ToIdentifierLiteral(argument->model()->model()->Name());
      argument_list.push_back(absl::StrCat("MODEL ", model_alias));
      continue;
    }

    // If this is a relation argument, generate SQL for each of its attributes.
    const ResolvedScan* scan = argument->scan();
    ZETASQL_RET_CHECK(scan != nullptr);
    ZETASQL_RET_CHECK_LT(arg_idx, node->signature()->input_arguments().size());
    ZETASQL_RET_CHECK(node->signature()->argument(arg_idx).is_relation());
    const TVFRelation& relation =
        node->signature()->argument(arg_idx).relation();
    if (relation.is_value_table()) {
      // If the input table is a value table, add SELECT AS VALUE to make sure
      // that the generated SQL for the scan is also a value table.
      ZETASQL_RET_CHECK_EQ(1, scan->column_list_size());
      ZETASQL_RET_CHECK_EQ(1, argument->argument_column_list_size());
      ZETASQL_RET_CHECK_EQ(scan->column_list(0).column_id(),
                   argument->argument_column_list(0).column_id());
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(scan));
      const std::string column_alias = zetasql_base::FindWithDefault(
          computed_column_alias_, scan->column_list(0).column_id(), "");
      ZETASQL_RET_CHECK(!column_alias.empty())
          << "scan: " << scan->DebugString() << "\n\n computed_column_alias_: "
          << ComputedColumnAliasDebugString(computed_column_alias_);
      argument_list.push_back(absl::StrCat("(SELECT AS VALUE ", column_alias,
                                           " FROM (", result->GetSQL(), "))"));
    } else {
      // If the input table is not a value table, assign each required column
      // name as an alias for each element in the SELECT list.
      ZETASQL_RET_CHECK_EQ(node->argument_list_size(),
                   node->signature()->input_arguments().size())
          << node->DebugString();

      // Generate SQL for the input relation argument. Before doing so, check if
      // the argument_column_list differs from the scan's column_list. If so,
      // add a wrapping query that explicitly lists the former columns in the
      // correct order.
      //
      // First build a map from each column ID of the input scan to the alias
      // used when we generated the input scan SELECT list.
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(scan));
      std::map<int, std::string> column_id_to_alias;
      const std::vector<std::pair<std::string, std::string>>& select_list =
          result->query_expression->SelectList();
      ZETASQL_RET_CHECK_EQ(select_list.size(), scan->column_list_size())
          << scan->DebugString();
      for (int i = 0; i < select_list.size(); ++i) {
        zetasql_base::InsertIfNotPresent(&column_id_to_alias,
                                scan->column_list(i).column_id(),
                                select_list[i].second);
        if (arg_idx == 0 &&
            node->tvf()->Is<ForwardInputSchemaToOutputSchemaTVF>()) {
          output_aliases.push_back(select_list[i].second);
        }
      }
      // Then build a new SELECT list containing only the columns explicitly
      // referenced in the 'argument_column_list', using aliases from the
      // provided input table if necessary.
      std::vector<std::string> arg_col_list_select_items;
      ZETASQL_RET_CHECK_EQ(argument->argument_column_list_size(),
                   relation.num_columns()) << relation.DebugString();
      for (int i = 0; i < argument->argument_column_list_size(); ++i) {
        const ResolvedColumn& col = argument->argument_column_list(i);
        const std::string* alias =
            zetasql_base::FindOrNull(column_id_to_alias, col.column_id());
        ZETASQL_RET_CHECK(alias != nullptr) << col.DebugString();
        arg_col_list_select_items.push_back(*alias);

        const std::string required_col = relation.column(i).name;
        if (!IsInternalAlias(required_col)) {
          // If the TVF call included explicit column names for the input table
          // argument, specify them again here in case the TVF requires these
          // exact column names to work properly.
          absl::StrAppend(&arg_col_list_select_items.back(), " AS ",
                          required_col);
          zetasql_base::InsertOrUpdate(&computed_column_alias_, col.column_id(),
                              required_col);
        } else {
          // If there is no explicit column/alias name, then we use the
          // original column name/alias.
          zetasql_base::InsertOrUpdate(&computed_column_alias_, col.column_id(), *alias);
        }
      }
      argument_list.push_back(absl::StrCat(
          "(SELECT ", absl::StrJoin(arg_col_list_select_items, ", "), " FROM (",
          result->GetSQL(), "))"));
    }
  }
  absl::StrAppend(&from, absl::StrJoin(argument_list, ", "));
  absl::StrAppend(&from, ")");
  // Append the query hint, if present.
  if (node->hint_list_size() > 0) {
    absl::StrAppend(&from, " ");
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &from));
    absl::StrAppend(&from, " ");
  }

  const std::string tvf_scan_alias = GetScanAlias(node);
  absl::StrAppend(&from, " AS ", tvf_scan_alias);

  // Copy the TVF's column names to the pending_columns_ list so that the
  // outer SELECT that includes this TVF in the FROM clause uses the TVF
  // names from its output schema.
  bool is_value_table = node->signature()->result_schema().is_value_table();
  for (int i = 0; i < node->column_list_size(); ++i) {
    const ResolvedColumn& column = node->column_list(i);
    std::string column_name;
    if (i == 0 && is_value_table) {
      zetasql_base::InsertOrDie(&pending_columns_, column.column_id(), tvf_scan_alias);
    } else {
      // If this TVF simply forwards the schema of the input relation in its
      // first argument to the TVF's output relation, reuse the column names.
      if (node->tvf()->Is<ForwardInputSchemaToOutputSchemaTVF>()) {
        ZETASQL_RET_CHECK(node->signature()->argument(0).is_relation());
        column_name = node->signature()->argument(0).relation().column(i).name;
        if (IsInternalAlias(column_name)) {
          ZETASQL_RET_CHECK_LT(i, output_aliases.size());
          column_name = output_aliases[i];
        }
      } else {
        column_name = column.name();
      }
      zetasql_base::InsertOrDie(&pending_columns_, column.column_id(), column_name);
    }
  }

  ZETASQL_RET_CHECK(query_expression->TrySetFromClause(from));

  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedRelationArgumentScan(
    const ResolvedRelationArgumentScan* node) {
  std::unique_ptr<QueryExpression> query_expression(new QueryExpression);
  ZETASQL_RET_CHECK(query_expression->TrySetFromClause(node->name()));
  std::vector<std::pair<std::string, std::string>> select_list;
  if (node->is_value_table()) {
    ZETASQL_RET_CHECK_EQ(1, node->column_list().size());
    select_list.push_back(
        std::make_pair(ToIdentifierLiteral(node->name()),
                       GetColumnAlias(node->column_list()[0])));
  } else {
    for (const ResolvedColumn& column : node->column_list()) {
      select_list.push_back(
          std::make_pair(absl::StrCat(ToIdentifierLiteral(node->name()), ".",
                                      ToIdentifierLiteral(column.name())),
                         GetColumnAlias(column)));
    }
  }
  ZETASQL_RET_CHECK(query_expression->TrySetSelectClause(select_list, ""));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedFilterScan(
    const ResolvedFilterScan* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> input_result,
                   ProcessNode(node->input_scan()));
  std::unique_ptr<QueryExpression> query_expression(
      input_result->query_expression.release());

  // Filter scans over non-value table scans can always be flattened.  Other
  // combinations can probably be flattened more agressively too, but I only
  // needed simple table scans at this time. Can work harder on this when it
  // becomes needed.
  const ResolvedTableScan* simple_table_scan = nullptr;
  if (node->input_scan()->node_kind() == zetasql::RESOLVED_TABLE_SCAN) {
    simple_table_scan = node->input_scan()->GetAs<ResolvedTableScan>();
    if (simple_table_scan->table()->IsValueTable()) {
      simple_table_scan = nullptr;
    }
  }
  if (simple_table_scan != nullptr) {
    // Make columns from the input_scan available to filter_expr node processing
    // below.
    SetPathForColumnsInScan(
        simple_table_scan,
        zetasql_base::FindWithDefault(table_alias_map_, simple_table_scan->table(), ""));
    // Remove the underlying TableScan's select list, to let the ProjectScan
    // just above this FilterScan to install its select list without wrapping
    // this fragment.
    query_expression->ResetSelectClause();
  } else {
    if (!query_expression->CanSetWhereClause()) {
      ZETASQL_RETURN_IF_ERROR(
          WrapQueryExpression(node->input_scan(), query_expression.get()));
    }
  }
  std::string where;
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->filter_expr()));
  absl::StrAppend(&where, result->GetSQL());
  ZETASQL_RET_CHECK(query_expression->TrySetWhereClause(where));

  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAnalyticScan(
    const ResolvedAnalyticScan* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> input_result,
                   ProcessNode(node->input_scan()));
  std::unique_ptr<QueryExpression> query_expression(
      input_result->query_expression.release());
  if (query_expression->CanFormSQLQuery()) {
    ZETASQL_RETURN_IF_ERROR(
        WrapQueryExpression(node->input_scan(), query_expression.get()));
  }

  for (const auto& function_group : node->function_group_list()) {
    ZETASQL_RETURN_IF_ERROR(function_group->Accept(this));
  }
  ZETASQL_RETURN_IF_ERROR(
      AddSelectListIfNeeded(node->column_list(), query_expression.get()));

  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

static std::string GetJoinTypeString(ResolvedJoinScan::JoinType join_type,
                                bool expect_on_condition) {
  switch (join_type) {
    case ResolvedJoinScan::INNER:
      return expect_on_condition ? "INNER JOIN" : "CROSS JOIN";
    case ResolvedJoinScan::LEFT:
      return "LEFT JOIN";
    case ResolvedJoinScan::RIGHT:
      return "RIGHT JOIN";
    case ResolvedJoinScan::FULL:
      return "FULL JOIN";
  }
}

zetasql_base::StatusOr<std::string> SQLBuilder::GetJoinOperand(const ResolvedScan* scan) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> scan_f, ProcessNode(scan));

  std::string alias = GetScanAlias(scan);
  ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(scan->column_list(),
                                        scan_f->query_expression.get()));
  SetPathForColumnList(scan->column_list(), alias);
  return absl::StrCat("(", scan_f->GetSQL(), ") AS ", alias);
}

std::string SQLBuilder::MakeNonconflictingAlias(const std::string& name) {
  const std::string alias_prefix_lower = absl::AsciiStrToLower(name);
  std::string alias;
  do {
    alias = absl::StrCat(alias_prefix_lower, "_", GetUniqueId());
  } while (zetasql_base::ContainsKey(col_ref_names_, alias));
  return ToIdentifierLiteral(alias);
}

std::string SQLBuilder::GetTableAlias(const Table* table) {
  if (!zetasql_base::ContainsKey(table_alias_map_, table)) {
    zetasql_base::InsertIfNotPresent(&table_alias_map_, table,
                            MakeNonconflictingAlias(table->Name()));
  }

  return zetasql_base::FindOrDie(table_alias_map_, table);
}

std::string SQLBuilder::GetScanAlias(const ResolvedScan* scan) {
  if (!zetasql_base::ContainsKey(scan_alias_map_, scan)) {
    const std::string scan_alias_prefix_lower = absl::AsciiStrToLower(
        (scan->node_kind() == RESOLVED_TABLE_SCAN)
            // We prefer to use table names as alias prefix when possible.
            ? scan->GetAs<ResolvedTableScan>()->table()->Name()
            : scan->node_kind_string());
    zetasql_base::InsertIfNotPresent(&scan_alias_map_, scan,
                            MakeNonconflictingAlias(scan_alias_prefix_lower));
  }

  return zetasql_base::FindOrDie(scan_alias_map_, scan);
}

int64_t SQLBuilder::GetUniqueId() {
  int64_t id = scan_id_sequence_.GetNext();
  if (id == 0) {  // Avoid using scan id 0.
    id = scan_id_sequence_.GetNext();
    DCHECK_NE(id, 0);
  }
  DCHECK_LE(id, std::numeric_limits<int32_t>::max());
  return id;
}

void SQLBuilder::SetPathForColumn(const ResolvedColumn& column,
                                  const std::string& path) {
  zetasql_base::InsertOrUpdate(&column_paths_, column.column_id(), path);
}

void SQLBuilder::SetPathForColumnList(const ResolvedColumnList& column_list,
                                      const std::string& scan_alias) {
  for (const ResolvedColumn& col : column_list) {
    SetPathForColumn(col, absl::StrCat(scan_alias, ".", GetColumnAlias(col)));
  }
}

void SQLBuilder::SetPathForColumnsInScan(const ResolvedScan* scan,
                                         const std::string& alias) {
  if (scan->node_kind() == RESOLVED_TABLE_SCAN &&
      scan->GetAs<ResolvedTableScan>()->table()->IsValueTable()) {
    if (scan->column_list_size() > 0) {
      // This code is wrong.  See http://b/37291554.
      const Table* table = scan->GetAs<ResolvedTableScan>()->table();
      std::string table_name = alias.empty() ?
          ToIdentifierLiteral(table->Name()) : alias;
      SetPathForColumn(scan->column_list(0), table_name);
    }
  } else {
    for (const ResolvedColumn& column : scan->column_list()) {
      std::string table_name = alias.empty() ?
          ToIdentifierLiteral(column.table_name()) : alias;
      SetPathForColumn(
          column,
          absl::StrCat(table_name, ".", ToIdentifierLiteral(column.name())));
    }
  }
}

zetasql_base::Status SQLBuilder::VisitResolvedJoinScan(const ResolvedJoinScan* node) {
  std::unique_ptr<QueryExpression> query_expression(new QueryExpression);
  ZETASQL_ASSIGN_OR_RETURN(const std::string left_join_operand,
                   GetJoinOperand(node->left_scan()));
  ZETASQL_ASSIGN_OR_RETURN(const std::string right_join_operand,
                   GetJoinOperand(node->right_scan()));

  std::string hints = "";
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &hints));
  }
  std::string from;
  absl::StrAppend(
      &from, left_join_operand, " ",
      GetJoinTypeString(node->join_type(), node->join_expr() != nullptr), hints,
      " ", right_join_operand);
  if (node->join_expr() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->join_expr()));
    absl::StrAppend(&from, " ON ", result->GetSQL());
  }
  ZETASQL_RET_CHECK(query_expression->TrySetFromClause(from));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedColumnHolder(
    const ResolvedColumnHolder* node) {
  PushQueryFragment(node, GetColumnPath(node->column()));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedArrayScan(const ResolvedArrayScan* node) {
  std::unique_ptr<QueryExpression> query_expression(new QueryExpression);
  std::string from;
  if (node->input_scan() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string join_operand,
                     GetJoinOperand(node->input_scan()));
    absl::StrAppend(&from, join_operand, node->is_outer() ? " LEFT" : "",
                    " JOIN ");
  }
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->array_expr()));
  absl::StrAppend(&from, "UNNEST(", result->GetSQL(), ") ",
                  GetColumnAlias(node->element_column()));

  if (node->array_offset_column() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->array_offset_column()));
    absl::StrAppend(&from, " WITH OFFSET ", result->GetSQL());
  }
  if (node->join_expr() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->join_expr()));
    absl::StrAppend(&from, " ON ", result->GetSQL());
  }

  ZETASQL_RET_CHECK(query_expression->TrySetFromClause(from));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedLimitOffsetScan(
    const ResolvedLimitOffsetScan* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> input_result,
                   ProcessNode(node->input_scan()));
  std::unique_ptr<QueryExpression> query_expression(
      input_result->query_expression.release());

  if (node->limit() != nullptr) {
    if (!query_expression->CanSetLimitClause()) {
      ZETASQL_RETURN_IF_ERROR(
          WrapQueryExpression(node->input_scan(), query_expression.get()));
    }
    ZETASQL_ASSIGN_OR_RETURN(
        std::unique_ptr<QueryFragment> result,
        // If limit is a ResolvedCast, it means that the original limit in the
        // query is a literal or parameter with a type other than int64_t and
        // hence, analyzer has added a cast on top of it. We should skip this
        // cast here to avoid returning CAST(CAST ...)).
        ProcessNode(node->limit()->node_kind() != RESOLVED_CAST
                        ? node->limit()
                        : node->limit()->GetAs<ResolvedCast>()->expr()));
    ZETASQL_RET_CHECK(query_expression->TrySetLimitClause(result->GetSQL()));
  }
  if (node->offset() != nullptr) {
    if (!query_expression->CanSetOffsetClause()) {
      ZETASQL_RETURN_IF_ERROR(
          WrapQueryExpression(node->input_scan(), query_expression.get()));
    }
    ZETASQL_ASSIGN_OR_RETURN(
        std::unique_ptr<QueryFragment> result,
        ProcessNode(node->offset()->node_kind() != RESOLVED_CAST
                        ? node->offset()
                        : node->offset()->GetAs<ResolvedCast>()->expr()));
    ZETASQL_RET_CHECK(query_expression->TrySetOffsetClause(result->GetSQL()));
  }
  ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(node->column_list(),
                                        query_expression.get()));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

std::pair<std::string, std::string> GetOpTypePair(
    ResolvedSetOperationScan::SetOperationType op_type) {
  switch (op_type) {
    case ResolvedSetOperationScan::UNION_ALL:
      return std::make_pair("UNION", "ALL");
    case ResolvedSetOperationScan::UNION_DISTINCT:
      return std::make_pair("UNION", "DISTINCT");
    case ResolvedSetOperationScan::INTERSECT_ALL:
      return std::make_pair("INTERSECT", "ALL");
    case ResolvedSetOperationScan::INTERSECT_DISTINCT:
      return std::make_pair("INTERSECT", "DISTINCT");
    case ResolvedSetOperationScan::EXCEPT_ALL:
      return std::make_pair("EXCEPT", "ALL");
    case ResolvedSetOperationScan::EXCEPT_DISTINCT:
      return std::make_pair("EXCEPT", "ALL");
  }
}

zetasql_base::Status SQLBuilder::VisitResolvedSetOperationScan(
    const ResolvedSetOperationScan* node) {
  std::unique_ptr<QueryExpression> query_expression(new QueryExpression);
  std::vector<std::unique_ptr<QueryExpression>> set_op_scan_list;
  for (const auto& input_item : node->input_item_list()) {
    DCHECK_EQ(input_item->output_column_list_size(),
              node->column_list_size());
    const ResolvedScan* scan = input_item->scan();
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result, ProcessNode(scan));
    set_op_scan_list.push_back(std::move(result->query_expression));
    ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(
        input_item->output_column_list(), set_op_scan_list.back().get()));

    // If the query's column list doesn't exactly match the input to the
    // union, then add a wrapper query to make sure we have the right number
    // of columns and they have unique aliases.
    if (input_item->output_column_list() != scan->column_list()) {
      ZETASQL_RETURN_IF_ERROR(WrapQueryExpression(
          scan, set_op_scan_list.back().get()));
      ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(
          input_item->output_column_list(), set_op_scan_list.back().get()));
    }
  }

  // In a SetOperation scan, output columns use the column names from the first
  // subquery.
  DCHECK_GT(set_op_scan_list.size(), 0);
  const std::vector<std::pair<std::string, std::string>>& first_select_list =
      set_op_scan_list[0]->SelectList();
  // If node->column_list() was empty, first_select_list will have a NULL added.
  DCHECK_EQ(first_select_list.size(),
            std::max<std::size_t>(node->column_list_size(), 1));
  for (int i = 0; i < node->column_list_size(); i++) {
    zetasql_base::InsertOrDie(&computed_column_alias_, node->column_list(i).column_id(),
                     first_select_list[i].second);
  }

  std::string query_hints;
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &query_hints));
  }

  const auto& pair = GetOpTypePair(node->op_type());
  ZETASQL_RET_CHECK(query_expression->TrySetSetOpScanList(&set_op_scan_list, pair.first,
                                                  pair.second, query_hints));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedOrderByItem(
    const ResolvedOrderByItem* node) {
  std::string text;
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->column_ref()));
  absl::StrAppend(&text, result->GetSQL());

  if (node->collation_name() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> collation,
                     ProcessNode(node->collation_name()));
    absl::StrAppend(&text, " COLLATE ", collation->GetSQL());
  }

  absl::StrAppend(&text, node->is_descending() ? " DESC" : "");
  if (node->null_order() != ResolvedOrderByItemEnums::ORDER_UNSPECIFIED) {
    absl::StrAppend(&text,
                    node->null_order() == ResolvedOrderByItemEnums::NULLS_FIRST
                        ? " NULLS FIRST"
                        : " NULLS LAST");
  }
  PushQueryFragment(node, text);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedComputedColumn(
    const ResolvedComputedColumn* node) {
  return node->expr()->Accept(this);
}

zetasql_base::Status SQLBuilder::VisitResolvedOrderByScan(
    const ResolvedOrderByScan* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> input_result,
                   ProcessNode(node->input_scan()));
  std::unique_ptr<QueryExpression> query_expression(
      input_result->query_expression.release());
  if (!query_expression->CanSetOrderByClause()) {
    ZETASQL_RETURN_IF_ERROR(
        WrapQueryExpression(node->input_scan(), query_expression.get()));
  }

  std::string order_by_hint_list;
  ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &order_by_hint_list));

  ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(node->input_scan()->column_list(),
                                        query_expression.get()));
  std::vector<std::string> order_by_list;
  for (const auto& order_by_item : node->order_by_item_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> item_result,
                     ProcessNode(order_by_item.get()));
    order_by_list.push_back(item_result->GetSQL());
  }
  ZETASQL_RET_CHECK(
      query_expression->TrySetOrderByClause(order_by_list, order_by_hint_list));

  // We always wrap the built query_expression (with order-by clause) as a
  // subquery so that any changes made to the select alias (when we try to match
  // the output column names) would not render the order-by invalid.
  ZETASQL_RETURN_IF_ERROR(
      WrapQueryExpression(node->input_scan(), query_expression.get()));
  ZETASQL_RETURN_IF_ERROR(
      AddSelectListIfNeeded(node->column_list(), query_expression.get()));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::ProcessAggregateScanBase(
    const ResolvedAggregateScanBase* node,
    const std::vector<int>& rollup_column_id_list,
    QueryExpression* query_expression) {
  if (!query_expression->CanSetGroupByClause()) {
    ZETASQL_RETURN_IF_ERROR(WrapQueryExpression(node->input_scan(), query_expression));
  }

  std::map<int, std::string> group_by_list;
  for (const auto& computed_col : node->group_by_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(computed_col->expr()));
    zetasql_base::InsertOrDie(&group_by_list, computed_col->column().column_id(),
                     result->GetSQL());
  }

  for (const auto& computed_col : node->aggregate_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(computed_col->expr()));
    zetasql_base::InsertOrDie(&pending_columns_, computed_col->column().column_id(),
                     result->GetSQL());
  }

  std::string group_by_hints;
  ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &group_by_hints));

  ZETASQL_RET_CHECK(query_expression->TrySetGroupByClause(group_by_list, group_by_hints,
                                                  rollup_column_id_list));
  ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(node->column_list(), query_expression));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAggregateScan(
    const ResolvedAggregateScan* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> input_result,
                   ProcessNode(node->input_scan()));
  std::unique_ptr<QueryExpression> query_expression(
      input_result->query_expression.release());

  // We don't use the grouping set list in the unparsed SQL, since at the time
  // of this writing, we can't parse GROUPING SETS directly. The expectation is
  // that if there are grouping sets, there should be one for each prefix of the
  // rollup list, which includes the empty set.
  std::vector<int> rollup_column_id_list;
  if (!node->grouping_set_list().empty() ||
      !node->rollup_column_list().empty()) {
    ZETASQL_RET_CHECK_EQ(node->grouping_set_list_size(),
                 node->rollup_column_list_size() + 1);
    for (const auto& column_ref : node->rollup_column_list()) {
      rollup_column_id_list.push_back(column_ref->column().column_id());
    }
    // Mark grouping_set fields as accessed.
    for (const auto& grouping_set : node->grouping_set_list()) {
      for (const auto& group_by_column : grouping_set->group_by_column_list()) {
        group_by_column->column();
      }
    }
  }
  ZETASQL_RETURN_IF_ERROR(ProcessAggregateScanBase(node, rollup_column_id_list,
                                           query_expression.get()));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedWithScan(const ResolvedWithScan* node) {
  // Save state of the WITH alias map from the outer scope so we can restore it
  // after processing the local query.
  const std::map<std::string, const ResolvedScan*> old_with_query_name_to_scan =
      with_query_name_to_scan_;

  std::vector<std::pair<std::string, std::string>> with_list;
  for (const auto& with_entry : node->with_entry_list()) {
    const std::string name = with_entry->with_query_name();
    const ResolvedScan* scan = with_entry->with_subquery();
    zetasql_base::InsertOrDie(&with_query_name_to_scan_, name, scan);

    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result, ProcessNode(scan));
    std::unique_ptr<QueryExpression> query_expression =
        std::move(result->query_expression);
    ZETASQL_RETURN_IF_ERROR(
        AddSelectListIfNeeded(scan->column_list(), query_expression.get()));

    with_list.push_back(std::make_pair(
        ToIdentifierLiteral(name),
        absl::StrCat("(", query_expression->GetSQLQuery(), ")")));
    SetPathForColumnList(scan->column_list(), ToIdentifierLiteral(name));
  }
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->query()));
  std::unique_ptr<QueryExpression> query_expression(
      result->query_expression.release());
  ZETASQL_RET_CHECK(query_expression->TrySetWithClause(with_list));
  PushSQLForQueryExpression(node, query_expression.release());

  with_query_name_to_scan_ = old_with_query_name_to_scan;
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedWithRefScan(
    const ResolvedWithRefScan* node) {
  std::unique_ptr<QueryExpression> query_expression(new QueryExpression);
  const std::string alias = GetScanAlias(node);
  std::string from;
  absl::StrAppend(&from, ToIdentifierLiteral(node->with_query_name()), " AS ",
                  alias);
  const ResolvedScan* with_scan =
      zetasql_base::FindOrDie(with_query_name_to_scan_, node->with_query_name());
  ZETASQL_RET_CHECK_EQ(node->column_list_size(), with_scan->column_list_size());
  for (int i = 0; i < node->column_list_size(); ++i) {
    zetasql_base::InsertOrDie(&computed_column_alias_, node->column_list(i).column_id(),
                     GetColumnAlias(with_scan->column_list(i)));
  }
  SetPathForColumnList(node->column_list(), alias);
  ZETASQL_RET_CHECK(query_expression->TrySetFromClause(from));
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedSampleScan(
    const ResolvedSampleScan* node) {
  std::string sample = " TABLESAMPLE ";
  ZETASQL_RET_CHECK(!node->method().empty());
  absl::StrAppend(&sample, node->method(), " (");

  ZETASQL_RET_CHECK(node->size() != nullptr);
  if (node->size()->node_kind() == RESOLVED_LITERAL) {
    const Value value = node->size()->GetAs<ResolvedLiteral>()->value();
    ZETASQL_RET_CHECK(!value.is_null());
    ZETASQL_ASSIGN_OR_RETURN(const std::string value_sql,
                     GetSQL(value, options_.product_mode,
                            true /* is_constant_value */));
    absl::StrAppend(&sample, value_sql);
  } else {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> size,
                     ProcessNode(node->size()));
    absl::StrAppend(&sample, size->GetSQL());
  }

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> input_result,
                   ProcessNode(node->input_scan()));
  std::unique_ptr<QueryExpression> query_expression(
      input_result->query_expression.release());

  std::string from_clause;
  switch (node->input_scan()->node_kind()) {
    case RESOLVED_TABLE_SCAN: {
      // As we know the from clause here is just the table name, we will just
      // append the TABLESAMPLE clause at the end.
      from_clause = query_expression->FromClause();

      // If the PARTITION BY is present, then the column references in the
      // PARTITION BY must reference the table scan columns directly by name
      // (the PARTITION BY expressions cannot reference aliases defined in the
      // SELECT list).  So we must make columns from the input_scan available
      // to partition by node processing below.  This call will associate the
      // original table/column names with the associated column ids (so that
      // they can be referenced in the PARTITION BY).
      const ResolvedTableScan* resolved_table_scan =
          node->input_scan()->GetAs<ResolvedTableScan>();
      SetPathForColumnsInScan(
          node->input_scan(),
          zetasql_base::FindWithDefault(table_alias_map_,
                               resolved_table_scan->table(), ""));
      break;
    }
    case RESOLVED_JOIN_SCAN:
      // We explicitly parenthesize joins here so that later we can append
      // sample clause to it.
      from_clause = absl::StrCat("(", query_expression->FromClause(), ")");
      break;
    case RESOLVED_WITH_REF_SCAN: {
      // Ideally the ResolvedWithRefScan would be treated like the
      // ResolvedTableScan, so the FROM clause could just be the WITH table
      // name (and alias) with the TABLESAMPLE clause appended to the end.
      // However, the ProcessNode() call for ResolvedWithRefScan creates
      // a table alias for the WITH table and column aliases for its
      // columns which are subsequently referenced.  So we need to turn the
      // ResolvedWithRefScan into a subquery that produces all the WITH table
      // columns and also contains the sample clause.
      ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(node->input_scan()->column_list(),
                                            query_expression.get()));
      from_clause = query_expression->FromClause();
      break;
    }
    default: {
      ZETASQL_RETURN_IF_ERROR(WrapQueryExpression(node->input_scan(),
                                          query_expression.get()));
      from_clause = query_expression->FromClause();
    }
  }

  if (node->unit() == ResolvedSampleScan::ROWS) {
    absl::StrAppend(&sample, " ROWS");
    if (!node->partition_by_list().empty()) {
      absl::StrAppend(&sample, " PARTITION BY ");
      ZETASQL_RETURN_IF_ERROR(
          GetPartitionByListString(node->partition_by_list(), &sample));
    }
    absl::StrAppend(&sample, ")");
  } else {
    ZETASQL_RET_CHECK_EQ(node->unit(), ResolvedSampleScan::PERCENT);
    absl::StrAppend(&sample, " PERCENT)");
  }

  if (node->weight_column() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->weight_column()));
    absl::StrAppend(&sample, " WITH WEIGHT ", result->GetSQL());
    query_expression->MutableSelectList()->emplace_back(
        result->GetSQL() /* select column */,
        result->GetSQL() /* select alias */);
  }

  if (node->repeatable_argument() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> repeatable_argument,
                     ProcessNode(node->repeatable_argument()));
    absl::StrAppend(&sample, " REPEATABLE(", repeatable_argument->GetSQL(),
                    ")");
  }

  *(query_expression->MutableFromClause()) = absl::StrCat(from_clause, sample);
  PushSQLForQueryExpression(node, query_expression.release());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::MatchOutputColumns(
    const std::vector<std::unique_ptr<const ResolvedOutputColumn>>&
        output_column_list,
    const ResolvedScan* query, QueryExpression* query_expression) {
  ResolvedColumnList column_list;
  for (const auto& output_col : output_column_list) {
    column_list.push_back(output_col->column());
  }

  // Check whether columns in column_list and columns in query_expression's
  // select-list matches 1:1 in order.
  bool matches = false;
  if (column_list.size() == query_expression->SelectList().size()) {
    matches = true;
    // Stores the earliest occurring alias for the column-id while traversing
    // through the select-list.
    std::map<int /* column_id */, std::string /* alias */> select_aliases;
    for (int i = 0; i < column_list.size() && matches; ++i) {
      const ResolvedColumn& output_col = column_list[i];
      // Alias assigned for the column in select-list. If the column occurs more
      // than once in the select-list, we fetch the alias from the column's
      // earliest occurrence, which is registered to refer that column later in
      // the query.
      std::string select_alias =
          zetasql_base::LookupOrInsert(&select_aliases, output_col.column_id(),
                              query_expression->SelectList()[i].second);

      // As all the aliases (assigned through GetColumnAlias()) are unique
      // across column-id, to see whether the column in select-list and the
      // output_col are the same, we compare select_alias to the alias unique to
      // the output_col.
      matches = (select_alias == GetColumnAlias(output_col));
    }
  }

  if (!matches) {
    // When the select-list does not match the output-columns, we wrap the
    // earlier query_expression as a subquery to the from clause, selecting
    // columns matching the output_column_list.
    ZETASQL_RETURN_IF_ERROR(WrapQueryExpression(query, query_expression));
    ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(column_list, query_expression));
  }

  ZETASQL_RET_CHECK_EQ(output_column_list.size(),
               query_expression->SelectList().size());
  for (int i = 0; i < output_column_list.size(); ++i) {
    const std::string& output_col_alias = output_column_list[i]->name();
    if (!IsInternalAlias(output_col_alias)) {
      query_expression->SetAliasForSelectColumn(
          i /* select pos */, ToIdentifierLiteral(output_col_alias));
    }
  }
  return ::zetasql_base::OkStatus();
}

zetasql_base::StatusOr<QueryExpression*> SQLBuilder::ProcessQuery(
    const ResolvedScan* query,
    const std::vector<std::unique_ptr<const ResolvedOutputColumn>>&
        output_column_list) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result, ProcessNode(query));
  std::unique_ptr<QueryExpression> query_expression(
      result->query_expression.release());
  ZETASQL_RETURN_IF_ERROR(
      AddSelectListIfNeeded(query->column_list(), query_expression.get()));
  ZETASQL_RETURN_IF_ERROR(
      MatchOutputColumns(output_column_list, query, query_expression.get()));
  return query_expression.release();
}

zetasql_base::Status SQLBuilder::VisitResolvedQueryStmt(const ResolvedQueryStmt* node) {
  // Dummy access on the output_column_list so as to pass the final
  // CheckFieldsAccessed() on a statement level before building the sql.
  for (const auto& output_col : node->output_column_list()) {
    output_col->name();
    output_col->column();
  }

  ZETASQL_ASSIGN_OR_RETURN(
      QueryExpression* query_result,
      ProcessQuery(node->query(), node->output_column_list()));
  std::unique_ptr<QueryExpression> query_expression(query_result);

  std::string sql;
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
    absl::StrAppend(&sql, " ");
  }
  // We should get a struct or a proto if is_value_table=true.
  if (node->is_value_table()) {
    ZETASQL_RET_CHECK_EQ(query_expression->SelectList().size(), 1);
    query_expression->SetSelectAsModifier("AS VALUE");
  }
  absl::StrAppend(&sql, query_expression->GetSQLQuery());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedExplainStmt(
    const ResolvedExplainStmt* node) {
  std::string sql;
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
    absl::StrAppend(&sql, " ");
  }

  DCHECK(node->statement() != nullptr);
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->statement()));
  absl::StrAppend(&sql, "EXPLAIN ", result->GetSQL());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

static std::string GetSqlSecuritySql(
    ResolvedCreateStatementEnums::SqlSecurity sql_security) {
  switch (sql_security) {
    case ResolvedCreateStatementEnums::SQL_SECURITY_UNSPECIFIED:
      return "";
    case ResolvedCreateStatementEnums::SQL_SECURITY_INVOKER:
      return " SQL SECURITY INVOKER";
    case ResolvedCreateStatementEnums::SQL_SECURITY_DEFINER:
      return " SQL SECURITY DEFINER";
  }
}

static std::string GetDeterminismLevelSql(
    ResolvedCreateStatementEnums::DeterminismLevel level) {
  switch (level) {
    case ResolvedCreateStatementEnums::DETERMINISM_UNSPECIFIED:
      return "";
    case ResolvedCreateStatementEnums::DETERMINISM_DETERMINISTIC:
      return " DETERMINISTIC";
    case ResolvedCreateStatementEnums::DETERMINISM_NOT_DETERMINISTIC:
      return " NOT DETERMINISTIC";
    case ResolvedCreateStatementEnums::DETERMINISM_IMMUTABLE:
      return " IMMUTABLE";
    case ResolvedCreateStatementEnums::DETERMINISM_STABLE:
      return " STABLE";
    case ResolvedCreateStatementEnums::DETERMINISM_VOLATILE:
      return " VOLATILE";
  }
}

zetasql_base::Status SQLBuilder::GetCreateViewStatement(
    const ResolvedCreateViewBase* node, bool is_value_table,
    const std::string& view_type) {
  // Dummy access on the fields so as to pass the final CheckFieldsAccessed() on
  // a statement level before building the sql.
  for (const auto& output_col : node->output_column_list()) {
    output_col->name();
    output_col->column();
  }

  ZETASQL_ASSIGN_OR_RETURN(QueryExpression * query_result,
                   ProcessQuery(node->query(), node->output_column_list()));
  std::unique_ptr<QueryExpression> query_expression(query_result);

  std::string sql;
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(node, view_type, &sql));

  absl::StrAppend(&sql, GetSqlSecuritySql(node->sql_security()));

  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string result,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, "OPTIONS(", result, ") ");
  }
  // We should get a struct or a proto if is_value_table=true.
  if (is_value_table) {
    ZETASQL_RET_CHECK_EQ(query_expression->SelectList().size(), 1);
    query_expression->SetSelectAsModifier("AS VALUE");
  }
  absl::StrAppend(&sql, "AS ", query_expression->GetSQLQuery());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::GetCreateStatementPrefix(
    const ResolvedCreateStatement* node,
    const std::string& object_type,
    std::string* sql) {
  bool is_index = object_type == "INDEX";
  sql->clear();
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), sql));
    absl::StrAppend(sql, " ");
  }
  absl::StrAppend(sql, "CREATE ");
  if (node->create_mode() == node->CREATE_OR_REPLACE) {
    absl::StrAppend(sql, "OR REPLACE ");
  }
  if (is_index) {
    const auto* create_index = node->GetAs<ResolvedCreateIndexStmt>();
    ZETASQL_RET_CHECK(create_index);
    if (create_index->is_unique()) {
      absl::StrAppend(sql, "UNIQUE ");
    }
  } else {
    switch (node->create_scope()) {
      case ResolvedCreateStatement::CREATE_PRIVATE:
        absl::StrAppend(sql, "PRIVATE ");
        break;
      case ResolvedCreateStatement::CREATE_PUBLIC:
        absl::StrAppend(sql, "PUBLIC ");
        break;
      case ResolvedCreateStatement::CREATE_TEMP:
        absl::StrAppend(sql, "TEMP ");
        break;
      case ResolvedCreateStatement::CREATE_DEFAULT_SCOPE:
        break;
    }
  }
  absl::StrAppend(sql, object_type, " ");
  if (node->create_mode() == node->CREATE_IF_NOT_EXISTS) {
    absl::StrAppend(sql, "IF NOT EXISTS ");
  }
  absl::StrAppend(sql, IdentifierPathToString(node->name_path()), " ");
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::GetPartitionByListString(
    const std::vector<std::unique_ptr<const ResolvedExpr>>& partition_by_list,
    std::string* sql) {
  DCHECK(!partition_by_list.empty());

  std::vector<std::string> expressions;
  expressions.reserve(partition_by_list.size());
  for (const auto& partition_by_expr : partition_by_list) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> expr,
                     ProcessNode(partition_by_expr.get()));
    expressions.push_back(expr->GetSQL());
  }
  absl::StrAppend(sql, absl::StrJoin(expressions, ","));

  return ::zetasql_base::OkStatus();
}

std::string SQLBuilder::GetOptionalObjectType(const std::string& object_type) {
  if (object_type.empty()) {
    return "";
  } else {
    return absl::StrCat(ToIdentifierLiteral(object_type), " ");
  }
}

zetasql_base::Status SQLBuilder::GetPrivilegesString(
    const ResolvedGrantOrRevokeStmt* node, std::string* sql) {
  std::vector<std::string> privilege_list_sql;
  for (const auto& privilege : node->privilege_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(privilege.get()));
    privilege_list_sql.push_back(result->GetSQL());
  }

  *sql = absl::StrCat(
      (privilege_list_sql.empty() ? "ALL PRIVILEGES"
                                  : absl::StrJoin(privilege_list_sql, ", ")),
      " ON ", GetOptionalObjectType(node->object_type()),
      IdentifierPathToString(node->name_path()));

  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateDatabaseStmt(
    const ResolvedCreateDatabaseStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "CREATE DATABASE ");
  absl::StrAppend(&sql, IdentifierPathToString(node->name_path()));
  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, " OPTIONS(", options_string, ")");
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

static std::string GetColumnListSql(const std::vector<int>& column_index_list,
                               const std::function<std::string(int)>& get_name) {
  std::vector<std::string> column_names;
  column_names.reserve(column_index_list.size());
  for (auto index : column_index_list) {
    column_names.push_back(get_name(index));
  }
  return absl::StrCat("(", absl::StrJoin(column_names, ","), ")");
}

static std::string GetForeignKeyMatchSql(ResolvedForeignKey::MatchMode mode) {
  switch (mode) {
    case ResolvedForeignKey::SIMPLE:
      return "SIMPLE";
    case ResolvedForeignKey::FULL:
      return "FULL";
    case ResolvedForeignKey::NOT_DISTINCT:
      return "NOT DISTINCT";
  }
}

static std::string GetForeignKeyActionSql(
    ResolvedForeignKey::ActionOperation action) {
  switch (action) {
    case ResolvedForeignKey::NO_ACTION:
      return "NO ACTION";
    case ResolvedForeignKey::RESTRICT:
      return "RESTRICT";
    case ResolvedForeignKey::CASCADE:
      return "CASCADE";
    case ResolvedForeignKey::SET_NULL:
      return "SET NULL";
  }
}

zetasql_base::StatusOr<std::string> SQLBuilder::ProcessCreateTableStmtBase(
    const ResolvedCreateTableStmtBase* node,
    bool process_column_definitions) {
  std::string sql;

  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(node, "TABLE", &sql));
  // Make column aliases available for PARTITION BY, CLUSTER BY and table
  // constraints.
  for (const auto& column_definition : node->column_definition_list()) {
    computed_column_alias_[column_definition->column().column_id()] =
        column_definition->name();
  }
  for (const ResolvedColumn& column : node->pseudo_column_list()) {
    computed_column_alias_[column.column_id()] = column.name();
  }
  if (process_column_definitions) {
    absl::StrAppend(&sql, "(");
    std::vector<std::string> table_elements;
    // Go through each column and generate the SQL corresponding to it.
    for (const auto& c : node->column_definition_list()) {
      std::string table_element = absl::StrCat(ToIdentifierLiteral(c->name()), " ");
      ZETASQL_RETURN_IF_ERROR(
          AppendColumnSchema(c->type(), c->is_hidden(), c->annotations(),
                             c->generated_column_info(), &table_element));
      table_elements.push_back(std::move(table_element));
    }
    if (node->primary_key() != nullptr) {
      const auto* pk = node->primary_key();
      std::string primary_key = "PRIMARY KEY";
      absl::StrAppend(&primary_key,
                      GetColumnListSql(
                          pk->column_offset_list(),
                          [node](int i) {
                            return node->column_definition_list()[i]->name();
                          }),
                      " ");
      ZETASQL_RETURN_IF_ERROR(AppendOptionsIfPresent(pk->option_list(), &primary_key));
      table_elements.push_back(primary_key);
    }
    for (const auto& fk : node->foreign_key_list()) {
      std::string foreign_key;
      if (!fk->constraint_name().empty()) {
        absl::StrAppend(&foreign_key,
                        "CONSTRAINT ", fk->constraint_name(), " ");
      }
      absl::StrAppend(&foreign_key,
                      "FOREIGN KEY",
                      GetColumnListSql(
                          fk->referencing_column_offset_list(),
                          [node](int i) {
                            return node->column_definition_list()[i]->name();
                          }),
                      " ");
      absl::StrAppend(&foreign_key,
                      "REFERENCES ",
                      fk->referenced_table()->Name(),
                      GetColumnListSql(
                          fk->referenced_column_offset_list(),
                          [&fk](int i) {
                            return fk->referenced_table()->GetColumn(i)->Name();
                          }),
                      " ");
      absl::StrAppend(&foreign_key, "MATCH ",
                      GetForeignKeyMatchSql(fk->match_mode()), " ");
      absl::StrAppend(&foreign_key, "ON UPDATE ",
                      GetForeignKeyActionSql(fk->update_action()), " ");
      absl::StrAppend(&foreign_key, "ON DELETE ",
                      GetForeignKeyActionSql(fk->delete_action()), " ");
      if (!fk->enforced()) {
        absl::StrAppend(&foreign_key, "NOT ");
      }
      absl::StrAppend(&foreign_key, "ENFORCED");
      ZETASQL_RETURN_IF_ERROR(AppendOptionsIfPresent(fk->option_list(), &foreign_key));
      table_elements.push_back(foreign_key);
    }
    for (const auto& check_constraint : node->check_constraint_list()) {
      std::string check_constraint_sql;
      if (!check_constraint->constraint_name().empty()) {
        absl::StrAppend(&check_constraint_sql, "CONSTRAINT ",
                        check_constraint->constraint_name(), " ");
      }
      absl::StrAppend(&check_constraint_sql, "CHECK (");
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(check_constraint->expression()));
      absl::StrAppend(&check_constraint_sql, result->GetSQL(), ") ");
      if (!check_constraint->enforced()) {
        absl::StrAppend(&check_constraint_sql, "NOT ");
      }
      absl::StrAppend(&check_constraint_sql, "ENFORCED");
      ZETASQL_RETURN_IF_ERROR(AppendOptionsIfPresent(check_constraint->option_list(),
                                             &check_constraint_sql));
      table_elements.push_back(check_constraint_sql);
    }
    absl::StrAppend(&sql, absl::StrJoin(table_elements, ", "));
    absl::StrAppend(&sql, ")");
  }
  if (!node->partition_by_list().empty()) {
    absl::StrAppend(&sql, " PARTITION BY ");
    ZETASQL_RETURN_IF_ERROR(GetPartitionByListString(node->partition_by_list(), &sql));
  }

  if (!node->cluster_by_list().empty()) {
    absl::StrAppend(&sql, " CLUSTER BY ");
    ZETASQL_RETURN_IF_ERROR(GetPartitionByListString(node->cluster_by_list(), &sql));
  }

  if (!node->option_list().empty()) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, " OPTIONS(", options_string, ") ");
  }

  return sql;
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateTableStmt(
    const ResolvedCreateTableStmt* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::string sql,
                   ProcessCreateTableStmtBase(
                       node, /* process_column_definitions = */ true));
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateTableAsSelectStmt(
    const ResolvedCreateTableAsSelectStmt* node) {
  // Dummy access on the fields so as to pass the final CheckFieldsAccessed() on
  // a statement level before building the sql.
  for (const auto& output_col : node->output_column_list()) {
    output_col->name();
    output_col->column();
  }

  ZETASQL_ASSIGN_OR_RETURN(
      QueryExpression* query_result,
      ProcessQuery(node->query(), node->output_column_list()));
  std::unique_ptr<QueryExpression> query_expression(query_result);

  // Print the column definition list only if it contains more information
  // than the output column list.
  bool process_column_definitions =
      (node->primary_key() != nullptr || !node->foreign_key_list().empty() ||
       !node->check_constraint_list().empty());
  if (!process_column_definitions) {
    for (const auto& column_def : node->column_definition_list()) {
      column_def->name();  // Mark accessed.
      column_def->type();  // Mark accessed.
      if (column_def->annotations() != nullptr ||
          column_def->is_hidden() ||
          column_def->generated_column_info() != nullptr) {
        process_column_definitions = true;
        break;
      }
    }
  }
  ZETASQL_ASSIGN_OR_RETURN(
      std::string sql,
      ProcessCreateTableStmtBase(node, process_column_definitions));
  // We should get a struct or a proto if is_value_table=true.
  if (node->is_value_table()) {
    ZETASQL_RET_CHECK_EQ(query_expression->SelectList().size(), 1);
    query_expression->SetSelectAsModifier(" AS VALUE");
  }
  absl::StrAppend(&sql, " AS ", query_expression->GetSQLQuery());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateModelStmt(
    const ResolvedCreateModelStmt* node) {
  // Dummy access on the fields so as to pass the final CheckFieldsAccessed() on
  // a statement level before building the sql.
  for (const auto& output_col : node->output_column_list()) {
    output_col->name();
    output_col->column();
  }

  std::string sql;
  // Restore CREATE MODEL sql prefix.
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(node, "MODEL", &sql));

  // Restore SELECT statement.
  // Note this step must run before Restore TRANSFORM clause to fill
  // computed_column_alias_.
  ZETASQL_ASSIGN_OR_RETURN(QueryExpression * query_result,
                   ProcessQuery(node->query(), node->output_column_list()));
  std::unique_ptr<QueryExpression> query_expression(query_result);

  // Restore TRANSFORM clause.
  if (!node->transform_list().empty()) {
    DCHECK_EQ(node->transform_list_size(),
              node->transform_output_column_list_size());
    // Rename columns in TRANSFORM with the aliases from SELECT statement.
    for (const auto& output_col : node->output_column_list()) {
      const int output_col_id = output_col->column().column_id();
      const std::string alias = output_col->name();
      if (!zetasql_base::ContainsKey(computed_column_alias_, output_col_id)) {
        return ::zetasql_base::InternalErrorBuilder() << absl::Substitute(
                   "Column id $0 with name '$1' is not found in "
                   "computed_column_alias_",
                   output_col_id, alias);
      }
      computed_column_alias_[output_col_id] = alias;
    }
    for (const auto& analytic_function_group :
         node->transform_analytic_function_group_list()) {
      ZETASQL_RETURN_IF_ERROR(
          VisitResolvedAnalyticFunctionGroup(analytic_function_group.get()));
    }

    // Assemble TRANSFORM clause sql std::string.
    std::vector<std::string> transform_list_strs;
    for (int i = 0; i < node->transform_list_size(); ++i) {
      const ResolvedComputedColumn* transform_element = node->transform_list(i);
      const ResolvedOutputColumn* transform_output_col =
          node->transform_output_column_list(i);
      // Dummy access.
      transform_output_col->column();
      ZETASQL_ASSIGN_OR_RETURN(
          std::unique_ptr<QueryFragment> transform_element_fragment,
          ProcessNode(transform_element));
      std::string expr_sql = transform_element_fragment->GetSQL();
      // The column name of ResolvedComputedColumn is always set as alias. Even
      // when the original TRANSFORM is 'a', the unparsed sql will be 'a AS a'.
      absl::StrAppend(&expr_sql, " AS ", transform_output_col->name());
      transform_list_strs.push_back(expr_sql);
    }
    absl::StrAppend(&sql, " TRANSFORM(",
                    absl::StrJoin(transform_list_strs, ", "), ")");
  }

  // Restore OPTIONS list.
  if (!node->option_list().empty()) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, " OPTIONS(", options_string, ") ");
  }

  // Append SELECT statement.
  absl::StrAppend(&sql, " AS ", query_expression->GetSQLQuery());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateIndexStmt(
    const ResolvedCreateIndexStmt* node) {
  std::string sql;
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(node, "INDEX", &sql));
  absl::StrAppend(&sql, "ON ");
  absl::StrAppend(&sql, IdentifierPathToString(node->table_name_path()));

  const ResolvedTableScan* table_scan = node->table_scan();
  ZETASQL_RET_CHECK(table_scan != nullptr);
  // Dummy access so that we can pass CheckFieldsAccessed().
  ZETASQL_RET_CHECK(table_scan->table() != nullptr);

  if (table_scan->table()->IsValueTable()) {
    if (table_scan->column_list_size() > 0) {
      // Set the path of the value column as the table name. This is consistent
      // with the spec in (broken link).
      SetPathForColumn(table_scan->column_list(0), table_scan->table()->Name());
      for (int i = 1; i < table_scan->column_list_size(); i++) {
        SetPathForColumn(
            table_scan->column_list(i),
            ToIdentifierLiteral(table_scan->column_list(i).name()));
      }
    }
  } else {
    for (const ResolvedColumn& column : table_scan->column_list()) {
      SetPathForColumn(column, ToIdentifierLiteral(column.name()));
    }
  }

  if (!node->unnest_expressions_list().empty()) {
    absl::StrAppend(&sql, "\n");
    for (const auto& index_unnest_expression :
        node->unnest_expressions_list()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(index_unnest_expression->array_expr()));
      absl::StrAppend(&sql, "UNNEST(", result->GetSQL(), ") ",
                      ToIdentifierLiteral(
                          index_unnest_expression->element_column().name()));
      SetPathForColumn(index_unnest_expression->element_column(),
                       ToIdentifierLiteral(
                           index_unnest_expression->element_column().name()));
      if (index_unnest_expression->array_offset_column() != nullptr) {
        absl::StrAppend(
            &sql, " WITH OFFSET ",
            ToIdentifierLiteral(index_unnest_expression->array_offset_column()
                                    ->column().name()));
        SetPathForColumn(
            index_unnest_expression->array_offset_column()->column(),
            ToIdentifierLiteral(index_unnest_expression->array_offset_column()
                                    ->column()
                                    .name()));
      }
      absl::StrAppend(&sql, "\n");
    }
  }

  absl::StrAppend(&sql, "(");
  std::vector<std::string> cols;
  absl::flat_hash_map</*column_id=*/int, const ResolvedExpr*>
      computed_column_expressions;
  for (const auto& computed_column : node->computed_columns_list()) {
    computed_column_expressions.insert(
        {computed_column->column().column_id(), computed_column->expr()});
  }
  for (const auto& item : node->index_item_list()) {
    const int column_id = item->column_ref()->column().column_id();
    const ResolvedExpr* resolved_expr =
        zetasql_base::FindPtrOrNull(computed_column_expressions, column_id);
    if (resolved_expr == nullptr) {
      // The index key is on the table column, array element column, or offset
      // column.
      cols.push_back(absl::StrCat(GetColumnPath(item->column_ref()->column()),
                                  item->descending() ? " DESC" : ""));
      continue;
    }
    // The index key is an extracted expression.
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(resolved_expr));
    cols.push_back(
        absl::StrCat(result->GetSQL(), item->descending() ? " DESC" : ""));
  }
  absl::StrAppend(&sql, absl::StrJoin(cols, ","), ") ");

  if (!node->storing_expression_list().empty()) {
    std::vector<std::string> argument_list;
    argument_list.reserve(node->storing_expression_list_size());
    for (int i = 0; i < node->storing_expression_list_size(); ++i) {
      const ResolvedExpr* argument = node->storing_expression_list(i);
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(argument));
      argument_list.push_back(result->GetSQL());
    }
    absl::StrAppend(
        &sql, "STORING(",
        absl::StrJoin(argument_list.begin(), argument_list.end(), ","), ")");
  }

  ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                   GetHintListString(node->option_list()));
  if (!options_string.empty()) {
    absl::StrAppend(&sql, "OPTIONS(", options_string, ") ");
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateViewStmt(
    const ResolvedCreateViewStmt* node) {
  return GetCreateViewStatement(node, node->is_value_table(), "VIEW");
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateMaterializedViewStmt(
    const ResolvedCreateMaterializedViewStmt* node) {
  // Dummy access on the fields so as to pass the final CheckFieldsAccessed() on
  // a statement level before building the sql.
  for (const auto& output_col : node->output_column_list()) {
    output_col->name();
    output_col->column();
  }
  for (const auto& column_def : node->column_definition_list()) {
    column_def->name();
    column_def->type();
  }

  ZETASQL_ASSIGN_OR_RETURN(QueryExpression * query_result,
                   ProcessQuery(node->query(), node->output_column_list()));
  std::unique_ptr<QueryExpression> query_expression(query_result);

  std::string sql;
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(node, "MATERIALIZED VIEW", &sql));

  // Make column aliases available for PARTITION BY, CLUSTER BY.
  for (const auto& column_definition : node->column_definition_list()) {
    computed_column_alias_[column_definition->column().column_id()] =
        column_definition->name();
  }
  absl::StrAppend(&sql, GetSqlSecuritySql(node->sql_security()));

  if (!node->partition_by_list().empty()) {
    absl::StrAppend(&sql, " PARTITION BY ");
    ZETASQL_RETURN_IF_ERROR(GetPartitionByListString(node->partition_by_list(), &sql));
  }
  if (!node->cluster_by_list().empty()) {
    absl::StrAppend(&sql, " CLUSTER BY ");
    ZETASQL_RETURN_IF_ERROR(GetPartitionByListString(node->cluster_by_list(), &sql));
  }
  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string result,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, " OPTIONS(", result, ")");
  }
  // We should get a struct or a proto if is_value_table=true.
  if (node->is_value_table()) {
    ZETASQL_RET_CHECK_EQ(query_expression->SelectList().size(), 1);
    query_expression->SetSelectAsModifier(" AS VALUE");
  }
  absl::StrAppend(&sql, " AS ", query_expression->GetSQLQuery());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateExternalTableStmt(
    const ResolvedCreateExternalTableStmt* node) {
  std::string sql;
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(node, "EXTERNAL TABLE", &sql));

  ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                   GetHintListString(node->option_list()));
  absl::StrAppend(&sql, "OPTIONS(", options_string, ") ");

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::StatusOr<std::string> SQLBuilder::GetFunctionArgListString(
    const std::vector<std::string>& arg_name_list,
    const FunctionSignature& signature) {
  if (signature.arguments().empty()) {
    return std::string();  // no args
  }
  std::vector<std::string> arg_list_sql;
  arg_list_sql.reserve(arg_name_list.size());
  for (int i = 0; i < arg_name_list.size(); ++i) {
    std::string procedure_argument_mode;
    FunctionEnums::ProcedureArgumentMode mode =
        signature.argument(i).options().procedure_argument_mode();
    if (mode != FunctionEnums::NOT_SET) {
      procedure_argument_mode =
          absl::StrCat(FunctionEnums::ProcedureArgumentMode_Name(mode), " ");
    }
    arg_list_sql.push_back(absl::StrCat(
        procedure_argument_mode,
        arg_name_list[i], " ",
        signature.argument(i).GetSQLDeclaration(options_.product_mode),
        signature.argument(i).options().is_not_aggregate() ? " NOT AGGREGATE"
                                                           : ""));
  }
  return absl::StrJoin(arg_list_sql, ", ");
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateConstantStmt(
    const ResolvedCreateConstantStmt* node) {
  std::string sql;
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(
      node, "CONSTANT", &sql));
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> expr_fragment,
                   ProcessNode(node->expr()));
  absl::StrAppend(&sql, " = ", expr_fragment->GetSQL());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateFunctionStmt(
    const ResolvedCreateFunctionStmt* node) {
  std::string sql;
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(
      node, node->is_aggregate() ? "AGGREGATE FUNCTION" : "FUNCTION", &sql));
  ZETASQL_ASSIGN_OR_RETURN(
      const std::string args,
      GetFunctionArgListString(node->argument_name_list(), node->signature()));
  absl::StrAppend(&sql, node->signature().GetSQLDeclaration(
                            node->argument_name_list(), options_.product_mode));
  absl::StrAppend(&sql, GetDeterminismLevelSql(node->determinism_level()));
  bool is_sql_defined = absl::AsciiStrToUpper(node->language()) == "SQL";
  if (!is_sql_defined) {
    absl::StrAppend(&sql, " LANGUAGE ", ToIdentifierLiteral(node->language()));
  }

  absl::StrAppend(&sql, GetSqlSecuritySql(node->sql_security()));

  // If we have aggregates, extract the strings for the aggregate expressions
  // and store them in pending_columns_ so they will be substituted into
  // the main expression body.
  for (const auto& computed_col : node->aggregate_expression_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(computed_col->expr()));
    zetasql_base::InsertOrDie(&pending_columns_, computed_col->column().column_id(),
                     result->GetSQL());
  }
  node->is_aggregate();  // Mark as accessed.

  if (node->function_expression() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> expr,
                     ProcessNode(node->function_expression()));
    absl::StrAppend(&sql, " AS (", expr->GetSQL(), ")");
  } else if (is_sql_defined) {
    // This case covers SQL defined function templates that do not have a
    // resolved function_expression().
    absl::StrAppend(&sql, " AS (", node->code(), ")");
  } else {
    absl::StrAppend(&sql, " AS ", ToStringLiteral(node->code()));
  }
  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, " OPTIONS(", options_string, ") ");
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateTableFunctionStmt(
    const ResolvedCreateTableFunctionStmt* node) {
  std::string function_type = "TABLE FUNCTION";
  std::string sql;
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(node, function_type, &sql));

  absl::StrAppend(&sql, node->signature().GetSQLDeclaration(
                            node->argument_name_list(), options_.product_mode));

  absl::StrAppend(&sql, GetSqlSecuritySql(node->sql_security()));

  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, " OPTIONS(", options_string, ") ");
  }
  bool is_sql_language = zetasql_base::CaseEqual(node->language(), "SQL");
  bool is_undeclared_language = zetasql_base::CaseEqual(node->language(), "UNDECLARED");
  bool is_external_language = !is_sql_language && !is_undeclared_language;
  if (is_external_language) {
    absl::StrAppend(&sql, " LANGUAGE ", ToIdentifierLiteral(node->language()));
    ZETASQL_RET_CHECK(node->output_column_list().empty());
  }

  if (node->query() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(QueryExpression * query_result,
                     ProcessQuery(node->query(),
                                  node->output_column_list()));
    std::unique_ptr<QueryExpression> query_expression(query_result);
    absl::StrAppend(&sql, " AS ", query_expression->GetSQLQuery());
  } else if (!node->code().empty()) {
    if (is_external_language) {
      absl::StrAppend(&sql, " AS ", ToStringLiteral(node->code()));
    } else {
      ZETASQL_RET_CHECK(is_sql_language);
      absl::StrAppend(&sql, " AS (", node->code(), ")");
    }
  }

  if (node->query() != nullptr) {
    ZETASQL_RET_CHECK(!node->output_column_list().empty());
  } else {
    ZETASQL_RET_CHECK(node->output_column_list().empty());
  }
  // Dummy access on is_value_table field so as to pass the final
  // CheckFieldsAccessed() on a statement level before building the sql.
  node->is_value_table();
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateProcedureStmt(
    const ResolvedCreateProcedureStmt* node) {
  std::string sql;
  ZETASQL_RETURN_IF_ERROR(GetCreateStatementPrefix(node, "PROCEDURE", &sql));
  ZETASQL_ASSIGN_OR_RETURN(
      const std::string args,
      GetFunctionArgListString(node->argument_name_list(), node->signature()));
  absl::StrAppend(&sql, node->signature().GetSQLDeclaration(
                            node->argument_name_list(), options_.product_mode));
  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, " OPTIONS(", options_string, ") ");
  }

  absl::StrAppend(&sql, node->procedure_body());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedArgumentDef(
    const ResolvedArgumentDef* node) {
  PushQueryFragment(
      node,
      absl::StrCat(ToIdentifierLiteral(node->name()), " ",
                   node->type()->TypeName(options_.product_mode),
                   node->argument_kind() == ResolvedArgumentDef::NOT_AGGREGATE
                       ? " NOT AGGREGATE"
                       : ""));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedArgumentRef(
    const ResolvedArgumentRef* node) {
  PushQueryFragment(node, ToIdentifierLiteral(node->name()));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedExportDataStmt(
    const ResolvedExportDataStmt* node) {
  // Dummy access on the fields so as to pass the final CheckFieldsAccessed() on
  // a statement level before building the sql.
  for (const auto& output_col : node->output_column_list()) {
    output_col->name();
    output_col->column();
  }

  ZETASQL_ASSIGN_OR_RETURN(
      QueryExpression* query_result,
      ProcessQuery(node->query(), node->output_column_list()));
  std::unique_ptr<QueryExpression> query_expression(query_result);

  std::string sql;
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
    absl::StrAppend(&sql, " ");
  }
  absl::StrAppend(&sql, "EXPORT DATA ");
  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string result,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, "OPTIONS(", result, ") ");
  }
  // We should get a struct or a proto if is_value_table=true.
  if (node->is_value_table()) {
    ZETASQL_RET_CHECK_EQ(query_expression->SelectList().size(), 1);
    query_expression->SetSelectAsModifier("AS VALUE");
  }
  absl::StrAppend(&sql, "AS ", query_expression->GetSQLQuery());

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCallStmt(const ResolvedCallStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "CALL ");
  absl::StrAppend(&sql, IdentifierPathToString(node->procedure()->name_path()));

  std::vector<std::string> argument_list;
  argument_list.reserve(node->argument_list_size());
  for (int i = 0; i < node->argument_list_size(); ++i) {
    const ResolvedExpr* argument = node->argument_list(i);
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(argument));
    argument_list.push_back(result->GetSQL());
  }
  absl::StrAppend(&sql, "(", absl::StrJoin(argument_list, ", "), ")");
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDefineTableStmt(
    const ResolvedDefineTableStmt* node) {
  std::string sql;
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
    absl::StrAppend(&sql, " ");
  }
  absl::StrAppend(&sql, "DEFINE TABLE ",
                  IdentifierPathToString(node->name_path()));
  ZETASQL_ASSIGN_OR_RETURN(const std::string result, GetHintListString(node->option_list()));
  absl::StrAppend(&sql, "(", result, ")");

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDescribeStmt(
    const ResolvedDescribeStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "DESCRIBE ", GetOptionalObjectType(node->object_type()),
                  IdentifierPathToString(node->name_path()));
  if (!node->from_name_path().empty()) {
    absl::StrAppend(&sql, " FROM ",
                    IdentifierPathToString(node->from_name_path()));
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedShowStmt(
    const ResolvedShowStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "SHOW ", ToIdentifierLiteral(node->identifier()));
  if (!node->name_path().empty()) {
    absl::StrAppend(&sql, " FROM ", IdentifierPathToString(node->name_path()));
  }
  if (node->like_expr() != nullptr) {
    const Value value = node->like_expr()->value();
    ZETASQL_RET_CHECK(!value.is_null());
    ZETASQL_ASSIGN_OR_RETURN(const std::string result, GetSQL(value, options_.product_mode));
    absl::StrAppend(&sql, " LIKE ", result);
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedBeginStmt(
    const ResolvedBeginStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "BEGIN TRANSACTION");
  std::vector<std::string> modes;
  switch (node->read_write_mode()) {
    case ResolvedBeginStmtEnums::MODE_UNSPECIFIED:
      break;
    case ResolvedBeginStmtEnums::MODE_READ_ONLY:
      modes.push_back("READ ONLY");
      break;
    case ResolvedBeginStmtEnums::MODE_READ_WRITE:
      modes.push_back("READ WRITE");
      break;
  }

  if (!node->isolation_level_list().empty()) {
    modes.push_back("ISOLATION LEVEL");
    for (const std::string& part : node->isolation_level_list()) {
      absl::StrAppend(&modes.back(), " ", ToIdentifierLiteral(part));
    }
  }
  if (!modes.empty()) {
    absl::StrAppend(&sql, " ", absl::StrJoin(modes, ", "));
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedSetTransactionStmt(
    const ResolvedSetTransactionStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "SET TRANSACTION");
  std::vector<std::string> modes;
  switch (node->read_write_mode()) {
    case ResolvedBeginStmtEnums::MODE_UNSPECIFIED:
      break;
    case ResolvedBeginStmtEnums::MODE_READ_ONLY:
      modes.push_back("READ ONLY");
      break;
    case ResolvedBeginStmtEnums::MODE_READ_WRITE:
      modes.push_back("READ WRITE");
      break;
  }
  if (!node->isolation_level_list().empty()) {
    modes.push_back("ISOLATION LEVEL");
    for (const std::string& part : node->isolation_level_list()) {
      absl::StrAppend(&modes.back(), " ", ToIdentifierLiteral(part));
    }
  }
  if (!modes.empty()) {
    absl::StrAppend(&sql, " ", absl::StrJoin(modes, ", "));
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedCommitStmt(
    const ResolvedCommitStmt* node) {
  PushQueryFragment(node, "COMMIT");
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedRollbackStmt(
    const ResolvedRollbackStmt* node) {
  PushQueryFragment(node, "ROLLBACK");
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedStartBatchStmt(
    const ResolvedStartBatchStmt* node) {
  std::string sql = "START BATCH";
  if (!node->batch_type().empty()) {
    absl::StrAppend(&sql, " ", node->batch_type());
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedRunBatchStmt(
    const ResolvedRunBatchStmt* node) {
  PushQueryFragment(node, "RUN BATCH");
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAbortBatchStmt(
    const ResolvedAbortBatchStmt* node) {
  PushQueryFragment(node, "ABORT BATCH");
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAssertStmt(
    const ResolvedAssertStmt* node) {
  std::string sql;
  const ResolvedExpr* expr = node->expression();
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result, ProcessNode(expr));
  absl::StrAppend(&sql, "ASSERT ", result->GetSQL());
  if (!node->description().empty()) {
    absl::StrAppend(&sql, " AS ", ToStringLiteral(node->description()));
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAssertRowsModified(
    const ResolvedAssertRowsModified* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->rows()));
  PushQueryFragment(node,
                    absl::StrCat("ASSERT_ROWS_MODIFIED ", result->GetSQL()));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDeleteStmt(
    const ResolvedDeleteStmt* node) {
  std::string sql;
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
    absl::StrAppend(&sql, " ");
  }

  std::string target_sql;
  if (node->table_scan() != nullptr) {
    const Table* table = node->table_scan()->table();
    const std::string alias = GetScanAlias(node->table_scan());
    if (table->IsValueTable()) {
      // Use table alias instead for referring to the value table column.
      // This code is wrong.  See http://b/37291554.
      SetPathForColumn(node->table_scan()->column_list(0),
                       ToIdentifierLiteral(alias));
    } else {
      for (const ResolvedColumn& column : node->table_scan()->column_list()) {
        SetPathForColumn(
            column,
            absl::StrCat(alias, ".", ToIdentifierLiteral(column.name())));
      }
    }
    target_sql =
        absl::StrCat(ToIdentifierLiteral(table->Name()), " AS ", alias);
  } else {
    ZETASQL_RET_CHECK(!nested_dml_targets_.empty());
    target_sql = nested_dml_targets_.back().first;
    if (nested_dml_targets_.back().second != kEmptyAlias) {
      absl::StrAppend(&target_sql, " ", nested_dml_targets_.back().second);
    }
  }
  ZETASQL_RET_CHECK(!target_sql.empty());
  absl::StrAppend(&sql, "DELETE ", target_sql);

  if (node->array_offset_column() != nullptr) {
    const ResolvedColumn& offset_column = node->array_offset_column()->column();
    const std::string offset_alias = ToIdentifierLiteral(offset_column.name());
    SetPathForColumn(offset_column, offset_alias);
    absl::StrAppend(&sql, " WITH OFFSET AS ", offset_alias);
  }
  if (node->where_expr() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> where,
                     ProcessNode(node->where_expr()));
    absl::StrAppend(&sql, " WHERE ", where->GetSQL());
  }
  if (node->assert_rows_modified() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> assert_rows_modified,
                     ProcessNode(node->assert_rows_modified()));
    absl::StrAppend(&sql, " ", assert_rows_modified->GetSQL());
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDropStmt(
    const ResolvedDropStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "DROP ", ToIdentifierLiteral(node->object_type()),
                  node->is_if_exists() ? " IF EXISTS " : " ",
                  IdentifierPathToString(node->name_path()));
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDropFunctionStmt(
    const ResolvedDropFunctionStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "DROP FUNCTION ",
                  node->is_if_exists() ? " IF EXISTS " : " ",
                  IdentifierPathToString(node->name_path()));
  if (node->signature() != nullptr) {
    absl::StrAppend(&sql, node->signature()->signature().GetSQLDeclaration(
                              {} /* arg_name_list */, options_.product_mode));
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDropMaterializedViewStmt(
    const ResolvedDropMaterializedViewStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "DROP MATERIALIZED VIEW",
                  node->is_if_exists() ? " IF EXISTS " : " ",
                  IdentifierPathToString(node->name_path()));
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDropRowAccessPolicyStmt(
    const ResolvedDropRowAccessPolicyStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "DROP ");
  if (node->is_drop_all()) {
    absl::StrAppend(&sql, "ALL ROW ACCESS POLICIES");
  } else {
    absl::StrAppend(&sql, "ROW ACCESS POLICY",
                    node->is_if_exists() ? " IF EXISTS " : " ",
                    ToIdentifierLiteral(node->name()));
  }
  absl::StrAppend(&sql, " ON ",
                  IdentifierPathToString(node->target_name_path()));
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedTruncateStmt(
    const ResolvedTruncateStmt* node) {
  std::string sql = "TRUNCATE TABLE ";
  ZETASQL_RET_CHECK(node->table_scan() != nullptr) << "Missing target table.";
  std::string name_path = ToIdentifierLiteral(node->table_scan()->table()->Name());
  ZETASQL_RET_CHECK(!name_path.empty());
  absl::StrAppend(&sql, name_path, " ");

  // Make column aliases available for WHERE expression
  for (const auto& column_definition : node->table_scan()->column_list()) {
    computed_column_alias_[column_definition.column_id()] =
        column_definition.name();
  }

  if (node->where_expr() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> where,
                     ProcessNode(node->where_expr()));
    absl::StrAppend(&sql, " WHERE ", where->GetSQL());
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDMLDefault(
    const ResolvedDMLDefault* node) {
  PushQueryFragment(node, "DEFAULT");
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedDMLValue(
    const ResolvedDMLValue* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->value()));
  PushQueryFragment(node, result->GetSQL());
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedUpdateItem(
    const ResolvedUpdateItem* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> target,
                   ProcessNode(node->target()));
  ZETASQL_RET_CHECK(!update_item_targets_and_offsets_.empty());
  // Use an empty offset for now. VisitResolvedUpdateArrayItem will fill it in
  // later if needed.
  update_item_targets_and_offsets_.back().emplace_back(target->GetSQL(),
                                                       /*offset_sql=*/"");

  if (node->array_update_list_size() > 0) {
    // Use kEmptyAlias as the path so that VisitResolvedGet{Proto,Struct}Field
    // will print "foo" instead of <column>.foo.
    SetPathForColumn(node->element_column()->column(), kEmptyAlias);

    std::vector<std::string> sql_fragments;
    for (const auto& array_item : node->array_update_list()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> fragment,
                       ProcessNode(array_item.get()));
      sql_fragments.push_back(fragment->GetSQL());
    }

    PushQueryFragment(node, absl::StrJoin(sql_fragments, ", "));
  } else {
    std::string target_sql;
    for (int i = 0; i < update_item_targets_and_offsets_.back().size(); ++i) {
      const auto& target_and_offset =
          update_item_targets_and_offsets_.back()[i];
      const std::string& target = target_and_offset.first;
      const std::string& offset = target_and_offset.second;

      const bool last =
          (i == update_item_targets_and_offsets_.back().size() - 1);
      ZETASQL_RET_CHECK_EQ(last, offset.empty());

      // The ResolvedColumn representing an array element has path
      // kEmptyAlias. It is suppressed by VisitResolvedGet{Proto,Struct}Field,
      // but if we are modifying the element and not a field of it, then we need
      // to suppress the std::string here (or else we would get something like
      // a[OFFSET(1)]``).
      if (target != kEmptyAlias) {
        if (i == 0) {
          target_sql = target;
        } else {
          absl::StrAppend(&target_sql, ".", target);
        }
      } else {
        ZETASQL_RET_CHECK(last);
      }

      if (!offset.empty()) {
        absl::StrAppend(&target_sql, "[OFFSET(", offset, ")]");
      }
    }

    std::string sql;
    if (node->set_value() != nullptr) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> value,
                       ProcessNode(node->set_value()));
      absl::StrAppend(&sql, target_sql, " = ", value->GetSQL());
    }

    std::string target_alias;
    if (node->element_column() != nullptr) {
      const ResolvedColumn& column = node->element_column()->column();
      if (IsInternalAlias(column.name())) {
        // We use an internal alias for the target of a nested DML statement
        // when it does not end in an identifier, and a ResolvedUpdateItem can
        // refer to that. E.g., the 'target' field of the ResolvedUpdateItem for
        // "b = 4" in "UPDATE T SET (UPDATE T.(a) SET b = 4 WHERE ..." contains
        // a ResolvedColumnRef for T.(a) using an internal alias.
        target_alias = kEmptyAlias;
      } else {
        target_alias = ToIdentifierLiteral(column.name());
      }
      SetPathForColumn(column, target_alias);
    }
    nested_dml_targets_.push_back(std::make_pair(target_sql, target_alias));

    std::vector<std::string> nested_statements_sql;
    if (node->delete_list_size() > 0) {
      for (const auto& delete_stmt : node->delete_list()) {
        ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                         ProcessNode(delete_stmt.get()));
        nested_statements_sql.push_back(
            absl::StrCat("(", result->GetSQL(), ")"));
      }
    }
    if (node->update_list_size() > 0) {
      for (const auto& update_stmt : node->update_list()) {
        ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                         ProcessNode(update_stmt.get()));
        nested_statements_sql.push_back(
            absl::StrCat("(", result->GetSQL(), ")"));
      }
    }
    if (node->insert_list_size() > 0) {
      for (const auto& insert_stmt : node->insert_list()) {
        ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                         ProcessNode(insert_stmt.get()));
        nested_statements_sql.push_back(
            absl::StrCat("(", result->GetSQL(), ")"));
      }
    }
    absl::StrAppend(&sql, absl::StrJoin(nested_statements_sql, ", "));

    nested_dml_targets_.pop_back();
    PushQueryFragment(node, sql);
  }

  update_item_targets_and_offsets_.back().pop_back();
  return zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedUpdateArrayItem(
    const ResolvedUpdateArrayItem* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> offset,
                   ProcessNode(node->offset()));
  ZETASQL_RET_CHECK(!update_item_targets_and_offsets_.empty());
  ZETASQL_RET_CHECK(!update_item_targets_and_offsets_.back().empty());
  ZETASQL_RET_CHECK_EQ("", update_item_targets_and_offsets_.back().back().second);
  const std::string offset_sql = offset->GetSQL();
  ZETASQL_RET_CHECK(!offset_sql.empty());
  update_item_targets_and_offsets_.back().back().second = offset_sql;

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> update,
                   ProcessNode(node->update_item()));

  // Clear the offset_sql.
  update_item_targets_and_offsets_.back().back().second.clear();

  PushQueryFragment(node, update->GetSQL());
  return zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedUpdateStmt(
    const ResolvedUpdateStmt* node) {
  std::string sql;
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
    absl::StrAppend(&sql, " ");
  }

  std::string target_sql;
  if (node->table_scan() != nullptr) {
    // Always use a table alias. If we have the FROM clause, the alias might
    // appear in the FROM scan.
    const std::string alias = GetScanAlias(node->table_scan());
    SetPathForColumnsInScan(node->table_scan(), alias);
    absl::StrAppend(&target_sql,
                    ToIdentifierLiteral(node->table_scan()->table()->Name()),
                    " AS ", alias);
  } else {
    ZETASQL_RET_CHECK(!nested_dml_targets_.empty());
    target_sql = nested_dml_targets_.back().first;
    if (nested_dml_targets_.back().second != kEmptyAlias) {
      absl::StrAppend(&target_sql, " ", nested_dml_targets_.back().second);
    }
  }
  ZETASQL_RET_CHECK(!target_sql.empty());
  absl::StrAppend(&sql, "UPDATE ", target_sql);

  if (node->array_offset_column() != nullptr) {
    const ResolvedColumn& offset_column = node->array_offset_column()->column();
    const std::string offset_alias = ToIdentifierLiteral(offset_column.name());
    SetPathForColumn(offset_column, offset_alias);
    absl::StrAppend(&sql, " WITH OFFSET AS ", offset_alias);
  }

  std::string from_sql;
  if (node->from_scan() != nullptr) {
    SetPathForColumnsInScan(node->from_scan(), "");
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> from,
                     ProcessNode(node->from_scan()));
    std::unique_ptr<QueryExpression> query_expression(
        from->query_expression.release());
    absl::StrAppend(&from_sql, " FROM ", query_expression->FromClause());
  }

  ZETASQL_ASSIGN_OR_RETURN(std::string update_item_list_sql,
                   GetUpdateItemListSQL(node->update_item_list()));
  absl::StrAppend(&sql, " SET ", update_item_list_sql);

  if (!from_sql.empty()) {
    absl::StrAppend(&sql, from_sql);
  }

  if (node->where_expr() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> where,
                     ProcessNode(node->where_expr()));
    absl::StrAppend(&sql, " WHERE ", where->GetSQL());
  }
  if (node->assert_rows_modified() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> assert_rows_modified,
                     ProcessNode(node->assert_rows_modified()));
    absl::StrAppend(&sql, " ", assert_rows_modified->GetSQL());
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedInsertRow(const ResolvedInsertRow* node) {
  std::vector<std::string> values_sql;
  for (const auto& value : node->value_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(value.get()));
    values_sql.push_back(result->GetSQL());
  }
  PushQueryFragment(node,
                    absl::StrCat("(", absl::StrJoin(values_sql, ", "), ")"));
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedInsertStmt(
    const ResolvedInsertStmt* node) {
  std::string sql;
  if (node->hint_list_size() > 0) {
    ZETASQL_RETURN_IF_ERROR(AppendHintsIfPresent(node->hint_list(), &sql));
    absl::StrAppend(&sql, " ");
  }

  absl::StrAppend(&sql, "INSERT ");
  switch (node->insert_mode()) {
    case ResolvedInsertStmt::OR_IGNORE:
      absl::StrAppend(&sql, "OR IGNORE ");
      break;
    case ResolvedInsertStmt::OR_REPLACE:
      absl::StrAppend(&sql, "OR REPLACE ");
      break;
    case ResolvedInsertStmt::OR_UPDATE:
      absl::StrAppend(&sql, "OR UPDATE ");
      break;
    case ResolvedInsertStmt::OR_ERROR:
      break;
  }

  std::string target_sql;
  if (node->table_scan() != nullptr) {
    target_sql = ToIdentifierLiteral(node->table_scan()->table()->Name());
  } else {
    ZETASQL_RET_CHECK(!nested_dml_targets_.empty());
    target_sql = nested_dml_targets_.back().first;
  }
  ZETASQL_RET_CHECK(!target_sql.empty());
  absl::StrAppend(&sql, target_sql, " ");

  if (node->insert_column_list_size() > 0) {
    absl::StrAppend(&sql, "(",
                    GetInsertColumnListSQL(node->insert_column_list()), ") ");
  }

  if (node->row_list_size() > 0) {
    std::vector<std::string> rows_sql;
    for (const auto& row : node->row_list()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(row.get()));
      rows_sql.push_back(result->GetSQL());
    }
    absl::StrAppend(&sql, "VALUES ", absl::StrJoin(rows_sql, ", "));
  } else {
    ZETASQL_RET_CHECK(node->query() != nullptr);
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(node->query()));

    // If the query's column list doesn't exactly match the target columns
    // of the insert, then add a wrapper query to make the select-list match.
    // TODO Is it necessary to do this unconditionally?  With
    // randomly permuted column_lists, there are some cases where the SQLBuilder
    // generates a final output query with columns in the wrong order, which
    // is fixed if I force this block to run.  I don't understand it though.
    if (node->query_output_column_list() != node->query()->column_list()) {
      ZETASQL_RETURN_IF_ERROR(WrapQueryExpression(
          node->query(), result->query_expression.get()));
      ZETASQL_RETURN_IF_ERROR(AddSelectListIfNeeded(
          node->query_output_column_list(), result->query_expression.get()));
    }

    // Dummy access to the query parameter list for
    // ResolvedInsertStmt::CheckFieldsAccessed().
    for (const std::unique_ptr<const ResolvedColumnRef>& parameter :
         node->query_parameter_list()) {
      parameter->column();
    }

    absl::StrAppend(&sql, result->GetSQL());
  }

  if (node->assert_rows_modified() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> assert_rows_modified,
                     ProcessNode(node->assert_rows_modified()));
    absl::StrAppend(&sql, " ", assert_rows_modified->GetSQL());
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedMergeStmt(const ResolvedMergeStmt* node) {
  std::string sql = "MERGE INTO ";
  ZETASQL_RET_CHECK(node->table_scan() != nullptr) << "Missing target table.";
  // Creates alias for the target table, because its column names may appear in
  // the source scan.
  std::string alias = GetScanAlias(node->table_scan());
  SetPathForColumnsInScan(node->table_scan(), alias);
  absl::StrAppend(&sql,
                  ToIdentifierLiteral(node->table_scan()->table()->Name()),
                  " AS ", alias);

  ZETASQL_RET_CHECK(node->from_scan() != nullptr) << "Missing data source.";
  absl::StrAppend(&sql, " USING ");
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> source,
                   ProcessNode(node->from_scan()));
  std::unique_ptr<QueryExpression> query_expression(
      source->query_expression.release());
  ZETASQL_RETURN_IF_ERROR(
      WrapQueryExpression(node->from_scan(), query_expression.get()));
  absl::StrAppend(&sql, query_expression->FromClause());

  ZETASQL_RET_CHECK(node->merge_expr() != nullptr) << "Missing merge condition.";
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> merge_condition,
                   ProcessNode(node->merge_expr()));
  absl::StrAppend(&sql, " ON ", merge_condition->GetSQL());

  for (const auto& when_clause : node->when_clause_list()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> when_clause_sql,
                     ProcessNode(when_clause.get()));
    absl::StrAppend(&sql, " ", when_clause_sql->GetSQL());
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedMergeWhen(const ResolvedMergeWhen* node) {
  std::string sql = "WHEN ";
  switch (node->match_type()) {
    case ResolvedMergeWhen::MATCHED:
      absl::StrAppend(&sql, "MATCHED");
      break;
    case ResolvedMergeWhen::NOT_MATCHED_BY_SOURCE:
      absl::StrAppend(&sql, "NOT MATCHED BY SOURCE");
      break;
    case ResolvedMergeWhen::NOT_MATCHED_BY_TARGET:
      absl::StrAppend(&sql, "NOT MATCHED BY TARGET");
      break;
  }

  if (node->match_expr() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> match_condition,
                     ProcessNode(node->match_expr()));
    absl::StrAppend(&sql, " AND ", match_condition->GetSQL());
  }

  absl::StrAppend(&sql, " THEN ");

  switch (node->action_type()) {
    case ResolvedMergeWhen::INSERT: {
      ZETASQL_RET_CHECK(!node->insert_column_list().empty());
      ZETASQL_RET_CHECK_NE(nullptr, node->insert_row());
      ZETASQL_RET_CHECK_EQ(node->insert_column_list_size(),
                   node->insert_row()->value_list_size());
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> row_value,
                       ProcessNode(node->insert_row()));
      absl::StrAppend(&sql, "INSERT (",
                      GetInsertColumnListSQL(node->insert_column_list()), ") ",
                      "VALUES ", row_value->GetSQL());
      break;
    }
    case ResolvedMergeWhen::UPDATE: {
      ZETASQL_RET_CHECK(!node->update_item_list().empty());
      ZETASQL_ASSIGN_OR_RETURN(std::string update_item_list_sql,
                       GetUpdateItemListSQL(node->update_item_list()));
      absl::StrAppend(&sql, "UPDATE SET ", update_item_list_sql);
      break;
    }
    case ResolvedMergeWhen::DELETE:
      absl::StrAppend(&sql, "DELETE");
      break;
  }
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAlterDatabaseStmt(
    const ResolvedAlterDatabaseStmt* node) {
  return GetResolvedAlterObjectStmtSQL(node, "DATABASE");
}

zetasql_base::Status SQLBuilder::VisitResolvedAlterTableSetOptionsStmt(
    const ResolvedAlterTableSetOptionsStmt* node) {
  std::string sql = "ALTER TABLE ";
  absl::StrAppend(&sql,
                  node->is_if_exists() ? "IF EXISTS " : "",
                  IdentifierPathToString(node->name_path()), " ");
  ZETASQL_ASSIGN_OR_RETURN(const std::string options_string,
                   GetHintListString(node->option_list()));
  absl::StrAppend(&sql, "SET OPTIONS(", options_string, ") ");
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::GetResolvedAlterObjectStmtSQL(
    const ResolvedAlterObjectStmt* node, absl::string_view object_kind) {
  std::string sql = "ALTER ";
  ZETASQL_ASSIGN_OR_RETURN(const std::string actions_string,
                   GetAlterActionSQL(node->alter_action_list()));
  absl::StrAppend(
      &sql, object_kind, " ", node->is_if_exists() ? "IF EXISTS " : "",
      IdentifierPathToString(node->name_path()), " ", actions_string);
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedAlterTableStmt(
    const ResolvedAlterTableStmt* node) {
  return GetResolvedAlterObjectStmtSQL(node, "TABLE");
}

zetasql_base::Status SQLBuilder::VisitResolvedAlterViewStmt(
    const ResolvedAlterViewStmt* node) {
  return GetResolvedAlterObjectStmtSQL(node, "VIEW");
}

zetasql_base::Status SQLBuilder::VisitResolvedAlterMaterializedViewStmt(
    const ResolvedAlterMaterializedViewStmt* node) {
  return GetResolvedAlterObjectStmtSQL(node, "MATERIALIZED VIEW");
}

zetasql_base::StatusOr<std::string> SQLBuilder::GetAlterActionSQL(
    const std::vector<std::unique_ptr<const ResolvedAlterAction>>&
        alter_action_list) {
  std::vector<std::string> alter_action_sql;
  for (const auto& alter_action : alter_action_list) {
    switch (alter_action->node_kind()) {
      case RESOLVED_SET_OPTIONS_ACTION: {
        ZETASQL_ASSIGN_OR_RETURN(
            const std::string options_string,
            GetHintListString(alter_action->GetAs<ResolvedSetOptionsAction>()
                                  ->option_list()));
        alter_action_sql.push_back(
            absl::StrCat("SET OPTIONS(", options_string, ") "));
      } break;
      case RESOLVED_ADD_COLUMN_ACTION: {
        const auto* add_action = alter_action->GetAs<ResolvedAddColumnAction>();
        const auto* column_definition = add_action->column_definition();
        std::string add_column =
            absl::StrCat("ADD COLUMN ",
                         add_action->is_if_not_exists() ? "IF NOT EXISTS " : "",
                         column_definition->name(), " ");
        ZETASQL_RETURN_IF_ERROR(AppendColumnSchema(
            column_definition->type(), column_definition->is_hidden(),
            column_definition->annotations(),
            column_definition->generated_column_info(), &add_column));
        alter_action_sql.push_back(std::move(add_column));
      } break;
      case RESOLVED_DROP_COLUMN_ACTION: {
        auto* drop_action = alter_action->GetAs<ResolvedDropColumnAction>();
        alter_action_sql.push_back(absl::StrCat(
            "DROP COLUMN ", drop_action->is_if_exists() ? "IF EXISTS " : "",
            drop_action->name()));
      } break;
      case RESOLVED_GRANT_TO_ACTION: {
        auto* grant_to_action = alter_action->GetAs<ResolvedGrantToAction>();
        ZETASQL_ASSIGN_OR_RETURN(
            std::string grantee_sql,
            GetGranteeListSQL("", {}, grant_to_action->grantee_expr_list()));
        alter_action_sql.push_back(
            absl::StrCat("GRANT TO (", grantee_sql, ")"));
      } break;
      case RESOLVED_FILTER_USING_ACTION: {
        auto* filter_using_action =
            alter_action->GetAs<ResolvedFilterUsingAction>();
        alter_action_sql.push_back(absl::StrCat(
            "FILTER USING (", filter_using_action->predicate_str(), ")"));
      } break;
      case RESOLVED_REVOKE_FROM_ACTION: {
        auto* revoke_from_action =
            alter_action->GetAs<ResolvedRevokeFromAction>();
        std::string revokees;
        if (revoke_from_action->is_revoke_from_all()) {
          revokees = "ALL";
        } else {
          ZETASQL_ASSIGN_OR_RETURN(
              std::string revokee_list,
              GetGranteeListSQL("", {},
                                revoke_from_action->revokee_expr_list()));
          revokees = absl::StrCat("(", revokee_list, ")");
        }
        alter_action_sql.push_back(absl::StrCat("REVOKE FROM ", revokees));
      } break;
      case RESOLVED_RENAME_TO_ACTION: {
        auto* rename_to_action = alter_action->GetAs<ResolvedRenameToAction>();
        alter_action_sql.push_back(absl::StrCat(
            "RENAME TO ", ToIdentifierLiteral(rename_to_action->new_name())));
      } break;
      default:
        ZETASQL_RET_CHECK_FAIL() << "Unexpected AlterAction: "
                         << alter_action->DebugString();
    }
  }
  return absl::StrJoin(alter_action_sql, ", ");
}

namespace {

typedef std::string (*Escaper)(absl::string_view);

// Formatter which escapes a std::string with given escaper function, such as
// ToStringLiteral or ToIdentifierLiteral.
class EscapeFormatter {
 public:
  explicit EscapeFormatter(Escaper escaper) : escaper_(escaper) {}
  void operator()(std::string* out, const std::string& in) const {
    absl::StrAppend(out, escaper_(in));
  }
 private:
  Escaper escaper_;
};

}  // namespace

zetasql_base::StatusOr<std::string> SQLBuilder::GetGranteeListSQL(
    const std::string& prefix, const std::vector<std::string>& grantee_list,
    const std::vector<std::unique_ptr<const ResolvedExpr>>& grantee_expr_list) {
  std::string sql;
  // We CHECK the expected invariant that only one of grantee_list or
  // grantee_expr_list is empty.
  if (!grantee_list.empty()) {
    ZETASQL_RET_CHECK(grantee_expr_list.empty());
    absl::StrAppend(&sql, prefix,
                    absl::StrJoin(grantee_list, ", ",
                                  EscapeFormatter(ToStringLiteral)));
  }
  if (!grantee_expr_list.empty()) {
    ZETASQL_RET_CHECK(grantee_list.empty());
    std::vector<std::string> grantee_string_list;
    for (const std::unique_ptr<const ResolvedExpr>& grantee :
             grantee_expr_list) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                       ProcessNode(grantee.get()));
      grantee_string_list.push_back(result->GetSQL());
    }
    absl::StrAppend(&sql, prefix, absl::StrJoin(grantee_string_list, ", "));
  }
  return sql;
}

zetasql_base::Status SQLBuilder::VisitResolvedAlterRowAccessPolicyStmt(
    const ResolvedAlterRowAccessPolicyStmt* node) {
  std::string sql = "ALTER ROW ACCESS POLICY ";
  absl::StrAppend(&sql, node->is_if_exists() ? "IF EXISTS " : "");
  absl::StrAppend(&sql, ToIdentifierLiteral(node->name()));
  absl::StrAppend(&sql, " ON ", IdentifierPathToString(node->name_path()));
  ZETASQL_ASSIGN_OR_RETURN(const std::string actions_string,
                   GetAlterActionSQL(node->alter_action_list()));
  absl::StrAppend(&sql, " ", actions_string);
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedPrivilege(const ResolvedPrivilege* node) {
  std::string sql;

  absl::StrAppend(&sql, ToIdentifierLiteral(node->action_type()));

  if (!node->unit_list().empty()) {
    const auto& formatter = [](std::string* out, const std::string& in) {
      absl::StrAppend(out, ToIdentifierLiteral(in));
    };
    absl::StrAppend(&sql, "(",
                    absl::StrJoin(node->unit_list(), ", ", formatter), ")");
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::StatusOr<absl::string_view> SQLBuilder::GetNullHandlingModifier(
    ResolvedNonScalarFunctionCallBase::NullHandlingModifier kind) {
  switch (kind) {
    case ResolvedNonScalarFunctionCallBase::DEFAULT_NULL_HANDLING:
      return absl::string_view("");
    case ResolvedNonScalarFunctionCallBase::IGNORE_NULLS:
      return absl::string_view(" IGNORE NULLS");
    case ResolvedNonScalarFunctionCallBase::RESPECT_NULLS:
      return absl::string_view(" RESPECT NULLS");
      // No "default:". Let the compilation fail in case an entry is added to
      // the enum without being handled here.
  }
  ZETASQL_RET_CHECK_FAIL() << "Encountered invalid NullHandlingModifier " << kind;
}

zetasql_base::Status SQLBuilder::VisitResolvedAggregateHavingModifier(
    const ResolvedAggregateHavingModifier* node) {
  std::string sql(" HAVING ");
  if (node->kind() == ResolvedAggregateHavingModifier::MAX) {
    absl::StrAppend(&sql, "MAX ");
  } else {
    absl::StrAppend(&sql, "MIN ");
  }

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                   ProcessNode(node->having_expr()));
  absl::StrAppend(&sql, result->GetSQL());
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedGrantStmt(
    const ResolvedGrantStmt* node) {
  std::string sql, privileges_string;

  ZETASQL_RETURN_IF_ERROR(GetPrivilegesString(node, &privileges_string));

  absl::StrAppend(&sql, "GRANT ", privileges_string);

  ZETASQL_ASSIGN_OR_RETURN(std::string grantee_sql,
                   GetGranteeListSQL(" TO ", node->grantee_list(),
                                     node->grantee_expr_list()));
  ZETASQL_RET_CHECK(!grantee_sql.empty());
  absl::StrAppend(&sql, grantee_sql);

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedRevokeStmt(
    const ResolvedRevokeStmt* node) {
  std::string sql, privileges_string;

  ZETASQL_RETURN_IF_ERROR(GetPrivilegesString(node, &privileges_string));

  absl::StrAppend(&sql, "REVOKE ", privileges_string);

  ZETASQL_ASSIGN_OR_RETURN(std::string grantee_sql,
                   GetGranteeListSQL(" FROM ", node->grantee_list(),
                                     node->grantee_expr_list()));
  ZETASQL_RET_CHECK(!grantee_sql.empty());
  absl::StrAppend(&sql, grantee_sql);

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedRenameStmt(
    const ResolvedRenameStmt* node) {
  std::string sql;
  absl::StrAppend(&sql, "RENAME ", ToIdentifierLiteral(node->object_type()),
                  " ", IdentifierPathToString(node->old_name_path()), " TO ",
                  IdentifierPathToString(node->new_name_path()));
  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

std::string SQLBuilder::sql() {
  if (sql_.empty()) {
    if (query_fragments_.empty()) {
      // No sql to build here since no ResolvedAST was visited before calling
      // sql().
      return "";
    }

    std::unique_ptr<QueryFragment> query_fragment = PopQueryFragment();
    DCHECK(query_fragments_.empty());
    sql_ = query_fragment->GetSQL();
    ZETASQL_DCHECK_OK(query_fragment->node->CheckFieldsAccessed())
        << "sql is\n" << sql_;
  }
  return sql_;
}

zetasql_base::Status SQLBuilder::VisitResolvedCreateRowAccessPolicyStmt(
    const ResolvedCreateRowAccessPolicyStmt* node) {
  std::string sql = "CREATE ";
  if (node->create_mode() == ResolvedCreateStatement::CREATE_OR_REPLACE) {
    absl::StrAppend(&sql, "OR REPLACE ");
  }
  absl::StrAppend(&sql, "ROW ACCESS POLICY ");
  if (node->create_mode() == ResolvedCreateStatement::CREATE_IF_NOT_EXISTS) {
    absl::StrAppend(&sql, "IF NOT EXISTS ");
  }

  if (!node->name().empty()) {
    absl::StrAppend(&sql, ToIdentifierLiteral(node->name()), " ");
  }

  absl::StrAppend(&sql, "ON ",
                  IdentifierPathToString(node->target_name_path()));

  ZETASQL_ASSIGN_OR_RETURN(
      std::string grantee_sql,
      GetGranteeListSQL("", node->grantee_list(), node->grantee_expr_list()));
  if (!grantee_sql.empty()) {
    absl::StrAppend(&sql, " GRANT TO (");
    absl::StrAppend(&sql, grantee_sql);
    absl::StrAppend(&sql, ")");
  }

  // ProcessNode(node->predicate()) should produce an equivalent QueryFragment,
  // but we use the std::string form directly because it's simpler.
  absl::StrAppend(&sql, " FILTER USING (", node->predicate_str(), ")");

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedImportStmt(
    const ResolvedImportStmt* node) {
  std::string sql = "IMPORT ";
  switch (node->import_kind()) {
    case ResolvedImportStmt::MODULE:
      ZETASQL_RET_CHECK(node->file_path().empty());
      absl::StrAppend(&sql, "MODULE ");
      absl::StrAppend(&sql, IdentifierPathToString(node->name_path()), " ");
      break;
    case ResolvedImportStmt::PROTO:
      ZETASQL_RET_CHECK(node->name_path().empty());
      absl::StrAppend(&sql, "PROTO ");
      absl::StrAppend(&sql, ToStringLiteral(node->file_path()));
      break;
    default:
      ZETASQL_RET_CHECK_FAIL() << "Unexpected ImportKind " << node->import_kind()
                       << " for node: " << node->DebugString();
      break;
  }

  std::string alias_prefix;
  std::string alias;
  if (!node->alias_path().empty()) {
    alias_prefix = "AS ";
    alias = IdentifierPathToString(node->alias_path());
  } else if (!node->into_alias_path().empty()) {
    alias_prefix = "INTO ";
    alias = IdentifierPathToString(node->into_alias_path());
  }
  if (!alias.empty()) {
    absl::StrAppend(&sql, alias_prefix, alias, " ");
  }

  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string result,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, "OPTIONS(", result, ") ");
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status SQLBuilder::VisitResolvedModuleStmt(
    const ResolvedModuleStmt* node) {
  std::string sql = "MODULE ";
  absl::StrAppend(&sql, IdentifierPathToString(node->name_path()), " ");
  if (node->option_list_size() > 0) {
    ZETASQL_ASSIGN_OR_RETURN(const std::string result,
                     GetHintListString(node->option_list()));
    absl::StrAppend(&sql, "OPTIONS(", result, ") ");
  }

  PushQueryFragment(node, sql);
  return ::zetasql_base::OkStatus();
}

void SQLBuilder::PushSQLForQueryExpression(const ResolvedNode* node,
                                           QueryExpression* query_expression) {
  PushQueryFragment(node, query_expression);
  DumpQueryFragmentStack();
}

zetasql_base::Status SQLBuilder::DefaultVisit(const ResolvedNode* node) {
  ZETASQL_RET_CHECK_FAIL() << "SQLBuilder visitor not implemented for "
                   << node->node_kind_string();
}

zetasql_base::StatusOr<std::string> SQLBuilder::GetUpdateItemListSQL(
    const std::vector<std::unique_ptr<const ResolvedUpdateItem>>&
        update_item_list) {
  std::vector<std::string> update_item_list_sql;
  update_item_list_sql.reserve(update_item_list.size());
  for (const auto& update_item : update_item_list) {
    update_item_targets_and_offsets_.emplace_back();
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<QueryFragment> result,
                     ProcessNode(update_item.get()));
    update_item_targets_and_offsets_.pop_back();

    update_item_list_sql.push_back(result->GetSQL());
  }
  return absl::StrJoin(update_item_list_sql, ", ");
}

std::string SQLBuilder::GetInsertColumnListSQL(
    const std::vector<ResolvedColumn>& insert_column_list) const {
  std::vector<std::string> columns_sql;
  columns_sql.reserve(insert_column_list.size());
  for (const auto& col : insert_column_list) {
    columns_sql.push_back(ToIdentifierLiteral(col.name()));
  }
  return absl::StrJoin(columns_sql, ", ");
}

}  // namespace zetasql
