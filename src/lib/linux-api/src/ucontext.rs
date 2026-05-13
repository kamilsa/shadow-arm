#[cfg(target_arch = "x86_64")]
pub use crate::bindings::linux_ucontext;
#[cfg(target_arch = "aarch64")]
pub use aarch64::linux_ucontext;
#[allow(non_camel_case_types)]
pub type ucontext = linux_ucontext;

#[cfg(target_arch = "x86_64")]
pub use crate::bindings::linux_sigcontext;
#[cfg(target_arch = "aarch64")]
pub use aarch64::linux_sigcontext;
#[allow(non_camel_case_types)]
pub type sigcontext = linux_sigcontext;

#[cfg(target_arch = "aarch64")]
mod aarch64 {
    use crate::bindings::{linux_sigset_t, linux_stack_t};

    /// The 4 KiB reserved extension area in the ARM64 signal context.
    ///
    /// The kernel and glibc require this field to be 16-byte aligned, which adds
    /// padding after `pstate` and gives `linux_sigcontext` the same layout as
    /// `mcontext_t` from glibc's `<sys/ucontext.h>`.
    #[repr(C, align(16))]
    #[derive(Copy, Clone)]
    pub struct linux_aarch64_ctx_reserved(pub [u8; 4096]);

    #[repr(C)]
    #[derive(Copy, Clone)]
    pub struct linux_sigcontext {
        pub fault_address: u64,
        pub regs: [u64; 31],
        pub sp: u64,
        pub pc: u64,
        pub pstate: u64,
        pub __reserved: linux_aarch64_ctx_reserved,
    }

    #[repr(C)]
    #[derive(Copy, Clone)]
    pub struct linux_ucontext {
        pub uc_flags: core::ffi::c_ulong,
        pub uc_link: *mut linux_ucontext,
        pub uc_stack: linux_stack_t,
        pub uc_sigmask: linux_sigset_t,
        pub __unused: [u8; 120],
        pub uc_mcontext: linux_sigcontext,
    }

    const _: () = {
        assert!(core::mem::size_of::<linux_sigcontext>() == 4384);
        assert!(core::mem::align_of::<linux_sigcontext>() == 16);
        assert!(core::mem::offset_of!(linux_sigcontext, fault_address) == 0);
        assert!(core::mem::offset_of!(linux_sigcontext, regs) == 8);
        assert!(core::mem::offset_of!(linux_sigcontext, sp) == 256);
        assert!(core::mem::offset_of!(linux_sigcontext, pc) == 264);
        assert!(core::mem::offset_of!(linux_sigcontext, pstate) == 272);
        assert!(core::mem::offset_of!(linux_sigcontext, __reserved) == 288);

        assert!(core::mem::size_of::<linux_ucontext>() == 4560);
        assert!(core::mem::align_of::<linux_ucontext>() == 16);
        assert!(core::mem::offset_of!(linux_ucontext, uc_flags) == 0);
        assert!(core::mem::offset_of!(linux_ucontext, uc_link) == 8);
        assert!(core::mem::offset_of!(linux_ucontext, uc_stack) == 16);
        assert!(core::mem::offset_of!(linux_ucontext, uc_sigmask) == 40);
        assert!(core::mem::offset_of!(linux_ucontext, __unused) == 48);
        assert!(core::mem::offset_of!(linux_ucontext, uc_mcontext) == 176);
    };
}
