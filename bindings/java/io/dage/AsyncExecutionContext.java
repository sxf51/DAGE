package io.dage;

public final class AsyncExecutionContext {
    private long completion;
    private final String runId;
    private final String nodeId;
    private final String nodeType;
    private final int attempt;
    private final String idempotencyKey;
    private final String runMode;
    private final long deadlineRemainingMs;

    AsyncExecutionContext(long completion, String runId, String nodeId, String nodeType, int attempt,
            String idempotencyKey, String runMode, long deadlineRemainingMs) {
        this.completion = completion;
        this.runId = runId;
        this.nodeId = nodeId;
        this.nodeType = nodeType;
        this.attempt = attempt;
        this.idempotencyKey = idempotencyKey;
        this.runMode = runMode;
        this.deadlineRemainingMs = deadlineRemainingMs;
    }

    public String runId() { return runId; }
    public String nodeId() { return nodeId; }
    public String nodeType() { return nodeType; }
    public int attempt() { return attempt; }
    public String idempotencyKey() { return idempotencyKey; }
    public String runMode() { return runMode; }
    public long deadlineRemainingMs() { return deadlineRemainingMs; }

    public synchronized boolean isCancelled() {
        return completion == 0 || nativeIsCancelled(completion);
    }

    public synchronized void commitEffect() {
        if (completion == 0) throw new IllegalStateException("Executor completion is closed");
        nativeCommitEffect(completion);
    }

    synchronized void complete(String outputJson) {
        if (outputJson == null) {
            fail(new NullPointerException("Async Executor returned null JSON"));
            return;
        }
        long handle = completion;
        completion = 0;
        if (handle != 0) nativeComplete(handle, outputJson);
    }

    synchronized void fail(Throwable error) {
        long handle = completion;
        completion = 0;
        if (handle != 0) nativeFail(handle, error == null ? "Async Executor failed" : error.toString());
    }

    synchronized void abandon() {
        long handle = completion;
        completion = 0;
        if (handle != 0) nativeAbandon(handle);
    }

    private static native boolean nativeIsCancelled(long completion);
    private static native void nativeCommitEffect(long completion);
    private static native void nativeComplete(long completion, String outputJson);
    private static native void nativeFail(long completion, String message);
    private static native void nativeAbandon(long completion);
}
