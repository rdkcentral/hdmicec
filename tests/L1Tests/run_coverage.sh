#!/usr/bin/env bash
# run_coverage.sh -- gcov/lcov coverage runner and line-coverage gate for the HDMI-CEC
# middleware's L1 unit-test suite (run_L1Tests).
#
# ==============================================================================
# PURPOSE
# ==============================================================================
#   Capture coverage from the instrumented middleware build, write an HTML report,
#   print a per-file table of LINE, FUNCTION and BRANCH figures derived from the lcov
#   trace records, enumerate every file below the bar together with its uncovered line
#   numbers, and exit non-zero when line coverage misses the threshold.
#
#   It exists because this submodule's own workflow already captures coverage but
#   (a) collects no branch data and (b) applies no numeric threshold whatsoever.
#   Everything else -- the capture directory, the exclusion globs, the artifact file
#   names and the genhtml title -- is reproduced from .github/workflows/L1-tests.yml
#   lines 150-173, which is this submodule's own recipe and the authority for every
#   lcov argument used below.  That workflow is NOT modified by this script and must not
#   be: it is the authority, so it is read, never rewritten.
#
#   Two gaps in the CI recipe, and how they are closed here:
#     1. NO BRANCH DATA.  lcov collects branch data only when asked, and the legacy
#        `lcov_branch_coverage` configuration key is deprecated in lcov 2.x and defaults
#        to zero.  The workflow passes no `--rc` and copies no lcov configuration, so the
#        CI step inherits the system default and discards branch data entirely.  This
#        script therefore passes `--rc branch_coverage=1` explicitly on the capture, the
#        filter, the genhtml and the summary invocations -- a run-time override outranks
#        every configuration file, which is why it appears on all four steps rather than
#        only in a configuration file.
#     2. NO THRESHOLD.  No numeric coverage check exists anywhere in this submodule's
#        build, Makefiles or workflows.  The `--fail-under-lines` invocation below is the
#        first machine-checkable coverage gate in this workspace.
#
# ==============================================================================
# GOVERNING CONTRACT
# ==============================================================================
#   This project has NO user-specified rules: `review_rules` returns exactly
#   "No user rules provided."  No rule governs this file, so none is cited and none is
#   invented.  Their absence is not licence to lower the bar, so the substituting binding
#   contract is the enterprise-standard bar (specification section 0.12.1):
#
#     1. REPOSITORY CONVENTION IS AUTHORITATIVE.  The workflow's recipe is reproduced
#        rather than reinvented, and the sibling runner
#        entservices-hdmicecsink/Tests/run_coverage.sh sets the shape (logging helpers,
#        absolute tool resolution, home-configuration isolation, awk trace parsing,
#        per-file table, gate spelling).  Where the tool's real behaviour contradicts
#        expectation the tension is documented, not silently resolved -- see the two
#        MEASURED CORRECTIONS below.
#     2. NO NEW FRAMEWORK, TOOL OR DEPENDENCY.  bash, the POSIX/coreutils primitives it
#        already needs (printf, awk, sed, grep, sort, find, wc, mktemp, dirname,
#        basename, mv, rm, ln, touch, mkdir), lcov 2.x, genhtml and gcov.  No gcovr, no
#        jq, no python, no coverage wrapper of any kind.
#     3. ADDITIVE AND NON-DESTRUCTIVE.  This script writes nothing into ccec/**,
#        osal/**, mocks/**, tests/L1Tests/test_main.cpp, tests/L1Tests/.gitignore, the
#        workflows, or /etc/lcovrc.  It never commits, never regenerates a committed
#        build file behind your back, and IT NEVER RUNS `git checkout` AT ALL -- not in
#        the coverage pipeline, not in `--restore`, and not as advice it prints for
#        someone to paste.  The promise covers advice as well as action for a measured
#        reason rather than a theoretical one.
#        `configure` OVERWRITES the six git-tracked Makefiles listed at
#        REGENERATED_MAKEFILES, and one of them -- ccec/src/Makefile -- is not toolchain
#        output at all: it is a hand-written RDK build file that carries real source
#        content, and it is in AC_CONFIG_FILES anyway, so `configure` clobbers it by
#        design.  `git checkout --` restores whatever the INDEX holds, which makes it
#        correct only under an assumption about the index and makes it silently DESTROY
#        an uncommitted edit to any of the six.  A measurement tool does not get to lose
#        someone's work as a side effect of measuring.
#        So `--build` SNAPSHOTS the working-tree bytes of all six immediately before it
#        runs autoreconf, and restores from that snapshot at the end of the SAME
#        invocation -- exactly what was there, committed or not, index or no index.  The
#        snapshot lives in a run-scoped mktemp -d directory registered with the same
#        EXIT/INT/TERM trap as everything else here, so a cancelled build restores too.
#        A standalone `--restore` that finds the six dirty and has no snapshot from its
#        own invocation REFUSES and exits non-zero, because the only thing it could
#        otherwise reach for is the `git checkout` this clause has just forbidden.
#        Its two side effects are stated rather than buried:
#          (a) $HOME is NOT one of them, and that is the point.  lcov reads
#              $HOME/.lcovrc silently, and a branch-disabled copy there would defeat
#              everything above -- CI plants exactly such a copy in the plugin jobs, so
#              the hazard is real.  This script does not move that file aside: it gives
#              every lcov and genhtml invocation a PRIVATE, EMPTY, mode-0700 HOME of its
#              own (see lcov_run/genhtml_run), so no home configuration can be in effect
#              and the caller's file is never opened, moved, replaced or deleted.
#              DO NOT REPLACE THIS WITH A STASH-AND-RESTORE OF ~/.lcovrc: the restoring
#              `mv -f` silently DESTROYS a ~/.lcovrc that reappeared during the run --
#              the owner's, or a concurrent sibling runner's -- and a file recreated
#              mid-run is in effect for every lcov call after it.  A private HOME has
#              neither failure mode and needs no trap to undo.
#              /etc/lcovrc is likewise never touched; --config-file below means lcov
#              reads exactly one configuration file, this suite's versioned one.
#          (b) Artifacts -- the fixed names under ARTIFACTS below are created and
#              OVERWRITTEN WITHOUT PROMPTING, exactly as CI overwrites them in
#              $GITHUB_WORKSPACE.  See ARTIFACT HYGIENE for where they land and why.
#          With --build or --run there are two further, opt-in side effects: the build
#          writes into this submodule's working tree, and the run zeroes this build
#          tree's *.gcda counters.  Both are off by default.
#     4. MEASURED CLAIMS ONLY.  Every number printed is read out of the trace captured
#        moments earlier.  Nothing is hard-coded, defaulted or estimated, and an empty
#        capture is a loud failure rather than a plausible-looking zero.  gcov counters
#        ACCUMULATE across runs, so a stale *.gcda keeps a line marked hit long after the
#        test that hit it stopped running -- which would let a gate pass on an earlier
#        run's evidence.
#        SO THE ORDER IS: ZERO ONCE, RUN THE MATRIX, CAPTURE ONCE -- not zero once per
#        invocation, and the reason is the reason the matrix exists at all.
#        The HAL back-end selection resolves ONCE PER PROCESS,
#        inside LibCCEC::init, so one binary run can only ever exercise one arm of the
#        selection branch; covering both needs several processes, and ACCUMULATION ACROSS
#        THEM is the only way anything ever sees both arms.  Zeroing before each
#        invocation would leave the capture describing only the last one -- a report that
#        looks complete and covers half the design.  Never zeroing would let an earlier
#        run's evidence in, which this clause forbids outright.  Zeroing once
#        before the first invocation and capturing once after the last one has PASSED has
#        neither failure mode, and the anti-stale guarantee holds either way: the figures
#        are an account of THIS run and of nothing before it.
#        The verification does not trust the tool: after zeroing,
#        the *.gcda files are RE-COUNTED rather than `lcov --zerocounters`'s exit status
#        being believed, and a survivor is fatal.  Nothing is captured unless the
#        invocations produced fresh counters.
#        ONE BUILD, ONE MACHINE, and this is a constraint rather than a convenience: gcov
#        counters cannot accumulate across machines, and a counter file from a different
#        build does not describe the same objects.  Every invocation therefore runs
#        against the tree this invocation of the script measured, which is also why the
#        hosted CI job cannot contribute its run to a capture made anywhere else.
#        Without `--run` the script reports how many counter files it found and when they
#        were last written, so a stale measurement is visible rather than silent.
#        A ZERO EXIT STATUS IS NOT TAKEN AS PROOF THAT ANY TEST RAN.  GoogleTest exits 0
#        when a filter selects nothing, so `GTEST_EXTRA_ARGS=--gtest_filter=NoSuchTest.*`
#        would otherwise be reported as "the L1 suite passed" and then measured at 0.0% -- a run
#        that failed only because the number happened to be zero, not because anything
#        asserted it had tested nothing.  A filter that still cleared the bar would have
#        been reported as a pass.  `--run` therefore DELETES the results file before the
#        binary starts and then requires it to exist, to report a non-zero test count, and
#        to name at least one of this suite's own fixtures -- the same three checks the
#        sibling plugin runners apply, for the same reason.
#     5. DETERMINISTIC AND ISOLATED.  Fixed artifact NAMES with no timestamp in any of them,
#        no wall-clock sleeps, no reliance on the caller's working directory: every path is
#        resolved absolutely from BASH_SOURCE, so running this script from its own
#        directory, from the submodule root or from the superproject root produces
#        identical results.  The coverage FIGURES are a pure function of the trace, so the
#        same trace always yields the same table -- with one deliberate exception, which is
#        stated rather than hidden: the provenance artifact and the leading `# PROVENANCE`
#        comment of the per-file TSV record the generation time in UTC, so an artifact found
#        later can be dated.  Those lines are the only non-reproducible bytes.  The HTML report is
#        built in a private staging directory and published by rename so no page from a
#        larger earlier trace can survive into a smaller later one.
#     6. HONEST REPORTING OVER CONVENIENT NUMBERS.  The seven exclusion globs are
#        reproduced VERBATIM and NOTHING is added to them.  Those globs are what keep the
#        coverage denominator production-source-only, which is what forces coverage to
#        move by adding tests rather than by editing source or widening a filter.  Files
#        below the bar are printed with their real figures and their uncovered line
#        numbers, never excluded to flatter a percentage.  The threshold is a GATE, not a
#        FILTER.
#     7. REPRODUCIBILITY.  `set -euo pipefail`, absolute path resolution, the exact
#        command forms below, and the full build recipe recorded in BUILD RECIPE so the
#        figures can be reproduced rather than trusted.
#
#   The zero-production-source-modification directive binds this file absolutely: it is a
#   read-and-measure tool.  Where a coverage gap cannot be closed without a production
#   change, this script's job is to REPORT it -- which is what the below-bar enumeration
#   is for -- not to close it.
#
# ==============================================================================
# MEASURED CORRECTIONS TO THE OBVIOUS SPELLINGS  (both verified against lcov 2.0-1)
# ==============================================================================
#   1. THE GATE MUST BE SPELLED WITH AN OPERATION.  The intuitive form
#          lcov --fail-under-lines 80 filtered_coverage.info
#      is rejected outright:
#          lcov: ERROR: invalid command line:
#          Need one of options -z, -c, -a, -e, -r, -l, --diff, --intersect,
#          --subtract, or --summary
#      and exits 2.  Because 2 is non-zero, that form LOOKS like a working gate while
#      actually failing for an unrelated reason and never once consulting the coverage
#      figure -- a gate that can only fail is no more a gate than one that can only
#      pass.  The correct spelling, used below, pairs the threshold with an operation:
#          lcov --summary filtered_coverage.info --fail-under-lines 80
#      Verified: exit 0 at or above the bar, exit 1 below it.  This script adds a THIRD
#      status of its own, 3, for a measurement that is arithmetically at or above the bar
#      but is not evidence anyone should accept -- see ADVISORY VERDICT below.
#   1a. AN ADVISORY VERDICT IS NOT AN ACCEPTANCE VERDICT (exit 3).  A whole family of
#      situations produces real numbers from evidence this invocation did not establish, or
#      from settings an acceptance run does not use: measuring counters without --run (gcov
#      counters ACCUMULATE, so they may credit a test that no longer runs), running the suite
#      with GTEST_EXTRA_ARGS instead of the matrix's own filters, deferring an invocation for
#      want of a binder driver, moving the bar, waiving the per-file half of the gate, and
#      either half of the branch-arm gate being unable to measure or to check a required arm.
#      Every one of them ends in "COVERAGE ADVISORY" and exit 3 rather than in "COVERAGE GATE
#      PASSED" and exit 0, because exit 0 is indistinguishable from a clean acceptance to any
#      caller, human or CI: the figures and every artifact are still produced, and a caller
#      that requires an acceptance verdict fails on the status instead of being told a
#      reassuring lie.  Below the bar is exit 1 in every mode.  EVERY trigger is enumerated in
#      the EXIT STATUS section of --help, which is the list to keep current: a summary here
#      that drifts from the call sites is how a qualifier such as "currently only when --run
#      was omitted" comes to stand beside triggers it does not describe.
#   2. FUNCTION FIGURES COME FROM THE ALIAS RECORDS, NOT THE LEADER RECORDS.  lcov 2.x
#      writes per-function data as FNL:<index>,<start>,<end> and
#      FNA:<index>,<count>,<name> (this trace carries 440 FNA and 431 FNL records and
#      zero of the older FN:/FNDA: records).  Counting the FNA aliases reproduces
#      `lcov --summary` EXACTLY -- both report 377/440 -- whereas the per-file FNF:/FNH:
#      leader totals give 375/431, because several aliases can share one leader.  The
#      FUNCTIONS column below is therefore alias-derived so that the TOTAL row reconciles
#      with the summary block, and the leader figures are printed alongside it rather than
#      dropped.  Only FNA's numeric SECOND field is read, so a demangled name containing
#      commas (templates, operator overloads) cannot corrupt the count; the name is
#      always the LAST field.
#   3. NOT EVERY FILE HAS BRANCH DATA, AND THAT IS CORRECT.  22 of the 30 files in the
#      filtered trace carry BRF:/BRH: records; the other 8 contain no branch arcs at all,
#      so lcov emits no branch record for them.  Those cells render "n/a  no data"
#      instead of a misleading 0.0%.
#   4. `--ignore-errors category` IS NEVER PASSED.  It is not a documented error class;
#      it is deliberately absent from every ignore list below and must not be added.
#   5. `lcov --list` IS NEVER USED.  It emits malformed rates above 100% in lcov 2.x.
#      Every figure here is parsed out of the trace records instead.
#
# ==============================================================================
# ARTIFACT HYGIENE -- READ BEFORE CHANGING THE OUTPUT LOCATION
# ==============================================================================
#   tests/L1Tests/.gitignore ignores *.o *.lo *.la .libs/ .deps/ Makefile Makefile.in
#   run_L1Tests *.log *.trs *.xml .dirstamp -- and does NOT ignore coverage.info,
#   filtered_coverage.info or coverage/.  hdmicec/.gitignore and the superproject's
#   .gitignore are both empty.  That .gitignore is OUT OF SCOPE for this change and is
#   not edited, so the mitigation is placement instead of ignoring: this script's outputs
#   are UNCOMMITTABLE BUILD ARTIFACTS -- the same discipline the six configure-generated
#   Makefiles get -- and they default to a directory OUTSIDE the git tree entirely:
#
#       mktemp -d "<safe parent>/hdmicec-l1-coverage.XXXXXXXX"
#
#   created atomically at mode 0700, with an UNPREDICTABLE name, and printed as the
#   "artifacts:" line when the run starts.  The name is unpredictable on purpose: a fixed
#   one under a world-writable directory can be pre-created by any local account as a symlink
#   or as a directory it owns, and every artifact written afterwards would land where it
#   chose.  A fresh root per run also means a trace from an earlier run cannot be mistaken
#   for this one's -- the same staleness discipline the counters get.
#
#   THE PARENT IS CHOSEN, NOT DEFAULTED TO /tmp.  select_safe_temp_parent tries TMPDIR, then
#   XDG_RUNTIME_DIR, then HOME, and takes the first whose whole ancestry passes the same
#   checks a caller-named --output-dir is held to; a group- or world-writable directory with
#   no sticky bit disqualifies a candidate rather than producing a warning that is then
#   ignored.  On this host that excludes /tmp, measured at mode 2777, and the run lands under
#   $HOME instead.
#   Outside the tree they cannot be staged even by `git add -A`, and a per-run root keeps
#   parallel checkouts of this superproject from overwriting each other's evidence without
#   needing any environment variable.  The CI file NAMES are preserved
#   exactly, so `--output-dir .` run from the superproject root reproduces CI's layout
#   byte for byte if that is what you want -- at which point keeping them out of a commit
#   becomes yours to manage.
#
#   ARTIFACT CUSTODY IS THE SAME ON BOTH BRANCHES.  A directory this script MINTS is
#   owner-only by construction; a directory a caller NAMED is held to the same standard
#   rather than accepted exactly as found, because accepting it as found leaves the files
#   inside it created at the caller's umask.  Under `umask 000` and a pre-existing mode-0755
#   directory that means every artifact at 0666, which lets an unprivileged local account
#   read them, append to them, and forge the gate's own input (a fabricated 100.0% row in
#   per_file_coverage.tsv) or squat on .run.lock to defeat the concurrency guard.  Three
#   things therefore hold for both branches:
#     * `umask 077` is set before anything is resolved, so every file this run or any of
#       its children creates is 0600 and every directory 0700 -- see the block above
#       `set -euo pipefail`'s successor at the top of the executable section;
#     * a caller-named directory is brought to mode 0700 and then asserted private
#       (restrict_artifact_dir_to_owner), the same two steps the minted branch ends with;
#     * a caller-named path is COLLAPSED lexically and then checked for where it lands
#       (canonicalise_path_lexically, assert_artifact_location_plausible), so a near-root
#       value or one whose '..' components collapse into a system tree is refused instead
#       of created -- the same refusal both plugin runners make, for the same reason.
#
# ==============================================================================
# USAGE
# ==============================================================================
#   ./run_coverage.sh                       measure existing profile data, report, gate
#   ./run_coverage.sh --run                 zero counters once, run the whole invocation
#                                           matrix, capture once, then the above
#   ./run_coverage.sh --build --run         build first, then the above
#   ./run_coverage.sh --threshold 90        raise the bar for a diagnostic run
#   ./run_coverage.sh --per-file-gate       also fail if any single file is below the bar
#                                           (the default -- Directive 4 states the bar PER TARGET)
#   ./run_coverage.sh --no-per-file-gate    opt out: gate on the aggregate only, and report the
#                                           per-file breaches without failing on them
#   ./run_coverage.sh --output-dir DIR      write artifacts to DIR
#   ./run_coverage.sh --no-html             skip the genhtml report
#   ./run_coverage.sh --restore             restore the six tracked Makefiles from this
#                                           invocation's snapshot, then exit (refuses when
#                                           they are dirty and no snapshot exists)
#   ./run_coverage.sh --help                full option and environment reference
#
#   ENVIRONMENT (all optional; command-line flags win)
#     COVERAGE_MIN            line-coverage bar, default 80
#     COVERAGE_OUTPUT_DIR     artifact directory, default as under ARTIFACT HYGIENE
#     COVERAGE_PER_FILE_GATE  1 to gate per file as well as on the aggregate, default 1
#                             (0 gates the aggregate only; the per-file enumeration is
#                             printed either way).  Directive 4 states the bar PER TARGET,
#                             so the per-file half is ON unless it is switched off explicitly
#     SUITE_TIMEOUT           wall-clock bound in seconds on the L1 suite under --run,
#                             default 600; exceeding it is reported as a hang, not a failure
#     GTEST_PREFIX            prefix providing libgtest/libgmock, default <WS>/install/usr
#     GTEST_EXTRA_ARGS        extra arguments appended to the run_L1Tests command line
#
#   ARTIFACTS (fixed names, written under the output directory)
#     coverage.info              raw capture over the whole submodule -- and the BRANCH GATE'S
#                                INPUT, deliberately unfiltered (see ONE TRACE, TWO CONSUMERS)
#     filtered_coverage.info     production-source-only trace, after the seven globs; the
#                                CI-interchangeable form, and the input to the step below
#     line_gate_coverage.info    filtered_coverage.info with the dependency headers removed --
#                                the LINE GATE'S INPUT, and what the per-file table, the
#                                uncovered-line list, the summary and the HTML report describe
#     coverage/index.html        genhtml report, with a Branches column
#     per_file_coverage.tsv      machine-readable per-file LINE/FUNCTION/BRANCH figures
#     uncovered_lines.txt        per-file uncovered line numbers for below-bar targets
#     rdkTestResults_invocation_<L>.json   GoogleTest results, ONE PER INVOCATION under --run
#     run_invocation_<L>.log               that invocation's captured output, one per invocation
#     provenance.txt             the full manifest: revisions, toolchain, runner checksum
#     run_status.txt             THE AUTHORITATIVE MARKER: which run id this directory holds,
#                                which invocations ran, which deferred, and the outcome.  Read
#                                this FIRST when an artifact's provenance is in doubt -- the
#                                names above are shared by every run that writes this directory.
#                                A run claims it BEFORE it removes or writes anything else and
#                                stops if it cannot, and it exits non-zero if it cannot record
#                                its own verdict -- so this file never carries one run's
#                                acceptance verdict over another run's artifacts
#     capture.log filter.log filter_dependencies.log genhtml.log build.log
#
#   THE DIRECTORY HOLDS EXACTLY ONE GENERATION.  Every artifact a previous run could have left
#   under one of these names is removed at run start, under the output lock, before anything is
#   written -- see ONE GENERATION PER DIRECTORY.  Without that, an invocation that DEFERS in this
#   run (B, C and E defer on any host with no binder driver) would leave the previous run's
#   results file standing as this run's evidence.
#
#   PER-INVOCATION NAMES CARRY A LETTER, NEVER A TIMESTAMP.  The suite is run once per
#   selection outcome (see THE INVOCATION MATRIX below), and a single results file would mean
#   the last run's evidence standing in for all of them -- an empty or failed invocation erased
#   by whichever green one came after it.  The letter keeps them distinct while keeping clause 5
#   intact: the file set is a fixed, predictable function of the matrix, so the same tree
#   produces the same names and a reader does not have to list the directory to find them.
#
#   The DIRECTORY is the override point; the FILE NAMES are fixed, and that is deliberate
#   rather than an omission.  coverage.info, filtered_coverage.info and coverage/ are the
#   names the workflow writes into $GITHUB_WORKSPACE, so keeping them makes a local run's
#   output interchangeable with a CI artifact bundle and lets any downstream consumer name
#   one path.  Making them individually configurable would buy nothing and would let two
#   runs disagree about what the evidence is called; when you need two sets of artifacts
#   side by side, give each run its own --output-dir.
#
# ==============================================================================
# PREREQUISITES
# ==============================================================================
#   * lcov 2.x, genhtml and gcov on PATH.
#   * The suite must have been BUILT WITH INSTRUMENTATION and EXECUTED, so that *.gcno
#     and *.gcda exist under this submodule.  `--build` and `--run` will do both; without
#     them the script measures whatever counters are already present and says so.
#   * libglib2.0-dev is a HARD requirement of the build: configure.ac runs
#     PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 0.10.28]) and configure fails without it.
#   * --enable-l1tests is what pulls in PKG_CHECK_MODULES([GTEST], [gtest >= 1.10.0]);
#     without that flag no test suite is configured at all.
#   * stubs/ is a BUILD PREREQUISITE, not a committed artifact -- the workflow generates
#     it and it is excluded from coverage by the '*/stubs/*' glob.
#
# ==============================================================================
# BUILD RECIPE  (what --build runs; reproduced from .github/workflows/L1-tests.yml)
# ==============================================================================
#   cd <hdmicec>
#   mkdir -p stubs/rdk/iarmbus stubs/ccec/drivers/iarmbus
#   touch stubs/rdk/iarmbus/libIARM.h stubs/rdk/iarmbus/libIBus.h \
#         stubs/rdk/iarmbus/libIBusDaemon.h stubs/ccec/drivers/iarmbus/CecIARMBusMgr.h
#   ln -sf ../../../mocks/hdmicec/hdmi_cec_driver.h stubs/ccec/drivers/hdmi_cec_driver.h
#   autoreconf -if
#   PKG_CONFIG_PATH="$GTEST_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH" \
#   CPPFLAGS="-I$PWD/mocks -I$PWD/stubs -I$GTEST_PREFIX/include" \
#   LDFLAGS="-L$GTEST_PREFIX/lib -fprofile-arcs -ftest-coverage" \
#   CXXFLAGS="-fprofile-arcs -ftest-coverage" ./configure --enable-l1tests \
#       [HALIF_PREFIX=DIR] [HALIF_LIB_DIR=DIR] \
#       [BINDER_SDK_DIR=DIR] [BINDER_SDK_INCLUDE_DIR=DIR]
#   make -j$(nproc) all && make -C tests/L1Tests all && make -C tests/L2Tests all
#
#   THE STUB RECIPE IS SPELLED TWICE IN THIS FILE -- here, and executed in do_build -- and
#   the two are kept identical on purpose: a documented recipe that has drifted from the
#   one that runs is worse than no documentation, because it is trusted.  It carries no
#   entry for the AIDL back-end, and that is not an omission: stubs/ mutes the IARM bus
#   headers this middleware includes but does not ship, and substitutes the HAL driver mock
#   for the real legacy driver header.  The AIDL headers are REAL and are found through the
#   prefixes below, so there is nothing to stub.
#
#   THE FOUR AIDL/BINDER PREFIXES ARE OPTIONAL ON THE COMMAND LINE AND MANDATORY IN EFFECT.
#   libRCEC links the generated AIDL client stubs and the Binder client library
#   unconditionally, for every SOC vendor, so the build needs their headers and libraries --
#   but this script names no path for them.  configure.ac declares all four with AC_ARG_VAR
#   and DERIVES from them the four include roots (the hdmicec and common snapshot roots, the
#   common/current root that is the only source of halcompat.h, and the Binder header root),
#   the -L directories, the four -l edges and the C++17 flag.  Omit them and configure looks
#   for the sibling rdk-halif-aidl checkout; get them wrong and configure fails naming the
#   roots it tried.  Either way there is ONE place that decides, which is what keeps the
#   Autotools build and the hand-written ccec/src/Makefile from being configured differently.
#
#   `make -C tests/L2Tests all` is the third make and builds two programs: the L2 runner and
#   the out-of-process fake service host.  Without it invocations D and E have no binary.
#
#   The PKG_CONFIG_PATH line is the one addition to the workflow's own command, and it is
#   not optional off a CI runner: configure.ac uses PKG_CHECK_MODULES for GoogleTest, which
#   consults pkg-config ONLY and ignores -I/-L.  CI additionally apt-installs libgtest-dev,
#   so a system gtest.pc satisfies the check there; a host whose GoogleTest is a
#   source-built copy in a private prefix needs the prefix on pkg-config's path or configure
#   aborts with "Google Test not found".
#
#   MANDATORY BUILD HYGIENE.  autoreconf and configure REWRITE six GIT-TRACKED Makefile
#   files in this submodule (REGENERATED_MAKEFILES below).  Five of them are pure local
#   toolchain output.  THE SIXTH IS NOT: ccec/src/Makefile is a hand-written RDK build
#   file with its own object list and link command -- real source content -- and it is
#   listed in configure.ac's AC_CONFIG_FILES all the same, so configure clobbers it by
#   design.  Measured, not assumed: after a `--build` on this host the regenerated copy
#   does not contain the DriverAidlImpl.o entry the hand-written one carries.
#
#   SO `--build` SNAPSHOTS ALL SIX AND RESTORES THEM ITSELF.  The snapshot is taken from
#   the WORKING TREE immediately before autoreconf runs, held in a run-scoped mktemp -d
#   directory, and restored at the end of the same invocation from the trap that already
#   cleans up the private lcov HOME -- so a cancelled or failed build restores too.
#   Nothing is left for anyone to paste.
#
#   DO NOT REVERT THESE SIX WITH `git checkout`, AND THIS SCRIPT DOES NOT OFFER IT.
#   A blanket checkout over those paths restores whatever the INDEX holds: it is right
#   only under an assumption about the index, and it silently DESTROYS an uncommitted
#   edit to any of the six -- an edit to ccec/src/Makefile most of all, that being the
#   one a person actually works on.  Restoring the bytes that were there is
#   unconditionally correct; restoring the bytes git happens to hold is not.  `--restore`
#   therefore restores from a snapshot or refuses: with the six dirty and no snapshot
#   from its own invocation it exits non-zero and says why, rather than reaching for git.
#   No `git checkout` is executed anywhere in this script, in any mode.
#
# ==============================================================================
# ONE TRACE, TWO CONSUMERS
# ==============================================================================
#   The capture is taken ONCE and read in two forms, because the two gates ask different
#   questions and neither form answers both.  This is stated up here rather than left to
#   the function that does it, because "two gates read two files" looks like a defect
#   until the reason is in front of the reader.
#
#   THE BRANCH GATE READS THE UNFILTERED CAPTURE.  libRCEC carries the AIDL back-end, so
#   some of the branches that have to be accounted for live in header-only code
#   outside this submodule -- above all in halcompat.h's isCompatible<>(), whose empty-hash,
#   "-1", "notfrozen", era and major arms are exactly what the selection rests on.  Filter
#   first and those records are gone, and a branch gate that cannot see its own subject
#   passes silently and for ever.
#
#   THE LINE GATE READS A DERIVATIVE with the dependency headers removed, and only them.
#   Its 80% threshold is an aggregate, and an aggregate means nothing without a stable
#   denominator.  MEASURED -- one full `--build --run` on this host, gcc/gcov 13, with
#   invocations A and D executed and B, C and E deferred for want of a binder driver: the
#   RECORD LINEAGE IS 146 -> 43 -> 32.  The capture holds 146 source-file records; the seven
#   globs reduce that to 43; and of those 43, ELEVEN are dependency headers -- six from the
#   binder SDK and five generated AIDL stubs -- leaving the 32 the line gate judges: 31 inside
#   this submodule plus halcompat.h, which is retained deliberately (see below).  Left in, the
#   eleven move the denominator away from the one the baseline was measured over and, because
#   per-file gating is on, start failing runs over how well this suite covers a third-party
#   header it neither owns nor tests.
#
#   halcompat.h IS RETAINED in the derivative: it is a consumed HALIF header called from
#   the selection path, not toolchain code.  ccec/src/Driver.cpp and
#   ccec/src/DriverAidlImpl.cpp are never filtered from either form, and their presence is
#   ASSERTED after filtering rather than inferred from the globs not naming them.
#
#   The seven globs themselves are UNTOUCHED and no exclusion is added to them -- clause 6
#   holds.  The dependency exclusions are a separate list applied in a separate step
#   producing a separate artifact, so filtered_coverage.info means exactly what its comment
#   says it means.
#
# ==============================================================================
# THE LEGACY-PATH BASELINE -- REFERENCE ONLY, AND LABELLED AS SUCH
# ==============================================================================
#   These are the figures this suite measured BEFORE the AIDL back-end existed, on the
#   legacy path alone.  They are recorded here so a later run has something to compare
#   against, and they are labelled because a number without its provenance is the thing
#   clause 4 exists to prevent:
#
#     483 tests from 18 suites, all passing, 6545 ms
#     aggregate over 30 source files: 94.5% line (1999/2115), 93.2% function (425/456),
#                                     58.4% branch (1122/1920)
#     ccec/src/DriverImpl.cpp  97.5% line / 94.4% function / 66.2% branch
#     ccec/src/LibCCEC.cpp     100%  line / 100%  function / 65.9% branch
#
#   THEY ARE NOT A POST-CHANGE EXPECTATION AND MUST NOT BE READ AS ONE.  The suite is
#   larger than that, ccec/src/DriverAidlImpl.cpp is in the denominator, and -- decisively -- the
#   figures a run produces depend on HOW MANY INVOCATIONS THE HOST COULD RUN.  On a host
#   with no binder driver, three of the five are deferred and the AIDL back-end's source is
#   unmeasured by construction, so the aggregate is necessarily lower and says nothing about
#   the test set.  NO POST-CHANGE AGGREGATE IS WRITTEN DOWN HERE, on purpose: the only
#   honest source for one is a full five-invocation run on a binder-capable host, and this
#   script prints what it measured on whatever host it ran on rather than carrying a number
#   somebody would then compare the wrong thing against.
#
# ==============================================================================
# GATE
# ==============================================================================
#   LINE COVERAGE ONLY.  The aggregate check is delegated to lcov's own
#   --fail-under-lines so the number that decides the outcome is lcov's, not this
#   script's, and its non-zero exit is what fails the run.  Every file below the bar is
#   ALWAYS enumerated -- with its uncovered line numbers, so the "enumerate the specific
#   uncoverable lines and the reason" requirement can be answered directly from this
#   output.
#
#   THE PER-FILE HALF OF THE GATE IS ON BY DEFAULT.  Directive 4 sets the bar PER TARGET,
#   so an aggregate that clears 80% while one file sits at 33% clears a total and misses the
#   requirement.  Every file in this suite's filtered trace is at or above the bar, so
#   there is nothing for the per-file half to be lenient about.
#
#   That includes the three template-heavy headers under ccec/include that measured below it at
#   the engagement baseline: ccec/include/ccec/Operand.hpp 0.0% (0/6),
#   Exception.hpp 33.3% (4/12), Messages.hpp 76.5% (228/298).  Each is covered the only way a
#   coverage bar may be closed, by TESTS rather than by exemption: Operand's three default
#   virtuals and all seven Exception::what() overrides are exercised from
#   ccec/test_Operands.cpp, and the message types those figures leave unserialised from
#   ccec/test_MessageEncoder.cpp.  None of them is exempted, and none is excluded from the
#   trace.
#
#   `--no-per-file-gate` (or COVERAGE_PER_FILE_GATE=0) downgrades the per-file half to a
#   report, for a diagnostic run that wants the table without the verdict.  It is not a
#   setting an acceptance run uses.  COVERAGE_GATE_EXEMPT_FILES -- empty by default -- can
#   suppress one named file's vote, and the rules for when that is admissible are stated at
#   its definition below.  The enumeration prints in every mode, so neither switch can hide a
#   figure; they only decide whether it fails the run.
#
#   BRANCH COVERAGE IS REPORTED AS EVIDENCE AND NEVER GATED.  gcov models branches as
#   control-flow-graph arcs, and a C++ translation unit's arcs include compiler-generated
#   exception and static-destruction paths that no test can drive, so 100% branch
#   coverage is unattainable for most translation units and a low branch rate is not by
#   itself evidence of a weak test.  Branch movement is the evidence that negative and
#   corner-case tests landed; line coverage is the acceptance criterion.
#
#   RANDOMISED-ORDER RUNS ARE NOT WIRED IN, ON PURPOSE.  `--gtest_shuffle` is a
#   diagnostic: this suite contains pre-existing order-fragile tests that pass in the
#   default order and are not rewritten, so a shuffled run's outcome depends on the seed --
#   green for some, assertion failures for others, and an exit 139 from a production race
#   for others again (the measured seed table is in tests/L1Tests/README.md).  A result that
#   depends on the seed must never gate anything.  Nothing here enables it, and no watch
#   mode exists or is added.
#
# ==============================================================================
set -euo pipefail

# ------------------------------------------------------------------------------------
# FILE MODE FOR EVERYTHING THIS RUN CREATES.  Set here, before any path is resolved and
# long before any byte is written, because it is inherited by every child too -- lcov,
# genhtml, gcov and the test binary all create files in this run's name.
#
# WHY IT IS NOT ENOUGH TO CHMOD THE DIRECTORY.  The artifact DIRECTORY is created 0700 on
# the branch that mints it, but a directory's mode says nothing about the FILES inside it:
# absent a umask of this run's own they are created at whatever umask the caller happens
# to have.  Under a permissive one -- `umask 000` is the case measured here --
# coverage.info, filtered_coverage.info, per_file_coverage.tsv, uncovered_lines.txt,
# capture.log, filter.log and .run.lock are all written 0666.  Inside a caller-supplied
# directory that is world-traversable, an unprivileged local account can then READ them,
# APPEND to them, and FORGE the very file the gate reads: a fabricated 100.0% row in
# per_file_coverage.tsv, or a .run.lock
# it can hold to break the concurrency guard.  For a script whose only product is
# trustworthy coverage evidence, that is the failure that matters -- not confidentiality
# (the artifacts hold source paths and counts, never secrets) but INTEGRITY.
#
# 077 rather than 022: group and other get nothing at all.  Nothing in this pipeline is
# read by another account -- the suite, lcov, genhtml and the gate all run as this user
# in this process tree -- so there is no consumer to break, and CI collects artifacts as
# the same user that produced them.  The HTML report stays fully readable by its owner.
# provenance.txt chmods itself to 600 in its own right; the umask makes every other
# artifact match it instead of leaving it the exception.
# ------------------------------------------------------------------------------------
umask 077

# ------------------------------------------------------------------------------------
# DIRECT-OBJECT LOADER VARIABLES ARE REFUSED, NOT FILTERED.
#
# build_trusted_library_path below rebuilds the loader's SEARCH PATH from roots this run
# resolved and proved safe.  That closes the "which directory answers for libbinder.so"
# question and nothing else: the variables in the set below do not name directories to
# search, they name OBJECTS TO LOAD, or they change how symbols bind, so a rebuilt search
# path has no effect on any of them whatsoever.
#
# MEASURED, on this host, before this guard existed: a marker DSO whose constructor appends
# to a file, exported through LD_PRELOAD, ran 384 times during `run_coverage.sh --help` and
# 758 times during `--selftest`.  Every child this script launches -- lcov, genhtml, gcov,
# awk, git, make, the L1 and L2 test binaries and the out-of-process fake service host --
# loads and runs the named object first.  For a COVERAGE runner that is not only a code
# execution surface, it is an evidence-integrity failure: the figures, the per-invocation
# pass counts and the gate verdict would all be produced by processes an object nobody
# reviewed had already modified, and the run would report them as measurements.
#
# THE SET, and why each member is in it:
#
#   LD_PRELOAD        loads the named objects before every other, so their constructors run
#                     in every child and their symbols interpose on every library.
#   LD_AUDIT          loads audit objects into the link namespace; their la_* callbacks see
#                     and can redirect every symbol binding in the process.
#   LD_PROFILE        makes the loader profile the named object and write a profiling file,
#                     which both changes the loaded object set and writes into a directory
#                     this run did not choose.
#   LD_DYNAMIC_WEAK   makes weak definitions overridable, which is symbol interposition
#                     without naming an object at all.
#   LD_ORIGIN_PATH    changes what $ORIGIN expands to, redirecting resolution of any
#                     RPATH/RUNPATH entry.  Included because it is the one search-path
#                     mechanism the rebuild below cannot reach: the rebuild owns
#                     LD_LIBRARY_PATH, not the RUNPATH stored inside a binary.
#
# REFUSED RATHER THAN SCRUBBED AT THE TOP, and the distinction matters.  Scrubbing silently
# would run the measurement in an environment the caller did not get, and the caller would
# read a green result without ever learning that what they configured was discarded.  A
# refusal names the variable, states the remedy, and costs the caller one `env -u`.
#
# WHY THIS BLOCK SITS HERE, ABOVE EVERYTHING, AND RUNS AT LOAD TIME RATHER THAN FROM main().
# "Refuse before anything is launched" is only true if nothing has been launched yet, and
# measurement decides where "yet" is.  Only `set -euo pipefail` and `umask` precede this
# point, so this script has spawned NO child process when it runs.  Placed at the first
# statement of main() instead it would follow the four dirname/basename calls that resolve
# this script's own path, the timeout(1) capability probe and the `id -u` read -- measured:
# nine loads of the preloaded object instead of two.  The residual two are `/usr/bin/env` and
# `bash` themselves, which load LD_PRELOAD before this file's first line is parsed and which
# no in-script measure can reach; that is the floor, and it is stated rather than glossed.
#
# THE PRICE OF THE POSITION, paid deliberately: log(), warn(), die() and render_untrusted()
# are all defined below this point, so this block cannot call any of them.  It therefore
# spells its own two-line output inline -- and it DOES NOT ECHO THE VALUE.  That is not a
# limitation dressed up as a decision: the value is a caller-supplied object path, echoing it
# would be a CWE-117 surface at the one place in this file that has no renderer available,
# and the variable NAME is the whole of what a caller needs in order to unset it.  Nothing
# downstream reads these variables, so nothing later needs the value either.
#
# EMPTY IS NOT THE SAME AS ABSENT, and one member of the set makes that load-bearing:
# glibc's loader activates LD_DYNAMIC_WEAK on the PRESENCE of the variable and ignores its
# value, so `LD_DYNAMIC_WEAK=` is active while `LD_PRELOAD=` is inert.  A present-but-empty
# variable is therefore removed outright and the removal is reported -- it cannot be a
# deliberate instruction, because for every member of the set the empty value either means
# nothing or means something the caller cannot have intended.
#
# THE REFUSAL APPLIES TO EVERY INVOCATION, INCLUDING --help, --selftest AND --restore --
# necessarily so, now that it runs at load time, and that is the intended answer rather than
# a consequence tolerated.  The reason differs per action:
#   * --selftest MUST be subject to it.  Its green result is used as evidence that this
#     script's own decision functions are sound, and it launches external tools (bash -n,
#     the lcov/gcov version probes in require_tools).  A self-test that passed under an
#     interposed loader would be evidence produced by the very contamination the finding is
#     about -- 758 marker executions, measured.
#   * --restore MUST be subject to it.  It WRITES into the source tree, restoring six
#     git-tracked Makefiles.  A run that modifies a checkout is the last place to make an
#     exception.
#   * --help is subject to it because one refusal at one point is the whole of the
#     mechanism.  A per-action exemption would be a second code path to keep correct, and
#     the reproduction for this finding is `--help` itself, so exempting it would leave the
#     reproduction green.  The cost is one line of output naming the remedy.
# Every action remains usable in the sense that matters: unchanged in any environment that
# does not carry one of these variables, and refusing with an actionable message in one that
# does.
# ------------------------------------------------------------------------------------
LOADER_DIRECT_OBJECT_VARIABLES=(LD_PRELOAD LD_AUDIT LD_PROFILE LD_DYNAMIC_WEAK LD_ORIGIN_PATH)

# One variable's state, as a word.  Split out from the refusal so it can be exercised
# directly by the self-test: a function that dies cannot be table-tested.
#
#   unset  -- not in the environment at all.  Nothing to do.
#   empty  -- present with an empty value.  Removed, because presence alone is enough for
#             LD_DYNAMIC_WEAK and an empty value is meaningless for the rest.
#   set    -- present and non-empty.  Fatal.
LOADER_OBJECT_VARIABLE_STATE=''
loader_object_variable_state() { # $1=variable name -> sets LOADER_OBJECT_VARIABLE_STATE
    # ${!1+set} is the only spelling that distinguishes "unset" from "set to empty" while
    # `set -u` is in force; ${!1:-} would collapse the two into one answer.
    if [ -z "${!1+set}" ]; then
        LOADER_OBJECT_VARIABLE_STATE='unset'
    elif [ -n "${!1}" ]; then
        LOADER_OBJECT_VARIABLE_STATE='set'
    else
        LOADER_OBJECT_VARIABLE_STATE='empty'
    fi
}

# Refuse the run if any member of the set is present and non-empty; remove any that is
# present and empty.  Called at LOAD TIME, immediately below, before this script has spawned
# a single child process of its own.
assert_no_loader_object_variables() {
    local name state
    local -a offenders=()

    for name in "${LOADER_DIRECT_OBJECT_VARIABLES[@]}"; do
        # A function call, not a command substitution: $( ) forks a subshell, and a fork here
        # would be one more process with the object loaded - in the one function whose whole
        # purpose is that there are none.  loader_object_variable_state() is kept as a
        # separate, table-testable predicate and read through a variable instead.
        loader_object_variable_state "$name"
        state="$LOADER_OBJECT_VARIABLE_STATE"
        case "$state" in
            unset) ;;
            empty)
                unset -v "$name"
                printf '[run_coverage] WARNING: %s was present with an empty value and has been removed.\n' \
                    "$name" >&2
                printf '[run_coverage] WARNING:   glibc activates LD_DYNAMIC_WEAK on presence alone and ignores the\n' >&2
                printf '[run_coverage] WARNING:   value, so an empty member of this set is not reliably inert.  Nothing\n' >&2
                printf '[run_coverage] WARNING:   was refused: an empty value cannot be a deliberate instruction here.\n' >&2 ;;
            set)   offenders+=("$name") ;;
        esac
    done

    if [ "${#offenders[@]}" -gt 0 ]; then
        printf '[run_coverage] ERROR: refusing to run with %d direct-object loader variable(s) set: %s\n' \
            "${#offenders[@]}" "${offenders[*]}" >&2
        printf '[run_coverage] ERROR:   These do not name directories to search -- they name objects to LOAD, or\n' >&2
        printf '[run_coverage] ERROR:   they change how symbols bind -- so this script'"'"'s loader-path rebuild has no\n' >&2
        printf '[run_coverage] ERROR:   effect on them.  Every child of this run (lcov, genhtml, gcov, awk, git, make,\n' >&2
        printf '[run_coverage] ERROR:   the L1 and L2 test binaries and the fake service host) would load and run the\n' >&2
        printf '[run_coverage] ERROR:   named object first, so the coverage figures, the per-invocation pass counts\n' >&2
        printf '[run_coverage] ERROR:   and the gate verdict would be produced by processes an object nobody reviewed\n' >&2
        printf '[run_coverage] ERROR:   had already modified.  Refused rather than scrubbed silently, so that a\n' >&2
        printf '[run_coverage] ERROR:   measurement is never taken in an environment the caller did not get.\n' >&2
        printf '[run_coverage] ERROR:   The values are deliberately NOT reproduced here: the name is what you unset,\n' >&2
        printf '[run_coverage] ERROR:   and this is the one point in this file that runs before the diagnostic\n' >&2
        printf '[run_coverage] ERROR:   renderer exists.\n' >&2
        printf '[run_coverage] ERROR:   REMEDY: env -u %s %s <options>\n' \
            "${offenders[0]}" "${BASH_SOURCE[0]}" >&2
        printf '[run_coverage] ERROR:   If an object genuinely must be interposed, do it in a separate diagnostic\n' >&2
        printf '[run_coverage] ERROR:   run and do not present its output as a measurement.\n' >&2
        exit 1
    fi
}

# Remove every member of the set from the environment of a launch, in the subshell that
# performs it.  This is defence in depth: assert_no_loader_object_variables has already
# refused the run if the caller supplied one, so the only way a member can be present here
# is that something INSIDE this script set it -- a helper, or a future edit -- and the
# launched binary must not inherit it in that case either.
#
# `unset -v` rather than an empty command-prefix assignment, and that is not a stylistic
# choice: LD_DYNAMIC_WEAK is activated by presence, so `LD_DYNAMIC_WEAK= cmd` would leave it
# active in the child.  Removal is the only spelling that holds for the whole set.  It runs
# in the launch's own subshell, so it extends the existing "compose the child's environment
# explicitly at the launch" mechanism rather than replacing it, and the parent shell's
# environment is untouched.
scrub_loader_object_variables() {
    unset -v "${LOADER_DIRECT_OBJECT_VARIABLES[@]}"
}

# CALLED HERE, AT LOAD TIME.  Two statements have run before it -- `set -euo pipefail` and
# `umask` -- and neither spawns a process, so no child of this script has been created yet.
assert_no_loader_object_variables


# ------------------------------------------------------------------------------------
# Path resolution.  Everything is derived from this script's own location so that the
# caller's working directory is irrelevant: the CI recipe captures over the submodule
# root while this file lives two levels below it, and `-d hdmicec` in the workflow is
# only correct when the working directory happens to be the superproject root.
#   SCRIPT_DIR    <hdmicec>/tests/L1Tests
#   HDMICEC_ROOT  <hdmicec>                 -- the lcov capture directory
#   WS            the directory holding <hdmicec>, i.e. CI's $GITHUB_WORKSPACE
# `pwd -P` resolves symlinks so that the paths compared against trace records, and the
# paths handed to `lcov -d`, are the same physical paths gcov recorded.
# ------------------------------------------------------------------------------------
SCRIPT_PATH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/$(basename -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname -- "$SCRIPT_PATH")"
HDMICEC_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd -P)"
WS="$(dirname -- "$HDMICEC_ROOT")"
readonly SCRIPT_PATH SCRIPT_DIR HDMICEC_ROOT WS

# ------------------------------------------------------------------------------------
# Tool resolution.  Absolute paths are taken from the inherited environment up front so
# that a later PATH change -- for example prepending an install tree before running the
# suite -- cannot swap the measurement tools underneath the pipeline.
# ------------------------------------------------------------------------------------
resolve_tool() { # $1=tool name -> absolute path on stdout, empty when absent
    command -v -- "$1" 2>/dev/null || true
}
LCOV_BIN="$(resolve_tool lcov)"
GENHTML_BIN="$(resolve_tool genhtml)"
GCOV_BIN="$(resolve_tool gcov)"
AWK_BIN="$(resolve_tool awk)"
FIND_BIN="$(resolve_tool find)"
MKTEMP_BIN="$(resolve_tool mktemp)"

# `timeout` bounds the suite run under --run, and do_run() REFUSES to run the suite without it
# rather than degrading to an unbounded run -- an unbounded run of a suite that drives real
# mutexes, condition variables and threads can hang for ever with no output and no verdict.
# Resolved here with everything else so a later PATH change cannot swap it, and resolved
# unconditionally because an invocation that only measures existing counters does not need it.
TIMEOUT_BIN="$(resolve_tool timeout)"

# --kill-after is desirable (a suite that ignores SIGTERM still dies) but is NOT universally
# safe: this workspace's `timeout` is uutils coreutils, and with -k it reports a timeout as exit
# 125 rather than GNU's 124 -- and 125 also means "timeout itself failed", so the two become
# indistinguishable and a hang would be misreported as a broken invocation.  One cheap probe
# settles it for this host instead of inferring it from a version string: a 1s bound on a 3s
# sleep must yield exactly 124 before -k is used at all.  This is the probe the run step's
# comment refers to.
TIMEOUT_KILL_AFTER=()
if [ -n "$TIMEOUT_BIN" ]; then
    timeout_probe=0
    "$TIMEOUT_BIN" -k 1 1 sleep 3 >/dev/null 2>&1 || timeout_probe=$?
    if [ "$timeout_probe" -eq 124 ]; then
        TIMEOUT_KILL_AFTER=(-k 30)
    fi
    unset timeout_probe
fi
readonly TIMEOUT_BIN TIMEOUT_KILL_AFTER
# stat is how the ancestry of every artifact path is checked (owner, mode, type) before a
# byte is written to it.  It is coreutils, like the rest of the primitives above.
STAT_BIN="$(resolve_tool stat)"

# flock is how this run proves it is the only one operating on this working tree.  Resolved
# here with the other primitives -- rather than probed with `command -v` at the moment of use --
# for the same reason as the rest: a PATH change made later in the run (the suite step prepends
# an install tree) must not be able to swap or hide it.  It is util-linux, and its ABSENCE is
# FATAL for every acceptance operation; see acquire_tree_lock for why that is not negotiable.
FLOCK_BIN="$(resolve_tool flock)"

# cmp is how the Makefile restore PROVES it put the bytes back rather than reporting that it
# tried.  A restore that returns 0 because `cp` returned 0 establishes that a write happened,
# not that the file now matches the snapshot -- a short write, a full filesystem or a
# concurrent writer all leave `cp` happy.  cmp is POSIX and ships with diffutils on every
# distribution this suite builds on; its absence is fatal at the point of use, not here,
# because an invocation that never builds never restores.
CMP_BIN="$(resolve_tool cmp)"
readonly LCOV_BIN GENHTML_BIN GCOV_BIN AWK_BIN FIND_BIN MKTEMP_BIN STAT_BIN FLOCK_BIN CMP_BIN

# ------------------------------------------------------------------------------------
# Configuration.  Every value is overridable from the environment, and the defaults are
# the ones that reproduce CI.  Command-line flags are applied after parsing and win.
# ------------------------------------------------------------------------------------
COVERAGE_MIN="${COVERAGE_MIN:-80}"
# Per-file gating defaults ON, because Directive 4 states the bar PER TARGET: an aggregate
# that clears 80% while one target sits at 33% satisfies the letter of a total and none of
# the intent.  `--no-per-file-gate` / COVERAGE_PER_FILE_GATE=0 downgrades it to a report for
# a diagnostic run; it is not a setting an acceptance run uses.  See GATE in the header.
COVERAGE_PER_FILE_GATE="${COVERAGE_PER_FILE_GATE:-1}"

# ------------------------------------------------------------------------------------
# THE PER-FILE GATE'S EXEMPTION LIST -- newline-separated trace paths, matched WHOLE-LINE
# against the `file` column of per_file_coverage.tsv (which is relative to $WS).
#
# EMPTY BY DEFAULT, AND THAT IS THE POINT.  Every file in the filtered trace of this suite
# is at or above the bar, including the three template-heavy ccec/include headers whose
# engagement-baseline figures sit below it -- Operand.hpp (0/6), Exception.hpp (4/12) and
# Messages.hpp (228/298).  Each is covered the only way a coverage bar may be closed: by
# tests (ccec/test_Operands.cpp and ccec/test_MessageEncoder.cpp), never by listing it here.
#
# WHAT AN ENTRY HERE MEANS, AND WHAT IT DOES NOT.  An entry suppresses one file's per-file
# pass/fail VOTE.  It does not remove the file from the trace, from the aggregate
# denominator, from the per-file table, or from the uncovered-line enumeration -- all four
# still report it, and apply_gate() prints every exempt file with its figure and a pointer
# back to this comment.  So an exemption is a visible, argued decision, not a filter.
#
# THE ONLY ADMISSIBLE REASON is that a line cannot be reached from a test-only change --
# Directive 6 puts production source out of scope, and Directive 4 then requires the
# specific lines and the reason to be enumerated in the traceability report.  "No test
# covers it yet" is not that reason: it is a gap, and the answer to a gap is a test.
# An entry added for any other reason lowers the bar while appearing to hold it.
#
# FORMAT, with the reason on the line above each path, e.g.
#     COVERAGE_GATE_EXEMPT_FILES="hdmicec/ccec/src/Example.cpp"
# and for more than one, one path per line inside the quotes.
# ------------------------------------------------------------------------------------
COVERAGE_GATE_EXEMPT_FILES="${COVERAGE_GATE_EXEMPT_FILES-}"

# Wall-clock bound in seconds on the run_L1Tests invocation under --run.  This suite normally
# finishes in about two seconds, so 600 is generous by two orders of magnitude and exists only
# so that a deadlock -- this suite drives real pthread mutexes, condition variables and threads
# -- is reported as a HANG with the last test named, instead of blocking for ever with no
# output, no exit status and no gate verdict.  0 is refused rather than honoured, because
# timeout(1) reads it as "no limit" and would silently restore the unbounded run.
SUITE_TIMEOUT="${SUITE_TIMEOUT:-600}"
# Artifact directory.  EMPTY BY DEFAULT, and that is the security-relevant part: with no
# explicit choice this script creates its root with `mktemp -d` under the parent
# select_safe_temp_parent chose, so the name is unpredictable and the directory is created
# atomically at mode 0700 (see resolve_output_dir).  DO NOT GIVE IT A FIXED DEFAULT: a
# predictable path such as <parent>/hdmicec-l1-coverage/<workspace basename> is guessable, and
# anything able to create entries in a world-writable parent could pre-create that name as a
# symlink or as a directory of its own and collect, redirect or tamper with every trace, log
# and HTML page written under it.  An unpredictable name cannot be pre-created, and a
# world-writable parent is not chosen at all.
# A caller who needs a stable location still passes --output-dir / COVERAGE_OUTPUT_DIR,
# and that path is then held to the stricter checks in resolve_output_dir, because a name
# chosen in advance is by definition guessable.
OUTPUT_DIR="${COVERAGE_OUTPUT_DIR:-}"
# 1 when the caller named the directory (flag or environment), 0 when this script mints it.
OUTPUT_DIR_EXPLICIT=0
if [ -n "$OUTPUT_DIR" ]; then
    OUTPUT_DIR_EXPLICIT=1
fi
# Prefix providing libgtest/libgmock and their headers.  CI uses $GITHUB_WORKSPACE/install/usr.
GTEST_PREFIX="${GTEST_PREFIX:-$WS/install/usr}"
GTEST_EXTRA_ARGS="${GTEST_EXTRA_ARGS:-}"

# ------------------------------------------------------------------------------------
# THE AIDL/BINDER STAGING PREFIXES -- four names, and NOT ONE HARD-CODED PATH.
#
# libRCEC links the generated AIDL client stubs and the Binder client library
# unconditionally, for every SOC vendor, so the build this script drives needs to find their
# headers and libraries.  It does NOT get to decide where those are.
#
# WHY THE NAMES ARE THESE FOUR AND NOT NAMES OF THIS SCRIPT'S CHOOSING.  configure.ac declares
# exactly these four as AC_ARG_VAR precious variables and derives everything else from them --
# the four include roots, the -L directories, the four -l edges and the C++17 dialect flag are
# all AC_SUBSTed outputs computed inside configure, not inputs anybody supplies twice.  Two
# staging prefixes are the whole of the orchestrator contract and the other two are overrides
# for split staging layouts, and the hand-written ccec/src/Makefile reads the same four names
# from the environment for exactly the same reason.  So this script passes the four through and
# lets configure derive the rest: a path spelled here as well would be a second place for the
# two build systems to disagree, which is the failure the single-prefix contract exists to
# prevent.
#
#   HALIF_PREFIX            root of the staged rdk-halif-aidl tree.  Left unset, configure
#                           falls back to the sibling checkout beside this submodule, which is
#                           the normal in-workspace arrangement -- so this stays EMPTY by
#                           default rather than duplicating that fallback here and risking a
#                           different answer.
#   HALIF_LIB_DIR           stub library directory, when it is not HALIF_PREFIX/lib/halif
#   BINDER_SDK_DIR          root of the staged Binder SDK, holding lib/binder and
#                           include/binder_sdk
#   BINDER_SDK_INCLUDE_DIR  Binder header prefix, when it is not under BINDER_SDK_DIR
#
# Every one of them is read from the environment and passed on untouched.  An unset variable is
# passed as unset rather than as an empty string, because `configure BINDER_SDK_DIR=` is a
# different instruction from omitting it: the first says "there is no Binder SDK", which fails
# the configure-time check, and the second lets configure look where it knows to look.
# ------------------------------------------------------------------------------------
HALIF_PREFIX="${HALIF_PREFIX:-}"
HALIF_LIB_DIR="${HALIF_LIB_DIR:-}"
BINDER_SDK_DIR="${BINDER_SDK_DIR:-}"
BINDER_SDK_INCLUDE_DIR="${BINDER_SDK_INCLUDE_DIR:-}"

DO_BUILD=0
DO_RUN=0
DO_HTML=1
DO_RESTORE_ONLY=0

# Reasons this invocation's figures, however good, are NOT an acceptance verdict.  Empty
# means the numbers stand on evidence this run established itself; non-empty makes the
# final verdict ADVISORY and the exit status 3.  See ADVISORY VERDICT in the header.
ADVISORY_REASONS=''
readonly EXIT_ADVISORY=3

note_advisory() { # $1=one-line reason
    if [ -z "$ADVISORY_REASONS" ]; then
        ADVISORY_REASONS="$1"
    else
        ADVISORY_REASONS="$ADVISORY_REASONS
$1"
    fi
}

# ------------------------------------------------------------------------------------
# Constants reproduced from .github/workflows/L1-tests.yml.
#
# THE SEVEN EXCLUSION GLOBS, VERBATIM AND IN THE WORKFLOW'S ORDER.  Do not add one, do
# not remove one, do not reorder them.  They are architecturally load-bearing: they strip
# system headers, the installed dependency headers, the generated stub tree, the HAL
# driver mock, GoogleTest, the whole test tree and any test translation unit from the
# denominator, which leaves production source only.  That is precisely what forces
# coverage to move by adding tests rather than by editing production source or widening a
# filter, so widening this list would not raise coverage -- it would destroy the
# guarantee that the number means anything.  Each element is single-quoted at its point of
# use and every expansion below is quoted, so the shell never gets a chance to glob them
# against the working directory.
# ------------------------------------------------------------------------------------
readonly LCOV_EXCLUDES=(
    '/usr/include/*'
    '*/install/usr/include/*'
    '*/stubs/*'
    '*/mocks/*'
    '*/googletest/*'
    '*/tests/*'
    '*/test_*.cpp'
)

# ------------------------------------------------------------------------------------
# THE DEPENDENCY-HEADER EXCLUSIONS -- a SECOND, SEPARATE list, and separate on purpose.
#
# WHY THEY ARE NOT ADDED TO THE SEVEN ABOVE.  Clause 6 says the seven exclusion globs are
# reproduced verbatim from the workflow and that NOTHING is added to them, and that is not
# ceremony: those seven are what make a local run's filtered trace interchangeable with CI's
# artifact bundle, and a list that has quietly grown by two is a list that no longer means what
# its comment says.  So this is a second list, applied in a second step, producing a third
# artifact -- and the seven-glob step keeps its name, its content and its meaning untouched.
#
# WHY A SECOND LIST IS NEEDED AT ALL.  libRCEC carries the AIDL back-end, so the compiled
# objects reference header-only code from outside this submodule: halcompat.h's templates, the
# generated Bp*/Bn* inline definitions, and binder SDK headers.  gcov attributes records to
# those paths, and NONE of the seven globs describes them.  MEASURED on this host, from one full
# `--build --run` with invocations A and D executed and B, C and E deferred: the capture holds
# 146 source-file records, the seven globs reduce that to 43, and ELEVEN of those 43 are
# dependency headers -- six binder SDK headers and five generated AIDL stub headers -- which
# this second list removes.  That leaves 32 records for the line gate: 31 inside this submodule
# and halcompat.h, retained for the reason below.  So the lineage is 146 raw -> 43 after the
# seven globs -> 32 in the trace the line gate reads.  Left in, they change the file set the per-file
# gate judges and the denominator the aggregate is computed over, so the figure would
# not be comparable with the recorded baseline and the per-file gate would start voting on
# whether a binder SDK header is well tested by this suite.
#
# WHAT IS FILTERED AND WHAT IS DELIBERATELY KEPT:
#
#   '*/binder_sdk/*'      the binder SDK header root.  configure.ac fixes that directory name
#                         -- the header root is <prefix>/include/binder_sdk -- so the glob
#                         describes the layout rather than one host's path.  This is third-party
#                         toolchain code that this suite neither owns nor tests.
#   '*/com/rdk/hal/*'     the GENERATED AIDL stub headers.  The C++ AIDL backend package-paths
#                         every generated header under com/rdk/hal/, so this one glob catches
#                         them from any prefix and at any snapshot version -- which a
#                         version-spelled glob would not.  They are committed pre-generated
#                         code, not source anyone in this repository writes.
#
#   halcompat.h IS RETAINED, and that is the important half of this policy.  It lives at
#   <halif prefix>/common/current/halcompat.h -- OUTSIDE com/rdk/hal/, which is precisely why
#   the glob above separates the two cleanly rather than by luck -- and it is a CONSUMED HALIF
#   HEADER rather than third-party toolchain code: getService<I>() and isCompatible<I>() are
#   called from the selection path, their branches are branches this migration is answerable
#   for, and SC6 asks for them by name.  Filtering it would delete the evidence.
#
# Anything the caller states about the staging layout is added below, at resolve time, so a
# Yocto-style layout whose binder headers do not sit under a binder_sdk directory is still
# described.  A prefix that is not set contributes nothing.
# ------------------------------------------------------------------------------------
readonly DEPENDENCY_EXCLUDES=(
    '*/binder_sdk/*'
    '*/com/rdk/hal/*'
)

# ------------------------------------------------------------------------------------
# THE RECORDS THAT MUST NEVER DISAPPEAR FROM EITHER DERIVED TRACE, WHATEVER ANY GLOB SAYS.
#
# A filter that removed one of these would leave a green gate standing over code nobody
# measured -- the one failure mode a filter can produce while looking like it is working.  The
# globs above do not name them, but "the globs do not name them" is a claim about the globs, and
# what follows is a check on the RESULT.  Asserted after BOTH filtering steps, because the two
# steps apply two different lists and either one could be the one that goes wrong.
#
# WHY halcompat.h IS ON THIS LIST AND NOT MERELY MENTIONED IN A COMMENT.  AAP 0.6.5 requires it
# retained in the filtered copy: it is CONSUMED HALIF SOURCE, not third-party toolchain code.
# getService<I>() and isCompatible<I>() are called from the selection path, and SEVENTEEN of the
# 89 required branch arms in BRANCH_MANIFEST below live inside isCompatible<>() -- both arms of
# the null gate, of the empty-hash and "-1" gates, of the "notfrozen" gate, and of each of the
# era, major and ordering comparisons.  The branch gate reads the UNFILTERED capture, so those
# seventeen are not what a
# missing record here would cost; what it would cost is halcompat.h's LINES silently leaving the
# aggregate denominator and the per-file gate, so a header whose branches this migration is
# answerable for would stop being judged at all while every step still printed success.
#
# It is retained by the DEPENDENCY_EXCLUDES policy above rather than by accident: it lives at
# <halif prefix>/common/current/halcompat.h, OUTSIDE com/rdk/hal/, which is why the generated-stub
# glob separates the two cleanly.  This entry is the check that the separation actually held.
#
# PATHS ARE SUFFIXES, AND ARE NOT ALL SUBMODULE-RELATIVE.  The first two are relative to this
# submodule's root; halcompat.h lives in the SIBLING rdk-halif-aidl checkout, so its suffix
# starts one level above.  Nothing here may be an absolute path: the same trace is read on this
# host, on a CI runner and inside the QEMU guest, and each of the three has a different prefix.
# ------------------------------------------------------------------------------------
readonly PROTECTED_TRACE_FILES=(
    'ccec/src/Driver.cpp'
    'ccec/src/DriverAidlImpl.cpp'
    'rdk-halif-aidl/common/current/halcompat.h'
)

GENHTML_TITLE='hdmicec coverage'
# NOT readonly: write_provenance() appends the superproject's short SHA so that every page of
# the HTML report carries the revision it was produced from. A trace or a report that does not
# say which tree it came from cannot be relied on: artifacts produced in one clone and read as
# though they applied to another are unusable, and nothing in the artifact itself would say so.
PROVENANCE_TXT=''

# Error classes tolerated per step.  `category` is NOT a documented class and is
# deliberately absent from all three lists -- do not add it.  `deprecated` appears
# because .lcovrc_l1 deliberately uses the backward-compatible key spellings
# (lcov_branch_coverage, lcov_function_coverage, geninfo_no_exception_branch) so that a
# pre-2.x lcov can still read it; the doubled token is lcov's own spelling for demoting
# the warning to a counted message, and the counts stay visible in the message summary
# and in the per-step logs, so nothing is hidden.
readonly LCOV_CAPTURE_IGNORE='mismatch,gcov,unused,empty,negative,source,graph,inconsistent,corrupt,deprecated,deprecated'
readonly LCOV_FILTER_IGNORE='unused,empty,inconsistent,deprecated,deprecated'
readonly LCOV_SUMMARY_IGNORE='empty,inconsistent,deprecated,deprecated'
readonly GENHTML_IGNORE='unused,empty,inconsistent,source,deprecated,deprecated'

# Branch collection, forced at run time.  A command-line --rc outranks ~/.lcovrc,
# /etc/lcovrc and --config-file alike, which is why it is repeated on capture, filter,
# genhtml and summary instead of being left to configuration.
readonly LCOV_RC_ARGS=(--rc branch_coverage=1)

# This suite's versioned lcov configuration, passed with --config-file so that lcov reads
# it INSTEAD OF ~/.lcovrc and /etc/lcovrc.  It carries the genhtml presentation settings
# and geninfo_auto_base=1, which is what lets a single capture over the submodule root
# resolve both the ccec/src and osal/src object trees of this autotools build.  Verified:
# passing it yields byte-identical trace metrics to omitting it, so it changes
# presentation and path resolution, never the measurement.  If it is absent the run
# continues without it rather than failing, because the measurement does not depend on it.
LCOV_CONFIG_ARGS=()
if [ -f "$SCRIPT_DIR/.lcovrc_l1" ]; then
    LCOV_CONFIG_ARGS=(--config-file "$SCRIPT_DIR/.lcovrc_l1")
fi

# ------------------------------------------------------------------------------------
# The six git-tracked Makefiles that autoreconf/configure rewrite (see BUILD RECIPE).
#
# FIVE OF THESE ARE TOOLCHAIN OUTPUT AND THE SIXTH IS NOT, which is the whole reason the
# snapshot machinery below exists.  ccec/src/Makefile is a hand-written RDK build file --
# its own CFLAGS, its own explicit object list, its own link command -- and it is in
# configure.ac's AC_CONFIG_FILES anyway, so configure overwrites it with generated content
# every time.  Restoring it is therefore not "throwing away generated noise": it is putting
# a source file back, and the only mechanism that does that correctly under every condition
# is one that remembers the bytes.
# ------------------------------------------------------------------------------------
readonly REGENERATED_MAKEFILES=(
    Makefile
    ccec/Makefile
    ccec/src/Makefile
    osal/Makefile
    osal/src/Makefile
    tests/Makefile
)

# ------------------------------------------------------------------------------------
# THE MAKEFILE SNAPSHOT -- why the restore remembers the bytes instead of asking git for
# them.
#
# WHY `git checkout` IS NOT THE MECHANISM.  `git -C <hdmicec> checkout -- <the six>`
# restores WHATEVER THE INDEX HOLDS, which is a different claim from "what was there before
# configure ran", and the difference is not academic: an uncommitted edit to any of the six
# is destroyed outright, with no prompt and no copy.  ccec/src/Makefile is precisely the
# file someone edits -- it is hand-written source, not output -- so the path where a blanket
# checkout does the most damage is also the one it would most often be aimed at.  No mode of
# this script runs it, and no message prints it as advice for a caller to paste.
#
# WHAT THE SNAPSHOT DOES INSTEAD.  `--build` copies the working-tree bytes and the MODE of all six
# into a run-scoped mktemp -d directory immediately before autoreconf -- with autoreconf as the
# literal next mutating command, the stub-header step running ahead of it -- and
# the EXIT/INT/TERM trap restores from that copy at the end of the same invocation.  It is
# unconditionally correct: it does not consult the index, does not care whether the file is
# committed, modified, staged or pristine, and cannot revert anything that was not itself about
# to be clobbered.  A cancelled build restores too, because the restore hangs off the trap
# rather than off the build's success path.
#
# THREE PROPERTIES THE RESTORE HAS THAT A `cp` LOOP DOES NOT, each closing a way a restore
# can leave the tree worse than it found it:
#   * ATOMIC PER FILE.  Each path is written to a temporary IN THE SAME DIRECTORY and renamed
#     into place, so there is no moment at which a tracked Makefile holds a partial write for
#     an impatient second Ctrl-C to catch.
#   * UNINTERRUPTIBLE AS A WHOLE.  INT, TERM and HUP are masked across the loop and re-armed
#     after it, so a signal cannot land between two of the six and leave half the tree
#     pre-build and half generated.
#   * VERIFIED, NOT ASSUMED.  Every restored file is compared against the snapshot and its mode
#     re-read afterwards; every recorded-absent path is confirmed absent.  A failure keeps the
#     snapshot, names it, and makes the run exit non-zero.
#
# WHAT IT DELIBERATELY DOES NOT DO.  It does not fall back to git when there is no snapshot,
# and it does not treat a git index comparison as a substitute for one.  A fallback would
# bring back exactly the destruction described above, at the one moment a caller is least able
# to notice -- and "the safe thing was unavailable, so the unsafe thing ran" is not a
# recovery.  A standalone `--restore` has no snapshot by construction, so it always reports
# that and exits non-zero.
#
# PRESENCE IS PART OF THE SNAPSHOT, NOT A SIDE EFFECT OF WHAT WAS COPIED.
#
# A record that was merely the list of paths successfully copied would make "absent" and
# "never recorded" the same value, and the consequence is one-directional: a path that did
# NOT exist when the snapshot was taken -- an unconfigured checkout has none of the five
# toolchain-generated Makefiles -- is created by configure and then left behind, because a
# restore that only puts back what it copied has nothing to say about it.  The tree that came
# out of the run would differ from the tree that went in, in the one respect this mechanism
# exists to prevent, and `git status` would report five untracked-looking modifications
# nobody asked for.
#
# So every allowlisted path carries an EXPLICIT state, and the restore acts on both:
#   present -- content and mode were captured; the restore puts those bytes back and proves it
#   absent  -- the path did not exist; the restore REMOVES it if the build created one
# A path with no record at all is a defect in the snapshot, and the restore says so rather
# than skipping it.
#
#   MAKEFILE_SNAPSHOT_DIR       the run-scoped copy, empty when none has been taken
#   MAKEFILE_SNAPSHOT_STATE     one '<state>|<mode>|<path>' line per allowlisted path, where
#                               state is `present` or `absent` and mode is the octal mode as
#                               it was found (or '-' for an absent path).  '|' is the
#                               separator this script already uses for its record tables, and
#                               none of the six literal paths contains one.
#   MAKEFILE_SNAPSHOT_IDENTITY  "<device>:<inode>" of the snapshot directory, recorded when it
#                               is created and re-checked before the restore reads from it.
#                               The snapshot is the ONLY copy of six tracked files, one of
#                               them hand-written build source, so restoring from a directory
#                               that is no longer the one this run wrote is not a degraded
#                               restore -- it is writing someone else's content over tracked
#                               files under the name of a recovery.
#   MAKEFILE_SNAPSHOT_RETAIN    1 once a restore has failed.  The snapshot is then the only
#                               remaining copy of the pre-build content, so cleanup_makefile_
#                               snapshot leaves it on disk and names it.  Removing the recovery
#                               evidence at the exact moment recovery became necessary is the
#                               one outcome this mechanism must never produce.
# ------------------------------------------------------------------------------------
MAKEFILE_SNAPSHOT_DIR=''
MAKEFILE_SNAPSHOT_STATE=''
MAKEFILE_SNAPSHOT_IDENTITY=''
MAKEFILE_SNAPSHOT_RETAIN=0

# Re-prove custody of the snapshot directory at the moment it is read from or written to.
# Same four properties as assert_output_dir_still_safe, for the same reason, against a
# directory whose contents matter even more: this is the recovery copy.
assert_snapshot_dir_still_safe() { # $1=what is about to happen, for the message
    local what="${1:-a snapshot operation}" dir_now

    [ -n "$MAKEFILE_SNAPSHOT_DIR" ] || die "internal error: assert_snapshot_dir_still_safe
       called with no snapshot directory recorded ($what)"
    assert_safe_ancestry "$MAKEFILE_SNAPSHOT_DIR" minted
    [ -d "$MAKEFILE_SNAPSHOT_DIR" ] || die "the Makefile snapshot directory disappeared during
       the run: $MAKEFILE_SNAPSHOT_DIR
       It holds the only copy of the six tracked Makefiles as they were before this run, and
       $what cannot proceed without it."
    assert_private_dir "$MAKEFILE_SNAPSHOT_DIR"

    if [ -n "$MAKEFILE_SNAPSHOT_IDENTITY" ]; then
        dir_now="$(path_identity "$MAKEFILE_SNAPSHOT_DIR")"
        [ "$dir_now" = "$MAKEFILE_SNAPSHOT_IDENTITY" ] || die "the Makefile snapshot directory is
       no longer the directory this run created: $MAKEFILE_SNAPSHOT_DIR
       created $MAKEFILE_SNAPSHOT_IDENTITY, the name now resolves to ${dir_now:-nothing} (device:inode).
       Refusing $what: restoring from a substituted snapshot would write content this run
       never saved over six tracked files, one of which is hand-written build source."
    fi
}

# ------------------------------------------------------------------------------------
# THE TREE LOCK -- one lock, tree-scoped, covering EVERY stage, fail-closed.
#
# WHAT IT PROTECTS, AND WHY ONE LOCK RATHER THAN A LOCK PER STAGE.  Every stage of this
# script writes to, or reads a verdict out of, the SAME working tree:
#
#   the stub headers and the HAL mock symlink       written into the tree
#   the six tracked Makefiles                       overwritten by autoreconf/configure,
#                                                   snapshotted before and restored after
#   the object and .gcno files                      produced by the build
#   the .gcda counters                              zeroed once, then accumulated by every
#                                                   invocation in the matrix
#   the capture, the filter and the gate            READ those counters and decide the verdict
#
# A lock that covered only the build would leave the rest of
# that list unprotected, and the unprotected part is where a second run does the most damage:
# it can zero the counters between this run's last invocation and its capture, so the capture
# reports a tree that nothing exercised; or run its own suite while this one captures, so the
# trace mixes two runs' counters and the coverage figure belongs to neither.  Neither shows
# up as an error -- both produce a plausible number -- which is exactly why the lock has to
# span from the first write to the last read rather than bracket the build alone.
#
# So: ONE lock, taken before the first stage that touches the tree and held until the exit
# trap has finished restoring and cleaning up.  Re-entrant by memo (acquire_tree_lock is a
# no-op once it holds it), so any stage may assert it without a second acquisition.
#
# TREE-SCOPED, from the absolute path of the submodule root: two clones of this superproject
# get two different locks and never block each other, which is what parallel clones need.
#
# THE NAME IS PREDICTABLE AND THAT IS UNAVOIDABLE -- two runs that must exclude each other
# have to agree on it -- so the DIRECTORY it lives in is what carries the safety: a private
# per-user directory created with mkdir (so it cannot be adopted from a foreign owner) and
# re-read with lstat, under a parent chosen by select_safe_temp_parent for not being
# group- or world-writable.  On top of that the leaf is verified BY IDENTITY: device and
# inode are read before the open, compared against what the descriptor actually refers to,
# and compared again after flock returns.  A mismatch at any of the three points means
# somebody unlinked and recreated the name, which is the precise mechanism by which two
# contenders end up flocking two different inodes and both believing they hold the lock.
#
# MISSING flock IS FATAL, not advisory.  A warning plus a recorded advisory would let an
# acceptance run on a host without flock reach a verdict while having proved
# nothing about exclusivity, and an advisory that reaches acceptance is indistinguishable from
# no check at all: the only two honest outcomes are "the lock is held" and "this run stops".
# ------------------------------------------------------------------------------------
TREE_LOCK_FD=''            # open descriptor while held; empty when not
TREE_LOCK_PATH=''          # the lock leaf
TREE_LOCK_IDENTITY=''      # "<device>:<inode>" the descriptor refers to
TREE_LOCK_PURPOSE=''       # what the first acquisition said it was for, for the log

# ------------------------------------------------------------------------------------
# Output helpers.  A single prefix makes this script's lines distinguishable from lcov's
# and GoogleTest's in a shared log.  Diagnostics go to stderr so that stdout stays a
# clean, pipeable report.
# ------------------------------------------------------------------------------------
log()  { printf '[run_coverage] %s\n' "$*"; }
warn() { printf '[run_coverage] WARNING: %s\n' "$*" >&2; }
die()  { printf '[run_coverage] ERROR: %s\n' "$*" >&2; exit 1; }
rule() { printf '%s\n' '--------------------------------------------------------------------------------'; }

# ------------------------------------------------------------------------------------
# RENDERING AN UNTRUSTED VALUE INTO A DIAGNOSTIC.  CWE-117, closed by construction.
#
# THE DEFECT THIS REMOVES.  Every diagnostic in this file used to interpolate caller-supplied
# text verbatim.  A value carrying a newline therefore did not appear inside a message -- it
# ENDED that message and began a new line of its own, and a line beginning "::error::" is a
# GitHub Actions workflow command.  Measured before the fix: a newline-bearing value produced a
# standalone forged "::error::" annotation in the job log, and a five-thousand-character value
# produced more than ten kilobytes of diagnostics, burying the real failure.
#
# THE CONTRACT, in the order the steps are applied, because the order is what makes the
# escaping unambiguous and reversible:
#
#   (a) a literal backslash becomes \\ FIRST, so that every escape introduced below is
#       distinguishable from the same characters occurring literally in the value;
#   (b) 0x0A becomes \n, 0x0D becomes \r, 0x09 becomes \t, and EVERY OTHER byte outside
#       printable ASCII 0x20..0x7E becomes \xNN in lower-case hex.  The classification is by
#       byte value under LC_ALL=C, so no locale can reinterpret a byte as printable and no
#       multi-byte sequence can hide a control character inside itself;
#   (c) the rendering is truncated at 200 characters and "...[truncated, N bytes total]" is
#       appended when it truncates, N being the value's own length in bytes.  A caller cannot
#       drown a log, and the message still says how much was withheld;
#   (d) it is applied to the VALUE and never to the surrounding message, and the result is
#       always returned delimited -- [ ... ] here -- so an empty value is visible as [] rather
#       than as a gap in a sentence;
#   (e) it never begins a diagnostic line.  Every message keeps this script's own "[name]"
#       prefix in front of it, and the rendering itself begins with '['.  Together with (b),
#       which leaves no raw newline anywhere in the output, that makes a forged standalone
#       ::error::, ::warning:: or ::notice:: line UNREACHABLE BY CONSTRUCTION rather than
#       unlikely.
#
# THIS IS ONE OF FIVE COPIES OF ONE CONTRACT.  The five must not diverge; a change to any of
# them is a change to all five.  The others are:
#
#   * hdmicec/tests/L1Tests/run_coverage.sh                      -- render_untrusted()
#   * hdmicec/.github/workflows/aidl-path-tests-rootfs.sh        -- render_untrusted()
#   * hdmicec/tests/L1Tests/test_main.cpp                        -- renderUntrustedValue()
#   * hdmicec/tests/L2Tests/test_main.cpp                        -- renderUntrustedValue()
#   * hdmicec/mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp  -- renderUntrustedValue()
#
# They are five file-local copies rather than one shared helper deliberately: two are shell
# and three are C++, they live in three build targets and one non-built script, and a shared
# header would add a build-system edge for a twenty-line function.  The cost of the choice is
# this cross-reference, which is why every copy carries it.
#
# NO EXTERNAL COMMAND IS RUN.  Everything below is a bash builtin, so this is usable from any
# diagnostic including ones that fire before the tooling pre-flight has established that
# anything external exists.
# ------------------------------------------------------------------------------------
render_untrusted() { # $1=the untrusted value -> prints [escaped, bounded] on stdout
    # LC_ALL and LC_CTYPE are local, and bash re-reads the locale when either is assigned, so
    # for the duration of this function ${#value} counts BYTES and ${value:i:1} yields ONE
    # byte.  Both revert on return.
    local LC_ALL=C LC_CTYPE=C
    local value="${1-}"
    local total=${#value}
    local limit=200
    local out='' rendered=0 index=0 byte piece ord

    while [ "$index" -lt "$total" ]; do
        byte="${value:index:1}"
        # The numeric value of the byte, via printf's "'c" form.  Under LC_ALL=C this is the
        # byte itself, and it is obtained before any classification so that the decision below
        # is made on the number and never on a pattern match that a locale could influence.
        printf -v ord '%d' "'$byte"
        if   [ "$ord" -eq 92 ]; then piece='\\'
        elif [ "$ord" -eq 10 ]; then piece='\n'
        elif [ "$ord" -eq 13 ]; then piece='\r'
        elif [ "$ord" -eq  9 ]; then piece='\t'
        elif [ "$ord" -ge 32 ] && [ "$ord" -le 126 ]; then piece="$byte"
        else printf -v piece '\\x%02x' "$ord"
        fi

        if [ $(( rendered + ${#piece} )) -gt "$limit" ]; then
            printf '[%s...[truncated, %d bytes total]]' "$out" "$total"
            return 0
        fi
        out="${out}${piece}"
        rendered=$(( rendered + ${#piece} ))
        index=$(( index + 1 ))
    done

    printf '[%s]' "$out"
}

# ------------------------------------------------------------------------------------
# PATH SAFETY -- the ancestry of every path this script writes to.
#
# A CALLER-NAMED artifact root is PREDICTABLE by construction -- it is chosen in advance and
# usually appears in a CI file -- and the artifact names underneath any root are fixed, so a
# reader knows where to look and CI can collect them.  A predictable path under a
# world-writable directory is an invitation: anything that can create entries there can
# create the root FIRST -- as a symlink to a directory it does not own, or as a directory it
# does own -- and then every trace, log and HTML page this script writes lands somewhere it
# chose, with this script's privileges.  On a CI runner that is a write into another job's
# workspace; run under sudo, it is a write anywhere.
#
# The root this script MINTS is not predictable (mktemp -d), and select_safe_temp_parent keeps
# it out of a world-writable parent as well.  Both roots are held to the
# checks below regardless, because "unpredictable" and "unreachable" are different properties
# and only the second one survives another account being able to write the parent.
#
# Checking only the leaf does not close that, and must not be reduced to it: the leaf can be
# perfectly ordinary while its PARENT is the substitution.  So the whole chain
# from / down is checked, and every existing component must satisfy all three of:
#
#   * not a symbolic link.  A link is exactly the substitution being defended against, and
#     resolving it first (`pwd -P`, `mkdir -p`) would validate the target while the write
#     still goes through the link -- so the link is rejected instead of followed.
#   * owned by this effective user, or by root.  Root ownership is accepted because /,
#     /tmp and /var are legitimately root's; anyone ELSE owning a component means someone
#     else can rename or replace it underneath this run.
#   * not group- or world-writable unless sticky.  1777 on /tmp is the standard and is
#     safe for entries this script creates, because the sticky bit stops a non-owner
#     removing or renaming them.  The same permissions WITHOUT the sticky bit mean any
#     local account can swap a component out mid-run.
#
# Directories this script creates are created 0700, one component at a time, so an
# intermediate never exists with permissive modes even briefly.  And because a check is
# only true at the moment it runs, the whole set is REPEATED immediately before each
# destructive step (see assert_output_dir_still_safe) rather than once at startup.
# ------------------------------------------------------------------------------------
EUID_VALUE="$(id -u)"
readonly EUID_VALUE

# ------------------------------------------------------------------------------------
# LEXICAL CANONICALISATION -- collapse '.', '..' and doubled slashes, and NOTHING ELSE.
#
# WHY IT IS NEEDED.  Absolutising a caller-supplied path is not the same as canonicalising
# it.  `--output-dir "$SANDBOX/ok/../../../../etc/name"` is already absolute, so an ancestry
# walk handed the raw value validates `$SANDBOX/ok/..`,
# `$SANDBOX/ok/../..` and so on as ordinary existing directories owned by root, creates
# every missing component in turn, and writes the artifacts into /etc/name.  Every
# individual check holds; the PATH has simply left the tree the caller appears to name.
# Collapsing the components first means the checks below, the location guard above and the
# messages a reader sees all describe the one directory that will actually be written to.
#
# WHY IT IS LEXICAL AND NOT `realpath`.  `realpath` (without --no-symlinks) RESOLVES
# symbolic links, which would quietly defeat this script's strongest guarantee: that it
# refuses to write THROUGH a link rather than following it (assert_safe_ancestry,
# create_safe_dir).  A resolved path has no links left to refuse, so the refusal would
# never fire again and a substituted component would be validated at its target instead.
# Collapsing textually keeps every link visible to those checks -- and needs no external
# tool, which also keeps the "no new dependency" clause intact.
#
# `local -` scopes the option change to this function, so `set -f` (no pathname expansion
# while the path is split on '/') cannot leak into the caller: without it a component
# containing '*' would be glob-expanded during the split.
# ------------------------------------------------------------------------------------
canonicalise_path_lexically() { # $1=absolute path -> canonical path on stdout
    local input="$1" out='' component saved_ifs
    local -
    set -f

    saved_ifs="$IFS"
    IFS='/'
    # Deliberate word splitting on '/' to walk the components in order.
    # shellcheck disable=SC2086
    set -- ${input#/}
    IFS="$saved_ifs"

    for component in "$@"; do
        case "$component" in
            ''|.)  : ;;                        # '' comes from a doubled slash; '.' is a no-op
            ..)    out="${out%/*}" ;;          # one level up, textually -- never via the filesystem
            *)     out="$out/$component" ;;
        esac
    done
    printf '%s\n' "${out:-/}"
}

# ------------------------------------------------------------------------------------
# WHERE AN ARTIFACT ROOT MAY NOT BE.  Both sibling plugin runners refuse a near-root
# artifact root outright ("implausibly short"), and this one refuses it identically: without
# that refusal `--output-dir /etc` is accepted and creates /etc/.run.lock, and a
# '..'-collapsed value lands under /etc just as easily.  Three sibling scripts written
# together must not disagree about that, so the same refusal lives here, with the
# system-location half made explicit rather than left to a length test.
#
# The list is deliberately SHORT and holds only trees the operating system owns.  /tmp,
# /var/tmp, /run/user/<uid>, /opt, /home and /root are all legitimate destinations and are
# NOT refused -- and neither is a path inside the checkout, because the header documents
# `--output-dir .` from the superproject root as the way to reproduce CI's layout byte for
# byte.  A guard that broke a documented usage would be a worse defect than the one it
# closes.
# ------------------------------------------------------------------------------------
readonly PROTECTED_SYSTEM_ROOTS=(
    /bin /boot /dev /etc /lib /lib32 /lib64 /libx32 /proc /run /sbin /sys /usr /var
)

assert_artifact_location_plausible() { # $1=canonical absolute path  $2=how it was chosen
    local path="$1" origin="$2" root

    case "$path" in
        /)  die "the artifact directory must not be '/' ($origin).  This script creates and
       overwrites fixed artifact names underneath it and takes a lock file there; the
       filesystem root is not a place to do that." ;;
        /*) : ;;
        *)  die "internal error: assert_artifact_location_plausible needs an absolute path;
       got '$path' ($origin)." ;;
    esac

    # Same test, same wording, as the two plugin runners: four characters or fewer cannot be
    # anything but a near-root directory.
    [ "${#path}" -gt 4 ] || die "the artifact directory '$path' is implausibly short ($origin).
       Fixed artifact names are created and overwritten underneath it, so a near-root path is
       refused.  Give a path that is unmistakably yours, for example
       \"\$HOME/.cache/hdmicec-l1-coverage\", or drop --output-dir entirely and let this
       script mint an unpredictable mode-0700 root with mktemp -d under a parent it has
       proved safe."

    # Checked BEFORE the protected-root loop, because /var/tmp and /run/user/<uid> are
    # ordinary per-user scratch directories that happen to live under a protected root.
    case "$path" in
        /var/tmp|/var/tmp/*|/run/user/*) return 0 ;;
    esac

    for root in "${PROTECTED_SYSTEM_ROOTS[@]}"; do
        case "$path" in
            "$root"|"$root"/*)
                die "refusing to write coverage artifacts to
           $path
       ($origin), because it is $root or lies underneath it -- a directory the operating
       system owns.  This script creates a directory there if it is missing, takes
       .run.lock inside it, and overwrites fixed artifact names on every run; none of that
       belongs in a system tree, and a value that reaches one is nearly always a '..' that
       collapsed out of the intended path or a mistyped root.
       Use \"\$HOME/.cache/hdmicec-l1-coverage\", a directory inside your own tree, or
       drop --output-dir / COVERAGE_OUTPUT_DIR and let this script mint an unpredictable
       mode-0700 root with mktemp -d under a parent it has proved safe." ;;
        esac
    done
    return 0
}

# "<uid> <octal mode> <type>" for an existing path, empty for one that does not exist.
# lstat semantics (stat does not follow the final link), so a symlink reports as such
# rather than as whatever it points at.
path_metadata() { # $1=path
    # Checked here rather than only in require_tools: the first ancestry validation happens
    # before require_tools' message would be reached in some orderings, and a missing stat
    # would otherwise degrade every check below into a silent "could not stat" failure.
    [ -n "${STAT_BIN:-}" ] || die "stat was not found on PATH, so the ownership and permissions of
       the paths this script writes to cannot be checked.  Refusing to write anything.  stat
       ships with coreutils."
    "$STAT_BIN" -c '%u %a %F' -- "$1" 2>/dev/null || true
}

# "<device>:<inode>" for an existing path, empty for one that does not exist.  lstat
# semantics, so a symlink reports ITS OWN identity rather than its target's.
#
# WHY IDENTITY AND NOT JUST THE NAME.  A name is a lookup, not a thing: the file a name
# resolved to a microsecond ago can be unlinked and a different file put in its place under
# the same name, and every subsequent open of that name reaches the new file while an already
# open descriptor still refers to the old one.  For a lock that is not a subtlety but the whole
# failure: two contenders that flock two different inodes both believe they hold it.  Device
# and inode together are the only identity a filesystem offers, so they are what gets compared.
path_identity() { # $1=path
    [ -n "${STAT_BIN:-}" ] || die "stat was not found on PATH, so this script cannot verify the
       identity of the files it locks and writes.  Refusing to continue.  stat ships with
       coreutils."
    "$STAT_BIN" -c '%d:%i' -- "$1" 2>/dev/null || true
}

# "<device>:<inode>" of the file an OPEN DESCRIPTOR refers to -- which is the question that
# matters after an open, because the descriptor cannot be redirected by anything happening to
# the name afterwards.
#
# /proc/<pid>/fd/<n> is a symlink to the open file, so `stat -L` on it reports the target.
# `$$` rather than `self` because the read happens in a command-substitution SUBSHELL: the
# descriptor is inherited (bash does not set close-on-exec on `exec {var}>`, measured on this
# host), so both spellings resolve, but the owning shell's pid is the one that is true by
# construction rather than by inheritance.  A fallback to `self` covers a /proc that hides
# other pids; if neither can be read this FAILS CLOSED, because an unverifiable lock is
# indistinguishable from an unheld one.
open_fd_identity() { # $1=file descriptor number
    local fd="$1" ident=''
    [ -n "${STAT_BIN:-}" ] || die "stat was not found on PATH, so this script cannot verify the
       identity of the descriptors it holds.  Refusing to continue."
    ident="$("$STAT_BIN" -L -c '%d:%i' "/proc/$$/fd/$fd" 2>/dev/null || true)"
    if [ -z "$ident" ]; then
        ident="$("$STAT_BIN" -L -c '%d:%i' "/proc/self/fd/$fd" 2>/dev/null || true)"
    fi
    [ -n "$ident" ] || die "could not read back which file descriptor $fd refers to via /proc.
       This script verifies every lock it takes by device and inode, and a lock it cannot
       verify is one it cannot claim to hold.  Mount /proc, or run this script somewhere it
       is visible."
    printf '%s\n' "$ident"
}

# ------------------------------------------------------------------------------------
# PROBE MODE FOR THE PATH CHECKS.
#
# The ancestry checks below are written to be FATAL: they exist to stop a write, so ending the
# run is the correct response to an unsafe path.  Choosing BETWEEN candidate parents needs the
# same rules with a different verdict -- "this one is not safe, try the next" -- and the one
# thing that must not happen is a second, subtly different copy of the rules for the choosing.
#
# So the rules stay in one place and the VERDICT becomes a mode.  While PATH_CHECK_PROBE is 1,
# a violation records its reason and returns non-zero instead of exiting; every other caller
# leaves the flag at 0 and gets the fatal verdict.  `die` never returns, so
# `path_check_reject ... || return 1` is a no-op in fatal mode rather than a second code path.
# ------------------------------------------------------------------------------------
PATH_CHECK_PROBE=0        # 1 only inside probe_safe_ancestry
PATH_CHECK_REASON=''      # why the last probe rejected its candidate

path_check_reject() { # $1=message
    if [ "$PATH_CHECK_PROBE" -eq 1 ]; then
        PATH_CHECK_REASON="$1"
        return 1
    fi
    die "$1"
}

# One component of a chain: must exist, be a directory, be ours or root's, and not be
# writable by anyone else unless the sticky bit protects it.
# $3 is how the path below this component was chosen, and it changes only the verdict for
# the "writable by others without a sticky bit" case:
#   named  -- a caller chose the name, so it is predictable: that condition is FATAL.
#   minted -- this script created it with mktemp -d, so it could not be pre-created: the
#             condition is reported and the re-validation before each destructive step is
#             what carries the guarantee.
assert_component_safe() { # $1=path  $2=context for the message  $3=named|minted
    local comp="$1" context="$2" choice="${3:-named}" meta uid rest mode kind numeric_mode

    meta="$(path_metadata "$comp")"
    [ -n "$meta" ] || path_check_reject "could not stat $comp while validating the ancestry of
       $context
       Refusing to write below a path whose ownership and permissions cannot be read." || return 1

    uid="${meta%% *}"
    rest="${meta#* }"
    mode="${rest%% *}"
    kind="${rest#* }"

    [ "$kind" = "directory" ] || path_check_reject "$comp is a $kind, not a directory, while validating
       the ancestry of
       $context
       Choose an --output-dir whose every parent is a real directory." || return 1

    if [ "$uid" != "$EUID_VALUE" ] && [ "$uid" != "0" ]; then
        path_check_reject "$comp is owned by uid $uid, which is neither this user ($EUID_VALUE) nor root,
       while validating the ancestry of
       $context
       Another user who owns a parent directory can replace it underneath this run, so the
       artifacts would be written somewhere they chose.  Use --output-dir (or
       COVERAGE_OUTPUT_DIR) to name a location you own." || return 1
    fi

    numeric_mode="$(( 8#$mode ))"
    if [ "$(( numeric_mode & 0022 ))" -ne 0 ] && [ "$(( numeric_mode & 01000 ))" -eq 0 ]; then
        # Writable by others, with no sticky bit to stop them renaming or removing what is
        # inside it.  BOTH VERDICTS ARE FATAL, the `minted` arm included, and that is the
        # point of this block rather than an incidental strictness.
        #
        #   * A CALLER-CHOSEN path is guessable by construction -- it was chosen in advance
        #     and often appears in a CI file -- so pre-creating the name is enough to collect
        #     or redirect the evidence.
        #   * A path this script MINTED with mktemp -d cannot be pre-created, which covers only
        #     half the attack: the name is unguessable, but another local account able to write
        #     the PARENT can still remove the minted directory mid-run and put its own there
        #     under the same name, and it can do it in the window between any two of this
        #     script's steps.
        #
        # A warning in place of that refusal is not theoretical either.  Measured on this host:
        # /tmp is mode 2777 -- world-writable, setgid, no sticky bit -- so a warning fires on
        # every run, twice, while the trace, the lock and the Makefile snapshot are written
        # below /tmp regardless.  A warning nobody can act on is a warning that trains readers
        # to ignore it.
        #
        # WHAT MAKES FAILING CLOSED SAFE HERE, and why this is not simply the script refusing
        # to run on a normal container: nothing this script mints lands under an unsafe parent.
        # select_safe_temp_parent chooses the parent -- TMPDIR, then
        # XDG_RUNTIME_DIR, then HOME -- and holds each candidate to these same rules at
        # `named` strictness before choosing it, so on this host the lock, the snapshot,
        # lcov's HOME and the minted artifact root all land under $HOME (0700) and this arm
        # is never reached.  It is reached only if a parent that measured safe at selection
        # time is loosened during the run, which is a fact worth stopping for.  Re-validation
        # before every destructive step (assert_output_dir_still_safe) is kept as well: the
        # two checks answer different questions and neither replaces the other.
        path_check_reject "$comp has mode $mode -- writable by group or world, without the sticky bit --
       while validating the ancestry of
       $context
       (that path was chosen: $choice)
       Any local account able to write $comp can remove or replace what is below it, and this
       run's trace, lock and Makefile snapshot are what it would be replacing.  A path this
       script minted has an unguessable NAME, which is not the same as an unreachable one.
       Either of these makes it work:
           chmod +t $comp   (the sticky bit a conventional /tmp has), or
           TMPDIR=\"\$HOME/.cache/hdmicec-coverage\"   (create it mode 0700 first)
       For a path you named yourself, tighten its mode or drop --output-dir /
       COVERAGE_OUTPUT_DIR and let this script mint a mode-0700 root under a safe parent." \
            || return 1
    fi
}

# The whole chain from / down to $1.  $1 itself need not exist; the walk stops at the
# first component that does not, because nothing below it exists either.
assert_safe_ancestry() { # $1=absolute path  $2=named|minted (see assert_component_safe)
    local target="$1" choice="${2:-named}" walked='' component

    case "$target" in
        /*) ;;
        *)  die "internal error: assert_safe_ancestry needs an absolute path; got: $target" ;;
    esac

    assert_component_safe "/" "$target" "$choice" || return 1

    local saved_ifs="$IFS"
    IFS='/'
    # Deliberate word splitting on '/' to walk the components in order.
    # shellcheck disable=SC2086
    set -- ${target#/}
    IFS="$saved_ifs"

    for component in "$@"; do
        [ -n "$component" ] || continue
        walked="$walked/$component"
        # Checked BEFORE -e, because -e is false for a dangling symlink and a dangling
        # symlink is precisely how a path gets created somewhere unintended.
        if [ -L "$walked" ]; then
            path_check_reject "$walked is a symbolic link, and this script will not write through one.
       It is a component of
       $target
       Remove it, or use --output-dir (or COVERAGE_OUTPUT_DIR) to name a real directory." || return 1
        fi
        [ -e "$walked" ] || return 0
        assert_component_safe "$walked" "$target" "$choice" || return 1
    done
    return 0
}

# The same rules, asked as a question.  Returns 0 when $1's whole existing ancestry is safe
# under the $2 strictness, non-zero with the reason in PATH_CHECK_REASON when it is not.
# Never exits, and always restores the flag -- including on the failing path, which is why
# the return value is captured rather than the call being the function's last command.
probe_safe_ancestry() { # $1=absolute path  $2=named|minted
    local rc=0
    PATH_CHECK_PROBE=1
    PATH_CHECK_REASON=''
    assert_safe_ancestry "$1" "${2:-named}" || rc=1
    PATH_CHECK_PROBE=0
    return "$rc"
}

# ------------------------------------------------------------------------------------
# WHERE THIS RUN'S PRIVATE DIRECTORIES GO, AND WHY IT IS NOT "${TMPDIR:-/tmp}".
#
# Every private path this script needs -- the tree lock, the Makefile snapshot, lcov's
# throwaway HOME, the minted artifact root -- has to sit where no other account can reach it.
# "${TMPDIR:-/tmp}" does not establish that: it falls back to /tmp unconditionally, and an
# unsafe /tmp can then only be reported as a warning, because failing closed on it would make
# the script unrunnable on any host whose /tmp is group- or world-writable without the sticky
# bit.
#
# MEASURED ON THIS HOST: /tmp is mode 2777 -- world-writable, setgid, and NO sticky bit --
# TMPDIR and XDG_RUNTIME_DIR are both unset, and $HOME is /root at mode 0700.  So that warning
# is not theoretical: an unconditional /tmp puts this run's evidence and its lock below a
# directory any local account can rename or remove entries in, and says so twice in the log
# while doing it.
#
# So the parent is CHOSEN rather than accepted and complained about.  In order:
#   TMPDIR            -- the caller's explicit choice, honoured first when it is safe
#   XDG_RUNTIME_DIR   -- per-user, mode 0700 by specification, the right answer on a systemd
#                        host and empty on a container like this one
#   HOME              -- always private in practice and the last resort that keeps this
#                        script runnable on a host whose /tmp is unsafe
# Each candidate is measured with the SAME rules that guard the artifact directory, at
# `named` strictness -- these names are all predictable, so group/world-writable without a
# sticky bit disqualifies them, which is exactly what excludes this host's /tmp.
#
# NO ESCAPE HATCH IS PROVIDED, deliberately: no --allow-unsafe, no environment override.  An
# override would be used by the one caller whose host is misconfigured, which is the caller
# whose evidence is at risk.  If none of the three is safe the run stops with the two
# one-line fixes that would make it work.
# ------------------------------------------------------------------------------------
RUNNER_TEMP_PARENT=''          # memoised: chosen once, reported once, reused everywhere
RUNNER_TEMP_PARENT_SOURCE=''   # which of the three it came from, for the log

# Try one candidate.  0 and RUNNER_TEMP_PARENT set when it is usable.
consider_temp_parent() { # $1=variable name it came from  $2=its value
    local name="$1" candidate="$2"

    [ -n "$candidate" ] || return 1
    case "$candidate" in
        /*) ;;
        *)  warn "$name is not an absolute path and cannot be checked safely; skipping it: $candidate"
            return 1 ;;
    esac
    candidate="$(canonicalise_path_lexically "$candidate")"
    [ -d "$candidate" ] || return 1
    if ! probe_safe_ancestry "$candidate" named; then
        warn "$name ($candidate) is not a safe parent for this run's private directories:"
        printf '%s\n' "$PATH_CHECK_REASON" | sed 's/^/[run_coverage]     /' >&2
        return 1
    fi
    RUNNER_TEMP_PARENT="$candidate"
    RUNNER_TEMP_PARENT_SOURCE="$name"
    return 0
}

# Chooses once and records the answer in RUNNER_TEMP_PARENT / RUNNER_TEMP_PARENT_SOURCE.
#
# IT SETS GLOBALS RATHER THAN PRINTING THE PATH, and that is not a style preference.  A
# printing form has to be read with a command substitution, which runs it in a SUBSHELL: the
# memo assignment dies with the subshell, so the selection is re-derived on every call, the
# announcement prints once per call, and RUNNER_TEMP_PARENT_SOURCE -- which one caller puts in
# a diagnostic -- stays permanently empty in the caller.  Measured in that shape: three
# identical "private directories for this run go under" lines in a single measure-only run.
select_safe_temp_parent() { # -> sets RUNNER_TEMP_PARENT and RUNNER_TEMP_PARENT_SOURCE
    [ -z "$RUNNER_TEMP_PARENT" ] || return 0

    consider_temp_parent TMPDIR          "${TMPDIR:-}"          \
        || consider_temp_parent XDG_RUNTIME_DIR "${XDG_RUNTIME_DIR:-}" \
        || consider_temp_parent HOME     "${HOME:-}"            \
        || die "none of TMPDIR, XDG_RUNTIME_DIR or HOME names a directory this run can safely
       put its lock, its Makefile snapshot and its private lcov HOME under.  The reasons are
       above, one per candidate.
       This script will not fall back to a world-writable /tmp: the Makefile snapshot is the
       only way back from a build that overwrites six tracked files, and the lock is the only
       thing that stops two runs interleaving those restores.
       Either of these makes it work:
           TMPDIR=\"\$HOME/.cache/hdmicec-coverage\" (create it mode 0700 first), or
           chmod +t /tmp   (the sticky bit a conventional /tmp has)"

    log "private directories for this run go under $RUNNER_TEMP_PARENT (from \$$RUNNER_TEMP_PARENT_SOURCE)"
}

# ------------------------------------------------------------------------------------
# A VERIFIED PRIVATE PER-USER DIRECTORY FOR THIS RUN'S LOCKS.
#
# The lock leaf cannot be minted per run -- two runs that must exclude each other have to
# agree on the name, so it is derived from the tree path and it PERSISTS between runs.  That
# makes it predictable, and a predictable name in a shared directory is the inode-substitution
# hole: a foreign-owned regular file sitting at that name is lockable by anybody, and a
# contender that unlinks and recreates it splits the two runs across two inodes so that both
# hold "the" lock.
#
# The name stays predictable -- it has to -- but it lives one level down, inside a
# directory that is proved to be ours and unreachable by anyone else.  `mkdir` is the
# primitive that does the proving: it either creates the directory or fails, so an existing
# entry of the wrong kind or ownership cannot be silently adopted, and assert_private_dir then
# re-reads it with lstat and demands owner == this user and mode & 0077 == 0.
# ------------------------------------------------------------------------------------
RUNNER_LOCK_DIR=''

# Sets RUNNER_LOCK_DIR, for the same subshell reason select_safe_temp_parent sets a global:
# a printing form memoises nothing when it is read with a command substitution, so the
# directory is re-created and re-validated on every call.
runner_lock_dir() { # -> sets RUNNER_LOCK_DIR
    [ -z "$RUNNER_LOCK_DIR" ] || return 0

    local leaf
    select_safe_temp_parent
    leaf="$RUNNER_TEMP_PARENT/hdmicec-l1-locks-$EUID_VALUE"

    if ! mkdir -m 0700 -- "$leaf" 2>/dev/null; then
        [ -d "$leaf" ] || die "could not create the private lock directory $leaf, and nothing
       usable is there already.  This run needs it before it may build, snapshot, run or
       capture anything."
    fi
    [ ! -L "$leaf" ] || die "the private lock directory is a symbolic link: $leaf
       Remove it.  This script will not place a lock through a link it does not control."
    # Owner, mode, kind and not-a-symlink, re-read with lstat after the create rather than
    # assumed from it.
    assert_private_dir "$leaf"

    RUNNER_LOCK_DIR="$leaf"
}

# Create $1 and any missing parent, 0700 and one component at a time, after proving the
# existing part of the chain is safe.  mkdir -p -m applies the mode to the FINAL component
# only, which would leave intermediates at the umask default, so the loop is not redundant.
# Only ever used for a CALLER-NAMED path, hence the unconditional "named" strictness.
create_safe_dir() { # $1=absolute path
    local target="$1" walked='' component

    assert_safe_ancestry "$target" named

    local saved_ifs="$IFS"
    IFS='/'
    # shellcheck disable=SC2086
    set -- ${target#/}
    IFS="$saved_ifs"

    for component in "$@"; do
        [ -n "$component" ] || continue
        walked="$walked/$component"
        [ ! -L "$walked" ] || die "$walked became a symbolic link while the output directory
       was being created.  Refusing to continue."
        if [ ! -e "$walked" ]; then
            mkdir -m 0700 -- "$walked" 2>/dev/null || {
                # A concurrent run of this same script legitimately creates the same
                # component; losing that race is fine as long as what won is safe.
                [ -d "$walked" ] || die "could not create $walked while preparing
       $target"
            }
        fi
        assert_component_safe "$walked" "$target" named
    done
    return 0
}

# Stricter than assert_component_safe, for a directory this script created for its own
# private use: nobody else may write to it at all, sticky bit or not.
assert_private_dir() { # $1=path
    local path="$1" meta uid rest mode kind numeric_mode

    [ ! -L "$path" ] || die "expected a private directory but found a symbolic link: $path"
    meta="$(path_metadata "$path")"
    [ -n "$meta" ] || die "expected a private directory but could not stat it: $path"
    uid="${meta%% *}"
    rest="${meta#* }"
    mode="${rest%% *}"
    kind="${rest#* }"
    [ "$kind" = "directory" ] || die "expected a private directory but found a $kind: $path"
    [ "$uid" = "$EUID_VALUE" ] || die "a directory this script created is owned by uid $uid
       rather than by this user ($EUID_VALUE): $path"
    numeric_mode="$(( 8#$mode ))"
    [ "$(( numeric_mode & 0077 ))" -eq 0 ] || die "a directory this script created for its own
       use has mode $mode, which lets other accounts read or write it: $path"
}

# ------------------------------------------------------------------------------------
# ONE PRIVACY POSTURE FOR BOTH BRANCHES OF THE ARTIFACT DIRECTORY.
#
# The minted branch ends at `chmod 700` + assert_private_dir, so a directory this script
# created is owner-only.  A CALLER-SUPPLIED directory accepted as it is found gets neither: a
# pre-existing mode-0755 directory stays 0755 and every artifact written into it is reachable
# by any local account that can traverse it, which is all it takes for an unprivileged user
# to read, append to and forge the gate's own input file.
# The caller-named branch is the predictable one, so if anything it warrants the stricter
# treatment, not the weaker -- hence both branches end in the same posture.
#
# TIGHTENED RATHER THAN REFUSED, and only when the mode actually grants something away:
# the directory has already been proved to be OWNED by this user (assert_owned_and_private),
# so narrowing it is a change to the caller's own directory, it is announced when it
# happens, and it costs the caller nothing this pipeline needs.  Refusing instead would
# turn an ordinary `--output-dir ~/cov` into a hard failure over a bit this script can
# simply fix.  A chmod that does not take IS fatal -- proceeding would write evidence
# somewhere it can be replaced.
# ------------------------------------------------------------------------------------
restrict_artifact_dir_to_owner() { # $1=directory this run writes its artifacts into
    local dir="$1" mode
    mode="$(stat -c '%a' -- "$dir" 2>/dev/null)" \
        || die "cannot stat the artifact directory to check its mode: $dir"
    if [ "$(( 8#$mode & 0077 ))" -ne 0 ]; then
        chmod 700 -- "$dir" \
            || die "the artifact directory $dir is mode $mode -- readable or writable by other
       accounts -- and could not be tightened to 0700.  Everything written there is the
       evidence this run is judged on, and another account able to write it can replace a
       trace between the capture and the gate.  Fix its permissions, or point
       --output-dir / COVERAGE_OUTPUT_DIR at a directory you own."
        warn "tightened the artifact directory from mode $mode to 0700: $dir"
        warn "  Its contents are the trace, the table and the log the gate and the"
        warn "  traceability report rest on, so no other account may read or replace them."
    fi
    # The same assertion the minted branch makes, so both branches end in the same state
    # rather than in two states that merely look similar.
    assert_private_dir "$dir"
}

# ------------------------------------------------------------------------------------
# RE-VALIDATE CUSTODY IMMEDIATELY BEFORE EVERY DESTRUCTIVE OR PUBLISHING STEP.
#
# A path that was safe when the run started is not necessarily safe thirty seconds later, and
# a check performed once at startup answers a question about the past.  Every call site that
# writes, removes or publishes goes through here first, so the guarantee holds at the moment
# of the write.
#
# FOUR PROPERTIES ARE CHECKED, and the fourth is the one a name-based check cannot give:
#   * the whole ancestry, walked from / down, under the strictness the directory was chosen
#     with -- so a loosened parent or a symlink that appeared anywhere on the chain is caught;
#   * ownership and mode of the directory ITSELF, via assert_private_dir, which is stricter
#     than the ancestry component check: the walk refuses group/world WRITE without a sticky
#     bit, whereas a directory holding this run's evidence must grant no group or other access
#     at all, read included;
#   * the artifact leaf is not a symlink that appeared during the run;
#   * DEVICE AND INODE of the directory, compared against what was recorded when it was
#     resolved.  This is what catches a substitution that reproduces every other property: a
#     replacement directory can be made to have the same name, the same owner, the same mode
#     and the same ancestry, and it cannot be made to have the same inode.  A mismatch is
#     fatal, never a warning -- the artifacts in the directory this run believes it is writing
#     would then be a mixture of two directories' contents.
#
# The run lock's own identity is checked too, for the same reason and separately: the lock is
# what makes this run the only writer, so a lock file that is no longer the file this run
# locked means the exclusion no longer holds even if the directory is unchanged.
# ------------------------------------------------------------------------------------
OUTPUT_DIR_IDENTITY=''    # "<device>:<inode>" of OUTPUT_DIR, recorded once it is resolved

assert_output_dir_still_safe() { # $1=artifact path about to be written (optional)
    local artifact="${1:-}"

    [ -n "${OUTPUT_DIR:-}" ] || die "internal error: assert_output_dir_still_safe called before
       the output directory was resolved"
    assert_safe_ancestry "$OUTPUT_DIR" "$( [ "${OUTPUT_DIR_EXPLICIT:-1}" -eq 1 ] && printf 'named' || printf 'minted' )"
    [ -d "$OUTPUT_DIR" ] || die "the output directory disappeared during the run: $OUTPUT_DIR"
    # Owner and mode, stricter than the ancestry walk: no group or other access whatsoever.
    assert_private_dir "$OUTPUT_DIR"

    # Identity.  Skipped only before prepare_output_dir has recorded it, which is the one
    # window in which OUTPUT_DIR_IDENTITY is legitimately empty.
    if [ -n "$OUTPUT_DIR_IDENTITY" ]; then
        local dir_now
        dir_now="$(path_identity "$OUTPUT_DIR")"
        [ "$dir_now" = "$OUTPUT_DIR_IDENTITY" ] || die "the output directory is no longer the
       directory this run resolved: $OUTPUT_DIR
       resolved $OUTPUT_DIR_IDENTITY, the name now resolves to ${dir_now:-nothing} (device:inode).
       The name was re-pointed at a different directory underneath this run, so the artifacts
       already written and the one about to be written would belong to two different places.
       Refusing to write ${artifact:-into it}."
    fi

    if [ -n "$artifact" ]; then
        [ ! -L "$artifact" ] || die "refusing to write through a symbolic link that appeared
       during the run: $artifact"
    fi

    # The run lock lives inside this directory, so if the directory this run is writing into
    # is still the one it locked, the lock file's identity is unchanged too.  Checking it is
    # how a directory that was swapped for another one holding a same-named lock file is
    # caught: the ancestry and mode of the replacement can be made to look identical, its
    # inode cannot.  Skipped before the lock is taken, which is the only window in which
    # LOCK_IDENTITY is legitimately empty.
    if [ -n "$LOCK_IDENTITY" ]; then
        local lock_now
        lock_now="$(path_identity "$OUTPUT_DIR/.run.lock")"
        [ "$lock_now" = "$LOCK_IDENTITY" ] || die "the run lock inside the output directory is
       no longer the file this run locked: $OUTPUT_DIR/.run.lock
       locked $LOCK_IDENTITY, the name now resolves to ${lock_now:-nothing} (device:inode).
       Either the lock file or the whole directory was replaced underneath this run, so this
       run no longer excludes another one writing the same artifacts.  Refusing to write
       ${artifact:-into it}."
    fi
}

# ------------------------------------------------------------------------------------
# Cleanup state and the single trap that services it.  All three actions are idempotent, so
# running the trap on a normal exit and again on a signal is harmless.
#   LCOV_HOME         private, empty, mode-0700 HOME handed to every lcov and genhtml call
#   STAGE_DIR         private staging directory for the HTML report
#   LISTING_GCOV_DIR  the sink the --gtest_list_tests launches' counters are diverted into
# Each carries the device:inode it had when this script created it, so the recursive remove
# below can prove it is deleting the directory it made -- see remove_minted_dir.
# ------------------------------------------------------------------------------------
LCOV_HOME=''
LCOV_HOME_IDENTITY=''
STAGE_DIR=''
STAGE_DIR_IDENTITY=''
LISTING_GCOV_DIR=''
LISTING_GCOV_DIR_IDENTITY=''
LISTING_GCOV_PREFIX_STRIP=''

# ------------------------------------------------------------------------------------
# `rm -rf` A DIRECTORY THIS SCRIPT MINTED, HAVING RE-PROVED IT IS THE SAME DIRECTORY.
#
# A recursive remove is the most destructive thing this script does, and a two-component path
# pattern alone does not authorise one: it keeps the remove away from '/' and '/anything', and
# it establishes nothing about whether the name still refers to the directory this
# script created.  Between creation and cleanup the name can be re-pointed -- that is the
# whole reason the minted-parent arm of assert_component_safe is fatal -- and the remove
# would then delete whatever the new target is, recursively, as the last act of a run whose
# only job was to measure.
#
# So the identity recorded at creation is compared before the remove, and it is deliberately
# checked BEFORE the -d test: -d follows symlinks, so a name replaced with a link to somebody
# else's directory looks like a directory to it.
#
# A MISMATCH IS A WARNING THAT LEAVES THE DIRECTORY ALONE, not a fatal error, and that is a
# considered choice rather than leniency.  Every caller is a cleanup function running from the
# exit trap: the run's verdict is already decided, the remaining work is housekeeping, and
# `die` from inside the trap would replace the run's real exit status with this one.  Not
# removing costs a leaked private directory that the message names; removing the wrong one
# cannot be undone.
# ------------------------------------------------------------------------------------
remove_minted_dir() { # $1=path  $2=identity recorded at creation  $3=what it is, for messages
    local dir="$1" recorded="$2" label="$3" now

    case "$dir" in
        /*/*) ;;
        *)    warn "refusing to remove an implausible $label path: $dir"
              return 0 ;;
    esac
    if [ -L "$dir" ]; then
        warn "not removing the $label directory: its name is now a symbolic link: $dir"
        return 0
    fi
    [ -d "$dir" ] || return 0
    if [ -n "$recorded" ]; then
        now="$(path_identity "$dir")"
        if [ "$now" != "$recorded" ]; then
            warn "not removing the $label directory: it is no longer the one this run created."
            warn "  $dir"
            warn "  created $recorded, the name now resolves to ${now:-nothing} (device:inode)."
            warn "  It is left in place: deleting a directory this run did not create is worse"
            warn "  than leaking one it did."
            return 0
        fi
    fi
    rm -rf -- "$dir" || warn "could not remove the $label directory: $dir"
    return 0
}

cleanup_lcov_home() {
    [ -n "$LCOV_HOME" ] || return 0
    local home="$LCOV_HOME" identity="$LCOV_HOME_IDENTITY"
    LCOV_HOME=''
    LCOV_HOME_IDENTITY=''
    remove_minted_dir "$home" "$identity" "private lcov HOME"
    return 0
}

cleanup_stage_dir() {
    [ -n "$STAGE_DIR" ] || return 0
    local stage="$STAGE_DIR" identity="$STAGE_DIR_IDENTITY"
    STAGE_DIR=''
    STAGE_DIR_IDENTITY=''
    remove_minted_dir "$stage" "$identity" "HTML staging"
    return 0
}

# The diverted listing counters are DISCARDED, and that is the whole point of collecting them
# somewhere else: they describe a process that listed test names and executed no test, so there
# is nothing in them any figure in this run may be computed from.  Removed on every exit path
# for the same reason the staging directory is -- it is this run's private directory and
# nobody's evidence.
cleanup_listing_gcov_dir() {
    [ -n "$LISTING_GCOV_DIR" ] || return 0
    local sink="$LISTING_GCOV_DIR" identity="$LISTING_GCOV_DIR_IDENTITY"
    LISTING_GCOV_DIR=''
    LISTING_GCOV_DIR_IDENTITY=''
    remove_minted_dir "$sink" "$identity" "diverted listing-counter"
    return 0
}

cleanup_makefile_snapshot() {
    [ -n "$MAKEFILE_SNAPSHOT_DIR" ] || return 0

    # THE ONE CASE WHERE CLEANUP IS THE WRONG ANSWER.  A restore that did not fully succeed
    # leaves the snapshot as the only surviving copy of the pre-build content of six tracked
    # files, one of them hand-written build source.  Removing it here would destroy the
    # recovery evidence at the exact moment recovery became necessary, and would do it inside
    # an exit trap where nobody is looking.  So it is kept, named, and left for a human.
    if [ "$MAKEFILE_SNAPSHOT_RETAIN" -ne 0 ]; then
        warn "the Makefile snapshot is DELIBERATELY LEFT IN PLACE, because the restore did not"
        warn "  fully succeed and it is now the only copy of the pre-build content:"
        warn "    $MAKEFILE_SNAPSHOT_DIR"
        warn "  It holds the six under their own relative paths, plus a manifest.txt recording"
        warn "  which of them existed and at what mode.  Copy them back from there by hand."
        warn "  Do NOT reach for 'git checkout' over these paths: it restores what the INDEX"
        warn "  holds, so on ccec/src/Makefile -- hand-written source -- it would destroy an"
        warn "  uncommitted edit.  Remove the snapshot yourself once you are done with it."
        return 0
    fi

    local snapshot="$MAKEFILE_SNAPSHOT_DIR" identity="$MAKEFILE_SNAPSHOT_IDENTITY"
    MAKEFILE_SNAPSHOT_DIR=''
    MAKEFILE_SNAPSHOT_STATE=''
    MAKEFILE_SNAPSHOT_IDENTITY=''
    remove_minted_dir "$snapshot" "$identity" "Makefile snapshot"
    return 0
}

# ------------------------------------------------------------------------------------
# Take the snapshot.  Called from do_build with AUTORECONF AS THE NEXT MUTATING COMMAND --
# the stub-header step is its own step ahead of this one rather than sitting in between, so
# the snapshot is provably of pre-build content rather than argued to be (see do_build).
#
# EVERY allowlisted path gets a record, present or absent, and the mode is captured with the
# content.  A file that exists and cannot be read, or whose copy does not compare equal to it,
# is FATAL: carrying on would mean building with no way back for that path, and a `cp` that
# returned 0 is not evidence that the bytes arrived.  ccec/src/Makefile is mode 0755 in this
# tree, so a restore that silently dropped the executable bit would be a change of its own --
# which is why the mode is recorded rather than inferred from the copy.
# ------------------------------------------------------------------------------------
# Append one record to MAKEFILE_SNAPSHOT_STATE.  A function rather than an inline append so
# the record format is written in exactly one place.
snapshot_record_state() { # $1=present|absent  $2=octal mode or '-'  $3=relative path
    MAKEFILE_SNAPSHOT_STATE="${MAKEFILE_SNAPSHOT_STATE}$1|$2|$3
"
}

snapshot_regenerated_makefiles() {
    [ -n "$MKTEMP_BIN" ] || die "internal error: snapshot_regenerated_makefiles needs mktemp"
    [ -z "$MAKEFILE_SNAPSHOT_DIR" ] || die "internal error: a Makefile snapshot already exists at
       $MAKEFILE_SNAPSHOT_DIR
       Taking a second one would overwrite the record of what the tree looked like before
       this invocation touched it."

    # THE SNAPSHOT IS THE ONLY WAY BACK from a build that overwrites six tracked files, one
    # of which -- ccec/src/Makefile -- is hand-written RDK build SOURCE that `git checkout`
    # would destroy rather than restore.  It therefore gets the strictest parent this host
    # can offer, chosen by select_safe_temp_parent rather than defaulted to /tmp, which is
    # mode 2777 with no sticky bit here: any local account can remove entries there, and the
    # recovery copy would be one of them.
    local parent
    select_safe_temp_parent
    parent="$RUNNER_TEMP_PARENT"
    assert_safe_ancestry "$parent/hdmicec-makefile-snapshot" minted

    MAKEFILE_SNAPSHOT_DIR="$("$MKTEMP_BIN" -d "$parent/hdmicec-makefile-snapshot.XXXXXXXX")" \
        || die "could not create a snapshot directory for the six tracked Makefiles under
       $parent
       Refusing to run autoreconf and configure, because they overwrite those six files and
       this run would then have no way to put them back.  'git checkout' is NOT the fallback:
       it restores what the index holds and would destroy any uncommitted edit."
    chmod 700 -- "$MAKEFILE_SNAPSHOT_DIR" \
        || die "could not restrict the Makefile snapshot directory to mode 0700: $MAKEFILE_SNAPSHOT_DIR"
    assert_private_dir "$MAKEFILE_SNAPSHOT_DIR"
    # Recorded now, checked by assert_snapshot_dir_still_safe before every later read or write.
    MAKEFILE_SNAPSHOT_IDENTITY="$(path_identity "$MAKEFILE_SNAPSHOT_DIR")"
    [ -n "$MAKEFILE_SNAPSHOT_IDENTITY" ] || die "could not read the identity (device:inode) of
       the Makefile snapshot directory: $MAKEFILE_SNAPSHOT_DIR"

    local f mode captured=0 absent=0
    for f in "${REGENERATED_MAKEFILES[@]}"; do
        # Checked before -e, which is false for a dangling link: a tracked Makefile that is a
        # symlink is not something this mechanism can snapshot or safely restore, and quietly
        # treating it as absent would license the restore to delete it.
        [ ! -L "$HDMICEC_ROOT/$f" ] || die "$HDMICEC_ROOT/$f is a symbolic link.
       The six tracked Makefiles are regular files; a link means something else supplies that
       path, and this run will neither snapshot it nor restore over it.  Investigate it
       before building."

        if [ ! -e "$HDMICEC_ROOT/$f" ]; then
            # RECORDED, not skipped.  The restore reads this and REMOVES the path if configure
            # creates one, which is what returns the tree to the state it was found in.
            snapshot_record_state absent '-' "$f"
            absent=$((absent + 1))
            continue
        fi
        [ -f "$HDMICEC_ROOT/$f" ] || die "$HDMICEC_ROOT/$f exists but is not a regular file, so it
       cannot be snapshotted and this build would have no way to restore it.  Investigate that
       path before building."

        mode="$("$STAT_BIN" -c '%a' -- "$HDMICEC_ROOT/$f" 2>/dev/null || true)"
        [ -n "$mode" ] || die "could not read the mode of $HDMICEC_ROOT/$f.
       The mode is restored alongside the content -- ccec/src/Makefile is 0755 here -- so a
       snapshot that cannot record it is not a snapshot this run may build on top of."

        mkdir -p -- "$MAKEFILE_SNAPSHOT_DIR/$(dirname -- "$f")" \
            || die "could not create the snapshot subdirectory for $f under $MAKEFILE_SNAPSHOT_DIR"
        # -p preserves the mode, which matters: ccec/src/Makefile is mode 0755 in this tree
        # and a restore that silently dropped the executable bit would be a change of its own.
        cp -p -- "$HDMICEC_ROOT/$f" "$MAKEFILE_SNAPSHOT_DIR/$f" \
            || die "could not snapshot $HDMICEC_ROOT/$f.
       Refusing to run autoreconf and configure without a way back for every one of the six."
        # VERIFY THE COPY NOW, while the original is still there to compare against.  `cp`
        # returning 0 establishes that a write happened, not that the bytes match: a full
        # filesystem, a short write or a concurrent writer all leave it happy, and a snapshot
        # that silently holds truncated content is worse than no snapshot at all because the
        # restore would confidently write it back.
        if [ -n "$CMP_BIN" ]; then
            "$CMP_BIN" -s -- "$HDMICEC_ROOT/$f" "$MAKEFILE_SNAPSHOT_DIR/$f" \
                || die "the snapshot copy of $f does not match the original.
       original: $HDMICEC_ROOT/$f
       copy:     $MAKEFILE_SNAPSHOT_DIR/$f
       Refusing to run autoreconf and configure on top of a snapshot that cannot restore
       this file faithfully."
        fi
        snapshot_record_state present "$mode" "$f"
        captured=$((captured + 1))
    done

    # A HUMAN-READABLE COPY OF THE SAME RECORD, inside the snapshot, for the one case that
    # matters: a restore that failed leaves this directory behind (see cleanup_makefile_
    # snapshot), and whoever picks it up needs to know which paths were supposed to exist and
    # at what mode.  The in-process string stays authoritative; this is documentation.
    {
        printf '# hdmicec run_coverage.sh Makefile snapshot\n'
        printf '# taken before autoreconf by pid %s\n' "$$"
        printf '# tree: %s\n' "$HDMICEC_ROOT"
        printf '# format: <state>|<octal mode>|<path relative to the tree above>\n'
        printf '%s' "$MAKEFILE_SNAPSHOT_STATE"
    } >"$MAKEFILE_SNAPSHOT_DIR/manifest.txt" \
        || die "could not write the snapshot manifest to $MAKEFILE_SNAPSHOT_DIR/manifest.txt"

    log "snapshotted $captured of ${#REGENERATED_MAKEFILES[@]} tracked Makefile(s) before autoreconf"
    if [ "$absent" -ne 0 ]; then
        log "  ($absent did not exist and are RECORDED AS ABSENT, so the restore removes any"
        log "   that configure creates rather than leaving them behind)"
    fi
    log "  snapshot: $MAKEFILE_SNAPSHOT_DIR (removed when this run exits, unless a restore fails)"
}

# ------------------------------------------------------------------------------------
# PUT ONE PATH BACK, ATOMICALLY, AND PROVE IT.
#
# A restore of the shape `cp -p snapshot target` plus a `die` if cp complains has two defects,
# both of the kind that only show up on the bad day:
#
#   * IT IS INTERRUPTIBLE IN THE MIDDLE OF A FILE.  `cp` writes into the target in place, so
#     a second Ctrl-C landing during the write leaves a tracked Makefile that is neither the
#     pre-build content nor the generated content -- a truncated hybrid, with the snapshot
#     removed by the cleanup that follows.  So the write goes to a temporary in the
#     SAME DIRECTORY and is renamed into place: `mv` within one directory is rename(2), which
#     either has happened or has not, and there is no state in between for a signal to catch.
#     The whole restore also runs with INT/TERM/HUP masked (see the caller).
#   * IT PROVES NOTHING.  A `cp` that returns 0 establishes that a write was attempted.  Full
#     filesystem, short write, a concurrent writer -- all leave it happy.  So the temporary is
#     compared against the snapshot BEFORE the rename, and the target is compared again AFTER
#     it, together with its mode.
#
# Returns 0 only when the target is byte-identical to the snapshot at the recorded mode.  Every
# failure warns with the path and the operation and returns non-zero; nothing here calls `die`,
# because the caller runs from the exit trap and has cleanup of its own left to do.
# ------------------------------------------------------------------------------------
restore_one_makefile() { # $1=relative path  $2=octal mode recorded at snapshot time
    local rel="$1" want_mode="$2"
    local src="$MAKEFILE_SNAPSHOT_DIR/$rel" dst="$HDMICEC_ROOT/$rel"
    local dir tmp now_mode

    if [ ! -f "$src" ]; then
        warn "the snapshot is missing $rel although it was recorded as present."
        warn "  looked in: $src"
        return 1
    fi
    if [ -L "$dst" ]; then
        warn "refusing to restore through a symbolic link: $dst"
        return 1
    fi
    dir="$(dirname -- "$dst")"
    if [ ! -d "$dir" ]; then
        warn "the directory that should hold $rel does not exist: $dir"
        return 1
    fi

    # THE TEMPORARY LIVES BESIDE THE TARGET, not in a temp directory, and that is the whole
    # mechanism: rename(2) is atomic only within one filesystem, and a cross-filesystem `mv`
    # silently degrades to copy-then-unlink, which opens exactly the partial-write window this
    # exists to close.  The leading dot and the pid keep it out of the way; it is removed on
    # every failure path, so only a SIGKILL between the copy and the rename can leave one.
    tmp="$dir/.run_coverage-restore.$$.$(basename -- "$rel")"
    rm -f -- "$tmp" 2>/dev/null || true
    if [ -e "$tmp" ] || [ -L "$tmp" ]; then
        warn "could not clear the restore temporary $tmp, so $rel was not restored."
        return 1
    fi
    if ! cp -p -- "$src" "$tmp"; then
        warn "could not stage the restore of $rel into $tmp"
        rm -f -- "$tmp" 2>/dev/null || true
        return 1
    fi
    if [ -n "$CMP_BIN" ] && ! "$CMP_BIN" -s -- "$src" "$tmp"; then
        warn "the staged copy of $rel does not match the snapshot, so it was NOT put in place."
        warn "  snapshot: $src"
        warn "  staged:   $tmp"
        rm -f -- "$tmp" 2>/dev/null || true
        return 1
    fi
    # Set explicitly rather than trusted to cp -p: the recorded mode is the authority, and this
    # is the one line that makes ccec/src/Makefile come back executable.
    if ! chmod -- "$want_mode" "$tmp"; then
        warn "could not set mode $want_mode on the staged copy of $rel, so it was NOT put in place."
        rm -f -- "$tmp" 2>/dev/null || true
        return 1
    fi
    if ! mv -f -- "$tmp" "$dst"; then
        warn "could not move the staged copy of $rel into place: $tmp -> $dst"
        rm -f -- "$tmp" 2>/dev/null || true
        return 1
    fi

    # POSTCONDITIONS, read back from the filesystem rather than inferred from the exit statuses
    # above.  A rename that succeeded still has to have produced the right file.
    if [ -n "$CMP_BIN" ] && ! "$CMP_BIN" -s -- "$src" "$dst"; then
        warn "$rel was restored but does not match the snapshot afterwards."
        warn "  snapshot: $src"
        warn "  tree:     $dst"
        return 1
    fi
    now_mode="$("$STAT_BIN" -c '%a' -- "$dst" 2>/dev/null || true)"
    if [ "$now_mode" != "$want_mode" ]; then
        warn "$rel was restored with mode ${now_mode:-unreadable}, not the recorded $want_mode: $dst"
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------------------------
# TAKE ONE PATH BACK OUT, because it did not exist when the snapshot was taken.
#
# This is the other half of returning the tree exactly as it was found: an unconfigured
# checkout has none of the five toolchain-generated Makefiles, configure creates them, and a
# restore that replaced only what it had copied would leave all five behind -- so the tree
# that came out of the run would differ from the tree that went in, in the one respect this
# mechanism exists to prevent.  Only paths RECORDED ABSENT are removed, and only when they are
# regular files -- so this can never delete content the snapshot holds, and never follows a link.
# ------------------------------------------------------------------------------------
remove_regenerated_makefile() { # $1=relative path recorded as absent
    local rel="$1"
    local dst="$HDMICEC_ROOT/$rel"

    if [ -L "$dst" ]; then
        warn "$rel was recorded as absent and is now a symbolic link: $dst"
        warn "  Not removing it: this run did not create a link there and will not delete one."
        return 1
    fi
    # Already absent is the ordinary case: a build that failed before configure ran.
    [ -e "$dst" ] || return 0
    if [ ! -f "$dst" ]; then
        warn "$rel was recorded as absent and is now a $( [ -d "$dst" ] && printf 'directory' || printf 'special file' ): $dst"
        warn "  Not removing it."
        return 1
    fi
    if ! rm -f -- "$dst"; then
        warn "could not remove $dst, which did not exist before this run."
        return 1
    fi
    if [ -e "$dst" ] || [ -L "$dst" ]; then
        warn "$dst still exists after being removed; something is recreating it."
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------------------------
# Put the allowlisted Makefiles back the way this run found them.  Idempotent, so the trap and
# an explicit --restore can both call it, and a no-op when no snapshot was ever taken.
#
# UNINTERRUPTIBLE FOR ITS DURATION.  INT, TERM and HUP are masked across the whole loop and
# re-armed afterwards, because a signal arriving between two of the six leaves half the tree
# holding pre-build content and half holding generated content -- and the second Ctrl-C that
# does it is exactly what an impatient caller sends when the first one appears to have done
# nothing.  Masking discards the signal rather than deferring it, which shell offers no way
# around; the trade is deliberate and small, because the restore is six file copies and is
# bounded by that, whereas a half-restored hand-written Makefile is unbounded work for a human.
#
# RETURNS NON-ZERO AND KEEPS THE SNAPSHOT when any path fails.  MAKEFILE_SNAPSHOT_RETAIN is
# what tells the cleanup not to delete the only remaining copy of the pre-build content, and
# the caller (on_exit) turns the non-zero into the run's exit status.  Nothing in here calls
# `die`: a `die` from inside the exit trap would skip the staging cleanup, the private lcov
# HOME and the lock release, so every failure is reported and returned instead.
#
# A PRE-CHECK AND A POST-CHECK BRACKET THE WORK: report what was dirty going in (`was:`), do
# the restore, then re-read git and report anything still dirty
# (`still:`).  `git` is used here for REPORTING ONLY -- when it is
# absent or this is not a working tree the restore still happens and the reporting degrades to
# a note.  The restore does not depend on git in any way, which is the whole point.
# ------------------------------------------------------------------------------------
restore_regenerated_makefiles_from_snapshot() {
    [ -n "$MAKEFILE_SNAPSHOT_DIR" ] || return 0
    [ -n "$MAKEFILE_SNAPSHOT_STATE" ] || return 0

    # MASK FIRST, so even the custody check and the reporting cannot be interrupted half way
    # into the loop that follows.
    trap '' INT TERM HUP

    # Custody of the snapshot, re-proved at the moment it is read from.  The assertions are
    # fatal by design, so they are run in a SUBSHELL and their status captured: measured on
    # this host, a ( ) subshell does NOT run the parent's EXIT trap, so a `die` in there ends
    # the subshell and nothing else, while its message still reaches the caller's stderr.
    if ! ( assert_snapshot_dir_still_safe "restoring the tracked Makefiles" ); then
        MAKEFILE_SNAPSHOT_RETAIN=1
        warn "the snapshot directory cannot be vouched for (see above), so NOTHING was restored."
        warn "  The snapshot is left in place; treat its contents with suspicion and compare"
        warn "  them against your own copy before using them."
        trap 'on_signal INT 130' INT
        trap 'on_signal TERM 143' TERM
        trap 'on_signal HUP 129' HUP
        return 1
    fi

    local git_usable=0
    if command -v git >/dev/null 2>&1 \
       && git -C "$HDMICEC_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        git_usable=1
    fi

    local before=''
    if [ "$git_usable" -eq 1 ]; then
        before="$(git -C "$HDMICEC_ROOT" status --porcelain -- "${REGENERATED_MAKEFILES[@]}" 2>/dev/null || true)"
    fi

    rule
    log "restoring the tracked Makefiles from this run's snapshot"
    if [ -n "$before" ]; then
        printf '%s\n' "$before" | sed 's/^/[run_coverage]   was: /'
    elif [ "$git_usable" -eq 1 ]; then
        log "  none of the six is currently modified; restoring from the snapshot anyway, because"
        log "  the snapshot is the authority on their pre-build content and a byte-identical copy"
        log "  is a no-op rather than a risk."
    fi

    local state mode f restored=0 removed=0 failures=0 seen=0
    while IFS='|' read -r state mode f; do
        [ -n "$f" ] || continue
        seen=$((seen + 1))
        case "$state" in
            present)
                if restore_one_makefile "$f" "$mode"; then
                    restored=$((restored + 1))
                else
                    failures=$((failures + 1))
                fi
                ;;
            absent)
                if remove_regenerated_makefile "$f"; then
                    removed=$((removed + 1))
                else
                    failures=$((failures + 1))
                fi
                ;;
            *)
                # A record this function does not understand is a defect in the snapshot, and
                # guessing would mean either fabricating a file or deleting one.
                warn "the snapshot record for $f has an unrecognised state '$state', so that path"
                warn "  was left exactly as it is.  This is a defect in this script; report it."
                failures=$((failures + 1))
                ;;
        esac
    done <<< "$MAKEFILE_SNAPSHOT_STATE"

    # EVERY ALLOWLISTED PATH MUST HAVE HAD A RECORD.  A path with none was never snapshotted
    # and is therefore neither restorable nor safe to remove, so its absence from the record is
    # reported as a failure rather than passing unnoticed as a shorter loop.
    if [ "$seen" -ne "${#REGENERATED_MAKEFILES[@]}" ]; then
        warn "the snapshot holds $seen record(s) for ${#REGENERATED_MAKEFILES[@]} allowlisted path(s)."
        warn "  A path with no record was not snapshotted, so this run cannot say what it looked"
        warn "  like before the build.  Compare the six against your own copy."
        failures=$((failures + 1))
    fi

    log "restored $restored file(s) from the snapshot; removed $removed that did not exist before"

    if [ "$failures" -ne 0 ]; then
        MAKEFILE_SNAPSHOT_RETAIN=1
        warn "$failures of $seen allowlisted path(s) could NOT be returned to the state this run"
        warn "  found them in; the reasons are above, one per path.  The tree is therefore a"
        warn "  MIXTURE of pre-build and generated content and must not be committed as it is."
        # Re-armed before returning, so a caller that continues after the failure is once again
        # cancellable.
        trap 'on_signal INT 130' INT
        trap 'on_signal TERM 143' TERM
        trap 'on_signal HUP 129' HUP
        return 1
    fi

    if [ "$git_usable" -eq 1 ]; then
        local after
        after="$(git -C "$HDMICEC_ROOT" status --porcelain -- "${REGENERATED_MAKEFILES[@]}" 2>/dev/null || true)"
        if [ -n "$after" ]; then
            printf '%s\n' "$after" | sed 's/^/[run_coverage]   still: /' >&2
            # NOT fatal, deliberately.  A check that took the INDEX as its reference could read
            # "still dirty" only as a restore that had failed.  Here the snapshot is the
            # reference, and a path that was ALREADY
            # modified relative to the index before this run started is still modified after a
            # faithful restore -- correctly so.  That is a report, not a failure.
            warn "the paths above still differ from the index.  That is expected when they were"
            warn "  already modified before this run started: the snapshot restores the bytes"
            warn "  this run found, which is not the same as the bytes git holds.  Compare them"
            warn "  yourself if you need to -- this script will not resolve it for you by"
            warn "  reverting anything."
        else
            log "the six now match the index"
        fi
    else
        log "git is not usable here, so the restore was not cross-checked against the index."
        log "  The copy itself does not depend on git and has already been verified above."
    fi

    # THE SUCCESS PATH RE-ARMS TOO.  The mask covers the restore and nothing else; leaving it
    # in place would make the rest of the exit trap, and anything a --restore caller does next,
    # silently uncancellable.
    trap 'on_signal INT 130' INT
    trap 'on_signal TERM 143' TERM
    trap 'on_signal HUP 129' HUP
    return 0
}

# ------------------------------------------------------------------------------------
# Take the one tree lock.  Idempotent: the first caller acquires it, every later caller is a
# no-op that simply confirms it is still held.  See THE TREE LOCK above for what it covers
# and why nothing here is advisory.
# ------------------------------------------------------------------------------------
acquire_tree_lock() { # $1=short description of what this run is about to do, for the log
    local purpose="${1:-work in this tree}"

    if [ -n "$TREE_LOCK_FD" ]; then
        verify_tree_lock_identity "$purpose"
        return 0
    fi

    [ -n "$FLOCK_BIN" ] || die "flock was not found on PATH, so this run cannot prove it is the
       only one operating on
       $HDMICEC_ROOT
       That proof is not decoration.  Without it a second run can zero the .gcda counters
       between this run's last invocation and its capture, or run its own suite while this one
       captures, and in both cases the coverage figure that comes out belongs to neither run
       while looking entirely plausible.  It can also interleave the Makefile snapshot with a
       restore and put the other run's GENERATED content back over your source.
       flock ships with util-linux; install it, or run this script on a host that has it.
       (This is fatal rather than advisory: a warning that still reaches a verdict proves
       nothing.)"

    local dir key path identity_before identity_open identity_after
    runner_lock_dir
    dir="$RUNNER_LOCK_DIR"

    # '/' is not legal in a filename, so the tree's own path becomes the key with the
    # separators flattened.  A path long enough to exceed the filename limit keeps its TAIL,
    # which is the part that distinguishes sibling clones, with the full length prefixed so
    # two different paths that share a tail cannot collide silently.
    key="${HDMICEC_ROOT//\//_}"
    if [ "${#key}" -gt 180 ]; then
        key="${#key}${key: -180}"
    fi
    path="$dir/tree$key.lock"

    # THE LEAF GETS FILE CHECKS, NOT THE ANCESTRY WALK, and the reason is measured rather than
    # a subtlety worth guessing at.  assert_safe_ancestry validates every EXISTING
    # component of the path it is given and requires each one to be a DIRECTORY.  The lock's
    # leaf is a regular FILE and -- unlike the artifact root, which is minted fresh -- it
    # PERSISTS between runs, so a walk over the leaf reaches an existing regular file
    # where it expects a directory and refuses.  Measured symptom of that shape: the first
    # --build succeeds because the leaf does not exist yet, and every subsequent one dies with
    # "is a regular empty file, not a directory".
    #
    # So the ancestry is proved once, for the DIRECTORY, by runner_lock_dir -- which is
    # strictly stronger than walking the leaf, because that directory is created by mkdir and
    # verified owner-only, so no other account can put anything beside the leaf at all -- and
    # the leaf itself gets the checks that are meaningful for a file: not a symlink, a regular
    # file, created exclusively when it is new, and verified by device and inode across the
    # open and across the flock.
    [ ! -L "$path" ] || die "refusing to lock through a symbolic link: $path
       Remove it.  Nothing but this script should be creating entries in $dir."
    if [ -e "$path" ]; then
        [ -f "$path" ] || die "the tree lock path exists and is not a regular file: $path"
    else
        # noclobber makes '>' fail if the name exists AND refuses to follow a symlink to a
        # non-existent target, which is the create-exclusive semantics this needs.  Losing the
        # race to a concurrent run of this same script is fine: what matters is that the leaf
        # is a regular file in a directory only this user can write.
        ( set -o noclobber; : > "$path" ) 2>/dev/null || {
            [ -f "$path" ] || die "could not create the tree lock file: $path"
        }
    fi

    identity_before="$(path_identity "$path")"
    [ -n "$identity_before" ] || die "the tree lock file vanished immediately after it was
       created: $path"

    # '>>' so an existing lock file is never truncated: another run may be holding it, and its
    # descriptor is the only thing that matters -- the contents are deliberately empty.
    exec {TREE_LOCK_FD}>>"$path" || die "could not open the tree lock: $path"

    identity_open="$(open_fd_identity "$TREE_LOCK_FD")"
    [ "$identity_open" = "$identity_before" ] || die "the tree lock file was replaced between
       the check and the open: $path
       checked $identity_before, opened $identity_open (device:inode).
       Somebody unlinked and recreated that name.  Two runs holding two different inodes both
       believe they hold this lock, which is the failure this check exists to catch.  Refusing
       to continue."

    "$FLOCK_BIN" -n "$TREE_LOCK_FD" || die "another run of this script is already working in
       $HDMICEC_ROOT
       One run per tree: the build, the Makefile snapshot and restore, the counter reset, the
       five invocations and the capture all operate on this one tree, and two runs interleaving
       them produce a verdict that belongs to neither.  Wait for the other run to finish.
       (The lock is $path, held on an open descriptor and released when that run's shell exits,
       so a crashed run does not leave it stuck.)"

    identity_after="$(path_identity "$path")"
    [ "$identity_after" = "$identity_before" ] || die "the tree lock file was replaced while
       this run was acquiring it: $path
       locked $identity_before, the name now resolves to $identity_after (device:inode).
       The lock this run holds no longer guards the name a second run would open, so it no
       longer excludes anything.  Refusing to continue."

    TREE_LOCK_PATH="$path"
    TREE_LOCK_IDENTITY="$identity_before"
    TREE_LOCK_PURPOSE="$purpose"
    log "holding the exclusive tree lock for $HDMICEC_ROOT ($purpose)"
    log "  lock: $path [$identity_before]"
}

# Confirm the lock this run took still guards the name it took it on.  Called at the start of
# every stage that writes to the tree or reads a verdict out of it, because "the lock was held
# when the run started" is a weaker statement than "the lock is held now" -- and the gap
# between them is exactly where a second run gets in.
verify_tree_lock_identity() { # $1=what is about to happen, for the message
    local what="${1:-this step}" identity_open identity_now

    [ -n "$TREE_LOCK_FD" ] || die "internal error: $what was reached without the tree lock.
       Every stage that touches this tree must run under it; this is a defect in this script,
       not in the caller's invocation."

    identity_open="$(open_fd_identity "$TREE_LOCK_FD")"
    [ "$identity_open" = "$TREE_LOCK_IDENTITY" ] || die "the descriptor holding the tree lock no
       longer refers to the file it was taken on, before $what.
       held $TREE_LOCK_IDENTITY, descriptor now refers to $identity_open (device:inode)."

    identity_now="$(path_identity "$TREE_LOCK_PATH")"
    [ "$identity_now" = "$TREE_LOCK_IDENTITY" ] || die "the tree lock file was replaced during
       this run, before $what: $TREE_LOCK_PATH
       locked $TREE_LOCK_IDENTITY, the name now resolves to ${identity_now:-nothing}
       (device:inode).  A second run opening that name would not be excluded by this run's
       lock, so this run can no longer claim exclusive use of
       $HDMICEC_ROOT
       (this run holds it for: ${TREE_LOCK_PURPOSE:-unrecorded})
       Refusing to continue."
    return 0
}

# Release it, and only ever as the LAST thing the exit trap does.  The cleanup it runs after
# the work -- restoring the six tracked Makefiles, removing the snapshot -- is itself work on
# this tree, so releasing before that finishes would let a second run start half way through
# a restore.
release_tree_lock() {
    [ -n "$TREE_LOCK_FD" ] || return 0
    local fd="$TREE_LOCK_FD"
    TREE_LOCK_FD=''
    # Closing the descriptor releases the flock; the empty lock file is deliberately left in
    # place, because it is the agreed name and re-creating it per run is what would make it
    # substitutable.
    exec {fd}>&- 2>/dev/null || true
    return 0
}

# ------------------------------------------------------------------------------------
# CANCELLATION.  A run that cannot be stopped is a run CI cannot cancel.
#
# Bash runs a trap only BETWEEN commands.  A suite launched as a FOREGROUND child
# -- `( cd …; timeout … ./run_L1Tests ) >"$run_log"` -- leaves this shell sitting inside that
# command, so an external SIGTERM is recorded and then withheld from the handler until the child
# finishes on its own.  For the length of a whole suite the runner would ignore its own
# cancellation, with nothing forwarding the signal to the suite: a cancelled job leaves the
# test binary running and the private lcov HOME and staging directory behind it.
#
# So the suite is launched in the BACKGROUND and in its OWN PROCESS GROUP -- `set -m` makes a
# background job a process-group leader -- and this shell waits on it.  `wait` is interruptible,
# so a signal reaches the handler at once; the handler then signals the whole GROUP, which is
# `timeout` and the test binary, both of which stay in that group because `timeout --foreground`
# deliberately does not create one of its own.  A bounded grace period follows, then the group is
# killed outright.
#
# ONE GROUP IS NOT THE WHOLE TREE, AND THAT WAS MEASURED RATHER THAN SUSPECTED.  The L2 harness
# makes the child it forks a process-group LEADER, deliberately and on both sides of the fork
# (tests/L2Tests/test_main.cpp: the child's own `setpgid(0, 0)`, and the parent's
# `setpgid(child, child)` immediately after it).  It has to: the fake service host may itself
# fork, and a teardown that could only signal the one pid it forked left the host's own children
# running -- while the only group-wide signal available to the harness addressed the RUNNER's
# group, which would have killed the runner.  The consequence for this script is that the host
# and everything below it sit in a group of their OWN, and a signal addressed to `-$SUITE_PGID`
# never reaches them.  Measured at signal time during invocation D, cancelled with SIGTERM:
#
#     timeout       3234979  pgid 3234979   <-- = SUITE_PGID, the backgrounded subshell
#     run_L2Tests   3234980  pgid 3234979       in the suite's group
#     run_L2Tests   3234996  pgid 3234996   <-- ITS OWN GROUP: escapes -$SUITE_PGID entirely
#     run_L2Tests   3234997  pgid 3234996       and so does everything below it
#
# 3234996 and 3234997 survived the cancellation with PPid 1, were still alive at t=60s, one of
# them with SIGTERM ignored (SigIgn 0x5006), each holding the cancelled run's coverage temp
# directory -- while this script logged that the group was "confirmed gone".  On invocation E
# that survivor is the fake service host, still holding the production "HdmiCec" registration
# that both harnesses treat as a HARD FAILURE when they find it already taken, so one cancelled
# run poisoned every later AIDL invocation on that host until somebody killed it by hand.
#
# SO THE CUSTODY RECORD IS A SET OF GROUPS, NOT ONE GROUP.  SUITE_PGID is the backgrounded
# subshell's pid and, under `set -m`, also its group id, which makes it the root of a walk over
# the process tree; SUITE_EXTRA_PGIDS holds every OTHER process group found below that root.  The
# walk is taken BEFORE the first signal is sent -- see suite_capture_descendant_groups for why
# that ordering is the whole of the fix rather than merely tidier.
SUITE_PGID=''                  # process group of the running suite; empty when none is running
SUITE_EXTRA_PGIDS=''           # space-separated ids of the DESCENDANT process groups of that
                               # group, discovered by walking the tree before it is signalled.
                               # Part of the same custody record as SUITE_PGID: the two are
                               # cleared together, and only when every id in the set has been
                               # probed and found extinct, so the EXIT trap's retry inherits
                               # whatever the first attempt could not establish -- by which
                               # point these ids may be all that is left of it.
SUITE_SIGNAL=''                # name of the signal that cancelled this run, if any
# How long a signalled process group is given to exit before it is killed outright.  Bounded
# because the point of the exercise is that a cancellation completes.
SUITE_STOP_GRACE_SECONDS="${SUITE_STOP_GRACE_SECONDS:-10}"
# CHECKED AT THE DECLARATION rather than in parse_args, and that position is deliberate: the
# signal handler that prints this value can fire from the moment the trap is installed, which is
# before any argument has been parsed, so a check in parse_args would leave a window in which a
# newline-bearing value reached a diagnostic.  die() and render_untrusted() are both defined
# above this line, which is what makes checking here possible at all.  Zero is ACCEPTED here,
# unlike SUITE_TIMEOUT: "give the group no grace at all, kill it immediately" is a coherent
# choice, and await_process_exit() implements it as one.
case "$SUITE_STOP_GRACE_SECONDS" in
    ''|*[!0-9]*)
        die "SUITE_STOP_GRACE_SECONDS must be a plain non-negative integer number of seconds, got
       $(render_untrusted "$SUITE_STOP_GRACE_SECONDS").  It bounds how long a signalled suite is
       given to exit and is named in the diagnostic that reports the kill, so a value that cannot
       be read exactly is refused rather than coerced." ;;
esac
readonly SUITE_STOP_GRACE_SECONDS

# Wait up to $1 seconds for EVERY kill target named in $2.. (a pid, or -pgid) to disappear.
# 0 when all of them are gone, 1 when the period expired with at least one still answering.
#
# ONE PERIOD FOR THE WHOLE SET, which is why this takes a list rather than being called in a
# loop.  Groups signalled together exit concurrently, so a per-target wait would charge
# SUITE_STOP_GRACE_SECONDS for each of them in turn and multiply the bound by the number of
# groups -- and the bound exists precisely because a cancellation has to COMPLETE.  Each pass
# probes the targets in order and stops at the first one still alive, so the ordinary case (one
# target, already gone) costs the single `kill -0` it always did.
#
# A zero-second period is still a coherent instruction and is implemented as one: the first pass
# probes, and if anything answers, `waited` (0) is not less than 0, so it returns 1 immediately
# and the caller escalates without waiting.  Nothing is signalled here -- this only observes.
await_process_exit() { # $1 = seconds  $2.. = kill targets
    local seconds="$1" waited=0 target alive
    shift
    # No targets is not an error: the caller filtered a set that turned out to be empty, and
    # "everything in the empty set is gone" is the truthful answer rather than a special case.
    [ "$#" -gt 0 ] || return 0
    while : ; do
        alive=''
        for target in "$@"; do
            if kill -0 -- "$target" 2>/dev/null; then
                alive="$target"
                break
            fi
        done
        [ -n "$alive" ] || return 0
        [ "$waited" -lt "$seconds" ] || return 1
        sleep 1
        waited=$((waited + 1))
    done
}

# ------------------------------------------------------------------------------------
# READING THE PROCESS TREE OUT OF /proc, BECAUSE THE ALTERNATIVES ARE WORSE.
#
# The rule this script keeps everywhere else applies here too: processes are addressed BY ID and
# never by a command-line pattern.  `pkill -f run_L1Tests` would match anything that merely
# mentions the name -- up to and including the harness that started this script, and the CI step
# that started the harness -- so a pattern is not a narrower tool here, it is an unbounded one.
# `ps --ppid` would do, but it is one fork per level of the tree and its output format varies
# between the procps and busybox implementations this script is run under; /proc is already how
# this script establishes descriptor identity (see open_fd_identity), so it is the idiomatic
# source here rather than a novel one, and it needs no external binary at all.
#
# PROC_STAT_PPID and PROC_STAT_PGID are set by read_proc_ppid_and_pgid rather than printed,
# which is not a style choice: the walk below reads every entry under /proc, and a command
# substitution per entry would be one fork per process on the host for every cancellation.
# ------------------------------------------------------------------------------------
PROC_STAT_PPID=''
PROC_STAT_PGID=''

# Read the parent pid and the process group id of $1 into PROC_STAT_PPID / PROC_STAT_PGID.
# Both are left EMPTY when the pid is gone, when /proc is not readable, or when the line cannot
# be parsed with certainty -- and every caller treats empty as "no information", never as zero.
read_proc_ppid_and_pgid() { # $1 = pid
    local pid="$1" line rest
    PROC_STAT_PPID=''
    PROC_STAT_PGID=''

    # A pid that disappears between the directory listing and this read is the ORDINARY case
    # when walking a tree that is dying, not an error worth a diagnostic: the redirection fails,
    # `2>/dev/null` (placed FIRST, so it is in force before the failing `<` is attempted)
    # swallows bash's message, and the empty globals report "gone" to the caller.
    IFS= read -r line 2>/dev/null < "/proc/$pid/stat" || return 0
    [ -n "$line" ] || return 0

    # THE COMM FIELD IS PARENTHESISED AND UNESCAPED, WHICH IS WHY THIS DOES NOT SPLIT ON
    # WHITESPACE.  /proc/<pid>/stat renders field 2 as the executable name in parentheses with
    # nothing quoted or stripped, so a process named `sh) 1 2 3 (x` -- which a caller may create
    # deliberately -- shifts every field after it and a whitespace split silently yields another
    # process's parent and group.  Stripping the LONGEST prefix up to a `)` cannot be fooled by
    # that: the kernel writes exactly one `)` after the comm, so the last `)` on the line is
    # always the one that closes it, whatever the comm itself contains.
    rest="${line##*)}"
    # Deliberate word splitting on a known-numeric field list, and the positional parameters are
    # local to this function so nothing outside it is disturbed.  After the comm come: state (1),
    # ppid (2), pgrp (3).
    # shellcheck disable=SC2086  # intentional split of /proc stat fields, which are never quoted
    set -- $rest
    [ "$#" -ge 3 ] || return 0
    case "$2" in ''|*[!0-9]*) return 0 ;; esac
    case "$3" in ''|*[!0-9]*) return 0 ;; esac

    PROC_STAT_PPID="$2"
    PROC_STAT_PGID="$3"
    return 0
}

# ------------------------------------------------------------------------------------
# ENUMERATE THE SUITE'S DESCENDANT PROCESS GROUPS, AND DO IT BEFORE ANYTHING IS SIGNALLED.
#
# THE ORDERING IS THE FIX, NOT AN OPTIMISATION.  A descendant group is discoverable only while
# the chain of parents that leads to it is intact.  The measurement in THE CANCELLATION block
# above shows both halves of that: at signal time 3234996's parent was 3234980, itself a child of
# the subshell, so a walk from SUITE_PGID reached it; one SIGTERM later 3234980 was gone, 3234996
# had been reparented to PID 1, and NO walk from this script could ever find it again.  So this
# runs at the top of stop_suite_group, before the first `kill`, and its result is kept in the
# custody record for the retry rather than recomputed there.
#
# MERGED, NEVER REPLACED.  A second call adds to SUITE_EXTRA_PGIDS and removes nothing: by the
# time the EXIT trap calls stop_suite_group again, the tree that yielded these ids no longer
# exists, and dropping an id because this pass could not re-derive it would discard the only
# handle the script has on a process that is still alive.
#
# SEEDED TWO WAYS, WHICH IS WHAT MAKES IT WORK ON BOTH PATHS.  The root pid covers the
# cancellation path, where the subshell is still running.  Every live process whose group is
# already in the custody record is also a seed, which covers the close-out path, where `wait` has
# already collected the subshell and only lower members of its group are left.  A group whose
# entire ancestor chain has already exited is beyond the reach of any walk -- that case is what
# the pre-signal ordering exists to prevent, and on the success path it is the harness's own
# teardown, which signals its host's group by the id it read back after the fork, that covers it.
#
# WHAT IT REFUSES TO RETURN, and this guard is load-bearing rather than defensive book-keeping.
# This script's own process group is EXCLUDED: a descendant may join any group in its session,
# the runner's group is in that session, and a group-directed signal to it would kill the runner
# in the middle of its own cleanup -- the exact outcome the L2 harness's comment block cites as
# the reason it never signals the group a fork inherits.  Groups 0 and 1 are excluded for the
# same reason at larger scale.  SUITE_PGID itself is not filtered: `set -m` created it for the
# backgrounded subshell alone, so it is this run's by construction.
#
# Always returns 0.  It is called from the signal handler and from the EXIT trap, both of which
# run under `set -e`, and a non-zero return from an enumeration would abort a cleanup over what
# is only ever a best-effort reading of a tree that is disappearing as it is read.
# ------------------------------------------------------------------------------------
suite_capture_descendant_groups() {
    local entry pid own_pgid table='' seeds='' frontier='' next='' seen='' groups=''
    local line p pp pg cursor

    [ -n "$SUITE_PGID" ] || return 0

    # OUR OWN GROUP FIRST, AND NO WALK AT ALL WITHOUT IT.  The exclusion above cannot be applied
    # to a value that could not be read, and a walk that cannot exclude the runner's group could
    # hand a caller an id whose signalling ends this script.  A /proc this script cannot read
    # therefore degrades to exactly the previous behaviour -- SUITE_PGID alone -- which is a
    # smaller failure than either guessing or refusing to clean up.
    read_proc_ppid_and_pgid "$$"
    own_pgid="$PROC_STAT_PGID"
    if [ -z "$own_pgid" ]; then
        warn "could not read this script's own process group from /proc, so the suite's"
        warn "    descendant process groups cannot be enumerated safely; falling back to"
        warn "    signalling process group $SUITE_PGID alone.  A descendant that made itself a"
        warn "    group leader -- the L2 fake service host does -- may survive this cleanup."
        return 0
    fi

    # ONE SNAPSHOT, THEN THE WALK OVER THAT SNAPSHOT.  Re-reading /proc per level would mix
    # readings taken at different instants, and a tree that is being torn down changes between
    # them: a child seen in the second reading but not the first is a child whose parent link was
    # read after the parent had gone.  A single pass is not atomic either -- nothing over /proc
    # is -- but it is one instant's worth of inconsistency instead of one per level.
    for entry in /proc/[0-9]*; do
        pid="${entry#/proc/}"
        read_proc_ppid_and_pgid "$pid"
        [ -n "$PROC_STAT_PGID" ] || continue
        table="${table}${pid} ${PROC_STAT_PPID} ${PROC_STAT_PGID}
"
    done
    [ -n "$table" ] || return 0

    # SEEDS.  The root pid, plus every live process already in one of the recorded groups.
    seeds="$SUITE_PGID"
    while IFS=' ' read -r p pp pg; do
        [ -n "$p" ] || continue
        case " $SUITE_PGID $SUITE_EXTRA_PGIDS " in
            *" $pg "*) case " $seeds " in *" $p "*) : ;; *) seeds="$seeds $p" ;; esac ;;
        esac
    done <<< "$table"

    # BREADTH-FIRST, PARENT TO CHILDREN.  Downward only: the relation is followed from a pid to
    # the processes whose ppid is that pid and never the other way, so the walk cannot climb out
    # of the suite's tree into the harness that started this script.  `seen` makes it terminate
    # whatever /proc reports -- a ppid cycle cannot occur, but a snapshot in which a pid was
    # reused between two reads could present one, and a walk over live kernel data is not the
    # place to assume it cannot.
    frontier="$seeds"
    while [ -n "$frontier" ]; do
        next=''
        for cursor in $frontier; do
            case " $seen " in *" $cursor "*) continue ;; esac
            seen="$seen $cursor"
            while IFS=' ' read -r p pp pg; do
                [ -n "$p" ] || continue
                if [ "$p" = "$cursor" ]; then
                    case " $groups " in *" $pg "*) : ;; *) groups="$groups $pg" ;; esac
                fi
                if [ "$pp" = "$cursor" ]; then
                    case " $seen $next " in *" $p "*) : ;; *) next="$next $p" ;; esac
                fi
            done <<< "$table"
        done
        frontier="$next"
    done

    for pg in $groups; do
        # SUITE_PGID is already the head of the custody record; 0 and 1 and this script's own
        # group are the three ids a group-directed signal must never be pointed at.
        [ "$pg" != "$SUITE_PGID" ] || continue
        [ "$pg" != "$own_pgid" ]   || continue
        [ "$pg" -gt 1 ]            || continue
        case " $SUITE_EXTRA_PGIDS " in
            *" $pg "*) : ;;
            *) SUITE_EXTRA_PGIDS="${SUITE_EXTRA_PGIDS:+$SUITE_EXTRA_PGIDS }$pg" ;;
        esac
    done
    return 0
}

# The custody record as a flat list, SUITE_PGID first so the ordinary single-group case reads
# and behaves exactly as it did before this became a set.  Empty when nothing is registered.
suite_custody_pgids() {
    local pgid out=''
    if [ -n "$SUITE_PGID" ]; then
        out="$SUITE_PGID"
    fi
    for pgid in $SUITE_EXTRA_PGIDS; do
        case " $out " in
            *" $pgid "*) : ;;
            *) out="${out:+$out }$pgid" ;;
        esac
    done
    printf '%s' "$out"
}

# Forward $1 to the suite's process group AND to every descendant process group of it, then make
# sure each one is actually gone.  Idempotent, so the signal handler and the EXIT handler can both
# call it.  Every group is addressed by id -- never by a command-line pattern, because
# `pkill -f run_L1Tests` would also match anything that merely mentions the name, including the
# harness that started this script.
#
# THE ENUMERATION IS THE FIRST THING THIS FUNCTION DOES, AND THAT IS THE FIX.  Signalling
# `-$SUITE_PGID` and only then looking for what else is out there finds nothing: the intermediate
# process dies, its child is reparented to PID 1, and the link that made the child reachable is
# gone.  The measurement in THE CANCELLATION block above is exactly that sequence.  So the walk
# runs while the tree is still intact, before the first `kill`, and what it finds is added to the
# custody record rather than used and forgotten.
#
# RETURN CONTRACT, AND WHY IT IS NOT ALWAYS 0.
#   0 -- there was nothing registered, or every group in the custody record has now been PROBED
#        and found extinct.  SUITE_PGID and SUITE_EXTRA_PGIDS are both cleared.
#   1 -- at least one group STILL EXISTS after SIGTERM and SIGKILL.  Extinction could not be
#        established, so the WHOLE custody record -- SUITE_PGID and SUITE_EXTRA_PGIDS alike -- is
#        DELIBERATELY LEFT POPULATED: the EXIT trap calls this function again and gets another
#        attempt, and the ids stay in every diagnostic printed after this point.  The descendant
#        ids matter most here, because they are the ones a later walk can no longer re-derive:
#        once their parents are gone they are reachable only through this record.  Clearing
#        either half would discard the only custody record this script has at the exact moment
#        that record becomes load-bearing, leaving a live process nothing could address and
#        nothing could report.
# A caller that cannot act on the status must say so explicitly (`|| warn …`), never by ignoring
# it: this runs under `set -e`, and a bare call inside the EXIT trap would abort the rest of the
# trap -- the Makefile restore included -- the first time a group outlived a SIGKILL.
stop_suite_group() { # $1 = signal name to forward
    local signal="${1:-TERM}"
    local registered live='' survivors='' pgid count=0 live_count=0
    local -a kill_targets=()

    # Both halves of the record are consulted.  A retry can arrive with SUITE_PGID's own group
    # already extinct and a descendant group still alive, and returning early on the first half
    # alone would be the same false "nothing to do" the single-group code gave for a tree it
    # never looked at.
    registered="$(suite_custody_pgids)"
    [ -n "$registered" ] || return 0

    suite_capture_descendant_groups
    registered="$(suite_custody_pgids)"
    for pgid in $registered; do
        count=$((count + 1))
    done

    # PROBE BEFORE SIGNALLING, AND SIGNAL ONLY WHAT ANSWERS.  A group that is already gone needs
    # no signal and must not be claimed as one that was stopped; a `kill` to a dead group would
    # also fail, and a failure that carries no information is worth neither the diagnostic nor
    # the `|| true` that would hide it.
    for pgid in $registered; do
        if kill -0 -- "-$pgid" 2>/dev/null; then
            live="${live:+$live }$pgid"
            live_count=$((live_count + 1))
            kill_targets+=("-$pgid")
        fi
    done
    if [ -z "$live" ]; then
        SUITE_PGID=''
        SUITE_EXTRA_PGIDS=''
        return 0
    fi

    # THE COUNT IS REPORTED EVEN WHEN IT IS ONE, and the ids are named as soon as it is more,
    # because "this suite had a process group of its own below the one the runner created" is
    # itself a fact about the harness that a reader of this log needs.  The single-group wording
    # is preserved verbatim: on invocations A, B and C there is nothing below the suite's own
    # group, and that log line should read exactly as it always has.
    if [ "$count" -eq 1 ]; then
        warn "forwarding SIG$signal to the suite process group $SUITE_PGID"
    else
        warn "forwarding SIG$signal to the suite's $count process groups: $registered"
        warn "    ($SUITE_PGID is the group this runner created; the rest are descendant groups"
        warn "     found by walking the tree before signalling it.  The L2 harness makes the fake"
        warn "     service host a group leader of its own, so a signal to $SUITE_PGID alone would"
        warn "     leave it and everything below it running.)"
        if [ "$live_count" -ne "$count" ]; then
            warn "    $live_count of the $count were still alive at this probe: $live"
        fi
    fi
    for pgid in $live; do
        kill -"$signal" -- "-$pgid" 2>/dev/null || true
    done

    # ONE GRACE PERIOD FOR THE WHOLE SET, not one per group: see await_process_exit.  The groups
    # were signalled together and exit concurrently, and the bound exists so that a cancellation
    # completes -- multiplying it by however many groups the harness created would defeat it.
    if ! await_process_exit "$SUITE_STOP_GRACE_SECONDS" "${kill_targets[@]}"; then
        if [ "$live_count" -eq 1 ]; then
            warn "the suite process group $live ignored SIG$signal for"
        else
            warn "one or more of the suite process groups $live ignored SIG$signal for"
        fi
        warn "    ${SUITE_STOP_GRACE_SECONDS}s (SUITE_STOP_GRACE_SECONDS); killing them outright."
        for pgid in $live; do
            kill -KILL -- "-$pgid" 2>/dev/null || true
        done
        # PHRASED AS THE SET, NOT AS EACH MEMBER OF IT, because that is all this wait
        # establishes: await_process_exit stops at the first target still answering, so it knows
        # that SOMETHING in the set outlived SIGKILL and not which.  Naming them all here would
        # report groups that had in fact gone.  The per-group re-probe below is what names the
        # actual survivors, and it runs unconditionally a few lines later.
        await_process_exit 5 "${kill_targets[@]}" \
            || warn "at least one of the process group(s) $live is still answering after SIGKILL;
       report this, it should not happen."
    fi

    # EXTINCTION IS VERIFIED, NOT ASSUMED FROM THE KILL, AND IT IS VERIFIED PER GROUP.  `kill`
    # returning 0 means the signal was delivered, not that anything acted on it, so every group
    # is re-probed here after the grace period and the escalation.  await_process_exit returns 0
    # only when `kill -0` on all of them has actually started failing, and this last per-group
    # probe is what turns "we sent SIGKILL" into "there is nothing left in these groups" for the
    # log -- and what keeps the log from naming a group it did not look at.
    for pgid in $live; do
        if kill -0 -- "-$pgid" 2>/dev/null; then
            survivors="${survivors:+$survivors }$pgid"
        fi
    done
    if [ -n "$survivors" ]; then
        warn "process group(s) $survivors STILL EXIST after SIG$signal and SIGKILL."
        warn "  Something in them is unkillable (a task blocked in the kernel, most likely in a"
        warn "  binder ioctl).  On invocation E that process is still holding the \"HdmiCec\""
        warn "  service name, which is fixed in production code and cannot be worked around:"
        warn "  no later AIDL invocation on this host will resolve correctly until it is gone."
        warn "  This run therefore CANNOT report success: it is leaving a process behind that"
        warn "  changes the result of the next run on this host.  Kill it by hand -- the group"
        warn "  id(s) above are the whole handle you need -- before starting another invocation."
        # The whole custody record is left set on purpose.  See the return contract above: it is
        # the record for groups that are still alive, and the EXIT trap's own call is the retry.
        return 1
    fi
    # NAMING ONLY WHAT WAS PROBED.  Every id in $registered was probed by the loop above -- the
    # live ones after the signal and the escalation, the rest at the pre-signal probe -- so the
    # claim covers exactly the set this function looked at and nothing wider.  It says how many
    # there were, because a reader who sees two here has learned something about the harness.
    if [ "$count" -eq 1 ]; then
        log "suite process group $SUITE_PGID is confirmed gone"
    else
        log "all $count suite process groups are confirmed gone: $registered"
    fi
    SUITE_PGID=''
    SUITE_EXTRA_PGIDS=''
    return 0
}

# ------------------------------------------------------------------------------------
# Close out an invocation's process group ON THE SUCCESS PATH, and account for anything that
# outlived the runner.
#
# WHY THE SUCCESS PATH NEEDS THIS AT ALL.  `wait "$SUITE_PGID"` waits for the subshell LEADER --
# that is `timeout`, and through it the test binary.  It says nothing about the rest of the
# group, and nothing whatever about the groups BELOW it.  The L2 harness forks the fake service
# host from inside that group and makes it a group leader in its own right (see THE CANCELLATION
# block above for the measured shape and why the harness does it), and if a teardown path in the
# harness ever fails to reap it, the runner exits 0 while the host keeps running and keeps the
# global "HdmiCec" registration.  The next AIDL invocation then resolves against a process from a
# previous run: not a crash, but a silently wrong selection, which is the worst shape a false
# green can take here.  So EVERY group in the custody record is closed out explicitly even when
# everything passed -- a check that could only see the runner's own group would report an empty
# group and a clean invocation while the host it was looking for ran on in a group of its own --
# and a survivor is REPORTED rather than quietly cleaned up: a harness that leaks its host is a
# defect worth someone's attention, and this is the only place it surfaces.
#
# A DETECTED SURVIVOR FAILS THE INVOCATION, WHICH IS THE POINT OF THE FUNCTION.  Reporting
# it and returning 0 would let the run continue, later invocations run against a process from
# this one, and the coverage verdict be issued as though nothing had happened.  Nothing
# downstream can distinguish "the harness reaped its host" from "the harness leaked it and this
# script cleaned up after it", so the two cannot share an outcome -- the second is a failed
# invocation whether or not the cleanup then worked, because the evidence the invocation
# produced was gathered with a process alive that the harness believed it had stopped.
#
# RETURN CONTRACT:
#   0 -- every group in the custody record was empty at close-out (the ordinary case).  Both
#        SUITE_PGID and SUITE_EXTRA_PGIDS are cleared.
#   1 -- a survivor was found in one of them and this function extinguished it.  The invocation
#        FAILS; the host is gone, so the next invocation is not endangered, but the harness
#        leaked it.
#   2 -- a survivor was found and could NOT be extinguished.  The invocation fails and the
#        surviving process still holds whatever it registered; the whole custody record stays
#        populated, descendant group ids included, because those ids are the only handle left on
#        a process whose parents have already exited.
# The caller distinguishes 1 from 2 in its message, because the two need different actions from
# whoever reads the log: fix the harness, versus fix the harness AND kill something by hand
# before the next run on this host means anything.
#
# Idempotent and cheap in the ordinary case: the groups are already empty, so this is one
# `kill -0` per registered group -- one, on every invocation where the harness created no group
# of its own -- plus one walk over /proc, which forks nothing.  The walk is what makes the
# assertion cover the same set stop_suite_group would signal; without it this function would
# assert emptiness of the runner's group and call an invocation clean while a leaked host ran on
# in a group it never looked at.
# ------------------------------------------------------------------------------------
finish_suite_group() { # $1 = invocation label, for the message
    local label="${1:-?}"
    local registered live='' pgid count=0 live_count=0

    registered="$(suite_custody_pgids)"
    [ -n "$registered" ] || return 0

    # ENUMERATED BEFORE IT IS PROBED, for the same reason stop_suite_group does it: whatever is
    # still reachable from this run's tree has to be found while the links are there.  By this
    # point `wait` has collected the subshell, so the seeding by group membership -- rather than
    # from the root pid alone -- is what still finds anything at all; a group whose entire
    # ancestor chain has already exited is beyond the reach of any walk, and on this path it is
    # the harness's own teardown, signalling its host's group by the id it read back after the
    # fork, that is responsible for it.
    suite_capture_descendant_groups
    registered="$(suite_custody_pgids)"
    for pgid in $registered; do
        count=$((count + 1))
        if kill -0 -- "-$pgid" 2>/dev/null; then
            live="${live:+$live }$pgid"
            live_count=$((live_count + 1))
        fi
    done
    if [ -z "$live" ]; then
        SUITE_PGID=''
        SUITE_EXTRA_PGIDS=''
        return 0
    fi

    if [ "$count" -eq 1 ]; then
        warn "invocation $label finished, but its process group $SUITE_PGID is NOT EMPTY."
    else
        warn "invocation $label finished, but $live_count of its $count process groups are NOT"
        warn "  EMPTY: $live  (of $registered)."
    fi
    warn "  The runner exited and something it started is still alive -- on an L2 invocation"
    warn "  that is the fake service host, which the harness is supposed to terminate and reap"
    warn "  in its own teardown.  Terminating the group(s) here so nothing can outlive this run"
    warn "  and hold the \"HdmiCec\" service name, and FAILING the invocation because the evidence"
    warn "  it just produced was gathered with a process alive that the harness thought it had"
    warn "  stopped -- which is exactly the state the next invocation must not inherit."
    stop_suite_group TERM || return 2
    return 1
}

on_exit() {
    local rc=$?
    # Children first: removing the staging directory or the private lcov HOME while the suite is
    # still running is a cleanup that has to be done twice.  The signal that cancelled the run,
    # when there was one, is the signal forwarded on -- a run cancelled with SIGHUP should not
    # report that it sent SIGTERM.
    #
    # A GROUP THAT SURVIVES ITS SIGKILL MAKES THE RUN FAIL, AND CANNOT MASK A FAILURE THAT WAS
    # ALREADY THERE.  Those are two requirements, not one, and the `[ "$rc" -ne 0 ] || rc=1`
    # shape is what satisfies both: a run that measured cleanly cannot report success while
    # leaving a process behind that changes the next run's result, and a run that was cancelled
    # (130/143/129), that failed a gate (1) or that reached only an advisory verdict (3) keeps
    # its own status, because that status is the primary finding and the leak is a second one.
    # The explicit `||` is also structural: this trap runs under `set -e`, so a bare call whose
    # status can be non-zero would abort the rest of the trap -- the Makefile restore
    # included -- exactly when the tree most needs restoring.
    stop_suite_group "${SUITE_SIGNAL:-TERM}" || { [ "$rc" -ne 0 ] || rc=1; }
    # THE MAKEFILE RESTORE HANGS OFF THE TRAP, NOT OFF THE BUILD'S SUCCESS PATH, and that is
    # the reason it is here rather than at the end of do_build.  A build that fails half way
    # through configure has already had some of the six overwritten; a run cancelled with
    # Ctrl-C has too.  Hung off the success path, neither would be restored at all and both
    # would leave the caller holding generated content over their own source with no way back.
    # Restoring from the trap covers every exit path there is -- success, failure, gate failure
    # and cancellation -- with one mechanism and no special cases.  It runs BEFORE the snapshot
    # is removed, and
    # both are no-ops when no snapshot was ever taken (any invocation without --build).
    #
    # A FAILED RESTORE CHANGES THE RUN'S EXIT STATUS, and `return` from an EXIT trap is what
    # does it.  Measured on this host's bash: a trap that `return N`s sets the script's status
    # to N (probe: body `exit 0`, trap `return 1` -> script exits 1; body `exit 5`, trap
    # `return 3` -> script exits 3).  So the `|| rc=1` below is not decorative -- a run that
    # measured cleanly but left the tree half restored cannot report success, because the tree
    # is then a mixture of pre-build and generated content and must not be committed.
    #
    # cleanup_makefile_snapshot RUNS EITHER WAY and decides for itself whether to remove the
    # snapshot: a failed restore sets MAKEFILE_SNAPSHOT_RETAIN and the snapshot is kept and
    # named, because it is the only remaining copy of the pre-build content.
    restore_regenerated_makefiles_from_snapshot || rc=1
    cleanup_makefile_snapshot
    cleanup_stage_dir
    cleanup_listing_gcov_dir
    cleanup_lcov_home
    # THE MARKER IS REWRITTEN HERE AND NOWHERE ELSE, so that one line of code covers every exit
    # path there is: success, a die anywhere above, a failed gate, and a cancellation through
    # on_signal (which exits rather than re-raising precisely so that this trap runs).  It is
    # written AFTER the restore, because a restore that failed changes the status this run must
    # report, and BEFORE the locks are released, so no second run can start while the directory
    # still claims to be IN-PROGRESS.
    #
    # AND THE VERDICT IS ONLY REPORTED IF IT WAS PUBLISHED.  A run that passed every gate and
    # could not record that fact has not produced the evidence it exists to produce, so its
    # status becomes 1 -- which `return "$rc"` below turns into the script's own exit status,
    # measured on this host's bash.  Two things make that safe rather than merely strict.  The
    # marker on disk at that moment cannot be another generation's verdict: the purge supersedes
    # or removes the prior one and prepare_output_dir refuses to run without publishing this
    # run's IN-PROGRESS, so what survives a failed final write is at worst THIS run's own
    # IN-PROGRESS or PURGING marker -- a same-run, conservative "did not reach its end", which is
    # exactly what a reader should conclude.  And the conservative write is attempted anyway,
    # because a failure that was transient (a full filesystem that has since drained, a content
    # composition that failed) is worth one more try at the honest answer.
    #
    # A FAILURE ON THE OTHER TWO PATHS CHANGES NOTHING, deliberately.  A cancelled run reports
    # the cancellation and a failed run reports its failure: those statuses are already correct
    # and already non-zero, and overwriting them with 1 because a marker could not be written
    # would replace a specific finding with a vaguer one.  The unwritten marker is warned about
    # and the run's own status stands.
    if [ "$rc" -eq 0 ]; then
        if ! write_run_status 'COMPLETE-PASS' 'the run reached its end and every gate passed'; then
            warn "this run passed every gate and COULD NOT PUBLISH that verdict (see above)."
            warn "  It therefore reports FAILURE: the numbers were real, and nothing in"
            warn "  $OUTPUT_DIR"
            warn "  can be shown to belong to the run that produced them.  The marker left there"
            warn "  is this run's own earlier one, which says the run did not reach its end --"
            warn "  conservative and same-generation, never a previous run's verdict."
            rc=1
            write_run_status 'COMPLETE-FAIL' \
                'every gate passed but the COMPLETE-PASS marker could not be published; treat this bundle as unverified' \
                || warn "the conservative marker could not be published either, so the marker on" \
                        "disk is this run's earlier IN-PROGRESS or PURGING one -- same run, and" \
                        "still the conservative reading"
        fi
    elif [ -n "${SUITE_SIGNAL:-}" ]; then
        write_run_status 'CANCELLED' "the run was cancelled by SIG${SUITE_SIGNAL}; exit status $rc" \
            || warn "the CANCELLED marker could not be published; this run reports the" \
                    "cancellation (status $rc) regardless, and the marker on disk is this run's" \
                    "earlier one rather than any previous run's verdict"
    else
        write_run_status 'COMPLETE-FAIL' "the run reached its end and reported failure; exit status $rc" \
            || warn "the COMPLETE-FAIL marker could not be published; this run reports its" \
                    "failure (status $rc) regardless, and the marker on disk is this run's" \
                    "earlier one rather than any previous run's verdict"
    fi
    # LAST, and after the restore rather than before it.  The restore and the snapshot removal
    # are themselves work on this tree, so releasing the lock any earlier would let a second
    # run start while the six tracked Makefiles are half restored -- which is the exact
    # interleaving the lock exists to prevent, moved into the cleanup where it would be
    # hardest to see.
    release_tree_lock
    return "$rc"
}

# The signal handler `exit`s rather than re-raising, because a shell terminated by a signal with
# its default disposition never runs its EXIT trap: re-raising would skip the staging
# cleanup, the private lcov HOME and the suite itself.  `exit 130/143/129` reports the
# same status a signalled shell would while guaranteeing the handler runs.
on_signal() { # $1 = signal name  $2 = exit status
    SUITE_SIGNAL="$1"
    warn "received SIG$1 -- cancelling this run"
    # THE CANCELLATION STATUS IS AUTHORITATIVE HERE, so a group that could not be extinguished
    # is reported and the handler still exits with the signal's own status.  The `||` is not
    # optional: under `set -e` a non-zero return from this call would end the shell with THAT
    # status instead of reaching `exit "$2"`, and the run would report 1 for a cancellation.
    # The EXIT trap calls stop_suite_group again -- the custody record, SUITE_PGID and the
    # descendant group ids alike, is deliberately still populated when extinction failed -- and
    # folds the survivor into the status there, where it cannot overwrite the 130/143/129 this
    # line is about to set.  The retry matters most for the descendant ids: the walk that found
    # them cannot re-derive them once this handler's signal has removed their parents, so the
    # record carried forward from this call is the only thing that still addresses them.
    stop_suite_group "$1" \
        || warn "a process from this run survived the cancellation; it is named above and this" \
                "run still reports the cancellation as its primary status"
    exit "$2"
}
trap on_exit EXIT
trap 'on_signal INT 130' INT
trap 'on_signal TERM 143' TERM
trap 'on_signal HUP 129' HUP


# ------------------------------------------------------------------------------------
# Step 0 -- give lcov a private HOME, and never touch the caller's.
#
# lcov reads $HOME/.lcovrc silently on EVERY invocation, --version included, and a copy
# there that disables branch collection would quietly defeat the whole point of this
# script -- CI plants exactly such a copy in the plugin jobs, so the hazard is real and
# not hypothetical.  What this step needs is only that NO home configuration is in effect
# while lcov runs.
#
# The obvious ways to get that are both wrong.  `rm -f ~/.lcovrc` destroys a developer's
# file as a side effect nobody asked for.  Moving it aside and moving it back is worse than
# it looks, and must not be introduced here: the restoring `mv -f` overwrites whatever
# stands at $HOME/.lcovrc at that moment, so a file that REAPPEARED during the run -- the
# owner recreating it, or a sibling coverage runner in this workspace if one were ever
# changed to write there -- would be silently destroyed by a script whose only job is to
# measure; and until the restore, a file recreated mid-run is in effect for every lcov
# call after it, which is the very thing the stash existed to prevent.
#
# So the caller's HOME is left completely alone and lcov is given one of its own: an empty
# mode-0700 directory that contains no .lcovrc and never will.  Every lcov and genhtml
# invocation in this script goes through lcov_run/genhtml_run below, which set HOME to it.
# There is nothing to restore, nothing to race, and no trap needed to undo it -- the
# directory is removed on exit and that is all.
#
# /etc/lcovrc is never touched either.  Together with --config-file that leaves lcov
# reading exactly one configuration file -- this suite's versioned one -- plus the --rc
# overrides, which is what makes the run reproducible on any host.
# ------------------------------------------------------------------------------------
make_private_lcov_home() {
    # The parent comes from select_safe_temp_parent, not from "${TMPDIR:-/tmp}": a fallback
    # to /tmp would put lcov's configuration directory below a world-writable directory on this
    # host (measured mode 2777, no sticky bit).  The chosen parent has already been proved
    # safe at `named` strictness, so the ancestry check below is a re-assertion at the moment
    # of use rather than the first check -- the posture every custody path in this
    # script takes.
    local parent
    select_safe_temp_parent
    parent="$RUNNER_TEMP_PARENT"
    assert_safe_ancestry "$parent/hdmicec-lcov-home" minted

    LCOV_HOME="$("$MKTEMP_BIN" -d "$parent/hdmicec-lcov-home.XXXXXXXX")" \
        || die "could not create a private HOME for lcov under $parent.
       Refusing to run lcov with the caller's home configuration in effect, and refusing
       to delete or move the caller's ~/.lcovrc to get around it."
    chmod 700 -- "$LCOV_HOME" || die "could not restrict the private lcov HOME to mode 0700: $LCOV_HOME"

    # mktemp -d created it, so it is empty and owned by this user; assert both rather than
    # assume them, because everything downstream trusts this directory to hold no
    # configuration.
    assert_private_dir "$LCOV_HOME"
    if [ -e "$LCOV_HOME/.lcovrc" ] || [ -L "$LCOV_HOME/.lcovrc" ]; then
        die "the private lcov HOME already contains a .lcovrc: $LCOV_HOME/.lcovrc
       mktemp -d had just created that directory, so something raced this run.  Refusing to
       run lcov against a configuration file this script did not put there."
    fi
    # Recorded so cleanup_lcov_home's recursive remove can prove it is deleting this run's
    # own directory rather than whatever the name points at by then.
    LCOV_HOME_IDENTITY="$(path_identity "$LCOV_HOME")"
    [ -n "$LCOV_HOME_IDENTITY" ] || die "could not read the identity (device:inode) of the
       private lcov HOME: $LCOV_HOME"
    log "lcov runs with a private empty HOME: $LCOV_HOME (your \$HOME is not read or written)"
}

# ------------------------------------------------------------------------------------
# WHERE THE --gtest_list_tests LAUNCHES' COUNTERS GO, and why they must go somewhere.
#
# THE DEFECT THIS EXISTS TO CLOSE, measured on this host before it did.  do_run zeroes the
# counters, proves the tree holds no .gcda, and then runs the matrix -- but the first thing
# each invocation does is ASK THE BINARY WHAT IT REGISTERS, three or four times, through
# registered_test_count's `--gtest_list_tests` launch.  A gcov-instrumented process writes its
# .gcda on exit whatever it did, so those listings deposited counters into the object tree
# AFTER the zeroing and BEFORE the first measured case: 32 .gcda files, 347 hit lines and 68
# hit arcs, all of them describing static initialisation and the listing path rather than a
# test.  Every figure the capture then reported included them.  Production hit lines happened
# to be zero, which made the contamination easy to overlook and did not make it harmless: the
# counters accumulate, so any line the listing path ever reaches is credited to the matrix.
#
# WHY DIVERSION RATHER THAN REORDERING.  Moving the listings ahead of the zeroing was the other
# candidate and it is the weaker fix: the counts are read PER INVOCATION with that invocation's
# own filters, deliberately (see registered_test_count), so hoisting them out of the loop would
# either re-order the evidence away from the invocation it belongs to or leave the listings
# inside the loop and contaminating from the second invocation onwards.  GCOV_PREFIX is the
# mechanism libgcov itself provides for exactly this: the process still writes its counters, it
# writes them somewhere this run then discards, and the object tree is untouched.
#
# GCOV_PREFIX_STRIP IS NOT OPTIONAL WITH IT.  GCOV_PREFIX alone PREPENDS to the object file's
# full absolute path, so the sink would mirror the whole path from '/' downwards -- on this host
# a seven-component tree per .gcda, for files nobody will read.  Stripping exactly the depth of
# the submodule root reproduces the tree from ccec/... downwards and nothing above it.  The
# depth is COMPUTED from the root this run resolved rather than written down, because a clone at
# a different path has a different depth and a wrong strip count is silent: it just makes the
# sink deeper.
#
# ONE SINK PER RUN, CREATED IN THE MAIN SHELL.  registered_test_count is always read through a
# command substitution, so a global it assigned would die with that subshell and the directory
# would be re-minted and leaked on every call.  do_run therefore creates it once, before the
# matrix, and this function is what it calls; the launch site only READS the two values.
# ------------------------------------------------------------------------------------
make_listing_counter_sink() {
    [ -z "$LISTING_GCOV_DIR" ] || return 0

    local parent depth
    select_safe_temp_parent
    parent="$RUNNER_TEMP_PARENT"
    assert_safe_ancestry "$parent/hdmicec-listing-gcda" minted

    LISTING_GCOV_DIR="$("$MKTEMP_BIN" -d "$parent/hdmicec-listing-gcda.XXXXXXXX")" \
        || die "could not create the directory this run diverts the --gtest_list_tests
       launches' gcov counters into, under $parent.
       Refusing to run the matrix without it: those launches would then write their .gcda
       into the object tree between the zeroing and the first measured case, and every
       coverage figure this run reported would include execution no test performed."
    chmod 700 -- "$LISTING_GCOV_DIR" \
        || die "could not restrict the diverted listing-counter directory to mode 0700: $LISTING_GCOV_DIR"
    assert_private_dir "$LISTING_GCOV_DIR"

    # Recorded so cleanup_listing_gcov_dir's recursive remove can prove it is deleting this
    # run's own directory rather than whatever the name points at by then.
    LISTING_GCOV_DIR_IDENTITY="$(path_identity "$LISTING_GCOV_DIR")"
    [ -n "$LISTING_GCOV_DIR_IDENTITY" ] || die "could not read the identity (device:inode) of the
       diverted listing-counter directory: $LISTING_GCOV_DIR"

    # The submodule root's own depth, so the mirrored tree starts at the first path component
    # BELOW it.  ${HDMICEC_ROOT#/} drops the leading slash, which would otherwise be counted as
    # an empty first field.
    depth="$(printf '%s\n' "${HDMICEC_ROOT#/}" | "$AWK_BIN" -F/ '{ print NF; exit }')"
    case "$depth" in
        ''|*[!0-9]*) die "could not compute the GCOV_PREFIX_STRIP depth of $HDMICEC_ROOT
       (read back as '${depth:-<empty>}').  Refusing to divert the listing counters with a
       strip count this script cannot state, because a wrong one is silent." ;;
    esac
    LISTING_GCOV_PREFIX_STRIP="$depth"

    log "the --gtest_list_tests launches write their counters to $LISTING_GCOV_DIR"
    log "  (GCOV_PREFIX with GCOV_PREFIX_STRIP=$LISTING_GCOV_PREFIX_STRIP; discarded on exit, so"
    log "   no listing-only execution reaches the tree the capture reads)"
}

# ------------------------------------------------------------------------------------
# THE INVARIANT THE DIVERSION ABOVE EXISTS TO ESTABLISH, ASSERTED RATHER THAN TRUSTED.
#
# The diversion fixes the launches that exist today.  This is what makes the defect unable to
# come back: it is checked immediately before the FIRST measured suite launch, so ANY future
# launch of an instrumented binary added between do_run's zeroing and that point -- another
# listing, a version probe, a smoke check -- is caught by the counters it leaves behind rather
# than by somebody re-reading this file.
#
# ONCE, not before every invocation, and that is the correct scope rather than a saving.  From
# the second invocation onwards the tree legitimately holds the counters of every invocation
# before it: accumulation across the matrix into a single capture is the whole design (see ONE
# BUILD, ONE MACHINE), so a zero test there would fail on the very behaviour it is protecting.
#
# It reuses gcda_count rather than counting again, so there is one definition of "how many
# counter files are under this tree" and no second walk to keep in step with it.
# ------------------------------------------------------------------------------------
FIRST_MEASURED_LAUNCH_SEEN=0
assert_no_counters_before_first_case() { # $1=invocation label, for the message
    local label="$1" present offenders
    [ "$FIRST_MEASURED_LAUNCH_SEEN" -eq 0 ] || return 0
    FIRST_MEASURED_LAUNCH_SEEN=1

    present="$(gcda_count)"
    [ "$present" -eq 0 ] || {
        offenders="$( { "$FIND_BIN" "$HDMICEC_ROOT" -name '*.gcda' -type f 2>/dev/null || true; } \
                      | head -n 10 | sed 's/^/         /')"
        die "$present .gcda counter file(s) exist under
       $HDMICEC_ROOT
       BEFORE invocation $label -- the first measured case -- has run, and after do_run
       proved the tree held none.  Something between the zeroing and this launch executed
       instrumented code, so the counters this run is about to accumulate into are not
       exclusively the matrix's and no figure derived from them could be attributed to it.
       The known source of this was registered_test_count's --gtest_list_tests launches,
       which are diverted with GCOV_PREFIX (see make_listing_counter_sink); a count here
       means a NEW launch has appeared that is not diverted.  Give it the same diversion.
       First offenders:
$offenders"
    }
    log "counters verified still zero immediately before invocation $label's launch: the"
    log "  matrix is the only thing that will have written to them"
}

# Every lcov and genhtml invocation in this script goes through these two wrappers, and
# HOME is the only reason they exist.  Setting it per-command rather than exporting it for
# the whole script is deliberate: the suite under test, the compiler and git all run from
# here too, and none of them should have its HOME rewritten by a coverage script.
lcov_run() {
    [ -n "$LCOV_HOME" ] || die "internal error: lcov_run called before the private HOME was created"
    HOME="$LCOV_HOME" "$LCOV_BIN" "$@"
}
genhtml_run() {
    [ -n "$LCOV_HOME" ] || die "internal error: genhtml_run called before the private HOME was created"
    HOME="$LCOV_HOME" "$GENHTML_BIN" "$@"
}


# ------------------------------------------------------------------------------------
# Words the reference audit must NOT treat as a command or as a shell variable, each
# with the reason it is not one.  The scanner reads shell text with grep, so it cannot
# distinguish a shell command from a word inside a single-quoted awk program, a heredoc
# or a filename literal; this list is the complete set of such words in this script and
# anything not listed has to resolve.  Keep it short -- a growing list is the sign that
# the audit is being worked around rather than satisfied.
#   br_cell            awk-local in the per-file table program (write_per_file_tsv / per_file_report)
#   func_cell          awk-local in the per-file table program
#   line_cell          awk-local in the per-file table program
# ------------------------------------------------------------------------------------
SELFTEST_NOT_A_COMMAND='br_cell
func_cell
line_cell'
readonly SELFTEST_NOT_A_COMMAND

# ------------------------------------------------------------------------------------
# SELF-REFERENCE AUDIT, and the `selftest` subcommand built on it.
#
# WHY THIS EXISTS.  This script is long, it runs under `set -euo pipefail`, and bash resolves
# a command name only when control reaches it.  A call to a function that does not exist, or an
# expansion of a variable that was never defined, is therefore invisible to `bash -n`, invisible
# to shellcheck, and invisible until the run is already under way -- at which point it aborts
# with `command not found` or `unbound variable` after the banner has printed, having produced
# no measurement.  Defects of exactly that shape are what this audit is for, and they are not
# hypothetical in a runner of this size: a call to a `prepare_lcov_home` that nothing defines,
# sitting beside wrappers built over an undefined `LCOV_HOME_DIR`; or a per-file gate that
# expands an undefined `COVERAGE_GATE_EXEMPT_FILES` the moment any file falls below the bar.
#
# WHAT IT CHECKS.  Two things, statically, over this script's own text:
#   1. Every snake_case word in a command position resolves -- to a function defined here, a
#      variable this script assigns, a shell builtin, or an executable on PATH.
#   2. Every ${UPPER_CASE} expansion is either assigned by this script, written with a default
#      (${X:-...}, ${X-...}, ${X+...}), or already exported in the environment.
#
# THE ONE BLIND SPOT, NAMED RATHER THAN GLOSSED OVER.  The scanner reads shell text with grep,
# so it cannot tell a shell command from a word inside a single-quoted awk program, a
# heredoc, or a filename literal.  Those tokens are listed in SELFTEST_NOT_A_COMMAND below,
# each with the reason it is not a call.  Everything else must resolve; the list is the
# complete set of exceptions and a reader can check every entry against the source.
#
# WHEN IT RUNS.  Always, as main()'s first act, before any validation, any filesystem write and
# any lcov invocation -- so a script that cannot resolve its own references reports exactly
# that and touches nothing.  `selftest` runs the audit plus the tooling pre-flight and stops
# there: no suite, no counters zeroed, no capture, no artifact directory.
# ------------------------------------------------------------------------------------
reference_audit() {
    local body defined assigned locals called globals defaulted setglob w failures=0
    [ -r "$SCRIPT_PATH" ] || die "cannot read this script back for the reference audit: $SCRIPT_PATH"

    body="$(grep -vE '^[[:space:]]*#' -- "$SCRIPT_PATH" || true)"
    defined="$(grep -oE '^[a-zA-Z_][a-zA-Z0-9_]*\(\)' -- "$SCRIPT_PATH" | tr -d '()' | sort -u)"
    assigned="$(printf '%s\n' "$body" | grep -oE '(^|[[:space:]]|\(|;)[a-z][a-z0-9_]*=' | grep -oE '[a-z][a-z0-9_]*' | sort -u)"
    locals="$(printf '%s\n' "$body" | grep -oE '\b(local|read -r|read)[[:space:]]+([a-z][a-z0-9_]*[[:space:]]*)+' | grep -oE '[a-z][a-z0-9_]*' | sort -u)"
    called="$(printf '%s\n' "$body" \
        | grep -oE '(^|[[:space:]]|;|\||&|\(|!)[[:space:]]*[a-z][a-z0-9]*(_[a-z0-9]+)+([[:space:]]|$|\))' \
        | grep -oE '[a-z][a-z0-9]*(_[a-z0-9]+)+' | sort -u)"

    for w in $called; do
        printf '%s\n' "$defined"                | grep -qx -- "$w" && continue
        printf '%s\n' "$assigned"               | grep -qx -- "$w" && continue
        printf '%s\n' "$locals"                 | grep -qx -- "$w" && continue
        printf '%s\n' "$SELFTEST_NOT_A_COMMAND" | grep -qx -- "$w" && continue
        command -v -- "$w" >/dev/null 2>&1 && continue
        warn "reference audit: '$w' is used in a command position but is not a function defined"
        warn "    here, not a variable this script assigns, not a builtin and not on PATH."
        failures=$((failures + 1))
    done

    globals="$(printf '%s\n' "$body" | grep -oE '\$\{?[A-Z][A-Z0-9_]*' | grep -oE '[A-Z][A-Z0-9_]*' | sort -u)"
    defaulted="$(printf '%s\n' "$body" | grep -oE '\$\{[A-Z][A-Z0-9_]*[:+-]' | grep -oE '[A-Z][A-Z0-9_]*' | sort -u)"
    setglob="$(grep -oE '^[[:space:]]*(export[[:space:]]+|readonly[[:space:]]+|local[[:space:]]+|declare[[:space:]]+-[a-zA-Z]+[[:space:]]+)?[A-Z][A-Z0-9_]*(=|\+=|\()' -- "$SCRIPT_PATH" \
        | grep -oE '[A-Z][A-Z0-9_]*' | sort -u)"
    for w in $globals; do
        printf '%s\n' "$setglob"                | grep -qx -- "$w" && continue
        printf '%s\n' "$defaulted"              | grep -qx -- "$w" && continue
        printf '%s\n' "$SELFTEST_NOT_A_COMMAND" | grep -qx -- "$w" && continue
        [ -n "${!w+x}" ] && continue
        warn "reference audit: \$$w is expanded but is never assigned here, has no default form"
        warn "    (\${$w:-...}) and is not set in the environment; under 'set -u' that aborts the run."
        failures=$((failures + 1))
    done

    # STAGE 3: a reference written WITH a default but never assigned anywhere.
    #
    # Stage 2 deliberately accepts \${X:-...} because it cannot abort under 'set -u'.  That is
    # exactly the shape a defect slips through in: a runner that reads \${STAT_BIN:-} in
    # path_metadata() while never assigning STAT_BIN has a permanently empty guard, and every run
    # dies at the first artifact-ancestry check with "stat was not found on PATH" while
    # /usr/bin/stat is on PATH all along.  Safe from 'set -u', and wrong in every run - so the
    # default form is audited too, not treated as proof of resolution.
    #
    # Names that are genuinely read from the environment and are MEANT to be unassigned here are
    # listed below with the reason.  Every other tool handle, path and tunable this script uses is
    # assigned in one place, so the list stays short by construction.
    #   TMPDIR       - a standard environment variable, read by consider_temp_parent as the
    #                  first candidate parent for this run's private directories
    #   X            - not a variable: it appears as \${X:-...} inside this audit's own
    #                  explanatory comment, describing the default FORM rather than naming a
    #                  real reference
    #   CLONE_INDEX  - set by the workspace environment to distinguish parallel clones.  Read
    #                  only by write_provenance, and only to RECORD which clone produced an
    #                  artifact; "unset" is a truthful value there, so an empty read is correct
    #                  behaviour rather than a silent failure
    #   CXX          - the standard compiler environment variable.  write_provenance reads it to
    #                  record which compiler produced the instrumentation, falling back to g++,
    #                  which is the same convention the build itself uses
    for w in $defaulted; do
        printf '%s\n' "$setglob" | grep -qx -- "$w" && continue
    #   XDG_RUNTIME_DIR - the per-user runtime directory a systemd host provides at mode 0700.
    #                  Read only by select_safe_temp_parent, as the second candidate parent for
    #                  this run's private directories; unset is the normal case in a container
    #                  and is handled by moving on to the next candidate
    #   HOME         - the standard environment variable, and the last-resort candidate parent.
    #                  Never written: lcov and genhtml are given a private HOME of their own by
    #                  lcov_run/genhtml_run, which is a per-command assignment and not an
    #                  assignment to this shell's HOME
        case " $w " in
            " TMPDIR " | " X " | " CLONE_INDEX " | " CXX " | " XDG_RUNTIME_DIR " | " HOME ") continue ;;
        esac
        warn "reference audit: \$$w is only ever read with a default (\${$w:-...}) and is never"
        warn "    assigned by this script.  It cannot abort the run, so it will silently be empty"
        warn "    every time - which makes whatever depends on it dead or permanently failing."
        warn "    Either assign it, or add it to this stage's environment-only list with a reason."
        failures=$((failures + 1))
    done

    if [ "$failures" -ne 0 ]; then
        die "the reference audit found $failures unresolved name(s) in this script.
       Every one of them would abort a real run part-way through, after the banner and
       before any measurement.  Fix the name, or -- if it is genuinely not a shell
       command (a word inside an embedded awk program, a heredoc, or a filename) -- add
       it to SELFTEST_NOT_A_COMMAND with the reason."
    fi
    return 0
}

# ------------------------------------------------------------------------------------
# THE RENDERER'S OWN SELF-TEST.  A table of values and the exact rendering each must
# produce, so that a change to render_untrusted() that weakened any clause of the
# contract fails here rather than in a log nobody reads.
#
# The cases are the ways the defect presented itself in the finding: a value carrying a
# NEWLINE followed by a forged "::error::" workflow command, a value carrying a CARRIAGE
# RETURN (which redraws a line rather than ending it, so it HIDES output instead of
# forging it), a value carrying an ANSI ESCAPE SEQUENCE, an EMPTY value, a value that is
# ITSELF nothing but a forged annotation, and a value long enough to drown the log.  Each
# expected string is spelled out in full rather than recomputed with the code under test.
#
# Three properties are asserted for EVERY row, on top of the exact match, because they are
# what the contract actually promises and an exact-match table alone would let a rewrite
# satisfy by accident:
#
#   * the rendering is ONE LINE - it contains no 0x0A and no 0x0D, so it cannot end the
#     diagnostic it sits in and cannot begin a line of its own;
#   * it BEGINS with '[' - so even a value that is entirely "::error::something" appears
#     as [::error::something], which is not a workflow command;
#   * it is BOUNDED - never longer than the limit plus the truncation note and the two
#     delimiters, whatever the input length.
#
# No suite is run, nothing is captured and nothing is written: this is a pure function fed
# a table.  It runs before require_tools, so it uses no external command.
# ------------------------------------------------------------------------------------
render_untrusted_selftest() {
    local failures=0 rendered expected value name rest record
    local long_value long_expected

    long_value=''
    while [ "${#long_value}" -lt 5000 ]; do
        long_value+='XXXXXXXXXXXXXXXXXXXX'
    done
    long_expected='['
    while [ "${#long_expected}" -lt 201 ]; do
        long_expected+='XXXXXXXXXXXXXXXXXXXX'
    done
    long_expected+='...[truncated, 5000 bytes total]]'

    # name | value | expected rendering
    local -a render_cases=(
        "newline-and-forged-annotation|$(printf 'bogus\n::error::FORGED')|[bogus\\n::error::FORGED]"
        "carriage-return-erase-line|$(printf 'ok\r\033[2Khidden')|[ok\\r\\x1b[2Khidden]"
        "ansi-colour-escape|$(printf '\033[31mred\033[0m')|[\\x1b[31mred\\x1b[0m]"
        "tab-and-literal-backslash|$(printf 'a\tb\\nc')|[a\\tb\\\\nc]"
        "empty-value||[]"
        "leading-forged-annotation|::error::WHOLE_VALUE|[::error::WHOLE_VALUE]"
        "non-ascii-utf8|$(printf '\303\251')|[\\xc3\\xa9]"
        "high-byte|$(printf '\377')|[\\xff]"
    )

    for record in "${render_cases[@]}"; do
        name="${record%%|*}"
        rest="${record#*|}"
        value="${rest%|*}"
        expected="${rest##*|}"

        rendered="$(render_untrusted "$value")"

        if [ "$rendered" != "$expected" ]; then
            warn "renderer self-test: $name rendered as '$rendered', expected '$expected'."
            failures=$((failures + 1))
        fi
        case "$rendered" in
            '['*) : ;;
            *)  warn "renderer self-test: $name does not begin with '[', so it could begin a line."
                failures=$((failures + 1)) ;;
        esac
        case "$rendered" in
            *$'\n'*|*$'\r'*)
                warn "renderer self-test: $name still contains a line terminator."
                failures=$((failures + 1)) ;;
        esac
        if [ "${#rendered}" -gt 240 ]; then
            warn "renderer self-test: $name rendered ${#rendered} characters, which is unbounded."
            failures=$((failures + 1))
        fi
    done

    rendered="$(render_untrusted "$long_value")"
    if [ "$rendered" != "$long_expected" ]; then
        warn "renderer self-test: a 5000-byte value rendered ${#rendered} characters, and not the"
        warn "  expected 200 plus the truncation note."
        failures=$((failures + 1))
    fi

    [ "$failures" -eq 0 ] || die "the untrusted-value renderer failed $failures check(s).
       Every diagnostic in this script that names a value the caller supplied goes through
       render_untrusted(), and the five-clause contract documented on it is what makes a forged
       standalone ::error:: line unreachable and a diagnostic bounded.  A renderer that does not
       hold that contract must not be shipped: fix render_untrusted() rather than this table.
       The same contract is implemented in four other files -- see the cross-reference on
       render_untrusted() -- and a change here belongs in all five."
}

# ------------------------------------------------------------------------------------
# THE LOADER GUARD'S OWN SELF-TEST.  Five things are checked, and the fourth is the one that
# matters: the guard is exercised END TO END, by re-invoking this script with a direct-object
# loader variable set and asserting that it refuses.  A unit test of the predicate would
# prove the predicate; only a re-invocation proves that the refusal is actually WIRED, and
# wiring is what the finding was about -- the variables were documented as out of scope and
# nothing acted on them.
#
# The re-invocation uses --help, so it is bounded, writes nothing, creates no artifact
# directory and cannot recurse.
# ------------------------------------------------------------------------------------
loader_guard_selftest() {
    local failures=0 name state rc out err

    # 1. THE SET IS NOT EMPTY AND NAMES THE TWO THE FINDING NAMED.  A future edit that
    #    emptied the array would otherwise leave every check below passing vacuously.
    case " ${LOADER_DIRECT_OBJECT_VARIABLES[*]} " in
        *' LD_PRELOAD '*) ;;
        *)  warn "loader guard self-test: LD_PRELOAD is not in LOADER_DIRECT_OBJECT_VARIABLES."
            failures=$((failures + 1)) ;;
    esac
    case " ${LOADER_DIRECT_OBJECT_VARIABLES[*]} " in
        *' LD_AUDIT '*) ;;
        *)  warn "loader guard self-test: LD_AUDIT is not in LOADER_DIRECT_OBJECT_VARIABLES."
            failures=$((failures + 1)) ;;
    esac

    # 2. THE STATE PREDICATE DISTINGUISHES THE THREE CASES, including the one `set -u` makes
    #    easy to get wrong: present-but-empty is neither "unset" nor "set".  Exercised on a
    #    variable of its own so that nothing in this process is disturbed.
    #
    #    THE TWO `shellcheck disable=SC2034` DIRECTIVES BELOW ARE NOT A SUPPRESSED DEFECT.
    #    CEC_LOADER_SELFTEST_PROBE is read three times -- on the next line after each
    #    assignment, and once more after the `unset -v` -- but every read goes through
    #    `loader_object_variable_state`, which takes the variable's NAME as its argument and
    #    dereferences it by INDIRECT EXPANSION (${!name+set} / ${!name}).  shellcheck cannot
    #    follow a name through indirection, so it reports the assignments as unused.  Passing
    #    the name rather than the value is the whole point: the predicate's contract is to
    #    distinguish unset from present-but-empty, and a value-passing signature could not
    #    express the difference at all.  Each directive is scoped to the one assignment below
    #    it rather than to the whole function, so a genuinely unused variable added to this
    #    self-test later is still reported.  This matters beyond tidiness: §9.5
    #    of the project guide runs `shellcheck -S warning` over this runner and documents
    #    "no findings" as the pass, so an accepted false positive here would either falsify
    #    that step or force it to be weakened.
    # shellcheck disable=SC2034
    CEC_LOADER_SELFTEST_PROBE='x'
    loader_object_variable_state CEC_LOADER_SELFTEST_PROBE
    state="$LOADER_OBJECT_VARIABLE_STATE"
    if [ "$state" != 'set' ]; then
        warn "loader guard self-test: a non-empty variable classified as '$state', not 'set'."
        failures=$((failures + 1))
    fi
    # shellcheck disable=SC2034
    CEC_LOADER_SELFTEST_PROBE=''
    loader_object_variable_state CEC_LOADER_SELFTEST_PROBE
    state="$LOADER_OBJECT_VARIABLE_STATE"
    if [ "$state" != 'empty' ]; then
        warn "loader guard self-test: an empty variable classified as '$state', not 'empty'."
        failures=$((failures + 1))
    fi
    unset -v CEC_LOADER_SELFTEST_PROBE
    loader_object_variable_state CEC_LOADER_SELFTEST_PROBE
    state="$LOADER_OBJECT_VARIABLE_STATE"
    if [ "$state" != 'unset' ]; then
        warn "loader guard self-test: an absent variable classified as '$state', not 'unset'."
        failures=$((failures + 1))
    fi

    # 3. THE SCRUB REMOVES RATHER THAN EMPTIES.  Emptying would leave LD_DYNAMIC_WEAK active,
    #    because glibc activates it on presence and ignores the value, so this asserts the
    #    state is 'unset' and not merely 'empty'.  Run in a subshell: the assignments must not
    #    survive into the rest of this run.
    if ! (
        export LD_PRELOAD='/nonexistent/selftest.so'
        export LD_DYNAMIC_WEAK='1'
        scrub_loader_object_variables
        for name in "${LOADER_DIRECT_OBJECT_VARIABLES[@]}"; do
            loader_object_variable_state "$name"
            [ "$LOADER_OBJECT_VARIABLE_STATE" = 'unset' ] || exit 1
        done
        exit 0
    ); then
        warn "loader guard self-test: scrub_loader_object_variables left a member of the set"
        warn "  present.  An emptied variable is not a removed one: LD_DYNAMIC_WEAK is active"
        warn "  on presence alone."
        failures=$((failures + 1))
    fi

    # 4. END TO END: THE REFUSAL IS WIRED.  --help is the cheapest complete invocation and is
    #    the reproduction the finding used, so this is that reproduction as a test.  stdout
    #    must be EMPTY: refusing and then printing the help text anyway would be a warning
    #    dressed as a refusal.
    out="$(LD_PRELOAD='/nonexistent/selftest.so' "$SCRIPT_PATH" --help 2>/dev/null)" && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        warn "loader guard self-test: '--help' succeeded with LD_PRELOAD set; it must refuse."
        failures=$((failures + 1))
    fi
    if [ -n "$out" ]; then
        warn "loader guard self-test: the refused invocation still wrote ${#out} bytes to stdout."
        failures=$((failures + 1))
    fi
    err="$(LD_AUDIT='/nonexistent/selftest.so' "$SCRIPT_PATH" --help 2>&1 >/dev/null)" || true
    case "$err" in
        *'refusing to run with'*'LD_AUDIT'*) ;;
        *)  warn "loader guard self-test: LD_AUDIT did not produce the refusal diagnostic."
            failures=$((failures + 1)) ;;
    esac

    # 5. A PRESENT-BUT-EMPTY MEMBER IS REMOVED RATHER THAN REFUSED, because refusing it would
    #    break a caller who exported it empty by accident and would gain nothing.
    if ! LD_PRELOAD='' "$SCRIPT_PATH" --help >/dev/null 2>&1; then
        warn "loader guard self-test: an EMPTY LD_PRELOAD was refused; it must be removed and"
        warn "  the run allowed to continue."
        failures=$((failures + 1))
    fi

    [ "$failures" -eq 0 ] || die "the direct-object loader guard failed $failures check(s).
       LD_PRELOAD, LD_AUDIT, LD_PROFILE, LD_DYNAMIC_WEAK and LD_ORIGIN_PATH name objects to
       load or change how symbols bind, so the loader-path rebuild cannot cover them; the
       guard is the only thing that does.  Measured before it existed: a preloaded object ran
       384 times during --help and 758 times during --selftest, in every process that produced
       a coverage figure.  A run whose guard does not hold cannot present its output as a
       measurement."
}

selftest() {
    rule
    log "SELF-TEST: no suite is run, no counters are zeroed, nothing is captured and no"
    log "  artifact directory is created."
    rule
    log "1/6 re-parsing this script"
    bash -n -- "$SCRIPT_PATH" || die "this script does not parse: $SCRIPT_PATH"
    log "    parses cleanly"
    log "2/6 auditing every internal function and variable reference"
    reference_audit
    log "    every reference resolves"
    log "3/6 threshold range guard, at both ends of the shell's integer range"
    threshold_selftest
    log "    every boundary value is classified correctly"
    log "4/6 untrusted-value renderer: CR, LF, ANSI, a forged ::error:: and an over-long value"
    render_untrusted_selftest
    log "    every value is escaped, bounded, single-line and cannot begin a line"
    log "5/6 direct-object loader guard, end to end"
    loader_guard_selftest
    log "    a run carrying LD_PRELOAD or LD_AUDIT is refused, and the scrub removes rather"
    log "      than empties"
    log "6/6 measurement tooling pre-flight"
    require_tools
    log "    tooling present and usable"
    rule
    log "SELF-TEST PASSED"
}

usage() {
    # The matrix is rendered into a variable BEFORE the heredoc rather than inside a command
    # substitution within it.  Two reasons, and the second is the load-bearing one: a heredoc
    # holding a loop is unreadable, and the reference audit reads this script's text with grep,
    # so a loop variable inside a heredoc looks exactly like a call to a command that does not
    # exist.  The audit reports that shape as a call to a missing definition, which is the audit
    # doing its job, so the loop is written where there is nothing to except rather than excepted.
    local matrix_summary='' record
    local label tier mode back_end select_filter exclude_filter synopsis permit_skip
    for record in "${INVOCATION_MATRIX[@]}"; do
        IFS='|' read -r label tier mode back_end select_filter exclude_filter synopsis permit_skip <<< "$record"
        matrix_summary="${matrix_summary}$(printf '  %s  %-3s CEC_TEST_AIDL_MODE=%-13s expects %-6s  %s' \
            "$label" "$tier" "$mode" "$back_end" "$synopsis")
"
    done

    # THE BINDER-NODE NOTE IS COMPUTED HERE RATHER THAN INSIDE THE HEREDOC, and the reason is
    # the same finding that reworked usage() in .github/workflows/aidl-path-tests-rootfs.sh.
    # That script's help text held a backtick-quoted word inside an UNQUOTED heredoc, so bash
    # performed COMMAND SUBSTITUTION while rendering help and executed a PATH-resolved `gcov`
    # -- measured, as root, with the attacker's stdout substituted into the help output.  The
    # heredoc below is unquoted for the same reason that one was: it interpolates a dozen
    # ${VARIABLE} values that a reader needs resolved.  It used to carry ONE substitution,
    # $( binder_transport_present && printf ... ), directly in its body.
    #
    # That particular one was NOT exploitable -- binder_transport_present uses only the `[`
    # and `printf` builtins, so no PATH lookup happens while help is rendered -- but leaving a
    # construct of the same shape in place after fixing the exploitable one is how the next
    # edit reintroduces the vulnerability: whoever adds a sentence beside it has a working
    # example of substitution-inside-help to copy.  So the body now contains NO substitution of
    # any kind, and the precedent it follows is the matrix_summary loop directly above, which
    # is computed here for a related reason of its own.
    local binder_node_note
    if binder_transport_present; then
        binder_node_note='   (present and usable: every invocation can run)'
    else
        binder_node_note='   (ABSENT or unusable: the AIDL-selected invocations will be DEFERRED)'
    fi

    cat <<USAGE
Usage: run_coverage.sh [OPTIONS]

gcov/lcov coverage runner and line-coverage gate for the HDMI-CEC middleware L1 suite.
Reproduces the "Generate coverage" step of .github/workflows/L1-tests.yml, adds the
branch data that step discards, and adds the numeric threshold it has never had.

OPTIONS
  -b, --build              Build the middleware, its L1 suite and the L2 tier with
                           coverage instrumentation first.  OFF by default so an existing
                           measurement is never silently discarded.  autoreconf and
                           configure rewrite six git-tracked Makefiles: this snapshots
                           their working-tree content beforehand and RESTORES THEM ITSELF
                           on every exit path, including a failure or a Ctrl-C.  No
                           'git checkout' is run or suggested (see --restore).
  -r, --run                Zero this build tree's gcov counters ONCE, then run every
                           invocation of the matrix -- one process per selection outcome,
                           each with its own results and log artifact -- and capture ONCE
                           after all of them have passed.  OFF by default.
                           Zeroing once is what makes the figures an account of THIS run:
                           gcov counters accumulate, so a stale .gcda would let the gate
                           pass on evidence the current tests did not produce.  Zeroing
                           once rather than per invocation is what makes them an account
                           of the WHOLE run: the back-end selection resolves once per
                           process, so accumulation across the invocations is the only way
                           both arms of that branch are ever covered.
                           An invocation that cannot run on this host -- no binder driver,
                           for the AIDL-selected ones -- is reported as DEFERRED and the
                           verdict becomes advisory.  It is never counted as passed.
  -t, --threshold N        Line-coverage bar.  Default ${COVERAGE_MIN}.  N is spelled as
                           digits or digits.digits and must be between 0 and 100 -- 80, 0,
                           100 and 80.5 are all accepted, because lcov's
                           --fail-under-lines takes a fraction.  Anything that would have
                           to be guessed at is REFUSED rather than coerced: an empty
                           value, a letter, a sign, surrounding spaces, or more than one
                           decimal point.
                           Lower it only for a deliberate diagnostic run; 80 is the
                           required bar, and any other value makes the verdict ADVISORY
                           (exit 3), never an acceptance.
  -o, --output-dir DIR     Write artifacts to DIR.  Left unset -- the default -- the root
                           is MINTED per run with
                           mktemp -d "<safe parent>/hdmicec-l1-coverage.XXXXXXXX",
                           where <safe parent> is the first of \$TMPDIR, \$XDG_RUNTIME_DIR
                           and \$HOME whose whole ancestry passes the custody checks -- a
                           group- or world-writable directory without the sticky bit is
                           skipped rather than warned about, so /tmp is not used on a host
                           where it is mode 2777.  The root is outside the git tree because
                           this suite's .gitignore
                           does not cover coverage.info, filtered_coverage.info or
                           coverage/ and is out of scope for editing, and unpredictable so
                           it cannot be pre-created by another account.  The chosen path is
                           printed as the "artifacts:" line.
                           A DIR you name is held to the same custody as a minted one and
                           to two checks a minted name cannot need: it is collapsed
                           lexically first (so '..' cannot land the artifacts outside the
                           tree it appears to name) and refused if it is near-root or
                           inside a system tree -- /etc, /usr, /var and the rest; /tmp,
                           /var/tmp, /run/user, /opt, /home, /root and anywhere in your own
                           checkout are all accepted.  It is then brought to mode 0700, and
                           every file written under it is 0600.
                           REUSING A DIR IS SAFE: every artifact a previous run could have
                           left under one of this runner's fixed names is removed at run
                           start, under the run lock, so the directory holds exactly one
                           generation.  Nothing else under DIR is touched -- the removal
                           works from a fixed list of names, never recursively over DIR.
                           run_status.txt names the run id the directory holds; read it
                           first when an artifact's provenance is in doubt.  A run takes
                           that marker over before it removes or writes anything else and
                           refuses to start if it cannot, so a previous run's verdict can
                           never stand over this run's artifacts.
      --per-file-gate      In addition to the aggregate gate, fail when ANY non-exempt file
                           in the filtered trace is below the bar.  ON by default, because
                           Directive 4 states the bar per target; the flag exists so a
                           caller can state it explicitly and override
                           COVERAGE_PER_FILE_GATE=0 from the environment.
      --no-per-file-gate   Gate on the aggregate only, for a diagnostic run that wants the
                           per-file table without its verdict.  NOT a setting an acceptance
                           run uses.  Below-bar files are enumerated either way.
      --no-html            Skip the genhtml report (the trace and the tables are still
                           produced).
      --restore            Restore the six git-tracked Makefiles that autoreconf and
                           configure rewrite from THIS invocation's snapshot, and exit.
                           AS A STANDALONE FLAG IT ALWAYS FAILS, and that is the contract
                           rather than a limitation: a snapshot exists only inside the
                           invocation that took it, --build is the only thing that takes
                           one, and parse_args forbids combining the two.  So a bare
                           --restore has no authority to restore from, says so, and exits
                           non-zero.  It does NOT substitute a git index comparison for
                           that authority -- clean-against-the-index does not mean
                           unchanged-since-this-run-started, and a tree whose Makefiles
                           were committed in generated form reads as perfectly clean while
                           holding exactly the content that needs replacing.  What git says
                           is still printed, as information about the tree.
                           It never runs 'git checkout' over those paths, in any mode: that
                           restores what the index holds rather than what was there, and
                           would destroy an uncommitted edit to ccec/src/Makefile --
                           hand-written RDK build source, not generated output.
                           You normally need none of this: --build snapshots all six before
                           autoreconf and restores them from its exit trap on every path
                           out, including a failure and a Ctrl-C.
  -h, --help               Print this help and exit.
      --selftest           Audit every internal function and variable reference in this
                           script, re-parse it, and verify the measurement tooling -- then
                           exit.  Runs no suite, zeroes no counters, captures nothing and
                           creates no artifact directory.  The same audit runs at the start
                           of every real invocation.

ENVIRONMENT (command-line flags win)
  COVERAGE_MIN=N               same as --threshold
  COVERAGE_OUTPUT_DIR=DIR      same as --output-dir
  COVERAGE_PER_FILE_GATE=1     same as --per-file-gate (the default)
  COVERAGE_PER_FILE_GATE=0     same as --no-per-file-gate
  COVERAGE_GATE_EXEMPT_FILES   newline-separated trace paths whose per-file VOTE is
                               suppressed.  Empty by default and meant to stay that way;
                               the only admissible reason for an entry, and what an entry
                               does and does not do, are stated at its definition in this
                               script.  Exempt files are still traced, still counted in
                               the aggregate, and still printed with their figures.
  GTEST_PREFIX=DIR             prefix providing libgtest/libgmock and their headers,
                               used by --build and put on LD_LIBRARY_PATH by --run.
                               Default: ${GTEST_PREFIX}
  GTEST_EXTRA_ARGS="..."       extra arguments appended to each invocation's command line.
                               --gtest_shuffle is a DIAGNOSTIC: this suite has
                               pre-existing order-fragile tests that pass in the default
                               order, so a shuffled run is expected to fail and must
                               never be treated as a gate.
                               A --gtest_filter here OVERRIDES the invocation's own filter
                               (GoogleTest lets the last one win) and therefore FAILS the
                               invocation on its case count, with both numbers named. That
                               is deliberate: a re-filtered invocation is a different
                               invocation and this script will not report one as the other.

  ARTIFACT PROVENANCE -- optional, and needed only where this script runs somewhere the
  revision cannot be read from the tree it is measuring.
  CEC_PROVENANCE_REVISION_<LABEL>
                               the commit sha to record for one repository, where <LABEL> is
                               the label provenance.txt prints for it (superproject,
                               hdmicec, entservices-hdmicecsink, ...) upper-cased with '-'
                               mapped to '_'.  40 lowercase hex characters, optionally
                               suffixed -dirty; anything else, an empty value included, is
                               refused rather than recorded, because a provenance record
                               nobody can trust is worse than one that says 'unavailable'.
                               Consulted ONLY where git cannot answer for that path -- no
                               git, no repository, no HEAD, which is the case inside a
                               payload assembled with 'git archive'.  Where git CAN answer,
                               git wins and provenance.txt says the supplied value went
                               unused, so a measured identity and an asserted one are never
                               confused.  Unset for every label: exactly the behaviour this
                               script had before the variable existed.

  AIDL/BINDER STAGING PREFIXES -- passed straight through to configure by --build, which
  names no path of its own.  configure.ac declares all four with AC_ARG_VAR and derives the
  include roots, the library directories, the four -l edges and the C++17 flag from them, so
  these are the only inputs and there is one place that decides.  Unset is passed as unset,
  never as empty, because 'BINDER_SDK_DIR=' asserts there is no SDK while omitting it lets
  configure look where it knows to look.
  HALIF_PREFIX=DIR             root of the staged rdk-halif-aidl tree, supplying the
                               generated AIDL stub headers, common/current/halcompat.h and
                               the stub libraries.  Unset: configure falls back to the
                               sibling rdk-halif-aidl checkout beside this submodule.
                               Currently: ${HALIF_PREFIX:-<unset>}
  HALIF_LIB_DIR=DIR            stub library directory, for a split staging layout in which
                               it is not HALIF_PREFIX/lib/halif.
                               Currently: ${HALIF_LIB_DIR:-<unset>}
  BINDER_SDK_DIR=DIR           root of the staged Binder SDK, holding lib/binder and
                               include/binder_sdk.
                               Currently: ${BINDER_SDK_DIR:-<unset>}
  BINDER_SDK_INCLUDE_DIR=DIR   Binder header prefix, for a layout whose headers are not
                               under BINDER_SDK_DIR.
                               Currently: ${BINDER_SDK_INCLUDE_DIR:-<unset>}

  THE LOADER PATH THE RUNNERS RECEIVE IS REBUILT FROM THE PREFIXES ABOVE AND FROM NOTHING
  ELSE.  An LD_LIBRARY_PATH present in this script's own environment is deliberately NOT
  propagated to the test binaries: an inherited loader path is untrusted input to a process
  that loads shared objects, and these binaries and the staged Binder closure carry no
  RUNPATH, so the first directory that answers a soname is the one that gets loaded.  Each
  directory derived from the prefixes is admitted only if it exists, is a real directory
  rather than a symbolic link, is owned by you or by root, and is not group- or
  world-writable; one that exists and fails a check is named and the run stops rather than
  searching it, and one that is simply absent is named and skipped.  Every discarded
  inherited entry is logged too, so nothing disappears quietly.  Stage the libraries under
  these prefixes, or register them with ldconfig, rather than exporting LD_LIBRARY_PATH.

  TEST HARNESS VARIABLES -- set by this script per invocation, listed so their contract is
  discoverable from --help rather than only from the sources.  Do not set them yourself: a
  value in the environment would be overridden per invocation anyway.
  CEC_TEST_AIDL_MODE           absent | compatible | incompatible | remote.  What the
                               harness makes the service lookup find, and the only thing
                               that distinguishes one invocation from the next.  Read by
                               tests/L1Tests/test_main.cpp and tests/L2Tests/test_main.cpp
                               and by NO production source.  Each harness hard-fails on a
                               value it does not implement rather than downgrading to
                               'absent', so a typo cannot silently become a legacy run.
  CEC_FAKE_AIDL_HOST_PATH      where the L2 harness finds the out-of-process fake service
                               host.  Set for both L2 invocations; only 'remote' reads it.
  CEC_FAKE_HOST_READY_FD       NOT SET BY THIS SCRIPT, and it must not be.  It names an
                               INHERITED descriptor -- the write end of a pipe whose read
                               end the waiting parent holds -- and the parent is the L2
                               harness, which creates the pipe, forks, sets this in the
                               child's environment and blocks on the read end under a
                               bounded timeout.  A value exported from here would name a
                               descriptor this script never opened; the host treats an
                               unusable descriptor as a hard failure precisely so a botched
                               handoff is reported rather than answered on stdout.
  BINDER_DRIVER_NODE           the driver node whose presence decides whether the
                               AIDL-selected invocations can run at all.  Default
                               /dev/binder.  Absent or unusable, those invocations are
                               reported DEFERRED and never attempted -- registering a
                               service name opens the driver, and on the pinned binder stack
                               that aborts the process when the node is missing.
                               Currently: ${BINDER_DRIVER_NODE}

EXIT STATUS
  0  ACCEPTANCE: the suite was run by this invocation, it was green, and line coverage is
     at or above the bar.  This is the only status that means "measured and passing".
  1  a prerequisite is missing, a step failed, or the coverage gate failed
  3  ADVISORY: coverage is at or above the bar, but this invocation did not establish the
     evidence for it, or did not hold itself to the acceptance settings.  All artifacts are
     produced; the numbers are real but not an acceptance verdict.  Do not treat 3 as a
     pass.  EVERY trigger is listed here, because a status whose causes are summarised is
     a status a caller cannot reason about:
       * --run was omitted, so the figures come from pre-existing cumulative counters that
         may credit a test that no longer runs
       * GTEST_EXTRA_ARGS was set, so the suite ran with arguments other than the
         invocation matrix's own filters
       * one or more invocations were DEFERRED rather than run (no usable binder driver
         for the AIDL-selected ones), so at least one arm of the selection branch was
         never exercised
       * --threshold set the bar to anything other than the 80% Directive 4 requires
       * --no-per-file-gate / COVERAGE_PER_FILE_GATE=0 turned the per-file half off
       * COVERAGE_GATE_EXEMPT_FILES is set, so at least one target's per-file vote was
         waived
       * the line gate's trace holds dependency-header records from outside this submodule
         that the DEPENDENCY_EXCLUDES policy has not classified, so its denominator is not
         the one the recorded baseline was measured over
       * the branch-arm gate could not MEASURE one or more required arms because the
         invocation or the binder driver that arm needs was unavailable
       * the branch-arm gate could not CHECK one or more required arms because a manifest
         entry was added without coordinates
     Two related conditions do not end here.  A missing or unusable flock is fatal, because
     a run that cannot prove it is alone must not reach a verdict; and an unsafe ancestry for
     the minted directory is not reported at all, because a safe parent is chosen instead.

RESOLVED PATHS FOR THIS INVOCATION
  script          ${SCRIPT_PATH}
  submodule root  ${HDMICEC_ROOT}      (the lcov capture directory)
  workspace root  ${WS}
  artifacts       ${OUTPUT_DIR:-<minted with mktemp -d under the first safe parent of \$TMPDIR, \$XDG_RUNTIME_DIR, \$HOME; printed as "artifacts:" when the run starts>}
  L1 runner       ${HDMICEC_ROOT}/${TEST_BINARY_REL}
  L2 runner       ${HDMICEC_ROOT}/${L2_TEST_BINARY_REL}
  fake host       ${HDMICEC_ROOT}/${FAKE_HOST_BINARY_REL}   (launched by the L2 harness, never by this script)
  binder node     ${BINDER_DRIVER_NODE}${binder_node_note}

THE INVOCATION MATRIX
${matrix_summary}
USAGE
}

# Significant digits before the decimal point: padding zeros removed, and an absent integer
# part (".5") read as 0.  Pure string work -- no arithmetic -- so nothing here can overflow
# whatever spelling the caller was given.
threshold_normalized_int() {
    local int="${1%%.*}"
    : "${int:=0}"
    while [ "${#int}" -gt 1 ] && [ "${int#0}" != "$int" ]; do
        int="${int#0}"
    done
    printf '%s\n' "$int"
}

# Decide whether a threshold spelling lies OUTSIDE 0..100.  Returns 0 (true) when it does.  The
# caller has already established the spelling is digits or digits.digits, so this function only
# has to rule on magnitude.
#
# WHY THIS IS LEXICAL AND NOT ARITHMETIC -- a fix, not a matter of style.  `[ "$n" -gt 100 ]`
# does not return false for a value the shell cannot hold: it returns status 2 and writes
# "integer expression expected" to stderr.  Status 2 is not 0, so an `||` list whose every arm
# does that evaluates FALSE, and a `die` guarded by one is skipped entirely.  A threshold of 2^63
# or larger therefore used to pass this validation untouched and go on to take the build lock,
# probe the measurement tooling, mint an artifact directory and reach lcov before failing for a
# reason that had nothing to do with the argument -- and because the not-80 advisory below was
# lost the same way, the run also presented itself as an ordinary acceptance attempt at the 80%
# bar.  Refusing a bad bar BEFORE any of that happens is the whole reason this validation lives
# in parse_args, so the guard must not have a magnitude of its own to overflow.
#
# A value in 0..100 has at most three significant digits, and "100" is the only three-digit
# member, so the entire decision is a length test and two string compares.
threshold_out_of_range() {
    local int frac=""
    int="$(threshold_normalized_int "$1")"
    case "$1" in
        *.*) frac="${1#*.}" ;;
    esac
    if [ "${#int}" -gt 3 ]; then
        return 0                       # four or more significant digits: 1000 and up
    fi
    if [ "${#int}" -eq 3 ] && [ "$int" != "100" ]; then
        return 0                       # 101..999
    fi
    if [ "$int" = "100" ] && [ -n "${frac//0/}" ]; then
        return 0                       # 100.5 is above the bar; 100.0 and 100.00 are not
    fi
    return 1
}

# Hold the range guard to both ends of the shell's integer range, so a future rewrite of it
# cannot quietly reintroduce the status-2 hole described above.  No suite is run, no counter is
# touched and nothing is written: this is a decision function fed a table of values.
threshold_selftest() {
    local v failures=0
    for v in 0 00 8 80 80.0 80.00 100 100.0 100.00 0100 99.999 0.5; do
        if threshold_out_of_range "$v"; then
            warn "threshold self-test: '$v' is inside 0..100 but the guard refuses it."
            failures=$((failures + 1))
        fi
    done
    for v in 101 100.5 100.01 999 1000 4294967296 9223372036854775807 \
             9223372036854775808 18446744073709551616 99999999999999999999999999; do
        if ! threshold_out_of_range "$v"; then
            warn "threshold self-test: '$v' is outside 0..100 but the guard accepts it."
            failures=$((failures + 1))
        fi
    done
    [ "$failures" -eq 0 ] || die "the threshold range guard misclassified $failures value(s).
       Every value outside 0..100 must be refused in parse_args -- BEFORE the run lock, the
       tooling pre-flight, the counter zeroing and the artifact directory -- so a bar that
       cannot be met never acquires the power to disturb a tree or an existing measurement."
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -b|--build)          DO_BUILD=1 ;;
            -r|--run)            DO_RUN=1 ;;
            --per-file-gate)     COVERAGE_PER_FILE_GATE=1 ;;
            --no-per-file-gate)  COVERAGE_PER_FILE_GATE=0 ;;
            --no-html)           DO_HTML=0 ;;
            --restore)           DO_RESTORE_ONLY=1 ;;
            -h|--help)           usage; exit 0 ;;
            --selftest)          selftest; exit 0 ;;
            -t|--threshold)
                [ $# -ge 2 ] || die "--threshold requires a value"
                COVERAGE_MIN="$2"; shift ;;
            --threshold=*)       COVERAGE_MIN="${1#*=}" ;;
            -o|--output-dir)
                [ $# -ge 2 ] || die "--output-dir requires a value"
                OUTPUT_DIR="$2"; OUTPUT_DIR_EXPLICIT=1; shift ;;
            --output-dir=*)      OUTPUT_DIR="${1#*=}"; OUTPUT_DIR_EXPLICIT=1 ;;
            --)                  shift; break ;;
            -*)                  die "unknown option $(render_untrusted "$1"). Run '$SCRIPT_PATH --help' for the option list." ;;
            *)                   die "unexpected argument $(render_untrusted "$1"). This script takes options only; run --help." ;;
        esac
        shift
    done
    [ $# -eq 0 ] || die "unexpected trailing argument $(render_untrusted "$1") and $(( $# - 1 )) more"

    # THRESHOLD SHAPE AND RANGE.  The accepted spelling is deliberately the same as the two
    # plugin runners': digits, or digits.digits.  Keep the three in step -- a bar that is a
    # working diagnostic value for one runner and a hard error in another cannot be wired
    # through one pipeline.  Fractions are accepted because lcov's --fail-under-lines takes
    # one, so refusing them would buy nothing.
    #
    # What is NOT accepted is anything that would have to be guessed at: an empty value, a
    # letter (`8O` for `80` is the classic typo), a sign, surrounding spaces, or more than one
    # decimal point.  A bar that cannot be read exactly is refused rather than coerced,
    # because a coerced bar produces a gate verdict for a percentage nobody asked for.
    case "$COVERAGE_MIN" in
        ''|*[!0-9.]*|*.*.*|.*|*.)
            die "threshold must be a number spelled as digits or digits.digits -- for example
       80, 0, 100 or 80.5 -- and between 0 and 100 (got $(render_untrusted "$COVERAGE_MIN")).  A threshold that
       cannot be read exactly is refused rather than rounded: a coerced bar would produce a
       gate verdict for a percentage nobody asked for." ;;
    esac
    # Range and the not-80 diagnostic are decided from the value's own digits rather than
    # with awk or shell arithmetic.  `[ 80.5 -le 100 ]` is a syntax error in every POSIX
    # shell, and awk cannot be used here because this validation runs BEFORE require_tools()
    # -- deliberately, so that a bad threshold is refused before anything else happens -- so
    # an absent awk would surface as a threshold error, which would be a lie.
    if threshold_out_of_range "$COVERAGE_MIN"; then
        die "threshold must be between 0 and 100 (got $(render_untrusted "$COVERAGE_MIN")).  A bar above 100% can
       never be met, so the gate could only ever fail and would say nothing about the tests."
    fi
    local min_int min_frac=""
    min_int="$(threshold_normalized_int "$COVERAGE_MIN")"
    case "$COVERAGE_MIN" in
        *.*) min_frac="${COVERAGE_MIN#*.}" ;;
    esac
    # 80 is this submodule's acceptance bar.  Any other value is a diagnostic, and saying so is
    # not enough on its own: a warning still lets the run print the same PASS line and return
    # the same 0 a real acceptance run returns, so on a warning alone `COVERAGE_MIN=0` buys an
    # acceptance success with no coverage requirement behind it.  The weakening is recorded as an
    # ADVISORY REASON, which forces the final verdict to ADVISORY and the exit status to
    # $EXIT_ADVISORY - a status a caller can branch on and cannot mistake for a pass.
    # 80, 80.0 and 80.00 are the same bar; 80.5 is not.  The integer half is compared as a
    # STRING for the same reason the range guard above is: `[ "$min_int" -ne 80 ]` returns
    # status 2, not false, for a value the shell cannot hold, and a status-2 arm in this `||`
    # list would silently suppress the advisory -- so an unreadably large bar would have
    # presented itself as an ordinary acceptance run at 80%.  min_int is already normalised to
    # significant digits, so "80" is its only spelling of eighty.
    if [ "$min_int" != "80" ] || [ -n "${min_frac//0/}" ]; then
        warn "the line bar is ${COVERAGE_MIN}%, not the required 80%.  This is a DIAGNOSTIC run:"
        warn "    its verdict is NOT the acceptance verdict for this submodule."
        note_advisory "the line bar was set to ${COVERAGE_MIN}%, not the 80% Directive 4 requires,
       so no verdict this run produces is an acceptance verdict for this submodule."
    fi

    case "$COVERAGE_PER_FILE_GATE" in
        0) note_advisory "per-file gating was turned off (--no-per-file-gate /
       COVERAGE_PER_FILE_GATE=0).  Directive 4 states the bar PER TARGET, so a run without the
       per-file half cannot produce an acceptance verdict however good the aggregate is." ;;
        1) ;;
        *)   die "COVERAGE_PER_FILE_GATE must be 0 or 1, got $(render_untrusted "$COVERAGE_PER_FILE_GATE")" ;;
    esac

    # An exemption list suppresses a per-file vote, which is exactly the shape of weakening the
    # aggregate cannot reveal: one file at 0% inside a large denominator moves the total by
    # almost nothing.  Naming a file here is admissible for a diagnostic run and never for an
    # acceptance one, so its presence is recorded rather than only printed later.
    if [ -n "$COVERAGE_GATE_EXEMPT_FILES" ]; then
        note_advisory "COVERAGE_GATE_EXEMPT_FILES is set, so at least one target's per-file vote
       was suppressed.  Directive 4 admits no exemption, so this run's verdict is diagnostic:
       $(render_untrusted "$COVERAGE_GATE_EXEMPT_FILES")"
    fi

    # An empty value here means two very different things depending on how it got there.
    # Unset -- the default -- means "mint an unpredictable root with mktemp -d", which is the
    # normal path and must not be rejected.  Explicitly EMPTY (--output-dir '' or
    # COVERAGE_OUTPUT_DIR=) would otherwise silently resolve to the caller's working directory,
    # so that one is still refused.  OUTPUT_DIR_EXPLICIT is what distinguishes them.
    if [ "$OUTPUT_DIR_EXPLICIT" -eq 1 ] && [ -z "$OUTPUT_DIR" ]; then
        die "--output-dir (or COVERAGE_OUTPUT_DIR) was given as an empty value.  An empty path
       would resolve to this run's working directory; leave it unset to have an unpredictable
       mode-0700 root minted under a parent it has proved safe instead, or give a real
       directory."
    fi

    # A CONTROL CHARACTER IN THE ARTIFACT ROOT IS REFUSED AT THIS ONE BOUNDARY, which is what
    # lets the two dozen downstream diagnostics that name the path keep interpolating it
    # directly.  This path reaches the ancestry walk, the lock diagnostics, the per-invocation
    # log and JSON names, the cleanup listing and the final summary; rendering it at each of
    # those would be twenty-four places to keep in step, whereas refusing the class here means
    # that by the time any of them runs the value is proven free of control characters.  It is
    # refused rather than stripped, because a directory this script silently renamed would not
    # be the one the caller named.  The value IS rendered in this message, because this is the
    # diagnostic that has to report it.
    case "$OUTPUT_DIR" in
        *[[:cntrl:]]*)
            die "--output-dir (or COVERAGE_OUTPUT_DIR) contains a control character:
       $(render_untrusted "$OUTPUT_DIR")
       A newline, a carriage return or an escape sequence in this path would be reproduced in
       every log line, lock diagnostic and artifact name that mentions it -- forging or hiding
       output rather than naming a directory.  Give a path made of printable characters." ;;
    esac

    # SUITE_TIMEOUT IS SHAPE-CHECKED HERE, for the same reason and with the same consequence as
    # OUTPUT_DIR above: it is named by the run banner, by the hang diagnostic and by the
    # SUITE_TIMEOUT=<seconds> remedy that diagnostic prints, and proving its shape at this one
    # boundary is what lets those three keep interpolating it directly.  It cannot be checked at
    # its declaration near the top of this file, because die() and render_untrusted() are defined
    # below that point; this is the first place both exist.
    #
    # Zero is refused, which the declaration has always said and nothing implemented until now:
    # timeout(1) reads 0 as "no limit", so SUITE_TIMEOUT=0 silently restored the unbounded run
    # this bound exists to prevent -- a deadlocked suite would then block for ever with no exit
    # status and no gate verdict, which is exactly the outcome the bound was introduced to make
    # impossible.
    case "$SUITE_TIMEOUT" in
        ''|*[!0-9]*)
            die "SUITE_TIMEOUT must be a plain positive integer number of seconds, got
       $(render_untrusted "$SUITE_TIMEOUT").  It is passed to timeout(1) and printed in this run's
       banner and in the hang diagnostic, so a value that cannot be read exactly is refused
       rather than coerced." ;;
    esac
    if [ -z "${SUITE_TIMEOUT//0/}" ]; then
        die "SUITE_TIMEOUT must not be zero (got $(render_untrusted "$SUITE_TIMEOUT")).  timeout(1)
       reads 0 as \"no limit\", so this would restore the unbounded run the bound exists to
       prevent: a deadlocked suite would block for ever with no exit status and no gate verdict.
       Give a positive number of seconds, or leave it unset for the 600s default."
    fi
}

# ------------------------------------------------------------------------------------
# Prerequisites.  Each failure names the thing that is missing AND the command that
# supplies it, because a coverage runner that just says "not found" wastes the reader's
# time at exactly the moment they have least context.
# ------------------------------------------------------------------------------------
require_tools() {
    local missing=0
    [ -n "$LCOV_BIN" ]    || { warn "lcov not found on PATH";    missing=1; }
    [ -n "$GENHTML_BIN" ] || { warn "genhtml not found on PATH"; missing=1; }
    [ -n "$GCOV_BIN" ]    || { warn "gcov not found on PATH";    missing=1; }
    [ -n "$AWK_BIN" ]     || { warn "awk not found on PATH";     missing=1; }
    [ -n "$FIND_BIN" ]    || { warn "find not found on PATH";    missing=1; }
    [ -n "$MKTEMP_BIN" ]  || { warn "mktemp not found on PATH";  missing=1; }
    [ -n "$STAT_BIN" ]    || { warn "stat not found on PATH";    missing=1; }
    [ "$missing" -eq 0 ] || die "missing coverage tooling.
       lcov, genhtml and gcov are what this script measures with, and awk, find and
       mktemp are how it parses and stages.  On a Debian/Ubuntu host:
           sudo apt-get install -y lcov
       gcov ships with gcc; awk, find, mktemp and stat ship with the base system.
       lcov 2.x is required: this script uses --fail-under-lines and
       --rc branch_coverage=1, neither of which exists in lcov 1.x."
}

# Recorded rather than enforced: the trace-record spellings this script parses (FNL:, FNA:)
# and the gate option it relies on are lcov 2.x behaviour.  A 1.x lcov would fail later with
# a confusing message, so name the version now.
#
# This runs AFTER make_private_lcov_home(), and the order is not cosmetic.  lcov reads
# $HOME/.lcovrc on EVERY invocation, `--version` included, and a file it cannot parse makes
# every one of them fail: a home configuration setting both `lcov_branch_coverage` and
# `genhtml_branch_coverage` makes lcov 2.0-1 answer `--version` with
# "ERROR: unexpected ARRAY for branch_coverage value".  Printed before the private HOME
# existed, this banner reported that error text as though it were a version string -- a line
# that looks like provenance and is not.  Run with the private HOME in place, what it prints
# is the version of the tool that will actually do the measuring.
#
# `head -n1` can close the pipe early and hand the producer a SIGPIPE, which pipefail would
# otherwise turn into a fatal error over a banner line -- hence the `|| true`.
log_tool_versions() {
    log "lcov:    $( { lcov_run    --version 2>&1 | head -n1; } || true )"
    log "genhtml: $( { genhtml_run --version 2>&1 | head -n1; } || true )"
    log "gcov:    $( { "$GCOV_BIN"    --version 2>&1 | head -n1; } || true )"
}

# ------------------------------------------------------------------------------------
# THE TWO TEST TIERS AND THE FAKE SERVICE HOST.
#
# Each is the libtool WRAPPER, never .libs/<name>: the wrapper is what sets up the
# uninstalled shared-library search path for libRCEC and libRCECOSHal, and for the L2 tier
# also for the AIDL stub and Binder libraries libRCEC now links.
#
# TEST_BINARY_REL keeps its name and its meaning -- the L1 runner -- because
# require_instrumented_tree and do_build both treat it as THE binary whose absence means
# "this tree was never built", and that judgement is still the L1 runner's to make: the L1
# suite is the one that always exists, and a tree with an L1 runner but no L2 runner is a
# partial build rather than an unbuilt one.  The L2 pair is named separately and checked
# separately, in the invocations that use it.
#
# FAKE_HOST_BINARY_REL is NOT a test and is never run by this script.  It is the
# out-of-process fake service the L2 harness launches for invocation E; tests/L2Tests's own
# Makefile.am keeps it out of TESTS for the same reason (listing a process that runs until
# signalled would hang `make check` rather than fail it).  All this script does with it is
# build it and tell the harness where it is -- see CEC_FAKE_AIDL_HOST_PATH below.
# ------------------------------------------------------------------------------------
readonly TEST_BINARY_REL='tests/L1Tests/run_L1Tests'
readonly L2_TEST_BINARY_REL='tests/L2Tests/run_L2Tests'
readonly FAKE_HOST_BINARY_REL='tests/L2Tests/fake_hdmi_cec_aidl_host'

# Counters are always consumed through a command substitution, and `find` returns
# non-zero the moment it meets one unreadable directory even with stderr silenced.  Under
# `set -e` plus pipefail that would abort the run over a partial walk, so both helpers
# absorb the status and always emit a number: a short count shows up as a prerequisite
# failure with a useful message instead of a bare exit.
profile_file_count() { # $1=extension glob, e.g. '*.gcda'
    { "$FIND_BIN" "$HDMICEC_ROOT" -name "$1" -type f 2>/dev/null || true; } | wc -l | tr -d '[:space:]'
}

gcda_count() { profile_file_count '*.gcda'; }
gcno_count() { profile_file_count '*.gcno'; }


require_instrumented_tree() {
    local gcno gcda
    gcno="$(gcno_count)"
    gcda="$(gcda_count)"

    [ -x "$HDMICEC_ROOT/$TEST_BINARY_REL" ] || die "the L1 test binary is missing:
           $HDMICEC_ROOT/$TEST_BINARY_REL
       Build it with:  $SCRIPT_PATH --build
       or follow the BUILD RECIPE in this script's header.  Remember --enable-l1tests:
       without it configure builds no test suite at all."

    # THE L2 TIER IS REPORTED HERE, NOT REQUIRED HERE, and the difference is deliberate.  An
    # invocation that needs a binary it does not have fails in run_one_invocation, naming that
    # binary and that invocation -- which is a better diagnostic than a blanket prerequisite
    # failure, and it leaves the L1-only invocations runnable on a tree where the L2 tier was
    # not configured.  Saying so up front is still worth doing: a caller who expected five
    # invocations and is about to get three should learn it before the counters are zeroed.
    if [ ! -x "$HDMICEC_ROOT/$L2_TEST_BINARY_REL" ] || [ ! -x "$HDMICEC_ROOT/$FAKE_HOST_BINARY_REL" ]; then
        warn "the L2 integration tier is not built:"
        [ -x "$HDMICEC_ROOT/$L2_TEST_BINARY_REL" ]   || warn "    missing: $L2_TEST_BINARY_REL"
        [ -x "$HDMICEC_ROOT/$FAKE_HOST_BINARY_REL" ] || warn "    missing: $FAKE_HOST_BINARY_REL"
        warn "  Invocations D and E need both and will fail naming them.  '$SCRIPT_PATH --build'"
        warn "  builds them; they come from tests/L2Tests, which configure.ac configures through"
        warn "  AC_CONFIG_FILES and tests/Makefile.am recurses into under --enable-l1tests."
    fi

    [ "$gcno" -gt 0 ] || die "no *.gcno files under $HDMICEC_ROOT, so the tree is not
       coverage-instrumented and gcov has nothing to report on.  Rebuild with:
           $SCRIPT_PATH --build
       which configures with CXXFLAGS/LDFLAGS carrying -fprofile-arcs -ftest-coverage."

    # Counters are only a PRECONDITION when this invocation is not going to produce them.
    # With --run they are about to be written: do_run zeroes them first and then hard-fails
    # if the run leaves none behind, so the guarantee is kept either way.  Demanding them
    # here unconditionally would make the documented '--build --run' invocation impossible
    # on a freshly built tree, which is exactly the tree that flow exists for.
    if [ "$DO_RUN" -eq 0 ]; then
        [ "$gcda" -gt 0 ] || die "no *.gcda files under $HDMICEC_ROOT: the suite is
       instrumented ($gcno .gcno files) but has never been executed, so no counters
       exist.  Run it with:
           $SCRIPT_PATH --run
       or by hand:
           cd $HDMICEC_ROOT/tests/L1Tests && ./run_L1Tests"
    fi

    log "instrumentation: $gcno .gcno file(s), $gcda .gcda counter file(s)"
    if [ "$DO_RUN" -eq 0 ]; then
        # Clause 4 of the governing contract: gcov counters accumulate, so figures taken
        # from counters this invocation did not produce could credit a test that no longer
        # runs.  That cannot be prevented without running the suite, so it is made VISIBLE
        # instead of silent -- the age of the newest counter is usually enough to tell a
        # fresh measurement from a stale one at a glance.
        #
        # ACCUMULATION IS BOTH THE MECHANISM AND THE HAZARD, which is why this warning
        # says less than it looks as though it should.  Under `--run` the counters are
        # zeroed once and then accumulated DELIBERATELY across the matrix, because the
        # back-end selection resolves once per process and that is the only way both arms
        # of the selection branch are ever covered.  Without `--run` the same accumulation
        # is uncontrolled: these counters may hold one complete matrix, a partial one, or
        # several runs of different builds, and nothing here can tell which.  So the
        # figures are still produced and still real, and the verdict is still advisory --
        # what cannot be established is WHICH executions they describe.
        local newest
        newest="$( { "$FIND_BIN" "$HDMICEC_ROOT" -name '*.gcda' -type f \
                         -printf '%TY-%Tm-%Td %TH:%TM\n' 2>/dev/null || true; } \
                   | LC_ALL=C sort | tail -n1 || true)"
        warn "measuring PRE-EXISTING counters; the suite was not run by this invocation."
        warn "  newest .gcda timestamp: ${newest:-unknown}"
        warn "  gcov counters ACCUMULATE across runs, so these figures describe every run"
        warn "  that has ever executed against this build tree, not just the latest one."
        warn "  Use --run to zero the counters and measure exactly one suite execution."
        # And make that visible in the VERDICT, not only in the log.  Warning about stale
        # evidence and then exiting 0 with "COVERAGE GATE PASSED" is indistinguishable, to
        # any caller, from a clean measurement -- so this invocation does not produce
        # an acceptance verdict at all.  The figures and artifacts are still produced.
        note_advisory "the suite was NOT run by this invocation (no --run): the figures come from
       pre-existing, cumulative .gcda counters (newest: ${newest:-unknown}) and may credit
       tests that no longer run.  Re-run with --run for an acceptance verdict."
    fi
}

# ------------------------------------------------------------------------------------
# THE STUB HEADERS AND THE HAL MOCK SYMLINK -- ITS OWN STEP, AHEAD OF THE SNAPSHOT.
#
# stubs/ mutes the IARM bus headers this middleware includes but does not ship, and the
# symlink is what substitutes the HAL driver mock for the real driver header.  Both are build
# prerequisites, not committed artifacts, and both are stripped from the coverage denominator
# by the '*/stubs/*' and '*/mocks/*' globs.
#
# WHY IT IS ITS OWN STEP RATHER THAN THE FIRST LINK OF THE BUILD CHAIN.  As the first link of
# the `&&` chain inside the build subshell it would be a MUTATING step sitting between the
# Makefile snapshot and autoreconf.  The mutation is benign -- it creates directories and one
# symlink under stubs/ and touches none of the six -- but "benign" is a property of the current
# step list, and the snapshot's guarantee should not depend on a reader re-deriving it every
# time the chain changes.  Kept out here, autoreconf is literally the next mutating command
# after the snapshot, so the guarantee is structural rather than argued.
#
# IT ALSO CARRIES VALIDATION, which an inline chain has none of: `touch` and `ln -sf` both
# succeed in situations that leave the build unable to compile -- a stubs/ path that is a
# symlink to somewhere else, a mock header that has been moved so the link dangles, a stub
# that exists as a directory.  Each is checked, and each failure names the file and the
# operation rather than surfacing later as a missing-header error from the compiler.
#
# The `###STEP:` marker is written to the same build log the chain writes to, so the failure
# attribution the chain relies on -- grep the last marker -- covers this step too.
# ------------------------------------------------------------------------------------
STUB_HEADER_DIRS=(
    'stubs/rdk/iarmbus'
    'stubs/ccec/drivers/iarmbus'
)
STUB_HEADER_FILES=(
    'stubs/rdk/iarmbus/libIARM.h'
    'stubs/rdk/iarmbus/libIBus.h'
    'stubs/rdk/iarmbus/libIBusDaemon.h'
    'stubs/ccec/drivers/iarmbus/CecIARMBusMgr.h'
)
readonly STUB_HEADER_DIRS STUB_HEADER_FILES
# The link, its literal target as written, and the file that target must resolve to.  The
# target is RELATIVE and is spelled exactly as the workflow and the setup documents spell it;
# the resolved path is derived from it here only so the check can compare content.
readonly STUB_HAL_SYMLINK='stubs/ccec/drivers/hdmi_cec_driver.h'
readonly STUB_HAL_SYMLINK_TARGET='../../../mocks/hdmicec/hdmi_cec_driver.h'
readonly STUB_HAL_MOCK_HEADER='mocks/hdmicec/hdmi_cec_driver.h'

prepare_stub_headers() { # $1=build log the step marker and any tool output are appended to
    local build_log="$1" d f

    printf '%s\n' '###STEP:generate stub headers and inject the HAL driver mock' >>"$build_log"
    log "  generating the stub headers and the HAL driver mock symlink"

    # The mock header has to be there BEFORE the link is made, because a link created to a
    # missing target is a dangling link that `ln -sf` reports as success.
    [ -f "$HDMICEC_ROOT/$STUB_HAL_MOCK_HEADER" ] || die "the HAL driver mock header is missing:
       $HDMICEC_ROOT/$STUB_HAL_MOCK_HEADER
       It is what stubs/$STUB_HAL_SYMLINK stands in for, so without it the build compiles
       against the real driver header -- which this tree does not ship -- or against nothing.
       This is a checkout problem, not a build problem."

    # Each directory component is created and then re-read: `mkdir -p` is content with an
    # existing symlink-to-directory, and a build that writes its stubs through one is writing
    # them somewhere this script did not choose.
    for d in "${STUB_HEADER_DIRS[@]}"; do
        mkdir -p -- "$HDMICEC_ROOT/$d" >>"$build_log" 2>&1 \
            || die "could not create the stub directory $HDMICEC_ROOT/$d (step: generate stub headers)"
        [ ! -L "$HDMICEC_ROOT/$d" ] || die "the stub directory $HDMICEC_ROOT/$d is a symbolic link.
       This build will not write its stub headers through a link it did not create.  Remove it."
        [ -d "$HDMICEC_ROOT/$d" ] || die "$HDMICEC_ROOT/$d exists and is not a directory, so the
       stub headers cannot be created there (step: generate stub headers)."
    done

    for f in "${STUB_HEADER_FILES[@]}"; do
        [ ! -L "$HDMICEC_ROOT/$f" ] || die "the stub header $HDMICEC_ROOT/$f is a symbolic link.
       Stub headers are empty files this build creates; a link means something else is
       supplying that header.  Remove it (step: generate stub headers)."
        touch -- "$HDMICEC_ROOT/$f" >>"$build_log" 2>&1 \
            || die "could not create the stub header $HDMICEC_ROOT/$f (step: generate stub headers)"
        [ -f "$HDMICEC_ROOT/$f" ] || die "$HDMICEC_ROOT/$f is not a regular file after touch, so
       the compiler cannot include it (step: generate stub headers)."
    done

    # -n as well as -f: without it, a link that already points at a DIRECTORY makes ln create
    # the new link INSIDE that directory instead of replacing it.
    ln -sfn -- "$STUB_HAL_SYMLINK_TARGET" "$HDMICEC_ROOT/$STUB_HAL_SYMLINK" >>"$build_log" 2>&1 \
        || die "could not create the HAL driver mock symlink
       $HDMICEC_ROOT/$STUB_HAL_SYMLINK -> $STUB_HAL_SYMLINK_TARGET
       (step: generate stub headers)"

    [ -L "$HDMICEC_ROOT/$STUB_HAL_SYMLINK" ] || die "$HDMICEC_ROOT/$STUB_HAL_SYMLINK exists but is
       not a symbolic link, so the HAL driver mock is not being injected and the build would
       compile against whatever that file happens to contain (step: generate stub headers)."
    # -f follows the link, so this is the dangling-link check.
    [ -f "$HDMICEC_ROOT/$STUB_HAL_SYMLINK" ] || die "the HAL driver mock symlink dangles:
       $HDMICEC_ROOT/$STUB_HAL_SYMLINK -> $STUB_HAL_SYMLINK_TARGET
       ln reports a link to a missing target as success, which is why this is checked rather
       than assumed (step: generate stub headers)."
    # And that it lands on the mock rather than on some other header of the same name.  Content
    # equality rather than path resolution, so no readlink -f is needed and a bind mount or a
    # relocated checkout does not produce a false failure.
    if [ -n "$CMP_BIN" ]; then
        "$CMP_BIN" -s -- "$HDMICEC_ROOT/$STUB_HAL_SYMLINK" "$HDMICEC_ROOT/$STUB_HAL_MOCK_HEADER" \
            || die "$HDMICEC_ROOT/$STUB_HAL_SYMLINK does not resolve to the HAL driver mock:
       it should reach $HDMICEC_ROOT/$STUB_HAL_MOCK_HEADER, and the two differ.
       Something else is at that path (step: generate stub headers)."
    fi

    log "    ${#STUB_HEADER_FILES[@]} stub header(s) and 1 HAL mock symlink in place and verified"
}

# ------------------------------------------------------------------------------------
# Optional build.  A faithful replay of the workflow's "Generate stub headers" and
# "Build hdmicec" steps.  Off by default, because a rebuild is the one action that can
# destroy an existing measurement, and this script's primary job is to measure.
# ------------------------------------------------------------------------------------
do_build() {
    rule
    log "building the middleware and its L1 suite with coverage instrumentation"
    log "  (reproducing .github/workflows/L1-tests.yml steps 'Generate stub headers' and 'Build hdmicec')"

    local build_log="$OUTPUT_DIR/build.log"
    local nproc_count
    nproc_count="$( (command -v nproc >/dev/null 2>&1 && nproc) || echo 1 )"
    # Custody is re-checked here for the same reason it is re-checked before every trace
    # write: build.log is the record a failed build is diagnosed from, and truncating a file
    # in a directory that was substituted since startup would write it somewhere else.
    assert_output_dir_still_safe "$build_log"
    : >"$build_log"

    # configure.ac locates GoogleTest with PKG_CHECK_MODULES([GTEST], [gtest >= 1.10.0]),
    # which searches pkg-config's path and ignores -I/-L entirely.  CI gets away with
    # passing only include and library flags because it ALSO apt-installs libgtest-dev, so
    # a system gtest.pc is present; on a host where GoogleTest exists only as a
    # source-built copy under a private prefix -- which is the normal case when the
    # distribution's GoogleTest is too new for this project's C++ standard -- that check
    # fails and configure aborts.  Adding the prefix's pkgconfig directory here makes
    # GTEST_PREFIX mean what it says.  Any caller-supplied PKG_CONFIG_PATH is preserved
    # behind it rather than replaced.
    local build_pkg_config_path="$GTEST_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

    # ------------------------------------------------------------------------------------
    # THE AIDL/BINDER STAGING PREFIXES, HANDED TO configure AS PRECIOUS VARIABLES.
    #
    # configure.ac declares all four with AC_ARG_VAR, so `./configure NAME=value` is the
    # documented way to supply them, and it DERIVES everything else from them itself: the four
    # include roots, the -L directories, the four -l edges and the C++17 dialect flag are
    # AC_SUBSTed outputs computed inside configure.  So this passes the prefixes and nothing
    # else -- no -I, no -L, no -l is spelled here.
    #
    # WHY NOT PUT THE INCLUDE ROOTS IN CPPFLAGS ABOVE, which would look more direct.  Because
    # then two places would decide where halcompat.h lives, and configure.ac exists precisely
    # so that ONE place does: the same two prefixes feed the Autotools build and the
    # hand-written ccec/src/Makefile, and the whole point of the single-prefix contract is that
    # the two build systems cannot end up configured differently.  A root spelled here as well
    # would be a second source of truth and a silent way for them to diverge.
    #
    # AN UNSET PREFIX IS PASSED AS UNSET, NOT AS AN EMPTY STRING, and the difference is real:
    # `configure BINDER_SDK_DIR=` asserts there is no Binder SDK and fails configure's
    # availability check, while omitting it lets configure look where it knows to look -- for
    # HALIF_PREFIX, the sibling rdk-halif-aidl checkout beside this submodule, which is the
    # ordinary in-workspace arrangement.  Hence the append-only array rather than four
    # unconditional assignments.
    # ------------------------------------------------------------------------------------
    local -a configure_prefix_args=()
    [ -z "$HALIF_PREFIX" ]            || configure_prefix_args+=("HALIF_PREFIX=$HALIF_PREFIX")
    [ -z "$HALIF_LIB_DIR" ]           || configure_prefix_args+=("HALIF_LIB_DIR=$HALIF_LIB_DIR")
    [ -z "$BINDER_SDK_DIR" ]          || configure_prefix_args+=("BINDER_SDK_DIR=$BINDER_SDK_DIR")
    [ -z "$BINDER_SDK_INCLUDE_DIR" ]  || configure_prefix_args+=("BINDER_SDK_INCLUDE_DIR=$BINDER_SDK_INCLUDE_DIR")

    if [ "${#configure_prefix_args[@]}" -eq 0 ]; then
        log "  no AIDL/Binder staging prefix was supplied; configure will look for the sibling"
        log "  rdk-halif-aidl checkout and for the Binder SDK where it knows to look.  If it"
        log "  cannot find them it fails at configure time with the roots it tried, which is the"
        log "  intended outcome: the alternative is a libRCEC carrying only one back-end."
    else
        log "  AIDL/Binder staging prefixes passed to configure:"
        local prefix_arg
        for prefix_arg in "${configure_prefix_args[@]}"; do
            log "    $prefix_arg"
        done
    fi

    # WHY THE STEPS ARE CHAINED WITH EXPLICIT `&&` RATHER THAN LEFT TO `set -e`:
    # a compound command that is the left operand of `||` runs with errexit SUPPRESSED,
    # and that suppression reaches inside a subshell, so a `set -e` written in here would
    # NOT stop the sequence.  Measured consequence of getting this wrong: a configure that
    # failed on the GoogleTest check fell straight through to `make`, which then ran the
    # committed non-autotools Makefiles and buried the real cause under a cascade of
    # unrelated link errors.  The explicit `&&` chain short-circuits regardless of errexit
    # state, and the `###STEP:` markers make the failure attributable to one step.
    # THE ORDER OF THE NEXT THREE STATEMENTS IS THE WHOLE OF THE SNAPSHOT CONTRACT.
    #
    #   1. THE LOCK, first, because every one of the remaining steps mutates this tree and a
    #      second run interleaving with any of them can put its generated content back over
    #      your source.  It is idempotent -- main() already took it -- so this call confirms
    #      the same descriptor still refers to the same inode rather than taking it again.
    #   2. THE STUBS, second and as their own step, because they are the only mutation that
    #      has to happen before autoreconf and yet must not sit between the snapshot and it.
    #      See prepare_stub_headers for what it writes and what it verifies.
    #   3. THE SNAPSHOT, third, so that AUTORECONF IS THE VERY NEXT MUTATING COMMAND.  Nothing
    #      between them writes anything at all, so the snapshot is provably of pre-build
    #      content instead of being argued to be.
    #
    # THE SNAPSHOT IS ALSO OUTSIDE THE SUBSHELL, and that is a separate constraint: a variable
    # assigned inside a subshell is lost the moment it exits, and MAKEFILE_SNAPSHOT_DIR has to
    # survive into the exit trap that does the restoring.  So the snapshot cannot be a link of
    # the `&&` chain even though that is where it reads most naturally.
    acquire_tree_lock "building the middleware and both test suites"
    prepare_stub_headers "$build_log"
    snapshot_regenerated_makefiles

    (
        cd "$HDMICEC_ROOT" || exit 1
        echo '###STEP:autoreconf -if' &&
        autoreconf -if &&
        echo '###STEP:configure --enable-l1tests' &&
        PKG_CONFIG_PATH="$build_pkg_config_path" \
        CPPFLAGS="-I$HDMICEC_ROOT/mocks -I$HDMICEC_ROOT/stubs -I$GTEST_PREFIX/include" \
        LDFLAGS="-L$GTEST_PREFIX/lib -fprofile-arcs -ftest-coverage" \
        CXXFLAGS="-fprofile-arcs -ftest-coverage" \
        ./configure --enable-l1tests "${configure_prefix_args[@]}" &&
        echo '###STEP:make all' &&
        make -j"$nproc_count" all &&
        echo '###STEP:make -C tests/L1Tests all' &&
        make -C tests/L1Tests all &&
        echo '###STEP:make -C tests/L2Tests all' &&
        make -C tests/L2Tests all
    ) >>"$build_log" 2>&1 || {
        local failed_step
        failed_step="$( { grep '^###STEP:' "$build_log" | tail -n1 | sed 's/^###STEP://'; } || true )"
        die "the build failed during step: ${failed_step:-<none reached>}
       Full log: $build_log
       Tail:
$( { tail -n 25 -- "$build_log" | sed 's/^/         /'; } 2>/dev/null || true )
       If configure failed on GLIB, install libglib2.0-dev: configure.ac runs
       PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 0.10.28]) and there is no way past it.
       If configure failed on GTEST, point GTEST_PREFIX at a prefix whose lib/pkgconfig
       holds gtest.pc. It is currently $GTEST_PREFIX, so pkg-config was searched with
           PKG_CONFIG_PATH=$build_pkg_config_path
       Note that PKG_CHECK_MODULES ignores -I and -L: only pkg-config's path decides.
       If configure failed on the AIDL/Binder headers or libraries, it prints the roots it
       tried; supply the staging prefixes rather than adding -I or -L flags of your own,
       because configure derives every root from them and a flag added here would leave the
       hand-written ccec/src/Makefile configured differently. This run passed:
           ${configure_prefix_args[*]:-<none: configure looked for the sibling rdk-halif-aidl checkout>}
       If 'make -C tests/L2Tests all' failed, note that it builds TWO programs -- the L2
       runner and the out-of-process fake service host -- and that invocations D and E have
       no binary without both."
    }

    [ -x "$HDMICEC_ROOT/$TEST_BINARY_REL" ] || die "the build reported success but produced no
       executable at $HDMICEC_ROOT/$TEST_BINARY_REL. Full log: $build_log"
    log "build complete: $HDMICEC_ROOT/$TEST_BINARY_REL (log: $build_log)"
    announce_regenerated_makefiles
}

# ------------------------------------------------------------------------------------
# The build rewrote six git-tracked Makefiles.  THIS RUN PUTS THEM BACK ITSELF, from the
# snapshot it took before autoreconf, so this function's job is to say so rather than
# to hand the caller a command.
#
# WHY IT PRINTS NO COMMAND.  A `git -C <hdmicec> checkout -- <the six>` string offered here as
# a route would be the more dangerous of the two available, because a paste is unreviewed: it
# restores whatever the index holds and destroys any uncommitted edit to ccec/src/Makefile,
# which is hand-written source rather than generated output.  Making the action safe while
# leaving that advice standing would leave the footgun loaded and merely move the trigger, so
# neither exists: nothing here prints a command for anyone to run.
# ------------------------------------------------------------------------------------
announce_regenerated_makefiles() {
    rule
    warn "MANDATORY BUILD HYGIENE: autoreconf and configure rewrite six GIT-TRACKED files."
    warn "Five are local toolchain output.  ccec/src/Makefile is NOT: it is a hand-written"
    warn "  RDK build file that configure clobbers anyway, so restoring it puts source back."
    warn "None of the six may be committed in its generated form."
    if command -v git >/dev/null 2>&1 && git -C "$HDMICEC_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        local dirty
        dirty="$(git -C "$HDMICEC_ROOT" status --porcelain -- "${REGENERATED_MAKEFILES[@]}" 2>/dev/null || true)"
        if [ -n "$dirty" ]; then
            warn "currently modified:"
            printf '%s\n' "$dirty" | sed 's/^/[run_coverage]     /' >&2
        else
            warn "none of the six is currently modified."
        fi
    fi
    if [ -n "$MAKEFILE_SNAPSHOT_DIR" ]; then
        warn "THIS RUN WILL RESTORE THEM before it exits, from the snapshot it took of their"
        warn "  working-tree content immediately before autoreconf:"
        warn "    $MAKEFILE_SNAPSHOT_DIR"
        warn "  That happens on EVERY exit path -- success, build failure, gate failure and"
        warn "  Ctrl-C -- because the restore hangs off this script's exit trap.  You do not"
        warn "  need to run anything, and there is nothing to paste."
        warn "DO NOT revert these paths with 'git checkout'.  It restores what the INDEX holds,"
        warn "  not what was there, so it would silently destroy an uncommitted edit to any of"
        warn "  the six.  This script offers no such route in any form."
    else
        # Reachable only if do_build is ever called without its snapshot step, which would be
        # a defect in this script rather than a caller error -- so it says exactly that.
        warn "NO SNAPSHOT WAS TAKEN FOR THIS RUN, which should not be possible: do_build takes"
        warn "  one before autoreconf.  The six cannot be restored automatically, and this"
        warn "  script will not reach for 'git checkout' to cover for its own defect.  Compare"
        warn "  them against your own copy and report this."
    fi
}

# ------------------------------------------------------------------------------------
# `--restore` as a standalone action.  IT SUCCEEDS ONLY WHEN IT ACTUALLY RESTORED SOMETHING.
#
# THE ONE AUTHORITY IS A SNAPSHOT FROM THIS SAME INVOCATION, and nothing else is admitted.
# The snapshot holds the WORKING-TREE bytes and modes as this run found them, per path, with
# presence recorded; it is the only record that can put the tree back the way it was.
#
# WHAT IT MUST NOT DO.  With no snapshot, falling back to asking git whether the six differ
# from the INDEX -- and printing "nothing to restore" with a 0 status when they do not -- is a
# success status for an invocation that restored nothing and, worse, it
# is a success status derived from the wrong reference:
#
#   * clean-against-the-index does not mean unchanged-since-this-run-started.  A tree whose
#     six were committed in their GENERATED form reads as perfectly clean while holding exactly
#     the content this mechanism exists to remove.
#   * a path that was already modified before any build started reads as dirty and always
#     will, so the same reference calls a correct state a failure.
#
# Either way the caller asked for a restore and did not get one, so the honest answer is an
# error and a non-zero status.  git is still consulted, and what it says is still printed --
# as INFORMATION about the tree, never as the basis for the verdict.
#
# AND IT NEVER REACHES FOR `git checkout`.  That is the mechanism this whole snapshot machinery
# stands in place of: it restores what the index holds rather than what was there, so on
# ccec/src/Makefile -- hand-written RDK build source that configure clobbers because it is in
# AC_CONFIG_FILES --
# it would silently destroy an uncommitted edit.  The one code path a caller invokes precisely
# when something has already gone wrong must not be the one that loses their work.
# ------------------------------------------------------------------------------------
do_restore() {
    rule
    log "checking the tracked Makefiles in $HDMICEC_ROOT"

    # A snapshot from this same invocation is the only thing that can be restored from, and the
    # only way to have one is to have run --build here -- which parse_args forbids combining
    # with --restore.  So this arm exists for the internal caller (the exit trap) rather than
    # for the flag, and it is kept for exactly one reason: if the two are ever allowed to
    # combine, this function must restore rather than refuse.  Its STATUS IS PROPAGATED: a
    # restore that could not put every path back is not a successful --restore.
    if [ -n "$MAKEFILE_SNAPSHOT_DIR" ]; then
        restore_regenerated_makefiles_from_snapshot || return 1
        return 0
    fi

    # Whatever git can tell us is printed, because it is genuinely useful to see -- and then
    # the refusal happens regardless of what it said.
    if command -v git >/dev/null 2>&1 \
       && git -C "$HDMICEC_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        local before
        before="$(git -C "$HDMICEC_ROOT" status --porcelain -- "${REGENERATED_MAKEFILES[@]}" 2>/dev/null || true)"
        if [ -n "$before" ]; then
            printf '%s\n' "$before" | sed 's/^/[run_coverage]   differs from the index: /' >&2
        else
            warn "none of the allowlisted paths differs from the index right now."
            warn "  THAT IS NOT THE SAME AS 'nothing to restore', which is why it does not make"
            warn "  this invocation a success: the index is not the reference.  A tree whose"
            warn "  Makefiles were committed in their generated form reads as clean while"
            warn "  holding exactly the content that needs replacing."
        fi
    else
        warn "git is not usable here, so not even the index comparison is available."
    fi

    die "THIS INVOCATION HAS NO SNAPSHOT, so there is nothing it can authoritatively restore
       from, and nothing was changed.
           the allowlisted paths: ${REGENERATED_MAKEFILES[*]}
       WHY THIS IS AN ERROR RATHER THAN 'nothing to do'.  A snapshot records the WORKING-TREE
       bytes, the modes and the presence of each path as this run found them, and it exists
       only inside the invocation that took it.  Without one, this script cannot tell what any
       of these paths looked like before -- so reporting success would be reporting that a
       restore happened when none did.
       WHAT IT WILL NOT DO INSTEAD.  It will not run
           git -C $HDMICEC_ROOT checkout -- <the allowlisted paths>
       in this or any other mode.  That restores whatever the INDEX holds rather than what was
       there, so on ccec/src/Makefile -- a hand-written RDK build file with its own object list
       and link command, which configure clobbers because it is in AC_CONFIG_FILES -- it would
       silently destroy an uncommitted edit.  Losing someone's source is not an acceptable way
       to tidy up generated content.  It will not substitute the index comparison above for a
       verdict either, for the reason printed with it.
       WHAT TO DO INSTEAD.  Let the build take a snapshot and put them back for you:
           $SCRIPT_PATH --build --run
       That snapshots every allowlisted path before autoreconf and restores it from the exit
       trap on every path out -- success, build failure, gate failure and Ctrl-C -- so there is
       normally nothing left for --restore to do.  If you already know these paths hold nothing
       you want, revert them however you normally revert a file: that decision is yours to take
       deliberately, and it is not one a measurement tool should take for you."
}

# ====================================================================================
# THE INVOCATION MATRIX
# ====================================================================================
#
# WHY THERE IS A MATRIX AT ALL, rather than one run.  The middleware compiles BOTH HAL
# back-ends into libRCEC and chooses between them ONCE PER PROCESS, inside LibCCEC::init,
# which is the first thing either harness does that forces Driver::getInstance().  By the
# time any TEST_F body runs the choice is made and cannot be changed, so ONE BINARY RUN
# YIELDS EXACTLY ONE SELECTION OUTCOME and every outcome that has to be covered needs its
# own process.  A single run cannot exercise both arms of the selection branch no matter
# what it is filtered to.
#
# THE FIVE INVOCATIONS, and what each is the only way to establish:
#
#   A  run_L1Tests  CEC_TEST_AIDL_MODE=absent        expects the LEGACY back-end
#      No service is registered and no host is launched.  This is the regression arm: the
#      whole pre-existing suite plus every case in the new contract suite that does not
#      require the AIDL back-end.  It is also the arm that demonstrates the
#      fallback-not-abort requirement on the one platform that naturally exhibits it -- a
#      host with no binder driver at all, where an unguarded service lookup would abort the
#      process rather than return an error.
#
#   B  run_L1Tests  CEC_TEST_AIDL_MODE=compatible    expects the AIDL back-end
#      The harness registers a compatible fake IN THIS PROCESS before init.  This is the arm
#      that proves the ADAPTER translates correctly -- array marshalling, status mapping,
#      the frame-length guard, the state guards.
#
#   C  run_L1Tests  CEC_TEST_AIDL_MODE=incompatible  expects the LEGACY back-end
#      The harness registers a fake that reports an interface hash of "-1".  This is the arm
#      that proves "present but not usable falls back", and it CANNOT be replaced by a unit
#      test: that is a FACTORY-level behaviour, and the selection helper that acts on the
#      answer (resolveBackEnd in ccec/src/Driver.cpp) sits in an anonymous namespace, so it
#      has internal linkage and no test can call it.  A unit test of the predicate proves the
#      predicate; only a process that STARTS with an incompatible service registered proves
#      the factory acts on it.  It is also only feasible IN-PROCESS: a remote Bn* service
#      cannot report bad metadata at all, because its generated onTransact answers those
#      transactions from compiled-in constants.
#
#   D  run_L2Tests  CEC_TEST_AIDL_MODE=absent        expects the LEGACY back-end
#      The end-to-end round trip -- HAL event to Bus reader to Connection to FrameListener --
#      driven through the legacy in-process mock.
#
#   E  run_L2Tests  CEC_TEST_AIDL_MODE=remote        expects the AIDL back-end
#      The L2 harness launches the out-of-process fake service host, waits for its readiness
#      token, and only then initializes.  The middleware holds a real Bp* proxy, transactions
#      cross the binder driver, and the listener callback arrives on a binder threadpool
#      thread.
#
# B AND E ARE BOTH REQUIRED AND NEITHER SUBSTITUTES FOR THE OTHER, which is worth stating
# because five invocations look like more than are needed.  An in-process registration IS NOT
# IPC: libbinder resolves a name registered in the calling process to the local BBinder, so
# interface_cast hands back that very object -- no proxy is created, no transaction crosses
# the driver, and the client threadpool is never involved.  B therefore proves translation
# and cannot prove transport; E proves transport and, because a remote service cannot report
# bad metadata, cannot reach the compatibility-rejection arms B and C reach.
#
# ------------------------------------------------------------------------------------
# THE FILTERS ARE MEASURED, NOT ESTIMATED -- and no count written down here is a gate input.
#
# --gtest_filter selects by SUITE name, and several source files declare more than one suite,
# so a filter derived from file names would be wrong in a way nothing would catch.  The two
# suite groups below were read out of the built binary with --gtest_list_tests, and the case
# counts in the comments are what that listing reported on the host this was written on.
#
# THOSE COUNTS ARE DOCUMENTATION.  The gate does not use them: run_one_invocation asks the
# binary itself how many cases its own filter selects, every time.  That is deliberate, and it
# is what the two test files ask for in as many words -- adding a case must not require
# editing an expected number here, because a stale number is a gate that fails on a correct
# build and, worse, one that a caller then learns to override.
# ------------------------------------------------------------------------------------

# BACK-END-NEUTRAL: ten suites, 308 cases as measured, containing no reference to either HAL
# back-end -- no mock, no EXPECT_CALL, no Driver::getInstance.  They run under EVERY
# invocation, unchanged and unmodified, and that is the backward-compatibility evidence: the
# same cases, the same assertions, passing with either back-end resolved.
readonly NEUTRAL_SUITES='CECFrameTest.*:MessageDecoderTest.*:MessageDecoderTrackingTest.*:MessageEncoderTest.*:OpCodeTest.*:OperandsTest.*:UtilTest.*:ConditionVariableTest.*:MutexTest.*:ThreadTest.*'

# LEGACY-BOUND: eight suites, 175 cases as measured, whose EXPECT_CALLs name HdmiCec*
# functions a binder back-end never calls.  Under AIDL selection they would fail on unmet
# expectations rather than on behaviour, which is a false negative and not evidence of
# anything, so they run on invocation A only.  Their behavioural content is re-asserted
# back-end-agnostically by the contract suite, which is what keeps nothing dropped.
# Two of them also hold the ill-typed static_cast<DriverImpl &> route through the legacy
# receive callback, so restricting them to A makes that route structurally unreachable under
# AIDL selection rather than merely unused.
readonly LEGACY_BOUND_SUITES='BusTest.*:ConnectionTest.*:IntegrationFlowTest.*:DriverTest.*:DriverImplAsyncTest.*:HdmiCecDriverMockTest.*:LibCCECTest.*:LibCCECUninitializedTest.*'

# The contract suite's own fixtures, partitioned by which back-end each one requires.  Read
# from the FIXTURE MANIFEST in tests/L1Tests/ccec/test_DriverAidl.cpp, which is the authority
# for its own case set, and cross-checked against the built binary.
#   back-end independent  Compatibility 23, Preflight 28, LocalInstance 25  = 76, under A, B and C
#   legacy back-end only  Selection 4, LegacyArm 5                          =  9, under A only
#   AIDL back-end only    Session 26, Transmit 12                           = 38, under B only
# That is 123 contract cases, so run_L1Tests registers 606 in total: the 483 pre-existing cases
# plus these 123.  Under the filters below that comes out as 568 selected and 38 excluded on
# invocation A, 422 and 184 on B, and 384 and 222 on C -- figures the reconciliation in
# run_one_invocation measures from the binary rather than reading from here.
readonly CONTRACT_ANY_BACKEND_SUITES='DriverAidlCompatibilityTest.*:DriverAidlPreflightTest.*:DriverAidlLocalInstanceTest.*'
readonly CONTRACT_LEGACY_ONLY_SUITES='DriverAidlSelectionTest.*:DriverAidlLegacyArmTest.*'
readonly CONTRACT_AIDL_ONLY_SUITES='DriverAidlSessionTest.*:DriverAidlTransmitTest.*'

# The L2 tier's own filter.  All three of its fixtures share the DualPath prefix precisely so
# that one glob selects the tier, and the registered total is IDENTICAL for D and E by
# design: the two arm-specific fixtures SKIP rather than FAIL when the resolved back-end is
# not theirs, so every case is registered and reported under both and only the pass/skip
# split differs.  That is what lets one expected count serve both invocations.
readonly L2_SUITES='DualPath*'

# ------------------------------------------------------------------------------------
# THE MATRIX, one record per invocation, '|'-separated:
#
#   1 letter            the invocation's name, and the suffix on every artifact it writes
#   2 tier              L1 or L2 -- which runner, which directory, which fixture pattern
#   3 mode              the CEC_TEST_AIDL_MODE value handed to the harness
#   4 back-end          the back-end name that MUST appear in the selected-path log line
#   5 select filter     --gtest_filter for the run; empty means "no filter"
#   6 exclude filter    the COMPLEMENT, spelled out as suite globs rather than derived
#   7 synopsis          one line, printed when the invocation starts
#   8 permitted skips   the ONLY cases this invocation may report as SKIPPED, as gtest globs.
#                       Empty means none: any skip fails the invocation.
#
# FIELD 8 IS THE MANDATORY-PASS CONTRACT, and it exists because a skipped case counts as an
# executed one everywhere else.  The count check compares the JSON's "tests" against the
# filter's selection, and GoogleTest counts a case it SKIPPED among "tests" -- so without this
# field an invocation whose cases all skipped in SetUp reconciles perfectly, exits 0, and
# reports a full complement of evidence it did not produce.
# On invocation E that is not a hypothetical: its four AIDL-flow cases are the
# only proof this suite ever has that a frame crosses real binder IPC and arrives on a binder
# thread, and they skip rather than fail when the resolved back-end is not theirs.  A green E
# with those four skipped is the exact false green this whole matrix exists to prevent.
#
# WHAT IT IS NOT: a count.  A tolerance of "up to four skips" would accept four skipped
# MANDATORY cases while four permitted ones ran, which is the same false green wearing a
# different number.  So the permitted set is named, every skip is matched against it, and a skip
# that does not match is fatal WITH ITS NAME.  The mandatory count is then MEASURED from the
# binary as "the selection minus the permitted set", never written down, so adding a case to a
# fixture needs no edit here -- and the numbers it derives on this host are the settled contract:
# D has 11 mandatory (15 selected less the 4 permitted DualPathAidlFlowTest cases) and E has 9
# (15 less the 6 permitted DualPathLegacyFlowTest cases).  Read off the built binary with
# --gtest_list_tests, the tier registers 15: DualPathSelectionTest 4, DualPathLegacyFlowTest 6,
# DualPathAidlFlowTest 4 and DualPathHostLifecycleTest 1.  So D's eleven are the four selection
# cases, the six legacy-flow cases and the one lifecycle case, and E's nine are the four
# selection cases, the four AIDL-flow cases and the same one lifecycle case -- which is the
# fixture that regression-pins the group-wide bounded teardown and is back-end independent, so
# it is mandatory under BOTH.
#
# These figures moved from 14/10/8 when that lifecycle fixture was added, and the fact that
# only this COMMENT had to move is the property being described: the mandatory count is
# measured, so the mechanism needed no edit at all.
#
# THE TWO L2 INVOCATIONS ARE MIRROR IMAGES, which is the point of naming rather than counting:
# the AIDL-flow fixture is PERMITTED to skip under D (legacy is resolved, so it cannot run) and
# MANDATORY under E, and the legacy-flow fixture is the other way round.  One number could never
# express that.  The L1 invocations permit nothing: their arm-specific fixtures assert in SetUp
# and FAIL rather than skip, deliberately, so a skip there is already a defect.
#
# WHY FIELD 6 EXISTS RATHER THAN BEING COMPUTED.  The check it feeds is
# `selected + excluded == registered`, measured all three times from the binary, and that is
# the generalisation of a guard against a suite that goes unrun.  Comparing the executed count
# against the UNFILTERED inventory and raising an advisory on any gap is exactly right when
# nothing is filtered, and fires on every filtered invocation by design once a matrix exists.
# Deriving the complement arithmetically (registered - selected) would prove nothing at all,
# because both sides would come from the same subtraction.  Spelling it out as suite globs and
# MEASURING it makes the reconciliation real, and it buys what an unfiltered comparison cannot:
# a suite added to the binary and classified into neither group breaks the arithmetic and
# fails the invocation, instead of silently going unrun on four invocations out of five.
#
# INVOCATION A CARRIES A NEGATIVE FILTER, AND IT IS NOT OPTIONAL.  The two AIDL-only fixtures
# assert in SetUp that the AIDL back-end is the resolved one, and they FAIL rather than SKIP
# when it is not -- deliberately, because a skipped arm is indistinguishable from a passing
# one in an aggregate count, which is the swallowed-failure shape the whole suite is built to
# avoid.  So A excludes them by name.  Everything else in the binary runs.
# ------------------------------------------------------------------------------------
readonly L2_LEGACY_FLOW_SUITE='DualPathLegacyFlowTest.*'
readonly L2_AIDL_FLOW_SUITE='DualPathAidlFlowTest.*'

readonly INVOCATION_MATRIX=(
    "A|L1|absent|legacy|-${CONTRACT_AIDL_ONLY_SUITES}|${CONTRACT_AIDL_ONLY_SUITES}|no service registered: the regression arm, and the fallback-not-abort demonstration|"
    "B|L1|compatible|AIDL|DriverAidl*:${NEUTRAL_SUITES}-${CONTRACT_LEGACY_ONLY_SUITES}|${LEGACY_BOUND_SUITES}:${CONTRACT_LEGACY_ONLY_SUITES}|in-process compatible fake: the adapter-translation arm|"
    "C|L1|incompatible|legacy|${CONTRACT_ANY_BACKEND_SUITES}:${NEUTRAL_SUITES}|${LEGACY_BOUND_SUITES}:${CONTRACT_LEGACY_ONLY_SUITES}:${CONTRACT_AIDL_ONLY_SUITES}|in-process fake reporting hash \"-1\": present but not usable falls back|"
    "D|L2|absent|legacy|${L2_SUITES}||legacy round trip through the in-process mock, end to end|${L2_AIDL_FLOW_SUITE}"
    "E|L2|remote|AIDL|${L2_SUITES}||out-of-process fake over real binder IPC, end to end|${L2_LEGACY_FLOW_SUITE}"
)

# ------------------------------------------------------------------------------------
# WHICH INVOCATIONS THIS HOST CAN ACTUALLY RUN.
#
# Compiling and linking the AIDL back-end needs only headers and libraries and is fully
# verifiable anywhere.  EXECUTING an AIDL-selected case needs three more things: a kernel with
# binder support, a binder protocol version matching the one libbinder was built for, and a
# running service manager.  Where the driver node is absent, invocations B, C and E cannot run
# AT ALL -- and not merely because the lookup would fail.  Registering a name requires
# defaultServiceManager(), which opens the driver, and on the pinned binder stack that call
# ABORTS the process when the node is missing.  So an in-process fake is as unreachable as a
# remote one, and an attempt would take the whole runner down with it.
#
# The consequence for this script is a hard rule: an invocation that cannot run is REPORTED AS
# DEFERRED AND NEVER ATTEMPTED.  It is not launched, not counted as passed, and never allowed to
# stand in for evidence it did not produce.  DEFERRED is not the same thing as SKIPPED, and the
# distinction is deliberate: a skip is something GoogleTest does to a case inside a process that
# started, whereas a deferred invocation never starts a process at all, so none of its cases is
# reported in any state.  The final report says which invocations ran and which did not, and a
# run that deferred any of them cannot produce an acceptance verdict.
#
# The probe is the driver node itself, checked exactly as the middleware's own bounded
# preflight checks it first: does the node exist and can it be opened.  This script does NOT
# reimplement the rest of that predicate -- the protocol-version equality check and the bounded
# wait for binder handle 0 are production code in the middleware and are not duplicated here.
# What this probe answers is narrower and is all it claims: "is there any point attempting an
# AIDL-selected invocation on this host".
# ------------------------------------------------------------------------------------
BINDER_DRIVER_NODE="${BINDER_DRIVER_NODE:-/dev/binder}"
# A CONTROL CHARACTER IN THE PROBED NODE PATH IS REFUSED AT THIS ONE BOUNDARY, which is what lets
# the three transport diagnostics that name it keep interpolating it directly.  Empty is refused
# too: `[ -e "" ]` is false, so an empty value would present itself as "the binder driver is
# absent" and defer every AIDL-selected invocation for a reason that was never true.
case "$BINDER_DRIVER_NODE" in
    '')
        die "BINDER_DRIVER_NODE was given as an empty value.  An empty path tests as absent, so
       every AIDL-selected invocation would be deferred with a reason that is not the real one.
       Leave it unset for /dev/binder, or name a real node." ;;
    *[[:cntrl:]]*)
        die "BINDER_DRIVER_NODE contains a control character:
       $(render_untrusted "$BINDER_DRIVER_NODE")
       This path is reproduced in the transport diagnostics that report whether the AIDL
       invocations can run, so a newline or an escape sequence in it would forge or hide output
       rather than name a node.  Give a path made of printable characters." ;;
esac
readonly BINDER_DRIVER_NODE

binder_transport_present() {
    [ -e "$BINDER_DRIVER_NODE" ] || return 1
    # Readable-and-writable rather than merely present: the node can exist with permissions
    # that make it useless, and a run that got past this check only to abort inside libbinder
    # would report a crash where it should have reported a deferral.
    [ -r "$BINDER_DRIVER_NODE" ] && [ -w "$BINDER_DRIVER_NODE" ]
}

# ------------------------------------------------------------------------------------
# THE RUNTIME LIBRARY PATH FOR THE BINDER CLOSURE.
#
# libRCEC now links four libraries directly -- the two generated AIDL stub libraries, libbinder
# and libutils -- and libbinder itself pulls in liblog, libbase, libcutils and libcutils_sockets
# beneath it.  Those four are NOT direct edges of anything in the middleware and are deliberately
# not named as such, but the loader still has to find every one of them or the runner does not
# start.  So this covers the DIRECTORIES rather than enumerating the libraries: whatever the
# staging layout puts in them is what the closure needs.
#
# WHY THIS IS NOT A NO-OP EVEN WHERE ldconfig ALREADY RESOLVES THEM.  On a host whose loader
# cache has been taught about the staged libraries this adds nothing, and that is fine -- a
# duplicated directory on the search path costs a stat.  On a host where they are staged and NOT
# registered, which is the ordinary case for a build tree, it is the difference between a suite
# that runs and one that dies with "cannot open shared object file" before its first test.
# Handling only the first case would make this script work on the machine it was written on.
#
# Each directory is added only if it exists, so an unset prefix contributes nothing rather than
# an empty path element -- an empty element in LD_LIBRARY_PATH means the CURRENT DIRECTORY,
# which is a genuine hazard and not merely untidy.
# ------------------------------------------------------------------------------------
# AN INHERITED LOADER PATH IS UNTRUSTED INPUT TO A PROCESS THAT LOADS SHARED OBJECTS, so the
# search path handed to the test binaries is REBUILT from this script's own resolved roots
# rather than filtered out of whatever the caller happened to export.
#
# WHAT THIS REPLACES, stated plainly because the shape of the old code is what made it wrong.
# The path used to be SEEDED with ${LD_LIBRARY_PATH:-} and the trusted roots PREPENDED to it.
# Prepending changes precedence and nothing else: the inherited tail survived intact and was
# exported to the runner, so every directory on the caller's loader path was searched by the
# test binaries this script launches.  That is not a theoretical exposure here -- the
# hand-built libRCEC and the staged Binder closure carry no RUNPATH, so EVERY soname they need
# is resolved by searching, and the first directory that answers wins.  A shadowing
# libbinder.so, libutils.so, libgtest.so or any member of the transitive closure, placed in a
# directory an attacker can write and reached through a loader path nobody reviews, is loaded in
# preference to nothing at all -- into a process that opens the binder driver, registers a
# global service name, and is routinely run with elevated privileges in CI.  CWE-427, and the
# fix is to stop treating the ambient environment as an input to the search path.
#
# WHY REBUILDING RATHER THAN FILTERING.  A filter has to be right about every entry it keeps,
# and an entry kept by mistake is indistinguishable from one it was told to keep.  Rebuilding
# inverts the default: the only directories the child can search are the ones this script
# resolved for itself and then proved safe, so a new hazard has to get past an admission check
# to appear at all.  Little is lost in the ordinary case either -- the staged roots are exactly
# what a working environment puts on LD_LIBRARY_PATH in the first place, and here they are named
# from configuration this run prints in its own log.
#
# WHAT "TRUSTED" MEANS HERE, and it is a narrower claim than the artifact-path checks above
# make.  A candidate is admitted only when all of these hold:
#   * it exists and is a directory -- a name that resolves to nothing contributes nothing, and
#     a non-directory on a search path is a mis-staged prefix rather than a library store;
#   * it is not a symbolic link.  A link is a name whose target can be repointed after the check
#     without the name changing, which is the substitution being defended against; resolving it
#     and admitting the target would validate one thing and search another;
#   * it is an absolute path.  A relative element is resolved against the working directory of
#     the process that inherits it -- and the launch below cds into the binary's directory --
#     so a relative root is a search path that moves with the process;
#   * it is owned by this effective user or by root.  Anyone else owning it can replace its
#     contents between this check and the load;
#   * it is NOT group- or world-writable, AND THERE IS NO STICKY-BIT EXEMPTION, which is where
#     this deliberately differs from assert_component_safe.  The sticky bit stops a non-owner
#     REMOVING or RENAMING an entry, which is the whole risk for a directory this script writes
#     artifacts into.  It does not stop anyone CREATING one -- and creating libbinder.so where
#     the loader will look for it is the entire attack, so sticky buys nothing here and
#     accepting it would reintroduce the finding under a different mode.
#
# THE ANCESTRY IS DELIBERATELY NOT WALKED, unlike the artifact paths.  Those are paths this
# script WRITES to, where a substituted parent redirects the write; this is a path the loader
# READS from, and these roots are named by this run's own configuration rather than pre-created
# by somebody else.  Requiring a safe ancestry would also reject the ordinary case outright: a
# staged prefix or a build tree under a conventional /tmp (measured on this host: mode 2777,
# world-writable, no sticky bit) would fail on its grandparent, and a check that refuses to run
# on the normal arrangement gets deleted rather than obeyed.  What is searched is the candidate
# itself, so the candidate itself is what is held to the rules.
#
# THE SCOPE OF THE CLAIM, stated as what is and is not guaranteed.  This block governs the
# loader's SEARCH PATH: which directories a child may search for a soname, and nothing else.
#
# WHAT IS GUARANTEED, and by which mechanism:
#   * the search path a child inherits contains only roots this run resolved from its own
#     configuration and then held to the admission rules above -- this block;
#   * no direct-object loader variable is in force anywhere in the run.  LD_PRELOAD, LD_AUDIT,
#     LD_PROFILE, LD_DYNAMIC_WEAK and LD_ORIGIN_PATH name objects to load or change how symbols
#     bind, so this block could never have covered them; they are REFUSED at load time by
#     assert_no_loader_object_variables -- before this script spawns any child of its own --
#     and removed again from the environment of each launch by scrub_loader_object_variables.
#     That is a different mechanism for a different attack, and both are needed.
#
# WHAT IS NOT GUARANTEED, said plainly rather than implied:
#   * a DT_RPATH or DT_RUNPATH compiled into a binary or a library is searched before
#     LD_LIBRARY_PATH and is not reachable from any environment variable this script controls.
#     The refusal of LD_ORIGIN_PATH removes the one way an inherited variable could redirect an
#     $ORIGIN-relative RUNPATH entry; a literal RUNPATH remains a property of the artifact, and
#     the measured artifacts here carry none;
#   * /etc/ld.so.conf, /etc/ld.so.preload and the loader cache are system configuration, owned
#     by the host and outside anything a test runner can assert;
#   * a library that is genuinely inside an admitted root is loaded.  The rules above are about
#     WHO CAN PUT A FILE THERE, not about what the file contains.
#
# NOTHING IS DROPPED SILENTLY, in either direction.  A candidate that fails a SECURITY property
# is fatal, because admitting it would import the exposure while quietly skipping it would
# surface three steps later as "cannot open shared object file" with nothing naming the cause.
# A candidate that simply is not there is reported and skipped, because a derived path such as
# HALIF_PREFIX/lib/halif legitimately does not exist in a split staging layout -- it is still
# named, so a mis-staged prefix is visible now rather than mysterious later.  And an inherited
# LD_LIBRARY_PATH is reported entry by entry, so a caller who relied on it learns that it was
# discarded on purpose instead of debugging a load failure.
# ------------------------------------------------------------------------------------

# The ONE place the Binder/HALIF runtime roots are named.  It populates an array rather than
# printing, so its caller runs in THIS shell and a fatal verdict on a candidate ends the run
# rather than ending a command substitution.
#
# The order is the order the prepending version added them in, and it is preserved so that this
# change alters WHAT is searched and not the order in which it is searched.
BINDER_RUNTIME_LIBRARY_ROOTS=()
binder_runtime_library_roots() { # -> BINDER_RUNTIME_LIBRARY_ROOTS
    BINDER_RUNTIME_LIBRARY_ROOTS=()
    [ -z "$HALIF_LIB_DIR" ]  || BINDER_RUNTIME_LIBRARY_ROOTS+=("$HALIF_LIB_DIR")
    [ -z "$HALIF_PREFIX" ]   || BINDER_RUNTIME_LIBRARY_ROOTS+=("$HALIF_PREFIX/lib/halif")
    [ -z "$BINDER_SDK_DIR" ] || BINDER_RUNTIME_LIBRARY_ROOTS+=("$BINDER_SDK_DIR/lib/binder" \
                                                               "$BINDER_SDK_DIR/lib")
}

# One candidate's verdict, and no exit of its own: the caller decides what a verdict costs.
# That split is what keeps the rules in a single copy and lets them be exercised directly.
#
#   admit   -- every property above holds; it may go on the loader path.
#   absent  -- there is nothing there to admit.  Reported by the caller, not fatal.
#   unsafe  -- it exists and violates a security property.  Fatal at the caller.
TRUSTED_LIBRARY_DIR_VERDICT=''
TRUSTED_LIBRARY_DIR_REASON=''
classify_trusted_library_dir() { # $1=candidate directory -> 0 admit, 1 reject (verdict+reason set)
    local candidate="${1:-}" meta uid rest mode kind numeric_mode

    TRUSTED_LIBRARY_DIR_VERDICT='unsafe'
    TRUSTED_LIBRARY_DIR_REASON=''

    if [ -z "$candidate" ]; then
        TRUSTED_LIBRARY_DIR_VERDICT='absent'
        TRUSTED_LIBRARY_DIR_REASON='the name is empty, and an empty loader-path element names the
       current working directory rather than nothing'
        return 1
    fi

    case "$candidate" in
        /*) ;;
        *)  TRUSTED_LIBRARY_DIR_REASON="it is not an absolute path, so it would be resolved against
       the working directory of whichever process inherited it"
            return 1 ;;
    esac

    # lstat semantics, and BEFORE the metadata read, so a link is rejected as a link rather than
    # reported as whatever it points at.
    if [ -L "$candidate" ]; then
        TRUSTED_LIBRARY_DIR_REASON="it is a symbolic link, whose target can be repointed after this
       check without the name changing.  Name the real directory instead"
        return 1
    fi

    meta="$(path_metadata "$candidate")"
    if [ -z "$meta" ]; then
        TRUSTED_LIBRARY_DIR_VERDICT='absent'
        TRUSTED_LIBRARY_DIR_REASON='it does not exist, or its ownership and permissions cannot be read'
        return 1
    fi

    uid="${meta%% *}"
    rest="${meta#* }"
    mode="${rest%% *}"
    kind="${rest#* }"

    if [ "$kind" != "directory" ]; then
        TRUSTED_LIBRARY_DIR_REASON="it is a $kind, not a directory"
        return 1
    fi

    if [ "$uid" != "$EUID_VALUE" ] && [ "$uid" != "0" ]; then
        TRUSTED_LIBRARY_DIR_REASON="it is owned by uid $uid, which is neither this user ($EUID_VALUE)
       nor root, so its owner can replace what is inside it between this check and the load"
        return 1
    fi

    # No sticky-bit exemption, for the reason given at length above: sticky stops removal and
    # renaming, not creation, and creating a shadowing library is the attack.
    numeric_mode="$(( 8#$mode ))"
    if [ "$(( numeric_mode & 0022 ))" -ne 0 ]; then
        TRUSTED_LIBRARY_DIR_REASON="its mode is $mode -- writable by group or world -- so any local
       account able to write it can place a shadowing shared object where the loader will find it"
        return 1
    fi

    TRUSTED_LIBRARY_DIR_VERDICT='admit'
    return 0
}

# The loader path the launched test binaries receive: built from the roots above and from
# nothing else.
#
# BUILT ONCE PER RUN and memoised, so the diagnostics are printed once rather than once per
# invocation of the matrix.  That is sound because every input is resolved configuration that
# does not change during a run -- the four staging variables and GTEST_PREFIX are fixed at
# startup, and the first call happens after any --build step has finished staging.
TRUSTED_LIBRARY_PATH=''
TRUSTED_LIBRARY_PATH_BUILT=0
build_trusted_library_path() {
    [ "$TRUSTED_LIBRARY_PATH_BUILT" -eq 0 ] || return 0

    local inherited="${LD_LIBRARY_PATH:-}"
    local path='' candidate entry
    local -a candidates=()

    # GTest goes on first and the Binder/HALIF roots are prepended over it, which reproduces the
    # precedence the previous version produced.
    [ -z "$GTEST_PREFIX" ] || candidates+=("$GTEST_PREFIX/lib")
    binder_runtime_library_roots
    candidates+=("${BINDER_RUNTIME_LIBRARY_ROOTS[@]:-}")

    for candidate in "${candidates[@]:-}"; do
        [ -n "$candidate" ] || continue
        if ! classify_trusted_library_dir "$candidate"; then
            case "$TRUSTED_LIBRARY_DIR_VERDICT" in
                absent)
                    log "loader path: NOT admitted -- $candidate"
                    log "             ($TRUSTED_LIBRARY_DIR_REASON)"
                    ;;
                *)
                    die "the staged library directory
           $candidate
       cannot be put on the loader path of the test binaries this script launches, because
       $TRUSTED_LIBRARY_DIR_REASON.
       Those binaries and the staged Binder closure carry no RUNPATH, so every shared object
       they need is resolved by searching this path and the first directory that answers wins.
       This run will not search a directory it cannot vouch for, and will not drop one silently
       either: dropping it would reappear later as 'cannot open shared object file' with nothing
       naming the cause.
       Fix the staging rather than the check:
           chmod g-w,o-w '$candidate'      (if it is writable by group or world)
           chown $EUID_VALUE '$candidate'  (if it is owned by somebody else)
       or point GTEST_PREFIX / HALIF_LIB_DIR / HALIF_PREFIX / BINDER_SDK_DIR at a real
       directory you own."
                    ;;
            esac
            continue
        fi
        # Already present?  Do not add it twice; a search path that grows on every call is how
        # a long-running loop ends up with an environment too large to exec.
        case ":$path:" in
            *":$candidate:"*) continue ;;
        esac
        path="$candidate${path:+:$path}"
    done

    TRUSTED_LIBRARY_PATH="$path"
    TRUSTED_LIBRARY_PATH_BUILT=1

    if [ -n "$path" ]; then
        log "loader path for the test binaries (rebuilt from validated roots): $path"
    else
        # Legitimate when the whole closure is registered with ldconfig, and the cause of a load
        # failure when it is not -- so it is named either way rather than assumed.
        log "loader path for the test binaries is EMPTY: no staged library directory was admitted,
       so the runners must resolve libbinder, libutils, the AIDL stubs and gtest through the
       system loader cache alone.  If they cannot, set GTEST_PREFIX, HALIF_LIB_DIR and
       BINDER_SDK_DIR to the staged prefixes."
    fi

    # WHAT THE CALLER'S OWN LD_LIBRARY_PATH WAS, AND THAT IT WENT NOWHERE.  A user who exported
    # it -- the shared environment file in this workspace does -- is entitled to know why the run
    # behaves differently rather than to discover it through a load failure.  Every entry is
    # named, and an entry the rebuilt path supplies anyway is marked as such, because "dropped"
    # and "dropped but also admitted from a trusted root" are different facts to a reader.
    if [ -n "$inherited" ]; then
        local -a inherited_entries=()
        log "an inherited LD_LIBRARY_PATH was present in this script's own environment and was
       deliberately NOT propagated to the test binaries: an inherited loader path is untrusted
       input to a process that loads shared objects, so the path above was rebuilt from this
       script's resolved roots instead of filtered.  Entry by entry:"
        IFS=':' read -r -a inherited_entries <<<"$inherited"
        for entry in "${inherited_entries[@]:-}"; do
            if [ -z "$entry" ]; then
                log "             dropped: <empty element -- names the current directory>"
                continue
            fi
            case ":$path:" in
                *":$entry:"*)
                    log "             dropped: $entry" \
                        "(also admitted from a trusted root, so nothing is lost)" ;;
                *)  log "             dropped: $entry" ;;
            esac
        done
    fi
}

# Does this invocation need the binder transport in order to run at all?
#
# B and C need it even though their fake is in-process, for the reason given above: the
# registration itself opens the driver.  E needs it for the transport.  A and D need it not at
# all and must not be made to look as though they do -- A on a driverless host is exactly the
# fallback-not-abort evidence, so it is the one place the absence is a FEATURE of the run.
invocation_needs_binder() { # $1 = invocation letter
    case "$1" in
        B|C|E) return 0 ;;
        *)     return 1 ;;
    esac
}

# ------------------------------------------------------------------------------------
# Optional suite execution.  The order -- zero once, run the matrix, capture once -- is the
# whole point, and the first of those three words rests on the measured reason recorded at
# clause 4 in the header: gcov counters ACCUMULATE, and because the back-end selection
# resolves once per process, accumulation ACROSS the invocations is the only way both arms of
# the selection branch are ever covered by anything.  Zeroing per invocation would leave the
# capture describing only the last one; never zeroing would let an earlier run's evidence into
# the figures.  Zeroing once, before the first invocation, and capturing once, after the last
# one has passed, is the only arrangement that has neither failure mode.
#
# Verification after each invocation is what proves a suite ran rather than exited early.
# ------------------------------------------------------------------------------------
# A zero exit status from run_L1Tests means "nothing failed", which is NOT the same as
# "something ran".  GoogleTest exits 0 for an empty selection, so a mistyped or overly narrow
# --gtest_filter yields a green line and a coverage figure measured over no execution at
# all.  Three things are therefore required of the results file the run writes.  The file was
# deleted immediately beforehand, so:
#   * it exists            -> this run wrote it, rather than an earlier one;
#   * "tests" is > 0       -> the binary selected at least one case;
#   * it names one of this suite's own fixtures -> what ran was THIS suite, not, say, only
#     a GoogleTest self-test or a case from an unrelated binary that happened to be on PATH.
# The fixture list is deliberately a stable subset rather than every fixture in the suite: it
# names the oldest, largest fixtures, so adding or renaming a test file does not require
# editing this list, while a run that exercised none of them is not this suite.
#
# ONE PATTERN PER TIER, because the two runners have DISJOINT fixture sets.  A single pattern
# covering both would accept an L1 run that produced only L2 fixture names and vice versa,
# which is precisely the "whatever ran was not this suite" condition it exists to catch.  Each
# invocation names which pattern applies, from its tier.
#
# THE L1 PATTERN INCLUDES THE CONTRACT SUITE'S BACK-END-INDEPENDENT FIXTURES, and that is not a
# weakening even though it widens what counts as "this suite".  It is safe because the
# pattern is not the guard against a PARTIAL run: the per-invocation count
# reconciliation below is, and it is far stronger -- it compares the executed count against
# what the invocation's own filter selects and requires selected + excluded to equal the whole
# registered inventory.  A run narrowed to one fixture fails on the count with a specific
# number rather than having to be caught by not matching a name.  Widening the name test
# while a real count test stands behind it costs nothing; widening it INSTEAD of one would
# cost everything.
readonly EXPECTED_SUITE_PATTERN='"(classname|name)"[[:space:]]*:[[:space:]]*"(CECFrameTest|ConnectionTest|BusTest|DriverTest|LibCCECTest|OpCodeTest|MessageDecoderTest|MessageEncoderTest|OperandsTest|DriverAidlCompatibilityTest|DriverAidlPreflightTest|DriverAidlLocalInstanceTest)'

# The L2 tier's THREE INTEGRATION FIXTURES, and deliberately not every fixture the runner
# registers.  The tier also carries DualPathHostLifecycleTest, which proves the harness ends every
# process it started and inherits no descriptor it was not given; it is left OUT of this pattern on
# purpose, because a results file naming only that fixture would be a run that exercised the
# harness and no back-end at all, and this check exists to reject exactly that.  The three named
# here are the selection arm and the two flow arms, so a file matching one of them did run this
# tier's integration cases.  Adding a fourth name would widen the check for no gain; the
# selected-plus-excluded-equals-registered reconciliation below is what notices a fixture that
# went unrun, and it counts every fixture including the lifecycle one.
readonly L2_EXPECTED_SUITE_PATTERN='"(classname|name)"[[:space:]]*:[[:space:]]*"(DualPathSelectionTest|DualPathLegacyFlowTest|DualPathAidlFlowTest)'

# Number of test cases a runner has REGISTERED, read from the binary itself.
#
# --gtest_list_tests enumerates without executing, so this is cheap.  A test line is indented by
# exactly two spaces; a fixture line is not indented, and GoogleTest's trailing "# GetParam() ="
# annotations are stripped before counting.
#
# THE FILTER PARAMETER NEEDS STATING, because taking one at all looks like the mistake it would
# be on its own: a filtered listing agrees with a filtered run and proves nothing.  That
# objection is correct as far as it goes, which is why the caller asks THREE questions rather
# than one, and never just the filtered one:
#
#   registered_test_count <...>            the whole inventory, no filter        -> registered
#   registered_test_count <...> "$select"  what this invocation intends to run   -> selected
#   registered_test_count <...> "$exclude" what it intends NOT to run            -> excluded
#
# `executed == selected` alone would indeed prove nothing, for exactly that reason.
# `selected + excluded == registered`, with all three read independently from the binary, is
# what makes it an assertion: it is only satisfiable if the invocation's own account of what it
# runs and what it skips adds up to everything that exists.
#
# Prints the count, or nothing when the listing could not be obtained - in which case the caller
# treats the inventory as unknown rather than as satisfied.
# $3 IS THE REBUILT LOADER PATH, not an inherited one: this function launches the test binary to
# list its cases, so it is a launch site in its own right and receives exactly the allowlist
# build_trusted_library_path constructed.  Nothing here reads LD_LIBRARY_PATH from the
# environment; the value is passed in so there is one construction and one place to audit it.
registered_test_count() { # $1=directory  $2=binary name  $3=rebuilt loader path  $4=filter (optional)
    local dir="$1" binary="$2" ld_path="$3" filter="${4:-}" listing
    local -a filter_args=()
    [ -z "$filter" ] || filter_args=("--gtest_filter=$filter")

    # THIS LAUNCH'S COUNTERS ARE DIVERTED AND THE MEASURED LAUNCH'S ARE NOT.  The binary below
    # is the instrumented one, so it writes a .gcda on exit having executed no test at all --
    # and it runs AFTER do_run zeroed the counters, three or four times per invocation.
    # GCOV_PREFIX sends those files to a directory this run discards; the reasoning, the
    # measurement and the strip depth are at make_listing_counter_sink.
    #
    # BOTH ARE COMMAND-PREFIX ASSIGNMENTS on the launch itself, alongside the LD_LIBRARY_PATH
    # this launch already carries, so they are in the environment of THIS process and of nothing
    # else: not exported, not set in the caller, and unreachable from the suite launch in
    # run_one_invocation, which must keep writing its counters into the tree because they are
    # the ones the capture reads.
    #
    # THE SINK IS A PRECONDITION RATHER THAN SOMETHING TO CREATE HERE.  This function is always
    # read through a command substitution, so anything it minted would be minted per call and
    # leaked; do_run creates it once before the matrix and this is the guard that says so.
    [ -n "$LISTING_GCOV_DIR" ] || die "internal error: registered_test_count would launch the
       instrumented $binary before make_listing_counter_sink ran, so its counters would land in
       the object tree the capture reads.  do_run creates the sink before the matrix; a caller
       that reaches this function from anywhere else must do the same."

    # scrub_loader_object_variables runs inside this command substitution's subshell, so the
    # removal applies to this launch and to nothing else.  See its own comment for why it is
    # an unset and not an empty command-prefix assignment.
    listing="$(cd "$dir" \
        && scrub_loader_object_variables \
        && LD_LIBRARY_PATH="$ld_path" \
           GCOV_PREFIX="$LISTING_GCOV_DIR" \
           GCOV_PREFIX_STRIP="$LISTING_GCOV_PREFIX_STRIP" \
           "$TIMEOUT_BIN" --foreground 120 \
            "./$binary" --gtest_list_tests "${filter_args[@]}" 2>/dev/null)" || return 0
    [ -n "$listing" ] || return 0
    printf '%s\n' "$listing" \
        | sed -n 's/^  \([A-Za-z_][A-Za-z0-9_]*\).*/\1/p' \
        | grep -c . || true
}

# ------------------------------------------------------------------------------------
# Verify one invocation's results file.
#
# EVERY GUARD A SINGLE UNFILTERED RUN WOULD GET APPLIES HERE TOO: the file must exist (it was
# deleted immediately beforehand), "tests" must be positive, executed = declared - disabled
# must be positive, failures and errors must both be zero even when the exit status was zero,
# and the results must name one of this tier's own fixtures.  None of them is weakened to
# accommodate a filtered run; the parameters below are what let them apply per invocation
# instead of once.
#
# THE INVENTORY COMPARISON IS THE ONE THAT HAS TO BE PER-INVOCATION.  Comparing the executed
# count against the UNFILTERED registered inventory and raising an advisory on any gap is
# exactly right when the run is unfiltered and fires on every filtered invocation by design.
# So it is a per-invocation HARD CHECK against the count the invocation's own filter
# selects, backed by the selected + excluded == registered reconciliation described at
# registered_test_count.  That is stricter than an advisory in two ways worth naming: a
# mismatch FAILS rather than merely noting, and a suite classified into neither the select nor
# the exclude group breaks the reconciliation instead of quietly going unrun.
# ------------------------------------------------------------------------------------
# ------------------------------------------------------------------------------------
# THE NAMES OF THE CASES A RUN REPORTED AS SKIPPED, one per line, as Fixture.Case.
#
# WHY PER CASE RATHER THAN FROM THE HEADER.  Measured on this host's GoogleTest: the JSON
# header carries exactly disabled, errors, failures, name, tests, testsuites, time and
# timestamp -- and NO "skipped" field.  There is therefore no total to read, and a run whose
# every case skipped has a header indistinguishable from a run whose every case passed.  The
# per-case objects are where the evidence is: a skipped case carries "result": "SKIPPED" and a
# "skipped" member, a passing one carries "result": "COMPLETED".
#
# HOW A CASE IS TOLD FROM A SUITE.  Both objects have a "name", so a scan keyed on that alone
# would attribute a suite's name to a case.  Only CASE objects carry "classname" -- measured --
# and its field order puts it AFTER "result", so buffering the name, flagging on the result and
# emitting on the classname yields Fixture.Case for cases and nothing at all for suites.
#
# awk rather than a JSON parser for the reason the rest of this file gives: no dependency beyond
# the POSIX tools already required.  The output is a name list, so a case whose name contained a
# newline would corrupt it -- GoogleTest case names are C++ identifiers plus '/' for
# parameterised suites, so that cannot arise.
# ------------------------------------------------------------------------------------
skipped_case_names() { # $1 = results JSON
    # shellcheck disable=SC2016  # single quotes are the awk program's own; $0 is awk's line
    "$AWK_BIN" '
        /^[[:space:]]*"name"[[:space:]]*:/ {
            n = $0
            sub(/^[^:]*:[[:space:]]*"/, "", n)
            sub(/".*$/, "", n)
            nm = n
            sk = 0
            next
        }
        /^[[:space:]]*"result"[[:space:]]*:[[:space:]]*"SKIPPED"/ { sk = 1; next }
        /^[[:space:]]*"classname"[[:space:]]*:/ {
            c = $0
            sub(/^[^:]*:[[:space:]]*"/, "", c)
            sub(/".*$/, "", c)
            if (sk && nm != "") { print c "." nm }
            next
        }
    ' "$1"
}

# Does $1 match any of the ':'-separated gtest globs in $2?  Shell pattern matching is what
# gtest's own filter uses -- '*' and '?' over the Fixture.Case name -- so `case` is the right
# tool and no pattern translation is needed.  An empty pattern list matches nothing, which is
# the "no skips permitted" case and must not degenerate into "everything permitted".
name_matches_any_pattern() { # $1 = Fixture.Case  $2 = ':'-separated globs
    local name="$1" patterns="$2" pat rest
    [ -n "$patterns" ] || return 1
    rest="$patterns"
    while [ -n "$rest" ]; do
        pat="${rest%%:*}"
        if [ "$pat" = "$rest" ]; then rest=''; else rest="${rest#*:}"; fi
        [ -n "$pat" ] || continue
        # shellcheck disable=SC2254  # the glob is the pattern; quoting it would defeat the match
        case "$name" in $pat) return 0 ;; esac
    done
    return 1
}

verify_results() { # $1=results JSON  $2=label  $3=fixture pattern  $4=expected executed  $5=registered  $6=excluded  $7=permitted-skip globs  $8=mandatory count  $9=log to append the accounting marker to
    # THE LD_LIBRARY_PATH PARAMETER IS GONE.  It was accepted, assigned and never read: this
    # function reads a results file and a listing that the CALLER has already produced, and it
    # runs nothing that needs a loader path.  A parameter that is only positional padding is a
    # standing invitation to pass the wrong argument in the right slot, which is exactly the
    # class of mistake the reconciliation below exists to catch -- so it is removed rather than
    # documented.
    local results="$1" label="$2" fixture_pattern="$3"
    local expected="$4" registered="$5" excluded="$6"
    local permit_skip="${7:-}" mandatory="${8:-}" accounting_log="${9:-/dev/null}" count

    [ -f "$results" ] || die "invocation $label exited 0 but wrote no results file at
       $results
       The file was deleted immediately before the run, so its absence means the binary
       produced no results at all.  A coverage figure cannot be attributed to a run that
       left no evidence it executed anything."

    # THE DECLARED COUNT AND THE EXECUTED COUNT ARE NOT THE SAME NUMBER.
    #
    # The GoogleTest JSON header carries the run's totals, and its "tests" field counts every case
    # in the selection INCLUDING the DISABLED_ ones, which are listed as entries and never
    # executed.  Both numbers are needed here and they are used for different things:
    #   * "declared" is what is compared against --gtest_list_tests below, because that listing
    #     also enumerates DISABLED_ cases -- comparing an executed count against an inclusive
    #     listing would report a phantom gap for every disabled case in the suite;
    #   * "executed" is what is REPORTED, because it is the number a reader quotes as evidence,
    #     and a suite with disabled cases does not execute as many as it declares.
    # sed rather than a JSON parser, because this script adds no dependency beyond the POSIX tools
    # it already needs; each field is taken from its first occurrence, which is the top-level
    # header preceding the "testsuites" array.
    local declared disabled failures errors
    declared="$(sed -n 's/^[[:space:]]*"tests"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$results" | head -1)"
    disabled="$(sed -n 's/^[[:space:]]*"disabled"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$results" | head -1)"
    failures="$(sed -n 's/^[[:space:]]*"failures"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$results" | head -1)"
    errors="$(sed -n 's/^[[:space:]]*"errors"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$results" | head -1)"
    : "${disabled:=0}" "${failures:=0}" "${errors:=0}"
    count="$declared"
    if [ -z "$count" ] || [ "$count" -le 0 ]; then
        die "invocation $label exited 0 but $results reports no tests (\"tests\": ${count:-absent}).
       An empty run cannot substantiate a coverage figure: every line would be reported
       unhit and the gate would fail for the wrong reason, or -- worse, with a partial
       filter -- a subset's figure would be reported as the suite's.
       This invocation's own --gtest_filter selected $expected case(s), so a zero here means
       either that filter matched nothing in this binary or something overrode it."
    fi

    local executed=$((declared - disabled))
    if [ "$executed" -le 0 ]; then
        die "invocation $label exited 0 and $results declares $declared case(s), but $disabled of them are
       DISABLED_ and so none actually executed.  A coverage figure cannot be attributed to a run in
       which nothing ran."
    fi
    # The results file is this run's evidence and a zero exit status is a different claim, so a
    # suite that recorded a failure or an error while still exiting 0 is treated as a failing suite.
    if [ "$failures" -ne 0 ] || [ "$errors" -ne 0 ]; then
        die "invocation $label exited 0 but $results records $failures failure(s) and $errors error(s).
       The results file is the evidence and it contradicts the exit status, so no coverage is
       reported for this run."
    fi

    grep -Eq "$fixture_pattern" "$results" || die "invocation $label exited 0 and $results
       reports $count test case(s), but not one of them belongs to a known fixture of this
       tier.  Whatever ran was not this suite, while the capture would credit this
       submodule's objects.  Check that the runner in that directory is the binary built
       from this tree and that GTEST_EXTRA_ARGS is not restricting the run to unrelated
       cases."

    # ------------------------------------------------------------------------------------
    # CHECK 2 OF THE FOUR PER-INVOCATION CHECKS: the executed count is the count this
    # invocation's own filter selects.
    #
    # Everything above establishes that SOMETHING from this tier ran; none of it establishes
    # that ALL OF WHAT THIS INVOCATION WAS SUPPOSED TO RUN ran.
    # `GTEST_EXTRA_ARGS=--gtest_filter=-SomeFailingSuite.*` leaves a positive count, keeps
    # recognisable fixture names in the JSON, and exits 0 -- so the suite reads as green while
    # the cases that would have failed were never executed, and the coverage captured
    # afterwards is credited to a run that skipped them.
    #
    # The expected number is READ FROM THE BINARY with this invocation's declared filter, never
    # written down here, so adding a test case requires no edit to this script.  A caller who
    # overrides the filter through GTEST_EXTRA_ARGS therefore fails HERE, loudly, with both
    # numbers named -- which is the intended outcome: GTEST_EXTRA_ARGS may add diagnostic flags,
    # but re-filtering an invocation makes it a different invocation and this script will not
    # report one as the other.
    # ------------------------------------------------------------------------------------
    if [ -z "$registered" ] || [ "$registered" -le 0 ] || [ -z "$expected" ] || [ "$expected" -le 0 ]; then
        die "invocation $label ran, but its expected case count could not be read from the binary
       (--gtest_list_tests produced nothing usable: registered='${registered:-}',
       selected='${expected:-}').  Without it there is no way to tell a complete invocation
       from a partial one, and a coverage figure credited to an unverifiable execution is
       exactly what this script exists not to produce."
    fi

    # Both sides include DISABLED_ cases -- the listing enumerates them and the JSON's "tests"
    # counts them -- so this compares SELECTION against SELECTION and a disabled case cannot by
    # itself open a gap.
    if [ "$count" -ne "$expected" ]; then
        # The override is named ONLY when it is actually set.  An unconditional "filter :" line
        # printed an empty field on every other cause of a mismatch, which reads as though the
        # filter were the culprit when it is not -- and the overwhelmingly likeliest cause of
        # this failure is a GTEST_EXTRA_ARGS filter layered on top of the matrix's own, so when
        # it IS set that fact belongs in the message rather than in the reader's head.
        local override_note=''
        if [ -n "${GTEST_EXTRA_ARGS:-}" ]; then
            override_note="
           GTEST_EXTRA_ARGS      : set to $(render_untrusted "$GTEST_EXTRA_ARGS"), which narrows the matrix's filter"
        fi
        die "invocation $label executed a different set from the one it declared.
           declared by the filter : $expected case(s)
           actually selected      : $count case(s)$override_note
       A run that exits 0 because the cases that would have failed were filtered out is not a
       green run, and coverage captured from it would be credited to an execution that did not
       happen.  Both numbers above came from this same binary moments apart, so the difference
       is real."
    fi

    # ------------------------------------------------------------------------------------
    # THE RECONCILIATION.  selected + excluded == registered, all three measured.
    #
    # This is the matrix's form of an unfiltered comparison rather than a replacement for one.
    # Noticing cases that were never selected is the purpose; under a matrix non-selection is by
    # design, so the question is not "were any cases skipped" but "are the skipped cases exactly
    # the ones this invocation SAID it skips".  A suite added to the binary and classified into
    # neither group fails here -- which an unfiltered comparison could not catch either, since an
    # unclassified suite would simply widen the gap such a check tolerates as an advisory.
    # ------------------------------------------------------------------------------------
    local accounted=$((expected + excluded))
    if [ "$accounted" -ne "$registered" ]; then
        die "invocation $label does not account for the whole registered inventory.
           selected by its filter : $expected
           excluded by its filter : $excluded
           sum                    : $accounted
           registered in binary   : $registered
       $(( registered - accounted )) case(s) are in neither group, so they run under no
       invocation and nothing would ever report them.  This almost always means a new test
       suite was added without being classified in INVOCATION_MATRIX: put it in the select
       filter of the invocations that can run it and in the exclude filter of the rest.
       Do NOT widen a select filter with a wildcard to make this arithmetic close -- that
       would run the new suite under an invocation whose back-end it may not support, which
       is the failure this classification exists to prevent."
    fi

    if [ "$disabled" -gt 0 ]; then
        log "invocation $label EXECUTED $executed case(s) per $results (of $declared declared;
       $disabled DISABLED_ and therefore not run) -- matching the $expected its filter selects,
       with $excluded excluded and $registered registered"
    else
        log "invocation $label EXECUTED all $executed declared case(s) -- matching the $expected its"
        log "  filter selects, with $excluded excluded and $registered registered in the binary"
    fi

    # ------------------------------------------------------------------------------------
    # THE MANDATORY-PASS CONTRACT.  Everything above counts cases; this is the only part that
    # asks whether they PASSED.
    #
    # "executed" up to here includes cases GoogleTest skipped, because its "tests" total counts
    # them -- so an invocation whose mandatory cases all skipped in SetUp reconciles perfectly
    # against its own filter and exits 0.  That is the false green this check closes: every skip
    # is matched by NAME against the invocation's permitted set, an unpermitted skip is fatal
    # with its name printed, and the mandatory count is measured from the binary rather than
    # written down.
    # ------------------------------------------------------------------------------------
    local skipped_names skipped=0 unpermitted='' one
    skipped_names="$(skipped_case_names "$results")"
    if [ -n "$skipped_names" ]; then
        while IFS= read -r one; do
            [ -n "$one" ] || continue
            skipped=$((skipped + 1))
            name_matches_any_pattern "$one" "$permit_skip" \
                || unpermitted="${unpermitted}${unpermitted:+
}           $one"
        done <<< "$skipped_names"
    fi

    if [ -n "$unpermitted" ]; then
        die "invocation $label SKIPPED case(s) it is required to PASS:
$unpermitted
       This invocation permits skips only in: ${permit_skip:-<none: every case is mandatory>}
       A skipped case is reported among GoogleTest's \"tests\" total, so it reconciles against
       the filter and leaves the exit status at 0 -- which is why this is checked by name and
       not by arithmetic.  Nothing above would have noticed it.
       WHAT A SKIP HERE USUALLY MEANS.  These fixtures skip in SetUp when the resolved back-end
       is not the one they exercise, so a mandatory case skipping says the invocation resolved
       the WRONG BACK-END while still logging a plausible selection -- or that the harness could
       not reach the state the case needs.  On invocation E specifically, the AIDL-flow cases are
       the only evidence this suite ever produces that a frame crosses real binder IPC and
       arrives on a binder thread: a green E with those skipped is a run that proved nothing it
       exists to prove.  Read $results and the run log before treating this as a test defect."
    fi

    local passed=$((executed - skipped))
    # DEFAULTED TO "EVERYTHING IS MANDATORY", which is the fail-closed direction: a caller that
    # supplies no floor gets the strictest one rather than none.  Validated as a number because
    # an arithmetic comparison against a stray word would abort with a shell error instead of a
    # verdict, and this function's whole job is to produce a verdict.
    : "${mandatory:=$executed}"
    case "$mandatory" in
        ''|*[!0-9]*) die "internal error: invocation $label was given a non-numeric mandatory
       case count [$mandatory].  A contract that cannot be compared is not a contract." ;;
    esac

    # The permitted skips are PERMITTED, not required: a host on which they can also run leaves
    # more passes than the floor, which is a better result and not a discrepancy.  Fewer is
    # impossible without an unpermitted skip or a failure, both fatal above -- so this is a
    # cross-check on the three numbers agreeing rather than a second policy.
    if [ "$passed" -lt "$mandatory" ]; then
        die "invocation $label passed $passed case(s) but $mandatory are mandatory.
           executed : $executed
           skipped  : $skipped (each one permitted by name)
           passed   : $passed
           mandatory: $mandatory  (measured from the binary as its selection less ${permit_skip:-nothing})
       An unpermitted skip is fatal above and a recorded failure is fatal earlier still, so a
       shortfall here means these numbers disagree with each other.  Report it."
    fi

    log "  MANDATORY-PASS CONTRACT MET: $passed passed, $skipped skipped, $failures failed"
    if [ -n "$permit_skip" ]; then
        log "    $mandatory of the $executed selected case(s) were mandatory, and all $mandatory passed"
        log "    the $skipped skip(s) are all inside [$permit_skip], which this invocation permits"
    else
        log "    every one of the $executed selected case(s) was mandatory, and none was skipped"
    fi

    # MACHINE-READABLE, so the numbers are auditable rather than only printed.  It goes into the
    # per-invocation run log rather than a new artifact: the artifact name set is fixed and
    # enumerated in one place, and a reader or a CI step can grep one marker out of the log it
    # already collects.  Every field is a bare integer or a glob, on one line, in a fixed order.
    printf '###ACCOUNTING invocation=%s declared=%s disabled=%s executed=%s passed=%s failed=%s errors=%s skipped=%s mandatory=%s permitted_skips=%s\n' \
        "$label" "$declared" "$disabled" "$executed" "$passed" "$failures" "$errors" \
        "$skipped" "${mandatory:-0}" "${permit_skip:-none}" >>"$accounting_log"
}

# ------------------------------------------------------------------------------------
# CHECK 3 OF FOUR: the selected-path line, in THIS invocation's own log.
#
# WHY THIS CHECK IS NOT OPTIONAL AND CANNOT BE INFERRED.  An invocation that was supposed to
# resolve the AIDL back-end and quietly resolved the legacy one instead is GREEN in every
# other respect: the back-end-neutral cases pass either way, the arm-specific fixtures skip
# rather than fail by design, the count reconciles, the exit status is zero.  Nothing in the
# results file records WHICH back-end ran.  So a mis-wired invocation E -- a host launched too
# late, a readiness token never written, a service name already taken -- would report a clean
# AIDL result having tested the legacy path from start to finish.  This grep is the only thing
# standing between that and an acceptance verdict.
#
# THE STRING IS NOT INVENTED HERE.  It is emitted once per process at LOG_INFO by the selection
# helper in ccec/src/Driver.cpp, from a single format constant with the back-end name as its one
# substitution, and both test files transcribe it verbatim for the same reason this does.  It
# reaches the log through CCEC_LOG, which prefixes a timestamp and the CEC log prefix, so the
# match is on the TRAILING SUBSTRING rather than on a whole line.
#
# EXACTLY ONE HIT IS REQUIRED, not at least one.  The two "service is not usable" lines that
# precede it on a legacy fallback are deliberately worded so that they do not match, which is
# what makes "one hit" meaningful: more than one would mean the selection resolved twice, and
# the whole design rests on it resolving once per process.
# ------------------------------------------------------------------------------------
readonly SELECTED_BACK_END_LOG_PREFIX='Driver::getInstance : HDMI CEC HAL back-end selected : '

assert_selected_back_end() { # $1=log file  $2=expected back-end name  $3=invocation label
    local log_file="$1" expected_back_end="$2" label="$3" hits other

    [ -f "$log_file" ] || die "invocation $label left no log at $log_file, so the back-end it
       selected cannot be established."

    hits="$(grep -c -F -- "${SELECTED_BACK_END_LOG_PREFIX}${expected_back_end}" "$log_file" || true)"
    if [ "${hits:-0}" -eq 1 ]; then
        log "invocation $label resolved the $expected_back_end back-end (selected-path line found, once)"
        return 0
    fi

    # Name what WAS selected when something was, because "expected AIDL" is a much less useful
    # diagnostic than "expected AIDL, got legacy".
    other="$(grep -o -- "${SELECTED_BACK_END_LOG_PREFIX}[A-Za-z]*" "$log_file" 2>/dev/null | sort -u | sed 's/^/         /' || true)"
    if [ "${hits:-0}" -eq 0 ]; then
        die "invocation $label did not resolve the $expected_back_end back-end.
       The selected-path line naming it is absent from
           $log_file
       Selected-path line(s) actually present:
${other:-         (none at all)}
       This invocation is therefore NOT evidence about the back-end it was configured for, and
       it is treated as a failure rather than as a pass, because every other signal it produces
       -- exit status, case count, fixture names -- is identical whichever back-end resolved.
       For invocation E the usual causes are a fake service host that never became ready, one
       launched after LibCCEC::init rather than before it, or a stale registration already
       holding the production service name.  For B and C, a harness that could not register its
       in-process fake."
    fi
    die "invocation $label emitted the $expected_back_end selected-path line $hits times, and it
       must appear EXACTLY ONCE: the selection resolves once per process and is held for the
       process lifetime, so a second line means it resolved twice.  Log: $log_file"
}

do_run() {
    # A run has to be BOUNDED, so `timeout` is a hard requirement of this path rather than an
    # optional nicety: the L1 suite drives real pthread mutexes, condition variables and threads,
    # and without a bound a deadlocked case hangs here for ever with no output, no exit status and
    # no gate.  Checked here, before a single counter is zeroed, so the tree is left untouched.
    [ -n "$TIMEOUT_BIN" ] || die "timeout was not found on PATH, and the suite will not be run
       without it, because the run would then be unbounded.  timeout ships with coreutils:
           sudo apt-get install -y coreutils
       Then re-run, or invoke this script without --run against counters produced elsewhere."

    # THE LISTING SINK IS MINTED BEFORE THE FIRST COUNTER IS ZEROED, for the same reason the
    # timeout check above is made here: a failure to create it must leave the tree exactly as it
    # was found rather than half prepared.  It is also the only order in which the guard inside
    # registered_test_count can be an invariant instead of a race -- every listing launch in the
    # matrix happens after this line.
    make_listing_counter_sink

    rule
    log "zeroing gcov counters under $HDMICEC_ROOT (leaves *.gcno instrumentation intact)"
    lcov_run --zerocounters -d "$HDMICEC_ROOT" \
        "${LCOV_CONFIG_ARGS[@]}" "${LCOV_RC_ARGS[@]}" \
        --ignore-errors "$LCOV_SUMMARY_IGNORE" >/dev/null 2>&1 \
        || die "lcov --zerocounters failed for $HDMICEC_ROOT.
       Refusing to run the suite, because the capture would then mix this run's counters
       with whatever was left behind by earlier ones."
    # A zero exit from `lcov --zerocounters` is not proof that the counters are gone: it walks
    # the tree it was given and reports success for the files it managed to clear, so a .gcda
    # in a directory it did not descend into -- or one it could not remove -- survives with no
    # error.  That survivor still holds an EARLIER run's execution counts, gcov ACCUMULATES
    # into it, and the capture below would then credit this run with work it never did; the
    # gate could pass on evidence the current tests did not produce.  A warning here would be
    # exactly the shape of "reported but not prevented", so this is FATAL, matching the sink
    # runner: refuse to measure rather than measure the wrong thing.
    local after_zero
    after_zero="$(gcda_count)"
    # FATAL, not a warning.  lcov --zerocounters exiting 0 means "the command ran", not
    # "every counter is gone": it silently leaves behind any .gcda it could not unlink -- a
    # read-only directory, a file owned by another user, a stray copy outside the object
    # tree it walked.  Those counters are then captured alongside this run's and credited to
    # it, so a line last executed by a test that has since been deleted or filtered out
    # still reports as hit.  That is precisely the stale-evidence acceptance this whole
    # script exists to prevent, and warning about it while carrying on and printing
    # "COVERAGE GATE PASSED" made the warning worthless.  Nothing downstream can repair it,
    # so the run stops here.
    if [ "$after_zero" -ne 0 ]; then
        local survivors
        survivors="$( { "$FIND_BIN" "$HDMICEC_ROOT" -name '*.gcda' -type f 2>/dev/null || true; } \
                      | head -n 10 | sed 's/^/         /')"
        die "$after_zero .gcda counter file(s) SURVIVED lcov --zerocounters under
       $HDMICEC_ROOT
       gcov counters accumulate, so capturing now would mix this run's execution with
       whatever produced those files and credit all of it to this run.  Refusing to
       measure, because the resulting figure could not be attributed to anything.
       First survivors:
$survivors
       Remove them and retry:
           find '$HDMICEC_ROOT' -name '*.gcda' -type f -delete
       If they cannot be removed, the build tree is not writable by this user and a
       fresh --build into a tree you own is the fix."
    fi

    log "counters zeroed ONCE for the whole matrix; the invocations accumulate into them"
    run_invocation_matrix

    local fresh
    fresh="$(gcda_count)"
    [ "$fresh" -gt 0 ] || die "every invocation passed but no *.gcda counters exist under
       $HDMICEC_ROOT.  Either the binaries are not the instrumented ones or the counters went
       somewhere unexpected.  Refusing to report a coverage figure that was not measured."
    log "the matrix produced $fresh .gcda counter file(s), accumulated across every invocation"
}

# ------------------------------------------------------------------------------------
# Run ONE invocation of the matrix and hold it to all four checks.
#
# ARTIFACTS ARE PER-INVOCATION AND A LATER ONE CANNOT OVERWRITE AN EARLIER ONE.  A single
# hard-coded run_L1Tests.log and a single rdkL1TestResults.json would mean, under a matrix, the
# last invocation's evidence standing in for all five: an empty or failed
# run erased by whichever green one came after it, and an artifact bundle that says
# nothing about how the result was reached.  The invocation LETTER goes in the name -- and a
# letter, not a timestamp, because clause 5 of the governing contract requires fixed artifact
# names with no timestamp in any of them, so that the same tree produces the same file set and
# a reader knows what to look for without listing the directory.
#
# ORDERING IS LOAD-BEARING AND IT IS THE HARNESS THAT OWNS IT.  On B and C the in-process fake
# must be registered, and on E the host must be launched and signalled ready, BEFORE
# LibCCEC::init -- because init is what forces the selection, and a service that appears after
# it leaves the already-resolved choice on legacy and produces a green run that proves nothing.
# Both harnesses do that themselves, in their global environment SetUp, before the init call.
# What this script contributes is the mode and the host's path, handed over as environment
# before the process starts, and then the assertion that the intended back-end actually
# resolved -- which is the only check that can catch a mis-ordering after the fact.
# ------------------------------------------------------------------------------------
run_one_invocation() { # $1=letter $2=tier $3=mode $4=back-end $5=select $6=exclude $7=synopsis $8=permitted skips
    local label="$1" tier="$2" mode="$3" back_end="$4"
    local select_filter="$5" exclude_filter="$6" synopsis="$7" permit_skip="${8:-}"
    local binary_rel binary_name binary_dir fixture_pattern

    case "$tier" in
        L1) binary_rel="$TEST_BINARY_REL";    fixture_pattern="$EXPECTED_SUITE_PATTERN" ;;
        L2) binary_rel="$L2_TEST_BINARY_REL"; fixture_pattern="$L2_EXPECTED_SUITE_PATTERN" ;;
        *)  die "internal error: invocation $label names an unknown tier '$tier'" ;;
    esac
    binary_name="$(basename -- "$binary_rel")"
    binary_dir="$HDMICEC_ROOT/$(dirname -- "$binary_rel")"

    [ -x "$HDMICEC_ROOT/$binary_rel" ] || die "invocation $label needs $binary_rel and it is
       not an executable in this tree:
           $HDMICEC_ROOT/$binary_rel
       Build it with:  $SCRIPT_PATH --build
       The L2 tier is configured by tests/L2Tests/Makefile in configure.ac's AC_CONFIG_FILES
       and recursed into from tests/Makefile.am's SUBDIRS, both under --enable-l1tests."

    local run_log="$OUTPUT_DIR/run_invocation_${label}.log"
    local results_json="$OUTPUT_DIR/rdkTestResults_invocation_${label}.json"
    local ld_path=''

    # Custody first, because the next two statements REMOVE a file and then have the suite
    # write two more.  A removal is the most destructive thing this function does, and the
    # directory it removes from is re-proved to be the one this run resolved and locked before
    # the unlink rather than after it.  Both artifact paths are checked, since either could
    # have acquired a symlink since the run started.
    assert_output_dir_still_safe "$results_json"
    assert_output_dir_still_safe "$run_log"

    # Deleted BEFORE the binary starts, so a results file that exists afterwards can only
    # have been written by this invocation.  Without this, a binary that produced nothing would
    # be judged against whatever an earlier invocation left at the same path -- and with
    # per-invocation names, against its own earlier attempt.
    rm -f -- "$results_json"

    # THE LOADER PATH IS REBUILT, NEVER INHERITED.  Every directory the launched runner searches
    # for a shared object comes from build_trusted_library_path -- this script's own resolved
    # GTest and Binder/HALIF roots, each one validated -- and no part of it comes from the
    # LD_LIBRARY_PATH this script was invoked with.  The reasoning is at that function; the short
    # form is that an inherited loader path is untrusted input to a process that loads shared
    # objects, these binaries have no RUNPATH, and the first directory that answers a soname is
    # the one that gets loaded.  Both launch sites below -- the --gtest_list_tests listings in
    # registered_test_count and the suite launch itself -- receive this one value and nothing
    # else, so there is no second path by which the environment can reach the loader.
    build_trusted_library_path
    ld_path="$TRUSTED_LIBRARY_PATH"

    # THE EXPECTED COUNTS ARE READ FROM THE BINARY, NOW, with this invocation's own filters.
    # Three separate listings, so the reconciliation in verify_results compares independently
    # obtained numbers rather than two views of one subtraction.
    local registered selected excluded
    registered="$(registered_test_count "$binary_dir" "$binary_name" "$ld_path" '')"
    selected="$(registered_test_count "$binary_dir" "$binary_name" "$ld_path" "$select_filter")"
    if [ -n "$exclude_filter" ]; then
        excluded="$(registered_test_count "$binary_dir" "$binary_name" "$ld_path" "$exclude_filter")"
    else
        # No exclude filter means the invocation claims to run everything, and the
        # reconciliation then reduces to selected == registered, which is exactly right.
        excluded=0
    fi
    : "${registered:=0}" "${selected:=0}" "${excluded:=0}"

    # THE MANDATORY COUNT IS MEASURED, NEVER WRITTEN DOWN.  It is this invocation's selection
    # with its permitted-skip set subtracted, obtained from the binary by a fourth listing so
    # that adding a case to a fixture changes it automatically.  A hard-coded 10 and 8 -- the
    # values D and E measure here today -- would have to be edited by whoever next adds an L2
    # case, and the one thing worse than no contract is a contract that silently stops
    # matching the suite.
    #
    # THE COMPOSITION IS GUARDED.  gtest filter syntax is `positive-negative` with at most ONE
    # '-', so appending a negative clause to a select filter that already carries one would
    # produce a filter that means something else entirely -- and it would still parse, still
    # exit 0, and still yield a plausible number.  Every invocation that permits skips has a
    # purely positive select filter today; if that ever changes this refuses rather than guesses.
    local mandatory="$selected"
    if [ -n "$permit_skip" ]; then
        case "$select_filter" in
            *-*) die "internal error: invocation $label permits skips in [$permit_skip] but its
       select filter already carries a negative clause:
           $select_filter
       gtest filters allow one '-' only, so the mandatory set cannot be derived by appending
       another.  Express this invocation's mandatory set as its own positive filter instead." ;;
        esac
        mandatory="$(registered_test_count "$binary_dir" "$binary_name" "$ld_path" \
                        "${select_filter}-${permit_skip}")"
        : "${mandatory:=0}"
        [ "$mandatory" -gt 0 ] || die "invocation $label derives a mandatory set of $mandatory case(s)
       from filter [${select_filter}-${permit_skip}].
       An invocation with nothing mandatory can pass while skipping everything it exists to
       prove, which is precisely the shape this contract exists to reject.  Either the permitted
       set in INVOCATION_MATRIX now covers the whole selection, or the fixtures it names have
       been renamed."
    fi

    rule
    log "INVOCATION $label -- $synopsis"
    log "  runner            : $binary_rel"
    log "  CEC_TEST_AIDL_MODE: $mode"
    log "  expected back-end : $back_end"
    log "  filter            : ${select_filter:-<none: the whole registered inventory>}"
    log "  expected cases    : $selected selected, $excluded excluded, $registered registered"
    log "  mandatory passes  : $mandatory of $selected; skips permitted only in ${permit_skip:-<nothing>}"
    log "  results JSON      : $results_json"
    log "  log               : $run_log"
    log "  time limit        : ${SUITE_TIMEOUT}s (SUITE_TIMEOUT)"
    if [ -n "$GTEST_EXTRA_ARGS" ]; then
        log "  extra gtest args  : $(render_untrusted "$GTEST_EXTRA_ARGS")"
        # ANY caller-supplied gtest argument makes this run diagnostic, not acceptance.
        #
        # A filter is the obvious case - `--gtest_filter=-KnownFailingSuite.*` leaves a
        # recognisable, positive, exit-0 run with the failures excised - but it is not the only
        # one: --gtest_shuffle changes an order this suite is known to be fragile under,
        # --gtest_repeat changes what the counters accumulate, and --gtest_also_run_disabled_tests
        # changes which cases the figures came from.  Rather than enumerate a permitted subset
        # and be wrong about the next one, every custom argument is recorded, so the verdict is
        # ADVISORY and the exit status is $EXIT_ADVISORY.
        #
        # A FILTER PASSED THIS WAY FAILS THE INVOCATION rather than being reported as a partial
        # one, which is stricter than the advisory above and deliberately so.  GTEST_EXTRA_ARGS
        # is appended after this invocation's own --gtest_filter, and GoogleTest lets the last
        # one win, so an override silently turns invocation B into some other selection
        # entirely.  The expected
        # count in verify_results comes from the invocation's DECLARED filter, so the two
        # disagree and it dies with both numbers named.  Diagnostic flags remain useful here;
        # re-filtering does not, because a re-filtered invocation is a different invocation and
        # this script will not report one as the other.
        note_advisory "GTEST_EXTRA_ARGS was set to $(render_untrusted "$GTEST_EXTRA_ARGS"), so the suite was NOT run
       the way an acceptance run runs it.  Custom arguments can change which cases execute, in
       what order and how often, so this run's figures are diagnostic."
    fi

    # --gtest_print_time=1 and the JSON output mirror the workflow's own run step.
    # GTEST_EXTRA_ARGS is intentionally word-split: it is a user-supplied argument list.
    #
    # BOUNDED, because an unbounded run is not a run that can fail.  A test that deadlocks --
    # this suite drives real pthread mutexes, condition variables and threads, and a mock that
    # never signals is one edit away -- would otherwise hang here for ever: no output, no exit,
    # no gate, and in CI a job killed by the runner's own limit with no diagnosis attached.
    # `timeout --foreground` is kept, but NOT for the reason it usually is: the suite does not
    # run in the terminal's foreground group at all (see the cancellation block next to
    # `trap on_exit`).  It is kept because --foreground makes `timeout` refrain from putting its
    # child in a process group of its OWN, which is what keeps `timeout` and the test binary
    # inside the one group the signal handler signals.  A Ctrl-C reaches the suite through that
    # handler rather than through the terminal.  --kill-after is added where the local timeout
    # preserves exit 124 with it (see the probe above), to escalate to SIGKILL if the suite
    # ignores SIGTERM.  Exit 124 is timeout's own signal that the limit fired, and it is reported
    # as its own outcome below rather than folded into "the suite failed", because the two need
    # different fixes.
    #
    # CANCELLABLE: the suite is launched in the BACKGROUND and in its OWN PROCESS GROUP, and this
    # shell waits on it, so an external INT/TERM/HUP reaches the handler WHILE the suite runs
    # rather than after it (see the block next to `trap on_exit`).  `timeout --foreground` is kept
    # for exactly that reason: it deliberately does not put its child in a new process group, so
    # `timeout` and the test binary both stay in the one group the handler signals.
    # THE HARNESS ENVIRONMENT, and the one variable this script deliberately does NOT set.
    #
    # LD_LIBRARY_PATH IS SET FROM THE REBUILT ALLOWLIST AND NEVER PASSED THROUGH.  The value
    # below is build_trusted_library_path's output -- this script's own validated GTest and
    # Binder/HALIF roots -- and it REPLACES whatever loader path this script inherited, which is
    # discarded and logged rather than forwarded.  Setting it explicitly here is what makes that
    # true of the child: an environment assignment on the command that execs the runner overrides
    # the inherited variable, so the child sees the allowlist and nothing else.
    #
    # CEC_TEST_AIDL_MODE is what distinguishes one invocation from the next.  It is read by
    # tests/L1Tests/test_main.cpp and tests/L2Tests/test_main.cpp and by nothing else -- no
    # production source reads it, and none may.  Each harness hard-fails on a value it does not
    # implement rather than quietly downgrading to `absent`, which is why passing `remote` to
    # the L1 runner or `compatible` to the L2 runner is a loud error rather than a silent
    # legacy run.  The matrix never does either.
    #
    # CEC_FAKE_AIDL_HOST_PATH tells the L2 harness where the out-of-process fake service host
    # is.  It is set for BOTH L2 invocations even though only E launches anything: D reads it
    # not at all, so setting it costs nothing and keeps one code path.
    #
    # CEC_FAKE_HOST_READY_FD IS NOT SET HERE, AND MUST NOT BE.  It names the number of an
    # INHERITED DESCRIPTOR -- the write end of a pipe whose read end the waiting parent holds --
    # and the parent is the L2 harness, not this script.  The harness creates the pipe, forks,
    # sets that variable in the CHILD's environment, and blocks on the read end under a bounded
    # timeout.  A value exported from here would name a descriptor this script never opened, and
    # the host treats an unusable descriptor as a hard failure precisely so that a botched
    # handoff is reported rather than silently answered on standard output.  Setting it would
    # therefore break the very handoff it looks like it is helping with.
    #
    # THE READINESS WAIT AND THE REAPING ARE THE HARNESS'S TOO, and both are bounded there: it
    # matches the fixed token verbatim, fails the run rather than proceeding when the token does
    # not arrive, and terminates and reaps the host in its teardown.  This script's contribution
    # to that contract is SUITE_TIMEOUT as an outer bound and the selected-path assertion as the
    # after-the-fact check -- a host that never became ready leaves the harness failing, and a
    # host that became ready too late leaves a legacy selection, which the assertion catches.
    #
    # NOTHING HERE STARTS A BINDER THREADPOOL.  The client-side pool is DriverAidlImpl::open()'s
    # to start, during init; the host starts its own service-side pool.  A third one from the
    # harness or from here would duplicate an ownership the production back-end already holds.
    # ------------------------------------------------------------------------------------
    # THE LAUNCH IS UNINTERRUPTIBLE UNTIL THE GROUP IS REGISTERED, and the child undoes the
    # mask before it execs.  Both halves are necessary and each is useless without the other.
    #
    # THE WINDOW.  `( … ) &` and `SUITE_PGID=$!` are two commands, and bash runs a pending trap
    # BETWEEN commands.  A SIGINT arriving in that gap therefore reached on_signal with
    # SUITE_PGID still empty, so stop_suite_group returned immediately and the suite -- and, on
    # invocation E, the fake service host forked from inside its group and leading a group of its
    # own -- was left running while this script exited.  The orphan then held the global "HdmiCec" binder service name, which is
    # not a name a second run can work around: it is fixed in production code by
    # IHdmiCec::serviceName(), so one leak makes every later AIDL invocation on the host resolve
    # against a stale process.  Masking INT/TERM/HUP across the launch and the registration
    # closes the window; the signal is discarded rather than deferred, which across two commands
    # is the smaller loss.
    #
    # WHY THE CHILD RESETS THEM.  Measured on this host: a subshell forked while the parent has
    # `trap '' INT TERM HUP` in force inherits those signals as SIG_IGN (/proc/self/status
    # SigIgn=0x4003 = HUP|INT|TERM), and an exec'd binary keeps that mask -- so `timeout` and the
    # test binary would IGNORE the SIGTERM stop_suite_group sends, and the group could then only
    # be removed with SIGKILL.  A `trap - INT TERM HUP` as the child's first statement restores
    # the default disposition (measured SigIgn=0x0, and the child then dies on TERM); bash's rule
    # that signals ignored ON SHELL ENTRY cannot be reset does not apply to a signal the script
    # itself set to ignore.  So the mask protects the registration and nothing else.
    # THE LAST THING BEFORE THE FIRST MEASURED LAUNCH: prove the counters are still zero.
    # Everything between do_run's zeroing and this point is this script's own preparation --
    # three or four --gtest_list_tests launches of the instrumented binary among it -- and the
    # counters it leaves behind would be indistinguishable from a test's.  Once only, because
    # from the second invocation onwards the accumulated counters of the earlier ones are
    # exactly what the design requires to be there.
    assert_no_counters_before_first_case "$label"

    local rc=0
    trap '' INT TERM HUP
    set -m
    (
        trap - INT TERM HUP
        cd "$binary_dir"
        # In this subshell only, and before the exec: the suite binary and the fake service
        # host it launches must not inherit a direct-object loader variable from anywhere,
        # including from this script itself.  See scrub_loader_object_variables.
        scrub_loader_object_variables
        CEC_TEST_AIDL_MODE="$mode" \
        CEC_FAKE_AIDL_HOST_PATH="$HDMICEC_ROOT/$FAKE_HOST_BINARY_REL" \
        LD_LIBRARY_PATH="$ld_path" \
        exec "$TIMEOUT_BIN" --foreground "${TIMEOUT_KILL_AFTER[@]}" "$SUITE_TIMEOUT" \
            "./$binary_name" --gtest_print_time=1 "--gtest_output=json:$results_json" \
                ${select_filter:+"--gtest_filter=$select_filter"} \
                ${GTEST_EXTRA_ARGS:+$GTEST_EXTRA_ARGS}
    ) >"$run_log" 2>&1 &
    SUITE_PGID=$!
    set +m
    # LAUNCHED IS RECORDED HERE, WHILE THE SUITE IS STILL RUNNING, and that is the point of
    # recording it at all: from this line onwards a log exists at $run_log, so every exit path
    # below -- including a cancellation that never reaches the checks and a `die` from any of
    # them -- has an invocation the status marker must describe as launched rather than as one
    # that never ran.  The marker is written from the EXIT trap, so it reads whatever these
    # globals hold at the moment the run ends.
    INVOCATIONS_LAUNCHED="${INVOCATIONS_LAUNCHED}${INVOCATIONS_LAUNCHED:+, }$label"
    trap 'on_signal INT 130' INT
    trap 'on_signal TERM 143' TERM
    trap 'on_signal HUP 129' HUP
    # `|| rc=$?` rather than a set +e / set -e pair: toggling errexit inside a function that may
    # itself have been invoked in a `||` or `if !` context re-arms it where the caller had
    # deliberately suppressed it.  A trapped signal interrupts the wait either way, which is the
    # whole point of waiting rather than running the suite in the foreground.
    #
    # THE PROCESS GROUP IS NOT, BY ITSELF, WHAT REAPS THE FAKE SERVICE HOST -- and this comment
    # said it was until it was measured.  The harness forks the host from inside this group and
    # then makes it a group LEADER of its own, on both sides of the fork
    # (tests/L2Tests/test_main.cpp), so a signal addressed to this group alone reaches `timeout`
    # and the runner and nothing else: measured during a cancelled invocation D, two run_L2Tests
    # processes in a group of their own survived with PPid 1 and were still alive at t=60s.  What
    # reaps the host is stop_suite_group WALKING THE TREE before it signals anything and then
    # addressing every group it finds BY ID -- see THE CANCELLATION block for the measurement and
    # the ordering.  Addressing by id is what matters about the mechanism: a pattern-based kill
    # would match anything merely mentioning the binary's name, up to and including the harness
    # that started this script.
    #
    # THE REGISTRATION SURVIVES THE WAIT, and that is load-bearing.  Cleared on the line after
    # this one it would go before the exit-status check, before the case-count check and before
    # the selected-path check, all three of which are FATAL: a `die` from any of them would then
    # run on_exit with SUITE_PGID already empty, so stop_suite_group would have nothing to
    # address and every surviving member of the group would be orphaned by the very failure that
    # was supposed to end the run.  On invocation E that member is the fake service host.  So it
    # is cleared by finish_suite_group instead, once the invocation is fully accounted for, and
    # every path out of the three checks below -- pass, die, timeout and signal -- goes through
    # a terminate-reap-verify.
    wait "$SUITE_PGID" || rc=$?

    # COMPLETED, WITH THE STATUS, recorded before anything is judged.  The process is gone and
    # its status is in hand: that is a fact about execution and it is true whether the status is
    # 0, a test failure, or the 124 `timeout` reports.  Recording it here rather than after the
    # checks is what stops a rejected invocation being reported as one that never ran, and
    # carrying the status with it is what stops "completed" being read as "completed cleanly".
    INVOCATIONS_COMPLETED="${INVOCATIONS_COMPLETED}${INVOCATIONS_COMPLETED:+, }$label"
    INVOCATION_EXIT_STATUSES="${INVOCATION_EXIT_STATUSES}${label}	${rc}
"

    # STAMP THE GENERATION INTO THE LOG, and do it here rather than as a header.  The redirect
    # above TRUNCATES this file, so anything written before the suite starts is destroyed by the
    # suite's own first byte; and it is written before the three fatal checks below, so a log
    # from a FAILED invocation carries the stamp too -- which is the log a reader is most likely
    # to be holding beside one from another run.  The GoogleTest JSON cannot carry this, because
    # the test binary writes it; run_status.txt is what ties the JSONs to a generation.
    printf '###RUNID invocation=%s run_id=%s runner_pid=%s\n' "$label" "$RUN_ID" "$$" >>"$run_log"

    # The tail is the part a reader needs; the whole log is on disk either way.
    "$AWK_BIN" '/^\[==========\]|^\[  PASSED  \]|^\[  FAILED  \]|^\[  SKIPPED \]|tests? from .* ran/' "$run_log" \
        | sed 's/^/[run_coverage]   /' || true

    # ------------------------------------------------------------------------------------
    # CHECK 1 OF FOUR: the exit status.
    #
    # WHAT `timeout` DOES AND DOES NOT BOUND HERE.  The bound is on the process `timeout`
    # exec'd -- the test binary -- and on nothing else.  `--foreground` is passed so the binary
    # keeps this run's terminal and stays in the group this shell registered, and the measured
    # consequence on the uutils `timeout` this host provides (0.2.2, not GNU) is that a timeout
    # signals THAT process only: a descendant the binary forked, which on an L2 invocation is
    # the fake service host, is not signalled and does not stop.  So the 124/137 status below is
    # a report that the runner was cut short, never evidence that everything it started is gone.
    # WHAT ACTUALLY STOPS THE DESCENDANTS is the process-group signal -- stop_suite_group,
    # reached from on_exit because the `die` here leaves SUITE_PGID registered -- which walks the
    # tree while it is still intact, addresses every process group it finds by id, and then
    # re-probes each one rather than inferring extinction from the kill.  The walk is not
    # optional here: the fake service host leads a group of its own, so a signal to the suite's
    # group alone would leave exactly the descendant this timeout failed to stop.
    # ------------------------------------------------------------------------------------
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        printf '%s\n' "--- last 30 lines ---" >&2
        tail -n 30 -- "$run_log" | sed 's/^/[run_coverage]   /' >&2 || true
        die "invocation $label did not finish within ${SUITE_TIMEOUT}s and was terminated (exit $rc).
       This is a HANG, not a test failure: the last test named in the log above is where it
       stopped.  Coverage is not captured, because a suite that was killed part-way through
       produced partial counters.  Investigate that test, or raise the bound deliberately with
       SUITE_TIMEOUT=<seconds> if the suite has legitimately grown.  Full log: $run_log
       On invocation E specifically, a hang can also mean the fake service host is waiting on a
       service manager that is not running: reaching one blocks in one-second retries until
       binder handle 0 resolves, and this bound is what turns that into a reported failure."
    fi
    if [ "$rc" -ne 0 ]; then
        warn "invocation $label exited $rc. Full log: $run_log"
        printf '%s\n' "--- last 30 lines ---" >&2
        tail -n 30 -- "$run_log" | sed 's/^/[run_coverage]   /' >&2 || true
        die "invocation $label FAILED, so coverage was not measured and no later invocation was run.
       A red suite makes every coverage figure meaningless: fix the tests first.
       Coverage is deliberately NOT captured for a failing run, so that no report can
       ever be produced from one, and the artifacts of this and every earlier invocation
       are left in place under $OUTPUT_DIR for diagnosis."
    fi
    log "invocation $label exited 0"

    # ------------------------------------------------------------------------------------
    # CHECKS 2 AND 3.  Both are FATAL, and both run before the next invocation starts, as
    # does check 4 below.
    # ------------------------------------------------------------------------------------
    verify_results "$results_json" "$label" "$fixture_pattern" \
                   "$selected" "$registered" "$excluded" \
                   "$permit_skip" "$mandatory" "$run_log"
    assert_selected_back_end "$run_log" "$back_end" "$label"

    # ------------------------------------------------------------------------------------
    # CHECK 4 OF FOUR: NOTHING THIS INVOCATION STARTED IS STILL RUNNING.
    #
    # THE INVOCATION IS ONLY NOW FULLY ACCOUNTED FOR, so and only so is the process group
    # released.  Every fatal path above left SUITE_PGID registered on purpose, which is what
    # lets on_exit's stop_suite_group reach the group instead of finding nothing to reach.
    #
    # AND THE RESULT IS GATED, which is the whole of this check.  Called for its side effect
    # with its status thrown away, finish_suite_group would let an invocation that passed its
    # three checks while leaking a live descendant be recorded as a pass, and the matrix would go
    # on to the next letter.  On an L2 invocation that descendant is the fake service host, holding
    # the "HdmiCec" name that IHdmiCec::serviceName() fixes in production code -- so the NEXT
    # invocation resolves against a process from this one and reports a selection that is not
    # the one under test.  A leaked descendant therefore fails the invocation it belongs to,
    # here, before any later invocation can inherit it.
    #
    # `die` rather than a status carried onwards, because that is what checks 1 to 3 above do and
    # this script keeps ONE verdict and one exit status: the matrix stops at the first failing
    # invocation, nothing is captured, and apply_gate is never reached.  Every artifact written
    # so far survives for diagnosis.
    # ------------------------------------------------------------------------------------
    local close_rc=0
    finish_suite_group "$label" || close_rc=$?
    if [ "$close_rc" -eq 2 ]; then
        die "invocation $label passed its three checks but LEFT A PROCESS RUNNING that could not
       be killed, in the process group named above.  Coverage is not captured and no later
       invocation is run: on this host that process still holds whatever it registered -- the
       fixed \"HdmiCec\" service name, on an L2 invocation -- so every later AIDL invocation
       would resolve against it and report a selection that is not the one under test.
       Kill it by hand before re-running.  Full log: $run_log"
    elif [ "$close_rc" -ne 0 ]; then
        die "invocation $label passed its three checks but LEAKED A DESCENDANT PROCESS, which
       this script has now terminated (see the group id above).  The invocation is failed
       deliberately rather than reported clean: its results were produced with a process alive
       that the harness's own teardown is supposed to have reaped, so the harness -- not this
       script -- has the defect, and the next invocation must not be run on the strength of
       evidence gathered in that state.  Full log: $run_log"
    fi

    log "invocation $label PASSED all four checks (exit status, case count, selected back-end,"
    log "  and an empty process group at close-out)"
}

# ------------------------------------------------------------------------------------
# Run the whole matrix, in order, stopping at the first invocation that fails.
#
# WHY IT STOPS RATHER THAN COLLECTING FAILURES.  The counters accumulate into one set, and a
# capture taken after a failed invocation would describe a mixture of complete and incomplete
# executions with no way to tell which lines came from which.  There is no useful report to be
# had from a partial matrix, so the run ends where the evidence stops being attributable -- and
# every artifact written so far survives, because they are per-invocation and nothing overwrites
# them.
#
# AN INVOCATION THAT CANNOT RUN IS DEFERRED, NOT SKIPPED QUIETLY AND NEVER PASSED.  On a host
# with no binder driver, B, C and E cannot execute at all -- an attempt would abort inside
# libbinder rather than fail a test.  Those are recorded as deferred, listed in the summary, and
# turned into an advisory reason, so the run cannot produce an acceptance verdict while claiming
# five invocations' worth of evidence from two.
# ------------------------------------------------------------------------------------
# ------------------------------------------------------------------------------------
# WHAT AN INVOCATION REACHED, IN THREE INDEPENDENT STATES RATHER THAN ONE.
#
# THE DEFECT THIS REPLACES.  There used to be one list, appended to only AFTER
# run_one_invocation RETURNED -- and every evidence check inside that function is fatal.  So an
# invocation that launched, ran, PASSED every one of its cases and then failed an evidence check
# was reported by the status marker as `ran : none` and by the per-letter artifact list as "did
# not run in this generation", with its log and its results file sitting on disk beside the
# marker saying so.  That is the most misleading available summary of "it ran and its evidence
# was rejected": a reader looking for whether the suite executed is told it did not.
#
# THE THREE STATES, and why each is separate:
#   LAUNCHED   the process was started, so a log EXISTS and must be listed whatever came next.
#   COMPLETED  the process is gone and its exit status was collected.  Recorded with that status,
#              because "completed, exit 124" (the timeout fired) and "completed, exit 0" are
#              both completions and lead to entirely different conclusions.
#   VALIDATED  all four checks passed: exit status, case-count reconciliation, exactly one
#              correct selected-path line, and an empty process group at close-out.
# DEFERRED stays what it was -- never attempted, for a named reason -- and is reported apart
# from all three, because an invocation nobody started is not an invocation that failed.
#
# INVOCATIONS_RUN KEEPS ITS NAME AND ITS MEANING, deliberately: it is the VALIDATED list, and
# branch_arm_unmet_requirement resolves the manifest's per-arm invocation tokens against it.
# Widening it to "launched" would make the branch gate judge arms on the strength of an
# invocation whose evidence was rejected, which is the opposite of this finding's remedy.
#
# NOTHING HERE MAKES A FAILURE SURVIVABLE.  Every `die` in run_one_invocation still ends the run
# at the first failing invocation with nothing captured.  What changes is only what the status
# marker SAYS about the invocation that failed.
# ------------------------------------------------------------------------------------
INVOCATIONS_LAUNCHED=''
INVOCATIONS_COMPLETED=''
INVOCATIONS_RUN=''
INVOCATIONS_DEFERRED=''
INVOCATION_EXIT_STATUSES=''

# Membership with delimiters on both sides, so "A" cannot match a future invocation named "AB".
# The same shape as branch_arm_unmet_requirement's own test, which is where it was measured.
letter_in_list() { # $1=letter  $2=', '-joined list;  0 when the letter is in the list
    case ", $2, " in
        *", $1, "*) return 0 ;;
    esac
    return 1
}

# The state of one invocation as a phrase, for the status marker.  One function so that the
# vocabulary is identical everywhere a reader meets it, and so that "not reached" -- the matrix
# stopped at an earlier letter -- is never rendered as though the invocation had been considered
# and declined.
invocation_state_phrase() { # $1=letter
    local letter="$1" rc
    if letter_in_list "$letter" "$INVOCATIONS_DEFERRED"; then
        printf 'DEFERRED -- not attempted on this host, and not passed\n'
        return 0
    fi
    if ! letter_in_list "$letter" "$INVOCATIONS_LAUNCHED"; then
        printf 'not reached: the matrix ended before this invocation started\n'
        return 0
    fi
    rc="$(printf '%s' "$INVOCATION_EXIT_STATUSES" \
          | "$AWK_BIN" -v want="$letter" '$1 == want { print $2; exit }')"
    if ! letter_in_list "$letter" "$INVOCATIONS_COMPLETED"; then
        printf 'LAUNCHED and did not complete: no exit status was collected, so it was killed or cancelled mid-run\n'
        return 0
    fi
    if letter_in_list "$letter" "$INVOCATIONS_RUN"; then
        printf 'launched, completed (exit %s), evidence validated\n' "${rc:-?}"
        return 0
    fi
    printf 'launched, completed (exit %s), EVIDENCE NOT VALIDATED -- it ran; its evidence was rejected\n' "${rc:-?}"
    return 0
}

run_invocation_matrix() {
    local binder_available=0
    if binder_transport_present; then
        binder_available=1
    fi

    rule
    log "the invocation matrix: ${#INVOCATION_MATRIX[@]} invocation(s), one process each,"
    log "  all against this one build so the gcov counters accumulate into a single capture"
    if [ "$binder_available" -eq 1 ]; then
        log "  binder transport: $BINDER_DRIVER_NODE is present and usable, so every invocation runs"
    else
        log "  binder transport: $BINDER_DRIVER_NODE is absent or unusable on this host"
        log "  => the AIDL-selected invocations are DEFERRED, not attempted and not passed."
        log "     Executing one needs a kernel with binder support, a matching binder protocol"
        log "     version and a running service manager.  Compiling and linking the back-end"
        log "     needs none of those and has already been verified by the build."
    fi

    local record label tier mode back_end select_filter exclude_filter synopsis permit_skip
    for record in "${INVOCATION_MATRIX[@]}"; do
        IFS='|' read -r label tier mode back_end select_filter exclude_filter synopsis permit_skip <<< "$record"

        if [ "$binder_available" -eq 0 ] && invocation_needs_binder "$label"; then
            rule
            warn "INVOCATION $label DEFERRED -- $synopsis"
            warn "  It needs the binder transport and $BINDER_DRIVER_NODE is not usable here."
            warn "  Not attempted: on the pinned binder stack, registering a service name calls"
            warn "  defaultServiceManager(), which opens the driver and ABORTS the process when"
            warn "  the node is missing -- so an attempt would take the runner down with it and"
            warn "  destroy the counters the invocations that CAN run have accumulated."
            INVOCATIONS_DEFERRED="${INVOCATIONS_DEFERRED}${INVOCATIONS_DEFERRED:+, }$label"
            continue
        fi

        run_one_invocation "$label" "$tier" "$mode" "$back_end" \
                           "$select_filter" "$exclude_filter" "$synopsis" "$permit_skip"
        # EVIDENCE-VALIDATED, and only here: reaching this line means run_one_invocation
        # returned, which it does only after all four checks passed -- every one of them exits
        # the run instead of returning.  The launched and completed states were recorded inside
        # that function, at the moments they became true, so a failing invocation is still
        # described accurately by the status marker.
        INVOCATIONS_RUN="${INVOCATIONS_RUN}${INVOCATIONS_RUN:+, }$label"
    done

    rule
    log "invocations launched : ${INVOCATIONS_LAUNCHED:-none}"
    log "invocations completed: ${INVOCATIONS_COMPLETED:-none}  (a status was collected; see the log for which)"
    log "invocations validated: ${INVOCATIONS_RUN:-none}  (all four checks passed)"
    log "invocations deferred : ${INVOCATIONS_DEFERRED:-none}"
    [ -n "$INVOCATIONS_RUN" ] || die "no invocation ran at all, so there is nothing to measure.
       Every one of them was deferred for want of a binder transport, which cannot be right:
       invocations A and D need none.  Check that both runners exist and are executable."

    if [ -n "$INVOCATIONS_DEFERRED" ]; then
        note_advisory "invocation(s) $INVOCATIONS_DEFERRED were DEFERRED, not run: this host has no
       usable binder transport at $BINDER_DRIVER_NODE.  The figures below therefore describe
       only invocation(s) $INVOCATIONS_RUN, so no arm of the selection branch that needs the
       AIDL back-end was exercised and the branch gate cannot be satisfied from this run.
       Re-run the whole matrix on a binder-capable host for an acceptance verdict; nothing here
       may be reported as though those invocations had passed."
    fi
}


# ------------------------------------------------------------------------------------
# Artifact paths.  Fixed names, matching CI's, inside a directory that defaults outside
# the git tree.  Every destination is checked before it is written: a symlink or a
# wrong-typed object at one of these paths would otherwise let a run scribble somewhere
# nobody intended.
# ------------------------------------------------------------------------------------
RAW_TRACE=''
FILTERED_TRACE=''
LINE_GATE_TRACE=''
HTML_DIR=''
PER_FILE_TSV=''
UNCOVERED_TXT=''

# ------------------------------------------------------------------------------------
# ONE GENERATION PER DIRECTORY -- THE RUN IDENTITY, THE PURGE AND THE STATUS MARKER.
#
# THE DEFECT THIS CLOSES.  Every artifact name above is FIXED, which clause 5 of the governing
# contract requires and which this design keeps: fixed names are what make a local run's bundle
# interchangeable with CI's.  But fixed names carry a consequence of their own.  Point an
# explicit --output-dir at a directory a previous run already wrote, and every artifact that
# THIS run happens not to produce is left standing from the previous one -- with a modification
# time, a plausible name and no way for a reader to tell it apart from this run's output.
#
# The worst case is precisely the case this host produces.  Invocations B, C and E DEFER here,
# and they defer BEFORE run_one_invocation writes anything, so rdkTestResults_invocation_B.json
# and run_invocation_B.log are never created.  A directory that once held a full binder-capable
# A-E bundle would therefore present a complete five-invocation result set of which two
# invocations never ran in this generation -- and the per-invocation JSON is the artifact the
# mandatory-pass contract's evidence is read from.  That is a false green with a file to point
# at, which is worse than no evidence at all.
#
# WHY A PURGE AND NOT A PRIVATE STAGING DIRECTORY.  Staging one complete generation and
# publishing it by rename is the other correct answer, and it is what generate_html already does
# for the HTML tree.  It is the wrong answer for the directory as a whole: RAW_TRACE and the five
# other artifact paths are declared `readonly` the moment prepare_output_dir resolves them, on
# purpose, so that nothing downstream can re-point a destination mid-run.  Staging would mean
# making all six mutable and rewriting every consumer to compose paths at use time -- a change
# whose blast radius is every write in the file, to fix a problem that removing a known,
# enumerated file set solves completely.  The purge also degrades better: an interrupted purge
# has removed some of a stale generation, which is strictly safer than leaving all of it.
#
# WHAT IS REMOVED, AND WHAT IS DELIBERATELY NOT.  The set below is the complete list of fixed
# names any code path in this file writes below OUTPUT_DIR, enumerated by name.  It is NOT an
# `rm -rf "$OUTPUT_DIR"`: the caller may legitimately have named a directory that holds other
# things (a CI job's shared artifact root, for instance), and recursively deleting a
# caller-supplied path is how a runner destroys something it was never asked to touch.
#
#   .run.lock       is EXCLUDED, and must stay excluded.  This run holds an open descriptor and
#                   an flock on it; removing it would unlink the inode a second contender is
#                   about to create afresh, splitting two runs across two inodes -- the exact
#                   failure mode acquire_tree_lock's identity checks exist to prevent, recreated
#                   here by a cleanup.
#   run_status.txt  is REPLACED FIRST, ATOMICALLY, AND WITH THIS RUN'S OWN MARKER, and both
#                   halves of that are load-bearing.  There must be no window in which the
#                   directory holds artifacts and no marker saying which run they belong to, so
#                   removing the leaf and writing a new one later is not acceptable -- and a
#                   rewrite the run merely gets round to is not acceptable either.  An optional
#                   rewrite (one that warns and returns 0 on failure) happening AFTER the purge
#                   leaves a directory whose marker leaf could not be replaced carrying the
#                   PREVIOUS generation's COMPLETE-PASS while this run removes the rest of that
#                   generation and writes its own artifacts beside it.  A current artifact set
#                   under a prior run's acceptance verdict is the worst outcome available here,
#                   and it reaches a reader with no warning anywhere in the bundle.
#                   So the marker is the FIRST thing the purge touches, replaced by rename with
#                   a marker naming THIS run id, before any other artifact of the previous
#                   generation is removed; a leaf that cannot be rewritten is removed instead;
#                   and a leaf that can be neither rewritten nor removed ENDS THE RUN.  There is
#                   no path from here on which another generation's verdict outlives the purge.
#   .run_status.txt.tmp is in the purge list below and is removed like any other artifact: it is
#                   this mechanism's own temp leaf, and a stale one is content from another run.
#
# THE RUN ID DOES NOT GO INTO ANY FILENAME.  Clause 5 forbids a timestamp in an artifact name,
# and a run id in a name would break the interchangeability the fixed names buy.  It goes into
# the CONTENT: the status marker, the provenance artifact and the header of every per-invocation
# log.  The one artifact that cannot carry it is the GoogleTest JSON, which the test binary
# writes and this script only reads -- which is itself a reason the marker enumerates, per
# invocation, which JSONs belong to this generation.
# ------------------------------------------------------------------------------------
RUN_ID=''
RUN_STATUS_TXT=''

# Every fixed name written below OUTPUT_DIR by any path in this file.  Kept in one place so that
# adding an artifact and forgetting to purge it is a single-line omission a reader can spot,
# rather than a name that exists in one function and in nobody's cleanup.
readonly PURGEABLE_ARTIFACT_FILES=(
    'coverage.info'
    'filtered_coverage.info'
    'line_gate_coverage.info'
    'per_file_coverage.tsv'
    'uncovered_lines.txt'
    'provenance.txt'
    'build.log'
    'capture.log'
    'filter.log'
    'filter_dependencies.log'
    'genhtml.log'
    '.per_file_coverage.tsv.tmp'
    '.uncovered_lines.txt.tmp'
    '.run_status.txt.tmp'
)
readonly PURGEABLE_ARTIFACT_DIRS=(
    'coverage'
)
# Per-invocation artifacts, one pair per letter in the matrix.  Spelled from the letters rather
# than from the matrix array so that this list is complete even when a filter or a deferral means
# the matrix is never walked.
readonly PURGEABLE_INVOCATION_LETTERS='A B C D E'

# ------------------------------------------------------------------------------------
# Mint the run identity.  Readable, unique per run, and cheap: a UTC timestamp for a human
# reading two artifacts side by side, and the pid so that two runs started inside the same
# second are still distinguishable.  Not used to name anything -- see above.
# ------------------------------------------------------------------------------------
mint_run_id() {
    local stamp
    stamp="$(date -u '+%Y%m%dT%H%M%SZ' 2>/dev/null)" || stamp=''
    [ -n "$stamp" ] || stamp='time-unavailable'
    RUN_ID="${stamp}-pid${$}"
    readonly RUN_ID
}

# ------------------------------------------------------------------------------------
# STEP ZERO OF THE PURGE: TAKE THE MARKER AWAY FROM THE PREVIOUS GENERATION, ATOMICALLY, AND
# BEFORE ANYTHING ELSE IS TOUCHED.
#
# The marker is the one artifact whose staleness is dangerous rather than merely confusing: it is
# the authoritative statement of a run's verdict, and every other artifact in the directory is
# read in its light.  So it is the first thing the purge deals with, and it is dealt with by
# RENAME rather than by remove-then-write-later: a rename replaces the old marker with a marker
# naming THIS run in one step, so there is no instant at which the directory holds a previous
# generation's artifacts and no marker, and none at which it holds them under a stale verdict.
#
# THE ORDER OF PREFERENCE IS DELIBERATE.
#   1. Replace it with this run's PURGING marker.  A reader arriving mid-purge sees this run id
#      and a phase that says the directory is mid-takeover, which is the truth.
#   2. If it cannot be replaced -- an immutable leaf, an ACL, an I/O failure -- REMOVE it.  A
#      directory with artifacts and no marker is unattributable, which is strictly safer than
#      one whose marker asserts another run's COMPLETE-PASS over this run's files.
#   3. If it can be neither replaced nor removed, END THE RUN.  Measured on this host: `chattr
#      +i` on the leaf makes both `mv` onto it and `rm -f` of it fail with EPERM, so this arm is
#      reachable and is not theoretical.  Dying here is what makes the guarantee absolute: the
#      run stops before it has removed one previous-generation artifact and before it has written
#      one of its own, so the directory is left exactly as it was found, wholly the previous
#      generation's, with nothing of this run's mixed into it.
#
# A NON-REGULAR OBJECT AT THE MARKER NAME CANNOT REACH THIS FUNCTION: prepare_output_dir runs
# assert_safe_artifact_path "$RUN_STATUS_TXT" file first, which dies on a symlink, a directory or
# anything else that is not a regular file.  The `-L`/`-f` tests below are therefore a
# re-assertion at the moment of use, in the posture every custody path in this script has, and
# the reason the fallback needs only `rm -f`.
# ------------------------------------------------------------------------------------
invalidate_previous_run_status() {
    [ -n "$RUN_STATUS_TXT" ] || return 0
    # -e is false for a dangling symlink, so test both.
    if [ ! -e "$RUN_STATUS_TXT" ] && [ ! -L "$RUN_STATUS_TXT" ]; then
        return 0          # no marker to take away; the required IN-PROGRESS write puts ours in
    fi
    assert_output_dir_still_safe "$RUN_STATUS_TXT"

    if [ ! -L "$RUN_STATUS_TXT" ] && [ -f "$RUN_STATUS_TXT" ] && write_run_status 'PURGING' \
        "this run has taken the directory over; the previous generation is being removed"; then
        log "the status marker in $OUTPUT_DIR now names this run (id $RUN_ID); the previous"
        log "  generation's verdict has been superseded before any of its artifacts were touched"
        return 0
    fi

    warn "the status marker at $RUN_STATUS_TXT could not be replaced by this run's."
    warn "  Removing it instead: a directory whose artifacts have no marker is unattributable,"
    warn "  which is safe, whereas one carrying another run's verdict over this run's files is"
    warn "  a false green with a file to point at."
    rm -f -- "$RUN_STATUS_TXT" || die "the previous run's status marker can be neither replaced
       nor removed: $RUN_STATUS_TXT
       Check for an immutable bit (lsattr), an ACL, or a read-only filesystem.  This run has
       stopped BEFORE removing any artifact of the previous generation and before writing any of
       its own, so that directory is still wholly the previous run's and nothing in it is a
       mixture.  Clear the obstruction, or point --output-dir/COVERAGE_OUTPUT_DIR at a directory
       whose marker this run can own: continuing would mean writing this run's artifacts under
       another run's recorded verdict, which is the one thing this mechanism exists to prevent."
    log "the previous run's status marker was removed; this run's IN-PROGRESS marker follows"
    return 0
}

# ------------------------------------------------------------------------------------
# Remove every artifact a previous generation could have left in this directory.
#
# Called with the tree lock AND the output lock held, and with OUTPUT_DIR_IDENTITY already
# recorded, so the directory being emptied is provably the one this run validated.  Every removal
# re-checks custody first, because "the directory was safe when the run started" is a statement
# about the past and a removal happens now.
# ------------------------------------------------------------------------------------
purge_previous_generation() {
    local name path removed=0 letter

    # FIRST, AND BEFORE ANY REMOVAL.  See the function above: the marker is the artifact whose
    # staleness turns the rest of the bundle into a false green, so this run owns it before it
    # begins removing anything the previous generation left.
    invalidate_previous_run_status

    for name in "${PURGEABLE_ARTIFACT_FILES[@]}"; do
        path="$OUTPUT_DIR/$name"
        # -e is false for a dangling symlink, so test both: a stale symlink at an artifact name
        # is exactly the object that must not survive into this generation.
        if [ -e "$path" ] || [ -L "$path" ]; then
            assert_output_dir_still_safe "$path"
            rm -f -- "$path" || die "could not remove the previous generation's $name from
       $OUTPUT_DIR
       This run cannot continue: leaving it would put an artifact from another run beside this
       run's own, under the same name the reader expects to be current."
            removed=$((removed + 1))
        fi
    done

    for letter in $PURGEABLE_INVOCATION_LETTERS; do
        for name in "rdkTestResults_invocation_${letter}.json" "run_invocation_${letter}.log"; do
            path="$OUTPUT_DIR/$name"
            if [ -e "$path" ] || [ -L "$path" ]; then
                assert_output_dir_still_safe "$path"
                rm -f -- "$path" || die "could not remove the previous generation's $name from
       $OUTPUT_DIR
       An invocation that DEFERS in this run never writes its own results file, so a stale one
       left here would be read as this run's evidence for an invocation that did not happen."
                removed=$((removed + 1))
            fi
        done
    done

    for name in "${PURGEABLE_ARTIFACT_DIRS[@]}"; do
        path="$OUTPUT_DIR/$name"
        if [ -L "$path" ]; then
            assert_output_dir_still_safe "$path"
            rm -f -- "$path" || die "could not remove a symlink at the artifact path $path"
            removed=$((removed + 1))
        elif [ -d "$path" ]; then
            assert_output_dir_still_safe "$path"
            # Recursive, but ONLY over a name from the fixed list above and only after custody
            # has just been re-verified -- never over the caller's directory itself.
            rm -rf -- "$path" || die "could not remove the previous generation's $name/ from
       $OUTPUT_DIR
       A partially overwritten HTML report shows pages from two different traces."
            removed=$((removed + 1))
        fi
    done

    # Any leftover HTML staging directory from a run that was killed between mktemp and its
    # own cleanup.  Matched by the fixed prefix, so nothing outside this script's own naming
    # can be caught by it; the glob is evaluated with nullglob-equivalent care below.
    local stage
    for stage in "$OUTPUT_DIR"/.genhtml-stage.*; do
        [ -e "$stage" ] || continue          # no match: the glob came back literal
        assert_output_dir_still_safe "$stage"
        rm -rf -- "$stage" || die "could not remove an abandoned HTML staging directory: $stage"
        removed=$((removed + 1))
    done

    if [ "$removed" -gt 0 ]; then
        log "removed $removed artifact object(s) left by a previous run in $OUTPUT_DIR"
        log "  this directory now holds exactly one generation: run id $RUN_ID"
    else
        log "no artifacts from a previous run were present; run id $RUN_ID"
    fi
}

# ------------------------------------------------------------------------------------
# The authoritative status marker.
#
# Written three times at most and always by rename, so it is never half a file and never another
# run's: PURGING as this run takes a reused directory over (invalidate_previous_run_status),
# IN-PROGRESS the moment the directory is prepared, and the run's real outcome from the EXIT trap.
# A reader that finds artifacts and a marker saying IN-PROGRESS knows the run was killed; one that
# finds a marker naming a different run id than the one they expected knows the bundle is not
# theirs.  A reader is the one party who cannot be asked to retry.
#
# TOLERANT OF BEING CALLED TOO EARLY, AND ONLY OF THAT.  The EXIT trap runs for every exit,
# including a die inside require_tools before an output directory exists.  With no directory
# there is nothing to mark and nothing to mislead, so that one case returns 0 quietly.
#
# EVERY OTHER FAILURE RETURNS NON-ZERO, and that is what keeps the marker from being advisory
# in exactly the situations it exists for.  All three of them -- a refused custody check, a temp
# file that cannot be written, and a rename that cannot be completed -- are reported to the
# caller rather than warned about and swallowed.  A version that warned and returned 0 would
# leave a reused directory whose marker leaf cannot be replaced (a path-specific immutable bit,
# an ACL, an I/O error) carrying the PREVIOUS run's COMPLETE-PASS verdict while this run removes
# the rest of that generation, writes its own artifacts beside it and still exits 0 -- the
# mixed-generation false green in its purest form: a current artifact set under a prior run's
# acceptance verdict.
#
# The status is what the callers act on, each differently and each documented at its call site:
# purge_previous_generation falls back to removing the leaf, prepare_output_dir dies, and
# on_exit turns a clean run non-zero while leaving the cancellation status of a cancelled one
# alone.  Nothing here decides the run's fate; it only reports honestly whether the marker on
# disk is this run's.
#
#   0 -- the marker on disk now names this run and this phase, or there is no directory to mark.
#   1 -- the marker on disk is NOT this run's.  Whatever stands at that path is either the
#        previous generation's or nothing at all.
# ------------------------------------------------------------------------------------
write_run_status() { # $1=phase word  $2=one-line detail
    local phase="$1" detail="$2" tmp letter
    [ -n "$RUN_STATUS_TXT" ] || return 0
    [ -n "$OUTPUT_DIR" ] && [ -d "$OUTPUT_DIR" ] || return 0

    # A failure here must not mask the run's own outcome, so custody is CHECKED and a refusal
    # warns rather than dies -- this is the last thing a cancelled run does -- but it is
    # REPORTED, because a marker that was not written is not a marker.
    if ! ( assert_output_dir_still_safe "$RUN_STATUS_TXT" ) >/dev/null 2>&1; then
        warn "not writing the run status marker: the output directory no longer passes its"
        warn "  custody check ($OUTPUT_DIR).  Nothing was written."
        return 1
    fi

    # THE DESTINATION MUST BE A REGULAR FILE OR ABSENT, and this check is not defensive
    # boilerplate.  Measured on this host: `mv -f -- <tmp> <path>` where <path> is a DIRECTORY
    # moves the temp file INSIDE it and exits 0.  Without this refusal a directory at the marker
    # name -- planted, or left by an interrupted tool -- would make the publish below report
    # success while nothing was published under the name a reader looks at, which is the one
    # failure mode this whole mechanism must not have.  A symlink is already refused by the
    # custody check above; anything else non-regular is refused here.
    if [ -e "$RUN_STATUS_TXT" ] && [ ! -f "$RUN_STATUS_TXT" ]; then
        warn "not writing the run status marker: $RUN_STATUS_TXT exists and is not a regular"
        warn "  file, so it cannot be replaced by rename.  Remove it, or choose another"
        warn "  --output-dir; this run's marker cannot be published while it stands there."
        return 1
    fi

    tmp="$OUTPUT_DIR/.run_status.txt.tmp"
    {
        printf 'HDMI-CEC middleware L1 coverage -- AUTHORITATIVE RUN STATUS\n'
        printf '==========================================================\n\n'
        printf 'Run id           : %s\n' "$RUN_ID"
        printf 'Phase            : %s\n' "$phase"
        printf 'Detail           : %s\n' "$detail"
        printf 'Written (UTC)    : %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || printf 'unavailable')"
        printf 'Runner pid       : %s\n' "$$"
        printf 'Output directory : %s\n' "$OUTPUT_DIR"
        printf '\nWHAT THIS RUN ASKED FOR\n'
        printf '  --build          : %s\n' "$( [ "$DO_BUILD" -eq 1 ] && printf yes || printf no )"
        printf '  --run            : %s\n' "$( [ "$DO_RUN" -eq 1 ] && printf yes || printf no )"
        printf '  line bar         : %s%%  (per-file gating: %s)\n' \
            "$COVERAGE_MIN" "$( [ "$COVERAGE_PER_FILE_GATE" -eq 1 ] && printf on || printf off )"
        # ------------------------------------------------------------------------------
        # THREE STATES, NOT ONE, because "did it run" and "was its evidence accepted" are
        # different questions and this marker used to answer the first with the second.  An
        # invocation whose cases all passed and whose evidence was then rejected was reported
        # here as `ran : none`, with its log and its results file on disk beside the marker.
        # The vocabulary is defined at INVOCATIONS_LAUNCHED; each list is cumulative and each
        # is a subset of the one above it.
        # ------------------------------------------------------------------------------
        printf '\nINVOCATIONS\n'
        printf '  launched           : %s\n' "${INVOCATIONS_LAUNCHED:-none}"
        printf '  completed          : %s\n' "${INVOCATIONS_COMPLETED:-none}"
        printf '  evidence-validated : %s\n' "${INVOCATIONS_RUN:-none}"
        printf '  deferred           : %s\n' "${INVOCATIONS_DEFERRED:-none}"
        printf '%s\n' "  launched means a process started and a log exists; completed means its exit"
        printf '%s\n' "  status was collected; evidence-validated means all four checks passed (exit"
        printf '%s\n' "  status, case-count reconciliation, exactly one correct selected-path line, and"
        printf '%s\n' "  an empty process group at close-out).  A letter that is launched and completed"
        printf '%s\n' "  but not validated RAN: its evidence was rejected, and the reason is in its log."
        printf '\nPER-INVOCATION ARTIFACTS BELONGING TO THIS GENERATION\n'
        printf '%s\n' "  (the GoogleTest JSON is written by the test binary and cannot carry the run"
        printf '%s\n' "   id, so this list is what says which of those files are this run's)"
        # THE PURGING PHASE MUST NOT CLAIM AN INVENTORY, and this is not a cosmetic distinction.
        # This marker is written by invalidate_previous_run_status, which runs BEFORE the previous
        # generation's per-invocation files are removed -- so every one of them is still on disk
        # at this instant.  Listing them under a heading that says "belonging to this generation"
        # would put the previous run's results files into this run's inventory, which is the
        # mislabelling this whole mechanism exists to prevent.
        if [ "$phase" = 'PURGING' ]; then
            printf '%s\n' "  none yet: this run has taken the directory over and is still removing the"
            printf '%s\n' "  previous generation.  Any per-invocation file present at this moment belongs"
            printf '%s\n' "  to THAT generation and is about to be removed, so none is listed here."
        else
            # THE ARTIFACTS ARE LISTED FROM WHAT IS ON DISK, AND THE STATE FROM WHAT HAPPENED --
            # two independent sources, both reported.  Keying the whole entry on the results
            # JSON, as this used to, made an invocation that produced a log and no JSON read as
            # one that never ran; and an invocation can legitimately have a log without a JSON
            # (a suite killed before it wrote one, or a diverted output path), which is a
            # difference a reader needs rather than one to hide.
            for letter in $PURGEABLE_INVOCATION_LETTERS; do
                local have_json='' have_log='' artifacts=''
                if [ -f "$OUTPUT_DIR/rdkTestResults_invocation_${letter}.json" ]; then
                    have_json="rdkTestResults_invocation_${letter}.json"
                fi
                if [ -f "$OUTPUT_DIR/run_invocation_${letter}.log" ]; then
                    have_log="run_invocation_${letter}.log"
                fi
                artifacts="${have_json}${have_json:+${have_log:+, }}${have_log}"
                printf '  %s : %s\n' "$letter" "$(invocation_state_phrase "$letter")"
                printf '      artifacts: %s\n' "${artifacts:-none in this directory}"
                if [ -n "$have_log" ] && [ -z "$have_json" ]; then
                    printf '%s\n' "      the log exists and the results JSON does not: the process ran and either"
                    printf '%s\n' "      did not reach the point of writing its JSON or wrote it elsewhere.  Read"
                    printf '%s\n' "      the log before concluding anything about what executed."
                fi
            done
        fi
        printf '\nHOW TO READ THIS\n'
        printf '  Phase IN-PROGRESS means the run did not reach its own end: it was cancelled or\n'
        printf '  killed, and every artifact beside this marker is from an incomplete run.\n'
        printf '%s\n' "  Phase PURGING means it did not get past taking the directory over: this run"
        printf '  claimed the marker and then stopped, so whatever else is here is a mixture of\n'
        printf '  what the previous run left and nothing of this one.  Treat none of it as evidence.\n'
        printf '%s\n' "  A run id that is not the one you started is somebody else's bundle: the fixed"
        printf '  artifact names are shared by every run that writes this directory, and this\n'
        printf '  marker is the only thing that says which run they came from.\n'
    } >"$tmp" || { warn "could not write the run status marker to $tmp"; rm -f -- "$tmp" 2>/dev/null || true; return 1; }

    mv -f -- "$tmp" "$RUN_STATUS_TXT" \
        || { warn "could not publish the run status marker to $RUN_STATUS_TXT"; rm -f -- "$tmp"; return 1; }
    return 0
}

assert_safe_artifact_path() { # $1=path  $2=file|dir
    local path="$1" kind="$2"
    [ ! -L "$path" ] || die "refusing to write through a symlink: $path
       Remove it, or choose another --output-dir."
    if [ -e "$path" ]; then
        case "$kind" in
            file) [ -f "$path" ] || die "artifact path exists and is not a regular file: $path" ;;
            dir)  [ -d "$path" ] || die "artifact path exists and is not a directory: $path" ;;
            # Without this arm a mistyped kind would silently skip the type check and let a
            # later step write through whatever object is sitting at that path.
            *)    die "assert_safe_artifact_path: unknown kind '$kind' for $path (expected 'file' or 'dir')" ;;
        esac
    fi
}

# ------------------------------------------------------------------------------------
# EVIDENCE CUSTODY.  The default output directory is under the parent
# select_safe_temp_parent chose, and its name is
# derivable from the workspace path -- so on a shared host any local user can work out
# where the next run will write and pre-create it, or plant files in it, BEFORE this run
# starts.  Everything downstream then treats what it finds there as this run's evidence:
# the traces are what the gate reads and what the traceability report quotes.  Predictable
# plus writable-by-others is the whole vulnerability, so three things are required of the
# directory before a single byte is written into it, and the third is the one that closes
# it rather than merely narrowing it:
#   * it is not a symlink, and it is a directory (already checked below);
#   * it is owned by the user running this script, and grants no group or other write --
#     created that way under umask 077, and refused rather than silently repaired when it
#     already exists with someone else's ownership;
#   * every writable ancestor between it and / is either owned by root or by this user, or
#     carries the sticky bit -- the /tmp case -- because an attacker who can rename an
#     ancestor can substitute the whole subtree however tight its own mode is.
# A run also takes an exclusive lock on the directory for its whole duration, so two runs
# cannot interleave their captures into one set of artifact names and hand the gate a mix.
# ------------------------------------------------------------------------------------
assert_owned_and_private() { # $1=path
    local path="$1" owner mode
    owner="$(stat -c '%u' -- "$path" 2>/dev/null)" \
        || die "cannot stat the artifact directory: $path"
    mode="$(stat -c '%a' -- "$path" 2>/dev/null)"
    if [ "$owner" != "$(id -u)" ]; then
        die "the artifact directory $path is owned by uid $owner, not by you ($(id -u)).
       Evidence this run is judged on must not be under another account's control: another
       owner can replace a trace between the capture and the gate.  Remove it, or point
       --output-dir/COVERAGE_OUTPUT_DIR somewhere you own."
    fi
    case "$mode" in
        *[2367]|*[2367]?) : ;;   # group- or other-writable bit set somewhere
        *) return 0 ;;
    esac
    chmod go-w -- "$path" 2>/dev/null \
        || die "the artifact directory $path is group- or other-writable (mode $mode) and the
       permissions could not be tightened.  A directory anyone can write is a directory
       anyone can plant a trace in."
    log "tightened the artifact directory to owner-only write (was mode $mode)"
}

assert_ancestors_safe() { # $1=path -- walks upwards from the parent to /
    local dir owner mode uid
    uid="$(id -u)"
    dir="$(dirname -- "$1")"
    while : ; do
        owner="$(stat -c '%u' -- "$dir" 2>/dev/null)" || break
        mode="$(stat -c '%a' -- "$dir" 2>/dev/null)"
        # World- or group-writable is fine when the sticky bit is set (this is exactly /tmp)
        # or when the directory belongs to root or to us; otherwise a third party can rename
        # this component and substitute everything beneath it.
        case "$mode" in
            *[2367]|*[2367]?)
                # `-k` tests the sticky bit exactly, which a mode-string prefix match does not:
                # a sticky directory that is also setgid reads as 3777, not 1777, and would be
                # mistaken for non-sticky by a leading-1 test.
                if [ ! -k "$dir" ] && [ "$owner" != 0 ] && [ "$owner" != "$uid" ]; then
                    die "$dir is writable by others (mode $mode, owned by uid $owner) and is an
       ancestor of the artifact directory.  Anyone who can rename it can substitute the whole
       evidence tree.  Choose an output directory whose ancestors are owned by you or by root."
                fi
                    if [ ! -k "$dir" ]; then
                        warn "$dir is writable by others (mode $mode) and is NOT sticky, so anyone able to
         write there can rename or remove this subtree -- including the artifact directory beneath
         it.  It is owned by uid $owner (root or you), so this run continues, but the evidence
         under it is only as protected as that directory is.  A sticky /tmp (mode 1777) or an
         ARTIFACT_ROOT under a directory you own removes the exposure."
                    fi
                ;;
            *) : ;;
        esac
        [ "$dir" != / ] || break
        dir="$(dirname -- "$dir")"
    done
    return 0
}

# Held for the whole run on a lock file inside the artifact directory.  Non-blocking: a second
# concurrent run is a mistake to report, not something to queue behind, because both would be
# writing the same fixed artifact names and the gate would read whichever won.
# The descriptor is allocated by bash rather than hard-coded, because a literal number is a
# number this script does not own: fd 9 is free today and would be silently clobbered the moment
# anything else in the run wanted it, releasing the lock without a word.  `{LOCK_FD}>>` asks bash
# for a free descriptor (>= 10) and records which one it got; it stays open, and the lock stays
# held, until this shell exits.
#
# SECOND LOCK, DIFFERENT RESOURCE, SAME FAIL-CLOSED RULE.  The tree lock excludes two runs
# operating on this working tree; this one excludes two runs writing the same artifact
# directory, which is a different pair -- one tree can be measured into two output directories,
# and one output directory can be targeted from two trees.  Both are needed and neither
# subsumes the other.
#
# Missing flock is FATAL here for the same reason it is fatal there: the alternative is a
# warning followed by a verdict, and a verdict produced without exclusivity over the files it
# read is not evidence.  The identity checks are lighter than the tree lock's because
# OUTPUT_DIR has already been proved to be a directory owned by this user at mode 0700
# (assert_owned_and_private + restrict_artifact_dir_to_owner), so no other account can create
# or replace an entry inside it; what remains worth checking is that the name this run opened
# is the file the descriptor holds.
LOCK_FD=''
LOCK_IDENTITY=''
acquire_output_lock() {
    local lock="$OUTPUT_DIR/.run.lock" identity_before identity_open

    [ -n "$FLOCK_BIN" ] || die "flock was not found on PATH, so this run cannot prove it is the
       only one writing
       $OUTPUT_DIR
       Two runs write the same fixed artifact names there, and the gate reads whichever
       finished last -- which is a verdict about a mixture of two runs.  flock ships with
       util-linux.  (This is fatal rather than advisory: a warning would still let the run
       reach a verdict.)"

    [ ! -L "$lock" ] || die "refusing to lock through a symlink: $lock"
    exec {LOCK_FD}>>"$lock" || die "could not open the run lock: $lock"

    identity_before="$(path_identity "$lock")"
    identity_open="$(open_fd_identity "$LOCK_FD")"
    [ "$identity_open" = "$identity_before" ] || die "the run lock file was replaced between the
       open and the check: $lock
       checked $identity_before, descriptor refers to $identity_open (device:inode)."

    "$FLOCK_BIN" -n "$LOCK_FD" || die "another coverage run holds the lock on $OUTPUT_DIR.
       Two runs would write the same artifact names ($(basename -- "$RAW_TRACE"),
       $(basename -- "$FILTERED_TRACE")) and the gate would read whichever finished last.
       Wait for it, or use a different --output-dir."

    LOCK_IDENTITY="$identity_before"
    log "holding the exclusive run lock on $OUTPUT_DIR [$identity_before]"
}

prepare_output_dir() {
    if [ "$OUTPUT_DIR_EXPLICIT" -eq 0 ]; then
        # NO NAME WAS CHOSEN, so mint one that could not have been.  mktemp -d creates the
        # directory and the name in one atomic step, which is the property a pre-creation
        # attack needs and cannot get: there is no window in which the name exists but the
        # directory does not, and nothing to guess beforehand.
        # THE PARENT IS CHOSEN, NOT DEFAULTED.  select_safe_temp_parent tries TMPDIR, then
        # XDG_RUNTIME_DIR, then HOME, and holds each to the same rules that guard a named
        # --output-dir; it collapses the candidate lexically before checking it, so a TMPDIR
        # of /tmp/x/../../../etc is rejected for where it actually lands rather than for how
        # it is spelled.  An unconditional fall back to /tmp would put this
        # run's evidence below a directory measured at mode 2777 on this host.
        local parent
        select_safe_temp_parent
        parent="$RUNNER_TEMP_PARENT"
        assert_artifact_location_plausible "$parent/hdmicec-l1-coverage" \
            "\$${RUNNER_TEMP_PARENT_SOURCE} (chosen automatically for this run's private directories)"
        assert_safe_ancestry "$parent/hdmicec-l1-coverage" minted
        OUTPUT_DIR="$("$MKTEMP_BIN" -d "$parent/hdmicec-l1-coverage.XXXXXXXX")" \
            || die "could not create an artifact directory under $parent.
       Pass --output-dir DIR to write the artifacts somewhere else."
        chmod 700 -- "$OUTPUT_DIR" || die "could not restrict the artifact directory to mode 0700: $OUTPUT_DIR"
        assert_private_dir "$OUTPUT_DIR"
        log "artifact root minted for this run (mktemp -d, mode 0700): $OUTPUT_DIR"
    else
        # Absolutise before anything else, so later `cd` calls cannot re-root a relative path.
        case "$OUTPUT_DIR" in
            /*) ;;
            *)  OUTPUT_DIR="$(pwd -P)/$OUTPUT_DIR" ;;
        esac
        # Then COLLAPSE it, and only then decide whether the place it names is acceptable.
        # Both steps precede create_safe_dir deliberately: that function CREATES what is
        # missing, so anything it is handed has already been created by the time a later
        # check could object.  The collapsed form is printed when it differs from what the
        # caller typed, because a path that quietly means somewhere else is the whole
        # defect being closed here and a reader must be able to see it happen.
        local named_output_dir="$OUTPUT_DIR"
        OUTPUT_DIR="$(canonicalise_path_lexically "$OUTPUT_DIR")"
        if [ "$OUTPUT_DIR" != "$named_output_dir" ]; then
            log "the output directory you named collapses to: $OUTPUT_DIR"
            log "  as given: $named_output_dir"
        fi
        assert_artifact_location_plausible "$OUTPUT_DIR" "--output-dir / COVERAGE_OUTPUT_DIR"
        # The WHOLE chain, not just the leaf: the leaf can be an ordinary directory while its
        # parent is the substituted one.  create_safe_dir validates what exists, creates what
        # does not at 0700 one component at a time, and re-validates each component afterwards.
        create_safe_dir "$OUTPUT_DIR"
    fi
    [ -w "$OUTPUT_DIR" ] || die "the output directory is not writable: $OUTPUT_DIR"
    # Safe to resolve symlinks now: the walk above proved there are none to resolve.
    OUTPUT_DIR="$(cd -- "$OUTPUT_DIR" && pwd -P)"
    assert_owned_and_private "$OUTPUT_DIR"
    assert_ancestors_safe "$OUTPUT_DIR"
    # Applied to BOTH branches, after ownership has been established: the minted branch is
    # already 0700 so this is a no-op there, and a caller-supplied directory is brought to
    # the same posture instead of being trusted as found.  See the function's own comment.
    restrict_artifact_dir_to_owner "$OUTPUT_DIR"

    # RECORD THE DIRECTORY'S IDENTITY, once, now that it is fully resolved and its posture is
    # settled.  Every later destructive or publishing step compares against this through
    # assert_output_dir_still_safe, so a directory re-pointed mid-run is refused rather than
    # written into.  Taken after restrict_artifact_dir_to_owner deliberately: a chmod changes
    # the mode, never the inode, so the value recorded here is the one that stays true for a
    # directory nobody substituted.
    OUTPUT_DIR_IDENTITY="$(path_identity "$OUTPUT_DIR")"
    [ -n "$OUTPUT_DIR_IDENTITY" ] || die "could not read the identity (device:inode) of the
       output directory: $OUTPUT_DIR
       Every write below it is checked against that identity, and a directory whose identity
       cannot be read is one whose substitution cannot be detected."

    RAW_TRACE="$OUTPUT_DIR/coverage.info"
    FILTERED_TRACE="$OUTPUT_DIR/filtered_coverage.info"
    # The line gate's own input: filtered_coverage.info with the dependency headers removed.
    # A THIRD name rather than an overwrite, so all three stages of the trace survive side by
    # side and a reader can see exactly what each step took out.
    LINE_GATE_TRACE="$OUTPUT_DIR/line_gate_coverage.info"
    HTML_DIR="$OUTPUT_DIR/coverage"
    PER_FILE_TSV="$OUTPUT_DIR/per_file_coverage.tsv"
    UNCOVERED_TXT="$OUTPUT_DIR/uncovered_lines.txt"
    readonly RAW_TRACE FILTERED_TRACE LINE_GATE_TRACE HTML_DIR PER_FILE_TSV UNCOVERED_TXT

    # The status marker is NOT in the readonly artifact set above, because it is the one path
    # written more than once: replaced by rename at every phase change, from this run taking the
    # directory over to the verdict the EXIT trap records.  It IS dealt with by the purge, first
    # of everything and atomically -- see invalidate_previous_run_status -- so that there is never
    # a moment when this directory holds artifacts under another run's verdict, and never one
    # where it holds them with no marker at all.
    RUN_STATUS_TXT="$OUTPUT_DIR/run_status.txt"

    assert_safe_artifact_path "$RAW_TRACE" file
    assert_safe_artifact_path "$FILTERED_TRACE" file
    assert_safe_artifact_path "$LINE_GATE_TRACE" file
    assert_safe_artifact_path "$HTML_DIR" dir
    assert_safe_artifact_path "$PER_FILE_TSV" file
    assert_safe_artifact_path "$UNCOVERED_TXT" file
    assert_safe_artifact_path "$RUN_STATUS_TXT" file
    acquire_output_lock

    log "artifacts: $OUTPUT_DIR (owner-only, locked for this run)"

    # ONE GENERATION PER DIRECTORY.  Ordered deliberately: the output lock is held (so no second
    # run is removing the same names), the directory's identity is recorded (so the removals are
    # provably inside the directory this run validated), and the marker is written IMMEDIATELY
    # afterwards so the window in which the directory is empty and unexplained is as small as a
    # single mv.  A reader who arrives during the run finds IN-PROGRESS, which is the truth.
    #
    # PUBLISHING THIS MARKER IS A PRECONDITION OF THE RUN, NOT A COURTESY, and the `die` is the
    # whole of that.  Every artifact this run is about to write -- build.log, the per-invocation
    # logs and results, the traces, the HTML tree -- is read in the light of the marker beside it,
    # so a run that cannot publish its own marker cannot honestly write any of them: the bundle
    # would be this generation's files under either nothing or, before the purge learnt to take
    # the marker first, a previous run's verdict.  Dying HERE is what bounds the damage: the
    # purge has already superseded or removed the prior marker, and not one artifact of this
    # generation has been written yet, so there is no mixture to clean up.
    purge_previous_generation
    write_run_status 'IN-PROGRESS' 'the run has started; no verdict has been reached' \
        || die "this run cannot publish its own status marker in $OUTPUT_DIR
       The authoritative marker is what ties every artifact in that directory to a run id, and
       this run is unable to write it (the warning above says why: a refused custody check, a
       temp file that could not be written, or a rename that could not complete).  Nothing of
       this run's has been written yet and nothing more will be: a bundle whose artifacts cannot
       be attributed to the run that produced them is worse than no bundle at all.
       Fix the obstruction -- lsattr for an immutable bit, getfacl for an ACL, df for a full
       filesystem -- or point --output-dir/COVERAGE_OUTPUT_DIR at a directory this run owns."

    # The one case worth a warning: artifacts placed inside the git tree, where this
    # suite's .gitignore does not cover them and an `git add -A` would pick them up.
    case "$OUTPUT_DIR" in
        "$HDMICEC_ROOT"|"$HDMICEC_ROOT"/*|"$WS"|"$WS"/*)
            warn "the output directory is inside the repository tree."
            warn "  coverage.info, filtered_coverage.info and coverage/ are NOT covered by"
            warn "  tests/L1Tests/.gitignore, and that file is out of scope for editing, so"
            warn "  these artifacts WILL show up in git status. They are build output: do not"
            warn "  commit them. The default output directory lives outside the tree precisely"
            warn "  to make that impossible."
            ;;
        *)  ;;   # outside the repository tree: the intended case, nothing to warn about
    esac
}

# ------------------------------------------------------------------------------------
# Step 1 -- CAPTURE.  `-d "$HDMICEC_ROOT"` is the workflow's `-d hdmicec` made
# cwd-independent: the capture directory is the WHOLE submodule, not just the test tree,
# because the objects for ccec/src and osal/src live beside their sources and both trees
# must be walked.  Everything the capture pulls in that is not production source is
# removed by the filter in step 2 -- capture wide, then narrow, exactly as CI does.
#
# The ignore list exists because a coverage-instrumented C++ tree reliably produces
# inconsistent-count and source-mismatch diagnostics for inlined and template code that
# are informational rather than actionable; without it lcov 2.x aborts the capture and no
# report is produced at all.  Counts remain visible in the message summary and in
# capture.log.  `category` is not in the list and must not be added.
# ------------------------------------------------------------------------------------
capture_coverage() {
    rule
    log "capturing coverage over $HDMICEC_ROOT"
    # Re-checked here, not merely at startup: this is the moment of the write.
    assert_output_dir_still_safe "$RAW_TRACE"
    rm -f -- "$RAW_TRACE"
    lcov_run -c \
        -o "$RAW_TRACE" \
        -d "$HDMICEC_ROOT" \
        "${LCOV_CONFIG_ARGS[@]}" \
        "${LCOV_RC_ARGS[@]}" \
        --ignore-errors "$LCOV_CAPTURE_IGNORE" \
        >"$OUTPUT_DIR/capture.log" 2>&1 \
        || die "lcov capture failed. Full log: $OUTPUT_DIR/capture.log
       Tail:
$(tail -n 20 -- "$OUTPUT_DIR/capture.log" 2>/dev/null | sed 's/^/         /')"

    [ -s "$RAW_TRACE" ] || die "the capture produced an empty trace ($RAW_TRACE).
       Refusing to report a coverage figure that was not measured. See
       $OUTPUT_DIR/capture.log"
    log "raw trace: $RAW_TRACE ($(grep -c '^SF:' "$RAW_TRACE" || true) source file record(s))"
}

# ------------------------------------------------------------------------------------
# ASSERT THAT EVERY PROTECTED RECORD SURVIVED A FILTERING STEP.
#
# Called after BOTH filtering steps, on the trace each one produced.  One function rather than
# two loops, so the two derived traces cannot drift apart in what they guarantee -- which is
# exactly what happened before: the line gate's trace was checked and the filtered trace, which
# is the input to that step and the trace the aggregate figure is quoted from, was not.
#
# WHY THE MATCH IS A `case` GLOB AND NOT A grep PATTERN.  The obvious spelling is
#     grep -q "^SF:.*/${protected_file}\$"
# and it is wrong in a way that only ever fails silently.  Every entry in the list contains a
# '.', which in a basic regular expression is ANY CHARACTER -- so that pattern accepts
# 'ccec/src/DriverXcpp' as proof that 'ccec/src/Driver.cpp' is present.  A trace that had lost
# the real record but held a differently-named neighbour would pass.  `case` with the variable
# QUOTED compares literally, and because `case` matches the WHOLE word, '*/'"$suffix" is
# end-anchored by construction: it cannot be satisfied by 'halcompat.h.orig' or by
# 'common/current/halcompat.hpp', and it cannot be defeated by the absolute prefix differing
# between this host, a CI runner and the QEMU guest.  The leading '*/' is what makes it a
# SUFFIX check and forbids a bare basename match at the filesystem root.
# ------------------------------------------------------------------------------------
assert_protected_records_present() { # $1=trace file  $2=human label for the trace
    local trace="$1" label="$2"
    local protected_file sf found

    for protected_file in "${PROTECTED_TRACE_FILES[@]}"; do
        found=0
        while IFS= read -r sf; do
            case "$sf" in
                */"$protected_file") found=1; break ;;
            esac
        done < <(grep '^SF:' "$trace" | sed 's/^SF://')

        [ "$found" -eq 1 ] || die "$protected_file is ABSENT from $label
       ($trace), and it must never be.  It is source this migration is answerable for, so a
       figure computed without it would be quoted over code nobody measured.  Either one of the
       exclusion globs applied to produce that trace matches it -- compare them against
           grep '^SF:' $RAW_TRACE
       -- or it was never compiled into the objects the capture walked.  If this is halcompat.h,
       note that AAP 0.6.5 requires it RETAINED as consumed HALIF source: it is not third-party
       toolchain code and must not be added to DEPENDENCY_EXCLUDES to make this message go away."
    done
    log "verified: all ${#PROTECTED_TRACE_FILES[@]} protected record(s) present in $label"
}

# ------------------------------------------------------------------------------------
# Step 2 -- FILTER.  The seven globs, verbatim, in the workflow's order, expanded from a
# quoted array so the shell cannot match them against the working directory.
#
# `unused` is tolerated here for a specific and benign reason: several globs
# ('*/install/usr/include/*', '*/googletest/*') match nothing when the dependency headers
# were consumed from a system prefix rather than an install tree.  A glob that matches
# nothing is not an error and certainly not grounds for deleting it -- keeping all seven
# is what keeps this step identical to CI's on every host.
# ------------------------------------------------------------------------------------
filter_coverage() {
    rule
    log "filtering the trace down to production source (${#LCOV_EXCLUDES[@]} exclusion globs, verbatim from the workflow)"
    local g
    for g in "${LCOV_EXCLUDES[@]}"; do
        log "    exclude: $g"
    done
    assert_output_dir_still_safe "$FILTERED_TRACE"
    rm -f -- "$FILTERED_TRACE"
    lcov_run -r "$RAW_TRACE" \
        "${LCOV_EXCLUDES[@]}" \
        -o "$FILTERED_TRACE" \
        "${LCOV_CONFIG_ARGS[@]}" \
        "${LCOV_RC_ARGS[@]}" \
        --ignore-errors "$LCOV_FILTER_IGNORE" \
        >"$OUTPUT_DIR/filter.log" 2>&1 \
        || die "lcov filtering failed. Full log: $OUTPUT_DIR/filter.log
       Tail:
$(tail -n 20 -- "$OUTPUT_DIR/filter.log" 2>/dev/null | sed 's/^/         /')"

    [ -s "$FILTERED_TRACE" ] || die "filtering produced an empty trace ($FILTERED_TRACE).
       Every file was excluded, which means the capture found no production source at all.
       Refusing to report a coverage figure that was not measured."

    local files
    files="$(grep -c '^SF:' "$FILTERED_TRACE" || true)"
    [ "${files:-0}" -gt 0 ] || die "the filtered trace contains no source-file records."
    log "filtered trace: $FILTERED_TRACE ($files production source file record(s))"

    # A filter that let a test file, mock or stub through would silently inflate the
    # denominator with code the gate is not supposed to judge.  Cheap to check, so checked.
    local leaked
    leaked="$(grep '^SF:' "$FILTERED_TRACE" \
              | grep -E '/usr/include/|/stubs/|/mocks/|/googletest/|/tests/|/test_[^/]*\.cpp$' || true)"
    if [ -n "$leaked" ]; then
        printf '%s\n' "$leaked" | sed 's/^/[run_coverage]   leaked: /' >&2
        die "the filtered trace still contains non-production paths (listed above).
       The exclusion globs are reproduced verbatim from the workflow and must not be
       changed, so this is a capture-path problem, not a glob problem: check whether the
       build tree lives somewhere the globs cannot describe."
    fi
    log "verified: the denominator is production source only"

    # The protected records, asserted on the FILTERED trace as well as on the line gate's.  This
    # is the trace the aggregate figure is quoted from and the input to the next step, so a
    # record lost HERE is lost from both consumers -- and one of the seven verbatim globs
    # matching this migration's own source is precisely the accident nobody would notice.
    assert_protected_records_present "$FILTERED_TRACE" "the filtered trace"

    # BRANCH DATA IS A DELIVERABLE, so its absence is fatal here rather than a note later.
    #
    # Branch collection is off in lcov by default and the legacy `lcov_branch_coverage` key is
    # deprecated, so a single missing --rc branch_coverage=1, an lcov 1.x on PATH that spells the
    # option differently, or a home/system configuration that wins would all produce a trace with
    # no BRDA/BRF records at all -- and every step after this one would still succeed, printing
    # "n/a" in the branch column and a report that silently answers a different question than the
    # one asked.  That is precisely the "requested but not enforced" shape, so it is checked on
    # the FILTERED trace (the one that is quoted and gated) and checked in the AGGREGATE: an
    # individual file with no branch arcs legitimately has no BRF record, which is why the test
    # is "no branch records anywhere", not "every file has one".
    local branch_records
    branch_records="$(grep -c '^BRDA:' "$FILTERED_TRACE" || true)"
    if [ "${branch_records:-0}" -eq 0 ]; then
        die "the filtered trace contains no branch records at all ($FILTERED_TRACE).
       Every lcov invocation here passes --rc branch_coverage=1, so branch data should be
       present for any file with branch arcs -- and this trace has none for ANY file, which
       means collection did not happen rather than that the code has no branches.
       Check that the lcov on PATH is 2.x (\`lcov --version\`), that no configuration file
       overrides branch_coverage, and that the objects were compiled with -fprofile-arcs
       -ftest-coverage.  Branch coverage is reported as evidence and never gated, but a report
       with no branch data would not be the report this suite is required to produce."
    fi
    local branch_files
    branch_files="$(grep -c '^BRF:' "$FILTERED_TRACE" || true)"
    log "branch data present: $branch_records BRDA record(s) across ${branch_files:-0} file record(s)"
}

# ------------------------------------------------------------------------------------
# Step 2b -- ONE TRACE, TWO CONSUMERS.  Produce the line gate's input by removing the
# dependency headers, and leave the branch gate reading the UNFILTERED capture.
#
# THE SPLIT, AND WHY IT IS NOT AN INCONSISTENCY.  Two gates now read two different traces, and
# that is deliberate because they are asking two different questions:
#
#   THE BRANCH GATE READS THE UNFILTERED CAPTURE ($RAW_TRACE).  Some of the branches SC6 asks
#   for by name live inside halcompat.h's isCompatible<>() -- the empty-hash arm, the "-1" arm,
#   the "notfrozen" arm, the era and major comparisons -- and some of the arcs it needs are in
#   header-only code.  Filtering first would delete exactly the evidence the gate exists to
#   check, and a gate that cannot see its own subject is worse than no gate: it would pass
#   silently and for ever.
#
#   THE LINE GATE READS THIS DERIVATIVE.  Its threshold is 80% over an aggregate, and an
#   aggregate is only meaningful against a stable denominator.  Admitting six binder SDK
#   headers and four generated stub headers would change the file set, change the denominator,
#   make the figure incomparable with the recorded baseline, and -- because per-file gating is
#   on -- start failing the run over how well this suite covers a third-party header it neither
#   owns nor tests.  So the dependency headers come out, and only they.
#
# WHY NOT SIMPLY GATE BOTH ON THE SAME TRACE.  Both single-trace answers are worse.  Gating
# lines on the unfiltered capture imports the denominator problem above.  Gating branches on
# the filtered copy deletes halcompat.h's arcs.  Two consumers of one capture, each reading the
# form that answers its own question, is the only arrangement in which both gates mean
# something -- and the capture is still taken ONCE, so the two are views of the same evidence
# rather than two measurements that could disagree.
# ------------------------------------------------------------------------------------
filter_dependency_headers() {
    rule
    log "producing the line gate's trace: removing dependency headers from the filtered trace"

    # The fixed globs, plus whatever the caller's staging prefixes tell us about this host.  A
    # Yocto-style layout can put the binder headers somewhere that no fixed glob describes, and
    # the prefixes are the only thing that knows where.  Derived here rather than at definition
    # time because they depend on values parse_args may still have been changing.
    local -a excludes=("${DEPENDENCY_EXCLUDES[@]}")
    local root
    for root in "$BINDER_SDK_INCLUDE_DIR" "$BINDER_SDK_DIR"; do
        [ -n "$root" ] || continue
        excludes+=("$root/*")
    done

    local g
    for g in "${excludes[@]}"; do
        log "    exclude: $g"
    done
    log "    RETAINED: halcompat.h -- a consumed HALIF header, and the branches SC6 names live in it"

    assert_output_dir_still_safe "$LINE_GATE_TRACE"
    rm -f -- "$LINE_GATE_TRACE"
    lcov_run -r "$FILTERED_TRACE" \
        "${excludes[@]}" \
        -o "$LINE_GATE_TRACE" \
        "${LCOV_CONFIG_ARGS[@]}" \
        "${LCOV_RC_ARGS[@]}" \
        --ignore-errors "$LCOV_FILTER_IGNORE" \
        >"$OUTPUT_DIR/filter_dependencies.log" 2>&1 \
        || die "removing the dependency headers failed. Full log: $OUTPUT_DIR/filter_dependencies.log
       Tail:
$(tail -n 20 -- "$OUTPUT_DIR/filter_dependencies.log" 2>/dev/null | sed 's/^/         /')"

    [ -s "$LINE_GATE_TRACE" ] || die "removing the dependency headers produced an empty trace
       ($LINE_GATE_TRACE).  One of the globs above must be matching this submodule's own source;
       compare them against the file list in $FILTERED_TRACE before changing anything else."

    # The protected records, asserted on the trace the LINE GATE reads.  This is the step whose
    # exclusions could plausibly take halcompat.h out -- it is the step that removes dependency
    # headers, and halcompat.h is a dependency header that must nevertheless stay.
    assert_protected_records_present "$LINE_GATE_TRACE" "the line gate's trace"

    # THE FILE SET IS ENUMERATED, NOT SUMMARISED.  What came out and what stayed are both
    # printed, because a policy about which dependency headers belong in a denominator is only
    # auditable if a reader can see the decision applied to real paths.
    local before after removed
    before="$(grep -c '^SF:' "$FILTERED_TRACE" || true)"
    after="$(grep -c '^SF:' "$LINE_GATE_TRACE" || true)"
    removed=$((before - after))
    log "line gate trace: $LINE_GATE_TRACE ($after file record(s); $removed dependency header(s) removed)"

    if [ "$removed" -gt 0 ]; then
        log "  removed:"
        # comm needs sorted input; LC_ALL=C so the ordering is the same on every host.
        LC_ALL=C comm -23 \
            <(grep '^SF:' "$FILTERED_TRACE"   | sed 's/^SF://' | LC_ALL=C sort) \
            <(grep '^SF:' "$LINE_GATE_TRACE" | sed 's/^SF://' | LC_ALL=C sort) \
            | sed 's/^/[run_coverage]     /'
    fi

    # Any surviving record from outside the submodule is a NEWLY APPEARING dependency header
    # that this policy has not classified.  It is reported rather than filtered, because
    # silently widening a filter is how a denominator drifts: the reader decides whether it is
    # consumed HALIF source that belongs in the gate, like halcompat.h, or toolchain code that
    # does not.
    local unclassified
    unclassified="$(grep '^SF:' "$LINE_GATE_TRACE" | sed 's/^SF://' \
                    | grep -v "^$HDMICEC_ROOT/" | grep -v '/halcompat\.h$' || true)"
    if [ -n "$unclassified" ]; then
        warn "the line gate's trace still holds file record(s) from outside this submodule that"
        warn "  this policy has not classified:"
        printf '%s\n' "$unclassified" | sed 's/^/[run_coverage]     /' >&2
        warn "  They are IN the aggregate denominator and IN the per-file gate as things stand."
        warn "  Decide deliberately: a consumed HALIF header belongs there, as halcompat.h does;"
        warn "  third-party toolchain code does not and belongs in DEPENDENCY_EXCLUDES.  Nothing"
        warn "  is filtered automatically here, because a filter that widens itself is how a"
        warn "  coverage denominator drifts without anyone deciding that it should."
        note_advisory "the line gate's trace holds unclassified dependency header record(s) from
       outside this submodule, so its aggregate denominator is not the one the recorded baseline
       was measured over.  They are listed in this run's output; classify them in
       DEPENDENCY_EXCLUDES or accept them deliberately."
    fi

    log "  retained inside this submodule: $(grep '^SF:' "$LINE_GATE_TRACE" | sed 's/^SF://' | grep -c "^$HDMICEC_ROOT/" || true) file record(s)"
}

# ------------------------------------------------------------------------------------
# Step 3 -- HTML REPORT.  Built into a private staging directory and published by rename,
# so a smaller trace can never leave pages from a larger earlier one behind, and an
# interrupted run cannot leave a half-written report at the published path.
# ------------------------------------------------------------------------------------
generate_html() {
    rule
    if [ "$DO_HTML" -eq 0 ]; then
        log "skipping the genhtml report (--no-html)"
        return 0
    fi
    log "generating the HTML report: $HTML_DIR"
    assert_output_dir_still_safe "$HTML_DIR"
    STAGE_DIR="$("$MKTEMP_BIN" -d "$OUTPUT_DIR/.genhtml-stage.XXXXXXXX")" \
        || die "could not create a staging directory under $OUTPUT_DIR"
    chmod 700 -- "$STAGE_DIR" || die "could not restrict the staging directory to mode 0700: $STAGE_DIR"
    # The report is assembled here and then moved into place, so the staging directory is
    # held to the private standard rather than merely the ancestry one.
    assert_private_dir "$STAGE_DIR"
    # Recorded for cleanup_stage_dir's recursive remove, same reason as the lcov HOME.
    STAGE_DIR_IDENTITY="$(path_identity "$STAGE_DIR")"
    [ -n "$STAGE_DIR_IDENTITY" ] || die "could not read the identity (device:inode) of the
       HTML staging directory: $STAGE_DIR"

    genhtml_run \
        -o "$STAGE_DIR/coverage" \
        -t "$GENHTML_TITLE" \
        "$LINE_GATE_TRACE" \
        "${LCOV_CONFIG_ARGS[@]}" \
        "${LCOV_RC_ARGS[@]}" \
        --ignore-errors "$GENHTML_IGNORE" \
        >"$OUTPUT_DIR/genhtml.log" 2>&1 \
        || die "genhtml failed. Full log: $OUTPUT_DIR/genhtml.log
       Tail:
$(tail -n 20 -- "$OUTPUT_DIR/genhtml.log" 2>/dev/null | sed 's/^/         /')"

    [ -f "$STAGE_DIR/coverage/index.html" ] || die "genhtml exited 0 but wrote no index.html.
       See $OUTPUT_DIR/genhtml.log"

    # Re-checked immediately before the one recursive delete in the pipeline.
    assert_output_dir_still_safe "$HTML_DIR"
    rm -rf -- "$HTML_DIR"
    mv -- "$STAGE_DIR/coverage" "$HTML_DIR" || die "could not publish the report to $HTML_DIR"
    cleanup_stage_dir
    log "report: $HTML_DIR/index.html"

    # The Branches column only appears when branch data survived into the trace, so its
    # presence is a direct check that --rc branch_coverage=1 actually took effect.
    if grep -q 'Branches' "$HTML_DIR/index.html" 2>/dev/null; then
        log "the report includes a Branches column (branch collection confirmed active)"
    else
        warn "the report has no Branches column: branch data did not survive into the trace."
        warn "  Every lcov step here passes --rc branch_coverage=1, so investigate the"
        warn "  toolchain rather than the configuration -- lcov 1.x, for instance, spells"
        warn "  this option differently and would silently produce a branch-free report."
    fi
}

# ------------------------------------------------------------------------------------
# Step 4 -- AGGREGATE SUMMARY.  lcov's own words, printed verbatim, so the reader can see
# the authoritative figures next to the per-file table this script derives.  If the two
# ever disagree, lcov is right and the parser below is wrong.
# ------------------------------------------------------------------------------------
summarise_coverage() {
    rule
    log "aggregate coverage (production source only), as reported by lcov itself:"
    lcov_run --summary "$LINE_GATE_TRACE" \
        "${LCOV_CONFIG_ARGS[@]}" \
        "${LCOV_RC_ARGS[@]}" \
        --ignore-errors "$LCOV_SUMMARY_IGNORE" 2>&1 \
        | sed 's/^/[run_coverage]   /' \
        || die "lcov --summary failed for $LINE_GATE_TRACE"
}


# ------------------------------------------------------------------------------------
# Step 5 -- PER-FILE FIGURES.
#
# The requirement is per TARGET, and a healthy aggregate hides a below-bar file, so LINE,
# FUNCTION and BRANCH figures are reported for every file in the filtered trace.  These
# three columns are a contract, not a convenience: the workspace traceability report
# transcribes them directly for its before/after per-target table.
#
# Everything is parsed out of the trace records -- never out of `lcov --list`, which emits
# malformed rates above 100% in lcov 2.x:
#     SF:<path>                     starts a file record
#     LF:/LH:                       lines found / hit
#     FNA:<index>,<count>,<name>    one per function alias; count is ALWAYS field 2, so a
#                                   demangled name containing commas cannot corrupt it
#     FNDA:<count>,<name>           the pre-2.x spelling of the same thing, also handled
#     FNF:/FNH:                     per-file leader totals -- reported, but NOT used for
#                                   the primary column: several aliases can share one
#                                   leader, so these do not reconcile with lcov --summary
#     BRF:/BRH:                     branches found / hit; ABSENT for a file with no
#                                   branch arcs, which is why presence is tracked
#                                   separately and rendered "n/a" rather than 0.0%
#     DA:<line>,<count>             per-line hits; count 0 means the line never executed
#
# The machine-readable TSV is written first and the human table is rendered FROM it, so
# the two can never disagree.  Both are sorted by path with LC_ALL=C, so the same trace
# always produces byte-identical output.
# ------------------------------------------------------------------------------------
BELOW_BAR_FILES=''

# The three functions below embed awk programs, and every one of them is single-quoted on
# purpose: `$1`, `$4`, `$13`, `$0` and `\t` inside those programs are AWK syntax that awk
# itself evaluates against the current record.  Double-quoting them would let the shell
# substitute its own positional parameters first and hand awk a mangled program, so the
# single quotes are load-bearing and shellcheck's SC2016 ("expressions don't expand in
# single quotes") is a false positive here.  Values that genuinely must come from the
# shell are passed in properly with `-v` instead, which is why HDMICEC_ROOT and
# COVERAGE_MIN appear as `-v root=` and `-v min=` rather than being interpolated.  The
# suppressions below are placed at the specific expressions, or immediately before the one
# function that holds them, rather than on the file, so that a future quoting mistake anywhere
# else is still reported.
#
# A DIRECTIVE ATTACHES TO THE NEXT COMMAND, NOT TO THE NEXT FEW FUNCTIONS, which is why there
# is no single blanket SC2016 suppression here (the directive text is deliberately not written
# out, so that a grep for suppressions finds only real ones) covering this function and the
# three below it.  MEASURED: one placed here suppresses nothing -- shellcheck skips comments
# when it looks for what a directive annotates, so it lands on git_sha_of(), which contains no
# single-quoted awk program at all, while all seven real diagnostics go on being reported.  A
# suppression that suppresses nothing is worse than none: it reads as "this was considered"
# when it was not.
# ------------------------------------------------------------------------------------
# PROVENANCE.  Who produced these artifacts, from which tree, with which script.
#
# Coverage numbers are only evidence if they can be tied to a revision. Numbers with no
# revision are indistinguishable from numbers produced by a different checkout, a different
# runner body, or a different toolchain - and once separated from their tree they cannot be
# re-attached, because nothing in an lcov trace records where it came from.
#
# So attribution is written three ways, deliberately redundantly:
#   * provenance.txt, the full manifest, beside the traces;
#   * the superproject's short SHA appended to the genhtml title, which puts it on EVERY page
#     of the HTML report rather than in one file that can be separated from it;
#   * a header block in the per-file TSV and the uncovered-lines list, so those two remain
#     self-describing when quoted on their own - which is how they are usually read.
#
# The runner's OWN sha256 is included because a trace can outlive the script that made it. If
# the recorded hash does not match the script now on disk, the artifact was produced by a
# different runner and its acceptance decision does not transfer.
# ------------------------------------------------------------------------------------
# ------------------------------------------------------------------------------------
# AN IDENTITY THAT IS A FUNCTION OF THE CONTENT, WHICH THE COMMIT SHA ALONE IS NOT.
#
# THE DEFECT THIS REPLACES, reproduced before it was fixed.  The identity used to be
# "<sha> (<branch>)  [DIRTY: tracked files modified]" -- a FIXED string for every dirty tree at
# that commit.  Two different contents of one tracked file therefore produced byte-identical
# provenance: measured in a scratch repository, blob 702203843c04 and blob 50d9b13397f8 both
# recorded as `e8f4b3b6afa1 (master)  [DIRTY: tracked files modified]`.  Recording provenance
# beside a measurement exists to answer one question -- "were these two numbers produced from
# the same source?" -- and an identity that cannot distinguish two trees cannot answer it.  Worse
# than useless: it looks like an answer.
#
# WHY DIRTY TREES ARE NOT SIMPLY REFUSED, which was the other candidate.  This runner is
# routinely invoked from a working tree that is dirty BY CONSTRUCTION -- an in-flight change is
# the normal subject of a coverage run, and `configure` overwrites six tracked Makefiles during
# --build, so even a pristine checkout is dirty by the time the capture happens.  Refusing would
# make the tool unusable on its own use case; describing the dirt exactly costs one hash.
#
# WHAT MAKES IT CONTENT-ADDRESSED.  The digest is taken over `git diff --binary HEAD`, which is
# the complete difference between the recorded commit and what is on disk for every tracked
# path: content, mode changes, additions, deletions and gitlink movements alike.  --binary so a
# binary file contributes its bytes rather than the words "Binary files differ"; --no-ext-diff
# and --no-textconv so a configured diff driver cannot make the digest a function of anything
# but the content.  HEAD plus that digest names the tree exactly, and two runs whose lines match
# are comparable.
#
# THE SHORT FORM IS ON THE ONE-LINE IDENTITY AND THE FULL ONE IS IN THE DETAIL BLOCK.  A human
# reads this line to decide whether two results are comparable, and 64 hex characters per
# repository across nine repositories is a wall rather than evidence; 16 hex characters is 64
# bits of it, which no accident collides.  write_provenance prints the full digest and the
# per-path hashes underneath, so nothing is only available in truncated form.
# ------------------------------------------------------------------------------------
git_tracked_diff_digest() { # $1 = repository path;  prints the sha256 of the tracked diff
    local repo="$1" digest
    command -v sha256sum >/dev/null 2>&1 || { printf 'unavailable (no sha256sum)\n'; return 0; }
    # The `|| true` is inside the braces so a git failure yields an empty diff rather than
    # killing the pipeline under pipefail; an empty diff hashes to the empty-input digest, and
    # the caller only prints this for a tree git has already reported as dirty.
    # shellcheck disable=SC2016  # $1 is awk's first field, not a shell parameter
    digest="$( { git -C "$repo" diff --binary --no-ext-diff --no-textconv HEAD -- 2>/dev/null || true; } \
               | sha256sum | "$AWK_BIN" '{print $1; exit}' )" || digest=''
    printf '%s\n' "${digest:-unavailable}"
}

git_dirty_paths_of() { # $1 = repository path;  prints one tracked path per line, or nothing
    local repo="$1" path
    command -v git >/dev/null 2>&1 || return 0
    # -z so a path containing a space, a quote or a newline arrives verbatim: git's default
    # output QUOTES such paths, and a quoted path is not the path -- it cannot be hashed and it
    # cannot be looked up.  This workspace has one today (blitzy/documentation/Project Guide.md).
    while IFS= read -r -d '' path; do
        [ -n "$path" ] || continue
        printf '%s\n' "$path"
    done < <( { git -C "$repo" diff --name-only -z HEAD -- 2>/dev/null || true; } )
    return 0
}

# ------------------------------------------------------------------------------------
# A REVISION FOR A TREE THAT HAS NO .git, SUPPLIED BY WHOEVER BUILT THE PAYLOAD.
#
# THE DEFECT THIS CLOSES, observed in a harvested artifact.  Inside the QEMU guest the payload is
# a `git archive` -- source bytes and nothing else, no .git anywhere -- so every row of
# provenance.txt read `unavailable (not a repository)`, while the file's own step 1 told the
# reader to compare that row with `git rev-parse HEAD`.  An artifact whose prescribed check
# cannot be performed cannot be tied back to a commit at all, which is the one thing provenance
# exists to make possible.  The revision IS known at that moment -- the step that assembled the
# payload read it from a real checkout -- it simply had no way to travel with the payload.
#
# THE INTERFACE.  One optional environment variable per repository,
# CEC_PROVENANCE_REVISION_<LABEL>, where <LABEL> is the label provenance_repositories() emits,
# upper-cased with `-` mapped to `_`.  The name is DERIVED from the label rather than looked up in
# a list, so a repository added to that function needs no edit here and cannot be forgotten.
#
# WHY GIT ALWAYS WINS WHEN IT CAN ANSWER.  A revision read from the tree that was actually
# compiled is a measurement; a revision passed in on an environment variable is a claim, and the
# two can disagree -- a stale export in a CI job, or a value carried over from a previous stage.
# So the variable is consulted ONLY on git's three "cannot answer" outcomes (no git binary, not a
# repository, no HEAD), and where it went unused the record SAYS it went unused rather than
# leaving the reader to wonder which of the two they are looking at.
#
# WHY A MALFORMED VALUE IS FATAL RATHER THAN IGNORED.  This is the posture the project already
# takes on CEC_TEST_AIDL_MODE, where a value the harness does not implement is refused instead of
# quietly downgrading the run, and the reason is the same one: a provenance record nobody can
# trust is worse than one that honestly says "unavailable".  Silently dropping a malformed value
# would produce exactly the artifact this change exists to eliminate, and silently RECORDING one
# would produce a worse one -- a sha-shaped string that resolves to nothing.  An empty value is
# malformed too: a producing step that could not determine a revision must not export the
# variable, because "exported and empty" and "not exported" would otherwise mean the same thing
# and one of them is a bug in the producer.
#
# CHECKED HERE, AT THE DECLARATION, AND NOT WHERE IT IS USED.  Every use of a supplied revision
# is inside a `$(...)` command substitution, and `die` in a subshell ends the subshell rather
# than the run: the diagnostic would print, the empty result would be interpolated, and the run
# would carry on and publish the artifact anyway.  So the values are validated once, at the top
# level, before main() is reached and before anything is built or measured, and the validated
# pairs are pre-rendered into PROVENANCE_SUPPLIED_REVISIONS.  The use-time lookup is then a pure
# read of an already-checked string, with no failure mode of its own.
# ------------------------------------------------------------------------------------
PROVENANCE_SUPPLIED_REVISIONS=''   # "<variable name><TAB><value>" per validated supplied revision

# The environment variable name for a provenance label.  Derived, never looked up.
provenance_revision_variable_name() { # $1 = label -> the variable name
    # LC_ALL=C so the case conversion is byte-wise: a Turkish locale maps `i` to a dotted capital
    # I, which would produce a variable name no producing step could ever set.
    local LC_ALL=C
    local upper="${1//-/_}"
    printf 'CEC_PROVENANCE_REVISION_%s' "${upper^^}"
}

validate_supplied_provenance_revisions() {
    # LC_ALL=C for the same reason render_untrusted takes it: the `[!0-9a-f]` class below is a
    # range, and ranges collate by locale.  A commit sha is bytes, so it is matched as bytes.
    local LC_ALL=C
    local name value core suffix tab
    tab="$(printf '\t')"

    # ${!prefix@} lists the NAMES of the shell variables with that prefix -- exported ones
    # included, since an exported variable is a shell variable.  Measured on this host's bash:
    # with no match it expands to nothing and does not trip `set -u`, so no guard is needed.
    for name in ${!CEC_PROVENANCE_REVISION_@}; do
        value="${!name}"
        core="$value"
        suffix=''
        case "$value" in
            *-dirty) core="${value%-dirty}"; suffix='-dirty' ;;
        esac
        if [ -z "$value" ]; then
            die "$name is set but empty.
       It names the revision to record for a tree with no git metadata, and an empty value names
       nothing.  A step that could not determine a revision must leave the variable UNSET, which
       records \"unavailable\" honestly; exported-and-empty is indistinguishable from a producer
       that computed a revision and lost it, so it is refused rather than absorbed."
        fi
        if [ "${#core}" -ne 40 ]; then
            die "$name must be a 40-character lowercase hex commit sha, optionally suffixed
       -dirty, got $(render_untrusted "$value") (${#core} characters before any -dirty suffix).
       This value is written into provenance.txt as the identity of a tree that has no git
       metadata of its own, and a value that is not a commit sha resolves to nothing for whoever
       later tries to tie these numbers back to a commit.  It is refused rather than recorded,
       for the same reason CEC_TEST_AIDL_MODE refuses a mode it does not implement."
        fi
        case "$core" in
            *[!0-9a-f]*)
                die "$name must be a 40-character LOWERCASE hex commit sha, optionally suffixed
       -dirty, got $(render_untrusted "$value").
       Upper-case hex, an abbreviated sha, a tag, a branch name and a \`git describe\` string are
       all refused deliberately: provenance is compared by string equality against
       \`git rev-parse HEAD\`, and anything that is not that exact form silently fails to match
       a tree it in fact describes." ;;
        esac
        PROVENANCE_SUPPLIED_REVISIONS="${PROVENANCE_SUPPLIED_REVISIONS}${name}${tab}${core}${suffix}
"
    done
    return 0
}
validate_supplied_provenance_revisions

# The validated supplied revision for a label, or nothing.  Prints "<variable name><TAB><value>"
# so a caller can name the source as well as the value -- which is the whole point: a reader has
# to be able to tell a measured identity from a supplied one without leaving the file.
provenance_supplied_revision_for() { # $1 = label -> "<variable name><TAB><value>" or nothing
    local label="$1" want name value tab
    tab="$(printf '\t')"
    [ -n "$PROVENANCE_SUPPLIED_REVISIONS" ] || return 0
    want="$(provenance_revision_variable_name "$label")"
    while IFS="$tab" read -r name value; do
        [ -n "$name" ] || continue
        if [ "$name" = "$want" ]; then
            printf '%s%s%s' "$name" "$tab" "$value"
            return 0
        fi
    done <<< "$PROVENANCE_SUPPLIED_REVISIONS"
    return 0
}

# Whether git can answer for a path at all -- the same three conditions git_sha_of tests, in the
# same order, so the disposition block in provenance.txt cannot disagree with the rows above it.
git_can_answer_for() { # $1 = repository path
    local repo="$1"
    command -v git >/dev/null 2>&1 || return 1
    git -C "$repo" rev-parse --git-dir >/dev/null 2>&1 || return 1
    [ -n "$(git -C "$repo" rev-parse HEAD 2>/dev/null)" ] || return 1
    return 0
}

# What git_sha_of prints when git could not answer: the SUPPLIED revision if one was passed in for
# this label, and otherwise the exact string this script has always printed.
#
# THE NO-VARIABLE PATH IS BYTE-IDENTICAL TO THE PREVIOUS BEHAVIOUR, deliberately.  Those three
# strings appear in harvested artifacts, in the per-file TSV header and in this script's own log,
# and a run with nothing supplied must be indistinguishable from a run of the previous version --
# so nothing is appended to them, not even a hint that the variable exists.
#
# WHAT THE SUPPLIED FORM CARRIES, AND WHAT IT CANNOT.  It carries the sha and the name of the
# variable it came from, so "read from a repository" and "asserted by the payload builder" are
# distinguishable at a glance.  It cannot carry a branch, nor the content-addressed
# [DIRTY: N tracked path(s), content sha256 ...] detail that the block above explains is what
# stops two different dirty trees at one commit recording the same identity: both are derived from
# a repository, and by construction there is none at this path.  A `-dirty` suffix is therefore
# recorded as the coarse statement it is, explicitly labelled as coarser than the git-read form,
# rather than dressed up as the digest it is not.
# ONE LINE, ALWAYS, AND THAT IS A HARD CONSTRAINT RATHER THAN A PREFERENCE.  This string is
# stored in PROVENANCE_START_IDENTITIES as "<label><TAB><identity>" with one record per line and
# looked up from there by an awk program keyed on the label; it is also printed through
# `%-18s : %s`.  A second line in it would add a junk record to that map and misalign the report.
# The reasoning that does not fit on the line lives in the block above and in the SUPPLIED
# REVISIONS section of provenance.txt.
git_sha_unavailable() { # $1 = provenance label (may be empty)  $2 = why git could not answer
    local label="${1:-}" reason="$2" supplied name value tab because
    tab="$(printf '\t')"

    if [ -n "$label" ]; then
        supplied="$(provenance_supplied_revision_for "$label")"
        if [ -n "$supplied" ]; then
            name="${supplied%%"$tab"*}"
            value="${supplied#*"$tab"}"
            # The reason is rendered for a human here rather than reused verbatim: the three
            # tokens above are written to read inside "unavailable (...)", and the same words
            # inside a sentence do not parse.
            case "$reason" in
                'no git')           because='no git binary on PATH' ;;
                'not a repository') because='no git metadata at this path' ;;
                'no HEAD')          because='the repository at this path has no HEAD' ;;
                *)                  because="$reason" ;;
            esac
            case "$value" in
                *-dirty)
                    printf '%s  [SUPPLIED via %s -- %s; declared DIRTY by its -dirty suffix, which is coarser than a git-read digest]\n' \
                        "${value%-dirty}" "$name" "$because" ;;
                *)
                    printf '%s  [SUPPLIED via %s -- %s; declared clean]\n' \
                        "$value" "$name" "$because" ;;
            esac
            return 0
        fi
    fi
    printf 'unavailable (%s)\n' "$reason"
    return 0
}

# $2 is OPTIONAL and additive: with no label there is no variable to consult, so the function
# behaves exactly as it did before supplied revisions existed.  Every caller in this script
# passes one, because a row that could have carried an identity and did not is the defect being
# fixed rather than a caller's private choice.
git_sha_of() { # $1 = repository path;  $2 = provenance label (optional);  prints the identity
    local repo="$1" label="${2:-}" sha branch dirty='' paths count digest
    command -v git >/dev/null 2>&1 || { git_sha_unavailable "$label" 'no git'; return 0; }
    git -C "$repo" rev-parse --git-dir >/dev/null 2>&1 || { git_sha_unavailable "$label" 'not a repository'; return 0; }
    sha="$(git -C "$repo" rev-parse HEAD 2>/dev/null)" || sha=''
    [ -n "$sha" ] || { git_sha_unavailable "$label" 'no HEAD'; return 0; }
    branch="$(git -C "$repo" rev-parse --abbrev-ref HEAD 2>/dev/null)" || branch='?'
    # --porcelain over tracked paths only: untracked build residue is not a content difference
    # and must not be reported as one, or every instrumented tree would read as dirty.
    if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        paths="$(git_dirty_paths_of "$repo")"
        count="$(printf '%s' "$paths" | grep -c . || true)"
        digest="$(git_tracked_diff_digest "$repo")"
        dirty="$(printf '  [DIRTY: %s tracked path(s), content sha256 %.16s]' \
                    "${count:-?}" "$digest")"
    fi
    printf '%s (%s)%s\n' "$sha" "$branch" "$dirty"
}

sha256_of() { # $1 = file;  prints the hex digest, or a reason
    local f="$1"
    [ -f "$f" ] || { printf 'absent\n'; return 0; }
    if command -v sha256sum >/dev/null 2>&1; then
        # shellcheck disable=SC2016  # $1 is awk's first field, not a shell parameter
        sha256sum -- "$f" 2>/dev/null | "$AWK_BIN" '{print $1; exit}'
    else
        printf 'unavailable (no sha256sum)\n'
    fi
}

# ------------------------------------------------------------------------------------
# THE REPOSITORIES THIS RUN REPORTS ON, NAMED ONCE.
#
# Two consumers read this list -- the run-start capture below and write_provenance -- and a
# second copy of it is how the two would come to describe different trees.  Prints
# "<label><TAB><path>", the superproject first and then each submodule that is actually checked
# out, because a submodule directory that is not there is not evidence of anything.
# ------------------------------------------------------------------------------------
provenance_repositories() { # -> "<label>\t<path>" per line
    local sub
    printf 'superproject\t%s\n' "$WS"
    for sub in hdmicec entservices-hdmicecsource entservices-hdmicecsink entservices-testframework \
               entservices-apis entservices-helpers Thunder ThunderTools; do
        if [ -d "$WS/$sub" ]; then
            printf '%s\t%s\n' "$sub" "$WS/$sub"
        fi
    done
    return 0
}

# ------------------------------------------------------------------------------------
# THE IDENTITY OF THE TREE AS IT STOOD WHEN THIS RUN STARTED, captured BEFORE the build.
#
# WHY IT CANNOT BE LEFT TO write_provenance.  That function runs after --build and after the
# matrix, and --build's `configure` OVERWRITES six git-tracked Makefiles: the identity read at
# that point describes a tree the caller never had, and its diff digest changes from run to run
# for reasons that have nothing to do with what was measured.  So the identity is read here,
# before do_build, and the reading at write time is kept as well -- the pair is what makes a
# mid-run rewrite VISIBLE rather than absorbed into one number.
#
# TWO GLOBALS, and each has one job.  PROVENANCE_START_DETAIL is pre-rendered text for
# provenance.txt to print, and PROVENANCE_START_IDENTITIES is a "<label><TAB><identity>" map for
# the write-time comparison.  Rendering at capture time is deliberate: the tree it describes no
# longer exists by the time the file is written, so anything not captured now cannot be
# recovered later.
# ------------------------------------------------------------------------------------
PROVENANCE_START_DETAIL=''
PROVENANCE_START_IDENTITIES=''
capture_start_provenance() {
    local tab label path identity paths one hash
    tab="$(printf '\t')"

    while IFS="$tab" read -r label path; do
        [ -n "$label" ] || continue
        identity="$(git_sha_of "$path" "$label")"
        PROVENANCE_START_IDENTITIES="${PROVENANCE_START_IDENTITIES}${label}${tab}${identity}
"
        PROVENANCE_START_DETAIL="${PROVENANCE_START_DETAIL}$(printf '  %-18s : %s' "$label" "$identity")
"
        paths="$(git_dirty_paths_of "$path")"
        [ -n "$paths" ] || continue

        # THE PER-PATH HASHES ARE WHAT MAKE THE LINE ACTIONABLE.  The diff digest says two trees
        # differ; these say WHERE, and they are read from the working tree rather than from git,
        # because the working tree is what was compiled.
        PROVENANCE_START_DETAIL="${PROVENANCE_START_DETAIL}$(printf '      tracked diff sha256 : %s' \
            "$(git_tracked_diff_digest "$path")")
"
        while IFS= read -r one; do
            [ -n "$one" ] || continue
            if [ -d "$path/$one" ]; then
                # A gitlink: its content is the submodule's own row, so hashing the directory
                # would be both impossible and the wrong answer.
                hash='(gitlink or directory -- its content is its own row above or below)'
            elif [ -f "$path/$one" ]; then
                hash="sha256 $(sha256_of "$path/$one")"
            else
                hash='(deleted from the working tree)'
            fi
            PROVENANCE_START_DETAIL="${PROVENANCE_START_DETAIL}$(printf '      %-58s %s' "$one" "$hash")
"
        done <<< "$paths"
    done <<< "$(provenance_repositories)"

    log "tree identity captured before the build: content-addressed, one line per repository,"
    log "  with the dirty paths and their hashes (written to provenance.txt)"
}

write_provenance() {
    PROVENANCE_TXT="$OUTPUT_DIR/provenance.txt"
    assert_output_dir_still_safe "$PROVENANCE_TXT"

    # THE TITLE CARRIES THE REVISION ONTO EVERY HTML PAGE, which is why the supplied value is
    # consulted here too.  A guest-produced report titled `revision-unavailable` on all of its
    # pages is the same defect as the provenance row that could not name a commit -- the title is
    # in fact the copy most likely to be read, because it cannot be separated from the report.
    # `(supplied)` is on it so a reader is never left thinking a page was titled from a checkout.
    local super short supplied title_tab
    super="$WS"
    title_tab="$(printf '\t')"
    short="$(git -C "$super" rev-parse --short=12 HEAD 2>/dev/null)" || short=''
    if [ -n "$short" ]; then
        GENHTML_TITLE="$GENHTML_TITLE @ $short"
    else
        supplied="$(provenance_supplied_revision_for superproject)"
        if [ -n "$supplied" ]; then
            # "<variable name><TAB><value>": the value, abbreviated to the same 12 characters
            # `git rev-parse --short=12` would have produced, with any -dirty suffix kept because
            # a dirty tree's report must not be titled as though it came from a clean commit.
            supplied="${supplied#*"$title_tab"}"
            case "$supplied" in
                *-dirty) GENHTML_TITLE="$GENHTML_TITLE @ ${supplied:0:12}-dirty (supplied)" ;;
                *)       GENHTML_TITLE="$GENHTML_TITLE @ ${supplied:0:12} (supplied)" ;;
            esac
        else
            GENHTML_TITLE="$GENHTML_TITLE @ revision-unavailable"
        fi
    fi

    {
        printf 'HDMI-CEC middleware L1 coverage -- ARTIFACT PROVENANCE\n'
        printf '=====================================================\n\n'
        printf 'Run id               : %s\n' "$RUN_ID"
        printf 'Generated (UTC)      : %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || printf 'unavailable')"
        printf 'Host                 : %s\n' "$(uname -n 2>/dev/null || printf 'unavailable')"
        printf 'Clone index          : %s\n' "${CLONE_INDEX:-unset}"
        printf 'Workspace root       : %s\n' "$WS"
        printf 'Submodule root       : %s\n' "$HDMICEC_ROOT"
        printf 'Output directory     : %s\n' "$OUTPUT_DIR"
        # ------------------------------------------------------------------------------
        # TWO READINGS OF THE SAME REPOSITORIES, and the pair is the point.
        #
        # THE FIRST is the tree as it stood when this run started, captured by
        # capture_start_provenance before --build ran.  That is the tree whose source was
        # compiled and whose behaviour was measured, and it is the identity another run has to
        # match for two results to be comparable.
        #
        # THE SECOND is read now.  It differs from the first whenever something changed the tree
        # during the run -- and one thing reliably does: `configure` overwrites six git-tracked
        # Makefiles, one of them hand-written build source (see THE REGENERATED MAKEFILES).  A
        # single reading taken at this point would fold that rewrite into the identity and report
        # a tree the caller never had; two readings say plainly that it happened and where.
        # ------------------------------------------------------------------------------
        printf '\nREVISIONS -- THE TREE AS IT STOOD WHEN THIS RUN STARTED, BEFORE ANY BUILD\n'
        printf '%s\n' "  (content-addressed: HEAD, branch, and a sha256 over the tracked diff, so two"
        printf '%s\n' "   different dirty trees at one commit cannot record the same identity)"
        if [ -n "$PROVENANCE_START_DETAIL" ]; then
            printf '%s' "$PROVENANCE_START_DETAIL"
        else
            printf '%s\n' "  not captured: this invocation reached the report without passing through"
            printf '%s\n' "  capture_start_provenance, so no pre-build identity exists.  Treat the"
            printf '%s\n' "  reading below as the only one, and note that it is post-build."
        fi

        printf '\nREVISIONS -- RE-READ NOW, AS THIS FILE IS WRITTEN\n'
        local tab label path start_identity now_identity changed=0
        tab="$(printf '\t')"
        while IFS="$tab" read -r label path; do
            [ -n "$label" ] || continue
            now_identity="$(git_sha_of "$path" "$label")"
            start_identity="$(printf '%s' "$PROVENANCE_START_IDENTITIES" \
                              | "$AWK_BIN" -F"$tab" -v want="$label" \
                                    '$1 == want { sub(/^[^\t]*\t/, ""); print; exit }')"
            if [ -z "$start_identity" ]; then
                printf '  %-18s : %s   (no pre-build reading to compare with)\n' "$label" "$now_identity"
            elif [ "$start_identity" = "$now_identity" ]; then
                printf '  %-18s : unchanged since this run started\n' "$label"
            else
                changed=$((changed + 1))
                printf '  %-18s : CHANGED DURING THIS RUN\n' "$label"
                printf '      at start : %s\n' "$start_identity"
                printf '      now      : %s\n' "$now_identity"
            fi
        done <<< "$(provenance_repositories)"
        if [ "$changed" -ne 0 ]; then
            printf '%s\n' "  $changed repository/repositories changed while this run was in progress.  The"
            printf '%s\n' "  expected cause is configure overwriting the six tracked Makefiles during"
            printf '%s\n' "  --build, which this run restores from its own snapshot on exit; anything else"
            printf '%s\n' "  means the tree was edited under a measurement, and the numbers in this bundle"
            printf '%s\n' "  belong to the identity recorded ABOVE, not to this one."
        fi

        # ------------------------------------------------------------------------------
        # THE DISPOSITION OF EVERY SUPPLIED REVISION, INCLUDING THE ONES THAT WENT UNUSED.
        #
        # A supplied revision that git overrode leaves no trace on the rows above -- correctly,
        # since those rows report what was measured -- and a reader who knows a value was passed
        # in has no way to tell whether it was believed, disagreed with, or never looked at.  This
        # block answers that, and it is emitted only when something was supplied, so a run with
        # nothing supplied produces the file it always produced.
        #
        # A DISAGREEMENT IS NAMED, NOT SMOOTHED OVER.  Where git answered and the supplied value
        # names a different commit, that is either a stale export or a payload built from a tree
        # other than the one measured here, and both are worth a reader's attention: the rows
        # above are the measurement and remain authoritative, and this block says what else was
        # claimed.  A supplied label with no repository in this run is reported too, because a
        # producing step that exports a revision for a tree that is not here has a defect of its
        # own that nothing else in this bundle would reveal.
        # ------------------------------------------------------------------------------
        if [ -n "$PROVENANCE_SUPPLIED_REVISIONS" ]; then
            printf '\nSUPPLIED REVISIONS -- PASSED IN ON THE ENVIRONMENT, AND WHAT BECAME OF EACH\n'
            printf '%s\n' "  (CEC_PROVENANCE_REVISION_<LABEL>.  Consulted ONLY where git cannot answer for"
            printf '%s\n' "   that path -- no git, no repository, no HEAD -- which is the case inside a"
            printf '%s\n' "   payload assembled with 'git archive'.  A supplied value names a commit and"
            printf '%s\n' "   nothing more: no branch, and none of the content-addressed [DIRTY: ...] detail,"
            printf '%s\n' "   both of which can only be derived from a repository.)"
            local sup_name sup_value sup_repo_label sup_repo_path sup_git_head sup_matched
            while IFS="$tab" read -r sup_name sup_value; do
                [ -n "$sup_name" ] || continue
                sup_matched=''
                while IFS="$tab" read -r sup_repo_label sup_repo_path; do
                    [ -n "$sup_repo_label" ] || continue
                    [ "$(provenance_revision_variable_name "$sup_repo_label")" = "$sup_name" ] || continue
                    sup_matched="$sup_repo_label"
                    if git_can_answer_for "$sup_repo_path"; then
                        sup_git_head="$(git -C "$sup_repo_path" rev-parse HEAD 2>/dev/null)" || sup_git_head=''
                        if [ "$sup_git_head" = "${sup_value%-dirty}" ]; then
                            printf '  %-40s NOT USED: git answered for %s, and agrees\n' \
                                "$sup_name" "$sup_repo_label"
                        else
                            printf '  %-40s NOT USED: git answered for %s and DISAGREES\n' \
                                "$sup_name" "$sup_repo_label"
                            printf '      supplied : %s\n' "$sup_value"
                            printf '      git HEAD : %s\n' "${sup_git_head:-unreadable}"
                        fi
                    else
                        printf '  %-40s USED as the identity of %s: %s\n' \
                            "$sup_name" "$sup_repo_label" "$sup_value"
                    fi
                    break
                done <<< "$(provenance_repositories)"
                if [ -z "$sup_matched" ]; then
                    printf '  %-40s SET, but no repository with that label is part of this run\n' "$sup_name"
                    printf '      value    : %s\n' "$sup_value"
                fi
            done <<< "$PROVENANCE_SUPPLIED_REVISIONS"
        fi
        printf '\nRUNNER AND CONFIGURATION\n'
        printf '  runner path        : %s\n' "$SCRIPT_PATH"
        printf '  runner sha256      : %s\n' "$(sha256_of "$SCRIPT_PATH")"
        printf '  runner bytes       : %s\n' "$(wc -c <"$SCRIPT_PATH" 2>/dev/null | tr -d ' ' || printf 'unavailable')"
        printf '  lcov config        : %s\n' "$SCRIPT_DIR/.lcovrc_l1"
        printf '  lcov config sha256 : %s\n' "$(sha256_of "$SCRIPT_DIR/.lcovrc_l1")"
        printf '  line bar           : %s%%  (per-file gating: %s)\n' \
            "$COVERAGE_MIN" "$( [ "$COVERAGE_PER_FILE_GATE" -eq 1 ] && printf on || printf off )"
        printf '  branch data        : forced on (--rc branch_coverage=1)\n'
        printf '\nTOOLCHAIN\n'
        printf '  lcov               : %s\n' "$(lcov_run --version 2>/dev/null | head -n1 || printf 'unavailable')"
        printf '  gcov               : %s\n' "$(gcov --version 2>/dev/null | head -n1 || printf 'unavailable')"
        printf '  compiler           : %s\n' "$("${CXX:-g++}" --version 2>/dev/null | head -n1 || printf 'unavailable')"
        # ------------------------------------------------------------------------------
        # THE INSTRUCTION HAS TO BE PERFORMABLE IN THE PLACE THE FILE IS READ, which is what it
        # was not.  It said "compare the revision above with `git rev-parse HEAD`" -- and where
        # this bundle is harvested from, a payload assembled with `git archive`, there is no .git
        # for that command to read, so the one check the artifact prescribed for tying itself back
        # to a commit could not be carried out at all.  Both cases are now spelled out, and which
        # one applies is written on the row itself: a row marked SUPPLIED came from the payload
        # builder, any other row was read from a repository.
        # ------------------------------------------------------------------------------
        printf '\nHOW TO CHECK THIS ARTIFACT STILL APPLIES\n'
        # shellcheck disable=SC2016  # literal backticks: these lines QUOTE commands to type
        printf '  1. Compare the superproject revision above with the tree you mean to trust.\n'
        # shellcheck disable=SC2016  # literal backticks, same reason as the line above
        printf '     In a checkout: `git rev-parse HEAD`, run at the workspace root.\n'
        printf '     Where the row is marked SUPPLIED there is no repository to ask, by\n'
        printf '     construction -- compare it instead with the revision the step that assembled\n'
        printf '     this payload recorded, which is the value it passed in\n'
        printf '     CEC_PROVENANCE_REVISION_SUPERPROJECT and the same string printed above.\n'
        printf '     A row reading "unavailable" carries no revision at all: neither git nor the\n'
        printf '     builder supplied one, and these numbers cannot be tied to a commit.\n'
        # shellcheck disable=SC2016  # literal backticks, same reason as the lines above
        printf '  2. Compare the runner sha256 above with `sha256sum %s`.\n' "$SCRIPT_PATH"
        printf '  If either differs, these numbers were produced from a different tree or a\n'
        printf '  different script, and the acceptance decision they carry does not transfer.\n'
    } >"$PROVENANCE_TXT" || die "could not write $PROVENANCE_TXT"

    chmod 600 -- "$PROVENANCE_TXT" 2>/dev/null || true
    log "provenance: $PROVENANCE_TXT"
    log "  superproject : $(git_sha_of "$WS" superproject)"
    log "  runner sha256: $(sha256_of "$SCRIPT_PATH")"
}

# The directive is on the FUNCTION rather than on the two expressions, and that is forced
# rather than lax: the second awk program is the third stage of a backslash-continued pipeline
# ('... ' "$LINE_GATE_TRACE" \ | sort \ | "$AWK_BIN" ...), and a comment cannot be inserted
# between two segments of one continued command.  Function scope is still narrow -- it is the
# same form used for write_uncovered_lines and per_file_report below -- and a quoting mistake
# in any other function is still reported.
# shellcheck disable=SC2016  # both single-quoted blocks are awk programs; see the note above
write_per_file_tsv() {
    local tmp_tsv="$OUTPUT_DIR/.per_file_coverage.tsv.tmp"
    assert_output_dir_still_safe "$PER_FILE_TSV"
    rm -f -- "$tmp_tsv"

    {
        printf '# Per-file coverage for the HDMI-CEC middleware L1 suite, derived from\n'
        printf '# %s\n' "$LINE_GATE_TRACE"
        printf '# PROVENANCE  superproject %s\n' "$(git_sha_of "$WS" superproject)"
        printf '# PROVENANCE  hdmicec      %s\n' "$(git_sha_of "$HDMICEC_ROOT" hdmicec)"
        printf '# PROVENANCE  runner       %s  sha256 %s\n' "$SCRIPT_PATH" "$(sha256_of "$SCRIPT_PATH")"
        printf '# PROVENANCE  generated    %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || printf unavailable)"
        printf '# Full manifest: %s\n' "${PROVENANCE_TXT:-provenance.txt}"
        printf '# Paths are relative to %s. functions_* come from the trace FNA/FNDA alias\n' "$HDMICEC_ROOT"
        printf '# records, which reconcile exactly with lcov --summary; func_leader_* are the\n'
        printf '# FNF:/FNH: per-file leader totals, which do not (aliases can share a leader).\n'
        printf '# branches_* are "n/a" for a file with no branch arcs. The final TOTAL row is an\n'
        printf '# aggregate, not a file: filter it with  grep -v "^TOTAL"\n'
        printf 'file\tlines_hit\tlines_found\tlines_pct\tfunctions_hit\tfunctions_found\tfunctions_pct\tfunc_leader_hit\tfunc_leader_found\tbranches_hit\tbranches_found\tbranches_pct\tline_gate\n'

        "$AWK_BIN" -v root="$HDMICEC_ROOT/" '
            function flush_record() {
                if (sf == "") return
                rel = sf
                if (index(rel, root) == 1) rel = substr(rel, length(root) + 1)
                printf "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", \
                       rel, lh, lf, fnah, fna, fnh, fnf, brh, brf, hasbr
                sf = ""
            }
            /^SF:/  { flush_record()
                      sf = substr($0, 4)
                      lh = 0; lf = 0; fnh = 0; fnf = 0
                      fnah = 0; fna = 0; brh = 0; brf = 0; hasbr = 0
                      next }
            /^LF:/  { lf  = substr($0, 4) + 0; next }
            /^LH:/  { lh  = substr($0, 4) + 0; next }
            /^FNF:/ { fnf = substr($0, 5) + 0; next }
            /^FNH:/ { fnh = substr($0, 5) + 0; next }
            /^BRF:/ { brf = substr($0, 5) + 0; hasbr = 1; next }
            /^BRH:/ { brh = substr($0, 5) + 0; next }
            # lcov 2.x alias record: FNA:<index>,<execution count>,<name>.
            /^FNA:/ { split(substr($0, 5), f, ",")
                      fna++
                      if (f[2] + 0 > 0) fnah++
                      next }
            # Pre-2.x equivalents, so this parser is not silently wrong on an older trace.
            # FN:<line>,<name> declares an alias; FNDA:<count>,<name> gives its hit count.
            /^FN:/   { fna++;  next }
            /^FNDA:/ { split(substr($0, 6), f, ",")
                       if (f[1] + 0 > 0) fnah++
                       next }
            /^end_of_record/ { flush_record(); next }
            END { flush_record() }
        ' "$LINE_GATE_TRACE" \
        | LC_ALL=C sort -t "$(printf '\t')" -k1,1 \
        | "$AWK_BIN" -F'\t' -v min="$COVERAGE_MIN" '
            function pct(hit, found) { return found > 0 ? 100 * hit / found : 0 }
            {
                rel = $1; lh = $2 + 0; lf = $3 + 0
                fnah = $4 + 0; fna = $5 + 0; fnh = $6 + 0; fnf = $7 + 0
                brh = $8 + 0; brf = $9 + 0; hasbr = $10 + 0

                # Verdict from the EXACT ratio, never from the rounded display value, so a
                # file at 79.95% is never let through by a cell that reads "80.0%".
                if (lf == 0)                     gate = "no-lines"
                else if (pct(lh, lf) >= min + 0) gate = "PASS"
                else                             gate = "BELOW"

                printf "%s\t%d\t%d\t%.1f\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%s\t%s\n", \
                       rel, lh, lf, pct(lh, lf), fnah, fna, \
                       (fna > 0 ? sprintf("%.1f", pct(fnah, fna)) : "n/a"), \
                       fnh, fnf, brh, brf, \
                       (hasbr && brf > 0 ? sprintf("%.1f", pct(brh, brf)) : "n/a"), \
                       gate

                TLH += lh; TLF += lf; TFNAH += fnah; TFNA += fna
                TFNH += fnh; TFNF += fnf; TBRH += brh; TBRF += brf
                files++
            }
            END {
                if (files == 0) { print "##NODATA"; exit 0 }
                printf "TOTAL\t%d\t%d\t%.1f\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%s\t%s\n", \
                       TLH, TLF, pct(TLH, TLF), TFNAH, TFNA, \
                       (TFNA > 0 ? sprintf("%.1f", pct(TFNAH, TFNA)) : "n/a"), \
                       TFNH, TFNF, TBRH, TBRF, \
                       (TBRF > 0 ? sprintf("%.1f", pct(TBRH, TBRF)) : "n/a"), \
                       (pct(TLH, TLF) >= min + 0 ? "PASS" : "BELOW")
            }
        '
    } >"$tmp_tsv" || { rm -f -- "$tmp_tsv"; die "per-file parsing of $LINE_GATE_TRACE failed"; }

    if grep -q '^##NODATA$' "$tmp_tsv"; then
        rm -f -- "$tmp_tsv"
        die "the filtered trace yielded no per-file records.
       Refusing to report a coverage figure that was not measured."
    fi
    mv -f -- "$tmp_tsv" "$PER_FILE_TSV" || die "could not write $PER_FILE_TSV"
}

# ------------------------------------------------------------------------------------
# The uncovered-line enumeration.  This is what turns "below the bar" into something
# actionable and is exactly what the "enumerate the specific uncoverable lines and the
# reason" requirement needs: the line numbers come from the trace, and the reason is a
# human judgement to be recorded against them in the traceability report.  Written for
# every file that has at least one uncovered line, not just the below-bar ones, so the
# report can attribute the residue in an already-passing file too.
# ------------------------------------------------------------------------------------
# shellcheck disable=SC2016  # single-quoted awk programs; see the note above write_per_file_tsv.
write_uncovered_lines() {
    local tmp="$OUTPUT_DIR/.uncovered_lines.txt.tmp"
    assert_output_dir_still_safe "$UNCOVERED_TXT"
    rm -f -- "$tmp"
    {
        printf '# Uncovered (never-executed) source lines per file, from\n'
        printf '# %s\n' "$LINE_GATE_TRACE"
        printf '# Paths are relative to %s. A line is listed when its DA: record shows a hit\n' "$HDMICEC_ROOT"
        printf '# count of zero. Files with full line coverage are omitted.\n'
        printf '# Line coverage bar in force for this run: %s%%\n' "$COVERAGE_MIN"
        printf '# PROVENANCE  superproject %s\n' "$(git_sha_of "$WS" superproject)"
        printf '# PROVENANCE  runner       %s  sha256 %s\n' "$SCRIPT_PATH" "$(sha256_of "$SCRIPT_PATH")"
        printf '# Full manifest: %s\n' "${PROVENANCE_TXT:-provenance.txt}"
        printf '#\n'
        # Emitted as ONE tab-separated line per file so that sorting cannot separate a
        # file's line list from its heading, then split into the two-line human form
        # afterwards.  Sorting the already-split form is the obvious mistake here: the
        # continuation lines begin with whitespace and all migrate to the top of the file.
        "$AWK_BIN" -v root="$HDMICEC_ROOT/" -v min="$COVERAGE_MIN" '
            function flush_record() {
                if (sf == "") return
                rel = sf
                if (index(rel, root) == 1) rel = substr(rel, length(root) + 1)
                if (nmiss > 0) {
                    pct = (lf > 0 ? 100 * lh / lf : 0)
                    printf "%s\t%d/%d lines = %.1f%%\t%s\t%d uncovered\t%s\n", \
                           rel, lh, lf, pct, \
                           (lf > 0 && pct >= min + 0 ? "PASS" : "BELOW"), nmiss, miss
                }
                sf = ""
            }
            /^SF:/  { flush_record()
                      sf = substr($0, 4); lh = 0; lf = 0; nmiss = 0; miss = ""
                      next }
            /^LF:/  { lf = substr($0, 4) + 0; next }
            /^LH:/  { lh = substr($0, 4) + 0; next }
            # DA:<line>,<hit count>[,<checksum>]
            /^DA:/  { split(substr($0, 4), f, ",")
                      if (f[2] + 0 == 0) { nmiss++; miss = miss (nmiss > 1 ? " " : "") f[1] }
                      next }
            /^end_of_record/ { flush_record(); next }
            END { flush_record() }
        ' "$LINE_GATE_TRACE" \
        | LC_ALL=C sort -t "$(printf '\t')" -k1,1 \
        | "$AWK_BIN" -F'\t' '{ printf "%s\t%s\t%s\t%s\n    uncovered lines: %s\n", $1, $2, $3, $4, $5 }'
    } >"$tmp" || { rm -f -- "$tmp"; die "uncovered-line enumeration failed for $LINE_GATE_TRACE"; }
    mv -f -- "$tmp" "$UNCOVERED_TXT" || die "could not write $UNCOVERED_TXT"
}

# shellcheck disable=SC2016  # single-quoted awk programs; see the note above write_per_file_tsv.
per_file_report() {
    rule
    write_per_file_tsv
    write_uncovered_lines

    log "per-file coverage, derived from the filtered trace records:"
    printf '\n'
    # Rendered from the TSV so the printed table and the machine-readable artifact are the
    # same numbers by construction.  Column widths are fixed, and a metric with no data
    # reads "n/a" instead of a misleading 0.0%.
    "$AWK_BIN" -F'\t' -v min="$COVERAGE_MIN" '
        function cell(hit, found, rate) {
            if (rate == "n/a" || found + 0 <= 0) return sprintf("%7s %11s", "n/a", "no data")
            return sprintf("%6.1f%% %5d/%-5d", rate + 0, hit, found)
        }
        BEGIN {
            # Metric headers are LEFT-aligned in the same 19-character field the metric
            # cells occupy, so each header sits directly over its percentage.
            printf "  %-40s %-19s %-19s %-19s  %s\n", "FILE", "LINES", "FUNCTIONS", "BRANCHES", "LINE GATE"
            printf "  %-40s %-19s %-19s %-19s  %s\n", \
                   "----------------------------------------", \
                   "-------------------", "-------------------", "-------------------", "---------"
        }
        /^#/ { next }
        $1 == "file" { next }
        {
            rel = $1
            line_cell = cell($2, $3, $4)
            func_cell = cell($5, $6, $7)
            br_cell   = cell($10, $11, $12)
            verdict   = $13
            if (rel == "TOTAL") {
                printf "  %-40s %-19s %-19s %-19s  %s\n", \
                       "----------------------------------------", \
                       "-------------------", "-------------------", "-------------------", "---------"
                printf "  %-40s %19s %19s %19s  %s\n", \
                       "TOTAL (" files " source files)", line_cell, func_cell, br_cell, verdict
                next
            }
            files++
            printf "  %-40s %19s %19s %19s  %s\n", rel, line_cell, func_cell, br_cell, verdict
        }
    ' "$PER_FILE_TSV"
    printf '\n'

    log "FUNCTIONS above is the alias model (trace FNA:/FNDA: records), which is what lcov"
    log "  --summary and genhtml report, so the TOTAL row reconciles with the summary block."
    log "  The trace also carries FNF:/FNH: per-file leader totals, whose denominator is"
    log "  smaller because several aliases can share one leader; they are in column"
    log "  func_leader_* of the TSV rather than dropped."
    log "BRANCHES is evidence, never a gate: gcov counts branches as control-flow-graph arcs,"
    log "  including compiler-generated exception and static-destruction arcs no test reaches."
    log "machine-readable table: $PER_FILE_TSV"
    log "uncovered line numbers: $UNCOVERED_TXT"

    # Collected here, consumed by the gate, so the enumeration happens exactly once.
    BELOW_BAR_FILES="$("$AWK_BIN" -F'\t' '!/^#/ && $1 != "file" && $1 != "TOTAL" && $13 == "BELOW" { printf "%s\t%s\n", $1, $4 }' "$PER_FILE_TSV")"

    # Partition the below-bar set. Both halves are reported; only the gated half votes.
    BELOW_BAR_GATED=''
    BELOW_BAR_EXEMPT=''
    if [ -n "$BELOW_BAR_FILES" ]; then
        BELOW_BAR_GATED="$(printf '%s\n' "$BELOW_BAR_FILES" | while IFS="$(printf '\t')" read -r p v; do
            [ -n "$p" ] || continue
            if printf '%s\n' "$COVERAGE_GATE_EXEMPT_FILES" | grep -Fxq -- "$p"; then :; else printf '%s\t%s\n' "$p" "$v"; fi
        done)"
        BELOW_BAR_EXEMPT="$(printf '%s\n' "$BELOW_BAR_FILES" | while IFS="$(printf '\t')" read -r p v; do
            [ -n "$p" ] || continue
            if printf '%s\n' "$COVERAGE_GATE_EXEMPT_FILES" | grep -Fxq -- "$p"; then printf '%s\t%s\n' "$p" "$v"; fi
        done)"
    fi
}


# ------------------------------------------------------------------------------------
# Step 6 -- THE GATE.  The first machine-checkable coverage threshold in this workspace.
#
# The aggregate verdict is delegated to lcov's own --fail-under-lines so the number that
# decides the outcome is lcov's rather than this script's, and its non-zero exit is what
# fails the run.  It MUST be paired with an operation: the bare
# `lcov --fail-under-lines N <trace>` form is rejected with "Need one of options -z, -c,
# -a, -e, -r, -l, --diff, --intersect, --subtract, or --summary" and exits 2 -- which,
# being non-zero, would masquerade as a working gate while never once looking at the
# coverage figure.  Hence `--summary <trace> --fail-under-lines N`.
#
# Files below the bar are enumerated unconditionally, because the requirement is per
# target and the enumeration is itself a deliverable.  Whether that enumeration also
# FAILS the run is the caller's choice: per-file gating is ON by default and
# `--no-per-file-gate` turns it off, for the reason given under GATE in the header.
# Branch coverage is never gated.
# ------------------------------------------------------------------------------------
# ====================================================================================
# THE BRANCH-COVERAGE MANIFEST AND ITS GATE
# ====================================================================================
#
# WHAT PROBLEM THIS SOLVES.  The requirement is full branch coverage of the SELECTION and the
# ARRAY-ADAPTATION code specifically, and a whole-file percentage cannot establish that:
# ccec/src/DriverAidlImpl.cpp contains many branches that are neither, so a high file figure can
# sit happily on top of an uncovered selection arm.  The gate therefore has to name individual
# arms -- which means mapping a semantic arm onto something a trace actually contains.
#
# WHY A MAPPING IS UNAVOIDABLE.  LCOV records a branch as
#     BRDA:<line>,<block>,<branch>,<taken>
# and those are COORDINATES, not names.  Nothing in the trace says "this is the
# service-not-found arm".  So the mapping from semantic arm to coordinate has to be produced
# from a real trace and written down, and it is written down HERE, with the source line quoted
# beside each entry so that a later reader can re-derive it rather than take it on trust.
#
# WHY IT LIVES IN THIS SCRIPT RATHER THAN IN A FILE OF ITS OWN.  A sibling manifest file would
# be a TWENTY-SIXTH in-submodule path in a change whose diff check requires exactly TWENTY-FIVE
# and nothing more, so it would fail that check.  The scope identity, measured rather than
# recalled -- `git diff --name-status` against the change's baseline reports 25 paths inside
# this submodule, 13 modified and 12 added, and the superproject diff reports the `hdmicec`
# gitlink advancing plus `blitzy/documentation/Project Guide.md`, giving 26 LOGICAL FILES:
# 14 UPDATE, of which the Guide is one, and 12 CREATE.  An extra file here would therefore be
# both a twenty-sixth submodule path and a twenty-seventh logical file, and the check names its
# allowlist exactly, so neither number has any slack to absorb it.  The manifest is data plus
# gate logic, both of which a shell script can hold perfectly well, so it holds them.
#
# IDENTIFICATION IS BY BLOCK AND BRANCH INDEX, NEVER BY LINE ALONE.  A short-circuited
# condition contributes several arcs on ONE line -- `if (a && b)` is at least two -- so a
# line-only key would match an arm the entry did not mean and would pass or fail on the wrong
# arc.  Every entry therefore carries all three of line, block and branch.
#
# THE GATE FAILS ON TWO CONDITIONS, AND THE SECOND IS THE ONE THAT EARNS ITS KEEP:
#   (a) a mapped record whose `taken` count is ZERO -- the arm exists and nothing drove it;
#   (b) a mapped record that is ABSENT FROM THE TRACE ALTOGETHER -- which is what catches a
#       refactor that silently deleted an arm.  Without (b), deleting the branch would make the
#       gate greener rather than redder, and a gate that rewards deletion is worse than none.
#
# WHAT `-` ACTUALLY MEANS, MEASURED, BECAUSE IT IS EASY TO GET WRONG.  It reads as an arc gcov
# considers impossible to drive, which would make every `-` "unexercisable" and absolve it.  That
# reading is FALSE, and it is the single largest false-green vector available to this gate: `-`
# means THE ENCLOSING BASIC BLOCK
# WAS NEVER ENTERED.  A four-function probe compiled with the same gcc-13 and read back through
# the same lcov settles it -- a function that is compiled, linked and never called reports `-`
# on BOTH arcs of its `if`, while the identical `if` in a function that is called reports `1` and
# `0`.  So under that reading "unexercisable" is indistinguishable from "untested", and the gate
# would treat the latter as the former for every arm nothing drove.
#
# `-` IS THEREFORE A FAILURE ON ANY ARM THIS RUN COULD HAVE MEASURED, and is expected only where
# the arm's reacher could not run at all.  Which of those two applies is not guesswork: field 9
# of each record states what the arm NEEDS, and the gate compares that against what actually ran.
#
# THIS MANIFEST IS REVIEWED WHENEVER A MAPPED FILE CHANGES.  Coordinates are a function of the
# source: inserting a line above a mapped arm moves it, and adding a condition to a mapped `if`
# renumbers its branches.  A stale coordinate shows up as condition (b) -- absent from the
# trace -- which is the loud failure rather than the quiet one, but it is still a review that
# belongs with the source change and not with a puzzled reading of this gate's output.
#
# ------------------------------------------------------------------------------------
# WHERE THESE COORDINATES CAME FROM, AND WHAT IS AND IS NOT PROVEN BY THEM.
#
# EVERY COORDINATE BELOW WAS READ OUT OF A REAL UNFILTERED TRACE.  Not guessed, not derived from
# the source by counting conditions: captured from `coverage.info` after a real `--build --run`
# on a host running invocations A and D, and cross-checked arm by arm against `gcov -b -c -t`'s
# own labelled output so that each index is tied to the branch the entry names.
#
# THAT IS POSSIBLE WITHOUT A BINDER DRIVER, AND THE REASON MATTERS.  BRDA coordinates come from
# the COMPILE-TIME .gcno, not from execution: a file that is compiled and linked emits a BRDA
# record for every branch in it, with a `taken` of 0 or `-` where nothing drove it.  So the
# COORDINATES of an arm belonging to a deferred invocation are as measurable here as any other,
# and only its `taken` COUNT is unavailable.  Concluding that a host without a driver can produce
# no coordinates at all, and leaving every one of them empty, is a wrong inference from a correct
# principle; the principle -- never write a number that was not measured -- is what the `needs`
# fields carry instead.
#
# WHAT IS PROVEN, AND WHERE.  Every one of the 89 required arms below has been observed non-zero,
# and the 30 that carry a prerequisite were observed on a binder-capable guest rather than assumed.
# On 2026-09-01 the full A-E matrix ran in one instrumented i386 guest under the QEMU provisioning
# that .github/workflows/aidl-path-tests.yml owns -- kernel 5.15.148 with
# CONFIG_ANDROID_BINDER_IPC_32BIT=y, binder protocol 7 agreeing between kernel, SDK build and the
# BINDER_VERSION ioctl, servicemanager answering -- and this gate reported, over the one
# accumulated capture: `89 taken, 0 never taken, 0 absent, 0 deferred, 0 unchecked`.  All five
# invocations passed all four of their own checks, and the line gate passed both halves
# (aggregate and per file) for the first time, with ccec/src/DriverAidlImpl.cpp at 88.4% line.
# So the 28 arms needing invocation B, the 1 needing invocation C and the 1 needing a driver with
# no service on it are measured facts, not projections.
#
# WHAT A DRIVERLESS HOST STILL CANNOT SHOW, STATED PRECISELY.  Those same 30 arms are reported
# DEFERRED, not passed, on any host without a binder driver -- which is the ordinary development
# case and the hosted-CI case both.  A driverless run therefore enforces 59 of the 89 and names
# the rest with the invocation or resource they are missing; it is not an acceptance run and this
# script does not let it read as one.  The other 59 required arms are observed non-zero by a
# driverless run: every arm of the bounded preflight, including the two
# beyond the BINDER_VERSION gate, is reached under invocation A through the BinderPreflightProbe
# seam that ccec/src/DriverAidlImpl.hpp declares and isBinderPreflightOk() takes as a defaulted
# third parameter, so their `needs` fields are empty and their counts are real.  A seam rather
# than syscall interposition is what makes that true without a driver and without redefining
# ioctl() or mmap() for a 568-case process.  The receive path's own two guards and the
# slow-call diagnostic are in the deferred set, because a
# listener callback and a synchronous AIDL call both need a live session; the queue handoff's
# reserved-slot arithmetic and the context-manager timeout ceiling are NOT, because a
# test-local subclass drives offerReceivedFrame() directly and the probe seam records the
# bound it was handed, so both of those pairs are enforced on any host.
# branch_arm_unmet_requirement() below understands a
# `binder` resource token and one arm -- availability.service-absent -- declares it, because that
# arm needs the preflight to SUCCEED and the lookup to then return nothing, which is a driver
# present with no service on it and is not the same condition as any invocation letter.
# Arms with an unmet prerequisite are reported DEFERRED with the missing invocation or resource
# NAMED, they are folded into the advisory that stops this run producing an acceptance verdict,
# and they are never counted as passes.  The arms reachable under invocation A were observed and
# their counts are real; if one of those ever reads zero it is a FAILURE on this host, today.
#
# HOW EACH INDEX WAS TIED TO ITS ARM, so a later reader can repeat it rather than trust it:
#   * lcov DROPS gcov's `call` records, renumbers the surviving branches from 0 in gcov's order,
#     and writes block `e0` for the arcs gcov labels "(throw)".  Block ids are therefore NOT
#     always numeric, which is why the gate compares them as strings.
#   * For a plain `if (cond)`, the FIRST non-throw record on the line is gcov's "(fallthrough)"
#     arc, which is the condition TRUE / body-entered arm.  Verified on resolveBackEnd()'s
#     isServiceAvailable() guard in Driver.cpp, on DriverAidlImpl::open()'s state guard, and on
#     the probe.
#   * For a `&&`/`||` chain the per-operand arcs appear in evaluation order, and "fallthrough"
#     there means "carry on evaluating" rather than "true" -- which is why halcompat.h's entries
#     do not all sit on the line the condition is written on.  Its `era(server) == era(client)`
#     is written on line 112 and its arcs are recorded against line 114; that is gcc's
#     attribution, it was measured one semantic case at a time with volatile arguments to defeat
#     constexpr folding, and the entries below follow the measurement rather than the source
#     layout.
#
# THIS MANIFEST IS REVIEWED WHENEVER A MAPPED FILE CHANGES, and the ABSENT check is what enforces
# it: a coordinate whose arm has moved or been deleted fails loudly rather than passing quietly.
# ------------------------------------------------------------------------------------
#
# RECORD FORMAT, '|'-separated:
#   1 id         short stable name for the arm, used in the gate's output
#   2 file       path suffix as it appears after SF: in the trace
#   3 line       gcov line number, measured
#   4 block      gcov block index, measured -- a STRING, because `e0` is a legitimate value
#   5 branch     gcov branch index, measured
#   6 reacher    the invocation or unit test that drives this arm, in prose, for the report.
#                An arm with no named reacher is a DEFECT IN THE TEST SET, not a target to
#                lower -- so every entry below has one.
#   7 status     required | unreachable
#   8 needs      what must have been available for this run to MEASURE the arm: a
#                space-separated conjunction of invocation letters and the literal `binder`.
#                EMPTY means "measurable whenever invocation A runs", which is every run --
#                so an empty-needs arm reading 0 or `-` is a failure here and now.
#   9 source     the source line, quoted, so the coordinate can be re-derived
#
# THE QUOTED SOURCE IS DELIBERATELY THE LAST FIELD, and that is not cosmetic.  It is free text
# lifted out of C++, so it can and does contain the '|' this format separates on -- halcompat.h's
# `if (hash.empty() || hash == "-1")` contains two.  A `read` with a fixed field list puts all
# remaining text into its LAST variable, so keeping the quote last makes an embedded '|' harmless
# rather than something to escape and get wrong.  Measured the other way round: with the quote
# in field 8, those two entries have their source truncated at the '||' and the fragment after it
# parses as a requirement token, and the gate duly reports "needs invocation |".
# Every other field is a token this script generates or a short prose phrase with no '|' in it.
#
# WHY FIELD 9 IS A SEPARATE FIELD RATHER THAN PARSED OUT OF FIELD 6.  Field 6 is prose written
# for a human reading a failure report, and it says things like "invocation C at factory level;
# unit test otherwise".  A gate that scraped invocation letters out of that would be deciding
# fatal-versus-deferred by regex over an English sentence, and would silently change verdict the
# next time someone improves the wording.  So the machine-readable requirement is stated once,
# separately, and the prose stays prose.
# ====================================================================================
readonly BRANCH_MANIFEST=(
# THE `needs` FIELD IS THIS RUNNER'S OWN, and the values below are stated in its terms rather
#   than as a list of every invocation that happens to reach an arm.  It names the PREREQUISITES
#   an arm has, whitespace separated, ALL of which must be satisfied before the gate will judge
#   its takenness -- so an EMPTY field means "measurable on any run, including a driverless one",
#   an invocation letter means "defer unless that invocation ran", and `binder` means "defer
#   unless a usable binder driver is present".  An arm a driverless invocation reaches therefore
#   carries an EMPTY field and is ENFORCED here, which is the disposition that matters: it is
#   the only half of this manifest a host without a binder kernel can hold to account, and
#   leaving those arms nominally waiting on an invocation that did run would have enforced
#   nothing.  ABSENCE is judged before prerequisites either way, so a coordinate that has moved
#   or been deleted fails whatever its needs field says.
    # ---- GROUP 1: THE SELECTION HELPER.  resolveBackEnd in ccec/src/Driver.cpp decides once per
    # process at line 140 and then reports the reason at line 170, so there are four arms: the
    # two outcomes of the decision, and the two arms of the reason report.
    #
    # WHY THE REASON ARMS ARE NOT THE ONES THAT DISTINGUISH THE CAUSES.  The factory does not
    # work out WHY the AIDL back-end was declined by calling
    # DriverAidlImpl::isBinderPreflightOk() a second time: isServiceAvailable() records which
    # of its ordered stages declined and the factory reads that back through
    # unavailabilityReason(), because re-running the preflight pays its context-manager timeout
    # twice and can report a reason that did not cause the fallback.  So line 170's branch is
    # only "was a reason recorded", and the arms that distinguish transport-unavailable from
    # no-compatible-service belong to isServiceAvailable() -- mapped in GROUP 1B below, at their
    # real home rather than at a proxy for it.
    "selection.aidl-selected|ccec/src/Driver.cpp|140|0|4|invocation B, and invocation E in the L2 tier: a compatible service resolves and the AIDL back-end is returned|required|B|if (aidlBackEnd.isServiceAvailable()) {"
    "selection.legacy-fallback|ccec/src/Driver.cpp|140|0|5|invocations A, C and D (measured taken 2 by A and D on a driverless host)|required||if (aidlBackEnd.isServiceAvailable()) {"
    "selection.reason-reported|ccec/src/Driver.cpp|170|0|0|every legacy fallback: the query recorded a reason and the factory logs it (measured taken 2 by A and D on a driverless host)|required||if (unavailability != NULL) {"
    "selection.reason-unrecorded|ccec/src/Driver.cpp|170|0|1|NOTHING, and that is the point. isServiceAvailable() assigns a reason on every one of its false exits, so a NULL reason cannot arise from any code path that exists; the arm is a defensive report for an outcome nobody could otherwise diagnose. It is recorded UNREACHABLE rather than required so the gate still fails if it is DELETED while exempting it from the taken check -- exactly the disposition the one pre-existing unreachable record has|unreachable||if (unavailability != NULL) {"

    # ---- GROUP 1B: THE AVAILABILITY QUERY, which now owns the arms GROUP 1 used to proxy.
    # DriverAidlImpl::isServiceAvailable() answers in THREE ordered stages and records which one
    # declined.  ALL THREE ARE GATED HERE, in six records -- two arms each.  AAP section 0.9.3
    # requires the service-found, service-NOT-found and found-but-incompatible arms by name, and
    # an aggregate branch percentage cannot stand in for a named-arm gate: a high file figure and
    # an uncovered selection arm coexist perfectly well.
    #
    # THE MIDDLE STAGE IS MAPPED WITH A HOST-AWARE REACHER RATHER THAN OMITTED.  No invocation
    # reaches it on EVERY host -- A stops at the preflight where there is no transport, and B, C
    # and E all register a service -- and that is a fact about reachability rather than a reason
    # to leave the arm out.  The NO-SERVICE token resolves through NO_SERVICE_EVIDENCE exactly as
    # PREFLIGHT-FALSE resolves through PREFLIGHT_FALSE_EVIDENCE, so the arm is DEFERRED on a
    # driverless host and REQUIRED on a binder-capable one, and neither host reports a false
    # verdict.
    #
    # Arc numbering follows the same rule as everywhere else in this manifest: a condition whose
    # evaluation involves calls contributes a call-and-exception arc PAIR per call BEFORE the
    # decision pair, so the decision is the LAST pair on the line.  Read from the unfiltered
    # trace of this tree rather than assumed, at the coordinates the six records below carry:
    #   * the transport stage (availability.transport-declined / -usable) sits on a line of FOUR
    #     arcs -- its condition calls isBinderPreflightOk() with all five arguments named, so the
    #     call contributes one call-and-exception pair rather than the three the defaulted form
    #     contributed.  The custody path is what changed that: the predicate takes the retained
    #     descriptor and its identity as out-parameters, so the call site names them.  The
    #     decision is therefore arcs 2 and 3.
    #   * the service-presence stage (availability.service-absent / -present) sits on a line of
    #     FOUR, its condition being a comparison against a call result, so the decision is arcs
    #     2 and 3.
    #   * the compatibility stage (availability.service-incompatible / -compatible) tests a plain
    #     bool local, so its line carries the decision pair alone, arcs 0 and 1.
    #   * the pre-lookup re-verification stage, mapped in GROUP 3b, sits on a line of FOUR for
    #     the same reason as the transport stage.
    #     same reason as 3826.
    "availability.transport-declined|ccec/src/DriverAidlImpl.cpp|3573|0|2|the preflight-false arm, read at its source: invocation A on a driverless host, or invocation A-NB on a binder-capable one (measured taken 2)|required||if (!isBinderPreflightOk(binderDriverPath, contextManagerTimeoutMs, probe,"
    "availability.transport-usable|ccec/src/DriverAidlImpl.cpp|3573|0|3|the preflight-TRUE arm. NO LONGER DEFERRED: the custody and re-verification cases drive isServiceAvailable() through a synthetic BinderPreflightProbe that passes, so this arm is measured on a driverless host and its needs field is now empty (measured taken 4)|required||if (!isBinderPreflightOk(binderDriverPath, contextManagerTimeoutMs, probe,"
    "availability.service-absent|ccec/src/DriverAidlImpl.cpp|3605|0|2|invocation A on a host WITH a usable binder transport: it registers nothing, so the lookup reaches the service manager and returns null. This is the service-NOT-FOUND arm AAP section 0.9.3 requires by name. Its reacher is the host-aware NO-SERVICE token, so it is DEFERRED rather than failed where there is no transport for A to reach|required|binder|if (service == 0) {"
    "availability.service-present|ccec/src/DriverAidlImpl.cpp|3605|0|3|invocations B, C and E: each registers a service under the production name, so the lookup returns non-null and the query proceeds to the compatibility stage. This is the service-FOUND arm|required|B|if (service == 0) {"
    "availability.service-incompatible|ccec/src/DriverAidlImpl.cpp|3617|0|0|invocation C: a registered service whose metadata halcompat rejects. The compatibility verdict is now taken into a named local so the F10 elapsed-time diagnostic can bracket the call, so the branch is the negation of that local and carries two arcs rather than the call/throw pair the inline call produced|required|C|if (!compatible) {"
    "availability.service-compatible|ccec/src/DriverAidlImpl.cpp|3617|0|1|invocation B, and invocation E in the L2 tier: halcompat accepts the registered service and the AIDL back-end becomes selectable. Same named-local shape as the sibling arm above|required|B|if (!compatible) {"

    # ---- GROUP 2: halcompat's compatibility predicate, arm by arm.
    # The template form is what the selection calls; the reject arms are only reachable
    # IN-PROCESS, because a remote Bn* service answers metadata transactions from compiled-in
    # constants and so cannot report a bad hash or a bad version at all.  Every arm here is
    # driven by DriverAidlCompatibilityTest, which is back-end independent and therefore runs
    # under A, B and C -- so `requires` is A,B,C throughout and A alone always satisfies it.
    #
    # The counts quoted are from the measured trace and are what fixes each label.  Measured over
    # the compatibility suite as it stands: 17 calls to the template (1 null, 16 with a
    # service); 16 interface-hash reads inside it (1 empty, 4 "-1", 2 "notfrozen", 9 frozen); and
    # 19 evaluations of detail::isCompatible -- the 9 that reach it through the template plus 10
    # made DIRECTLY, by the ordering case with a client of its own and by
    # DriverAidlImpl::observedMetadataWouldBeAccepted(), which the emitter calls on every
    # rejection it describes.
    #
    # THESE COUNTS ARE VOLATILE BY CONSTRUCTION and every one of them is re-measured whenever a
    # case is added or removed.  They are quoted because on a short-circuited condition the
    # count is what DISTINGUISHES one arc pair from another on the same line -- see the era and
    # ordering records on line 114 -- not because the gate compares them; the gate checks
    # presence and non-zero taken, so a stale count here misleads a reader without failing a
    # run, which is exactly why it is checked by hand against the trace.
    "compat.reject-null|rdk-halif-aidl/common/current/halcompat.h|161|0|2|unit test, DriverAidlCompatibilityTest.IncompatibleWhenServiceIsNull (measured taken 1 of 17 calls)|required||if (service == nullptr) {"
    "compat.service-present|rdk-halif-aidl/common/current/halcompat.h|161|0|3|every non-null case in DriverAidlCompatibilityTest (measured taken 16 of 17 calls)|required||if (service == nullptr) {"
    "compat.reject-empty-hash|rdk-halif-aidl/common/current/halcompat.h|165|0|1|unit test, DriverAidlCompatibilityTest.IncompatibleWhenInterfaceHashIsEmpty (measured taken 1; this is the hash.empty() TRUE arc, and on this short-circuited condition index 1 is the true arm, not index 0)|required||if (hash.empty() || hash == \"-1\") {"
    "compat.hash-non-empty|rdk-halif-aidl/common/current/halcompat.h|165|0|0|every case reporting a non-empty hash (measured taken 15, which is the number of times the second condition was evaluated at all)|required||if (hash.empty() || hash == \"-1\") {"
    "compat.reject-minus-one-hash|rdk-halif-aidl/common/current/halcompat.h|165|0|4|unit test, DriverAidlCompatibilityTest.IncompatibleWhenInterfaceHashIsMinusOne; also reached at FACTORY level by invocation C, whose in-process fake reports \"-1\" (measured taken 4 on a driverless host, where C is deferred. The four are TEMPLATE calls, which is the only way this arc is reachable: .IncompatibleWhenInterfaceHashIsMinusOne once, .ARecoveredMetadataReadMakesTwoCompatibilityCallsDisagree once on its first read, and .DelayedMetadataRecoveryOnAThirdReadCannotBeBlamedOnTheVersionRule twice while its double is still failing. DriverAidlImpl::observedMetadataWouldBeAccepted() does NOT contribute here and must not be credited with it -- it takes a hash by value, tests it inline and calls only detail::isCompatible, so it never enters this template at all)|required||if (hash.empty() || hash == \"-1\") {"
    "compat.hash-not-minus-one|rdk-halif-aidl/common/current/halcompat.h|165|0|5|every case reporting a usable hash (measured taken 11)|required||if (hash.empty() || hash == \"-1\") {"
    # LINE 168 IS THE ONE PLACE IN THIS MANIFEST WHERE THE ARC LAYOUT IS NOT PORTABLE, so its two
    #   records carry per-layout alternatives instead of a bare index.  Measured on both targets
    #   this project runs on: x86-64 gives line 168 three block-0 arcs (0, 2 and 3) plus an `e0`
    #   throw arc, and i386 gives it two (0 and 1) -- gcc expands the inlined std::string
    #   comparison differently per target.  The pair below previously read 2 and 3, which is right
    #   on x86-64 and does not exist on i386; rewriting them to 0 and 1 was right on i386 and made
    #   arc 1 vanish on x86-64.  Both spellings were observed failing as ABSENT arms, which is the
    #   signal reserved for a branch deleted from the source, so neither single spelling is usable
    #   and the alternates mechanism exists for exactly this.  See resolve_branch_alternates().
    #
    #   THE DIRECTION IS SETTLED BY THE LINE HITS EITHER SIDE OF THE BRANCH, never by the arc
    #   counts, because only the line hits say which arc is the true arm.  On the i386 layout
    #   DA:169 -- `return allowUnfrozen;`, reached only when the comparison is TRUE -- is 6, which
    #   is BRDA:168,0,0; DA:171, the version rule reached only when it is FALSE, is 32, which is
    #   BRDA:168,0,1.  The x86-64 pairing keeps the indices the original derivation measured there.
    "compat.reject-notfrozen|rdk-halif-aidl/common/current/halcompat.h|168|0|2:0,3:2|unit test, DriverAidlCompatibilityTest.UnfrozenServerIsRejectedByDefaultAndAcceptedOnlyWhenOptedIn (measured taken 6 across the template instantiations this suite exercises; confirmed as the TRUE arm because DA:169, the return-allowUnfrozen body, is 6 on the same run)|required||if (hash == \"notfrozen\") {"
    "compat.hash-frozen|rdk-halif-aidl/common/current/halcompat.h|168|0|2:1,3:3|the frozen-hash cases, which are the ones that reach the version rule at all (measured taken 32; confirmed as the FALSE arm because DA:171, the version rule it falls through to, is 32 on the same run)|required||if (hash == \"notfrozen\") {"
    "compat.server-era-frozen|rdk-halif-aidl/common/current/halcompat.h|111|0|0|unit test, DriverAidlCompatibilityTest.IncompatibleWhenServerReportsDifferentEra: the only server value with era >= 1 (measured taken 2 of the 19 evaluations on this line, which is what identifies this pair as era(serverVersion) >= 1 rather than the ternary's true arm)|required||? (serverVersion >= clientVersion)"
    "compat.server-era-zero|rdk-halif-aidl/common/current/halcompat.h|111|0|1|every era-0 server value the version cases use (measured taken 17)|required||? (serverVersion >= clientVersion)"
    "compat.client-era-zero|rdk-halif-aidl/common/current/halcompat.h|110|0|1|unit test, DriverAidlCompatibilityTest.IncompatibleWhenServerReportsDifferentEra: this client is era 0, so the second conjunct is false whenever it is evaluated at all (measured taken 2 of the 2 evaluations, which is what identifies this pair as era(clientVersion) >= 1)|required||return (era(serverVersion) >= 1 && era(clientVersion) >= 1)"
    "compat.era-equal|rdk-halif-aidl/common/current/halcompat.h|114|0|0|every same-era pair the version cases use (measured taken 17 of the 19 evaluations that reach the era-0 branch, which is what identifies this FIRST arc pair on line 114 as the era EQUALITY test)|required||&& serverVersion >= clientVersion);"
    "compat.reject-cross-era|rdk-halif-aidl/common/current/halcompat.h|114|0|1|unit test, DriverAidlCompatibilityTest.IncompatibleWhenServerReportsDifferentEra, plus the observation case that drives a cross-era snapshot through observedMetadataWouldBeAccepted (measured taken 2)|required||&& serverVersion >= clientVersion);"
    "compat.major-equal|rdk-halif-aidl/common/current/halcompat.h|113|0|0|the exact-version, newer-same-major and ordering cases (measured taken 12 of 17 evaluations)|required||&& major(serverVersion) == major(clientVersion)"
    "compat.reject-cross-major|rdk-halif-aidl/common/current/halcompat.h|113|0|1|unit tests, DriverAidlCompatibilityTest.IncompatibleWhenServerReportsDifferentMajor and .IncompatibleWhenServerReportsUnfrozenGeneratorVersion -- version 1 decodes to major 0 and is rejected here, not by the ordering conjunct; the further reachers are the observation case driving a cross-major snapshot through observedMetadataWouldBeAccepted and the two production-emitter cases that drive a cross-major server through the emitter (measured taken 5)|required||&& major(serverVersion) == major(clientVersion)"
    "compat.accept-not-older|rdk-halif-aidl/common/current/halcompat.h|114|0|2|unit tests, DriverAidlCompatibilityTest.CompatibleWhenServerReportsThisClientsVersion and .CompatibleWhenServerReportsNewerVersionInSameMajor, which pin both ends of [1000,1999]; this SECOND arc pair on line 114 is the era-0 ordering conjunct, evaluated 12 times (measured taken 10)|required||&& serverVersion >= clientVersion);"
    "compat.reject-older-same-major|rdk-halif-aidl/common/current/halcompat.h|114|0|3|unit test, DriverAidlCompatibilityTest.OlderSameMajorServerIsRejectedByTheOrderingRule, which calls detail::isCompatible DIRECTLY with a client of 3020 and a server of 3000 because the arm is unreachable through the template for a client of 1000; the second reacher is DelayedMetadataRecoveryOnAThirdReadCannotBeBlamedOnTheVersionRule, whose observation snapshot pins the same ordering rule (measured taken 2)|required||&& serverVersion >= clientVersion);"

    # ---- GROUP 2a: ONE ARM THAT IS UNREACHABLE BY CONSTRUCTION.  Recorded with its proof so
    # that nobody spends time hunting it, and exempt from the TAKEN half of the gate only -- it
    # must still be PRESENT in the trace, so deleting it is caught like any other deletion.
    #
    # PROOF.  IHdmiCec::VERSION is 1000.  Under the encoding era*100000 + major*1000 + minor*10
    # + bugfix that decodes to era 0, major 1.  The era >= 1 ternary arm therefore cannot be
    # reached through halcompat::isCompatible<IHdmiCec>: (era(server) >= 1 && era(client) >= 1)
    # has era(client) == 0 as a conjunct and is false for every possible server value.  Measured
    # taken 0, on the one evaluation the cross-era case produces.
    #
    # WHY "OLDER-SAME-MAJOR" IS GATED ABOVE RATHER THAN EXEMPT HERE, THOUGH IT READS AS
    # UNREACHABLE.  The argument for exempting it is that for client 1000 the same-era
    # same-major servers are exactly [1000,1999] and every one of them satisfies
    # server >= client.  That proof is sound as far as it goes -- and it is about the
    # TEMPLATE, not about the ARC.  The ordering conjunct's false arc IS reachable, by calling
    # detail::isCompatible with a client of its own, which is what
    # OlderSameMajorServerIsRejectedByTheOrderingRule does; the arc is measured taken 2.  So it
    # is a required record above, not an exemption here.  An arm is exempt only when no test can
    # drive it, never merely because one route to it is closed.
    #
    # WORTH KNOWING WHILE READING halcompat.h's OWN PROOFS: its
    # static_assert(!isCompatible(3000, 2000), "era0 older rejected") is MISLABELLED.
    # major(3000) == 3 and major(2000) == 2, so that case is decided by the major inequality
    # and merely re-covers cross-major.  A genuine older-same-major call needs equal era AND
    # equal major with a lower server -- detail::isCompatible(3020, 3000), which is the pair the
    # test above uses for exactly this reason.
    "compat.client-era-frozen|rdk-halif-aidl/common/current/halcompat.h|110|0|0|UNREACHABLE: IHdmiCec::VERSION is 1000, so era(client) == 0 falsifies the conjunction for every server value; measured taken 0|unreachable||return (era(serverVersion) >= 1 && era(clientVersion) >= 1)"

    # ---- GROUP 3: THE BOUNDED BINDER PREFLIGHT, one record per arc of each of the eight decision
    # points isBinderPreflightOk is built from -- the source labels them "Decision point N of 8"
    # so that this mapping can be checked against it by reading rather than by inference.  Five of
    # the eight are here; the three the F3 custody path added -- the node-identity, character-device
    # and root-owner gates -- are GROUP 3a, so that the arms this migration introduced can be read
    # apart from the ones it inherited.
    #
    # THE RECORDS ARE ONE PER REAL ARC -- TEN IN THIS GROUP, AND SIXTEEN ACROSS THE PREDICATE
    # ONCE GROUP 3a'S THREE GATES ARE COUNTED -- RATHER THAN ONE PER NAMED FAILURE MODE.
    # node-absent and node-unopenable are not separate arms and do not get separate records --
    # there is one open call and one `driverFd < 0` test, and a missing node and an unopenable
    # one are indistinguishable at it.  In the other direction, the empty-path check and the
    # protocol-version READ -- as distinct from the protocol-version COMPARISON -- are arcs in
    # their own right and each carries its own record, which a failure-mode keying would miss.
    #
    # THE REACHERS ARE UNIT TESTS UNDER INVOCATION A, NOT INVOCATION B, and that is a property of
    # the seam rather than of this host.  isBinderPreflightOk takes a BinderPreflightProbe, so
    # DriverAidlPreflightTest drives the protocol-mismatch, context-manager and TRUE-positive
    # arms with a synthetic probe on any host, with no binder driver and without touching a real
    # node.  The remaining arms use a real path chosen by the test.  DriverAidlPreflightTest is
    # back-end independent and runs under A, B and C.
    "preflight.empty-path|ccec/src/DriverAidlImpl.cpp|2657|0|0|unit test, DriverAidlPreflightTest.DeclinesAnEmptyDriverPath, and the same arm inside .EveryPreflightArmReleasesExactlyTheDescriptorsItOpened (measured taken 3)|required||if (binderDriverPath.empty()) {"
    "preflight.path-given|ccec/src/DriverAidlImpl.cpp|2657|0|1|every other preflight case, and every production initialization (measured taken 33)|required||if (binderDriverPath.empty()) {"
    "preflight.node-unopenable|ccec/src/DriverAidlImpl.cpp|2665|0|0|unit test, DriverAidlPreflightTest.DeclinesANonexistentDriverPath -- still the ONE arc a missing node and an unopenable node share, because both decline the predicate and the verdict is what this gate maps. They no longer share the MESSAGE: a nested test on errno inside this arm reports ENOENT as a legacy-only platform and every other errno as a platform fault. That nested branch changes no verdict, so it is not a further negative arm and is deliberately not a record of its own; the ENOENT side is the one every case here drives (measured taken 8, which includes the driverless host's own production preflight)|required||if (driverFd < 0) {"
    "preflight.node-opened|ccec/src/DriverAidlImpl.cpp|2665|0|1|unit tests, DriverAidlPreflightTest.DeclinesAPathThatOpensButIsNotABinderDriver and every synthetic-probe case (measured taken 25)|required||if (driverFd < 0) {"
    "preflight.version-unreadable|ccec/src/DriverAidlImpl.cpp|2781|0|2|unit test, DriverAidlPreflightTest.DeclinesAPathThatOpensButIsNotABinderDriver: a real node that opens and answers no BINDER_VERSION ioctl (measured taken 2)|required||if (0 != probe.readProtocolVersion(driverFd, &protocolVersion)) {"
    "preflight.version-read|ccec/src/DriverAidlImpl.cpp|2781|0|3|every synthetic-probe case that survives the node-identity gates, which reports a version and so reaches the comparison (measured taken 16)|required||if (0 != probe.readProtocolVersion(driverFd, &protocolVersion)) {"
    "preflight.protocol-mismatch|ccec/src/DriverAidlImpl.cpp|2807|0|2|unit test, DriverAidlPreflightTest.DeclinesANodeWhoseProtocolVersionDiffersFromThisBuild, driving expected+1 through the synthetic probe (measured taken 3)|required||if (protocolVersion != expectedBinderProtocolVersion()) {"
    "preflight.protocol-equal|ccec/src/DriverAidlImpl.cpp|2807|0|3|unit tests, DriverAidlPreflightTest.DeclinesAMatchingProtocolWhoseContextManagerNeverAnswers and .AcceptsANodeWhoseProtocolMatchesAndWhoseContextManagerAnswers, plus every custody and service-query case that needs the preflight to pass (measured taken 13)|required||if (protocolVersion != expectedBinderProtocolVersion()) {"
    "preflight.context-manager-answers|ccec/src/DriverAidlImpl.cpp|2831|0|1|unit test, DriverAidlPreflightTest.AcceptsANodeWhoseProtocolMatchesAndWhoseContextManagerAnswers -- the whole-predicate TRUE verdict. THE CONDITION IS NOW WRITTEN NEGATED, if (!contextManagerReachable), because the positive arm falls through to the custody hand-off rather than to a return, so this arm is index 1 where it used to be index 0 (measured taken 8)|required||if (!contextManagerReachable) {"
    "preflight.context-manager-silent|ccec/src/DriverAidlImpl.cpp|2831|0|0|unit test, DriverAidlPreflightTest.DeclinesAMatchingProtocolWhoseContextManagerNeverAnswers, which also asserts the caller's deadline reached the probe. Index 0 of the negated condition, per the sibling arm above (measured taken 5)|required||if (!contextManagerReachable) {"

    # ---- GROUP 4: THE ONE-ELEMENT ADDRESS ARRAYS, add and remove, both arcs of both decisions.
    # These are AIDL-path arms: they need the AIDL back-end resolved, so invocation B is their
    # reacher and a driverless host reports them DEFERRED rather than passed or failed.  Both
    # arcs of each decision are recorded, which is also what makes the TRUE/FALSE labels safe on
    # a host that cannot execute the line: a label the wrong way round still gates both arms.
    "addlogical.status-not-ok|ccec/src/DriverAidlImpl.cpp|2323|0|0|invocation B, DriverAidlSessionTest.AddLogicalAddressMapsRefusalAndTransportFailureToDistinctExceptions: a non-ok binder Status -> IOException|required|B|if (!txn.isOk()) {"
    "addlogical.status-ok|ccec/src/DriverAidlImpl.cpp|2323|0|1|invocation B, DriverAidlSessionTest.AddLogicalAddressMarshalsExactlyOneElement|required|B|if (!txn.isOk()) {"
    "addlogical.refused|ccec/src/DriverAidlImpl.cpp|2327|0|0|invocation B, DriverAidlSessionTest.AddLogicalAddressMapsRefusalAndTransportFailureToDistinctExceptions: added == false -> AddressNotAvailableException|required|B|else if (!added) {"
    "addlogical.accepted|ccec/src/DriverAidlImpl.cpp|2327|0|1|invocation B, DriverAidlSessionTest.AddLogicalAddressMarshalsExactlyOneElement: the address is appended to the local list|required|B|else if (!added) {"
    "removelogical.status-not-ok|ccec/src/DriverAidlImpl.cpp|2283|0|0|invocation B, DriverAidlSessionTest.RemoveLogicalAddressIgnoresTransportFailureAndStillRemovesLocally|required|B|if (!txn.isOk()) {"
    "removelogical.status-ok|ccec/src/DriverAidlImpl.cpp|2283|0|1|invocation B, DriverAidlSessionTest.RemoveLogicalAddressSucceedsMarshalsOneElementAndDropsItLocally|required|B|if (!txn.isOk()) {"
    "removelogical.refused|ccec/src/DriverAidlImpl.cpp|2286|0|0|invocation B, DriverAidlSessionTest.RemoveLogicalAddressMarshalsOneElementAndIgnoresHalRefusal: removed == false is logged at LOG_EXP and ignored|required|B|else if (!removed) {"
    "removelogical.accepted|ccec/src/DriverAidlImpl.cpp|2286|0|1|invocation B, DriverAidlSessionTest.RemoveLogicalAddressSucceedsMarshalsOneElementAndDropsItLocally|required|B|else if (!removed) {"

    # ---- GROUP 5: getLogicalAddresses, every cardinality plus the transport failure.
    "getlogical.status-not-ok|ccec/src/DriverAidlImpl.cpp|2171|0|0|invocation B, DriverAidlSessionTest.GetLogicalAddressReportsZeroOnTransportFailureWithoutRaising|required|B|if (!txn.isOk()) {"
    "getlogical.status-ok|ccec/src/DriverAidlImpl.cpp|2171|0|1|invocation B, DriverAidlSessionTest.GetLogicalAddressReadsEntryZeroFromTheServiceInterface|required|B|if (!txn.isOk()) {"
    "getlogical.empty|ccec/src/DriverAidlImpl.cpp|2174|0|0|invocation B, DriverAidlSessionTest.EmptyAddressResultReportsZeroSoTheExistingCallerSignalSurvives|required|B|else if (halAddresses.empty()) {"
    "getlogical.non-empty|ccec/src/DriverAidlImpl.cpp|2174|0|1|invocation B, DriverAidlSessionTest.GetLogicalAddressReadsEntryZeroFromTheServiceInterface|required|B|else if (halAddresses.empty()) {"
    "getlogical.more-than-one|ccec/src/DriverAidlImpl.cpp|2186|0|0|invocation B, DriverAidlSessionTest.MultipleReturnedAddressesUseEntryZeroAndAreLogged|required|B|if (halAddresses.size() > 1) {"
    "getlogical.exactly-one|ccec/src/DriverAidlImpl.cpp|2186|0|1|invocation B, DriverAidlSessionTest.GetLogicalAddressReadsEntryZeroFromTheServiceInterface|required|B|if (halAddresses.size() > 1) {"

    # ---- GROUP 6: the frame-length guard, both sides.  The over-length side is difference 1
    # of the authorized observable differences, so both arms are evidence rather than trivia.
    "framelen.over-contract|ccec/src/DriverAidlImpl.cpp|2021|0|0|invocation B, DriverAidlTransmitTest.FramesOverTheAidlLimitAreRefusedWithoutBeingSentOrTruncated|required|B|if (length > AIDL_MAX_MESSAGE_LENGTH) {"
    "framelen.within-contract|ccec/src/DriverAidlImpl.cpp|2021|0|1|invocation B, every DriverAidlTransmitTest case that transmits, beginning with .DirectedFrameAcknowledgedByTheFollowerSucceeds|required|B|if (length > AIDL_MAX_MESSAGE_LENGTH) {"
    # ---- GROUP 7: THE RECEIVE PATH'S TWO GUARDS.  Both were added because an out-of-process
    # HAL can deliver what an in-process one never could.
    #
    # THE LENGTH GUARD IS NOT DEFENSIVE PROGRAMMING.  An empty std::vector<uint8_t> is a legal
    # AIDL payload, and an empty CECFrame on the incoming queue is taken by the Bus reader,
    # handed to printFrameDetails(), and decoded as Header(frame, 0) -- which reaches
    # frame.at(0) and raises std::out_of_range.  printFrameDetails() catches Exception&, the
    # CCEC base, which does not match it, and Bus::Reader::run() catches InvalidStateException&,
    # which does not either, so the exception escapes the thread function and TERMINATES THE
    # PROCESS on one malformed message from the HAL.  The bound is one byte and not two because
    # a header-only frame is a legitimate CEC poll -- the very frame this back-end's own poll()
    # transmits.
    #
    # THE OWNERSHIP ARM IS THE OTHER HALF.  offerReceivedFrame() reports whether the queue took
    # the frame; on true the callback clears its pointer immediately and on false it releases
    # the frame itself.  Both arcs are mapped because a change that always reported acceptance
    # would leak one frame per event, and one that always reported refusal would double free.
    #
    # Both lines carry `-` on a driverless host: the listener is reached only through a live
    # session, so these arms belong to invocation B and E and are declared as needing B.
    "receive.message-too-short|ccec/src/DriverAidlImpl.cpp|1269|0|0|invocations B and E: the fake service delivers an empty payload and the callback discards it before allocating|required|B|if (message.size() < MIN_RECEIVED_MESSAGE_LENGTH) {"
    "receive.message-acceptable|ccec/src/DriverAidlImpl.cpp|1269|0|1|invocations B and E: every ordinary received frame, including the one-byte poll|required|B|if (message.size() < MIN_RECEIVED_MESSAGE_LENGTH) {"
    "receive.queue-accepted|ccec/src/DriverAidlImpl.cpp|1313|0|2|invocations B and E: the ordinary delivery, where the queue takes the frame and the callback drops its pointer|required|B|if (owner->offerReceivedFrame(frame)) {"
    "receive.queue-refused|ccec/src/DriverAidlImpl.cpp|1313|0|3|invocations B and E with a stalled reader: the queue is at its receive limit and the callback releases the frame rather than leaking it|required|B|if (owner->offerReceivedFrame(frame)) {"

    # ---- GROUP 8: THE QUEUE HANDOFF'S RESERVED SLOT.  Both arms are measured DRIVERLESS, which
    # is what makes this group the enforced half of the receive-path work: DriverAidlLocalInstanceTest
    # drives offerReceivedFrame() directly on a test-local subclass, so the arithmetic that keeps
    # close()'s sentinel undroppable is held to account on any host.
    #
    # The refusal point is one BELOW the capacity on purpose.  A sentinel the queue swallowed
    # would leave the Bus reader blocked in EventQueue::poll() with nothing coming to wake it,
    # so the receive path stops one entry early.  Measured on this host: 10 refusals and 7876
    # acceptances across the five F02 cases.
    "offer.no-slot|ccec/src/DriverAidlImpl.cpp|2502|0|0|invocation A, DriverAidlLocalInstanceTest.ReceiveQueueReservesTheLastSlotForCloseSentinelAndRefusesTheFrameThatWouldTakeIt and .EveryFrameOfferedToAFullReceiveQueueHasExactlyOneOwner (measured taken 11)|required||if (occupancyBefore >= (INCOMING_QUEUE_CAPACITY - 1)) {"
    "offer.slot-available|ccec/src/DriverAidlImpl.cpp|2502|0|1|invocation A, every accepted offer in the five F02 cases (measured taken 7878)|required||if (occupancyBefore >= (INCOMING_QUEUE_CAPACITY - 1)) {"

    # ---- GROUP 9: THE CONTEXT-MANAGER TIMEOUT CEILING.  isBinderPreflightOk() takes its bound
    # as an unsigned int, so a caller can name a figure larger than any plausible servicemanager
    # start-up delay and every millisecond of it would be time LibCCEC::init() spends blocked.
    # The clamp is measured driverlessly through the probe seam, which records the bound it was
    # actually handed, so both arms are enforced here rather than deferred.
    "preflight.timeout-clamped|ccec/src/DriverAidlImpl.cpp|2823|0|0|invocation A, DriverAidlPreflightTest.ClampsAContextManagerTimeoutAboveTheCeilingToTheCeiling (measured taken 1)|required||if (effectiveTimeoutMs > MAX_CONTEXT_MANAGER_TIMEOUT_MS) {"
    "preflight.timeout-within-ceiling|ccec/src/DriverAidlImpl.cpp|2823|0|1|invocation A, every other preflight case including .PassesAContextManagerTimeoutAtTheCeilingThroughUnchanged (measured taken 12)|required||if (effectiveTimeoutMs > MAX_CONTEXT_MANAGER_TIMEOUT_MS) {"

    # ---- GROUP 10: THE SLOW-HAL-CALL DIAGNOSTIC.  A THRESHOLD, NOT A TIMEOUT: crossing it
    # abandons nothing and raises nothing, it only leaves a LOG_WARN line where a stall would
    # otherwise be silent.  B4 records that the underlying unbounded synchronous call cannot be
    # bounded on the pinned libbinder, so these arms measure the mitigation that IS in scope.
    #
    # All three carry `-` driverlessly, because no synchronous AIDL call is issued at all when
    # the preflight declines: they belong to invocation B.  The clock-unreadable arm is mapped
    # alongside the threshold because it decides whether a measurement is trusted, and a
    # diagnostic computed from a fabricated origin would be worse than none.
    # THE PAIR ON LINE 490 WAS INVERTED, and the first full-matrix trace settles it beyond
    #   argument.  Line 490 is a short-circuited `||` carrying six arcs; arcs 0 and 1 are its
    #   FIRST decision, `HAL_CALL_CLOCK_UNREADABLE == startedMs`.  DA:491 -- the `return;` reached
    #   only when the whole condition is TRUE -- is 0, and DA:494, the `elapsedMs` computation
    #   reached only when it is FALSE, is 288.  BRDA:490,0,0 is 288 and BRDA:490,0,1 is 0, so arc
    #   0 is the readable arm and arc 1 the unreadable one, the opposite of what was recorded.
    #   Read the old way the gate credited "clock unreadable" with 288 takings on runs where no
    #   clock read had failed at all, which is worse than a miss: it reported an unexercised
    #   defensive arm as covered.
    #
    # AND THE UNREADABLE ARM IS UNREACHABLE FROM ANY TEST SURFACE, recorded as such rather than
    #   left to fail.  Two independent reasons, both measured rather than argued: monotonicNowMs()
    #   and halCallStarted() sit inside the anonymous namespace opened at DriverAidlImpl.cpp:185,
    #   so they have internal linkage and `nm -DC libRCEC.so` finds no dynamic symbol for either --
    #   no test translation unit can call them or substitute for them.  And the only production
    #   route into the arm is clock_gettime(CLOCK_MONOTONIC, &stack_timespec) returning non-zero,
    #   which Linux does not do: EINVAL needs an unsupported clk_id and this one is a compile-time
    #   constant the vDSO serves, EFAULT needs a bad pointer and this one is a stack local.  It is
    #   kept in the manifest, not deleted, so the gate still fails if the arm is REMOVED from the
    #   source -- the same disposition as compat.client-era-frozen and selection.reason-unrecorded.
    #   Driving it would take a clock_gettime interposer, which would test the interposer.
    "halcall.clock-unreadable|ccec/src/DriverAidlImpl.cpp|490|0|1|UNREACHABLE: halCallStarted() only yields HAL_CALL_CLOCK_UNREADABLE when clock_gettime(CLOCK_MONOTONIC) fails on a stack timespec, which Linux does not do, and the helper has internal linkage so no test can substitute for it; measured taken 0 with DA:491 also 0 on the full matrix|unreachable|B|if ((HAL_CALL_CLOCK_UNREADABLE == startedMs) || !monotonicNowMs(nowMs)) {"
    "halcall.clock-readable|ccec/src/DriverAidlImpl.cpp|490|0|0|invocation B: the ordinary case, where startedMs was readable so the second read is evaluated and the elapsed time can be compared (measured taken 288, matching DA:494 on the same run)|required|B|if ((HAL_CALL_CLOCK_UNREADABLE == startedMs) || !monotonicNowMs(nowMs)) {"
    "halcall.slow|ccec/src/DriverAidlImpl.cpp|496|0|0|invocation B against a deliberately delayed fake: one LOG_WARN line naming the operation and the elapsed time|required|B|if (elapsedMs > SLOW_HAL_CALL_WARN_MS) {"
    "halcall.within-threshold|ccec/src/DriverAidlImpl.cpp|496|0|1|invocation B: every healthy synchronous call, which must stay silent|required|B|if (elapsedMs > SLOW_HAL_CALL_WARN_MS) {"

    # ---- GROUP 3a: THE NODE-IDENTITY GATES, added with the F3 custody path.  The preflight used
    # to open the driver node, validate it and CLOSE it before returning, and libbinder then
    # reopened the SAME PATHNAME during the service lookup -- so everything the check established
    # was established about an object the process no longer held.  On the pinned stack a driver
    # open that fails, or a protocol that mismatches, is a LOG_ALWAYS_FATAL_IF abort rather than
    # an error return, so a node substituted in that window turned a check whose whole purpose is
    # a NONFATAL legacy fallback into a process abort.
    #
    # Three gates now stand between the open and the protocol read, and all three are measured
    # driverlessly through the same BinderPreflightProbe seam the rest of GROUP 3 uses: the
    # object behind the descriptor can be identified at all, it is a CHARACTER DEVICE, and it is
    # owned by ROOT.  Each is a real refusal with its own verdict, so each is one arc pair.
    "preflight.identity-unreadable|ccec/src/DriverAidlImpl.cpp|2706|0|2|unit test, DriverAidlPreflightTest.DeclinesANodeWhoseDescriptorCannotBeIdentified: without an identity there is nothing to re-verify before libbinder opens the same name, so the predicate declines rather than proceeding on an unverifiable node (measured taken 2)|required||if (0 != probe.identifyDescriptor(driverFd, &identity)) {"
    "preflight.identity-read|ccec/src/DriverAidlImpl.cpp|2706|0|3|every synthetic-probe case whose node identifies, which is every case that reaches the type and owner gates below (measured taken 23)|required||if (0 != probe.identifyDescriptor(driverFd, &identity)) {"
    "preflight.not-character-device|ccec/src/DriverAidlImpl.cpp|2723|0|0|unit test, DriverAidlPreflightTest.DeclinesANodeThatIsNotACharacterDevice. Every supported binder layout publishes a character device -- a devtmpfs node under /dev, or a binderfs node reached directly or through a symlink -- so a regular file at that name is the shape a substitution takes and is refused rather than handed to libbinder (measured taken 3)|required||if ((identity.mode & BINDER_NODE_MODE_TYPE_MASK) != BINDER_NODE_MODE_CHARACTER_DEVICE) {"
    "preflight.character-device|ccec/src/DriverAidlImpl.cpp|2723|0|1|every case whose node is the expected character device (measured taken 20)|required||if ((identity.mode & BINDER_NODE_MODE_TYPE_MASK) != BINDER_NODE_MODE_CHARACTER_DEVICE) {"
    "preflight.owner-not-root|ccec/src/DriverAidlImpl.cpp|2739|0|0|unit test, DriverAidlPreflightTest.DeclinesACharacterDeviceThatIsNotOwnedByRoot. devtmpfs and binderfs both create the node as root, so a node owned by anyone else is one an unprivileged process could have created, and selecting the AIDL path against it would put every transmitted and every delivered frame behind whatever registered there (measured taken 2)|required||if (identity.uid != BINDER_NODE_REQUIRED_OWNER_UID) {"
    "preflight.owner-root|ccec/src/DriverAidlImpl.cpp|2739|0|1|every case whose node is root-owned (measured taken 18)|required||if (identity.uid != BINDER_NODE_REQUIRED_OWNER_UID) {"
    # THE PERMISSION OBSERVATION IS VERDICT-NEUTRAL, AND BOTH ARMS ARE STILL REQUIRED.  A
    # permissive binder node is REPORTED and then accepted, because binder requires the node to
    # be openable by every client that uses it and rejecting a broadly-accessible node would
    # decline the AIDL path on a conformant platform rather than protect it.  Both arms are
    # mapped precisely BECAUSE the arm is verdict-neutral: the only thing that can regress here
    # is somebody turning the report into a refusal, or deleting it, and an unmapped arm would
    # let either through silently.
    #
    # Arc polarity was MEASURED, not assumed, and it is the opposite of the divergence decision
    # inside binderNodeIdentitiesMatch(): running only
    # DriverAidlPreflightTest.AcceptsAWorldWritableNodeAndReportsItsPermissionBits against zeroed
    # counters puts branch 0 at taken 1 and branch 1 at taken 0, so branch 0 is the arm that
    # reports.  gcov's "(fallthrough)" annotation does NOT identify the true arm consistently
    # across sites in this file -- verify by running one case, never by reading the label.
    "preflight.node-writable-beyond-owner|ccec/src/DriverAidlImpl.cpp|2774|0|0|unit test, DriverAidlPreflightTest.AcceptsAWorldWritableNodeAndReportsItsPermissionBits: a 0666 root-owned character device is ACCEPTED and its permission bits are reported once (measured taken 1)|required||if (0u != (identity.mode & (BINDER_NODE_MODE_GROUP_WRITE | BINDER_NODE_MODE_WORLD_WRITE))) {"
    "preflight.node-owner-only|ccec/src/DriverAidlImpl.cpp|2774|0|1|every other case reaching this point, whose synthetic node is not writable beyond its owner and which must emit nothing (measured taken 22)|required||if (0u != (identity.mode & (BINDER_NODE_MODE_GROUP_WRITE | BINDER_NODE_MODE_WORLD_WRITE))) {"

    # ---- GROUP 3b: THE PRE-LOOKUP RE-VERIFICATION, which is the other half of the F3 custody
    # path and the whole of the F4 mitigation that IS implementable.  reverifyBinderNodeBeforeLookup()
    # runs immediately before halcompat::getService(), and it decides FOUR things: the name still
    # resolves, it resolves to the SAME object the preflight validated, a descriptor reopened on it
    # still refers to that object, and a context manager still answers under the same bound.
    #
    # WHY THE PING RUNS ON A FRESH DESCRIPTOR rather than on the retained one, which is the one
    # place this design departs from "re-check what you are holding": the binder driver permits
    # exactly ONE mmap per open descriptor for that descriptor's lifetime, and pingContextManager
    # maps the fd it is given, so a second ping on the retained descriptor would fail on every
    # real platform and would decline the AIDL path universally.  The retained descriptor is the
    # INODE PIN; the fresh one is fstat-identified against the retained identity before it is used.
    #
    # The reopened-unidentifiable arm and the ping-answers arm are the two that need naming: the
    # first is driven by a probe whose identify fails only on its SECOND call, and the second
    # cannot be driven on a driverless host at all, because a re-verification that SUCCEEDS is
    # followed immediately by the real getService(), which needs a driver.
    "reverify.path-unresolvable|ccec/src/DriverAidlImpl.cpp|3197|0|2|unit test, DriverAidlPreflightTest.TheServiceQueryDeclinesWhenThePathCannotBeResolvedBeforeTheLookup: the name stopped resolving between the preflight and the lookup (measured taken 1)|required||if (0 != probe.identifyPath(binderDriverPath.c_str(), &observed)) {"
    "reverify.path-resolved|ccec/src/DriverAidlImpl.cpp|3197|0|3|every case that reaches the identity comparison below (measured taken 3)|required||if (0 != probe.identifyPath(binderDriverPath.c_str(), &observed)) {"
    "reverify.identity-changed|ccec/src/DriverAidlImpl.cpp|3209|0|3|unit test, DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheNameResolvesToADifferentNodeBeforeTheLookup: the substituted-node case this whole custody path exists for (measured taken 1)|required||if (!binderNodeIdentitiesMatch(validated, observed)) {"
    "reverify.identity-unchanged|ccec/src/DriverAidlImpl.cpp|3209|0|2|every case whose node is still the validated one (measured taken 1)|required||if (!binderNodeIdentitiesMatch(validated, observed)) {"
    # THE FIVE-ATTRIBUTE COMPARISON IS MEASURED HERE, AT ITS CALLER, AND DELIBERATELY NOT AT THE
    # `if (0u == divergence)` DECISION INSIDE binderNodeIdentitiesMatch() ITSELF.  The helper is
    # inlined at some call sites and not others, so the arcs on its out-of-line body move with the
    # compiler's inlining decisions and with which cases a filter happens to select: running one
    # case alone reports that decision as never executed while the full suite reports both arms
    # non-zero.  A record on an arc that relocates when the filter changes would report ABSENT for
    # a reason that has nothing to do with the code, which is the one failure this gate must not
    # manufacture.  The two arms above are on the caller's own line, they are stable, and every
    # attribute the helper compares is asserted by a named case: mode by
    # ...IsRePermissionedBeforeTheLookup, uid by ...IsReOwnedBeforeTheLookup, device/inode/rdev by
    # the pre-existing substitution cases, and the all-match path by
    # ...AcceptsAnUnchangedNodeOnAllFiveIdentityAttributes.
    "reverify.reopened-unidentifiable|ccec/src/DriverAidlImpl.cpp|3232|0|3|unit test, DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheReopenedDescriptorCannotBeIdentified, whose probe fails identifyDescriptor on its SECOND call only. THE ARC POLARITY IS PROVEN RATHER THAN ASSUMED: this is a short-circuited OR, and index 2 -- the operand-FALSE arc -- carries exactly the number of times the second operand was evaluated at all, which is the call count recorded on the next line, so index 3 is the operand-TRUE arc this record maps|required||if ((0 != probe.identifyDescriptor(freshFd, &reopened)) ||"
    "reverify.reopened-identified|ccec/src/DriverAidlImpl.cpp|3232|0|2|every case whose reopened descriptor identifies, which is every case that goes on to compare it against the validated identity (measured taken 2). The second operand's own arc pair, on the continuation line, is deliberately NOT mapped: both of its arcs are taken exactly once by this suite, so nothing in the trace distinguishes the match arm from the mismatch arm, and a record that named one of them would be a guess wearing a coordinate|required||if ((0 != probe.identifyDescriptor(freshFd, &reopened)) ||"
    "reverify.context-manager-silent|ccec/src/DriverAidlImpl.cpp|3254|0|0|unit test, DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheContextManagerStopsAnsweringBeforeTheLookup. This is the F4 mitigation measured: driver present and servicemanager wedged becomes a DECLINED selection instead of an unbounded wait inside defaultServiceManager() (measured taken 1)|required||if (!contextManagerReachable) {"
    "reverify.context-manager-answers|ccec/src/DriverAidlImpl.cpp|3254|0|1|invocations B, C and E: a re-verification that succeeds is followed immediately by the real halcompat::getService(), so this arm needs a binder driver and cannot be driven on a driverless host|required|B|if (!contextManagerReachable) {"
    "availability.reverify-declined|ccec/src/DriverAidlImpl.cpp|3592|0|2|the caller's side of the same decision: every re-verification refusal above reaches isServiceAvailable() through this arc and declines the AIDL path nonfatally (measured taken 4)|required||if (!reverifyBinderNodeBeforeLookup(binderDriverPath, contextManagerTimeoutMs, probe,"
    "availability.reverify-passed|ccec/src/DriverAidlImpl.cpp|3592|0|3|invocations B, C and E: the arm that proceeds to the service lookup, which needs a binder driver|required|B|if (!reverifyBinderNodeBeforeLookup(binderDriverPath, contextManagerTimeoutMs, probe,"

    # ---- GROUP 5a: THE LOGICAL-ADDRESS RANGE GATE, added with F7.  getLogicalAddress() used to
    # convert entry zero of the HAL's array with an unchecked cast, so any int32 the HAL returned
    # became the local device's logical address and reached the initiator nibble of every outbound
    # frame through Connection::matchSource().  The RAW value is what is tested, before any
    # conversion: LogicalAddress narrows, so 256 would become 0 and 271 would become 0xF, and a
    # post-conversion check would accept a plausible address the HAL never reported.
    #
    # It is a short-circuited `||`, so the first operand's pair is on the `if` line and the
    # second operand's pair follows it there; the polarity is fixed by the count, exactly as in
    # GROUP 3b -- index 0 carries the number of times the second operand was evaluated.
    "getlogical.address-below-min|ccec/src/DriverAidlImpl.cpp|2190|0|1|invocation A, DriverAidlLocalInstanceTest.TheLogicalAddressReadAcceptsOnlyContractRangeValues: the -1 and INT32_MIN rows (measured taken 2)|required||if ((rawAddress < HAL_LOGICAL_ADDRESS_MIN) || (rawAddress > HAL_LOGICAL_ADDRESS_MAX)) {"
    "getlogical.address-at-or-above-min|ccec/src/DriverAidlImpl.cpp|2190|0|0|every row at or above 0x0, which is every row that reaches the upper-bound test (measured taken 11)|required||if ((rawAddress < HAL_LOGICAL_ADDRESS_MIN) || (rawAddress > HAL_LOGICAL_ADDRESS_MAX)) {"
    "getlogical.address-above-max|ccec/src/DriverAidlImpl.cpp|2190|0|3|invocation A, the 0xF, 0x10, 256, 271 and INT32_MAX rows of the same case -- including the two that a post-conversion check would have accepted (measured taken 5)|required||if ((rawAddress < HAL_LOGICAL_ADDRESS_MIN) || (rawAddress > HAL_LOGICAL_ADDRESS_MAX)) {"
    "getlogical.address-within-contract|ccec/src/DriverAidlImpl.cpp|2190|0|2|invocation A, the 0x0, 0x1, 0x4 and 0xE rows plus the multi-address case's entry zero: the only values converted (measured taken 6)|required||if ((rawAddress < HAL_LOGICAL_ADDRESS_MIN) || (rawAddress > HAL_LOGICAL_ADDRESS_MAX)) {"

    # ---- GROUP 6a: THE TRANSMIT-STATUS SWITCH, added with F8.  write() used to decide the
    # transmit outcome with an if/else-if chain over SendMessageStatus, so a value that was
    # none of BUSY, ACK_STATE_0 or ACK_STATE_1 fell through to "Send Completed" and a transmit
    # the HAL never confirmed was reported to the caller as a success.  It is an exhaustive
    # switch now, and the DEFAULT arm is the one this gate exists to hold.
    #
    # Two of the four arcs on that line are mapped and two are not, deliberately.  The default
    # arm is unambiguous -- twelve rows drive it, which is the only count on the line that
    # matches a single case -- and so is the ACK_STATE_0 arm at three.  The remaining two arcs
    # are taken exactly twice each, so nothing in the trace tells BUSY from ACK_STATE_1, and
    # naming either would be a guess.  Their behaviour is asserted by the tests regardless;
    # what is missing is only a coordinate this gate could hold them to.
    "transmit.status-unknown|ccec/src/DriverAidlImpl.cpp|2066|0|3|invocation A, DriverAidlLocalInstanceTest.AnUndocumentedTransmitStatusIsTreatedAsAFailedTransmit: 3, 4, 99, -1, INT32_MAX and INT32_MIN crossed with a directed and a broadcast destination, all twelve raising IOException instead of reporting a completed send (measured taken 12)|required||switch (sendResult) {"
    "transmit.status-ack-state-0|ccec/src/DriverAidlImpl.cpp|2066|0|2|invocation A, DriverAidlLocalInstanceTest.EveryDocumentedTransmitStatusKeepsItsLegacyMapping: the directed-ACK row, the CEC-CTS-9-3-3 broadcast row and the non-CTS broadcast row (measured taken 3)|required||switch (sendResult) {"

    # ---- GROUP 7a: THE RECEIVE FLUSH'S NULL CHECK, added with F5.  read()'s flush arm dequeued
    # and dereferenced without checking, while close() offers a NULL sentinel on every transition
    # out of OPENED -- so two sentinels in the queue faulted the Bus reader thread, the one thread
    # whose death silently ends all CEC reception.
    #
    # THE POLARITY HERE IS MEASURED, NOT INFERRED FROM THE OTHER RECORDS, and it is the opposite
    # of the fallthrough=true shape most of this manifest uses.  Running
    # DriverAidlLocalInstanceTest.TheReceiveFlushSurvivesASecondCloseSentinel ALONE against a
    # zeroed counter set puts 1 on arc 0 and 0 on arc 1, and that case's flush drains exactly one
    # entry and that entry is a sentinel -- so arc 0 is the SKIPPED-SENTINEL arm.  A reader who
    # assumes the shape rather than repeating that measurement will map these two backwards.
    "read.flush-sentinel-skipped|ccec/src/DriverAidlImpl.cpp|1898|0|0|invocation A, DriverAidlLocalInstanceTest.TheReceiveFlushSurvivesASecondCloseSentinel: the second sentinel is skipped rather than dereferenced (measured taken 1)|required||if (inFrame != 0) {"
    "read.flush-frame-drained|ccec/src/DriverAidlImpl.cpp|1898|0|1|invocation A, DriverAidlLocalInstanceTest.TheReceiveFlushReleasesARealFrameQueuedBehindTheCloseSentinel: the parity arm, where the flush meets a real frame and copies and releases it exactly as it did before the null check existed|required||if (inFrame != 0) {"
)

# Set by apply_branch_gate and folded into apply_gate's own failure count, so the script keeps
# ONE final verdict and one exit status rather than two gates racing to exit first.
BRANCH_GATE_FAILURES=0

# ------------------------------------------------------------------------------------
# Print the BRDA records for one file suffix out of a trace, as "line block branch taken".
#
# Reads the trace once per file rather than once per manifest entry: several entries share a
# file, and re-walking a trace for each of thirty-odd arms would turn a gate into a cost.
# ------------------------------------------------------------------------------------
# ------------------------------------------------------------------------------------
# WAS THIS ARM MEASURABLE BY THIS RUN?  Prints the reason it was not, or nothing at all.
#
# Field 8 of a manifest record is a space-separated CONJUNCTION of requirement tokens, each
# either an invocation letter or the literal `binder`.  Every token must have been satisfied for
# the arm's `taken` count to mean anything: an arm belonging to invocation B on a host that
# deferred B has a count of `-` for a reason that says nothing whatever about the test set.
#
# WHY A CONJUNCTION AND NOT A LIST OF ALTERNATIVES.  No arm in the manifest is reached by "B or
# C"; the ones with several possible reachers all include invocation A, which always runs, and
# their `needs` is therefore empty.  A disjunction would be machinery with no member.  What the
# conjunction buys is a field that reads the same however many tokens an arm's requirement takes:
# `availability.transport-usable` needs `B` alone, because an invocation that selected the AIDL
# back-end implies a usable driver, while an arm needing the DRIVER without needing a selection
# names `binder` on its own.
#
# THE `binder` TOKEN IS CARRIED BY EXACTLY ONE RECORD, `availability.service-absent`, whose
# condition is a usable driver with nothing registered on it -- the service-NOT-FOUND arm, which
# invocation A reaches on a binder-capable host and cannot reach at all without one.  No other
# arm needs it: the preflight's protocol-version and context-manager arms name a driver in their
# prose but reach those decisions through the BinderPreflightProbe seam rather than through a
# real node (see GROUP 3), so their `needs` is empty and they are enforced on any host.  The
# token is the honest answer for an arm that genuinely needs the driver while being named by a
# test that runs regardless, and it stays in the mechanism for the next arm of that shape.
#
# THE INVOCATION SIDE IS ANSWERED FROM WHAT ACTUALLY RAN, not from what this host could in
# principle do: INVOCATIONS_RUN is built by the matrix as each invocation passes its four
# checks, so an invocation that ran and failed never reaches this gate at all.
# ------------------------------------------------------------------------------------
branch_arm_unmet_requirement() { # $1 = the record's needs field
    local needs="$1" token
    [ -n "$needs" ] || return 0
    for token in $needs; do
        case "$token" in
            binder)
                binder_transport_present \
                    || { printf 'a usable binder driver at %s' "$BINDER_DRIVER_NODE"; return 0; }
                ;;
            *)
                # Matched against the ', '-joined list with delimiters on both sides, so "A"
                # cannot match some future invocation named "AB".
                case ", ${INVOCATIONS_RUN}, " in
                    *", ${token}, "*) ;;
                    *) printf 'invocation %s, which did not run' "$token"; return 0 ;;
                esac
                ;;
        esac
    done
    return 0
}

branch_records_for_file() { # $1=trace  $2=SF path suffix
    # shellcheck disable=SC2016  # the single quotes are the awk program's; $0/$2 are awk's
    "$AWK_BIN" -v want="$2" '
        /^SF:/ {
            path = substr($0, 4)
            # SUFFIX match anchored on the END of the path, with the manifest supplying enough
            # leading directory components to be unambiguous.  Both halves matter and both were
            # checked against a real trace: end-anchoring is what stops "ccec/src/Driver.cpp"
            # matching "ccec/src/DriverAidlImpl.cpp" or "ccec/src/DriverImpl.cpp", and the
            # directory components are what stop it matching "tests/L1Tests/ccec/test_Driver.cpp".
            # A bare basename would satisfy neither.
            infile = (index(path, want) > 0 && index(path, want) + length(want) - 1 == length(path))
            next
        }
        /^end_of_record/ { infile = 0; next }
        infile && /^BRDA:/ {
            rec = substr($0, 6)
            n = split(rec, f, ",")
            if (n >= 4) print f[1], f[2], f[3], f[4]
        }
    ' "$1"
}

# Resolve a manifest branch field that carries PER-ARC-LAYOUT ALTERNATIVES.
#
# WHY ANY RECORD NEEDS ALTERNATIVES AT ALL, because a coordinate ought to be a property of the
# source and mostly is.  Measured across both architectures this project builds and runs on --
# x86-64 for the hosted L1 job, i386 for the binder-capable matrix job -- 312 of the 316
# branch-carrying lines in the three mapped files emit an IDENTICAL arc layout.  Four do not,
# because gcc expands some short-circuited conditions and some inlined std::string comparisons
# into a different number of arcs per target, and one of those four -- halcompat.h:168, the
# "notfrozen" gate -- carries mapped records.  There it is 3 block-0 arcs on x86-64 (0, 2, 3)
# against 2 on i386 (0, 1).  A manifest holding one set is therefore WRONG on one architecture
# whichever set it holds, and it fails in the worst available way: as an ABSENT arm, which is the
# signal reserved for a branch that has been deleted from the source.  Both were observed --
# absent on i386 with the x86-64 coordinates, absent on x86-64 with the i386 ones.
#
# WHAT THE KEY IS, AND WHY IT IS THE ARC COUNT rather than uname, a build flag or an environment
# variable.  The thing that varies is the layout, so the layout selects: the key is how many arcs
# the trace itself holds for that file, line and block.  It is read from the same trace the gate
# is judging, so it cannot disagree with it, and it needs nothing passed in from outside.  A
# record reading `2:0,3:2` says "arc 0 where the line has two block arcs, arc 2 where it has
# three", which is a statement a reader can check against a trace in one grep.
#
# AND WHAT HAPPENS TO A LAYOUT NOBODY HAS SEEN.  No key matches, so this prints a sentinel that
# cannot match any BRDA record, and the arm reports ABSENT.  That is deliberately the
# conservative direction: a new architecture with a fourth layout makes the gate FAIL and asks
# for a measurement, rather than quietly picking the nearest arc and crediting the wrong one.
#
# Arguments: $1 the branch field, $2 the records for the file, $3 the line, $4 the block.
# Prints: the resolved branch index, or the field unchanged when it carries no alternatives.
resolve_branch_alternates() {
    local field="$1" records="$2" line="$3" block="$4"

    # The overwhelmingly common case, kept first and kept free: a bare arc index, no ':' in it,
    # returned untouched.  Ninety of the ninety-two records take this path.
    case "$field" in
        *:*) ;;
        *) printf '%s\n' "$field"; return 0 ;;
    esac

    local arc_count
    arc_count="$(printf '%s\n' "$records" \
                 | "$AWK_BIN" -v l="$line" -v b="$block" \
                       '$1 "" == l "" && $2 "" == b "" { n++ } END { print n + 0 }')"

    local pair
    local old_ifs="$IFS"
    IFS=','
    # shellcheck disable=SC2086  # deliberate word splitting on the comma-separated list
    set -- $field
    IFS="$old_ifs"

    for pair in "$@"; do
        if [ "${pair%%:*}" = "$arc_count" ]; then
            printf '%s\n' "${pair#*:}"
            return 0
        fi
    done

    # No layout matched.  The sentinel is a string no BRDA branch field can ever equal, so the
    # caller's lookup reports ABSENT and the gate says so by name.
    printf '%s\n' "UNMATCHED-ARC-LAYOUT-${arc_count}"
}

# ------------------------------------------------------------------------------------
# THE BRANCH GATE.  Reads the UNFILTERED trace, per ONE TRACE, TWO CONSUMERS in the header:
# some of the arms above live in halcompat.h, which the line gate's derivative keeps but which
# any narrower filtering would remove.
#
# IT RUNS AFTER THE SUITE RESULTS AND AFTER THE LINE GATE'S INPUT IS PREPARED, and it records
# its failures rather than exiting on them, so that apply_gate remains the single place a
# verdict is issued.  Coverage is subordinate to functional passes: by the time this runs, every
# invocation has already been run and held to its four checks, and a red matrix never reaches
# here at all.
# ------------------------------------------------------------------------------------
apply_branch_gate() {
    rule
    log "branch-arm gate: checking the manifest's named arms against the UNFILTERED trace"
    log "  trace: $RAW_TRACE"

    [ -s "$RAW_TRACE" ] || die "the branch gate has no trace to read at $RAW_TRACE."

    local record id file line block branch reacher status source needs
    local required=0 unreachable=0 unpopulated=0 covered=0 zero_taken=0 absent=0 deferred=0
    local unreachable_present=0 unreachable_taken=0 unreachable_absent=0 unreachable_unpopulated=0
    local absent_required=0
    local -a zero_list=() absent_list=() deferred_list=() unreachable_list=() unreachable_taken_list=()

    # One pass per distinct file, cached in a variable keyed by nothing more elaborate than the
    # file we last read -- the manifest is grouped by file, so this is a single read per group.
    local cached_file='' cached_records=''

    # ------------------------------------------------------------------------------------
    # EVERY POPULATED TUPLE IS LOOKED UP, INCLUDING THE UNREACHABLE ONES.  This is the whole
    # of what `unreachable` exempts and the whole of what it does not, and the distinction is
    # the reason the status exists at all.
    #
    # THE DEFECT THIS SHAPE REPLACES.  The loop used to count an `unreachable` entry and
    # `continue` BEFORE it ever read the trace, so such an entry was never compared against
    # anything: delete its BRDA record, or move it by editing the line above it, and this gate
    # stayed green.  Both entries that carry the status say in their own text that they are
    # recorded unreachable "so the gate still fails if it is DELETED while exempting it from the
    # taken check" -- the manifest documented the intent and the code did not implement it, which
    # is the most expensive kind of disagreement because the manifest reads as evidence.
    #
    # SO THE EXEMPTION IS NARROWED TO EXACTLY TWO CHECKS.  An unreachable arm is exempt from the
    # taken-count check -- a `taken` of 0 or `-` is its EXPECTED state, which is why it is not
    # required -- and from the DEFERRED classification, which asks whether this run could have
    # driven the arm and is a question with no meaning for one nothing can drive.  It is NOT
    # exempt from existing: absence fails for an unreachable arm exactly as it fails for a
    # required one, because a coordinate that has vanished has vanished for the same two reasons
    # either way -- the source moved, or the branch was deleted -- and neither is made harmless
    # by the arm having been unreachable.
    #
    # AND THE THREE POPULATIONS ARE COUNTED APART.  required-and-taken, required-but-deferred and
    # unreachable-present-and-untaken are three different states of evidence, and a single
    # "checked" line that merged them would let a reader conclude that a taken count had been
    # observed where none was expected to exist.  Each has its own counter and its own line
    # below.
    # ------------------------------------------------------------------------------------
    local exempt_from_taken
    for record in "${BRANCH_MANIFEST[@]}"; do
        IFS='|' read -r id file line block branch reacher status needs source <<< "$record"

        exempt_from_taken=0
        if [ "$status" = 'unreachable' ]; then
            unreachable=$((unreachable + 1))
            exempt_from_taken=1
        else
            required=$((required + 1))
        fi

        if [ -z "$line" ] || [ -z "$block" ] || [ -z "$branch" ]; then
            # An unreachable entry with no coordinates is counted in its OWN unpopulated tally
            # rather than in the required one: the remedy is the same (measure it and fill them
            # in) but the number of required arms this gate could not check must stay the number
            # of REQUIRED arms, or the advisory below misstates its own subject.
            if [ "$exempt_from_taken" -eq 1 ]; then
                unreachable_unpopulated=$((unreachable_unpopulated + 1))
            else
                unpopulated=$((unpopulated + 1))
            fi
            continue
        fi

        if [ "$file" != "$cached_file" ]; then
            cached_records="$(branch_records_for_file "$RAW_TRACE" "$file")"
            cached_file="$file"
        fi

        # BLOCK AND BRANCH ARE COMPARED AS STRINGS, never numerically, because lcov writes `e0`
        # for the arcs gcov labels "(throw)" and that value appears in all three mapped files.
        # An awk numeric comparison would coerce `e0` to 0 and match the wrong record -- a
        # silent mis-identification rather than a visible error, which is the worst kind.
        # PER-ARC-LAYOUT RESOLUTION, ahead of the lookup and reported in the failure text below.
        # A record carrying no alternatives comes back unchanged, so this costs the other ninety
        # records one case statement.  See resolve_branch_alternates() for why four of the 316
        # branch-carrying lines differ between the x86-64 and i386 builds and why the arc count is
        # the key.
        local resolved_branch
        resolved_branch="$(resolve_branch_alternates "$branch" "$cached_records" "$line" "$block")"

        local taken
        # shellcheck disable=SC2016  # $1/$2/$3/$4 below are awk's fields, passed -v l/b/br
        taken="$(printf '%s\n' "$cached_records" \
                 | "$AWK_BIN" -v l="$line" -v b="$block" -v br="$resolved_branch" \
                       '$1 "" == l "" && $2 "" == b "" && $3 "" == br "" { print $4; found = 1; exit }
                        END { if (!found) print "ABSENT" }')"

        # ABSENCE IS CHECKED BEFORE MEASURABILITY AND BEFORE THE UNREACHABLE EXEMPTION, and both
        # orderings are deliberate.  An arm missing from the trace has moved or been deleted in
        # the SOURCE, which is true regardless of which invocations ran and regardless of whether
        # anything could ever have driven it -- and it is exactly the case neither a deferral nor
        # an exemption may excuse, because excusing it would let a refactor delete a mapped branch
        # and make this gate greener.
        if [ "$taken" = 'ABSENT' ]; then
            absent=$((absent + 1))
            if [ "$exempt_from_taken" -eq 1 ]; then
                unreachable_absent=$((unreachable_absent + 1))
            else
                absent_required=$((absent_required + 1))
            fi
            # The quoted source line is carried into the report, not just held in the
            # manifest: a reader told an arm has moved needs the line to look for, and
            # making them go and read the manifest to find it is a step this can save.
            #
            # THE STATUS IS NAMED IN THE ENTRY rather than splitting the list in two.  An
            # absent arm is one failure with one remedy whichever status it carried, and a
            # reader looking at the list needs to know which of the two they are holding --
            # for an unreachable one, "it was never taken" is not the explanation, because it
            # was never expected to be.
            #
            # The coordinate is reported with the RESOLVED branch index, which is the one the
            # lookup above actually used and therefore the one a reader can grep the trace for.
            if [ "$exempt_from_taken" -eq 1 ]; then
                absent_list+=("$id  ($file:$line block $block branch $resolved_branch)  [recorded UNREACHABLE]")
                absent_list+=("    exempt from the taken check, NOT from existing: this coordinate is")
                absent_list+=("    gone from the trace, so the arm has moved or been deleted.")
            else
                absent_list+=("$id  ($file:$line block $block branch $resolved_branch)")
            fi
            absent_list+=("    manifest source: $source")
            continue
        fi

        # THE UNREACHABLE ARMS END HERE, having been proved to EXIST.  Their taken count is
        # reported and not judged: 0 or `-` is the state the entry predicts, and a count that
        # is neither is a statement about the manifest rather than about the test set, so it is
        # carried into its own list below instead of being folded into either verdict.
        if [ "$exempt_from_taken" -eq 1 ]; then
            unreachable_present=$((unreachable_present + 1))
            case "$taken" in
                -|0)
                    unreachable_list+=("$id  ($file:$line block $block branch $resolved_branch)  taken: $taken")
                    unreachable_list+=("    $reacher")
                    ;;
                *)
                    unreachable_taken=$((unreachable_taken + 1))
                    unreachable_taken_list+=("$id  ($file:$line block $block branch $resolved_branch)  taken: $taken")
                    unreachable_taken_list+=("    recorded as: $reacher")
                    unreachable_taken_list+=("    manifest source: $source")
                    ;;
            esac
            continue
        fi

        # WAS THIS RUN IN A POSITION TO MEASURE THE ARM AT ALL?  If not, its count is reported
        # as DEFERRED with the missing resource named -- never as a pass, and never as a failure
        # of the test set, because neither would be true.
        local unmet
        unmet="$(branch_arm_unmet_requirement "$needs")"
        if [ -n "$unmet" ]; then
            deferred=$((deferred + 1))
            deferred_list+=("$id  ($file:$line block $block branch $resolved_branch)  needs $unmet")
            continue
        fi

        case "$taken" in
            -|0)
                # BOTH VALUES ARE FAILURES HERE, and that they read differently in the trace is
                # a detail of how much of the function ran rather than a difference in verdict:
                # `0` is "the block was entered and this arc was not taken", `-` is "the block
                # was never entered at all".  This arm was measurable by this run and was not
                # measured, which is the same finding either way.
                zero_taken=$((zero_taken + 1))
                zero_list+=("$id  ($file:$line block $block branch $resolved_branch)  taken: $taken")
                zero_list+=("    should be reached by: $reacher")
                zero_list+=("    manifest source:     $source")
                ;;
            *)
                covered=$((covered + 1))
                ;;
        esac
    done

    log "  manifest: $required required arm(s), $unreachable recorded unreachable by construction"

    # ------------------------------------------------------------------------------------
    # THE UNREACHABLE POPULATION, REPORTED AS ITS OWN THING.  Present in the trace, exempt from
    # the taken check, and now provably present rather than assumed to be: each line names the
    # count actually read, so a reader can see that the exemption was applied to a record that
    # exists.  An absent one never reaches here -- it is a failure above, in the absent list.
    # ------------------------------------------------------------------------------------
    if [ "$unreachable_present" -gt 0 ]; then
        log "  $unreachable_present of $unreachable unreachable arm(s) are PRESENT in the trace and exempt"
        log "    from the taken check only:"
        printf '%s\n' "${unreachable_list[@]}" | sed 's/^/[run_coverage]      /'
    fi
    if [ "$unreachable_unpopulated" -gt 0 ]; then
        warn "  $unreachable_unpopulated unreachable arm(s) carry NO COORDINATES, so their presence in the"
        warn "  trace was not checked either.  An entry with no coordinates is exempt from"
        warn "  everything, which is not what 'unreachable' is for: fill them in the way the"
        warn "  populated entries were, from $RAW_TRACE."
        note_advisory "the branch-arm gate could not check the existence of $unreachable_unpopulated arm(s) recorded
       unreachable in BRANCH_MANIFEST: they carry no line/block/branch coordinates, so the one
       check that still applies to an unreachable arm -- that its record is still there -- was
       not performed on them."
    fi
    if [ "$unreachable_taken" -gt 0 ]; then
        warn "  $unreachable_taken arm(s) recorded UNREACHABLE were TAKEN by this run:"
        printf '%s\n' "${unreachable_taken_list[@]}" | sed 's/^/[run_coverage]      /' >&2
        warn "  That is a finding about the MANIFEST, not about the test set: something drove an"
        warn "  arm whose entry states nothing can.  The entry is wrong and should become a"
        warn "  required one with the reacher that drove it named -- which makes this gate"
        warn "  STRICTER, so it is reported rather than silently accepted.  It is not counted as"
        warn "  a failing arm, because no requirement has gone unmet."
        note_advisory "the branch-arm gate observed $unreachable_taken arm(s) recorded UNREACHABLE in
       BRANCH_MANIFEST with a non-zero taken count.  The exemption those entries carry is
       therefore unjustified as written: re-derive them as required arms naming what drove them,
       because an exemption nobody needs hides the next arm that does read zero."
    fi

    # ------------------------------------------------------------------------------------
    # THE UNPOPULATED CASE.  It is neither a pass nor a failure of the TEST SET: it is this
    # gate reporting that it could not do its job, which is the only honest thing to say when
    # the coordinates it needs have never been read out of a real trace.
    # ------------------------------------------------------------------------------------
    if [ "$unpopulated" -gt 0 ]; then
        warn "  $unpopulated of $required required arm(s) have NO COORDINATES in the manifest, so"
        warn "  they were NOT checked.  This is not a pass and is not counted as one."
        warn "  EVERY ENTRY SHIPPED IN THIS SCRIPT IS POPULATED, so an empty one means an entry"
        warn "  was added without its coordinates.  Fill them in the way the others were: run"
        warn "  --build --run, then read the (line, block, branch) of the arm out of"
        warn "    $RAW_TRACE"
        warn "  matching it against the source line quoted in the entry, and cross-check the"
        warn "  index with 'gcov -b -c -t' so the arm and the arc are known to be the same thing."
        note_advisory "the branch-arm gate could not check $unpopulated of $required required arm(s) in
       BRANCH_MANIFEST: they carry no line/block/branch coordinates, so nothing about them was
       measured and nothing may be reported as though it had been.  Every entry shipped in this
       script is populated, so this means an arm was added without measuring it -- the remedy is
       to measure it, not to remove it."
    fi

    # ------------------------------------------------------------------------------------
    # THE DEFERRED ARMS.  Present in the trace, coordinates verified, but nothing this run was
    # able to do could have driven them -- so their counts are reported as unmeasured, with the
    # missing invocation or resource named per arm, and folded into the SAME advisory mechanism
    # that already stops a partial matrix producing an acceptance verdict.
    #
    # THIS IS NOT A PASS AND IS NOT COUNTED AS ONE.  It is also not a failure of the test set:
    # the cases exist, are compiled into the binaries, and were not run.  Saying either of those
    # instead would be a lie in one direction or the other, and the whole value of this gate is
    # that it says which.
    # ------------------------------------------------------------------------------------
    if [ "$deferred" -gt 0 ]; then
        warn "  $deferred of $required required arm(s) are DEFERRED -- present in the trace with"
        warn "  their coordinates confirmed, but unmeasurable by this run:"
        printf '%s\n' "${deferred_list[@]}" | sed 's/^/[run_coverage]      /' >&2
        warn "  Each line names what was missing.  A deferred arm reports no branch evidence and"
        warn "  contributes no pass; re-run the whole matrix on a binder-capable host to measure"
        warn "  them.  Do NOT respond by removing an entry: the arm list is the requirement, and"
        warn "  a shorter manifest measures less while looking better."
        note_advisory "the branch-arm gate could not measure $deferred of $required required arm(s): the
       invocation or the binder driver each one needs was not available on this host, so full
       branch coverage of the selection and array-adaptation code is NOT established by this run.
       The coordinates are populated and verified against the trace -- what is missing is the
       execution that would drive them, which is the same deferral the invocation matrix reports
       above and not a second finding."
    fi

    # ------------------------------------------------------------------------------------
    # FAILURE CONDITION (b): a mapped arm that is not in the trace at all.
    # Listed FIRST because it usually means the source moved, which changes what the reader
    # should do next -- review the manifest against the source, rather than write a test.
    # ------------------------------------------------------------------------------------
    if [ "$absent" -gt 0 ]; then
        warn "  $absent mapped arm(s) are ABSENT FROM THE TRACE ENTIRELY:"
        printf '%s\n' "${absent_list[@]}" | sed 's/^/[run_coverage]      /' >&2
        warn "  An arm that is not in the trace has either been DELETED from the source or has"
        warn "  MOVED, and both matter: a deleted branch would otherwise make this gate greener"
        warn "  rather than redder, which is why absence fails instead of being ignored."
        warn "  THIS INCLUDES THE ARMS RECORDED UNREACHABLE, marked as such in the list above:"
        warn "  their exemption is from the taken check, never from existing, so a deleted or"
        warn "  moved coordinate fails here whatever status its entry carries."
        warn "  Check the source lines quoted in BRANCH_MANIFEST for each id above, then either"
        warn "  re-derive the coordinates or remove the entry with a reason."
        BRANCH_GATE_FAILURES=$((BRANCH_GATE_FAILURES + absent))
    fi

    # ------------------------------------------------------------------------------------
    # FAILURE CONDITION (a): a mapped arm present and never driven.
    # ------------------------------------------------------------------------------------
    if [ "$zero_taken" -gt 0 ]; then
        warn "  $zero_taken mapped arm(s) WERE MEASURABLE BY THIS RUN AND WERE NOT TAKEN:"
        printf '%s\n' "${zero_list[@]}" | sed 's/^/[run_coverage]      /' >&2
        warn "  Every arm listed here had everything it needs -- its reacher ran, and any binder"
        warn "  driver it needs is present -- so this is a GENUINE GAP in the test set and not a"
        warn "  consequence of a deferral.  Arms whose reacher could not run are reported"
        warn "  separately above as DEFERRED and are not in this list."
        warn "  A 'taken' of '-' here means the enclosing block was never entered, which is the"
        warn "  same finding as 0 and not a milder one.  The answer is a test that drives the"
        warn "  arm -- never a smaller manifest."
        BRANCH_GATE_FAILURES=$((BRANCH_GATE_FAILURES + zero_taken))
    fi

    if [ "$BRANCH_GATE_FAILURES" -eq 0 ] && [ "$unpopulated" -eq 0 ] && [ "$deferred" -eq 0 ]; then
        log "  every one of the $required required arm(s) is present and taken at least once"
    elif [ "$BRANCH_GATE_FAILURES" -eq 0 ] && [ "$unpopulated" -eq 0 ]; then
        log "  the $covered measurable arm(s) are all present and taken; $deferred could not be"
        log "    measured here and are reported above rather than passed"
    fi
    # THE THREE POPULATIONS ON THREE LINES, because they are three different kinds of evidence
    # and one merged line is what let the middle one be assumed rather than read.  The required
    # tally accounts for every required arm; the unreachable tally accounts for every
    # unreachable one; and both add up to the manifest, which is the arithmetic a reader can
    # check without reading the manifest.
    log "  required    ($required): $covered taken, $zero_taken never taken, $deferred deferred,"
    log "                  $absent_required absent, $unpopulated unchecked"
    log "  unreachable ($unreachable): $unreachable_present present and exempt from the taken check only,"
    log "                  $unreachable_absent absent, $unreachable_unpopulated with no coordinates to check"
    log "                  (of the present, $unreachable_taken were taken -- see above if not zero)"
    log "  absent from the trace, either population: $absent -- each one a FAILURE above"
    return 0
}

apply_gate() {
    local failures=0 rc=0

    rule
    log "applying the >= ${COVERAGE_MIN}% line-coverage gate to the aggregate (lcov --fail-under-lines)"
    lcov_run --summary "$LINE_GATE_TRACE" \
        --fail-under-lines "$COVERAGE_MIN" \
        "${LCOV_CONFIG_ARGS[@]}" \
        "${LCOV_RC_ARGS[@]}" \
        --ignore-errors "$LCOV_SUMMARY_IGNORE" >/dev/null 2>&1 || rc=$?

    if [ "$rc" -eq 0 ]; then
        log "aggregate line coverage meets the ${COVERAGE_MIN}% bar"
    elif [ "$rc" -eq 2 ]; then
        # Defensive: a 2 here means lcov rejected the command line rather than the
        # coverage, and reporting that as a coverage failure would be a lie.
        die "lcov rejected the gate invocation (exit 2), which is a command-line error and
       NOT a coverage verdict. Re-run the same command without >/dev/null to see it:
           $LCOV_BIN --summary '$LINE_GATE_TRACE' --fail-under-lines $COVERAGE_MIN --rc branch_coverage=1
       lcov 2.x is required; 1.x has no --fail-under-lines at all."
    else
        warn "aggregate line coverage is BELOW ${COVERAGE_MIN}% (lcov exited $rc)"
        failures=$((failures + 1))
    fi

    if [ -n "$BELOW_BAR_EXEMPT" ]; then
        rule
        warn "below the ${COVERAGE_MIN}% bar, and LISTED AS EXEMPT FROM THE PER-FILE GATE:"
        printf '%s\n' "$BELOW_BAR_EXEMPT" | while IFS="$(printf '\t')" read -r path pct_value; do
            [ -n "$path" ] || continue
            printf '[run_coverage]     %-46s %s%%\n' "$path" "$pct_value" >&2
        done
        warn "These are still in the trace and still in the AGGREGATE denominator above - the"
        warn "listing suppresses only the per-file pass/fail vote. The reason for each is"
        warn "recorded at COVERAGE_GATE_EXEMPT_FILES in this script; read it before trusting it."
        warn "Their uncovered line numbers are in: $UNCOVERED_TXT"
    fi

    if [ -n "$BELOW_BAR_GATED" ]; then
        rule
        warn "targets below the ${COVERAGE_MIN}% line-coverage bar:"
        printf '%s\n' "$BELOW_BAR_GATED" | while IFS="$(printf '\t')" read -r path pct_value; do
            [ -n "$path" ] || continue
            printf '[run_coverage]     %-46s %s%%\n' "$path" "$pct_value" >&2
        done
        warn "Their uncovered line numbers are enumerated in:"
        warn "    $UNCOVERED_TXT"
        # A MISSING TEST AND A DEFERRED INVOCATION ARE DIFFERENT DIAGNOSES, and telling
        # someone to write tests that already exist wastes exactly the reader whose time this
        # script is meant to save.
        #
        # THE VERDICT DOES NOT CHANGE.  Below the bar still fails, and a deferral is never a
        # licence to pass: the code really is unmeasured, whatever the reason.  What changes is
        # only the ADVICE, because on a host that could not run the AIDL-selected invocations
        # the AIDL back-end's source is unmeasured BY CONSTRUCTION -- the cases that cover it
        # are written, are in the binary, and were never executed for want of a binder driver.
        # They were not skipped: the invocations that select them never started.  Adding tests
        # would not move those figures by a line.
        if [ -n "$INVOCATIONS_DEFERRED" ]; then
            warn "READ THE DEFERRALS BEFORE READING THESE FIGURES.  Invocation(s)"
            warn "  $INVOCATIONS_DEFERRED did not run on this host, so any target reached only by"
            warn "  them is unmeasured HERE for that reason and not for want of a test.  The AIDL"
            warn "  back-end and the selection helper are covered by cases that exist, are"
            warn "  compiled into the binaries and were never executed -- so these numbers are a"
            warn "  property of this HOST, not of the test set."
            warn "  This still FAILS, and deliberately: unmeasured is unmeasured whatever the"
            warn "  cause, and a run that excused itself here would be the one place a real gap"
            warn "  could hide. Re-run the whole matrix on a binder-capable host before treating"
            warn "  any figure above as this suite's coverage."
            warn "  Do NOT respond to this by lowering the bar, by exempting a file, or by"
            warn "  adding an exclusion glob. Nothing about a deferred invocation is fixed by"
            warn "  changing what the gate looks at."
        else
            warn "Close each gap by ADDING TESTS. Never by adding an exclusion glob, never by"
            warn "editing production source, and never by lowering the bar: the threshold is a"
            warn "gate, not a filter. Where a line genuinely cannot be reached from a test-only"
            warn "change, record it with its reason in the traceability report instead."
        fi
        if [ "$COVERAGE_PER_FILE_GATE" -eq 1 ]; then
            warn "per-file gating is IN FORCE (the default), so the above fails this run."
            failures=$((failures + 1))
        else
            warn "per-file gating has been turned OFF for this run (--no-per-file-gate or"
            warn "  COVERAGE_PER_FILE_GATE=0), so the above is reported but does not fail it."
            warn "  That is a DIAGNOSTIC setting: Directive 4 states the bar per target, so an"
            warn "  acceptance run leaves the per-file half on. Re-run without the override."
        fi
    elif [ -z "$BELOW_BAR_FILES" ]; then
        log "every target in the filtered trace meets the ${COVERAGE_MIN}% bar"
    else
        log "every gated target meets the ${COVERAGE_MIN}% bar; the exempt listing above is the"
        log "  complete set of files below it, and each carries its reason in this script."
    fi

    # THE BRANCH-ARM GATE'S VERDICT IS FOLDED IN HERE RATHER THAN ISSUED SEPARATELY, so that
    # this function stays the one place a verdict is decided and the script keeps one exit
    # status a caller can branch on.  It is counted AFTER the line gate, which is the required
    # ordering: coverage is subordinate to functional passes, and within coverage the line bar
    # is the acceptance criterion while the named-arm check is the evidence that the negative
    # and corner-case tests actually landed.
    if [ "$BRANCH_GATE_FAILURES" -ne 0 ]; then
        warn "the branch-arm gate reported $BRANCH_GATE_FAILURES failing arm(s) (listed above)."
        failures=$((failures + 1))
    fi

    rule
    if [ "$failures" -ne 0 ]; then
        local artifacts="$LINE_GATE_TRACE
           $PER_FILE_TSV
           $UNCOVERED_TXT"
        if [ "$DO_HTML" -eq 1 ]; then
            artifacts="$artifacts
           $HTML_DIR/index.html"
        fi
        # Below the bar fails the same way in both modes: a bad number is a bad number
        # whether or not this invocation produced the evidence for it.
        die "COVERAGE GATE FAILED (bar: ${COVERAGE_MIN}% line coverage).
       Artifacts for diagnosis:
           $artifacts"
    fi

    # At or above the bar -- but on whose evidence?  If anything reduced this run to
    # measuring what it did not establish, say so in the verdict and exit with a status a
    # caller can branch on, instead of printing the same line a clean run prints.
    if [ -n "$ADVISORY_REASONS" ]; then
        warn "COVERAGE ADVISORY (bar: ${COVERAGE_MIN}% line coverage): the figures meet the bar,"
        warn "  but THIS IS NOT AN ACCEPTANCE VERDICT, because:"
        printf '%s\n' "$ADVISORY_REASONS" | sed 's/^/[run_coverage]     /' >&2
        warn "  Every artifact was still produced and every number above is real; what is"
        warn "  missing is the provenance that would let anyone rely on them.  Exit status"
        warn "  $EXIT_ADVISORY marks that difference so a caller cannot mistake this for a pass."
        exit "$EXIT_ADVISORY"
    fi

    log "COVERAGE GATE PASSED (bar: ${COVERAGE_MIN}% line coverage)"
}

# ------------------------------------------------------------------------------------
# main.  The order is deliberate and is the whole argument of the script:
#   parse -> require the tools -> create the private lcov HOME -> name the tool
#   versions -> resolve artifacts -> [build] -> require an instrumented, exercised tree ->
#   [zero ONCE + run the whole invocation matrix + verify EACH invocation] -> capture ONCE
#   -> filter -> report HTML -> summarise -> per-file table -> gate.
#
# THE SUITE RESULTS COME BEFORE ANY COVERAGE NUMBER, and that ordering is a requirement
# rather than an accident of layout: a coverage figure never substitutes for a passing
# suite.  Every invocation is run and held to its four checks BEFORE a single counter is
# captured, and the first one that fails ends the run with nothing captured at all -- so
# there is no arrangement of these steps in which a coverage report can be produced from a
# red or partial matrix.
# The private HOME is created before the FIRST lcov invocation of the run, which is the
# version banner, not the capture: lcov reads a home configuration on every invocation and one
# it cannot parse breaks all of them.
# --restore is a standalone action and short-circuits everything else, so it can never be
# mistaken for part of a measurement run.
# ------------------------------------------------------------------------------------
main() {
    # THE LOADER GUARD HAS ALREADY RUN, at load time, above this file's first function
    # definition -- see DIRECT-OBJECT LOADER VARIABLES ARE REFUSED, NOT FILTERED.  It is not
    # called from here on purpose: by the time main() is entered this script has resolved its
    # own path and probed timeout(1), which is nine loads of a preloaded object rather than
    # two, and a second call here would only repeat a check that has already passed.
    #
    # THE REFERENCE AUDIT RUNS FIRST OF THE THINGS main() DOES, before parsing, before any
    # filesystem write.  A script that cannot resolve one of its own function or variable
    # names must say so and stop, not discover it mid-run after the banner has printed.
    reference_audit

    parse_args "$@"

    if [ "$DO_RESTORE_ONLY" -eq 1 ]; then
        if [ "$DO_BUILD" -eq 1 ] || [ "$DO_RUN" -eq 1 ]; then
            die "--restore is a standalone action; do not combine it with --build or --run."
        fi
        do_restore
        exit 0
    fi

    # MINTED BEFORE THE BANNER so that every line this run prints can be tied back to the
    # artifacts it produced.  Nothing is named after it -- see ONE GENERATION PER DIRECTORY.
    mint_run_id

    rule
    log "HDMI-CEC middleware L1 coverage runner"
    log "  run id         : $RUN_ID"
    log "  submodule root : $HDMICEC_ROOT"
    log "  workspace root : $WS"
    log "  line bar       : ${COVERAGE_MIN}%  (per-file gating: $( [ "$COVERAGE_PER_FILE_GATE" -eq 1 ] && echo on || echo off ))"
    if [ "${#LCOV_CONFIG_ARGS[@]}" -gt 0 ]; then
        log "  lcov config    : $SCRIPT_DIR/.lcovrc_l1 (via --config-file, read instead of /etc/lcovrc; the private HOME has no .lcovrc)"
    else
        log "  lcov config    : none found at $SCRIPT_DIR/.lcovrc_l1; continuing with --rc overrides only."
        warn "without --config-file lcov also reads /etc/lcovrc; the --rc overrides still outrank"
        warn "  it, and no home configuration is reachable either way (private HOME)."
    fi
    log "  branch data    : forced on with --rc branch_coverage=1 on capture, filter, genhtml and summary"

    # Tool PRESENCE first (it needs no lcov invocation), then the private HOME, and only
    # then is lcov asked anything -- including for its version.  Creating the private HOME
    # ahead of every lcov call also covers the counter zeroing inside do_run, which is an
    # lcov invocation like any other and must not see a home configuration either.
    require_tools

    # THE TREE LOCK IS TAKEN HERE: after tool resolution (it needs flock and stat) and before
    # the first thing that touches the tree.  Every stage below it either writes to this tree
    # or reads its .gcda counters, and each of them asserts the lock again on entry rather
    # than assuming this line ran -- see THE TREE LOCK.
    #
    # Taken even when neither --build nor --run was given: a measure-only invocation still
    # reads the counters, and a concurrent run that zeroes them mid-capture produces a trace
    # of a tree nothing exercised.
    acquire_tree_lock "$( [ "$DO_BUILD" -eq 1 ] && printf 'build, ' )$( [ "$DO_RUN" -eq 1 ] && printf 'run the invocation matrix, ' )capture and gate"

    make_private_lcov_home
    log_tool_versions
    prepare_output_dir

    # THE TREE'S IDENTITY IS READ HERE, BEFORE THE BUILD, and that position is the whole of what
    # makes it evidence.  --build's configure step overwrites six git-tracked Makefiles, so an
    # identity read after it describes a tree the caller never had; write_provenance reads them
    # again at the end and reports both, which is how a mid-run change becomes visible instead of
    # being absorbed.  It needs the resolved tools, so it comes after require_tools, and it
    # writes nothing into the tree it reads.
    capture_start_provenance

    if [ "$DO_BUILD" -eq 1 ]; then
        do_build
    fi
    require_instrumented_tree
    if [ "$DO_RUN" -eq 1 ]; then
        do_run
    fi

    # Provenance is written BEFORE the report, because it also augments the genhtml title -
    # the mechanism that puts the revision on every page rather than in one detachable file.
    write_provenance

    capture_coverage
    filter_coverage
    filter_dependency_headers
    generate_html
    summarise_coverage
    per_file_report
    # The branch-arm gate runs BEFORE apply_gate and records rather than exits, so that
    # apply_gate remains the single place a verdict is issued.  By this point every invocation
    # has already run and passed its four checks -- a red matrix never reaches either gate.
    apply_branch_gate
    apply_gate
}

main "$@"
