#!/usr/bin/env python3
"""gen.py <cuda include dir> <exports list>: the forwarding code for the CUDA driver API, generated into gen/.

The functions are those cudaTypedefs.h has a PFN_<name>_v<version>[_ptds|_ptsz] typedef for, under the name the
driver exports each as (the exports list: what the host's cuGetProcAddress resolves every PFN to, probe/gpa.c; e.g.
cuMemAlloc v3020 -> cuMemAlloc_v2). Each parameter travels by its kind:

  value       scalars, handles and structs by value (the bytes as they are: Windows x64 and Linux x86-64 lay out
              the API's types alike, they use no long)
  in, out     one object behind a pointer (const: in), inout both ways
  string      a NUL-terminated input string
  in[n] out[n] arrays of n elements (bytes for void); n is another parameter, or *p for the value p points to
  manual      anything else: void pointers, pointers to pointers, callbacks, and structs that carry pointers (a guest
              address the host would follow). Those functions are hand-written (guest/nvcuda.c, host/server.c) or
              answer CUDA_ERROR_NOT_SUPPORTED and log their name, so programs show what to add next

Functions that take a count next to a pointer are only forwarded once RULES says what their pointers are (the
pointer-is-one-object default is wrong for arrays). The headers (NVIDIA's cuda_cudart redistributable, include/)
are read here and compiled against, not kept in the repository."""
import json, os, re, subprocess, sys, tempfile
from pycparser import c_parser, c_ast, c_generator

# per-function parameter kinds where the defaults don't hold (parameter name -> kind)
RULES = {
    'cuDeviceGetName': {'name': 'out[len]'},
    'cuDeviceGetPCIBusId': {'pciBusId': 'out[len]'},
    'cuDeviceGetCount': {},
    'cuFuncGetParamCount': {},
    'cuKernelGetParamCount': {},
    'cuModuleGetFunctionCount': {},
    'cuLibraryGetKernelCount': {},
    'cuDeviceGetTexture1DLinearMaxWidth': {},
    'cuOccupancyAvailableDynamicSMemPerBlock': {},
    'cuOccupancyMaxActiveBlocksPerMultiprocessor': {},
    'cuOccupancyMaxActiveBlocksPerMultiprocessorWithFlags': {},
    'cuMipmappedArrayCreate': {},
    'cuModuleEnumerateFunctions': {'functions': 'out[numFunctions]'},
    'cuLibraryEnumerateKernels': {'kernels': 'out[numKernels]'},
    'cuMemSetAccess': {'desc': 'in[count]'},
    'cuMemPoolSetAccess': {'map': 'in[count]'},
    'cuGraphGetNodes': {'nodes': 'out[*numNodes]', 'numNodes': 'inout'},
    'cuGraphGetRootNodes': {'rootNodes': 'out[*numRootNodes]', 'numRootNodes': 'inout'},
    'cuGraphAddEmptyNode': {'dependencies': 'in[numDependencies]'},
    'cuGraphAddChildGraphNode': {'dependencies': 'in[numDependencies]'},
    'cuGraphAddEventRecordNode': {'dependencies': 'in[numDependencies]'},
    'cuGraphAddEventWaitNode': {'dependencies': 'in[numDependencies]'},
    'cuGraphAddMemFreeNode': {'dependencies': 'in[numDependencies]'},
    'cuGraphAddMemsetNode': {'dependencies': 'in[numDependencies]'},
    'cuGraphAddMemAllocNode': {'dependencies': 'in[numDependencies]', 'nodeParams': 'inout'},
    'cuGraphAddDependencies': {'from': 'in[numDependencies]', 'to': 'in[numDependencies]'},
    'cuGraphRemoveDependencies': {'from': 'in[numDependencies]', 'to': 'in[numDependencies]'},
    'cuGraphicsMapResources': {'resources': 'in[count]'},
    'cuGraphicsUnmapResources': {'resources': 'in[count]'},
}
for n in list(RULES):
    RULES.setdefault(n + '_ptsz', RULES[n])
    RULES.setdefault(n + '_ptds', RULES[n])
# copies between guest memory and the device: pageable host memory makes the async ones return once the source is
# staged (to the device) or the copy is done (from it), so the bytes can travel with the call either way
for n, rule in {'cuMemcpyHtoD': {'srcHost': 'in[ByteCount]'}, 'cuMemcpyDtoH': {'dstHost': 'out[ByteCount]'},
                'cuMemcpyHtoA': {'srcHost': 'in[ByteCount]'}, 'cuMemcpyAtoH': {'dstHost': 'out[ByteCount]'}}.items():
    for v in ('', '_v2'):
        for a in ('', 'Async'):
            for f in ('', '_ptds', '_ptsz'):
                RULES[n + a + v + f] = rule
COUNTED = re.compile(r'^(num|count)|(Count|count|Num)$')

inc, exports = sys.argv[1], sys.argv[2]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gen')
os.makedirs(out, exist_ok=True)
stub = tempfile.mkdtemp()
open(os.path.join(stub, 'stdlib.h'), 'w').write('typedef unsigned long size_t;\n')
open(os.path.join(stub, 'stdint.h'), 'w').write(
    'typedef unsigned char uint8_t; typedef unsigned short uint16_t; typedef unsigned int uint32_t;'
    ' typedef unsigned long long uint64_t; typedef long long int64_t; typedef int int32_t;\n')
src = subprocess.run(['gcc', '-E', '-P', '-nostdinc', '-I', stub, '-I', inc, '-D__attribute__(x)=',
                      '-D__extension__=', os.path.join(inc, 'cudaTypedefs.h')],
                     check=True, capture_output=True, text=True).stdout
ast = c_parser.CParser().parse(src)
cgen = c_generator.CGenerator()

typedefs, structs = {}, {}
class Structs(c_ast.NodeVisitor):
    def visit_Struct(self, n):
        if n.decls is not None and n.name:
            structs[n.name] = n
        self.generic_visit(n)
    visit_Union = visit_Struct
Structs().visit(ast)
for n in ast.ext:
    if isinstance(n, c_ast.Typedef):
        typedefs[n.name] = n.type

def resolve(t):
    while isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.IdentifierType) and t.type.names[-1] in typedefs:
        t = typedefs[t.type.names[-1]]
    return t

def carries_pointer(t, seen=()):
    """whether a type holds an address the host would follow (handles, pointers to opaque structs, don't count)"""
    t = resolve(t)
    if isinstance(t, c_ast.PtrDecl):
        p = resolve(t.type)
        if isinstance(p, c_ast.TypeDecl) and isinstance(p.type, (c_ast.Struct, c_ast.Union)):
            return p.type.name in structs and bool(structs[p.type.name].decls)
        return True
    if isinstance(t, c_ast.ArrayDecl):
        return carries_pointer(t.type, seen)
    if isinstance(t, c_ast.TypeDecl) and isinstance(t.type, (c_ast.Struct, c_ast.Union)):
        s = t.type if t.type.decls is not None else structs.get(t.type.name)
        if s is None or s.name in seen:
            return False
        return any(carries_pointer(d.type, seen + (s.name,)) for d in s.decls or [])
    return False

def ctype(t, name=''):
    return cgen.visit(c_ast.Decl(name, [], [], [], [], t, None, None)).strip()

def kind(t):
    if not isinstance(t, c_ast.PtrDecl):
        return 'manual' if carries_pointer(t) else 'value'
    target = t.type
    const = 'const' in getattr(target, 'quals', [])
    if isinstance(target, c_ast.TypeDecl) and isinstance(target.type, c_ast.IdentifierType):
        if target.type.names[-1] == 'char':
            return 'string' if const else 'manual'
        if target.type.names[-1] == 'void' or isinstance(resolve(target), c_ast.FuncDecl):
            return 'manual'
        if carries_pointer(target):
            return 'manual'
        return 'in' if const else 'out'
    return 'manual'

def base(t):
    """the pointed-to type without const, for temporaries"""
    import copy
    b = copy.deepcopy(t.type)
    b.quals = [q for q in b.quals if q != 'const']
    return ctype(b)

mapping = {}
for line in open(exports):
    m = re.match(r'(\w+) v(\d+) flags (\d+) -> (\w+)', line)
    if m:
        mapping[(m.group(1), int(m.group(2)), m.group(3))] = m.group(4)

api = {}
for n in ast.ext:
    if not isinstance(n, c_ast.Typedef):
        continue
    m = re.match(r'PFN_(cu\w+?)_v(\d+)(_ptds|_ptsz)?$', n.name)
    if not m or not isinstance(n.type.type, c_ast.FuncDecl):
        continue
    sym = mapping.get((m.group(1), int(m.group(2)), '2' if m.group(3) else '0'))
    if not sym:
        continue
    fd = n.type.type
    params = []
    for i, p in enumerate(fd.args.params if fd.args else []):
        if isinstance(p, c_ast.Typename) and ctype(p.type) == 'void':
            continue
        name = p.name or f'a{i}'
        q = {'name': name, 'decl': ctype(p.type, name), 'kind': kind(p.type)}
        if isinstance(p.type, c_ast.PtrDecl):
            q['base'] = base(p.type)
        params.append(q)
    rule = RULES.get(sym)
    if rule is None and any(COUNTED.search(q['name']) for q in params) and \
            any(q['kind'] in ('in', 'out') for q in params):
        rule = 'unreviewed'
    if isinstance(rule, dict):
        for q in params:
            if q['name'] in rule:
                q['kind'] = rule[q['name']]
    api[sym] = {'pfn': n.name, 'params': params,
                'forward': rule != 'unreviewed' and all(q['kind'] != 'manual' for q in params)}
api = dict(sorted(api.items()))
json.dump(api, open(os.path.join(out, 'api.json'), 'w'), indent=1)

# hand-written in guest/nvcuda.c and host/server.c (gen skips them)
MANUAL = set(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'manual.txt')).read().split())

def arr(k):
    m = re.match(r'(in|out)\[(\*?)(\w+)\]$', k)
    return m and (m.group(1), m.group(2) == '*', m.group(3))

def count_expr(deref, n):
    return f'({n} ? (uint64_t)*{n} : 0)' if deref else f'(uint64_t){n}'

guest, host, table, ids = [], [], [], []
fid = 0
for sym, f in api.items():
    if sym in MANUAL:
        continue
    params = f['params']
    sig = ', '.join(q['decl'] for q in params) or 'void'
    if not f['forward']:
        guest.append(f'#undef {sym}\nCUresult CUDAAPI {sym}({sig})\n{{\n'
                     + ''.join(f'   (void){q["name"]};\n' for q in params)
                     + f'   return fwd_unsupported("{sym}");\n}}\n')
        continue
    fid += 1
    ids.append(f'#define FN_{sym} {fid}')
    g = [f'#undef {sym}\nCUresult CUDAAPI {sym}({sig})\n{{\n   fwd_call c;\n   fwd_begin(&c, FN_{sym});']
    h = [f'static CUresult h_{sym}(rbuf *in, wbuf *out)\n{{']
    call, after_g, after_h = [], [], []
    for q in params:
        n, k = q['name'], q['kind']
        a = arr(k)
        if k == 'value':
            g.append(f'   w_put(&c.w, &{n}, sizeof {n});')
            h.append(f'   {q["decl"]};\n   r_get(in, &{n}, sizeof {n});')
            call.append(n)
        elif k == 'string':
            g.append(f'   w_str(&c.w, {n});')
            h.append(f'   const char *{n} = r_str(in);')
            call.append(n)
        elif k in ('in', 'inout'):
            g.append(f'   w_obj(&c.w, {n}, sizeof *{n});')
            h.append(f'   {q["base"]} t_{n}, *{n} = r_obj(in, &t_{n}, sizeof t_{n});')
            call.append(n)
            if k == 'inout':
                after_g.append(f'   r_obj(&c.r, {n}, sizeof *{n});')
                after_h.append(f'   w_obj(out, {n}, sizeof *{n});')
        elif k == 'out':
            g.append(f'   w_flag(&c.w, {n} != NULL);')
            h.append(f'   {q["base"]} t_{n}, *{n} = r_flag(in) ? &t_{n} : NULL;')
            call.append(n)
            after_g.append(f'   if (r == CUDA_SUCCESS) r_obj(&c.r, {n}, sizeof *{n});')
            after_h.append(f'   if (r == CUDA_SUCCESS) w_obj(out, {n}, sizeof *{n});')
        elif a:
            d, deref, cnt = a
            esz = '1' if q['base'] == 'void' else f'sizeof *{n}'
            g.append(f'   uint64_t n_{n} = {count_expr(deref, cnt)};')
            if d == 'in':
                g.append(f'   w_arr(&c.w, {n}, n_{n}, {esz});')
                h.append(f'   uint64_t n_{n}; {q["base"]} *{n} = ({q["base"]} *)r_arr(in, &n_{n}, {esz});')
            else:
                g.append(f'   w_flag(&c.w, {n} != NULL); w_put(&c.w, &n_{n}, 8);')
                h.append(f'   uint64_t n_{n}; {q["base"]} *{n} = ({q["base"]} *)r_outarr(in, &n_{n}, {esz});')
                after_g.append(f'   if (r == CUDA_SUCCESS && {n}) r_get(&c.r, {n}, n_{n} * {esz});')
                after_h.append(f'   if (r == CUDA_SUCCESS && {n}) w_put(out, {n}, n_{n} * {esz});')
            call.append(n)
        else:
            raise SystemExit(f'{sym}: {n}: kind {k}')
    g.append('   CUresult r = fwd_exchange(&c);')
    g += after_g
    g.append('   fwd_end(&c);\n   return r;\n}\n')
    h.append(f'   if (in->bad) return CUDA_ERROR_INVALID_VALUE;\n   CUresult r = real.{sym}({", ".join(call)});')
    h += after_h
    h.append('   return r;\n}\n')
    guest.append('\n'.join(g))
    host.append('\n'.join(h))
    table.append(f'   [FN_{sym}] = h_{sym},')

hdr = '/* generated by gen.py from the CUDA headers: do not edit */\n'
open(os.path.join(out, 'ids.h'), 'w').write(hdr + '\n'.join(ids) + f'\n#define FN_GENERATED_COUNT {fid + 1}\n')
open(os.path.join(out, 'guest_stubs.c'), 'w').write(hdr + '\n'.join(guest))
reals = '\n'.join(f'   {f["pfn"]} {s};' for s, f in api.items() if f['forward'] or s in MANUAL)
loads = '\n'.join(f'   real.{s} = ({f["pfn"]})dlsym(lib, "{s}");' for s, f in api.items() if f['forward'] or s in MANUAL)
open(os.path.join(out, 'host_real.h'), 'w').write(
    hdr + f'static struct {{\n{reals}\n}} real;\n\nstatic void real_load(void *lib)\n{{\n{loads}\n}}\n')
open(os.path.join(out, 'host_dispatch.c'), 'w').write(
    hdr + '\n'.join(host) + '\nstatic CUresult (*const dispatch[FN_GENERATED_COUNT])(rbuf *, wbuf *) = {\n'
    + '\n'.join(table) + '\n};\n')
open(os.path.join(out, 'nvcuda.def'), 'w').write(
    'LIBRARY nvcuda.dll\nEXPORTS\n' + ''.join(f'   {s}\n' for s in api) + ''.join(
        f'   {s}\n' for s in sorted(MANUAL - set(api))))
print(f'{len(api)} functions: {fid} forwarded, {sum(1 for s in api if s in MANUAL)} hand-written, '
      f'{sum(1 for s, f in api.items() if not f["forward"] and s not in MANUAL)} not supported')
