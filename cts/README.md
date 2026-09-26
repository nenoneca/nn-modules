# nn HAL Compatibility Suite (CTS)

A test suite a new camera platform must pass **before** the application is
ported to it. It exercises each portability seam in nn-modules against its
written contract, on real hardware, with a bench peer where the contract
involves another party.

The five tiers, the case tables and the conformance matrix are described in
the plan. This directory is the source of truth for the **cases** and the
**platform declarations**; the plan's tables are kept in sync with these
files, not the other way round.

    cts/
      cases/            case records, one file per tier+area (t2_ble.yaml, …)
      platforms/        one declaration per platform (posix.yaml, hub8735.yaml)
      runner/           T0 today; T1–T3 on-target runner to come

## T0 — the symbol census

    cts/runner/t0_census.sh <platform>        # posix, hub8735, …

Links a generated table that takes the address of **every function declared
in every seam header** against the platform's declared backends. A seam
function with no implementation therefore fails here, on a host, rather than
on the board.

**A green T0 means the backend is complete. It says nothing about whether any
of it works.** The BlueZ backend had every symbol in this table and deadlocked
on its first write. Do not quote a green census as evidence about behaviour;
that is what T1 upward are for.

Three properties are worth knowing because each of them is a way this tier
could have lied while staying green:

- **The census is generated, never hand-written.** `gen_t0_symbols.py` parses
  the headers; `--check` fails the run if the committed `t0_symbols.c` has
  drifted from them. A hand-maintained list of 154 references stops covering
  whatever is added next week, and T0 then reports "complete" about a seam it
  is no longer looking at.
- **Only link symbols are censused.** The seam headers also provide 28 `static
  inline` helpers and one macro (byteorder, libc shims, a few accessors).
  Those come from the header itself, so a backend cannot fail to supply them
  and a census cannot check them — they need behavioural tests in T1. Do not
  read `t0_symbols.c` as the whole API surface; it is 154 of 183.
- **Narrowing is declared, never silent.** The seam surface is not uniform:
  on posix, `uart`, `watchdog` and `work` have no backend branch at all and
  `#error` out. A platform lists such groups in `skip_groups`, the census
  prints every skipped group, and a `skip_groups` entry that does not name a
  real header is a hard error — a typo there would skip nothing while looking
  like it skipped something.

### `known_missing`, and why it is not a way to pass

A symbol that is declared in a seam header the platform *does* supply, and has
no implementation, is a real gap. Listing it under `known_missing` with a
reason lets T0 gate the **next** one without a red CI on the day the tier is
introduced. Two rules keep that list honest:

- an unlisted missing symbol **fails**;
- a listed symbol that turns out to exist **also fails**, so the list cannot
  rot into a permanent excuse.

The contract it is holding platforms to: a backend that cannot do something
should still **define** the function and return `-ENOTSUP`. A missing symbol
is indistinguishable from a typo; an explicit `-ENOTSUP` is a decision a
reader can check.

### What the census found on its first run (2026-09-13)

`posix`, the reference platform, has **8** such gaps:

- `nn_osal_shell_{print,print_v,warn,error}` — `nn_osal/shell.h` has a POSIX
  branch in its internal backend header, so the platform claims the seam, but
  none of the four entry points is defined in `src/posix`.
- `nn_pal_ble_gatt_{discover,read,subscribe,write}` — the BlueZ backend is
  **peripheral only**; central-role GATT is declared and not implemented.

Both are recorded in `platforms/posix.yaml`. This is the hole the plan
predicted T0 would find ("returns `-ENOTSUP` by accident"), and it is worse
than predicted: the symbols are simply absent, so a caller gets a link error
rather than a value it can test.

## Adding a platform

1. Write `platforms/<name>.yaml`. Declare the compiler, the flags, the backend
   defines, the sources, and `skip_groups` for every seam it does not supply.
   Do this **before** writing backend code — the file is the work plan, and
   the shrinking `skip_groups` list is the port's real progress.
2. Set `linkable: no` while the final link still needs vendor libraries that
   are not vendored. T0 then compiles the census and reports **INCONCLUSIVE**,
   which proves the headers are coherent for the target and the skips are
   right, and proves nothing about completeness.
3. Record what must arrive with the board under `needed_from_vendor`, and what
   you do not know under `unknowns`. Both are load-bearing: `hub8735.yaml`
   names three vendor-gated blockers, each of which stops a tier.
