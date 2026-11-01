// Host-side test of the quest outline's matching rules (Engine/Source/WXLOutline/QuestMatch.h). No game data.
// Build and run it with run_questmatch_tests.sh.
#include <cstdio>

#include "Engine/Source/WXLOutline/QuestMatch.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main()
{
    using wxl::outline::QuestNeeds;
    using wxl::outline::ParseHexColor;

    QuestNeeds none;
    CHECK(none.Empty(), "nothing added: empty");
    CHECK(!none.NeedsCreature(1234, nullptr, 0), "empty needs match no creature");
    CHECK(!none.NeedsGameObject(77), "empty needs match no object");

    QuestNeeds n;
    n.AddObjective(1234);   // kill creature 1234
    n.AddObjective(-5678);  // use game object 5678
    n.AddObjective(0);      // no objective: ignored
    n.AddItem(900);
    n.AddItem(0);           // no item: ignored
    CHECK(!n.Empty(), "objectives and items added");

    CHECK(n.NeedsCreature(1234, nullptr, 0), "creature matched by its own entry");
    CHECK(!n.NeedsCreature(1235, nullptr, 0), "other creature does not match");
    CHECK(!n.NeedsCreature(5678, nullptr, 0), "a game object objective is not a creature");
    CHECK(n.NeedsGameObject(5678), "game object matched by its negated entry");
    CHECK(!n.NeedsGameObject(1234), "a creature objective is not a game object");
    CHECK(!n.NeedsGameObject(0), "entry 0 never matches");

    const uint32_t credit[2] = { 0, 1234 };
    CHECK(n.NeedsCreature(4444, credit, 2), "creature matched through a kill-credit entry");
    const uint32_t noCredit[2] = { 0, 0 };
    CHECK(!n.NeedsCreature(4444, noCredit, 2), "zero kill credits match nothing");

    const uint32_t drops[6] = { 0, 0, 900, 0, 0, 0 };
    const uint32_t other[6] = { 1, 2, 3, 0, 0, 0 };
    CHECK(n.NeedsItemFrom(drops, 6), "creature or object that can hold a missing quest item");
    CHECK(!n.NeedsItemFrom(other, 6), "holds only unrelated items");
    CHECK(!n.NeedsItemFrom(nullptr, 6), "no item list");
    CHECK(!n.NeedsItemFrom(drops, 2), "only the first two slots looked at");

    float c[3] = {};
    CHECK(ParseHexColor("00C8FF", c) && c[0] == 0.0f && c[2] == 1.0f && c[1] > 0.78f && c[1] < 0.79f, "plain hex");
    CHECK(ParseHexColor("#ff0000", c) && c[0] == 1.0f && c[1] == 0.0f, "# prefix, lower case");
    CHECK(ParseHexColor("0xFFD100", c) && c[0] == 1.0f && c[2] == 0.0f, "0x prefix");
    CHECK(!ParseHexColor("", c), "empty is not a color");
    CHECK(!ParseHexColor("FFF", c), "three digits is not accepted");
    CHECK(!ParseHexColor("GG0000", c), "non hex digit");
    CHECK(!ParseHexColor("00C8FF00", c), "eight digits is not accepted");
    CHECK(!ParseHexColor(nullptr, c), "null");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
