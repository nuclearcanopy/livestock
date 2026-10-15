#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <array>
#include <unistd.h>
using namespace std;

// Deterministic mean-field clan simulation: tracks *expected* member counts
// per tenure cohort instead of sampling individuals. See docs/model.md for
// the cohort schedule, hazard/score formulas, and citations (Bartholomew
// 1982 manpower-planning cohorts; Ducheneaut et al. 2007, Zhou & Mockus 2016
// for the tenure-hazard shape).

static const int COHORTS = 14;
static const int EDGE[COHORTS + 1] = {0, 1, 2, 3, 4, 5, 6, 7, 14, 21, 35, 56, 90, 150, 1 << 30};

// REP/WIDTH depend only on the (compile-time-fixed) cohort schedule, so
// they're computed once into flat tables instead of recomputed every call.
static double REP[COHORTS];
static int WIDTH[COHORTS];
static void init_cohort_tables() {
  for (int c = 0; c < COHORTS; c++) {
    REP[c] = (c == COHORTS - 1) ? 200.0 : (EDGE[c] + EDGE[c + 1]) * 0.5;
    WIDTH[c] = EDGE[c + 1] - EDGE[c];
  }
}

// hazard/weight only depend on the per-simulation knobs (inactive_odds,
// boost, decay, veteran_bonus, halflife) and the fixed per-cohort tenure —
// never on the day or the count threshold — so each is computed exactly
// once per parameter point into a 14-entry table, not re-evaluated (with an
// exp() call!) on every single simulated day. This is the dominant cost: a
// day loop with billions of iterations was calling exp() 28x per iteration
// for values that never changed across the whole run.
static void precompute_rates(double inactive_odds, double boost, double decay,
                              double veteran_bonus, double halflife,
                              double haz[COHORTS], double vw[COHORTS]) {
  double d = decay > 1e-6 ? decay : 1e-6;
  double h = halflife > 1e-6 ? halflife : 1e-6;
  for (int c = 0; c < COHORTS; c++) {
    haz[c] = inactive_odds * 0.01 * (1.0 + boost * exp(-REP[c] / d));
    vw[c] = 1.0 + veteran_bonus * (1.0 - exp(-REP[c] / h));
  }
}

struct SimState {
  double active[COHORTS];
  int timer[COHORTS];
  double inactive;
  double score;
};

static inline void init_state(SimState &s, int count, double leader) {
  for (int c = 0; c < COHORTS; c++) { s.active[c] = 0.0; s.timer[c] = WIDTH[c]; }
  double founders = count - leader;
  s.active[0] = founders > 0.0 ? founders : 0.0;
  s.inactive = 0.0;
  s.score = 0.0;
}

// has_leader reserves exactly 1 slot that's always active (never hazarded,
// never kicked, never counted as a joinable opening) — a clan leader who
// doesn't churn like a regular member. Everything else (hazard/kick/join)
// operates on the remaining n-1 slots; the leader is added back in only for
// totals/score at the end.
static inline void one_day(SimState &s, int count, int n, double max_odds, double inactive_penalty,
                            double has_leader, const double haz[COHORTS], const double vw[COHORTS]) {
  double leader = has_leader != 0.0 ? 1.0 : 0.0;
  int eff_n = n - (int)leader;
  int eff_count = count - (int)leader;
  if (eff_count < 1) eff_count = 1;

  double total_active = 0.0;
  for (int c = 0; c < COHORTS; c++) total_active += s.active[c];

  // hazard: expected inactivations per cohort, proportionally clamped so at
  // least one active member always remains (same invariant as before).
  double room = total_active - 1.0;
  if (room < 0.0) room = 0.0;
  double proposed[COHORTS];
  double total_proposed = 0.0;
  for (int c = 0; c < COHORTS; c++) {
    proposed[c] = s.active[c] * haz[c];
    total_proposed += proposed[c];
  }
  double scale = (total_proposed > room && total_proposed > 0.0) ? room / total_proposed : 1.0;
  for (int c = 0; c < COHORTS; c++) {
    double amt = proposed[c] * scale;
    s.active[c] -= amt;
    s.inactive += amt;
  }

  total_active = 0.0;
  for (int c = 0; c < COHORTS; c++) total_active += s.active[c];
  double total_members = total_active + s.inactive;

  // kick: closed form of "purge inactives one at a time while roster >= count"
  if (total_members >= eff_count) {
    double cap = total_members - eff_count + 1.0;
    double kicked = s.inactive < cap ? s.inactive : cap;
    if (kicked < 0.0) kicked = 0.0;
    s.inactive -= kicked;
    total_members -= kicked;
  }

  // age: shift each cohort's full contents into the next once its bucket
  // width elapses; cohort 0 (width 1) empties every day, ready for joiners.
  for (int c = 0; c < COHORTS - 1; c++) {
    s.timer[c]--;
    if (s.timer[c] <= 0) {
      s.active[c + 1] += s.active[c];
      s.active[c] = 0.0;
      s.timer[c] = WIDTH[c];
    }
  }

  // join: new members always enter cohort 0 at tenure 0. A roster that's
  // visibly full of inactive dead weight looks less appealing to recruits
  // than one of the same size that's mostly active — scale recruitment
  // down by how much of the roster is inactive, not just by how full it is.
  // Join room/chance are computed over the non-leader slots only — the
  // leader's reserved slot was never recruitable to begin with.
  double inactive_frac = total_members > 0.0 ? s.inactive / total_members : 0.0;
  double penalty = 1.0 - inactive_penalty * inactive_frac;
  if (penalty < 0.0) penalty = 0.0;
  double chance = max_odds / (double)(eff_n - 1) * total_members * penalty;
  double new_m = chance * 0.01;
  double room_join = (double)eff_n - total_members;
  if (new_m > room_join) new_m = room_join;
  if (new_m < 0.0) new_m = 0.0;
  s.active[0] += new_m;

  // score: veteran members contribute marginally more (design choice, not
  // literature-calibrated — see docs/model.md). The leader, if present, is
  // assumed to have been there forever — scored at the terminal (fully
  // saturated veteran) weight.
  double day_score = 0.0;
  for (int c = 0; c < COHORTS; c++) day_score += s.active[c] * vw[c];
  day_score += leader * vw[COHORTS - 1];
  s.score += day_score;
}

int generate_best_count(int n, double inactive_odds, double max_odds,
                         double boost, double decay, double veteran_bonus, double halflife,
                         double inactive_penalty, double has_leader, int days = 10000) {
  double haz[COHORTS], vw[COHORTS];
  precompute_rates(inactive_odds, boost, decay, veteran_bonus, halflife, haz, vw);
  double leader = has_leader != 0.0 ? 1.0 : 0.0;

  vector<SimState> states(n + 1);
  for (int count = 1; count <= n; count++) {
    init_state(states[count], count, leader);
    for (int day = 1; day <= days; day++) {
      one_day(states[count], count, n, max_odds, inactive_penalty, has_leader, haz, vw);
    }
  }

  double best_score = -1.0;
  int best_count = 0;
  for (int i = 1; i <= n; i++) {
    if (states[i].score >= best_score) { best_score = states[i].score; best_count = i; }
  }
  return best_count;
}

static inline double productivity(const SimState &s, const double vw[COHORTS]) {
  double p = 0.0;
  for (int c = 0; c < COHORTS; c++) p += s.active[c] * vw[c];
  return p;
}

// Advisor mode: answers "I'm a clan leader with this many total/inactive
// members right now, what should I actually do" by reusing the same
// one_day() mechanics, but seeded from the leader's real current state
// instead of a fresh count-many-founders start. Current active members are
// assumed to sit in the terminal (veteran) cohort, since an advisor asking
// about "right now" is describing an established clan, not day-0 joiners —
// a simplification forced by not knowing each real member's actual tenure.
static void run_advise(int n, double inactive_odds, double max_odds,
                        double boost, double decay, double veteran_bonus, double halflife,
                        double inactive_penalty, double has_leader, int current_total, int current_inactive,
                        int days) {
  double haz[COHORTS], vw[COHORTS];
  precompute_rates(inactive_odds, boost, decay, veteran_bonus, halflife, haz, vw);
  double leader = has_leader != 0.0 ? 1.0 : 0.0;

  int best_count = generate_best_count(n, inactive_odds, max_odds, boost, decay,
                                        veteran_bonus, halflife, inactive_penalty, has_leader, days);

  int current_active = current_total - current_inactive;
  if (current_active < 0) current_active = 0;

  double kick_now = 0.0;
  if (current_total >= best_count) {
    double cap = current_total - best_count + 1.0;
    kick_now = current_inactive < cap ? current_inactive : cap;
    if (kick_now < 0.0) kick_now = 0.0;
  }

  double current_productivity = current_active * vw[COHORTS - 1];

  // current_total/current_active are the user's real counts, leader
  // included — seed only the non-leader remainder into the cohort pool so
  // one_day's own `leader` top-up isn't double-counted.
  double non_leader_active = current_active - leader;
  if (non_leader_active < 0.0) non_leader_active = 0.0;

  SimState s{};
  for (int c = 0; c < COHORTS; c++) { s.active[c] = 0.0; s.timer[c] = WIDTH[c]; }
  s.active[COHORTS - 1] = non_leader_active;
  s.inactive = current_inactive;
  s.score = 0.0;
  for (int day = 1; day <= days; day++) one_day(s, best_count, n, max_odds, inactive_penalty, has_leader, haz, vw);

  double steady_active = leader;
  for (int c = 0; c < COHORTS; c++) steady_active += s.active[c];
  double steady_productivity = productivity(s, vw) + leader * vw[COHORTS - 1];

  cout << best_count << ' ' << kick_now << ' ' << steady_active << ' ' << s.inactive
       << ' ' << current_productivity << ' ' << steady_productivity << "\n";
}

static int best_of(const vector<SimState> &state, int n) {
  double best_score = -1.0;
  int best_count = 0;
  for (int c = 1; c <= n; c++) {
    if (state[c].score >= best_score) { best_score = state[c].score; best_count = c; }
  }
  return best_count;
}

// stream mode: parallel across sets (OpenMP), batched days between emits.
// Trick for MASSIVE max_days: after WARMUP real days, the system has
// converged to its deterministic fixed point and score/day per (set, count)
// is constant. We freeze the sim and extrapolate score linearly beyond
// WARMUP. The client still sees the day counter climb; best_of() still
// recomputes correctly against the extrapolated scores; and the compute
// cost is O(WARMUP) instead of O(max_days).
static int run_stream() {
  int max_days, emit_every, sleep_us, N;
  if (!(cin >> max_days >> emit_every >> sleep_us >> N)) return 1;
  vector<int> ns(N);
  vector<double> ios(N), mos(N), boosts(N), decays(N), vbs(N), vhs(N), pens(N), leaders(N);
  for (int k = 0; k < N; k++)
    cin >> ns[k] >> ios[k] >> mos[k] >> boosts[k] >> decays[k] >> vbs[k] >> vhs[k] >> pens[k] >> leaders[k];

  vector<vector<SimState>> state(N);
  for (int k = 0; k < N; k++) {
    state[k].resize(ns[k] + 1);
    for (int c = 1; c <= ns[k]; c++) init_state(state[k][c], c, leaders[k] != 0.0 ? 1.0 : 0.0);
  }

  // precompute per-set hazard/weight tables once, outside every day/count loop
  vector<array<double, COHORTS>> haz(N), vw(N);
  for (int k = 0; k < N; k++) {
    precompute_rates(ios[k], boosts[k], decays[k], vbs[k], vhs[k], haz[k].data(), vw[k].data());
  }

  ios::sync_with_stdio(false);

  // The old fixed WARMUP=3000 was sized for the slowest possible half-life
  // the UI allows (365 days), applied uniformly to every parameter point —
  // but convergence time is a direct function of a set's own decay/halflife
  // (a curve with a 10-day half-life is settled by ~60-180 days). Derive a
  // per-set warmup from its own constants (6 half-lives ~ 0.25% residual)
  // instead of paying the worst case on every point. The 300-day floor is a
  // safety margin for the roster-level join/kick equilibration, which has
  // no closed-form decay constant in this model.
  vector<int> warmup_k(N);
  for (int k = 0; k < N; k++) {
    double slowest = decays[k] > vhs[k] ? decays[k] : vhs[k];
    int w = (int)ceil(6.0 * slowest);
    if (w < 300) w = 300;
    if (w > 3000) w = 3000;
    warmup_k[k] = w;
  }

  vector<vector<double>> rate(N);
  vector<int> warmup_end_day(N, 0);
  vector<char> locked(N, 0);
  for (int k = 0; k < N; k++) rate[k].assign(ns[k] + 1, 0.0);

  int cur_day = 0;
  while (cur_day < max_days) {
    int next_emit = cur_day + emit_every;
    if (next_emit > max_days) next_emit = max_days;

    #pragma omp parallel for schedule(dynamic)
    for (int k = 0; k < N; k++) {
      if (locked[k]) {
        int span = next_emit - cur_day;
        for (int c = 1; c <= ns[k]; c++) state[k][c].score += rate[k][c] * (double)span;
        continue;
      }

      int wk = warmup_k[k];
      int real_end = next_emit < wk ? next_emit : wk;
      int real_span = real_end - cur_day;
      if (real_span > 0) {
        int nk = ns[k];
        double mk = mos[k];
        double pk = pens[k];
        double lk = leaders[k];
        const double *hk = haz[k].data();
        const double *vk = vw[k].data();
        auto &st = state[k];
        for (int d = 0; d < real_span; d++) {
          for (int c = 1; c <= nk; c++) one_day(st[c], c, nk, mk, pk, lk, hk, vk);
        }
        warmup_end_day[k] = real_end;
      }
      if (real_end >= wk) {
        for (int c = 1; c <= ns[k]; c++) rate[k][c] = state[k][c].score / (double)warmup_end_day[k];
        locked[k] = 1;
        int extra = next_emit - warmup_end_day[k];
        if (extra > 0) {
          for (int c = 1; c <= ns[k]; c++) state[k][c].score += rate[k][c] * (double)extra;
        }
      }
    }

    cur_day = next_emit;

    cout << cur_day;
    for (int k = 0; k < N; k++) cout << ' ' << best_of(state[k], ns[k]);
    cout << '\n' << flush;
    if (sleep_us > 0) usleep(sleep_us);
  }
  return 0;
}

int main(int argc, char** argv) {
  init_cohort_tables();

  if (argc >= 2 && strcmp(argv[1], "stream") == 0) return run_stream();

  if (argc >= 2 && strcmp(argv[1], "advise") == 0) {
    // advise n i m b d v h p l current_total current_inactive [days]
    int n = atoi(argv[2]);
    double inactive_odds = atof(argv[3]), max_odds = atof(argv[4]);
    double boost = atof(argv[5]), decay = atof(argv[6]);
    double veteran_bonus = atof(argv[7]), halflife = atof(argv[8]);
    double inactive_penalty = atof(argv[9]);
    double has_leader = atof(argv[10]);
    int current_total = atoi(argv[11]), current_inactive = atoi(argv[12]);
    int days = argc >= 14 ? atoi(argv[13]) : 3000;
    run_advise(n, inactive_odds, max_odds, boost, decay, veteran_bonus, halflife,
               inactive_penalty, has_leader, current_total, current_inactive, days);
    return 0;
  }

  if (argc >= 2 && strcmp(argv[1], "batch") == 0) {
    int days = 10000;
    if (argc >= 3) days = atoi(argv[2]);
    ios::sync_with_stdio(false);
    int bn; double bi, bm, bb, bd, bv, bh, bp, bl;
    while (cin >> bn >> bi >> bm >> bb >> bd >> bv >> bh >> bp >> bl) {
      cout << generate_best_count(bn, bi, bm, bb, bd, bv, bh, bp, bl, days) << "\n";
    }
    return 0;
  }

  int n, days = 10000;
  double inactive_odds, max_odds, boost, decay, veteran_bonus, halflife, inactive_penalty, has_leader;

  if (argc >= 10) {
    n = atoi(argv[1]);
    inactive_odds = atof(argv[2]);
    max_odds = atof(argv[3]);
    boost = atof(argv[4]);
    decay = atof(argv[5]);
    veteran_bonus = atof(argv[6]);
    halflife = atof(argv[7]);
    inactive_penalty = atof(argv[8]);
    has_leader = atof(argv[9]);
    if (argc >= 11) days = atoi(argv[10]);
    cout << generate_best_count(n, inactive_odds, max_odds, boost, decay, veteran_bonus, halflife, inactive_penalty, has_leader, days);
    return 0;
  }

  cout << "maximum clan members: [enter a number 0-10000]";
  cin >> n;
  cout << "\n";
  cout << "odds one clan member becomes inactive in a day [enter a number 0-100]: ";
  cin >> inactive_odds;
  cout << "\n";
  cout << "maximum odds of joining (assumed at n-1/n) [enter a number 0-1000]: ";
  cin >> max_odds;
  cout << "\n";
  cout << "early-churn boost (0 = none, extra flakiness for brand-new members): ";
  cin >> boost;
  cout << "\n";
  cout << "early-churn decay half-life in days: ";
  cin >> decay;
  cout << "\n";
  cout << "veteran score bonus (0 = none, max fractional bonus at saturation): ";
  cin >> veteran_bonus;
  cout << "\n";
  cout << "veteran bonus half-life in days: ";
  cin >> halflife;
  cout << "\n";
  cout << "inactive-member recruitment penalty (0 = none, 1 = fully discouraged by a 100% inactive roster): ";
  cin >> inactive_penalty;
  cout << "\n";
  cout << "does this clan have an always-active reserved leader slot (0 = no, 1 = yes): ";
  cin >> has_leader;
  cout << "\n";
  cout << generate_best_count(n, inactive_odds, max_odds, boost, decay, veteran_bonus, halflife, inactive_penalty, has_leader);
  return 0;
}
