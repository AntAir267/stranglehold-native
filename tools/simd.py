"""SSE, SSE2 and MMX instructions for the code generator (codegen.FuncGen.insn).

The program was built with /arch:SSE, so scalar SSE does most of its floating-point maths.
The generated C uses the host's own SSE through compiler intrinsics: the host is x86-64, so
every result (rounding, NaNs, the approximate rcp/rsqrt) is what an SSE CPU computes.

Registers: xmm0-7 are __m128 locals x0..x7, MMX mm0-7 are uint64_t locals m0..m7, written back
to the CPU record around calls like the general registers (FuncGen.gen adds them to SAVE/LOAD).
MMX instructions are done on the low half of a 128-bit value with the matching SSE2 intrinsic.
3DNow! is never reached: the CPU the program sees (runtime cpu_cpuid) is an Intel without it.
"""

SS_ARITH = {'addss': '_mm_add_ss', 'subss': '_mm_sub_ss', 'mulss': '_mm_mul_ss',
            'divss': '_mm_div_ss', 'minss': '_mm_min_ss', 'maxss': '_mm_max_ss'}
PS_ARITH = {'addps': '_mm_add_ps', 'subps': '_mm_sub_ps', 'mulps': '_mm_mul_ps',
            'divps': '_mm_div_ps', 'minps': '_mm_min_ps', 'maxps': '_mm_max_ps',
            'andps': '_mm_and_ps', 'andnps': '_mm_andnot_ps', 'orps': '_mm_or_ps',
            'xorps': '_mm_xor_ps', 'unpcklps': '_mm_unpacklo_ps', 'unpckhps': '_mm_unpackhi_ps',
            'movhlps': '_mm_movehl_ps', 'movlhps': '_mm_movelh_ps'}
SS_UNARY = {'sqrtss': '_mm_sqrt_ss', 'rcpss': '_mm_rcp_ss', 'rsqrtss': '_mm_rsqrt_ss'}
PS_UNARY = {'sqrtps': '_mm_sqrt_ps', 'rcpps': '_mm_rcp_ps', 'rsqrtps': '_mm_rsqrt_ps'}
CMP_PRED = ('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord')
# integer SIMD (MMX on mm registers, SSE2 on xmm): lanewise two-operand forms
INT_OPS = {'paddb': '_mm_add_epi8', 'paddw': '_mm_add_epi16', 'paddd': '_mm_add_epi32',
           'paddq': '_mm_add_epi64', 'psubb': '_mm_sub_epi8', 'psubw': '_mm_sub_epi16',
           'psubd': '_mm_sub_epi32', 'psubq': '_mm_sub_epi64',
           'paddsb': '_mm_adds_epi8', 'paddsw': '_mm_adds_epi16', 'paddusb': '_mm_adds_epu8',
           'paddusw': '_mm_adds_epu16', 'psubsb': '_mm_subs_epi8', 'psubsw': '_mm_subs_epi16',
           'psubusb': '_mm_subs_epu8', 'psubusw': '_mm_subs_epu16',
           'pmullw': '_mm_mullo_epi16', 'pmulhw': '_mm_mulhi_epi16', 'pmulhuw': '_mm_mulhi_epu16',
           'pmaddwd': '_mm_madd_epi16', 'pmuludq': '_mm_mul_epu32',
           'pand': '_mm_and_si128', 'pandn': '_mm_andnot_si128', 'por': '_mm_or_si128',
           'pxor': '_mm_xor_si128',
           'pcmpeqb': '_mm_cmpeq_epi8', 'pcmpeqw': '_mm_cmpeq_epi16', 'pcmpeqd': '_mm_cmpeq_epi32',
           'pcmpgtb': '_mm_cmpgt_epi8', 'pcmpgtw': '_mm_cmpgt_epi16', 'pcmpgtd': '_mm_cmpgt_epi32',
           'pmaxsw': '_mm_max_epi16', 'pminsw': '_mm_min_epi16', 'pmaxub': '_mm_max_epu8',
           'pminub': '_mm_min_epu8', 'pavgb': '_mm_avg_epu8', 'pavgw': '_mm_avg_epu16',
           'psadbw': '_mm_sad_epu8',
           'packsswb': '_mm_packs_epi16', 'packssdw': '_mm_packs_epi32', 'packuswb': '_mm_packus_epi16'}
# unpacks: on MMX registers the high forms take the upper half of the low unpack
UNPACK = {'punpcklbw': ('_mm_unpacklo_epi8', 0), 'punpcklwd': ('_mm_unpacklo_epi16', 0),
          'punpckldq': ('_mm_unpacklo_epi32', 0), 'punpckhbw': ('_mm_unpackhi_epi8', 1),
          'punpckhwd': ('_mm_unpackhi_epi16', 1), 'punpckhdq': ('_mm_unpackhi_epi32', 1),
          'punpcklqdq': ('_mm_unpacklo_epi64', 0), 'punpckhqdq': ('_mm_unpackhi_epi64', 1)}
SHIFTS = {'psllw': 'sll_epi16', 'pslld': 'sll_epi32', 'psllq': 'sll_epi64',
          'psrlw': 'srl_epi16', 'psrld': 'srl_epi32', 'psrlq': 'srl_epi64',
          'psraw': 'sra_epi16', 'psrad': 'sra_epi32'}
MNEMONICS = (set(SS_ARITH) | set(PS_ARITH) | set(SS_UNARY) | set(PS_UNARY) | set(INT_OPS) |
             set(UNPACK) | set(SHIFTS) |
             {'movss', 'movaps', 'movups', 'movlps', 'movhps', 'shufps', 'comiss', 'ucomiss',
              'cvtsi2ss', 'cvttss2si', 'cvtss2si', 'cvtpi2ps', 'cvtps2pi', 'cvttps2pi', 'cvtdq2ps',
              'cvttps2dq', 'cvtps2dq', 'movmskps', 'movd', 'movq', 'movdqa', 'movdqu', 'emms',
              'stmxcsr', 'ldmxcsr', 'pshufw', 'pshufd', 'pextrw', 'pinsrw', 'prefetchnta',
              'prefetcht0', 'prefetcht1', 'prefetcht2', 'sfence', 'movntps', 'movntq', 'pmovmskb'} |
             {'cmp%sss' % p for p in CMP_PRED} | {'cmp%sps' % p for p in CMP_PRED})


class SimdGen:
    def __init__(self, g):
        self.g = g

    def ea(self, op):
        return self.g.ea(op)

    def is_x(self, op):
        return op[0] == 'r' and op[1].startswith('xmm')

    def is_m(self, op):
        return op[0] == 'r' and op[1].startswith('mm')

    def x(self, op):
        n = int(op[1][3:])
        self.g.xregs.add(n)
        return 'x%d' % n

    def mm(self, op):
        n = int(op[1][2:])
        self.g.mregs.add(n)
        return 'm%d' % n

    # ------------------------------------------------ operand reads (as __m128 / __m128i)
    def ps(self, op, n=None):
        """an xmm operand or memory as __m128; memory of size n (4: low lane, 8: low half)"""
        if self.is_x(op):
            return self.x(op)
        if op[0] != 'm':
            raise self.g.Unsupported('simd operand')
        n = n or op[1]
        a = self.ea(op)
        if n == 16:
            return '_mm_loadu_ps((const float *)GP(%s))' % a
        if n == 8:
            return '_mm_castpd_ps(_mm_load_sd((const double *)GP(%s)))' % a
        if n == 4:
            return '_mm_load_ss((const float *)GP(%s))' % a
        raise self.g.Unsupported('simd memory size %d' % n)

    def si(self, op, n=None):
        """an integer SIMD operand as __m128i: xmm, mm (low half) or memory"""
        if self.is_x(op):
            return '_mm_castps_si128(%s)' % self.x(op)
        if self.is_m(op):
            return '_mm_cvtsi64_si128((long long)%s)' % self.mm(op)
        if op[0] == 'm':
            n = n or op[1]
            a = self.ea(op)
            if n == 16:
                return '_mm_loadu_si128((const __m128i *)GP(%s))' % a
            if n == 8:
                return '_mm_loadl_epi64((const __m128i *)GP(%s))' % a
            if n == 4:
                return '_mm_cvtsi32_si128((int)R32(%s))' % a
        raise self.g.Unsupported('simd operand')

    def r32(self, op):
        if op[0] == 'r':
            return self.g.rd(op)
        return 'R32(%s)' % self.ea(op)

    # ------------------------------------------------ writes
    def put_si(self, op, v, mmx_hi=False):
        """store an __m128i result into an xmm or mm register"""
        if self.is_x(op):
            return '%s = _mm_castsi128_ps(%s);' % (self.x(op), v)
        if self.is_m(op):
            if mmx_hi:
                return '{ __m128i t = %s; %s = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(t, t)); }' % (v, self.mm(op))
            return '%s = (uint64_t)_mm_cvtsi128_si64(%s);' % (self.mm(op), v)
        raise self.g.Unsupported('simd destination')

    # ------------------------------------------------ dispatch
    def gen(self, i, m):
        E = self.g.emit
        ops = i.ops
        if m in SS_ARITH:
            d = self.x(ops[0])
            E('%s = %s(%s, %s);' % (d, SS_ARITH[m], d, self.ps(ops[1], 4)))
            return False
        if m in PS_ARITH:
            d = self.x(ops[0])
            E('%s = %s(%s, %s);' % (d, PS_ARITH[m], d, self.ps(ops[1])))
            return False
        if m in SS_UNARY:
            d = self.x(ops[0])
            E('%s = _mm_move_ss(%s, %s(%s));' % (d, d, SS_UNARY[m], self.ps(ops[1], 4)))
            return False
        if m in PS_UNARY:
            E('%s = %s(%s);' % (self.x(ops[0]), PS_UNARY[m], self.ps(ops[1])))
            return False
        if m.startswith('cmp') and (m.endswith('ss') or m.endswith('ps')) and m[3:-2] in CMP_PRED:
            d = self.x(ops[0])
            kind = m[-2:]
            E('%s = _mm_cmp%s_%s(%s, %s);' % (d, m[3:-2], kind, d, self.ps(ops[1], 4 if kind == 'ss' else None)))
            return False
        if m == 'movss':
            if self.is_x(ops[0]) and self.is_x(ops[1]):
                d = self.x(ops[0])
                E('%s = _mm_move_ss(%s, %s);' % (d, d, self.x(ops[1])))
            elif self.is_x(ops[0]):
                E('%s = _mm_load_ss((const float *)GP(%s));' % (self.x(ops[0]), self.ea(ops[1])))
            else:
                E('_mm_store_ss((float *)GP(%s), %s);' % (self.ea(ops[0]), self.x(ops[1])))
            return False
        if m in ('movaps', 'movups', 'movntps'):
            if self.is_x(ops[0]):
                E('%s = %s;' % (self.x(ops[0]), self.ps(ops[1], 16)))
            else:
                E('_mm_storeu_ps((float *)GP(%s), %s);' % (self.ea(ops[0]), self.x(ops[1])))
            return False
        if m in ('movdqa', 'movdqu'):
            if self.is_x(ops[0]):
                E('%s = _mm_castsi128_ps(%s);' % (self.x(ops[0]), self.si(ops[1], 16)))
            else:
                E('_mm_storeu_si128((__m128i *)GP(%s), _mm_castps_si128(%s));' % (self.ea(ops[0]), self.x(ops[1])))
            return False
        if m in ('movlps', 'movhps'):
            hi = m == 'movhps'
            if self.is_x(ops[0]):
                d = self.x(ops[0])
                E('%s = _mm_load%s_pi(%s, (const __m64 *)GP(%s));' % (d, 'h' if hi else 'l', d, self.ea(ops[1])))
            else:
                E('_mm_store%s_pi((__m64 *)GP(%s), %s);' % ('h' if hi else 'l', self.ea(ops[0]), self.x(ops[1])))
            return False
        if m == 'shufps':
            d = self.x(ops[0])
            E('%s = _mm_shuffle_ps(%s, %s, 0x%x);' % (d, d, self.ps(ops[1]), ops[2][1] & 0xff))
            return False
        if m in ('comiss', 'ucomiss'):
            return self.fcompare('_mm_cvtss_f32(%s)' % self.x(ops[0]), '_mm_cvtss_f32(%s)' % self.ps(ops[1], 4), i)
        if m == 'cvtsi2ss':
            d = self.x(ops[0])
            E('%s = _mm_cvtsi32_ss(%s, (int)%s);' % (d, d, self.r32(ops[1])))
            return False
        if m in ('cvttss2si', 'cvtss2si'):
            fn = '_mm_cvttss_si32' if m == 'cvttss2si' else '_mm_cvtss_si32'
            E(self.g.wr(ops[0], '(uint32_t)%s(%s)' % (fn, self.ps(ops[1], 4))))
            return False
        if m == 'cvtpi2ps':
            d = self.x(ops[0])
            E('{ __m128 t = _mm_cvtepi32_ps(%s); %s = _mm_shuffle_ps(t, %s, 0xe4); }' % (self.si(ops[1], 8), d, d))
            return False
        if m in ('cvtps2pi', 'cvttps2pi'):
            fn = '_mm_cvttps_epi32' if m == 'cvttps2pi' else '_mm_cvtps_epi32'
            E(self.put_si(ops[0], '%s(%s)' % (fn, self.ps(ops[1], 8))))
            return False
        if m == 'cvtdq2ps':
            E('%s = _mm_cvtepi32_ps(%s);' % (self.x(ops[0]), self.si(ops[1], 16)))
            return False
        if m in ('cvttps2dq', 'cvtps2dq'):
            fn = '_mm_cvttps_epi32' if m == 'cvttps2dq' else '_mm_cvtps_epi32'
            E('%s = _mm_castsi128_ps(%s(%s));' % (self.x(ops[0]), fn, self.ps(ops[1], 16)))
            return False
        if m == 'movmskps':
            E(self.g.wr(ops[0], '(uint32_t)_mm_movemask_ps(%s)' % self.x(ops[1])))
            return False
        if m == 'pmovmskb':
            v = '_mm_movemask_epi8(%s)' % self.si(ops[1])
            if self.is_m(ops[1]):
                v = '(%s & 0xff)' % v
            E(self.g.wr(ops[0], '(uint32_t)%s' % v))
            return False
        if m == 'movd':
            d, s = ops
            if self.is_x(d):
                E('%s = _mm_castsi128_ps(_mm_cvtsi32_si128((int)%s));' % (self.x(d), self.r32(s)))
            elif self.is_m(d):
                E('%s = (uint32_t)%s;' % (self.mm(d), self.r32(s)))
            elif self.is_x(s):
                E(self.g.wr(d, '(uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(%s))' % self.x(s)))
            else:
                E(self.g.wr(d, '(uint32_t)%s' % self.mm(s)))
            return False
        if m in ('movq', 'movntq'):
            d, s = ops
            if self.is_x(d):
                E('%s = _mm_castsi128_ps(_mm_move_epi64(%s));' % (self.x(d), self.si(s, 8)))
            elif self.is_m(d):
                if self.is_m(s):
                    E('%s = %s;' % (self.mm(d), self.mm(s)))
                elif self.is_x(s):
                    E('%s = (uint64_t)_mm_cvtsi128_si64(_mm_castps_si128(%s));' % (self.mm(d), self.x(s)))
                else:
                    E('%s = R64(%s);' % (self.mm(d), self.ea(s)))
            else:
                if self.is_m(s):
                    E('W64(%s, %s);' % (self.ea(d), self.mm(s)))
                else:
                    E('_mm_storel_epi64((__m128i *)GP(%s), _mm_castps_si128(%s));' % (self.ea(d), self.x(s)))
            return False
        if m in INT_OPS:
            d = ops[0]
            a, b = self.si(d), self.si(ops[1])
            if m == 'pandn':
                v = '_mm_andnot_si128(%s, %s)' % (a, b)
            elif m in ('packsswb', 'packssdw', 'packuswb') and self.is_m(d):
                # MMX packs: 4+4 lanes into 8; the 128-bit pack of (a:b in one register)
                v = '%s(_mm_unpacklo_epi64(%s, %s), _mm_setzero_si128())' % (INT_OPS[m], a, b)
            elif m == 'pmuludq' and self.is_m(d):
                v = '_mm_mul_epu32(%s, %s)' % (a, b)
            else:
                v = '%s(%s, %s)' % (INT_OPS[m], a, b)
            E(self.put_si(d, v))
            return False
        if m in UNPACK:
            d = ops[0]
            fn, hi = UNPACK[m]
            a, b = self.si(d), self.si(ops[1])
            if self.is_m(d):
                lo_fn = fn.replace('unpackhi', 'unpacklo')
                E(self.put_si(d, '%s(%s, %s)' % (lo_fn, a, b), mmx_hi=bool(hi)))
            else:
                E(self.put_si(d, '%s(%s, %s)' % (fn, a, b)))
            return False
        if m in SHIFTS:
            d = ops[0]
            k = SHIFTS[m]
            if ops[1][0] == 'i':
                v = '_mm_%s(%s, %d)' % (k.replace('l_', 'li_').replace('a_', 'ai_'), self.si(d), ops[1][1] & 0xff)
            else:
                v = '_mm_%s(%s, %s)' % (k, self.si(d), self.si(ops[1], 8))
            E(self.put_si(d, v))
            return False
        if m in ('pshufw', 'pshufd'):
            v = '_mm_shuffle%s_epi%s(%s, 0x%x)' % ('lo' if m == 'pshufw' else '', '16' if m == 'pshufw' else '32',
                                                   self.si(ops[1]), ops[2][1] & 0xff)
            E(self.put_si(ops[0], v))
            return False
        if m == 'pextrw':
            E(self.g.wr(ops[0], '(uint32_t)_mm_extract_epi16(%s, %d)' % (self.si(ops[1]), ops[2][1] & (3 if self.is_m(ops[1]) else 7))))
            return False
        if m == 'pinsrw':
            n = ops[2][1] & (3 if self.is_m(ops[0]) else 7)
            src = self.g.rd(ops[1]) if ops[1][0] == 'r' else 'R16(%s)' % self.ea(ops[1])
            E(self.put_si(ops[0], '_mm_insert_epi16(%s, (int)(%s & 0xffffu), %d)' % (self.si(ops[0]), src, n)))
            return False
        if m == 'emms' or m.startswith('prefetch') or m == 'sfence':
            return False
        if m == 'stmxcsr':
            E('W32(%s, c->mxcsr);' % self.ea(ops[0]))
            return False
        if m == 'ldmxcsr':
            E('cpu_ldmxcsr(c, R32(%s));' % self.ea(ops[0]))
            return False
        raise self.g.Unsupported('simd ' + m)

    def fcompare(self, a, b, i):
        """comiss / fcomi: ZF, PF, CF from an ordered/unordered compare; OF, SF, AF cleared"""
        E = self.g.emit
        live = self.g.live_out_defs.get(i.addr, 0)
        E('{ float a = %s, b = %s; uint32_t u = (a != a) | (b != b);' % (a, b))
        self.flags_from_compare(live)
        E('}')
        return False

    def flags_from_compare(self, live):
        E = self.g.emit
        from codegen import CF, PF, AF, ZF, SF, OF
        if live & ZF: E('    zf = u | (a == b);')
        if live & PF: E('    pf = u;')
        if live & CF: E('    cf = u | (a < b);')
        if live & OF: E('    of = 0;')
        if live & SF: E('    sf = 0;')
        if live & AF: E('    af = 0;')
