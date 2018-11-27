// Copyright 2010-2018 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/sat/cp_model_presolve.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_join.h"
#include "ortools/base/hash.h"
#include "ortools/base/integral_types.h"
#include "ortools/base/logging.h"
#include "ortools/base/map_util.h"
#include "ortools/base/stl_util.h"
#include "ortools/port/proto_utils.h"
#include "ortools/sat/cp_model_checker.h"
#include "ortools/sat/cp_model_loader.h"
#include "ortools/sat/cp_model_objective.h"
#include "ortools/sat/cp_model_utils.h"
#include "ortools/sat/probing.h"
#include "ortools/sat/sat_base.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "ortools/sat/simplification.h"
#include "ortools/util/affine_relation.h"
#include "ortools/util/bitset.h"
#include "ortools/util/sorted_interval_list.h"

namespace operations_research {
namespace sat {
namespace {

// Wrap the CpModelProto we are presolving with extra data structure like the
// in-memory domain of each variables and the constraint variable graph.
struct PresolveContext {
  bool DomainIsEmpty(int ref) const {
    return domains[PositiveRef(ref)].IsEmpty();
  }

  bool IsFixed(int ref) const {
    CHECK(!DomainIsEmpty(ref));
    return domains[PositiveRef(ref)].Min() == domains[PositiveRef(ref)].Max();
  }

  bool LiteralIsTrue(int lit) const {
    if (!IsFixed(lit)) return false;
    if (RefIsPositive(lit)) {
      return domains[lit].Min() == 1ll;
    } else {
      return domains[PositiveRef(lit)].Max() == 0ll;
    }
  }

  bool LiteralIsFalse(int lit) const {
    if (!IsFixed(lit)) return false;
    if (RefIsPositive(lit)) {
      return domains[lit].Max() == 0ll;
    } else {
      return domains[PositiveRef(lit)].Min() == 1ll;
    }
  }

  int64 MinOf(int ref) const {
    CHECK(!DomainIsEmpty(ref));
    return RefIsPositive(ref) ? domains[PositiveRef(ref)].Min()
                              : -domains[PositiveRef(ref)].Max();
  }

  int64 MaxOf(int ref) const {
    CHECK(!DomainIsEmpty(ref));
    return RefIsPositive(ref) ? domains[PositiveRef(ref)].Max()
                              : -domains[PositiveRef(ref)].Min();
  }

  // Returns true if this ref only appear in one constraint.
  bool VariableIsUniqueAndRemovable(int ref) const {
    return var_to_constraints[PositiveRef(ref)].size() == 1 &&
           !enumerate_all_solutions;
  }

  Domain DomainOf(int ref) const {
    if (RefIsPositive(ref)) return domains[ref];
    return domains[PositiveRef(ref)].Negation();
  }

  // Returns true iff the domain changed.
  bool IntersectDomainWith(int ref, const Domain& domain) {
    CHECK(!DomainIsEmpty(ref));
    const int var = PositiveRef(ref);

    if (RefIsPositive(ref)) {
      if (domains[var].IsIncludedIn(domain)) return false;
      domains[var] = domains[var].IntersectionWith(domain);
    } else {
      const Domain temp = domain.Negation();
      if (domains[var].IsIncludedIn(temp)) return false;
      domains[var] = domains[var].IntersectionWith(temp);
    }

    modified_domains.Set(var);
    if (domains[var].IsEmpty()) is_unsat = true;
    return true;
  }

  void SetLiteralToFalse(int lit) {
    const int var = PositiveRef(lit);
    const int64 value = RefIsPositive(lit) ? 0ll : 1ll;
    if (IsFixed(var)) {
      const int64 fixed_value = MinOf(var);
      if (value != fixed_value) {
        is_unsat = true;
      }
    } else {
      IntersectDomainWith(var, Domain(value));
    }
  }

  void SetLiteralToTrue(int lit) { return SetLiteralToFalse(NegatedRef(lit)); }

  void UpdateRuleStats(const std::string& name) { stats_by_rule_name[name]++; }

  // Update the constraints <-> variables graph. This needs to be called each
  // time a constraint is modified.
  void UpdateConstraintVariableUsage(int c) {
    CHECK_EQ(constraint_to_vars.size(), working_model->constraints_size());
    const ConstraintProto& ct = working_model->constraints(c);
    for (const int v : constraint_to_vars[c]) var_to_constraints[v].erase(c);
    constraint_to_vars[c] = UsedVariables(ct);
    for (const int v : constraint_to_vars[c]) var_to_constraints[v].insert(c);
  }

  // Calls UpdateConstraintVariableUsage() on all newly created constraints.
  void UpdateNewConstraintsVariableUsage() {
    const int old_size = constraint_to_vars.size();
    const int new_size = working_model->constraints_size();
    CHECK_LE(old_size, new_size);
    constraint_to_vars.resize(new_size);
    for (int c = old_size; c < new_size; ++c) {
      constraint_to_vars[c] = UsedVariables(working_model->constraints(c));
      for (const int v : constraint_to_vars[c]) var_to_constraints[v].insert(c);
    }
  }

  // Returns true if our current constraints <-> variables graph is ok.
  // This is meant to be used in DEBUG mode only.
  bool ConstraintVariableUsageIsConsistent() {
    if (is_unsat) return true;
    if (constraint_to_vars.size() != working_model->constraints_size()) {
      LOG(INFO) << "Wrong constraint_to_vars size!";
      return false;
    }
    for (int c = 0; c < constraint_to_vars.size(); ++c) {
      if (constraint_to_vars[c] !=
          UsedVariables(working_model->constraints(c))) {
        LOG(INFO) << "Wrong variables usage for constraint: \n"
                  << ProtobufDebugString(working_model->constraints(c));
        return false;
      }
    }
    return true;
  }

  // Regroups fixed variables with the same value.
  // TODO(user): Also regroup cte and -cte?
  void ExploitFixedDomain(int var) {
    CHECK(IsFixed(var));
    const int min = MinOf(var);
    if (gtl::ContainsKey(constant_to_ref, min)) {
      const int representative = constant_to_ref[min];
      if (representative != var) {
        affine_relations.TryAdd(var, representative, 1, 0);
        var_equiv_relations.TryAdd(var, representative, 1, 0);
      }
    } else {
      constant_to_ref[min] = var;
    }
  }

  // Adds the relation (ref_x = coeff * ref_y + offset) to the repository.
  void AddAffineRelation(const ConstraintProto& ct, int ref_x, int ref_y,
                         int64 coeff, int64 offset) {
    int x = PositiveRef(ref_x);
    int y = PositiveRef(ref_y);
    if (IsFixed(x) || IsFixed(y)) return;

    int64 c = RefIsPositive(ref_x) == RefIsPositive(ref_y) ? coeff : -coeff;
    int64 o = RefIsPositive(ref_x) ? offset : -offset;
    const int rep_x = affine_relations.Get(x).representative;
    const int rep_y = affine_relations.Get(y).representative;

    // If a Boolean variable (one with domain [0, 1]) appear in this affine
    // equivalence class, then we want its representative to be Boolean. Note
    // that this is always possible because a Boolean variable can never be
    // equal to a multiple of another if std::abs(coeff) is greater than 1 and
    // if it is not fixed to zero. This is important because it allows to simply
    // use the same representative for any referenced literals.
    bool allow_rep_x = MinOf(rep_x) == 0 && MaxOf(rep_x) == 1;
    bool allow_rep_y = MinOf(rep_y) == 0 && MaxOf(rep_y) == 1;
    if (!allow_rep_x && !allow_rep_y) {
      // If none are Boolean, we can use any representative.
      allow_rep_x = true;
      allow_rep_y = true;
    }

    // TODO(user): can we force the rep and remove GetAffineRelation()?
    bool added = affine_relations.TryAdd(x, y, c, o, allow_rep_x, allow_rep_y);
    if ((c == 1 || c == -1) && o == 0) {
      added |= var_equiv_relations.TryAdd(x, y, c, o, allow_rep_x, allow_rep_y);
    }
    if (added) {
      // The domain didn't change, but this notification allows to re-process
      // any constraint containing these variables.
      modified_domains.Set(x);
      modified_domains.Set(y);
      affine_constraints.insert(&ct);
    }
  }

  void AddBooleanEqualityRelation(int ref_a, int ref_b) {
    if (ref_a == ref_b) return;
    if (ref_a == NegatedRef(ref_b)) {
      is_unsat = true;
      return;
    }
    bool added = false;
    if (RefIsPositive(ref_a) == RefIsPositive(ref_b)) {
      added |=
          affine_relations.TryAdd(PositiveRef(ref_a), PositiveRef(ref_b), 1, 0);
      added |= var_equiv_relations.TryAdd(PositiveRef(ref_a),
                                          PositiveRef(ref_b), 1, 0);
    } else {
      added |= affine_relations.TryAdd(PositiveRef(ref_a), PositiveRef(ref_b),
                                       -1, 1);
    }
    if (!added) return;

    modified_domains.Set(PositiveRef(ref_a));
    modified_domains.Set(PositiveRef(ref_b));

    // For now, we do need to add the relation ref_a == ref_b so we have a
    // proper variable usage count and propagation between ref_a and ref_b.
    //
    // TODO(user): This looks unclean. We should probably handle the affine
    // relation together without the need of keep all the constraints that
    // define them around.
    ConstraintProto* ct = working_model->add_constraints();
    auto* arg = ct->mutable_linear();
    arg->add_vars(PositiveRef(ref_a));
    arg->add_vars(PositiveRef(ref_b));
    if (RefIsPositive(ref_a) == RefIsPositive(ref_b)) {
      // a = b
      arg->add_coeffs(1);
      arg->add_coeffs(-1);
      arg->add_domain(0);
      arg->add_domain(0);
    } else {
      // a = 1 - b
      arg->add_coeffs(1);
      arg->add_coeffs(1);
      arg->add_domain(1);
      arg->add_domain(1);
    }
    affine_constraints.insert(ct);
    UpdateNewConstraintsVariableUsage();
  }

  // This makes sure that the affine relation only uses one of the
  // representative from the var_equiv_relations.
  AffineRelation::Relation GetAffineRelation(int var) {
    CHECK(RefIsPositive(var));
    AffineRelation::Relation r = affine_relations.Get(var);
    AffineRelation::Relation o = var_equiv_relations.Get(r.representative);
    r.representative = o.representative;
    if (o.coeff == -1) r.coeff = -r.coeff;
    return r;
  }

  // Create the internal structure for any new variables in working_model.
  void InitializeNewDomains() {
    for (int i = domains.size(); i < working_model->variables_size(); ++i) {
      domains.push_back(ReadDomainFromProto(working_model->variables(i)));
      if (IsFixed(i)) ExploitFixedDomain(i);
    }
    modified_domains.Resize(domains.size());
    var_to_constraints.resize(domains.size());
  }

  // This regroup all the affine relations between variables. Note that the
  // constraints used to detect such relations will not be removed from the
  // model at detection time (thus allowing proper domain propagation). However,
  // if the arity of a variable becomes one, then such constraint will be
  // removed.
  AffineRelation affine_relations;
  AffineRelation var_equiv_relations;

  // Set of constraint that implies an "affine relation". We need to mark them,
  // because we can't simplify them using the relation they added.
  //
  // WARNING: This assumes the ConstraintProto* to stay valid during the full
  // presolve even if we add new constraint to the CpModelProto.
  absl::flat_hash_set<ConstraintProto const*> affine_constraints;

  // For each constant variable appearing in the model, we maintain a reference
  // variable with the same constant value. If two variables end up having the
  // same fixed value, then we can detect it using this and add a new
  // equivalence relation. See ExploitFixedDomain().
  absl::flat_hash_map<int64, int> constant_to_ref;

  // Variable <-> constraint graph.
  // The vector list is sorted and contains unique elements.
  //
  // Important: To properly handle the objective, var_to_constraints[objective]
  // contains -1 so that if the objective appear in only one constraint, the
  // constraint cannot be simplified.
  //
  // TODO(user): Make this private?
  std::vector<std::vector<int>> constraint_to_vars;
  std::vector<absl::flat_hash_set<int>> var_to_constraints;

  CpModelProto* working_model;
  CpModelProto* mapping_model;

  // Initially false, and set to true on the first inconsistency.
  bool is_unsat = false;

  // Indicate if we are enumerating all solutions. This disable some presolve
  // rules.
  bool enumerate_all_solutions = false;

  // Just used to display statistics on the presolve rules that were used.
  absl::flat_hash_map<std::string, int> stats_by_rule_name;

  // Temporary storage.
  std::vector<int> tmp_literals;
  std::vector<Domain> tmp_term_domains;
  std::vector<Domain> tmp_left_domains;

  // Each time a domain is modified this is set to true.
  SparseBitset<int64> modified_domains;

 private:
  // The current domain of each variables.
  std::vector<Domain> domains;
};

// =============================================================================
// Presolve functions.
//
// They should return false only if the constraint <-> variable graph didn't
// change. This is just an optimization, returning true is always correct.
//
// TODO(user): it migth be better to simply move all these functions to the
// PresolveContext class.
// =============================================================================

ABSL_MUST_USE_RESULT bool RemoveConstraint(ConstraintProto* ct,
                                           PresolveContext* context) {
  ct->Clear();
  return true;
}

bool PresolveEnforcementLiteral(ConstraintProto* ct, PresolveContext* context) {
  if (!HasEnforcementLiteral(*ct)) return false;

  int new_size = 0;
  const int old_size = ct->enforcement_literal().size();
  for (const int literal : ct->enforcement_literal()) {
    // Remove true literal.
    if (context->LiteralIsTrue(literal)) {
      context->UpdateRuleStats("true enforcement literal");
      continue;
    }

    if (context->LiteralIsFalse(literal)) {
      context->UpdateRuleStats("false enforcement literal");
      return RemoveConstraint(ct, context);
    } else if (context->VariableIsUniqueAndRemovable(literal)) {
      // We can simply set it to false and ignore the constraint in this case.
      context->UpdateRuleStats("enforcement literal not used");
      context->SetLiteralToFalse(literal);
      return RemoveConstraint(ct, context);
    }

    ct->set_enforcement_literal(new_size++, literal);
  }
  ct->mutable_enforcement_literal()->Truncate(new_size);
  return new_size != old_size;
}

bool PresolveBoolOr(ConstraintProto* ct, PresolveContext* context) {
  // Move the enforcement literal inside the clause if any. Note that we do not
  // mark this as a change since the literal in the constraint are the same.
  if (HasEnforcementLiteral(*ct)) {
    context->UpdateRuleStats("bool_or: removed enforcement literal");
    for (const int literal : ct->enforcement_literal()) {
      ct->mutable_bool_or()->add_literals(NegatedRef(literal));
    }
    ct->clear_enforcement_literal();
  }

  // Inspects the literals and deal with fixed ones.
  //
  // TODO(user): detect if one literal is the negation of another in which
  // case the constraint is true. Remove duplicates too. Do the same for
  // the PresolveBoolAnd() function.
  bool changed = false;
  context->tmp_literals.clear();
  for (const int literal : ct->bool_or().literals()) {
    if (context->LiteralIsFalse(literal)) {
      changed = true;
      continue;
    }
    if (context->LiteralIsTrue(literal)) {
      context->UpdateRuleStats("bool_or: always true");
      return RemoveConstraint(ct, context);
    }
    // We can just set the variable to true in this case since it is not
    // used in any other constraint (note that we artifically bump the
    // objective var usage by 1).
    if (context->VariableIsUniqueAndRemovable(literal)) {
      context->UpdateRuleStats("bool_or: singleton");
      context->SetLiteralToTrue(literal);
      return RemoveConstraint(ct, context);
    }
    context->tmp_literals.push_back(literal);
  }

  if (context->tmp_literals.empty()) {
    context->UpdateRuleStats("bool_or: empty");
    context->is_unsat = true;
    return true;
  }
  if (context->tmp_literals.size() == 1) {
    context->UpdateRuleStats("bool_or: only one literal");
    context->SetLiteralToTrue(context->tmp_literals[0]);
    return RemoveConstraint(ct, context);
  }
  if (context->tmp_literals.size() == 2) {
    // For consistency, we move all "implication" into half-reified bool_and.
    // TODO(user): merge by enforcement literal and detect implication cycles.
    context->UpdateRuleStats("bool_or: implications");
    ct->add_enforcement_literal(NegatedRef(context->tmp_literals[0]));
    ct->mutable_bool_and()->add_literals(context->tmp_literals[1]);
    return changed;
  }

  if (changed) {
    context->UpdateRuleStats("bool_or: fixed literals");
    ct->mutable_bool_or()->mutable_literals()->Clear();
    for (const int lit : context->tmp_literals) {
      ct->mutable_bool_or()->add_literals(lit);
    }
  }
  return changed;
}

ABSL_MUST_USE_RESULT bool MarkConstraintAsFalse(ConstraintProto* ct,
                                                PresolveContext* context) {
  if (HasEnforcementLiteral(*ct)) {
    // Change the constraint to a bool_or.
    ct->mutable_bool_or()->clear_literals();
    for (const int lit : ct->enforcement_literal()) {
      ct->mutable_bool_or()->add_literals(NegatedRef(lit));
    }
    ct->clear_enforcement_literal();
    return PresolveBoolOr(ct, context);
  } else {
    context->is_unsat = true;
    return RemoveConstraint(ct, context);
  }
}

bool PresolveBoolAnd(ConstraintProto* ct, PresolveContext* context) {
  if (!HasEnforcementLiteral(*ct)) {
    context->UpdateRuleStats("bool_and: non-reified.");
    for (const int literal : ct->bool_and().literals()) {
      context->SetLiteralToTrue(literal);
    }
    return RemoveConstraint(ct, context);
  }

  bool changed = false;
  context->tmp_literals.clear();
  for (const int literal : ct->bool_and().literals()) {
    if (context->LiteralIsFalse(literal)) {
      context->UpdateRuleStats("bool_and: always false");
      return MarkConstraintAsFalse(ct, context);
    }
    if (context->LiteralIsTrue(literal)) {
      changed = true;
      continue;
    }
    if (context->VariableIsUniqueAndRemovable(literal)) {
      changed = true;
      context->SetLiteralToTrue(literal);
      continue;
    }
    context->tmp_literals.push_back(literal);
  }

  // Note that this is not the same behavior as a bool_or:
  // - bool_or means "at least one", so it is false if empty.
  // - bool_and means "all literals inside true", so it is true if empty.
  if (context->tmp_literals.empty()) return RemoveConstraint(ct, context);

  if (changed) {
    ct->mutable_bool_and()->mutable_literals()->Clear();
    for (const int lit : context->tmp_literals) {
      ct->mutable_bool_and()->add_literals(lit);
    }
    context->UpdateRuleStats("bool_and: fixed literals");
  }
  return changed;
}

bool PresolveAtMostOne(ConstraintProto* ct, PresolveContext* context) {
  CHECK(!HasEnforcementLiteral(*ct));

  bool changed = false;
  context->tmp_literals.clear();
  for (const int literal : ct->at_most_one().literals()) {
    if (context->LiteralIsTrue(literal)) {
      context->UpdateRuleStats("at_most_one: satisfied");
      for (const int other : ct->at_most_one().literals()) {
        if (other == literal) continue;
        context->SetLiteralToFalse(other);
      }
      return RemoveConstraint(ct, context);
    }
    if (context->LiteralIsFalse(literal)) {
      changed = true;
      continue;
    }
    context->tmp_literals.push_back(literal);
  }
  if (context->tmp_literals.empty()) return RemoveConstraint(ct, context);

  if (changed) {
    ct->mutable_at_most_one()->mutable_literals()->Clear();
    for (const int lit : context->tmp_literals) {
      ct->mutable_at_most_one()->add_literals(lit);
    }
    context->UpdateRuleStats("at_most_one: removed literals");
  }
  return changed;
}

bool PresolveIntMax(ConstraintProto* ct, PresolveContext* context) {
  if (ct->int_max().vars().empty()) {
    return MarkConstraintAsFalse(ct, context);
  }
  const int target_ref = ct->int_max().target();

  // Pass 1, compute the infered min of the target, and remove duplicates.
  int64 target_min = context->MinOf(target_ref);
  int64 target_max = kint64min;
  bool contains_target_ref = false;
  std::set<int> used_ref;
  int new_size = 0;
  for (const int ref : ct->int_max().vars()) {
    if (ref == target_ref) contains_target_ref = true;
    if (gtl::ContainsKey(used_ref, ref)) continue;
    if (gtl::ContainsKey(used_ref, NegatedRef(ref)) ||
        ref == NegatedRef(target_ref)) {
      target_min = std::max(target_min, int64{0});
    }
    used_ref.insert(ref);
    ct->mutable_int_max()->set_vars(new_size++, ref);
    target_min = std::max(target_min, context->MinOf(ref));
    target_max = std::max(target_max, context->MaxOf(ref));
  }
  if (new_size < ct->int_max().vars_size()) {
    context->UpdateRuleStats("int_max: removed dup");
  }
  ct->mutable_int_max()->mutable_vars()->Truncate(new_size);
  if (contains_target_ref) {
    context->UpdateRuleStats("int_max: x = max(x, ...)");
    for (const int ref : ct->int_max().vars()) {
      if (ref == target_ref) continue;
      ConstraintProto* new_ct = context->working_model->add_constraints();
      *new_ct->mutable_enforcement_literal() = ct->enforcement_literal();
      auto* arg = new_ct->mutable_linear();
      arg->add_vars(target_ref);
      arg->add_coeffs(1);
      arg->add_vars(ref);
      arg->add_coeffs(-1);
      arg->add_domain(0);
      arg->add_domain(kint64max);
    }
    return RemoveConstraint(ct, context);
  }

  // Update the target domain.
  bool domain_reduced = false;
  if (!HasEnforcementLiteral(*ct)) {
    Domain infered_domain;
    for (const int ref : ct->int_max().vars()) {
      infered_domain = infered_domain.UnionWith(
          context->DomainOf(ref).IntersectionWith({target_min, target_max}));
    }
    domain_reduced |= context->IntersectDomainWith(target_ref, infered_domain);
  }

  // Pass 2, update the argument domains. Filter them eventually.
  new_size = 0;
  const int size = ct->int_max().vars_size();
  target_max = context->MaxOf(target_ref);
  for (const int ref : ct->int_max().vars()) {
    if (!HasEnforcementLiteral(*ct)) {
      domain_reduced |=
          context->IntersectDomainWith(ref, Domain(kint64min, target_max));
    }
    if (context->MaxOf(ref) >= target_min) {
      ct->mutable_int_max()->set_vars(new_size++, ref);
    }
  }
  if (domain_reduced) {
    context->UpdateRuleStats("int_max: reduced domains");
  }

  bool modified = false;
  if (new_size < size) {
    context->UpdateRuleStats("int_max: removed variables");
    ct->mutable_int_max()->mutable_vars()->Truncate(new_size);
    modified = true;
  }

  if (new_size == 0) {
    return MarkConstraintAsFalse(ct, context);
  }
  if (new_size == 1) {
    // Convert to an equality. Note that we create a new constraint otherwise it
    // might not be processed again.
    context->UpdateRuleStats("int_max: converted to equality");
    ConstraintProto* new_ct = context->working_model->add_constraints();
    *new_ct = *ct;  // copy name and potential reification.
    auto* arg = new_ct->mutable_linear();
    arg->add_vars(target_ref);
    arg->add_coeffs(1);
    arg->add_vars(ct->int_max().vars(0));
    arg->add_coeffs(-1);
    arg->add_domain(0);
    arg->add_domain(0);
    return RemoveConstraint(ct, context);
  }
  return modified;
}

bool PresolveIntMin(ConstraintProto* ct, PresolveContext* context) {
  const auto copy = ct->int_min();
  ct->mutable_int_max()->set_target(NegatedRef(copy.target()));
  for (const int ref : copy.vars()) {
    ct->mutable_int_max()->add_vars(NegatedRef(ref));
  }
  return PresolveIntMax(ct, context);
}

bool PresolveIntProd(ConstraintProto* ct, PresolveContext* context) {
  if (HasEnforcementLiteral(*ct)) return false;

  if (ct->int_prod().vars_size() == 2) {
    int a = ct->int_prod().vars(0);
    int b = ct->int_prod().vars(1);
    const int p = ct->int_prod().target();

    if (context->IsFixed(b)) std::swap(a, b);
    if (context->IsFixed(a)) {
      ConstraintProto* const lin = context->working_model->add_constraints();
      lin->mutable_linear()->add_vars(b);
      lin->mutable_linear()->add_coeffs(context->MinOf(a));
      lin->mutable_linear()->add_vars(p);
      lin->mutable_linear()->add_coeffs(-1);
      lin->mutable_linear()->add_domain(0);
      lin->mutable_linear()->add_domain(0);

      context->UpdateRuleStats("int_prod: linearize product by constant.");
      return RemoveConstraint(ct, context);
    }
  }

  // For now, we only presolve the case where all variables are Booleans.
  const int target_ref = ct->int_prod().target();
  if (!RefIsPositive(target_ref)) return false;
  for (const int var : ct->int_prod().vars()) {
    if (!RefIsPositive(var)) return false;
    if (context->MinOf(var) < 0) return false;
    if (context->MaxOf(var) > 1) return false;
  }

  // This is a bool constraint!
  context->IntersectDomainWith(target_ref, Domain(0, 1));
  context->UpdateRuleStats("int_prod: all Boolean.");
  {
    ConstraintProto* new_ct = context->working_model->add_constraints();
    new_ct->add_enforcement_literal(target_ref);
    auto* arg = new_ct->mutable_bool_and();
    for (const int var : ct->int_prod().vars()) {
      arg->add_literals(var);
    }
  }
  {
    ConstraintProto* new_ct = context->working_model->add_constraints();
    auto* arg = new_ct->mutable_bool_or();
    arg->add_literals(target_ref);
    for (const int var : ct->int_prod().vars()) {
      arg->add_literals(NegatedRef(var));
    }
  }
  return RemoveConstraint(ct, context);
}

bool PresolveIntDiv(ConstraintProto* ct, PresolveContext* context) {
  // For now, we only presolve the case where the divisor is constant.
  const int target = ct->int_div().target();
  const int ref_x = ct->int_div().vars(0);
  const int ref_div = ct->int_div().vars(1);
  if (!RefIsPositive(target) || !RefIsPositive(ref_x) ||
      !RefIsPositive(ref_div) || !context->IsFixed(ref_div)) {
    return false;
  }

  const int64 divisor = context->MinOf(ref_div);
  if (divisor == 1) {
    context->UpdateRuleStats("TODO int_div: rewrite to equality");
  }
  if (context->IntersectDomainWith(
          target, context->DomainOf(ref_x).DivisionBy(divisor))) {
    context->UpdateRuleStats(
        "int_div: updated domain of target in target = X / cte");
  }

  // TODO(user): reduce the domain of X by introducing an
  // InverseDivisionOfSortedDisjointIntervals().
  return false;
}

bool ExploitEquivalenceRelations(ConstraintProto* ct,
                                 PresolveContext* context) {
  if (gtl::ContainsKey(context->affine_constraints, ct)) return false;
  bool changed = false;

  // Remap equal and negated variables to their representative.
  ApplyToAllVariableIndices(
      [&changed, context](int* ref) {
        const int var = PositiveRef(*ref);
        const AffineRelation::Relation r =
            context->var_equiv_relations.Get(var);
        if (r.representative != var) {
          CHECK_EQ(r.offset, 0);
          CHECK_EQ(std::abs(r.coeff), 1);
          *ref = (r.coeff == 1) == RefIsPositive(*ref)
                     ? r.representative
                     : NegatedRef(r.representative);
          changed = true;
        }
      },
      ct);

  // Remap literal and negated literal to their representative.
  ApplyToAllLiteralIndices(
      [&changed, context](int* ref) {
        const int var = PositiveRef(*ref);
        const AffineRelation::Relation r = context->GetAffineRelation(var);
        if (r.representative == var) return;

        // Tricky: We might not have propagated the domain of the variables yet,
        // so we may have weird offset/coeff pair that will force one variable
        // to be fixed. This will be dealt with later, so we just handle the
        // two proper full mapping between [0, 1] variables here.
        const bool is_positive = (r.offset == 0 && r.coeff == 1);
        const bool is_negative = (r.offset == 1 && r.coeff == -1);
        if (is_positive || is_negative) {
          *ref = (is_positive == RefIsPositive(*ref))
                     ? r.representative
                     : NegatedRef(r.representative);
          changed = true;
        }
      },
      ct);
  return changed;
}

bool PresolveLinear(ConstraintProto* ct, PresolveContext* context) {
  bool var_constraint_graph_changed = false;
  Domain rhs = ReadDomainFromProto(ct->linear());

  // First regroup the terms on the same variables and sum the fixed ones.
  // Note that we use a map to sort the variables and because we expect most
  // constraints to be small.
  //
  // TODO(user): move the map in context to reuse its memory. Add a quick pass
  // to skip most of the work below if the constraint is already in canonical
  // form (strictly increasing var, no-fixed var, gcd = 1).
  int64 sum_of_fixed_terms = 0;
  std::map<int, int64> var_to_coeff;
  const LinearConstraintProto& arg = ct->linear();
  const bool was_affine = gtl::ContainsKey(context->affine_constraints, ct);
  for (int i = 0; i < arg.vars_size(); ++i) {
    const int var = PositiveRef(arg.vars(i));
    const int64 coeff =
        RefIsPositive(arg.vars(i)) ? arg.coeffs(i) : -arg.coeffs(i);
    if (coeff == 0) continue;
    if (context->IsFixed(var)) {
      sum_of_fixed_terms += coeff * context->MinOf(var);
      continue;
    }

    if (!was_affine) {
      const AffineRelation::Relation r = context->GetAffineRelation(var);
      if (r.representative != var) {
        var_constraint_graph_changed = true;
        sum_of_fixed_terms += coeff * r.offset;
      }
      var_to_coeff[r.representative] += coeff * r.coeff;
      if (var_to_coeff[r.representative] == 0) {
        var_to_coeff.erase(r.representative);
      }
    } else {
      var_to_coeff[var] += coeff;
      if (var_to_coeff[var] == 0) var_to_coeff.erase(var);
    }
  }

  // Test for singleton variable. Not that we need to do that after the
  // canonicalization of the constraint in case a variable was appearing more
  // than once.
  //
  // TODO(user): This trigger a bug in some rare case (run on radiation.fzn).
  // Investigate and fix.
  if (/* DISABLES CODE */ (false) && !was_affine) {
    std::vector<int> var_to_erase;
    for (const auto entry : var_to_coeff) {
      const int var = entry.first;
      const int64 coeff = entry.second;

      // Because we may have replaced the variable of this constraint by their
      // representative, the constraint count of var may not be up to date here
      // if var is part of an affine equivalence class.
      //
      // TODO(user): In some case, we could still remove var, but we also need
      // to not keep the affine relationship around in the constraint count.
      if (context->VariableIsUniqueAndRemovable(var) &&
          context->affine_relations.ClassSize(var) == 1) {
        bool success;
        const auto term_domain =
            context->DomainOf(var).MultiplicationBy(-coeff, &success);
        if (success) {
          // Note that we can't do that if we loose information in the
          // multiplication above because the new domain might not be as strict
          // as the initial constraint otherwise. TODO(user): because of the
          // addition, it might be possible to cover more cases though.
          var_to_erase.push_back(var);
          rhs = rhs.AdditionWith(term_domain);
          continue;
        }
      }
    }
    if (!var_to_erase.empty()) {
      for (const int var : var_to_erase) var_to_coeff.erase(var);
      context->UpdateRuleStats("linear: singleton column");
      // TODO(user): we could add the constraint to mapping_model only once
      // instead of adding a reduced version of it each time a new singleton
      // variable appear in the same constraint later. That would work but would
      // also force the postsolve to take search decisions...
      *(context->mapping_model->add_constraints()) = *ct;
    }
  }

  // Compute the GCD of all coefficients.
  int64 gcd = 1;
  bool first_coeff = true;
  for (const auto entry : var_to_coeff) {
    // GCD(gcd, coeff) = GCD(coeff, gcd % coeff);
    int64 coeff = std::abs(entry.second);
    if (first_coeff) {
      if (coeff != 0) {
        first_coeff = false;
        gcd = coeff;
      }
      continue;
    }
    while (coeff != 0) {
      const int64 r = gcd % coeff;
      gcd = coeff;
      coeff = r;
    }
    if (gcd == 1) break;
  }
  if (gcd > 1) {
    context->UpdateRuleStats("linear: divide by GCD");
  }

  if (var_to_coeff.size() < arg.vars_size()) {
    context->UpdateRuleStats("linear: fixed or dup variables");
    var_constraint_graph_changed = true;
  }

  // Rewrite the constraint in canonical form and update rhs (it will be copied
  // to the constraint later).
  if (sum_of_fixed_terms != 0) {
    rhs = rhs.AdditionWith({-sum_of_fixed_terms, -sum_of_fixed_terms});
  }
  if (gcd > 1) {
    rhs = rhs.InverseMultiplicationBy(gcd);
  }
  ct->mutable_linear()->clear_vars();
  ct->mutable_linear()->clear_coeffs();
  for (const auto entry : var_to_coeff) {
    CHECK(RefIsPositive(entry.first));
    ct->mutable_linear()->add_vars(entry.first);
    ct->mutable_linear()->add_coeffs(entry.second / gcd);
  }

  // Empty constraint?
  if (ct->linear().vars().empty()) {
    context->UpdateRuleStats("linear: empty");
    if (rhs.Contains(0)) {
      return RemoveConstraint(ct, context);
    } else {
      return MarkConstraintAsFalse(ct, context);
    }
  }

  // Size one constraint?
  if (ct->linear().vars().size() == 1 && !HasEnforcementLiteral(*ct)) {
    const int64 coeff =
        RefIsPositive(arg.vars(0)) ? arg.coeffs(0) : -arg.coeffs(0);
    context->UpdateRuleStats("linear: size one");
    const int var = PositiveRef(arg.vars(0));
    if (coeff == 1) {
      context->IntersectDomainWith(var, rhs);
    } else {
      DCHECK_EQ(coeff, -1);  // Because of the GCD above.
      context->IntersectDomainWith(var, rhs.Negation());
    }
    return RemoveConstraint(ct, context);
  }

  // Compute the implied rhs bounds from the variable ones.
  const int kDomainComplexityLimit = 100;
  auto& term_domains = context->tmp_term_domains;
  auto& left_domains = context->tmp_left_domains;
  const int num_vars = arg.vars_size();
  term_domains.resize(num_vars + 1);
  left_domains.resize(num_vars + 1);
  left_domains[0] = Domain(0);
  for (int i = 0; i < num_vars; ++i) {
    const int var = PositiveRef(arg.vars(i));
    const int64 coeff = arg.coeffs(i);

    // TODO(user): Try PreciseMultiplicationOfSortedDisjointIntervals() if
    // the size is reasonable.
    term_domains[i] = context->DomainOf(var).ContinuousMultiplicationBy(coeff);
    left_domains[i + 1] = left_domains[i].AdditionWith(term_domains[i]);
    if (left_domains[i + 1].NumIntervals() > kDomainComplexityLimit) {
      // We take a super-set, otherwise it will be too slow.
      //
      // TODO(user): We could be smarter in how we compute this if we allow for
      // more than one intervals.
      left_domains[i + 1] =
          Domain(left_domains[i + 1].Min(), left_domains[i + 1].Max());
    }
  }
  const Domain& implied_rhs = left_domains[num_vars];

  // Abort if intersection is empty.
  const Domain restricted_rhs = rhs.IntersectionWith(implied_rhs);
  if (restricted_rhs.IsEmpty()) {
    context->UpdateRuleStats("linear: infeasible");
    return MarkConstraintAsFalse(ct, context);
  }

  // Relax the constraint rhs for faster propagation.
  // TODO(user): add an IntersectionIsEmpty() function.
  std::vector<ClosedInterval> rhs_intervals;
  for (const ClosedInterval i :
       restricted_rhs.UnionWith(implied_rhs.Complement())) {
    if (!Domain::FromIntervals({i})
             .IntersectionWith(restricted_rhs)
             .IsEmpty()) {
      rhs_intervals.push_back(i);
    }
  }
  rhs = Domain::FromIntervals(rhs_intervals);
  if (rhs == Domain::AllValues()) {
    context->UpdateRuleStats("linear: always true");
    return RemoveConstraint(ct, context);
  }
  if (rhs != ReadDomainFromProto(ct->linear())) {
    context->UpdateRuleStats("linear: simplified rhs");
  }
  FillDomainInProto(rhs, ct->mutable_linear());

  // Propagate the variable bounds.
  if (!HasEnforcementLiteral(*ct)) {
    bool new_bounds = false;
    Domain new_domain;
    Domain right_domain(0, 0);
    term_domains[num_vars] = rhs.Negation();
    for (int i = num_vars - 1; i >= 0; --i) {
      right_domain = right_domain.AdditionWith(term_domains[i + 1]);
      if (right_domain.NumIntervals() > kDomainComplexityLimit) {
        // We take a super-set, otherwise it will be too slow.
        right_domain = Domain(right_domain.Min(), right_domain.Max());
      }
      new_domain = left_domains[i]
                       .AdditionWith(right_domain)
                       .InverseMultiplicationBy(-arg.coeffs(i));
      if (context->IntersectDomainWith(arg.vars(i), new_domain)) {
        new_bounds = true;
      }
    }
    if (new_bounds) {
      context->UpdateRuleStats("linear: reduced variable domains");
    }
  }

  // Detect affine relation.
  //
  // TODO(user): it might be better to first add only the affine relation with
  // a coefficient of magnitude 1, and later the one with larger coeffs.
  if (!was_affine && !HasEnforcementLiteral(*ct)) {
    const LinearConstraintProto& arg = ct->linear();
    const int64 rhs_min = rhs.Min();
    const int64 rhs_max = rhs.Max();
    if (rhs_min == rhs_max && arg.vars_size() == 2) {
      const int v1 = arg.vars(0);
      const int v2 = arg.vars(1);
      const int64 coeff1 = arg.coeffs(0);
      const int64 coeff2 = arg.coeffs(1);
      if (coeff1 == 1) {
        context->AddAffineRelation(*ct, v1, v2, -coeff2, rhs_max);
      } else if (coeff2 == 1) {
        context->AddAffineRelation(*ct, v2, v1, -coeff1, rhs_max);
      } else if (coeff1 == -1) {
        context->AddAffineRelation(*ct, v1, v2, coeff2, -rhs_max);
      } else if (coeff2 == -1) {
        context->AddAffineRelation(*ct, v2, v1, coeff1, -rhs_max);
      }
    }
  }
  return var_constraint_graph_changed;
}

// Fixes the variable at 'var_index' to 'fixed_value' in the constraint and
// returns the modified RHS Domain.
Domain FixVariableInLinearConstraint(const int var_index,
                                     const int64 fixed_value,
                                     ConstraintProto* ct,
                                     PresolveContext* context) {
  auto* arg = ct->mutable_linear();
  const int num_vars = arg->vars_size();
  CHECK_LT(var_index, num_vars);
  const int ref = arg->vars(var_index);
  CHECK(context->DomainOf(ref).Contains(fixed_value));
  const int64 coeff = arg->coeffs(var_index);
  // Subtract the fixed term from the domain.
  const Domain term_domain(coeff * fixed_value);
  const Domain rhs_domain = ReadDomainFromProto(ct->linear());
  const Domain new_rhs_domain = rhs_domain.AdditionWith(term_domain.Negation());
  std::vector<std::pair<int, int64>> constraint_entries;
  // Copy coefficients of all variables except the fixed one.
  for (int i = 0; i < num_vars; ++i) {
    if (i == var_index) continue;
    constraint_entries.push_back({arg->vars(i), arg->coeffs(i)});
  }
  arg->clear_coeffs();
  arg->clear_vars();
  for (int i = 0; i < constraint_entries.size(); ++i) {
    arg->add_vars(constraint_entries[i].first);
    arg->add_coeffs(constraint_entries[i].second);
  }
  FillDomainInProto(new_rhs_domain, arg);
  return new_rhs_domain;
}

// Identify Boolean variable that makes the constraint always true when set to
// true or false. Moves such literal to the constraint enforcement literals
// list.
//
// This operation is similar to coefficient strengthening in the MIP world.
void ExtractEnforcementLiteralFromLinearConstraint(ConstraintProto* ct,
                                                   PresolveContext* context) {
  Domain rhs_domain = ReadDomainFromProto(ct->linear());

  if (rhs_domain.NumIntervals() != 1) return;

  // Return early if the constraint has both bounds. This is because in
  // PresolveLinear() we relax the rhs domain, and after this operation, if
  // we have two finite bounds, then there can be no literal that will make
  // the constraint always true.
  if (rhs_domain.Min() != kint64min && rhs_domain.Max() != kint64max) return;

  const LinearConstraintProto& arg = ct->linear();
  const int num_vars = arg.vars_size();
  int64 min_sum = 0;
  int64 max_sum = 0;
  for (int i = 0; i < num_vars; ++i) {
    const int ref = arg.vars(i);
    const int64 coeff = arg.coeffs(i);
    const int64 term_a = coeff * context->MinOf(ref);
    const int64 term_b = coeff * context->MaxOf(ref);
    min_sum += std::min(term_a, term_b);
    max_sum += std::max(term_a, term_b);
  }
  for (int i = 0; i < arg.vars_size(); ++i) {
    // Only work with binary variables.
    //
    // TODO(user, krunalp): This could be generalized to non-binary variable
    // but that would require introducing the encoding "literal <=> integer
    // variable at is min/max" and using this literal in the enforcement list.
    // It is thus a bit more involved, and might not be as useful.
    const int ref = arg.vars(i);
    if (context->MinOf(ref) != 0) continue;
    if (context->MaxOf(ref) != 1) continue;
    const int64 coeff = arg.coeffs(i);
    if (rhs_domain.Max() != kint64max) {
      DCHECK_EQ(rhs_domain.Min(), kint64min);
      if (max_sum - std::abs(coeff) <= rhs_domain.Max()) {
        if (coeff > 0) {
          // Fix the variable to 1 in the constraint and add it as enforcement
          // literal.
          rhs_domain = FixVariableInLinearConstraint(i, 1, ct, context);
          ct->add_enforcement_literal(ref);
          // 'min_sum' remains unaffected.
          max_sum -= coeff;
        } else {
          // Fix the variable to 0 in the constraint and add its negation as
          // enforcement literal.
          rhs_domain = FixVariableInLinearConstraint(i, 0, ct, context);
          ct->add_enforcement_literal(NegatedRef(ref));
          // 'max_sum' remains unaffected.
          min_sum -= coeff;
        }
        context->UpdateRuleStats(
            "linear: extracted enforcement literal from constraint");
        --i;
        continue;
      }
    } else {
      DCHECK_NE(rhs_domain.Min(), kint64min);
      DCHECK_EQ(rhs_domain.Max(), kint64max);

      if (min_sum + std::abs(coeff) >= rhs_domain.Min()) {
        if (coeff > 0) {
          // Fix the variable to 0 in the constraint and add its negation as
          // enforcement literal.
          rhs_domain = FixVariableInLinearConstraint(i, 0, ct, context);
          ct->add_enforcement_literal(NegatedRef(ref));
          // 'min_sum' remains unaffected.
          max_sum -= coeff;
        } else {
          // Fix the variable to 1 in the constraint and add it as enforcement
          // literal.
          rhs_domain = FixVariableInLinearConstraint(i, 1, ct, context);
          ct->add_enforcement_literal(ref);
          // 'max_sum' remains unaffected.
          min_sum -= coeff;
        }
        context->UpdateRuleStats(
            "linear: extracted enforcement literal from constraint");
        --i;
        continue;
      }
    }
  }
}

void ExtractAtMostOneFromLinear(ConstraintProto* ct, PresolveContext* context) {
  if (HasEnforcementLiteral(*ct)) return;
  const Domain domain = ReadDomainFromProto(ct->linear());

  const LinearConstraintProto& arg = ct->linear();
  const int num_vars = arg.vars_size();
  int64 min_sum = 0;
  int64 max_sum = 0;
  for (int i = 0; i < num_vars; ++i) {
    const int ref = arg.vars(i);
    const int64 coeff = arg.coeffs(i);
    const int64 term_a = coeff * context->MinOf(ref);
    const int64 term_b = coeff * context->MaxOf(ref);
    min_sum += std::min(term_a, term_b);
    max_sum += std::max(term_a, term_b);
  }
  for (const int type : {0, 1}) {
    std::vector<int> at_most_one;
    for (int i = 0; i < num_vars; ++i) {
      const int ref = arg.vars(i);
      const int64 coeff = arg.coeffs(i);
      if (context->MinOf(ref) != 0) continue;
      if (context->MaxOf(ref) != 1) continue;

      if (type == 0) {
        // TODO(user): we could potentially add one more Boolean with a lower
        // coeff as long as we have lower_coeff + min_of_other_coeff >
        // domain.Max().
        if (min_sum + 2 * std::abs(coeff) > domain.Max()) {
          at_most_one.push_back(coeff > 0 ? ref : NegatedRef(ref));
        }
      } else {
        if (max_sum - 2 * std::abs(coeff) < domain.Min()) {
          at_most_one.push_back(coeff > 0 ? NegatedRef(ref) : ref);
        }
      }
    }
    if (at_most_one.size() > 1) {
      if (type == 0) {
        context->UpdateRuleStats("linear: extracted at most one (max).");
      } else {
        context->UpdateRuleStats("linear: extracted at most one (min).");
      }
      ConstraintProto* new_ct = context->working_model->add_constraints();
      for (const int ref : at_most_one) {
        new_ct->mutable_at_most_one()->add_literals(ref);
      }
    }
  }
}

// Convert some linear constraint involving only Booleans to their Boolean
// form.
bool PresolveLinearOnBooleans(ConstraintProto* ct, PresolveContext* context) {
  // TODO(user): the alternative to mark any newly created constraints might
  // be better.
  if (gtl::ContainsKey(context->affine_constraints, ct)) return false;
  const LinearConstraintProto& arg = ct->linear();
  const int num_vars = arg.vars_size();
  int64 min_coeff = kint64max;
  int64 max_coeff = 0;
  int64 min_sum = 0;
  int64 max_sum = 0;
  for (int i = 0; i < num_vars; ++i) {
    // We assume we already ran PresolveLinear().
    const int var = arg.vars(i);
    const int64 coeff = arg.coeffs(i);
    CHECK(RefIsPositive(var));
    CHECK_NE(coeff, 0);
    if (context->MinOf(var) != 0) return false;
    if (context->MaxOf(var) != 1) return false;

    if (coeff > 0) {
      max_sum += coeff;
      min_coeff = std::min(min_coeff, coeff);
      max_coeff = std::min(max_coeff, coeff);
    } else {
      // We replace the Boolean ref, by a ref to its negation (1 - x).
      min_sum += coeff;
      min_coeff = std::min(min_coeff, -coeff);
      max_coeff = std::min(max_coeff, -coeff);
    }
  }

  // Detect clauses, reified ands, at_most_one.
  //
  // TODO(user): split a == 1 constraint or similar into a clause and an at
  // most one constraint?
  const Domain domain = ReadDomainFromProto(arg);
  DCHECK(!domain.IsEmpty());
  if (min_sum + min_coeff > domain.Max()) {
    // All Boolean are false if the reified literal is true.
    context->UpdateRuleStats("linear: negative reified and");
    const auto copy = arg;
    ct->mutable_bool_and()->clear_literals();
    for (int i = 0; i < num_vars; ++i) {
      ct->mutable_bool_and()->add_literals(
          copy.coeffs(i) > 0 ? NegatedRef(copy.vars(i)) : copy.vars(i));
    }
    return PresolveBoolAnd(ct, context);
  } else if (max_sum - min_coeff < domain.Min()) {
    // All Boolean are true if the reified literal is true.
    context->UpdateRuleStats("linear: positive reified and");
    const auto copy = arg;
    ct->mutable_bool_and()->clear_literals();
    for (int i = 0; i < num_vars; ++i) {
      ct->mutable_bool_and()->add_literals(
          copy.coeffs(i) > 0 ? copy.vars(i) : NegatedRef(copy.vars(i)));
    }
    return PresolveBoolAnd(ct, context);
  } else if (min_sum + min_coeff >= domain.Min() &&
             domain.front().end == kint64max) {
    // At least one Boolean is true.
    context->UpdateRuleStats("linear: positive clause");
    const auto copy = arg;
    ct->mutable_bool_or()->clear_literals();
    for (int i = 0; i < num_vars; ++i) {
      ct->mutable_bool_or()->add_literals(
          copy.coeffs(i) > 0 ? copy.vars(i) : NegatedRef(copy.vars(i)));
    }
    return PresolveBoolOr(ct, context);
  } else if (max_sum - min_coeff <= domain.Max() &&
             domain.back().start == kint64min) {
    // At least one Boolean is false.
    context->UpdateRuleStats("linear: negative clause");
    const auto copy = arg;
    ct->mutable_bool_or()->clear_literals();
    for (int i = 0; i < num_vars; ++i) {
      ct->mutable_bool_or()->add_literals(
          copy.coeffs(i) > 0 ? NegatedRef(copy.vars(i)) : copy.vars(i));
    }
    return PresolveBoolOr(ct, context);
  } else if (!HasEnforcementLiteral(*ct) &&
             min_sum + max_coeff <= domain.Max() &&
             min_sum + 2 * min_coeff > domain.Max() &&
             domain.back().start == kint64min) {
    // At most one Boolean is true.
    context->UpdateRuleStats("linear: positive at most one");
    const auto copy = arg;
    ct->mutable_at_most_one()->clear_literals();
    for (int i = 0; i < num_vars; ++i) {
      ct->mutable_at_most_one()->add_literals(
          copy.coeffs(i) > 0 ? copy.vars(i) : NegatedRef(copy.vars(i)));
    }
    return true;
  } else if (!HasEnforcementLiteral(*ct) &&
             max_sum - max_coeff >= domain.Min() &&
             max_sum - 2 * min_coeff < domain.Min() &&
             domain.front().end == kint64max) {
    // At most one Boolean is false.
    context->UpdateRuleStats("linear: negative at most one");
    const auto copy = arg;
    ct->mutable_at_most_one()->clear_literals();
    for (int i = 0; i < num_vars; ++i) {
      ct->mutable_at_most_one()->add_literals(
          copy.coeffs(i) > 0 ? NegatedRef(copy.vars(i)) : copy.vars(i));
    }
    return true;
  }

  // Expand small expression into clause.
  //
  // TODO(user): This is bad from a LP relaxation perspective. Do not do that
  // now? On another hand it is good for the SAT presolving.
  if (num_vars > 3) return false;
  context->UpdateRuleStats("linear: small Boolean expression");

  // Enumerate all possible value of the Booleans and add a clause if constraint
  // is false. TODO(user): the encoding could be made better in some cases.
  const int max_mask = (1 << arg.vars_size());
  for (int mask = 0; mask < max_mask; ++mask) {
    int64 value = 0;
    for (int i = 0; i < num_vars; ++i) {
      if ((mask >> i) & 1) value += arg.coeffs(i);
    }
    if (domain.Contains(value)) continue;

    // Add a new clause to exclude this bad assignment.
    ConstraintProto* new_ct = context->working_model->add_constraints();
    auto* new_arg = new_ct->mutable_bool_or();
    if (HasEnforcementLiteral(*ct)) {
      *new_ct->mutable_enforcement_literal() = ct->enforcement_literal();
    }
    for (int i = 0; i < num_vars; ++i) {
      new_arg->add_literals(((mask >> i) & 1) ? NegatedRef(arg.vars(i))
                                              : arg.vars(i));
    }
  }

  return RemoveConstraint(ct, context);
}

bool PresolveInterval(ConstraintProto* ct, PresolveContext* context) {
  if (!ct->enforcement_literal().empty()) return false;
  const int start = ct->interval().start();
  const int end = ct->interval().end();
  const int size = ct->interval().size();
  bool changed = false;
  changed |= context->IntersectDomainWith(
      end, context->DomainOf(start).AdditionWith(context->DomainOf(size)));
  changed |= context->IntersectDomainWith(
      start,
      context->DomainOf(end).AdditionWith(context->DomainOf(size).Negation()));
  changed |= context->IntersectDomainWith(
      size,
      context->DomainOf(end).AdditionWith(context->DomainOf(start).Negation()));
  if (changed) {
    context->UpdateRuleStats("interval: reduced domains");
  }

  // TODO(user): This currently has a side effect that both the interval and
  // a linear constraint are added to the presolved model. Fix.
  if (false && context->IsFixed(size)) {
    // We add it even if the interval is optional.
    // TODO(user): we must verify that all the variable of an optional interval
    // do not appear in a constraint which is not reified by the same literal.
    context->AddAffineRelation(*ct, ct->interval().end(),
                               ct->interval().start(), 1, context->MinOf(size));
  }

  // This never change the constraint-variable graph.
  return false;
}

bool PresolveElement(ConstraintProto* ct, PresolveContext* context) {
  const int index_ref = ct->element().index();
  const int target_ref = ct->element().target();

  // TODO(user): think about this once we do have such constraint.
  if (HasEnforcementLiteral(*ct)) return false;

  int num_vars = 0;
  bool all_constants = true;
  absl::flat_hash_set<int64> constant_set;

  bool all_included_in_target_domain = true;
  bool reduced_index_domain = false;
  if (context->IntersectDomainWith(index_ref,
                                   Domain(0, ct->element().vars_size() - 1))) {
    reduced_index_domain = true;
  }

  Domain infered_domain;
  const Domain target_domain = context->DomainOf(target_ref);
  for (const ClosedInterval interval : context->DomainOf(index_ref)) {
    for (int value = interval.start; value <= interval.end; ++value) {
      CHECK_GE(value, 0);
      CHECK_LT(value, ct->element().vars_size());
      const int ref = ct->element().vars(value);
      const Domain domain = context->DomainOf(ref);
      if (domain.IntersectionWith(target_domain).IsEmpty()) {
        context->IntersectDomainWith(index_ref, Domain(value).Complement());
        reduced_index_domain = true;
      } else {
        ++num_vars;
        if (domain.Min() == domain.Max()) {
          constant_set.insert(domain.Min());
        } else {
          all_constants = false;
        }
        if (!domain.IsIncludedIn(target_domain)) {
          all_included_in_target_domain = false;
        }
        infered_domain = infered_domain.UnionWith(domain);
      }
    }
  }
  if (reduced_index_domain) {
    context->UpdateRuleStats("element: reduced index domain");
  }
  if (context->IntersectDomainWith(target_ref, infered_domain)) {
    if (context->DomainOf(target_ref).IsEmpty()) return true;
    context->UpdateRuleStats("element: reduced target domain");
  }

  const bool unique_index = context->VariableIsUniqueAndRemovable(index_ref) ||
                            context->IsFixed(index_ref);
  if (all_constants && unique_index) {
    // This constraint is just here to reduce the domain of the target! We can
    // add it to the mapping_model to reconstruct the index value during
    // postsolve and get rid of it now.
    context->UpdateRuleStats("element: trivial target domain reduction");
    *(context->mapping_model->add_constraints()) = *ct;
    return RemoveConstraint(ct, context);
  }

  const bool unique_target =
      context->VariableIsUniqueAndRemovable(target_ref) ||
      context->IsFixed(target_ref);
  if (all_included_in_target_domain && unique_target) {
    context->UpdateRuleStats("element: trivial index domain reduction");
    *(context->mapping_model->add_constraints()) = *ct;
    return RemoveConstraint(ct, context);
  }

  if (all_constants && num_vars == constant_set.size()) {
    // TODO(user): We should be able to do something for simple mapping.
    context->UpdateRuleStats("TODO element: one to one mapping");
  }
  if (unique_target) {
    context->UpdateRuleStats("TODO element: target not used elsewhere");
  }
  if (context->IsFixed(index_ref)) {
    context->UpdateRuleStats("TODO element: fixed index.");
  } else if (unique_index) {
    context->UpdateRuleStats("TODO element: index not used elsewhere");
  }

  return false;
}

bool PresolveTable(ConstraintProto* ct, PresolveContext* context) {
  if (HasEnforcementLiteral(*ct)) return false;
  if (ct->table().negated()) return false;
  if (ct->table().vars().empty()) {
    context->UpdateRuleStats("table: empty constraint");
    return RemoveConstraint(ct, context);
  }

  // Filter the unreachable tuples.
  //
  // TODO(user): this is not supper efficient. Optimize if needed.
  const int num_vars = ct->table().vars_size();
  const int num_tuples = ct->table().values_size() / num_vars;
  std::vector<int64> tuple(num_vars);
  std::vector<std::vector<int64>> new_tuples;
  new_tuples.reserve(num_tuples);
  std::vector<absl::flat_hash_set<int64>> new_domains(num_vars);
  for (int i = 0; i < num_tuples; ++i) {
    bool delete_row = false;
    std::string tmp;
    for (int j = 0; j < num_vars; ++j) {
      const int ref = ct->table().vars(j);
      const int64 v = ct->table().values(i * num_vars + j);
      tuple[j] = v;
      if (!context->DomainOf(ref).Contains(v)) {
        delete_row = true;
        break;
      }
    }
    if (delete_row) continue;
    new_tuples.push_back(tuple);
    for (int j = 0; j < num_vars; ++j) {
      const int ref = ct->table().vars(j);
      const int64 v = tuple[j];
      new_domains[j].insert(RefIsPositive(ref) ? v : -v);
    }
  }
  gtl::STLSortAndRemoveDuplicates(&new_tuples);

  // Update the list of tuples if needed.
  if (new_tuples.size() < num_tuples) {
    ct->mutable_table()->clear_values();
    for (const std::vector<int64>& t : new_tuples) {
      for (const int64 v : t) {
        ct->mutable_table()->add_values(v);
      }
    }
    context->UpdateRuleStats("table: removed rows");
  }

  // Filter the variable domains.
  bool changed = false;
  for (int j = 0; j < num_vars; ++j) {
    const int ref = ct->table().vars(j);
    changed |= context->IntersectDomainWith(
        PositiveRef(ref), Domain::FromValues(std::vector<int64>(
                              new_domains[j].begin(), new_domains[j].end())));
  }
  if (changed) {
    context->UpdateRuleStats("table: reduced variable domains");
  }
  if (num_vars == 1) {
    // Now that we properly update the domain, we can remove the constraint.
    context->UpdateRuleStats("table: only one column!");
    return RemoveConstraint(ct, context);
  }

  // Check that the table is not complete or just here to exclude a few tuples.
  double prod = 1.0;
  for (int j = 0; j < num_vars; ++j) prod *= new_domains[j].size();
  if (prod == new_tuples.size()) {
    context->UpdateRuleStats("table: all tuples!");
    return RemoveConstraint(ct, context);
  }

  // Convert to the negated table if we gain a lot of entries by doing so.
  // Note however that currently the negated table do not propagate as much as
  // it could.
  if (new_tuples.size() > 0.7 * prod) {
    // Enumerate all tuples.
    std::vector<std::vector<int64>> var_to_values(num_vars);
    for (int j = 0; j < num_vars; ++j) {
      var_to_values[j].assign(new_domains[j].begin(), new_domains[j].end());
    }
    std::vector<std::vector<int64>> all_tuples(prod);
    for (int i = 0; i < prod; ++i) {
      all_tuples[i].resize(num_vars);
      int index = i;
      for (int j = 0; j < num_vars; ++j) {
        all_tuples[i][j] = var_to_values[j][index % var_to_values[j].size()];
        index /= var_to_values[j].size();
      }
    }
    gtl::STLSortAndRemoveDuplicates(&all_tuples);

    // Compute the complement of new_tuples.
    std::vector<std::vector<int64>> diff(prod - new_tuples.size());
    std::set_difference(all_tuples.begin(), all_tuples.end(),
                        new_tuples.begin(), new_tuples.end(), diff.begin());

    // Negate the constraint.
    ct->mutable_table()->set_negated(!ct->table().negated());
    ct->mutable_table()->clear_values();
    for (const std::vector<int64>& t : diff) {
      for (const int64 v : t) ct->mutable_table()->add_values(v);
    }
    context->UpdateRuleStats("table: negated");
  }
  return false;
}

bool PresolveAllDiff(ConstraintProto* ct, PresolveContext* context) {
  if (HasEnforcementLiteral(*ct)) return false;
  const int size = ct->all_diff().vars_size();
  if (size == 0) {
    context->UpdateRuleStats("all_diff: empty constraint");
    return RemoveConstraint(ct, context);
  }
  if (size == 1) {
    context->UpdateRuleStats("all_diff: only one variable");
    return RemoveConstraint(ct, context);
  }

  bool contains_fixed_variable = false;
  for (int i = 0; i < size; ++i) {
    if (context->IsFixed(ct->all_diff().vars(i))) {
      contains_fixed_variable = true;
      break;
    }
  }
  if (contains_fixed_variable) {
    context->UpdateRuleStats("TODO all_diff: fixed variables");
  }
  return false;
}

bool PresolveNoOverlap(ConstraintProto* ct, PresolveContext* context) {
  const NoOverlapConstraintProto& proto = ct->no_overlap();

  // Filter absent intervals.
  int new_size = 0;
  for (int i = 0; i < proto.intervals_size(); ++i) {
    const int interval_index = proto.intervals(i);
    if (context->working_model->constraints(interval_index).constraint_case() ==
        ConstraintProto::ConstraintCase::CONSTRAINT_NOT_SET) {
      continue;
    }
    ct->mutable_no_overlap()->set_intervals(new_size++, interval_index);
  }
  ct->mutable_no_overlap()->mutable_intervals()->Truncate(new_size);

  if (proto.intervals_size() == 1) {
    context->UpdateRuleStats("no_overlap: only one interval");
    return RemoveConstraint(ct, context);
  }
  if (proto.intervals().empty()) {
    context->UpdateRuleStats("no_overlap: no intervals");
    return RemoveConstraint(ct, context);
  }
  return false;
}

bool PresolveCumulative(ConstraintProto* ct, PresolveContext* context) {
  const CumulativeConstraintProto& proto = ct->cumulative();

  // Filter absent intervals.
  int new_size = 0;
  bool changed = false;
  for (int i = 0; i < proto.intervals_size(); ++i) {
    if (context->working_model->constraints(proto.intervals(i))
            .constraint_case() ==
        ConstraintProto::ConstraintCase::CONSTRAINT_NOT_SET) {
      continue;
    }
    ct->mutable_cumulative()->set_intervals(new_size, proto.intervals(i));
    ct->mutable_cumulative()->set_demands(new_size, proto.demands(i));
    new_size++;
  }
  if (new_size < proto.intervals_size()) {
    changed = true;
    ct->mutable_cumulative()->mutable_intervals()->Truncate(new_size);
    ct->mutable_cumulative()->mutable_demands()->Truncate(new_size);
  }

  if (HasEnforcementLiteral(*ct)) return false;
  if (!context->IsFixed(proto.capacity())) return false;
  const int64 capacity = context->MinOf(proto.capacity());

  const int size = proto.intervals_size();
  std::vector<int> start_indices(size, -1);

  int num_duration_one = 0;
  int num_greater_half_capacity = 0;

  bool has_optional_interval = false;
  for (int i = 0; i < size; ++i) {
    // TODO(user): adapt in the presence of optional intervals.
    const ConstraintProto& ct =
        context->working_model->constraints(proto.intervals(i));
    if (!ct.enforcement_literal().empty()) has_optional_interval = true;
    const IntervalConstraintProto& interval = ct.interval();
    start_indices[i] = interval.start();
    const int duration_ref = interval.size();
    const int demand_ref = proto.demands(i);
    if (context->IsFixed(duration_ref) && context->MinOf(duration_ref) == 1) {
      num_duration_one++;
    }
    if (context->MinOf(duration_ref) == 0) {
      // The behavior for zero-duration interval is currently not the same in
      // the no-overlap and the cumulative constraint.
      return false;
    }
    const int64 demand_min = context->MinOf(demand_ref);
    const int64 demand_max = context->MaxOf(demand_ref);
    if (demand_min > capacity / 2) {
      num_greater_half_capacity++;
    }
    if (demand_min > capacity) {
      context->UpdateRuleStats("cumulative: demand_min exceeds capacity");
      if (ct.enforcement_literal().empty()) {
        context->is_unsat = true;
        return false;
      } else {
        CHECK_EQ(ct.enforcement_literal().size(), 1);
        context->SetLiteralToFalse(ct.enforcement_literal(0));
      }
      return false;
    } else if (demand_max > capacity) {
      if (ct.enforcement_literal().empty()) {
        context->UpdateRuleStats("cumulative: demand_max exceeds capacity.");
        context->IntersectDomainWith(demand_ref, Domain(kint64min, capacity));
      } else {
        // TODO(user): we abort because we cannot convert this to a no_overlap
        // for instance.
        context->UpdateRuleStats(
            "cumulative: demand_max of optional interval exceeds capacity.");
        return false;
      }
    }
  }

  if (num_greater_half_capacity == size) {
    if (num_duration_one == size && !has_optional_interval) {
      context->UpdateRuleStats("cumulative: convert to all_different");
      ConstraintProto* new_ct = context->working_model->add_constraints();
      auto* arg = new_ct->mutable_all_diff();
      for (const int var : start_indices) {
        arg->add_vars(var);
      }
      return RemoveConstraint(ct, context);
    } else {
      context->UpdateRuleStats("cumulative: convert to no_overlap");
      ConstraintProto* new_ct = context->working_model->add_constraints();
      auto* arg = new_ct->mutable_no_overlap();
      for (const int interval : proto.intervals()) {
        arg->add_intervals(interval);
      }
      return RemoveConstraint(ct, context);
    }
  }

  return changed;
}

bool PresolveCircuit(ConstraintProto* ct, PresolveContext* context) {
  if (HasEnforcementLiteral(*ct)) return false;
  CircuitConstraintProto& proto = *ct->mutable_circuit();

  // Convert the flat structure to a graph, note that we includes all the arcs
  // here (even if they are at false).
  std::vector<std::vector<int>> incoming_arcs;
  std::vector<std::vector<int>> outgoing_arcs;
  const int num_arcs = proto.literals_size();
  int num_nodes = 0;
  for (int i = 0; i < num_arcs; ++i) {
    const int ref = proto.literals(i);
    const int tail = proto.tails(i);
    const int head = proto.heads(i);
    num_nodes = std::max(num_nodes, std::max(tail, head) + 1);
    if (std::max(tail, head) >= incoming_arcs.size()) {
      incoming_arcs.resize(std::max(tail, head) + 1);
      outgoing_arcs.resize(std::max(tail, head) + 1);
    }
    incoming_arcs[head].push_back(ref);
    outgoing_arcs[tail].push_back(ref);
  }

  int num_fixed_at_true = 0;
  for (const auto* node_to_refs : {&incoming_arcs, &outgoing_arcs}) {
    for (const std::vector<int>& refs : *node_to_refs) {
      if (refs.size() == 1) {
        if (!context->LiteralIsTrue(refs.front())) {
          ++num_fixed_at_true;
          context->SetLiteralToTrue(refs.front());
        }
        continue;
      }

      // At most one true, so if there is one, mark all the other to false.
      int num_true = 0;
      int true_ref;
      for (const int ref : refs) {
        if (context->LiteralIsTrue(ref)) {
          ++num_true;
          true_ref = ref;
          break;
        }
      }
      if (num_true > 0) {
        for (const int ref : refs) {
          if (ref != true_ref) {
            context->SetLiteralToFalse(ref);
          }
        }
      }
    }
  }
  if (num_fixed_at_true > 0) {
    context->UpdateRuleStats("circuit: fixed singleton arcs.");
  }

  // Remove false arcs.
  int new_size = 0;
  int num_true = 0;
  int circuit_start = -1;
  std::vector<int> next(num_nodes, -1);
  std::vector<int> new_in_degree(num_nodes, 0);
  std::vector<int> new_out_degree(num_nodes, 0);
  for (int i = 0; i < num_arcs; ++i) {
    const int ref = proto.literals(i);
    if (context->LiteralIsFalse(ref)) continue;
    if (context->LiteralIsTrue(ref)) {
      if (next[proto.tails(i)] != -1) {
        context->is_unsat = true;
        return true;
      }
      next[proto.tails(i)] = proto.heads(i);
      if (proto.tails(i) != proto.heads(i)) {
        circuit_start = proto.tails(i);
      }
      ++num_true;
    }
    ++new_out_degree[proto.tails(i)];
    ++new_in_degree[proto.heads(i)];
    proto.set_tails(new_size, proto.tails(i));
    proto.set_heads(new_size, proto.heads(i));
    proto.set_literals(new_size, proto.literals(i));
    ++new_size;
  }

  // Detect infeasibility due to a node having no more incoming or outgoing arc.
  // This is a bit tricky because for now the meaning of the constraint says
  // that all nodes that appear in at least one of the arcs must be in the
  // circuit or have a self-arc. So if any such node ends up with an incoming or
  // outgoing degree of zero once we remove false arcs then the constraint is
  // infeasible!
  for (int i = 0; i < num_nodes; ++i) {
    // Skip initially ignored node.
    if (incoming_arcs[i].empty() && outgoing_arcs[i].empty()) continue;

    if (new_in_degree[i] == 0 || new_out_degree[i] == 0) {
      context->is_unsat = true;
      return true;
    }
  }

  // Test if a subcircuit is already present.
  if (circuit_start != -1) {
    std::vector<bool> visited(num_nodes, false);
    int current = circuit_start;
    while (current != -1 && !visited[current]) {
      visited[current] = true;
      current = next[current];
    }
    if (current == circuit_start) {
      // We have a sub-circuit! mark all other arc false except self-loop not in
      // circuit.
      for (int i = 0; i < num_arcs; ++i) {
        if (visited[proto.tails(i)]) continue;
        if (proto.tails(i) == proto.heads(i)) {
          context->SetLiteralToTrue(proto.literals(i));
        } else {
          context->SetLiteralToFalse(proto.literals(i));
        }
      }
      context->UpdateRuleStats("circuit: fully specified.");
      return RemoveConstraint(ct, context);
    }
  } else {
    // All self loop?
    if (num_true == new_size) {
      context->UpdateRuleStats("circuit: empty circuit.");
      return RemoveConstraint(ct, context);
    }
  }

  // Look for in/out-degree of two, this will imply that one of the indicator
  // Boolean is equal to the negation of the other.
  for (int i = 0; i < num_nodes; ++i) {
    for (const std::vector<int>* arc_literals :
         {&incoming_arcs[i], &outgoing_arcs[i]}) {
      std::vector<int> literals;
      for (const int ref : *arc_literals) {
        if (context->LiteralIsFalse(ref)) continue;
        if (context->LiteralIsTrue(ref)) {
          literals.clear();
          break;
        }
        literals.push_back(ref);
      }
      if (literals.size() == 2 && literals[0] != NegatedRef(literals[1])) {
        context->UpdateRuleStats("circuit: degree 2");
        context->AddBooleanEqualityRelation(literals[0],
                                            NegatedRef(literals[1]));
      }
    }
  }

  // Truncate the circuit and return.
  if (new_size < num_arcs) {
    proto.mutable_tails()->Truncate(new_size);
    proto.mutable_heads()->Truncate(new_size);
    proto.mutable_literals()->Truncate(new_size);
    context->UpdateRuleStats("circuit: removed false arcs.");
    return true;
  }
  return false;
}

template <typename ClauseContainer>
void ExtractClauses(const ClauseContainer& container, CpModelProto* proto) {
  // We regroup the "implication" into bool_and to have a more consise proto and
  // also for nicer information about the number of binary clauses.
  absl::flat_hash_map<int, int> ref_to_bool_and;
  for (int i = 0; i < container.NumClauses(); ++i) {
    const std::vector<Literal>& clause = container.Clause(i);
    if (clause.empty()) continue;

    // bool_and.
    if (clause.size() == 2) {
      const int a = clause[0].IsPositive()
                        ? clause[0].Variable().value()
                        : NegatedRef(clause[0].Variable().value());
      const int b = clause[1].IsPositive()
                        ? clause[1].Variable().value()
                        : NegatedRef(clause[1].Variable().value());
      if (gtl::ContainsKey(ref_to_bool_and, NegatedRef(a))) {
        const int ct_index = ref_to_bool_and[NegatedRef(a)];
        proto->mutable_constraints(ct_index)->mutable_bool_and()->add_literals(
            b);
      } else if (gtl::ContainsKey(ref_to_bool_and, NegatedRef(b))) {
        const int ct_index = ref_to_bool_and[NegatedRef(b)];
        proto->mutable_constraints(ct_index)->mutable_bool_and()->add_literals(
            a);
      } else {
        ref_to_bool_and[NegatedRef(a)] = proto->constraints_size();
        ConstraintProto* ct = proto->add_constraints();
        ct->add_enforcement_literal(NegatedRef(a));
        ct->mutable_bool_and()->add_literals(b);
      }
      continue;
    }

    // bool_or.
    ConstraintProto* ct = proto->add_constraints();
    for (const Literal l : clause) {
      if (l.IsPositive()) {
        ct->mutable_bool_or()->add_literals(l.Variable().value());
      } else {
        ct->mutable_bool_or()->add_literals(NegatedRef(l.Variable().value()));
      }
    }
  }
}

void Probe(TimeLimit* global_time_limit, PresolveContext* context) {
  if (context->is_unsat) return;

  // Update the domain in the current CpModelProto.
  for (int i = 0; i < context->working_model->variables_size(); ++i) {
    FillDomainInProto(context->DomainOf(i),
                      context->working_model->mutable_variables(i));
  }
  const CpModelProto& model_proto = *(context->working_model);

  // Load the constraints in a local model.
  //
  // TODO(user): remove code duplication with cp_mode_solver. Here we also do
  // not run the heuristic to decide which variable to fully encode.
  //
  // TODO(user): Maybe do not load slow to propagate constraints? for instance
  // we do not use any linear relaxation here.
  Model model;
  model.GetOrCreate<TimeLimit>()->MergeWithGlobalTimeLimit(global_time_limit);
  auto* encoder = model.GetOrCreate<IntegerEncoder>();
  encoder->DisableImplicationBetweenLiteral();
  auto* mapping = model.GetOrCreate<CpModelMapping>();
  mapping->CreateVariables(model_proto, false, &model);
  mapping->DetectOptionalVariables(model_proto, &model);
  mapping->ExtractEncoding(model_proto, &model);
  for (const ConstraintProto& ct : model_proto.constraints()) {
    if (mapping->ConstraintIsAlreadyLoaded(&ct)) continue;
    CHECK(LoadConstraint(ct, &model));
  }
  encoder->AddAllImplicationsBetweenAssociatedLiterals();
  auto* sat_solver = model.GetOrCreate<SatSolver>();
  sat_solver->Propagate();

  // Probe.
  //
  // TODO(user): Compute the transitive reduction instead of just the
  // equivalences, and use the newly learned binary clauses?
  auto* implication_graph = model.GetOrCreate<BinaryImplicationGraph>();
  ProbeBooleanVariables(/*deterministic_time_limit=*/1.0, &model);
  if (sat_solver->IsModelUnsat() || !implication_graph->DetectEquivalences()) {
    context->is_unsat = true;
    return;
  }

  // Update the presolve context with fixed Boolean variables.
  for (int i = 0; i < sat_solver->LiteralTrail().Index(); ++i) {
    const Literal l = sat_solver->LiteralTrail()[i];
    const int var = mapping->GetProtoVariableFromBooleanVariable(l.Variable());
    if (var >= 0) {
      const int ref = l.IsPositive() ? var : NegatedRef(var);
      context->SetLiteralToTrue(ref);
    }
  }

  const int num_variables = context->working_model->variables().size();
  auto* integer_trail = model.GetOrCreate<IntegerTrail>();
  for (int var = 0; var < num_variables; ++var) {
    // Restrict IntegerVariable domain.
    // Note that Boolean are already dealt with above.
    if (!mapping->IsBoolean(var)) {
      const Domain new_domain =
          integer_trail->InitialVariableDomain(mapping->Integer(var));
      context->IntersectDomainWith(var, new_domain);
      continue;
    }

    // Add Boolean equivalence relations.
    const Literal l = mapping->Literal(var);
    const Literal r = implication_graph->RepresentativeOf(l);
    if (r != l) {
      const int r_var =
          mapping->GetProtoVariableFromBooleanVariable(r.Variable());
      CHECK_GE(r_var, 0);
      context->AddBooleanEqualityRelation(
          var, r.IsPositive() ? r_var : NegatedRef(r_var));
    }
  }
}

void PresolvePureSatPart(PresolveContext* context) {
  // TODO(user, lperron): Reenable some SAT presolve with
  // enumerate_all_solutions set to true.
  if (context->is_unsat || context->enumerate_all_solutions) return;

  const int num_variables = context->working_model->variables_size();
  SatPostsolver postsolver(num_variables);
  SatPresolver presolver(&postsolver);
  presolver.SetNumVariables(num_variables);

  SatParameters params;

  // TODO(user): enable blocked clause. The problem is that our postsolve
  // do not support changing the value of a variable from the solution of the
  // presolved problem, and we do need this for blocked clause.
  params.set_presolve_blocked_clause(false);

  // TODO(user): BVA takes times and do not seems to help on the minizinc
  // benchmarks. That said, it was useful on pure sat problems, so we may
  // want to enable it.
  params.set_presolve_use_bva(false);
  presolver.SetParameters(params);

  // Converts a cp_model literal ref to a sat::Literal used by SatPresolver.
  auto convert = [](int ref) {
    if (RefIsPositive(ref)) return Literal(BooleanVariable(ref), true);
    return Literal(BooleanVariable(NegatedRef(ref)), false);
  };

  // Load all Clauses into the presolver and remove them from the current model.
  //
  // TODO(user): The removing and adding back of the same clause when nothing
  // happens in the presolve "seems" bad. That said, complexity wise, it is
  // a lot faster that what happens in the presolve though.
  //
  // TODO(user): Add the "small" at most one constraints to the SAT presolver by
  // expanding them to implications? that could remove a lot of clauses. Do that
  // when we are sure we don't load duplicates at_most_one/implications in the
  // solver.
  std::vector<Literal> clause;
  int num_removed_constraints = 0;
  for (int i = 0; i < context->working_model->constraints_size(); ++i) {
    const ConstraintProto& ct = context->working_model->constraints(i);

    if (ct.constraint_case() == ConstraintProto::ConstraintCase::kBoolOr) {
      ++num_removed_constraints;
      clause.clear();
      for (const int ref : ct.bool_or().literals()) {
        clause.push_back(convert(ref));
      }
      presolver.AddClause(clause);

      context->working_model->mutable_constraints(i)->Clear();
      context->UpdateConstraintVariableUsage(i);
      continue;
    }

    if (ct.constraint_case() == ConstraintProto::ConstraintCase::kBoolAnd) {
      ++num_removed_constraints;
      const Literal l = convert(ct.enforcement_literal(0)).Negated();
      CHECK(!ct.bool_and().literals().empty());
      for (const int ref : ct.bool_and().literals()) {
        presolver.AddClause({l, convert(ref)});
      }

      context->working_model->mutable_constraints(i)->Clear();
      context->UpdateConstraintVariableUsage(i);
      continue;
    }
  }

  // Abort early if there was no Boolean constraints.
  if (num_removed_constraints == 0) return;

  // Mark the variables appearing elsewhere or in the objective as non-removable
  // by the sat presolver.
  //
  // TODO(user): do not remove variable that appear in the decision heuristic?
  // TODO(user): We could go further for variable with only one polarity by
  // removing variable from the objective if they can be set to their "low"
  // objective value, and also removing enforcement literal that can be set to
  // false and don't appear elsewhere.
  int num_removable = 0;
  std::vector<bool> can_be_removed(num_variables, false);
  for (int i = 0; i < num_variables; ++i) {
    if (context->var_to_constraints[i].empty()) {
      ++num_removable;
      can_be_removed[i] = true;
    }
  }

  // Run the presolve for a small number of passes.
  // TODO(user): Add probing like we do in the pure sat solver presolve loop?
  // TODO(user): Add a time limit, this can be slow on big SAT problem.
  VLOG(1) << "num removable Booleans: " << num_removable;
  const int num_passes = params.presolve_use_bva() ? 4 : 1;
  for (int i = 0; i < num_passes; ++i) {
    const int old_num_clause = postsolver.NumClauses();
    if (!presolver.Presolve(can_be_removed)) {
      LOG(INFO) << "UNSAT during SAT presolve.";
      context->is_unsat = true;
      return;
    }
    if (old_num_clause == postsolver.NumClauses()) break;
  }

  // Add any new variables to our internal structure.
  const int new_num_variables = presolver.NumVariables();
  if (new_num_variables > context->working_model->variables_size()) {
    LOG(INFO) << "New variables added by the SAT presolver.";
    for (int i = context->working_model->variables_size();
         i < new_num_variables; ++i) {
      IntegerVariableProto* var_proto = context->working_model->add_variables();
      var_proto->add_domain(0);
      var_proto->add_domain(1);
    }
    context->InitializeNewDomains();
  }

  // Add the presolver clauses back into the model.
  ExtractClauses(presolver, context->working_model);

  // Update the constraints <-> variables graph.
  context->UpdateNewConstraintsVariableUsage();

  // Add the postsolver clauses to mapping_model.
  ExtractClauses(postsolver, context->mapping_model);
}

// TODO(user): The idea behind this was that it is better to have an objective
// as spreaded as possible. However on some problems this have the opposite
// effect. Like on a triangular matrix where each expansion reduced the size
// of the objective by one. Investigate and fix?
void ExpandObjective(PresolveContext* context) {
  if (context->is_unsat) return;

  // This is because we called EncodeObjectiveAsSingleVariable(). Note that
  // it allows us to update the proto objective domain too.
  CHECK_EQ(context->working_model->objective().vars_size(), 1);
  CHECK_EQ(context->working_model->objective().coeffs(0), 1);

  // This is also because of EncodeObjectiveAsSingleVariable(). Note that
  // we DO NOT count the offset in the domain, which make the code below quite
  // tricky. TODO(user): maybe we should change that.
  int64 objective_offset_change = 0;
  const auto initial_objective_domain =
      context->DomainOf(context->working_model->objective().vars(0));

  // Replace the objective by its representative.
  {
    const int ref = context->working_model->objective().vars(0);
    const int var = PositiveRef(ref);
    const AffineRelation::Relation r = context->GetAffineRelation(var);
    if (r.representative != var) {
      auto* mutable_objective = context->working_model->mutable_objective();
      const int coeff = RefIsPositive(ref) ? r.coeff : -r.coeff;
      const int offset = RefIsPositive(ref) ? r.offset : -r.offset;

      objective_offset_change += offset;
      mutable_objective->set_coeffs(0, coeff);
      mutable_objective->set_vars(0, r.representative);

      context->var_to_constraints[var].erase(-1);
      context->var_to_constraints[r.representative].insert(-1);
    }
  }

  // Convert the objective linear expression to a map for ease of use below.
  std::map<int, int64> objective_map;
  for (int i = 0; i < context->working_model->objective().vars_size(); ++i) {
    const int ref = context->working_model->objective().vars(i);
    const int64 coeff = context->working_model->objective().coeffs(i);
    objective_map[PositiveRef(ref)] = RefIsPositive(ref) ? coeff : -coeff;
  }

  // To avoid a bad complexity, we need to compute the number of relevant
  // constraints for each variables.
  const int num_variables = context->working_model->variables_size();
  const int num_constraints = context->working_model->constraints_size();
  absl::flat_hash_set<int> relevant_constraints;
  std::vector<int> var_to_num_relevant_constraints(num_variables, 0);
  for (int ct_index = 0; ct_index < num_constraints; ++ct_index) {
    const ConstraintProto& ct = context->working_model->constraints(ct_index);
    // Skip everything that is not a linear equality constraint.
    if (!ct.enforcement_literal().empty() ||
        ct.constraint_case() != ConstraintProto::ConstraintCase::kLinear ||
        ct.linear().domain().size() != 2 ||
        ct.linear().domain(0) != ct.linear().domain(1)) {
      continue;
    }

    relevant_constraints.insert(ct_index);
    const int num_terms = ct.linear().vars_size();
    for (int i = 0; i < num_terms; ++i) {
      var_to_num_relevant_constraints[PositiveRef(ct.linear().vars(i))]++;
    }
  }

  // We currently never expand a variable more than once.
  int num_expansions = 0;
  absl::flat_hash_set<int> processed_vars;
  while (!relevant_constraints.empty()) {
    // Find a not yet expanded var.
    int objective_var = -1;
    for (const auto& entry : objective_map) {
      const int var = entry.first;
      if (processed_vars.count(var)) continue;
      if (var_to_num_relevant_constraints[var] == 0) continue;
      objective_var = var;
      break;
    }
    if (objective_var == -1) break;
    CHECK(RefIsPositive(objective_var));
    processed_vars.insert(objective_var);

    int expanded_linear_index = -1;
    int64 objective_coeff_in_expanded_constraint;
    int64 size_of_expanded_constraint = 0;
    const auto& non_deterministic_list =
        context->var_to_constraints[objective_var];
    std::vector<int> constraints_with_objective(non_deterministic_list.begin(),
                                                non_deterministic_list.end());
    std::sort(constraints_with_objective.begin(),
              constraints_with_objective.end());
    for (const int ct_index : constraints_with_objective) {
      if (ct_index == -1) continue;
      if (relevant_constraints.count(ct_index) == 0) continue;
      const ConstraintProto& ct = context->working_model->constraints(ct_index);

      // This constraint is relevant now, but it will never be later because
      // it will contain the objective_var wich is already processed!
      relevant_constraints.erase(ct_index);
      const int num_terms = ct.linear().vars_size();
      for (int i = 0; i < num_terms; ++i) {
        var_to_num_relevant_constraints[PositiveRef(ct.linear().vars(i))]--;
      }

      // Find the coefficient of objective_var in this constraint, and perform
      // various checks.
      bool is_present = false;
      int64 objective_coeff;
      for (int i = 0; i < num_terms; ++i) {
        const int ref = ct.linear().vars(i);
        const int64 coeff = ct.linear().coeffs(i);
        if (PositiveRef(ref) == objective_var) {
          CHECK(!is_present) << "Duplicate variables not supported.";
          is_present = true;
          objective_coeff = (ref == objective_var) ? coeff : -coeff;
        } else {
          // This is not possible since we only consider relevant constraints.
          CHECK(!processed_vars.count(PositiveRef(ref)));
        }
      }
      CHECK(is_present);

      // We use the longest equality we can find.
      //
      // TODO(user): Deal with objective_coeff with a magnitude greater than
      // 1? This will only be possible if we change the objective coeff type
      // to double.
      if (std::abs(objective_coeff) == 1 &&
          num_terms > size_of_expanded_constraint) {
        expanded_linear_index = ct_index;
        size_of_expanded_constraint = num_terms;
        objective_coeff_in_expanded_constraint = objective_coeff;
      }
    }

    if (expanded_linear_index != -1) {
      context->UpdateRuleStats("objective: expanded objective constraint.");

      // Update the objective map. Note that the division is possible because
      // currently we only expand with coeff with a magnitude of 1.
      CHECK_EQ(std::abs(objective_coeff_in_expanded_constraint), 1);
      const int64 factor =
          objective_map[objective_var] / objective_coeff_in_expanded_constraint;

      objective_map.erase(objective_var);
      context->var_to_constraints[objective_var].erase(-1);

      const ConstraintProto& ct =
          context->working_model->constraints(expanded_linear_index);
      const int num_terms = ct.linear().vars_size();
      for (int i = 0; i < num_terms; ++i) {
        const int ref = ct.linear().vars(i);
        const int var = PositiveRef(ref);
        if (var == objective_var) continue;

        int64 coeff = -ct.linear().coeffs(i) * factor;
        if (!RefIsPositive(ref)) coeff = -coeff;
        if (!gtl::ContainsKey(objective_map, var)) {
          context->var_to_constraints[var].insert(-1);
        }
        objective_map[var] += coeff;
        if (objective_map[var] == 0.0) {
          objective_map.erase(var);
          context->var_to_constraints[var].erase(-1);
        }
      }
      objective_offset_change += ct.linear().domain(0) * factor;

      // If the objective variable wasn't used in other constraints and it can
      // be reconstructed whatever the value of the other variables, we can
      // remove the constraint.
      //
      // TODO(user): It should be possible to refactor the code so this is
      // automatically done by the linear constraint singleton presolve rule.
      if (context->var_to_constraints[objective_var].size() == 1) {
        // Compute implied domain on objective_var.
        Domain implied_domain = ReadDomainFromProto(ct.linear());
        for (int i = 0; i < num_terms; ++i) {
          const int ref = ct.linear().vars(i);
          if (PositiveRef(ref) == objective_var) continue;
          implied_domain = implied_domain.AdditionWith(
              context->DomainOf(ref).ContinuousMultiplicationBy(
                  -ct.linear().coeffs(i)));
        }
        implied_domain = implied_domain.InverseMultiplicationBy(
            objective_coeff_in_expanded_constraint);

        // Remove the constraint if the implied domain is included in the
        // domain of the objective_var term.
        //
        // Note the special case for the first expansion where any domain
        // restriction will be handled by the objective domain because we
        // called EncodeObjectiveAsSingleVariable() above.
        if (num_expansions == 0 ||
            implied_domain.IsIncludedIn(context->DomainOf(objective_var))) {
          context->UpdateRuleStats("objective: removed objective constraint.");
          *(context->mapping_model->add_constraints()) = ct;
          context->working_model->mutable_constraints(expanded_linear_index)
              ->Clear();
          context->UpdateConstraintVariableUsage(expanded_linear_index);
        }
      }
      ++num_expansions;
    }
  }

  // Re-write the objective.
  CpObjectiveProto* const mutable_objective =
      context->working_model->mutable_objective();
  mutable_objective->clear_coeffs();
  mutable_objective->clear_vars();
  for (const auto& entry : objective_map) {
    mutable_objective->add_vars(entry.first);
    mutable_objective->add_coeffs(entry.second);
  }
  mutable_objective->set_offset(mutable_objective->offset() +
                                objective_offset_change);
  FillDomainInProto(
      initial_objective_domain.AdditionWith(Domain(-objective_offset_change)),
      mutable_objective);
}

void MergeNoOverlapConstraints(PresolveContext* context) {
  if (context->is_unsat) return;

  const int num_constraints = context->working_model->constraints_size();
  int old_num_no_overlaps = 0;
  int old_num_intervals = 0;

  // Extract the no-overlap constraints.
  std::vector<int> disjunctive_index;
  std::vector<std::vector<Literal>> cliques;
  for (int c = 0; c < num_constraints; ++c) {
    const ConstraintProto& ct = context->working_model->constraints(c);
    if (ct.constraint_case() != ConstraintProto::ConstraintCase::kNoOverlap) {
      continue;
    }
    std::vector<Literal> clique;
    for (const int i : ct.no_overlap().intervals()) {
      clique.push_back(Literal(BooleanVariable(i), true));
    }
    cliques.push_back(clique);
    disjunctive_index.push_back(c);

    old_num_no_overlaps++;
    old_num_intervals += clique.size();
  }

  // We reuse the max-clique code from sat.
  Model local_model;
  auto* graph = local_model.GetOrCreate<BinaryImplicationGraph>();
  graph->Resize(num_constraints);
  for (const std::vector<Literal>& clique : cliques) {
    graph->AddAtMostOne(clique);
  }
  CHECK(graph->DetectEquivalences());
  graph->TransformIntoMaxCliques(&cliques);

  // Replace each no-overlap with an extended version, or remove if empty.
  int new_num_no_overlaps = 0;
  int new_num_intervals = 0;
  for (int i = 0; i < cliques.size(); ++i) {
    const int ct_index = disjunctive_index[i];
    ConstraintProto* ct = context->working_model->mutable_constraints(ct_index);
    ct->Clear();
    if (cliques[i].empty()) continue;
    for (const Literal l : cliques[i]) {
      CHECK(l.IsPositive());
      ct->mutable_no_overlap()->add_intervals(l.Variable().value());
    }
    new_num_no_overlaps++;
    new_num_intervals += cliques[i].size();
  }
  if (old_num_intervals != new_num_intervals ||
      old_num_no_overlaps != new_num_no_overlaps) {
    VLOG(1) << absl::StrCat("Merged ", old_num_no_overlaps, " no-overlaps (",
                            old_num_intervals, " intervals) into ",
                            new_num_no_overlaps, " no-overlaps (",
                            new_num_intervals, " intervals).");
    context->UpdateRuleStats("no_overlap: merged constraints");
  }
}

bool PresolveOneConstraint(int c, PresolveContext* context) {
  ConstraintProto* ct = context->working_model->mutable_constraints(c);

  // Generic presolve to exploit variable/literal equivalence.
  if (ExploitEquivalenceRelations(ct, context)) {
    context->UpdateConstraintVariableUsage(c);
  }

  // Generic presolve for reified constraint.
  if (PresolveEnforcementLiteral(ct, context)) {
    context->UpdateConstraintVariableUsage(c);
  }

  // Call the presolve function for this constraint if any.
  switch (ct->constraint_case()) {
    case ConstraintProto::ConstraintCase::kBoolOr:
      return PresolveBoolOr(ct, context);
    case ConstraintProto::ConstraintCase::kBoolAnd:
      return PresolveBoolAnd(ct, context);
    case ConstraintProto::ConstraintCase::kAtMostOne:
      return PresolveAtMostOne(ct, context);
    case ConstraintProto::ConstraintCase::kIntMax:
      return PresolveIntMax(ct, context);
    case ConstraintProto::ConstraintCase::kIntMin:
      return PresolveIntMin(ct, context);
    case ConstraintProto::ConstraintCase::kIntProd:
      return PresolveIntProd(ct, context);
    case ConstraintProto::ConstraintCase::kIntDiv:
      return PresolveIntDiv(ct, context);
    case ConstraintProto::ConstraintCase::kLinear: {
      if (PresolveLinear(ct, context)) {
        context->UpdateConstraintVariableUsage(c);
      }
      if (ct->constraint_case() == ConstraintProto::ConstraintCase::kLinear) {
        const int old_num_enforcement_literals = ct->enforcement_literal_size();
        ExtractEnforcementLiteralFromLinearConstraint(ct, context);
        if (ct->enforcement_literal_size() > old_num_enforcement_literals) {
          PresolveLinear(ct, context);
          context->UpdateConstraintVariableUsage(c);
        }
      }

      if (ct->constraint_case() == ConstraintProto::ConstraintCase::kLinear) {
        return PresolveLinearOnBooleans(ct, context);
      }
      return false;
    }
    case ConstraintProto::ConstraintCase::kInterval:
      return PresolveInterval(ct, context);
    case ConstraintProto::ConstraintCase::kElement:
      return PresolveElement(ct, context);
    case ConstraintProto::ConstraintCase::kTable:
      return PresolveTable(ct, context);
    case ConstraintProto::ConstraintCase::kAllDiff:
      return PresolveAllDiff(ct, context);
    case ConstraintProto::ConstraintCase::kNoOverlap:
      return PresolveNoOverlap(ct, context);
    case ConstraintProto::ConstraintCase::kCumulative:
      return PresolveCumulative(ct, context);
    case ConstraintProto::ConstraintCase::kCircuit:
      return PresolveCircuit(ct, context);
    default:
      return false;
  }
}

void PresolveToFixPoint(PresolveContext* context) {
  if (context->is_unsat) return;

  // This is used for constraint having unique variables in them (i.e. not
  // appearing anywhere else) to not call the presolve more than once for this
  // reason.
  absl::flat_hash_set<std::pair<int, int>> var_constraint_pair_already_called;

  // The queue of "active" constraints, initialized to all of them.
  std::vector<bool> in_queue(context->working_model->constraints_size(), true);
  std::deque<int> queue(context->working_model->constraints_size());
  std::iota(queue.begin(), queue.end(), 0);
  while (!queue.empty() && !context->is_unsat) {
    while (!queue.empty() && !context->is_unsat) {
      const int c = queue.front();
      in_queue[c] = false;
      queue.pop_front();

      const int old_num_constraint = context->working_model->constraints_size();
      const bool changed = PresolveOneConstraint(c, context);

      // Add to the queue any newly created constraints.
      const int new_num_constraints =
          context->working_model->constraints_size();
      if (new_num_constraints > old_num_constraint) {
        context->UpdateNewConstraintsVariableUsage();
        in_queue.resize(new_num_constraints, true);
        for (int c = old_num_constraint; c < new_num_constraints; ++c) {
          queue.push_back(c);
        }
      }

      // TODO(user): Is seems safer to simply remove the changed Boolean.
      // We loose a bit of preformance, but the code is simpler.
      if (changed) {
        context->UpdateConstraintVariableUsage(c);
      }
    }

    // Re-add to the queue constraints that have unique variables. Note that to
    // not enter an infinite loop, we call each (var, constraint) pair at most
    // once.
    for (int v = 0; v < context->var_to_constraints.size(); ++v) {
      const auto& constraints = context->var_to_constraints[v];
      if (constraints.size() != 1) continue;
      const int c = *constraints.begin();
      if (c < 0) continue;
      if (gtl::ContainsKey(var_constraint_pair_already_called,
                           std::pair<int, int>(v, c))) {
        continue;
      }
      var_constraint_pair_already_called.insert({v, c});
      if (!in_queue[c]) {
        in_queue[c] = true;
        queue.push_back(c);
      }
    }

    // Re-add to the queue the constraints that touch a variable that changed.
    //
    // TODO(user): Avoid reprocessing the constraints that changed the variables
    // with the use of timestamp.
    const int old_queue_size = queue.size();
    for (const int v : context->modified_domains.PositionsSetAtLeastOnce()) {
      if (context->DomainIsEmpty(v)) {
        context->is_unsat = true;
        break;
      }
      if (context->IsFixed(v)) context->ExploitFixedDomain(v);

      for (const int c : context->var_to_constraints[v]) {
        if (c >= 0 && !in_queue[c]) {
          in_queue[c] = true;
          queue.push_back(c);
        }
      }
    }

    // Make sure the order is deterministic! because var_to_constraints[]
    // order changes from one run to the next.
    std::sort(queue.begin() + old_queue_size, queue.end());
    context->modified_domains.SparseClearAll();
  }

  if (context->is_unsat) return;

  // Make sure we filter out absent intervals.
  //
  // TODO(user): ideally we should "wake-up" any constraint that contains an
  // absent interval in the main propagation loop above. But we currently don't
  // maintain such list.
  const int num_constraints = context->working_model->constraints_size();
  for (int c = 0; c < num_constraints; ++c) {
    ConstraintProto* ct = context->working_model->mutable_constraints(c);
    switch (ct->constraint_case()) {
      case ConstraintProto::ConstraintCase::kNoOverlap:
        if (PresolveNoOverlap(ct, context)) {
          context->UpdateConstraintVariableUsage(c);
        }
        break;
      case ConstraintProto::ConstraintCase::kNoOverlap2D:
        // TODO(user): Implement if we ever support optional intervals in
        // this constraint. Currently we do not.
        break;
      case ConstraintProto::ConstraintCase::kCumulative:
        if (PresolveCumulative(ct, context)) {
          context->UpdateConstraintVariableUsage(c);
        }
        break;
      default:
        break;
    }
  }
}

void RemoveUnusedEquivalentVariables(PresolveContext* context) {
  if (context->is_unsat || context->enumerate_all_solutions) return;

  // Remove all affine constraints (they will be re-added later if
  // needed) in the presolved model.
  for (int c = 0; c < context->working_model->constraints_size(); ++c) {
    ConstraintProto* ct = context->working_model->mutable_constraints(c);
    if (gtl::ContainsKey(context->affine_constraints, ct)) {
      ct->Clear();
      context->UpdateConstraintVariableUsage(c);
      continue;
    }
  }

  // Add back the affine relations to the presolved model or to the mapping
  // model, depending where they are needed.
  //
  // TODO(user): unfortunately, for now, this duplicates the interval relations
  // with a fixed size.
  int num_affine_relations = 0;
  for (int var = 0; var < context->working_model->variables_size(); ++var) {
    if (context->IsFixed(var)) continue;

    const AffineRelation::Relation r = context->GetAffineRelation(var);
    if (r.representative == var) continue;

    // We can get rid of this variable, only if:
    // - it is not used elsewhere.
    // - whatever the value of the representative, we can always find a value
    //   for this variable.
    CpModelProto* proto;
    if (context->var_to_constraints[var].empty()) {
      // Make sure that domain(representative) is tight.
      const Domain implied = context->DomainOf(var)
                                 .AdditionWith({-r.offset, -r.offset})
                                 .InverseMultiplicationBy(r.coeff);
      if (context->IntersectDomainWith(r.representative, implied)) {
        LOG(WARNING) << "Domain of " << r.representative
                     << " was not fully propagated using the affine relation "
                     << "(representative =" << r.representative
                     << ", coeff = " << r.coeff << ", offset = " << r.offset
                     << ")";
      }
      proto = context->mapping_model;
    } else {
      proto = context->working_model;
      ++num_affine_relations;
    }

    ConstraintProto* ct = proto->add_constraints();
    auto* arg = ct->mutable_linear();
    arg->add_vars(var);
    arg->add_coeffs(1);
    arg->add_vars(r.representative);
    arg->add_coeffs(-r.coeff);
    arg->add_domain(r.offset);
    arg->add_domain(r.offset);
  }

  // Update the variable usage.
  context->UpdateNewConstraintsVariableUsage();
}

}  // namespace.

// =============================================================================
// Public API.
// =============================================================================

// The presolve works as follow:
//
// First stage:
// We will process all active constraints until a fix point is reached. During
// this stage:
// - Variable will never be deleted, but their domain will be reduced.
// - Constraint will never be deleted (they will be marked as empty if needed).
// - New variables and new constraints can be added after the existing ones.
// - Constraints are added only when needed to the mapping_problem if they are
//   needed during the postsolve.
//
// Second stage:
// - All the variables domain will be copied to the mapping_model.
// - Everything will be remapped so that only the variables appearing in some
//   constraints will be kept and their index will be in [0, num_new_variables).
void PresolveCpModel(const PresolveOptions& options,
                     CpModelProto* presolved_model, CpModelProto* mapping_model,
                     std::vector<int>* postsolve_mapping) {
  PresolveContext context;
  context.working_model = presolved_model;
  context.mapping_model = mapping_model;
  context.enumerate_all_solutions =
      options.parameters.enumerate_all_solutions();

  // We copy the search strategy to the mapping_model.
  for (const auto& decision_strategy : presolved_model->search_strategy()) {
    *mapping_model->add_search_strategy() = decision_strategy;
  }

  // Encode linear objective, so that it can be presolved like a normal
  // constraint.
  EncodeObjectiveAsSingleVariable(context.working_model);

  // Initialize the initial context.working_model domains.
  context.InitializeNewDomains();

  // Initialize the constraint <-> variable graph.
  context.var_to_constraints.resize(context.working_model->variables_size());
  context.UpdateNewConstraintsVariableUsage();

  // Hack for the objective so that it is never considered to appear in only one
  // constraint.
  if (context.working_model->has_objective()) {
    for (const int obj_var : context.working_model->objective().vars()) {
      context.var_to_constraints[PositiveRef(obj_var)].insert(-1);
    }
  }

  // Main propagation loop.
  PresolveToFixPoint(&context);

  // Runs the probing.
  // TODO(user): do that and the pure-SAT part below more than once.
  if (options.parameters.cp_model_probing_level() > 0) {
    Probe(options.time_limit, &context);
    PresolveToFixPoint(&context);
  }

  RemoveUnusedEquivalentVariables(&context);

  // Run SAT specific presolve on the pure-SAT part of the problem.
  // Note that because this can only remove/fix variable not used in the other
  // part of the problem, there is no need to redo more presolve afterwards.
  //
  // TODO(user): expose the parameters here so we can use
  // cp_model_use_sat_presolve().
  PresolvePureSatPart(&context);

  // Extract redundant at most one constraint form the linear ones.
  //
  // TODO(user): more generally if we do some probing, the same relation will
  // be detected (and more). Also add an option to turn this off?
  //
  // TODO(user): instead of extracting at most one, extra pairwise conflicts
  // and add them to bool_and clauses? this is some sort of small scale probing,
  // but good for sat presolve and clique later?
  if (!context.is_unsat) {
    const int old_size = context.working_model->constraints_size();
    for (int c = 0; c < old_size; ++c) {
      ConstraintProto* ct = context.working_model->mutable_constraints(c);
      if (ct->constraint_case() != ConstraintProto::ConstraintCase::kLinear) {
        continue;
      }
      if (gtl::ContainsKey(context.affine_constraints, ct)) {
        continue;
      }
      ExtractAtMostOneFromLinear(ct, &context);
    }
    context.UpdateNewConstraintsVariableUsage();
  }

  if (context.is_unsat) {
    // Set presolved_model to the simplest UNSAT problem (empty clause).
    presolved_model->Clear();
    presolved_model->add_constraints()->mutable_bool_or();
    return;
  }

  // Regroup no-overlaps into max-cliques.
  MergeNoOverlapConstraints(&context);

  if (context.working_model->has_objective()) {
    ExpandObjective(&context);
  }

  // TODO(user): Past this point the context.constraint_to_vars[] graph is not
  // consistent and shouldn't be used. We do use var_to_constraints.size()
  // though.
  DCHECK(context.ConstraintVariableUsageIsConsistent());

  // Remove all empty constraints. Note that we need to remap the interval
  // references.
  std::vector<int> interval_mapping(presolved_model->constraints_size(), -1);
  int new_num_constraints = 0;
  const int old_num_constraints = presolved_model->constraints_size();
  for (int c = 0; c < old_num_constraints; ++c) {
    const auto type = presolved_model->constraints(c).constraint_case();
    if (type == ConstraintProto::ConstraintCase::CONSTRAINT_NOT_SET) continue;
    if (type == ConstraintProto::ConstraintCase::kInterval) {
      interval_mapping[c] = new_num_constraints;
    }
    presolved_model->mutable_constraints(new_num_constraints++)
        ->Swap(presolved_model->mutable_constraints(c));
  }
  presolved_model->mutable_constraints()->DeleteSubrange(
      new_num_constraints, old_num_constraints - new_num_constraints);
  for (ConstraintProto& ct_ref : *presolved_model->mutable_constraints()) {
    ApplyToAllIntervalIndices(
        [&interval_mapping](int* ref) {
          *ref = interval_mapping[*ref];
          CHECK_NE(-1, *ref);
        },
        &ct_ref);
  }

  // The strategy variable indices will be remapped in ApplyVariableMapping()
  // but first we use the representative of the affine relations for the
  // variables that are not present anymore.
  //
  // Note that we properly take into account the sign of the coefficient which
  // will result in the same domain reduction strategy. Moreover, if the
  // variable order is not CHOOSE_FIRST, then we also encode the associated
  // affine transformation in order to preserve the order.
  absl::flat_hash_set<int> used_variables;
  for (DecisionStrategyProto& strategy :
       *presolved_model->mutable_search_strategy()) {
    DecisionStrategyProto copy = strategy;
    strategy.clear_variables();
    for (const int ref : copy.variables()) {
      const int var = PositiveRef(ref);

      // Remove fixed variables.
      if (context.IsFixed(var)) continue;

      // There is not point having a variable appear twice, so we only keep
      // the first occurrence in the first strategy in which it occurs.
      if (gtl::ContainsKey(used_variables, var)) continue;
      used_variables.insert(var);

      if (context.var_to_constraints[var].empty()) {
        const AffineRelation::Relation r = context.GetAffineRelation(var);
        if (!context.var_to_constraints[r.representative].empty()) {
          const int rep = (r.coeff > 0) == RefIsPositive(ref)
                              ? r.representative
                              : NegatedRef(r.representative);
          strategy.add_variables(rep);
          if (strategy.variable_selection_strategy() !=
              DecisionStrategyProto::CHOOSE_FIRST) {
            DecisionStrategyProto::AffineTransformation* t =
                strategy.add_transformations();
            t->set_var(rep);
            t->set_offset(r.offset);
            t->set_positive_coeff(std::abs(r.coeff));
          }
        } else {
          // TODO(user): this variable was removed entirely by the presolve (no
          // equivalent variable present). We simply ignore it entirely which
          // might result in a different search...
        }
      } else {
        strategy.add_variables(ref);
      }
    }
  }

  // Update the variables domain of the presolved_model.
  for (int i = 0; i < presolved_model->variables_size(); ++i) {
    FillDomainInProto(context.DomainOf(i),
                      presolved_model->mutable_variables(i));
  }

  // Set the variables of the mapping_model.
  mapping_model->mutable_variables()->CopyFrom(presolved_model->variables());

  // Remove all the unused variables from the presolved model.
  postsolve_mapping->clear();
  std::vector<int> mapping(presolved_model->variables_size(), -1);
  for (int i = 0; i < presolved_model->variables_size(); ++i) {
    if (context.var_to_constraints[i].empty() &&
        !context.enumerate_all_solutions) {
      continue;
    }
    mapping[i] = postsolve_mapping->size();
    postsolve_mapping->push_back(i);
  }
  ApplyVariableMapping(mapping, presolved_model);

  // Stats and checks.
  if (options.log_info) {
    LOG(INFO) << "- " << context.affine_relations.NumRelations()
              << " affine relations were detected.";
    LOG(INFO) << "- " << context.var_equiv_relations.NumRelations()
              << " variable equivalence relations were detected.";
    std::map<std::string, int> sorted_rules(context.stats_by_rule_name.begin(),
                                            context.stats_by_rule_name.end());
    for (const auto& entry : sorted_rules) {
      if (entry.second == 1) {
        LOG(INFO) << "- rule '" << entry.first << "' was applied 1 time.";
      } else {
        LOG(INFO) << "- rule '" << entry.first << "' was applied "
                  << entry.second << " times.";
      }
    }
  }
  CHECK_EQ("", ValidateCpModel(*presolved_model));
  CHECK_EQ("", ValidateCpModel(*mapping_model));
}

void ApplyVariableMapping(const std::vector<int>& mapping,
                          CpModelProto* proto) {
  // Remap all the variable/literal references in the constraints and the
  // enforcement literals in the variables.
  auto mapping_function = [&mapping](int* ref) {
    const int image = mapping[PositiveRef(*ref)];
    CHECK_GE(image, 0);
    *ref = RefIsPositive(*ref) ? image : NegatedRef(image);
  };
  for (ConstraintProto& ct_ref : *proto->mutable_constraints()) {
    ApplyToAllVariableIndices(mapping_function, &ct_ref);
    ApplyToAllLiteralIndices(mapping_function, &ct_ref);
  }

  // Remap the objective variables.
  if (proto->has_objective()) {
    for (int& mutable_ref : *proto->mutable_objective()->mutable_vars()) {
      mapping_function(&mutable_ref);
    }
  }

  // Remap the search decision heuristic.
  // Note that we delete any heuristic related to a removed variable.
  for (DecisionStrategyProto& strategy : *proto->mutable_search_strategy()) {
    DecisionStrategyProto copy = strategy;
    strategy.clear_variables();
    for (const int ref : copy.variables()) {
      const int image = mapping[PositiveRef(ref)];
      if (image >= 0) {
        strategy.add_variables(RefIsPositive(ref) ? image : NegatedRef(image));
      }
    }
    strategy.clear_transformations();
    for (const auto& transform : copy.transformations()) {
      const int ref = transform.var();
      const int image = mapping[PositiveRef(ref)];
      if (image >= 0) {
        auto* new_transform = strategy.add_transformations();
        *new_transform = transform;
        new_transform->set_var(RefIsPositive(ref) ? image : NegatedRef(image));
      }
    }
  }

  // Remap the solution hint.
  if (proto->has_solution_hint()) {
    auto* mutable_hint = proto->mutable_solution_hint();
    int new_size = 0;
    for (int i = 0; i < mutable_hint->vars_size(); ++i) {
      const int ref = mutable_hint->vars(i);
      const int image = mapping[PositiveRef(ref)];
      if (image >= 0) {
        mutable_hint->set_vars(new_size,
                               RefIsPositive(ref) ? image : NegatedRef(image));
        mutable_hint->set_values(new_size, mutable_hint->values(i));
        ++new_size;
      }
    }
    if (new_size > 0) {
      mutable_hint->mutable_vars()->Truncate(new_size);
      mutable_hint->mutable_values()->Truncate(new_size);
    } else {
      proto->clear_solution_hint();
    }
  }

  // Move the variable definitions.
  std::vector<IntegerVariableProto> new_variables;
  for (int i = 0; i < mapping.size(); ++i) {
    const int image = mapping[i];
    if (image < 0) continue;
    if (image >= new_variables.size()) {
      new_variables.resize(image + 1, IntegerVariableProto());
    }
    new_variables[image].Swap(proto->mutable_variables(i));
  }
  proto->clear_variables();
  for (IntegerVariableProto& proto_ref : new_variables) {
    proto->add_variables()->Swap(&proto_ref);
  }

  // Check that all variables are used.
  for (const IntegerVariableProto& v : proto->variables()) {
    CHECK_GT(v.domain_size(), 0);
  }
}

}  // namespace sat
}  // namespace operations_research
