# Mosh Tests

## ocb-aes

This is a unit test for the OCB-AES encryption used in mosh, including
Rogaway's OCB implementation and some of mosh's surrounding C++
support code.

## encrypt-decrypt

This is a simple functional test of mosh's implementation of encrypted
messages.

## base64

This tests Mosh's homegrown base64 functionality.  The associated
`genbase64.pl` script is used to independently generate validated test
vectors.

## e2e-test

This is a test framework for end-to-end testing of mosh.  It uses tmux
to invoke mosh in a nicely stable interactive pty, and also uses
tmux's `capture-pane` command to get a dump of the terminal screen
that mosh-client has drawn, neatly getting around Mosh's somewhat
non-deterministic display redraw.

There are four essential parts to the framework:

* your test script
* `e2e-test`
* `e2e-test-server`
* `e2e-test-subrs`

The test script has two roles: when invoked without arguments, it is a
wrapper script for the overall test, and when invoked with an
argument, it performs a testing-related action.  In wrapper mode, it
invokes e2e-test with action arguments, which are used to invoke the
test script for actions at appropriate points by e2e-test.  These
provide a suite of behaviors that you can use to test various mosh
behaviors.

`e2e-test` is the heart of the framework.  It runs actions as
requested, logs their output, compares and/or validates their results,
and generates the final result (exitstatus, mostly) for the Automake
testing framework used by the mosh build.  For test execution, it runs
an action in an interactive session, in a tmux `screen`, to exercise
some behavior.  The action can optionally be run in a mosh session, or
directly in tmux (doing both and comparing the result is a useful way
to test complex terminal emulation behaviors).  The action generally
writes some output to the terminal that can later be verified by
another action.  Optionally, a client action can generate tty input or
otherwise exercise mosh in some fashion (this capability is untested,
but it's a useful place to use `expect` or other interactive
simulations).  The action is run by `e2e-test-server`, which is a
relatively small wrapper script to capture errors, and capture the
tmux screen.

There are several different categories of actions:

### Execution

`baseline` is an action that almost all tests will use.  This invokes
the test script inside mosh, where it can generate some output, and
then captures the client-side tmux display with `tmux capture-pane`.

`direct` is the same as the above, except that mosh is not used--
`e2e-wrapper-script` and the test script are invoked directly inside
tmux.

`variant` can be used to provide a slightly different action from
`baseline`.

### Verification

`verify` compares captures from the `baseline` and `direct` test
actions, which are expected to be identical.

`same` compares captures from the `baseline` and `variant` test
actions, which are expected to be identical.

`different` compares captures from the `baseline` and `variant` test
actions, which are expected to be different.

`post` is a catchall script hook which allows custom verification
actions to be coded.

### Client wrappers

`tmux` injects a wrapper command into the test command before tmux.
If this is not run, a default command called `hold-stdin` is run
instead.  These commands are expected to hold tmux's stdin open,
possibly injecting tmux commands, while the test runs.  See
`window-resize.test` for an example of this that manipulates tmux
state.  Alternately, this could use expect or something similar.

`client` simply injects a wrapper command into the (long) test command
between tmux and mosh.  It's expected to interact with its wrapped
command line as `expect` might do.  This is not actually tested yet.

### Flags

`mosh-args`, `client-args` and `server-args` inject extra arguments
into the invocations of the respective commands.

## Logging and error reporting

Each execution action is run, and recorded in
`<testname>.test.d/<action>.*`. `<action>.exitstatus` is the
exitstatus from the server wrapper.  `<action>.tmux.log` is the output
of tmux for the entire test run for that action; `<action>.capture` is
a capture of the Mosh client screen after the test action is complete,
generated with `tmux capture-pane`.

In accordance with GNU Automake's test framework, the test should
return these exit status values:

* 0 test success
* 1 test failure
* 77 test skipped (tmux or ssh is unavailable if needed)
* 99 hard error

These values are also used internally between the various scripts;
errors are conveyed out to the build test framework.


## Sample tests

A few tests have been implemented so far to test the framework itself,
and to provide examples for further development.

`e2e-success` is a simple test that executes `baseline` and `direct`
with the same stimulus (simply clearing the screen), and expects to
see identical results.

`e2e-failure` is similar to `e2e-success`, but expects to see
different results from `baseline` and `variant`.  Since it uses the
same stimulus for the two execution action, it fails.  A more
realistic test might be to have `variant` execute some escape sequence
that is absent from `baseline`; this would verify that the escape
sequence actually does something.

`emulation-back-tab` tests an escape sequence that mosh does not
support.  It expects the test to produce the output that would be
generated if the escape sequence were implemented.  If it gets output
as expected when the escape sequence is *not* implemented, the test
fails.  But if the output does not match one of these two cases, the
test returns an error.  This is an example of error handling within
the test framework.

`unicode-later-combining` demonstrates mosh's handling of a Unicode
edge case, a combining character drawn without a printing character in
the same cell.  It verifies the output in the `post` action; since
there are a couple of different Unicode renderings that are reasonable
in this case, a regex that covers both is used.  It also implements an
unused `variant` action that draws blank-space+combiner in a correct
fashion.

## Notes

The shell command `printf` is generally used in place of
`echo` in this framework, because of its more precisely-specified and
portable behavior.  But beware, even `printf` varies between systems--
GNU printf, for example, implements `\e`, which is a non-POSIX
extension unavailable in BSD implementations

It's fairly simple to test each of these scripts independently, but
the entire chain is a bit prone to behaving oddly in hard-to-debug
ways.  `set -x` is your friend here.

The test scripts are a bit fragile about timeouts.  They will
generally run correctly on an unloaded machine without the `make -j`
flag.  Using `make -j` is obviously very convenient for development,
and it works fine on faster machines, but I don't recommend it for
automated testing.
