package io.dage;

import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;

final class AsyncExecutorBridge {
    private final AsyncExecutor callback;
    private final Executor executor;

    AsyncExecutorBridge(AsyncExecutor callback, Executor executor) {
        this.callback = Objects.requireNonNull(callback, "callback");
        this.executor = Objects.requireNonNull(executor, "executor");
    }

    // Called briefly from a DAGE worker through JNI. Work is always handed to the host Executor.
    void start(AsyncExecutionContext context, String inputJson) {
        try {
            executor.execute(() -> invoke(context, inputJson));
        } catch (Throwable rejected) {
            context.fail(rejected);
        }
    }

    private void invoke(AsyncExecutionContext context, String inputJson) {
        final CompletionStage<String> stage;
        try {
            stage = Objects.requireNonNull(
                callback.execute(context, inputJson), "Async Executor returned null stage");
        } catch (Throwable error) {
            context.fail(error);
            return;
        }
        try {
            stage.whenComplete((output, error) -> {
                if (error instanceof CancellationException) context.abandon();
                else if (error != null) context.fail(error);
                else context.complete(output);
            });
        } catch (Throwable registrationFailure) {
            context.fail(registrationFailure);
        }
    }
}
