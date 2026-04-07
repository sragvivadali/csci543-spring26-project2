#include "duckdb/execution/jit/jit_profiler.hpp"

#include "duckdb/planner/expression/list.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace duckdb {

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local vector<idx_t> tls_query_scope;

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

static void SortFingerprintCounts(vector<JITProfilerFingerprintCount> &v) {
	std::sort(v.begin(), v.end(),
	          [](const JITProfilerFingerprintCount &a, const JITProfilerFingerprintCount &b) {
		          return a.fingerprint < b.fingerprint;
	          });
}

} // namespace

uint64_t JITProfiler::Fingerprint(const Expression &expr) {
	// Start from FNV offset basis, then fold the full tree shape into `hash`.
	uint64_t hash = FNV_OFFSET_BASIS;
	HashExpressionTree(hash, expr);
	return hash;
}

bool JITProfiler::ProfilesAsArithmeticRoot(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	const auto &name = expr.Cast<BoundFunctionExpression>().function.name;
	return name == "+" || name == "-" || name == "*" || name == "/" || name == "//" || name == "%";
}

JITProfiler &JITProfiler::GetInstance() {
	static JITProfiler instance;
	return instance;
}

void JITProfiler::PushQueryScope(idx_t query_id) {
	tls_query_scope.push_back(query_id);
}

void JITProfiler::PopQueryScope() {
	if (!tls_query_scope.empty()) {
		tls_query_scope.pop_back();
	}
}

void JITProfiler::Record(const Expression &expr, idx_t tuple_count) {
	if (!ProfilesAsArithmeticRoot(expr)) {
		return;
	}

	record_invocations.fetch_add(1, std::memory_order_relaxed);
	tuples_accounted.fetch_add(tuple_count, std::memory_order_relaxed);

	const uint64_t fp = Fingerprint(expr);
	const idx_t *active_query = nullptr;
	if (!tls_query_scope.empty()) {
		active_query = &tls_query_scope.back();
	}

	std::function<void(uint64_t)> notify_cb;
	bool do_notify = false;

	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	{
		std::lock_guard<std::mutex> guard(lock);
		uint64_t &slot = counts[fp];
		const uint64_t new_count = slot + tuple_count;
		slot = new_count;
		if (active_query) {
			per_query_counts[*active_query][fp] += tuple_count;
		}
		if (hotness_threshold.has_value() && new_count >= *hotness_threshold) {
			if (hot_labeled_fingerprints.insert(fp).second) {
				do_notify = true;
				notify_cb = on_fingerprint_became_hot;
			}
		}
	}
	const auto t1 = clock::now();
	const auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
	locked_section_nanoseconds.fetch_add(static_cast<uint64_t>(delta), std::memory_order_relaxed);

	if (do_notify && notify_cb) {
		notify_cb(fp);
	}
}

uint64_t JITProfiler::GetCount(const Expression &expr) const {
	const uint64_t fp = Fingerprint(expr);
	std::lock_guard<std::mutex> guard(lock);
	auto it = counts.find(fp);
	return it == counts.end() ? 0 : it->second;
}

uint64_t JITProfiler::GetCountForQuery(idx_t query_id, const Expression &expr) const {
	const uint64_t fp = Fingerprint(expr);
	std::lock_guard<std::mutex> guard(lock);
	auto qit = per_query_counts.find(query_id);
	if (qit == per_query_counts.end()) {
		return 0;
	}
	auto it = qit->second.find(fp);
	return it == qit->second.end() ? 0 : it->second;
}

vector<JITProfilerFingerprintCount> JITProfiler::GetGlobalExpressionStats() const {
	std::lock_guard<std::mutex> guard(lock);
	vector<JITProfilerFingerprintCount> out;
	out.reserve(counts.size());
	for (const auto &entry : counts) {
		out.push_back({entry.first, entry.second});
	}
	SortFingerprintCounts(out);
	return out;
}

vector<JITProfilerQueryFingerprintStats> JITProfiler::GetPerQueryExpressionStats() const {
	std::lock_guard<std::mutex> guard(lock);
	vector<idx_t> query_ids;
	query_ids.reserve(per_query_counts.size());
	for (const auto &qe : per_query_counts) {
		query_ids.push_back(qe.first);
	}
	std::sort(query_ids.begin(), query_ids.end());

	vector<JITProfilerQueryFingerprintStats> out;
	out.reserve(query_ids.size());
	for (const idx_t qid : query_ids) {
		JITProfilerQueryFingerprintStats qs;
		qs.query_id = qid;
		const auto &inner = per_query_counts.find(qid)->second;
		qs.per_expression.reserve(inner.size());
		for (const auto &entry : inner) {
			qs.per_expression.push_back({entry.first, entry.second});
		}
		SortFingerprintCounts(qs.per_expression);
		out.push_back(std::move(qs));
	}
	return out;
}

vector<JITProfilerFingerprintCount> JITProfiler::GetTopHotExpressions(idx_t limit) const {
	std::lock_guard<std::mutex> guard(lock);
	vector<JITProfilerFingerprintCount> out;
	out.reserve(counts.size());
	for (const auto &entry : counts) {
		out.push_back({entry.first, entry.second});
	}
	std::sort(out.begin(), out.end(), [](const JITProfilerFingerprintCount &a,
	                                      const JITProfilerFingerprintCount &b) {
		if (a.tuple_count != b.tuple_count) {
			return a.tuple_count > b.tuple_count;
		}
		return a.fingerprint < b.fingerprint;
	});
	if (out.size() > limit) {
		out.resize(limit);
	}
	return out;
}

void JITProfiler::RebuildHotLabelsLocked() {
	hot_labeled_fingerprints.clear();
	if (!hotness_threshold.has_value()) {
		return;
	}
	const uint64_t threshold_value = *hotness_threshold;
	for (const auto &entry : counts) {
		if (entry.second >= threshold_value) {
			hot_labeled_fingerprints.insert(entry.first);
		}
	}
}

void JITProfiler::SetHotnessThreshold(uint64_t minimum_tuple_count) {
	std::lock_guard<std::mutex> guard(lock);
	hotness_threshold = minimum_tuple_count;
	RebuildHotLabelsLocked();
}

void JITProfiler::ClearHotnessThreshold() {
	std::lock_guard<std::mutex> guard(lock);
	hotness_threshold.reset();
	hot_labeled_fingerprints.clear();
}

bool JITProfiler::HasHotnessThreshold() const {
	std::lock_guard<std::mutex> guard(lock);
	return hotness_threshold.has_value();
}

std::optional<uint64_t> JITProfiler::GetHotnessThreshold() const {
	std::lock_guard<std::mutex> guard(lock);
	return hotness_threshold;
}

bool JITProfiler::PassesHotnessThreshold(uint64_t tuple_count) const {
	std::lock_guard<std::mutex> guard(lock);
	if (!hotness_threshold.has_value()) {
		return tuple_count > 0;
	}
	return tuple_count >= *hotness_threshold;
}

bool JITProfiler::IsFingerprintHot(uint64_t fingerprint) const {
	std::lock_guard<std::mutex> guard(lock);
	if (hotness_threshold.has_value()) {
		return hot_labeled_fingerprints.count(fingerprint) != 0;
	}
	auto it = counts.find(fingerprint);
	return it != counts.end() && it->second > 0;
}

bool JITProfiler::IsExpressionHot(const Expression &expr) const {
	return IsFingerprintHot(Fingerprint(expr));
}

vector<uint64_t> JITProfiler::GetHotLabeledFingerprints() const {
	std::lock_guard<std::mutex> guard(lock);
	if (!hotness_threshold.has_value()) {
		return {};
	}
	vector<uint64_t> out;
	out.reserve(hot_labeled_fingerprints.size());
	for (const uint64_t fp : hot_labeled_fingerprints) {
		out.push_back(fp);
	}
	std::sort(out.begin(), out.end());
	return out;
}

void JITProfiler::SetFingerprintBecameHotCallback(std::function<void(uint64_t fingerprint)> callback) {
	std::lock_guard<std::mutex> guard(lock);
	on_fingerprint_became_hot = std::move(callback);
}

void JITProfiler::ClearFingerprintBecameHotCallback() {
	std::lock_guard<std::mutex> guard(lock);
	on_fingerprint_became_hot = nullptr;
}

JITProfilerOverheadStats JITProfiler::GetOverheadStats() const {
	JITProfilerOverheadStats s;
	s.record_calls = record_invocations.load(std::memory_order_relaxed);
	s.tuples_accounted = tuples_accounted.load(std::memory_order_relaxed);
	s.locked_section_nanoseconds = locked_section_nanoseconds.load(std::memory_order_relaxed);
	return s;
}

JITProfilerExportData JITProfiler::ExportSnapshot() const {
	JITProfilerExportData out;
	{
		std::lock_guard<std::mutex> guard(lock);
		out.global_by_fingerprint.reserve(counts.size());
		for (const auto &entry : counts) {
			out.global_by_fingerprint.push_back({entry.first, entry.second});
		}
		SortFingerprintCounts(out.global_by_fingerprint);

		vector<idx_t> query_ids;
		query_ids.reserve(per_query_counts.size());
		for (const auto &qe : per_query_counts) {
			query_ids.push_back(qe.first);
		}
		std::sort(query_ids.begin(), query_ids.end());
		out.by_query.reserve(query_ids.size());
		for (const idx_t qid : query_ids) {
			JITProfilerQueryFingerprintStats qs;
			qs.query_id = qid;
			const auto &inner = per_query_counts.find(qid)->second;
			qs.per_expression.reserve(inner.size());
			for (const auto &entry : inner) {
				qs.per_expression.push_back({entry.first, entry.second});
			}
			SortFingerprintCounts(qs.per_expression);
			out.by_query.push_back(std::move(qs));
		}

		out.hotness_threshold_configured = hotness_threshold.has_value();
		out.hotness_threshold_value = hotness_threshold.value_or(0);
		out.hot_labeled_fingerprints.reserve(hot_labeled_fingerprints.size());
		for (const uint64_t fp : hot_labeled_fingerprints) {
			out.hot_labeled_fingerprints.push_back(fp);
		}
		std::sort(out.hot_labeled_fingerprints.begin(), out.hot_labeled_fingerprints.end());
	}
	out.overhead = GetOverheadStats();
	return out;
}

void JITProfiler::PrintStats() const {
	const auto snap = ExportSnapshot();
	const auto oh = snap.overhead;

	std::cout << "JITProfiler summary\n";
	std::cout << "  record_calls: " << oh.record_calls << "\n";
	std::cout << "  tuples_accounted: " << oh.tuples_accounted << "\n";
	std::cout << "  locked_section_ns (cumulative): " << oh.locked_section_nanoseconds << "\n";
	if (snap.hotness_threshold_configured) {
		std::cout << "  hotness_threshold: " << snap.hotness_threshold_value << " (configured)\n";
	} else {
		std::cout << "  hotness_threshold: (not configured; PassesHotnessThreshold uses count > 0)\n";
	}
	std::cout << "Global expression totals (fingerprint -> tuples):\n";
	for (const auto &e : snap.global_by_fingerprint) {
		std::cout << "  fp " << e.fingerprint << " -> " << e.tuple_count << "\n";
	}
	if (!snap.by_query.empty()) {
		std::cout << "Per-query totals:\n";
		for (const auto &q : snap.by_query) {
			std::cout << "  query_id " << q.query_id << ":\n";
			for (const auto &e : q.per_expression) {
				std::cout << "    fp " << e.fingerprint << " -> " << e.tuple_count << "\n";
			}
		}
	}
	if (!snap.hot_labeled_fingerprints.empty()) {
		std::cout << "Hot-labeled fingerprints (threshold mode):\n";
		for (const uint64_t fp : snap.hot_labeled_fingerprints) {
			std::cout << "  fp " << fp << "\n";
		}
	}
}

void JITProfiler::Reset() {
	std::lock_guard<std::mutex> guard(lock);
	counts.clear();
	per_query_counts.clear();
	hotness_threshold.reset();
	hot_labeled_fingerprints.clear();
	on_fingerprint_became_hot = nullptr;
	record_invocations.store(0, std::memory_order_relaxed);
	tuples_accounted.store(0, std::memory_order_relaxed);
	locked_section_nanoseconds.store(0, std::memory_order_relaxed);
}

} // namespace duckdb
