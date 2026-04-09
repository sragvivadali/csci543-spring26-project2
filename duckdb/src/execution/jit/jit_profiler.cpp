#include "duckdb/execution/jit/jit_profiler.hpp"

#include "duckdb/planner/expression/list.hpp"

#include <iostream>

namespace duckdb {

namespace {

// FNV-1a (64-bit): mixes per-node type info and child hashes; see header for semantics.

static constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
static constexpr uint64_t FNV_PRIME = 1099511628211ULL;

static void Fnv1aUpdate(uint64_t &hash, const void *data, size_t len) {
	auto bytes = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < len; i++) {
		hash ^= bytes[i];
		hash *= FNV_PRIME;
	}
}

template <class T>
static void HashPod(uint64_t &hash, const T &value) {
	Fnv1aUpdate(hash, &value, sizeof(T));
}

static void HashExpressionTree(uint64_t &hash, const Expression &expr);

static void HashString(uint64_t &hash, const string &s) {
	HashPod(hash, s.size());
	Fnv1aUpdate(hash, s.data(), s.size());
}

static void HashChildren(uint64_t &hash, const vector<unique_ptr<Expression>> &children) {
	HashPod(hash, children.size());
	for (const auto &child : children) {
		HashExpressionTree(hash, *child);
	}
}

static void HashExpressionTree(uint64_t &hash, const Expression &expr) {
	// Node header: expression kind + output type. This ensures e.g. comparison
	// vs arithmetic, or INT vs DOUBLE return types, do not share fingerprints.
	HashPod(hash, static_cast<uint32_t>(expr.GetExpressionType()));
	HashPod(hash, static_cast<uint8_t>(expr.GetExpressionClass()));
	HashPod(hash, static_cast<uint8_t>(expr.return_type.id()));
	HashPod(hash, static_cast<uint8_t>(expr.return_type.InternalType()));

	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF: {
		auto &r = expr.Cast<BoundReferenceExpression>();
		HashPod(hash, r.index);
		break;
	}
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &c = expr.Cast<BoundColumnRefExpression>();
		HashPod(hash, c.binding.table_index.index);
		HashPod(hash, c.binding.column_index.GetIndexUnsafe());
		HashPod(hash, c.depth);
		break;
	}
	case ExpressionClass::BOUND_CONSTANT:
		// Deliberately no Value payload: constants collide by type only.
		break;
	case ExpressionClass::BOUND_PARAMETER:
		break;
	case ExpressionClass::BOUND_COMPARISON: {
		auto &c = expr.Cast<BoundComparisonExpression>();
		HashExpressionTree(hash, *c.left);
		HashExpressionTree(hash, *c.right);
		break;
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &c = expr.Cast<BoundConjunctionExpression>();
		HashChildren(hash, c.children);
		break;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &o = expr.Cast<BoundOperatorExpression>();
		HashChildren(hash, o.children);
		break;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &f = expr.Cast<BoundFunctionExpression>();
		HashString(hash, f.function.name);
		HashChildren(hash, f.children);
		break;
	}
	case ExpressionClass::BOUND_CAST: {
		auto &c = expr.Cast<BoundCastExpression>();
		HashExpressionTree(hash, *c.child);
		HashPod(hash, c.try_cast);
		break;
	}
	case ExpressionClass::BOUND_CASE: {
		auto &c = expr.Cast<BoundCaseExpression>();
		HashPod(hash, c.case_checks.size());
		for (const auto &check : c.case_checks) {
			HashExpressionTree(hash, *check.when_expr);
			HashExpressionTree(hash, *check.then_expr);
		}
		if (c.else_expr) {
			HashExpressionTree(hash, *c.else_expr);
		}
		break;
	}
	case ExpressionClass::BOUND_BETWEEN: {
		auto &b = expr.Cast<BoundBetweenExpression>();
		HashPod(hash, b.lower_inclusive);
		HashPod(hash, b.upper_inclusive);
		HashExpressionTree(hash, *b.input);
		HashExpressionTree(hash, *b.lower);
		HashExpressionTree(hash, *b.upper);
		break;
	}
	default:
		// Unknown/unhandled classes: hash only the node header (types above); no children.
		break;
	}
}

} // namespace

uint64_t JITProfiler::Fingerprint(const Expression &expr) {
	// Start from FNV offset basis, then fold the full tree shape into `hash`.
	uint64_t hash = FNV_OFFSET_BASIS;
	HashExpressionTree(hash, expr);
	return hash;
}

JITProfiler &JITProfiler::GetInstance() {
	static auto *instance = new JITProfiler();
	return *instance;
}

void JITProfiler::Record(const Expression &expr, idx_t tuple_count) {
	const uint64_t fp = Fingerprint(expr);
	std::lock_guard<std::mutex> guard(lock);
	// The cache is "fingerprint -> total tuples evaluated".
	counts[fp] += tuple_count;
}

uint64_t JITProfiler::GetCount(const Expression &expr) const {
	const uint64_t fp = Fingerprint(expr);
	std::lock_guard<std::mutex> guard(lock);
	auto it = counts.find(fp);
	return it == counts.end() ? 0 : it->second;
}

void JITProfiler::PrintStats() const {
	std::lock_guard<std::mutex> guard(lock);
	for (const auto &entry : counts) {
		std::cout << "Expression fingerprint " << entry.first << " evaluated " << entry.second << " times\n";
	}
}

void JITProfiler::Reset() {
	std::lock_guard<std::mutex> guard(lock);
	counts.clear();
}

} // namespace duckdb
