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

#ifndef ZETASQL_PARSER_VISIT_RESULT_H_
#define ZETASQL_PARSER_VISIT_RESULT_H_

#include <functional>

namespace zetasql {

class ASTNode;
class VisitResult {
 public:
  // Indicates that no new visit actions should be performed.
  static VisitResult Empty() { return VisitResult(nullptr, nullptr); }

  // Indicates that the children of <node> should be visited next.
  static VisitResult VisitChildren(const ASTNode* node) {
    return VisitResult(node, nullptr);
  }

  // Indicates that the children of <node> should be visited next; then,
  // <continuation> should be invoked.
  static VisitResult VisitChildren(const ASTNode* node,
                                   std::function<void()> continuation) {
    return VisitResult(node, [continuation]() {
      continuation();
      return VisitResult::Empty();
    });
  }

  VisitResult() : VisitResult(nullptr, nullptr) {}
  VisitResult(const VisitResult&) = default;
  VisitResult& operator=(const VisitResult&) = default;
  VisitResult(VisitResult&&) = default;

  // Node whose children to visit; nullptr if no child visits are needed.
  const ASTNode* node_for_child_visit() const { return node_; }

  // Action to perform after all children are visited; nullptr if not needed.
  std::function<VisitResult()> continuation() const { return continuation_; }

 private:
  VisitResult(const ASTNode* node, std::function<VisitResult()> continuation)
      : node_(node), continuation_(continuation) {}

  // Node to visit the children of, null to not perform any more visits.
  // Children are visited depth-first in in the order they appear.
  const ASTNode* node_ = nullptr;

  // Function to be invoked after all children have been visited.  nullptr to
  // skip.
  std::function<VisitResult()> continuation_;
};
}  // namespace zetasql

#endif  // ZETASQL_PARSER_VISIT_RESULT_H_
