// HookScopeTest — which secrets a hook may read, and how its header edits are applied.
#include "application/HookScope.h"

#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"

#include <reqloom/engine/Actor.h>
#include <reqloom/engine/RunContext.h>

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ce = reqloom::engine;

namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

}  // namespace

TEST(HookScope, secret_names_come_from_operation_actor_env_and_script) {
    ce::Project project;
    ce::Actor actor;
    actor.id = ce::ActorId{"alice"};
    actor.inject.headers["Authorization"] = "Bearer {{secret.ACTOR_TOKEN}}";
    project.actors[actor.id] = actor;
    ce::Operation op;
    op.actor = actor.id;
    op.pathTemplate = "/items/{{ secret.PATH_KEY }}";
    op.headers["X-Env"] = "{{env.indirect}}";
    const std::map<std::string, std::string> env{{"indirect", "{{secret.VIA_ENV}}"},
                                                 {"unused", "{{secret.NOT_MINE}}"}};

    const auto names = ce::hookSecretNames(
        op, project, env, "const a = ctx.secret.FROM_DOT; const b = ctx.secret['FROM_INDEX'];");

    EXPECT_EQ(
        names,
        (std::set<std::string>{"ACTOR_TOKEN", "FROM_DOT", "FROM_INDEX", "PATH_KEY", "VIA_ENV"}));
}

TEST(HookScope, read_only_hook_keeps_duplicate_headers) {
    const Headers original{{"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}, {"X-Id", "7"}};
    const std::map<std::string, std::string> exposed{{"Set-Cookie", "b=2"}, {"X-Id", "7"}};

    EXPECT_EQ(ce::mergeHookHeaders(original, exposed, exposed), original);
}

TEST(HookScope, edited_header_replaces_only_that_name) {
    const Headers original{{"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}, {"X-Id", "7"}};
    const std::map<std::string, std::string> exposed{{"Set-Cookie", "b=2"}, {"X-Id", "7"}};
    const std::map<std::string, std::string> mutated{{"Set-Cookie", "b=2"}, {"X-New", "n"}};

    const auto merged = ce::mergeHookHeaders(original, exposed, mutated);

    EXPECT_EQ(merged, (Headers{{"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}, {"X-New", "n"}}));
}

TEST(HookScope, changed_duplicate_header_becomes_one_entry) {
    const Headers original{{"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}};
    const std::map<std::string, std::string> exposed{{"Set-Cookie", "b=2"}};
    const std::map<std::string, std::string> mutated{{"Set-Cookie", "c=3"}};

    EXPECT_EQ(ce::mergeHookHeaders(original, exposed, mutated), (Headers{{"Set-Cookie", "c=3"}}));
}

TEST(RunContextInstances, replace_drops_only_the_producers_earlier_instances) {
    ce::RunContext ctx;
    const ce::ResourceId order{"order"};
    ce::ResourceInstance seeded;
    seeded.variables["id"] = "pinned";
    ctx.appendInstance(order, seeded);
    std::vector<ce::ResourceInstance> first(2);
    ctx.replaceInstances(order, ce::OperationId{"order.list"}, first);
    std::vector<ce::ResourceInstance> other(1);
    ctx.replaceInstances(order, ce::OperationId{"order.pay"}, other);

    std::vector<ce::ResourceInstance> again(1);
    again.front().variables["id"] = "new";
    ctx.replaceInstances(order, ce::OperationId{"order.list"}, again);

    const auto& all = ctx.instances(order);
    ASSERT_EQ(all.size(), 3U);
    EXPECT_EQ(all[0].variables.at("id"), "pinned");
    EXPECT_EQ(all[1].producer.value, "order.pay");
    EXPECT_EQ(all[2].producer.value, "order.list");
    EXPECT_EQ(all[2].variables.at("id"), "new");
}
