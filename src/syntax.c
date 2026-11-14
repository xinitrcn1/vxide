/* Syntax highlighting lexers: C23, x86 assembly (AT&T and Intel), Lisp.
 *
 * Each lexer is a pure function of (state at line start, line bytes) that
 * writes token spans and returns the state at the end of the line. Keyword
 * lookup goes through small open-addressing hash tables built once from
 * space-separated word lists (see kw_init). Classification follows the
 * TextMate scopes of VSCodium's c.tmLanguage under Dark+/Light+. */
#include "syntax.h"

#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Span output                                                         */

static inline void emit(SynSpans *o, u32 st, u32 len, u32 cls)
{
	if (!o || !len || cls == TK_TEXT)
		return;
	u32 c = o->count;
	if (c && o->cls[c - 1] == cls && o->start[c - 1] + o->len[c - 1] == st) {
		o->len[c - 1] += len;
		return;
	}
	if (c >= o->cap)
		return;
	o->start[c] = st;
	o->len[c] = len;
	o->cls[c] = (u8)cls;
	o->count = c + 1;
}

/* ------------------------------------------------------------------ */
/* Character classes                                                   */

static inline bool is_digit(u8 c) { return (unsigned)(c - '0') < 10u; }
static inline bool is_alpha(u8 c) { return (unsigned)((c | 32) - 'a') < 26u; }
static inline bool is_xdigit(u8 c) { return is_digit(c) || (unsigned)((c | 32) - 'a') < 6u; }
static inline bool is_space(u8 c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
static inline bool is_cident(u8 c) { return is_alpha(c) || is_digit(c) || c == '_' || c == '$' || c >= 0x80; }
static inline u8 lower(u8 c) { return (unsigned)(c - 'A') < 26u ? (u8)(c | 32) : c; }

static inline u32 skip_ws(const char *s, u32 n, u32 i)
{
	while (i < n && is_space((u8)s[i]))
		i++;
	return i;
}

static inline bool word_is(const char *s, u32 n, const char *w)
{
	size_t l = strlen(w);
	return n == l && memcmp(s, w, l) == 0;
}

/* ------------------------------------------------------------------ */
/* Keyword sets                                                        */

#define KW_SLOTS 8192u
#define KW_MAX 4096u

/* Internal classes beyond TK_COUNT carry extra lexer behaviour; kw_tk()
 * maps them back to the colour class. */
enum {
	KC_TAG = TK_COUNT, /* C struct/union/enum: next identifier is a type */
	KC_GOTO,           /* C goto: next identifier is a label             */
	KC_DEFINED,        /* C `defined` (directive only in #if lines)      */
	KC_DOXPARAM,       /* doxygen @param/@tparam: next word is a name    */
	KC_PREFIX,         /* asm prefix (rep, lock): mnemonic follows       */
	KC_DEFFN,          /* lisp defun & co: next symbol is a function     */
	KC_DEFVAR,         /* lisp defvar & co: next symbol is a variable    */
	KC_DEFTYPE,        /* lisp defstruct & co: next symbol is a type     */
};

static inline u32 kw_tk(u32 k)
{
	switch (k) {
	case KC_TAG: return TK_STORAGE;
	case KC_GOTO: return TK_CONTROL;
	case KC_DEFINED: return TK_CONTROL;
	case KC_DOXPARAM: return TK_STORAGE;
	case KC_PREFIX: return TK_STORAGE;
	case KC_DEFFN: case KC_DEFVAR: case KC_DEFTYPE: return TK_STORAGE;
	default: return k;
	}
}

typedef struct {
	const char *w[KW_MAX];
	u8 wl[KW_MAX];
	u8 wc[KW_MAX];
	u32 n;
	u16 slot[KW_SLOTS]; /* index + 1, 0 = empty */
	u32 lenmask[256];   /* by first byte: bit l set when a word of length l (<32) exists */
} KwSet;

static KwSet kw_c, kw_dox, kw_mn, kw_reg, kw_nasm, kw_size, kw_lisp;

static inline u32 kw_hash(const char *s, u32 n)
{
	u32 h = 2166136261u;
	for (u32 i = 0; i < n; i++) {
		h ^= (u8)s[i];
		h *= 16777619u;
	}
	return h;
}

/* Find a word (exact bytes); returns its class or -1. */
static int kw_find(const KwSet *k, const char *s, u32 n)
{
	if (n == 0 || n > 255)
		return -1;
	if (n < 32 && !(k->lenmask[(u8)s[0]] & (1u << n)))
		return -1;
	u32 h = kw_hash(s, n) & (KW_SLOTS - 1);
	for (;;) {
		u32 e = k->slot[h];
		if (!e)
			return -1;
		e--;
		if (k->wl[e] == n && memcmp(k->w[e], s, n) == 0)
			return k->wc[e];
		h = (h + 1) & (KW_SLOTS - 1);
	}
}

/* Case-insensitive lookup: tables hold lowercase words. */
static int kw_find_ci(const KwSet *k, const char *s, u32 n)
{
	char b[64];
	if (n == 0 || n >= sizeof b)
		return -1;
	for (u32 i = 0; i < n; i++)
		b[i] = (char)lower((u8)s[i]);
	return kw_find(k, b, n);
}

static void kw_add(KwSet *k, const char *w, u32 n, u8 cls)
{
	if (kw_find(k, w, n) >= 0 || k->n >= KW_MAX)
		return;
	u32 e = k->n++;
	k->w[e] = w;
	k->wl[e] = (u8)n;
	k->wc[e] = cls;
	k->lenmask[(u8)w[0]] |= n < 32 ? 1u << n : 0;
	u32 h = kw_hash(w, n) & (KW_SLOTS - 1);
	while (k->slot[h])
		h = (h + 1) & (KW_SLOTS - 1);
	k->slot[h] = (u16)(e + 1);
}

/* Add every space-separated word of list (a string literal with static
 * storage: the table points into it). */
static void kw_list(KwSet *k, const char *list, u8 cls)
{
	const char *p = list;
	while (*p) {
		while (*p == ' ' || *p == '\n')
			p++;
		const char *q = p;
		while (*q && *q != ' ' && *q != '\n')
			q++;
		if (q > p)
			kw_add(k, p, (u32)(q - p), cls);
		p = q;
	}
}

/* --- C --------------------------------------------------------------- */

static const char c_control[] =
	"break continue do else for goto if _Pragma return while switch case default";
static const char c_storage[] =
	"unsigned signed double _Bool short float long void char bool int typedef "
	"const extern register restrict static volatile inline auto constexpr "
	"thread_local _Thread_local _Atomic _Noreturn noreturn alignas _Alignas "
	"alignof _Alignof sizeof typeof typeof_unqual __typeof__ __typeof "
	"__typeof_unqual__ _Generic static_assert _Static_assert _BitInt _Complex "
	"_Imaginary _Decimal32 _Decimal64 _Decimal128 _Float16 _Float32 _Float64 "
	"_Float128 _Float32x _Float64x _Float128x __int128 __int128_t __uint128_t "
	"__attribute__ __attribute __asm__ asm __asm __inline__ __inline "
	"__restrict__ __restrict __volatile__ __extension__ __declspec __thread "
	"_Countof countof __auto_type";
static const char c_langconst[] = "NULL true false TRUE FALSE nullptr";
static const char c_support[] =
	"int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
	"int_least8_t int_least16_t int_least32_t int_least64_t uint_least8_t "
	"uint_least16_t uint_least32_t uint_least64_t int_fast8_t int_fast16_t "
	"int_fast32_t int_fast64_t uint_fast8_t uint_fast16_t uint_fast32_t "
	"uint_fast64_t intptr_t uintptr_t intmax_t uintmax_t u_char u_short u_int "
	"u_long ushort uint u_quad_t quad_t qaddr_t caddr_t daddr_t div_t dev_t "
	"fixpt_t blkcnt_t blksize_t gid_t in_addr_t in_port_t ino_t key_t mode_t "
	"nlink_t id_t pid_t off_t segsz_t swblk_t uid_t clock_t size_t ssize_t "
	"time_t useconds_t suseconds_t pthread_attr_t pthread_cond_t "
	"pthread_condattr_t pthread_mutex_t pthread_mutexattr_t pthread_once_t "
	"pthread_rwlock_t pthread_rwlockattr_t pthread_t pthread_key_t";
static const char c_tag[] = "struct union enum";

static const char dox_tags[] =
	"a addindex addtogroup anchor arg attention author authors b brief bug c "
	"callergraph callgraph category cite class code cond copybrief copydetails "
	"copydoc copyright date def defgroup deprecated details dir dontinclude "
	"dot e else elseif em emoji endcode endcond enddot endif endlink "
	"endverbatim enum example exception extends file fn headerfile hideinitializer "
	"htmlinclude if ifnot image implements include includelineno ingroup "
	"internal invariant interface li line link mainpage memberof name namespace "
	"note overload p package page par paragraph post pre private property "
	"protected public pure ref related relates remark remarks result return "
	"returns retval sa section see short showinitializer since skip skipline "
	"snippet struct subpage subsection subsubsection test throw throws todo "
	"typedef union until var verbatim version warning weakgroup xrefitem";

/* --- x86 assembly ----------------------------------------------------- */

static const char asm_control[] =
	"jmp ljmp call lcall ret retf retn lret iret iretd iretq iretw sysret "
	"sysretq sysexit sysenter syscall int int1 int3 into loop loope loopne "
	"loopz loopnz jcxz jecxz jrcxz callq retq jmpq callw callq jmpl calll "
	"retl retw";
static const char asm_prefix[] =
	"rep repe repz repne repnz lock data16 data32 addr16 addr32 rex rex64 "
	"notrack bnd xacquire xrelease";
static const char asm_gp[] =
	/* general purpose */
	"aaa aad aam aas adc adcx add adox and andn arpl bextr blcfill blci blcic "
	"blcmsk blcs blsfill blsi blsic blsmsk blsr bound bsf bsr bswap bt btc btr "
	"bts bzhi cbw cdq cdqe clac clc cld cldemote clflush clflushopt cli "
	"clrssbsy clts clui clwb clzero cmc cmp cmps cmpsb cmpsd cmpsq cmpsw "
	"cmpxchg cmpxchg8b cmpxchg16b cpuid crc32 cqo cwd cwde daa das dec div "
	"endbr32 endbr64 enter hlt hreset idiv imul in inc incsspd incsspq ins "
	"insb insd insw invd invlpg invlpga invpcid lahf lar lds lea leave les "
	"lfence lfs lgdt lgs lidt lldt lmsw lods lodsb lodsd lodsq lodsw lsl lss "
	"ltr lzcnt mfence monitor monitorx mov movabs movbe movd movq movs movsb "
	"movsd movsq movsw movsx movsxd movzx mul mulx mwait mwaitx neg nop not or "
	"out outs outsb outsd outsw pause pdep pext pop popa popad popcnt popf "
	"popfd popfq prefetch prefetchnta prefetcht0 prefetcht1 prefetcht2 "
	"prefetchw prefetchwt1 push pusha pushad pushf pushfd pushfq rcl rcr "
	"rdfsbase rdgsbase rdmsr rdpid rdpkru rdpmc rdrand rdseed rdsspd rdsspq "
	"rdtsc rdtscp rol ror rorx rsm sahf sal sar sarx sbb scas scasb scasd "
	"scasq scasw serialize sfence sgdt shl shld shlx shr shrd shrx sidt sldt "
	"smsw stac stc std sti stos stosb stosd stosq stosw str sub swapgs test "
	"tpause tzcnt ud0 ud1 ud2 umonitor umwait verr verw wbinvd wbnoinvd "
	"wrfsbase wrgsbase wrmsr wrpkru wrssd wrssq xabort xadd xbegin xchg xend "
	"xgetbv xlat xlatb xor xrstor xrstor64 xrstors xsave xsave64 xsavec "
	"xsaveopt xsaves xsetbv xtest getsec vmcall vmclear vmfunc vmlaunch "
	"vmptrld vmptrst vmread vmresume vmwrite vmxoff vmxon vmrun vmload vmsave "
	"vmmcall stgi clgi skinit invept invvpid "
	/* AT&T spellings */
	"movzbl movzbw movzbq movzwl movzwq movsbl movsbw movsbq movswl movswq "
	"movslq cltq cqto cltd cwtl cbtw cwtd movabsq "
	/* x87 */
	"f2xm1 fabs fadd faddp fbld fbstp fchs fclex fcmovb fcmovbe fcmove "
	"fcmovnb fcmovnbe fcmovne fcmovnu fcmovu fcom fcomi fcomip fcomp fcompp "
	"fcos fdecstp fdiv fdivp fdivr fdivrp ffree fiadd ficom ficomp fidiv "
	"fidivr fild fimul fincstp finit fist fistp fisttp fisub fisubr fld fld1 "
	"fldcw fldenv fldl2e fldl2t fldlg2 fldln2 fldpi fldz fmul fmulp fnclex "
	"fninit fnop fnsave fnstcw fnstenv fnstsw fpatan fprem fprem1 fptan "
	"frndint frstor fsave fscale fsin fsincos fsqrt fst fstcw fstenv fstp "
	"fstsw fsub fsubp fsubr fsubrp ftst fucom fucomi fucomip fucomp fucompp "
	"fwait fxam fxch fxrstor fxrstor64 fxsave fxsave64 fxtract fyl2x fyl2xp1 "
	"wait emms femms";
static const char asm_sse[] =
	/* MMX / SSE..SSE4.2 (VEX forms are found by stripping a leading 'v') */
	"addpd addps addsd addss addsubpd addsubps andnpd andnps andpd andps "
	"blendpd blendps blendvpd blendvps cmppd cmpps cmpss comisd comiss "
	"cvtdq2pd cvtdq2ps cvtpd2dq cvtpd2pi cvtpd2ps cvtpi2pd cvtpi2ps cvtps2dq "
	"cvtps2pd cvtps2pi cvtsd2si cvtsd2ss cvtsi2sd cvtsi2ss cvtss2sd cvtss2si "
	"cvttpd2dq cvttpd2pi cvttps2dq cvttps2pi cvttsd2si cvttss2si divpd divps "
	"divsd divss dppd dpps extractps haddpd haddps hsubpd hsubps insertps "
	"lddqu ldmxcsr maskmovdqu maskmovq maxpd maxps maxsd maxss minpd minps "
	"minsd minss movapd movaps movddup movdq2q movdqa movdqu movhlps movhpd "
	"movhps movlhps movlpd movlps movmskpd movmskps movntdq movntdqa movnti "
	"movntpd movntps movntq movq2dq movshdup movsldup movss movupd movups "
	"mpsadbw mulpd mulps mulsd mulss orpd orps pabsb pabsd pabsw packssdw "
	"packsswb packusdw packuswb paddb paddd paddq paddsb paddsw paddusb "
	"paddusw paddw palignr pand pandn pavgb pavgw pblendvb pblendw pclmulqdq "
	"pcmpeqb pcmpeqd pcmpeqq pcmpeqw pcmpestri pcmpestrm pcmpgtb pcmpgtd "
	"pcmpgtq pcmpgtw pcmpistri pcmpistrm pextrb pextrd pextrq pextrw phaddd "
	"phaddsw phaddw phminposuw phsubd phsubsw phsubw pinsrb pinsrd pinsrq "
	"pinsrw pmaddubsw pmaddwd pmaxsb pmaxsd pmaxsw pmaxub pmaxud pmaxuw "
	"pminsb pminsd pminsw pminub pminud pminuw pmovmskb pmovsxbd pmovsxbq "
	"pmovsxbw pmovsxdq pmovsxwd pmovsxwq pmovzxbd pmovzxbq pmovzxbw pmovzxdq "
	"pmovzxwd pmovzxwq pmuldq pmulhrsw pmulhuw pmulhw pmulld pmullw pmuludq "
	"por psadbw pshufb pshufd pshufhw pshuflw pshufw psignb psignd psignw "
	"pslld pslldq psllq psllw psrad psraw psrld psrldq psrlq psrlw psubb "
	"psubd psubq psubsb psubsw psubusb psubusw psubw ptest punpckhbw "
	"punpckhdq punpckhqdq punpckhwd punpcklbw punpckldq punpcklqdq punpcklwd "
	"pxor rcpps rcpss roundpd roundps roundsd roundss rsqrtps rsqrtss shufpd "
	"shufps sqrtpd sqrtps sqrtsd sqrtss stmxcsr subpd subps subsd subss "
	"ucomisd ucomiss unpckhpd unpckhps unpcklpd unpcklps xorpd xorps "
	/* AES, SHA, GFNI */
	"aesdec aesdeclast aesenc aesenclast aesimc aeskeygenassist sha1msg1 "
	"sha1msg2 sha1nexte sha1rnds4 sha256msg1 sha256msg2 sha256rnds2 "
	"gf2p8affineqb gf2p8affineinvqb gf2p8mulb "
	/* AVX / AVX2 only */
	"vbroadcastss vbroadcastsd vbroadcastf128 vbroadcasti128 vextractf128 "
	"vextracti128 vinsertf128 vinserti128 vmaskmovps vmaskmovpd vpmaskmovd "
	"vpmaskmovq vperm2f128 vperm2i128 vpermilps vpermilpd vpermd vpermq "
	"vpermps vpermpd vpsllvd vpsllvq vpsravd vpsrlvd vpsrlvq vpbroadcastb "
	"vpbroadcastw vpbroadcastd vpbroadcastq vpblendd vgatherdps vgatherdpd "
	"vgatherqps vgatherqpd vpgatherdd vpgatherdq vpgatherqd vpgatherqq "
	"vzeroall vzeroupper vtestps vtestpd vcvtph2ps vcvtps2ph";
static const char asm_avx512[] =
	/* AVX-512 */
	"valignd valignq vblendmpd vblendmps vbroadcastf32x2 vbroadcastf32x4 "
	"vbroadcastf32x8 vbroadcastf64x2 vbroadcastf64x4 vbroadcasti32x2 "
	"vbroadcasti32x4 vbroadcasti32x8 vbroadcasti64x2 vbroadcasti64x4 "
	"vcompresspd vcompressps vcvtpd2qq vcvtpd2udq vcvtpd2uqq vcvtps2qq "
	"vcvtps2udq vcvtps2uqq vcvtqq2pd vcvtqq2ps vcvtsd2usi vcvtss2usi "
	"vcvttpd2qq vcvttpd2udq vcvttpd2uqq vcvttps2qq vcvttps2udq vcvttps2uqq "
	"vcvttsd2usi vcvttss2usi vcvtudq2pd vcvtudq2ps vcvtuqq2pd vcvtuqq2ps "
	"vcvtusi2sd vcvtusi2ss vdbpsadbw vexpandpd vexpandps vextractf32x4 "
	"vextractf32x8 vextractf64x2 vextractf64x4 vextracti32x4 vextracti32x8 "
	"vextracti64x2 vextracti64x4 vfixupimmpd vfixupimmps vfixupimmsd "
	"vfixupimmss vfpclasspd vfpclassps vfpclasssd vfpclassss vgetexppd "
	"vgetexpps vgetexpsd vgetexpss vgetmantpd vgetmantps vgetmantsd "
	"vgetmantss vinsertf32x4 vinsertf32x8 vinsertf64x2 vinsertf64x4 "
	"vinserti32x4 vinserti32x8 vinserti64x2 vinserti64x4 vmovdqa32 vmovdqa64 "
	"vmovdqu8 vmovdqu16 vmovdqu32 vmovdqu64 vpabsq vpandd vpandnd vpandnq "
	"vpandq vpblendmb vpblendmd vpblendmq vpblendmw vpbroadcastmb2q "
	"vpbroadcastmw2d vpcmpb vpcmpd vpcmpq vpcmpub vpcmpud vpcmpuq vpcmpuw "
	"vpcmpw vpcompressb vpcompressd vpcompressq vpcompressw vpconflictd "
	"vpconflictq vpdpbusd vpdpbusds vpdpwssd vpdpwssds vpermb vpermi2b "
	"vpermi2d vpermi2pd vpermi2ps vpermi2q vpermi2w vpermt2b vpermt2d "
	"vpermt2pd vpermt2ps vpermt2q vpermt2w vpermw vpexpandb vpexpandd "
	"vpexpandq vpexpandw vplzcntd vplzcntq vpmadd52huq vpmadd52luq vpmaxsq "
	"vpmaxuq vpminsq vpminuq vpmovb2m vpmovd2m vpmovdb vpmovdw vpmovm2b "
	"vpmovm2d vpmovm2q vpmovm2w vpmovq2m vpmovqb vpmovqd vpmovqw vpmovsdb "
	"vpmovsdw vpmovsqb vpmovsqd vpmovsqw vpmovswb vpmovusdb vpmovusdw "
	"vpmovusqb vpmovusqd vpmovusqw vpmovuswb vpmovw2m vpmovwb vpmullq "
	"vpmultishiftqb vpopcntb vpopcntd vpopcntq vpopcntw vpord vporq vprold "
	"vprolq vprolvd vprolvq vprord vprorq vprorvd vprorvq vpscatterdd "
	"vpscatterdq vpscatterqd vpscatterqq vpshldd vpshldq vpshldvd vpshldvq "
	"vpshldvw vpshldw vpshrdd vpshrdq vpshrdvd vpshrdvq vpshrdvw vpshrdw "
	"vpshufbitqmb vpsllvw vpsraq vpsravq vpsravw vpsrlvw vpternlogd "
	"vpternlogq vptestmb vptestmd vptestmq vptestmw vptestnmb vptestnmd "
	"vptestnmq vptestnmw vpxord vpxorq vrangepd vrangeps vrangesd vrangess "
	"vrcp14pd vrcp14ps vrcp14sd vrcp14ss vreducepd vreduceps vreducesd "
	"vreducess vrndscalepd vrndscaleps vrndscalesd vrndscaless vrsqrt14pd "
	"vrsqrt14ps vrsqrt14sd vrsqrt14ss vscalefpd vscalefps vscalefsd "
	"vscalefss vscatterdpd vscatterdps vscatterqpd vscatterqps vshuff32x4 "
	"vshuff64x2 vshufi32x4 vshufi64x2 vpdpbssd vcvtne2ps2bf16 vcvtneps2bf16 "
	"vdpbf16ps";

static const char asm_regs[] =
	"al ah ax eax rax bl bh bx ebx rbx cl ch cx ecx rcx dl dh dx edx rdx "
	"si sil esi rsi di dil edi rdi bp bpl ebp rbp sp spl esp rsp ip eip rip "
	"cs ds es fs gs ss flags eflags rflags st mxcsr";

static const char nasm_dirs[] =
	"section segment global globl extern common bits use16 use32 use64 default "
	"align alignb absolute org cpu float struc endstruc istruc iend at times "
	"equ incbin db dw dd dq dt do dy dz resb resw resd resq rest reso resy "
	"resz static required proc endp ends end public extrn externdef include "
	"includelib invoke assume label macro endm local dup proto model option";
static const char nasm_size[] =
	"byte word dword qword tword oword yword zword fword ptr near far short "
	"rel abs strict nosplit offset seg wrt";

/* --- Lisp ------------------------------------------------------------- */

static const char lisp_control[] =
	"if cond when unless case ecase ccase typecase etypecase ctypecase progn "
	"prog1 prog2 prog prog* let let* letrec letrec* let-values flet labels "
	"macrolet symbol-macrolet loop do do* dolist dotimes setf setq psetf psetq "
	"and or not return return-from block tagbody go catch throw "
	"unwind-protect handler-case handler-bind ignore-errors restart-case "
	"multiple-value-bind multiple-value-prog1 destructuring-bind begin set! "
	"else recur try finally when-let if-let when-not if-not doseq for while "
	"save-excursion save-restriction condition-case eval-when declare the "
	"quote quasiquote unquote function lambda fn fn* λ case-lambda match "
	"with-slots with-accessors with-open-file loop-finish named-let assert "
	"letfn binding cond-> cond->> -> ->> some-> some->> as-> doto dosync "
	"locking delay force parameterize guard dynamic-wind call/cc "
	"call-with-current-continuation in-package";
static const char lisp_deffn[] =
	"defun defmacro defgeneric defmethod define-syntax define-macro defn defn- "
	"defmulti defsubst define-compiler-macro define-modify-macro defsetf "
	"define-setf-expander define-method-combination define-inline defalias "
	"defadvice define-derived-mode define-minor-mode cl-defun cl-defmacro "
	"cl-defgeneric cl-defmethod define-syntax-rule";
static const char lisp_defvar[] =
	"defvar defparameter defconstant defcustom defonce def defconst define "
	"define-values defface defgroup define-symbol-macro";
static const char lisp_deftype[] =
	"defstruct defclass deftype defpackage define-condition define-record-type "
	"defrecord defprotocol definterface deftype cl-defstruct ns";
static const char lisp_const[] = "t nil true false #t #f #true #false";

/* ------------------------------------------------------------------ */
/* One-time init (thread safe)                                          */

static atomic_int syn_ready;

static void syn_build(void)
{
	kw_add(&kw_c, "goto", 4, KC_GOTO); /* before the list, which skips duplicates */
	kw_list(&kw_c, c_control, TK_CONTROL);
	kw_list(&kw_c, c_storage, TK_STORAGE);
	kw_list(&kw_c, c_langconst, TK_LANGCONST);
	kw_list(&kw_c, c_support, TK_TYPE);
	kw_list(&kw_c, c_tag, KC_TAG);
	kw_add(&kw_c, "defined", 7, KC_DEFINED);

	kw_list(&kw_dox, dox_tags, TK_STORAGE);
	kw_add(&kw_dox, "param", 5, KC_DOXPARAM);
	kw_add(&kw_dox, "tparam", 6, KC_DOXPARAM);

	kw_list(&kw_mn, asm_control, TK_CONTROL);
	kw_list(&kw_mn, asm_prefix, KC_PREFIX);
	kw_list(&kw_mn, asm_gp, TK_STORAGE);
	kw_list(&kw_mn, asm_sse, TK_STORAGE);
	kw_list(&kw_mn, asm_avx512, TK_STORAGE);
	kw_list(&kw_reg, asm_regs, TK_VARIABLE);
	kw_list(&kw_nasm, nasm_dirs, TK_CONTROL);
	kw_list(&kw_size, nasm_size, TK_STORAGE);

	kw_list(&kw_lisp, lisp_deffn, KC_DEFFN);
	kw_list(&kw_lisp, lisp_defvar, KC_DEFVAR);
	kw_list(&kw_lisp, lisp_deftype, KC_DEFTYPE);
	kw_list(&kw_lisp, lisp_control, TK_CONTROL);
	kw_list(&kw_lisp, lisp_const, TK_LANGCONST);
}

static void syn_init(void)
{
	if (atomic_load_explicit(&syn_ready, memory_order_acquire) == 2)
		return;
	int e = 0;
	if (atomic_compare_exchange_strong(&syn_ready, &e, 1)) {
		syn_build();
		atomic_store_explicit(&syn_ready, 2, memory_order_release);
		return;
	}
	while (atomic_load_explicit(&syn_ready, memory_order_acquire) != 2)
		;
}

/* ================================================================== */
/* C23                                                                 */

/* State word: bits 0-2 mode, bit 3 inside a continued directive, bits 4-5
 * the directive kind, bits 8-15 the depth of a dead `#if 0` region. */
enum { CM_NORMAL, CM_BLOCK, CM_DOCBLOCK, CM_STRING, CM_CHAR, CM_LINECMT, CM_DOCLINE };
enum { PPK_OTHER, PPK_DEFINE, PPK_COND, PPK_DIAG };
#define C_PP 0x08u
#define C_STATE(mode, pp, ppk, if0) \
	((u32)(mode) | ((pp) ? C_PP : 0u) | ((u32)(ppk) << 4) | ((u32)(if0) << 8))

/* Comment text [i, end): doc comments colour doxygen tags. */
static void c_comment_text(const char *s, u32 i, u32 end, bool doc, SynSpans *o)
{
	if (!doc) {
		emit(o, i, end - i, TK_COMMENT);
		return;
	}
	u32 run = i;
	while (i < end) {
		u8 c = (u8)s[i];
		if ((c == '@' || c == '\\') && i + 1 < end && is_alpha((u8)s[i + 1]) &&
		    (i == 0 || is_space((u8)s[i - 1]) || s[i - 1] == '*' || s[i - 1] == '/' ||
		     s[i - 1] == '!')) {
			u32 j = i + 1;
			while (j < end && (is_alpha((u8)s[j]) || is_digit((u8)s[j]) || s[j] == '_'))
				j++;
			int k = kw_find(&kw_dox, s + i + 1, j - i - 1);
			if (k >= 0 && (j == end || !is_cident((u8)s[j]))) {
				emit(o, run, i - run, TK_COMMENT);
				emit(o, i, j - i, TK_STORAGE);
				i = run = j;
				if (k == KC_DOXPARAM) {
					u32 a = skip_ws(s, end, j);
					if (a < end && s[a] == '[') { /* [in,out] */
						while (a < end && s[a] != ']')
							a++;
						a = skip_ws(s, end, a < end ? a + 1 : a);
					}
					u32 b = a;
					while (b < end && (is_alpha((u8)s[b]) || is_digit((u8)s[b]) || s[b] == '_'))
						b++;
					if (b > a) {
						emit(o, run, a - run, TK_COMMENT);
						emit(o, a, b - a, TK_VARIABLE);
						i = run = b;
					}
				}
				continue;
			}
			i = j;
			continue;
		}
		i++;
	}
	emit(o, run, end - run, TK_COMMENT);
}

/* Block comment starting at st, searching for the terminator from i. */
static u32 c_block(const char *s, u32 n, u32 st, u32 i, bool doc, SynSpans *o, bool *closed)
{
	while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
		i++;
	if (i + 1 < n) {
		c_comment_text(s, st, i + 2, doc, o);
		*closed = true;
		return i + 2;
	}
	c_comment_text(s, st, n, doc, o);
	*closed = false;
	return n;
}

/* Escape at s[i] == '\\'; returns its end and whether it is valid. */
static u32 c_escape(const char *s, u32 n, u32 i, bool *ok)
{
	u32 j = i + 1;
	*ok = true;
	if (j >= n)
		return j;
	u8 c = (u8)s[j];
	u32 max = 0;
	switch (c) {
	case '\\': case 'a': case 'b': case 'e': case 'f': case 'n': case 'p':
	case 'r': case 't': case 'v': case '\'': case '"': case '?':
		return j + 1;
	case 'x': max = 2; break;
	case 'u': max = 4; break;
	case 'U': max = 8; break;
	default:
		if (c >= '0' && c <= '7') {
			u32 lim = c <= '3' ? 3 : 2;
			u32 k = 0;
			while (k < lim && j < n && is_digit((u8)s[j])) {
				j++;
				k++;
			}
			return j;
		}
		*ok = false;
		return j + 1;
	}
	j++;
	for (u32 k = 0; k < max && j < n && is_xdigit((u8)s[j]); k++)
		j++;
	return j;
}

/* printf placeholder at s[i] == '%' (grammar's string_placeholder);
 * returns its end, or i when none. */
static u32 c_placeholder(const char *s, u32 n, u32 i)
{
	u32 j = i + 1, k;
	/* (\d+\$)? */
	for (k = j; k < n && is_digit((u8)s[k]); k++)
		;
	if (k > j && k < n && s[k] == '$')
		j = k + 1;
	while (j < n && (s[j] == '#' || s[j] == '0' || s[j] == '-' || s[j] == ' ' ||
	                 s[j] == '+' || s[j] == '\''))
		j++;
	if (j < n && (s[j] == ',' || s[j] == ';' || s[j] == ':' || s[j] == '_'))
		j++;
	/* width */
	if (j < n && s[j] == '*') {
		j++;
	} else {
		k = j;
		if (k < n && s[k] == '-')
			k++;
		u32 d = k;
		while (k < n && is_digit((u8)s[k]))
			k++;
		if (k > d)
			j = k;
	}
	/* precision */
	if (j < n && s[j] == '.') {
		j++;
		if (j < n && s[j] == '*') {
			j++;
		} else {
			if (j < n && s[j] == '-')
				j++;
			while (j < n && is_digit((u8)s[j]))
				j++;
		}
	}
	/* length */
	if (j + 1 < n && ((s[j] == 'h' && s[j + 1] == 'h') || (s[j] == 'l' && s[j + 1] == 'l') ||
	                  (s[j] == 'v' && (s[j + 1] == 'h' || s[j + 1] == 'l')) ||
	                  (s[j] == 'h' && (s[j + 1] == 'v' || s[j + 1] == 'l'))))
		j += 2;
	else if (j < n && strchr("hljtzqLv", s[j]) && s[j])
		j++;
	if (j < n && s[j] && strchr("diouxXDOUeEfFgGaACcSspn%", s[j]))
		return j + 1;
	return i;
}

/* String/char body from i (past the opening quote) with the span starting
 * at st. Returns the index after the closing quote, or n. */
static u32 c_string(const char *s, u32 n, u32 i, u32 st, u8 q, SynSpans *o, bool *cont)
{
	u32 run = st;
	*cont = false;
	while (i < n) {
		u8 c = (u8)s[i];
		if (c == q) {
			i++;
			emit(o, run, i - run, TK_STRING);
			return i;
		}
		if (c == '\\') {
			emit(o, run, i - run, TK_STRING);
			if (i + 1 == n) {
				emit(o, i, 1, TK_ESCAPE);
				*cont = true;
				return n;
			}
			bool ok;
			u32 j = c_escape(s, n, i, &ok);
			emit(o, i, j - i, ok ? TK_ESCAPE : TK_INVALID);
			i = run = j;
			continue;
		}
		if (c == '%' && q == '"') {
			u32 j = c_placeholder(s, n, i);
			if (j > i) {
				emit(o, run, i - run, TK_STRING);
				emit(o, i, j - i, TK_VARIABLE);
				i = run = j;
				continue;
			}
		}
		i++;
	}
	emit(o, run, n - run, TK_STRING);
	return n;
}

static u32 c_number(const char *s, u32 n, u32 i)
{
	bool hex = s[i] == '0' && i + 1 < n && (s[i + 1] | 32) == 'x';
	u32 j = i + 1;
	while (j < n) {
		u8 c = (u8)s[j];
		if (is_alpha(c) || is_digit(c) || c == '_' || c == '.')
			j++;
		else if (c == '\'' && j + 1 < n && (is_alpha((u8)s[j + 1]) || is_digit((u8)s[j + 1])))
			j++;
		else if ((c == '+' || c == '-') &&
		         (hex ? (s[j - 1] | 32) == 'p' : (s[j - 1] | 32) == 'e'))
			j++;
		else
			break;
	}
	return j;
}

static bool c_str_prefix(const char *s, u32 n)
{
	return (n == 1 && (s[0] == 'u' || s[0] == 'U' || s[0] == 'L')) ||
	       (n == 2 && s[0] == 'u' && s[1] == '8');
}

static bool c_dead_opener(const char *w, u32 n)
{
	return word_is(w, n, "if") || word_is(w, n, "ifdef") || word_is(w, n, "ifndef");
}

static bool c_dead_else(const char *w, u32 n)
{
	return word_is(w, n, "else") || word_is(w, n, "elif") || word_is(w, n, "elifdef") ||
	       word_is(w, n, "elifndef");
}

enum { P_NONE, P_DOT, P_TAG, P_GOTO, P_DEFINE, P_INCLUDE, P_PRAGMA };

static u32 lex_c(u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 mode = state & 7u, ppk = (state >> 4) & 3u, if0 = (state >> 8) & 0xFFu;
	bool pp = (state & C_PP) != 0;
	u32 i = 0;

	/* Inside `#if 0`: everything is a comment until the matching
	 * #else/#elif/#endif, which is lexed as a live directive. */
	if (if0 && mode == CM_NORMAL && !pp) {
		u32 j = skip_ws(s, n, 0);
		if (j < n && s[j] == '#') {
			u32 a = skip_ws(s, n, j + 1), b = a;
			while (b < n && is_cident((u8)s[b]))
				b++;
			if (c_dead_opener(s + a, b - a)) {
				emit(o, 0, n, TK_COMMENT);
				return C_STATE(CM_NORMAL, 0, 0, if0 < 255 ? if0 + 1 : if0);
			}
			if (word_is(s + a, b - a, "endif")) {
				if (--if0) {
					emit(o, 0, n, TK_COMMENT);
					return C_STATE(CM_NORMAL, 0, 0, if0);
				}
			} else if (if0 == 1 && c_dead_else(s + a, b - a)) {
				if0 = 0;
			} else {
				emit(o, 0, n, TK_COMMENT);
				return state;
			}
		} else {
			emit(o, 0, n, TK_COMMENT);
			return state;
		}
	}

	/* Constructs continued from the previous line. */
	switch (mode) {
	case CM_BLOCK:
	case CM_DOCBLOCK: {
		bool closed;
		i = c_block(s, n, 0, 0, mode == CM_DOCBLOCK, o, &closed);
		if (!closed)
			return state;
		break;
	}
	case CM_LINECMT:
	case CM_DOCLINE:
		c_comment_text(s, 0, n, mode == CM_DOCLINE, o);
		if (n && s[n - 1] == '\\')
			return state;
		return C_STATE(CM_NORMAL, 0, 0, if0);
	case CM_STRING:
	case CM_CHAR: {
		bool cont;
		i = c_string(s, n, 0, 0, mode == CM_STRING ? '"' : '\'', o, &cont);
		if (cont)
			return state;
		break;
	}
	default:
		break;
	}

	bool line_pp = pp;
	u32 kind = pp ? ppk : PPK_OTHER;
	u32 if0_next = if0;
	u32 prev = P_NONE;
	bool in_params = false;
	u32 attr = 0;

	/* A directive starts a fresh line. */
	if (!pp && i == 0) {
		u32 j = skip_ws(s, n, 0);
		if (j < n && s[j] == '#') {
			u32 a = skip_ws(s, n, j + 1), b = a;
			while (b < n && is_cident((u8)s[b]))
				b++;
			const char *w = s + a;
			u32 wl = b - a;
			emit(o, j, b - j, TK_CONTROL);
			line_pp = true;
			i = b;
			if (word_is(w, wl, "define") || word_is(w, wl, "undef")) {
				kind = PPK_DEFINE;
				prev = P_DEFINE;
			} else if (word_is(w, wl, "include") || word_is(w, wl, "include_next") ||
			           word_is(w, wl, "import") || word_is(w, wl, "embed")) {
				prev = P_INCLUDE;
			} else if (word_is(w, wl, "if") || word_is(w, wl, "elif") ||
			           word_is(w, wl, "ifdef") || word_is(w, wl, "ifndef") ||
			           word_is(w, wl, "elifdef") || word_is(w, wl, "elifndef")) {
				kind = PPK_COND;
				if (word_is(w, wl, "if") || word_is(w, wl, "elif")) {
					u32 z = skip_ws(s, n, b);
					if (z < n && s[z] == '0' && (z + 1 == n || is_space((u8)s[z + 1]) || s[z + 1] == '/'))
						if0_next = 1;
				}
			} else if (word_is(w, wl, "error") || word_is(w, wl, "warning")) {
				kind = PPK_DIAG;
			} else if (word_is(w, wl, "pragma")) {
				prev = P_PRAGMA;
			}
		}
	}

	if (line_pp && kind == PPK_DIAG) {
		u32 a = skip_ws(s, n, i);
		bool cont = n && s[n - 1] == '\\';
		emit(o, a, n - a - (cont ? 1 : 0), TK_STRING);
		if (cont) {
			emit(o, n - 1, 1, TK_ESCAPE);
			return C_STATE(CM_NORMAL, 1, PPK_DIAG, if0_next);
		}
		return C_STATE(CM_NORMAL, 0, 0, if0_next);
	}

	u32 dflt = line_pp ? TK_PREPROC : TK_TEXT;

	while (i < n) {
		u8 c = (u8)s[i];
		if (is_space(c)) {
			i++;
			continue;
		}
		u32 newprev = P_NONE;

		if (c == '/' && i + 1 < n && s[i + 1] == '/') {
			bool d = i + 2 < n && (s[i + 2] == '!' || (s[i + 2] == '/' && !(i + 3 < n && s[i + 3] == '/')));
			c_comment_text(s, i, n, d, o);
			if (s[n - 1] == '\\')
				return C_STATE(d ? CM_DOCLINE : CM_LINECMT, line_pp, kind, if0_next);
			return C_STATE(CM_NORMAL, 0, 0, if0_next);
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '*') {
			bool d = i + 2 < n && (s[i + 2] == '!' ||
			                       (s[i + 2] == '*' && !(i + 3 < n && s[i + 3] == '/')));
			bool closed;
			i = c_block(s, n, i, i + 2, d, o, &closed);
			if (!closed)
				return C_STATE(d ? CM_DOCBLOCK : CM_BLOCK, line_pp, kind, if0_next);
			continue; /* comments do not reset prev */
		}
		if (c == '"' || c == '\'') {
			bool cont;
			i = c_string(s, n, i + 1, i, c, o, &cont);
			if (cont)
				return C_STATE(c == '"' ? CM_STRING : CM_CHAR, line_pp, kind, if0_next);
			prev = P_NONE;
			continue;
		}
		if (c == '<' && prev == P_INCLUDE) {
			u32 j = i + 1;
			while (j < n && s[j] != '>')
				j++;
			if (j < n)
				j++;
			emit(o, i, j - i, TK_STRING);
			i = j;
			prev = P_NONE;
			continue;
		}
		if (is_digit(c) || (c == '.' && i + 1 < n && is_digit((u8)s[i + 1]))) {
			u32 j = c_number(s, n, i);
			emit(o, i, j - i, TK_NUMBER);
			i = j;
			prev = P_NONE;
			continue;
		}
		if (is_cident(c)) {
			u32 j = i + 1;
			while (j < n && is_cident((u8)s[j]))
				j++;
			u32 len = j - i;
			if (j < n && (s[j] == '"' || s[j] == '\'') && c_str_prefix(s + i, len)) {
				bool cont;
				u8 q = (u8)s[j];
				i = c_string(s, n, j + 1, i, q, o, &cont);
				if (cont)
					return C_STATE(q == '"' ? CM_STRING : CM_CHAR, line_pp, kind, if0_next);
				prev = P_NONE;
				continue;
			}
			u32 nx = skip_ws(s, n, j);
			u32 cls;
			int k;
			if (prev == P_DEFINE) {
				cls = TK_PREPROC; /* entity.name.function.preprocessor */
				in_params = j < n && s[j] == '(';
			} else if (in_params) {
				cls = TK_VARIABLE; /* variable.parameter.preprocessor */
			} else if (prev == P_PRAGMA) {
				cls = TK_VARIABLE; /* entity.other.attribute-name.pragma */
			} else if ((k = kw_find(&kw_c, s + i, len)) >= 0) {
				if (k == KC_TAG)
					newprev = P_TAG;
				else if (k == KC_GOTO)
					newprev = P_GOTO;
				if (k == KC_DEFINED)
					cls = (line_pp && kind == PPK_COND) ? TK_CONTROL : dflt;
				else
					cls = kw_tk((u32)k);
			} else if (attr) {
				cls = TK_TYPE;
			} else if (prev == P_TAG) {
				cls = TK_TYPE;
			} else if (prev == P_GOTO) {
				cls = TK_LABEL;
			} else if (line_pp && kind == PPK_COND) {
				cls = TK_PREPROC;
			} else if (nx < n && s[nx] == '(') {
				cls = TK_FUNCTION;
			} else if (len > 2 && s[j - 2] == '_' && s[j - 1] == 't') {
				cls = TK_TYPE; /* support.type.posix-reserved */
			} else if (prev == P_DOT) {
				cls = TK_VARIABLE; /* variable.other.member */
			} else if (nx < n && ((s[nx] == '.' && !(nx + 1 < n && s[nx + 1] == '.')) ||
			                      (s[nx] == '-' && nx + 1 < n && s[nx + 1] == '>'))) {
				cls = TK_VARIABLE; /* variable.other.object.access */
			} else {
				cls = dflt;
			}
			emit(o, i, len, cls);
			i = j;
			prev = newprev;
			continue;
		}
		/* punctuation and operators */
		if (c == '[' && i + 1 < n && s[i + 1] == '[') {
			attr++;
			emit(o, i, 2, dflt);
			i += 2;
		} else if (c == ']' && attr && i + 1 < n && s[i + 1] == ']') {
			attr--;
			emit(o, i, 2, dflt);
			i += 2;
		} else if (c == '-' && i + 1 < n && s[i + 1] == '>') {
			emit(o, i, 2, dflt);
			i += 2;
			newprev = P_DOT;
		} else if (c == '.') {
			if (i + 2 < n && s[i + 1] == '.' && s[i + 2] == '.') {
				emit(o, i, 3, in_params ? TK_VARIABLE : dflt);
				i += 3;
			} else {
				emit(o, i, 1, dflt);
				i++;
				newprev = P_DOT;
			}
		} else if (c == '\\' && i + 1 == n) {
			emit(o, i, 1, TK_ESCAPE); /* line continuation */
			i++;
		} else if (c && strchr("+-*/%=&|^!~<>?:", c)) {
			emit(o, i, 1, TK_OPERATOR);
			i++;
		} else {
			if (c == ')' && in_params)
				in_params = false;
			emit(o, i, 1, dflt);
			i++;
		}
		prev = newprev;
	}

	bool cont_pp = line_pp && n && s[n - 1] == '\\';
	return C_STATE(CM_NORMAL, cont_pp, cont_pp ? kind : 0, if0_next);
}

/* ================================================================== */
/* x86 assembly                                                        */

static const char *const asm_cc[] = {
	"o", "no", "b", "c", "nae", "ae", "nb", "nc", "e", "z", "ne", "nz", "be",
	"na", "a", "nbe", "s", "ns", "p", "pe", "np", "po", "l", "nge", "ge", "nl",
	"le", "ng", "g", "nle",
};

static bool asm_is_cc(const char *w, u32 n)
{
	for (u32 i = 0; i < VX_COUNT(asm_cc); i++)
		if (word_is(w, n, asm_cc[i]))
			return true;
	return false;
}

static bool prefix_is(const char *w, u32 n, const char *p, u32 *pl)
{
	size_t l = strlen(p);
	if (n < l || memcmp(w, p, l) != 0)
		return false;
	*pl = (u32)l;
	return true;
}

/* vfmadd132ps and friends. */
static bool asm_fma(const char *w, u32 n)
{
	static const char *const ops[] = { "vfmaddsub", "vfmsubadd", "vfnmadd", "vfnmsub", "vfmadd", "vfmsub" };
	for (u32 i = 0; i < VX_COUNT(ops); i++) {
		u32 l;
		if (!prefix_is(w, n, ops[i], &l))
			continue;
		if (n != l + 5)
			continue;
		const char *r = w + l;
		if (!(memcmp(r, "132", 3) == 0 || memcmp(r, "213", 3) == 0 || memcmp(r, "231", 3) == 0))
			continue;
		r += 3;
		if (r[0] == 'p' || r[0] == 's')
			if (r[1] == 's' || r[1] == 'd' || r[1] == 'h')
				return true;
	}
	return false;
}

/* AVX-512 mask instructions: kandw, kmovq, kshiftlb, kunpckbw, ... */
static bool asm_kmask(const char *w, u32 n)
{
	static const char *const ops[] = { "kandn", "kand", "kor", "kxnor", "kxor", "knot", "kmov",
	                                   "kshiftl", "kshiftr", "kortest", "ktest", "kadd", "kunpck" };
	for (u32 i = 0; i < VX_COUNT(ops); i++) {
		u32 l;
		if (!prefix_is(w, n, ops[i], &l))
			continue;
		const char *r = w + l;
		u32 rl = n - l;
		if ((rl == 1 && strchr("bwdq", r[0])) ||
		    (rl == 2 && (memcmp(r, "bw", 2) == 0 || memcmp(r, "wd", 2) == 0 || memcmp(r, "dq", 2) == 0)))
			return true;
	}
	return false;
}

/* Mnemonic class for a lowercase word, or -1. */
static int asm_mnemonic(const char *w, u32 n, bool gas)
{
	int k = kw_find(&kw_mn, w, n);
	if (k >= 0)
		return k;
	u32 l;
	if (n > 1 && w[0] == 'j' && asm_is_cc(w + 1, n - 1))
		return TK_CONTROL;
	if (prefix_is(w, n, "set", &l) && asm_is_cc(w + l, n - l))
		return TK_STORAGE;
	if (prefix_is(w, n, "cmov", &l) && asm_is_cc(w + l, n - l))
		return TK_STORAGE;
	if (asm_fma(w, n) || asm_kmask(w, n))
		return TK_STORAGE;
	if (n > 2 && w[0] == 'v' && kw_find(&kw_mn, w + 1, n - 1) == TK_STORAGE)
		return TK_STORAGE;
	/* AT&T size suffixes: movl, pushq, fldt, ... */
	if (gas && n > 2 && strchr("bwlqst", w[n - 1])) {
		k = asm_mnemonic(w, n - 1, false);
		if (k >= 0)
			return k;
	}
	return -1;
}

static bool num_suffix(const char *w, u32 n, u32 base_len, u32 *v, u32 lo, u32 hi)
{
	if (n <= base_len || n > base_len + 2)
		return false;
	u32 x = 0;
	for (u32 i = base_len; i < n; i++) {
		if (!is_digit((u8)w[i]))
			return false;
		x = x * 10 + (u32)(w[i] - '0');
	}
	if (n - base_len == 2 && w[base_len] == '0')
		return false;
	*v = x;
	return x >= lo && x <= hi;
}

/* Register name test for a lowercase word. */
static bool asm_register(const char *w, u32 n)
{
	if (kw_find(&kw_reg, w, n) >= 0)
		return true;
	u32 v;
	if (n >= 2 && w[0] == 'r' && is_digit((u8)w[1])) { /* r8..r15 + b/w/d/l */
		u32 m = n;
		if (strchr("bwdl", w[n - 1]))
			m--;
		return num_suffix(w, m, 1, &v, 8, 15);
	}
	if (n >= 4 && (w[0] == 'x' || w[0] == 'y' || w[0] == 'z') && w[1] == 'm' && w[2] == 'm')
		return num_suffix(w, n, 3, &v, 0, 31);
	if (n >= 3 && w[0] == 'm' && w[1] == 'm')
		return num_suffix(w, n, 2, &v, 0, 7);
	if (n >= 2 && w[0] == 'k')
		return num_suffix(w, n, 1, &v, 0, 7);
	if (n >= 3 && w[0] == 'c' && w[1] == 'r')
		return num_suffix(w, n, 2, &v, 0, 15);
	if (n >= 3 && w[0] == 'd' && w[1] == 'r')
		return num_suffix(w, n, 2, &v, 0, 15);
	if (n >= 3 && w[0] == 't' && w[1] == 'r')
		return num_suffix(w, n, 2, &v, 0, 7);
	if (n >= 3 && w[0] == 's' && w[1] == 't')
		return num_suffix(w, n, 2, &v, 0, 7);
	if (n >= 4 && memcmp(w, "bnd", 3) == 0)
		return num_suffix(w, n, 3, &v, 0, 3);
	if (n >= 4 && memcmp(w, "tmm", 3) == 0)
		return num_suffix(w, n, 3, &v, 0, 7);
	return false;
}

static bool asm_ident(u8 c, bool gas)
{
	return is_alpha(c) || is_digit(c) || c == '_' || c == '.' || c == '$' || c >= 0x80 ||
	       (!gas && (c == '@' || c == '?' || c == '#' || c == '~'));
}

static const char *const cpp_dirs[] = {
	"include", "define", "undef", "if", "ifdef", "ifndef", "elif", "else", "endif",
	"error", "warning", "pragma", "line", "include_next", "elifdef", "elifndef",
};

/* `#include` etc. at line start in .S files (C preprocessed assembly). */
static bool asm_cpp_line(const char *s, u32 n, SynSpans *o)
{
	u32 j = skip_ws(s, n, 0);
	if (j >= n || s[j] != '#')
		return false;
	u32 a = skip_ws(s, n, j + 1), b = a;
	while (b < n && is_cident((u8)s[b]))
		b++;
	bool hit = false;
	for (u32 k = 0; k < VX_COUNT(cpp_dirs); k++)
		if (word_is(s + a, b - a, cpp_dirs[k]))
			hit = true;
	if (!hit)
		return false;
	emit(o, j, b - j, TK_CONTROL);
	u32 i = b;
	while (i < n) {
		u8 c = (u8)s[i];
		if (c == '<' || c == '"') {
			u8 q = c == '<' ? '>' : '"';
			u32 e = i + 1;
			while (e < n && s[e] != q)
				e++;
			if (e < n)
				e++;
			emit(o, i, e - i, TK_STRING);
			i = e;
		} else if (c == '/' && i + 1 < n && s[i + 1] == '/') {
			emit(o, i, n - i, TK_COMMENT);
			break;
		} else if (is_digit(c)) {
			u32 e = c_number(s, n, i);
			emit(o, i, e - i, TK_NUMBER);
			i = e;
		} else if (is_cident(c)) {
			u32 e = i;
			while (e < n && is_cident((u8)s[e]))
				e++;
			emit(o, i, e - i, word_is(s + i, e - i, "defined") ? TK_CONTROL : TK_PREPROC);
			i = e;
		} else {
			i++;
		}
	}
	return true;
}

static u32 lex_asm(bool gas, u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 i = 0;
	if (state == 1) {
		bool closed;
		i = c_block(s, n, 0, 0, false, o, &closed);
		if (!closed)
			return 1;
	} else if (asm_cpp_line(s, n, o)) {
		return 0;
	}

	bool stmt = true; /* expecting a label or a mnemonic/directive */
	char w[64];
	while (i < n) {
		u8 c = (u8)s[i];
		if (is_space(c)) {
			i++;
			continue;
		}
		if ((gas && c == '#') || (!gas && (c == ';' || c == '#')) ||
		    (c == '/' && i + 1 < n && s[i + 1] == '/')) {
			emit(o, i, n - i, TK_COMMENT);
			return 0;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '*') {
			bool closed;
			i = c_block(s, n, i, i + 2, false, o, &closed);
			if (!closed)
				return 1;
			continue;
		}
		if (gas && c == ';') {
			stmt = true;
			i++;
			continue;
		}
		if (c == '"' || (!gas && (c == '\'' || c == '`'))) {
			u32 j = i + 1, run = i;
			while (j < n && s[j] != (char)c) {
				if (s[j] == '\\' && (gas || c == '`') && j + 1 < n) {
					emit(o, run, j - run, TK_STRING);
					emit(o, j, 2, TK_ESCAPE);
					j += 2;
					run = j;
					continue;
				}
				j++;
			}
			if (j < n)
				j++;
			emit(o, run, j - run, TK_STRING);
			i = j;
			stmt = false;
			continue;
		}
		if (gas && c == '\'') { /* 'c character constant */
			u32 j = i + 1;
			if (j < n && s[j] == '\\')
				j++;
			if (j < n)
				j++;
			if (j < n && s[j] == '\'')
				j++;
			emit(o, i, j - i, TK_STRING);
			i = j;
			continue;
		}
		if (gas && c == '%' && i + 1 < n && is_alpha((u8)s[i + 1])) {
			u32 j = i + 1;
			while (j < n && (is_alpha((u8)s[j]) || is_digit((u8)s[j])))
				j++;
			u32 l = j - i - 1;
			bool reg = false;
			if (l < sizeof w) {
				for (u32 k = 0; k < l; k++)
					w[k] = (char)lower((u8)s[i + 1 + k]);
				reg = asm_register(w, l);
			}
			emit(o, i, j - i, reg ? TK_VARIABLE : TK_TEXT);
			i = j;
			continue;
		}
		if (gas && c == '$') {
			u32 j = i + 1;
			if (j < n && s[j] == '-')
				j++;
			if (j < n && is_digit((u8)s[j])) {
				u32 e = c_number(s, n, j);
				emit(o, i, e - i, TK_NUMBER);
				i = e;
			} else {
				emit(o, i, 1, TK_OPERATOR);
				i++;
			}
			continue;
		}
		if (is_digit(c)) {
			u32 j = i + 1;
			while (j < n && (is_alpha((u8)s[j]) || is_digit((u8)s[j]) || s[j] == '_' || s[j] == '.'))
				j++;
			if (stmt && j < n && s[j] == ':') { /* numeric local label */
				emit(o, i, j - i, TK_FUNCTION);
				i = j + 1;
				continue;
			}
			emit(o, i, j - i, TK_NUMBER);
			i = j;
			continue;
		}
		bool nasm_pct = !gas && c == '%' && i + 1 < n && (is_alpha((u8)s[i + 1]) || s[i + 1] == '%');
		if (nasm_pct || (asm_ident(c, gas) && c != '#')) {
			u32 j = i + 1;
			if (nasm_pct && s[j] == '%')
				j++;
			while (j < n && asm_ident((u8)s[j], gas))
				j++;
			u32 len = j - i;
			u32 l = len < sizeof w ? len : 0;
			for (u32 k = 0; k < l; k++)
				w[k] = (char)lower((u8)s[i + k]);
			u32 nx = skip_ws(s, n, j);
			if (stmt && nx < n && s[nx] == ':' && !(nx + 1 < n && s[nx + 1] == ':')) {
				emit(o, i, len, TK_FUNCTION); /* label: */
				i = nx + 1;
				continue;
			}
			u32 cls = TK_TEXT;
			if (len == 1 && c == '$') {
				cls = TK_LANGCONST;
			} else if (len == 2 && c == '$' && s[i + 1] == '$') {
				cls = TK_LANGCONST;
			} else if (stmt) {
				int k;
				if (c == '.' || c == '%') {
					cls = TK_CONTROL;
					stmt = false;
				} else if (!gas && l && (k = kw_find(&kw_nasm, w, l)) >= 0) {
					cls = TK_CONTROL;
					stmt = false;
				} else if (l && (k = asm_mnemonic(w, l, gas)) >= 0) {
					cls = kw_tk((u32)k);
					stmt = k == KC_PREFIX;
				} else {
					/* NASM/MASM label without a colon: `msg db ...` */
					u32 a = nx, b = nx;
					while (b < n && asm_ident((u8)s[b], gas))
						b++;
					char w2[32];
					u32 l2 = b - a < sizeof w2 ? b - a : 0;
					for (u32 q = 0; q < l2; q++)
						w2[q] = (char)lower((u8)s[a + q]);
					if (!gas && l2 && (kw_find(&kw_nasm, w2, l2) >= 0 || asm_mnemonic(w2, l2, gas) >= 0)) {
						cls = TK_FUNCTION;
					} else {
						cls = TK_STORAGE; /* macro invocation in mnemonic position */
						stmt = false;
					}
				}
			} else if (l) {
				if (!gas && asm_register(w, l))
					cls = TK_VARIABLE;
				else if (!gas && kw_find(&kw_size, w, l) >= 0)
					cls = TK_STORAGE;
				else if (!gas && kw_find(&kw_nasm, w, l) >= 0)
					cls = TK_CONTROL;
				else if (c == '@' || (gas && w[0] == '@'))
					cls = TK_STORAGE;
			}
			emit(o, i, len, cls);
			i = j;
			continue;
		}
		if (c && strchr("+-*/<>|&^~!=", c))
			emit(o, i, 1, TK_OPERATOR);
		i++;
	}
	return 0;
}

/* ================================================================== */
/* Lisp                                                                */

enum { LM_NORMAL, LM_STRING, LM_BLOCK };

static inline bool lisp_sym(u8 c)
{
	return !(is_space(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
	         c == '}' || c == '"' || c == '\'' || c == ';' || c == '`' || c == ',');
}

static u32 lisp_block(const char *s, u32 n, u32 st, u32 i, u32 *depth, SynSpans *o)
{
	while (i < n && *depth) {
		if (s[i] == '|' && i + 1 < n && s[i + 1] == '#') {
			(*depth)--;
			i += 2;
		} else if (s[i] == '#' && i + 1 < n && s[i + 1] == '|') {
			if (*depth < 0xFFFFFFu)
				(*depth)++;
			i += 2;
		} else {
			i++;
		}
	}
	emit(o, st, i - st, TK_COMMENT);
	return i;
}

static u32 lisp_string(const char *s, u32 n, u32 i, u32 st, SynSpans *o, bool *closed)
{
	u32 run = st;
	while (i < n) {
		if (s[i] == '"') {
			emit(o, run, i + 1 - run, TK_STRING);
			*closed = true;
			return i + 1;
		}
		if (s[i] == '\\') {
			emit(o, run, i - run, TK_STRING);
			u32 e = i + 2 <= n ? i + 2 : n;
			emit(o, i, e - i, TK_ESCAPE);
			i = run = e;
			continue;
		}
		i++;
	}
	emit(o, run, n - run, TK_STRING);
	*closed = false;
	return n;
}

static bool lisp_number(const char *s, u32 n)
{
	u32 i = 0;
	if (i < n && (s[i] == '+' || s[i] == '-'))
		i++;
	u32 d0 = i;
	while (i < n && is_digit((u8)s[i]))
		i++;
	bool digits = i > d0;
	if (i < n && s[i] == '/') { /* ratio */
		u32 d1 = ++i;
		while (i < n && is_digit((u8)s[i]))
			i++;
		return digits && i > d1 && i == n;
	}
	if (i < n && s[i] == '.') {
		i++;
		u32 d1 = i;
		while (i < n && is_digit((u8)s[i]))
			i++;
		digits = digits || i > d1;
	}
	if (!digits)
		return false;
	if (i < n && strchr("eEdDfFsSlL", s[i])) {
		u32 k = i + 1;
		if (k < n && (s[k] == '+' || s[k] == '-'))
			k++;
		u32 d2 = k;
		while (k < n && is_digit((u8)s[k]))
			k++;
		if (k > d2)
			i = k;
	}
	if (i < n && (s[i] == 'N' || s[i] == 'M') && i + 1 == n) /* Clojure */
		i++;
	return i == n;
}

static u32 lex_lisp(u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 mode = state & 3u, depth = state >> 8;
	u32 i = 0;
	if (mode == LM_BLOCK) {
		i = lisp_block(s, n, 0, 0, &depth, o);
		if (depth)
			return LM_BLOCK | (depth << 8);
	} else if (mode == LM_STRING) {
		bool closed;
		i = lisp_string(s, n, 0, 0, o, &closed);
		if (!closed)
			return LM_STRING;
	}

	bool head = false;
	u32 expect = 0; /* KC_DEF* pending a name */
	while (i < n) {
		u8 c = (u8)s[i];
		if (is_space(c)) {
			i++;
			continue;
		}
		if (c == ';') {
			emit(o, i, n - i, TK_COMMENT);
			return LM_NORMAL;
		}
		if (c == '#' && i + 1 < n && s[i + 1] == '|') {
			depth = 1;
			i = lisp_block(s, n, i, i + 2, &depth, o);
			if (depth)
				return LM_BLOCK | (depth << 8);
			continue;
		}
		if (c == '"') {
			bool closed;
			i = lisp_string(s, n, i + 1, i, o, &closed);
			if (!closed)
				return LM_STRING;
			head = false;
			expect = 0;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			bool quoted = i > 0 && (s[i - 1] == '\'' || s[i - 1] == '`');
			head = c == '(' && !quoted;
			expect = 0;
			i++;
			continue;
		}
		if (c == ')' || c == ']' || c == '}') {
			head = false;
			expect = 0;
			i++;
			continue;
		}
		if (c == '\'' || c == '`') {
			emit(o, i, 1, TK_OPERATOR);
			i++;
			continue;
		}
		if (c == ',') {
			u32 l = i + 1 < n && s[i + 1] == '@' ? 2 : 1;
			emit(o, i, l, TK_OPERATOR);
			i += l;
			continue;
		}
		if (c == '#' && i + 1 < n) {
			u8 d = (u8)s[i + 1];
			if (d == '\\') { /* character #\a #\Space */
				u32 j = i + 2;
				if (j < n) {
					u32 cp;
					j += utf8_decode(s + j, n - j, &cp);
				}
				while (j < n && lisp_sym((u8)s[j]) && is_alpha((u8)s[j - 1]) && is_alpha((u8)s[j]))
					j++;
				emit(o, i, j - i, TK_LANGCONST);
				i = j;
				head = false;
				continue;
			}
			if (d == '\'') {
				emit(o, i, 2, TK_OPERATOR);
				i += 2;
				continue;
			}
			if (d == '(' || d == '{' || d == '_' || d == '?') {
				i++; /* reader dispatch: the bracket is handled next */
				continue;
			}
		}
		/* symbol or number */
		u32 j = i;
		while (j < n && lisp_sym((u8)s[j]))
			j++;
		if (j == i) {
			i++;
			continue;
		}
		u32 len = j - i;
		u32 cls;
		int k;
		if (c == ':') {
			cls = TK_CONSTANT;
		} else if (c == '&') {
			cls = TK_STORAGE;
		} else if (c == '#' && len > 1 && strchr("xXbBoO", s[i + 1])) {
			cls = TK_NUMBER;
		} else if (c == '#' && len > 2 && is_digit((u8)s[i + 1]) &&
		           (memchr(s + i + 1, 'r', len - 1) || memchr(s + i + 1, 'R', len - 1))) {
			cls = TK_NUMBER;
		} else if (lisp_number(s + i, len)) {
			cls = TK_NUMBER;
		} else if (expect) {
			cls = expect == KC_DEFFN ? TK_FUNCTION : expect == KC_DEFVAR ? TK_VARIABLE : TK_TYPE;
			expect = 0;
		} else if ((k = kw_find_ci(&kw_lisp, s + i, len)) >= 0) {
			cls = kw_tk((u32)k);
			if (k == KC_DEFFN || k == KC_DEFVAR || k == KC_DEFTYPE)
				expect = (u32)k;
		} else if (head) {
			cls = TK_FUNCTION;
		} else {
			cls = TK_TEXT;
		}
		emit(o, i, len, cls);
		head = false;
		i = j;
	}
	return LM_NORMAL;
}

/* ================================================================== */
/* Public API                                                          */

u32 syn_line(int lang, u32 state, const char *s, u32 n, SynSpans *out)
{
	syn_init();
	if (out)
		out->count = 0;
	switch (lang) {
	case LANG_C: return lex_c(state, s, n, out);
	case LANG_ASM_GAS: return lex_asm(true, state, s, n, out);
	case LANG_ASM_INTEL: return lex_asm(false, state, s, n, out);
	case LANG_LISP: return lex_lisp(state, s, n, out);
	default: return 0;
	}
}

const char *syn_lang_name(int lang)
{
	switch (lang) {
	case LANG_C: return "C";
	case LANG_ASM_GAS: return "x86 Assembly (AT&T)";
	case LANG_ASM_INTEL: return "x86 Assembly (Intel)";
	case LANG_LISP: return "Lisp";
	default: return "Plain Text";
	}
}

const char *syn_lang_lsp_id(int lang)
{
	switch (lang) {
	case LANG_C: return "c";
	case LANG_ASM_GAS:
	case LANG_ASM_INTEL: return "asm";
	case LANG_LISP: return "lisp";
	default: return NULL;
	}
}

const char *syn_line_comment(int lang)
{
	switch (lang) {
	case LANG_C: return "//";
	case LANG_ASM_GAS: return "#";
	case LANG_ASM_INTEL:
	case LANG_LISP: return ";";
	default: return NULL;
	}
}

/* Score the first bytes of an assembly file: >0 AT&T, <0 Intel. */
static int asm_guess(const char *h, size_t n)
{
	syn_init();
	int score = 0;
	size_t i = 0;
	if (n > 8192)
		n = 8192;
	while (i < n) {
		size_t e = i;
		while (e < n && h[e] != '\n')
			e++;
		const char *l = h + i;
		u32 ln = (u32)(e - i);
		u32 a = skip_ws(l, ln, 0), b = a;
		while (b < ln && (is_alpha((u8)l[b]) || l[b] == '.' || l[b] == '_' || l[b] == '%'))
			b++;
		char w[24];
		u32 wl = b - a < sizeof w ? b - a : 0;
		for (u32 k = 0; k < wl; k++)
			w[k] = (char)lower((u8)l[a + k]);
		if (wl) {
			if (word_is(w, wl, ".att_syntax"))
				score += 20;
			else if (word_is(w, wl, ".intel_syntax"))
				score -= 20;
			else if (word_is(w, wl, "section") || word_is(w, wl, "segment") || word_is(w, wl, "bits") ||
			         word_is(w, wl, "global") || word_is(w, wl, "extern") || word_is(w, wl, "default") ||
			         word_is(w, wl, "%define") || word_is(w, wl, "%include") || word_is(w, wl, "%macro") ||
			         word_is(w, wl, "org") || word_is(w, wl, "cpu"))
				score -= 3;
			else if (word_is(w, wl, ".globl") || word_is(w, wl, ".text") || word_is(w, wl, ".section") ||
			         word_is(w, wl, ".type") || word_is(w, wl, ".p2align") || word_is(w, wl, ".cfi_startproc"))
				score += 2;
		}
		if (a < ln && l[a] == ';')
			score -= 1;
		if (a < ln && l[a] == '#' && !(a + 1 < ln && is_alpha((u8)l[a + 1])))
			score += 1;
		bool pct = false, brk = false;
		for (u32 k = 0; k + 1 < ln; k++) {
			if (l[k] == ';' || l[k] == '#')
				break;
			if (l[k] == '%' && is_alpha((u8)l[k + 1])) {
				u32 m = k + 1, q = 0;
				char r[8];
				while (m < ln && q < sizeof r && (is_alpha((u8)l[m]) || is_digit((u8)l[m])))
					r[q++] = (char)lower((u8)l[m++]);
				if (asm_register(r, q))
					pct = true;
			}
			if (l[k] == '[')
				brk = true;
		}
		if (pct)
			score += 3;
		if (brk && !pct)
			score -= 2;
		i = e + 1;
	}
	return score;
}

int syn_lang_for_path(const char *path, const char *head, size_t headlen)
{
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	const char *dot = strrchr(base, '.');
	if (!dot || dot == base)
		return LANG_NONE;
	char e[12];
	size_t el = strlen(dot + 1);
	if (el == 0 || el >= sizeof e)
		return LANG_NONE;
	for (size_t i = 0; i <= el; i++)
		e[i] = (char)lower((u8)dot[1 + i]);
	if (!strcmp(e, "c") || !strcmp(e, "h"))
		return LANG_C;
	static const char *const lisp[] = { "lisp", "lsp", "cl", "el", "scm", "ss", "rkt", "clj",
	                                    "cljs", "cljc", "edn", "fnl", "asd", "sld", "sls" };
	for (size_t i = 0; i < VX_COUNT(lisp); i++)
		if (!strcmp(e, lisp[i]))
			return LANG_LISP;
	if (!strcmp(e, "s") || !strcmp(e, "sx")) {
		int sc = head ? asm_guess(head, headlen) : 0;
		return sc < 0 ? LANG_ASM_INTEL : LANG_ASM_GAS;
	}
	if (!strcmp(e, "asm") || !strcmp(e, "nasm") || !strcmp(e, "inc") || !strcmp(e, "yasm") ||
	    !strcmp(e, "masm") || !strcmp(e, "mac")) {
		int sc = head ? asm_guess(head, headlen) : 0;
		return sc > 0 ? LANG_ASM_GAS : LANG_ASM_INTEL;
	}
	return LANG_NONE;
}
