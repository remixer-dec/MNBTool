#!/usr/bin/env python3
"""Generate ARM64 game-hook bindings and the game-call table from the original ARMv7
compiler graph.

Distributed with the SDK. Requires Python 3.9+ and NDK Clang, never a framework
checkout, PORT_ENGINE installation, mod catalogue or author-written adapter.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import sys

from _compiler.sr_mod_object import ModObjectError, compiler_roots
from _compiler.sr_mod_sdk_hooks import (apply_edits, extract_public_calls, extract_public_hooks,
                                        public_adapter_view, public_call_table, public_game_pointers)
from _compiler.sr_mod_types import PROBE, resolve_signatures


def generate(compiler, source, output, flags):
    source = source.resolve()
    original = source.read_bytes()
    # The script supplies ABI-neutral compilation flags; the source contract
    # always comes from ARMv7 base AAPCS, including soft-float arguments.
    common = [*flags, '-DMF_TRANSLATED_ARM64=1', '-I'+str(source.parent), '-fsyntax-only', '-Xclang', '-ast-dump=json']
    # Keep the target after user flags so extra flags cannot change the source
    # contract into LP64 or hard-float while still labeling it ARMv7 base AAPCS.
    command = [compiler, *common, '--target=armv7a-linux-androideabi24', '-mfloat-abi=softfp', '-marm']
    native_command = [compiler, *common, '--target=aarch64-linux-android24', '-mno-outline-atomics']

    def graph(path, *, native=False):
        result = subprocess.run([*(native_command if native else command), str(path)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            sys.stderr.buffer.write(result.stderr)
            raise ModObjectError(('ARM64' if native else 'ARMV7')+'_COMPILER_CONTRACT_FAILED')
        roots = compiler_roots(result.stdout.decode())
        if len(roots) != 1 or roots[0].get('kind') != 'TranslationUnitDecl':
            raise ModObjectError('COMPLETE_PACKAGE_COMPILER_GRAPH_REQUIRED')
        return roots

    def validate_native():
        # Target-conditioned source must not introduce a raw ARM64 game hook
        # absent from the ARMv7 graph or register an unrewritten branch.
        native_hooks, _ = extract_public_hooks(graph(output, native=True)[0])
        if native_hooks:
            raise ModObjectError('ARM64_GAME_HOOK_WITHOUT_SOURCE_BINDING')

    roots = graph(source)
    cpp = source.suffix != '.c'
    hooks, _ = extract_public_hooks(roots[0])
    calls = extract_public_calls(roots[0])
    table = public_call_table(calls, cpp) if calls else b''
    calls = [dict(call, kind='game_call') for call in calls]
    # Game addresses converted to pointers, in any package file: the headers are
    # then compiled from copies next to the output, all of them so each is included once.
    files, pointers = public_game_pointers(roots[0], source, cpp)
    calls += [dict(kind='game_pointer', file=str(path.relative_to(source.parent)), offset=offset)
              for path, offset in pointers]
    edits = files.pop(source, {})
    if edits or any(files.values()):
        for path, changes in files.items():
            copy = output.parent/path.relative_to(source.parent)
            copy.parent.mkdir(parents=True, exist_ok=True)
            copy.write_bytes(apply_edits(path.read_bytes(), changes))
    if not hooks:
        output.write_bytes(apply_edits(original, edits)+table)
        validate_native()
        return calls
    if source.suffix == '.c':
        raise ModObjectError('AUTOMATIC_GAME_HOOKS_REQUIRE_CPP17; rename mod.c to mod.cpp')
    requests = [(hook['identifier'], 'decltype(&'+hook['name']+')') for hook in hooks]
    probe = '\nnamespace '+PROBE+'_source {\n'+''.join(
        'using type_'+str(index)+' = '+expression+';\n' for index, (_, expression) in enumerate(requests))+'}\n'
    # Same include origin as the original file; quoted package headers work
    # without copying or editing any author files.
    with tempfile.TemporaryDirectory(prefix='mf-contract-', dir=output.parent) as scratch:
        probe_path = Path(scratch)/'probe.cpp'
        probe_path.write_bytes(original+probe.encode())
        abi = resolve_signatures(graph(probe_path), requests, source_model=True, require_sdk_definitions=False)
    view, contracts = public_adapter_view(original, roots[0], str(source), abi, edits)
    output.write_bytes(view+table)
    validate_native()
    return contracts+calls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('flags', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    flags = args.flags[1:] if args.flags[:1] == ['--'] else args.flags
    try:
        contracts = generate(args.compiler, args.source, args.output, flags)
        args.output.with_suffix('.contracts.json').write_text(json.dumps(contracts, indent=2)+'\n')
    except (ModObjectError, OSError, ValueError) as error:
        args.output.unlink(missing_ok=True)
        raise SystemExit('build-mod: automatic compatibility: '+str(error))
    calls = sum(1 for contract in contracts if contract.get('kind') == 'game_call')
    pointers = sum(1 for contract in contracts if contract.get('kind') == 'game_pointer')
    print('ARMv7 compiler contracts: %d game hooks, %d game calls, %d game address conversions; '
          'generated public-only ARM64 bindings' % (len(contracts)-calls-pointers, calls, pointers))


if __name__ == '__main__': main()
