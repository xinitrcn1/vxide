/* Syntax highlighting lexers: C23, x86 assembly (AT&T and Intel), Lisp,
 * shell, Perl, troff and Markdown (which embeds the others in fences).
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

static void syn_build2(void);

static void syn_build(void)
{
	syn_build2();
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
/* Shared helpers for the script/markup lexers                         */

static inline bool is_word(u8 c) { return is_alpha(c) || is_digit(c) || c == '_'; }
static inline bool in_set(const char *set, u8 c) { return c && strchr(set, c) != NULL; }

/* 15-bit hash of a heredoc delimiter (fits the 20-bit state budget). */
static u32 hash15(const char *s, u32 n)
{
	u32 h = kw_hash(s, n);
	return (h ^ (h >> 15) ^ (h >> 30)) & 0x7FFFu;
}

static KwSet kw_sh, kw_pl, kw_mdoc;

/* ================================================================== */
/* Shell (sh, bash, zsh, ksh)                                          */
/*
 * Follows VSCodium's shell-unix-bash grammar under Dark+: command names
 * (entity.name.function.call / support.function.builtin) are functions,
 * unquoted arguments are string.unquoted.argument (string colour), options
 * are constant.other.option, $1/$@/$name are variables and $? $$ $# ...
 * are variable.language.
 *
 * State: bits 0-2 mode, bit 3 `<<-` (strip leading tabs), bit 4 quoted
 * delimiter, bits 5-19 hash15 of the heredoc delimiter. */
enum { SH_NORMAL, SH_SQ, SH_DQ, SH_DSQ, SH_HEREDOC };
#define SH_STRIP 0x08u
#define SH_RAWHD 0x10u

enum { SHK_CTRL = 1, SHK_CTRL_CMD, SHK_END, SHK_LOOP, SHK_IN, SHK_FUNCTION, SHK_DECL };

static const char sh_ctrl_cmd[] = "if then else elif do while until time ! { coproc";
static const char sh_end[] = "fi done esac }";
static const char sh_loop[] = "for select case";
static const char sh_ctrl[] = "return continue break";
static const char sh_decl[] = "export declare typeset local readonly";

typedef struct {
	const char *s;
	u32 n;
	SynSpans *o;
	u32 hd;   /* heredoc state to enter after this line, 0 = none */
	u32 open; /* SH_SQ/SH_DQ/SH_DSQ when the line ends in a string */
} Sh;

static u32 sh_seg(Sh *c, u32 i, u8 stop, int depth);

static inline bool sh_meta(u8 ch)
{
	return is_space(ch) || ch == ';' || ch == '&' || ch == '|' || ch == '<' || ch == '>' || ch == '(' ||
	       ch == ')' || ch == '`' || ch == '\'' || ch == '"' || ch == '$' || ch == '\\';
}

/* $... at i; returns the end. Only plain variable forms. */
static u32 sh_var(Sh *c, u32 i)
{
	const char *s = c->s;
	u32 n = c->n, j = i + 1;
	if (j >= n) {
		emit(c->o, i, 1, TK_STRING);
		return n;
	}
	u8 ch = (u8)s[j];
	if (ch == '{') {
		int d = 0;
		u32 k = j;
		for (; k < n; k++) {
			if (s[k] == '{')
				d++;
			else if (s[k] == '}' && --d == 0) {
				k++;
				break;
			}
		}
		emit(c->o, i, k - i, TK_VARIABLE);
		return k;
	}
	if (ch == '@' || is_digit(ch)) {
		emit(c->o, i, 2, TK_VARIABLE);
		return i + 2;
	}
	if (in_set("-*#?$!_", ch)) {
		emit(c->o, i, 2, TK_LANGCONST);
		return i + 2;
	}
	if (is_alpha(ch) || ch == '_') {
		while (j < n && is_word((u8)s[j]))
			j++;
		emit(c->o, i, j - i, TK_VARIABLE);
		return j;
	}
	emit(c->o, i, 1, TK_STRING);
	return i + 1;
}

/* Arithmetic $(( )) / (( )) body starting at i; returns after "))". */
static u32 sh_arith(Sh *c, u32 i)
{
	const char *s = c->s;
	u32 n = c->n;
	int d = 0;
	while (i < n) {
		u8 ch = (u8)s[i];
		if (ch == ')' && d == 0) {
			u32 e = (i + 1 < n && s[i + 1] == ')') ? i + 2 : i + 1;
			return e;
		}
		if (ch == '(') {
			d++;
			i++;
		} else if (ch == ')') {
			d--;
			i++;
		} else if (ch == '$') {
			i = sh_var(c, i);
		} else if (is_digit(ch)) {
			u32 j = i;
			while (j < n && (is_word((u8)s[j]) || s[j] == '#'))
				j++;
			emit(c->o, i, j - i, TK_NUMBER);
			i = j;
		} else if (is_alpha(ch) || ch == '_') {
			u32 j = i;
			while (j < n && is_word((u8)s[j]))
				j++;
			emit(c->o, i, j - i, TK_VARIABLE);
			i = j;
		} else if (in_set("+-*/%<>=!&|^~?:,", ch)) {
			emit(c->o, i, 1, TK_OPERATOR);
			i++;
		} else {
			i++;
		}
	}
	return n;
}

/* "..." body from i (after the quote). *closed tells if it ended. */
static u32 sh_dq(Sh *c, u32 i, int depth, bool *closed)
{
	const char *s = c->s;
	u32 n = c->n;
	*closed = false;
	while (i < n) {
		u8 ch = (u8)s[i];
		if (ch == '"') {
			emit(c->o, i, 1, TK_STRING);
			*closed = true;
			return i + 1;
		}
		if (ch == '\\') {
			if (i + 1 >= n) {
				emit(c->o, i, 1, TK_ESCAPE);
				return n;
			}
			u8 e = (u8)s[i + 1];
			emit(c->o, i, 2, (e == '$' || e == '`' || e == '"' || e == '\\') ? TK_ESCAPE : TK_STRING);
			i += 2;
			continue;
		}
		if (ch == '$' && i + 1 < n && s[i + 1] == '(' && depth < 8) {
			if (i + 2 < n && s[i + 2] == '(') {
				emit(c->o, i, 3, TK_STRING);
				i = sh_arith(c, i + 3);
				continue;
			}
			emit(c->o, i, 2, TK_STRING);
			i = sh_seg(c, i + 2, ')', depth + 1);
			if (i < n) {
				emit(c->o, i, 1, TK_STRING);
				i++;
			}
			continue;
		}
		if (ch == '$' && i + 1 < n && !is_space((u8)s[i + 1]) && s[i + 1] != '"') {
			i = sh_var(c, i);
			continue;
		}
		if (ch == '`' && depth < 8) {
			emit(c->o, i, 1, TK_STRING);
			i = sh_seg(c, i + 1, '`', depth + 1);
			if (i < n) {
				emit(c->o, i, 1, TK_STRING);
				i++;
			}
			continue;
		}
		emit(c->o, i, 1, TK_STRING);
		i++;
	}
	return n;
}

static u32 sh_sq(Sh *c, u32 i, bool dollar, bool *closed)
{
	const char *s = c->s;
	u32 n = c->n;
	*closed = false;
	while (i < n) {
		if (dollar && s[i] == '\\' && i + 1 < n) {
			emit(c->o, i, 2, TK_ESCAPE);
			i += 2;
			continue;
		}
		if (s[i] == '\'') {
			emit(c->o, i, 1, TK_STRING);
			*closed = true;
			return i + 1;
		}
		emit(c->o, i, 1, TK_STRING);
		i++;
	}
	return n;
}

static bool sh_is_number(const char *w, u32 n)
{
	u32 i = 0;
	if (i < n && w[i] == '-')
		i++;
	if (i >= n)
		return false;
	if (n - i > 2 && w[i] == '0' && (w[i + 1] | 32) == 'x') {
		for (u32 k = i + 2; k < n; k++)
			if (!is_xdigit((u8)w[k]))
				return false;
		return true;
	}
	bool dig = false;
	for (; i < n; i++) {
		if (is_digit((u8)w[i]))
			dig = true;
		else if (w[i] != '.')
			return false;
	}
	return dig;
}

/* Heredoc operator at i ("<<" not "<<<"); records the pending state. */
static u32 sh_heredoc_op(Sh *c, u32 i)
{
	const char *s = c->s;
	u32 n = c->n, k = i + 2;
	u32 flags = 0;
	if (k < n && s[k] == '-') {
		flags |= SH_STRIP;
		k++;
	}
	emit(c->o, i, k - i, TK_OPERATOR);
	k = skip_ws(s, n, k);
	u32 a, b;
	if (k < n && (s[k] == '\'' || s[k] == '"')) {
		u8 q = (u8)s[k];
		a = k + 1;
		b = a;
		while (b < n && (u8)s[b] != q)
			b++;
		flags |= SH_RAWHD;
		k = b < n ? b + 1 : n;
	} else {
		if (k < n && s[k] == '\\') {
			flags |= SH_RAWHD;
			k++;
		}
		a = k;
		while (k < n && !sh_meta((u8)s[k]))
			k++;
		b = k;
	}
	if (b > a && !c->hd)
		c->hd = SH_HEREDOC | flags | (hash15(s + a, b - a) << 5);
	return k;
}

static u32 sh_seg(Sh *c, u32 i, u8 stop, int depth)
{
	const char *s = c->s;
	u32 n = c->n;
	bool cmd = true;    /* at a command position             */
	bool fn_next = false; /* the word after `function`        */
	bool plain_next = false; /* for/select/case subject word     */
	bool want_in = false;
	bool decl = false;  /* export/local ...: NAME= assignments */
	bool test = false;  /* inside [ ] / [[ ]]                   */
	bool in_assign = false; /* inside NAME=value: quotes keep cmd  */
	int par = 0;
	while (i < n) {
		u8 ch = (u8)s[i];
		if (stop && ch == stop && (stop != ')' || par == 0))
			return i;
		if (is_space(ch)) {
			i++;
			in_assign = false;
			continue;
		}
		if (ch == '#' && (i == 0 || in_set(" \t;&|(", (u8)s[i - 1]))) {
			emit(c->o, i, n - i, TK_COMMENT);
			return n;
		}
		if (ch == '\\') {
			if (i + 1 >= n) {
				emit(c->o, i, 1, TK_ESCAPE);
				return n;
			}
			emit(c->o, i, 2, TK_ESCAPE);
			i += 2;
			cmd = false;
			continue;
		}
		if (ch == ';' || ch == '&' || ch == '|') {
			u32 k = i + 1;
			if (k < n && (s[k] == ch || (ch == '|' && s[k] == '&') || (ch == '&' && s[k] == '>') ||
			              (ch == ';' && s[k] == '&')))
				k++;
			if (ch == '&' && k == i + 2 && s[i + 1] == '>') {
				emit(c->o, i, 2, TK_OPERATOR);
				i = k;
				continue;
			}
			emit(c->o, i, k - i, TK_OPERATOR);
			i = k;
			cmd = true;
			decl = test = false;
			continue;
		}
		if (ch == '<' || ch == '>') {
			if (ch == '<' && i + 1 < n && s[i + 1] == '<') {
				if (i + 2 < n && s[i + 2] == '<') {
					emit(c->o, i, 3, TK_OPERATOR);
					i += 3;
				} else {
					i = sh_heredoc_op(c, i);
				}
				continue;
			}
			if (test) { /* < > compare inside [[ ]] */
				emit(c->o, i, 1, TK_OPERATOR);
				i++;
				continue;
			}
			u32 k = i + 1;
			while (k < n && (s[k] == '>' || s[k] == '&' || s[k] == '|') && k < i + 3)
				k++;
			while (k < n && is_digit((u8)s[k]) && s[k - 1] == '&')
				k++;
			if (k < n && s[k] == '-' && s[k - 1] == '&')
				k++;
			emit(c->o, i, k - i, TK_OPERATOR);
			i = k;
			continue;
		}
		if (ch == '(') {
			if (i + 1 < n && s[i + 1] == '(' && cmd) {
				i = sh_arith(c, i + 2);
				cmd = false;
				continue;
			}
			par++;
			i++;
			cmd = true;
			continue;
		}
		if (ch == ')') {
			if (par > 0)
				par--;
			i++;
			cmd = true; /* case pattern) command */
			continue;
		}
		if (ch == '`') {
			if (depth >= 8) {
				i++;
				continue;
			}
			i = sh_seg(c, i + 1, '`', depth + 1);
			if (i < n)
				i++;
			if (!in_assign)
				cmd = false;
			continue;
		}
		if (ch == '\'') {
			bool closed;
			emit(c->o, i, 1, TK_STRING);
			i = sh_sq(c, i + 1, false, &closed);
			if (!closed)
				c->open = SH_SQ;
			if (!in_assign)
				cmd = false;
			continue;
		}
		if (ch == '"') {
			bool closed;
			emit(c->o, i, 1, TK_STRING);
			i = sh_dq(c, i + 1, depth, &closed);
			if (!closed)
				c->open = SH_DQ;
			if (!in_assign)
				cmd = false;
			continue;
		}
		if (ch == '$') {
			u8 nx = i + 1 < n ? (u8)s[i + 1] : 0;
			if (nx == '\'') {
				bool closed;
				emit(c->o, i, 2, TK_STRING);
				i = sh_sq(c, i + 2, true, &closed);
				if (!closed)
					c->open = SH_DSQ;
			} else if (nx == '"') {
				bool closed;
				emit(c->o, i, 2, TK_STRING);
				i = sh_dq(c, i + 2, depth, &closed);
				if (!closed)
					c->open = SH_DQ;
			} else if (nx == '(' && i + 2 < n && s[i + 2] == '(') {
				i = sh_arith(c, i + 3);
			} else if (nx == '(' && depth < 8) {
				i = sh_seg(c, i + 2, ')', depth + 1);
				if (i < n)
					i++;
			} else {
				i = sh_var(c, i);
			}
			if (!in_assign)
				cmd = false;
			continue;
		}
		/* a word */
		u32 j = i;
		while (j < n && !sh_meta((u8)s[j]))
			j++;
		const char *w = s + i;
		u32 wl = j - i;
		if (wl == 0) {
			i++;
			continue;
		}
		/* N> redirection (2>&1) */
		if (j < n && (s[j] == '>' || s[j] == '<') && sh_is_number(w, wl) && (i == 0 || is_space((u8)s[i - 1]))) {
			emit(c->o, i, wl, TK_OPERATOR);
			i = j;
			continue;
		}
		bool word_start = i == 0 || sh_meta((u8)s[i - 1]) || s[i - 1] == '{';
		int k = word_start ? kw_find(&kw_sh, w, wl) : -1;
		/* NAME= / NAME+= / NAME[i]= assignment */
		u32 eq = 0;
		if ((cmd || decl) && word_start && (is_alpha((u8)w[0]) || w[0] == '_')) {
			u32 q = 0;
			while (q < wl && is_word((u8)w[q]))
				q++;
			if (q < wl && w[q] == '[') {
				while (q < wl && w[q] != ']')
					q++;
				if (q < wl)
					q++;
			}
			if (q < wl && w[q] == '+' && q + 1 < wl && w[q + 1] == '=')
				eq = q;
			else if (q < wl && w[q] == '=')
				eq = q;
		}
		if (eq) {
			u32 ol = w[eq] == '+' ? 2 : 1;
			emit(c->o, i, eq, TK_VARIABLE);
			emit(c->o, i + eq, ol, TK_OPERATOR);
			u32 v = i + eq + ol;
			if (j > v && sh_is_number(s + v, j - v))
				emit(c->o, v, j - v, TK_NUMBER);
			else if (j > v)
				emit(c->o, v, j - v, TK_STRING);
			i = j;
			in_assign = true;
			continue; /* stays at a command position */
		}
		if (cmd && k > 0 && !(want_in && k != SHK_IN)) {
			switch (k) {
			case SHK_CTRL_CMD:
				emit(c->o, i, wl, w[0] == '{' || w[0] == '!' ? TK_TEXT : TK_CONTROL);
				cmd = true;
				break;
			case SHK_END:
				emit(c->o, i, wl, w[0] == '}' ? TK_TEXT : TK_CONTROL);
				cmd = false;
				break;
			case SHK_LOOP:
				emit(c->o, i, wl, TK_CONTROL);
				plain_next = true;
				want_in = true;
				cmd = false;
				break;
			case SHK_FUNCTION:
				emit(c->o, i, wl, TK_STORAGE);
				fn_next = true;
				cmd = false;
				break;
			case SHK_DECL:
				emit(c->o, i, wl, TK_STORAGE);
				decl = true;
				cmd = false;
				break;
			default:
				emit(c->o, i, wl, TK_CONTROL);
				cmd = false;
				break;
			}
			i = j;
			continue;
		}
		if (want_in && k == SHK_IN) {
			emit(c->o, i, wl, TK_CONTROL);
			want_in = false;
			plain_next = false;
			i = j;
			continue;
		}
		if (cmd) {
			/* name() function definition */
			u32 p = skip_ws(s, n, j);
			u32 q = p + 1 < n ? skip_ws(s, n, p + 1) : n;
			if (p < n && s[p] == '(' && q < n && s[q] == ')') {
				emit(c->o, i, wl, TK_FUNCTION);
				i = q + 1;
				cmd = true;
				continue;
			}
			if ((wl == 1 && w[0] == '[') || (wl == 2 && w[0] == '[' && w[1] == '[')) {
				test = true;
				cmd = false;
				i = j;
				continue;
			}
			emit(c->o, i, wl, TK_FUNCTION);
			cmd = false;
			i = j;
			continue;
		}
		if (fn_next) {
			emit(c->o, i, wl, TK_FUNCTION);
			fn_next = false;
		} else if (plain_next) {
			plain_next = false;
			if (!want_in)
				emit(c->o, i, wl, TK_STRING);
		} else if (test && ((wl == 1 && w[0] == ']') || (wl == 2 && w[0] == ']' && w[1] == ']'))) {
			test = false;
		} else if (w[0] == '-' && wl > 1 && !is_digit((u8)w[1])) {
			emit(c->o, i, wl, test ? TK_OPERATOR : TK_LANGCONST);
		} else if (word_start && sh_is_number(w, wl) &&
		           (j >= n || is_space((u8)s[j]) || s[j] == ';' || s[j] == ')' || s[j] == '}')) {
			emit(c->o, i, wl, TK_NUMBER);
		} else if (word_start && (word_is(w, wl, "true") || word_is(w, wl, "false")) &&
		           (j >= n || sh_meta((u8)s[j]))) {
			emit(c->o, i, wl, TK_LANGCONST);
		} else if (test && (word_is(w, wl, "==") || word_is(w, wl, "!=") || word_is(w, wl, "=~") ||
		                    word_is(w, wl, "=") || word_is(w, wl, "!"))) {
			emit(c->o, i, wl, TK_OPERATOR);
		} else {
			/* '*' is variable.language.special.wildcard */
			for (u32 q = 0; q < wl; q++)
				emit(c->o, i + q, 1, w[q] == '*' ? TK_LANGCONST : TK_STRING);
		}
		i = j;
	}
	return n;
}

static u32 lex_sh(u32 state, const char *s, u32 n, SynSpans *o)
{
	Sh c = {s, n, o, 0, 0};
	u32 i = 0;
	u32 mode = state & 7u;
	if (mode == SH_HEREDOC) {
		u32 k = 0;
		if (state & SH_STRIP)
			while (k < n && s[k] == '\t')
				k++;
		u32 e = n;
		while (e > k && (s[e - 1] == ' ' || s[e - 1] == '\r'))
			e--;
		if (e > k && hash15(s + k, e - k) == ((state >> 5) & 0x7FFFu))
			return SH_NORMAL; /* the delimiter line */
		emit(o, 0, n, TK_STRING);
		return state;
	}
	if (mode == SH_SQ || mode == SH_DSQ) {
		bool closed;
		i = sh_sq(&c, 0, mode == SH_DSQ, &closed);
		if (!closed)
			return mode;
	} else if (mode == SH_DQ) {
		bool closed;
		i = sh_dq(&c, 0, 0, &closed);
		if (!closed)
			return mode;
	}
	sh_seg(&c, i, 0, 0);
	if (c.hd)
		return c.hd;
	return c.open;
}

/* ================================================================== */
/* Perl                                                                */
/*
 * Classes follow VSCodium's perl.tmLanguage under Dark+: keyword.control
 * (if, return, use, ...), storage.modifier (my our local), support.function
 * (print, push, eq, ...), string.regexp for m// s/// tr/// qr// and /.../,
 * variable.other.* for sigiled names, POD as comment. Numbers have no scope
 * in that grammar; they are coloured as numbers here anyway.
 *
 * State (20 bits):
 *   bits 0-2  mode: NORMAL, POD, STR (an open quote-like), HEREDOC, END
 *             (after __END__/__DATA__), ENDPOD (POD after __END__)
 *   STR:      bits 3-4 kind (string, regex, first part of s/tr, gap before
 *             the second part of a bracketed s{}{}), bit 5 interpolating,
 *             bits 6-12 closing delimiter, bits 13-17 bracket depth
 *   HEREDOC:  bit 3 interpolating, bit 4 `<<~`, bits 5-19 hash15 of the
 *             terminator */
enum { PL_NORMAL, PL_POD, PL_STR, PL_HEREDOC, PL_END, PL_ENDPOD };
enum { PK_STR, PK_REGEX, PK_SUBST1, PK_GAP };
enum { PLK_CTRL = 1, PLK_STORAGE, PLK_FUNC, PLK_OP, PLK_CONST, PLK_SUB, PLK_PACKAGE, PLK_BLOCK };

static const char pl_ctrl[] =
	"continue default die do else elsif exit for foreach given goto if last next redo return select "
	"unless until wait when while switch case require use eval no";
static const char pl_storage[] = "my our local state";
static const char pl_func[] =
	"ARGV DATA ENV SIG STDERR STDIN STDOUT atan2 bind binmode bless caller chdir chmod chomp chop chown "
	"chr chroot close closedir cmp connect cos crypt dbmclose dbmopen defined delete dump each endgrent "
	"endhostent endnetent endprotoent endpwent endservent eof eq exec exists exp fcntl fileno flock "
	"fork formline ge getc getgrent getgrgid getgrnam gethostbyaddr gethostbyname gethostent getlogin "
	"getnetbyaddr getnetbyname getnetent getpeername getpgrp getppid getpriority getprotobyname "
	"getprotobynumber getprotoent getpwent getpwnam getpwuid getservbyname getservbyport getservent "
	"getsockname getsockopt glob gmtime grep gt hex import index int ioctl join keys kill lc lcfirst le "
	"length link listen localtime log lstat lt map mkdir msgctl msgget msgrcv msgsnd ne oct open "
	"opendir ord pack pipe pop pos print printf push quotemeta rand read readdir readlink recv ref "
	"rename reset reverse rewinddir rindex rmdir say scalar seek seekdir semctl semget semop send "
	"setgrent sethostent setnetent setpgrp setpriority setprotoent setpwent setservent setsockopt shift "
	"shmctl shmget shmread shmwrite shutdown sin sleep socket socketpair sort splice split sprintf sqrt "
	"srand stat study substr symlink syscall sysopen sysread system syswrite tell telldir tie tied time "
	"times truncate uc ucfirst umask undef unlink unpack unshift untie utime values vec waitpid "
	"wantarray warn write";
static const char pl_op[] = "and or xor as not";
static const char pl_const[] = "__FILE__ __LINE__ __PACKAGE__ __SUB__";
static const char pl_block[] = "BEGIN END INIT CHECK UNITCHECK AUTOLOAD DESTROY";

#define PL_STR_STATE(kind, interp, close, depth) \
	(PL_STR | ((u32)(kind) << 3) | ((interp) ? 0x20u : 0u) | (((u32)(close) & 0x7Fu) << 6) | (((u32)(depth) & 31u) << 13))

typedef struct {
	const char *s;
	u32 n;
	SynSpans *o;
	u32 hd; /* pending heredoc state */
} Pl;

static u8 pl_open_of(u8 close)
{
	switch (close) {
	case ')': return '(';
	case ']': return '[';
	case '}': return '{';
	case '>': return '<';
	default: return 0;
	}
}

static u8 pl_close_of(u8 open)
{
	switch (open) {
	case '(': return ')';
	case '[': return ']';
	case '{': return '}';
	case '<': return '>';
	default: return open;
	}
}

/* Variable starting at a sigil inside interpolated text; returns end or i. */
static u32 pl_interp_var(Pl *c, u32 i, u32 end, bool regex)
{
	const char *s = c->s;
	u8 sig = (u8)s[i];
	u32 j = i + 1;
	if (j >= end)
		return i;
	u8 ch = (u8)s[j];
	if (sig == '$' && regex && (ch == ')' || ch == '|'))
		return i; /* anchor */
	if (ch == '{') {
		u32 k = j + 1;
		while (k < end && s[k] != '}')
			k++;
		if (k < end) {
			emit(c->o, i, k + 1 - i, TK_VARIABLE);
			return k + 1;
		}
		return i;
	}
	if (is_alpha(ch) || ch == '_' || (ch == ':' && j + 1 < end && s[j + 1] == ':')) {
		while (j < end && (is_word((u8)s[j]) || (s[j] == ':' && j + 1 < end && s[j + 1] == ':' && ++j)))
			j++;
		emit(c->o, i, j - i, TK_VARIABLE);
		return j;
	}
	if (sig == '$' && (is_digit(ch) || ch == '&' || (!regex && in_set("_!@/\\,;.0", ch)))) {
		emit(c->o, i, 2, TK_VARIABLE);
		return i + 2;
	}
	return i;
}

/* Body of a quoted construct from i; returns position after it and the
 * state to carry when the line ends inside it (0 when closed). */
static u32 pl_quoted(Pl *c, u32 i, u32 kind, bool interp, u8 close, u32 depth, u32 *carry)
{
	const char *s = c->s;
	u32 n = c->n;
	u8 open = pl_open_of(close);
	u32 cls = kind == PK_STR ? TK_STRING : TK_REGEX;
	bool regex = kind != PK_STR;
	*carry = 0;
	for (;;) {
		while (i < n) {
			u8 ch = (u8)s[i];
			if (ch == '\\' && i + 1 < n) {
				bool esc = interp || (u8)s[i + 1] == close || s[i + 1] == '\\';
				emit(c->o, i, 2, esc ? TK_ESCAPE : cls);
				i += 2;
				continue;
			}
			if (open && ch == open) {
				depth++;
			} else if (ch == close) {
				if (depth == 0)
					break;
				depth--;
			} else if (interp && (ch == '$' || ch == '@') && close != ch) {
				u32 e = pl_interp_var(c, i, n, regex);
				if (e > i) {
					i = e;
					continue;
				}
			}
			emit(c->o, i, 1, cls);
			i++;
		}
		if (i >= n) {
			*carry = PL_STR_STATE(kind, interp, close, depth);
			return n;
		}
		emit(c->o, i, 1, cls); /* closing delimiter */
		i++;
		if (kind == PK_SUBST1) {
			if (open) {
				/* s{...}{...}: a new delimiter follows */
				i = skip_ws(s, n, i);
				if (i >= n) {
					*carry = PL_STR_STATE(PK_GAP, interp, 0, 0);
					return n;
				}
				u8 o2 = (u8)s[i];
				emit(c->o, i, 1, cls);
				close = pl_close_of(o2);
				open = pl_open_of(close);
				i++;
			}
			kind = PK_REGEX;
			depth = 0;
			continue;
		}
		break;
	}
	if (kind == PK_REGEX) {
		u32 j = i;
		while (j < n && is_alpha((u8)s[j]))
			j++;
		emit(c->o, i, j - i, TK_CONTROL); /* keyword.control.regexp-option */
		i = j;
	}
	return i;
}

/* Heredoc after "<<" at i; returns the end of the marker. */
static u32 pl_heredoc_op(Pl *c, u32 i)
{
	const char *s = c->s;
	u32 n = c->n, k = i + 2;
	bool indent = false, interp = true;
	if (k < n && s[k] == '~') {
		indent = true;
		k++;
	}
	u32 a, b;
	if (k < n && (s[k] == '"' || s[k] == '\'')) {
		u8 q = (u8)s[k];
		interp = q == '"';
		a = k + 1;
		b = a;
		while (b < n && (u8)s[b] != q)
			b++;
		k = b < n ? b + 1 : n;
	} else {
		a = k;
		while (k < n && is_word((u8)s[k]))
			k++;
		b = k;
	}
	emit(c->o, i, k - i, TK_STRING);
	if (b > a && !c->hd)
		c->hd = PL_HEREDOC | (interp ? 0x08u : 0u) | (indent ? 0x10u : 0u) | (hash15(s + a, b - a) << 5);
	return k;
}

static bool pl_quote_op(const char *w, u32 wl, u32 *kind, bool *interp, bool *two)
{
	*two = false;
	if (word_is(w, wl, "q") || word_is(w, wl, "qw")) {
		*kind = PK_STR;
		*interp = false;
	} else if (word_is(w, wl, "qq") || word_is(w, wl, "qx")) {
		*kind = PK_STR;
		*interp = true;
	} else if (word_is(w, wl, "m") || word_is(w, wl, "qr")) {
		*kind = PK_REGEX;
		*interp = true;
	} else if (word_is(w, wl, "s")) {
		*kind = PK_SUBST1;
		*interp = true;
		*two = true;
	} else if (word_is(w, wl, "tr") || word_is(w, wl, "y")) {
		*kind = PK_SUBST1;
		*interp = false;
		*two = true;
	} else {
		return false;
	}
	return true;
}

static u32 pl_number(const char *s, u32 n, u32 i)
{
	u32 j = i;
	if (s[j] == '0' && j + 1 < n && ((s[j + 1] | 32) == 'x' || (s[j + 1] | 32) == 'b')) {
		j += 2;
		while (j < n && (is_xdigit((u8)s[j]) || s[j] == '_'))
			j++;
		return j;
	}
	while (j < n && (is_digit((u8)s[j]) || s[j] == '_'))
		j++;
	if (j + 1 < n && s[j] == '.' && is_digit((u8)s[j + 1])) {
		j++;
		while (j < n && (is_digit((u8)s[j]) || s[j] == '_'))
			j++;
	}
	if (j < n && (s[j] | 32) == 'e') {
		u32 k = j + 1;
		if (k < n && (s[k] == '+' || s[k] == '-'))
			k++;
		if (k < n && is_digit((u8)s[k])) {
			j = k;
			while (j < n && is_digit((u8)s[j]))
				j++;
		}
	}
	return j;
}

static bool pl_end_marker(const char *s, u32 n)
{
	u32 l = n >= 8 && memcmp(s, "__DATA__", 8) == 0 ? 8 : n >= 7 && memcmp(s, "__END__", 7) == 0 ? 7 : 0;
	return l && skip_ws(s, n, l) == n;
}

static u32 lex_perl(u32 state, const char *s, u32 n, SynSpans *o)
{
	Pl c = {s, n, o, 0};
	u32 mode = state & 7u;
	u32 i = 0;
	bool pod_line = n > 1 && s[0] == '=' && is_alpha((u8)s[1]);
	if (mode == PL_POD || mode == PL_ENDPOD) {
		emit(o, 0, n, TK_COMMENT);
		if (n >= 4 && memcmp(s, "=cut", 4) == 0 && (n == 4 || !is_word((u8)s[4])))
			return mode == PL_POD ? PL_NORMAL : PL_END;
		return mode;
	}
	if (mode == PL_END) {
		if (pod_line) {
			emit(o, 0, n, TK_COMMENT);
			return PL_ENDPOD;
		}
		return PL_END;
	}
	if (mode == PL_HEREDOC) {
		u32 k = 0;
		if (state & 0x10u)
			while (k < n && (s[k] == ' ' || s[k] == '\t'))
				k++;
		u32 e = n;
		while (e > k && s[e - 1] == '\r')
			e--;
		if (e > k && hash15(s + k, e - k) == ((state >> 5) & 0x7FFFu)) {
			emit(o, k, e - k, TK_STRING);
			return PL_NORMAL;
		}
		if (state & 0x08u) {
			u32 j = 0;
			while (j < n) {
				if ((s[j] == '$' || s[j] == '@') && !(j && s[j - 1] == '\\')) {
					u32 e2 = pl_interp_var(&c, j, n, false);
					if (e2 > j) {
						j = e2;
						continue;
					}
				}
				if (s[j] == '\\' && j + 1 < n) {
					emit(o, j, 2, TK_ESCAPE);
					j += 2;
					continue;
				}
				emit(o, j, 1, TK_STRING);
				j++;
			}
		} else {
			emit(o, 0, n, TK_STRING);
		}
		return state;
	}
	if (mode == PL_STR) {
		u32 kind = (state >> 3) & 3u;
		bool interp = state & 0x20u;
		u8 close = (u8)((state >> 6) & 0x7Fu);
		u32 depth = (state >> 13) & 31u;
		u32 carry;
		if (kind == PK_GAP) {
			i = skip_ws(s, n, 0);
			if (i >= n)
				return state;
			u8 o2 = (u8)s[i];
			emit(o, i, 1, TK_REGEX);
			i = pl_quoted(&c, i + 1, PK_REGEX, interp, pl_close_of(o2), 0, &carry);
		} else {
			i = pl_quoted(&c, 0, kind, interp, close, depth, &carry);
		}
		if (carry)
			return carry;
	}
	if (pod_line && i == 0) {
		emit(o, 0, n, TK_COMMENT);
		if (n >= 4 && memcmp(s, "=cut", 4) == 0)
			return PL_NORMAL;
		return PL_POD;
	}
	if (i == 0 && pl_end_marker(s, n)) {
		emit(o, 0, n, TK_LANGCONST);
		return PL_END;
	}
	bool term = true;
	int next_kind = 0; /* 1: sub name, 2: package name */
	u32 carry = 0;
	while (i < n) {
		u8 ch = (u8)s[i];
		if (is_space(ch)) {
			i++;
			continue;
		}
		if (ch == '#') {
			emit(o, i, n - i, TK_COMMENT);
			break;
		}
		if (ch == '$' || ch == '@' || (ch == '%' && term)) {
			u32 j = i + 1;
			if (ch == '$' && j < n && s[j] == '#') {
				/* $#array, $#{expr}, $#$ref */
				if (j + 1 < n && (is_alpha((u8)s[j + 1]) || s[j + 1] == '_' || s[j + 1] == '{' || s[j + 1] == '$'))
					j++;
			}
			if (j < n && s[j] == '{') {
				u32 k = j + 1;
				while (k < n && (is_word((u8)s[k]) || s[k] == ':' || s[k] == '^'))
					k++;
				if (k < n && s[k] == '}' && k > j + 1) {
					emit(o, i, k + 1 - i, TK_VARIABLE);
					i = k + 1;
				} else {
					emit(o, i, j - i, TK_VARIABLE);
					i = j;
				}
				term = false;
				continue;
			}
			if (j < n && (is_alpha((u8)s[j]) || s[j] == '_' || (s[j] == ':' && j + 1 < n && s[j + 1] == ':'))) {
				while (j < n && (is_word((u8)s[j]) || (s[j] == ':' && j + 1 < n && s[j + 1] == ':')))
					j += s[j] == ':' ? 2 : 1;
				emit(o, i, j - i, TK_VARIABLE);
				i = j;
				term = false;
				continue;
			}
			if (ch == '$' && j < n && s[j] == '^' && j + 1 < n && is_alpha((u8)s[j + 1])) {
				emit(o, i, 3, TK_VARIABLE);
				i += 3;
				term = false;
				continue;
			}
			if (ch == '$' && j < n && (is_digit((u8)s[j]) || in_set("&`'+!@/\\,;.<>()[]^:?|$\"-~=%*0_", (u8)s[j]))) {
				if (s[j] == '$' && j + 1 < n && (is_alpha((u8)s[j + 1]) || s[j + 1] == '_' || s[j + 1] == '{')) {
					emit(o, i, 1, TK_VARIABLE); /* $$ref deref */
					i++;
					continue;
				}
				u32 e = j + 1;
				if (is_digit((u8)s[j]))
					while (e < n && is_digit((u8)s[e]))
						e++;
				emit(o, i, e - i, TK_VARIABLE);
				i = e;
				term = false;
				continue;
			}
			if (ch == '@' && j < n && (s[j] == '$' || s[j] == '_')) {
				emit(o, i, 1, TK_VARIABLE);
				i++;
				continue;
			}
			i++;
			term = true;
			continue;
		}
		if (ch == '"' || ch == '`' || ch == '\'') {
			emit(o, i, 1, TK_STRING);
			i = pl_quoted(&c, i + 1, PK_STR, ch != '\'', ch, 0, &carry);
			if (carry)
				break;
			term = false;
			continue;
		}
		if (ch == '/' && term) {
			emit(o, i, 1, TK_REGEX);
			i = pl_quoted(&c, i + 1, PK_REGEX, true, '/', 0, &carry);
			if (carry)
				break;
			term = false;
			continue;
		}
		if (ch == '<' && i + 2 < n && s[i + 1] == '<' &&
		    (s[i + 2] == '"' || s[i + 2] == '\'' || s[i + 2] == '~' ||
		     (term && (is_alpha((u8)s[i + 2]) || s[i + 2] == '_')))) {
			i = pl_heredoc_op(&c, i);
			term = false;
			continue;
		}
		if (is_digit(ch) || (ch == '.' && term && i + 1 < n && is_digit((u8)s[i + 1]))) {
			u32 j = pl_number(s, n, ch == '.' ? i + 1 : i);
			emit(o, i, j - i, TK_NUMBER);
			i = j;
			term = false;
			continue;
		}
		if (ch == '-' && term && i + 1 < n && in_set("rwxoRWXOezsfdlpSbctugkTBMAC", (u8)s[i + 1]) &&
		    (i + 2 >= n || !is_word((u8)s[i + 2]))) {
			emit(o, i, 2, TK_OPERATOR); /* file test */
			i += 2;
			continue;
		}
		if (is_alpha(ch) || ch == '_') {
			u32 j = i;
			while (j < n && (is_word((u8)s[j]) || (s[j] == ':' && j + 1 < n && s[j + 1] == ':')))
				j += s[j] == ':' ? 2 : 1;
			const char *w = s + i;
			u32 wl = j - i;
			u32 p = skip_ws(s, n, j);
			bool fat = p + 1 < n && s[p] == '=' && s[p + 1] == '>';
			bool arrow = i >= 2 && s[i - 1] == '>' && s[i - 2] == '-';
			bool brace_key = i > 0 && s[i - 1] == '{' && j < n && s[j] == '}';
			if (next_kind) {
				emit(o, i, wl, next_kind == 1 ? TK_FUNCTION : TK_TYPE);
				next_kind = 0;
				i = j;
				term = false;
				continue;
			}
			if (fat || arrow || brace_key) {
				i = j;
				term = false;
				continue;
			}
			u32 qk;
			bool qi, two;
			if (pl_quote_op(w, wl, &qk, &qi, &two) && p < n) {
				u8 d = (u8)s[p];
				bool ok = !is_word(d) && d != ',' && d != ';' && d != ')' && !(d == '=' && p + 1 < n && s[p + 1] == '>') &&
				          !(p > j && (d == '#' || d == '='));
				if (ok) {
					emit(o, i, p + 1 - i, qk == PK_STR ? TK_STRING : TK_REGEX);
					(void)two;
					i = pl_quoted(&c, p + 1, qk, qi && d != '\'', pl_close_of(d), 0, &carry);
					if (carry)
						break;
					term = false;
					continue;
				}
			}
			int k = kw_find(&kw_pl, w, wl);
			switch (k) {
			case PLK_CTRL:
				emit(o, i, wl, TK_CONTROL);
				term = true;
				break;
			case PLK_STORAGE:
				emit(o, i, wl, TK_STORAGE);
				term = true;
				break;
			case PLK_FUNC:
				emit(o, i, wl, TK_FUNCTION);
				term = true;
				break;
			case PLK_OP:
				emit(o, i, wl, TK_OPERATOR);
				term = true;
				break;
			case PLK_CONST:
				emit(o, i, wl, TK_LANGCONST);
				term = false;
				break;
			case PLK_SUB:
				emit(o, i, wl, TK_STORAGE);
				next_kind = 1;
				term = true;
				break;
			case PLK_PACKAGE:
				emit(o, i, wl, TK_STORAGE);
				next_kind = 2;
				term = true;
				break;
			case PLK_BLOCK:
				emit(o, i, wl, p < n && s[p] == '{' ? TK_CONTROL : TK_TEXT);
				term = true;
				break;
			default:
				term = false;
				break;
			}
			i = j;
			continue;
		}
		/* operators and punctuation */
		if (ch == '=' && i + 1 < n && (s[i + 1] == '>' || s[i + 1] == '~')) {
			emit(o, i, 2, TK_OPERATOR);
			i += 2;
			term = true;
			continue;
		}
		if (ch == '-' && i + 1 < n && s[i + 1] == '>') {
			emit(o, i, 2, TK_OPERATOR);
			i += 2;
			term = false;
			continue;
		}
		if (ch == '<' && i + 2 < n && s[i + 1] == '=' && s[i + 2] == '>') {
			emit(o, i, 3, TK_OPERATOR);
			i += 3;
			term = true;
			continue;
		}
		term = !(ch == ')' || ch == ']' || ch == '}');
		i++;
	}
	if (carry)
		return carry;
	if (c.hd)
		return c.hd;
	return PL_NORMAL;
}

/* ================================================================== */
/* troff (man, mdoc, ms, me, mm)                                        */
/*
 * VSCodium has no built-in roff grammar; this follows the scopes of the
 * common "roff" TextMate grammar: requests and macros at line start are
 * keywords (keyword.control -> TK_CONTROL), section headings (.SH/.Sh)
 * markup.heading, .B/.I arguments markup.bold/italic, font and size escapes
 * (\fB \s+2 \(em \[name] \e) constant.character.escape, strings and
 * registers (\*x \n(xx \$1) variables, comments (.\" \" \#) comments,
 * quoted macro arguments strings, numbers numbers, and mdoc callable
 * macros inside a macro line (Fl Ar Op ...) storage keywords.
 *
 * State: 0 normal, 1 inside .ig ... .. (shown as a comment). */

static const char mdoc_callable[] =
	"Ad An Ap Ar At Bc Bo Bq Brc Bro Brq Bsx Bx Cd Cm Dc Do Dq Dv Dx Ec Em Eo Er Es Ev Fa Fc Fl Fn Fo Fr "
	"Ic Li Lk Ms Mt Nm Nm Nx Oc Oo Op Ot Ox Pa Pc Pf Po Pq Qc Ql Qo Qq Sc So Sq Sx Sy Ta Tn Ux Va Vt Xc "
	"Xo Xr No Ns Ns";

/* One escape at i (s[i] == '\\'); returns its end, or 0 for a comment. */
static u32 roff_escape(const char *s, u32 n, u32 i, SynSpans *o)
{
	if (i + 1 >= n) {
		emit(o, i, 1, TK_ESCAPE);
		return n;
	}
	u8 c = (u8)s[i + 1];
	u32 j = i + 2;
	u32 cls = TK_ESCAPE;
	if (c == '"' || c == '#') {
		emit(o, i, n - i, TK_COMMENT);
		return 0;
	}
	/* argument form: x, (xx, [name] */
	bool arg = in_set("fFsnm*gkYVOMmz$", c);
	if (c == '*' || c == 'n' || c == '$' || c == 'g' || c == 'V')
		cls = TK_VARIABLE;
	if (c == '(') {
		j = VX_MIN(i + 4, n);
	} else if (c == '[') {
		while (j < n && s[j] != ']')
			j++;
		if (j < n)
			j++;
	} else if (arg) {
		if (c == 's' && j < n && (s[j] == '+' || s[j] == '-'))
			j++;
		if (c == 'n' && j < n && (s[j] == '+' || s[j] == '-'))
			j++;
		if (j < n && s[j] == '(')
			j = VX_MIN(j + 3, n);
		else if (j < n && s[j] == '[') {
			while (j < n && s[j] != ']')
				j++;
			if (j < n)
				j++;
		} else if (c == 's') {
			while (j < n && is_digit((u8)s[j]) && j < i + 5)
				j++;
		} else if (j < n) {
			j++;
		}
	} else if (in_set("hvlLbDoRwXZAB", c) && j < n && (s[j] == '\'' || s[j] == '"')) {
		u8 q = (u8)s[j];
		j++;
		while (j < n && (u8)s[j] != q)
			j++;
		if (j < n)
			j++;
	}
	emit(o, i, j - i, cls);
	return j;
}

/* Text with escapes in [i, n) using base class cls. Returns n. */
static void roff_text(const char *s, u32 n, u32 i, u32 cls, SynSpans *o)
{
	while (i < n) {
		if (s[i] == '\\') {
			u32 e = roff_escape(s, n, i, o);
			if (!e)
				return;
			i = e;
			continue;
		}
		u32 j = i;
		while (j < n && s[j] != '\\')
			j++;
		emit(o, i, j - i, cls);
		i = j;
	}
}

static u32 lex_troff(u32 state, const char *s, u32 n, SynSpans *o)
{
	if (state == 1) {
		if (n >= 2 && s[0] == '.' && s[1] == '.' && skip_ws(s, n, 2) == n) {
			emit(o, 0, n, TK_CONTROL);
			return 0;
		}
		emit(o, 0, n, TK_COMMENT);
		return 1;
	}
	if (n == 0)
		return 0;
	if (s[0] != '.' && s[0] != '\'') {
		roff_text(s, n, 0, TK_TEXT, o);
		return 0;
	}
	u32 i = skip_ws(s, n, 1);
	if (i < n && s[i] == '\\' && i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '#')) {
		emit(o, 0, n, TK_COMMENT);
		return 0;
	}
	u32 j = i;
	while (j < n && !is_space((u8)s[j]) && s[j] != '\\')
		j++;
	emit(o, 0, j, TK_CONTROL);
	const char *m = s + i;
	u32 ml = j - i;
	if (word_is(m, ml, "ig")) {
		roff_text(s, n, j, TK_TEXT, o);
		return 1;
	}
	u32 arg = TK_TEXT;
	if (word_is(m, ml, "SH") || word_is(m, ml, "SS") || word_is(m, ml, "Sh") || word_is(m, ml, "Ss") ||
	    word_is(m, ml, "NH") || word_is(m, ml, "TL"))
		arg = TK_HEADING;
	else if (word_is(m, ml, "B") || word_is(m, ml, "SB") || word_is(m, ml, "Sy"))
		arg = TK_BOLD;
	else if (word_is(m, ml, "I") || word_is(m, ml, "Em"))
		arg = TK_ITALIC;
	bool mdoc = ml >= 2 && ml <= 3 && s[i] >= 'A' && s[i] <= 'Z' && s[i + 1] >= 'a' && s[i + 1] <= 'z';
	i = j;
	while (i < n) {
		u8 ch = (u8)s[i];
		if (is_space(ch)) {
			i++;
			continue;
		}
		if (ch == '"') {
			u32 k = i + 1;
			while (k < n && s[k] != '"') {
				if (s[k] == '\\' && k + 1 < n && s[k + 1] == '"')
					break;
				k++;
			}
			if (k < n && s[k] == '"')
				k++;
			emit(o, i, 1, arg == TK_TEXT ? TK_STRING : arg);
			roff_text(s, k, i + 1, arg == TK_TEXT ? TK_STRING : arg, o);
			i = k;
			continue;
		}
		u32 k = i;
		while (k < n && !is_space((u8)s[k]) && s[k] != '"') {
			if (s[k] == '\\') {
				if (k + 1 < n && (s[k + 1] == '"' || s[k + 1] == '#'))
					break;
				k = VX_MIN(k + 2, n);
				continue;
			}
			k++;
		}
		const char *w = s + i;
		u32 wl = k - i;
		if (mdoc && wl >= 2 && wl <= 3 && kw_find(&kw_mdoc, w, wl) >= 0)
			emit(o, i, wl, TK_STORAGE);
		else if (wl && (is_digit((u8)w[0]) || ((w[0] == '-' || w[0] == '+') && wl > 1 && is_digit((u8)w[1])))) {
			u32 q = w[0] == '-' || w[0] == '+' ? 1 : 0;
			while (q < wl && (is_digit((u8)w[q]) || w[q] == '.'))
				q++;
			if (q < wl && in_set("ciPpmnvuMf", (u8)w[q]) && q + 1 == wl)
				q++;
			emit(o, i, q, q == wl ? TK_NUMBER : TK_TEXT);
			if (q < wl)
				roff_text(s, k, i + q, arg, o);
		} else {
			roff_text(s, k, i, arg, o);
		}
		if (k < n && s[k] == '\\') { /* comment */
			emit(o, k, n - k, TK_COMMENT);
			break;
		}
		i = k;
	}
	return 0;
}

/* ================================================================== */
/* Shared helpers for the build-file / data / git lexers               */

static u32 lex_any(int lang, u32 state, const char *s, u32 n, SynSpans *o);

/* Run another lexer on s[off, n) and emit its spans shifted by off. */
static u32 lex_shifted(int lang, u32 state, const char *s, u32 n, u32 off, SynSpans *o)
{
	if (off > n)
		off = n;
	if (!o)
		return lex_any(lang, state, s + off, n - off, NULL);
	u32 st[512], ln[512];
	u8 cl[512];
	SynSpans t = {st, ln, cl, 0, 512};
	u32 r = lex_any(lang, state, s + off, n - off, &t);
	for (u32 k = 0; k < t.count; k++)
		emit(o, t.start[k] + off, t.len[k], t.cls[k]);
	return r;
}

/* Does the line end with an odd number of backslashes (continuation)? */
static bool line_continues(const char *s, u32 n)
{
	while (n && (s[n - 1] == '\r'))
		n--;
	u32 k = 0;
	while (k < n && s[n - 1 - k] == '\\')
		k++;
	return k & 1u;
}

static KwSet kw_cmake, kw_mkfn, kw_mkdir, kw_bsddir;

/* ================================================================== */
/* Ignore files (.gitignore, .dockerignore, .gitattributes ...)        */
/*
 * VSCodium's ignore.tmLanguage only scopes comments. vxide also marks the
 * negation "!" and the trailing "/" (operators), escapes, and glob
 * metacharacters (* ? ** [...], regexp colour). In .gitattributes-style
 * lines, words after the pattern are attributes ("-text", "eol=lf"). */
static u32 lex_ignore(u32 state, const char *s, u32 n, SynSpans *o)
{
	(void)state;
	u32 i = skip_ws(s, n, 0);
	if (i < n && s[i] == '#') {
		emit(o, i, n - i, TK_COMMENT);
		return 0;
	}
	if (i < n && s[i] == '!') {
		emit(o, i, 1, TK_OPERATOR);
		i++;
	}
	/* end of the pattern: first unescaped blank */
	u32 e = i;
	while (e < n && s[e] != ' ' && s[e] != '\t' && s[e] != '\r') {
		if (s[e] == '\\' && e + 1 < n)
			e++;
		e++;
	}
	for (u32 k = i; k < e; k++) {
		u8 c = (u8)s[k];
		if (c == '\\' && k + 1 < e) {
			emit(o, k, 2, TK_ESCAPE);
			k++;
		} else if (c == '*' || c == '?') {
			emit(o, k, 1, TK_REGEX);
		} else if (c == '[') {
			u32 j = k + 1;
			while (j < e && s[j] != ']')
				j++;
			if (j < e) {
				emit(o, k, j - k + 1, TK_REGEX);
				k = j;
			}
		} else if (c == '/' && (k + 1 == e || k == i)) {
			emit(o, k, 1, TK_OPERATOR); /* directory marker / anchor */
		}
	}
	/* attributes (.gitattributes) */
	u32 k = e;
	while (k < n) {
		k = skip_ws(s, n, k);
		if (k >= n)
			break;
		if (s[k] == '#') {
			emit(o, k, n - k, TK_COMMENT);
			break;
		}
		u32 a = k;
		if (s[k] == '-' || s[k] == '!') {
			emit(o, k, 1, TK_OPERATOR);
			a = ++k;
		}
		while (k < n && s[k] != '=' && s[k] != ' ' && s[k] != '\t')
			k++;
		emit(o, a, k - a, TK_PROPERTY);
		if (k < n && s[k] == '=') {
			emit(o, k, 1, TK_OPERATOR);
			u32 v = ++k;
			while (k < n && s[k] != ' ' && s[k] != '\t')
				k++;
			emit(o, v, k - v, TK_STRING);
		}
	}
	return 0;
}

/* ================================================================== */
/* JSON / JSONC                                                        */
/*
 * Per VSCodium's JSON.tmLanguage under Dark+: object keys are
 * support.type.property-name.json (#9CDCFE / #0451A5), string values
 * string.quoted.double (#CE9178), escapes constant.character.escape,
 * bad escapes invalid.illegal, numbers, true/false/null constant.language.
 * A string is a key when the next non-blank character on its line is ':'.
 * Comments are coloured in both JSON and JSONC (the grammar has them).
 *
 * State: 0 normal, 1 inside a block comment. */
static u32 lex_json(u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 i = 0;
	if (state == 1) {
		while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
			i++;
		if (i + 1 >= n) {
			emit(o, 0, n, TK_COMMENT);
			return 1;
		}
		i += 2;
		emit(o, 0, i, TK_COMMENT);
	}
	while (i < n) {
		u8 c = (u8)s[i];
		if (is_space(c)) {
			i++;
			continue;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '/') {
			emit(o, i, n - i, TK_COMMENT);
			return 0;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '*') {
			u32 j = i + 2;
			while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/'))
				j++;
			if (j + 1 >= n) {
				emit(o, i, n - i, TK_COMMENT);
				return 1;
			}
			emit(o, i, j + 2 - i, TK_COMMENT);
			i = j + 2;
			continue;
		}
		if (c == '"') {
			/* find the end first to decide key vs value */
			u32 j = i + 1;
			while (j < n && s[j] != '"') {
				if (s[j] == '\\' && j + 1 < n)
					j++;
				j++;
			}
			u32 end = j < n ? j + 1 : n;
			u32 nx = skip_ws(s, n, end);
			u32 cls = (nx < n && s[nx] == ':') ? TK_PROPERTY : TK_STRING;
			u32 run = i;
			for (u32 k = i + 1; k < end; k++) {
				if (s[k] != '\\' || k + 1 >= end)
					continue;
				u8 e = (u8)s[k + 1];
				u32 el = 2;
				bool ok = in_set("\"\\/bfnrt", e);
				if (e == 'u') {
					ok = k + 5 < end + 1 && k + 5 <= n;
					for (u32 q = 2; ok && q < 6; q++)
						ok = k + q < n && is_xdigit((u8)s[k + q]);
					el = ok ? 6 : 2;
				}
				emit(o, run, k - run, cls);
				emit(o, k, el, ok ? TK_ESCAPE : TK_INVALID);
				k += el - 1;
				run = k + 1;
			}
			emit(o, run, end - run, cls);
			i = end;
			continue;
		}
		if (c == '-' || is_digit(c)) {
			u32 j = i + (c == '-');
			while (j < n && is_digit((u8)s[j]))
				j++;
			if (j < n && s[j] == '.') {
				j++;
				while (j < n && is_digit((u8)s[j]))
					j++;
			}
			if (j < n && (s[j] == 'e' || s[j] == 'E')) {
				j++;
				if (j < n && (s[j] == '+' || s[j] == '-'))
					j++;
				while (j < n && is_digit((u8)s[j]))
					j++;
			}
			emit(o, i, j - i, TK_NUMBER);
			i = j;
			continue;
		}
		if (is_alpha(c)) {
			u32 j = i;
			while (j < n && is_word((u8)s[j]))
				j++;
			if (word_is(s + i, j - i, "true") || word_is(s + i, j - i, "false") || word_is(s + i, j - i, "null"))
				emit(o, i, j - i, TK_LANGCONST);
			i = j;
			continue;
		}
		i++;
	}
	return 0;
}

/* ================================================================== */
/* CMake                                                               */
/*
 * VSCodium has no CMake grammar; scopes follow the twxs.cmake extension:
 * flow control commands are keyword.control, other commands (known or
 * user-defined) entity.name.function, upper-case arguments (PUBLIC,
 * REQUIRED, CMAKE_C_STANDARD ...) constants, ON/OFF/TRUE/FALSE/YES/NO
 * constant.language, ${VAR} $ENV{VAR} $CACHE{VAR} variables (nested),
 * $<GENEX:...> preprocessor-like, quoted strings with escapes, bracket
 * arguments [=[...]=] strings and #[=[...]=] bracket comments.
 *
 * State: bits 0-1 mode (0 normal, 1 bracket comment, 2 bracket argument,
 * 3 quoted string), bits 2-5 '=' count of the open bracket, bits 6-9
 * parenthesis depth (an identifier followed by '(' at depth 0 is a
 * command). */
enum { CMK_NORMAL, CMK_BCOMMENT, CMK_BARG, CMK_STRING };
#define CMK_STATE(mode, eq, depth) ((u32)(mode) | ((u32)(eq) << 2) | ((u32)(depth) << 6))

static const char cmake_control[] =
    "if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro "
    "return break continue block endblock";
static const char cmake_const[] = "on off true false yes no y n ignore notfound";

/* Bracket "[" "="* "[" at i: returns '=' count, or -1. */
static int cmake_bracket_open(const char *s, u32 n, u32 i)
{
	if (i >= n || s[i] != '[')
		return -1;
	u32 j = i + 1;
	while (j < n && s[j] == '=')
		j++;
	if (j < n && s[j] == '[' && j - i - 1 < 16)
		return (int)(j - i - 1);
	return -1;
}

/* Find "]" "="*eq "]" from i; returns the index after it or UINT32_MAX. */
static u32 cmake_bracket_close(const char *s, u32 n, u32 i, u32 eq)
{
	for (u32 k = i; k + eq + 1 < n; k++) {
		if (s[k] != ']')
			continue;
		u32 q = 0;
		while (q < eq && s[k + 1 + q] == '=')
			q++;
		if (q == eq && s[k + 1 + eq] == ']')
			return k + eq + 2;
	}
	return UINT32_MAX;
}

/* ${...} $ENV{...} $CACHE{...} (nested) or $<...> at s[i] == '$'. */
static u32 cmake_ref(const char *s, u32 n, u32 i, SynSpans *o, int depth)
{
	u32 j = i + 1;
	if (j < n && s[j] == '<') {
		/* generator expression: $<NAME:args> */
		u32 a = j + 1;
		while (a < n && (is_word((u8)s[a]) || s[a] == '-'))
			a++;
		emit(o, i, a - i, TK_PREPROC);
		u32 k = a;
		while (k < n && s[k] != '>') {
			if (s[k] == '$' && depth < 8) {
				k = cmake_ref(s, n, k, o, depth + 1);
				continue;
			}
			k++;
		}
		if (k < n) {
			emit(o, k, 1, TK_PREPROC);
			k++;
		}
		return k;
	}
	if (j + 3 < n && memcmp(s + j, "ENV{", 4) == 0)
		j += 3;
	else if (j + 5 < n && memcmp(s + j, "CACHE{", 6) == 0)
		j += 5;
	if (j >= n || s[j] != '{') {
		return i + 1; /* lone $ */
	}
	u32 k = j + 1, run = i;
	while (k < n && s[k] != '}') {
		if (s[k] == '$' && depth < 8 && k + 1 < n && (s[k + 1] == '{' || s[k + 1] == '<' || s[k + 1] == 'E' || s[k + 1] == 'C')) {
			emit(o, run, k - run, TK_VARIABLE);
			k = cmake_ref(s, n, k, o, depth + 1);
			run = k;
			continue;
		}
		k++;
	}
	if (k < n)
		k++;
	emit(o, run, k - run, TK_VARIABLE);
	return k;
}

/* Quoted argument body from i (after the opening quote); returns the index
 * after the closing quote, or n with *closed = false. */
static u32 cmake_string(const char *s, u32 n, u32 i, SynSpans *o, bool *closed)
{
	u32 run = i;
	*closed = false;
	while (i < n) {
		if (s[i] == '"') {
			emit(o, run, i + 1 - run, TK_STRING);
			*closed = true;
			return i + 1;
		}
		if (s[i] == '\\' && i + 1 < n) {
			emit(o, run, i - run, TK_STRING);
			emit(o, i, 2, TK_ESCAPE);
			i += 2;
			run = i;
			continue;
		}
		if (s[i] == '$' && i + 1 < n && (s[i + 1] == '{' || s[i + 1] == '<' || s[i + 1] == 'E' || s[i + 1] == 'C')) {
			emit(o, run, i - run, TK_STRING);
			u32 k = cmake_ref(s, n, i, o, 0);
			if (k == i + 1)
				emit(o, i, 1, TK_STRING);
			i = k;
			run = i;
			continue;
		}
		i++;
	}
	emit(o, run, n - run, TK_STRING);
	return n;
}

static u32 lex_cmake(u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 mode = state & 3u, eq = (state >> 2) & 15u, depth = (state >> 6) & 15u;
	u32 i = 0;
	if (mode == CMK_BCOMMENT || mode == CMK_BARG) {
		u32 e = cmake_bracket_close(s, n, 0, eq);
		u32 cls = mode == CMK_BCOMMENT ? TK_COMMENT : TK_STRING;
		if (e == UINT32_MAX) {
			emit(o, 0, n, cls);
			return state;
		}
		emit(o, 0, e, cls);
		i = e;
	} else if (mode == CMK_STRING) {
		bool closed;
		i = cmake_string(s, n, 0, o, &closed);
		if (!closed)
			return state;
	}
	while (i < n) {
		u8 c = (u8)s[i];
		if (is_space(c)) {
			i++;
			continue;
		}
		if (c == '#') {
			int b = cmake_bracket_open(s, n, i + 1);
			if (b >= 0) {
				u32 e = cmake_bracket_close(s, n, i + (u32)b + 3, (u32)b);
				if (e == UINT32_MAX) {
					emit(o, i, n - i, TK_COMMENT);
					return CMK_STATE(CMK_BCOMMENT, b, depth);
				}
				emit(o, i, e - i, TK_COMMENT);
				i = e;
				continue;
			}
			emit(o, i, n - i, TK_COMMENT);
			break;
		}
		if (c == '[') {
			int b = cmake_bracket_open(s, n, i);
			if (b >= 0) {
				u32 e = cmake_bracket_close(s, n, i + (u32)b + 2, (u32)b);
				if (e == UINT32_MAX) {
					emit(o, i, n - i, TK_STRING);
					return CMK_STATE(CMK_BARG, b, depth);
				}
				emit(o, i, e - i, TK_STRING);
				i = e;
				continue;
			}
		}
		if (c == '"') {
			bool closed;
			emit(o, i, 1, TK_STRING);
			i = cmake_string(s, n, i + 1, o, &closed);
			if (!closed)
				return CMK_STATE(CMK_STRING, 0, depth);
			continue;
		}
		if (c == '$') {
			u32 k = cmake_ref(s, n, i, o, 0);
			i = k > i ? k : i + 1;
			continue;
		}
		if (c == '(') {
			if (depth < 15)
				depth++;
			i++;
			continue;
		}
		if (c == ')') {
			if (depth)
				depth--;
			i++;
			continue;
		}
		/* unquoted argument or command name */
		u32 j = i;
		while (j < n && !is_space((u8)s[j]) && !in_set("()\"#$", (u8)s[j])) {
			if (s[j] == '\\' && j + 1 < n)
				j++;
			j++;
		}
		if (j == i) {
			i++;
			continue;
		}
		u32 wl = j - i;
		bool ident = is_alpha(c) || c == '_';
		for (u32 k = i; k < j && ident; k++)
			ident = is_word((u8)s[k]);
		if (depth == 0 && ident) {
			u32 nx = skip_ws(s, n, j);
			if (nx < n && s[nx] == '(') {
				emit(o, i, wl, kw_find_ci(&kw_cmake, s + i, wl) == TK_CONTROL ? TK_CONTROL : TK_FUNCTION);
				i = j;
				continue;
			}
		}
		if (kw_find_ci(&kw_cmake, s + i, wl) == TK_LANGCONST) {
			emit(o, i, wl, TK_LANGCONST);
		} else if (ident && wl >= 2) {
			bool upper = true, alpha = false;
			for (u32 k = i; k < j; k++) {
				u8 q = (u8)s[k];
				if (q >= 'a' && q <= 'z')
					upper = false;
				if (is_alpha(q))
					alpha = true;
			}
			if (upper && alpha)
				emit(o, i, wl, TK_CONSTANT);
		} else {
			bool num = is_digit(c);
			for (u32 k = i; k < j && num; k++)
				num = is_digit((u8)s[k]) || s[k] == '.';
			if (num)
				emit(o, i, wl, TK_NUMBER);
			else
				for (u32 k = i; k + 1 < j; k++)
					if (s[k] == '\\') {
						emit(o, k, 2, TK_ESCAPE);
						k++;
					}
		}
		i = j;
	}
	return CMK_STATE(CMK_NORMAL, 0, depth);
}

/* ================================================================== */
/* Make (BSD make and GNU make)                                        */
/*
 * GNU parts follow VSCodium's make.tmLanguage under Dark+: targets are
 * entity.name.function.target, assignments variable.other, $(VAR) and
 * ${VAR} variable.other, automatic and built-in variables ($@ $< $(MAKE))
 * variable.language, $(patsubst ...) and friends support.function, and
 * directives keyword.control. Recipe lines (tab first) are highlighted
 * with the shell lexer, with make references painted over it; the
 * @/-/+ prefixes are keyword.control as in the grammar.
 *
 * BSD make: .if/.for/.include/... are keyword.control, condition
 * functions (defined make empty exists target commands) functions,
 * ${VAR:M*.c:S/a/b/} modifiers functions, local variables (${.TARGET},
 * ${.ALLSRC} ...) variable.language.
 *
 * State: bits 0-2 mode (0 normal, 1 continued value line, 2 recipe
 * continuation, 3 define body, 4 continued comment); bits 3-19 the shell
 * lexer's state across continued recipe lines (17 bits: a heredoc cannot
 * span recipe lines). */
enum { MK_NORMAL, MK_CONT, MK_RECIPE, MK_DEFINE, MK_COMMENT };
enum { MKF_FUNC = 1, MKF_BUILTIN };

static const char mk_functions[] =
    "subst patsubst strip findstring filter filter-out sort word wordlist words firstword lastword dir notdir "
    "suffix basename addsuffix addprefix join wildcard realpath abspath error warning shell origin flavor "
    "foreach if or and call eval file value info let intcmp guile";
static const char mk_builtins[] =
    "MAKEFILES VPATH SHELL MAKESHELL MAKE MAKELEVEL MAKEFLAGS MFLAGS MAKECMDGOALS CURDIR SUFFIXES "
    ".LIBPATTERNS .DEFAULT_GOAL .RECIPEPREFIX .VARIABLES .FEATURES .INCLUDE_DIRS .SHELLSTATUS "
    ".TARGET .ALLSRC .IMPSRC .OODATE .PREFIX .MEMBER .ARCHIVE .CURDIR .OBJDIR .MAKE .MAKEFLAGS "
    ".MAKEOVERRIDES .PARSEDIR .PARSEFILE .INCLUDEDFROMDIR .INCLUDEDFROMFILE .MAKE.LEVEL .MAKE.MODE "
    ".MAKE.OS .MAKE.PID .MAKE.PPID .MAKE.JOBS .MAKE.MAKEFILES .MAKE.META.FILES .SHELL .PATH .ERROR_TARGET "
    "MACHINE MACHINE_ARCH MAKEOBJDIR MAKEOBJDIRPREFIX MAKESYSPATH";
static const char mk_gnu_dirs[] =
    "include -include sinclude ifeq ifneq ifdef ifndef else endif define endef export unexport override "
    "undefine vpath private";
static const char mk_bsd_dirs[] =
    "if ifdef ifndef ifmake ifnmake elif elifdef elifndef elifmake elifnmake else endif for endfor include "
    "sinclude dinclude -include undef export export-env export-literal unexport unexport-env info warning "
    "error break poison";

static u32 mk_ref(const char *s, u32 n, u32 i, SynSpans *o, int depth);

/* Text with make references and comments; stops at a '#' comment unless
 * no_comment. Returns the end. */
static u32 mk_text(const char *s, u32 n, u32 i, u32 end, SynSpans *o, bool comments, u32 cls)
{
	(void)n;
	u32 run = i;
	while (i < end) {
		u8 c = (u8)s[i];
		if (c == '\\' && i + 1 < end) {
			i += 2;
			continue;
		}
		if (c == '$') {
			emit(o, run, i - run, cls);
			i = mk_ref(s, end, i, o, 0);
			run = i;
			continue;
		}
		if (c == '#' && comments) {
			emit(o, run, i - run, cls);
			emit(o, i, end - i, TK_COMMENT);
			return end;
		}
		i++;
	}
	emit(o, run, end - run, cls);
	return end;
}

/* $x, $(...), ${...} at s[i] == '$'. */
static u32 mk_ref(const char *s, u32 n, u32 i, SynSpans *o, int depth)
{
	if (i + 1 >= n)
		return i + 1;
	u8 c = (u8)s[i + 1];
	if (c != '(' && c != '{') {
		emit(o, i, 2, TK_LANGCONST); /* $@ $< $^ $$ $1 ... */
		return i + 2;
	}
	u8 close = c == '(' ? ')' : '}';
	u32 a = i + 2, k = a;
	while (k < n && !is_space((u8)s[k]) && s[k] != ':' && s[k] != close && s[k] != '$' && s[k] != '=' &&
	       s[k] != '(' && s[k] != '{')
		k++;
	u32 nl = k - a;
	int kind = kw_find(&kw_mkfn, s + a, nl);
	bool fn = kind == MKF_FUNC && k < n && is_space((u8)s[k]);
	u32 cls = fn ? TK_FUNCTION : kind == MKF_BUILTIN ? TK_LANGCONST : TK_VARIABLE;
	if (!fn && nl == 1 && in_set("@<^?*%+|", (u8)s[a]))
		cls = TK_LANGCONST; /* ${@} ${<} */
	emit(o, i, k - i, cls);
	/* nested name / arguments / modifiers up to the matching close */
	u32 d = 0;
	u32 run = k;
	bool in_mod = false;
	while (k < n) {
		u8 q = (u8)s[k];
		if (q == '$' && depth < 8) {
			emit(o, run, k - run, (fn || in_mod) ? TK_TEXT : cls);
			k = mk_ref(s, n, k, o, depth + 1);
			run = k;
			continue;
		}
		if (q == '(' || q == '{')
			d++;
		else if (q == close || q == (close == ')' ? '}' : ')')) {
			if (d == 0 && q == close) {
				emit(o, run, k - run, (fn || in_mod) ? TK_TEXT : cls);
				emit(o, k, 1, cls);
				return k + 1;
			}
			if (d)
				d--;
		} else if (q == ':' && !fn && d == 0) {
			/* modifier: BSD :M :S/a/b/ :tl ...; GNU $(X:.c=.o) */
			emit(o, run, k - run, in_mod ? TK_TEXT : cls);
			u32 m = k + 1;
			while (m < n && is_alpha((u8)s[m]))
				m++;
			if (m == k + 1 && m < n && in_set("!=?@%_[", (u8)s[m]))
				m++;
			emit(o, k, m - k, m > k + 1 ? TK_FUNCTION : TK_OPERATOR);
			in_mod = true;
			k = m;
			run = k;
			continue;
		}
		k++;
	}
	emit(o, run, n - run, (fn || in_mod) ? TK_TEXT : cls);
	return n;
}

/* Make references in a recipe painted over the shell lexer's spans. */
static u32 mk_recipe(u32 shst, const char *s, u32 n, u32 off, SynSpans *o)
{
	enum { MAXL = 2048 };
	if (!o || n - off > MAXL) {
		u32 r = lex_any(LANG_SH, shst, s + off, n - off, NULL);
		mk_text(s, n, off, n, o, false, TK_TEXT);
		return r;
	}
	u8 cls[MAXL];
	u32 len = n - off;
	memset(cls, TK_TEXT, len);
	u32 st[512], ln[512];
	u8 cl[512];
	SynSpans t = {st, ln, cl, 0, 512};
	u32 r = lex_any(LANG_SH, shst, s + off, len, &t);
	for (u32 k = 0; k < t.count; k++)
		memset(cls + t.start[k], t.cls[k], VX_MIN(t.len[k], len - t.start[k]));
	t.count = 0;
	for (u32 i = off; i < n;) {
		if (s[i] == '$') {
			u32 e = mk_ref(s, n, i, &t, 0);
			memset(cls + (i - off), TK_TEXT, e - i);
			i = e;
		} else {
			i++;
		}
	}
	for (u32 k = 0; k < t.count; k++)
		memset(cls + (t.start[k] - off), t.cls[k], VX_MIN(t.len[k], n - t.start[k]));
	u32 a = 0;
	for (u32 k = 1; k <= len; k++)
		if (k == len || cls[k] != cls[a]) {
			emit(o, off + a, k - a, cls[a]);
			a = k;
		}
	return r;
}

static u32 mk_recipe_state(u32 shst, bool cont)
{
	if (!cont)
		return MK_NORMAL;
	if (shst >= (1u << 17))
		shst = 0;
	return MK_RECIPE | (shst << 3);
}

/* Assignment at i? Sets the name end and the operator span. */
static bool mk_assign(const char *s, u32 n, u32 i, u32 *name_end, u32 *op, u32 *opl)
{
	u32 k = i;
	int d = 0;
	while (k < n) {
		u8 c = (u8)s[k];
		if (c == '$' && k + 1 < n && (s[k + 1] == '(' || s[k + 1] == '{')) {
			d++;
			k += 2;
			continue;
		}
		if (d && (c == ')' || c == '}')) {
			d--;
			k++;
			continue;
		}
		if (!d && (is_space(c) || c == '=' || c == ':' || c == '#' || c == ';'))
			break;
		if (!d && (c == '?' || c == '+' || c == '!') && k + 1 < n && s[k + 1] == '=')
			break;
		k++;
	}
	if (k == i)
		return false;
	*name_end = k;
	u32 p = skip_ws(s, n, k);
	static const char *const ops[] = {":::=", "::=", ":=", "?=", "+=", "!=", "="};
	for (size_t q = 0; q < VX_COUNT(ops); q++) {
		u32 l = (u32)strlen(ops[q]);
		if (p + l <= n && memcmp(s + p, ops[q], l) == 0) {
			*op = p;
			*opl = l;
			return true;
		}
	}
	return false;
}

/* BSD conditional expression / directive arguments. */
static void mk_cond(const char *s, u32 n, u32 i, SynSpans *o)
{
	while (i < n) {
		u8 c = (u8)s[i];
		if (c == '#') {
			emit(o, i, n - i, TK_COMMENT);
			return;
		}
		if (c == '$') {
			i = mk_ref(s, n, i, o, 0);
			continue;
		}
		if (c == '"' || c == '<') {
			u8 q = c == '<' ? '>' : '"';
			u32 j = i + 1, run = i;
			while (j < n && s[j] != q) {
				if (s[j] == '$') {
					emit(o, run, j - run, TK_STRING);
					j = mk_ref(s, n, j, o, 0);
					run = j;
					continue;
				}
				j++;
			}
			if (j < n)
				j++;
			emit(o, run, j - run, TK_STRING);
			i = j;
			continue;
		}
		if (in_set("=!<>&|", c)) {
			u32 j = i + 1;
			if (j < n && in_set("=&|", (u8)s[j]))
				j++;
			emit(o, i, j - i, TK_OPERATOR);
			i = j;
			continue;
		}
		if (is_digit(c)) {
			u32 j = i;
			while (j < n && (is_xdigit((u8)s[j]) || s[j] == 'x' || s[j] == '.'))
				j++;
			emit(o, i, j - i, TK_NUMBER);
			i = j;
			continue;
		}
		if (is_alpha(c)) {
			u32 j = i;
			while (j < n && is_word((u8)s[j]))
				j++;
			u32 nx = skip_ws(s, n, j);
			if (nx < n && s[nx] == '(' &&
			    (word_is(s + i, j - i, "defined") || word_is(s + i, j - i, "make") || word_is(s + i, j - i, "empty") ||
			     word_is(s + i, j - i, "exists") || word_is(s + i, j - i, "target") || word_is(s + i, j - i, "commands")))
				emit(o, i, j - i, TK_FUNCTION);
			i = j;
			continue;
		}
		i++;
	}
}

static u32 mk_end_state(const char *s, u32 n, u32 mode_if_cont)
{
	return line_continues(s, n) ? mode_if_cont : MK_NORMAL;
}

static u32 lex_make(u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 mode = state & 7u;
	if (mode == MK_COMMENT) {
		emit(o, 0, n, TK_COMMENT);
		return mk_end_state(s, n, MK_COMMENT);
	}
	if (mode == MK_RECIPE) {
		u32 r = mk_recipe(state >> 3, s, n, 0, o);
		return mk_recipe_state(r, line_continues(s, n));
	}
	if (mode == MK_CONT) {
		mk_text(s, n, 0, n, o, true, TK_TEXT);
		return mk_end_state(s, n, MK_CONT);
	}
	if (mode == MK_DEFINE) {
		u32 i = skip_ws(s, n, 0), j = i;
		while (j < n && is_word((u8)s[j]))
			j++;
		if (word_is(s + i, j - i, "endef")) {
			emit(o, i, 5, TK_CONTROL);
			mk_text(s, n, j, n, o, true, TK_TEXT);
			return MK_NORMAL;
		}
		mk_text(s, n, 0, n, o, false, TK_TEXT);
		return MK_DEFINE;
	}
	if (n && s[0] == '\t') {
		/* recipe: @ - + prefixes, then shell */
		u32 i = 1;
		while (i < n && (s[i] == '@' || s[i] == '-' || s[i] == '+' || s[i] == ' ' || s[i] == '\t')) {
			if (s[i] != ' ' && s[i] != '\t')
				emit(o, i, 1, TK_CONTROL);
			i++;
		}
		u32 r = mk_recipe(0, s, n, i, o);
		return mk_recipe_state(r, line_continues(s, n));
	}
	u32 i = skip_ws(s, n, 0);
	if (i >= n)
		return MK_NORMAL;
	if (s[i] == '#') {
		emit(o, i, n - i, TK_COMMENT);
		return mk_end_state(s, n, MK_COMMENT);
	}
	/* BSD directive: '.' blanks word */
	if (s[i] == '.') {
		u32 a = skip_ws(s, n, i + 1), b = a;
		while (b < n && (is_word((u8)s[b]) || s[b] == '-'))
			b++;
		if (b > a && kw_find(&kw_bsddir, s + a, b - a) >= 0) {
			emit(o, i, 1, TK_CONTROL);
			emit(o, a, b - a, TK_CONTROL);
			if (word_is(s + a, b - a, "for")) {
				/* .for x y in list */
				u32 k = b;
				for (;;) {
					k = skip_ws(s, n, k);
					u32 w = k;
					while (k < n && !is_space((u8)s[k]))
						k++;
					if (k == w)
						break;
					if (word_is(s + w, k - w, "in")) {
						emit(o, w, 2, TK_CONTROL);
						mk_text(s, n, k, n, o, true, TK_TEXT);
						break;
					}
					emit(o, w, k - w, TK_VARIABLE);
				}
			} else {
				mk_cond(s, n, b, o);
			}
			return mk_end_state(s, n, MK_CONT);
		}
	}
	/* GNU directive */
	{
		u32 b = i;
		while (b < n && (is_word((u8)s[b]) || s[b] == '-'))
			b++;
		if (b > i && (b == n || is_space((u8)s[b]) || s[b] == '(') && kw_find(&kw_mkdir, s + i, b - i) >= 0) {
			u32 na, op, opl;
			bool assign_after = (word_is(s + i, b - i, "export") || word_is(s + i, b - i, "override") ||
			                     word_is(s + i, b - i, "private") || word_is(s + i, b - i, "unexport")) &&
			                    mk_assign(s, n, skip_ws(s, n, b), &na, &op, &opl);
			emit(o, i, b - i, TK_CONTROL);
			if (word_is(s + i, b - i, "define")) {
				u32 a = skip_ws(s, n, b), e = a;
				while (e < n && !is_space((u8)s[e]))
					e++;
				emit(o, a, e - a, TK_VARIABLE);
				mk_text(s, n, e, n, o, true, TK_TEXT);
				return MK_DEFINE;
			}
			if (assign_after) {
				u32 a = skip_ws(s, n, b);
				mk_text(s, na, a, na, o, false, TK_VARIABLE);
				emit(o, op, opl, TK_OPERATOR);
				mk_text(s, n, op + opl, n, o, true, TK_TEXT);
			} else if (word_is(s + i, b - i, "else")) {
				/* else ifeq (...) */
				u32 a = skip_ws(s, n, b), e = a;
				while (e < n && is_word((u8)s[e]))
					e++;
				if (e > a && kw_find(&kw_mkdir, s + a, e - a) >= 0)
					emit(o, a, e - a, TK_CONTROL);
				mk_text(s, n, e, n, o, true, TK_TEXT);
			} else {
				mk_text(s, n, b, n, o, true, TK_TEXT);
			}
			return mk_end_state(s, n, MK_CONT);
		}
	}
	/* assignment */
	u32 na, op, opl;
	if (mk_assign(s, n, i, &na, &op, &opl)) {
		mk_text(s, na, i, na, o, false, TK_VARIABLE);
		emit(o, op, opl, TK_OPERATOR);
		mk_text(s, n, op + opl, n, o, true, TK_TEXT);
		return mk_end_state(s, n, MK_CONT);
	}
	/* rule: targets ':' / '::' / '!' prerequisites [; recipe] */
	u32 k = i;
	int d = 0;
	while (k < n) {
		u8 c = (u8)s[k];
		if (c == '$' && k + 1 < n && (s[k + 1] == '(' || s[k + 1] == '{')) {
			d++;
			k += 2;
			continue;
		}
		if (d && (c == ')' || c == '}'))
			d--;
		else if (!d && (c == ':' || c == '#' || (c == '!' && k > i && is_space((u8)s[k - 1]))))
			break;
		k++;
	}
	if (k < n && (s[k] == ':' || s[k] == '!')) {
		/* target names */
		u32 t = i;
		while (t < k) {
			t = skip_ws(s, k, t);
			u32 e = t;
			int dd = 0;
			while (e < k && (dd || !is_space((u8)s[e]))) {
				if (s[e] == '$' && e + 1 < k && (s[e + 1] == '(' || s[e + 1] == '{'))
					dd++;
				else if (dd && (s[e] == ')' || s[e] == '}'))
					dd--;
				e++;
			}
			if (e > t) {
				u32 run = t;
				for (u32 q = t; q < e;) {
					if (s[q] == '$') {
						emit(o, run, q - run, TK_FUNCTION);
						q = mk_ref(s, e, q, o, 0);
						run = q;
					} else if (s[q] == '%') {
						emit(o, run, q - run, TK_FUNCTION);
						emit(o, q, 1, TK_VARIABLE);
						run = ++q;
					} else {
						q++;
					}
				}
				emit(o, run, e - run, TK_FUNCTION);
			}
			t = e;
		}
		u32 p = k + 1;
		if (p < n && s[p] == ':')
			p++;
		/* prerequisites, then an optional ;recipe */
		u32 semi = p;
		int d2 = 0;
		while (semi < n && !(d2 == 0 && (s[semi] == ';' || s[semi] == '#'))) {
			if (s[semi] == '$' && semi + 1 < n && (s[semi + 1] == '(' || s[semi + 1] == '{'))
				d2++;
			else if (d2 && (s[semi] == ')' || s[semi] == '}'))
				d2--;
			semi++;
		}
		for (u32 q = p; q < semi;) {
			if (s[q] == '$') {
				q = mk_ref(s, semi, q, o, 0);
			} else {
				if (s[q] == '%' || s[q] == '*')
					emit(o, q, 1, TK_VARIABLE);
				q++;
			}
		}
		if (semi < n && s[semi] == '#') {
			emit(o, semi, n - semi, TK_COMMENT);
			return mk_end_state(s, n, MK_COMMENT);
		}
		if (semi < n && s[semi] == ';') {
			u32 r = mk_recipe(0, s, n, semi + 1, o);
			return mk_recipe_state(r, line_continues(s, n));
		}
		return mk_end_state(s, n, MK_CONT);
	}
	mk_text(s, n, i, n, o, true, TK_TEXT);
	return mk_end_state(s, n, MK_CONT);
}

/* ================================================================== */
/* Diff / patch (and `git show` output)                                */
/*
 * Per VSCodium's diff.tmLanguage under Dark+: meta.diff.header lines
 * (diff --git, --- a/x, +++ b/x) #569CD6 / #000080, markup.inserted
 * (+, >) #B5CEA8 / #098658, markup.deleted (-, <) #CE9178 / #A31515,
 * markup.changed (!) #569CD6 / #0451A5, '#' comments. Range lines (@@)
 * and index lines have no colour in VSCodium and none here. For `git
 * show`: "commit <sha>" and the Author:/Date:/Merge: keys use the header
 * colour, the sha the number colour. Stateless. */
static u32 lex_diff(u32 state, const char *s, u32 n, SynSpans *o)
{
	(void)state;
	if (n == 0)
		return 0;
	if ((n >= 5 && memcmp(s, "diff ", 5) == 0) || (n >= 4 && (memcmp(s, "--- ", 4) == 0 ||
	                                                          memcmp(s, "+++ ", 4) == 0 || memcmp(s, "*** ", 4) == 0))) {
		/* "--- x" and "*** x" are headers; "*** x ****" is a context range */
		if (s[0] == '*' && n >= 5 && memcmp(s + n - 4, "****", 4) == 0)
			return 0;
		if (s[0] == '-' && n >= 5 && memcmp(s + n - 4, "----", 4) == 0)
			return 0;
		emit(o, 0, n, TK_DIFFHDR);
		return 0;
	}
	if (n >= 7 && memcmp(s, "commit ", 7) == 0) {
		emit(o, 0, 6, TK_DIFFHDR);
		u32 a = 7, b = a;
		while (b < n && is_xdigit((u8)s[b]))
			b++;
		emit(o, a, b - a, TK_NUMBER);
		return 0;
	}
	/* `git format-patch` mail: "From <sha> Mon Sep 17 ..." like "commit <sha>" */
	if (n >= 46 && memcmp(s, "From ", 5) == 0) {
		u32 b = 5;
		while (b < n && is_xdigit((u8)s[b]))
			b++;
		if (b == 45) {
			emit(o, 0, 4, TK_DIFFHDR);
			emit(o, 5, 40, TK_NUMBER);
			return 0;
		}
	}
	static const char *const keys[] = {"Author:", "AuthorDate:", "Commit:", "CommitDate:", "Date:", "Merge:",
	                                   "From:", "Subject:"};
	for (size_t k = 0; k < VX_COUNT(keys); k++) {
		u32 l = (u32)strlen(keys[k]);
		if (n >= l && memcmp(s, keys[k], l) == 0) {
			emit(o, 0, l, TK_DIFFHDR);
			if (k == 5) /* Merge: sha sha */
				for (u32 q = l; q < n;) {
					q = skip_ws(s, n, q);
					u32 e = q;
					while (e < n && is_xdigit((u8)s[e]))
						e++;
					emit(o, q, e - q, TK_NUMBER);
					q = e > q ? e : q + 1;
				}
			return 0;
		}
	}
	switch (s[0]) {
	case '+':
	case '>':
		emit(o, 0, n, TK_INSERTED);
		break;
	case '-':
	case '<':
		if (!(n == 3 && memcmp(s, "---", 3) == 0))
			emit(o, 0, n, TK_DELETED);
		break;
	case '!':
		emit(o, 0, n, TK_CHANGED);
		break;
	case '#':
		emit(o, 0, n, TK_COMMENT);
		break;
	default:
		break;
	}
	return 0;
}

/* ================================================================== */
/* Git commit message                                                  */
/*
 * Per git-commit.tmLanguage: the first line of each message block (after
 * the start or a comment block) is the subject; characters past 50 are
 * invalid.deprecated and past 72 invalid.illegal (both #F44747 under
 * Dark+). '#' lines are comments, with "#\tmodified:" changed,
 * "#\tnew file:" inserted, "#\tdeleted:" deleted, other "#\tkind: file"
 * keyword + string. From "diff --git" to the end: embedded diff.
 *
 * State: bit 0 inside the trailing diff, bit 1 subject already seen. */
static u32 lex_git_commit(u32 state, const char *s, u32 n, SynSpans *o)
{
	if ((state & 1u) || (n >= 10 && memcmp(s, "diff --git", 10) == 0)) {
		lex_diff(0, s, n, o);
		return 1u;
	}
	if (n && s[0] == '#') {
		if (n > 2 && s[1] == '\t') {
			const char *r = s + 2;
			u32 rn = n - 2;
			u32 cls = 0;
			if ((rn >= 9 && memcmp(r, "modified:", 9) == 0) || (rn >= 8 && memcmp(r, "renamed:", 8) == 0))
				cls = TK_CHANGED;
			else if (rn >= 9 && memcmp(r, "new file:", 9) == 0)
				cls = TK_INSERTED;
			else if (rn >= 7 && memcmp(r, "deleted", 7) == 0)
				cls = TK_DELETED;
			if (cls) {
				emit(o, 0, 2, TK_COMMENT);
				emit(o, 2, n - 2, cls);
				return 0;
			}
			const char *colon = memchr(r, ':', rn);
			if (colon) {
				u32 c = (u32)(colon - s);
				emit(o, 0, 2, TK_COMMENT);
				emit(o, 2, c - 2, TK_STORAGE);
				emit(o, c, 1, TK_COMMENT);
				u32 f = skip_ws(s, n, c + 1);
				emit(o, c + 1, f - c - 1, TK_COMMENT);
				emit(o, f, n - f, TK_STRING);
				return 0;
			}
		}
		emit(o, 0, n, TK_COMMENT);
		return 0; /* the next message line is a subject again */
	}
	if (!(state & 2u)) {
		/* subject: count characters, not bytes */
		u32 chars = 0, i = 0, at50 = n;
		while (i < n) {
			if (chars == 50)
				at50 = i;
			u32 cp;
			i += utf8_decode(s + i, n - i, &cp);
			chars++;
		}
		if (at50 < n)
			emit(o, at50, n - at50, TK_INVALID); /* 51-72 deprecated, >72 illegal: both "invalid" */
		return 2u;
	}
	return 2u;
}

/* ================================================================== */
/* git-rebase-todo                                                     */
/*
 * Per git-rebase.tmLanguage: commands support.function.git-rebase
 * (#9CDCFE / #0451A5, the property class), shas constant.sha.git-rebase
 * (#B5CEA8 / #098658), exec lines embed the shell, '#' comments. vxide
 * also knows label/reset/merge/update-ref and fixup -C/-c. Stateless. */
static u32 lex_git_rebase(u32 state, const char *s, u32 n, SynSpans *o)
{
	(void)state;
	u32 i = skip_ws(s, n, 0);
	if (i >= n)
		return 0;
	if (s[i] == '#') {
		emit(o, 0, n, TK_COMMENT);
		return 0;
	}
	u32 e = i;
	while (e < n && (is_alpha((u8)s[e]) || s[e] == '-'))
		e++;
	static const char *const sha_cmds[] = {"pick", "p", "reword", "r", "edit", "e", "squash", "s", "fixup", "f", "drop", "d"};
	static const char *const other[] = {"break", "b", "label", "l", "reset", "t", "merge", "m", "update-ref", "u"};
	bool sha = false, known = false;
	for (size_t k = 0; k < VX_COUNT(sha_cmds); k++)
		if (word_is(s + i, e - i, sha_cmds[k]))
			sha = known = true;
	for (size_t k = 0; k < VX_COUNT(other) && !known; k++)
		if (word_is(s + i, e - i, other[k]))
			known = true;
	if (word_is(s + i, e - i, "exec") || word_is(s + i, e - i, "x")) {
		emit(o, i, e - i, TK_PROPERTY);
		lex_shifted(LANG_SH, 0, s, n, skip_ws(s, n, e), o);
		return 0;
	}
	if (!known)
		return 0;
	emit(o, i, e - i, TK_PROPERTY);
	u32 k = skip_ws(s, n, e);
	/* options like fixup -C / merge -C <sha> */
	while (k + 1 < n && s[k] == '-' && is_alpha((u8)s[k + 1])) {
		emit(o, k, 2, TK_PROPERTY);
		k = skip_ws(s, n, k + 2);
		sha = true;
	}
	if (sha) {
		u32 h = k;
		while (h < n && is_xdigit((u8)s[h]))
			h++;
		if (h > k && (h == n || is_space((u8)s[h])))
			emit(o, k, h - k, TK_NUMBER);
	}
	return 0;
}

/* ================================================================== */
/* Markdown                                                            */
/*
 * Block structure per line, inline spans after it, following VSCodium's
 * markdown.tmLanguage scopes under Dark+/Light+. Fenced code with a known
 * info string is highlighted by that language's lexer (fenced code with an
 * unknown language has no colour, as in VSCodium).
 *
 * State (32 bits):
 *   bits 0-1   mode: 0 normal, 1 fenced code, 2 inside <!-- -->
 *   bits 2-6   embedded LANG_* of the fence (0 = none)
 *   bit 7      fence uses '~'
 *   bits 8-11  fence length (3..15; longer fences are clamped to 15)
 *   bits 12-31 embedded lexer state (20 bits) in fenced code;
 *              in normal mode bit 12 = the previous line was paragraph
 *              text (an ===/--- line under it is a setext heading). */
enum { MD_NORMAL, MD_FENCE, MD_HTMLCMT };
#define MD_PARA (1u << 12)

static u32 lex_any(int lang, u32 state, const char *s, u32 n, SynSpans *o);

/* ================================================================== */
/* Python                                                              */
/*
 * Per VSCodium's MagicPython.tmLanguage under Dark+/Light+:
 *   keyword.control.flow / .import       -> TK_CONTROL  (#C586C0)
 *   storage.type.function/class, storage.modifier.declaration (def class
 *   lambda async global nonlocal)        -> TK_STORAGE  (#569CD6)
 *   keyword.operator.logical.python (and or not in is) -> TK_STORAGE
 *   entity.name.function (def name), decorators, support.function.builtin
 *   and .magic                           -> TK_FUNCTION (#DCDCAA)
 *   entity.name.type.class, entity.other.inherited-class, support.type
 *   (builtin types, exceptions)          -> TK_TYPE     (#4EC9B0)
 *   variable.parameter (def params, call keywords), support.variable.magic
 *                                        -> TK_VARIABLE (#9CDCFE)
 *   variable.language.special.self (in bodies), constant.language
 *                                        -> TK_LANGCONST (#569CD6)
 *   storage.type.string (r b u f prefixes), the {} of f-strings and
 *   format placeholders (constant.character.format.placeholder)
 *                                        -> TK_STORAGE
 *   raw strings with a lowercase r are regexps (string.regexp) -> TK_REGEX
 * Generic calls (meta.function-call.generic) have no colour in Dark+.
 *
 * State: bits 0-2 string mode (0 none, 1 ''', 2 """, 3 ' continued,
 * 4 " continued); bits 3-6 string flags (raw, f, bytes, regex);
 * bit 7 inside a def parameter list, bit 8 inside class bases,
 * bit 9 expecting a parameter name; bits 10-13 bracket depth there. */

enum { PYS_NONE, PYS_TRI1, PYS_TRI2, PYS_CONT1, PYS_CONT2 };
enum { PYF_RAW = 8, PYF_F = 16, PYF_BYTES = 32, PYF_REGEX = 64 };
enum { PY_DEFP = 1u << 7, PY_CLASSB = 1u << 8, PY_EXPECT = 1u << 9 };
enum { PYK_CTRL = 1, PYK_STORAGE, PYK_LOGIC, PYK_CONST, PYK_FUNC, PYK_TYPE, PYK_MAGICVAR, PYK_DEF, PYK_CLASS, PYK_ASYNC, PYK_SOFT };

static KwSet kw_py;

static const char py_ctrl[] =
    "if elif else for while break continue return yield try except finally raise with as assert pass del "
    "await import from";
static const char py_storage[] = "lambda global nonlocal";
static const char py_logic[] = "and or not in is";
static const char py_const[] = "None True False Ellipsis NotImplemented __debug__";
static const char py_func[] =
    "abs aiter all anext any ascii bin breakpoint callable chr compile copyright credits delattr dir divmod "
    "enumerate eval exec exit filter format getattr globals hasattr hash help hex id input isinstance "
    "issubclass iter len license locals map max memoryview min next oct open ord pow print quit range repr "
    "reversed round setattr sorted sum vars zip __import__";
static const char py_type[] =
    "bool bytearray bytes classmethod complex dict float frozenset int list object property set slice "
    "staticmethod str super tuple type "
    "BaseException BaseExceptionGroup Exception ExceptionGroup ArithmeticError AssertionError AttributeError "
    "BlockingIOError BrokenPipeError BufferError ChildProcessError ConnectionAbortedError ConnectionError "
    "ConnectionRefusedError ConnectionResetError EOFError EnvironmentError FileExistsError FileNotFoundError "
    "FloatingPointError GeneratorExit IOError ImportError IndentationError IndexError InterruptedError "
    "IsADirectoryError KeyError KeyboardInterrupt LookupError MemoryError ModuleNotFoundError NameError "
    "NotADirectoryError NotImplementedError OSError OverflowError PermissionError ProcessLookupError "
    "RecursionError ReferenceError RuntimeError StopAsyncIteration StopIteration SyntaxError SystemError "
    "SystemExit TabError TimeoutError TypeError UnboundLocalError UnicodeDecodeError UnicodeEncodeError "
    "UnicodeError UnicodeTranslateError ValueError ZeroDivisionError Warning BytesWarning DeprecationWarning "
    "EncodingWarning FutureWarning ImportWarning PendingDeprecationWarning ResourceWarning RuntimeWarning "
    "SyntaxWarning UnicodeWarning UserWarning";
static const char py_magicvar[] =
    "__all__ __annotations__ __builtins__ __class__ __closure__ __code__ __debug__ __defaults__ __dict__ "
    "__doc__ __file__ __func__ __globals__ __kwdefaults__ __loader__ __module__ __mro__ __name__ "
    "__package__ __path__ __qualname__ __self__ __slots__ __spec__ __version__ __weakref__ __match_args__";

static void py_build(void)
{
	kw_list(&kw_py, py_ctrl, PYK_CTRL);
	kw_list(&kw_py, py_storage, PYK_STORAGE);
	kw_list(&kw_py, py_logic, PYK_LOGIC);
	kw_list(&kw_py, py_const, PYK_CONST);
	kw_list(&kw_py, py_func, PYK_FUNC);
	kw_list(&kw_py, py_type, PYK_TYPE);
	kw_list(&kw_py, py_magicvar, PYK_MAGICVAR);
	kw_add(&kw_py, "def", 3, PYK_DEF);
	kw_add(&kw_py, "class", 5, PYK_CLASS);
	kw_add(&kw_py, "async", 5, PYK_ASYNC);
	kw_list(&kw_py, "match case", PYK_SOFT);
}

static inline bool py_ident(u8 c) { return is_alpha(c) || is_digit(c) || c == '_' || c >= 0x80; }

/* Python number at i (caller checked the first char); returns its end. */
static u32 py_number(const char *s, u32 n, u32 i, SynSpans *o)
{
	u32 a = i;
	if (s[i] == '0' && i + 1 < n && (lower((u8)s[i + 1]) == 'x' || lower((u8)s[i + 1]) == 'o' || lower((u8)s[i + 1]) == 'b')) {
		i += 2;
		while (i < n && (is_xdigit((u8)s[i]) || s[i] == '_'))
			i++;
	} else {
		while (i < n && (is_digit((u8)s[i]) || s[i] == '_'))
			i++;
		if (i < n && s[i] == '.') {
			i++;
			while (i < n && (is_digit((u8)s[i]) || s[i] == '_'))
				i++;
		}
		if (i < n && lower((u8)s[i]) == 'e') {
			u32 k = i + 1;
			if (k < n && (s[k] == '+' || s[k] == '-'))
				k++;
			if (k < n && is_digit((u8)s[k])) {
				i = k;
				while (i < n && (is_digit((u8)s[i]) || s[i] == '_'))
					i++;
			}
		}
	}
	emit(o, a, i - a, TK_NUMBER);
	if (i < n && lower((u8)s[i]) == 'j') {
		emit(o, i, 1, TK_STORAGE); /* storage.type.imaginary.number */
		i++;
	}
	return i;
}

static u32 py_code(const char *s, u32 n, u32 i, u32 end, u32 *st, SynSpans *o, bool in_fstring);

/* Placeholders of str.format / %-format in a plain string piece. */
static u32 py_placeholder(const char *s, u32 n, u32 i)
{
	if (s[i] == '%') {
		u32 k = i + 1;
		if (k < n && s[k] == '%')
			return k + 1;
		if (k < n && s[k] == '(') {
			while (k < n && s[k] != ')')
				k++;
			if (k >= n)
				return i;
			k++;
		}
		while (k < n && strchr("#0- +", s[k]))
			k++;
		while (k < n && (is_digit((u8)s[k]) || s[k] == '.' || s[k] == '*'))
			k++;
		if (k < n && strchr("diouxXeEfFgGcrsab", s[k]))
			return k + 1;
		return i;
	}
	if (s[i] == '{') {
		u32 k = i + 1;
		while (k < n && s[k] != '}' && s[k] != '{' && s[k] != '\'' && s[k] != '"' && s[k] != ' ')
			k++;
		if (k < n && s[k] == '}')
			return k + 1;
	}
	return i;
}

/* Body of a string from i until its closing quote; returns the end and
 * sets *closed. quote: the quote byte; tri: triple quoted. */
static u32 py_string_body(const char *s, u32 n, u32 i, u8 quote, bool tri, u32 flags, bool *closed, SynSpans *o)
{
	u32 base = (flags & PYF_REGEX) ? TK_REGEX : TK_STRING;
	bool raw = flags & PYF_RAW;
	bool fstr = flags & PYF_F;
	*closed = false;
	u32 seg = i;
	while (i < n) {
		u8 c = (u8)s[i];
		if (c == quote && (!tri || (i + 2 < n && (u8)s[i + 1] == quote && (u8)s[i + 2] == quote))) {
			u32 ql = tri ? 3 : 1;
			emit(o, seg, i + ql - seg, base);
			*closed = true;
			return i + ql;
		}
		if (c == '\\' && i + 1 < n) {
			emit(o, seg, i - seg, base);
			if (raw) {
				/* raw: \\ and \' are kept; in regexps \d etc are classes */
				u8 d = (u8)s[i + 1];
				bool cls = (flags & PYF_REGEX) && strchr("dDwWsSbBAZ", d);
				emit(o, i, 2, cls ? TK_REGEX : (flags & PYF_REGEX) ? TK_ESCAPE : base);
				i += 2;
			} else {
				u32 k = i + 2;
				u8 d = (u8)s[i + 1];
				if (d == 'x')
					k = VX_MIN(n, i + 4);
				else if (d == 'u' && !(flags & PYF_BYTES))
					k = VX_MIN(n, i + 6);
				else if (d == 'U' && !(flags & PYF_BYTES))
					k = VX_MIN(n, i + 10);
				else if (d == 'N' && i + 2 < n && s[i + 2] == '{') {
					k = i + 3;
					while (k < n && s[k] != '}')
						k++;
					k = VX_MIN(n, k + 1);
				} else if (d >= '0' && d <= '7') {
					k = i + 1;
					while (k < n && k < i + 4 && s[k] >= '0' && s[k] <= '7')
						k++;
				}
				emit(o, i, k - i, TK_ESCAPE);
				i = k;
			}
			seg = i;
			continue;
		}
		if (fstr && (c == '{' || c == '}')) {
			emit(o, seg, i - seg, base);
			if (i + 1 < n && s[i + 1] == (char)c) {
				emit(o, i, 2, TK_ESCAPE); /* {{ and }} */
				i += 2;
				seg = i;
				continue;
			}
			if (c == '}') {
				emit(o, i, 1, TK_STORAGE);
				seg = ++i;
				continue;
			}
			/* replacement field: { expr [!conv] [:spec] } */
			emit(o, i, 1, TK_STORAGE);
			u32 k = i + 1, depth = 0;
			u8 q = 0;
			while (k < n) {
				u8 e = (u8)s[k];
				if (q) {
					if (e == q)
						q = 0;
				} else if (e == '\'' || e == '"') {
					if (e == quote)
						break;
					q = e;
				} else if (e == '(' || e == '[' || e == '{') {
					depth++;
				} else if ((e == ')' || e == ']') && depth) {
					depth--;
				} else if (e == '}') {
					if (!depth)
						break;
					depth--;
				} else if (!depth && (e == '!' || e == ':') && !(e == '!' && k + 1 < n && s[k + 1] == '=')) {
					break;
				}
				k++;
			}
			u32 dummy = 0;
			py_code(s, n, i + 1, k, &dummy, o, true);
			i = k;
			if (i < n && (s[i] == '!' || s[i] == ':')) {
				u32 spec = i;
				while (i < n && s[i] != '}' && (u8)s[i] != quote)
					i++;
				emit(o, spec, i - spec, TK_STORAGE); /* storage.type.format */
			}
			if (i < n && s[i] == '}') {
				emit(o, i, 1, TK_STORAGE);
				i++;
			}
			seg = i;
			continue;
		}
		if (!fstr && !raw && !(flags & PYF_BYTES) && (c == '%' || c == '{')) {
			u32 k = py_placeholder(s, n, i);
			if (k > i) {
				emit(o, seg, i - seg, base);
				emit(o, i, k - i, TK_STORAGE);
				i = k;
				seg = i;
				continue;
			}
		}
		i++;
	}
	emit(o, seg, n - seg, base);
	return n;
}

/* Lex code in [i, end); returns end. *st carries the def/class context. */
static u32 py_code(const char *s, u32 n, u32 i, u32 end, u32 *st, SynSpans *o, bool in_fstring)
{
	u32 ctx = *st & (PY_DEFP | PY_CLASSB | PY_EXPECT);
	u32 depth = (*st >> 10) & 15u;
	u32 next_name = 0; /* 1 def name, 2 class name */
	bool line_start = true;
	for (u32 k = 0; k < i; k++)
		if (!is_space((u8)s[k]))
			line_start = false;
	while (i < end) {
		u8 c = (u8)s[i];
		if (is_space(c)) {
			i++;
			continue;
		}
		if (c == '#' && !in_fstring) {
			emit(o, i, end - i, TK_COMMENT);
			i = end;
			break;
		}
		/* strings, with prefixes */
		u32 p = i;
		while (p < end && p - i < 3 && strchr("rRbBuUfF", s[p]))
			p++;
		if (p < end && (s[p] == '\'' || s[p] == '"') && (p == i || !py_ident((u8)(i ? s[i - 1] : ' ')))) {
			u32 flags = 0;
			bool lower_r = false;
			for (u32 k = i; k < p; k++) {
				u8 l = lower((u8)s[k]);
				if (l == 'r') {
					flags |= PYF_RAW;
					lower_r |= s[k] == 'r';
				} else if (l == 'f')
					flags |= PYF_F;
				else if (l == 'b')
					flags |= PYF_BYTES;
			}
			/* MagicPython: r'', rb'', br'' and ur'' are regular expressions */
			if (lower_r && !(flags & PYF_F))
				flags |= PYF_REGEX;
			emit(o, i, p - i, TK_STORAGE);
			u8 q = (u8)s[p];
			bool tri = p + 2 < end && (u8)s[p + 1] == q && (u8)s[p + 2] == q;
			bool closed;
			if (in_fstring)
				tri = false;
			u32 qs = p, body = p + (tri ? 3 : 1);
			emit(o, qs, body - qs, (flags & PYF_REGEX) ? TK_REGEX : TK_STRING);
			i = py_string_body(s, in_fstring ? end : n, body, q, tri, flags, &closed, o);
			if (!closed && !in_fstring) {
				u32 mode = tri ? (q == '\'' ? PYS_TRI1 : PYS_TRI2) : PYS_NONE;
				if (!tri && n && s[n - 1] == '\\')
					mode = q == '\'' ? PYS_CONT1 : PYS_CONT2;
				*st = mode | flags | ctx | (depth << 10);
				return n;
			}
			line_start = false;
			continue;
		}
		if (is_digit(c) || (c == '.' && i + 1 < end && is_digit((u8)s[i + 1]))) {
			i = py_number(s, end, i, o);
			line_start = false;
			continue;
		}
		if (c == '@' && line_start) {
			/* decorator: @name.attr */
			u32 k = i + 1;
			while (k < end && (py_ident((u8)s[k]) || s[k] == '.'))
				k++;
			emit(o, i, k - i, TK_FUNCTION);
			i = k;
			line_start = false;
			continue;
		}
		if (py_ident(c)) {
			u32 a = i;
			while (i < end && py_ident((u8)s[i]))
				i++;
			u32 len = i - a;
			bool after_dot = a > 0 && s[a - 1] == '.';
			u32 nx = skip_ws(s, end, i);
			if (next_name) {
				emit(o, a, len, next_name == 1 ? TK_FUNCTION : TK_TYPE);
				if (next_name == 1 && nx < end && s[nx] == '(') {
					ctx = PY_DEFP | PY_EXPECT;
					depth = 0;
				} else if (next_name == 2 && nx < end && s[nx] == '(') {
					ctx = PY_CLASSB | PY_EXPECT;
					depth = 0;
				}
				next_name = 0;
				line_start = false;
				continue;
			}
			int kc = after_dot ? -1 : kw_find(&kw_py, s + a, len);
			bool is_self = !after_dot && (word_is(s + a, len, "self") || word_is(s + a, len, "cls"));
			if ((ctx & PY_DEFP) && depth == 1 && (ctx & PY_EXPECT)) {
				emit(o, a, len, TK_VARIABLE); /* variable.parameter.function */
				ctx &= ~PY_EXPECT;
			} else if ((ctx & PY_CLASSB) && depth == 1 && (ctx & PY_EXPECT) && !(nx < end && s[nx] == '=')) {
				emit(o, a, len, TK_TYPE); /* entity.other.inherited-class */
				ctx &= ~PY_EXPECT;
			} else if (is_self) {
				emit(o, a, len, TK_LANGCONST); /* variable.language.special.self */
			} else if (kc == PYK_DEF) {
				emit(o, a, len, TK_STORAGE);
				next_name = 1;
			} else if (kc == PYK_CLASS) {
				emit(o, a, len, TK_STORAGE);
				next_name = 2;
			} else if (kc == PYK_ASYNC) {
				bool before_def = nx + 3 <= end && !memcmp(s + nx, "def", 3);
				emit(o, a, len, before_def ? TK_STORAGE : TK_CONTROL);
			} else if (kc == PYK_SOFT) {
				/* match/case are keywords only as statements ending in ':' */
				u32 e = end;
				while (e > i && is_space((u8)s[e - 1]))
					e--;
				bool stmt = line_start && e > i && s[e - 1] == ':' && nx < end && s[nx] != '=' && s[nx] != '.' && s[nx] != '(';
				emit(o, a, len, stmt ? TK_CONTROL : TK_TEXT);
			} else if (kc == PYK_CTRL) {
				emit(o, a, len, TK_CONTROL);
			} else if (kc == PYK_STORAGE || kc == PYK_LOGIC) {
				emit(o, a, len, TK_STORAGE);
			} else if (kc == PYK_CONST) {
				emit(o, a, len, TK_LANGCONST);
			} else if (kc == PYK_FUNC) {
				emit(o, a, len, TK_FUNCTION);
			} else if (kc == PYK_TYPE) {
				emit(o, a, len, TK_TYPE);
			} else if (kc == PYK_MAGICVAR) {
				emit(o, a, len, TK_VARIABLE);
			} else if (len > 4 && s[a] == '_' && s[a + 1] == '_' && s[i - 1] == '_' && s[i - 2] == '_') {
				emit(o, a, len, TK_FUNCTION); /* support.function.magic */
			} else if (depth && nx < end && s[nx] == '=' && !(nx + 1 < end && s[nx + 1] == '=') && !(ctx & (PY_DEFP | PY_CLASSB))) {
				emit(o, a, len, TK_VARIABLE); /* variable.parameter.function-call (f(x=1)) */
			}
			line_start = false;
			continue;
		}
		/* punctuation and operators */
		if (c == '(' || c == '[' || c == '{') {
			depth++;
			if ((ctx & (PY_DEFP | PY_CLASSB)) && depth == 1 && c == '(')
				ctx |= PY_EXPECT;
		} else if (c == ')' || c == ']' || c == '}') {
			if (depth)
				depth--;
			if ((ctx & (PY_DEFP | PY_CLASSB)) && depth == 0)
				ctx = 0;
		} else if (c == ',' && depth == 1 && (ctx & (PY_DEFP | PY_CLASSB))) {
			ctx |= PY_EXPECT;
		} else if (c == '*' && depth == 1 && (ctx & PY_DEFP)) {
			/* *args, **kwargs keep expecting a name */
		} else if ((c == ':' || c == '=') && depth == 1 && (ctx & PY_DEFP)) {
			ctx &= ~PY_EXPECT; /* annotation or default value */
		} else if (c == '/' && depth == 1 && (ctx & PY_DEFP)) {
		} else if (strchr("+-*/%=<>!&|^~@", c)) {
			u32 k = i + 1;
			while (k < end && strchr("+-*/%=<>!&|^~@", s[k]) && k - i < 3)
				k++;
			if (!(c == '-' && k == i + 1 && i + 1 < end && s[i + 1] == '>'))
				emit(o, i, k - i, TK_OPERATOR);
			i = k;
			line_start = false;
			continue;
		}
		i++;
		line_start = false;
	}
	if (ctx & (PY_DEFP | PY_CLASSB))
		*st = ctx | (VX_MIN(depth, 15u) << 10);
	else
		*st = 0;
	return end;
}

static u32 lex_python(u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 mode = state & 7u;
	u32 flags = state & (PYF_RAW | PYF_F | PYF_BYTES | PYF_REGEX);
	u32 ctx = state & (PY_DEFP | PY_CLASSB | PY_EXPECT | (15u << 10));
	u32 i = 0;
	if (mode != PYS_NONE) {
		u8 q = (mode == PYS_TRI1 || mode == PYS_CONT1) ? '\'' : '"';
		bool tri = mode == PYS_TRI1 || mode == PYS_TRI2;
		bool closed;
		i = py_string_body(s, n, 0, q, tri, flags, &closed, o);
		if (!closed) {
			if (!tri && !(n && s[n - 1] == '\\'))
				return ctx; /* an unterminated single-quoted string ends at the line */
			return state;
		}
	}
	u32 st = ctx;
	py_code(s, n, i, n, &st, o, false);
	return st;
}

/* ================================================================== */
/* YAML                                                                */
/*
 * Per VSCodium's yaml-1.2 grammar under Dark+/Light+: mapping keys are
 * entity.name.tag (TK_TAG: #569CD6 / #800000); plain and block scalars
 * and single-quoted strings are string.unquoted / string.quoted.single,
 * which Light+ shows #0000FF (TK_UNQUOTED); double-quoted strings
 * TK_STRING with escapes; true/false/null constant.language; numbers;
 * anchors and aliases: & and * keyword.control.flow (TK_CONTROL), their
 * names variable.other (TK_VARIABLE); tags storage.type (TK_STORAGE);
 * block scalar headers | > keyword.control.flow, chomping storage.modifier;
 * %directives keyword.other (TK_STORAGE); document markers and
 * punctuation have no colour.
 *
 * State: bits 0-1 mode (0 normal, 1 block scalar, 2 "..." continued,
 * 3 '...' continued), bits 2-9 the indentation the block scalar must
 * exceed, bits 10-13 flow-collection depth. */

enum { YM_NORMAL, YM_BLOCK, YM_DQ, YM_SQ };

static u32 yaml_quoted(const char *s, u32 n, u32 i, u8 q, bool *closed, SynSpans *o)
{
	u32 cls = q == '"' ? TK_STRING : TK_UNQUOTED;
	u32 seg = i;
	*closed = false;
	while (i < n) {
		if ((u8)s[i] == q) {
			if (q == '\'' && i + 1 < n && s[i + 1] == '\'') {
				emit(o, seg, i - seg, cls);
				emit(o, i, 2, TK_ESCAPE);
				i += 2;
				seg = i;
				continue;
			}
			emit(o, seg, i + 1 - seg, cls);
			*closed = true;
			return i + 1;
		}
		if (q == '"' && s[i] == '\\' && i + 1 < n) {
			emit(o, seg, i - seg, cls);
			u32 k = i + 2;
			u8 d = (u8)s[i + 1];
			if (d == 'x')
				k = VX_MIN(n, i + 4);
			else if (d == 'u')
				k = VX_MIN(n, i + 6);
			else if (d == 'U')
				k = VX_MIN(n, i + 10);
			emit(o, i, k - i, TK_ESCAPE);
			i = k;
			seg = i;
			continue;
		}
		i++;
	}
	emit(o, seg, n - seg, cls);
	return n;
}

/* Classify a plain scalar s[a, b): constant, number or string. */
static u32 yaml_plain_cls(const char *s, u32 a, u32 b)
{
	u32 l = b - a;
	const char *w = s + a;
	static const char *const consts[] = {"true", "True", "TRUE", "false", "False", "FALSE", "null", "Null", "NULL", "~"};
	for (size_t k = 0; k < VX_COUNT(consts); k++)
		if (word_is(w, l, consts[k]))
			return TK_LANGCONST;
	static const char *const special[] = {".inf", ".Inf", ".INF", "-.inf", "+.inf", "-.Inf", "+.Inf", ".nan", ".NaN", ".NAN"};
	for (size_t k = 0; k < VX_COUNT(special); k++)
		if (word_is(w, l, special[k]))
			return TK_NUMBER;
	u32 i = 0;
	if (i < l && (w[i] == '-' || w[i] == '+'))
		i++;
	if (i + 1 < l && w[i] == '0' && (w[i + 1] == 'x' || w[i + 1] == 'o')) {
		u32 k = i + 2;
		while (k < l && is_xdigit((u8)w[k]))
			k++;
		return k == l && k > i + 2 ? TK_NUMBER : TK_UNQUOTED;
	}
	bool digits = false, ok = true;
	for (; i < l && ok; i++) {
		u8 c = (u8)w[i];
		if (is_digit(c))
			digits = true;
		else if (c == '.' || c == '_')
			;
		else if ((c == 'e' || c == 'E') && digits && i + 1 < l) {
			if (w[i + 1] == '+' || w[i + 1] == '-')
				i++;
		} else
			ok = false;
	}
	return ok && digits ? TK_NUMBER : TK_UNQUOTED;
}

static u32 lex_yaml(u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 mode = state & 3u;
	u32 bind = (state >> 2) & 0xFFu;
	u32 flow = (state >> 10) & 15u;
	u32 indent = 0;
	while (indent < n && s[indent] == ' ')
		indent++;
	u32 i = 0;
	if (mode == YM_BLOCK) {
		/* block scalar: blank lines and lines indented past the parent */
		if (indent >= n || indent > bind) {
			emit(o, indent, n - indent, TK_UNQUOTED);
			return state;
		}
		mode = YM_NORMAL;
	} else if (mode == YM_DQ || mode == YM_SQ) {
		bool closed;
		i = yaml_quoted(s, n, 0, mode == YM_DQ ? '"' : '\'', &closed, o);
		if (!closed)
			return state;
		mode = YM_NORMAL;
	}
	i = VX_MAX(i, indent);
	if (i < n && s[i] == '#' && mode == YM_NORMAL) {
		emit(o, i, n - i, TK_COMMENT);
		return flow << 10;
	}
	if (i == 0 && n >= 1 && s[0] == '%') {
		/* %YAML 1.2 / %TAG ! tag:... */
		u32 k = 1;
		while (k < n && !is_space((u8)s[k]))
			k++;
		emit(o, 0, k, TK_STORAGE);
		emit(o, k, n - k, TK_UNQUOTED);
		return 0;
	}
	if (i == 0 && n >= 3 && (!memcmp(s, "---", 3) || !memcmp(s, "...", 3)) && (n == 3 || is_space((u8)s[3]))) {
		i = 3; /* document markers: no colour */
		flow = 0;
	}
	bool value = false; /* after "key:" or at a node position */
	while (i < n) {
		u8 c = (u8)s[i];
		if (is_space(c)) {
			i++;
			continue;
		}
		if (c == '#' && (i == 0 || is_space((u8)s[i - 1]))) {
			emit(o, i, n - i, TK_COMMENT);
			break;
		}
		if ((c == '-' || c == '?' || c == ':') && (i + 1 >= n || is_space((u8)s[i + 1])) && !flow) {
			/* sequence item, explicit key, or value separator (no colour) */
			value = c == ':';
			i++;
			continue;
		}
		if (c == '[' || c == '{') {
			flow++;
			i++;
			continue;
		}
		if ((c == ']' || c == '}') && flow) {
			flow--;
			i++;
			continue;
		}
		if (c == ',' && flow) {
			i++;
			continue;
		}
		if (c == '&' || c == '*') {
			u32 k = i + 1;
			while (k < n && !is_space((u8)s[k]) && !strchr(",[]{}", s[k]))
				k++;
			emit(o, i, 1, TK_CONTROL);
			emit(o, i + 1, k - i - 1, TK_VARIABLE);
			i = k;
			continue;
		}
		if (c == '!') {
			u32 k = i + 1;
			while (k < n && !is_space((u8)s[k]) && !strchr(",[]{}", s[k]))
				k++;
			emit(o, i, k - i, TK_STORAGE);
			i = k;
			continue;
		}
		if ((c == '|' || c == '>') && !flow) {
			/* block scalar header: indicator, chomping +/-, indentation digit */
			u32 k = i + 1;
			emit(o, i, 1, TK_CONTROL);
			while (k < n && (s[k] == '+' || s[k] == '-' || is_digit((u8)s[k]))) {
				emit(o, k, 1, is_digit((u8)s[k]) ? TK_NUMBER : TK_STORAGE);
				k++;
			}
			k = skip_ws(s, n, k);
			if (k < n && s[k] == '#')
				emit(o, k, n - k, TK_COMMENT);
			return YM_BLOCK | (VX_MIN(indent, 255u) << 2);
		}
		if (c == '"' || c == '\'') {
			bool closed;
			u32 a = i;
			emit(o, i, 1, c == '"' ? TK_STRING : TK_UNQUOTED); /* the opening quote */
			u32 k = yaml_quoted(s, n, i + 1, c, &closed, o);
			if (!closed)
				return (c == '"' ? YM_DQ : YM_SQ) | (flow << 10);
			u32 nx = skip_ws(s, n, k);
			if (nx < n && s[nx] == ':' && (nx + 1 >= n || is_space((u8)s[nx + 1]) || flow)) {
				/* a quoted key: re-colour as entity.name.tag */
				if (o) {
					while (o->count && o->start[o->count - 1] >= a)
						o->count--;
					/* a span merged across the key's start is cut back */
					if (o->count && o->start[o->count - 1] + o->len[o->count - 1] > a)
						o->len[o->count - 1] = a - o->start[o->count - 1];
				}
				emit(o, a, k - a, TK_TAG);
			}
			i = k;
			continue;
		}
		/* plain scalar: up to ": ", " #", or (in flow) , ] } */
		u32 a = i, e = i, key_end = UINT32_MAX;
		while (e < n) {
			u8 d = (u8)s[e];
			if (d == ':' && (e + 1 >= n || is_space((u8)s[e + 1]) || (flow && strchr(",[]{}", s[e + 1])))) {
				key_end = e;
				break;
			}
			if (d == '#' && e > a && is_space((u8)s[e - 1]))
				break;
			if (flow && strchr(",[]{}", d))
				break;
			e++;
		}
		u32 te = key_end != UINT32_MAX ? key_end : e;
		while (te > a && is_space((u8)s[te - 1]))
			te--;
		if (key_end != UINT32_MAX) {
			emit(o, a, te - a, TK_TAG);
			i = key_end + 1;
			value = true;
			continue;
		}
		emit(o, a, te - a, yaml_plain_cls(s, a, te));
		i = e;
		(void)value;
	}
	return VX_MIN(flow, 15u) << 10;
}

/* ================================================================== */
/* CODEOWNERS                                                          */
/*
 * No VSCodium grammar: patterns are coloured like ignore files (globs
 * TK_REGEX, anchoring / TK_OPERATOR, escapes), owners (@user,
 * @org/team) TK_VARIABLE, e-mail owners TK_STRING, GitLab sections
 * ([Section], ^[Optional][2]) TK_HEADING, comments TK_COMMENT. */
static u32 lex_codeowners(u32 state, const char *s, u32 n, SynSpans *o)
{
	(void)state;
	u32 i = skip_ws(s, n, 0);
	if (i >= n)
		return 0;
	if (s[i] == '#') {
		emit(o, i, n - i, TK_COMMENT);
		return 0;
	}
	if (s[i] == '[' || (s[i] == '^' && i + 1 < n && s[i + 1] == '[')) {
		u32 k = i;
		if (s[k] == '^') {
			emit(o, k, 1, TK_OPERATOR);
			k++;
		}
		u32 a = k;
		while (k < n && s[k] != ']')
			k++;
		if (k < n)
			k++;
		emit(o, a, k - a, TK_HEADING);
		if (k < n && s[k] == '[') {
			u32 b = k;
			while (k < n && s[k] != ']')
				k++;
			if (k < n)
				k++;
			emit(o, b, k - b, TK_NUMBER); /* required approvals */
		}
		i = k;
	} else {
		/* the path pattern */
		u32 e = i;
		while (e < n && !is_space((u8)s[e])) {
			if (s[e] == '\\' && e + 1 < n)
				e++;
			e++;
		}
		for (u32 k = i; k < e; k++) {
			u8 c = (u8)s[k];
			if (c == '\\' && k + 1 < e) {
				emit(o, k, 2, TK_ESCAPE);
				k++;
			} else if (c == '*' || c == '?') {
				emit(o, k, 1, TK_REGEX);
			} else if (c == '[') {
				u32 j = k + 1;
				while (j < e && s[j] != ']')
					j++;
				if (j < e) {
					emit(o, k, j - k + 1, TK_REGEX);
					k = j;
				}
			} else if (c == '/' && (k == i || k + 1 == e)) {
				emit(o, k, 1, TK_OPERATOR);
			}
		}
		i = e;
	}
	/* owners */
	while (i < n) {
		i = skip_ws(s, n, i);
		if (i >= n)
			break;
		if (s[i] == '#') {
			emit(o, i, n - i, TK_COMMENT);
			break;
		}
		u32 a = i;
		while (i < n && !is_space((u8)s[i]))
			i++;
		if (s[a] == '@')
			emit(o, a, i - a, TK_VARIABLE);
		else if (memchr(s + a, '@', i - a))
			emit(o, a, i - a, TK_STRING);
	}
	return 0;
}

static int md_fence_lang(const char *w, u32 n)
{
	char b[16];
	if (n == 0 || n >= sizeof b)
		return LANG_NONE;
	for (u32 i = 0; i < n; i++)
		b[i] = (char)lower((u8)w[i]);
	b[n] = 0;
	static const struct {
		const char *name;
		u8 lang;
	} map[] = {
	    {"c", LANG_C},          {"h", LANG_C},           {"sh", LANG_SH},          {"bash", LANG_SH},
	    {"shell", LANG_SH},     {"zsh", LANG_SH},        {"ksh", LANG_SH},         {"console", LANG_SH},
	    {"shellscript", LANG_SH}, {"perl", LANG_PERL},   {"pl", LANG_PERL},        {"pm", LANG_PERL},
	    {"pod", LANG_PERL},     {"lisp", LANG_LISP},     {"elisp", LANG_LISP},     {"scheme", LANG_LISP},
	    {"clojure", LANG_LISP}, {"clj", LANG_LISP},      {"cljs", LANG_LISP},      {"racket", LANG_LISP},
	    {"commonlisp", LANG_LISP}, {"asm", LANG_ASM_INTEL}, {"nasm", LANG_ASM_INTEL}, {"x86asm", LANG_ASM_INTEL},
	    {"masm", LANG_ASM_INTEL}, {"gas", LANG_ASM_GAS}, {"att", LANG_ASM_GAS},    {"s", LANG_ASM_GAS},
	    {"troff", LANG_TROFF},  {"roff", LANG_TROFF},    {"groff", LANG_TROFF},    {"nroff", LANG_TROFF},
	    {"man", LANG_TROFF},    {"mdoc", LANG_TROFF},
	    {"cmake", LANG_CMAKE},  {"make", LANG_MAKE},     {"makefile", LANG_MAKE},  {"mk", LANG_MAKE},
	    {"bsdmake", LANG_MAKE}, {"json", LANG_JSON},     {"jsonc", LANG_JSONC},    {"json5", LANG_JSONC},
	    {"gitignore", LANG_IGNORE}, {"ignore", LANG_IGNORE}, {"diff", LANG_DIFF},  {"patch", LANG_DIFF},
	    {"gitcommit", LANG_GIT_COMMIT}, {"git-commit", LANG_GIT_COMMIT},
	    {"gitrebase", LANG_GIT_REBASE}, {"git-rebase", LANG_GIT_REBASE},
	    {"python", LANG_PYTHON}, {"py", LANG_PYTHON}, {"python3", LANG_PYTHON}, {"py3", LANG_PYTHON},
	    {"yaml", LANG_YAML},    {"yml", LANG_YAML},      {"codeowners", LANG_CODEOWNERS},
	};
	for (size_t i = 0; i < VX_COUNT(map); i++)
		if (!strcmp(b, map[i].name))
			return map[i].lang;
	return LANG_NONE;
}

/* Find a closing run of `k` backticks/markers starting at or after i. */
static u32 md_find(const char *s, u32 n, u32 i, const char *mk, u32 ml)
{
	for (; i + ml <= n; i++) {
		if (s[i] == '\\') {
			i++;
			continue;
		}
		if (memcmp(s + i, mk, ml) == 0)
			return i;
	}
	return UINT32_MAX;
}

static u32 md_inline(const char *s, u32 n, u32 i, u32 base, SynSpans *o, u32 *mode);

/* HTML tag at i (s[i] == '<'); returns its end or i when not a tag. */
static u32 md_tag(const char *s, u32 n, u32 i, SynSpans *o)
{
	u32 j = i + 1;
	bool close = j < n && s[j] == '/';
	if (close)
		j++;
	if (j >= n || !is_alpha((u8)s[j]))
		return i;
	u32 a = j;
	while (j < n && (is_word((u8)s[j]) || s[j] == '-'))
		j++;
	u32 name_end = j;
	/* scan attributes up to '>' */
	u32 k = j;
	while (k < n && s[k] != '>' && s[k] != '<') {
		if (s[k] == '"' || s[k] == '\'') {
			u8 q = (u8)s[k];
			k++;
			while (k < n && (u8)s[k] != q)
				k++;
		}
		if (k < n)
			k++;
	}
	if (k >= n || s[k] != '>')
		return i;
	emit(o, i, a - i, TK_TAGPUNCT);
	emit(o, a, name_end - a, TK_TAG);
	k = j;
	while (k < n && s[k] != '>') {
		u8 ch = (u8)s[k];
		if (is_alpha(ch) || ch == '_' || ch == ':') {
			u32 e = k;
			while (e < n && (is_word((u8)s[e]) || s[e] == '-' || s[e] == ':'))
				e++;
			emit(o, k, e - k, TK_ATTR);
			k = e;
		} else if (ch == '"' || ch == '\'') {
			u32 e = k + 1;
			while (e < n && s[e] != (char)ch)
				e++;
			if (e < n)
				e++;
			emit(o, k, e - k, TK_STRING);
			k = e;
		} else if (ch == '/') {
			emit(o, k, 1, TK_TAGPUNCT);
			k++;
		} else {
			k++;
		}
	}
	emit(o, k, 1, TK_TAGPUNCT);
	return k + 1;
}

static bool md_url_start(const char *s, u32 n, u32 i)
{
	return (n - i > 7 && memcmp(s + i, "http://", 7) == 0) || (n - i > 8 && memcmp(s + i, "https://", 8) == 0) ||
	       (n - i > 4 && memcmp(s + i, "www.", 4) == 0);
}

/* Inline spans in [i, n) over base class `base`; *mode may become an open
 * HTML comment. */
static u32 md_inline(const char *s, u32 n, u32 i, u32 base, SynSpans *o, u32 *mode)
{
	while (i < n) {
		u8 ch = (u8)s[i];
		if (ch == '\\' && i + 1 < n && in_set("\\`*_{}[]()#+-.!<>|~", (u8)s[i + 1])) {
			emit(o, i, 2, TK_ESCAPE);
			i += 2;
			continue;
		}
		if (ch == '`') {
			u32 k = i;
			while (k < n && s[k] == '`')
				k++;
			u32 run = k - i;
			u32 e = k;
			for (;;) {
				while (e < n && s[e] != '`')
					e++;
				if (e >= n)
					break;
				u32 f = e;
				while (f < n && s[f] == '`')
					f++;
				if (f - e == run) {
					e = f;
					break;
				}
				e = f;
			}
			if (e <= n && e > k && s[e - 1] == '`') {
				emit(o, i, e - i, TK_RAW);
				i = e;
				continue;
			}
			emit(o, i, run, base);
			i = k;
			continue;
		}
		if (ch == '<' && n - i >= 4 && memcmp(s + i, "<!--", 4) == 0) {
			u32 e = md_find(s, n, i + 4, "-->", 3);
			if (e == UINT32_MAX) {
				emit(o, i, n - i, TK_COMMENT);
				*mode = MD_HTMLCMT;
				return n;
			}
			emit(o, i, e + 3 - i, TK_COMMENT);
			i = e + 3;
			continue;
		}
		if (ch == '<') {
			/* autolink <http://...> or tag */
			u32 e = i + 1;
			while (e < n && s[e] != '>' && s[e] != ' ')
				e++;
			if (e < n && s[e] == '>' && (md_url_start(s, e, i + 1) || memchr(s + i + 1, '@', e - i - 1))) {
				emit(o, i + 1, e - i - 1, TK_LINK);
				i = e + 1;
				continue;
			}
			u32 t = md_tag(s, n, i, o);
			if (t > i) {
				i = t;
				continue;
			}
		}
		if ((ch == '*' || ch == '_') && i + 1 < n && s[i + 1] == ch) {
			char mk[2] = {(char)ch, (char)ch};
			bool open_ok = i + 2 < n && !is_space((u8)s[i + 2]) && (ch == '*' || i == 0 || !is_word((u8)s[i - 1]));
			u32 e = open_ok ? md_find(s, n, i + 2, mk, 2) : UINT32_MAX;
			if (e != UINT32_MAX && e > i + 2 && !is_space((u8)s[e - 1])) {
				emit(o, i, 2, TK_BOLD);
				md_inline(s, e, i + 2, TK_BOLD, o, mode);
				emit(o, e, 2, TK_BOLD);
				i = e + 2;
				continue;
			}
		}
		if (ch == '*' || ch == '_') {
			bool open_ok = i + 1 < n && !is_space((u8)s[i + 1]) && (ch == '*' || i == 0 || !is_word((u8)s[i - 1]));
			u32 e = UINT32_MAX;
			if (open_ok)
				for (u32 k = i + 1; k < n; k++) {
					if (s[k] == '\\') {
						k++;
						continue;
					}
					if (s[k] == ch && !(k + 1 < n && s[k + 1] == ch) && !is_space((u8)s[k - 1]) &&
					    (ch == '*' || k + 1 >= n || !is_word((u8)s[k + 1]))) {
						e = k;
						break;
					}
					if (s[k] == ch && k + 1 < n && s[k + 1] == ch)
						k++;
				}
			if (e != UINT32_MAX && e > i + 1) {
				emit(o, i, 1, TK_ITALIC);
				md_inline(s, e, i + 1, TK_ITALIC, o, mode);
				emit(o, e, 1, TK_ITALIC);
				i = e + 1;
				continue;
			}
		}
		if (ch == '~' && i + 1 < n && s[i + 1] == '~') {
			u32 e = md_find(s, n, i + 2, "~~", 2);
			if (e != UINT32_MAX && e > i + 2) {
				emit(o, i, 2, TK_STRIKE);
				md_inline(s, e, i + 2, TK_STRIKE, o, mode);
				emit(o, e, 2, TK_STRIKE);
				i = e + 2;
				continue;
			}
		}
		if (ch == '[' || (ch == '!' && i + 1 < n && s[i + 1] == '[')) {
			u32 a = ch == '!' ? i + 2 : i + 1;
			int d = 1;
			u32 k = a;
			for (; k < n; k++) {
				if (s[k] == '\\') {
					k++;
					continue;
				}
				if (s[k] == '[')
					d++;
				else if (s[k] == ']' && --d == 0)
					break;
			}
			if (k < n && k + 1 < n && (s[k + 1] == '(' || s[k + 1] == '[')) {
				u8 cl = s[k + 1] == '(' ? ')' : ']';
				u32 e = k + 2;
				int pd = 1;
				for (; e < n; e++) {
					if (s[e] == (char)(cl == ')' ? '(' : '['))
						pd++;
					else if ((u8)s[e] == cl && --pd == 0)
						break;
				}
				if (e < n) {
					md_inline(s, k, a, TK_STRING, o, mode);
					if (cl == ')') {
						u32 u = k + 2, ue = u;
						while (ue < e && !is_space((u8)s[ue]))
							ue++;
						emit(o, u, ue - u, TK_LINK);
						if (ue < e)
							emit(o, ue, e - ue, TK_STRING); /* "title" */
					}
					i = e + 1;
					continue;
				}
			}
		}
		if ((ch == 'h' || ch == 'w') && (i == 0 || !is_word((u8)s[i - 1])) && md_url_start(s, n, i)) {
			u32 e = i;
			while (e < n && !is_space((u8)s[e]) && s[e] != ')' && s[e] != '>' && s[e] != '"')
				e++;
			while (e > i && in_set(".,;:!?", (u8)s[e - 1]))
				e--;
			emit(o, i, e - i, TK_LINK);
			i = e;
			continue;
		}
		emit(o, i, 1, base);
		i++;
	}
	return n;
}

static u32 lex_md(u32 state, const char *s, u32 n, SynSpans *o)
{
	u32 mode = state & 3u;
	if (mode == MD_FENCE) {
		u8 fch = (state & 0x80u) ? '~' : '`';
		u32 flen = (state >> 8) & 15u;
		u32 k = skip_ws(s, n, 0);
		u32 r = k;
		while (r < n && (u8)s[r] == fch)
			r++;
		if (r - k >= flen && skip_ws(s, n, r) == n)
			return MD_NORMAL; /* closing fence */
		int lang = (int)((state >> 2) & 31u);
		if (lang == LANG_NONE)
			return state;
		u32 inner = lex_any(lang, state >> 12, s, n, o) & ((1u << SYN_STATE_BITS) - 1u);
		return (state & 0xFFFu) | (inner << 12);
	}
	if (mode == MD_HTMLCMT) {
		u32 e = md_find(s, n, 0, "-->", 3);
		if (e == UINT32_MAX) {
			emit(o, 0, n, TK_COMMENT);
			return MD_HTMLCMT;
		}
		emit(o, 0, e + 3, TK_COMMENT);
		u32 m = MD_NORMAL;
		md_inline(s, n, e + 3, TK_TEXT, o, &m);
		return m;
	}
	bool para = state & MD_PARA;
	u32 i = 0;
	u32 ind = 0;
	while (i < n && s[i] == ' ') {
		i++;
		ind++;
	}
	if (i >= n)
		return MD_NORMAL; /* blank */
	u8 ch = (u8)s[i];
	/* fenced code */
	if ((ch == '`' || ch == '~') && ind <= 3) {
		u32 r = i;
		while (r < n && (u8)s[r] == ch)
			r++;
		if (r - i >= 3 && !(ch == '`' && memchr(s + r, '`', n - r))) {
			u32 a = skip_ws(s, n, r), b = a;
			while (b < n && !is_space((u8)s[b]) && s[b] != '{' && s[b] != ',')
				b++;
			int lang = md_fence_lang(s + a, b - a);
			u32 fl = VX_MIN(r - i, 15u); /* longer fences close on >= 15 marks */
			return MD_FENCE | ((u32)lang << 2) | (ch == '~' ? 0x80u : 0u) | (fl << 8);
		}
	}
	/* ATX heading */
	if (ch == '#' && ind <= 3) {
		u32 h = i;
		while (h < n && s[h] == '#')
			h++;
		if (h - i <= 6 && (h == n || is_space((u8)s[h]))) {
			emit(o, i, n - i, TK_HEADING);
			return MD_NORMAL;
		}
	}
	/* setext underline / thematic break */
	if ((ch == '=' || ch == '-' || ch == '*' || ch == '_') && ind <= 3) {
		u32 k = i, cnt = 0;
		bool only = true;
		while (k < n) {
			if ((u8)s[k] == ch)
				cnt++;
			else if (!is_space((u8)s[k]) && !(ch != '=' && is_space((u8)s[k])))
				only = false;
			k++;
		}
		if (only && (ch == '=' || ch == '-') && para && cnt >= 1) {
			bool contiguous = true;
			u32 e = n;
			while (e > i && is_space((u8)s[e - 1]))
				e--;
			for (u32 q = i; q < e; q++)
				if ((u8)s[q] != ch)
					contiguous = false;
			if (contiguous) {
				emit(o, i, e - i, TK_HEADING);
				return MD_NORMAL;
			}
		}
		if (only && cnt >= 3 && ch != '=')
			return MD_NORMAL; /* meta.separator: no colour */
	}
	u32 mode_out = MD_NORMAL;
	/* block quotes (possibly nested) */
	while (i < n && s[i] == '>') {
		emit(o, i, 1, TK_QUOTE);
		i = skip_ws(s, n, i + 1);
	}
	/* list markers */
	if (i < n && (s[i] == '*' || s[i] == '+' || s[i] == '-') && i + 1 < n && is_space((u8)s[i + 1])) {
		emit(o, i, 1, TK_LIST);
		i = skip_ws(s, n, i + 1);
		/* task list [ ] / [x] */
	} else if (i < n && is_digit((u8)s[i])) {
		u32 k = i;
		while (k < n && is_digit((u8)s[k]) && k < i + 9)
			k++;
		if (k < n && (s[k] == '.' || s[k] == ')') && (k + 1 >= n || is_space((u8)s[k + 1]))) {
			emit(o, i, k + 1 - i, TK_LIST);
			i = skip_ws(s, n, k + 1);
		}
	}
	md_inline(s, n, i, TK_TEXT, o, &mode_out);
	if (mode_out != MD_NORMAL)
		return mode_out;
	return MD_NORMAL | MD_PARA;
}

static u32 lex_any(int lang, u32 state, const char *s, u32 n, SynSpans *o)
{
	switch (lang) {
	case LANG_C: return lex_c(state, s, n, o);
	case LANG_ASM_GAS: return lex_asm(true, state, s, n, o);
	case LANG_ASM_INTEL: return lex_asm(false, state, s, n, o);
	case LANG_LISP: return lex_lisp(state, s, n, o);
	case LANG_SH: return lex_sh(state, s, n, o);
	case LANG_MARKDOWN: return lex_md(state, s, n, o);
	case LANG_TROFF: return lex_troff(state, s, n, o);
	case LANG_PERL: return lex_perl(state, s, n, o);
	case LANG_CMAKE: return lex_cmake(state, s, n, o);
	case LANG_MAKE: return lex_make(state, s, n, o);
	case LANG_JSON:
	case LANG_JSONC: return lex_json(state, s, n, o);
	case LANG_IGNORE: return lex_ignore(state, s, n, o);
	case LANG_DIFF: return lex_diff(state, s, n, o);
	case LANG_GIT_COMMIT: return lex_git_commit(state, s, n, o);
	case LANG_GIT_REBASE: return lex_git_rebase(state, s, n, o);
	case LANG_PYTHON: return lex_python(state, s, n, o);
	case LANG_YAML: return lex_yaml(state, s, n, o);
	case LANG_CODEOWNERS: return lex_codeowners(state, s, n, o);
	default: return 0;
	}
}

static void syn_build2(void)
{
	kw_list(&kw_sh, sh_ctrl_cmd, SHK_CTRL_CMD);
	kw_list(&kw_sh, sh_end, SHK_END);
	kw_list(&kw_sh, sh_loop, SHK_LOOP);
	kw_list(&kw_sh, sh_ctrl, SHK_CTRL);
	kw_list(&kw_sh, sh_decl, SHK_DECL);
	kw_add(&kw_sh, "function", 8, SHK_FUNCTION);
	kw_add(&kw_sh, "in", 2, SHK_IN);

	kw_add(&kw_pl, "sub", 3, PLK_SUB);
	kw_add(&kw_pl, "package", 7, PLK_PACKAGE);
	kw_list(&kw_pl, pl_ctrl, PLK_CTRL);
	kw_list(&kw_pl, pl_storage, PLK_STORAGE);
	kw_list(&kw_pl, pl_func, PLK_FUNC);
	kw_list(&kw_pl, pl_op, PLK_OP);
	kw_list(&kw_pl, pl_const, PLK_CONST);
	kw_list(&kw_pl, pl_block, PLK_BLOCK);

	kw_list(&kw_mdoc, mdoc_callable, 1);

	kw_list(&kw_cmake, cmake_control, TK_CONTROL);
	kw_list(&kw_cmake, cmake_const, TK_LANGCONST);
	kw_list(&kw_mkfn, mk_functions, MKF_FUNC);
	kw_list(&kw_mkfn, mk_builtins, MKF_BUILTIN);
	kw_list(&kw_mkdir, mk_gnu_dirs, 1);
	kw_list(&kw_bsddir, mk_bsd_dirs, 1);
	py_build();
}

/* ================================================================== */
/* Public API                                                          */

u32 syn_line(int lang, u32 state, const char *s, u32 n, SynSpans *out)
{
	syn_init();
	if (out)
		out->count = 0;
	return lex_any(lang, state, s, n, out);
}

const char *syn_lang_name(int lang)
{
	switch (lang) {
	case LANG_C: return "C";
	case LANG_ASM_GAS: return "x86 Assembly (AT&T)";
	case LANG_ASM_INTEL: return "x86 Assembly (Intel)";
	case LANG_LISP: return "Lisp";
	case LANG_SH: return "Shell Script";
	case LANG_MARKDOWN: return "Markdown";
	case LANG_TROFF: return "Roff";
	case LANG_PERL: return "Perl";
	case LANG_CMAKE: return "CMake";
	case LANG_MAKE: return "Makefile";
	case LANG_JSON: return "JSON";
	case LANG_JSONC: return "JSON with Comments";
	case LANG_IGNORE: return "Ignore";
	case LANG_DIFF: return "Diff";
	case LANG_GIT_COMMIT: return "Git Commit Message";
	case LANG_GIT_REBASE: return "Git Rebase Message";
	case LANG_PYTHON: return "Python";
	case LANG_YAML: return "YAML";
	case LANG_CODEOWNERS: return "CODEOWNERS";
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
	case LANG_SH: return "shellscript";
	case LANG_MARKDOWN: return "markdown";
	case LANG_PERL: return "perl";
	case LANG_CMAKE: return "cmake";
	case LANG_MAKE: return "makefile";
	case LANG_JSON: return "json";
	case LANG_JSONC: return "jsonc";
	case LANG_IGNORE: return "ignore";
	case LANG_DIFF: return "diff";
	case LANG_GIT_COMMIT: return "git-commit";
	case LANG_GIT_REBASE: return "git-rebase";
	case LANG_PYTHON: return "python";
	case LANG_YAML: return "yaml";
	case LANG_CODEOWNERS: return "codeowners";
	default: return NULL;
	}
}

const char *syn_line_comment(int lang)
{
	switch (lang) {
	case LANG_C: return "//";
	case LANG_ASM_GAS:
	case LANG_SH:
	case LANG_PERL:
	case LANG_CMAKE:
	case LANG_MAKE:
	case LANG_IGNORE:
	case LANG_GIT_COMMIT:
	case LANG_GIT_REBASE:
	case LANG_PYTHON:
	case LANG_YAML:
	case LANG_CODEOWNERS: return "#";
	case LANG_JSONC: return "//";
	case LANG_ASM_INTEL:
	case LANG_LISP: return ";";
	case LANG_TROFF: return ".\\\"";
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

/* Interpreter named by a "#!" line: LANG_SH, LANG_PERL or LANG_NONE. */
static int shebang_lang(const char *h, size_t n)
{
	if (!h || n < 3 || h[0] != '#' || h[1] != '!')
		return LANG_NONE;
	size_t e = 2;
	while (e < n && h[e] != '\n')
		e++;
	/* split the line into words; take the interpreter's basename, skipping
	 * "env" and its options */
	size_t i = 2;
	bool env = false;
	while (i < e) {
		while (i < e && (h[i] == ' ' || h[i] == '\t'))
			i++;
		size_t a = i;
		while (i < e && h[i] != ' ' && h[i] != '\t' && h[i] != '\r')
			i++;
		if (a == i)
			break;
		size_t b = a;
		for (size_t k = a; k < i; k++)
			if (h[k] == '/')
				b = k + 1;
		const char *w = h + b;
		size_t wl = i - b;
		if (wl == 3 && memcmp(w, "env", 3) == 0) {
			env = true;
			continue;
		}
		if (env && (w[0] == '-' || memchr(w, '=', wl)))
			continue;
		static const char *const shells[] = {"sh", "bash", "zsh", "ksh", "dash", "ash", "mksh", "yash", "pdksh", "ksh93", "rbash"};
		for (size_t k = 0; k < VX_COUNT(shells); k++)
			if (wl == strlen(shells[k]) && memcmp(w, shells[k], wl) == 0)
				return LANG_SH;
		if (wl >= 4 && memcmp(w, "perl", 4) == 0 && (wl == 4 || is_digit((u8)w[4])))
			return LANG_PERL;
		if (wl >= 6 && memcmp(w, "python", 6) == 0) {
			bool ok = true; /* python, python3, python3.12 */
			for (size_t k = 6; k < wl; k++)
				if (!is_digit((u8)w[k]) && w[k] != '.')
					ok = false;
			if (ok)
				return LANG_PYTHON;
		}
		if (wl >= 4 && (memcmp(w, "pypy", 4) == 0))
			return LANG_PYTHON;
		return LANG_NONE;
	}
	return LANG_NONE;
}

/* Does the head look like roff (.TH, .Dd, .\" ...)? */
static bool roff_head(const char *h, size_t n)
{
	if (!h)
		return false;
	size_t i = 0;
	int lines = 0;
	while (i < n && lines < 20) {
		size_t e = i;
		while (e < n && h[e] != '\n')
			e++;
		if (e > i + 1 && (h[i] == '.' || h[i] == '\'')) {
			const char *l = h + i + 1;
			size_t ln = e - i - 1;
			static const char *const req[] = {"\\\"", "TH", "Dd", "Dt", "SH", "Sh", "so", "de", "ig", "Os", "TL", "PP", "LP", "Nm", "ds", "nr", "ad", "nh"};
			for (size_t k = 0; k < VX_COUNT(req); k++) {
				size_t rl = strlen(req[k]);
				if (ln >= rl && memcmp(l, req[k], rl) == 0 && (ln == rl || !is_alpha((u8)l[rl]) || req[k][0] == '\\'))
					return true;
			}
		}
		i = e + 1;
		lines++;
	}
	return false;
}

/* Extensionless head with BSD/GNU make constructs (.include, .if, .PHONY:). */
static bool make_head(const char *h, size_t n)
{
	if (!h)
		return false;
	size_t i = 0;
	int lines = 0;
	while (i < n && lines < 40) {
		size_t e = i;
		while (e < n && h[e] != '\n')
			e++;
		const char *l = h + i;
		size_t ln = e - i;
		static const char *const pre[] = {".include", ".if ", ".ifdef", ".ifndef", ".for ", ".PHONY:", "include ", ".SUFFIXES:"};
		for (size_t k = 0; k < VX_COUNT(pre); k++) {
			size_t pl = strlen(pre[k]);
			if (ln >= pl && memcmp(l, pre[k], pl) == 0)
				return k < 6 || k == 7;
		}
		i = e + 1;
		lines++;
	}
	return false;
}

int syn_lang_for_path(const char *path, const char *head, size_t headlen)
{
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	static const struct {
		const char *name;
		u8 lang;
	} names[] = {
	    {"CMakeLists.txt", LANG_CMAKE},   {"Makefile", LANG_MAKE},          {"makefile", LANG_MAKE},
	    {"GNUmakefile", LANG_MAKE},       {"BSDmakefile", LANG_MAKE},       {".depend", LANG_MAKE},
	    {".gitignore", LANG_IGNORE},      {".dockerignore", LANG_IGNORE},   {".ignore", LANG_IGNORE},
	    {".rgignore", LANG_IGNORE},       {".npmignore", LANG_IGNORE},      {".hgignore", LANG_IGNORE},
	    {".eslintignore", LANG_IGNORE},   {".prettierignore", LANG_IGNORE}, {".vscodeignore", LANG_IGNORE},
	    {".cvsignore", LANG_IGNORE},      {".gitattributes", LANG_IGNORE},  {".stylelintignore", LANG_IGNORE},
	    {".clang-format-ignore", LANG_IGNORE},
	    {"COMMIT_EDITMSG", LANG_GIT_COMMIT}, {"MERGE_MSG", LANG_GIT_COMMIT}, {"TAG_EDITMSG", LANG_GIT_COMMIT},
	    {"SQUASH_MSG", LANG_GIT_COMMIT},  {"git-rebase-todo", LANG_GIT_REBASE},
	    {"settings.json", LANG_JSONC},    {"keybindings.json", LANG_JSONC}, {"launch.json", LANG_JSONC},
	    {"tasks.json", LANG_JSONC},       {"extensions.json", LANG_JSONC},  {"devcontainer.json", LANG_JSONC},
	    {".devcontainer.json", LANG_JSONC}, {".babelrc", LANG_JSONC},       {".eslintrc", LANG_JSONC},
	    {".jshintrc", LANG_JSONC},        {".swcrc", LANG_JSONC},           {".jscsrc", LANG_JSONC},
	    {"SConstruct", LANG_PYTHON},      {"SConscript", LANG_PYTHON},      {"CODEOWNERS", LANG_CODEOWNERS},
	    {".clang-format", LANG_YAML},     {"_clang-format", LANG_YAML},     {".clang-tidy", LANG_YAML},
	    {".clangd", LANG_YAML},
	};
	for (size_t i = 0; i < VX_COUNT(names); i++)
		if (!strcmp(base, names[i].name))
			return names[i].lang;
	size_t bl = strlen(base);
	if (bl > 8 && (!strncmp(base, "Makefile.", 9) || !strncmp(base, "makefile.", 9)))
		return LANG_MAKE;
	if (bl > 9 && !strcmp(base + bl - 9, ".cmake.in"))
		return LANG_CMAKE;
	if (bl > 15 && !strcmp(base + bl - 15, ".code-workspace"))
		return LANG_JSONC;
	if (strstr(path, "/info/exclude") && !strcmp(base, "exclude"))
		return LANG_IGNORE;
	if ((!strncmp(base, "tsconfig", 8) || !strncmp(base, "jsconfig", 8)) && bl > 5 && !strcmp(base + bl - 5, ".json"))
		return LANG_JSONC;
	static const char *const sh_names[] = {".bashrc", ".bash_profile", ".bash_login", ".bash_logout", ".bash_aliases",
	                                       ".profile", ".zshrc", ".zprofile", ".zshenv", ".zlogin", ".zlogout",
	                                       ".kshrc", ".mkshrc", "PKGBUILD", "APKBUILD", ".xinitrc", ".xsession"};
	for (size_t i = 0; i < VX_COUNT(sh_names); i++)
		if (!strcmp(base, sh_names[i]))
			return LANG_SH;
	int bang = shebang_lang(head, headlen);
	const char *dot = strrchr(base, '.');
	if (!dot || dot == base) {
		if (!bang && make_head(head, headlen))
			return LANG_MAKE;
		if (!bang && head && headlen >= 10 && memcmp(head, "diff --git", 10) == 0)
			return LANG_DIFF;
		return bang;
	}
	char e[12];
	size_t el = strlen(dot + 1);
	if (el == 0 || el >= sizeof e)
		return bang;
	for (size_t i = 0; i <= el; i++)
		e[i] = (char)lower((u8)dot[1 + i]);
	if (!strcmp(e, "c") || !strcmp(e, "h"))
		return LANG_C;
	if (!strcmp(e, "py") || !strcmp(e, "pyi") || !strcmp(e, "pyw") || !strcmp(e, "gyp") || !strcmp(e, "gypi"))
		return LANG_PYTHON;
	if (!strcmp(e, "yml") || !strcmp(e, "yaml"))
		return LANG_YAML;
	if (!strcmp(e, "cmake"))
		return LANG_CMAKE;
	if (!strcmp(e, "mk") || !strcmp(e, "mak") || !strcmp(e, "make"))
		return LANG_MAKE;
	if (!strcmp(e, "jsonc") || !strcmp(e, "json5") || !strcmp(e, "code-workspace"))
		return LANG_JSONC;
	static const char *const json[] = {"json", "webmanifest", "har", "geojson", "topojson", "jsonl", "ndjson", "tsbuildinfo"};
	for (size_t i = 0; i < VX_COUNT(json); i++)
		if (!strcmp(e, json[i]))
			return LANG_JSON;
	if (!strcmp(e, "diff") || !strcmp(e, "patch") || !strcmp(e, "rej"))
		return LANG_DIFF;
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
	static const char *const sh[] = {"sh", "bash", "zsh", "ksh", "ash", "dash", "mksh", "bats", "command", "ebuild", "eclass"};
	for (size_t i = 0; i < VX_COUNT(sh); i++)
		if (!strcmp(e, sh[i]))
			return LANG_SH;
	static const char *const md[] = {"md", "markdown", "mdown", "mkd", "mkdn", "mdwn", "mdtxt", "mdtext", "workbook"};
	for (size_t i = 0; i < VX_COUNT(md); i++)
		if (!strcmp(e, md[i]))
			return LANG_MARKDOWN;
	static const char *const pl[] = {"pl", "pm", "pod", "psgi", "perl", "plx", "ph"};
	for (size_t i = 0; i < VX_COUNT(pl); i++)
		if (!strcmp(e, pl[i]))
			return LANG_PERL;
	if (!strcmp(e, "t")) /* Perl test unless it is a roff document */
		return roff_head(head, headlen) ? LANG_TROFF : LANG_PERL;
	if (!strcmp(e, "cgi") || !strcmp(e, "fcgi"))
		return bang ? bang : LANG_PERL;
	static const char *const roff[] = {"man", "mdoc", "ms", "me", "mm", "mom", "roff", "tr", "tmac", "groff", "nroff", "troff", "1in", "3in"};
	for (size_t i = 0; i < VX_COUNT(roff); i++)
		if (!strcmp(e, roff[i]))
			return LANG_TROFF;
	/* man sections: .1 .3p .1m .8 .3ssl ... */
	if (e[0] >= '1' && e[0] <= '9') {
		bool ok = true;
		for (size_t i = 1; i < el; i++)
			if (!is_alpha((u8)e[i]))
				ok = false;
		if (ok && (el <= 4 || roff_head(head, headlen)))
			return LANG_TROFF;
	}
	return bang;
}
