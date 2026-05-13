use std::{
    env,
    path::{Path, PathBuf},
};

use shadow_build_common::{Compiler, ShadowBuildCommon};

fn run_cbindgen(build_common: &ShadowBuildCommon) {
    let mut config = build_common.cbindgen_base_config();
    config.include_guard = Some("tsc_rs_h".into());
    let crate_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    cbindgen::Builder::new()
        .with_crate(crate_dir)
        .with_config(config)
        .generate()
        .expect("Unable to generate bindings")
        .write_to_file("../../../build/src/lib/tsc/tsc.h");
}

fn run_bindgen(build_common: &ShadowBuildCommon) {
    let bindings = build_common
        .bindgen_builder()
        .header("tsc_internal.h")
        .allowlist_function("TscC_.*")
        // the tsc C functions may call rust functions that do unwind, so I think we need this
        .override_abi(bindgen::Abi::CUnwind, ".*")
        .generate()
        .expect("Unable to generate bindings");
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("c_internal.rs"))
        .expect("Couldn't write bindings!");
}

fn main() {
    let build_common = ShadowBuildCommon::new(Path::new("../../.."), None);

    let target_arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap();

    if target_arch == "x86_64" {
        // The C bindings should be generated first since cbindgen doesn't require
        // the Rust code to be valid, whereas bindgen does require the C code to be
        // valid. If the C bindings are no longer correct, but the Rust bindings are
        // generated first, then there will be no way to correct the C bindings
        // since the Rust binding generation will always fail before the C bindings
        // can be corrected.
        run_cbindgen(&build_common);
        run_bindgen(&build_common);

        build_common
            .cc_build(Compiler::C)
            .file("tsc.c")
            .compile("tsc_c");
    } else {
        // ARM64: generate a stub tsc.h header and compile a stub C implementation.
        // cbindgen would process the stub Tsc type but may fail; we write the header
        // manually. bindgen still runs against the stub header to produce c_internal.rs.
        std::fs::create_dir_all("../../../build/src/lib/tsc").ok();
        std::fs::write("../../../build/src/lib/tsc/tsc.h",
            "#ifndef TSC_H\n#define TSC_H\n#include <inttypes.h>\n\n\
// Stub Tsc struct for ARM64 (rdtsc emulation not used on ARM64)\n\
typedef struct Tsc {\n\
  uint64_t cyclesPerSecond;\n\
} Tsc;\n\
\n\
uint64_t Tsc_nativeCyclesPerSecond(void);\n\
Tsc Tsc_create(uint64_t cycles_per_second);\n\
void Tsc_emulateRdtsc(const Tsc *tsc, uint64_t *rax, uint64_t *rdx, uint64_t *rip, uint64_t nanos);\n\
void Tsc_emulateRdtscp(const Tsc *tsc, uint64_t *rax, uint64_t *rdx, uint64_t *rcx, uint64_t *rip, uint64_t nanos);\n\
\n#endif\n"
        ).expect("Couldn't write stub tsc.h");

        // Still need to run bindgen so that c_internal.rs exists.
        run_bindgen(&build_common);

        build_common
            .cc_build(Compiler::C)
            .file("tsc_stub.c")
            .compile("tsc_c");
    }
}
