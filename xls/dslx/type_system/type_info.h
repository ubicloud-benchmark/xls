// Copyright 2020 The XLS Authors
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

// Support for carrying information from the type inferencing phase.

#ifndef XLS_DSLX_TYPE_INFO_H_
#define XLS_DSLX_TYPE_INFO_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "xls/common/logging/logging.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/concrete_type.h"
#include "xls/dslx/type_system/parametric_env.h"

namespace xls::dslx {

class TypeInfo;

// Information associated with an import node in the AST.
struct ImportedInfo {
  Module* module;
  TypeInfo* type_info;
};

// Represents a (start, width) pair used for a bit-slice operation, as
// determined at type inference time.
struct StartAndWidth {
  int64_t start;
  int64_t width;
};

// Data associated with a slice AST node, associating it with concrete
// start/width values determined at type inferencing time.
struct SliceData {
  Slice* node;
  absl::flat_hash_map<ParametricEnv, StartAndWidth> bindings_to_start_width;
};

// For a given invocation, this is the data we record on the parameteric callee
// -- "callee_bindings" notes what the parametric environment is for the callee
// and "derived_type_info" holds the type information that is specific to that
// parametric instantiation.
struct InvocationCalleeData {
  ParametricEnv callee_bindings;
  TypeInfo* derived_type_info;
};

// Parametric instantiation information related to an invocation AST node.
struct InvocationData {
  // Invocation/Spawn AST node.
  const Invocation* node;

  // Function containing the above invocation "node". This is held for
  // "referential integrity" so we can check the validity of the caller
  // environments in "env_to_callee_data".
  //
  // Note that this can be nullptr when the invocation is at the top level, e.g.
  // in a const binding.
  const Function* caller;

  // Map from symbolic bindings in the caller to the corresponding symbolic
  // bindings in the callee for this invocation.
  absl::flat_hash_map<ParametricEnv, InvocationCalleeData> env_to_callee_data;

  std::string ToString() const;
};

// Owns "type information" objects created during the type checking process.
//
// In the process of type checking we may instantiate "sub type-infos" for
// things like particular parametric instantiations, that have
// parametric-independent type information as a parent (see TypeInfo::parent()).
//
// Since we decide to create these "sub type-infos" in a way that is driven by
// the program at type checking time, we place all type info objects into this
// owned pool (arena style ownership to avoid circular references or leaks or
// any other sort of lifetime issues).
class TypeInfoOwner {
 public:
  // Returns an error status iff parent is nullptr and "module" already has a
  // root type info.
  absl::StatusOr<TypeInfo*> New(Module* module, TypeInfo* parent = nullptr);

  // Retrieves the root type information for the given module, or a not-found
  // status error if it is not present.
  absl::StatusOr<TypeInfo*> GetRootTypeInfo(const Module* module);

 private:
  // Mapping from module to the "root" (or "parentmost") type info -- these have
  // nullptr as their parent. There should only be one of these for any given
  // module.
  absl::flat_hash_map<const Module*, TypeInfo*> module_to_root_;

  // Owned type information objects -- TypeInfoOwner is the lifetime owner for
  // these.
  std::vector<std::unique_ptr<TypeInfo>> type_infos_;
};

class TypeInfo {
 public:
  ~TypeInfo();

  // Type information can be "differential"; e.g. when we obtain type
  // information for a particular parametric instantiation the type information
  // is backed by the enclosing type information for the module. Therefore, type
  // information objects can have a "parent" they delegate queries to if they
  // can't satisfy the information from their local mappings.
  TypeInfo* parent() const { return parent_; }

  // Notes start/width for a slice operation found during type inference.
  void AddSliceStartAndWidth(Slice* node, const ParametricEnv& parametric_env,
                             StartAndWidth start_width);

  // Retrieves the start/width pair for a given slice, see comment on SliceData.
  std::optional<StartAndWidth> GetSliceStartAndWidth(
      Slice* node, const ParametricEnv& parametric_env) const;

  // Notes caller/callee relation of parametric env at an invocation.
  //
  // This is kept from type inferencing time for convenience purposes (so it
  // doesn't need to be recalculated anywhere; e.g. in the interpreter).
  //
  // Args:
  //   invocation: The invocation node that (may have) caused parametric
  //     instantiation.
  //   caller: The function containing the invocation -- note that this can be
  //     nullptr if the invocation is at the top level of the module.
  //   caller_env: The caller's symbolic bindings at the point of invocation.
  //   callee_env: The callee's computed symbolic bindings for the invocation.
  void AddInvocationTypeInfo(const Invocation& invocation,
                             const Function* caller,
                             const ParametricEnv& caller_env,
                             const ParametricEnv& callee_env,
                             TypeInfo* derived_type_info);

  // Attempts to retrieve "instantiation" type information -- that is, when
  // there's an invocation with parametrics in a caller, it may map to
  // particular type-information for the callee.
  std::optional<TypeInfo*> GetInvocationTypeInfo(
      const Invocation* invocation, const ParametricEnv& caller) const;

  // As above, but returns a NotFound error if the invocation does not have
  // associated type information.
  absl::StatusOr<TypeInfo*> GetInvocationTypeInfoOrError(
      const Invocation* invocation, const ParametricEnv& caller) const;

  // Sets the type info for the given proc when typechecked at top-level (i.e.,
  // not via an instantiation). Can only be called on the module root TypeInfo.
  absl::Status SetTopLevelProcTypeInfo(const Proc* p, TypeInfo* ti);

  // Gets the TypeInfo for the given function or proc. Can only [successfully]
  // called on the module root TypeInfo.
  absl::StatusOr<TypeInfo*> GetTopLevelProcTypeInfo(const Proc* p);

  // Sets the type associated with the given AST node.
  void SetItem(const AstNode* key, const ConcreteType& value) {
    XLS_CHECK_EQ(key->owner(), module_);
    dict_[key] = value.CloneToUnique();
  }

  // Attempts to resolve AST node 'key' in the node-to-type dictionary.
  std::optional<ConcreteType*> GetItem(const AstNode* key) const;
  absl::StatusOr<ConcreteType*> GetItemOrError(const AstNode* key) const;

  // Attempts to resolve AST node 'key' to a type with subtype T; e.g.:
  //
  //    absl::StatusOr<FunctionType*> f_type =
  //        type_info.GetItemAs<FunctionType>(my_func);
  //
  // If the value is not present, or it is not of the expected type, returns an
  // error status.
  template <typename T>
  absl::StatusOr<T*> GetItemAs(const AstNode* key) const;

  bool Contains(AstNode* key) const;

  // Import AST node based information.
  //
  // Note that added type information and such will generally be owned by the
  // import cache.
  void AddImport(Import* import, Module* module, TypeInfo* type_info);
  std::optional<const ImportedInfo*> GetImported(Import* import) const;
  absl::StatusOr<const ImportedInfo*> GetImportedOrError(Import* import) const;
  const absl::flat_hash_map<Import*, ImportedInfo>& imports() const {
    return imports_;
  }

  // Returns the type information for m, if it is available either as this
  // module or an import of this module.
  std::optional<TypeInfo*> GetImportedTypeInfo(Module* m);

  // Returns whether function "f" requires an implicit token parameter; i.e. it
  // contains a `fail!()` or `cover!()` as determined during type inferencing.
  std::optional<bool> GetRequiresImplicitToken(const Function* f) const;
  void NoteRequiresImplicitToken(const Function* f, bool is_required);

  // Attempts to retrieve the callee's parametric values in an "instantiation".
  // That is, in the case of:
  //
  //    fn id<M: u32>(x: bits[M]) -> bits[M] { x }
  //    fn p<N: u32>(x: bits[N]) -> bits[N] { id(x) }
  //
  // The invocation of `id(x)` in caller `p` when N=32 would resolve to a record
  // with M=32.
  //
  // When calling a non-parametric callee, the record will be absent.
  std::optional<const ParametricEnv*> GetInvocationCalleeBindings(
      const Invocation* invocation, const ParametricEnv& caller) const;

  Module* module() const { return module_; }

  // Notes the evaluation of a constexpr to a value, as discovered during type
  // checking. Some constructs *require* constexprs, e.g. slice bounds or
  // for-loop range upper limits.
  //
  // Since TypeInfos exist in a tree to indicate parametric instantiation, the
  // note of constexpr evaluation lives on this TypeInfo specifically (it does
  // not automatically get placed in the root of the tree). This avoids
  // collisions in cases e.g. where you slice `[0:N]` where `N` is a parametric
  // value.
  //
  // Note that these index over AstNodes instead of Exprs so that NameDefs can
  // be used as constexpr keys.
  void NoteConstExpr(const AstNode* const_expr, InterpValue value);
  bool IsKnownConstExpr(const AstNode* node) const;
  bool IsKnownNonConstExpr(const AstNode* node) const;
  absl::StatusOr<InterpValue> GetConstExpr(const AstNode* const_expr) const;
  std::optional<InterpValue> GetConstExprOption(
      const AstNode* const_expr) const;

  // Retrieves a string that shows the module associated with this type info and
  // which imported modules are present, suitable for debugging.
  std::string GetImportsDebugString() const;

  // Returns a string with the tree of type information (e.g. with
  // what instantiations are present and what the derivated type info pointers
  // are) suitable for debugging.
  std::string GetTypeInfoTreeString() const;

  // Returns the invocation-to-instantiation-data mapping that present on the
  // root type information for this type information tree.
  //
  // Implementation note: all instantiation information is only held on the root
  // type information, which is why `invocations()` is not exposed publicly.
  const absl::flat_hash_map<const Invocation*, InvocationData>&
  GetRootInvocations() const {
    return GetRoot()->invocations();
  }

  // Returns a reference to the underlying mapping that associates an AST node
  // with its deduced type.
  const absl::flat_hash_map<const AstNode*, std::unique_ptr<ConcreteType>>&
  dict() const {
    return dict_;
  }

 private:
  friend class TypeInfoOwner;

  const absl::flat_hash_map<const Invocation*, InvocationData>& invocations()
      const {
    XLS_CHECK(IsRoot());
    return invocations_;
  }

  // Args:
  //  module: The module that owns the AST nodes referenced in the (member)
  //    maps.
  //  parent: Type information that should be queried from the same scope (i.e.
  //    if an AST node is not resolved in the local member maps, the lookup is
  //    then performed in the parent, and so on transitively).
  explicit TypeInfo(Module* module, TypeInfo* parent = nullptr);

  // Traverses to the 'root' (AKA 'most parent') TypeInfo. This is a place to
  // stash context-free information (e.g. that is found in a parametric
  // instantiation context, but that we want to be accessible to other
  // parametric instantiations).
  TypeInfo* GetRoot() {
    TypeInfo* t = this;
    while (t->parent_ != nullptr) {
      t = t->parent_;
    }
    return t;
  }
  const TypeInfo* GetRoot() const {
    return const_cast<TypeInfo*>(this)->GetRoot();
  }

  // Returns whether this is the root type information for the module (vs. a
  // dervied type info for e.g. a parametric instantiation context).
  bool IsRoot() const { return this == GetRoot(); }

  Module* module_;

  // Node to type mapping -- this is present on "derived" type info (i.e. for
  // instantiated parametric type info) as well as the root type information for
  // a module.
  absl::flat_hash_map<const AstNode*, std::unique_ptr<ConcreteType>> dict_;

  // Node to constexpr-value mapping -- this is also present on "derived" type
  // info as constexprs take on different values in different parametric
  // instantiation contexts.
  absl::flat_hash_map<const AstNode*, std::optional<InterpValue>> const_exprs_;

  // The following are only present on the root type info.
  absl::flat_hash_map<Import*, ImportedInfo> imports_;
  absl::flat_hash_map<const Invocation*, InvocationData> invocations_;
  absl::flat_hash_map<Slice*, SliceData> slices_;
  absl::flat_hash_map<const Function*, bool> requires_implicit_token_;

  // Maps a Proc to the TypeInfo used for its top-level typechecking.
  absl::flat_hash_map<const Proc*, TypeInfo*> top_level_proc_type_info_;

  TypeInfo* parent_;  // Note: may be nullptr.
};

// -- Inlines

template <typename T>
inline absl::StatusOr<T*> TypeInfo::GetItemAs(const AstNode* key) const {
  std::optional<ConcreteType*> t = GetItem(key);
  if (!t.has_value()) {
    return absl::NotFoundError(
        absl::StrFormat("No type found for AST node: %s @ %s", key->ToString(),
                        SpanToString(key->GetSpan())));
  }
  XLS_DCHECK(t.value() != nullptr);
  auto* target = dynamic_cast<T*>(t.value());
  if (target == nullptr) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "AST node (%s) @ %s did not have expected ConcreteType subtype.",
        key->GetNodeTypeName(), SpanToString(key->GetSpan())));
  }
  return target;
}

}  // namespace xls::dslx

#endif  // XLS_DSLX_TYPE_INFO_H_
