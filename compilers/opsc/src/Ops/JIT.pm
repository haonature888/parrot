#! parrot-nqp

=begin Description

LLVM JITter?

=end Description

class Ops::JIT;

# Ops::OpsFile
has $!ops_file;
has %!ops;

# OpLib
has $!oplib;

# Packfile used.
has $!packfile;
has $!constants;
has $!bytecode;
has $!opmap;

# LLVM stuff
has $!module;
has $!builder;

has $!interp_struct_type;
has $!opcode_ptr_type;

=item new
Create new JITter for given PBC and OpsFile.

method new(Str $pbc, Ops::File $ops_file, OpLib $oplib) {
    $!ops_file := $ops_file;
    $!oplib    := $oplib;

    # Generate lookup hash by opname.
    for $ops_file.ops -> $op {
        Ops::Util::strip_source($op);
        %!ops{$op.full_name} := $op;
    };

    self._load_pbc($pbc);

    self._init_llvm();

    self;
}

=item load_pbc
Load PBC file.

method _load_pbc(Str $file) {
    # Load PBC into memory
    my $handle   := open($file, :r, :bin);
    my $contents := $handle.readall;
    $handle.close();

    $!packfile := pir::new('Packfile');
    $!packfile.unpack($contents);

    # Find Bytecode and Constants segments.
    my $dir := $!packfile.get_directory() // die("Couldn't find directory in Packfile");

    # Types:
    # 2 Constants.
    # 3 Bytecode.
    # 4 DEBUG
    for $dir -> $it {
        #say("Segment { $it.key } => { $it.value.type }");
        my $segment := $it.value;
        if $segment.type == 2 {
            $!constants := $segment;
        }
        elsif $segment.type == 3 {
            $!bytecode := $segment;
        }
    }

    # Sanity check
    $!bytecode // die("Couldn't find Bytecode");
    $!constants // die("Couldn't find Constants");

    $!opmap := $!bytecode.opmap() // die("Couldn't load OpMap");
}

=item _init_llvm
Initialize LLVM

method _init_llvm() {
    # t/jit/jitted_ops.bc generated by:
    # ./ops2c -d t/jit/jitted.ops
    # llvm-gcc-4.2 -emit-llvm -O0 -Iinclude -I/usr/include/i486-linux-gnu -o t/jit/jitted_ops.bc -c t/jit/jitted_ops.c
    $!module := LLVM::Module.create('JIT');
    $!module.read("t/jit/jitted_ops.bc") // die("Couldn't read t/jit/jitted_ops.bc");

    $!builder := LLVM::Builder.create();

    # Shortcuts for types
    $!interp_struct_type := $!module.get_type("struct.parrot_interp_t")
                            // die("Couldn't find parrot_interp_t definition");

    $!opcode_ptr_type := LLVM::Type::pointer(LLVM::Type::UINTVAL());

}

=begin Processing

We process bytecode from $start position. $end isn't required because last op
in any sub will be :flow op. E.g. C<returncc> or C<end>.

1. We create new LLVM::Function for jitted bytecode. Including prologue and
epilogue.

2. Iterate over ops and create LLVM::BasicBlock for each. This step is
required for handling "local branches" - C<branch>, C<lt>, etc.

3. Iterate over ops again and jit every op. We can stop early for various
reasons - non-local jump, unhandled construct, etc. 

4. For the rest of non-processed ops we just remove BasicBlocks.

5. Run LLVM optimizer.

6. JIT new function.

7. ... add dynop lib to interp? Store new function in it and alter bytecode?
Any other ways?

8. Invoke function.

=end Processing

=item jit($start)
JIT bytecode starting from C<$start> position.

method jit($start) {

    # jit_context hold state for currently jitting ops.
    my %jit_context := self._create_jit_context($start);

    # Generate JITted function for Sub. I use "functional" approach
    # when functions doesn't have side-effects. This is main reason
    # for having such signature.
    %jit_context := self._create_jitted_function(%jit_context, $start);

    self;
}

method _create_jit_context($start) {
    hash(
        bytecode    => $!bytecode,
        constants   => $!constants,
        start       => $start,
        cur_opcode  => $start,

        basic_blocks => hash(), # offset->basic_block
    );
}

method _create_jitted_function (%jit_context, $start) {

    # Generate JITted function for Sub.
    my $jitted_sub := $!module.add_function(
        "jitted$start",
        $!opcode_ptr_type,
        $!opcode_ptr_type,
        LLVM::Type::pointer($!interp_struct_type),
    );
    %jit_context<jitted_sub> := $jitted_sub;

    #$module.dump();

    # Handle arguments
    my $entry := $jitted_sub.append_basic_block("entry");
    %jit_context<entry> := $entry;

    # Create explicit return
    my $leave := $jitted_sub.append_basic_block("leave");
    %jit_context<leave> := $leave;

    # Handle args.
    $!builder.set_position($entry);

    my $cur_opcode := $jitted_sub.param(0);
    $cur_opcode.name("cur_opcode");
    my $cur_opcode_addr := $!builder.store(
        $cur_opcode,
        $!builder.alloca($cur_opcode.typeof()).name("cur_opcode_addr")
    );
    %jit_context<cur_opcode_addr> := $cur_opcode_addr;

    my $interp := $jitted_sub.param(1);
    $interp.name("interp");
    my $interp_addr := $!builder.store(
        $interp,
        $!builder.alloca($interp.typeof()).name("interp_addr")
    );
    %jit_context<interp_addr> := $interp_addr;

    # Few helper values
    my $retval := $!builder.alloca($!opcode_ptr_type).name("retval");
    %jit_context<retval> := $retval;

    my $cur_ctx := $!builder.struct_gep($interp, 0, "CUR_CTX");
    %jit_context<cur_ctx> := $cur_ctx;

    # Load current context from interp

    # Create default return.
    $!builder.set_position($leave);
    $!builder.ret(
        $!builder.load($retval)
    );

    %jit_context;
}

method _create_basic_blocks(%jit_context) {

    my $bc          := %jit_context<bytecode>;
    my $entry       := %jit_context<entry>;
    my $leave       := %jit_context<leave>;
    my $i           := %jit_context<start>;
    my $total       := +$bc - %jit_context<start>;
    my $keep_going  := 1;

    # Enumerate ops and create BasicBlock for each.
    while $keep_going && ($i < $total) {
        # Mapped op
        my $id     := $bc[$i];

        # Real opname
        my $opname := $!opmap[$id];

        # Get op
        my $op     := $!oplib{$opname};

        say("# $opname");
        my $bb := $leave.insert_before("L$i");
        %jit_context<basic_blocks>{$i} := hash(
            label => "L$i",
            bb    => $bb,
        );

        # Next op
        $i := $i + _opsize(%jit_context, $op);

        $keep_going   := self._keep_going($opname);

        #say("# keep_going $keep_going { $parsed_op<flags>.keys.join(',') }");
    }

    # Branch from "entry" BB to next one.
    $!builder.set_position($entry);
    $!builder.br($entry.next);

    %jit_context;
}

=item _count_args
Calculate number of op's args. In most cases it's predefined for op. For 4
exceptions C<set_args>, C<get_results>, C<get_params>, C<set_returns> we have
to calculate it in run-time..

sub _opsize(%jit_context, $op) {
    die("NYI") if _op_is_special(~$op);

    1 + Q:PIR {
        .local pmc op
        .local int s
        find_lex op, '$op'
        s = elements op
        %r = box s
    };
}

=item _op_is_special
Check that op has vaiable length

sub _op_is_special($name) {
    $name eq 'set_args_pc'
    || $name eq 'get_results_pc'
    || $name eq 'get_params_pc'
    || $name eq 'set_returns_pc';
}

=item _keep_going
Should we continue processing ops? If this is non-special :flow op - stop now.

method _keep_going($opname) {
    # If this is non-special :flow op - stop.
    my $parsed_op := %!ops{ $opname };
    $parsed_op && ( _op_is_special($opname) || !$parsed_op<flags><flow> );
}

=item process(Ops::Op, %c) -> Bool.
Process single Op. Return false if we should stop JITting. Dies if can't handle op.
We stop on :flow ops because PCC will interrupt "C" flow and our PCC is way too
complext to implement it in JITter.

method process(Ops::Op $op, %c) {
    self.process($_, %c) for @($op);
}

# Recursively process body chunks returning string.
our multi method process(PAST::Val $val, %c) {
    die('!!!');
}

our multi method process(PAST::Var $var, %c) {
    die('!!!');
}

=item process(PAST::Op)
Dispatch deeper.

our multi method process(PAST::Op $chunk, %c) {
    my $type := $chunk.pasttype // 'undef';
    my $sub  := pir::find_sub_not_null__ps('process:pasttype<' ~ $type ~ '>');
    $sub(self, $chunk, %c);
}

our method process:pasttype<inline> (PAST::Op $chunk, %c) {
    die("Can't handle 'inline' chunks");
}

our method process:pasttype<macro> (PAST::Op $chunk, %c) {
}

our method process:pasttype<macro_define> (PAST::Op $chunk, %c) {
}


our method process:pasttype<macro_if> (PAST::Op $chunk, %c) {
}

our method process:pasttype<call> (PAST::Op $chunk, %c) {
}

our method process:pasttype<if> (PAST::Op $chunk, %c) {
}

our method process:pasttype<while> (PAST::Op $chunk, %c) {
}

our method process:pasttype<do-while> (PAST::Op $chunk, %c) {
}

our method process:pasttype<for> (PAST::Op $chunk, %c) {
}

our method process:pasttype<switch> (PAST::Op $chunk, %c) {
}

our method process:pasttype<undef> (PAST::Op $chunk, %c) {
}

our multi method process(PAST::Stmts $chunk, %c) {
}

our multi method process(PAST::Block $chunk, %c) {
}

our multi method process(String $str, %c) {
}


INIT {
    pir::load_bytecode("LLVM.pbc");
    pir::load_bytecode("nqp-setting.pbc");
}
# vim: ft=perl6
