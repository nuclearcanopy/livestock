## model

clan membership is simulated as a deterministic mean-field system: instead of
sampling individual members with an rng, the sim tracks *expected* member
counts per tenure cohort and propagates them forward with exact arithmetic.
this removes monte carlo noise entirely, so sweeping a parameter produces a
smooth curve instead of a jagged one where the "best count" threshold flips
unpredictably on sampling noise.

the technique (cohorts of people moving through tenure, advanced by expected
recruitment/wastage flow rather than simulated individuals) is the classical
"markov manpower planning" method:

- D.J. Bartholomew, *Stochastic Models for Social Processes*, Wiley, 3rd ed.
  1982.

### tenure cohorts

members are bucketed by days-since-joining into 14 fixed cohorts, independent
of clan size. this keeps per-day cost at O(cohorts), not O(members), so
sweeping many parameter points stays cheap regardless of how large `n` is.

```
[0,1) [1,2) [2,3) [3,4) [4,5) [5,6) [6,7) [7,14) [14,21) [21,35) [35,56) [56,90) [90,150) [150,inf)
```

the first 7 buckets are 1-day wide (daily resolution where the early-churn
decay curve changes fastest); later buckets widen geometrically; the last
bucket is a terminal sink that never ages further, since both the hazard and
score curves below are designed to saturate well before day 150.

### inactivation hazard

```
hazard(t) = inactive_odds * (1 + boost * exp(-t / decay))
```

brand-new members (`t=0`) are `boost`x more likely to go inactive than the
long-run baseline `inactive_odds`, decaying to baseline with half-life
`decay`. this declining-then-stabilizing shape (elevated early churn,
settling as tenure grows) is well documented across online communities, even
though exact Clash Royale parameters aren't available (see calibration,
below):

- N. Ducheneaut, N. Yee, E. Nickell, R. Moore, "The Life and Death of Online
  Gaming Communities: A Look at Guilds in World of Warcraft", CHI 2007 —
  guild churn magnitude in WoW (concrete numbers below).
- J. Kawale, A. Pal, J. Srivastava, "Churn Prediction in MMORPGs: A Social
  Influence Based Approach", IEEE Int'l Conf. on Computational Science and
  Engineering, 2009 — EverQuest II churn prediction via social influence.
- Z. Borbora, J. Srivastava, K.-W. Hsu, D. Williams, "Churn Prediction in
  MMORPGs Using Player Motivation Theories and an Ensemble Approach", IEEE
  SocialCom 2011 — EverQuest II, concrete churn-labeling definition below.
- M. Zhou, A. Mockus, "Who Will Stay in the FLOSS Community? Modeling
  Participant's Initial Behavior", IEEE TSE 41(1), 2015 — newcomer dropout
  hazard declining over tenure in OSS projects; the closest thing to an
  actual fitted tenure-hazard curve available, used as a shape proxy.

### veteran score bonus

```
weight(t) = 1 + veteran_bonus * (1 - exp(-t / halflife))
```

active members contribute marginally more to the daily score the longer
they've stuck around, saturating at `1 + veteran_bonus` as `t -> inf`. this
is a **design choice** rewarding retention, not a literature-calibrated
quantity — no source for a quantitative "veteran contribution multiplier"
was found during research, so don't cite this half as empirically grounded.

### default values

the UI ships with `boost=1.0`, `veteran_bonus=0.3`, `inactive_penalty=0.5`,
and the reserved-leader-slot toggle on. these are deliberately non-zero so
the tenure/penalty/leader mechanics are visibly doing something by default
rather than looking toggled-off — they are **not** fitted to data (per the
honesty note above, no quantitative source exists for `boost`/`veteran_bonus`
specifically), just moderate values picked to demonstrate the mechanism:
day-0 members ~2x as flaky as baseline, decaying over `decay=10` days;
veterans worth up to 30% more at saturation; a fully-inactive roster recruits
at half strength. `inactive_odds` (`i`) remains the one parameter with a real
literature-derived default (see above).

### kick / join mechanics (unchanged from the original aggregate model)

once total roster size reaches the `count` threshold, inactive members are
purged to free recruiting room; join odds scale with how full the roster
already is (`max_odds/(n-1) * current_members`). these aren't tenure-aware:
an inactive member already contributes 0 to score before being kicked, so
which one gets purged first doesn't affect the result.

### literature-derived numbers

pulled directly from the papers above (full text extracted via `pdftotext`,
not abstracts) — these are real, quotable figures, not estimates:

- **~25% monthly member churn in WoW guilds, stable across guild sizes.**
  "The churn percentage is around 25% and fairly stable across guilds of all
  sizes. In other words, if we see a guild that currently has 20 members,
  then over the past month, there were 5 members who have left the guild."
  (Ducheneaut et al. 2007, p.841). This directly implies a daily hazard: if
  25% leave over 30 days, daily survival `s` solves `s^30 = 0.75`, giving
  `s ≈ 0.99046`, i.e. **`inactive_odds ≈ 0.954%/day`** — now the tool's
  actual default (`server.py`'s `AXIS_DEFAULT["i"]`, `index.html`'s
  `AXES.i.init`), replacing the previous arbitrary 5%. This is the one
  parameter with a real, precise literature-derived value; see the
  calibration note below on why `boost`/`decay`/`veteran_bonus`/`halflife`
  are deliberately left uncalibrated instead of getting false-precision
  numbers assigned to them.
- **~25% of guilds "die" (disappear entirely) per month**, independent of
  the within-guild member churn above: "almost a quarter of the guilds we
  observe at any point in time have disappeared after a month" (p.841).
- **Guild size distribution is power-law**: mean 16.8, median 9, 90th
  percentile 35, largest observed 257 members (p.841) — useful context for
  what a "realistic" `n` range looks like, if extrapolating from WoW guilds
  to Clash Royale clans (which cap at 50).
- **Churn-switch behavior changes with tenure/level, but don't cite the
  specific direction without checking the original PDF layout first**: the
  extracted text reads "Between levels 21-40, unguild events are 3 times
  more frequent than guild switch events (4% vs. 13%); between levels
  41-60, unguild events are 7 times more likely than guild switch events
  (3% vs. 21%)" (p.841) — but the cited percentages contradict the prose
  (4%<13% and 3%<21%, i.e. the numbers say *switch* is more frequent by
  almost exactly 3x and 7x, the opposite of what the sentence claims). This
  is almost certainly a two-column PDF extraction artifact (`pdftotext` on
  academic layouts commonly interleaves columns/figure callouts), not
  necessarily an error in the paper itself. Don't quote a direction here
  without pulling the actual PDF page and checking the figure/column
  layout by eye first.
- **Roster-composition pressure drives churn**: "the quest for a
  well-balanced roster leads to churn, as players from the needed classes
  are recruited away from one guild to another" (p.843) — a concrete
  mechanism for why `max_odds`-style recruitment and churn are coupled, not
  independent knobs; this model's join/kick coupling is directionally
  consistent with this finding even though the mechanism (class balance) is
  WoW-specific.
- **Operational definition of "churned" in EverQuest II**: "churners are the
  players who have cancelled their subscription and/or those with no
  recorded activity in [the most recent] two months" (Borbora et al. 2011,
  p.2) — i.e. the field's own convention treats **~60 days of inactivity**
  as the threshold for "gone," independent of this paper's actual hazard
  curve shape. That's a useful order-of-magnitude anchor for `decay`/
  `halflife`: if 60 days of silence is "surely churned," a hazard curve
  with a `decay` half-life in the 10-30 day range (this model's UI default)
  is plausible, not a wild guess.
- **~100-day inactivity timescale cited for OSS/FLOSS contributor dropout**
  as consistent with Zhou & Mockus (2015)'s empirical findings, per later
  work building on it — same order of magnitude as the EverQuest II 60-day
  convention above, reinforcing that `decay`/`halflife` values in the
  tens-of-days range are the right ballpark rather than needing to span
  hundreds of days.

Kawale et al. 2009 is methodologically relevant (social-influence-based
churn prediction) but didn't yield an extractable standalone churn-rate
number distinct from Borbora's — it's cited for the modeling approach, not
a specific figure.

**Why `boost`/`decay`/`veteran_bonus`/`halflife` stay at their defaults
(0/10/0/30) rather than getting literature numbers applied like
`inactive_odds` did**: the 60-100 day figures above describe "how long
until someone's declared gone," not "how much more likely a brand-new
member is to flake, and how fast that excess fades" — a different quantity
this model's `boost`/`decay` actually control. No paper found gives that
specific curve for any platform in scope. Defaulting `boost`/`veteran_bonus`
to 0 means the tenure effects are off until deliberately turned on, which is
the honest choice given no real fit exists yet — not a value dressed up to
look calibrated.

### calibration status

no raw join/leave timestamped dataset exists, checked directly, for any of:

- **Clash Royale**: official API exposes `lastSeen` (usable for inactivity
  calibration) but no join date. no bulk/historical export exists anywhere
  (RoyaleAPI's own API shut down in 2020).
- **Clash of Clans**: official API has neither `lastSeen` nor join date.
- **EVE Online**: the one endpoint with real join dates
  (`/corporations/{id}/membertracking/`) is director-role-gated; the public
  alternative (`corporationhistory`) is per-character, partial, and has a
  history of being throttled/disabled by CCP.
- **Discord**: `joined_at` is real and per-member, but requires a bot with
  the privileged members intent in the target server; no leave timestamp or
  public dataset exists.
- **Steam groups**: no join-date field at all, snapshot-only.

practical path: self-poll the Clash Royale API's `/clans/{tag}/members`
against real clans over weeks-to-months to fit the actual inactivity-vs-
`lastSeen` distribution (this is the long-term background-harvesting plan),
using the published churn-shape literature above as a starting default in
the meantime. GitHub org/OSS contributor join-leave history (via the REST
API or GH Archive) is the best genuinely-timestamped surrogate dataset if a
faster stand-in is needed before enough Clash Royale data accumulates.

## performance

going from the original single-sample stochastic sim to a 14-cohort
deterministic one is ~14x more arithmetic per simulated day by design (that
cost buys the noise-free graphs). three optimizations brought a worst-case
graphing workload (1200 simultaneously-swept parameter points, each up to
200 members, full 150,000-day horizon) from **112.6s down to 6.4s wall-clock
— 17.6x**, verified bit-for-bit identical output against the unoptimized
version at each step:

1. **hoisting `exp()` out of the hot loop.** `hazard(t)` and `vet_weight(t)`
   only depend on the per-parameter-point knobs and a fixed per-cohort
   tenure table — never on the day or the count threshold — so they're
   identical on every single call within one simulation run. The original
   code recomputed them (with a transcendental `exp()` call) inside the
   innermost loop: ~363 million `one_day()` calls × 28 `exp()` calls each ≈
   10 billion transcendental evaluations for values that never changed.
   Precomputing a 14-entry table once per parameter point (`precompute_rates`
   in `main.cpp`) cut CPU time from 111.6s to 40.4s on its own — classic
   loop-invariant code motion, not an approximation.
2. **enabling the OpenMP parallelism that was already written but never
   compiled in.** `run_stream`'s `#pragma omp parallel for` across
   parameter sets had no effect because the documented build command
   (`shell.nix`) never passed `-fopenmp` — so it had been running
   single-threaded the whole time, pragma silently ignored. On this
   machine (Intel i5-8350U, 4 cores / 8 threads) this parallelizes the
   dominant cost ~3.3x in practice (hyperthreading gives diminishing
   returns on a compute-bound loop like this, so effective scaling tracks
   physical cores more than logical ones).
3. **deriving the warmup window analytically instead of using one
   worst-case constant for every parameter point.** The extrapolation
   trick (simulate until steady state, then extrapolate linearly) used a
   fixed `WARMUP = 3000` days sized for the slowest half-life the UI
   allows (365 days). A curve with a 10-day half-life (the UI's own
   default) is converged by ~60-180 days. Each parameter point now gets
   its own warmup window, `clamp(6 * max(decay, halflife), 300, 3000)` —
   6 half-lives is ~0.25% residual, and the 300-day floor is a safety
   margin for the roster-level join/kick equilibration dynamics, which
   don't have a closed-form decay constant in this model and so can't be
   derived the same way. This is the biggest single lever for realistic
   (non-worst-case) UI defaults, since most parameter points never need
   anywhere near 3000 days.

Notably: forking an OpenMP thread team on every single emit window — even
once every parameter point had already locked into pure linear
extrapolation, where the per-window work is trivial — measured *faster*
wall-clock than skipping the parallel region for those windows despite
using far more CPU-seconds (48.5s vs 12.0s CPU, but 6.4s vs 9.2s wall). Don't
trust CPU-seconds as a proxy for the metric that actually matters
(wall-clock latency) when parallelism is involved — measure the thing you
actually care about.
