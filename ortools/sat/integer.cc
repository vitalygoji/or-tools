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

#include "ortools/sat/integer.h"

#include <algorithm>
#include <queue>
#include <type_traits>

#include "ortools/base/iterator_adaptors.h"
#include "ortools/base/stl_util.h"
#include "ortools/util/time_limit.h"

namespace operations_research {
namespace sat {

std::vector<IntegerVariable> NegationOf(
    const std::vector<IntegerVariable>& vars) {
  std::vector<IntegerVariable> result(vars.size());
  for (int i = 0; i < vars.size(); ++i) {
    result[i] = NegationOf(vars[i]);
  }
  return result;
}

void IntegerEncoder::FullyEncodeVariable(IntegerVariable var) {
  CHECK(!VariableIsFullyEncoded(var));
  CHECK_EQ(0, sat_solver_->CurrentDecisionLevel());
  CHECK(!(*domains_)[var].IsEmpty());  // UNSAT. We don't deal with that here.

  std::vector<IntegerValue> values;
  for (const ClosedInterval interval : (*domains_)[var]) {
    for (IntegerValue v(interval.start); v <= interval.end; ++v) {
      values.push_back(v);
      CHECK_LT(values.size(), 100000) << "Domain too large for full encoding.";
    }
  }

  std::vector<Literal> literals;
  if (values.size() == 1) {
    literals.push_back(GetTrueLiteral());
  } else if (values.size() == 2) {
    literals.push_back(GetOrCreateAssociatedLiteral(
        IntegerLiteral::LowerOrEqual(var, values[0])));
    literals.push_back(literals.back().Negated());
  } else {
    for (int i = 0; i < values.size(); ++i) {
      const std::pair<IntegerVariable, IntegerValue> key{var, values[i]};
      if (gtl::ContainsKey(equality_to_associated_literal_, key)) {
        literals.push_back(equality_to_associated_literal_[key]);
      } else {
        literals.push_back(Literal(sat_solver_->NewBooleanVariable(), true));
      }
    }
  }

  // Create the associated literal (<= and >=) in order (best for the
  // implications between them). Note that we only create literals like this for
  // value inside the domain. This is nice since these will be the only kind of
  // literal pushed by Enqueue() (we look at the domain there).
  for (int i = 0; i + 1 < literals.size(); ++i) {
    const IntegerLiteral i_lit = IntegerLiteral::LowerOrEqual(var, values[i]);
    const IntegerLiteral i_lit_negated =
        IntegerLiteral::GreaterOrEqual(var, values[i + 1]);
    if (i == 0) {
      // Special case for the start.
      HalfAssociateGivenLiteral(i_lit, literals[0]);
      HalfAssociateGivenLiteral(i_lit_negated, literals[0].Negated());
    } else if (i + 2 == literals.size()) {
      // Special case for the end.
      HalfAssociateGivenLiteral(i_lit, literals.back().Negated());
      HalfAssociateGivenLiteral(i_lit_negated, literals.back());
    } else {
      // Normal case.
      if (!LiteralIsAssociated(i_lit) || !LiteralIsAssociated(i_lit_negated)) {
        const BooleanVariable b = sat_solver_->NewBooleanVariable();
        HalfAssociateGivenLiteral(i_lit, Literal(b, true));
        HalfAssociateGivenLiteral(i_lit_negated, Literal(b, false));
      }
    }
  }

  // Now that all literals are created, wire them together using
  //    (X == v)  <=>  (X >= v) and (X <= v).
  //
  // TODO(user): this is currently in O(n^2) which is potentially bad even if
  // we do it only once per variable.
  for (int i = 0; i < literals.size(); ++i) {
    AssociateToIntegerEqualValue(literals[i], var, values[i]);
  }

  // Mark var and Negation(var) as fully encoded.
  const int required_size = std::max(var, NegationOf(var)).value() + 1;
  if (required_size > is_fully_encoded_.size()) {
    is_fully_encoded_.resize(required_size, false);
  }
  is_fully_encoded_[var] = true;
  is_fully_encoded_[NegationOf(var)] = true;
}

std::vector<IntegerEncoder::ValueLiteralPair>
IntegerEncoder::FullDomainEncoding(IntegerVariable var) const {
  CHECK(VariableIsFullyEncoded(var));
  std::vector<ValueLiteralPair> encoding;
  for (const ClosedInterval interval : (*domains_)[var]) {
    for (IntegerValue value(interval.start); value <= interval.end; ++value) {
      const std::pair<IntegerVariable, IntegerValue> key{var, value};
      const Literal literal =
          gtl::FindOrDieNoPrint(equality_to_associated_literal_, key);
      if (sat_solver_->Assignment().LiteralIsTrue(literal)) {
        return {{value, literal}};
      } else if (!sat_solver_->Assignment().LiteralIsFalse(literal)) {
        encoding.push_back({value, literal});
      }
    }
  }
  return encoding;
}

std::vector<IntegerEncoder::ValueLiteralPair>
IntegerEncoder::PartialDomainEncoding(IntegerVariable var) const {
  std::vector<ValueLiteralPair> encoding;

  // Because the domain of var can be arbitrary large, we use the fact that
  // when (var == value) is created, then we have (var >= value && var <= value)
  // too. Except for the min/max of the initial domain.
  if (var >= encoding_by_var_.size()) return encoding;

  std::vector<IntegerValue> possible_values;
  {
    const IntegerValue min_value((*domains_)[var].Min());
    const IntegerValue max_value((*domains_)[var].Max());
    possible_values.push_back(min_value);
    for (const auto entry : encoding_by_var_[var]) {
      if (entry.first >= max_value) break;
      if (entry.first > min_value) {
        possible_values.push_back(entry.first);
      }
    }
    possible_values.push_back(max_value);
    DCHECK(std::is_sorted(possible_values.begin(), possible_values.end()));
  }

  for (const IntegerValue value : possible_values) {
    const std::pair<IntegerVariable, IntegerValue> key{var, value};
    const auto it = equality_to_associated_literal_.find(key);
    if (it == equality_to_associated_literal_.end()) continue;
    const Literal literal = it->second;
    if (sat_solver_->Assignment().LiteralIsTrue(literal)) {
      return {{value, literal}};
    } else if (!sat_solver_->Assignment().LiteralIsFalse(literal)) {
      encoding.push_back({value, literal});
    }
  }
  return encoding;
}

// Note that by not inserting the literal in "order" we can in the worst case
// use twice as much implication (2 by literals) instead of only one between
// consecutive literals.
void IntegerEncoder::AddImplications(IntegerLiteral i_lit,
                                     Literal associated_lit) {
  if (i_lit.var >= encoding_by_var_.size()) {
    encoding_by_var_.resize(i_lit.var.value() + 1);
  }

  std::map<IntegerValue, Literal>& map_ref =
      encoding_by_var_[IntegerVariable(i_lit.var)];
  CHECK(!gtl::ContainsKey(map_ref, i_lit.bound));

  if (add_implications_) {
    auto after_it = map_ref.lower_bound(i_lit.bound);
    if (after_it != map_ref.end()) {
      // Literal(after) => associated_lit
      if (sat_solver_->CurrentDecisionLevel() == 0) {
        sat_solver_->AddBinaryClause(after_it->second.Negated(),
                                     associated_lit);
      } else {
        sat_solver_->AddBinaryClauseDuringSearch(after_it->second.Negated(),
                                                 associated_lit);
      }
    }
    if (after_it != map_ref.begin()) {
      // associated_lit => Literal(before)
      if (sat_solver_->CurrentDecisionLevel() == 0) {
        sat_solver_->AddBinaryClause(associated_lit.Negated(),
                                     (--after_it)->second);
      } else {
        sat_solver_->AddBinaryClauseDuringSearch(associated_lit.Negated(),
                                                 (--after_it)->second);
      }
    }
  }

  // Add the new entry.
  map_ref[i_lit.bound] = associated_lit;
}

void IntegerEncoder::AddAllImplicationsBetweenAssociatedLiterals() {
  CHECK_EQ(0, sat_solver_->CurrentDecisionLevel());
  add_implications_ = true;
  for (const std::map<IntegerValue, Literal>& encoding : encoding_by_var_) {
    LiteralIndex previous = kNoLiteralIndex;
    for (const auto value_literal : encoding) {
      const Literal lit = value_literal.second;
      if (previous != kNoLiteralIndex) {
        // lit => previous.
        sat_solver_->AddBinaryClause(lit.Negated(), Literal(previous));
      }
      previous = lit.Index();
    }
  }
}

std::pair<IntegerLiteral, IntegerLiteral> IntegerEncoder::Canonicalize(
    IntegerLiteral i_lit) const {
  const IntegerVariable var(i_lit.var);
  IntegerValue after(i_lit.bound);
  IntegerValue before(i_lit.bound - 1);
  CHECK_GE(before, (*domains_)[var].Min());
  CHECK_LE(after, (*domains_)[var].Max());
  int64 previous = kint64min;
  for (const ClosedInterval& interval : (*domains_)[var]) {
    if (before > previous && before < interval.start) before = previous;
    if (after > previous && after < interval.start) after = interval.start;
    if (after <= interval.end) break;
    previous = interval.end;
  }
  return {IntegerLiteral::GreaterOrEqual(var, after),
          IntegerLiteral::LowerOrEqual(var, before)};
}

Literal IntegerEncoder::GetOrCreateAssociatedLiteral(IntegerLiteral i_lit) {
  if (i_lit.bound <= (*domains_)[i_lit.var].Min()) {
    return GetTrueLiteral();
  }
  if (i_lit.bound > (*domains_)[i_lit.var].Max()) {
    return GetFalseLiteral();
  }

  const auto canonicalization = Canonicalize(i_lit);
  const IntegerLiteral new_lit = canonicalization.first;
  if (LiteralIsAssociated(new_lit)) {
    return Literal(GetAssociatedLiteral(new_lit));
  }
  if (LiteralIsAssociated(canonicalization.second)) {
    return Literal(GetAssociatedLiteral(canonicalization.second)).Negated();
  }

  ++num_created_variables_;
  const Literal literal(sat_solver_->NewBooleanVariable(), true);
  AssociateToIntegerLiteral(literal, new_lit);
  return literal;
}

Literal IntegerEncoder::GetOrCreateLiteralAssociatedToEquality(
    IntegerVariable var, IntegerValue value) {
  {
    const std::pair<IntegerVariable, IntegerValue> key{var, value};
    const auto it = equality_to_associated_literal_.find(key);
    if (it != equality_to_associated_literal_.end()) {
      return it->second;
    }
  }

  ++num_created_variables_;
  const Literal literal(sat_solver_->NewBooleanVariable(), true);
  AssociateToIntegerEqualValue(literal, var, value);
  return literal;
}

void IntegerEncoder::AssociateToIntegerLiteral(Literal literal,
                                               IntegerLiteral i_lit) {
  const auto& domain = (*domains_)[i_lit.var];
  const IntegerValue min(domain.Min());
  const IntegerValue max(domain.Max());
  if (i_lit.bound <= min) {
    sat_solver_->AddUnitClause(literal);
  } else if (i_lit.bound > max) {
    sat_solver_->AddUnitClause(literal.Negated());
  } else {
    const auto pair = Canonicalize(i_lit);
    HalfAssociateGivenLiteral(pair.first, literal);
    HalfAssociateGivenLiteral(pair.second, literal.Negated());

    // Detect the case >= max or <= min and properly register them. Note that
    // both cases will happen at the same time if there is just two possible
    // value in the domain.
    if (pair.first.bound == max) {
      AssociateToIntegerEqualValue(literal, i_lit.var, max);
    }
    if (-pair.second.bound == min) {
      AssociateToIntegerEqualValue(literal.Negated(), i_lit.var, min);
    }
  }
}

void IntegerEncoder::AssociateToIntegerEqualValue(Literal literal,
                                                  IntegerVariable var,
                                                  IntegerValue value) {
  // Detect literal view. Note that the same literal can be associated to more
  // than one variable, and thus already have a view. We don't change it in
  // this case.
  const Domain& domain = (*domains_)[var];
  if (value == 1 && domain.Min() >= 0 && domain.Max() <= 1) {
    if (literal.Index() >= literal_view_.size()) {
      literal_view_.resize(literal.Index().value() + 1, kNoIntegerVariable);
      literal_view_[literal.Index()] = var;
    } else if (literal_view_[literal.Index()] == kNoIntegerVariable) {
      literal_view_[literal.Index()] = var;
    }
  }
  if (value == -1 && domain.Min() >= -1 && domain.Max() <= 0) {
    if (literal.Index() >= literal_view_.size()) {
      literal_view_.resize(literal.Index().value() + 1, kNoIntegerVariable);
      literal_view_[literal.Index()] = NegationOf(var);
    } else if (literal_view_[literal.Index()] == kNoIntegerVariable) {
      literal_view_[literal.Index()] = NegationOf(var);
    }
  }

  const std::pair<IntegerVariable, IntegerValue> key{var, value};
  if (gtl::ContainsKey(equality_to_associated_literal_, key)) {
    // If this key is already associated, make the two literals equal.
    const Literal representative = equality_to_associated_literal_[key];
    if (representative != literal) {
      DCHECK_EQ(sat_solver_->CurrentDecisionLevel(), 0);
      sat_solver_->AddBinaryClause(literal, representative.Negated());
      sat_solver_->AddBinaryClause(literal.Negated(), representative);
    }
    return;
  }
  equality_to_associated_literal_[key] = literal;
  equality_to_associated_literal_[{NegationOf(var), -value}] = literal;

  // Fix literal for value outside the domain or for singleton domain.
  if (!domain.Contains(value.value())) {
    sat_solver_->AddUnitClause(literal.Negated());
    return;
  }
  if (value == domain.Min() && value == domain.Max()) {
    sat_solver_->AddUnitClause(literal);
    return;
  }

  // Special case for the first and last value.
  if (value == domain.Min()) {
    // Note that this will recursively call AssociateToIntegerEqualValue() but
    // since equality_to_associated_literal_[] is now set, the recursion will
    // stop there. When a domain has just 2 values, this allows to call just
    // once AssociateToIntegerEqualValue() and also associate the other value to
    // the negation of the given literal.
    AssociateToIntegerLiteral(literal,
                              IntegerLiteral::LowerOrEqual(var, value));
    return;
  }
  if (value == domain.Max()) {
    AssociateToIntegerLiteral(literal,
                              IntegerLiteral::GreaterOrEqual(var, value));
    return;
  }

  // (var == value)  <=>  (var >= value) and (var <= value).
  const Literal a(
      GetOrCreateAssociatedLiteral(IntegerLiteral::GreaterOrEqual(var, value)));
  const Literal b(
      GetOrCreateAssociatedLiteral(IntegerLiteral::LowerOrEqual(var, value)));
  sat_solver_->AddBinaryClause(a, literal.Negated());
  sat_solver_->AddBinaryClause(b, literal.Negated());
  sat_solver_->AddProblemClause({a.Negated(), b.Negated(), literal});
}

// TODO(user): The hard constraints we add between associated literals seems to
// work for optional variables, but I am not 100% sure why!! I think it works
// because these literals can only appear in a conflict if the presence literal
// of the optional variables is true.
void IntegerEncoder::HalfAssociateGivenLiteral(IntegerLiteral i_lit,
                                               Literal literal) {
  // Resize reverse encoding.
  const int new_size = 1 + literal.Index().value();
  if (new_size > reverse_encoding_.size()) reverse_encoding_.resize(new_size);

  // Associate the new literal to i_lit.
  if (!LiteralIsAssociated(i_lit)) {
    AddImplications(i_lit, literal);
    if (sat_solver_->Assignment().LiteralIsTrue(literal)) {
      CHECK_EQ(sat_solver_->CurrentDecisionLevel(), 0);
      newly_fixed_integer_literals_.push_back(i_lit);
    }
    reverse_encoding_[literal.Index()].push_back(i_lit);
  } else {
    const Literal associated(GetAssociatedLiteral(i_lit));
    if (associated != literal) {
      DCHECK_EQ(sat_solver_->CurrentDecisionLevel(), 0);
      sat_solver_->AddBinaryClause(literal, associated.Negated());
      sat_solver_->AddBinaryClause(literal.Negated(), associated);
    }
  }
}

bool IntegerEncoder::LiteralIsAssociated(IntegerLiteral i) const {
  if (i.var >= encoding_by_var_.size()) return false;
  const std::map<IntegerValue, Literal>& encoding = encoding_by_var_[i.var];
  return encoding.find(i.bound) != encoding.end();
}

LiteralIndex IntegerEncoder::GetAssociatedLiteral(IntegerLiteral i) {
  if (i.var >= encoding_by_var_.size()) return kNoLiteralIndex;
  const std::map<IntegerValue, Literal>& encoding = encoding_by_var_[i.var];
  const auto result = encoding.find(i.bound);
  if (result == encoding.end()) return kNoLiteralIndex;
  return result->second.Index();
}

LiteralIndex IntegerEncoder::SearchForLiteralAtOrBefore(
    IntegerLiteral i) const {
  // We take the element before the upper_bound() which is either the encoding
  // of i if it already exists, or the encoding just before it.
  if (i.var >= encoding_by_var_.size()) return kNoLiteralIndex;
  const std::map<IntegerValue, Literal>& encoding = encoding_by_var_[i.var];
  auto after_it = encoding.upper_bound(i.bound);
  if (after_it == encoding.begin()) return kNoLiteralIndex;
  --after_it;
  return after_it->second.Index();
}

bool IntegerTrail::Propagate(Trail* trail) {
  const int level = trail->CurrentDecisionLevel();
  for (ReversibleInterface* rev : reversible_classes_) rev->SetLevel(level);

  // Make sure that our internal "integer_search_levels_" size matches the
  // sat decision levels. At the level zero, integer_search_levels_ should
  // be empty.
  if (level > integer_search_levels_.size()) {
    integer_search_levels_.push_back(integer_trail_.size());
    reason_decision_levels_.push_back(literals_reason_starts_.size());
    CHECK_EQ(trail->CurrentDecisionLevel(), integer_search_levels_.size());
  }

  // This is used to map any integer literal out of the initial variable domain
  // into one that use one of the domain value.
  var_to_current_lb_interval_index_.SetLevel(level);

  // This is required because when loading a model it is possible that we add
  // (literal <-> integer literal) associations for literals that have already
  // been propagated here. This often happens when the presolve is off
  // and many variables are fixed.
  //
  // TODO(user): refactor the interaction IntegerTrail <-> IntegerEncoder so
  // that we can just push right away such literal. Unfortunately, this is is
  // a big chunck of work.
  if (level == 0) {
    for (const IntegerLiteral i_lit : encoder_->NewlyFixedIntegerLiterals()) {
      if (IsCurrentlyIgnored(i_lit.var)) continue;
      if (!Enqueue(i_lit, {}, {})) return false;
    }
    encoder_->ClearNewlyFixedIntegerLiterals();
  }

  // Process all the "associated" literals and Enqueue() the corresponding
  // bounds.
  while (propagation_trail_index_ < trail->Index()) {
    const Literal literal = (*trail)[propagation_trail_index_++];
    for (const IntegerLiteral i_lit : encoder_->GetIntegerLiterals(literal)) {
      if (IsCurrentlyIgnored(i_lit.var)) continue;

      // The reason is simply the associated literal.
      if (!Enqueue(i_lit, {literal.Negated()}, {})) return false;
    }
  }

  return true;
}

void IntegerTrail::Untrail(const Trail& trail, int literal_trail_index) {
  const int level = trail.CurrentDecisionLevel();
  for (ReversibleInterface* rev : reversible_classes_) rev->SetLevel(level);
  var_to_current_lb_interval_index_.SetLevel(level);
  propagation_trail_index_ =
      std::min(propagation_trail_index_, literal_trail_index);

  // Note that if a conflict was detected before Propagate() of this class was
  // even called, it is possible that there is nothing to backtrack.
  if (level >= integer_search_levels_.size()) return;
  const int target = integer_search_levels_[level];
  integer_search_levels_.resize(level);
  CHECK_GE(target, vars_.size());
  CHECK_LE(target, integer_trail_.size());

  for (int index = integer_trail_.size() - 1; index >= target; --index) {
    const TrailEntry& entry = integer_trail_[index];
    if (entry.var < 0) continue;  // entry used by EnqueueLiteral().
    vars_[entry.var].current_trail_index = entry.prev_trail_index;
    vars_[entry.var].current_bound =
        integer_trail_[entry.prev_trail_index].bound;
  }
  integer_trail_.resize(target);

  // Clear reason.
  const int old_size = reason_decision_levels_[level];
  reason_decision_levels_.resize(level);
  if (old_size < literals_reason_starts_.size()) {
    literals_reason_buffer_.resize(literals_reason_starts_[old_size]);
    bounds_reason_buffer_.resize(bounds_reason_starts_[old_size]);
    literals_reason_starts_.resize(old_size);
    bounds_reason_starts_.resize(old_size);
  }
}

IntegerVariable IntegerTrail::AddIntegerVariable(IntegerValue lower_bound,
                                                 IntegerValue upper_bound) {
  CHECK_GE(lower_bound, kMinIntegerValue);
  CHECK_LE(lower_bound, kMaxIntegerValue);
  CHECK_GE(upper_bound, kMinIntegerValue);
  CHECK_LE(upper_bound, kMaxIntegerValue);
  CHECK(integer_search_levels_.empty());
  CHECK_EQ(vars_.size(), integer_trail_.size());

  const IntegerVariable i(vars_.size());
  is_ignored_literals_.push_back(kNoLiteralIndex);
  vars_.push_back({lower_bound, static_cast<int>(integer_trail_.size())});
  var_trail_index_cache_.push_back(integer_trail_.size());
  integer_trail_.push_back({lower_bound, i});
  domains_->push_back(Domain(lower_bound.value(), upper_bound.value()));

  // TODO(user): the is_ignored_literals_ Booleans are currently always the same
  // for a variable and its negation. So it may be better not to store it twice
  // so that we don't have to be careful when setting them.
  CHECK_EQ(NegationOf(i).value(), vars_.size());
  is_ignored_literals_.push_back(kNoLiteralIndex);
  vars_.push_back({-upper_bound, static_cast<int>(integer_trail_.size())});
  var_trail_index_cache_.push_back(integer_trail_.size());
  integer_trail_.push_back({-upper_bound, NegationOf(i)});
  domains_->push_back(Domain(-upper_bound.value(), -lower_bound.value()));

  for (SparseBitset<IntegerVariable>* w : watchers_) {
    w->Resize(NumIntegerVariables());
  }
  return i;
}

IntegerVariable IntegerTrail::AddIntegerVariable(const Domain& domain) {
  CHECK(!domain.IsEmpty());
  const IntegerVariable var = AddIntegerVariable(IntegerValue(domain.Min()),
                                                 IntegerValue(domain.Max()));
  CHECK(UpdateInitialDomain(var, domain));
  return var;
}

const Domain& IntegerTrail::InitialVariableDomain(IntegerVariable var) const {
  return (*domains_)[var];
}

bool IntegerTrail::UpdateInitialDomain(IntegerVariable var, Domain domain) {
  CHECK_EQ(trail_->CurrentDecisionLevel(), 0);

  // TODO(user): A bit inefficient as this recreate a vector for no reason.
  // The IntersectionOfSortedDisjointIntervals() should take a Span<> instead.
  const Domain& old_domain = InitialVariableDomain(var);
  domain = domain.IntersectionWith(old_domain);
  if (old_domain == domain) return true;
  if (domain.IsEmpty()) return false;

  (*domains_)[var] = domain;
  (*domains_)[NegationOf(var)] = domain.Negation();

  if (domain.NumIntervals() > 1) {
    var_to_current_lb_interval_index_.Set(var, 0);
    var_to_current_lb_interval_index_.Set(NegationOf(var), 0);
  }

  // TODO(user): That works, but it might be better to simply update the
  // bounds here directly. This is because these function might call again
  // UpdateInitialDomain(), and we will abort after realizing that the domain
  // didn't change this time.
  CHECK(Enqueue(IntegerLiteral::GreaterOrEqual(var, IntegerValue(domain.Min())),
                {}, {}));
  CHECK(Enqueue(IntegerLiteral::LowerOrEqual(var, IntegerValue(domain.Max())),
                {}, {}));

  // Set to false excluded literals.
  // TODO(user): This is only needed to propagate holes and is a bit slow, I am
  // not sure it is worthwhile.
  int i = 0;
  int num_fixed = 0;
  const auto encoding = encoder_->PartialDomainEncoding(var);
  for (const auto pair : encoding) {
    while (i < domain.NumIntervals() && pair.value > domain[i].end) ++i;
    if (i == domain.NumIntervals() || pair.value < domain[i].start) {
      // Set the literal to false;
      ++num_fixed;
      if (trail_->Assignment().LiteralIsTrue(pair.literal)) return false;
      if (!trail_->Assignment().LiteralIsFalse(pair.literal)) {
        trail_->EnqueueWithUnitReason(pair.literal.Negated());
      }
    }
  }
  if (num_fixed > 0) {
    VLOG(1) << "Domain intersection removed " << num_fixed << " values "
            << "(out of " << encoding.size() << ").";
  }

  return true;
}

IntegerVariable IntegerTrail::GetOrCreateConstantIntegerVariable(
    IntegerValue value) {
  auto insert = constant_map_.insert(std::make_pair(value, kNoIntegerVariable));
  if (insert.second) {  // new element.
    const IntegerVariable new_var = AddIntegerVariable(value, value);
    insert.first->second = new_var;
    if (value != 0) {
      // Note that this might invalidate insert.first->second.
      gtl::InsertOrDie(&constant_map_, -value, NegationOf(new_var));
    }
    return new_var;
  }
  return insert.first->second;
}

int IntegerTrail::NumConstantVariables() const {
  // The +1 if for the special key zero (the only case when we have an odd
  // number of entries).
  return (constant_map_.size() + 1) / 2;
}

int IntegerTrail::FindLowestTrailIndexThatExplainBound(
    IntegerLiteral i_lit) const {
  DCHECK_LE(i_lit.bound, vars_[i_lit.var].current_bound);
  if (i_lit.bound <= LevelZeroBound(i_lit.var)) return -1;
  int trail_index = vars_[i_lit.var].current_trail_index;

  // Check the validity of the cached index and use it if possible. This caching
  // mechanism is important in case of long chain of propagation on the same
  // variable. Because during conflict resolution, we call
  // FindLowestTrailIndexThatExplainBound() with lowest and lowest bound, this
  // cache can transform a quadratic complexity into a linear one.
  {
    const int cached_index = var_trail_index_cache_[i_lit.var];
    if (cached_index < trail_index) {
      const TrailEntry& entry = integer_trail_[cached_index];
      if (entry.var == i_lit.var && entry.bound >= i_lit.bound) {
        trail_index = cached_index;
      }
    }
  }

  int prev_trail_index = trail_index;
  while (true) {
    const TrailEntry& entry = integer_trail_[trail_index];
    if (entry.bound == i_lit.bound) {
      var_trail_index_cache_[i_lit.var] = trail_index;
      return trail_index;
    }
    if (entry.bound < i_lit.bound) {
      var_trail_index_cache_[i_lit.var] = prev_trail_index;
      return prev_trail_index;
    }
    prev_trail_index = trail_index;
    trail_index = entry.prev_trail_index;
  }
}

// We try to relax the reason in a smart way here by minimizing the maximum
// trail indices of the literals appearing in reason.
//
// TODO(user): use priority queue instead of O(n^2) algo.
void IntegerTrail::RelaxLinearReason(
    IntegerValue slack, absl::Span<const IntegerValue> coeffs,
    std::vector<IntegerLiteral>* reason) const {
  CHECK_GE(slack, 0);
  if (slack == 0) return;
  const int size = reason->size();
  std::vector<int> indices(size);
  for (int i = 0; i < size; ++i) {
    CHECK_EQ((*reason)[i].bound, LowerBound((*reason)[i].var));
    CHECK_GE(coeffs[i], 0);
    indices[i] = vars_[(*reason)[i].var].current_trail_index;
  }

  const int num_vars = vars_.size();
  while (slack != 0) {
    int best_i = -1;
    for (int i = 0; i < size; ++i) {
      if (indices[i] < num_vars) continue;  // level zero.
      if (best_i != -1 && indices[i] < indices[best_i]) continue;
      const TrailEntry& entry = integer_trail_[indices[i]];
      const TrailEntry& previous_entry = integer_trail_[entry.prev_trail_index];

      // Note that both terms of the product are positive.
      if (CapProd(coeffs[i].value(),
                  (entry.bound - previous_entry.bound).value()) > slack) {
        continue;
      }
      best_i = i;
    }
    if (best_i == -1) return;

    const TrailEntry& entry = integer_trail_[indices[best_i]];
    const TrailEntry& previous_entry = integer_trail_[entry.prev_trail_index];
    indices[best_i] = entry.prev_trail_index;
    (*reason)[best_i].bound = previous_entry.bound;
    slack -= coeffs[best_i] * (entry.bound - previous_entry.bound);
  }
}

void IntegerTrail::RemoveLevelZeroBounds(
    std::vector<IntegerLiteral>* reason) const {
  int new_size = 0;
  for (const IntegerLiteral literal : *reason) {
    if (literal.bound <= LevelZeroBound(literal.var)) continue;
    (*reason)[new_size++] = literal;
  }
  reason->resize(new_size);
}

bool IntegerTrail::EnqueueAssociatedLiteral(
    Literal literal, int trail_index_with_same_reason,
    absl::Span<const Literal> literal_reason,
    absl::Span<const IntegerLiteral> integer_reason,
    BooleanVariable* variable_with_same_reason) {
  if (!trail_->Assignment().VariableIsAssigned(literal.Variable())) {
    if (integer_search_levels_.empty()) {
      trail_->EnqueueWithUnitReason(literal);
      return true;
    }

    if (*variable_with_same_reason != kNoBooleanVariable) {
      trail_->EnqueueWithSameReasonAs(literal, *variable_with_same_reason);
      return true;
    }
    *variable_with_same_reason = literal.Variable();

    // Subtle: the reason is the same as i_lit, that we will enqueue if no
    // conflict occur at position integer_trail_.size(), so we just refer to
    // this index here. See EnqueueLiteral().
    const int trail_index = trail_->Index();
    if (trail_index >= boolean_trail_index_to_integer_one_.size()) {
      boolean_trail_index_to_integer_one_.resize(trail_index + 1);
    }
    boolean_trail_index_to_integer_one_[trail_index] =
        trail_index_with_same_reason;
    trail_->Enqueue(literal, propagator_id_);
    return true;
  }
  if (trail_->Assignment().LiteralIsFalse(literal)) {
    std::vector<Literal>* conflict = trail_->MutableConflict();
    conflict->assign(literal_reason.begin(), literal_reason.end());

    // This is tricky, in some corner cases, the same Enqueue() will call
    // EnqueueAssociatedLiteral() on a literal and its opposite. In this case,
    // we don't want to have this in the conflict.
    const AssignmentInfo& info =
        trail_->Info(trail_->ReferenceVarWithSameReason(literal.Variable()));
    if (info.type != propagator_id_ ||
        info.trail_index >= boolean_trail_index_to_integer_one_.size() ||
        boolean_trail_index_to_integer_one_[info.trail_index] !=
            integer_trail_.size()) {
      conflict->push_back(literal);
    }
    MergeReasonInto(integer_reason, conflict);
    return false;
  }
  return true;
}

namespace {

std::string ReasonDebugString(absl::Span<const Literal> literal_reason,
                              absl::Span<const IntegerLiteral> integer_reason) {
  std::string result = "literals:{";
  for (const Literal l : literal_reason) {
    if (result.back() != '{') result += ",";
    result += l.DebugString();
  }
  result += "} bounds:{";
  for (const IntegerLiteral l : integer_reason) {
    if (result.back() != '{') result += ",";
    result += l.DebugString();
  }
  result += "}";
  return result;
}

}  // namespace

std::string IntegerTrail::DebugString() {
  std::string result = "trail:{";
  const int num_vars = vars_.size();
  const int limit =
      std::min(num_vars + 30, static_cast<int>(integer_trail_.size()));
  for (int i = num_vars; i < limit; ++i) {
    if (result.back() != '{') result += ",";
    result +=
        IntegerLiteral::GreaterOrEqual(IntegerVariable(integer_trail_[i].var),
                                       integer_trail_[i].bound)
            .DebugString();
  }
  if (limit < integer_trail_.size()) {
    result += ", ...";
  }
  result += "}";
  return result;
}

bool IntegerTrail::Enqueue(IntegerLiteral i_lit,
                           absl::Span<const Literal> literal_reason,
                           absl::Span<const IntegerLiteral> integer_reason) {
  return Enqueue(i_lit, literal_reason, integer_reason, integer_trail_.size());
}

bool IntegerTrail::ReasonIsValid(
    absl::Span<const Literal> literal_reason,
    absl::Span<const IntegerLiteral> integer_reason) {
  const VariablesAssignment& assignment = trail_->Assignment();
  for (const Literal lit : literal_reason) {
    if (!assignment.LiteralIsFalse(lit)) return false;
  }
  for (const IntegerLiteral i_lit : integer_reason) {
    if (i_lit.bound > vars_[i_lit.var].current_bound) {
      if (IsOptional(i_lit.var)) {
        const Literal is_ignored = IsIgnoredLiteral(i_lit.var);
        LOG(INFO) << "Reason " << i_lit << " is not true!"
                  << " optional variable:" << i_lit.var
                  << " present:" << assignment.LiteralIsFalse(is_ignored)
                  << " absent:" << assignment.LiteralIsTrue(is_ignored)
                  << " current_lb:" << vars_[i_lit.var].current_bound;
      } else {
        LOG(INFO) << "Reason " << i_lit << " is not true!"
                  << " non-optional variable:" << i_lit.var
                  << " current_lb:" << vars_[i_lit.var].current_bound;
      }
      return false;
    }
  }

  // This may not indicate an incorectness, but just some propagators that
  // didn't reach a fixed-point at level zero.
  if (!integer_search_levels_.empty()) {
    int num_literal_assigned_after_root_node = 0;
    for (const Literal lit : literal_reason) {
      if (trail_->Info(lit.Variable()).level > 0) {
        num_literal_assigned_after_root_node++;
      }
    }
    for (const IntegerLiteral i_lit : integer_reason) {
      if (LevelZeroBound(i_lit.var) < i_lit.bound) {
        num_literal_assigned_after_root_node++;
      }
    }
    LOG_IF(WARNING, num_literal_assigned_after_root_node == 0)
        << "Propagating a literal with no reason at a positive level!\n"
        << "level:" << integer_search_levels_.size() << " "
        << ReasonDebugString(literal_reason, integer_reason) << "\n"
        << DebugString();
  }

  return true;
}

bool IntegerTrail::Enqueue(IntegerLiteral i_lit,
                           absl::Span<const Literal> literal_reason,
                           absl::Span<const IntegerLiteral> integer_reason,
                           int trail_index_with_same_reason) {
  DCHECK(ReasonIsValid(literal_reason, integer_reason));

  // No point doing work if the variable is already ignored.
  if (IsCurrentlyIgnored(i_lit.var)) return true;

  // Nothing to do if the bound is not better than the current one.
  // TODO(user): Change this to a CHECK? propagator shouldn't try to push such
  // bound and waste time explaining it.
  if (i_lit.bound <= vars_[i_lit.var].current_bound) return true;
  ++num_enqueues_;

  const IntegerVariable var(i_lit.var);

  // If the domain of var is not a single intervals and i_lit.bound fall into a
  // "hole", we increase it to the next possible value. This ensure that we
  // never Enqueue() non-canonical literals. See also Canonicalize().
  //
  // Note: The literals in the reason are not necessarily canonical, but then
  // we always map these to enqueued literals during conflict resolution.
  if ((*domains_)[var].NumIntervals() > 1) {
    const auto& domain = (*domains_)[var];
    int index = var_to_current_lb_interval_index_.FindOrDie(var);
    const int size = domain.NumIntervals();
    while (index < size && i_lit.bound > domain[index].end) {
      ++index;
    }
    if (index == size) {
      return ReportConflict(literal_reason, integer_reason);
    } else {
      var_to_current_lb_interval_index_.Set(var, index);
      i_lit.bound = std::max(i_lit.bound, IntegerValue(domain[index].start));
    }
  }

  // For the EnqueueWithSameReasonAs() mechanism.
  BooleanVariable first_propagated_variable = kNoBooleanVariable;

  // Check if the integer variable has an empty domain.
  if (i_lit.bound > UpperBound(var)) {
    // We relax the upper bound as much as possible to still have a conflict.
    const auto ub_reason = IntegerLiteral::LowerOrEqual(var, i_lit.bound - 1);

    if (!IsOptional(var) || trail_->Assignment().LiteralIsFalse(
                                Literal(is_ignored_literals_[var]))) {
      std::vector<Literal>* conflict = trail_->MutableConflict();
      conflict->assign(literal_reason.begin(), literal_reason.end());
      if (IsOptional(var)) {
        conflict->push_back(Literal(is_ignored_literals_[var]));
      }

      // This is the same as:
      //   MergeReasonInto(integer_reason, conflict);
      //   MergeReasonInto({ub_reason)}, conflict);
      // but with just one call to MergeReasonIntoInternal() for speed. Note
      // that it may also produce a smaller reason overall.
      DCHECK(tmp_queue_.empty());
      const int size = vars_.size();
      for (const IntegerLiteral& literal : integer_reason) {
        const int trail_index = FindLowestTrailIndexThatExplainBound(literal);
        if (trail_index >= size) tmp_queue_.push_back(trail_index);
      }
      {
        const int trail_index = FindLowestTrailIndexThatExplainBound(ub_reason);
        if (trail_index >= size) tmp_queue_.push_back(trail_index);
      }
      MergeReasonIntoInternal(conflict);
      return false;
    } else {
      // Note(user): We never make the bound of an optional literal cross. We
      // used to have a bug where we propagated these bounds and their
      // associated literals, and we were reaching a conflict while propagating
      // the associated literal instead of setting is_ignored below to false.
      const Literal is_ignored = Literal(is_ignored_literals_[var]);
      if (integer_search_levels_.empty()) {
        trail_->EnqueueWithUnitReason(is_ignored);
      } else {
        EnqueueLiteral(is_ignored, literal_reason, integer_reason);
        bounds_reason_buffer_.push_back(ub_reason);
      }
      return true;
    }
  }

  // Notify the watchers.
  for (SparseBitset<IntegerVariable>* bitset : watchers_) {
    bitset->Set(i_lit.var);
  }

  // Enqueue the strongest associated Boolean literal implied by this one.
  // Because we linked all such literal with implications, all the one before
  // will be propagated by the SAT solver.
  //
  // TODO(user): It might be simply better and more efficient to simply enqueue
  // all of them here. We have also more liberty to choose the explanation we
  // want. A drawback might be that the implications might not be used in the
  // binary conflict minimization algo.
  const LiteralIndex literal_index =
      encoder_->SearchForLiteralAtOrBefore(i_lit);
  if (literal_index != kNoLiteralIndex) {
    if (!EnqueueAssociatedLiteral(Literal(literal_index), integer_trail_.size(),
                                  literal_reason, integer_reason,
                                  &first_propagated_variable)) {
      return false;
    }
  }

  // Special case for level zero.
  if (integer_search_levels_.empty()) {
    vars_[i_lit.var].current_bound = i_lit.bound;
    integer_trail_[i_lit.var.value()].bound = i_lit.bound;

    // We also update the initial domain. If this fail, since we are at level
    // zero, we don't care about the reason.
    trail_->MutableConflict()->clear();
    return UpdateInitialDomain(
        i_lit.var,
        Domain(LowerBound(i_lit.var).value(), UpperBound(i_lit.var).value()));
  }
  DCHECK_GT(trail_->CurrentDecisionLevel(), 0);

  int reason_index = literals_reason_starts_.size();
  if (trail_index_with_same_reason >= integer_trail_.size()) {
    // Save the reason into our internal buffers.
    literals_reason_starts_.push_back(literals_reason_buffer_.size());
    if (!literal_reason.empty()) {
      literals_reason_buffer_.insert(literals_reason_buffer_.end(),
                                     literal_reason.begin(),
                                     literal_reason.end());
    }
    bounds_reason_starts_.push_back(bounds_reason_buffer_.size());
    if (!integer_reason.empty()) {
      CHECK_NE(integer_reason[0].var, kNoIntegerVariable);
      bounds_reason_buffer_.insert(bounds_reason_buffer_.end(),
                                   integer_reason.begin(),
                                   integer_reason.end());
    }
  } else {
    reason_index = integer_trail_[trail_index_with_same_reason].reason_index;
  }

  integer_trail_.push_back(
      {/*bound=*/i_lit.bound,
       /*var=*/i_lit.var,
       /*prev_trail_index=*/vars_[i_lit.var].current_trail_index,
       reason_index});

  vars_[i_lit.var].current_bound = i_lit.bound;
  vars_[i_lit.var].current_trail_index = integer_trail_.size() - 1;
  return true;
}

util::BeginEndWrapper<std::vector<IntegerLiteral>::const_iterator>
IntegerTrail::Dependencies(int trail_index) const {
  const int reason_index = integer_trail_[trail_index].reason_index;
  const int start = bounds_reason_starts_[reason_index];
  const int end = reason_index + 1 < bounds_reason_starts_.size()
                      ? bounds_reason_starts_[reason_index + 1]
                      : bounds_reason_buffer_.size();
  if (start < end && bounds_reason_buffer_[start].var >= 0) {
    // HACK. This is a critical code, so we reuse the IntegerLiteral.var to
    // store the result of FindLowestTrailIndexThatExplainBound() applied to all
    // the IntegerLiteral.
    //
    // To detect if we already did the computation, we store the negated index.
    // Note that we will redo the computation in the corner case where all the
    // given IntegerLiterals turn out to be assigned at level zero.
    //
    // TODO(user): We could check that the same IntegerVariable never appear
    // twice. And if it does the one with the lowest bound could be removed.
    int out = start;
    const int size = vars_.size();
    for (int i = start; i < end; ++i) {
      const int dep =
          FindLowestTrailIndexThatExplainBound(bounds_reason_buffer_[i]);
      if (dep >= size) bounds_reason_buffer_[out++].var = -dep;
    }
  }
  const std::vector<IntegerLiteral>::const_iterator b =
      bounds_reason_buffer_.begin() + start;
  const std::vector<IntegerLiteral>::const_iterator e =
      bounds_reason_buffer_.begin() + end;
  return util::BeginEndRange(b, e);
}

void IntegerTrail::AppendLiteralsReason(int trail_index,
                                        std::vector<Literal>* output) const {
  const int reason_index = integer_trail_[trail_index].reason_index;
  const int start = literals_reason_starts_[reason_index];
  const int end = reason_index + 1 < literals_reason_starts_.size()
                      ? literals_reason_starts_[reason_index + 1]
                      : literals_reason_buffer_.size();
  for (int i = start; i < end; ++i) {
    const Literal l = literals_reason_buffer_[i];
    if (!added_variables_[l.Variable()]) {
      added_variables_.Set(l.Variable());
      output->push_back(l);
    }
  }
}

std::vector<Literal> IntegerTrail::ReasonFor(IntegerLiteral literal) const {
  std::vector<Literal> reason;
  MergeReasonInto({literal}, &reason);
  return reason;
}

// TODO(user): If this is called many time on the same variables, it could be
// made faster by using some caching mecanism.
void IntegerTrail::MergeReasonInto(absl::Span<const IntegerLiteral> literals,
                                   std::vector<Literal>* output) const {
  DCHECK(tmp_queue_.empty());
  const int size = vars_.size();
  for (const IntegerLiteral& literal : literals) {
    const int trail_index = FindLowestTrailIndexThatExplainBound(literal);

    // Any indices lower than that means that there is no reason needed.
    // Note that it is important for size to be signed because of -1 indices.
    if (trail_index >= size) tmp_queue_.push_back(trail_index);
  }
  return MergeReasonIntoInternal(output);
}

// This will expand the reason of the IntegerLiteral already in tmp_queue_ until
// everything is explained in term of Literal.
void IntegerTrail::MergeReasonIntoInternal(std::vector<Literal>* output) const {
  // All relevant trail indices will be >= vars_.size(), so we can safely use
  // zero to means that no literal refering to this variable is in the queue.
  tmp_var_to_trail_index_in_queue_.resize(vars_.size(), 0);
  DCHECK(std::all_of(tmp_var_to_trail_index_in_queue_.begin(),
                     tmp_var_to_trail_index_in_queue_.end(),
                     [](int v) { return v == 0; }));

  added_variables_.ClearAndResize(BooleanVariable(trail_->NumVariables()));
  for (const Literal l : *output) {
    added_variables_.Set(l.Variable());
  }

  // During the algorithm execution, all the queue entries that do not match the
  // content of tmp_var_to_trail_index_in_queue_[] will be ignored.
  for (const int trail_index : tmp_queue_) {
    const TrailEntry& entry = integer_trail_[trail_index];
    tmp_var_to_trail_index_in_queue_[entry.var] =
        std::max(tmp_var_to_trail_index_in_queue_[entry.var], trail_index);
  }

  // We manage our heap by hand so that we can range iterate over it above, and
  // this initial heapify is faster.
  std::make_heap(tmp_queue_.begin(), tmp_queue_.end());

  // We process the entries by highest trail_index first. The content of the
  // queue will always be a valid reason for the literals we already added to
  // the output.
  tmp_to_clear_.clear();
  while (!tmp_queue_.empty()) {
    const int trail_index = tmp_queue_.front();
    const TrailEntry& entry = integer_trail_[trail_index];
    std::pop_heap(tmp_queue_.begin(), tmp_queue_.end());
    tmp_queue_.pop_back();

    // Skip any stale queue entry. Amongst all the entry refering to a given
    // variable, only the latest added to the queue is valid and we detect it
    // using its trail index.
    if (tmp_var_to_trail_index_in_queue_[entry.var] != trail_index) {
      continue;
    }

    // If this entry has an associated literal, then we use it as a reason
    // instead of the stored reason. If later this literal needs to be
    // explained, then the associated literal will be expanded with the stored
    // reason.
    {
      const LiteralIndex associated_lit =
          encoder_->GetAssociatedLiteral(IntegerLiteral::GreaterOrEqual(
              IntegerVariable(entry.var), entry.bound));
      if (associated_lit != kNoLiteralIndex) {
        output->push_back(Literal(associated_lit).Negated());

        // Ignore any entries of the queue refering to this variable and make
        // sure no such entry are added later.
        tmp_to_clear_.push_back(entry.var);
        tmp_var_to_trail_index_in_queue_[entry.var] = kint32max;
        continue;
      }
    }

    // Process this entry. Note that if any of the next expansion include the
    // variable entry.var in their reason, we must process it again because we
    // cannot easily detect if it was needed to infer the current entry.
    //
    // Important: the queue might already contains entries refering to the same
    // variable. The code act like if we deleted all of them at this point, we
    // just do that lazily. tmp_var_to_trail_index_in_queue_[var] will
    // only refer to newly added entries.
    AppendLiteralsReason(trail_index, output);
    tmp_var_to_trail_index_in_queue_[entry.var] = 0;

    // TODO(user): we could speed up Dependencies() by using the indices stored
    // in tmp_var_to_trail_index_in_queue_ instead of redoing
    // FindLowestTrailIndexThatExplainBound() from the latest trail index.
    bool has_dependency = false;
    for (const IntegerLiteral lit : Dependencies(trail_index)) {
      // Extract the next_trail_index from the returned literal, we can break
      // as soon as we get a negative next_trail_index. See the encoding in
      // Dependencies().
      const int next_trail_index = -lit.var.value();
      if (next_trail_index < 0) break;
      const TrailEntry& next_entry = integer_trail_[next_trail_index];
      has_dependency = true;

      // Only add literals that are not "implied" by the ones already present.
      // For instance, do not add (x >= 4) if we already have (x >= 7). This
      // translate into only adding a trail index if it is larger than the one
      // in the queue refering to the same variable.
      if (next_trail_index > tmp_var_to_trail_index_in_queue_[next_entry.var]) {
        tmp_var_to_trail_index_in_queue_[next_entry.var] = next_trail_index;
        tmp_queue_.push_back(next_trail_index);
        std::push_heap(tmp_queue_.begin(), tmp_queue_.end());
      }
    }

    // Special case for a "leaf", we will never need this variable again.
    if (!has_dependency) {
      tmp_to_clear_.push_back(entry.var);
      tmp_var_to_trail_index_in_queue_[entry.var] = kint32max;
    }
  }

  // clean-up.
  for (const IntegerVariable var : tmp_to_clear_) {
    tmp_var_to_trail_index_in_queue_[var] = 0;
  }
}

absl::Span<const Literal> IntegerTrail::Reason(const Trail& trail,
                                               int trail_index) const {
  const int index = boolean_trail_index_to_integer_one_[trail_index];
  std::vector<Literal>* reason = trail.GetEmptyVectorToStoreReason(trail_index);
  added_variables_.ClearAndResize(BooleanVariable(trail_->NumVariables()));
  AppendLiteralsReason(index, reason);
  DCHECK(tmp_queue_.empty());
  for (const IntegerLiteral lit : Dependencies(index)) {
    const int next_trail_index = -lit.var.value();
    if (next_trail_index <= 0) break;
    DCHECK_GE(next_trail_index, vars_.size());
    tmp_queue_.push_back(next_trail_index);
  }
  MergeReasonIntoInternal(reason);
  return *reason;
}

void IntegerTrail::EnqueueLiteral(
    Literal literal, absl::Span<const Literal> literal_reason,
    absl::Span<const IntegerLiteral> integer_reason) {
  DCHECK(!trail_->Assignment().LiteralIsAssigned(literal));
  DCHECK(ReasonIsValid(literal_reason, integer_reason));
  if (integer_search_levels_.empty()) {
    // Level zero. We don't keep any reason.
    trail_->EnqueueWithUnitReason(literal);
    return;
  }

  const int trail_index = trail_->Index();
  if (trail_index >= boolean_trail_index_to_integer_one_.size()) {
    boolean_trail_index_to_integer_one_.resize(trail_index + 1);
  }
  boolean_trail_index_to_integer_one_[trail_index] = integer_trail_.size();
  integer_trail_.push_back(
      {/*bound=*/IntegerValue(0), kNoIntegerVariable,
       /*prev_trail_index=*/-1,
       static_cast<int32>(literals_reason_starts_.size())});
  literals_reason_starts_.push_back(literals_reason_buffer_.size());
  literals_reason_buffer_.insert(literals_reason_buffer_.end(),
                                 literal_reason.begin(), literal_reason.end());
  bounds_reason_starts_.push_back(bounds_reason_buffer_.size());
  bounds_reason_buffer_.insert(bounds_reason_buffer_.end(),
                               integer_reason.begin(), integer_reason.end());
  trail_->Enqueue(literal, propagator_id_);
}

// TODO(user): Implement a dense version if there is more trail entries
// than variables!
void IntegerTrail::AppendNewBounds(std::vector<IntegerLiteral>* output) const {
  tmp_marked_.ClearAndResize(IntegerVariable(vars_.size()));
  for (int i = vars_.size(); i < integer_trail_.size(); ++i) {
    const TrailEntry& entry = integer_trail_[i];
    if (entry.var == kNoIntegerVariable) continue;
    if (tmp_marked_[entry.var]) continue;

    tmp_marked_.Set(entry.var);
    output->push_back(IntegerLiteral::GreaterOrEqual(entry.var, entry.bound));
  }
}

GenericLiteralWatcher::GenericLiteralWatcher(Model* model)
    : SatPropagator("GenericLiteralWatcher"),
      integer_trail_(model->GetOrCreate<IntegerTrail>()),
      rev_int_repository_(model->GetOrCreate<RevIntRepository>()) {
  // TODO(user): This propagator currently needs to be last because it is the
  // only one enforcing that a fix-point is reached on the integer variables.
  // Figure out a better interaction between the sat propagation loop and
  // this one.
  model->GetOrCreate<SatSolver>()->AddLastPropagator(this);

  integer_trail_->RegisterWatcher(&modified_vars_);
  queue_by_priority_.resize(2);  // Because default priority is 1.
}

void GenericLiteralWatcher::UpdateCallingNeeds(Trail* trail) {
  // Process any new Literal on the trail.
  while (propagation_trail_index_ < trail->Index()) {
    const Literal literal = (*trail)[propagation_trail_index_++];
    if (literal.Index() >= literal_to_watcher_.size()) continue;
    for (const auto entry : literal_to_watcher_[literal.Index()]) {
      if (!in_queue_[entry.id]) {
        in_queue_[entry.id] = true;
        queue_by_priority_[id_to_priority_[entry.id]].push_back(entry.id);
      }
      if (entry.watch_index >= 0) {
        id_to_watch_indices_[entry.id].push_back(entry.watch_index);
      }
    }
  }

  // Process the newly changed variables lower bounds.
  for (const IntegerVariable var : modified_vars_.PositionsSetAtLeastOnce()) {
    if (var.value() >= var_to_watcher_.size()) continue;
    for (const auto entry : var_to_watcher_[var]) {
      if (!in_queue_[entry.id]) {
        in_queue_[entry.id] = true;
        queue_by_priority_[id_to_priority_[entry.id]].push_back(entry.id);
      }
      if (entry.watch_index >= 0) {
        id_to_watch_indices_[entry.id].push_back(entry.watch_index);
      }
    }
  }
  modified_vars_.ClearAndResize(integer_trail_->NumIntegerVariables());
}

bool GenericLiteralWatcher::Propagate(Trail* trail) {
  const int level = trail->CurrentDecisionLevel();
  UpdateCallingNeeds(trail);

  // Note that the priority may be set to -1 inside the loop in order to restart
  // at zero.
  for (int priority = 0; priority < queue_by_priority_.size(); ++priority) {
    std::deque<int>& queue = queue_by_priority_[priority];
    while (!queue.empty()) {
      const int id = queue.front();
      queue.pop_front();

      // Before we propagate, make sure any reversible structure are up to date.
      // Note that we never do anything expensive more than once per level.
      {
        const int low = id_to_greatest_common_level_since_last_call_[id];
        const int high = id_to_level_at_last_call_[id];
        if (low < high || level > low) {  // Equivalent to not all equal.
          id_to_level_at_last_call_[id] = level;
          id_to_greatest_common_level_since_last_call_[id] = level;
          for (ReversibleInterface* rev : id_to_reversible_classes_[id]) {
            if (low < high) rev->SetLevel(low);
            if (level > low) rev->SetLevel(level);
          }
          for (int* rev_int : id_to_reversible_ints_[id]) {
            rev_int_repository_->SaveState(rev_int);
          }
        }
      }

      // This is needed to detect if the propagator propagated anything or not.
      const int64 old_integer_timestamp = integer_trail_->num_enqueues();
      const int64 old_boolean_timestamp = trail->Index();

      // TODO(user): Maybe just provide one function Propagate(watch_indices) ?
      std::vector<int>& watch_indices_ref = id_to_watch_indices_[id];
      const bool result =
          watch_indices_ref.empty()
              ? watchers_[id]->Propagate()
              : watchers_[id]->IncrementalPropagate(watch_indices_ref);
      if (!result) {
        watch_indices_ref.clear();
        in_queue_[id] = false;
        return false;
      }

      // Update the propagation queue. At this point, the propagator has been
      // removed from the queue but in_queue_ is still true.
      if (id_to_idempotence_[id]) {
        // If the propagator is assumed to be idempotent, then we set in_queue_
        // to false after UpdateCallingNeeds() so this later function will never
        // add it back.
        UpdateCallingNeeds(trail);
        watch_indices_ref.clear();
        in_queue_[id] = false;
      } else {
        // Otherwise, we set in_queue_ to false first so that
        // UpdateCallingNeeds() may add it back if the propagator modified any
        // of its watched variables.
        watch_indices_ref.clear();
        in_queue_[id] = false;
        UpdateCallingNeeds(trail);
      }

      // If the propagator pushed an integer bound, we revert to priority = 0.
      if (integer_trail_->num_enqueues() > old_integer_timestamp) {
        priority = -1;  // Because of the ++priority in the for loop.
      }

      // If the propagator pushed a literal, we have two options.
      if (trail->Index() > old_boolean_timestamp) {
        // Important: for now we need to re-run the clauses propagator each time
        // we push a new literal because some propagator like the arc consistent
        // all diff relies on this.
        //
        // However, on some problem, it seems to work better to not do that. One
        // possible reason is that the reason of a "natural" propagation might
        // be better than one we learned.
        const bool run_sat_propagators_at_higher_priority = true;
        if (run_sat_propagators_at_higher_priority) {
          // We exit in order to rerun all SAT only propagators first. Note that
          // since a literal was pushed we are guaranteed to be called again,
          // and we will resume from priority 0.
          return true;
        } else {
          priority = -1;
        }
      }
    }
  }
  return true;
}

void GenericLiteralWatcher::Untrail(const Trail& trail, int trail_index) {
  if (propagation_trail_index_ <= trail_index) {
    // Nothing to do since we found a conflict before Propagate() was called.
    CHECK_EQ(propagation_trail_index_, trail_index);
    return;
  }

  // We need to clear the watch indices on untrail.
  for (std::deque<int>& queue : queue_by_priority_) {
    for (const int id : queue) {
      id_to_watch_indices_[id].clear();
    }
    queue.clear();
  }

  // This means that we already propagated all there is to propagate
  // at the level trail_index, so we can safely clear modified_vars_ in case
  // it wasn't already done.
  propagation_trail_index_ = trail_index;
  modified_vars_.ClearAndResize(integer_trail_->NumIntegerVariables());
  in_queue_.assign(watchers_.size(), false);

  const int level = trail.CurrentDecisionLevel();
  for (int& ref : id_to_greatest_common_level_since_last_call_) {
    ref = std::min(ref, level);
  }
}

// Registers a propagator and returns its unique ids.
int GenericLiteralWatcher::Register(PropagatorInterface* propagator) {
  const int id = watchers_.size();
  watchers_.push_back(propagator);
  id_to_level_at_last_call_.push_back(0);
  id_to_greatest_common_level_since_last_call_.push_back(0);
  id_to_reversible_classes_.push_back(std::vector<ReversibleInterface*>());
  id_to_reversible_ints_.push_back(std::vector<int*>());
  id_to_watch_indices_.push_back(std::vector<int>());
  id_to_priority_.push_back(1);
  id_to_idempotence_.push_back(true);

  // Call this propagator at least once the next time Propagate() is called.
  //
  // TODO(user): This initial propagation does not respect any later priority
  // settings. Fix this. Maybe we should force users to pass the priority at
  // registration. For now I didn't want to change the interface because there
  // are plans to implement a kind of "dynamic" priority, and if it works we may
  // want to get rid of this altogether.
  in_queue_.push_back(true);
  queue_by_priority_[1].push_back(id);
  return id;
}

void GenericLiteralWatcher::SetPropagatorPriority(int id, int priority) {
  id_to_priority_[id] = priority;
  if (priority >= queue_by_priority_.size()) {
    queue_by_priority_.resize(priority + 1);
  }
}

void GenericLiteralWatcher::NotifyThatPropagatorMayNotReachFixedPointInOnePass(
    int id) {
  id_to_idempotence_[id] = false;
}

void GenericLiteralWatcher::RegisterReversibleClass(int id,
                                                    ReversibleInterface* rev) {
  id_to_reversible_classes_[id].push_back(rev);
}

void GenericLiteralWatcher::RegisterReversibleInt(int id, int* rev) {
  id_to_reversible_ints_[id].push_back(rev);
}

// This is really close to ExcludeCurrentSolutionAndBacktrack().
std::function<void(Model*)>
ExcludeCurrentSolutionWithoutIgnoredVariableAndBacktrack() {
  return [=](Model* model) {
    SatSolver* sat_solver = model->GetOrCreate<SatSolver>();
    IntegerTrail* integer_trail = model->GetOrCreate<IntegerTrail>();
    IntegerEncoder* encoder = model->GetOrCreate<IntegerEncoder>();

    const int current_level = sat_solver->CurrentDecisionLevel();
    std::vector<Literal> clause_to_exclude_solution;
    clause_to_exclude_solution.reserve(current_level);
    for (int i = 0; i < current_level; ++i) {
      bool include_decision = true;
      const Literal decision = sat_solver->Decisions()[i].literal;

      // Tests if this decision is associated to a bound of an ignored variable
      // in the current assignment.
      const InlinedIntegerLiteralVector& associated_literals =
          encoder->GetIntegerLiterals(decision);
      for (const IntegerLiteral bound : associated_literals) {
        if (integer_trail->IsCurrentlyIgnored(bound.var)) {
          // In this case we replace the decision (which is a bound on an
          // ignored variable) with the fact that the integer variable was
          // ignored. This works because the only impact a bound of an ignored
          // variable can have on the rest of the model is through the
          // is_ignored literal.
          clause_to_exclude_solution.push_back(
              integer_trail->IsIgnoredLiteral(bound.var).Negated());
          include_decision = false;
        }
      }

      if (include_decision) {
        clause_to_exclude_solution.push_back(decision.Negated());
      }
    }

    // Note that it is okay to add duplicates literals in ClauseConstraint(),
    // the clause will be preprocessed correctly.
    sat_solver->Backtrack(0);
    model->Add(ClauseConstraint(clause_to_exclude_solution));
  };
}

}  // namespace sat
}  // namespace operations_research
