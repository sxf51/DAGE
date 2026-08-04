package io.dage;

public final class Workflow implements AutoCloseable {
    private final Engine engine;
    private long handle;
    Workflow(Engine engine, long handle) { this.engine = engine; this.handle = handle; }

    Engine owner() { return engine; }
    long handle() {
        if (handle == 0) throw new IllegalStateException("Workflow is closed");
        return handle;
    }

    public String mermaid() {
        return nativeMermaid(handle());
    }

    @Override public void close() {
        if (handle != 0) { nativeDestroy(handle); handle = 0; }
    }

    private static native String nativeMermaid(long handle);
    private static native void nativeDestroy(long handle);
}
