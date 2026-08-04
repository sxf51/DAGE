package io.dage;

import java.util.Objects;
import java.util.concurrent.Executor;

public final class Engine implements AutoCloseable {
    static { System.loadLibrary("dage_jni"); }
    private long handle;
    private int liveRuns;

    public Engine() {
        handle = nativeCreate();
        if (handle == 0) throw new DageException("Unable to create engine");
    }

    public synchronized Workflow load(String json) {
        ensureOpen();
        Objects.requireNonNull(json, "json");
        long workflow = nativeLoad(handle, json);
        if (workflow == 0) throw new DageException(nativeLastError(handle));
        return new Workflow(this, workflow);
    }

    public synchronized Run createRun(Workflow workflow) {
        ensureOpen();
        Objects.requireNonNull(workflow, "workflow");
        if (workflow.owner() != this) {
            throw new IllegalArgumentException("Workflow belongs to another Engine");
        }
        long run = nativeCreateRun(handle, workflow.handle());
        if (run == 0) throw new DageException(nativeLastError(handle));
        liveRuns++;
        return new Run(this, workflow, run);
    }

    public synchronized void registerAsyncExecutor(
            String name, AsyncExecutor callback, Executor executor) {
        ensureOpen();
        Objects.requireNonNull(name, "name");
        if (name.isEmpty()) throw new IllegalArgumentException("Executor name is empty");
        nativeRegisterAsyncExecutor(
            handle, name, new AsyncExecutorBridge(callback, executor));
    }

    synchronized void releaseRun() {
        if (liveRuns <= 0) throw new IllegalStateException("Run ownership underflow");
        liveRuns--;
    }

    private void ensureOpen() {
        if (handle == 0) throw new IllegalStateException("Engine is closed");
    }

    @Override public synchronized void close() {
        if (liveRuns != 0) throw new IllegalStateException("Engine has live Runs");
        if (handle != 0) { nativeDestroy(handle); handle = 0; }
    }

    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native long nativeLoad(long handle, String json);
    private static native long nativeCreateRun(long engine, long workflow);
    private static native void nativeRegisterAsyncExecutor(
        long engine, String name, AsyncExecutorBridge bridge);
    private static native String nativeLastError(long handle);
}
