package io.dage;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;

/**
 * A single native Run. Only one execute operation may be active.
 *
 * <p>Cancellation is cooperative: cancelling the returned future first forwards a reason to the
 * native Run. {@link #close()} waits until the native call has actually left before destroying the
 * handle, so a cancelled future cannot cause a use-after-free.</p>
 */
public final class Run implements AutoCloseable {
    private final Engine engine;
    private final Workflow workflow;
    private long handle;
    private CompletableFuture<String> active;
    private CountDownLatch nativeExit;
    private Thread runner;

    Run(Engine engine, Workflow workflow, long handle) {
        this.engine = engine;
        this.workflow = workflow;
        this.handle = handle;
    }

    public CompletableFuture<String> executeAsync(String inputJson, Executor executor) {
        Objects.requireNonNull(inputJson, "inputJson");
        Objects.requireNonNull(executor, "executor");
        final NativeFuture future;
        final CountDownLatch exit = new CountDownLatch(1);
        synchronized (this) {
            ensureOpen();
            if (active != null) throw new IllegalStateException("Run is already executing");
            future = new NativeFuture(this);
            active = future;
            nativeExit = exit;
        }
        try {
            executor.execute(() -> {
                final long nativeHandle;
                synchronized (Run.this) {
                    nativeHandle = handle;
                    runner = Thread.currentThread();
                }
                try {
                    future.complete(nativeExecute(nativeHandle, inputJson));
                } catch (Throwable error) {
                    future.completeExceptionally(error);
                } finally {
                    synchronized (Run.this) {
                        runner = null;
                        active = null;
                        nativeExit = null;
                    }
                    exit.countDown();
                }
            });
        } catch (RuntimeException rejected) {
            synchronized (this) {
                active = null;
                nativeExit = null;
            }
            exit.countDown();
            throw rejected;
        }
        return future;
    }

    public void cancel(String reason) {
        Objects.requireNonNull(reason, "reason");
        synchronized (this) {
            ensureOpen();
            nativeCancel(handle, reason);
        }
    }

    @Override public void close() {
        final long nativeHandle;
        final CountDownLatch exit;
        synchronized (this) {
            if (handle == 0) return;
            if (runner == Thread.currentThread()) {
                throw new IllegalStateException("Run cannot be closed by its executing thread");
            }
            nativeHandle = handle;
            exit = nativeExit;
            if (active != null) nativeCancel(nativeHandle, "Java Run.close");
        }
        boolean interrupted = false;
        if (exit != null) {
            while (true) {
                try {
                    exit.await();
                    break;
                } catch (InterruptedException ignored) {
                    interrupted = true;
                    nativeCancel(nativeHandle, "Java close interrupted");
                }
            }
        }
        synchronized (this) {
            if (handle != 0) {
                nativeDestroy(handle);
                handle = 0;
                engine.releaseRun();
            }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }

    private void ensureOpen() {
        if (handle == 0) throw new IllegalStateException("Run is closed");
    }

    private static final class NativeFuture extends CompletableFuture<String> {
        private final Run owner;
        NativeFuture(Run owner) { this.owner = owner; }
        @Override public boolean cancel(boolean mayInterruptIfRunning) {
            if (!super.cancel(mayInterruptIfRunning)) return false;
            try {
                owner.cancel("Java CompletableFuture cancelled");
            } catch (IllegalStateException alreadyClosed) {
                // The native operation has already left.
            } catch (DageException cancellationFailure) {
                throw cancellationFailure;
            }
            return true;
        }
    }

    private static native String nativeExecute(long handle, String inputJson);
    private static native void nativeCancel(long handle, String reason);
    private static native void nativeDestroy(long handle);
}
