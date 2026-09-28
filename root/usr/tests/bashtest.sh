# bashtest.sh - GNU bash on Genesis, non-interactively (ROADMAP item 15).
#
# Run as `/bin/bash /usr/tests/bashtest.sh` (tools/guest_run.py does). Every
# check here is something bash does through the kernel - fork, wait4 for a
# particular pid, signals and their exit statuses, pipes, rlimits, select -
# rather than something bash could answer from its own memory. It ends with
# the same tally line as the other suites.
#
# There are no utilities on the volume yet (no cat, no sleep), so everything
# here is a builtin or bash itself. The interactive half - readline, raw
# mode, Ctrl-C - is not covered by this script; see tools/guest_sh.py.

passed=0
failed=0

check() {   # check "description" test-args...
    local what=$1
    shift
    if "$@"; then
        passed=$((passed + 1))
        echo "  ok    $what"
    else
        failed=$((failed + 1))
        echo "  FAIL  $what"
    fi
}

eq() { [ "$1" = "$2" ]; }

echo "bashtest: GNU bash $BASH_VERSION"

# --- the language, briefly: if this fails nothing below means anything ------
x=$((6 * 7))
check "arithmetic" eq "$x" 42
arr=(a b c)
check "arrays" eq "${#arr[@]}:${arr[1]}" "3:b"
s="hello world"
check "parameter expansion" eq "${s%% *}-${s#* }" "hello-world"
f() { echo "f:$1"; }
check "functions" eq "$(f arg)" "f:arg"

# --- processes ----------------------------------------------------------------
check "command substitution forks and collects output" eq "$(echo sub)" "sub"
check "nested substitution" eq "$(echo "$(echo in)")" "in"
(exit 7)
check "a subshell's exit status comes back through wait4" eq "$?" 7
(exit 3) &
p1=$!
(exit 5) &
p2=$!
wait "$p2"
r2=$?
wait "$p1"
r1=$?
check "wait for a PARTICULAR child gets that child's status" eq "$r2:$r1" "5:3"
check "\$! and \$\$ are real pids" test "$p1" -gt 1 -a "$$" -gt 1

# --- signals --------------------------------------------------------------------
( kill -TERM $BASHPID; exit 0 )
check "a child killed by SIGTERM reports 128+15" eq "$?" 143
( trap 'exit 42' USR1; kill -USR1 $BASHPID; exit 1 )
check "a trap runs in the child that caught the signal" eq "$?" 42
check "kill -0 probes a live pid" kill -0 $$

# --- pipes and redirection -----------------------------------------------------
v=$(echo piped | { read -r line; echo "$line"; })
check "a pipeline between two bash processes" eq "$v" "piped"
read -r a b <<EOF
here doc
EOF
check "a here-document into read" eq "$a:$b" "here:doc"
read -r w <<< "herestring"
check "a here-string" eq "$w" "herestring"
check "PIPESTATUS sees every stage" eq "$(false | true; echo "${PIPESTATUS[*]}")" "1 0"

# --- limits, time, the terminal-less paths -----------------------------------
check "ulimit -n is the real descriptor limit" eq "$(ulimit -n)" 32
( ulimit -n 8; exec 9>&1 ) 2>/dev/null
check "and lowering it is enforced (fd 9 refused)" test "$?" -ne 0
read -r -t 0.2 nothing < /dev/console
check "read -t times out (select) with status > 128" test "$?" -gt 128
t=$(times)
check "times reports something" test -n "$t"

echo "bashtest: $passed passed, $failed failed"
