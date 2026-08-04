fn main() {
    if let Ok(path) = std::env::var("DAGE_LIB_DIR") {
        println!("cargo:rustc-link-search=native={path}");
    }
    println!("cargo:rerun-if-env-changed=DAGE_LIB_DIR");
}
