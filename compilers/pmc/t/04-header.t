#!parrot
# Check generating header for parsed PMC

.include 'compilers/pmc/t/common.pir'

.sub 'main' :main
.include 'test_more.pir'
load_bytecode 'compilers/pmc/pmc.pbc'
    .local int total

    plan(1)

    .local string filename
    filename = 'compilers/pmc/t/data/class00.pmc'
    $S0 = _slurp(filename)
    check_one_header(filename, $S0, "'DO NOT EDIT THIS FILE'", "Warning generated")

.end

# Check genrated header.
# Parse passed string, generate header, check against supplied pattern
.sub 'check_one_header'
    .param string name
    .param string source
    .param string pattern
    .param string message

    .local pmc compiler
    compiler = compreg 'PMC'
    $P0 = compiler.'compile'(source, 'target'=>'past')

    .local pmc emitter
    $P1 = split '::', 'PMC::Emitter'
    emitter = new $P1
    emitter.'set_filename'(name)
    $S0 = emitter.'generate_h_file'($P0)
    like($S0, pattern, message)
.end

# Don't forget to update plan!

# Local Variables:
#   mode: cperl
#   cperl-indent-level: 4
#   fill-column: 100
# End:
# vim: expandtab shiftwidth=4 ft=pir:
