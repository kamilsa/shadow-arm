use shadow_shim_helper_rs::syscall_types::ForeignPtr;

use crate::cshadow;
use crate::host::descriptor::CompatFile;
use crate::host::syscall::handler::{SyscallContext, SyscallHandler};
use crate::host::syscall::type_formatting::SyscallStringArg;
use crate::host::syscall::types::{SyscallError, SyscallResult};

impl SyscallHandler {
    log_syscall!(
        statx,
        /* rv */ std::ffi::c_int,
        /* dfd */ std::ffi::c_int,
        /* filename */ SyscallStringArg,
        /* flags */ std::ffi::c_int,
        /* statxbuf */ *const std::ffi::c_void,
    );
    pub fn statx(ctx: &mut SyscallContext) -> SyscallResult {
        Self::legacy_syscall(cshadow::syscallhandler_statx, ctx)
    }

    log_syscall!(
        fstat,
        /* rv */ std::ffi::c_int,
        /* fd */ std::ffi::c_uint,
        /* statbuf */ *const linux_api::stat::stat,
    );
    pub fn fstat(
        ctx: &mut SyscallContext,
        fd: std::ffi::c_uint,
        statbuf_ptr: ForeignPtr<linux_api::stat::stat>,
    ) -> Result<(), SyscallError> {
        let desc_table = ctx.objs.thread.descriptor_table_borrow(ctx.objs.host);
        let file = match Self::get_descriptor(&desc_table, fd)?.file() {
            CompatFile::New(file) => file.clone(),
            // if it's a legacy file, use the C syscall handler instead
            CompatFile::Legacy(_) => {
                drop(desc_table);
                let rv: i32 = Self::legacy_syscall(cshadow::syscallhandler_fstat, ctx)?;
                assert_eq!(rv, 0);
                return Ok(());
            }
        };

        let stat = file.inner_file().borrow().stat()?;

        ctx.objs
            .process
            .memory_borrow_mut()
            .write(statbuf_ptr, &stat)?;

        Ok(())
    }

    log_syscall!(fstatfs, /* rv */ std::ffi::c_int);
    pub fn fstatfs(ctx: &mut SyscallContext) -> SyscallResult {
        Self::legacy_syscall(cshadow::syscallhandler_fstatfs, ctx)
    }

    log_syscall!(
        newfstatat,
        /* rv */ std::ffi::c_int,
        /* dirfd */ std::ffi::c_int,
        /* path */ SyscallStringArg,
        /* statbuf */ *const linux_api::stat::stat,
        /* flag */ std::ffi::c_int,
    );
    pub fn newfstatat(ctx: &mut SyscallContext) -> SyscallResult {
        // Read raw syscall args to check for AT_EMPTY_PATH.
        // On ARM64 glibc implements fstat via newfstatat(fd, "", buf, AT_EMPTY_PATH).
        // Handle that case here using the Rust fstat path.
        let dirfd: i32 = ctx.args.get(0).into();
        let statbuf_ptr: ForeignPtr<linux_api::stat::stat> = ctx.args.get(2).into();
        let flags: i32 = ctx.args.get(3).into();

        const AT_EMPTY_PATH: i32 = 4096;
        if flags & AT_EMPTY_PATH != 0 {
            let desc_table = ctx.objs.thread.descriptor_table_borrow(ctx.objs.host);
            let file = match Self::get_descriptor(&desc_table, dirfd as u32) {
                Ok(f) => match f.file() {
                    CompatFile::New(file) => file.clone(),
                    CompatFile::Legacy(_) => {
                        drop(desc_table);
                        return Self::legacy_syscall(cshadow::syscallhandler_newfstatat, ctx);
                    }
                },
                Err(e) => return Err(e.into()),
            };
            let stat = match file.inner_file().borrow().stat() {
                Ok(s) => s,
                Err(e) => return Err(e.into()),
            };
            drop(desc_table);
            match ctx.objs.process.memory_borrow_mut().write(statbuf_ptr, &stat) {
                Ok(()) => Ok(0i64.into()),
                Err(e) => return Err(e.into()),
            }
        } else {
            Self::legacy_syscall(cshadow::syscallhandler_newfstatat, ctx)
        }
    }
}
