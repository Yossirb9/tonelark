set pagination off
set confirm off
handle SIGSEGV stop print
run
bt 30
thread apply all bt 8
quit
