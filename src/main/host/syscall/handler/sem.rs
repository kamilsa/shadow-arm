impl super::SyscallHandler {
    log_syscall!(semget, /* rv */ u64);
    pub fn semget(_ctx: &mut super::SyscallContext) -> Result<u64, super::SyscallError> {
        // Return a fake semaphore set ID. Programs that probe this
        // (e.g. Python 3.12 on ARM64) just need a non-error result.
        log::warn!("semget stubbed: returning fake semaphore set ID");
        Ok(42)
    }

    log_syscall!(semop, /* rv */ u64);
    pub fn semop(_ctx: &mut super::SyscallContext) -> Result<u64, super::SyscallError> {
        log::trace!("semop stubbed");
        Ok(0)
    }

    log_syscall!(semctl, /* rv */ u64);
    pub fn semctl(_ctx: &mut super::SyscallContext) -> Result<u64, super::SyscallError> {
        log::trace!("semctl stubbed");
        Ok(0)
    }

    log_syscall!(semtimedop, /* rv */ u64);
    pub fn semtimedop(_ctx: &mut super::SyscallContext) -> Result<u64, super::SyscallError> {
        log::trace!("semtimedop stubbed");
        Ok(0)
    }

    log_syscall!(personality, /* rv */ u64);
    pub fn personality(_ctx: &mut super::SyscallContext) -> Result<u64, super::SyscallError> {
        // Return the current personality (PER_LINUX = 0).
        // Called by glibc on ARM64 during process startup.
        log::trace!("personality stubbed");
        Ok(0)
    }
}
