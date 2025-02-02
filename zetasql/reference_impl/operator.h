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

#ifndef ZETASQL_REFERENCE_IMPL_OPERATOR_H_
#define ZETASQL_REFERENCE_IMPL_OPERATOR_H_

// Defines algebraic operators used in the ZetaSQL reference implementation.
// AlgebraNode is the common abstract base class. It has two subclasses:
// ValueExpr (representing the value of a ZetaSQL expression), and
// RelationalOp (which produces a relation of tuples, which are internal data
// structures used in execution and are not part of the type system. See
// (broken link) for more information.
//
// Each of these operators may have arguments. AlgebraArg is the abstract base
// class for representing an argument, such as a typed argument (ExprArg) or a
// relational argument (RelationalArg). AlgebraArg can be subclassed to hold
// operator-specific information. For example, KeyArg argument specifies the
// sort order in SortOp and AggregateArg specifies whether distinct values only
// are to be aggregated.
//
// The implementation of operator.h is in the following files:
// - aggregate_op.cc (for aggregate function evaluation)
// - analytic_op.cc (for analytic function evaluation)
// - operator.cc (for base classes like AlgebraArg, AlgebraNode, etc.)
// - relational_op.cc (other relational operation code)
// - value_expr.cc (code for ValueExprs)

#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zetasql/base/logging.h"
#include "google/protobuf/descriptor.h"
#include "zetasql/public/catalog.h"
#include "zetasql/public/evaluator_table_iterator.h"
#include "zetasql/public/type.h"
#include "zetasql/public/value.h"
#include "zetasql/reference_impl/evaluation.h"
#include "zetasql/reference_impl/tuple.h"
#include "zetasql/reference_impl/tuple_comparator.h"
#include "zetasql/reference_impl/variable_generator.h"
#include "zetasql/reference_impl/variable_id.h"
#include "zetasql/resolved_ast/resolved_ast.h"
#include "zetasql/resolved_ast/resolved_column.h"
#include "zetasql/resolved_ast/resolved_node.h"
#include <cstdint>
#include "absl/hash/hash.h"
#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "zetasql/base/stl_util.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status.h"
#include "zetasql/base/statusor.h"

namespace zetasql {

// Declared below.
class AggregateFunctionBody;
class AggregateFunctionCallExpr;
class AlgebraNode;
class AnalyticFunctionBody;
class AnalyticFunctionCallExpr;
class ExprArg;
class KeyArg;
class RelationalArg;
class RelationalOp;
class ValueExpr;

// -------------------------------------------------------
// Base classes
// -------------------------------------------------------

// Abstract base class for operator arguments. The implementation is designed to
// make it easy to deal with ExprArgs and RelationalArgs since those are the
// most common kinds of arguments.
class AlgebraArg {
 public:
  AlgebraArg(const AlgebraArg&) = delete;
  AlgebraArg& operator=(const AlgebraArg&) = delete;
  virtual ~AlgebraArg();

  // Downcasts the argument to ExprArg or nullptr.
  virtual const ExprArg* AsExprArg() const { return nullptr; }

  // Downcasts the argument to RelationalArg or nullptr.
  virtual const RelationalArg* AsRelationalArg() const { return nullptr; }

  // Argument kind is usually set from an enum defined in the subclass of
  // AlgebraNode. Its interpretation is operator-specific.
  int kind() const { return kind_; }
  bool is_kind(int kind) const { return kind_ == kind; }
  void set_kind(int kind) { kind_ = kind; }

  bool has_node() const { return node_ != nullptr; }
  const AlgebraNode* node() const { return node_.get(); }
  AlgebraNode* mutable_node() { return node_.get(); }

  bool has_variable() const { return variable_.is_valid(); }
  const VariableId& variable() const { return variable_; }

  // Convenience method, returns node()->AsValueExpr() or nullptr.
  const ValueExpr* value_expr() const;
  ValueExpr* mutable_value_expr();
  // Convenience method, returns node()->AsRelationalOp() or nullptr.
  const RelationalOp* relational_op() const;
  RelationalOp* mutable_relational_op();

  // Returns a std::string representation of the operator for debugging. If
  // 'verbose' is true, prints more information.
  std::string DebugString(bool verbose = false) const;

  // Returns a std::string representation of the argument for debugging.
  // 'level' specifies the indentation level of the output std::string. If 'verbose'
  // is true, prints more information.
  virtual std::string DebugInternal(const std::string& indent,
                                    bool verbose) const;

 protected:
  // 'variable' may be invalid (e.g., for a ValueExpr with no corresponding
  // variable).
  //
  // 'node' may be NULL if this argument does not wrap a single node. (The
  // 'node' feature is mostly to make it easy to work with ExprArgs and
  // RelationalArgs, which are the most common instances of this class.)
  //
  // If 'variable' is valid than 'node' must be a ValueExpr.
  AlgebraArg(const VariableId& variable, std::unique_ptr<AlgebraNode> node);

 private:
  const VariableId variable_;          // unused if empty
  std::unique_ptr<AlgebraNode> node_;  // may be NULL
  int kind_ = -1;
};

// Concrete base class for arguments that have some Type, likely represented by
// a ValueExpr.
class ExprArg : public AlgebraArg {
 public:
  // Creates an argument that populates 'variable' from value 'expr'.
  ExprArg(const VariableId& variable, std::unique_ptr<ValueExpr> expr);

  // Creates a typed argument that has a variable name.
  // Does not take ownership of 'type'.
  ExprArg(const VariableId& variable, const Type* type);

  ExprArg(const ExprArg&) = delete;
  ExprArg& operator=(const ExprArg&) = delete;

  // Creates an argument from 'expr', no variable.
  explicit ExprArg(std::unique_ptr<ValueExpr> expr);

  ~ExprArg() override {}

  const ExprArg* AsExprArg() const override { return this; }

  const Type* type() const { return type_; }

 private:
  const Type* type_;
};

struct AnalyticWindow {
  // Constructor for an empty window.
  AnalyticWindow()
      : start_tuple_id(0), num_tuples(0) {}

  // Constructor for a non-empty window.
  AnalyticWindow(int start_tuple_id_in, int num_tuples_in)
      : start_tuple_id(start_tuple_id_in),
        num_tuples(num_tuples_in) {
    DCHECK_GE(start_tuple_id, 0);
    DCHECK_GT(num_tuples, 0);
  }

  bool operator==(const AnalyticWindow& other) const {
    return start_tuple_id == other.start_tuple_id &&
        num_tuples == other.num_tuples;
  }

  // The 0-based index of the starting tuple of the window. The index is
  // relative to the beginning of the partition that the window belongs to.
  int start_tuple_id;
  // The total number of tuples in the window.
  int num_tuples;
};

// Window frame boundary argument that contains the boundary type and
// a boundary expression.
class WindowFrameBoundaryArg : public AlgebraArg {
 public:
  enum BoundaryType {
    kUnboundedPreceding,
    kOffsetPreceding,
    kCurrentRow,
    kOffsetFollowing,
    kUnboundedFollowing
  };

  WindowFrameBoundaryArg(const WindowFrameBoundaryArg&) = delete;
  WindowFrameBoundaryArg& operator=(const WindowFrameBoundaryArg&) = delete;
  ~WindowFrameBoundaryArg() override {}

  BoundaryType boundary_type() const {
    return boundary_type_;
  }

  const ValueExpr* boundary_offset_expr() const {
    return boundary_offset_expr_.get();
  }

  bool IsUnbounded() const {
    return boundary_type_ == kUnboundedPreceding ||
           boundary_type_ == kUnboundedFollowing;
  }

  bool IsCurrentRow() const {
    return boundary_type_ == kCurrentRow;
  }

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  // Creates a validated WindowFrameBoundaryArg that checks
  // whether expr is allowed for boundary_type.
  static zetasql_base::StatusOr<std::unique_ptr<WindowFrameBoundaryArg>> Create(
      BoundaryType boundary_type, std::unique_ptr<ValueExpr> expr);

  // Sets the schemas used in the methods that take parameters.
  zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas);

  // Returns the start or end <window_boundaries> based on physical offsets
  // for all tuples in a partition with size <partition_size>.
  // If <is_end_boundary> then the window frame end boundaries are computed for
  // each tuple.  Otherwise, window frame start boundaries are computed.
  // <params> contains the parameters for evaluating <boundary_offset_expr_>.
  // Each element in <window_boundaries> is the 0-based index of the boundary
  // position for a tuple in the partition. The size of <window_boundaries> is
  // equal to <partition_size>. The value can be -1 for an end window boundary,
  // or <partition_size> for a start window boundary. Both indicate the
  // associated window is empty.
  zetasql_base::Status GetRowsBasedWindowBoundaries(
      bool is_end_boundary, int partition_size,
      absl::Span<const TupleData* const> params, EvaluationContext* context,
      std::vector<int>* window_boundaries) const;

  // Returns the start or end <window_boundaries> based on order key offsets for
  // all tuples in a <partition>. If <is_end_boundary> then the window frame end
  // boundaries are computed for each tuple.  Otherwise, window frame start
  // boundaries are computed. <params> contains the parameters for evaluating
  // <boundary_offset_expr_>. Each element in <window_boundaries> is the
  // 0-based index of the boundary position for a tuple in the partition.
  // The size of <window_boundaries> is equal to the partition size. The value
  // can be equal to -1 for an end window boundary, or the partition size for a
  // start window boundary. Both indicate the associated window is empty.
  // <order_keys> cannot be empty unless the window frame is unbounded.
  // The size of <order_keys> must be 1 if there is an offset boundary.
  //
  // Does not take ownership of <order_keys> or <window_boundaries>.
  zetasql_base::Status GetRangeBasedWindowBoundaries(
      bool is_end_boundary, const TupleSchema& schema,
      absl::Span<const TupleData* const> partition,
      absl::Span<const KeyArg* const> order_keys,
      absl::Span<const TupleData* const> params, EvaluationContext* context,
      std::vector<int>* window_boundaries) const;

  // Evaluates the boundary expression. Returns an error when the offset value
  // is NULL, negative or NaN.
  zetasql_base::Status GetOffsetValue(absl::Span<const TupleData* const> params,
                              EvaluationContext* context,
                              Value* range_offset_value) const;

 private:
  // Specifies a window boundary for a group of tuples.
  struct GroupBoundary;

  WindowFrameBoundaryArg(BoundaryType boundary_type,
                         std::unique_ptr<ValueExpr> boundary_offset_expr);

  // Populates <window_boundaries> with <group_boundaries>. The tuples in
  // the same group have the same boundary.
  zetasql_base::Status SetGroupBoundaries(
      absl::Span<const GroupBoundary> group_boundaries,
      std::vector<int>* window_boundaries) const;

  // Computes the boundary positions for a range-based start offset PRECEDING
  // boundary on an ascending <partition>. If <is_end_boundary> then the window
  // frame end boundaries are computed for each tuple. Otherwise, window frame
  // start boundaries are computed.
  zetasql_base::Status GetOffsetPrecedingRangeBoundariesAsc(
      bool is_end_boundary, const TupleSchema& schema,
      absl::Span<const TupleData* const> partition, int order_key_slot_idx,
      const Value& offset_value, std::vector<int>* window_boundaries) const;

  // Computes the boundary positions for a range-based start offset PRECEDING
  // boundary on a descending <partition>. If <is_end_boundary> then the window
  // frame end boundaries are computed for each tuple. Otherwise, window frame
  // start boundaries are computed.
  zetasql_base::Status GetOffsetPrecedingRangeBoundariesDesc(
      bool is_end_boundary, const TupleSchema& schema,
      absl::Span<const TupleData* const> partition, int order_key_slot_idx,
      const Value& offset_value, std::vector<int>* window_boundaries) const;

  // Computes the boundary positions for a range-based start offset PRECEDING
  // boundary on an ascending <partition>. If <is_end_boundary> then the window
  // frame end boundaries are computed for each tuple. Otherwise, window frame
  // start boundaries are computed.
  zetasql_base::Status GetOffsetFollowingRangeBoundariesAsc(
      bool is_end_boundary, const TupleSchema& schema,
      absl::Span<const TupleData* const> partition, int order_key_slot_idx,
      const Value& offset_value, std::vector<int>* window_boundaries) const;

  // Computes the boundary positions for a range-based start offset PRECEDING
  // boundary on a descending <partition>. If <is_end_boundary> then the window
  // frame end boundaries are computed for each tuple. Otherwise, window frame
  // start boundaries are computed.
  zetasql_base::Status GetOffsetFollowingRangeBoundariesDesc(
      bool is_end_boundary, const TupleSchema& schema,
      absl::Span<const TupleData* const> partition, int order_key_slot_idx,
      const Value& offset_value, std::vector<int>* window_boundaries) const;

  // Divides the ascending partition into groups according to the order key
  // values and returns the boundary for each group.
  //
  // <end_null> is the position of the last tuple with a NULL key. -1 if there
  // is no tuple with a NULL key.
  // <end_nan> is the position of the last tuple with a NaN key. If not exists,
  // <end_nan> is equal to <end_null>.
  // <end_neg_inf> is the position of the last tuple with a negative infinity
  // key. If not exists, <end_neg_inf> is equal to <end_nan>.
  // <start_pos_inf> is the position of the first tuple with a positive infinity
  // key. If not exists, <start_pos_inf> is equal to the size of the partition.
  void DivideAscendingPartition(const TupleSchema& schema,
                                absl::Span<const TupleData* const> partition,
                                int order_key_slot_idx, int* end_null,
                                int* end_nan, int* end_neg_inf,
                                int* start_pos_inf) const;

  // Divides the descending partition into groups according to the order key
  // values and returns the boundary for each group.
  //
  // <end_pos_inf> is the position of the last tuple with a positive infinity
  // key. 0 if not exists.
  // <start_neg_inf> is the position of the first tuple with a negative infinity
  // key. If not exists, it is equal to <start_nan_key>.
  // <start_nan_key> is the position of the first tuple with a NaN key. If not
  // eixsts, it is equal to <start_null_key>.
  // <start_null_key> is the position of the first tuple with a NULL key. If not
  // exists, it is equal to the size of the partition.
  void DivideDescendingPartition(const TupleSchema& schema,
                                 absl::Span<const TupleData* const> partition,
                                 int order_key_slot_idx, int* end_pos_inf,
                                 int* start_neg_inf, int* start_nan_key,
                                 int* start_null_key) const;

  static std::string GetBoundaryTypeString(BoundaryType boundary_type);

  BoundaryType boundary_type_;
  // Must be nullptr if <boundary_type_> is kUnboundedPreceding, kCurrentRow
  // or kUnboundedFollowing. Cannot be nullptr otherwise.
  std::unique_ptr<ValueExpr> boundary_offset_expr_;
  // Set by SetSchemasForEvaluation().
  std::vector<std::unique_ptr<const TupleSchema>> params_schemas_;
};

// Window frame argument in an analytic function call expression.
class WindowFrameArg : public AlgebraArg {
 public:
  enum WindowFrameType { kRows, kRange };

  static ::zetasql_base::StatusOr<std::unique_ptr<WindowFrameArg>> Create(
      WindowFrameType window_frame_type,
      std::unique_ptr<WindowFrameBoundaryArg> start_boundary_arg,
      std::unique_ptr<WindowFrameBoundaryArg> end_boundary_arg) {
    ZETASQL_RET_CHECK(start_boundary_arg != nullptr);
    ZETASQL_RET_CHECK(end_boundary_arg != nullptr);
    return absl::WrapUnique(new WindowFrameArg(window_frame_type,
                                               std::move(start_boundary_arg),
                                               std::move(end_boundary_arg)));
  }

  WindowFrameArg(const WindowFrameArg&) = delete;
  WindowFrameArg& operator=(const WindowFrameArg&) = delete;
  ~WindowFrameArg() override {}

  // Sets evaluation context on the boundary offset expressions.
  void SetContext(EvaluationContext* context) const;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  // Sets the schemas of the parameters used in GetWindows.
  zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas);

  // Computes the window for each tuple in <partition> (which has TupleSchema
  // <schema>). <windows> stores the computed windows, which correspond 1:1 with
  // <partition>.  <order_keys> specifies the ordering keys and directions of
  // <partition>.  <params> gives the parameter values for evaluating constant
  // boundary offset expressions.
  //
  // <is_deterministic> is set to true if no window changes when the associated
  // partition is in a different total ordering (but still conforming to the
  // window ordering). In other words, if a window is deterministic,
  // the positions of the two window boundaries do not change regardless of the
  // ordering of tuples that are tied in the window ordering.
  //
  // For example, consider a partition P(a, b) containing the following three
  // tuples: (1, 2), (1, 3), (2, 4).
  //
  // For SUM(b) OVER (ORDER BY a ROWS BETWEEN 1 PRECEDING AND CURRENT ROW),
  // the window for the first tuple (1, 2) contains itself only. However,
  // if we switch the first and the second tuple, the window for (1, 2)
  // contains two tuples, so the window is non-deterministic.
  //
  // For SUM(b) OVER (ORDER BY a RANGE BETWEEN 1 PRECEDING AND CURRENT ROW),
  // the first and the second tuples belong to the same ordering group because
  // they have the same 'a' value. The windows for the two are the same and do
  // not change when we switch their positions, so the window is deterministic.
  //
  // <is_deterministic> is determined by the window frame definition (type and
  // bounds) and the partition size, and is constant for all the returned
  // <windows>.
  //
  // A rows-based window frame is non-deterministic if
  //   a) the order within a partition is non-deterministic, and
  //   b) the window frame definition is
  //      - BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING, or
  //      - BETWEEN CURRENT ROW AND CURRENT ROW, or
  //      - BETWEEN BETWEEN 'm' PRECEDING AND 'n' PRECEDING, where 'm' < 'n', or
  //      - BETWEEN 'm' FOLLOWING AND 'n' FOLLOWING, where 'm' > 'n', or
  //      - BETWEEN 'start_boundary' AND 'n' PRECEDING, where 'start_boundary'
  //        is a valid start boundary and 'n' is not less than the size of
  //        the partition, or
  //      - BETWEEN 'm' FOLLOWING and 'end_boundary', where 'end_boundary'
  //        is a valid end boundary and 'm' is not less than the size of
  //        the partition.
  //
  // A range-based window fame is always deterministic.
  zetasql_base::Status GetWindows(const TupleSchema& schema,
                          absl::Span<const TupleData* const> partition,
                          absl::Span<const KeyArg* const> order_keys,
                          absl::Span<const TupleData* const> params,
                          EvaluationContext* context,
                          std::vector<AnalyticWindow>* windows,
                          bool* is_deterministic) const;

 private:
  WindowFrameArg(WindowFrameType window_frame_type,
                 std::unique_ptr<WindowFrameBoundaryArg> start_boundary_arg,
                 std::unique_ptr<WindowFrameBoundaryArg> end_boundary_arg)
      : AlgebraArg(VariableId(), /*node=*/nullptr),
        window_frame_type_(window_frame_type),
        start_boundary_arg_(std::move(start_boundary_arg)),
        end_boundary_arg_(std::move(end_boundary_arg)) {}

  // Sets <is_empty> to true if the window frame is statically empty.
  // <params> gives the parameter values for evaluating constant boundary
  // offset expressions.
  //
  // The following types are statically empty:
  // a) ROWS BETWEEN 'm' PRECEDING AND 'n' PRECEDING, where 'm' < 'n';
  // b) ROWS BETWEEN 'm' FOLLOWING AND 'n' FOLLOWING, where 'm' > 'n';
  // c) ROWS BETWEEN 'start_boundary' AND 'n' PRECEDING, where 'start_boundary'
  //    is a valid start boundary and 'n' is not less than <partition_size>;
  // D) ROWS BETWEEN 'm' FOLLOWING and 'end_boundary', where 'end_boundary'
  //    is a valid end boundary and 'm' is not less than <partition_size>.
  zetasql_base::Status IsStaticallyEmpty(absl::Span<const TupleData* const> params,
                                 int partition_size, EvaluationContext* context,
                                 bool* is_empty) const;

  WindowFrameType window_frame_type_;
  std::unique_ptr<WindowFrameBoundaryArg> start_boundary_arg_;
  std::unique_ptr<WindowFrameBoundaryArg> end_boundary_arg_;
};

// Concrete base class for arguments containing relational operators.
class RelationalArg : public AlgebraArg {
 public:
  // Creates an argument from 'op', no variable.
  explicit RelationalArg(std::unique_ptr<RelationalOp> op);
  RelationalArg(const RelationalArg&) = delete;
  RelationalArg& operator=(const RelationalArg&) = delete;

  const RelationalArg* AsRelationalArg() const override { return this; }
  ~RelationalArg() override;
};

// Operator argument class used by SortOp and AggregateOp for key arguments.
class KeyArg : public ExprArg {
 public:
  enum SortOrder { kNotApplicable, kAscending, kDescending };
  enum NullOrder { kDefaultNullOrder, kNullsFirst, kNullsLast };
  explicit KeyArg(const VariableId& variable, std::unique_ptr<ValueExpr> key,
                  SortOrder order = kNotApplicable,
                  NullOrder null_order = kDefaultNullOrder)
      : ExprArg(variable, std::move(key)),
        order_(order),
        null_order_(null_order) {}

  KeyArg(const KeyArg&) = delete;
  KeyArg& operator=(const KeyArg&) = delete;

  void set_collation(std::unique_ptr<ValueExpr> collation) {
    collation_ = std::move(collation);
  }

  SortOrder order() const { return order_; }
  bool is_descending() const { return order_ == kDescending; }
  NullOrder null_order() const { return null_order_; }
  const ValueExpr* collation() const { return collation_.get(); }
  ValueExpr* mutable_collation() { return collation_.get(); }

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  SortOrder order_;
  NullOrder null_order_;
  // <collation> indicates the COLLATE specific rules to sort the std::string fields.
  // If nullptr, then strings are compared based on their UTF-8 encoding.
  std::unique_ptr<ValueExpr> collation_;
};

// Accumulator interface that supports passing in a bunch of input rows and
// getting a corresponding aggregate value.
// Example usage:
//   std::unique_ptr<AggregateArgAccumulator> accumulator = ...
//   while (...) {
//     ...
//     bool stop_accumulation;
//     ::zetasql_base::Status status;
//     if (!accumulator->Accumulate(..., &stop_accumulation, &status)) {
//       return status;
//     }
//     if (stop_accumulation) break;
//   }
//   return accumulator->GetFinalResult(/*inputs_in_defined_order=*/false);
class AggregateArgAccumulator {
 public:
  virtual ~AggregateArgAccumulator() {}

  // Resets the accumulation.
  virtual ::zetasql_base::Status Reset() = 0;

  // Accumulates 'input_row'. On success, returns true and populates
  // 'stop_accumulation' with whether the caller may skip subsequent calls to
  // Accumulate(). On failure, returns false and populates 'status'. Does not
  // return ::zetasql_base::Status for performance reasons.
  virtual bool Accumulate(const TupleData& input_row, bool* stop_accumulation,
                          ::zetasql_base::Status* status) = 0;

  // Returns the final result of the accumulation. 'inputs_in_defined_order'
  // should be true if the order that values were passed to Accumulate() was
  // defined by ZetaSQL semantics. The value of 'inputs_in_defined_order' is
  // only important if we are doing compliance or random query testing.
  virtual ::zetasql_base::StatusOr<Value> GetFinalResult(
      bool inputs_in_defined_order) = 0;
};

// Operator argument class used by AggregateOp for aggregated arguments.
class AggregateArg : public ExprArg {
 public:
  // kDistinct means that the input values to the aggregate are deduped.
  enum Distinctness { kAll, kDistinct };

  // kHavingNone indicates that no HAVING modifier is present.
  // With any other HavingModifierKind, having_expr must be non-null.
  enum HavingModifierKind { kHavingNone, kHavingMax, kHavingMin };

  static ::zetasql_base::StatusOr<std::unique_ptr<AggregateArg>> Create(
      const VariableId& variable,
      std::unique_ptr<const AggregateFunctionBody> function,
      std::vector<std::unique_ptr<ValueExpr>> arguments = {},
      Distinctness distinct = kAll,
      std::unique_ptr<ValueExpr> having_expr = nullptr,
      const HavingModifierKind having_modifier_kind = kHavingNone,
      std::vector<std::unique_ptr<KeyArg>> order_by_keys = {},
      std::unique_ptr<ValueExpr> limit = nullptr,
      ResolvedFunctionCallBase::ErrorMode error_mode =
          ResolvedFunctionCallBase::DEFAULT_ERROR_MODE);

  // Sets the schemas used in CreateAccumulator/EvalAgg.
  ::zetasql_base::Status SetSchemasForEvaluation(
      const TupleSchema& group_schema,
      absl::Span<const TupleSchema* const> params_schemas);

  // Returns an accumulator corresponding this aggregation operations.
  ::zetasql_base::StatusOr<std::unique_ptr<AggregateArgAccumulator>> CreateAccumulator(
      absl::Span<const TupleData* const> params,
      EvaluationContext* context) const;

  // Convenience method that creates an accumulator, accumulates all the rows in
  // 'group', and then returns the result.
  zetasql_base::StatusOr<Value> EvalAgg(absl::Span<const TupleData* const> group,
                                absl::Span<const TupleData* const> params,
                                EvaluationContext* context) const;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  // Sets evaluation context on input expressions.
  void SetContext(EvaluationContext* context) const;

 private:
  AggregateArg(const VariableId& variable,
               std::unique_ptr<AggregateFunctionCallExpr> function,
               Distinctness distinct, std::unique_ptr<ValueExpr> having_expr,
               const HavingModifierKind having_modifier_kind,
               std::vector<std::unique_ptr<KeyArg>> order_by_keys,
               std::unique_ptr<ValueExpr> limit,
               ResolvedFunctionCallBase::ErrorMode error_mode);

  AggregateArg(const AggregateArg&) = delete;
  AggregateArg& operator=(const AggregateArg&) = delete;

  Distinctness distinct() const { return distinct_; }

  const AggregateFunctionCallExpr* aggregate_function() const;
  AggregateFunctionCallExpr* mutable_aggregate_function();

  int num_input_fields() const;
  const Type* input_type() const;
  bool ignores_null() const;

  const std::vector<const KeyArg*>& order_by_keys() const {
    return order_by_keys_;
  }

  const ValueExpr* limit() const { return limit_.get(); }
  ValueExpr* mutable_limit() { return limit_.get(); }

  const ValueExpr* having_expr() const { return having_expr_.get(); }
  ValueExpr* mutable_having_expr() { return having_expr_.get(); }

  HavingModifierKind having_modifier_kind() const {
    return having_modifier_kind_;
  }

  ResolvedFunctionCallBase::ErrorMode error_mode() const {
    return error_mode_;
  }

  // The fields to be aggregated.
  int input_field_list_size() const { return num_input_fields(); }
  const ValueExpr* input_field(int i) const;
  ValueExpr* mutable_input_field(int i);

  // Additional literals or parameters to be passed to the aggregation
  // function. (E.g., the delimeter in STRING_AGG(s, ".").)
  int parameter_list_size() const;
  const ValueExpr* parameter(int i) const;
  ValueExpr* mutable_parameter(int i);

  const Distinctness distinct_;
  const std::unique_ptr<ValueExpr> having_expr_;
  const HavingModifierKind having_modifier_kind_;
  std::vector<const KeyArg*> order_by_keys_;
  zetasql_base::ElementDeleter order_by_keys_deleter_;
  const std::unique_ptr<ValueExpr> limit_;
  const ResolvedFunctionCallBase::ErrorMode error_mode_;
  // Set by SetSchemasForEvaluation().
  std::unique_ptr<const TupleSchema> group_schema_;
};

// Abstract expression argument class that specifies an analytic function and
// a window frame if available for AnalyticOp.
class AnalyticArg : public ExprArg {
 public:
  AnalyticArg(const AnalyticArg&) = delete;
  AnalyticArg& operator=(const AnalyticArg&) = delete;

  // Sets the schemas used in Eval.
  virtual zetasql_base::Status SetSchemasForEvaluation(
      const TupleSchema& partition_schemas,
      absl::Span<const TupleSchema* const> params_schemas) = 0;

  // Evaluates the analytic function over the tuples in <partition>.
  // <order_keys> gives the ordering keys and directions of <partition> and can
  // be empty.
  // <params> gives the parameter values for expression evaluation.
  virtual zetasql_base::Status Eval(absl::Span<const TupleData* const> partition,
                            absl::Span<const KeyArg* const> order_keys,
                            absl::Span<const TupleData* const> params,
                            EvaluationContext* context,
                            std::vector<Value>* values) const = 0;

 protected:
  // Takes ownership of <window_frame>.
  AnalyticArg(const VariableId& variable, const Type* type,
              std::unique_ptr<WindowFrameArg> window_frame,
              ResolvedFunctionCallBase::ErrorMode error_mode)
      : ExprArg(variable, type),
        window_frame_(std::move(window_frame)),
        error_mode_(error_mode) {}

  // Can be nullptr.
  const std::unique_ptr<WindowFrameArg> window_frame_;
  const ResolvedFunctionCallBase::ErrorMode error_mode_;
};

// Aggregate analytic expression argument.
class AggregateAnalyticArg : public AnalyticArg {
 public:
  // 'window_frame' cannot be nullptr, because all aggregate functions must
  // support window framing.
  static ::zetasql_base::StatusOr<std::unique_ptr<AggregateAnalyticArg>> Create(
      std::unique_ptr<WindowFrameArg> window_frame,
      std::unique_ptr<AggregateArg> aggregator,
      ResolvedFunctionCallBase::ErrorMode error_mode) {
    return absl::WrapUnique(new AggregateAnalyticArg(
        std::move(window_frame), std::move(aggregator), error_mode));
  }

  AggregateAnalyticArg(const AggregateAnalyticArg&) = delete;
  AggregateAnalyticArg& operator=(const AggregateAnalyticArg&) = delete;

  zetasql_base::Status SetSchemasForEvaluation(
      const TupleSchema& partition_schema,
      absl::Span<const TupleSchema* const> params_schemas) override;

  zetasql_base::Status Eval(absl::Span<const TupleData* const> partition,
                    absl::Span<const KeyArg* const> order_keys,
                    absl::Span<const TupleData* const> params,
                    EvaluationContext* context,
                    std::vector<Value>* values) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  // 'window_frame' cannot be nullptr, because all aggregate functions must
  // support window framing.
  AggregateAnalyticArg(std::unique_ptr<WindowFrameArg> window_frame,
                       std::unique_ptr<AggregateArg> aggregator,
                       ResolvedFunctionCallBase::ErrorMode error_mode)
      : AnalyticArg(aggregator->variable(), aggregator->type(),
                    std::move(window_frame), error_mode),
        aggregator_(std::move(aggregator)) {}

  std::unique_ptr<AggregateArg> aggregator_;
  // Set by SetSchemasForEvaluation().
  std::unique_ptr<const TupleSchema> partition_schema_;
};

// Non-aggregate analytic expression argument.
class NonAggregateAnalyticArg : public AnalyticArg {
 public:
  NonAggregateAnalyticArg(const NonAggregateAnalyticArg&) = delete;
  NonAggregateAnalyticArg& operator=(const NonAggregateAnalyticArg&) = delete;

  // <window_frame> can be null if <function> does not support window frames.
  static ::zetasql_base::StatusOr<std::unique_ptr<NonAggregateAnalyticArg>> Create(
      const VariableId& variable_id,
      std::unique_ptr<WindowFrameArg> window_frame,
      std::unique_ptr<const AnalyticFunctionBody> function,
      std::vector<std::unique_ptr<ValueExpr>> non_const_arguments,
      std::vector<std::unique_ptr<ValueExpr>> const_arguments,
      ResolvedFunctionCallBase::ErrorMode error_mode);

  zetasql_base::Status SetSchemasForEvaluation(
      const TupleSchema& partition_schema,
      absl::Span<const TupleSchema* const> params_schemas) override;

  zetasql_base::Status Eval(absl::Span<const TupleData* const> partition,
                    absl::Span<const KeyArg* const> order_keys,
                    absl::Span<const TupleData* const> params,
                    EvaluationContext* context,
                    std::vector<Value>* values) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  NonAggregateAnalyticArg(
      const VariableId& variable_id,
      std::unique_ptr<WindowFrameArg> window_frame,
      const Type* function_output_type,
      std::unique_ptr<AnalyticFunctionCallExpr> function_call,
      ResolvedFunctionCallBase::ErrorMode error_mode);

  std::unique_ptr<AnalyticFunctionCallExpr> function_call_;
  // Set by SetSchemasForEvaluation().
  std::unique_ptr<const TupleSchema> partition_schema_;
};

// An argument in the tree that generates column filters for an
// EvaluatorTableScanOp.
class ColumnFilterArg : public AlgebraArg {
 public:
  explicit ColumnFilterArg(int column_idx)
      : AlgebraArg(VariableId(), /*node=*/nullptr), column_idx_(column_idx) {}

  ~ColumnFilterArg() override {}

  // Sets the TupleSchemas for the TupleDatas passed to Eval(). A particular
  // VariableId can only occur in one TupleSchema.
  virtual ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) = 0;

  // Returns a ColumnFilter corresponding to the given parameters.
  virtual ::zetasql_base::StatusOr<std::unique_ptr<ColumnFilter>> Eval(
      absl::Span<const TupleData* const> params,
      EvaluationContext* context) const = 0;

  int column_idx() const { return column_idx_; }

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override = 0;

 private:
  const int column_idx_;
};

// Represents a ColumnFilter of the form $column IN <array>.
class InArrayColumnFilterArg : public ColumnFilterArg {
 public:
  InArrayColumnFilterArg(const InArrayColumnFilterArg&) = delete;
  InArrayColumnFilterArg& operator=(const InArrayColumnFilterArg&) = delete;

  // 'variable' is the VariableId used for the column for debug
  // logging. 'column_idx' is the index of the column in the scan (not the
  // Table).
  static ::zetasql_base::StatusOr<std::unique_ptr<InArrayColumnFilterArg>> Create(
      const VariableId& variable, int column_idx,
      std::unique_ptr<ValueExpr> array);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<ColumnFilter>> Eval(
      absl::Span<const TupleData* const> params,
      EvaluationContext* context) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  InArrayColumnFilterArg(const VariableId& variable, int column_idx,
                         std::unique_ptr<ValueExpr> array);

  const VariableId variable_;
  std::unique_ptr<ValueExpr> array_;
};

// Represents a ColumnFilter of the form $column IN
// <list>. InArrayColumnFilterArg cannot be used for this purpose because the
// element type of <list> may be an array, and ZetaSQL does not support arrays
// of arrays.
class InListColumnFilterArg : public ColumnFilterArg {
 public:
  InListColumnFilterArg(const InListColumnFilterArg&) = delete;
  InListColumnFilterArg& operator=(const InListColumnFilterArg&) = delete;

  // 'variable' is the VariableId used for the column for debug
  // logging. 'column_idx' is the index of the column in the scan (not the
  // Table).
  static ::zetasql_base::StatusOr<std::unique_ptr<InListColumnFilterArg>> Create(
      const VariableId& variable, int column_idx,
      std::vector<std::unique_ptr<ValueExpr>> elements);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<ColumnFilter>> Eval(
      absl::Span<const TupleData* const> params,
      EvaluationContext* context) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  InListColumnFilterArg(const VariableId& variable, int column_idx,
                        std::vector<std::unique_ptr<ValueExpr>> elements);

  const VariableId variable_;
  std::vector<std::unique_ptr<ValueExpr>> elements_;
};

// Represents a ColumnFilter for (-infinity, <arg>] or [<arg>, infinity).
class HalfUnboundedColumnFilterArg : public ColumnFilterArg {
 public:
  enum Kind { kLE, kGE };

  HalfUnboundedColumnFilterArg(const HalfUnboundedColumnFilterArg&) = delete;
  HalfUnboundedColumnFilterArg& operator=(const HalfUnboundedColumnFilterArg&) =
      delete;

  // 'variable' is the VariableId used for the column for debug
  // logging. 'column_idx' is the index of the column in the scan (not the
  // Table).
  static ::zetasql_base::StatusOr<std::unique_ptr<HalfUnboundedColumnFilterArg>> Create(
      const VariableId& variable, int column_idx, Kind kind,
      std::unique_ptr<ValueExpr> arg);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<ColumnFilter>> Eval(
      absl::Span<const TupleData* const> params,
      EvaluationContext* context) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  HalfUnboundedColumnFilterArg(const VariableId& variable, int column_idx,
                               Kind kind, std::unique_ptr<ValueExpr> arg);

  const VariableId variable_;
  const Kind kind_;
  std::unique_ptr<ValueExpr> arg_;
};

// Abstract base class for an operator.
class AlgebraNode {
 public:
  // Printing mode of an operator argument used in ArgDebugString.
  enum ArgPrintMode {
    k0,  // Don't print, always empty.
    k1,  // Print as a single argument.
    kN   // Print as a repeated argument.
  };

  // For printing tree lines.
  static constexpr char kIndentFork[] = "+-";
  static constexpr char kIndentBar[] = "| ";
  static constexpr char kIndentSpace[] = "  ";

  AlgebraNode() {}
  AlgebraNode(const AlgebraNode&) = delete;
  AlgebraNode& operator=(const AlgebraNode&) = delete;
  virtual ~AlgebraNode();

  // Downcast methods, return nullptr if downcast is invalid.
  virtual const ValueExpr* AsValueExpr() const;
  virtual ValueExpr* AsMutableValueExpr();
  virtual const RelationalOp* AsRelationalOp() const;
  virtual RelationalOp* AsMutableRelationalOp();

  // Value and aggregator operators have an output type.
  virtual const Type* output_type() const = 0;

  // Returns a std::string representation of the operator for debugging. If
  // 'verbose' is true, prints more information.
  std::string DebugString(bool verbose = false) const;

  // Returns a std::string representation of the operator for debugging.
  // 'level' specifies the indentation level of the output std::string. If 'verbose'
  // is true, prints more information.
  virtual std::string DebugInternal(const std::string& indent,
                                    bool verbose) const = 0;

  // Returns all arguments of the operator.
  absl::Span<const AlgebraArg* const> GetArgs() const { return args_; }
  absl::Span<AlgebraArg* const> GetMutableArgs() { return args_; }

  // Returns a singleton argument of the given 'kind'.
  const AlgebraArg* GetArg(int kind) const;
  AlgebraArg* GetMutableArg(int kind);

  // Returns an argument that produces the given 'variable'.
  const AlgebraArg* FindArgByVariable(const VariableId& variable) const;

  // Returns a repeated argument of the given 'kind', downcast to T.
  template <typename T>
  absl::Span<const T* const> GetArgs(int kind) const {
    int start = arg_slices_[kind].start;
    int size = arg_slices_[kind].size;
    if (size > 0) {
      return absl::Span<const T* const>(
          reinterpret_cast<const T* const*>(&args_[start]), size);
    } else {
      return absl::Span<const T* const>();
    }
  }

  // Returns a repeated argument of the given 'kind', downcast to T.
  template <typename T>
  absl::Span<T* const> GetMutableArgs(int kind) {
    int start = arg_slices_[kind].start;
    int size = arg_slices_[kind].size;
    if (size > 0) {
      return absl::Span<T* const>(reinterpret_cast<T* const*>(&args_[start]),
                                  size);
    } else {
      return absl::Span<T* const>();
    }
  }

  // Returns a debug std::string representation of 'node', which must have
  // 'arg_names.size()' arguments. Each argument is printed with corresponding
  // entry of 'arg_mode' (which must have the same number of elements as
  // 'arg_names').
  std::string ArgDebugString(absl::Span<const std::string> arg_names,
                             absl::Span<const ArgPrintMode> arg_mode,
                             const std::string& indent, bool verbose) const;

 protected:
  // Set methods are to be called in the constructor. Argument 'kind' of an
  // operator is typically an enum defined in the operator class.

  // Sets a singleton argument of the given 'kind'.
  void SetArg(int kind, std::unique_ptr<AlgebraArg> argument);

  // Sets a repeated argument of the given 'kind'.
  template <typename T>
  void SetArgs(int kind, std::vector<std::unique_ptr<T>> args) {
    if (kind >= arg_slices_.size()) {
      arg_slices_.resize(kind + 1);
    }
    for (auto& argument : args) {
      argument->set_kind(kind);
      args_.push_back(argument.release());
    }
    arg_slices_[kind] = ArgSlice(args_.size() - args.size(), args.size());
  }

 private:
  // An argument of a given 'kind' is represented as a slice of the argument
  // vector 'args_' that contains a list of all arguments combined.
  struct ArgSlice {
    ArgSlice() : start(0), size(0) {}
    ArgSlice(int start, int size) : start(start), size(size) {}
    int start;  // into AlgebraNode::args_
    int size;  // number of operators for that argument
    // Intentionally copyable.
  };
  std::vector<ArgSlice> arg_slices_;  // array slices of args_
  std::vector<AlgebraArg*> args_;     // owned
};

// Abstract base class for value operators.
class ValueExpr : public AlgebraNode {
 public:
  explicit ValueExpr(const Type* output_type) : output_type_(output_type) {}
  ValueExpr(const ValueExpr&) = delete;
  ValueExpr& operator=(const ValueExpr&) = delete;

  ~ValueExpr() override;

  // Sets the TupleSchemas for the TupleDatas passed to Eval(). A particular
  // VariableId can only occur in one TupleSchema.
  virtual ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) = 0;

  // Evaluates the ValueExpr using 'params'. Requires that
  // SetSchemasForEvaluation() has already been called. On success, populates
  // 'result' and returns true. On failure, populates 'status' and returns
  // false. We avoid returning ::zetasql_base::Status for performance reasons.
  virtual bool Eval(absl::Span<const TupleData* const> params,
                    EvaluationContext* context, VirtualTupleSlot* result,
                    ::zetasql_base::Status* status) const = 0;

  // Convenience method for populating a TupleSlot.
  bool EvalSimple(absl::Span<const TupleData* const> params,
                  EvaluationContext* context, TupleSlot* result,
                  ::zetasql_base::Status* status) const {
    VirtualTupleSlot virtual_slot(result);
    return Eval(params, context, &virtual_slot, status);
  }

  const ValueExpr* AsValueExpr() const override { return this; }
  ValueExpr* AsMutableValueExpr() override { return this; }

  const Type* output_type() const override { return output_type_; }

  virtual bool IsConstant() const { return false; }

 private:
  const Type* output_type_;
};

// Abstract base class for relational operators.
class RelationalOp : public AlgebraNode {
 public:
  RelationalOp() {}
  RelationalOp(const RelationalOp&) = delete;
  RelationalOp& operator=(const RelationalOp&) = delete;
  ~RelationalOp() override;

  // Sets the TupleSchemas for the TupleDatas passed to Eval(). A particular
  // VariableId can only occur in one TupleSchema.
  virtual ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) = 0;

  // Returns an iterator over the tuples representing the relation corresponding
  // to this operator and 'params'. The tuples returned by the iterator have an
  // extra 'num_extra_slots' at the end to allow stacked iterators to avoid
  // copying a tuple into a wider tuple augmented with more slots. The lifetime
  // of the iterator must not exceed the lifetime of the RelationalOp.
  //
  // The schemas for 'params' must have already been set by a call to
  // SetSchemasForEvaluation().
  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> Eval(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const;

  // This is the method that actually creates the iterator for Eval(), which
  // wraps it in a PassThroughTupleIterator to allow for cancellation while it
  // is running. This method is only public for internal purposes. Users should
  // call Eval() instead.
  virtual ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const = 0;

  // Returns a copy of the output schema of the TupleIterator corresponding to
  // this operator.
  virtual std::unique_ptr<TupleSchema> CreateOutputSchema() const = 0;

  // Returns the result of constructing a TupleIterator with this object with
  // scrambling disabled and getting its debug std::string. If it isn't possible to
  // determine that debug std::string (e.g., it requires evaluating an expression),
  // returns an approximation.
  virtual std::string IteratorDebugString() const = 0;

  const RelationalOp* AsRelationalOp() const override { return this; }
  RelationalOp* AsMutableRelationalOp() override { return this; }

  const Type* output_type() const override {
    LOG(FATAL) << "Relational operators have no type";
  }

  // Order-preservation is copied from the resolved AST.
  bool is_order_preserving() const { return is_order_preserving_; }

  // 'is_order_preserving' may be true only if the operator
  // 'may_preserve_order()'.
  zetasql_base::Status set_is_order_preserving(bool is_order_preserving);

  // Relational operators typically do not preserve order.
  virtual bool may_preserve_order() const { return false; }

 protected:
  // Depending on the EvaluationOptions in 'context', either returns 'iter' or a
  // ReorderingTupleIterator that wraps 'iter'.
  zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> MaybeReorder(
      std::unique_ptr<TupleIterator> iter, EvaluationContext* context) const;

 private:
  // If false, the operator's output is never marked as ordered.
  bool is_order_preserving_ = false;
};

// -------------------------------------------------------
// Relational operators
// -------------------------------------------------------

// Produces a relation from a Table (possibly with more data than can
// simultaneously fit in memory).
class EvaluatorTableScanOp : public RelationalOp {
 public:
  EvaluatorTableScanOp(const EvaluatorTableScanOp&) = delete;
  EvaluatorTableScanOp& operator=(const EvaluatorTableScanOp&) = delete;

  static std::string GetIteratorDebugString(absl::string_view table_name);

  static ::zetasql_base::StatusOr<std::unique_ptr<EvaluatorTableScanOp>> Create(
      const Table* table, const std::string& alias,
      absl::Span<const int> column_idxs,
      absl::Span<const std::string> column_names,
      absl::Span<const VariableId> variables,
      std::vector<std::unique_ptr<ColumnFilterArg>> and_filters);

  // Returns a ColumnFilter corresponding to the intersection of 'filters'. This
  // method is only public for unit testing purposes.
  static ::zetasql_base::StatusOr<std::unique_ptr<ColumnFilter>> IntersectColumnFilters(
      const std::vector<std::unique_ptr<ColumnFilter>>& filters);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the TupleSchema corresponding to the 'variables' passed to the
  // constructor.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  EvaluatorTableScanOp(
      const Table* table, const std::string& alias,
      absl::Span<const int> column_idxs,
      absl::Span<const std::string> column_names,
      absl::Span<const VariableId> variables,
      std::vector<std::unique_ptr<ColumnFilterArg>> and_filters);

  const Table* table_;
  const std::string alias_;
  const std::vector<int> column_idxs_;
  const std::vector<std::string> column_names_;
  const std::vector<VariableId> variables_;
  std::vector<std::unique_ptr<ColumnFilterArg>> and_filters_;
};

// Evaluates some expressions and makes them available to 'body'. Each
// expression is allowed to depend on the results of the previous expressions.
class LetOp : public RelationalOp {
 public:
  LetOp(const LetOp&) = delete;
  LetOp& operator=(const LetOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view input_debug_string);

  static ::zetasql_base::StatusOr<std::unique_ptr<LetOp>> Create(
      std::vector<std::unique_ptr<ExprArg>> assign,
      std::unique_ptr<RelationalOp> body);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the TupleSchema of the 'body' passed to the constructor.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kAssign, kBody };

  LetOp(std::vector<std::unique_ptr<ExprArg>> assign,
        std::unique_ptr<RelationalOp> body);

  absl::Span<const ExprArg* const> assign() const;
  absl::Span<ExprArg* const> mutable_assign();

  const RelationalOp* body() const;
  RelationalOp* mutable_body();
};

// Produces a relation by joining 'left' and 'right' inputs on the specified
// 'condition'. Four kinds of joins are supported: inner, left outer, right
// outer, and full outer join. The 'left_outputs' expressions are evaluated on
// each joining left tuple and are appended to the output tuple; for left outer
// and full outer joins, if a left tuple has no joining right tuple,
// 'left_outputs' is padded with NULLs. Similarly, 'right_outputs' expressions
// are evaluated on each joining right tuple and are padded with NULLs for
// non-joining tuples for right outer and full outer joins. 'condition' is
// evaluated on a pair of left/right input tuple. It is allowed to reference the
// variables produced by either input.
//
// Inner join passes through all variables from the input. Left outer join
// passes through the variables from the left but no variables from the right;
// 'right_outputs' needs to be specified to retrieve the data from right
// tuples. Right outer join passes through variables from the right but no
// variables from the left; 'left_outputs' needs to be specified to retrieve
// data from left tuples. Full outer join does not pass through any variables
// from either input; both 'left_outputs' and 'right_outputs' should be used to
// produce non-empty output tuples. 'left_outputs' may be non-empty for right
// outer or full outer join, and 'right_outputs' may be non-empty for left outer
// or full outer join.
//
// Cross apply is inner join with left-right correlation. Outer apply is left
// outer join with left-right correlation. Inner, left, and full join must not
// have correlated parameters; their inputs must be evaluated only once for
// correctness. Correlated input of cross/outer apply may be evaluated multiple
// times even if no correlated references are present.
class JoinOp : public RelationalOp {
 public:
  enum JoinKind {
    kInnerJoin,
    kLeftOuterJoin,
    kRightOuterJoin,
    kFullOuterJoin,
    kCrossApply,
    kOuterApply
  };

  // Represents an equality in the join condition where one side is determined
  // by the left-hand side of the join and the other side is determinded by the
  // right-hand side of the join (which must not be correlated). The algebrizer
  // attempts to represent the join condition in terms of HashJoinEqualityExprs
  // and an arbitrary bool-Value'd ValueExpr for the remainder. If
  // HashJoinEqualityExprs are present, the join algorithm is a hash join where
  // the right-hand side is the build side.
  struct HashJoinEqualityExprs {
    std::unique_ptr<ExprArg> left_expr;
    std::unique_ptr<ExprArg> right_expr;
  };

  JoinOp(const JoinOp&) = delete;
  JoinOp& operator=(const JoinOp&) = delete;

  static const std::string& JoinKindToString(JoinKind join_kind);

  static std::string GetIteratorDebugString(
      JoinKind join_kind, absl::string_view left_input_debug_string,
      absl::string_view right_input_debug_string);

  // 'equality_exprs' must be empty for cross/outer apply.
  static ::zetasql_base::StatusOr<std::unique_ptr<JoinOp>> Create(
      JoinKind kind, std::vector<HashJoinEqualityExprs> equality_exprs,
      std::unique_ptr<ValueExpr> remaining_condition,
      std::unique_ptr<RelationalOp> left, std::unique_ptr<RelationalOp> right,
      std::vector<std::unique_ptr<ExprArg>> left_outputs,
      std::vector<std::unique_ptr<ExprArg>> right_outputs);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema consisting of the following, in order: left inputs, left
  // outputs, right inputs, right outputs. Any of these may be missing depending
  // on the join kind as described in the class comment.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind {
    kLeftOutput,
    kRightOutput,
    kHashJoinEqualityLeftExprs,
    kHashJoinEqualityRightExprs,
    kRemainingCondition,
    kLeftInput,
    kRightInput
  };

  JoinOp(JoinKind kind,
         std::vector<std::unique_ptr<ExprArg>> hash_join_equality_left_exprs,
         std::vector<std::unique_ptr<ExprArg>> hash_join_equality_right_exprs,
         std::unique_ptr<ValueExpr> remaining_condition,
         std::unique_ptr<RelationalOp> left,
         std::unique_ptr<RelationalOp> right,
         std::vector<std::unique_ptr<ExprArg>> left_outputs,
         std::vector<std::unique_ptr<ExprArg>> right_outputs);

  absl::Span<const ExprArg* const> hash_join_equality_left_exprs() const;
  absl::Span<ExprArg* const> mutable_hash_join_equality_left_exprs();

  absl::Span<const ExprArg* const> hash_join_equality_right_exprs() const;
  absl::Span<ExprArg* const> mutable_hash_join_equality_right_exprs();

  const ValueExpr* remaining_join_expr() const;
  ValueExpr* mutable_remaining_join_expr();

  const RelationalOp* left_input() const;
  RelationalOp* mutable_left_input();

  const RelationalOp* right_input() const;
  RelationalOp* mutable_right_input();

  absl::Span<const ExprArg* const> left_outputs() const;
  absl::Span<ExprArg* const> mutable_left_outputs();

  absl::Span<const ExprArg* const> right_outputs() const;
  absl::Span<ExprArg* const> mutable_right_outputs();

  const JoinKind join_kind_;
};

// Partitions the input using 'keys' and returns tuples constructed from
// 'aggregators' evaluated on each partition.
class AggregateOp : public RelationalOp {
 public:
  AggregateOp(const AggregateOp&) = delete;
  AggregateOp& operator=(const AggregateOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view input_iter_debug_string);

  // Creates a validated AggregateOp that checks whether the keys can be
  // compared for equality and that no collations are used.
  static zetasql_base::StatusOr<std::unique_ptr<AggregateOp>> Create(
      std::vector<std::unique_ptr<KeyArg>> keys,
      std::vector<std::unique_ptr<AggregateArg>> aggregators,
      std::unique_ptr<RelationalOp> input);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema consisting of the variables for the keys, followed by
  // the variables for the aggregators.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kKey, kAggregator, kInput };

  AggregateOp(std::vector<std::unique_ptr<KeyArg>> keys,
              std::vector<std::unique_ptr<AggregateArg>> aggregators,
              std::unique_ptr<RelationalOp> input);

  absl::Span<const KeyArg* const> keys() const;
  absl::Span<KeyArg* const> mutable_keys();

  absl::Span<const AggregateArg* const> aggregators() const;
  absl::Span<AggregateArg* const> mutable_aggregators();

  const RelationalOp* input() const;
  RelationalOp* mutable_input();
};

// Partitions the input by <partition_keys>, and evaluates a number of analytic
// functions on each input tuple based on a set of related tuples in the same
// partition. All analytic functions in an AnalyticOp must have the exact same
// ordering. The implication is that if we had
// 'sum(x) over (partition by a order by b)' and
// 'sum(x) over (partition by a order by b, c)', we would have two different
// AnalyticOps for them, not one even though they can leverage the same sort
// ordering.
//
// Note that this operator assumes that the input relation is ordered by
// <partition_keys> and <order_keys>.
class AnalyticOp : public RelationalOp {
 public:
  AnalyticOp(const AnalyticOp&) = delete;
  AnalyticOp& operator=(const AnalyticOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view input_iter_debug_string);

  static ::zetasql_base::StatusOr<std::unique_ptr<AnalyticOp>> Create(
      std::vector<std::unique_ptr<KeyArg>> partition_keys,
      std::vector<std::unique_ptr<KeyArg>> order_keys,
      std::vector<std::unique_ptr<AnalyticArg>> analytic_args,
      std::unique_ptr<RelationalOp> input, bool preserves_order);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema consisting of the variables for the input, followed by
  // the partition keys, then the order keys, then the analytic arguments.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  bool may_preserve_order() const override { return true; }

 private:
  enum ArgKind { kPartitionKey, kOrderKey, kAnalytic, kInput };

  AnalyticOp(std::vector<std::unique_ptr<KeyArg>> partition_keys,
             std::vector<std::unique_ptr<KeyArg>> order_keys,
             std::vector<std::unique_ptr<AnalyticArg>> analytic_args,
             std::unique_ptr<RelationalOp> input);

  absl::Span<const KeyArg* const> partition_keys() const;
  absl::Span<KeyArg* const> mutable_partition_keys();

  absl::Span<const KeyArg* const> order_keys() const;
  absl::Span<KeyArg* const> mutable_order_keys();

  absl::Span<const AnalyticArg* const> analytic_args() const;
  absl::Span<AnalyticArg* const> mutable_analytic_args();

  const RelationalOp* input() const;
  RelationalOp* mutable_input();
};

// Sorts 'values' in 'input' using 'keys'.
class SortOp : public RelationalOp {
 public:
  SortOp(const SortOp&) = delete;
  SortOp& operator=(const SortOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view input_iter_debug_string);

  // Creates a validated SortOp that checks whether the keys can be
  // compared for equality.
  //
  // 'limit' and 'offset' must either both be NULL or both non-NULL. If 'limit'
  // is non-NULL, the resulting iterator only returns the 'limit' tuples after
  // 'offset' in sorted order. This is equivalent to LimitOp(limit, offset,
  // SortOp) but uses less memory. However, it is not safe to use this feature
  // for compliance or random query testing because it does not support
  // scrambling or setting non-deterministic test output, so it can cause
  // spurious test failures.
  //
  // We do not support setting both 'limit' and 'is_stable_sort'.
  static zetasql_base::StatusOr<std::unique_ptr<SortOp>> Create(
      std::vector<std::unique_ptr<KeyArg>> keys,
      std::vector<std::unique_ptr<ExprArg>> values,
      std::unique_ptr<ValueExpr> limit, std::unique_ptr<ValueExpr> offset,
      std::unique_ptr<RelationalOp> input, bool is_order_preserving,
      bool is_stable_sort);

  zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema consisting of the keys followed by the values.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  bool may_preserve_order() const override { return true; }

 private:
  enum ArgKind { kKey, kValue, kLimit, kOffset, kInput };

  SortOp(std::vector<std::unique_ptr<KeyArg>> keys,
         std::vector<std::unique_ptr<ExprArg>> values,
         std::unique_ptr<ValueExpr> limit, std::unique_ptr<ValueExpr> offset,
         std::unique_ptr<RelationalOp> input, bool is_stable_sort);

  absl::Span<const KeyArg* const> keys() const;
  absl::Span<KeyArg* const> mutable_keys();

  absl::Span<const ExprArg* const> values() const;
  absl::Span<ExprArg* const> mutable_values();

  bool has_limit() const { return has_limit_; }
  const ValueExpr* limit() const;
  ValueExpr* mutable_limit();

  bool has_offset() const { return has_offset_; }
  const ValueExpr* offset() const;
  ValueExpr* mutable_offset();

  const RelationalOp* input() const;
  RelationalOp* mutable_input();

  const bool has_limit_;
  const bool has_offset_;
  const bool is_stable_sort_;
};

// Scans (or unnests) an 'array' as a relation. Each output tuple contains an
// optional 'element' variable bound to an element of the array (if 'element' is
// non-empty), and an optional 'position' variable (if 'position' is
// non-empty). If the array has not fully specified order, output positions are
// non-deterministic. Note that all tables have unspecified order of rows.
// For an 'array' of structs, 'fields' may specify (variable, field_index)
// pairs (which are useful for scanning a table represented as an array of
// structs in the compliance test framework). field_index refers to the field
// number in the struct type inside the array type.
class ArrayScanOp : public RelationalOp {
 public:
  // When ArrayScanOp is used to represent a scan of a table represented by an
  // array of structs (e.g., in the compliance tests), this class is used to
  // represent one of the columns being scanned (each one corresponds to a field
  // in the struct with an associated variable).
  class FieldArg : public ExprArg {
   public:
    FieldArg(const VariableId& variable, int field_index, const Type* type)
        : ExprArg(variable, type), field_index_(field_index) {}
    int field_index() const { return field_index_; }
    std::string DebugInternal(const std::string& indent,
                              bool verbose) const override;

   private:
    int field_index_;
  };

  ArrayScanOp(const ArrayScanOp&) = delete;
  ArrayScanOp& operator=(const ArrayScanOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view array_debug_string);

  static ::zetasql_base::StatusOr<std::unique_ptr<ArrayScanOp>> Create(
      const VariableId& element, const VariableId& position,
      absl::Span<const std::pair<VariableId, int>> fields,
      std::unique_ptr<ValueExpr> array);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema consisting of the fields, followed optionally by
  // 'element' and 'position' (depending on whether those VariableIds are
  // valid).
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kElement, kPosition, kField, kArray };

  // 'fields' contains (variable, field_index) pairs given an 'array' of
  // structs, and must be empty otherwise.
  ArrayScanOp(const VariableId& element, const VariableId& position,
              absl::Span<const std::pair<VariableId, int>> fields,
              std::unique_ptr<ValueExpr> array);

  const VariableId& element() const;  // May be empty, i.e., unused.
  const VariableId& position() const;  // May be empty, i.e., unused.
  absl::Span<const FieldArg* const> field_list() const;

  const ValueExpr* array_expr() const;
  ValueExpr* mutable_array_expr();
};

// Returns the union of N relations in 'inputs'. Each output tuple is
// constructed by evaluating M value operators. (The resolved AST allows for
// union operations to arbitrarily remap the columns in an underlying scan,
// although there may not be syntax to fully exploit that generality.)
class UnionAllOp : public RelationalOp {
 public:
  UnionAllOp(const UnionAllOp&) = delete;
  UnionAllOp& operator=(const UnionAllOp&) = delete;

  using Input = std::pair<std::unique_ptr<RelationalOp>,
                          std::vector<std::unique_ptr<ExprArg>>>;

  static std::string GetIteratorDebugString(
      absl::Span<const std::string> input_iter_debug_string);

  static ::zetasql_base::StatusOr<std::unique_ptr<UnionAllOp>> Create(
      std::vector<Input> inputs);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema consisting of the variables passed in the ExprArgs to
  // the constructor.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  explicit UnionAllOp(std::vector<Input> inputs);

  absl::Span<const ExprArg* const> values(int i) const;
  absl::Span<ExprArg* const> mutable_values(int i);

  const RelationalOp* rel(int i) const;
  RelationalOp* mutable_rel(int i);

  int num_rel() const { return num_rel_; }
  const VariableId& variable(int i) const;
  int num_variables() const;

  const int num_rel_;
};

// Augments the tuples from 'input' by 'map' slots computed for each tuple.
// map[i + 1] may depend on variables produced by map[0]...map[i].
class ComputeOp : public RelationalOp {
 public:
  ComputeOp(const ComputeOp&) = delete;
  ComputeOp& operator=(const ComputeOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view input_iter_debug_string);

  static ::zetasql_base::StatusOr<std::unique_ptr<ComputeOp>> Create(
      std::vector<std::unique_ptr<ExprArg>> map,
      std::unique_ptr<RelationalOp> input);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema consisting of the variables from the input, followed the
  // variables passed as 'map' to the constructor.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kMap, kInput };

  ComputeOp(std::vector<std::unique_ptr<ExprArg>> map,
            std::unique_ptr<RelationalOp> input);

  absl::Span<const ExprArg* const> map() const;
  absl::Span<ExprArg* const> mutable_map();

  const RelationalOp* input() const;
  RelationalOp* mutable_input();
};

// Discards tuples of 'input' on which 'predicate' evaluates to false or NULL.
class FilterOp : public RelationalOp {
 public:
  FilterOp(const FilterOp&) = delete;
  FilterOp& operator=(const FilterOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view input_iter_debug_string);

  static ::zetasql_base::StatusOr<std::unique_ptr<FilterOp>> Create(
      std::unique_ptr<ValueExpr> predicate,
      std::unique_ptr<RelationalOp> input);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema of the input.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kPredicate, kInput };

  FilterOp(std::unique_ptr<ValueExpr> predicate,
           std::unique_ptr<RelationalOp> input);

  const ValueExpr* predicate() const;
  ValueExpr* mutable_predicate();

  const RelationalOp* input() const;
  RelationalOp* mutable_input();
};

// Skips 'offset' tuples of 'input' and returns the next 'row_count' tuples.
// Produces non-deterministic result if the order of the input is unspecified.
class LimitOp : public RelationalOp {
 public:
  LimitOp(const LimitOp&) = delete;
  LimitOp& operator=(const LimitOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view input_iter_debug_string);

  static ::zetasql_base::StatusOr<std::unique_ptr<LimitOp>> Create(
      std::unique_ptr<ValueExpr> row_count, std::unique_ptr<ValueExpr> offset,
      std::unique_ptr<RelationalOp> input, bool is_order_preserving);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns the schema of the input.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  bool may_preserve_order() const override { return true; }

 private:
  enum ArgKind { kRowCount, kOffset, kInput };

  LimitOp(std::unique_ptr<ValueExpr> row_count,
          std::unique_ptr<ValueExpr> offset,
          std::unique_ptr<RelationalOp> input);

  const ValueExpr* row_count() const;
  ValueExpr* mutable_row_count();

  const ValueExpr* offset() const;
  ValueExpr* mutable_offset();

  const RelationalOp* input() const;
  RelationalOp* mutable_input();
};

// Relation with no columns emitting N rows. N specified as an integer
// expression. If N is negative, returns 0 rows. This operator is used to
// represent a single-row relation (e.g., in SELECT 1) and N-row relations in
// EXCEPT ALL / INTERSECT ALL queries.
class EnumerateOp : public RelationalOp {
 public:
  EnumerateOp(const EnumerateOp&) = delete;
  EnumerateOp& operator=(const EnumerateOp&) = delete;

  static std::string GetIteratorDebugString(
      absl::string_view count_debug_string);

  static ::zetasql_base::StatusOr<std::unique_ptr<EnumerateOp>> Create(
      std::unique_ptr<ValueExpr> row_count);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  // Returns a schema with no variables.
  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kRowCount };

  explicit EnumerateOp(std::unique_ptr<ValueExpr> row_count);

  const ValueExpr* row_count() const;
  ValueExpr* mutable_row_count();
};

// -------------------------------------------------------
// Value expressions
// -------------------------------------------------------

// Returns the contents of a table 'table_name' as an array.
class TableAsArrayExpr : public ValueExpr {
 public:
  TableAsArrayExpr(const TableAsArrayExpr&) = delete;
  TableAsArrayExpr& operator=(const TableAsArrayExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<TableAsArrayExpr>> Create(
      const std::string& table_name, const ArrayType* type);

  const std::string& table_name() const { return table_name_; }

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  TableAsArrayExpr(const std::string& table_name, const ArrayType* type);

  const std::string table_name_;
};

// Returns the value of variable (or attribute) 'name' of the given 'type'.
class DerefExpr : public ValueExpr {
 public:
  DerefExpr(const DerefExpr&) = delete;
  DerefExpr& operator=(const DerefExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<DerefExpr>> Create(
      const VariableId& name, const Type* type);

  const VariableId& name() const { return name_; }

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  DerefExpr(const VariableId& name, const Type* type);

  VariableId name_;
  // Set by SetSchemasForEvaluation().
  int idx_in_params_ = -1;
  int slot_ = -1;
};

// Returns field 'field_name' (or 'field_index') from 'expr'.
class FieldValueExpr : public ValueExpr {
 public:
  FieldValueExpr(const FieldValueExpr&) = delete;
  FieldValueExpr& operator=(const FieldValueExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<FieldValueExpr>> Create(
      absl::string_view field_name, std::unique_ptr<ValueExpr> expr);

  static ::zetasql_base::StatusOr<std::unique_ptr<FieldValueExpr>> Create(
      int field_index, std::unique_ptr<ValueExpr> expr);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kStruct };

  FieldValueExpr(int field_index, std::unique_ptr<ValueExpr> expr);

  int field_index() const { return field_index_; }
  std::string field_name() const { return field_name_; }

  const ValueExpr* input() const;
  ValueExpr* mutable_input();

  std::string field_name_;
  int field_index_;
};

// Class that actually reads fields from a proto, as specified by a
// ProtoFieldAccessInfo from a ProtoFieldRegistry.
class ProtoFieldReader {
 public:
  // 'id' is a unique identifier of this instance that is used for debug
  // logging. Does not take ownership of 'registry'.
  ProtoFieldReader(int id, const ProtoFieldAccessInfo& access_info,
                   ProtoFieldRegistry* registry)
      : id_(id),
        access_info_(access_info),
        registry_(registry),
        access_info_registry_id_(registry->RegisterField(&access_info_)) {}

  ProtoFieldReader(const ProtoFieldReader&) = delete;
  ProtoFieldReader& operator=(const ProtoFieldReader&) = delete;

  // Populates 'field_value' with the appropriate field value from
  // 'proto_slot', which must have a proto Value. If the field values
  // (corresponding to 'registry_') have not been stored in the
  // ProtoFieldValueList in 'proto_slot', reads all of them. If
  // EvaluationOptions::store_proto_field_value_maps is true, also stores
  // them in 'proto_slot'. On failure, returns false and populates
  // 'status'. (This method does not return ::zetasql_base::StatusOr<Value> for
  // performance reasons.)
  bool GetFieldValue(const TupleSlot& proto_slot, EvaluationContext* context,
                     Value* field_value, ::zetasql_base::Status* status) const;

  const ProtoFieldAccessInfo& access_info() const { return access_info_; }

  int id() const { return id_; }

  int registry_id() const { return registry_->id(); }

 private:
  const int id_;
  const ProtoFieldAccessInfo access_info_;
  const ProtoFieldRegistry* registry_;
  const int access_info_registry_id_;
};

// Returns the Value corresponding to a specific field of a proto-valued
// expression.
//
// There are various optimizations at work here:
//
// 1) Reading a field from a proto can require scanning all of the (tag, value)
//    pairs in the wire format. To avoid doing that twice for two fields of the
//    same proto, we store a FieldRegistry object corresponding to the proto,
//    which is shared by all of the GetProtoFieldExprs that extract fields from
//    that proto. During algebrization, the FieldRegistry is configured with all
//    of the ProtoFieldAccessInfos that the query needs. The first time
//    GetProtoFieldExpr::Eval() is called for one of those fields, it parses all
//    of the registered fields, and stores them in the input TupleSlot for use
//    by other GetProtoFieldExpr::Eval() method calls.
//
// 2) It's possible that the result of GetProtoFieldExpr could be needed by
//    another ValueExpr, or even another GetProtoFieldExpr. For example,
//    consider this query:
//
//        SELECT proto_column.a,
//               proto_column.a.b,
//               proto_column.a.c,
//               proto_column.z
//        FROM Table
//
//    Ideally we would like to compute proto_column.a only once, and at the same
//    time as proto_column.z. The registry described above partially solves this
//    problem, but it does not allow for the GetProtoFieldExpr node for
//    proto_column.a to be used as the proto-valued expr for the nodes
//    corresponding to both proto_column.a.b and proto_column.a.c, as well as
//    for the standalone proto_column.a expression, because of pointer ownership
//    issues (algebrized tree nodes own their input nodes). The solution here is
//    to make GetProtoFieldExpr a wrapper around a shared ProtoFieldReader.
//
// Here is an example of how these classes are arranged for the expression
// proto_column.a.b in the above query:
//
// GetProtoFieldExpr
// - GetProtoFieldExpr (corresponding to proto_column.a)
//   - ValueExpr (corresponding to proto_column)
//   - ProtoFieldReader (corresponding to a, shared with all the other
//                       GetProtoFieldExpr nodes for proto_column.a).
//     - ProtoFieldAccessInfo (corresponding to a)
//     - FieldRegistry (shared with the GetProtoFieldExpr node for
//                      proto_column.z)
// - ProtoFieldReader (corresponding to b)
//    - ProtoFieldAccessInfo (corresponding to b)
//    - FieldRegistry (shared with the GetProtoFieldExpr node for
//                     proto_column.a.c)
//
// In the example above, the ValueExpr correspoding to proto_column returns the
// Value from a row, and one of the ProtoFieldReaders stores information
// alongside it corresponding to all of the fields in the corresponding
// FieldRegistry.
class GetProtoFieldExpr : public ValueExpr {
 public:
  GetProtoFieldExpr(const GetProtoFieldExpr&) = delete;
  GetProtoFieldExpr& operator=(const GetProtoFieldExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<GetProtoFieldExpr>> Create(
      std::unique_ptr<ValueExpr> proto_expr,
      const ProtoFieldReader* field_reader);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  const ProtoFieldReader* field_reader() const { return field_reader_; }

 private:
  enum ArgKind { kProtoExpr };

  // Does not take ownership of 'field_reader'.
  GetProtoFieldExpr(std::unique_ptr<ValueExpr> proto_expr,
                    const ProtoFieldReader* field_reader);

  // Returns the ValueExpr passed to the constructor.
  const ValueExpr* proto_expr() const;
  ValueExpr* mutable_proto_expr();

  const ProtoFieldReader* field_reader_;
};

// Nests 'element's of 'input' as an array. 'is_with_table' is true if this
// operator corresponds to the table in a WITH clause (which affects the memory
// bound that is applied to the output).
class ArrayNestExpr : public ValueExpr {
 public:
  ArrayNestExpr(const ArrayNestExpr&) = delete;
  ArrayNestExpr& operator=(const ArrayNestExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<ArrayNestExpr>> Create(
      const ArrayType* array_type, std::unique_ptr<ValueExpr> element,
      std::unique_ptr<RelationalOp> input, bool is_with_table);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  const ArrayType* output_type() const override {
    DCHECK(ValueExpr::output_type()->IsArray());
    return ValueExpr::output_type()->AsArray();
  }

 private:
  enum ArgKind { kElement, kInput };

  ArrayNestExpr(const ArrayType* array_type, std::unique_ptr<ValueExpr> element,
                std::unique_ptr<RelationalOp> input, bool is_with_table);

  const ValueExpr* element() const;
  ValueExpr* mutable_element();

  const RelationalOp* input() const;
  RelationalOp* mutable_input();
  const bool is_with_table_;
};

// Constructs a struct of the given 'type' and 'args'. Number and order of
// fields must match the type definition.
class NewStructExpr : public ValueExpr {
 public:
  NewStructExpr(const NewStructExpr&) = delete;
  NewStructExpr& operator=(const NewStructExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<NewStructExpr>> Create(
      const StructType* type, std::vector<std::unique_ptr<ExprArg>> args);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kField };

  NewStructExpr(const StructType* type,
                std::vector<std::unique_ptr<ExprArg>> args);

  absl::Span<const ExprArg* const> field_list() const;
  absl::Span<ExprArg* const> mutable_field_list();
};

// Constructs an array from the given 'elements'.
class NewArrayExpr : public ValueExpr {
 public:
  NewArrayExpr(const NewArrayExpr&) = delete;
  NewArrayExpr& operator=(const NewArrayExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<NewArrayExpr>> Create(
      const ArrayType* array_type,
      std::vector<std::unique_ptr<ValueExpr>> elements);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kElement };

  NewArrayExpr(const ArrayType* array_type,
               std::vector<std::unique_ptr<ValueExpr>> elements);

  absl::Span<const ExprArg* const> elements() const;
  absl::Span<ExprArg* const> mutable_elements();
};

// Produces a constant 'value'.
class ConstExpr : public ValueExpr {
 public:
  ConstExpr(const ConstExpr&) = delete;
  ConstExpr& operator=(const ConstExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<ConstExpr>> Create(
      const Value& value);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  bool IsConstant() const override { return true; }

  const Value& value() const { return slot_.value(); }

  // Provides a view of the entire internal slot. (Only for unit tests).
  const TupleSlot& slot_test_only() const { return slot_; }

 private:
  explicit ConstExpr(const Value& value);

  TupleSlot slot_;
};

// Produces a single value from the variable ranging over the given 'input'
// relation, or NULL if the 'input' is empty. Sets an error if the 'input' has
// more than one element.
class SingleValueExpr : public ValueExpr {
 public:
  SingleValueExpr(const SingleValueExpr&) = delete;
  SingleValueExpr& operator=(const SingleValueExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<SingleValueExpr>> Create(
      std::unique_ptr<ValueExpr> value, std::unique_ptr<RelationalOp> input);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kValue, kInput };

  SingleValueExpr(std::unique_ptr<ValueExpr> value,
                  std::unique_ptr<RelationalOp> input);

  const ValueExpr* value() const;
  ValueExpr* mutable_value();

  const RelationalOp* input() const;
  RelationalOp* mutable_input();
};

// Returns Bool(true) if the relation has at least one row. Otherwise returns
// Bool(false).
class ExistsExpr : public ValueExpr {
 public:
  ExistsExpr(const ExistsExpr&) = delete;
  ExistsExpr& operator=(const ExistsExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<ExistsExpr>> Create(
      std::unique_ptr<RelationalOp> input);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kInput };

  explicit ExistsExpr(std::unique_ptr<RelationalOp> input);

  const RelationalOp* input() const;
  RelationalOp* mutable_input();
};

// Defines an executable function.
class FunctionBody {
 public:
  enum Mode {
    kScalar,
    kAggregate,
    kAnalytic
  };

  FunctionBody(Mode mode,
               const Type* output_type)
      : mode_(mode),
        output_type_(output_type) {}

  FunctionBody(const FunctionBody&) = delete;
  FunctionBody& operator=(const FunctionBody&) = delete;
  virtual ~FunctionBody() {}
  virtual std::string debug_name() const = 0;
  Mode mode() const { return mode_; }
  const Type* output_type() const { return output_type_; }
 private:
  Mode mode_;
  const Type* output_type_;  // Not owned.
};

// Defines an executable scalar function.
class ScalarFunctionBody : public FunctionBody {
 public:
  explicit ScalarFunctionBody(const Type* output_type)
      : FunctionBody(kScalar /* mode */,
                     output_type) {}

  ScalarFunctionBody(const ScalarFunctionBody&) = delete;
  ScalarFunctionBody& operator=(const ScalarFunctionBody&) = delete;
  ~ScalarFunctionBody() override {}

  // Evaluates the function using 'args'. On success, populates 'result' and
  // returns true. On failure, populates 'status' and returns false. We avoid
  // returning ::zetasql_base::StatusOr<Value> for performance reasons.
  virtual bool Eval(absl::Span<const Value> args, EvaluationContext* context,
                    Value* result, ::zetasql_base::Status* status) const = 0;
};

// Accumulator interface for aggregating a bunch of values.
// Example usage:
//   std::unique_ptr<AggregateAccumulator> accumulator = ...
//   while (...) {
//     ...
//     bool stop_accumulation;
//     ::zetasql_base::Status status;
//     if (!accumulator->Accumulate(..., &stop_accumulation, &status)) {
//       return status;
//     }
//     if (stop_accumulation) break;
//   }
//   return accumulator->GetFinalResult(/*inputs_in_defined_order=*/false);
class AggregateAccumulator {
 public:
  virtual ~AggregateAccumulator() {}

  // Resets the accumulation.
  virtual ::zetasql_base::Status Reset() = 0;

  // Accumulates 'value' (which may be a struct for aggregation functions like
  // APPROX_TOP_SUM and COVAR_POP that aggregate multiple columns). On success,
  // returns true and populates 'stop_accumulation' with whether the caller may
  // skip subsequent calls to Accumulate(). On failure, returns false and
  // populates 'status'. Does not return ::zetasql_base::Status for performance reasons.
  virtual bool Accumulate(const Value& value, bool* stop_accumulation,
                          ::zetasql_base::Status* status) = 0;

  // Returns the final result of the accumulation. 'inputs_in_defined_order'
  // should be true if the order that values wered passed to Accumulate() was
  // defined by ZetaSQL semantics. The value of 'inputs_in_defined_order' is
  // only important if we are doing compliance or random query testing.
  virtual ::zetasql_base::StatusOr<Value> GetFinalResult(
      bool inputs_in_defined_order) = 0;
};

// Defines an executable aggregate function.
class AggregateFunctionBody : public FunctionBody {
 public:
  // 'num_input_fields' is the number of fields the aggregation function is fed
  // on every row. If 'num_input_fields' is 1, 'input_type' is the corresponding
  // Type. Otherwise, 'input_type' is a struct with 'num_input_fields' fields.
  AggregateFunctionBody(const Type* output_type, int num_input_fields,
                        const Type* input_type, bool ignores_null)
      : FunctionBody(kAggregate /* mode */, output_type),
        num_input_fields_(num_input_fields),
        input_type_(input_type),
        ignores_null_(ignores_null) {}

  AggregateFunctionBody(const AggregateFunctionBody&) = delete;
  AggregateFunctionBody& operator=(const AggregateFunctionBody&) = delete;
  ~AggregateFunctionBody() override {}

  bool ignores_null() const { return ignores_null_; }

  const int num_input_fields() const { return num_input_fields_; }
  const Type* input_type() const { return input_type_; }

  // 'args' contains the constant arguments for the aggregation
  // function (e.g., the delimeter for STRING_AGG).
  virtual ::zetasql_base::StatusOr<std::unique_ptr<AggregateAccumulator>>
  CreateAccumulator(absl::Span<const Value> args,
                    EvaluationContext* context) const = 0;

 private:
  const int num_input_fields_;
  const Type* input_type_;
  const bool ignores_null_;
};

// Evaluates a scalar function of the given 'function' and 'arguments'.
class ScalarFunctionCallExpr : public ValueExpr {
 public:
  static ::zetasql_base::StatusOr<std::unique_ptr<ScalarFunctionCallExpr>> Create(
      std::unique_ptr<const ScalarFunctionBody> function,
      std::vector<std::unique_ptr<ValueExpr>> exprs,
      ResolvedFunctionCallBase::ErrorMode error_mode =
          ResolvedFunctionCallBase::DEFAULT_ERROR_MODE);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kArgument };

  ScalarFunctionCallExpr(std::unique_ptr<const ScalarFunctionBody> function,
                         std::vector<std::unique_ptr<ValueExpr>> exprs,
                         ResolvedFunctionCallBase::ErrorMode error_mode);

  ScalarFunctionCallExpr(const ScalarFunctionCallExpr&) = delete;
  ScalarFunctionCallExpr& operator=(const ScalarFunctionCallExpr&) = delete;

  const ScalarFunctionBody* function() const { return function_.get(); }

  std::unique_ptr<const ScalarFunctionBody> function_;
  const ResolvedFunctionCallBase::ErrorMode error_mode_;
};

// Defines an aggregate function call with the given 'exprs' and 'arguments'.
// Note that it cannot evaluate the aggregate function, which is computed
// in AggregateArg.
class AggregateFunctionCallExpr : public ValueExpr {
 public:
  static ::zetasql_base::StatusOr<std::unique_ptr<AggregateFunctionCallExpr>> Create(
      std::unique_ptr<const AggregateFunctionBody> function,
      std::vector<std::unique_ptr<ValueExpr>> exprs);

  const AggregateFunctionBody* function() const {
    return function_.get();
  }

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kArgument };

  AggregateFunctionCallExpr(
      std::unique_ptr<const AggregateFunctionBody> function,
      std::vector<std::unique_ptr<ValueExpr>> exprs);
  AggregateFunctionCallExpr(const AggregateFunctionCallExpr&) = delete;
  AggregateFunctionCallExpr& operator=(const AggregateFunctionCallExpr&) =
      delete;

  // Not implemented. Use AggregateArg::Eval instead to evaluate an aggregate
  // function.
  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::unique_ptr<const AggregateFunctionBody> function_;
};

// Base class for (non-aggregate) analytic functions.
class AnalyticFunctionBody : public FunctionBody {
 public:
  explicit AnalyticFunctionBody(const Type* output_type)
      : FunctionBody(kAnalytic, output_type) {}

  AnalyticFunctionBody(const AnalyticFunctionBody&) = delete;
  AnalyticFunctionBody& operator=(const AnalyticFunctionBody&) = delete;
  ~AnalyticFunctionBody() override {}

  // Returns true if a tuple comparator is required for evaluation.
  virtual bool RequireTupleComparator() const = 0;

  // Evaluates the function over <tuples> (which have TupleSchema <schema>) with
  // given <arguments> and <windows>.  The returned vector <result> matches 1:1
  // with <tuples>.
  //
  // Each element in <arguments> is a vector and gives the result of an argument
  // expression. If the argument expression must be constant according to the
  // function specification, the element is a vector with a single value that is
  // equal to the constant argument value. Otherwise, the element must be
  // a vector of the same size as <tuples>, with each value being the result of
  // the argument expression on a tuple in <tuples>.
  // As an example, for the analytic function LEAD(expr, offset, default_expr),
  // 'offset' and 'default_expr' must be constant, while 'expr' is not required
  // to be constant. The <arguments> for this function has three elements.
  //   1) The first element for 'expr' has a size equal to the size of <tuples>,
  //      no matter whether 'expr' is constant or not. The n-th value of the
  //      first element is the result of evaluating 'expr' on the n-th tuple
  //      for any n within the range of <tuples>.
  //   2) The second element for 'offset' has a single value, which is the
  //      value of 'offset'.
  //   3) The third element for 'default_expr' has a single value, which is
  //      set to the constant value of 'default_expr'.
  //
  // If <windows> is not empty, then it identifies the AnalyticWindow for
  // each tuple. For example, consider the following query:
  //   SELECT FIRST_VALUE(t.x) OVER (ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING)
  //   FROM T t.
  // Suppose there are 4 tuples, then we have 4 "windows" in the format of
  // (start_tuple_id, num_tuples): (0, 2), (0, 3), (1, 3), (2, 2).
  //
  // <comparator> cannot be nullptr if RequireTupleComparator returns true.
  //
  // If <error_mode> is SAFE_ERROR_MODE, NULL values should be added to
  // <result> rather than failing for deterministic errors.
  virtual zetasql_base::Status Eval(
      const TupleSchema& schema,
      const absl::Span<const TupleData* const>& tuples,
      const absl::Span<const std::vector<Value>>& arguments,
      const absl::Span<const AnalyticWindow>& windows,
      const TupleComparator* comparator,
      ResolvedFunctionCallBase::ErrorMode error_mode,
      EvaluationContext* context, std::vector<Value>* result) const = 0;
};

// Defines a non-aggregate analytic function call expression.
class AnalyticFunctionCallExpr : public ValueExpr {
 public:
  AnalyticFunctionCallExpr(const AnalyticFunctionCallExpr&) = delete;
  AnalyticFunctionCallExpr& operator=(const AnalyticFunctionCallExpr&) = delete;

  // <const_arguments> contains the argument expressions that must be constant,
  // while other argument expressions are in <non_const_arguments>.
  static ::zetasql_base::StatusOr<std::unique_ptr<AnalyticFunctionCallExpr>> Create(
      std::unique_ptr<const AnalyticFunctionBody> function,
      std::vector<std::unique_ptr<ValueExpr>> non_const_arguments,
      std::vector<std::unique_ptr<ValueExpr>> const_arguments);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

  const AnalyticFunctionBody* function() const {
    return function_.get();
  }

  absl::Span<const ExprArg* const> non_const_arguments() const;
  absl::Span<ExprArg* const> mutable_non_const_arguments();

  absl::Span<const ExprArg* const> const_arguments() const;
  absl::Span<ExprArg* const> mutable_const_arguments();

 private:
  enum ArgKind { kNonConstArgument, kConstArgument };

  AnalyticFunctionCallExpr(
      std::unique_ptr<const AnalyticFunctionBody> function,
      std::vector<std::unique_ptr<ValueExpr>> non_const_arguments,
      std::vector<std::unique_ptr<ValueExpr>> const_arguments);

  // Not implemented. Use NonAggregateAnalyticArg::Eval instead to evaluate a
  // non-aggregate analytic function.
  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::unique_ptr<const AnalyticFunctionBody> function_;
};

// Operator backing conditional statements such as IF, CASE, and COALESCE. If
// 'condition' is true returns 'true_value', else 'false_value'. It is not a
// regular function since its true/false inputs cannot be evaluated prior to
// evaluating the operator.
class IfExpr : public ValueExpr {
 public:
  IfExpr(const IfExpr&) = delete;
  IfExpr& operator=(const IfExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<IfExpr>> Create(
      std::unique_ptr<ValueExpr> condition,
      std::unique_ptr<ValueExpr> true_value,
      std::unique_ptr<ValueExpr> false_value);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kCondition, kTrueValue, kFalseValue };

  IfExpr(std::unique_ptr<ValueExpr> condition,
         std::unique_ptr<ValueExpr> true_value,
         std::unique_ptr<ValueExpr> false_value);

  const ValueExpr* join_expr() const;
  ValueExpr* mutable_join_expr();

  const ValueExpr* true_value() const;
  ValueExpr* mutable_true_value();

  const ValueExpr* false_value() const;
  ValueExpr* mutable_false_value();
};

// Let operator creates local variables in value expressions and can be used to
// eliminate common subexpressions. It is equivalent to applying a lambda
// expression. LetExpr specifies the variables to be bound and the body to be
// evaluated. For example, expressing Coalesce via IfExpr below requires two
// occurrences of subexpression a+1:
//     Coalesce(a+1, b) = IfExpr(IsNull(a+1), b, a+1)
// In contrast, only one a+1 is used with LetExpr:
//     Coalesce(a+1, b) = LetExpr(x:=a+1, IfExpr(IsNull(x), b, x))
// Assignments can reference variables from previous assignments, e.g.,
// LetExpr(x:=expr,y:=x+1, ...).
// LetExpr can also be used to represent the WITH statement if we are
// representing the results as a Value (instead of a TupleIterator).
class LetExpr: public ValueExpr {
 public:
  LetExpr(const LetExpr&) = delete;
  LetExpr& operator=(const LetExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<LetExpr>> Create(
      std::vector<std::unique_ptr<ExprArg>> assign,
      std::unique_ptr<ValueExpr> body);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kAssign, kBody };

  LetExpr(std::vector<std::unique_ptr<ExprArg>> assign,
          std::unique_ptr<ValueExpr> body);

  absl::Span<const ExprArg* const> assign() const;
  absl::Span<ExprArg* const> mutable_assign();

  const ValueExpr* body() const;
  ValueExpr* mutable_body();
};

// Maps all ResolvedScan descendants of a node (except those that are also
// descendants of a ResolvedExpr or another ResolvedScan) to their corresponding
// RelationalOps.
using ResolvedScanMap =
    std::unordered_map<const ResolvedScan*, std::unique_ptr<RelationalOp>>;

// Maps all ResolvedExpr descendants of a node (except those that are also
// descendants of a ResolvedScan or another ResolvedExpr) to their corresponding
// ValueExprs.
using ResolvedExprMap =
    std::unordered_map<const ResolvedExpr*, std::unique_ptr<ValueExpr>>;

// This abstract class is a hack to allow executing a resolved DML statement
// directly off its resolved AST node without using an intermediate algebra.
//
// Note that the DML support in the reference implementation (implemented by
// subclasses of this class) is only for compliance and random query tests and
// not for any production use case such as the evaluator
// PreparedExpression/PreparedQuery APIs. Thus, this code can have poor cpu and
// memory usage, and it does not enforce any memory bounds specified in the
// EvaluationOptions.
class DMLValueExpr : public ValueExpr {
 public:
  DMLValueExpr(const DMLValueExpr&) = delete;
  DMLValueExpr& operator=(const DMLValueExpr&) = delete;
  ~DMLValueExpr() override {}

  // Returns true if the table being modified by the corresponding DML statement
  // is a value table.
  bool is_value_table() const { return is_value_table_; }

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> schemas) override = 0;

  // The returned value is a struct with two fields: an int64_t representing the
  // number of rows modified by the statement, and an array of structs, where
  // each element of the array represents a row of the modified table.
  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override {
    auto status_or_result = Eval(params, context);
    if (!status_or_result.ok()) {
      *status = status_or_result.status();
      return false;
    }
    result->SetValue(std::move(status_or_result).ValueOrDie());
    return true;
  }

  // More convenient form of the above, since performance doesn't matter for DML
  // because it is just for compliance testing.
  virtual ::zetasql_base::StatusOr<Value> Eval(
      absl::Span<const TupleData* const> params,
      EvaluationContext* context) const = 0;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 protected:
  // Wraps a row with its corresponding (unique) row number in the table.
  struct RowNumberAndValues {
    int64_t row_number = -1;
    std::vector<Value> values;
  };

  using ValueHasher = absl::Hash<Value>;

  // Maps the primary key value for a row (as defined by
  // GetPrimaryKeyOrRowNumber()) to its corresponding row number and values.
  //
  // Note that using Value::operator==() is ok here because it implements the
  // notion of equality used by GROUP BY (e.g., NULL == NULL). The compliance
  // test framework (specifically, SQLTestBase::ValidateFirstColumnPrimaryKey())
  // enforces that the primary key supports GROUP BY with the language options
  // that are in use.
  using PrimaryKeyRowMap =
      std::unordered_map<Value, RowNumberAndValues, ValueHasher>;

  DMLValueExpr(
      bool is_value_table, const ArrayType* table_array_type,
      const StructType* dml_output_type, const ResolvedNode* resolved_node,
      const ResolvedColumnList* column_list,
      std::unique_ptr<const ColumnToVariableMapping> column_to_variable_mapping,
      std::unique_ptr<const ResolvedScanMap> resolved_scan_map,
      std::unique_ptr<const ResolvedExprMap> resolved_expr_map);

  // RET_CHECKs that 'resolved_scan' is in 'resolved_scan_map_', and then
  // returns the corresponding RelationalOp.
  ::zetasql_base::StatusOr<RelationalOp*> LookupResolvedScan(
      const ResolvedScan* resolved_scan) const;

  // RET_CHECKs that 'resolved_expr' is in 'resolved_expr_map_', and then
  // returns the corresponding ValueExpr.
  ::zetasql_base::StatusOr<ValueExpr*> LookupResolvedExpr(
      const ResolvedExpr* resolved_expr) const;

  // Returns a zetasql_base::Status corresponding to whether 'actual_num_rows_modified'
  // matches 'assert_rows_modified' (which may be NULL, which results in
  // success). Note that if 'assert_rows_modified' is not NULL, then the
  // ResolvedExpr inside it must not be NULL, and it is a runtime error if that
  // expression evaluates to a NULL value. If 'print_array_elements' is true,
  // then any error message refers to "array elements" being modified instead of
  // "rows" (useful for when ASSERT_ROWS_MODIFIED is used with nested DML).
  ::zetasql_base::Status VerifyNumRowsModified(
      const ResolvedAssertRowsModified* assert_rows_modified,
      absl::Span<const TupleData* const> params, int64_t actual_num_rows_modified,
      EvaluationContext* context, bool print_array_elements = false) const;

  // Returns a vector of Values corresponding to 't'. The elements of the
  // returned vector correspond to 'column_list'.
  ::zetasql_base::StatusOr<std::vector<Value>> GetScannedTupleAsColumnValues(
      const ResolvedColumnList& column_list, const Tuple& t) const;

  // Returns the value of 'column' in 't'.
  ::zetasql_base::StatusOr<Value> GetColumnValue(const ResolvedColumn& column,
                                         const Tuple& t) const;

  // Populates 'row_map' according to 'original_rows'. If the table does not
  // have a primary key, uses the row number instead. Also sets
  // 'has_primary_key' to true if the table has a primary key. If a duplicate
  // primary key is found, the return value will have error code
  // 'duplicate_primary_key_error_code' and error message
  // 'duplicate_primary_key_error_prefix' + ' (' + <primary_key_value> + ')'.
  ::zetasql_base::Status PopulatePrimaryKeyRowMap(
      const std::vector<std::vector<Value>>& original_rows,
      zetasql_base::StatusCode duplicate_primary_key_error_code,
      absl::string_view duplicate_primary_key_error_prefix,
      EvaluationContext* context, PrimaryKeyRowMap* row_map,
      bool* has_primary_key) const;

  // Returns the primary key corresponding to 'row'. If the table does not have
  // a primary key, we use the row number as the primary key. If
  // 'has_primary_key' is non-NULL, sets it to true if the table has a primary
  // key.
  ::zetasql_base::StatusOr<Value> GetPrimaryKeyOrRowNumber(
      const RowNumberAndValues& row_number_and_values,
      EvaluationContext* context, bool* has_primary_key = nullptr) const;

  // Returns the index of the primary column in 'column_list_', if there is one.
  ::zetasql_base::StatusOr<absl::optional<int>> GetPrimaryKeyColumnIndex(
      EvaluationContext* context) const;

  // Returns the output of Eval(), which has type 'dml_output_type_',
  // corresponding to the input arguments.
  //
  // The returned value is a struct with two fields: an int64_t representing the
  // number of rows modified by the statement, and an array of structs, where
  // each element of the array represents a row of the modified table.
  ::zetasql_base::StatusOr<Value> GetDMLOutputValue(
      int64_t num_rows_modified,
      const std::vector<std::vector<Value>>& dml_output_rows,
      EvaluationContext* context) const;

  const bool is_value_table_;
  const ArrayType* table_array_type_;
  const StructType* dml_output_type_;
  const ResolvedNode* resolved_node_;
  const ResolvedColumnList* column_list_;
  const std::unique_ptr<const ColumnToVariableMapping>
      column_to_variable_mapping_;
  const std::unique_ptr<const ResolvedScanMap> resolved_scan_map_;
  const std::unique_ptr<const ResolvedExprMap> resolved_expr_map_;
};

// Represents a DML DELETE statement.
class DMLDeleteValueExpr : public DMLValueExpr {
 public:
  DMLDeleteValueExpr(const DMLDeleteValueExpr&) = delete;
  DMLDeleteValueExpr& operator=(const DMLDeleteValueExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<DMLDeleteValueExpr>> Create(
      bool is_value_table, const ArrayType* table_array_type,
      const StructType* dml_output_type,
      const ResolvedDeleteStmt* resolved_node,
      const ResolvedColumnList* column_list,
      std::unique_ptr<const ColumnToVariableMapping> column_to_variable_mapping,
      std::unique_ptr<const ResolvedScanMap> resolved_scan_map,
      std::unique_ptr<const ResolvedExprMap> resolved_expr_map);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                               EvaluationContext* context) const override;

 private:
  DMLDeleteValueExpr(
      bool is_value_table, const ArrayType* table_array_type,
      const StructType* dml_output_type,
      const ResolvedDeleteStmt* resolved_node,
      const ResolvedColumnList* column_list,
      std::unique_ptr<const ColumnToVariableMapping> column_to_variable_mapping,
      std::unique_ptr<const ResolvedScanMap> resolved_scan_map,
      std::unique_ptr<const ResolvedExprMap> resolved_expr_map);

  const ResolvedDeleteStmt* stmt() const {
    return resolved_node_->GetAs<ResolvedDeleteStmt>();
  }
};

// Represents a DML UPDATE statement.
class DMLUpdateValueExpr : public DMLValueExpr {
 public:
  DMLUpdateValueExpr(const DMLUpdateValueExpr&) = delete;
  DMLUpdateValueExpr& operator=(const DMLUpdateValueExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<DMLUpdateValueExpr>> Create(
      bool is_value_table, const ArrayType* table_array_type,
      const StructType* dml_output_type,
      const ResolvedUpdateStmt* resolved_node,
      const ResolvedColumnList* column_list,
      std::unique_ptr<const ColumnToVariableMapping> column_to_variable_mapping,
      std::unique_ptr<const ResolvedScanMap> resolved_scan_map,
      std::unique_ptr<const ResolvedExprMap> resolved_expr_map);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                               EvaluationContext* context) const override;

 private:
  // Represents a non-column component of an update path. E.g., for an update
  // target a.b.c[1], the column is "a" and the UpdatePathComponents are ".b",
  // ".c", and "[1]".
  class UpdatePathComponent {
   public:
    enum class Kind { PROTO_FIELD, STRUCT_FIELD, ARRAY_OFFSET };

    struct Less {
      bool operator()(const UpdatePathComponent& c1,
                      const UpdatePathComponent& c2) const {
        // Comparing 'kind_' is arguably not needed, because we should never be
        // comparing UpdatePathComponents of different kinds. We include it
        // for completeness.
        if (c1.kind_ != c2.kind_) return c1.kind_ < c2.kind_;
        return c1.component_ < c2.component_;
      }
    };

    static std::string GetKindString(Kind kind) {
      switch (kind) {
        case Kind::PROTO_FIELD:
          return "PROTO_FIELD";
        case Kind::STRUCT_FIELD:
          return "STRUCT_FIELD";
        case Kind::ARRAY_OFFSET:
          return "ARRAY_OFFSET";
      }
    }

    explicit UpdatePathComponent(
        const google::protobuf::FieldDescriptor* field_descriptor)
        : kind_(Kind::PROTO_FIELD), component_(field_descriptor) {}

    explicit UpdatePathComponent(bool is_struct_field_index, int64_t int_value)
        : kind_(is_struct_field_index ? Kind::STRUCT_FIELD
                                      : Kind::ARRAY_OFFSET),
          component_(int_value) {}

    Kind kind() const { return kind_; }

    // Must only be called if 'kind()' is PROTO_FIELD.
    const google::protobuf::FieldDescriptor* proto_field_descriptor() const {
      return *absl::get_if<const google::protobuf::FieldDescriptor*>(&component_);
    }

    // Must only be called if 'kind()' is STRUCT_FIELD.
    int64_t struct_field_index() const {
      return *absl::get_if<int64_t>(&component_);
    }

    // Must only be called if 'kind()' is ARRAY_OFFSET.
    int64_t array_offset() const { return *absl::get_if<int64_t>(&component_); }

   private:
    Kind kind_;
    // Stores a FieldDescriptor for PROTO_FIELD or an int64_t for the other Kinds.
    absl::variant<const google::protobuf::FieldDescriptor*, int64_t> component_;

    // Allow copy/move/assign.
  };

  // Represents an update to a column (or field, subfield, or array element)
  // C. There are two kinds of UpdateNodes:
  // - "Internal" nodes have children UpdateNodes, each of which is associated
  //   with a field, subfield, or array element of C.
  // - "Leaf" nodes specify a new value for C.
  //
  // For example, the UpdateNode corresponding to "a" for
  //   SET a.b = 5, a.c = 6
  // is:
  //   InternalNode(a)
  //   +-LeafNode(b)
  //   | +-value: 5
  //   +-LeafNode(c)
  //     +-value: 6
  class UpdateNode {
   public:
    UpdateNode(const UpdateNode&) = delete;
    UpdateNode& operator=(const UpdateNode&) = delete;

    // For internal nodes, maps an UpdatePathComponent to its corresponding
    // child node. All values in this map must have the same
    // UpdatePathComponent::Kind.
    //
    // TODO: Consider changing to a hash map when absl::variant (which
    // is used by UpdatePathComponent) has a hash function implemented.
    using ChildMap = std::map<UpdatePathComponent, std::unique_ptr<UpdateNode>,
                              UpdatePathComponent::Less>;

    // Creates a new UpdateNode. The kind (leaf or internal) of an UpdateItem
    // never changes over its lifetime.
    explicit UpdateNode(bool is_leaf) {
      if (is_leaf) {
        contents_ = Value();
      } else {
        contents_ = ChildMap();
      }
    }

    // Returns true if this is a leaf node, false if this is an internal node.
    bool is_leaf() const { return absl::holds_alternative<Value>(contents_); }

    // Returns the value for a leaf node. Must only be called if 'is_leaf()'
    // returns true.
    const Value& leaf_value() const { return *absl::get_if<Value>(&contents_); }
    Value* mutable_leaf_value() { return absl::get_if<Value>(&contents_); }

    // Returns the children of an internal node. Must only be called if
    // 'is_leaf()' returns false.
    const ChildMap& child_map() const {
      return *absl::get_if<ChildMap>(&contents_);
    }
    ChildMap* mutable_child_map() { return absl::get_if<ChildMap>(&contents_); }

    // Returns the new value obtained by modifying 'original_value' according to
    // the update information represented by this object.
    ::zetasql_base::StatusOr<Value> GetNewValue(const Value& original_value) const;

   private:
    // Same as GetNewValue(), but specifically for an UpdateNode that represents
    // a proto.
    ::zetasql_base::StatusOr<Value> GetNewProtoValue(const Value& original_value) const;

    absl::variant<Value, ChildMap> contents_;
  };

  // Represents the new value for an element in an array that has been modified
  // by a nested DML statement. There are three kinds of NewElements:
  // - UNMODIFIED elements have not been modified by nested DML.
  // - DELETED elements have been deleted.
  // - MODIFIED elements have been updated or inserted.
  class UpdatedElement {
   public:
    enum class Kind { UNMODIFIED, DELETED, MODIFIED };

    // Creates an UNMODIFIED UpdatedElement.
    UpdatedElement() {}

    // Returns the Kind of this UpdatedElement.
    Kind kind() const {
      if (!new_value_.has_value()) return Kind::UNMODIFIED;
      if (!new_value_.value().has_value()) return Kind::DELETED;
      return Kind::MODIFIED;
    }

    // Changes 'kind()' to DELETED.
    void delete_value() { new_value_ = absl::optional<Value>(); }

    // Returns the new value for this UpdatedElement. Must only be called if
    // 'kind()' returns MODIFIED.
    const Value& new_value() const { return new_value_.value().value(); }

    // Sets a new value for this UpdatedElement (and changes 'kind()' to
    // MODIFIED).
    void set_new_value(const Value& new_value) {
      new_value_ = absl::make_optional(new_value);
    }

   private:
    // If the outer absl::optional has no value, the element is UNMODIFIED. If
    // the inner absl::optional has no value, the element is DELETED. Otherwise
    // the element is MODIFIED.
    absl::optional<absl::optional<Value>> new_value_;

    // Allow copy/move/assign.
  };

  // Maps a ResolvedColumn modified by 'stmt()' to its corresponding UpdateNode.
  using UpdateMap =
      std::unordered_map<ResolvedColumn, std::unique_ptr<UpdateNode>,
                         ResolvedColumnHasher>;

  DMLUpdateValueExpr(
      bool is_value_table, const ArrayType* table_array_type,
      const StructType* dml_output_type,
      const ResolvedUpdateStmt* resolved_node,
      const ResolvedColumnList* column_list,
      std::unique_ptr<const ColumnToVariableMapping> column_to_variable_mapping,
      std::unique_ptr<const ResolvedScanMap> resolved_scan_map,
      std::unique_ptr<const ResolvedExprMap> resolved_expr_map);

  const ResolvedUpdateStmt* stmt() const {
    return resolved_node_->GetAs<ResolvedUpdateStmt>();
  }

  // Helper method of SetSchemasForEvaluationOfUpdateItem() that propagates
  // 'params_schemas' to all of the RelationalOps and ValueExprs in
  // 'update_item'.
  ::zetasql_base::Status SetSchemasForEvaluationOfUpdateItem(
      const ResolvedUpdateItem* update_item,
      absl::Span<const TupleSchema* const> params_schemas);

  // Helper method of SetSchemasForEvaluationOfUpdateItem() that propagates
  // 'params_schemas' to all of the RelationalOps and ValueExprs in
  // 'nested_delete'.
  ::zetasql_base::Status SetSchemasForEvaluationOfNestedDelete(
      const ResolvedDeleteStmt* nested_delete,
      const ResolvedColumn& element_column,
      absl::Span<const TupleSchema* const> params_schemas);

  // Helper method of SetSchemasForEvaluationOfUpdateItem() that propagates
  // 'params_schemas' to all of the RelationalOps and ValueExprs in
  // 'nested_update'.
  ::zetasql_base::Status SetSchemasForEvaluationOfNestedUpdate(
      const ResolvedUpdateStmt* nested_update,
      const ResolvedColumn& element_column,
      absl::Span<const TupleSchema* const> params_schemas);

  // Helper method of SetSchemasForEvaluationOfUpdateItem() that propagates
  // 'params_schemas' to all of the RelationalOps and ValueExprs in
  // 'nested_insert'.
  ::zetasql_base::Status SetSchemasForEvaluationOfNestedInsert(
      const ResolvedInsertStmt* nested_insert,
      absl::Span<const TupleSchema* const> params_schemas);

  // Computes the (at most one) concatenated tuple obtained from 'params',
  // 'left_tuple' (which must not be NULL) and one of the entries in
  // 'right_tuples' for which 'where_expr' is true, and returns it in
  // 'joined_tuple_datas'.  More precisely,
  // - If 'right_tuples' is NULL, either sets 'joined_tuple_datas' to empty or
  //   sets 'joined_tuple_data' to correspond to 'params' and ;left_tuple',
  //   depending on whether 'where_expr->Eval(ConcatSpans(params,
  //   {&left_tuple}))' is true.
  // - If 'right_tuples' is non-NULL, materializes all the concatenated
  //   tuples of the form obtained from 'params', 'left_tuple' and one of the
  //   'right_tuples' and evaluates 'where_expr' on each of them.
  //     - If 'where_expr->Eval()' returns true for none of them,
  //       sets 'joined_tuple_datas' to empty.
  //     - If 'where_expr->Eval()' returns true for one of them, returns that
  //       'joined_tuple_datas'.
  //     - If 'where_expr->Eval()' returns true for more than one of them,
  //       returns an error. (This case corresponds to an otherwise
  //       non-deterministic UPDATE with join.)
  // The pointers in 'joined_tuple_datas' are only valid for the lifetime of
  // 'params', 'left_tuple', and 'right_tuples'.
  ::zetasql_base::Status GetJoinedTupleDatas(
      absl::Span<const TupleData* const> params, const TupleData* left_tuple,
      const std::vector<std::unique_ptr<TupleData>>* right_tuples,
      const ValueExpr* where_expr, EvaluationContext* context,
      std::vector<const TupleData*>* joined_tuple_datas) const;

  // Modifies 'update_map' to incorporate the modifications represented by
  // 'update_item' for the row represented by 'tuples_for_row'. Also populates
  // 'update_target_column' with the ResolvedColumn leaf of
  // 'update_item->target()'.
  //
  // If 'update_item' is a top-level update item (i.e., it is not a child of a
  // ResolvedArrayUpdateItem), then 'prefix_components' must be initially empty
  // and the initial value of 'update_column' is ignored, but is set to the
  // corresponding top-level update column (the column being modified by the
  // corresponding top-level update item). Otherwise, 'update_column' must point
  // to the corresponding top-level update column and 'update_item->target()' is
  // relative to 'prefix_components' (e.g., if 'update_item' corresponds to
  // a[0].b, then 'prefix_components' is "a[0]" and 'update_item->target)' is
  // ".b". In either case, this method restores 'prefix_components' to its
  // initial value prior to return.
  ::zetasql_base::Status AddToUpdateMap(
      const ResolvedUpdateItem* update_item,
      absl::Span<const TupleData* const> tuples_for_row,
      EvaluationContext* context, ResolvedColumn* update_column,
      ResolvedColumn* update_target_column,
      std::vector<UpdatePathComponent>* prefix_components,
      UpdateMap* update_map) const;

  // Populates 'column' and 'components' (deepest node first) according to
  // 'update_target'. For example, if 'update_target' represents a.b.c, then
  // 'column' is set to 'a' and 'components' is set to ['b', 'c'].
  ::zetasql_base::Status PopulateUpdatePathComponents(
      const ResolvedExpr* update_target, ResolvedColumn* column,
      std::vector<UpdatePathComponent>* components) const;

  // Updates 'update_node' to reflect that the path corresponding to
  // ['start_component', 'end_component') is being set to 'leaf_value'.
  ::zetasql_base::Status AddToUpdateNode(
      std::vector<UpdatePathComponent>::const_iterator start_component,
      std::vector<UpdatePathComponent>::const_iterator end_component,
      const Value& leaf_value, UpdateNode* update_node) const;

  // Returns the Value to store in the leaf UpdateNode corresponding to
  // 'update_item' (which must not have any ResolvedArrayUpdateItem children)
  // for the variables given by 'tuples_for_row'.
  ::zetasql_base::StatusOr<Value> GetLeafValue(
      const ResolvedUpdateItem* update_item,
      absl::Span<const TupleData* const> tuples_for_row,
      EvaluationContext* context) const;

  // Returns the DML output row corresponding to the input row represented by
  // 'tuple' and 'update_map'.
  ::zetasql_base::StatusOr<std::vector<Value>> GetDMLOutputRow(
      const Tuple& tuple, const UpdateMap& update_map,
      EvaluationContext* context) const;

  // Applies 'nested_delete' to a vector whose values were originally
  // 'original_elements' in a row corresponding to 'tuples_for_row', but have
  // been modified to 'new_elements' (which must have the same size as
  // 'original_values') based on previous nested delete
  // statements. 'element_column' represents the array element being modified
  // (from the enclosing ResolvedUpdateItem). Updates 'new_elements' to reflect
  // any additional deletions.
  ::zetasql_base::Status ProcessNestedDelete(
      const ResolvedDeleteStmt* nested_delete,
      absl::Span<const TupleData* const> tuples_for_row,
      const ResolvedColumn& element_column,
      const std::vector<Value>& original_elements, EvaluationContext* context,
      std::vector<UpdatedElement>* new_elements) const;

  // Applies 'nested_update' to a vector whose values were originally
  // 'original_elements' in a row corresponding to 'tuples_for_row', but have
  // been modified to 'new_elements' (which must have the same size as
  // 'original_values') based on previous nested delete and update
  // statements. 'element_column' represents the array element being modified
  // (from the enclosing ResolvedUpdateItem). Updates 'new_elements' to reflect
  // any additional modifications.
  ::zetasql_base::Status ProcessNestedUpdate(
      const ResolvedUpdateStmt* nested_update,
      absl::Span<const TupleData* const> tuples_for_row,
      const ResolvedColumn& element_column,
      const std::vector<Value>& original_elements, EvaluationContext* context,
      std::vector<UpdatedElement>* new_elements) const;

  // Applies 'nested_insert' to a vector whose values were originally
  // 'original_elements' in a row corresponding to 'tuples_for_row', but have
  // been modified to 'new_elements' based on previous nested DML
  // statements. Adds to 'new_elements' to reflect any new elements to insert.
  ::zetasql_base::Status ProcessNestedInsert(
      const ResolvedInsertStmt* nested_insert,
      absl::Span<const TupleData* const> tuples_for_row,
      const std::vector<Value>& original_elements, EvaluationContext* context,
      std::vector<UpdatedElement>* new_elements) const;

  // Verifies that 'rows' does not contain any duplicate values of the primary
  // key (if there is one).
  ::zetasql_base::Status VerifyNoDuplicatePrimaryKeys(
      const std::vector<std::vector<Value>>& rows) const;
};

// Represents a DML INSERT statement.
class DMLInsertValueExpr : public DMLValueExpr {
 public:
  DMLInsertValueExpr(const DMLInsertValueExpr&) = delete;
  DMLInsertValueExpr& operator=(const DMLInsertValueExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<DMLInsertValueExpr>> Create(
      bool is_value_table, const ArrayType* table_array_type,
      const StructType* dml_output_type,
      const ResolvedInsertStmt* resolved_node,
      const ResolvedColumnList* column_list,
      std::unique_ptr<const ColumnToVariableMapping> column_to_variable_mapping,
      std::unique_ptr<const ResolvedScanMap> resolved_scan_map,
      std::unique_ptr<const ResolvedExprMap> resolved_expr_map);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                               EvaluationContext* context) const override;

 private:
  // Positions corresponding to an element of 'stmt()->insert_column_list()'.
  struct InsertColumnOffsets {
    // The index of the column in 'column_list_'. We avoid use of the term
    // "index" in code to avoid confusion with an index on primary keys.
    int column_offset = -1;
    // The index of the column in 'stmt()->insert_column_list()'.
    int insert_column_offset = -1;
  };

  // Stores index for information for the columns in
  // 'stmt()->insert_column_list()'.
  using InsertColumnMap =
      std::unordered_map<ResolvedColumn, InsertColumnOffsets,
                         ResolvedColumnHasher>;

  DMLInsertValueExpr(
      bool is_value_table, const ArrayType* table_array_type,
      const StructType* dml_output_type,
      const ResolvedInsertStmt* resolved_node,
      const ResolvedColumnList* column_list,
      std::unique_ptr<const ColumnToVariableMapping> column_to_variable_mapping,
      std::unique_ptr<const ResolvedScanMap> resolved_scan_map,
      std::unique_ptr<const ResolvedExprMap> resolved_expr_map);

  const ResolvedInsertStmt* stmt() const {
    return resolved_node_->GetAs<ResolvedInsertStmt>();
  }

  // Populates 'insert_column_map' with keys from 'stmt()->insert_column_list()'
  // and corresponding values as described in the comment for
  // InsertColumnIndexes.
  ::zetasql_base::Status PopulateInsertColumnMap(
      InsertColumnMap* insert_column_map) const;

  // Populates 'insert_rows' with the rows to insert.
  ::zetasql_base::Status PopulateRowsToInsert(
      const InsertColumnMap& insert_column_map,
      absl::Span<const TupleData* const> params, EvaluationContext* context,
      std::vector<std::vector<Value>>* rows_to_insert) const;

  // Populates 'columns_to_insert' with one entry per row to insert. The columns
  // of those entries correspond to insert columns.
  ::zetasql_base::Status PopulateColumnsToInsert(
      const InsertColumnMap& insert_column_map,
      absl::Span<const TupleData* const> params, EvaluationContext* context,
      std::vector<std::vector<Value>>* columns_to_insert) const;

  // Populates 'original_rows' with the rows in the table before insertion. Each
  // Value has type 'table_array_type_->element_type()'.
  ::zetasql_base::Status PopulateRowsInOriginalTable(
      absl::Span<const TupleData* const> params, EvaluationContext* context,
      std::vector<std::vector<Value>>* original_rows) const;

  // Adds the rows in 'rows_to_insert' to 'row_map' and returns the number of
  // rows modified. Handles all the various insert modes and possibly generates
  // an error if there is a primary key collision.
  ::zetasql_base::StatusOr<int64_t> InsertRows(
      const InsertColumnMap& insert_column_map,
      const std::vector<std::vector<Value>>& rows_to_insert,
      EvaluationContext* context, PrimaryKeyRowMap* row_map) const;

  // Returns the DML output value corresponding to the arguments.
  ::zetasql_base::StatusOr<Value> GetDMLOutputValue(int64_t num_rows_modified,
                                            const PrimaryKeyRowMap& row_map,
                                            EvaluationContext* context) const;
};

// -------------------------------------------------------
// Dummy root objects
// -------------------------------------------------------

// Shared objects in the tree whose ownership is managed by the dummy root node.
struct RootData {
  std::vector<std::unique_ptr<ProtoFieldRegistry>> registries;
  std::vector<std::unique_ptr<ProtoFieldReader>> field_readers;
};

// A dummy root object that wraps up a RelationalOp along with ownership of
// shared objects in the tree.
class RootOp : public RelationalOp {
 public:
  RootOp(const RootOp&) = delete;
  RootOp& operator=(const RootOp&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<RootOp>> Create(
      std::unique_ptr<RelationalOp> input, std::unique_ptr<RootData> root_data);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  ::zetasql_base::StatusOr<std::unique_ptr<TupleIterator>> CreateIterator(
      absl::Span<const TupleData* const> params, int num_extra_slots,
      EvaluationContext* context) const override;

  std::unique_ptr<TupleSchema> CreateOutputSchema() const override;

  std::string IteratorDebugString() const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kInput };

  RootOp(std::unique_ptr<RelationalOp> input,
         std::unique_ptr<RootData> root_data);

  // Returns the RelationalOp passed to the constructor.
  const RelationalOp* input() const;
  RelationalOp* mutable_input();

  std::unique_ptr<RootData> root_data_;
};

// A dummy root object that wraps up a ValueExpr along with ownership of
// shared objects in the tree.
class RootExpr : public ValueExpr {
 public:
  RootExpr(const RootExpr&) = delete;
  RootExpr& operator=(const RootExpr&) = delete;

  static ::zetasql_base::StatusOr<std::unique_ptr<RootExpr>> Create(
      std::unique_ptr<ValueExpr> value_expr,
      std::unique_ptr<RootData> root_data);

  ::zetasql_base::Status SetSchemasForEvaluation(
      absl::Span<const TupleSchema* const> params_schemas) override;

  bool Eval(absl::Span<const TupleData* const> params,
            EvaluationContext* context, VirtualTupleSlot* result,
            ::zetasql_base::Status* status) const override;

  std::string DebugInternal(const std::string& indent,
                            bool verbose) const override;

 private:
  enum ArgKind { kValueExpr };

  RootExpr(std::unique_ptr<ValueExpr> value_expr,
           std::unique_ptr<RootData> root_data);

  // Returns the ValueExpr passed to the constructor.
  const ValueExpr* value_expr() const;
  ValueExpr* mutable_value_expr();

  std::unique_ptr<RootData> root_data_;
};

}  // namespace zetasql

#endif  // ZETASQL_REFERENCE_IMPL_OPERATOR_H_
