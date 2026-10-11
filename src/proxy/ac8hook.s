/* =============================================================================
 *  AC8 简中汉化 · 自研 dalek 钩子（x86-64 Windows / COFF）
 *
 *  目标：FPakFile 构造函数入口（RVA 0x27F6C80）
 *      void* __fastcall FPakFileCtor(void* this, IPlatformFile* lower,
 *                                    const wchar_t* path, bool bSigned, bool bLoadIndex)
 *      rcx=this  rdx=lower  r8=path  r9b=bSigned  [rsp+0x28]=bLoadIndex
 *
 *  为什么要汇写入口：入口前 15 字节被换成一条绝对 jmp 跳到 ac8_thunk。
 *  在 C 里做这件事必然改变 rsp（call 会压返回地址），而构造函数的后续
 *  指令全是 rsp 相对寻址，rsp 一歪整个函数就崩。所以 thunk 必须在
 *  「调用完 C 判定函数」之后**把 rsp 精确还原成 R**，再进 trampoline。
 *  这种事 C 编译器不保证，只能手写。
 *
 *  15 字节被覆盖的原指令（与 GAS 编码逐字节一致，见 ac8_tramp）：
 *      48 89 5C 24 10   mov [rsp+0x10], rbx
 *      48 89 6C 24 18   mov [rsp+0x18], rbp
 *      48 89 74 24 20   mov [rsp+0x20], rsi
 * ============================================================================= */

    .text

/* ---------------------------------------------------------------------------
 * ac8_thunk —— 从构造函数入口被 jmp 进来。rsp = R（R % 16 == 8）
 *
 * 栈帧 0x48：低 0x20 是给 C 判定函数用的影子空间，其余是保存槽。
 *   [rsp+0x20] this     [rsp+0x28] lower    [rsp+0x30] path
 *   [rsp+0x38] bSigned  [rsp+0x40] 入口 rsp R
 * ------------------------------------------------------------------------- */
    .globl  ac8_thunk
ac8_thunk:
    subq    $0x48, %rsp
    movq    %rcx, 0x20(%rsp)
    movq    %rdx, 0x28(%rsp)
    movq    %r8,  0x30(%rsp)
    movq    %r9,  0x38(%rsp)
    leaq    0x48(%rsp), %rax            /* RAX = R */
    movq    %rax, 0x40(%rsp)

    movq    %r8, %rcx                   /* arg1 = path */
    movq    %r9, %rdx                   /* arg2 = bSigned */
    call    ac8_decide                  /* unsigned char ac8_decide(const wchar_t*, unsigned char) */
    movzbl  %al, %eax
    movq    %rax, 0x38(%rsp)            /* 用返回值覆盖 bSigned 槽 */

    movq    0x20(%rsp), %rcx
    movq    0x28(%rsp), %rdx
    movq    0x30(%rsp), %r8
    movq    0x38(%rsp), %r9
    movq    0x40(%rsp), %rsp            /* ★ rsp 精确还原为 R */
    leaq    ac8_tramp(%rip), %rax
    jmp     *%rax

/* ---------------------------------------------------------------------------
 * ac8_tramp —— 逐字节重放被覆盖的 15 字节prologue，再跳回构造函数 +15。
 * 进入时 rsp 必须 == R。
 * ------------------------------------------------------------------------- */
    .globl  ac8_tramp
ac8_tramp:
    movq    %rbx, 0x10(%rsp)
    movq    %rbp, 0x18(%rsp)
    movq    %rsi, 0x20(%rsp)
    movq    ac8_resume(%rip), %rax      /* = ctor + 15，运行时由 C 填入 */
    jmp     *%rax

    .data
    .globl  ac8_resume
    .p2align 3
ac8_resume:
    .quad   0
