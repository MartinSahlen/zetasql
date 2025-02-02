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

#include "zetasql/parser/parse_tree.h"

#include <map>
#include <memory>

#include "zetasql/common/errors.h"
#include "zetasql/common/status_payload_utils.h"
#include "zetasql/base/testing/status_matchers.h"
#include "zetasql/parser/parse_tree_visitor.h"
#include "zetasql/parser/parser.h"
#include "zetasql/proto/internal_error_location.pb.h"
#include "zetasql/public/error_location.pb.h"
#include "zetasql/public/parse_location.h"
#include "zetasql/testdata/test_schema.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "zetasql/base/source_location.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_builder.h"
#include "zetasql/base/status_payload.h"

namespace zetasql {
namespace testing {
namespace {

TEST(StatusWithInternalErrorLocation, ByteOffset) {
  const ParseLocationPoint point =
      ParseLocationPoint::FromByteOffset("filename_7", 7);

  const zetasql_base::Status status = StatusWithInternalErrorLocation(
      ::zetasql_base::InternalErrorBuilder() << "Foo bar baz", point);
  const InternalErrorLocation location =
      internal::GetPayload<InternalErrorLocation>(status);
  EXPECT_EQ(7, location.byte_offset());
  EXPECT_EQ("filename_7", location.filename());
}

TEST(ConvertInternalErrorLocationToExternal, ByteOffset) {
  // A query that has the byte offset referenced in the error location.
  const std::string dummy_query =
      "1234567890123456789\n"
      "1234567890123456789\n";

  const ParseLocationPoint point =
      ParseLocationPoint::FromByteOffset("filename_23", 23);

  const zetasql_base::Status status = StatusWithInternalErrorLocation(
      ::zetasql_base::InternalErrorBuilder() << "Foo bar baz", point);
  const zetasql_base::Status status2 =
      ConvertInternalErrorLocationToExternal(status, dummy_query);
  EXPECT_FALSE(internal::HasPayloadWithType<InternalErrorLocation>(status2));
  const ErrorLocation external_location =
      internal::GetPayload<ErrorLocation>(status2);
  EXPECT_EQ(2, external_location.line());
  EXPECT_EQ(4, external_location.column());
  EXPECT_TRUE(external_location.has_filename());
  EXPECT_EQ("filename_23", external_location.filename());
}


TEST(ConvertInternalErrorLocationToExternal, Idempotent) {
  // A query that has the offset referenced in the error location.
  const std::string dummy_query =
      "1234567890123456789\n"
      "1234567890123456789\n";

  const ParseLocationPoint point =
      ParseLocationPoint::FromByteOffset("filename_22", 22);

  const zetasql_base::Status status = StatusWithInternalErrorLocation(
      ::zetasql_base::InternalErrorBuilder() << "Foo bar baz", point);
  const zetasql_base::Status status2 =
      ConvertInternalErrorLocationToExternal(status, dummy_query);
  const zetasql_base::Status status3 =
      ConvertInternalErrorLocationToExternal(status2, dummy_query);

  EXPECT_EQ(status2, status3);
}

TEST(ConvertInternalErrorLocationToExternal, NoPayload) {
  const zetasql_base::Status status = ::zetasql_base::InternalErrorBuilder() << "Foo bar baz";

  const zetasql_base::Status status2 =
      ConvertInternalErrorLocationToExternal(status, "abc" /* dummy query */);
  EXPECT_FALSE(internal::HasPayload(status2));
}

TEST(ConvertInternalErrorLocationToExternal, NoLocationWithExtraPayload) {
  zetasql_test::TestStatusPayload extra_extension;
  extra_extension.set_value("abc");

  const zetasql_base::Status status =
      ::zetasql_base::InternalErrorBuilder().Attach(extra_extension) << "Foo bar baz";

  const zetasql_base::Status status2 =
      ConvertInternalErrorLocationToExternal(status, "abc" /* dummy query */);
  EXPECT_FALSE(internal::HasPayloadWithType<InternalErrorLocation>(status2));
  EXPECT_FALSE(internal::HasPayloadWithType<ErrorLocation>(status2));
  EXPECT_TRUE(
      internal::HasPayloadWithType<zetasql_test::TestStatusPayload>(status2));
}

TEST(ConvertInternalErrorLocationToExternal, LocationWithExtraPayload) {
  // A query that has the line numbers referenced in the error location.
  const std::string dummy_query =
      "1234567890123456789\n"
      "1234567890123456789\n";

  zetasql_test::TestStatusPayload extra_extension;
  extra_extension.set_value("abc");

  const ParseLocationPoint point =
      ParseLocationPoint::FromByteOffset("filename_21", 21);

  const zetasql_base::Status status = StatusWithInternalErrorLocation(
      ::zetasql_base::InternalErrorBuilder().Attach(extra_extension) << "Foo bar baz",
      point);

  const zetasql_base::Status status2 =
      ConvertInternalErrorLocationToExternal(status, dummy_query);
  EXPECT_FALSE(internal::HasPayloadWithType<InternalErrorLocation>(status2));
  EXPECT_TRUE(internal::HasPayloadWithType<ErrorLocation>(status2));
  EXPECT_TRUE(
      internal::HasPayloadWithType<zetasql_test::TestStatusPayload>(status2));
}

// Return a std::string with counts of node kinds that looks like
// "Expression:3 Select:1", with names in sorted order.
static std::string CountNodeKinds(const std::vector<const ASTNode*>& nodes) {
  std::map<std::string, int> counts;
  for (const ASTNode* node : nodes) {
    ++counts[node->GetNodeKindString()];
  }
  std::string ret;
  for (const auto& item : counts) {
    if (!ret.empty()) ret += " ";
    absl::StrAppend(&ret, item.first, ":", item.second);
  }
  return ret;
}

TEST(ParseTreeTest, NodeKindCategories_IfStatement) {
  const std::string sql = "if true then select 5; end if";

  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_ASSERT_OK(ParseScript(sql, ParserOptions(), ERROR_MESSAGE_WITH_PAYLOAD,
                        &parser_output));
  auto statement_list = parser_output->script()->statement_list();
  ASSERT_EQ(statement_list.size(), 1);
  const ASTStatement* statement = statement_list[0];

  EXPECT_TRUE(statement->IsStatement());
  EXPECT_TRUE(statement->IsScriptStatement());
  EXPECT_FALSE(statement->IsLoopStatement());
  EXPECT_FALSE(statement->IsSqlStatement());
  EXPECT_FALSE(statement->IsType());
  EXPECT_FALSE(statement->IsExpression());
  EXPECT_FALSE(statement->IsQueryExpression());
  EXPECT_FALSE(statement->IsTableExpression());
}

TEST(ParseTreeTest, NodeKindCategories_LoopStatement) {
  const std::string sql = "LOOP END LOOP;";

  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_ASSERT_OK(ParseScript(sql, ParserOptions(), ERROR_MESSAGE_WITH_PAYLOAD,
                        &parser_output));
  ASSERT_EQ(parser_output->script()->statement_list().size(), 1);
  const ASTStatement* statement = parser_output->script()->statement_list()[0];

  EXPECT_TRUE(statement->IsStatement());
  EXPECT_TRUE(statement->IsScriptStatement());
  EXPECT_TRUE(statement->IsLoopStatement());
  EXPECT_FALSE(statement->IsSqlStatement());
  EXPECT_FALSE(statement->IsType());
  EXPECT_FALSE(statement->IsExpression());
  EXPECT_FALSE(statement->IsQueryExpression());
  EXPECT_FALSE(statement->IsTableExpression());
}

TEST(ParseTreeTest, NodeKindCategories_WhileStatement) {
  const std::string sql = "WHILE TRUE DO END WHILE;";

  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_ASSERT_OK(ParseScript(sql, ParserOptions(), ERROR_MESSAGE_WITH_PAYLOAD,
                        &parser_output));
  ASSERT_EQ(parser_output->script()->statement_list().size(), 1);
  const ASTStatement* statement = parser_output->script()->statement_list()[0];

  EXPECT_TRUE(statement->IsStatement());
  EXPECT_TRUE(statement->IsScriptStatement());
  EXPECT_TRUE(statement->IsLoopStatement());
  EXPECT_FALSE(statement->IsSqlStatement());
  EXPECT_FALSE(statement->IsType());
  EXPECT_FALSE(statement->IsExpression());
  EXPECT_FALSE(statement->IsQueryExpression());
  EXPECT_FALSE(statement->IsTableExpression());
}

TEST(ParseTreeTest, NodeKindCategories_QueryStatement) {
  const std::string sql = "SELECT 5";

  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_ASSERT_OK(ParseStatement(sql, ParserOptions(), &parser_output));
  const ASTStatement* statement = parser_output->statement();

  EXPECT_TRUE(statement->IsStatement());
  EXPECT_FALSE(statement->IsScriptStatement());
  EXPECT_TRUE(statement->IsSqlStatement());
  EXPECT_FALSE(statement->IsType());
  EXPECT_FALSE(statement->IsExpression());
  EXPECT_FALSE(statement->IsQueryExpression());
  EXPECT_FALSE(statement->IsTableExpression());
}

TEST(ParseTreeTest, NodeKindCategories_LiteralExpression) {
  const std::string sql = "5";

  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_ASSERT_OK(ParseExpression(sql, ParserOptions(), &parser_output));
  const ASTExpression* expr = parser_output->expression();

  EXPECT_FALSE(expr->IsStatement());
  EXPECT_FALSE(expr->IsScriptStatement());
  EXPECT_FALSE(expr->IsSqlStatement());
  EXPECT_FALSE(expr->IsType());
  EXPECT_TRUE(expr->IsExpression());
  EXPECT_FALSE(expr->IsQueryExpression());
  EXPECT_FALSE(expr->IsTableExpression());
}

TEST(ParseTreeTest, GetDescendantsWithKinds) {
  const std::string sql =
      "select * from (select 1+0x2, x+y), "
      "  (select x union all select f(x+y)+z), "
      "  (select 5+(select 6+7))";

  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_ASSERT_OK(ParseStatement(sql, ParserOptions(), &parser_output));
  const ASTStatement* statement = parser_output->statement();

  LOG(INFO) << "Parse tree:\n" << statement->DebugString();

  std::vector<const ASTNode*> found_nodes;
  // Look up empty set of node kinds.
  statement->GetDescendantSubtreesWithKinds({}, &found_nodes);
  EXPECT_EQ("", CountNodeKinds(found_nodes));
  statement->GetDescendantsWithKinds({}, &found_nodes);
  EXPECT_EQ("", CountNodeKinds(found_nodes));

  // Look up a node kind that doesn't occur.
  statement->GetDescendantSubtreesWithKinds({AST_OR_EXPR}, &found_nodes);
  EXPECT_EQ("", CountNodeKinds(found_nodes));
  statement->GetDescendantsWithKinds({AST_OR_EXPR}, &found_nodes);
  EXPECT_EQ("", CountNodeKinds(found_nodes));

  // Look up the root node.
  statement->GetDescendantSubtreesWithKinds({AST_QUERY_STATEMENT},
                                            &found_nodes);
  EXPECT_EQ("QueryStatement:1", CountNodeKinds(found_nodes));
  EXPECT_EQ(AST_QUERY_STATEMENT, statement->node_kind());
  statement->GetDescendantsWithKinds({AST_QUERY_STATEMENT}, &found_nodes);
  EXPECT_EQ("QueryStatement:1", CountNodeKinds(found_nodes));

  // Look up the outmost query node.
  statement->GetDescendantSubtreesWithKinds({AST_QUERY}, &found_nodes);
  EXPECT_EQ("Query:1", CountNodeKinds(found_nodes));
  statement->GetDescendantsWithKinds({AST_QUERY}, &found_nodes);
  EXPECT_EQ("Query:5", CountNodeKinds(found_nodes));

  // Look up one node that does occur.
  statement->GetDescendantSubtreesWithKinds({AST_SET_OPERATION},
                                            &found_nodes);
  EXPECT_EQ("SetOperation:1", CountNodeKinds(found_nodes));
  statement->GetDescendantsWithKinds({AST_SET_OPERATION}, &found_nodes);
  EXPECT_EQ("SetOperation:1", CountNodeKinds(found_nodes));

  // Look up a node that occurs multiple times.
  statement->GetDescendantSubtreesWithKinds({AST_INT_LITERAL},
                                            &found_nodes);
  EXPECT_EQ("IntLiteral:5", CountNodeKinds(found_nodes));
  statement->GetDescendantsWithKinds({AST_INT_LITERAL}, &found_nodes);
  EXPECT_EQ("IntLiteral:5", CountNodeKinds(found_nodes));

  // We find the two binary expressions in the first subquery, the outermost
  // one in the third subquery, and none under the union all.
  statement->GetDescendantSubtreesWithKinds(
      {AST_BINARY_EXPRESSION, AST_SET_OPERATION}, &found_nodes);
  EXPECT_EQ("BinaryExpression:3 SetOperation:1", CountNodeKinds(found_nodes));
  statement->GetDescendantsWithKinds(
      {AST_BINARY_EXPRESSION, AST_SET_OPERATION}, &found_nodes);
  EXPECT_EQ("BinaryExpression:6 SetOperation:1", CountNodeKinds(found_nodes));
}

class TestVisitor : public NonRecursiveParseTreeVisitor {
 public:
  explicit TestVisitor(bool post_visit) : post_visit_(post_visit) {}
  VisitResult defaultVisit(const ASTNode* node) override {
    return VisitInternal(node, "default");
  }

  // Choose an arbitrary node type to provide a specific visitor for.
  // This allows the test to verify that specific visit() methods are invoked
  // when provided, rather than the default.
  VisitResult visitASTPathExpression(const ASTPathExpression* node) override {
    return VisitInternal(node, "path_expression");
  }

  const std::string& log() const { return log_; }

  int pre_visit_count() const { return pre_visit_count_; }
  int post_visit_count() const { return post_visit_count_; }

 private:
  VisitResult VisitInternal(const ASTNode* node, const std::string& label) {
    ++pre_visit_count_;
    absl::StrAppend(&log_, "preVisit(", label,
                    "): ", node->SingleNodeDebugString(), "\n");

    if (post_visit_) {
      return VisitResult::VisitChildren(node, [node, this, label]() {
        ++post_visit_count_;
        absl::StrAppend(&log_, "postVisit(", label,
                        "): ", node->SingleNodeDebugString(), "\n");
      });
    } else {
      return VisitResult::VisitChildren(node);
    }
  }

  int pre_visit_count_ = 0;
  int post_visit_count_ = 0;
  std::string log_ = "\n";
  bool post_visit_;
};

TEST(ParseTreeTest, NonRecursiveVisitors_WithPostVisit) {
  const std::string sql = "select (1+x) from t;";

  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_ASSERT_OK(ParseStatement(sql, ParserOptions(), &parser_output));

  TestVisitor visitor(/*post_visit=*/true);
  parser_output->statement()->TraverseNonRecursive(&visitor);
  std::string expected_log = R"(
preVisit(default): QueryStatement
preVisit(default): Query
preVisit(default): Select
preVisit(default): SelectList
preVisit(default): SelectColumn
preVisit(default): BinaryExpression(+)
preVisit(default): IntLiteral(1)
postVisit(default): IntLiteral(1)
preVisit(path_expression): PathExpression
preVisit(default): Identifier(x)
postVisit(default): Identifier(x)
postVisit(path_expression): PathExpression
postVisit(default): BinaryExpression(+)
postVisit(default): SelectColumn
postVisit(default): SelectList
preVisit(default): FromClause
preVisit(default): TablePathExpression
preVisit(path_expression): PathExpression
preVisit(default): Identifier(t)
postVisit(default): Identifier(t)
postVisit(path_expression): PathExpression
postVisit(default): TablePathExpression
postVisit(default): FromClause
postVisit(default): Select
postVisit(default): Query
postVisit(default): QueryStatement
)";
  ASSERT_THAT(visitor.log(), expected_log);
  ASSERT_EQ(visitor.pre_visit_count(), visitor.post_visit_count());
  ASSERT_GT(visitor.pre_visit_count(), 0);
}

TEST(ParseTreeTest, NonRecursiveVisitors_NoPostVisit) {
  const std::string sql = "select (1+x) from t;";

  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_ASSERT_OK(ParseStatement(sql, ParserOptions(), &parser_output));

  TestVisitor visitor(/*post_visit=*/false);
  parser_output->statement()->TraverseNonRecursive(&visitor);
  std::string expected_log = R"(
preVisit(default): QueryStatement
preVisit(default): Query
preVisit(default): Select
preVisit(default): SelectList
preVisit(default): SelectColumn
preVisit(default): BinaryExpression(+)
preVisit(default): IntLiteral(1)
preVisit(path_expression): PathExpression
preVisit(default): Identifier(x)
preVisit(default): FromClause
preVisit(default): TablePathExpression
preVisit(path_expression): PathExpression
preVisit(default): Identifier(t)
)";
  ASSERT_THAT(visitor.log(), expected_log);
  ASSERT_GT(visitor.pre_visit_count(), 0);
  ASSERT_EQ(visitor.post_visit_count(), 0);
}

// Returns a copy of <s> repeated <count> times.
std::string Repeat(absl::string_view s, int count) {
  CHECK_GT(count, 0);
  return absl::StrJoin(std::vector<absl::string_view>(count, s), "");
}

}  // namespace
}  // namespace testing
}  // namespace zetasql
