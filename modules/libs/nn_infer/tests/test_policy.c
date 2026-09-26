/* Runs the SHARED fixture (nn-media-stream/tests/policy_fixture.json) against
 * the C evaluator.  The cases are transcribed here rather than parsed so the
 * test has no JSON dependency on an MCU; keep them in sync with the file. */
#include <nn_infer/policy.h>
#include <stdio.h>

struct step { uint16_t feed, agg; bool det; };
struct tcase { const char *name; uint8_t agg; uint16_t start, stop;
               struct step s[8]; int n; };

static struct tcase cases[] = {
  {"ramp-up + hysteresis", 5, 600, 400, {
     {1000,200,false},{1000,400,false},{1000,600,true},{1000,800,true},
     {0,800,true},{0,600,true},{0,400,true},{0,200,false}}, 8},
  {"single spike ignored", 5, 600, 400, {
     {1000,200,false},{0,200,false},{0,200,false},{0,200,false},{0,200,false}}, 5},
  {"agg=1 instantaneous", 1, 500, 500, {
     {900,900,true},{100,100,false},{900,900,true}}, 3},
  {"stop clamped to start", 2, 500, 900, {
     {1000,500,true},{1000,1000,true},{0,500,true}}, 3},
};

int main(void)
{
    int bad = 0;
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        nn_infer_class_state_t st;
        nn_infer_class_policy_t p = { .capture = true, .agg = cases[c].agg,
            .start_x1000 = cases[c].start, .stop_x1000 = cases[c].stop };
        nn_infer_policy_set(&st, &p);
        for (int i = 0; i < cases[c].n; i++) {
            bool ch;
            uint16_t agg = nn_infer_policy_feed(&st, cases[c].s[i].feed, &ch);
            if (agg != cases[c].s[i].agg ||
                nn_infer_policy_detected(&st) != cases[c].s[i].det) {
                printf("FAIL %s step %d: agg=%u exp=%u det=%d exp=%d\n",
                       cases[c].name, i, agg, cases[c].s[i].agg,
                       nn_infer_policy_detected(&st), cases[c].s[i].det);
                bad++;
            }
        }
    }
    printf("c fixture: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
