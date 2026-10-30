// EDSL / rules-as-data / Intent->Plan / executor semantics.
#include "testkit.hpp"

using namespace amr;

TEST(sexpr_roundtrip_and_structural_equality) {
    const char* src = R"((Assemble :budget "64" (Rank :by "recency" :k "5" (Retrieve :source "notes"))))";
    NodeId a = parse_sexpr(src);
    NodeId b = parse_sexpr(to_sexpr(a));
    CHECK(a != b);
    CHECK(deep_equal(a, b));
    CHECK(deep_hash(a) == deep_hash(b));
    CHECK(count_nodes(a) == 3);
    CHECK_THROWS(parse_sexpr("(Unclosed"), std::runtime_error);
}

TEST(intent_to_plan_uses_all_edsl_nodes) {
    Intent in;
    in.entity = "e1";
    in.sources = {"notes", "chat"};
    in.since = 1100;
    in.tag = "pref";
    in.relate_source = "tools";
    NodeId p = intent_to_plan(in);
    for (const char* k : {"CostHint", "Assemble", "Rank", "Join", "Filter", "Parallel", "Retrieve"})
        CHECK(find_kind(p, k) != kNoNode);
    std::printf("    %s\n", to_sexpr(p).c_str());
}

TEST(pattern_match_binds_and_template_reuses_nodes) {
    NodeId plan = parse_sexpr(R"((Filter :field "entity" :op "=" :value "e1" (Retrieve :source "notes")))");
    RuleView r = view_rule(tk::pushdown_rule());
    Bindings b;
    CHECK(match(r.pattern, plan, b));
    CHECK(b.strs["?v"] == "e1");
    NodeId out = instantiate(r.rewrite, b);
    CHECK(node(out).kind == "Retrieve");
    CHECK(*node(out).attr("pfield") == "entity");
    CHECK(*node(out).attr("source") == "notes");
    // "~" = attr must be absent: the rewritten node no longer matches
    Bindings b2;
    NodeId again = parse_sexpr(R"((Filter :field "ts" :op ">=" :value "1" (Retrieve :source "notes" :pfield "entity")))");
    CHECK(!match(r.pattern, again, b2));
}

TEST(rules_are_queryable_data) {
    RuleSet rs;
    rs = rs.with_upsert(tk::safety_rule()).with_upsert(tk::rank_default_rule()).with_upsert(tk::pushdown_rule());
    auto never = rs.query(parse_sexpr(R"((Rule :tier "never-auto"))"));
    CHECK(never.size() == 1);
    CHECK(view_rule(never[0]).name == "safety-redact-secrets");
    auto all = rs.query(parse_sexpr(R"((Rule :name "_"))"));
    CHECK(all.size() == 3);
    CHECK(rs.with_remove("rank-default-k").size() == 2);
    CHECK(rs.size() == 3);
    for (auto& line : rs.to_text())
        CHECK(!line.empty());
}

TEST(optimizer_reaches_fixpoint_and_safety_rule_applies) {
    Intent in;
    in.entity = "e3";
    in.sources = {"notes", "chat"};
    in.since = 1300;
    RuleSet rs;
    rs = rs.with_upsert(tk::safety_rule()).with_upsert(tk::pushdown_rule()).with_upsert(tk::filter_through_parallel_rule());
    OptStats st;
    NodeId out = optimize(intent_to_plan(in), rs, st);
    CHECK(st.passes < 16);
    CHECK(st.hits["safety-redact-secrets"] == 1);
    CHECK(st.hits["filter-through-parallel"] >= 1);
    CHECK(st.hits["pushdown-filter-into-retrieve"] >= 2);
    OptStats st2;
    NodeId out2 = optimize(out, rs, st2);
    CHECK(deep_equal(out, out2)); // idempotent at fixpoint
    std::printf("    %s\n", to_sexpr(out).c_str());
}

TEST(executor_semantics) {
    auto dir = tk::tmpdir("exec");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    rt->put_memory({0, "alice", "notes", "pref", 10, "likes tea"});
    rt->put_memory({0, "alice", "notes", "secret", 11, "password hunter2"});
    rt->put_memory({0, "alice", "chat", "fact", 12, "lives in paris"});
    rt->put_memory({0, "bob", "notes", "fact", 13, "likes coffee"});
    rt->put_memory({0, "alice", "tools", "fact", 14, "calendar synced"});
    Intent in;
    in.entity = "alice";
    in.sources = {"notes", "chat"};
    auto r = rt->query(in);
    CHECK(r.ids.size() == 2); // the secret is redacted by the never-auto safety rule
    CHECK(r.ids[0] == 3 && r.ids[1] == 1); // recency order
    in.relate_source = "tools"; // Relate on entity: alice's tools row joins the context
    auto rj = rt->query(in);
    CHECK(rj.ids.size() == 3);
    CHECK(!rj.ids.empty() && rj.ids[0] == 5);
    in.entity = "bob";
    in.sources = {"notes"};
    CHECK(rt->query(in).ids.empty()); // bob has no tools row
    in.relate_source.clear();
    in.budget_tokens = 1; // token budget truncates assembly
    CHECK(rt->query(in).ids.empty());
}

int main() { return tk::run_all("rules_engine"); }
