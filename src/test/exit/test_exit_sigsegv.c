// Dieing with SIGSEGV in particular is a special case since we also use
// intercept rdtsc as SIGSEGV, via `prctl(PR_SET_TSC)`.
int main() {
    // Access memory address 0, triggering a SEGV. Shadow should detect that the
    // process has exited and clean it up.
    //
    // We use assembly here to ensure we really access the NULL pointer and get a
    // SEGV. Done at the C level, the compiler could detect undefined behavior
    // and do something else.
#if defined(__x86_64__)
    asm("mov 0, %rax");
#elif defined(__aarch64__)
    asm("mov x0, 0\nldr x1, [x0]");
#else
#error "Unsupported architecture"
#endif
}
