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

#ifndef ZETASQL_PUBLIC_SIMPLE_CATALOG_H_
#define ZETASQL_PUBLIC_SIMPLE_CATALOG_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zetasql/base/logging.h"
#include "google/protobuf/descriptor.h"
#include "zetasql/common/simple_evaluator_table_iterator.h"
#include "zetasql/public/builtin_function.h"
#include "zetasql/public/catalog.h"
#include "zetasql/public/constant.h"
#include "zetasql/public/function.h"
#include "zetasql/public/procedure.h"
#include "zetasql/public/table_valued_function.h"
#include "zetasql/public/type.h"
#include "zetasql/public/value.h"
#include <cstdint>
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status.h"

namespace zetasql {

class SimpleCatalogProto;
class SimpleColumnProto;
class SimpleConstantProto;
class SimpleTableProto;

// SimpleCatalog is a concrete implementation of the Catalog interface.
// It acts as a simple container for objects in the Catalog.
//
// This class is thread-safe.
class SimpleCatalog : public EnumerableCatalog {
 public:
  // Construct a Catalog with catalog name <name>.
  //
  // If <type_factory> is non-NULL, it will be stored (unowned) with this
  // SimpleCatalog and used to allocate any Types needed for the Catalog.
  // If <type_factory> is NULL, an owned TypeFactory will be constructed
  // internally when needed.
  explicit SimpleCatalog(const std::string& name,
                         TypeFactory* type_factory = nullptr);
  SimpleCatalog(const SimpleCatalog&) = delete;
  SimpleCatalog& operator=(const SimpleCatalog&) = delete;

  std::string FullName() const override { return name_; }

  zetasql_base::Status GetTable(const std::string& name, const Table** table,
                        const FindOptions& options = FindOptions()) override
      LOCKS_EXCLUDED(mutex_);

  zetasql_base::Status GetModel(const std::string& name, const Model** model,
                        const FindOptions& options = FindOptions()) override
      LOCKS_EXCLUDED(mutex_);

  zetasql_base::Status GetConnection(const std::string& name, const Connection** connection,
                             const FindOptions& options) override
      LOCKS_EXCLUDED(mutex_);

  zetasql_base::Status GetFunction(const std::string& name, const Function** function,
                           const FindOptions& options = FindOptions()) override
      LOCKS_EXCLUDED(mutex_);

  zetasql_base::Status GetTableValuedFunction(
      const std::string& name, const TableValuedFunction** function,
      const FindOptions& options = FindOptions()) override
      LOCKS_EXCLUDED(mutex_);

  zetasql_base::Status GetProcedure(
      const std::string& name,
      const Procedure** procedure,
      const FindOptions& options = FindOptions()) override;

  zetasql_base::Status GetType(const std::string& name, const Type** type,
                       const FindOptions& options = FindOptions()) override
      LOCKS_EXCLUDED(mutex_);

  zetasql_base::Status GetCatalog(const std::string& name, Catalog** catalog,
                          const FindOptions& options = FindOptions()) override
      LOCKS_EXCLUDED(mutex_);

  zetasql_base::Status GetConstant(const std::string& name, const Constant** constant,
                           const FindOptions& options = FindOptions()) override
      LOCKS_EXCLUDED(mutex_);

  // For suggestions we look from the last level of <mistyped_path>:
  //  - Whether the object exists directly in sub-catalogs.
  //  - If not above, whether there is a single name that's misspelled in the
  //    current catalog.
  std::string SuggestTable(const absl::Span<const std::string>& mistyped_path) override;
  std::string SuggestFunction(
      const absl::Span<const std::string>& mistyped_path) override;
  std::string SuggestTableValuedFunction(
      const absl::Span<const std::string>& mistyped_path) override;
  std::string SuggestConstant(
      const absl::Span<const std::string>& mistyped_path) override;
  // TODO: Implement SuggestModel function.
  // TODO: Implement SuggestConnection function.

  // Add objects to the SimpleCatalog.
  // Names must be unique (case-insensitively) or the call will die.
  // Caller maintains ownership of all added objects which must outlive
  // this catalog.
  void AddTable(const std::string& name, const Table* table) LOCKS_EXCLUDED(mutex_);
  void AddTable(const Table* table) LOCKS_EXCLUDED(mutex_);

  //Alec Addition: If the table with name already exists, update name to point to table
  void AddOrReplaceTable(const std::string& name, const Table* table) LOCKS_EXCLUDED(mutex_);
  // Same as above, but take ownership of the added object.
  void AddOwnedTable(const std::string& name, std::unique_ptr<const Table> table)
      LOCKS_EXCLUDED(mutex_);
  // Same as above, but take ownership of the added object.
  void AddOwnedTable(std::unique_ptr<const Table> table) LOCKS_EXCLUDED(mutex_);

  // Consider the unique_ptr version.
  void AddOwnedTable(const std::string& name, const Table* table);
  // Consider the unique_ptr version.
  void AddOwnedTable(const Table* table) LOCKS_EXCLUDED(mutex_);

  void AddModel(const std::string& name, const Model* model) LOCKS_EXCLUDED(mutex_);
  void AddModel(const Model* model) LOCKS_EXCLUDED(mutex_);
  void AddOwnedModel(const std::string& name, std::unique_ptr<const Model> model)
      LOCKS_EXCLUDED(mutex_);
  void AddOwnedModel(std::unique_ptr<const Model> model) LOCKS_EXCLUDED(mutex_);
  // Consider the unique_ptr version.
  void AddOwnedModel(const std::string& name, const Model* model);
  // Consider the unique_ptr version.
  void AddOwnedModel(const Model* model) LOCKS_EXCLUDED(mutex_);

  void AddConnection(const std::string& name, const Connection* connection)
      LOCKS_EXCLUDED(mutex_);
  void AddConnection(const Connection* connection) LOCKS_EXCLUDED(mutex_);

  void AddType(const std::string& name, const Type* type) LOCKS_EXCLUDED(mutex_);
  // Similar to the previous, but does not take ownership of <type>.
  bool AddTypeIfNotPresent(const std::string& name, const Type* type);
  // Similar as above, but using the type's own name.
  bool AddTypeIfNotPresent(const Type* type);

  void AddCatalog(const std::string& name, Catalog* catalog) LOCKS_EXCLUDED(mutex_);
  // Add a Table, Model, Catalog, or Function using its own name.
  void AddCatalog(Catalog* catalog) LOCKS_EXCLUDED(mutex_);

  void AddOwnedCatalog(const std::string& name, std::unique_ptr<Catalog> catalog);
  // TODO: Cleanup callers and delete
  void AddOwnedCatalog(std::unique_ptr<Catalog> catalog) LOCKS_EXCLUDED(mutex_);

  // Consider the unique_ptr version.
  void AddOwnedCatalog(const std::string& name, Catalog* catalog);
  // Consider the unique_ptr version.
  void AddOwnedCatalog(Catalog* catalog) LOCKS_EXCLUDED(mutex_);

  // Add a new (owned) SimpleCatalog named <name>, and return it.
  SimpleCatalog* MakeOwnedSimpleCatalog(const std::string& name)
      LOCKS_EXCLUDED(mutex_);

  void AddFunction(const std::string& name, const Function* function)
      LOCKS_EXCLUDED(mutex_);
  void AddFunction(const Function* function) LOCKS_EXCLUDED(mutex_);

  void AddOwnedFunction(const std::string& name,
                        std::unique_ptr<const Function> function);
  void AddOwnedFunction(std::unique_ptr<const Function> function)
      LOCKS_EXCLUDED(mutex_);

  bool AddOwnedFunctionIfNotPresent(const std::string& name,
                                    std::unique_ptr<Function>* function);
  bool AddOwnedFunctionIfNotPresent(std::unique_ptr<Function>* function);

  // Consider the unique_ptr version.
  void AddOwnedFunction(const std::string& name, const Function* function);
  // Consider the unique_ptr version.
  void AddOwnedFunction(const Function* function) LOCKS_EXCLUDED(mutex_);

  // Table Valued Functions
  void AddTableValuedFunction(const std::string& name,
                              const TableValuedFunction* function)
      LOCKS_EXCLUDED(mutex_);
  void AddTableValuedFunction(const TableValuedFunction* function)
      LOCKS_EXCLUDED(mutex_);

  void AddOwnedTableValuedFunction(
      const std::string& name, std::unique_ptr<const TableValuedFunction> function);
  void AddOwnedTableValuedFunction(
      std::unique_ptr<const TableValuedFunction> function);

  bool AddOwnedTableValuedFunctionIfNotPresent(
      const std::string& name, std::unique_ptr<TableValuedFunction>* table_function);
  // Similar as above, but using the table function's own name.
  bool AddOwnedTableValuedFunctionIfNotPresent(
      std::unique_ptr<TableValuedFunction>* table_function);
  // Consider the unique_ptr version.
  void AddOwnedTableValuedFunction(const std::string& name,
                                   const TableValuedFunction* function);
  // Consider the unique_ptr version.
  void AddOwnedTableValuedFunction(const TableValuedFunction* function)
      LOCKS_EXCLUDED(mutex_);

  // Procedure
  void AddProcedure(const std::string& name, const Procedure* procedure);
  void AddProcedure(const Procedure* procedure) LOCKS_EXCLUDED(mutex_);
  void AddOwnedProcedure(const std::string& name,
                         std::unique_ptr<const Procedure> procedure);
  void AddOwnedProcedure(std::unique_ptr<const Procedure> procedure)
      LOCKS_EXCLUDED(mutex_);

  // Consider the unique_ptr version.
  void AddOwnedProcedure(const std::string& name, const Procedure* procedure);
  // Consider the unique_ptr version.
  void AddOwnedProcedure(const Procedure* procedure) LOCKS_EXCLUDED(mutex_);

  // Constant
  void AddConstant(const std::string& name, const Constant* constant);
  void AddConstant(const Constant* constant) LOCKS_EXCLUDED(mutex_);

  void AddOwnedConstant(const std::string& name,
                        std::unique_ptr<const Constant> constant);
  void AddOwnedConstant(std::unique_ptr<const Constant> constant)
      LOCKS_EXCLUDED(mutex_);

  // Consider the unique_ptr version.
  void AddOwnedConstant(const std::string& name, const Constant* constant);
  // Consider the unique_ptr version.
  void AddOwnedConstant(const Constant* constant) LOCKS_EXCLUDED(mutex_);

  // Add ZetaSQL built-in function definitions into this catalog.
  // <options> can be used to select which functions get loaded.
  // See builtin_function.h. Provided such functions are specified in <options>
  // this can add functions in both the global namespace and more specific
  // namespaces. If any of the selected functions are in namespaces,
  // sub-Catalogs will be created and the appropriate functions will be added in
  // those sub-Catalogs.
  // Also: Functions and Catalogs with the same names must not already exist.
  void AddZetaSQLFunctions(const ZetaSQLBuiltinFunctionOptions& options =
                                 ZetaSQLBuiltinFunctionOptions())
      LOCKS_EXCLUDED(mutex_);

  // Set the google::protobuf::DescriptorPool to use when resolving Types.
  // All message and enum types declared in <pool> will be resolvable with
  // FindType or GetType, treating the full name as one identifier.
  // These type name lookups will be case sensitive.
  //
  // If overlapping names are registered using AddType, those names will
  // take precedence.  There is no check for ambiguous names, and currently
  // there is no mechanism to return an ambiguous type name error.
  //
  // The DescriptorPool can only be set once and cannot be changed.
  // Any types returned from <pool> must stay live for the lifetime of
  // this SimpleCatalog.
  //
  void SetDescriptorPool(const google::protobuf::DescriptorPool* pool)
      LOCKS_EXCLUDED(mutex_);
  void SetOwnedDescriptorPool(const google::protobuf::DescriptorPool* pool)
      LOCKS_EXCLUDED(mutex_);

  // Clear the set of functions stored in this Catalog and any subcatalogs
  // created for zetasql namespaces. Does not affect any other catalogs.
  // This can be called between calls to AddZetaSQLFunctions with different
  // options.
  void ClearFunctions() LOCKS_EXCLUDED(mutex_);

  // Clear the set of table-valued functions stored in this Catalog and any
  // subcatalogs created for zetasql namespaces. Does not affect any other
  // catalogs.
  void ClearTableValuedFunctions() LOCKS_EXCLUDED(mutex_);

  // Deserialize SimpleCatalog from proto. Types will be deserialized using
  // the TypeFactory owned by this catalog and given Descriptors from the
  // given DescriptorPools. The DescriptorPools should have been created by
  // type serialization, and all proto types in the catalog are treated as
  // references into these pools. The DescriptorPools must both outlive the
  // result SimpleCatalog.
  static zetasql_base::Status Deserialize(
      const SimpleCatalogProto& proto,
      const std::vector<const google::protobuf::DescriptorPool*>& pools,
      std::unique_ptr<SimpleCatalog>* result);

  // Serialize the SimpleCatalog to proto, optionally ignoring built-in
  // functions and recursive subcatalogs. file_descriptor_set_map is used
  // to store serialized FileDescriptorSets, which can be deserialized into
  // separate DescriptorPools in order to reconstruct the Type. The map may
  // be non-empty and may be used across calls to this method in order to
  // serialize multiple types. The map may NOT be null.
  // NOTE: recursion detection is done with seen catalogs pointers, which
  // may effectively detect multiple-step recursions, but also recognize
  // siblings pointing to the same catalog object as false positives.
  zetasql_base::Status Serialize(FileDescriptorSetMap* file_descriptor_set_map,
                         SimpleCatalogProto* proto, bool ignore_builtin = true,
                         bool ignore_recursive = true) const
      LOCKS_EXCLUDED(mutex_);

  // Return a TypeFactory owned by this SimpleCatalog.
  TypeFactory* type_factory() LOCKS_EXCLUDED(mutex_);

  zetasql_base::Status GetCatalogs(
      absl::flat_hash_set<const Catalog*>* output) const override;
  zetasql_base::Status GetTables(
      absl::flat_hash_set<const Table*>* output) const override;
  zetasql_base::Status GetTypes(
      absl::flat_hash_set<const Type*>* output) const override;
  zetasql_base::Status GetFunctions(
      absl::flat_hash_set<const Function*>* output) const override;

  // Accessors for reading a copy of the object lists in this SimpleCatalog.
  // This is intended primarily for tests.
  std::vector<const Table*> tables() const LOCKS_EXCLUDED(mutex_);
  std::vector<const Model*> models() const LOCKS_EXCLUDED(mutex_);
  std::vector<const Type*> types() const LOCKS_EXCLUDED(mutex_);
  std::vector<const Function*> functions() const LOCKS_EXCLUDED(mutex_);
  std::vector<const TableValuedFunction*> table_valued_functions() const
      LOCKS_EXCLUDED(mutex_);
  std::vector<const Procedure*> procedures() const LOCKS_EXCLUDED(mutex_);
  std::vector<Catalog*> catalogs() const LOCKS_EXCLUDED(mutex_);
  std::vector<const Constant*> constants() const LOCKS_EXCLUDED(mutex_);

  // Accessors for reading a copy of the key (object-name) lists in this
  // SimpleCatalog. Note that all keys are lower case.
  std::vector<std::string> table_names() const LOCKS_EXCLUDED(mutex_);
  std::vector<std::string> model_names() const LOCKS_EXCLUDED(mutex_);
  std::vector<std::string> function_names() const LOCKS_EXCLUDED(mutex_);
  std::vector<std::string> table_valued_function_names() const
      LOCKS_EXCLUDED(mutex_);
  std::vector<std::string> catalog_names() const LOCKS_EXCLUDED(mutex_);
  std::vector<std::string> constant_names() const LOCKS_EXCLUDED(mutex_);

 private:
  zetasql_base::Status SerializeImpl(absl::flat_hash_set<const Catalog*>* seen_catalogs,
                             FileDescriptorSetMap* file_descriptor_set_map,
                             SimpleCatalogProto* proto, bool ignore_builtin,
                             bool ignore_recursive) const
      LOCKS_EXCLUDED(mutex_);

  // Implements AddCatalog() interface for callers that already own mutex_.
  void AddCatalogLocked(const std::string& name, Catalog* catalog)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // Helper methods for adding objects while holding <mutex_>.
  // TODO: Refactor the Add*() methods for other object types
  // to use a common locked implementation, similar to these for Function.
  void AddFunctionLocked(const std::string& name, const Function* function)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void AddOwnedFunctionLocked(const std::string& name,
                              std::unique_ptr<const Function> function)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void AddTableValuedFunctionLocked(
      const std::string& name, const TableValuedFunction* table_function)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void AddOwnedTableValuedFunctionLocked(
      const std::string& name,
      std::unique_ptr<const TableValuedFunction> table_function)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void AddTypeLocked(const std::string& name, const Type* type)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Unified implementation of SuggestFunction and SuggestTableValuedFunction.
  std::string SuggestFunctionOrTableValuedFunction(
      bool is_table_valued_function, absl::Span<const std::string> mistyped_path);

  const std::string name_;

  mutable absl::Mutex mutex_;

  // The TypeFactory can be allocated lazily, so may be NULL.
  TypeFactory* type_factory_ GUARDED_BY(mutex_);
  std::unique_ptr<TypeFactory> owned_type_factory_ GUARDED_BY(mutex_);

  absl::flat_hash_map<std::string, const Table*> tables_ GUARDED_BY(mutex_);
  absl::flat_hash_map<std::string, const Connection*> connections_
      GUARDED_BY(mutex_);
  absl::flat_hash_map<std::string, const Model*> models_ GUARDED_BY(mutex_);
  // Case-insensitive map of names to Types explicitly added to the Catalog via
  // AddType (including proto an enum types). Names in types_ override names in
  // cached_proto_or_enum_types.
  absl::flat_hash_map<std::string, const Type*> types_ GUARDED_BY(mutex_);
  // Case-sensitive map of names to cached ProtoType or EnumType objects created
  // on-the-fly in GetType from the DescriptorPool.
  absl::flat_hash_map<std::string, const Type*> cached_proto_or_enum_types_
      GUARDED_BY(mutex_);
  absl::flat_hash_map<std::string, const Function*> functions_ GUARDED_BY(mutex_);
  absl::flat_hash_map<std::string, const TableValuedFunction*>
      table_valued_functions_ GUARDED_BY(mutex_);
  absl::flat_hash_map<std::string, const Procedure*> procedures_ GUARDED_BY(mutex_);
  absl::flat_hash_map<std::string, Catalog*> catalogs_ GUARDED_BY(mutex_);
  absl::flat_hash_map<std::string, const Constant*> constants_ GUARDED_BY(mutex_);

  std::vector<std::unique_ptr<const Table>> owned_tables_ GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<const Model>> owned_models_ GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<const Connection>> owned_connections_
      GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<const Function>> owned_functions_
      GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<const TableValuedFunction>>
      owned_table_valued_functions_ GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<const Procedure>> owned_procedures_
      GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<const Catalog>> owned_catalogs_
      GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<const Constant>> owned_constants_
      GUARDED_BY(mutex_);

  // Subcatalogs added for zetasql function namespaces. Kept separate from
  // owned_catalogs_ to keep them as SimpleCatalog types.
  absl::flat_hash_map<std::string, std::unique_ptr<SimpleCatalog>>
      owned_zetasql_subcatalogs_ GUARDED_BY(mutex_);

  const google::protobuf::DescriptorPool* descriptor_pool_ GUARDED_BY(mutex_) = nullptr;
  std::unique_ptr<const google::protobuf::DescriptorPool> GUARDED_BY(mutex_)
      owned_descriptor_pool_;
};

// SimpleTable is a concrete implementation of the Table interface.
class SimpleTable : public Table {
 public:
  // Make a table with columns with the given names and types.
  // Crashes if there are duplicate column names.
  typedef std::pair<std::string, const Type*> NameAndType;
  SimpleTable(const std::string& name, const std::vector<NameAndType>& columns,
              const int64_t id = 0);

  // Make a table with the given Columns.
  // Crashes if there are duplicate column names.
  // Takes ownership of elements of <columns> if <take_ownership> is true.
  SimpleTable(const std::string& name, const std::vector<const Column*>& columns,
              bool take_ownership = false, const int64_t id = 0);

  // Make a value table with row type <row_type>.
  // The value column has no name visible in SQL but will be called
  // "value" in the resolved AST.
  SimpleTable(const std::string& name, const Type* row_type, const int64_t id = 0);

  // Make a table with no Columns.  (Other constructors are ambiguous for this.)
  explicit SimpleTable(const std::string& name, const int64_t id = 0);

  SimpleTable(const SimpleTable&) = delete;
  SimpleTable& operator=(const SimpleTable&) = delete;

  std::string Name() const override { return name_; }
  std::string FullName() const override { return name_; }

  int NumColumns() const override { return columns_.size(); }
  const Column* GetColumn(int i) const override { return columns_[i]; }
  const Column* FindColumnByName(const std::string& name) const override;

  bool IsValueTable() const override { return is_value_table_; }

  void set_is_value_table(bool value) { is_value_table_ = value; }

  bool AllowAnonymousColumnName() const { return allow_anonymous_column_name_; }

  // Setter for allow_anonymous_column_name_. If the existing condition
  // conflicts with the value to be set, the setting will fail.
  zetasql_base::Status set_allow_anonymous_column_name(bool value) {
    ZETASQL_RET_CHECK(value || !anonymous_column_seen_);
    allow_anonymous_column_name_ = value;
    return zetasql_base::OkStatus();
  }

  bool AllowDuplicateColumnNames() const {
    return allow_duplicate_column_names_;
  }

  // Setter for allow_duplicate_column_names_. If the existing condition
  // conflicts with the value to be set, the setting will fail.
  zetasql_base::Status set_allow_duplicate_column_names(bool value) {
    ZETASQL_RET_CHECK(value || duplicate_column_names_.empty());
    allow_duplicate_column_names_ = value;
    return zetasql_base::OkStatus();
  }

  // Add a column. Returns an error if constraints allow_anonymous_column_name_
  // or allow_duplicate_column_names_ are violated.
  // If is_owned is set to true but an error is returned, the column will be
  // deleted inside this function.
  zetasql_base::Status AddColumn(const Column* column, bool is_owned);

  int64_t GetSerializationId() const override { return id_; }

  // Constructs an EvaluatorTableIterator from a list of column indexes.
  // Represents the signature of Table::CreateEvaluatorTableIterator().
  using EvaluatorTableIteratorFactory =
      std::function<zetasql_base::StatusOr<std::unique_ptr<EvaluatorTableIterator>>(
          absl::Span<const int>)>;

  // Sets a factory to be returned by CreateEvaluatorTableIterator().
  // CAVEAT: This is not preserved by serialization/deserialization. It is only
  // relevant to users of the evaluator API defined in public/evaluator.h.
  void SetEvaluatorTableIteratorFactory(
      const EvaluatorTableIteratorFactory& factory) {
    evaluator_table_iterator_factory_ =
        absl::make_unique<EvaluatorTableIteratorFactory>(factory);
  }

  // Convenience method that calls SetEvaluatorTableIteratorFactory to
  // correspond to a list of rows. More specifically, sets the table contents
  // to a copy of 'rows' and sets up a callback to return those values when
  // CreateEvaluatorTableIterator() is called.
  // CAVEAT: This is not preserved by serialization/deserialization.  It is only
  // relevant to users of the evaluator API defined in public/evaluator.h.
  void SetContents(const std::vector<std::vector<Value>>& rows);

  zetasql_base::StatusOr<std::unique_ptr<EvaluatorTableIterator>>
  CreateEvaluatorTableIterator(
      absl::Span<const int> column_idxs) const override;

  // Serialize this table into protobuf. The provided map is used to store
  // serialized FileDescriptorSets, which can be deserialized into separate
  // DescriptorPools in order to reconstruct the Type. The map may be
  // non-empty and may be used across calls to this method in order to
  // serialize multiple types. The map may NOT be null.
  zetasql_base::Status Serialize(
      FileDescriptorSetMap* file_descriptor_set_map,
      SimpleTableProto* proto) const;

  // Deserialize SimpleTable from proto. Types will be deserialized using
  // the given TypeFactory and Descriptors from the given DescriptorPools.
  // The DescriptorPools should have been created by type serialization for
  // columns, and all proto type are treated as references into these pools.
  // The TypeFactory and the DescriptorPools must both outlive the result
  // SimpleTable.
  static zetasql_base::Status Deserialize(
      const SimpleTableProto& proto,
      const std::vector<const google::protobuf::DescriptorPool*>& pools,
      TypeFactory* factory,
      std::unique_ptr<SimpleTable>* result);

 protected:
  // Returns the current contents (passed to the last call to SetContents()) in
  // column-major order.
  const std::vector<std::shared_ptr<const std::vector<Value>>>&
  column_major_contents() const {
    return column_major_contents_;
  }

 private:
  // Insert a column to columns_map_. Return error when
  // allow_anonymous_column_name_ or allow_duplicate_column_names_ are violated.
  // Furthermore, if the column's name is duplicated, it's recorded in
  // duplicate_column_names_ and the original column is removed from
  // columns_map_.
  zetasql_base::Status InsertColumnToColumnMap(const Column* column);

  const std::string name_;
  bool is_value_table_ = false;
  std::vector<const Column*> columns_;
  std::vector<std::unique_ptr<const Column>> owned_columns_;
  absl::flat_hash_map<std::string, const Column*> columns_map_;
  absl::flat_hash_set<std::string> duplicate_column_names_;
  int64_t id_ = 0;
  bool allow_anonymous_column_name_ = false;
  bool anonymous_column_seen_ = false;
  bool allow_duplicate_column_names_ = false;

  // We use shared_ptrs to handle calls to SetContets() while there are
  // iterators outstanding.
  std::vector<std::shared_ptr<const std::vector<Value>>> column_major_contents_;
  std::unique_ptr<EvaluatorTableIteratorFactory>
      evaluator_table_iterator_factory_;

  static zetasql_base::Status ValidateNonEmptyColumnName(const std::string& column_name);
};

// SimpleModel is a concrete implementation of the Model interface.
class SimpleModel : public Model {
 public:
  // Make a model with input and output columns with the given names and types.
  // Crashes if there are duplicate column names.
  typedef std::pair<std::string, const Type*> NameAndType;
  SimpleModel(const std::string& name, const std::vector<NameAndType>& inputs,
              const std::vector<NameAndType>& outputs, const int64_t id = 0);

  // Make a model with the given inputs and outputs.
  // Crashes if there are duplicate column names.
  // Takes ownership of elements of <inputs> and <outputs> if <take_ownership>
  // is true.
  SimpleModel(const std::string& name, const std::vector<const Column*>& inputs,
              const std::vector<const Column*>& outputs,
              bool take_ownership = false, const int64_t id = 0);

  SimpleModel(const SimpleModel&) = delete;
  SimpleModel& operator=(const SimpleModel&) = delete;

  std::string Name() const override { return name_; }
  std::string FullName() const override { return name_; }

  uint64_t NumInputs() const override { return inputs_.size(); }
  // i must be less than NumInputs.
  const Column* GetInput(int i) const override { return inputs_[i]; }
  const Column* FindInputByName(const std::string& name) const override;

  uint64_t NumOutputs() const override { return outputs_.size(); }
  // i must be less than NumOutputs.
  const Column* GetOutput(int i) const override { return outputs_[i]; }
  const Column* FindOutputByName(const std::string& name) const override;

  // Add an input.
  // If is_owned is set to true but an error is returned, the column will be
  // deleted inside this function.
  zetasql_base::Status AddInput(const Column* column, bool is_owned);
  // Add an output.
  // If is_owned is set to true but an error is returned, the column will be
  // deleted inside this function.
  zetasql_base::Status AddOutput(const Column* column, bool is_owned);

  int64_t GetSerializationId() const override { return id_; }

  // TODO: Add serialize and deserialize functions.
 private:
  const std::string name_;
  // Columns added to the <inputs_>, <outputs_> and the corresponding maps may
  // be owned by this class or not. In case they are owned by this class, they
  // will be added to <owned_inputs_outputs_> and will be deleted at destruction
  // time.
  absl::flat_hash_map<std::string, const Column*> inputs_map_;
  std::vector<const Column*> inputs_;

  absl::flat_hash_map<std::string, const Column*> outputs_map_;
  std::vector<const Column*> outputs_;

  // All the input and output columns that this class owns.
  std::vector<std::unique_ptr<const Column>> owned_inputs_outputs_;
  int64_t id_ = 0;
};

class SimpleConnection : public Connection {
 public:
  explicit SimpleConnection(const std::string& name) : name_(name) {}
  SimpleConnection(const SimpleConnection&) = delete;
  SimpleConnection& operator=(const Connection&) = delete;

  std::string Name() const override { return name_; }
  std::string FullName() const override { return name_; }

  // TODO: Add serialize and deserialize functions.
 private:
  const std::string name_;
};

// SimpleColumn is a concrete implementation of the Column interface.
class SimpleColumn : public Column {
 public:
  // Constructor.
  SimpleColumn(const std::string& table_name, const std::string& name, const Type* type,
               bool is_pseudo_column = false, bool is_writable_column = true);
  SimpleColumn(const SimpleColumn&) = delete;
  SimpleColumn& operator=(const SimpleColumn&) = delete;

  ~SimpleColumn() override;

  std::string Name() const override { return name_; }
  std::string FullName() const override { return full_name_; }
  const Type* GetType() const override { return type_; }
  bool IsPseudoColumn() const override { return is_pseudo_column_; }
  bool IsWritableColumn() const override { return is_writable_column_; }

  void set_is_pseudo_column(bool v) { is_pseudo_column_ = v; }

  // Serialize this column into protobuf, the provided map is used to store
  // serialized FileDescriptorSets, which can be deserialized into separate
  // DescriptorPools in order to reconstruct the Type. The map may be
  // non-empty and may be used across calls to this method in order to
  // serialize multiple types. The map may NOT be null.
  zetasql_base::Status Serialize(
      FileDescriptorSetMap* file_descriptor_set_map,
      SimpleColumnProto* proto)const;

  // Deserialize SimpleColumn from proto. Types will be deserialized using
  // the given TypeFactory and Descriptors from the given DescriptorPools.
  // The DescriptorPools should have been created by type serialization,
  // and all proto types are treated as references into these pools.
  // The TypeFactory and the DescriptorPools must both outlive the result
  // SimpleColumn.
  static zetasql_base::Status Deserialize(
      const SimpleColumnProto& proto,
      const std::string& table_name,
      const std::vector<const google::protobuf::DescriptorPool*>& pools,
      TypeFactory* factory,
      std::unique_ptr<SimpleColumn>* result);

 private:
  const std::string name_;
  const std::string full_name_;
  const Type* type_;
  bool is_pseudo_column_ = false;
  bool is_writable_column_ = true;
};

// A named constant with a concrete value in the catalog.
class SimpleConstant : public Constant {
 public:
  // Creates and returns a SimpleConstant, returning an error if <value> is
  // an invalid Value or the <name_path> is empty.
  static zetasql_base::Status Create(
      const std::vector<std::string>& name_path, const Value& value,
      std::unique_ptr<SimpleConstant>* simple_constant);

  ~SimpleConstant() override {}

  // This class is neither copyable nor assignable.
  SimpleConstant(const SimpleConstant& other_simple_constant) = delete;
  SimpleConstant& operator=(const SimpleConstant& other_simple_constant) =
      delete;

  // Serializes this SimpleConstant to proto.
  //
  // See SimpleCatalog::Serialize() for details about <file_descriptor_set_map>.
  zetasql_base::Status Serialize(FileDescriptorSetMap* file_descriptor_set_map,
                         SimpleConstantProto* simple_constant_proto) const;

  // Deserializes this SimpleConstant from proto.
  //
  // See SimpleCatalog::Deserialize() for details about <descriptor_pools>.
  static zetasql_base::Status Deserialize(
      const SimpleConstantProto& simple_constant_proto,
      const std::vector<const google::protobuf::DescriptorPool*>& descriptor_pools,
      TypeFactory* type_factory,
      std::unique_ptr<SimpleConstant>* simple_constant);

  const Type* type() const override { return value_.type(); }

  const Value& value() const { return value_; }

  // Returns a std::string describing this Constant for debugging purposes.
  std::string DebugString() const override;
  // Same as the previous, but includes the Type debug std::string.
  std::string VerboseDebugString() const;

 private:
  SimpleConstant(const std::vector<std::string>& name_path, Value value)
      : Constant(name_path), value_(std::move(value)) {}

  // The value of this Constant. This is the RHS in a CREATE CONSTANT statement.
  Value value_;
};

}  // namespace zetasql

#endif  // ZETASQL_PUBLIC_SIMPLE_CATALOG_H_
